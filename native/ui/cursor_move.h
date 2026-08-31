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
// And the two S5 adds:
//
//   • MIXER is a SHAPE, not a rectangle. Rows 2 and 3 exist only in column 8, so the cursor walks the
//     eight track meters along row 0, drops to the two send returns on row 1, and reaches the master
//     strip by continuing DOWN column 8. Its row-0 columns are the only ones in the app that WRAP
//     (track 0 ← master → track 0), because a mixer is a ring of channels rather than a document.
//   • EFFECTS is one column of eight rows, and they CLAMP at both ends.
//
//   • PROJECT and SETTINGS are FORMS whose rows WRAP, and whose every row change snaps the column
//     back to 1 — their rows have 1, 2, 3 and 20 columns, so a carried column would land nowhere.
//     SETTINGS additionally wraps over its VISIBLE rows only (ui/settings_row_layout.h), which is a
//     loop where Kotlin gets away with a single substitution.
//
// Any screen not named below falls through to the shared 16-row default, which is what the Kotlin
// `else` branch does for anything it has not named.
//
// And the one thing a SETTING changes (ui/song_pointer.h):
//
//   • PHRASE SPILLS ACROSS CHAIN ROWS under NAV = SONG. Stepping off step 0F lands on step 00 of the
//     next filled row's phrase rather than back at 00 of the same one — the 16 steps stop being the
//     loop and the CHAIN becomes it. Under NAV = POOL, PHRASE is the plain 16-row default it has
//     always been, which is why the arm below states both answers rather than branching around one.

#include <algorithm>

#include "ui/app_state.h"
#include "ui/instrument_row_layout.h"
#include "ui/modules/scale_editor.h"
#include "ui/settings_row_layout.h"
#include "ui/song_pointer.h"

namespace pt::ui {

// ─── INSTRUMENT ──────────────────────────────────────────────────────────────────────────────────

namespace detail {

/** The layout selector: which of the three row tables the cursor is walking. */
inline songcore::InstrumentType instrument_type_of(const AppState& s) {
    return s.project->instruments[static_cast<size_t>(s.currentInstrument)].instrumentType;
}

/** Step ±1 rows with wrap, stepping straight OVER the spacers. `TrackerController.instrumentRowStep`. */
inline int instrument_row_step(songcore::InstrumentType type, int from, int delta) {
    const int count = instrument_row_count(type);
    int       r     = from;
    do {
        r += delta;
        if (r < 0) r = count - 1;
        if (r >= count) r = 0;
    } while (instrument_row_kind(type, r) == InstrumentRowKind::SPACER);
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
inline int instrument_column_for(songcore::InstrumentType type, int new_row, int old_row,
                                 int old_column) {
    const auto has_right = [](InstrumentRowKind k) {
        return k == InstrumentRowKind::DUAL || k == InstrumentRowKind::NAME ||
               k == InstrumentRowKind::TRIPLE;
    };
    const InstrumentRowKind old = instrument_row_kind(type, old_row);

    switch (instrument_row_kind(type, new_row)) {
        case InstrumentRowKind::SOURCE:
            return 2;   // LOAD

        case InstrumentRowKind::TRIPLE:
            if (has_right(old) && old_column == 3) return 3;
            if (old == InstrumentRowKind::TRIPLE && old_column == 5) return 5;
            return 1;

        case InstrumentRowKind::DUAL:
            return (has_right(old) && old_column >= 3) ? 3 : 1;

        case InstrumentRowKind::NAME:
            // Row 0's EDIT (column 3) is drawn on samplers only, so a SoundFont must not land on it —
            // nor may EXTERNAL, which draws neither button. The cap is the row's own, read off the
            // table, so the three types cannot disagree with what is drawn.
            return (has_right(old) && old_column >= 3 &&
                    instrument_name_row_max_column(type) >= 3)
                       ? 3
                       : 1;

        default:
            return 1;   // SINGLE / SPACER
    }
}

/** The leftmost column reachable from `column` on this row. getInstrumentCursorLeftColumn. */
inline int instrument_left_column(songcore::InstrumentType type, int row, int column) {
    switch (instrument_row_kind(type, row)) {
        case InstrumentRowKind::NAME:   return column - 1 < 1 ? 1 : column - 1;  // 3→2→1
        case InstrumentRowKind::SOURCE: return column - 1 < 2 ? 2 : column - 1;  // 3→2, never below LOAD
        case InstrumentRowKind::TRIPLE: return column - 2 < 1 ? 1 : column - 2;  // 5→3→1
        case InstrumentRowKind::DUAL:   return 1;                                // 3→1, in one jump
        default:                        return 1;
    }
}

/** The rightmost. getInstrumentCursorRightColumn. */
inline int instrument_right_column(songcore::InstrumentType type, int row, int column) {
    switch (instrument_row_kind(type, row)) {
        case InstrumentRowKind::NAME: {
            // Row 0's EDIT (column 3) is drawn on samplers only — a SoundFont has no waveform to edit,
            // and EXTERNAL has no source at all — so the cursor caps at LOAD (2) or at TYPE (1); a
            // cursor past the cap would sit on a cell that is not drawn.
            const int cap = instrument_name_row_max_column(type);
            const int c   = column + 1;
            return c > cap ? cap : c;
        }
        case InstrumentRowKind::SOURCE: {
            // The INST PRESET row: SAVE (2) and LOAD (3) are both drawn for EITHER instrument type — a
            // preset round-trips a sampler or a SoundFont alike — so there is no per-type cap here.
            const int c = column + 1;
            return c > 3 ? 3 : c;
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


// ─── SAMPLE EDITOR ───────────────────────────────────────────────────────────────────────────────
//
// Its rows are a SPARSE map (1, 2, 8, 10, 11, 13, 14, 16, 18, 19) — the gaps are the waveform and the
// section spacers — so a step is a table lookup, not `row ± 1`. And the row you land on may have fewer
// columns than the one you left (NAME has one; the op rows have six), so the column CLAMPS on the way.
// See ui/modules/sample_editor.h.
namespace detail {

inline void sample_editor_step_row(AppState& s, int delta) {
    SampleEditorState& se = s.sampleEditor;
    const int newRow = (delta < 0) ? SampleEditorModule::row_above(se.cursorRow, se.sliceMethod)
                                   : SampleEditorModule::row_below(se.cursorRow, se.sliceMethod);
    se.cursorRow = newRow;
    se.cursorCol = std::min(se.cursorCol,
                            SampleEditorModule::max_col_for_row(newRow, se.sliceMethod));
}

/**
 * ⚠️ The two OP rows (13 = CROP…DEL, 14 = NORM…UNDO) WRAP; every other row clamps.
 *
 * That is not an inconsistency — it is what those rows are. They are a ring of six BUTTONS, not a range
 * of values, and stepping right off DEL to reach CROP is the same gesture as stepping right off the
 * master strip on the MIXER (the app's only other wrapping column, and for the same reason: a ring of
 * channels rather than a document).
 */
inline void sample_editor_step_col(AppState& s, int delta) {
    SampleEditorState& se     = s.sampleEditor;
    const int          maxCol = SampleEditorModule::max_col_for_row(se.cursorRow, se.sliceMethod);
    const bool         isOps  = (se.cursorRow == 13 || se.cursorRow == 14);

    if (isOps) {
        se.cursorCol = (delta < 0) ? ((se.cursorCol == 0) ? maxCol : se.cursorCol - 1)
                                   : ((se.cursorCol + 1) % (maxCol + 1));
    } else {
        se.cursorCol = std::clamp(se.cursorCol + delta, 0, maxCol);
    }
}

}  // namespace detail

inline void move_cursor_up(AppState& s) {
    switch (s.currentScreen) {
        case ScreenType::SAMPLE_EDITOR:
            detail::sample_editor_step_row(s, -1);
            break;

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
        case ScreenType::SCALE:
            s.scaleCursorRow =
                (s.scaleCursorRow > 0) ? s.scaleCursorRow - 1 : SCALE_ROW_COUNT - 1;
            break;

        case ScreenType::INSTRUMENT: {
            const songcore::InstrumentType ty = detail::instrument_type_of(s);
            const int  oldRow    = s.instrumentCursorRow;
            const int  oldColumn = s.instrumentCursorColumn;
            s.instrumentCursorRow    = detail::instrument_row_step(ty, oldRow, -1);
            s.instrumentCursorColumn =
                detail::instrument_column_for(ty, s.instrumentCursorRow, oldRow, oldColumn);
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

        case ScreenType::MIXER:
            // Out of a send return, UP lands on the FIRST TRACK — not on the track above it, because
            // there is no track above it: the sends sit under the whole meter row, not under a channel.
            if (s.mixerMasterRow == 1 && (s.mixerCursorColumn == 0 || s.mixerCursorColumn == 1)) {
                s.mixerMasterRow    = 0;
                s.mixerCursorColumn = 0;
            } else if (s.mixerMasterRow > 0) {
                s.mixerMasterRow--;   // up the master strip: LIM → OTT → EQ → MIX, column 8 throughout
            }
            // Row 0 (the meters): nothing above them — stay.
            break;

        case ScreenType::EFFECTS:
            if (s.effectsCursorRow > 0) s.effectsCursorRow--;
            break;

        // PROJECT's rows WRAP, and every row change snaps the column back to 1 — you never arrive on
        // a row holding the column you left the last one on, because the rows have 1, 2, 3 and 20 of
        // them and a carried column would land nowhere.
        case ScreenType::PROJECT:
            s.projectCursorRow    = project_next_visible_row(s.projectCursorRow, -1, s.caps);
            s.projectCursorColumn = 1;
            break;

        // SETTINGS wraps too — over the VISIBLE rows (ui/settings_row_layout.h).
        case ScreenType::SETTINGS:
            s.settingsCursorRow    = settings_next_visible_row(s.settingsCursorRow, -1, s.caps);
            s.settingsCursorColumn = 1;
            break;

        // MIDI wraps over a fixed eight — no caps filter, because every row is on every platform.
        // ⚠️ The column resets to 1, which matters since E3 gave IN CH eight of them: leaving the
        // cursor on column 6 while stepping onto a one-column row would put it on a cell that is not
        // drawn, and `cursor_context` would answer for a track the screen is not showing.
        case ScreenType::MIDI:
            s.midiCursorRow = (s.midiCursorRow > 0) ? s.midiCursorRow - 1 : MIDI_ROW_COUNT - 1;
            s.midiCursorColumn = 1;
            break;

        // Off the top step, under NAV = SONG: the pointer climbs to the previous FILLED chain row and
        // the cursor lands on the last step of whatever phrase sits there. The move itself is the same
        // wrap either way — only the pointer is extra — so the assignment below is said once.
        // ⚠️ A chain with a single filled row answers `from` and nothing moves, which is exactly right:
        // the wrap then happens inside the one phrase, as it does under POOL.
        case ScreenType::PHRASE:
            if (s.settings.navSongRelative && s.cursorRow == 0) {
                const int from = pointer_chain_row(s);
                const int to   = songcore::next_chain_row(*s.project, s.currentChain, from, -1,
                                                         /*wrap=*/true);
                if (to != from) { set_pointer_chain_row(s, to); refresh_song_relative_refs(s); }
            }
            s.cursorRow = (s.cursorRow > 0) ? s.cursorRow - 1 : 15;
            break;

        default:
            s.cursorRow = (s.cursorRow > 0) ? s.cursorRow - 1 : 15;
            break;
    }
}

inline void move_cursor_down(AppState& s) {
    switch (s.currentScreen) {
        case ScreenType::SAMPLE_EDITOR:
            detail::sample_editor_step_row(s, +1);
            break;

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
        case ScreenType::SCALE:
            s.scaleCursorRow =
                (s.scaleCursorRow < SCALE_ROW_COUNT - 1) ? s.scaleCursorRow + 1 : 0;
            break;

        case ScreenType::INSTRUMENT: {
            const songcore::InstrumentType ty = detail::instrument_type_of(s);
            const int  oldRow    = s.instrumentCursorRow;
            const int  oldColumn = s.instrumentCursorColumn;
            s.instrumentCursorRow    = detail::instrument_row_step(ty, oldRow, +1);
            s.instrumentCursorColumn =
                detail::instrument_column_for(ty, s.instrumentCursorRow, oldRow, oldColumn);
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

        case ScreenType::MIXER:
            if (s.mixerMasterRow == 0 && s.mixerCursorColumn < 8) {
                // Down off ANY track meter → the REV send. (The tracks feed the sends; the gesture says
                // so.) Column 8 is excluded because the master strip continues downward instead.
                s.mixerMasterRow    = 1;
                s.mixerCursorColumn = 0;
            } else if (s.mixerCursorColumn == 8 && s.mixerMasterRow < 3) {
                s.mixerMasterRow++;   // MIX → EQ → OTT|DUST → LIM
            }
            // A send return: nothing below it — stay.
            break;

        case ScreenType::EFFECTS:
            if (s.effectsCursorRow < 7) s.effectsCursorRow++;
            break;

        case ScreenType::PROJECT:
            s.projectCursorRow    = project_next_visible_row(s.projectCursorRow, +1, s.caps);
            s.projectCursorColumn = 1;
            break;

        case ScreenType::SETTINGS:
            s.settingsCursorRow    = settings_next_visible_row(s.settingsCursorRow, +1, s.caps);
            s.settingsCursorColumn = 1;
            break;

        case ScreenType::MIDI:
            s.midiCursorRow = (s.midiCursorRow < MIDI_ROW_COUNT - 1) ? s.midiCursorRow + 1 : 0;
            s.midiCursorColumn = 1;
            break;

        // …and off the bottom step it descends. See move_cursor_up's arm.
        case ScreenType::PHRASE:
            if (s.settings.navSongRelative && s.cursorRow == 15) {
                const int from = pointer_chain_row(s);
                const int to   = songcore::next_chain_row(*s.project, s.currentChain, from, +1,
                                                         /*wrap=*/true);
                if (to != from) { set_pointer_chain_row(s, to); refresh_song_relative_refs(s); }
            }
            s.cursorRow = (s.cursorRow < 15) ? s.cursorRow + 1 : 0;
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
    if (s.currentScreen == ScreenType::SAMPLE_EDITOR) {
        detail::sample_editor_step_col(s, -1);
        return;
    }
    switch (s.currentScreen) {
        case ScreenType::TABLE:
            if (s.tableCursorColumn > 1) s.tableCursorColumn--;
            return;

        case ScreenType::INSTRUMENT: {
            const int minColumn = detail::instrument_left_column(
                detail::instrument_type_of(s), s.instrumentCursorRow, s.instrumentCursorColumn);
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

        case ScreenType::MIXER:
            if (s.mixerMasterRow == 0) {
                // The meter row WRAPS — the only wrapping column in the app. Track 0 → master, and the
                // master → track 7.
                s.mixerCursorColumn = (s.mixerCursorColumn > 0) ? s.mixerCursorColumn - 1 : 8;
            } else if (s.mixerMasterRow == 1 && s.mixerCursorColumn == 1) {
                s.mixerCursorColumn = 0;   // DEL → REV
            } else if (s.mixerCursorColumn == 8) {
                // The whole master strip (EQ / OTT / LIM) exits LEFT onto the DEL send, whichever row
                // it was on — the strip is a column, and this is the door out of it.
                s.mixerMasterRow    = 1;
                s.mixerCursorColumn = 1;
            }
            // REV (row 1, column 0): nothing to its left — stay.
            return;

        // Both step back toward column 1, never to 0: column 0 is the row LABEL, and it is not a cell.
        case ScreenType::PROJECT:
            if (s.projectCursorColumn > 1) s.projectCursorColumn--;
            return;

        // ⚠️ SETTINGS SNAPS, it does not step. LEFT from the second column goes straight to the first,
        // because there are only ever two and there is nothing between them. (Kotlin: a bare
        // `settingsCursorColumn = 1`.)
        case ScreenType::SETTINGS:
            s.settingsCursorColumn = 1;
            return;

        // ⚠️ MIDI IS ONE COLUMN WIDE EXCEPT ON IN CH, AND SAYS SO OUT LOUD rather than falling into the
        // `default` and reaching the same answer by accident. It would, for the one-column rows: min ==
        // max == 0 there. But the accident it would rely on is EFFECTS' documented bug two comments
        // down — LEFT walking the SHARED `cursorColumn` on a screen that never reads it — and a screen
        // that is correct only because another screen's bug is harmless is one row away from not being.
        case ScreenType::MIDI:
            if (static_cast<MidiRow>(s.midiCursorRow) == MidiRow::IN_MAP && s.midiCursorColumn > 1)
                s.midiCursorColumn--;
            return;

        default:
            break;
    }
    // GROOVE falls through here too, and correctly does nothing: min == max == 0.
    //
    // ⚠️ So does EFFECTS — and there it is not a no-op: min_cursor_column(EFFECTS) is 0, so LEFT walks
    // the SHARED `cursorColumn` (SONG / CHAIN / PHRASE's) down to 0 while you are looking at a screen
    // that never reads it. Kotlin does exactly this, for exactly the same reason (EFFECTS is not named
    // in its `when`, so it lands in the `else`), and it is invisible on both platforms because
    // `go_to_screen` restores or refreshes that column on the way back into the three screens that own
    // it. Kept bug-for-bug rather than "fixed": the fix would be a divergence with no observable.
    const int minColumn = min_cursor_column(s.currentScreen);
    if (s.cursorColumn > minColumn) s.cursorColumn--;
}

inline void move_cursor_right(AppState& s) {
    if (s.currentScreen == ScreenType::SAMPLE_EDITOR) {
        detail::sample_editor_step_col(s, +1);
        return;
    }
    switch (s.currentScreen) {
        case ScreenType::TABLE:
            if (s.tableCursorColumn < 8) s.tableCursorColumn++;
            return;

        case ScreenType::INSTRUMENT: {
            const int maxColumn = detail::instrument_right_column(
                detail::instrument_type_of(s), s.instrumentCursorRow, s.instrumentCursorColumn);
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

        case ScreenType::MIXER:
            if (s.mixerMasterRow == 0) {
                s.mixerCursorColumn = (s.mixerCursorColumn < 8) ? s.mixerCursorColumn + 1 : 0;
            } else if (s.mixerMasterRow == 1 && s.mixerCursorColumn == 0) {
                s.mixerCursorColumn = 1;   // REV → DEL
            } else if (s.mixerMasterRow == 1 && s.mixerCursorColumn == 1) {
                s.mixerCursorColumn = 8;   // DEL → the master strip, entering it at the EQ row
            }
            // Already in column 8: it is the rightmost — stay.
            return;

        // PROJECT steps right within the row's own column count: 20 on NAME (one per character),
        // 3 on PROJECT, 2 on EXPORT and COMPACT, 1 everywhere else.
        case ScreenType::PROJECT: {
            const int max = project_row_max_column(static_cast<ProjectRow>(s.projectCursorRow));
            if (s.projectCursorColumn < max) s.projectCursorColumn++;
            return;
        }

        // ⚠️ SETTINGS SNAPS to column 2 — if the row has one at all. Which rows do is caps-dependent:
        // TRACE's second column is ENG, and on the shell there is no second sequencer to select, so
        // RIGHT there must not move. (Kotlin asks the same question with a hard-coded row set,
        // `settingsCursorRow in setOf(2, 3, 4, 10, 12)`, plus the dynamic LAYOUT case.)
        case ScreenType::SETTINGS: {
            const SettingsRow row = static_cast<SettingsRow>(s.settingsCursorRow);
            const bool hasSkins   = s.settings.skinCount > 0;
            if (settings_row_has_second_column(row, s.caps, hasSkins) && s.settingsCursorColumn < 2)
                s.settingsCursorColumn = 2;
            return;
        }

        // One column, except IN CH's eight — see the matching arm in move_cursor_left for why it is
        // stated rather than left to the fall-through. ⚠️ The bound is the PROJECT's own vector, not
        // the constant: a project carrying fewer than eight tracks must not offer a ninth cell that
        // `handle_input` will then refuse.
        case ScreenType::MIDI: {
            if (static_cast<MidiRow>(s.midiCursorRow) != MidiRow::IN_MAP) return;
            const int tracks = s.project ? static_cast<int>(s.project->midiInputChannels.size()) : 0;
            const int last   = std::min(MIDI_IN_MAP_COLUMNS, tracks);
            if (s.midiCursorColumn < last) s.midiCursorColumn++;
            return;
        }

        default:
            break;
    }
    const int maxColumn = max_cursor_column(s.currentScreen);
    if (s.cursorColumn < maxColumn) s.cursorColumn++;
}

}  // namespace pt::ui
