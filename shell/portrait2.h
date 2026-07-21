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
// ⚠️ **RENDERING ONLY, THIS INCREMENT.** The buttons DRAW but are not yet hit-testable: `sdl-touch.cpp`
// is landscape-only, and a portrait branch of it is the next increment. So the cluster is a picture a
// finger cannot press yet — deliberately, so the pixels can be device-proven before the input path is
// wired onto them (the D-increment discipline: see one thing work before adding the next).

#ifndef POCKETTRACKER_PORTRAIT2_H
#define POCKETTRACKER_PORTRAIT2_H

#include <SDL.h>

#include "ui/touch_layout.h"

#include <cstdint>

class SdlInput;

namespace ptshell {

class Skin;

class PortraitSkin {
public:
    /**
     * Recompute the band / frame / button geometry for the current output size. Cheap (a handful of int
     * ops); called each frame BEFORE present, like `SdlTouch::layout`, so a rotation is absorbed the
     * next frame. `enabled` is the same touchscreen gate the skin load and `SdlTouch` use — a phone yes,
     * a desktop no (unless POCKETTRACKER_TOUCH forces it for a bring-up).
     */
    void layout(int outW, int outH, bool enabled);

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

    /** The casing colour the whole output is cleared to before the bands composite over it — it shows
     *  at the sides when the skin is narrower than the device (case C) and in any bottom gap. */
    uint32_t casing_argb() const { return CASING_ARGB; }

    /** UNDERLAY (drawn after the casing clear, BEFORE the frame): the four chrome bands and the black
     *  inner-bezel the frame lands on. A missing piece is a no-op, so an incomplete theme shows casing
     *  or black through rather than crashing. */
    void draw_chrome(SDL_Renderer* r, const Skin& skin) const;

    /** OVERLAY (drawn AFTER the frame): the ten buttons on the backing band, each in its PNG variant
     *  (wide L/R, dark A/B, plain square for the rest; pressed when held) with its label centred. */
    void draw_buttons(SDL_Renderer* r, const Skin& skin, const SdlInput& input) const;

    /**
     * A fingerprint of what this layout would draw, for `SdlVideo`'s C7 pixel gate — the held buttons
     * plus the output geometry. The chrome bands are static (one theme this increment), so only button
     * state and geometry vary. Non-zero while active (a distinct marker bit from `SdlTouch`'s), so a
     * mode switch never collides with a landscape signature.
     */
    uint64_t signature(const SdlInput& input) const;

private:
    // ⚠️ amiga-2's theme scalars, hardcoded for now — the SAME hardcode as `Skin::load("amiga-2")` in
    // app.cpp's walking skeleton. A C++ device-skin table (the twin of Kotlin's DeviceTheme/DeviceSkin)
    // replaces all three when theme SELECTION lands; until a second skin can be chosen there is nothing
    // for a table to choose BETWEEN, and inventing one now is the "abstraction for a caller that does
    // not exist" the shell's own headers warn against. amiga-2: casing 0xFF56606C, label white, and a
    // bezel border of 3 skin-X units (a bezel PNG, so density is irrelevant — see portrait2_skin).
    static constexpr uint32_t CASING_ARGB   = 0xFF56606C;
    static constexpr float    BEZEL_THICK_X = 3.0f;
    static constexpr uint32_t LABEL_RGB     = 0xFFFFFF;

    bool active_ = false;
    int  outW_   = 0;
    int  outH_   = 0;

    pt::ui::touch_layout::Portrait2Skin geom_{};     // the bands + frame + inner bezel, device pixels
    pt::ui::touch_layout::BoxRects      buttons_{};   // box-local; offset by geom_.buttons.{x,y} to draw
    SDL_Rect                            frame_{0, 0, 0, 0};
};

}  // namespace ptshell

#endif  // POCKETTRACKER_PORTRAIT2_H
