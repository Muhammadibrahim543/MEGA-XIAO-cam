#pragma once
// ══════════════════════════════════════════════════════════════════
//  sd_player.h
//  Plays MJPEG+PCM AVI from SD card
//  Audio output: MAX98357A amplifier (BCLK=7, DIN=8, LRC=9)
//  Speaker uses ESP_I2S (new IDF API) — same as mic modules.
// ══════════════════════════════════════════════════════════════════
#include <Arduino.h>
#include <SdFat.h>
extern SdFs SD;
#include "camera_config.h"

// ─── Speaker Pins (MAX98357A) ────────────────────────────────────
#define SPK_BCLK        7
#define SPK_DOUT        8
#define SPK_LRC         9
#define SPK_SAMPLE_RATE 16000

// ─── File list ────────────────────────────────────────────────────
#define MAX_FILES  64

struct FileList {
    char    names[MAX_FILES][32];
    uint8_t count;
    uint8_t selected;
};

// ─── Player State ─────────────────────────────────────────────────
enum PlayState { PLAY_IDLE, PLAY_PLAYING, PLAY_PAUSED, PLAY_DONE };

struct PlayerState {
    PlayState state;
    FsFile    file;
    char      filename[32];

    uint32_t  moviOffset;
    uint32_t  moviEnd;
    uint32_t  totalFrames;
    uint32_t  currentFrame;
    uint32_t  microsPerFrame;
    uint16_t  vidW, vidH;
    uint16_t  decodedW, decodedH;

    uint32_t  audioRate;
    uint16_t  audioBits;
    uint16_t  audioChannels;

    uint32_t  lastFrameUs;
    bool      audioReady;
};

// ─── Public API ───────────────────────────────────────────────────
bool  player_spk_init();
void  audio_spk_deinit();   // called from main .ino on closePlayback
void  player_spk_write(const int16_t* pcm, size_t samples);

void  player_scan_files(FileList& fl);
bool  player_open(PlayerState& ps, const char* path);
void  player_close(PlayerState& ps);

// Returns true when a video frame was decoded
bool  player_step(PlayerState& ps, uint8_t* outRGB565, size_t bufSize);
