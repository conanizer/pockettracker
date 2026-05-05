#pragma once
#include "modules/limiter-module.h"
#include "modules/ott-module.h"
#include "modules/dust-chain.h"
#include "modules/eq-module.h"

// ===========================================================================
// MasterChain — final stereo output bus effect chain.
//
// Signal flow: mixed output → masterEq → [OttModule | DustChain] → LimiterModule → output.
// Only one bus effect runs at a time; masterFx selects which (0=OTT, 1=DUST).
// OttModule is bypassed when ott.enabled == false (depth = 0).
// DustChain is bypassed when dustDepth == 0.
// masterEq is bypassed when masterEq.active == false (slot = -1).
// ===========================================================================
struct MasterChain {
    OttModule         ott;
    skdust::DustChain dust;
    LimiterModule     limiter;
    EqModule          masterEq;  // Master 3-band parametric EQ (slot -1 = bypass)

    int   masterFx  = 0;
    float dustDepth = 0.f;

    void reset(float sampleRate = 44100.0f) {
        ott.reset(sampleRate);
        dust.prepare(sampleRate, 512, 2);
        dust.reset();
        limiter.reset();
        masterEq.reset(sampleRate);
    }

    void setMasterFx(int fx) {
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

    // Process interleaved stereo buffer in-place. buf layout: [L0, R0, L1, R1, ...]
    void process(float* buf, int numFrames, int channelCount) {
        // Master EQ (before bus FX)
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
