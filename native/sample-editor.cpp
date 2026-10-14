// The sample editor's operations on a loaded sample. UI thread only.
#include "audio-engine.h"
#include "effects/primitives/sola-stretch.h"
#include <cmath>

// The undo and RATE caches are int16, half the RAM: they only restore the working buffer. Clamped
// (an over-unity sample restores at full scale), /32768 as the WAV decoder — exact for 16-bit WAVs.
static inline int16_t f32ToCacheI16(float f) {
    float c = std::min(1.0f, std::max(-1.0f, f));
    int   v = (int)std::lround(c * 32768.0f);
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}
static inline float cacheI16ToF32(int16_t v) { return v / 32768.0f; }

// Replaces `id`'s buffers (newR null for mono) and frees the old, keeping left, right and length in
// step. Every length change goes through this.
void AudioEngine::setSampleBuffers(int id, float* newL, float* newR, int newLen) {
    delete[] samples[id];
    delete[] samplesRight[id];
    samples[id]       = newL;
    samplesRight[id]  = newR;
    sampleLengths[id] = newLen;
    touchSample(id);   // after the stores: see sampleGen
}

// Takes the edit lock and ends every voice on slot `id`. ⚠️ Every destructive op holds it while it
// changes the buffers: the mix try_locks it and skips a block rather than read freed memory.
std::unique_lock<std::mutex> AudioEngine::beginSampleEdit(int id) {
    std::unique_lock<std::mutex> lock(sampleEditMutex);
    touchSample(id);
    return lock;
}

int AudioEngine::getSampleLength(int id) {
    if (id < 0 || id >= 256 || !samples[id]) return 0;
    return sampleLengths[id];
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
    // RIGHT with no right buffer reads left
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
                v = (bufL[i] + bufR[i]) * 0.5f;  // STEREO/MONO view
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
    // By slot and generation, not pointer: a new buffer can land at the old one's address.
    const uint32_t gen = sampleGen[id].load();
    const VoiceView& view = voiceView();
    for (int v = 0; v < MAX_VOICES; v++) {
        const VoiceView::Sampler& voice = view.sampler[v];
        if (voice.active && !voice.fading && voice.instrId == id && voice.sampleGen == gen) {
            return (float)(voice.position / (double)sampleLengths[id]);
        }
    }
    return -1.0f;
}

void AudioEngine::normalizeSample(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    // In place: no voice stop, but the lock, so no half-edited block plays. Same below.
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id].load(), endFrame);
    if (startFrame >= endFrame) return;
    float* bufR = samplesRight[id];
    // One peak over both channels keeps the stereo image.
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

void AudioEngine::fadeInSample(int id, int startFrame, int endFrame)  { fadeSample(id, startFrame, endFrame, false); }
void AudioEngine::fadeOutSample(int id, int startFrame, int endFrame) { fadeSample(id, startFrame, endFrame, true); }

void AudioEngine::fadeSample(int id, int startFrame, int endFrame, bool out) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id].load(), endFrame);
    int count  = endFrame - startFrame;
    if (count <= 0) return;
    float* bufR = samplesRight[id];
    for (int i = 0; i < count; i++) {
        float g = out ? 1.0f - (float)i / count : (float)i / count;
        samples[id][startFrame + i] *= g;
        if (bufR) bufR[startFrame + i] *= g;
    }
}

void AudioEngine::silenceRegion(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id].load(), endFrame);
    // ⚠️ An inverted window is reachable (START and END are independent cells); a negative length
    // would become ~2^64 in the memset.
    if (startFrame >= endFrame) return;
    std::memset(samples[id] + startFrame, 0, (endFrame - startFrame) * sizeof(float));
    if (samplesRight[id])
        std::memset(samplesRight[id] + startFrame, 0, (endFrame - startFrame) * sizeof(float));
}

void AudioEngine::reverseSample(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    std::lock_guard<std::mutex> lock(sampleEditMutex);
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id].load(), endFrame);
    // ⚠️ As silenceRegion: an inverted window would make std::reverse walk off both ends.
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
    auto editLock = beginSampleEdit(id);
    int len = sampleBackupLengths[id];
    float* newL = new float[len];
    for (int i = 0; i < len; i++) newL[i] = cacheI16ToF32(sampleBackups[id][i]);
    float* newR = nullptr;
    if (sampleBackupsRight[id]) {
        newR = new float[len];
        for (int i = 0; i < len; i++) newR[i] = cacheI16ToF32(sampleBackupsRight[id][i]);
    }
    setSampleBuffers(id, newL, newR, len);  // the backup's mono/stereo too
}

void AudioEngine::freeSampleUndo(int id) {
    if (id < 0 || id >= 256) return;
    // The mix never reads the backup, so no lock. The RATE cache stays — RATE needs it to restore.
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
        auto lock = beginSampleEdit(id);
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
    endFrame   = std::min(sampleLengths[id].load(), endFrame);
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
    instrumentParams.edit(id).startPoint = 0;
    instrumentParams.edit(id).endPoint   = 255;
    instrumentParams.publish(id);
}

void AudioEngine::deleteSampleRegion(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    auto editLock = beginSampleEdit(id);
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id].load(), endFrame);
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
    instrumentParams.edit(id).startPoint = 0;
    instrumentParams.edit(id).endPoint   = 255;
    instrumentParams.publish(id);
}

void AudioEngine::copyRegion(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    // The lock, so no edit swaps the buffers mid-copy — but not beginSampleEdit(): COPY must not cut
    // a preview.
    std::unique_lock<std::mutex> editLock(sampleEditMutex);
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id].load(), endFrame);
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

// Slot → slot for the editor's audition: one channel, the two mixed, or (mode 2) as is.
void AudioEngine::prepareSourcePreview(int dstId, int srcId, int mode) {
    if (dstId < 0 || dstId >= 256 || srcId < 0 || srcId >= 256 || dstId == srcId) return;
    // The one edit lock also holds the source steady.
    auto editLock = beginSampleEdit(dstId);
    if (!samples[srcId] || sampleLengths[srcId] <= 0) return;
    const int len  = sampleLengths[srcId];
    const float* L = samples[srcId];
    const float* R = samplesRight[srcId] ? samplesRight[srcId] : L;
    float* out = new (std::nothrow) float[len];
    if (!out) return;
    float* outR = nullptr;
    if (mode == 1) {
        std::memcpy(out, R, (size_t)len * sizeof(float));
    } else if (mode == 3) {
        for (int i = 0; i < len; i++) out[i] = (L[i] + R[i]) * 0.5f;
    } else {  // 0 = LEFT (a mono source's R aliases L); 2 = STEREO
        std::memcpy(out, L, (size_t)len * sizeof(float));
        if (mode == 2 && samplesRight[srcId]) {
            outR = new (std::nothrow) float[len];
            if (outR) std::memcpy(outR, R, (size_t)len * sizeof(float));
        }
    }
    setSampleBuffers(dstId, out, outR, len);
}

void AudioEngine::pasteRegion(int id, int insertAt) {
    if (id < 0 || id >= 256 || !samples[id] || !sampleClipboard || sampleClipboardLength <= 0) return;
    auto editLock = beginSampleEdit(id);
    insertAt = std::max(0, std::min(sampleLengths[id].load(), insertAt));
    int oldLen = sampleLengths[id];
    int clip   = sampleClipboardLength;
    int newLen = oldLen + clip;
    float* newL = new float[newLen];
    std::memcpy(newL,                   samples[id],            insertAt * sizeof(float));
    std::memcpy(newL + insertAt,        sampleClipboard,        clip * sizeof(float));
    std::memcpy(newL + insertAt + clip, samples[id] + insertAt, (oldLen - insertAt) * sizeof(float));
    float* newR = nullptr;
    if (samplesRight[id]) {
        // A mono clip into a stereo sample goes on both sides.
        const float* clipR = sampleClipboardRight ? sampleClipboardRight : sampleClipboard;
        newR = new float[newLen];
        std::memcpy(newR,                   samplesRight[id],            insertAt * sizeof(float));
        std::memcpy(newR + insertAt,        clipR,                       clip * sizeof(float));
        std::memcpy(newR + insertAt + clip, samplesRight[id] + insertAt, (oldLen - insertAt) * sizeof(float));
    }
    setSampleBuffers(id, newL, newR, newLen);
    instrumentParams.edit(id).startPoint = 0;
    instrumentParams.edit(id).endPoint   = 255;
    instrumentParams.publish(id);
}

int AudioEngine::getClipboardLength() {
    return sampleClipboardLength;
}

// The editor's BIT cell: round to the nearest step of a `bits`-bit grid, once, no dither — not CRUSH,
// which truncates per block. Mid-tread, so silence stays silence; exact in float; the top step clamps.
static inline float quantizeToBits(float v, int bits) {
    const float q   = static_cast<float>(1 << (bits - 1));
    const float top = (q - 1.0f) / q;
    float y = std::floor(v * q + 0.5f) / q;
    if (y > top)   y = top;
    if (y < -1.0f) y = -1.0f;
    return y;
}

void AudioEngine::freeRateCache(int id) {
    delete[] originalSamples[id];        originalSamples[id] = nullptr;
    delete[] originalSamplesRight[id];   originalSamplesRight[id] = nullptr;
    delete[] originalSamplesF[id];       originalSamplesF[id] = nullptr;
    delete[] originalSamplesRightF[id];  originalSamplesRightF[id] = nullptr;
    originalSampleLengths[id] = 0;
}

void AudioEngine::setSampleSourceFormat(int id, int bits, bool isFloat) {
    if (id < 0 || id >= 256) return;
    freeRateCache(id);
    sampleBitDepth[id] = static_cast<uint8_t>(bits);
    sampleIsFloat[id]  = isFloat;
}

int AudioEngine::getSampleBitDepth(int id) const {
    return (id >= 0 && id < 256) ? sampleBitDepth[id] : 16;
}

bool AudioEngine::isSampleFloat(int id) const {
    return id >= 0 && id < 256 && sampleIsFloat[id];
}

void AudioEngine::adoptSavedSampleFormat(int id, int bits, bool isFloat) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    std::lock_guard<std::mutex> lock(sampleEditMutex);   // the cache is freed under it
    setSampleSourceFormat(id, bits, isFloat);
}

void AudioEngine::applyRateAndBits(int id, int factor, int bits) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    if (factor < 1) factor = 1;
    const int source = sampleBitDepth[id];
    bits = std::max(1, std::min(source, bits));   // never above the source depth

    auto editLock = beginSampleEdit(id);

    const bool haveCache = originalSamples[id] || originalSamplesF[id];
    const auto cached = [&](bool right, int i) -> float {
        if (originalSamplesF[id]) return (right ? originalSamplesRightF[id] : originalSamplesF[id])[i];
        return cacheI16ToF32((right ? originalSamplesRight[id] : originalSamples[id])[i]);
    };
    // ⚠️ A function, not a value: before the first departure there is no cache, and a test now says mono.
    const auto stereoCache = [&] { return originalSamplesRightF[id] != nullptr || originalSamplesRight[id] != nullptr; };

    if (factor == 1 && bits == source) {
        // Back to HIGH at the source depth: restore the original and drop it.
        if (!haveCache) return;
        int len = originalSampleLengths[id];
        float* newL = new float[len];
        for (int i = 0; i < len; i++) newL[i] = cached(false, i);
        float* newR = nullptr;
        if (stereoCache()) {
            newR = new float[len];
            for (int i = 0; i < len; i++) newR[i] = cached(true, i);
        }
        setSampleBuffers(id, newL, newR, len);
        freeRateCache(id);
        return;
    }

    // The original, on the first departure: float when deeper than 16 bits, so 24/32 come back intact.
    if (!haveCache) {
        int len = sampleLengths[id];
        if (source > 16) {
            originalSamplesF[id] = new float[len];
            std::memcpy(originalSamplesF[id], samples[id], (size_t)len * sizeof(float));
            if (samplesRight[id]) {
                originalSamplesRightF[id] = new float[len];
                std::memcpy(originalSamplesRightF[id], samplesRight[id], (size_t)len * sizeof(float));
            }
        } else {
            originalSamples[id] = new int16_t[len];
            for (int i = 0; i < len; i++) originalSamples[id][i] = f32ToCacheI16(samples[id][i]);
            if (samplesRight[id]) {
                originalSamplesRight[id] = new int16_t[len];
                for (int i = 0; i < len; i++) originalSamplesRight[id][i] = f32ToCacheI16(samplesRight[id][i]);
            }
        }
        originalSampleLengths[id] = len;
    }

    // Always from the original, so any round trip is lossless.
    const bool requantise = bits < source;
    int newLen = originalSampleLengths[id] / factor;
    if (newLen < 1) return;
    float* newL = new float[newLen];
    for (int i = 0; i < newLen; i++) {
        const float v = cached(false, i * factor);
        newL[i] = requantise ? quantizeToBits(v, bits) : v;
    }
    float* newR = nullptr;
    if (stereoCache()) {
        newR = new float[newLen];
        for (int i = 0; i < newLen; i++) {
            const float v = cached(true, i * factor);
            newR[i] = requantise ? quantizeToBits(v, bits) : v;
        }
    }
    setSampleBuffers(id, newL, newR, newLen);
}

void AudioEngine::pitchShiftSample(int id, float semitones) {
    if (id < 0 || id >= 256 || !samples[id] || semitones == 0.0f) return;
    setFlushToZeroForCurrentThread();

    auto editLock = beginSampleEdit(id);

    // The result is the new original: the RATE cache goes.
    freeRateCache(id);

    float ratio  = std::pow(2.0f, semitones / 12.0f);
    int   oldLen = sampleLengths[id];
    int   newLen = std::max(1, (int)std::round((float)oldLen / ratio));

    // Linear resample, oldLen → newLen.
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

void AudioEngine::timeStretchSample(int id, float ratio, float grainMs, float sampleRate) {
    if (id < 0 || id >= 256 || !samples[id] || sampleLengths[id] <= 0) return;
    // ⚠️ In frames, not a ratio: 0.1 % of an 8 s loop is 8 ms, and SYNC must land on the tick.
    if ((int)std::lround((double)sampleLengths[id] * (double)ratio) == sampleLengths[id]) return;
    setFlushToZeroForCurrentThread();

    auto editLock = beginSampleEdit(id);

    // The result is the new original: the RATE cache goes.
    freeRateCache(id);

    int oldLen = sampleLengths[id];
    std::vector<float> outL = sola::stretch(samples[id], oldLen, ratio, sampleRate, grainMs);
    int newLen = (int)outL.size();
    float* newL = new float[newLen];
    std::copy(outL.begin(), outL.end(), newL);

    float* newR = nullptr;
    if (samplesRight[id]) {
        // Exactly newLen, whatever the stretch returned.
        std::vector<float> outR = sola::stretch(samplesRight[id], oldLen, ratio, sampleRate, grainMs);
        newR = new float[newLen];
        int n = std::min((int)outR.size(), newLen);
        std::copy(outR.begin(), outR.begin() + n, newR);
        for (int i = n; i < newLen; i++) newR[i] = 0.0f;
    }
    setSampleBuffers(id, newL, newR, newLen);
}

void AudioEngine::applySampleFx(int id, int fxType, int fxValue, float sampleRate, int limiterPreGain) {
    if (id < 0 || id >= 256 || !samples[id] || sampleLengths[id] <= 0) return;
    // ⚠️ Feedback DSP on the UI thread: without FTZ a fade-out sinks into denormals and freezes the screen.
    setFlushToZeroForCurrentThread();

    auto editLock = beginSampleEdit(id);

    int len = sampleLengths[id];

    // The effect + limiter on one channel, in place; fresh modules per channel.
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
            // ⚠️ DUST delays by `latencySamples()`; baked into a file that would shift the sound. So
            // it runs `len + latency` frames (silence past the end) and writes frame j back at
            // j - latency.
            const int latency = skdust::DustChain::latencySamples();
            // Safe in place while the latency is shorter than one chunk.
            static_assert(skdust::DustChain::latencySamples() < CHUNK,
                          "a latency of a chunk or more would overwrite unread input");

            skdust::DustChain dust;
            dust.prepare((double)sampleRate, CHUNK, 2);
            dust.setDustAmount(fxValue / 255.0f);
            for (int pos = 0; pos < len + latency; pos += CHUNK) {
                int n = std::min(CHUNK, len + latency - pos);
                for (int i = 0; i < n; i++) {
                    const int src = pos + i;
                    const float s = (src < len) ? buf[src] : 0.0f;
                    stereo[i * 2]     = s;
                    stereo[i * 2 + 1] = s;
                }
                dust.process(stereo, n, 2);
                for (int i = 0; i < n; i++) {
                    const int dst = pos + i - latency;
                    if (dst >= 0 && dst < len) buf[dst] = stereo[i * 2];
                }
            }
        } else if (fxType == 2) { // DRIVE
            DriveModule drive;
            drive.reset();
            drive.setDrive(fxValue);
            for (int i = 0; i < len; i++) buf[i] = drive.processMono(buf[i]);
        } else if (fxType == 3) { // EQ — apply preset slot (fxValue = slot 0-127)
            int slot = std::min(fxValue, 127);
            const EqPresetBank& preset = eqPresets.staged(slot);
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

        // The master limiter's pre-gain, keeping the result within ±1.
        LimiterModule lim;
        lim.reset();
        lim.setPreGain(1.0f + (limiterPreGain / 255.0f) * 3.0f);
        lim.limL.ProcessBlock(buf, static_cast<size_t>(len), lim.preGain);
    };

    applyToChannel(samples[id]);
    if (samplesRight[id]) applyToChannel(samplesRight[id]);
}

int AudioEngine::findZeroCrossing(int id, int frame, int dir, int searchRadius, int sourceMode) {
    if (id < 0 || id >= 256 || !samples[id] || sampleLengths[id] < 2) return frame;
    const float* L   = samples[id];
    const float* R   = samplesRight[id];
    const int    len = sampleLengths[id];

    // SOURCE (0 LEFT, 1 RIGHT, 2 STEREO, 3 MONO) names the signal the cut lands in; a mono sample is LEFT.
    const int mode = R ? sourceMode : 0;

    // STEREO: a frame where EITHER channel crosses is a candidate; the best has the quietest louder
    // channel. On dual-mono the first left crossing wins, as the left-only search.
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
        if (!stereo) return 0.0f;   // one signal: the nearest wins
        return std::max(std::abs(L[i]), std::abs(R[i]));
    };

    int   best      = frame;
    float bestScore = 0.0f;
    bool  found     = false;

    // True when the search can stop: at the first candidate for one signal; stereo keeps looking.
    auto consider = [&](int i) {
        if (i < 1 || i >= len || !crosses(i)) return false;
        const float sc = score(i);
        if (!found || sc < bestScore) { bestScore = sc; best = i; found = true; }
        return !stereo;
    };

    // dir > 0 forward, < 0 back, 0 nearest. Never behind `frame`, so a marker cannot snap back and
    // stick; ties go to the nearest.
    for (int d = 0; d <= searchRadius; d++) {
        if (dir >= 0 && consider(frame + d)) break;
        if (dir <= 0 && consider(frame - d)) break;
    }
    return found ? best : frame;
}

void AudioEngine::setEqBand(int slot, int band, int type, int freqHex, int gainHex, int qHex) {
    if (slot < 0 || slot >= 128 || band < 0 || band >= 3) return;
    auto& b = eqPresets.edit(slot).bands[band];
    b.type   = type;
    b.freqHz = 20.0f * powf(1000.0f, freqHex / 255.0f);
    b.gainDb = gainHex / 10.0f - 12.0f;   // gainHex 0..240 → −12.0..+12.0 dB (0.1 dB/step)
    b.q      = 0.1f  * powf(100.0f,  qHex   / 255.0f);
    // The authored bytes too — a table morph interpolates them. The only writer of either array.
    auto& h = eqPresetHex.edit(slot);
    h.type[band] = type;
    h.freq[band] = freqHex;
    h.gain[band] = gainHex;
    h.q[band]    = qHex;
    eqPresets.publish(slot);
    eqPresetHex.publish(slot);
}

void AudioEngine::setInstrumentEqSlot(int instrId, int slot) {
    if (instrId < 0 || instrId >= 256) return;
    InstrumentParams& ip = instrumentParams.edit(instrId);
    if (slot < 0 || slot >= 128) {
        ip.eqActive = false;
    } else {
        const EqPresetBank& preset = eqPresets.staged(slot);
        bool any = false;
        for (int i = 0; i < 3; i++) {
            ip.eqBands[i] = preset.bands[i];
            if (preset.bands[i].type != 0) any = true;
        }
        ip.eqActive = any;
    }
    instrumentParams.publish(instrId);
}

void AudioEngine::setProgram(int instrumentId, const songcore::Program& program,
                             const int64_t* markers, int count) {
    if (instrumentId < 0 || instrumentId >= songcore::PROGRAM_SLOTS) return;
    ProgramRow& row = programs.edit(instrumentId);
    const int n = (count < 0) ? 0
                : (count > songcore::PROGRAM_SLICE_MARKERS ? songcore::PROGRAM_SLICE_MARKERS : count);
    row.program = program;
    for (int i = 0; i < n; ++i) row.markers[i] = markers[i];
    row.program.sliceCount   = n;
    row.program.sliceMarkers = nullptr;   // programView points it at the audio thread's copy
    programs.publish(instrumentId);
}

void AudioEngine::setInstrumentSendLevels(int instrId, int reverbHex, int delayHex) {
    if (instrId < 0 || instrId >= 256) return;
    instrumentParams.edit(instrId).reverbSend = reverbHex / 255.0f;
    instrumentParams.edit(instrId).delaySend  = delayHex  / 255.0f;
    instrumentParams.publish(instrId);
}

void AudioEngine::setReverbParams(int feedbackHex, int dampHex, int wetHex, int sizeHex) {
    busSettings.reverbDecay = feedbackHex; busSettings.reverbDamp = dampHex;
    busSettings.reverbWet   = wetHex;      busSettings.reverbSize = sizeHex;
    prepareReverb();
    recordBus(BUS_REVERB_PARAMS);
    reverbReturnGain = wetHex / 255.0f;
}

void AudioEngine::setDelayParams(int timeOrSubdiv, int feedbackHex, bool syncMode, float bpm, int wetHex) {
    setDelayTime(timeOrSubdiv, syncMode, bpm);
    setDelayFeedbackWet(feedbackHex, wetHex);
}

// Separate halves: while a TIM drives the time, a globals push still sets FDBK and WET.
void AudioEngine::setDelayTime(int timeOrSubdiv, bool syncMode, float bpm) {
    busSettings.delayTime = timeOrSubdiv; busSettings.delaySync = syncMode; busSettings.delayBpm = bpm;
    recordBus(BUS_DELAY_TIME);
}

void AudioEngine::setDelayFeedbackWet(int feedbackHex, int wetHex) {
    busSettings.delayFeedback = feedbackHex; busSettings.delayWet = wetHex;
    recordBus(BUS_DELAY_FEEDBACK);
    delayReturnGain = wetHex / 255.0f;
}

void AudioEngine::setDelayCharacter(bool pong, int toneHex, int wobbleHex) {
    busSettings.delayPong = pong; busSettings.delayTone = toneHex; busSettings.delayWobble = wobbleHex;
    recordBus(BUS_DELAY_CHAR);
}

void AudioEngine::setDelayReverbSend(int sendHex) {
    busSettings.delayReverbSend = sendHex; busSettings.pushed = true;
    delayToReverbSend = sendHex / 255.0f;
}

void AudioEngine::setReverbCharacter(int preHex, int widthHex, int modHex) {
    busSettings.reverbPre = preHex; busSettings.reverbWidth = widthHex; busSettings.reverbMod = modHex;
    prepareReverb();
    recordBus(BUS_REVERB_CHAR);
}

void AudioEngine::setReverbAlgo(int algo) {
    busSettings.reverbAlgo = algo;
    prepareReverb();
    recordBus(BUS_REVERB_ALGO);
}

void AudioEngine::setReverbInputEq(int slot) {
    busSettings.reverbInputEq = slot;
    recordBus(BUS_REVERB_INEQ);
}

void AudioEngine::setDelayInputEq(int slot) {
    busSettings.delayInputEq = slot;
    recordBus(BUS_DELAY_INEQ);
}

void AudioEngine::setMasterEqSlot(int slot) {
    busSettings.masterEqSlot = slot;
    recordBus(BUS_MASTER_EQ);
}

void AudioEngine::setInstrumentParams(int instrumentId, int start, int end, bool rev, int loop, int loopSt, int loopEn,
                                      int drv, int crsh, int dwn, int fType, int fCut, int fRes) {
    if (instrumentId < 0 || instrumentId >= 256) return;

    instrumentParams.edit(instrumentId).startPoint = start;
    instrumentParams.edit(instrumentId).endPoint = end;
    instrumentParams.edit(instrumentId).reverse = rev;
    instrumentParams.edit(instrumentId).loopMode = loop;
    instrumentParams.edit(instrumentId).loopStart = loopSt;
    instrumentParams.edit(instrumentId).loopEnd = loopEn;
    instrumentParams.edit(instrumentId).drive = drv;
    instrumentParams.edit(instrumentId).crush = crsh;
    instrumentParams.edit(instrumentId).downsample = dwn;
    instrumentParams.edit(instrumentId).filterType = fType;
    instrumentParams.edit(instrumentId).filterCut = fCut;
    instrumentParams.edit(instrumentId).filterRes = fRes;
    // Ends any exact-frame window, so the editor's audition never outlives the next ordinary push.
    instrumentParams.edit(instrumentId).startFrame = -1;
    instrumentParams.edit(instrumentId).endFrame   = -1;
    instrumentParams.publish(instrumentId);
}

void AudioEngine::setInstrumentFrameWindow(int instrumentId, int startFrame, int endFrame) {
    if (instrumentId < 0 || instrumentId >= 256) return;
    const bool armed = (startFrame >= 0 && endFrame > startFrame);
    instrumentParams.edit(instrumentId).startFrame = armed ? startFrame : -1;
    instrumentParams.edit(instrumentId).endFrame   = armed ? endFrame   : -1;
    instrumentParams.publish(instrumentId);
}
