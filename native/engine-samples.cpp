// The sample pool: loading a WAV or a compressed file into a slot, clearing it, and the memory count.
// The editor's operations on a loaded sample are in sample-editor.cpp.
#include "audio-engine.h"
#include "vendor/tsf/tsf.h"    // tsf_get_fontsamplecount, for the memory count
#include "audio-decoders.h"
#include "common/byte_source.h"       // pt_fopen
#include "common/platform_memory.h"   // load_budget_bytes
#include "common/load_progress.h"     // load_tick / load_cancelled
#include "common/sample_formats.h"    // sample_decoder_for
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

bool AudioEngine::loadSample(int id, const float* data, int length) {
    if (id < 0 || id >= 256 || !data || length < 1) return false;

    // ⚠️ Allocated before anything is freed, so a failure keeps the old sample. nothrow: an uncaught
    // bad_alloc would lose the song. (64-bit Android never refuses — only a check before the load helps.)
    float* newL = new (std::nothrow) float[length];
    if (!newL) {
        LOGE("loadSample: OOM allocating %d frames", length);
        return false;
    }
    std::memcpy(newL, data, static_cast<size_t>(length) * sizeof(float));

    // The mix try_locks this: it skips a block rather than read a freed buffer.
    std::lock_guard<std::mutex> lock(sampleEditMutex);

    // Undo would restore the previous file's audio onto this one.
    delete[] sampleBackups[id];        sampleBackups[id] = nullptr;
    delete[] sampleBackupsRight[id];   sampleBackupsRight[id] = nullptr;
    sampleBackupLengths[id] = 0;
    // A float buffer carries no depth; a loader that knows sets it afterwards.
    setSampleSourceFormat(id, 16, false);

    setSampleBuffers(id, newL, nullptr, length);   // voices on the old one end at the next mix

    LOGD("Sample %d: %d frames (mono)", id, length);
    return true;
}

// One WAV sample at `p` → [-1, 1).
static inline float decodeWavSample(const uint8_t* p, int audioFormat, int bitsPerSample) {
    if (audioFormat == 3 && bitsPerSample == 32) {           // IEEE float
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        float out;
        std::memcpy(&out, &u, sizeof(out));
        return out;
    }
    if (bitsPerSample == 8) {                                // unsigned, centre 128
        return (p[0] - 128) / 128.0f;
    }
    if (bitsPerSample == 16) {
        int16_t v = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        return v / 32768.0f;
    }
    if (bitsPerSample == 24) {
        // Assembled unsigned (no signed-shift UB), then sign-extended.
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        int32_t v = (u & 0x800000u) ? (int32_t)(u | 0xFF000000u) : (int32_t)u;
        return v / 8388608.0f;                               // 2^23
    }
    // 32-bit PCM
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)u / 2147483648.0f;                       // 2^31
}

int AudioEngine::loadSampleFromWavFile(int id, const char* path) {
    if (id < 0 || id >= 256 || !path) return 0;

    // PARSE unless raised or cleared below — never an earlier load's reason.
    lastLoadFailure_ = LoadFailure::PARSE;

    FILE* f = pt_fopen(path, "rb");
    if (!f) { LOGE("loadSampleFromWavFile: cannot open %s", path); return 0; }

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 ||
        std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        LOGE("loadSampleFromWavFile: not a RIFF/WAVE file: %s", path);
        fclose(f);
        return 0;
    }

    // Walk the chunks — JUNK/bext before fmt shift everything.
    int audioFormat = 0, channels = 0, sampleRate = 0, bitsPerSample = 0;
    bool haveFmt = false;
    long dataOffset = -1;
    uint32_t dataSize = 0;
    uint8_t ch[8];
    while (fread(ch, 1, 8, f) == 8) {
        uint32_t chunkSize = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                             ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[40] = {0};
            uint32_t toRead = chunkSize < sizeof(fmt) ? chunkSize : (uint32_t)sizeof(fmt);
            if (fread(fmt, 1, toRead, f) != toRead) break;
            audioFormat   = (int)(fmt[0] | (fmt[1] << 8));
            channels      = (int)(fmt[2] | (fmt[3] << 8));
            sampleRate    = (int)((uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
                                  ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24));
            bitsPerSample = (int)(fmt[14] | (fmt[15] << 8));
            // EXTENSIBLE: the real code opens the sub-format GUID, offset 24.
            if (audioFormat == 0xFFFE && toRead >= 26)
                audioFormat = (int)(fmt[24] | (fmt[25] << 8));
            haveFmt = true;
            long skip = (long)chunkSize - (long)toRead + (long)(chunkSize & 1);
            if (skip > 0) fseek(f, skip, SEEK_CUR);
        } else if (std::memcmp(ch, "data", 4) == 0) {
            dataOffset = ftell(f);
            dataSize = chunkSize;
            break;
        } else {
            fseek(f, (long)chunkSize + (long)(chunkSize & 1), SEEK_CUR);  // skip, pad to even
        }
    }

    if (!haveFmt || dataOffset < 0 || channels < 1 || channels > 2 ||
        bitsPerSample == 0 || sampleRate == 0) {
        LOGE("loadSampleFromWavFile: bad header (fmt=%d ch=%d bits=%d rate=%d) %s",
             audioFormat, channels, bitsPerSample, sampleRate, path);
        fclose(f);
        return 0;
    }
    bool isFloat = (audioFormat == 3 && bitsPerSample == 32);
    bool isPcm   = (audioFormat == 1 && (bitsPerSample == 8 || bitsPerSample == 16 || bitsPerSample == 24 || bitsPerSample == 32));
    if (!isFloat && !isPcm) {
        LOGE("loadSampleFromWavFile: unsupported format=%d bits=%d %s", audioFormat, bitsPerSample, path);
        fclose(f);
        return 0;
    }

    int bytesPerSample = bitsPerSample / 8;
    int bytesPerFrame  = bytesPerSample * channels;

    // A bogus chunk size cannot over-read.
    fseek(f, 0, SEEK_END);
    long fileEnd = ftell(f);
    fseek(f, dataOffset, SEEK_SET);
    long avail = fileEnd - dataOffset;
    if (avail < 0) avail = 0;
    if ((long)dataSize > avail) dataSize = (uint32_t)avail;

    int totalFrames = (int)(dataSize / (uint32_t)bytesPerFrame);
    if (totalFrames < 1) { fclose(f); return 0; }

    // The size is known up front, so it is checked before any work — against the load budget, not
    // free memory. 0 = unknown, nothing refused.
    {
        const int64_t needed = static_cast<int64_t>(totalFrames) * channels * 4;
        const int64_t budget = pt::load_budget_bytes();
        if (budget > 0 && needed > budget) {
            LOGE("loadSampleFromWavFile: %s needs %lld MB, %lld MB free — refused",
                 path, (long long)(needed >> 20), (long long)(budget >> 20));
            lastLoadFailure_ = LoadFailure::OUT_OF_MEMORY;
            fclose(f);
            return 0;
        }
    }

    float* newL = new (std::nothrow) float[totalFrames];
    float* newR = (channels == 2) ? new (std::nothrow) float[totalFrames] : nullptr;
    if (!newL || (channels == 2 && !newR)) {
        delete[] newL;
        delete[] newR;
        fclose(f);
        LOGE("loadSampleFromWavFile: OOM allocating %d frames", totalFrames);
        return 0;
    }

    // Whole-frame blocks, so no sample splits across a read.
    const int BLOCK_FRAMES = 16384;
    std::vector<uint8_t> blk((size_t)BLOCK_FRAMES * bytesPerFrame);
    int  frameIdx  = 0;
    bool cancelled = false;
    while (frameIdx < totalFrames) {
        int want = totalFrames - frameIdx;
        if (want > BLOCK_FRAMES) want = BLOCK_FRAMES;
        size_t got = fread(blk.data(), 1, (size_t)want * bytesPerFrame, f);
        int framesGot = (int)(got / (size_t)bytesPerFrame);
        for (int i = 0; i < framesGot; i++) {
            const uint8_t* p = blk.data() + (size_t)i * bytesPerFrame;
            newL[frameIdx + i] = decodeWavSample(p, audioFormat, bitsPerSample);
            if (channels == 2)
                newR[frameIdx + i] = decodeWavSample(p + bytesPerSample, audioFormat, bitsPerSample);
        }
        frameIdx += framesGot;
        if (!pt::load_tick((float)frameIdx / (float)totalFrames)) { cancelled = true; break; }
        if (framesGot < want) break;  // truncated file
    }
    fclose(f);

    // On a cancel the slot keeps its sample; the fresh buffers are still ours alone.
    if (cancelled) {
        delete[] newL;
        delete[] newR;
        lastLoadFailure_ = LoadFailure::CANCELLED;
        LOGD("loadSampleFromWavFile: cancelled at %d/%d frames: %s", frameIdx, totalFrames, path);
        return 0;
    }

    // A short read must not leave uninitialised samples to play as noise.
    if (frameIdx < totalFrames) {
        std::memset(newL + frameIdx, 0, (size_t)(totalFrames - frameIdx) * sizeof(float));
        if (newR) std::memset(newR + frameIdx, 0, (size_t)(totalFrames - frameIdx) * sizeof(float));
    }

    // As loadSample; every stale per-slot buffer (undo, rate cache) goes.
    {
        std::lock_guard<std::mutex> lock(sampleEditMutex);
        delete[] sampleBackups[id];        sampleBackups[id] = nullptr;
        delete[] sampleBackupsRight[id];   sampleBackupsRight[id] = nullptr;
        sampleBackupLengths[id] = 0;
        setSampleSourceFormat(id, bitsPerSample, isFloat);
        setSampleBuffers(id, newL, newR, totalFrames);
    }

    lastLoadFailure_ = LoadFailure::NONE;
    LOGD("loadSampleFromWavFile: id=%d %d frames %s rate=%d bits=%d fmt=%d",
         id, totalFrames, channels == 2 ? "stereo" : "mono", sampleRate, bitsPerSample, audioFormat);
    return sampleRate;
}

int AudioEngine::loadSampleFromCompressed(int id, const char* path) {
    if (id < 0 || id >= 256 || !path) return 0;

    const char* dot = std::strrchr(path, '.');
    if (!dot) return 0;
    char ext[8] = {0};
    for (int i = 0; i < 7 && dot[i + 1]; i++) {
        char c = dot[i + 1];
        ext[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }

    ptdec::PcmSink pcm;
    int sr = 0;
    bool ok = false;

    // ⚠️ The only `catch` in native/: the decoders' vectors are sized by the file, and an uncaught
    // bad_alloc would take the unsaved song with it.
    try {
        switch (pt::sample_decoder_for(ext)) {
            case pt::SampleDecoder::MP3:  ok = ptdec::decodeMp3File(path, pcm, sr);  break;
            case pt::SampleDecoder::FLAC: ok = ptdec::decodeFlacFile(path, pcm, sr); break;
            case pt::SampleDecoder::OGG:
                // Vorbis, then Opus on a miss. ⚠️ A cancel is not a miss, or a stop decodes the file again.
                ok = ptdec::decodeOggFile(path, pcm, sr);
                if (!ok && !pt::load_cancelled()) {
                    pcm.clear();
                    ok = ptdec::decodeOpusFile(path, pcm, sr);
                }
                break;
            case pt::SampleDecoder::OPUS: ok = ptdec::decodeOpusFile(path, pcm, sr); break;
            case pt::SampleDecoder::MP4:  ok = ptdec::decodeMp4File(path, pcm, sr);  break;
            case pt::SampleDecoder::WAV:
            case pt::SampleDecoder::NONE:
                LOGE("loadSampleFromCompressed: unsupported extension '%s'", ext);
                return 0;
        }
    } catch (const std::bad_alloc&) {
        LOGE("loadSampleFromCompressed: out of memory decoding %s", path);
        lastLoadFailure_ = LoadFailure::OUT_OF_MEMORY;
        return 0;
    }

    if (!ok || pcm.frames() == 0 || sr <= 0) {
        // ⚠️ Cancel first: it returns the same false but is not a failure.
        if (pt::load_cancelled()) {
            lastLoadFailure_ = LoadFailure::CANCELLED;
            LOGD("loadSampleFromCompressed: cancelled (%s)", path);
            return 0;
        }
        LOGE("loadSampleFromCompressed: decode failed (%s)", path);
        lastLoadFailure_ = pcm.out_of_memory() ? LoadFailure::OUT_OF_MEMORY : LoadFailure::PARSE;
        return 0;
    }
    pcm.finish();

    // A FLAC keeps its source depth: bits-per-sample − 1 is the 5 bits across bytes 20–21 (STREAMINFO
    // is always the first block).
    int bits = 16;
    if (std::strcmp(ext, "flac") == 0) {
        if (FILE* ff = pt_fopen(path, "rb")) {
            uint8_t head[22];
            if (fread(head, 1, sizeof(head), ff) == sizeof(head) && std::memcmp(head, "fLaC", 4) == 0 &&
                (head[4] & 0x7F) == 0) {
                const int b = (((head[20] & 0x01) << 4) | (head[21] >> 4)) + 1;
                if (b > 16) bits = b > 24 ? 32 : 24;
            }
            fclose(ff);
        }
    }

    // The decoded buffers become the slot's — no copy.
    const int frames = static_cast<int>(pcm.frames());
    const bool stereo = pcm.stereo();
    float* newL = nullptr;
    float* newR = nullptr;
    pcm.release(newL, newR);
    {
        std::lock_guard<std::mutex> lock(sampleEditMutex);
        delete[] sampleBackups[id];        sampleBackups[id] = nullptr;
        delete[] sampleBackupsRight[id];   sampleBackupsRight[id] = nullptr;
        sampleBackupLengths[id] = 0;
        setSampleSourceFormat(id, bits, false);
        setSampleBuffers(id, newL, newR, frames);
    }

    lastLoadFailure_ = LoadFailure::NONE;
    LOGD("loadSampleFromCompressed: id=%d %d frames %s rate=%d (%s)",
         id, frames, stereo ? "stereo" : "mono", sr, ext);
    return sr;
}

bool AudioEngine::hasStereoData(int id) {
    if (id < 0 || id >= 256) return false;
    return samplesRight[id] != nullptr;
}

// One line per buffer the engine owns; a mono sample's right channel is null.
int64_t AudioEngine::audio_memory_bytes() const {
    const auto pcm = [](const void* left, const void* right, int64_t frames, int64_t bytesPerFrame) {
        if (!left || frames <= 0) return int64_t{0};
        return frames * bytesPerFrame * (right ? 2 : 1);
    };

    int64_t total = 0;
    for (int id = 0; id < 256; ++id) {
        total += pcm(samples[id],         samplesRight[id],         sampleLengths[id],         4);
        total += pcm(sampleBackups[id],   sampleBackupsRight[id],   sampleBackupLengths[id],   2);
        total += pcm(originalSamples[id], originalSamplesRight[id], originalSampleLengths[id], 2);
        total += pcm(originalSamplesF[id], originalSamplesRightF[id], originalSampleLengths[id], 4);
    }
    total += pcm(fxPreviewBackup, fxPreviewBackupRight, fxPreviewBackupLen,    4);
    total += pcm(sampleClipboard, sampleClipboardRight, sampleClipboardLength, 4);

    // tsf holds 16-bit samples; counted once per loaded font, not per instrument.
    for (int slot = 0; slot < MAX_SOUNDFONTS; ++slot) {
        if (soundfonts[slot].handle)
            total += static_cast<int64_t>(tsf_get_fontsamplecount(soundfonts[slot].handle)) * 2;
    }
    return total;
}

void AudioEngine::clearSample(int id) {
    if (id < 0 || id >= 256) return;
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    // Every per-slot buffer, so nothing lingers once the slot is repurposed.
    setSampleBuffers(id, nullptr, nullptr, 0);
    delete[] sampleBackups[id];        sampleBackups[id] = nullptr;
    delete[] sampleBackupsRight[id];   sampleBackupsRight[id] = nullptr;
    setSampleSourceFormat(id, 16, false);
    sampleBackupLengths[id]  = 0;
    LOGD("Sample %d cleared from memory", id);
}

void AudioEngine::clearAllSamples() {
    // Held throughout; the mix try_locks it.
    std::lock_guard<std::mutex> lock(sampleEditMutex);

    // Cleared so nothing re-triggers the voices the swap ends.
    noteQueue.clear();
    killQueue.clear();
    paramUpdateQueue.clear();

    for (int i = 0; i < 256; i++) {
        setSampleBuffers(i, nullptr, nullptr, 0);   // voices end at the next mix
        setSampleSourceFormat(i, 16, false);
    }
    LOGD("All samples cleared");
}
