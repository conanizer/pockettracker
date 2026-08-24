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
#include "table_automation.h"  // AUS/AUF pairing over a table's rows — shared with the table editor
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <new>
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
}  // namespace

AudioEngine::AudioEngine() {
    for (int i = 0; i < 256; i++) {
        samples[i] = nullptr;
        samplesRight[i] = nullptr;
        sampleLengths[i] = 0;
        instrumentParams[i] = InstrumentParams();
        sampleBackups[i] = nullptr;
        sampleBackupsRight[i] = nullptr;
        sampleBackupLengths[i] = 0;
        originalSamples[i] = nullptr;
        originalSamplesRight[i] = nullptr;
        originalSampleLengths[i] = 0;
    }
    for (int t = 0; t < SF_VOICE_COUNT; t++) tic00Cursor[t] = Tic00Cursor();
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
    resetEffectState();   // reads getSampleRate(), i.e. the value just stored
}

AudioEngine::~AudioEngine() {
    // The platform backend (OboeAudioEngine on Android, SdlAudioEngine on desktop) owns and closes the
    // output stream; the core just frees its buffers. The owner (android-main's / the shell's `main`)
    // destroys the backend first, so no callback can run during this teardown.
    for (int i = 0; i < 256; i++) {
        if (samples[i])              delete[] samples[i];
        if (samplesRight[i])         delete[] samplesRight[i];
        if (sampleBackups[i])        delete[] sampleBackups[i];
        if (sampleBackupsRight[i])   delete[] sampleBackupsRight[i];
        if (originalSamples[i])      delete[] originalSamples[i];
        if (originalSamplesRight[i]) delete[] originalSamplesRight[i];
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

    // Stop any voice that is currently playing this sample so its sampleData
    // pointer can't be followed after we free the buffer below.
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].instrId == id && voices[i].isActive) voices[i].stop();
    }

    if (samples[id]) delete[] samples[id];
    if (samplesRight[id]) { delete[] samplesRight[id]; samplesRight[id] = nullptr; }
    // New file also invalidates the sample editor's undo backup — keeping it would hold RAM and
    // let undoSample restore the PREVIOUS sample's audio onto this one.
    delete[] sampleBackups[id];        sampleBackups[id] = nullptr;
    delete[] sampleBackupsRight[id];   sampleBackupsRight[id] = nullptr;
    sampleBackupLengths[id] = 0;
    // New file replaces the original — discard any cached rate-mode original.
    if (originalSamples[id]) {
        delete[] originalSamples[id];
        originalSamples[id] = nullptr;
        originalSampleLengths[id] = 0;
    }
    if (originalSamplesRight[id]) { delete[] originalSamplesRight[id]; originalSamplesRight[id] = nullptr; }

    samples[id] = newL;
    sampleLengths[id] = length;

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

    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].instrId == id && voices[i].isActive) voices[i].stop();
    }

    if (samples[id]) delete[] samples[id];
    if (samplesRight[id]) { delete[] samplesRight[id]; samplesRight[id] = nullptr; }
    // Same as loadSample: a new file invalidates the old sample's undo backup.
    delete[] sampleBackups[id];        sampleBackups[id] = nullptr;
    delete[] sampleBackupsRight[id];   sampleBackupsRight[id] = nullptr;
    sampleBackupLengths[id] = 0;
    if (originalSamples[id]) {
        delete[] originalSamples[id];
        originalSamples[id] = nullptr;
        originalSampleLengths[id] = 0;
    }
    if (originalSamplesRight[id]) { delete[] originalSamplesRight[id]; originalSamplesRight[id] = nullptr; }

    samples[id]      = newL;
    samplesRight[id] = newR;
    sampleLengths[id] = length;

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
    // Stop any voice on this slot, then free every stale per-slot buffer (mirror loadSampleFromWavFile).
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].instrId == id && voices[v].isActive) voices[v].stop();
    }
    delete[] samples[id];
    delete[] samplesRight[id];
    delete[] sampleBackups[id];        sampleBackups[id] = nullptr;
    delete[] sampleBackupsRight[id];   sampleBackupsRight[id] = nullptr;
    sampleBackupLengths[id] = 0;
    delete[] originalSamples[id];      originalSamples[id] = nullptr;
    delete[] originalSamplesRight[id]; originalSamplesRight[id] = nullptr;
    originalSampleLengths[id] = 0;
    samples[id]       = newL;
    samplesRight[id]  = newR;
    sampleLengths[id] = 0;             // not playable until finalize
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
    int frames = streamLoadFilled;
    streamLoadId = -1; streamLoadChannels = 0; streamLoadCapacity = 0; streamLoadFilled = 0;
    LOGD("Streaming sample load: id=%d %d frames", id, frames);
    return frames;
}

void AudioEngine::cancelSampleLoad(int id) {
    // Decode failed/aborted — free the partially-filled buffer so it doesn't linger or play as garbage.
    if (id != streamLoadId) return;
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].instrId == id && voices[v].isActive) voices[v].stop();
    }
    delete[] samples[id];      samples[id] = nullptr;
    delete[] samplesRight[id]; samplesRight[id] = nullptr;
    sampleLengths[id] = 0;
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
    int frameIdx = 0;
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
        if (framesGot < want) break;  // short read / truncated file (shouldn't happen — see clamp)
    }
    fclose(f);

    // `new float[]` is not zero-initialized; a short read above would leave indeterminate tail
    // samples. dataSize is clamped to the bytes actually present, so this is defensive — but zero
    // any unfilled tail so we can never play uninitialized memory as noise.
    if (frameIdx < totalFrames) {
        std::memset(newL + frameIdx, 0, (size_t)(totalFrames - frameIdx) * sizeof(float));
        if (newR) std::memset(newR + frameIdx, 0, (size_t)(totalFrames - frameIdx) * sizeof(float));
    }

    // Swap into the slot under the edit lock (audio thread try_locks it in the mix loop), stopping
    // any voice reading the old buffer first — same discipline as loadSample. Free EVERY stale
    // per-slot buffer: a fresh file makes the old sample's undo/rate caches meaningless.
    {
        std::lock_guard<std::mutex> lock(sampleEditMutex);
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].instrId == id && voices[v].isActive) voices[v].stop();
        }
        delete[] samples[id];
        delete[] samplesRight[id];
        delete[] sampleBackups[id];        sampleBackups[id] = nullptr;
        delete[] sampleBackupsRight[id];   sampleBackupsRight[id] = nullptr;
        sampleBackupLengths[id] = 0;
        delete[] originalSamples[id];      originalSamples[id] = nullptr;
        delete[] originalSamplesRight[id]; originalSamplesRight[id] = nullptr;
        originalSampleLengths[id] = 0;
        samples[id] = newL;
        samplesRight[id] = newR;
        sampleLengths[id] = totalFrames;
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
            ok = ptdec::decodeOggFile(path, L, R, sr);
            if (!ok) { L.clear(); R.clear(); ok = ptdec::decodeOpusFile(path, L, R, sr); }
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
    }
    total += pcm(fxPreviewBackup, fxPreviewBackupRight, fxPreviewBackupLen,    4);
    total += pcm(sampleClipboard, sampleClipboardRight, sampleClipboardLength, 4);

    // A SoundFont's PCM lives inside tsf, which converts every sample to float on load. One handle is
    // shared by every track pointed at that slot, so it is counted once per LOADED FONT rather than
    // per instrument — the same 40 MB font on four tracks is 40 MB, not 160.
    for (int slot = 0; slot < MAX_SOUNDFONTS; ++slot) {
        if (soundfonts[slot].handle)
            total += static_cast<int64_t>(tsf_get_fontsamplecount(soundfonts[slot].handle)) * 4;
    }
    return total;
}

void AudioEngine::clearSample(int id) {
    if (id < 0 || id >= 256) return;
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].instrId == id && voices[i].isActive) voices[i].stop();
    }
    // Free every per-slot buffer so a sample doesn't linger in memory after the slot is repurposed
    // (e.g. switching the instrument to SoundFont). delete[] nullptr is a safe no-op.
    delete[] samples[id];              samples[id] = nullptr;
    delete[] samplesRight[id];         samplesRight[id] = nullptr;
    delete[] sampleBackups[id];        sampleBackups[id] = nullptr;
    delete[] sampleBackupsRight[id];   sampleBackupsRight[id] = nullptr;
    delete[] originalSamples[id];      originalSamples[id] = nullptr;
    delete[] originalSamplesRight[id]; originalSamplesRight[id] = nullptr;
    sampleLengths[id]        = 0;
    sampleBackupLengths[id]  = 0;
    originalSampleLengths[id] = 0;
    LOGD("Sample %d cleared from memory", id);
}

void AudioEngine::clearAllSamples() {
    // Hold sampleEditMutex for the entire operation.  The audio thread uses
    // try_to_lock so it skips its mix block rather than reading freed memory.
    std::lock_guard<std::mutex> lock(sampleEditMutex);

    // Stop all voices inside the lock so we know the audio thread can't be
    // mid-read when we free the buffers below.
    for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].stop();
    }
    // Clear queues to prevent re-triggering stopped voices.
    // Each queue method acquires its own internal mutex; no deadlock risk
    // because the audio thread cannot hold those mutexes while we hold sampleEditMutex.
    noteQueue.clear();
    killQueue.clear();
    paramUpdateQueue.clear();

    for (int i = 0; i < 256; i++) {
        if (samples[i]) {
            delete[] samples[i];
            samples[i] = nullptr;
        }
        if (samplesRight[i]) {
            delete[] samplesRight[i];
            samplesRight[i] = nullptr;
        }
        sampleLengths[i] = 0;
        if (originalSamples[i]) {
            delete[] originalSamples[i];
            originalSamples[i] = nullptr;
            originalSampleLengths[i] = 0;
        }
        if (originalSamplesRight[i]) { delete[] originalSamplesRight[i]; originalSamplesRight[i] = nullptr; }
    }
    LOGD("All samples cleared");
}

// Sample editor operations live in sample-editor.cpp.

void AudioEngine::stopTrack(int trackId) {
    // Fade instead of hard stop: this is the preview-stop path (new preview supersedes the old
    // one / user stops it), and a mid-waveform stop() clicks on sustained material.
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].trackId == trackId && voices[i].isActive) {
            voices[i].startFadeOut(KILL_FADE_SAMPLES);
        }
    }
    if (trackId >= 0 && trackId < SF_VOICE_COUNT) {
        SoundfontVoice& sv = sfVoices[trackId];
        // Preview lane: first stop starts TSF's release envelope (click-free, musical stop for the
        // "any button stops the preview" UX); a second stop while releasing hard-cuts so a long-REL
        // sound can't ignore the user. Song tracks keep the immediate hard stop.
        if (trackId == PREVIEW_LANE && sv.isActive && !sv.isReleasingOnly) {
            sv.noteOff();
        } else {
            sv.hardStop();
        }
    }
}

void AudioEngine::stopAll() {
    for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].stop();
    }
    // Stop all soundfont notes on all tracks (incl. the preview lane)
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        sfVoices[t].hardStop();
        tic00Cursor[t] = Tic00Cursor();  // transport stop rewinds every TIC00 table: PLAY starts at row 0
    }
    LOGD("stopAll: voices and SF notes cleared, stream stays running");
}

int AudioEngine::getActiveVoiceCount() {
    int count = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].isActive) {
            count++;
        }
    }
    return count;
}

void AudioEngine::getTrackActiveNotes(int* out, int trackCount) {
    for (int t = 0; t < trackCount; t++) out[t] = -1;
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].isActive) continue;
        int t = voices[v].trackId;
        if (t >= 0 && t < trackCount && out[t] == -1) {
            out[t] = voices[v].noteOctave * 12 + voices[v].notePitch;
        }
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
        if (out[t] == -1 && sfVoices[t].isActive) {
            out[t] = sfVoices[t].noteOctave * 12 + sfVoices[t].notePitch;
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
void AudioEngine::effectiveTicRatesFor(int tableId, int fallback, int out[TABLE_LANES]) {
    for (int l = 0; l < TABLE_LANES; ++l) out[l] = fallback;
    if (tableId < 0 || tableId >= 256) return;
    std::lock_guard<std::mutex> lock(tableMutex);
    if (!tables[tableId].loaded) return;
    const TableRow& lastRow = tables[tableId].rows[15];
    if (lastRow.fx1Type == FX_TIC) out[0] = lastRow.fx1Value;
    if (lastRow.fx2Type == FX_TIC) out[1] = lastRow.fx2Value;
    if (lastRow.fx3Type == FX_TIC) out[2] = lastRow.fx3Value;
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
    // ONE tableMutex acquisition per voice per block: read the loaded flag AND copy the whole table.
    //
    // ⚠️ THE WHOLE TABLE, not just the current row, because the AUS/AUF pairing below is re-derived
    // from every row on every block — that is what lets a backwards HOP resume a ramp mid-span with
    // nothing stored per voice. 128 bytes inside a lock the block already takes, rather than a
    // second acquisition for the ramps.
    bool tableLoaded = false;
    TableRow rows[16];
    {
        std::lock_guard<std::mutex> lock(tableMutex);
        tableLoaded = tables[voice.tableId].loaded;
        if (tableLoaded)
            for (int i = 0; i < 16; ++i) rows[i] = tables[voice.tableId].rows[i];
    }
    if (!tableLoaded) return;

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

        if (shouldProcessRow) processTableRow(voice, rows[L.row], lane, shouldAdvance, sampleRate);
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
template <typename V>
void AudioEngine::processTableRow(V& voice, const TableRow& row, int lane, bool shouldAdvance,
                                  float sampleRate) {
    TableLane& L = voice.lanes[lane];

    if (lane == 0) {
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
                setMasterEqSlot(fxValue);
                tableMasterEqTouched.store(true, std::memory_order_relaxed);
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
    } else if (shouldAdvance) {
        L.row = (L.row + 1) % 16;
    }

    if (lane == 0 && shouldAdvance && L.row == 0) {
        LOGT("📋 Table %d loop: track=%d, transpose=%.0f, vol=%.2f",
             voice.tableId, voice.getTrackId(), voice.tableTranspose, voice.tableVolume);
    }
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
    for (int t = 0; t < 8; t++) { framePeaksPerTrackL[t] = 0.0f; framePeaksPerTrackR[t] = 0.0f; }
    frameSendPeakRevL = frameSendPeakRevR = frameSendPeakDelL = frameSendPeakDelR = 0.0f;

    // Snapshot real-time volumes ONCE per block under a single lock, then read them lock-free in the
    // hot loops below. Previously volumeMutex was taken per-sample-per-voice (~350k locks/sec/voice) —
    // pure overhead plus a dropout hazard if the Kotlin thread held the lock during setTrackVolume/
    // setMasterVolume. One block of slightly-stale volume is inaudible.

    // ⚠️ HOISTED ABOVE THE SNAPSHOT because the mute gate reads it: an export must not FADE a muted
    // track out over its first 5.8 ms, it must start silent. Also used by the visualizer gates below.
    const bool offlineRender = isOfflineRendering.load(std::memory_order_relaxed);

    float trackVolSnapshot[SF_VOICE_COUNT];
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
    float masterVolSnapshot;
    int previewTrack;
    {
        std::lock_guard<std::mutex> lock(volumeMutex);
        // Per full swing, so the ramp is the same wall-clock length whatever the block size.
        const float gateStep = (float)numFrames / (float)MUTE_GATE_SAMPLES;
        for (int t = 0; t < 8; t++) {
            trackVolSnapshot[t] = trackVolumes[t];
            const float target  = trackMuted[t] ? 0.0f : 1.0f;
            if (offlineRender) trackGate[t] = target;   // a mute is a STATE in an export, not a gesture
            gateStart[t] = trackGate[t];
            if      (trackGate[t] < target) trackGate[t] = fminf(target, trackGate[t] + gateStep);
            else if (trackGate[t] > target) trackGate[t] = fmaxf(target, trackGate[t] - gateStep);
            gateEnd[t]   = trackGate[t];
        }
        masterVolSnapshot = masterVolume;
        previewTrack      = previewLaneTrack;
    }
    // The preview lane borrows the fader of the channel the audition came from — the lane is a ninth
    // voice with no fader of its own, and an instrument you can only hear at full dry level tells you
    // nothing about how it sits in the mix. -1 (no origin) keeps the neutral gain it has always had.
    //
    // ⚠️ THE ASSIGNMENT SITS AFTER THE SNAPSHOT LOOP, not in the setter: reading trackVolSnapshot here
    // is what makes it the LIVE fader, so a VTR or a mixer move lands in the audition it is aimed at.
    const bool previewBorrows      = (previewTrack >= 0 && previewTrack < 8);
    trackVolSnapshot[PREVIEW_LANE] = previewBorrows ? trackVolSnapshot[previewTrack] : 1.0f;
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
    paramUpdateQueue.drainUntil(blockEnd, paramBatch);
    killQueue.drainUntil(blockEnd, killBatch);
    noteQueue.drainUntil(blockEnd, noteBatch);
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
                    setMasterEqSlot((int)upd.value);
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
                case PARAM_UPDATE_TRACK_VOL: {            // VTR — this track's mixer fader
                    if (upd.trackId >= 0 && upd.trackId < 8) {
                        // Both take the authored value: the mute is no longer folded in here, it is a
                        // separate gate multiplied at the two read sites. The fader therefore keeps
                        // moving under a muted track exactly as before, so unmuting lands on wherever
                        // the ramp has got to rather than on where it started.
                        applyTrackVolume(upd.trackId, upd.value);
                        trackVolSnapshot[upd.trackId] = upd.value;
                    }
                    break;
                }
                case PARAM_UPDATE_MASTER_VOL: {           // VMV — the master fader (global)
                    applyMasterVolume(upd.value);
                    masterVolSnapshot = upd.value;
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
                triggerKeyRelease(kill.trackId);
                // SF: unchanged. TSF owns its own release envelope, a SoundFont preset always HAS one,
                // and "a one-shot with no envelope" is a sampler-only shape — there is nothing for the
                // §4.1 rule to decide on this side.
                if (kill.trackId >= 0 && kill.trackId < SF_VOICE_COUNT) {
                    sfVoices[kill.trackId].noteOff();
                }
                LOGT("🎹 Key release: track %d at frame %lld", kill.trackId, (long long)currentFrame);
            } else if (kill.mode == KILL_SOFT) {
                triggerNoteOff(kill.trackId);  // Sampler: trigger ADSR release
                // SF: noteOff (TSF handles its own release envelope internally)
                if (kill.trackId >= 0 && kill.trackId < SF_VOICE_COUNT) {
                    sfVoices[kill.trackId].noteOff();
                }
                LOGT("🎵 Note-off: track %d at frame %lld", kill.trackId, (long long)currentFrame);
            } else {
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == kill.trackId && voices[v].isActive) {
                        voices[v].startFadeOut(KILL_FADE_SAMPLES);  // soft deliberate cut, not a steal
                        LOGT("🔪 Killed track %d at frame %lld", kill.trackId, (long long)currentFrame);
                    }
                }
                // SF: soft kill so TSF's internal release envelope can play out.
                if (kill.trackId >= 0 && kill.trackId < SF_VOICE_COUNT) {
                    sfVoices[kill.trackId].noteOff();
                }
            }
        }

        // Trigger all notes scheduled for this exact frame
        while (noteIdx < noteBatch.size() && noteBatch[noteIdx].targetFrame <= currentFrame) {
            ScheduledNote note = noteBatch[noteIdx++];

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
                    float trkVol = trackVolSnapshot[t];
                    // This instrument's ADSR override (applied atomically inside fireArmedNote, before
                    // note_on) — keyed by instrument id so de-duplicated handles stay isolated.
                    int eAtk = -1, eDec = -1, eSus = -1, eRel = -1;
                    if (note.sampleId >= 0 && note.sampleId < 256) {
                        const SfEnvOverride& eo = sfEnvOverrides[note.sampleId];
                        eAtk = eo.atk; eDec = eo.dec; eSus = eo.sus; eRel = eo.rel;
                    }
                    if (!sv.armNote(note.sfSlot, note.midiNote, note.midiVelocity,
                                    note.volume, trkVol, note.pan, note.sfBank, note.sfPreset, t,
                                    eAtk, eDec, eSus, eRel)) {
                        LOGT("🎹 SF DROPPED: sfSlot=%d track=%d (handle not loaded)",
                             note.sfSlot, note.trackId);
                        continue;   // …and the voice keeps whatever it was already playing
                    }
                    soundfonts[note.sfSlot].lastUsed.store(nextSfUseTick(), std::memory_order_relaxed);  // LRU touch
                    sv.isReleasingOnly = false;
                    sv.resetPitchState();
                    sv.detuneSemitones = note.detuneSemitones;  // static instrument detune (set after reset)
                    sv.startDelayFrames = frame;  // start rendering at the note's exact intra-block frame

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
                    sv.chain.reset(sampleRate);
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
            for (int v = 0; v < MAX_VOICES; v++) {
                if (voices[v].trackId == note.trackId && voices[v].isActive && !voices[v].isFadingOut
                    && voices[v].tableId >= 0) {
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
                note.tableId >= 0 && tic00Cursor[note.trackId].tableId == note.tableId) {
                const Tic00Cursor& c = tic00Cursor[note.trackId];
                for (int l = 0; l < TABLE_LANES; ++l) {
                    if (c.ticRate[l] != 0x00 || !c.active[l]) continue;
                    wasTIC00Mode = true;
                    savedTableRows[l] = tic00RowAfter(c.row[l], c.lastProcessed[l]);
                    LOGT("📋 TIC00: table row %d for track %d column %d retrigger (from track cursor)",
                         savedTableRows[l], note.trackId, l + 1);
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

            // Step 1: mono per track — fade whatever is still playing on this track
            for (int v = 0; v < MAX_VOICES; v++) {
                if (voices[v].trackId == note.trackId && voices[v].isActive && !voices[v].isFadingOut) {
                    voices[v].startFadeOut();
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
                if (note.sampleId >= 0 && note.sampleId < 256 && samples[note.sampleId]) {
                    // Per-track mono across voice types: a sampler note replaces an SF note
                    // still sounding on this track. noteOff (not hardStop) so the SF release
                    // plays out musically — findActiveVoiceForTrack skips releasing SF voices,
                    // so mid-note params already target the new sampler voice meanwhile.
                    if (note.trackId >= 0 && note.trackId < SF_VOICE_COUNT &&
                        sfVoices[note.trackId].isActive && !sfVoices[note.trackId].isReleasingOnly) {
                        sfVoices[note.trackId].noteOff();
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

                    voices[v].trigger(samples[note.sampleId], samplesRight[note.sampleId], sampleLengths[note.sampleId],
                                      note.trackId, rate, note.volume, note.phraseVolume, note.pan, instrumentParams[note.sampleId],
                                      sampleRate, note.startPointOverride, note.endPointOverride,
                                      note.tableId, effectiveTicRates, note.noteOctave, note.notePitch, startRows);
                    voices[v].instrId = note.sampleId;
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
                         note.tableId, effectiveTicRate, note.noteOctave, note.notePitch, startRow);
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
                Tic00Cursor& c = tic00Cursor[t];
                c.tableId = voices[v].tableId;
                for (int l = 0; l < TABLE_LANES; ++l) {
                    c.row[l]           = voices[v].lanes[l].row;
                    c.lastProcessed[l] = voices[v].lanes[l].lastProcessed;
                    c.ticRate[l]       = voices[v].lanes[l].ticRate;
                    c.active[l]        = voices[v].lanes[l].active;
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

    // Mix voices — try_lock so applyRateMode can swap buffers safely.
    // If the edit lock is held we skip one callback (~10ms silence) instead of crashing.
    {
    std::unique_lock<std::mutex> editLock(sampleEditMutex, std::try_to_lock);
    if (editLock.owns_lock()) {
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice& voice = voices[v];
        if (!voice.isActive || !voice.sampleData) continue;

        float modulatedRate = getModulatedPlaybackRate(voice);

        int effDrive      = std::max(0, std::min(255, (int)(voice.params.base[PARAM_DRIVE]      + voice.modDestValues[PARAM_DRIVE])));
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
                voice.actualStart = std::max(0,             std::min((int)(rawStart * sl / 255.0f), sl - 2));
                voice.actualEnd   = std::max(voice.actualStart + 1, std::min((int)(rawEnd * sl / 255.0f), sl - 1));
            }
            float rawLoop    = voice.params.base[PARAM_LOOP_START]   + voice.modDestValues[PARAM_LOOP_START];
            voice.actualLoopStart = std::max(voice.actualStart, std::min((int)(rawLoop * sl / 255.0f), voice.actualEnd - 1));
            voice.actualLoopEnd   = std::max(voice.actualLoopStart + 1, std::min((int)((float)voice.loopEndNorm * sl / 255.0f), voice.actualEnd));
        }

        // Honour the intra-block trigger offset: a note dispatched at blockStart+f must not
        // sound before frame f (kills/params stay block-quantized — onsets are the audible case).
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
            // ⚠️ THE MUTE GATE RIDES ALONG HERE, interpolated across the block on the same `t` as pan:
            // it is the only thing between a mute press and a full-scale step in the output.
            float trackVol = (voice.trackId >= 0 && voice.trackId < SF_VOICE_COUNT)
                           ? trackVolSnapshot[voice.trackId]
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

            if ((stemsMode == 0 || stemsMode >= 9) && voice.reverbSend > 0.0f) {
                revSendBufL[i] += procL * panL * voice.reverbSend;
                revSendBufR[i] += procR * panR * voice.reverbSend;
            }
            if ((stemsMode == 0 || stemsMode >= 9) && voice.delaySend > 0.0f) {
                dlySendBufL[i] += procL * panL * voice.delaySend;
                dlySendBufR[i] += procR * panR * voice.delaySend;
            }

            float globalMul = trackVol * antiClick;
            procL *= globalMul;
            procR *= globalMul;

            if (voice.isFadingOut) {
                float fo = (float)voice.fadeOutRemaining / (float)voice.fadeOutTotal;
                procL *= fo;
                procR *= fo;
                if (--voice.fadeOutRemaining <= 0) {
                    voice.isFadingOut = false;
                    voice.isActive = false;
                }
            }

            float sampleL = procL * panL;
            float sampleR = procR * panR;

            if (stemsMode == 0 || voice.trackId == stemsMode - 1) {
                output[i * channelCount] += sampleL;
                output[i * channelCount + 1] += sampleR;
            }

            if (!voice.isFadingOut && voice.trackId >= 0 && voice.trackId < 8) {
                framePeaksPerTrackL[voice.trackId] = fmaxf(framePeaksPerTrackL[voice.trackId], fabsf(sampleL));
                framePeaksPerTrackR[voice.trackId] = fmaxf(framePeaksPerTrackR[voice.trackId], fabsf(sampleR));
            }
            // OCTA per-track capture: tracks 0-7 plus the preview lane (PREVIEW_TRACK_ID == PREVIEW_LANE).
            // Gated on octaWanted: the accumulators are only zeroed/read when OCTA is shown.
            if (octaWanted && voice.trackId >= 0 && voice.trackId < TRACK_WAVEFORM_COUNT) {
                if (!voice.isFadingOut) trackWasActive[voice.trackId] = true;
                trackWaveAccumL[voice.trackId][i] += sampleL;
                trackWaveAccumR[voice.trackId][i] += sampleR;
            }
            if (monitoredInstrId >= 0 && voice.instrId == monitoredInstrId) {
                instrSpectrumTempL[i] += sampleL;
            }

            if (!voice.isActive) break;

            // Active looping is bounded by LOOP END (region [loopStart, loopEnd]). Once loopReleasing
            // is set (ADSR note-off on a looping voice) the loop is abandoned: every mode runs forward
            // to actualEnd so the [loopEnd, end] tail plays out under the release envelope, then fades.
            if (voice.loopMode == 2 && !voice.loopReleasing) {
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
                    if (voice.loopMode == 1) {
                        voice.position = (double)voice.actualLoopStart;
                    } else {
                        voice.position = (double)voice.actualStart;
                        voice.startFadeOut();
                        break;
                    }
                }
            } else {
                voice.position += modulatedRate;
                bool activeForwardLoop = (voice.loopMode == 1 && !voice.loopReleasing);
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

            float noteVol = sv.modDestValues[PARAM_VOL];
            for (int m = 0; m < 4; m++) {
                VoiceModSlot& mod = sv.voiceMods[m];
                if (mod.type == 0 || mod.stage == 0 || mod.dest != 1) continue;
                // Skip completed mods — don't silence the channel during TSF's release tail.
                // AHD/DRUM done at stage 4, ADSR/TRIG done at stage 5.
                if ((mod.type == 1 || mod.type == 4) && mod.stage == 4) continue;
                if ((mod.type == 2 || mod.type == 5) && mod.stage == 5) continue;
                if (mod.type == 3) {  // LFO: bipolar tremolo
                    noteVol = fmaxf(0.0f, noteVol * (1.0f + mod.envValue * mod.effectiveAmt));
                } else {  // AHD/DRUM/ADSR/TRIG: unipolar gain reduction
                    noteVol = fmaxf(0.0f, noteVol + (mod.envValue - 1.0f) * mod.effectiveAmt);
                }
            }
            // PAN modulation. A SoundFont voice has no panLeft/panRight gains in the mix loop — TSF
            // pans on its own channel — so the modulated value goes back through
            // tsf_channel_set_pan instead, guarded by the same |mod| > 0.001 test the sampler path
            // uses so an unmodulated voice keeps whatever pan the note or a PAN effect gave it.
            const bool panModded = fabsf(sv.params.mod[PARAM_PAN]) > 0.001f;
            const float modPan = panModded ? fmaxf(0.0f, fminf(1.0f, sv.params.get(PARAM_PAN))) : 0.0f;

            // Snapshot sfSlot ONCE into a local: eviction (JNI thread) calls detach() which sets
            // sv.sfSlot = -1 at any moment — re-reading the member after the >= 0 check indexes
            // soundfonts[-1] (out of bounds → garbage tsf* → SIGSEGV in tsf_channel_set_volume).
            int volSlot = sv.sfSlot;
            if (volSlot >= 0 && volSlot < MAX_SOUNDFONTS) {
                float trkVol = trackVolSnapshot[t];
                // Read the handle INSIDE the slot mutex: loadSoundfont's eviction path can
                // tsf_close + null it concurrently; a stale pointer here is a use-after-free.
                std::lock_guard<std::mutex> sfLock(soundfonts[volSlot].mutex);
                tsf* h = soundfonts[volSlot].handle;
                if (h) {
                    tsf_channel_set_volume(h, t, noteVol * trkVol);
                    if (panModded) tsf_channel_set_pan(h, t, modPan);
                }
            }

            // When releasing with ADSR/TRIG VOL mods: stop as soon as all have finished
            // so the channel volume doesn't jump back to the base level after release.
            // (Without this, the mod would be skipped at stage 5, making the channel loud
            // again for one block before TSF silence detection fires.)
            if (sv.isReleasingOnly) {
                bool hasAdsrVolMod  = false;
                bool allAdsrVolDone = true;
                for (int m = 0; m < 4; m++) {
                    const VoiceModSlot& mod = sv.voiceMods[m];
                    if (mod.dest == 1 && (mod.type == 2 || mod.type == 5) && mod.stage > 0) {
                        hasAdsrVolMod = true;
                        if (mod.stage < 5) allAdsrVolDone = false;
                    }
                }
                if (hasAdsrVolMod && allAdsrVolDone) {
                    sv.hardStop();
                    continue;
                }
            }

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
            // Local sfSlot snapshot — see the volSlot comment above (detach() race).
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
                // Handle must be read INSIDE the lock: capturing it before would let
                // loadSoundfont's eviction tsf_close it between the read and the render.
                std::lock_guard<std::mutex> sfLock(soundfonts[slot].mutex);
                tsf* h = soundfonts[slot].handle;
                if (h && !sv.hasArmedNote) {
                    tsf_render_float_channel(h, t, sfBuf, numFrames, 0 /* overwrite */);
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
                    for (int i = rampStart; i < fadeEnd; i++) {
                        const float g = (float)(fadeEnd - i - 1) / (float)(rampLen > 1 ? rampLen - 1 : 1);
                        sfBuf[i * 2]     *= g;
                        sfBuf[i * 2 + 1] *= g;
                    }
                    // Now the old voices can be cut: the samples they contributed are already at zero.
                    sv.fireArmedNote(h);
                    // ⚠️ MIXING, not overwrite — [sfStart, fadeEnd) still holds the tail of the ramp.
                    if (numFrames - sfStart > 0) {
                        tsf_render_float_channel(h, t, sfBuf + sfStart * 2, numFrames - sfStart,
                                                 1 /* mixing */);
                    }
                    rendered = true;
                }
            }
            if (!rendered) continue;

            // ⚠️ THE MUTE GATE IS APPLIED HERE, and it has to be ABOVE the send tap below: the fader
            // itself reaches this path through `tsf_channel_set_volume`, which makes a SoundFont send
            // post-fader where the sampler's is pre-fader, and a muted SF track has always taken its
            // reverb and delay down with it. The gate cannot ride the channel volume — that is set
            // once per block, which is exactly the staircase the ramp exists to remove.
            for (int i = 0; i < numFrames; i++) {
                float lerp_t = (numFrames > 1) ? (float)(i + 1) / (float)numFrames : 1.0f;
                float L = sfBuf[i * 2];
                float R = sfBuf[i * 2 + 1];
                sv.chain.filter.setInterpolatedCoeffs(lerp_t);
                sv.chain.processStereo(L, R);
                const float gate = gateStart[t] + (gateEnd[t] - gateStart[t]) * lerp_t;
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
                trackPeakL = fmaxf(trackPeakL, fabsf(outL));
                trackPeakR = fmaxf(trackPeakR, fabsf(outR));
                if (stemsMode == 0 || t == stemsMode - 1) {
                    output[i * 2]     += outL;
                    output[i * 2 + 1] += outR;
                }
                if (octaWanted) {
                    trackWaveAccumL[t][i] += outL;
                    trackWaveAccumR[t][i] += outR;
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
        }
    }

    // Per-track waveform capture for OCTA visualizer — only when OCTA is being displayed.
    if (octaWanted) {
        std::lock_guard<std::mutex> lock(waveformMutex);
        for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) trackHasVoice[t] = trackWasActive[t];
        for (int i = 0; i < numFrames; i++) {
            for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) {
                // × masterVolSnapshot because the accumulators are filled pre-master: OCTA shows what
                // leaves the master fader, so a master fade takes the scopes down with it.
                trackWaveformBuffer[t][trackWaveformIndex] =
                    (trackWaveAccumL[t][i] + trackWaveAccumR[t][i]) * 0.5f * masterVolSnapshot;
            }
            trackWaveformIndex = (trackWaveformIndex + 1) % WAVEFORM_SIZE;
        }
    }

    // SEND BUSES: delay first so its output can feed into reverb, then reverb
    {
        // revWet*/dlyWet* are engine members; process() fully overwrites them.
        delaySend.process(dlySendBufL, dlySendBufR, dlyWetL, dlyWetR, numFrames);
        if (delayToReverbSend > 0.0001f) {
            for (int i = 0; i < numFrames; i++) {
                revSendBufL[i] += dlyWetL[i] * delayToReverbSend;
                revSendBufR[i] += dlyWetR[i] * delayToReverbSend;
            }
        }
        reverbSend.process(revSendBufL, revSendBufR, revWetL, revWetR, numFrames);
        // Only capture when the EQ/spectrum UI is actually polling, and never block the audio
        // thread on the UI's read — try_lock and drop this block's data on contention (invisible).
        if (spectrumWanted) {
            std::unique_lock<std::mutex> lock(spectrumMutex, std::try_to_lock);
            if (lock.owns_lock()) {
                for (int i = 0; i < numFrames; i++) {
                    delaySpectrumBuffer[delaySpectrumWriteIdx] = dlyWetL[i];
                    delaySpectrumWriteIdx = (delaySpectrumWriteIdx + 1) % SPECTRUM_SIZE;
                    reverbSpectrumBuffer[reverbSpectrumWriteIdx] = revWetL[i];
                    reverbSpectrumWriteIdx = (reverbSpectrumWriteIdx + 1) % SPECTRUM_SIZE;
                }
            }
        }
        for (int i = 0; i < numFrames; i++) {
            float rv  = revWetL[i] * reverbReturnGain;
            float rvR = revWetR[i] * reverbReturnGain;
            float dl  = dlyWetL[i] * delayReturnGain;
            float dlR = dlyWetR[i] * delayReturnGain;
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
            frameSendPeakRevL = fmaxf(frameSendPeakRevL, fabsf(rv));
            frameSendPeakRevR = fmaxf(frameSendPeakRevR, fabsf(rvR));
            frameSendPeakDelL = fmaxf(frameSendPeakDelL, fabsf(dl));
            frameSendPeakDelR = fmaxf(frameSendPeakDelR, fabsf(dlR));
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
    // One multiply for the whole block is not an approximation of the per-voice one it replaces: the
    // param queue drains completely (above the mix loops), so masterVolSnapshot already held a single
    // value for the entire block. A VMV ramp moves the fader once per block either way.
    //
    // The meters and visualiser accumulators were filled pre-master and are scaled to match, so every
    // reading stays post-master as it was.
    //
    // The `!= 1.0f` skip is an optimisation and nothing more — multiplying by exactly 1.0f is the
    // identity in IEEE 754, so a project at the default master FF takes the same samples either way.
    if (masterVolSnapshot != 1.0f) {
        for (int i = 0; i < numFrames * channelCount; i++) output[i] *= masterVolSnapshot;
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

    // During offline WAV render: output silence and let renderOffline process the queue.
    if (isOfflineRendering.load()) {
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

    {
        std::lock_guard<std::mutex> lock(waveformMutex);
        for (int i = 0; i < numFrames; i++) {
            waveformDownsampleCounter++;
            if (waveformDownsampleCounter >= WAVEFORM_DOWNSAMPLE) {
                waveformBuffer[waveformIndex] = output[i * channelCount];
                waveformIndex = (waveformIndex + 1) % WAVEFORM_SIZE;
                waveformDownsampleCounter = 0;
            }
        }
    }

    // Master spectrum ring — only while the spectrum visualizer or EQ screen is polling,
    // and try_lock so the audio thread never blocks on the UI's 2048-sample copy-out.
    if ((nowMs() - lastSpectrumReadMs.load(std::memory_order_relaxed)) < CAPTURE_IDLE_MS) {
        std::unique_lock<std::mutex> lock(spectrumMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (int i = 0; i < numFrames; i++) {
                spectrumBuffer[spectrumWriteIdx] = output[i * channelCount];
                spectrumWriteIdx = (spectrumWriteIdx + 1) % SPECTRUM_SIZE;
            }
        }
    }

    // Update peak levels for mixer meters (live-only — not needed during WAV export)
    {
        std::lock_guard<std::mutex> lock(peakMutex);

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

void AudioEngine::setSoundfontEnvelopeOverride(int instrumentId, int atk, int dec, int sus, int rel) {
    if (instrumentId < 0 || instrumentId >= 256) return;
    SfEnvOverride& o = sfEnvOverrides[instrumentId];
    o.atk = atk; o.dec = dec; o.sus = sus; o.rel = rel;
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
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        if (sfVoices[t].sfSlot == slot) sfVoices[t].detach();
    }
    std::lock_guard<std::mutex> sfLock(soundfonts[slot].mutex);
    if (soundfonts[slot].handle) {
        tsf_close(soundfonts[slot].handle);
        soundfonts[slot].handle = nullptr;
    }
    soundfonts[slot].instrumentId = -1;
    soundfonts[slot].filePath.clear();
}

int AudioEngine::loadSoundfont(int instrumentId, const char* path) {
    if (!path) return -1;

    // De-dup: this exact file already loaded reuses its slot instead of a second copy. Multiple
    // instruments share one handle — they play on distinct MIDI channels (= tracks) and apply their
    // ADSR override per-note in fireArmedNote, so per-instrument state stays isolated. Frees stay
    // reference-guarded (setInstrumentType / clearAllSoundfonts).
    for (int i = 0; i < MAX_SOUNDFONTS; i++) {
        if (soundfonts[i].handle != nullptr && soundfonts[i].filePath == path) {
            soundfonts[i].lastUsed.store(nextSfUseTick(), std::memory_order_relaxed);
            LOGD("🎹 Reusing soundfont slot %d (de-dup): %s", i, path);
            return i;
        }
    }

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

    // Parse the SF2 into a single master TSF handle. All tracks share it via MIDI channels — no
    // per-track clones, which would cost 8× the file size in RAM and stall the audio callback.
    //
    // ⚠️ **THE PARSE HAPPENS OUTSIDE THE SLOT MUTEX, and that is the point of the local.**
    // `tsf_load` reads and allocates a whole SF2 — tens to hundreds of milliseconds — and the audio
    // thread takes this same mutex two or three times per active SoundFont voice per block. Holding
    // it across the parse makes a load and a dropout the same event. The mutex is taken only to
    // PUBLISH the finished pointer, which is a store.
    //
    // `tsf_load` over a `FILE*` rather than `tsf_load_filename`, so the open goes through pt_fopen
    // like every other one. It is the same stream tsf builds for itself in `tsf_load_filename` —
    // sequential reads and forward skips only, so the SF2 still streams and peak RAM is the parsed
    // soundfont, not the file on top of it.
    FILE* sf = pt_fopen(path, "rb");
    if (!sf) {
        LOGE("❌ Cannot open soundfont: %s", path);
        return -1;
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
        if (sf_memory_guard_tripped()) {
            LOGE("❌ Soundfont too large for this device (%lld MB free): %s",
                 (long long)(pt::available_memory_bytes() >> 20), path);
            lastLoadFailure_ = LoadFailure::OUT_OF_MEMORY;
        } else {
            LOGE("❌ Failed to parse soundfont: %s", path);
            lastLoadFailure_ = LoadFailure::PARSE;
        }
        return -1;
    }
    // Configured before publication, for the same reason: a voice that sees the handle must see it
    // ready. `tsf_set_output` is not a read the audio thread can be racing, because nothing else has
    // the pointer yet.
    tsf_set_output(loaded, TSF_STEREO_INTERLEAVED, getSampleRate(), 0.0f);

    std::lock_guard<std::mutex> sfLock(soundfonts[slot].mutex);
    soundfonts[slot].handle = loaded;
    soundfonts[slot].instrumentId = instrumentId;
    soundfonts[slot].filePath = path;
    soundfonts[slot].lastUsed.store(nextSfUseTick(), std::memory_order_relaxed);  // freshly loaded = newest
    lastLoadFailure_ = LoadFailure::NONE;
    LOGD("🎹 Loaded soundfont slot %d: %s (instrumentId=%d)", slot, path, instrumentId);
    return slot;
}

void AudioEngine::unloadSoundfont(int slot) {
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return;
    freeSoundfontSlot(slot);
    LOGD("🎹 Unloaded soundfont slot %d", slot);
}

void AudioEngine::clearAllSoundfonts() {
    // Free EVERY slot — called when the project changes (NEW / load). The cache otherwise only
    // reclaims a slot on LRU eviction (one more distinct SF2 than there are slots), so a loaded SF2's
    // float samples (≈2× its file size) would stay resident across NEW/load.
    for (int s = 0; s < MAX_SOUNDFONTS; s++) freeSoundfontSlot(s);
    LOGD("🎹 Cleared all soundfont slots");
}

std::string AudioEngine::getSoundfontPresetName(int slot, int bank, int preset) {
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return "---";
    std::lock_guard<std::mutex> sfLock(soundfonts[slot].mutex);
    tsf* h = soundfonts[slot].handle;
    if (!h) return "---";
    const char* name = tsf_bank_get_presetname(h, bank, preset);
    return name ? std::string(name) : std::string("---");
}

bool AudioEngine::getSoundfontPresetAt(int slot, int index, int* bank, int* presetNumber) {
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return false;
    std::lock_guard<std::mutex> sfLock(soundfonts[slot].mutex);
    tsf* h = soundfonts[slot].handle;
    return h && tsf_get_preset_at(h, index, bank, presetNumber);
}

int AudioEngine::getSoundfontPresetCount(int slot) {
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return 0;
    std::lock_guard<std::mutex> sfLock(soundfonts[slot].mutex);
    tsf* h = soundfonts[slot].handle;
    return h ? tsf_get_presetcount(h) : 0;
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

    std::lock_guard<std::mutex> lock(tableMutex);
    Table& table = tables[tableId];

    for (int row = 0; row < 16; row++) {
        int offset = row * 8;
        table.rows[row].transpose = (int8_t)rowData[offset + 0];
        table.rows[row].volume = rowData[offset + 1];
        table.rows[row].fx1Type = rowData[offset + 2];
        table.rows[row].fx1Value = rowData[offset + 3];
        table.rows[row].fx2Type = rowData[offset + 4];
        table.rows[row].fx2Value = rowData[offset + 5];
        table.rows[row].fx3Type = rowData[offset + 6];
        table.rows[row].fx3Value = rowData[offset + 7];
    }
    table.loaded = true;

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

// ⚠️ A column that has executed `HOP FF` reads −1, the same "no position" the whole call answers with
// — the marker for that column disappears while its neighbours keep moving, which is the only honest
// drawing of a table with one column stopped.
static void lanesOf(const TableLane (&lanes)[TABLE_LANES], int out[TABLE_LANES]) {
    for (int l = 0; l < TABLE_LANES; ++l) out[l] = lanes[l].active ? lanes[l].row : -1;
}

void AudioEngine::getVoiceTableRows(int trackId, int out[TABLE_LANES]) {
    for (int l = 0; l < TABLE_LANES; ++l) out[l] = -1;

    const int live = findTrackVoice(voices, trackId, /*fading=*/false);
    if (live >= 0) { lanesOf(voices[live].lanes, out); return; }

    if (trackId >= 0 && trackId < SF_VOICE_COUNT) {
        const SoundfontVoice& sv = sfVoices[trackId];
        if (sv.isActive && sv.tableId >= 0) { lanesOf(sv.lanes, out); return; }
        const Tic00Cursor& c = tic00Cursor[trackId];
        if (c.tableId >= 0) {
            for (int l = 0; l < TABLE_LANES; ++l) out[l] = c.active[l] ? c.row[l] : -1;
            return;
        }
    }
    const int fading = findTrackVoice(voices, trackId, /*fading=*/true);
    if (fading >= 0) lanesOf(voices[fading].lanes, out);
}

int AudioEngine::getVoiceTableId(int trackId) {
    const int live = findTrackVoice(voices, trackId, /*fading=*/false);
    if (live >= 0) return voices[live].tableId;

    if (trackId >= 0 && trackId < SF_VOICE_COUNT) {
        const SoundfontVoice& sv = sfVoices[trackId];
        if (sv.isActive) return sv.tableId;
        if (tic00Cursor[trackId].tableId >= 0) return tic00Cursor[trackId].tableId;
    }
    const int fading = findTrackVoice(voices, trackId, /*fading=*/true);
    return fading >= 0 ? voices[fading].tableId : -1;
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
static void computeSpectrumFFT(kiss_fft_scalar* input, int numBins, float* out, float sampleRate) {
    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) {
        float w = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (SPECTRUM_FFT_SIZE - 1)));
        input[i] *= w;
    }

    // Cache the config across calls: kiss_fftr_alloc does a malloc + twiddle-table trig init
    // every time. All callers are the single UI poll thread (~20 fps while the EQ screen is open),
    // so a function-local static is safe and removes that per-call churn. FFT size is constant,
    // so the cfg lives for the process (never freed).
    static kiss_fftr_cfg cfg = kiss_fftr_alloc(SPECTRUM_FFT_SIZE, 0, nullptr, nullptr);
    kiss_fft_cpx cpx_out[SPECTRUM_FFT_SIZE / 2 + 1];
    kiss_fftr(cfg, input, cpx_out);

    const float fMin = 20.0f, fMax = 20000.0f;
    const float logRange = logf(fMax / fMin);

    for (int bi = 0; bi < numBins; bi++) {
        float t    = (float)bi / (numBins - 1);
        float freq = fMin * expf(t * logRange);
        int bin    = (int)(freq * SPECTRUM_FFT_SIZE / sampleRate + 0.5f);
        if (bin < 1)                      bin = 1;
        if (bin >= SPECTRUM_FFT_SIZE / 2) bin = SPECTRUM_FFT_SIZE / 2 - 1;

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
// setters are the helpers plus a log line, and the queue arms call the helpers directly — one copy of
// what "set this fader" means, rather than the SF-cache rule written out twice.
void AudioEngine::applyTrackVolume(int trackId, float volume) {
    if (trackId < 0 || trackId >= 8) return;
    { std::lock_guard<std::mutex> lock(volumeMutex); trackVolumes[trackId] = volume; }
    SoundfontVoice& sv = sfVoices[trackId];
    sv.trackVolume = volume;
    int slot = sv.sfSlot;  // snapshot once — detach() can set the member to -1 concurrently
    if (sv.isActive && slot >= 0 && slot < MAX_SOUNDFONTS) {
        std::lock_guard<std::mutex> sfLock(soundfonts[slot].mutex);
        tsf* h = soundfonts[slot].handle;
        if (h) tsf_channel_set_volume(h, trackId, sv.noteVolume * volume);
    }
}

void AudioEngine::applyMasterVolume(float volume) {
    std::lock_guard<std::mutex> lock(volumeMutex);
    masterVolume = volume;
}

void AudioEngine::setTrackVolume(int trackId, float volume) {
    if (trackId < 0 || trackId >= 8) return;
    applyTrackVolume(trackId, volume);
    LOGD("🔊 Track %d volume set to %.2f", trackId, volume);
}

void AudioEngine::setTrackMuted(int trackId, bool muted) {
    if (trackId < 0 || trackId >= 8) return;
    { std::lock_guard<std::mutex> lock(volumeMutex); trackMuted[trackId] = muted; }
    // Nothing else is needed to silence what is ringing: the next block picks the new target up and
    // both mix paths walk their gate to it over MUTE_GATE_SAMPLES, so a mute lands in ~5.8 ms rather
    // than in one sample. Voices keep running underneath — a mute is a gate, never a stop.
    LOGD("🔇 Track %d %s", trackId, muted ? "muted" : "unmuted");
}

void AudioEngine::setMasterVolume(float volume) {
    applyMasterVolume(volume);
    LOGD("🔊 Master volume set to %.2f", volume);
}

void AudioEngine::setPreviewTrack(int trackId) {
    std::lock_guard<std::mutex> lock(volumeMutex);
    previewLaneTrack = (trackId >= 0 && trackId < 8) ? trackId : -1;
}

// The sample-accurate faces of the same two faders — what a VTR / VMV effect schedules. VMV is global
// and carries no track, so it borrows the queue's `trackId` field as -1 the way EQM does.
void AudioEngine::scheduleTrackVolume(int64_t targetFrame, int trackId, float volume) {
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, volume, PARAM_UPDATE_TRACK_VOL, 0.0f });
}

void AudioEngine::scheduleMasterVolume(int64_t targetFrame, float volume) {
    paramUpdateQueue.schedule({ targetFrame, -1, 0, volume, PARAM_UPDATE_MASTER_VOL, 0.0f });
}

void AudioEngine::setOttDepth(int depth) {
    float d = depth / 255.0f;
    masterChain.ott.setDepth(d);
}

void AudioEngine::setOttDepthForRender(int depth) {
    float d = depth / 255.0f;
    masterChain.ott.resetForRender(d);
}

void AudioEngine::setMasterFx(int fx) {
    masterChain.setMasterFx(fx);
}

void AudioEngine::setDustDepth(int depth) {
    masterChain.setDustDepth(depth / 255.0f);
}

void AudioEngine::setDustDepthForRender(int depth) {
    masterChain.setDustDepthForRender(depth / 255.0f);
}

void AudioEngine::setLimiterPreGain(int depth) {
    masterChain.setLimiterPreGain(1.0f + (depth / 255.0f) * 3.0f);
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
    InstrumentModSlot& slot = instrumentModSlots[sampleId][slotIndex];
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

void AudioEngine::triggerNoteOff(int trackId) {
    // The release-vs-fade decision lives in Voice::noteOff — one implementation.
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].trackId == trackId) voices[v].noteOff();
    }
}

void AudioEngine::triggerKeyRelease(int trackId) {
    // …and the key-release decision lives in Voice::keyRelease, for the same reason: this loop is
    // allocation, not policy. The two differ in exactly one arm (see sampler-voice.h).
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].trackId == trackId) voices[v].keyRelease();
    }
}

void AudioEngine::clearInstrumentModulation(int sampleId) {
    if (sampleId < 0 || sampleId >= 256) return;
    for (int m = 0; m < 4; m++) {
        instrumentModSlots[sampleId][m] = InstrumentModSlot();
    }
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
    // voice.playbackRate has no transpose baked in; arpeggio adjusts it via setMidiNote().
    float rateMod = powf(2.0f, voice.modDestValues[PARAM_PITCH] / 12.0f);
    return voice.playbackRate * rateMod;
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
    // Everything above is now at FACTORY DEFAULTS, not at the project's values — the caller re-pushes.
    LOGD("🎬 Effect chains reset to clean state (caller must re-push the project's FX)");
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
