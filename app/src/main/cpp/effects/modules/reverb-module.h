#pragma once
#include "../primitives/daisysp/reverbsc.h"
#include "eq-module.h"
#include <cmath>

// ===========================================================================
// ReverbModule — Schroeder-Moorer stereo reverb send (DaisySP ReverbSc).
//
// Takes a mono send-bus sum, expands to stereo wet output.
// inputEq is a pre-reverb EQ band (applied before the reverb algorithm).
// ===========================================================================
struct ReverbModule {
    daisysp::ReverbSc reverb;
    EqModule          inputEq;
    float             sampleRate = 44100.0f;

    void reset(float sr) {
        sampleRate = sr;
        reverb.Init(sr);
        reverb.SetFeedback(0x60 / 255.0f);
        reverb.SetLpFreq(200.0f * powf(100.0f, 0x80 / 255.0f));
        inputEq.reset(sr);
    }

    // feedbackHex 00-FF → 0.0–1.0; dampHex 00-FF → LP 200Hz–20kHz
    void setParams(int feedbackHex, int dampHex) {
        reverb.SetFeedback(feedbackHex / 255.0f);
        reverb.SetLpFreq(200.0f * powf(100.0f, dampHex / 255.0f));
    }

    // Process stereo send bus into stereo wet output. Always 100% wet. Writes to outL/outR.
    // inputEq applied stereo (independent L/R biquads) before the reverb algorithm.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numFrames) {
        for (int i = 0; i < numFrames; i++) {
            float l = inL[i], r = inR[i];
            if (inputEq.active) {
                inputEq.processStereo(l, r);
            }
            float wl, wr;
            reverb.Process(l, r, &wl, &wr);
            outL[i] = wl;
            outR[i] = wr;
        }
    }
};
