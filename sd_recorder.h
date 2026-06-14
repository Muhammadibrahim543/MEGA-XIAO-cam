#pragma once
// ══════════════════════════════════════════════════════════════════
//  sd_recorder.h
//  Records MJPEG+PCM AVI to SD card with synchronized audio
//  Audio via built-in PDM mic (CLK=42, DATA=41)
//  Uses ESP_I2S (new IDF API) to avoid GDMA conflict with camera
//  AVI 1.0 RIFF container with audio/video interleave
// ══════════════════════════════════════════════════════════════════
#include <Arduino.h>
#include <SdFat.h>
extern SdFs SD;
// NOTE: ESP_I2S.h is intentionally NOT included here.
//       It is included only in sd_recorder.cpp to avoid typedef
//       conflicts with driver/i2s.h used by audio.h and sd_player.h.
#include "esp_heap_caps.h"
#include "camera_config.h"

// ─── SD SPI Pins (Xiao ESP32-S3 Sense microSD) ───────────────────
#define SD_CS    21
#define SD_MOSI   9
#define SD_MISO   8
#define SD_SCK    7
#define SD_SPI_FREQ 25000000

extern SPIClass spiSD;

// ─── Microphone Pins (XIAO ESP32-S3 Sense built-in PDM mic) ──────
#define MIC_CLK_PIN    42   // PDM CLK
#define MIC_DATA_PIN   41   // PDM DATA

// NOTE: MIC_I2S_PORT removed — ESP_I2S manages port selection internally
//       and correctly assigns PDM to I2S0 without conflicting with camera.

// ─── Audio parameters ─────────────────────────────────────────────
#define MIC_SAMPLE_RATE  16000
#define MIC_BITS         16
#define MIC_CHANNELS     1

// ─── Recording parameters ─────────────────────────────────────────
#define REC_FPS                  15
#define AUDIO_BUF_MS             66
#define AUDIO_SAMPLES_PER_FRAME  ((MIC_SAMPLE_RATE * AUDIO_BUF_MS) / 1000)

// ─── AVI RIFF helpers ─────────────────────────────────────────────
#pragma pack(push,1)
struct RIFFChunk   { char id[4]; uint32_t size; };
struct AVIMainHdr  {
    uint32_t dwMicroSecPerFrame;
    uint32_t dwMaxBytesPerSec;
    uint32_t dwPaddingGranularity;
    uint32_t dwFlags;
    uint32_t dwTotalFrames;
    uint32_t dwInitialFrames;
    uint32_t dwStreams;
    uint32_t dwSuggestedBufferSize;
    uint32_t dwWidth;
    uint32_t dwHeight;
    uint32_t dwReserved[4];
};
struct AVIStreamHdr {
    char     fccType[4];
    char     fccHandler[4];
    uint32_t dwFlags;
    uint16_t wPriority;
    uint16_t wLanguage;
    uint32_t dwInitialFrames;
    uint32_t dwScale;
    uint32_t dwRate;
    uint32_t dwStart;
    uint32_t dwLength;
    uint32_t dwSuggestedBufferSize;
    uint32_t dwQuality;
    uint32_t dwSampleSize;
    struct { int16_t left,top,right,bottom; } rcFrame;
};
struct BitmapInfoHdr {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};
struct WAVEFormat {
    uint16_t wFormatTag;
    uint16_t nChannels;
    uint32_t nSamplesPerSec;
    uint32_t nAvgBytesPerSec;
    uint16_t nBlockAlign;
    uint16_t wBitsPerSample;
};
#pragma pack(pop)

// ─── Recorder State ───────────────────────────────────────────────
struct RecorderState {
    bool     active;
    FsFile   file;
    uint32_t frameCount;
    uint32_t startMs;
    uint32_t totalVideoBytes;
    uint32_t totalAudioBytes;
    uint32_t moviOffset;
    uint32_t indexOffset;

    // AVI index (stored in PSRAM)
    struct IdxEntry { char id[4]; uint32_t flags; uint32_t offset; uint32_t size; };
    IdxEntry* idx;
    uint32_t  idxCount;
    uint32_t  idxCapacity;

    uint16_t  vidW, vidH;

    // Audio sync state
    bool      audioEnabled;
    int16_t*  audioBuf;    // PSRAM buffer for audio chunk per frame
    size_t    audioBufSz;
};

// ─── Public API ───────────────────────────────────────────────────
bool  recorder_sd_init();
bool  recorder_mic_init();
void  recorder_mic_deinit();
bool  recorder_start(RecorderState& rs, uint16_t vidW, uint16_t vidH,
                     bool withAudio = true);
bool  recorder_add_frame(RecorderState& rs, const uint8_t* jpegBuf, size_t jpegLen);
bool  recorder_add_audio(RecorderState& rs, const int16_t* pcm, size_t samples);
void  recorder_stop(RecorderState& rs);
bool  recorder_read_mic(int16_t* buf, size_t samples, size_t& gotSamples);
