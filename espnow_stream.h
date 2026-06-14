#pragma once
// ══════════════════════════════════════════════════════════════════
//  espnow_stream.h  —  ESP-NOW Live Video Streaming (Sender)
//  Seeed XIAO ESP32-S3 Sense  |  OV3660
//  Protocol: JPEG chunked over ESP-NOW, 250-byte max packet
//
//  Throughput budget:
//    ESP-NOW practical TX ≈ 150-180 KB/s
//    HQVGA JPEG Q12       ≈  8-12 KB/frame
//    Target FPS           ≈  12-15 fps (was 6 fps with QVGA Q8)
// ══════════════════════════════════════════════════════════════════
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include "esp_camera.h"

// ─── Packet Types ─────────────────────────────────────────────────
#define VID_HANDSHAKE     0x10
#define VID_HANDSHAKE_ACK 0x11
#define VID_FRAME_START   0x12
#define VID_FRAME_CHUNK   0x13
#define VID_FRAME_END     0x14
#define VID_ACK           0x15
#define VID_ERROR         0x16

// ─── Protocol constants ───────────────────────────────────────────
#define ESPNOW_CHANNEL       1
#define ESPNOW_CHUNK_SIZE  240   // bytes of JPEG payload per chunk (250 - 7 byte header = 243, use 240)
#define ESPNOW_MAX_PACKET  250
#define HANDSHAKE_TIMEOUT_MS 5000
#define FRAME_TIMEOUT_MS     300

// ── Chunk delay tuning ────────────────────────────────────────────
// 1ms is the sweet spot: enough for WiFi task to drain TX queue,
// not so much that it kills FPS. Double-buffer on receiver means
// we don't need extra delay to "protect" the decode window.
#define CHUNK_DELAY_MS         1   // 1ms — proven minimum for ESP-NOW stability

// ── Adaptive frame drop ───────────────────────────────────────────
// If the previous frame's chunks are still in-flight, skip this frame
// instead of queuing — keeps latency low and avoids buffer overflow.
#define ENABLE_ADAPTIVE_DROP   1

// ─── JPEG quality & resolution for streaming ──────────────────────
//  OV3660 jpeg_quality: lower number = BETTER quality, larger file
//  Reality check from serial logs:
//    Q6  → ~2400B  (camera ignores, applies internal minimum compression)
//    Q10 → ~4000B  (camera accepts, good quality)
//    Q12 → ~2000B  (too compressed, blocky)
//  HQVGA 240×176: ~8 chunks at Q10 → fast send, good quality
#define STREAM_JPEG_QUALITY   10           // Q10 — OV3660 actually respects this
#define STREAM_FRAME_SIZE     FRAMESIZE_HQVGA     // 240×176

// ─── Streaming state ──────────────────────────────────────────────
enum StreamState {
    STREAM_IDLE = 0,
    STREAM_SEARCHING,    // broadcasting handshake, waiting for ACK
    STREAM_CONNECTED,    // receiver found, streaming active
    STREAM_PAUSED,       // user paused
    STREAM_ERROR
};

struct StreamStats {
    uint32_t framesSent;
    uint32_t bytesSent;
    uint32_t droppedFrames;
    uint32_t fps;
    uint32_t avgFrameKB;
    uint32_t dataRateBps;      // current data rate in bytes/sec
    uint32_t chunksSent;       // total chunks sent
    uint32_t chunksFailed;     // chunks that failed to send
    uint8_t  lossPercent;      // estimated packet loss %
    uint8_t  receiverMAC[6];
    bool     receiverFound;
};

// ─── Public API ───────────────────────────────────────────────────

// Call once in setup() — does NOT touch WiFi mode (caller sets WIFI_AP_STA)
void espnow_stream_init();

// Call from loop() when SCR_ESPNOW is active
void espnow_stream_tick();

// Start/stop streaming
void espnow_stream_start();
void espnow_stream_stop();

// Broadcast a new handshake (re-scan for receiver)
void espnow_stream_scan();

// Current state & stats (read-only)
StreamState        espnow_stream_state();
const StreamStats& espnow_stream_stats();

// True if a frame was just captured and sent this tick
bool espnow_stream_active();

// Deinit (called when leaving SCR_ESPNOW screen)
void espnow_stream_deinit();

// ─── Frame rate target ────────────────────────────────────────────
extern uint32_t g_streamFrameIntervalMs;  // default 80 ms (~12 FPS, was 150ms ~6 FPS)