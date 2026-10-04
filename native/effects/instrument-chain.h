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
// Signal order: Crush → Drive → Filter → EQ
// Within Crush: Decimator applies Downsample → Bitcrush in one pass.
//
// Sampler voices: crush.setParams(effCrush, 0) — downsample=0 bypasses
//   the chain downsample; pre-interpolation address quantization stays
//   inline in the sampler mix loop (different lo-fi character, cannot move).
// SF voices: crush.setParams(effCrush, effDownsample) — the chain handles both.
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

    // False until the note's first sample has gone through — what tells a set-up write (instant) from a
    // change to a sounding note (glided). See the modules' `fresh`.
    bool started = false;

    // sampleRate is for EqModule's init.
    // ⚠️ `keepToneState` leaves the CRUSH, FILTER and EQ memory alone (the caller re-sets the
    // parameters either way): a SoundFont chain belongs to the TRACK, and the stolen note is still
    // flowing through it for the rest of the block — zeroing a held sample or a resonant SVF steps
    // the output to zero. A sampler voice comes out of the pool silent, so it clears.
    void reset(float sampleRate = 44100.0f, bool keepToneState = false) {
        started = false;
        drive.reset();
        if (keepToneState) {
            // Filter and EQ are re-armed below by the caller — only the memory of the signal still passing
            // through survives. Held at the same defaults reset() would have left them at, so a
            // caller that then declines to set a filter type or an EQ band gets silence from them.
            filter.type = 0;
            filter.fresh = true;   // the new note's own filter lands at once, not as a glide
            crush.fresh = true;
            eq.active   = false;
        } else {
            crush.reset();
            filter.reset();
            eq.reset(sampleRate);
        }
    }

    // Signal order: Crush → Drive → Filter → EQ
    inline float processMono(float in) {
        started = true;
        in = crush.processMono(in);
        in = drive.processMono(in);
        in = filter.processMono(in);
        return eq.processMono(in);
    }

    inline void processStereo(float& L, float& R) {
        started = true;
        crush.processStereo(L, R);
        drive.processStereo(L, R);
        filter.processStereo(L, R);
        eq.processStereo(L, R);
    }
};
