#pragma once
#include "modules/filter-module.h"
#include "modules/drive-module.h"
#include "modules/crush-module.h"

// ===========================================================================
// InstrumentChain — per-voice inline effect chain.
// Applied to voice output before it reaches the track bus.
// Sampler voices: processMono() called per sample in the mix loop.
// SF voices: processStereo() called per sample after tsf_render_float_channel().
//
// Signal order: Crush → Drive → Filter
// Within Crush: Decimator applies Downsample → Bitcrush in one pass.
//
// Sampler voices: crush.setParams(effCrush, 0) — downsample=0 bypasses
//   the chain downsample; pre-interpolation address quantization stays
//   inline in the sampler mix loop (different lo-fi character, cannot move).
// SF voices: crush.setParams(instrParams.crush, instrParams.downsample) —
//   chain handles both; the old sfBuf index-based downsample is removed.
//
// Adding a new module:
//   1. Add a member of the module type.
//   2. Call module.reset() in reset().
//   3. Call module.process*() in processMono() / processStereo() at the right position.
//   Call sites in audio-engine.cpp do not change.
// ===========================================================================
struct InstrumentChain {
    BitcrushModule crush;
    DriveModule    drive;
    FilterModule   filter;

    void reset() {
        crush.reset();
        drive.reset();
        filter.reset();
    }

    inline float processMono(float in) {
        in = crush.processMono(in);
        in = drive.processMono(in);
        return filter.processMono(in);
    }

    inline void processStereo(float& L, float& R) {
        crush.processStereo(L, R);
        drive.processStereo(L, R);
        filter.processStereo(L, R);
    }
};
