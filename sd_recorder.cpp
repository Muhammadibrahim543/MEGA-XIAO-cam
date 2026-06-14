// ══════════════════════════════════════════════════════════════════
//  sd_recorder.cpp
//  MJPEG+PCM AVI recording with synchronized audio via built-in PDM mic
//  Uses ESP_I2S (new IDF API) — avoids GDMA conflict with camera driver
//  No external audio_system dependency — mic managed internally
// ══════════════════════════════════════════════════════════════════

// ESP_I2S.h MUST be included before sd_recorder.h (and before any
// driver/i2s.h pull-in) so the new-API typedefs are seen first.
// Keeping it here in the .cpp prevents header-level typedef conflicts.
#include "ESP_I2S.h"
#include "sd_recorder.h"
#include <SPI.h>
#include "esp_heap_caps.h"

// ─── ESP_I2S instance for PDM microphone ─────────────────────────
static I2SClass i2sMic;
static bool s_micReady = false;

// ─── Write helpers ────────────────────────────────────────────────
static void write32(FsFile& f, uint32_t v)     { f.write((uint8_t*)&v, 4); }
static void write16(FsFile& f, uint16_t v)     { f.write((uint8_t*)&v, 2); }
static void writeTag(FsFile& f, const char* t) { f.write((const uint8_t*)t, 4); }

// ─── AVI patching offsets (file-scoped) ───────────────────────────
static uint32_t g_riffSizeOff;
static uint32_t g_moviSizeOff;
static uint32_t g_avihTotalFrOff;
static uint32_t g_vstrhLengthOff;

// ─── SD Init ──────────────────────────────────────────────────────
SdFs SD;
SPIClass spiSD(HSPI);

bool recorder_sd_init() {
    spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SdSpiConfig(SD_CS, SHARED_SPI, SD_SPI_FREQ, &spiSD))) {
        Serial.println("[REC] SD init failed");
        return false;
    }
    Serial.printf("[REC] SD ready\n");
    return true;
}

// ─── Microphone Init / Deinit ─────────────────────────────────────
bool recorder_mic_init() {
    if (s_micReady) return true;

    Serial.println("[REC] Initializing PDM microphone (ESP_I2S)...");

    // XIAO ESP32-S3 Sense PDM pins: CLK=42, DATA=41
    // setPinsPdmRx(clk, data) — CLK is first argument
    i2sMic.setPinsPdmRx(MIC_CLK_PIN, MIC_DATA_PIN);

    // bufferSize=8192: large DMA buffer so mic data doesn't overflow
    // while the camera task is busy encoding a JPEG frame.
    if (!i2sMic.begin(I2S_MODE_PDM_RX, MIC_SAMPLE_RATE,
                      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO,
                      8192)) {
        Serial.println("[REC] PDM mic initialization failed!");
        return false;
    }

    s_micReady = true;
    Serial.printf("[REC] PDM mic ready — %d Hz Mono 16-bit (buf=8192)\n", MIC_SAMPLE_RATE);
    return true;
}

void recorder_mic_deinit() {
    if (!s_micReady) return;
    i2sMic.end();
    s_micReady = false;
    Serial.println("[REC] PDM mic deinitialized");
}

bool recorder_read_mic(int16_t* buf, size_t samples, size_t& gotSamples) {
    if (!s_micReady) { gotSamples = 0; return false; }

    // Read with a retry loop: ESP_I2S readBytes() may return fewer bytes
    // than requested if the DMA buffer hasn't filled yet. Keep reading
    // until we have all samples or the buffer is genuinely empty.
    size_t bytesWant  = samples * sizeof(int16_t);
    size_t bytesTotal = 0;
    uint8_t* ptr      = (uint8_t*)buf;
    const uint8_t MAX_RETRIES = 10;

    for (uint8_t retry = 0; retry < MAX_RETRIES && bytesTotal < bytesWant; retry++) {
        size_t got = i2sMic.readBytes((char*)(ptr + bytesTotal),
                                       bytesWant - bytesTotal);
        if (got == 0) break;
        bytesTotal += got;
    }

    gotSamples = bytesTotal / sizeof(int16_t);
    return (gotSamples > 0);
}

// ─── AVI Recording Start ──────────────────────────────────────────
bool recorder_start(RecorderState& rs, uint16_t vidW, uint16_t vidH, bool withAudio) {
    // Generate unique filename in root
    char path[48];
    for (int i = 0; i < 9999; i++) {
        snprintf(path, sizeof(path), "/VID_%04d.avi", i);
        if (!SD.exists(path)) break;
    }

    rs.file = SD.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!rs.file) {
        Serial.printf("[REC] Cannot open %s\n", path);
        return false;
    }

    rs.active          = true;
    rs.frameCount      = 0;
    rs.totalVideoBytes = 0;
    rs.totalAudioBytes = 0;
    rs.vidW            = vidW;
    rs.vidH            = vidH;
    rs.idxCount        = 0;
    rs.idxCapacity     = 4096;
    rs.audioEnabled    = withAudio && s_micReady;

    rs.idx = (RecorderState::IdxEntry*)heap_caps_malloc(
        rs.idxCapacity * sizeof(RecorderState::IdxEntry),
        MALLOC_CAP_SPIRAM);
    if (!rs.idx) {
        Serial.println("[REC] PSRAM alloc for index failed");
        rs.file.close();
        return false;
    }

    // Allocate audio buffer (enough for one frame worth of audio)
    rs.audioBufSz = AUDIO_SAMPLES_PER_FRAME * 2 * sizeof(int16_t);
    rs.audioBuf = (int16_t*)heap_caps_malloc(rs.audioBufSz, MALLOC_CAP_SPIRAM);
    if (!rs.audioBuf) {
        Serial.println("[REC] Audio buffer alloc failed — recording without audio");
        rs.audioEnabled = false;
        rs.audioBufSz = 0;
    }

    uint32_t microsPerFrame = 1000000UL / REC_FPS;

    // ── RIFF header ───────────────────────────────────────────────
    writeTag(rs.file, "RIFF");
    g_riffSizeOff = rs.file.position();
    write32(rs.file, 0);               // patched later
    writeTag(rs.file, "AVI ");

    // ── hdrl LIST ─────────────────────────────────────────────────
    writeTag(rs.file, "LIST");
    uint32_t hdrlSzOff = rs.file.position();
    write32(rs.file, 0);
    writeTag(rs.file, "hdrl");

    // avih
    writeTag(rs.file, "avih");
    write32(rs.file, sizeof(AVIMainHdr));
    AVIMainHdr avih = {};
    avih.dwMicroSecPerFrame    = microsPerFrame;
    avih.dwMaxBytesPerSec      = 250000;
    avih.dwFlags               = 0x10;  // AVIF_HASINDEX
    g_avihTotalFrOff = rs.file.position() + 12;
    avih.dwTotalFrames         = 0;
    avih.dwStreams              = rs.audioEnabled ? 2 : 1;
    avih.dwSuggestedBufferSize = vidW * vidH * 3;
    avih.dwWidth               = vidW;
    avih.dwHeight              = vidH;
    rs.file.write((uint8_t*)&avih, sizeof(avih));

    // ── Video stream strl ─────────────────────────────────────────
    writeTag(rs.file, "LIST");
    uint32_t vstrlSzOff = rs.file.position();
    write32(rs.file, 0);
    writeTag(rs.file, "strl");

    writeTag(rs.file, "strh");
    write32(rs.file, sizeof(AVIStreamHdr));
    AVIStreamHdr vstrh = {};
    memcpy(vstrh.fccType,    "vids", 4);
    memcpy(vstrh.fccHandler, "MJPG", 4);
    vstrh.dwScale              = 1;
    vstrh.dwRate               = REC_FPS;
    g_vstrhLengthOff           = rs.file.position() + 32;
    vstrh.dwLength             = 0;
    vstrh.dwSuggestedBufferSize= vidW * vidH * 3;
    vstrh.dwQuality            = (uint32_t)-1;
    rs.file.write((uint8_t*)&vstrh, sizeof(vstrh));

    writeTag(rs.file, "strf");
    write32(rs.file, sizeof(BitmapInfoHdr));
    BitmapInfoHdr bih = {};
    bih.biSize        = sizeof(BitmapInfoHdr);
    bih.biWidth       = vidW;
    bih.biHeight      = vidH;
    bih.biPlanes      = 1;
    bih.biBitCount    = 24;
    bih.biCompression = 0x47504A4D; // 'MJPG'
    bih.biSizeImage   = vidW * vidH * 3;
    rs.file.write((uint8_t*)&bih, sizeof(bih));

    uint32_t vstrlEnd = rs.file.position();
    rs.file.seek(vstrlSzOff);
    write32(rs.file, vstrlEnd - vstrlSzOff - 4);
    rs.file.seek(vstrlEnd);

    // ── Audio stream strl (only if audio enabled) ─────────────────
    if (rs.audioEnabled) {
        writeTag(rs.file, "LIST");
        uint32_t astrlSzOff = rs.file.position();
        write32(rs.file, 0);
        writeTag(rs.file, "strl");

        writeTag(rs.file, "strh");
        write32(rs.file, sizeof(AVIStreamHdr));
        AVIStreamHdr astrh = {};
        memcpy(astrh.fccType, "auds", 4);
        astrh.dwScale              = 1;
        astrh.dwRate               = MIC_SAMPLE_RATE;
        astrh.dwSuggestedBufferSize= AUDIO_SAMPLES_PER_FRAME * sizeof(int16_t) * 2;
        astrh.dwSampleSize         = sizeof(int16_t) * MIC_CHANNELS;
        rs.file.write((uint8_t*)&astrh, sizeof(astrh));

        writeTag(rs.file, "strf");
        write32(rs.file, sizeof(WAVEFormat));
        WAVEFormat wfmt = {};
        wfmt.wFormatTag      = 1;
        wfmt.nChannels       = MIC_CHANNELS;
        wfmt.nSamplesPerSec  = MIC_SAMPLE_RATE;
        wfmt.wBitsPerSample  = MIC_BITS;
        wfmt.nBlockAlign     = MIC_CHANNELS * (MIC_BITS / 8);
        wfmt.nAvgBytesPerSec = MIC_SAMPLE_RATE * wfmt.nBlockAlign;
        rs.file.write((uint8_t*)&wfmt, sizeof(wfmt));

        uint32_t astrlEnd = rs.file.position();
        rs.file.seek(astrlSzOff);
        write32(rs.file, astrlEnd - astrlSzOff - 4);
        rs.file.seek(astrlEnd);
    }

    // Patch hdrl size
    uint32_t hdrlEnd = rs.file.position();
    rs.file.seek(hdrlSzOff);
    write32(rs.file, hdrlEnd - hdrlSzOff - 4);
    rs.file.seek(hdrlEnd);

    // ── movi LIST ─────────────────────────────────────────────────
    writeTag(rs.file, "LIST");
    g_moviSizeOff = rs.file.position();
    write32(rs.file, 0);
    writeTag(rs.file, "movi");
    rs.moviOffset = rs.file.position() - 4;

    rs.startMs = millis();
    Serial.printf("[REC] Recording → %s  %dx%d  audio=%s\n",
                  path, vidW, vidH, rs.audioEnabled ? "YES" : "NO");
    return true;
}

// ─── Add JPEG video frame ──────────────────────────────────────────
bool recorder_add_frame(RecorderState& rs, const uint8_t* jpegBuf, size_t jpegLen) {
    if (!rs.active) return false;

    // Grow index if needed
    if (rs.idxCount >= rs.idxCapacity) {
        rs.idxCapacity *= 2;
        rs.idx = (RecorderState::IdxEntry*)heap_caps_realloc(
            rs.idx, rs.idxCapacity * sizeof(RecorderState::IdxEntry),
            MALLOC_CAP_SPIRAM);
        if (!rs.idx) return false;
    }

    uint32_t offset = rs.file.position() - rs.moviOffset - 4;

    writeTag(rs.file, "00dc");
    uint32_t padLen = (jpegLen + 1) & ~1u;
    write32(rs.file, (uint32_t)jpegLen);
    rs.file.write(jpegBuf, jpegLen);
    if (padLen > jpegLen) rs.file.write((uint8_t)0);

    auto& e   = rs.idx[rs.idxCount++];
    memcpy(e.id, "00dc", 4);
    e.flags  = 0x10;
    e.offset = offset;
    e.size   = (uint32_t)jpegLen;

    rs.frameCount++;
    rs.totalVideoBytes += jpegLen;

    // Capture and write synchronized audio for this frame
    if (rs.audioEnabled && rs.audioBuf && s_micReady) {
        size_t want     = AUDIO_SAMPLES_PER_FRAME;
        size_t totalGot = 0;
        while (totalGot < want) {
            size_t chunk = 0;
            bool ok = recorder_read_mic(rs.audioBuf + totalGot,
                                        want - totalGot, chunk);
            if (!ok || chunk == 0) break;
            totalGot += chunk;
        }

        if (totalGot > 0) {
            // Apply a gentle gain boost for cleaner voice
            for (size_t i = 0; i < totalGot; i++) {
                int32_t s = (int32_t)rs.audioBuf[i] * 3;
                if (s >  32767) s =  32767;
                if (s < -32768) s = -32768;
                rs.audioBuf[i] = (int16_t)s;
            }
            recorder_add_audio(rs, rs.audioBuf, totalGot);
        }
    }

    return true;
}

// ─── Add PCM audio chunk ──────────────────────────────────────────
bool recorder_add_audio(RecorderState& rs, const int16_t* pcm, size_t samples) {
    if (!rs.active || !rs.audioEnabled || samples == 0) return false;

    // Grow index if needed
    if (rs.idxCount >= rs.idxCapacity) {
        rs.idxCapacity *= 2;
        rs.idx = (RecorderState::IdxEntry*)heap_caps_realloc(
            rs.idx, rs.idxCapacity * sizeof(RecorderState::IdxEntry),
            MALLOC_CAP_SPIRAM);
        if (!rs.idx) return false;
    }

    uint32_t offset = rs.file.position() - rs.moviOffset - 4;
    size_t   bytes  = samples * sizeof(int16_t);
    size_t   padLen = (bytes + 1) & ~1u;

    writeTag(rs.file, "01wb");
    write32(rs.file, (uint32_t)bytes);
    rs.file.write((const uint8_t*)pcm, bytes);
    if (padLen > bytes) rs.file.write((uint8_t)0);

    auto& e = rs.idx[rs.idxCount++];
    memcpy(e.id, "01wb", 4);
    e.flags  = 0;
    e.offset = offset;
    e.size   = (uint32_t)bytes;

    rs.totalAudioBytes += bytes;
    return true;
}

// ─── Stop and finalise ────────────────────────────────────────────
void recorder_stop(RecorderState& rs) {
    if (!rs.active) return;
    rs.active = false;

    uint32_t moviEnd = rs.file.position();

    // Write idx1
    rs.indexOffset = rs.file.position();
    writeTag(rs.file, "idx1");
    write32(rs.file, rs.idxCount * 16);
    for (uint32_t i = 0; i < rs.idxCount; i++) {
        rs.file.write((uint8_t*)rs.idx[i].id, 4);
        write32(rs.file, rs.idx[i].flags);
        write32(rs.file, rs.idx[i].offset);
        write32(rs.file, rs.idx[i].size);
    }

    uint32_t fileEnd = rs.file.position();

    rs.file.seek(g_riffSizeOff);
    write32(rs.file, fileEnd - 8);

    rs.file.seek(g_moviSizeOff);
    write32(rs.file, moviEnd - g_moviSizeOff - 4);

    rs.file.seek(g_avihTotalFrOff);
    write32(rs.file, rs.frameCount);

    rs.file.seek(g_vstrhLengthOff);
    write32(rs.file, rs.frameCount);

    rs.file.close();

    if (rs.idx) {
        heap_caps_free(rs.idx);
        rs.idx = nullptr;
    }
    if (rs.audioBuf) {
        heap_caps_free(rs.audioBuf);
        rs.audioBuf = nullptr;
        rs.audioBufSz = 0;
    }

    uint32_t dur = (millis() - rs.startMs) / 1000;
    Serial.printf("[REC] Done: %lu frames, %lu s, ~%lu KB video, ~%lu KB audio\n",
        rs.frameCount, dur,
        rs.totalVideoBytes / 1024, rs.totalAudioBytes / 1024);
}
