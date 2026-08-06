#pragma once
#include "../primitives/daisysp/delayline.h"
#include "eq-module.h"
#include <cmath>
#include <cstring>

// Max delay line: 88200 samples per channel — 2 seconds at 44100 Hz, 1.84 at 48000, 0.92 at 96000.
//
// ⚠️ **IT IS A SAMPLE COUNT AND EVERY TIME BELOW IS IN SECONDS OR BEATS, so the ceiling is a TIME
// that moves with the device rate — and both setters CLAMP SILENTLY when they cross it.** The
// consequences, worth knowing before changing either:
//   * free mode's documented "00-FF -> 0-2 seconds" is 0-2 s only at 44.1 kHz;
//   * sync mode's longest subdivisions are unreachable at slow tempos even at 44.1 kHz — 1/1 is four
//     beats, i.e. exactly 2 s at 120 BPM and longer below it, so at 100 BPM a `00` delay runs 17%
//     early while the cell still reads `00`. 1/2 breaks the same way below 60 BPM, dotted-1/4 below 45.
//
// Growing it is not free: two `DelayLine<float, DELAY_MAX_SAMPLES>` are 706 KB, which is 48% of
// `sizeof(AudioEngine)`, so doubling the ceiling doubles that. The alternative — offering only the
// subdivisions the current tempo can actually produce — makes an FX cell's range depend on the
// project tempo, which collides with "a member's number is its identity". Left as it is deliberately;
// the constraint belongs in the manual.
static constexpr size_t DELAY_MAX_SAMPLES = 88200;

// Subdivision beat fractions (in quarter-note beats) for sync mode.
// Index: 00=1/1  01=1/2   02=1/4   03=1/8   04=1/16  05=1/32
//        06=1/4T 07=1/8T  08=1/16T 09=1/4.  10=1/8.  11=1/16.
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
// inputEq is applied to the stereo send input (independent L/R biquads) before writing to the delay line.
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
        // Default: 1/4 note at 120 BPM = 500 ms (index 2)
        float defaultSamples = 1.0f * (60.0f / 120.0f) * sr;
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
                inputEq.processStereo(l, r);
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
