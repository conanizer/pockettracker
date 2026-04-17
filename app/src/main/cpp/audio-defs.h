#pragma once
#include <android/log.h>

#define LOG_TAG "NativeAudio"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// LOGT — trace-level log for hot paths (note triggers, table processing, etc.).
// Disabled by default to avoid flooding logcat during playback.
// Flip to 1 to get per-note / per-table-row tracing when debugging audio logic.
#define AUDIO_TRACE 0
#if AUDIO_TRACE
#  define LOGT(...) LOGD(__VA_ARGS__)
#else
#  define LOGT(...)
#endif

const int MAX_VOICES = 8;  // Reduced for testing
const int DECLICK_SAMPLES = 64;  // ~1.45ms anti-click fade at 44100Hz

// ===================================
// EFFECT TYPE CONSTANTS (must match EffectProcessor.kt)
// ===================================
const int FX_NONE = 0x00;
const int FX_ARC = 0x03;       // Cxx - Arpeggio Config
const int FX_HOP = 0x08;       // Hxx - Table hop (repeat-count jump, FF = stop table)
const int FX_TIC = 0x09;       // Txx - Table tick rate (01-FB = tics/row, FC-FF = special modes)
const int FX_ARPEGGIO = 0x0A;  // Axx - Arpeggio
const int FX_KILL = 0x0B;      // K00 - Kill voice
const int FX_OFFSET = 0x0F;    // Oxx - Sample offset
const int FX_REPEAT = 0x12;    // Rxx - Retrigger
const int FX_THO = 0x15;       // THO 0X - Table hop to row X (simple unconditional jump)
const int FX_VOLUME = 0x16;    // Vxx - Volume
