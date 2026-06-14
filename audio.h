#pragma once
// ══════════════════════════════════════════════════════════════════
//  audio.h
//  Standalone audio recording module for XIAO ESP32-S3 Sense
//  PDM mic: CLK=42, DATA=41 — uses ESP_I2S (new IDF API)
//  Records 16-bit PCM WAV to SD card — camera must be deinitialized
//  before entering this module.
//
//  NOTE: ESP_I2S.h is intentionally NOT included here.
//        It is included only in audio.cpp so that this header
//        remains safe to include from any translation unit.
// ══════════════════════════════════════════════════════════════════
#include <Arduino.h>
#include <SdFat.h>
extern SdFs SD;
#include "display.h"

// ─── Microphone Pins (XIAO ESP32-S3 Sense built-in PDM mic) ──────
#ifndef MIC_CLK_PIN
#define MIC_CLK_PIN    42   // PDM CLK
#endif
#ifndef MIC_DATA_PIN
#define MIC_DATA_PIN   41   // PDM DATA
#endif

// NOTE: MIC_I2S_PORT removed — ESP_I2S manages port assignment
//       internally and correctly routes PDM to I2S0.

// ─── Audio parameters ─────────────────────────────────────────────
#ifndef MIC_SAMPLE_RATE
#define MIC_SAMPLE_RATE  16000
#endif
#ifndef MIC_BITS
#define MIC_BITS         16
#endif
#ifndef MIC_CHANNELS
#define MIC_CHANNELS     1
#endif
#define MIC_WAV_HEAD     44
#define MIC_VOLUME_GAIN  3
#define AUDIO_READ_BYTES 2048

// ─── WAV header (44 bytes) ────────────────────────────────────────
#pragma pack(push, 1)
struct WavHeader {
    char     riff[4];          // "RIFF"
    uint32_t fileSize;         // total file size - 8
    char     wave[4];          // "WAVE"
    char     fmt[4];           // "fmt "
    uint32_t fmtSize;          // 16
    uint16_t audioFormat;      // 1 = PCM
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char     data[4];          // "data"
    uint32_t dataSize;
};
#pragma pack(pop)

#define WAV_HEADER_SIZE  44

// ─── Audio recorder state ─────────────────────────────────────────
struct AudioState {
    bool     active;           // currently recording
    FsFile   file;
    uint32_t startMs;
    uint32_t bytesWritten;     // PCM bytes written (excludes header)
    char     fileName[32];     // e.g. "AUD_0001.WAV"
};

// ─── Public API ───────────────────────────────────────────────────

// Init / deinit PDM mic via ESP_I2S
bool audio_mic_init();
void audio_mic_deinit();

// Start a new WAV recording to SD; fills state.fileName
bool audio_start(AudioState& as);

// Pump — call repeatedly from loop() while active; reads DMA and
// writes PCM to SD. Returns false on write error.
bool audio_pump(AudioState& as);

// Stop recording, finalise WAV header, close file.
void audio_stop(AudioState& as);

// Read raw PCM data directly from the mic (for streaming)
// Returns number of bytes read
size_t audio_read_mic(int16_t* buf, size_t maxSamples);

// Draw the audio recording screen (panel area below divider)
// Pass spMenu sprite (DISP_W × MENU_H).
void audio_draw_ui(TFT_eSprite& sp, const AudioState& as);
