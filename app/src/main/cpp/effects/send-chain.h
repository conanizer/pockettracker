#pragma once

// ===========================================================================
// SendChain — stereo parallel effect bus (reverb, delay, or chorus slot).
// Currently a pass-through stub.
//
// Signal flow: dry input → chain modules → wet output (caller mixes dry + wet).
// Future module slots: ReverbModule, DelayModule, ChorusModule.
// ===========================================================================
struct SendChain {
    // Process one block. outL/outR receive the wet signal (caller adds to dry mix).
    void process(const float* /*inL*/, const float* /*inR*/,
                 float* outL, float* outR, int numFrames) {
        for (int i = 0; i < numFrames; i++) {
            outL[i] = 0.0f;
            outR[i] = 0.0f;
        }
    }
};
