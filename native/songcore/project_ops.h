#pragma once
// The PROJECT screen's NEW and the two COMPACTs, on the document alone; telling the engine is the
// host's job (SongcoreHost::new_project / clean_inst).

#include <cstddef>
#include <deque>
#include <set>

#include "effects.h"
#include "effects/modules/delay-presets.h"
#include "effects/modules/reverb-presets.h"
#include "model.h"
#include "traversal.h"

namespace songcore {

/** What the SONG still reaches: chains → their phrases → the instruments those phrases NOTE. */
struct UsedRefs {
    std::set<int> chains;
    std::set<int> phrases;
    std::set<int> instruments;
};

inline UsedRefs collect_used_refs(const Project& project) {
    UsedRefs used;

    for (const Track& track : project.tracks)
        for (int ref : track.chainRefs)
            if (ref >= 0) used.chains.insert(ref);

    for (int chainId : used.chains) {
        if (chainId >= static_cast<int>(project.chains.size())) continue;
        for (int ref : project.chains[static_cast<size_t>(chainId)].phraseRefs)
            if (ref >= 0) used.phrases.insert(ref);
    }

    // ⚠️ Only steps WITH A NOTE count their instrument: a note-less step's instrument column never
    // triggers or configures anything, so an instrument referenced only there is unused.
    bool sawIns = false, sawRandom = false;
    for (int phraseId : used.phrases) {
        if (phraseId >= static_cast<int>(project.phrases.size())) continue;
        for (const PhraseStep& step : project.phrases[static_cast<size_t>(phraseId)].steps) {
            if (step_has_fx(step, FX_RND) || step_has_fx(step, FX_RNL)) sawRandom = true;
            if (step_has_note(step)) {
                used.instruments.insert(step.instrument);
                // An INS cell's instrument is played too, so COMPACT must not wipe it.
                const int ins = step_ins_instrument(step);
                if (ins >= 0) { used.instruments.insert(ins); sawIns = true; }
            }
        }
    }
    // ⚠️ A randomized INS can land on any instrument, so COMPACT keeps them all.
    if (sawIns && sawRandom)
        for (int i = 0; i < static_cast<int>(project.instruments.size()); ++i) used.instruments.insert(i);

    return used;
}

/** COMPACT → SEQ. Every chain and phrase the song does not reach goes back to factory. */
inline void clean_unused_seq(Project& project) {
    const UsedRefs used = collect_used_refs(project);

    for (int i = 0; i < static_cast<int>(project.chains.size()); ++i)
        if (used.chains.count(i) == 0) project.chains[static_cast<size_t>(i)] = Chain(i);

    for (int i = 0; i < static_cast<int>(project.phrases.size()); ++i)
        if (used.phrases.count(i) == 0) project.phrases[static_cast<size_t>(i)] = Phrase(i);
}

// COMPACT → INST: every instrument, table and groove the song does not reach back to factory.
// ⚠️ Tables reach tables (any effect is allowed in a table's FX), so the walk is TRANSITIVE.
inline void clean_unused_inst(Project& project) {
    const UsedRefs used = collect_used_refs(project);

    std::set<int> usedTables;
    std::set<int> usedGrooves;
    usedGrooves.insert(0);  // groove 0 is always kept

    // The reached phrases' own TBL / GRV effects.
    for (int phraseId : used.phrases) {
        if (phraseId >= static_cast<int>(project.phrases.size())) continue;
        for (const PhraseStep& step : project.phrases[static_cast<size_t>(phraseId)].steps) {
            const int types[3]  = {step.fx1Type,  step.fx2Type,  step.fx3Type};
            const int values[3] = {step.fx1Value, step.fx2Value, step.fx3Value};
            for (int k = 0; k < 3; ++k) {
                if (types[k] == FX_TBL) usedTables.insert(values[k] & 0xFF);
                if (types[k] == FX_GRV) usedGrooves.insert(values[k] & 0xFF);
            }
        }
    }

    // The implicit instrument→table mapping, plus an explicit override if the instrument carries one.
    for (int instId : used.instruments) {
        if (instId < 0 || instId >= static_cast<int>(project.instruments.size())) continue;
        usedTables.insert(instId);
        const int tableId = project.instruments[static_cast<size_t>(instId)].tableId;
        if (tableId >= 0) usedTables.insert(tableId);
    }

    // …and now transitively, through the tables' own rows.
    std::deque<int> worklist(usedTables.begin(), usedTables.end());
    while (!worklist.empty()) {
        const int tableId = worklist.front();
        worklist.pop_front();
        if (tableId < 0 || tableId >= static_cast<int>(project.tables.size())) continue;

        for (const TableRow& row : project.tables[static_cast<size_t>(tableId)].rows) {
            const int types[3]  = {row.fx1Type,  row.fx2Type,  row.fx3Type};
            const int values[3] = {row.fx1Value, row.fx2Value, row.fx3Value};
            for (int k = 0; k < 3; ++k) {
                if (types[k] == FX_TBL) {
                    const int ref = values[k] & 0xFF;
                    if (usedTables.insert(ref).second) worklist.push_back(ref);
                } else if (types[k] == FX_GRV) {
                    usedGrooves.insert(values[k] & 0xFF);
                }
            }
        }
    }

    // ⚠️ `Instrument(i)` keeps `sampleId = -1` where a fresh project has `i` — they differ on disk, and
    // the round-trip tests pin it.
    for (int i = 0; i < static_cast<int>(project.instruments.size()); ++i)
        if (used.instruments.count(i) == 0) project.instruments[static_cast<size_t>(i)] = Instrument(i);

    for (int i = 0; i < static_cast<int>(project.tables.size()); ++i)
        if (usedTables.count(i) == 0) project.tables[static_cast<size_t>(i)] = Table(i);

    for (int i = 0; i < static_cast<int>(project.grooves.size()); ++i)
        if (usedGrooves.count(i) == 0) project.grooves[static_cast<size_t>(i)] = Groove(i);
}

// A built-in preset onto the section's cells, and its name onto the row. Out of range does nothing.
inline void reverb_apply_bank(Project& p, int index) {
    if (index < 0 || index >= kReverbPresetCount) return;
    const ReverbPreset& r = kReverbPresets[index];
    p.reverbAlgo     = r.algo;
    p.reverbFeedback = r.decay;
    p.reverbSize     = r.room;
    p.reverbDamp     = r.damp;
    p.reverbPreDelay = r.pre;
    p.reverbWidth    = r.width;
    p.reverbMod      = r.mod;
    p.reverbPreset   = r.name;
}

inline void delay_apply_bank(Project& p, int index) {
    if (index < 0 || index >= kDelayPresetCount) return;
    const DelayPreset& d = kDelayPresets[index];
    p.delayPong       = d.pong;
    p.delaySync       = d.sync;
    p.delayTime       = d.time;
    p.delayFeedback   = d.feedback;
    p.delayTone       = d.tone;
    p.delayWobble     = d.wobble;
    p.delayReverbSend = d.reverbSend;
    p.delayPreset     = d.name;
}

// NEW: a fresh document. ⚠️ `version = 1`, not the struct's 0 — a 0 would load as a legacy file.
// It opens on the first built-in reverb and delay; the struct defaults stay what older files play with.
inline void new_project(Project& project) {
    project         = make_default_project();
    project.version = 1;
    reverb_apply_bank(project, 0);
    delay_apply_bank(project, 0);
}

}  // namespace songcore
