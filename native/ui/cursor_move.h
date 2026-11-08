#pragma once

// What the D-pad alone does, per screen. Rows wrap and columns clamp, except where noted:
//   • SONG's 256 rows clamp too; column 0 (the gutter) is never reachable.
//   • INSTRUMENT walks the row table (ui/instrument_row_layout.h), skipping spacers.
//   • MODS is (pair, side, row); a slot's depth depends on its type.
//   • MIXER: tracks on row 0 (a ring with the master), the sends on row 1, the master strip down column 8.
//   • EFFECTS walks in drawn order and has no column: LEFT/RIGHT move to the row beside.
//   • PROJECT, SETTINGS and MIDI snap the column to 1 on every row change.
//   • Under NAV = SONG, PHRASE spills over to the next filled chain row's phrase (ui/song_pointer.h).

#include <algorithm>

#include "ui/app_state.h"
#include "ui/effects_row_layout.h"
#include "ui/grid_cursor.h"
#include "ui/instrument_row_layout.h"
#include "ui/modules/groove_editor.h"
#include "ui/modules/midi_map_editor.h"
#include "ui/modules/scale_editor.h"
#include "ui/settings_row_layout.h"
#include "ui/song_pointer.h"

namespace pt::ui {

// ─── INSTRUMENT ──────────────────────────────────────────────────────────────────────────────────

namespace detail {

// One step of ±1 over 0..count−1, wrapping at both ends.
inline int wrap_step(int v, int delta, int count) {
    return delta < 0 ? (v > 0 ? v - 1 : count - 1) : (v < count - 1 ? v + 1 : 0);
}

// One step of ±1, not taken past lo going down or past hi going up.
inline void nudge(int& v, int delta, int lo, int hi) {
    if (delta < 0 && v > lo) v--;
    if (delta > 0 && v < hi) v++;
}

// Which row table the cursor is walking.
inline songcore::InstrumentType instrument_type_of(const AppState& s) {
    return s.project->instruments[static_cast<size_t>(s.currentInstrument)].instrumentType;
}

// Step ±1 rows, wrapping, over the spacers.
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

// The column after a vertical move: keep it if the new row has one like it, else 1. SOURCE snaps to LOAD.
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
            // EDIT (column 3) exists on samplers only.
            return (has_right(old) && old_column >= 3 &&
                    instrument_name_row_max_column(type) >= 3)
                       ? 3
                       : 1;

        default:
            return 1;   // SINGLE / SPACER
    }
}

// The next column left of `column` on this row.
inline int instrument_left_column(songcore::InstrumentType type, int row, int column) {
    switch (instrument_row_kind(type, row)) {
        case InstrumentRowKind::NAME:   return column - 1 < 1 ? 1 : column - 1;  // 3→2→1
        case InstrumentRowKind::SOURCE: return column - 1 < 2 ? 2 : column - 1;  // 3→2, never below LOAD
        case InstrumentRowKind::TRIPLE: return column - 2 < 1 ? 1 : column - 2;  // 5→3→1
        case InstrumentRowKind::DUAL:   return 1;                                // 3→1, in one jump
        default:                        return 1;
    }
}

// …and right.
inline int instrument_right_column(songcore::InstrumentType type, int row, int column) {
    switch (instrument_row_kind(type, row)) {
        case InstrumentRowKind::NAME: {
            const int cap = instrument_name_row_max_column(type);
            const int c   = column + 1;
            return c > cap ? cap : c;
        }
        case InstrumentRowKind::SOURCE: {
            const int c = column + 1;   // the preset row: SAVE and LOAD on every type
            return c > 3 ? 3 : c;
        }
        case InstrumentRowKind::TRIPLE: return column + 2 > 5 ? 5 : column + 2;  // 1→3→5
        case InstrumentRowKind::DUAL:   return 3;                                // 1→3, in one jump
        default:                        return 1;
    }
}

// MODS: how many rows the slot at (pair, side) has.
inline int mod_slot_rows(const AppState& s, int pair, int side) {
    const songcore::Instrument& ins =
        s.project->instruments[static_cast<size_t>(s.currentInstrument)];
    return songcore::mod_slot_row_count(ins.modSlots[static_cast<size_t>(pair * 2 + side)]);
}

// INST.POOL: the row is the selected instrument; wraps 00↔7F.
inline void move_pool_selection(AppState& s, int delta) {
    const int size = static_cast<int>(s.project->instruments.size());
    s.currentInstrument    = ((s.currentInstrument + delta) % size + size) % size;
    s.lastEditedInstrument = s.currentInstrument;
}

}  // namespace detail

// Put the INSTRUMENT cursor back on a cell after the instrument under it changed type.
inline void snap_instrument_cursor(AppState& s) {
    const InstrumentCursor c = instrument_nearest_cell(detail::instrument_type_of(s), s.instrumentCursor.row,
                                                       s.instrumentCursor.column);
    s.instrumentCursor.row    = c.row;
    s.instrumentCursor.column = c.column;
}


// ─── SAMPLE EDITOR ───────────────────────────────────────────────────────────────────────────────
namespace detail {

inline void sample_editor_step_row(AppState& s, int delta) {
    SampleEditorState& se = s.sampleEditor;
    se.cursorRow = SampleEditorModule::row_step(se.cursorRow, delta, se.sliceMethod);
    se.cursorCol = std::min(se.cursorCol, SampleEditorModule::max_col(se.cursorRow, se.sliceMethod));
}

// The two op rows wrap; every other row clamps.
inline void sample_editor_step_col(AppState& s, int delta) {
    SampleEditorState& se     = s.sampleEditor;
    const int          maxCol = SampleEditorModule::max_col(se.cursorRow, se.sliceMethod);
    const bool         isOps  = (se.cursorRow == SampleRow::OPS1 || se.cursorRow == SampleRow::OPS2);

    if (isOps) {
        se.cursorCol = (delta < 0) ? ((se.cursorCol == 0) ? maxCol : se.cursorCol - 1)
                                   : ((se.cursorCol + 1) % (maxCol + 1));
    } else {
        se.cursorCol = std::clamp(se.cursorCol + delta, 0, maxCol);
    }
}

inline void instrument_step_row(AppState& s, int delta) {
    const songcore::InstrumentType ty = instrument_type_of(s);
    const int oldRow = s.instrumentCursor.row;
    s.instrumentCursor.row    = instrument_row_step(ty, oldRow, delta);
    s.instrumentCursor.column =
        instrument_column_for(ty, s.instrumentCursor.row, oldRow, s.instrumentCursor.column);
}

// MODS: the other slot of the pair; the row clamps to its depth.
inline void mod_select_side(AppState& s, int side) {
    s.modCursorSide = side;
    const int rows  = mod_slot_rows(s, s.modCursorPair, side);
    const int max   = rows - 1 < 0 ? 0 : rows - 1;
    if (s.modCursorRow > max) s.modCursorRow = max;
}

// PHRASE: the 16-row loop. Under NAV = SONG, off either end the pointer moves to the next filled
// chain row (a chain with one filled row answers `from`).
inline void phrase_step_row(AppState& s, int delta) {
    const int edge = delta < 0 ? 0 : PHRASE_GRID.maxRow;
    int& row = s.phraseCursor.row;
    if (s.settings.navSongRelative && row == edge) {
        const int from = s.chainCursor.row;
        const int to   = songcore::next_chain_row(*s.project, s.currentChain, from, delta,
                                                 /*wrap=*/true);
        if (to != from) { s.chainCursor.row = to; refresh_song_relative_refs(s); }
    }
    row = wrap_step(row, delta, PHRASE_GRID.maxRow + 1);
}

}  // namespace detail

// ─── Each screen's step: `dr` and `dc` are −1, 0 or +1, one of them 0 ───────────────────────────────
namespace detail {

// SONG's rows and columns clamp; the window follows the row.
inline void song_step(AppState& s, int dr, int dc) {
    nudge(s.songCursor.row, dr, 0, SONG_GRID.maxRow);
    nudge(s.songCursor.column, dc, 1, SONG_GRID.maxColumn);
    if (dr) scroll_song_to_row(s, s.songCursor.row);
}

// CHAIN and TABLE: rows wrap, columns clamp.
inline void grid_step(AppState& s, const GridCursor& g, int dr, int dc) {
    CursorPosition& c = s.*g.cursor;
    if (dr) c.row = wrap_step(c.row, dr, g.maxRow + 1);
    nudge(c.column, dc, 1, g.maxColumn);
}

inline void phrase_step(AppState& s, int dr, int dc) {
    if (dr) phrase_step_row(s, dr);
    nudge(s.phraseCursor.column, dc, 1, PHRASE_GRID.maxColumn);
}

// The panel wraps within itself and the tick row stays put; RIGHT from any tick row enters the panel,
// LEFT walks the panel's cells, then back to the tick column.
inline void groove_step(AppState& s, int dr, int dc) {
    const bool inPanel = s.grooveCursor.column == GROOVE_COL_PANEL;
    if (dr && inPanel) {
        s.groovePanel.row    = wrap_step(s.groovePanel.row, dr, GROOVE_PANEL_ROWS);
        s.groovePanel.column = 0;
    } else if (dr) {
        s.grooveCursor.row = wrap_step(s.grooveCursor.row, dr, 16);
    } else if (dc > 0 && !inPanel) {
        s.grooveCursor.column = GROOVE_COL_PANEL;
    } else if (dc > 0) {
        nudge(s.groovePanel.column, +1, 0, groove_panel_cell_count(s.groovePanel.row) - 1);
    } else if (dc < 0 && inPanel) {
        if (s.groovePanel.column > 0) s.groovePanel.column--;
        else                          s.grooveCursor.column = GROOVE_COL_TICK;
    }
}

// Only the NAME row has columns.
inline void scale_step(AppState& s, int dr, int dc) {
    if (dr) s.scaleCursor.row = wrap_step(s.scaleCursor.row, dr, SCALE_ROW_COUNT);
    if (s.scaleCursor.row == SCALE_NAME_ROW) nudge(s.scaleCursor.column, dc, 0, SCALE_NAME_COL_COUNT - 1);
}

inline void instrument_step(AppState& s, int dr, int dc) {
    if (dr) { instrument_step_row(s, dr); return; }
    const songcore::InstrumentType ty = instrument_type_of(s);
    int& column = s.instrumentCursor.column;
    if (dc < 0) column = std::min(column, instrument_left_column(ty, s.instrumentCursor.row, column));
    if (dc > 0) column = std::max(column, instrument_right_column(ty, s.instrumentCursor.row, column));
}

// Up out of a pair: the bottom of the pair above; down off a pair's bottom: the top of the next.
inline void mods_step(AppState& s, int dr, int dc) {
    if (dc) { mod_select_side(s, dc < 0 ? 0 : 1); return; }
    const int rows = mod_slot_rows(s, s.modCursorPair, s.modCursorSide);
    if (dr < 0 && s.modCursorRow > 0) {
        s.modCursorRow--;
    } else if (dr < 0 && s.modCursorPair > 0) {
        s.modCursorPair = 0;
        s.modCursorRow  = std::max(0, mod_slot_rows(s, 0, s.modCursorSide) - 1);
    } else if (dr > 0 && s.modCursorRow < rows - 1) {
        s.modCursorRow++;
    } else if (dr > 0 && s.modCursorPair < 1) {
        s.modCursorPair = 1;
        s.modCursorRow  = 0;
    }
}

inline void pool_step(AppState& s, int dr, int dc) {
    if (dr) move_pool_selection(s, dr);
    nudge(s.poolCursorColumn, dc, 0, 4);
}

// Tracks on row 0, a ring with the master; the sends on row 1; the master strip down column 8.
inline void mixer_step(AppState& s, int dr, int dc) {
    int& row = s.mixerMasterRow;
    int& col = s.mixerCursorColumn;
    if (dr < 0) {
        if (row == 1 && (col == 0 || col == 1)) { row = 0; col = 0; }   // out of a send onto the first track
        else if (row > 0) row--;                                         // up the master strip
    } else if (dr > 0) {
        if (row == 0 && col < 8)        { row = 1; col = 0; }   // any track meter → the REV send
        else if (col == 8 && row < 3)   row++;                  // MIX → EQ → OTT|DUST → LIM
    } else if (row == 0) {
        col = wrap_step(col, dc, 9);
    } else if (dc < 0) {
        if (row == 1 && col == 1) col = 0;                  // DEL → REV
        else if (col == 8)        { row = 1; col = 1; }     // the master strip exits onto the DEL send
    } else {
        if (row == 1 && col == 0)      col = 1;             // REV → DEL
        else if (row == 1 && col == 1) col = 8;             // DEL → the master strip, at the EQ row
    }
}

// One drawn line per step, clamped; no column: LEFT/RIGHT move to the row beside.
inline void effects_step(AppState& s, int dr, int dc) {
    if (dr) s.effectsCursorRow = effects_next_row(s.effectsCursorRow, dr);
    if (dc) s.effectsCursorRow = effects_step_column(s.effectsCursorRow, dc);
}

// Column 0 is the label; a row change snaps the column to 1.
inline void project_step(AppState& s, int dr, int dc) {
    if (dr) s.projectCursor = {project_next_visible_row(s.projectCursor.row, dr, s.caps), 1};
    nudge(s.projectCursor.column, dc, 1, project_row_max_column(static_cast<ProjectRow>(s.projectCursor.row)));
}

// Two columns at most; a row change snaps to the first.
inline void settings_step(AppState& s, int dr, int dc) {
    CursorPosition& c = s.settingsCursor;
    if (dr) c = {settings_next_visible_row(c.row, dr, s.caps), 1};
    if (dc < 0) c.column = 1;
    if (dc > 0 && settings_row_has_second_column(static_cast<SettingsRow>(c.row), s.caps,
                                                 s.settings.skinCount > 0, s.settings.oneHanded))
        c.column = std::max(c.column, 2);
}

inline void midi_step(AppState& s, int dr, int) {
    if (dr) s.midiCursor = {wrap_step(s.midiCursor.row, dr, MIDI_ROW_COUNT), 1};
}

// The column is carried, then clamped: ADD has one cell, and not every destination has a SCOPE cell.
inline void midi_map_step(AppState& s, int dr, int dc) {
    if (!s.project) return;
    const songcore::Project& p = *s.project;
    CursorPosition& c = s.midiMapCursor;
    if (dr) {
        c.row    = wrap_step(c.row, dr, midi_map_row_count(p));
        c.column = midi_map_clamp_column(p, c.row, c.column);
    } else if (dc < 0 && c.column > 1) {
        c.column = midi_map_clamp_column(p, c.row, c.column - 1, /*prefer=*/-1);
    } else if (dc > 0 && c.column < midi_map_max_column(p, c.row)) {
        c.column = midi_map_clamp_column(p, c.row, c.column + 1, /*prefer=*/+1);
    }
}

inline void sample_editor_step(AppState& s, int dr, int dc) {
    if (dr) sample_editor_step_row(s, dr);
    if (dc) sample_editor_step_col(s, dc);
}

}  // namespace detail

inline void move_cursor(AppState& s, NavDir dir) {
    const int dr = dir == NavDir::UP ? -1 : dir == NavDir::DOWN ? 1 : 0;
    const int dc = dir == NavDir::LEFT ? -1 : dir == NavDir::RIGHT ? 1 : 0;
    using namespace detail;
    switch (s.currentScreen) {
        case ScreenType::SONG:          song_step(s, dr, dc); break;
        case ScreenType::CHAIN:         grid_step(s, CHAIN_GRID, dr, dc); break;
        case ScreenType::PHRASE:        phrase_step(s, dr, dc); break;
        case ScreenType::TABLE:         grid_step(s, TABLE_GRID, dr, dc); break;
        case ScreenType::GROOVE:        groove_step(s, dr, dc); break;
        case ScreenType::SCALE:         scale_step(s, dr, dc); break;
        case ScreenType::INSTRUMENT:    instrument_step(s, dr, dc); break;
        case ScreenType::MODS:          mods_step(s, dr, dc); break;
        case ScreenType::INST_POOL:     pool_step(s, dr, dc); break;
        case ScreenType::MIXER:         mixer_step(s, dr, dc); break;
        case ScreenType::EFFECTS:       effects_step(s, dr, dc); break;
        case ScreenType::PROJECT:       project_step(s, dr, dc); break;
        case ScreenType::SETTINGS:      settings_step(s, dr, dc); break;
        case ScreenType::MIDI:          midi_step(s, dr, dc); break;
        case ScreenType::MIDI_MAP:      midi_map_step(s, dr, dc); break;
        case ScreenType::SAMPLE_EDITOR: sample_editor_step(s, dr, dc); break;
        case ScreenType::FILE_BROWSER:  break;   // its own handler moves it
    }
}

}  // namespace pt::ui
