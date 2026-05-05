#pragma once
#include "../primitives/daisysp/delayline.h"
#include "eq-module.h"
#include <cmath>
#include <cstring>

// Max delay: 2 seconds at 44100 Hz (88200 samples per channel)
static constexpr size_t DELAY_MAX_SAMPLES = 88200;

// Subdivision beat fractions (in quarter-note beats) for sync mode.
// Index: 00=1/1  01=1/2  02=1/4  03=1/8  04=1/16  05=1/32
//        06=1/2T 07=1/4T 08=1/8T 09=1/4D 10=1/8D  11=1/16D
static const float kDelaySyncBeats[] = {
    4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f,
    2.0f / 3.0f, 1.0f / 3.0f, 1.0f / 6.0f,
    1.5f, 0.75f, 0.375f
};
static constexpr int kDelaySyncCount = 12;

// ===========================================================================
// DelayModule — stereo tap-delay send (DaisySP DelayLine).
//
// Takes a mono send-bus sum, writes identical delayed signal to L and R.
// inputEq is applied to the send input before writing to the delay line.
// ===========================================================================
struct DelayModule {
    daisysp::DelayLine<float, DELAY_MAX_SAMPLES> delL;
    daisysp::DelayLine<float, DELAY_MAX_SAMPLES> delR;
    EqModule inputEq;
    float    feedback   = 0.375f;
    float    sampleRate = 44100.0f;

    void reset(float sr) {
        sampleRate = sr;
        delL.Init();
        delR.Init();
        inputEq.reset(sr);
        feedback = 0x60 / 255.0f;
        // Default: 1/8 note at 120 BPM = 125 ms
        float defaultSamples = 0.125f * (60.0f / 120.0f) * sr;
        delL.SetDelay(defaultSamples);
        delR.SetDelay(defaultSamples);
    }

    // Free mode: timeHex 00-FF → 0–2 seconds
    void setParamsFree(int timeHex, int feedbackHex) {
        float samples = (timeHex / 255.0f) * 2.0f * sampleRate;
        samples = fmaxf(1.0f, fminf(samples, (float)(DELAY_MAX_SAMPLES - 1)));
        delL.SetDelay(samples);
        delR.SetDelay(samples);
        feedback = feedbackHex / 255.0f;
    }

    // Sync mode: subdivIdx 0–11 (see kDelaySyncBeats), BPM from project
    void setParamsSync(int subdivIdx, int feedbackHex, float bpm) {
        if (subdivIdx < 0 || subdivIdx >= kDelaySyncCount) subdivIdx = 2;
        float samples = kDelaySyncBeats[subdivIdx] * (60.0f / bpm) * sampleRate;
        samples = fmaxf(1.0f, fminf(samples, (float)(DELAY_MAX_SAMPLES - 1)));
        delL.SetDelay(samples);
        delR.SetDelay(samples);
        feedback = feedbackHex / 255.0f;
    }

    // Process stereo send bus into stereo wet output. Always 100% wet. Writes to outL/outR.
    // Each channel has its own delay line — panned instruments echo on the correct side.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numFrames) {
        for (int i = 0; i < numFrames; i++) {
            float l = inL[i], r = inR[i];
            if (inputEq.active) {
                float mid = (l + r) * 0.5f;
                float eqMid = inputEq.processMono(mid);
                if (fabsf(mid) > 1e-7f) { float g = eqMid / mid; l *= g; r *= g; }
            }
            float readL = delL.Read();
            float readR = delR.Read();
            delL.Write(l + readL * feedback);
            delR.Write(r + readR * feedback);
            outL[i] = readL;
            outR[i] = readR;
        }
    }
};
