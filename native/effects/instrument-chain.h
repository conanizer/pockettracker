#pragma once
#include "modules/filter-module.h"
#include "modules/drive-module.h"
#include "modules/crush-module.h"
#include "modules/eq-module.h"

// The per-voice effect chain, per sample: Crush → Drive → Filter → EQ.
//
// ⚠️ A sampler voice passes downsample 0 to the crush: its downsample is a quantized read address in
// the mix loop, a different sound. A SoundFont voice hands the chain both.
// A new module is a member, a line in reset() and a line in each process*() — and in idle() / touch().
struct InstrumentChain {
    BitcrushModule crush;
    DriveModule    drive;
    FilterModule   filter;
    EqModule       eq;    // 3-band parametric EQ (loShelf / bell / hiShelf)

    // False until the note's first sample has gone through: a write before it lands at once, one
    // after it glides. See the modules' `fresh`.
    bool started = false;

    // ⚠️ `keepToneState` leaves the crush, filter and EQ memory alone: a SoundFont chain belongs to
    // the TRACK, and the stolen note is still flowing through it — zeroing it steps the output.
    void reset(float sampleRate = 44100.0f, bool keepToneState = false) {
        started = false;
        drive.reset();
        if (keepToneState) {
            // Off, as reset() would leave them, until the caller sets them again.
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

    // Nothing is on and nothing is fading off: a sample through the chain is `touch()` and no more.
    // Holds until the next parameter write, so a caller may ask once per piece.
    bool idle() const {
        return !crush.enabled() && crush.fadeLeft == 0 && !drive.enabled() && drive.fadeLeft == 0 &&
               !filter.enabled() && filter.typeFadeLeft == 0 && !eq.active;
    }

    // What a sample through an idle chain leaves behind: the note has started.
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
