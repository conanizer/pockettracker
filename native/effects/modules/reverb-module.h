#pragma once
#include "../primitives/daisysp/reverbsc.h"
#include "../primitives/mverb.h"
#include "eq-module.h"
#include "reverb-presets.h"
#include <cmath>

// The pre-delay line, per channel: 0.15 s at 48 kHz.
//
// ⚠️ **IT IS A SAMPLE COUNT AND PRE IS A TIME, so the ceiling moves with the device rate** — the same
// shape as the echo's DELAY_MAX_SAMPLES, and it CLAMPS SILENTLY at the top. At 44.1 kHz the cell's
// full range fits with room to spare; at 96 kHz the top of the cell is 75 ms rather than 150 and the
// cell still reads FF. The two lines are 58 KB together, which is why the ceiling is a twelfth of the
// echo's rather than matched to it: a reverb pre-delay past about 150 ms stops reading as one space
// and starts reading as a slap, which the echo already does better.
static constexpr size_t REVERB_PREDELAY_MAX_SAMPLES = 7200;
static constexpr float  kReverbPreDelayMaxSeconds   = 0.15f;

// ===========================================================================
// ReverbModule — the stereo reverb send, and the choice of WHICH reverb.
//
// Takes a mono send-bus sum, expands to stereo wet output.
// inputEq is a pre-reverb EQ band (applied before the reverb algorithm).
//
// ⚠️⚠️ **TWO ALGORITHMS ARE RESIDENT AT ONCE AND ONLY ONE SOUNDS.** Between them they are ~390 KB, in
// an `AudioEngine` that is heap-allocated precisely because of sizes like this. The alternative —
// one algorithm on the heap, swapped when the cell changes — was rejected: a swap cannot allocate or
// free on the audio thread, so it would need a handoff and a deferred free to save a quarter of a
// megabyte on platforms that have hundreds of them. ⚠️ **THE COST IS PAID PER ALGORITHM ADDED**, so a
// third one is the point at which this decision is worth re-opening rather than repeating.
//
// ⚠️ **THE SILENT ONE IS NOT RUNNING, so its lines still hold whatever they held when the cell last
// moved.** Both are cleared together on `reset`, which is what stops a switch mid-project from
// dropping a tail recorded minutes ago into the middle of a take.
//
// ⚠️ The pre-delay ring and the mid/side pair below are OUTSIDE both algorithms and are shared, so
// PRE and WIDE mean one thing whichever is sounding. Everything else is read per algorithm — see
// reverb-presets.h, where every mapping is a named function for this reason.
// ===========================================================================
struct ReverbModule {
    daisysp::ReverbSc   reverb;                // algorithm 0 — the wash that shipped in 0.9.8
    mverb::MVerb<float> tank;                  // algorithm 1 — the Dattorro tank with early reflections
    EqModule            inputEq;
    float               sampleRate = 44100.0f; // the rate the delay lines were actually built at

    // ⚠️ ReverbSc carves all eight delay lines out of ONE fixed array, sized for exactly this rate,
    // and refuses any rate whose lines will not fit. Its refusal leaves three buffer pointers
    // indeterminate, so the return value is not optional: `Process` would dereference them.
    //
    // A device above the ceiling gets a reverb built at the ceiling instead: shorter and brighter
    // than intended, which is the same compromise the whole engine ran on before the buses learned
    // the device rate at all, and much better than no reverb. `sampleRate` records what was really
    // used, so nobody reads it as the device rate.
    static constexpr float MAX_SUPPORTED_RATE = DSY_REVERBSC_MAX_RATE;

    // ⚠️⚠️ **THREE INDEPENDENT CELLS, AND PRE AND WIDE ARE NEUTRAL AT THEIR DEFAULTS.** They are not
    // a mode between them: any combination is legal, because the TYPE cell on screen only writes them
    // and never comes back to ask. Those two are gated so that at their defaults the arithmetic below
    // is skipped rather than performed as an identity — that is what keeps an old project's
    // pre-delay and stereo image exactly as they were.
    //
    // ⚠️ **MOD IS NOT ONE OF THEM.** Its default is a real setting the algorithm is driven to, not a
    // no-op: it is set on every `reset` and every push, and lowering it below what `Init` leaves
    // behind is the whole point of the value chosen. There is nothing to gate — the call is made
    // unconditionally and there is no cheaper path to fall back to.
    //
    // ⚠️ WIDE's neutral is 0x80 and not 00, because a width cell has to reach BOTH sides of "as it
    // is". The mid/side pair that reconstructs L and R exactly is not bit-identical to leaving them
    // alone, so 0x80 skips the arithmetic rather than performing the identity.
    // ⚠️ PRE is kept as the CELL as well as in frames, the way the echo keeps `toneHex`, because the
    // frame count depends on the RATE: `reset` is where the rate changes, and a module that only held
    // the frames would keep a 44.1 kHz pre-delay on a device running at 48.
    int preHex          = 0x00;     // 00 = the send goes straight in, as it always did
    int preDelaySamples = 0;        // derived from preHex and the rate — never written directly
    int widthHex        = 0x80;     // 0x80 = untouched; 00 = mono, FF = twice the sides
    int modHex          = 0x10;     // 00 = none; 0x40 is the wander the algorithm is built around

    // ⚠️ **0 IS THE REVERB THAT SHIPPED AND ITS NUMBER IS ITS IDENTITY** — append, never insert. A
    // project written before the cell existed loads without it and lands here.
    int algo = 0;

    // ⚠️⚠️ **THE LAST SIZE CELL THE TANK WAS BUILT AT, AND IT EXISTS TO STOP A `memset`.** MVerb's SIZE
    // *is* the lengths of its eight tank lines, so setting it re-lengths and clears them — ~180 KB of
    // `memset` and a tail cut dead. The globals are re-pushed on EVERY project edit, so a caller that
    // pushed SIZE unconditionally would silence the reverb every time the user typed a note. -1 is
    // "never built", so the first push always lands.
    int tankSizeHex = -1;

    // What `setParams` last saw, kept so that a switch back to algorithm 0 can re-apply the cells
    // `ReverbSc::Init` throws away. ⚠️ They are the CELLS and not the derived numbers, for the reason
    // PRE is: the derivation is the mapping's job and it may change.
    int sizeHex     = 0x60;
    int dampHexCell = 0x80;

    // The pre-delay ring, and one write head for both channels. Cleared with the reverb, because a
    // line holding the last project's audio would push it into the tail of the first block after a
    // load — inaudible on the dry path and several seconds long on this one.
    float preBufL[REVERB_PREDELAY_MAX_SAMPLES] = {};
    float preBufR[REVERB_PREDELAY_MAX_SAMPLES] = {};
    int   preWrite = 0;

    // ⚠️ **RAMPED ACROSS THE BLOCK, because SIZE is edited while the reverb is sounding** and this
    // gain moves nearly 20 dB across the cell — a step that size lands on a tail that is still
    // ringing, where nothing downstream would hide it. The pre-delay above deliberately does NOT
    // ramp; it is a voicing control set once, and this one is turned in front of the speakers.
    float wetGain       = 1.0f;   // where the last block left it
    float wetGainTarget = 1.0f;   // what SIZE last asked for

    void reset(float sr) {
        sampleRate = sr;
        if (reverb.Init(sr) != 0) {
            sampleRate = MAX_SUPPORTED_RATE;
            reverb.Init(MAX_SUPPORTED_RATE);
        }
        // ⚠️ **THE SECOND ALGORITHM SHARES THE FIRST'S CEILING, AND IT IS NOT OPTIONAL.** MVerb's read
        // heads are set from `seconds × rate` while its buffers are sized for the design rate, so a
        // rate above it would place a head past the end of a line. `sampleRate` is already the clamped
        // rate by the time we get here, and the two ceilings are deliberately the same number.
        static_assert(static_cast<int>(MAX_SUPPORTED_RATE) <= mverb::kMVerbDesignRate,
                      "MVerb's lines are sized for kMVerbDesignRate; the reverb must never be built above it");
        tank.setSampleRate(sampleRate);
        tankSizeHex = -1;    // the lines were just rebuilt, so the next push must re-length them
        // The default cells, until the project pushes its own — `reverb_size_gain` is 1 at 0x60 by
        // construction, so the ramp starts where a default project would already have it.
        setParams(0x60, 0x80);
        wetGain = wetGainTarget;
        // ⚠️ Both re-derived here rather than left to the next push, the way the echo's tone
        // coefficient is. `Init` puts the wander back to 1 whatever the cell said, and the pre-delay
        // is a count of FRAMES for a rate that has just changed — a caller that forgot would leave a
        // 44.1 kHz pre-delay running on a device at 48, and drop a MOD the user had set.
        reverb.SetPitchMod(reverb_mod_scale(modHex));
        // ⚠️ The second algorithm's cells re-derived here for the same reason, and its one remaining
        // FIXED value with them: `setSampleRate` above rebuilt the tank from MVerb's own constructor
        // defaults, so a caller that reset and then never pushed would run at the vendored input
        // bandwidth rather than the open one this reverb wants.
        tank.setParameter(mverb::MVerb<float>::EARLYMIX, mverb_early_param(modHex));
        tank.setParameter(mverb::MVerb<float>::BANDWIDTHFREQ, kMverbBandwidth);
        updatePreDelaySamples();
        inputEq.reset(sr);
        clearPreDelay();
    }

    // SIZE and DAMP, pushed to BOTH algorithms — the cells are stored once and read twice, so the
    // silent one stays in step and a switch does not need the project pushed again.
    //
    // ⚠️ Every mapping lives in reverb-presets.h and the reasoning is there: to algorithm 0 a cell is
    // a decay TIME and a corner in Hz, and its wet gain is what stops the time from also being a
    // volume; to algorithm 1 the same cell is a room's dimensions, and there is no gain to derive
    // because that algorithm's level barely moves with it.
    void setParams(int feedbackHex, int dampHex, int decayHex = 0x60, int densityHex = 0x99) {
        sizeHex       = feedbackHex;
        dampHexCell   = dampHex;
        reverb.SetFeedback(reverb_size_feedback(feedbackHex));
        reverb.SetLpFreq(reverb_damp_freq(dampHex));
        wetGainTarget = reverb_size_gain(feedbackHex);

        tank.setParameter(mverb::MVerb<float>::DAMPINGFREQ, mverb_damp_param(dampHex));
        tank.setParameter(mverb::MVerb<float>::DECAY, mverb_decay_param(decayHex));
        tank.setParameter(mverb::MVerb<float>::DENSITY, mverb_density_param(densityHex));
        // ⚠️⚠️ **GATED, AND THE GATE IS THE WHOLE REASON `tankSizeHex` EXISTS** — see its comment. SIZE
        // re-lengths and clears eight delay lines, so pushing it unchanged would cut the tail dead on
        // every project edit. DECAY and DAMP above are smoothed inside the algorithm and are free.
        if (feedbackHex != tankSizeHex) {
            tankSizeHex = feedbackHex;
            tank.setParameter(mverb::MVerb<float>::SIZE, mverb_size_param(feedbackHex));
        }
    }

    /**
     * Which algorithm sounds. ⚠️ The voicing cells are NOT rewritten — see reverb-presets.h.
     *
     * ⚠️⚠️ **THE ONE BEING SWITCHED TO IS CLEARED, BECAUSE THE SILENT ONE IS FROZEN AND NOT DECAYING.**
     * Its delay lines still hold whatever they held when the cell last moved, so without this a switch
     * would drop a tail from minutes ago into the middle of a take — the same trap the pre-delay ring
     * is cleared for, and worse here because the tail rings for seconds.
     *
     * ⚠️⚠️ **AND THE `algo == next` GUARD IS LOAD-BEARING, NOT AN OPTIMISATION.** The globals are
     * re-pushed on every project edit, so without it typing a note would re-`Init` the reverb and cut
     * its tail dead. It is also what keeps every existing project bit-identical: a project on
     * algorithm 0 never reaches the clear at all.
     */
    void setAlgo(int algoIn) {
        const int next = (algoIn >= 0 && algoIn < kReverbAlgoCount) ? algoIn : 0;
        if (next == algo) return;
        algo = next;
        if (algo == 1) {
            // Rebuilds its lines from the cells it is already holding, so nothing needs re-pushing.
            tank.reset();
        } else {
            // ⚠️ `Init` clears the lines AND puts feedback, corner and wander back to ITS defaults, so
            // the three cells are re-applied here rather than left to the caller's next push. The
            // order `push_global_effects` happens to use is not something this module may rely on.
            reverb.Init(sampleRate);
            reverb.SetFeedback(reverb_size_feedback(sizeHex));
            reverb.SetLpFreq(reverb_damp_freq(dampHexCell));
            reverb.SetPitchMod(reverb_mod_scale(modHex));
        }
    }

    // The three character cells, together, because they arrive together from the project.
    //
    // ⚠️ **PRE IS NOT RAMPED, so moving it while the send is loud puts a step into the reverb's
    // INPUT** — the same trade the echo's TIME cell makes, and it lands in the same place: the wet
    // return only, softened by everything downstream of it. It is a voicing control, set once and
    // left, and smoothing it would cost a second read head for a click nobody has complained about.
    void setCharacter(int preHexIn, int widthHexIn, int modHexIn) {
        const bool wasOff = (preDelaySamples == 0);
        preHex            = preHexIn;
        updatePreDelaySamples();
        // ⚠️ **THE RING IS ONLY WRITTEN WHILE PRE IS UP, so switching it on again would otherwise
        // replay whatever was in it when it went off** — a burst of audio from minutes ago, straight
        // into a tail that rings for seconds. Cleared on that one transition and no other, because
        // clearing it on every push would punch a hole in a pre-delay that was already running: the
        // global effects are pushed again on any project edit.
        if (wasOff && preDelaySamples > 0) clearPreDelay();
        widthHex = widthHexIn;
        modHex   = modHexIn;
        reverb.SetPitchMod(reverb_mod_scale(modHex));
        // The same cell, read the other way: a pitch wander above, the early reflections' share here.
        tank.setParameter(mverb::MVerb<float>::EARLYMIX, mverb_early_param(modHex));
        tank.setParameter(mverb::MVerb<float>::BANDWIDTHFREQ, kMverbBandwidth);
    }

    // Process stereo send bus into stereo wet output. Always 100% wet. Writes to outL/outR.
    // inputEq applied stereo (independent L/R biquads) before the reverb algorithm.
    //
    // ⚠️ **AT THE DEFAULT PRE AND WIDE CELLS THE TWO GATES BELOW ADD NOTHING TO EITHER SIDE OF
    // `Process`** — that is what keeps an old project's pre-delay and stereo image untouched. MOD is
    // not here at all; it lives inside the algorithm. The wet gain is NOT one of the gates either:
    // it is derived from SIZE at every setting including the default, because the level it divides
    // out is there at every setting too.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numFrames) {
        constexpr int ring     = static_cast<int>(REVERB_PREDELAY_MAX_SAMPLES);
        const bool    delayed  = preDelaySamples > 0;
        const bool    widening = widthHex != 0x80;
        const float   side     = widthHex / 128.0f;

        // ⚠️ **ALGORITHM 1 HAS NO LEVEL TO DIVIDE OUT, SO ITS TARGET IS A CONSTANT.** `wetGainTarget`
        // is algorithm 0's SIZE compensation and means nothing here; what algorithm 1 needs instead is
        // the fixed trim that puts the two at the same loudness. Both ride the SAME ramp, so switching
        // the cell while the reverb sounds slides between them rather than stepping.
        const float target   = (algo == 1) ? kMverbOutputTrim : wetGainTarget;
        const float gainStep = numFrames > 0 ? (target - wetGain) / numFrames : 0.0f;
        float       gain     = wetGain;

        // Both heads walked by hand rather than by `%` per frame: the ring is not a power of two, so
        // the modulo would be an integer division on every sample of every block.
        int preRead = preWrite - preDelaySamples;
        if (preRead < 0) preRead += ring;

        // The input stage — EQ and the pre-delay ring, both OUTSIDE the algorithms and shared by
        // them. It lands in the output buffers, which every path below then reads back in place.
        for (int i = 0; i < numFrames; i++) {
            float l = inL[i], r = inR[i];
            if (inputEq.active) {
                inputEq.processStereo(l, r);
            }
            if (delayed) {
                preBufL[preWrite] = l;
                preBufR[preWrite] = r;
                l = preBufL[preRead];
                r = preBufR[preRead];
                if (++preWrite >= ring) preWrite = 0;
                if (++preRead >= ring) preRead = 0;
            }
            outL[i] = l;
            outR[i] = r;
        }

        // ⚠️ Both algorithms read index i and write index i in the same step, so running them over the
        // output buffers in place is safe and saves a scratch pair the size of a sub-block.
        if (algo == 1) {
            tank.process(outL, outR, outL, outR, numFrames);
        } else {
            for (int i = 0; i < numFrames; i++) {
                float wl, wr;
                reverb.Process(outL[i], outR[i], &wl, &wr);
                outL[i] = wl;
                outR[i] = wr;
            }
        }

        for (int i = 0; i < numFrames; i++) {
            float wl = outL[i], wr = outR[i];
            if (widening) {
                // ⚠️ The mid stays at unity whatever WIDE says, so a reverb turned to mono is not
                // also turned down: only the difference between the channels is scaled.
                const float mid = (wl + wr) * 0.5f;
                const float sd  = (wl - wr) * 0.5f * side;
                wl = mid + sd;
                wr = mid - sd;
            }
            gain += gainStep;
            outL[i] = wl * gain;
            outR[i] = wr * gain;
        }
        wetGain = target;
    }

  private:
    // The one writer of `preDelaySamples`, because it has two inputs and both move: the cell and the
    // device rate. ⚠️ It CLAMPS SILENTLY — see the ceiling's own comment at the top of this file.
    void updatePreDelaySamples() {
        const float wanted = (preHex / 255.0f) * kReverbPreDelayMaxSeconds * sampleRate;
        preDelaySamples    = static_cast<int>(
            fminf(wanted, static_cast<float>(REVERB_PREDELAY_MAX_SAMPLES - 1)));
    }

    void clearPreDelay() {
        for (size_t i = 0; i < REVERB_PREDELAY_MAX_SAMPLES; i++) preBufL[i] = preBufR[i] = 0.0f;
        preWrite = 0;
    }
};
