#pragma once

#include "effects/glide.h"   // every fade length

#define LOG_TAG "NativeAudio"

// Logging: Android's log on Android, stderr elsewhere.
#ifdef __ANDROID__
#  include <android/log.h>
#  define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#  define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#  include <cstdio>
#  include <cstdlib>

// Off Android LOGD is opt-in (POCKETTRACKER_LOG set and not "0") — there is no logcat to filter it.
// LOGE always prints. Read once before main: getenv is not real-time safe.
namespace ptlog {
inline const bool debug_enabled = [] {
    const char* v = std::getenv("POCKETTRACKER_LOG");
    return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}();
}  // namespace ptlog

#  define LOGD(...)                                            \
      do {                                                     \
          if (::ptlog::debug_enabled) {                        \
              fprintf(stderr, "[D/" LOG_TAG "] ");             \
              fprintf(stderr, __VA_ARGS__);                    \
              fputc('\n', stderr);                             \
          }                                                    \
      } while (0)
#  define LOGE(...) do { fprintf(stderr, "[E/" LOG_TAG "] "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

// LOGT: per-note / per-row tracing for hot paths; set to 1 when debugging.
#define AUDIO_TRACE 0
#if AUDIO_TRACE
#  define LOGT(...) LOGD(__VA_ARGS__)
#else
#  define LOGT(...)
#endif

// One voice per track. Nine lanes (8 = preview) but eight voices: a preview steals a fading one.
const int MAX_VOICES = 8;

// ⚠️ Must match songcore/effects.h, where the codes are defined. Only what the table engine runs.
const int FX_CHA    = 0x04;  // CHA xy - chance: x for its nearest filled neighbour on the LEFT, y on the RIGHT
const int FX_RNL    = 0x11;  // RNL xx - add a random 0..xx to the value in the slot to its LEFT
const int FX_RND    = 0x10;  // RND xx - re-fire the last command in this column with 0..xx added
const int FX_PAN    = 0x1F;  // PAN xx - the voice's pan: 00 left, 80 centre, FF right
const int FX_HOP    = 0x08;  // Hxx - Table hop (repeat-count jump, FF = stop table)
const int FX_TIC    = 0x09;  // Txx - Table tick rate (01-FB = tics/row, FC-FF = special modes)
const int FX_KILL   = 0x0B;  // K00 - Kill voice
const int FX_OFFSET = 0x0F;  // Oxx - Sample offset
const int FX_THO    = 0x15;  // THO 0X - Table hop to row X (simple unconditional jump)
const int FX_VOLUME = 0x16;  // Vxx - Volume
const int FX_EQN    = 0x23;  // EQN xx - per-voice EQ preset slot (00-7F)
const int FX_EQM    = 0x24;  // EQM xx - master/mixer EQ preset slot (00-7F)
const int FX_CUT    = 0x2F;  // CUT xx - filter cutoff   (inert when the instrument runs no filter)
const int FX_RES    = 0x30;  // RES xx - filter resonance (likewise)
// The three that switch one ON at a cutoff, so the two above are never inert after one of them.
const int FX_LPF    = 0x33;  // LPF xx - low-pass  on, cutoff xx
const int FX_HPF    = 0x34;  // HPF xx - high-pass on, cutoff xx
const int FX_BPF    = 0x35;  // BPF xx - band-pass on, cutoff xx
const int FX_DRV    = 0x37;  // DRV xx - overdrive amount
const int FX_CRU    = 0x38;  // CRU xy - x = bits crushed, y = downsample; two 4-bit values in one cell
const int FX_FIN    = 0x39;  // FIN xx - fine tune; 80 = in tune, one semitone either way
const int FX_INS    = 0x3C;  // INS xx - play this hit on instrument xx; on a table row it is a SWITCH
const int FX_TIM    = 0x3D;  // TIM xx - the delay's echo time, free scale (00-FF = 0-2 s); GLOBAL
const int FX_CHD    = 0x3F;  // CHD xy - the note as a chord; read only in a column that steps per note
const int FX_ARC    = 0x03;  // ARC xy - right of a CHD, strums it: x the order, y tics between notes
const int FX_ARPEGGIO = 0x0A;  // ARP - not run in a table; it takes an ARC to its right off a CHD

// The most a random roll may push a table command to — songcore's effect_value_max, pinned in
// engine_consumer.h.
inline constexpr int tableFxCeiling(int fxType) {
    return (fxType == FX_EQN || fxType == FX_EQM || fxType == FX_INS) ? 127 : 255;
}
// ─── The loop modes, the engine's own numbers (the file stores names) ────────────────────────────
// ⚠️ OSCILLATOR walks like FORWARD with a retuned rate: ask `isForwardLoopMode`, never
// `== LOOP_MODE_FORWARD`, or an oscillator plays once and stops.
const int LOOP_MODE_OFF        = 0;
const int LOOP_MODE_FORWARD    = 1;
const int LOOP_MODE_PINGPONG   = 2;
const int LOOP_MODE_OSCILLATOR = 3;

inline constexpr bool isForwardLoopMode(int mode) {
    return mode == LOOP_MODE_FORWARD || mode == LOOP_MODE_OSCILLATOR;
}

// No 0x3A (TSX): resolved before scheduling, the engine never sees it.
const int FX_LPO    = 0x3B;  // LPO xx - slide the loop window, signed sixteenths of its own length

// LPO's byte as signed sixteenths — songcore decodes it the same way.
inline constexpr int loopSlideSixteenthsOf(int value) {
    return (value & 0xFF) < 0x80 ? (value & 0xFF) : (value & 0xFF) - 256;
}

// FIN's byte as semitones: 80 in tune, 00 a semitone flat, 1/128 per step — meeting PIT with no gap.
inline constexpr float fineTuneSemitonesOf(int value) { return (value - 128) / 128.0f; }

// Nothing else would catch a centre one step off (0.78 cents) or a wrong divisor.
static_assert(fineTuneSemitonesOf(0x80) == 0.0f, "FIN's centre byte must be exactly in tune");
static_assert(fineTuneSemitonesOf(0x00) == -1.0f, "FIN's low end must be exactly a semitone flat");

// The two halves of a CRU byte; engine_consumer.h asserts they match songcore's.
inline constexpr int crushBitsOf(int value)       { return (value >> 4) & 0x0F; }
inline constexpr int crushDownsampleOf(int value) { return value & 0x0F; }
