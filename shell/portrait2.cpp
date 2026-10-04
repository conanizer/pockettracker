#include "portrait2.h"

#include "button_glyphs.h"
#include "font.h"
#include "sdl-input.h"
#include "skin.h"

#include "ui/canvas.h"  // DESIGN_W / DESIGN_H — the 640×480 the FIT frame is fitted from

#include <algorithm>
#include <cmath>

namespace ptshell {

namespace tl = pt::ui::touch_layout;
using pt::ui::Button;

namespace {

SDL_Rect to_sdl(const tl::LayoutRect& lr) { return SDL_Rect{lr.x, lr.y, lr.w, lr.h}; }

// The button image: the shifts are WIDE, A/B the DARK square (falling back to the plain square when
// a theme ships no dark PNG), everything else the plain square, pressed or normal. The fallback is
// resolved here, against what actually loaded — `Skin::draw` can only no-op a missing piece.
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

// A PORTRAIT2 button's label: the text (or, for the D-pad, an `Arrow` — Helvetica has no arrow
// glyphs), its SIZE class (large = A/B and the arrows; small = Sel/Start and the shifts) and its X
// OFFSET (wide = the shifts). Y offset and the pressed shift are the same for all.
struct Portrait2Label {
    const char* text;   // the letters; "" when arrow
    bool        arrow;
    Arrow       dir;    // meaningful only when arrow
    bool        large;  // large font (A/B/arrows) vs small (Sel/Start/L/R shift)
    bool        wide;   // wide X offset (L/R shift) vs the square offset
};

Portrait2Label label_for(Button b) {
    switch (b) {
        case Button::L_SHIFT:    return {"L Shift", false, Arrow::Up,    false, true};
        case Button::R_SHIFT:    return {"R Shift", false, Arrow::Up,    false, true};
        case Button::A:          return {"A",       false, Arrow::Up,    true,  false};
        case Button::B:          return {"B",       false, Arrow::Up,    true,  false};
        case Button::SELECT:     return {"Sel",     false, Arrow::Up,    false, false};
        case Button::START:      return {"Start",   false, Arrow::Up,    false, false};
        case Button::DPAD_UP:    return {"",        true,  Arrow::Up,    true,  false};
        case Button::DPAD_DOWN:  return {"",        true,  Arrow::Down,  true,  false};
        case Button::DPAD_LEFT:  return {"",        true,  Arrow::Left,  true,  false};
        case Button::DPAD_RIGHT: return {"",        true,  Arrow::Right, true,  false};
        default:                 return {"?",       false, Arrow::Up,    true,  false};
    }
}

// The D-pad arrow codepoints (↑↓←→, U+2190–2193) as UTF-8, blitted from the bundled Linux Biolinum
// arrow font like any letter. Only reached when that font loaded.
const char* arrow_utf8(Arrow d) {
    switch (d) {
        case Arrow::Up:    return "\xE2\x86\x91";  // ↑ U+2191
        case Arrow::Down:  return "\xE2\x86\x93";  // ↓ U+2193
        case Arrow::Left:  return "\xE2\x86\x90";  // ← U+2190
        case Arrow::Right: return "\xE2\x86\x92";  // → U+2192
    }
    return "";
}

// The arrow GLYPH is drawn at a FRACTION of the letter px: Biolinum's arrow fills more of the em, so
// at letter size it reads ~1.7× too tall. BASELINE-anchored (see draw_buttons), so shrinking keeps
// its bottom where a full glyph's baseline is.
constexpr float ARROW_PX_FRAC = 0.6f;

// Sizing the FALLBACK line-arrow (used only when the arrow font is missing) so it reads at the same
// scale and height as the A/B letters beside it: the visible arrow is about a capital's height, and its
// box is centred on the letters' vertical mid-band.
constexpr float CAP_HEIGHT_FRAC = 0.72f;  // Helvetica cap height / em
constexpr float ARROW_BOX_FRAC  = 1.00f;  // arrow box / large-font px (the sprite carries its own margin)

}  // namespace

void PortraitSkin::layout(int outW, int outH, bool enabled, bool fit) {
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

    // The bands + the frame-in-bezel. density=1 and the dp fallback are inert for a skin with a
    // bezel PNG, so the output size alone decides the geometry. A CHROMELESS skin takes the BARE
    // layout (no bands: the screen spans the device width, the cluster hangs below) — same struct.
    geom_ = chromeless() ? tl::portrait2_skin_bare(outW, outH)
                         : tl::portrait2_skin(outW, outH, /*density=*/1.0f, /*bezelThicknessDp=*/9.0f,
                                              /*bezelThicknessX=*/bezelX_);

    // SETTINGS > SCALING: INTEGER uses the integer-scaled, centred `geom_.frame`; FIT fills the
    // bezel's inner area with the largest 4:3 fit (`min(innerW/640, innerH/480)`), centred. The
    // filtering that makes a fractional scale smooth follows `SdlVideo::set_scaling`.
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

    // The ten buttons inside the cluster band, with a 100 px floor on its height.
    buttons_ = tl::portrait2_rects(geom_.buttons.w, std::max(geom_.buttons.h, 100));
}

void PortraitSkin::draw_chrome(SDL_Renderer* r, const Skin& skin, uint32_t innerBezelArgb) const {
    if (!active_) return;

    // A chromeless skin has no bands, and its ground is the casing clear (already the theme
    // background) — nothing to draw.
    if (chromeless()) return;

    // Band 1 — the vent panel (absent in case C, so guard on empty()).
    if (!geom_.topPanel.empty()) skin.draw(r, SkinPiece::TopPanel, to_sdl(geom_.topPanel));

    // Band 2 — the bezel, then its padded inner area in the live tracker background (an argument, so
    // the theme editor tracks live), so the gap around the frame reads as one surface with it. BEFORE
    // the frame, which present_skinned draws after this underlay.
    skin.draw(r, SkinPiece::ScreenBezel, to_sdl(geom_.bezel));
    if (!geom_.innerBezel.empty()) {
        const SDL_Rect ib = to_sdl(geom_.innerBezel);
        SDL_SetRenderDrawColor(r, static_cast<Uint8>((innerBezelArgb >> 16) & 0xFF),
                               static_cast<Uint8>((innerBezelArgb >> 8) & 0xFF),
                               static_cast<Uint8>(innerBezelArgb & 0xFF), 255);
        SDL_RenderFillRect(r, &ib);
    }

    // Band 3 — the branding strip (full device width). Band 4 — the button backing; the buttons land on
    // top of it in draw_buttons, after the frame.
    skin.draw(r, SkinPiece::BrandingPanel, to_sdl(geom_.branding));
    skin.draw(r, SkinPiece::ButtonBacking, to_sdl(geom_.buttons));
}

void PortraitSkin::draw_buttons(SDL_Renderer* r, const Skin& skin, Font& font, Font& arrowFont,
                                const SdlInput& input) const {
    if (!active_) return;

    const int ox = geom_.buttons.x;
    const int oy = geom_.buttons.y;

    // The Helvetica label metrics, IN PIXELS: density cancels for on-screen size as it does for
    // positions (touch_layout.h), so `portrait2(..., density=1)`'s `large_sp` IS the pixel size.
    const bool          useHelv = font.loaded();
    const tl::Portrait2 fm = tl::portrait2(geom_.buttons.w, std::max(geom_.buttons.h, 100), 1.0f);
    const auto          R = [](float v) { return static_cast<int>(std::lround(v)); };

    // ONE colour for the shape and the character on it. The TRANSPARENT skin's art is a bare outline
    // authored in white, so it takes its colour from the same tint the label does; the chrome skins'
    // art is finished casing art and must not be multiplied by anything, which is the whole of the
    // difference below.
    const uint32_t ink   = ink_rgb();
    const bool     tinted = chromeless();

    for (int i = 0; i < buttons_.count; ++i) {
        const tl::ButtonRect& br = buttons_.r[i];
        const SDL_Rect        dst{br.x + ox, br.y + oy, br.w, br.h};
        const bool            pressed = input.is_held(br.button);
        // RenderCopy stretches the button PNG to the cell. A missing piece is a Skin::draw no-op, so
        // an incomplete theme shows the backing through.
        const SkinPiece piece = piece_for(skin, br.button, pressed);
        if (tinted) skin.draw_tinted(r, piece, dst, ink);
        else        skin.draw(r, piece, dst);

        // No Helvetica (asset missing / unparseable) → the shared 5×5 label font.
        if (!useHelv) {
            draw_label(r, br.button, dst, ink);
            continue;
        }

        const Portrait2Label lab  = label_for(br.button);
        const float          px   = lab.large ? fm.large_sp : fm.small_sp;
        const int            offX = R(lab.wide ? fm.wide_off_x_dp : fm.sq_off_x_dp);
        const int            offY = R(fm.off_y_dp) + (pressed ? R(fm.pressed_dp) : 0);

        if (lab.arrow) {
            // The D-pad: preferably the REAL arrow glyph from the bundled arrow font, at the letters'
            // left offset and top. Without that font, the shell-drawn line arrow (a capital's height
            // on the letters' baseline) rather than nothing.
            if (arrowFont.loaded()) {
                // Smaller than a letter (ARROW_PX_FRAC), BASELINE-anchored: the shrunk glyph's
                // baseline sits where a full-size glyph's would (dst.y+offY+ascent(px)).
                const float apx  = px * ARROW_PX_FRAC;
                const int   yTop = dst.y + offY + arrowFont.ascent_px(px) - arrowFont.ascent_px(apx);
                arrowFont.draw_text(arrow_utf8(lab.dir), dst.x + offX, yTop, apx, ink);
            } else {
                const int      baseline = dst.y + offY + font.ascent_px(px);
                const int      capH     = R(px * CAP_HEIGHT_FRAC);
                const int      side     = R(px * ARROW_BOX_FRAC);
                const SDL_Rect abox{dst.x + offX, baseline - capH / 2 - side / 2, side, side};
                font.draw_arrow(lab.dir, abox, ink);
            }
        } else {
            // Top-start plus the start/top offset.
            font.draw_text(lab.text, dst.x + offX, dst.y + offY, px, ink);
        }
    }
}

uint64_t PortraitSkin::signature(const SdlInput& input) const {
    if (!active_) return 0;

    uint64_t bits = 0;
    for (int i = 0; i < buttons_.count; ++i)
        if (input.is_held(buttons_.r[i].button))
            bits |= (1ull << static_cast<int>(buttons_.r[i].button));

    // ⚠️ A CHROMELESS skin's INK colour belongs in here: it is applied at blit time, so editing TXT
    // VALUE changes neither a canvas pixel nor the casing — the two things the pixel gate compares —
    // and the buttons would keep the old colour until something else forced a frame.
    // Bits 10-15 (button flags end at 9, geometry starts at 16): a 6-bit FOLD, not the colour — a
    // net under the canvas compare, not an exact channel.
    if (chromeless()) {
        const uint32_t ink = ink_rgb();
        const uint32_t f   = (ink ^ (ink >> 12)) ^ ((ink ^ (ink >> 12)) >> 6);
        bits ^= static_cast<uint64_t>(f & 0x3Fu) << 10;
    }

    // Geometry too, so a rotate/resize forces a repaint with the same buttons held. Bit 62 marks
    // "portrait active" (SdlTouch uses 63), so a landscape↔portrait switch always changes the value.
    return bits | (static_cast<uint64_t>(outW_ & 0xFFFF) << 16) |
           (static_cast<uint64_t>(outH_ & 0xFFFF) << 32) | (1ull << 62);
}

}  // namespace ptshell
