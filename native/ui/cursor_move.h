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
#include "ui/instrument_row_layout.h"
#include "ui/modules/groove_editor.h"
#include "ui/modules/midi_map_editor.h"
#include "ui/modules/scale_editor.h"
#include "ui/settings_row_layout.h"
#include "ui/song_pointer.h"

namespace pt::ui {

// ─── INSTRUMENT ──────────────────────────────────────────────────────────────────────────────────

namespace detail {

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
    const InstrumentCursor c = instrument_nearest_cell(detail::instrument_type_of(s), s.instrumentCursorRow,
                                                       s.instrumentCursorColumn);
    s.instrumentCursorRow    = c.row;
    s.instrumentCursorColumn = c.column;
}


// ─── SAMPLE EDITOR: sparse rows (ui/modules/sample_editor.h), so a step is a lookup ──────────────
namespace detail {

inline void sample_editor_step_row(AppState& s, int delta) {
    SampleEditorState& se = s.sampleEditor;
    const int newRow = (delta < 0) ? SampleEditorModule::row_above(se.cursorRow, se.sliceMethod)
                                   : SampleEditorModule::row_below(se.cursorRow, se.sliceMethod);
    se.cursorRow = newRow;
    se.cursorCol = std::min(se.cursorCol,
                            SampleEditorModule::max_col_for_row(newRow, se.sliceMethod));
}

// The two op rows (13, 14) wrap; every other row clamps.
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

inline void instrument_step_row(AppState& s, int delta) {
    const songcore::InstrumentType ty = instrument_type_of(s);
    const int oldRow = s.instrumentCursorRow;
    s.instrumentCursorRow    = instrument_row_step(ty, oldRow, delta);
    s.instrumentCursorColumn =
        instrument_column_for(ty, s.instrumentCursorRow, oldRow, s.instrumentCursorColumn);
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
    const int edge = delta < 0 ? 0 : 15;
    if (s.settings.navSongRelative && s.cursorRow == edge) {
        const int from = pointer_chain_row(s);
        const int to   = songcore::next_chain_row(*s.project, s.currentChain, from, delta,
                                                 /*wrap=*/true);
        if (to != from) { set_pointer_chain_row(s, to); refresh_song_relative_refs(s); }
    }
    if (delta < 0) s.cursorRow = (s.cursorRow > 0) ? s.cursorRow - 1 : 15;
    else           s.cursorRow = (s.cursorRow < 15) ? s.cursorRow + 1 : 0;
}

}  // namespace detail

inline void move_cursor_up(AppState& s) {
    switch (s.currentScreen) {
        case ScreenType::SAMPLE_EDITOR:
            detail::sample_editor_step_row(s, -1);
            break;

        case ScreenType::SONG:
            if (s.cursorRow > 0) {
                s.cursorRow--;
                if (s.cursorRow < s.songScrollPosition) s.songScrollPosition = s.cursorRow;
            }
            break;
        case ScreenType::TABLE:
            s.tableCursorRow = (s.tableCursorRow > 0) ? s.tableCursorRow - 1 : 15;
            break;
        // The panel wraps within itself; the tick row stays put.
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

        case ScreenType::INSTRUMENT:
            detail::instrument_step_row(s, -1);
            break;

        case ScreenType::MODS:
            // Up out of a pair: the bottom of the pair above.
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
            // Out of a send, UP lands on the first track.
            if (s.mixerMasterRow == 1 && (s.mixerCursorColumn == 0 || s.mixerCursorColumn == 1)) {
                s.mixerMasterRow    = 0;
                s.mixerCursorColumn = 0;
            } else if (s.mixerMasterRow > 0) {
                s.mixerMasterRow--;   // up the master strip
            }
            break;

        // One drawn line per step; clamps (ui/effects_row_layout.h).
        case ScreenType::EFFECTS:
            s.effectsCursorRow = effects_next_row(s.effectsCursorRow, -1);
            break;

        case ScreenType::PROJECT:
            s.projectCursorRow    = project_next_visible_row(s.projectCursorRow, -1, s.caps);
            s.projectCursorColumn = 1;
            break;

        case ScreenType::SETTINGS:
            s.settingsCursorRow    = settings_next_visible_row(s.settingsCursorRow, -1, s.caps);
            s.settingsCursorColumn = 1;
            break;

        case ScreenType::MIDI:
            s.midiCursorRow    = (s.midiCursorRow > 0) ? s.midiCursorRow - 1 : MIDI_ROW_COUNT - 1;
            s.midiCursorColumn = 1;
            break;

        // The column is carried, then clamped (ADD has one cell; some destinations have no scope cell).
        case ScreenType::MIDI_MAP:
            if (s.project) {
                const int rows = midi_map_row_count(*s.project);
                s.midiMapCursorRow = (s.midiMapCursorRow > 0) ? s.midiMapCursorRow - 1 : rows - 1;
                s.midiMapCursorColumn =
                    midi_map_clamp_column(*s.project, s.midiMapCursorRow, s.midiMapCursorColumn);
            }
            break;

        case ScreenType::PHRASE:
            detail::phrase_step_row(s, -1);
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

        case ScreenType::INSTRUMENT:
            detail::instrument_step_row(s, +1);
            break;

        case ScreenType::MODS: {
            const int rows = detail::mod_slot_rows(s, s.modCursorPair, s.modCursorSide);
            if (s.modCursorRow < rows - 1) {
                s.modCursorRow++;
            } else if (s.modCursorPair < 1) {
                s.modCursorPair = 1;
                s.modCursorRow  = 0;
            }
            break;
        }

        case ScreenType::INST_POOL:
            detail::move_pool_selection(s, +1);
            break;

        case ScreenType::MIXER:
            if (s.mixerMasterRow == 0 && s.mixerCursorColumn < 8) {
                s.mixerMasterRow    = 1;   // any track meter → the REV send
                s.mixerCursorColumn = 0;
            } else if (s.mixerCursorColumn == 8 && s.mixerMasterRow < 3) {
                s.mixerMasterRow++;   // MIX → EQ → OTT|DUST → LIM
            }
            break;

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

        case ScreenType::MIDI_MAP:
            if (s.project) {
                const int rows = midi_map_row_count(*s.project);
                s.midiMapCursorRow = (s.midiMapCursorRow < rows - 1) ? s.midiMapCursorRow + 1 : 0;
                s.midiMapCursorColumn =
                    midi_map_clamp_column(*s.project, s.midiMapCursorRow, s.midiMapCursorColumn);
            }
            break;

        case ScreenType::PHRASE:
            detail::phrase_step_row(s, +1);
            break;

        default:
            s.cursorRow = (s.cursorRow < 15) ? s.cursorRow + 1 : 0;
            break;
    }
}

// The leftmost column on the shared cursor's screens (column 0 is the gutter).
inline int min_cursor_column(ScreenType s) {
    switch (s) {
        case ScreenType::SONG:
        case ScreenType::CHAIN:
        case ScreenType::PHRASE: return 1;
        default: return 0;
    }
}

// The rightmost; SONG's is a track number.
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

        // Only the NAME row has columns.
        case ScreenType::SCALE:
            if (s.scaleCursorRow == SCALE_NAME_ROW && s.scaleCursorColumn > 0) s.scaleCursorColumn--;
            return;

        // The panel's cells, then back to the tick column.
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

        case ScreenType::MODS:
            detail::mod_select_side(s, 0);
            return;

        case ScreenType::INST_POOL:
            if (s.poolCursorColumn > 0) s.poolCursorColumn--;
            return;

        case ScreenType::MIXER:
            if (s.mixerMasterRow == 0) {
                s.mixerCursorColumn = (s.mixerCursorColumn > 0) ? s.mixerCursorColumn - 1 : 8;
            } else if (s.mixerMasterRow == 1 && s.mixerCursorColumn == 1) {
                s.mixerCursorColumn = 0;   // DEL → REV
            } else if (s.mixerCursorColumn == 8) {
                s.mixerMasterRow    = 1;   // the master strip exits onto the DEL send
                s.mixerCursorColumn = 1;
            }
            return;

        case ScreenType::PROJECT:   // column 0 is the label
            if (s.projectCursorColumn > 1) s.projectCursorColumn--;
            return;

        case ScreenType::SETTINGS:   // two columns at most
            s.settingsCursorColumn = 1;
            return;

        case ScreenType::EFFECTS:
            s.effectsCursorRow = effects_step_column(s.effectsCursorRow, -1);
            return;

        case ScreenType::MIDI:
            return;

        // Through the clamp: the SCOPE cell is not drawn on every destination.
        case ScreenType::MIDI_MAP:
            if (s.project && s.midiMapCursorColumn > 1)
                s.midiMapCursorColumn = midi_map_clamp_column(
                    *s.project, s.midiMapCursorRow, s.midiMapCursorColumn - 1, /*prefer=*/-1);
            return;

        default:
            break;
    }
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

        // RIGHT from any tick row enters the panel, on the panel's own last row.
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

        case ScreenType::MODS:
            detail::mod_select_side(s, 1);
            return;

        case ScreenType::INST_POOL:
            if (s.poolCursorColumn < 4) s.poolCursorColumn++;
            return;

        case ScreenType::MIXER:
            if (s.mixerMasterRow == 0) {
                s.mixerCursorColumn = (s.mixerCursorColumn < 8) ? s.mixerCursorColumn + 1 : 0;
            } else if (s.mixerMasterRow == 1 && s.mixerCursorColumn == 0) {
                s.mixerCursorColumn = 1;   // REV → DEL
            } else if (s.mixerMasterRow == 1 && s.mixerCursorColumn == 1) {
                s.mixerCursorColumn = 8;   // DEL → the master strip, at the EQ row
            }
            return;

        case ScreenType::PROJECT: {
            const int max = project_row_max_column(static_cast<ProjectRow>(s.projectCursorRow));
            if (s.projectCursorColumn < max) s.projectCursorColumn++;
            return;
        }

        case ScreenType::SETTINGS: {
            const SettingsRow row = static_cast<SettingsRow>(s.settingsCursorRow);
            const bool hasSkins   = s.settings.skinCount > 0;
            if (settings_row_has_second_column(row, s.caps, hasSkins) && s.settingsCursorColumn < 2)
                s.settingsCursorColumn = 2;
            return;
        }

        case ScreenType::EFFECTS:
            s.effectsCursorRow = effects_step_column(s.effectsCursorRow, +1);
            return;

        case ScreenType::MIDI:
            return;

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
