#pragma once

// The SETTINGS, PROJECT and MIDI rows: the one table the cursor walks and the module draws. Which rows
// exist depends on the platform (ui/platform_caps.h).
// ⚠️ A row's number is its identity on every platform; the cursor stores it, never a compacted index.
// Hidden rows can be adjacent, so the walks loop over them.

#include "platform_caps.h"

namespace pt::ui {

// ⚠️ Append only: settings.json stores the cursor row by number. Where a row draws is
// SETTINGS_DISPLAY_ORDER's business.
enum class SettingsRow {
    LAYOUT     = 0,    // FULLSCREEN / LANDSCAPE / PORTRAIT (+ a skin column when skinned)
    SCALING    = 1,    // INT / BILINEAR
    OVERLAY    = 2,    // a PNG over the button skin, + STR (debug)
    BTN_SOUND  = 3,    // ON / OFF, + VOL
    BTN_VIBRO  = 4,    // ON / OFF, + POW
    KB_INSERT  = 5,    // BEFORE / AFTER — where a typed character lands
    CURSOR     = 6,    // REMEMBER / REFRESH
    NOTE_PREV  = 7,    // ON / OFF
    VISUALIZER = 8,
    THEME      = 9,    // opens the theme editor
    TEMPLATE   = 10,   // SAVE / CLEAR
    RESUME     = 11,   // ASK / AUTO — what to do with a crash autosave
    TRACE      = 12,   // ON / OFF (debug)
    FOLDER     = 13,   // REMEMBER / REFRESH the last sample folder
    NAV        = 14,   // POOL / SONG — what B+D-pad walks
    ABXY       = 15,   // AUTO / XBOX / NINTENDO — which face button is A; only with a pad
    METRONOME  = 16,   // ON / OFF, + VOL
    HELP       = 17,   // OFF / SHORT / FULL
    AUDIO_OUT  = 18,   // the output device (Windows)
};

inline constexpr int SETTINGS_ROW_COUNT = 19;

// The order rows are drawn and walked; every SettingsRow exactly once.
inline constexpr SettingsRow SETTINGS_DISPLAY_ORDER[SETTINGS_ROW_COUNT] = {
    SettingsRow::LAYOUT,   SettingsRow::SCALING,   SettingsRow::OVERLAY,
    SettingsRow::AUDIO_OUT,
    SettingsRow::BTN_SOUND, SettingsRow::BTN_VIBRO, SettingsRow::ABXY,
    SettingsRow::METRONOME,
    SettingsRow::KB_INSERT, SettingsRow::CURSOR,    SettingsRow::NAV,
    SettingsRow::FOLDER,    SettingsRow::NOTE_PREV,
    SettingsRow::VISUALIZER, SettingsRow::HELP,     SettingsRow::THEME,
    SettingsRow::TEMPLATE,
    SettingsRow::RESUME,    SettingsRow::TRACE,
};

// Where `row` sits in the display order.
inline int settings_display_index(SettingsRow row) {
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i)
        if (SETTINGS_DISPLAY_ORDER[i] == row) return i;
    return 0;
}

// Is this row followed by a group gap (an extra row of air)? It sits on a group's last row.
inline bool settings_row_gap_after(SettingsRow row) {
    switch (row) {
        case SettingsRow::OVERLAY:    // …before BTN SOUND
        case SettingsRow::METRONOME:  // …before KB INSERT
        case SettingsRow::NOTE_PREV:  // …before VISUALIZER
        case SettingsRow::THEME:      // …before TEMPLATE
            return true;
        default:
            return false;
    }
}

// Does this platform have this row at all?
inline bool settings_row_visible(SettingsRow row, const PlatformCaps& caps) {
    switch (row) {
        case SettingsRow::LAYOUT:    return caps.touchLayouts;
        case SettingsRow::OVERLAY:   return caps.skinOverlay && caps.debug;
        case SettingsRow::BTN_SOUND:
        case SettingsRow::BTN_VIBRO: return caps.buttonFeedback;
        case SettingsRow::ABXY:      return caps.padAttached;
        case SettingsRow::AUDIO_OUT: return caps.audioOutputs;
        case SettingsRow::RESUME:    return caps.autosave;
        case SettingsRow::TRACE:     return caps.debug;

        // ⚠️ Never gate NAV: under NAV = SONG a phrase not in the song is reachable only by changing it.
        default: return true;
    }
}

// The next visible row in display order, `delta` ±1, wrapping; `from` if nothing else is visible.
inline int settings_next_visible_row(int from, int delta, const PlatformCaps& caps) {
    int idx = settings_display_index(static_cast<SettingsRow>(from));
    for (int guard = 0; guard < SETTINGS_ROW_COUNT; ++guard) {
        idx += delta;
        if (idx < 0)                    idx = SETTINGS_ROW_COUNT - 1;
        if (idx >= SETTINGS_ROW_COUNT)  idx = 0;
        const SettingsRow row = SETTINGS_DISPLAY_ORDER[idx];
        if (settings_row_visible(row, caps)) return static_cast<int>(row);
    }
    return from;
}

// Where the cursor lands on entry, and the fallback for a stale one.
inline int settings_first_visible_row(const PlatformCaps& caps) {
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i)
        if (settings_row_visible(SETTINGS_DISPLAY_ORDER[i], caps))
            return static_cast<int>(SETTINGS_DISPLAY_ORDER[i]);
    return 0;
}

// Does this row have a second column? LAYOUT's exists only for a skinned layout.
inline bool settings_row_has_second_column(SettingsRow row, const PlatformCaps& caps,
                                           bool layoutHasSkins) {
    switch (row) {
        case SettingsRow::LAYOUT:    return caps.touchLayouts && layoutHasSkins;
        case SettingsRow::OVERLAY:   // STR
        case SettingsRow::BTN_SOUND: // VOL
        case SettingsRow::BTN_VIBRO: // POW
        case SettingsRow::METRONOME: // VOL
        case SettingsRow::TEMPLATE:  // SAVE | CLEAR
            return true;
        default:                     return false;
    }
}

// The height of the first `count` rows in display order. A hidden row still pays its group gap, so
// the groups stay apart whatever is hidden.
inline int settings_rows_height(int count, const PlatformCaps& caps, int rowHeight) {
    int y = 0;
    for (int i = 0; i < count; ++i) {
        const SettingsRow row = SETTINGS_DISPLAY_ORDER[i];
        y += rowHeight * (settings_row_visible(row, caps) ? 1 : 0);
        y += rowHeight * (settings_row_gap_after(row) ? 1 : 0);
    }
    return y;
}

// How far down the panel this row is drawn.
inline int settings_row_offset_y(SettingsRow target, const PlatformCaps& caps, int rowHeight) {
    return settings_rows_height(settings_display_index(target), caps, rowHeight);
}

// The height of all rows on this platform; the panel scrolls when it exceeds the rows area.
inline int settings_content_height(const PlatformCaps& caps, int rowHeight) {
    return settings_rows_height(SETTINGS_ROW_COUNT, caps, rowHeight);
}

// ─── PROJECT ─────────────────────────────────────────────────────────────────────────────────────

// ⚠️ Append only, and a hidden row is skipped rather than renumbered.
enum class ProjectRow {
    TEMPO     = 0,
    TRANSPOSE = 1,
    NAME      = 2,   // 20 characters, one per cursor column
    PROJECT   = 3,   // SAVE | LOAD | NEW
    EXPORT    = 4,   // MIX | STEMS
    COMPACT   = 5,   // SEQ | INST
    SYSTEM    = 6,   // SETTINGS >
    MIDI      = 7,   // MIDI >     — only where the MIDI screens are built
    EXIT      = 8,   // EXIT       — only where a launcher takes the process back
};

inline constexpr int PROJECT_ROW_COUNT = 9;

inline bool project_row_visible(ProjectRow row, const PlatformCaps& caps) {
    switch (row) {
        case ProjectRow::MIDI: return caps.midi;
        case ProjectRow::EXIT: return caps.appExit;
        default:               return true;
    }
}

inline int project_row_count(const PlatformCaps& caps) {
    int n = 0;
    for (int i = 0; i < PROJECT_ROW_COUNT; ++i)
        if (project_row_visible(static_cast<ProjectRow>(i), caps)) ++n;
    return n;
}

// EXIT, MIDI or SYSTEM, depending on the build.
inline ProjectRow project_last_row(const PlatformCaps& caps) {
    ProjectRow last = ProjectRow::TEMPO;
    for (int i = 0; i < PROJECT_ROW_COUNT; ++i) {
        const auto row = static_cast<ProjectRow>(i);
        if (project_row_visible(row, caps)) last = row;
    }
    return last;
}

// The next visible row, as on SETTINGS.
inline int project_next_visible_row(int from, int delta, const PlatformCaps& caps) {
    int idx = from;
    for (int guard = 0; guard < PROJECT_ROW_COUNT; ++guard) {
        idx += delta;
        if (idx < 0)                   idx = PROJECT_ROW_COUNT - 1;
        if (idx >= PROJECT_ROW_COUNT)  idx = 0;
        if (project_row_visible(static_cast<ProjectRow>(idx), caps)) return idx;
    }
    return from;
}

// `row` if this build draws it, else the first visible row.
inline int project_clamp_row(int row, const PlatformCaps& caps) {
    if (row >= 0 && row < PROJECT_ROW_COUNT &&
        project_row_visible(static_cast<ProjectRow>(row), caps))
        return row;
    for (int i = 0; i < PROJECT_ROW_COUNT; ++i)
        if (project_row_visible(static_cast<ProjectRow>(i), caps)) return i;
    return 0;
}

// A project name's length: the NAME row's columns, the keyboard's limit. The row shows fewer and scrolls.
inline constexpr int PROJECT_NAME_MAX_CHARS = 20;

// The highest cursor column on a PROJECT row (column 0 is the label).
inline int project_row_max_column(ProjectRow row) {
    switch (row) {
        case ProjectRow::NAME:    return PROJECT_NAME_MAX_CHARS;  // one column per character
        case ProjectRow::PROJECT: return 3;   // SAVE | LOAD | NEW
        case ProjectRow::EXPORT:  return 2;   // MIX | STEMS
        case ProjectRow::COMPACT: return 2;   // SEQ | INST
        // ⚠️ TAP is a cell of its own: on the value cell every A+UP would count as a tap.
        case ProjectRow::TEMPO:   return 2;   // the value | TAP
        default:                  return 1;
    }
}

// Group gaps: after the values, after the actions, and before EXIT.
inline bool project_row_gap_after(ProjectRow row) {
    return row == ProjectRow::TRANSPOSE || row == ProjectRow::COMPACT || row == ProjectRow::MIDI;
}

// How far down a PROJECT row is drawn. Unlike SETTINGS a hidden row adds nothing, so without MIDI,
// EXIT closes up under SYSTEM.
inline int project_row_offset_y(ProjectRow target, const PlatformCaps& caps, int rowHeight) {
    int y = 0;
    for (int i = 0; i < static_cast<int>(target); ++i) {
        const ProjectRow row = static_cast<ProjectRow>(i);
        if (!project_row_visible(row, caps)) continue;
        y += rowHeight * (project_row_gap_after(row) ? 2 : 1);
    }
    return y;
}

// ─── MIDI: one column, the last rows buttons; every row on every platform ────────────────────────
enum class MidiRow {
    OUTPUT   = 0,   // <device name> | AUTO | OFF — the cable
    INPUT    = 1,   // <device name> | AUTO | OFF — the cable
    OFFSET   = 2,   // -99..+99 MS           — the cable
    SYNC     = 3,   // SYNC OUT: ON | OFF     — the cable (24 PPQN clock + transport)
    SYNC_IN  = 4,   // OFF | TEMPO | FULL    — the cable: follow another device's clock
    CTL_CH   = 5,   // ALL | 01..16          — the cable: which channel carries MAPPING knobs
    PROG_CHG = 6,   // ON | OFF              — the project (Instrument BANK/PROG on note-on)
    KEYS     = 7,   // MONO | POLY 2..8      — how a live key plays (settings.json)
    VELOCITY = 8,   // ON | OFF              — OFF plays every live key at full strength (settings.json)
    MAPPING  = 9,   // A: the mapping list   — the project
    PANIC    = 10,  // A: ALL NOTES OFF
    TEST     = 11,  // A: C-4 CH 1
};

// Unlike PROJECT's, these rows may be inserted: nothing stores a MIDI row index.
constexpr int MIDI_ROW_COUNT = 12;

// A gap before the keyboard's rows, and another before the actions.
inline bool midi_row_gap_after(MidiRow row) { return row == MidiRow::PROG_CHG || row == MidiRow::VELOCITY; }

inline int midi_row_offset_y(MidiRow target, int rowHeight) {
    int y = 0;
    for (int i = 0; i < static_cast<int>(target); ++i)
        y += rowHeight * (midi_row_gap_after(static_cast<MidiRow>(i)) ? 2 : 1);
    return y;
}

}  // namespace pt::ui
