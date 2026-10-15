#pragma once

// The theme editor: a THEME row (built-in cycle, SAVE, LOAD), a RANDOMIZE row (scheme, ROLL), then
// one R/G/B row per colour with a swatch. No CursorContext on purpose: a value here is a channel of
// a colour, not a document cell; the edit is the free functions below.

#include <cstdint>
#include <string>

#include "ui/canvas.h"
#include "ui/theme.h"
#include "ui/theme_random.h"

namespace pt::ui {

struct ThemeEditorState {
    bool isOpen = false;

    // 0 = THEME, 1 = RANDOMIZE, 2.. = a colour row (`theme_color_index`).
    int cursorRow = 0;

    // THEME: 0 name, 1 SAVE, 2 LOAD. RANDOMIZE: 0 scheme, 1 ROLL. A colour: R, G, B.
    // ⚠️ Rows differ in channel count — `theme_channel_count`.
    int cursorChannel = 0;

    // ⚠️ Session only, on purpose: a forgotten lock saved for days is a row that silently refuses
    // to change.
    ThemeLocks  locks{};
    ThemeScheme scheme = ThemeScheme::ANALOG;

    // Advanced every roll, so holding RAND walks a sequence.
    uint32_t seed = 0x5EED0001u;

    // A failed roll's message. The clash message is derived in the draw, not stored.
    std::string message;
};

// ⚠️ Nothing may compute `cursorRow - 2` itself: every site goes through `theme_color_index`.

inline constexpr int THEME_ROW_THEME  = 0;   ///< the name, SAVE and LOAD
inline constexpr int THEME_ROW_RANDOM = 1;   ///< the scheme and ROLL
inline constexpr int THEME_FIRST_COLOR_ROW = 2;

// The colour a cursor row is on, or −1 on a header row.
inline int theme_color_index(int cursorRow) {
    const int index = cursorRow - THEME_FIRST_COLOR_ROW;
    return (index >= 0 && index < static_cast<int>(theme_color_rows().size())) ? index : -1;
}

inline int theme_channel_count(int cursorRow) { return cursorRow == THEME_ROW_RANDOM ? 2 : 3; }

struct ThemeState {
    Theme            theme = theme_classic();
    ThemeEditorState editor{};
};

// ─── The edit ────────────────────────────────────────────────────────────────────────────────────

// Nudge one channel of a colour by `delta` (±0x01 or ±0x10).
// ⚠️ `row` is the colour's 1-based position (`theme_color_index() + 1`), not the cursor row.
// Channels clamp (wrapping would drop a colour to black); alpha is forced to 0xFF.
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

    theme.*field = 0xFF000000u
                 | (static_cast<Argb>(r) << 16)
                 | (static_cast<Argb>(g) << 8)
                 | static_cast<Argb>(b);
}

// Step the built-in palette by `delta` (±1). ⚠️ Replaces the whole theme; SAVE keeps dialled colours.
// A theme that is not a built-in enters the ring at an end. The visualizer is kept.
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

    // One row per colour after the two headers; derived from `theme_color_rows()`. The cursor wraps.
    static int max_row() {
        return THEME_FIRST_COLOR_ROW + static_cast<int>(theme_color_rows().size()) - 1;
    }

    void draw(Canvas& c, int x, int y, const ThemeState& s) const;

    // ── Geometry, public because the scroll follows the cursor row ───────────────────────────────

    static int visible_row_count();

    // 0 until the cursor pushes past the end.
    static int scroll_offset(int cursor_row);

private:
    static constexpr int NAME_COL_X = 10;

    // The clash mark, in the gap between `BACKGROUND` and R.
    static constexpr int WARN_COL_X = 200;

    static constexpr int R_COL_X    = 230;
    static constexpr int G_COL_X    = 267;
    static constexpr int B_COL_X    = 304;
    static constexpr int SWATCH_X   = 350;
    static constexpr int SWATCH_W   = WIDTH - SWATCH_X - 10;   // 150

    static constexpr int THEME_NAME_X   = 105;
    static constexpr int SAVE_LABEL_X   = 354;
    static constexpr int LOAD_LABEL_X   = 432;

    // The RANDOMIZE label runs past the THEME name column, so the scheme starts after it.
    static constexpr int SCHEME_LABEL_X = 175;
    static constexpr int ROLL_LABEL_X   = SAVE_LABEL_X;
};

}  // namespace pt::ui
