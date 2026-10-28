#ifndef POCKETTRACKER_SONGCORE_TRAVERSAL_H
#define POCKETTRACKER_SONGCORE_TRAVERSAL_H

// The song → chain → phrase → step walk for STATIC analysis ("which instruments does this range use?"),
// bounds guards in one place. Not the scheduler's walk.

#include <algorithm>
#include <functional>
#include <set>
#include "effects.h"   // step_ins_instrument
#include "model.h"

namespace songcore {

// True when a step's note cell is blank. ⚠️ An OFF is not blank, though it plays nothing.
inline bool step_is_empty(const PhraseStep& step) { return step.note == Note::EMPTY(); }

// Every phrase step in song rows [start_row, end_row]; inaudible tracks skipped unless asked.
// Render-side only: live playback never consults audibility.
template <typename Action>
inline void for_each_step_in_song_range(const Project& project, int start_row, int end_row,
                                        bool include_inaudible, Action action) {
    for (int row = start_row; row <= end_row; ++row) {
        for (const Track& track : project.tracks) {
            if (!include_inaudible && track.mute) continue;
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
// are skipped, as in the render; out-of-pool instrument bytes are ignored. Sorted and unique.
inline std::set<int> collect_used_instruments(const Project& project, int start_row, int end_row) {
    std::set<int> used;
    int n = static_cast<int>(project.instruments.size());
    bool sawIns = false, sawRandom = false;
    for_each_step_in_song_range(project, start_row, end_row, /*include_inaudible=*/false,
        [&](const PhraseStep& step) {
            if (step_has_fx(step, FX_RND) || step_has_fx(step, FX_RNL)) sawRandom = true;
            if (!step_has_note(step)) return;
            if (step.instrument >= 0 && step.instrument < n) used.insert(step.instrument);
            // An INS cell plays its instrument instead.
            const int ins = step_ins_instrument(step);
            if (ins >= 0 && ins < n) { used.insert(ins); sawIns = true; }
        });
    // ⚠️ A randomized INS can land on any instrument, and one missing here exports with default params.
    if (sawIns && sawRandom)
        for (int i = 0; i < n; ++i) used.insert(i);
    return used;
}


// ─── Which mixer track a chain or phrase belongs to — the arrangement answers, not a cursor ──────

// The track holding `chainId`; `preferred` wins if it is one of them, else the lowest, else 0.
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

// The track holding `phraseId`, through the chain on screen first, else the first place that reaches it.
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

// ─── Song-relative navigation: B+DPAD walks the ARRANGEMENT, clamping (nothing that way = unchanged)

// The chain in song cell (track, row), or −1. ⚠️ A track's chainRefs may be SHORTER than the song.
inline int chain_at(const Project& project, int track, int songRow) {
    if (track < 0 || track >= static_cast<int>(project.tracks.size()) || songRow < 0) return -1;
    const std::vector<int>& refs = project.tracks[static_cast<size_t>(track)].chainRefs;
    if (songRow >= static_cast<int>(refs.size())) return -1;
    const int id = refs[static_cast<size_t>(songRow)];
    return (id >= 0 && id < static_cast<int>(project.chains.size())) ? id : -1;
}

/** The phrase in row `chainRow` of chain `chainId`, or −1. */
inline int phrase_at(const Project& project, int chainId, int chainRow) {
    if (chainId < 0 || chainId >= static_cast<int>(project.chains.size())) return -1;
    return chain_phrase_ref(project.chains[static_cast<size_t>(chainId)], chainRow);
}

// The nearest track sideways whose cell in `songRow` holds a chain (with a phrase at
// `requirePhraseAtRow`, if ≥ 0).
inline int next_song_cell_h(const Project& project, int songRow, int track, int delta,
                            int requirePhraseAtRow = -1) {
    const int trackCount = static_cast<int>(project.tracks.size());
    for (int t = track + delta; t >= 0 && t < trackCount; t += delta) {
        const int chainId = chain_at(project, t, songRow);
        if (chainId < 0) continue;
        if (requirePhraseAtRow >= 0 && phrase_at(project, chainId, requirePhraseAtRow) < 0) continue;
        return t;
    }
    return track;
}

// The nearest song row above/below whose cell in `track` holds a chain.
inline int next_song_cell_v(const Project& project, int songRow, int track, int delta) {
    if (track < 0 || track >= static_cast<int>(project.tracks.size())) return songRow;
    const int last =
        static_cast<int>(project.tracks[static_cast<size_t>(track)].chainRefs.size()) - 1;
    for (int r = songRow + delta; r >= 0 && r <= last; r += delta)
        if (chain_at(project, track, r) >= 0) return r;
    return songRow;
}

// The nearest row of `chainId` above/below holding a phrase; clamps, or wraps for the plain UP/DOWN spill.
inline int next_chain_row(const Project& project, int chainId, int fromRow, int delta,
                          bool wrap = false) {
    if (chainId < 0 || chainId >= static_cast<int>(project.chains.size())) return fromRow;
    const Chain& chain = project.chains[static_cast<size_t>(chainId)];
    for (int step = 1; step <= CHAIN_ROWS; ++step) {
        int r = fromRow + delta * step;
        if (r < 0 || r >= CHAIN_ROWS) {
            if (!wrap) return fromRow;
            r = ((r % CHAIN_ROWS) + CHAIN_ROWS) % CHAIN_ROWS;
        }
        if (r == fromRow) break;                       // a full lap: nothing else is filled
        if (chain_phrase_ref(chain, r) >= 0) return r;
    }
    return fromRow;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_TRAVERSAL_H
