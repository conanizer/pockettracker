#pragma once

// ─── The SETTINGS screen's row geometry ──────────────────────────────────────────────────────────
//
// The ONE table the cursor walks and the module draws. WHICH ROWS EXIST depends on the platform
// (ui/platform_caps.h), so nothing may re-derive it.
// ⚠️ A ROW'S NUMBER IS ITS IDENTITY on every platform: `SettingsRow::KB_INSERT` is 5 whether or not rows
// 0 and 2–4 exist here, and the cursor stores THAT, never a compacted index — the tests address rows by
// number, and every platform agrees what a setting IS.
// ⚠️ Hidden rows can be ADJACENT (OVERLAY, BTN SOUND and BTN VIBRO vanish together on the shell), so the
// walk below LOOPS over hidden rows rather than hopping one.

#include "platform_caps.h"

namespace pt::ui {

/**
 * The rows, by identity.
 *
 * 0  LAYOUT      FULLSCREEN / LANDSCAPE / PORTRAIT  (+ a skin column when the layout is skinned)
 * 1  SCALING     INT / BILINEAR                     (the display's texture filter)
 * 2  OVERLAY     a PNG over the button skin, + STR  (debug)
 * 3  BTN SOUND   ON / OFF, + VOL
 * 4  BTN VIBRO   ON / OFF, + POW
 * 5  KB INSERT   BEFORE / AFTER                     (where a typed character lands)
 * 6  CURSOR      REMEMBER / REFRESH
 * 7  NOTE PREV   ON / OFF
 * 8  VISUALIZER  SCOPE / FLAT / OCTA / OCTA.F / SPECT / SPCT.P
 * 9  THEME       opens the theme editor
 * 10 TEMPLATE    SAVE / CLEAR
 * 11 RESUME      ASK / AUTO                          (what to do with a crash autosave)
 * 12 TRACE       ON / OFF                           (debug)
 * 13 FOLDER      REMEMBER / REFRESH                 (remember the last sample folder)
 * 14 NAV         POOL / SONG                        (what B+D-pad walks: the 00..FF pool, or the song)
 * 15 ABXY        AUTO / XBOX / NINTENDO             (which face button is PRINTED A; only with a pad)
 * 16 METRONOME   ON / OFF, + VOL                    (a click on every quarter note while playing)
 * 17 HELP        OFF / SHORT / FULL                 (what SELECT shows: nothing, the panel, the overlay)
 * 18 AUDIO OUT   the output device                  (Windows)
 */
enum class SettingsRow {
    LAYOUT     = 0,
    SCALING    = 1,
    OVERLAY    = 2,
    BTN_SOUND  = 3,
    BTN_VIBRO  = 4,
    KB_INSERT  = 5,
    CURSOR     = 6,
    NOTE_PREV  = 7,
    VISUALIZER = 8,
    THEME      = 9,
    TEMPLATE   = 10,
    RESUME     = 11,
    TRACE      = 12,
    // ⚠️ From here on APPENDED: a row's VALUE is its identity (the settings.json cursor, every
    // `SettingsRow::X` in the tools), so new rows take the next free ordinal. Where they DRAW is
    // SETTINGS_DISPLAY_ORDER's business, not this enum's.
    FOLDER     = 13,
    // ⚠️ settings.json stores the cursor row by NUMBER — an insert would move every row a file names.
    NAV        = 14,
    ABXY       = 15,
    METRONOME  = 16,
    HELP       = 17,
    // Windows only.
    AUDIO_OUT  = 18,
};

inline constexpr int SETTINGS_ROW_COUNT = 19;

// ─── Display / navigation order ──────────────────────────────────────────────────────────────────
//
// The order rows are DRAWN and the D-pad walks, independent of the enum VALUE (the identity). The
// walkers (`offset_y`, `next_visible_row`, `first_visible_row`) all use THIS array; the cursor still
// stores the VALUE.
// ⚠️ Every SettingsRow appears exactly once and the length is SETTINGS_ROW_COUNT.
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

/** Where `row` sits in the display/navigation order (0-based). */
inline int settings_display_index(SettingsRow row) {
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i)
        if (SETTINGS_DISPLAY_ORDER[i] == row) return i;
    return 0;
}

/** Is this row followed by a group gap (an extra ROW_HEIGHT of air)? */
inline bool settings_row_gap_after(SettingsRow row) {
    switch (row) {
        case SettingsRow::OVERLAY:    // …before BTN SOUND
        // The gap sits on the cluster's LAST row. A hidden row still contributes its gap
        // (settings_row_offset_y), so the air before KB INSERT is one row whatever is hidden.
        case SettingsRow::METRONOME:  // …before KB INSERT
        case SettingsRow::NOTE_PREV:  // …before VISUALIZER
        case SettingsRow::THEME:      // …before TEMPLATE
            // FOLDER and NAV sit mid-group (between CURSOR and NOTE PREV) and take no gap of their own.
            return true;
        default:
            return false;
    }
}

/** Does this platform have this row at all? */
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

        // The rest are about the app, not the device; every platform has them (METRONOME's click is the
        // engine's, so there is nothing to gate it on).
        // ⚠️ NAV must NEVER be gated: under NAV = SONG (the default) a phrase not placed in the song is
        // unreachable, and this row is the only way out.
        default: return true;
    }
}

/**
 * The next VISIBLE row's VALUE in direction `delta` (+1 down, −1 up), wrapping, walking
 * SETTINGS_DISPLAY_ORDER. Returns `from` unchanged if nothing else is visible.
 */
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

/** The first visible row's VALUE — where the cursor lands on entry, and the fallback for a stale one. */
inline int settings_first_visible_row(const PlatformCaps& caps) {
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i)
        if (settings_row_visible(SETTINGS_DISPLAY_ORDER[i], caps))
            return static_cast<int>(SETTINGS_DISPLAY_ORDER[i]);
    return 0;
}

/**
 * Does this row have a SECOND column?
 * ⚠️ LAYOUT's is dynamic: the skin column exists only for a skinned layout (`layoutHasSkins`).
 */
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

/**
 * How far down the panel this row is drawn, in pixels from the first row's top.
 * ⚠️ A HIDDEN ROW STILL PAYS ITS GROUP GAP (not its height), so dropping BTN SOUND and BTN VIBRO still
 * leaves the air before KB INSERT and the groups stay legible.
 */
inline int settings_row_offset_y(SettingsRow target, const PlatformCaps& caps, int rowHeight) {
    const int targetIdx = settings_display_index(target);
    int       y         = 0;
    for (int i = 0; i < targetIdx; ++i) {   // walk the DISPLAY order
        const SettingsRow row     = SETTINGS_DISPLAY_ORDER[i];
        const bool        visible = settings_row_visible(row, caps);
        const bool        gap     = settings_row_gap_after(row);
        if (visible) y += rowHeight * (gap ? 2 : 1);
        else         y += rowHeight * (gap ? 1 : 0);
    }
    return y;
}

/**
 * The total height of all rows on this platform — what every row contributes to `offset_y` (visible:
 * height + gap; hidden: gap). The panel SCROLLS when this exceeds the rows area, clamped to
 * `content_height − viewport`; when it fits, nothing scrolls.
 */
inline int settings_content_height(const PlatformCaps& caps, int rowHeight) {
    int y = 0;
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i) {
        const SettingsRow row     = SETTINGS_DISPLAY_ORDER[i];
        const bool        visible = settings_row_visible(row, caps);
        const bool        gap     = settings_row_gap_after(row);
        if (visible) y += rowHeight * (gap ? 2 : 1);
        else         y += rowHeight * (gap ? 1 : 0);
    }
    return y;
}

// ─── PROJECT ─────────────────────────────────────────────────────────────────────────────────────
//
// Rows 0–6 exist everywhere; the last two are conditional (`project_row_visible`).

enum class ProjectRow {
    TEMPO     = 0,
    TRANSPOSE = 1,
    NAME      = 2,   // 20 characters, one per cursor column
    PROJECT   = 3,   // SAVE | LOAD | NEW
    EXPORT    = 4,   // MIX | STEMS
    COMPACT   = 5,   // SEQ | INST
    SYSTEM    = 6,   // SETTINGS >
    MIDI      = 7,   // MIDI >     — only where the MIDI surfaces are authorable
    EXIT      = 8,   // EXIT       — only where the process can be handed back to a launcher
};

// ⚠️ A ROW'S NUMBER IS ITS IDENTITY: the recorded PROJECT input cases, the cursor and settings.json all
// use it, so rows are APPENDED, never inserted, and a hidden row is skipped rather than renumbered.
inline constexpr int PROJECT_ROW_COUNT = 9;

/**
 * Does this build have this row? EXIT where the process can be handed back to a launcher; MIDI where
 * the MIDI surfaces are authorable (ui/platform_caps.h). ⚠️ Hiding MIDI does NOT renumber EXIT to 7.
 */
inline bool project_row_visible(ProjectRow row, const PlatformCaps& caps) {
    switch (row) {
        case ProjectRow::MIDI: return caps.midi;
        case ProjectRow::EXIT: return caps.appExit;
        default:               return true;
    }
}

/** How many PROJECT rows this build draws. */
inline int project_row_count(const PlatformCaps& caps) {
    int n = 0;
    for (int i = 0; i < PROJECT_ROW_COUNT; ++i)
        if (project_row_visible(static_cast<ProjectRow>(i), caps)) ++n;
    return n;
}

/** The last visible PROJECT row's VALUE — EXIT, or MIDI, or SYSTEM, depending on the build. */
inline ProjectRow project_last_row(const PlatformCaps& caps) {
    ProjectRow last = ProjectRow::TEMPO;
    for (int i = 0; i < PROJECT_ROW_COUNT; ++i) {
        const auto row = static_cast<ProjectRow>(i);
        if (project_row_visible(row, caps)) last = row;
    }
    return last;
}

/** The next VISIBLE row's VALUE in direction `delta`, wrapping — as SETTINGS, so a hidden row
 *  mid-map is never landed on. Returns `from` if nothing else is visible. */
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

/** `row` if this build draws it, else the first visible row — the landing spot for a stale cursor. */
inline int project_clamp_row(int row, const PlatformCaps& caps) {
    if (row >= 0 && row < PROJECT_ROW_COUNT &&
        project_row_visible(static_cast<ProjectRow>(row), caps))
        return row;
    for (int i = 0; i < PROJECT_ROW_COUNT; ++i)
        if (project_row_visible(static_cast<ProjectRow>(i), caps)) return i;
    return 0;
}

/**
 * How many characters a project NAME holds — the NAME row's column count, the QWERTY limit and the
 * module's cell count, one number. More than fit on the row: the module shows
 * `ProjectModule::NAME_VISIBLE_CHARS` and scrolls (`layout.h` pins that against the clip).
 */
inline constexpr int PROJECT_NAME_MAX_CHARS = 20;

/** The highest cursor column on a PROJECT row. Column 0 is the row's LABEL and never reachable. */
inline int project_row_max_column(ProjectRow row) {
    switch (row) {
        case ProjectRow::NAME:    return PROJECT_NAME_MAX_CHARS;  // one column per character
        case ProjectRow::PROJECT: return 3;   // SAVE | LOAD | NEW
        case ProjectRow::EXPORT:  return 2;   // MIX | STEMS
        case ProjectRow::COMPACT: return 2;   // SEQ | INST
        // TEMPO's second column is TAP, a BUTTON. ⚠️ It must be a cell of its own: A is the modifier of
        // A+DPAD, so a tap read off the value cell would count every A+UP that nudges the BPM.
        case ProjectRow::TEMPO:   return 2;   // the value | TAP
        default:                  return 1;
    }
}

/** Group gaps: after TRANSPOSE (values end), after COMPACT (actions end), after MIDI (the way OUT
 *  stands apart from the two doors above it). */
inline bool project_row_gap_after(ProjectRow row) {
    return row == ProjectRow::TRANSPOSE || row == ProjectRow::COMPACT || row == ProjectRow::MIDI;
}

/**
 * How far down the panel a PROJECT row is drawn. Unlike SETTINGS, a hidden row contributes NOTHING —
 * the gap exists to separate MIDI from EXIT, so without MIDI, EXIT closes up under SYSTEM.
 */
inline int project_row_offset_y(ProjectRow target, const PlatformCaps& caps, int rowHeight) {
    int y = 0;
    for (int i = 0; i < static_cast<int>(target); ++i) {
        const ProjectRow row = static_cast<ProjectRow>(i);
        if (!project_row_visible(row, caps)) continue;
        y += rowHeight * (project_row_gap_after(row) ? 2 : 1);
    }
    return y;
}

// ─── MIDI ────────────────────────────────────────────────────────────────────────────────────────
//
// A short single-column form whose last rows are BUTTONS. Every row on every platform — a device with
// no port simply lists none. No SYNC IN row: nothing reads it, and a stored choice nobody reads is a
// lie.
enum class MidiRow {
    OUTPUT   = 0,   // <device name> | AUTO | OFF — the cable
    INPUT    = 1,   // <device name> | AUTO | OFF — the cable
    OFFSET   = 2,   // -99..+99 MS           — the cable
    SYNC     = 3,   // ON | OFF              — the cable (24 PPQN clock + transport)
    CTL_CH   = 4,   // ALL | 01..16          — the cable: which channel carries MAPPING knobs
    PROG_CHG = 5,   // ON | OFF              — the project (Instrument BANK/PROG on note-on)
    KEYS     = 6,   // MONO | POLY 2..8      — how a live key plays (settings.json)
    VELOCITY = 7,   // ON | OFF              — OFF plays every live key at full strength (settings.json)
    MAPPING  = 8,   // A: the mapping list   — the project
    PANIC    = 9,   // A: ALL NOTES OFF
    TEST     = 10,  // A: C-4 CH 1
};

// ROWS MAY BE INSERTED here, unlike PROJECT's: nothing stores a MIDI row index (settings.json and the
// .ptp key by NAME, the tools by enumerator, the cursor is clamped). The order is the grouping: the
// cable, how a keyboard plays, then actions.
constexpr int MIDI_ROW_COUNT = 11;

/** A blank row before the keyboard's rows, and another before the actions. */
inline bool midi_row_gap_after(MidiRow row) { return row == MidiRow::PROG_CHG || row == MidiRow::VELOCITY; }

/** How far down the panel a MIDI row is drawn, in pixels from the first row's top. */
inline int midi_row_offset_y(MidiRow target, int rowHeight) {
    int y = 0;
    for (int i = 0; i < static_cast<int>(target); ++i)
        y += rowHeight * (midi_row_gap_after(static_cast<MidiRow>(i)) ? 2 : 1);
    return y;
}

}  // namespace pt::ui
