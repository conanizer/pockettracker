#pragma once

#define LOG_TAG "NativeAudio"

// Platform logging shim: <android/log.h> on Android, stderr elsewhere, so no engine translation unit
// depends on the Android log API.
#ifdef __ANDROID__
#  include <android/log.h>
#  define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#  define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#  include <cstdio>
#  include <cstdlib>

// ⚠️ OFF ANDROID THERE IS NO LOGCAT TO FILTER THESE: LOGD would go straight to the console behind
// the tracker. So LOGD is OPT-IN off Android: POCKETTRACKER_LOG=1 (anything but unset/empty/"0",
// as POCKETTRACKER_HOME is read).
//
// ⚠️ LOGE IS NOT GATED: an error is not spam, and a user can paste the console back when it breaks.
//
// ⚠️ A plain bool read once (an inline variable, initialised before main), not getenv() per call:
// getenv is not real-time safe, and control-path logging is reachable from queue drains.
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

// One voice per track (8 tracks); stereo samples use one slot with sampleDataRight. There are 9
// LOGICAL lanes (tracks 0-7 + the preview lane, PREVIEW_TRACK_ID = 8) but 8 voices: a preview
// steals a fading voice when all 8 tracks sound (scheduleNoteBatch's voice search).
const int MAX_VOICES = 8;
const int DECLICK_SAMPLES = 64;  // ~1.45ms anti-click fade at 44100Hz (note start, voice steal)
// Deliberate kills (K00 / table KIL / preview stop) fade longer: 1.45 ms from a high-amplitude
// point still lands as a small tick, ~5.8 ms is soft yet perceptually instant. Steals stay at
// DECLICK_SAMPLES — their tail is masked by the new note, and short fades keep slots turning
// over at phrase-boundary trigger bursts.
const int KILL_FADE_SAMPLES = 256;
// The MUTE/SOLO gate, for the same reason and at the same length: a track cut to zero at a
// high-amplitude point is a click whatever cut it. ⚠️ It is a GATE, not a fade — it runs in both
// directions, so unmuting rides back up to the fader over the same 5.8 ms rather than snapping.
const int MUTE_GATE_SAMPLES = 256;
// A mixer fader (a hand, VTR, VMV) glides at most one full swing per this many samples, across as many
// blocks as it takes. A VTR on a note's own step does not glide at all (engine-mix.cpp).
const int FADER_GLIDE_SAMPLES = 256;

// ===================================
// EFFECT TYPE CONSTANTS — ⚠️ must match native/songcore/effects.h, which is where an effect's code is
// defined and where the UI, the file format and the scheduler all read it from. Only effects the C++
// TABLE engine processes are listed here; the phrase-level ones (ARP/ARC/REP/…) are songcore's.
// ===================================
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

// The highest value a random roll may push a table command to — songcore's effect_value_max for the
// codes a table row acts on, and pinned to it in engine_consumer.h.
inline constexpr int tableFxCeiling(int fxType) {
    return (fxType == FX_EQN || fxType == FX_EQM || fxType == FX_INS) ? 127 : 255;
}
// ─── The loop modes, as the engine numbers them ─────────────────────────────────────────────────
//
// ⚠️ **OSCILLATOR IS A FORWARD LOOP WITH ITS SCAN RATE RETUNED**, not a fourth kind of traversal:
// everything about how the position walks between the two loop points is mode 1's, and the only
// difference is the factor `getModulatedPlaybackRate` applies. So every branch that asks "is this
// looping forward?" must ask `isForwardLoopMode`, never `== LOOP_MODE_FORWARD` — a site that missed
// the new mode would play the note ONCE and stop, with no error anywhere.
//
// ⚠️ The project file stores the mode as a NAME, not as this number, so the numbering is the
// engine's alone and appending here renumbers nothing a user has saved.
const int LOOP_MODE_OFF        = 0;
const int LOOP_MODE_FORWARD    = 1;
const int LOOP_MODE_PINGPONG   = 2;
const int LOOP_MODE_OSCILLATOR = 3;

/** Does this mode loop FORWARD between the two loop points? */
inline constexpr bool isForwardLoopMode(int mode) {
    return mode == LOOP_MODE_FORWARD || mode == LOOP_MODE_OSCILLATOR;
}

// ⚠️ 0x3A (TSX) is deliberately absent: it is resolved before a note is scheduled and the engine
// never sees it, so mirroring it here would be a constant with no reader.
const int FX_LPO    = 0x3B;  // LPO xx - slide the loop window, signed sixteenths of its own length

// LPO's byte as a signed count of sixteenths, the same decode songcore does. Both sides spell it
// because the table engine reads the raw cell and the phrase path reads the resolved bundle.
inline constexpr int loopSlideSixteenthsOf(int value) {
    return (value & 0xFF) < 0x80 ? (value & 0xFF) : (value & 0xFF) - 256;
}

// FIN's byte as semitones: the cents between the whole semitones PIT steps in. 80 is unity, 00 is a
// semitone flat and FF one step short of a semitone sharp, so one step is 1/128 of a semitone
// (0.78 cents) and the two commands meet with no gap. Spelled here rather than in songcore because
// the byte reaches the seam undivided and only the engine ever turns it into a pitch.
inline constexpr float fineTuneSemitonesOf(int value) { return (value - 128) / 128.0f; }

// ⚠️ The two claims here are the ones NOTHING ELSE CAN CATCH. A centre that is one step off detunes
// every cell typed at 80 by 0.78 cents — under the ear, and under any render comparison that is not
// looking for it. The endpoint is a second, independent claim: it fails for a wrong DIVISOR, which
// the centre check passes either way.
static_assert(fineTuneSemitonesOf(0x80) == 0.0f, "FIN's centre byte must be exactly in tune");
static_assert(fineTuneSemitonesOf(0x00) == -1.0f, "FIN's low end must be exactly a semitone flat");

// The two halves of a CRU byte. Spelled here because the engine does not see songcore/effects.h,
// which spells them too; engine_consumer.h - the one file that sees both - asserts they agree over
// every byte, exactly as it does for the effect codes above.
inline constexpr int crushBitsOf(int value)       { return (value >> 4) & 0x0F; }
inline constexpr int crushDownsampleOf(int value) { return value & 0x0F; }
