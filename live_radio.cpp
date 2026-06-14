// ESP_I2S.h must be included before any header that may indirectly pull
// in legacy I2S types.
#include "ESP_I2S.h"
#include "live_radio.h"
#include "audio.h"
#include "display.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

namespace {

constexpr uint8_t  LR_MAGIC              = 0xAD;
constexpr uint32_t LR_SAMPLE_RATE        = 16000;
constexpr uint16_t LR_CHUNK_SAMPLES      = 120;
constexpr uint16_t LR_CHUNK_BYTES        = LR_CHUNK_SAMPLES * sizeof(int16_t);
constexpr uint32_t LR_RX_IDLE_TIMEOUT_MS = 180;
constexpr uint8_t  LR_NUM_CHANNELS       = 4;
constexpr bool     LR_ENABLE_AMP         = false;

static const uint8_t LR_CHANNEL_PEER[LR_NUM_CHANNELS][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01},
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02},
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x03},
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x04},
};

static const char* const LR_CHANNEL_NAME[LR_NUM_CHANNELS] = {
    "ALPHA",
    "BRAVO",
    "CHARLIE",
    "DELTA",
};

#pragma pack(push, 1)
struct LRPacket {
    uint8_t  magic;
    uint8_t  channel;
    uint8_t  seq;
    uint8_t  total;
    uint16_t sampleRate;
    uint16_t numSamples;
    int16_t  samples[LR_CHUNK_SAMPLES];
};
#pragma pack(pop)

static I2SClass       s_mic;
static bool           s_entered      = false;
static bool           s_peerAdded    = false;
static uint8_t        s_seq          = 0;
static LiveRadioStats s_stats        = {};

static uint16_t ui_color_for_channel(uint8_t channel) {
    static const uint16_t kAccents[LR_NUM_CHANNELS] = {
        0x07FF, 0xFD20, 0xA15F, 0xFA18
    };
    return (channel < LR_NUM_CHANNELS) ? kAccents[channel] : 0x07FF;
}

static const char* state_label(LiveRadioState st) {
    switch (st) {
        case LRADIO_TX:    return "TX";
        case LRADIO_RX:    return "RX";
        case LRADIO_ERROR: return "ERR";
        default:           return "IDLE";
    }
}

static bool mic_init() {
    if (s_stats.micReady) return true;

    s_mic.setPinsPdmRx(MIC_CLK_PIN, MIC_DATA_PIN);
    if (!s_mic.begin(I2S_MODE_PDM_RX, LR_SAMPLE_RATE,
                     I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, 4096)) {
        s_stats.state = LRADIO_ERROR;
        return false;
    }

    s_stats.micReady = true;
    return true;
}

static void mic_deinit() {
    if (!s_stats.micReady) return;
    s_mic.end();
    s_stats.micReady = false;
}

static void unregister_peers() {
    for (uint8_t i = 0; i < LR_NUM_CHANNELS; i++) {
        if (esp_now_is_peer_exist(LR_CHANNEL_PEER[i])) {
            esp_now_del_peer(LR_CHANNEL_PEER[i]);
        }
    }
    s_peerAdded = false;
}

static bool register_peer(uint8_t channel) {
    if (!s_stats.espNowReady || channel >= LR_NUM_CHANNELS) return false;

    unregister_peers();

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, LR_CHANNEL_PEER[channel], sizeof(peer.peer_addr));
    peer.channel = 0;
    peer.encrypt = false;

    if (esp_now_add_peer(&peer) != ESP_OK) {
        return false;
    }

    s_peerAdded = true;
    return true;
}

static int16_t average_level_from_samples(const int16_t* samples, uint16_t count) {
    if (!samples || count == 0) return 0;

    int32_t acc = 0;
    for (uint16_t i = 0; i < count; i++) {
        int32_t v = samples[i];
        acc += (v < 0) ? -v : v;
    }
    return (int16_t)(acc / count);
}

static void on_sent(const uint8_t* mac, esp_now_send_status_t status) {
    (void)mac;
    if (status != ESP_NOW_SEND_SUCCESS) {
        s_stats.state = LRADIO_ERROR;
    }
}

static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (!s_entered || len < (int)sizeof(LRPacket)) return;

    const LRPacket* pkt = reinterpret_cast<const LRPacket*>(data);
    if (pkt->magic != LR_MAGIC) return;
    if (pkt->channel != s_stats.channel) return;
    if (s_stats.state == LRADIO_TX) return;

    s_stats.state    = LRADIO_RX;
    s_stats.lastRxMs = millis();
    s_stats.rxPackets++;
    s_stats.rxBytes += pkt->numSamples * sizeof(int16_t);
    s_stats.level    = average_level_from_samples(pkt->samples, pkt->numSamples);
    if (info && info->rx_ctrl) {
        s_stats.rssi = info->rx_ctrl->rssi;
    }
}

static void reset_runtime_state() {
    LiveRadioState keepState = LRADIO_IDLE;
    uint8_t keepChannel = s_stats.channel;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.state      = keepState;
    s_stats.channel    = keepChannel;
    s_stats.ampEnabled = LR_ENABLE_AMP;
    s_stats.cameraOff  = true;
    s_stats.rssi       = 0;
}

}  // namespace

bool live_radio_enter() {
    if (s_entered) return true;

    uint8_t keepChannel = s_stats.channel % LR_NUM_CHANNELS;
    reset_runtime_state();
    s_stats.channel = keepChannel;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        s_stats.state = LRADIO_ERROR;
        return false;
    }

    esp_now_register_send_cb(on_sent);
    esp_now_register_recv_cb(on_recv);
    s_stats.espNowReady = true;

    if (!register_peer(s_stats.channel)) {
        live_radio_leave();
        s_stats.state = LRADIO_ERROR;
        return false;
    }

    s_entered = true;
    return true;
}

void live_radio_leave() {
    live_radio_stop_tx();

    s_entered         = false;
    s_stats.state     = LRADIO_IDLE;
    s_stats.level     = 0;
    s_stats.cameraOff = false;
}

void live_radio_shutdown_stack() {
    if (s_entered) return;
    if (!s_stats.espNowReady) return;

    delay(20);
    unregister_peers();
    delay(10);
    esp_now_unregister_send_cb();
    esp_now_unregister_recv_cb();
    delay(10);
    esp_now_deinit();
    s_stats.espNowReady = false;
    WiFi.disconnect();
}

void live_radio_start_tx() {
    if (!s_entered || s_stats.state == LRADIO_TX) return;
    if (!mic_init()) return;

    s_seq               = 0;
    s_stats.state       = LRADIO_TX;
    s_stats.txStartedMs = millis();
    s_stats.level       = 0;
}

void live_radio_stop_tx() {
    if (s_stats.state == LRADIO_TX) {
        mic_deinit();
        s_stats.state = LRADIO_IDLE;
        s_stats.level = 0;
    } else {
        mic_deinit();
    }
}

void live_radio_next_channel() {
    if (!s_entered || s_stats.state == LRADIO_TX) return;
    s_stats.channel = (s_stats.channel + 1) % LR_NUM_CHANNELS;
    register_peer(s_stats.channel);
}

void live_radio_prev_channel() {
    if (!s_entered || s_stats.state == LRADIO_TX) return;
    s_stats.channel = (s_stats.channel + LR_NUM_CHANNELS - 1) % LR_NUM_CHANNELS;
    register_peer(s_stats.channel);
}

void live_radio_select_action() {
    if (!s_entered) return;
    if (s_stats.state == LRADIO_ERROR) {
        s_stats.state = LRADIO_IDLE;
    }
}

void live_radio_tick() {
    if (!s_entered) return;

    uint32_t now = millis();

    if (s_stats.state == LRADIO_TX && s_stats.micReady && s_peerAdded) {
        for (uint8_t packetsThisTick = 0; packetsThisTick < 4; packetsThisTick++) {
            int16_t pcm[LR_CHUNK_SAMPLES] = {};
            size_t  bytesTotal = 0;

            for (uint8_t retry = 0; retry < 3 && bytesTotal < LR_CHUNK_BYTES; retry++) {
                size_t got = s_mic.readBytes((char*)((uint8_t*)pcm + bytesTotal),
                                             LR_CHUNK_BYTES - bytesTotal);
                if (got == 0) break;
                bytesTotal += got;
            }

            if (bytesTotal == 0) break;

            uint16_t samplesRead = (uint16_t)(bytesTotal / sizeof(int16_t));
            for (uint16_t i = 0; i < samplesRead; i++) {
                int32_t v = (int32_t)pcm[i] * 3;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                pcm[i] = (int16_t)v;
            }

            LRPacket pkt = {};
            pkt.magic      = LR_MAGIC;
            pkt.channel    = s_stats.channel;
            pkt.seq        = s_seq++;
            pkt.total      = 0;
            pkt.sampleRate = LR_SAMPLE_RATE;
            pkt.numSamples = samplesRead;
            memcpy(pkt.samples, pcm, samplesRead * sizeof(int16_t));

            if (esp_now_send(LR_CHANNEL_PEER[s_stats.channel],
                             reinterpret_cast<const uint8_t*>(&pkt),
                             sizeof(pkt)) == ESP_OK) {
                s_stats.txPackets++;
                s_stats.txBytes += samplesRead * sizeof(int16_t);
                s_stats.level = average_level_from_samples(pcm, samplesRead);
            }

            if (samplesRead < LR_CHUNK_SAMPLES) break;
        }
    } else if (s_stats.state == LRADIO_RX) {
        if (now - s_stats.lastRxMs > LR_RX_IDLE_TIMEOUT_MS) {
            s_stats.state = LRADIO_IDLE;
        }
    }

    if (s_stats.state == LRADIO_IDLE) {
        s_stats.level = (int16_t)(s_stats.level * 7 / 8);
    }
}

const LiveRadioStats& live_radio_stats() {
    return s_stats;
}

void live_radio_draw(TFT_eSprite& spFeed, TFT_eSprite& spMenu, uint32_t ms) {
    const LiveRadioStats& st = live_radio_stats();
    const uint16_t accent = (st.state == LRADIO_TX) ? 0xF800 :
                            (st.state == LRADIO_RX) ? 0x07E0 :
                            (st.state == LRADIO_ERROR) ? 0xFC00 :
                            ui_color_for_channel(st.channel);
    const uint16_t bg     = 0x0841;
    const uint16_t panel  = 0x1082;
    const uint16_t card   = 0x18C3;
    const uint16_t dim    = 0x39E7;
    const uint16_t white  = 0xFFFF;
    const uint16_t grey   = 0xBDF7;
    const uint16_t black  = 0x0000;

    spFeed.fillSprite(black);
    spFeed.fillRect(0, 0, DISP_W, 18, panel);
    spFeed.fillRect(0, 17, DISP_W, 1, accent);
    spFeed.setTextSize(1);
    spFeed.setTextColor(accent, panel);
    spFeed.setCursor(6, 5);
    spFeed.print("LIVE RADIO");

    spFeed.drawRoundRect(10, 28, DISP_W - 20, FEED_H - 40, 8, accent);
    spFeed.fillRoundRect(16, 34, DISP_W - 32, 26, 6, 0x0000);
    spFeed.setTextColor(white, black);
    spFeed.setTextSize(2);
    spFeed.drawString(state_label(st.state), (DISP_W / 2) - 18, 40);

    spFeed.setTextSize(1);
    spFeed.setTextColor(accent, black);
    spFeed.drawString(LR_CHANNEL_NAME[st.channel], 16, 74);

    spFeed.setTextColor(grey, black);
    spFeed.drawString("CAMERA OFF", 16, 96);
    spFeed.drawString("MIC ON ONLY DURING TX", 16, 110);
    spFeed.drawString("AMPLIFIER DISABLED", 16, 124);

    int barX = 16;
    int barY = FEED_H - 26;
    int barW = DISP_W - 32;
    int fillW = map(constrain((int)st.level, 0, 6000), 0, 6000, 0, barW - 2);
    spFeed.drawRect(barX, barY, barW, 10, dim);
    if (fillW > 0) {
        spFeed.fillRect(barX + 1, barY + 1, fillW, 8, accent);
    }
    if (st.state == LRADIO_TX) {
        bool blink = ((ms / 220) & 1U) != 0;
        spFeed.fillCircle(DISP_W - 12, 9, 4, blink ? accent : dim);
    }
    spFeed.pushSprite(0, FEED_Y);

    spMenu.fillSprite(bg);
    spMenu.fillRect(0, 0, DISP_W, 14, panel);
    spMenu.setTextColor(accent, panel);
    spMenu.setTextSize(1);
    spMenu.setCursor(5, 3);
    spMenu.print("UP:PTT  DN:CH  OK:INFO  HOLD:EXIT");

    spMenu.fillRoundRect(6, 20, DISP_W - 12, 28, 5, card);
    spMenu.setTextColor(white, card);
    spMenu.setCursor(12, 28);
    spMenu.print("CH ");
    spMenu.print(st.channel);
    spMenu.print("  ");
    spMenu.print(LR_CHANNEL_NAME[st.channel]);

    spMenu.setTextColor(accent, bg);
    spMenu.setCursor(8, 60);
    spMenu.print("State:");
    spMenu.setTextColor(white, bg);
    spMenu.setCursor(56, 60);
    spMenu.print(state_label(st.state));

    spMenu.setTextColor(accent, bg);
    spMenu.setCursor(8, 74);
    spMenu.print("TX:");
    spMenu.setTextColor(grey, bg);
    spMenu.setCursor(32, 74);
    spMenu.print(st.txPackets);
    spMenu.print(" pkt  ");
    spMenu.print(st.txBytes / 1024);
    spMenu.print(" KB");

    spMenu.setTextColor(accent, bg);
    spMenu.setCursor(8, 88);
    spMenu.print("RX:");
    spMenu.setTextColor(grey, bg);
    spMenu.setCursor(32, 88);
    spMenu.print(st.rxPackets);
    spMenu.print(" pkt  ");
    spMenu.print(st.rxBytes / 1024);
    spMenu.print(" KB");

    spMenu.setTextColor(accent, bg);
    spMenu.setCursor(8, 102);
    spMenu.print("RSSI:");
    spMenu.setTextColor(grey, bg);
    spMenu.setCursor(44, 102);
    if (st.rssi != 0) spMenu.print(st.rssi);
    else              spMenu.print("---");

    spMenu.setTextColor(accent, bg);
    spMenu.setCursor(8, 116);
    spMenu.print("MIC:");
    spMenu.setTextColor(grey, bg);
    spMenu.setCursor(36, 116);
    spMenu.print(st.micReady ? "READY" : "OFF");
    spMenu.setCursor(92, 116);
    spMenu.setTextColor(grey, bg);
    spMenu.print("AMP:");
    spMenu.setCursor(122, 116);
    spMenu.print(st.ampEnabled ? "ON" : "OFF");

    if (st.state == LRADIO_TX && st.txStartedMs > 0) {
        uint32_t secs = (ms - st.txStartedMs) / 1000;
        spMenu.setTextColor(accent, bg);
        spMenu.setCursor(8, 130);
        spMenu.print("PTT:");
        spMenu.setTextColor(white, bg);
        spMenu.setCursor(38, 130);
        spMenu.print(secs);
        spMenu.print(" s");
    } else if (st.lastRxMs > 0) {
        uint32_t ago = (ms - st.lastRxMs) / 1000;
        spMenu.setTextColor(accent, bg);
        spMenu.setCursor(8, 130);
        spMenu.print("LAST RX:");
        spMenu.setTextColor(white, bg);
        spMenu.setCursor(58, 130);
        spMenu.print(ago);
        spMenu.print(" s ago");
    }

    spMenu.pushSprite(0, MENU_Y);
}
