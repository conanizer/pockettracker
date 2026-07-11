#ifndef POCKETTRACKER_SONGCORE_TRAVERSAL_H
#define POCKETTRACKER_SONGCORE_TRAVERSAL_H

// ─── Static song traversal ────────────────────────────────────────────────────────────────────────
//
// 1:1 port of core/logic/SongTraversal.kt: the shared song → chain → phrase → step walk used for
// STATIC analysis of the song (e.g. "which instruments does this row range use?"), applying the same
// bounds guards every time so the copies can't drift. This is deliberately NOT the live scheduler's
// walk (that goes by playback position / HOP / checkpoints — songcore S4) and NOT CLEAN's whole-song
// collector (which counts muted tracks as used and gathers more ref kinds).
//
// SongTraversal.kt is the executable spec. tools/ptresolve proves collect_used_instruments against a
// JVM golden over the real /testdata projects.

#include <functional>
#include <set>
#include "model.h"

namespace songcore {

// True when a step carries no note. Mirrors PhraseStep.isEmpty().
inline bool step_is_empty(const PhraseStep& step) { return step.note == Note::EMPTY(); }

// Visit every phrase step in song rows [start_row, end_row] (inclusive) across all 8 tracks, applying
// the same guards each time: a track row past the end, or one pointing at an empty chain/phrase slot,
// is skipped; muted tracks are skipped unless include_muted. Mirrors Project.forEachStepInSongRange().
template <typename Action>
inline void for_each_step_in_song_range(const Project& project, int start_row, int end_row,
                                        bool include_muted, Action action) {
    for (int row = start_row; row <= end_row; ++row) {
        for (const Track& track : project.tracks) {
            if (!include_muted && track.mute) continue;
            if (row >= static_cast<int>(track.chainRefs.size())) continue;
            int chainId = track.chainRefs[row];
            if (chainId < 0 || chainId > 255) continue;
            const Chain& chain = project.chains[chainId];
            for (int slot = 0; slot <= 15; ++slot) {
                int phraseId = chain.phraseRefs[slot];
                if (phraseId < 0 || phraseId > 255) continue;
                for (const PhraseStep& step : project.phrases[phraseId].steps) action(step);
            }
        }
    }
}

// Instrument IDs (0..127) used by any non-empty step in song rows [start_row, end_row]. Muted tracks
// are skipped (matching the render paths); out-of-pool instrument bytes from older files are ignored.
// Mirrors Project.collectUsedInstruments(). std::set keeps the ids sorted+unique, like the Kotlin Set
// once sorted for the golden.
inline std::set<int> collect_used_instruments(const Project& project, int start_row, int end_row) {
    std::set<int> used;
    int n = static_cast<int>(project.instruments.size());
    for_each_step_in_song_range(project, start_row, end_row, /*include_muted=*/false,
        [&](const PhraseStep& step) {
            if (!step_is_empty(step) && step.instrument >= 0 && step.instrument < n)
                used.insert(step.instrument);
        });
    return used;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_TRAVERSAL_H
