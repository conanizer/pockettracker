#pragma once
#include <cstring>
#include "note-queue.h"

// ===================================
// PARAM BUS — unified parameter + modulation accumulator (UAA Phase 2a)
// ===================================
// Every continuously-controllable value is a slot in the bus.
// Each slot has a base value (set at trigger or by effect setters) and a mod
// accumulator (reset to 0 each audio block, written by all modulation sources).
// Final value = base + mod. Clamping is done at the read site.
//
// This decouples modulation sources (envelopes, LFO, tables, effect columns) from
// voice types. Sources write addMod(); voices read get() in render() and translate
// to their own API. No branching on instrument type anywhere in effect code.
//
// NOTE: VOL modulation is intentionally NOT accumulated here — it's applied
// per-sample in the mix loop with envelope interpolation for click-free fades.
// PARAM_VOL base is set for future SoundfontVoice block-rate use.

enum ParamId {
    PARAM_VOL        = 0,  // Volume: 0.0–1.0 (base only in Phase 2a; mod done per-sample)
    PARAM_PAN        = 1,  // Pan: 0.0=left, 0.5=center, 1.0=right
    PARAM_PITCH      = 2,  // Pitch offset in semitones from envelope/LFO mods (not PSL/PBN state)
    PARAM_FILTER_CUT = 3,  // Filter cutoff: 0–255 param units
    PARAM_FILTER_RES = 4,  // Filter resonance: 0–255 param units
    PARAM_COUNT      = 5
};

struct ParamBus {
    float base[PARAM_COUNT];  // User-set values; written at trigger or by effect setters
    float mod[PARAM_COUNT];   // Frame accumulator — reset each block, written by modulation sources

    ParamBus() {
        base[PARAM_VOL]        = 1.0f;
        base[PARAM_PAN]        = 0.5f;
        base[PARAM_PITCH]      = 0.0f;
        base[PARAM_FILTER_CUT] = 128.0f;
        base[PARAM_FILTER_RES] = 0.0f;
        memset(mod, 0, sizeof(mod));
    }

    void resetMods()                      { memset(mod, 0, sizeof(mod)); }
    void addMod(ParamId id, float delta)  { mod[id] += delta; }
    void setBase(ParamId id, float value) { base[id] = value; }
    float get(ParamId id)   const         { return base[id] + mod[id]; }
};

// ===================================
// MODULE SYSTEM — Source/Destination arrays (Phase 1)
// ===================================
// Two-array pattern (Polyhedrus / Surge XT):
//   modSourceValues[] — every modulation source writes its current value here once per block.
//   modDestValues[]   — processRoutes() accumulates weighted source values into destinations.
//   Sound modules read only from modDestValues[]. Sources never know what they're modulating.
//
// Adding a new source: add one enum entry, write to modSourceValues[]. Nothing else changes.
// Adding a new destination: add one enum entry to ParamId, read from modDestValues[].
// The processRoutes loop below never changes.

enum ModSourceId {
    MOD_SRC_NONE = 0,  // Always 0.0f — used as via=NONE placeholder

    // Per-voice dynamic sources (state machine advances each audio block)
    MOD_SRC_ENV0, MOD_SRC_ENV1, MOD_SRC_ENV2, MOD_SRC_ENV3,  // AHD / ADSR / DRUM / TRIG types
    MOD_SRC_LFO0, MOD_SRC_LFO1, MOD_SRC_LFO2, MOD_SRC_LFO3,  // LFO type

    // Per-note static sources (captured at note-on, constant for the note's lifetime)
    MOD_SRC_VELOCITY,  // 0.0–1.0  (note volume proxies velocity)
    MOD_SRC_KEYTRACK,  // (midiNote − 60) / 12.0, bipolar
    MOD_SRC_RANDOM,    // random 0.0–1.0 sampled at note-on

    // Sequencer-driven sources — written each block by table/pitch state machines (Phase 2)
    MOD_SRC_TABLE_VOL,    // 0.0–1.0 from table row Vxx column
    MOD_SRC_TABLE_PITCH,  // semitones from table row transpose column
    MOD_SRC_PITCH_SLIDE,  // semitones from PSL / PBN state machine
    MOD_SRC_VIBRATO,      // −1..+1 sine from PVB / PVX state machine

    MOD_SRC_COUNT  // = 16
};

// ModRoute — a single weighted connection from one source to one destination.
//   depth     : signed scale applied to the source value (±1.0 typical).
//   via       : optional secondary modulator; MOD_SRC_NONE = unused.
//   viaAmount : 0.0 = ignore via completely; 1.0 = via fully controls effective depth.
// Formula (Polyhedrus ApplyRoute):
//   val = source * ((1 − viaAmount) + via * viaAmount)
//   modDestValues[dest] += val * depth
struct ModRoute {
    ModSourceId source;
    ParamId     dest;
    float       depth;
    ModSourceId via;       // MOD_SRC_NONE if unused
    float       viaAmount; // 0.0 if unused
};

// Process all routes: zero dstValues, then accumulate weighted source contributions.
// srcValues : modSourceValues[MOD_SRC_COUNT] — filled this block by state machines.
// dstValues : modDestValues[PARAM_COUNT]     — receives accumulated mod deltas.
inline void processRoutes(
    const float*    srcValues,
    float*          dstValues,
    const ModRoute* routes,
    int             routeCount)
{
    memset(dstValues, 0, sizeof(float) * PARAM_COUNT);
    for (int i = 0; i < routeCount; i++) {
        if (routes[i].source == MOD_SRC_NONE) continue;
        float src = srcValues[routes[i].source];
        float via = srcValues[routes[i].via];  // 0.0 when via == MOD_SRC_NONE
        float val = src * ((1.0f - routes[i].viaAmount) + via * routes[i].viaAmount);
        dstValues[routes[i].dest] += val * routes[i].depth;
    }
}

// ===================================
// IAUDIOVOICE — unified voice interface (UAA Phase 1)
// ===================================
// All concrete voice types (sampler, soundfont, future synths) implement this.
// The mixer loop and effect helpers operate on IAudioVoice* so they are
// source-agnostic — the same code works for every source type.
class IAudioVoice {
public:
    virtual ~IAudioVoice() = default;

    // Unified parameter bus: base values (user-set) + mod accumulators (frame-reset).
    // All modulation sources write addMod(); voice render() reads get() and translates.
    ParamBus params;

    // True while this slot is producing audio (or fading out).
    virtual bool active() const = 0;

    // Hard-stop: silence immediately, mark slot free.
    virtual void hardStop() = 0;

    // Soft note-off: trigger ADSR release (or hard-stop if no release envelope).
    virtual void noteOff() = 0;

    // Real-time volume override (Vxx effect, table volume column).
    virtual void setVolume(float v) = 0;

    // Real-time pan override (table/mod destination).
    virtual void setPan(float pan) = 0;

    // Retrigger the voice at the given start-point (Repeat effect).
    // startPoint: 0-255 normalised, -1 = use current.
    virtual void retrigger(int startPoint) = 0;

    // Change the pitch to midiNote (Arpeggio effect).
    virtual void setMidiNote(int midiNote) = 0;

    // Pitch effects (PSL/PBN/PVB/PVX) — rate/total-frames already converted from ticks by Kotlin.
    // setPitchSlideRaw: slide from current offset to targetSemitones over totalFrames audio frames.
    virtual void setPitchSlideRaw(float targetSemitones, float totalFrames) = 0;
    // setPitchBendRaw: continuous bend at ratePerFrame semitones/frame; 0 = stop.
    virtual void setPitchBendRaw(float ratePerFrame) = 0;
    // setVibratoRaw: LFO vibrato at speed Hz, depth semitones; depth=0 = stop.
    virtual void setVibratoRaw(float speed, float depth) = 0;
    // clearPitchMod: reset all pitch mod state (offset, slide, vibrato).
    virtual void clearPitchMod() = 0;
    // setInitialPitchOffset: set pitchOffset without starting a slide (PSL setup).
    virtual void setInitialPitchOffset(float semitones) = 0;

    // Render up to numFrames of stereo audio into buf (interleaved L/R).
    // Returns the peak level of this block (used for per-track metering).
    virtual float render(float* buf, int numFrames) = 0;

    // Track assignment (0-7), or -1 if unassigned.
    virtual int getTrackId() const = 0;
};
