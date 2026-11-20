#pragma once
#include <cmath>

#include "../glide.h"
#include "../primitives/daisysp/dsp.h"

// DaisySP's Overdrive curve and level compensation, with its own pre-gain: ×1 at 01 rising to ×16 at FF
// (24·d^1.6 dB), so it stays warm most of the way and turns to fuzz only near the top.
struct SoftDrive {
    float pre = 1.0f, post = 1.0f;

    void set(int drive) {
        const float d  = static_cast<float>(drive) / 255.0f;
        const float dr = 2.0f * (0.3f + 0.3f * d);   // DaisySP's drive_ at the old SetDrive(0.3..0.6)
        pre  = powf(10.0f, 1.2f * powf(d, 1.6f));
        post = 1.0f / daisysp::SoftClip(0.33f + dr * (2.0f - dr) * (pre - 0.33f));
    }
    inline float Process(float in) const { return daisysp::SoftClip(pre * in) * post; }
};

// Drive 0-255 (0 = off), level-compensated; one curve for every instrument type. Stateless, so one
// instance serves both channels.
struct DriveModule {
    // The glide rule (glide.h) as a crossfade from the old setting's output.

    int drive = 0;
    SoftDrive od;
    SoftDrive odPrev;   // the setting fading out
    int  prevDrive = 0;
    Ramp oldShare;               // the old setting's share of the output, 1 to 0
    bool fresh     = true;

    void reset() {
        drive = 0;
        od = SoftDrive{};
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
        od.set(d);
    }

    inline float shape(float in)     { return od.Process(in); }
    inline float shapePrev(float in) { return odPrev.Process(in); }

    inline float processMono(float in) {
        fresh = false;
        if (!enabled() && !oldShare.moving()) return in;
        float out = enabled() ? shape(in) : in;
        if (oldShare.moving()) {
            const float k = oldShare.next();
            out = (prevDrive > 0 ? shapePrev(in) : in) * k + out * (1.0f - k);
        }
        return out;
    }

    inline void processStereo(float& L, float& R) {
        fresh = false;
        if (!enabled() && !oldShare.moving()) return;
        float outL = enabled() ? shape(L) : L, outR = enabled() ? shape(R) : R;
        if (oldShare.moving()) {
            const float k = oldShare.next();
            outL = (prevDrive > 0 ? shapePrev(L) : L) * k + outL * (1.0f - k);
            outR = (prevDrive > 0 ? shapePrev(R) : R) * k + outR * (1.0f - k);
        }
        L = outL;
        R = outR;
    }
};
