// ══════════════════════════════════════════════════════════════════
//  sd_player.cpp
//  AVI playback with self-contained speaker output (MAX98357A)
//  Speaker uses ESP_I2S (new IDF API) — same API as mic modules.
//  Mixing legacy driver/i2s.h with ESP_I2S causes a runtime abort;
//  using ESP_I2S throughout eliminates that conflict entirely.
// ══════════════════════════════════════════════════════════════════

// ESP_I2S.h MUST be first — before any header that could pull in
// driver/i2s.h — so the new-API typedefs are established first.
#include "ESP_I2S.h"
#include "sd_player.h"
#include "esp_heap_caps.h"
#include "img_converters.h"

// ─── ESP_I2S speaker instance ─────────────────────────────────────
static I2SClass i2sSpk;
static bool     s_spkReady = false;
static uint32_t s_spkRate  = 0;

// ─── Speaker init/deinit — self-contained ─────────────────────────
bool player_spk_init() {
    // No-op at boot — GPIO 7/8/9 shared with SD SPI.
    // Speaker is lazily initialised in player_open() after SD is done.
    return true;
}

static bool spk_init_with_rate(uint32_t rate) {
    if (s_spkReady && s_spkRate == rate) return true;
    if (s_spkReady) {
        i2sSpk.end();
        s_spkReady = false;
    }

    // MAX98357A: standard I2S TX, mono left channel
    i2sSpk.setPins(SPK_BCLK, SPK_LRC, SPK_DOUT, -1, -1); // bclk, ws, dout, din, mck

    if (!i2sSpk.begin(I2S_MODE_STD, (uint32_t)rate,
                      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
        Serial.printf("[PLY] Speaker init failed at %lu Hz\n", rate);
        return false;
    }

    s_spkReady = true;
    s_spkRate  = rate;
    Serial.printf("[PLY] Speaker ready — %lu Hz Mono 16-bit\n", rate);
    return true;
}

void audio_spk_deinit() {
    if (!s_spkReady) return;
    i2sSpk.end();
    s_spkReady = false;
    s_spkRate  = 0;
    Serial.println("[PLY] Speaker deinitialized");
}

void player_spk_write(const int16_t* pcm, size_t samples) {
    if (!s_spkReady || !pcm || samples == 0) return;
    i2sSpk.write((uint8_t*)pcm, samples * sizeof(int16_t));
}

// ─── File scanner ─────────────────────────────────────────────────
void player_scan_files(FileList& fl) {
    fl.count = 0;
    FsFile root = SD.open("/");
    if (!root) return;
    while (fl.count < MAX_FILES) {
        FsFile f = root.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            char name[64];
            f.getName(name, sizeof(name));
            size_t len = strlen(name);
            if (len > 4 && strcasecmp(name + len - 4, ".avi") == 0) {
                strncpy(fl.names[fl.count], name, 31);
                fl.names[fl.count][31] = '\0';
                fl.count++;
            }
        }
        f.close();
    }
    root.close();

    // Sort descending (newest first)
    for (uint8_t i = 0; i < fl.count; i++) {
        for (uint8_t j = i + 1; j < fl.count; j++) {
            if (strcasecmp(fl.names[j], fl.names[i]) > 0) {
                char tmp[32];
                strncpy(tmp, fl.names[i], sizeof(tmp));
                tmp[31] = '\0';
                strncpy(fl.names[i], fl.names[j], sizeof(fl.names[i]));
                fl.names[i][31] = '\0';
                strncpy(fl.names[j], tmp, sizeof(fl.names[j]));
                fl.names[j][31] = '\0';
            }
        }
    }
    Serial.printf("[PLY] Found %d AVI files\n", fl.count);
}

// ─── AVI header parser helpers ────────────────────────────────────
static bool read32(FsFile& f, uint32_t& v) { return f.read((uint8_t*)&v, 4) == 4; }
static bool readTag(FsFile& f, char* tag)  { return f.read((uint8_t*)tag, 4) == 4; }

#define PLAYER_ALLOW_FRAME_SKIP 0

static jpg_scale_t playback_scale_for(uint16_t w, uint16_t h,
                                       uint16_t& outW, uint16_t& outH) {
    uint8_t div = 1;
    jpg_scale_t scale = JPG_SCALE_NONE;
    if (w >= 800 || h >= 600) {
        div = 8; scale = JPG_SCALE_8X;
    } else if (w >= DISP_W * 3 && h >= FEED_H * 3) {
        div = 4; scale = JPG_SCALE_4X;
    } else if (w >= DISP_W * 2 && h >= FEED_H * 2) {
        div = 2; scale = JPG_SCALE_2X;
    }
    outW = (w + div - 1) / div;
    outH = (h + div - 1) / div;
    return scale;
}

static bool player_scan_avi_headers(PlayerState& ps) {
    ps.file.seek(12);
    bool currentStreamIsVideo = false;
    char tag[5] = {};

    while (ps.file.available() &&
           (uint32_t)ps.file.position() + 8 <= ps.file.size()) {
        uint32_t chunkStart = ps.file.position();
        uint32_t size = 0;
        if (!readTag(ps.file, tag)) break;
        if (!read32(ps.file, size)) break;

        uint32_t dataStart = ps.file.position();
        uint32_t next = dataStart + size + (size & 1);
        if (next <= chunkStart || next > ps.file.size() + 1) break;

        if (memcmp(tag, "LIST", 4) == 0) {
            char ltype[5] = {};
            if (!readTag(ps.file, ltype)) break;
            if (memcmp(ltype, "movi", 4) == 0) {
                ps.moviOffset = ps.file.position();
                ps.moviEnd    = dataStart + size;
                return ps.vidW > 0;
            }
            if (memcmp(ltype, "hdrl", 4) == 0 ||
                memcmp(ltype, "strl", 4) == 0) continue;
            ps.file.seek(next);
            continue;
        }

        if (memcmp(tag, "avih", 4) == 0 && size >= 20) {
            uint32_t mspf = 0;
            read32(ps.file, mspf);
            ps.microsPerFrame = mspf;
            ps.file.seek(ps.file.position() + 12);
            read32(ps.file, ps.totalFrames);
        } else if (memcmp(tag, "strh", 4) == 0 && size >= 8) {
            char fccType[5] = {};
            ps.file.read((uint8_t*)fccType, 4);
            currentStreamIsVideo = (memcmp(fccType, "vids", 4) == 0);
        } else if (memcmp(tag, "strf", 4) == 0 && size >= 16) {
            if (currentStreamIsVideo) {
                uint32_t biSz = 0;
                read32(ps.file, biSz);
                if (biSz >= 40) {
                    int32_t w = 0, h = 0;
                    ps.file.read((uint8_t*)&w, 4);
                    ps.file.read((uint8_t*)&h, 4);
                    ps.vidW = (uint16_t)w;
                    ps.vidH = (uint16_t)(h < 0 ? -h : h);
                }
            } else {
                uint16_t fmt = 0, ch = 0;
                uint32_t sr = 0;
                ps.file.read((uint8_t*)&fmt, 2);
                ps.file.read((uint8_t*)&ch, 2);
                ps.file.read((uint8_t*)&sr, 4);
                ps.audioRate     = sr;
                ps.audioChannels = ch;
            }
        }
        ps.file.seek(next);
    }
    return ps.moviOffset != 0 && ps.vidW != 0;
}

bool player_open(PlayerState& ps, const char* path) {
    ps.file = SD.open(path, O_RDONLY);
    if (!ps.file) return false;
    strncpy(ps.filename, path, 31);

    char tag[5] = {};
    uint32_t size;
    readTag(ps.file, tag); if (memcmp(tag,"RIFF",4)!=0) { ps.file.close(); return false; }
    read32(ps.file, size);
    readTag(ps.file, tag); if (memcmp(tag,"AVI ",4)!=0) { ps.file.close(); return false; }

    ps.microsPerFrame = 66666;
    ps.vidW = ps.vidH = 0;
    ps.decodedW = ps.decodedH = 0;
    ps.audioRate = SPK_SAMPLE_RATE;
    ps.audioBits = 16;
    ps.audioChannels = 1;
    ps.totalFrames = 0;
    ps.moviOffset = 0;
    ps.moviEnd    = 0;

    // Walk top-level chunks
    while (ps.file.available()) {
        if (!readTag(ps.file, tag)) break;
        if (!read32(ps.file, size)) break;

        if (memcmp(tag,"LIST",4)==0) {
            char ltype[5]={};
            readTag(ps.file, ltype);
            if (memcmp(ltype,"hdrl",4)==0) {
                char inner[5]={};
                uint32_t isz;
                while ((uint32_t)ps.file.position() < ps.file.size()) {
                    if (!readTag(ps.file,inner)) break;
                    if (!read32(ps.file,isz)) break;
                    if (memcmp(inner,"avih",4)==0) {
                        uint32_t mspf; read32(ps.file,mspf);
                        ps.microsPerFrame = mspf;
                        ps.file.seek(ps.file.position()+4);
                        ps.file.seek(ps.file.position()+4);
                        ps.file.seek(ps.file.position()+4);
                        uint32_t tf; read32(ps.file,tf);
                        ps.totalFrames = tf;
                        ps.file.seek(ps.file.position() + isz - 20);
                    } else if (memcmp(inner,"strf",4)==0) {
                        uint32_t biSz; read32(ps.file,biSz);
                        if (biSz >= 40) {
                            int32_t w,h;
                            ps.file.read((uint8_t*)&w,4);
                            ps.file.read((uint8_t*)&h,4);
                            ps.vidW=(uint16_t)w;
                            ps.vidH=(uint16_t)(h<0?-h:h);
                            ps.file.seek(ps.file.position()+isz-4-8);
                        } else if (biSz==18||biSz==16) {
                            ps.file.seek(ps.file.position()-4);
                            uint16_t fmt; ps.file.read((uint8_t*)&fmt,2);
                            uint16_t ch;  ps.file.read((uint8_t*)&ch,2);
                            uint32_t sr;  ps.file.read((uint8_t*)&sr,4);
                            ps.audioRate=sr; ps.audioChannels=ch;
                            ps.file.seek(ps.file.position()+isz-8);
                        } else {
                            ps.file.seek(ps.file.position()+isz-4);
                        }
                    } else {
                        ps.file.seek(ps.file.position()+isz);
                        if (isz&1) ps.file.seek(ps.file.position()+1);
                    }
                    if (memcmp(inner,"LIST",4)==0||memcmp(inner,"avih",4)==0) break;
                }
            } else if (memcmp(ltype,"movi",4)==0) {
                ps.moviOffset = ps.file.position();
                ps.moviEnd    = ps.moviOffset + size - 4;
                break;
            } else {
                ps.file.seek(ps.file.position()+size-4);
            }
        } else {
            if (size&1) size++;
            ps.file.seek(ps.file.position()+size);
        }
    }

    if (ps.moviOffset==0||ps.vidW==0) player_scan_avi_headers(ps);
    if (ps.moviOffset==0||ps.vidW==0) {
        Serial.println("[PLY] AVI parse failed");
        ps.file.close();
        return false;
    }

    // Init speaker at the AVI's audio rate
    audio_spk_deinit();
    bool spkOk = spk_init_with_rate(ps.audioRate > 0 ? ps.audioRate : SPK_SAMPLE_RATE);

    ps.file.seek(ps.moviOffset);
    playback_scale_for(ps.vidW, ps.vidH, ps.decodedW, ps.decodedH);
    ps.currentFrame = 0;
    ps.state        = PLAY_PLAYING;
    ps.lastFrameUs  = micros();
    ps.audioReady   = spkOk;

    Serial.printf("[PLY] Opened %s  %dx%d  %.0ffps  %lu frames  audio=%s\n",
        path, ps.vidW, ps.vidH, 1e6 / ps.microsPerFrame,
        ps.totalFrames, spkOk ? "YES" : "NO");
    return true;
}

void player_close(PlayerState& ps) {
    if (ps.file) ps.file.close();
    ps.state = PLAY_IDLE;
}

// ─── Step: decode one frame ───────────────────────────────────────
bool player_step(PlayerState& ps, uint8_t* outRGB565, size_t bufSize) {
    if (ps.state != PLAY_PLAYING && ps.state != PLAY_PAUSED) return false;
    if (ps.state == PLAY_PAUSED) return false;

    uint32_t elapsed   = micros() - ps.lastFrameUs;
    uint32_t framesDue = elapsed / ps.microsPerFrame;
    if (framesDue == 0) return false;

#if PLAYER_ALLOW_FRAME_SKIP
    uint32_t framesToSkip = (framesDue > 1) ? framesDue - 1 : 0;
    if (framesToSkip > 5) framesToSkip = 5;
#else
    uint32_t framesToSkip = 0;
#endif
    ps.lastFrameUs += framesDue * ps.microsPerFrame;

    if ((uint32_t)ps.file.position() >= ps.moviEnd) {
        ps.state = PLAY_DONE;
        return false;
    }

    char     tag[5] = {};
    uint32_t csize;
    bool     gotVideo = false;

    while ((uint32_t)ps.file.position() < ps.moviEnd) {
        if (!readTag(ps.file, tag)) break;
        if (!read32(ps.file, csize)) break;

        if (memcmp(tag, "00dc", 4) == 0) {
            // JPEG video frame
            if (framesToSkip > 0) {
                ps.file.seek(ps.file.position() + csize + (csize & 1));
                ps.currentFrame++;
                framesToSkip--;
                continue;
            }
            uint16_t outW = 0, outH = 0;
            jpg_scale_t scale = playback_scale_for(ps.vidW, ps.vidH, outW, outH);
            size_t needed = (size_t)outW * outH * 2;
            if (csize > 0 && needed <= bufSize) {
                static uint8_t* jpegBuf   = nullptr;
                static size_t   jpegBufSz = 0;
                if (csize > jpegBufSz) {
                    if (jpegBuf) heap_caps_free(jpegBuf);
                    jpegBuf   = (uint8_t*)heap_caps_malloc(csize + 16, MALLOC_CAP_SPIRAM);
                    jpegBufSz = jpegBuf ? csize + 16 : 0;
                }
                if (jpegBuf) {
                    ps.file.read(jpegBuf, csize);
                    bool ok = jpg2rgb565(jpegBuf, csize, outRGB565, scale);
                    if (ok) {
                        ps.decodedW = outW;
                        ps.decodedH = outH;
                        gotVideo    = true;
                    }
                } else {
                    ps.file.seek(ps.file.position() + csize);
                }
            } else {
                ps.file.seek(ps.file.position() + csize);
            }
            if (csize & 1) ps.file.seek(ps.file.position() + 1);
            ps.currentFrame++;
            if (gotVideo) break;

        } else if (memcmp(tag, "01wb", 4) == 0) {
            // PCM audio chunk
            if (ps.audioReady && csize > 0) {
                static int16_t* audioBuf   = nullptr;
                static size_t   audioBufSz = 0;
                size_t samples = csize / sizeof(int16_t);
                if (csize > audioBufSz) {
                    if (audioBuf) heap_caps_free(audioBuf);
                    audioBuf   = (int16_t*)heap_caps_malloc(csize + 4, MALLOC_CAP_SPIRAM);
                    audioBufSz = audioBuf ? csize + 4 : 0;
                }
                if (audioBuf) {
                    ps.file.read((uint8_t*)audioBuf, csize);
                    player_spk_write(audioBuf, samples);
                } else {
                    ps.file.seek(ps.file.position() + csize);
                }
            } else {
                ps.file.seek(ps.file.position() + csize);
            }
            if (csize & 1) ps.file.seek(ps.file.position() + 1);

        } else if (memcmp(tag, "idx1", 4) == 0) {
            ps.state = PLAY_DONE;
            break;
        } else {
            if (csize & 1) csize++;
            ps.file.seek(ps.file.position() + csize);
        }
    }

    if ((uint32_t)ps.file.position() >= ps.moviEnd) ps.state = PLAY_DONE;
    return gotVideo;
}
