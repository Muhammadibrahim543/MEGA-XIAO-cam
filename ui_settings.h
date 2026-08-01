#pragma once
// ══════════════════════════════════════════════════════════════════
//  ui_settings.h
//  Sprite-based double-buffered UI for ST7789 172×320
//  Screens: VIEWFINDER, MAIN_MENU, SETTINGS, FILE_LIST,
//           PLAYBACK, ESPNOW, LIVE_RADIO
// ══════════════════════════════════════════════════════════════════
#include <TFT_eSPI.h>
#include <EEPROM.h>
#include "camera_config.h"
#include "sd_player.h"

// ─── EEPROM ───────────────────────────────────────────────────────
#define EEPROM_SIZE       128
#define EEPROM_MAGIC      0xA7
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_CAM   1

// ─── Color Palette (RGB565) ───────────────────────────────────────
#define C_BG        0x0841
#define C_PANEL     0x1082
#define C_CARD      0x18C3
#define C_ACCENT    0xFD40
#define C_ACCENT2   0x07FF
#define C_RED       0xF800
#define C_GREEN     0x07E0
#define C_WHITE     0xFFFF
#define C_LTGREY    0xBDF7
#define C_GREY      0x7BEF
#define C_DKGREY    0x39E7
#define C_DIVIDER   0x2104
#define C_ORANGE    0xFC60

// ─── Screens ──────────────────────────────────────────────────────
enum Screen {
    SCR_VIEWFINDER = 0,
    SCR_MAIN_MENU,
    SCR_SETTINGS,
    SCR_FILES,
    SCR_PLAYBACK,
    SCR_ESPNOW,
    SCR_AUDIO,         // Standalone audio recording (camera off)
    SCR_QR_READER,     // QR Code Reader
    SCR_LIVE_RADIO,    // ESP-NOW live voice with camera off
    SCR_USB_WEBCAM,    // Stream to PC via Native USB
    SCR_WIFI_STREAM,   // Wi-Fi Data Stream / Mesh
    SCR_COUNT
};

// ─── Main Menu Items ──────────────────────────────────────────────
#define MAIN_MENU_COUNT  9

struct MainMenuItem {
    const char* label;
    const char* sublabel;
    uint16_t    iconColor;
    Screen      target;
};

// ─── Settings IDs ─────────────────────────────────────────────────
enum SettingID {
    S_FRAMESIZE=0, S_REC_FRAMESIZE, S_WC_FRAMESIZE, S_QUALITY, S_BRIGHTNESS, S_CONTRAST,
    S_SATURATION,  S_HMIRROR, S_VFLIP,      S_AWB,
    S_WB_MODE,     S_AEC,     S_AEC_VALUE,  S_AGC,
    S_AGC_GAIN,    S_LENC,    S_RAW_GMA,    S_BPC,
    S_WPC,         S_COUNT
};

// ─── Layout constants ─────────────────────────────────────────────
#define ROW_H        32
#define MENU_ROWS     4
#define VISIBLE_FILES 4

// ─── Main Menu Layout ─────────────────────────────────────────────
#define MM_ITEM_H    38
#define MM_VISIBLE    3

// ─── Scroll animation state ───────────────────────────────────────
struct ScrollAnim {
    float    current;
    float    target;
    float    velocity;
    bool     animating;
    uint32_t lastMs;
};

// ─── UI State ─────────────────────────────────────────────────────
struct UIState {
    Screen   screen;

    // Camera settings menu
    uint8_t  menuCursor;
    uint8_t  menuTop;
    bool     editMode;

    // Main menu
    uint8_t  mmCursor;
    ScrollAnim mmScroll;

    // File list (video)
    uint8_t  fileCursor;
    uint8_t  fileTop;

    // Status overlays
    bool     recording;
    uint32_t recStartMs;
    uint32_t fps;
    bool     sdReady;

    // Video playback
    bool     playPaused;

    // ESP-NOW streaming
    bool     streamActive;
    bool     streamConnected;

    // Dirty flags
    bool     dirtyFeed;
    bool     dirtyMenu;
};

// ─── Public API ───────────────────────────────────────────────────
void  ui_init(TFT_eSPI& tft, TFT_eSprite& spFeed, TFT_eSprite& spMenu);
void  ui_eeprom_load(CamSettings& cs);
void  ui_eeprom_save(const CamSettings& cs);

// Core screen draw calls
void  ui_draw_menu(TFT_eSprite& sp, const CamSettings& cs, const UIState& ui);
void  ui_draw_feed_overlay(TFT_eSprite& sp, const UIState& ui);
void  ui_draw_filelist(TFT_eSprite& sp, const FileList& fl, const UIState& ui);
void  ui_draw_playback_hud(TFT_eSprite& sp, const PlayerState& ps, const UIState& ui);
void  ui_draw_main_menu(TFT_eSprite& sp, UIState& ui);
void  ui_draw_espnow_screen(TFT_eSprite& sp, const UIState& ui,
                            uint8_t streamState, uint32_t fps, uint32_t framesSent,
                            uint32_t bytesSent, const uint8_t* receiverMAC,
                            uint32_t avgFrameKB, uint32_t droppedFrames);
void  ui_draw_usb_webcam(TFT_eSprite& sp, const UIState& ui, bool isStreaming);
// Audio screen static panel (header only; audio.cpp draws live data itself)
void  ui_draw_audio_idle(TFT_eSprite& sp);

// QR Code Reader screen
void  ui_draw_qr_screen(TFT_eSprite& sp, const UIState& ui,
                        const char* qrData, bool scanning, bool saved);

// Navigation — returns true if framesize reinit needed
bool  ui_nav_up(CamSettings& cs, UIState& ui, FileList& fl);
bool  ui_nav_down(CamSettings& cs, UIState& ui, FileList& fl);
bool  ui_nav_ok(CamSettings& cs, UIState& ui);

// Main menu helpers
void   ui_mm_anim_tick(UIState& ui);
Screen ui_mm_select(const UIState& ui);

const char* setting_label(SettingID id);
void        setting_value_str(SettingID id, const CamSettings& cs,
                               char* buf, size_t len);
