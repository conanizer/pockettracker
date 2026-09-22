#pragma once
#include "../primitives/daisysp/delayline.h"
#include "delay-presets.h"
#include "eq-module.h"
#include <cmath>
#include <cstring>

// Max delay line: 88200 samples per channel — 2 seconds at 44100 Hz, 1.84 at 48000, 0.92 at 96000.
//
// ⚠️ **IT IS A SAMPLE COUNT AND EVERY TIME BELOW IS IN SECONDS OR BEATS, so the ceiling is a TIME
// that moves with the device rate — and both setters CLAMP SILENTLY when they cross it.** The
// consequences, worth knowing before changing either:
//   * free mode's documented "00-FF -> 0-2 seconds" is 0-2 s only at 44.1 kHz;
//   * sync mode's longest subdivisions are unreachable at slow tempos even at 44.1 kHz — 1/1 is four
//     beats, i.e. exactly 2 s at 120 BPM and longer below it, so at 100 BPM a `00` delay runs 17%
//     early while the cell still reads `00`. 1/2 breaks the same way below 60 BPM, dotted-1/4 below 45.
//
// Growing it is not free: two `DelayLine<float, DELAY_MAX_SAMPLES>` are 706 KB, which is 48% of
// `sizeof(AudioEngine)`, so doubling the ceiling doubles that. The alternative — offering only the
// subdivisions the current tempo can actually produce — makes an FX cell's range depend on the
// project tempo, which collides with "a member's number is its identity". Left as it is deliberately;
// the constraint belongs in the manual.
static constexpr size_t DELAY_MAX_SAMPLES = 88200;

// Subdivision beat fractions (in quarter-note beats) for sync mode.
// Index: 00=1/1  01=1/2   02=1/4   03=1/8   04=1/16  05=1/32
//        06=1/4T 07=1/8T  08=1/16T 09=1/4.  10=1/8.  11=1/16.
static const float kDelaySyncBeats[] = {
    4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f,
    2.0f / 3.0f, 1.0f / 3.0f, 1.0f / 6.0f,
    1.5f, 0.75f, 0.375f
};
static constexpr int kDelaySyncCount = 12;

// The read head's drift whenever WOBL is up: two incommensurate sines, a slow wow under a faster
// flutter. ⚠️ **DELIBERATELY NOT NOISE-DRIVEN.** A render has to be reproducible — a noise source
// here would put the delay in the same bucket as a random FX or an RND LFO, and make every export of
// one project a different file.
static constexpr float kDelayWowHz     = 0.71f;
static constexpr float kDelayFlutterHz = 5.3f;

// How far the head drifts, as a fraction of the delay TIME, at WOBL FF. Tape speed error is a
// percentage, so a slapback wobbles proportionally less than a two-second echo — which is what a
// listener expects, and why this is not an absolute number of samples.
//
// ⚠️ **WHAT IS AUDIBLE IS THE PITCH DEVIATION, NOT THE EXCURSION, and the two differ by the LFO
// RATE**: a modulated read head shifts pitch by `depth × 2πf / rate`, so the fast flutter term does
// far more of the audible work than the slow wow term at the same depth. At 0.004 this whole control
// was worth about 15 cents at FF on a 1/8 delay — real, measurable, and reported as "cannot feel it".
static constexpr float kDelayWobbleFraction = 0.025f;

// ⚠️ **AND A CEILING ON IT, IN TIME**, because the fraction alone runs away: the pitch swing grows
// with the delay TIME, so the same WOBL FF that is a pleasant warble on a 1/8 echo is most of an
// octave on a two-second one. Capping the excursion holds the strongest setting at roughly a
// semitone whatever TIME is set to, while leaving short delays proportional — a slapback still
// wobbles less than a long echo, which is the part of the tape model worth keeping.
static constexpr float kDelayWobbleMaxSeconds = 0.007f;

static constexpr float kDelayTwoPi = 6.28318530717958647692f;

// ─── How the read head reaches a new TIME ────────────────────────────────────────────────────────
//
// It GLIDES there instead of jumping, and the pitch shift everyone knows from a tape echo is not an
// effect added on top — it is what a read head moving through the buffer at a rate other than 1
// unavoidably does. Output frame `n` is `buffer[n - D(n)]`, so the playback ratio is `1 - D'(n)`: a
// shortening delay reads forward faster and pitches UP, a lengthening one pitches DOWN.
//
// ⚠️ **A JUMP IS A DISCONTINUITY IN THE OUTPUT**, which is the click a turned TIME knob used to make,
// and a knob held down is a click per step — the "robotic" stutter, not a resampling artefact.
//
// One-pole toward the target, so a small step settles quickly and a big one starts fast and eases in.
// ⚠️ **AND A CAP ON THE RATE, which is what keeps the sound musical rather than a screech**: without
// it a half-second jump would start the head moving at four samples per sample — two octaves up, or
// backwards. Half a sample per sample is an octave down at worst, and a jump from the shortest echo
// to the longest takes about two seconds to arrive, which is roughly how long moving a real tape head
// that far takes.
static constexpr float kDelayGlideSeconds = 0.08f;
static constexpr float kDelayGlideMaxRate = 0.5f;
// Near enough to have arrived, in samples of delay — a twentieth of a sample is four microseconds.
static constexpr float kDelayGlideEpsilon = 0.05f;

// ===========================================================================
// DelayModule — stereo tap-delay send (DaisySP DelayLine).
//
// Takes a mono send-bus sum, writes identical delayed signal to L and R.
// inputEq is applied to the stereo send input (independent L/R biquads) before writing to the delay line.
// ===========================================================================
struct DelayModule {
    daisysp::DelayLine<float, DELAY_MAX_SAMPLES> delL;
    daisysp::DelayLine<float, DELAY_MAX_SAMPLES> delR;
    EqModule inputEq;
    float    feedback   = 0.375f;
    float    sampleRate = 44100.0f;

    // ⚠️⚠️ **THREE INDEPENDENT SWITCHES, EACH OFF AT ITS DEFAULT, AND THE DEFAULTS ARE THE DELAY THAT
    // SHIPPED.** They are not a mode between them: any combination is legal, because the TYPE cell on
    // screen only writes them and never comes back to ask. Each is gated so that at its default the
    // arithmetic in `process` is the arithmetic that was there before the cell existed — not merely
    // close to it. That is what lets every project written until now play unchanged.
    //
    // ⚠️ **THE REGENERATION GAIN IS FDBK ALONE, AND FDBK TOPS OUT AT EXACTLY 1.0** — the repeats can
    // hold but never grow, so this loop cannot run away whatever the cells below say. Anything that
    // lifts it past unity has to bring a limiting curve of its own with it.
    bool  pong      = false;   // a side's repeat feeds the OTHER line
    int   toneHex   = 0xFF;    // kept as the CELL, because the coefficient below depends on the rate
    float toneCoeff = 1.0f;    // the regeneration one-pole's coefficient; 1 lets everything through
    float wobble    = 0.0f;    // 0..1, how far the read head drifts

    // The base position the drifting tap moves around. `DelayLine::SetDelay` keeps its own copy and
    // offers no way to read it back, so this is the delay time's second home rather than its first —
    // which is why every write of one goes through `setDelaySamples`.
    float delaySamples = 22050.0f;

    // Where the read head actually IS, which is `delaySamples` except while a TIME change is being
    // glided to. ⚠️ **THE HEAD, NOT THE TARGET, IS WHAT THE WOBBLE SWINGS AROUND AND WHAT THE LINES
    // ARE READ AT** — the target is only where it is heading.
    float headSamples = 22050.0f;

    // ⚠️ **A LOAD AND A RESET PLACE THE HEAD, THEY DO NOT MOVE IT.** Armed by `reset`, spent by the
    // first TIME written after it, so opening a project whose echo is set differently from the last
    // one starts there instead of swooping down to it over two seconds.
    bool snapNextTime = true;

    // Per-channel regeneration state, cleared with the lines: a filter holding the last project's
    // audio, or a phase left mid-drift, would otherwise ring into the first block after a load.
    float toneStateL = 0.0f, toneStateR = 0.0f;
    float wowPhase = 0.0f, flutterPhase = 0.0f;

    void reset(float sr) {
        sampleRate = sr;
        delL.Init();
        delR.Init();
        inputEq.reset(sr);
        feedback = 0x60 / 255.0f;
        // Default: 1/4 note at 120 BPM = 500 ms (index 2)
        float defaultSamples = 1.0f * (60.0f / 120.0f) * sr;
        snapNextTime = true;
        setDelaySamples(defaultSamples);
        // ⚠️ RE-ARMED AFTER THE DEFAULT IS PLACED, because `resetEffectState` leaves the module at
        // FACTORY defaults and the caller re-pushes the project's own values immediately after
        // (engine_setup.h). That re-push is still part of the load, so it must land rather than glide.
        snapNextTime = true;
        toneStateL = toneStateR = 0.0f;
        wowPhase = flutterPhase = 0.0f;
        // ⚠️ Re-derived here, not left to the next push: the cutoff is a fraction of the RATE, and
        // `reset` is where the rate changes. A caller that forgot would leave the repeats filtered
        // for a 44.1 kHz device on a machine running at 48.
        updateToneCoeff();
    }

    // Free mode: timeHex 00-FF → 0–2 seconds
    void setParamsFree(int timeHex, int feedbackHex) {
        setTimeFree(timeHex);
        feedback = feedbackHex / 255.0f;
    }

    // Sync mode: subdivIdx 0–11 (see kDelaySyncBeats), BPM from project
    void setParamsSync(int subdivIdx, int feedbackHex, float bpm) {
        setTimeSync(subdivIdx, bpm);
        feedback = feedbackHex / 255.0f;
    }

    // The TIME alone. `TIM` writes this one and the two above leave it to it, so there is a single
    // place a delay time is turned into a head position whichever screen or cell asked for it.
    //
    // ⚠️ **TIM IS ALWAYS THE FREE SCALE, EVEN WHILE THE SCREEN READS 1/8T.** A ramp needs 256 values
    // in a row to slide through, and the twelve subdivisions are a list rather than a scale — an AUS
    // over them would jump between named divisions instead of sliding.
    void setTimeFree(int timeHex) {
        setDelaySamples((timeHex / 255.0f) * 2.0f * sampleRate);
    }

    void setTimeSync(int subdivIdx, float bpm) {
        if (subdivIdx < 0 || subdivIdx >= kDelaySyncCount) subdivIdx = 2;
        setDelaySamples(kDelaySyncBeats[subdivIdx] * (60.0f / bpm) * sampleRate);
    }

    // How far the read head swings, in frames, at the current TIME and WOBL. ⚠️ Public and stated
    // ONCE because it is the quantity a listener actually hears — the pitch swing is this times the
    // LFO rate — so anything measuring the wobble must read the same number `process` uses rather
    // than recomputing it from the constants and drifting away from the code.
    float wobbleDepthSamples() const {
        const float proportional = wobble * headSamples * kDelayWobbleFraction;
        const float ceiling      = wobble * kDelayWobbleMaxSeconds * sampleRate;
        return fminf(proportional, ceiling);
    }

    // The position the drifting head swings AROUND — the delay time, pulled down just far enough
    // that a full swing still fits inside the line.
    //
    // ⚠️ **AT THE LONGEST DELAY TIME THERE IS NO ROOM ABOVE TO DRIFT INTO**: TIME is already clamped
    // to the last sample of the buffer, so a head that only ever swung upward from there had nowhere
    // to go and the wobble silently vanished at exactly the setting where it is most audible. Moving
    // the CENTRE down costs a couple of milliseconds off a two-second echo — inaudible, and only
    // when WOBL is up — where flattening the swing against the clamp would have been heard.
    float wobbleCentreSamples() const {
        const float ceiling = static_cast<float>(DELAY_MAX_SAMPLES) - 3.0f - wobbleDepthSamples();
        return fminf(headSamples, fmaxf(2.0f, ceiling));
    }

    // The three character cells, together, because they arrive together from the project.
    //
    // TONE is the brightness of the repeats, and maps 200 Hz–20 kHz by the same curve the reverb's
    // DAMP cell uses — one curve in the tree, and the two cells that colour a send read the same way
    // round. ⚠️ **FF IS NOT THE TOP OF THAT CURVE, IT IS THE FILTER SWITCHED OUT**: a one-pole at
    // 20 kHz still takes a little off every pass, and a delay whose brightest setting is not the
    // delay that shipped would have no way back to it. FE is 19 kHz and inaudibly below FF — the
    // step is a discontinuity in the code and not in what anyone hears.
    void setCharacter(bool pongOn, int toneHexIn, int wobbleHex) {
        pong    = pongOn;
        toneHex = toneHexIn;
        wobble  = wobbleHex / 255.0f;
        updateToneCoeff();
    }

    // Process stereo send bus into stereo wet output. Always 100% wet. Writes to outL/outR.
    // Each channel has its own delay line — panned instruments echo on the correct side.
    //
    // ⚠️⚠️ **AT THE DEFAULT CELLS THIS LOOP MUST COLLAPSE TO EXACTLY `Read()`, `l + read * feedback`,
    // `out = read`** — the arithmetic the delay had before it had a character, in that order and with
    // no term added. The three gates below are what keeps that true, and each one is drawn where the
    // cell is genuinely OFF rather than merely gentle: TONE FF removes the filter (a one-pole at
    // 20 kHz is not transparent), WOBL 00 goes back to `Read()` (`ReadHermite` is a different
    // interpolator even at a standing position), and PONG off leaves each side to itself.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numFrames) {
        const bool drifting = wobble > 0.0f;
        const bool filtered = toneHex < 0xFF;
        // ⚠️ A GLIDE IS THE THIRD WAY THE HEAD CAN BE SOMEWHERE OTHER THAN `Read()`'s position, and it
        // borrows the wobble's interpolated read rather than adding one of its own. It ends by landing
        // EXACTLY on the target (below the loop), so a delay whose TIME nobody touches is back to
        // `Read()` on the very next block and the default arithmetic is untouched.
        const bool gliding = fabsf(delaySamples - headSamples) > kDelayGlideEpsilon;
        const bool moving  = drifting || gliding;
        const float glideCoeff  = 1.0f / (kDelayGlideSeconds * sampleRate);
        const float headAtStart = headSamples;

        // The drift is evaluated at the block's two ends and interpolated across it, the way the
        // track mute gate is, and it costs two sines per block rather than two per frame. ⚠️ The
        // term that bounds this is the FLUTTER, not the wow: at 5.3 Hz a 256-frame block is 3% of a
        // cycle, where a straight line between the ends is the curve. Raising kDelayFlutterHz much
        // further is what would make this an approximation worth worrying about.
        const float wobbleCentre = wobbleCentreSamples();
        float drift = 0.0f, driftStep = 0.0f;
        if (drifting && numFrames > 0) {
            const float depth   = wobbleDepthSamples();
            const float wowEnd  = wowPhase + kDelayTwoPi * kDelayWowHz / sampleRate * numFrames;
            const float flutEnd = flutterPhase + kDelayTwoPi * kDelayFlutterHz / sampleRate * numFrames;
            drift = depth * (0.8f * sinf(wowPhase) + 0.2f * sinf(flutterPhase));
            const float driftEnd = depth * (0.8f * sinf(wowEnd) + 0.2f * sinf(flutEnd));
            driftStep    = (driftEnd - drift) / static_cast<float>(numFrames);
            wowPhase     = fmodf(wowEnd, kDelayTwoPi);
            flutterPhase = fmodf(flutEnd, kDelayTwoPi);
        }

        for (int i = 0; i < numFrames; i++) {
            float l = inL[i], r = inR[i];
            if (inputEq.active) {
                inputEq.processStereo(l, r);
            }

            float readL, readR;
            if (moving) {
                if (gliding) {
                    // One pole toward the target, rate-capped. The head's SPEED is the pitch shift —
                    // see the constants — so this line is the whole tape effect.
                    float rate = (delaySamples - headSamples) * glideCoeff;
                    rate = fmaxf(-kDelayGlideMaxRate, fminf(rate, kDelayGlideMaxRate));
                    headSamples += rate;
                }
                // ⚠️ Clamped to leave ReadHermite its four taps — it reads t−1 through t+2, so a
                // position at either end of the line would wrap past the write head and pull the
                // OLDEST audio in the buffer out as if it were the newest.
                float pos = wobbleCentre + (headSamples - headAtStart) + drift;
                pos = fmaxf(2.0f, fminf(pos, static_cast<float>(DELAY_MAX_SAMPLES) - 3.0f));
                readL = delL.ReadHermite(pos);
                readR = delR.ReadHermite(pos);
                drift += driftStep;
            } else {
                readL = delL.Read();
                readR = delR.Read();
            }

            float fbL = pong ? readR : readL;
            float fbR = pong ? readL : readR;

            if (filtered) {
                toneStateL += toneCoeff * (fbL - toneStateL);
                toneStateR += toneCoeff * (fbR - toneStateR);
                fbL = toneStateL;
                fbR = toneStateR;
            }

            const float regenL = fbL * feedback;
            const float regenR = fbR * feedback;

            // ⚠️⚠️ **WHERE THE INPUT ENTERS IS WHAT MAKES A PING-PONG A PING-PONG — the cross-feed
            // above is not enough on its own.** The send bus is STEREO but a CENTRED instrument
            // arrives with L == R, so PONG on symmetric input would be arithmetic that cancels — the
            // two lines holding identical audio for ever, and the bounce never happening. Summing to
            // mono and injecting into ONE line is what makes the bounce a property of the EFFECT
            // rather than of how the source happened to be panned.
            //
            // ⚠️ So PONG deliberately DISCARDS the source's pan. That is what the name promises, and
            // PONG off is where a panned echo still lives.
            delL.Write((pong ? (l + r) * 0.5f : l) + regenL);
            delR.Write((pong ? 0.0f : r) + regenR);
            outL[i] = readL;
            outR[i] = readR;
        }

        // ⚠️ **ARRIVAL IS EXACT, AND IT HAS TO BE.** A one-pole only approaches, so without this the
        // head would creep for ever and `Read()` — which is where the default arithmetic lives — would
        // never come back.
        if (gliding && fabsf(delaySamples - headSamples) <= kDelayGlideEpsilon)
            headSamples = delaySamples;
    }

  private:
    void updateToneCoeff() {
        const float cutoffHz = 200.0f * powf(100.0f, toneHex / 255.0f);
        const float coeff    = 1.0f - expf(-kDelayTwoPi * cutoffHz / sampleRate);
        toneCoeff = fminf(1.0f, fmaxf(0.0f, coeff));
    }

    // The one writer of the delay time, because it has three homes: the lines' own copy, the target
    // the head glides to, and — on a load — the head itself.
    void setDelaySamples(float samples) {
        samples      = fmaxf(1.0f, fminf(samples, (float)(DELAY_MAX_SAMPLES - 1)));
        delaySamples = samples;
        if (snapNextTime) {
            headSamples  = samples;
            snapNextTime = false;
        }
        delL.SetDelay(samples);
        delR.SetDelay(samples);
    }
};
