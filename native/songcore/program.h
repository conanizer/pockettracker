#ifndef POCKETTRACKER_SONGCORE_PROGRAM_H
#define POCKETTRACKER_SONGCORE_PROGRAM_H

// ─── An instrument as a NUMBER: the flat facts a note is derived from ────────────────────────────
//
// `Program` is everything the two note derivations read off an `Instrument`, as plain scalars. It
// exists so the derivation can run somewhere `Instrument` cannot go — the audio thread, which must
// not touch a std::string, a std::vector or an optional.
//
// ⚠️ This header is deliberately dependency-light (event.h + note_tables.h + timing.h, all POD and
// arithmetic). Anything added here that pulls in model.h or the sequencer takes the derivation back
// out of reach of the engine, which is the one thing it is here to avoid.

#include <algorithm>
#include <cstdint>

#include "event.h"
#include "note_tables.h"

namespace songcore {

// Tics per phrase step. Mirrors TrackerData.TICS_PER_STEP — do not hardcode 12 anywhere else.
// Here rather than in timing.h because that header needs the model and this one must not.
constexpr int TICS_PER_STEP = 12;

// Clamps. Here rather than in scheduler.h because the derivation needs them and has no business
// including the sequencer — the same move note_to_midi and hex_to_float already made into model.h.
inline int   clampi(int v, int lo, int hi)        { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi)  { return v < lo ? lo : (v > hi ? hi : v); }

// Reference pitch for C-4 — the canonical sample base frequency before sample-rate compensation and
// detune. Only slice mode uses it, to strip ROOT back out of the base frequency.
// (⚠️ NOT equal to note_hz(60): the literal is 261.63, the table's C-4 is 261.6256 from 440·2^(-9/12).
// Each is used exactly where Kotlin uses it, and swapping them moves every rendered byte.)
constexpr float C4_HZ = 261.63f;

// Instrument kinds, as numbers — InstrumentType's own order. ⚠️ A member's number is its identity:
// append, never insert.
enum ProgramType : int8_t { PROGRAM_SAMPLER = 0, PROGRAM_SOUNDFONT = 1, PROGRAM_EXTERNAL = 2 };

// ─── Program ─────────────────────────────────────────────────────────────────────────────────────
// One instrument, flattened. Built by make_program() (voice_derive.h) from an Instrument plus the two
// facts only the loaders know — the sample-rate ratio and the SoundFont slot.
//
// ⚠️ `sliceMarkers` is a VIEW, not storage: it points at memory the caller owns and must outlive the
// Program. A long-lived copy (the engine's program table) has to supply its own array.
struct Program {
    int8_t  type           = PROGRAM_SAMPLER;
    bool    hasSample      = false;   // sampleFilePath is set — THE empty-slot test, nothing else is
    bool    hasSoundfont   = false;   // soundfontPath is set
    int32_t sampleId       = -1;
    int32_t rootMidi       = 60;      // note_to_midi(root); -1 for an empty note, as the model gives it
    int32_t detune         = 0x80;
    float   sampleRateRatio = 1.0f;   // deviceRate / fileRate; 1.0 = no correction (or unloaded)
    int32_t sfSlot         = -1;      // -1 = the SF2 never loaded → the note is dropped
    int32_t sfBank         = 0;
    int32_t sfPreset       = 0;
    int32_t tableTicRate   = 6;
    int32_t slicingMode    = 0;       // 0 off, 1 CUT, 2 TRU

    const int64_t* sliceMarkers = nullptr;
    int32_t        sliceCount   = 0;
};

// ─── ProgramTable — the engine's own copy, one row per instrument ────────────────────────────────
//
// The audio thread cannot read the project: its instruments hold strings and vectors the UI thread
// may reallocate under it. So the engine keeps this, written from the UI thread whenever an
// instrument changes and read at the note trigger, exactly like instrumentParams[] beside it.

constexpr int PROGRAM_SLOTS = 128;   // instrument ids; static_asserted against POOL_INSTRUMENTS

// ⚠️ A cap, and it is provably not a limit anyone can reach: the slice a note selects is at most
// (note 131 − transpose −128 − 60) = 199, and an SLI byte is at most 255. Markers past 255 can be
// chosen by nothing, so truncating here cannot change a sound.
constexpr int PROGRAM_SLICE_MARKERS = 256;

struct ProgramTable {
    Program programs[PROGRAM_SLOTS];
    int64_t markers[PROGRAM_SLOTS][PROGRAM_SLICE_MARKERS];

    /** Replace one instrument's row. `src`/`count` are copied in; anything past the cap is dropped. */
    void set(int id, const Program& p, const int64_t* src, int count) {
        if (id < 0 || id >= PROGRAM_SLOTS) return;
        programs[id] = p;
        const int n = (count < 0) ? 0
                    : (count > PROGRAM_SLICE_MARKERS ? PROGRAM_SLICE_MARKERS : count);
        for (int i = 0; i < n; ++i) markers[id][i] = src[i];
        programs[id].sliceCount   = n;
        programs[id].sliceMarkers = nullptr;   // the view() below re-points it; see the warning there
    }

    /**
     * Instrument `id` as a Program the derivation can use.
     *
     * ⚠️ The stored row's `sliceMarkers` is deliberately null — a self-pointer would dangle the
     * moment the table were copied. It is re-pointed here, on the way out, and the returned view is
     * valid only while this table is.
     */
    Program view(int id) const {
        if (id < 0 || id >= PROGRAM_SLOTS) return Program{};
        Program p = programs[id];
        p.sliceMarkers = markers[id];
        return p;
    }
};

/**
 * Is a note on this instrument a PITCH, or is it choosing a slice?
 *
 * ⚠️ **A SLICED INSTRUMENT'S NOTE IS A SELECTOR, NOT A PITCH** — C-4 is slice 0, C#4 is slice 1, and
 * the semitone between them has nothing to do with the sound. `sliceOverride` is the step's own SLI
 * value, -1 for none: an SLI turns slice selection on for one note even with slicing mode off.
 *
 * ⚠️ model.h's note_selects_slice() decides the same question for the scale quantizer and the two
 * answers MUST agree — a note quantized there and sliced here is a drum kit playing the wrong drum.
 */
inline bool program_selects_slice(const Program& p, int sliceOverride) {
    return (p.slicingMode != 0 || sliceOverride >= 0) && p.sliceCount > 0;
}

// ─── the engine's argument lists, as data ────────────────────────────────────────────────────────

// AudioEngine::scheduleNote (the sampler path).
struct SamplerNoteArgs {
    int64_t frame = 0;
    int   sampleId = 0;
    int   trackId = 0;
    float frequency = 0.0f;
    float baseFrequency = 0.0f;
    float volume = 1.0f;        // seam arg `volume`    — the velocity curve (velGain)
    float phraseVolume = 1.0f;  // seam arg `phraseVol` — instrument vol | Vxx  (volGain)
    float pan = 0.5f;
    int   startPointOverride = -1;
    int   endPointOverride = -1;
    int   tableId = -1;
    int   tableTicRate = 6;
    int   noteOctave = 4;
    int   notePitch = 0;
    float pslInitialOffset = 0.0f;
    float pslDuration = 0.0f;   // frames
    float pbnRate = 0.0f;       // per frame
    float vibratoSpeed = 0.0f;
    float vibratoDepth = 0.0f;
    int   tableStartRow = -1;
    bool  valid = false;        // false = the note is dropped (empty slot)
};

// AudioEngine::scheduleSoundfontNote.
struct SoundfontNoteArgs {
    int64_t frame = 0;
    int   trackId = 0;
    int   sfSlot = -1;
    int   midiNote = 60;
    int   midiVelocity = 100;
    float vol = 1.0f;
    float pan = 0.5f;
    int   bank = 0;
    int   preset = 0;
    float pslInitialOffset = 0.0f;
    float pslDuration = 0.0f;
    float pbnRate = 0.0f;
    float vibratoSpeed = 0.0f;
    float vibratoDepth = 0.0f;
    float phraseVol = 1.0f;
    int   sampleId = -1;        // the INSTRUMENT id on this path, not a sample
    int   tableId = -1;
    int   tableTicRate = 6;
    int   noteOctave = 4;
    int   notePitch = 0;
    int   tableStartRow = -1;
    float detuneSemitones = 0.0f;
    bool  valid = false;        // false = dropped (no soundfontPath, or the SF2 never loaded)
};

// ─── small derivations ───────────────────────────────────────────────────────────────────────────
// ⚠️ Each keeps its operation order: these values reach the engine's pitch math and a 1-ULP drift
// changes every rendered byte.

// Instrument.detuneSemitones(): (d>>4) + (d&0xF)/16 − 8.
inline float detune_semitones(int detune) {
    return static_cast<float>(detune >> 4) + ((detune & 0x0F) / 16.0f) - 8.0f;
}

inline float frames_per_tic_f(int tempo, int sampleRate) {
    return static_cast<float>(sampleRate) / (tempo / 60.0f * 4.0f * TICS_PER_STEP);
}

// Truncates toward zero, then floors at 0.
inline int tics_to_frames(int tics, float framesPerTic) {
    const int frames = static_cast<int>(tics * framesPerTic);
    return frames < 0 ? 0 : frames;
}

// ROOT × sampleRateRatio ÷ detune. ⚠️ Detune DIVIDES — playback rate is noteFreq/baseFreq, so a
// sharper detune must LOWER baseFreq to raise the pitch.
inline float program_base_frequency(const Program& p) {
    return note_hz(p.rootMidi) * p.sampleRateRatio / detune_multiplier(p.detune);
}

// Shift a (pitch, octave) pair by semitones, clamped to the MIDI range.
inline void shift_note(int& pitch, int& octave, int semitones) {
    const int midi = clampi((octave + 1) * 12 + pitch + semitones, 0, 127);
    pitch  = midi % 12;
    octave = midi / 12 - 1;
}

// ─── the sampler note ────────────────────────────────────────────────────────────────────────────
// `sampleLength` is the engine's length for p.sampleId — slice windows need it, and asking the engine
// is the caller's job so this stays pure.
inline SamplerNoteArgs derive_sampler_note(const NoteOnPayload& n, int64_t frame, int trackId,
                                           int instrumentId, const Program& p,
                                           int tempo, int sampleRate, int64_t sampleLength) {
    SamplerNoteArgs a;
    // ⚠️ An empty slot is "no sample FILE", nothing else. Without this, stale engine-side PCM would
    // sound for an instrument the UI shows as empty.
    if (!p.hasSample) return a;   // valid = false → dropped

    // The record carries the MIDI number; recover (pitch, octave) by the raw formula — a 0..127 guard
    // here would turn an authored B-9 (131) into an empty note.
    int notePitch  = n.note % 12;
    int noteOctave = n.note / 12 - 1;

    float baseFreq = program_base_frequency(p);
    int   effStart = n.start;
    int   effEnd   = -1;

    // Slice playback: slicingMode (CUT/TRU) or an explicit SLI. Pitch becomes ROOT + chain/master
    // transpose; the phrase note only selects the slice. PIT applies after, shifting pitch without
    // changing the selection.
    if (program_selects_slice(p, n.slice)) {
        const int markerCount = p.sliceCount;

        // SLI wins; otherwise derive from the raw phrase note before transpose (C-4 = slice 0).
        const int sliceIndex = (n.slice >= 0) ? std::min(n.slice, markerCount)
                                              : clampi(n.note - n.transpose - 60, 0, markerCount);

        if (sampleLength > 0) {
            const int64_t sliceStart = (sliceIndex == 0) ? 0 : p.sliceMarkers[sliceIndex - 1];
            effStart = clampi(static_cast<int>((sliceStart * 255LL) / sampleLength), 0, 255);

            const int rootMidi = clampi(p.rootMidi + n.transpose, 0, 127);
            notePitch  = rootMidi % 12;
            noteOctave = rootMidi / 12 - 1;

            // The standard baseFreq bakes ROOT in, and the note here is ROOT-derived too, so the
            // engine's freq/baseFreq ratio would cancel ROOT out entirely. Strip it: baseFreq carries
            // only sampleRateRatio ÷ detune, leaving ROOT in the numerator alone.
            baseFreq = C4_HZ * p.sampleRateRatio / detune_multiplier(p.detune);

            // CUT bounds the slice end; OFF+SLI and TRU play on to the sample end.
            if (p.slicingMode == 1) {
                const int64_t sliceEnd = (sliceIndex < markerCount) ? p.sliceMarkers[sliceIndex]
                                                                    : sampleLength;
                effEnd = clampi(static_cast<int>((sliceEnd * 255LL) / sampleLength), 0, 255);
            }
        }
    }

    // PIT after slice selection; ARP after that (pitch only — the slice is already resolved).
    if (n.pit != 0) shift_note(notePitch, noteOctave, n.pit);
    if (n.arp != 0) shift_note(notePitch, noteOctave, n.arp);

    const float framesPerTic  = frames_per_tic_f(tempo, sampleRate);
    const float framesPerStep = framesPerTic * TICS_PER_STEP;
    const float pslDur        = f32_from_bits(n.pslDurBits);
    const float pbnRate       = f32_from_bits(n.pbnRateBits);

    a.frame              = frame;
    a.sampleId           = p.sampleId;
    a.trackId            = trackId;
    a.frequency          = note_hz((noteOctave + 1) * 12 + notePitch);
    a.baseFrequency      = baseFreq;
    a.volume             = f32_from_bits(n.velGainBits);
    a.phraseVolume       = f32_from_bits(n.volGainBits);
    a.pan                = f32_from_bits(n.panBits);
    a.startPointOverride = effStart;
    a.endPointOverride   = effEnd;
    a.tableId            = (n.tableId >= 0) ? n.tableId : instrumentId;
    a.tableTicRate       = p.tableTicRate;
    a.noteOctave         = noteOctave;
    a.notePitch          = notePitch;
    a.pslInitialOffset   = f32_from_bits(n.pslOffBits);
    a.pslDuration        = (pslDur  > 0.0f) ? pslDur * framesPerTic  : 0.0f;
    a.pbnRate            = (pbnRate != 0.0f) ? pbnRate / framesPerStep : 0.0f;
    a.vibratoSpeed       = f32_from_bits(n.vibSpdBits);
    a.vibratoDepth       = f32_from_bits(n.vibDepBits);
    a.tableStartRow      = n.tableRow;
    a.valid              = true;
    return a;
}

// ─── the SoundFont note ──────────────────────────────────────────────────────────────────────────
inline SoundfontNoteArgs derive_soundfont_note(const NoteOnPayload& n, int64_t frame, int trackId,
                                               int instrumentId, const Program& p,
                                               int tempo, int sampleRate,
                                               bool rootAudition = false) {
    SoundfontNoteArgs a;
    if (!p.hasSoundfont) return a;   // valid = false → dropped
    if (p.sfSlot < 0) return a;

    const int notePitch  = n.note % 12;
    const int noteOctave = n.note / 12 - 1;

    const int baseMidi = n.note + n.arp;
    // ROOT acts as a transpose, matching the sampler: a ROOT below C-4 raises pitch, above lowers it.
    //
    // ⚠️ `rootAudition` is the INSTRUMENT screen's "play me my root" preview, and it is not a nicety:
    // that preview plays note == root, so the transpose below would resolve to root + (60 − root) = 60
    // — a C-4 for every ROOT, and the button would appear to ignore the parameter it auditions. The
    // sequencer NEVER sets it.
    const int transpose = rootAudition ? 0 : 60 - p.rootMidi;

    const float volume = f32_from_bits(n.velGainBits);
    // The V column IS the MIDI velocity — TSF applies its own dB curve, so the channel volume stays at
    // 1.0 and instrument-vol/Vxx arrives via phraseVol (no double curve). Legacy callers (retrig/arp)
    // pass −1 and derive the velocity from the gain instead.
    const int   velocity  = (n.velocity >= 0) ? clampi(n.velocity, 1, 127)
                                              : clampi(static_cast<int>(volume * 127.0f), 1, 127);
    const float sfNoteVol = (n.velocity >= 0) ? 1.0f : volume;

    const float framesPerTic  = frames_per_tic_f(tempo, sampleRate);
    const float framesPerStep = framesPerTic * TICS_PER_STEP;
    const float pslDur        = f32_from_bits(n.pslDurBits);
    const float pbnRate       = f32_from_bits(n.pbnRateBits);

    a.frame            = frame;
    a.trackId          = trackId;
    a.sfSlot           = p.sfSlot;
    a.midiNote         = clampi(baseMidi + transpose, 0, 127);
    a.midiVelocity     = velocity;
    a.vol              = sfNoteVol;
    a.pan              = f32_from_bits(n.panBits);
    a.bank             = p.sfBank;
    a.preset           = p.sfPreset;
    a.pslInitialOffset = f32_from_bits(n.pslOffBits);
    a.pslDuration      = (pslDur  > 0.0f) ? pslDur * framesPerTic  : 0.0f;
    a.pbnRate          = (pbnRate != 0.0f) ? pbnRate / framesPerStep : 0.0f;
    a.vibratoSpeed     = f32_from_bits(n.vibSpdBits);
    a.vibratoDepth     = f32_from_bits(n.vibDepBits);
    a.phraseVol        = f32_from_bits(n.volGainBits);
    a.sampleId         = instrumentId;   // the SF path passes the instrument id here
    a.tableId          = (n.tableId >= 0) ? n.tableId : instrumentId;
    a.tableTicRate     = p.tableTicRate;
    a.noteOctave       = noteOctave;
    a.notePitch        = notePitch;
    a.tableStartRow    = n.tableRow;
    // Detune reaches the SF voice as a fractional pitch-wheel offset (the sampler bakes the same
    // semitone offset into baseFreq instead).
    a.detuneSemitones  = detune_semitones(p.detune);
    a.valid            = true;
    return a;
}

// ─── one note, either kind ───────────────────────────────────────────────────────────────────────
// Which of the two derivations a note takes is the PROGRAM's answer, not the caller's. Asking it in
// one place is what lets the instrument still be undecided when the note was queued: the sequencer
// and the audio thread run this same function and cannot disagree about the fork.
struct DerivedNote {
    bool isSoundfont = false;
    SamplerNoteArgs   sampler;
    SoundfontNoteArgs soundfont;
    bool valid = false;   // false = dropped: an empty slot, or an SF2 that never loaded
};

inline DerivedNote derive_note(const NoteOnPayload& n, int64_t frame, int trackId, int instrumentId,
                               const Program& p, int tempo, int sampleRate, int64_t sampleLength,
                               bool rootAudition = false) {
    DerivedNote d;
    if (p.type == PROGRAM_SOUNDFONT) {
        d.isSoundfont = true;
        d.soundfont = derive_soundfont_note(n, frame, trackId, instrumentId, p, tempo, sampleRate,
                                            rootAudition);
        d.valid = d.soundfont.valid;
    } else {
        d.sampler = derive_sampler_note(n, frame, trackId, instrumentId, p, tempo, sampleRate,
                                        sampleLength);
        d.valid = d.sampler.valid;
    }
    return d;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_PROGRAM_H
