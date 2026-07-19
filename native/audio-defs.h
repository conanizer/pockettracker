#pragma once

#define LOG_TAG "NativeAudio"

// Platform logging shim. On Android the engine logs through <android/log.h>; on any other platform
// (e.g. the planned Linux port) it falls back to stderr. Routing the macros through this header keeps
// every engine translation unit free of a hard dependency on the Android log API.
#ifdef __ANDROID__
#  include <android/log.h>
#  define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#  define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#  include <cstdio>
#  include <cstdlib>

// ⚠️ OFF ANDROID THERE IS NO LOGCAT TO FILTER THESE. On Android LOGD is a debug-priority line that a
// user never sees; here the same 35 call sites go straight to stderr — which, in a shipped desktop
// build, is the console window sitting behind the tracker. A2's first-run experience was a wall of
//
//     [D/NativeAudio] 🔊 Track 0 volume set to 1.00
//
// on every boot, with the emoji arriving as mojibake on any console that is not UTF-8 (which is
// main.cpp's own stated ASCII rule, broken by a header that predates it). The PortMaster build has
// always done this too; nobody noticed because a handheld's stderr goes nowhere anyone looks.
//
// So LOGD is OPT-IN off Android:  POCKETTRACKER_LOG=1 ./pockettracker-sdl
// Anything but unset/empty/"0" turns it on, matching how POCKETTRACKER_HOME is read.
//
// ⚠️ LOGE IS NOT GATED, deliberately. An error is not spam, and a shipped build that swallows its
// errors is worse than one that is chatty — the whole reason the console is worth keeping is that a
// user can paste it back when something breaks.
//
// ⚠️ A PLAIN BOOL READ ONCE, not getenv() per call. getenv allocates nothing but walks the
// environment and is not real-time safe, and while processAudioBlock currently contains no log call
// at all (checked), the control-path functions that do are reachable from queue drains. A C++17
// inline variable is initialised before main, so every LOGD after start-up costs one predictable
// branch on an already-hot bool — cheaper than today's unconditional fprintf, and safe if a log
// line ever does land on the audio thread.
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

// LOGT — trace-level log for hot paths (note triggers, table processing, etc.).
// Disabled by default to avoid flooding logcat during playback.
// Flip to 1 to get per-note / per-table-row tracing when debugging audio logic.
#define AUDIO_TRACE 0
#if AUDIO_TRACE
#  define LOGT(...) LOGD(__VA_ARGS__)
#else
#  define LOGT(...)
#endif

// One voice per track (8 tracks); stereo samples use one slot with sampleDataRight.
// NOTE: there are 9 *logical* lanes (tracks 0-7 + the preview lane, PREVIEW_TRACK_ID = 8) but only 8
// voices. This is intentional: a preview is a transient audition, so when all 8 song tracks are
// sounding a preview note deliberately steals/preempts a fading voice (see the Step-2/3 voice search
// in scheduleNoteBatch) rather than the pool being widened to 9. Bump this to 9 only if simultaneous
// preview-over-full-song becomes a real requirement.
const int MAX_VOICES = 8;
const int DECLICK_SAMPLES = 64;  // ~1.45ms anti-click fade at 44100Hz (note start, voice steal)
// Deliberate kills (K00 / table KIL / preview stop) fade longer: 1.45 ms from a high-amplitude
// point still lands as a small tick, ~5.8 ms is soft yet perceptually instant. Steals stay at
// DECLICK_SAMPLES — their tail is masked by the new note, and short fades keep slots turning
// over at phrase-boundary trigger bursts.
const int KILL_FADE_SAMPLES = 256;

// ===================================
// EFFECT TYPE CONSTANTS (must match EffectProcessor.kt)
// Only effects processed by the C++ table engine are listed here.
// Phrase-level effects (ARP/ARC/REP/etc.) are handled entirely in Kotlin.
// ===================================
const int FX_HOP    = 0x08;  // Hxx - Table hop (repeat-count jump, FF = stop table)
const int FX_TIC    = 0x09;  // Txx - Table tick rate (01-FB = tics/row, FC-FF = special modes)
const int FX_KILL   = 0x0B;  // K00 - Kill voice
const int FX_OFFSET = 0x0F;  // Oxx - Sample offset
const int FX_THO    = 0x15;  // THO 0X - Table hop to row X (simple unconditional jump)
const int FX_VOLUME = 0x16;  // Vxx - Volume
