#pragma once
#include "../primitives/daisysp/decimator.h"

// Bit and sample-rate reduction (DaisySP Decimator), one per channel.
// crush 0-15: bits dropped (0 = off, 15 = 1-bit). downsample 0-15: 0 = off.
struct BitcrushModule {
    // A change on a sounding note crossfades over CHANGE_FRAMES: the new setting starts in the idle
    // pair while the old pair plays out (a Decimator has state and cannot be copied). Anything set
    // before the note's first sample is instant.
    static constexpr int CHANGE_FRAMES = 128;

    int crush      = 0;
    int downsample = 0;
    daisysp::Decimator decL[2];
    daisysp::Decimator decR[2];
    int  cur         = 0;       // the pair in force; the other is the one fading out
    bool prevEnabled = false;
    int  fadeLeft    = 0;
    bool fresh       = true;

    static void bypass(daisysp::Decimator& d) {
        d.Init();
        // Init leaves downsampling on.
        d.SetDownsampleFactor(0.0f);
        d.SetBitsToCrush(0);
    }

    void reset() {
        crush = 0;
        downsample = 0;
        for (auto& d : decL) bypass(d);
        for (auto& d : decR) bypass(d);
        cur = 0;
        fadeLeft = 0;
        fresh = true;
    }

    bool enabled() const { return crush > 0 || downsample > 0; }

    void setParams(int crushParam, int downsampleParam) {
        if (!fresh && (crushParam != crush || downsampleParam != downsample)) {
            prevEnabled = enabled();
            cur ^= 1;
            bypass(decL[cur]);
            bypass(decR[cur]);
            fadeLeft = CHANGE_FRAMES;
        }
        crush      = crushParam;
        downsample = downsampleParam;
        decL[cur].SetBitsToCrush(static_cast<uint8_t>(crush));
        decR[cur].SetBitsToCrush(static_cast<uint8_t>(crush));
        decL[cur].SetDownsampleFactor(static_cast<float>(downsample) / 15.0f);
        decR[cur].SetDownsampleFactor(static_cast<float>(downsample) / 15.0f);
    }

    inline float processMono(float in) {
        fresh = false;
        if (!enabled() && fadeLeft == 0) return in;
        float out = enabled() ? decL[cur].Process(in) : in;
        if (fadeLeft > 0) {
            const float k = static_cast<float>(fadeLeft) / CHANGE_FRAMES;
            out = (prevEnabled ? decL[cur ^ 1].Process(in) : in) * k + out * (1.0f - k);
            --fadeLeft;
        }
        return out;
    }

    inline void processStereo(float& L, float& R) {
        fresh = false;
        if (!enabled() && fadeLeft == 0) return;
        float outL = enabled() ? decL[cur].Process(L) : L, outR = enabled() ? decR[cur].Process(R) : R;
        if (fadeLeft > 0) {
            const float k = static_cast<float>(fadeLeft) / CHANGE_FRAMES;
            outL = (prevEnabled ? decL[cur ^ 1].Process(L) : L) * k + outL * (1.0f - k);
            outR = (prevEnabled ? decR[cur ^ 1].Process(R) : R) * k + outR * (1.0f - k);
            --fadeLeft;
        }
        L = outL;
        R = outR;
    }
};
