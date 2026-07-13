#pragma once

// ─── Cursor movement ─────────────────────────────────────────────────────────────────────────────
//
// A 1:1 port of `TrackerController.moveCursorUp/Down/Left/Right` — what the D-PAD ALONE does, as
// opposed to R+DPAD (which moves between screens: ui/navigation.h) and A+DPAD (which edits the cell
// under the cursor: ui/cursor.h).
//
// It is a per-screen table of bounds, and the bounds are NOT uniform — this is the whole content of
// the file, and every irregularity in it is deliberate:
//
//   • ROWS WRAP, COLUMNS CLAMP. Off the bottom of a phrase you land back on step 0; off the right of
//     it you simply stay. (A tracker is a loop vertically and a record horizontally.)
//   • …EXCEPT ON SONG, WHERE ROWS CLAMP TOO — its 256 rows are a document, not a loop, and wrapping
//     from row 255 to row 0 in a long arrangement would be a way to lose your place, not to save time.
//   • THE STEP-NUMBER COLUMN IS NEVER REACHABLE. Every editor's minimum column is 1, not 0 (0 is the
//     read-only row-number gutter). The cursor context for column 0 exists — `cc::read_only()` — but
//     only because a module must answer for every column, not because you can get there.
//   • TABLE AND GROOVE CARRY THEIR OWN CURSORS. SONG / CHAIN / PHRASE share `cursorRow`/`cursorColumn`
//     so that moving between them keeps your place; the other two do not, exactly as Kotlin has it.
//
// The screens that do not exist yet (INSTRUMENT, MIXER, MODS, the pool…) each have their own rules and
// they land with their modules. Until then they fall through to the shared 16-row default, which is
// what the Kotlin `else` branch does for anything it has not named.

#include "ui/app_state.h"

namespace pt::ui {

/** SONG shows 16 of its 256 rows; keep `cursorRow` inside that window. `scrollSongToRow` in Kotlin. */
inline void scroll_song_to_row(AppState& s, int row) {
    if (row < s.songScrollPosition)            s.songScrollPosition = row;
    else if (row >= s.songScrollPosition + 16) s.songScrollPosition = row - 15;
}

inline void move_cursor_up(AppState& s) {
    switch (s.currentScreen) {
        case ScreenType::SONG:
            // Clamps, and drags the viewport with it.
            if (s.cursorRow > 0) {
                s.cursorRow--;
                if (s.cursorRow < s.songScrollPosition) s.songScrollPosition = s.cursorRow;
            }
            break;
        case ScreenType::TABLE:
            s.tableCursorRow = (s.tableCursorRow > 0) ? s.tableCursorRow - 1 : 15;
            break;
        case ScreenType::GROOVE:
            s.grooveCursorRow = (s.grooveCursorRow > 0) ? s.grooveCursorRow - 1 : 15;
            break;
        default:
            s.cursorRow = (s.cursorRow > 0) ? s.cursorRow - 1 : 15;
            break;
    }
}

inline void move_cursor_down(AppState& s) {
    switch (s.currentScreen) {
        case ScreenType::SONG:
            if (s.cursorRow < 255) {
                s.cursorRow++;
                if (s.cursorRow >= s.songScrollPosition + 16) s.songScrollPosition = s.cursorRow - 15;
            }
            break;
        case ScreenType::TABLE:
            s.tableCursorRow = (s.tableCursorRow < 15) ? s.tableCursorRow + 1 : 0;
            break;
        case ScreenType::GROOVE:
            s.grooveCursorRow = (s.grooveCursorRow < 15) ? s.grooveCursorRow + 1 : 0;
            break;
        default:
            s.cursorRow = (s.cursorRow < 15) ? s.cursorRow + 1 : 0;
            break;
    }
}

/** The leftmost column the cursor may occupy. 1 everywhere it is defined — column 0 is the gutter. */
inline int min_cursor_column(ScreenType s) {
    switch (s) {
        case ScreenType::SONG:
        case ScreenType::CHAIN:
        case ScreenType::PHRASE: return 1;
        default: return 0;
    }
}

/** The rightmost. SONG's is a TRACK number (1..8), not a field index. */
inline int max_cursor_column(ScreenType s) {
    switch (s) {
        case ScreenType::SONG:   return 8;  // 8 tracks
        case ScreenType::CHAIN:  return 2;  // phrase, transpose
        case ScreenType::PHRASE: return 9;  // note, vel, inst, 3 × (fx type, fx value)
        default: return 0;
    }
}

inline void move_cursor_left(AppState& s) {
    if (s.currentScreen == ScreenType::TABLE) {
        if (s.tableCursorColumn > 1) s.tableCursorColumn--;
        return;
    }
    // GROOVE falls through here too, and correctly does nothing: min == max == 0.
    const int minColumn = min_cursor_column(s.currentScreen);
    if (s.cursorColumn > minColumn) s.cursorColumn--;
}

inline void move_cursor_right(AppState& s) {
    if (s.currentScreen == ScreenType::TABLE) {
        if (s.tableCursorColumn < 8) s.tableCursorColumn++;
        return;
    }
    const int maxColumn = max_cursor_column(s.currentScreen);
    if (s.cursorColumn < maxColumn) s.cursorColumn++;
}

}  // namespace pt::ui
