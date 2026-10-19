#pragma once
#include "../glide.h"
#include "../primitives/daisysp/overdrive.h"

// Overdrive (DaisySP), level-compensated. drive 0-255 (0 = off) → SetDrive 0.30..0.60 — below 0.3
// the DaisySP stage only attenuates. Stateless, so one instance serves both channels.
struct DriveModule {
    // The glide rule (glide.h) as a crossfade from the old setting's output.

    int drive = 0;
    daisysp::Overdrive od;
    daisysp::Overdrive odPrev;   // the setting fading out
    int  prevDrive = 0;
    Ramp oldShare;               // the old setting's share of the output, 1 to 0
    bool fresh     = true;

    void reset() {
        drive = 0;
        od.Init();
        oldShare = Ramp{};
        fresh = true;
    }

    bool enabled() const { return drive > 0; }

    void setDrive(int d) {
        if (!fresh && d != drive) {
            odPrev    = od;
            prevDrive = drive;
            oldShare.start(1.0f, 0.0f, GLIDE_SAMPLES);
        }
        drive = d;
        od.SetDrive(0.3f + (d / 255.0f) * 0.3f);
    }

    inline float processMono(float in) {
        fresh = false;
        if (!enabled() && !oldShare.moving()) return in;
        float out = enabled() ? od.Process(in) : in;
        if (oldShare.moving()) {
            const float k = oldShare.next();
            out = (prevDrive > 0 ? odPrev.Process(in) : in) * k + out * (1.0f - k);
        }
        return out;
    }

    inline void processStereo(float& L, float& R) {
        fresh = false;
        if (!enabled() && !oldShare.moving()) return;
        float outL = enabled() ? od.Process(L) : L, outR = enabled() ? od.Process(R) : R;
        if (oldShare.moving()) {
            const float k = oldShare.next();
            outL = (prevDrive > 0 ? odPrev.Process(L) : L) * k + outL * (1.0f - k);
            outR = (prevDrive > 0 ? odPrev.Process(R) : R) * k + outR * (1.0f - k);
        }
        L = outL;
        R = outR;
    }
};
