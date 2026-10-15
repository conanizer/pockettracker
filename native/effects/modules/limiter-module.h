#pragma once
#include "../primitives/daisysp/limiter.h"

// The master bus's soft limiter (DaisySP), one per channel, run a sample at a time on the
// interleaved buffer. `preGain` above 1 drives it harder.
struct LimiterModule {
    daisysp::Limiter limL, limR;
    float preGain = 1.0f;

    void reset() {
        limL.Init();
        limR.Init();
    }

    void setPreGain(float g) { preGain = g; }

    void process(float* buf, int numFrames, int channelCount) {
        for (int i = 0; i < numFrames; i++) {
            limL.ProcessBlock(&buf[i * channelCount],     1, preGain);
            limR.ProcessBlock(&buf[i * channelCount + 1], 1, preGain);
        }
    }
};
