#pragma once

// ─── The song-relative pointer (SETTINGS → NAV = SONG) ───────────────────────────────────────────
//
// ⭐⭐ THE POINTER IS THE CELL, NOT THE ITEM.
//
// Under NAV = POOL, `currentChain` and `currentPhrase` ARE the state (B+LEFT/RIGHT scroll the pool).
// Under NAV = SONG they are a READING of the cursors:
//
//     songRow, track := the SONG cursor    (songCursorRow, songCursorColumn − 1)
//     chainRow       := the CHAIN cursor   (chainCursorRow)
//
//     currentChain   = tracks[track].chainRefs[songRow]
//     currentPhrase  = chains[currentChain].phraseRefs[chainRow]
//
// No new AppState field: `go_to_screen` already saves and restores those cursors. And it is the only
// shape that behaves right — a B+UP onto the SAME chain has still MOVED (the cell moved), so the next
// B+RIGHT goes somewhere else.
// ⚠️ The two current* fields are still WRITTEN, derived by `refresh_song_relative_refs` wherever the
// pointer can move (`go_to_screen`, the B+D-pad walks, the PHRASE spill), because many sites index
// `project.chains[currentChain]` directly.
// ⚠️ An EMPTY cell leaves the old value alone: `currentChain` is read unguarded, so −1 would be an
// out-of-bounds read. The entry gate keeps the user off empty cells; this is the backup.

#include <algorithm>

#include "songcore/traversal.h"
#include "ui/app_state.h"
#include "ui/screen.h"

namespace pt::ui {

/**
 * The three pointer components, LIVE-CURSOR AWARE: `go_to_screen` writes the saved copy on the way OUT,
 * so on the screen that owns it the live cursor is the answer. The setter below applies the same test —
 * reader and writer must agree on which copy is authoritative.
 */
inline int pointer_song_row(const AppState& s) {
    return (s.currentScreen == ScreenType::SONG) ? s.cursorRow : s.songCursorRow;
}
inline int pointer_track(const AppState& s) {
    return ((s.currentScreen == ScreenType::SONG) ? s.cursorColumn : s.songCursorColumn) - 1;
}
inline int pointer_chain_row(const AppState& s) {
    return (s.currentScreen == ScreenType::CHAIN) ? s.cursorRow : s.chainCursorRow;
}

/** Move the pointer's song cell. `track` is 0-based; the cursor column it writes is 1-based. */
inline void set_pointer_song_cell(AppState& s, int songRow, int track) {
    if (s.currentScreen == ScreenType::SONG) {
        s.cursorRow    = songRow;
        s.cursorColumn = track + 1;
    } else {
        s.songCursorRow    = songRow;
        s.songCursorColumn = track + 1;
    }
    // The SONG viewport must be able to show the cell on arrival (`go_to_screen` scrolls to the cursor).
    scroll_song_to_row(s, songRow);
}

/** Move the pointer's chain row. */
inline void set_pointer_chain_row(AppState& s, int chainRow) {
    if (s.currentScreen == ScreenType::CHAIN) s.cursorRow      = chainRow;
    else                                      s.chainCursorRow = chainRow;
}

/** Re-read `currentChain` / `currentPhrase` off the pointer. A no-op under NAV = POOL. */
inline void refresh_song_relative_refs(AppState& s) {
    if (!s.settings.navSongRelative || s.project == nullptr) return;
    const songcore::Project& p = *s.project;

    const int chainId = songcore::chain_at(p, pointer_track(s), pointer_song_row(s));
    if (chainId >= 0) {
        s.currentChain    = chainId;
        s.lastEditedChain = chainId;
    }
    const int phraseId = songcore::phrase_at(p, s.currentChain, pointer_chain_row(s));
    if (phraseId >= 0) {
        s.currentPhrase    = phraseId;
        s.lastEditedPhrase = phraseId;
    }
}

/**
 * May R+RIGHT enter `to`? CHAIN needs a filled song cell under the cursor, PHRASE a phrase at the chain
 * row. ⚠️ R+RIGHT only — gating R+LEFT or R+UP/DOWN could strand the user. A refused press does nothing;
 * the SONG cell is the explanation.
 */
inline bool song_relative_entry_allowed(const AppState& s, ScreenType to) {
    if (!s.settings.navSongRelative || s.project == nullptr) return true;
    const songcore::Project& p = *s.project;
    switch (to) {
        case ScreenType::CHAIN:
            return songcore::chain_at(p, pointer_track(s), pointer_song_row(s)) >= 0;
        case ScreenType::PHRASE:
            return songcore::phrase_at(p, s.currentChain, pointer_chain_row(s)) >= 0;
        default:
            return true;
    }
}

/**
 * Put the pointer on a cell that exists, and re-read the refs. A `.ptp` loaded under NAV = SONG can
 * leave the remembered cell empty, and the gate would then refuse CHAIN with nothing on screen to say
 * why — so the pointer moves to the first filled cell (song row outer, track inner), and the chain row
 * likewise. An empty arrangement leaves it alone.
 */
inline void clamp_song_pointer(AppState& s) {
    if (!s.settings.navSongRelative || s.project == nullptr) return;
    const songcore::Project& p = *s.project;

    if (songcore::chain_at(p, pointer_track(s), pointer_song_row(s)) < 0) {
        const int trackCount = static_cast<int>(p.tracks.size());
        int longest = 0;
        for (const songcore::Track& t : p.tracks)
            longest = std::max(longest, static_cast<int>(t.chainRefs.size()));
        bool found = false;
        for (int row = 0; row < longest && !found; ++row)
            for (int t = 0; t < trackCount && !found; ++t)
                if (songcore::chain_at(p, t, row) >= 0) {
                    set_pointer_song_cell(s, row, t);
                    found = true;
                }
    }

    // …and the chain row: a real chain with an empty row refuses PHRASE just as flatly.
    const int chainId = songcore::chain_at(p, pointer_track(s), pointer_song_row(s));
    if (chainId >= 0 && songcore::phrase_at(p, chainId, pointer_chain_row(s)) < 0) {
        for (int row = 0; row < songcore::CHAIN_ROWS; ++row)
            if (songcore::phrase_at(p, chainId, row) >= 0) { set_pointer_chain_row(s, row); break; }
    }

    refresh_song_relative_refs(s);
}

}  // namespace pt::ui
