#pragma once

// ─── The THEME EDITOR ────────────────────────────────────────────────────────────────────────────
//
// The 1:1 twin of `ui/modules/ThemeEditorModule.kt`, and the last screen in the Kotlin dispatcher the
// port had not reached. Two header rows — THEME (the built-in cycle, SAVE, LOAD) and RANDOMIZE (the
// colour scheme and ROLL) — and one row per colour under them, each an R/G/B triple with a live
// swatch. The list is taller than the panel, so it scrolls — the same idea as SONG and the browser.
//
// ⚠️ IT HAS NO CursorContext, AND THAT IS NOT AN OVERSIGHT. Kotlin's `handleGenericInput` opens with
// `if (themeEditorState.isOpen) return` — the whole cursor-context system is bypassed, and the four
// A+DPAD handlers call `adjustThemeColor` directly. It is the third screen in the app to work that way
// (the file browser has no context either, and the sample editor's waveform rows have none), and for
// the same reason all three do: a value here is a CHANNEL OF A COLOUR, not a cell of a document, and
// nothing in `CursorContext`'s vocabulary can say that.
//
// So the edit lives in the two free functions below rather than in a `handle_input` override. They are
// pure — a `Theme` in, a `Theme` out, no engine, no project, no canvas — which is what lets `ptinput`
// byte-compare them against the real Kotlin `adjustThemeColor` and `cycle*BuiltinTheme` directly,
// instead of a copy of their arithmetic living in the test. (S6a's lesson, one layer up: a fixture that
// re-derives the thing it is measuring cannot catch the thing it is measuring.)

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
     * ⚠️ **THE CLASH MESSAGE IS NOT STORED HERE — it is DERIVED in the draw** from the palette and
     * the cursor. Storing it would mean every future site that moves the cursor or changes a colour
     * has to remember to refresh it, and the cost of forgetting is a line of text describing a
     * palette that no longer exists. A failed roll is an EVENT and has nowhere to be derived from,
     * which is why that one is state.
     */
    std::string message;
};

// ─── The two header rows, and the one place the colour list is indexed ───────────────────────────
//
// ⚠️ **NOTHING MAY WRITE `cursorRow - 2` ITSELF.** The offset was 1 while THEME was the only header
// row, it is 2 now, and it is read by the draw, the nudge, the lock, the re-roll, the clash message
// and the help lookup — six sites, one of which silently edits the wrong colour if it is missed.

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
 * Nudge one channel of the colour under the cursor. Kotlin's `AppInputDispatcher.adjustThemeColor`.
 *
 * ⚠️ `row` IS THE COLOUR'S OWN 1-BASED POSITION, NOT THE CURSOR ROW — `theme_color_index() + 1`.
 * The two were the same number while THEME was the only header row above the list, and they are not
 * any more. 0 and anything past the list are rejected, as Kotlin rejects them.
 * `delta` is ±0x01 from A+RIGHT / A+LEFT and ±0x10 from A+UP / A+DOWN.
 *
 * ⚠️ Each channel CLAMPS at 0 and 255 — it does not wrap. Rolling 0xFF over to 0x00 would take a
 * colour you are dialling UP and drop it to black, which is not a nudge, it is a cliff.
 *
 * ⚠️ And the alpha is FORCED to 0xFF on every write (`0xFF000000L or …`), not preserved. A theme colour
 * is opaque by construction; there is no alpha channel in the editor and no row to reach one from, so a
 * `.ptt` hand-edited to carry a translucent colour is made opaque again by the first nudge. Kotlin's,
 * reproduced.
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
    // else: NOTHING changes — but note that the colour is still REBUILT below with its alpha forced to
    // 0xFF, rather than returned untouched. That is Kotlin's `else ->` arm, which reassembles r/g/b and
    // ORs the alpha back on exactly as the three real arms do. It is unreachable (the cursor wraps 0..2
    // and nothing else calls this), and it is mirrored anyway so the golden can sweep an out-of-range
    // channel and PROVE it is a no-op instead of assuming it — which, on a translucent colour from a
    // hand-edited .ptt, it would NOT be.

    theme.*field = 0xFF000000u
                 | (static_cast<Argb>(r) << 16)
                 | (static_cast<Argb>(g) << 8)
                 | static_cast<Argb>(b);
}

/**
 * Step the built-in palette. `delta` is −1 (A+LEFT, Kotlin's `cyclePrevBuiltinTheme`) or +1 (A+RIGHT,
 * `cycleNextBuiltinTheme`).
 *
 * ⚠️ IT REPLACES THE WHOLE THEME, so every colour the user has dialled on the current one is GONE. That
 * is Kotlin's — the cycle is how you pick a starting palette, and SAVE is how you keep one you have
 * changed. Reproduced without a guard, because adding a "you have unsaved changes" dialog here would be
 * a behaviour the phone does not have.
 *
 * ⚠️ The two directions are NOT exact inverses, and that falls out of Kotlin's two expressions rather
 * than being designed:
 *
 *     next = if (idx >= 0) (idx + 1) % size else 0        // an unknown name → CLASSIC
 *     prev = if (idx > 0)  idx - 1        else size - 1   // an unknown name → MONO
 *
 * `idx` is the index of the CURRENT theme's NAME among the built-ins, so a palette loaded from a `.ptt`
 * (or any edited-and-renamed one) is not in the list at all — it is −1. Stepping DOWN from it lands on
 * CLASSIC and stepping UP lands on MONO: both "enter the ring at an end", which is sane, and neither
 * takes you back where you came from, which is the honest consequence of the theme having left the ring
 * the moment it stopped being a built-in. Ported as written.
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
     * The last cursor row — one per colour, after the two header rows. The list scrolls; the cursor
     * does not clamp, it WRAPS.
     *
     * ⚠️ DERIVED FROM THE ROW TABLE, not typed. A colour added to `theme_color_rows()` is a row the
     * module draws; if this were a constant it would also be a row the cursor could never reach, and
     * the only symptom would be a colour that quietly cannot be edited.
     */
    static int max_row() {
        return THEME_FIRST_COLOR_ROW + static_cast<int>(theme_color_rows().size()) - 1;
    }

    void draw(Canvas& c, int x, int y, const ThemeState& s) const;

    // ── The geometry, exposed because the scroll is a function of the cursor and ptshot photographs
    //    it at every row (and because a row the module can draw but the cursor cannot reach, or the
    //    reverse, is exactly the class of bug S2 found in the nav map).

    /** How many rows fit in the panel below the title. */
    static int visible_row_count();

    /** The first logical row drawn, given where the cursor is. 0 until the cursor pushes past the end. */
    static int scroll_offset(int cursor_row);

private:
    static constexpr int NAME_COL_X = 10;

    /**
     * The clash mark, between the longest label and the R column.
     *
     * ⚠️ IT IS THE ONLY GAP ON THE ROW. The swatch runs to within 10px of the panel's edge, so there
     * is nothing free to its right; `BACKGROUND` is ten characters and ends at 180, and R starts at
     * 230, which leaves exactly one character of air here.
     */
    static constexpr int WARN_COL_X = 200;

    static constexpr int R_COL_X    = 230;
    static constexpr int G_COL_X    = 267;
    static constexpr int B_COL_X    = 304;
    static constexpr int SWATCH_X   = 350;
    static constexpr int SWATCH_W   = WIDTH - SWATCH_X - 10;   // 150

    static constexpr int THEME_NAME_X   = 105;
    static constexpr int SAVE_LABEL_X   = 354;
    static constexpr int LOAD_LABEL_X   = 432;

    // ⚠️ THE RANDOMIZE ROW'S FIRST CELL CANNOT SIT UNDER THE THEME NAME'S. Its label is nine
    // characters where `THEME` is five, so it runs to 163 and the name column starts at 105. The
    // scheme cell takes the next clear space instead, and `SPLIT-COMP` — the longest of the seven —
    // is what sets how far right it may start before it reaches ROLL.
    static constexpr int SCHEME_LABEL_X = 175;
    static constexpr int ROLL_LABEL_X   = SAVE_LABEL_X;
};

}  // namespace pt::ui
