#pragma once

// ─── The THEME EDITOR ────────────────────────────────────────────────────────────────────────────
//
// Two header rows — THEME (the built-in cycle, SAVE, LOAD) and RANDOMIZE (the colour scheme and
// ROLL) — and one row per colour, each an R/G/B triple with a live swatch. The list scrolls.
//
// ⚠️ IT HAS NO CursorContext, ON PURPOSE: a value here is a CHANNEL OF A COLOUR, not a cell of a
// document, which CursorContext cannot say (the file browser and the waveform rows are the same).
// The edit is the two pure free functions below — a `Theme` in, a `Theme` out.

#include <cstdint>
#include <string>

#include "ui/canvas.h"
#include "ui/theme.h"
#include "ui/theme_random.h"

namespace pt::ui {

/** The overlay's live state — what `AppState` holds while it is up. */
struct ThemeEditorState {
    bool isOpen = false;

    /** 0 = THEME, 1 = RANDOMIZE, 2..`max_row()` = a colour row — see `theme_color_index`. */
    int cursorRow = 0;

    /**
     * On THEME: 0 = name, 1 = SAVE, 2 = LOAD. On RANDOMIZE: 0 = the scheme, 1 = ROLL. On a colour
     * row: 0 = R, 1 = G, 2 = B.
     *
     * ⚠️ THE ROWS DO NOT ALL HAVE THE SAME NUMBER OF CHANNELS, so the cursor's ring is a function of
     * the row rather than a constant 3 — see `theme_channel_count`.
     */
    int cursorChannel = 0;

    /**
     * ⚠️ SESSION STATE, DELIBERATELY. Locks are not written to settings.json: a lock the user set
     * days ago and has forgotten is a row that silently refuses to change, with nothing on screen
     * old enough to explain why.
     */
    ThemeLocks  locks{};
    ThemeScheme scheme = ThemeScheme::ANALOG;

    /** Advanced every roll, so holding RAND walks a sequence instead of re-rolling one palette. */
    uint32_t seed = 0x5EED0001u;

    /**
     * A failed roll's message, and ONLY that.
     *
     * ⚠️ THE CLASH MESSAGE IS NOT STORED HERE — it is DERIVED in the draw, so no site that moves the
     * cursor or changes a colour has to refresh it. A failed roll is an event, so that one is state.
     */
    std::string message;
};

// ─── The two header rows, and the one place the colour list is indexed ───────────────────────────
//
// ⚠️ NOTHING MAY WRITE `cursorRow - 2` ITSELF: the draw, nudge, lock, re-roll, clash message and
// help lookup all go through `theme_color_index`, and a missed site edits the wrong colour.

inline constexpr int THEME_ROW_THEME  = 0;   ///< the name, SAVE and LOAD
inline constexpr int THEME_ROW_RANDOM = 1;   ///< the scheme and ROLL
inline constexpr int THEME_FIRST_COLOR_ROW = 2;

/** Which colour a cursor row is on, or −1 when it is on a header row. */
inline int theme_color_index(int cursorRow) {
    const int index = cursorRow - THEME_FIRST_COLOR_ROW;
    return (index >= 0 && index < static_cast<int>(theme_color_rows().size())) ? index : -1;
}

/** THEME: name, SAVE, LOAD. RANDOMIZE: the scheme, ROLL. A colour row: R, G, B. */
inline int theme_channel_count(int cursorRow) {
    if (cursorRow == THEME_ROW_THEME)  return 3;
    if (cursorRow == THEME_ROW_RANDOM) return 2;
    return 3;
}

/** What the module is handed to draw one frame. */
struct ThemeState {
    Theme            theme = theme_classic();
    ThemeEditorState editor{};
};

// ─── The edit ────────────────────────────────────────────────────────────────────────────────────

/**
 * Nudge one channel of the colour under the cursor.
 *
 * ⚠️ `row` IS THE COLOUR'S OWN 1-BASED POSITION (`theme_color_index() + 1`), NOT THE CURSOR ROW. 0
 * and anything past the list are rejected. `delta` is ±0x01 (A+RIGHT/LEFT) or ±0x10 (A+UP/DOWN).
 *
 * ⚠️ Each channel CLAMPS at 0 and 255 — wrapping would drop a colour being dialled up to black.
 * The alpha is FORCED to 0xFF on every write: a theme colour is opaque, so a hand-edited translucent
 * one becomes opaque on the first nudge.
 */
inline void theme_adjust_color(Theme& theme, int row, int channel, int delta) {
    const auto& rows = theme_color_rows();
    if (row < 1 || row > static_cast<int>(rows.size())) return;   // 0 = not on a colour at all

    Argb Theme::* field = rows[static_cast<size_t>(row) - 1].field;
    const Argb current = theme.*field;

    const auto clamp255 = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };

    int r = static_cast<int>((current >> 16) & 0xFF);
    int g = static_cast<int>((current >> 8) & 0xFF);
    int b = static_cast<int>(current & 0xFF);

    if      (channel == 0) r = clamp255(r + delta);
    else if (channel == 1) g = clamp255(g + delta);
    else if (channel == 2) b = clamp255(b + delta);
    // else: nothing changes, but the colour is still rebuilt below with its alpha forced to 0xFF.

    theme.*field = 0xFF000000u
                 | (static_cast<Argb>(r) << 16)
                 | (static_cast<Argb>(g) << 8)
                 | static_cast<Argb>(b);
}

/**
 * Step the built-in palette: `delta` −1 (A+LEFT) or +1 (A+RIGHT).
 *
 * ⚠️ IT REPLACES THE WHOLE THEME — dialled colours are gone; SAVE is how you keep them.
 *
 * ⚠️ The directions are not inverses for a theme that is not a built-in (loaded or renamed, idx −1):
 * next lands on the first built-in, prev on the last — both enter the ring at an end.
 *
 * `visualizerType` rides across the swap — the palette belongs to the theme, the visualizer to the user.
 */
inline void theme_cycle_builtin(Theme& theme, int delta) {
    const std::vector<Theme> builtins = theme_builtins();
    const int size = static_cast<int>(builtins.size());

    int idx = -1;
    for (int i = 0; i < size; ++i)
        if (builtins[static_cast<size_t>(i)].name == theme.name) { idx = i; break; }

    int target;
    if (delta >= 0) target = (idx >= 0) ? (idx + 1) % size : 0;          // next
    else            target = (idx > 0)  ? idx - 1          : size - 1;   // prev

    const VisualizerType keep = theme.visualizerType;
    theme = builtins[static_cast<size_t>(target)];
    theme.visualizerType = keep;
}

// ─── The module ──────────────────────────────────────────────────────────────────────────────────

class ThemeEditorModule {
public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    /**
     * The last cursor row: one per colour, after the two header rows. The cursor WRAPS.
     * ⚠️ Derived from the row table, so a colour added to `theme_color_rows()` is reachable.
     */
    static int max_row() {
        return THEME_FIRST_COLOR_ROW + static_cast<int>(theme_color_rows().size()) - 1;
    }

    void draw(Canvas& c, int x, int y, const ThemeState& s) const;

    // ── The geometry: public because the scroll is a function of the cursor row.

    /** How many rows fit in the panel below the title. */
    static int visible_row_count();

    /** The first logical row drawn, given where the cursor is. 0 until the cursor pushes past the end. */
    static int scroll_offset(int cursor_row);

private:
    static constexpr int NAME_COL_X = 10;

    /** The clash mark, in the only free gap on the row: `BACKGROUND` ends at 180, R starts at 230. */
    static constexpr int WARN_COL_X = 200;

    static constexpr int R_COL_X    = 230;
    static constexpr int G_COL_X    = 267;
    static constexpr int B_COL_X    = 304;
    static constexpr int SWATCH_X   = 350;
    static constexpr int SWATCH_W   = WIDTH - SWATCH_X - 10;   // 150

    static constexpr int THEME_NAME_X   = 105;
    static constexpr int SAVE_LABEL_X   = 354;
    static constexpr int LOAD_LABEL_X   = 432;

    // ⚠️ The RANDOMIZE label runs to 163, past the THEME name column (105), so the scheme cell starts
    // at the next clear space; `SPLIT-COMP`, the longest scheme, bounds it before ROLL.
    static constexpr int SCHEME_LABEL_X = 175;
    static constexpr int ROLL_LABEL_X   = SAVE_LABEL_X;
};

}  // namespace pt::ui
