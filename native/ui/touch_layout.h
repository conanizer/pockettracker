#pragma once

// ─── THE TOUCH LAYOUT — SIZE ARITHMETIC ──────────────────────────────────────────────────────────
//
// The C++ twin of `input/TouchLayoutMetrics.kt`: the SIZE maths behind the four on-screen touch
// layouts (the LEFT box, the RIGHT box, the two-box PORTRAIT split, and the 135-unit PORTRAIT2 grid).
// Given a control-box size in pixels and the display density, it answers how big each button is and
// how much space sits between them. Nothing else. No SDL, no Canvas, no window — this header includes
// `<cstdint>`, `<cmath>` and `<algorithm>` and stops, exactly like `buttons.h`/`button_mapper.h`,
// which is what lets `tools/pttouch` link it with nothing and byte-compare it against the golden.
//
// ── WHY THIS IS SHARED C++ AND NOT SHELL CODE (convergence D1) ────────────────────────────────────
//
// The convergence plan (§6 D1) splits the touch skin along a seam the architecture already draws:
// the hit-rect LAYOUT is pure arithmetic over (available width, height, density) — portable, and
// covered by a golden — so it lives HERE in the shared tree; the RENDERING (the casing, bezel and
// button PNGs composited around the letterboxed 640×480 frame) is device-resolution chrome and lives
// in the shell (`sdl-video.cpp`). This file is the first, shared half. The canvas keeps its four
// primitives; nothing image-shaped ever reaches `pt-ui`.
//
// ── ⚠️ SIZES, NOT POSITIONS — the same split `TouchLayoutMetrics.kt` states ───────────────────────
//
// A touch layout is two computations wearing one coat and only one of them is here:
//
//   SIZES     — "each arrow is X px square, spacers are 0.2X" — plain arithmetic. That is everything
//               below, and it is what `testdata/units/touch-layout.txt` (the B2 golden) pins.
//   POSITIONS — "the UP arrow lands at x=340, y=210" — produced on Android by COMPOSE's measure/layout
//               pass (Column/Row + Arrangement.Center; Portrait2's Modifier.weight). It exists in no
//               Kotlin file, so no JVM test recorded it and this header does not compute it either.
//               The arrangement is ported and eyeball-verified on a device in a later D increment;
//               a wrong PROPORTION at an unowned screen size is what the golden catches, a wrong
//               ARRANGEMENT is what looking at a phone catches.
//
// ── ⚠️ IEEE-EXACTNESS IS A CORRECTNESS REQUIREMENT HERE ───────────────────────────────────────────
//
// The golden records the font sizes and every Portrait2 field as raw binary32 BITS — nothing passes
// by being close. So every float operation below must land on the same bits Kotlin's `Float` maths
// did: the literals are `f`-suffixed (the arithmetic stays in binary32, never promoting to double),
// the multiply/divide order matches the Kotlin expression exactly, and `tools/pttouch` is built with
// `pt_ieee_exact()` so no fma contraction rounds `a*b` differently than the JVM did. `patternUnit`'s
// `.toInt()` is a truncation of an already-floored value, which `static_cast<int>` reproduces.
//
// The single fork in the file is `pattern_unit`'s `boxRatio < patternRatio` (the `<` is strict, so
// the exact tie takes the HEIGHT arm — golden case 340×510 pins it). The constants are float because
// the Kotlin ones are: `3.4f / 5.1f` is compared as a ratio of floats, not a rational.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pt::ui::touch_layout {

// Pattern: 3.4w × 5.1h for each side box; Portrait2's grid is 135 units on its shorter axis. The
// literals mirror `TouchLayoutMetrics`'s `PATTERN_WIDTH`/`PATTERN_HEIGHT`/`GRID_UNITS`.
inline constexpr float PATTERN_WIDTH  = 3.4f;
inline constexpr float PATTERN_HEIGHT = 5.1f;
inline constexpr float GRID_UNITS     = 135.0f;

/**
 * The base unit X for the 3.4×5.1 pattern: the largest whole pixel size at which the pattern still
 * fits inside (w, h). Shared by the LEFT box, the RIGHT box and the two-box PORTRAIT split — one
 * function, not three copies, exactly as in the Kotlin.
 *
 * Copied verbatim including the `.toInt()` placement: both branches yield a Float and the truncation
 * applies to the whole if-expression, so `static_cast<int>` wraps the floored result.
 */
inline int pattern_unit(int available_width, int available_height) {
    const float box_ratio     = static_cast<float>(available_width) / static_cast<float>(available_height);
    const float pattern_ratio = PATTERN_WIDTH / PATTERN_HEIGHT;
    const float v = (box_ratio < pattern_ratio)
                        ? std::floor(static_cast<float>(available_width) / PATTERN_WIDTH)
                        : std::floor(static_cast<float>(available_height) / PATTERN_HEIGHT);
    return static_cast<int>(v);
}

/** LEFT box: L trigger, D-pad, SELECT. */
struct Left {
    int   x;
    int   button_size;
    int   l_button_width;
    int   l_button_height;
    int   select_width;
    int   select_height;
    int   small_spacer;
    int   large_spacer;
    int   medium_spacer_width;
    float main_font_sp;
    float trigger_font_sp;
    float small_font_sp;
};

/** RIGHT box: R trigger, A, B, START. Not a mirror of LEFT — see `medium_spacer` (0.7X vs LEFT's
 *  1.0X) and the left/right spacers RIGHT alone carries. */
struct Right {
    int   x;
    int   button_size;
    int   r_button_width;
    int   r_button_height;
    int   start_width;
    int   start_height;
    int   small_spacer;
    int   medium_spacer;
    int   large_spacer;
    int   left_spacer;
    int   right_spacer;
    float main_font_sp;
    float trigger_font_sp;
    float small_font_sp;
};

/** The two-box PORTRAIT split. Each box gets HALF the width, and the box size it computes is handed
 *  down to `left()`/`right()` as their own available size — so an error here moves every button. */
struct Portrait {
    int x;
    int box_width;
    int box_height;
};

/** PORTRAIT2: the 135-unit grid. `x` is a FLOAT (flooring would leave sub-unit edge gaps on any
 *  resolution that is not a multiple of 135) and is floored at 1.0. */
struct Portrait2 {
    float x;
    float cell_dp;
    float padding_dp;
    float large_sp;
    float small_sp;
    float sq_off_x_dp;
    float wide_off_x_dp;
    float off_y_dp;
    float pressed_dp;
};

inline Left left(int available_width, int available_height, float density) {
    const int x = pattern_unit(available_width, available_height);
    Left m;
    m.x                   = x;
    m.button_size         = x;
    m.l_button_width      = static_cast<int>(std::floor(x * 1.5f));
    m.l_button_height     = static_cast<int>(std::floor(x * 0.7f));
    m.select_width        = static_cast<int>(std::floor(x * 1.2f));
    m.select_height       = static_cast<int>(std::floor(x * 0.6f));
    m.small_spacer        = static_cast<int>(std::floor(x * 0.2f));
    m.large_spacer        = static_cast<int>(std::floor(x * 2.0f));
    m.medium_spacer_width = static_cast<int>(std::floor(x * 1.0f));
    m.main_font_sp        = x * 0.4f / density;
    m.trigger_font_sp     = x * 0.35f / density;
    m.small_font_sp       = x * 0.25f / density;
    return m;
}

inline Right right(int available_width, int available_height, float density) {
    const int x = pattern_unit(available_width, available_height);
    Right m;
    m.x               = x;
    m.button_size     = x;
    m.r_button_width  = static_cast<int>(std::floor(x * 1.5f));
    m.r_button_height = static_cast<int>(std::floor(x * 0.7f));
    m.start_width     = static_cast<int>(std::floor(x * 1.2f));
    m.start_height    = static_cast<int>(std::floor(x * 0.6f));
    m.small_spacer    = static_cast<int>(std::floor(x * 0.2f));
    // ⚠️ 0.7f here where LEFT's medium_spacer_width is 1.0f; the two boxes are not mirror images.
    m.medium_spacer   = static_cast<int>(std::floor(x * 0.7f));
    m.large_spacer    = static_cast<int>(std::floor(x * 2.0f));
    m.left_spacer     = static_cast<int>(std::floor(x * 1.7f));
    m.right_spacer    = static_cast<int>(std::floor(x * 0.7f));
    m.main_font_sp    = x * 0.4f / density;
    m.trigger_font_sp = x * 0.35f / density;
    m.small_font_sp   = x * 0.25f / density;
    return m;
}

inline Portrait portrait(int available_width, int available_height) {
    const int box_available_width  = available_width / 2;  // integer division, as in Kotlin
    const int box_available_height = available_height;
    const int x = pattern_unit(box_available_width, box_available_height);
    Portrait m;
    m.x          = x;
    m.box_width  = static_cast<int>(std::floor(x * PATTERN_WIDTH));
    m.box_height = static_cast<int>(std::floor(x * PATTERN_HEIGHT));
    return m;
}

inline Portrait2 portrait2(int available_width, int available_height, float density) {
    // minOf(w/135, h/135).coerceAtLeast(1f): the smaller axis ratio, floored at 1.0. For our
    // positive, non-NaN inputs std::min/std::max reproduce Kotlin's minOf/coerceAtLeast exactly.
    const float x = std::max(std::min(static_cast<float>(available_width) / GRID_UNITS,
                                      static_cast<float>(available_height) / GRID_UNITS),
                             1.0f);
    Portrait2 m;
    m.x             = x;
    m.cell_dp       = x * 33.0f / density;
    m.padding_dp    = x * 1.5f / density;
    m.large_sp      = x * 11.0f / density;
    m.small_sp      = x * 7.0f / density;
    m.sq_off_x_dp   = x * 7.0f / density;
    m.wide_off_x_dp = x * 8.0f / density;
    m.off_y_dp      = x * 4.0f / density;
    m.pressed_dp    = x * 1.0f / density;
    return m;
}

}  // namespace pt::ui::touch_layout
