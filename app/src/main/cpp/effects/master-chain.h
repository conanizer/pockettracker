#pragma once
#include "modules/limiter-module.h"

// ===========================================================================
// MasterChain — final stereo output bus effect chain.
//
// Signal flow: mixed output → LimiterModule → final output.
//
// Adding a new module:
//   1. Add a member of the module type.
//   2. Call module.reset() in reset().
//   3. Call module.process() in process() at the right position.
//   Call sites in audio-engine.cpp do not change.
//
// Future module slots: EQModule, CompressorModule, OTTModule.
// ===========================================================================
struct MasterChain {
    LimiterModule limiter;

    void reset() {
        limiter.reset();
    }

    // Process interleaved stereo buffer in-place. buf layout: [L0, R0, L1, R1, ...]
    void process(float* buf, int numFrames, int channelCount) {
        limiter.process(buf, numFrames, channelCount);
    }
};
