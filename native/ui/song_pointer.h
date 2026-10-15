#pragma once

// NAV = SONG: the pointer is the cell, not the item. `currentChain` and `currentPhrase` are read off
// the SONG and CHAIN cursors:
//     currentChain  = tracks[songCursorColumn − 1].chainRefs[songCursorRow]
//     currentPhrase = chains[currentChain].phraseRefs[chainCursorRow]
// (Under NAV = POOL they are the state.) They are still written, by `refresh_song_relative_refs`,
// wherever the pointer moves. ⚠️ An empty cell leaves the old value: −1 would be read out of bounds.

#include <algorithm>

#include "songcore/traversal.h"
#include "ui/app_state.h"
#include "ui/screen.h"

namespace pt::ui {

// On the screen that owns a cursor the live one is the answer; elsewhere the saved copy. The setters
// apply the same test.
inline int pointer_song_row(const AppState& s) {
    return (s.currentScreen == ScreenType::SONG) ? s.cursorRow : s.songCursorRow;
}
inline int pointer_track(const AppState& s) {
    return ((s.currentScreen == ScreenType::SONG) ? s.cursorColumn : s.songCursorColumn) - 1;
}
inline int pointer_chain_row(const AppState& s) {
    return (s.currentScreen == ScreenType::CHAIN) ? s.cursorRow : s.chainCursorRow;
}

// `track` is 0-based; the cursor column it writes is 1-based.
inline void set_pointer_song_cell(AppState& s, int songRow, int track) {
    if (s.currentScreen == ScreenType::SONG) {
        s.cursorRow    = songRow;
        s.cursorColumn = track + 1;
    } else {
        s.songCursorRow    = songRow;
        s.songCursorColumn = track + 1;
    }
    scroll_song_to_row(s, songRow);
}

inline void set_pointer_chain_row(AppState& s, int chainRow) {
    if (s.currentScreen == ScreenType::CHAIN) s.cursorRow      = chainRow;
    else                                      s.chainCursorRow = chainRow;
}

// Re-read `currentChain` / `currentPhrase` off the pointer; a no-op under NAV = POOL.
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

// May R+RIGHT enter `to`? CHAIN needs a filled song cell, PHRASE a phrase at the chain row.
// ⚠️ R+RIGHT only — gating any other move could strand the user.
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

// After a load, put the pointer on the first filled cell (and chain row), or the gate refuses CHAIN
// with nothing on screen saying why.
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

    const int chainId = songcore::chain_at(p, pointer_track(s), pointer_song_row(s));
    if (chainId >= 0 && songcore::phrase_at(p, chainId, pointer_chain_row(s)) < 0) {
        for (int row = 0; row < songcore::CHAIN_ROWS; ++row)
            if (songcore::phrase_at(p, chainId, row) >= 0) { set_pointer_chain_row(s, row); break; }
    }

    refresh_song_relative_refs(s);
}

}  // namespace pt::ui
