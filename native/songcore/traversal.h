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
// JVM golden over the real /tools/testdata projects.

#include <algorithm>
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
            if (chainId < 0 || chainId >= static_cast<int>(project.chains.size())) continue;
            const Chain& chain = project.chains[chainId];
            for (int slot = 0; slot < CHAIN_ROWS; ++slot) {
                int phraseId = chain_phrase_ref(chain, slot);
                if (phraseId < 0) continue;
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


// ─── Which mixer track a chain or a phrase belongs to ────────────────────────────────────────────
//
// The arrangement is the answer, not a remembered cursor: a chain reached by scrolling the 00..FF
// pool was never entered from a song cell, so there is nothing to remember and a memory-only answer
// would leave it on track 0. `preferred` is a TIE-BREAK only — it is honoured just when it is one of
// the tracks that actually hold the chain, so a stale one can never route a chain somewhere it does
// not live.
//
// A chain in no track at all answers 0: the pre-existing behaviour, and the arm a project that has
// never been arranged still takes.

/** The track holding `chainId`; `preferred` wins if it is one of them, else the lowest, else 0. */
inline int track_of_chain(const Project& project, int chainId, int preferred = -1) {
    if (chainId < 0) return 0;
    int lowest = -1;
    const int trackCount = static_cast<int>(project.tracks.size());
    for (int t = 0; t < trackCount; ++t) {
        const std::vector<int>& refs = project.tracks[t].chainRefs;
        if (std::find(refs.begin(), refs.end(), chainId) == refs.end()) continue;
        if (t == preferred) return t;
        if (lowest < 0) lowest = t;
    }
    return lowest >= 0 ? lowest : 0;
}

/**
 * The track holding `phraseId`, asked through the chain the user is looking at.
 *
 * The chain on screen is consulted FIRST and its answer is that chain's — the same phrase may sit in
 * five chains, and the one you are inside is the only one with a gesture behind it. Only when the
 * phrase is not in that chain does this fall back to the first place in the arrangement that reaches
 * it, in (track, song row, chain row) order.
 */
inline int track_of_phrase(const Project& project, int phraseId, int currentChainId,
                           int preferred = -1) {
    if (phraseId < 0) return 0;
    const int chainCount = static_cast<int>(project.chains.size());
    if (currentChainId >= 0 && currentChainId < chainCount) {
        const Chain& chain = project.chains[currentChainId];
        for (int row = 0; row < CHAIN_ROWS; ++row)
            if (chain_phrase_ref(chain, row) == phraseId)
                return track_of_chain(project, currentChainId, preferred);
    }
    const int trackCount = static_cast<int>(project.tracks.size());
    for (int t = 0; t < trackCount; ++t) {
        for (int chainId : project.tracks[t].chainRefs) {
            if (chainId < 0 || chainId >= chainCount) continue;
            const Chain& chain = project.chains[chainId];
            for (int row = 0; row < CHAIN_ROWS; ++row)
                if (chain_phrase_ref(chain, row) == phraseId) return t;
        }
    }
    return 0;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_TRAVERSAL_H
