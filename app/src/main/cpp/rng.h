#pragma once
#include <cstdint>

// Tiny lock-free PRNG for audio-thread use. libc rand() is cheap on bionic but takes a
// process-global lock on glibc (the Linux port's default) — a priority-inversion hazard
// on a real-time thread. State must never be 0 (xorshift's fixed point).
inline uint32_t xorshift32(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// Uniform float in [0, 1].
inline float xorshift32Unit(uint32_t& state) {
    return (float)(xorshift32(state) & 0xFFFFFF) / 16777215.0f;
}

// Uniform float in [-1, +1].
inline float xorshift32Bipolar(uint32_t& state) {
    return xorshift32Unit(state) * 2.0f - 1.0f;
}
