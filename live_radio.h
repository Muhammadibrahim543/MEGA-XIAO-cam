#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

enum LiveRadioState {
    LRADIO_IDLE = 0,
    LRADIO_TX,
    LRADIO_RX,
    LRADIO_ERROR
};

struct LiveRadioStats {
    LiveRadioState state;
    uint8_t  channel;
    uint32_t txPackets;
    uint32_t rxPackets;
    uint32_t txBytes;
    uint32_t rxBytes;
    uint32_t txStartedMs;
    uint32_t lastRxMs;
    int      rssi;
    int16_t  level;
    bool     micReady;
    bool     espNowReady;
    bool     ampEnabled;
    bool     cameraOff;
};

bool live_radio_enter();
void live_radio_leave();
void live_radio_shutdown_stack();
void live_radio_tick();

void live_radio_start_tx();
void live_radio_stop_tx();
void live_radio_next_channel();
void live_radio_prev_channel();
void live_radio_select_action();

const LiveRadioStats& live_radio_stats();
void live_radio_draw(TFT_eSprite& spFeed, TFT_eSprite& spMenu, uint32_t ms);
