#pragma once

// The 5×5 screen grid R+DPAD moves around; the movement and the corner map both read `SCREEN_GRID`.
// A screen in several columns (PROJECT / MIXER / EFFECTS) is left relative to `previousColumn`, the one
// it was entered from. The `navigate_*` functions answer; `go_to_screen` applies.

#include "app_state.h"
#include "cursor_move.h"
#include "mixer_cell_layout.h"
#include "screen.h"

#include <algorithm>
#include <optional>

namespace pt::ui {

// Where R+DPAD lands, and the column to remember with it.
struct NavResult {
    ScreenType screen = ScreenType::PHRASE;
    int        column = 2;
    bool instrumentFromPool = false;   // R+RIGHT out of the pool: R+LEFT goes back there
};

struct NavState {
    ScreenType currentScreen     = ScreenType::PHRASE;
    int        previousColumn    = 2;
    bool       instrumentFromPool = false;
};

inline constexpr int NAV_ROWS     = 5;
inline constexpr int NAV_COLUMNS  = 5;
inline constexpr int NAV_MAIN_ROW = 2;   // always drawn on the map; R+LEFT/RIGHT walk along it

// [row][column]. Popups (SETTINGS, MIDI, the browser…) are not on it.
inline constexpr std::optional<ScreenType> SCREEN_GRID[NAV_ROWS][NAV_COLUMNS] = {
    {std::nullopt,         std::nullopt,        ScreenType::SCALE,   ScreenType::INST_POOL,  std::nullopt},
    {ScreenType::PROJECT,  ScreenType::PROJECT, ScreenType::GROOVE,  ScreenType::MODS,       ScreenType::PROJECT},
    {ScreenType::SONG,     ScreenType::CHAIN,   ScreenType::PHRASE,  ScreenType::INSTRUMENT, ScreenType::TABLE},
    {ScreenType::MIXER,    ScreenType::MIXER,   ScreenType::MIXER,   ScreenType::MIXER,      ScreenType::MIXER},
    {ScreenType::EFFECTS,  ScreenType::EFFECTS, ScreenType::EFFECTS, ScreenType::EFFECTS,    ScreenType::EFFECTS},
};

// The column a screen owns; −1 for one in several columns or none.
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
// The screen's own column, or the remembered one.
inline int context_column(const NavState& s) {
    const int c = screen_column(s.currentScreen);
    return (c == -1) ? s.previousColumn : c;
}

// −1 off the grid.
inline int screen_row(ScreenType s) {
    for (int row = 0; row < NAV_ROWS; ++row)
        for (const auto& cell : SCREEN_GRID[row])
            if (cell == s) return row;
    return -1;
}

// R+LEFT/RIGHT off rows 1, 3 and 4 step onto the main row one column over (row 0 drops to its own
// column's).
inline bool exits_sideways_to_main_row(ScreenType s) {
    const int row = screen_row(s);
    return row > 0 && row != NAV_MAIN_ROW;
}

// A popup has no cell in the grid. ⚠️ R+DPAD must not move off one; B is the way out.
inline bool is_popup(ScreenType s) { return screen_row(s) == -1; }

// R+UP/DOWN: the nearest screen above or below in the context column; stay if none.
inline NavResult step_in_column(const NavState& s, int step) {
    const int col = context_column(s);
    if (col < 0 || col >= NAV_COLUMNS) return {s.currentScreen, col};

    int row = -1;
    for (int r = 0; r < NAV_ROWS; ++r)
        if (SCREEN_GRID[r][col] == s.currentScreen) row = r;
    if (row == -1) return {s.currentScreen, col};

    for (int r = row + step; r >= 0 && r < NAV_ROWS; r += step)
        if (SCREEN_GRID[r][col]) return {*SCREEN_GRID[r][col], col};
    return {s.currentScreen, col};
}

// R+LEFT/RIGHT along the main row; stay at either end.
inline NavResult step_along_main_row(ScreenType s, int step) {
    const int col = std::clamp(screen_column(s) + step, 0, NAV_COLUMNS - 1);
    return {main_screen_for_column(col), col};
}

// R+LEFT/RIGHT once the pool's pair is answered: off rows 1, 3, 4 onto the main row beside, off row 0
// down to its own column's, else along the main row.
inline NavResult step_sideways(const NavState& s, int step) {
    if (exits_sideways_to_main_row(s.currentScreen)) {
        const int col    = context_column(s);
        const int target = step < 0 ? std::max(col - 1, 0) : std::min(col + 1, NAV_COLUMNS - 1);
        return {main_screen_for_column(target), target};
    }
    if (!is_main_row(s.currentScreen)) {
        const int c = screen_column(s.currentScreen);
        return {main_screen_for_column(c), c};
    }
    return step_along_main_row(s.currentScreen, step);
}
}  // namespace detail

inline NavResult navigate_up(const NavState& s) {
    // INSTRUMENT entered from the pool sits on row 0: nothing above.
    if (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool)
        return {ScreenType::INSTRUMENT, 3, true};
    return detail::step_in_column(s, -1);
}

inline NavResult navigate_down(const NavState& s) {
    if (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool)
        return {ScreenType::MODS, 3};
    return detail::step_in_column(s, +1);
}

inline NavResult navigate_left(const NavState& s) {
    if (detail::is_popup(s.currentScreen)) return {s.currentScreen, s.previousColumn};

    // The pool exits to PHRASE; INSTRUMENT entered from the pool returns to it.
    if (s.currentScreen == ScreenType::INST_POOL) return {ScreenType::PHRASE, 2};
    if (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool)
        return {ScreenType::INST_POOL, 3, true};
    return detail::step_sideways(s, -1);
}

inline NavResult navigate_right(const NavState& s) {
    if (detail::is_popup(s.currentScreen)) return {s.currentScreen, s.previousColumn};

    // Out of the pool to INSTRUMENT, marked so R+LEFT comes back; nothing to its right.
    if (s.currentScreen == ScreenType::INST_POOL) return {ScreenType::INSTRUMENT, 3, true};
    if (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool)
        return {ScreenType::INSTRUMENT, 3, true};
    return detail::step_sideways(s, +1);
}

// ─── Applying it ─────────────────────────────────────────────────────────────────────────────────

inline NavState nav_state_of(const AppState& s) {
    return NavState{s.currentScreen, s.previousColumn, s.instrumentFromPool};
}

// Land on a screen: save and restore the shared cursor (the three screens' column counts differ),
// point TABLE at the current instrument, keep the pool flag only on INSTRUMENT.
inline void go_to_screen(AppState& s, const NavResult& r) {
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
        const int last    = static_cast<int>(s.project->tables.size()) - 1;
        s.currentTable    = std::min(last, std::max(0, s.currentInstrument));
        s.lastEditedTable = s.currentTable;
    }

    // Restore, or refresh (the default). ⚠️ Under NAV = SONG, SONG and CHAIN always restore: their
    // cursors are the pointer, and a refresh would re-aim it.
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
            default: break;   // the rest own their cursors
        }
    } else {
        // Back to the top-left editable cell. EFFECTS, PROJECT and SETTINGS persist in both modes.
        switch (r.screen) {
            case ScreenType::SONG:
            case ScreenType::CHAIN:
            case ScreenType::PHRASE:
                s.cursorRow    = 0;
                s.cursorColumn = min_cursor_column(r.screen);
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
                s.poolCursorColumn = 0;   // not currentInstrument: that is the pool's row
                break;
            case ScreenType::MIXER:
                s.mixerCursorColumn = 0; s.mixerMasterRow = 0;
                break;
            default: break;
        }
    }

    // ⚠️ Bounds checks, in both modes: a remembered cell may not be drawn here. SETTINGS hides rows per
    // platform (row 0 is not drawn on the desktop).
    if (r.screen == ScreenType::SETTINGS &&
        !settings_row_visible(static_cast<SettingsRow>(s.settingsCursorRow), s.caps)) {
        s.settingsCursorRow    = settings_first_visible_row(s.caps);
        s.settingsCursorColumn = 1;
    }

    // MIXER: back to the fader above, which exists in every column.
    if (r.screen == ScreenType::MIXER && !mixer_cell_exists(s.mixerMasterRow, s.mixerCursorColumn)) {
        s.mixerMasterRow    = 0;
        s.mixerCursorColumn = (s.mixerCursorColumn >= 0 && s.mixerCursorColumn <= 8)
                                  ? s.mixerCursorColumn
                                  : 0;
    }

    // INSTRUMENT: the layouts differ by type.
    if (r.screen == ScreenType::INSTRUMENT) snap_instrument_cursor(s);

    if (r.screen == ScreenType::SONG) scroll_song_to_row(s, s.cursorRow);

    // Under NAV = SONG the current* refs are read from the cursors just restored.
    refresh_song_relative_refs(s);
}

}  // namespace pt::ui
