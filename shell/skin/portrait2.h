// The portrait device skin's renderer: on a phone held upright, the 640×480 tracker sits above a
// button cluster. `touch_layout` computes the rects; this composites them.
//
//   Chrome skin                                Chromeless (TRNS) skin
//   ┌───────────────┐ top vent (may be absent)  ┌───────────────┐
//   │  ┌─────────┐  │ bezel, frame inside       │    640×480    │ full width, no bezel
//   │  │ 640×480 │  │                           ├───────────────┤
//   ├──┴─────────┴──┤ branding strip            │  [buttons...]  │ tinted to the live theme
//   │  [buttons...]  │ button cluster            └───────────────┘
//   └───────────────┘
//
// The cluster geometry is shared by both, and `cluster_rect()` / `button_rects()` are what
// `SdlTouch::layout_portrait2` hit-tests — so a press always lights the button under the finger.

#ifndef POCKETTRACKER_PORTRAIT2_H
#define POCKETTRACKER_PORTRAIT2_H

#include <SDL.h>

#include "skin/skin.h"   // SkinArt, stored by value
#include "ui/touch_layout.h"

#include <cstdint>

class SdlInput;

namespace ptshell {

class Font;

class PortraitSkin {
public:
    // Recompute the geometry for the output size; cheap, called each frame before present, so a
    // rotation is absorbed next frame. `fit` (SETTINGS > SCALING) fills the bezel with the largest 4:3
    // fit instead of an integer scale; it moves only `frame_rect()`.
    void layout(int outW, int outH, bool enabled, bool fit);

    // Present the skin instead of the centred frame: a touchscreen in portrait. Derived from the output
    // aspect each frame, never stored.
    bool active() const { return active_; }

    // Where the tracker texture blits: inside band 2's bezel.
    SDL_Rect frame_rect() const { return frame_; }

    // The bezel's glass: `draw_chrome` fills it, `frame_rect()` sits inside, and a modal dims it (the
    // gap around an integer-scaled frame too) but never the casing. No bezel → the frame rect.
    SDL_Rect screen_rect() const {
        return geom_.innerBezel.empty()
                   ? frame_
                   : SDL_Rect{geom_.innerBezel.x, geom_.innerBezel.y, geom_.innerBezel.w,
                              geom_.innerBezel.h};
    }

    // The cluster band in output pixels and the ten button rects local to it — what `draw_buttons`
    // uses and `SdlTouch::layout_portrait2` hit-tests.
    SDL_Rect cluster_rect() const {
        return SDL_Rect{geom_.buttons.x, geom_.buttons.y, geom_.buttons.w, geom_.buttons.h};
    }
    const pt::ui::touch_layout::BoxRects& button_rects() const { return buttons_; }

    // The selected skin's casing, label colour, bezel border (skin X-units) and art; defaults are amiga-2.
    void set_skin(uint32_t casingArgb, uint32_t labelRgb, float bezelThicknessX, SkinArt art) {
        casing_   = casingArgb;
        labelRgb_ = labelRgb;
        bezelX_   = bezelThicknessX;
        art_      = art;
    }

    // The live theme's two colours, pushed every frame: a chromeless skin draws its ground in
    // `background` and every key and label in `textValue`, so a theme edit restyles it at once.
    // ⚠️ Push before `casing_argb()` and `signature()` are read for the frame.
    void set_theme(uint32_t backgroundArgb, uint32_t textValueArgb) {
        themeBg_  = backgroundArgb;
        themeInk_ = textValueArgb;
    }

    // What the whole output is cleared to: the casing (seen beside a narrower skin), or the theme
    // background for a chromeless skin, where it is the keys' own ground.
    uint32_t casing_argb() const { return chromeless() ? themeBg_ : casing_; }

    // After the clear, before the frame: the four bands and the inner bezel, filled with
    // `innerBezelArgb` (the theme background) so frame and gap read as one surface. A missing piece
    // shows casing. Draws nothing for a chromeless skin.
    void draw_chrome(SDL_Renderer* r, const Skin& skin, uint32_t innerBezelArgb) const;

    // After the frame: the ten buttons in their PNG variants (pressed when held) with labels — letters
    // from `font`, arrows from `arrowFont` (Helvetica has none), line arrows if it did not load, the
    // 5×5 font if `font` did not. Fonts are mutable: they cache glyphs. Chromeless tints art and labels.
    void draw_buttons(SDL_Renderer* r, const Skin& skin, Font& font, Font& arrowFont,
                      const SdlInput& input) const;

    // A fingerprint for the pixel gate: held buttons + output geometry. Non-zero while active, with a
    // marker bit distinct from `SdlTouch`'s.
    uint64_t signature(const SdlInput& input) const;

private:
    // ⚠️ Derived from the art, not stored: it decides the bare layout, the ground and the tint together.
    bool chromeless() const { return art_ != SkinArt::Chrome; }

    // A button's art and label colour: the live theme, or the chrome skin's own constant.
    uint32_t ink_rgb() const { return (chromeless() ? themeInk_ : labelRgb_) & 0x00FFFFFFu; }

    uint32_t casing_   = 0xFF56606C;
    uint32_t labelRgb_ = 0xFFFFFF;
    float    bezelX_   = 3.0f;
    SkinArt  art_      = SkinArt::Chrome;

    // Used only by a chromeless skin; white on black until the first `set_theme`.
    uint32_t themeBg_  = 0xFF000000;
    uint32_t themeInk_ = 0xFFFFFFFF;

    bool active_ = false;
    int  outW_   = 0;
    int  outH_   = 0;

    pt::ui::touch_layout::Portrait2Skin geom_{};     // the bands + frame + inner bezel, device pixels
    pt::ui::touch_layout::BoxRects      buttons_{};   // local to geom_.buttons
    SDL_Rect                            frame_{0, 0, 0, 0};
};

}  // namespace ptshell

#endif  // POCKETTRACKER_PORTRAIT2_H
