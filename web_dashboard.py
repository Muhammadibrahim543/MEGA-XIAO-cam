import math
from flask import Flask, render_template_string, jsonify, request, Response
import serial
import serial.tools.list_ports
import threading
import json
import time
import struct
import numpy as np
import sounddevice as sd
import queue

app = Flask(__name__)

# State
device_state = {
    "appMode": 0,
    "navState": 1,
    "menuSel": 16,
    "vBatt": 0,
    "heap": 0,
    "lrPage": 0,
    "lrState": 0,
    "lrChannel": 0,
    "lrMicGain": 0,
    "lrVolGain": 1.0,
    "lrNoiseGate": 0,
    "lrSquelch": 0,
    "lrPttHeld": False,
    "liveVideoPage": 0,
    "liveVideoActive": False,
    "liveVideoState": 0,
}

ser = None
is_connected = False
pending_responses = {}

# Audio configuration
BAUD_RATE    = 921600
SAMPLE_RATE  = 16000
MAGIC_0      = 0xAA
MAGIC_1      = 0x55
FRAME_HEADER = 4
FFT_SIZE     = 1024
SPECTRUM_BANDS = 48

_audio_queue = queue.Queue(maxsize=50)
_current_db = -60.0
_audio_buffer = np.zeros(FFT_SIZE, dtype=np.float32)
_smooth_bands = np.zeros(SPECTRUM_BANDS, dtype=np.float32)
audio_stream_active = False

def create_log_bins(n_bins, n_fft, sample_rate):
    min_freq = 60
    max_freq = 8000
    min_bin = max(1, int(min_freq / (sample_rate / n_fft)))
    max_bin = min(n_fft // 2, int(max_freq / (sample_rate / n_fft)))
    edges = np.logspace(np.log10(min_bin), np.log10(max_bin), n_bins + 1)
    return edges.astype(int)

bin_edges = create_log_bins(SPECTRUM_BANDS, FFT_SIZE, SAMPLE_RATE)
hamming_window = np.hamming(FFT_SIZE)

remainder = np.array([], dtype=np.float32)
def audio_callback(outdata, frames, time_info, status):
    global remainder
    needed = frames
    out = np.zeros(frames, dtype=np.float32)
    pos = 0

    if len(remainder) > 0:
        take = min(len(remainder), needed)
        out[pos:pos+take] = remainder[:take]
        remainder = remainder[take:]
        pos += take

    while pos < needed:
        try:
            chunk = _audio_queue.get_nowait()
        except queue.Empty:
            break
        take = min(len(chunk), needed - pos)
        out[pos:pos+take] = chunk[:take]
        if take < len(chunk):
            remainder = chunk[take:]
        pos += take
    outdata[:, 0] = out

_audio_stream_obj = None

def start_audio_stream():
    global audio_stream_active, _audio_stream_obj
    if not audio_stream_active:
        try:
            devs = sd.query_devices()
            target_dev = None
            for i, d in enumerate(devs):
                if d['max_output_channels'] > 0 and "CABLE" in d['name'].upper():
                    target_dev = i
                    break
            
            _audio_stream_obj = sd.OutputStream(
                device=target_dev,
                samplerate=SAMPLE_RATE,
                channels=1,
                dtype="float32",
                latency=0.1,
                callback=audio_callback
            )
            _audio_stream_obj.start()
            audio_stream_active = True
            print("[Audio] Stream started successfully.")
        except Exception as e:
            print("[Audio] Error starting stream:", e)

def try_auto_connect():
    pass

def read_serial():
    global device_state, is_connected, ser, _current_db, _audio_buffer, _smooth_bands
    buf = bytearray()
    
    start_audio_stream()

    import traceback

    while True:
        if ser and ser.is_open:
            is_connected = True
            try:
                chunk = ser.read(ser.in_waiting or 1)
                if chunk:
                    buf.extend(chunk)
                else:
                    time.sleep(0.01)
                    continue
                
                while len(buf) > 0:
                    idx_audio = buf.find(b'\xAA\x55')
                    idx_nl = buf.find(b'\n')
                    
                    if idx_audio == -1 and idx_nl == -1:
                        if len(buf) > 8192: buf.clear()
                        break

                    if idx_audio != -1 and (idx_nl == -1 or idx_audio < idx_nl):
                        if idx_audio > 0:
                            buf = buf[idx_audio:]
                        if len(buf) < FRAME_HEADER:
                            break
                        payload_len = struct.unpack_from("<H", buf, 2)[0]
                        if payload_len % 2 != 0 or payload_len > 4096:
                            buf = buf[2:]
                            continue
                        total = FRAME_HEADER + payload_len
                        if len(buf) < total:
                            break
                            
                        raw = bytes(buf[FRAME_HEADER:total])
                        buf = buf[total:]
                        
                        samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
                        if len(samples) > 0:
                            rms = np.sqrt(np.mean(samples**2))
                            db = 20.0 * np.log10(rms) if rms > 0.0001 else -60.0
                            _current_db = 0.25 * db + 0.75 * _current_db
                            
                            n = len(samples)
                            if n >= FFT_SIZE:
                                _audio_buffer[:] = samples[-FFT_SIZE:]
                            else:
                                _audio_buffer = np.roll(_audio_buffer, -n)
                                _audio_buffer[-n:] = samples
                                
                            windowed = _audio_buffer * hamming_window
                            fft_vals = np.abs(np.fft.rfft(windowed)) / (FFT_SIZE / 2)
                            
                            for i in range(SPECTRUM_BANDS):
                                band_energy = np.mean(fft_vals[bin_edges[i]:bin_edges[i+1]]) if bin_edges[i+1] > bin_edges[i] else 0
                                _smooth_bands[i] = max(_smooth_bands[i] * 0.7, band_energy)
                            
                            if not _audio_queue.full():
                                _audio_queue.put_nowait(samples)
                    else:
                        line_bytes = buf[:idx_nl]
                        buf = buf[idx_nl+1:]
                        if b'{' in line_bytes:
                            idx_brace = line_bytes.find(b'{')
                            line_str = line_bytes[idx_brace:].decode('utf-8', errors='ignore').strip()
                            try:
                                data = json.loads(line_str)
                                print(f"[ESP32 Data Received] {line_str}")
                                if data.get("type") == "STATE":
                                    for key, value in data.items():
                                        device_state[key] = value
                                elif data.get("type") in ("FS_LS_RES", "FS_READ_RES"):
                                    pending_responses[data.get("type")] = data
                            except json.JSONDecodeError:
                                pass

            except Exception as e:
                print("Serial Error:", e)
                traceback.print_exc()
                is_connected = False
                try: ser.close()
                except: pass
                ser = None
                time.sleep(0.5)
        else:
            is_connected = False
            time.sleep(1)



@app.after_request
def add_header(r):
    r.headers["Cache-Control"] = "no-cache, no-store, must-revalidate"
    r.headers["Pragma"] = "no-cache"
    r.headers["Expires"] = "0"
    return r

@app.route('/')
def index():
    try:
        import os
        template_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dashboard_template.html')
        with open(template_path, 'r', encoding='utf-8') as f:
            html = f.read()
        ports = [port.device for port in serial.tools.list_ports.comports()]
        return render_template_string(html, ports=ports)
    except FileNotFoundError:
        return "dashboard_template.html not found. Please wait while it is generated."

@app.route('/connect', methods=['POST'])
def connect():
    global ser, is_connected
    port = request.json.get('port')
    try:
        if ser:
            try: ser.close()
            except: pass
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = 115200
        ser.timeout = 0.1
        ser.dtr = False
        ser.rts = False
        ser.open()
        is_connected = True
        return jsonify({"success": True})
    except Exception as e:
        is_connected = False
        return jsonify({"success": False, "error": str(e)})

@app.route('/disconnect', methods=['POST'])
def disconnect():
    global ser, is_connected
    try:
        if ser:
            try: ser.close()
            except: pass
            ser = None
        is_connected = False
        return jsonify({"success": True})
    except Exception as e:
        return jsonify({"success": False, "error": str(e)})

@app.route('/state')
def get_state():
    state = device_state.copy()
    state["connected"] = is_connected and (ser is not None and ser.is_open)
    db_val = float(_current_db)
    if math.isinf(db_val) or math.isnan(db_val):
        db_val = -100.0
    state["mic_db"] = db_val
    state["mic_spectrum"] = _smooth_bands.tolist()
    return jsonify(state)

@app.route('/stream')
def stream():
    def generate():
        while True:
            state = device_state.copy()
            state["connected"] = is_connected and (ser is not None and ser.is_open)
            db_val = float(_current_db)
            if math.isinf(db_val) or math.isnan(db_val):
                db_val = -100.0
            state["mic_db"] = db_val
            state["mic_spectrum"] = _smooth_bands.tolist()
            try:
                dumped = json.dumps(state)
            except Exception as e:
                dumped = json.dumps({"error": repr(e), "state_keys": str(list(state.keys()))})
            yield f"data: {dumped}\n\n"
            time.sleep(0.05)
    return Response(generate(), mimetype='text/event-stream')

@app.route('/cmd', methods=['POST'])
def send_cmd():
    if ser and ser.is_open:
        cmd_data = request.json
        ser.write((json.dumps(cmd_data) + '\n').encode('utf-8'))
        return jsonify({"success": True})
    return jsonify({"success": False})

import base64

@app.route('/api/fs/ls', methods=['POST'])
def fs_ls():
    if not (ser and ser.is_open):
        return jsonify({"error": "Not connected"})
    path = request.json.get("path", "/")
    pending_responses.pop("FS_LS_RES", None)
    cmd = {"cmd": "FS_LS", "path": path}
    ser.write((json.dumps(cmd) + '\n').encode('utf-8'))
    
    for _ in range(30):
        time.sleep(0.1)
        if "FS_LS_RES" in pending_responses:
            res = pending_responses.pop("FS_LS_RES")
            return jsonify(res)
    return jsonify({"error": "Timeout waiting for ESP32"})

@app.route('/api/fs/download')
def fs_download():
    path = request.args.get("path")
    if not (ser and ser.is_open):
        return "Not connected", 400

    def generate():
        offset = 0
        chunk_size = 512
        while True:
            pending_responses.pop("FS_READ_RES", None)
            cmd = {"cmd": "FS_READ", "path": path, "offset": offset, "size": chunk_size}
            try:
                ser.write((json.dumps(cmd) + '\n').encode('utf-8'))
            except:
                break
            
            res = None
            for _ in range(50):
                time.sleep(0.1)
                if "FS_READ_RES" in pending_responses:
                    res = pending_responses.pop("FS_READ_RES")
                    break
            
            if not res or "error" in res:
                break
                
            b64_data = res.get("data", "")
            if not b64_data:
                break
                
            try:
                bin_data = base64.b64decode(b64_data)
                if len(bin_data) > 0:
                    yield bin_data
                offset += len(bin_data)
                if len(bin_data) < chunk_size:
                    break
            except:
                break

    filename = path.split('/')[-1] if '/' in path else "download.bin"
    return Response(generate(), mimetype="application/octet-stream", headers={"Content-Disposition": f"attachment; filename={filename}"})

if __name__ == '__main__':
    import os
    import subprocess
    import webbrowser
    import threading
    import time

    threading.Thread(target=read_serial, daemon=True).start()
    
    def launch_app():
        time.sleep(1.5)
        print("[Desktop App] Opening in Default Browser...")
        webbrowser.open('http://127.0.0.1:5000/')

    threading.Thread(target=launch_app, daemon=True).start()
    
    app.run(port=5000, debug=False, threaded=True)
