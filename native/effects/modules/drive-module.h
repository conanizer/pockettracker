#pragma once
#include "../primitives/daisysp/overdrive.h"

// ===========================================================================
// DriveModule — overdrive / soft saturation via DaisySP Overdrive.
//
// DaisySP Overdrive by Emilie Gillet, ported by Ben Sergentanis (MIT).
// Uses SoftClip (Padé rational approximant) with drive-dependent gain
// staging: blends linear pre-gain (low drive) with hypercubic pre-gain
// (high drive) and compensates output volume automatically so level stays
// consistent across the drive range.
//
// param drive: 0–255 (0 = bypass, 255 = heavy overdrive)
//   Maps to SetDrive(0.30..0.60). drive=1 ≈ gentle warmth (+2 dB, light saturation).
//   DaisySP Overdrive below SetDrive(~0.3) attenuates severely: post_gain
//   compensation assumes real saturation but pre_gain is sub-unity there.
//   The 1-255 → 0.30-0.60 mapping keeps the effect in the musically usable zone.
//
// Call setDrive() once per block when the param changes (not per sample).
// Process() is a pure function — one od instance is safe for both channels.
// ===========================================================================
struct DriveModule {
    // A change on a sounding note crossfades from the old setting's output over CHANGE_FRAMES samples —
    // a jump in drive is a jump in the waveform. Anything set before the note's first sample is instant.
    static constexpr int CHANGE_FRAMES = 128;

    int drive = 0;
    daisysp::Overdrive od;
    daisysp::Overdrive odPrev;   // the setting being faded out (the shaper is stateless, so a copy is it)
    int  prevDrive = 0;
    int  fadeLeft  = 0;
    bool fresh     = true;

    void reset() {
        drive = 0;
        od.Init();
        fadeLeft = 0;
        fresh = true;
    }

    bool enabled() const { return drive > 0; }

    // Call once per block (or at trigger) when drive changes.
    void setDrive(int d) {
        if (!fresh && d != drive) {
            odPrev    = od;
            prevDrive = drive;
            fadeLeft  = CHANGE_FRAMES;
        }
        drive = d;
        od.SetDrive(0.3f + (d / 255.0f) * 0.3f);
    }

    inline float processMono(float in) {
        fresh = false;
        if (!enabled() && fadeLeft == 0) return in;
        float out = enabled() ? od.Process(in) : in;
        if (fadeLeft > 0) {
            const float k = static_cast<float>(fadeLeft) / CHANGE_FRAMES;
            out = (prevDrive > 0 ? odPrev.Process(in) : in) * k + out * (1.0f - k);
            --fadeLeft;
        }
        return out;
    }

    inline void processStereo(float& L, float& R) {
        fresh = false;
        if (!enabled() && fadeLeft == 0) return;
        float outL = enabled() ? od.Process(L) : L, outR = enabled() ? od.Process(R) : R;
        if (fadeLeft > 0) {
            const float k = static_cast<float>(fadeLeft) / CHANGE_FRAMES;
            outL = (prevDrive > 0 ? odPrev.Process(L) : L) * k + outL * (1.0f - k);
            outR = (prevDrive > 0 ? odPrev.Process(R) : R) * k + outR * (1.0f - k);
            --fadeLeft;
        }
        L = outL;
        R = outR;
    }
};
