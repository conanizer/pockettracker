#pragma once

// The on-screen touch layouts (LEFT, RIGHT, the PORTRAIT split, the 135-unit PORTRAIT2 grid): button
// sizes and spacing from a box size and density, and where each button lands. Drawing is the shell's.
// Sizes are pinned by a golden; positions (`*_rects`) by a hand-written oracle and a device.
//
// ⚠️ Bit-exact float: the golden stores binary32 bits. Keep literals `f`-suffixed and the operation
// order as written; the test builds without fma contraction. `pattern_unit`'s tie takes the height arm.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "buttons.h"  // pt::ui::Button, for the POSITIONS rects — a leaf (<cstdint>)

namespace pt::ui::touch_layout {

// Each side box is 3.4 × 5.1 units; PORTRAIT2's grid is 135 units on its shorter axis.
inline constexpr float PATTERN_WIDTH  = 3.4f;
inline constexpr float PATTERN_HEIGHT = 5.1f;
inline constexpr float GRID_UNITS     = 135.0f;

// canvas.h's DESIGN_W/H, restated to keep this header a leaf.
inline constexpr int DESIGN_FRAME_W = 640;
inline constexpr int DESIGN_FRAME_H = 480;

// The unit X: the largest whole pixel size at which 3.4×5.1 fits the box. Shared by LEFT, RIGHT, PORTRAIT.
inline int pattern_unit(int available_width, int available_height) {
    const float box_ratio     = static_cast<float>(available_width) / static_cast<float>(available_height);
    const float pattern_ratio = PATTERN_WIDTH / PATTERN_HEIGHT;
    const float v = (box_ratio < pattern_ratio)
                        ? std::floor(static_cast<float>(available_width) / PATTERN_WIDTH)
                        : std::floor(static_cast<float>(available_height) / PATTERN_HEIGHT);
    return static_cast<int>(v);
}

// LEFT box: L, D-pad, SELECT.
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

// RIGHT box: R, A, B, START. Not a mirror of LEFT (its own spacers).
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

// The two-box PORTRAIT split: each box gets half the width, handed down to `left()`/`right()`.
struct Portrait {
    int x;
    int box_width;
    int box_height;
};

// PORTRAIT2's 135-unit grid. `x` stays a float (flooring would leave edge gaps), at least 1.0.
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
    m.medium_spacer   = static_cast<int>(std::floor(x * 0.7f));   // LEFT's is 1.0
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

// ─── Positions: where each button lands, in box-local pixels ─────────────────────────────────────
// Density cancels out of positions (it only sizes fonts). A column, vertically centred; each row
// centred, its children packed left to right.

struct ButtonRect {
    Button button;
    int    x, y, w, h;
    int  cx() const { return x + w / 2; }
    int  cy() const { return y + h / 2; }
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// A laid-out box, in draw order (LEFT fills 6, RIGHT 4, PORTRAIT2 10).
struct BoxRects {
    ButtonRect r[10];
    int        count = 0;
};

// LEFT: L on top, the D-pad cross, SELECT below.
inline BoxRects left_rects(int available_width, int available_height) {
    const Left m = left(available_width, available_height, 1.0f);
    const int  W = available_width;

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

// RIGHT: R on top, A upper-right and B lower-left, START below.
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

// ─── PORTRAIT2: four rows, inset by round(1.5X), each round(33X) tall, split into equal slots ─────
//
//   Row 0:  [ L Shift ][ R Shift ]     (2 wide slots)
//   Row 1:  [    ][ ↑ ][ B ][ A ]      (4 slots; slot 0 empty)
//   Row 2:  [ ← ][ ↓ ][ → ][    ]      (4 slots; slot 3 empty)
//   Row 3:  [    ][Sel][Sta][    ]     (4 slots; slots 0 and 3 empty)

// Split `W` into `n` slots of round(W/n), handing the leftover (maybe negative) out one per slot from
// the left, so the widths sum to exactly `W`.
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

// All ten buttons, row by row. An empty slot gets no rect, so a tap there hits nothing.
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

// ─── PORTRAIT2's device skin: four bands on a 135X × 300X screen, top to bottom ──────────────────
//   1 top vent panel (may be 0) · 2 screen bezel, the frame inside · 3 branding, full device width ·
//   4 the button cluster.
// ⚠️ The frame sits in the bezel, not centred in the window — high on a tall screen.

// Device pixels; `empty()` marks an absent band.
struct LayoutRect {
    int  x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

// The 640×480 frame at the largest integer scale that fits `area` (at least 1×), centred in it.
inline LayoutRect integer_fit(const LayoutRect& area) {
    const int scale = std::max(std::min((area.w + 1) / DESIGN_FRAME_W,    // `+1`: a rounding allowance
                                        (area.h + 1) / DESIGN_FRAME_H), 1);
    const int w = DESIGN_FRAME_W * scale, h = DESIGN_FRAME_H * scale;
    return {area.x + std::max(0, (area.w - w) / 2), area.y + std::max(0, (area.h - h) / 2), w, h};
}

struct Portrait2Skin {
    LayoutRect topPanel;    // band 1 (empty in case C)
    LayoutRect bezel;       // band 2
    LayoutRect branding;    // band 3, full device width
    LayoutRect buttons;     // band 4 — what `portrait2_rects` fills
    LayoutRect frame;       // the 640×480 frame, integer-scaled, centred in the bezel's inner area
    LayoutRect innerBezel;  // the bezel's padded inner area (a FIT-mode frame fits here)
    float      x = 0.0f;    // the skin unit (135X = skin width)
};

// The bezel border is `bezelThicknessX` skin units if > 0 (a bezel image), else
// `bezelThicknessDp * density` (a solid bezel). Three cases: A the skin fits; B the top panel shrinks;
// C height-bound, the skin narrower than the device.
inline Portrait2Skin portrait2_skin(int deviceW, int deviceH, float density,
                                    float bezelThicknessDp, float bezelThicknessX) {
    const float xFromWidth = static_cast<float>(deviceW) / 135.0f;
    // Branding spans the device width, so it follows xFromWidth, not X.
    const int brandingH = static_cast<int>(xFromWidth * 22.5f);

    float X;
    int   topPanelH;
    if (xFromWidth * 300.0f <= static_cast<float>(deviceH)) {          // A
        X         = xFromWidth;
        topPanelH = std::min(static_cast<int>(deviceH - X * 267.0f), static_cast<int>(X * 33.0f));
    } else if (xFromWidth * 267.0f <= static_cast<float>(deviceH)) {   // B
        X         = xFromWidth;
        topPanelH = std::max(static_cast<int>(deviceH - X * 267.0f), 0);
    } else {                                                           // C
        topPanelH = 0;
        X         = static_cast<float>(deviceH - brandingH) / (102.75f + 141.75f);
    }

    const int contentW    = static_cast<int>(X * 135.0f);
    const int bezelH      = static_cast<int>(X * 102.75f);
    const int buttonAreaH = static_cast<int>(X * 141.75f);
    const int contentX    = (deviceW - contentW) / 2;

    Portrait2Skin s;
    s.x = X;

    int y = 0;
    if (topPanelH > 0) s.topPanel = {contentX, y, contentW, topPanelH};
    y += topPanelH;
    s.bezel    = {contentX, y, contentW, bezelH};
    y += bezelH;
    s.branding = {0, y, deviceW, brandingH};
    y += brandingH;
    s.buttons  = {contentX, y, contentW, buttonAreaH};

    const float bezelThickPx = (bezelThicknessX > 0.0f) ? (X * bezelThicknessX)
                                                        : (bezelThicknessDp * density);
    const int   innerX = contentX + static_cast<int>(bezelThickPx);
    const int   innerY = topPanelH + static_cast<int>(bezelThickPx);
    const int   innerW = static_cast<int>(static_cast<float>(contentW) - 2.0f * bezelThickPx);
    const int   innerH = static_cast<int>(static_cast<float>(bezelH)   - 2.0f * bezelThickPx);
    s.innerBezel = {innerX, innerY, innerW, innerH};
    s.frame      = integer_fit(s.innerBezel);
    return s;
}

// ─── PORTRAIT2 bare: button art only — the screen takes the full width above the same cluster ────
inline Portrait2Skin portrait2_skin_bare(int deviceW, int deviceH) {
    // The cluster wants the full width; when that leaves too little screen for a full-width 4:3
    // frame, the cluster shrinks instead.
    const float wantScreenH = static_cast<float>(deviceW) * DESIGN_FRAME_H / DESIGN_FRAME_W;

    float X = static_cast<float>(deviceW) / GRID_UNITS;
    if (static_cast<float>(deviceH) - X * 141.75f < wantScreenH)
        X = (static_cast<float>(deviceH) - wantScreenH) / 141.75f;
    X = std::max(X, 1.0f);   // never a zero-width cluster

    // Capped at the screen, so odd inputs cannot push the cluster off the bottom.
    const int buttonAreaH = std::min(static_cast<int>(X * 141.75f), std::max(deviceH, 0));
    const int screenH     = deviceH - buttonAreaH;
    const int contentW    = static_cast<int>(X * GRID_UNITS);
    const int contentX    = (deviceW - contentW) / 2;

    Portrait2Skin s;
    s.x = X;

    // No top panel or branding; `bezel` is the screen area itself (the shell reads it in both layouts).
    s.bezel      = {0, 0, deviceW, screenH};
    s.innerBezel = s.bezel;
    s.buttons    = {contentX, screenH, contentW, buttonAreaH};
    s.frame      = integer_fit(s.bezel);
    return s;
}

// The button under a box-local point (the rects never overlap).
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
