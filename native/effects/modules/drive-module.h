#pragma once
#include "../primitives/daisysp/overdrive.h"

// Overdrive (DaisySP), level-compensated. drive 0-255 (0 = off) → SetDrive 0.30..0.60 — below 0.3
// the DaisySP stage only attenuates. Stateless, so one instance serves both channels.
struct DriveModule {
    // A change on a sounding note crossfades over CHANGE_FRAMES; before its first sample it is instant.
    static constexpr int CHANGE_FRAMES = 128;

    int drive = 0;
    daisysp::Overdrive od;
    daisysp::Overdrive odPrev;   // the setting fading out
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
