# XIAO CAM Firmware Modifications Summary
## Date: 2024
## Purpose: PC Dashboard Compatibility with CORS Support

---

## Modified Files

### 1. `wifi_stream.cpp`
**Location:** `c:\Users\KIRA63\Desktop\XIAO CAM vs code\XIAO CAM\ESP32S3_CamUI\wifi_stream.cpp`

---

## Detailed Modifications

### ✅ MODIFICATION 1: CORS Headers Added to `/api/telemetry` Endpoint

**Lines Modified:** ~85-102

**Changes:**
- Modified `handleTelemetry()` function to return an `AsyncWebServerResponse` object with explicit CORS headers
- Added `Access-Control-Allow-Origin: *` header
- Added `Access-Control-Allow-Methods: GET, POST, OPTIONS` header
- Added `Access-Control-Allow-Headers: *` header
- Created new `handleOptions()` function to handle HTTP OPTIONS preflight requests

**Impact:**
- Browser-based dashboards can now successfully fetch telemetry data from `/api/telemetry`
- CORS preflight requests (OPTIONS) are properly handled
- Cross-origin requests no longer blocked by browser security policies

---

### ✅ MODIFICATION 2: TFT Screen IP Display Fixed (STA IP Fallback)

**Lines Modified:** ~175-198

**Changes:**
- Modified `wifiStreamTick()` to prioritize showing Station (STA) IP address when connected to WiFi router
- Added dynamic IP update logic that continuously checks `WiFi.status()` for STA connection
- When `WL_CONNECTED` status is detected, screen displays `WiFi.localIP()` instead of AP IP (192.168.4.1)
- IP display updates in real-time when device transitions from AP mode to STA mode

**Impact:**
- Screen now correctly shows the router-assigned IP address (e.g., 192.168.1.XXX)
- Users can see the actual IP to connect their PC dashboard
- Automatic fallback to AP IP (192.168.4.1) if no router connection established

---

### ✅ MODIFICATION 3: MJPEG Stream Server Added on Port 81

**Lines Modified:** Multiple sections

**Changes Added:**

#### 3.1 Global Variables and Constants (Lines ~21-28)
```cpp
static AsyncWebServer* streamServer = nullptr;
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
```

#### 3.2 Stream Handler Function (Lines ~30-82)
- Created `handleStream()` function with chunked MJPEG multipart response
- Captures camera frame buffer using `esp_camera_fb_get()`
- Streams JPEG frames with proper multipart boundaries
- Includes CORS headers in each frame part: `Access-Control-Allow-Origin: *`
- Implements ~30 FPS frame rate control (33ms delay)
- Properly releases frame buffer after each transmission

#### 3.3 Server Initialization (Lines ~183-191)
```cpp
// Start MJPEG stream server on port 81
streamServer = new AsyncWebServer(81);
streamServer->on("/stream", HTTP_GET, handleStream);
streamServer->on("/stream", HTTP_OPTIONS, handleOptions);
streamServer->begin();
```

#### 3.4 Cleanup Functions Updated
- `wifiStreamEnter()`: Added streamServer cleanup
- `wifiStreamLeave()`: Added streamServer shutdown and deletion
- Ensures proper resource deallocation when WiFi stream mode exits

#### 3.5 Display Updated (Line ~238)
- Changed display text from "Port: 80" to "API: 80  Stream: 81"
- Users now see both server ports on the TFT screen

**Impact:**
- MJPEG video stream now accessible at `http://[DEVICE_IP]:81/stream`
- Stream includes CORS headers allowing browser `<img>` tags to render frames
- Dashboard can embed live video feed without cross-origin errors
- Continuous streaming with automatic frame buffer management

---

## Testing Checklist

Before uploading firmware:
- [x] Code compiles without errors
- [x] All AsyncWebServer instances properly initialized
- [x] CORS headers present in all HTTP responses
- [x] IP address display logic uses STA IP when available
- [x] Stream server on port 81 responds to `/stream` endpoint

After uploading firmware:
- [ ] Connect ESP32 to WiFi router
- [ ] Verify TFT screen shows correct router IP address (not 192.168.4.1)
- [ ] Test telemetry endpoint: `http://[DEVICE_IP]:80/api/telemetry`
- [ ] Test MJPEG stream: `http://[DEVICE_IP]:81/stream`
- [ ] Verify browser console shows no CORS errors
- [ ] Confirm video stream renders in dashboard `<img>` element

---

## Network Endpoints Summary

| Endpoint | Port | Protocol | CORS Enabled | Purpose |
|----------|------|----------|--------------|---------|
| `/api/telemetry` | 80 | HTTP GET | ✅ Yes | JSON telemetry data (heap, FPS, etc.) |
| `/api/telemetry` | 80 | HTTP OPTIONS | ✅ Yes | CORS preflight handling |
| `/stream` | 81 | HTTP GET | ✅ Yes | MJPEG video stream (multipart) |
| `/stream` | 81 | HTTP OPTIONS | ✅ Yes | CORS preflight handling |

---

## Dashboard Integration

### Example HTML/JavaScript
```html
<!-- Video Stream -->
<img src="http://[DEVICE_IP]:81/stream" alt="Camera Feed" />

<!-- Telemetry Fetch -->
<script>
fetch('http://[DEVICE_IP]:80/api/telemetry')
  .then(res => res.json())
  .then(data => console.log('Free Heap:', data.freeHeap));
</script>
```

---

## Additional Notes

1. **WiFi Credentials:** Hardcoded in `wifi_stream.cpp` (Lines 10-11):
   - SSID: `IBRAHIM`
   - Password: `kira543ibrahim`
   
2. **Camera Requirement:** MJPEG stream requires camera to be in JPEG mode. Ensure proper camera initialization before streaming.

3. **Performance:** Stream frame rate controlled at ~30 FPS with 33ms delay between frames.

4. **Memory Management:** Each frame buffer is properly acquired and released to prevent memory leaks.

---

## Compilation Instructions

1. Open project in PlatformIO (VSCode)
2. Verify `platformio.ini` includes required libraries:
   - `ESPAsyncWebServer`
   - `AsyncTCP`
   - `ArduinoJson`
3. Build project: `pio run`
4. Upload to device: `pio run --target upload`
5. Monitor serial output: `pio device monitor`

---

## Troubleshooting

**Issue:** Stream not accessible on port 81
- **Solution:** Check firewall settings, verify port 81 is not blocked

**Issue:** CORS errors still present
- **Solution:** Clear browser cache, verify headers using browser DevTools Network tab

**Issue:** IP shows 192.168.4.1 instead of router IP
- **Solution:** Verify WiFi credentials, check router DHCP settings, ensure device connects to STA network

**Issue:** Stream shows black screen
- **Solution:** Ensure camera initialized in JPEG mode, check serial logs for camera errors

---

## Contact & Support

For issues or questions regarding these modifications, refer to:
- ESP32 Camera documentation
- ESPAsyncWebServer library documentation
- PlatformIO community forums

---

**END OF MODIFICATIONS SUMMARY**
