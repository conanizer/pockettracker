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
#  define LOGD(...) do { fprintf(stderr, "[D/" LOG_TAG "] "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
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

const int MAX_VOICES = 8;  // One voice per track (8 tracks); stereo samples use one slot with sampleDataRight
const int DECLICK_SAMPLES = 64;  // ~1.45ms anti-click fade at 44100Hz

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
