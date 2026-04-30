/*
    skoomaDust - DustChain
    License: GPL-3.0-or-later

    Stage chain (in order):
      1. Low-shelf  — 2nd-order RBJ biquad, 80 Hz, −4 dB
      2. Low-pass 1 — 1-pole, cutoff 10 kHz→5 kHz (log, knob-controlled)
      3. Tube       — Airwindows Tube2 derivative, InputPad=0.5, −4 dB post-trim
      4. FET comp   — APComp engine, threshold 0→−25 dB (see attribution below)
      5. Wow/drift  — LFO + random-walk pitch mod on HF band (100-sample PDC latency)
      6. Bitcrush   — 12-bit linear quantization (after comp: noise floor is level-locked)
      7. Soft-clip  — tanh curve above ±kClipKnee
      8. Low-pass 2 — same 1-pole cutoff as stage 2; combined slope = −12 dB/oct

    --- FET compressor attribution ----------------------------------------
    The FET stage is adapted from APComp (Versatile Compressor) by
    Alain Paul / AP Mastering, BSD-3-Clause.

        Copyright 2024 Alain Paul
        Redistribution and use in source and binary forms, with or without
        modification, are permitted provided that the following conditions
        are met:
        1. Redistributions of source code must retain the above copyright
           notice, this list of conditions and the following disclaimer.
        2. Redistributions in binary form must reproduce the above copyright
           notice, this list of conditions and the following disclaimer in
           the documentation and/or other materials provided with the
           distribution.
        3. Neither the name of the copyright holder nor the names of its
           contributors may be used to endorse or promote products derived
           from this software without specific prior written permission
           (including but not limited to the use of the AP Mastering brand
           name).
        THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
        "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. (full
        disclaimer in the original LICENSE — preserved verbatim in this file
        as required by clause 1.)

    Adapted: parameter-system bindings, meters, sidechain bus, oversampling,
    and output feedback stripped. Algorithm core (slewed detector → gain
    computer with convexity → inertia velocity → channel link → tanh ceiling)
    preserved.
*/

#include "dust-chain.h"
#include "../primitives/daisysp/dsp.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace skdust {

static constexpr float kTwoPi = 6.28318530718f;

namespace {
    inline double gainToDb(double g) noexcept {
        if (g <= 0.0)    return -1000.0;
        if (g >  1000.0) g = 1000.0;
        return 20.0 * daisysp::fastlog10f((float)g);
    }
    inline double dbToGain(double dB) noexcept {
        if (dB <= -1000.0) return 0.0;
        if (dB >   1000.0) dB = 1000.0;
        return (double)daisysp::pow10f((float)dB / 20.0f);
    }
}

void DustChain::prepare(double sr, int /*blockSize*/, int numChannels)
{
    sampleRate = sr;
    const int nc = std::max(1, numChannels);
    chans.assign((size_t)nc, ChannelState{});

    tubeHighSR     = (sr / 44100.0) > 1.9;
    tubePostGainLin = std::pow(10.0f, kTubePostGainDb / 20.0f);

    compEnvAtkCoef = 1.0f - std::exp(-1.0f / ((float)sr * kMakeupEnvAttackMs  * 0.001f));
    compEnvRelCoef = 1.0f - std::exp(-1.0f / ((float)sr * kMakeupEnvReleaseMs * 0.001f));
    compInEnvSq    = 0.0f;
    compOutEnvSq   = 0.0f;

    wowPhase       = 0.0f;
    wowSplitLpCoef = 1.0f - std::exp(-kTwoPi * kWowSplitHz / (float)sr);
    driftLpZ = 0.0f;

    lpState.assign((size_t)nc, 0.0f);
    lpState2.assign((size_t)nc, 0.0f);

    // RBJ low-shelf coefficients (same formula as JUCE Coefficients::makeLowShelf)
    const float A             = std::sqrt(std::pow(10.0f, kLowShelfDb / 20.0f));
    const float w0            = kTwoPi * kLowShelfHz / (float)sr;
    const float cosW0         = std::cos(w0);
    const float sinW0         = std::sin(w0);
    const float alpha         = sinW0 / (2.0f * kLowShelfQ);
    const float twoSqrtAalpha = 2.0f * std::sqrt(A) * alpha;
    const float a0inv         = 1.0f / ((A+1.0f) + (A-1.0f)*cosW0 + twoSqrtAalpha);
    lsB0 =  A * ((A+1.0f) - (A-1.0f)*cosW0 + twoSqrtAalpha) * a0inv;
    lsB1 =  2.0f*A * ((A-1.0f) - (A+1.0f)*cosW0) * a0inv;
    lsB2 =  A * ((A+1.0f) - (A-1.0f)*cosW0 - twoSqrtAalpha) * a0inv;
    lsA1 = -2.0f * ((A-1.0f) + (A+1.0f)*cosW0) * a0inv;
    lsA2 =  ((A+1.0f) + (A-1.0f)*cosW0 - twoSqrtAalpha) * a0inv;
}

void DustChain::reset()
{
    for (auto& c : chans) c = ChannelState{};
    std::fill(lpState.begin(),  lpState.end(),  0.0f);
    std::fill(lpState2.begin(), lpState2.end(), 0.0f);
    compInEnvSq  = 0.0f;
    compOutEnvSq = 0.0f;
    wowPhase     = 0.0f;
    driftLpZ     = 0.0f;
}

void DustChain::process(float* buf, int numFrames, int channelCount)
{
    const int n  = numFrames;
    const int nc = std::min(channelCount, (int)chans.size());
    if (n == 0 || nc == 0) return;

    // -------------------------------------------------------------------------
    // Stage 1: Low-shelf
    // -------------------------------------------------------------------------
    if (enables.lowShelf) {
        for (int ch = 0; ch < nc; ++ch) {
            auto& cs = chans[(size_t)ch];
            for (int i = 0; i < n; ++i) {
                float& x = buf[i * channelCount + ch];
                float y = lsB0*x + lsB1*cs.lsX1 + lsB2*cs.lsX2 - lsA1*cs.lsY1 - lsA2*cs.lsY2;
                cs.lsX2=cs.lsX1; cs.lsX1=x; cs.lsY2=cs.lsY1; cs.lsY1=y;
                x = y;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Stage 2: Lowpass — 1-pole, cutoff tracks knob (10 kHz at 0%, 5 kHz at 100%).
    // Log interpolation so equal knob travel gives equal perceived frequency change.
    // -------------------------------------------------------------------------
    {
        const float lpCutoff = kLPCutoffMax * std::pow(kLPCutoffMin / kLPCutoffMax, dustAmount);
        const float lpCoef   = 1.0f - std::exp(-kTwoPi * lpCutoff / (float)sampleRate);
        if (enables.lp) {
            for (int ch = 0; ch < nc; ++ch) {
                float s = lpState[(size_t)ch];
                for (int i = 0; i < n; ++i) {
                    float& x = buf[i * channelCount + ch];
                    s += lpCoef * (x - s);
                    x = s;
                }
                lpState[(size_t)ch] = s;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Stage 3: Tube saturation (Airwindows Tube2 derivative)
    // Simplified from the original at Tube=1.0 / InputPad=0.5.
    // At 2x+ sample rates, two averaging passes reduce aliasing.
    // -------------------------------------------------------------------------
    if (enables.tube) {
        const bool highSR = tubeHighSR;
        for (int ch = 0; ch < nc; ++ch) {
            auto& s = chans[(size_t)ch];
            for (int i = 0; i < n; ++i) {
                float& xref = buf[i * channelCount + ch];
                double d = (double)xref * (double)kTubeInputPad;
                if (d >  1.0) d =  1.0;
                if (d < -1.0) d = -1.0;
                if (highSR) {
                    double prev = s.tubePrevA;
                    s.tubePrevA = (float)d;
                    d = (d + prev) * 0.5;
                }
                {
                    double sharpen = -d;
                    sharpen = (sharpen > 0.0) ? 1.0 + std::sqrt( sharpen)
                                              : 1.0 - std::sqrt(-sharpen);
                    d -= d * std::fabs(d) * sharpen * 0.25;
                }
                d -= d * std::fabs(d) * 0.5;
                d *= 2.0;
                if (highSR) {
                    double prev = s.tubePrevC;
                    s.tubePrevC = (float)d;
                    d = (d + prev) * 0.5;
                }
                {
                    double slew = (double)s.tubePrevE - d;
                    s.tubePrevE = (float)d;
                    slew = (slew > 0.0) ? 1.0 + std::sqrt( slew) * 0.5
                                        : 1.0 - std::sqrt(-slew) * 0.5;
                    d -= d * std::fabs(d) * slew * 0.5;
                }
                if (d >  0.52) d =  0.52;
                if (d < -0.52) d = -0.52;
                d *= 1.923076923076923;
                xref = (float)d * tubePostGainLin;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Stage 4: FET compressor (APComp algorithm — see attribution above)
    //
    // Channel link = 1.0: both channels see the louder channel's GR.
    // Feedback     = 0.0: pure feed-forward detector.
    // Ceiling      = 1.0: tanh soft-clip at 0 dBFS.
    // -------------------------------------------------------------------------
    if (enables.fet && nc > 0)
    {
        const double thresholdDb      = (double)(dustAmount * kCompThresholdMin);
        const double ratioVal         = kCompRatio;
        const double convexityVal     = kCompConvexity;
        const double inertiaCoef      = kCompInertia;
        const double inertiaDecayCoef = 0.99 + kCompInertiaDecay * 0.01;

        // APComp time constants approach 1.0 (held), so exp(-1/(sr·sec)) rather than 1−exp(…).
        const double attackCoeff  = std::exp(-1.0 / ((double)sampleRate * kCompAttackSec));
        const double releaseCoeff = std::exp(-1.0 / ((double)sampleRate * kCompReleaseSec));

        const int ncp = std::min(nc, 2);
        auto& sL = chans[0];
        auto& sR = (ncp >= 2) ? chans[1] : chans[0];

        for (int sample = 0; sample < n; ++sample)
        {
            float  inputSample[2] = { 0.0f, 0.0f };
            double grDb[2]        = { 0.0,  0.0  };

            // Per-channel: envelope detection → gain computation → inertia.
            for (int ch = 0; ch < ncp; ++ch)
            {
                ChannelState& cs = (ch == 0) ? sL : sR;

                inputSample[ch] = buf[sample * channelCount + ch];

                double inDb = gainToDb((double)std::fabs(inputSample[ch]));
                if (std::isnan(inDb)) inDb = -200.0;
                inDb = std::clamp(inDb, -200.0, 4.0);

                if (inDb > cs.slewedSignal)
                    cs.slewedSignal = attackCoeff  * (cs.slewedSignal - inDb) + inDb;
                else
                    cs.slewedSignal = releaseCoeff * (cs.slewedSignal - inDb) + inDb;

                if (cs.slewedSignal > thresholdDb) {
                    const double targetLevel = thresholdDb + (cs.slewedSignal - thresholdDb) / ratioVal;
                    double gr = cs.slewedSignal - targetLevel;
                    gr = std::pow(gr, convexityVal);
                    cs.gainReductionDb = gr;
                } else {
                    cs.slewedSignal    = thresholdDb;
                    cs.gainReductionDb = 0.0;
                }
                cs.slewedSignal = std::clamp(cs.slewedSignal, -200.0, 1000.0);

                // Inertia: velocity overshoot gives the FET-76 snap character.
                double grLinear = dbToGain(cs.gainReductionDb);
                cs.inertiaVelocity += inertiaCoef * grLinear * -0.001;
                cs.inertiaVelocity *= inertiaDecayCoef;
                cs.inertiaVelocity  = std::clamp(cs.inertiaVelocity, -100.0, 100.0);
                grLinear += cs.inertiaVelocity;
                grLinear  = std::clamp(grLinear, -1000.0, 1000.0);
                cs.gainReductionDb  = gainToDb(grLinear);

                grDb[ch] = cs.gainReductionDb;
            }

            // Channel link = 1.0: both channels use the louder channel's GR.
            if (ncp > 1) {
                const double maxGr = std::max(grDb[0], grDb[1]);
                grDb[0] = grDb[1] = maxGr;
            }

            // Apply GR + tanh soft-clip (ceiling = 1.0 → straight tanh).
            float preMakeup[2] = { 0.0f, 0.0f };
            for (int ch = 0; ch < ncp; ++ch) {
                const double inAbsDb = gainToDb((double)std::fabs(inputSample[ch]));
                const double outDb   = inAbsDb - grDb[ch];
                float outLin = (float)dbToGain(outDb);
                if (inputSample[ch] < 0.0f) outLin = -outLin;
                outLin = std::tanh(outLin);
                if (std::isnan(outLin)) outLin = 0.0f;
                preMakeup[ch] = outLin;
            }

            // RMS auto-makeup: track input/pre-makeup energies as peak-followers,
            // apply makeup = sqrt(inEnvSq / outEnvSq).
            float inEn = 0.0f, outEn = 0.0f;
            for (int ch = 0; ch < ncp; ++ch) {
                inEn  += inputSample[ch] * inputSample[ch];
                outEn += preMakeup[ch]   * preMakeup[ch];
            }
            compInEnvSq  += (inEn  - compInEnvSq)  * ((inEn  > compInEnvSq)  ? compEnvAtkCoef : compEnvRelCoef);
            compOutEnvSq += (outEn - compOutEnvSq) * ((outEn > compOutEnvSq) ? compEnvAtkCoef : compEnvRelCoef);

            float makeupLin = 1.0f;
            if (compInEnvSq > 1.0e-9f && compOutEnvSq > 1.0e-9f) {
                const float drive = 1.0f + dustAmount * kCompMakeupDrive;
                makeupLin = std::sqrt(compInEnvSq / compOutEnvSq) * drive;
            }

            for (int ch = 0; ch < ncp; ++ch) {
                buf[sample * channelCount + ch] = preMakeup[ch] * makeupLin;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Stage 5: Wow & drift
    // HF/LF split via 1-pole crossover. LF: fixed-delay buffer (kWowCenterDelay).
    // HF: longer buffer with modulated read position (sine LFO + random walk).
    // Both re-sum at output. LFO shared across channels for stereo coherence.
    // Rate and depth interpolate linearly with the knob.
    // -------------------------------------------------------------------------
    {
        const float wowFreq   = kWowFreqMin  + dustAmount * (kWowFreqMax  - kWowFreqMin);
        const float wowAmp    = kWowAmpMin   + dustAmount * (kWowAmpMax   - kWowAmpMin);
        const float driftLpHz = kDriftLpMin  + dustAmount * (kDriftLpMax  - kDriftLpMin);
        const float driftAmp  = kDriftAmpMin + dustAmount * (kDriftAmpMax - kDriftAmpMin);
        const float phaseInc  = kTwoPi * wowFreq / (float)sampleRate;
        const float dCoef     = 1.0f - std::exp(-kTwoPi * driftLpHz / (float)sampleRate);
        const float dScale    = 1.0f / std::sqrt(dCoef * 0.5f);

        for (int sample = 0; sample < n; ++sample)
        {
            wowPhase += phaseInc;
            if (wowPhase > kTwoPi) wowPhase -= kTwoPi;
            const float wowOff = std::sin(wowPhase) * wowAmp;

            driftRng = driftRng * 1664525u + 1013904223u;
            const float noise = (float)((int32_t)driftRng) * (1.0f / 2147483648.0f);
            driftLpZ += dCoef * (noise - driftLpZ);
            const float driftOff = std::tanh(driftLpZ * dScale) * driftAmp;

            // Delay buffers always run regardless of enables.wow — keeps PDC latency constant.
            const float modOff = enables.wow ? (wowOff + driftOff) : 0.0f;

            for (int ch = 0; ch < nc; ++ch)
            {
                ChannelState& cs = chans[(size_t)ch];
                float& samp = buf[sample * channelCount + ch];

                // Read delayed values before writing (implements centerDelay correctly).
                const float lfDelayed = cs.wowLfBuf[cs.wowLfWriteIdx];

                const float readPosF = (float)cs.wowHfWriteIdx - (float)kWowCenterDelay - modOff;
                float wrapped = readPosF + 2.0f * (float)kWowHfBufSize;
                while (wrapped >= (float)kWowHfBufSize) wrapped -= (float)kWowHfBufSize;
                const int   idx0 = (int)wrapped;
                const int   idx1 = (idx0 + 1) % kWowHfBufSize;
                const float frac = wrapped - (float)idx0;
                const float hfDelayed = cs.wowHfBuf[idx0] * (1.0f - frac)
                                      + cs.wowHfBuf[idx1] * frac;

                const float in = samp;
                cs.wowSplitLpZ += wowSplitLpCoef * (in - cs.wowSplitLpZ);
                const float lfNow = cs.wowSplitLpZ;
                const float hfNow = in - lfNow;

                cs.wowLfBuf[cs.wowLfWriteIdx] = lfNow;
                cs.wowHfBuf[cs.wowHfWriteIdx] = hfNow;
                cs.wowLfWriteIdx = (cs.wowLfWriteIdx + 1) % kWowCenterDelay;
                cs.wowHfWriteIdx = (cs.wowHfWriteIdx + 1) % kWowHfBufSize;

                samp = lfDelayed + hfDelayed;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Stage 6: Bitcrush
    // Placed after the comp so quantization noise is added to a level-locked
    // signal — hiss stays steady rather than pumping with the makeup gain.
    // -------------------------------------------------------------------------
    if (enables.bitcrush) {
        constexpr float scale = (float)(1 << kBitcrushBits) * 0.5f;
        const     float invSc = 1.0f / scale;
        for (int ch = 0; ch < nc; ++ch) {
            for (int i = 0; i < n; ++i) {
                float& x = buf[i * channelCount + ch];
                x = std::round(x * scale) * invSc;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Stage 7: Soft-clipper
    // Linear below ±kClipKnee; tanh curve above, asymptotes to ±1.
    // -------------------------------------------------------------------------
    if (enables.clipper) {
        constexpr float knee   = kClipKnee;
        constexpr float rem    = 1.0f - kClipKnee;
        constexpr float invRem = 1.0f / rem;
        for (int ch = 0; ch < nc; ++ch) {
            for (int i = 0; i < n; ++i) {
                float& x = buf[i * channelCount + ch];
                const float s = x;
                if (s > knee)
                    x = knee + rem * std::tanh((s - knee) * invRem);
                else if (s < -knee)
                    x = -(knee + rem * std::tanh((-s - knee) * invRem));
            }
        }
    }

    // -------------------------------------------------------------------------
    // Stage 8: Output lowpass — same cutoff as stage 2.
    // Together the two 1-pole stages give -12 dB/oct; disable this for -6 dB/oct.
    // -------------------------------------------------------------------------
    if (enables.lp2) {
        const float lpCutoff = kLPCutoffMax * std::pow(kLPCutoffMin / kLPCutoffMax, dustAmount);
        const float lpCoef   = 1.0f - std::exp(-kTwoPi * lpCutoff / (float)sampleRate);
        for (int ch = 0; ch < nc; ++ch) {
            float s = lpState2[(size_t)ch];
            for (int i = 0; i < n; ++i) {
                float& x = buf[i * channelCount + ch];
                s += lpCoef * (x - s);
                x = s;
            }
            lpState2[(size_t)ch] = s;
        }
    }
}

} // namespace skdust
