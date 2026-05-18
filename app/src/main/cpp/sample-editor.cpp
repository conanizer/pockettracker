// sample-editor.cpp — AudioEngine sample editor operations (non-realtime)
// All methods here are called from the UI thread, never from the audio callback.
#include "audio-engine.h"
#include "effects/primitives/sola-stretch.h"

// ============================================================
// SAMPLE EDITOR OPERATIONS
// ============================================================

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
            return voice.position / (float)sampleLengths[id];
        }
    }
    return -1.0f;
}

void AudioEngine::normalizeSample(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    if (startFrame >= endFrame) return;
    float peak = 0.0f;
    for (int i = startFrame; i < endFrame; i++) {
        float v = std::abs(samples[id][i]);
        if (v > peak) peak = v;
    }
    if (peak < 0.0001f) return;
    float gain = 1.0f / peak;
    for (int i = startFrame; i < endFrame; i++) samples[id][i] *= gain;
}

void AudioEngine::fadeInSample(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    int count  = endFrame - startFrame;
    if (count <= 0) return;
    for (int i = 0; i < count; i++)
        samples[id][startFrame + i] *= (float)i / count;
}

void AudioEngine::fadeOutSample(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    int count  = endFrame - startFrame;
    if (count <= 0) return;
    for (int i = 0; i < count; i++)
        samples[id][startFrame + i] *= 1.0f - (float)i / count;
}

void AudioEngine::silenceRegion(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    std::memset(samples[id] + startFrame, 0, (endFrame - startFrame) * sizeof(float));
}

void AudioEngine::reverseSample(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    std::reverse(samples[id] + startFrame, samples[id] + endFrame);
}

void AudioEngine::backupSample(int id) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    delete[] sampleBackups[id];
    int len = sampleLengths[id];
    sampleBackups[id] = new float[len];
    std::memcpy(sampleBackups[id], samples[id], len * sizeof(float));
    sampleBackupLengths[id] = len;
}

void AudioEngine::undoSample(int id) {
    if (id < 0 || id >= 256 || !sampleBackups[id]) return;
    delete[] samples[id];
    int len = sampleBackupLengths[id];
    samples[id] = new float[len];
    std::memcpy(samples[id], sampleBackups[id], len * sizeof(float));
    sampleLengths[id] = len;
}

void AudioEngine::saveFxPreviewBackup(int id) {
    if (id < 0 || id >= 256 || !samples[id] || sampleLengths[id] <= 0) return;
    delete[] fxPreviewBackup;
    int len = sampleLengths[id];
    fxPreviewBackup    = new float[len];
    std::memcpy(fxPreviewBackup, samples[id], len * sizeof(float));
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
    }
    delete[] fxPreviewBackup;
    fxPreviewBackup    = nullptr;
    fxPreviewBackupLen = 0;
    fxPreviewBackupId  = -1;
}

void AudioEngine::cropSample(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    if (startFrame >= endFrame) return;
    int newLen = endFrame - startFrame;
    float* buf = new float[newLen];
    std::memcpy(buf, samples[id] + startFrame, newLen * sizeof(float));
    delete[] samples[id];
    samples[id] = buf;
    sampleLengths[id] = newLen;
    instrumentParams[id].startPoint = 0;
    instrumentParams[id].endPoint   = 255;
}

void AudioEngine::deleteSampleRegion(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    if (startFrame >= endFrame) return;
    int oldLen = sampleLengths[id];
    int newLen = oldLen - (endFrame - startFrame);
    if (newLen <= 0) return;
    float* buf = new float[newLen];
    std::memcpy(buf,               samples[id],             startFrame * sizeof(float));
    std::memcpy(buf + startFrame,  samples[id] + endFrame,  (oldLen - endFrame) * sizeof(float));
    delete[] samples[id];
    samples[id] = buf;
    sampleLengths[id] = newLen;
    instrumentParams[id].startPoint = 0;
    instrumentParams[id].endPoint   = 255;
}

void AudioEngine::copyRegion(int id, int startFrame, int endFrame) {
    if (id < 0 || id >= 256 || !samples[id]) return;
    startFrame = std::max(0, startFrame);
    endFrame   = std::min(sampleLengths[id], endFrame);
    if (startFrame >= endFrame) return;
    int len = endFrame - startFrame;
    delete[] sampleClipboard;
    sampleClipboard = new float[len];
    std::memcpy(sampleClipboard, samples[id] + startFrame, len * sizeof(float));
    sampleClipboardLength = len;
}

void AudioEngine::pasteRegion(int id, int insertAt) {
    if (id < 0 || id >= 256 || !samples[id] || !sampleClipboard || sampleClipboardLength <= 0) return;
    insertAt = std::max(0, std::min(sampleLengths[id], insertAt));
    int oldLen = sampleLengths[id];
    int newLen = oldLen + sampleClipboardLength;
    float* buf = new float[newLen];
    std::memcpy(buf,                             samples[id],          insertAt * sizeof(float));
    std::memcpy(buf + insertAt,                  sampleClipboard,      sampleClipboardLength * sizeof(float));
    std::memcpy(buf + insertAt + sampleClipboardLength, samples[id] + insertAt, (oldLen - insertAt) * sizeof(float));
    delete[] samples[id];
    samples[id] = buf;
    sampleLengths[id] = newLen;
    instrumentParams[id].startPoint = 0;
    instrumentParams[id].endPoint   = 255;
}

int AudioEngine::getClipboardLength() {
    return sampleClipboardLength;
}

void AudioEngine::downsampleSample(int id, int factor) {
    if (id < 0 || id >= 256 || !samples[id] || factor <= 1) return;
    int oldLen = sampleLengths[id];
    int newLen = oldLen / factor;
    if (newLen < 1) return;
    float* newBuf = new float[newLen];
    for (int i = 0; i < newLen; i++) newBuf[i] = samples[id][i * factor];
    delete[] samples[id];
    samples[id] = newBuf;
    sampleLengths[id] = newLen;
    // Instrument start/end points (0-255 fraction) map to the same relative positions,
    // so they remain valid after decimation without adjustment.
}

void AudioEngine::applyRateMode(int id, int factor) {
    if (id < 0 || id >= 256 || !samples[id]) return;

    // Stop any voice currently reading this sample so the callback won't be mid-read
    // when we swap the buffer pointer below.
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].sampleData == samples[id]) voices[v].stop();
    }

    // Hold the edit lock so the callback's try_lock fails during the swap, giving the
    // callback one silent period rather than a use-after-free crash.
    std::lock_guard<std::mutex> lock(sampleEditMutex);

    if (factor <= 1) {
        // Restore HIGH: copy original back, then discard cache.
        if (!originalSamples[id]) return; // Already at HIGH, nothing to restore.
        delete[] samples[id];
        samples[id] = new float[originalSampleLengths[id]];
        std::memcpy(samples[id], originalSamples[id], originalSampleLengths[id] * sizeof(float));
        sampleLengths[id] = originalSampleLengths[id];
        delete[] originalSamples[id];
        originalSamples[id] = nullptr;
        originalSampleLengths[id] = 0;
    } else {
        // Store original on first rate change away from HIGH.
        if (!originalSamples[id]) {
            originalSamples[id] = new float[sampleLengths[id]];
            std::memcpy(originalSamples[id], samples[id], sampleLengths[id] * sizeof(float));
            originalSampleLengths[id] = sampleLengths[id];
        }
        // Always derive from the cached original so NORM→LOFI→NORM roundtrips are lossless.
        int newLen = originalSampleLengths[id] / factor;
        if (newLen < 1) return;
        delete[] samples[id];
        samples[id] = new float[newLen];
        for (int i = 0; i < newLen; i++) samples[id][i] = originalSamples[id][i * factor];
        sampleLengths[id] = newLen;
    }
}

void AudioEngine::pitchShiftSample(int id, float semitones) {
    if (id < 0 || id >= 256 || !samples[id] || semitones == 0.0f) return;

    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].sampleData == samples[id]) voices[v].stop();
    }

    std::lock_guard<std::mutex> lock(sampleEditMutex);

    // Pitch shift makes this buffer the new "original"; discard any RATE cache.
    if (originalSamples[id]) {
        delete[] originalSamples[id];
        originalSamples[id] = nullptr;
        originalSampleLengths[id] = 0;
    }

    float ratio  = std::pow(2.0f, semitones / 12.0f);
    int   oldLen = sampleLengths[id];
    int   newLen = std::max(1, (int)std::round((float)oldLen / ratio));

    float* newBuf = new float[newLen];
    for (int i = 0; i < newLen; i++) {
        float srcPos = i * ratio;
        int   srcIdx = (int)srcPos;
        float frac   = srcPos - srcIdx;
        float s0 = (srcIdx     < oldLen) ? samples[id][srcIdx]     : 0.0f;
        float s1 = (srcIdx + 1 < oldLen) ? samples[id][srcIdx + 1] : s0;
        newBuf[i] = s0 + (s1 - s0) * frac;
    }

    delete[] samples[id];
    samples[id]       = newBuf;
    sampleLengths[id] = newLen;
}

void AudioEngine::timeStretchSample(int id, float ratio) {
    if (id < 0 || id >= 256 || !samples[id] || sampleLengths[id] <= 0) return;
    if (ratio > 0.999f && ratio < 1.001f) return;

    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].sampleData == samples[id]) voices[v].stop();
    }

    std::lock_guard<std::mutex> lock(sampleEditMutex);

    // Time-stretch makes this buffer the new "original"; discard any RATE cache.
    if (originalSamples[id]) {
        delete[] originalSamples[id];
        originalSamples[id] = nullptr;
        originalSampleLengths[id] = 0;
    }

    int oldLen = sampleLengths[id];
    std::vector<float> newL = sola::stretch(samples[id], oldLen, ratio, 44100.0f);
    int newLen = (int)newL.size();

    delete[] samples[id];
    samples[id] = new float[newLen];
    std::copy(newL.begin(), newL.end(), samples[id]);
    sampleLengths[id] = newLen;

    if (samplesRight[id]) {
        std::vector<float> newR = sola::stretch(samplesRight[id], oldLen, ratio, 44100.0f);
        int rLen = (int)newR.size();
        delete[] samplesRight[id];
        samplesRight[id] = new float[rLen];
        std::copy(newR.begin(), newR.end(), samplesRight[id]);
    }
}

void AudioEngine::applySampleFx(int id, int fxType, int fxValue, float sampleRate) {
    if (id < 0 || id >= 256 || !samples[id] || sampleLengths[id] <= 0) return;

    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].sampleData == samples[id]) voices[v].stop();
    }

    std::lock_guard<std::mutex> lock(sampleEditMutex);

    float* buf = samples[id];
    int    len = sampleLengths[id];

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
}

int AudioEngine::findZeroCrossing(int id, int frame, int searchRadius) {
    if (id < 0 || id >= 256 || !samples[id] || sampleLengths[id] < 2) return frame;
    const float* buf = samples[id];
    const int    len = sampleLengths[id];
    // Scan outward from `frame` in both directions simultaneously.
    for (int d = 0; d <= searchRadius; d++) {
        int fwd = frame + d;
        int bwd = frame - d;
        if (fwd >= 1 && fwd < len &&
            ((buf[fwd - 1] < 0.0f) != (buf[fwd] < 0.0f))) return fwd;
        if (d > 0 && bwd >= 1 && bwd < len &&
            ((buf[bwd - 1] < 0.0f) != (buf[bwd] < 0.0f))) return bwd;
    }
    return frame;
}

void AudioEngine::setEqBand(int slot, int band, int type, int freqHex, int gainHex, int qHex) {
    if (slot < 0 || slot >= 128 || band < 0 || band >= 3) return;
    auto& b = eqPresets[slot].bands[band];
    b.type   = type;
    b.freqHz = 20.0f * powf(1000.0f, freqHex / 255.0f);
    b.gainDb = (gainHex / 255.0f) * 24.0f - 12.0f;
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

void AudioEngine::setInstrumentParams(int instrumentId, int start, int end, bool rev, int loop, int loopSt,
                                      int drv, int crsh, int dwn, int fType, int fCut, int fRes) {
    if (instrumentId < 0 || instrumentId >= 256) return;

    instrumentParams[instrumentId].startPoint = start;
    instrumentParams[instrumentId].endPoint = end;
    instrumentParams[instrumentId].reverse = rev;
    instrumentParams[instrumentId].loopMode = loop;
    instrumentParams[instrumentId].loopStart = loopSt;
    instrumentParams[instrumentId].drive = drv;
    instrumentParams[instrumentId].crush = crsh;
    instrumentParams[instrumentId].downsample = dwn;
    instrumentParams[instrumentId].filterType = fType;
    instrumentParams[instrumentId].filterCut = fCut;
    instrumentParams[instrumentId].filterRes = fRes;

    LOGD("Instrument %d params: start=%d, end=%d, rev=%d, loop=%d, loopStart=%d, drive=%d, crush=%d, downsample=%d, filter=%d, cut=%d, res=%d",
         instrumentId, start, end, rev, loop, loopSt, drv, crsh, dwn, fType, fCut, fRes);
}
