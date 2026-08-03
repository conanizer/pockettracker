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
 * 13 FOLDER      REMEMBER / REFRESH                 (v0.9.4 D2a — remember the last sample folder)
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
    // ⚠️ APPENDED — a row's VALUE is its stable identity (the settings.json cursor, every
    // `SettingsRow::X` in ptdispatch, the numeric-row tests), so FOLDER keeps the next free ordinal 13
    // and is never renumbered. Its VALUE and its POSITION are now two different things: it DRAWS and
    // NAVIGATES between CURSOR and NOTE PREV, with the other app toggles (user's call, v0.9.4 D2a) — see
    // SETTINGS_DISPLAY_ORDER below, which is the order the panel walks, decoupled from these values.
    FOLDER     = 13,
};

inline constexpr int SETTINGS_ROW_COUNT = 14;

// ─── Display / navigation order ──────────────────────────────────────────────────────────────────
//
// The order the rows are DRAWN down the panel and the D-pad walks — DECOUPLED from the enum VALUE above,
// which stays each row's identity. The only divergence from ascending value is FOLDER (value 13): it
// lives here between CURSOR and NOTE PREV so a session toggle sits with the other app toggles, while its
// identity stays 13 everywhere a row is named or persisted. `offset_y`, `next_visible_row` and
// `first_visible_row` all walk THIS array; the cursor still stores the row's VALUE, not its position.
//
// ⚠️ Every SettingsRow appears exactly once and the length is SETTINGS_ROW_COUNT — the walkers assume it.
inline constexpr SettingsRow SETTINGS_DISPLAY_ORDER[SETTINGS_ROW_COUNT] = {
    SettingsRow::LAYOUT,   SettingsRow::SCALING,   SettingsRow::OVERLAY,
    SettingsRow::BTN_SOUND, SettingsRow::BTN_VIBRO,
    SettingsRow::KB_INSERT, SettingsRow::CURSOR,    SettingsRow::FOLDER, SettingsRow::NOTE_PREV,
    SettingsRow::VISUALIZER, SettingsRow::THEME,    SettingsRow::TEMPLATE,
    SettingsRow::RESUME,    SettingsRow::TRACE,
};

/** Where `row` sits in the display/navigation order (0-based). */
inline int settings_display_index(SettingsRow row) {
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i)
        if (SETTINGS_DISPLAY_ORDER[i] == row) return i;
    return 0;
}

/** Is this row followed by a group gap (an extra ROW_HEIGHT of air)? Kotlin's `rowY += ROW_HEIGHT * 2`. */
inline bool settings_row_gap_after(SettingsRow row) {
    switch (row) {
        case SettingsRow::OVERLAY:    // …before BTN SOUND
        case SettingsRow::BTN_VIBRO:  // …before KB INSERT
        case SettingsRow::NOTE_PREV:  // …before VISUALIZER
        case SettingsRow::THEME:      // …before TEMPLATE
            // FOLDER (D2a) sits mid-group between CURSOR and NOTE PREV (SETTINGS_DISPLAY_ORDER), so it
            // takes NO gap of its own — it joins the KB INSERT / CURSOR / NOTE PREV app-toggle group.
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
 * The next VISIBLE row's VALUE in direction `delta` (+1 = down, −1 = up), wrapping — the loop Kotlin's
 * one-level substitution stands in for. Walks SETTINGS_DISPLAY_ORDER (so FOLDER is reached between CURSOR
 * and NOTE PREV, not after RESUME), and returns the landed row's VALUE — which is what the cursor stores.
 * Returns `from` unchanged if nothing else is visible.
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

/** The first visible row's VALUE (top of the display order) — where the cursor lands on entry, and the
 *  fallback for a stale one. */
inline int settings_first_visible_row(const PlatformCaps& caps) {
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i)
        if (settings_row_visible(SETTINGS_DISPLAY_ORDER[i], caps))
            return static_cast<int>(SETTINGS_DISPLAY_ORDER[i]);
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
    const int targetIdx = settings_display_index(target);
    int       y         = 0;
    for (int i = 0; i < targetIdx; ++i) {   // walk the DISPLAY order, not the ascending value
        const SettingsRow row     = SETTINGS_DISPLAY_ORDER[i];
        const bool        visible = settings_row_visible(row, caps);
        const bool        gap     = settings_row_gap_after(row);
        if (visible) y += rowHeight * (gap ? 2 : 1);
        else         y += rowHeight * (gap ? 1 : 0);
    }
    return y;
}

/**
 * The total height of all rows on this platform, in pixels — the sum every display row contributes to
 * `offset_y` (a visible row its height + any gap; a hidden one only its gap). The settings panel SCROLLS
 * when this exceeds the visible rows area (v0.9.4 D2a), so the last rows (RESUME/TRACE in the dense debug
 * caps, where the 14 rows overflow the 392px panel) stay reachable — the module clamps the scroll to
 * `content_height − viewport`. When it fits, the clamp is zero and nothing scrolls (every release build).
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
    MIDI      = 7,   // MIDI >     (plan §8.1)
    EXIT      = 8,   // the shell only — Android apps never exit
};

// ⚠️ MIDI IS APPENDED AFTER SYSTEM, NOT INSERTED BEFORE IT, AND THAT IS THE WHOLE REASON `p3-input`
// SURVIVED. The plan called this the "PROJECT-row landmine": the golden carries 4410 PROJECT cases and
// renumbering a row it records rewrites all of them. It records rows 0–6 ONLY — EXIT is never in it,
// because the corpus was recorded from Android and Android has no EXIT row. So SYSTEM keeping the
// number 6 is what makes this a pure append: every recorded case still names the row it was recorded
// against, and the only row that renumbers is the one nothing has ever tested.
//
// It also happens to be the placement §8.1 asked for ("PROJECT row 7") and the one that reads right:
// SYSTEM and MIDI are both doors to a sub-screen, so they sit together, above the way out.
inline int project_row_count(const PlatformCaps& caps) { return caps.appExit ? 9 : 8; }

/** The last PROJECT row on this platform — MIDI, or EXIT where there is one. */
inline ProjectRow project_last_row(const PlatformCaps& caps) {
    return caps.appExit ? ProjectRow::EXIT : ProjectRow::MIDI;
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

// ─── MIDI (plan §8.1, phase B4.3) ────────────────────────────────────────────────────────────────
//
// The same shape as PROJECT: a short single-column form whose last rows are BUTTONS. Every row is on
// every platform — a MIDI cable is not a device capability the way a touchscreen is, and a phone with
// no port simply enumerates none, which OUTPUT already has a word for.
//
// ⚠️ **SYNC OUT WAS DELIBERATELY ABSENT UNTIL PHASE C, AND `INPUT` / `IN CH` UNTIL PHASE E3**, though
// `Project::midiSyncOut` and `Project::midiInputChannels` have round-tripped since B1. A row that
// stores a choice nobody reads is the exact trap the guardrails name: a setting that round-trips is not
// a setting that is applied. Each got its row in the increment that got its reader. `SYNC IN` is still
// absent for that reason (§9 defers it) — the two-column sketch in §8.1 is one column here because the
// rows its right-hand column held are these, arriving one phase at a time.
enum class MidiRow {
    OUTPUT   = 0,   // <device name> | OFF   — the cable
    INPUT    = 1,   // <device name> | OFF   — the cable      (phase E3)
    OFFSET   = 2,   // -99..+99 MS           — the cable
    SYNC     = 3,   // ON | OFF              — the cable (phase C: 24 PPQN clock + transport)
    PROG_CHG = 4,   // ON | OFF              — the project (Instrument BANK/PROG on note-on)
    IN_MAP   = 5,   // 8 cells, -- | 01..16  — the project (per-track input channel, phase E3)
    PANIC    = 6,   // A: ALL NOTES OFF
    TEST     = 7,   // A: C-4 CH 1
};

// ⚠️ ROWS ARE INSERTED HERE, NOT APPENDED, and unlike B4.3's PROJECT row that is safe: nothing in the
// tree stores a MIDI row index. `settings.json` and the `.ptp` both key rows by NAME, `ptdispatch`
// drives them by enumerator, `p3-input.txt` has no MIDI line at all (the screen has no Kotlin twin),
// and the cursor is clamped to `MIDI_ROW_COUNT` on every move. What decided each position is the
// grouping: OUTPUT/INPUT/OFFSET/SYNC describe THIS MACHINE'S CABLE and live in settings.json, PROG CHG
// and IN CH describe THE SONG and travel in the .ptp, and the last two are actions. INPUT sits beside
// OUTPUT rather than after SYNC because the question a user arrives with is "which cables am I on".
constexpr int MIDI_ROW_COUNT = 8;

/** The IN CH row is eight cells wide — one per track — and they are cursor COLUMNS 1..8. */
constexpr int MIDI_IN_MAP_COLUMNS = 8;

/**
 * Group gaps: after PROG CHG (the single-value rows end and the track map begins — the blank row is
 * also where the map's `1 2 3 4 5 6 7 8` header is drawn) and after IN CH (the values end, the two
 * actions begin).
 */
inline bool midi_row_gap_after(MidiRow row) {
    return row == MidiRow::PROG_CHG || row == MidiRow::IN_MAP;
}

/** How far down the panel a MIDI row is drawn, in pixels from the first row's top. */
inline int midi_row_offset_y(MidiRow target, int rowHeight) {
    int y = 0;
    for (int i = 0; i < static_cast<int>(target); ++i)
        y += rowHeight * (midi_row_gap_after(static_cast<MidiRow>(i)) ? 2 : 1);
    return y;
}

}  // namespace pt::ui
