#!/usr/bin/env python3
"""
═══════════════════════════════════════════════════════════════════════════════
  XIAO ESP32-S3 Vision & Audio Hub — Unified Dual-Port Web & Remote Hub
  Merges web_dashboard.py and pc_webcam.py with DUAL COM PORT ARCHITECTURE:
  - Port 1 (XIAO CAM): Video Stream, Mic Audio, Cam Controls, 180° Display Flip
  - Port 2 (MAIN DEVICE): Telemetry, Walkie-Talkie / RF, Main Device Remote
  - Zero collision: completely independent threads, buffers, and serial locks!
═══════════════════════════════════════════════════════════════════════════════
"""

import os
import sys
import math
import time
import json
import queue
import struct
import base64
import threading
import webbrowser
import numpy as np
import cv2
import serial
import serial.tools.list_ports
import sounddevice as sd
from flask import Flask, render_template_string, jsonify, request, Response, send_file

try:
    import pyvirtualcam
    HAS_PYVIRTUALCAM = True
except ImportError:
    HAS_PYVIRTUALCAM = False

app = Flask(__name__)

# ─── Packet Magic Signatures ────────────────────────────────────────────────
MAGIC_VID        = b'\x78\x56\x34\x12'   # 0x12345678 (Video JPEG Frame)
MAGIC_AUD        = b'\x21\x43\x65\x87'   # 0x87654321 (Audio PCM Chunk)
MAGIC_LEGACY_AUD = b'\xAA\x55'           # 0xAA55 (Legacy Main Device Audio)

SAMPLE_RATE    = 16000
CHANNELS       = 1
FFT_SIZE       = 1024
SPECTRUM_BANDS = 48

# ─── Dual Serial Port Connections & Locks ───────────────────────────────────
# Port 1: XIAO CAM (e.g. COM9)
ser_cam = None
ser_cam_lock = threading.Lock()
is_cam_connected = False
cam_port_name = "COM9"

# Port 2: MAIN TRACKER DEVICE (e.g. COM12 - Walkie-Talkie)
ser_main = None
ser_main_lock = threading.Lock()
is_main_connected = False
main_port_name = "COM12"

pending_responses = {}

# ─── Telemetry & Device States ──────────────────────────────────────────────
cam_state = {
    "screen": 1,
    "screenName": "VIEWFINDER",
    "recording": False,
    "fps": 0,
    "heap": 0,
    "psram": 0,
    "rotation": 0,
    "cam_q": 10,
    "cam_res": 9,
}

main_state = {
    "appMode": 0,
    "navState": 1,
    "menuSel": 16,
    "vBatt": 0.0,
    "heap": 0,
    "lrPage": 0,
    "lrState": 0,
    "lrChannel": 0,
    "lrMicGain": 0,
    "lrVolGain": 1.0,
    "lrNoiseGate": 0,
    "lrSquelch": 0,
    "lrPttHeld": False,
}

# Video state
latest_jpeg_frame = None
frame_lock = threading.Lock()
frame_condition = threading.Condition(frame_lock)
video_frame_count = 0
fps_measure_time = time.time()
last_video_frame_time = 0.0
current_video_fps = 0

vcam = None
vcam_enabled = False

# Audio state - Camera Pipeline
cam_audio_gain = 1.0
cam_current_db = -60.0
cam_audio_buffer = np.zeros(FFT_SIZE, dtype=np.float32)
cam_smooth_bands = np.zeros(SPECTRUM_BANDS, dtype=np.float32)
last_cam_audio_time = 0.0

# Audio state - Main Device Pipeline (INMP441 / USB Mic)
main_audio_gain = 1.0
main_current_db = -60.0
main_audio_buffer = np.zeros(FFT_SIZE, dtype=np.float32)
main_smooth_bands = np.zeros(SPECTRUM_BANDS, dtype=np.float32)
last_main_audio_time = 0.0
main_mic_active = False

# Backward compatibility aliases
audio_gain = 1.0
current_db = -60.0
smooth_bands = cam_smooth_bands

current_audio_device = None
audio_stream_obj = None

def create_log_bins(n_bins, n_fft, sample_rate):
    min_freq = 60
    max_freq = 8000
    min_bin = max(1, int(min_freq / (sample_rate / n_fft)))
    max_bin = min(n_fft // 2, int(max_freq / (sample_rate / n_fft)))
    edges = np.logspace(np.log10(min_bin), np.log10(max_bin), n_bins + 1)
    return edges.astype(int)

bin_edges = create_log_bins(SPECTRUM_BANDS, FFT_SIZE, SAMPLE_RATE)
hamming_window = np.hamming(FFT_SIZE)

# ─── Dual Independent Audio Jitter Buffers for Crystal Clear Sound ──────────
class AudioJitterBuffer:
    def __init__(self, target_delay_samples=2400, max_samples=64000):
        self.lock = threading.Lock()
        self.buf = np.zeros(max_samples, dtype=np.float32)
        self.write_idx = 0
        self.read_idx = 0
        self.count = 0
        self.target_delay = target_delay_samples
        self.prebuffering = True

    def write(self, samples):
        with self.lock:
            n = len(samples)
            if n == 0: return
            space = len(self.buf) - self.count
            if n > space:
                skip = n - space
                self.read_idx = (self.read_idx + skip) % len(self.buf)
                self.count -= skip

            for i in range(n):
                self.buf[self.write_idx] = samples[i]
                self.write_idx = (self.write_idx + 1) % len(self.buf)
            self.count += n
            
            if self.prebuffering and self.count >= self.target_delay:
                self.prebuffering = False

    def read(self, n_samples):
        with self.lock:
            if self.prebuffering:
                return np.zeros(n_samples, dtype=np.float32)
            
            out = np.zeros(n_samples, dtype=np.float32)
            available = min(n_samples, self.count)
            for i in range(available):
                out[i] = self.buf[self.read_idx]
                self.read_idx = (self.read_idx + 1) % len(self.buf)
            self.count -= available
            
            if available < n_samples:
                if available > 0:
                    fade = np.linspace(1.0, 0.0, available, dtype=np.float32)
                    out[:available] *= fade
                self.prebuffering = True
                
            return out

cam_jitter_buffer = AudioJitterBuffer(target_delay_samples=2400, max_samples=64000)
main_jitter_buffer = AudioJitterBuffer(target_delay_samples=2400, max_samples=64000)
jitter_buffer = cam_jitter_buffer  # fallback alias

def get_audio_output_devices():
    devices = {}
    try:
        all_devs = sd.query_devices()
        for i, d in enumerate(all_devs):
            if d.get("max_output_channels", 0) > 0:
                name = f"[{i}] {d['name']}"
                devices[name] = i
    except Exception as e:
        print("[Audio Dev Query Error]:", e)
    return devices

def start_audio_output_stream(dev_idx=None):
    global audio_stream_obj, current_audio_device
    stop_audio_output_stream()

    if dev_idx is None:
        devs = get_audio_output_devices()
        for name, idx in devs.items():
            if "CABLE" in name.upper() or "DEFAULT" in name.upper():
                dev_idx = idx
                break
        if dev_idx is None and len(devs) > 0:
            dev_idx = list(devs.values())[0]

    current_audio_device = dev_idx
    target_sr = SAMPLE_RATE
    if dev_idx is not None:
        try:
            info = sd.query_devices(dev_idx, 'output')
            target_sr = int(info.get('default_samplerate', SAMPLE_RATE))
        except:
            pass

    ratio = SAMPLE_RATE / target_sr

    def audio_cb(outdata, frames, time_info, status):
        needed_in = int(round(frames * ratio))
        if needed_in < 1: needed_in = 1
        
        # Read from both Camera and Main Device independent jitter buffers
        cam_in = cam_jitter_buffer.read(needed_in)
        main_in = main_jitter_buffer.read(needed_in)
        
        # Sum both streams and clip to [-1.0, 1.0] for clean dual playback
        mixed = cam_in + main_in
        np.clip(mixed, -1.0, 1.0, out=mixed)

        if abs(ratio - 1.0) > 1e-4 and len(mixed) > 1:
            x_old = np.linspace(0, 1, len(mixed), endpoint=False)
            x_new = np.linspace(0, 1, frames, endpoint=False)
            out = np.interp(x_new, x_old, mixed)
        else:
            out = mixed

        outdata[:, 0] = out.astype(np.float32)

    try:
        audio_stream_obj = sd.OutputStream(
            device=dev_idx,
            samplerate=target_sr,
            channels=CHANNELS,
            dtype="float32",
            latency="low",
            callback=audio_cb
        )
        audio_stream_obj.start()
        print(f"[Audio Output] Dual-Mic Stream active on dev #{dev_idx} @ {target_sr} Hz")
    except Exception as e:
        print("[Audio Output Error]:", e)

def stop_audio_output_stream():
    global audio_stream_obj
    if audio_stream_obj:
        try:
            audio_stream_obj.stop()
            audio_stream_obj.close()
        except:
            pass
        audio_stream_obj = None

# ─── Thread 1: XIAO CAM Serial Worker (Dedicated to Camera & Cam Remote) ────
def serial_cam_worker():
    global ser_cam, is_cam_connected, latest_jpeg_frame, video_frame_count, last_video_frame_time
    global current_video_fps, fps_measure_time, vcam, current_db, audio_buffer, smooth_bands
    global cam_current_db, cam_smooth_bands, cam_audio_buffer, last_cam_audio_time, cam_audio_gain, audio_gain
    
    buf = bytearray()
    start_audio_output_stream()

    while True:
        if not (ser_cam and ser_cam.is_open):
            is_cam_connected = False
            time.sleep(0.1)
            continue

        try:
            chunk = ser_cam.read(ser_cam.in_waiting or 4096)
            if not chunk:
                if time.time() - last_cam_audio_time > 0.3:
                    cam_current_db = max(-60.0, cam_current_db - 2.5)
                    current_db = cam_current_db
                    cam_smooth_bands *= 0.75
                time.sleep(0.005)
                continue
            
            buf.extend(chunk)

            while len(buf) > 0:
                vid_idx = buf.find(MAGIC_VID)
                aud_idx = buf.find(MAGIC_AUD)
                nl_idx  = buf.find(b'\n')

                if vid_idx == -1 and aud_idx == -1 and nl_idx == -1:
                    if len(buf) > 65536:
                        buf.clear()
                    break

                candidates = []
                if vid_idx != -1: candidates.append((vid_idx, 'VID'))
                if aud_idx != -1: candidates.append((aud_idx, 'AUD'))
                if nl_idx != -1:  candidates.append((nl_idx, 'TEXT'))
                candidates.sort(key=lambda x: x[0])
                first_pos, packet_type = candidates[0]

                # ── 1. Video JPEG Frame ──────────────────────────────────────
                if packet_type == 'VID':
                    if first_pos > 0:
                        buf = buf[first_pos:]
                    if len(buf) < 8:
                        break
                    
                    frame_len = struct.unpack('<I', buf[4:8])[0]
                    if frame_len <= 0 or frame_len > 3 * 1024 * 1024:
                        buf = buf[4:]
                        continue
                    if len(buf) < 8 + frame_len:
                        break
                    
                    jpeg_bytes = bytes(buf[8 : 8 + frame_len])
                    buf = buf[8 + frame_len :]

                    with frame_condition:
                        latest_jpeg_frame = jpeg_bytes
                        last_video_frame_time = time.time()
                        frame_condition.notify_all()

                    video_frame_count += 1
                    now = time.time()
                    if now - fps_measure_time >= 1.0:
                        current_video_fps = video_frame_count / (now - fps_measure_time)
                        cam_state["fps"] = int(round(current_video_fps))
                        video_frame_count = 0
                        fps_measure_time = now

                    # Virtual Camera forward
                    if vcam_enabled and HAS_PYVIRTUALCAM:
                        try:
                            img_arr = np.frombuffer(jpeg_bytes, dtype=np.uint8)
                            frame_bgr = cv2.imdecode(img_arr, cv2.IMREAD_COLOR)
                            if frame_bgr is not None:
                                frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
                                h, w, _ = frame_rgb.shape
                                if vcam is None or vcam.width != w or vcam.height != h:
                                    if vcam: vcam.close()
                                    vcam = pyvirtualcam.Camera(width=w, height=h, fps=30, fmt=pyvirtualcam.PixelFormat.RGB)
                                vcam.send(frame_rgb)
                        except Exception as ve:
                            pass

                # ── 2. Audio PCM Packet ──────────────────────────────────────
                elif packet_type == 'AUD':
                    if first_pos > 0:
                        buf = buf[first_pos:]
                    if len(buf) < 8:
                        break
                    
                    audio_len = struct.unpack('<I', buf[4:8])[0]
                    if audio_len <= 0 or audio_len > 65536 or audio_len % 2 != 0:
                        buf = buf[4:]
                        continue
                    if len(buf) < 8 + audio_len:
                        break
                    
                    pcm_bytes = bytes(buf[8 : 8 + audio_len])
                    buf = buf[8 + audio_len :]

                    samples = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32) / 32768.0
                    effective_cam_gain = cam_audio_gain if cam_audio_gain != 1.0 else audio_gain
                    if effective_cam_gain != 1.0:
                        samples *= effective_cam_gain
                        np.clip(samples, -1.0, 1.0, out=samples)

                    cam_jitter_buffer.write(samples)
                    last_cam_audio_time = time.time()

                    # Compute RMS dB and 48-band FFT spectrum for Camera
                    if len(samples) > 0:
                        rms = np.sqrt(np.mean(samples**2))
                        db = 20.0 * np.log10(rms) if rms > 0.0001 else -60.0
                        cam_current_db = 0.25 * db + 0.75 * cam_current_db
                        current_db = cam_current_db

                        n = len(samples)
                        if n >= FFT_SIZE:
                            cam_audio_buffer[:] = samples[-FFT_SIZE:]
                        else:
                            cam_audio_buffer = np.roll(cam_audio_buffer, -n)
                            cam_audio_buffer[-n:] = samples

                        windowed = cam_audio_buffer * hamming_window
                        fft_vals = np.abs(np.fft.rfft(windowed)) / (FFT_SIZE / 2)
                        for i in range(SPECTRUM_BANDS):
                            b_start = bin_edges[i]
                            b_end = max(b_start + 1, bin_edges[i+1])
                            band_e = np.mean(fft_vals[b_start:b_end]) if b_end > b_start else 0
                            cam_smooth_bands[i] = max(cam_smooth_bands[i] * 0.7, float(band_e))

                # ── 3. JSON / Text Line from XIAO CAM ────────────────────────
                elif packet_type == 'TEXT':
                    line_bytes = buf[:first_pos]
                    buf = buf[first_pos + 1:]
                    
                    if b'{' in line_bytes:
                        idx_brace = line_bytes.find(b'{')
                        line_str = line_bytes[idx_brace:].decode('utf-8', errors='ignore').strip()
                        try:
                            data = json.loads(line_str)
                            t = data.get("type")
                            if t == "STATE":
                                for k, v in data.items():
                                    cam_state[k] = v
                            elif t in ("PWR_PROFILE_CHANGE", "PWR_INFO"):
                                if "cpuMhz" in data:
                                    cam_state["cpuMhz"] = data["cpuMhz"]
                                if "profile" in data:
                                    cam_state["pwrProfile"] = data["profile"]
                                if "throttled" in data:
                                    cam_state["throttled"] = data["throttled"]
                                if "coreTemp" in data:
                                    cam_state["coreTemp"] = data["coreTemp"]
                            elif t in ("FS_LS_RES", "FS_READ_RES", "PONG", "ROT_CHANGE"):
                                pending_responses[t] = data

                        except json.JSONDecodeError:
                            pass
                    else:
                        raw_line = line_bytes.decode('utf-8', errors='ignore').strip()
                        if raw_line:
                            try:
                                print(f"[XIAO CAM LOG] {raw_line}".encode('ascii', errors='replace').decode('ascii'), flush=True)
                            except: pass

        except Exception as e:
            import traceback
            traceback.print_exc()
            print("[CAM Serial Error]:", e, flush=True)
            is_cam_connected = False
            with ser_cam_lock:
                try: ser_cam.close()
                except: pass
                ser_cam = None
            time.sleep(0.5)

threading.Thread(target=serial_cam_worker, daemon=True).start()

# ─── Thread 2: MAIN DEVICE Serial Worker (Dedicated to Main Tracker & RF) ───
def serial_main_worker():
    global ser_main, is_main_connected, main_state
    global main_current_db, main_audio_buffer, main_smooth_bands, last_main_audio_time, main_mic_active
    
    buf = bytearray()

    while True:
        if not (ser_main and ser_main.is_open):
            is_main_connected = False
            main_current_db = -60.0
            main_smooth_bands.fill(0.0)
            main_mic_active = False
            time.sleep(0.1)
            continue

        try:
            chunk = ser_main.read(ser_main.in_waiting or 4096)
            if not chunk:
                # Decay audio metrics if silent for > 0.3s
                if time.time() - last_main_audio_time > 0.3:
                    main_current_db = max(-60.0, main_current_db - 2.5)
                    main_smooth_bands *= 0.75
                    main_mic_active = False
                time.sleep(0.005)
                continue
            
            buf.extend(chunk)

            while len(buf) > 0:
                aud_idx = buf.find(MAGIC_LEGACY_AUD)
                nl_idx  = buf.find(b'\n')
                brace_idx = buf.find(b'{')

                # 1. Complete JSON state / response line available
                if brace_idx != -1 and nl_idx != -1 and nl_idx > brace_idx and (aud_idx == -1 or brace_idx < aud_idx):
                    line_bytes = buf[brace_idx:nl_idx]
                    buf = buf[nl_idx + 1:]
                    line_str = line_bytes.decode('utf-8', errors='ignore').strip()
                    try:
                        data = json.loads(line_str)
                        t = data.get("type")
                        if t in ("FS_LS_RES", "FS_READ_RES", "PONG"):
                            pending_responses[t + "_MAIN"] = data
                        else:
                            for k, v in data.items():
                                main_state[k] = v
                    except json.JSONDecodeError:
                        pass
                    continue

                # 2. Incomplete JSON line before any audio marker: wait for more bytes
                if brace_idx != -1 and nl_idx == -1 and (aud_idx == -1 or brace_idx < aud_idx):
                    if len(buf) < 2048:
                        break  # Wait for newline from UART
                    else:
                        buf = buf[brace_idx + 1:]
                        continue

                # 3. Audio PCM packet (MAGIC_LEGACY_AUD = 0xAA55)
                if aud_idx != -1 and (brace_idx == -1 or aud_idx < brace_idx):
                    if aud_idx > 0:
                        buf = buf[aud_idx:]
                    if len(buf) < 4:
                        break
                    
                    payload_len = struct.unpack_from("<H", buf, 2)[0]
                    if payload_len % 2 != 0 or payload_len < 32 or payload_len > 4096:
                        buf = buf[2:]
                        continue
                    if len(buf) < 4 + payload_len:
                        break
                    
                    raw = bytes(buf[4 : 4 + payload_len])
                    buf = buf[4 + payload_len :]

                    samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
                    if main_audio_gain != 1.0:
                        samples *= main_audio_gain
                        np.clip(samples, -1.0, 1.0, out=samples)

                    main_jitter_buffer.write(samples)
                    last_main_audio_time = time.time()
                    main_mic_active = True

                    # Compute RMS dB and 48-band FFT spectrum for Main Device INMP441
                    if len(samples) > 0:
                        rms = np.sqrt(np.mean(samples**2))
                        db = 20.0 * np.log10(rms) if rms > 0.0001 else -60.0
                        main_current_db = 0.25 * db + 0.75 * main_current_db

                        n = len(samples)
                        if n >= FFT_SIZE:
                            main_audio_buffer[:] = samples[-FFT_SIZE:]
                        else:
                            main_audio_buffer = np.roll(main_audio_buffer, -n)
                            main_audio_buffer[-n:] = samples

                        windowed = main_audio_buffer * hamming_window
                        fft_vals = np.abs(np.fft.rfft(windowed)) / (FFT_SIZE / 2)
                        for i in range(SPECTRUM_BANDS):
                            b_start = bin_edges[i]
                            b_end = max(b_start + 1, bin_edges[i+1])
                            band_e = np.mean(fft_vals[b_start:b_end]) if b_end > b_start else 0
                            main_smooth_bands[i] = max(main_smooth_bands[i] * 0.7, float(band_e))
                    continue

                # 4. Plain text log line (without leading brace)
                if nl_idx != -1:
                    line_bytes = buf[:nl_idx]
                    buf = buf[nl_idx + 1:]
                    raw_line = line_bytes.decode('utf-8', errors='ignore').strip()
                    if raw_line:
                        if "[LIVEVIDEO-RX]" in raw_line:
                            try:
                                parts = raw_line.split()
                                if 'fps' in parts:
                                    fps_idx = parts.index('fps')
                                    main_state["espnow_rx_fps"] = float(parts[fps_idx - 1])
                                for p in parts:
                                    if p.startswith('loss='):
                                        main_state["espnow_rx_loss"] = p.split('=')[1]
                                main_state["espnow_rx_time"] = time.time()
                            except: pass
                        try:
                            print(f"[MAIN DEV LOG] {raw_line}".encode('ascii', errors='replace').decode('ascii'), flush=True)
                        except: pass
                    continue

                if len(buf) > 16384:
                    buf.clear()
                break

        except Exception as e:
            print("[MAIN Serial Error]:", e)
            is_main_connected = False
            with ser_main_lock:
                try: ser_main.close()
                except: pass
                ser_main = None
            time.sleep(0.5)

threading.Thread(target=serial_main_worker, daemon=True).start()

# ─── Flask HTTP Endpoints ───────────────────────────────────────────────────

@app.after_request
def add_header(r):
    r.headers["Cache-Control"] = "no-cache, no-store, must-revalidate"
    r.headers["Pragma"] = "no-cache"
    r.headers["Expires"] = "0"
    return r

@app.route('/')
def index():
    template_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'unified_dashboard_template.html')
    try:
        with open(template_path, 'r', encoding='utf-8') as f:
            html = f.read()
    except Exception as e:
        return f"Error loading template: {e}", 500

    ports = [p.device for p in serial.tools.list_ports.comports()]
    audio_devices = get_audio_output_devices()
    return render_template_string(html, ports=ports, audio_devices=audio_devices)

def do_connect_cam(port, baud=115200):
    global ser_cam, is_cam_connected, cam_port_name
    if not port:
        return False, "No COM port selected"
    with ser_cam_lock:
        if ser_cam:
            try: ser_cam.close()
            except: pass
            ser_cam = None

        try:
            ser_cam = serial.Serial()
            ser_cam.port = port
            ser_cam.baudrate = baud
            ser_cam.timeout = 0.1
            ser_cam.dtr = False
            ser_cam.rts = False
            ser_cam.open()
            is_cam_connected = True
            cam_port_name = port
            print(f"[XIAO CAM] Connected to {port} @ {baud} baud")

            def auto_activate():
                time.sleep(0.3)
                with ser_cam_lock:
                    if ser_cam and ser_cam.is_open:
                        try:
                            ser_cam.write(b'{"cmd":"PING"}\n')
                            ser_cam.write(b'{"cmd":"PWR_GET_INFO"}\n')
                            ser_cam.write(b"SET:CAM:1\nSET:MIC:1\n")
                            ser_cam.write(b'{"cmd":"START_STREAM"}\n')
                            ser_cam.flush()
                            print(f"[XIAO CAM] Initialized on {port} (USB Webcam stream active)")
                        except Exception as e:
                            print(f"[XIAO CAM] Activation error: {e}")

            threading.Thread(target=auto_activate, daemon=True).start()
            return True, None
        except Exception as e:
            is_cam_connected = False
            return False, str(e)

def do_connect_main(port, baud=115200):
    global ser_main, is_main_connected, main_port_name
    if not port:
        return False, "No COM port selected"
    with ser_main_lock:
        if ser_main:
            try: ser_main.close()
            except: pass
            ser_main = None

        try:
            ser_main = serial.Serial()
            ser_main.port = port
            ser_main.baudrate = baud
            ser_main.timeout = 0.1
            ser_main.dtr = False
            ser_main.rts = False
            ser_main.open()
            is_main_connected = True
            main_port_name = port
            print(f"[MAIN DEVICE] Connected to {port} @ {baud} baud")
            return True, None
        except Exception as e:
            is_main_connected = False
            return False, str(e)

# Connect / Disconnect for CAM Port (e.g. COM9)
@app.route('/connect/cam', methods=['POST'])
@app.route('/connect', methods=['POST'])
def connect_cam():
    data = request.json or {}
    port = data.get('port', 'COM9')
    baud = int(data.get('baudrate', 115200))
    ok, err = do_connect_cam(port, baud)
    if ok:
        return jsonify({"success": True, "port": port})
    return jsonify({"success": False, "error": err})

@app.route('/disconnect/cam', methods=['POST'])
@app.route('/disconnect', methods=['POST'])
def disconnect_cam():
    global ser_cam, is_cam_connected
    with ser_cam_lock:
        if ser_cam:
            try:
                ser_cam.write(b"SET:CAM:0\nSET:MIC:0\n")
                ser_cam.flush()
            except: pass
            try: ser_cam.close()
            except: pass
            ser_cam = None
        is_cam_connected = False
    return jsonify({"success": True})

# Connect / Disconnect for MAIN DEVICE Port (e.g. COM12 - Walkie-Talkie)
@app.route('/connect/main', methods=['POST'])
def connect_main():
    data = request.json or {}
    port = data.get('port', 'COM12')
    baud = int(data.get('baudrate', 115200))
    ok, err = do_connect_main(port, baud)
    if ok:
        return jsonify({"success": True, "port": port})
    return jsonify({"success": False, "error": err})


@app.route('/disconnect/main', methods=['POST'])
def disconnect_main():
    global ser_main, is_main_connected
    with ser_main_lock:
        if ser_main:
            try: ser_main.close()
            except: pass
            ser_main = None
        is_main_connected = False
    return jsonify({"success": True})

# ─── Live Video MJPEG Stream (Strict RFC / Chromium Compatible) ──────────────
def generate_mjpeg_stream():
    def make_standby_frame(text="WAITING FOR CAMERA STREAM..."):
        img = np.zeros((480, 640, 3), dtype=np.uint8)
        img[:] = (16, 20, 30)
        cv2.putText(img, "XIAO ESP32-S3 VISION", (120, 200), cv2.FONT_HERSHEY_SIMPLEX, 0.85, (0, 212, 255), 2)
        cv2.putText(img, text, (95, 250), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (142, 155, 176), 1)
        cv2.putText(img, "Click 'Stream' to re-activate Webcam mode", (110, 290), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 230, 118), 1)
        _, jpeg = cv2.imencode('.jpg', img, [int(cv2.IMWRITE_JPEG_QUALITY), 80])
        return jpeg.tobytes()

    standby_bytes = make_standby_frame()

    try:
        while True:
            frame_bytes = None
            is_live = False

            with frame_condition:
                # Wait up to 100ms for fresh frame from camera worker
                frame_condition.wait(timeout=0.1)
                now = time.time()
                if latest_jpeg_frame is not None and (now - last_video_frame_time < 2.0):
                    frame_bytes = latest_jpeg_frame
                    is_live = True

            if not is_live or frame_bytes is None:
                frame_bytes = standby_bytes
                time.sleep(0.3)

            # Strict Chromium / Blink multipart frame formatting with Content-Length
            header = (
                b'--frame\r\n'
                b'Content-Type: image/jpeg\r\n'
                b'Content-Length: ' + str(len(frame_bytes)).encode('ascii') + b'\r\n\r\n'
            )
            yield header + frame_bytes + b'\r\n'

            if is_live:
                time.sleep(0.005)
    except GeneratorExit:
        pass
    except Exception:
        pass

@app.route('/video_feed')
def video_feed():
    return Response(generate_mjpeg_stream(), mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route('/api/cam/frame')
def api_cam_frame():
    with frame_lock:
        if latest_jpeg_frame is not None:
            return Response(latest_jpeg_frame, mimetype='image/jpeg',
                            headers={'Cache-Control': 'no-cache, no-store, must-revalidate'})
    return Response(b'', status=204)

@app.route('/api/snapshot')
def api_snapshot():
    with frame_lock:
        if latest_jpeg_frame is not None:
            return Response(latest_jpeg_frame, mimetype='image/jpeg',
                            headers={"Content-Disposition": f"attachment; filename=snapshot_{int(time.time())}.jpg"})
    return "No frame captured yet", 404

@app.route('/cmd/cam/stream/start', methods=['POST'])
def cmd_cam_stream_start():
    with ser_cam_lock:
        if ser_cam and ser_cam.is_open:
            try:
                ser_cam.write(b"SET:CAM:0\n")
                ser_cam.flush()
                time.sleep(0.04)
                ser_cam.write(b"SET:CAM:1\nSET:MIC:1\n")
                ser_cam.write(b'{"cmd":"START_STREAM"}\n')
                ser_cam.flush()
                return jsonify({"success": True, "message": "Camera stream started"})
            except Exception as e:
                return jsonify({"success": False, "error": str(e)})
    return jsonify({"success": False, "error": "Camera not connected"})

@app.route('/cmd/cam/stream/stop', methods=['POST'])
def cmd_cam_stream_stop():
    with ser_cam_lock:
        if ser_cam and ser_cam.is_open:
            try:
                ser_cam.write(b"SET:CAM:0\n")
                ser_cam.write(b'{"cmd":"STOP_STREAM"}\n')
                ser_cam.flush()
                return jsonify({"success": True, "message": "Camera stream stopped"})
            except Exception as e:
                return jsonify({"success": False, "error": str(e)})
    return jsonify({"success": False, "error": "Camera not connected"})

# ─── Real-Time Telemetry Engine ──────────────────────────────────────────────
def get_telemetry_snapshot():
    now = time.time()
    is_streaming = (now - last_video_frame_time <= 2.2) and (latest_jpeg_frame is not None)

    combined = {
        "cam_connected": is_cam_connected and (ser_cam is not None and ser_cam.is_open),
        "main_connected": is_main_connected and (ser_main is not None and ser_main.is_open),
        "cam_port": cam_port_name,
        "main_port": main_port_name,
        "connected": is_cam_connected or is_main_connected,
        "video_fresh": is_streaming,
    }
    # Camera state metrics
    for k, v in cam_state.items():
        combined[k] = v
        combined["cam_" + k] = v

    # True stream FPS vs Camera internal hardware FPS
    combined["stream_fps"] = current_video_fps if is_streaming else 0
    if is_streaming:
        combined["fps"] = current_video_fps
    else:
        combined["fps"] = cam_state.get("fps", 0)

    # Main state metrics: add with main_ prefix and keep root only for non-colliding keys
    for k, v in main_state.items():
        combined["main_" + k] = v
        if k not in ("heap", "coreTemp", "fps", "screen", "screenName", "rotation", "throttled", "cpuMhz", "pwrProfile"):
            combined[k] = v

    # ESP-NOW Stream Status
    espnow_recent = (now - main_state.get("espnow_rx_time", 0) <= 2.5)
    combined["espnow_active"] = espnow_recent
    if espnow_recent and cam_state.get("screen") == 5:
        combined["fps"] = round(main_state.get("espnow_rx_fps", 15.0), 1)

    # Core temperatures
    cam_temp = cam_state.get("coreTemp", None)
    main_temp = main_state.get("coreTemp", None)
    combined["cam_core_temp"] = round(float(cam_temp), 1) if cam_temp is not None else None
    combined["main_core_temp"] = round(float(main_temp), 1) if main_temp is not None else None
    if cam_temp is not None:
        combined["coreTemp"] = round(float(cam_temp), 1)

    # Camera mic metrics
    cam_db_val = float(cam_current_db)
    if math.isinf(cam_db_val) or math.isnan(cam_db_val):
        cam_db_val = -60.0
    
    # Main device mic metrics
    main_db_val = float(main_current_db)
    if math.isinf(main_db_val) or math.isnan(main_db_val):
        main_db_val = -60.0

    # Dedicated keys for Camera
    combined["cam_mic_db"] = round(cam_db_val, 1)
    combined["cam_mic_spectrum"] = cam_smooth_bands.tolist()
    combined["mic_db"] = round(cam_db_val, 1)
    combined["mic_spectrum"] = cam_smooth_bands.tolist()

    # Dedicated keys for Main Device
    combined["main_mic_db"] = round(main_db_val, 1)
    combined["main_mic_spectrum"] = main_smooth_bands.tolist()
    combined["main_mic_active"] = main_mic_active

    return combined

@app.route('/api/telemetry')
def api_telemetry():
    return jsonify(get_telemetry_snapshot())

@app.route('/stream')
def stream_sse():
    def event_generator():
        while True:
            combined = get_telemetry_snapshot()
            try:
                dumped = json.dumps(combined)
            except Exception as e:
                dumped = json.dumps({"error": str(e)})

            yield f"data: {dumped}\n\n"
            time.sleep(0.05)

    return Response(event_generator(), mimetype='text/event-stream')

# ─── Command Routing Engine ──────────────────────────────────────────────────
@app.route('/cmd/cam', methods=['POST'])
def send_cmd_cam():
    cmd_data = request.json or {}
    if not (ser_cam and ser_cam.is_open):
        return jsonify({"success": False, "error": "XIAO CAM not connected"})
    payload = (json.dumps(cmd_data) + '\n').encode('utf-8')
    with ser_cam_lock:
        try:
            ser_cam.write(payload)
            ser_cam.flush()
            return jsonify({"success": True})
        except Exception as e:
            return jsonify({"success": False, "error": str(e)})

@app.route('/cmd/main', methods=['POST'])
def send_cmd_main():
    cmd_data = request.json or {}
    if not (ser_main and ser_main.is_open):
        return jsonify({"success": False, "error": "MAIN DEVICE not connected"})
    payload = (json.dumps(cmd_data) + '\n').encode('utf-8')
    with ser_main_lock:
        try:
            ser_main.write(payload)
            ser_main.flush()
            return jsonify({"success": True})
        except Exception as e:
            return jsonify({"success": False, "error": str(e)})

@app.route('/cmd', methods=['POST'])
def send_cmd():
    cmd_data = request.json or {}
    target = cmd_data.get('target', 'cam') # 'cam', 'main', or 'both'
    payload = (json.dumps(cmd_data) + '\n').encode('utf-8')
    success = False

    if target in ('cam', 'both'):
        if ser_cam and ser_cam.is_open:
            with ser_cam_lock:
                try:
                    ser_cam.write(payload)
                    ser_cam.flush()
                    success = True
                except Exception as e:
                    print("[CAM CMD Write Error]:", e)

    if target in ('main', 'both'):
        if ser_main and ser_main.is_open:
            with ser_main_lock:
                try:
                    ser_main.write(payload)
                    ser_main.flush()
                    success = True
                except Exception as e:
                    print("[MAIN CMD Write Error]:", e)

    return jsonify({"success": success})

@app.route('/cmd/cam/screen', methods=['POST'])
def set_cam_screen():
    data = request.json or {}
    scr = data.get('screen', 0)
    if int(scr) == 9:
        return cmd_cam_stream_start()
    cmd = {"cmd": "SET_SCREEN", "screen": int(scr)}
    with ser_cam_lock:
        if ser_cam and ser_cam.is_open:
            ser_cam.write((json.dumps(cmd) + '\n').encode('utf-8'))
            ser_cam.flush()
            return jsonify({"success": True})
    return jsonify({"success": False, "error": "Camera not connected"})

@app.route('/api/cam/set', methods=['POST'])
def api_cam_set():
    if not (ser_cam and ser_cam.is_open):
        return jsonify({"success": False, "error": "XIAO CAM not connected"})
    
    data = request.json or {}
    param = data.get('param')
    val = data.get('val')
    if param is None or val is None:
        return jsonify({"success": False, "error": "Invalid param or val"})

    try:
        cmd_str = f"SET:{param}:{val}\n"
        with ser_cam_lock:
            ser_cam.write(cmd_str.encode('utf-8'))
            ser_cam.flush()
            if param == 'RES':
                time.sleep(0.05)
                ser_cam.write(b"SET:CAM:1\n")
                ser_cam.flush()
        return jsonify({"success": True})
    except Exception as e:
        return jsonify({"success": False, "error": str(e)})

@app.route('/toggle_vcam', methods=['POST'])
def toggle_vcam():
    global vcam_enabled, vcam
    data = request.json or {}
    enable = bool(data.get('enable', False))
    vcam_enabled = enable
    if not enable and vcam:
        try: vcam.close()
        except: pass
        vcam = None
    return jsonify({"success": True, "vcam_enabled": vcam_enabled})

@app.route('/api/audio/device', methods=['POST'])
def api_audio_device():
    data = request.json or {}
    dev_idx = data.get('device')
    start_audio_output_stream(dev_idx)
    return jsonify({"success": True, "device": dev_idx})

@app.route('/api/audio/gain', methods=['POST'])
def api_audio_gain():
    global audio_gain, cam_audio_gain, main_audio_gain
    data = request.json or {}
    val = float(data.get('gain', 1.0))
    target = data.get('target', 'both') # 'cam', 'main', or 'both'
    if target in ('cam', 'both'):
        cam_audio_gain = val
        audio_gain = val
    if target in ('main', 'both'):
        main_audio_gain = val
    return jsonify({"success": True, "cam_gain": cam_audio_gain, "main_gain": main_audio_gain})

# ─── SD Card File Manager (Routes to CAM or MAIN) ───────────────────────────
@app.route('/api/fs/ls', methods=['POST'])
def fs_ls():
    active_ser = ser_cam if (ser_cam and ser_cam.is_open) else ser_main
    active_lock = ser_cam_lock if (ser_cam and ser_cam.is_open) else ser_main_lock

    if not (active_ser and active_ser.is_open):
        return jsonify({"error": "No device connected"})
    
    path = (request.json or {}).get("path", "/")
    res_key = "FS_LS_RES" if (active_ser == ser_cam) else "FS_LS_RES_MAIN"
    pending_responses.pop(res_key, None)
    
    cmd = {"cmd": "FS_LS", "path": path}
    try:
        with active_lock:
            active_ser.write((json.dumps(cmd) + '\n').encode('utf-8'))
            active_ser.flush()
    except Exception as e:
        return jsonify({"error": str(e)})

    for _ in range(60):
        time.sleep(0.05)
        if res_key in pending_responses:
            return jsonify(pending_responses.pop(res_key))

    return jsonify({"error": "Timeout waiting for SD card listing"})

@app.route('/api/fs/download')
def fs_download():
    path = request.args.get("path")
    active_ser = ser_cam if (ser_cam and ser_cam.is_open) else ser_main
    active_lock = ser_cam_lock if (ser_cam and ser_cam.is_open) else ser_main_lock

    if not (active_ser and active_ser.is_open):
        return "Device not connected", 400
    if not path:
        return "No path provided", 400

    res_key = "FS_READ_RES" if (active_ser == ser_cam) else "FS_READ_RES_MAIN"

    def file_stream_generator():
        offset = 0
        chunk_size = 512
        while True:
            pending_responses.pop(res_key, None)
            cmd = {"cmd": "FS_READ", "path": path, "offset": offset, "size": chunk_size}
            try:
                with active_lock:
                    active_ser.write((json.dumps(cmd) + '\n').encode('utf-8'))
                    active_ser.flush()
            except:
                break
            
            res = None
            for _ in range(60):
                time.sleep(0.05)
                if res_key in pending_responses:
                    res = pending_responses.pop(res_key)
                    break


            if not res or "error" in res:
                break

            b64_data = res.get("data", "")
            if not b64_data:
                break

            try:
                bin_chunk = base64.b64decode(b64_data)
                if len(bin_chunk) > 0:
                    yield bin_chunk
                offset += len(bin_chunk)
                if len(bin_chunk) < chunk_size:
                    break
            except:
                break

    filename = path.split('/')[-1] if '/' in path else "recording.bin"
    return Response(file_stream_generator(), mimetype="application/octet-stream",
                    headers={"Content-Disposition": f"attachment; filename={filename}"})

# ─── Application Startup ────────────────────────────────────────────────────
if __name__ == '__main__':
    def open_browser():
        time.sleep(1.2)
        print("\n" + "="*75)
        print("  * XIAO ESP32-S3 Dual-Device Vision & Audio Hub Online")
        print("  * Dashboard URL: http://127.0.0.1:5000/")
        print("  * Port 1 (XIAO CAM): Default COM9 (Camera, Mic Audio, Cam Remote)")
        print("  * Port 2 (MAIN DEVICE): Separate COM port (Telemetry, RF, Main Remote)")
        print("  * Keyboard Controls: [Arrows/WASD] Menu Nav, [Enter] OK, [Esc/B] Back")
        print("="*75 + "\n")
        webbrowser.open('http://127.0.0.1:5000/')

    def background_autoconnect():
        time.sleep(1.0)
        avail_ports = [p.device for p in serial.tools.list_ports.comports()]
        print(f"[AUTOCONNECT] Available COM ports: {avail_ports}")
        if 'COM9' in avail_ports and not (ser_cam and ser_cam.is_open):
            ok, err = do_connect_cam('COM9', 115200)
            if ok:
                print("[AUTOCONNECT] Auto-connected XIAO CAM on COM9")
            else:
                print("[AUTOCONNECT] COM9 connect error:", err)
        # Autoconnect Main Device (Walkie-Talkie on COM12)
        target_main = None
        if 'COM12' in avail_ports:
            target_main = 'COM12'
        else:
            for p in serial.tools.list_ports.comports():
                if p.device != 'COM9' and getattr(p, 'vid', None) == 0x303A:
                    target_main = p.device
                    break

        if target_main and not (ser_main and ser_main.is_open):
            ok, err = do_connect_main(target_main, 115200)
            if ok:
                print(f"[AUTOCONNECT] Auto-connected MAIN DEVICE (Walkie-Talkie) on {target_main}")
            else:
                print(f"[AUTOCONNECT] {target_main} connect error:", err)

    threading.Thread(target=background_autoconnect, daemon=True).start()
    threading.Thread(target=open_browser, daemon=True).start()
    app.run(host='0.0.0.0', port=5000, debug=False, threaded=True)

