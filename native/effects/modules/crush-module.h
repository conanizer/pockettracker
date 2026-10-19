#pragma once
#include "../glide.h"
#include "../primitives/daisysp/decimator.h"

// Bit and sample-rate reduction (DaisySP Decimator), one per channel.
// crush 0-15: bits dropped (0 = off, 15 = 1-bit). downsample 0-15: 0 = off.
struct BitcrushModule {
    // The glide rule (glide.h) as a crossfade: the new setting starts clean in the idle pair while the
    // old pair plays out with its state.

    int crush      = 0;
    int downsample = 0;
    daisysp::Decimator decL[2];
    daisysp::Decimator decR[2];
    int  cur         = 0;       // the pair in force; the other is the one fading out
    bool prevEnabled = false;
    Ramp oldShare;              // the old pair's share of the output, 1 to 0
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
        oldShare = Ramp{};
        fresh = true;
    }

    bool enabled() const { return crush > 0 || downsample > 0; }

    void setParams(int crushParam, int downsampleParam) {
        if (!fresh && (crushParam != crush || downsampleParam != downsample)) {
            prevEnabled = enabled();
            cur ^= 1;
            bypass(decL[cur]);
            bypass(decR[cur]);
            oldShare.start(1.0f, 0.0f, GLIDE_SAMPLES);
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
        if (!enabled() && !oldShare.moving()) return in;
        float out = enabled() ? decL[cur].Process(in) : in;
        if (oldShare.moving()) {
            const float k = oldShare.next();
            out = (prevEnabled ? decL[cur ^ 1].Process(in) : in) * k + out * (1.0f - k);
        }
        return out;
    }

    inline void processStereo(float& L, float& R) {
        fresh = false;
        if (!enabled() && !oldShare.moving()) return;
        float outL = enabled() ? decL[cur].Process(L) : L, outR = enabled() ? decR[cur].Process(R) : R;
        if (oldShare.moving()) {
            const float k = oldShare.next();
            outL = (prevEnabled ? decL[cur ^ 1].Process(L) : L) * k + outL * (1.0f - k);
            outR = (prevEnabled ? decR[cur ^ 1].Process(R) : R) * k + outR * (1.0f - k);
        }
        L = outL;
        R = outR;
    }
};
