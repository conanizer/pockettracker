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
    float             sampleRate = 44100.0f;   // the rate the delay lines were actually built at

    // ⚠️ ReverbSc carves all eight delay lines out of ONE fixed array, sized for exactly this rate,
    // and refuses any rate whose lines will not fit. Its refusal leaves three buffer pointers
    // indeterminate, so the return value is not optional: `Process` would dereference them.
    //
    // A device above the ceiling gets a reverb built at the ceiling instead: shorter and brighter
    // than intended, which is the same compromise the whole engine ran on before the buses learned
    // the device rate at all, and much better than no reverb. `sampleRate` records what was really
    // used, so nobody reads it as the device rate.
    static constexpr float MAX_SUPPORTED_RATE = DSY_REVERBSC_MAX_RATE;

    void reset(float sr) {
        sampleRate = sr;
        if (reverb.Init(sr) != 0) {
            sampleRate = MAX_SUPPORTED_RATE;
            reverb.Init(MAX_SUPPORTED_RATE);
        }
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
