// TSF API declarations only — TSF_IMPLEMENTATION lives in soundfont-voice.cpp
#include "audio-engine.h"
#include "kissfft/kiss_fftr.h"
#include "vendor/tsf/tsf.h"
#include "mods/mod-runner.h"
#include "mods/modules/pitch-slide-module.h"
#include "mods/modules/vibrato-module.h"
#include "effects/primitives/sola-stretch.h"
#include "audio-decoders.h"
#include "byte_source.h"   // pt_fopen — the WAV reader and the soundfont loader open through it
#include "platform_memory.h"   // load_budget_bytes — refuses a load the device cannot hold
#include "load_progress.h"     // load_tick / load_cancelled — a slow load reports itself and can be stopped
#include "table_automation.h"  // AUS/AUF pairing over a table's rows — shared with the table editor
#include <cstdio>
#include <cstdint>
#include <climits>   // INT_MAX — tsf_load_memory takes an int size
#include <cstring>
#include <new>
#include <vector>
// MSVC defines neither __SSE2__ nor __x86_64__ — it signals x86/x64 with _M_X64 / _M_IX86 — so the
// host build would silently lose denormal protection without these arms. SSE2 is baseline on x64.
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86) && _M_IX86_FP >= 2)
#define PT_HAS_SSE_DENORMAL_CTRL 1
#include <pmmintrin.h>  // FTZ/DAZ MXCSR macros for setFlushToZeroForCurrentThread
#endif

// Definition of the per-track soundfont voice array (extern declared in audio-engine.h):
// song tracks 0-7 + the preview lane (track 8).
SoundfontVoice sfVoices[SF_VOICE_COUNT];

namespace {
// The `tsf_stream` pair loadSoundfont hands to tsf_load — the same one tsf builds inside
// tsf_load_filename, which is unused here because the open must be pt_fopen's. `read` returns the
// byte count, `skip` returns 1 on success and 0 on error, per tsf.h.
//
// Declared `void*` rather than `FILE*`: tsf's own pair takes FILE* and is cast into the struct at
// the call site, which is a call through a mismatched function-pointer type. Taking void* and
// casting inside costs nothing and is the same shape the dr_libs callbacks use.
int sfStreamRead(void* f, void* ptr, unsigned int size) {
    return (int)std::fread(ptr, 1, size, (FILE*)f);
}
int sfStreamSkip(void* f, unsigned int count) {
    return std::fseek((FILE*)f, (long)count, SEEK_CUR) == 0;
}

// A meter's running peak. Not fmaxf, which without -ffast-math is a libm call, per sample here;
// a NaN is ignored the same way.
inline float peak_hold(float peak, float x) {
    x = fabsf(x);
    return x > peak ? x : peak;
}
}  // namespace

AudioEngine::AudioEngine() {
    for (int i = 0; i < 256; i++) {
        samples[i] = nullptr;
        samplesRight[i] = nullptr;
        sampleLengths[i] = 0;
        sampleGen[i] = 0;
        sampleBackups[i] = nullptr;
        sampleBackupsRight[i] = nullptr;
        sampleBackupLengths[i] = 0;
        originalSamples[i] = nullptr;
        originalSamplesRight[i] = nullptr;
        originalSamplesF[i] = nullptr;
        originalSamplesRightF[i] = nullptr;
        originalSampleLengths[i] = 0;
        sampleBitDepth[i] = 16;
        sampleIsFloat[i]  = false;
    }
    resetTic00Cursors();
    globalFrameCounter.store(0, std::memory_order_relaxed);
    noteSeedEntropy = ((uint32_t)nowMs() * 2654435761u) | 1u;  // vary RND/DRNK per app session

    // Pre-size the per-block drain buffers. A single ~23 ms block only ever holds a handful of
    // events (a few tracks × retrigs), and 64 covers a dense AUS/AUF ramp across all eight.
    //
    // ⚠️ It is a typical bound, not a hard one, and `drainUntil` uses push_back — so this is "the
    // audio thread almost never allocates", not "never". The bound is not a property of the block:
    // `drainUntil` takes everything scheduled at or before the block end, INCLUDING anything already
    // overdue, so a stall or a resume after a long pause can cross it. The cost is one `operator new`
    // inside the callback, once, after which the capacity persists. Said out loud rather than left to
    // read as a guarantee.
    noteBatch.reserve(64);
    killBatch.reserve(64);
    paramBatch.reserve(64);

    for (int i = 0; i < WAVEFORM_SIZE; i++) {
        waveformBuffer[i] = 0.0f;
    }
    waveformIndex = 0;
    waveformDownsampleCounter = 0;
    for (int i = 0; i < SPECTRUM_SIZE; i++) {
        spectrumBuffer[i] = 0.0f;
        delaySpectrumBuffer[i] = 0.0f;
        reverbSpectrumBuffer[i] = 0.0f;
        instrSpectrumBuffer[i] = 0.0f;
    }
    spectrumWriteIdx = delaySpectrumWriteIdx = reverbSpectrumWriteIdx = instrSpectrumWriteIdx = 0;
    // The buses need valid coefficients before any audio, and there is no device yet to ask — so they
    // are built at the fallback rate and REBUILT by setDeviceSampleRate the moment the shell learns
    // the real one. `effectsSampleRate` records which rate this was, so that call knows what is owed.
    const float defaultRate = static_cast<float>(effectsSampleRate);
    reverbSend.reset(defaultRate);
    delaySend.reset(defaultRate);
    masterChain.reset(defaultRate);
}

void AudioEngine::setDeviceSampleRate(int sr) {
    if (sr <= 0) return;
    deviceSampleRate.store(sr, std::memory_order_relaxed);
    if (sr == effectsSampleRate) return;
    effectsSampleRate = sr;
    resetEffectState();     // reads getSampleRate(), i.e. the value just stored
    // The buses are at factory defaults now; the next block puts the song's own values back.
    prepareReverb();   // …and the reverb's engine, rebuilt at the new rate, is waiting for it
    busReplayRequested.store(true, std::memory_order_release);
}

AudioEngine::~AudioEngine() {
    // ⚠️ FIRST. A background preset load holds a `this` capture, so nothing below may free anything
    // while it is still running. It is at most one preset's decode.
    discardSoundfontLoad();

    // The platform backend (OboeAudioEngine on Android, SdlAudioEngine on desktop) owns and closes the
    // output stream; the core just frees its buffers. The owner (android-main's / the shell's `main`)
    // destroys the backend first, so no callback can run during this teardown.
    for (int i = 0; i < 256; i++) {
        if (samples[i])              delete[] samples[i];
        if (samplesRight[i])         delete[] samplesRight[i];
        if (sampleBackups[i])        delete[] sampleBackups[i];
        if (sampleBackupsRight[i])   delete[] sampleBackupsRight[i];
        freeRateCache(i);
    }
    delete[] sampleClipboard;
    delete[] sampleClipboardRight;
    delete[] fxPreviewBackup;
    delete[] fxPreviewBackupRight;
}

// Stream lifecycle (openStream/closeStream/resumeStream) and the audio callback now live in the
// platform backend, oboe-audio-engine.cpp. The core is backend-agnostic.

bool AudioEngine::loadSample(int id, const float* data, int length) {
    if (id < 0 || id >= 256 || !data || length < 1) return false;

    // ⚠️ Allocated and filled BEFORE the lock and before anything is freed, so a failure leaves the
    // slot holding the sample it already had. Freeing first and then failing to allocate would
    // destroy a loaded sample to report that a different one could not be loaded.
    //
    // std::nothrow because a bare `new` throws and nothing in native/ catches — an uncaught
    // bad_alloc is std::terminate, which loses the song and not just the file.
    //
    // ⚠️ This buys a clean failure only where the allocator HAS one to give. Windows (real commit
    // accounting) and 32-bit armhf (3 GB of address space) reach it. 64-bit Android does not: bionic
    // grants any size — 256 GB was granted on a 7 GB device — and the process is killed on the write
    // instead. Nothing written at an allocation site can turn that into a LOAD FAILED; only refusing
    // the load before it starts can.
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

bool AudioEngine::loadSampleStereo(int id, const float* left, const float* right, int length) {
    if (id < 0 || id >= 256 || !left || !right || length < 1) return false;

    // Both channels up front, for loadSample's reason — and both or neither, so a half-allocated
    // stereo pair can never be published.
    float* newL = new (std::nothrow) float[length];
    float* newR = new (std::nothrow) float[length];
    if (!newL || !newR) {
        delete[] newL;
        delete[] newR;
        LOGE("loadSampleStereo: OOM allocating 2 x %d frames", length);
        return false;
    }
    std::memcpy(newL, left,  static_cast<size_t>(length) * sizeof(float));
    std::memcpy(newR, right, static_cast<size_t>(length) * sizeof(float));

    std::lock_guard<std::mutex> lock(sampleEditMutex);

    // Same as loadSample: a new file invalidates the old sample's undo backup.
    delete[] sampleBackups[id];        sampleBackups[id] = nullptr;
    delete[] sampleBackupsRight[id];   sampleBackupsRight[id] = nullptr;
    sampleBackupLengths[id] = 0;
    setSampleSourceFormat(id, 16, false);

    setSampleBuffers(id, newL, newR, length);

    LOGD("Sample %d: %d frames (stereo)", id, length);
    return true;
}

bool AudioEngine::beginSampleLoad(int id, int channels, int estimatedFrames) {
    if (id < 0 || id >= 256 || estimatedFrames < 1) return false;
    int ch = (channels >= 2) ? 2 : 1;
    // Allocate the destination up front so chunks fill it in place — no whole-file PCM on the Java heap
    // and no second native copy at finalize. std::nothrow: a real OOM returns false instead of aborting
    // (native new aborts under -fno-exceptions). Not zero-filled: sampleLengths stays 0 until finalize,
    // and finalize publishes only the frames actually written, so the unfilled tail is never read.
    float* newL = new (std::nothrow) float[estimatedFrames];
    float* newR = (ch == 2) ? new (std::nothrow) float[estimatedFrames] : nullptr;
    if (!newL || (ch == 2 && !newR)) { delete[] newL; delete[] newR; return false; }

    std::lock_guard<std::mutex> lock(sampleEditMutex);
    // Free every stale per-slot buffer (mirror loadSampleFromWavFile).
    delete[] sampleBackups[id];        sampleBackups[id] = nullptr;
    delete[] sampleBackupsRight[id];   sampleBackupsRight[id] = nullptr;
    sampleBackupLengths[id] = 0;
    setSampleSourceFormat(id, 16, false);   // the chunks arrive as int16
    setSampleBuffers(id, newL, newR, 0);    // not playable until finalize
    streamLoadId       = id;
    streamLoadChannels = ch;
    streamLoadCapacity = estimatedFrames;
    streamLoadFilled   = 0;
    return true;
}

void AudioEngine::fillSampleChunk(int id, const int16_t* interleaved, int frameCount, int channels) {
    // No lock: begin left sampleLengths[id]=0 so no voice reads this slot until finalize; we only write
    // the already-allocated buffer in place. Clamp to capacity (a duration under-estimate drops the tail).
    if (id != streamLoadId || !samples[id] || !interleaved || frameCount < 1) return;
    int n = frameCount;
    if (streamLoadFilled + n > streamLoadCapacity) n = streamLoadCapacity - streamLoadFilled;
    if (n <= 0) return;
    float* L = samples[id];
    float* R = samplesRight[id];
    const int base = streamLoadFilled;
    if (channels >= 2) {
        for (int i = 0; i < n; i++) {
            L[base + i] = interleaved[(size_t)i * channels] / 32768.0f;
            if (R) R[base + i] = interleaved[(size_t)i * channels + 1] / 32768.0f;
        }
    } else {
        for (int i = 0; i < n; i++) L[base + i] = interleaved[i] / 32768.0f;
    }
    streamLoadFilled += n;
}

int AudioEngine::finalizeSampleLoad(int id) {
    if (id != streamLoadId) return 0;
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    sampleLengths[id] = streamLoadFilled;   // publish actual frames; the unfilled tail is never reached
    touchSample(id);                        // a voice started on the empty slot ends
    int frames = streamLoadFilled;
    streamLoadId = -1; streamLoadChannels = 0; streamLoadCapacity = 0; streamLoadFilled = 0;
    LOGD("Streaming sample load: id=%d %d frames", id, frames);
    return frames;
}

void AudioEngine::cancelSampleLoad(int id) {
    // Decode failed/aborted — free the partially-filled buffer so it doesn't linger or play as garbage.
    if (id != streamLoadId) return;
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    setSampleBuffers(id, nullptr, nullptr, 0);
    streamLoadId = -1; streamLoadChannels = 0; streamLoadCapacity = 0; streamLoadFilled = 0;
}

// Decode one WAV sample at `p` to a normalized float in [-1, 1). Mirrors AudioEngine.kt
// parseWavBuffer's `decode()` byte-for-byte (little-endian, identical divisors) so a native file
// load is bit-identical to the old Java decode.
static inline float decodeWavSample(const uint8_t* p, int audioFormat, int bitsPerSample) {
    if (audioFormat == 3 && bitsPerSample == 32) {           // IEEE float
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        float out;
        std::memcpy(&out, &u, sizeof(out));
        return out;
    }
    if (bitsPerSample == 8) {                                // PCM 8-bit, UNSIGNED (center 128)
        return (p[0] - 128) / 128.0f;                        // native-only: the dead Java decode had no 8-bit case
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
    // offsets — a JUNK/bext/RF64 chunk before fmt shifts everything (matches parseWavBuffer).
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

    // ⭐ The one source whose cost is known EXACTLY before a byte is decoded: `dataSize` is in the
    // header, so the destination is `totalFrames x channels x 4` and nothing has to be guessed. The
    // soundfont and compressed paths cannot do this — they learn their size only by decoding — and
    // are guarded where they grow instead.
    //
    // ⚠️ Checked against `load_budget_bytes()` rather than raw free memory, because this is a
    // PREDICTION made before the work starts, and the budget is the number that carries the floor for
    // Android's under-reporting. The guards that measure live use free memory directly. A budget of 0
    // means the platform could not answer, and then nothing is refused.
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

    // Allocate the destination buffers in NATIVE memory (not the capped Java heap). std::nothrow so
    // a genuine OOM returns cleanly instead of terminating (native new aborts under -fno-exceptions).
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

// ACCEPTED 2x MEMORY PEAK: the decoder fills std::vector L/R, then loadSampleStereo
// allocates the slot buffers and copies — both alive at once, so a multi-minute file
// transiently needs ~2x its decoded PCM in native heap. The begin/fill/finalize streaming
// path avoids this (MediaCodec loads use it) and dr_mp3/dr_flac/stb_vorbis all support
// chunked reads; wire them up only if 1 GB devices actually hit this in practice.
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

    std::vector<float> L, R;
    int sr = 0;
    bool ok;

    // ⚠️ THE ONLY `catch` IN native/, AND IT IS HERE BECAUSE THE ALLOCATIONS ARE NOT OURS. The
    // decoders below grow std::vectors — five vendored libraries, each with its own idea of how much
    // to reserve — so there is no allocation site here to hand a std::nothrow to. An uncaught
    // bad_alloc is std::terminate, which takes the unsaved song with it; a caught one is a
    // LOAD FAILED that costs the user only the file they picked.
    //
    // ⚠️ It cannot help on 64-bit Android, where bionic grants any size and the kernel kills on the
    // write — see loadSample. It is Windows and 32-bit armhf that reach a real bad_alloc.
    try {
        if      (std::strcmp(ext, "mp3")  == 0) ok = ptdec::decodeMp3File(path, L, R, sr);
        else if (std::strcmp(ext, "flac") == 0) ok = ptdec::decodeFlacFile(path, L, R, sr);
        else if (std::strcmp(ext, "ogg")  == 0) {
            // An .ogg holds either Vorbis or Opus. Try Vorbis (stb_vorbis); on a miss, retry as Opus.
            // ⚠️ A CANCEL IS NOT A MISS. Without that term the retry decodes the whole file a second
            // time with the box still up and the user's press already spent — the one place in the
            // app where "it failed, try the other decoder" and "stop" arrive as the same false.
            ok = ptdec::decodeOggFile(path, L, R, sr);
            if (!ok && !pt::load_cancelled()) {
                L.clear(); R.clear();
                ok = ptdec::decodeOpusFile(path, L, R, sr);
            }
        }
        else if (std::strcmp(ext, "opus") == 0) ok = ptdec::decodeOpusFile(path, L, R, sr);
        // ISO-BMFF containers holding AAC (minimp4 demux + FAAD2). One decoder covers them all — .m4a and
        // the container extensions are the same box format. Raw .aac (ADTS) is deliberately NOT here: it is
        // a bare stream, not a container, and is not a sample format the app offers.
        else if (std::strcmp(ext, "m4a") == 0 || std::strcmp(ext, "mp4") == 0 ||
                 std::strcmp(ext, "m4b") == 0 || std::strcmp(ext, "mov") == 0 ||
                 std::strcmp(ext, "3gp") == 0)
            ok = ptdec::decodeMp4File(path, L, R, sr);
        else { LOGE("loadSampleFromCompressed: unsupported extension '%s'", ext); return 0; }
    } catch (const std::bad_alloc&) {
        LOGE("loadSampleFromCompressed: out of memory decoding %s", path);
        lastLoadFailure_ = LoadFailure::OUT_OF_MEMORY;
        return 0;
    }

    if (!ok || L.empty() || sr <= 0) {
        // ⚠️ A CANCEL comes back as the same false as everything else, and it is asked FIRST because
        // it is the one answer that is not a failure: nothing is wrong with the file and the user is
        // not to be told there is.
        if (pt::load_cancelled()) {
            lastLoadFailure_ = LoadFailure::CANCELLED;
            LOGD("loadSampleFromCompressed: cancelled (%s)", path);
            return 0;
        }
        // ⚠️ A decoder that ran out of room returns false exactly as a corrupt file does, so the
        // reason comes from whether memory is short RIGHT NOW rather than from the return value.
        // The decoders abandon the decode and free as they unwind, so this reads the state that
        // stopped them.
        LOGE("loadSampleFromCompressed: decode failed (%s)", path);
        const int64_t budget = pt::load_budget_bytes();
        const int64_t decoded = static_cast<int64_t>(L.capacity() + R.capacity()) * sizeof(float);
        lastLoadFailure_ = (budget > 0 && decoded > 0 && decoded * 2 > budget)
                               ? LoadFailure::OUT_OF_MEMORY
                               : LoadFailure::PARSE;
        return 0;
    }

    // Publish via the existing, tested slot path (voice-stop + buffer free — incl. the stale undo
    // backup — + mutex all handled there).
    //
    // ⚠️ The publish allocates a SECOND full copy and the decoded vectors are still live across it,
    // so this path peaks at twice what it keeps. The WAV path does not — it allocates its destination
    // up front and streams into it. Removing the asymmetry means decoding straight into the slot,
    // which needs the frame count before the decode starts.
    const bool published =
        (!R.empty() && R.size() == L.size())
            ? loadSampleStereo(id, L.data(), R.data(), (int)L.size())
            : loadSample(id, L.data(), (int)L.size());
    if (!published) {
        LOGE("loadSampleFromCompressed: could not allocate slot %d for %zu frames", id, L.size());
        lastLoadFailure_ = LoadFailure::OUT_OF_MEMORY;
        return 0;
    }

    // A FLAC keeps its source depth, and the float decode above holds all of it. The depth is the 5 bits
    // straddling bytes 20–21: "fLaC", a 4-byte block header, then STREAMINFO — whose place as the first
    // block the format requires — with bits-per-sample minus one after the rate and channel fields.
    if (std::strcmp(ext, "flac") == 0) {
        if (FILE* ff = pt_fopen(path, "rb")) {
            uint8_t head[22];
            if (fread(head, 1, sizeof(head), ff) == sizeof(head) && std::memcmp(head, "fLaC", 4) == 0 &&
                (head[4] & 0x7F) == 0) {
                const int bits = (((head[20] & 0x01) << 4) | (head[21] >> 4)) + 1;
                if (bits > 16) setSampleSourceFormat(id, bits > 24 ? 32 : 24, false);
            }
            fclose(ff);
        }
    }

    lastLoadFailure_ = LoadFailure::NONE;
    LOGD("loadSampleFromCompressed: id=%d %zu frames %s rate=%d (%s)",
         id, L.size(), R.empty() ? "mono" : "stereo", sr, ext);
    return sr;
}

bool AudioEngine::hasStereoData(int id) {
    if (id < 0 || id >= 256) return false;
    return samplesRight[id] != nullptr;
}

// Declared in audio-engine.h, where the reason it is a member rather than a UI-side walk is written
// down. One line per buffer this class owns; a right-channel pointer is counted only when it exists,
// since a mono sample leaves it null and shares the one length. It lives in THIS file because tsf.h
// is only included here and in soundfont-voice.cpp.
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

// Sample editor operations live in sample-editor.cpp.

void AudioEngine::stopAll() {
    stopRampRequested.store(false, std::memory_order_relaxed);   // superseded: nothing is left to ramp
    for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].stop();
    }
    // Stop all soundfont notes on all tracks (incl. the preview lane)
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        sfVoices[t].hardStop();
    }
    // Transport stop rewinds every TIC00 table, router or sounding: PLAY starts at row 0.
    resetTic00Cursors();
    LOGD("stopAll: voices and SF notes cleared, stream stays running");
}

void AudioEngine::stopAllRamped() {
    stopRampRequested.store(true, std::memory_order_release);
}

void AudioEngine::startStopRamp(int64_t blockStartFrame) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].isActive) voices[i].startFadeOut(KILL_FADE_SAMPLES);
        else                    voices[i].stop();   // idle slot: clear any stale fade state
    }
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        if (sfVoices[t].isActive) sfVoices[t].startStopFade(KILL_FADE_SAMPLES);
        else                      sfVoices[t].hardStop();
    }
    resetTic00Cursors();   // as stopAll(): PLAY starts at row 0
    // The deadline the audio thread reclaims the slots at — see processAudioBlock, which is also
    // where it says why a fade counter alone does not get there.
    stopRampEndFrame.store(blockStartFrame + KILL_FADE_SAMPLES, std::memory_order_relaxed);
}

int AudioEngine::getActiveVoiceCount() {
    const VoiceView& view = voiceView();
    int count = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (view.sampler[i].active) count++;
    }
    return count;
}

void AudioEngine::getTrackActiveNotes(int* out, int trackCount) {
    for (int t = 0; t < trackCount; t++) out[t] = -1;
    const VoiceView& view = voiceView();
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!view.sampler[v].active) continue;
        int t = view.sampler[v].trackId;
        if (t >= 0 && t < trackCount && out[t] == -1) out[t] = view.sampler[v].note;
    }
    // ⚠️ THE SOUNDFONT POOL IS A SECOND VOICE POOL AND THE MONITOR HAS TO READ BOTH. `voices[]` holds
    // samplers only, so a track playing an SF2 instrument reported no note at all — the note column
    // beside the navigation map stayed blank for exactly the instruments TSF renders.
    //
    // Same encoding and the same fields as the sampler above: `resetTableState` copies the scheduled
    // note's octave and pitch onto the SF voice at every trigger. Sampler first — a sampler note on a
    // track supersedes an SF note still releasing on it (that is what the note-off at the sampler
    // trigger site means), so whichever pool answered first is the one still being played.
    for (int t = 0; t < SF_VOICE_COUNT && t < trackCount; t++) {
        if (out[t] == -1 && view.sf[t].active) {
            out[t] = view.sf[t].note;
        }
    }
}

int AudioEngine::getSampleRate() {
    // Cached from the platform backend at stream-open (setDeviceSampleRate). Defaults to 44100 until
    // then — matching every other fallback in the engine (schedulePitchBend, updateVoiceModulation,
    // the Kotlin layer); 48000 here once made rate/pitch math ~8.8% off if Kotlin cached this
    // before the stream opened.
    return deviceSampleRate.load(std::memory_order_relaxed);
}

// Flush-to-Zero eliminates denormal CPU stalls (10-100x slowdowns in reverb/delay/EQ feedback
// tails). See the declaration in audio-engine.h for why it is a member rather than a file-static:
// the offline sample-editor paths run the same DSP on the UI thread and must arm it too.
void AudioEngine::setFlushToZeroForCurrentThread() {
    thread_local bool done = false;
    if (done) return;
    done = true;
#if defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);  // FZ bit
    asm volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__arm__)
    uint32_t fpscr;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1U << 24);  // FZ bit
    asm volatile("vmsr fpscr, %0" : : "r"(fpscr));
#elif defined(PT_HAS_SSE_DENORMAL_CTRL)
    // x86/x86_64 — the emulator ABIs today, desktop Linux/Windows hosts now (ptrender), desktop
    // Linux later. FTZ (outputs) + DAZ (inputs) via MXCSR; both Android x86 ABIs and any x86_64
    // desktop have SSE3+.
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

// A TIC in the table's LAST ROW overrides the instrument's rate — per COLUMN, because each column
// has its own playhead. FX1 sets lane 0's rate, FX2 lane 1's, FX3 lane 2's, and a column with no TIC
// there keeps the instrument's.
//
// ⚠️ This is the one row where a TIC does not act when the playhead reaches it: it is read at TRIGGER,
// so the rate is in force from row 0. A TIC anywhere else takes effect when its lane arrives.
void AudioEngine::resetTic00Cursors() {
    for (int t = 0; t < SF_VOICE_COUNT; ++t) {
        for (int s = 0; s < TIC00_SLOTS; ++s) tic00Cursor[t][s] = Tic00Cursor();
        tic00Sounding[t] = -1;
    }
}

// ⚠️ A track runs out of slots only with a chain deeper than the cap plus a table per link, and then
// the OLDEST-numbered slot is reused rather than the lookup failing: one table forgetting its place
// costs a rotation restarting, where refusing the bookmark would cost the switch itself.
AudioEngine::Tic00Cursor* AudioEngine::tic00Slot(int trackId, int tableId, bool create) {
    if (trackId < 0 || trackId >= SF_VOICE_COUNT || tableId < 0) return nullptr;
    Tic00Cursor* row = tic00Cursor[trackId];
    for (int s = 0; s < TIC00_SLOTS; ++s)
        if (row[s].tableId == tableId) return &row[s];
    if (!create) return nullptr;
    for (int s = 0; s < TIC00_SLOTS; ++s) {
        if (row[s].tableId < 0) { row[s] = Tic00Cursor(); row[s].tableId = tableId; return &row[s]; }
    }
    row[0] = Tic00Cursor();
    row[0].tableId = tableId;
    return &row[0];
}

// Defined below, beside the table tick and the trigger that are their other callers.
static inline int tic00RowAfter(int tableRow, int lastProcessedRow);
static inline int tic00RowAfter(const TableLane& lane);
static int findTrackVoice(Voice* voices, int trackId, bool fading);

// A table row's three FX slots, as (type, value) pairs — slot 1 is index 0.
static inline void rowFx(const TableRow& r, int type[3], int value[3]) {
    type[0] = r.fx1Type; value[0] = r.fx1Value;
    type[1] = r.fx2Type; value[1] = r.fx2Value;
    type[2] = r.fx3Type; value[2] = r.fx3Value;
}

int AudioEngine::resolveChain(int trackId, int instrumentId, int tableIdOverride, int* outTableId) {
    int tableId = (tableIdOverride >= 0) ? tableIdOverride : instrumentId;
    int visited[CHAIN_MAX_LINKS];
    int visitedCount = 0;

    for (int link = 0; link < CHAIN_MAX_LINKS; ++link) {
        if (outTableId) *outTableId = tableId;

        // Silence beats a fallback: an empty slot and external gear both mean "this instrument
        // cannot answer", and a hit that quietly played something else is the bug that started this.
        // Link 0 is exempt from the empty test — the scheduler already made it, before any pushes.
        if (instrumentId < 0 || instrumentId >= songcore::PROGRAM_SLOTS) return -1;
        const songcore::Program& p = programs[instrumentId].program;
        if (p.type == songcore::PROGRAM_EXTERNAL) return -1;   // until the cable is a program too
        if (link > 0 && !p.hasSample && !p.hasSoundfont) return -1;

        // A table reached twice on one hit would loop forever; stop and sound where we stand.
        for (int i = 0; i < visitedCount; ++i) if (visited[i] == tableId) return instrumentId;
        if (tableId < 0 || tableId >= 256) return instrumentId;

        TableRow rows[16];
        if (!tables.read(tableId, rows)) return instrumentId;

        // A table with no INS anywhere in it cannot route, so it is left completely alone — no place
        // is kept for it and nothing below can touch how its voice plays it.
        bool routes = false;
        for (int r = 0; r < 16 && !routes; ++r) {
            int t3[3], v3[3];
            rowFx(rows[r], t3, v3);
            for (int s = 0; s < 3; ++s) routes |= (t3[s] == FX_INS);
        }
        if (!routes) return instrumentId;

        // ⚠️ **THE ROW IS THE ONE THE TABLE IS STANDING ON FOR THIS TRIGGER — THE SAME ROW THE VOICE
        // WOULD START AT, BY THE SAME RULE.** There is ONE place per table, not a separate one for
        // routing: at TIC00 it steps on a row per trigger (which is the rotation) and at every other
        // rate it starts at row 0 each note, exactly as a table with no INS in it does. So when no
        // switch is found, the voice below picks up this same row and that row's transpose, volume
        // and FX apply to the note — they are in the path, not part of the switch.
        // Row 15's TIC overrides the instrument's rate per column — `effectiveTicRatesFor`'s rule,
        // read off the copy already in hand rather than taking the table lock a second time.
        int rates[TABLE_LANES] = {p.tableTicRate, p.tableTicRate, p.tableTicRate};
        if (rows[15].fx1Type == FX_TIC) rates[0] = rows[15].fx1Value;
        if (rows[15].fx2Type == FX_TIC) rates[1] = rows[15].fx2Value;
        if (rows[15].fx3Type == FX_TIC) rates[2] = rows[15].fx3Value;

        const Tic00Cursor* cur = tic00Slot(trackId, tableId, /*create=*/false);
        const int liveVoice = findTrackVoice(voices, trackId, /*fading=*/false);

        int laneRow[TABLE_LANES] = {0, 0, 0};
        for (int l = 0; l < TABLE_LANES; ++l) {
            if (rates[l] != 0x00) continue;   // not a per-trigger column: it starts at the top
            if (liveVoice >= 0 && voices[liveVoice].tableId == tableId &&
                voices[liveVoice].lanes[l].ticRate == 0x00) {
                laneRow[l] = tic00RowAfter(voices[liveVoice].lanes[l]) & 0x0F;
            } else if (cur && cur->ticRate[l] == 0x00 && cur->active[l]) {
                laneRow[l] = tic00RowAfter(cur->row[l], cur->lastProcessed[l]) & 0x0F;
            }
        }

        // HOP steers the rotation, so a router follows it before it reads anything on the row —
        // per COLUMN, like everywhere else, to the row in its low nibble. `HOP FF` stops its column
        // and needs nothing here: the row it stands on carries no switch. The repeat count does not
        // carry between hits, because `reset_table_lanes` places a lane fresh at every trigger, so a
        // counted HOP jumps on every hit exactly as one in a TIC00 table does.
        for (int l = 0; l < TABLE_LANES; ++l) {
            uint16_t seen = 0;
            for (;;) {
                if (seen & (uint16_t)(1u << laneRow[l])) break;   // round again: stand where we are
                seen |= (uint16_t)(1u << laneRow[l]);
                int t3[3], v3[3];
                rowFx(rows[laneRow[l]], t3, v3);
                if (t3[l] != FX_HOP || v3[l] == 0xFF) break;
                laneRow[l] = v3[l] & 0x0F;
            }
        }

        // ⚠️ **EACH COLUMN IS READ AT ITS OWN ROW**, the rule the rest of the table already follows.
        // At TIC 00 — the mode this feature is for — all three stand on the same row anyway.
        int type[3], value[3], slotRow[3];
        for (int s = 0; s < 3; ++s) {
            int t3[3], v3[3];
            rowFx(rows[laneRow[s]], t3, v3);
            type[s] = t3[s]; value[s] = v3[s]; slotRow[s] = laneRow[s];
        }

        // CHA decides whether the switch fires at all: on a failed roll its low nibble clears that
        // slot, and a cleared INS is simply a row with no switch on it.
        for (int s = 0; s < 3; ++s) {
            if (type[s] != FX_CHA) continue;
            const int odds  = (value[s] >> 4) & 0x0F;
            const int target = value[s] & 0x0F;
            if (static_cast<int>(xorshift32(chainRngState) % 15u) < odds) continue;   // passed
            if (target >= 1 && target <= 3) type[target - 1] = 0;
        }

        // RNL adds a random 0..xx to the slot on its LEFT, so an RNL beside an INS IS the instrument
        // number — the pool-picker. Read wherever it sits: it configures the switch, it is not in
        // the path through the row.
        for (int s = 1; s < 3; ++s) {
            if (type[s] != FX_RNL || type[s - 1] != FX_INS) continue;
            const int range = value[s] & 0xFF;
            if (range > 0) value[s - 1] += static_cast<int>(xorshift32(chainRngState) % (uint32_t)(range + 1));
            if (value[s - 1] > 127) value[s - 1] = 127;   // INS's own ceiling
        }

        // ⚠️ **THE ROTATION MOVES ON EVERY HIT, SWITCH OR NO SWITCH**, and it is written before the
        // row is judged for exactly that reason: a row with no INS on it would otherwise freeze the
        // table on that row, and every later hit would read it again and never reach the rows past it.
        // ⚠️ **LEFTMOST WINS** — the signal leaves at the first switch it meets, so anything past it
        // on the row is not part of the path.
        int next = -1;
        for (int s = 0; s < 3; ++s) {
            if (type[s] == FX_INS) { next = value[s] & 0x7F; break; }
        }
        // No switch here, so this instrument sounds and its voice runs this table from `laneRow` —
        // which is where the voice would have started anyway, so nothing has to be handed down.
        if (next < 0) return instrumentId;

        // ⚠️ **THE HIT LEAVES, SO NOTHING ELSE WILL MOVE THIS TABLE ON.** No voice runs it, so the
        // step the voice would have made has to be made here or the next trigger reads the same row
        // for ever. Only a per-trigger column has a place to keep; the others start at the top anyway.
        for (int l = 0; l < TABLE_LANES; ++l) {
            if (rates[l] != 0x00) continue;
            Tic00Cursor* c = tic00Slot(trackId, tableId, /*create=*/true);
            if (!c) break;
            c->row[l]           = slotRow[l];
            c->lastProcessed[l] = slotRow[l];   // "this row has been used" — the next trigger steps on
            c->ticRate[l]       = 0x00;
            c->active[l]        = true;
        }
        if (visitedCount < CHAIN_MAX_LINKS) visited[visitedCount++] = tableId;

        instrumentId = next;
        tableId      = next;   // the next link brings its OWN table
    }

    // The depth cap was hit. Sound where we stand rather than following further.
    if (outTableId) *outTableId = tableId;
    if (instrumentId < 0 || instrumentId >= songcore::PROGRAM_SLOTS) return -1;
    const songcore::Program& last = programs[instrumentId].program;
    if (last.type == songcore::PROGRAM_EXTERNAL) return -1;
    if (!last.hasSample && !last.hasSoundfont) return -1;
    return instrumentId;
}

void AudioEngine::effectiveTicRatesFor(int tableId, int fallback, int out[TABLE_LANES]) {
    for (int l = 0; l < TABLE_LANES; ++l) out[l] = fallback;
    if (tableId < 0 || tableId >= 256) return;
    TableRow rows[16];
    if (!tables.read(tableId, rows)) return;
    const TableRow& lastRow = rows[15];
    if (lastRow.fx1Type == FX_TIC) out[0] = lastRow.fx1Value;
    if (lastRow.fx2Type == FX_TIC) out[1] = lastRow.fx2Value;
    if (lastRow.fx3Type == FX_TIC) out[2] = lastRow.fx3Value;
}

// Scale an interleaved stereo buffer by a gain moving linearly from `from` to `to`, reaching `to` on
// the last frame — the same per-sample ramp the mute gate uses.
static inline void applyGainRamp(float* buf, int frames, float from, float to) {
    for (int i = 0; i < frames; i++) {
        const float g = from + (to - from) * (float)(i + 1) / (float)frames;
        buf[i * 2]     *= g;
        buf[i * 2 + 1] *= g;
    }
}

// Per-voice-type table FX behaviour, resolved at compile time inside processTableTick:
//   KIL:    sampler = declicked kill fade; SF = noteOff (TSF plays its own release).
//   OFFSET: sampler repositions playback; SF voices have no sample position — ignored.
static inline void tableKill(Voice& v)          { v.startFadeOut(KILL_FADE_SAMPLES); }
static inline void tableKill(SoundfontVoice& v) { v.noteOff(); }
static inline void tableOffset(Voice& v, uint8_t fxValue) {
    if (v.sampleLength > 0) {
        double normalizedPos = fxValue / 255.0;
        v.position = normalizedPos * (v.sampleLength - 1);
    }
}
static inline void tableOffset(SoundfontVoice&, uint8_t) {}

// ─── CUT / RES — the one place a live filter override lands ───────────────────────────────────────
//
// Shared by the param-queue arm (the FX column and an AUS/AUF ramp) and the table's own CUT/RES rows,
// for both voice types.
//
// ⚠️ **INERT ON A VOICE RUNNING NO FILTER** (`type == 0`, i.e. the instrument's FILTER TYPE is OFF).
// These move the filter the instrument declares; they do not switch one on.
//
// ⚠️ **THE PARAM BUS IS WHERE THE LIVE VALUES LIVE, AND THE SF VOICE NEEDS A SECOND WRITE.** The
// sampler's per-block mod recompute reads `params.get()`, but the SF's reads `instrParams +
// modDestValues` — so an SF voice given only the bus would have the old value back a block later.
// `filterStore` below is that difference and the only thing that differs per voice type.
//
// The override is per-note by construction: a note-on rebuilds the chain from the instrument, so
// nothing has to be restored when the note ends.
static inline void filterStore(Voice& v, int cut, int res) {
    v.params.setBase(PARAM_FILTER_CUT, (float)cut);
    v.params.setBase(PARAM_FILTER_RES, (float)res);
}
static inline void filterStore(SoundfontVoice& v, int cut, int res) {
    v.params.setBase(PARAM_FILTER_CUT, (float)cut);
    v.params.setBase(PARAM_FILTER_RES, (float)res);
    v.instrParams.filterCut = cut;   // what THIS voice type's per-block recompute reads
    v.instrParams.filterRes = res;
}

template <typename V>
static inline void voiceSetFilter(V& v, int cut, int res, float sampleRate) {
    filterStore(v, cut, res);
    if (!v.chain.filter.enabled()) return;
    // Modulation still applies on top — the same sum the per-block recompute makes, so a CUT under an
    // LFO moves the centre the LFO swings around instead of fighting it for one block.
    int modCut = std::max(0, std::min(255, (int)(cut + v.modDestValues[PARAM_FILTER_CUT])));
    int modRes = std::max(0, std::min(255, (int)(res + v.modDestValues[PARAM_FILTER_RES])));
    v.chain.filter.setParams(v.chain.filter.type, modCut, modRes, v.chain.filter.drive, (int)sampleRate);
}
// CUT and RES are one FilterModule call, so each carries the other's CURRENT value through. Read off
// the bus, which both voice types seed from the instrument at trigger.
template <typename V> static inline void voiceSetFilterCut(V& v, int cut, float sr) {
    voiceSetFilter(v, cut, (int)v.params.base[PARAM_FILTER_RES], sr);
}
template <typename V> static inline void voiceSetFilterRes(V& v, int res, float sr) {
    voiceSetFilter(v, (int)v.params.base[PARAM_FILTER_CUT], res, sr);
}

// LPF / HPF / BPF — the one write that switches a filter ON.
//
// ⚠️ **IT DELIBERATELY SKIPS `voiceSetFilter`'s `enabled()` GUARD**, which is the whole difference:
// that guard is what makes CUT and RES inert on an instrument whose FILTER TYPE is OFF, and turning
// the filter on is what these three are for. Resonance rides along at whatever the voice currently
// holds — the instrument's, or the last RES — because one FilterModule call takes all four numbers
// and passing a fresh 0 here would silently undo a RES written on the step before.
//
// Per-note like the other two: a note-on rebuilds the chain from the instrument, so a filter opened
// from a cell is gone by the next note with nothing to restore.
template <typename V> static inline void voiceSetFilterMode(V& v, int type, int cut, float sr) {
    const int res = (int)v.params.base[PARAM_FILTER_RES];
    filterStore(v, cut, res);
    int modCut = std::max(0, std::min(255, (int)(cut + v.modDestValues[PARAM_FILTER_CUT])));
    int modRes = std::max(0, std::min(255, (int)(res + v.modDestValues[PARAM_FILTER_RES])));
    v.chain.filter.setParams(type, modCut, modRes, v.chain.filter.drive, (int)sr);
}

// ─── DRV / CRU — two writes onto the per-block recompute's own inputs ────────────────────
//
// ⚠️ **THE SAMPLER RE-DERIVES BOTH FROM `params.base` EVERY BLOCK AND THE SF VOICE DOES NOT**,
// and that is the whole difference between the overloads below — the same split `filterStore` makes,
// for the same reason. A base write IS the command on a sampler voice; on an SF voice, whose chain is
// set once at trigger, the module has to be written or the value would never be read at all.
//
// Per-note by construction: a note-on reseeds the bus and rebuilds the chain from the instrument, so
// nothing is restored when the note ends.

// LPO. ⚠️ **IT ADDS, WHERE EVERY OTHER SETTER ON THIS PAGE ASSIGNS** — the byte is a signed STEP in
// sixteenths of the loop's own length, and the voice keeps the running total. Written on a table row
// it therefore walks the window a step per tic, which is the shape the technique is actually used in.
// Sampler-only, silently: a SoundFont voice has no sample position to slide.
static inline void voiceSlideLoop(Voice& v, int byteValue) {
    v.loopSlideSixteenths += loopSlideSixteenthsOf(byteValue);
}
static inline void voiceSlideLoop(SoundfontVoice&, int) {}

static inline void voiceSetDrive(Voice& v, int drive) {
    v.params.setBase(PARAM_DRIVE, (float)drive);
}
static inline void voiceSetDrive(SoundfontVoice& v, int drive) {
    v.instrParams.drive = drive;   // this voice's own copy: what a later reset would read back
    v.chain.drive.setDrive(drive);
}

// ⚠️ The cell is TWO numbers. The sampler passes 0 for downsample at the chain and quantizes the read
// address instead — a different effect from the module's, and the reason the base write must carry
// both halves rather than being handed to `crush.setParams` here.
static inline void voiceSetCrush(Voice& v, int packed) {
    v.params.setBase(PARAM_CRUSH,      (float)crushBitsOf(packed));
    v.params.setBase(PARAM_DOWNSAMPLE, (float)crushDownsampleOf(packed));
}
static inline void voiceSetCrush(SoundfontVoice& v, int packed) {
    v.instrParams.crush      = crushBitsOf(packed);
    v.instrParams.downsample = crushDownsampleOf(packed);
    v.chain.crush.setParams(v.instrParams.crush, v.instrParams.downsample);
}

// ─── REV / DEL — the send levels, the same per-voice-type split ──────────────────────────────────
static inline void voiceSetSends(Voice& v, float rev, float dly) {
    v.reverbSend = rev;
    v.delaySend  = dly;
}
static inline void voiceSetSends(SoundfontVoice& v, float rev, float dly) {
    v.instrParams.reverbSend = rev;   // the SF send tap reads this copy, not the engine's
    v.instrParams.delaySend  = dly;
}

// ─── A SOUNDING VOICE RE-READS ITS INSTRUMENT ────────────────────────────────────────────────────
//
// ⚠️ **A TRIGGER COPIES THE INSTRUMENT INTO THE VOICE**, which is what makes an edit to the filter,
// the drive, the crush or the sends silent until the next note — the note you are hearing is running
// on the copy it took. This is the same write the FX arms above make, with the values coming from the
// instrument instead of a cell, and it is aimed at an INSTRUMENT rather than a track: a knob names a
// parameter of instrument 3, not of whatever track 3 happens to be playing.
//
// ⚠️ It therefore ENDS a live override an FX cell had made on those same parameters — the hand that
// just moved the instrument wins, exactly as it does everywhere else a press meets a running take.
template <typename V>
static inline void voiceReloadInstrument(V& v, const InstrumentParams& ip, float sampleRate) {
    // Mode before resonance: the mode call is the one that can switch the filter ON, and it carries
    // whatever resonance the voice holds — so the resonance write has to come after it, not before.
    voiceSetFilterMode(v, ip.filterType, ip.filterCut, sampleRate);
    voiceSetFilterRes(v, ip.filterRes, sampleRate);
    voiceSetDrive(v, ip.drive);
    voiceSetCrush(v, ((ip.crush & 0x0F) << 4) | (ip.downsample & 0x0F));
    voiceSetSends(v, ip.reverbSend, ip.delaySend);
}

// ─── FIN — the one command that lands in a bus slot the voices ALREADY reset ─────────────────────
//
// ⭐ It needs no field of its own: `PARAM_PITCH`'s BASE has been written to zero by both voice types'
// trigger paths since the bus existed and read by neither, while the MOD half carries the table
// transpose, the slides and the vibrato. Putting the fine tune in the base is therefore per-note by
// construction — a trigger clears it — and it cannot collide with any of those.
//
// ⚠️ Both voice types have to READ it, and they read it in different places: the sampler folds it
// into the playback rate (`getModulatedPlaybackRate`), the SoundFont voice into the pitch wheel
// (`soundfont-voice.cpp`), which also has to count it as active pitch or it re-centres the wheel.
template <typename V> static inline void voiceSetFineTune(V& v, int byteValue) {
    v.params.setBase(PARAM_PITCH, fineTuneSemitonesOf(byteValue));
}

/** A 0-1 CC value back to the 00-FF byte the author typed. */
static inline int filterByteOf(float value) {
    return std::max(0, std::min(255, (int)(value * 255.0f + 0.5f)));
}

// The row a TIC00 retrigger continues from.
//
// ⚠️ The NEXT row — unless the one the voice is standing on has not been applied yet, in which case
// it is that row again. `lastProcessedRow` is the record of consumption, and it can disagree with
// `tableRow` three ways: a voice triggered but not yet ticked, a HOP target, a THO write. Stepping
// past a row in any of them drops the row entirely — and a dropped HOP row lets the table walk on
// past its loop point. processTableTick consumes at most one row per voice per audio BLOCK while
// notes arrive per FRAME, so two triggers inside one block reach this with the row still pending.
static inline int tic00RowAfter(int tableRow, int lastProcessedRow) {
    return (lastProcessedRow != tableRow) ? tableRow : (tableRow + 1) % 16;
}
static inline int tic00RowAfter(const TableLane& lane) {
    return tic00RowAfter(lane.row, lane.lastProcessed);
}

// Special TIC modes:
//   TIC00 (0x00): Trigger mode — table row set by note, doesn't advance automatically
//   TICFC (0xFC): Octave map — row = triggered note's octave (0-9)
//   TICFE (0xFE): Note map — row = triggered note's pitch (0-11)
//   TICFF (0xFF): 200Hz mode — advance ~1 row per 5ms
template <typename V>
void AudioEngine::processTableTick(V& voice, int numFrames, float sampleRate) {
    // ⚠️ THE WHOLE TABLE, not just the current row, because the AUS/AUF pairing below is re-derived
    // from every row on every block — that is what lets a backwards HOP resume a ramp mid-span with
    // nothing stored per voice.
    TableRow rows[16];
    if (!tables.read(voice.tableId, rows)) return;

    // How far each lane is through the row it is standing on, for the ramp's sub-row interpolation.
    // 0 in the three non-advancing TIC modes, which hold the row still by design.
    double rowFraction[TABLE_LANES] = {0.0, 0.0, 0.0};

    // ⚠️ **THE RATE MACHINE RUNS ONCE PER LANE, AND NOTHING IN IT IS SHARED.** A lane at TICFF next
    // to a lane at TIC 06 is the point of the feature; a single accumulator would make the faster
    // one drag the slower.
    for (int lane = 0; lane < TABLE_LANES; ++lane) {
        TableLane& L = voice.lanes[lane];
        if (!L.active) continue;   // this column executed HOP FF; the others carry on

        bool shouldProcessRow = false;
        bool shouldAdvance = false;

        if (L.ticRate == 0x00) {
            // TIC00: Trigger mode - apply row effects ONCE, don't advance automatically
            shouldProcessRow = (L.row != L.lastProcessed);
            shouldAdvance = false;
        } else if (L.ticRate == 0xFC || L.ticRate == 0xFE) {
            // TICFC/TICFE: Static mapping modes - row is fixed, process ONCE
            shouldProcessRow = (L.row != L.lastProcessed);
            shouldAdvance = false;
        } else if (L.ticRate == 0xFF) {
            // TICFF: 200Hz mode - faster advancement
            L.tic200Accum += numFrames;
            float samplesPerTic = sampleRate / 200.0f;
            if (L.tic200Accum >= samplesPerTic) {
                L.tic200Accum -= samplesPerTic;
                shouldProcessRow = true;
                shouldAdvance = true;
            }
            if (samplesPerTic > 0.0f) rowFraction[lane] = L.tic200Accum / samplesPerTic;
        } else {
            // Standard tic mode (01-FB): advance one row every `ticRate` musical tics.
            // Frame-accurate and tempo-locked (like the TICFF branch above) so table speed tracks
            // the sequencer, is identical live vs. offline render, and is independent of the audio
            // buffer size. framesPerTic = sr / (BPM/60 · 4 steps/beat · 12 tics/step).
            if (L.lastProcessed == -1) {
                // Fire the first tic AT trigger so row 0's transpose/vol/FX apply immediately.
                // Otherwise the voice plays one full row-duration with no table effect, which sounds
                // like the first row lasts twice as long. Mirrors TIC00's note-on processing.
                L.frameAccum = 0.0f;
                shouldProcessRow = true;
                shouldAdvance = true;
            } else {
                int tempo = currentTempo.load(std::memory_order_relaxed);
                float framesPerRow = sampleRate / (tempo / 60.0f * 4.0f * 12.0f) * (float)L.ticRate;
                L.frameAccum += numFrames;
                if (framesPerRow > 0.0f && L.frameAccum >= framesPerRow) {
                    L.frameAccum -= framesPerRow;
                    // A block longer than one row (very fast tables) can't advance >1 row here, so
                    // drop the extra rather than banking it (which would run away). Normal tic rates
                    // have framesPerRow >> block, so the remainder carries and the rate stays exact.
                    if (L.frameAccum >= framesPerRow) L.frameAccum = 0.0f;
                    shouldProcessRow = true;
                    shouldAdvance = true;
                }
                if (framesPerRow > 0.0f) rowFraction[lane] = L.frameAccum / framesPerRow;
            }
        }

        // ⚠️ A HOP or THO does not consume the tic — it moves the lane and the row it lands on plays
        // here, in this same tic. Bounded at one table's worth of rows so a HOP onto itself, or a
        // ring of them, cannot spin the audio thread; a ring with no playable row simply sounds
        // nothing, which is what a table of pure jumps deserves.
        for (int steered = 0; shouldProcessRow && steered <= 16; ++steered)
            if (!processTableRow(voice, rows[L.row], lane, shouldAdvance, sampleRate)) break;
        // ⚠️ A HOP FF in an EARLIER lane can have cleared tableId this same block. Stop reading the
        // table copy the moment it does — the remaining lanes are already down.
        if (voice.tableId < 0) break;
    }

    // ⚠️ THE RAMP IS EVALUATED AGAINST `lastProcessed`, NOT the lane's current row. The row whose
    // effects are in force is the one that was last consumed — the cursor has already been advanced
    // (or HOPped) to the one that comes NEXT, and reading it would run every fade a whole row ahead
    // of what is being heard. `frameAccum` is the progress through that same consumed row, so the
    // pair is consistent by construction.
    //
    // ⚠️ And AFTER the row work, so a table that just executed `HOP FF` in every lane runs no ramp.
    if (voice.tableId >= 0) applyTableRamps(voice, rows, rowFraction, sampleRate);
}

// One lane consuming one row.
//
// ⚠️ **THE TRANSPOSE AND VOLUME COLUMNS BELONG TO LANE 0, and only lane 0 applies them.** They share
// FX1's playhead by definition — that is what "lane A" means — so running them from any other lane
// would rewrite the note's pitch at FX2's rate.
//
// ⚠️ And a lane reads **exactly one** FX slot: its own. `KIL VOL OFFSET CUT RES EQN EQM` are global
// effects any column may carry, but `HOP`, `TIC` and `THO` steer the lane they are written in.
//
// ⚠️⚠️ **A ROW THAT STEERS THE LANE IS NEVER HEARD.** `HOP` and `THO` have no tic of their own: the
// lane resolves them and the row they land on is the row that plays. Letting one play would put a
// tic of the table's DEFAULTS in front of every loop — no transpose, no filter, full volume — which
// is an untransposed grace note before the transposed one, and a burst of raw sample under a closed
// filter. The caller re-enters on `true` until a row actually plays.
template <typename V>
bool AudioEngine::processTableRow(V& voice, const TableRow& row, int lane, bool shouldAdvance,
                                  float sampleRate) {
    TableLane& L = voice.lanes[lane];

    const uint8_t laneFxType = (lane == 0) ? row.fx1Type : (lane == 1) ? row.fx2Type : row.fx3Type;
    const bool steers = (laneFxType == FX_HOP || laneFxType == FX_THO);

    if (lane == 0 && !steers) {
        // playbackRate does not include transpose; getModulatedPlaybackRate reads
        // modDestValues[PARAM_PITCH] which processRoutes accumulates from TABLE_PITCH.
        int semitones = transposeToSemitones(row.transpose);
        voice.tableTranspose = (float)semitones;  // kept for debug log
        voice.modSourceValues[MOD_SRC_TABLE_PITCH] = (float)semitones;

        // Mix loop reads modDestValues[PARAM_VOL] instead of voice.tableVolume.
        if (row.volume == 0xFF) {
            voice.tableVolume = 1.0f;  // kept for debug log
        } else {
            voice.tableVolume = row.volume / 255.0f;
        }
        voice.modSourceValues[MOD_SRC_TABLE_VOL] = voice.tableVolume;
    }

    bool hopExecuted = false;
    int hopTarget = -1;

    auto processEffect = [&](uint8_t fxType, uint8_t fxValue) {
        switch (fxType) {
            case FX_KILL:
                if (fxValue == 0x00) {
                    tableKill(voice);
                    LOGT("📋 Table effect: KILL track %d", voice.getTrackId());
                }
                break;

            case FX_HOP:
                // HOP XY: X=repeat count (0=infinite), Y=target row; FF=stop THIS COLUMN
                if (fxValue == 0xFF) {
                    // ⚠️ **THE LANE, NOT THE TABLE.** The voice's table only ends once all three
                    // columns have stopped — a HOP FF typed in FX3 must not silence the note and
                    // volume columns, which is exactly the coupling per-column playback removes.
                    L.active = false;
                    L.hopTarget = -1;
                    L.hopRepeat = 0;
                    bool anyRunning = false;
                    for (int o = 0; o < TABLE_LANES; ++o) anyRunning |= voice.lanes[o].active;
                    if (!anyRunning) voice.tableId = -1;
                    LOGT("📋 Table HOP FF: stopped column %d for track %d%s", lane + 1,
                         voice.getTrackId(), anyRunning ? "" : " (table ended)");
                } else {
                    int repeatCount = (fxValue >> 4) & 0x0F;  // High nibble = X
                    int targetRow = fxValue & 0x0F;           // Low nibble = Y

                    if (repeatCount == 0) {
                        // HOP 0Y = Infinite loop to row Y
                        hopExecuted = true;
                        hopTarget = targetRow;
                        LOGT("📋 Table HOP %02X: infinite loop to row %d, track %d", fxValue, targetRow, voice.getTrackId());
                    } else {
                        // HOP XY (X>0) = Jump X times, then continue
                        if (L.hopTarget == -1 || L.hopTarget != targetRow) {
                            L.hopRepeat = repeatCount;
                            L.hopTarget = targetRow;
                            LOGT("📋 Table HOP %02X: initialized counter=%d, target=%d, track %d",
                                 fxValue, repeatCount, targetRow, voice.getTrackId());
                        }

                        if (L.hopRepeat > 0) {
                            L.hopRepeat--;
                            hopExecuted = true;
                            hopTarget = targetRow;
                            LOGT("📋 Table HOP: jump to row %d, %d jumps remaining, track %d",
                                 targetRow, L.hopRepeat, voice.getTrackId());
                        } else {
                            // Counter exhausted, don't jump, reset state and continue normally
                            L.hopTarget = -1;
                            LOGT("📋 Table HOP: counter exhausted, continuing past row, track %d", voice.getTrackId());
                        }
                    }
                }
                break;

            case FX_VOLUME:
                voice.tableVolume = fxValue / 255.0f;
                voice.modSourceValues[MOD_SRC_TABLE_VOL] = voice.tableVolume;
                break;

            case FX_OFFSET:
                tableOffset(voice, fxValue);
                break;

            // CUT / RES on a table row: the same per-voice write the FX column makes, once per tic —
            // a sweep that follows every note the instrument plays without being written per phrase.
            case FX_CUT:
                voiceSetFilterCut(voice, fxValue, sampleRate);
                break;

            case FX_RES:
                voiceSetFilterRes(voice, fxValue, sampleRate);
                break;

            // LPF / HPF / BPF on a table row. The reason they belong here as much as in a phrase: a
            // table follows the INSTRUMENT, so one row gives every note that instrument ever plays a
            // filter — including the CUT and RES rows above it, which without one are inert.
            case FX_LPF: voiceSetFilterMode(voice, 1, fxValue, sampleRate); break;
            case FX_HPF: voiceSetFilterMode(voice, 2, fxValue, sampleRate); break;
            case FX_BPF: voiceSetFilterMode(voice, 3, fxValue, sampleRate); break;

            // DRV / CRU on a table row — the same per-voice writes the FX column makes, once per
            // tic. A table is where a dirt that rises while the note holds is actually written,
            // because it wants a value per tic rather than one per step.
            case FX_DRV: voiceSetDrive(voice, fxValue);     break;
            case FX_CRU: voiceSetCrush(voice, fxValue);     break;

            // FIN on a table row — a tuning per tic, which is where a chorus or a drifting detune is
            // actually written. ⚠️ It shares no state with the table's TRANSPOSE column: that column
            // drives the MOD half of the same bus slot and this writes the BASE, so the two add.
            case FX_FIN: voiceSetFineTune(voice, fxValue);  break;

            // LPO on a table row, which is where the slide is most of the point: a row under a HOP
            // walks the loop window a step per tic for as long as the note holds, and that walk is
            // the drone, the timestretch and the wavetable scan. ⚠️ It ACCUMULATES — a row that fires
            // 100 tics has moved the window 100 steps, unlike every other arm here.
            case FX_LPO: voiceSlideLoop(voice, fxValue);    break;

            // EQN / EQM on a table row: the same two writes the FX column's EQN and EQM make, once
            // per tic, reached directly rather than through the param queue because the voice is
            // already in hand — the queue's only job on that path is finding it.
            //
            // ⚠️ THE TWO HAVE DIFFERENT LIFETIMES, and only one of them cleans up after itself. EQN
            // writes THIS voice's chain and dies with the note, because a note-on rebuilds the chain
            // from the instrument. EQM writes the MASTER BUS, which outlives every voice and the
            // table with them — so it is armed for the restore on stop() the phrase-level EQM gets
            // from the scheduler's own flag (songcore/scheduler.h eqm_active, host.h stop). Without
            // the arming the bus keeps the table's preset after the transport stops, and the FX
            // helper's "resets to mixer EQ on stop" would be false for exactly this one way in.
            case FX_EQN:
                applyEqPresetToModule(voice.chain.eq, fxValue);
                break;

            case FX_EQM:
                applyEqPresetToModule(masterChain.masterEq, fxValue);   // not setMasterEqSlot: a table's
                tableMasterEqTouched.store(true, std::memory_order_relaxed);   // override is not the song's
                break;

            // TIM on a table row — the delay's echo time, once per tic, which is where the command is
            // most of the fun: a row under a HOP walks the time a step per tic, and the head glides to
            // each one, so the repeats bend continuously.
            //
            // ⚠️ IT IS GLOBAL AND THE VOICES ARE NOT. Two voices standing on different rows of the same
            // table write it in turn and the last one in the block wins — exactly what EQM above does,
            // and for the same reason: a shared bus reached from a per-voice walk. A TIM belongs in a
            // table one instrument drives, not in one eight tracks share.
            //
            // ⚠️ Latched like EQM, and for the identical reason: the send outlives every voice, so
            // stop() has to put the DELAY screen's own time back (host.h).
            case FX_TIM:
                delaySend.setTimeFree(fxValue);
                tableDelayTimeTouched.store(true, std::memory_order_relaxed);
                break;

            case FX_TIC:
                // The rate of the COLUMN it is written in — that is what lets one table carry two
                // speeds at once. (Row 15's TIC is read at trigger instead; effectiveTicRatesFor.)
                if (fxValue >= 0x01 && fxValue <= 0xFB) {
                    L.ticRate = fxValue;
                    LOGT("📋 Table effect: TIC %02X - column %d now advances every %d tics",
                         fxValue, lane + 1, fxValue);
                }
                break;

            case FX_THO:
                hopExecuted = true;
                hopTarget = fxValue & 0x0F;
                LOGT("📋 Table THO %02X: hop to row %d, track %d", fxValue, hopTarget, voice.getTrackId());
                break;

            default:
                break;
        }
    };

    if (lane == 0)      processEffect(row.fx1Type, row.fx1Value);
    else if (lane == 1) processEffect(row.fx2Type, row.fx2Value);
    else                processEffect(row.fx3Type, row.fx3Value);

    L.lastProcessed = L.row;

    if (hopExecuted && hopTarget >= 0) {
        L.row = hopTarget % 16;
        LOGT("📋 Table HOP: track %d column %d jumped to row %d", voice.getTrackId(), lane + 1, L.row);
        return L.active && voice.tableId >= 0;
    }
    // A HOP whose repeat count is spent still does not play: it is stepped past, and the row after
    // it is the one that plays. ⚠️ Unconditionally, NOT under `shouldAdvance` — a lane that does not
    // advance on its own (TIC 00 and the two map modes) would otherwise sit on a row that can never
    // sound.
    if (steers && L.active) {
        L.row = (L.row + 1) % 16;
        return voice.tableId >= 0;
    }
    if (shouldAdvance) {
        L.row = (L.row + 1) % 16;
    }

    if (lane == 0 && shouldAdvance && L.row == 0) {
        LOGT("📋 Table %d loop: track=%d, transpose=%.0f, vol=%.2f",
             voice.tableId, voice.getTrackId(), voice.tableTranspose, voice.tableVolume);
    }
    return false;
}

// ─── AUS / AUF on a table row ────────────────────────────────────────────────────────────────────
//
// The pairing and the curve are both shared — `table_automation.h` runs the same walk the TABLE
// editor dims cells from, and `automation_curve.h` is the same polynomial the phrase path emits
// through, so a table morph and a phrase morph over the same two presets land on the same bytes.
// What is here is only the apply: the five effects a table row can both carry and ramp.
//
// ⚠️ **EVERY BLOCK, NOT EVERY ROW** — that is the whole reason processTableTick was split. A ramp
// re-evaluated only on a row change would be sixteen values, the sub-row interpolation would be dead
// code, and a slow morph would step audibly. The cost is one 48-slot walk plus, for an EQ ramp, six
// `powf` per voice per block; at eight voices that is a fraction of a percent of a core, and it buys
// a fade instead of a staircase.
template <typename V>
void AudioEngine::applyTableRamps(V& voice, const TableRow* rows,
                                  const double (&rowFraction)[TABLE_LANES], float sampleRate) {
    const table_automation::TableRampSet ramps = table_automation::find_table_ramps(rows, 16);

    for (int i = 0; i < ramps.count; ++i) {
        const table_automation::TableRamp& r = ramps.items[i];
        // ⚠️⚠️ **A RAMP RUNS ON THE COLUMN ITS PARAMETER IS IN, NOT THE ONE ITS AUS IS IN.** AUS pairs
        // by looking LEFT along its own row, so the value being faded always sits in a lower slot
        // than the curve — every ramp spans at least two columns, and with three playheads there are
        // two candidate clocks. The parameter's is the only one that cannot fight itself: that same
        // cell is re-applied on its own column's tic, and driving the fade from the AUS's column
        // would have the two writing one destination at two rates. (In practice the parameter is in
        // FX1, so a fade written before per-column playback existed keeps running on lane 0.)
        const TableLane& L = voice.lanes[r.paramSlot - 1];
        if (!L.active || L.lastProcessed < 0) continue;
        const double t = table_automation::table_ramp_position(r, L.lastProcessed,
                                                               rowFraction[r.paramSlot - 1]);
        if (t < 0.0) continue;   // this ramp does not cover the row that column is standing on

        if (r.eqPreset) {
            // ⚠️ Clamped rather than trusted, as the phrase morph is: pairing refuses an endpoint
            // that is not a slot, so the authored path cannot produce one — but a hand-edited
            // project file is not the authored path, and an unchecked index reads off the bank.
            if (!table_automation::is_eq_slot(r.startByte) ||
                !table_automation::is_eq_slot(r.destByte)) continue;
            const EqBandsHex& from = eqPresetHex[r.startByte];
            const EqBandsHex& to   = eqPresetHex[r.destByte];
            EqBandsHex m;
            for (int b = 0; b < 3; ++b) {
                const songcore::AutomationEqBand v = songcore::automation_eq_band_at(
                        { from.type[b], from.freq[b], from.gain[b], from.q[b] },
                        { to.type[b],   to.freq[b],   to.gain[b],   to.q[b]   }, r.curveByte, t);
                m.type[b] = v.type;
                m.freq[b] = v.freq;
                m.gain[b] = v.gain;
                m.q[b]    = v.q;
            }
            if (r.fxCode == FX_EQN) {
                applyEqBandsToModule(voice.chain.eq, m);
            } else {
                applyEqBandsToModule(masterChain.masterEq, m);
                // Same one-way latch the per-row EQM arms: the master bus outlives the voice, so
                // stop() has to put the project's own preset back.
                tableMasterEqTouched.store(true, std::memory_order_relaxed);
            }
            continue;
        }

        const int value = songcore::automation_value_byte(r.startByte, r.destByte, r.curveByte, t);
        switch (r.fxCode) {
            case FX_VOLUME:
                voice.tableVolume = value / 255.0f;
                voice.modSourceValues[MOD_SRC_TABLE_VOL] = voice.tableVolume;
                break;
            case FX_CUT: voiceSetFilterCut(voice, value, sampleRate); break;
            case FX_RES: voiceSetFilterRes(voice, value, sampleRate); break;
            // A ramp over one of these moves the CUTOFF and re-asserts the same type every block, so
            // a sweep cannot lose the filter it opened with half way through.
            case FX_LPF: voiceSetFilterMode(voice, 1, value, sampleRate); break;
            case FX_HPF: voiceSetFilterMode(voice, 2, value, sampleRate); break;
            case FX_BPF: voiceSetFilterMode(voice, 3, value, sampleRate); break;
            case FX_DRV: voiceSetDrive(voice, value);     break;
            // A ramp over FIN is a glide: end to end is two semitones, spread over the AUS window.
            case FX_FIN: voiceSetFineTune(voice, value);  break;
            // …and a ramp over TIM is the tape swoop — see the per-row arm for what it writes and why
            // it is latched.
            case FX_TIM:
                delaySend.setTimeFree(value);
                tableDelayTimeTouched.store(true, std::memory_order_relaxed);
                break;
            // ⚠️ No FX_CRU arm, and its ARMS row says `rampable = false` — a packed pair of nibbles
            // is not a quantity to interpolate (songcore/effects.h). The default below drops it.
            default: break;   // the registry admits nothing else the table has an arm for
        }
    }
}

// ALL audio DSP lives here. processLiveBlock (live, via the platform backend's callback) and
// renderOffline (WAV export) are thin wrappers.
// Rule: NEVER add audio processing logic directly to processLiveBlock or renderOffline.
void AudioEngine::processAudioBlock(float* output, int numFrames, int channelCount, float sampleRate) {
    // The engine mixes STEREO ONLY: the SF render pass and the send/master chains index
    // [i*2] directly (only the sampler loop honours channelCount). Backends must open
    // stereo streams (the Oboe builder requests it; renderOffline is fixed at 2). Guard —
    // one silent block — rather than write past a mono buffer if a future backend drifts.
    if (channelCount != 2) return;
    // And the same guard for the block SIZE, for the same reason. Every per-block member below —
    // the send buses, the OCTA accumulators, sfBuf — is a fixed PROCESS_SUBBLOCK array indexed by
    // `numFrames`. Both shipped wrappers chunk at PROCESS_SUBBLOCK so nothing reaches this today;
    // the reader who will is the future ALSA/JACK backend audio-engine.h invites, and that reader
    // calls this function directly. ⚠️ A block larger than this would also resolve events too
    // coarsely — see the constant. Silence is the safe answer to both.
    if (numFrames > PROCESS_SUBBLOCK) return;

    // What waitForAudioBlockBoundary() watches: while this is set, a SoundFont handle this block
    // loaded may still be in use. seq_cst on both sides — see the wait. On the way out, whichever
    // return is taken, the block also publishes what the screen reads of the voices.
    struct InBlock {
        AudioEngine& e;
        explicit InBlock(AudioEngine& en) : e(en) { e.audioInBlock.store(true); }
        ~InBlock() {
            e.publishVoiceView();
            e.audioBlocksDone.fetch_add(1);
            e.audioInBlock.store(false);
        }
    } inBlock(*this);
    for (int t = 0; t < 8; t++) { framePeaksPerTrackL[t] = 0.0f; framePeaksPerTrackR[t] = 0.0f; }
    frameSendPeakRevL = frameSendPeakRevR = frameSendPeakDelL = frameSendPeakDelR = 0.0f;

    // Snapshot the mixer's targets ONCE per block, then read the snapshot in the hot loops below.
    // The targets are atomics the setters write from the UI thread; one block of a stale value is
    // inaudible, and the audio thread never waits on a fader move.

    // ⚠️ HOISTED ABOVE THE SNAPSHOT because the mute gate reads it: an export must not FADE a muted
    // track out over its first 5.8 ms, it must start silent. Also used by the visualizer gates below.
    const bool offlineRender = isOfflineRendering.load(std::memory_order_relaxed);

    // ⚠️ A FADER IS A PAIR, NOT A NUMBER, for the same reason the gate below is: its value at the
    // block's first frame and at its last, interpolated per sample by both mix paths. A fader that
    // jumps at a block edge steps the waveform, and a knob sending 0-127 steps it ~0.8% per message
    // — a tick per message, thirty times a second. ⚠️ SNAPPED under an export, like the gate: a
    // render must be the same samples every time, and nothing in one is a gesture.
    float trackVolStart[SF_VOICE_COUNT];
    float trackVolEnd[SF_VOICE_COUNT];
    // ⚠️ THE MUTE IS NO LONGER FOLDED INTO THE FADER — it is a RAMP now, and a ramp cannot be carried
    // by one per-block number. The gate arrives as its value at the block's first frame and its value
    // at the last, and both read sites interpolate between them per sample exactly as pan and the
    // mod-destination routes already do. Slamming a track to zero in one sample is what a mute sounded
    // like, and it was a step ~14x anything the signal does on its own.
    //
    // ⚠️ TWO READ SITES, AND BOTH ARE OBLIGATORY: the sampler's per-sample gain, and the SoundFont
    // buffer's post-chain multiply. They are the only two places a track's audio exists on its own
    // before it is summed — the SF path cannot take the gate through `tsf_channel_set_volume` because
    // that is set once per block, which is the very staircase this removes.
    float gateStart[SF_VOICE_COUNT];
    float gateEnd[SF_VOICE_COUNT];
    // The same pair for the three BUS gates — the two send returns and the dry sum. See setBusMutes().
    float revGateStart, revGateEnd, dlyGateStart, dlyGateEnd, dryGateStart, dryGateEnd;
    float masterVolStart, masterVolSnapshot;   // …and the master fader, same pair, same reason
    int previewTrack;
    {
        // Per full swing, so the ramp is the same wall-clock length whatever the block size.
        const float gateStep = (float)numFrames / (float)MUTE_GATE_SAMPLES;
        // One walk for every gate in the mixer, so a return cannot end up ramping differently from a
        // track. `gate` is the member that remembers where it got to; start/end bracket THIS block.
        const auto walk_gate = [&](float& gate, bool muted, float& start, float& end) {
            const float target = muted ? 0.0f : 1.0f;
            if (offlineRender) gate = target;   // a mute is a STATE in an export, not a gesture
            start = gate;
            if      (gate < target) gate = fminf(target, gate + gateStep);
            else if (gate > target) gate = fmaxf(target, gate - gateStep);
            end = gate;
        };
        // The fader's walk, and it reaches its target within the block rather than chasing it over
        // several: one block is already long enough to carry a full swing without a step (that is
        // what MUTE_GATE_SAMPLES is), so anything longer would only be lag.
        const auto walk_fader = [&](float& ramp, float target, float& start, float& end) {
            if (offlineRender) ramp = target;
            start = ramp;
            end = ramp = target;
        };
        for (int t = 0; t < 8; t++) {
            walk_fader(trackVolRamp[t], trackVolumes[t].load(std::memory_order_relaxed), trackVolStart[t], trackVolEnd[t]);
            walk_gate(trackGate[t], trackMuted[t].load(std::memory_order_relaxed), gateStart[t], gateEnd[t]);
        }
        walk_gate(revReturnGate,   revReturnMuted.load(std::memory_order_relaxed),   revGateStart, revGateEnd);
        walk_gate(delayReturnGate, delayReturnMuted.load(std::memory_order_relaxed), dlyGateStart, dlyGateEnd);
        walk_gate(dryGate,         dryMuted.load(std::memory_order_relaxed),         dryGateStart, dryGateEnd);
        walk_fader(masterVolRamp, masterVolume.load(std::memory_order_relaxed), masterVolStart, masterVolSnapshot);
        previewTrack      = previewLaneTrack.load(std::memory_order_relaxed);
    }
    // The preview lane borrows the fader of the channel the audition came from — the lane is a ninth
    // voice with no fader of its own, and an instrument you can only hear at full dry level tells you
    // nothing about how it sits in the mix. -1 (no origin) keeps the neutral gain it has always had.
    //
    // ⚠️ THE ASSIGNMENT SITS AFTER THE SNAPSHOT LOOP, not in the setter: reading the fader here
    // is what makes it the LIVE one, so a VTR or a mixer move lands in the audition it is aimed at.
    const bool previewBorrows   = (previewTrack >= 0 && previewTrack < 8);
    trackVolStart[PREVIEW_LANE] = previewBorrows ? trackVolStart[previewTrack] : 1.0f;
    trackVolEnd[PREVIEW_LANE]   = previewBorrows ? trackVolEnd[previewTrack]   : 1.0f;
    // …and the gate comes with it: an audition off a muted channel stayed silent when the mute was a
    // fold into the fader, and it has to keep doing that now the gate is carried separately.
    gateStart[PREVIEW_LANE]        = previewBorrows ? gateStart[previewTrack] : 1.0f;
    gateEnd[PREVIEW_LANE]          = previewBorrows ? gateEnd[previewTrack]   : 1.0f;

    // Zero only the [0,numFrames) slice actually used (not the full PROCESS_SUBBLOCK arrays), and
    // skip the expensive visualizer accumulators when nobody is watching (see CAPTURE_IDLE_MS).
    // Also skip all visualizer capture during offline WAV export: the live stream is silent so the
    // scopes already read flat, and OCTA would otherwise snapshot random mid-render frames that only
    // repaint on progress ticks (a frozen, twitching scope). Let the visualizers sit flat mid-render.
    const int64_t nowMsec       = nowMs();
    const bool octaWanted       = !offlineRender && (nowMsec - lastTrackWaveformReadMs.load(std::memory_order_relaxed)) < CAPTURE_IDLE_MS;
    const bool spectrumWanted   = !offlineRender && (nowMsec - lastSpectrumReadMs.load(std::memory_order_relaxed))      < CAPTURE_IDLE_MS;
    const size_t frameBytes     = (size_t)numFrames * sizeof(float);

    // Per-block scratch (send buses, OCTA accumulators, instrument-spectrum sum) lives on the engine
    // object, not the audio-thread stack — declared in the header. (Re)initialised here every block;
    // PROCESS_SUBBLOCK is the class cap and processLiveBlock/renderOffline chunk larger requests, so
    // only [0,numFrames) is ever touched.
    memset(revSendBufL, 0, frameBytes); memset(revSendBufR, 0, frameBytes);
    memset(dlySendBufL, 0, frameBytes); memset(dlySendBufR, 0, frameBytes);

    // The 64 KB+ OCTA accumulator pair is read only by OCTA — zero/fill it only when OCTA is shown.
    // trackWasActive is reset every block (matches the former `= {}` init; read under octaWanted below).
    memset(trackWasActive, 0, sizeof(trackWasActive));
    if (octaWanted) {
        for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) {
            memset(trackWaveAccumL[t], 0, frameBytes);
            memset(trackWaveAccumR[t], 0, frameBytes);
        }
    }

    // Per-instrument spectrum accumulator (mono sum of one instrument's voices) — only when the
    // EQ screen is monitoring an instrument.
    int   monitoredInstrId = instrSpectrumInstrId.load(std::memory_order_relaxed);
    if (monitoredInstrId >= 0) memset(instrSpectrumTempL, 0, frameBytes);

    // Drain each queue ONCE for the whole block (one lock each) into reusable batch buffers,
    // then dispatch from them inside the frame loop with zero locking. The heap pops earliest-first
    // so each batch is already sorted ascending by targetFrame.
    noteBatch.clear(); killBatch.clear(); paramBatch.clear();
    // Snapshot the frame counter once (it's atomic; this thread is the only writer, so a single
    // relaxed load is enough and avoids re-loading it per frame below).
    const int64_t blockStartFrame = globalFrameCounter.load(std::memory_order_relaxed);
    const int64_t blockEnd = blockStartFrame + numFrames - 1;

    // A SoundFont freed since a voice was armed: the voice lets go before anything this block can
    // reach the slot, which may by now hold a different font.
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        SoundfontVoice& sv = sfVoices[t];
        if (sv.sfSlot >= 0 && sv.sfGen != soundfonts[sv.sfSlot].gen.load()) sv.detach();
    }

    // ⚠️ THE TRANSPORT-STOP RAMP HAS A DEADLINE, AND THE POOL IS ONLY EIGHT SLOTS.
    //
    // stopAllRamped() arms a fade and leaves the audio thread to finish it, which is right for the
    // audio and not enough for the slots: a voice whose playhead has already run off the end of its
    // sample mixes ONE frame per block (the bounds check pins the position and breaks), so its fade
    // counter falls by 1 a block and the slot is held for ~256 blocks. The instant stop used to
    // collect those voices as a side effect; nothing else ever did. Three of eight slots held after
    // every stop is what pushes the NEXT take's allocator into step 3/4, where it preempts a fading
    // voice and clicks — the very thing the ramp is here to remove.
    //
    // By this frame the ramp is over and every voice it armed is at zero, so ending them is silent.
    // Only voices that are FADING are touched: a note triggered by a restart is not, and one that
    // began fading inside the ramp window was within KILL_FADE_SAMPLES of silence anyway.
    if (stopRampRequested.exchange(false, std::memory_order_acquire)) startStopRamp(blockStartFrame);
    {
        const int64_t rampEnd = stopRampEndFrame.load(std::memory_order_relaxed);
        if (rampEnd >= 0 && blockStartFrame >= rampEnd) {
            stopRampEndFrame.store(-1, std::memory_order_relaxed);
            for (int i = 0; i < MAX_VOICES; i++)
                if (voices[i].isFadingOut) voices[i].stop();
        }
    }
    // A live key's bytes are turned into records HERE, stamped at this block's first frame, so the
    // drain just below picks them up and the key sounds in the block its bytes arrived in. Never
    // during an export: a render is the same samples every time, and a key is not part of the song.
    if (!offlineRender) {
        if (LiveInputSource* live = liveInput.load(std::memory_order_acquire)) live->drainLiveInput(blockStartFrame);
    }
    paramUpdateQueue.drainUntil(blockEnd, paramBatch);
    killQueue.drainUntil(blockEnd, killBatch);
    noteQueue.drainUntil(blockEnd, noteBatch);
    // What the control thread set is taken in AFTER the drain: an edit it made before queuing an event
    // (a knob, then the reload of the instrument) is then always in force when that event runs. And
    // before the dispatch, so a table's EQM or TIM this block lands on top of the song's setting.
    // ⚠️ The instrument data FIRST: the bus EQs are applied from the EQ presets it carries.
    syncInstrumentData();
    applyBusSettings();
    size_t paramIdx = 0, killIdx = 0, noteIdx = 0;

    // ⚠️ **THE THREE BATCHES ARE ONE TIMELINE, NOT THREE.** Each loop below takes everything due
    // (`targetFrame <= currentFrame`), which orders them correctly only while the block is keeping up:
    // one frame index per frame, so a param stamped at F+1 cannot be reached before the note at F.
    // The moment anything is LATE that stops being true — a note at F and its params at F+1 both fall
    // due at the SAME frame index, and the only thing left deciding which runs first is which loop is
    // written first, which is the params. That is a silent DROP, not a reorder: every per-voice param
    // (REV/DEL/BCK/CUT/RES/EQN ride one frame behind their note by design, `voiceFxFrame` in
    // scheduler.h) is applied to a track whose voice the note has not started yet, and finds nothing.
    //
    // Late is the first step of every take, not an exotic state: `Sequencer::playPhrase` stamps it at
    // `getCurrentFrame()`, which is the counter as of the last COMPLETED sub-block, so a T PLAY landing
    // while the audio thread is inside processAudioBlock schedules onto a frame it has already passed.
    //
    // So each queue yields to the earlier of the ones after it. `<=` and not `<`: at EQUAL frames the
    // written order stands (params, then kills, then notes), which is what `voiceFxFrame`'s +1 and a
    // K00 sharing its step's frame both rest on. What a loop holds back is applied on the next frame
    // index — 23 µs, and still ahead of everything authored after it. tools/ptlate.
    const auto dueKill = [&] { return killIdx < killBatch.size() ? killBatch[killIdx].targetFrame : INT64_MAX; };
    const auto dueNote = [&] { return noteIdx < noteBatch.size() ? noteBatch[noteIdx].targetFrame : INT64_MAX; };

    for (int32_t frame = 0; frame < numFrames; frame++) {
        int64_t currentFrame = blockStartFrame + frame;

        // Apply scheduled parameter updates at their exact frame. Running here (on the audio
        // thread) is what makes live PBN/PVB/PVX/THO race-free: the look-ahead scheduler only
        // enqueues; the voices[] mutation happens below, where the mix loop is the sole writer.
        while (paramIdx < paramBatch.size() && paramBatch[paramIdx].targetFrame <= currentFrame &&
               paramBatch[paramIdx].targetFrame <= std::min(dueKill(), dueNote())) {
            ScheduledParamUpdate upd = paramBatch[paramIdx++];
            switch (upd.action) {
                case PARAM_UPDATE_PITCH_BEND: {           // PBN on empty step
                    IAudioVoice* pv = findActiveVoiceForTrack(upd.trackId);
                    if (pv) pv->setPitchBendRaw(upd.value);
                    break;
                }
                case PARAM_UPDATE_VIBRATO: {              // PVB/PVX on empty step
                    IAudioVoice* pv = findActiveVoiceForTrack(upd.trackId);
                    if (pv) pv->setVibratoRaw(upd.value, upd.value2);
                    break;
                }
                case PARAM_UPDATE_TABLE_ROW: {            // THO on empty step (sampler voices only)
                    for (int v = 0; v < MAX_VOICES; v++) {
                        if (voices[v].isActive && voices[v].trackId == upd.trackId) {
                            // ⚠️ ALL THREE COLUMNS. This THO is written in a PHRASE, not in the
                            // table, so it belongs to no column — "put this track's table on row X"
                            // is the only thing it can mean. A THO inside the table steers the
                            // column it is typed in; that one is in processTableRow.
                            for (int l = 0; l < TABLE_LANES; ++l) {
                                voices[v].lanes[l].row = (int)upd.value % 16;
                                voices[v].lanes[l].lastProcessed = -1;  // re-apply the row immediately
                            }
                            break;
                        }
                    }
                    break;
                }
                case PARAM_UPDATE_PAN: {                  // PAN — per-note pan override
                    IAudioVoice* pv = findActiveVoiceForTrack(upd.trackId);
                    if (pv) pv->setPan(upd.value);
                    break;
                }
                case PARAM_UPDATE_REVERB_SEND: {          // REV — per-note reverb send
                    for (int v = 0; v < MAX_VOICES; v++)
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId)
                            voices[v].reverbSend = upd.value;
                    if (upd.trackId >= 0 && upd.trackId < SF_VOICE_COUNT && sfVoices[upd.trackId].isActive)
                        sfVoices[upd.trackId].instrParams.reverbSend = upd.value;
                    break;
                }
                case PARAM_UPDATE_DELAY_SEND: {           // DEL — per-note delay send
                    for (int v = 0; v < MAX_VOICES; v++)
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId)
                            voices[v].delaySend = upd.value;
                    if (upd.trackId >= 0 && upd.trackId < SF_VOICE_COUNT && sfVoices[upd.trackId].isActive)
                        sfVoices[upd.trackId].instrParams.delaySend = upd.value;
                    break;
                }
                case PARAM_UPDATE_REVERSE: {              // BCK — playback direction (sampler only)
                    bool rev     = (upd.value  != 0.0f);
                    bool restart = (upd.value2 != 0.0f);
                    for (int v = 0; v < MAX_VOICES; v++) {
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId) {
                            Voice& vo = voices[v];
                            vo.reverse = rev;
                            if (restart) {
                                // With-note BCK: (re)start at the boundary the new direction reads FROM, so a
                                // "play backwards" note begins at the sample's end instead of instantly hitting
                                // actualStart and fading out. Mid-note BCK (restart=false) keeps the live position
                                // so direction flips are continuous (scratching).
                                vo.position = rev ? (double)(vo.actualEnd > vo.actualStart ? vo.actualEnd - 1 : vo.actualStart)
                                                  : (double)vo.actualStart;
                            }
                            break;
                        }
                    }
                    break;
                }
                // CUT / RES — the sounding voice's filter. Inert on an instrument whose FILTER TYPE
                // is OFF, and gone with the note: the next note-on reloads the instrument's own.
                case PARAM_UPDATE_FILTER_CUT: {           // CUT — filter cutoff
                    int cut = filterByteOf(upd.value);
                    for (int v = 0; v < MAX_VOICES; v++)
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId) {
                            voiceSetFilterCut(voices[v], cut, sampleRate); break;
                        }
                    if (upd.trackId >= 0 && upd.trackId < SF_VOICE_COUNT && sfVoices[upd.trackId].isActive)
                        voiceSetFilterCut(sfVoices[upd.trackId], cut, sampleRate);
                    break;
                }
                case PARAM_UPDATE_FILTER_RES: {           // RES — filter resonance
                    int res = filterByteOf(upd.value);
                    for (int v = 0; v < MAX_VOICES; v++)
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId) {
                            voiceSetFilterRes(voices[v], res, sampleRate); break;
                        }
                    if (upd.trackId >= 0 && upd.trackId < SF_VOICE_COUNT && sfVoices[upd.trackId].isActive)
                        voiceSetFilterRes(sfVoices[upd.trackId], res, sampleRate);
                    break;
                }
                // LPF / HPF / BPF — the type and the cutoff out of ONE record, so the filter opens at
                // the cutoff it was told rather than a block before it. Unlike the two above this is
                // NOT inert on a voice whose instrument declares no filter: it declares one.
                case PARAM_UPDATE_FILTER_MODE: {
                    int cut  = filterByteOf(upd.value);
                    int type = (int)upd.value2;
                    for (int v = 0; v < MAX_VOICES; v++)
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId) {
                            voiceSetFilterMode(voices[v], type, cut, sampleRate); break;
                        }
                    if (upd.trackId >= 0 && upd.trackId < SF_VOICE_COUNT && sfVoices[upd.trackId].isActive)
                        voiceSetFilterMode(sfVoices[upd.trackId], type, cut, sampleRate);
                    break;
                }
                // DRV / CRU. One write each onto the input the per-block recompute already
                // reads, so the change is audible in the block it lands in.
                case PARAM_UPDATE_DRIVE:
                case PARAM_UPDATE_CRUSH: {
                    int byteValue = filterByteOf(upd.value);
                    for (int v = 0; v < MAX_VOICES; v++)
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId) {
                            if (upd.action == PARAM_UPDATE_DRIVE) voiceSetDrive(voices[v], byteValue);
                            else                                  voiceSetCrush(voices[v], byteValue);
                            break;
                        }
                    if (upd.trackId >= 0 && upd.trackId < SF_VOICE_COUNT && sfVoices[upd.trackId].isActive) {
                        if (upd.action == PARAM_UPDATE_DRIVE) voiceSetDrive(sfVoices[upd.trackId], byteValue);
                        else                                  voiceSetCrush(sfVoices[upd.trackId], byteValue);
                    }
                    break;
                }
                // FIN. Its own arm rather than a fourth branch of the chain above: one setter serves
                // both voice types here, which is exactly what the three above cannot do.
                case PARAM_UPDATE_FINE_TUNE: {
                    int byteValue = filterByteOf(upd.value);
                    for (int v = 0; v < MAX_VOICES; v++)
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId) {
                            voiceSetFineTune(voices[v], byteValue); break;
                        }
                    if (upd.trackId >= 0 && upd.trackId < SF_VOICE_COUNT && sfVoices[upd.trackId].isActive)
                        voiceSetFineTune(sfVoices[upd.trackId], byteValue);
                    break;
                }
                // LPO. ⚠️ IT ADDS — every other arm in this switch assigns. The running total is in
                // SIXTEENTHS of the loop's own length and the mix loop turns it into samples, which
                // is what keeps sixteen small steps equal to one big one (sampler-voice.h).
                //
                // ⚠️ SAMPLER ONLY, and silently so: a SoundFont voice has no sample position to slide,
                // the same silence OFF keeps.
                case PARAM_UPDATE_LOOP_SLIDE: {
                    int steps = loopSlideSixteenthsOf(filterByteOf(upd.value));
                    for (int v = 0; v < MAX_VOICES; v++)
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId) {
                            voices[v].loopSlideSixteenths += steps; break;
                        }
                    break;
                }
                case PARAM_UPDATE_EQ_SLOT: {              // EQN — per-note EQ preset
                    int slot = (int)upd.value;
                    for (int v = 0; v < MAX_VOICES; v++)
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId) {
                            applyEqPresetToModule(voices[v].chain.eq, slot); break;
                        }
                    if (upd.trackId >= 0 && upd.trackId < SF_VOICE_COUNT && sfVoices[upd.trackId].isActive)
                        applyEqPresetToModule(sfVoices[upd.trackId].chain.eq, slot);
                    break;
                }
                case PARAM_UPDATE_MASTER_EQ: {            // EQM — master/mixer EQ preset (global)
                    applyEqPresetToModule(masterChain.masterEq, (int)upd.value);   // audio thread: not the setter
                    break;
                }
                // The two morph arms. Same targets as the two above, reached the same way — only the
                // source of the band values differs, so an EQN morph is subject to exactly the
                // per-voice limits an EQN is: it writes the SOUNDING voice and a note-on resets that
                // voice's EQ from the instrument, so the next tick (≤ 1/12 of a step) re-asserts it.
                case PARAM_UPDATE_EQ_BANDS: {             // EQN under AUS/AUF
                    for (int v = 0; v < MAX_VOICES; v++)
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId) {
                            applyEqBandsToModule(voices[v].chain.eq, upd.eqBands); break;
                        }
                    if (upd.trackId >= 0 && upd.trackId < SF_VOICE_COUNT && sfVoices[upd.trackId].isActive)
                        applyEqBandsToModule(sfVoices[upd.trackId].chain.eq, upd.eqBands);
                    break;
                }
                case PARAM_UPDATE_MASTER_EQ_BANDS: {      // EQM under AUS/AUF (global)
                    applyEqBandsToModule(masterChain.masterEq, upd.eqBands);
                    break;
                }
                // ⚠️ BOTH FADER ARMS WRITE THE SNAPSHOT AS WELL AS THE MEMBER. The snapshot above is
                // what the mix loops below actually read; the member is what survives to the next
                // block. Write one and the fader moves a block late, write the other and it moves for
                // one block and springs back.
                //
                // ⚠️ AND BOTH ENDS OF THE PAIR, WHICH KEEPS A SONG'S OWN RAMP EXACTLY AS IT WAS. A
                // `VTR` already moves in per-block steps of its own choosing and a render must
                // reproduce them to the sample; the smoothing above is for a fader a HAND moved
                // between blocks, which is the one that arrives as an unannounced step.
                case PARAM_UPDATE_TRACK_VOL: {            // VTR — this track's mixer fader
                    if (upd.trackId >= 0 && upd.trackId < 8) {
                        // Both take the authored value: the mute is no longer folded in here, it is a
                        // separate gate multiplied at the two read sites. The fader therefore keeps
                        // moving under a muted track exactly as before, so unmuting lands on wherever
                        // the ramp has got to rather than on where it started.
                        trackVolumes[upd.trackId].store(upd.value, std::memory_order_relaxed);
                        trackVolStart[upd.trackId] = trackVolEnd[upd.trackId] = upd.value;
                        trackVolRamp[upd.trackId]  = upd.value;
                    }
                    break;
                }
                case PARAM_UPDATE_MASTER_VOL: {           // VMV — the master fader (global)
                    masterVolume.store(upd.value, std::memory_order_relaxed);
                    masterVolStart = masterVolSnapshot = masterVolRamp = upd.value;
                    break;
                }
                case PARAM_UPDATE_DELAY_TIME: {           // TIM — the delay's echo time (global)
                    delaySend.setTimeFree(filterByteOf(upd.value));
                    break;
                }
                // An instrument was edited — a knob, a screen, a preset. Every voice still sounding
                // on it re-reads the parameters a trigger would have copied. ⚠️ BY INSTRUMENT, not by
                // track: the same instrument can be sounding on several at once.
                case PARAM_UPDATE_INSTRUMENT: {
                    const int id = upd.instrId;
                    if (id < 0 || id >= 256) break;
                    const InstrumentParams& ip = instrumentParams[id];
                    for (int v = 0; v < MAX_VOICES; v++)
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].instrId == id)
                            voiceReloadInstrument(voices[v], ip, sampleRate);
                    for (int t = 0; t < SF_VOICE_COUNT; t++)
                        if (sfVoices[t].isActive && sfVoices[t].instrId == id)
                            voiceReloadInstrument(sfVoices[t], ip, sampleRate);
                    break;
                }
                default: {                                // PARAM_UPDATE_MOD_SOURCE — Vxx phraseVol
                    for (int v = 0; v < MAX_VOICES; v++) {
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId) {
                            voices[v].modSourceValues[(ModSourceId)upd.sourceId] = upd.value;
                            break;
                        }
                    }
                    if (upd.trackId >= 0 && upd.trackId < SF_VOICE_COUNT && sfVoices[upd.trackId].isActive) {
                        sfVoices[upd.trackId].modSourceValues[(ModSourceId)upd.sourceId] = upd.value;
                    }
                    break;
                }
            }
        }

        // Process all scheduled kill events for this exact frame (BEFORE notes, and after any note
        // due EARLIER — see the timeline note above the frame loop)
        while (killIdx < killBatch.size() && killBatch[killIdx].targetFrame <= currentFrame &&
               killBatch[killIdx].targetFrame <= dueNote()) {
            ScheduledKill kill = killBatch[killIdx++];
            if (kill.mode == KILL_KEY_OFF) {
                // A live key let go of (MIDI plan §4.1). The three-way rule is inside
                // SamplerVoice::keyRelease — and the ONLY difference from a KIL is the one-shot arm,
                // which does nothing at all here and a declicked fade there.
                triggerKeyRelease(kill.trackId, frame);
                // SF: unchanged. TSF owns its own release envelope, a SoundFont preset always HAS one,
                // and "a one-shot with no envelope" is a sampler-only shape — there is nothing for the
                // §4.1 rule to decide on this side.
                if (kill.trackId >= 0 && kill.trackId < SF_VOICE_COUNT) {
                    sfVoices[kill.trackId].noteOffAt(frame);
                }
                LOGT("🎹 Key release: track %d at frame %lld", kill.trackId, (long long)currentFrame);
            } else if (kill.mode == KILL_SOFT) {
                triggerNoteOff(kill.trackId, frame);  // Sampler: trigger ADSR release
                // SF: noteOff (TSF handles its own release envelope internally)
                if (kill.trackId >= 0 && kill.trackId < SF_VOICE_COUNT) {
                    sfVoices[kill.trackId].noteOffAt(frame);
                }
                LOGT("🎵 Note-off: track %d at frame %lld", kill.trackId, (long long)currentFrame);
            } else if (kill.mode == KILL_CUT) {
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == kill.trackId && voices[v].isActive) {
                        voices[v].startFadeOut(KILL_FADE_SAMPLES, frame);
                    }
                }
                // SF: the transport-stop ramp, not a note-off — it ends in hardStop, so no TSF release
                // and no ADSR release outlive it.
                if (kill.trackId >= 0 && kill.trackId < SF_VOICE_COUNT) {
                    sfVoices[kill.trackId].startStopFade(KILL_FADE_SAMPLES, frame);
                }
            } else {
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == kill.trackId && voices[v].isActive) {
                        voices[v].startFadeOut(KILL_FADE_SAMPLES, frame);  // soft deliberate cut, not a steal
                        LOGT("🔪 Killed track %d at frame %lld", kill.trackId, (long long)currentFrame);
                    }
                }
                // SF: soft kill so TSF's internal release envelope can play out.
                if (kill.trackId >= 0 && kill.trackId < SF_VOICE_COUNT) {
                    sfVoices[kill.trackId].noteOffAt(frame);
                }
            }
        }

        // Trigger all notes scheduled for this exact frame
        while (noteIdx < noteBatch.size() && noteBatch[noteIdx].targetFrame <= currentFrame) {
            ScheduledNote note = noteBatch[noteIdx++];

            // ⚠️ **THE INSTRUMENT IS CHOSEN HERE, NOT WHERE THE NOTE WAS QUEUED.** A sequencer note
            // arrives as a number and becomes a sound on this line; everything below then runs on a
            // fully derived note and cannot tell the two paths apart.
            if (note.instrumentId >= 0 && !resolveScheduledNote(note)) continue;   // nothing to play

            // ---- SOUNDFONT PATH ----
            // Tracks use the master tsf* handle via MIDI channels (channel = trackId).
            // No per-track clone creation — tsf_load_memory() never runs on the audio thread.
            if (note.isSoundfont) {
                int t = note.trackId;
                // ⚠️ The handle is NOT tested here. It is a non-atomic pointer the JNI/UI thread can
                // null at any moment, and reading it without the slot mutex is a race whose answer
                // may already be stale by the next line. `armNote` reads it under the lock and says
                // whether the note is worth setting up; that answer is the one worth having, and the
                // whole block below belongs to a note that is going to play.
                if (t >= 0 && t < SF_VOICE_COUNT &&
                    note.sfSlot >= 0 && note.sfSlot < MAX_SOUNDFONTS) {

                    SoundfontVoice& sv = sfVoices[t];
                    // ⚠️ READ BEFORE `armNote`, which sets isActive unconditionally. This is the
                    // question the chain setup below has to ask: is this channel's filter/EQ full of
                    // a note that is still sounding? See InstrumentChain::reset's keepToneState.
                    const bool wasSounding = sv.isActive;
                    // This instrument's ADSR override (applied atomically inside fireArmedNote, before
                    // note_on) — keyed by instrument id so de-duplicated handles stay isolated.
                    int eAtk = -1, eDec = -1, eSus = -1, eRel = -1;
                    if (note.sampleId >= 0 && note.sampleId < 256) {
                        const SfEnvOverride& eo = sfEnvOverrides[note.sampleId];
                        eAtk = eo.atk; eDec = eo.dec; eSus = eo.sus; eRel = eo.rel;
                    }
                    if (!sv.armNote(note.sfSlot, note.midiNote, note.midiVelocity,
                                    note.volume, note.pan, note.sfBank, note.sfPreset, t,
                                    eAtk, eDec, eSus, eRel)) {
                        LOGT("🎹 SF DROPPED: sfSlot=%d track=%d (handle not loaded)",
                             note.sfSlot, note.trackId);
                        continue;   // …and the voice keeps whatever it was already playing
                    }
                    soundfonts[note.sfSlot].lastUsed.store(nextSfUseTick(), std::memory_order_relaxed);  // LRU touch
                    // Per-track mono across voice types, this direction: an SF note replaces a sampler
                    // note still sounding on this track with the fade a sampler note would give it.
                    // The sampler path does the reverse below. Only after armNote said yes — a dropped
                    // SF note leaves the track as it was.
                    for (int v = 0; v < MAX_VOICES; v++) {
                        if (voices[v].trackId == t && voices[v].isActive && !voices[v].isFadingOut) {
                            voices[v].startFadeOut();
                        }
                    }
                    sv.isReleasingOnly = false;
                    sv.resetPitchState();
                    sv.detuneSemitones = note.detuneSemitones;  // static instrument detune (set after reset)
                    sv.startDelayFrames = frame;  // start rendering at the note's exact intra-block frame
                    sv.instrId = note.sampleId;

                    // M8-style: a TIC in the table's last row overrides the instrument tic rate —
                    // one rate per FX column.
                    int effectiveTicRates[TABLE_LANES];
                    effectiveTicRatesFor(note.tableId, note.tableTicRate, effectiveTicRates);
                    const int sfStartRows[TABLE_LANES] = {note.tableStartRow, note.tableStartRow,
                                                          note.tableStartRow};
                    sv.resetTableState(note.tableId, effectiveTicRates,
                                       note.noteOctave, note.notePitch, sfStartRows);

                    // Only valid when sampleId >= 0 (phrase playback); previews pass -1.
                    if (note.sampleId >= 0 && note.sampleId < 256) {
                        sv.instrParams = instrumentParams[note.sampleId];
                        initVoiceModSlots(sv, note.sampleId, currentFrame, sampleRate);
                    } else {
                        sv.instrParams = InstrumentParams{};
                        for (int m = 0; m < 4; m++) sv.voiceMods[m] = VoiceModSlot{};
                    }
                    sv.chain.reset(sampleRate, /*keepToneState=*/wasSounding);
                    sv.chain.filter.setParams(sv.instrParams.filterType, sv.instrParams.filterCut,
                                              sv.instrParams.filterRes, sv.instrParams.filterDrive,
                                              (int)sampleRate);
                    sv.chain.filter.snapshotCoeffs(); // seed prev = target so first block doesn't interpolate from reset defaults
                    sv.chain.drive.setDrive(sv.instrParams.drive);
                    sv.chain.crush.setParams(sv.instrParams.crush, sv.instrParams.downsample);
                    if (sv.instrParams.eqActive) {
                        sv.chain.eq.active = true;
                        for (int i = 0; i < 3; i++) {
                            sv.chain.eq.bands[i].setParams(sv.instrParams.eqBands[i].type,
                                                           sv.instrParams.eqBands[i].freqHz,
                                                           sv.instrParams.eqBands[i].gainDb,
                                                           sv.instrParams.eqBands[i].q);
                        }
                    }

                    sv.params.setBase(PARAM_VOL,   note.volume);
                    sv.params.setBase(PARAM_PAN,   note.pan);
                    sv.params.setBase(PARAM_PITCH, 0.0f);
                    // The filter pair seeded from the instrument, as SamplerVoice::triggerNote does:
                    // CUT/RES carry each other's current value through the bus, so an SF voice whose
                    // bus still held the ParamBus default would jump to it on the first CUT.
                    sv.params.setBase(PARAM_FILTER_CUT, (float)sv.instrParams.filterCut);
                    sv.params.setBase(PARAM_FILTER_RES, (float)sv.instrParams.filterRes);
                    sv.params.resetMods();
                    memset(sv.modSourceValues,  0, sizeof(sv.modSourceValues));
                    memset(sv.modDestValues,    0, sizeof(sv.modDestValues));
                    memset(sv.prevModDestValues,0, sizeof(sv.prevModDestValues));
                    sv.modSourceValues[MOD_SRC_TABLE_VOL]  = 1.0f;
                    sv.modSourceValues[MOD_SRC_PHRASE_VOL] = note.phraseVolume;
                    float initVol = note.volume * note.phraseVolume;
                    sv.modDestValues[PARAM_VOL]     = initVol;
                    sv.prevModDestValues[PARAM_VOL] = initVol;
                    if (note.pslInitialOffset != 0.0f && note.pslDuration > 0.0f) {
                        sv.pitchOffset      = note.pslInitialOffset;
                        sv.pitchSlideTarget = 0.0f;
                        sv.pitchSlideRate   = -note.pslInitialOffset / note.pslDuration;
                        sv.pitchSliding     = true;
                    }
                    if (fabsf(note.pbnRate) > 0.0001f) {
                        sv.pitchSlideRate   = note.pbnRate;
                        sv.pitchSlideTarget = (note.pbnRate > 0) ? 127.0f : -127.0f;
                        sv.pitchSliding     = true;
                    }
                    if (note.vibratoDepth > 0.01f) {
                        sv.vibratoSpeed  = note.vibratoSpeed;
                        sv.vibratoDepth  = note.vibratoDepth;
                        sv.vibratoActive = true;
                    }
                    LOGT("🎹 SF FIRE: slot=%d track/ch=%d bank=%d preset=%d midi=%d vel=%d vol=%.2f",
                         note.sfSlot, t, note.sfBank, note.sfPreset,
                         note.midiNote, note.midiVelocity, note.volume);
                } else {
                    LOGT("🎹 SF DROPPED: sfSlot=%d track=%d (out of range)", note.sfSlot, note.trackId);
                }
                continue;  // Skip voice pool processing
            }
            // ---- END SOUNDFONT PATH ----

            // TIC00 support: continue the table where this track's previous note left off — per
            // COLUMN, since a table can be at TIC00 in FX2 and free-running in FX1. −1 = this column
            // has nothing to carry and starts wherever the trigger says.
            int savedTableRows[TABLE_LANES] = {-1, -1, -1};
            bool wasTIC00Mode = false;
            // ⚠️ …and it must be THIS table. A chain hands the note on, so the voice still sounding on
            // this track can be running a different table entirely, and its row means nothing here.
            for (int v = 0; v < MAX_VOICES; v++) {
                if (voices[v].trackId == note.trackId && voices[v].isActive && !voices[v].isFadingOut
                    && voices[v].tableId >= 0 && voices[v].tableId == note.tableId) {
                    for (int l = 0; l < TABLE_LANES; ++l) {
                        if (voices[v].lanes[l].ticRate != 0x00) continue;
                        wasTIC00Mode = true;
                        savedTableRows[l] = tic00RowAfter(voices[v].lanes[l]);
                        LOGT("📋 TIC00: table row %d for track %d column %d retrigger (from voice %d)",
                             savedTableRows[l], note.trackId, l + 1, v);
                    }
                }
            }
            // No voice left to read the row off — the previous note's sample ran out before this one
            // arrived. The track's cursor still holds it. Without this the table restarted at row 0
            // every time, so how far it got depended on the instrument's ROOT note (root → playback
            // rate → how long a one-shot lasts): a low root never left the first row or two.
            if (!wasTIC00Mode && note.trackId >= 0 && note.trackId < SF_VOICE_COUNT &&
                note.tableId >= 0) {
                if (const Tic00Cursor* c = tic00Slot(note.trackId, note.tableId, /*create=*/false)) {
                    for (int l = 0; l < TABLE_LANES; ++l) {
                        if (c->ticRate[l] != 0x00 || !c->active[l]) continue;
                        wasTIC00Mode = true;
                        savedTableRows[l] = tic00RowAfter(c->row[l], c->lastProcessed[l]);
                        LOGT("📋 TIC00: table row %d for track %d column %d retrigger (from table cursor)",
                             savedTableRows[l], note.trackId, l + 1);
                    }
                }
            }

            // ---------------------------------------------------------------
            // VOICE ALLOCATION — mono per track + 4-step slot choice
            //
            // Problem: "steal old + allocate new" temporarily consumes two
            // slots per track.  When N tracks all trigger at the same frame
            // (phrase boundaries) this exhausts the 8-slot pool even with
            // only 5 active tracks.
            //
            // Step 1 — fade any playing same-track voice (mono per track).
            // Step 2 — prefer a FREE slot, so the faded voice's declick tail
            //           actually plays out. (Recycling the fading same-track
            //           slot here instead cut its tail mid-fade — an audible
            //           pop on rapid same-sample retriggers/previews.)
            // Step 3 — no free slot: recycle a same-track fading voice
            //           directly (0 extra slots used; trackId is preserved
            //           through startFadeOut() precisely for this).
            // Step 4 — last resort: preempt any fading voice (other track).
            //           Produces at most a ~1ms click but prevents silence.
            // ---------------------------------------------------------------

            // A note with no sample behind it must not touch the track: fading the playing voice for
            // a note that cannot sound would silence the track for nothing (a preview of an empty slot).
            const bool haveSample = note.sampleId >= 0 && note.sampleId < 256 && samples[note.sampleId];

            // Step 1: mono per track — fade whatever is still playing on this track
            for (int v = 0; haveSample && v < MAX_VOICES; v++) {
                if (voices[v].trackId == note.trackId && voices[v].isActive && !voices[v].isFadingOut) {
                    voices[v].startFadeOut(DECLICK_SAMPLES, frame);   // …from the new note's own frame
                }
            }

            // Step 2: free slot
            int targetSlot = -1;
            for (int v = 0; v < MAX_VOICES; v++) {
                if (!voices[v].isActive) {
                    targetSlot = v;
                    break;
                }
            }

            // Step 3: pool full — recycle a same-track fading voice (cuts its tail)
            if (targetSlot == -1) {
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == note.trackId && voices[v].isFadingOut) {
                        targetSlot = v;
                        break;
                    }
                }
            }

            // Step 4: preempt any fading voice (last resort)
            if (targetSlot == -1) {
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].isFadingOut) {
                        targetSlot = v;
                        LOGT("⚠️ Voice pool tight: preempting fading slot %d for track %d", v, note.trackId);
                        break;
                    }
                }
            }

            if (targetSlot != -1) {
                int v = targetSlot;
                if (haveSample) {
                    // Per-track mono across voice types: a sampler note replaces an SF note
                    // still sounding on this track. noteOff (not hardStop) so the SF release
                    // plays out musically — findActiveVoiceForTrack skips releasing SF voices,
                    // so mid-note params already target the new sampler voice meanwhile.
                    if (note.trackId >= 0 && note.trackId < SF_VOICE_COUNT &&
                        sfVoices[note.trackId].isActive && !sfVoices[note.trackId].isReleasingOnly) {
                        sfVoices[note.trackId].noteOffAt(frame);
                    }
                    float rate = note.frequency / note.baseFrequency;

                    // M8-style: a TIC in the table's last row overrides the instrument tic rate —
                    // one rate per FX column.
                    int effectiveTicRates[TABLE_LANES];
                    effectiveTicRatesFor(note.tableId, note.tableTicRate, effectiveTicRates);

                    // A THO-with-note start row places every column; otherwise only a column that is
                    // BOTH at TIC00 and had something to carry resumes, and the rest begin at row 0.
                    int startRows[TABLE_LANES] = {0, 0, 0};
                    for (int l = 0; l < TABLE_LANES; ++l) {
                        if (note.tableStartRow >= 0) startRows[l] = note.tableStartRow % 16;
                        else if (wasTIC00Mode && effectiveTicRates[l] == 0x00 && savedTableRows[l] >= 0)
                            startRows[l] = savedTableRows[l];
                    }

                    // The generation BEFORE the buffers: the UI bumps it after storing them, so a
                    // mismatch here can only make the mix end the voice, never read a stale pointer.
                    const uint32_t gen = sampleGen[note.sampleId].load();
                    voices[v].trigger(samples[note.sampleId], samplesRight[note.sampleId], sampleLengths[note.sampleId],
                                      note.trackId, rate, note.baseFrequency,
                                      note.volume, note.phraseVolume, note.pan, instrumentParams[note.sampleId],
                                      sampleRate, note.startPointOverride, note.endPointOverride,
                                      note.tableId, effectiveTicRates, note.noteOctave, note.notePitch, startRows);
                    voices[v].instrId = note.sampleId;
                    voices[v].sampleGen = gen;
                    voices[v].startDelayFrames = frame;  // start mixing at the note's exact intra-block frame

                    // pslDuration is already in audio frames — songcore/voice_derive.h multiplies the
                    // authored tick count by framesPerTic before the note reaches the queue.
                    if (fabsf(note.pslInitialOffset) > 0.001f && note.pslDuration > 0.0f) {
                        voices[v].pitchOffset = note.pslInitialOffset;
                        float totalFrames = fmaxf(1.0f, note.pslDuration);
                        voices[v].pitchSlideTarget = 0.0f;
                        voices[v].pitchSlideRate = -note.pslInitialOffset / totalFrames;
                        voices[v].pitchSliding = true;
                        LOGT("🎵 PSL applied: offset=%.2f, duration=%.0f ticks, rate=%.6f",
                             note.pslInitialOffset, note.pslDuration, voices[v].pitchSlideRate);
                    }
                    // pbnRate is already in semitones/frame — songcore/voice_derive.h divides the
                    // authored per-step rate by framesPerStep before the note reaches the queue.
                    if (fabsf(note.pbnRate) > 0.0001f) {
                        voices[v].pitchSlideRate = note.pbnRate;
                        voices[v].pitchSlideTarget = (note.pbnRate > 0) ? 127.0f : -127.0f;
                        voices[v].pitchSliding = true;
                        LOGT("🎵 PBN applied: rate=%.4f semitones/tick", note.pbnRate);
                    }
                    if (note.vibratoDepth > 0.01f) {
                        voices[v].vibratoSpeed = note.vibratoSpeed;
                        voices[v].vibratoDepth = note.vibratoDepth;
                        voices[v].vibratoActive = true;
                        LOGT("🎵 Vibrato applied: speed=%.1fHz, depth=%.2f semitones",
                             note.vibratoSpeed, note.vibratoDepth);
                    }

                    initVoiceModSlots(voices[v], note.sampleId, currentFrame, sampleRate);

                    LOGT("🎵 Triggered note at frame %lld: sample=%d, track=%d, rate=%.3f, vol=%.4f, pan=%.2f, startOverride=%d, table=%d, tic=%d, oct=%d, pitch=%d, startRow=%d",
                         (long long)currentFrame, note.sampleId, note.trackId, rate, note.volume, note.pan, note.startPointOverride,
                         note.tableId, effectiveTicRates[0], note.noteOctave, note.notePitch, startRows[0]);
                } else {
                    if (note.sampleId < 0 || note.sampleId >= 256) {
                        LOGT("❌ Invalid sampleId=%d for note at frame %lld", note.sampleId, (long long)currentFrame);
                    } else {
                        LOGT("❌ Sample %d not loaded! Note at frame %lld cannot play", note.sampleId, (long long)currentFrame);
                    }
                }
            } else {
                LOGT("⚠️ No free voice (all 8 fully active) for note at frame %lld, sample=%d", (long long)currentFrame, note.sampleId);
            }
        }
    }

    // Table machinery (tic advance + row FX processing) — ONE implementation for both
    // voice types: processTableTick above (KIL/OFFSET differences resolve via the
    // tableKill/tableOffset overloads).
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].isActive || voices[v].tableId < 0) continue;
        processTableTick(voices[v], numFrames, sampleRate);
        // The ONE place the track's TIC00 cursor is written — below the row logic, so it cannot drift
        // from it. Only the voice a retrigger would have read (live, not fading) owns the cursor;
        // letting a fading voice write it would make the value depend on slot order.
        //
        // Written when ANY column is at TIC00, and it stores all three: the retrigger re-checks each
        // column's own rate, exactly as it does when reading them off a live voice.
        bool anyTic00 = false;
        for (int l = 0; l < TABLE_LANES; ++l) anyTic00 |= (voices[v].lanes[l].ticRate == 0x00);
        if (anyTic00 && !voices[v].isFadingOut) {
            const int t = voices[v].trackId;
            if (t >= 0 && t < SF_VOICE_COUNT) {
                if (Tic00Cursor* c = tic00Slot(t, voices[v].tableId, /*create=*/true)) {
                    for (int l = 0; l < TABLE_LANES; ++l) {
                        c->row[l]           = voices[v].lanes[l].row;
                        c->lastProcessed[l] = voices[v].lanes[l].lastProcessed;
                        c->ticRate[l]       = voices[v].lanes[l].ticRate;
                        c->active[l]        = voices[v].lanes[l].active;
                    }
                    tic00Sounding[t] = voices[v].tableId;   // what the TABLE screen draws
                }
            }
        }
    }
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        if (sfVoices[t].isActive && sfVoices[t].tableId >= 0)
            processTableTick(sfVoices[t], numFrames, sampleRate);
    }

    for (int v = 0; v < MAX_VOICES; v++) {
        Voice& voice = voices[v];
        if (!voice.isActive) continue;
        updateVoicePitchMod(voice, numFrames, sampleRate);
    }

    // Snapshot envValues before advancing so the mix loop can interpolate
    // per-sample (eliminates block-rate staircase artifacts on short envelopes).
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].isActive) continue;
        for (int m = 0; m < 4; m++)
            voices[v].voiceMods[m].prevEnvValue = voices[v].voiceMods[m].envValue;
    }
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].isActive) continue;
        updateVoiceModulation(voices[v], numFrames, sampleRate);
    }

    // Apply per-voice PAN and FILTER modulation (once per block)
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice& voice = voices[v];
        if (!voice.isActive) continue;

        // PAN modulation: snapshot before update so the mix loop can interpolate per-sample
        voice.prevPanLeft  = voice.panLeft;
        voice.prevPanRight = voice.panRight;
        if (fabsf(voice.params.mod[PARAM_PAN]) > 0.001f) {
            float modPan = fmaxf(0.0f, fminf(1.0f, voice.params.get(PARAM_PAN)));
            float panAngle = modPan * (float)M_PI * 0.5f;
            voice.panLeft  = cosf(panAngle);
            voice.panRight = sinf(panAngle);
        }

        // FILTER modulation: snapshot then recompute coefficients when LFO/ADSR drives CUT or RES
        voice.chain.filter.snapshotCoeffs();
        if (voice.chain.filter.enabled() &&
                (fabsf(voice.params.mod[PARAM_FILTER_CUT]) > 0.5f ||
                 fabsf(voice.params.mod[PARAM_FILTER_RES]) > 0.5f)) {
            int modCut = std::max(0, std::min(255, (int)voice.params.get(PARAM_FILTER_CUT)));
            int modRes = std::max(0, std::min(255, (int)voice.params.get(PARAM_FILTER_RES)));
            voice.chain.filter.setParams(voice.chain.filter.type, modCut, modRes, voice.chain.filter.drive, sampleRate);
        }

        // Auto-stop looping voice when volume envelope completes
        // AHD/DRUM done at stage 4; ADSR/TRIG done at stage 5
        if (voice.loopMode != 0) {
            bool hasVolMod = false, allDone = true;
            for (int m = 0; m < 4; m++) {
                const VoiceModSlot& mod = voice.voiceMods[m];
                if (mod.dest == 1 && (mod.type == 1 || mod.type == 2 || mod.type == 4 || mod.type == 5)) {
                    hasVolMod = true;
                    int doneStage = (mod.type == 2 || mod.type == 5) ? 5 : 4;
                    if (mod.stage < doneStage) allDone = false;
                }
            }
            if (hasVolMod && allDone) voice.isActive = false;
        }
    }

    // Mix voices — try_lock so applyRateAndBits can swap buffers safely.
    // If the edit lock is held we skip one callback (~10ms silence) instead of crashing.
    {
    std::unique_lock<std::mutex> editLock(sampleEditMutex, std::try_to_lock);
    if (editLock.owns_lock()) {
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice& voice = voices[v];
        if (!voice.isActive || !voice.sampleData) continue;
        // The slot's buffers changed since this voice was triggered: its pointer may be freed.
        if (voice.sampleGen != sampleGen[voice.instrId].load(std::memory_order_relaxed)) {
            voice.stop();
            continue;
        }

        int effDrive     = std::max(0, std::min(255, (int)(voice.params.base[PARAM_DRIVE]      + voice.modDestValues[PARAM_DRIVE])));
        int effCrush      = std::max(0, std::min(15,  (int)(voice.params.base[PARAM_CRUSH]      + voice.modDestValues[PARAM_CRUSH])));
        int effDownsample = std::max(0, std::min(15,  (int)(voice.params.base[PARAM_DOWNSAMPLE] + voice.modDestValues[PARAM_DOWNSAMPLE])));
        voice.chain.drive.setDrive(effDrive);
        voice.chain.crush.setParams(effCrush, 0);   // sampler: downsample=0, pre-interp handles it
        {
            int sl = voice.sampleLength;
            // START/END are re-derived every block so a mod route can move them WHILE the note rings.
            // ⚠️ An exact-frame window (note-queue.h) survives that: it is carried on the voice because
            // the 0-255 pair below cannot express it, and re-deriving would silently widen the sample
            // editor's audition to the whole file the moment its first block was mixed. A voice under a
            // frame window is not modulating its endpoints — a window is a property of the SLOT, and the
            // one caller that arms it is auditioning a cut with the modulation switched off anyway.
            if (voice.windowStartFrame >= 0) {
                voice.actualStart = std::max(0, std::min(voice.windowStartFrame, sl - 2));
                voice.actualEnd   = std::max(voice.actualStart + 1, std::min(voice.windowEndFrame, sl - 1));
            } else {
                float rawStart   = voice.params.base[PARAM_SAMPLE_START] + voice.modDestValues[PARAM_SAMPLE_START];
                float rawEnd     = voice.params.base[PARAM_SAMPLE_END]   + voice.modDestValues[PARAM_SAMPLE_END];
                derive_sample_window(rawStart, rawEnd, sl, voice.actualStart, voice.actualEnd);
            }
            float rawLoop    = voice.params.base[PARAM_LOOP_START]   + voice.modDestValues[PARAM_LOOP_START];
            voice.actualLoopStart = std::max(voice.actualStart, std::min((int)(rawLoop * sl / 255.0f), voice.actualEnd - 1));
            voice.actualLoopEnd   = std::max(voice.actualLoopStart + 1, std::min((int)((float)voice.loopEndNorm * sl / 255.0f), voice.actualEnd));

            // ── LPO: slide the whole window, BOTH ends by the same amount ───────────────────────
            //
            // ⚠️⚠️ **BOTH BOUNDS OR NOTHING.** Moving one changes the loop's LENGTH, and on a looped
            // note the length is the pitch — which is the exact opposite of what this command is for.
            // The two lines below are the command.
            //
            // ⭐ The offset is derived from the RUNNING TOTAL, never accumulated as samples: on a
            // loop length that is not a multiple of 16 each step rounds, and sixteen rounded steps
            // do not add up to one loop. From the total, sixteen steps of `01` land exactly where one
            // step of `10` lands, on every length.
            //
            // ⚠️ The COUNT is clamped, not just the offset. Clamping only the offset would let the
            // count wind up past the end of the sample, so a step back would have an overshoot to
            // unwind before the window moved at all. Clamped, the window simply STOPS — LGPT's
            // behaviour, and a wrap would make a drone jump.
            //
            // ⚠️⚠️ **THE PLAYHEAD MOVES WITH THE WINDOW, AND THAT IS THE COMMAND'S WHOLE FEEL.** The
            // window is not what travels — think of the window as fixed and the SAMPLE as sliding
            // underneath it. So the playhead keeps its position WITHIN the loop and the material
            // under it changes, right now, in the block the cell lands in.
            //
            // Leave the playhead where it is instead and both directions are wrong in their own way:
            // forward, it has to run to the end of the old window before it ever enters the new one,
            // so a whole-loop step is heard a loop late; backward, it is already past the new end, so
            // it snaps to the loop start at an arbitrary phase and **that snap is an audible click** —
            // certain on a whole-loop step, about one press in sixteen on a sixteenth.
            //
            // ⭐ Held as the offset LAST APPLIED, so the shift is a difference between two values both
            // derived from the running total. Accumulating it per call would reintroduce the rounding
            // drift the total exists to avoid, and a count that returns to zero would strand the
            // playhead where the last slide left it.
            const int len = voice.actualLoopEnd - voice.actualLoopStart;
            int       off = 0;
            if (voice.loopSlideSixteenths != 0 && len > 0) {
                const int maxSixteenths = (int)(((int64_t)(voice.actualEnd - voice.actualLoopEnd) * 16) / len);
                const int minSixteenths = (int)(((int64_t)(voice.actualStart - voice.actualLoopStart) * 16) / len);
                voice.loopSlideSixteenths = std::max(minSixteenths,
                                                     std::min(voice.loopSlideSixteenths, maxSixteenths));
                off = (int)(((int64_t)voice.loopSlideSixteenths * len) / 16);
            }
            if (off != voice.loopSlideFrames) {
                // ⚠️ ONLY WHILE THE PLAYHEAD IS INSIDE THE LOOP. Before the first wrap it is still in
                // the intro (start → loop start), and after a note-off on an ADSR voice it is running
                // the release tail with the loop abandoned; neither is a position the window owns, and
                // dragging it would walk a released note backwards into the loop it just left.
                const int wasStart = voice.actualLoopStart + voice.loopSlideFrames;
                const int wasEnd   = voice.actualLoopEnd   + voice.loopSlideFrames;
                if (voice.position >= (double)wasStart && voice.position < (double)wasEnd)
                    voice.position += (double)(off - voice.loopSlideFrames);
                voice.loopSlideFrames = off;
            }
            voice.actualLoopStart += off;
            voice.actualLoopEnd   += off;
        }

        // ⚠️ AFTER the loop bounds, not before: oscillator mode's rate is derived from the loop
        // LENGTH, so reading it above would run a block behind every LPO slide and every loop edit.
        float modulatedRate = getModulatedPlaybackRate(voice);

        // Honour the intra-block trigger offset: a note dispatched at blockStart+f must not sound
        // before frame f. The fade has the same offset (fadeStartFrame, held in the loop below);
        // params stay block-quantized — they are ramped across the block, and the ramp is the
        // anti-click.
        int startFrame = 0;
        if (voice.startDelayFrames > 0) {
            startFrame = std::min(voice.startDelayFrames, numFrames);
            voice.startDelayFrames = 0;
        }

        for (int i = startFrame; i < numFrames; i++) {
            int idx = (int)voice.position;
            // frac computed in double THEN narrowed: (float)idx is inexact past 2^24, which
            // would corrupt frac for exactly the long samples double position exists for.
            float frac = (float)(voice.position - (double)idx);

            // Bounds check - need idx+1 for interpolation
            if (idx < 0 || idx >= voice.sampleLength - 1) {
                if (idx < 0) {
                    voice.isActive = false;  // negative position: safety hard-stop
                } else {
                    // At or past last interpolation point: fade out so SVF resonance decays
                    voice.position = (double)(voice.sampleLength - 2);
                    voice.startFadeOut();  // no-op if already fading
                }
                break;
            }

            // STEP 4 scalars (shared by mono and stereo paths)
            float t = (numFrames > 1) ? (float)(i + 1) / (float)numFrames : 1.0f;
            float panL = voice.prevPanLeft  + (voice.panLeft  - voice.prevPanLeft)  * t;
            float panR = voice.prevPanRight + (voice.panRight - voice.prevPanRight) * t;
            float finalVol = voice.volume;
            for (int m = 0; m < 4; m++) {
                const VoiceModSlot& mod = voice.voiceMods[m];
                if (mod.type == 0 || mod.stage == 0) continue;
                if (mod.dest == 1) {
                    if (mod.type == 3) {
                        float envAtI = mod.prevEnvValue + (mod.envValue - mod.prevEnvValue) * t;
                        finalVol = fmaxf(0.0f, finalVol * (1.0f + envAtI * mod.effectiveAmt));
                    } else {
                        float envAtI = mod.prevEnvValue + (mod.envValue - mod.prevEnvValue) * t;
                        finalVol = fmaxf(0.0f, finalVol + (envAtI - 1.0f) * mod.effectiveAmt);
                    }
                }
            }
            float volRoute = voice.prevModDestValues[PARAM_VOL]
                           + (voice.modDestValues[PARAM_VOL] - voice.prevModDestValues[PARAM_VOL]) * t;
            // ⚠️ SF_VOICE_COUNT, not 8: the preview lane is index 8 and now carries a real fader.
            // Bounded at 8 the sampler path would hold unity while the SoundFont path (which indexes
            // the same array by trackId with no such clamp) followed it — two readings of one array.
            // ⚠️ THE FADER AND THE MUTE GATE BOTH RIDE HERE, interpolated across the block on the same
            // `t` as pan: together they are everything between a mixer move and a step in the output.
            float trackVol = (voice.trackId >= 0 && voice.trackId < SF_VOICE_COUNT)
                           ? (trackVolStart[voice.trackId]
                              + (trackVolEnd[voice.trackId] - trackVolStart[voice.trackId]) * t)
                             * (gateStart[voice.trackId]
                                + (gateEnd[voice.trackId] - gateStart[voice.trackId]) * t)
                           : 1.0f;
            float antiClick = voice.antiClickFade();

            // Sample fetch + per-voice chain is the ONLY mono/stereo difference; a mono
            // sample simply feeds the same value to both lanes (procL == procR). The shared
            // tail below replaces two ~40-line copies (stereo semantics; the mono path's
            // multiplies regroup by one ulp at most).
            float procL, procR;
            if (voice.sampleDataRight) {
                // ── STEREO FETCH ─────────────────────────────────────────────────
                float s1L, s2L, s1R, s2R;
                if (effDownsample > 0) {
                    int factor = 1 << effDownsample;
                    int qi = (idx / factor) * factor;
                    s1L = s2L = voice.sampleData[qi];
                    s1R = s2R = voice.sampleDataRight[qi];
                } else {
                    s1L = voice.sampleData[idx];       s2L = voice.sampleData[idx + 1];
                    s1R = voice.sampleDataRight[idx];  s2R = voice.sampleDataRight[idx + 1];
                }
                procL = s1L + (s2L - s1L) * frac;
                procR = s1R + (s2R - s1R) * frac;
                voice.chain.filter.setInterpolatedCoeffs(t);
                voice.chain.processStereo(procL, procR);
            } else {
                // ── MONO FETCH ───────────────────────────────────────────────────
                float sample1 = voice.sampleData[idx];
                float sample2 = voice.sampleData[idx + 1];
                if (effDownsample > 0) {
                    int downsampleFactor = 1 << effDownsample;
                    int quantizedIdx = (idx / downsampleFactor) * downsampleFactor;
                    sample1 = voice.sampleData[quantizedIdx];
                    sample2 = voice.sampleData[quantizedIdx];
                }
                float processedSample = sample1 + (sample2 - sample1) * frac;
                voice.chain.filter.setInterpolatedCoeffs(t);
                procL = procR = voice.chain.processMono(processedSample);
            }

            // ── SHARED TAIL: sends → global gain → fade-out → pan ────────────────
            //
            // The send tap sits ABOVE the track fader, so a send is PRE-FADER with respect to the
            // mixer and POST-fader with respect to everything on the instrument (VOL, the phrase `V`
            // column, VOL mods). Pulling a track down therefore leaves its reverb and delay tails at
            // full level, by design. ⚠️ The MASTER fader is NOT in this list — it multiplies the summed
            // bus below, after the returns come back, so it is the one fader that carries the tails
            // with it (the volume chain the manual documents ends at a real master).
            float scalar = finalVol * volRoute;
            procL *= scalar;
            procR *= scalar;

            // ⚠️⚠️ **THE VOICE'S OWN FADES MUST REACH THE SENDS, AND THE TRACK FADER MUST NOT.**
            // They are two different things that used to sit on the same side of the tap:
            //
            //   * `antiClick` and the KIL/steal fade-out are the VOICE's envelope — the ramps that
            //     exist so a note never starts or stops on a discontinuity. A send that misses them
            //     receives a waveform cut off mid-cycle, so a KIL that is clean on the dry signal
            //     puts a click into the reverb and delay tails, which then ring on for seconds.
            //   * `trackVol` is the MIXER fader, and it stays below the tap on purpose: sends are
            //     pre-fader, so pulling a track down leaves its tails at full level (see below).
            //
            // The fade is resolved HERE, once, because it advances a counter and ends the voice —
            // and it is applied to the dry path in its original position and order below, so the
            // dry signal is arithmetically untouched by this.
            // A fade dispatched at frame f of this block is held until the loop reaches f.
            const bool fading = voice.isFadingOut && i >= voice.fadeStartFrame;
            float voiceFade   = antiClick;
            float fo          = 1.0f;
            if (fading) {
                fo = (float)voice.fadeOutRemaining / (float)voice.fadeOutTotal;
                voiceFade *= fo;
                if (--voice.fadeOutRemaining <= 0) {
                    voice.isFadingOut = false;
                    voice.isActive = false;
                }
            }

            if ((stemsMode == 0 || stemsMode >= 9) && voice.reverbSend > 0.0f) {
                revSendBufL[i] += procL * voiceFade * panL * voice.reverbSend;
                revSendBufR[i] += procR * voiceFade * panR * voice.reverbSend;
            }
            if ((stemsMode == 0 || stemsMode >= 9) && voice.delaySend > 0.0f) {
                dlySendBufL[i] += procL * voiceFade * panL * voice.delaySend;
                dlySendBufR[i] += procR * voiceFade * panR * voice.delaySend;
            }

            float globalMul = trackVol * antiClick;
            procL *= globalMul;
            procR *= globalMul;

            if (fading) {
                procL *= fo;
                procR *= fo;
            }

            float sampleL = procL * panL;
            float sampleR = procR * panR;

            if (stemsMode == 0 || voice.trackId == stemsMode - 1) {
                output[i * channelCount] += sampleL;
                output[i * channelCount + 1] += sampleR;
            }

            if (!voice.isFadingOut && voice.trackId >= 0 && voice.trackId < 8) {
                framePeaksPerTrackL[voice.trackId] = peak_hold(framePeaksPerTrackL[voice.trackId], sampleL);
                framePeaksPerTrackR[voice.trackId] = peak_hold(framePeaksPerTrackR[voice.trackId], sampleR);
            }
            // OCTA per-track capture: tracks 0-7 plus the preview lane (PREVIEW_TRACK_ID == PREVIEW_LANE).
            // Gated on octaWanted: the accumulators are only zeroed/read when OCTA is shown.
            if (octaWanted && voice.trackId >= 0 && voice.trackId < TRACK_WAVEFORM_COUNT) {
                if (!voice.isFadingOut) trackWasActive[voice.trackId] = true;
                trackWaveAccumL[voice.trackId][i] += sampleL;
                trackWaveAccumR[voice.trackId][i] += sampleR;
            }
            if (monitoredInstrId >= 0 && voice.instrId == monitoredInstrId) {
                instrSpectrumTempL[i] += 0.5f * (sampleL + sampleR);
            }

            if (!voice.isActive) break;

            // Active looping is bounded by LOOP END (region [loopStart, loopEnd]). Once loopReleasing
            // is set (ADSR note-off on a looping voice) the loop is abandoned: every mode runs forward
            // to actualEnd so the [loopEnd, end] tail plays out under the release envelope, then fades.
            if (voice.loopMode == LOOP_MODE_PINGPONG && !voice.loopReleasing) {
                if (voice.loopingBack) {
                    voice.position -= modulatedRate;
                    if (voice.position <= voice.actualLoopStart) {
                        voice.loopingBack = false;
                        voice.position = (double)voice.actualLoopStart;
                    }
                } else {
                    voice.position += modulatedRate;
                    if (voice.position >= voice.actualLoopEnd) {
                        voice.loopingBack = true;
                        voice.position = (double)voice.actualLoopEnd;
                    }
                }
            } else if (voice.reverse && !voice.loopReleasing) {
                voice.position -= modulatedRate;
                if (voice.position <= voice.actualStart) {
                    if (isForwardLoopMode(voice.loopMode)) {
                        voice.position = (double)voice.actualLoopStart;
                    } else {
                        voice.position = (double)voice.actualStart;
                        voice.startFadeOut();
                        break;
                    }
                }
            } else {
                voice.position += modulatedRate;
                bool activeForwardLoop = (isForwardLoopMode(voice.loopMode) && !voice.loopReleasing);
                double fwdBoundary = activeForwardLoop ? (double)voice.actualLoopEnd : (double)voice.actualEnd;
                if (voice.position >= fwdBoundary) {
                    if (activeForwardLoop) {
                        voice.position = (double)voice.actualLoopStart;
                    } else {
                        voice.position = (double)(voice.actualEnd - 1);
                        voice.startFadeOut();
                        break;
                    }
                }
            }
        } // for (int i = 0; i < numFrames; i++)
    } // for (int v = 0; v < MAX_VOICES; v++)
    } // if (editLock.owns_lock())
    } // sampleEditMutex try_lock scope

    {
        // sfBuf (per-track SF render, PROCESS_SUBBLOCK frames * 2 channels) is an engine member; it is
        // memset per use below before each tsf render.
        for (int t = 0; t < SF_VOICE_COUNT; t++) {
            SoundfontVoice& sv = sfVoices[t];
            if (!sv.isActive) continue;

            updateVoiceModulation(sv, numFrames, (float)sampleRate);

            // The note's gain at the end of this block, and — for a note armed this block — at its
            // onset, where an envelope with an attack starts from zero. A finished AHD/ADSR still
            // counts: its value is 0, and skipping it would bring the note back at full volume.
            float noteVol   = sv.modDestValues[PARAM_VOL];
            float onsetVol  = noteVol;
            bool  hasVolEnv = false, volEnvDone = true;
            for (int m = 0; m < 4; m++) {
                VoiceModSlot& mod = sv.voiceMods[m];
                if (mod.type == 0 || mod.stage == 0 || mod.dest != 1) continue;
                if (mod.type == 3) {  // LFO: bipolar tremolo
                    noteVol  = fmaxf(0.0f, noteVol  * (1.0f + mod.envValue * mod.effectiveAmt));
                    onsetVol = fmaxf(0.0f, onsetVol * (1.0f + mod.envValue * mod.effectiveAmt));
                } else if (mod.type == 1 || mod.type == 2 || mod.type == 4 || mod.type == 5) {
                    // Unipolar gain reduction. AHD/DRUM are done at stage 4, ADSR/TRIG at stage 5.
                    hasVolEnv = true;
                    if (mod.stage < ((mod.type == 2 || mod.type == 5) ? 5 : 4)) volEnvDone = false;
                    const float onsetEnv = mod.attackSamples > 0 ? 0.0f : 1.0f;
                    noteVol  = fmaxf(0.0f, noteVol  + (mod.envValue - 1.0f) * mod.effectiveAmt);
                    onsetVol = fmaxf(0.0f, onsetVol + (onsetEnv     - 1.0f) * mod.effectiveAmt);
                }
            }
            sv.volGainFrom = sv.hasArmedNote ? onsetVol : sv.volGain;
            sv.volGainTo   = noteVol;
            // PAN modulation. A SoundFont voice has no panLeft/panRight gains in the mix loop — TSF
            // pans on its own channel — so the modulated value goes back through
            // tsf_channel_set_pan instead, guarded by the same |mod| > 0.001 test the sampler path
            // uses so an unmodulated voice keeps whatever pan the note or a PAN effect gave it.
            const bool panModded = fabsf(sv.params.mod[PARAM_PAN]) > 0.001f;
            const float modPan = panModded ? fmaxf(0.0f, fminf(1.0f, sv.params.get(PARAM_PAN))) : 0.0f;

            int volSlot = sv.sfSlot;
            if (panModded && volSlot >= 0 && volSlot < MAX_SOUNDFONTS) {
                tsf* h = soundfonts[volSlot].handle.load();
                if (h) tsf_channel_set_pan(h, t, modPan);
            }

            // Every VOL envelope has finished: the note is over, or — with AMT below FF — held at a
            // level it can no longer leave. Either way it ends, faded from where this block's ramp
            // leaves it, so a partial AMT cannot end in a step. (An armed note's envelopes have
            // only just started, so this never ends a note before it is heard.)
            if (hasVolEnv && volEnvDone && !sv.hasArmedNote) sv.startStopFade(DECLICK_SAMPLES);

            // If filter mod is active, snapshot then recompute coefficients via InstrumentChain.
            sv.chain.filter.snapshotCoeffs();
            if (sv.chain.filter.enabled()) {
                int modCut = std::max(0, std::min(255,
                    (int)(sv.instrParams.filterCut + sv.modDestValues[PARAM_FILTER_CUT])));
                int modRes = std::max(0, std::min(255,
                    (int)(sv.instrParams.filterRes + sv.modDestValues[PARAM_FILTER_RES])));
                if (modCut != sv.instrParams.filterCut || modRes != sv.instrParams.filterRes) {
                    sv.chain.filter.setParams(sv.chain.filter.type, modCut, modRes, sv.chain.filter.drive, (int)sampleRate);
                }
            }

            sv.applyPitchMod((float)sampleRate, numFrames);
        }

        for (int t = 0; t < SF_VOICE_COUNT; t++) {
            SoundfontVoice& sv = sfVoices[t];
            int slot = sv.sfSlot;
            if (!sv.isActive || slot < 0 || slot >= MAX_SOUNDFONTS) continue;

            memset(sfBuf, 0, sizeof(float) * numFrames * 2);
            // Honour the intra-block trigger offset (see the sampler mix loop): the note starts at its
            // exact frame, and everything before it belongs to whatever this track was already playing.
            int sfStart = 0;
            if (sv.startDelayFrames > 0) {
                sfStart = std::min(sv.startDelayFrames, numFrames);
                sv.startDelayFrames = 0;
            }
            bool rendered = false;
            {
                tsf* h = soundfonts[slot].handle.load();   // valid until this block ends
                if (h && !sv.hasArmedNote) {
                    // A note-off dispatched mid-block is sent between the two halves of the render,
                    // so TSF's release begins on its frame. TSF steps its envelope per render call
                    // (in 64-sample chunks from the call's first frame), which is what makes the
                    // split land it exactly.
                    const int offAt = sv.pendingTsfOffAt;
                    if (offAt > 0 && offAt < numFrames) {
                        tsf_render_float_channel(h, t, sfBuf, offAt, 0 /* overwrite */);
                        tsf_channel_note_off(h, t, sv.pendingTsfOffNote);
                        tsf_render_float_channel(h, t, sfBuf + offAt * 2, numFrames - offAt, 0 /* overwrite */);
                    } else {
                        // Pending at 0: carried over from a block that did not render this voice.
                        if (offAt >= 0) tsf_channel_note_off(h, t, sv.pendingTsfOffNote);
                        tsf_render_float_channel(h, t, sfBuf, numFrames, 0 /* overwrite */);
                    }
                    sv.pendingTsfOffAt = -1;
                    applyGainRamp(sfBuf, numFrames, sv.volGainFrom, sv.volGainTo);
                    rendered = true;
                } else if (h) {
                    // ⚠️⚠️ A NOTE THAT STEALS ANOTHER IS RENDERED IN TWO PASSES WITH A FADE BETWEEN THEM,
                    // AND EVERY PIECE OF THAT IS LEVERAGE AGAINST THE SAME CRACK.
                    //
                    // Pass one is the note being REPLACED, rendered up to `fadeEnd` — past the new
                    // note's own onset. It used to be rendered not at all: the trigger note_on'd where
                    // the note was SCHEDULED, one pass earlier, killing the old TSF voices before a
                    // single frame of this block existed, so the block came out silent up to `sfStart`
                    // and the previous note ended in a step at the block boundary, at whatever amplitude
                    // its waveform happened to be at. That is what the armed note is for.
                    //
                    // ⚠️ AND THE FADE IS OURS, NOT TSF'S — this is the part that is not obvious. TSF
                    // computes its amplitude envelope ONCE PER 64-SAMPLE BLOCK and holds it flat across
                    // it (`gainMono = noteGain * v->ampenv.level` in tsf_voice_render). Its short
                    // release, `tsf_voice_endquick`, drops the level to 26% at the first of those
                    // boundaries — so asking TSF to fade a stolen note out quickly buys a smaller step,
                    // not no step. Measured: still 0.33 of peak. A ramp applied to the rendered samples
                    // has no such granularity, which is also why the sampler pool fades its own steals
                    // here rather than in a voice (DECLICK_SAMPLES, audio-defs.h — the same length).
                    //
                    // `fadeEnd` is clamped to the block, so the ramp slides EARLIER when a note lands
                    // near the end of one; a note landing at frame 0 still gets the full 64 samples. It
                    // is `min(fadeEnd, DECLICK_SAMPLES)` long either way, never a stub.
                    const int fadeEnd   = std::min(numFrames, sfStart + DECLICK_SAMPLES);
                    const int rampStart = std::max(0, fadeEnd - DECLICK_SAMPLES);
                    const int rampLen   = fadeEnd - rampStart;
                    tsf_render_float_channel(h, t, sfBuf, fadeEnd, 0 /* overwrite */);
                    // The old note keeps the gain it ended the last block on — the new note's
                    // envelope has already replaced the mods, and must not reach the note it cuts.
                    applyGainRamp(sfBuf, fadeEnd, sv.volGain, sv.volGain);
                    for (int i = rampStart; i < fadeEnd; i++) {
                        const float g = (float)(fadeEnd - i - 1) / (float)(rampLen > 1 ? rampLen - 1 : 1);
                        sfBuf[i * 2]     *= g;
                        sfBuf[i * 2 + 1] *= g;
                    }
                    // Now the old voices can be cut: the samples they contributed are already at zero.
                    sv.fireArmedNote(h);
                    // Rendered apart and ADDED — [sfStart, fadeEnd) still holds the tail of the fade,
                    // and the two notes carry different gains across it.
                    const int noteFrames = numFrames - sfStart;
                    if (noteFrames > 0) {
                        tsf_render_float_channel(h, t, sfNoteBuf, noteFrames, 0 /* overwrite */);
                        applyGainRamp(sfNoteBuf, noteFrames, sv.volGainFrom, sv.volGainTo);
                        for (int i = 0; i < noteFrames * 2; i++) sfBuf[sfStart * 2 + i] += sfNoteBuf[i];
                    }
                    rendered = true;
                }
            }
            if (!rendered) continue;
            sv.volGain = sv.volGainTo;

            // ⚠️ THE TRACK FADER IS APPLIED TO THE RENDERED SAMPLES, NOT TO THE TSF CHANNEL. A channel
            // volume is one value per render call, so the fader could only step at a block edge — the
            // staircase a knob turns into a tick per message. Here it is a ramp, like the note's own
            // gain two lines up. It stays ABOVE the chain, where the channel volume had it, so an
            // instrument's drive and filter hear the same signal they always did.
            if (trackVolStart[t] != 1.0f || trackVolEnd[t] != 1.0f)
                applyGainRamp(sfBuf, numFrames, trackVolStart[t], trackVolEnd[t]);

            // ⚠️ THE MUTE GATE IS APPLIED HERE, and it has to be ABOVE the send tap below: the fader
            // is already in the buffer, which makes a SoundFont send post-fader where the sampler's is
            // pre-fader, and a muted SF track has always taken its reverb and delay down with it.
            // ⚠️ THE TRANSPORT-STOP RAMP RIDES HERE, on the gate and for the gate's own reason: it has
            // to be per sample (a per-block value is the staircase both ramps exist to remove), it has
            // to sit BELOW the filter so a stop cannot slam the chain under a note still ringing
            // through it, and it has to be ABOVE the send tap so the reverb and delay are fed the
            // faded signal rather than a waveform cut off mid-cycle.
            bool stopFadeDone = false;
            for (int i = 0; i < numFrames; i++) {
                float lerp_t = (numFrames > 1) ? (float)(i + 1) / (float)numFrames : 1.0f;
                float L = sfBuf[i * 2];
                float R = sfBuf[i * 2 + 1];
                sv.chain.filter.setInterpolatedCoeffs(lerp_t);
                sv.chain.processStereo(L, R);
                float gate = gateStart[t] + (gateEnd[t] - gateStart[t]) * lerp_t;
                if (sv.stopFadeRemaining > 0) {
                    if (i >= sv.stopFadeStartFrame) {   // a ramp dispatched mid-block waits for its frame
                        gate *= (float)sv.stopFadeRemaining / (float)sv.stopFadeTotal;
                        if (--sv.stopFadeRemaining <= 0) stopFadeDone = true;
                    }
                } else if (stopFadeDone) {
                    gate = 0.0f;   // the ramp ended inside this block; the rest of it is silence
                }
                sfBuf[i * 2]     = L * gate;
                sfBuf[i * 2 + 1] = R * gate;
            }

            // SEND TAP: stereo post-chain SF buffer into reverb/delay buses
            if ((stemsMode == 0 || stemsMode >= 9) && (sv.instrParams.reverbSend > 0.0f || sv.instrParams.delaySend > 0.0f)) {
                for (int i = 0; i < numFrames; i++) {
                    revSendBufL[i] += sfBuf[i * 2]     * sv.instrParams.reverbSend;
                    revSendBufR[i] += sfBuf[i * 2 + 1] * sv.instrParams.reverbSend;
                    dlySendBufL[i] += sfBuf[i * 2]     * sv.instrParams.delaySend;
                    dlySendBufR[i] += sfBuf[i * 2 + 1] * sv.instrParams.delaySend;
                }
            }

            float trackPeakL = 0.0f, trackPeakR = 0.0f;
            if (octaWanted) trackWasActive[t] = true;  // OCTA capture only
            for (int i = 0; i < numFrames; i++) {
                float outL = sfBuf[i * 2];
                float outR = sfBuf[i * 2 + 1];
                // Pre-master, like the sampler path's sampleL/sampleR — the master fader is applied to
                // the summed bus below, and the meters and OCTA accumulators are scaled by it there.
                trackPeakL = peak_hold(trackPeakL, outL);
                trackPeakR = peak_hold(trackPeakR, outR);
                if (stemsMode == 0 || t == stemsMode - 1) {
                    output[i * 2]     += outL;
                    output[i * 2 + 1] += outR;
                }
                if (octaWanted) {
                    trackWaveAccumL[t][i] += outL;
                    trackWaveAccumR[t][i] += outR;
                }
                if (monitoredInstrId >= 0 && sv.instrId == monitoredInstrId) {
                    instrSpectrumTempL[i] += 0.5f * (outL + outR);
                }
            }
            float trackPeak = fmaxf(trackPeakL, trackPeakR);
            if (t < 8) {  // mixer meters cover song tracks only, not the preview lane
                framePeaksPerTrackL[t] = fmaxf(framePeaksPerTrackL[t], trackPeakL);
                framePeaksPerTrackR[t] = fmaxf(framePeaksPerTrackR[t], trackPeakR);
            }

            // Release tail: when noteOff() was called, keep rendering until TSF goes silent.
            // Suppressed while an ADSR/TRIG VOL release is active (stage 4) — TSF is still
            // generating audio for the fade; the render loop in pass 1 calls hardStop() when
            // the ADSR mod reaches stage 5.
            if (sv.isReleasingOnly && trackPeak < 0.0005f) {
                bool adsrReleasing = false;
                for (int m = 0; m < 4; m++) {
                    const VoiceModSlot& mod = sv.voiceMods[m];
                    if (mod.dest == 1 && (mod.type == 2 || mod.type == 5) && mod.stage == 4) {
                        adsrReleasing = true; break;
                    }
                }
                if (!adsrReleasing) sv.hardStop();
            }

            // The ramp reached zero inside this block, and its last faded samples are already summed
            // into the bus above. Ending the voice here — not where the button was pressed — is the
            // whole difference between a stop that ramps and a stop that cuts.
            if (stopFadeDone) sv.hardStop();
        }
    }

    // The intra-block frames above were for THIS block. A voice the block did not mix (the sample
    // edit lock was held, the handle was gone) keeps its fade or note-off and takes it from the
    // next block's first frame — one block late, never one block early.
    for (int v = 0; v < MAX_VOICES; v++) voices[v].fadeStartFrame = 0;
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        sfVoices[t].stopFadeStartFrame = 0;
        if (sfVoices[t].pendingTsfOffAt > 0) sfVoices[t].pendingTsfOffAt = 0;
    }

    // Per-track waveform capture for OCTA visualizer — only when OCTA is being displayed.
    // try_lock: the UI holds this mutex while it copies or decays the scopes, and a block that
    // finds it held skips the capture rather than wait. A scope missing one block is invisible.
    if (octaWanted) {
        std::unique_lock<std::mutex> lock(waveformMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) trackHasVoice[t] = trackWasActive[t];
            for (int i = 0; i < numFrames; i++) {
                for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) {
                    // × masterVolSnapshot because the accumulators are filled pre-master: OCTA shows
                    // what leaves the master fader, so a master fade takes the scopes down with it.
                    trackWaveformBuffer[t][trackWaveformIndex] =
                        (trackWaveAccumL[t][i] + trackWaveAccumR[t][i]) * 0.5f * masterVolSnapshot;
                }
                trackWaveformIndex = (trackWaveformIndex + 1) % WAVEFORM_SIZE;
            }
        }
    }

    // ─── THE DRY GATE — every track's audio, summed, BELOW every send tap ────────────────────────
    //
    // ⚠️ It has to live here and nowhere else. Soloing a send return means "let me hear only what comes
    // back from the reverb", and the reverb is fed by the tracks: taking the dry mix down as eight track
    // mutes would stop the notes (both schedulers skip an inaudible track) and cut the SoundFont path's
    // send with them, so the soloed return would have nothing to return. Everything above has already
    // tapped the sends; this multiply is what the listener loses.
    if (dryGateStart < 1.0f || dryGateEnd < 1.0f) {
        for (int i = 0; i < numFrames; i++) {
            const float lerp_t = (numFrames > 1) ? (float)(i + 1) / (float)numFrames : 1.0f;
            const float g      = dryGateStart + (dryGateEnd - dryGateStart) * lerp_t;
            output[i * channelCount]     *= g;
            output[i * channelCount + 1] *= g;
        }
    }

    // SEND BUSES: delay first so its output can feed into reverb, then reverb
    {
        // revWet*/dlyWet* are engine members; process() fully overwrites them.
        delaySend.process(dlySendBufL, dlySendBufR, dlyWetL, dlyWetR, numFrames);
        const float dlyToRev = delayToReverbSend.load(std::memory_order_relaxed);
        if (dlyToRev > 0.0001f) {
            for (int i = 0; i < numFrames; i++) {
                revSendBufL[i] += dlyWetL[i] * dlyToRev;
                revSendBufR[i] += dlyWetR[i] * dlyToRev;
            }
        }
        reverbSend.process(revSendBufL, revSendBufR, revWetL, revWetR, numFrames);
        // Only capture when the EQ/spectrum UI is actually polling, and never block the audio
        // thread on the UI's read — try_lock and drop this block's data on contention (invisible).
        if (spectrumWanted) {
            std::unique_lock<std::mutex> lock(spectrumMutex, std::try_to_lock);
            if (lock.owns_lock()) {
                for (int i = 0; i < numFrames; i++) {
                    delaySpectrumBuffer[delaySpectrumWriteIdx] = 0.5f * (dlyWetL[i] + dlyWetR[i]);
                    delaySpectrumWriteIdx = (delaySpectrumWriteIdx + 1) % SPECTRUM_SIZE;
                    reverbSpectrumBuffer[reverbSpectrumWriteIdx] = 0.5f * (revWetL[i] + revWetR[i]);
                    reverbSpectrumWriteIdx = (reverbSpectrumWriteIdx + 1) % SPECTRUM_SIZE;
                }
            }
        }
        const float rvReturn = reverbReturnGain.load(std::memory_order_relaxed);
        const float dlReturn = delayReturnGain.load(std::memory_order_relaxed);
        for (int i = 0; i < numFrames; i++) {
            // ⚠️ The return's own mute rides HERE, below the module, so a muted reverb keeps building
            // its tail while it is silent — unmuting drops you back into the tail the song has been
            // feeding it, not into a reverb that starts from nothing.
            const float lerp_t = (numFrames > 1) ? (float)(i + 1) / (float)numFrames : 1.0f;
            const float rvGate = revGateStart + (revGateEnd - revGateStart) * lerp_t;
            const float dlGate = dlyGateStart + (dlyGateEnd - dlyGateStart) * lerp_t;
            float rv  = revWetL[i] * rvReturn * rvGate;
            float rvR = revWetR[i] * rvReturn * rvGate;
            float dl  = dlyWetL[i] * dlReturn * dlGate;
            float dlR = dlyWetR[i] * dlReturn * dlGate;
            if (stemsMode == 0) {
                output[i * channelCount]     += rv + dl;
                output[i * channelCount + 1] += rvR + dlR;
            } else if (stemsMode == 9) {
                output[i * channelCount]     += rv;
                output[i * channelCount + 1] += rvR;
            } else if (stemsMode == 10) {
                output[i * channelCount]     += dl;
                output[i * channelCount + 1] += dlR;
            }
            // modes 1-8: no send returns (dry track stems)
            frameSendPeakRevL = peak_hold(frameSendPeakRevL, rv);
            frameSendPeakRevR = peak_hold(frameSendPeakRevR, rvR);
            frameSendPeakDelL = peak_hold(frameSendPeakDelL, dl);
            frameSendPeakDelR = peak_hold(frameSendPeakDelR, dlR);
        }
    }

    // ─── THE MASTER FADER — one multiply over the summed bus, dry AND returns ────────────────────
    //
    // ⚠️ It lives HERE, and not with the per-voice gains, because a fader that only scales the dry
    // path is not a master: fading to 00 would leave the reverb and delay returns at full level,
    // playing on over a silent mix. The volume chain the manual documents (instrument VOL × phrase V ×
    // track fader × master) ends at this line, and only this line is downstream of the send returns.
    //
    // Placed before masterChain, which is where it has always been relative to the master EQ, bus FX
    // and limiter — the limiter still sees a post-fader signal, so pulling the master down still backs
    // it off rather than being squashed flat by it.
    //
    // ⚠️ IT RAMPS ACROSS THE BLOCK, like every other fader here: a `VMV` moves it once per block and
    // nothing is lost by that, but a knob moves it between blocks and a step in the summed bus is the
    // one a listener hears most clearly. Start and end are equal unless something moved it.
    //
    // The meters and visualiser accumulators were filled pre-master and are scaled to match, so every
    // reading stays post-master as it was — by the value the block ENDS on, which is the fader the
    // screen is showing.
    //
    // The `!= 1.0f` skip is an optimisation and nothing more — multiplying by exactly 1.0f is the
    // identity in IEEE 754, so a project at the default master FF takes the same samples either way.
    if (masterVolStart != 1.0f || masterVolSnapshot != 1.0f) {
        for (int i = 0; i < numFrames; i++) {
            const float g = masterVolStart
                          + (masterVolSnapshot - masterVolStart) * (float)(i + 1) / (float)numFrames;
            for (int c = 0; c < channelCount; c++) output[i * channelCount + c] *= g;
        }
        for (int t = 0; t < 8; t++) {
            framePeaksPerTrackL[t] *= masterVolSnapshot;
            framePeaksPerTrackR[t] *= masterVolSnapshot;
        }
        frameSendPeakRevL *= masterVolSnapshot;
        frameSendPeakRevR *= masterVolSnapshot;
        frameSendPeakDelL *= masterVolSnapshot;
        frameSendPeakDelR *= masterVolSnapshot;
    }

    // Master chain: master EQ → bus FX (OTT or DUST) → limiter
    // Stems mode bypasses EQ and bus FX; only limiter is applied.
    if (stemsMode == 0)
        masterChain.process(output, numFrames, channelCount);
    else
        masterChain.limiter.process(output, numFrames, channelCount);

    // ⚠️ AFTER the master chain, and that placement is the point: a click run through the limiter
    // would duck the whole mix on every beat, and one run through the master EQ would be coloured by
    // a setting that has nothing to do with it. It is a monitor sitting on top of the finished block.
    renderMetronome(output, numFrames, channelCount, sampleRate, blockStartFrame, offlineRender);

    // Only when an instrument is being monitored (EQ screen), and never block on the UI read.
    if (monitoredInstrId >= 0) {
        std::unique_lock<std::mutex> lock(spectrumMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (int i = 0; i < numFrames; i++) {
                // × masterVolSnapshot: the accumulator is filled pre-master (see the fader above), and
                // this curve has always been drawn post-master.
                instrSpectrumBuffer[instrSpectrumWriteIdx] = instrSpectrumTempL[i] * masterVolSnapshot;
                instrSpectrumWriteIdx = (instrSpectrumWriteIdx + 1) % SPECTRUM_SIZE;
            }
        }
    }

    globalFrameCounter.store(blockStartFrame + numFrames, std::memory_order_relaxed);
}

void AudioEngine::processLiveBlock(float* output, int numFrames, int channelCount, float sampleRate) {

    setFlushToZeroForCurrentThread();

    for (int i = 0; i < numFrames * channelCount; i++) {
        output[i] = 0.0f;
    }

    // During offline WAV render: output silence and let renderOffline process the queue. The keys
    // pressed meanwhile are dropped, not saved up — see setLiveInput.
    if (isOfflineRendering.load()) {
        if (LiveInputSource* live = liveInput.load(std::memory_order_acquire)) live->discardLiveInput();
        return;
    }

    // ⚠️ Chunk at PROCESS_SUBBLOCK — the difference from a device-sized block is AUDIBLE, see the constant.
    // A device hands us whatever its period is (the Flip's ALSA: 940 frames; Oboe: 192-960), and
    // processing that in one pass resolves a block's note-ons too coarsely: same-track retriggers
    // sharing a block exhaust the voice pool and get dropped. renderOffline has always chunked at
    // this size, so chunking here is what makes live playback and the export agree.
    int processed = 0;
    while (processed < numFrames) {
        int chunk = std::min((int)numFrames - processed, PROCESS_SUBBLOCK);
        processAudioBlock(output + processed * channelCount, chunk, channelCount, sampleRate);
        processed += chunk;
    }

    // The master scope and the meters below take their mutex with try_lock, as the spectrum does:
    // the UI holds each one while it copies or decays, and the audio thread never waits for that. A
    // block that finds the lock held skips its capture — one block missing from a scope or a meter
    // is invisible.
    {
        std::unique_lock<std::mutex> lock(waveformMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (int i = 0; i < numFrames; i++) {
                waveformDownsampleCounter++;
                if (waveformDownsampleCounter >= WAVEFORM_DOWNSAMPLE) {
                    waveformBuffer[waveformIndex] = output[i * channelCount];
                    waveformIndex = (waveformIndex + 1) % WAVEFORM_SIZE;
                    waveformDownsampleCounter = 0;
                }
            }
        }
    }

    // Master spectrum ring — only while the spectrum visualizer or EQ screen is polling,
    // and try_lock so the audio thread never blocks on the UI's 2048-sample copy-out.
    if ((nowMs() - lastSpectrumReadMs.load(std::memory_order_relaxed)) < CAPTURE_IDLE_MS) {
        std::unique_lock<std::mutex> lock(spectrumMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (int i = 0; i < numFrames; i++) {
                spectrumBuffer[spectrumWriteIdx] = channelCount > 1
                    ? 0.5f * (output[i * channelCount] + output[i * channelCount + 1])
                    : output[i * channelCount];
                spectrumWriteIdx = (spectrumWriteIdx + 1) % SPECTRUM_SIZE;
            }
        }
    }

    // Update peak levels for mixer meters (live-only — not needed during WAV export)
    {
        std::unique_lock<std::mutex> lock(peakMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (int t = 0; t < 8; t++) {
                trackPeaksL[t] *= PEAK_DECAY;
                trackPeaksR[t] *= PEAK_DECAY;
            }
            masterPeakL *= PEAK_DECAY;
            masterPeakR *= PEAK_DECAY;

            for (int t = 0; t < 8; t++) {
                trackPeaksL[t] = fmaxf(trackPeaksL[t], framePeaksPerTrackL[t]);
                trackPeaksR[t] = fmaxf(trackPeaksR[t], framePeaksPerTrackR[t]);
            }

            float maxL = 0.0f, maxR = 0.0f;
            for (int i = 0; i < numFrames; i++) {
                float absL = fabsf(output[i * channelCount]);
                float absR = fabsf(output[i * channelCount + 1]);
                if (absL > maxL) maxL = absL;
                if (absR > maxR) maxR = absR;
            }
            masterPeakL = fmaxf(masterPeakL, maxL);
            masterPeakR = fmaxf(masterPeakR, maxR);

            sendPeakRevL *= PEAK_DECAY; sendPeakRevR *= PEAK_DECAY;
            sendPeakDelL *= PEAK_DECAY; sendPeakDelR *= PEAK_DECAY;
            sendPeakRevL = fmaxf(sendPeakRevL, frameSendPeakRevL);
            sendPeakRevR = fmaxf(sendPeakRevR, frameSendPeakRevR);
            sendPeakDelL = fmaxf(sendPeakDelL, frameSendPeakDelL);
            sendPeakDelR = fmaxf(sendPeakDelR, frameSendPeakDelR);
        }
    }
}

int64_t AudioEngine::getCurrentFrame() {
    return globalFrameCounter.load(std::memory_order_relaxed);
}

void AudioEngine::scheduleNote(int64_t targetFrame, int sampleId, int trackId,
                               float frequency, float baseFrequency, float volume, float phraseVolume, float pan,
                               int startPointOverride, int endPointOverride, int tableId, int tableTicRate,
                               int noteOctave, int notePitch,
                               float pslInitialOffset, float pslDuration,
                               float pbnRate, float vibratoSpeed, float vibratoDepth,
                               int tableStartRow) {
    ScheduledNote note{};
    note.targetFrame        = targetFrame;
    note.sampleId           = sampleId;
    note.trackId            = trackId;
    note.frequency          = frequency;
    note.baseFrequency      = baseFrequency;
    note.volume             = volume;
    note.phraseVolume       = phraseVolume;
    note.pan                = pan;
    note.startPointOverride = startPointOverride;
    note.endPointOverride   = endPointOverride;
    note.tableId            = tableId;
    note.tableTicRate       = tableTicRate;
    note.noteOctave         = noteOctave;
    note.notePitch          = notePitch;
    note.pslInitialOffset   = pslInitialOffset;
    note.pslDuration        = pslDuration;
    note.pbnRate            = pbnRate;
    note.vibratoSpeed       = vibratoSpeed;
    note.vibratoDepth       = vibratoDepth;
    note.tableStartRow      = tableStartRow;
    noteQueue.schedule(note);
}

void AudioEngine::scheduleProgramNote(int64_t targetFrame, int trackId, int instrumentId,
                                      const songcore::NoteOnPayload& noteOn, int tempo,
                                      bool rootAudition) {
    ScheduledNote note{};
    note.targetFrame  = targetFrame;
    note.trackId      = trackId;
    note.instrumentId = instrumentId;   // >= 0 is what marks this note as still undecided
    note.noteOn       = noteOn;
    note.tempo        = tempo;
    note.rootAudition = rootAudition;
    noteQueue.schedule(note);
}

bool AudioEngine::resolveScheduledNote(ScheduledNote& note) {
    // Follow any INS cells the hit passes through. With no INS anywhere this returns the instrument
    // it was given and the table it would have used, so the ordinary note path is unchanged.
    int chainTableId = -1;
    const int sounding = resolveChain(note.trackId, note.instrumentId, note.noteOn.tableId,
                                      &chainTableId);
    if (sounding < 0) return false;                 // an empty slot, or external gear: silence
    note.instrumentId  = sounding;
    note.noteOn.tableId = chainTableId;             // the voice runs the LAST link's table

    const songcore::Program p = programView(note.instrumentId);
    const songcore::DerivedNote d =
        songcore::derive_note(note.noteOn, note.targetFrame, note.trackId, note.instrumentId, p,
                              note.tempo, deviceSampleRate.load(std::memory_order_relaxed),
                              getSampleLength(p.sampleId), note.rootAudition);
    if (!d.valid) return false;

    note.isSoundfont = d.isSoundfont;
    if (d.isSoundfont) {
        const songcore::SoundfontNoteArgs& a = d.soundfont;
        note.sfSlot             = a.sfSlot;
        note.midiNote           = a.midiNote;
        note.midiVelocity       = a.midiVelocity;
        note.volume             = a.vol;
        note.phraseVolume       = a.phraseVol;
        note.pan                = a.pan;
        note.sfBank             = a.bank;
        note.sfPreset           = a.preset;
        note.sampleId           = a.sampleId;
        note.frequency          = 440.0f;
        note.baseFrequency      = 440.0f;
        note.startPointOverride = -1;
        note.tableId            = a.tableId;
        note.tableTicRate       = a.tableTicRate;
        note.noteOctave         = a.noteOctave;
        note.notePitch          = a.notePitch;
        note.pslInitialOffset   = a.pslInitialOffset;
        note.pslDuration        = a.pslDuration;
        note.pbnRate            = a.pbnRate;
        note.vibratoSpeed       = a.vibratoSpeed;
        note.vibratoDepth       = a.vibratoDepth;
        note.tableStartRow      = a.tableStartRow;
        note.detuneSemitones    = a.detuneSemitones;
    } else {
        const songcore::SamplerNoteArgs& a = d.sampler;
        note.sampleId           = a.sampleId;
        note.frequency          = a.frequency;
        note.baseFrequency      = a.baseFrequency;
        note.volume             = a.volume;
        note.phraseVolume       = a.phraseVolume;
        note.pan                = a.pan;
        note.startPointOverride = a.startPointOverride;
        note.endPointOverride   = a.endPointOverride;
        note.tableId            = a.tableId;
        note.tableTicRate       = a.tableTicRate;
        note.noteOctave         = a.noteOctave;
        note.notePitch          = a.notePitch;
        note.pslInitialOffset   = a.pslInitialOffset;
        note.pslDuration        = a.pslDuration;
        note.pbnRate            = a.pbnRate;
        note.vibratoSpeed       = a.vibratoSpeed;
        note.vibratoDepth       = a.vibratoDepth;
        note.tableStartRow      = a.tableStartRow;
    }
    return true;
}

void AudioEngine::scheduleSoundfontNote(int64_t targetFrame, int trackId, int sfSlot,
                                        int midiNote, int midiVelocity, float vol, float pan,
                                        int bank, int preset,
                                        float pslInitialOffset, float pslDuration,
                                        float pbnRate, float vibratoSpeed, float vibratoDepth,
                                        float phraseVol, int sampleId,
                                        int tableId, int tableTicRate,
                                        int noteOctave, int notePitch, int tableStartRow,
                                        float detuneSemitones) {
    ScheduledNote note{};
    note.targetFrame      = targetFrame;
    note.trackId          = trackId;
    note.isSoundfont      = true;
    note.sfSlot           = sfSlot;
    note.midiNote         = midiNote;
    note.midiVelocity     = midiVelocity;
    note.volume           = vol;
    note.phraseVolume     = phraseVol;
    note.pan              = pan;
    note.sfBank           = bank;
    note.sfPreset         = preset;
    note.sampleId         = sampleId;
    note.frequency        = 440.0f;
    note.baseFrequency    = 440.0f;
    note.startPointOverride = -1;
    note.tableId          = tableId;
    note.tableTicRate     = tableTicRate;
    note.noteOctave       = noteOctave;
    note.notePitch        = notePitch;
    note.pslInitialOffset = pslInitialOffset;
    note.pslDuration      = pslDuration;
    note.pbnRate          = pbnRate;
    note.vibratoSpeed     = vibratoSpeed;
    note.vibratoDepth     = vibratoDepth;
    note.tableStartRow    = tableStartRow;
    note.detuneSemitones  = detuneSemitones;
    noteQueue.schedule(note);
}

// Audio thread. The stored row's `sliceMarkers` is null — a self-pointer would not survive the
// copy between the two sides — so it is re-pointed here, at this thread's own copy of the markers.
songcore::Program AudioEngine::programView(int id) const {
    if (id < 0 || id >= songcore::PROGRAM_SLOTS) return songcore::Program{};
    songcore::Program p = programs[id].program;
    p.sliceMarkers = programs[id].markers;
    return p;
}

// Audio thread, once per block: take in every per-instrument record the control thread published.
void AudioEngine::syncInstrumentData() {
    instrumentParams.sync();
    instrumentModSlots.sync();
    programs.sync();
    sfEnvOverrides.sync();
    eqPresets.sync();
    eqPresetHex.sync();
}

void AudioEngine::setSoundfontEnvelopeOverride(int instrumentId, int atk, int dec, int sus, int rel) {
    if (instrumentId < 0 || instrumentId >= 256) return;
    SfEnvOverride& o = sfEnvOverrides.edit(instrumentId);
    o.atk = atk; o.dec = dec; o.sus = sus; o.rel = rel;
    sfEnvOverrides.publish(instrumentId);
}

// ===================================
// SOUNDFONT BANK
// ===================================
// Moved here from jni-bridge.cpp in S6b. Nothing about parsing an SF2 and caching its handle is
// platform-specific, and leaving it behind the JNI wall meant no host build could load one — which
// blocked tools/ptrender and would have forced the SDL shell to write a second slot cache. See the
// header for the de-dup / LRU contract.

void AudioEngine::freeSoundfontSlot(int slot) {
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return;
    // The voices on this slot are detached by the audio thread's next block (the generation), and
    // the handle is closed only once the block that may have loaded it has ended.
    tsf* h = soundfonts[slot].handle.exchange(nullptr);
    soundfonts[slot].gen.fetch_add(1);
    if (h) {
        waitForAudioBlockBoundary();
        tsf_close(h);
    }
    soundfonts[slot].instrumentId = -1;
    soundfonts[slot].filePath.clear();
    soundfonts[slot].bank   = -1;
    soundfonts[slot].preset = -1;
}

/**
 * Turn a file plus a bank/preset into a parsed tsf handle. No slot is touched and no member is
 * written except `lastLoadFailure_`'s answer, which comes back through `failure` instead — this runs
 * on the background worker as often as on the calling thread.
 */
// ⚠️ Everything tsf would otherwise allocate on the AUDIO thread, done here: it grows its voice array
// on a note that finds none free, and its channel array on the first call naming a new channel. So
// both are sized now — a voice pool no preset here comes near, and a channel per SoundFont voice.
// With a fixed pool tsf takes the voice furthest into its release instead of growing.
static void readyForAudio(tsf* h, int sampleRate) {
    tsf_set_output(h, TSF_STEREO_INTERLEAVED, sampleRate, 0.0f);
    tsf_set_max_voices(h, 128);
    tsf_channel_set_pan(h, SF_VOICE_COUNT - 1, 0.5f);   // creates channels 0..8, each at its default
}

tsf* AudioEngine::parseSoundfont(const char* path, int bank, int preset, LoadFailure* failure) {
    *failure = LoadFailure::NONE;

    // ⭐ **THE PREFERRED PATH: cut the one preset out of the file and parse only that.** The trimmed
    // font is a complete, ordinary SoundFont holding this preset, the instruments it reaches and their
    // sample bytes — typically under 1 % of the bank — so tsf runs unmodified over a small buffer and
    // never sees, allocates or decodes the rest. Reading the index costs a few kilobytes.
    //
    // A false answer means "this file cannot be cut apart" (a layout with no per-sample byte ranges,
    // or a preset that is not in it), never "this file is broken" — so it falls through to the whole-
    // bank parse below, which is what the app did before and still does correctly.
    {
        std::vector<uint8_t> trimmed;
        sf_memory_guard_reset();
        if (pt::sf_build_trimmed_font(path, bank, preset, trimmed) &&
            trimmed.size() <= static_cast<size_t>(INT_MAX)) {
            tsf* small = tsf_load_memory(trimmed.data(), static_cast<int>(trimmed.size()));
            if (small) {
                readyForAudio(small, getSampleRate());
                return small;
            }
            // ⚠️ Asked before anything else, for the reason the whole-bank path below states: a cancel
            // and a parse failure unwind through the identical null, and falling through here would
            // answer a cancelled load by starting the very load the user just stopped.
            if (pt::load_cancelled()) {
                LOGD("🎹 Soundfont load cancelled: %s", path);
                *failure = LoadFailure::CANCELLED;
                return nullptr;
            }
            // A trimmed font that will not parse is a bug here, not a property of the file — but the
            // user's sound is worth more than the diagnosis, so fall through and load it whole.
            LOGE("❌ Trimmed soundfont failed to parse, loading whole bank: %s", path);
        }
    }

    // Parse the SF2 into a single master TSF handle. All tracks share it via MIDI channels — no
    // per-track clones, which would cost 8× the file size in RAM and stall the audio callback.
    //
    // `tsf_load` over a `FILE*` rather than `tsf_load_filename`, so the open goes through pt_fopen
    // like every other one. It is the same stream tsf builds for itself in `tsf_load_filename` —
    // sequential reads and forward skips only, so the SF2 still streams and peak RAM is the parsed
    // soundfont, not the file on top of it.
    FILE* sf = pt_fopen(path, "rb");
    if (!sf) {
        LOGE("❌ Cannot open soundfont: %s", path);
        *failure = LoadFailure::PARSE;
        return nullptr;
    }
    tsf_stream sfStream = { sf, &sfStreamRead, &sfStreamSkip };
    // ⭐ The guard that makes a too-large font a MESSAGE instead of a kill. There is no size to check
    // up front — nothing in an SF3 header states its decoded size — so the allocator itself refuses
    // when a block would exhaust the machine, and tsf's own null checks unwind to the failure below.
    // Reset first: the flag is what separates "too big for this device" from "not a soundfont".
    sf_memory_guard_reset();
    tsf* loaded = tsf_load(&sfStream);
    std::fclose(sf);
    if (!loaded) {
        // ⚠️ Asked FIRST, and before the guard: a cancel is not a failure and must not be reported as
        // one. tsf unwinds through the identical null return either way (the abort reuses the decode
        // failure's own cleanup), so the reason is only knowable out here.
        if (pt::load_cancelled()) {
            LOGD("🎹 Soundfont load cancelled: %s", path);
            *failure = LoadFailure::CANCELLED;
        } else if (sf_memory_guard_tripped()) {
            LOGE("❌ Soundfont too large for this device (%lld MB free): %s",
                 (long long)(pt::available_memory_bytes() >> 20), path);
            *failure = LoadFailure::OUT_OF_MEMORY;
        } else {
            LOGE("❌ Failed to parse soundfont: %s", path);
            *failure = LoadFailure::PARSE;
        }
        return nullptr;
    }
    // Configured before publication, for the same reason the trimmed path is: a voice that sees the
    // handle must see it ready. `tsf_set_output` is not a read the audio thread can be racing,
    // because nothing else has the pointer yet.
    readyForAudio(loaded, getSampleRate());
    return loaded;
}

/**
 * Give a parsed handle a slot, evicting the least-recently-used one if every slot is taken.
 *
 * ⚠️ Slot-table work only, and only on the thread that owns it — never across a parse.
 */
int AudioEngine::installSoundfont(tsf* handle, int instrumentId, const char* path, int bank,
                                  int preset) {
    if (!handle) return -1;

    // Find a free slot; if none, evict the genuinely least-recently-used one (smallest use tick), not
    // the smallest instrumentId — that could evict the SoundFont playing right now.
    int slot = -1;
    for (int i = 0; i < MAX_SOUNDFONTS; i++) {
        if (soundfonts[i].handle == nullptr) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        uint64_t oldest = UINT64_MAX;
        slot = 0;
        for (int i = 0; i < MAX_SOUNDFONTS; i++) {
            uint64_t lu = soundfonts[i].lastUsed.load(std::memory_order_relaxed);
            if (lu < oldest) { oldest = lu; slot = i; }
        }
        freeSoundfontSlot(slot);
        LOGD("🎹 Evicted soundfont slot %d to make room for instrumentId %d", slot, instrumentId);
    }

    soundfonts[slot].instrumentId = instrumentId;
    soundfonts[slot].filePath     = path;
    soundfonts[slot].bank         = bank;
    soundfonts[slot].preset       = preset;
    soundfonts[slot].lastUsed.store(nextSfUseTick(), std::memory_order_relaxed);
    soundfonts[slot].handle.store(handle);   // last: the audio thread may use it from here on
    LOGD("🎹 Loaded soundfont slot %d: %s [%d:%d]", slot, path, bank, preset);
    return slot;
}

int AudioEngine::loadSoundfont(int instrumentId, const char* path, int bank, int preset) {
    if (!path) return -1;

    // ⚠️ Only one tsf parse at a time — see parseSoundfont. A synchronous load takes precedence over
    // a background one simply by waiting for it, which is at most one preset's worth of decode.
    waitForSoundfontLoad();

    // De-dup: this exact SOUND already loaded reuses its slot instead of a second copy. Multiple
    // instruments share one handle — they play on distinct MIDI channels (= tracks) and apply their
    // ADSR override per-note in fireArmedNote, so per-instrument state stays isolated. Frees stay
    // reference-guarded (setInstrumentType / clearAllSoundfonts).
    //
    // ⚠️ The bank and preset are part of the key, not just the path: a slot holds ONE preset cut out
    // of the file, so two instruments on the same .sf2 at different sounds must not share one.
    for (int i = 0; i < MAX_SOUNDFONTS; i++) {
        if (soundfonts[i].handle != nullptr && soundfonts[i].filePath == path &&
            soundfonts[i].bank == bank && soundfonts[i].preset == preset) {
            soundfonts[i].lastUsed.store(nextSfUseTick(), std::memory_order_relaxed);
            LOGD("🎹 Reusing soundfont slot %d (de-dup): %s [%d:%d]", i, path, bank, preset);
            return i;
        }
    }

    LoadFailure failure = LoadFailure::NONE;
    tsf* handle = parseSoundfont(path, bank, preset, &failure);
    if (!handle) { lastLoadFailure_ = failure; return -1; }

    const int slot = installSoundfont(handle, instrumentId, path, bank, preset);
    lastLoadFailure_ = LoadFailure::NONE;
    return slot;
}

// ─── the same load, off the drawing thread ──────────────────────────────────────────────────────

AudioEngine::SfRequest AudioEngine::requestSoundfontLoad(int instrumentId, const char* path, int bank,
                                                         int preset, int* readySlot) {
    if (readySlot) *readySlot = -1;
    if (!path) return SfRequest::BUSY;

    // A finished worker still holding its result blocks the next request. The caller polls, so it
    // will collect it and come back — refusing is what keeps "one result waiting at a time" true.
    if (sfLoadBusy.load(std::memory_order_acquire)) return SfRequest::BUSY;

    // The same de-dup the synchronous path does, and for the same reason — but here it also spares a
    // thread: walking back to a preset another instrument still holds costs nothing at all.
    for (int i = 0; i < MAX_SOUNDFONTS; i++) {
        if (soundfonts[i].handle != nullptr && soundfonts[i].filePath == path &&
            soundfonts[i].bank == bank && soundfonts[i].preset == preset) {
            soundfonts[i].lastUsed.store(nextSfUseTick(), std::memory_order_relaxed);
            if (readySlot) *readySlot = i;
            return SfRequest::READY;
        }
    }

    sfLoadInstrument = instrumentId;
    sfLoadPath       = path;
    sfLoadBank       = bank;
    sfLoadPreset     = preset;
    sfLoadHandle     = nullptr;
    sfLoadFailure    = LoadFailure::NONE;
    sfLoadDone.store(false, std::memory_order_relaxed);
    sfLoadBusy.store(true, std::memory_order_release);

    // ⚠️ The worker reads the request fields and writes the result fields, and `sfLoadDone` is the
    // fence between the two halves. Nothing else touches them while `sfLoadBusy` is set.
    sfLoadThread = std::thread([this]() {
        tsf* handle = parseSoundfont(sfLoadPath.c_str(), sfLoadBank, sfLoadPreset, &sfLoadFailure);
        sfLoadHandle = handle;
        sfLoadDone.store(true, std::memory_order_release);
    });
    return SfRequest::STARTED;
}

bool AudioEngine::collectSoundfontLoad(int* instrumentId, int* slot) {
    if (!sfLoadBusy.load(std::memory_order_acquire)) return false;
    if (!sfLoadDone.load(std::memory_order_acquire)) return false;

    if (sfLoadThread.joinable()) sfLoadThread.join();

    tsf* handle = sfLoadHandle;
    sfLoadHandle = nullptr;

    const int landed = handle ? installSoundfont(handle, sfLoadInstrument, sfLoadPath.c_str(),
                                                 sfLoadBank, sfLoadPreset)
                              : -1;
    lastLoadFailure_ = handle ? LoadFailure::NONE : sfLoadFailure;
    if (instrumentId) *instrumentId = sfLoadInstrument;
    if (slot) *slot = landed;

    // Released LAST: it is what lets the next request start, and the result must be fully read out
    // of the members before another one can overwrite them.
    sfLoadBusy.store(false, std::memory_order_release);
    return true;
}

bool AudioEngine::soundfontLoadPending() const {
    return sfLoadBusy.load(std::memory_order_acquire);
}

void AudioEngine::waitForSoundfontLoad() {
    if (!sfLoadBusy.load(std::memory_order_acquire)) return;
    if (sfLoadThread.joinable()) sfLoadThread.join();

    // ⚠️⚠️ **THE RESULT IS KEPT, NOT THROWN AWAY, AND THAT IS THE WHOLE POINT OF WAITING RATHER THAN
    // CANCELLING.** Only the PARSE has to be alone; the answer is still the answer. Discarding it here
    // would lose a load the caller was already told had been accepted, and nothing asks twice — the
    // PATCH row clears its pending flag the moment a request is taken, so the instrument would sit on
    // its old sound for good. `sfLoadBusy` stays set and the next poll collects it as usual.
}

void AudioEngine::discardSoundfontLoad() {
    if (!sfLoadBusy.load(std::memory_order_acquire)) return;
    if (sfLoadThread.joinable()) sfLoadThread.join();

    // ⚠️ Here the answer really is worthless: the project it was asked for is being torn down, so a
    // slot given to it would belong to a document that no longer exists.
    if (sfLoadHandle) {
        tsf_close(sfLoadHandle);
        sfLoadHandle = nullptr;
    }
    sfLoadDone.store(false, std::memory_order_relaxed);
    sfLoadBusy.store(false, std::memory_order_release);
}

bool AudioEngine::soundfontSlotHolds(int slot, const char* path, int bank, int preset) {
    if (slot < 0 || slot >= MAX_SOUNDFONTS || !path) return false;
    return soundfonts[slot].handle.load() != nullptr && soundfonts[slot].filePath == path &&
           soundfonts[slot].bank == bank && soundfonts[slot].preset == preset;
}

int AudioEngine::soundfontSlotCount() const { return MAX_SOUNDFONTS; }

bool AudioEngine::soundfontSlotSound(int slot, std::string& path, int& bank, int& preset) {
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return false;
    if (!soundfonts[slot].handle.load()) return false;
    path   = soundfonts[slot].filePath;
    bank   = soundfonts[slot].bank;
    preset = soundfonts[slot].preset;
    return true;
}

void AudioEngine::unloadSoundfont(int slot) {
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return;
    freeSoundfontSlot(slot);
    LOGD("🎹 Unloaded soundfont slot %d", slot);
}

void AudioEngine::clearAllSoundfonts() {
    // ⚠️ A load still in flight belongs to the project being thrown away. Waited for and discarded,
    // or it would land in a slot the new project has to clear all over again.
    discardSoundfontLoad();

    // Free EVERY slot — called when the project changes (NEW / load). The cache otherwise only
    // reclaims a slot on LRU eviction (one more distinct SF2 than there are slots), so a loaded SF2's
    // samples (its 16-bit file bytes, resident) would stay across NEW/load.
    for (int s = 0; s < MAX_SOUNDFONTS; s++) freeSoundfontSlot(s);
    LOGD("🎹 Cleared all soundfont slots");
}

// ─── the FILE's preset list ─────────────────────────────────────────────────────────────────────
//
// A loaded slot holds one preset, so it can no longer say what else the file contains. These read the
// file's index instead — a few kilobytes even for a 200 MB bank, and no sample data at all, which is
// what makes browsing a font that is far too large to load work exactly like browsing a small one.

int AudioEngine::soundfontFileIndexSlot(const char* path) {
    for (size_t i = 0; i < sfFileIndexCache.size(); ++i) {
        if (sfFileIndexCache[i].path == path) return static_cast<int>(i);
    }
    std::vector<pt::SfPreset> presets;
    if (!pt::sf_read_preset_list(path, presets)) return -1;

    // Four files is more than the PATCH row can be walking at once; the oldest goes, and re-reading it
    // costs one small read.
    if (sfFileIndexCache.size() >= 4) sfFileIndexCache.erase(sfFileIndexCache.begin());
    sfFileIndexCache.push_back({ std::string(path), std::move(presets) });
    return static_cast<int>(sfFileIndexCache.size()) - 1;
}

int AudioEngine::getSoundfontFilePresetCount(const char* path) {
    if (!path) return 0;
    std::lock_guard<std::mutex> lock(sfFileIndexMutex);
    const int i = soundfontFileIndexSlot(path);
    return (i < 0) ? 0 : static_cast<int>(sfFileIndexCache[static_cast<size_t>(i)].presets.size());
}

bool AudioEngine::getSoundfontFilePresetAt(const char* path, int index, int* bank, int* presetNumber) {
    if (!path || index < 0) return false;
    std::lock_guard<std::mutex> lock(sfFileIndexMutex);
    const int i = soundfontFileIndexSlot(path);
    if (i < 0) return false;
    const std::vector<pt::SfPreset>& list = sfFileIndexCache[static_cast<size_t>(i)].presets;
    if (index >= static_cast<int>(list.size())) return false;
    if (bank) *bank = list[static_cast<size_t>(index)].bank;
    if (presetNumber) *presetNumber = list[static_cast<size_t>(index)].preset;
    return true;
}

std::string AudioEngine::getSoundfontFilePresetName(const char* path, int bank, int preset) {
    if (!path) return "---";
    std::lock_guard<std::mutex> lock(sfFileIndexMutex);
    const int i = soundfontFileIndexSlot(path);
    if (i < 0) return "---";
    for (const pt::SfPreset& p : sfFileIndexCache[static_cast<size_t>(i)].presets) {
        if (p.bank == bank && p.preset == preset) return p.name.empty() ? std::string("---") : p.name;
    }
    return "---";
}

void AudioEngine::scheduleKill(int64_t targetFrame, int trackId) {
    ScheduledKill kill{};
    kill.targetFrame = targetFrame;
    kill.trackId     = trackId;
    killQueue.schedule(kill);
}

void AudioEngine::scheduleNoteOff(int64_t targetFrame, int trackId) {
    ScheduledKill kill{};
    kill.targetFrame = targetFrame;
    kill.trackId     = trackId;
    kill.mode        = KILL_SOFT;
    killQueue.schedule(kill);
}

void AudioEngine::scheduleKeyRelease(int64_t targetFrame, int trackId) {
    ScheduledKill kill{};
    kill.targetFrame = targetFrame;
    kill.trackId     = trackId;
    kill.mode        = KILL_KEY_OFF;
    killQueue.schedule(kill);
}

void AudioEngine::scheduleCut(int64_t targetFrame, int trackId) {
    ScheduledKill kill{};
    kill.targetFrame = targetFrame;
    kill.trackId     = trackId;
    kill.mode        = KILL_CUT;
    killQueue.schedule(kill);
}

void AudioEngine::scheduleNoteOffAll(int64_t targetFrame) {
    // Every lane, the preview one included — a render owns the whole engine, and a lane with nothing
    // sounding on it costs one queue entry that finds no voice.
    for (int t = 0; t < SF_VOICE_COUNT; ++t) scheduleNoteOff(targetFrame, t);
}

void AudioEngine::clearScheduledNotes() {
    noteQueue.clear();
    killQueue.clear();
    paramUpdateQueue.clear();
}

void AudioEngine::clearScheduledNotesFrom(int64_t fromFrame, int trackId) {
    noteQueue.clearFrom(fromFrame, trackId);
    killQueue.clearFrom(fromFrame, trackId);
    paramUpdateQueue.clearFrom(fromFrame, trackId);
}

void AudioEngine::loadTable(int tableId, const uint8_t* rowData) {
    if (tableId < 0 || tableId >= 256) return;

    TableRow rows[16];
    for (int row = 0; row < 16; row++) {
        int offset = row * 8;
        rows[row].transpose = (int8_t)rowData[offset + 0];
        rows[row].volume = rowData[offset + 1];
        rows[row].fx1Type = rowData[offset + 2];
        rows[row].fx1Value = rowData[offset + 3];
        rows[row].fx2Type = rowData[offset + 4];
        rows[row].fx2Value = rowData[offset + 5];
        rows[row].fx3Type = rowData[offset + 6];
        rows[row].fx3Value = rowData[offset + 7];
    }
    std::lock_guard<std::mutex> lock(tableWriteMutex);   // writers only; the audio thread never takes it
    tables.write(tableId, rows);

    LOGD("📋 Loaded table %d", tableId);
}

// The TABLE screen's playing-row indicator (ui/engine_feed.h) reads these two, at 60 Hz.
//
// ⚠️ They answer "where is this track's table", NOT "is a voice sounding" — the two diverge, and the
// indicator is the thing that shows it. A retrigger leaves the OLD voice fading beside the new one for
// the length of its declick, so a plain first-active-slot scan can report the previous note's row; and
// a one-shot that ends before the next note leaves no voice at all, which read as "no table running"
// and blanked the indicator for the rest of the note. Both are set by the instrument's ROOT note —
// root sets playback rate, rate sets how long the sample lasts — so the same table on the same phrase
// stepped smoothly at one root and skipped and stalled at another.
//
// Order: the live voice, then the SF voice, then the track cursor (the TIC00 table outlives its
// voices), and a fading voice only as a last resort — it goes on ticking its own table after the note
// that replaced it has moved on, so it is the stalest source here, not the freshest.
static int findTrackVoice(Voice* voices, int trackId, bool fading) {
    for (int v = 0; v < MAX_VOICES; v++)
        if (voices[v].isActive && voices[v].isFadingOut == fading && voices[v].trackId == trackId) return v;
    return -1;
}

// ⚠️⚠️ **THE ROW IN FORCE IS `lastProcessed`, NOT `row`.** The lane's cursor has already been moved
// on — advanced, or HOPped — to the row that comes NEXT, so drawing it puts the marker a row ahead of
// what is being heard, and parks it on rows that never sound: a `HOP` row is resolved without ever
// playing, so `row` rests on it while the row it jumped to is the one making the sound. `row` is the
// honest answer only before the lane has consumed anything, where it is where the lane will start.
// Same pairing the AUS/AUF ramp reads, and for the same reason.
static inline int laneMarker(int row, int lastProcessed) {
    return lastProcessed >= 0 ? lastProcessed : row;
}

// ⚠️ A column that has executed `HOP FF` reads −1, the same "no position" the whole call answers with
// — the marker for that column disappears while its neighbours keep moving, which is the only honest
// drawing of a table with one column stopped.
static void lanesOf(const TableLane (&lanes)[TABLE_LANES], int out[TABLE_LANES]) {
    for (int l = 0; l < TABLE_LANES; ++l)
        out[l] = lanes[l].active ? laneMarker(lanes[l].row, lanes[l].lastProcessed) : -1;
}

// Audio thread, end of every block: what the voice getters below answer from.
void AudioEngine::publishVoiceView() {
    VoiceView v;
    for (int i = 0; i < MAX_VOICES; i++) {
        const Voice& src = voices[i];
        VoiceView::Sampler& d = v.sampler[i];
        d.active    = src.isActive;
        d.fading    = src.isFadingOut;
        d.trackId   = src.trackId;
        d.instrId   = src.instrId;
        d.tableId   = src.tableId;
        d.note      = src.noteOctave * 12 + src.notePitch;
        d.loopStart = src.actualLoopStart;
        d.loopEnd   = src.actualLoopEnd;
        d.sampleGen = src.sampleGen;
        d.position  = src.position;
        lanesOf(src.lanes, d.lanes);
    }
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        const SoundfontVoice& src = sfVoices[t];
        v.sf[t].active  = src.isActive;
        v.sf[t].tableId = src.tableId;
        v.sf[t].note    = src.noteOctave * 12 + src.notePitch;
        lanesOf(src.lanes, v.sf[t].lanes);
        v.tic00Sounding[t].tableId = tic00Sounding[t];
        for (int s = 0; s < TIC00_SLOTS; ++s) {
            const Tic00Cursor& c = tic00Cursor[t][s];
            v.tic00[t][s].tableId = c.tableId;
            for (int l = 0; l < TABLE_LANES; ++l)
                v.tic00[t][s].lanes[l] = c.active[l] ? laneMarker(c.row[l], c.lastProcessed[l]) : -1;
        }
    }
    voiceViewPublisher.publish(v);
}

static void copyLanes(const int (&from)[TABLE_LANES], int out[TABLE_LANES]) {
    for (int l = 0; l < TABLE_LANES; ++l) out[l] = from[l];
}

void AudioEngine::getVoiceTableRows(int trackId, int out[TABLE_LANES]) {
    for (int l = 0; l < TABLE_LANES; ++l) out[l] = -1;
    const VoiceView& v = voiceView();

    const int live = v.trackVoice(trackId, /*fadingOne=*/false);
    if (live >= 0) { copyLanes(v.sampler[live].lanes, out); return; }

    if (trackId >= 0 && trackId < SF_VOICE_COUNT) {
        const VoiceView::Sf& sv = v.sf[trackId];
        if (sv.active && sv.tableId >= 0) { copyLanes(sv.lanes, out); return; }
        // The table the voice last RAN — a chain's routers have bookmarks too, and the screen is
        // showing the one that made the sound.
        if (const VoiceView::Bookmark* b = v.bookmark(trackId, v.tic00Sounding[trackId].tableId)) {
            copyLanes(b->lanes, out);
            return;
        }
    }
    const int fading = v.trackVoice(trackId, /*fadingOne=*/true);
    if (fading >= 0) copyLanes(v.sampler[fading].lanes, out);
}

// Same precedence as getVoiceTableRows — live voice, SF voice, the table's bookmark, a fading voice
// last — but every source has to be in the table that was ASKED for.
bool AudioEngine::getTableRowsFor(int trackId, int tableId, int out[TABLE_LANES]) {
    for (int l = 0; l < TABLE_LANES; ++l) out[l] = -1;
    if (tableId < 0) return false;
    const VoiceView& v = voiceView();

    const int live = v.trackVoice(trackId, /*fadingOne=*/false);
    if (live >= 0 && v.sampler[live].tableId == tableId) { copyLanes(v.sampler[live].lanes, out); return true; }

    if (trackId >= 0 && trackId < SF_VOICE_COUNT) {
        const VoiceView::Sf& sv = v.sf[trackId];
        if (sv.active && sv.tableId == tableId) { copyLanes(sv.lanes, out); return true; }

        // No voice is carrying it: a TIC00 table between notes, or one a hit only ROUTED through,
        // which is the same place kept the same way.
        if (const VoiceView::Bookmark* b = v.bookmark(trackId, tableId)) {
            copyLanes(b->lanes, out);
            return true;
        }
    }

    const int fading = v.trackVoice(trackId, /*fadingOne=*/true);
    if (fading >= 0 && v.sampler[fading].tableId == tableId) { copyLanes(v.sampler[fading].lanes, out); return true; }
    return false;
}

bool AudioEngine::getVoiceLoopWindow(int trackId, int* startFrame, int* endFrame) {
    const VoiceView& v = voiceView();
    const int live = v.trackVoice(trackId, /*fadingOne=*/false);
    if (live < 0) return false;
    if (startFrame) *startFrame = v.sampler[live].loopStart;
    if (endFrame)   *endFrame   = v.sampler[live].loopEnd;
    return true;
}

int AudioEngine::getVoiceTableId(int trackId) {
    const VoiceView& v = voiceView();
    const int live = v.trackVoice(trackId, /*fadingOne=*/false);
    if (live >= 0) return v.sampler[live].tableId;

    if (trackId >= 0 && trackId < SF_VOICE_COUNT) {
        if (v.sf[trackId].active) return v.sf[trackId].tableId;
        if (v.tic00Sounding[trackId].tableId >= 0) return v.tic00Sounding[trackId].tableId;
    }
    const int fading = v.trackVoice(trackId, /*fadingOne=*/true);
    return fading >= 0 ? v.sampler[fading].tableId : -1;
}

void AudioEngine::scheduleVoiceTableRow(int64_t targetFrame, int trackId, int row) {
    // Enqueue; the audio thread applies it to voices[] in the drain loop (see processAudioBlock).
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, (float)row, PARAM_UPDATE_TABLE_ROW, 0.0f });
}

void AudioEngine::scheduleTrackPhraseVol(int64_t targetFrame, int trackId, float phraseVol) {
    paramUpdateQueue.schedule({ targetFrame, trackId, (int)MOD_SRC_PHRASE_VOL, phraseVol });
}

// ── Live per-note / mixer FX — all enqueue onto the same sample-accurate paramUpdateQueue,
// so the voices[] / masterEq mutation happens on the audio thread at the exact step frame (no race),
// and they replay identically during offline render (renderOffline drains the same queue). ──────────

void AudioEngine::scheduleVoicePan(int64_t targetFrame, int trackId, float pan) {                 // PAN
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, pan, PARAM_UPDATE_PAN, 0.0f });
}

void AudioEngine::scheduleVoiceReverbSend(int64_t targetFrame, int trackId, float send) {          // REV
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, send, PARAM_UPDATE_REVERB_SEND, 0.0f });
}

void AudioEngine::scheduleVoiceDelaySend(int64_t targetFrame, int trackId, float send) {           // DEL
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, send, PARAM_UPDATE_DELAY_SEND, 0.0f });
}

void AudioEngine::scheduleVoiceReverse(int64_t targetFrame, int trackId, bool reverse, bool restart) {  // BCK
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, reverse ? 1.0f : 0.0f,
                                PARAM_UPDATE_REVERSE, restart ? 1.0f : 0.0f });
}

void AudioEngine::scheduleVoiceFilterCut(int64_t targetFrame, int trackId, float cut) {            // CUT
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, cut, PARAM_UPDATE_FILTER_CUT, 0.0f });
}

void AudioEngine::scheduleVoiceFilterRes(int64_t targetFrame, int trackId, float res) {            // RES
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, res, PARAM_UPDATE_FILTER_RES, 0.0f });
}

void AudioEngine::scheduleVoiceFilterMode(int64_t targetFrame, int trackId,                        // LPF/HPF/BPF
                                          int type, float cut) {
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, cut, PARAM_UPDATE_FILTER_MODE, (float)type });
}

void AudioEngine::scheduleVoiceDrive(int64_t targetFrame, int trackId, float drive) {              // DRV
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, drive, PARAM_UPDATE_DRIVE, 0.0f });
}

void AudioEngine::scheduleVoiceCrush(int64_t targetFrame, int trackId, float packed) {             // CRU
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, packed, PARAM_UPDATE_CRUSH, 0.0f });
}

void AudioEngine::scheduleVoiceFineTune(int64_t targetFrame, int trackId, float fine) {            // FIN
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, fine, PARAM_UPDATE_FINE_TUNE, 0.0f });
}

void AudioEngine::scheduleVoiceLoopSlide(int64_t targetFrame, int trackId, float step) {           // LPO
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, step, PARAM_UPDATE_LOOP_SLIDE, 0.0f });
}

void AudioEngine::scheduleVoiceEqSlot(int64_t targetFrame, int trackId, int slot) {                // EQN
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, (float)slot, PARAM_UPDATE_EQ_SLOT, 0.0f });
}

void AudioEngine::scheduleMasterEqSlot(int64_t targetFrame, int slot) {                            // EQM
    paramUpdateQueue.schedule({ targetFrame, -1, 0, (float)slot, PARAM_UPDATE_MASTER_EQ, 0.0f });
}

void AudioEngine::scheduleVoiceEqBands(int64_t targetFrame, int trackId, const EqBandsHex& bands) {
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, 0.0f, PARAM_UPDATE_EQ_BANDS, 0.0f, bands });
}

void AudioEngine::scheduleMasterEqBands(int64_t targetFrame, const EqBandsHex& bands) {
    paramUpdateQueue.schedule({ targetFrame, -1, 0, 0.0f, PARAM_UPDATE_MASTER_EQ_BANDS, 0.0f, bands });
}

// Convert one band from authored hex to the Hz/dB/Q the filters run on.
// ⚠️ The three curves are setEqBand's, and there must not be a second copy of them: a morph that
// converted its frequency even slightly differently would land somewhere the preset it names does
// not, and only at the ends of a fade — the hardest place to hear it and the easiest to blame on the
// curve. Anything that changes there changes here, in the same commit.
static EqBandData eqBandFromHex(int type, int freqHex, int gainHex, int qHex) {
    EqBandData b;
    b.type   = type;
    b.freqHz = 20.0f * powf(1000.0f, freqHex / 255.0f);
    b.gainDb = gainHex / 10.0f - 12.0f;
    b.q      = 0.1f  * powf(100.0f,  qHex   / 255.0f);
    return b;
}

// Apply a morph tick's bands to an EQ (a live voice's inline EQ, or the master chain's). Mirrors
// applyEqPresetToModule, but the bands arrive as values rather than as a slot to look up.
void AudioEngine::applyEqBandsToModule(EqModule& eq, const EqBandsHex& bands) {
    bool any = false;
    for (int i = 0; i < 3; i++) {
        EqBandData d = eqBandFromHex(bands.type[i], bands.freq[i], bands.gain[i], bands.q[i]);
        eq.bands[i].setParams(d.type, d.freqHz, d.gainDb, d.q);
        if (d.type != 0) any = true;
    }
    eq.active = any;
}

// Apply a global EQ preset (slot 0-127, <0 = bypass) to any EQ: a live voice's inline EQ (EQN), the
// master bus (EQM), or either send's input EQ. The target was already sp_pareq_init'd by its owner's
// reset, so only the band params are re-set here.
//
// `active` follows "some band is not type 0" rather than "the slot index is in range". The two are
// audibly identical — a type-0 band sets its own `bypass` and returns the input untouched
// (eq-module.h) — but the derived form skips three bypassed bands per sample on an all-off preset.
void AudioEngine::applyEqPresetToModule(EqModule& eq, int slot) {
    if (slot < 0 || slot >= 128) { eq.active = false; return; }
    const EqPresetBank& preset = eqPresets[slot];
    bool any = false;
    for (int i = 0; i < 3; i++) {
        eq.bands[i].setParams(preset.bands[i].type, preset.bands[i].freqHz,
                              preset.bands[i].gainDb, preset.bands[i].q);
        if (preset.bands[i].type != 0) any = true;
    }
    eq.active = any;
}

static const int SPECTRUM_FFT_SIZE = 2048;

// Shared FFT helper — takes FFT_SIZE samples already copied from the circular buffer by the caller
// (under mutex), applies Hann window + FFT, maps to numBins log-spaced magnitude values [0,1].
// ⚠️ The two tables below are FUNCTION-LOCAL STATICS WITH NO LOCK, on the same terms as the FFT
// config: every caller is the single UI poll thread. A second calling thread would need more than a
// mutex here — it would need a slot of its own, because the bin map is chosen per call.
//
// Neither table depends on the audio, and recomputing them per call cost more than the transform:
// measured at 2048 points / 620 bins, 14 us of window and 25 us of bin mapping against 17 us of FFT.
// Caching both halves the cost of a poll, which is what lets the EQ panel run at frame rate for less
// than it used to cost at 20 Hz.

static const float* spectrum_hann_window() {
    static float w[SPECTRUM_FFT_SIZE];
    static const bool built = [] {
        for (int i = 0; i < SPECTRUM_FFT_SIZE; i++)
            w[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (SPECTRUM_FFT_SIZE - 1)));
        return true;
    }();
    (void)built;
    return w;
}

// Which FFT bin each output bin reads. Depends only on (numBins, sampleRate), and the live consumers
// ask for different counts — the EQ panel and the visualizer strip — so a single slot would thrash
// between them and rebuild on every call. Four slots, replaced round-robin on a miss.
static const int* spectrum_bin_map(int numBins, float sampleRate) {
    struct Entry {
        int              numBins    = 0;
        float            sampleRate = 0.0f;
        std::vector<int> idx;
    };
    static Entry cache[4];
    static int   next = 0;

    for (Entry& e : cache)
        if (e.numBins == numBins && e.sampleRate == sampleRate) return e.idx.data();

    Entry& e = cache[next];
    next = (next + 1) % 4;
    e.numBins    = numBins;
    e.sampleRate = sampleRate;
    e.idx.resize((size_t)numBins);

    const float fMin = 20.0f, fMax = 20000.0f;
    const float logRange = logf(fMax / fMin);
    const float denom    = (numBins > 1) ? (float)(numBins - 1) : 1.0f;
    for (int bi = 0; bi < numBins; bi++) {
        float t    = (float)bi / denom;
        float freq = fMin * expf(t * logRange);
        int bin    = (int)(freq * SPECTRUM_FFT_SIZE / sampleRate + 0.5f);
        if (bin < 1)                      bin = 1;
        if (bin >= SPECTRUM_FFT_SIZE / 2) bin = SPECTRUM_FFT_SIZE / 2 - 1;
        e.idx[(size_t)bi] = bin;
    }
    return e.idx.data();
}

static void computeSpectrumFFT(kiss_fft_scalar* input, int numBins, float* out, float sampleRate) {
    const float* window = spectrum_hann_window();
    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) input[i] *= window[i];

    // Cache the config across calls: kiss_fftr_alloc does a malloc + twiddle-table trig init
    // every time. FFT size is constant, so the cfg lives for the process (never freed).
    static kiss_fftr_cfg cfg = kiss_fftr_alloc(SPECTRUM_FFT_SIZE, 0, nullptr, nullptr);
    kiss_fft_cpx cpx_out[SPECTRUM_FFT_SIZE / 2 + 1];
    kiss_fftr(cfg, input, cpx_out);

    const int* binOf = spectrum_bin_map(numBins, sampleRate);
    for (int bi = 0; bi < numBins; bi++) {
        const int bin = binOf[bi];

        float re  = cpx_out[bin].r;
        float im  = cpx_out[bin].i;
        float mag = sqrtf(re*re + im*im) / (SPECTRUM_FFT_SIZE * 0.5f);

        float db         = 20.0f * log10f(mag + 1e-9f);
        float normalized = (db + 80.0f) / 80.0f;
        out[bi] = fmaxf(0.0f, fminf(1.0f, normalized));
    }
}

// Read SPECTRUM_FFT_SIZE contiguous samples from a circular buffer of size bufSize.
static void readCircularBuffer(const float* buf, int writeIdx, int bufSize, kiss_fft_scalar* input) {
    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) {
        int idx = (writeIdx - SPECTRUM_FFT_SIZE + i + bufSize) % bufSize;
        input[i] = buf[idx];
    }
}

void AudioEngine::getSpectrumMagnitudes(int numBins, float* out) {
    lastSpectrumReadMs.store(nowMs(), std::memory_order_relaxed);  // demand signal for 1.10 capture gate
    kiss_fft_scalar input[SPECTRUM_FFT_SIZE];
    {
        std::lock_guard<std::mutex> lock(spectrumMutex);
        readCircularBuffer(spectrumBuffer, spectrumWriteIdx, SPECTRUM_SIZE, input);
    }
    computeSpectrumFFT(input, numBins, out, (float)getSampleRate());
}

void AudioEngine::getSpectrumMagnitudesForSource(int source, int instrId, int numBins, float* out) {
    lastSpectrumReadMs.store(nowMs(), std::memory_order_relaxed);  // demand signal for 1.10 capture gate
    if (source == 3) instrSpectrumInstrId.store(instrId, std::memory_order_relaxed);

    kiss_fft_scalar input[SPECTRUM_FFT_SIZE];
    {
        std::lock_guard<std::mutex> lock(spectrumMutex);
        switch (source) {
            case 1:  readCircularBuffer(delaySpectrumBuffer,  delaySpectrumWriteIdx,  SPECTRUM_SIZE, input); break;
            case 2:  readCircularBuffer(reverbSpectrumBuffer, reverbSpectrumWriteIdx, SPECTRUM_SIZE, input); break;
            case 3:  readCircularBuffer(instrSpectrumBuffer,  instrSpectrumWriteIdx,  SPECTRUM_SIZE, input); break;
            default: readCircularBuffer(spectrumBuffer,       spectrumWriteIdx,       SPECTRUM_SIZE, input); break;
        }
    }
    computeSpectrumFFT(input, numBins, out, (float)getSampleRate());
}

void AudioEngine::getWaveform(float* outBuffer, int bufferSize) {
    std::lock_guard<std::mutex> lock(waveformMutex);
    for (int i = 0; i < bufferSize && i < WAVEFORM_SIZE; i++) {
        int readIndex = (waveformIndex + i) % WAVEFORM_SIZE;
        outBuffer[i] = waveformBuffer[readIndex];
    }
}

void AudioEngine::getTrackPeaks(float* outBuffer) {
    std::lock_guard<std::mutex> lock(peakMutex);
    for (int i = 0; i < 8; i++) {
        outBuffer[i * 2]     = trackPeaksL[i];
        outBuffer[i * 2 + 1] = trackPeaksR[i];
    }
}

void AudioEngine::getMasterPeaks(float* outBuffer) {
    std::lock_guard<std::mutex> lock(peakMutex);
    outBuffer[0] = masterPeakL;
    outBuffer[1] = masterPeakR;
}

void AudioEngine::getSendPeaks(float* outBuffer) {
    std::lock_guard<std::mutex> lock(peakMutex);
    outBuffer[0] = sendPeakRevL;
    outBuffer[1] = sendPeakRevR;
    outBuffer[2] = sendPeakDelL;
    outBuffer[3] = sendPeakDelR;
}

void AudioEngine::decayPeaks() {
    std::lock_guard<std::mutex> lock(peakMutex);
    const float MANUAL_DECAY = 0.92f;

    for (int t = 0; t < 8; t++) {
        trackPeaksL[t] *= MANUAL_DECAY;
        trackPeaksR[t] *= MANUAL_DECAY;
        if (trackPeaksL[t] < 0.001f) trackPeaksL[t] = 0.0f;
        if (trackPeaksR[t] < 0.001f) trackPeaksR[t] = 0.0f;
    }
    masterPeakL *= MANUAL_DECAY;
    masterPeakR *= MANUAL_DECAY;
    if (masterPeakL < 0.001f) masterPeakL = 0.0f;
    if (masterPeakR < 0.001f) masterPeakR = 0.0f;
    sendPeakRevL *= MANUAL_DECAY; sendPeakRevR *= MANUAL_DECAY;
    sendPeakDelL *= MANUAL_DECAY; sendPeakDelR *= MANUAL_DECAY;
    if (sendPeakRevL < 0.001f) sendPeakRevL = 0.0f;
    if (sendPeakRevR < 0.001f) sendPeakRevR = 0.0f;
    if (sendPeakDelL < 0.001f) sendPeakDelL = 0.0f;
    if (sendPeakDelR < 0.001f) sendPeakDelR = 0.0f;
}

void AudioEngine::decayWaveform() {
    std::lock_guard<std::mutex> lock(waveformMutex);
    const float WAVEFORM_DECAY = 0.90f;

    for (int i = 0; i < WAVEFORM_SIZE; i++) {
        waveformBuffer[i] *= WAVEFORM_DECAY;
        if (fabsf(waveformBuffer[i]) < 0.001f) waveformBuffer[i] = 0.0f;
    }
    for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) {
        for (int i = 0; i < WAVEFORM_SIZE; i++) {
            trackWaveformBuffer[t][i] *= WAVEFORM_DECAY;
            if (fabsf(trackWaveformBuffer[t][i]) < 0.001f) trackWaveformBuffer[t][i] = 0.0f;
        }
    }
}

void AudioEngine::getTrackWaveforms(float* outBuffer, bool* activeFlags) {
    lastTrackWaveformReadMs.store(nowMs(), std::memory_order_relaxed);  // demand signal for 1.2 OCTA gate
    std::lock_guard<std::mutex> lock(waveformMutex);
    for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) {
        activeFlags[t] = trackHasVoice[t];
        for (int i = 0; i < WAVEFORM_SIZE; i++) {
            int readIdx = (trackWaveformIndex + i) % WAVEFORM_SIZE;
            outBuffer[t * WAVEFORM_SIZE + i] = trackWaveformBuffer[t][readIdx];
        }
    }
}

// ⚠️ THE TWO apply* HELPERS EXIST BECAUSE VTR/VMV REACH THE SAME FADERS FROM THE AUDIO THREAD, and
// the difference that forces the split is the LOGD below them: `processAudioBlock` contains no log
// call at all (audio-defs.h states it as an invariant), and a ramp emitting one CC per tick would put
// an fprintf on the audio thread a hundred times a second whenever POCKETTRACKER_LOG is set. So the
// setters are the helpers plus a log line, and the queue arms call the helpers directly.
//
// ⚠️ SETTING THE VALUE IS ALL THERE IS TO DO — both mix paths re-read it every block and ramp to it,
// so nothing has to be poked into a voice or a TSF channel from here.
void AudioEngine::applyTrackVolume(int trackId, float volume) {
    if (trackId < 0 || trackId >= 8) return;
    trackVolumes[trackId].store(volume, std::memory_order_relaxed);
}

void AudioEngine::applyMasterVolume(float volume) {
    masterVolume.store(volume, std::memory_order_relaxed);
}

void AudioEngine::setTrackVolume(int trackId, float volume) {
    if (trackId < 0 || trackId >= 8) return;
    applyTrackVolume(trackId, volume);
    LOGD("🔊 Track %d volume set to %.2f", trackId, volume);
}

void AudioEngine::setTrackMuted(int trackId, bool muted) {
    if (trackId < 0 || trackId >= 8) return;
    trackMuted[trackId].store(muted, std::memory_order_relaxed);
    // Nothing else is needed to silence what is ringing: the next block picks the new target up and
    // both mix paths walk their gate to it over MUTE_GATE_SAMPLES, so a mute lands in ~5.8 ms rather
    // than in one sample. Voices keep running underneath — a mute is a gate, never a stop.
    LOGD("🔇 Track %d %s", trackId, muted ? "muted" : "unmuted");
}

void AudioEngine::setBusMutes(bool revMuted, bool dlyMuted, bool dryBusMuted) {
    revReturnMuted.store(revMuted, std::memory_order_relaxed);
    delayReturnMuted.store(dlyMuted, std::memory_order_relaxed);
    dryMuted.store(dryBusMuted, std::memory_order_relaxed);
}

void AudioEngine::setMasterVolume(float volume) {
    applyMasterVolume(volume);
    LOGD("🔊 Master volume set to %.2f", volume);
}

void AudioEngine::setPreviewTrack(int trackId) {
    previewLaneTrack.store((trackId >= 0 && trackId < 8) ? trackId : -1, std::memory_order_relaxed);
}

// The sample-accurate faces of the same two faders — what a VTR / VMV effect schedules. VMV is global
// and carries no track, so it borrows the queue's `trackId` field as -1 the way EQM does.
void AudioEngine::scheduleTrackVolume(int64_t targetFrame, int trackId, float volume) {
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, volume, PARAM_UPDATE_TRACK_VOL, 0.0f });
}

void AudioEngine::scheduleMasterVolume(int64_t targetFrame, float volume) {
    paramUpdateQueue.schedule({ targetFrame, -1, 0, volume, PARAM_UPDATE_MASTER_VOL, 0.0f });
}

// TIM. Global like VMV above, and carries no track for the same reason.
void AudioEngine::scheduleDelayTime(int64_t targetFrame, float time) {
    paramUpdateQueue.schedule({ targetFrame, -1, 0, time, PARAM_UPDATE_DELAY_TIME, 0.0f });
}

void AudioEngine::refreshSoundingInstrument(int instrumentId) {
    if (instrumentId < 0 || instrumentId >= 256) return;
    // ⚠️ NAMED RATHER THAN POSITIONAL: this is the one record that carries the instrument, and the
    // field sits past `eqBands` at the end of the struct where nothing else initialises.
    ScheduledParamUpdate update{};
    update.targetFrame = globalFrameCounter.load(std::memory_order_relaxed);
    update.trackId     = -1;
    update.action      = PARAM_UPDATE_INSTRUMENT;
    update.instrId     = instrumentId;
    paramUpdateQueue.schedule(update);
}

void AudioEngine::setOttDepth(int depth) {
    busSettings.ottDepth = depth; busSettings.ottForRender = false;
    recordBus(BUS_OTT);
}

void AudioEngine::setOttDepthForRender(int depth) {
    busSettings.ottDepth = depth; busSettings.ottForRender = true;
    recordBus(BUS_OTT);
}

void AudioEngine::setMasterFx(int fx) {
    busSettings.masterFx = fx;
    recordBus(BUS_MASTER_FX);
}

void AudioEngine::setDustDepth(int depth) {
    busSettings.dustDepth = depth; busSettings.dustForRender = false;
    recordBus(BUS_DUST);
}

void AudioEngine::setDustDepthForRender(int depth) {
    busSettings.dustDepth = depth; busSettings.dustForRender = true;
    recordBus(BUS_DUST);
}

void AudioEngine::setLimiterPreGain(int depth) {
    busSettings.limiterPreGain = depth;
    recordBus(BUS_LIMITER);
}

IAudioVoice* AudioEngine::findActiveVoiceForTrack(int trackId) {
    // Returns the track's CURRENT note, for mid-note param updates (PBN/PVB/PAN). A releasing
    // SF voice or a fading (stolen) sampler voice is the previous note's tail, never the
    // target: without the isReleasingOnly skip, one SF note left the track's SF voice
    // permanently preferred here (nothing note-offs a naturally-decayed SF note, so it stays
    // isActive) and PBN/PVB on every later sampler note went nowhere.
    if (trackId >= 0 && trackId < SF_VOICE_COUNT &&
        sfVoices[trackId].isActive && !sfVoices[trackId].isReleasingOnly) {
        return &sfVoices[trackId];
    }
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == trackId) {
            return &voices[v];
        }
    }
    return nullptr;
}

void AudioEngine::schedulePitchBend(int64_t targetFrame, int trackId, float semitonesPerStep, int tempo) {
    // Convert the per-step bend rate to per-frame here (sample-rate/tempo known on this thread),
    // then enqueue the raw rate; the audio thread applies it to the active voice. 0 = stop.
    float ratePerFrame = 0.0f;
    if (fabsf(semitonesPerStep) >= 0.0001f) {
        float sr = (float)getSampleRate();
        float framesPerStep = sr / (tempo / 60.0f * 4.0f * 12.0f) * 12.0f;
        ratePerFrame = semitonesPerStep / framesPerStep;
    }
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, ratePerFrame, PARAM_UPDATE_PITCH_BEND, 0.0f });
}

void AudioEngine::scheduleVibrato(int64_t targetFrame, int trackId, float speed, float depth) {
    // Enqueue speed+depth; the audio thread applies setVibratoRaw in the drain loop. depth=0 stops.
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, speed, PARAM_UPDATE_VIBRATO, depth });
}

void AudioEngine::setInstrumentModulation(int sampleId, int slotIndex,
                                          int type, int dest, float amount,
                                          int attackSamples, int holdSamples, int decaySamples,
                                          float sustainLevel, float lfoHz, int oscShape,
                                          int releaseSamples, int lfoTrigMode) {
    if (sampleId < 0 || sampleId >= 256 || slotIndex < 0 || slotIndex >= 4) return;
    InstrumentModSlot& slot = instrumentModSlots.edit(sampleId)[slotIndex];
    slot.type = type;
    slot.dest = dest;
    slot.amount = amount;
    slot.attackSamples = attackSamples;
    slot.holdSamples = holdSamples;
    slot.decaySamples = decaySamples;
    slot.sustainLevel = sustainLevel;
    slot.lfoHz = lfoHz;
    slot.oscShape = oscShape;
    slot.lfoTrigMode = lfoTrigMode;
    slot.releaseSamples = releaseSamples;
    instrumentModSlots.publish(sampleId);
}

void AudioEngine::initVoiceModSlots(IAudioVoice& voice, int sampleId, int64_t currentFrame, float sampleRate) {
    for (int m = 0; m < 4; m++) {
        const InstrumentModSlot& src = instrumentModSlots[sampleId][m];
        VoiceModSlot& dst = voice.voiceMods[m];
        dst.type = src.type;
        dst.dest = src.dest;
        dst.amount = src.amount;
        dst.attackSamples = src.attackSamples;
        dst.holdSamples = src.holdSamples;
        dst.decaySamples = src.decaySamples;
        dst.sustainLevel = src.sustainLevel;
        dst.lfoHz = src.lfoHz;
        dst.oscShape = src.oscShape;
        dst.lfoTrigMode = src.lfoTrigMode;
        dst.releaseSamples = src.releaseSamples;
        dst.effectiveAmt = src.amount;
        dst.effectiveRateMult = 1.0f;
        dst.prevEnvValue = 0.0f;
        dst.stage = (src.type != 0) ? 1 : 0;
        dst.envValue = 0.0f;
        dst.stageCounter = 0;

        // Per-slot RNG for the stateful RND/DRNK LFO shapes: RND holds a random level from
        // note-on, DRNK walks from it. Seed varies per note (frame), per slot, and per
        // session/render (noteSeedEntropy — frame-only seeds made renders bit-identical).
        dst.lfoRngState  = (((uint32_t)(uint64_t)currentFrame) * 747796405u
                            ^ (uint32_t)(m + 1) * 2891336453u
                            ^ noteSeedEntropy) | 1u;
        dst.lfoRandValue = (src.type == 3 && src.oscShape >= 8)
                           ? xorshift32Bipolar(dst.lfoRngState) : 0.0f;

        // LFO trigger mode: RETG/ONCE restart the cycle at note-on; FREE and HOLD align the
        // phase to the global frame clock (a stateless free-running LFO). HOLD additionally
        // freezes the clock-aligned value for the note's lifetime (tickLFO never advances it).
        dst.lfoPhase = 0.0f;
        if (src.type == 3 && (src.lfoTrigMode == 0 || src.lfoTrigMode == 2)) {
            double cycles = (double)currentFrame * (double)src.lfoHz / (double)sampleRate;
            dst.lfoPhase = (float)(fmod(cycles, 1.0) * 2.0 * M_PI);
            if (src.lfoTrigMode == 2) {
                dst.envValue = (src.oscShape >= 8) ? dst.lfoRandValue
                                                   : lfoShape(dst.lfoPhase, src.oscShape);
            }
        }
    }
}

void AudioEngine::triggerNoteOff(int trackId, int atFrame) {
    // The release-vs-fade decision lives in Voice::noteOff — one implementation.
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].trackId == trackId) voices[v].noteOffAt(atFrame);
    }
}

void AudioEngine::triggerKeyRelease(int trackId, int atFrame) {
    // …and the key-release decision lives in Voice::keyRelease, for the same reason: this loop is
    // allocation, not policy. The two differ in exactly one arm (see sampler-voice.h).
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].trackId == trackId) voices[v].keyRelease(atFrame);
    }
}

void AudioEngine::clearInstrumentModulation(int sampleId) {
    if (sampleId < 0 || sampleId >= 256) return;
    for (int m = 0; m < 4; m++) {
        instrumentModSlots.edit(sampleId)[m] = InstrumentModSlot();
    }
    instrumentModSlots.publish(sampleId);
}

void AudioEngine::updateVoiceModulation(IAudioVoice& voice, int numFrames, float sampleRate) {
    runModMatrix(voice, numFrames, sampleRate);
}

void AudioEngine::updateVoicePitchMod(Voice& voice, int numFrames, float sampleRate) {
    tickPitchSlide(voice, numFrames);
    tickVibrato(voice, numFrames, sampleRate);
}

float AudioEngine::getModulatedPlaybackRate(Voice& voice) {
    // modDestValues[PARAM_PITCH] accumulates: TABLE_PITCH + PITCH_SLIDE + VIBRATO + user mod slots.
    // params.base[PARAM_PITCH] is FIN's fine tune — the same slot, its other half, cleared by every
    // trigger. Read as a pair here so a fine tune and a table transpose add rather than replace.
    // voice.playbackRate has no transpose baked in; arpeggio adjusts it via setMidiNote().
    float rateMod = powf(2.0f,
                         (voice.modDestValues[PARAM_PITCH] + voice.params.base[PARAM_PITCH]) / 12.0f);
    const float rate = voice.playbackRate * rateMod;

    // ── OSCILLATOR loop mode: one trip round the loop is one cycle of the played note ───────────
    //
    // ⭐⭐ **THE WHOLE MODE IS THIS ONE FACTOR, AND IT INHERITS EVERY PITCH SOURCE FOR FREE.**
    // `rate × baseFrequency` is what the voice is sounding at right now — whatever moved it: the
    // table transpose, FIN, a slide, vibrato, an arpeggio. Traversals per second is
    // `rate × sampleRate / loopLength`, and the mode wants that to EQUAL the sounding frequency, so
    // the rate is scaled by `loopLength × baseFrequency / sampleRate` and nothing else has to know.
    //
    // ⭐ The factor is 1.0 exactly when the loop is one cycle long at the sample's own base pitch —
    // i.e. oscillator mode and forward mode agree precisely where you would expect them to, which is
    // the arithmetic's own self-check.
    //
    // ⚠️ So in this mode the loop LENGTH is a TIMBRE control, not a pitch one: a longer window packs
    // more of the file into each cycle. That is the opposite of a plain forward loop, where a short
    // loop is what makes the pitch, and it is why LPO — which never changes the length — is the
    // command this mode is built to be played with.
    if (voice.loopMode == LOOP_MODE_OSCILLATOR && voice.baseFrequency > 0.0f) {
        const int loopLength = voice.actualLoopEnd - voice.actualLoopStart;
        const float sr = (float)getSampleRate();
        if (loopLength > 0 && sr > 0.0f) return rate * ((float)loopLength * voice.baseFrequency / sr);
    }
    return rate;
}

void AudioEngine::renderOffline(int numFrames, float* output, int sampleRate) {
    setFlushToZeroForCurrentThread();
    for (int i = 0; i < numFrames * 2; i++) output[i] = 0.0f;

    // The same granularity live playback uses — the two must not drift apart, or the export stops
    // matching what you heard. See PROCESS_SUBBLOCK.
    int rendered = 0;
    while (rendered < numFrames) {
        int chunk = std::min(PROCESS_SUBBLOCK, numFrames - rendered);
        processAudioBlock(output + rendered * 2, chunk, 2, (float)sampleRate);
        rendered += chunk;
    }
}

void AudioEngine::resetFrameCounter() {
    globalFrameCounter.store(0, std::memory_order_relaxed);
    // Fresh randomness per render — see noteSeedEntropy in the header.
    noteSeedEntropy = ((uint32_t)nowMs() * 2654435761u) | 1u;
}

void AudioEngine::resetEffectState() {
    const float sr = (float)getSampleRate();
    reverbSend.reset(sr);   // zeroes the delay lines AND reseeds ReverbSc's random-lineseg LCG
    delaySend.reset(sr);    // zeroes both delay lines
    masterChain.reset(sr);  // OTT bands, DUST, limiter envelope, master EQ
    // Everything above is now at FACTORY DEFAULTS, not at the project's values. A render pushes the
    // project next; a device reopen replays `busSettings` (setDeviceSampleRate). Not replayed HERE:
    // a render must be a function of the project alone, so nothing may depend on what was pushed
    // before the reset — the two renders ptrender compares would otherwise take different paths.
    LOGD("🎬 Effect chains reset to clean state");
}

// Control thread: build the reverb's engine for what the record now says, BEFORE the record is
// published — so the block that applies the new ALGO or SIZE finds the engine already waiting.
void AudioEngine::prepareReverb() {
    const BusSettings& s = busSettings;
    reverbSend.dragonfly.prepare(s.reverbAlgo, s.reverbDecay, s.reverbSize, s.reverbDamp, s.reverbMod,
                                 static_cast<float>(getSampleRate()));
}

// Audio thread, top of the block: apply every bus group whose sequence number moved since this
// thread last applied it — or every group, after setDeviceSampleRate rebuilt the buses.
void AudioEngine::applyBusSettings() {
    busPublisher.read(busLive, busSeen);
    const bool all = busReplayRequested.exchange(false, std::memory_order_acquire) && busLive.pushed;
    const BusSettings& s = busLive;
    const auto due = [&](BusGroup g) {
        if (!all && s.seq[g] == busApplied[g]) return false;
        busApplied[g] = s.seq[g];
        return true;
    };
    if (due(BUS_REVERB_PARAMS)) reverbSend.setParams(s.reverbDecay, s.reverbDamp, s.reverbSize);
    if (due(BUS_REVERB_ALGO))   reverbSend.setAlgo(s.reverbAlgo);
    if (due(BUS_REVERB_CHAR))   reverbSend.setCharacter(s.reverbPre, s.reverbWidth, s.reverbMod);
    if (due(BUS_REVERB_INEQ))   applyEqPresetToModule(reverbSend.inputEq, s.reverbInputEq);
    if (due(BUS_DELAY_TIME)) {
        if (s.delaySync) delaySend.setTimeSync(s.delayTime, s.delayBpm);
        else             delaySend.setTimeFree(s.delayTime);
    }
    if (due(BUS_DELAY_FEEDBACK)) delaySend.feedback = s.delayFeedback / 255.0f;
    if (due(BUS_DELAY_CHAR))     delaySend.setCharacter(s.delayPong, s.delayTone, s.delayWobble);
    if (due(BUS_DELAY_INEQ))     applyEqPresetToModule(delaySend.inputEq, s.delayInputEq);
    if (due(BUS_MASTER_EQ))      applyEqPresetToModule(masterChain.masterEq, s.masterEqSlot);
    if (due(BUS_OTT)) {
        if (s.ottForRender) masterChain.ott.resetForRender(s.ottDepth / 255.0f);
        else                masterChain.ott.setDepth(s.ottDepth / 255.0f);
    }
    if (due(BUS_MASTER_FX)) masterChain.setMasterFx(s.masterFx);
    if (due(BUS_DUST)) {
        if (s.dustForRender) masterChain.setDustDepthForRender(s.dustDepth / 255.0f);
        else                 masterChain.setDustDepth(s.dustDepth / 255.0f);
    }
    if (due(BUS_LIMITER)) masterChain.setLimiterPreGain(1.0f + (s.limiterPreGain / 255.0f) * 3.0f);
}

int64_t AudioEngine::getFrameCounter() {
    return globalFrameCounter.load(std::memory_order_relaxed);
}

void AudioEngine::setOfflineRendering(bool offline) {
    isOfflineRendering.store(offline);
    LOGD("🎬 Offline rendering: %s", offline ? "ON" : "OFF");
}

void AudioEngine::setTempo(int tempo) {
    // Clamp to a sane musical range; the table-advance divides by this so it must be > 0.
    currentTempo.store(std::max(1, tempo), std::memory_order_relaxed);
}

// ─── The metronome ───────────────────────────────────────────────────────────────────────────────

void AudioEngine::setMetronome(bool enabled, float gain) {
    metronomeOn.store(enabled, std::memory_order_relaxed);
    metronomeGain.store(std::max(0.0f, std::min(1.0f, gain)), std::memory_order_relaxed);
}

void AudioEngine::startMetronome(int64_t startFrame, int64_t framesPerBeat) {
    metronomeBeatFrames.store(framesPerBeat > 0 ? framesPerBeat : 0, std::memory_order_relaxed);
    metronomeEpoch.store(startFrame, std::memory_order_relaxed);
}

void AudioEngine::setMetronomeBeat(int64_t framesPerBeat) {
    if (framesPerBeat > 0) metronomeBeatFrames.store(framesPerBeat, std::memory_order_relaxed);
}

void AudioEngine::stopMetronome() {
    metronomeEpoch.store(-1, std::memory_order_relaxed);
}

void AudioEngine::renderMetronome(float* output, int numFrames, int channelCount, float sampleRate,
                                  int64_t blockStartFrame, bool offlineRender) {
    // An export carries the SONG, never the click that was helping the user write it. Also drops the
    // click in flight, so a render started mid-beat cannot leak its tail into the first block back.
    if (offlineRender) { metroClickPos_ = -1; return; }

    const int64_t epoch = metronomeEpoch.load(std::memory_order_relaxed);
    const int64_t beat  = metronomeBeatFrames.load(std::memory_order_relaxed);

    if (epoch != metroEpoch_) {
        // A new take. The grid is re-pinned to the transport's own start frame, which is what makes
        // beat 0 the downbeat rather than "wherever the click happened to be".
        metroEpoch_      = epoch;
        metroBeatFrames_ = beat;
        metroIndex_      = 0;
        metroCount_      = 0;
        metroClickPos_   = -1;
    } else if (beat > 0 && beat != metroBeatFrames_) {
        // TEMPO was turned mid-take. The epoch moves to where the NEXT beat was already going to
        // fall and the index restarts, so that beat keeps its slot — no beat jumps, doubles or is
        // lost — and every one after it takes the new spacing. songcore::MidiClock::rebase, same
        // arithmetic and same reason.
        metroEpoch_      = metroEpoch_ + metroIndex_ * metroBeatFrames_;
        metroIndex_      = 0;
        metroBeatFrames_ = beat;
    }

    if (!metronomeOn.load(std::memory_order_relaxed)) { metroClickPos_ = -1; return; }
    const float gain = metronomeGain.load(std::memory_order_relaxed);
    if (gain <= 0.0f) { metroClickPos_ = -1; return; }
    if (metroEpoch_ < 0 || metroBeatFrames_ <= 0) return;   // nothing is playing

    // ⚠️ The audio device can STALL AND RESUME — an Android suspend, a CFW power menu — and the frame
    // counter then jumps by the whole stall at once. Walking the backlog one beat at a time would fire
    // a click per block until it caught up; snap the grid to where the song actually is instead. The
    // index still counts every beat the song passed, so the accent stays on the bar.
    const int64_t behind = blockStartFrame - (metroEpoch_ + metroIndex_ * metroBeatFrames_);
    if (behind > metroBeatFrames_) {
        const int64_t skipped = behind / metroBeatFrames_;
        metroIndex_ += skipped;
        metroCount_ += skipped;
    }

    const int clickFrames = std::max(1, static_cast<int>(sampleRate * METRONOME_CLICK_SEC));

    for (int i = 0; i < numFrames; i++) {
        // A beat always restarts the click, rather than being dropped while one is still sounding:
        // at 999 BPM a beat is shorter than the click, and a metronome that skips beats when it is
        // pushed is worse than one that cuts its own tail.
        if (blockStartFrame + i >= metroEpoch_ + metroIndex_ * metroBeatFrames_) {
            const bool accent = (metroCount_ % METRONOME_BEATS_PER_BAR) == 0;
            metroClickPhase_  = 0.0f;
            metroClickStep_   = 2.0f * static_cast<float>(M_PI) *
                                (accent ? METRONOME_ACCENT_HZ : METRONOME_BEAT_HZ) / sampleRate;
            metroClickPos_    = 0;
            metroIndex_++;
            metroCount_++;
        }
        if (metroClickPos_ < 0) continue;

        // A cubic decay from the first sample, which reaches exactly zero at the end of the window —
        // an envelope that merely got small would step at the cut. The sine starts at phase 0, so
        // there is no discontinuity at the onset either.
        const float t   = static_cast<float>(metroClickPos_) / static_cast<float>(clickFrames);
        const float env = (1.0f - t) * (1.0f - t) * (1.0f - t);
        const float s   = sinf(metroClickPhase_) * env * gain;
        metroClickPhase_ += metroClickStep_;
        if (++metroClickPos_ >= clickFrames) metroClickPos_ = -1;

        // Clamped, because this is summed BELOW the limiter: a loud mix plus a click can ask for more
        // than the DAC has, and a hard clip on a 30 ms transient is inaudible where the wrap is not.
        for (int ch = 0; ch < channelCount; ch++) {
            float& out = output[i * channelCount + ch];
            out = fmaxf(-1.0f, fminf(1.0f, out + s));
        }
    }
}
