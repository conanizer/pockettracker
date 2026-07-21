#include "portrait2.h"

#include "button_glyphs.h"
#include "sdl-input.h"
#include "skin.h"

#include <algorithm>

namespace ptshell {

namespace tl = pt::ui::touch_layout;
using pt::ui::Button;

namespace {

SDL_Rect to_sdl(const tl::LayoutRect& lr) { return SDL_Rect{lr.x, lr.y, lr.w, lr.h}; }

// `VirtualBtnThemed`'s image selection, ported: the shifts are WIDE, A/B use the DARK square variant —
// falling back to the plain square when a theme ships no dark PNG, exactly as Kotlin's
// `buttonSquarePressedDark ?: buttonSquarePressed` does — and everything else is the plain square, each
// in its pressed or normal state. The fallback is resolved HERE (against what actually loaded) rather
// than in `Skin::draw`, which only knows how to no-op a missing piece, not which piece to try instead.
SkinPiece piece_for(const Skin& skin, Button b, bool pressed) {
    switch (b) {
        case Button::L_SHIFT:
        case Button::R_SHIFT:
            return pressed ? SkinPiece::BtnWidePressed : SkinPiece::BtnWideNormal;
        case Button::A:
        case Button::B: {
            const SkinPiece dark =
                pressed ? SkinPiece::BtnSquarePressedDark : SkinPiece::BtnSquareNormalDark;
            if (skin.piece(dark)) return dark;
            return pressed ? SkinPiece::BtnSquarePressed : SkinPiece::BtnSquareNormal;
        }
        default:
            return pressed ? SkinPiece::BtnSquarePressed : SkinPiece::BtnSquareNormal;
    }
}

}  // namespace

void PortraitSkin::layout(int outW, int outH, bool enabled) {
    outW_ = outW;
    outH_ = outH;

    // PORTRAIT = the output is taller than it is wide. On a phone that is the physical orientation; on a
    // resizable desktop window it is dragging the window tall. A landscape or square output stays on the
    // plain centred present path (active_ == false) — every handheld, and the desktop's default shape.
    active_ = enabled && outW > 0 && outH > 0 && outH > outW;
    if (!active_) {
        buttons_.count = 0;
        frame_         = SDL_Rect{0, 0, 0, 0};
        return;
    }

    // The bands + the frame-in-bezel, host-checked by `pttouch --positions`. density=1 and the dp
    // fallback are inert for amiga-2 (its bezelThicknessX > 0), so the only inputs that decide the
    // geometry are the output size and the skin unit X the function derives from it.
    geom_  = tl::portrait2_skin(outW, outH, /*density=*/1.0f, /*bezelThicknessDp=*/9.0f,
                                /*bezelThicknessX=*/BEZEL_THICK_X);
    frame_ = to_sdl(geom_.frame);

    // The ten buttons inside the cluster band. `portrait2_rects` re-derives X from the band it is given,
    // and Kotlin hands it `buttonAreaH.coerceAtLeast(100)` — so that floor is restated here, not assumed.
    buttons_ = tl::portrait2_rects(geom_.buttons.w, std::max(geom_.buttons.h, 100));
}

void PortraitSkin::draw_chrome(SDL_Renderer* r, const Skin& skin) const {
    if (!active_) return;

    // Band 1 — the vent panel (absent in case C, so guard on empty()).
    if (!geom_.topPanel.empty()) skin.draw(r, SkinPiece::TopPanel, to_sdl(geom_.topPanel));

    // Band 2 — the bezel, then its padded inner area painted BLACK (ScreenLayouts.kt:481). The frame
    // integer-scales inside that area and its own black canvas background hides any letterbox gap, so
    // the black must go down BEFORE the frame — which present_skinned draws AFTER this underlay.
    skin.draw(r, SkinPiece::ScreenBezel, to_sdl(geom_.bezel));
    if (!geom_.innerBezel.empty()) {
        const SDL_Rect ib = to_sdl(geom_.innerBezel);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderFillRect(r, &ib);
    }

    // Band 3 — the branding strip (full device width). Band 4 — the button backing; the buttons land on
    // top of it in draw_buttons, after the frame.
    skin.draw(r, SkinPiece::BrandingPanel, to_sdl(geom_.branding));
    skin.draw(r, SkinPiece::ButtonBacking, to_sdl(geom_.buttons));
}

void PortraitSkin::draw_buttons(SDL_Renderer* r, const Skin& skin, const SdlInput& input) const {
    if (!active_) return;

    const int ox = geom_.buttons.x;
    const int oy = geom_.buttons.y;
    for (int i = 0; i < buttons_.count; ++i) {
        const tl::ButtonRect& br = buttons_.r[i];
        const SDL_Rect        dst{br.x + ox, br.y + oy, br.w, br.h};
        const bool            pressed = input.is_held(br.button);
        // FillBounds: RenderCopy stretches the button PNG to the cell — Compose's ContentScale.FillBounds.
        // A missing piece is a Skin::draw no-op, so an incomplete theme shows the backing through.
        skin.draw(r, piece_for(skin, br.button, pressed), dst);
        draw_label(r, br.button, dst, LABEL_RGB);
    }
}

uint64_t PortraitSkin::signature(const SdlInput& input) const {
    if (!active_) return 0;

    uint64_t bits = 0;
    for (int i = 0; i < buttons_.count; ++i)
        if (input.is_held(buttons_.r[i].button))
            bits |= (1ull << static_cast<int>(buttons_.r[i].button));

    // Geometry too, so a rotate/resize that moves the skin forces a repaint even with the same buttons
    // held. Bit 62 marks "portrait active" — distinct from SdlTouch's bit 63 — so a landscape↔portrait
    // switch always changes the value the C7 gate compares, and the value is never 0 while active.
    return bits | (static_cast<uint64_t>(outW_ & 0xFFFF) << 16) |
           (static_cast<uint64_t>(outH_ & 0xFFFF) << 32) | (1ull << 62);
}

}  // namespace ptshell
