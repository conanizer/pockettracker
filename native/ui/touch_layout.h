#pragma once

// ─── THE TOUCH LAYOUT — SIZE AND POSITION ARITHMETIC ─────────────────────────────────────────────
//
// The on-screen touch layouts (the LEFT box, the RIGHT box, the two-box PORTRAIT split, the 135-unit
// PORTRAIT2 grid): given a box size in pixels and the display density, how big each button is, how
// much space sits between them, and where each lands. Only `<cstdint>`, `<cmath>`, `<algorithm>` and
// `buttons.h`, so its test links nothing else. The hit-rect layout is portable arithmetic and
// lives here; the RENDERING (casing, bezel, button PNGs) is the shell's (`sdl-video.cpp`) — nothing
// image-shaped reaches `pt-ui`.
//
// Two computations, checked differently:
//   SIZES     — "each arrow is X px square, spacers 0.2X": `left`/`right`/`portrait`/`portrait2`, pinned
//               byte for byte by the touch-layout golden.
//   POSITIONS — "UP lands at x=253, y=305 in its box": the `*_rects` functions, checked by a
//               HAND-WRITTEN ORACLE plus eyes on a device — no golden exists.
//               PORTRAIT's two-box split has no `*_rects` yet.
//
// ⚠️ IEEE-EXACTNESS IS REQUIRED: the golden records font sizes and every Portrait2 field as raw binary32
// bits. Literals are `f`-suffixed (no promotion to double), multiply/divide order is fixed, and
// Its test is built with `pt_ieee_exact()` (no fma contraction).
// The one fork, `pattern_unit`'s `boxRatio < patternRatio`, is strict: the exact tie takes the HEIGHT
// arm (golden case 340×510).

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "buttons.h"  // pt::ui::Button, for the POSITIONS rects — a leaf (<cstdint>)

namespace pt::ui::touch_layout {

// Pattern: 3.4w × 5.1h for each side box; Portrait2's grid is 135 units on its shorter axis.
inline constexpr float PATTERN_WIDTH  = 3.4f;
inline constexpr float PATTERN_HEIGHT = 5.1f;
inline constexpr float GRID_UNITS     = 135.0f;

// The tracker design in pixels (= canvas.h's DESIGN_W/H), restated so this header need not include
// canvas.h.
inline constexpr int DESIGN_FRAME_W = 640;
inline constexpr int DESIGN_FRAME_H = 480;

/**
 * The base unit X for the 3.4×5.1 pattern: the largest whole pixel size at which the pattern fits in
 * (w, h). Shared by LEFT, RIGHT and the PORTRAIT split. The truncation applies to the floored result of
 * either branch.
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

/** PORTRAIT2: the 135-unit grid. `x` is a FLOAT (flooring would leave sub-unit edge gaps at sizes not a
 *  multiple of 135), floored at 1.0. */
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
    const int box_available_width  = available_width / 2;  // integer division
    const int box_available_height = available_height;
    const int x = pattern_unit(box_available_width, box_available_height);
    Portrait m;
    m.x          = x;
    m.box_width  = static_cast<int>(std::floor(x * PATTERN_WIDTH));
    m.box_height = static_cast<int>(std::floor(x * PATTERN_HEIGHT));
    return m;
}

inline Portrait2 portrait2(int available_width, int available_height, float density) {
    // The smaller axis ratio, floored at 1.0.
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

// ─── POSITIONS — where each button LANDS inside its box ──────────────────────────────────────────
//
// ⚠️ NOT golden-backed: checked by the hand-written oracle and a device.
//
// No `density` argument: children are sized `px / density` dp and measured back as `dp * density`, so
// density cancels and positions are integer-pixel arithmetic over the size structs' ints (density
// only affects font size).
//
// The arrangement: a Column, vertically centred, children horizontally centred. A Row wraps its
// content, so its children pack left to right with no slack; a width-only Spacer shifts neighbours
// without adding a row. Coordinates are BOX-LOCAL (origin top-left); the shell adds the box origin.

/** One laid-out button and where it sits inside its box (box-local pixels). */
struct ButtonRect {
    Button button;
    int    x, y, w, h;
    int  cx() const { return x + w / 2; }
    int  cy() const { return y + h / 2; }
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

/** A laid-out box: fixed storage (LEFT fills 6, RIGHT 4, PORTRAIT2 all 10), no allocation. Iterate
 *  `r[0..count)` in draw order. */
struct BoxRects {
    ButtonRect r[10];
    int        count = 0;
};

/** LEFT box: L on top, the D-pad cross (UP / LEFT·RIGHT / DOWN), SELECT below — 6 rects, draw order. */
inline BoxRects left_rects(int available_width, int available_height) {
    const Left m = left(available_width, available_height, 1.0f);  // density irrelevant to the ints
    const int  W = available_width;

    // The Column's content height, then the centring offset. A row's height is its tallest child: the
    // D-pad row is button_size, the SELECT row select_height.
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

/** RIGHT box: R on top, A and B on a diagonal (A upper-right, B lower-left), START below — 4 rects in
 *  draw order. A/B are NOT a mirror pair (see the spacers). */
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

// ─── PORTRAIT2: the skinned 4-row grid ───────────────────────────────────────────────────────────
//
// Column widths come from an equal-WEIGHT split, not the size arithmetic. The cluster box holds a
// column inset by `pad = round(1.5X)`, of four rows each `round(33X)` tall; each row splits its content
// width (available_width − 2·pad) into equal slots — two in row 0, four in rows 1–3 — with leftover
// pixels handed out one per slot, left to right. Some slots are empty (holes).
//
//   Row 0:  [ L Shift ][ R Shift ]     (2 wide slots)
//   Row 1:  [    ][ ↑ ][ B ][ A ]      (4 slots; slot 0 empty)
//   Row 2:  [ ← ][ ↓ ][ → ][    ]      (4 slots; slot 3 empty)
//   Row 3:  [    ][Sel][Sta][    ]     (4 slots; slots 0 and 3 empty)

/**
 * Split width `W` into `n` equal-weight slots: each gets `round(W/n)`, then the leftover
 * `W − n·round(W/n)` (possibly negative) is distributed one per slot, in order, until spent. The widths
 * sum to exactly `W`. `round` is `floor(v + 0.5)` in binary32, so a half lands consistently.
 */
inline void weight_split(int W, int n, int xs[], int ws[]) {
    const float unit = static_cast<float>(W) / static_cast<float>(n);  // each weight = 1
    const int   base = static_cast<int>(std::floor(unit + 0.5f));      // round(W/n)
    int remainder = W - n * base;
    int x = 0;
    for (int k = 0; k < n; ++k) {
        const int unit_adj = (remainder > 0) ? 1 : (remainder < 0 ? -1 : 0);  // the remainder's sign
        remainder -= unit_adj;
        ws[k] = base + unit_adj;
        xs[k] = x;
        x += ws[k];
    }
}

/** PORTRAIT2 grid: all ten buttons, box-local, row-major draw order. Empty slots get no rect — a tap
 *  there hits nothing (the oracle asserts it). */
inline BoxRects portrait2_rects(int available_width, int available_height) {
    const Portrait2 m = portrait2(available_width, available_height, 1.0f);  // density irrelevant
    const int pad    = static_cast<int>(std::floor(m.x * 1.5f + 0.5f));   // round(1.5X) — the inset
    const int cell_h = static_cast<int>(std::floor(m.x * 33.0f + 0.5f));  // round(33X) — each row's height

    const int content_w = available_width - 2 * pad;  // every row shares this
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

// ─── PORTRAIT2: the DEVICE-SKIN band geometry ────────────────────────────────────────────────────
//
// Outside the cluster: where the cluster, the tracker frame and three chrome bands land on the whole
// screen — a 135X × 300X (20:9) skin of four bands, top to bottom:
//
//     ┌───────────────┐  band 1  top vent panel   (bg_top_panel.png)      — height varies, may be 0
//     │               │  band 2  screen bezel      (bg_screen_bezel.png)   — the 640×480 frame sits INSIDE
//     ├───────────────┤  band 3  branding strip    (bg_branding_panel.png) — FULL device width
//     └───────────────┘  band 4  button cluster    (bg_button_backing.png) — portrait2_rects fills it
//
// ⚠️ The frame is NOT centred in the window (unlike landscape, `sdl-video.cpp`'s `dest_rect`): it sits in
// band 2, integer-scaled and centred inside the bezel's padding, high on a tall screen.
// Checked by the hand-written oracle and a device. X has three cases — A: the full skin fits; B: the top
// panel shrinks to absorb a height deficit; C: height-constrained, so the skin is NARROWER than the
// device and the casing fills the sides — which is why branding spans the DEVICE width, sized by
// `xFromWidth`, not `X`.

/** A plain rectangle in device-output pixels — no `Button`, unlike `ButtonRect`. `empty()` (h/w ≤ 0)
 *  marks a band that is absent, e.g. the top panel in case C. */
struct LayoutRect {
    int  x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

/** The full PORTRAIT2 skin on a `deviceW × deviceH` screen. The four bands composite their textures;
 *  `frame` is where the 640×480 tracker blits (in the bezel, NOT centred); `buttons` is the cluster that
 *  `portrait2_rects(buttons.w, max(buttons.h,100))` fills, offset by `buttons.{x,y}`. */
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
 * Lay out the PORTRAIT2 skin on a `deviceW × deviceH` output. `bezelThicknessX > 0` is the bezel border
 * in skin X units (a skin with a bezel PNG); otherwise `bezelThicknessDp * density` (a solid-colour
 * bezel). `density` is used ONLY there.
 */
inline Portrait2Skin portrait2_skin(int deviceW, int deviceH, float density,
                                    float bezelThicknessDp, float bezelThicknessX) {
    // ── X and the top-panel height: the three aspect cases ──
    const float xFromWidth = static_cast<float>(deviceW) / 135.0f;
    // Branding spans the full DEVICE width, so its height follows xFromWidth, not X (smaller in case C).
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
    const int contentX    = (deviceW - contentW) / 2;                 // the column is centred

    Portrait2Skin s;
    s.x = X;

    // ── The four bands, stacked top to bottom; branding is device-wide. ──
    int y = 0;
    if (topPanelH > 0) s.topPanel = {contentX, y, contentW, topPanelH};
    y += topPanelH;
    s.bezel    = {contentX, y, contentW, bezelH};
    y += bezelH;
    s.branding = {0, y, deviceW, brandingH};
    y += brandingH;
    s.buttons  = {contentX, y, contentW, buttonAreaH};

    // ── The bezel's padded inner area, and the 640×480 frame integer-scaled and centred in it. ──
    const float bezelThickPx = (bezelThicknessX > 0.0f) ? (X * bezelThicknessX)
                                                        : (bezelThicknessDp * density);
    const int   innerX = contentX + static_cast<int>(bezelThickPx);
    const int   innerY = topPanelH + static_cast<int>(bezelThickPx);
    const int   innerW = static_cast<int>(static_cast<float>(contentW) - 2.0f * bezelThickPx);
    const int   innerH = static_cast<int>(static_cast<float>(bezelH)   - 2.0f * bezelThickPx);
    s.innerBezel = {innerX, innerY, innerW, innerH};

    // The tracker picks its own integer scale from the inner bezel area (`+1` a rounding fudge) and
    // centres; its black background hides the letterbox. Integer centring, like `dest_rect`.
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

// ─── PORTRAIT2 BARE: the same cluster, no chrome, and the screen gets the rest ────────────────────
//
// For a skin with BUTTON ART ONLY — no vent panel, branding, backing or bezel. Two bands:
//
//     ┌───────────────┐  the SCREEN area — FULL DEVICE WIDTH, no border, the frame centred in it
//     │               │
//     ├───────────────┤
//     │  [buttons...]  │  the cluster — `portrait2_rects` fills it, as in the skinned layout
//     └───────────────┘
//
// The point is WIDTH: with no bezel to inset by, the frame grows to the device width. The cluster's own
// arithmetic is unchanged (same 141.75X band), so both layouts share one hit-test.
inline Portrait2Skin portrait2_skin_bare(int deviceW, int deviceH) {
    // The cluster wants the FULL device width (X = deviceW/135), fixing its height at 141.75X. When that
    // leaves too little screen for a full-width 4:3 frame, the CLUSTER gives way — this layout exists to
    // make the tracker bigger.
    const float wantScreenH = static_cast<float>(deviceW) * DESIGN_FRAME_H / DESIGN_FRAME_W;

    float X = static_cast<float>(deviceW) / GRID_UNITS;
    if (static_cast<float>(deviceH) - X * 141.75f < wantScreenH)
        X = (static_cast<float>(deviceH) - wantScreenH) / 141.75f;
    X = std::max(X, 1.0f);   // never a zero-width cluster

    // The band height is derived from the FLOORED X, so cluster height and button sizes agree; capped
    // at the screen so odd inputs cannot push the cluster off the bottom.
    const int buttonAreaH = std::min(static_cast<int>(X * 141.75f), std::max(deviceH, 0));
    const int screenH     = deviceH - buttonAreaH;
    const int contentW    = static_cast<int>(X * GRID_UNITS);
    const int contentX    = (deviceW - contentW) / 2;   // centred

    Portrait2Skin s;
    s.x = X;

    // topPanel and branding stay EMPTY. `bezel` is the screen area itself (full width, no border, so the
    // inner area is the same rect) — `PortraitSkin::screen_rect` and the modal scrim read it in both
    // layouts.
    s.bezel      = {0, 0, deviceW, screenH};
    s.innerBezel = s.bezel;
    s.buttons    = {contentX, screenH, contentW, buttonAreaH};

    // The frame: the skinned layout's integer scale-and-centre, over the whole width.
    const int scaleX  = (deviceW + 1) / DESIGN_FRAME_W;
    const int scaleY  = (screenH + 1) / DESIGN_FRAME_H;
    const int scale   = std::max(std::min(scaleX, scaleY), 1);
    const int renderW = DESIGN_FRAME_W * scale;
    const int renderH = DESIGN_FRAME_H * scale;
    s.frame = {std::max(0, (deviceW - renderW) / 2), std::max(0, (screenH - renderH) / 2),
               renderW, renderH};

    return s;
}

/** Which button in `box` contains the box-local point. The oracle asserts the rects never overlap, so
 *  the first hit is the only one. */
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
