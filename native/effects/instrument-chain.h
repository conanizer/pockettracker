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

    // False until sound has passed through the chain: a write before that lands at once, after it glides.
    bool started = false;

    // `sounding`: the track still carries the last note — a SoundFont voice is the track's. A level the
    // new note sets (send, pan, VOL) then glides over that tail. The modules start clean either way: the
    // tail finishes through its own copy of them (SoundfontVoice::cutChain).
    void reset(float sampleRate = 44100.0f, bool sounding = false) {
        started = sounding;
        crush.reset();
        drive.reset();
        filter.reset();
        eq.reset(sampleRate);
    }

    // Nothing on or fading: a sample through is just `touch()`. Holds until the next write.
    bool idle() const {
        return !crush.enabled() && !crush.oldShare.moving() && !drive.enabled() && !drive.oldShare.moving() &&
               !filter.enabled() && !filter.typeFade.moving() && !eq.active;
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
