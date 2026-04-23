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
// param drive: 0–255 (0 = bypass, 255 = maximum saturation)
//   Maps to SetDrive(0..1). drive=128 ≈ Init default behaviour.
//
// Call setDrive() once per block when the param changes (not per sample).
// Process() is a pure function — one od instance is safe for both channels.
// ===========================================================================
struct DriveModule {
    int drive = 0;
    daisysp::Overdrive od;

    void reset() {
        drive = 0;
        od.Init();
    }

    bool enabled() const { return drive > 0; }

    // Call once per block (or at trigger) when drive changes.
    void setDrive(int d) {
        drive = d;
        od.SetDrive(d / 255.0f);
    }

    inline float processMono(float in) {
        if (!enabled()) return in;
        return od.Process(in);
    }

    inline void processStereo(float& L, float& R) {
        if (!enabled()) return;
        L = od.Process(L);
        R = od.Process(R);
    }
};
