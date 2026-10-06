// The sample pool: loading a WAV or a compressed file into a slot, clearing it, and the memory count.
// The editor's operations on a loaded sample are in sample-editor.cpp.
#include "audio-engine.h"
#include "vendor/tsf/tsf.h"    // tsf_get_fontsamplecount — the memory count includes the loaded fonts
#include "audio-decoders.h"
#include "common/byte_source.h"       // pt_fopen — the WAV reader opens through it
#include "common/platform_memory.h"   // load_budget_bytes — refuses a load the device cannot hold
#include "common/load_progress.h"     // load_tick / load_cancelled — a slow load reports itself and can be stopped
#include "common/sample_formats.h"    // sample_decoder_for — which decoder an extension takes
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

bool AudioEngine::loadSample(int id, const float* data, int length) {
    if (id < 0 || id >= 256 || !data || length < 1) return false;

    // ⚠️ Allocated and filled BEFORE the lock and before anything is freed, so a failure leaves the
    // slot holding the sample it already had. Freeing first and then failing to allocate would
    // destroy a loaded sample to report that a different one could not be loaded.
    //
    // std::nothrow because a bare `new` throws and nothing in native/ catches — an uncaught
    // bad_alloc is std::terminate, which loses the song and not just the file.
    //
    // ⚠️ This buys a clean failure only where the allocator has one to give (Windows, 32-bit armhf).
    // 64-bit Android's bionic grants any size and kills on the write; only refusing the load first helps.
    float* newL = new (std::nothrow) float[length];
    if (!newL) {
        LOGE("loadSample: OOM allocating %d frames", length);
        return false;
    }
    std::memcpy(newL, data, static_cast<size_t>(length) * sizeof(float));

    // Hold sampleEditMutex while swapping the buffer.  The audio thread uses
    // try_to_lock on this mutex inside its mix loop, so it will skip at most
    // one callback (~10 ms of silence) rather than crashing on a freed pointer.
    std::lock_guard<std::mutex> lock(sampleEditMutex);

    // New file also invalidates the sample editor's undo backup — keeping it would hold RAM and
    // let undoSample restore the PREVIOUS sample's audio onto this one.
    delete[] sampleBackups[id];        sampleBackups[id] = nullptr;
    delete[] sampleBackupsRight[id];   sampleBackupsRight[id] = nullptr;
    sampleBackupLengths[id] = 0;
    // New file replaces the original — discard any cached rate-mode original. A float buffer handed
    // in carries no depth of its own; a loader that knows better sets it after this returns.
    setSampleSourceFormat(id, 16, false);

    setSampleBuffers(id, newL, nullptr, length);   // the voices playing the old one end at the next mix

    LOGD("Sample %d: %d frames (mono)", id, length);
    return true;
}

// Decode one WAV sample at `p` to a normalized float in [-1, 1): little-endian, standard divisors.
static inline float decodeWavSample(const uint8_t* p, int audioFormat, int bitsPerSample) {
    if (audioFormat == 3 && bitsPerSample == 32) {           // IEEE float
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        float out;
        std::memcpy(&out, &u, sizeof(out));
        return out;
    }
    if (bitsPerSample == 8) {                                // PCM 8-bit, UNSIGNED (center 128)
        return (p[0] - 128) / 128.0f;
    }
    if (bitsPerSample == 16) {                               // PCM 16-bit
        int16_t v = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        return v / 32768.0f;
    }
    if (bitsPerSample == 24) {                               // PCM 24-bit, little-endian signed
        // Assemble unsigned (no signed-shift UB), then sign-extend bit 23.
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        int32_t v = (u & 0x800000u) ? (int32_t)(u | 0xFF000000u) : (int32_t)u;
        return v / 8388608.0f;                               // 2^23
    }
    // PCM 32-bit (only remaining supported case)
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)u / 2147483648.0f;                       // 2^31
}

int AudioEngine::loadSampleFromWavFile(int id, const char* path) {
    if (id < 0 || id >= 256 || !path) return 0;

    // Every exit below leaves this at PARSE unless it is raised to OUT_OF_MEMORY or cleared on
    // success, so the UI can never read a reason left behind by an earlier load.
    lastLoadFailure_ = LoadFailure::PARSE;

    FILE* f = pt_fopen(path, "rb");
    if (!f) { LOGE("loadSampleFromWavFile: cannot open %s", path); return 0; }

    // RIFF/WAVE header (12 bytes).
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 ||
        std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        LOGE("loadSampleFromWavFile: not a RIFF/WAVE file: %s", path);
        fclose(f);
        return 0;
    }

    // Scan chunks for fmt + data (fmt always precedes data in a valid WAV). Don't assume fixed
    // offsets — a JUNK/bext/RF64 chunk before fmt shifts everything.
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
            // WAVE_FORMAT_EXTENSIBLE (0xFFFE): real format code is the first 2 bytes of the
            // sub-format GUID at offset 24 into the fmt body (1=PCM, 3=float).
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

    // Clamp dataSize to the real bytes left after dataOffset so a bogus chunk size can't over-read.
    fseek(f, 0, SEEK_END);
    long fileEnd = ftell(f);
    fseek(f, dataOffset, SEEK_SET);
    long avail = fileEnd - dataOffset;
    if (avail < 0) avail = 0;
    if ((long)dataSize > avail) dataSize = (uint32_t)avail;

    int totalFrames = (int)(dataSize / (uint32_t)bytesPerFrame);
    if (totalFrames < 1) { fclose(f); return 0; }

    // ⭐ The one source whose cost is known EXACTLY up front: `dataSize` is in the header. The
    // soundfont and compressed paths learn their size by decoding and are guarded where they grow.
    // ⚠️ Checked against `load_budget_bytes()`, not raw free memory: this is a PREDICTION, and the
    // budget carries the floor for Android's under-reporting. 0 = unknown, nothing refused.
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

    // std::nothrow, so a genuine OOM returns cleanly instead of terminating.
    float* newL = new (std::nothrow) float[totalFrames];
    float* newR = (channels == 2) ? new (std::nothrow) float[totalFrames] : nullptr;
    if (!newL || (channels == 2 && !newR)) {
        delete[] newL;
        delete[] newR;
        fclose(f);
        LOGE("loadSampleFromWavFile: OOM allocating %d frames", totalFrames);
        return 0;
    }

    // Stream the data chunk in whole-frame blocks so a sample is never split across a read.
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
        // ⭐ The only load in the app whose fraction is exact from the first block: `totalFrames` is
        // read out of the header. A WAV is a read rather than a decode, so this is normally over
        // before anything can be drawn — it matters on a slow card and on a very long file.
        if (!pt::load_tick((float)frameIdx / (float)totalFrames)) { cancelled = true; break; }
        if (framesGot < want) break;  // short read / truncated file (shouldn't happen — see clamp)
    }
    fclose(f);

    // ⚠️ Nothing is published on a cancel — the slot keeps the sample it had. The two fresh buffers
    // are ours alone at this point (the swap below is what hands them over), so freeing them here is
    // the whole cleanup.
    if (cancelled) {
        delete[] newL;
        delete[] newR;
        lastLoadFailure_ = LoadFailure::CANCELLED;
        LOGD("loadSampleFromWavFile: cancelled at %d/%d frames: %s", frameIdx, totalFrames, path);
        return 0;
    }

    // `new float[]` is not zero-initialized; a short read above would leave indeterminate tail
    // samples. dataSize is clamped to the bytes actually present, so this is defensive — but zero
    // any unfilled tail so we can never play uninitialized memory as noise.
    if (frameIdx < totalFrames) {
        std::memset(newL + frameIdx, 0, (size_t)(totalFrames - frameIdx) * sizeof(float));
        if (newR) std::memset(newR + frameIdx, 0, (size_t)(totalFrames - frameIdx) * sizeof(float));
    }

    // Swap into the slot under the edit lock (audio thread try_locks it in the mix loop) — same
    // discipline as loadSample. Free EVERY stale per-slot buffer: a fresh file makes the old
    // sample's undo/rate caches meaningless.
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

    // Lowercase the file extension (max 7 chars is plenty for mp3/flac/ogg).
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

    // ⚠️ THE ONLY `catch` IN native/: the decoders' block buffers and the MP4 whole-file read are
    // std::vectors sized by the file, with no nothrow site. An uncaught bad_alloc takes the unsaved
    // song with it; a caught one is a LOAD FAILED. (64-bit Android kills on the write instead.)
    try {
        switch (pt::sample_decoder_for(ext)) {
            case pt::SampleDecoder::MP3:  ok = ptdec::decodeMp3File(path, pcm, sr);  break;
            case pt::SampleDecoder::FLAC: ok = ptdec::decodeFlacFile(path, pcm, sr); break;
            case pt::SampleDecoder::OGG:
                // An .ogg holds Vorbis or Opus: try Vorbis, then Opus on a miss. ⚠️ A CANCEL IS NOT A
                // MISS — without that check the retry decodes the whole file again after a stop.
                ok = ptdec::decodeOggFile(path, pcm, sr);
                if (!ok && !pt::load_cancelled()) {
                    pcm.clear();
                    ok = ptdec::decodeOpusFile(path, pcm, sr);
                }
                break;
            case pt::SampleDecoder::OPUS: ok = ptdec::decodeOpusFile(path, pcm, sr); break;
            case pt::SampleDecoder::MP4:  ok = ptdec::decodeMp4File(path, pcm, sr);  break;   // minimp4 + FAAD2
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
        // ⚠️ A CANCEL comes back as the same false as everything else, and it is asked FIRST because
        // it is the one answer that is not a failure: nothing is wrong with the file and the user is
        // not to be told there is.
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

    // A FLAC keeps its source depth, and the float decode above holds all of it. The depth is the 5 bits
    // straddling bytes 20–21: "fLaC", a 4-byte block header, then STREAMINFO — whose place as the first
    // block the format requires — with bits-per-sample minus one after the rate and channel fields.
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

    // The decoded buffers become the slot's as they are — no second copy. Same swap as the WAV path.
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

// Declared in audio-engine.h, where the reason it is a member rather than a UI-side walk is written
// down. One line per buffer this class owns; a right-channel pointer is counted only when it exists,
// since a mono sample leaves it null and shares the one length.
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

    // A SoundFont's PCM lives inside tsf, which holds every sample as a 16-bit word. One handle is
    // shared by every track pointed at that slot, so it is counted once per LOADED FONT rather than
    // per instrument — the same 40 MB font on four tracks is 40 MB, not 160.
    for (int slot = 0; slot < MAX_SOUNDFONTS; ++slot) {
        if (soundfonts[slot].handle)
            total += static_cast<int64_t>(tsf_get_fontsamplecount(soundfonts[slot].handle)) * 2;
    }
    return total;
}

void AudioEngine::clearSample(int id) {
    if (id < 0 || id >= 256) return;
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    // Free every per-slot buffer so a sample doesn't linger in memory after the slot is repurposed
    // (e.g. switching the instrument to SoundFont). delete[] nullptr is a safe no-op.
    setSampleBuffers(id, nullptr, nullptr, 0);
    delete[] sampleBackups[id];        sampleBackups[id] = nullptr;
    delete[] sampleBackupsRight[id];   sampleBackupsRight[id] = nullptr;
    setSampleSourceFormat(id, 16, false);
    sampleBackupLengths[id]  = 0;
    LOGD("Sample %d cleared from memory", id);
}

void AudioEngine::clearAllSamples() {
    // Hold sampleEditMutex for the entire operation.  The audio thread uses
    // try_to_lock so it skips its mix block rather than reading freed memory.
    std::lock_guard<std::mutex> lock(sampleEditMutex);

    // Clear queues to prevent re-triggering the voices the swap below ends.
    // Each queue method acquires its own internal mutex; no deadlock risk
    // because the audio thread cannot hold those mutexes while we hold sampleEditMutex.
    noteQueue.clear();
    killQueue.clear();
    paramUpdateQueue.clear();

    for (int i = 0; i < 256; i++) {
        setSampleBuffers(i, nullptr, nullptr, 0);   // every sampler voice ends at the next mix
        setSampleSourceFormat(i, 16, false);
    }
    LOGD("All samples cleared");
}
