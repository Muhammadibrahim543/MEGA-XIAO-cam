import serial
import serial.tools.list_ports
import cv2
import numpy as np
import time
import struct
import threading
import customtkinter as ctk
from PIL import Image

try:
    import pyvirtualcam
    HAS_PYVIRTUALCAM = True
except ImportError:
    HAS_PYVIRTUALCAM = False

# Initialize CustomTkinter aesthetics
ctk.set_appearance_mode("Dark")  # Modes: "System" (standard), "Dark", "Light"
ctk.set_default_color_theme("blue")  # Themes: "blue" (standard), "green", "dark-blue"

MAGIC_VID = b'\x78\x56\x34\x12'  # 0x12345678 in little-endian

class ModernCamUI(ctk.CTk):
    def __init__(self, ser):
        super().__init__()

        self.ser = ser
        self.title("XIAO ESP32-S3 Vision Hub")
        self.geometry("1100x700")
        self.minsize(800, 500)
        
        # Threading & Virtual Cam
        self.current_frame = None
        self.frame_lock = threading.Lock()
        self.running = True
        self.vcam = None
        self.vcam_enabled = False

        # Configure Grid Layout
        self.grid_columnconfigure(1, weight=1)
        self.grid_rowconfigure(0, weight=1)

        # ================= LEFT PANEL (Controls) =================
        self.sidebar = ctk.CTkFrame(self, width=280, corner_radius=0)
        self.sidebar.grid(row=0, column=0, sticky="nsew")
        self.sidebar.grid_rowconfigure(11, weight=1)  # Push bottom elements down

        # Logo / Title
        self.logo_label = ctk.CTkLabel(self.sidebar, text="Camera Controls", font=ctk.CTkFont(size=20, weight="bold"))
        self.logo_label.grid(row=0, column=0, padx=20, pady=(20, 20))

        # Resolution Dropdown
        self.res_label = ctk.CTkLabel(self.sidebar, text="Resolution:")
        self.res_label.grid(row=1, column=0, padx=20, pady=(10, 0), sticky="w")
        
        self.resolutions = {
            "QVGA (320x240)": 5,
            "VGA (640x480)": 8,
            "SVGA (800x600)": 9,
            "XGA (1024x768)": 10,
            "HD (1280x720)": 11,
            "SXGA (1280x1024)": 12
        }
        self.res_combo = ctk.CTkComboBox(self.sidebar, values=list(self.resolutions.keys()), command=self.on_res_change)
        self.res_combo.set("SVGA (800x600)")
        self.res_combo.grid(row=2, column=0, padx=20, pady=(0, 15), sticky="ew")

        # Quality Slider
        self.q_label = ctk.CTkLabel(self.sidebar, text="Quality (Lower = Better): 10")
        self.q_label.grid(row=3, column=0, padx=20, pady=(10, 0), sticky="w")
        self.q_slider = ctk.CTkSlider(self.sidebar, from_=10, to=40, command=self.on_q_change)
        self.q_slider.set(10)
        self.q_slider.grid(row=4, column=0, padx=20, pady=(0, 15), sticky="ew")

        # Image Settings (Brightness, Contrast, Saturation)
        self.img_label = ctk.CTkLabel(self.sidebar, text="Image Adjustments", font=ctk.CTkFont(weight="bold"))
        self.img_label.grid(row=5, column=0, padx=20, pady=(15, 5), sticky="w")

        # Brightness
        self.bright_slider = ctk.CTkSlider(self.sidebar, from_=-2, to=2, number_of_steps=4, command=lambda v: self.send_cmd('B', int(v)))
        self.bright_slider.set(0)
        self.bright_slider.grid(row=6, column=0, padx=20, pady=5, sticky="ew")
        
        # Contrast
        self.contrast_slider = ctk.CTkSlider(self.sidebar, from_=-2, to=2, number_of_steps=4, command=lambda v: self.send_cmd('C', int(v)))
        self.contrast_slider.set(0)
        self.contrast_slider.grid(row=7, column=0, padx=20, pady=5, sticky="ew")

        # Switches Frame (H-Mirror & V-Flip)
        self.switches_frame = ctk.CTkFrame(self.sidebar, fg_color="transparent")
        self.switches_frame.grid(row=8, column=0, padx=20, pady=10, sticky="ew")
        self.switches_frame.grid_columnconfigure((0, 1), weight=1)

        self.hm_switch = ctk.CTkSwitch(self.switches_frame, text="H-Mirror", command=lambda: self.send_cmd('HM', self.hm_switch.get()))
        self.hm_switch.grid(row=0, column=0, sticky="w")
        
        self.vf_switch = ctk.CTkSwitch(self.switches_frame, text="V-Flip", command=lambda: self.send_cmd('VF', self.vf_switch.get()))
        self.vf_switch.grid(row=0, column=1, sticky="w")

        # Virtual Cam Switch (for Google Meet / Zoom / Discord)
        self.vcam_switch = ctk.CTkSwitch(self.sidebar, text="Virtual Cam (Meet/Zoom)", command=self.toggle_vcam)
        self.vcam_switch.grid(row=9, column=0, padx=20, pady=(5, 10), sticky="w")

        # Info Box at the bottom
        self.info_box = ctk.CTkTextbox(self.sidebar, height=100, corner_radius=10)
        self.info_box.grid(row=10, column=0, padx=20, pady=20, sticky="ew")
        self.info_box.insert("0.0", "System Ready.\nWaiting for ESP32-S3 Stream...")
        self.info_box.configure(state="disabled")

        # ================= RIGHT PANEL (Video Feed) =================
        self.video_frame = ctk.CTkFrame(self, corner_radius=15, fg_color="#1E1E1E")
        self.video_frame.grid(row=0, column=1, padx=20, pady=20, sticky="nsew")
        self.video_frame.grid_rowconfigure(0, weight=1)
        self.video_frame.grid_columnconfigure(0, weight=1)

        self.video_label = ctk.CTkLabel(self.video_frame, text="NO SIGNAL", font=ctk.CTkFont(size=30, weight="bold"), text_color="gray")
        self.video_label.grid(row=0, column=0, sticky="nsew")

        # ================= START THREADS =================
        self.stream_thread = threading.Thread(target=self.receive_stream, daemon=True)
        self.stream_thread.start()
        
        # Request ESP32 to start streaming camera feed
        self.send_cmd('CAM', 1)

        self.update_video()

    def send_cmd(self, prefix, val):
        try:
            self.ser.write(f"SET:{prefix}:{val}\n".encode())
        except:
            pass

    def on_res_change(self, choice):
        val = self.resolutions[choice]
        self.send_cmd('RES', val)

    def on_q_change(self, val):
        val = int(val)
        self.q_label.configure(text=f"Quality (Lower = Better): {val}")
        self.send_cmd('Q', val)

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

        while self.running:
            try:
                chunk = self.ser.read(4096)
                if not chunk:
                    continue
                    
                buffer.extend(chunk)
                
                while True:
                    # Search for MAGIC_VID header (0x12345678)
                    vid_idx = buffer.find(MAGIC_VID)
                    
                    if vid_idx == -1:
                        # Keep last 3 bytes in case magic is split across chunks
                        if len(buffer) > 4:
                            buffer = buffer[-3:]
                        break
                    
                    # Discard any noise/data before magic header
                    if vid_idx > 0:
                        buffer = buffer[vid_idx:]
                        
                    if len(buffer) < 8:
                        break  # Wait for 4-byte magic + 4-byte size header
                        
                    size_bytes = buffer[4:8]
                    frame_size = struct.unpack('<I', size_bytes)[0]
                    
                    # Sanity check frame size (max 2MB)
                    if frame_size <= 0 or frame_size > 2 * 1024 * 1024:
                        buffer = buffer[4:]  # Corrupted header, skip magic
                        continue
                        
                    if len(buffer) < 8 + frame_size:
                        break  # Wait for complete JPEG frame data
                        
                    # Extract complete JPEG frame
                    frame_data = buffer[8 : 8 + frame_size]
                    buffer = buffer[8 + frame_size :]
                    
                    img_array = np.frombuffer(frame_data, dtype=np.uint8)
                    frame = cv2.imdecode(img_array, cv2.IMREAD_COLOR)
                    
                    if frame is not None:
                        # Convert BGR to RGB for PIL & VirtualCam
                        frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                        
                        with self.frame_lock:
                            self.current_frame = frame

                        # Send frame to Virtual Camera if active
                        if self.vcam_enabled and HAS_PYVIRTUALCAM:
                            h, w, _ = frame.shape
                            if self.vcam is None or self.vcam.width != w or self.vcam.height != h:
                                if self.vcam:
                                    try:
                                        self.vcam.close()
                                    except:
                                        pass
                                try:
                                    self.vcam = pyvirtualcam.Camera(width=w, height=h, fps=30, fmt=pyvirtualcam.PixelFormat.RGB)
                                    self.update_info(f"Virtual Cam Active:\n{self.vcam.device}")
                                except Exception as e:
                                    self.vcam_enabled = False
                                    self.vcam_switch.deselect()
                                    self.update_info(f"Virtual Cam Error:\n{e}")
                            if self.vcam:
                                try:
                                    self.vcam.send(frame)
                                except Exception as e:
                                    pass
                        
                        frame_count += 1
                        current_time = time.time()
                        if current_time - fps_time >= 1.0:
                            fps = frame_count / (current_time - fps_time)
                            kb_size = frame_size / 1024
                            vcam_str = f" | VCam: ON" if self.vcam_enabled else ""
                            info_str = f"FPS: {fps:.1f}\nSize: {kb_size:.1f} KB\nRes: {frame.shape[1]}x{frame.shape[0]}{vcam_str}"
                            self.update_info(info_str)
                            fps_time = current_time
                            frame_count = 0
            except Exception as e:
                print(f"[STREAM ERROR]: {e}")
                time.sleep(0.05)

    def update_info(self, text):
        def _update():
            self.info_box.configure(state="normal")
            self.info_box.delete("0.0", "end")
            self.info_box.insert("0.0", text)
            self.info_box.configure(state="disabled")
        self.after(0, _update)

    def update_video(self):
        with self.frame_lock:
            frame = self.current_frame
            
        if frame is not None:
            # Resize frame to fit the window while maintaining aspect ratio
            frame_height, frame_width, _ = frame.shape
            
            # Get current label size
            lbl_w = self.video_frame.winfo_width()
            lbl_h = self.video_frame.winfo_height()
            
            if lbl_w > 10 and lbl_h > 10:
                # Calculate scale
                scale_w = lbl_w / frame_width
                scale_h = lbl_h / frame_height
                scale = min(scale_w, scale_h)
                
                new_w = int(frame_width * scale)
                new_h = int(frame_height * scale)
                
                # Resize using PIL (CTKImage will handle this natively)
                img = Image.fromarray(frame)
                ctk_img = ctk.CTkImage(light_image=img, dark_image=img, size=(new_w, new_h))
                
                self.video_label.configure(image=ctk_img, text="")
        
        # Schedule next update (30fps = ~33ms)
        self.after(33, self.update_video)
        
    def on_closing(self):
        self.running = False
        self.send_cmd('CAM', 0)
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
    print("ESP32-S3 Vision Hub - Modern UI")
    print("-" * 30)
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

    print("Connected! Launching UI...")
    
    app = ModernCamUI(ser)
    app.protocol("WM_DELETE_WINDOW", app.on_closing)
    app.mainloop()

if __name__ == '__main__':
    main()
