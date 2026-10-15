#pragma once
#include "modules/limiter-module.h"
#include "modules/ott-module.h"
#include "modules/dust-chain.h"
#include "modules/eq-module.h"

// The master bus: EQ → OTT or DUST (`masterFx` 0 / 1, skipped at depth 0) → limiter.
struct MasterChain {
    OttModule         ott;
    skdust::DustChain dust;
    LimiterModule     limiter;
    EqModule          masterEq;

    int   masterFx  = 0;
    float dustDepth = 0.f;

    // No default rate: every module bakes it in.
    void reset(float sampleRate) {
        ott.reset(sampleRate);
        dust.prepare(sampleRate, 512, 2);
        dust.reset();
        limiter.reset();
        masterEq.reset(sampleRate);
    }

    // ⚠️ Idempotent: it is re-sent on every mixer gesture, and resetting DUST mid-signal clicks.
    void setMasterFx(int fx) {
        if (fx == masterFx) return;
        masterFx = fx;
        if (fx == 1) {
            dust.reset();
            dust.setDustAmount(dustDepth);
        }
    }

    void setDustDepth(float d) {
        dustDepth = d;
        dust.setDustAmount(d);
    }

    void setDustDepthForRender(float d) {
        dustDepth = d;
        dust.reset();
        dust.setDustAmount(d);
    }

    void setLimiterPreGain(float g) { limiter.setPreGain(g); }

    // Interleaved stereo, in place.
    void process(float* buf, int numFrames, int channelCount) {
        if (masterEq.active) {
            for (int i = 0; i < numFrames; i++) {
                float L = buf[i * channelCount];
                float R = buf[i * channelCount + 1];
                masterEq.processStereo(L, R);
                buf[i * channelCount]     = L;
                buf[i * channelCount + 1] = R;
            }
        }
        if (masterFx == 0) {
            if (ott.enabled) ott.process(buf, numFrames, channelCount);
        } else {
            if (dustDepth > 0.f) dust.process(buf, numFrames, channelCount);
        }
        limiter.process(buf, numFrames, channelCount);
    }
};
