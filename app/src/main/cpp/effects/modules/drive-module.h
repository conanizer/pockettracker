#pragma once
#include <cmath>

// ===========================================================================
// DriveModule — tanh soft clipper with pre-gain boost.
// Stateless: no history, no primitives. Pure sample transform.
//
// param drive: 0–255 (0 = bypass, 255 = maximum saturation)
// ===========================================================================
struct DriveModule {
    int drive = 0;   // 0 = bypass

    void reset() { drive = 0; }

    bool enabled() const { return drive > 0; }

    inline float processMono(float in) const {
        if (!enabled()) return in;
        return tanhf(in * (drive / 128.0f));
    }

    inline void processStereo(float& L, float& R) const {
        if (!enabled()) return;
        float gain = drive / 128.0f;
        L = tanhf(L * gain);
        R = tanhf(R * gain);
    }
};
