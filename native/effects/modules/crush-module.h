#pragma once
#include "../primitives/daisysp/decimator.h"

// ===========================================================================
// BitcrushModule — bit-depth reduction + sample-rate reduction via
// DaisySP Decimator (MIT).
//
// Decimator applies sample-and-hold downsampling first, then reduces bit
// depth via integer bit-shifting. Both steps are combined in one Process()
// call. Two Decimator instances are needed (decL / decR) because the
// sample-hold counter is per-channel state.
//
// param crush:     0–15 (0 = bypass, 1 = 15-bit, 15 = 1-bit)
//   Maps to SetBitsToCrush(crush). 0 drops no bits → pass-through.
//
// param downsample: 0–15 (0 = bypass → full sample rate)
//   Maps to SetDownsampleFactor(downsample / 15.0f).
//   For sampler voices always pass 0 — pre-interpolation address
//   quantization stays inline in the sampler mix loop (different effect).
//   For SF voices pass instrParams.downsample so the chain handles it.
//
// Call setParams() once per block (or at trigger) when params change.
// ===========================================================================
struct BitcrushModule {
    int crush      = 0;
    int downsample = 0;
    daisysp::Decimator decL;
    daisysp::Decimator decR;

    void reset() {
        crush = 0;
        downsample = 0;
        decL.Init();
        decR.Init();
        // Decimator::Init() defaults downsample_factor=1 (active) — force bypass.
        decL.SetDownsampleFactor(0.0f);
        decR.SetDownsampleFactor(0.0f);
        decL.SetBitsToCrush(0);
        decR.SetBitsToCrush(0);
    }

    bool enabled() const { return crush > 0 || downsample > 0; }

    // Call once per block (or at trigger) when params change.
    void setParams(int crushParam, int downsampleParam) {
        crush      = crushParam;
        downsample = downsampleParam;
        decL.SetBitsToCrush(static_cast<uint8_t>(crush));
        decR.SetBitsToCrush(static_cast<uint8_t>(crush));
        decL.SetDownsampleFactor(downsample / 15.0f);
        decR.SetDownsampleFactor(downsample / 15.0f);
    }

    inline float processMono(float in) {
        if (!enabled()) return in;
        return decL.Process(in);
    }

    inline void processStereo(float& L, float& R) {
        if (!enabled()) return;
        L = decL.Process(L);
        R = decR.Process(R);
    }
};
