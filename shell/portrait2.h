// ─── shell/portrait2.{h,cpp} — the PORTRAIT2 device-skin RENDERER (convergence D) ─────────────────
//
// The shell half of the retro-device skin: on a phone held in portrait, the 640×480 tracker sits in a
// bezel with a ventilation panel above it, a branding strip below, and a themed button cluster filling
// the bottom — 20:9 chrome drawn AROUND the letterboxed frame, exactly as `PortraitLayout2WithVirtual‐
// Buttons` (ScreenLayouts.kt) + `VirtualControlsPortrait2` (VirtualControls.kt) do on Android today.
//
// It is the consumer the host-tested geometry was written for. `touch_layout::portrait2_skin` computes
// the four band rects + the frame-in-bezel (checked by `pttouch --positions`); this file COMPOSITES
// them: it clears to the casing colour, blits each band's PNG (from `Skin`, decoded once), tells
// `SdlVideo` where the frame goes, and draws the ten buttons on the backing. The split is convergence
// D1's: the ARITHMETIC is shared, portable, golden/oracle-checked C++; the PIXELS are shell-side, and
// this is the shell.
//
//   ┌───────────────┐  band 1  top vent panel   (SkinPiece::TopPanel)      — may be absent (case C)
//   │  ┌─────────┐  │  band 2  screen bezel      (SkinPiece::ScreenBezel)   — the 640×480 frame sits INSIDE
//   │  │ 640×480 │  │
//   ├──┴─────────┴──┤  band 3  branding strip    (SkinPiece::BrandingPanel) — FULL device width
//   │  [buttons...]  │  band 4  button cluster    (SkinPiece::ButtonBacking) — the ten portrait2_rects
//   └───────────────┘
//
// The buttons are hit-testable. `PortraitSkin` exposes the cluster rect + the ten box-local button rects
// (`cluster_rect()` / `button_rects()`) — the SAME geometry it draws — and `SdlTouch::layout_portrait2`
// hit-tests them, feeding fingers through `SdlInput`'s own press/release exactly as the landscape panels
// do. Drawing stays HERE; only the finger→Button mapping is SdlTouch's, so there is one source of truth
// for where a button is and a press can never highlight a cell the finger is not on.

#ifndef POCKETTRACKER_PORTRAIT2_H
#define POCKETTRACKER_PORTRAIT2_H

#include <SDL.h>

#include "ui/touch_layout.h"

#include <cstdint>

class SdlInput;

namespace ptshell {

class Skin;
class Font;

class PortraitSkin {
public:
    /**
     * Recompute the band / frame / button geometry for the current output size. Cheap (a handful of int
     * ops); called each frame BEFORE present, like `SdlTouch::layout`, so a rotation is absorbed the
     * next frame. `enabled` is the same touchscreen gate the skin load and `SdlTouch` use — a phone yes,
     * a desktop no (unless POCKETTRACKER_TOUCH forces it for a bring-up).
     *
     * `fit` is SETTINGS > SCALING (`scalingBilinear`): INTEGER (false) integer-scales the 640×480 frame
     * and centres it in the bezel; FIT (true) fills the bezel's inner area with the largest 4:3 fit
     * (a fractional scale, filtered), exactly as Kotlin's PortraitLayout2 does under BILINEAR. It only
     * changes where `frame_rect()` lands — the texture's own filtering follows `SdlVideo::set_scaling`.
     */
    void layout(int outW, int outH, bool enabled, bool fit);

    /**
     * True when the PORTRAIT2 skin should be presented instead of the centred landscape frame: a
     * touchscreen, in portrait (output taller than wide). Landscape and desktop stay on the plain
     * centred present path. This is the shell's whole mode selector — a value derived from the output
     * aspect, not a stored setting, so a live rotation switches it with nothing to keep in sync.
     */
    bool active() const { return active_; }

    /** Where the 640×480 tracker texture blits — band 2's inner bezel, integer-scaled and centred
     *  THERE (not window-centred). Handed to `SdlVideo::present_skinned` as the frame dest. */
    SDL_Rect frame_rect() const { return frame_; }

    /** The button-cluster band (band 4) in output pixels, and the ten button rects box-LOCAL to it
     *  (offset by the cluster origin to place them) — the SAME geometry `draw_buttons` uses. Handed to
     *  `SdlTouch::layout_portrait2` so the portrait hit-test shares ONE source of truth with the draw,
     *  and a press can never land on a button the finger is not over. */
    SDL_Rect cluster_rect() const {
        return SDL_Rect{geom_.buttons.x, geom_.buttons.y, geom_.buttons.w, geom_.buttons.h};
    }
    const pt::ui::touch_layout::BoxRects& button_rects() const { return buttons_; }

    /**
     * Adopt a device skin's scalars — the three values the PNG set does not carry: the casing fill, the
     * button-label colour and the bezel border in skin X-units. Called by the shell when the selected
     * skin loads or changes (device_skin.h / SETTINGS > LAYOUT skin column), so `PortraitSkin` no longer
     * hardcodes amiga-2. Defaults (below) are amiga-2, so an un-set instance behaves as it did before
     * selection existed.
     */
    void set_skin(uint32_t casingArgb, uint32_t labelRgb, float bezelThicknessX) {
        casing_    = casingArgb;
        labelRgb_  = labelRgb;
        bezelX_    = bezelThicknessX;
    }

    /** The casing colour the whole output is cleared to before the bands composite over it — it shows
     *  at the sides when the skin is narrower than the device (case C) and in any bottom gap. */
    uint32_t casing_argb() const { return casing_; }

    /** UNDERLAY (drawn after the casing clear, BEFORE the frame): the four chrome bands and the inner
     *  bezel the frame lands on. A missing piece is a no-op, so an incomplete theme shows casing through
     *  rather than crashing.
     *
     *  `innerBezelArgb` fills the bezel's padded inner area — the letterbox gap around the frame. It is
     *  the LIVE pt-ui theme's `background`, NOT black: Kotlin painted this black, but the shell matches
     *  it to the tracker's own background so the frame and the gap around it read as one surface — the
     *  same reasoning (and the same colour) as the landscape letterbox in `SdlVideo::present`. */
    void draw_chrome(SDL_Renderer* r, const Skin& skin, uint32_t innerBezelArgb) const;

    /** OVERLAY (drawn AFTER the frame): the ten buttons on the backing band, each in its PNG variant
     *  (wide L/R, dark A/B, plain square for the rest; pressed when held) with its label. LETTERS come
     *  from `font` (Helvetica) via `draw_text`; the D-pad ARROWS come from `arrowFont` (Linux Biolinum)
     *  via `draw_text` too — a real glyph, because Helvetica ships no arrows — falling back to `font`'s
     *  shell-drawn line arrow when `arrowFont` did not load. Both fonts are mutable: they cache glyph
     *  textures on first use. If `font` itself did not load, the whole cluster falls back to the 5×5
     *  label font, so a missing .otf shows blocky labels rather than none. */
    void draw_buttons(SDL_Renderer* r, const Skin& skin, Font& font, Font& arrowFont,
                      const SdlInput& input) const;

    /**
     * A fingerprint of what this layout would draw, for `SdlVideo`'s C7 pixel gate — the held buttons
     * plus the output geometry. The chrome bands are static (one theme this increment), so only button
     * state and geometry vary. Non-zero while active (a distinct marker bit from `SdlTouch`'s), so a
     * mode switch never collides with a landscape signature.
     */
    uint64_t signature(const SdlInput& input) const;

private:
    // The current skin's scalars, defaulting to amiga-2 (the shell's prior hardcode) so an un-set
    // instance is unchanged. `set_skin` swaps in NORM/DARK from the device-skin table (device_skin.h)
    // when the SETTINGS skin column changes. amiga-2: casing 0xFF56606C, white label, bezel 3 skin-X
    // units (a bezel PNG, so density is irrelevant — see portrait2_skin).
    uint32_t casing_   = 0xFF56606C;
    uint32_t labelRgb_ = 0xFFFFFF;
    float    bezelX_   = 3.0f;

    bool active_ = false;
    int  outW_   = 0;
    int  outH_   = 0;

    pt::ui::touch_layout::Portrait2Skin geom_{};     // the bands + frame + inner bezel, device pixels
    pt::ui::touch_layout::BoxRects      buttons_{};   // box-local; offset by geom_.buttons.{x,y} to draw
    SDL_Rect                            frame_{0, 0, 0, 0};
};

}  // namespace ptshell

#endif  // POCKETTRACKER_PORTRAIT2_H
