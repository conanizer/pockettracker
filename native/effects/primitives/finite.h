#pragma once
#include <cstdint>
#include <cstring>

// Copy, with any sample that is not a finite number replaced by silence: one NaN let into a feedback
// line or a peak follower stays there until the next reset. `dst` may be `src`.
// ⚠️ By the bits, not `std::isfinite`: under -ffast-math the compiler may fold that test to "true".
inline void copy_finite(float* dst, const float* src, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t bits;
        std::memcpy(&bits, src + i, sizeof bits);
        if ((bits & 0x7f800000u) == 0x7f800000u) bits = 0;
        std::memcpy(dst + i, &bits, sizeof bits);
    }
}
