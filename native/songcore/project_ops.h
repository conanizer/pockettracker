#pragma once
/**
 * native/songcore/project_ops.h — NEW, and the two COMPACT operations.
 *
 * The surgery behind the PROJECT screen's NEW button and its SEQ / INST buttons, ported 1:1 from
 * TrackerController.collectUsedRefs / cleanUnusedSeq / cleanUnusedInst.
 *
 * PURE: each verb takes a `Project&` and nothing else. Everything the ENGINE must be told afterwards
 * — reload its sample pool, drop its table cache, re-push the live params — is the HOST's job (see
 * SongcoreHost::new_project / clean_inst), not this file's. That split is not tidiness: it is what
 * lets `ptinput` and `ptdispatch` drive a COMPACT with no audio device in the process at all, which
 * is how the C++ side of these gets measured against Kotlin's.
 */

#include <cstddef>
#include <deque>
#include <set>

#include "effects.h"
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

    // ⚠️ Only steps WITH A NOTE count their instrument. A note-less step's instrument number never
    // triggers or configures anything at playback (scheduleStepWithEffects reads it only when
    // hasNote), so an instrument referenced solely by note-less steps is genuinely unused and gets
    // compacted away. Kotlin's rule, and its reasoning.
    for (int phraseId : used.phrases) {
        if (phraseId >= static_cast<int>(project.phrases.size())) continue;
        for (const PhraseStep& step : project.phrases[static_cast<size_t>(phraseId)].steps)
            if (!step_is_empty(step)) used.instruments.insert(step.instrument);
    }

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

/**
 * COMPACT → INST. Every instrument, table and groove the song does not reach goes back to factory.
 *
 * Tables are the subtle half. A table is reachable four ways — a phrase's TBL effect, the IMPLICIT
 * instrument→table mapping (instrument i uses table i), an instrument's explicit `tableId` override,
 * and ⚠️ from INSIDE ANOTHER TABLE, because the editor allows every effect in a table's own FX
 * columns. So the walk is TRANSITIVE: a groove used only from within a table, or a table chained
 * from another via a TBL row, must not be wiped out from under a still-referenced table.
 */
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

    // ⚠️ `Instrument(i)` is the FIELD default, which leaves `sampleId = -1` — where the same slot in a
    // FRESH project has `sampleId = i`, because the Project factory's Array(128) initializer sets it
    // (model.h). The two are therefore NOT the same object, and with `encodeDefaults = false` they do
    // not even serialize alike: -1 IS the field default and is omitted, i is not and is written. This
    // is Kotlin's `Instrument(id = i)` exactly, quirk included — a compacted project and a new project
    // differ on disk, and ptroundtrip would catch a "tidy-up" here as a byte divergence.
    for (int i = 0; i < static_cast<int>(project.instruments.size()); ++i)
        if (used.instruments.count(i) == 0) project.instruments[static_cast<size_t>(i)] = Instrument(i);

    for (int i = 0; i < static_cast<int>(project.tables.size()); ++i)
        if (usedTables.count(i) == 0) project.tables[static_cast<size_t>(i)] = Table(i);

    for (int i = 0; i < static_cast<int>(project.grooves.size()); ++i)
        if (usedGrooves.count(i) == 0) project.grooves[static_cast<size_t>(i)] = Groove(i);
}

/**
 * NEW. A fresh document, at the CURRENT file-format version.
 *
 * ⚠️ `version = 1`, not the struct's 0. Kotlin writes `Project(version = 1)` and 0 means
 * "pre-versioning" — a file written by a build that had no version field. A new project is not that,
 * and the emitter only writes `version` when it is non-zero, so getting this wrong would silently
 * stamp every newly created song as a legacy file and send it back through `migrate` on next load.
 */
inline void new_project(Project& project) {
    project         = make_default_project();
    project.version = 1;
}

}  // namespace songcore
