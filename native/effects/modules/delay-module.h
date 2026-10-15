#pragma once
#include "../primitives/daisysp/delayline.h"
#include "../primitives/daisysp/dsp.h"
#include "../primitives/bus-sleep.h"
#include "delay-presets.h"
#include "eq-module.h"
#include <cmath>
#include <cstring>

// The line's length per channel: 2 s at 44.1 kHz, less at higher rates.
// ⚠️ A sample count, so both setters clamp silently past it: "00-FF → 0-2 s" holds only at 44.1 kHz,
// and the longest sync subdivisions are out of reach at slow tempos.
static constexpr size_t DELAY_MAX_SAMPLES = 88200;

// Sync mode's subdivisions, in quarter-note beats:
// 00=1/1  01=1/2   02=1/4   03=1/8   04=1/16  05=1/32
// 06=1/4T 07=1/8T  08=1/16T 09=1/4.  10=1/8.  11=1/16.
static const float kDelaySyncBeats[] = {
    4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f,
    2.0f / 3.0f, 1.0f / 3.0f, 1.0f / 6.0f,
    1.5f, 0.75f, 0.375f
};
static constexpr int kDelaySyncCount = 12;

// WOBL's drift: a slow wow under a faster flutter, two sines.
// ⚠️ Deliberately not noise, so a render stays reproducible.
static constexpr float kDelayWowHz     = 0.71f;
static constexpr float kDelayFlutterHz = 5.3f;

// How far the head drifts at WOBL FF, as a fraction of the delay time, capped in seconds so a long
// echo's pitch swing stays near a semitone.
static constexpr float kDelayWobbleFraction = 0.025f;
static constexpr float kDelayWobbleMaxSeconds = 0.007f;

static constexpr float kDelayTwoPi = 6.28318530717958647692f;

// TONE's range, 00 to FF, on an exponential curve.
static constexpr float kDelayToneLowHz  = 356.4f;
static constexpr float kDelayToneHighHz = 20000.0f;

// Above FDBK E0 the loop sings: a band-pass (without the high-pass it silts into rumble), gain past
// unity that the band mostly eats back, and `SoftLimit` holding the note near −2 dBFS.
// ⚠️ Headroom is not the loop gain — judge it by ear. Below E0 the arithmetic is the plain delay's.
static constexpr float kDelayOscOnset       = 224.0f / 255.0f;   // FDBK E0
static constexpr float kDelayOscHighpassHz  = 180.0f;
static constexpr float kDelayOscLowpassHz   = 5000.0f;
static constexpr float kDelayOscHeadroom    = 0.25f;

// A new TIME glides the read head there (a one-pole) instead of jumping, which would click; the
// moving head is the tape pitch swoop. Only the playback ratio is bounded, four octaves either way.
static constexpr float kDelayGlideSeconds  = 0.08f;
static constexpr float kDelayGlideMinRatio = 1.0f / 16.0f;
static constexpr float kDelayGlideMaxRatio = 16.0f;
// Close enough to count as arrived, in samples.
static constexpr float kDelayGlideEpsilon = 0.05f;

// The delay send: a stereo line per channel, the input EQ in front of it.
struct DelayModule {
    daisysp::DelayLine<float, DELAY_MAX_SAMPLES> delL;
    daisysp::DelayLine<float, DELAY_MAX_SAMPLES> delR;
    EqModule inputEq;
    float    feedback   = 0.375f;
    float    sampleRate = 44100.0f;

    // ⚠️ Each is off at its default, and there `process` is exactly the plain delay, so an older
    // project plays unchanged.
    bool  pong      = false;   // a side's repeat feeds the other line
    int   toneHex   = 0xFF;    // the cell; the coefficient depends on the rate
    float toneCoeff = 1.0f;    // one-pole on the line's input; 1 lets everything through
    float wobble    = 0.0f;    // 0..1, how far the read head drifts

    // The target delay time. The lines keep their own copy, so every write goes through
    // `setDelaySamples`.
    float delaySamples = 22050.0f;

    // Where the read head is: `delaySamples` except mid-glide. The wobble swings around the head.
    float headSamples = 22050.0f;

    // Armed by `reset`: the first TIME after a load places the head instead of gliding to it.
    bool snapNextTime = true;

    // Cleared on reset so a load does not ring the last project's audio.
    float toneStateL = 0.0f, toneStateR = 0.0f;
    float wowPhase = 0.0f, flutterPhase = 0.0f;

    // The singing band's two one-poles.
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
        // Re-armed: the project's own time, pushed next, must land rather than glide.
        snapNextTime = true;
        toneStateL = toneStateR = 0.0f;
        oscHpL = oscHpR = oscLpL = oscLpR = 0.0f;
        wowPhase = flutterPhase = 0.0f;
        updateToneCoeff();   // the cutoffs depend on the rate
        updateOscCoeffs();
    }

    // How far into the singing region FDBK sits: 0 below the onset, 1 at FF.
    float oscAmount() const {
        return fminf(1.0f, fmaxf(0.0f, (feedback - kDelayOscOnset) / (1.0f - kDelayOscOnset)));
    }

    // Free mode: timeHex 00-FF → 0–2 seconds
    void setParamsFree(int timeHex, int feedbackHex) {
        setTimeFree(timeHex);
        feedback = feedbackHex / 255.0f;
    }


    // ⚠️ The TIM command always uses the free scale, even in sync mode: a ramp needs 256 steps, and
    // the subdivisions are a list.
    void setTimeFree(int timeHex) {
        setDelaySamples((timeHex / 255.0f) * 2.0f * sampleRate);
    }

    void setTimeSync(int subdivIdx, float bpm) {
        if (subdivIdx < 0 || subdivIdx >= kDelaySyncCount) subdivIdx = 2;
        setDelaySamples(kDelaySyncBeats[subdivIdx] * (60.0f / bpm) * sampleRate);
    }

    // How far the read head swings, in frames. Public so a measurement reads what `process` uses.
    float wobbleDepthSamples() const {
        const float proportional = wobble * headSamples * kDelayWobbleFraction;
        const float ceiling      = wobble * kDelayWobbleMaxSeconds * sampleRate;
        return fminf(proportional, ceiling);
    }

    // The centre of the swing: the head, pulled down just enough that a full swing fits the line.
    float wobbleCentreSamples() const {
        const float ceiling = static_cast<float>(DELAY_MAX_SAMPLES) - 3.0f - wobbleDepthSamples();
        return fminf(headSamples, fmaxf(2.0f, ceiling));
    }

    // PONG, TONE and WOBL. TONE is 356 Hz–20 kHz exponential; ⚠️ FF switches the filter out.
    void setCharacter(bool pongOn, int toneHexIn, int wobbleHex) {
        pong    = pongOn;
        toneHex = toneHexIn;
        wobble  = wobbleHex / 255.0f;
        updateToneCoeff();
    }

    // Stereo send in, 100 % wet out.
    // ⚠️ At the default cells this must collapse to exactly `Read()`, `l + read * feedback`,
    // `out = read` — ReadHermite differs even standing still.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numFrames) {
        if (sleep.skip(inL, inR, numFrames)) {
            headSamples = delaySamples;   // quiet: a pending TIME change lands without a glide
            std::memset(outL, 0, sizeof(float) * static_cast<size_t>(numFrames));
            std::memset(outR, 0, sizeof(float) * static_cast<size_t>(numFrames));
            return;
        }
        const bool  drifting = wobble > 0.0f;
        const bool  filtered = toneHex < 0xFF;
        // Scales every singing term, so stepping DF → E0 does not jump.
        const float osc     = oscAmount();
        const bool  singing = osc > 0.0f;
        const float oscGain = feedback + kDelayOscHeadroom * osc;
        // A glide uses the wobble's interpolated read until it lands exactly on the target.
        const bool gliding = fabsf(delaySamples - headSamples) > kDelayGlideEpsilon;
        const bool moving  = drifting || gliding;
        const float glideCoeff  = 1.0f / (kDelayGlideSeconds * sampleRate);
        const float headAtStart = headSamples;

        // The drift at the block's two ends, interpolated across it.
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
                    float rate = (delaySamples - headSamples) * glideCoeff;
                    rate = fmaxf(1.0f - kDelayGlideMaxRatio, fminf(rate, 1.0f - kDelayGlideMinRatio));
                    headSamples += rate;
                }
                // ⚠️ ReadHermite reads t−1..t+2; at either end it would wrap into the oldest audio.
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
                // The band. The low-pass stops the saturator's new harmonics stacking into hiss.
                oscHpL += oscHpCoeff * (fbL - oscHpL);
                oscHpR += oscHpCoeff * (fbR - oscHpR);
                oscLpL += oscLpCoeff * ((fbL - oscHpL) - oscLpL);
                oscLpR += oscLpCoeff * ((fbR - oscHpR) - oscLpR);

                regenL += osc * (oscLpL * oscGain - regenL);
                regenR += osc * (oscLpR * oscGain - regenR);

                // ⚠️ The only bound on this loop: the gain above is past unity on purpose.
                regenL += osc * (daisysp::SoftLimit(regenL) - regenL);
                regenR += osc * (daisysp::SoftLimit(regenR) - regenR);
            }

            // ⚠️ PONG feeds the input, summed to mono, into one line: a centred source would
            // otherwise bounce nothing. The source's pan is dropped on purpose.
            float writeL = (pong ? (l + r) * 0.5f : l) + regenL;
            float writeR = (pong ? 0.0f : r) + regenR;

            // TONE filters what enters the line, so the first repeat is filtered too.
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

        // ⚠️ Snap on arrival: a one-pole only approaches, and `Read()` would never come back.
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

    // The one writer of the delay time: the lines' copy, the glide target and, on a load, the head.
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
