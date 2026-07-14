#pragma once

// ─── The SETTINGS screen's row geometry ──────────────────────────────────────────────────────────
//
// The ONE table the cursor walks and the module draws, in the shape S4 gave the INSTRUMENT screen
// (ui/instrument_row_layout.h) and for a sharper reason: here, WHICH ROWS EXIST is a function of the
// platform (ui/platform_caps.h), so nothing may re-derive the answer for itself.
//
// ⚠️ A ROW'S NUMBER IS ITS IDENTITY, ON BOTH PLATFORMS. `SettingsRow::KB_INSERT` is 5 whether or not
// rows 0 and 2–4 exist on this device, and the cursor stores THAT — not a compacted "third visible
// row" index. Two things fall out, and both are the point:
//
//   • The Kotlin golden speaks in row numbers, so ptinput can drive the C++ module at row 5 and
//     compare it against Kotlin's row 5 directly. A compacted index would have made every ported
//     row's identity depend on the caps it was recorded under.
//   • The SDL shell and the Android app agree about what a setting IS, which is what makes a shared
//     settings file (and, one day, a shared screen) possible at all.
//
// ⚠️ AND KOTLIN'S ROW SKIP CANNOT BE PORTED LITERALLY. TrackerController hides its two debug rows
// with a single substitution:
//
//     var prev = if (settingsCursorRow > 0) settingsCursorRow - 1 else 12
//     if (!BuildConfig.DEBUG && prev == 12) prev = 11   // TRACE/ENG hidden in release
//     if (!BuildConfig.DEBUG && prev == 2)  prev = 1    // OVERLAY hidden in release
//
// That is a ONE-LEVEL hop, and it is correct on Android only because no two hidden rows are ever
// adjacent there. On the shell, rows 2, 3 and 4 (OVERLAY, BTN SOUND, BTN VIBRO) all vanish TOGETHER
// — so a single substitution off row 1 lands on row 3, which is not there either, and the cursor
// disappears onto a row that is never drawn. The walk below LOOPS, which is the general form of what
// Kotlin was approximating.

#include "platform_caps.h"

namespace pt::ui {

/**
 * The rows, by Kotlin's index — the canonical name of each setting.
 *
 * 0  LAYOUT      FULLSCREEN / LANDSCAPE / PORTRAIT  (+ a skin column when the layout is skinned)
 * 1  SCALING     INT / BILINEAR                     (the shell's texture filter; Android's surface)
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
 * 12 TRACE       ON / OFF, + ENG: KT / C++          (debug)
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
};

inline constexpr int SETTINGS_ROW_COUNT = 13;

/** Is this row followed by a group gap (an extra ROW_HEIGHT of air)? Kotlin's `rowY += ROW_HEIGHT * 2`. */
inline bool settings_row_gap_after(SettingsRow row) {
    switch (row) {
        case SettingsRow::OVERLAY:    // …before BTN SOUND
        case SettingsRow::BTN_VIBRO:  // …before KB INSERT
        case SettingsRow::NOTE_PREV:  // …before VISUALIZER
        case SettingsRow::THEME:      // …before TEMPLATE
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
        case SettingsRow::RESUME:    return caps.autosave;
        case SettingsRow::TRACE:     return caps.debug;

        // SCALING, KB INSERT, CURSOR, NOTE PREV, VISUALIZER, THEME and TEMPLATE are about the app,
        // not the device. Every platform has them.
        default: return true;
    }
}

/**
 * The next VISIBLE row in direction `delta` (+1 = down, −1 = up), wrapping — the loop Kotlin's
 * one-level substitution stands in for. Returns `from` unchanged if nothing else is visible.
 */
inline int settings_next_visible_row(int from, int delta, const PlatformCaps& caps) {
    int row = from;
    for (int guard = 0; guard < SETTINGS_ROW_COUNT; ++guard) {
        row += delta;
        if (row < 0)                    row = SETTINGS_ROW_COUNT - 1;
        if (row >= SETTINGS_ROW_COUNT)  row = 0;
        if (settings_row_visible(static_cast<SettingsRow>(row), caps)) return row;
    }
    return from;
}

/** The first visible row — where the cursor lands on entry, and the fallback for a stale one. */
inline int settings_first_visible_row(const PlatformCaps& caps) {
    for (int row = 0; row < SETTINGS_ROW_COUNT; ++row)
        if (settings_row_visible(static_cast<SettingsRow>(row), caps)) return row;
    return 0;
}

/**
 * Does this row have a SECOND column?
 *
 * ⚠️ TRACE's answer is caps-dependent, and it is the one row where the shell's column COUNT differs
 * rather than just its presence: column 2 is ENG (KT vs C++), and with no Kotlin sequencer in the
 * process there is nothing there to point at. RIGHT on TRACE must therefore not move on the shell,
 * while it must on Android.
 *
 * LAYOUT's is dynamic on Android too — the skin column exists only while the layout is a skinned one
 * (`SettingsModule.skinsForLayout`), which is what `layoutHasSkins` carries in.
 */
inline bool settings_row_has_second_column(SettingsRow row, const PlatformCaps& caps,
                                           bool layoutHasSkins) {
    switch (row) {
        case SettingsRow::LAYOUT:    return caps.touchLayouts && layoutHasSkins;
        case SettingsRow::OVERLAY:   // STR
        case SettingsRow::BTN_SOUND: // VOL
        case SettingsRow::BTN_VIBRO: // POW
        case SettingsRow::TEMPLATE:  // SAVE | CLEAR
            return true;
        case SettingsRow::TRACE:     return caps.engineToggle;  // ENG
        default:                     return false;
    }
}

/**
 * How far down the panel this row is drawn, in pixels from the first row's top.
 *
 * ⚠️ A HIDDEN ROW STILL PAYS ITS GROUP GAP. That looks like a quirk and is in fact Kotlin's release
 * behaviour, reproduced by one rule instead of a special case: SettingsModule's OVERLAY branch is
 *
 *     if (BuildConfig.DEBUG) { …draw…; rowY += ROW_HEIGHT * 2 }
 *     else                   { rowY += ROW_HEIGHT }   // "keep the group gap OVERLAY's spacer used to provide"
 *
 * i.e. a hidden row contributes its GAP but not its HEIGHT. Applied uniformly that also gives the
 * shell a sane layout for free — dropping BTN SOUND and BTN VIBRO still leaves the air before KB
 * INSERT, so the groups stay legible instead of collapsing into one slab.
 */
inline int settings_row_offset_y(SettingsRow target, const PlatformCaps& caps, int rowHeight) {
    int y = 0;
    for (int i = 0; i < static_cast<int>(target); ++i) {
        const SettingsRow row = static_cast<SettingsRow>(i);
        const bool visible = settings_row_visible(row, caps);
        const bool gap     = settings_row_gap_after(row);
        if (visible) y += rowHeight * (gap ? 2 : 1);
        else         y += rowHeight * (gap ? 1 : 0);
    }
    return y;
}

// ─── PROJECT ─────────────────────────────────────────────────────────────────────────────────────
//
// Its row map is Kotlin's, unfiltered — every row edits the PROJECT or acts on it, and a project is
// the same thing on every platform. The shell adds exactly one row on the end.

enum class ProjectRow {
    TEMPO     = 0,
    TRANSPOSE = 1,
    NAME      = 2,   // 20 characters, one per cursor column
    PROJECT   = 3,   // SAVE | LOAD | NEW
    EXPORT    = 4,   // MIX | STEMS
    COMPACT   = 5,   // SEQ | INST
    SYSTEM    = 6,   // SETTINGS >
    EXIT      = 7,   // the shell only — Android apps never exit
};

inline int project_row_count(const PlatformCaps& caps) { return caps.appExit ? 8 : 7; }

/** The last PROJECT row on this platform — SYSTEM, or EXIT where there is one. */
inline ProjectRow project_last_row(const PlatformCaps& caps) {
    return caps.appExit ? ProjectRow::EXIT : ProjectRow::SYSTEM;
}

/**
 * The highest cursor column on a PROJECT row. Column 0 is the row's LABEL and is never reachable —
 * `getProjectCursorLeftColumn` coerces to at least 1, and the cursor starts at 1 (which is why
 * ProjectModule's `cursorColumn == 0 -> readOnly()` arms are, on inspection, dead).
 */
inline int project_row_max_column(ProjectRow row) {
    switch (row) {
        case ProjectRow::NAME:    return 20;  // one column per character
        case ProjectRow::PROJECT: return 3;   // SAVE | LOAD | NEW
        case ProjectRow::EXPORT:  return 2;   // MIX | STEMS
        case ProjectRow::COMPACT: return 2;   // SEQ | INST
        default:                  return 1;
    }
}

/** Group gaps: after TRANSPOSE (the values end) and after COMPACT (the actions end). Kotlin's. */
inline bool project_row_gap_after(ProjectRow row) {
    return row == ProjectRow::TRANSPOSE || row == ProjectRow::COMPACT;
}

/** How far down the panel a PROJECT row is drawn, in pixels from the first row's top. */
inline int project_row_offset_y(ProjectRow target, int rowHeight) {
    int y = 0;
    for (int i = 0; i < static_cast<int>(target); ++i) {
        const ProjectRow row = static_cast<ProjectRow>(i);
        y += rowHeight * (project_row_gap_after(row) ? 2 : 1);
    }
    return y;
}

}  // namespace pt::ui
