#pragma once
#include <cmath>

// lfoShape(phase, shape) — maps LFO phase to output value.
//   phase : 0.0 to 2π
//   shape : 0=TRI  1=SIN  2=RMP+  3=RMP-  6=SQU+  7=SQU-
//           (EXP+/EXP-/RND/DRUNK not yet implemented — fall back to SIN)
// Returns −1.0 to +1.0 for bipolar shapes (SIN, TRI, RMP, SQU).
// TRI ranges −1..+1 across the full cycle matching the switch block in
// updateVoiceModulation before the Phase 1 split.
inline float lfoShape(float phase, int shape) {
    float norm = phase / (2.0f * (float)M_PI);
    switch (shape) {
        case 0: // TRI
            if      (norm < 0.25f) return norm * 4.0f;
            else if (norm < 0.75f) return 1.0f - (norm - 0.25f) * 4.0f;
            else                   return (norm - 1.0f) * 4.0f;
        case 1: // SIN
            return sinf(phase);
        case 2: // RMP+ (sawtooth rising)
            return norm * 2.0f - 1.0f;
        case 3: // RMP- (sawtooth falling)
            return 1.0f - norm * 2.0f;
        case 6: // SQU+
            return (norm < 0.5f) ? 1.0f : -1.0f;
        case 7: // SQU-
            return (norm < 0.5f) ? -1.0f : 1.0f;
        default: // EXP+/EXP-/RND/DRUNK — not yet implemented
            return sinf(phase);
    }
}
