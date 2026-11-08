#include "ui/help_topic.h"

#include <cstddef>

#include "ui/app_state.h"
#include "ui/effects_row_layout.h"
#include "ui/fx_columns.h"
#include "ui/instrument_row_layout.h"
#include "ui/mixer_cell_layout.h"
#include "ui/modules/groove_editor.h"
#include "ui/modules/midi_map_editor.h"
#include "ui/modules/modulation.h"
#include "ui/modules/sample_editor.h"
#include "ui/modules/scale_editor.h"
#include "ui/modules/theme_editor.h"
#include "ui/screen.h"
#include "ui/settings_row_layout.h"

namespace pt::ui {

namespace {

// The screen-level fallback.
HelpTopic help_screen_topic(ScreenType screen) {
    switch (screen) {
        case ScreenType::SONG:          return HelpTopic::SCREEN_SONG;
        case ScreenType::CHAIN:         return HelpTopic::SCREEN_CHAIN;
        case ScreenType::PHRASE:        return HelpTopic::SCREEN_PHRASE;
        case ScreenType::INSTRUMENT:    return HelpTopic::SCREEN_INSTRUMENT;
        case ScreenType::TABLE:         return HelpTopic::SCREEN_TABLE;
        case ScreenType::PROJECT:       return HelpTopic::SCREEN_PROJECT;
        case ScreenType::GROOVE:        return HelpTopic::SCREEN_GROOVE;
        case ScreenType::SCALE:         return HelpTopic::SCREEN_SCALE;
        case ScreenType::MODS:          return HelpTopic::SCREEN_MODS;
        case ScreenType::INST_POOL:     return HelpTopic::SCREEN_INST_POOL;
        case ScreenType::MIXER:         return HelpTopic::SCREEN_MIXER;
        case ScreenType::EFFECTS:       return HelpTopic::SCREEN_EFFECTS;
        case ScreenType::FILE_BROWSER:  return HelpTopic::SCREEN_FILE_BROWSER;
        case ScreenType::SETTINGS:      return HelpTopic::SCREEN_SETTINGS;
        case ScreenType::SAMPLE_EDITOR: return HelpTopic::SCREEN_SAMPLE_EDITOR;
        case ScreenType::MIDI:          return HelpTopic::SCREEN_MIDI;
        case ScreenType::MIDI_MAP:      return HelpTopic::SCREEN_MIDI_MAP;
    }
    return HelpTopic::NONE;
}

// PHRASE: note, volume, instrument, then the commands (column 0, the step number, is unreachable).
HelpTopic phrase_cell_topic(int column) {
    if (fx_slot_of_column(column, PHRASE_FX_COLUMN) != 0)
        return is_fx_type_column(column, PHRASE_FX_COLUMN) ? HelpTopic::PHRASE_FX_TYPE
                                                           : HelpTopic::PHRASE_FX_VALUE;
    switch (column) {
        case 1:  return HelpTopic::PHRASE_NOTE;
        case 2:  return HelpTopic::PHRASE_VOLUME;
        case 3:  return HelpTopic::PHRASE_INSTRUMENT;
        default: return HelpTopic::NONE;
    }
}

// TABLE: transpose, volume, then the commands — no instrument cell.
HelpTopic table_cell_topic(int column) {
    if (fx_slot_of_column(column, TABLE_FX_COLUMN) != 0)
        return is_fx_type_column(column, TABLE_FX_COLUMN) ? HelpTopic::TABLE_FX_TYPE
                                                          : HelpTopic::TABLE_FX_VALUE;
    switch (column) {
        case 1:  return HelpTopic::TABLE_TRANSPOSE;
        case 2:  return HelpTopic::TABLE_VOLUME;
        default: return HelpTopic::NONE;
    }
}

// INSTRUMENT, every type: the cell the type's table puts at (row, column).
HelpTopic instrument_topic(songcore::InstrumentType type, int row, int column) {
    using Cell = InstrumentCell;
    switch (instrument_cell_at(type, row, column)) {
        case Cell::NONE:         return HelpTopic::NONE;
        case Cell::TYPE:         return HelpTopic::INST_TYPE;
        case Cell::SOURCE_LOAD:  return HelpTopic::INST_SOURCE_LOAD;
        case Cell::SOURCE_EDIT:  return HelpTopic::INST_SOURCE_EDIT;
        case Cell::NAME:         return HelpTopic::INST_NAME;
        case Cell::PRESET_SAVE:  return HelpTopic::INST_PRESET_SAVE;
        case Cell::PRESET_LOAD:  return HelpTopic::INST_PRESET_LOAD;
        case Cell::VOL:          return HelpTopic::INST_VOLUME;
        case Cell::PAN:          return HelpTopic::INST_PAN;
        case Cell::TSP:          return HelpTopic::INST_TRANSPOSE;
        case Cell::TIC:          return HelpTopic::INST_TIC;
        case Cell::ROOT:         return HelpTopic::INST_ROOT;
        case Cell::DETUNE:       return HelpTopic::INST_DETUNE;
        case Cell::DRIVE:        return HelpTopic::INST_DRIVE;
        case Cell::FILTER:       return HelpTopic::INST_FILTER;
        case Cell::CRUSH:        return HelpTopic::INST_CRUSH;
        case Cell::FREQ:         return HelpTopic::INST_FILTER_FREQ;
        case Cell::DOWNSAMPLE:   return HelpTopic::INST_DOWNSAMPLE;
        case Cell::RES:          return HelpTopic::INST_FILTER_RES;
        case Cell::REV:          return HelpTopic::INST_REVERB_SEND;
        case Cell::DEL:          return HelpTopic::INST_DELAY_SEND;
        case Cell::EQ:           return HelpTopic::INST_EQ;
        case Cell::PATCH:        return HelpTopic::INST_PATCH;
        case Cell::SLICE:        return HelpTopic::INST_SLICE;
        case Cell::LOOP:         return HelpTopic::INST_LOOP_MODE;
        case Cell::START:        return HelpTopic::INST_SAMPLE_START;
        case Cell::LOOP_START:   return HelpTopic::INST_LOOP_START;
        case Cell::END:          return HelpTopic::INST_SAMPLE_END;
        case Cell::LOOP_END:     return HelpTopic::INST_LOOP_END;
        case Cell::REVERSE:      return HelpTopic::INST_REVERSE;
        case Cell::MIDI_CHANNEL: return HelpTopic::INST_MIDI_CHANNEL;
        case Cell::MIDI_BANK:    return HelpTopic::INST_MIDI_BANK;
        case Cell::MIDI_PROGRAM: return HelpTopic::INST_MIDI_PROGRAM;
        case Cell::MIDI_LENGTH:  return HelpTopic::INST_MIDI_LENGTH;
        case Cell::CC_NUMBER:    return HelpTopic::INST_MIDI_CC_NUMBER;
        case Cell::CC_VALUE:     return HelpTopic::INST_MIDI_CC_VALUE;
        case Cell::MIDI_BEND:    return HelpTopic::INST_MIDI_BEND;
    }
    return HelpTopic::NONE;
}

// PROJECT: two values, a name, then rows of buttons, read by column in the draw order
// (`project_editor.cpp`).
HelpTopic project_cell_topic(int row, int column) {
    if (row < 0 || row >= PROJECT_ROW_COUNT) return HelpTopic::NONE;
    switch (static_cast<ProjectRow>(row)) {
        case ProjectRow::TEMPO:
            return column == 2 ? HelpTopic::PROJECT_TAP : HelpTopic::PROJECT_TEMPO;
        case ProjectRow::TRANSPOSE: return HelpTopic::PROJECT_TRANSPOSE;
        case ProjectRow::NAME:      return HelpTopic::PROJECT_NAME;
        case ProjectRow::PROJECT:
            return column == 1   ? HelpTopic::PROJECT_SAVE
                   : column == 2 ? HelpTopic::PROJECT_LOAD
                                 : HelpTopic::PROJECT_NEW;
        case ProjectRow::EXPORT:
            return column == 1 ? HelpTopic::PROJECT_EXPORT_MIX : HelpTopic::PROJECT_EXPORT_STEMS;
        case ProjectRow::COMPACT:
            return column == 1 ? HelpTopic::PROJECT_COMPACT_SEQ : HelpTopic::PROJECT_COMPACT_INST;
        case ProjectRow::SYSTEM:    return HelpTopic::PROJECT_SYSTEM;
        case ProjectRow::MIDI:      return HelpTopic::PROJECT_MIDI;
        case ProjectRow::EXIT:      return HelpTopic::PROJECT_EXIT;
        case ProjectRow::COLLECT:   return HelpTopic::PROJECT_COLLECT;
    }
    return HelpTopic::NONE;
}

// GROOVE: the tick column, and the panel beside it.
HelpTopic groove_cell_topic(int column, int panel_row, int panel_column) {
    if (column != GROOVE_COL_PANEL) return HelpTopic::GROOVE_TIC;
    switch (panel_row) {
        case GROOVE_PANEL_NAME: return HelpTopic::GROOVE_NAME;
        case GROOVE_PANEL_FILE:
            return panel_column == 0 ? HelpTopic::GROOVE_SAVE : HelpTopic::GROOVE_LOAD;
        case GROOVE_PANEL_QNT:  return HelpTopic::GROOVE_QNT;
        case GROOVE_PANEL_SWG:  return HelpTopic::GROOVE_SWG;
        case GROOVE_PANEL_PPQ:  return HelpTopic::GROOVE_PPQ;
        default:                return HelpTopic::NONE;
    }
}

// SCALE: the NAME row has three cells; the rows below are degrees.
HelpTopic scale_cell_topic(int row, int column) {
    if (row == SCALE_NAME_ROW) {
        if (column == SCALE_NAME_COL_SAVE) return HelpTopic::SCALE_SAVE;
        if (column == SCALE_NAME_COL_LOAD) return HelpTopic::SCALE_LOAD;
        return HelpTopic::SCALE_NAME;
    }
    if (row == SCALE_KEY_ROW) return HelpTopic::SCALE_KEY;
    return (scale_row_degree(row) >= 0) ? HelpTopic::SCALE_DEGREE : HelpTopic::NONE;
}

// MODS: a row's meaning follows the slot's type (row 4 is HOLD, DEC or TRIG), asked in the module's
// order (`modulation.cpp`): LFO, then the AHD-shaped pair, then ADSR.
HelpTopic mod_cell_topic(songcore::ModType type, int row) {
    const bool lfo = (type == songcore::ModType::LFO);
    switch (row) {
        case 0: return HelpTopic::MOD_TYPE;
        case 1: return HelpTopic::MOD_DEST;
        case 2: return HelpTopic::MOD_AMOUNT;
        case 3: return lfo ? HelpTopic::MOD_OSC : HelpTopic::MOD_ATTACK;
        case 4:
            if (lfo) return HelpTopic::MOD_TRIG;
            return is_ahd_shaped(type) ? HelpTopic::MOD_HOLD : HelpTopic::MOD_DECAY;
        case 5:
            if (lfo) return HelpTopic::MOD_FREQ;
            return is_ahd_shaped(type) ? HelpTopic::MOD_DECAY : HelpTopic::MOD_SUSTAIN;
        case 6: return HelpTopic::MOD_RELEASE;
        default: return HelpTopic::NONE;
    }
}

// INST.POOL: the value columns are the instrument's own fields and take INSTRUMENT's entries; only the
// name column has text of its own.
HelpTopic pool_cell_topic(int column) {
    switch (column) {
        case 1:  return HelpTopic::INST_VOLUME;
        case 2:  return HelpTopic::INST_REVERB_SEND;
        case 3:  return HelpTopic::INST_DELAY_SEND;
        case 4:  return HelpTopic::INST_EQ;
        default: return HelpTopic::POOL_SLOT;
    }
}

// MIXER: rows 2 and 3 exist only on the master strip (column 8).
HelpTopic mixer_cell_topic(int master_row, int column) {
    switch (mixer_cell_at(master_row, column)) {
        case MixerCell::TRACK_VOL:  return HelpTopic::MIXER_TRACK_VOL;
        case MixerCell::MASTER_VOL: return HelpTopic::MIXER_MASTER_VOL;
        case MixerCell::REV_WET:    return HelpTopic::MIXER_REVERB_RETURN;
        case MixerCell::DLY_WET:    return HelpTopic::MIXER_DELAY_RETURN;
        case MixerCell::MASTER_EQ:  return HelpTopic::MIXER_MASTER_EQ;
        case MixerCell::MASTER_FX:  return HelpTopic::MIXER_MASTER_FX;
        case MixerCell::LIMITER:    return HelpTopic::MIXER_LIMITER;
        case MixerCell::NONE:       break;
    }
    return HelpTopic::NONE;
}

// EFFECTS, and the send editor, which shows EFFECTS' own cells.
HelpTopic effects_cell_topic(int row) {
    if (row < 0 || row >= EFFECTS_ROW_COUNT) return HelpTopic::NONE;
    switch (static_cast<EffectsRow>(row)) {
        case EffectsRow::MASTER_TYPE: return HelpTopic::FX_MASTER_TYPE;
        case EffectsRow::REV_DECAY:   return HelpTopic::FX_REVERB_DECAY;
        case EffectsRow::REV_SIZE:    return HelpTopic::FX_REVERB_SIZE;
        case EffectsRow::REV_DAMP:    return HelpTopic::FX_REVERB_DAMP;
        case EffectsRow::REV_EQ:      return HelpTopic::FX_REVERB_EQ;
        case EffectsRow::DLY_TIME:    return HelpTopic::FX_DELAY_TIME;
        case EffectsRow::DLY_FDBK:    return HelpTopic::FX_DELAY_FEEDBACK;
        case EffectsRow::DLY_REV:     return HelpTopic::FX_DELAY_TO_REVERB;
        case EffectsRow::DLY_EQ:      return HelpTopic::FX_DELAY_EQ;
        case EffectsRow::DLY_TYPE:    return HelpTopic::FX_DELAY_TYPE;
        case EffectsRow::DLY_TONE:    return HelpTopic::FX_DELAY_TONE;
        case EffectsRow::DLY_WOBBLE:  return HelpTopic::FX_DELAY_WOBBLE;
        case EffectsRow::DLY_PONG:    return HelpTopic::FX_DELAY_PONG;
        case EffectsRow::REV_TYPE:    return HelpTopic::FX_REVERB_TYPE;
        case EffectsRow::REV_PRE:     return HelpTopic::FX_REVERB_PRE;
        case EffectsRow::REV_WIDE:    return HelpTopic::FX_REVERB_WIDE;
        case EffectsRow::REV_MOD:     return HelpTopic::FX_REVERB_MOD;
        case EffectsRow::REV_ALGO:    return HelpTopic::FX_REVERB_ALGO;
        case EffectsRow::REV_SAVE:    return HelpTopic::FX_REVERB_SAVE;
        case EffectsRow::REV_LOAD:    return HelpTopic::FX_REVERB_LOAD;
        case EffectsRow::DLY_SAVE:    return HelpTopic::FX_DELAY_SAVE;
        case EffectsRow::DLY_LOAD:    return HelpTopic::FX_DELAY_LOAD;
    }
    return HelpTopic::NONE;
}

// SETTINGS: column 2 is the row's second cell where it has one (the skin, STR, VOL, POW, TEMPLATE's
// second button).
HelpTopic settings_cell_topic(int row, int column) {
    if (row < 0 || row >= SETTINGS_ROW_COUNT) return HelpTopic::NONE;
    const bool second = (column == 2);
    switch (static_cast<SettingsRow>(row)) {
        case SettingsRow::LAYOUT:     return second ? HelpTopic::SET_SKIN : HelpTopic::SET_LAYOUT;
        case SettingsRow::SCALING:    return HelpTopic::SET_SCALING;
        case SettingsRow::OVERLAY:
            return second ? HelpTopic::SET_OVERLAY_STRENGTH : HelpTopic::SET_OVERLAY;
        case SettingsRow::BTN_SOUND:
            return second ? HelpTopic::SET_BTN_SOUND_VOL : HelpTopic::SET_BTN_SOUND;
        case SettingsRow::BTN_VIBRO:
            return second ? HelpTopic::SET_BTN_VIBRO_POW : HelpTopic::SET_BTN_VIBRO;
        case SettingsRow::ABXY:       return HelpTopic::SET_ABXY;
        case SettingsRow::AUDIO_OUT:  return HelpTopic::SET_AUDIO_OUT;
        case SettingsRow::METRONOME:
            return second ? HelpTopic::SET_METRONOME_VOL : HelpTopic::SET_METRONOME;
        case SettingsRow::KB_INSERT:  return HelpTopic::SET_KB_INSERT;
        case SettingsRow::CURSOR:     return HelpTopic::SET_CURSOR;
        case SettingsRow::NAV:        return HelpTopic::SET_NAV;
        case SettingsRow::ONE_HAND:
            return second ? HelpTopic::SET_ONE_HAND_HOLD : HelpTopic::SET_ONE_HAND;
        case SettingsRow::FOLDER:     return HelpTopic::SET_FOLDER;
        case SettingsRow::NOTE_PREV:  return HelpTopic::SET_NOTE_PREVIEW;
        case SettingsRow::VISUALIZER: return HelpTopic::SET_VISUALIZER;
        case SettingsRow::HELP:       return HelpTopic::SET_HELP;
        case SettingsRow::THEME:      return HelpTopic::SET_THEME;
        case SettingsRow::TEMPLATE:
            return second ? HelpTopic::SET_TEMPLATE_CLEAR : HelpTopic::SET_TEMPLATE_SAVE;
        case SettingsRow::RESUME:     return HelpTopic::SET_RESUME;
        case SettingsRow::TRACE:      return HelpTopic::SET_TRACE;
    }
    return HelpTopic::NONE;
}

// MIDI: one topic per row.
HelpTopic midi_cell_topic(int row) {
    if (row < 0 || row >= MIDI_ROW_COUNT) return HelpTopic::NONE;
    switch (static_cast<MidiRow>(row)) {
        case MidiRow::OUTPUT:   return HelpTopic::MIDI_OUTPUT;
        case MidiRow::INPUT:    return HelpTopic::MIDI_INPUT;
        case MidiRow::OFFSET:   return HelpTopic::MIDI_OFFSET;
        case MidiRow::SYNC:     return HelpTopic::MIDI_SYNC;
        case MidiRow::SYNC_IN:  return HelpTopic::MIDI_SYNC_IN;
        case MidiRow::CTL_CH:   return HelpTopic::MIDI_CTL_CH;
        case MidiRow::KEYS:     return HelpTopic::MIDI_KEYS;
        case MidiRow::PROG_CHG: return HelpTopic::MIDI_PROG_CHG;
        case MidiRow::VELOCITY: return HelpTopic::MIDI_VELOCITY;
        case MidiRow::MAPPING:  return HelpTopic::MIDI_MAPPING;
        case MidiRow::PANIC:    return HelpTopic::MIDI_PANIC;
        case MidiRow::TEST:     return HelpTopic::MIDI_TEST;
    }
    return HelpTopic::NONE;
}

// A mapping row's columns. The ADD row has none; `help_topic` asks about it first.
HelpTopic midi_map_cell_topic(int column) {
    switch (static_cast<MapCol>(column)) {
        case MapCol::CC:    return HelpTopic::MAP_CC;
        case MapCol::MIN:   return HelpTopic::MAP_MIN;
        case MapCol::MAX:   return HelpTopic::MAP_MAX;
        case MapCol::GROUP:
        case MapCol::PARAM: return HelpTopic::MAP_DEST;
        case MapCol::SCOPE: return HelpTopic::MAP_SCOPE;
    }
    return HelpTopic::NONE;
}

// The EQ editor's cursor is one int over 3×4: band = row / 4, parameter = row % 4.
HelpTopic eq_cell_topic(int cursor_row) {
    if (cursor_row < 0) return HelpTopic::NONE;
    switch (cursor_row % 4) {
        case 0:  return HelpTopic::EQ_TYPE;
        case 1:  return HelpTopic::EQ_FREQ;
        case 2:  return HelpTopic::EQ_GAIN;
        default: return HelpTopic::EQ_Q;
    }
}

// The theme editor: THEME (name, SAVE, LOAD), RANDOMIZE (scheme, ROLL), then one row per colour.
HelpTopic theme_cell_topic(int row, int channel) {
    if (row == THEME_ROW_THEME) {
        if (channel == 1) return HelpTopic::THEME_SAVE;
        if (channel == 2) return HelpTopic::THEME_LOAD;
        return HelpTopic::THEME_NAME;
    }
    if (row == THEME_ROW_RANDOM) {
        return (channel == 1) ? HelpTopic::THEME_ROLL : HelpTopic::THEME_SCHEME;
    }
    const int index = theme_color_index(row);
    return (index >= 0) ? theme_color_rows()[static_cast<size_t>(index)].help : HelpTopic::NONE;
}


// The sample editor. SLICE's second cell is SENS or BY by slice method; FILE's CHOP exists only with
// slices.
HelpTopic sample_editor_cell_topic(SampleRow row, int column, int slice_method) {
    switch (row) {
        case SampleRow::VIEW:
            if (column == 0) return HelpTopic::SE_ZOOM;
            if (column == 1) return HelpTopic::SE_SOURCE;
            return (column == 2) ? HelpTopic::SE_RATE : HelpTopic::NONE;
        case SampleRow::EDIT:
            if (column == 0) return HelpTopic::SE_PITCH;
            if (column == 1) return HelpTopic::SE_DURATION;
            return (column == 2) ? HelpTopic::SE_BIT : HelpTopic::NONE;

        case SampleRow::SELECTION:   // the column is which edge
            return (column == 1) ? HelpTopic::SE_SEL_END : HelpTopic::SE_SEL_START;

        case SampleRow::SLICE:
            if (column == 0) return HelpTopic::SE_SLICE_METHOD;
            if (column != 1) return HelpTopic::NONE;
            if (slice_method == SampleEditorModule::SLICE_TRANSIENT) return HelpTopic::SE_SLICE_SENS;
            if (slice_method == SampleEditorModule::SLICE_DIVIDE)    return HelpTopic::SE_SLICE_BY;
            return HelpTopic::NONE;
        case SampleRow::SLICE_AT:
            return (column == 1) ? HelpTopic::SE_SLICE_POS : HelpTopic::SE_SLICE_INDEX;

        case SampleRow::OPS1:
        case SampleRow::OPS2: {
            const SampleOpCell* op = sample_op_at(row, column);
            return op ? op->help : HelpTopic::NONE;
        }

        case SampleRow::FX:
            if (column == 0) return HelpTopic::SE_FX_TYPE;
            if (column == 1) return HelpTopic::SE_FX_VALUE;
            return (column == 2) ? HelpTopic::SE_FX_APPLY : HelpTopic::NONE;

        case SampleRow::NAME: return HelpTopic::SE_NAME;
        case SampleRow::FILE:
            if (column == 0) return HelpTopic::SE_LOAD;
            if (column == 1) return HelpTopic::SE_SAVE;
            if (column == 2) return HelpTopic::SE_OVERWRITE;
            return (column == 3) ? HelpTopic::SE_CHOP : HelpTopic::NONE;
    }
    return HelpTopic::NONE;
}

// The TSTRETCH box. DURATION is the editor's own cell, shown again.
HelpTopic stretch_cell_topic(StretchCell cell) {
    switch (cell) {
        case StretchCell::DURATION: return HelpTopic::SE_DURATION;
        case StretchCell::BPM:      return HelpTopic::SE_STRETCH_BPM;
        case StretchCell::PITCH:    return HelpTopic::SE_STRETCH_PITCH;
        case StretchCell::GRAIN:    return HelpTopic::SE_STRETCH_GRAIN;
        case StretchCell::PREVIEW:  return HelpTopic::SE_STRETCH_PREVIEW;
        case StretchCell::APPLY:    return HelpTopic::SE_STRETCH_APPLY;
    }
    return HelpTopic::SCREEN_SAMPLE_EDITOR;
}


}  // namespace

// The arms read the same cursor fields each module's `cursor_context()` does.
HelpTopic help_topic(const AppState& s) {
    // ⚠️ The overlays first: neither changes `currentScreen`, and the screen under them is not on the
    // canvas. The EQ editor first of those — it can be up over the sample editor.
    if (s.eq.isOpen) {
        const HelpTopic eq = eq_cell_topic(s.eq.cursorRow);
        return (eq == HelpTopic::NONE) ? HelpTopic::SCREEN_EQ : eq;
    }
    if (theme_editor_up(s)) {
        const HelpTopic th = theme_cell_topic(s.themeEditor.cursorRow, s.themeEditor.cursorChannel);
        return (th == HelpTopic::NONE) ? HelpTopic::SCREEN_THEME : th;
    }
    // The send editor's cells are EFFECTS' own. Asked after the EQ editor, which opens over it.
    if (send_editor_up(s)) {
        const HelpTopic fx = effects_cell_topic(s.sendEditor.cursorRow);
        return (fx == HelpTopic::NONE) ? HelpTopic::SCREEN_EFFECTS : fx;
    }

    // The TSTRETCH box covers the sample editor's own cells.
    if (stretch_editor_up(s)) return stretch_cell_topic(s.sampleEditor.stretchCursor);

    HelpTopic cell = HelpTopic::NONE;

    switch (s.currentScreen) {
        case ScreenType::SONG:
            // Every reachable column is a track cell.
            cell = HelpTopic::SONG_CELL;
            break;
        case ScreenType::CHAIN:
            cell = (s.cursorColumn == 1) ? HelpTopic::CHAIN_PHRASE : HelpTopic::CHAIN_TRANSPOSE;
            break;
        case ScreenType::PHRASE:
            cell = phrase_cell_topic(s.cursorColumn);
            break;
        case ScreenType::TABLE:
            cell = table_cell_topic(s.tableCursorColumn);
            break;
        case ScreenType::INSTRUMENT:
            // ⚠️ Guarded: the tools build an AppState with no project.
            if (s.project != nullptr) {
                const songcore::Instrument& ins =
                    s.project->instruments[static_cast<size_t>(s.currentInstrument)];
                cell = instrument_topic(ins.instrumentType, s.instrumentCursorRow,
                                        s.instrumentCursorColumn);
            }
            break;
        case ScreenType::PROJECT:
            cell = project_cell_topic(s.projectCursorRow, s.projectCursorColumn);
            break;
        case ScreenType::GROOVE:
            cell = groove_cell_topic(s.grooveCursorColumn, s.groovePanelRow, s.groovePanelColumn);
            break;
        case ScreenType::SCALE:
            cell = scale_cell_topic(s.scaleCursorRow, s.scaleCursorColumn);
            break;
        case ScreenType::MODS:
            // Guarded like INSTRUMENT.
            if (s.project != nullptr) {
                const songcore::Instrument& ins =
                    s.project->instruments[static_cast<size_t>(s.currentInstrument)];
                // The cursor is (pair, side, row); the slot is which half of which pair.
                const size_t slot = static_cast<size_t>(s.modCursorPair * 2 + s.modCursorSide);
                if (slot < ins.modSlots.size())
                    cell = mod_cell_topic(ins.modSlots[slot].type, s.modCursorRow);
            }
            break;
        case ScreenType::INST_POOL:
            cell = pool_cell_topic(s.poolCursorColumn);
            break;
        case ScreenType::MIXER:
            cell = mixer_cell_topic(s.mixerMasterRow, s.mixerCursorColumn);
            break;
        case ScreenType::EFFECTS:
            cell = effects_cell_topic(s.effectsCursorRow);
            break;
        case ScreenType::SETTINGS:
            cell = settings_cell_topic(s.settingsCursorRow, s.settingsCursorColumn);
            break;
        case ScreenType::SAMPLE_EDITOR:
            cell = sample_editor_cell_topic(s.sampleEditor.cursorRow, s.sampleEditor.cursorCol,
                                            s.sampleEditor.sliceMethod);
            break;
        case ScreenType::MIDI:
            cell = midi_cell_topic(s.midiCursorRow);
            break;

        // The ADD row, one past the end of the list, is a button with its own topic.
        case ScreenType::MIDI_MAP:
            cell = (s.project && s.midiMapCursorRow >=
                                     static_cast<int>(s.project->midiMappings.size()))
                       ? HelpTopic::MAP_ADD
                       : midi_map_cell_topic(s.midiMapCursorColumn);
            break;
        default:
            break;
    }

    return (cell == HelpTopic::NONE) ? help_screen_topic(s.currentScreen) : cell;
}

}  // namespace pt::ui
