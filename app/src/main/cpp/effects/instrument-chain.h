#pragma once
#include "filter-module.h"

// ===========================================================================
// InstrumentChain — per-voice inline effect chain.
// Holds one instance of each DSP module applied to the voice signal.
// Sampler voices call processMono() in the mix loop (per sample, mono).
// SF voices call processStereo() on each sample after tsf_render_float_channel().
//
// Current module slots (signal order):
//   1. FilterModule  — resonant biquad filter (biquad now; SVF is next)
//   (Drive and crush are still inline in processAudioBlock — move here when wrapped.)
//
// Adding a new module:
//   Add a member of the module type.
//   Call module.reset() inside reset().
//   Call module.process*() inside processMono() / processStereo().
//   Call sites in audio-engine.cpp do not change.
// ===========================================================================
struct InstrumentChain {
    FilterModule filter;

    void reset() {
        filter.reset();
    }

    inline float processMono(float in) {
        return filter.processMono(in);
    }

    inline void processStereo(float& L, float& R) {
        filter.processStereo(L, R);
    }
};
