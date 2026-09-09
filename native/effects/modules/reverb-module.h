#pragma once
#include "../primitives/daisysp/reverbsc.h"
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
// ReverbModule — Schroeder-Moorer stereo reverb send (DaisySP ReverbSc).
//
// Takes a mono send-bus sum, expands to stereo wet output.
// inputEq is a pre-reverb EQ band (applied before the reverb algorithm).
// ===========================================================================
struct ReverbModule {
    daisysp::ReverbSc reverb;
    EqModule          inputEq;
    float             sampleRate = 44100.0f;   // the rate the delay lines were actually built at

    // ⚠️ ReverbSc carves all eight delay lines out of ONE fixed array, sized for exactly this rate,
    // and refuses any rate whose lines will not fit. Its refusal leaves three buffer pointers
    // indeterminate, so the return value is not optional: `Process` would dereference them.
    //
    // A device above the ceiling gets a reverb built at the ceiling instead: shorter and brighter
    // than intended, which is the same compromise the whole engine ran on before the buses learned
    // the device rate at all, and much better than no reverb. `sampleRate` records what was really
    // used, so nobody reads it as the device rate.
    static constexpr float MAX_SUPPORTED_RATE = DSY_REVERBSC_MAX_RATE;

    // ⚠️⚠️ **THREE INDEPENDENT CELLS, EACH NEUTRAL AT ITS DEFAULT, AND THE DEFAULTS ARE THE REVERB
    // THAT SHIPPED.** They are not a mode between them: any combination is legal, because the TYPE
    // cell on screen only writes them and never comes back to ask. Each is gated so that at its
    // default the arithmetic below is the arithmetic that was there before the cell existed — not
    // merely close to it — which is what lets every project written until now play unchanged.
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
    int modHex          = 0x40;     // 0x40 = the wander ReverbSc has always had; 00 = none

    // The pre-delay ring, and one write head for both channels. Cleared with the reverb, because a
    // line holding the last project's audio would push it into the tail of the first block after a
    // load — inaudible on the dry path and several seconds long on this one.
    float preBufL[REVERB_PREDELAY_MAX_SAMPLES] = {};
    float preBufR[REVERB_PREDELAY_MAX_SAMPLES] = {};
    int   preWrite = 0;

    void reset(float sr) {
        sampleRate = sr;
        if (reverb.Init(sr) != 0) {
            sampleRate = MAX_SUPPORTED_RATE;
            reverb.Init(MAX_SUPPORTED_RATE);
        }
        reverb.SetFeedback(0x60 / 255.0f);
        reverb.SetLpFreq(200.0f * powf(100.0f, 0x80 / 255.0f));
        // ⚠️ Both re-derived here rather than left to the next push, the way the echo's tone
        // coefficient is. `Init` puts the wander back to 1 whatever the cell said, and the pre-delay
        // is a count of FRAMES for a rate that has just changed — a caller that forgot would leave a
        // 44.1 kHz pre-delay running on a device at 48, and drop a MOD the user had set.
        reverb.SetPitchMod(reverb_mod_scale(modHex));
        updatePreDelaySamples();
        inputEq.reset(sr);
        clearPreDelay();
    }

    // feedbackHex 00-FF → 0.0–1.0; dampHex 00-FF → LP 200Hz–20kHz
    void setParams(int feedbackHex, int dampHex) {
        reverb.SetFeedback(feedbackHex / 255.0f);
        reverb.SetLpFreq(200.0f * powf(100.0f, dampHex / 255.0f));
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
    }

    // Process stereo send bus into stereo wet output. Always 100% wet. Writes to outL/outR.
    // inputEq applied stereo (independent L/R biquads) before the reverb algorithm.
    //
    // ⚠️⚠️ **AT THE DEFAULT CELLS THIS LOOP MUST COLLAPSE TO EXACTLY `Process(l, r, &wl, &wr)`** —
    // the reverb that was here before it had a character, with no term added on either side of it.
    // The two gates below are what keeps that true; MOD needs none, because 0x40 maps to the 1.0
    // `Init` sets and the call is the identity.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numFrames) {
        constexpr int ring     = static_cast<int>(REVERB_PREDELAY_MAX_SAMPLES);
        const bool    delayed  = preDelaySamples > 0;
        const bool    widening = widthHex != 0x80;
        const float   side     = widthHex / 128.0f;

        // Both heads walked by hand rather than by `%` per frame: the ring is not a power of two, so
        // the modulo would be an integer division on every sample of every block.
        int preRead = preWrite - preDelaySamples;
        if (preRead < 0) preRead += ring;

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
            float wl, wr;
            reverb.Process(l, r, &wl, &wr);
            if (widening) {
                // ⚠️ The mid stays at unity whatever WIDE says, so a reverb turned to mono is not
                // also turned down: only the difference between the channels is scaled.
                const float mid = (wl + wr) * 0.5f;
                const float sd  = (wl - wr) * 0.5f * side;
                wl = mid + sd;
                wr = mid - sd;
            }
            outL[i] = wl;
            outR[i] = wr;
        }
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
