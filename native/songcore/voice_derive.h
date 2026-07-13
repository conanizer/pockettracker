#ifndef POCKETTRACKER_SONGCORE_VOICE_DERIVE_H
#define POCKETTRACKER_SONGCORE_VOICE_DERIVE_H

// ─── Below-seam derivation — the PURE half of the consumer ───────────────────────────────────────
//
// Everything Kotlin's AudioEngine.scheduleNote() computes between "a NoteOn happened" and "call the
// engine": the frequency, the base frequency, the slice window, the SoundFont slot/velocity/root
// transpose, the tick→frame conversions, and the modulation-slot pushes. Pulled out as free functions
// over plain values — no engine, no I/O, no state — for one reason: **so it can be goldened**.
//
// The conformance trace stops at the router, ABOVE all of this (event-schema §6), so none of it is
// covered by the 32 golden traces. Left inside the consumer, calling AudioEngine directly, it would
// be verifiable only by ear on a device. As pure functions it gets the same measuring stick S3 gave
// resolve_step_params: a JVM golden (S5ConsumerGoldenTest → testdata/units/s5-consumer.txt) records
// what the REAL Kotlin code derives, and tools/ptvoice re-derives it here and byte-compares — floats
// as raw binary32 bits, so "close enough" cannot pass.
//
// engine_consumer.h is then only plumbing: derive → call the engine with the fields.
//
// Float exactness: note→Hz and detune→multiplier come from the generated note_tables.h (baked from
// Kotlin's own Double pow — see there), and every other expression keeps Kotlin's operation order,
// because these values reach the engine's pitch math and a 1-ULP drift changes every rendered byte.

#include <algorithm>
#include <cstdint>
#include <string>

#include "event.h"
#include "model.h"
#include "note_tables.h"
#include "scheduler.h"   // note_to_midi / note_from_midi / clampi
#include "timing.h"      // TICS_PER_STEP

namespace songcore {

// Reference pitch for C-4 (AudioEngine.C4_HZ) — the canonical sample base frequency before sample-rate
// compensation and detune. Only slice mode uses it, to strip ROOT back out of the base frequency.
// (Note this is NOT equal to note_hz(60): the literal is 261.63, the table's C-4 is 261.6256 from
// 440·2^(-9/12). Both are Kotlin's, and each is used exactly where Kotlin uses it.)
constexpr float C4_HZ = 261.63f;

// ─── Routing: the per-instrument facts the app resolves at load time ─────────────────────────────
// songcore never opens a file, so the two things the file loaders learn are pushed down here. Both
// mirror Kotlin state exactly: AudioEngine.sampleRateRatios and InstrumentController.sfSlotMap.
struct Routing {
    float sampleRateRatio[POOL_INSTRUMENTS];  // deviceRate / fileRate; 1.0 = no correction (or unloaded)
    int   sfSlot[POOL_INSTRUMENTS];           // slot the soundfontPath resolved to; -1 = none → note dropped

    Routing() { reset(); }
    void reset() {
        for (int i = 0; i < POOL_INSTRUMENTS; ++i) {
            sampleRateRatio[i] = 1.0f;
            sfSlot[i] = -1;
        }
    }
};

// ─── the engine's argument lists, as data ────────────────────────────────────────────────────────

// AudioEngine::scheduleNote (the sampler path — Kotlin's backend.scheduleNoteWithTable).
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
    bool  valid = false;        // false = the note is dropped (empty slot), as Kotlin returns early
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
    int   sampleId = -1;        // the INSTRUMENT id here (Kotlin passes sampleId = instrumentId)
    int   tableId = -1;
    int   tableTicRate = 6;
    int   noteOctave = 4;
    int   notePitch = 0;
    int   tableStartRow = -1;
    float detuneSemitones = 0.0f;
    bool  valid = false;        // false = dropped (no soundfontPath, or the SF2 never loaded)
};

// AudioEngine::setInstrumentModulation, one per slot. `type == 0` is the "clear this slot" push that
// Kotlin makes for NONE / unrouted-dest slots — it is a real call, not an absence.
struct ModPush {
    int   sampleId = 0;
    int   slotIndex = 0;
    int   type = 0;
    int   dest = 0;
    float amount = 0.0f;
    int   attackSamples = 0;
    int   holdSamples = 0;
    int   decaySamples = 0;
    float sustainLevel = 0.5f;   // Kotlin's default for every non-ADSR/TRIG slot
    float lfoHz = 4.0f;          // Kotlin's default for every non-LFO slot
    int   oscShape = 0;
    int   releaseSamples = 0;
    int   lfoTrigMode = 1;
};

struct ModPushes {
    ModPush slots[4];
    bool anyActive = false;   // false → the caller instead issues clearInstrumentModulation(sampleId)
};

// ─── small derivations (each mirrors one Kotlin expression, operation order included) ────────────

// Instrument.detuneSemitones(): (d>>4) + (d&0xF)/16 − 8. Pure binary32; no table needed.
inline float detune_semitones(int detune) {
    return static_cast<float>(detune >> 4) + ((detune & 0x0F) / 16.0f) - 8.0f;
}

// AudioEngine.framesPerTicAt(tempo).
inline float frames_per_tic_f(int tempo, int sampleRate) {
    return static_cast<float>(sampleRate) / (tempo / 60.0f * 4.0f * TICS_PER_STEP);
}

// Kotlin: (tics * framesPerTic).toInt().coerceAtLeast(0) — .toInt() truncates toward zero.
inline int tics_to_frames(int tics, float framesPerTic) {
    const int frames = static_cast<int>(tics * framesPerTic);
    return frames < 0 ? 0 : frames;
}

// AudioEngine.calculateInstrumentBaseFrequency: ROOT × sampleRateRatio ÷ detune. Detune DIVIDES —
// playback rate is noteFreq/baseFreq, so a sharper detune must LOWER baseFreq to raise the pitch.
inline float instrument_base_frequency(const Instrument& ins, float sampleRateRatio) {
    return note_hz(note_to_midi(ins.root)) * sampleRateRatio / detune_multiplier(ins.detune);
}

inline int mod_dest_code(ModDest dest) {
    switch (dest) {
        case ModDest::VOLUME:        return 1;
        case ModDest::PAN:           return 2;
        case ModDest::PITCH:         return 3;
        case ModDest::FINE_PITCH:    return 4;
        case ModDest::FILTER_CUTOFF: return 5;
        case ModDest::FILTER_RES:    return 6;
        case ModDest::SAMPLE_START:  return 7;
        case ModDest::MOD_AMT:       return 8;   // scales the NEXT slot's amount
        case ModDest::MOD_RATE:      return 9;   // scales the NEXT slot's time/freq
        case ModDest::MOD_BOTH:      return 10;
        default:                     return 0;
    }
}

inline int loop_mode_code(const std::string& mode) {
    if (mode == "fwd") return 1;
    if (mode == "png") return 2;
    return 0;
}

inline int filter_type_code(const std::string& type) {
    if (type == "lp") return 1;
    if (type == "hp") return 2;
    if (type == "bp") return 3;
    return 0;
}

// Shift a (pitch, octave) pair by semitones, clamped to the MIDI range — Kotlin's
// Note.fromMidi((note.toMidi() + n).coerceIn(0, 127)).
inline void shift_note(int& pitch, int& octave, int semitones) {
    const Note n = note_from_midi(clampi((octave + 1) * 12 + pitch + semitones, 0, 127));
    pitch  = n.pitch;
    octave = n.octave;
}

// ─── AudioEngine.pushInstrumentModulation ────────────────────────────────────────────────────────
// The 0.5f / 4.0f / 0 defaults below are Kotlin's *named-argument defaults* for sustainLevel / lfoHz /
// oscShape, which the C++ engine method does not have. Passing anything else would change how a
// non-LFO slot behaves, so they are written out.
inline ModPushes derive_mod_pushes(const Instrument& ins, int tempo, int sampleRate) {
    ModPushes out;
    const float framesPerTic = frames_per_tic_f(tempo, sampleRate);
    const int   sampleId     = ins.sampleId;

    for (int i = 0; i < 4; ++i) {
        ModPush& p = out.slots[i];
        p.sampleId  = sampleId;
        p.slotIndex = i;

        if (i >= static_cast<int>(ins.modSlots.size())) continue;   // cleared slot (type 0)
        const ModSlot& slot = ins.modSlots[i];
        const int dest = mod_dest_code(slot.dest);

        // Kotlin clears the slot when the dest is unrouted, and its `when` has no arm for NONE or
        // TRACKING — both fall to the else branch, which also clears.
        if (dest == 0 || slot.type == ModType::NONE || slot.type == ModType::TRACKING) continue;

        p.dest   = dest;
        p.amount = slot.amount / 255.0f;

        switch (slot.type) {
            case ModType::AHD:
            case ModType::DRUM:   // DRUM = AHD semantics; type 4 so C++ can differentiate later
                p.type          = (slot.type == ModType::AHD) ? 1 : 4;
                p.attackSamples = tics_to_frames(slot.attack, framesPerTic);
                p.holdSamples   = tics_to_frames(slot.hold,   framesPerTic);
                p.decaySamples  = tics_to_frames(slot.decay,  framesPerTic);
                out.anyActive   = true;
                break;

            case ModType::ADSR:
            case ModType::TRIG:   // TRIG = ADSR semantics; type 5
                p.type           = (slot.type == ModType::ADSR) ? 2 : 5;
                p.attackSamples  = tics_to_frames(slot.attack,  framesPerTic);
                p.holdSamples    = 0;
                p.decaySamples   = tics_to_frames(slot.decay,   framesPerTic);
                p.releaseSamples = tics_to_frames(slot.release, framesPerTic);
                p.sustainLevel   = slot.sustain / 255.0f;
                out.anyActive    = true;
                break;

            case ModType::LFO:
                p.type        = 3;
                p.lfoHz       = (slot.lfoFreq + 1) * 20.0f / 256.0f;   // 0x00-0xFF → 0.1 .. 20 Hz
                p.oscShape    = slot.oscShape;
                p.lfoTrigMode = slot.lfoTrigMode;
                out.anyActive = true;
                break;

            case ModType::SCALAR:   // amount is the fixed output value; no time params
                p.type        = 6;
                out.anyActive = true;
                break;

            default:
                p = ModPush{};            // unreachable; keep the cleared shape
                p.sampleId = sampleId;
                p.slotIndex = i;
                break;
        }
    }
    return out;
}

// The two pushes that must reach the engine BEFORE a voice triggers: the modulation slots and the
// EQ/send routing (AudioEngine.pushInstrumentModulation + pushInstrumentEqAndSends). A template over
// the engine, like plan_note_on, so tools/ptvoice golden-checks the calls it makes.
//
// Both the note path (below) and the project→engine setup (engine_setup.h) push these — Kotlin does
// too, from scheduleNote and from RenderController.setupInstrumentParams — so they share one
// implementation here rather than two that could drift.
template <typename Engine>
void push_instrument_mod_eq_sends(Engine& engine, const Instrument& ins, int tempo, int sampleRate) {
    const ModPushes m = derive_mod_pushes(ins, tempo, sampleRate);
    for (const ModPush& p : m.slots) {
        engine.setInstrumentModulation(p.sampleId, p.slotIndex, p.type, p.dest, p.amount,
                                       p.attackSamples, p.holdSamples, p.decaySamples,
                                       p.sustainLevel, p.lfoHz, p.oscShape,
                                       p.releaseSamples, p.lfoTrigMode);
    }
    if (!m.anyActive) engine.clearInstrumentModulation(ins.sampleId);
    engine.setInstrumentEqSlot(ins.sampleId, ins.eqSlot);
    engine.setInstrumentSendLevels(ins.sampleId, ins.reverbSend, ins.delaySend);
}

// AudioEngine.updateInstrumentPlaybackParams — the sample-playback window, loop, drive/crush/
// downsample and filter. (Kotlin's applySoundfontFilterOverrides is this same call under another
// name, which is why the SF and sampler setup paths push identical params.)
template <typename Engine>
void push_instrument_playback_params(Engine& engine, const Instrument& ins) {
    engine.setInstrumentParams(ins.sampleId, ins.sampleStart, ins.sampleEnd, ins.reverse,
                               loop_mode_code(ins.loopMode), ins.loopStart, ins.loopEnd,
                               ins.drive, ins.crush, ins.downsample,
                               filter_type_code(ins.filterType), ins.filterCut, ins.filterRes);
}

// ─── AudioEngine.scheduleNote — the sampler path ─────────────────────────────────────────────────
// `sampleLength` is engine_->getSampleLength(sampleId) (slice windows need it); `sampleRateRatio` is
// deviceRate/fileRate for that sample. Both are pushed/read by the caller, keeping this pure.
inline SamplerNoteArgs derive_sampler_note(const NoteOnPayload& n, int64_t frame, int trackId,
                                           int instrumentId, const Instrument& ins,
                                           int tempo, int sampleRate,
                                           float sampleRateRatio, int64_t sampleLength) {
    SamplerNoteArgs a;
    // sampleFilePath == null is the single "empty slot" signal — without this, stale engine-side PCM
    // would sound for an instrument the UI shows as empty.
    if (!ins.sampleFilePath.has_value()) return a;   // valid = false → dropped

    // The record carries the MIDI number; recover (pitch, octave) by the raw formula — NOT through
    // note_from_midi(), whose 0..127 guard would turn an authored B-9 (131) into an empty note.
    int notePitch  = n.note % 12;
    int noteOctave = n.note / 12 - 1;

    const int sampleId = ins.sampleId;

    float baseFreq = instrument_base_frequency(ins, sampleRateRatio);
    int   effStart = n.start;
    int   effEnd   = -1;

    // Slice playback: slicingMode (CUT/TRU) or an explicit SLI. Pitch becomes ROOT + chain/master
    // transpose; the phrase note only selects the slice. PIT applies after, shifting pitch without
    // changing the selection.
    const bool useSliceLogic = (ins.slicingMode != 0 || n.slice >= 0) && !ins.sliceMarkers.empty();
    if (useSliceLogic) {
        const std::vector<int64_t>& markers = ins.sliceMarkers;
        const int markerCount = static_cast<int>(markers.size());

        // SLI wins; otherwise derive from the raw phrase note before transpose (C-4 = slice 0).
        const int sliceIndex = (n.slice >= 0) ? std::min(n.slice, markerCount)
                                              : clampi(n.note - n.transpose - 60, 0, markerCount);

        if (sampleLength > 0) {
            const int64_t sliceStart = (sliceIndex == 0) ? 0 : markers[sliceIndex - 1];
            effStart = clampi(static_cast<int>((sliceStart * 255LL) / sampleLength), 0, 255);

            const Note en = note_from_midi(clampi(note_to_midi(ins.root) + n.transpose, 0, 127));
            notePitch  = en.pitch;
            noteOctave = en.octave;

            // The standard baseFreq bakes ROOT in, and the note here is ROOT-derived too, so the
            // engine's freq/baseFreq ratio would cancel ROOT out entirely. Strip it: baseFreq carries
            // only sampleRateRatio ÷ detune, leaving ROOT in the numerator alone.
            baseFreq = C4_HZ * sampleRateRatio / detune_multiplier(ins.detune);

            // CUT bounds the slice end; OFF+SLI and TRU play on to the sample end.
            if (ins.slicingMode == 1) {
                const int64_t sliceEnd = (sliceIndex < markerCount) ? markers[sliceIndex] : sampleLength;
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
    a.sampleId           = sampleId;
    a.trackId            = trackId;
    a.frequency          = note_hz((noteOctave + 1) * 12 + notePitch);
    a.baseFrequency      = baseFreq;
    a.volume             = f32_from_bits(n.velGainBits);
    a.phraseVolume       = f32_from_bits(n.volGainBits);
    a.pan                = f32_from_bits(n.panBits);
    a.startPointOverride = effStart;
    a.endPointOverride   = effEnd;
    a.tableId            = (n.tableId >= 0) ? n.tableId : instrumentId;
    a.tableTicRate       = ins.tableTicRate;
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

// ─── AudioEngine.scheduleNote — the SoundFont path ───────────────────────────────────────────────
// `sfSlot` is what the instrument's soundfontPath resolved to in the loader's path→slot map; -1 means
// the SF2 never loaded and Kotlin drops the note.
inline SoundfontNoteArgs derive_soundfont_note(const NoteOnPayload& n, int64_t frame, int trackId,
                                               int instrumentId, const Instrument& ins,
                                               int tempo, int sampleRate, int sfSlot,
                                               bool rootAudition = false) {
    SoundfontNoteArgs a;
    if (!ins.soundfontPath.has_value()) return a;   // valid = false → dropped
    if (sfSlot < 0) return a;

    const int notePitch  = n.note % 12;
    const int noteOctave = n.note / 12 - 1;

    const int baseMidi = n.note + n.arp;
    // ROOT acts as a transpose, matching the sampler: a ROOT below C-4 raises pitch, above lowers it.
    //
    // `rootAudition` is the INSTRUMENT screen's "play me my root" preview, and it is not a nicety: that
    // preview plays note == root, so the transpose below would resolve to root + (60 − root) = 60 — a
    // C-4, every time, for every ROOT. The button would appear to ignore the very parameter it is
    // there to audition. The sequencer NEVER sets it (Kotlin: `isRootAudition`, default false).
    const int transpose = rootAudition ? 0 : 60 - note_to_midi(ins.root);

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
    a.sfSlot           = sfSlot;
    a.midiNote         = clampi(baseMidi + transpose, 0, 127);
    a.midiVelocity     = velocity;
    a.vol              = sfNoteVol;
    a.pan              = f32_from_bits(n.panBits);
    a.bank             = ins.sfBank;
    a.preset           = ins.sfPreset;
    a.pslInitialOffset = f32_from_bits(n.pslOffBits);
    a.pslDuration      = (pslDur  > 0.0f) ? pslDur * framesPerTic  : 0.0f;
    a.pbnRate          = (pbnRate != 0.0f) ? pbnRate / framesPerStep : 0.0f;
    a.vibratoSpeed     = f32_from_bits(n.vibSpdBits);
    a.vibratoDepth     = f32_from_bits(n.vibDepBits);
    a.phraseVol        = f32_from_bits(n.volGainBits);
    a.sampleId         = instrumentId;   // Kotlin passes sampleId = instrumentId on this path
    a.tableId          = (n.tableId >= 0) ? n.tableId : instrumentId;
    a.tableTicRate     = ins.tableTicRate;
    a.noteOctave       = noteOctave;
    a.notePitch        = notePitch;
    a.tableStartRow    = n.tableRow;
    // Detune reaches the SF voice as a fractional pitch-wheel offset (the sampler bakes the same
    // semitone offset into baseFreq instead).
    a.detuneSemitones  = detune_semitones(ins.detune);
    a.valid            = true;
    return a;
}
// ─── The NoteOn plan ─────────────────────────────────────────────────────────────────────────────
// The exact sequence of engine calls a NoteOn produces — a TEMPLATE over the engine, not a call into
// a concrete one, for a specific reason: AudioEngine satisfies it as-is (same method names), and
// tools/ptvoice can instantiate it with a recorder instead. That means the host conformance check
// covers not just the derived values but the *sequence* — which calls happen, in what order, and when
// a note is dropped instead. Nothing about the note path is left to be verified only by ear.
//
// `tableLoaded` is the caller's POOL_TABLES-sized cache (Kotlin's `loadedTables` set).
//
// `rootAudition` is a PREVIEW-only flag and it is deliberately a parameter rather than a field on the
// Event: the event schema is ratified, its records are byte-compared against the goldens, and a
// preview is not a bus event in the first place (it never reaches the router or the trace). See
// derive_soundfont_note for what it does and why the INSTRUMENT screen cannot work without it.
template <typename Engine>
void plan_note_on(Engine& engine, const Event& ev, const Project& project, const Routing& routing,
                  bool* tableLoaded, bool rootAudition = false) {
    const NoteOnPayload& n = ev.noteOn;
    const int instrumentId = ev.instrument;
    const int trackId      = ev.track;

    if (instrumentId < 0 || instrumentId >= static_cast<int>(project.instruments.size())) return;
    const Instrument& ins = project.instruments[instrumentId];

    // Keep the engine's tempo current so the standard-mode table advance stays tempo-locked (live
    // playback and offline render both schedule through here).
    const int tempo      = project.tempo;
    const int sampleRate = engine.getSampleRate();
    engine.setTempo(tempo);

    // Modulation / EQ / sends must reach the engine BEFORE the note triggers.
    auto push_instrument_state = [&]() { push_instrument_mod_eq_sends(engine, ins, tempo, sampleRate); };

    // Lazy table push, exactly like Kotlin's `loadedTables` — but built from songcore's own project
    // copy, so no table data has to cross the JNI boundary.
    auto ensure_table_loaded = [&](int tableId) {
        if (tableId < 0 || tableId >= static_cast<int>(project.tables.size())) return;
        if (tableId >= POOL_TABLES || tableLoaded[tableId]) return;

        const Table& table = project.tables[tableId];
        uint8_t rowData[128] = {0};
        for (int rowIndex = 0; rowIndex < 16 && rowIndex < static_cast<int>(table.rows.size()); ++rowIndex) {
            const TableRow& row = table.rows[rowIndex];
            uint8_t* p = rowData + rowIndex * 8;
            p[0] = static_cast<uint8_t>(row.transpose);
            p[1] = static_cast<uint8_t>(row.volume);     // -1 → 0xFF, as Kotlin's toByte() gives
            p[2] = static_cast<uint8_t>(row.fx1Type);
            p[3] = static_cast<uint8_t>(row.fx1Value);
            p[4] = static_cast<uint8_t>(row.fx2Type);
            p[5] = static_cast<uint8_t>(row.fx2Value);
            p[6] = static_cast<uint8_t>(row.fx3Type);
            p[7] = static_cast<uint8_t>(row.fx3Value);
        }
        engine.loadTable(tableId, rowData);
        tableLoaded[tableId] = true;
    };

    if (ins.instrumentType == InstrumentType::SOUNDFONT) {
        const SoundfontNoteArgs a = derive_soundfont_note(n, ev.frame, trackId, instrumentId, ins,
                                                          tempo, sampleRate, routing.sfSlot[instrumentId],
                                                          rootAudition);
        if (!a.valid) return;   // no soundfontPath, or the SF2 never loaded — Kotlin drops it here

        push_instrument_state();
        // Every trigger: the TSF preset must carry the user's ATK/DEC/SUS/REL (else a KIL note-off
        // uses the SF2's own, often instant, release), and instrumentParams[sfId] must be reset to
        // drive=0 + the right filter (else stale WAV drive/filter values from a previous render or
        // project load bleed into the SF voice). Keyed by instrument id, not sampleId: two instruments
        // sharing one de-duplicated SF2 handle must stay isolated.
        const SFOverrides& ov = ins.sfOverrides;
        engine.setSoundfontEnvelopeOverride(ins.id, ov.ampAttack, ov.ampDecay, ov.ampSustain, ov.ampRelease);
        push_instrument_playback_params(engine, ins);

        ensure_table_loaded(a.tableId);
        engine.requestResume();
        engine.scheduleSoundfontNote(a.frame, a.trackId, a.sfSlot, a.midiNote, a.midiVelocity,
                                     a.vol, a.pan, a.bank, a.preset,
                                     a.pslInitialOffset, a.pslDuration, a.pbnRate,
                                     a.vibratoSpeed, a.vibratoDepth,
                                     a.phraseVol, a.sampleId, a.tableId, a.tableTicRate,
                                     a.noteOctave, a.notePitch, a.tableStartRow, a.detuneSemitones);
        return;
    }

    // ── Sampler path ─────────────────────────────────────────────────────────────────────────────
    const int sampleId = ins.sampleId;
    const float ratio = (sampleId >= 0 && sampleId < POOL_INSTRUMENTS)
                        ? routing.sampleRateRatio[sampleId] : 1.0f;   // Kotlin's `?: 1.0f`
    const SamplerNoteArgs a = derive_sampler_note(n, ev.frame, trackId, instrumentId, ins,
                                                  tempo, sampleRate, ratio,
                                                  engine.getSampleLength(sampleId));
    if (!a.valid) return;   // sampleFilePath == null — the empty-slot convention

    ensure_table_loaded(a.tableId);
    push_instrument_state();

    engine.requestResume();
    engine.scheduleNote(a.frame, a.sampleId, a.trackId, a.frequency, a.baseFrequency,
                        a.volume, a.phraseVolume, a.pan,
                        a.startPointOverride, a.endPointOverride,
                        a.tableId, a.tableTicRate, a.noteOctave, a.notePitch,
                        a.pslInitialOffset, a.pslDuration, a.pbnRate,
                        a.vibratoSpeed, a.vibratoDepth, a.tableStartRow);
}


}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_VOICE_DERIVE_H
