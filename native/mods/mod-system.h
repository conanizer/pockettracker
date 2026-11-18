#pragma once
#include <cmath>
#include <cstring>
#include <cstdint>
#include "audio-defs.h"
#include "songcore/voice_codes.h"
#include "effects/glide.h"

// A voice's parameters: each slot is a base (set at trigger or by an effect) plus a mod accumulator
// cleared every block. Value = base + mod, clamped where it is read.
// ⚠️ Envelope/LFO modulation of VOL is not accumulated here — the mix loop applies it per sample.
// The table, phrase and instrument volumes are, through the fixed VOL route.

enum ParamId {
    PARAM_VOL          = 0,  // Volume: 0.0–1.0
    PARAM_PAN          = 1,  // Pan: 0.0=left, 0.5=center, 1.0=right
    // ⚠️ The halves carry different things: `mod` is envelope and LFO, `base` is FIN's fine tune
    // (cleared by every trigger). They add.
    PARAM_PITCH        = 2,  // Pitch offset in semitones
    PARAM_FILTER_CUT   = 3,  // Filter cutoff: 0–255 param units
    PARAM_FILTER_RES   = 4,  // Filter resonance: 0–255 param units
    PARAM_DRIVE        = 5,  // Drive pre-gain boost: 0–255
    PARAM_CRUSH        = 6,  // Bit-crush depth: 0–15 (0=off, 15=1-bit)
    PARAM_DOWNSAMPLE   = 7,  // Sample-rate reduction: 0–15 (0=off; factor = 1 << value)
    PARAM_SAMPLE_START = 8,  // Sample start point: 0–255 normalized
    PARAM_SAMPLE_END   = 9,  // Sample end point: 0–255 normalized
    PARAM_LOOP_START   = 10, // Loop start point: 0–255 normalized
    PARAM_MACRO_TIMBRE = 11, // A MACRO voice's TIMBRE: 0–255
    PARAM_MACRO_COLOR  = 12, // …and its COLOR: 0–255
    PARAM_COUNT        = 13
};

struct ParamBus {
    // In ParamId order: full volume, centred, cutoff 128, the whole sample, TIMBRE and COLOR midway.
    float base[PARAM_COUNT] = {1.0f, 0.5f, 0.0f, 128.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 255.0f, 0.0f,
                               128.0f, 128.0f};
    float mod[PARAM_COUNT]  = {};   // cleared every block, written by the modulation sources

    void resetMods()                      { memset(mod, 0, sizeof(mod)); }
    void setBase(ParamId id, float value) { base[id] = value; }
    float get(ParamId id)   const         { return base[id] + mod[id]; }
};

// Modulation: each source writes `modSourceValues[]` once per block, `processRoutes` sums them into
// `modDestValues[]`, and sound code reads only the destinations.

enum ModSourceId {
    MOD_SRC_NONE = 0,  // always 0 — the "no via" placeholder

    // Advanced every block
    MOD_SRC_ENV0, MOD_SRC_ENV1, MOD_SRC_ENV2, MOD_SRC_ENV3,  // AHD / ADSR / DRUM / TRIG
    MOD_SRC_LFO0, MOD_SRC_LFO1, MOD_SRC_LFO2, MOD_SRC_LFO3,

    // Fixed at note-on
    MOD_SRC_VELOCITY,  // 0..1, the note's volume
    MOD_SRC_KEYTRACK,  // (midiNote − 60) / 12, bipolar
    MOD_SRC_RANDOM,    // 0..1

    // Written every block by the table and pitch state
    MOD_SRC_TABLE_VOL,    // 0..1
    MOD_SRC_TABLE_PITCH,  // semitones
    MOD_SRC_PITCH_SLIDE,  // semitones, PSL / PBN
    MOD_SRC_VIBRATO,      // −1..+1, PVB / PVX

    // Fixed at note-on
    MOD_SRC_PHRASE_VOL,   // 0..1, the step's volume (or a V command at trigger)

    MOD_SRC_COUNT  // = 17
};

using songcore::ModType;
using songcore::ModDest;

// One instrument mod slot in a voice: its settings, copied at the trigger, and its running state.
// An envelope's `stage`: AHD and DRUM 1 attack, 2 hold, 3 decay, 4 done; ADSR and TRIG 1 attack, 2 decay,
// 3 sustain, 4 release, 5 done. An LFO runs at 1 (ONCE holds at 4). 0 is a slot not running.
struct VoiceModSlot : songcore::EngineModSlot {
    int stage = 0;
    float envValue = 0.0f;       // an envelope 0..1, an LFO −1..+1
    int stageCounter = 0;        // samples into the stage
    float lfoPhase = 0.0f;       // 0 to 2π
    float lfoRandValue = 0.0f;   // RND/DRNK: the current sample-&-hold / drunk-walk level
    uint32_t lfoRngState = 1u;   // RND/DRNK: per-slot xorshift32 state (seeded at trigger, never 0)
    float releaseFrom = 0.0f;    // where the release began — it falls from there

    // Mod-to-mod, recomputed every block
    float effectiveAmt = 0.5f;        // amount + incoming MOD_AMT/BOTH
    float effectiveRateMult = 1.0f;   // time/freq multiplier from incoming MOD_RATE/BOTH

    float prevEnvValue = 0.0f;   // envValue at the block's start, for per-sample interpolation
};

// Slot types by behaviour: every envelope, and those with a release.
inline bool mod_is_envelope(ModType t) {
    return t == ModType::AHD || t == ModType::ADSR || t == ModType::DRUM || t == ModType::TRIG;
}
inline bool mod_has_release(ModType t) { return t == ModType::ADSR || t == ModType::TRIG; }

inline bool mod_running(const VoiceModSlot& m) { return m.type != ModType::NONE && m.stage != 0; }
// An envelope still on its way to the sustain — the stages an OFF moves into the release.
inline bool env_before_release(const VoiceModSlot& m) { return m.stage >= 1 && m.stage <= 3; }
inline bool env_releasing(const VoiceModSlot& m) { return mod_has_release(m.type) && m.stage == 4; }
inline bool env_done(const VoiceModSlot& m) { return m.stage >= (mod_has_release(m.type) ? 5 : 4); }

// An envelope's attack stage (stage 1, 0 → 1), shared by AHD and ADSR; then on to stage 2.
inline void tickAttack(VoiceModSlot& mod, int effAttack) {
    if (effAttack > 0) {
        mod.envValue = fminf(1.0f, (float)mod.stageCounter / effAttack);
        if (mod.stageCounter < effAttack) return;
    }
    mod.envValue = 1.0f;
    mod.stage = 2; mod.stageCounter = 0;
}

// Into the release, falling from wherever the envelope stands — an OFF mid-attack included.
inline void enter_release(VoiceModSlot& m) {
    m.stage        = 4;
    m.stageCounter = 0;
    m.releaseFrom  = m.envValue;
}

// At an OFF, every live envelope aimed anywhere but VOL falls. The VOL ones are the voice's own
// call: they decide whether it has a release to run at all.
inline void release_non_vol_envelopes(VoiceModSlot (&mods)[4]) {
    for (VoiceModSlot& m : mods)
        if (m.dest != ModDest::VOLUME && mod_has_release(m.type) && env_before_release(m)) enter_release(m);
}

// One source → one destination: dest += source × ((1 − viaAmount) + via × viaAmount) × depth.
struct ModRoute {
    ModSourceId source;
    ParamId     dest;
    float       depth;
    ModSourceId via;       // MOD_SRC_NONE if unused
    float       viaAmount; // 0.0 if unused
};

// Zeroes the destinations, then sums every route into them.
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

// What every voice type (sampler, SoundFont) offers the mixer and the effects.
class IAudioVoice {
public:
    virtual ~IAudioVoice() = default;

    // `mod` mirrors `modDestValues` every block.
    ParamBus params;

    // Copied from the instrument at trigger. `prevModDestValues` is the block's start, for
    // interpolation.
    VoiceModSlot voiceMods[4];
    float modSourceValues[MOD_SRC_COUNT]{};
    float modDestValues[PARAM_COUNT]{};
    float prevModDestValues[PARAM_COUNT]{};

    // The note's send levels: from the instrument at trigger, then REV / DEL; they fade, never jump.
    Ramp reverbSend;
    Ramp delaySend;

    // A MIDI-in pitch wheel, semitones: zeroed at every trigger, then set by the channel's bend.
    float midiWheel = 0.0f;

    virtual bool active() const = 0;   // sounding, or fading out
    virtual void hardStop() = 0;       // silent now, slot free
    virtual void noteOff() = 0;        // release, or a hard stop with no release envelope
    virtual void setVolume(float v) = 0;
    virtual void setPan(float pan) = 0;
    virtual void retrigger(int startPoint) = 0;   // 0-255, −1 = where it is
    virtual void setMidiNote(int midiNote) = 0;

    // PBN/PVB/PVX mid-note; PSL travels on the scheduled note instead.
    virtual void setPitchBendRaw(float ratePerFrame) = 0;   // semitones per frame; 0 stops
    virtual void setVibratoRaw(float speed, float depth) = 0;   // Hz, semitones; depth 0 stops

    // Interleaved stereo into buf; returns the block's peak for the track meter.
    virtual float render(float* buf, int numFrames) = 0;

    virtual int getTrackId() const = 0;   // 0-7, or −1
};
