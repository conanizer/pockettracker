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
// The three screens S4 adds are each irregular in their own way, and none of them is a grid:
//
//   • INSTRUMENT walks a ROW-KIND TABLE (ui/instrument_row_layout.h), not a range — rows have 1, 2 or
//     3 value columns, some are unreachable spacers, and the column you land on depends on the column
//     you left.
//   • MODS has no columns at all. Its cursor is (pair, side, row), and how far DOWN you may go is a
//     function of the mod TYPE under you — an ADSR is 7 rows deep where a NONE is 1.
//   • INST.POOL's cursor ROW *is* `currentInstrument`. Moving up and down the pool selects a different
//     instrument; only the column lives in the pool's own state.
//
// The screens that still do not exist (MIXER, PROJECT, SETTINGS…) fall through to the shared 16-row
// default, which is what the Kotlin `else` branch does for anything it has not named.

#include "ui/app_state.h"
#include "ui/instrument_row_layout.h"

namespace pt::ui {

// ─── INSTRUMENT ──────────────────────────────────────────────────────────────────────────────────

namespace detail {

inline bool instrument_is_sf(const AppState& s) {
    return s.project->instruments[static_cast<size_t>(s.currentInstrument)].instrumentType ==
           songcore::InstrumentType::SOUNDFONT;
}

/** Step ±1 rows with wrap, stepping straight OVER the spacers. `TrackerController.instrumentRowStep`. */
inline int instrument_row_step(bool is_sf, int from, int delta) {
    const int count = instrument_row_count(is_sf);
    int       r     = from;
    do {
        r += delta;
        if (r < 0) r = count - 1;
        if (r >= count) r = 0;
    } while (instrument_row_kind(is_sf, r) == InstrumentRowKind::SPACER);
    return r;
}

/**
 * The column to land on after a vertical move — `TrackerController.instrumentColumnFor`.
 *
 * The rule is "keep the column you were in, if the new row has one like it". Walking down the
 * right-hand column (FILTER → FREQ → RES) must stay in the right-hand column; walking off it onto a
 * row that has no such column falls back to 1 rather than leaving the cursor on a cell that is not
 * drawn. SOURCE always snaps to LOAD, whichever column you came from.
 */
inline int instrument_column_for(bool is_sf, int new_row, int old_row, int old_column) {
    const auto has_right = [](InstrumentRowKind k) {
        return k == InstrumentRowKind::DUAL || k == InstrumentRowKind::NAME ||
               k == InstrumentRowKind::TRIPLE;
    };
    const InstrumentRowKind old = instrument_row_kind(is_sf, old_row);

    switch (instrument_row_kind(is_sf, new_row)) {
        case InstrumentRowKind::SOURCE:
            return 2;   // LOAD

        case InstrumentRowKind::TRIPLE:
            if (has_right(old) && old_column == 3) return 3;
            if (old == InstrumentRowKind::TRIPLE && old_column == 5) return 5;
            return 1;

        case InstrumentRowKind::DUAL:
        case InstrumentRowKind::NAME:
            return (has_right(old) && old_column >= 3) ? 3 : 1;

        default:
            return 1;   // SINGLE / SPACER
    }
}

/** The leftmost column reachable from `column` on this row. getInstrumentCursorLeftColumn. */
inline int instrument_left_column(bool is_sf, int row, int column) {
    switch (instrument_row_kind(is_sf, row)) {
        case InstrumentRowKind::NAME:   return column - 1 < 1 ? 1 : column - 1;  // 3→2→1
        case InstrumentRowKind::SOURCE: return column - 1 < 2 ? 2 : column - 1;  // 3→2, never below LOAD
        case InstrumentRowKind::TRIPLE: return column - 2 < 1 ? 1 : column - 2;  // 5→3→1
        case InstrumentRowKind::DUAL:   return 1;                                // 3→1, in one jump
        default:                        return 1;
    }
}

/** The rightmost. getInstrumentCursorRightColumn. */
inline int instrument_right_column(bool is_sf, int row, int column) {
    switch (instrument_row_kind(is_sf, row)) {
        case InstrumentRowKind::NAME:   return column + 1 > 3 ? 3 : column + 1;  // 1→2→3
        case InstrumentRowKind::SOURCE: {
            // A SoundFont caps at LOAD: it has no editable waveform, so no EDIT button is drawn — and
            // a cursor allowed onto column 3 there would simply VANISH (no cell matches it).
            const int cap = is_sf ? 2 : 3;
            const int c   = column + 1;
            return c > cap ? cap : c;
        }
        case InstrumentRowKind::TRIPLE: return column + 2 > 5 ? 5 : column + 2;  // 1→3→5
        case InstrumentRowKind::DUAL:   return 3;                                // 1→3, in one jump
        default:                        return 1;
    }
}

/** MODS: how many rows the slot under (pair, side) has. */
inline int mod_slot_rows(const AppState& s, int pair, int side) {
    const songcore::Instrument& ins =
        s.project->instruments[static_cast<size_t>(s.currentInstrument)];
    return songcore::mod_slot_row_count(ins.modSlots[static_cast<size_t>(pair * 2 + side)]);
}

/** INST.POOL: the pool cursor's row IS the selected instrument, and it wraps 00↔7F. */
inline void move_pool_selection(AppState& s, int delta) {
    const int size = static_cast<int>(s.project->instruments.size());
    s.currentInstrument    = ((s.currentInstrument + delta) % size + size) % size;
    s.lastEditedInstrument = s.currentInstrument;
}

}  // namespace detail

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

        case ScreenType::INSTRUMENT: {
            const bool sf        = detail::instrument_is_sf(s);
            const int  oldRow    = s.instrumentCursorRow;
            const int  oldColumn = s.instrumentCursorColumn;
            s.instrumentCursorRow    = detail::instrument_row_step(sf, oldRow, -1);
            s.instrumentCursorColumn =
                detail::instrument_column_for(sf, s.instrumentCursorRow, oldRow, oldColumn);
            break;
        }

        case ScreenType::MODS:
            // Up out of the top of a pair drops to the BOTTOM of the pair above — clamped to that
            // slot's own depth, because the slot you are arriving at may be shorter than the one you
            // left (an ADSR above a NONE). At pair 0 row 0 there is nothing above: stay, no wrap.
            if (s.modCursorRow > 0) {
                s.modCursorRow--;
            } else if (s.modCursorPair > 0) {
                s.modCursorPair = 0;
                const int rows  = detail::mod_slot_rows(s, 0, s.modCursorSide);
                s.modCursorRow  = rows - 1 < 0 ? 0 : rows - 1;
            }
            break;

        case ScreenType::INST_POOL:
            detail::move_pool_selection(s, -1);
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

        case ScreenType::INSTRUMENT: {
            const bool sf        = detail::instrument_is_sf(s);
            const int  oldRow    = s.instrumentCursorRow;
            const int  oldColumn = s.instrumentCursorColumn;
            s.instrumentCursorRow    = detail::instrument_row_step(sf, oldRow, +1);
            s.instrumentCursorColumn =
                detail::instrument_column_for(sf, s.instrumentCursorRow, oldRow, oldColumn);
            break;
        }

        case ScreenType::MODS: {
            const int rows = detail::mod_slot_rows(s, s.modCursorPair, s.modCursorSide);
            if (s.modCursorRow < rows - 1) {
                s.modCursorRow++;
            } else if (s.modCursorPair < 1) {
                s.modCursorPair = 1;
                s.modCursorRow  = 0;
            }
            // At pair 1, last row: stay at the bottom, no wrap.
            break;
        }

        case ScreenType::INST_POOL:
            detail::move_pool_selection(s, +1);
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
    switch (s.currentScreen) {
        case ScreenType::TABLE:
            if (s.tableCursorColumn > 1) s.tableCursorColumn--;
            return;

        case ScreenType::INSTRUMENT: {
            const int minColumn = detail::instrument_left_column(
                detail::instrument_is_sf(s), s.instrumentCursorRow, s.instrumentCursorColumn);
            if (s.instrumentCursorColumn > minColumn) s.instrumentCursorColumn = minColumn;
            return;
        }

        case ScreenType::MODS: {
            // LEFT/RIGHT do not move along a row here — they change WHICH SLOT of the pair you are
            // editing. The row must then be clamped into the new slot's depth: cross from a 7-row ADSR
            // onto a 1-row NONE and row 6 does not exist there.
            s.modCursorSide = 0;
            const int rows  = detail::mod_slot_rows(s, s.modCursorPair, 0);
            const int max   = rows - 1 < 0 ? 0 : rows - 1;
            if (s.modCursorRow > max) s.modCursorRow = max;
            return;
        }

        case ScreenType::INST_POOL:
            if (s.poolCursorColumn > 0) s.poolCursorColumn--;
            return;

        default:
            break;
    }
    // GROOVE falls through here too, and correctly does nothing: min == max == 0.
    const int minColumn = min_cursor_column(s.currentScreen);
    if (s.cursorColumn > minColumn) s.cursorColumn--;
}

inline void move_cursor_right(AppState& s) {
    switch (s.currentScreen) {
        case ScreenType::TABLE:
            if (s.tableCursorColumn < 8) s.tableCursorColumn++;
            return;

        case ScreenType::INSTRUMENT: {
            const int maxColumn = detail::instrument_right_column(
                detail::instrument_is_sf(s), s.instrumentCursorRow, s.instrumentCursorColumn);
            if (s.instrumentCursorColumn < maxColumn) s.instrumentCursorColumn = maxColumn;
            return;
        }

        case ScreenType::MODS: {
            s.modCursorSide = 1;
            const int rows  = detail::mod_slot_rows(s, s.modCursorPair, 1);
            const int max   = rows - 1 < 0 ? 0 : rows - 1;
            if (s.modCursorRow > max) s.modCursorRow = max;
            return;
        }

        case ScreenType::INST_POOL:
            if (s.poolCursorColumn < 4) s.poolCursorColumn++;
            return;

        default:
            break;
    }
    const int maxColumn = max_cursor_column(s.currentScreen);
    if (s.cursorColumn < maxColumn) s.cursorColumn++;
}

}  // namespace pt::ui
