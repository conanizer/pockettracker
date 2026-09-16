#pragma once
#include "modules/filter-module.h"
#include "modules/drive-module.h"
#include "modules/crush-module.h"
#include "modules/eq-module.h"

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
    EqModule       eq;    // 3-band parametric EQ (loShelf / bell / hiShelf)

    // sampleRate required for EqModule init; other modules don't need it here.
    //
    // ⚠️ `keepToneState` leaves the FILTER's and the EQ's integrators alone — everything else is
    // cleared exactly as before, and the caller's setParams calls restore the parameters either way.
    // A SoundFont chain belongs to the TRACK, not to the note: the note being stolen is still
    // flowing through it when the next note is set up, so zeroing a resonant SVF steps its output to
    // zero in one sample. The steal's declick ramp rides the filter's INPUT and cannot cover that.
    // A sampler voice comes out of the pool silent, so it clears.
    void reset(float sampleRate = 44100.0f, bool keepToneState = false) {
        crush.reset();
        drive.reset();
        if (keepToneState) {
            // Both are re-armed below by the caller — only the memory of the signal still passing
            // through survives. Held at the same defaults reset() would have left them at, so a
            // caller that then declines to set a filter type or an EQ band gets silence from them.
            filter.type = 0;
            eq.active   = false;
        } else {
            filter.reset();
            eq.reset(sampleRate);
        }
    }

    // Signal order: Crush → Drive → Filter → EQ
    inline float processMono(float in) {
        in = crush.processMono(in);
        in = drive.processMono(in);
        in = filter.processMono(in);
        return eq.processMono(in);
    }

    inline void processStereo(float& L, float& R) {
        crush.processStereo(L, R);
        drive.processStereo(L, R);
        filter.processStereo(L, R);
        eq.processStereo(L, R);
    }
};
