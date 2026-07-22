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

// A PORTRAIT2 button's label, ported one-for-one from `VirtualControlsPortrait2`'s per-button call: the
// text (or, for the D-pad, an `Arrow` the shell draws itself — Helvetica has no arrow glyphs), which
// SIZE class it uses (large = A/B and the arrows; small = Sel/Start and the L/R shift), and which X
// OFFSET (wide = the two shift buttons; square for the rest). Y offset and the pressed shift are the
// same for all, so they are read from the metrics at the call site rather than repeated here.
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

// The D-pad arrow codepoints (↑↓←→, U+2190–2193) as UTF-8, for the arrow FONT path — Kotlin drew these
// exact characters as Text through the system fallback; the shell blits the real glyph from the bundled
// Linux Biolinum arrow font, same as a letter. Only reached when the arrow font loaded.
const char* arrow_utf8(Arrow d) {
    switch (d) {
        case Arrow::Up:    return "\xE2\x86\x91";  // ↑ U+2191
        case Arrow::Down:  return "\xE2\x86\x93";  // ↓ U+2193
        case Arrow::Left:  return "\xE2\x86\x90";  // ← U+2190
        case Arrow::Right: return "\xE2\x86\x92";  // → U+2192
    }
    return "";
}

// The arrow GLYPH is rendered at a FRACTION of the letter px. Measured against the Kotlin app's arrows
// (before/after device shots): Biolinum's arrow glyph fills more of the em than Android's system-fallback
// arrow did, so at the letter size it came out ~1.7x too tall. 0.6 matches the "before" size (ink height
// ratio 27/46 ≈ 0.59), and it is BASELINE-anchored (see draw_buttons) so shrinking keeps the arrow's
// bottom where a full glyph's baseline is — the before/after shots share that bottom edge exactly.
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

    // The bands + the frame-in-bezel, host-checked by `pttouch --positions`. density=1 and the dp
    // fallback are inert for amiga-2 (its bezelThicknessX > 0), so the only inputs that decide the
    // geometry are the output size and the skin unit X the function derives from it.
    geom_  = tl::portrait2_skin(outW, outH, /*density=*/1.0f, /*bezelThicknessDp=*/9.0f,
                                /*bezelThicknessX=*/bezelX_);

    // SETTINGS > SCALING decides where the 640×480 frame lands inside the bezel. INTEGER uses the
    // pre-computed integer-scaled, centred `geom_.frame` (the tracker's own black canvas hides the gap).
    // FIT fills the bezel's inner area with the largest 4:3 fit — the SAME fractional scale Kotlin's
    // PortraitLayout2 BILINEAR arm uses (`min(innerW/640, innerH/480)`), centred — so the picture grows
    // to nearly fill the bezel instead of snapping to a whole multiple. The texture filtering that makes
    // a fractional scale smooth rather than uneven follows `SdlVideo::set_scaling`, which runs on the
    // same `scalingBilinear` value later in the frame; this only chooses the destination rect.
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

    // The ten buttons inside the cluster band. `portrait2_rects` re-derives X from the band it is given,
    // and Kotlin hands it `buttonAreaH.coerceAtLeast(100)` — so that floor is restated here, not assumed.
    buttons_ = tl::portrait2_rects(geom_.buttons.w, std::max(geom_.buttons.h, 100));
}

void PortraitSkin::draw_chrome(SDL_Renderer* r, const Skin& skin, uint32_t innerBezelArgb) const {
    if (!active_) return;

    // Band 1 — the vent panel (absent in case C, so guard on empty()).
    if (!geom_.topPanel.empty()) skin.draw(r, SkinPiece::TopPanel, to_sdl(geom_.topPanel));

    // Band 2 — the bezel, then its padded inner area painted the TRACKER's own background colour. Kotlin
    // (ScreenLayouts.kt:481) painted this black; the shell fills it with the live pt-ui theme background
    // instead, so the letterbox gap around the frame matches the tracker's own module fill and the whole
    // bezel reads as one surface — the same colour and the same reasoning as the landscape letterbox
    // (SdlVideo::present). It must go down BEFORE the frame, which present_skinned draws after this
    // underlay. The colour arrives as an argument (not stored) so the theme editor tracks live.
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

    // The Helvetica label metrics, IN PIXELS. Density cancels for on-screen size exactly as it does for
    // positions (touch_layout.h): Kotlin draws `largeSp.sp` at `largeSp * density` px, and
    // largeSp = x*11/density, so the pixel size is x*11 — which is what `portrait2(..., density=1)`
    // returns as `large_sp`. So the SAME golden-checked Portrait2 fields give the on-device px with no
    // density threaded through the shell. Same coerced button-cluster box `portrait2_rects` used.
    const bool          useHelv = font.loaded();
    const tl::Portrait2 fm = tl::portrait2(geom_.buttons.w, std::max(geom_.buttons.h, 100), 1.0f);
    const auto          R = [](float v) { return static_cast<int>(std::lround(v)); };

    for (int i = 0; i < buttons_.count; ++i) {
        const tl::ButtonRect& br = buttons_.r[i];
        const SDL_Rect        dst{br.x + ox, br.y + oy, br.w, br.h};
        const bool            pressed = input.is_held(br.button);
        // FillBounds: RenderCopy stretches the button PNG to the cell — Compose's ContentScale.FillBounds.
        // A missing piece is a Skin::draw no-op, so an incomplete theme shows the backing through.
        skin.draw(r, piece_for(skin, br.button, pressed), dst);

        // No Helvetica (asset missing / unparseable) → the shared 5×5 label font, as before it existed.
        if (!useHelv) {
            draw_label(r, br.button, dst, labelRgb_);
            continue;
        }

        const Portrait2Label lab  = label_for(br.button);
        const float          px   = lab.large ? fm.large_sp : fm.small_sp;
        const int            offX = R(lab.wide ? fm.wide_off_x_dp : fm.sq_off_x_dp);
        const int            offY = R(fm.off_y_dp) + (pressed ? R(fm.pressed_dp) : 0);

        if (lab.arrow) {
            // The D-pad. Preferred: the REAL arrow glyph from the bundled arrow font (Linux Biolinum),
            // blitted as text at the SAME left offset and top the letters use — so ↑↓←→ read like the A/B
            // letters beside them, exactly as Kotlin drew them (Text through the system fallback). If the
            // arrow font did not load, fall back to the shell-drawn smooth line arrow (a box a capital's
            // height on the letters' baseline), so a missing font degrades to lines rather than nothing.
            if (arrowFont.loaded()) {
                // Smaller than a letter (ARROW_PX_FRAC), BASELINE-anchored: place the shrunk glyph so its
                // baseline is exactly where a full-size glyph's would be (dst.y+offY+ascent(px)), which
                // keeps the arrow's BOTTOM fixed as it shrinks — matching the Kotlin "before" arrows,
                // which share that bottom edge with the (larger) full-size render.
                const float apx  = px * ARROW_PX_FRAC;
                const int   yTop = dst.y + offY + arrowFont.ascent_px(px) - arrowFont.ascent_px(apx);
                arrowFont.draw_text(arrow_utf8(lab.dir), dst.x + offX, yTop, apx, labelRgb_);
            } else {
                const int      baseline = dst.y + offY + font.ascent_px(px);
                const int      capH     = R(px * CAP_HEIGHT_FRAC);
                const int      side     = R(px * ARROW_BOX_FRAC);
                const SDL_Rect abox{dst.x + offX, baseline - capH / 2 - side / 2, side, side};
                font.draw_arrow(lab.dir, abox, labelRgb_);
            }
        } else {
            // Top-start + offset, like Kotlin's `Text` with `Alignment.TopStart` and a start/top padding.
            font.draw_text(lab.text, dst.x + offX, dst.y + offY, px, labelRgb_);
        }
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
