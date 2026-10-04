#ifndef POCKETTRACKER_SONGCORE_TRAVERSAL_H
#define POCKETTRACKER_SONGCORE_TRAVERSAL_H

// ─── Static song traversal ────────────────────────────────────────────────────────────────────────
//
// The shared song → chain → phrase → step walk for STATIC analysis ("which instruments does this row
// range use?"), with the bounds guards in one place. Not the live scheduler's walk (playback position,
// HOP, checkpoints), and not COMPACT's collector (which counts muted tracks and more ref kinds).

#include <algorithm>
#include <functional>
#include <set>
#include "effects.h"   // step_ins_instrument
#include "model.h"

namespace songcore {

// True when a step carries no note.
inline bool step_is_empty(const PhraseStep& step) { return step.note == Note::EMPTY(); }

// Visit every phrase step in song rows [start_row, end_row] across all 8 tracks. Rows past a track's
// end and empty chain/phrase slots are skipped; inaudible tracks too, unless include_inaudible.
// ⚠️ RENDER-SIDE ONLY — live playback pushes every instrument and never consults audibility (mute is a
// mixer gate there).
template <typename Action>
inline void for_each_step_in_song_range(const Project& project, int start_row, int end_row,
                                        bool include_inaudible, Action action) {
    for (int row = start_row; row <= end_row; ++row) {
        for (const Track& track : project.tracks) {
            if (!include_inaudible && !track_audible(project, track)) continue;
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
            if (step_is_empty(step)) return;
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


// ─── Which mixer track a chain or a phrase belongs to ────────────────────────────────────────────
//
// The arrangement answers, not a remembered cursor — a chain reached from the pool was never entered
// from a song cell. `preferred` only breaks a tie among tracks that really hold the chain. A chain in
// no track answers 0.

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
 * The track holding `phraseId`, asked through the chain on screen first — the same phrase may sit in
 * several chains, and only the one you are inside reflects a gesture. Otherwise the first place in
 * the arrangement that reaches it, in (track, song row, chain row) order.
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

// ─── Walking the song grid ───────────────────────────────────────────────────────────────────────
//
// The pure half of song-relative navigation (SETTINGS → NAV = SONG): B+DPAD walks the ARRANGEMENT,
// not the 00..FF pool. They all CLAMP — nothing that way returns the index unchanged ("the press did
// nothing"). The one exception is `wrap` on `next_chain_row`, for the plain UP/DOWN spill.

/** The chain in song cell (track, row), or −1. ⚠️ A track's chainRefs may be SHORTER than the song. */
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

/**
 * The nearest track to the side of `track` whose song cell in `songRow` holds a chain — and, when
 * `requirePhraseAtRow >= 0`, whose chain holds a phrase at that row. One predicate for both reasons
 * the PHRASE screen skips a track.
 */
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

/**
 * The nearest song row above/below `songRow` whose cell in `track` holds a chain; gaps are skipped.
 * Bounded by the track's own chainRefs length — past it every cell is empty.
 */
inline int next_song_cell_v(const Project& project, int songRow, int track, int delta) {
    if (track < 0 || track >= static_cast<int>(project.tracks.size())) return songRow;
    const int last =
        static_cast<int>(project.tracks[static_cast<size_t>(track)].chainRefs.size()) - 1;
    for (int r = songRow + delta; r >= 0 && r <= last; r += delta)
        if (chain_at(project, track, r) >= 0) return r;
    return songRow;
}

/**
 * The nearest row of `chainId` above/below `fromRow` holding a phrase. B+UP/DOWN CLAMPS; the plain
 * UP/DOWN spill off step 00 or 0F WRAPS (`wrap`). Returns `fromRow` when nothing else is filled.
 */
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
