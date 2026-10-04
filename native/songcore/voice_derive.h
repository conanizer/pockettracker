#ifndef POCKETTRACKER_SONGCORE_VOICE_DERIVE_H
#define POCKETTRACKER_SONGCORE_VOICE_DERIVE_H

// ─── Below-seam derivation — the PURE half of the consumer ───────────────────────────────────────
//
// Everything between "a NoteOn happened" and "call the engine": frequency, base frequency, slice
// window, SoundFont slot/velocity/root transpose, tick→frame, modulation pushes. Free functions over
// plain values so they can be tested — the event traces stop at the router, above all of this.
// Floats are compared as raw bits, so "close enough" cannot pass.
// note→Hz and detune come from note_tables.h; every other expression keeps its operation order, since
// a 1-ULP drift changes every rendered byte.

#include <algorithm>
#include <cstdint>
#include <string>

#include "event.h"
#include "model.h"
#include "note_tables.h"
#include "program.h"     // Program + the two derivations
#include "scheduler.h"   // note_to_midi / note_from_midi
#include "timing.h"      // TICS_PER_STEP

namespace songcore {

// ─── Routing: the per-instrument facts learned at load time ──────────────────────────────────────
// songcore opens no file except through the loaders, which record these two facts here.
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

// One modulation slot's push. `type == 0` is a real "clear this slot" call, made for NONE and
// unrouted-dest slots.
struct ModPush {
    int   sampleId = 0;
    int   slotIndex = 0;
    int   type = 0;
    int   dest = 0;
    float amount = 0.0f;
    int   attackSamples = 0;
    int   holdSamples = 0;
    int   decaySamples = 0;
    float sustainLevel = 0.5f;   // the default for every non-ADSR/TRIG slot
    float lfoHz = 4.0f;          // the default for every non-LFO slot
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

// ⚠️ "off" is the answer for any name this does not know, so a newer build's project opens with the
// loop lost rather than mis-read. That failure is SILENT — know it before adding a mode.
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

// ─── The instrument's modulation push ────────────────────────────────────────────────────────────
// The 0.5f / 4.0f / 0 defaults for sustainLevel / lfoHz / oscShape are written out: anything else
// changes how a non-LFO slot behaves.
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

        // An unrouted dest clears the slot; NONE and TRACKING clear it too.
        if (dest == 0 || slot.type == ModType::NONE || slot.type == ModType::TRACKING) continue;

        p.dest   = dest;
        p.amount = slot.amount / 255.0f;

        switch (slot.type) {
            case ModType::AHD:
            case ModType::DRUM:   // AHD semantics; its own type 4 so the engine can tell them apart
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
// EQ/send routing. Shared by the note path and engine_setup.h; templated so a test can check the
// calls.
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

// The sample-playback window, loop, drive/crush/downsample and filter. SoundFont and sampler
// instruments push the same params.
template <typename Engine>
void push_instrument_playback_params(Engine& engine, const Instrument& ins) {
    engine.setInstrumentParams(ins.sampleId, ins.sampleStart, ins.sampleEnd, ins.reverse,
                               loop_mode_code(ins.loopMode), ins.loopStart, ins.loopEnd,
                               ins.drive, ins.crush, ins.downsample,
                               filter_type_code(ins.filterType), ins.filterCut, ins.filterRes);
}

// One table in the 16 × 8 byte layout `AudioEngine::loadTable` reads. One packer for the note path's
// lazy push and the host's eager one (engine_consumer.h), so they cannot disagree about a byte.
template <typename Engine>
void push_table(Engine& engine, const Project& project, int tableId) {
    if (tableId < 0 || tableId >= static_cast<int>(project.tables.size())) return;
    const Table& table = project.tables[static_cast<size_t>(tableId)];
    uint8_t rowData[128] = {0};
    for (int rowIndex = 0; rowIndex < 16 && rowIndex < static_cast<int>(table.rows.size()); ++rowIndex) {
        const TableRow& row = table.rows[static_cast<size_t>(rowIndex)];
        uint8_t* p = rowData + rowIndex * 8;
        p[0] = static_cast<uint8_t>(row.transpose);
        p[1] = static_cast<uint8_t>(row.volume);     // -1 → 0xFF
        p[2] = static_cast<uint8_t>(row.fx1Type);
        p[3] = static_cast<uint8_t>(row.fx1Value);
        p[4] = static_cast<uint8_t>(row.fx2Type);
        p[5] = static_cast<uint8_t>(row.fx2Value);
        p[6] = static_cast<uint8_t>(row.fx3Type);
        p[7] = static_cast<uint8_t>(row.fx3Value);
    }
    engine.loadTable(tableId, rowData);
}

// ─── The NoteOn plan ─────────────────────────────────────────────────────────────────────────────
// The exact sequence of engine calls a NoteOn produces — a template, so a test can substitute a
// recorder and check which calls happen, in what order, and when a note is dropped.
// `tableLoaded` is the caller's POOL_TABLES-sized cache.
// `rootAudition` is preview-only and a parameter, not an Event field: a preview never reaches the
// bus. See derive_soundfont_note.
template <typename Engine>
void plan_note_on(Engine& engine, const Event& ev, const Project& project, const Routing& routing,
                  bool* tableLoaded, bool rootAudition = false) {
    const NoteOnPayload& n = ev.noteOn;
    const int instrumentId = ev.instrument;
    const int trackId      = ev.track;

    if (instrumentId < 0 || instrumentId >= static_cast<int>(project.instruments.size())) return;
    const Instrument& ins = project.instruments[instrumentId];

    // Keep the engine's tempo current so the table advance stays tempo-locked.
    const int tempo      = project.tempo;
    const int sampleRate = engine.getSampleRate();
    engine.setTempo(tempo);

    // Modulation / EQ / sends must reach the engine BEFORE the note triggers.
    auto push_instrument_state = [&]() { push_instrument_mod_eq_sends(engine, ins, tempo, sampleRate); };

    // Lazy table push from songcore's own project copy.
    // ⚠️ And every table the hit can be handed on to: an `INS` cell sends it to another instrument and
    // its table, and the engine walks that chain at the TRIGGER from its own copies — an unsent or
    // stale link routes on data no longer on screen. The cache doubles as the visited set, so each
    // table is sent at most once per invalidation.
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
            for (int rowIndex = 0; rowIndex < 16 && rowIndex < static_cast<int>(table.rows.size()); ++rowIndex) {
                const TableRow& row = table.rows[rowIndex];
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
            push_table(engine, project, tableId);
            tableLoaded[tableId] = true;
        }
    };

    const Program program = make_program(project, routing, instrumentId);
    // ⚠️ Pushed here, on the path about to read it, so the engine's copy of the instrument cannot be
    // stale.
    engine.setProgram(instrumentId, program, program.sliceMarkers, program.sliceCount);

    // ⚠️ The note is queued BY NUMBER and resolved at the hit (AudioEngine::scheduleProgramNote), so
    // nothing below derives a sound — only what must be in the engine before the voice starts.
    // The empty-slot gate stays here: a note on an empty slot must make NO engine calls at all.
    const int tableId = (n.tableId >= 0) ? n.tableId : instrumentId;

    if (ins.instrumentType == InstrumentType::SOUNDFONT) {
        if (!program.hasSoundfont || program.sfSlot < 0) return;   // never loaded — dropped

        push_instrument_state();
        // Every trigger: the TSF preset must carry the user's ATK/DEC/SUS/REL (or a KIL uses the SF2's
        // own, often instant, release), and instrumentParams[sfId] is reset to drive 0 + the right
        // filter (or stale sampler values bleed into the SF voice). Keyed by instrument id: two
        // instruments sharing one SF2 handle must stay isolated.
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
