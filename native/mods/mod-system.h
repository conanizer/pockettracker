#pragma once
#include <cmath>
#include <cstring>
#include <cstdint>
#include "note-queue.h"

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
    PARAM_COUNT        = 11
};

struct ParamBus {
    // In ParamId order: full volume, centred, cutoff 128, the whole sample.
    float base[PARAM_COUNT] = {1.0f, 0.5f, 0.0f, 128.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 255.0f, 0.0f};
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

// One instrument mod slot's running state in a voice.
struct VoiceModSlot {
    int type = 0;          // 0=NONE, 1=AHD, 2=ADSR, 3=LFO, 4=DRUM, 5=TRIG, 6=SCALAR
    int dest = 0;          // 0=NONE, 1=VOL, 2=PAN, 3=PITCH, 4=FINE_PITCH, 5=CUT, 6=RES, 7=STA, 8=MOD_AMT, 9=MOD_RATE, 10=MOD_BOTH
    float amount = 0.5f;   // Depth 0.0-1.0
    // AHD/ADSR stage: 0=idle, 1=attack, 2=hold(AHD)/decay(ADSR), 3=decay(AHD)/sustain(ADSR), 4=done
    // LFO stage: 1=running
    int stage = 0;
    float envValue = 0.0f;     // AHD/ADSR: 0.0-1.0; LFO: -1.0 to +1.0
    int stageCounter = 0;      // Samples elapsed in current stage
    int attackSamples = 0;
    int holdSamples = 0;
    int decaySamples = 0;
    float sustainLevel = 0.5f;   // ADSR: sustain level 0.0-1.0
    float lfoHz = 4.0f;          // LFO: frequency in Hz
    float lfoPhase = 0.0f;       // LFO: current phase (0 to 2π)
    int oscShape = 0;            // LFO: oscillator shape (0=TRI, 1=SIN, ...; 8=RND, 9=DRNK are stateful)
    int lfoTrigMode = 1;         // LFO: 0=FREE (clock-aligned phase), 1=RETG, 2=HOLD, 3=ONCE
    float lfoRandValue = 0.0f;   // LFO RND/DRNK: current sample-&-hold / drunk-walk level
    uint32_t lfoRngState = 1u;   // LFO RND/DRNK: per-slot xorshift32 state (seeded at trigger, never 0)
    int releaseSamples = 0;      // ADSR/TRIG: release duration in audio samples

    // Mod-to-mod, recomputed every block
    float effectiveAmt = 0.5f;        // amount + incoming MOD_AMT/BOTH
    float effectiveRateMult = 1.0f;   // time/freq multiplier from incoming MOD_RATE/BOTH

    float prevEnvValue = 0.0f;   // envValue at the block's start, for per-sample interpolation
};

// An envelope's attack stage (stage 1, 0 → 1), shared by AHD and ADSR; then on to stage 2.
inline void tickAttack(VoiceModSlot& mod, int effAttack) {
    if (effAttack > 0) {
        mod.envValue = fminf(1.0f, (float)mod.stageCounter / effAttack);
        if (mod.stageCounter < effAttack) return;
    }
    mod.envValue = 1.0f;
    mod.stage = 2; mod.stageCounter = 0;
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

    // The note's send levels, 0-1: seeded from the instrument at trigger, moved by REV / DEL.
    float reverbSend = 0.0f;
    float delaySend  = 0.0f;

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
