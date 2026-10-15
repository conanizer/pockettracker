#pragma once
#include "modules/filter-module.h"
#include "modules/drive-module.h"
#include "modules/crush-module.h"
#include "modules/eq-module.h"

// The per-voice effect chain, per sample: Crush → Drive → Filter → EQ.
//
// ⚠️ A sampler voice passes downsample 0: it downsamples its read address in the mix loop instead.
// A new module also needs a line in reset(), each process*(), idle() and touch().
struct InstrumentChain {
    BitcrushModule crush;
    DriveModule    drive;
    FilterModule   filter;
    EqModule       eq;

    // False until the note's first sample: a write before it lands at once, after it glides.
    bool started = false;

    // ⚠️ `keepToneState` keeps the crush, filter and EQ memory: a SoundFont chain belongs to the track,
    // and zeroing it under the stolen note steps the output.
    void reset(float sampleRate = 44100.0f, bool keepToneState = false) {
        started = false;
        drive.reset();
        if (keepToneState) {
            // Off until the caller sets them; the new note's settings land at once.
            filter.type = 0;
            filter.fresh = true;
            crush.fresh = true;
            eq.active   = false;
        } else {
            crush.reset();
            filter.reset();
            eq.reset(sampleRate);
        }
    }

    // Nothing on or fading: a sample through is just `touch()`. Holds until the next write.
    bool idle() const {
        return !crush.enabled() && crush.fadeLeft == 0 && !drive.enabled() && drive.fadeLeft == 0 &&
               !filter.enabled() && filter.typeFadeLeft == 0 && !eq.active;
    }

    // What a sample through an idle chain leaves behind.
    void touch() {
        started = true;
        crush.fresh = drive.fresh = filter.fresh = false;
    }

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
