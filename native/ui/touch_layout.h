#pragma once

// ─── THE TOUCH LAYOUT — SIZE ARITHMETIC ──────────────────────────────────────────────────────────
//
// The C++ twin of `input/TouchLayoutMetrics.kt`: the SIZE maths behind the four on-screen touch
// layouts (the LEFT box, the RIGHT box, the two-box PORTRAIT split, and the 135-unit PORTRAIT2 grid).
// Given a control-box size in pixels and the display density, it answers how big each button is and
// how much space sits between them — and, since convergence D3, WHERE each button lands inside its
// box (see the POSITIONS section at the foot of the file). No SDL, no Canvas, no window — this header
// includes `<cstdint>`, `<cmath>`, `<algorithm>` and the leaf `buttons.h` (for the `Button` the rects
// map onto) and stops, exactly like `buttons.h`/`button_mapper.h`, which is what lets `tools/pttouch`
// link it with nothing and check it.
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
// ── ⚠️ SIZES vs POSITIONS — the same split `TouchLayoutMetrics.kt` states, now both ported ────────
//
// A touch layout is two computations wearing one coat, and the two are checked by DIFFERENT means:
//
//   SIZES     — "each arrow is X px square, spacers are 0.2X" — plain arithmetic. `left`/`right`/
//               `portrait`/`portrait2` below, and it is what `testdata/units/touch-layout.txt`
//               (the B2 golden) pins, byte-for-byte, through `tools/pttouch`.
//   POSITIONS — "the UP arrow lands at x=253, y=305 inside its box" — produced on Android by COMPOSE's
//               measure/layout pass (Column/Row + Arrangement.Center). It exists in no Kotlin file, so
//               no JVM test could record it: there is NO golden for it and there never can be. So the
//               `*_rects` functions in the POSITIONS section port the ARRANGEMENT by reading
//               `VirtualControls.kt`'s composables directly, and are checked the way the combo matrix
//               and the dispatcher are — by a HAND-WRITTEN ORACLE (`pttouch --positions`), the
//               ptmapper/ptdispatch pattern — plus eyes on a device. A wrong PROPORTION at an unowned
//               screen size is what the golden catches; a wrong ARRANGEMENT is what the oracle and the
//               phone catch. ⚠️ D3 ported the two LANDSCAPE boxes (LEFT/RIGHT); the PORTRAIT2 skinned
//               grid joined them (`portrait2_rects`, whose columns come from Compose's WEIGHT split,
//               not the size arithmetic), and then the PORTRAIT2 device-SKIN band geometry
//               (`portrait2_skin` — the four chrome bands and the frame-in-bezel positioning that the
//               button cluster hangs off). PORTRAIT's two-box split gets its `*_rects` when it lights up.
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

#include "buttons.h"  // pt::ui::Button — the enum the POSITIONS rects map onto. A leaf (<cstdint>),
                      // so `tools/pttouch` still links nothing.

namespace pt::ui::touch_layout {

// Pattern: 3.4w × 5.1h for each side box; Portrait2's grid is 135 units on its shorter axis. The
// literals mirror `TouchLayoutMetrics`'s `PATTERN_WIDTH`/`PATTERN_HEIGHT`/`GRID_UNITS`.
inline constexpr float PATTERN_WIDTH  = 3.4f;
inline constexpr float PATTERN_HEIGHT = 5.1f;
inline constexpr float GRID_UNITS     = 135.0f;

// The tracker design in pixels — 640×480, mirroring `pt::ui::DESIGN_W`/`DESIGN_H` (canvas.h) and
// Kotlin's `DESIGN_WIDTH_PX`/`DESIGN_HEIGHT_PX` (PixelPerfectRenderer.kt). Restated locally so the
// PORTRAIT2 skin geometry below can integer-scale the frame into the bezel without pulling in
// canvas.h — the "links nothing but buttons.h" discipline the whole header keeps.
inline constexpr int DESIGN_FRAME_W = 640;
inline constexpr int DESIGN_FRAME_H = 480;

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

// ─── POSITIONS — where each button LANDS inside its box (convergence D3) ──────────────────────────
//
// ⚠️ NOT golden-backed, and this is the file's most important caveat. The SIZE functions above match
// bytes Kotlin recorded; there are no such bytes for POSITIONS, because Compose's measure/layout pass
// produces them and no JVM test could snapshot it. These functions instead PORT the arrangement by
// reading `VirtualControls.kt`'s composables, and the check for them is a hand-written oracle
// (`pttouch --positions`) plus a device — the ptmapper/ptdispatch contract, weaker than a golden and
// honest about it.
//
// ── Why there is no `density` argument here, unlike the size functions ────────────────────────────
//
// `VirtualControlsLeft/Right` size every child `(px / density).dp`, and Compose measures a `.dp` back
// to pixels as `dp * density` — so the density cancels and the child is `px` pixels on screen, the
// same int `left()`/`right()` already computed. The ARRANGEMENT is therefore pure integer-pixel
// arithmetic over those ints (`button_size`, `small_spacer`, …); density touches only the FONT sp,
// which positions do not use. So `left_rects`/`right_rects` call `left`/`right` with a throwaway
// density and read only the int fields.
//
// ── The Compose arrangement these reproduce ───────────────────────────────────────────────────────
//
//   Column(verticalArrangement = Center, horizontalAlignment = CenterHorizontally): the children are
//   packed into one block whose height is their sum, and that block is centred in the box height; each
//   child (a button, or a Row) is centred in the box width. A Row wraps its content (no fillMaxWidth),
//   so `Arrangement.Center` inside it has no slack and the children are packed left-to-right; a
//   width-only `Spacer` has zero height, so it shifts its neighbours across without adding a row.
//
// Coordinates are BOX-LOCAL: origin at the box's top-left. The shell adds the box's on-screen origin.

/** One laid-out button and where it sits inside its box (box-local pixels). */
struct ButtonRect {
    Button button;
    int    x, y, w, h;
    int  cx() const { return x + w / 2; }
    int  cy() const { return y + h / 2; }
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

/** A laid-out box. Fixed storage — LEFT fills 6, RIGHT fills 4, PORTRAIT2's grid fills all 10 — no
 *  allocation, so the "links nothing" discipline of the size structs carries over. Iterate
 *  `r[0..count)` in draw order. */
struct BoxRects {
    ButtonRect r[10];
    int        count = 0;
};

/** LEFT box: L on top, the D-pad cross (UP / LEFT·RIGHT / DOWN), SELECT below — 6 rects, in draw
 *  order. Ported from `VirtualControlsLeft`. */
inline BoxRects left_rects(int available_width, int available_height) {
    const Left m = left(available_width, available_height, 1.0f);  // density irrelevant to the int fields
    const int  W = available_width;

    // The Column's content height (spacers + children), then Arrangement.Center's top offset. Row
    // heights are the tallest real child: the D-pad row is button_size, the SELECT row select_height.
    const int content_h =
        4 * m.small_spacer + m.l_button_height + 3 * m.button_size + m.select_height;
    int y = (available_height - content_h) / 2;

    BoxRects box;
    auto push = [&](Button b, int x, int yy, int w, int h) { box.r[box.count++] = {b, x, yy, w, h}; };
    auto cx   = [&](int w) { return (W - w) / 2; };  // centre a child of width w in the box

    y += m.small_spacer;  // Spacer
    push(Button::L_SHIFT, cx(m.l_button_width), y, m.l_button_width, m.l_button_height);
    y += m.l_button_height + m.small_spacer;  // + Spacer
    push(Button::DPAD_UP, cx(m.button_size), y, m.button_size, m.button_size);
    y += m.button_size;
    {  // Row: [small] LEFT [medium] RIGHT [small], centred as a unit
        const int row_w = 2 * m.small_spacer + 2 * m.button_size + m.medium_spacer_width;
        const int row_x = (W - row_w) / 2;
        push(Button::DPAD_LEFT, row_x + m.small_spacer, y, m.button_size, m.button_size);
        push(Button::DPAD_RIGHT, row_x + m.small_spacer + m.button_size + m.medium_spacer_width, y,
             m.button_size, m.button_size);
    }
    y += m.button_size;
    push(Button::DPAD_DOWN, cx(m.button_size), y, m.button_size, m.button_size);
    y += m.button_size + m.small_spacer;  // + Spacer
    {  // Row: [large] SELECT [small], centred
        const int row_w = m.large_spacer + m.select_width + m.small_spacer;
        const int row_x = (W - row_w) / 2;
        push(Button::SELECT, row_x + m.large_spacer, y, m.select_width, m.select_height);
    }
    return box;
}

/** RIGHT box: R on top, A and B on a diagonal (A upper-right, B lower-left), START below — 4 rects,
 *  in draw order. Ported from `VirtualControlsRight`; A/B are NOT a mirror pair, see the spacers. */
inline BoxRects right_rects(int available_width, int available_height) {
    const Right m = right(available_width, available_height, 1.0f);
    const int   W = available_width;

    const int content_h = 2 * m.small_spacer + 2 * m.medium_spacer + m.r_button_height +
                          2 * m.button_size + m.start_height;
    int y = (available_height - content_h) / 2;

    BoxRects box;
    auto push = [&](Button b, int x, int yy, int w, int h) { box.r[box.count++] = {b, x, yy, w, h}; };

    y += m.small_spacer;  // Spacer
    push(Button::R_SHIFT, (W - m.r_button_width) / 2, y, m.r_button_width, m.r_button_height);
    y += m.r_button_height + m.medium_spacer;  // + Spacer
    {  // Row: [left] A [right], centred — leftSpacer > rightSpacer, so A sits to the RIGHT
        const int row_w = m.left_spacer + m.button_size + m.right_spacer;
        const int row_x = (W - row_w) / 2;
        push(Button::A, row_x + m.left_spacer, y, m.button_size, m.button_size);
    }
    y += m.button_size;
    {  // Row: [right] B [left], centred — same total width, so B sits to the LEFT of where A was
        const int row_w = m.right_spacer + m.button_size + m.left_spacer;
        const int row_x = (W - row_w) / 2;
        push(Button::B, row_x + m.right_spacer, y, m.button_size, m.button_size);
    }
    y += m.button_size + m.medium_spacer;  // + Spacer
    {  // Row: [small] START [large], centred
        const int row_w = m.small_spacer + m.start_width + m.large_spacer;
        const int row_x = (W - row_w) / 2;
        push(Button::START, row_x + m.small_spacer, y, m.start_width, m.start_height);
    }
    return box;
}

// ─── PORTRAIT2: the skinned 4-row grid (convergence D — the skinned grid) ─────────────────────────
//
// Unlike LEFT/RIGHT (a Column of fixed-size children packed by Arrangement.Center), PORTRAIT2 is a
// grid whose column widths come from Compose's WEIGHT distribution, not from the size arithmetic
// above. The cluster box (available_width × available_height) holds a Column, inset by
// `pad = round(1.5X)` on every side, of four `Row`s each `round(33X)` tall (the `cellDp` height). Every
// child in a row is `weight(1f)`, so Compose splits the row's content width (available_width − 2·pad)
// into equal slots — two in row 0 (the wide L/R shift), four in rows 1–3 — handing the leftover pixels
// out one per child, left to right. Some slots are empty `Spacer`s (the grid's holes). Ported from
// `VirtualControlsPortrait2`; density cancels for positions, exactly as it does for LEFT/RIGHT.
//
//   Row 0:  [ L Shift ][ R Shift ]     (2 wide slots)
//   Row 1:  [    ][ ↑ ][ B ][ A ]      (4 slots; slot 0 empty)
//   Row 2:  [ ← ][ ↓ ][ → ][    ]      (4 slots; slot 3 empty)
//   Row 3:  [    ][Sel][Sta][    ]     (4 slots; slots 0 and 3 empty)

/**
 * Split an integer width `W` into `n` equal-weight slots the way a Compose `Row` does: each slot gets
 * `round(W/n)`, then the leftover `W − n·round(W/n)` pixels (which may be negative) are distributed one
 * per slot, in order, until spent — `RowColumnImpl`'s remainder loop verbatim (`remainderUnit =
 * remainder.sign` each pass). Fills `xs`/`ws` with each slot's left edge and width; the widths sum to
 * exactly `W` (no gap, no overflow), so the columns tile the row. `round` is Kotlin's `roundToInt`,
 * `floor(v + 0.5)`, matched here in binary32 so a half lands the way the JVM's did.
 */
inline void weight_split(int W, int n, int xs[], int ws[]) {
    const float unit = static_cast<float>(W) / static_cast<float>(n);  // weightUnitSpace, each weight = 1
    const int   base = static_cast<int>(std::floor(unit + 0.5f));      // round(W/n)
    int remainder = W - n * base;
    int x = 0;
    for (int k = 0; k < n; ++k) {
        const int unit_adj = (remainder > 0) ? 1 : (remainder < 0 ? -1 : 0);  // remainder.sign
        remainder -= unit_adj;
        ws[k] = base + unit_adj;
        xs[k] = x;
        x += ws[k];
    }
}

/** PORTRAIT2 grid: all ten buttons, box-local, in row-major draw order. Ported from
 *  `VirtualControlsPortrait2`. The empty grid slots hold `Spacer`s and so get no rect — a tap there
 *  hits nothing, which is what `pttouch --positions` asserts. */
inline BoxRects portrait2_rects(int available_width, int available_height) {
    const Portrait2 m = portrait2(available_width, available_height, 1.0f);  // density irrelevant to positions
    const int pad    = static_cast<int>(std::floor(m.x * 1.5f + 0.5f));   // round(1.5X) — the .padding()
    const int cell_h = static_cast<int>(std::floor(m.x * 33.0f + 0.5f));  // round(33X)  — each Row's .height(cellDp)

    const int content_w = available_width - 2 * pad;  // the fillMaxWidth() Rows all share this
    int x2[2], w2[2];  weight_split(content_w, 2, x2, w2);  // row 0: two wide slots
    int x4[4], w4[4];  weight_split(content_w, 4, x4, w4);  // rows 1–3: four square slots

    BoxRects box;
    auto push = [&](Button b, int col_x, int col_w, int row) {
        box.r[box.count++] = {b, pad + col_x, pad + row * cell_h, col_w, cell_h};
    };
    // Row 0: L Shift | R Shift
    push(Button::L_SHIFT, x2[0], w2[0], 0);
    push(Button::R_SHIFT, x2[1], w2[1], 0);
    // Row 1: [empty] ↑ B A
    push(Button::DPAD_UP, x4[1], w4[1], 1);
    push(Button::B,       x4[2], w4[2], 1);
    push(Button::A,       x4[3], w4[3], 1);
    // Row 2: ← ↓ → [empty]
    push(Button::DPAD_LEFT,  x4[0], w4[0], 2);
    push(Button::DPAD_DOWN,  x4[1], w4[1], 2);
    push(Button::DPAD_RIGHT, x4[2], w4[2], 2);
    // Row 3: [empty] Sel Sta [empty]
    push(Button::SELECT, x4[1], w4[1], 3);
    push(Button::START,  x4[2], w4[2], 3);
    return box;
}

// ─── PORTRAIT2: the DEVICE-SKIN band geometry (convergence D — the skinned renderer) ──────────────
//
// `portrait2_rects` above lays the ten buttons out INSIDE the cluster box. This is the layer OUTSIDE
// it: where that cluster — and the tracker frame, and the three chrome bands — land on the whole
// device screen. It ports `PortraitLayout2WithVirtualButtons` (ScreenLayouts.kt), the retro-device
// skin — a 135X × 300X canvas (20:9) of four bands stacked top-to-bottom:
//
//     ┌───────────────┐  band 1  top vent panel   (bg_top_panel.png)      — height varies, may be 0
//     │               │  band 2  screen bezel      (bg_screen_bezel.png)   — the 640×480 frame sits INSIDE
//     ├───────────────┤  band 3  branding strip    (bg_branding_panel.png) — FULL device width
//     └───────────────┘  band 4  button cluster    (bg_button_backing.png) — portrait2_rects fills it
//
// ⚠️ **THE HEADLINE: the frame is NOT centred in the window** (as it is in landscape/fullscreen — see
// `sdl-video.cpp`'s `dest_rect`). It sits in band 2, inside the bezel's padding, integer-scaled and
// centred THERE — well up in the top half of a tall screen, because the button cluster fills the
// bottom. That is exactly the positioning the shell's PORTRAIT2 present path needs, and the reason
// this is a function of its own rather than a tweak to the centred `dest_rect`.
//
// Like `portrait2_rects`, this is portable pixel arithmetic — density touches ONLY the solid-colour
// bezel's thickness fallback — checked by a HAND-WRITTEN ORACLE (`pttouch --positions`) plus eyes on a
// device: there is no golden for a Compose measure/layout result. The X-derivation's three cases
// (A: the full skin fits; B: the top panel shrinks to absorb a height deficit; C: height-constrained,
// so the skin is NARROWER than the device and the casing fills the sides) are ported verbatim — case C
// is why the branding band spans the DEVICE width, not the skin's, and why `xFromWidth` (not `X`)
// sizes it.

/** A plain rectangle in device-output pixels — no `Button`, unlike `ButtonRect`. `empty()` (h/w ≤ 0)
 *  marks a band that is absent, e.g. the top panel in case C. */
struct LayoutRect {
    int  x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

/** The full PORTRAIT2 device skin, laid out on a `deviceW × deviceH` screen. The four `band` rects
 *  composite their chrome textures; `frame` is where the 640×480 tracker texture blits (band 2, in the
 *  bezel — NOT centred); `buttons` is the cluster that `portrait2_rects(buttons.w, max(buttons.h,100))`
 *  fills, its rects offset by `buttons.{x,y}`. Ported from `PortraitLayout2WithVirtualButtons`. */
struct Portrait2Skin {
    LayoutRect topPanel;    // band 1 — bg_top_panel.png       (empty in case C)
    LayoutRect bezel;       // band 2 — bg_screen_bezel.png
    LayoutRect branding;    // band 3 — bg_branding_panel.png  (FULL device width)
    LayoutRect buttons;     // band 4 — bg_button_backing.png + the button grid
    LayoutRect frame;       // the 640×480 tracker frame, integer-scaled, centred in the bezel's inner area
    LayoutRect innerBezel;  // the bezel's padded inner (black) area — a FIT-mode frame the shell fits here itself
    float      x = 0.0f;    // the skin unit X (135X == skin width in px)
};

/**
 * Lay out the PORTRAIT2 device skin on a `deviceW × deviceH` output. `bezelThicknessX > 0` expresses
 * the bezel border in skin X-units (a skin with a bezel PNG — amiga/amiga-2 both use 3f); otherwise it
 * falls back to `bezelThicknessDp * density` (a solid-colour bezel, e.g. the DARK theme). `density`
 * is used ONLY on that fallback path — the band pixel geometry is otherwise density-free, exactly as
 * positions are for `portrait2_rects`.
 */
inline Portrait2Skin portrait2_skin(int deviceW, int deviceH, float density,
                                    float bezelThicknessDp, float bezelThicknessX) {
    // ── X and the top-panel height: the three aspect cases, verbatim from ScreenLayouts.kt ──
    const float xFromWidth = static_cast<float>(deviceW) / 135.0f;
    // Branding always spans the full DEVICE width, so its height is proportional to deviceW (via
    // xFromWidth), NOT to X — in case C, X is smaller, and using it here would shrink a full-width band.
    const int brandingH = static_cast<int>(xFromWidth * 22.5f);

    float X;
    int   topPanelH;
    if (xFromWidth * 300.0f <= static_cast<float>(deviceH)) {          // Case A: full skin fits vertically
        X         = xFromWidth;
        topPanelH = std::min(static_cast<int>(deviceH - X * 267.0f), static_cast<int>(X * 33.0f));
    } else if (xFromWidth * 267.0f <= static_cast<float>(deviceH)) {   // Case B: top panel shrinks to absorb deficit
        X         = xFromWidth;
        topPanelH = std::max(static_cast<int>(deviceH - X * 267.0f), 0);
    } else {                                                           // Case C: height-constrained, skin < device wide
        topPanelH = 0;
        X         = static_cast<float>(deviceH - brandingH) / (102.75f + 141.75f);
    }

    const int contentW    = static_cast<int>(X * 135.0f);
    const int bezelH      = static_cast<int>(X * 102.75f);
    const int buttonAreaH = static_cast<int>(X * 141.75f);
    const int contentX    = (deviceW - contentW) / 2;                 // Column horizontalAlignment = CenterHorizontally

    Portrait2Skin s;
    s.x = X;

    // ── The four bands, stacked top-to-bottom (Arrangement.Top). Branding is fillMaxWidth ⇒ device-wide. ──
    int y = 0;
    if (topPanelH > 0) s.topPanel = {contentX, y, contentW, topPanelH};
    y += topPanelH;
    s.bezel    = {contentX, y, contentW, bezelH};
    y += bezelH;
    s.branding = {0, y, deviceW, brandingH};
    y += brandingH;
    s.buttons  = {contentX, y, contentW, buttonAreaH};

    // ── The bezel's padded inner area, and the 640×480 frame integer-scaled and centred inside it. ──
    const float bezelThickPx = (bezelThicknessX > 0.0f) ? (X * bezelThicknessX)
                                                        : (bezelThicknessDp * density);
    const int   innerX = contentX + static_cast<int>(bezelThickPx);
    const int   innerY = topPanelH + static_cast<int>(bezelThickPx);
    const int   innerW = static_cast<int>(static_cast<float>(contentW) - 2.0f * bezelThickPx);
    const int   innerH = static_cast<int>(static_cast<float>(bezelH)   - 2.0f * bezelThickPx);
    s.innerBezel = {innerX, innerY, innerW, innerH};

    // PixelPerfectRenderer.kt:274-281 — the tracker picks its OWN integer scale from the box it is
    // given (here the inner bezel area), the `+1` its rounding fudge, and centres, its black background
    // hiding the letterbox. Integer centring (as the shell's `dest_rect` uses), not Kotlin's `/2f`: the
    // ≤1px difference is below the threshold B2 draws — a wrong proportion is what these checks catch;
    // a 1px bezel inset is what an eye on a device would.
    const int scaleX  = (innerW + 1) / DESIGN_FRAME_W;
    const int scaleY  = (innerH + 1) / DESIGN_FRAME_H;
    const int scale   = std::max(std::min(scaleX, scaleY), 1);
    const int renderW = DESIGN_FRAME_W * scale;
    const int renderH = DESIGN_FRAME_H * scale;
    const int offX    = std::max(0, (innerW - renderW) / 2);
    const int offY    = std::max(0, (innerH - renderH) / 2);
    s.frame = {innerX + offX, innerY + offY, renderW, renderH};

    return s;
}

/** Which button in `box` contains the box-local point, if any. Draw order == first hit; the oracle
 *  asserts the rects never overlap, so first-hit is the only hit. */
inline bool hit(const BoxRects& box, int px, int py, Button& out) {
    for (int i = 0; i < box.count; ++i) {
        if (box.r[i].contains(px, py)) {
            out = box.r[i].button;
            return true;
        }
    }
    return false;
}

}  // namespace pt::ui::touch_layout
