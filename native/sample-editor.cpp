// sample-editor.cpp — AudioEngine sample editor operations (non-realtime)
// All methods here are called from the UI thread, never from the audio callback.
#include "audio-engine.h"
#include "effects/primitives/sola-stretch.h"
#include <cmath>

// ============================================================
// SAMPLE EDITOR OPERATIONS
// ============================================================

// The undo and RATE-HIGH caches are stored as int16 to halve their RAM: they only
// ever restore the working buffer, never feed the mix loop. f32->i16 clamps to [-1,1] (so an
// over-unity working sample — post-normalize/gain — restores at full scale rather than wrapping)
// and rounds to nearest; i16->f32 uses /32768 to match the WAV decoder, making the round trip
// bit-exact for 16-bit-sourced WAVs and an inaudible ~-96 dBFS requantization for the rest.
static inline int16_t f32ToCacheI16(float f) {
    float c = std::min(1.0f, std::max(-1.0f, f));
    int   v = (int)std::lround(c * 32768.0f);
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}
static inline float cacheI16ToF32(int16_t v) { return v / 32768.0f; }

// Replace the working buffers for `id` with a new left + optional right of length newLen, freeing the
// old buffers. Keeps left/right and their shared length in lockstep so the stereo mix path can never
// read a stale or short right channel. Length-changing ops MUST go through this. Pass newR=nullptr for mono.
void AudioEngine::setSampleBuffers(int id, float* newL, float* newR, int newLen) {
    delete[] samples[id];
    delete[] samplesRight[id];
    samples[id]       = newL;
    samplesRight[id]  = newR;
    sampleLengths[id] = newLen;
}

// Stop voices reading slot `id`'s buffers, then acquire sampleEditMutex. Every destructive op
// below must hold the returned lock while mutating/freeing the slot's buffers: the audio thread
// try_locks this mutex in its mix loop, so it skips one block (~10 ms silence) instead of reading
// freed or half-edited memory. Without it, editing a sample that is audible at that moment
// (background playback, or preview-then-edit) is a use-after-free crash.
std::unique_lock<std::mutex> AudioEngine::beginSampleEdit(int id) {
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].sampleData == samples[id]) voices[v].stop();
    }
    return std::unique_lock<std::mutex>(sampleEditMutex);
}

int AudioEngine::getSampleLength(int id) {
    if (id < 0 || id >= 256 || !samples[id]) return 0;
    return sampleLengths[id];
}

void AudioEngine::getSampleWaveform(int id, float* out, int numBins) {
    if (id < 0 || id >= 256 || !samples[id] || numBins <= 0) {
        for (int i = 0; i < numBins * 2; i++) out[i] = 0.0f;
        return;
    }
    int len = sampleLengths[id];
    float* buf = samples[id];
    for (int bin = 0; bin < numBins; bin++) {
        int start = (int)((long long)bin * len / numBins);
        int end   = (int)((long long)(bin + 1) * len / numBins);
        if (end > len) end = len;
        float minV = 0.0f, maxV = 0.0f;
        for (int i = start; i < end; i++) {
            if (buf[i] < minV) minV = buf[i];
            if (buf[i] > maxV) maxV = buf[i];
        }
        out[bin * 2]     = minV;
        out[bin * 2 + 1] = maxV;
    }
}

void AudioEngine::getSampleWaveformRange(int id, int startFrame, int endFrame, float* out, int numBins) {
    if (id < 0 || id >= 256 || !samples[id] || numBins <= 0) {
        for (int i = 0; i < numBins * 2; i++) out[i] = 0.0f;
        return;
    }
    int len = sampleLengths[id];
    startFrame = std::max(0, std::min(startFrame, len));
    endFrame   = std::max(startFrame, std::min(endFrame, len));
    int rangeLen = endFrame - startFrame;
    if (rangeLen <= 0) {
        for (int i = 0; i < numBins * 2; i++) out[i] = 0.0f;
        return;
    }
    float* buf = samples[id];
    for (int bin = 0; bin < numBins; bin++) {
        int s = startFrame + (int)((long long)bin * rangeLen / numBins);
        int e = startFrame + (int)((long long)(bin + 1) * rangeLen / numBins);
        if (e > endFrame) e = endFrame;
        float minV = 0.0f, maxV = 0.0f;
        for (int i = s; i < e; i++) {
            if (buf[i] < minV) minV = buf[i];
            if (buf[i] > maxV) maxV = buf[i];
        }
        out[bin * 2]     = minV;
        out[bin * 2 + 1] = maxV;
    }
}

void AudioEngine::getSampleData(int id, float* out) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    std::memcpy(out, samples[id], sampleLengths[id] * sizeof(float));
}

void AudioEngine::getSampleDataRight(int id, float* out) {
    if (id < 0 || id >= 256 || !samplesRight[id]) return;
    std::memcpy(out, samplesRight[id], sampleLengths[id] * sizeof(float));
}

void AudioEngine::getSampleWaveformRangeSource(int id, int startFrame, int endFrame, float* out, int numBins, int channel) {
    if (id < 0 || id >= 256 || !samples[id] || numBins <= 0) {
        for (int i = 0; i < numBins * 2; i++) out[i] = 0.0f;
        return;
    }
    int len = sampleLengths[id];
    startFrame = std::max(0, std::min(startFrame, len));
    endFrame   = std::max(startFrame, std::min(endFrame, len));
    int rangeLen = endFrame - startFrame;
    if (rangeLen <= 0) {
        for (int i = 0; i < numBins * 2; i++) out[i] = 0.0f;
        return;
    }
    float* bufL = samples[id];
    float* bufR = samplesRight[id];
    // channel 1 (RIGHT) with no right buffer falls back to left
    if (channel == 1 && !bufR) channel = 0;
    for (int bin = 0; bin < numBins; bin++) {
        int s = startFrame + (int)((long long)bin * rangeLen / numBins);
        int e = startFrame + (int)((long long)(bin + 1) * rangeLen / numBins);
        if (e > endFrame) e = endFrame;
        float minV = 0.0f, maxV = 0.0f;
        for (int i = s; i < e; i++) {
            float v;
            if (channel == 1) {
                v = bufR[i];
            } else if (channel == 2 && bufR) {
                v = (bufL[i] + bufR[i]) * 0.5f;  // averaged for STEREO/MONO view
            } else {
                v = bufL[i];
            }
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
        }
        out[bin * 2]     = minV;
        out[bin * 2 + 1] = maxV;
    }
}

float AudioEngine::getSamplePlaybackPosition(int id) {
    if (id < 0 || id >= 256 || !samples[id] || sampleLengths[id] <= 0) return -1.0f;
    for (int v = 0; v < MAX_VOICES; v++) {
        const Voice& voice = voices[v];
        if (voice.isActive && !voice.isFadingOut && voice.sampleData == samples[id]) {
            return (float)(voice.position / (double)sampleLengths[id]);
        }
    }
    return -1.0f;
}

void AudioEngine::normalizeSample(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    // In-place op: no buffer is freed, so no voice-stop needed — but hold the edit lock so the
    // mix loop skips one block rather than playing a half-edited region. Same for the other
    // in-place ops below (fade/silence/reverse).
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    if (startFrame >= endFrame) return;
    float* bufR = samplesRight[id];
    // Peak across BOTH channels so the same gain is applied to each — preserves the stereo image.
    float peak = 0.0f;
    for (int i = startFrame; i < endFrame; i++) {
        float v = std::abs(samples[id][i]);
        if (v > peak) peak = v;
        if (bufR) { float r = std::abs(bufR[i]); if (r > peak) peak = r; }
    }
    if (peak < 0.0001f) return;
    float gain = 1.0f / peak;
    for (int i = startFrame; i < endFrame; i++) {
        samples[id][i] *= gain;
        if (bufR) bufR[i] *= gain;
    }
}

void AudioEngine::fadeInSample(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    int count  = endFrame - startFrame;
    if (count <= 0) return;
    float* bufR = samplesRight[id];
    for (int i = 0; i < count; i++) {
        float g = (float)i / count;
        samples[id][startFrame + i] *= g;
        if (bufR) bufR[startFrame + i] *= g;
    }
}

void AudioEngine::fadeOutSample(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    int count  = endFrame - startFrame;
    if (count <= 0) return;
    float* bufR = samplesRight[id];
    for (int i = 0; i < count; i++) {
        float g = 1.0f - (float)i / count;
        samples[id][startFrame + i] *= g;
        if (bufR) bufR[startFrame + i] *= g;
    }
}

void AudioEngine::silenceRegion(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    // ⚠️ An INVERTED window is reachable: `sampleStart` and `sampleEnd` are two independent free
    // 0-255 cells with nothing constraining one against the other, and the editor seeds its
    // selection from them. `endFrame - startFrame` is int and `sizeof(float)` is size_t, so a
    // negative length converts to ~2^64 and the memset walks off the buffer. The guard belongs here
    // rather than at the call sites, as it does in the four siblings above.
    if (startFrame >= endFrame) return;
    std::memset(samples[id] + startFrame, 0, (endFrame - startFrame) * sizeof(float));
    if (samplesRight[id])
        std::memset(samplesRight[id] + startFrame, 0, (endFrame - startFrame) * sizeof(float));
}

void AudioEngine::reverseSample(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    // ⚠️ Same inverted window as silenceRegion. `std::reverse(first, last)` with `first > last`
    // never reaches `first == last`, so it swaps outward from both ends of the buffer.
    if (startFrame >= endFrame) return;
    std::reverse(samples[id] + startFrame, samples[id] + endFrame);
    if (samplesRight[id])
        std::reverse(samplesRight[id] + startFrame, samplesRight[id] + endFrame);
}

void AudioEngine::backupSample(int id) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    delete[] sampleBackups[id];
    delete[] sampleBackupsRight[id];
    sampleBackupsRight[id] = nullptr;
    int len = sampleLengths[id];
    sampleBackups[id] = new int16_t[len];
    for (int i = 0; i < len; i++) sampleBackups[id][i] = f32ToCacheI16(samples[id][i]);
    if (samplesRight[id]) {
        sampleBackupsRight[id] = new int16_t[len];
        for (int i = 0; i < len; i++) sampleBackupsRight[id][i] = f32ToCacheI16(samplesRight[id][i]);
    }
    sampleBackupLengths[id] = len;
}

void AudioEngine::undoSample(int id) {
    if (id < 0 || id >= 256 || !sampleBackups[id]) return;
    // Same "stop voices reading this buffer + lock" preamble as the other edits (beginSampleEdit keys
    // off sampleData == samples[id], the buffer actually swapped below — more precise than instrId).
    auto editLock = beginSampleEdit(id);
    int len = sampleBackupLengths[id];
    float* newL = new float[len];
    for (int i = 0; i < len; i++) newL[i] = cacheI16ToF32(sampleBackups[id][i]);
    float* newR = nullptr;
    if (sampleBackupsRight[id]) {
        newR = new float[len];
        for (int i = 0; i < len; i++) newR[i] = cacheI16ToF32(sampleBackupsRight[id][i]);
    }
    setSampleBuffers(id, newL, newR, len);  // restores mono/stereo state of the backup
}

void AudioEngine::freeSampleUndo(int id) {
    if (id < 0 || id >= 256) return;
    // The undo backup is only ever read by undoSample (UI thread, inside the editor); the audio mix
    // loop never touches it, so no sampleEditMutex is needed. Once the editor closes, undo is
    // unreachable — free the backup so it doesn't linger in RAM.
    // NOTE: originalSamples (the RATE-HIGH cache) is deliberately NOT freed here — it's null when at
    // HIGH and required for lossless restore when at LOFI/NORM, so freeing it would only break RATE.
    delete[] sampleBackups[id];      sampleBackups[id] = nullptr;
    delete[] sampleBackupsRight[id]; sampleBackupsRight[id] = nullptr;
    sampleBackupLengths[id] = 0;
}

void AudioEngine::saveFxPreviewBackup(int id) {
    if (id < 0 || id >= 256 || !samples[id] || sampleLengths[id] <= 0) return;
    delete[] fxPreviewBackup;
    delete[] fxPreviewBackupRight;
    fxPreviewBackupRight = nullptr;
    int len = sampleLengths[id];
    fxPreviewBackup    = new float[len];
    std::memcpy(fxPreviewBackup, samples[id], len * sizeof(float));
    if (samplesRight[id]) {
        fxPreviewBackupRight = new float[len];
        std::memcpy(fxPreviewBackupRight, samplesRight[id], len * sizeof(float));
    }
    fxPreviewBackupLen = len;
    fxPreviewBackupId  = id;
}

void AudioEngine::restoreFxPreviewBackup() {
    if (fxPreviewBackupId < 0 || !fxPreviewBackup) return;
    int id = fxPreviewBackupId;
    if (id >= 0 && id < 256 && samples[id] && sampleLengths[id] == fxPreviewBackupLen) {
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].isActive && voices[v].sampleData == samples[id]) voices[v].stop();
        }
        std::lock_guard<std::mutex> lock(sampleEditMutex);
        std::memcpy(samples[id], fxPreviewBackup, fxPreviewBackupLen * sizeof(float));
        if (fxPreviewBackupRight && samplesRight[id])
            std::memcpy(samplesRight[id], fxPreviewBackupRight, fxPreviewBackupLen * sizeof(float));
    }
    delete[] fxPreviewBackup;
    delete[] fxPreviewBackupRight;
    fxPreviewBackup      = nullptr;
    fxPreviewBackupRight = nullptr;
    fxPreviewBackupLen   = 0;
    fxPreviewBackupId    = -1;
}

void AudioEngine::cropSample(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    auto editLock = beginSampleEdit(id);
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    if (startFrame >= endFrame) return;
    int newLen = endFrame - startFrame;
    float* newL = new float[newLen];
    std::memcpy(newL, samples[id] + startFrame, newLen * sizeof(float));
    float* newR = nullptr;
    if (samplesRight[id]) {
        newR = new float[newLen];
        std::memcpy(newR, samplesRight[id] + startFrame, newLen * sizeof(float));
    }
    setSampleBuffers(id, newL, newR, newLen);
    instrumentParams[id].startPoint = 0;
    instrumentParams[id].endPoint   = 255;
}

void AudioEngine::deleteSampleRegion(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    auto editLock = beginSampleEdit(id);
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    if (startFrame >= endFrame) return;
    int oldLen = sampleLengths[id];
    int newLen = oldLen - (endFrame - startFrame);
    if (newLen <= 0) return;
    float* newL = new float[newLen];
    std::memcpy(newL,              samples[id],            startFrame * sizeof(float));
    std::memcpy(newL + startFrame, samples[id] + endFrame, (oldLen - endFrame) * sizeof(float));
    float* newR = nullptr;
    if (samplesRight[id]) {
        newR = new float[newLen];
        std::memcpy(newR,              samplesRight[id],            startFrame * sizeof(float));
        std::memcpy(newR + startFrame, samplesRight[id] + endFrame, (oldLen - endFrame) * sizeof(float));
    }
    setSampleBuffers(id, newL, newR, newLen);
    instrumentParams[id].startPoint = 0;
    instrumentParams[id].endPoint   = 255;
}

void AudioEngine::copyRegion(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    // Read-only on the sample, but must still hold the edit mutex so no destructive op swaps
    // the buffers out from under the memcpy. Deliberately NOT beginSampleEdit(): that would
    // also stop voices playing this sample, and COPY must not cut an audible preview.
    std::unique_lock<std::mutex> editLock(sampleEditMutex);
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    if (startFrame >= endFrame) return;
    int len = endFrame - startFrame;
    delete[] sampleClipboard;
    delete[] sampleClipboardRight;
    sampleClipboardRight = nullptr;
    sampleClipboard = new float[len];
    std::memcpy(sampleClipboard, samples[id] + startFrame, len * sizeof(float));
    if (samplesRight[id]) {
        sampleClipboardRight = new float[len];
        std::memcpy(sampleClipboardRight, samplesRight[id] + startFrame, len * sizeof(float));
    }
    sampleClipboardLength = len;
}

// Prepare the sample-editor's LEFT/RIGHT/MONO source preview NATIVELY: the Kotlin side used
// to pull the full left + right PCM into Java arrays (plus a third for the MONO average) —
// up to 3x the sample size transiently on the capped Java heap, exactly the OOM class the
// native load paths exist to avoid. Slot→slot copy in native memory instead.
void AudioEngine::prepareSourcePreview(int dstId, int srcId, int mode) {
    if (dstId < 0 || dstId >= 256 || srcId < 0 || srcId >= 256 || dstId == srcId) return;
    // Stops voices reading the dst (scratch) slot and holds the edit mutex — which also
    // keeps the SOURCE buffers steady for the copy (single global edit mutex).
    auto editLock = beginSampleEdit(dstId);
    if (!samples[srcId] || sampleLengths[srcId] <= 0) return;
    const int len  = sampleLengths[srcId];
    const float* L = samples[srcId];
    const float* R = samplesRight[srcId] ? samplesRight[srcId] : L;
    float* out = new (std::nothrow) float[len];
    if (!out) return;
    if (mode == 1) {
        std::memcpy(out, R, (size_t)len * sizeof(float));
    } else if (mode == 3) {
        for (int i = 0; i < len; i++) out[i] = (L[i] + R[i]) * 0.5f;
    } else {  // 0 = LEFT (also the mono-source fallback: R aliases L then)
        std::memcpy(out, L, (size_t)len * sizeof(float));
    }
    setSampleBuffers(dstId, out, nullptr, len);
}

void AudioEngine::pasteRegion(int id, int insertAt) {
    if (id < 0 || id >= 256 || !samples[id] || !sampleClipboard || sampleClipboardLength <= 0) return;
    auto editLock = beginSampleEdit(id);
    insertAt = std::max(0, std::min(sampleLengths[id], insertAt));
    int oldLen = sampleLengths[id];
    int clip   = sampleClipboardLength;
    int newLen = oldLen + clip;
    float* newL = new float[newLen];
    std::memcpy(newL,                   samples[id],            insertAt * sizeof(float));
    std::memcpy(newL + insertAt,        sampleClipboard,        clip * sizeof(float));
    std::memcpy(newL + insertAt + clip, samples[id] + insertAt, (oldLen - insertAt) * sizeof(float));
    float* newR = nullptr;
    if (samplesRight[id]) {
        // Right of the inserted block: stereo clip → its right channel; mono clip → duplicate the
        // mono clip so the inserted audio is centred rather than silent on the right.
        const float* clipR = sampleClipboardRight ? sampleClipboardRight : sampleClipboard;
        newR = new float[newLen];
        std::memcpy(newR,                   samplesRight[id],            insertAt * sizeof(float));
        std::memcpy(newR + insertAt,        clipR,                       clip * sizeof(float));
        std::memcpy(newR + insertAt + clip, samplesRight[id] + insertAt, (oldLen - insertAt) * sizeof(float));
    }
    setSampleBuffers(id, newL, newR, newLen);
    instrumentParams[id].startPoint = 0;
    instrumentParams[id].endPoint   = 255;
}

int AudioEngine::getClipboardLength() {
    return sampleClipboardLength;
}

void AudioEngine::downsampleSample(int id, int factor) {
    if (id < 0 || id >= 256 || !samples[id] || factor <= 1) return;
    auto editLock = beginSampleEdit(id);
    int oldLen = sampleLengths[id];
    int newLen = oldLen / factor;
    if (newLen < 1) return;
    float* newL = new float[newLen];
    for (int i = 0; i < newLen; i++) newL[i] = samples[id][i * factor];
    float* newR = nullptr;
    if (samplesRight[id]) {
        newR = new float[newLen];
        for (int i = 0; i < newLen; i++) newR[i] = samplesRight[id][i * factor];
    }
    setSampleBuffers(id, newL, newR, newLen);
    // Instrument start/end points (0-255 fraction) map to the same relative positions,
    // so they remain valid after decimation without adjustment.
}

void AudioEngine::applyRateMode(int id, int factor) {
    if (id < 0 || id >= 256 || !samples[id]) return;

    // Stop voices reading this buffer, then hold the edit lock so the callback's try_lock fails
    // during the swap — one silent period rather than a use-after-free crash.
    auto editLock = beginSampleEdit(id);

    if (factor <= 1) {
        // Restore HIGH: copy original (both channels) back, then discard cache.
        if (!originalSamples[id]) return; // Already at HIGH, nothing to restore.
        int len = originalSampleLengths[id];
        float* newL = new float[len];
        for (int i = 0; i < len; i++) newL[i] = cacheI16ToF32(originalSamples[id][i]);
        float* newR = nullptr;
        if (originalSamplesRight[id]) {
            newR = new float[len];
            for (int i = 0; i < len; i++) newR[i] = cacheI16ToF32(originalSamplesRight[id][i]);
        }
        setSampleBuffers(id, newL, newR, len);
        delete[] originalSamples[id];       originalSamples[id] = nullptr;
        delete[] originalSamplesRight[id];  originalSamplesRight[id] = nullptr;
        originalSampleLengths[id] = 0;
    } else {
        // Store original (both channels) on first rate change away from HIGH.
        if (!originalSamples[id]) {
            int len = sampleLengths[id];
            originalSamples[id] = new int16_t[len];
            for (int i = 0; i < len; i++) originalSamples[id][i] = f32ToCacheI16(samples[id][i]);
            if (samplesRight[id]) {
                originalSamplesRight[id] = new int16_t[len];
                for (int i = 0; i < len; i++) originalSamplesRight[id][i] = f32ToCacheI16(samplesRight[id][i]);
            }
            originalSampleLengths[id] = len;
        }
        // Always derive from the cached original so NORM→LOFI→NORM roundtrips are lossless.
        int newLen = originalSampleLengths[id] / factor;
        if (newLen < 1) return;
        float* newL = new float[newLen];
        for (int i = 0; i < newLen; i++) newL[i] = cacheI16ToF32(originalSamples[id][i * factor]);
        float* newR = nullptr;
        if (originalSamplesRight[id]) {
            newR = new float[newLen];
            for (int i = 0; i < newLen; i++) newR[i] = cacheI16ToF32(originalSamplesRight[id][i * factor]);
        }
        setSampleBuffers(id, newL, newR, newLen);
    }
}

void AudioEngine::pitchShiftSample(int id, float semitones) {
    if (id < 0 || id >= 256 || !samples[id] || semitones == 0.0f) return;
    setFlushToZeroForCurrentThread();

    auto editLock = beginSampleEdit(id);  // stop voices reading this buffer, then hold the edit lock

    // Pitch shift makes this buffer the new "original"; discard any RATE cache (both channels).
    delete[] originalSamples[id];       originalSamples[id] = nullptr;
    delete[] originalSamplesRight[id];  originalSamplesRight[id] = nullptr;
    originalSampleLengths[id] = 0;

    float ratio  = std::pow(2.0f, semitones / 12.0f);
    int   oldLen = sampleLengths[id];
    int   newLen = std::max(1, (int)std::round((float)oldLen / ratio));

    // Linear-resample one channel of length oldLen into a fresh newLen buffer.
    auto resample = [&](const float* src) -> float* {
        float* dst = new float[newLen];
        for (int i = 0; i < newLen; i++) {
            float srcPos = i * ratio;
            int   srcIdx = (int)srcPos;
            float frac   = srcPos - srcIdx;
            float s0 = (srcIdx     < oldLen) ? src[srcIdx]     : 0.0f;
            float s1 = (srcIdx + 1 < oldLen) ? src[srcIdx + 1] : s0;
            dst[i] = s0 + (s1 - s0) * frac;
        }
        return dst;
    };

    float* newL = resample(samples[id]);
    float* newR = samplesRight[id] ? resample(samplesRight[id]) : nullptr;
    setSampleBuffers(id, newL, newR, newLen);
}

void AudioEngine::timeStretchSample(int id, float ratio) {
    if (id < 0 || id >= 256 || !samples[id] || sampleLengths[id] <= 0) return;
    // ⚠️ The skip is measured in FRAMES, not as a ratio window. A "0.1% is inaudible" window is 8 ms
    // on an 8-second loop — most of a tick — and SYNC's whole job is to land on the tick.
    if ((int)std::lround((double)sampleLengths[id] * (double)ratio) == sampleLengths[id]) return;
    setFlushToZeroForCurrentThread();

    auto editLock = beginSampleEdit(id);  // stop voices reading this buffer, then hold the edit lock

    // Time-stretch makes this buffer the new "original"; discard any RATE cache (both channels).
    delete[] originalSamples[id];       originalSamples[id] = nullptr;
    delete[] originalSamplesRight[id];  originalSamplesRight[id] = nullptr;
    originalSampleLengths[id] = 0;

    int oldLen = sampleLengths[id];
    std::vector<float> outL = sola::stretch(samples[id], oldLen, ratio, 44100.0f);
    int newLen = (int)outL.size();
    float* newL = new float[newLen];
    std::copy(outL.begin(), outL.end(), newL);

    float* newR = nullptr;
    if (samplesRight[id]) {
        // Deterministic for the same oldLen/ratio, so outR.size() == newLen; clamp + zero-pad defensively
        // so the right buffer is always exactly newLen (keeps L/R in lockstep).
        std::vector<float> outR = sola::stretch(samplesRight[id], oldLen, ratio, 44100.0f);
        newR = new float[newLen];
        int n = std::min((int)outR.size(), newLen);
        std::copy(outR.begin(), outR.begin() + n, newR);
        for (int i = n; i < newLen; i++) newR[i] = 0.0f;
    }
    setSampleBuffers(id, newL, newR, newLen);
}

void AudioEngine::applySampleFx(int id, int fxType, int fxValue, float sampleRate, int limiterPreGain) {
    if (id < 0 || id >= 256 || !samples[id] || sampleLengths[id] <= 0) return;
    // ⚠️ The UI thread runs OTT, DUST, the EQ and the limiter's 0.00002-alpha peak tracker below —
    // the same feedback-tail DSP the audio thread arms FTZ for. A fade-out decays into the denormal
    // range and stays there for the rest of the buffer, at 10-100x per sample, with the screen frozen.
    setFlushToZeroForCurrentThread();

    auto editLock = beginSampleEdit(id);  // stop voices reading this buffer, then hold the edit lock

    int len = sampleLengths[id];

    // Apply the selected destructive FX + limiter to one channel buffer in place. Each channel gets
    // its own fresh module instances so left and right are processed independently (and identically),
    // keeping a stereo sample's two channels in sync.
    auto applyToChannel = [&](float* buf) {
        constexpr int CHUNK = 512;
        float stereo[CHUNK * 2];

        if (fxType == 0) { // OTT
            OttModule ott;
            ott.reset(sampleRate);
            ott.resetForRender(fxValue / 255.0f);
            for (int pos = 0; pos < len; pos += CHUNK) {
                int n = std::min(CHUNK, len - pos);
                for (int i = 0; i < n; i++) {
                    stereo[i * 2]     = buf[pos + i];
                    stereo[i * 2 + 1] = buf[pos + i];
                }
                ott.process(stereo, n, 2);
                for (int i = 0; i < n; i++) buf[pos + i] = stereo[i * 2];
            }
        } else if (fxType == 1) { // DUST
            skdust::DustChain dust;
            dust.prepare((double)sampleRate, CHUNK, 2);
            dust.setDustAmount(fxValue / 255.0f);
            for (int pos = 0; pos < len; pos += CHUNK) {
                int n = std::min(CHUNK, len - pos);
                for (int i = 0; i < n; i++) {
                    stereo[i * 2]     = buf[pos + i];
                    stereo[i * 2 + 1] = buf[pos + i];
                }
                dust.process(stereo, n, 2);
                for (int i = 0; i < n; i++) buf[pos + i] = stereo[i * 2];
            }
        } else if (fxType == 2) { // DRIVE
            DriveModule drive;
            drive.reset();
            drive.setDrive(fxValue);
            for (int i = 0; i < len; i++) buf[i] = drive.processMono(buf[i]);
        } else if (fxType == 3) { // EQ — apply preset slot (fxValue = slot 0-127)
            int slot = std::min(fxValue, 127);
            const EqPresetBank& preset = eqPresets[slot];
            EqModule eq;
            eq.reset(sampleRate);
            for (int b = 0; b < 3; b++) {
                const EqBandData& bd = preset.bands[b];
                eq.bands[b].setParams(bd.type, bd.freqHz, bd.gainDb, bd.q);
                if (bd.type != 0) eq.active = true;
            }
            if (eq.active) {
                for (int i = 0; i < len; i++) buf[i] = eq.processMono(buf[i]);
            }
        }

        // Limiter pass: clamp the result to ±1 using the same pre-gain as the master limiter.
        // Prevents waveform from exceeding the visible area after saturation/compression effects.
        LimiterModule lim;
        lim.reset();
        lim.setPreGain(1.0f + (limiterPreGain / 255.0f) * 3.0f);
        for (int i = 0; i < len; i++) {
            lim.limL.ProcessBlock(&buf[i], 1, lim.preGain);
        }
    };

    applyToChannel(samples[id]);
    if (samplesRight[id]) applyToChannel(samplesRight[id]);
}

int AudioEngine::findZeroCrossing(int id, int frame, int dir, int searchRadius, int sourceMode) {
    if (id < 0 || id >= 256 || !samples[id] || sampleLengths[id] < 2) return frame;
    const float* L   = samples[id];
    const float* R   = samplesRight[id];
    const int    len = sampleLengths[id];

    // ⚠️ SOURCE NAMES THE SIGNAL THE SEAM WILL BE CUT IN, so it names the one to hunt a crossing in.
    // (0 = LEFT, 1 = RIGHT, 2 = STEREO, 3 = MONO — the sample editor's own numbering, passed straight
    // through so there is no second vocabulary to keep in step.) A mono sample has one signal whatever
    // the row says, and its right-channel pointer is null, so every mode collapses to LEFT there.
    const int mode = R ? sourceMode : 0;

    // ── STEREO: what counts as a candidate, and how good it is ───────────────────────────────────
    //
    // A stereo seam is only clean where BOTH channels are near zero, and the two channels of real
    // audio almost never cross on the same frame — so "both cross here" as a requirement would find
    // nothing and snap would silently stop working (the report's "there is going to be much less snap
    // points"). Instead: the candidates are the frames where EITHER channel crosses, and the one that
    // wins is the one whose WORST channel is quietest — `max(|L|, |R|)`, because the louder of the two
    // steps is the click you hear. A true double crossing scores ~0 and beats everything, which is
    // what makes this an improvement rather than a different answer.
    //
    // ⚠️ On a DUAL-MONO file (L == R — what `write_wav_mono` and every CHOP produce) every left
    // crossing is a right crossing scoring 0, so the first one wins and the answer is bit-identical to
    // the left-only search. The new path cannot regress the common case.
    const bool stereo = (mode == 2);

    auto val = [&](int i) -> float {
        if (mode == 1) return R[i];
        if (mode == 3) return (L[i] + R[i]) * 0.5f;   // MONO saves the downmix; cut the downmix
        return L[i];
    };
    auto crossesIn = [&](const float* b, int i) {
        return (b[i - 1] < 0.0f) != (b[i] < 0.0f);
    };
    auto crosses = [&](int i) {
        if (stereo) return crossesIn(L, i) || crossesIn(R, i);
        return (val(i - 1) < 0.0f) != (val(i) < 0.0f);
    };
    auto score = [&](int i) -> float {
        if (!stereo) return 0.0f;   // one signal: every candidate is equally good, so the nearest wins
        return std::max(std::abs(L[i]), std::abs(R[i]));
    };

    int   best      = frame;
    float bestScore = 0.0f;
    bool  found     = false;

    // Returns true when the search can stop — which for a single signal is the FIRST candidate, the
    // behaviour this has always had. Stereo keeps looking: a nearer candidate is not a better one.
    auto consider = [&](int i) {
        if (i < 1 || i >= len || !crosses(i)) return false;
        const float sc = score(i);
        if (!found || sc < bestScore) { bestScore = sc; best = i; found = true; }
        return !stereo;
    };

    // dir > 0: forward only; dir < 0: backward only; dir == 0: nearest (both ways).
    // Directional search is seeded from the already-stepped `frame`, so the result is always at or
    // past `frame` in the move direction — a marker can never snap back behind itself and stick (#8).
    // Walking outward in distance order is also what breaks a stereo score tie in favour of the
    // NEAREST candidate: `sc < bestScore` is strict.
    for (int d = 0; d <= searchRadius; d++) {
        if (dir >= 0 && consider(frame + d)) break;
        if (dir <= 0 && consider(frame - d)) break;
    }
    return found ? best : frame;
}

void AudioEngine::setEqBand(int slot, int band, int type, int freqHex, int gainHex, int qHex) {
    if (slot < 0 || slot >= 128 || band < 0 || band >= 3) return;
    auto& b = eqPresets[slot].bands[band];
    b.type   = type;
    b.freqHz = 20.0f * powf(1000.0f, freqHex / 255.0f);
    b.gainDb = gainHex / 10.0f - 12.0f;   // gainHex 0..240 → −12.0..+12.0 dB (0.1 dB/step)
    b.q      = 0.1f  * powf(100.0f,  qHex   / 255.0f);
}

void AudioEngine::setInstrumentEqSlot(int instrId, int slot) {
    if (instrId < 0 || instrId >= 256) return;
    if (slot < 0 || slot >= 128) {
        instrumentParams[instrId].eqActive = false;
        return;
    }
    const auto& preset = eqPresets[slot];
    bool any = false;
    for (int i = 0; i < 3; i++) {
        instrumentParams[instrId].eqBands[i] = preset.bands[i];
        if (preset.bands[i].type != 0) any = true;
    }
    instrumentParams[instrId].eqActive = any;
}

void AudioEngine::setInstrumentSendLevels(int instrId, int reverbSend, int delaySend) {
    if (instrId < 0 || instrId >= 256) return;
    instrumentParams[instrId].reverbSend = reverbSend / 255.0f;
    instrumentParams[instrId].delaySend  = delaySend  / 255.0f;
}

void AudioEngine::setReverbParams(int feedbackHex, int dampHex, int wetHex) {
    reverbSend.setParams(feedbackHex, dampHex);
    reverbReturnGain = wetHex / 255.0f;
}

void AudioEngine::setDelayParams(int timeOrSubdiv, int feedbackHex, bool syncMode, float bpm, int wetHex) {
    if (syncMode) {
        delaySend.setParamsSync(timeOrSubdiv, feedbackHex, bpm);
    } else {
        delaySend.setParamsFree(timeOrSubdiv, feedbackHex);
    }
    delayReturnGain = wetHex / 255.0f;
}

void AudioEngine::setDelayReverbSend(int sendHex) {
    delayToReverbSend = sendHex / 255.0f;
}

void AudioEngine::setReverbInputEq(int slot) {
    if (slot < 0 || slot >= 128) {
        reverbSend.inputEq.active = false;
        return;
    }
    const auto& preset = eqPresets[slot];
    reverbSend.inputEq.active = true;
    for (int b = 0; b < 3; b++)
        reverbSend.inputEq.bands[b].setParams(
            preset.bands[b].type, preset.bands[b].freqHz,
            preset.bands[b].gainDb, preset.bands[b].q);
}

void AudioEngine::setDelayInputEq(int slot) {
    if (slot < 0 || slot >= 128) {
        delaySend.inputEq.active = false;
        return;
    }
    const auto& preset = eqPresets[slot];
    delaySend.inputEq.active = true;
    for (int b = 0; b < 3; b++)
        delaySend.inputEq.bands[b].setParams(
            preset.bands[b].type, preset.bands[b].freqHz,
            preset.bands[b].gainDb, preset.bands[b].q);
}

void AudioEngine::setMasterEqSlot(int slot) {
    if (slot < 0 || slot >= 128) {
        masterChain.masterEq.active = false;
        return;
    }
    const auto& preset = eqPresets[slot];
    masterChain.masterEq.active = true;
    for (int b = 0; b < 3; b++)
        masterChain.masterEq.bands[b].setParams(
            preset.bands[b].type, preset.bands[b].freqHz,
            preset.bands[b].gainDb, preset.bands[b].q);
}

void AudioEngine::setInstrumentParams(int instrumentId, int start, int end, bool rev, int loop, int loopSt, int loopEn,
                                      int drv, int crsh, int dwn, int fType, int fCut, int fRes) {
    if (instrumentId < 0 || instrumentId >= 256) return;

    instrumentParams[instrumentId].startPoint = start;
    instrumentParams[instrumentId].endPoint = end;
    instrumentParams[instrumentId].reverse = rev;
    instrumentParams[instrumentId].loopMode = loop;
    instrumentParams[instrumentId].loopStart = loopSt;
    instrumentParams[instrumentId].loopEnd = loopEn;
    instrumentParams[instrumentId].drive = drv;
    instrumentParams[instrumentId].crush = crsh;
    instrumentParams[instrumentId].downsample = dwn;
    instrumentParams[instrumentId].filterType = fType;
    instrumentParams[instrumentId].filterCut = fCut;
    instrumentParams[instrumentId].filterRes = fRes;
    // ⚠️ A push of the instrument's own window ENDS any exact-frame window on this slot. That is what
    // makes the sample editor's audition safe to arm: whatever else happens, the next ordinary push —
    // the preview's own restore, an edit on the INSTRUMENT screen, a project load — takes it away, and
    // no caller has to know it was ever there.
    instrumentParams[instrumentId].startFrame = -1;
    instrumentParams[instrumentId].endFrame   = -1;
}

void AudioEngine::setInstrumentFrameWindow(int instrumentId, int startFrame, int endFrame) {
    if (instrumentId < 0 || instrumentId >= 256) return;
    const bool armed = (startFrame >= 0 && endFrame > startFrame);
    instrumentParams[instrumentId].startFrame = armed ? startFrame : -1;
    instrumentParams[instrumentId].endFrame   = armed ? endFrame   : -1;
}
