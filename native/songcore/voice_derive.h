#ifndef POCKETTRACKER_SONGCORE_VOICE_DERIVE_H
#define POCKETTRACKER_SONGCORE_VOICE_DERIVE_H

// Everything between "a NoteOn happened" and "call the engine", as pure functions so it can be tested.
// ⚠️ Keep each expression's operation order: a 1-ULP drift changes every rendered byte.

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

// Flatten one instrument into the scalars a note is derived from.
// ⚠️ `sliceMarkers` points INTO `ins`: valid only while it is.
inline Program make_program(const Instrument& ins, float sampleRateRatio, int sfSlot) {
    Program p;
    p.type            = (ins.instrumentType == InstrumentType::SOUNDFONT) ? PROGRAM_SOUNDFONT
                      : (ins.instrumentType == InstrumentType::EXTERNAL) ? PROGRAM_EXTERNAL
                      : (ins.instrumentType == InstrumentType::MACRO)    ? PROGRAM_MACRO
                                                                         : PROGRAM_SAMPLER;
    p.hasSample       = ins.sampleFilePath.has_value();
    p.hasSoundfont    = ins.soundfontPath.has_value();
    p.sampleId        = ins.id;
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
    if (p.type == PROGRAM_MACRO) {
        // No file behind it, so nothing of one: a slot kept from its days as a sampler may still
        // carry slices.
        p.hasSample       = false;
        p.sampleRateRatio = 1.0f;
        p.slicingMode     = 0;
        p.sliceMarkers    = nullptr;
        p.sliceCount      = 0;
        p.macroModel      = ins.macroModel;
        p.macroTimbre     = ins.macroTimbre;
        p.macroColor      = ins.macroColor;
    }
    return p;
}

/** The Program for instrument `id`, with the sample-rate ratio and SF slot the loaders resolved. */
inline Program make_program(const Project& project, const Routing& routing, int id) {
    if (id < 0 || id >= static_cast<int>(project.instruments.size())) return Program{};
    const Instrument& ins = project.instruments[static_cast<size_t>(id)];
    return make_program(ins, routing.sampleRateRatio[id], routing.sfSlot[id]);
}

// The four modulation slots as the engine runs them.
struct ModPushes {
    EngineModSlot slots[4];
    bool anyActive = false;   // false → the caller instead issues clearInstrumentModulation(id)
};

// ⚠️ "off" is the answer for any name this does not know, so a newer build's project opens with the
// loop lost rather than mis-read. That failure is SILENT — know it before adding a mode.
inline int loop_mode_code(const std::string& mode) {
    if (mode == "fwd") return 1;
    if (mode == "png") return 2;
    if (mode == "osc") return 3;   // a forward loop, scan rate retuned — audio-defs.h
    return 0;
}

// The engine's filter for the stored name; any other name is OFF.
inline FilterType filter_type_code(const std::string& type) {
    if (type == "lp") return FILTER_LP;
    if (type == "hp") return FILTER_HP;
    if (type == "bp") return FILTER_BP;
    return FILTER_OFF;
}

// The modulation push. A slot with no destination is cleared, as are NONE and TRACKING.
inline ModPushes derive_mod_pushes(const Instrument& ins, int tempo, int sampleRate) {
    ModPushes out;
    const float framesPerTic = frames_per_tic_f(tempo, sampleRate);
    const auto  tics         = [framesPerTic](int v) { return tics_to_frames(v, framesPerTic); };

    for (int i = 0; i < 4 && i < static_cast<int>(ins.modSlots.size()); ++i) {
        const ModSlot& slot = ins.modSlots[i];
        if (slot.dest == ModDest::NONE) continue;
        EngineModSlot& p = out.slots[i];

        switch (slot.type) {
            case ModType::AHD:
            case ModType::DRUM:
                p.attackSamples = tics(slot.attack);
                p.holdSamples   = tics(slot.hold);
                p.decaySamples  = tics(slot.decay);
                break;
            case ModType::ADSR:
            case ModType::TRIG:
                p.attackSamples  = tics(slot.attack);
                p.decaySamples   = tics(slot.decay);
                p.releaseSamples = tics(slot.release);
                p.sustainLevel   = slot.sustain / 255.0f;
                break;
            case ModType::LFO:
                p.lfoHz       = lfo_hz(slot.lfoFreq);
                p.oscShape    = slot.oscShape;
                p.lfoTrigMode = slot.lfoTrigMode;
                break;
            case ModType::SCALAR:   // its AMT is the output
                break;
            default:
                continue;
        }
        p.type   = slot.type;
        p.dest   = slot.dest;
        p.amount = slot.amount / 255.0f;
        out.anyActive = true;
    }
    return out;
}

// What must reach the engine BEFORE a voice triggers: the modulation slots and the EQ/send routing.
template <typename Engine>
void push_instrument_mod_eq_sends(Engine& engine, const Instrument& ins, int tempo, int sampleRate) {
    const ModPushes m = derive_mod_pushes(ins, tempo, sampleRate);
    for (int i = 0; i < 4; ++i) engine.setInstrumentModulation(ins.id, i, m.slots[i]);
    if (!m.anyActive) engine.clearInstrumentModulation(ins.id);
    engine.setInstrumentEqSlot(ins.id, ins.eqSlot);
    engine.setInstrumentSendLevels(ins.id, ins.reverbSend, ins.delaySend);
}

// The sample-playback window, loop, drive/crush/downsample and filter, to engine slot `slot` (the
// instrument's id but for the sample editor's scratch slot). SoundFont and sampler instruments push the
// same params.
template <typename Engine>
void push_instrument_playback_params(Engine& engine, const Instrument& ins, int slot) {
    engine.setInstrumentParams(slot, ins.sampleStart, ins.sampleEnd, ins.reverse,
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
        const TableRow row = as_played(table.rows[static_cast<size_t>(rowIndex)]);
        uint8_t* p = rowData + rowIndex * 8;
        p[0] = static_cast<uint8_t>(row.transpose);
        p[1] = static_cast<uint8_t>(row.volume);     // -1 → 0xFF
        for (int i = 0; i < FX_SLOTS; ++i) {
            p[2 + 2 * i] = static_cast<uint8_t>(row.fx[static_cast<size_t>(i)].type);
            p[3 + 2 * i] = static_cast<uint8_t>(row.fx[static_cast<size_t>(i)].value);
        }
    }
    engine.loadTable(tableId, rowData);
}

// The engine calls a NoteOn makes, in order. `tableLoaded` is the caller's cache; `rootAudition` is
// preview-only (a preview never reaches the bus).
template <typename Engine>
void plan_note_on(Engine& engine, const Event& ev, const Project& project, const Routing& routing,
                  bool* tableLoaded, bool rootAudition = false) {
    const NoteOnPayload& n = ev.noteOn;
    const int instrumentId = ev.instrument;
    const int trackId      = ev.track;

    if (instrumentId < 0 || instrumentId >= static_cast<int>(project.instruments.size())) return;
    const Instrument& ins = project.instruments[instrumentId];

    // Keep the engine's tempo current so the table advance stays tempo-locked.
    const int tempo      = tempo_in_force(project);
    const int sampleRate = engine.getSampleRate();
    engine.setTempo(tempo);

    // Modulation / EQ / sends must reach the engine BEFORE the note triggers.
    auto push_instrument_state = [&]() { push_instrument_mod_eq_sends(engine, ins, tempo, sampleRate); };

    // Lazy table push — ⚠️ and every table an `INS` cell can hand the hit to, since the engine follows
    // them from its own copies. The cache doubles as the visited set.
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
                for (const FxSlot& f : row.fx) {
                    if (f.type != FX_INS) continue;
                    const int next = f.value & 0x7F;
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

    // The note is queued BY NUMBER and resolved at the hit; below is only what must be in the engine
    // first. A note on an empty slot makes no engine call at all.
    const int tableId = (n.tableId >= 0) ? n.tableId : instrumentId;

    if (ins.instrumentType == InstrumentType::SOUNDFONT) {
        if (!program.hasSoundfont || program.sfSlot < 0) return;   // never loaded — dropped

        push_instrument_state();
        // Every trigger: the user's ATK/DEC/SUS/REL, keyed by instrument so two sharing one SF2 stay apart.
        const SFOverrides& ov = ins.sfOverrides;
        engine.setSoundfontEnvelopeOverride(ins.id, ov.ampAttack, ov.ampDecay, ov.ampSustain, ov.ampRelease);
        push_instrument_playback_params(engine, ins, ins.id);

        ensure_table_loaded(tableId);
        engine.requestResume();
        engine.scheduleProgramNote(ev.frame, trackId, instrumentId, n, tempo, rootAudition);
        return;
    }

    // ── Sampler path ─────────────────────────────────────────────────────────────────────────────
    // sampleFilePath == null — the empty-slot convention. A MACRO program has no file to lack.
    if (!program.hasSample && !program_is_macro(program)) return;

    ensure_table_loaded(tableId);
    push_instrument_state();

    engine.requestResume();
    engine.scheduleProgramNote(ev.frame, trackId, instrumentId, n, tempo, rootAudition);
}


}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_VOICE_DERIVE_H
