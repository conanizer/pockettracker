#include "skin/portrait2.h"

#include "skin/button_glyphs.h"
#include "skin/font.h"
#include "sdl/sdl-input.h"
#include "skin/skin.h"

#include "ui/canvas.h"  // DESIGN_W / DESIGN_H

#include <algorithm>
#include <cmath>

namespace ptshell {

namespace tl = pt::ui::touch_layout;
using pt::ui::Button;

namespace {

SDL_Rect to_sdl(const tl::LayoutRect& lr) { return SDL_Rect{lr.x, lr.y, lr.w, lr.h}; }

// Shifts are wide, A/B the dark square (or the plain one if the theme has no dark PNG — resolved
// against what loaded), the rest the plain square.
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

// A button's label — a word or a D-pad arrow — large (A/B, arrows) or small, and the wide (shift) or
// square X offset.
struct Portrait2Label {
    const char* text;
    bool        large;
    bool        wide;
};

Portrait2Label label_for(Button b) {
    switch (b) {
        case Button::L_SHIFT:    return {"L Shift",      false, true};
        case Button::R_SHIFT:    return {"R Shift",      false, true};
        case Button::A:          return {"A",            true,  false};
        case Button::B:          return {"B",            true,  false};
        case Button::SELECT:     return {"Sel",          false, false};
        case Button::START:      return {"Start",        false, false};
        case Button::DPAD_UP:    return {"\xE2\x86\x91", true,  false};  // ↑ U+2191
        case Button::DPAD_DOWN:  return {"\xE2\x86\x93", true,  false};  // ↓ U+2193
        case Button::DPAD_LEFT:  return {"\xE2\x86\x90", true,  false};  // ← U+2190
        case Button::DPAD_RIGHT: return {"\xE2\x86\x92", true,  false};  // → U+2192
        default:                 return {"?",            true,  false};
    }
}

}  // namespace

void PortraitSkin::layout(int outW, int outH, bool enabled, bool fit) {
    outW_ = outW;
    outH_ = outH;

    // Portrait = taller than wide (a phone upright, or a desktop window dragged tall).
    active_ = enabled && outW > 0 && outH > 0 && outH > outW;
    if (!active_) {
        buttons_.count = 0;
        frame_         = SDL_Rect{0, 0, 0, 0};
        return;
    }

    // The output size alone decides the geometry (density and the dp fallback are inert with a bezel
    // PNG). Chromeless takes the bare layout: no bands, the cluster below a full-width screen.
    geom_ = chromeless() ? tl::portrait2_skin_bare(outW, outH)
                         : tl::portrait2_skin(outW, outH, /*density=*/1.0f, /*bezelThicknessDp=*/9.0f,
                                              /*bezelThicknessX=*/bezelX_);

    // FIT: the largest 4:3 fit inside the bezel, centred; INTEGER: `geom_.frame`.
    if (fit && !geom_.innerBezel.empty()) {
        const tl::LayoutRect ib = geom_.innerBezel;
        const float s = std::min(static_cast<float>(ib.w) / pt::ui::DESIGN_W,
                                 static_cast<float>(ib.h) / pt::ui::DESIGN_H);
        const int   w = static_cast<int>(pt::ui::DESIGN_W * s);
        const int   h = static_cast<int>(pt::ui::DESIGN_H * s);
        frame_ = SDL_Rect{ib.x + (ib.w - w) / 2, ib.y + (ib.h - h) / 2, w, h};
    } else {
        frame_ = to_sdl(geom_.frame);
    }

    // The ten buttons in the cluster band, at least 100 px tall.
    buttons_ = tl::portrait2_rects(geom_.buttons.w, std::max(geom_.buttons.h, 100));
}

void PortraitSkin::draw_chrome(SDL_Renderer* r, const Skin& skin, uint32_t innerBezelArgb) const {
    if (!active_) return;

    // Chromeless: no bands, and the casing clear is already its ground.
    if (chromeless()) return;

    // Band 1 — the vent panel (absent in case C).
    if (!geom_.topPanel.empty()) skin.draw(r, SkinPiece::TopPanel, to_sdl(geom_.topPanel));

    // Band 2 — the bezel, its inner area filled with the live tracker background.
    skin.draw(r, SkinPiece::ScreenBezel, to_sdl(geom_.bezel));
    if (!geom_.innerBezel.empty()) {
        const SDL_Rect ib = to_sdl(geom_.innerBezel);
        SDL_SetRenderDrawColor(r, static_cast<Uint8>((innerBezelArgb >> 16) & 0xFF),
                               static_cast<Uint8>((innerBezelArgb >> 8) & 0xFF),
                               static_cast<Uint8>(innerBezelArgb & 0xFF), 255);
        SDL_RenderFillRect(r, &ib);
    }

    // Band 3 — branding (full width); band 4 — the button backing.
    skin.draw(r, SkinPiece::BrandingPanel, to_sdl(geom_.branding));
    skin.draw(r, SkinPiece::ButtonBacking, to_sdl(geom_.buttons));
}

void PortraitSkin::draw_buttons(SDL_Renderer* r, const Skin& skin, Font& font,
                                const SdlInput& input) const {
    if (!active_) return;

    const int ox = geom_.buttons.x;
    const int oy = geom_.buttons.y;

    // In pixels: density cancels, so `portrait2(…, 1)`'s `large_sp` is the pixel size.
    const bool          useLabelFont = font.loaded();
    const tl::Portrait2 fm = tl::portrait2(geom_.buttons.w, std::max(geom_.buttons.h, 100), 1.0f);
    const auto          R = [](float v) { return static_cast<int>(std::lround(v)); };

    // One colour for a button and its character. The transparent art is white and takes the tint;
    // chrome art is finished and must not be multiplied.
    const uint32_t ink   = ink_rgb();
    const bool     tinted = chromeless();

    for (int i = 0; i < buttons_.count; ++i) {
        const tl::ButtonRect& br = buttons_.r[i];
        const SDL_Rect        dst{br.x + ox, br.y + oy, br.w, br.h};
        const bool            pressed = input.is_held(br.button);
        // Stretched to the cell; a missing piece shows the backing through.
        const SkinPiece piece = piece_for(skin, br.button, pressed);
        if (tinted) skin.draw_tinted(r, piece, dst, ink);
        else        skin.draw(r, piece, dst);

        // No label font → the shared 5×5 font.
        if (!useLabelFont) {
            draw_label(r, br.button, dst, ink);
            continue;
        }

        const Portrait2Label lab  = label_for(br.button);
        const float          px   = lab.large ? fm.large_sp : fm.small_sp;
        const int            offX = R(lab.wide ? fm.wide_off_x_dp : fm.sq_off_x_dp);
        const int            offY = R(fm.off_y_dp) + (pressed ? R(fm.pressed_dp) : 0);

        font.draw_text(lab.text, dst.x + offX, dst.y + offY, px, ink);
    }
}

uint64_t PortraitSkin::signature(const SdlInput& input) const {
    if (!active_) return 0;

    uint64_t bits = 0;
    for (int i = 0; i < buttons_.count; ++i)
        if (input.is_held(buttons_.r[i].button))
            bits |= (1ull << static_cast<int>(buttons_.r[i].button));

    // ⚠️ A chromeless skin's ink is applied at blit time, so a TXT VALUE edit changes nothing the pixel
    // gate compares — fold it into bits 10–15 (button flags end at 9, geometry starts at 16).
    if (chromeless()) {
        const uint32_t ink = ink_rgb();
        const uint32_t f   = (ink ^ (ink >> 12)) ^ ((ink ^ (ink >> 12)) >> 6);
        bits ^= static_cast<uint64_t>(f & 0x3Fu) << 10;
    }

    // Geometry too; bit 62 marks portrait (SdlTouch uses 63), so a mode switch always changes it.
    return bits | (static_cast<uint64_t>(outW_ & 0xFFFF) << 16) |
           (static_cast<uint64_t>(outH_ & 0xFFFF) << 32) | (1ull << 62);
}

}  // namespace ptshell
