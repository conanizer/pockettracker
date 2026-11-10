#pragma once

// What a 00-FF cell means to the effects, in Hz and seconds: the one copy the DSP and the screens'
// value hints both read. Only <cmath>: the UI includes it.
// ⚠️ Keep each expression as it is — a reordered float changes every rendered byte.
#include <cmath>

// Filter cutoff, EQ band frequency, CUT/LPF/HPF/BPF: 20 Hz to 20 kHz, an octave about every 1A.
inline float cutoff_hz(int hex) { return 20.0f * powf(1000.0f, hex / 255.0f); }

// The delay's free TIME, and TIM: 0 to 2 seconds, even steps.
inline float delay_time_seconds(int timeHex) { return (timeHex / 255.0f) * 2.0f; }

// The delay's TONE: the repeats' low-pass, 356 Hz to 20 kHz. FF is no filter at all.
inline constexpr float kDelayToneLowHz  = 356.4f;
inline constexpr float kDelayToneHighHz = 20000.0f;
inline float delay_tone_hz(int toneHex) {
    return kDelayToneLowHz * powf(kDelayToneHighHz / kDelayToneLowHz, toneHex / 255.0f);
}
inline constexpr bool delay_tone_filters(int toneHex) { return toneHex < 0xFF; }
