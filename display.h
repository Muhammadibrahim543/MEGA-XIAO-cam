#pragma once
// ══════════════════════════════════════════════════════════════════
//  display.h  —  MAX FPS build
//  Key insight: pushImage(entire frame) = 1 SPI transaction
//  vs pushImage(row by row) = 172 transactions → 100x slower
// ══════════════════════════════════════════════════════════════════
#include <TFT_eSPI.h>
#include "esp_heap_caps.h"
#include "camera_config.h"
#include "img_converters.h"

// ─── Full scaled frame buffer in PSRAM ───────────────────────────
// We scale the entire frame into this buffer first,
// then push it to TFT in ONE single SPI transaction.
static uint16_t* s_scaleBuf = nullptr;  // DISP_W * FEED_H pixels
static uint16_t* s_rowBuf   = nullptr;  // temp row (unused now but kept)
static uint8_t*  s_rgbBuf   = nullptr;
static size_t    s_rgbSz    = 0;

inline void display_ensure(size_t rgbNeeded, uint16_t /*unused*/) {
    // Scale buffer: full feed area
    if (!s_scaleBuf) {
        s_scaleBuf = (uint16_t*)heap_caps_malloc(
            DISP_W * FEED_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }
    if (rgbNeeded > s_rgbSz) {
        if (s_rgbBuf) heap_caps_free(s_rgbBuf);
        s_rgbBuf = (uint8_t*)heap_caps_malloc(rgbNeeded + 64, MALLOC_CAP_SPIRAM);
        s_rgbSz  = s_rgbBuf ? rgbNeeded + 64 : 0;
    }
}

// ─── Core scaler → writes into s_scaleBuf ────────────────────────
// swapBytes=true: source pixel byte order differs from the TFT_eSPI sprite.
inline void display_scale_to_sprite(TFT_eSprite& sp,
                                     const uint8_t* src8,
                                     uint16_t srcW, uint16_t srcH,
                                     bool swapBytes)
{
    if (!s_scaleBuf) return;

    // Compute letterbox destination rect
    uint32_t scaleX = ((uint32_t)srcW << 16) / DISP_W;
    uint32_t scaleY = ((uint32_t)srcH << 16) / FEED_H;
    uint32_t scale  = scaleX > scaleY ? scaleX : scaleY;

    uint16_t dstW = (uint16_t)(((uint32_t)srcW << 16) / scale);
    uint16_t dstH = (uint16_t)(((uint32_t)srcH << 16) / scale);
    uint16_t offX = (DISP_W - dstW) / 2;
    uint16_t offY = (FEED_H - dstH) / 2;

    const uint16_t* src = (const uint16_t*)src8;
    uint16_t BLACK = 0x0000;

    // Fill entire scale buffer with black first (bars)
    memset(s_scaleBuf, 0, DISP_W * FEED_H * sizeof(uint16_t));

    // Scale into the correct region of s_scaleBuf
    for (uint16_t dy = 0; dy < dstH; dy++) {
        uint32_t sy = ((uint32_t)dy * scale) >> 16;
        if (sy >= srcH) sy = srcH - 1;
        const uint16_t* srcRow = src + sy * srcW;
        uint16_t* dstRow = s_scaleBuf + (offY + dy) * DISP_W + offX;

        for (uint16_t dx = 0; dx < dstW; dx++) {
            uint32_t sx = ((uint32_t)dx * scale) >> 16;
            if (sx >= srcW) sx = srcW - 1;
            uint16_t px = srcRow[sx];
            dstRow[dx] = swapBytes ? ((px >> 8) | (px << 8)) : px;
        }
    }

    // ONE single pushImage call for the entire feed area
    sp.pushImage(0, 0, DISP_W, FEED_H, s_scaleBuf);
}

// ─── Live feed: JPEG → decode → scale ────────────────────────────
inline void display_frame_to_sprite(TFT_eSprite& sp,
                                     const uint8_t* jpegBuf,
                                     size_t jpegLen,
                                     uint16_t srcW, uint16_t srcH)
{
    size_t needed = (size_t)srcW * srcH * 2;
    display_ensure(needed, DISP_W);
    if (!s_rgbBuf || !s_scaleBuf) { sp.fillSprite(TFT_BLACK); return; }

    if (!jpg2rgb565(jpegBuf, jpegLen, s_rgbBuf, JPG_SCALE_NONE)) return;

    display_scale_to_sprite(sp, s_rgbBuf, srcW, srcH, false);
}

// ─── Playback ────────────────────────────────────────────────────
inline void display_playback_to_sprite(TFT_eSprite& sp,
                                        const uint8_t* rgb565,
                                        uint16_t srcW, uint16_t srcH)
{
    display_ensure(0, DISP_W);
    display_scale_to_sprite(sp, rgb565, srcW, srcH, false);
}

inline void display_playback_to_tft(TFT_eSPI& tft,
                                    const uint8_t* src8,
                                    uint16_t srcW, uint16_t srcH)
{
    display_ensure(0, DISP_W);
    if (!s_scaleBuf || !src8 || srcW == 0 || srcH == 0) return;

    uint32_t scaleX = ((uint32_t)srcW << 16) / DISP_W;
    uint32_t scaleY = ((uint32_t)srcH << 16) / FEED_H;
    uint32_t scale  = scaleX > scaleY ? scaleX : scaleY;

    uint16_t dstW = (uint16_t)(((uint32_t)srcW << 16) / scale);
    uint16_t dstH = (uint16_t)(((uint32_t)srcH << 16) / scale);
    uint16_t offX = (DISP_W - dstW) / 2;
    uint16_t offY = (FEED_H - dstH) / 2;

    const uint16_t* src = (const uint16_t*)src8;
    memset(s_scaleBuf, 0, DISP_W * FEED_H * sizeof(uint16_t));

    for (uint16_t dy = 0; dy < dstH; dy++) {
        uint32_t sy = ((uint32_t)dy * scale) >> 16;
        if (sy >= srcH) sy = srcH - 1;
        const uint16_t* srcRow = src + sy * srcW;
        uint16_t* dstRow = s_scaleBuf + (offY + dy) * DISP_W + offX;

        for (uint16_t dx = 0; dx < dstW; dx++) {
            uint32_t sx = ((uint32_t)dx * scale) >> 16;
            if (sx >= srcW) sx = srcW - 1;
            dstRow[dx] = srcRow[sx];
        }
    }

    tft.pushImage(0, FEED_Y, DISP_W, FEED_H, s_scaleBuf);
}
