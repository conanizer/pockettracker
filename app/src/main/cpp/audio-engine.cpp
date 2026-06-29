// TSF API declarations only — TSF_IMPLEMENTATION lives in soundfont-voice.cpp
#include "audio-engine.h"
#include "kissfft/kiss_fftr.h"
#include "vendor/tsf/tsf.h"
#include "mods/mod-runner.h"
#include "mods/modules/pitch-slide-module.h"
#include "mods/modules/vibrato-module.h"
#include "effects/primitives/sola-stretch.h"
#include "audio-decoders.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <new>

// Definition of the per-track soundfont voice array (extern declared in audio-engine.h)
SoundfontVoice sfVoices[8];

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
    globalFrameCounter.store(0, std::memory_order_relaxed);

    // Pre-size the per-block drain buffers so the audio thread never reallocates them at
    // runtime. A single ~23 ms block only ever holds a handful of events (a few tracks × retrigs).
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
    reverbSend.reset(44100.0f);
    delaySend.reset(44100.0f);
    masterChain.reset();
}

AudioEngine::~AudioEngine() {
    // The platform backend (OboeAudioEngine) owns and closes the output stream; the core just frees
    // its buffers. jni-bridge destroys the shell first so no callback can run during this teardown.
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

void AudioEngine::loadSample(int id, const float* data, int length) {
    if (id < 0 || id >= 256) return;

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
    // New file replaces the original — discard any cached rate-mode original.
    if (originalSamples[id]) {
        delete[] originalSamples[id];
        originalSamples[id] = nullptr;
        originalSampleLengths[id] = 0;
    }
    if (originalSamplesRight[id]) { delete[] originalSamplesRight[id]; originalSamplesRight[id] = nullptr; }

    samples[id] = new float[length];
    for (int i = 0; i < length; i++) {
        samples[id][i] = data[i];
    }
    sampleLengths[id] = length;

    LOGD("Sample %d: %d frames (mono)", id, length);
}

void AudioEngine::loadSampleStereo(int id, const float* left, const float* right, int length) {
    if (id < 0 || id >= 256 || !left || !right) return;

    std::lock_guard<std::mutex> lock(sampleEditMutex);

    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].instrId == id && voices[i].isActive) voices[i].stop();
    }

    if (samples[id]) delete[] samples[id];
    if (samplesRight[id]) { delete[] samplesRight[id]; samplesRight[id] = nullptr; }
    if (originalSamples[id]) {
        delete[] originalSamples[id];
        originalSamples[id] = nullptr;
        originalSampleLengths[id] = 0;
    }
    if (originalSamplesRight[id]) { delete[] originalSamplesRight[id]; originalSamplesRight[id] = nullptr; }

    samples[id] = new float[length];
    samplesRight[id] = new float[length];
    std::memcpy(samples[id],      left,  length * sizeof(float));
    std::memcpy(samplesRight[id], right, length * sizeof(float));
    sampleLengths[id] = length;

    LOGD("Sample %d: %d frames (stereo)", id, length);
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

    FILE* f = fopen(path, "rb");
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
    // per-slot buffer, including the undo backup that loadSample/loadSampleStereo leak on reuse
    // A fresh file makes the old sample's undo/rate caches meaningless.
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

    std::vector<float> L, R;
    int sr = 0;
    bool ok;
    if      (std::strcmp(ext, "mp3")  == 0) ok = ptdec::decodeMp3File(path, L, R, sr);
    else if (std::strcmp(ext, "flac") == 0) ok = ptdec::decodeFlacFile(path, L, R, sr);
    else if (std::strcmp(ext, "ogg")  == 0) {
        // An .ogg holds either Vorbis or Opus. Try Vorbis (stb_vorbis); on a miss, retry as Opus.
        ok = ptdec::decodeOggFile(path, L, R, sr);
        if (!ok) { L.clear(); R.clear(); ok = ptdec::decodeOpusFile(path, L, R, sr); }
    }
    else if (std::strcmp(ext, "opus") == 0) ok = ptdec::decodeOpusFile(path, L, R, sr);
    else { LOGE("loadSampleFromCompressed: unsupported extension '%s'", ext); return 0; }

    if (!ok || L.empty() || sr <= 0) {
        LOGE("loadSampleFromCompressed: decode failed (%s)", path);
        return 0;
    }

    // loadSample/loadSampleStereo free the sample + RATE caches on slot reuse but NOT the sample-editor
    // undo backup (loadSampleFromWavFile does). Clear it here too so a stale backup from the slot's
    // previous sample can't be "undone" onto this one.
    {
        std::lock_guard<std::mutex> lock(sampleEditMutex);
        delete[] sampleBackups[id];      sampleBackups[id] = nullptr;
        delete[] sampleBackupsRight[id]; sampleBackupsRight[id] = nullptr;
        sampleBackupLengths[id] = 0;
    }

    // Publish via the existing, tested slot path (voice-stop + buffer free + mutex all handled there).
    if (!R.empty() && R.size() == L.size())
        loadSampleStereo(id, L.data(), R.data(), (int)L.size());
    else
        loadSample(id, L.data(), (int)L.size());

    LOGD("loadSampleFromCompressed: id=%d %zu frames %s rate=%d (%s)",
         id, L.size(), R.empty() ? "mono" : "stereo", sr, ext);
    return sr;
}

bool AudioEngine::hasStereoData(int id) {
    if (id < 0 || id >= 256) return false;
    return samplesRight[id] != nullptr;
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

void AudioEngine::triggerNote(int sampleId, int trackId, float freq, float baseFreq, float vol, float pan) {
    if (sampleId < 0 || sampleId >= 256 || !samples[sampleId]) return;

    requestResume();

    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].trackId == trackId) {
            voices[i].stop();
        }
    }

    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].isActive) {
            float rate = freq / baseFreq;
            float sampleRate = (float)getSampleRate();
            voices[i].trigger(samples[sampleId], samplesRight[sampleId], sampleLengths[sampleId], trackId, rate, vol, 1.0f, pan,
                              instrumentParams[sampleId], sampleRate);
            voices[i].instrId = sampleId;
            LOGD("Note: track=%d, sampleId=%d, rate=%.3f, pan=%.2f", trackId, sampleId, rate, pan);
            return;
        }
    }
}

void AudioEngine::stopTrack(int trackId) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].trackId == trackId && voices[i].isActive) {
            voices[i].stop();
        }
    }
    if (trackId >= 0 && trackId < 8) {
        sfVoices[trackId].hardStop();
    }
}

void AudioEngine::stopAll() {
    for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].stop();
    }
    // Stop all soundfont notes on all tracks
    for (int t = 0; t < 8; t++) {
        sfVoices[t].hardStop();
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
}

int AudioEngine::getSampleRate() {
    // Cached from the platform backend at stream-open (setDeviceSampleRate). Defaults to 44100 until
    // then — matching every other fallback in the engine (triggerNote, setPitchSlide/Bend,
    // updateVoiceModulation, the Kotlin layer); 48000 here once made rate/pitch math ~8.8% off if
    // Kotlin cached this before the stream opened.
    return deviceSampleRate.load(std::memory_order_relaxed);
}

// ARM FZ (Flush-to-Zero) bit eliminates denormal CPU stalls. Must be set per-thread
// because FPCR/FPSCR are thread-local registers.
static void setFlushToZeroForCurrentThread() {
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
#endif
}

// ALL audio DSP lives here. processLiveBlock (live, via the platform backend's callback) and
// renderOffline (WAV export) are thin wrappers.
// Rule: NEVER add audio processing logic directly to processLiveBlock or renderOffline.
void AudioEngine::processAudioBlock(float* output, int numFrames, int channelCount, float sampleRate) {
    for (int t = 0; t < 8; t++) { framePeaksPerTrackL[t] = 0.0f; framePeaksPerTrackR[t] = 0.0f; }
    frameSendPeakRevL = frameSendPeakRevR = frameSendPeakDelL = frameSendPeakDelR = 0.0f;

    // Snapshot real-time volumes ONCE per block under a single lock, then read them lock-free in the
    // hot loops below. Previously volumeMutex was taken per-sample-per-voice (~350k locks/sec/voice) —
    // pure overhead plus a dropout hazard if the Kotlin thread held the lock during setTrackVolume/
    // setMasterVolume. One block of slightly-stale volume is inaudible.
    float trackVolSnapshot[8];
    float masterVolSnapshot;
    {
        std::lock_guard<std::mutex> lock(volumeMutex);
        for (int t = 0; t < 8; t++) trackVolSnapshot[t] = trackVolumes[t];
        masterVolSnapshot = masterVolume;
    }

    // Zero only the [0,numFrames) slice actually used (not the full MAX_BLOCK arrays), and
    // skip the expensive visualizer accumulators when nobody is watching (see CAPTURE_IDLE_MS).
    // Also skip all visualizer capture during offline WAV export: the live stream is silent so the
    // scopes already read flat, and OCTA would otherwise snapshot random mid-render frames that only
    // repaint on progress ticks (a frozen, twitching scope). Let the visualizers sit flat mid-render.
    const bool offlineRender    = isOfflineRendering.load(std::memory_order_relaxed);
    const int64_t nowMsec       = nowMs();
    const bool octaWanted       = !offlineRender && (nowMsec - lastTrackWaveformReadMs.load(std::memory_order_relaxed)) < CAPTURE_IDLE_MS;
    const bool spectrumWanted   = !offlineRender && (nowMsec - lastSpectrumReadMs.load(std::memory_order_relaxed))      < CAPTURE_IDLE_MS;
    const size_t frameBytes     = (size_t)numFrames * sizeof(float);

    // Per-block scratch (send buses, OCTA accumulators, instrument-spectrum sum) lives on the engine
    // object, not the audio-thread stack — declared in the header. (Re)initialised here every block;
    // MAX_BLOCK is the class cap and processLiveBlock/renderOffline chunk larger requests, so only
    // [0,numFrames) is ever touched.
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

    for (int32_t frame = 0; frame < numFrames; frame++) {
        int64_t currentFrame = blockStartFrame + frame;

        // Apply scheduled parameter updates at their exact frame. Running here (on the audio
        // thread) is what makes live PBN/PVB/PVX/THO race-free: the look-ahead scheduler only
        // enqueues; the voices[] mutation happens below, where the mix loop is the sole writer.
        while (paramIdx < paramBatch.size() && paramBatch[paramIdx].targetFrame <= currentFrame) {
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
                            voices[v].tableRow = (int)upd.value % 16;
                            voices[v].lastProcessedRow = -1;  // re-apply the row immediately
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
                    if (upd.trackId >= 0 && upd.trackId < 8 && sfVoices[upd.trackId].isActive)
                        sfVoices[upd.trackId].instrParams.reverbSend = upd.value;
                    break;
                }
                case PARAM_UPDATE_DELAY_SEND: {           // DEL — per-note delay send
                    for (int v = 0; v < MAX_VOICES; v++)
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId)
                            voices[v].delaySend = upd.value;
                    if (upd.trackId >= 0 && upd.trackId < 8 && sfVoices[upd.trackId].isActive)
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
                                vo.position = rev ? (float)(vo.actualEnd > vo.actualStart ? vo.actualEnd - 1 : vo.actualStart)
                                                  : (float)vo.actualStart;
                            }
                            break;
                        }
                    }
                    break;
                }
                case PARAM_UPDATE_EQ_SLOT: {              // EQN — per-note EQ preset
                    int slot = (int)upd.value;
                    for (int v = 0; v < MAX_VOICES; v++)
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId) {
                            applyEqPresetToChain(voices[v].chain, slot); break;
                        }
                    if (upd.trackId >= 0 && upd.trackId < 8 && sfVoices[upd.trackId].isActive)
                        applyEqPresetToChain(sfVoices[upd.trackId].chain, slot);
                    break;
                }
                case PARAM_UPDATE_MASTER_EQ: {            // EQM — master/mixer EQ preset (global)
                    setMasterEqSlot((int)upd.value);
                    break;
                }
                default: {                                // PARAM_UPDATE_MOD_SOURCE — Vxx phraseVol
                    for (int v = 0; v < MAX_VOICES; v++) {
                        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId) {
                            voices[v].modSourceValues[(ModSourceId)upd.sourceId] = upd.value;
                            break;
                        }
                    }
                    if (upd.trackId >= 0 && upd.trackId < 8 && sfVoices[upd.trackId].isActive) {
                        sfVoices[upd.trackId].modSourceValues[(ModSourceId)upd.sourceId] = upd.value;
                    }
                    break;
                }
            }
        }

        // Process all scheduled kill events for this exact frame (BEFORE notes)
        while (killIdx < killBatch.size() && killBatch[killIdx].targetFrame <= currentFrame) {
            ScheduledKill kill = killBatch[killIdx++];
            if (kill.softKill) {
                triggerNoteOff(kill.trackId);  // Sampler: trigger ADSR release
                // SF: noteOff (TSF handles its own release envelope internally)
                if (kill.trackId >= 0 && kill.trackId < 8) {
                    sfVoices[kill.trackId].noteOff();
                }
                LOGT("🎵 Note-off: track %d at frame %lld", kill.trackId, (long long)currentFrame);
            } else {
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == kill.trackId && voices[v].isActive) {
                        voices[v].stop();
                        LOGT("🔪 Killed track %d at frame %lld", kill.trackId, (long long)currentFrame);
                    }
                }
                // SF: soft kill so TSF's internal release envelope can play out.
                if (kill.trackId >= 0 && kill.trackId < 8) {
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
                if (t >= 0 && t < 8 &&
                    note.sfSlot >= 0 && note.sfSlot < MAX_SOUNDFONTS &&
                    soundfonts[note.sfSlot].handle != nullptr) {

                    SoundfontVoice& sv = sfVoices[t];
                    float trkVol = trackVolSnapshot[t];
                    soundfonts[note.sfSlot].lastUsed.store(nextSfUseTick(), std::memory_order_relaxed);  // LRU touch
                    // This instrument's ADSR override (applied atomically inside triggerNote, before
                    // note_on) — keyed by instrument id so de-duplicated handles stay isolated.
                    int eAtk = -1, eDec = -1, eSus = -1, eRel = -1;
                    if (note.sampleId >= 0 && note.sampleId < 256) {
                        const SfEnvOverride& eo = sfEnvOverrides[note.sampleId];
                        eAtk = eo.atk; eDec = eo.dec; eSus = eo.sus; eRel = eo.rel;
                    }
                    sv.triggerNote(note.sfSlot, note.midiNote, note.midiVelocity,
                                   note.volume, trkVol, note.pan, note.sfBank, note.sfPreset, t,
                                   eAtk, eDec, eSus, eRel);
                    sv.isReleasingOnly = false;
                    sv.resetPitchState();
                    sv.detuneSemitones = note.detuneSemitones;  // static instrument detune (set after reset)

                    // M8-style: honour TIC effect in table's last row (same as sampler path).
                    int effectiveTicRate = note.tableTicRate;
                    if (note.tableId >= 0 && note.tableId < 256) {
                        std::lock_guard<std::mutex> lock(tableMutex);
                        if (tables[note.tableId].loaded) {
                            const TableRow& lastRow = tables[note.tableId].rows[15];
                            auto checkTic = [](uint8_t fxType, uint8_t fxValue) -> int {
                                return (fxType == FX_TIC) ? fxValue : -1;
                            };
                            int t1 = checkTic(lastRow.fx1Type, lastRow.fx1Value);
                            int t2 = checkTic(lastRow.fx2Type, lastRow.fx2Value);
                            int t3 = checkTic(lastRow.fx3Type, lastRow.fx3Value);
                            if      (t1 >= 0) effectiveTicRate = t1;
                            else if (t2 >= 0) effectiveTicRate = t2;
                            else if (t3 >= 0) effectiveTicRate = t3;
                        }
                    }
                    sv.resetTableState(note.tableId, effectiveTicRate,
                                       note.noteOctave, note.notePitch, note.tableStartRow);

                    // Only valid when sampleId >= 0 (phrase playback); previews pass -1.
                    if (note.sampleId >= 0 && note.sampleId < 256) {
                        sv.instrParams = instrumentParams[note.sampleId];
                        for (int m = 0; m < 4; m++) {
                            const InstrumentModSlot& src = instrumentModSlots[note.sampleId][m];
                            VoiceModSlot& dst = sv.voiceMods[m];
                            dst.type = src.type;
                            dst.dest = src.dest;
                            dst.amount = src.amount;
                            dst.attackSamples = src.attackSamples;
                            dst.holdSamples = src.holdSamples;
                            dst.decaySamples = src.decaySamples;
                            dst.sustainLevel = src.sustainLevel;
                            dst.lfoHz = src.lfoHz;
                            dst.oscShape = src.oscShape;
                            dst.lfoPhase = 0.0f;
                            dst.releaseSamples = src.releaseSamples;
                            dst.effectiveAmt = src.amount;
                            dst.effectiveRateMult = 1.0f;
                            dst.prevEnvValue = 0.0f;
                            if (src.type != 0) { dst.stage = 1; dst.envValue = 0.0f; dst.stageCounter = 0; }
                            else               { dst.stage = 0; dst.envValue = 0.0f; dst.stageCounter = 0; }
                        }
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
                    LOGT("🎹 SF DROPPED: sfSlot=%d track=%d (handle not loaded)", note.sfSlot, note.trackId);
                }
                continue;  // Skip voice pool processing
            }
            // ---- END SOUNDFONT PATH ----

            // TIC00 support: Check if previous voice on this track was using trigger mode
            int savedTableRow = 0;
            bool wasTIC00Mode = false;
            for (int v = 0; v < MAX_VOICES; v++) {
                if (voices[v].trackId == note.trackId && voices[v].isActive && !voices[v].isFadingOut) {
                    if (voices[v].tableTicRate == 0x00 && voices[v].tableId >= 0) {
                        wasTIC00Mode = true;
                        savedTableRow = (voices[v].tableRow + 1) % 16;
                        LOGT("📋 TIC00: Saving table row %d for track %d retrigger", savedTableRow, note.trackId);
                    }
                }
            }

            // ---------------------------------------------------------------
            // VOICE ALLOCATION — 3-step priority
            //
            // Problem: "steal old + allocate new" temporarily consumes two
            // slots per track.  When N tracks all trigger at the same frame
            // (phrase boundaries) this exhausts the 8-slot pool even with
            // only 5 active tracks.
            //
            // Step 1 — recycle fading same-track voice (0 extra slots used).
            //           trackId is preserved through startFadeOut() so we can
            //           find and reuse the slot immediately.
            // Step 2 — normal steal: fade old same-track voice, find free slot.
            // Step 3 — last resort: preempt any fading voice (other track).
            //           Produces at most a ~1ms click but prevents silence.
            // ---------------------------------------------------------------

            // Step 1: same-track fading voice → recycle directly
            int targetSlot = -1;
            for (int v = 0; v < MAX_VOICES; v++) {
                if (voices[v].trackId == note.trackId && voices[v].isFadingOut) {
                    targetSlot = v;
                    break;
                }
            }

            // Step 2: steal non-fading same-track voice, then find free slot
            if (targetSlot == -1) {
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == note.trackId && voices[v].isActive && !voices[v].isFadingOut) {
                        voices[v].startFadeOut();
                    }
                }
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (!voices[v].isActive) {
                        targetSlot = v;
                        break;
                    }
                }
            }

            // Step 3: preempt any fading voice (last resort)
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
                    float rate = note.frequency / note.baseFrequency;

                    int effectiveTicRate = note.tableTicRate;
                    if (note.tableId >= 0 && note.tableId < 256) {
                        std::lock_guard<std::mutex> lock(tableMutex);
                        if (tables[note.tableId].loaded) {
                            TableRow& lastRow = tables[note.tableId].rows[15];
                            auto checkTic = [](uint8_t fxType, uint8_t fxValue) -> int {
                                if (fxType == FX_TIC) return fxValue;
                                return -1;
                            };
                            int tic1 = checkTic(lastRow.fx1Type, lastRow.fx1Value);
                            int tic2 = checkTic(lastRow.fx2Type, lastRow.fx2Value);
                            int tic3 = checkTic(lastRow.fx3Type, lastRow.fx3Value);
                            if (tic1 >= 0) {
                                effectiveTicRate = tic1;
                                LOGT("📋 M8-style: Using TIC %02X from table %d last row", effectiveTicRate, note.tableId);
                            } else if (tic2 >= 0) {
                                effectiveTicRate = tic2;
                                LOGT("📋 M8-style: Using TIC %02X from table %d last row", effectiveTicRate, note.tableId);
                            } else if (tic3 >= 0) {
                                effectiveTicRate = tic3;
                                LOGT("📋 M8-style: Using TIC %02X from table %d last row", effectiveTicRate, note.tableId);
                            }
                        }
                    }

                    int startRow;
                    if (note.tableStartRow >= 0) {
                        startRow = note.tableStartRow % 16;
                    } else if (wasTIC00Mode && effectiveTicRate == 0x00) {
                        startRow = savedTableRow;
                    } else {
                        startRow = 0;
                    }

                    voices[v].trigger(samples[note.sampleId], samplesRight[note.sampleId], sampleLengths[note.sampleId],
                                      note.trackId, rate, note.volume, note.phraseVolume, note.pan, instrumentParams[note.sampleId],
                                      sampleRate, note.startPointOverride, note.endPointOverride,
                                      note.tableId, effectiveTicRate, note.noteOctave, note.notePitch, startRow);
                    voices[v].instrId = note.sampleId;

                    // pslDuration is already in audio frames (converted by AudioEngine.kt).
                    if (fabsf(note.pslInitialOffset) > 0.001f && note.pslDuration > 0.0f) {
                        voices[v].pitchOffset = note.pslInitialOffset;
                        float totalFrames = fmaxf(1.0f, note.pslDuration);
                        voices[v].pitchSlideTarget = 0.0f;
                        voices[v].pitchSlideRate = -note.pslInitialOffset / totalFrames;
                        voices[v].pitchSliding = true;
                        LOGT("🎵 PSL applied: offset=%.2f, duration=%.0f ticks, rate=%.6f",
                             note.pslInitialOffset, note.pslDuration, voices[v].pitchSlideRate);
                    }
                    // pbnRate is already in semitones/frame (converted by AudioEngine.kt).
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

                    for (int m = 0; m < 4; m++) {
                        const InstrumentModSlot& src = instrumentModSlots[note.sampleId][m];
                        VoiceModSlot& dst = voices[v].voiceMods[m];
                        dst.type = src.type;
                        dst.dest = src.dest;
                        dst.amount = src.amount;
                        dst.attackSamples = src.attackSamples;
                        dst.holdSamples = src.holdSamples;
                        dst.decaySamples = src.decaySamples;
                        dst.sustainLevel = src.sustainLevel;
                        dst.lfoHz = src.lfoHz;
                        dst.oscShape = src.oscShape;
                        dst.lfoPhase = 0.0f;
                        dst.releaseSamples = src.releaseSamples;
                        dst.effectiveAmt = src.amount;
                        dst.effectiveRateMult = 1.0f;
                        dst.prevEnvValue = 0.0f;
                        if (src.type != 0) {
                            dst.stage = 1;
                            dst.envValue = 0.0f;
                            dst.stageCounter = 0;
                        } else {
                            dst.stage = 0;
                            dst.envValue = 0.0f;
                            dst.stageCounter = 0;
                        }
                    }

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

    // Special TIC modes:
    //   TIC00 (0x00): Trigger mode — table row set by note, doesn't advance automatically
    //   TICFC (0xFC): Octave map — row = triggered note's octave (0-9)
    //   TICFE (0xFE): Note map — row = triggered note's pitch (0-11)
    //   TICFF (0xFF): 200Hz mode — advance ~1 row per 5ms
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice& voice = voices[v];
        if (!voice.isActive || voice.tableId < 0) continue;

        bool tableLoaded = false;
        {
            std::lock_guard<std::mutex> lock(tableMutex);
            tableLoaded = tables[voice.tableId].loaded;
        }
        if (!tableLoaded) continue;

        bool shouldProcessRow = false;
        bool shouldAdvance = false;

        if (voice.tableTicRate == 0x00) {
            // TIC00: Trigger mode - apply row effects ONCE, don't advance automatically
            shouldProcessRow = (voice.tableRow != voice.lastProcessedRow);
            shouldAdvance = false;
        } else if (voice.tableTicRate == 0xFC || voice.tableTicRate == 0xFE) {
            // TICFC/TICFE: Static mapping modes - row is fixed, process ONCE
            shouldProcessRow = (voice.tableRow != voice.lastProcessedRow);
            shouldAdvance = false;
        } else if (voice.tableTicRate == 0xFF) {
            // TICFF: 200Hz mode - faster advancement
            voice.tic200HzAccum += numFrames;
            float samplesPerTic = sampleRate / 200.0f;
            if (voice.tic200HzAccum >= samplesPerTic) {
                voice.tic200HzAccum -= samplesPerTic;
                shouldProcessRow = true;
                shouldAdvance = true;
            }
        } else {
            // Standard tic mode (01-FB): advance one row every `tableTicRate` musical tics.
            // Frame-accurate and tempo-locked (like the TICFF branch above) so table speed tracks
            // the sequencer, is identical live vs. offline render, and is independent of the audio
            // buffer size. framesPerTic = sr / (BPM/60 · 4 steps/beat · 12 tics/step).
            if (voice.lastProcessedRow == -1) {
                // Fire the first tic AT trigger so row 0's transpose/vol/FX apply immediately.
                // Otherwise the voice plays one full row-duration with no table effect, which sounds
                // like the first row lasts twice as long. Mirrors TIC00's note-on processing.
                voice.tableFrameAccum = 0.0f;
                shouldProcessRow = true;
                shouldAdvance = true;
            } else {
                int tempo = currentTempo.load(std::memory_order_relaxed);
                float framesPerRow = sampleRate / (tempo / 60.0f * 4.0f * 12.0f) * (float)voice.tableTicRate;
                voice.tableFrameAccum += numFrames;
                if (framesPerRow > 0.0f && voice.tableFrameAccum >= framesPerRow) {
                    voice.tableFrameAccum -= framesPerRow;
                    // A block longer than one row (very fast tables) can't advance >1 row here, so
                    // drop the extra rather than banking it (which would run away). Normal tic rates
                    // have framesPerRow >> block, so the remainder carries and the rate stays exact.
                    if (voice.tableFrameAccum >= framesPerRow) voice.tableFrameAccum = 0.0f;
                    shouldProcessRow = true;
                    shouldAdvance = true;
                }
            }
        }

        if (shouldProcessRow) {
            TableRow row;
            {
                std::lock_guard<std::mutex> lock(tableMutex);
                row = tables[voice.tableId].rows[voice.tableRow];
            }

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

            bool hopExecuted = false;
            int hopTarget = -1;

            auto processEffect = [&](uint8_t fxType, uint8_t fxValue) {
                switch (fxType) {
                    case FX_KILL:
                        if (fxValue == 0x00) {
                            voice.isActive = false;
                            LOGT("📋 Table effect: KILL voice %d", v);
                        }
                        break;

                    case FX_HOP:
                        // HOP XY: X=repeat count (0=infinite), Y=target row; FF=stop table
                        if (fxValue == 0xFF) {
                            voice.tableId = -1;
                            voice.hopTargetRow = -1;
                            voice.hopRepeatCount = 0;
                            LOGT("📋 Table HOP FF: stopped table for voice %d", v);
                        } else {
                            int repeatCount = (fxValue >> 4) & 0x0F;  // High nibble = X
                            int targetRow = fxValue & 0x0F;           // Low nibble = Y

                            if (repeatCount == 0) {
                                // HOP 0Y = Infinite loop to row Y
                                hopExecuted = true;
                                hopTarget = targetRow;
                                LOGT("📋 Table HOP %02X: infinite loop to row %d, voice %d", fxValue, targetRow, v);
                            } else {
                                // HOP XY (X>0) = Jump X times, then continue
                                if (voice.hopTargetRow == -1 || voice.hopTargetRow != targetRow) {
                                    voice.hopRepeatCount = repeatCount;
                                    voice.hopTargetRow = targetRow;
                                    LOGT("📋 Table HOP %02X: initialized counter=%d, target=%d, voice %d",
                                         fxValue, repeatCount, targetRow, v);
                                }

                                if (voice.hopRepeatCount > 0) {
                                    voice.hopRepeatCount--;
                                    hopExecuted = true;
                                    hopTarget = targetRow;
                                    LOGT("📋 Table HOP: jump to row %d, %d jumps remaining, voice %d",
                                         targetRow, voice.hopRepeatCount, v);
                                } else {
                                    // Counter exhausted, don't jump, reset state and continue normally
                                    voice.hopTargetRow = -1;
                                    LOGT("📋 Table HOP: counter exhausted, continuing past row, voice %d", v);
                                }
                            }
                        }
                        break;

                    case FX_VOLUME:
                        voice.tableVolume = fxValue / 255.0f;
                        voice.modSourceValues[MOD_SRC_TABLE_VOL] = voice.tableVolume;
                        break;

                    case FX_OFFSET:
                        if (voice.sampleLength > 0) {
                            float normalizedPos = fxValue / 255.0f;
                            voice.position = normalizedPos * (voice.sampleLength - 1);
                        }
                        break;

                    case FX_TIC:
                        if (fxValue >= 0x01 && fxValue <= 0xFB) {
                            voice.tableTicRate = fxValue;
                            voice.tableTicCounter = 0;
                            LOGT("📋 Table effect: TIC %02X - set tick rate to %d", fxValue, fxValue);
                        }
                        break;

                    case FX_THO:
                        hopExecuted = true;
                        hopTarget = fxValue & 0x0F;
                        LOGT("📋 Table THO %02X: hop to row %d, voice %d", fxValue, hopTarget, v);
                        break;

                    default:
                        break;
                }
            };

            processEffect(row.fx1Type, row.fx1Value);
            processEffect(row.fx2Type, row.fx2Value);
            processEffect(row.fx3Type, row.fx3Value);

            voice.lastProcessedRow = voice.tableRow;

            if (hopExecuted && hopTarget >= 0) {
                voice.tableRow = hopTarget % 16;
                LOGT("📋 Table HOP: voice %d jumped to row %d", v, voice.tableRow);
            } else if (shouldAdvance) {
                voice.tableRow = (voice.tableRow + 1) % 16;
            }

            if (shouldAdvance && voice.tableRow == 0) {
                LOGT("📋 Table %d loop: voice=%d, transpose=%.0f, vol=%.2f",
                     voice.tableId, v, voice.tableTranspose, voice.tableVolume);
            }
        }
    }

    // SF table processing: mirrors sampler loop above. Effects writing to modSourceValues[]
    // are picked up by updateVoiceModulation/applyPitchMod. FX_OFFSET silently skipped.
    for (int t = 0; t < 8; t++) {
        SoundfontVoice& sv = sfVoices[t];
        if (!sv.isActive || sv.tableId < 0) continue;

        bool tableLoaded = false;
        {
            std::lock_guard<std::mutex> lock(tableMutex);
            tableLoaded = tables[sv.tableId].loaded;
        }
        if (!tableLoaded) continue;

        bool shouldProcessRow = false;
        bool shouldAdvance    = false;

        if (sv.tableTicRate == 0x00) {
            shouldProcessRow = (sv.tableRow != sv.lastProcessedRow);
            shouldAdvance    = false;
        } else if (sv.tableTicRate == 0xFC || sv.tableTicRate == 0xFE) {
            shouldProcessRow = (sv.tableRow != sv.lastProcessedRow);
            shouldAdvance    = false;
        } else if (sv.tableTicRate == 0xFF) {
            sv.tic200HzAccum += numFrames;
            float samplesPerTic = sampleRate / 200.0f;
            if (sv.tic200HzAccum >= samplesPerTic) {
                sv.tic200HzAccum -= samplesPerTic;
                shouldProcessRow = true;
                shouldAdvance    = true;
            }
        } else {
            // Standard tic mode (01-FB): frame-accurate, tempo-locked advance (see sampler path).
            if (sv.lastProcessedRow == -1) {
                // Fire the first tic at trigger so row 0 applies immediately (see sampler path).
                sv.tableFrameAccum = 0.0f;
                shouldProcessRow   = true;
                shouldAdvance      = true;
            } else {
                int tempo = currentTempo.load(std::memory_order_relaxed);
                float framesPerRow = sampleRate / (tempo / 60.0f * 4.0f * 12.0f) * (float)sv.tableTicRate;
                sv.tableFrameAccum += numFrames;
                if (framesPerRow > 0.0f && sv.tableFrameAccum >= framesPerRow) {
                    sv.tableFrameAccum -= framesPerRow;
                    if (sv.tableFrameAccum >= framesPerRow) sv.tableFrameAccum = 0.0f;
                    shouldProcessRow   = true;
                    shouldAdvance      = true;
                }
            }
        }

        if (shouldProcessRow) {
            TableRow row;
            {
                std::lock_guard<std::mutex> lock(tableMutex);
                row = tables[sv.tableId].rows[sv.tableRow];
            }

            int semitones = transposeToSemitones(row.transpose);
            sv.tableTranspose = (float)semitones;
            sv.modSourceValues[MOD_SRC_TABLE_PITCH] = (float)semitones;

            if (row.volume == 0xFF) {
                sv.tableVolume = 1.0f;
            } else {
                sv.tableVolume = row.volume / 255.0f;
            }
            sv.modSourceValues[MOD_SRC_TABLE_VOL] = sv.tableVolume;

            bool hopExecuted = false;
            int  hopTarget   = -1;

            auto processEffect = [&](uint8_t fxType, uint8_t fxValue) {
                switch (fxType) {
                    case FX_KILL:
                        if (fxValue == 0x00) sv.noteOff();
                        break;
                    case FX_HOP:
                        if (fxValue == 0xFF) {
                            sv.tableId      = -1;
                            sv.hopTargetRow = -1;
                            sv.hopRepeatCount = 0;
                        } else {
                            int repeatCount = (fxValue >> 4) & 0x0F;
                            int targetRow   =  fxValue       & 0x0F;
                            if (repeatCount == 0) {
                                hopExecuted = true; hopTarget = targetRow;
                            } else {
                                if (sv.hopTargetRow == -1 || sv.hopTargetRow != targetRow) {
                                    sv.hopRepeatCount = repeatCount;
                                    sv.hopTargetRow   = targetRow;
                                }
                                if (sv.hopRepeatCount > 0) {
                                    sv.hopRepeatCount--;
                                    hopExecuted = true; hopTarget = targetRow;
                                } else {
                                    sv.hopTargetRow = -1;
                                }
                            }
                        }
                        break;
                    case FX_VOLUME:
                        sv.tableVolume = fxValue / 255.0f;
                        sv.modSourceValues[MOD_SRC_TABLE_VOL] = sv.tableVolume;
                        break;
                    case FX_TIC:
                        if (fxValue >= 0x01 && fxValue <= 0xFB) {
                            sv.tableTicRate    = fxValue;
                            sv.tableTicCounter = 0;
                        }
                        break;
                    case FX_THO:
                        hopExecuted = true; hopTarget = fxValue & 0x0F;
                        break;
                    default:
                        break;  // FX_OFFSET, FX_RETRIGGER, FX_ARP not applicable to SF
                }
            };

            processEffect(row.fx1Type, row.fx1Value);
            processEffect(row.fx2Type, row.fx2Value);
            processEffect(row.fx3Type, row.fx3Value);

            sv.lastProcessedRow = sv.tableRow;

            if (hopExecuted && hopTarget >= 0) {
                sv.tableRow = hopTarget % 16;
            } else if (shouldAdvance) {
                sv.tableRow = (sv.tableRow + 1) % 16;
            }
        }
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
            float rawStart   = voice.params.base[PARAM_SAMPLE_START] + voice.modDestValues[PARAM_SAMPLE_START];
            float rawEnd     = voice.params.base[PARAM_SAMPLE_END]   + voice.modDestValues[PARAM_SAMPLE_END];
            float rawLoop    = voice.params.base[PARAM_LOOP_START]   + voice.modDestValues[PARAM_LOOP_START];
            int sl = voice.sampleLength;
            voice.actualStart     = std::max(0,             std::min((int)(rawStart * sl / 255.0f), sl - 2));
            voice.actualEnd       = std::max(voice.actualStart + 1, std::min((int)(rawEnd * sl / 255.0f), sl - 1));
            voice.actualLoopStart = std::max(voice.actualStart, std::min((int)(rawLoop * sl / 255.0f), voice.actualEnd - 1));
            voice.actualLoopEnd   = std::max(voice.actualLoopStart + 1, std::min((int)((float)voice.loopEndNorm * sl / 255.0f), voice.actualEnd));
        }

        for (int i = 0; i < numFrames; i++) {
            int idx = (int)voice.position;
            float frac = voice.position - (float)idx;

            // Bounds check - need idx+1 for interpolation
            if (idx < 0 || idx >= voice.sampleLength - 1) {
                if (idx < 0) {
                    voice.isActive = false;  // negative position: safety hard-stop
                } else {
                    // At or past last interpolation point: fade out so SVF resonance decays
                    voice.position = (float)(voice.sampleLength - 2);
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
            float trackVol = (voice.trackId >= 0 && voice.trackId < 8) ? trackVolSnapshot[voice.trackId] : 1.0f;
            float masterVol = masterVolSnapshot;
            float antiClick = voice.antiClickFade();

            float sampleL, sampleR;

            if (voice.sampleDataRight) {
                // ── STEREO SAMPLE PATH ────────────────────────────────────────────
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
                float procL = s1L + (s2L - s1L) * frac;
                float procR = s1R + (s2R - s1R) * frac;
                voice.chain.filter.setInterpolatedCoeffs(t);
                voice.chain.processStereo(procL, procR);

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

                float globalMul = trackVol * masterVol * antiClick;
                procL *= globalMul;
                procR *= globalMul;

                if (voice.isFadingOut) {
                    float fo = (float)voice.fadeOutRemaining / (float)DECLICK_SAMPLES;
                    procL *= fo;
                    procR *= fo;
                    if (--voice.fadeOutRemaining <= 0) {
                        voice.isFadingOut = false;
                        voice.isActive = false;
                    }
                }

                sampleL = procL * panL;
                sampleR = procR * panR;
            } else {
                // ── MONO SAMPLE PATH ──────────────────────────────────────────────
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
                processedSample = voice.chain.processMono(processedSample);

                float sample = processedSample * finalVol * volRoute;

                if ((stemsMode == 0 || stemsMode >= 9) && voice.reverbSend > 0.0f) {
                    revSendBufL[i] += sample * panL * voice.reverbSend;
                    revSendBufR[i] += sample * panR * voice.reverbSend;
                }
                if ((stemsMode == 0 || stemsMode >= 9) && voice.delaySend > 0.0f) {
                    dlySendBufL[i] += sample * panL * voice.delaySend;
                    dlySendBufR[i] += sample * panR * voice.delaySend;
                }

                sample = sample * trackVol * masterVol * antiClick;

                if (voice.isFadingOut) {
                    sample *= (float)voice.fadeOutRemaining / (float)DECLICK_SAMPLES;
                    if (--voice.fadeOutRemaining <= 0) {
                        voice.isFadingOut = false;
                        voice.isActive = false;
                    }
                }

                sampleL = sample * panL;
                sampleR = sample * panR;
            }

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
                        voice.position = (float)voice.actualLoopStart;
                    }
                } else {
                    voice.position += modulatedRate;
                    if (voice.position >= voice.actualLoopEnd) {
                        voice.loopingBack = true;
                        voice.position = (float)voice.actualLoopEnd;
                    }
                }
            } else if (voice.reverse && !voice.loopReleasing) {
                voice.position -= modulatedRate;
                if (voice.position <= voice.actualStart) {
                    if (voice.loopMode == 1) {
                        voice.position = (float)voice.actualLoopStart;
                    } else {
                        voice.position = (float)voice.actualStart;
                        voice.startFadeOut();
                        break;
                    }
                }
            } else {
                voice.position += modulatedRate;
                bool activeForwardLoop = (voice.loopMode == 1 && !voice.loopReleasing);
                float fwdBoundary = activeForwardLoop ? (float)voice.actualLoopEnd : (float)voice.actualEnd;
                if (voice.position >= fwdBoundary) {
                    if (activeForwardLoop) {
                        voice.position = (float)voice.actualLoopStart;
                    } else {
                        voice.position = (float)(voice.actualEnd - 1);
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
        // sfBuf (per-track SF render, MAX_BLOCK frames * 2 channels) is an engine member; it is
        // memset per use below before each tsf render.
        float masterVol = masterVolSnapshot;

        for (int t = 0; t < 8; t++) {
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
                if (h) tsf_channel_set_volume(h, t, noteVol * trkVol);
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

        for (int t = 0; t < 8; t++) {
            SoundfontVoice& sv = sfVoices[t];
            // Local sfSlot snapshot — see the volSlot comment above (detach() race).
            int slot = sv.sfSlot;
            if (!sv.isActive || slot < 0 || slot >= MAX_SOUNDFONTS) continue;

            memset(sfBuf, 0, sizeof(float) * numFrames * 2);
            bool rendered = false;
            {
                // Handle must be read INSIDE the lock: capturing it before would let
                // loadSoundfont's eviction tsf_close it between the read and the render.
                std::lock_guard<std::mutex> sfLock(soundfonts[slot].mutex);
                tsf* h = soundfonts[slot].handle;
                if (h) {
                    tsf_render_float_channel(h, t, sfBuf, numFrames, 0 /* overwrite */);
                    rendered = true;
                }
            }
            if (!rendered) continue;

            for (int i = 0; i < numFrames; i++) {
                float lerp_t = (numFrames > 1) ? (float)(i + 1) / (float)numFrames : 1.0f;
                float L = sfBuf[i * 2];
                float R = sfBuf[i * 2 + 1];
                sv.chain.filter.setInterpolatedCoeffs(lerp_t);
                sv.chain.processStereo(L, R);
                sfBuf[i * 2]     = L;
                sfBuf[i * 2 + 1] = R;
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
                float sL = sfBuf[i * 2];
                float sR = sfBuf[i * 2 + 1];
                trackPeakL = fmaxf(trackPeakL, fabsf(sL));
                trackPeakR = fmaxf(trackPeakR, fabsf(sR));
                float outL = sL * masterVol;
                float outR = sR * masterVol;
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
            framePeaksPerTrackL[t] = fmaxf(framePeaksPerTrackL[t], trackPeakL);
            framePeaksPerTrackR[t] = fmaxf(framePeaksPerTrackR[t], trackPeakR);

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
                trackWaveformBuffer[t][trackWaveformIndex] =
                    (trackWaveAccumL[t][i] + trackWaveAccumR[t][i]) * 0.5f;
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
                instrSpectrumBuffer[instrSpectrumWriteIdx] = instrSpectrumTempL[i];
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

    // Oboe bursts are normally 192-960 frames, but the None/Shared fallback path or unusual
    // ROMs can exceed MAX_BLOCK — and every per-block buffer in processAudioBlock is sized to
    // MAX_BLOCK. Chunk to keep them in bounds (renderOffline already does the same).
    int processed = 0;
    while (processed < numFrames) {
        int chunk = std::min((int)numFrames - processed, MAX_BLOCK);
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
    ScheduledNote note = {
            .targetFrame = targetFrame,
            .sampleId = sampleId,
            .trackId = trackId,
            .frequency = frequency,
            .baseFrequency = baseFrequency,
            .volume = volume,
            .phraseVolume = phraseVolume,
            .pan = pan,
            .startPointOverride = startPointOverride,
            .endPointOverride = endPointOverride,
            .tableId = tableId,
            .tableTicRate = tableTicRate,
            .noteOctave = noteOctave,
            .notePitch = notePitch,
            .pslInitialOffset = pslInitialOffset,
            .pslDuration = pslDuration,
            .pbnRate = pbnRate,
            .vibratoSpeed = vibratoSpeed,
            .vibratoDepth = vibratoDepth,
            .tableStartRow = tableStartRow
    };
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

void AudioEngine::scheduleKill(int64_t targetFrame, int trackId) {
    ScheduledKill kill = {
            .targetFrame = targetFrame,
            .trackId = trackId
    };
    killQueue.schedule(kill);
}

void AudioEngine::scheduleNoteOff(int64_t targetFrame, int trackId) {
    ScheduledKill kill = {
            .targetFrame = targetFrame,
            .trackId = trackId,
            .softKill = true
    };
    killQueue.schedule(kill);
}

void AudioEngine::clearScheduledNotes() {
    noteQueue.clear();
    killQueue.clear();
    paramUpdateQueue.clear();
}

void AudioEngine::clearScheduledNotesFrom(int64_t fromFrame) {
    noteQueue.clearFrom(fromFrame);
    killQueue.clearFrom(fromFrame);
    paramUpdateQueue.clearFrom(fromFrame);
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

int AudioEngine::getVoiceTableRow(int trackId) {
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].trackId == trackId) {
            return voices[v].tableRow;
        }
    }
    // SF voices are indexed directly by trackId
    if (trackId >= 0 && trackId < 8) {
        const SoundfontVoice& sv = sfVoices[trackId];
        if (sv.isActive && sv.tableId >= 0) return sv.tableRow;
    }
    return -1;
}

int AudioEngine::getVoiceTableId(int trackId) {
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].trackId == trackId) {
            return voices[v].tableId;
        }
    }
    // SF voices are indexed directly by trackId
    if (trackId >= 0 && trackId < 8) {
        const SoundfontVoice& sv = sfVoices[trackId];
        if (sv.isActive) return sv.tableId;
    }
    return -1;
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

void AudioEngine::scheduleVoiceEqSlot(int64_t targetFrame, int trackId, int slot) {                // EQN
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, (float)slot, PARAM_UPDATE_EQ_SLOT, 0.0f });
}

void AudioEngine::scheduleMasterEqSlot(int64_t targetFrame, int slot) {                            // EQM
    paramUpdateQueue.schedule({ targetFrame, -1, 0, (float)slot, PARAM_UPDATE_MASTER_EQ, 0.0f });
}

// Apply a global EQ preset (slot 0-127, <0 = bypass) to a live voice's inline EQ. The voice's
// chain.eq was already sp_pareq_init'd at trigger (chain.reset), so we only re-set band params.
// Mirrors setInstrumentEqSlot's preset→bands copy, but writes straight into a playing voice (EQN).
void AudioEngine::applyEqPresetToChain(InstrumentChain& chain, int slot) {
    if (slot < 0 || slot >= 128) { chain.eq.active = false; return; }
    const EqPresetBank& preset = eqPresets[slot];
    bool any = false;
    for (int i = 0; i < 3; i++) {
        chain.eq.bands[i].setParams(preset.bands[i].type, preset.bands[i].freqHz,
                                    preset.bands[i].gainDb, preset.bands[i].q);
        if (preset.bands[i].type != 0) any = true;
    }
    chain.eq.active = any;
}

static const int SPECTRUM_FFT_SIZE = 2048;

// Shared FFT helper — takes FFT_SIZE samples already copied from the circular buffer by the caller
// (under mutex), applies Hann window + FFT, maps to numBins log-spaced magnitude values [0,1].
static void computeSpectrumFFT(kiss_fft_scalar* input, int numBins, float* out) {
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

    const float sampleRate = 44100.0f;
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
    computeSpectrumFFT(input, numBins, out);
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
    computeSpectrumFFT(input, numBins, out);
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

void AudioEngine::setTrackVolume(int trackId, float volume) {
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
    LOGD("🔊 Track %d volume set to %.2f", trackId, volume);
}

void AudioEngine::setMasterVolume(float volume) {
    std::lock_guard<std::mutex> lock(volumeMutex);
    masterVolume = volume;
    LOGD("🔊 Master volume set to %.2f", volume);
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
    if (trackId >= 0 && trackId < 8 && sfVoices[trackId].isActive) {
        return &sfVoices[trackId];
    }
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].trackId == trackId) {
            return &voices[v];
        }
    }
    return nullptr;
}

void AudioEngine::setPitchSlide(int trackId, float targetSemitones, float durationTicks, int tempo) {
    IAudioVoice* v = findActiveVoiceForTrack(trackId);
    if (!v) return;
    float sr = (float)getSampleRate();
    float framesPerTic = sr / (tempo / 60.0f * 4.0f * 12.0f);
    float totalFrames = fmaxf(1.0f, framesPerTic * durationTicks);
    v->setPitchSlideRaw(targetSemitones, totalFrames);
    LOGD("🎵 Pitch slide: track=%d, to=%.2f over %.0f frames", trackId, targetSemitones, totalFrames);
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

void AudioEngine::clearPitchMod(int trackId) {
    IAudioVoice* v = findActiveVoiceForTrack(trackId);
    if (!v) return;
    v->clearPitchMod();
    LOGD("🎵 Pitch mod cleared: track=%d", trackId);
}

void AudioEngine::setInitialPitchOffset(int trackId, float semitones) {
    IAudioVoice* v = findActiveVoiceForTrack(trackId);
    if (!v) return;
    v->setInitialPitchOffset(semitones);
    LOGD("🎵 Pitch offset set: track=%d, offset=%.2f semitones", trackId, semitones);
}

void AudioEngine::setInstrumentModulation(int sampleId, int slotIndex,
                                          int type, int dest, float amount,
                                          int attackSamples, int holdSamples, int decaySamples,
                                          float sustainLevel, float lfoHz, int oscShape,
                                          int releaseSamples) {
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
    slot.releaseSamples = releaseSamples;
}

void AudioEngine::triggerNoteOff(int trackId) {
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].isActive || voices[v].trackId != trackId) continue;
        bool hasRelease = false;
        for (int m = 0; m < 4; m++) {
            VoiceModSlot& mod = voices[v].voiceMods[m];
            if (mod.dest == 1 && (mod.type == 2 || mod.type == 5)) {
                if (mod.stage >= 1 && mod.stage <= 3 && mod.releaseSamples > 0) {
                    mod.stage = 4;
                    mod.stageCounter = 0;
                    hasRelease = true;
                } else if (mod.stage == 4) {
                    hasRelease = true;
                }
            }
        }
        if (hasRelease) {
            // Looping voice: leave the loop so the [loopEnd, end] tail plays out under the release env.
            if (voices[v].loopMode != 0) voices[v].loopReleasing = true;
        } else {
            // No release envelope: fade out over DECLICK_SAMPLES instead of a hard stop, so KIL (and any
            // soft note-off on a release-less voice) is click-free. startFadeOut keeps the slot reserved
            // and the main mix loop frees it when the fade finishes — same path voice-stealing uses.
            voices[v].startFadeOut();
        }
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

    const int BLOCK_SIZE = 256;
    int rendered = 0;
    while (rendered < numFrames) {
        int chunk = std::min(BLOCK_SIZE, numFrames - rendered);
        processAudioBlock(output + rendered * 2, chunk, 2, (float)sampleRate);
        rendered += chunk;
    }
}

void AudioEngine::resetFrameCounter() {
    globalFrameCounter.store(0, std::memory_order_relaxed);
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
