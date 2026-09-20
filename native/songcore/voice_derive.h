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
// resolve_step_params: a JVM golden (S5ConsumerGoldenTest → tools/testdata/units/s5-consumer.txt) records
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
#include "program.h"     // Program + the two derivations, which live below the sequencer
#include "scheduler.h"   // note_to_midi / note_from_midi
#include "timing.h"      // TICS_PER_STEP

namespace songcore {

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

// ─── Instrument → Program ────────────────────────────────────────────────────────────────────────
/**
 * Flatten one instrument into the scalars a note is derived from.
 *
 * ⚠️ The returned Program's `sliceMarkers` points INTO `ins` — it is valid only while `ins` is, so
 * derive from it and let it go. A table that outlives the instrument needs its own array.
 */
inline Program make_program(const Instrument& ins, float sampleRateRatio, int sfSlot) {
    Program p;
    p.type            = (ins.instrumentType == InstrumentType::SOUNDFONT) ? PROGRAM_SOUNDFONT
                      : (ins.instrumentType == InstrumentType::EXTERNAL) ? PROGRAM_EXTERNAL
                                                                         : PROGRAM_SAMPLER;
    p.hasSample       = ins.sampleFilePath.has_value();
    p.hasSoundfont    = ins.soundfontPath.has_value();
    p.sampleId        = ins.sampleId;
    p.rootMidi        = note_to_midi(ins.root);
    p.detune          = ins.detune;
    p.sampleRateRatio = sampleRateRatio;
    p.sfSlot          = sfSlot;
    p.sfBank          = ins.sfBank;
    p.sfPreset        = ins.sfPreset;
    p.tableTicRate    = ins.tableTicRate;
    p.slicingMode     = ins.slicingMode;
    p.sliceMarkers    = ins.sliceMarkers.empty() ? nullptr : ins.sliceMarkers.data();
    p.sliceCount      = static_cast<int32_t>(ins.sliceMarkers.size());
    return p;
}

/** The Program for instrument `id`, with the sample-rate ratio and SF slot the loaders resolved. */
inline Program make_program(const Project& project, const Routing& routing, int id) {
    if (id < 0 || id >= static_cast<int>(project.instruments.size())) return Program{};
    const Instrument& ins = project.instruments[static_cast<size_t>(id)];
    const int sid = ins.sampleId;
    const float ratio = (sid >= 0 && sid < POOL_INSTRUMENTS) ? routing.sampleRateRatio[sid] : 1.0f;
    return make_program(ins, ratio, routing.sfSlot[id]);
}

// AudioEngine.setInstrumentModulation, one per slot. `type == 0` is the "clear this slot" push that
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

// ⚠️ **"off" IS THE ANSWER FOR ANY NAME THIS DOES NOT KNOW**, which is how a project written by a
// newer build opens in an older one: the loop is lost rather than mis-read as the wrong mode. That is
// the right way to fail and it is SILENT — worth knowing before adding a mode, not after.
inline int loop_mode_code(const std::string& mode) {
    if (mode == "fwd") return 1;
    if (mode == "png") return 2;
    if (mode == "osc") return 3;   // a forward loop, scan rate retuned — audio-defs.h
    return 0;
}

inline int filter_type_code(const std::string& type) {
    if (type == "lp") return 1;
    if (type == "hp") return 2;
    if (type == "bp") return 3;
    return 0;
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
    // ⚠️ **AND EVERY TABLE THE HIT COULD BE HANDED ON TO.** An `INS` cell sends the hit to another
    // instrument, which brings its own table, and the engine walks that chain at the TRIGGER from its
    // own copies — so a link whose copy was never sent, or was sent before the user edited it, routes
    // on data that is no longer on screen. Pushing only the note's own table made an edit to a table
    // further down the chain do nothing at all.
    //
    // The walk follows INS cells, table by table, and the caller's cache doubles as the visited set,
    // so each table is built and sent at most once per invalidation.
    auto ensure_table_loaded = [&](int rootTableId) {
        int  pending[POOL_TABLES];
        bool queued[POOL_TABLES] = {false};
        int  top = 0;
        if (rootTableId >= 0 && rootTableId < POOL_TABLES) { pending[top++] = rootTableId; queued[rootTableId] = true; }

        while (top > 0) {
            const int tableId = pending[--top];
            if (tableId >= static_cast<int>(project.tables.size())) continue;
            if (tableLoaded[tableId]) continue;

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

                // A table id defaults to its instrument's id, so an INS value names the next table too.
                const int fxType[3]  = {row.fx1Type,  row.fx2Type,  row.fx3Type};
                const int fxValue[3] = {row.fx1Value, row.fx2Value, row.fx3Value};
                for (int s = 0; s < 3; ++s) {
                    if (fxType[s] != FX_INS) continue;
                    const int next = fxValue[s] & 0x7F;
                    if (next >= POOL_TABLES || queued[next] || tableLoaded[next]) continue;
                    queued[next] = true;
                    pending[top++] = next;
                }
            }
            engine.loadTable(tableId, rowData);
            tableLoaded[tableId] = true;
        }
    };

    const Program program = make_program(project, routing, instrumentId);
    // ⚠️ **PUSHED HERE SO IT CANNOT BE STALE.** The engine resolves this note from its own copy of
    // the instrument, and a copy that only some other call site remembered to refresh is a silent wrong
    // sound. Writing it on the path that is about to read it makes that impossible.
    engine.setProgram(instrumentId, program, program.sliceMarkers, program.sliceCount);

    // ⚠️ **THE NOTE IS QUEUED BY NUMBER AND RESOLVED AT THE HIT** (AudioEngine::scheduleProgramNote),
    // so nothing below derives a sound. What still happens HERE is everything that must already be in
    // the engine before the voice starts: the instrument's mod/EQ/sends, its playback params, its
    // table, and the resume.
    //
    // The empty-slot gate stays here too, and deliberately: a note on an empty slot must make NO
    // engine calls at all, which it cannot do if the question is not asked until the trigger.
    const int tableId = (n.tableId >= 0) ? n.tableId : instrumentId;

    if (ins.instrumentType == InstrumentType::SOUNDFONT) {
        if (!program.hasSoundfont || program.sfSlot < 0) return;   // never loaded — dropped

        push_instrument_state();
        // Every trigger: the TSF preset must carry the user's ATK/DEC/SUS/REL (else a KIL note-off
        // uses the SF2's own, often instant, release), and instrumentParams[sfId] must be reset to
        // drive=0 + the right filter (else stale WAV drive/filter values from a previous render or
        // project load bleed into the SF voice). Keyed by instrument id, not sampleId: two instruments
        // sharing one de-duplicated SF2 handle must stay isolated.
        const SFOverrides& ov = ins.sfOverrides;
        engine.setSoundfontEnvelopeOverride(ins.id, ov.ampAttack, ov.ampDecay, ov.ampSustain, ov.ampRelease);
        push_instrument_playback_params(engine, ins);

        ensure_table_loaded(tableId);
        engine.requestResume();
        engine.scheduleProgramNote(ev.frame, trackId, instrumentId, n, tempo, rootAudition);
        return;
    }

    // ── Sampler path ─────────────────────────────────────────────────────────────────────────────
    if (!program.hasSample) return;   // sampleFilePath == null — the empty-slot convention

    ensure_table_loaded(tableId);
    push_instrument_state();

    engine.requestResume();
    engine.scheduleProgramNote(ev.frame, trackId, instrumentId, n, tempo, rootAudition);
}


}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_VOICE_DERIVE_H
