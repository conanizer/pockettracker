#pragma once
#include <cmath>

// An LFO shape at `phase` (0..2π), −1..+1. Shape 0 TRI, 1 SIN, 2 RMP+, 3 RMP-, 4 EXP+, 5 EXP-,
// 6 SQU+, 7 SQU-. The stateful 8 RND and 9 DRNK live in tickLFO.
inline float lfoShape(float phase, int shape) {
    float norm = phase / (2.0f * (float)M_PI);
    switch (shape) {
        case 0: // TRI
            if      (norm < 0.25f) return norm * 4.0f;
            else if (norm < 0.75f) return 1.0f - (norm - 0.25f) * 4.0f;
            else                   return (norm - 1.0f) * 4.0f;
        case 1: // SIN
            return sinf(phase);
        case 2: // RMP+
            return norm * 2.0f - 1.0f;
        case 3: // RMP-
            return 1.0f - norm * 2.0f;
        case 4: // EXP+
            return norm * norm * 2.0f - 1.0f;
        case 5: // EXP-
            return (1.0f - norm) * (1.0f - norm) * 2.0f - 1.0f;
        case 6: // SQU+
            return (norm < 0.5f) ? 1.0f : -1.0f;
        case 7: // SQU-
            return (norm < 0.5f) ? -1.0f : 1.0f;
        default:
            return sinf(phase);
    }
}
