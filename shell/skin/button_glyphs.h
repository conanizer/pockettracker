// ─── shell/skin/button_glyphs.h — one virtual-button label renderer, shared by the touch layouts ───────
//
// A label in the shared 5×5 font: arrow glyphs for the D-pad, short words ("SEL", "STA", …) for the
// rest. Both the landscape gamepad (sdl-touch.cpp) and the PORTRAIT2 cluster (portrait2.cpp) draw
// it, so it lives here once. Inline: a few dozen `SDL_RenderFillRect` calls, no state.

#ifndef POCKETTRACKER_BUTTON_GLYPHS_H
#define POCKETTRACKER_BUTTON_GLYPHS_H

#include <SDL.h>

#include "ui/buttons.h"
#include "ui/font5x5.h"

#include <algorithm>
#include <cstdint>

namespace ptshell {

// One button's label as code points.
struct ButtonLabel {
    uint32_t cp[3];
    int      n;
};

inline ButtonLabel label_of(pt::ui::Button b) {
    using pt::ui::Button;
    switch (b) {
        case Button::DPAD_UP:    return {{pt::ui::CP_ARROW_UP}, 1};
        case Button::DPAD_DOWN:  return {{pt::ui::CP_ARROW_DOWN}, 1};
        case Button::DPAD_LEFT:  return {{pt::ui::CP_ARROW_LEFT}, 1};
        case Button::DPAD_RIGHT: return {{pt::ui::CP_ARROW_RIGHT}, 1};
        case Button::A:          return {{'A'}, 1};
        case Button::B:          return {{'B'}, 1};
        case Button::L_SHIFT:    return {{'L'}, 1};
        case Button::R_SHIFT:    return {{'R'}, 1};
        case Button::SELECT:     return {{'S', 'E', 'L'}, 3};
        case Button::START:      return {{'S', 'T', 'A'}, 3};
        default:                 return {{'?'}, 1};
    }
}

/**
 * Draw a button's label centred in `rc`, at the largest 5×5 scale that leaves a margin. One filled
 * rect per lit glyph pixel — a few dozen per label, and only on frames that actually present.
 *
 * `rgb` is the label colour, 0xRRGGBB — white by default, the label colour of the skins that use it.
 */
inline void draw_label(SDL_Renderer* r, pt::ui::Button b, const SDL_Rect& rc, uint32_t rgb = 0xFFFFFF) {
    const ButtonLabel lab = label_of(b);
    // The run is (6n − 1) px wide at scale 1: 5 px per glyph plus a 1 px gap, minus the trailing gap.
    const int denom = 6 * lab.n - 1;
    const int scale = std::max(1, std::min((rc.w * 7 / 10) / denom, (rc.h * 6 / 10) / 5));
    const int runW  = denom * scale;
    const int runH  = 5 * scale;
    int       x     = rc.x + (rc.w - runW) / 2;
    const int y     = rc.y + (rc.h - runH) / 2;

    SDL_SetRenderDrawColor(r, static_cast<Uint8>((rgb >> 16) & 0xFF),
                           static_cast<Uint8>((rgb >> 8) & 0xFF), static_cast<Uint8>(rgb & 0xFF), 255);
    for (int k = 0; k < lab.n; ++k) {
        const pt::ui::Glyph& g = pt::ui::glyph_for_codepoint(lab.cp[k]);
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (g[row] & (1 << (4 - col))) {
                    SDL_Rect px{x + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(r, &px);
                }
            }
        }
        x += 6 * scale;
    }
}

}  // namespace ptshell

#endif  // POCKETTRACKER_BUTTON_GLYPHS_H
