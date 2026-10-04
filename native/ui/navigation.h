#pragma once

// ─── Screen navigation ───────────────────────────────────────────────────────────────────────────
//
// The 5×5 screen grid R+DPAD moves around — the same grid `modules/navigation_map.h` draws. A cell you
// can see but not reach, or a landing the map does not show, is the bug to watch for.
//
//        col 0     col 1     col 2     col 3       col 4
//  row 0                     SCALE     INST.POOL              ← column-specific
//  row 1  PROJECT  PROJECT   GROOVE    MODS        PROJECT    ← column-specific
//  row 2  SONG     CHAIN     PHRASE    INSTRUMENT  TABLE      ← the main row, always visible
//  row 3  MIXER    MIXER     MIXER     MIXER       MIXER      ← shared
//  row 4  EFFECTS  EFFECTS   EFFECTS   EFFECTS     EFFECTS    ← shared
//
// PROJECT / MIXER / EFFECTS have no column of their own; `previousColumn` remembers the one you came
// from, so leaving them is relative to it. The new column travels with the new screen, or they desync.
// The four `navigate_*` functions are PURE; `go_to_screen` applies the answer and the bookkeeping that
// goes with it.

#include "app_state.h"
#include "cursor_move.h"
#include "screen.h"

#include <algorithm>

namespace pt::ui {

/** Where R+DPAD lands, and the column memory that must be stored with it. */
struct NavResult {
    ScreenType screen = ScreenType::PHRASE;
    int        column = 2;
    /** Set by R+RIGHT out of the pool, so R+LEFT returns there instead of to PHRASE. Cleared by any
     *  move off INSTRUMENT (`apply_navigation`). */
    bool instrumentFromPool = false;
};

/** The inputs the four navigate functions read. */
struct NavState {
    ScreenType currentScreen     = ScreenType::PHRASE;
    int        previousColumn    = 2;
    bool       instrumentFromPool = false;
};

/** The column a screen owns, or −1 for the shared screens (PROJECT / MIXER / EFFECTS) that own none. */
inline int screen_column(ScreenType s) {
    switch (s) {
        case ScreenType::SONG:  return 0;
        case ScreenType::CHAIN: return 1;
        case ScreenType::PHRASE:
        case ScreenType::GROOVE:
        case ScreenType::SCALE: return 2;
        case ScreenType::INSTRUMENT:
        case ScreenType::MODS:
        case ScreenType::INST_POOL: return 3;
        case ScreenType::TABLE: return 4;
        default: return -1;  // shared / popup — the caller substitutes previousColumn
    }
}

/** The main-row (row 2) screen of a column. */
inline ScreenType main_screen_for_column(int column) {
    switch (column) {
        case 0:  return ScreenType::SONG;
        case 1:  return ScreenType::CHAIN;
        case 2:  return ScreenType::PHRASE;
        case 3:  return ScreenType::INSTRUMENT;
        case 4:  return ScreenType::TABLE;
        default: return ScreenType::PHRASE;
    }
}

inline bool is_main_row(ScreenType s) {
    for (ScreenType m : MAIN_ROW_SCREENS)
        if (m == s) return true;
    return false;
}

namespace detail {
/** The column to reason from: the screen's own, or the remembered one if it has none. */
inline int context_column(const NavState& s) {
    const int c = screen_column(s.currentScreen);
    return (c == -1) ? s.previousColumn : c;
}

/**
 * Does R+LEFT/RIGHT step SIDEWAYS off this screen onto the MAIN row one column over? True for row 1
 * and the shared rows — walking along row 4 would change nothing you can see. Not INST.POOL (it owns
 * the fast-jump pair), SCALE (drops to its own column's main screen) or the popups.
 */
inline bool exits_sideways_to_main_row(ScreenType s) {
    return s == ScreenType::PROJECT || s == ScreenType::GROOVE || s == ScreenType::MODS ||
           s == ScreenType::MIXER   || s == ScreenType::EFFECTS;
}

/**
 * A POPUP: no cell in the grid — no column of its own, and not a shared row.
 * ⚠️⚠️ R+DPAD must not move off one; B is the only way out. Without this, R+LEFT from SETTINGS, MIDI or
 * MIDI MAPPING fell through to `main_screen_for_column(-1)` = PHRASE. Derived from `screen_column`
 * (what the map paints from), so a new popup is covered automatically.
 */
inline bool is_popup(ScreenType s) {
    return screen_column(s) == -1 && !exits_sideways_to_main_row(s);
}
}  // namespace detail

inline NavResult navigate_up(const NavState& s) {
    const int col = detail::context_column(s);

    // Row-0 instrument (entered from the pool): nothing above it — stay.
    if (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool)
        return {ScreenType::INSTRUMENT, 3, true};

    switch (s.currentScreen) {
        case ScreenType::EFFECTS: return {ScreenType::MIXER, col};                    // row 4 → 3
        case ScreenType::MIXER:   return {main_screen_for_column(col), col};          // row 3 → 2

        case ScreenType::SONG:                                                        // row 2 → 1
        case ScreenType::CHAIN:
        case ScreenType::TABLE:      return {ScreenType::PROJECT, col};
        case ScreenType::PHRASE:     return {ScreenType::GROOVE, 2};
        case ScreenType::INSTRUMENT: return {ScreenType::MODS, 3};

        case ScreenType::PROJECT: return {ScreenType::PROJECT, col};                  // row 1 → 0
        case ScreenType::GROOVE:  return {ScreenType::SCALE, 2};
        case ScreenType::MODS:    return {ScreenType::INST_POOL, 3};

        default: return {s.currentScreen, col};  // row 0 (SCALE / INST_POOL) and the popups: stay
    }
}

inline NavResult navigate_down(const NavState& s) {
    const int col = detail::context_column(s);

    // Row-0 instrument (from the pool) drops to MODS, like the pool to its left.
    if (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool)
        return {ScreenType::MODS, 3};

    switch (s.currentScreen) {
        case ScreenType::SCALE:     return {ScreenType::GROOVE, 2};                   // row 0 → 1
        case ScreenType::INST_POOL: return {ScreenType::MODS, 3};

        case ScreenType::GROOVE:  return {ScreenType::PHRASE, 2};                     // row 1 → 2
        case ScreenType::MODS:    return {ScreenType::INSTRUMENT, 3};
        case ScreenType::PROJECT: return {main_screen_for_column(col), col};

        case ScreenType::SONG:                                                        // row 2 → 3
        case ScreenType::CHAIN:
        case ScreenType::PHRASE:
        case ScreenType::INSTRUMENT:
        case ScreenType::TABLE: return {ScreenType::MIXER, col};

        case ScreenType::MIXER:   return {ScreenType::EFFECTS, col};                  // row 3 → 4
        case ScreenType::EFFECTS: return {ScreenType::EFFECTS, col};                  // row 4: stay

        default: return {s.currentScreen, col};
    }
}

inline NavResult navigate_left(const NavState& s) {
    // A popup has no cell to move from — B is its way out. See `is_popup`.
    if (detail::is_popup(s.currentScreen)) return {s.currentScreen, s.previousColumn};

    // The pool fast-jump pair, R+LEFT half: the pool exits to PHRASE; an INSTRUMENT entered from the
    // pool returns to it (a normally entered one goes to PHRASE).
    if (s.currentScreen == ScreenType::INST_POOL) return {ScreenType::PHRASE, 2};
    if (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool)
        return {ScreenType::INST_POOL, 3, true};

    // Rows 1, 3 and 4 exit sideways onto the MAIN row, one column over. The column is DERIVED by
    // `context_column`, the same reading the navigation MAP paints, so picture and movement agree — and
    // MIXER/EFFECTS exit beside the screen they were entered from.
    if (detail::exits_sideways_to_main_row(s.currentScreen)) {
        const int contextCol = detail::context_column(s);
        const int target = contextCol - 1 < 0 ? 0 : contextCol - 1;
        return {main_screen_for_column(target), target};
    }

    // Any other non-main-row screen (SCALE): drop to the main row of its own column.
    if (!is_main_row(s.currentScreen)) {
        const int c = screen_column(s.currentScreen);
        return {main_screen_for_column(c), c};
    }

    switch (s.currentScreen) {  // along the main row: S C P I T
        case ScreenType::TABLE:      return {ScreenType::INSTRUMENT, 3};
        case ScreenType::INSTRUMENT: return {ScreenType::PHRASE, 2};
        case ScreenType::PHRASE:     return {ScreenType::CHAIN, 1};
        case ScreenType::CHAIN:      return {ScreenType::SONG, 0};
        case ScreenType::SONG:       return {ScreenType::SONG, 0};  // leftmost: stay
        default: return {s.currentScreen, s.previousColumn};
    }
}

inline NavResult navigate_right(const NavState& s) {
    // …and the same on the way back. See `navigate_left`.
    if (detail::is_popup(s.currentScreen)) return {s.currentScreen, s.previousColumn};

    // R+RIGHT out of the pool jumps to INSTRUMENT and MARKS it, so R+LEFT comes back to the pool.
    if (s.currentScreen == ScreenType::INST_POOL) return {ScreenType::INSTRUMENT, 3, true};
    // …and that row-0 instrument has nothing to its right — stay, rather than fall through to TABLE.
    if (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool)
        return {ScreenType::INSTRUMENT, 3, true};

    // The mirror of navigate_left's.
    if (detail::exits_sideways_to_main_row(s.currentScreen)) {
        const int contextCol = detail::context_column(s);
        const int target = contextCol + 1 > 4 ? 4 : contextCol + 1;
        return {main_screen_for_column(target), target};
    }

    if (!is_main_row(s.currentScreen)) {
        const int c = screen_column(s.currentScreen);
        return {main_screen_for_column(c), c};
    }

    switch (s.currentScreen) {
        case ScreenType::SONG:       return {ScreenType::CHAIN, 1};
        case ScreenType::CHAIN:      return {ScreenType::PHRASE, 2};
        case ScreenType::PHRASE:     return {ScreenType::INSTRUMENT, 3};
        case ScreenType::INSTRUMENT: return {ScreenType::TABLE, 4};
        case ScreenType::TABLE:      return {ScreenType::TABLE, 4};  // rightmost: stay
        default: return {s.currentScreen, s.previousColumn};
    }
}

// ─── Applying it ─────────────────────────────────────────────────────────────────────────────────

/** The NavState the four functions above want, read off the live AppState. */
inline NavState nav_state_of(const AppState& s) {
    return NavState{s.currentScreen, s.previousColumn, s.instrumentFromPool};
}

/**
 * Land on a screen, with the bookkeeping that goes with it — none of it optional:
 *   • THE CURSOR IS SAVED AND RESTORED: SONG, CHAIN and PHRASE share `cursorColumn` but have 8, 2 and
 *     9 columns, and a carried column outside the new screen makes the cursor DISAPPEAR.
 *   • TABLE FOLLOWS THE INSTRUMENT: arriving on TABLE syncs `currentTable` to `currentInstrument`.
 *   • The pool flag survives only on INSTRUMENT, so a stale one cannot reroute a later R+LEFT.
 */
inline void go_to_screen(AppState& s, const NavResult& r) {
    // Save where we were leaving from (REMEMBER mode reads these back).
    switch (s.currentScreen) {
        case ScreenType::SONG:
            s.songCursorRow = s.cursorRow;   s.songCursorColumn = s.cursorColumn;   break;
        case ScreenType::CHAIN:
            s.chainCursorRow = s.cursorRow;  s.chainCursorColumn = s.cursorColumn;  break;
        case ScreenType::PHRASE:
            s.phraseCursorRow = s.cursorRow; s.phraseCursorColumn = s.cursorColumn; break;
        default: break;
    }

    s.currentScreen  = r.screen;
    s.previousColumn = r.column;

    s.instrumentFromPool = (r.screen == ScreenType::INSTRUMENT) ? r.instrumentFromPool : false;

    if (r.screen == ScreenType::TABLE) {
        // Clamp to the pool AND mirror lastEditedTable — assigned bare, lastEditedTable would trail one
        // navigation behind.
        const int last    = static_cast<int>(s.project->tables.size()) - 1;
        s.currentTable    = std::min(last, std::max(0, s.currentInstrument));
        s.lastEditedTable = s.currentTable;
    }

    // Restore, or refresh (the default).
    // ⚠️ Under NAV = SONG, SONG and CHAIN always RESTORE: their cursors ARE the pointer
    // (ui/song_pointer.h), and a refresh would re-aim it at row 0 / track 1. PHRASE's row is a step,
    // so it refreshes normally.
    const bool pointerScreen = s.settings.navSongRelative &&
                               (r.screen == ScreenType::SONG || r.screen == ScreenType::CHAIN);
    if (s.settings.cursorRemember || pointerScreen) {
        switch (r.screen) {
            case ScreenType::SONG:
                s.cursorRow = s.songCursorRow;   s.cursorColumn = s.songCursorColumn;   break;
            case ScreenType::CHAIN:
                s.cursorRow = s.chainCursorRow;  s.cursorColumn = s.chainCursorColumn;  break;
            case ScreenType::PHRASE:
                s.cursorRow = s.phraseCursorRow; s.cursorColumn = s.phraseCursorColumn; break;
            // TABLE / GROOVE / the rest own their cursors outright — they persist by construction.
            default: break;
        }
    } else {
        // REFRESH — every screen that owns a cursor resets it to its top-left editable cell (except the
        // two the pointer owns under NAV = SONG). ⚠️ INSTRUMENT included: its row map changes shape with
        // the instrument type, and a stale row can land nowhere. EFFECTS, PROJECT and SETTINGS
        // deliberately persist in both modes.
        switch (r.screen) {
            case ScreenType::SONG:
            case ScreenType::CHAIN:
            case ScreenType::PHRASE:
                s.cursorRow    = 0;
                s.cursorColumn = min_cursor_column(r.screen);  // 1 — never the gutter
                break;
            case ScreenType::TABLE:
                s.tableCursorRow = 0; s.tableCursorColumn = 1;
                break;
            case ScreenType::GROOVE:
                s.grooveCursorRow    = 0;
                s.grooveCursorColumn = GROOVE_COL_TICK;
                s.groovePanelRow     = 0;
                s.groovePanelColumn  = 0;
                break;
            case ScreenType::INSTRUMENT:
                s.instrumentCursorRow = 0; s.instrumentCursorColumn = 1;
                break;
            case ScreenType::MODS:
                s.modCursorRow = 0; s.modCursorPair = 0; s.modCursorSide = 0;
                break;
            case ScreenType::INST_POOL:
                s.poolCursorColumn = 0;   // but NOT currentInstrument: that IS the pool's row
                break;
            case ScreenType::MIXER:
                s.mixerCursorColumn = 0; s.mixerMasterRow = 0;
                break;
            default: break;
        }
    }

    // ⚠️ Not a refresh — a BOUNDS check: the SETTINGS row map is caps-FILTERED, and the default row 0
    // (LAYOUT) is not drawn on the shell. Without it the first entry would leave the cursor on an
    // invisible row, with A+DPAD editing a touch setting on a device with no touch screen.
    if (r.screen == ScreenType::SETTINGS &&
        !settings_row_visible(static_cast<SettingsRow>(s.settingsCursorRow), s.caps)) {
        s.settingsCursorRow    = settings_first_visible_row(s.caps);
        s.settingsCursorColumn = 1;
    }

    // ⚠️ The same for the MIXER: a pair carried in under REMEMBER can name an undrawn cell. Row 0 exists
    // in every column, so the cursor comes back on the fader above where it was.
    if (r.screen == ScreenType::MIXER && !mixer_cell_exists(s.mixerMasterRow, s.mixerCursorColumn)) {
        s.mixerMasterRow    = 0;
        s.mixerCursorColumn = (s.mixerCursorColumn >= 0 && s.mixerCursorColumn <= 8)
                                  ? s.mixerCursorColumn
                                  : 0;
    }

    // SONG's viewport must contain its cursor, whichever branch above set it.
    if (r.screen == ScreenType::SONG) scroll_song_to_row(s, s.cursorRow);

    // ⭐ Under NAV = SONG the current* refs are READ from the cursors just restored — taken here, once,
    // below every screen change. A no-op under NAV = POOL.
    refresh_song_relative_refs(s);
}

}  // namespace pt::ui
