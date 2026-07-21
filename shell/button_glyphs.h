// ─── shell/button_glyphs.h — one virtual-button label renderer, shared by the touch layouts ───────
//
// A virtual button's label is drawn with the shared 5×5 font: the D-pad gets the arrow glyphs
// `font5x5.h` already carries (the same '↑' '←' '→' '↓' the Kotlin buttons show), the rest are the
// short Kotlin labels ("SEL", "STA", …). Both the LANDSCAPE on-screen gamepad (`sdl-touch.cpp`) and
// the PORTRAIT2 skinned cluster (`portrait2.cpp`) draw exactly this, so it lives here rather than in
// either — a label routine copied into a second file is the drift the whole convergence exists to
// kill. Inline in a header for the same reason `font5x5.h` is: it is a few dozen `SDL_RenderFillRect`
// calls over the glyph bitmap, with no state to own.

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
 * `rgb` is the label colour, 0xRRGGBB. White by default: it is the label colour of BOTH amiga skins
 * (the classic's is dark, but the classic ships no PORTRAIT2 device art anyway), and a parameter so a
 * future light theme can pass its own without this routine caring which layout called it.
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
