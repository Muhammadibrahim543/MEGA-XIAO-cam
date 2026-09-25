/*  ════════════════════════════════════════════════════════════════
 *  ESP32S3_CamUI.ino  —  EXTREME PERFORMANCE BUILD
 *  Seeed XIAO ESP32-S3 Sense  |  OV3660  |  ST7789 172×320
 *  ════════════════════════════════════════════════════════════════
 *  Core 0: Display + UI (non-blocking)
 *  Core 1: Camera capture + JPEG decode
 *  Audio: Built-in PDM mic (CLK=42, DATA=41) records alongside video
 *  ════════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "img_converters.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "camera_config.h"
#include "display.h"
#include "ui_settings.h"
#include "sd_recorder.h"
#include "sd_player.h"
#include "espnow_stream.h"
#include "audio.h"
#include "live_radio.h"
#include "wifi_stream.h"
#include "power_manager.h"

// ─── Display Rotation Configuration ──────────────────────────────
// Default 0: 180-degree flip (inverted from original 2) so upside-down mounted display is right-side up.
#define DEFAULT_TFT_ROTATION 0
static uint8_t currentRotation = DEFAULT_TFT_ROTATION;

// ─── Remote Virtual Buttons (from PC Dashboard) ───────────────────
static volatile bool remoteBtnUp     = false;
static volatile bool remoteBtnOk     = false;
static volatile bool remoteBtnDn     = false;
static volatile bool remoteBtnBack   = false;
static volatile bool remoteUpPulse   = false;
static volatile bool remoteOkPulse   = false;
static volatile bool remoteDnPulse   = false;
static volatile bool remoteBackPulse = false;

// ─── Buttons ──────────────────────────────────────────────────────
#define BTN_UP       1
#define BTN_OK       2
#define BTN_DN       43
#define DEBOUNCE_MS  40
#define LONGPRESS_MS 600
#define HOLD_REPEAT_DELAY    500
#define HOLD_REPEAT_INTERVAL 120

// ─── Display ──────────────────────────────────────────────────────
TFT_eSPI    tft;
TFT_eSprite spFeed(&tft);
TFT_eSprite spMenu(&tft);

// ─── App state ────────────────────────────────────────────────────
CamSettings   camCfg;
UIState       uiState   = {};
RecorderState recState  = {};
PlayerState   playState = {};
FileList      fileList  = {};
AudioState    audState  = {};

// ─── QR Code Reader state ─────────────────────────────────────────
#include <quirc.h>
static struct quirc* qrDecoder     = nullptr;
static char          qrResult[256] = {};
static bool          qrScanning    = false;
static bool          qrSaved       = false;

// ─── Ping-pong frame buffers (PSRAM) ──────────────────────────────
#define FB_MAX_SIZE (1600 * 1200 * 2)

static uint8_t*          pingBuf      = nullptr;
static uint8_t*          pongBuf      = nullptr;
static size_t            pingPongSize = 0;
volatile uint8_t* readyBuf    = nullptr;  // Shared with wifi_stream.cpp
volatile uint16_t readyW      = 0;        // Shared with wifi_stream.cpp
volatile uint16_t readyH      = 0;        // Shared with wifi_stream.cpp
volatile bool     frameReady  = false;    // Shared with wifi_stream.cpp
SemaphoreHandle_t frameMutex;             // Shared with wifi_stream.cpp

// ─── FPS counters ─────────────────────────────────────────────────
static volatile uint32_t captureFps  = 0;
static volatile uint32_t displayFps  = 0;
static uint32_t          dispFpsCount= 0;
static uint32_t          dispFpsTimer= 0;

// ─── Camera task handle ───────────────────────────────────────────
static TaskHandle_t camTaskHandle   = nullptr;
volatile bool       camPauseRequest = false;
volatile bool       camPausedAck    = false;

#define LIVE_RGB565_SWAP_BYTES true

// ─── Playback buffer ──────────────────────────────────────────────
static uint8_t* playBuf   = nullptr;
static size_t   playBufSz = 0;

// ─── Button state ─────────────────────────────────────────────────
struct Btn {
    bool     last;
    uint32_t downAt;
    bool     shortPress;
    bool     longFired;
    uint32_t repeatNextMs;
    bool     repeatFired;
};
static Btn bUp={}, bOk={}, bDn={};
static bool lrPttWasHeld = false;
static bool liveRadioCleanupPending = false;
static uint32_t liveRadioCleanupAtMs = 0;

// ─── Function Prototypes ──────────────────────────────────────────
static void pollBtn(Btn& b, uint8_t pin, bool remoteState = false);
static bool longPressed(Btn& b);
static bool holdRepeat(Btn& b);
static void clearFrameState();
static void pauseCameraTask();
static void resumeCameraTask();
static bool reinitPreviewCamera();
static void openQRScreen();
static void closeQRScreen();
static void qrSaveToSD(const char* data);
static void qrDoScan();
static void camTask(void* arg);
static void switchScreen(Screen to);
static void startRecording();
static void stopRecording();
static void openPlayback(const char* name);
static void closePlayback();
static void renderLiveFrame();
static void drawVfPanel();
static void openAudioScreen();
static void closeAudioScreen();
static void openLiveRadioScreen();
static void closeLiveRadioScreen();
static bool usbWebcamStreaming = false;
static bool usbWebcamAudioStreaming = false;
static void openUsbWebcamScreen();
static void closeUsbWebcamScreen();
static void handleInput();
void setup();
void loop();

static void pollBtn(Btn& b, uint8_t pin, bool remoteState, volatile bool& remotePulse) {
    b.shortPress = false;
    if (remotePulse) {
        b.shortPress = true;
        remotePulse = false;
    }
    bool raw = (digitalRead(pin) == LOW) || remoteState;
    if (raw && !b.last) {
        b.downAt       = millis();
        b.longFired    = false;
        b.repeatFired  = false;
        b.repeatNextMs = millis() + HOLD_REPEAT_DELAY;
    }
    if (!raw && b.last && !b.longFired && (millis()-b.downAt)>=DEBOUNCE_MS)
        b.shortPress = true;
    if (!raw) b.repeatFired = false;
    if (b.shortPress || b.repeatFired || (raw && !b.last)) power_manager_activity();
    b.last = raw;
}

static bool longPressed(Btn& b) {
    if (b.last && !b.longFired && (millis()-b.downAt)>=LONGPRESS_MS) {
        b.longFired = true;
        return true;
    }
    return false;
}

static bool holdRepeat(Btn& b) {
    if (!b.last || b.longFired) return false;
    if ((millis()-b.downAt) < HOLD_REPEAT_DELAY) return false;
    if (millis() >= b.repeatNextMs) {
        b.repeatNextMs = millis() + HOLD_REPEAT_INTERVAL;
        b.repeatFired  = true;
        return true;
    }
    return false;
}

// ─── Frame helpers ────────────────────────────────────────────────
static volatile bool writeBufResetNeeded = false;

static void clearFrameState() {
    xSemaphoreTake(frameMutex, portMAX_DELAY);
    frameReady = false;
    readyBuf   = pingBuf;
    readyW     = 0;
    readyH     = 0;
    writeBufResetNeeded = true;
    xSemaphoreGive(frameMutex);
}

static void pauseCameraTask() {
    if (!camTaskHandle) return;
    camPauseRequest = true;
    while (!camPausedAck) vTaskDelay(pdMS_TO_TICKS(1));
    clearFrameState();
}

static void resumeCameraTask() {
    camPauseRequest = false;
    while (camPausedAck) vTaskDelay(pdMS_TO_TICKS(1));
}

static bool reinitPreviewCamera() {
    pauseCameraTask();
    recorder_mic_deinit();
    esp_camera_deinit();
    bool ok = camera_init_rgb565(camCfg);
    if (ok) {
        uint16_t fw = FRAME_OPTIONS[camCfg.frameIdx].w;
        uint16_t fh = FRAME_OPTIONS[camCfg.frameIdx].h;
        size_t needed = (size_t)fw * fh * 2;
        if (needed > pingPongSize) {
            if (pingBuf) heap_caps_free(pingBuf);
            if (pongBuf) heap_caps_free(pongBuf);
            pingBuf = pongBuf = nullptr;
            size_t avail  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            size_t perBuf = (avail > 1024*1024) ? (avail-512*1024)/2 : needed;
            if (perBuf < needed) perBuf = needed;
            if (perBuf > FB_MAX_SIZE) perBuf = FB_MAX_SIZE;
            pingBuf = (uint8_t*)heap_caps_malloc(perBuf, MALLOC_CAP_SPIRAM);
            pongBuf = (uint8_t*)heap_caps_malloc(perBuf, MALLOC_CAP_SPIRAM);
            if (!pingBuf || !pongBuf) { ok = false; }
            else pingPongSize = perBuf;
        }
        clearFrameState();
    }
    // recorder_mic_init(); // Disabled to prevent mic usage during viewfinder
    resumeCameraTask();
    return ok;
}

// ─── QR Code Reader ───────────────────────────────────────────────
static void openQRScreen() {
    if (!qrDecoder) {
        qrDecoder = quirc_new();
        Serial.printf("[QR] quirc_new() = %p\n", qrDecoder);
    }
    qrResult[0] = '\0';
    qrScanning  = true;
    qrSaved     = false;
    Serial.println("[QR] Screen opened, scanning auto-started");
    switchScreen(SCR_QR_READER);
}

static void closeQRScreen() {
    qrScanning  = false;
    qrResult[0] = '\0';
    qrSaved     = false;
    switchScreen(SCR_MAIN_MENU);
}

static void qrSaveToSD(const char* data) {
    if (!uiState.sdReady || !data || data[0] == '\0') return;
    if (!SD.exists("/QR")) SD.mkdir("/QR");
    char path[40];
    for (int i = 0; i < 9999; i++) {
        snprintf(path, sizeof(path), "/QR/QR_%04d.txt", i);
        if (!SD.exists(path)) break;
    }
    FsFile f = SD.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (f) {
        f.println(data);
        f.close();
        Serial.printf("[QR] Saved: %s\n", path);
    }
}

static void qrDoScan() {
    if (!qrDecoder) {
        Serial.println("[QR] ERROR: qrDecoder NULL");
        return;
    }

    xSemaphoreTake(frameMutex, portMAX_DELAY);
    int grayW = (int)readyW;
    int grayH = (int)readyH;
    
    if (grayW == 0 || grayH == 0) {
        xSemaphoreGive(frameMutex);
        Serial.println("[QR] ERROR: invalid frame dims");
        return;
    }
    
    Serial.printf("[QR] Scan: %dx%d frame\n", grayW, grayH);

    int curW = 0, curH = 0;
    quirc_begin(qrDecoder, &curW, &curH);
    if (curW != grayW || curH != grayH) {
        Serial.printf("[QR] quirc_resize(%d,%d)...\n", grayW, grayH);
        if (quirc_resize(qrDecoder, grayW, grayH) != 0) {
            xSemaphoreGive(frameMutex);
            Serial.println("[QR] ERROR: quirc_resize failed");
            return;
        }
    }

    uint8_t* dst = quirc_begin(qrDecoder, nullptr, nullptr);
    if (!dst) {
        xSemaphoreGive(frameMutex);
        Serial.println("[QR] ERROR: quirc_begin returned NULL");
        return;
    }

    // RGB565 → grayscale
    const uint8_t* src = (const uint8_t*)readyBuf;
    for (int p = 0; p < grayW * grayH; p++) {
        uint16_t px = ((uint16_t)src[p*2] << 8) | src[p*2+1];
        uint8_t r = ((px >> 11) & 0x1F) << 3;
        uint8_t g = ((px >>  5) & 0x3F) << 2;
        uint8_t b = ( px        & 0x1F) << 3;
        dst[p] = (uint8_t)(((uint16_t)r * 77 + (uint16_t)g * 150 + (uint16_t)b * 29) >> 8);
    }
    frameReady = false;  // Consume the frame
    xSemaphoreGive(frameMutex);

    // Pause camera to avoid PSRAM bus contention during heavy quirc processing
    pauseCameraTask();

    // Debug stack
    int dummyLocalVar = 0;
    Serial.printf("[QR] Stack pointer address: %p, Free stack (words): %d\n", 
                  &dummyLocalVar, uxTaskGetStackHighWaterMark(NULL));

    quirc_end(qrDecoder);
    Serial.println("[QR] quirc_end() done");

    int count = quirc_count(qrDecoder);
    Serial.printf("[QR] quirc_count = %d, Free stack after end: %d\n", 
                  count, uxTaskGetStackHighWaterMark(NULL));

    if (count > 0) {
        struct quirc_code* code = (struct quirc_code*)heap_caps_malloc(sizeof(struct quirc_code), MALLOC_CAP_SPIRAM);
        struct quirc_data* data = (struct quirc_data*)heap_caps_malloc(sizeof(struct quirc_data), MALLOC_CAP_SPIRAM);
        
        if (code && data) {
            quirc_extract(qrDecoder, 0, code);
            int err = quirc_decode(code, data);
            Serial.printf("[QR] quirc_decode returned %d\n", err);
            
            if (err == QUIRC_SUCCESS) {
                size_t dlen = data->payload_len < 255 ? data->payload_len : 255;
                memcpy(qrResult, data->payload, dlen);
                qrResult[dlen] = '\0';
                qrScanning = false;
                qrSaved    = false;
                uiState.dirtyMenu = true;
                Serial.printf("[QR] SUCCESS! Decoded: %s\n", qrResult);
            } else {
                Serial.printf("[QR] Decode error: %d\n", err);
            }
        } else {
            Serial.println("[QR] ERROR: code/data struct alloc failed");
        }
        
        if (code) heap_caps_free(code);
        if (data) heap_caps_free(data);
    }

    // Resume camera after scan
    resumeCameraTask();

    // Brief yield to let system tasks breathe
    vTaskDelay(pdMS_TO_TICKS(1));
}

// ─── Camera task — Core 1 ─────────────────────────────────────────
#define CAM_TARGET_FPS     30
#define CAM_FRAME_MS       (1000UL / CAM_TARGET_FPS)

static void camTask(void* arg) {
    uint8_t* writeBuf = pingBuf;

    uint32_t fpsTimer       = millis();
    uint32_t fpsCount       = 0;
    uint32_t lastRecFrameMs = 0;
    uint32_t frameStartMs   = 0;

    for (;;) {
        frameStartMs = millis();

        if (camPauseRequest) {
            camPausedAck = true;
            while (camPauseRequest) vTaskDelay(pdMS_TO_TICKS(1));
            camPausedAck = false;
            writeBuf = pingBuf;
        }

        if (writeBufResetNeeded) {
            xSemaphoreTake(frameMutex, portMAX_DELAY);
            writeBufResetNeeded = false;
            xSemaphoreGive(frameMutex);
            writeBuf = pingBuf;
        }

        // Camera not needed during playback or audio recording
        if (uiState.screen == SCR_PLAYBACK || uiState.screen == SCR_AUDIO) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(1); continue; }

        if (uiState.screen == SCR_USB_WEBCAM) {
            // Audio streaming block - drain the software RingBuffer
            if (usbWebcamAudioStreaming) {
                for (int a = 0; a < 10; a++) {
                    int16_t micBuf[1024]; // 2048 bytes chunk
                    size_t bytesRead = audio_read_mic(micBuf, 1024);
                    if (bytesRead == 0) break;
                    
                    uint32_t header[2] = {0x87654321, (uint32_t)bytesRead};
                    Serial.write((uint8_t*)header, 8);
                    Serial.write((uint8_t*)micBuf, bytesRead);
                    
                    // Break as soon as we caught up to the live stream
                    if (bytesRead < sizeof(micBuf)) break;
                }
            }
            if (usbWebcamStreaming && fb->format == PIXFORMAT_JPEG) {
                const uint32_t MAGIC = 0x12345678;
                uint32_t len = fb->len;
                Serial.write((uint8_t*)&MAGIC, 4);
                Serial.write((uint8_t*)&len, 4);
                Serial.write(fb->buf, fb->len);
            }
            esp_camera_fb_return(fb);
            
            fpsCount++;
            uint32_t now = millis();
            if (now - fpsTimer >= 1000) {
                captureFps = fpsCount;
                fpsCount   = 0;
                fpsTimer   = now;
            }
            if (usbWebcamStreaming) {
                vTaskDelay(1);
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            continue;
        }

        if (uiState.recording) {
            uint32_t nowMs = millis();
            const uint32_t recIntervalMs = 1000UL / REC_FPS;
            if (fb->format == PIXFORMAT_JPEG &&
                (lastRecFrameMs == 0 || nowMs - lastRecFrameMs >= recIntervalMs)) {
                recorder_add_frame(recState, fb->buf, fb->len);
                lastRecFrameMs = nowMs;
            }
            esp_camera_fb_return(fb);

        } else if (fb->format == PIXFORMAT_RGB565) {
            lastRecFrameMs = 0;
            uint16_t fw = fb->width, fh = fb->height;
            size_t frameBytes = (size_t)fw * fh * 2;
            if (frameBytes > pingPongSize) {
                esp_camera_fb_return(fb);
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            memcpy(writeBuf, fb->buf, frameBytes);
            esp_camera_fb_return(fb);
            xSemaphoreTake(frameMutex, portMAX_DELAY);
            readyBuf = writeBuf;
            writeBuf = (writeBuf == pingBuf) ? pongBuf : pingBuf;
            readyW = fw; readyH = fh; frameReady = true;
            xSemaphoreGive(frameMutex);

        } else if (fb->format == PIXFORMAT_JPEG) {
            lastRecFrameMs = 0;
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        } else {
            esp_camera_fb_return(fb);
        }

        fpsCount++;
        uint32_t now = millis();
        if (now - fpsTimer >= 1000) {
            captureFps = fpsCount;
            fpsCount   = 0;
            fpsTimer   = now;
        }

        uint32_t elapsed = millis() - frameStartMs;
        if (elapsed < CAM_FRAME_MS) {
            vTaskDelay(pdMS_TO_TICKS(CAM_FRAME_MS - elapsed));
        } else {
            vTaskDelay(1);
        }
    }
}

// ─── Screen helpers ───────────────────────────────────────────────
static void switchScreen(Screen to) {
    if (to == SCR_FILES) {
        player_scan_files(fileList);
        uiState.fileCursor = 0;
        uiState.fileTop    = 0;
    }
    uiState.screen    = to;
    uiState.editMode  = false;
    uiState.dirtyMenu = true;
    uiState.dirtyFeed = true;
    tft.fillRect(0, DIV_Y, DISP_W, DIV_H, C_DIVIDER);
}


// ─── Video recording ──────────────────────────────────────────────
static void startRecording() {
    if (recState.active || !uiState.sdReady) return;
    pauseCameraTask();
    recorder_mic_deinit();
    esp_camera_deinit();
    CamSettings recCfg = camCfg;
    recCfg.frameIdx = camCfg.recFrameIdx;
    if (!camera_init(recCfg)) {
        camera_init_rgb565(camCfg);
        resumeCameraTask();
        return;
    }
    // Init mic AFTER camera init — ESP_I2S handles GDMA ordering correctly
    bool micOk = recorder_mic_init();
    if (!micOk) {
        Serial.println("[REC] Mic init failed — recording video only");
    }

    uint16_t w = FRAME_OPTIONS[camCfg.recFrameIdx].w;
    uint16_t h = FRAME_OPTIONS[camCfg.recFrameIdx].h;
    if (!recorder_start(recState, w, h, micOk)) {
        recorder_mic_deinit();
        esp_camera_deinit();
        camera_init_rgb565(camCfg);
        resumeCameraTask();
        return;
    }
    uiState.recording  = true;
    uiState.recStartMs = millis();
    uiState.dirtyMenu  = true;
    resumeCameraTask();
}

static void stopRecording() {
    if (!recState.active) return;
    pauseCameraTask();
    recorder_stop(recState);
    uiState.recording = false;
    uiState.dirtyMenu = true;
    recorder_mic_deinit();
    esp_camera_deinit();
    camera_init_rgb565(camCfg);
    resumeCameraTask();
}

// ─── Video playback ───────────────────────────────────────────────
static void openPlayback(const char* name) {
    switchScreen(SCR_PLAYBACK);
    delay(50);
    pauseCameraTask();
    recorder_mic_deinit();
    esp_camera_deinit();
    char path[40];
    snprintf(path, sizeof(path), "/%s", name);
    if (!player_open(playState, path)) {
        camera_init_rgb565(camCfg);
        // recorder_mic_init(); // Disabled to keep mic off
        resumeCameraTask();
        switchScreen(SCR_FILES);
        return;
    }
    size_t needed = (size_t)playState.decodedW * playState.decodedH * 2;
    if (needed > playBufSz) {
        if (playBuf) heap_caps_free(playBuf);
        playBuf   = (uint8_t*)heap_caps_malloc(needed, MALLOC_CAP_SPIRAM);
        playBufSz = playBuf ? needed : 0;
    }
    uiState.playPaused = false;
    uiState.dirtyMenu  = true;
}

static void closePlayback() {
    player_close(playState);
    audio_spk_deinit();
    esp_camera_deinit();
    camera_init_rgb565(camCfg);
    // recorder_mic_init(); // Disabled after playback close
    resumeCameraTask();
    switchScreen(SCR_VIEWFINDER);
}

// ─── Shared live-feed render ──────────────────────────────────────
static void renderLiveFrame() {
    if (!frameReady) return;
    xSemaphoreTake(frameMutex, portMAX_DELAY);
    uint8_t* buf = (uint8_t*)readyBuf;
    uint16_t fw  = readyW, fh = readyH;
    frameReady   = false;
    xSemaphoreGive(frameMutex);
    spFeed.fillSprite(TFT_BLACK);
    display_ensure(0, 0);
    display_scale_to_sprite(spFeed, buf, fw, fh, LIVE_RGB565_SWAP_BYTES);

    // Sleek translucent top HUD bar (13px)
    spFeed.fillRect(0, 0, DISP_W, 13, 0x10A2);

    // FPS and CPU Frequency on top-left
    char fbuf[32];
    uint32_t activeFps = (displayFps > 0) ? displayFps : captureFps;
    uint32_t cpuMhz = power_manager_get_cpu_freq();
    snprintf(fbuf, sizeof(fbuf), "%lu FPS | %luM", (unsigned long)activeFps, (unsigned long)cpuMhz);
    spFeed.setTextColor(C_WHITE, 0x10A2);
    spFeed.setTextSize(1);
    spFeed.drawString(fbuf, 4, 3);

    // Temperature and Power Profile on top-right
    float coreT = temperatureRead();
    const PowerStats& pwr = power_manager_get_stats();
    char tbuf[24];
    snprintf(tbuf, sizeof(tbuf), "%.0fC %s", coreT, pwr.profileName);
    uint16_t tc = (coreT < 55.0f) ? C_ACCENT2 : (coreT < 70.0f) ? C_ORANGE : C_RED;
    spFeed.setTextColor(tc, 0x10A2);
    spFeed.drawString(tbuf, DISP_W - 54, 3);

    if (uiState.recording) {
        bool blink = ((millis()/400)&1);
        spFeed.fillCircle(8, FEED_H-10, 4, blink ? C_RED : C_DKGREY);
        uint32_t secs = (millis()-uiState.recStartMs)/1000;
        snprintf(fbuf, sizeof(fbuf), "%02lu:%02lu", secs/60, secs%60);
        spFeed.setTextColor(C_RED, TFT_BLACK);
        spFeed.drawString(fbuf, 18, FEED_H-14);
    }
    spFeed.pushSprite(0, FEED_Y);
    dispFpsCount++;
}

static void drawVfPanel() {
    spMenu.fillSprite(C_BG);
    spMenu.fillRect(0,0,DISP_W,14,C_PANEL);
    spMenu.setTextColor(C_ACCENT2,C_PANEL);
    spMenu.setTextSize(1);
    spMenu.setCursor(5,3);
    spMenu.print(uiState.recording ? "REC  OK:stop  HOLD:menu" : "OK:record  HOLD:menu");
    spMenu.setTextColor(C_GREY,C_BG); spMenu.setCursor(5,20);
    char buf[48];
    snprintf(buf,sizeof(buf),"Live:%s  Rec:%s",
             FRAME_OPTIONS[camCfg.frameIdx].label,
             FRAME_OPTIONS[camCfg.recFrameIdx].label);
    spMenu.print(buf);
    spMenu.setTextColor(C_ACCENT,C_BG); spMenu.setCursor(5,34);
    const PowerStats& pwr = power_manager_get_stats();
    uint32_t activeFps = (displayFps > 0) ? displayFps : captureFps;
    snprintf(buf,sizeof(buf),"%lu fps  %luMHz (%s)  %.0fC",
             (unsigned long)activeFps, (unsigned long)pwr.currentFreqMhz, pwr.profileName, pwr.coreTempC);
    spMenu.print(buf);
    spMenu.pushSprite(0, MENU_Y);
}


// ─── Audio screen ─────────────────────────────────────────────────
static void openAudioScreen() {
    // 1. Set screen flag first — camTask will stop calling fb_get
    uiState.screen = SCR_AUDIO;
    delay(40);

    // 2. Suspend camera task safely
    pauseCameraTask();

    // 3. Tear down camera hardware
    recorder_mic_deinit();
    esp_camera_deinit();
    delay(150);

    // NOTE: Legacy GDMA dummy-install workaround removed.
    //       ESP_I2S handles GDMA channel allocation correctly
    //       without needing to manually flush I2S_NUM_0.

    // 4. Clear feed area
    spFeed.fillSprite(C_BG);
    spFeed.setTextColor(C_ORANGE, C_BG);
    spFeed.setTextSize(2);
    spFeed.drawString("AUDIO MODE", 10, FEED_H / 2 - 8);
    spFeed.setTextSize(1);
    spFeed.setTextColor(C_GREY, C_BG);
    spFeed.drawString("Camera off", 36, FEED_H / 2 + 14);
    spFeed.pushSprite(0, FEED_Y);

    // 5. Init mic — ESP_I2S acquires GDMA cleanly after camera deinit
    delay(200);
    if (!audio_mic_init()) {
        Serial.println("[AUDIO] Mic init FAILED");
    }

    uiState.dirtyMenu = true;
    ui_draw_audio_idle(spMenu);
}

static void closeAudioScreen() {
    if (audState.active) audio_stop(audState);
    audio_mic_deinit();
    delay(50);

    // Restore camera
    esp_camera_deinit();
    camera_init_rgb565(camCfg);
    clearFrameState();

    // Resume camera task safely
    resumeCameraTask();

    switchScreen(SCR_MAIN_MENU);
}

// ─── Live radio screen ────────────────────────────────────────────
static void openLiveRadioScreen() {
    if (liveRadioCleanupPending) {
        live_radio_shutdown_stack();
        liveRadioCleanupPending = false;
    }

    uiState.screen = SCR_LIVE_RADIO;
    delay(40);

    pauseCameraTask();

    // Live radio owns ESP-NOW while active, so tear down the video streamer.
    espnow_stream_stop(false);
    espnow_stream_deinit(false);
    uiState.streamActive    = false;
    uiState.streamConnected = false;

    audio_mic_deinit();
    recorder_mic_deinit();
    esp_camera_deinit();
    delay(120);

    if (!live_radio_enter()) {
        esp_camera_deinit();
        camera_init_rgb565(camCfg);
        clearFrameState();
        if (camTaskHandle) vTaskResume(camTaskHandle);
        switchScreen(SCR_MAIN_MENU);
        return;
    }

    lrPttWasHeld    = false;
    uiState.dirtyFeed = true;
    uiState.dirtyMenu = true;
}

static void closeLiveRadioScreen() {
    live_radio_stop_tx();
    live_radio_leave();
    lrPttWasHeld = false;
    delay(50);

    esp_camera_deinit();
    camera_init_rgb565(camCfg);
    clearFrameState();

    resumeCameraTask();

    switchScreen(SCR_MAIN_MENU);
    liveRadioCleanupPending = true;
    liveRadioCleanupAtMs    = millis() + 250;
}

// ─── ESP-NOW Screen ───────────────────────────────────────────────
static void openEspNowScreen() {
    if (uiState.screen == SCR_USB_WEBCAM || usbWebcamStreaming) {
        usbWebcamStreaming = false;
        closeUsbWebcamScreen();
    }
    uiState.screen = SCR_ESPNOW;
    delay(20);

    espnow_stream_init();
    audio_mic_init();

    uiState.dirtyMenu = true;
    uiState.dirtyFeed = true;
    switchScreen(SCR_ESPNOW);
}

// ─── USB WebCam ───────────────────────────────────────────────────
static void openUsbWebcamScreen() {
    usbWebcamStreaming = false;
    uiState.screen = SCR_USB_WEBCAM;
    delay(40);
    
    pauseCameraTask();
    
    espnow_stream_stop(false);
    espnow_stream_deinit(false);
    uiState.streamActive    = false;
    uiState.streamConnected = false;

    audio_mic_deinit();
    recorder_mic_deinit();
    esp_camera_deinit();
    delay(120);
    
    CamSettings wcCfg = camCfg;
    wcCfg.frameIdx = camCfg.wcFrameIdx;
    if (!camera_init(wcCfg)) {
        camera_init_rgb565(camCfg);
        clearFrameState();
        resumeCameraTask();
        switchScreen(SCR_MAIN_MENU);
        return;
    }
    
    clearFrameState();
    resumeCameraTask();
    uiState.dirtyMenu = true;
    uiState.dirtyFeed = true;
}

static void closeUsbWebcamScreen() {
    usbWebcamStreaming = false;
    delay(50);
    
    pauseCameraTask();
    
    if (usbWebcamAudioStreaming) {
        usbWebcamAudioStreaming = false;
        audio_mic_deinit();
    }
    
    // Suspend camera and restart normal mode
    esp_camera_deinit();
    camera_init_rgb565(camCfg);
    clearFrameState();

    resumeCameraTask();

    switchScreen(SCR_MAIN_MENU);
}

static void openWiFiStreamScreen() {
    uiState.screen = SCR_WIFI_STREAM;
    delay(40);
    
    pauseCameraTask();
    
    espnow_stream_stop(false);
    espnow_stream_deinit(false);
    uiState.streamActive    = false;
    uiState.streamConnected = false;

    audio_mic_deinit();
    recorder_mic_deinit();
    esp_camera_deinit();
    delay(120);
    
    // IMPORTANT: Initialize camera in Native JPEG mode for stream!
    // Keep camTask PAUSED to isolate hardware and prevent stack overflow
    if (!camera_init(camCfg)) {
        Serial.println("[WIFI STREAM] Camera Init Failed!");
    }
    
    wifiStreamEnter();
    wifiStreamSelectAction();
}

static void closeWiFiStreamScreen() {
    wifiStreamLeave();
    delay(50);
    
    esp_camera_deinit();
    camera_init_rgb565(camCfg);
    clearFrameState();

    resumeCameraTask();

    switchScreen(SCR_MAIN_MENU);
}

static const char* getScreenName(Screen scr) {
    switch (scr) {
        case SCR_MAIN_MENU:   return "MAIN_MENU";
        case SCR_VIEWFINDER:  return "VIEWFINDER";
        case SCR_SETTINGS:    return "SETTINGS";
        case SCR_FILES:       return "FILES";
        case SCR_PLAYBACK:    return "PLAYBACK";
        case SCR_ESPNOW:      return "ESPNOW";
        case SCR_AUDIO:       return "AUDIO_REC";
        case SCR_LIVE_RADIO:  return "LIVE_RADIO";
        case SCR_QR_READER:   return "QR_READER";
        case SCR_USB_WEBCAM:  return "USB_WEBCAM";
        case SCR_WIFI_STREAM: return "WIFI_STREAM";
        default:              return "UNKNOWN";
    }
}

static void handleBackAction() {
    if (uiState.recording) { stopRecording(); return; }
    switch (uiState.screen) {
        case SCR_PLAYBACK:
            closePlayback();
            return;
        case SCR_ESPNOW:
            espnow_stream_stop();
            audio_mic_deinit();
            uiState.streamActive = false;
            switchScreen(SCR_MAIN_MENU);
            return;
        case SCR_AUDIO:
            closeAudioScreen();
            return;
        case SCR_LIVE_RADIO:
            closeLiveRadioScreen();
            return;
        case SCR_USB_WEBCAM:
            closeUsbWebcamScreen();
            return;
        case SCR_WIFI_STREAM:
            closeWiFiStreamScreen();
            return;
        case SCR_QR_READER:
            closeQRScreen();
            return;
        case SCR_VIEWFINDER:
            switchScreen(SCR_MAIN_MENU);
            return;
        case SCR_MAIN_MENU:
            switchScreen(SCR_VIEWFINDER);
            return;
        default:
            if (uiState.screen == SCR_SETTINGS) ui_eeprom_save(camCfg);
            switchScreen(SCR_MAIN_MENU);
            return;
    }
}

static void handleFsLs(const char* path) {
    if (!path || path[0] == '\0') path = "/";
    if (!uiState.sdReady) {
        Serial.println("{\"type\":\"FS_LS_RES\",\"error\":\"SD_NOT_READY\"}");
        Serial.flush();
        return;
    }
    FsFile dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        Serial.println("{\"type\":\"FS_LS_RES\",\"error\":\"DIR_NOT_FOUND\"}");
        Serial.flush();
        return;
    }
    Serial.print("{\"type\":\"FS_LS_RES\",\"path\":\"");
    Serial.print(path);
    Serial.print("\",\"files\":[");
    bool first = true;
    while (true) {
        FsFile f = dir.openNextFile();
        if (!f) break;
        if (!first) Serial.print(",");
        first = false;
        char name[64];
        f.getName(name, sizeof(name));
        bool isDir = f.isDirectory();
        uint32_t size = f.size();
        Serial.print("{\"name\":\"");
        Serial.print(name);
        Serial.print("\",\"isDir\":");
        Serial.print(isDir ? "true" : "false");
        Serial.print(",\"size\":");
        Serial.print(size);
        Serial.print("}");
        f.close();
    }
    dir.close();
    Serial.println("]}");
    Serial.flush();
}


static void handleFsRead(const char* path, uint32_t offset, uint32_t size) {
    if (!uiState.sdReady) {
        Serial.println("{\"type\":\"FS_READ_RES\",\"error\":\"SD_NOT_READY\"}");
        return;
    }
    FsFile f = SD.open(path, O_READ);
    if (!f || f.isDirectory()) {
        Serial.println("{\"type\":\"FS_READ_RES\",\"error\":\"FILE_NOT_FOUND\"}");
        return;
    }
    if (!f.seek(offset)) {
        f.close();
        Serial.println("{\"type\":\"FS_READ_RES\",\"error\":\"SEEK_ERR\"}");
        return;
    }
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) {
        f.close();
        Serial.println("{\"type\":\"FS_READ_RES\",\"error\":\"OOM\"}");
        return;
    }
    size_t n = f.read(buf, size);
    f.close();
    if (n == 0) {
        free(buf);
        Serial.println("{\"type\":\"FS_READ_RES\",\"error\":\"EOF\"}");
        return;
    }
    size_t olen = 0;
    mbedtls_base64_encode(NULL, 0, &olen, buf, n);
    char* b64 = (char*)malloc(olen + 1);
    if (!b64) {
        free(buf);
        Serial.println("{\"type\":\"FS_READ_RES\",\"error\":\"OOM_B64\"}");
        return;
    }
    mbedtls_base64_encode((unsigned char*)b64, olen, &olen, buf, n);
    b64[olen] = 0;
    free(buf);
    Serial.print("{\"type\":\"FS_READ_RES\",\"path\":\"");
    Serial.print(path);
    Serial.print("\",\"offset\":");
    Serial.print(offset);
    Serial.print(",\"size\":");
    Serial.print(n);
    Serial.print(",\"data\":\"");
    Serial.print(b64);
    Serial.println("\"}");
    free(b64);
}

static void processSerialCommands() {
    static String serialLine = "";
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            serialLine.trim();
            if (serialLine.length() > 0) {
                // SET commands (from pc_webcam.py or dashboard)
                if (serialLine.startsWith("SET:")) {
                    if (serialLine.startsWith("SET:CAM:")) {
                        int enable = serialLine.substring(8).toInt();
                        if (enable != 0) {
                            if (uiState.screen != SCR_USB_WEBCAM) {
                                openUsbWebcamScreen();
                            }
                            usbWebcamStreaming = true;
                        } else {
                            usbWebcamStreaming = false;
                            if (uiState.screen == SCR_USB_WEBCAM) {
                                closeUsbWebcamScreen();
                            }
                        }
                        uiState.dirtyMenu = true;
                    }
                    else if (serialLine.startsWith("SET:MIC:")) {
                        bool enable = (serialLine.substring(8).toInt() != 0);
                        if (enable) {
                            if (!usbWebcamAudioStreaming) {
                                audio_mic_init();
                                usbWebcamAudioStreaming = true;
                            }
                        } else {
                            if (usbWebcamAudioStreaming) {
                                usbWebcamAudioStreaming = false;
                                audio_mic_deinit();
                            }
                        }
                        uiState.dirtyMenu = true;
                    }
                    else if (serialLine.startsWith("SET:ROT:")) {
                        uint8_t rot = (uint8_t)serialLine.substring(8).toInt();
                        if (rot == 0 || rot == 2) {
                            currentRotation = rot;
                            tft.setRotation(currentRotation);
                            uiState.dirtyMenu = true;
                            uiState.dirtyFeed = true;
                            Serial.printf("{\"type\":\"ROT_CHANGE\",\"val\":%d}\n", currentRotation);
                        }
                    }
                    else {
                        sensor_t *s = esp_camera_sensor_get();
                        if (s) {
                            if (serialLine.startsWith("SET:B:")) s->set_brightness(s, serialLine.substring(6).toInt());
                            else if (serialLine.startsWith("SET:C:")) s->set_contrast(s, serialLine.substring(6).toInt());
                            else if (serialLine.startsWith("SET:S:")) s->set_saturation(s, serialLine.substring(6).toInt());
                            else if (serialLine.startsWith("SET:HM:")) s->set_hmirror(s, serialLine.substring(7).toInt());
                            else if (serialLine.startsWith("SET:VF:")) s->set_vflip(s, serialLine.substring(7).toInt());
                            else if (serialLine.startsWith("SET:Q:")) s->set_quality(s, serialLine.substring(6).toInt());
                            else if (serialLine.startsWith("SET:RES:")) s->set_framesize(s, (framesize_t)serialLine.substring(8).toInt());
                        }
                    }
                }
                // JSON commands (from web_dashboard.py / unified_dashboard.py)
                else if (serialLine.startsWith("{")) {
                    JsonDocument doc;
                    DeserializationError err = deserializeJson(doc, serialLine);
                    if (!err) {
                        const char* cmd = doc["cmd"];
                        if (cmd) {
                            if (strcmp(cmd, "BTN_STATE") == 0) {
                                int btnId = doc["val"] | -1;
                                int state = doc["state"] | 0;
                                if (btnId == 0) {
                                    remoteBtnUp = (state > 0);
                                    if (state > 0) remoteUpPulse = true;
                                }
                                else if (btnId == 1) {
                                    remoteBtnDn = (state > 0);
                                    if (state > 0) remoteDnPulse = true;
                                }
                                else if (btnId == 4) {
                                    remoteBtnOk = (state > 0);
                                    if (state > 0) remoteOkPulse = true;
                                }
                                else if (btnId == 5) {
                                    if (state > 0) remoteBackPulse = true;
                                }
                            }
                            else if (strcmp(cmd, "NAV") == 0) {
                                const char* act = doc["action"];
                                if (act) {
                                    if (strcmp(act, "UP") == 0) {
                                        remoteUpPulse = true;
                                    }
                                    else if (strcmp(act, "DOWN") == 0) {
                                        remoteDnPulse = true;
                                    }
                                    else if (strcmp(act, "OK") == 0) {
                                        remoteOkPulse = true;
                                    }
                                    else if (strcmp(act, "BACK") == 0) {
                                        if (uiState.screen == SCR_USB_WEBCAM) {
                                            closeUsbWebcamScreen();
                                            switchScreen(SCR_MAIN_MENU);
                                        } else {
                                            remoteBackPulse = true;
                                        }
                                    }
                                    else if (strcmp(act, "MENU") == 0) {
                                        if (uiState.screen == SCR_MAIN_MENU) {
                                            switchScreen(SCR_VIEWFINDER);
                                        } else {
                                            if (uiState.screen == SCR_USB_WEBCAM) closeUsbWebcamScreen();
                                            switchScreen(SCR_MAIN_MENU);
                                        }
                                    }
                                }
                            }
                            else if (strcmp(cmd, "SET_ROTATION") == 0) {
                                int rot = doc["val"] | 0;
                                if (rot == 0 || rot == 2) {
                                    currentRotation = (uint8_t)rot;
                                    tft.setRotation(currentRotation);
                                    uiState.dirtyMenu = true;
                                    uiState.dirtyFeed = true;
                                    Serial.printf("{\"type\":\"ROT_CHANGE\",\"val\":%d}\n", currentRotation);
                                }
                            }
                            else if (strcmp(cmd, "SET_SCREEN") == 0) {
                                int scr = doc["screen"] | 0;
                                if (scr >= 0 && scr < SCR_COUNT) {
                                    if (scr == SCR_USB_WEBCAM && uiState.screen != SCR_USB_WEBCAM) {
                                        openUsbWebcamScreen();
                                        usbWebcamStreaming = true;
                                    } else if (scr == SCR_ESPNOW && uiState.screen != SCR_ESPNOW) {
                                        openEspNowScreen();
                                    } else {
                                        if (uiState.screen == SCR_USB_WEBCAM && scr != SCR_USB_WEBCAM) {
                                            closeUsbWebcamScreen();
                                        }
                                        switchScreen((Screen)scr);
                                    }
                                }
                            }
                            else if (strcmp(cmd, "ESPNOW_START") == 0) {
                                if (uiState.screen != SCR_ESPNOW) {
                                    openEspNowScreen();
                                }
                                uiState.streamActive = true;
                                espnow_stream_start();
                                uiState.dirtyMenu = true;
                            }
                            else if (strcmp(cmd, "ESPNOW_STOP") == 0) {
                                uiState.streamActive = false;
                                espnow_stream_stop();
                                uiState.dirtyMenu = true;
                            }
                            else if (strcmp(cmd, "START_STREAM") == 0) {
                                if (uiState.screen != SCR_USB_WEBCAM) {
                                    openUsbWebcamScreen();
                                }
                                usbWebcamStreaming = true;
                            }
                            else if (strcmp(cmd, "STOP_STREAM") == 0) {
                                usbWebcamStreaming = false;
                                if (uiState.screen == SCR_USB_WEBCAM) {
                                    closeUsbWebcamScreen();
                                }
                            }
                            else if (strcmp(cmd, "REC_START") == 0) {
                                if (uiState.screen == SCR_VIEWFINDER && !uiState.recording) {
                                    startRecording();
                                }
                            }
                            else if (strcmp(cmd, "REC_STOP") == 0) {
                                if (uiState.recording) {
                                    stopRecording();
                                }
                            }
                            else if (strcmp(cmd, "PING") == 0) {
                                Serial.println("{\"type\":\"PONG\"}");
                            }
                            else if (strcmp(cmd, "FS_LS") == 0) {
                                handleFsLs(doc["path"] | "/");
                            }
                            else if (strcmp(cmd, "FS_READ") == 0) {
                                handleFsRead(doc["path"] | "", doc["offset"] | 0, doc["size"] | 512);
                            }
                            else if (strcmp(cmd, "PWR_SET_PROFILE") == 0) {
                                const char* prof = doc["profile"] | "AUTO";
                                if (strcmp(prof, "PERF") == 0) power_manager_set_profile(PWR_PROFILE_PERF);
                                else if (strcmp(prof, "BALANCED") == 0) power_manager_set_profile(PWR_PROFILE_BALANCED);
                                else if (strcmp(prof, "ECO") == 0) power_manager_set_profile(PWR_PROFILE_ECO);
                                else power_manager_set_profile(PWR_PROFILE_AUTO);
                                power_manager_activity();
                                Serial.printf("{\"type\":\"PWR_PROFILE_CHANGE\",\"profile\":\"%s\",\"cpuMhz\":%lu}\n",
                                              power_manager_get_profile_name(power_manager_get_profile()),
                                              (unsigned long)power_manager_get_cpu_freq());
                            }
                            else if (strcmp(cmd, "PWR_SLEEP") == 0) {
                                const char* mode = doc["mode"] | "LIGHT";
                                if (strcmp(mode, "DEEP") == 0) {
                                    power_manager_enter_deep_sleep();
                                } else {
                                    power_manager_enter_light_sleep();
                                    uiState.dirtyMenu = true;
                                    uiState.dirtyFeed = true;
                                }
                            }
                            else if (strcmp(cmd, "PWR_GET_INFO") == 0) {
                                const PowerStats& pwr = power_manager_get_stats();
                                Serial.printf("{\"type\":\"PWR_INFO\",\"profile\":\"%s\",\"cpuMhz\":%lu,\"coreTemp\":%.1f,\"throttled\":%s}\n",
                                              pwr.profileName, (unsigned long)pwr.currentFreqMhz, pwr.coreTempC,
                                              pwr.isThrottled ? "true" : "false");
                            }
                        }
                    }
                }
            }
            serialLine = "";
        } else if (c != '\r') {
            if (serialLine.length() < 256) serialLine += c;
        }
    }
}

static void broadcastDeviceState() {
    static uint32_t lastBroadcast = 0;
    static Screen lastReportedScreen = (Screen)-1;
    static bool lastReportedRec = false;

    // In USB webcam mode during active streaming, throttle serial text broadcast to every 2000ms
    if (uiState.screen == SCR_USB_WEBCAM && usbWebcamStreaming) {
        if (millis() - lastBroadcast < 2000) return;
    }

    uint32_t now = millis();
    bool stateChanged = (uiState.screen != lastReportedScreen || uiState.recording != lastReportedRec);

    if (stateChanged || (now - lastBroadcast >= 500)) {
        lastBroadcast = now;
        lastReportedScreen = uiState.screen;
        lastReportedRec = uiState.recording;

        const PowerStats& pwr = power_manager_get_stats();
        uint32_t activeFps = (displayFps > 0) ? displayFps : captureFps;
        Serial.printf("{\"type\":\"STATE\",\"screen\":%d,\"screenName\":\"%s\",\"recording\":%s,\"fps\":%u,\"heap\":%u,\"psram\":%u,\"rotation\":%d,\"coreTemp\":%.1f,\"cpuMhz\":%lu,\"pwrProfile\":\"%s\",\"throttled\":%s}\n",
            (int)uiState.screen,
            getScreenName(uiState.screen),
            uiState.recording ? "true" : "false",
            activeFps,
            esp_get_free_heap_size(),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            currentRotation,
            pwr.coreTempC,

            (unsigned long)pwr.currentFreqMhz,
            pwr.profileName,
            pwr.isThrottled ? "true" : "false"
        );
    }
}

// ─── Input ────────────────────────────────────────────────────────
static void handleInput() {
    pollBtn(bUp, BTN_UP, remoteBtnUp, remoteUpPulse);
    pollBtn(bOk, BTN_OK, remoteBtnOk, remoteOkPulse);
    pollBtn(bDn, BTN_DN, remoteBtnDn, remoteDnPulse);

    // Long-press OK or Remote Back: global / screen-specific exits
    if (longPressed(bOk) || remoteBtnBack || remoteBackPulse) {
        remoteBtnBack = false;
        remoteBackPulse = false;
        handleBackAction();
        return;
    }

    switch (uiState.screen) {

        case SCR_VIEWFINDER:
            if (bOk.shortPress) {
                if (uiState.recording) stopRecording();
                else                   startRecording();
            }
            if (bUp.shortPress) {
                switchScreen(SCR_MAIN_MENU);
            }
            if (bDn.shortPress) {
                switchScreen(SCR_SETTINGS);
            }
            break;

        case SCR_MAIN_MENU:
            if (bUp.shortPress || holdRepeat(bUp)) ui_nav_up(camCfg, uiState, fileList);
            if (bDn.shortPress || holdRepeat(bDn)) ui_nav_down(camCfg, uiState, fileList);
            if (bOk.shortPress) {
                Screen dest = ui_mm_select(uiState);
                if (dest == SCR_FILES) {
                    player_scan_files(fileList);
                    uiState.fileCursor = 0;
                    uiState.fileTop    = 0;
                    switchScreen(SCR_FILES);
                } else if (dest == SCR_AUDIO) {
                    openAudioScreen();
                } else if (dest == SCR_LIVE_RADIO) {
                    openLiveRadioScreen();
                } else if (dest == SCR_QR_READER) {
                    openQRScreen();
                } else if (dest == SCR_ESPNOW) {
                    openEspNowScreen();
                } else if (dest == SCR_USB_WEBCAM) {
                    openUsbWebcamScreen();
                } else if (dest == SCR_WIFI_STREAM) {
                    openWiFiStreamScreen();
                    // switchScreen(SCR_WIFI_STREAM);
                } else if (dest != SCR_MAIN_MENU) {
                    switchScreen(dest);
                }
            }
            break;

        case SCR_SETTINGS:
            if (bUp.shortPress || holdRepeat(bUp)) {
                bool ri = ui_nav_up(camCfg, uiState, fileList);
                if (ri) reinitPreviewCamera(); else camera_apply_sensor(camCfg);
                uiState.dirtyMenu = true;
            }
            if (bDn.shortPress || holdRepeat(bDn)) {
                bool ri = ui_nav_down(camCfg, uiState, fileList);
                if (ri) reinitPreviewCamera(); else camera_apply_sensor(camCfg);
                uiState.dirtyMenu = true;
            }
            if (bOk.shortPress) ui_nav_ok(camCfg, uiState);
            break;

        case SCR_FILES:
            if (bUp.shortPress || holdRepeat(bUp)) { ui_nav_up(camCfg,uiState,fileList);   uiState.dirtyMenu=true; }
            if (bDn.shortPress || holdRepeat(bDn)) { ui_nav_down(camCfg,uiState,fileList); uiState.dirtyMenu=true; }
            if (bOk.shortPress && fileList.count > 0)
                openPlayback(fileList.names[uiState.fileCursor]);
            break;

        case SCR_PLAYBACK:
            if (bOk.shortPress) {
                uiState.playPaused = !uiState.playPaused;
                playState.state    = uiState.playPaused ? PLAY_PAUSED : PLAY_PLAYING;
                uiState.dirtyMenu  = true;
            }
            if (bDn.shortPress) closePlayback();
            break;

        case SCR_ESPNOW:
            if (bOk.shortPress) {
                StreamState ss = espnow_stream_state();
                if (ss == STREAM_IDLE || ss == STREAM_ERROR || ss == STREAM_PAUSED) {
                    uiState.streamActive = true;
                    espnow_stream_start();
                } else {
                    uiState.streamActive = false;
                    espnow_stream_stop();
                }
                uiState.dirtyMenu = true;
            }
            break;

        case SCR_AUDIO:
            if (bOk.shortPress) {
                if (audState.active) {
                    audio_stop(audState);
                } else {
                    audio_start(audState);
                }
                uiState.dirtyMenu = true;
            }
            break;

        case SCR_LIVE_RADIO: {
            bool pttHeld = bUp.last;
            if (pttHeld && !lrPttWasHeld) {
                live_radio_start_tx();
            } else if (!pttHeld && lrPttWasHeld) {
                live_radio_stop_tx();
            }
            lrPttWasHeld = pttHeld;

            if (bDn.shortPress) {
                live_radio_next_channel();
                uiState.dirtyMenu = true;
                uiState.dirtyFeed = true;
            }
            if (bOk.shortPress) {
                live_radio_select_action();
                uiState.dirtyMenu = true;
            }
            break;
        }

        case SCR_QR_READER:
            if (bOk.shortPress) {
                if (qrResult[0] != '\0' && !qrSaved) {
                    // First OK after decode → save
                    qrSaveToSD(qrResult);
                    qrSaved = true;
                    uiState.dirtyMenu = true;
                } else {
                    // Second OK (after save) OR no result yet → restart scan
                    qrResult[0] = '\0';
                    qrSaved     = false;
                    qrScanning  = true;
                    uiState.dirtyMenu = true;
                }
            }
            break;

        case SCR_USB_WEBCAM:
            if (bOk.shortPress) {
                usbWebcamStreaming = !usbWebcamStreaming;
                uiState.dirtyMenu = true;
            }
            if (bUp.shortPress) {
                closeUsbWebcamScreen();
                switchScreen(SCR_MAIN_MENU);
            }
            if (bDn.shortPress) {
                closeUsbWebcamScreen();
                switchScreen(SCR_VIEWFINDER);
            }
            break;
        case SCR_WIFI_STREAM:
            if (bOk.shortPress) {
                wifiStreamSelectAction();
            }
            break;

        default: break;
    }
}

// ─── Setup ────────────────────────────────────────────────────────
void setup() {
    Serial.setTxBufferSize(32768);
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[MAIN] ESP32S3 CamUI — Video + Audio Recording");

    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_OK, INPUT_PULLUP);
    pinMode(BTN_DN, INPUT_PULLUP);

    // ── SPI Bus Isolation (Ensure SD Card is unselected before TFT init) ──
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH);

    pinMode(TFT_RST, OUTPUT);
    digitalWrite(TFT_RST, HIGH);
    delay(10);
    digitalWrite(TFT_RST, LOW);
    delay(50);
    digitalWrite(TFT_RST, HIGH);
    delay(120);

    tft.init();
    tft.setRotation(currentRotation);
    tft.fillScreen(TFT_BLACK);
    tft.setSwapBytes(true);
    power_manager_init(&tft);

    ui_eeprom_load(camCfg);

    frameMutex = xSemaphoreCreateMutex();

    esp_log_level_set("camera",  ESP_LOG_ERROR);
    esp_log_level_set("cam_hal", ESP_LOG_ERROR);
    esp_log_level_set("*",       ESP_LOG_ERROR);

    Serial.println("[MAIN] Init camera...");
    if (!camera_init_rgb565(camCfg)) {
        tft.fillScreen(TFT_RED);
        tft.setCursor(10, 150);
        tft.setTextColor(TFT_WHITE, TFT_RED);
        tft.print("CAM FAILED");
        Serial.println("[MAIN] Camera init FAILED — halting");
        while(1) delay(1000);
    }
    Serial.println("[MAIN] Camera OK");

    spFeed.setColorDepth(16);
    spFeed.setSwapBytes(true);
    if (!spFeed.createSprite(DISP_W, FEED_H)) {
        Serial.println("[MAIN] spFeed FAIL");
        while(1) delay(1000);
    }
    spMenu.setColorDepth(16);
    if (!spMenu.createSprite(DISP_W, MENU_H)) {
        Serial.println("[MAIN] spMenu FAIL");
        while(1) delay(1000);
    }
    Serial.println("[MAIN] Sprites OK");

    spFeed.fillSprite(TFT_BLACK);
    spFeed.setTextColor(C_ACCENT, TFT_BLACK);
    spFeed.drawString("Starting...", 10, FEED_H/2);
    spFeed.pushSprite(0, FEED_Y);
    tft.fillRect(0, DIV_Y, DISP_W, DIV_H, C_DIVIDER);

    // ── Ping-pong buffers ──────────────────────────────────────────
    {
        uint16_t fw = FRAME_OPTIONS[camCfg.frameIdx].w;
        uint16_t fh = FRAME_OPTIONS[camCfg.frameIdx].h;
        size_t needed = (size_t)fw * fh * 2;
        size_t perBuf = needed;
        if (perBuf > FB_MAX_SIZE) perBuf = FB_MAX_SIZE;

        pingBuf = (uint8_t*)heap_caps_malloc(perBuf, MALLOC_CAP_SPIRAM);
        pongBuf = (uint8_t*)heap_caps_malloc(perBuf, MALLOC_CAP_SPIRAM);
        if (!pingBuf || !pongBuf) {
            if (pingBuf) { heap_caps_free(pingBuf); pingBuf=nullptr; }
            if (pongBuf) { heap_caps_free(pongBuf); pongBuf=nullptr; }
            Serial.println("[MAIN] PSRAM frame buffer alloc FAILED");
            while(1) delay(1000);
        }
        pingPongSize = perBuf;
        readyBuf     = pingBuf;
        Serial.printf("[MAIN] Frame buffers OK (%u KB each, PSRAM free: %u KB)\n",
                      (unsigned)(pingPongSize/1024),
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1024));
    }

    display_ensure(0, DISP_W + 4);

    // ── SD init ────────────────────────────────────────────────────
    uiState.sdReady = recorder_sd_init();

    // ── TFT Re-init after SD init ──────────────────────────────────
    // recorder_sd_init() calls spiSD.begin(SCK=7,MISO=8,MOSI=9) which
    // reconfigures the HSPI peripheral and corrupts TFT_eSPI's SPI state.
    // We must reinitialize the TFT AFTER SD is ready to restore the display.
    digitalWrite(TFT_CS, HIGH);  // deselect TFT first
    delay(5);
    // Hardware reset pulse to bring ST7789 back to known state
    digitalWrite(TFT_RST, LOW);
    delay(20);
    digitalWrite(TFT_RST, HIGH);
    delay(120);
    tft.init();
    tft.setRotation(currentRotation);
    tft.fillScreen(TFT_BLACK);
    tft.setSwapBytes(true);
    // Redraw the "Starting..." message that was erased
    spFeed.fillSprite(TFT_BLACK);
    spFeed.setTextColor(C_ACCENT, TFT_BLACK);
    spFeed.drawString("Starting...", 10, FEED_H/2);
    spFeed.pushSprite(0, FEED_Y);
    tft.fillRect(0, DIV_Y, DISP_W, DIV_H, C_DIVIDER);
    Serial.println("[MAIN] TFT re-init after SD done");

    // ── Camera task on Core 1 — MUST be created BEFORE mic init ───
    // CRITICAL: Task stack allocated from DRAM first to prevent DMA
    // ring buffer from corrupting the stack canary at boot.
    xTaskCreatePinnedToCore(camTask, "cam", 8 * 1024, nullptr, 5, &camTaskHandle, 1);
    Serial.println("[MAIN] Camera task → Core 1 (stack=8KB)");

    // ── Microphone init (AFTER task creation) ─────────────────────
    // recorder_mic_init(); // Disabled to keep mic off at startup

    // ── Speaker probe (for playback of AVI audio tracks) ─────────
    player_spk_init();

    // ── WiFi / ESP-NOW — lazy init, only when ESP-NOW screen entered
    uiState.streamActive    = false;
    uiState.streamConnected = false;

    // ── Main menu scroll state ─────────────────────────────────────
    uiState.mmCursor          = 0;
    uiState.mmScroll.current  = 0.0f;
    uiState.mmScroll.target   = 0.0f;
    uiState.mmScroll.velocity = 0.0f;
    uiState.mmScroll.animating= false;
    uiState.mmScroll.lastMs   = millis();

    uiState.screen    = SCR_VIEWFINDER;
    uiState.dirtyMenu = true;
    drawVfPanel();

    dispFpsTimer = millis();
    Serial.printf("[MAIN] Free PSRAM: %u  Heap: %u\n",
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM), esp_get_free_heap_size());
    Serial.println("[MAIN] Ready");
}

// ─── Main loop — Core 0 ───────────────────────────────────────────
void loop() {
    processSerialCommands();
    broadcastDeviceState();

    if (liveRadioCleanupPending && millis() >= liveRadioCleanupAtMs &&
        uiState.screen != SCR_LIVE_RADIO) {
        live_radio_shutdown_stack();
        liveRadioCleanupPending = false;
    }

    handleInput();
    bool isHighLoad = (uiState.screen == SCR_QR_READER && qrScanning) || (uiState.screen == SCR_ESPNOW && uiState.streamConnected);
    power_manager_tick(uiState.recording, usbWebcamStreaming, isHighLoad);

    // ── VIEWFINDER ────────────────────────────────────────────────
    if (uiState.screen == SCR_VIEWFINDER || uiState.screen == SCR_SETTINGS) {
        renderLiveFrame();
        uint32_t now = millis();
        if (now - dispFpsTimer >= 1000) {
            displayFps   = dispFpsCount;
            dispFpsCount = 0;
            dispFpsTimer = now;
            uiState.dirtyMenu = true;
        }
        if (uiState.screen == SCR_SETTINGS) {
            if (uiState.dirtyMenu) { ui_draw_menu(spMenu, camCfg, uiState); uiState.dirtyMenu=false; }
        } else {
            if (uiState.dirtyMenu) { drawVfPanel(); uiState.dirtyMenu=false; }
        }
    }

    // ── MAIN MENU ─────────────────────────────────────────────────
    else if (uiState.screen == SCR_MAIN_MENU) {
        renderLiveFrame();
        uint32_t now = millis();
        if (now - dispFpsTimer >= 1000) {
            displayFps   = dispFpsCount;
            dispFpsCount = 0;
            dispFpsTimer = now;
        }
        ui_mm_anim_tick(uiState);
        if (uiState.dirtyMenu) {
            ui_draw_main_menu(spMenu, uiState);
            uiState.dirtyMenu = false;
        }
    }

    // ── FILE LIST (Video) ─────────────────────────────────────────
    else if (uiState.screen == SCR_FILES) {
        if (uiState.dirtyFeed) {
            spFeed.fillSprite(C_BG);
            spFeed.setTextColor(C_ACCENT2,C_BG);
            spFeed.setTextSize(2);
            spFeed.drawString("VIDEO FILES", DISP_W/2-40, FEED_H/2-10);
            spFeed.setTextSize(1);
            spFeed.setTextColor(C_DKGREY,C_BG);
            spFeed.drawString("Select AVI to play", 14, FEED_H/2+12);
            spFeed.pushSprite(0, FEED_Y);
            uiState.dirtyFeed = false;
        }
        if (uiState.dirtyMenu) { ui_draw_filelist(spMenu, fileList, uiState); uiState.dirtyMenu=false; }
    }

    // ── VIDEO PLAYBACK ────────────────────────────────────────────
    else if (uiState.screen == SCR_PLAYBACK) {
        if (playState.state == PLAY_PLAYING && playBuf) {
            bool got = player_step(playState, playBuf, playBufSz);
            if (got) {
                display_playback_to_tft(tft, playBuf, playState.decodedW, playState.decodedH);
                dispFpsCount++;
            }
        }
        if (playState.state == PLAY_DONE) { closePlayback(); return; }

        static uint32_t hudT = 0;
        if (millis()-hudT > 250 || uiState.dirtyMenu) {
            hudT = millis();
            ui_draw_playback_hud(spMenu, playState, uiState);
            uiState.dirtyMenu = false;
        }
        uint32_t now = millis();
        if (now - dispFpsTimer >= 1000) {
            displayFps=dispFpsCount; dispFpsCount=0; dispFpsTimer=now;
        }
    }

    // ── ESP-NOW STREAMING ─────────────────────────────────────────
    else if (uiState.screen == SCR_ESPNOW) {
        StreamState ss = espnow_stream_state();
        bool camInJpeg = (ss == STREAM_CONNECTED);

        if (!camInJpeg && frameReady) {
            xSemaphoreTake(frameMutex, portMAX_DELAY);
            uint8_t* buf = (uint8_t*)readyBuf;
            uint16_t fw  = readyW, fh = readyH;
            frameReady   = false;
            xSemaphoreGive(frameMutex);
            spFeed.fillSprite(TFT_BLACK);
            display_scale_to_sprite(spFeed, buf, fw, fh, LIVE_RGB565_SWAP_BYTES);
            spFeed.setTextColor(0xF81F, TFT_BLACK);
            spFeed.setTextSize(1);
            spFeed.drawString(ss==STREAM_SEARCHING?"SEARCHING...":"ESP-NOW", 4, 2);
            spFeed.pushSprite(0, FEED_Y);
            dispFpsCount++;
        } else if (camInJpeg) {
            static uint32_t feedT = 0;
            if (millis() - feedT > 200) {
                feedT = millis();
                spFeed.fillSprite(TFT_BLACK);
                spFeed.setTextColor(C_GREEN, TFT_BLACK);
                spFeed.setTextSize(2);
                spFeed.drawString("LIVE TX", DISP_W/2-28, FEED_H/2-16);
                spFeed.setTextSize(1);
                const StreamStats& st = espnow_stream_stats();
                char fbuf[32];
                snprintf(fbuf, sizeof(fbuf), "%lu fps  %.0fC  avg %luKB", st.fps, temperatureRead(), st.avgFrameKB);
                spFeed.setTextColor(C_ACCENT2, TFT_BLACK);
                spFeed.drawString(fbuf, 4, FEED_H-14);
                bool blink = ((millis()/300)&1);
                spFeed.fillCircle(DISP_W-8, 8, 4, blink?C_RED:C_DKGREY);
                spFeed.pushSprite(0, FEED_Y);
            }
        }

        uint32_t now = millis();
        if (now - dispFpsTimer >= 1000) {
            displayFps=dispFpsCount; dispFpsCount=0; dispFpsTimer=now;
        }

        espnow_stream_tick();
        ss = espnow_stream_state();
        uiState.streamConnected = (ss == STREAM_CONNECTED);

        static uint32_t espnowPanelT = 0;
        if (millis()-espnowPanelT>300 || uiState.dirtyMenu) {
            espnowPanelT = millis();
            const StreamStats& st = espnow_stream_stats();
            ui_draw_espnow_screen(spMenu, uiState, (uint8_t)ss,
                                  st.fps, st.framesSent, st.bytesSent,
                                  st.receiverFound?st.receiverMAC:nullptr,
                                  st.avgFrameKB, st.droppedFrames);
            uiState.dirtyMenu = false;
        }
    }

    // ── AUDIO RECORDING ───────────────────────────────────────────
    else if (uiState.screen == SCR_AUDIO) {
        // Pump mic → SD on every loop iteration (time-critical)
        if (audState.active) {
            if (!audio_pump(audState)) {
                audio_stop(audState);
                uiState.dirtyMenu = true;
            }
        }
        // UI refresh every 250 ms only
        static uint32_t audioUiT = 0;
        if (millis() - audioUiT > 250 || uiState.dirtyMenu) {
            audioUiT = millis();
            audio_draw_ui(spMenu, audState);
            uiState.dirtyMenu = false;
        }
    }

    // ── LIVE RADIO ────────────────────────────────────────────────
    else if (uiState.screen == SCR_LIVE_RADIO) {
        live_radio_tick();
        static uint32_t liveRadioUiT = 0;
        if (millis() - liveRadioUiT > 120 || uiState.dirtyMenu || uiState.dirtyFeed) {
            liveRadioUiT = millis();
            live_radio_draw(spFeed, spMenu, millis());
            uiState.dirtyFeed = false;
            uiState.dirtyMenu = false;
        }
    }

    // ── QR CODE READER ────────────────────────────────────────────
    else if (uiState.screen == SCR_QR_READER) {
        static uint32_t qrLastDbg = 0;
        if (millis() - qrLastDbg > 3000) {
            Serial.printf("[QR] State: scanning=%d result='%s' saved=%d frameReady=%d\n", 
                          qrScanning, qrResult, qrSaved, frameReady);
            qrLastDbg = millis();
        }

        // RUN SCAN FIRST before renderLiveFrame consumes frameReady
        if (qrScanning && frameReady) {
            qrDoScan();  // This will consume frameReady
            uiState.dirtyMenu = true;
        }

        // Now render the (consumed or not) frame for display
        renderLiveFrame();

        // UI refresh
        static uint32_t qrUiT = 0;
        if (millis() - qrUiT > 150 || uiState.dirtyMenu) {
            qrUiT = millis();
            ui_draw_qr_screen(spMenu, uiState, qrResult, qrScanning, qrSaved);
            uiState.dirtyMenu = false;
        }
    }

    // ── USB WEBCAM ───────────────────────────────────────────────
    else if (uiState.screen == SCR_USB_WEBCAM) {
        static uint32_t wcUiT = 0;
        if (millis() - wcUiT >= 250 || uiState.dirtyMenu || uiState.dirtyFeed) {
            wcUiT = millis();
            ui_draw_usb_webcam(spMenu, spFeed, uiState, usbWebcamStreaming, captureFps, camCfg, usbWebcamAudioStreaming);
            uiState.dirtyMenu = false;
            uiState.dirtyFeed = false;
        }
    } else if (uiState.screen == SCR_WIFI_STREAM) {
        wifiStreamTick();
        static uint32_t wifiUiT = 0;
        if (millis() - wifiUiT > 250 || uiState.dirtyMenu || uiState.dirtyFeed) {
            wifiUiT = millis();
            drawWiFiStream(spFeed, spMenu);
            uiState.dirtyMenu = false;
            uiState.dirtyFeed = false;
        }
    }
}
