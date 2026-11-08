#pragma once

#include "effects/glide.h"   // every fade length
#include "songcore/effect_codes.h"

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

// The most a random roll may push a table command to — songcore's effect_value_max, pinned in
// engine_consumer.h.
inline constexpr int tableFxCeiling(int fxType) {
    return (fxType == songcore::FX_EQN || fxType == songcore::FX_EQM || fxType == songcore::FX_INS) ? 127 : 255;
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

