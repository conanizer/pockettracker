#pragma once

// ─── Screen navigation ───────────────────────────────────────────────────────────────────────────
//
// The 5×5 screen grid R+DPAD moves around. `SCREEN_GRID` is the one copy: the movement below and the
// map in the corner (`modules/navigation_map.cpp`) both read it, so a screen is placed by adding it there.
//
// A screen in several columns (PROJECT / MIXER / EFFECTS) has no column of its own; `previousColumn`
// remembers the one you came from, so leaving them is relative to it. The new column travels with the
// new screen, or they desync. The four `navigate_*` functions are PURE; `go_to_screen` applies the
// answer and the bookkeeping that goes with it.

#include "app_state.h"
#include "cursor_move.h"
#include "mixer_cell_layout.h"
#include "screen.h"

#include <algorithm>
#include <optional>

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

inline constexpr int NAV_ROWS     = 5;
inline constexpr int NAV_COLUMNS  = 5;
inline constexpr int NAV_MAIN_ROW = 2;   // always drawn on the map; R+LEFT/RIGHT walk along it

/** [row][column]. Empty where nothing is. Popups (SETTINGS, MIDI, the browser…) are not on it. */
inline constexpr std::optional<ScreenType> SCREEN_GRID[NAV_ROWS][NAV_COLUMNS] = {
    {std::nullopt,         std::nullopt,        ScreenType::SCALE,   ScreenType::INST_POOL,  std::nullopt},
    {ScreenType::PROJECT,  ScreenType::PROJECT, ScreenType::GROOVE,  ScreenType::MODS,       ScreenType::PROJECT},
    {ScreenType::SONG,     ScreenType::CHAIN,   ScreenType::PHRASE,  ScreenType::INSTRUMENT, ScreenType::TABLE},
    {ScreenType::MIXER,    ScreenType::MIXER,   ScreenType::MIXER,   ScreenType::MIXER,      ScreenType::MIXER},
    {ScreenType::EFFECTS,  ScreenType::EFFECTS, ScreenType::EFFECTS, ScreenType::EFFECTS,    ScreenType::EFFECTS},
};

/** The column a screen owns, or −1 for one in several columns or none (the popups). */
inline int screen_column(ScreenType s) {
    int found = -1;
    for (int row = 0; row < NAV_ROWS; ++row)
        for (int col = 0; col < NAV_COLUMNS; ++col)
            if (SCREEN_GRID[row][col] == s) {
                if (found != -1 && found != col) return -1;
                found = col;
            }
    return found;
}

/** The main-row screen of a column. */
inline ScreenType main_screen_for_column(int column) {
    if (column < 0 || column >= NAV_COLUMNS) return ScreenType::PHRASE;
    return *SCREEN_GRID[NAV_MAIN_ROW][column];
}

inline bool is_main_row(ScreenType s) {
    for (const auto& cell : SCREEN_GRID[NAV_MAIN_ROW])
        if (cell == s) return true;
    return false;
}

namespace detail {
/** The column to reason from: the screen's own, or the remembered one if it has none. */
inline int context_column(const NavState& s) {
    const int c = screen_column(s.currentScreen);
    return (c == -1) ? s.previousColumn : c;
}

/** The grid row a screen sits on (each sits on one), or −1 off the grid. */
inline int screen_row(ScreenType s) {
    for (int row = 0; row < NAV_ROWS; ++row)
        for (const auto& cell : SCREEN_GRID[row])
            if (cell == s) return row;
    return -1;
}

/**
 * Does R+LEFT/RIGHT step SIDEWAYS off this screen onto the MAIN row one column over? True for rows 1,
 * 3 and 4 — walking along row 4 would change nothing you can see. Row 0 drops to its own column's main
 * screen instead (INST.POOL also owns a fast-jump pair, handled before this is asked).
 */
inline bool exits_sideways_to_main_row(ScreenType s) {
    const int row = screen_row(s);
    return row > 0 && row != NAV_MAIN_ROW;
}

/**
 * A POPUP: no cell in the grid.
 * ⚠️⚠️ R+DPAD must not move off one; B is the only way out. Without this, R+LEFT from SETTINGS, MIDI or
 * MIDI MAPPING fell through to `main_screen_for_column(-1)` = PHRASE.
 */
inline bool is_popup(ScreenType s) { return screen_row(s) == -1; }

/** R+UP / R+DOWN: the nearest screen above (−1) or below (+1) in the context column; stay if none. */
inline NavResult step_in_column(const NavState& s, int step) {
    const int col = context_column(s);
    if (col < 0 || col >= NAV_COLUMNS) return {s.currentScreen, col};

    int row = -1;
    for (int r = 0; r < NAV_ROWS; ++r)
        if (SCREEN_GRID[r][col] == s.currentScreen) row = r;
    if (row == -1) return {s.currentScreen, col};   // a popup, or a screen this column does not hold

    for (int r = row + step; r >= 0 && r < NAV_ROWS; r += step)
        if (SCREEN_GRID[r][col]) return {*SCREEN_GRID[r][col], col};
    return {s.currentScreen, col};
}

/** R+LEFT / R+RIGHT along the main row; stay at either end. */
inline NavResult step_along_main_row(ScreenType s, int step) {
    const int col = std::clamp(screen_column(s) + step, 0, NAV_COLUMNS - 1);
    return {main_screen_for_column(col), col};
}
}  // namespace detail

inline NavResult navigate_up(const NavState& s) {
    // Row-0 instrument (entered from the pool): nothing above it — stay.
    if (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool)
        return {ScreenType::INSTRUMENT, 3, true};
    return detail::step_in_column(s, -1);
}

inline NavResult navigate_down(const NavState& s) {
    // Row-0 instrument (from the pool) drops to MODS, like the pool to its left.
    if (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool)
        return {ScreenType::MODS, 3};
    return detail::step_in_column(s, +1);
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

    return detail::step_along_main_row(s.currentScreen, -1);
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

    return detail::step_along_main_row(s.currentScreen, +1);
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
