/* ═══════════════════════════════════════════════════════════════════════
wifi_stream.cpp  ·  KIRA-NET Camera Firmware Module (Optimized Non-Blocking)
═══════════════════════════════════════════════════════════════════════ */

#include <WiFi.h>
#include <WiFiServer.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "img_converters.h"

// --- Global Wi-Fi Stream State Variables ---
uint8_t wifiStreamPage = 0; 
uint8_t wifiStreamState = 0; 
char wifiStreamStatusMsg[64] = "READY";
char wifiStreamIP[24] = "---.---.---.---";

const char* WIFI_SSID = "IBRAHIM";
const char* WIFI_PASS = "kira543ibrahim";

static AsyncWebServer* wsServer = nullptr;      
static WiFiServer camServer(81);
static TaskHandle_t streamTaskHandle = NULL;  

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nAccess-Control-Allow-Origin: *\r\n\r\n";

// Global tracking for real FPS calculations
static uint32_t realFrameCount = 0;
static uint32_t lastFpsMillis = 0;
static uint32_t currentCalculatedFps = 0;

// Forward declarations
void wifiStreamLeave();

/* ── 1. CORS Pre-flight Options Handler ────────────────────────────── */
void handleOptions(AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "");
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "*");
    request->send(response);
}

/* ── 2. JSON Telemetry Endpoint Handler with CORS ───────────────────── */
void handleTelemetry(AsyncWebServerRequest *request) {
    // Calculate real dynamic FPS over 1-second intervals
    uint32_t currentMillis = millis();
    if (currentMillis - lastFpsMillis >= 1000) {
        currentCalculatedFps = (realFrameCount * 1000) / (currentMillis - lastFpsMillis);
        realFrameCount = 0;
        lastFpsMillis = currentMillis;
    }
    
    JsonDocument doc;
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["rssi"] = WiFi.RSSI();
    doc["uptime"] = currentMillis / 1000;
    doc["fps"] = currentCalculatedFps; // Pass verified dynamic FPS to JS
    
    String responseString;
    serializeJson(doc, responseString);
    
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", responseString);
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "*");
    request->send(response);
}

/* ── 3. Dedicated FreeRTOS Stream Task (Shared Buffer Access) ─────────── */
// External shared frame buffer from main camera task
extern volatile uint8_t* readyBuf;
extern volatile uint16_t readyW;
extern volatile uint16_t readyH;
extern volatile bool frameReady;
extern SemaphoreHandle_t frameMutex;

void streamTask(void *pvParameters) {
    camServer.begin();
    Serial.println("[STREAM] Port 81 Server Started (Native JPEG Mode)");
    
    while (true) {
        WiFiClient client = camServer.accept();
        
        if (client) {
            Serial.println("[STREAM] Client connected!");
            
            // Send HTTP headers
            client.println("HTTP/1.1 200 OK");
            client.println("Access-Control-Allow-Origin: *");
            client.println("Content-Type: multipart/x-mixed-replace; boundary=" PART_BOUNDARY);
            client.println();
            
            while (client.connected()) {
                camera_fb_t* fb = esp_camera_fb_get();
                if (!fb) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                
                if (fb->format != PIXFORMAT_JPEG) {
                    Serial.println("[STREAM] ERROR: Camera is not in JPEG mode!");
                    esp_camera_fb_return(fb);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    continue;
                }
                
                // Send multipart frame
                client.print(_STREAM_BOUNDARY);
                client.printf("Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
                client.write(fb->buf, fb->len);
                
                esp_camera_fb_return(fb);
                realFrameCount++;
                
                vTaskDelay(pdMS_TO_TICKS(33)); // ~30 FPS pacing
            }
            
            Serial.println("[STREAM] Client disconnected.");
            client.stop();
        }
        
        vTaskDelay(pdMS_TO_TICKS(100)); // Check for clients every 100ms
    }
}

/* ── 4. Old AsyncWebServer Stream Handler (DEPRECATED - NOT USED) ───── */
void handleStream(AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginChunkedResponse(
        _STREAM_CONTENT_TYPE, 
        [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
            // Enforce frame rate timing limit natively (~15 FPS max for 800x600)
            static uint32_t lastFrameT = 0;
            if (millis() - lastFrameT < 65) {
                vTaskDelay(pdMS_TO_TICKS(5));
                return 0; 
            }
            
            // Try to safely acquire the frame buffer. 
            // If the display task is holding it, this will safely return null instead of blocking.
            camera_fb_t * fb = esp_camera_fb_get();
            if (!fb) {
                // Fallback: If locked, yield to display task and try again next loop tick
                vTaskDelay(pdMS_TO_TICKS(2));
                return 0;
            }
            
            // Ensure the frame format is strictly JPEG
            if (fb->format != PIXFORMAT_JPEG) {
                esp_camera_fb_return(fb);
                vTaskDelay(pdMS_TO_TICKS(5));
                return 0;
            }
            
            size_t hLen = snprintf((char*)buffer, maxLen, _STREAM_PART, fb->len);
            if(hLen + fb->len + strlen(_STREAM_BOUNDARY) > maxLen) {
                esp_camera_fb_return(fb);
                return 0;
            }
            
            // Block transfer image matrix array
            memcpy(buffer + hLen, fb->buf, fb->len);
            size_t tLen = hLen + fb->len;
            memcpy(buffer + tLen, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
            size_t totalLen = tLen + strlen(_STREAM_BOUNDARY);
            
            // Return buffer immediately to free the hardware bus for the display task
            esp_camera_fb_return(fb);
            lastFrameT = millis();
            realFrameCount++; // Increment only on true transmission success
            
            return totalLen;
        }
    );
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
}

/* ── 4. Lifecycle Enter and Reset Routine ───────────────────────────── */
void wifiStreamInit() {
    wifiStreamState = 0; 
    snprintf(wifiStreamStatusMsg, sizeof(wifiStreamStatusMsg), "READY");
    snprintf(wifiStreamIP, sizeof(wifiStreamIP), "---.---.---.---");
}

void wifiStreamEnter() {
    wifiStreamInit();
}

/* ── 5. Select Action UI Trigger (Toggle Connect/Disconnect) ────────── */
void wifiStreamSelectAction() {
    if (wifiStreamState == 0 || wifiStreamState == 3) { 
        wifiStreamState = 1; 
        snprintf(wifiStreamStatusMsg, sizeof(wifiStreamStatusMsg), "CONNECTING...");
        
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("CAM_STREAM_AP", "12345678");
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        
    } else if (wifiStreamState == 2) { 
        wifiStreamLeave();
        snprintf(wifiStreamStatusMsg, sizeof(wifiStreamStatusMsg), "STOPPED");
    }
}

/* ── 6. Dynamic State Machine Tick and IP Update Loader ─────────────── */
void wifiStreamTick() {
    if (wifiStreamState == 1) { 
        if (WiFi.status() == WL_CONNECTED) {
            wifiStreamState = 2; 
            
            snprintf(wifiStreamIP, sizeof(wifiStreamIP), "%s", WiFi.localIP().toString().c_str());
            snprintf(wifiStreamStatusMsg, sizeof(wifiStreamStatusMsg), "STA CONNECTED");
            
            if (wsServer == nullptr) {
                wsServer = new AsyncWebServer(80);
                wsServer->on("/api/telemetry", HTTP_GET, handleTelemetry);
                wsServer->on("/api/telemetry", HTTP_OPTIONS, handleOptions);
                wsServer->begin();
            }
            
            // Spawn FreeRTOS stream task for Port 81
            if (streamTaskHandle == NULL) {
                Serial.println("[STREAM] Creating FreeRTOS Stream Task...");
                xTaskCreatePinnedToCore(streamTask, "StreamTask", 8192, NULL, 1, &streamTaskHandle, 1);
            }
            
        } else if (WiFi.softAPIP() != IPAddress(0,0,0,0) && strlen(wifiStreamIP) <= 15) {
            snprintf(wifiStreamIP, sizeof(wifiStreamIP), "%s", WiFi.softAPIP().toString().c_str());
            snprintf(wifiStreamStatusMsg, sizeof(wifiStreamStatusMsg), "AP ACTIVE");
        }
    }
    
    if (wifiStreamState == 2 && WiFi.status() != WL_CONNECTED) {
        wifiStreamState = 3; 
        snprintf(wifiStreamStatusMsg, sizeof(wifiStreamStatusMsg), "CONN LOST");
    }
}

/* ── 7. Safe Connection Leave and Resource Clean Cleanup ───────────── */
void wifiStreamLeave() {
    Serial.println("[STREAM] === Cleanup sequence initiated ===");
    
    // Kill Port 80 Telemetry Server
    if (wsServer) {
        Serial.println("[STREAM] Stopping Telemetry Server (Port 80)...");
        wsServer->end();
        delete wsServer;
        wsServer = nullptr;
        Serial.println("[STREAM] ✓ Telemetry Server stopped");
    }
    
    // Delete FreeRTOS Stream Task
    if (streamTaskHandle) {
        Serial.println("[STREAM] Deleting Stream Task...");
        vTaskDelete(streamTaskHandle);
        streamTaskHandle = NULL;
        Serial.println("[STREAM] ✓ Stream Task deleted");
    }
    
    // Stop WiFi Camera Server (Port 81)
    Serial.println("[STREAM] Stopping Camera Server (Port 81)...");
    camServer.end();
    Serial.println("[STREAM] ✓ Camera Server stopped");
    
    // Disconnect WiFi
    Serial.println("[STREAM] Disconnecting WiFi...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[STREAM] ✓ WiFi disconnected");
    
    // Reset state variables
    wifiStreamState = 0; 
    snprintf(wifiStreamIP, sizeof(wifiStreamIP), "---.---.---.---");
    
    Serial.printf("[STREAM] === Cleanup complete - Free Heap: %d KB ===\n", ESP.getFreeHeap() / 1024);
}

/* ── 8. UI Drawing Function for WiFi Stream Screen ─────────────────── */
#include "camera_config.h"
#include "ui_settings.h"

void drawWiFiStream(TFT_eSprite& spFeed, TFT_eSprite& spMenu) {
    static uint8_t lastState = 255;
    static uint32_t lastDebugTime = 0;
    
    // Only print debug when state actually changes or every 5 seconds
    if (lastState != wifiStreamState || (millis() - lastDebugTime > 5000)) {
        Serial.printf("[WIFI] State=%d, IP=%s, Status=%s, Heap=%dKB, RSSI=%ddBm\n",
                      wifiStreamState, wifiStreamIP, wifiStreamStatusMsg, 
                      ESP.getFreeHeap()/1024, WiFi.RSSI());
        lastState = wifiStreamState;
        lastDebugTime = millis();
    }
    
    // Draw Feed Area (top 172x172 section)
    spFeed.fillSprite(C_BG);
    spFeed.setTextColor(C_ACCENT, C_BG);
    spFeed.setTextSize(2);
    spFeed.drawString("WiFi Stream", DISP_W/2 - 55, FEED_H/2 - 30);
    
    // Display IP Address
    spFeed.setTextColor(C_WHITE, C_BG);
    spFeed.setTextSize(1);
    spFeed.setCursor((DISP_W - strlen(wifiStreamIP) * 6) / 2, FEED_H/2 + 10);
    spFeed.print(wifiStreamIP);
    
    spFeed.pushSprite(0, FEED_Y);
    
    // Draw Menu Area (bottom section)
    spMenu.fillSprite(C_BG);
    spMenu.fillRect(0, 0, DISP_W, 14, C_PANEL);
    spMenu.setTextColor(0x07FF, C_PANEL);
    spMenu.setTextSize(1);
    spMenu.setCursor(5, 3);
    spMenu.print("WiFi STREAM MODE");
    
    spMenu.setTextColor(C_WHITE, C_BG);
    spMenu.setTextSize(1);
    
    // Display Status based on state
    const char* stateLabels[] = {"IDLE", "CONNECTING", "RUNNING", "ERROR"};
    uint16_t stateColors[] = {C_GREY, C_ACCENT2, C_GREEN, C_RED};
    
    spMenu.setCursor(10, 24);
    spMenu.setTextColor(stateColors[wifiStreamState], C_BG);
    spMenu.print("Status: ");
    spMenu.print(stateLabels[wifiStreamState]);
    
    spMenu.setCursor(10, 40);
    spMenu.setTextColor(C_LTGREY, C_BG);
    spMenu.print(wifiStreamStatusMsg);
    
    // Display URLs when streaming
    if (wifiStreamState == 2) { // WS_RUNNING
        spMenu.setTextColor(C_ACCENT2, C_BG);
        spMenu.setCursor(10, 56);
        spMenu.print("Stream: :81/stream");
        
        spMenu.setCursor(10, 68);
        spMenu.print("Telemetry: :80/api/telemetry");
        
        // Blinking indicator
        bool blink = ((millis() / 400) & 1);
        spMenu.fillCircle(14, 86, 4, blink ? C_GREEN : C_DKGREY);
        spMenu.setTextColor(blink ? C_GREEN : C_GREY, C_BG);
        spMenu.setCursor(24, 82);
        spMenu.print("STREAMING");
        
        // Network info
        spMenu.setTextColor(C_GREY, C_BG);
        spMenu.setCursor(10, 100);
        spMenu.printf("RSSI: %d dBm", WiFi.RSSI());
        
        spMenu.setCursor(10, 112);
        spMenu.printf("Heap: %d KB", ESP.getFreeHeap() / 1024);
    } else if (wifiStreamState == 1) { // WS_STARTING
        // Connection progress
        static int dots = 0;
        dots = (millis() / 500) % 4;
        spMenu.setCursor(10, 56);
        spMenu.setTextColor(C_ACCENT2, C_BG);
        for (int i = 0; i < dots; i++) {
            spMenu.print(".");
        }
    } else if (wifiStreamState == 0 || wifiStreamState == 3) { // WS_IDLE or WS_ERROR
        spMenu.setTextColor(C_GREY, C_BG);
        spMenu.setCursor(10, 56);
        spMenu.print("Press OK to start");
    }
    
    // Bottom bar
    spMenu.fillRect(0, MENU_H - 14, DISP_W, 14, C_PANEL);
    spMenu.setTextColor(C_DKGREY, C_PANEL);
    spMenu.setTextSize(1);
    spMenu.setCursor(5, MENU_H - 11);
    spMenu.print(wifiStreamState == 2 ? "[OK]=Stop" : "[OK]=Start");
    spMenu.setCursor(DISP_W - 50, MENU_H - 11);
    spMenu.print("[BACK]");
    
    spMenu.pushSprite(0, MENU_Y);
}
