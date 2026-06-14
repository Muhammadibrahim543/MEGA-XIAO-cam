#pragma once
// ══════════════════════════════════════════════════════════════════
//  camera_config.h  —  OV3660 extreme performance config
//  Seeed XIAO ESP32-S3 Sense
// ══════════════════════════════════════════════════════════════════
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "img_converters.h"

#define CAM_STABLE_XCLK_HZ 20000000
#define CAM_FAST_XCLK_HZ   24000000

// ─── OV3660 Pin Map (Xiao ESP32-S3 Sense) ────────────────────────
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    10
#define CAM_PIN_SIOD    40
#define CAM_PIN_SIOC    39
#define CAM_PIN_D7      48
#define CAM_PIN_D6      11
#define CAM_PIN_D5      12
#define CAM_PIN_D4      14
#define CAM_PIN_D3      16
#define CAM_PIN_D2      18
#define CAM_PIN_D1      17
#define CAM_PIN_D0      15
#define CAM_PIN_VSYNC   38
#define CAM_PIN_HREF    47
#define CAM_PIN_PCLK    13

// ─── Display Geometry ─────────────────────────────────────────────
#define DISP_W  172
#define DISP_H  320
#define FEED_Y  0
#define FEED_H  172
#define DIV_Y   172
#define DIV_H   4
#define MENU_Y  176
#define MENU_H  144

// ─── Frame Size Table ─────────────────────────────────────────────
typedef struct { const char* label; framesize_t fs; uint16_t w, h; } FrameOption;

static const FrameOption FRAME_OPTIONS[] = {
    { "96x96",   FRAMESIZE_96X96,   96,   96  },
    { "QQVGA",   FRAMESIZE_QQVGA,   160,  120 },
    { "QCIF",    FRAMESIZE_QCIF,    176,  144 },
    { "HQVGA",   FRAMESIZE_HQVGA,   240,  176 },
    { "240x240", FRAMESIZE_240X240, 240,  240 },
    { "QVGA",    FRAMESIZE_QVGA,    320,  240 },
    { "CIF",     FRAMESIZE_CIF,     400,  296 },
    { "HVGA",    FRAMESIZE_HVGA,    480,  320 },
    { "VGA",     FRAMESIZE_VGA,     640,  480 },
    { "SVGA",    FRAMESIZE_SVGA,    800,  600 },
    { "XGA",     FRAMESIZE_XGA,     1024, 768 },
    { "HD",      FRAMESIZE_HD,      1280, 720 },
    { "SXGA",    FRAMESIZE_SXGA,    1280, 1024},
    { "UXGA",    FRAMESIZE_UXGA,    1600, 1200},
};
#define FRAME_OPTIONS_COUNT (sizeof(FRAME_OPTIONS)/sizeof(FRAME_OPTIONS[0]))

// ─── Settings Struct ──────────────────────────────────────────────
struct CamSettings {
    uint8_t  frameIdx;
    uint8_t  recFrameIdx;
    uint8_t  wcFrameIdx;
    int8_t   quality;
    int8_t   brightness;
    int8_t   contrast;
    int8_t   saturation;
    bool     hmirror;
    bool     vflip;
    bool     awb;
    uint8_t  wb_mode;
    bool     aec;
    int16_t  aec_value;
    bool     agc;
    uint8_t  agc_gain;
    bool     lenc;
    bool     raw_gma;
    bool     bpc;
    bool     wpc;
};

inline CamSettings cam_defaults() {
    CamSettings s;
    s.frameIdx   = 5;    // QVGA — best fps/quality balance
    s.recFrameIdx= 5;    // recording resolution
    s.wcFrameIdx = 5;    // webcam resolution
    s.quality    = 10;   // lower = better JPEG quality
    s.brightness = 1;
    s.contrast   = 1;
    s.saturation = 0;
    s.hmirror    = true;
    s.vflip      = false;
    s.awb        = true;
    s.wb_mode    = 0;
    s.aec        = true;
    s.aec_value  = 300;
    s.agc        = true;
    s.agc_gain   = 0;
    s.lenc       = false;
    s.raw_gma    = true;
    s.bpc        = false;
    s.wpc        = true;
    return s;
}

// ─── Apply sensor tweaks ──────────────────────────────────────────
inline void camera_apply_sensor(const CamSettings& s) {
    sensor_t* sx = esp_camera_sensor_get();
    if (!sx) return;
    sx->set_colorbar(sx,      0);
    sx->set_special_effect(sx,0);
    sx->set_brightness(sx,    s.brightness);
    sx->set_contrast(sx,      s.contrast);
    sx->set_saturation(sx,    s.saturation);
    sx->set_hmirror(sx,       s.hmirror  ? 1 : 0);
    sx->set_vflip(sx,         s.vflip    ? 1 : 0);
    sx->set_whitebal(sx,      s.awb      ? 1 : 0);
    sx->set_wb_mode(sx,       s.wb_mode);
    sx->set_exposure_ctrl(sx, s.aec      ? 1 : 0);
    if (!s.aec) sx->set_aec_value(sx, s.aec_value);
    sx->set_gain_ctrl(sx,     s.agc      ? 1 : 0);
    if (!s.agc) sx->set_agc_gain(sx,  s.agc_gain);
    sx->set_lenc(sx,          s.lenc     ? 1 : 0);
    sx->set_raw_gma(sx,       s.raw_gma  ? 1 : 0);
    sx->set_bpc(sx,           s.bpc      ? 1 : 0);
    sx->set_wpc(sx,           s.wpc      ? 1 : 0);
}

// ─── Standard init ────────────────────────────────────────────────
inline bool camera_init(const CamSettings& s) {
    camera_config_t cfg = {};
    cfg.ledc_channel  = LEDC_CHANNEL_0;
    cfg.ledc_timer    = LEDC_TIMER_0;
    cfg.pin_d0 = CAM_PIN_D0; cfg.pin_d1 = CAM_PIN_D1;
    cfg.pin_d2 = CAM_PIN_D2; cfg.pin_d3 = CAM_PIN_D3;
    cfg.pin_d4 = CAM_PIN_D4; cfg.pin_d5 = CAM_PIN_D5;
    cfg.pin_d6 = CAM_PIN_D6; cfg.pin_d7 = CAM_PIN_D7;
    cfg.pin_xclk      = CAM_PIN_XCLK;
    cfg.pin_pclk      = CAM_PIN_PCLK;
    cfg.pin_vsync     = CAM_PIN_VSYNC;
    cfg.pin_href      = CAM_PIN_HREF;
    cfg.pin_sccb_sda  = CAM_PIN_SIOD;
    cfg.pin_sccb_scl  = CAM_PIN_SIOC;
    cfg.pin_pwdn      = CAM_PIN_PWDN;
    cfg.pin_reset     = CAM_PIN_RESET;
    cfg.xclk_freq_hz  = CAM_FAST_XCLK_HZ;
    cfg.pixel_format  = PIXFORMAT_JPEG;
    cfg.frame_size    = FRAME_OPTIONS[s.frameIdx].fs;
    cfg.jpeg_quality  = s.quality;
    cfg.fb_count      = 2;
    cfg.grab_mode     = CAMERA_GRAB_LATEST;
    cfg.fb_location   = CAMERA_FB_IN_PSRAM;
    if (esp_camera_init(&cfg) != ESP_OK) return false;
    camera_apply_sensor(s);
    return true;
}

// ─── EXTREME init: max XCLK, 2 frame buffers, grab latest ─────────
inline bool camera_init_extreme(const CamSettings& s) {
    camera_config_t cfg = {};
    cfg.ledc_channel  = LEDC_CHANNEL_0;
    cfg.ledc_timer    = LEDC_TIMER_0;
    cfg.pin_d0 = CAM_PIN_D0; cfg.pin_d1 = CAM_PIN_D1;
    cfg.pin_d2 = CAM_PIN_D2; cfg.pin_d3 = CAM_PIN_D3;
    cfg.pin_d4 = CAM_PIN_D4; cfg.pin_d5 = CAM_PIN_D5;
    cfg.pin_d6 = CAM_PIN_D6; cfg.pin_d7 = CAM_PIN_D7;
    cfg.pin_xclk      = CAM_PIN_XCLK;
    cfg.pin_pclk      = CAM_PIN_PCLK;
    cfg.pin_vsync     = CAM_PIN_VSYNC;
    cfg.pin_href      = CAM_PIN_HREF;
    cfg.pin_sccb_sda  = CAM_PIN_SIOD;
    cfg.pin_sccb_scl  = CAM_PIN_SIOC;
    cfg.pin_pwdn      = CAM_PIN_PWDN;
    cfg.pin_reset     = CAM_PIN_RESET;

    // ── MAX XCLK: 24 MHz (OV3660 rated max) ───────────────────────
    cfg.xclk_freq_hz  = CAM_FAST_XCLK_HZ;

    cfg.pixel_format  = PIXFORMAT_JPEG;
    cfg.frame_size    = FRAME_OPTIONS[s.frameIdx].fs;

    // ── Minimum JPEG quality = maximum FPS ────────────────────────
    // Lower number = better quality but slower; we let user control this
    cfg.jpeg_quality  = s.quality;

    // ── 2 frame buffers in PSRAM — enough for double-buffering ───
    cfg.fb_count      = 2;

    // ── GRAB_LATEST = always return newest frame, drop old ones ───
    cfg.grab_mode     = CAMERA_GRAB_LATEST;

    cfg.fb_location   = CAMERA_FB_IN_PSRAM;

    if (esp_camera_init(&cfg) != ESP_OK) {
        Serial.println("[CAM] Extreme init failed, trying standard...");
        return camera_init(s);   // fallback
    }

    // ── Sensor register tweaks for OV3660 max fps ─────────────────
    sensor_t* sx = esp_camera_sensor_get();
    if (sx) {
        // Disable features that cost cycles
        sx->set_lenc(sx, 0);           // lens correction off
        sx->set_raw_gma(sx, 1);        // raw gamma (fast path)
        sx->set_dcw(sx, 1);            // downsize crop window ON
        sx->set_colorbar(sx, 0);       // color bar test OFF
        sx->set_special_effect(sx, 0); // no effect
        sx->set_ae_level(sx, 0);       // auto exposure level neutral
        sx->set_aec2(sx, 0);           // AEC2 algorithm off (simpler = faster)

        // Apply user settings on top
        camera_apply_sensor(s);

        // ── Window crop: only capture what we need ─────────────────
        // OV3660 supports windowing — crop to reduce data transfer
        // This dramatically increases fps at any given resolution
        // Using set_res_raw if available:
        // (OV3660 specific — some builds may not support all these)
        sx->set_quality(sx, s.quality);
    }

    Serial.printf("[CAM] Extreme init OK — XCLK=24MHz fb=2 grab=LATEST\n");
    return true;
}

// ─── Change framesize (requires reinit) ───────────────────────────
inline bool camera_change_framesize(CamSettings& s, uint8_t newIdx) {
    if (newIdx >= FRAME_OPTIONS_COUNT) return false;
    esp_camera_deinit();
    s.frameIdx = newIdx;
    return camera_init_extreme(s);
}

// ─── RGB565 direct mode init (no JPEG decode needed) ──────────────
// Faster for display — camera outputs raw RGB565
// Trade-off: larger frames, no SD recording in this mode
// NOTE: RGB565 frame size = w*h*2 bytes. For XGA (1024×768) that is 1.5 MB
// per frame. With fb_count=2 the driver needs 3 MB of PSRAM just for DMA.
// Use fb_count=1 for resolutions >= SVGA to keep PSRAM usage manageable.
inline bool camera_init_rgb565(const CamSettings& s) {
    camera_config_t cfg = {};
    cfg.ledc_channel  = LEDC_CHANNEL_0;
    cfg.ledc_timer    = LEDC_TIMER_0;
    cfg.pin_d0=CAM_PIN_D0; cfg.pin_d1=CAM_PIN_D1;
    cfg.pin_d2=CAM_PIN_D2; cfg.pin_d3=CAM_PIN_D3;
    cfg.pin_d4=CAM_PIN_D4; cfg.pin_d5=CAM_PIN_D5;
    cfg.pin_d6=CAM_PIN_D6; cfg.pin_d7=CAM_PIN_D7;
    cfg.pin_xclk=CAM_PIN_XCLK; cfg.pin_pclk=CAM_PIN_PCLK;
    cfg.pin_vsync=CAM_PIN_VSYNC; cfg.pin_href=CAM_PIN_HREF;
    cfg.pin_sccb_sda=CAM_PIN_SIOD; cfg.pin_sccb_scl=CAM_PIN_SIOC;
    cfg.pin_pwdn=CAM_PIN_PWDN; cfg.pin_reset=CAM_PIN_RESET;
    cfg.xclk_freq_hz  = CAM_STABLE_XCLK_HZ;  // 20 MHz — stable mode for viewfinder
    cfg.pixel_format  = PIXFORMAT_RGB565;   // direct — no decode step
    cfg.frame_size    = FRAME_OPTIONS[s.frameIdx].fs;
    cfg.jpeg_quality  = 0;   // unused for RGB565

    // For large resolutions each RGB565 frame is huge.
    // fb_count=2 at XGA = 2 × 1.5 MB = 3 MB just for camera DMA.
    // Use 1 buffer for SVGA and above to leave PSRAM for ping-pong buffers.
    uint32_t frameBytes = (uint32_t)FRAME_OPTIONS[s.frameIdx].w
                        * FRAME_OPTIONS[s.frameIdx].h * 2;
    cfg.fb_count      = (frameBytes > 800UL * 600 * 2) ? 1 : 2;

    cfg.grab_mode     = CAMERA_GRAB_LATEST;
    cfg.fb_location   = CAMERA_FB_IN_PSRAM;
    if (esp_camera_init(&cfg) != ESP_OK) return false;
    sensor_t* sx = esp_camera_sensor_get();
    if (sx) {
        sx->set_dcw(sx, 1);
        sx->set_aec2(sx, 0);
    }
    camera_apply_sensor(s);
    Serial.printf("[CAM] RGB565 mode OK — %s  fb_count=%d\n",
                  FRAME_OPTIONS[s.frameIdx].label, (int)cfg.fb_count);
    return true;
}
