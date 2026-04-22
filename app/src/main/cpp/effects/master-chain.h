#pragma once

// ===========================================================================
// MasterChain — final stereo output bus effect chain.
// Currently a pass-through stub.
//
// Signal flow: mixed output → chain modules → final output.
// Future module slots: EQModule, CompressorModule, OTTModule, LimiterModule.
// The brickwall limiter inline in processAudioBlock will move here when implemented.
// ===========================================================================
struct MasterChain {
    // Process interleaved stereo buffer in-place. buf layout: [L0, R0, L1, R1, ...]
    void process(float* /*buf*/, int /*numFrames*/, int /*channelCount*/) {
        // stub — no-op until first master module is implemented
    }
};
