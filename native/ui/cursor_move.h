#pragma once

// ─── Cursor movement ─────────────────────────────────────────────────────────────────────────────
//
// What the D-PAD ALONE does (R+DPAD changes screens: ui/navigation.h; A+DPAD edits: ui/cursor.h). A
// per-screen table of bounds, and every irregularity is deliberate:
//
//   • ROWS WRAP, COLUMNS CLAMP — a tracker is a loop vertically and a record horizontally.
//   • …except SONG, whose 256 rows clamp too: wrapping a long arrangement loses your place.
//   • Column 0, the row-number gutter, is never reachable (a module still answers for it).
//   • SONG / CHAIN / PHRASE share `cursorRow`/`cursorColumn`, so moving between them keeps your place;
//     TABLE and GROOVE carry their own.
//   • INSTRUMENT walks a ROW-KIND TABLE (ui/instrument_row_layout.h): rows have 1–3 value columns,
//     spacers are skipped, and the column you land on depends on the one you left.
//   • MODS has no columns: (pair, side, row), and the depth below you depends on the mod TYPE.
//   • INST.POOL's row IS `currentInstrument`.
//   • MIXER is a SHAPE: tracks on row 0, the two sends on row 1, the master strip reached by going DOWN
//     column 8. Row 0 WRAPS (track 0 ← master → track 0) — a mixer is a ring of channels.
//   • EFFECTS clamps, walks in DRAWN order (ui/effects_row_layout.h), and has no column: LEFT/RIGHT
//     move to the row beside.
//   • PROJECT and SETTINGS are forms whose rows WRAP and every row change snaps the column to 1 (rows
//     have 1, 2, 3 or 20 columns). SETTINGS wraps over its VISIBLE rows only.
//   • Under NAV = SONG, PHRASE SPILLS across chain rows: off step 0F you land on 00 of the next filled
//     row's phrase (ui/song_pointer.h). Under NAV = POOL it is the plain 16-row loop.
// Any screen not named falls through to the shared 16-row default.

#include <algorithm>

#include "ui/app_state.h"
#include "ui/effects_row_layout.h"
#include "ui/instrument_row_layout.h"
#include "ui/modules/groove_editor.h"
#include "ui/modules/midi_map_editor.h"
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

/** Step ±1 rows with wrap, stepping OVER the spacers. */
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
 * The column to land on after a vertical move: keep the column if the new row has one like it
 * (walking FILTER → FREQ → RES stays right), else 1. SOURCE always snaps to LOAD.
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
            // Row 0's EDIT (column 3) is drawn on samplers only; the cap is the row's own, read off the
            // table, so no type can land on an undrawn cell.
            return (has_right(old) && old_column >= 3 &&
                    instrument_name_row_max_column(type) >= 3)
                       ? 3
                       : 1;

        default:
            return 1;   // SINGLE / SPACER
    }
}

/** The leftmost column reachable from `column` on this row. */
inline int instrument_left_column(songcore::InstrumentType type, int row, int column) {
    switch (instrument_row_kind(type, row)) {
        case InstrumentRowKind::NAME:   return column - 1 < 1 ? 1 : column - 1;  // 3→2→1
        case InstrumentRowKind::SOURCE: return column - 1 < 2 ? 2 : column - 1;  // 3→2, never below LOAD
        case InstrumentRowKind::TRIPLE: return column - 2 < 1 ? 1 : column - 2;  // 5→3→1
        case InstrumentRowKind::DUAL:   return 1;                                // 3→1, in one jump
        default:                        return 1;
    }
}

/** The rightmost. */
inline int instrument_right_column(songcore::InstrumentType type, int row, int column) {
    switch (instrument_row_kind(type, row)) {
        case InstrumentRowKind::NAME: {
            // Row 0's EDIT is drawn on samplers only (and EXTERNAL has no source), so the cap is LOAD
            // (2) or TYPE (1).
            const int cap = instrument_name_row_max_column(type);
            const int c   = column + 1;
            return c > cap ? cap : c;
        }
        case InstrumentRowKind::SOURCE: {
            // The INST PRESET row: SAVE and LOAD are drawn for every type — no per-type cap.
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
// Rows are a SPARSE map (1, 2, 8, 10, 11, 13, 14, 16, 18, 19) — the gaps are the waveform and spacers
// — so a step is a lookup, and the column CLAMPS (NAME has one column, the op rows six).
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
 * ⚠️ The two OP rows (13 = CROP…DEL, 14 = NORM…UNDO) WRAP; every other row clamps. They are a ring of
 * six buttons, not a range — like the MIXER's master strip.
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
        // ⚠️ The panel wraps within itself and the tick row stays put — walking off it would land on
        // an undrawn cell, and the swing readout (which follows the tick row) holds still.
        case ScreenType::GROOVE:
            if (s.grooveCursorColumn == GROOVE_COL_PANEL) {
                s.groovePanelRow =
                    (s.groovePanelRow > 0) ? s.groovePanelRow - 1 : GROOVE_PANEL_ROWS - 1;
                s.groovePanelColumn = 0;
            } else {
                s.grooveCursorRow = (s.grooveCursorRow > 0) ? s.grooveCursorRow - 1 : 15;
            }
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
            // Up out of a pair drops to the BOTTOM of the pair above, clamped to that slot's depth. At
            // pair 0 row 0: stay.
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
            // Out of a send, UP lands on the FIRST TRACK: the sends sit under the whole meter row.
            if (s.mixerMasterRow == 1 && (s.mixerCursorColumn == 0 || s.mixerCursorColumn == 1)) {
                s.mixerMasterRow    = 0;
                s.mixerCursorColumn = 0;
            } else if (s.mixerMasterRow > 0) {
                s.mixerMasterRow--;   // up the master strip: LIM → OTT → EQ → MIX, column 8 throughout
            }
            // Row 0 (the meters): nothing above them — stay.
            break;

        // EFFECTS clamps at both ends. A step is one DRAWN LINE (ui/effects_row_layout.h); the column
        // is carried, and a single-cell line takes the cursor from either column.
        case ScreenType::EFFECTS:
            s.effectsCursorRow = effects_next_row(s.effectsCursorRow, -1);
            break;

        // PROJECT wraps, and every row change snaps the column back to 1.
        case ScreenType::PROJECT:
            s.projectCursorRow    = project_next_visible_row(s.projectCursorRow, -1, s.caps);
            s.projectCursorColumn = 1;
            break;

        // SETTINGS wraps too — over the VISIBLE rows (ui/settings_row_layout.h).
        case ScreenType::SETTINGS:
            s.settingsCursorRow    = settings_next_visible_row(s.settingsCursorRow, -1, s.caps);
            s.settingsCursorColumn = 1;
            break;

        // MIDI wraps over its rows (every row exists on every platform).
        case ScreenType::MIDI:
            s.midiCursorRow    = (s.midiCursorRow > 0) ? s.midiCursorRow - 1 : MIDI_ROW_COUNT - 1;
            s.midiCursorColumn = 1;
            break;

        // ⚠️ The mapping list's rows are alike, so the column is CARRIED, then clamped: the ADD row has
        // one cell, and a destination with no scope number has five.
        case ScreenType::MIDI_MAP:
            if (s.project) {
                const int rows = midi_map_row_count(*s.project);
                s.midiMapCursorRow = (s.midiMapCursorRow > 0) ? s.midiMapCursorRow - 1 : rows - 1;
                s.midiMapCursorColumn =
                    midi_map_clamp_column(*s.project, s.midiMapCursorRow, s.midiMapCursorColumn);
            }
            break;

        // Off the top step under NAV = SONG, the pointer climbs to the previous FILLED chain row and the
        // cursor lands on that phrase's last step. A chain with one filled row answers `from`, and the
        // wrap stays inside the phrase, as under POOL.
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
            if (s.grooveCursorColumn == GROOVE_COL_PANEL) {
                s.groovePanelRow =
                    (s.groovePanelRow < GROOVE_PANEL_ROWS - 1) ? s.groovePanelRow + 1 : 0;
                s.groovePanelColumn = 0;
            } else {
                s.grooveCursorRow = (s.grooveCursorRow < 15) ? s.grooveCursorRow + 1 : 0;
            }
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
                // Down off any track meter → the REV send (the tracks feed the sends). Column 8
                // continues down the master strip instead.
                s.mixerMasterRow    = 1;
                s.mixerCursorColumn = 0;
            } else if (s.mixerCursorColumn == 8 && s.mixerMasterRow < 3) {
                s.mixerMasterRow++;   // MIX → EQ → OTT|DUST → LIM
            }
            // A send return: nothing below it — stay.
            break;

        // …and down. See move_cursor_up's arm.
        case ScreenType::EFFECTS:
            s.effectsCursorRow = effects_next_row(s.effectsCursorRow, +1);
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
            s.midiCursorRow    = (s.midiCursorRow < MIDI_ROW_COUNT - 1) ? s.midiCursorRow + 1 : 0;
            s.midiCursorColumn = 1;
            break;

        // The carried column and its clamp — see the matching arm in move_cursor_up.
        case ScreenType::MIDI_MAP:
            if (s.project) {
                const int rows = midi_map_row_count(*s.project);
                s.midiMapCursorRow = (s.midiMapCursorRow < rows - 1) ? s.midiMapCursorRow + 1 : 0;
                s.midiMapCursorColumn =
                    midi_map_clamp_column(*s.project, s.midiMapCursorRow, s.midiMapCursorColumn);
            }
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

        // ⚠️ Only the NAME row has columns; LEFT/RIGHT below row 0 must do nothing rather than fall
        // through to the shared `cursorColumn`.
        case ScreenType::SCALE:
            if (s.scaleCursorRow == SCALE_NAME_ROW && s.scaleCursorColumn > 0) s.scaleCursorColumn--;
            return;

        // The panel's cells first, then back to the tick column (the tick row is untouched).
        case ScreenType::GROOVE:
            if (s.grooveCursorColumn != GROOVE_COL_PANEL) return;
            if (s.groovePanelColumn > 0) {
                s.groovePanelColumn--;
            } else {
                s.grooveCursorColumn = GROOVE_COL_TICK;
            }
            return;

        case ScreenType::INSTRUMENT: {
            const int minColumn = detail::instrument_left_column(
                detail::instrument_type_of(s), s.instrumentCursorRow, s.instrumentCursorColumn);
            if (s.instrumentCursorColumn > minColumn) s.instrumentCursorColumn = minColumn;
            return;
        }

        case ScreenType::MODS: {
            // LEFT/RIGHT change WHICH SLOT of the pair you edit; the row is clamped into the new slot's
            // depth (a 7-row ADSR beside a 1-row NONE).
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
                // The meter row WRAPS: track 0 → master, master → track 7.
                s.mixerCursorColumn = (s.mixerCursorColumn > 0) ? s.mixerCursorColumn - 1 : 8;
            } else if (s.mixerMasterRow == 1 && s.mixerCursorColumn == 1) {
                s.mixerCursorColumn = 0;   // DEL → REV
            } else if (s.mixerCursorColumn == 8) {
                // The whole master strip exits LEFT onto the DEL send — the strip's door.
                s.mixerMasterRow    = 1;
                s.mixerCursorColumn = 1;
            }
            // REV (row 1, column 0): nothing to its left — stay.
            return;

        // Toward column 1, never 0: column 0 is the row LABEL.
        case ScreenType::PROJECT:
            if (s.projectCursorColumn > 1) s.projectCursorColumn--;
            return;

        // ⚠️ SETTINGS SNAPS to the first column: there are only ever two.
        case ScreenType::SETTINGS:
            s.settingsCursorColumn = 1;
            return;

        // ⚠️ EFFECTS has no column in AppState — the row IS the column. A sideways move is a move to the
        // row beside (this row again on a single-cell line).
        case ScreenType::EFFECTS:
            s.effectsCursorRow = effects_step_column(s.effectsCursorRow, -1);
            return;

        // MIDI is one column wide: the label column is not a stop.
        case ScreenType::MIDI:
            return;

        // ⚠️ Through the clamp, not `--`: the SCOPE cell is not drawn on a destination without one.
        case ScreenType::MIDI_MAP:
            if (s.project && s.midiMapCursorColumn > 1)
                s.midiMapCursorColumn = midi_map_clamp_column(
                    *s.project, s.midiMapCursorRow, s.midiMapCursorColumn - 1, /*prefer=*/-1);
            return;

        default:
            break;
    }
    // GROOVE falls through here too, and correctly does nothing: min == max == 0.
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

        case ScreenType::SCALE:
            if (s.scaleCursorRow == SCALE_NAME_ROW &&
                s.scaleCursorColumn < SCALE_NAME_COL_COUNT - 1)
                s.scaleCursorColumn++;
            return;

        // ⚠️ RIGHT from ANY of the sixteen tick rows enters the panel — it is four rows tall, and
        // anchoring the door to those four would leave most rows with no way in. The panel keeps its
        // last row.
        case ScreenType::GROOVE:
            if (s.grooveCursorColumn != GROOVE_COL_PANEL) {
                s.grooveCursorColumn = GROOVE_COL_PANEL;
            } else if (s.groovePanelColumn < groove_panel_cell_count(s.groovePanelRow) - 1) {
                s.groovePanelColumn++;
            }
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

        // PROJECT steps within the row's own column count: 20 on NAME (one per character), 3 on
        // PROJECT, 2 on EXPORT and COMPACT, 1 elsewhere.
        case ScreenType::PROJECT: {
            const int max = project_row_max_column(static_cast<ProjectRow>(s.projectCursorRow));
            if (s.projectCursorColumn < max) s.projectCursorColumn++;
            return;
        }

        // ⚠️ SETTINGS SNAPS to column 2 — if the row has one, which is caps-dependent (TRACE's ENG column
        // only exists where there is a second sequencer).
        case ScreenType::SETTINGS: {
            const SettingsRow row = static_cast<SettingsRow>(s.settingsCursorRow);
            const bool hasSkins   = s.settings.skinCount > 0;
            if (settings_row_has_second_column(row, s.caps, hasSkins) && s.settingsCursorColumn < 2)
                s.settingsCursorColumn = 2;
            return;
        }

        // …and right, onto a section's second column. See the matching arm in move_cursor_left.
        case ScreenType::EFFECTS:
            s.effectsCursorRow = effects_step_column(s.effectsCursorRow, +1);
            return;

        case ScreenType::MIDI:
            return;

        // ⚠️ The bound is the ROW's (ADD has one cell), and the step goes through the clamp because the
        // SCOPE cell is not drawn on every destination.
        case ScreenType::MIDI_MAP:
            if (s.project &&
                s.midiMapCursorColumn < midi_map_max_column(*s.project, s.midiMapCursorRow))
                s.midiMapCursorColumn = midi_map_clamp_column(
                    *s.project, s.midiMapCursorRow, s.midiMapCursorColumn + 1, /*prefer=*/+1);
            return;

        default:
            break;
    }
    const int maxColumn = max_cursor_column(s.currentScreen);
    if (s.cursorColumn < maxColumn) s.cursorColumn++;
}

}  // namespace pt::ui
