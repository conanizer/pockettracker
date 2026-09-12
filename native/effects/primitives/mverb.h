#pragma once
//	Copyright (c) 2010 Martin Eastwood
//  This code is distributed under the terms of the GNU General Public License
//
//  MVerb is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  at your option) any later version.
//
//  MVerb is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this MVerb.  If not, see <http://www.gnu.org/licenses/>.

// ===========================================================================
// MVerb — a Dattorro-style tank reverb with an early-reflection bank.
//
// Vendored from martineastwood/mverb (GPL-3.0, the same licence as this app). What it has that
// ReverbSc does not: a room SIZE that stretches the tank independently of the DECAY that empties it,
// an eight-tap early-reflection bank per channel with its own balance against the tail, and a
// DENSITY that sets how fast the input smears.
//
// ⚠️⚠️ **EVERY LINE IS SIZED FOR kMVerbDesignRate AND `SetLength` CLAMPS SILENTLY ABOVE IT.** Upstream
// declares all fifteen buffers at a flat 96000 samples regardless of the 0.0036 s–0.15 s its own
// `reset()` asks for — 5.5 MB as written, which will not start on a handheld. Each array here is
// sized to the longest length asked of it, and that is the whole reason this is a copy rather than a
// submodule. **A caller must never build it above the design rate**: ReverbModule clamps to exactly
// the ceiling ReverbSc has always had, and the two ceilings are deliberately the same number.
//
// ⚠️ **THE PRE-DELAY IS NOT IN HERE.** Upstream's line was removed rather than left unused: this is a
// send whose pre-delay ring lives in ReverbModule and is shared with the other algorithm, so PRE
// means the same gap whichever one is sounding. A second one would cost 38 KB to hold a copy of a
// decision taken outside.
//
// ⚠️ **NO DRY PATH AND NO OUTPUT GAIN.** Upstream's MIX and GAIN are plugin-host parameters; this is
// a 100 %-wet send whose level is the REV fader on the MIXER screen, so `process` writes wet alone.
//
// ⚠️ Three of upstream's smoothed values were computed and then never read — DENSITY and the
// early/late balance were applied raw, so turning either while the reverb sounded put a step into a
// ringing tail. Both are read from the smoothed value here. SIZE is the one that genuinely cannot be
// smoothed: it re-lengths the delay lines, which is a discontinuity by construction.
// ===========================================================================

#include <cstring>
#include <cmath>

namespace mverb {

// The rate every buffer below is sized for. ⚠️ A CEILING, not a default: above it the lines clamp and
// the reverb is quietly shorter and brighter than it was asked to be.
inline constexpr int kMVerbDesignRate = 48000;

/** Samples needed to hold `seconds` at the design rate, with one to spare for the write head. */
constexpr int mverb_line(double seconds) {
    return static_cast<int>(seconds * kMVerbDesignRate) + 1;
}

// The longest length `reset()` and `applySize()` ask of each array, and nothing more. ⚠️⚠️ **THESE
// FOUR NUMBERS ARE THE MODULE'S MEMORY FOOTPRINT.** Lengthening a line below without lengthening the
// matching constant here is a silent clamp; lengthening one here without re-checking every
// `SetIndex` against it is a read past the end of the buffer.
inline constexpr int kAllpassLen  = mverb_line(0.0127);  // allpass[2], the longest of the four
inline constexpr int kTankApLen   = mverb_line(0.089);   // allpassFourTap[3]
inline constexpr int kTankLineLen = mverb_line(0.15);    // staticDelayLine[0]
inline constexpr int kEarlyLen    = mverb_line(0.089);   // earlyReflectionsDelayLine[0]

//forward declaration
template<typename T, int maxLength> class Allpass;
template<typename T, int maxLength> class StaticAllpassFourTap;
template<typename T, int maxLength> class StaticDelayLineFourTap;
template<typename T, int maxLength> class StaticDelayLineEightTap;
template<typename T, int OverSampleCount> class StateVariable;

template<typename T>
class MVerb
{
private:
    Allpass<T, kAllpassLen> allpass[4] = {};
    StaticAllpassFourTap<T, kTankApLen> allpassFourTap[4] = {};
    StateVariable<T,4> bandwidthFilter[2] = {};
    StateVariable<T,4> damping[2] = {};
    StaticDelayLineFourTap<T, kTankLineLen> staticDelayLine[4] = {};
    StaticDelayLineEightTap<T, kEarlyLen> earlyReflectionsDelayLine[2] = {};
    T SampleRate = {};
    T DampingFreq = {};
    T Density1 = {};
    T Density2 = {};
    T BandwidthFreq = {};
    T Decay = {};
    T EarlyMix = {};
    T Size = {};

    T EarlyLateSmooth = {};
    T BandwidthSmooth = {};
    T DampingSmooth = {};
    T DensitySmooth = {};
    T DecaySmooth = {};

    T PreviousLeftTank = {};
    T PreviousRightTank = {};

    int ControlRate = 0;
    int ControlRateCounter = 0;

    /** Where the smoothers settle. ⚠️ They are STARTED here rather than left at zero: a bandwidth of
     *  0 Hz is a filter that passes nothing, so a smoother that ramped up from zero would open every
     *  reverb — and every render — with a block of silence. */
    void settleSmoothers() {
        BandwidthSmooth = static_cast<T>((BandwidthFreq * 18400.) + 100.);
        DampingSmooth   = static_cast<T>((DampingFreq * 18400.) + 100.);
        DecaySmooth     = static_cast<T>((0.7995 * Decay) + 0.005);
        DensitySmooth   = static_cast<T>((0.7995 * Density1) + 0.005);
        EarlyLateSmooth = EarlyMix;
    }

public:
    enum
    {
        DAMPINGFREQ = 0,
        DENSITY,
        BANDWIDTHFREQ,
        DECAY,
        SIZE,
        EARLYMIX,
        NUM_PARAMS
    };

    MVerb() {
        DampingFreq   = static_cast<T>(0.9);
        BandwidthFreq = static_cast<T>(0.9);
        SampleRate    = static_cast<T>(44100.);
        Decay         = static_cast<T>(0.5);
        Density1      = static_cast<T>(0.5);
        Size          = static_cast<T>(1.);
        EarlyMix      = static_cast<T>(1.);
        ControlRate   = static_cast<int>(SampleRate / 1000);
        reset();
    }

    // ⚠️ **`StateVariable` HOLDS A POINTER INTO ITSELF** (`out = &low`), so a copy of this object
    // leaves the copy's filters reading the ORIGINAL's state. Nothing needs to copy it — it is a
    // member of an engine that is heap-allocated once — so the copy is deleted rather than fixed,
    // and a future caller finds out at compile time instead of hearing it.
    MVerb(const MVerb&)            = delete;
    MVerb& operator=(const MVerb&) = delete;

    /** The wet reverb, written to `outL`/`outR`. The dry signal never reaches the output. */
    void process(const T* inL, const T* inR, T* outL, T* outR, int sampleFrames) {
        if (sampleFrames <= 0) return;
        T OneOverSampleFrames = static_cast<T>(1. / sampleFrames);
        T EarlyLateDelta = (EarlyMix - EarlyLateSmooth) * OneOverSampleFrames;
        T BandwidthDelta = static_cast<T>((((BandwidthFreq * 18400.) + 100.) - BandwidthSmooth) * OneOverSampleFrames);
        T DampingDelta   = static_cast<T>((((DampingFreq * 18400.) + 100.) - DampingSmooth) * OneOverSampleFrames);
        T DecayDelta     = static_cast<T>((((0.7995 * Decay) + 0.005) - DecaySmooth) * OneOverSampleFrames);
        T DensityDelta   = static_cast<T>((((0.7995 * Density1) + 0.005) - DensitySmooth) * OneOverSampleFrames);
        for (int i = 0; i < sampleFrames; ++i) {
            T left  = inL[i];
            T right = inR[i];
            EarlyLateSmooth += EarlyLateDelta;
            BandwidthSmooth += BandwidthDelta;
            DampingSmooth += DampingDelta;
            DecaySmooth += DecayDelta;
            DensitySmooth += DensityDelta;
            if (ControlRateCounter >= ControlRate) {
                ControlRateCounter = 0;
                bandwidthFilter[0].Frequency(BandwidthSmooth);
                bandwidthFilter[1].Frequency(BandwidthSmooth);
                damping[0].Frequency(DampingSmooth);
                damping[1].Frequency(DampingSmooth);
            }
            ++ControlRateCounter;
            Density2 = static_cast<T>(DecaySmooth + 0.15);
            if (Density2 > 0.5) Density2 = static_cast<T>(0.5);
            if (Density2 < 0.25) Density2 = static_cast<T>(0.25);
            allpassFourTap[1].SetFeedback(Density2);
            allpassFourTap[3].SetFeedback(Density2);
            allpassFourTap[0].SetFeedback(DensitySmooth);
            allpassFourTap[2].SetFeedback(DensitySmooth);
            T bandwidthLeft  = bandwidthFilter[0](left);
            T bandwidthRight = bandwidthFilter[1](right);
            T earlyReflectionsL = static_cast<T>(earlyReflectionsDelayLine[0](bandwidthLeft * 0.5 + bandwidthRight * 0.3)
                                + earlyReflectionsDelayLine[0].GetIndex(2) * 0.6
                                + earlyReflectionsDelayLine[0].GetIndex(3) * 0.4
                                + earlyReflectionsDelayLine[0].GetIndex(4) * 0.3
                                + earlyReflectionsDelayLine[0].GetIndex(5) * 0.3
                                + earlyReflectionsDelayLine[0].GetIndex(6) * 0.1
                                + earlyReflectionsDelayLine[0].GetIndex(7) * 0.1
                                + (bandwidthLeft * 0.4 + bandwidthRight * 0.2) * 0.5);
            T earlyReflectionsR = static_cast<T>(earlyReflectionsDelayLine[1](bandwidthLeft * 0.3 + bandwidthRight * 0.5)
                                + earlyReflectionsDelayLine[1].GetIndex(2) * 0.6
                                + earlyReflectionsDelayLine[1].GetIndex(3) * 0.4
                                + earlyReflectionsDelayLine[1].GetIndex(4) * 0.3
                                + earlyReflectionsDelayLine[1].GetIndex(5) * 0.3
                                + earlyReflectionsDelayLine[1].GetIndex(6) * 0.1
                                + earlyReflectionsDelayLine[1].GetIndex(7) * 0.1
                                + (bandwidthLeft * 0.2 + bandwidthRight * 0.4) * 0.5);
            T smearedInput = static_cast<T>((bandwidthRight + bandwidthLeft) * 0.5);
            for (int j = 0; j < 4; j++) smearedInput = allpass[j](smearedInput);
            T leftTank = allpassFourTap[0](smearedInput + PreviousRightTank);
            leftTank   = staticDelayLine[0](leftTank);
            leftTank   = damping[0](leftTank);
            leftTank   = allpassFourTap[1](leftTank);
            leftTank   = staticDelayLine[1](leftTank);
            T rightTank = allpassFourTap[2](smearedInput + PreviousLeftTank);
            rightTank   = staticDelayLine[2](rightTank);
            rightTank   = damping[1](rightTank);
            rightTank   = allpassFourTap[3](rightTank);
            rightTank   = staticDelayLine[3](rightTank);
            PreviousLeftTank  = leftTank * DecaySmooth;
            PreviousRightTank = rightTank * DecaySmooth;
            T accumulatorL = static_cast<T>((0.6 * staticDelayLine[2].GetIndex(1))
                            + (0.6 * staticDelayLine[2].GetIndex(2))
                            - (0.6 * allpassFourTap[3].GetIndex(1))
                            + (0.6 * staticDelayLine[3].GetIndex(1))
                            - (0.6 * staticDelayLine[0].GetIndex(1))
                            - (0.6 * allpassFourTap[1].GetIndex(1))
                            - (0.6 * staticDelayLine[1].GetIndex(1)));
            T accumulatorR = static_cast<T>((0.6 * staticDelayLine[0].GetIndex(2))
                            + (0.6 * staticDelayLine[0].GetIndex(3))
                            - (0.6 * allpassFourTap[1].GetIndex(2))
                            + (0.6 * staticDelayLine[1].GetIndex(2))
                            - (0.6 * staticDelayLine[2].GetIndex(3))
                            - (0.6 * allpassFourTap[3].GetIndex(2))
                            - (0.6 * staticDelayLine[3].GetIndex(2)));
            // ⚠️ EARLYMIX IS THE LATE TAIL'S SHARE, NOT THE EARLY BANK'S: 1 is all tank, 0 is all
            // early reflections. Upstream's name reads the other way round, and the polarity is the
            // one thing a caller gets wrong.
            outL[i] = (accumulatorL * EarlyLateSmooth) + ((1 - EarlyLateSmooth) * earlyReflectionsL);
            outR[i] = (accumulatorR * EarlyLateSmooth) + ((1 - EarlyLateSmooth) * earlyReflectionsR);
        }
    }

    void reset() {
        ControlRateCounter = 0;
        PreviousLeftTank = PreviousRightTank = 0;
        settleSmoothers();
        bandwidthFilter[0].SetSampleRate(SampleRate);
        bandwidthFilter[1].SetSampleRate(SampleRate);
        bandwidthFilter[0].Reset();
        bandwidthFilter[1].Reset();
        bandwidthFilter[0].Frequency(BandwidthSmooth);
        bandwidthFilter[1].Frequency(BandwidthSmooth);
        damping[0].SetSampleRate(SampleRate);
        damping[1].SetSampleRate(SampleRate);
        damping[0].Reset();
        damping[1].Reset();
        damping[0].Frequency(DampingSmooth);
        damping[1].Frequency(DampingSmooth);
        allpass[0].Clear();
        allpass[1].Clear();
        allpass[2].Clear();
        allpass[3].Clear();
        allpass[0].SetLength(static_cast<int>(0.0048 * SampleRate));
        allpass[1].SetLength(static_cast<int>(0.0036 * SampleRate));
        allpass[2].SetLength(static_cast<int>(0.0127 * SampleRate));
        allpass[3].SetLength(static_cast<int>(0.0093 * SampleRate));
        allpass[0].SetFeedback(static_cast<T>(0.75));
        allpass[1].SetFeedback(static_cast<T>(0.75));
        allpass[2].SetFeedback(static_cast<T>(0.625));
        allpass[3].SetFeedback(static_cast<T>(0.625));
        allpassFourTap[0].Clear();
        allpassFourTap[1].Clear();
        allpassFourTap[2].Clear();
        allpassFourTap[3].Clear();
        allpassFourTap[0].SetFeedback(Density1);
        allpassFourTap[1].SetFeedback(Density2);
        allpassFourTap[2].SetFeedback(Density1);
        allpassFourTap[3].SetFeedback(Density2);
        staticDelayLine[0].Clear();
        staticDelayLine[1].Clear();
        staticDelayLine[2].Clear();
        staticDelayLine[3].Clear();
        applySize();
        earlyReflectionsDelayLine[0].Clear();
        earlyReflectionsDelayLine[1].Clear();
        earlyReflectionsDelayLine[0].SetLength(static_cast<int>(0.089 * SampleRate));
        earlyReflectionsDelayLine[0].SetIndex(0, static_cast<int>(0.0199 * SampleRate), static_cast<int>(0.0219 * SampleRate), static_cast<int>(0.0354 * SampleRate), static_cast<int>(0.0389 * SampleRate), static_cast<int>(0.0414 * SampleRate), static_cast<int>(0.0692 * SampleRate), 0);
        earlyReflectionsDelayLine[1].SetLength(static_cast<int>(0.069 * SampleRate));
        earlyReflectionsDelayLine[1].SetIndex(0, static_cast<int>(0.0099 * SampleRate), static_cast<int>(0.011 * SampleRate), static_cast<int>(0.0182 * SampleRate), static_cast<int>(0.0189 * SampleRate), static_cast<int>(0.0213 * SampleRate), static_cast<int>(0.0431 * SampleRate), 0);
    }

    /**
     * The eight tank lines' lengths and read heads, which is what SIZE *is*.
     *
     * ⚠️⚠️ **THE CALLER MUST CLEAR THEM AND MUST ONLY COME HERE WHEN SIZE ACTUALLY MOVED.** Re-lengthing
     * a line while it holds audio leaves read heads pointing into the wrong part of the ring, so a
     * SIZE turn cuts the tail dead — there is no smoothing this, the room's dimensions ARE the
     * lengths. The globals are re-pushed on every project edit, so a caller that pushed SIZE
     * unconditionally would silence the reverb every time the user typed a note.
     */
    void applySize() {
        allpassFourTap[0].SetLength(static_cast<int>(0.020 * SampleRate * Size));
        allpassFourTap[1].SetLength(static_cast<int>(0.060 * SampleRate * Size));
        allpassFourTap[2].SetLength(static_cast<int>(0.030 * SampleRate * Size));
        allpassFourTap[3].SetLength(static_cast<int>(0.089 * SampleRate * Size));
        allpassFourTap[0].SetIndex(0, 0, 0, 0);
        allpassFourTap[1].SetIndex(0, static_cast<int>(0.006 * SampleRate * Size), static_cast<int>(0.041 * SampleRate * Size), 0);
        allpassFourTap[2].SetIndex(0, 0, 0, 0);
        allpassFourTap[3].SetIndex(0, static_cast<int>(0.031 * SampleRate * Size), static_cast<int>(0.011 * SampleRate * Size), 0);
        staticDelayLine[0].SetLength(static_cast<int>(0.15 * SampleRate * Size));
        staticDelayLine[1].SetLength(static_cast<int>(0.12 * SampleRate * Size));
        staticDelayLine[2].SetLength(static_cast<int>(0.14 * SampleRate * Size));
        staticDelayLine[3].SetLength(static_cast<int>(0.11 * SampleRate * Size));
        staticDelayLine[0].SetIndex(0, static_cast<int>(0.067 * SampleRate * Size), static_cast<int>(0.011 * SampleRate * Size), static_cast<int>(0.121 * SampleRate * Size));
        staticDelayLine[1].SetIndex(0, static_cast<int>(0.036 * SampleRate * Size), static_cast<int>(0.089 * SampleRate * Size), 0);
        staticDelayLine[2].SetIndex(0, static_cast<int>(0.0089 * SampleRate * Size), static_cast<int>(0.099 * SampleRate * Size), 0);
        staticDelayLine[3].SetIndex(0, static_cast<int>(0.067 * SampleRate * Size), static_cast<int>(0.0041 * SampleRate * Size), 0);
    }

    void setParameter(int index, T value) {
        switch (index) {
            case DAMPINGFREQ: DampingFreq = static_cast<T>(1. - value); break;
            case DENSITY: Density1 = value; break;
            case BANDWIDTHFREQ: BandwidthFreq = value; break;
            case DECAY: Decay = value; break;
            case EARLYMIX: EarlyMix = value; break;
            case SIZE:
                Size = static_cast<T>((0.95 * value) + 0.05);
                staticDelayLine[0].Clear();
                staticDelayLine[1].Clear();
                staticDelayLine[2].Clear();
                staticDelayLine[3].Clear();
                allpassFourTap[0].Clear();
                allpassFourTap[1].Clear();
                allpassFourTap[2].Clear();
                allpassFourTap[3].Clear();
                applySize();
                break;
        }
    }

    /** ⚠️ Rebuilds every line, so it clears the tail. `sr` must not exceed kMVerbDesignRate. */
    void setSampleRate(T sr) {
        SampleRate  = sr;
        ControlRate = static_cast<int>(SampleRate / 1000);
        reset();
    }
};

// ───────────────────────────────────────────────────────────────────────────
// The delay primitives. ⚠️ Every `SetIndex` below CLAMPS into the buffer, which upstream does not:
// its read heads are set from `seconds * SampleRate * Size` while the buffer is sized for the design
// rate, so one call above that rate is a read past the end rather than a shorter reverb. A clamp
// turns a memory-corruption bug into a wrong-sounding one, and the ceiling comment at the top of
// this file is what stops it being reached at all.
// ───────────────────────────────────────────────────────────────────────────

/** A read head, forced inside the buffer. */
template <int maxLength>
inline int mverb_clamp_index(int index) {
    if (index < 0) return 0;
    if (index >= maxLength) return maxLength - 1;
    return index;
}

template<typename T, int maxLength>
class Allpass
{
private:
    T buffer[maxLength] = {};
    int index = 0;
    int Length = 0;
    T Feedback = {};

public:
    Allpass() {
        SetLength(maxLength - 1);
        Clear();
        Feedback = static_cast<T>(0.5);
    }

    T operator()(T input) {
        T output;
        T bufout;
        bufout      = buffer[index];
        T temp      = input * -Feedback;
        output      = bufout + temp;
        buffer[index] = input + ((bufout + temp) * Feedback);
        if (++index >= Length) index = 0;
        return output;
    }

    void SetLength(int len) {
        if (len >= maxLength) len = maxLength;
        if (len < 0) len = 0;
        Length = len;
    }

    void SetFeedback(T feedback) { Feedback = feedback; }

    void Clear() {
        memset(buffer, 0, sizeof(buffer));
        index = 0;
    }

    int GetLength() const { return Length; }
};

template<typename T, int maxLength>
class StaticAllpassFourTap
{
private:
    T buffer[maxLength] = {};
    int index1 = 0;
    int index2 = 0;
    int index3 = 0;
    int index4 = 0;
    int Length = 0;
    T Feedback = {};

public:
    StaticAllpassFourTap() {
        SetLength(maxLength - 1);
        Clear();
        Feedback = static_cast<T>(0.5);
    }

    T operator()(T input) {
        T output;
        T bufout;
        bufout         = buffer[index1];
        T temp         = input * -Feedback;
        output         = bufout + temp;
        buffer[index1] = input + ((bufout + temp) * Feedback);
        if (++index1 >= Length) index1 = 0;
        if (++index2 >= Length) index2 = 0;
        if (++index3 >= Length) index3 = 0;
        if (++index4 >= Length) index4 = 0;
        return output;
    }

    void SetIndex(int Index1, int Index2, int Index3, int Index4) {
        index1 = mverb_clamp_index<maxLength>(Index1);
        index2 = mverb_clamp_index<maxLength>(Index2);
        index3 = mverb_clamp_index<maxLength>(Index3);
        index4 = mverb_clamp_index<maxLength>(Index4);
    }

    T GetIndex(int Index) {
        switch (Index) {
            case 1: return buffer[index2];
            case 2: return buffer[index3];
            case 3: return buffer[index4];
            default: return buffer[index1];
        }
    }

    void SetLength(int len) {
        if (len >= maxLength) len = maxLength;
        if (len < 0) len = 0;
        Length = len;
    }

    void Clear() {
        memset(buffer, 0, sizeof(buffer));
        index1 = index2 = index3 = index4 = 0;
    }

    void SetFeedback(T feedback) { Feedback = feedback; }

    int GetLength() const { return Length; }
};

template<typename T, int maxLength>
class StaticDelayLineFourTap
{
private:
    T buffer[maxLength] = {};
    int index1 = 0;
    int index2 = 0;
    int index3 = 0;
    int index4 = 0;
    int Length = 0;

public:
    StaticDelayLineFourTap() {
        SetLength(maxLength - 1);
        Clear();
    }

    T operator()(T input) {
        T output         = buffer[index1];
        buffer[index1++] = input;
        if (index1 >= Length) index1 = 0;
        if (++index2 >= Length) index2 = 0;
        if (++index3 >= Length) index3 = 0;
        if (++index4 >= Length) index4 = 0;
        return output;
    }

    void SetIndex(int Index1, int Index2, int Index3, int Index4) {
        index1 = mverb_clamp_index<maxLength>(Index1);
        index2 = mverb_clamp_index<maxLength>(Index2);
        index3 = mverb_clamp_index<maxLength>(Index3);
        index4 = mverb_clamp_index<maxLength>(Index4);
    }

    T GetIndex(int Index) {
        switch (Index) {
            case 1: return buffer[index2];
            case 2: return buffer[index3];
            case 3: return buffer[index4];
            default: return buffer[index1];
        }
    }

    void SetLength(int len) {
        if (len >= maxLength) len = maxLength;
        if (len < 0) len = 0;
        Length = len;
    }

    void Clear() {
        memset(buffer, 0, sizeof(buffer));
        index1 = index2 = index3 = index4 = 0;
    }

    int GetLength() const { return Length; }
};

template<typename T, int maxLength>
class StaticDelayLineEightTap
{
private:
    T buffer[maxLength] = {};
    int index1 = 0;
    int index2 = 0;
    int index3 = 0;
    int index4 = 0;
    int index5 = 0;
    int index6 = 0;
    int index7 = 0;
    int index8 = 0;
    int Length = 0;

public:
    StaticDelayLineEightTap() {
        SetLength(maxLength - 1);
        Clear();
    }

    T operator()(T input) {
        T output         = buffer[index1];
        buffer[index1++] = input;
        if (index1 >= Length) index1 = 0;
        if (++index2 >= Length) index2 = 0;
        if (++index3 >= Length) index3 = 0;
        if (++index4 >= Length) index4 = 0;
        if (++index5 >= Length) index5 = 0;
        if (++index6 >= Length) index6 = 0;
        if (++index7 >= Length) index7 = 0;
        if (++index8 >= Length) index8 = 0;
        return output;
    }

    void SetIndex(int Index1, int Index2, int Index3, int Index4, int Index5, int Index6, int Index7, int Index8) {
        index1 = mverb_clamp_index<maxLength>(Index1);
        index2 = mverb_clamp_index<maxLength>(Index2);
        index3 = mverb_clamp_index<maxLength>(Index3);
        index4 = mverb_clamp_index<maxLength>(Index4);
        index5 = mverb_clamp_index<maxLength>(Index5);
        index6 = mverb_clamp_index<maxLength>(Index6);
        index7 = mverb_clamp_index<maxLength>(Index7);
        index8 = mverb_clamp_index<maxLength>(Index8);
    }

    T GetIndex(int Index) {
        switch (Index) {
            case 1: return buffer[index2];
            case 2: return buffer[index3];
            case 3: return buffer[index4];
            case 4: return buffer[index5];
            case 5: return buffer[index6];
            case 6: return buffer[index7];
            case 7: return buffer[index8];
            default: return buffer[index1];
        }
    }

    void SetLength(int len) {
        if (len >= maxLength) len = maxLength;
        if (len < 0) len = 0;
        Length = len;
    }

    void Clear() {
        memset(buffer, 0, sizeof(buffer));
        index1 = index2 = index3 = index4 = index5 = index6 = index7 = index8 = 0;
    }

    int GetLength() const { return Length; }
};

/**
 * A 4× oversampled Chamberlin state-variable filter, low-pass only here.
 *
 * ⚠️ **`out` IS A POINTER INTO THIS OBJECT**, which is why MVerb's copy constructor is deleted: a
 * copied filter reads the original's state and follows it forever.
 */
template<typename T, int OverSampleCount>
class StateVariable
{
public:
    enum FilterType { LOWPASS, HIGHPASS, BANDPASS, NOTCH, FilterTypeCount };

private:
    T sampleRate = {};
    T frequency = {};
    T q = {};
    T f = {};

    T low = {};
    T high = {};
    T band = {};
    T notch = {};

    T* out = nullptr;

public:
    StateVariable() {
        SetSampleRate(static_cast<T>(44100.));
        Frequency(static_cast<T>(1000.));
        Resonance(0);
        Type(LOWPASS);
        Reset();
    }

    T operator()(T input) {
        for (int i = 0; i < OverSampleCount; i++) {
            low += static_cast<T>(f * band + 1e-25);
            high = input - low - q * band;
            band += f * high;
            notch = low + high;
        }
        return *out;
    }

    void Reset() { low = high = band = notch = 0; }

    void SetSampleRate(T sr) {
        this->sampleRate = sr * OverSampleCount;
        UpdateCoefficient();
    }

    void Frequency(T freq) {
        this->frequency = freq;
        UpdateCoefficient();
    }

    void Resonance(T resonance) { this->q = 2 - 2 * resonance; }

    void Type(int type) {
        switch (type) {
            case HIGHPASS: out = &high; break;
            case BANDPASS: out = &band; break;
            case NOTCH: out = &notch; break;
            default: out = &low; break;
        }
    }

private:
    void UpdateCoefficient() {
        f = static_cast<T>(2. * sin(3.141592654 * frequency / sampleRate));
    }
};

}  // namespace mverb
