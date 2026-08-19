#pragma once

// ─── The song-relative pointer (SETTINGS → NAV = SONG) ───────────────────────────────────────────
//
// ⭐⭐ THE POINTER IS THE CELL, NOT THE ITEM.
//
// Under NAV = POOL — what the app has always done — `currentChain` and `currentPhrase` ARE the state:
// B+LEFT/RIGHT scrolls them over the whole 256-slot pool and nothing relates them to the arrangement.
// Under NAV = SONG they stop being state and become a READING of one:
//
//     songRow, track := the SONG cursor    (songCursorRow, songCursorColumn − 1)
//     chainRow       := the CHAIN cursor   (chainCursorRow)
//
//     currentChain   = tracks[track].chainRefs[songRow]
//     currentPhrase  = chains[currentChain].phraseRefs[chainRow]
//
// ⭐ NO NEW AppState FIELD, and that is the design rather than a saving. `go_to_screen` already saves
// and restores those three on every screen change, so the pointer is maintained by code that exists.
// It is also the only shape that gives the behaviour asked for: a B+UP that lands on the SAME chain
// has still MOVED, because what moved is the cell — so a later B+RIGHT goes somewhere else. Anything
// that stored "which chain am I looking at" would have to carry the cell beside it as a second,
// desyncable copy, which is the shape `CLAUDE.md` names.
//
// ⚠️ THE TWO current* FIELDS ARE STILL WRITTEN — they are not deleted, they are DERIVED. Thirty-odd
// sites index `project.chains[currentChain]` directly, and re-deriving at each of them is the bug this
// file exists to avoid. `refresh_song_relative_refs` runs at the places the pointer CAN move —
// `go_to_screen`, the B+D-pad walks, the PHRASE spill — each of which is itself the funnel for its
// kind of move rather than one site among many.
//
// ⚠️ AN EMPTY CELL LEAVES THE OLD VALUE ALONE. `currentChain` indexes `project.chains` unguarded
// wherever it is read, so writing −1 here would be an out-of-bounds read on the next frame. The entry
// gate is what keeps the user off an empty cell; this is the belt to its braces.

#include <algorithm>

#include "songcore/traversal.h"
#include "ui/app_state.h"
#include "ui/screen.h"

namespace pt::ui {

/**
 * The three pointer components, LIVE-CURSOR AWARE.
 *
 * ⚠️ The saved copy is one screen change behind while you are standing ON the screen that owns it —
 * `go_to_screen` writes `songCursorRow` on the way OUT of SONG, not on every cursor step. So the
 * answer is the live cursor there and the saved field everywhere else, and the setter below mirrors
 * the same test: a reader and a writer that disagree about which copy is authoritative is the whole
 * class of bug this file is here to avoid.
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
    // Whichever copy moved, the SONG viewport must be able to show the cell when the user arrives on
    // it — `go_to_screen` scrolls to `cursorRow`, and that is restored from `songCursorRow`.
    scroll_song_to_row(s, songRow);
}

/** Move the pointer's chain row. */
inline void set_pointer_chain_row(AppState& s, int chainRow) {
    if (s.currentScreen == ScreenType::CHAIN) s.cursorRow      = chainRow;
    else                                      s.chainCursorRow = chainRow;
}

/**
 * Re-read `currentChain` / `currentPhrase` off the pointer. A no-op under NAV = POOL, which is what
 * makes every existing caller and every existing golden indifferent to this file.
 */
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
 * May R+RIGHT enter `to`? The "you cannot get there until you put it in the song" half of the
 * ruleset: CHAIN needs a filled song cell under the cursor, PHRASE needs a phrase at the chain row.
 *
 * ⚠️ R+RIGHT ONLY. R+LEFT and R+UP/DOWN are never gated — they move OUT of a screen or between rows
 * of the grid, and refusing them could strand the user on a screen with no way back. A refused press
 * does nothing at all (LGPT's own answer): the SONG cell you are standing on is the explanation.
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
 * Put the pointer on a cell that exists, and re-read the refs from it.
 *
 * ⚠️ A `.ptp` loaded under NAV = SONG can leave the remembered song cell on an empty one. The entry
 * gate then refuses CHAIN — correctly — and the user is stuck on SONG until they find a filled cell
 * themselves, with nothing on screen saying why. So the pointer is DERIVED at load: the first filled
 * cell in reading order (song row outer, track inner), and the chain row likewise.
 *
 * An arrangement with nothing in it at all leaves the pointer alone: there is no better cell to name,
 * and the gate is then doing exactly what it should.
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

    // …and the chain row, for the same reason one level down: a pointer whose chain is real but whose
    // row is empty refuses PHRASE just as flatly.
    const int chainId = songcore::chain_at(p, pointer_track(s), pointer_song_row(s));
    if (chainId >= 0 && songcore::phrase_at(p, chainId, pointer_chain_row(s)) < 0) {
        for (int row = 0; row < songcore::CHAIN_ROWS; ++row)
            if (songcore::phrase_at(p, chainId, row) >= 0) { set_pointer_chain_row(s, row); break; }
    }

    refresh_song_relative_refs(s);
}

}  // namespace pt::ui
