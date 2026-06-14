// ══════════════════════════════════════════════════════════════════
//  espnow_stream.cpp  —  ESP-NOW Live Video Streaming (Sender)
//  Seeed XIAO ESP32-S3 Sense  |  OV3660  |  v1.6
//
//  Design notes:
//    • Camera reinit to JPEG mode only while streaming; restored to
//      RGB565 when streaming stops so the viewfinder is unaffected.
//    • All ESP-NOW callbacks run on WiFi task (not our core); shared
//      state is accessed through volatile flags only.
//    • Peer registry: broadcast for handshake, unicast once receiver
//      replies with its MAC in the ACK payload.
//    • No dynamic memory in hot path — static packet buffer only.
// ══════════════════════════════════════════════════════════════════
#include "espnow_stream.h"
#include "camera_config.h"
#include <string.h>
#include <stdio.h>

// ─── Internal state ───────────────────────────────────────────────
static StreamState  s_state      = STREAM_IDLE;
static StreamStats  s_stats      = {};
static bool         s_initialized = false;
static bool         s_camJpegMode = false;  // true = camera reinited for JPEG

static uint16_t     s_frameID    = 0;
static uint32_t     s_lastFrameMs = 0;
static uint32_t     s_lastHandshakeMs = 0;
static uint32_t     s_fpsTimer   = 0;
static uint32_t     s_fpsCount   = 0;
static uint32_t     s_bytesThisSec = 0;   // bytes sent in current second (for data rate)
static uint32_t     s_chunksThisSec = 0;  // chunks sent this second
static uint32_t     s_failsThisSec  = 0;  // failed chunks this second

static uint8_t      s_receiverMAC[6];
static bool         s_peerAdded  = false;

// Volatile flags set by ESP-NOW callbacks (WiFi task context)
static volatile bool s_ackReceived   = false;
static volatile bool s_sendOk        = false;
static volatile bool s_sendFailed    = false;

// Static TX packet buffer (never heap-allocated in hot path)
static uint8_t s_pkt[ESPNOW_MAX_PACKET];

// Broadcast MAC00
static const uint8_t BROADCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

uint32_t g_streamFrameIntervalMs = 80;   // ~12 FPS — matches HQVGA Q10 send time (8 chunks × 9ms = 72ms)

// ─── Send-done handshake ──────────────────────────────────────────
// Tracks whether the WiFi driver has finished sending the last packet.
// We wait for this before sending the next chunk — prevents ENOMEM drops.
static volatile bool s_lastSendDone = true;

// ─── Forward declarations ─────────────────────────────────────────
static bool  addReceiverPeer();
static void  sendHandshake();
static bool  captureAndStream(const CamSettings& cfg);
static void  sendFrameStart(uint16_t w, uint16_t h, uint32_t size, uint16_t totalChunks);
static void  sendFrameChunk(uint16_t chunkIdx, uint16_t total, const uint8_t* data, uint16_t len);
static void  sendFrameEnd();
static void  switchCamToJpeg(const CamSettings& cfg);
static void  switchCamToRgb565(const CamSettings& cfg);

// ─── ESP-NOW callbacks (WiFi task) ────────────────────────────────
static void onSent(const uint8_t* mac, esp_now_send_status_t status) {
    s_lastSendDone = true;   // release next chunk regardless of result
    if (status == ESP_NOW_SEND_SUCCESS) { s_sendOk     = true; }
    else                                { s_sendFailed = true; s_failsThisSec++; }
}

static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len < 1) return;
    uint8_t ptype = data[0];

    if (ptype == VID_HANDSHAKE_ACK && len >= 7) {
        // Payload bytes 1-6: receiver's MAC address
        memcpy(s_receiverMAC, data + 1, 6);
        s_stats.receiverFound = true;
        memcpy(s_stats.receiverMAC, s_receiverMAC, 6);
        s_ackReceived = true;
        // NOTE: No Serial.printf here — this runs on WiFi task which has a
        // small stack (~4KB). Serial.printf can overflow it and corrupt
        // adjacent memory (stack canary watchpoint crash).
    }
}

// ─── espnow_stream_init ───────────────────────────────────────────
void espnow_stream_init() {
    if (s_initialized) return;

    // WiFi must already be WIFI_AP_STA — set by main .ino
    if (esp_now_init() != ESP_OK) {
        Serial.println("[STREAM] ESP-NOW init FAILED");
        s_state = STREAM_ERROR;
        return;
    }
    esp_now_register_send_cb(onSent);
    esp_now_register_recv_cb(onRecv);

    // Add broadcast peer for handshake
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(BROADCAST_MAC))
        esp_now_add_peer(&peer);

    memset(&s_stats, 0, sizeof(s_stats));
    s_state       = STREAM_IDLE;
    s_initialized = true;
    Serial.println("[STREAM] ESP-NOW stream init OK");
}

// ─── espnow_stream_deinit ─────────────────────────────────────────
void espnow_stream_deinit() {
    if (!s_initialized) return;
    espnow_stream_stop();
    esp_now_unregister_send_cb();
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    s_initialized = false;
    s_peerAdded   = false;
    Serial.println("[STREAM] ESP-NOW deinit");
}

// ─── espnow_stream_start ──────────────────────────────────────────
void espnow_stream_start() {
    if (!s_initialized) return;
    if (s_state == STREAM_CONNECTED || s_state == STREAM_SEARCHING) return;

    // Begin scanning for receiver
    s_state             = STREAM_SEARCHING;
    s_stats.framesSent  = 0;
    s_stats.bytesSent   = 0;
    s_stats.droppedFrames = 0;
    s_stats.fps         = 0;
    s_ackReceived       = false;
    s_lastHandshakeMs   = 0;
    s_fpsTimer          = millis();
    s_fpsCount          = 0;
    Serial.println("[STREAM] Searching for receiver...");
}

// ─── espnow_stream_stop ───────────────────────────────────────────
void espnow_stream_stop() {
    extern volatile bool camPauseRequest;
    extern volatile bool camPausedAck;

    if (s_camJpegMode) {
        // camTask is already paused — switchCamToRgb565 will resume it
        extern CamSettings camCfg;
        switchCamToRgb565(camCfg);
    } else {
        // Streaming never connected, camTask is running normally
        // Make sure camPauseRequest is released (safety)
        camPauseRequest = false;
    }
    s_state     = STREAM_IDLE;
    s_peerAdded = false;
    Serial.println("[STREAM] Stopped");
}

// ─── espnow_stream_scan ───────────────────────────────────────────
void espnow_stream_scan() {
    s_stats.receiverFound = false;
    s_ackReceived = false;
    s_state = STREAM_SEARCHING;
    s_lastHandshakeMs = 0;
    Serial.println("[STREAM] Re-scanning...");
}

// ─── espnow_stream_state / stats ─────────────────────────────────
StreamState        espnow_stream_state()  { return s_state; }
const StreamStats& espnow_stream_stats()  { return s_stats; }
bool               espnow_stream_active() { return s_state == STREAM_CONNECTED; }

// ─── espnow_stream_tick ───────────────────────────────────────────
void espnow_stream_tick() {
    if (!s_initialized) return;

    uint32_t now = millis();

    // ── FPS counter update ────────────────────────────────────────
    if (now - s_fpsTimer >= 1000) {
        s_stats.fps        = s_fpsCount;
        s_stats.dataRateBps = s_bytesThisSec;   // bytes/sec this window
        // Packet loss estimate: failed / (sent + failed) * 100
        uint32_t totalPkts = s_chunksThisSec + s_failsThisSec;
        s_stats.lossPercent = (totalPkts > 0)
                              ? (uint8_t)(s_failsThisSec * 100 / totalPkts)
                              : 0;
        s_fpsCount        = 0;
        s_bytesThisSec    = 0;
        s_chunksThisSec   = 0;
        s_failsThisSec    = 0;
        s_fpsTimer        = now;
    }

    // ── SEARCHING: broadcast handshake every 1s ───────────────────
    if (s_state == STREAM_SEARCHING) {
        if (now - s_lastHandshakeMs >= 1000) {
            sendHandshake();
            s_lastHandshakeMs = now;
        }
        if (s_ackReceived) {
            s_ackReceived = false;
            // Add unicast peer
            if (addReceiverPeer()) {
                // Switch camera to JPEG for streaming
                extern CamSettings camCfg;
                switchCamToJpeg(camCfg);
                s_state       = STREAM_CONNECTED;
                s_lastFrameMs = now;
                Serial.println("[STREAM] Connected! Streaming started.");
            } else {
                s_state = STREAM_ERROR;
            }
        }
        return;
    }

    // ── CONNECTED: send frames at target interval ─────────────────
    if (s_state == STREAM_CONNECTED) {
        // Check for new ACK (receiver re-confirmed)
        if (s_ackReceived) s_ackReceived = false;

        if (now - s_lastFrameMs >= g_streamFrameIntervalMs) {
            extern CamSettings camCfg;
            if (!captureAndStream(camCfg)) {
                s_stats.droppedFrames++;
            }
            s_lastFrameMs = now;
        }
    }
}

// ─── sendHandshake ────────────────────────────────────────────────
static void sendHandshake() {
    s_pkt[0] = VID_HANDSHAKE;
    // Payload: our own MAC (6 bytes)
    uint8_t mac[6];
    WiFi.macAddress(mac);
    memcpy(s_pkt + 1, mac, 6);
    esp_now_send(BROADCAST_MAC, s_pkt, 7);
    Serial.printf("[STREAM] Handshake broadcast (MAC %02X:%02X:%02X:%02X:%02X:%02X)\n",
        mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

// ─── addReceiverPeer ──────────────────────────────────────────────
static bool addReceiverPeer() {
    if (s_peerAdded && esp_now_is_peer_exist(s_receiverMAC)) return true;
    if (esp_now_is_peer_exist(s_receiverMAC)) {
        esp_now_del_peer(s_receiverMAC);
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, s_receiverMAC, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("[STREAM] Failed to add receiver peer");
        return false;
    }
    s_peerAdded = true;
    return true;
}

// ─── Camera mode helpers ──────────────────────────────────────────
// IMPORTANT: switchCamToJpeg() pauses camTask and KEEPS it paused
// for the entire streaming session. camTask only resumes in
// switchCamToRgb565() when streaming stops.
static void switchCamToJpeg(const CamSettings& cfg) {
    if (s_camJpegMode) return;
    extern volatile bool camPauseRequest;
    extern volatile bool camPausedAck;

    // Pause camTask — keep it paused, do NOT resume until stop
    camPauseRequest = true;
    uint32_t t = millis();
    while (!camPausedAck) {
        delay(1);
        if (millis() - t > 2000) {
            Serial.println("[STREAM] camTask pause timeout!");
            break;
        }
    }

    esp_camera_deinit();

    CamSettings streamCfg = cfg;
    streamCfg.quality = STREAM_JPEG_QUALITY;
    for (uint8_t i = 0; i < FRAME_OPTIONS_COUNT; i++) {
        if (FRAME_OPTIONS[i].fs == STREAM_FRAME_SIZE) {
            streamCfg.frameIdx = i;
            break;
        }
    }

    // Use standard init (not extreme) for streaming:
    // camera_init_extreme calls set_aec2(0) which overrides jpeg_quality
    // on OV3660, causing frames to be tiny (~2400B) regardless of quality setting.
    // Standard init respects cfg.jpeg_quality correctly.
    camera_init(streamCfg);

    // Force quality on sensor register AFTER init
    sensor_t* sx = esp_camera_sensor_get();
    if (sx) {
        sx->set_quality(sx, STREAM_JPEG_QUALITY);
        sx->set_aec2(sx, 0);    // consistent exposure
        sx->set_lenc(sx, 0);    // lens correction off = less CPU
    }

    // Warmup: discard first 3 frames — camera AEC needs time to stabilize
    for (int w = 0; w < 3; w++) {
        camera_fb_t* wf = esp_camera_fb_get();
        if (wf) esp_camera_fb_return(wf);
        delay(30);
    }

    s_camJpegMode = true;
    Serial.printf("[STREAM] Camera → JPEG mode (HQVGA 240x176, Q%d) | camTask paused\n",
                  STREAM_JPEG_QUALITY);
}

static void switchCamToRgb565(const CamSettings& cfg) {
    if (!s_camJpegMode) return;
    extern volatile bool camPauseRequest;
    extern volatile bool camPausedAck;

    // camTask is already paused (camPauseRequest was true)
    // Just reinit camera and then release the task
    esp_camera_deinit();
    camera_init_rgb565(cfg);
    s_camJpegMode = false;

    // Now release camTask
    camPauseRequest = false;
    uint32_t t = millis();
    while (camPausedAck) {
        delay(1);
        if (millis() - t > 2000) {
            Serial.println("[STREAM] camTask resume timeout!");
            break;
        }
    }
    Serial.println("[STREAM] Camera → RGB565 mode | camTask resumed");
}

// ─── captureAndStream ─────────────────────────────────────────────
static bool captureAndStream(const CamSettings& cfg) {

#if ENABLE_ADAPTIVE_DROP
    // Skip frame if WiFi driver is still busy from last send.
    // This keeps latency low instead of piling up a backlog.
    if (!s_lastSendDone) {
        s_stats.droppedFrames++;
        return false;
    }
#endif

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[STREAM] Frame capture failed");
        return false;
    }
    if (fb->format != PIXFORMAT_JPEG) {
        esp_camera_fb_return(fb);
        Serial.println("[STREAM] Frame not JPEG, skipping");
        return false;
    }

    uint16_t totalChunks = (uint16_t)((fb->len + ESPNOW_CHUNK_SIZE - 1) / ESPNOW_CHUNK_SIZE);

    // ── VIDEO_FRAME_START ──────────────────────────────────────────
    s_lastSendDone = false;
    sendFrameStart((uint16_t)fb->width, (uint16_t)fb->height,
                   (uint32_t)fb->len,   totalChunks);
    // Wait for send-done callback (max 10 ms)
    uint32_t t = millis();
    while (!s_lastSendDone && millis() - t < 10) delayMicroseconds(200);

    // ── VIDEO_FRAME_CHUNK loop ─────────────────────────────────────
    uint32_t bytesSentThisFrame = 0;
    for (uint16_t i = 0; i < totalChunks; i++) {
        uint32_t offset   = (uint32_t)i * ESPNOW_CHUNK_SIZE;
        uint16_t chunkLen = (uint16_t)((fb->len - offset) < ESPNOW_CHUNK_SIZE
                                       ? (fb->len - offset)
                                       : ESPNOW_CHUNK_SIZE);

        s_lastSendDone = false;
        s_sendFailed   = false;
        sendFrameChunk(i, totalChunks, fb->buf + offset, chunkLen);
        bytesSentThisFrame += chunkLen;

        // Wait for this chunk's callback (max 8ms)
        t = millis();
        while (!s_lastSendDone && millis() - t < 8) delayMicroseconds(100);

        // Mandatory gap — WiFi task needs time to drain TX queue
        delay(CHUNK_DELAY_MS);
    }

    // ── VIDEO_FRAME_END ────────────────────────────────────────────
    s_lastSendDone = false;
    sendFrameEnd();
    t = millis();
    while (!s_lastSendDone && millis() - t < 10) delayMicroseconds(200);

    // Update stats
    s_stats.framesSent++;
    s_stats.bytesSent += bytesSentThisFrame;
    s_stats.avgFrameKB = (uint32_t)(s_stats.bytesSent / s_stats.framesSent) / 1024;
    s_stats.chunksSent += totalChunks;
    s_bytesThisSec     += bytesSentThisFrame;
    s_chunksThisSec    += totalChunks;
    s_fpsCount++;
    s_frameID++;

    esp_camera_fb_return(fb);
    return true;
}

// ─── Packet senders ───────────────────────────────────────────────
static void sendFrameStart(uint16_t w, uint16_t h, uint32_t size, uint16_t totalChunks) {
    // Byte layout:
    // [0]     type       = VID_FRAME_START (0x12)
    // [1-2]   frameID    uint16_t
    // [3-4]   chunkIdx   = 0x0000
    // [5-6]   total      = totalChunks
    // [7-8]   width      uint16_t
    // [9-10]  height     uint16_t
    // [11]    format     = 0x01 (JPEG)
    // [12-15] frameSize  uint32_t
    // [16-17] totalChunks uint16_t (redundant but explicit)
    s_pkt[0] = VID_FRAME_START;
    memcpy(s_pkt + 1,  &s_frameID,    2);
    uint16_t zero = 0;
    memcpy(s_pkt + 3,  &zero,         2);
    memcpy(s_pkt + 5,  &totalChunks,  2);
    memcpy(s_pkt + 7,  &w,            2);
    memcpy(s_pkt + 9,  &h,            2);
    s_pkt[11] = 0x01;  // JPEG
    memcpy(s_pkt + 12, &size,         4);
    memcpy(s_pkt + 16, &totalChunks,  2);
    esp_now_send(s_receiverMAC, s_pkt, 18);
}

static void sendFrameChunk(uint16_t chunkIdx, uint16_t total,
                           const uint8_t* data, uint16_t len) {
    // [0]     type      = VID_FRAME_CHUNK (0x13)
    // [1-2]   frameID   uint16_t
    // [3-4]   chunkIdx  uint16_t
    // [5-6]   total     uint16_t
    // [7+]    JPEG data (max 240 bytes)
    s_pkt[0] = VID_FRAME_CHUNK;
    memcpy(s_pkt + 1, &s_frameID,  2);
    memcpy(s_pkt + 3, &chunkIdx,   2);
    memcpy(s_pkt + 5, &total,      2);
    memcpy(s_pkt + 7, data,        len);
    esp_now_send(s_receiverMAC, s_pkt, 7 + len);
}

static void sendFrameEnd() {
    // [0]     type    = VID_FRAME_END (0x14)
    // [1-2]   frameID uint16_t
    s_pkt[0] = VID_FRAME_END;
    memcpy(s_pkt + 1, &s_frameID, 2);
    esp_now_send(s_receiverMAC, s_pkt, 3);
}