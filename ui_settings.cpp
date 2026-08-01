// ══════════════════════════════════════════════════════════════════
//  ui_settings.cpp
//  All screen draw routines + navigation (video only)
// ══════════════════════════════════════════════════════════════════
#include "ui_settings.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

// ─── Main Menu Table ──────────────────────────────────────────────
static const MainMenuItem MM_ITEMS[MAIN_MENU_COUNT] = {
    { "VIEWFINDER",    "Live camera preview",    C_ACCENT2,  SCR_VIEWFINDER },
    { "VIDEO FILES",   "Browse & play AVI",      C_GREEN,    SCR_FILES      },
    { "QR CODE",       "Scan & save QR codes",   0xFFE0,     SCR_QR_READER  },
    { "AUDIO REC",     "Record WAV to SD card",  C_ORANGE,   SCR_AUDIO      },
    { "LIVE RADIO",    "PTT voice over ESP-NOW", C_ACCENT2,  SCR_LIVE_RADIO },
    { "SETTINGS",      "Camera configuration",   C_ACCENT,   SCR_SETTINGS   },
    { "ESP-NOW",       "Wireless streaming",     0xF81F,     SCR_ESPNOW     },
    { "USB WEBCAM",    "Stream to PC via USB",   0x07FF,     SCR_USB_WEBCAM },
    { "WI-FI STREAM",  "Telemetry & File sync",  C_GREEN,    SCR_WIFI_STREAM}
};

// ─── Spring constants ─────────────────────────────────────────────
#define SPRING_STIFFNESS  0.28f
#define SPRING_DAMPING    0.62f
#define ANIM_THRESHOLD    0.4f

// ─── Camera setting labels/values ────────────────────────────────
static const char* LABELS[S_COUNT] = {
    "Resolution","Recording Res","WebCam Res","Quality","Brightness","Contrast",
    "Saturation","H-Mirror","V-Flip","Auto WB",
    "WB Mode","Auto Exp","Exp Value","Auto Gain",
    "Gain","Lens Corr","Raw Gamma","BPC","WPC"
};
static const char* WB_NAMES[] = {"Auto","Sunny","Cloudy","Office","Home"};

const char* setting_label(SettingID id) {
    return (id < S_COUNT) ? LABELS[id] : "?";
}

void setting_value_str(SettingID id, const CamSettings& cs, char* b, size_t n) {
    switch(id) {
        case S_FRAMESIZE:     snprintf(b,n,"%s",FRAME_OPTIONS[cs.frameIdx].label); return;
        case S_REC_FRAMESIZE: snprintf(b,n,"%s",FRAME_OPTIONS[cs.recFrameIdx].label); return;
        case S_WC_FRAMESIZE:  snprintf(b,n,"%s",FRAME_OPTIONS[cs.wcFrameIdx].label); return;
        case S_QUALITY:       snprintf(b,n,"%d",cs.quality);     return;
        case S_BRIGHTNESS:    snprintf(b,n,"%+d",cs.brightness); return;
        case S_CONTRAST:      snprintf(b,n,"%+d",cs.contrast);   return;
        case S_SATURATION:    snprintf(b,n,"%+d",cs.saturation); return;
        case S_HMIRROR:       snprintf(b,n,"%s",cs.hmirror?"ON":"OFF"); return;
        case S_VFLIP:         snprintf(b,n,"%s",cs.vflip  ?"ON":"OFF"); return;
        case S_AWB:           snprintf(b,n,"%s",cs.awb    ?"ON":"OFF"); return;
        case S_WB_MODE:       snprintf(b,n,"%s",WB_NAMES[cs.wb_mode%5]); return;
        case S_AEC:           snprintf(b,n,"%s",cs.aec    ?"ON":"OFF"); return;
        case S_AEC_VALUE:     snprintf(b,n,"%d",cs.aec_value); return;
        case S_AGC:           snprintf(b,n,"%s",cs.agc    ?"ON":"OFF"); return;
        case S_AGC_GAIN:      snprintf(b,n,"%d",cs.agc_gain); return;
        case S_LENC:          snprintf(b,n,"%s",cs.lenc   ?"ON":"OFF"); return;
        case S_RAW_GMA:       snprintf(b,n,"%s",cs.raw_gma?"ON":"OFF"); return;
        case S_BPC:           snprintf(b,n,"%s",cs.bpc    ?"ON":"OFF"); return;
        case S_WPC:           snprintf(b,n,"%s",cs.wpc    ?"ON":"OFF"); return;
        default:              snprintf(b,n,"?"); return;
    }
}

// ─── EEPROM ───────────────────────────────────────────────────────
void ui_eeprom_load(CamSettings& cs) {
    EEPROM.begin(128);
    if (EEPROM.read(EEPROM_ADDR_MAGIC) != EEPROM_MAGIC) {
        Serial.println("[CFG] No saved settings, using defaults");
        cs = cam_defaults();
        return;
    }
    EEPROM.get(EEPROM_ADDR_CAM, cs);
    if (cs.frameIdx    >= FRAME_OPTIONS_COUNT) cs.frameIdx    = 5;
    if (cs.recFrameIdx >= FRAME_OPTIONS_COUNT) cs.recFrameIdx = 5;
    if (cs.wcFrameIdx  >= FRAME_OPTIONS_COUNT) cs.wcFrameIdx  = 5;
    if (cs.quality < 4 || cs.quality > 63)    cs.quality      = 10;
    if (cs.wb_mode > 4)                        cs.wb_mode      = 0;
    if (cs.aec_value < 0 || cs.aec_value > 1200) cs.aec_value = 300;
    if (cs.agc_gain > 30)                      cs.agc_gain     = 0;
    Serial.println("[CFG] Settings loaded from EEPROM");
}

void ui_eeprom_save(const CamSettings& cs) {
    EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
    EEPROM.put(EEPROM_ADDR_CAM, cs);
    EEPROM.commit();
    Serial.println("[CFG] Settings saved to EEPROM");
}

void ui_init(TFT_eSPI& tft, TFT_eSprite& spFeed, TFT_eSprite& spMenu) {
    tft.init();
    tft.setRotation(0);
    tft.fillScreen(C_BG);
    spFeed.setColorDepth(16);
    spFeed.createSprite(DISP_W, FEED_H);
    spFeed.fillSprite(TFT_BLACK);
    spMenu.setColorDepth(16);
    spMenu.createSprite(DISP_W, MENU_H);
    spMenu.fillSprite(C_BG);
    tft.fillRect(0, DIV_Y, DISP_W, DIV_H, C_DIVIDER);
}

void ui_draw_feed_overlay(TFT_eSprite& sp, const UIState& ui) {
    sp.setTextColor(C_ACCENT2, TFT_TRANSPARENT);
    sp.setTextSize(1);
    sp.setCursor(3,3);
    sp.print("LIVE");
    if (ui.recording) {
        uint32_t elapsed = (millis() - ui.recStartMs) / 1000;
        bool blink = ((millis()/500)&1);
        sp.fillCircle(10, FEED_H-10, 5, blink ? C_RED : C_DKGREY);
        sp.setTextColor(blink?C_RED:C_GREY, TFT_TRANSPARENT);
        sp.setCursor(20, FEED_H-14);
        char rbuf[12];
        snprintf(rbuf,sizeof(rbuf),"REC %02lu:%02lu",elapsed/60,elapsed%60);
        sp.print(rbuf);
    } else if (ui.sdReady) {
        sp.setTextColor(C_GREEN, TFT_TRANSPARENT);
        sp.setCursor(3, FEED_H-12);
        sp.print("SD");
    }
}

// ─── Scroll animation tick ────────────────────────────────────────
void ui_mm_anim_tick(UIState& ui) {
    ScrollAnim& sa = ui.mmScroll;
    if (!sa.animating) return;
    uint32_t now = millis();
    float dt = (now - sa.lastMs) / 1000.0f;
    sa.lastMs = now;
    if (dt > 0.05f) dt = 0.05f;
    float diff  = sa.target - sa.current;
    float force = SPRING_STIFFNESS * diff - SPRING_DAMPING * sa.velocity;
    sa.velocity += force;
    sa.current  += sa.velocity;
    if (fabsf(diff) < ANIM_THRESHOLD && fabsf(sa.velocity) < ANIM_THRESHOLD) {
        sa.current  = sa.target;
        sa.velocity = 0.0f;
        sa.animating= false;
    }
    ui.dirtyMenu = true;
}

static void mm_set_target(UIState& ui) {
    float desired = (float)ui.mmCursor * MM_ITEM_H
                    - (MENU_H / 2.0f - MM_ITEM_H / 2.0f);
    float maxScroll = (float)(MAIN_MENU_COUNT - 1) * MM_ITEM_H
                      - (MENU_H / 2.0f - MM_ITEM_H / 2.0f);
    if (maxScroll < 0) maxScroll = 0;
    if (desired   < 0) desired   = 0;
    if (desired > maxScroll) desired = maxScroll;
    ui.mmScroll.target    = desired;
    ui.mmScroll.animating = true;
    ui.mmScroll.lastMs    = millis();
}

// ─── Main Menu ────────────────────────────────────────────────────
void ui_draw_main_menu(TFT_eSprite& sp, UIState& ui) {
    sp.fillSprite(C_BG);
    sp.fillRect(0, 0, DISP_W, 16, C_PANEL);
    sp.setTextSize(1);
    sp.setTextColor(C_ACCENT2, C_PANEL);
    sp.setCursor(6, 4);
    sp.print("MENU");

    int16_t scrollY = 16;
    int16_t areaH   = MENU_H - scrollY - 14;
    float   offset  = ui.mmScroll.current;

    for (int i = 0; i < MAIN_MENU_COUNT; i++) {
        float itemTop = scrollY + (float)i * MM_ITEM_H - offset;
        float itemBot = itemTop + MM_ITEM_H;
        if (itemBot < scrollY || itemTop > scrollY + areaH) continue;

        int16_t y   = (int16_t)itemTop;
        bool    sel = (i == ui.mmCursor);

        uint16_t bg = sel ? C_CARD : C_BG;
        sp.fillRect(0, y, DISP_W, MM_ITEM_H - 2, bg);
        sp.fillRect(0, y, 4, MM_ITEM_H - 2,
                    sel ? MM_ITEMS[i].iconColor : C_DKGREY);
        sp.fillCircle(12, y + MM_ITEM_H / 2 - 1, 5,
                      sel ? MM_ITEMS[i].iconColor : C_DKGREY);
        sp.setTextColor(sel ? C_WHITE : C_GREY, bg);
        sp.setTextSize(1);
        sp.setCursor(22, y + 7);
        sp.print(MM_ITEMS[i].label);
        sp.setTextColor(sel ? MM_ITEMS[i].iconColor : C_DKGREY, bg);
        sp.setCursor(22, y + 18);
        sp.print(MM_ITEMS[i].sublabel);
        sp.drawFastHLine(0, y + MM_ITEM_H - 2, DISP_W, C_DIVIDER);
    }

    int16_t hintY = MENU_H - 14;
    sp.fillRect(0, hintY, DISP_W, 14, C_PANEL);
    sp.setTextColor(C_DKGREY, C_PANEL);
    sp.setTextSize(1);
    sp.setCursor(4, hintY + 3);
    sp.print("UP/DN  OK:select  HOLD:VF");
    sp.pushSprite(0, MENU_Y);
}

// ─── Camera Settings ──────────────────────────────────────────────
void ui_draw_menu(TFT_eSprite& sp, const CamSettings& cs, const UIState& ui) {
    sp.fillSprite(C_BG);
    sp.fillRect(0, 0, DISP_W, 14, C_PANEL);
    sp.setTextSize(1);
    sp.setTextColor(C_ACCENT2, C_PANEL);
    sp.setCursor(5,3);
    sp.print("CAM SETTINGS");

    char valBuf[16];
    for (uint8_t row = 0; row < MENU_ROWS; row++) {
        uint8_t idx = ui.menuTop + row;
        int16_t y   = 14 + row * ROW_H;
        if (idx >= S_COUNT) break;
        bool isCursor = (idx == ui.menuCursor);
        bool isEdit   = isCursor && ui.editMode;
        uint16_t bg   = isEdit ? C_ACCENT : (isCursor ? C_CARD : C_BG);
        sp.fillRect(0, y, DISP_W, ROW_H-1, bg);
        if (isCursor) sp.fillRect(0, y, 3, ROW_H-1, isEdit?C_ACCENT2:C_ACCENT);
        sp.drawFastHLine(4, y+ROW_H-1, DISP_W-4, C_DIVIDER);
        uint16_t lblCol = isEdit?C_BG:(isCursor?C_WHITE:C_GREY);
        sp.setTextColor(lblCol, bg);
        sp.setCursor(7, y+5);
        sp.print(setting_label((SettingID)idx));
        setting_value_str((SettingID)idx, cs, valBuf, sizeof(valBuf));
        sp.setTextColor(isEdit?C_BG:(isCursor?C_ACCENT:C_ACCENT2), bg);
        sp.setCursor(7, y+17);
        sp.print(valBuf);
        if (isEdit) {
            sp.setTextColor(C_BG, C_ACCENT);
            sp.setCursor(DISP_W-14, y+10);
            sp.print("<>");
        }
    }

    int16_t hintY = 14 + MENU_ROWS * ROW_H;
    sp.fillRect(0, hintY, DISP_W, MENU_H-hintY, C_PANEL);
    sp.setTextColor(C_DKGREY, C_PANEL);
    sp.setTextSize(1);
    sp.setCursor(4, hintY+4);
    sp.print(ui.editMode ? "UP/DN:change  OK:confirm" : "UP/DN:scroll  OK:edit");
    sp.pushSprite(0, MENU_Y);
}

// ─── File List ────────────────────────────────────────────────────
void ui_draw_filelist(TFT_eSprite& sp, const FileList& fl, const UIState& ui) {
    sp.fillSprite(C_BG);
    sp.fillRect(0,0,DISP_W,14,C_PANEL);
    sp.setTextColor(C_ACCENT2,C_PANEL);
    sp.setTextSize(1);
    sp.setCursor(5,3);
    char hdr[24];
    snprintf(hdr,sizeof(hdr),"VIDEO FILES (%d)",fl.count);
    sp.print(hdr);

    if (fl.count==0) {
        sp.setTextColor(C_GREY,C_BG);
        sp.setCursor(10,60);
        sp.print("No AVI files on SD");
    } else {
        for (uint8_t row=0;row<VISIBLE_FILES;row++) {
            uint8_t idx=ui.fileTop+row;
            if (idx>=fl.count) break;
            int16_t y=14+row*ROW_H;
            bool sel=(idx==ui.fileCursor);
            sp.fillRect(0,y,DISP_W,ROW_H-1,sel?C_CARD:C_BG);
            if (sel) sp.fillRect(0,y,3,ROW_H-1,C_GREEN);
            sp.drawFastHLine(4,y+ROW_H-1,DISP_W-4,C_DIVIDER);
            sp.setTextColor(sel?C_WHITE:C_GREY,sel?C_CARD:C_BG);
            sp.setCursor(7,y+5);
            char name[22]; strncpy(name,fl.names[idx],21); name[21]='\0';
            sp.print(name);
            if (sel) {
                sp.setTextColor(C_GREEN,C_CARD);
                sp.setCursor(DISP_W-24,y+5);
                sp.print("PLAY");
            }
        }
    }
    int16_t hintY=14+VISIBLE_FILES*ROW_H;
    sp.fillRect(0,hintY,DISP_W,MENU_H-hintY,C_PANEL);
    sp.setTextColor(C_DKGREY,C_PANEL);
    sp.setTextSize(1);
    sp.setCursor(4,hintY+4);
    sp.print("UP/DN:sel  OK:play  HOLD:back");
    sp.pushSprite(0,MENU_Y);
}

// ─── Playback HUD (video) ─────────────────────────────────────────
void ui_draw_playback_hud(TFT_eSprite& sp, const PlayerState& ps, const UIState& ui) {
    sp.fillSprite(C_BG);
    sp.fillRect(0,0,DISP_W,14,C_PANEL);
    sp.setTextColor(C_ACCENT2,C_PANEL);
    sp.setTextSize(1);
    sp.setCursor(5,3);
    sp.print("PLAYBACK");

    // Filename
    sp.setTextColor(C_LTGREY,C_BG);
    sp.setCursor(5,18);
    char name[22]; strncpy(name,ps.filename,21); name[21]='\0';
    sp.print(name);

    // Progress bar
    uint16_t barW=DISP_W-10, barY=36;
    sp.drawRect(5,barY,barW,7,C_DKGREY);
    if (ps.totalFrames>0) {
        uint16_t filled=(uint16_t)((uint32_t)barW*ps.currentFrame/ps.totalFrames);
        sp.fillRect(6,barY+1,filled,5,C_GREEN);
    }

    // Frame counter
    sp.setTextColor(C_GREY,C_BG);
    sp.setCursor(5,48);
    char fbuf[32];
    snprintf(fbuf,sizeof(fbuf),"%lu/%lu fr",ps.currentFrame,ps.totalFrames);
    sp.print(fbuf);

    float fps=ps.microsPerFrame>0?1e6f/ps.microsPerFrame:0;
    snprintf(fbuf,sizeof(fbuf),"%dx%d  %.0ffps",ps.vidW,ps.vidH,fps);
    sp.setCursor(5,60);
    sp.print(fbuf);

    // Status badge
    int16_t stY=72;
    uint16_t stCol = (ps.state==PLAY_DONE)?C_DKGREY:(ui.playPaused?C_ORANGE:C_GREEN);
    sp.fillRect(0,stY,DISP_W,14,C_CARD);
    sp.fillRect(0,stY,3,14,stCol);
    sp.setTextColor(stCol,C_CARD);
    sp.setCursor(8,stY+3);
    if (ps.state==PLAY_DONE)      sp.print("DONE");
    else if (ui.playPaused)       sp.print("PAUSED  OK:resume  DN:stop");
    else                          sp.print("PLAYING  OK:pause  DN:stop");

    // Hint
    sp.fillRect(0,MENU_H-14,DISP_W,14,C_PANEL);
    sp.setTextColor(C_DKGREY,C_PANEL);
    sp.setTextSize(1);
    sp.setCursor(4,MENU_H-10);
    sp.print("OK:pause  DN:stop  HOLD:menu");
    sp.pushSprite(0,MENU_Y);
}

// ─── ESP-NOW Screen ──────────────────────────────────────────────
void ui_draw_espnow_screen(TFT_eSprite& sp, const UIState& ui,
                            uint8_t streamState, uint32_t fps, uint32_t framesSent,
                            uint32_t bytesSent, const uint8_t* receiverMAC,
                            uint32_t avgFrameKB, uint32_t droppedFrames) {
    sp.fillSprite(C_BG);
    sp.fillRect(0,0,DISP_W,14,C_PANEL);
    sp.setTextColor(0xF81F,C_PANEL);
    sp.setTextSize(1);
    sp.setCursor(5,3);
    sp.print("ESP-NOW STREAM");

    const char* stateStr[] = {"IDLE","SEARCHING...","CONNECTED","ERROR"};
    uint16_t stateCol[]    = {C_DKGREY, C_ACCENT, C_GREEN, C_RED};
    uint8_t  si = streamState < 4 ? streamState : 3;

    sp.setTextColor(stateCol[si],C_BG);
    sp.setTextSize(1);
    sp.setCursor(5,18);
    sp.print(stateStr[si]);

    if (streamState == 2 && receiverMAC) {
        char mac[20];
        snprintf(mac,sizeof(mac),"%02X:%02X:%02X:%02X:%02X:%02X",
                 receiverMAC[0],receiverMAC[1],receiverMAC[2],
                 receiverMAC[3],receiverMAC[4],receiverMAC[5]);
        sp.setTextColor(C_GREY,C_BG);
        sp.setCursor(5,30);
        sp.print(mac);
    }

    char buf[32];
    snprintf(buf,sizeof(buf),"FPS: %lu  Avg: %lu KB",fps,avgFrameKB);
    sp.setTextColor(C_ACCENT2,C_BG);
    sp.setCursor(5,44);
    sp.print(buf);

    snprintf(buf,sizeof(buf),"Frames: %lu",framesSent);
    sp.setCursor(5,56);
    sp.print(buf);

    snprintf(buf,sizeof(buf),"Dropped: %lu",droppedFrames);
    sp.setTextColor(droppedFrames>0?C_RED:C_GREY,C_BG);
    sp.setCursor(5,68);
    sp.print(buf);

    snprintf(buf,sizeof(buf),"TX: %lu KB",bytesSent/1024);
    sp.setTextColor(C_GREY,C_BG);
    sp.setCursor(5,80);
    sp.print(buf);

    sp.fillRect(0,MENU_H-26,DISP_W,26,C_CARD);
    sp.setTextColor(C_WHITE,C_CARD);
    sp.setCursor(5,MENU_H-22);
    sp.print(streamState==0||streamState==3?"OK:Start stream":"OK:Stop stream");
    sp.setTextColor(C_DKGREY,C_CARD);
    sp.setCursor(5,MENU_H-11);
    sp.print("HOLD:back to menu");
    sp.pushSprite(0,MENU_Y);
}

// ─── Navigation ───────────────────────────────────────────────────
bool ui_nav_up(CamSettings& cs, UIState& ui, FileList& fl) {
    switch (ui.screen) {
        case SCR_MAIN_MENU:
            if (ui.mmCursor > 0) {
                ui.mmCursor--;
                mm_set_target(ui);
                ui.dirtyMenu = true;
            }
            break;

        case SCR_SETTINGS:
            if (ui.editMode) {
                bool reinit = false;
                switch ((SettingID)ui.menuCursor) {
                    case S_FRAMESIZE:     { int8_t n=(int8_t)cs.frameIdx+1; if(n<(int8_t)FRAME_OPTIONS_COUNT){cs.frameIdx=n;reinit=true;} break; }
                    case S_REC_FRAMESIZE: { int8_t n=(int8_t)cs.recFrameIdx+1; if(n<(int8_t)FRAME_OPTIONS_COUNT)cs.recFrameIdx=n; break; }
                    case S_WC_FRAMESIZE:  { int8_t n=(int8_t)cs.wcFrameIdx+1; if(n<(int8_t)FRAME_OPTIONS_COUNT)cs.wcFrameIdx=n; break; }
                    case S_QUALITY:    cs.quality    =constrain(cs.quality-1,4,63); break;
                    case S_BRIGHTNESS: cs.brightness =constrain(cs.brightness+1,-2,2); break;
                    case S_CONTRAST:   cs.contrast   =constrain(cs.contrast+1,-2,2); break;
                    case S_SATURATION: cs.saturation =constrain(cs.saturation+1,-2,2); break;
                    case S_HMIRROR:    cs.hmirror  =!cs.hmirror; break;
                    case S_VFLIP:      cs.vflip    =!cs.vflip; break;
                    case S_AWB:        cs.awb      =!cs.awb; break;
                    case S_WB_MODE:    cs.wb_mode  =(cs.wb_mode+1)%5; break;
                    case S_AEC:        cs.aec      =!cs.aec; break;
                    case S_AEC_VALUE:  cs.aec_value=constrain(cs.aec_value+20,0,1200); break;
                    case S_AGC:        cs.agc      =!cs.agc; break;
                    case S_AGC_GAIN:   cs.agc_gain =constrain(cs.agc_gain+1,0,30); break;
                    case S_LENC:       cs.lenc     =!cs.lenc; break;
                    case S_RAW_GMA:    cs.raw_gma  =!cs.raw_gma; break;
                    case S_BPC:        cs.bpc      =!cs.bpc; break;
                    case S_WPC:        cs.wpc      =!cs.wpc; break;
                    default: break;
                }
                ui.dirtyMenu = true;
                return reinit;
            } else {
                if (ui.menuCursor > 0) {
                    ui.menuCursor--;
                    if (ui.menuCursor < ui.menuTop) ui.menuTop = ui.menuCursor;
                    ui.dirtyMenu = true;
                }
            }
            break;

        case SCR_FILES:
            if (ui.fileCursor > 0) {
                ui.fileCursor--;
                if (ui.fileCursor < ui.fileTop) ui.fileTop = ui.fileCursor;
                ui.dirtyMenu = true;
            }
            break;

        default: break;
    }
    return false;
}

bool ui_nav_down(CamSettings& cs, UIState& ui, FileList& fl) {
    switch (ui.screen) {
        case SCR_MAIN_MENU:
            if (ui.mmCursor < MAIN_MENU_COUNT - 1) {
                ui.mmCursor++;
                mm_set_target(ui);
                ui.dirtyMenu = true;
            }
            break;

        case SCR_SETTINGS:
            if (ui.editMode) {
                bool reinit = false;
                switch ((SettingID)ui.menuCursor) {
                    case S_FRAMESIZE:     { int8_t n=(int8_t)cs.frameIdx-1; if(n>=0){cs.frameIdx=(uint8_t)n;reinit=true;} break; }
                    case S_REC_FRAMESIZE: { int8_t n=(int8_t)cs.recFrameIdx-1; if(n>=0)cs.recFrameIdx=(uint8_t)n; break; }
                    case S_WC_FRAMESIZE:  { int8_t n=(int8_t)cs.wcFrameIdx-1; if(n>=0)cs.wcFrameIdx=(uint8_t)n; break; }
                    case S_QUALITY:    cs.quality    =constrain(cs.quality+1,4,63); break;
                    case S_BRIGHTNESS: cs.brightness =constrain(cs.brightness-1,-2,2); break;
                    case S_CONTRAST:   cs.contrast   =constrain(cs.contrast-1,-2,2); break;
                    case S_SATURATION: cs.saturation =constrain(cs.saturation-1,-2,2); break;
                    case S_HMIRROR:    cs.hmirror  =!cs.hmirror; break;
                    case S_VFLIP:      cs.vflip    =!cs.vflip; break;
                    case S_AWB:        cs.awb      =!cs.awb; break;
                    case S_WB_MODE:    cs.wb_mode  =(cs.wb_mode+4)%5; break;
                    case S_AEC:        cs.aec      =!cs.aec; break;
                    case S_AEC_VALUE:  cs.aec_value=constrain(cs.aec_value-20,0,1200); break;
                    case S_AGC:        cs.agc      =!cs.agc; break;
                    case S_AGC_GAIN:   cs.agc_gain =constrain(cs.agc_gain-1,0,30); break;
                    case S_LENC:       cs.lenc     =!cs.lenc; break;
                    case S_RAW_GMA:    cs.raw_gma  =!cs.raw_gma; break;
                    case S_BPC:        cs.bpc      =!cs.bpc; break;
                    case S_WPC:        cs.wpc      =!cs.wpc; break;
                    default: break;
                }
                ui.dirtyMenu = true;
                return reinit;
            } else {
                if (ui.menuCursor < S_COUNT-1) {
                    ui.menuCursor++;
                    if (ui.menuCursor >= ui.menuTop + MENU_ROWS)
                        ui.menuTop = ui.menuCursor - MENU_ROWS + 1;
                    ui.dirtyMenu = true;
                }
            }
            break;

        case SCR_FILES:
            if (fl.count > 0 && ui.fileCursor < fl.count-1) {
                ui.fileCursor++;
                if (ui.fileCursor >= ui.fileTop + VISIBLE_FILES)
                    ui.fileTop = ui.fileCursor - VISIBLE_FILES + 1;
                ui.dirtyMenu = true;
            }
            break;

        default: break;
    }
    return false;
}

bool ui_nav_ok(CamSettings& cs, UIState& ui) {
    if (ui.screen == SCR_SETTINGS) {
        ui.editMode  = !ui.editMode;
        ui.dirtyMenu = true;
    }
    return false;
}

// ─── Main Menu select ─────────────────────────────────────────────
Screen ui_mm_select(const UIState& ui) {
    if (ui.mmCursor < MAIN_MENU_COUNT) {
        return MM_ITEMS[ui.mmCursor].target;
    }
    return SCR_MAIN_MENU;
}

// ─── Audio idle screen (used before first pump call) ──────────────
void ui_draw_audio_idle(TFT_eSprite& sp) {
    sp.fillSprite(C_BG);
    sp.fillRect(0, 0, DISP_W, 14, C_PANEL);
    sp.setTextColor(C_ACCENT2, C_PANEL);
    sp.setTextSize(1);
    sp.setCursor(5, 3);
    sp.print("OK:record  HOLD:exit");
    sp.setTextColor(C_WHITE, C_BG);
    sp.setTextSize(2);
    sp.setCursor(14, 24);
    sp.print("AUDIO REC");
    sp.setTextSize(1);
    sp.setTextColor(C_GREY, C_BG);
    sp.setCursor(14, 46);
    sp.print("Press OK to start");
    sp.setCursor(14, 58);
    sp.print("Files saved as WAV");
    sp.pushSprite(0, MENU_Y);
}

// ─── QR Code Reader screen ────────────────────────────────────────
void ui_draw_qr_screen(TFT_eSprite& sp, const UIState& ui,
                       const char* qrData, bool scanning, bool saved) {
    sp.fillSprite(C_BG);
    sp.fillRect(0, 0, DISP_W, 14, C_PANEL);
    sp.setTextColor(0xFFE0, C_PANEL);
    sp.setTextSize(1);
    sp.setCursor(5, 3);
    sp.print("QR CODE READER");

    if (scanning) {
        sp.setTextColor(C_ACCENT2, C_BG);
        sp.setTextSize(1);
        sp.setCursor(10, 20);
        sp.print("Scanning...");
        // Draw scan frame corners
        uint16_t fx = 24, fy = 32, fs = 60;
        uint16_t cornerLen = 10;
        uint16_t cc = 0xFFE0;
        sp.drawFastHLine(fx, fy, cornerLen, cc);
        sp.drawFastVLine(fx, fy, cornerLen, cc);
        sp.drawFastHLine(fx+fs-cornerLen, fy, cornerLen, cc);
        sp.drawFastVLine(fx+fs, fy, cornerLen, cc);
        sp.drawFastHLine(fx, fy+fs, cornerLen, cc);
        sp.drawFastVLine(fx, fy+fs-cornerLen, cornerLen, cc);
        sp.drawFastHLine(fx+fs-cornerLen, fy+fs, cornerLen, cc);
        sp.drawFastVLine(fx+fs, fy+fs-cornerLen, cornerLen, cc);
        bool blink = ((millis()/400)&1);
        if (blink) sp.drawFastHLine(fx+1, fy + (fs/2), fs-1, C_RED);
        sp.setTextColor(C_DKGREY, C_BG);
        sp.setCursor(4, MENU_H - 24);
        sp.print("Point camera at QR code");
    } else if (qrData && qrData[0] != '\0') {
        sp.fillRect(0, 16, DISP_W, 14, C_CARD);
        sp.setTextColor(C_GREEN, C_CARD);
        sp.setCursor(5, 19);
        sp.print(saved ? "SAVED  OK:scan again" : "FOUND  OK:save");

        sp.setTextColor(C_ACCENT2, C_BG);
        sp.setCursor(5, 34);
        bool isUrl = (strncmp(qrData, "http", 4) == 0);
        sp.print(isUrl ? "URL:" : "DATA:");

        sp.setTextColor(C_WHITE, C_BG);
        char lineBuf[22];
        uint8_t lineY = 44;
        uint8_t srcPos = 0;
        uint8_t srcLen = (uint8_t)strlen(qrData);
        for (uint8_t line = 0; line < 3 && srcPos < srcLen; line++) {
            uint8_t copyLen = srcLen - srcPos;
            if (copyLen > 21) copyLen = 21;
            strncpy(lineBuf, qrData + srcPos, copyLen);
            lineBuf[copyLen] = '\0';
            sp.setCursor(5, lineY);
            sp.print(lineBuf);
            srcPos += copyLen;
            lineY  += 11;
        }
        if (srcPos < srcLen) {
            sp.setTextColor(C_DKGREY, C_BG);
            sp.setCursor(5, lineY);
            sp.print("...(truncated)");
        }

        if (saved) {
            sp.setTextColor(C_GREEN, C_BG);
            sp.setCursor(5, MENU_H - 24);
            sp.print("Saved to /QR/ on SD");
        }
    } else {
        sp.setTextColor(C_GREY, C_BG);
        sp.setTextSize(1);
        sp.setCursor(10, 24);
        sp.print("No QR found yet");
        sp.setCursor(10, 36);
        sp.print("Press OK to scan");
    }

    sp.fillRect(0, MENU_H - 14, DISP_W, 14, C_PANEL);
    sp.setTextColor(C_DKGREY, C_PANEL);
    sp.setTextSize(1);
    sp.setCursor(4, MENU_H - 10);
    if (scanning)
        sp.print("Scanning...  HOLD:back");
    else if (qrData && qrData[0] != '\0' && !saved)
        sp.print("OK:save  HOLD:back");
    else if (saved)
        sp.print("OK:scan again  HOLD:back");
    else
        sp.print("OK:start scan  HOLD:back");
    sp.pushSprite(0, MENU_Y);
}

// ─── USB Webcam Screen ──────────────────────────────────────────────
void ui_draw_usb_webcam(TFT_eSprite& sp, const UIState& ui, bool isStreaming) {
    sp.fillSprite(C_BG);
    sp.fillRect(0,0,DISP_W,14,C_PANEL);
    sp.setTextColor(0x07FF,C_PANEL);
    sp.setTextSize(1);
    sp.setCursor(5,3);
    sp.print("USB WEBCAM MODE");

    sp.setTextColor(C_WHITE,C_BG);
    sp.setTextSize(1);
    sp.setCursor(10,24);
    sp.print(isStreaming ? "STREAMING TO PC" : "IDLE");

    if (isStreaming) {
        sp.setTextColor(C_ACCENT2,C_BG);
        sp.setCursor(10,40);
        sp.print("Run pc_webcam.py script");
        bool blink = ((millis()/400)&1);
        sp.fillCircle(14, 58, 4, blink ? C_RED : C_DKGREY);
        sp.setTextColor(blink?C_RED:C_GREY,C_BG);
        sp.setCursor(24, 54);
        sp.print("TX Active");
    } else {
        sp.setTextColor(C_GREY,C_BG);
        sp.setCursor(10,40);
        sp.print("Press OK to start stream");
    }

    sp.fillRect(0,MENU_H-14,DISP_W,14,C_PANEL);
    sp.setTextColor(C_DKGREY,C_PANEL);
    sp.setTextSize(1);
    sp.setCursor(4,MENU_H-10);
    sp.print(isStreaming ? "OK:stop  HOLD:back" : "OK:start  HOLD:back");
    sp.pushSprite(0,MENU_Y);
}

