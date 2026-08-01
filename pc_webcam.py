import serial
import serial.tools.list_ports
import cv2
import numpy as np
import time
import struct
import threading
import queue
import customtkinter as ctk
from PIL import Image
import sounddevice as sd

try:
    import pyvirtualcam
    HAS_PYVIRTUALCAM = True
except ImportError:
    HAS_PYVIRTUALCAM = False

# Initialize CustomTkinter aesthetics
ctk.set_appearance_mode("Dark")
ctk.set_default_color_theme("blue")

MAGIC_VID = b'\x78\x56\x34\x12'  # 0x12345678 (Video JPEG Frame)
MAGIC_AUD = b'\x21\x43\x65\x87'  # 0x87654321 (Audio PCM Chunk)

SAMPLE_RATE = 16000
CHANNELS = 1

# TUI Spectrum settings
FFT_SIZE = 1024
SPECTRUM_BANDS = 48

def create_log_bins(n_bins, n_fft, sample_rate):
    min_freq = 60
    max_freq = 8000
    min_bin = max(1, int(min_freq / (sample_rate / n_fft)))
    max_bin = min(n_fft // 2, int(max_freq / (sample_rate / n_fft)))
    edges = np.logspace(np.log10(min_bin), np.log10(max_bin), n_bins + 1)
    return edges.astype(int)

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
            if self.prebuffering or self.count < n_samples:
                return np.zeros(n_samples, dtype=np.float32)
            
            out = np.zeros(n_samples, dtype=np.float32)
            for i in range(n_samples):
                out[i] = self.buf[self.read_idx]
                self.read_idx = (self.read_idx + 1) % len(self.buf)
            self.count -= n_samples
            
            if self.count == 0:
                self.prebuffering = True
                
            return out

class ModernCamUI(ctk.CTk):
    def __init__(self, ser):
        super().__init__()

        self.ser = ser
        self.title("XIAO ESP32-S3 Vision & Audio Hub")
        self.geometry("1180x800")
        self.minsize(900, 600)
        
        # Threading & Virtual Cam / Mic State
        self.current_frame = None
        self.frame_lock = threading.Lock()
        self.running = True
        
        self.vcam = None
        self.vcam_enabled = False
        
        self.mic_enabled = False
        self.audio_gain = 1.0
        self.audio_queue = queue.Queue(maxsize=50)
        self.jitter_buffer = AudioJitterBuffer()
        self.dc_offset = 0.0
        self.audio_stream = None
        self.current_db = -60.0
        
        self.audio_buffer = np.zeros(FFT_SIZE, dtype=np.float32)
        self.fft_bands = np.zeros(SPECTRUM_BANDS, dtype=np.float32)
        
        # Create logarithmic bin edges for FFT spectrum
        min_bin = 1
        max_bin = FFT_SIZE // 2
        self.bin_edges = np.logspace(np.log10(min_bin), np.log10(max_bin), SPECTRUM_BANDS + 1).astype(int)

        # Configure Grid Layout (Left Controls Sidebar, Right Media Panel)
        self.grid_columnconfigure(1, weight=1)
        self.grid_rowconfigure(0, weight=1)

        # ================= LEFT PANEL (Controls Sidebar) =================
        self.sidebar_scroll = ctk.CTkScrollableFrame(self, width=320, corner_radius=0)
        self.sidebar_scroll.grid(row=0, column=0, sticky="nsew")

        # Logo / Title
        self.logo_label = ctk.CTkLabel(self.sidebar_scroll, text="⚡ XIAO Vision & Audio Hub", font=ctk.CTkFont(size=18, weight="bold"))
        self.logo_label.pack(padx=15, pady=(15, 10), anchor="w")

        # ---------------- VIDEO CONTROLS CARD ----------------
        self.cam_card = ctk.CTkFrame(self.sidebar_scroll, corner_radius=10)
        self.cam_card.pack(padx=10, pady=8, fill="x")

        self.cam_card_title = ctk.CTkLabel(self.cam_card, text="📹 Camera Settings", font=ctk.CTkFont(size=14, weight="bold"))
        self.cam_card_title.pack(padx=15, pady=(10, 5), anchor="w")

        # Video Switch (Enable/Disable Camera Stream)
        self.cam_switch = ctk.CTkSwitch(self.cam_card, text="Camera Feed (ON)", command=self.toggle_cam_stream)
        self.cam_switch.select()
        self.cam_switch.pack(padx=15, pady=5, anchor="w")

        # Resolution Dropdown
        self.res_label = ctk.CTkLabel(self.cam_card, text="Resolution:")
        self.res_label.pack(padx=15, pady=(5, 0), anchor="w")
        
        self.resolutions = {
            "QVGA (320x240)": 5,
            "VGA (640x480)": 8,
            "SVGA (800x600)": 9,
            "XGA (1024x768)": 10,
            "HD (1280x720)": 11,
            "SXGA (1280x1024)": 12
        }
        self.res_combo = ctk.CTkComboBox(self.cam_card, values=list(self.resolutions.keys()), command=self.on_res_change)
        self.res_combo.set("SVGA (800x600)")
        self.res_combo.pack(padx=15, pady=(2, 10), fill="x")

        # Quality Slider
        self.q_label = ctk.CTkLabel(self.cam_card, text="Quality (Lower = Better): 10")
        self.q_label.pack(padx=15, pady=(5, 0), anchor="w")
        self.q_slider = ctk.CTkSlider(self.cam_card, from_=10, to=40, command=self.on_q_change)
        self.q_slider.set(10)
        self.q_slider.pack(padx=15, pady=(2, 10), fill="x")

        # Brightness
        self.bright_label = ctk.CTkLabel(self.cam_card, text="Brightness")
        self.bright_label.pack(padx=15, pady=(2, 0), anchor="w")
        self.bright_slider = ctk.CTkSlider(self.cam_card, from_=-2, to=2, number_of_steps=4, command=lambda v: self.send_cmd('B', int(v)))
        self.bright_slider.set(0)
        self.bright_slider.pack(padx=15, pady=(2, 8), fill="x")

        # Contrast
        self.contrast_label = ctk.CTkLabel(self.cam_card, text="Contrast")
        self.contrast_label.pack(padx=15, pady=(2, 0), anchor="w")
        self.contrast_slider = ctk.CTkSlider(self.cam_card, from_=-2, to=2, number_of_steps=4, command=lambda v: self.send_cmd('C', int(v)))
        self.contrast_slider.set(0)
        self.contrast_slider.pack(padx=15, pady=(2, 8), fill="x")

        # Switches Frame (H-Mirror & V-Flip)
        self.switches_frame = ctk.CTkFrame(self.cam_card, fg_color="transparent")
        self.switches_frame.pack(padx=15, pady=5, fill="x")
        self.switches_frame.grid_columnconfigure((0, 1), weight=1)

        self.hm_switch = ctk.CTkSwitch(self.switches_frame, text="H-Mirror", command=lambda: self.send_cmd('HM', self.hm_switch.get()))
        self.hm_switch.grid(row=0, column=0, sticky="w")
        
        self.vf_switch = ctk.CTkSwitch(self.switches_frame, text="V-Flip", command=lambda: self.send_cmd('VF', self.vf_switch.get()))
        self.vf_switch.grid(row=0, column=1, sticky="w")

        # Virtual Cam Switch
        self.vcam_switch = ctk.CTkSwitch(self.cam_card, text="Virtual Cam (Meet/Zoom)", command=self.toggle_vcam)
        self.vcam_switch.pack(padx=15, pady=(8, 12), anchor="w")


        # ---------------- MICROPHONE CONTROLS CARD ----------------
        self.mic_card = ctk.CTkFrame(self.sidebar_scroll, corner_radius=10)
        self.mic_card.pack(padx=10, pady=8, fill="x")

        self.mic_card_title = ctk.CTkLabel(self.mic_card, text="🎙️ Microphone & Audio", font=ctk.CTkFont(size=14, weight="bold"))
        self.mic_card_title.pack(padx=15, pady=(10, 5), anchor="w")

        # Microphone Stream Switch
        self.mic_switch = ctk.CTkSwitch(self.mic_card, text="Microphone Stream", command=self.toggle_mic_stream)
        self.mic_switch.pack(padx=15, pady=5, anchor="w")

        # Audio Output Device Dropdown
        self.aud_dev_label = ctk.CTkLabel(self.mic_card, text="Output Audio Device:")
        self.aud_dev_label.pack(padx=15, pady=(5, 0), anchor="w")
        
        self.audio_devices = self.get_output_audio_devices()
        dev_names = list(self.audio_devices.keys()) if self.audio_devices else ["No Output Device"]
        
        self.aud_dev_combo = ctk.CTkComboBox(self.mic_card, values=dev_names, command=self.on_audio_device_change)
        
        # Auto-select VB-Audio Cable if found
        default_dev = dev_names[0]
        for name in dev_names:
            if "CABLE" in name.upper():
                default_dev = name
                break
        self.aud_dev_combo.set(default_dev)
        self.aud_dev_combo.pack(padx=15, pady=(2, 10), fill="x")

        # Audio Gain Slider
        self.gain_label = ctk.CTkLabel(self.mic_card, text="Audio Gain: 1.0x")
        self.gain_label.pack(padx=15, pady=(2, 0), anchor="w")
        self.gain_slider = ctk.CTkSlider(self.mic_card, from_=0.5, to=5.0, number_of_steps=45, command=self.on_gain_change)
        self.gain_slider.set(1.0)
        self.gain_slider.pack(padx=15, pady=(2, 12), fill="x")


        # ---------------- SYSTEM INFO BOX ----------------
        self.info_box = ctk.CTkTextbox(self.sidebar_scroll, height=110, corner_radius=10)
        self.info_box.pack(padx=10, pady=(8, 15), fill="x")
        self.info_box.insert("0.0", "System Ready.\nWaiting for ESP32-S3 Stream...")
        self.info_box.configure(state="disabled")

        # ================= RIGHT PANEL (Video Feed & Audio Dashboard) =================
        self.right_panel = ctk.CTkFrame(self, corner_radius=0, fg_color="transparent")
        self.right_panel.grid(row=0, column=1, padx=15, pady=15, sticky="nsew")
        self.right_panel.grid_rowconfigure(0, weight=4)  # Video gets more height
        self.right_panel.grid_rowconfigure(1, weight=1)  # Audio visualizer panel
        self.right_panel.grid_columnconfigure(0, weight=1)

        # Top Video Frame
        self.video_frame = ctk.CTkFrame(self.right_panel, corner_radius=12, fg_color="#1E1E1E")
        self.video_frame.grid(row=0, column=0, padx=0, pady=(0, 10), sticky="nsew")
        self.video_frame.grid_rowconfigure(0, weight=1)
        self.video_frame.grid_columnconfigure(0, weight=1)

        self.video_label = ctk.CTkLabel(self.video_frame, text="NO SIGNAL", font=ctk.CTkFont(size=28, weight="bold"), text_color="gray")
        self.video_label.grid(row=0, column=0, sticky="nsew")

        # Bottom Audio Visualizer Panel
        self.audio_panel = ctk.CTkFrame(self.right_panel, corner_radius=12, fg_color="#1E1E1E")
        self.audio_panel.grid(row=1, column=0, padx=0, pady=0, sticky="nsew")
        self.audio_panel.grid_columnconfigure(0, weight=1)

        self.aud_panel_title = ctk.CTkLabel(self.audio_panel, text="🎙️ Live Microphone Spectrum & Volume Level", font=ctk.CTkFont(size=13, weight="bold"))
        self.aud_panel_title.pack(padx=15, pady=(8, 2), anchor="w")

        # Volume Meter Bar (Canvas)
        self.vol_canvas = ctk.CTkCanvas(self.audio_panel, height=18, bg="#252526", highlightthickness=0)
        self.vol_canvas.pack(padx=15, pady=(2, 4), fill="x")

        # Spectrum Visualizer (Canvas)
        self.spec_canvas = ctk.CTkCanvas(self.audio_panel, height=75, bg="#252526", highlightthickness=0)
        self.spec_canvas.pack(padx=15, pady=(2, 8), fill="x")


        # ================= START THREADS & TIMERS =================
        self.stream_thread = threading.Thread(target=self.receive_stream, daemon=True)
        self.stream_thread.start()
        
        # Start initial camera command
        self.send_cmd('CAM', 1)

        # UI Update loops
        self.update_video()
        self.update_audio_visualizer()

    def get_output_audio_devices(self):
        """Query all available output audio devices on the system"""
        devs = {}
        try:
            all_devs = sd.query_devices()
            for i, d in enumerate(all_devs):
                if d["max_output_channels"] > 0:
                    name = f"[{i}] {d['name']}"
                    devs[name] = i
        except Exception as e:
            print(f"[AUDIO DEV QUERY ERROR]: {e}")
        return devs

    def send_cmd(self, prefix, val):
        try:
            self.ser.write(f"SET:{prefix}:{val}\n".encode())
        except Exception as e:
            print(f"[SERIAL SEND ERROR]: {e}")

    def on_res_change(self, choice):
        val = self.resolutions[choice]
        self.send_cmd('RES', val)

    def on_q_change(self, val):
        val = int(val)
        self.q_label.configure(text=f"Quality (Lower = Better): {val}")
        self.send_cmd('Q', val)

    def on_gain_change(self, val):
        self.audio_gain = float(val)
        self.gain_label.configure(text=f"Audio Gain: {self.audio_gain:.1f}x")

    def toggle_cam_stream(self):
        enabled = self.cam_switch.get()
        self.send_cmd('CAM', 1 if enabled else 0)
        if enabled:
            self.update_info("Camera Stream Enabled")
        else:
            self.update_info("Camera Stream Muted")
            self.video_label.configure(image=None, text="CAMERA OFF")

    def toggle_mic_stream(self):
        enabled = self.mic_switch.get()
        self.send_cmd('MIC', 1 if enabled else 0)
        if enabled:
            self.start_audio_output()
            self.update_info("Microphone Stream Enabled 🟢")
        else:
            self.stop_audio_output()
            self.update_info("Microphone Muted 🔴")

    def on_audio_device_change(self, choice):
        if choice in self.audio_devices:
            dev_idx = self.audio_devices[choice]
            if self.mic_enabled:
                self.start_audio_output(dev_idx)

    def start_audio_output(self, dev_idx=None):
        self.stop_audio_output()
        if dev_idx is None:
            choice = self.aud_dev_combo.get()
            dev_idx = self.audio_devices.get(choice, None)

        target_sr = SAMPLE_RATE
        if dev_idx is not None:
            try:
                dev_info = sd.query_devices(dev_idx, 'output')
                target_sr = int(dev_info.get('default_samplerate', SAMPLE_RATE))
            except:
                pass

        ratio = SAMPLE_RATE / target_sr

        def audio_callback(outdata, frames, time_info, status):
            needed_in = int(round(frames * ratio))
            if needed_in < 1: needed_in = 1
            
            concat_in = self.jitter_buffer.read(needed_in)
            if abs(ratio - 1.0) > 1e-4 and len(concat_in) > 1:
                x_old = np.linspace(0, 1, len(concat_in), endpoint=False)
                x_new = np.linspace(0, 1, frames, endpoint=False)
                out = np.interp(x_new, x_old, concat_in)
            else:
                out = concat_in

            outdata[:, 0] = out.astype(np.float32)

        try:
            self.audio_stream = sd.OutputStream(
                device=dev_idx,
                samplerate=target_sr,
                channels=CHANNELS,
                dtype="float32",
                latency="low",
                callback=audio_callback
            )
            self.audio_stream.start()
            self.mic_enabled = True
        except Exception as e:
            try:
                self.audio_stream = sd.OutputStream(
                    device=dev_idx,
                    samplerate=SAMPLE_RATE,
                    channels=CHANNELS,
                    dtype="float32",
                    latency="low",
                    callback=audio_callback
                )
                self.audio_stream.start()
                self.mic_enabled = True
            except Exception as e2:
                print(f"[AUDIO OUTPUT ERROR]: {e2}")
                self.update_info(f"Audio Output Error:\n{e2}")

    def stop_audio_output(self):
        self.mic_enabled = False
        if self.audio_stream:
            try:
                self.audio_stream.stop()
                self.audio_stream.close()
            except:
                pass
            self.audio_stream = None

    def toggle_vcam(self):
        if not HAS_PYVIRTUALCAM:
            self.vcam_switch.deselect()
            self.update_info("pyvirtualcam not installed!\nRun: pip install pyvirtualcam")
            return
        
        if self.vcam_switch.get() == 1:
            self.vcam_enabled = True
            self.update_info("Virtual Cam Starting...")
        else:
            self.vcam_enabled = False
            if self.vcam:
                try:
                    self.vcam.close()
                except:
                    pass
                self.vcam = None
            self.update_info("Virtual Cam Stopped.")

    def receive_stream(self):
        buffer = bytearray()
        fps_time = time.time()
        frame_count = 0
        bytes_count = 0
        audio_bytes_count = 0

        while self.running:
            try:
                chunk = self.ser.read(4096)
                if not chunk:
                    continue
                    
                buffer.extend(chunk)
                bytes_count += len(chunk)
                
                while True:
                    if len(buffer) < 8:
                        break
                    
                    vid_idx = buffer.find(MAGIC_VID)
                    aud_idx = buffer.find(MAGIC_AUD)
                    
                    if vid_idx == -1 and aud_idx == -1:
                        buffer = buffer[-3:]
                        break
                    
                    # Determine which packet header is first
                    if vid_idx != -1 and (aud_idx == -1 or vid_idx < aud_idx):
                        if vid_idx > 0:
                            buffer = buffer[vid_idx:]
                        if len(buffer) < 8:
                            break
                        
                        frame_size = struct.unpack('<I', buffer[4:8])[0]
                        if frame_size <= 0 or frame_size > 2 * 1024 * 1024:
                            buffer = buffer[4:] # Skip corrupted magic
                            continue
                        if len(buffer) < 8 + frame_size:
                            break
                        
                        frame_data = bytes(buffer[8 : 8 + frame_size])
                        buffer = buffer[8 + frame_size :]
                        
                        img_array = np.frombuffer(frame_data, dtype=np.uint8)
                        frame = cv2.imdecode(img_array, cv2.IMREAD_COLOR)
                        
                        if frame is not None:
                            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                            with self.frame_lock:
                                self.current_frame = frame

                            if self.vcam_enabled and HAS_PYVIRTUALCAM:
                                h, w, _ = frame.shape
                                if self.vcam is None or self.vcam.width != w or self.vcam.height != h:
                                    if self.vcam:
                                        try: self.vcam.close()
                                        except: pass
                                    try:
                                        self.vcam = pyvirtualcam.Camera(width=w, height=h, fps=30, fmt=pyvirtualcam.PixelFormat.RGB)
                                        self.update_info(f"Virtual Cam Active:\n{self.vcam.device}")
                                    except Exception as e:
                                        self.vcam_enabled = False
                                        self.vcam_switch.deselect()
                                        self.update_info(f"Virtual Cam Error:\n{e}")
                                if self.vcam:
                                    try: self.vcam.send(frame)
                                    except: pass
                            frame_count += 1
                    else:
                        # Audio Packet Processing
                        if aud_idx > 0:
                            buffer = buffer[aud_idx:]
                        if len(buffer) < 8:
                            break
                        
                        audio_size = struct.unpack('<I', buffer[4:8])[0]
                        if audio_size <= 0 or audio_size > 65536 or audio_size % 2 != 0:
                            buffer = buffer[4:] # Skip corrupted header
                            continue
                        if len(buffer) < 8 + audio_size:
                            break
                        
                        pcm_data = bytes(buffer[8 : 8 + audio_size])
                        buffer = buffer[8 + audio_size :]
                        audio_bytes_count += audio_size
                        
                        # Convert 16-bit PCM to float32 (-1.0 to +1.0)
                        samples = np.frombuffer(pcm_data, dtype=np.int16).astype(np.float32) / 32768.0
                        
                        # Real-time DC Offset Removal (Centers waveform at 0)
                        if len(samples) > 0:
                            mean_val = np.mean(samples)
                            self.dc_offset = 0.95 * self.dc_offset + 0.05 * mean_val
                            samples -= self.dc_offset

                        if self.audio_gain != 1.0:
                            samples *= self.audio_gain
                            np.clip(samples, -1.0, 1.0, out=samples)
                        
                        # Calculate volume in dB & FFT spectrum for GUI
                        if len(samples) > 0:
                            rms = np.sqrt(np.mean(samples**2))
                            db = 20.0 * np.log10(rms) if rms > 0.0001 else -60.0
                            self.current_db = 0.25 * db + 0.75 * self.current_db
                            
                            # Roll audio FFT buffer
                            n = len(samples)
                            if n >= FFT_SIZE:
                                self.audio_buffer[:] = samples[-FFT_SIZE:]
                            else:
                                self.audio_buffer = np.roll(self.audio_buffer, -n)
                                self.audio_buffer[-n:] = samples
                            
                            # Calculate FFT frequency bands
                            windowed = self.audio_buffer * np.hanning(FFT_SIZE)
                            fft_mag = np.abs(np.fft.rfft(windowed)) / FFT_SIZE
                            bands = np.zeros(SPECTRUM_BANDS, dtype=np.float32)
                            for i in range(SPECTRUM_BANDS):
                                s_bin = self.bin_edges[i]
                                e_bin = max(s_bin + 1, self.bin_edges[i+1])
                                bands[i] = np.mean(fft_mag[s_bin:e_bin])
                            
                            bands_db = 20 * np.log10(bands + 1e-6)
                            bands_norm = np.clip((bands_db + 60) / 60.0, 0.0, 1.0)
                            self.fft_bands = 0.35 * bands_norm + 0.65 * self.fft_bands

                        if self.mic_enabled:
                            self.jitter_buffer.write(samples)

                # Update Stats timer
                current_time = time.time()
                if current_time - fps_time >= 1.0:
                    fps = frame_count / (current_time - fps_time)
                    kbps = (bytes_count / 1024) / (current_time - fps_time)
                    aud_kbps = (audio_bytes_count / 1024) / (current_time - fps_time)
                    
                    vcam_str = "ON" if self.vcam_enabled else "OFF"
                    mic_str = "ACTIVE 🟢" if self.mic_enabled else "MUTED 🔴"
                    
                    info_str = (
                        f"Video FPS: {fps:.1f} ({kbps:.1f} KB/s)\n"
                        f"Audio Stream: {mic_str} ({aud_kbps:.1f} KB/s)\n"
                        f"Virtual Cam: {vcam_str} | Level: {self.current_db:.1f} dB"
                    )
                    self.update_info(info_str)
                    
                    fps_time = current_time
                    frame_count = 0
                    bytes_count = 0
                    audio_bytes_count = 0
            except Exception as e:
                time.sleep(0.05)

    def update_info(self, text):
        def _update():
            self.info_box.configure(state="normal")
            self.info_box.delete("0.0", "end")
            self.info_box.insert("0.0", text)
            self.info_box.configure(state="disabled")
        self.after(0, _update)

    def update_video(self):
        try:
            if not self.running: return
            with self.frame_lock:
                frame = self.current_frame
                
            if frame is not None and self.cam_switch.get() == 1:
                frame_height, frame_width, _ = frame.shape
                
                lbl_w = self.video_frame.winfo_width()
                lbl_h = self.video_frame.winfo_height()
                
                if lbl_w > 10 and lbl_h > 10:
                    scale_w = lbl_w / frame_width
                    scale_h = lbl_h / frame_height
                    scale = min(scale_w, scale_h)
                    
                    new_w = int(frame_width * scale)
                    new_h = int(frame_height * scale)
                    
                    img = Image.fromarray(frame)
                    self.current_ctk_img = ctk.CTkImage(light_image=img, dark_image=img, size=(new_w, new_h))
                    self.video_label.configure(image=self.current_ctk_img, text="")
        except Exception:
            pass
        
        if self.running:
            self.after(33, self.update_video)

    def update_audio_visualizer(self):
        try:
            if not self.running: return
            # 1. Update Volume Meter Canvas
            w = self.vol_canvas.winfo_width()
            h = self.vol_canvas.winfo_height()
            if w > 10 and h > 5:
                self.vol_canvas.delete("all")
                db_val = max(-60.0, min(0.0, self.current_db))
                pct = (db_val + 60.0) / 60.0
                fill_w = int(pct * w)
                
                # Color gradient depending on volume level
                if db_val < -15.0:
                    color = "#2EB872"  # Vibrant Green
                elif db_val < -5.0:
                    color = "#F4AF25"  # Warning Yellow
                else:
                    color = "#E74C3C"  # Peak Red
                    
                self.vol_canvas.create_rectangle(0, 0, fill_w, h, fill=color, outline="")
                
                # Draw dB text overlay
                text_str = f"{db_val:.1f} dB"
                self.vol_canvas.create_text(w - 40, h // 2, text=text_str, fill="#FFFFFF", font=("Consolas", 9, "bold"))

            # 2. Update Spectrum Visualizer Canvas
            sw = self.spec_canvas.winfo_width()
            sh = self.spec_canvas.winfo_height()
            if sw > 10 and sh > 10:
                self.spec_canvas.delete("all")
                n_bands = SPECTRUM_BANDS
                bar_gap = 3
                bar_w = max(2, (sw - (n_bands + 1) * bar_gap) // n_bands)
                
                for i, val in enumerate(self.fft_bands):
                    bar_h = int(val * (sh - 4))
                    x0 = bar_gap + i * (bar_w + bar_gap)
                    y0 = sh - bar_h
                    x1 = x0 + bar_w
                    y1 = sh
                    
                    # Height color
                    if val < 0.5:
                        bar_color = "#007ACC"
                    elif val < 0.75:
                        bar_color = "#3ABF92"
                    else:
                        bar_color = "#FF9500"
                        
                    if bar_h > 0:
                        self.spec_canvas.create_rectangle(x0, y0, x1, y1, fill=bar_color, outline="")
        except Exception:
            pass

        # Schedule next visualizer update (40ms = ~25 FPS)
        if self.running:
            self.after(40, self.update_audio_visualizer)

    def on_closing(self):
        self.running = False
        self.send_cmd('CAM', 0)
        self.send_cmd('MIC', 0)
        self.stop_audio_output()
        if self.vcam:
            try:
                self.vcam.close()
            except:
                pass
            self.vcam = None
        try:
            self.ser.close()
        except:
            pass
        self.destroy()

def main():
    print("ESP32-S3 Vision & Audio Hub - Modern UI")
    print("-" * 40)
    print("Available ports:")
    ports = serial.tools.list_ports.comports()
    
    if not ports:
        print("No serial ports found! Please connect the ESP32-S3.")
        return
        
    for i, port in enumerate(ports):
        print(f" {i+1}. {port.device}: {port.description}")
        
    print(f" {len(ports)+1}. Auto-detect")
    
    while True:
        choice = input(f"\nEnter the number of the port you want to use (1-{len(ports)+1}): ")
        try:
            choice = int(choice)
            if 1 <= choice <= len(ports):
                port = ports[choice-1].device
                break
            elif choice == len(ports)+1:
                port = None
                for p in ports:
                    if "USB" in p.description or "Serial" in p.description:
                        port = p.device
                        break
                if port:
                    print(f"Auto-selected {port}")
                    break
                else:
                    port = ports[0].device
                    print(f"Fallback to {port}")
                    break
            else:
                print("Invalid choice.")
        except ValueError:
            print("Please enter a valid number.")

    print(f"\nConnecting to {port}...")
    try:
        ser = serial.Serial(port, 2000000, timeout=1)
        ser.set_buffer_size(rx_size=1024*1024)
    except Exception as e:
        print(f"Failed to open {port}: {e}")
        return

    print("Connected! Launching Vision & Audio Hub...")
    
    app = ModernCamUI(ser)
    app.protocol("WM_DELETE_WINDOW", app.on_closing)
    app.mainloop()

if __name__ == '__main__':
    main()
