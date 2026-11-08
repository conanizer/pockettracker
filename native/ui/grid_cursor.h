#pragma once

// The four grid screens — SONG, CHAIN, PHRASE and TABLE: whose cursor, and how far it goes. The
// selection, the clipboard and the D-pad read them here.

#include "ui/app_state.h"
#include "ui/screen.h"

namespace pt::ui {

struct GridCursor {
    CursorPosition AppState::*cursor;
    int maxRow;     // 255 on SONG: a selection spans the arrangement, not the 16 rows shown
    int maxColumn;  // the rightmost column; column 0 is the gutter
    // A+D-pad over a selection steps every selected column, not just the cursor's: on SONG each column
    // is a track of the same chain numbers; elsewhere each column is a different parameter.
    bool stepsAllColumns = false;
};

inline constexpr GridCursor SONG_GRID{&AppState::songCursor, 255, 8, true};       // the tracks
inline constexpr GridCursor CHAIN_GRID{&AppState::chainCursor, 15, 2};            // phrase, transpose
inline constexpr GridCursor PHRASE_GRID{&AppState::phraseCursor, 15, 9};          // note, vel, inst, 3 × FX
inline constexpr GridCursor TABLE_GRID{&AppState::tableCursor, 15, 8};            // transpose, vol, 3 × FX

// Is (row, column) a cell of `g`? Column 0 is the gutter.
inline bool in_grid(const GridCursor& g, int row, int column) {
    return row >= 0 && row <= g.maxRow && column >= 1 && column <= g.maxColumn;
}

// Null off the four.
inline const GridCursor* grid_of(ScreenType screen) {
    switch (screen) {
        case ScreenType::SONG:   return &SONG_GRID;
        case ScreenType::CHAIN:  return &CHAIN_GRID;
        case ScreenType::PHRASE: return &PHRASE_GRID;
        case ScreenType::TABLE:  return &TABLE_GRID;
        default:                 return nullptr;
    }
}

}  // namespace pt::ui
