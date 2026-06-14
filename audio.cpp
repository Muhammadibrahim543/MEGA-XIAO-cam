// ══════════════════════════════════════════════════════════════════
//  audio.cpp
//  Standalone audio recording — PDM mic, WAV to SD
//  Uses ESP_I2S (new IDF API) — avoids GDMA conflict with camera.
//  Camera MUST be deinitialized before calling audio_mic_init().
//
//  ESP_I2S.h is included here (not in audio.h) to prevent typedef
//  conflicts with driver/i2s.h used by sd_player.h.
// ══════════════════════════════════════════════════════════════════

// ESP_I2S.h MUST come first — before any other header that could
// pull in driver/i2s.h — so the new-API typedefs win.
#include "ESP_I2S.h"
#include "audio.h"
#include "ui_settings.h"   // for color palette (C_BG, C_PANEL, etc.)
#include "sd_recorder.h"   // for SD_CS, SD_MOSI, SD_MISO, SD_SCK, spiSD
#include "display.h"
#include <string.h>
#include <stdio.h>

// ─── ESP_I2S instance for PDM microphone ─────────────────────────
static I2SClass i2sMic;
static bool     s_micReady = false;

// ─── audio_mic_init ───────────────────────────────────────────────
bool audio_mic_init() {
    if (s_micReady) return true;

    Serial.println("[AUDIO] Initializing PDM microphone (ESP_I2S)...");

    // XIAO ESP32-S3 Sense PDM pins: CLK=42, DATA=41
    // setPinsPdmRx(clk, data) — CLK is first argument
    i2sMic.setPinsPdmRx(MIC_CLK_PIN, MIC_DATA_PIN);

    // bufferSize=4096: large DMA buffer prevents underrun during SD writes
    if (!i2sMic.begin(I2S_MODE_PDM_RX, MIC_SAMPLE_RATE,
                      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO,
                      4096)) {
        Serial.println("[AUDIO] PDM mic initialization failed!");
        return false;
    }

    s_micReady = true;
    Serial.printf("[AUDIO] PDM mic ready — %d Hz Mono 16-bit (buf=4096)\n", MIC_SAMPLE_RATE);
    return true;
}

// ─── audio_mic_deinit ─────────────────────────────────────────────
void audio_mic_deinit() {
    if (!s_micReady) return;
    i2sMic.end();
    s_micReady = false;
    Serial.println("[AUDIO] PDM mic deinitialized");
}

// ─── Next file name on SD ─────────────────────────────────────────
static void next_wav_name(char* out, size_t len) {
    for (uint16_t n = 1; n < 10000; n++) {
        snprintf(out, len, "/AUD_%04u.WAV", n);
        if (!SD.exists(out)) return;
    }
    snprintf(out, len, "/AUD_ERR.WAV");
}

// ─── Write WAV header (called twice: placeholder + final) ─────────
static void write_wav_header(FsFile& f, uint32_t dataBytes) {
    WavHeader h = {};
    memcpy(h.riff,  "RIFF", 4);
    h.fileSize      = dataBytes + WAV_HEADER_SIZE - 8;
    memcpy(h.wave,  "WAVE", 4);
    memcpy(h.fmt,   "fmt ", 4);
    h.fmtSize       = 16;
    h.audioFormat   = 1;                           // PCM
    h.numChannels   = MIC_CHANNELS;
    h.sampleRate    = MIC_SAMPLE_RATE;
    h.bitsPerSample = MIC_BITS;
    h.blockAlign    = (MIC_CHANNELS * MIC_BITS) / 8;
    h.byteRate      = MIC_SAMPLE_RATE * h.blockAlign;
    memcpy(h.data,  "data", 4);
    h.dataSize      = dataBytes;
    f.write((const uint8_t*)&h, sizeof(h));
}

// ─── audio_start ──────────────────────────────────────────────────
bool audio_start(AudioState& as) {
    if (as.active) return false;

    char path[32];
    next_wav_name(path, sizeof(path));

    as.file = SD.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!as.file) {
        Serial.printf("[AUDIO] Cannot open %s\n", path);
        return false;
    }

    // Write placeholder header — rewritten with correct size on stop
    write_wav_header(as.file, 0);

    // Store filename without leading '/'
    strncpy(as.fileName, path + 1, sizeof(as.fileName) - 1);
    as.fileName[sizeof(as.fileName) - 1] = '\0';

    as.active       = true;
    as.startMs      = millis();
    as.bytesWritten = 0;

    Serial.printf("[AUDIO] Recording → %s\n", path);
    return true;
}

// ─── audio_pump ───────────────────────────────────────────────────
bool audio_pump(AudioState& as) {
    if (!as.active || !s_micReady) return true;

    static int16_t buf[AUDIO_READ_BYTES / sizeof(int16_t)];

    // Read with retry: readBytes() may return fewer bytes than requested
    // if the DMA buffer hasn't filled yet. Loop until we get a full chunk
    // or the buffer is genuinely empty (gap in mic data).
    size_t bytesWant  = AUDIO_READ_BYTES;
    size_t bytesTotal = 0;
    const uint8_t MAX_RETRIES = 8;

    for (uint8_t retry = 0; retry < MAX_RETRIES && bytesTotal < bytesWant; retry++) {
        size_t got = i2sMic.readBytes((char*)((uint8_t*)buf + bytesTotal),
                                       bytesWant - bytesTotal);
        if (got == 0) break;
        bytesTotal += got;
    }

    // Debug: first 20 pumps + every 10th thereafter
    static int dbgCount = 0;
    if (dbgCount < 20 || dbgCount % 10 == 0) {
        Serial.printf("[AUDIO DBG] pump#%d bytesRead=%u/%u\n",
                      dbgCount, (unsigned)bytesTotal, (unsigned)bytesWant);
    }
    dbgCount++;

    if (bytesTotal == 0) return true;

    // Apply volume gain with saturation clamp
    size_t samples = bytesTotal / sizeof(int16_t);
    for (size_t i = 0; i < samples; i++) {
        int32_t s = (int32_t)buf[i] << MIC_VOLUME_GAIN;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }

    size_t written = as.file.write((const uint8_t*)buf, bytesTotal);
    if (written != bytesTotal) {
        Serial.println("[AUDIO] SD write error");
        return false;
    }
    as.bytesWritten += written;
    return true;
}

// ─── audio_stop ───────────────────────────────────────────────────
void audio_stop(AudioState& as) {
    if (!as.active) return;

    // Rewrite header with correct data size
    as.file.seek(0);
    write_wav_header(as.file, as.bytesWritten);
    as.file.close();

    as.active = false;
    Serial.printf("[AUDIO] Saved %s  (%lu bytes PCM)\n",
                  as.fileName, (unsigned long)as.bytesWritten);
}

// ─── audio_read_mic ───────────────────────────────────────────────
size_t audio_read_mic(int16_t* buf, size_t maxSamples) {
    if (!s_micReady) return 0;
    
    // Read whatever is available in the DMA buffer
    size_t bytesWant = maxSamples * sizeof(int16_t);
    size_t got = i2sMic.readBytes((char*)buf, bytesWant);
    
    if (got > 0) {
        size_t samples = got / sizeof(int16_t);
        for (size_t i = 0; i < samples; i++) {
            int32_t s = (int32_t)buf[i] << MIC_VOLUME_GAIN;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            buf[i] = (int16_t)s;
        }
    }
    return got;
}

// ─── audio_draw_ui ────────────────────────────────────────────────
void audio_draw_ui(TFT_eSprite& sp, const AudioState& as) {
    sp.fillSprite(C_BG);

    // ── Title bar ───────────────────────────────────────────────
    sp.fillRect(0, 0, DISP_W, 14, C_PANEL);
    sp.setTextColor(C_ACCENT2, C_PANEL);
    sp.setTextSize(1);
    sp.setCursor(5, 3);
    sp.print(as.active ? "OK:stop    HOLD:exit" : "OK:record  HOLD:exit");

    if (!as.active) {
        // ── Idle state ──────────────────────────────────────────
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
    } else {
        // ── Recording state ─────────────────────────────────────
        uint32_t secs    = (millis() - as.startMs) / 1000;
        uint32_t minutes = secs / 60;
        uint32_t seconds = secs % 60;
        uint32_t kbytes  = as.bytesWritten / 1024;

        // Blink indicator
        bool blink = ((millis() / 400) & 1);
        sp.fillCircle(10, 28, 5, blink ? C_RED : C_DKGREY);

        sp.setTextColor(C_RED, C_BG);
        sp.setTextSize(2);
        sp.setCursor(22, 20);
        sp.print("REC");

        // Timer
        char tbuf[12];
        snprintf(tbuf, sizeof(tbuf), "%02lu:%02lu", minutes, seconds);
        sp.setTextColor(C_WHITE, C_BG);
        sp.setTextSize(2);
        sp.setCursor(70, 20);
        sp.print(tbuf);

        // File name
        sp.setTextSize(1);
        sp.setTextColor(C_ACCENT2, C_BG);
        sp.setCursor(5, 44);
        sp.print(as.fileName);

        // Size
        char sbuf[20];
        snprintf(sbuf, sizeof(sbuf), "Size: %lu KB", (unsigned long)kbytes);
        sp.setTextColor(C_GREY, C_BG);
        sp.setCursor(5, 58);
        sp.print(sbuf);

        // Simple level bar
        uint8_t barW = (uint8_t)(((secs % 10) * (DISP_W - 16)) / 9);
        sp.drawRect(5, 74, DISP_W - 10, 8, C_DKGREY);
        sp.fillRect(6, 75, barW, 6, blink ? C_RED : C_ACCENT);
    }

    sp.pushSprite(0, MENU_Y);
}
