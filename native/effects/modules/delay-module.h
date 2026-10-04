#pragma once
#include "../primitives/daisysp/delayline.h"
#include "../primitives/daisysp/dsp.h"
#include "../primitives/bus-sleep.h"
#include "delay-presets.h"
#include "eq-module.h"
#include <cmath>
#include <cstring>

// Max delay line: 88200 samples per channel — 2 seconds at 44100 Hz, 1.84 at 48000, 0.92 at 96000.
//
// ⚠️ IT IS A SAMPLE COUNT, so the ceiling is a TIME that moves with the device rate, and both setters
// CLAMP SILENTLY past it: free mode's "00-FF → 0-2 s" holds only at 44.1 kHz, and sync mode's longest
// subdivisions are unreachable at slow tempos (1/1 is exactly 2 s at 120 BPM). Growing it costs
// 706 KB per doubling; limiting subdivisions by tempo would make a cell's range depend on the tempo.
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

// How far the head drifts, as a fraction of the delay TIME, at WOBL FF — tape speed error is a
// percentage, so a slapback wobbles less than a long echo.
// ⚠️ What is audible is the PITCH deviation (`depth × 2πf / rate`), so the fast flutter term does
// most of the audible work.
static constexpr float kDelayWobbleFraction = 0.025f;

// ⚠️ AND A CEILING ON IT, IN TIME: the pitch swing grows with TIME, so WOBL FF would be most of an
// octave on a two-second echo. The cap holds the strongest setting near a semitone while short
// delays stay proportional.
static constexpr float kDelayWobbleMaxSeconds = 0.007f;

static constexpr float kDelayTwoPi = 6.28318530717958647692f;

// TONE's range, 00 to FF, on an exponential curve.
static constexpr float kDelayToneLowHz  = 356.4f;
static constexpr float kDelayToneHighHz = 20000.0f;

// ─── Where the echo starts to sing ───────────────────────────────────────────────────────────────
//
// Past `kDelayOscOnset` the loop becomes a tape machine pushed too far. Three things arrive together:
//
//   * ⭐⭐⭐ A BAND, NOT A ROLL-OFF. TONE is a low-pass, so without a bottom bound the steady sound
//     silts up below 100 Hz into a rumble. ⚠️ It does not collapse all the way to DC.
//   * Gain past unity, most of which the band eats back, so the howl blooms over seconds.
//     ⚠️ The nominal figure is NOT the loop gain; judge this constant by ear, not by its value.
//   * A curve that bounds it: `SoftLimit` turns growth into a steady note at about −2 dBFS.
//
// WOBL is the fourth ingredient, left to the user.
//
// ⚠️⚠️ The onset is at `E0`, so only the top eighth of FDBK is affected; below it — the `60`
// default included — the arithmetic is unchanged.
static constexpr float kDelayOscOnset       = 224.0f / 255.0f;   // FDBK E0
static constexpr float kDelayOscHighpassHz  = 180.0f;
static constexpr float kDelayOscLowpassHz   = 5000.0f;
static constexpr float kDelayOscHeadroom    = 0.25f;

// ─── How the read head reaches a new TIME ────────────────────────────────────────────────────────
//
// It GLIDES instead of jumping, and the tape pitch shift is simply what a moving read head does:
// output frame `n` is `buffer[n - D(n)]`, so the playback ratio is `1 - D'(n)` — a shortening delay
// pitches UP, a lengthening one DOWN. ⚠️ A jump is a discontinuity: a click per knob step.
//
// One-pole toward the target, so the pitch deviation decays exponentially, like a tape settling.
// ⚠️ A CAP ON THE RATE would flatten every big move into a constant detune before the swoop. Only
// the playback ratio is bounded, where it stops being a pitch (0 = DC, below = backwards, far
// above = Hermite aliasing) — four octaves either way.
static constexpr float kDelayGlideSeconds  = 0.08f;
static constexpr float kDelayGlideMinRatio = 1.0f / 16.0f;
static constexpr float kDelayGlideMaxRatio = 16.0f;
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

    // ⚠️⚠️ THREE INDEPENDENT SWITCHES, EACH OFF AT ITS DEFAULT — and at its default the arithmetic in
    // `process` is exactly the plain delay's, so an older project plays unchanged. Any combination
    // is legal: the TYPE cell only writes them.
    // ⚠️ Below `kDelayOscOnset` the regeneration gain is FDBK alone, topping out at exactly 1.0 (the
    // repeats hold, never grow); above it the gain passes unity and `SoftLimit` is the bound.
    bool  pong      = false;   // a side's repeat feeds the OTHER line
    int   toneHex   = 0xFF;    // kept as the CELL, because the coefficient below depends on the rate
    float toneCoeff = 1.0f;    // the one-pole on the line's input; 1 lets everything through
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

    // The singing band's two one-poles, and the coefficients depend on the rate exactly as TONE's do.
    float oscHpL = 0.0f, oscHpR = 0.0f, oscLpL = 0.0f, oscLpR = 0.0f;
    float oscHpCoeff = 0.0f, oscLpCoeff = 0.0f;

    BusSleep sleep;

    void reset(float sr) {
        sampleRate = sr;
        sleep.reset();
        delL.Init();
        delR.Init();
        inputEq.reset(sr);
        feedback = 0x60 / 255.0f;
        // Default: 1/4 note at 120 BPM = 500 ms (index 2)
        float defaultSamples = 1.0f * (60.0f / 120.0f) * sr;
        snapNextTime = true;
        setDelaySamples(defaultSamples);
        // ⚠️ RE-ARMED AFTER THE DEFAULT IS PLACED: the project's own time is pushed straight after a
        // reset, and that push is part of the load, so it must land rather than glide.
        snapNextTime = true;
        toneStateL = toneStateR = 0.0f;
        oscHpL = oscHpR = oscLpL = oscLpR = 0.0f;
        wowPhase = flutterPhase = 0.0f;
        // ⚠️ Re-derived here: the cutoff is a fraction of the RATE, and `reset` is where it changes.
        updateToneCoeff();
        updateOscCoeffs();
    }

    // How far into the singing region FDBK sits, 0 below the onset and 1 at FF. ⚠️ DERIVED from
    // `feedback` (exactly hex / 255), so no setter has to keep a second value in step.
    float oscAmount() const {
        return fminf(1.0f, fmaxf(0.0f, (feedback - kDelayOscOnset) / (1.0f - kDelayOscOnset)));
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

    // How far the read head swings, in frames, at the current TIME and WOBL. ⚠️ Stated once and
    // public: anything measuring the wobble must read the number `process` uses.
    float wobbleDepthSamples() const {
        const float proportional = wobble * headSamples * kDelayWobbleFraction;
        const float ceiling      = wobble * kDelayWobbleMaxSeconds * sampleRate;
        return fminf(proportional, ceiling);
    }

    // The position the drifting head swings AROUND — the delay time, pulled down just far enough
    // that a full swing still fits inside the line.
    //
    // ⚠️ At the longest TIME there is no room above to drift into, so the CENTRE moves down rather
    // than the swing flattening against the clamp — a couple of ms off a two-second echo.
    float wobbleCentreSamples() const {
        const float ceiling = static_cast<float>(DELAY_MAX_SAMPLES) - 3.0f - wobbleDepthSamples();
        return fminf(headSamples, fmaxf(2.0f, ceiling));
    }

    // The three character cells, together, because they arrive together from the project.
    //
    // TONE is the brightness of the repeats, 356 Hz–20 kHz exponential. ⚠️ FF IS THE FILTER
    // SWITCHED OUT, not the top of the curve: a one-pole at 20 kHz still takes a little off every
    // pass. FE is 19.7 kHz — inaudibly below FF.
    void setCharacter(bool pongOn, int toneHexIn, int wobbleHex) {
        pong    = pongOn;
        toneHex = toneHexIn;
        wobble  = wobbleHex / 255.0f;
        updateToneCoeff();
    }

    // Process stereo send bus into stereo wet output. Always 100% wet. Writes to outL/outR.
    // Each channel has its own delay line — panned instruments echo on the correct side.
    //
    // ⚠️⚠️ AT THE DEFAULT CELLS THIS LOOP MUST COLLAPSE TO EXACTLY `Read()`, `l + read * feedback`,
    // `out = read`. Each gate below sits where its cell is genuinely OFF: TONE FF removes the filter,
    // WOBL 00 goes back to `Read()` (ReadHermite differs even standing still), PONG off keeps sides apart.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numFrames) {
        if (sleep.skip(inL, inR, numFrames)) {
            // The line is quiet, so a TIME change made meanwhile can land now instead of gliding
            // under the next sound.
            headSamples = delaySamples;
            std::memset(outL, 0, sizeof(float) * static_cast<size_t>(numFrames));
            std::memset(outR, 0, sizeof(float) * static_cast<size_t>(numFrames));
            return;
        }
        const bool  drifting = wobble > 0.0f;
        const bool  filtered = toneHex < 0xFF;
        // ⚠️ Every term below is scaled by this, so at the onset step the band, gain and curve are
        // worth ZERO — otherwise `DF` → `E0` would jump in tone and level.
        const float osc     = oscAmount();
        const bool  singing = osc > 0.0f;
        const float oscGain = feedback + kDelayOscHeadroom * osc;
        // ⚠️ A GLIDE borrows the wobble's interpolated read. It lands EXACTLY on the target (below),
        // so an untouched delay is back on `Read()` next block.
        const bool gliding = fabsf(delaySamples - headSamples) > kDelayGlideEpsilon;
        const bool moving  = drifting || gliding;
        const float glideCoeff  = 1.0f / (kDelayGlideSeconds * sampleRate);
        const float headAtStart = headSamples;

        // The drift is evaluated at the block's two ends and interpolated across it (two sines per
        // block). ⚠️ The FLUTTER bounds that: at 5.3 Hz a 256-frame block is 3% of a cycle.
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
                    // One pole toward the target, bounded only where the ratio stops being a pitch.
                    // The head's SPEED is the pitch shift — see the constants — so this line is the
                    // whole tape effect.
                    float rate = (delaySamples - headSamples) * glideCoeff;
                    rate = fmaxf(1.0f - kDelayGlideMaxRatio, fminf(rate, 1.0f - kDelayGlideMinRatio));
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

            float regenL = fbL * feedback;
            float regenR = fbR * feedback;

            if (singing) {
                // The band. A one-pole high-pass is the input less its own low-passed self, and the
                // low-pass works alongside whatever TONE is doing rather than replacing it — the
                // saturator below makes new harmonics every pass, and with nothing above them they
                // stack into hiss instead of a note.
                oscHpL += oscHpCoeff * (fbL - oscHpL);
                oscHpR += oscHpCoeff * (fbR - oscHpR);
                oscLpL += oscLpCoeff * ((fbL - oscHpL) - oscLpL);
                oscLpR += oscLpCoeff * ((fbR - oscHpR) - oscLpR);

                regenL += osc * (oscLpL * oscGain - regenL);
                regenR += osc * (oscLpR * oscGain - regenR);

                // ⚠️ **THE BOUND ON THIS LOOP IS THIS LINE AND NOTHING ELSE** — the gain above is past
                // unity on purpose, so without the curve the repeats grow without end. The steady
                // note settles where the curve's own gain has fallen back to 1.
                regenL += osc * (daisysp::SoftLimit(regenL) - regenL);
                regenR += osc * (daisysp::SoftLimit(regenR) - regenR);
            }

            // ⚠️⚠️ WHERE THE INPUT ENTERS MAKES A PING-PONG: a centred instrument arrives with L == R,
            // so cross-feed alone would bounce nothing. PONG sums to mono into ONE line — deliberately
            // discarding the source's pan; PONG off is where a panned echo lives.
            float writeL = (pong ? (l + r) * 0.5f : l) + regenL;
            float writeR = (pong ? 0.0f : r) + regenR;

            // TONE filters what goes INTO the line, so the first repeat has passed it once and each
            // later one once more. On the regeneration alone it would leave the first repeat clean.
            if (filtered) {
                toneStateL += toneCoeff * (writeL - toneStateL);
                toneStateR += toneCoeff * (writeR - toneStateR);
                writeL = toneStateL;
                writeR = toneStateR;
            }

            delL.Write(writeL);
            delR.Write(writeR);
            outL[i] = readL;
            outR[i] = readR;
        }

        // ⚠️ **ARRIVAL IS EXACT, AND IT HAS TO BE.** A one-pole only approaches, so without this the
        // head would creep for ever and `Read()` — which is where the default arithmetic lives — would
        // never come back.
        if (gliding && fabsf(delaySamples - headSamples) <= kDelayGlideEpsilon)
            headSamples = delaySamples;

        // The span is the whole line: an echo can sit in it that long while the output is silent.
        sleep.observe(inL, inR, outL, outR, numFrames, static_cast<int>(DELAY_MAX_SAMPLES));
    }

  private:
    void updateOscCoeffs() {
        oscHpCoeff = 1.0f - expf(-kDelayTwoPi * kDelayOscHighpassHz / sampleRate);
        oscLpCoeff = 1.0f - expf(-kDelayTwoPi * kDelayOscLowpassHz / sampleRate);
        oscHpCoeff = fminf(1.0f, fmaxf(0.0f, oscHpCoeff));
        oscLpCoeff = fminf(1.0f, fmaxf(0.0f, oscLpCoeff));
    }

    void updateToneCoeff() {
        const float cutoffHz = kDelayToneLowHz * powf(kDelayToneHighHz / kDelayToneLowHz, toneHex / 255.0f);
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
