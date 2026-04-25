#pragma once
#include "modules/limiter-module.h"
#include "modules/ott-module.h"

// ===========================================================================
// MasterChain — final stereo output bus effect chain.
//
// Signal flow: mixed output → OttModule → LimiterModule → final output.
// OTT runs before the limiter so upward compression can't push past 0 dBFS.
// OttModule is bypassed (no DSP) when ott.enabled == false (depth = 0).
//
// Adding a new module:
//   1. Add a member of the module type.
//   2. Call module.reset() in reset().
//   3. Call module.process() in process() at the right position.
//   Call sites in audio-engine.cpp do not change.
// ===========================================================================
struct MasterChain {
    OttModule     ott;
    LimiterModule limiter;

    // sampleRate defaults to 44100 — matches the forced Oboe sample rate.
    // The constructor call in AudioEngine() uses the default and stays unchanged.
    void reset(float sampleRate = 44100.0f) {
        ott.reset(sampleRate);
        limiter.reset();
    }

    // Process interleaved stereo buffer in-place. buf layout: [L0, R0, L1, R1, ...]
    void process(float* buf, int numFrames, int channelCount) {
        if (ott.enabled) ott.process(buf, numFrames, channelCount);
        limiter.process(buf, numFrames, channelCount);
    }
};
