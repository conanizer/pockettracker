// sdl-touch.{h,cpp} — the on-screen virtual gamepad (convergence D3/D4/D5, landscape).
//
// The one part of the Android app with no C++ twin (convergence plan §6), landed as the plan's D1
// split says: the hit-rect LAYOUT is shared, portable C++ (`native/ui/touch_layout.h`), and the
// RENDERING is shell-side — the buttons are chrome drawn AROUND the letterboxed 640×480 frame, not
// content inside the canvas, so `pt-ui` keeps its four primitives and never learns about a panel.
//
// This file is the shell half. It:
//   • places the two LANDSCAPE boxes into the letterbox bars either side of the frame (D5) — the
//     centred frame already leaves a bar on each side, and those bars ARE the panels, so nothing about
//     the scaler had to change;
//   • turns an SDL finger into a `Button` by hit-testing the shared rects, and feeds it through
//     `SdlInput`'s OWN press/release (D4) — inheriting the mods snapshot, the one key-repeat engine and
//     the held-button de-dup for free, exactly as Kotlin routes its virtual buttons through
//     `InputMapper.onVirtualButton`;
//   • draws the boxes, the buttons and their labels with the shared 5×5 font, highlighting a held one.
//
// ⚠️ LANDSCAPE panels + PORTRAIT2 hit-test. `layout()` draws AND hit-tests the two landscape bars;
// `layout_portrait2()` hit-tests the PORTRAIT2 skinned grid (which `PortraitSkin` draws — this file only
// maps the finger). Still missing: PORTRAIT's two-box split (`touch_layout.h` has `left_rects`/
// `right_rects` for the bars; the portrait split reuses them). The SETTINGS LAYOUT row
// (PlatformCaps::touchLayouts) stays OFF until every mode exists, because a picker offering modes that
// do nothing is the "setting which configures nothing" platform_caps.h refuses.

#ifndef POCKETTRACKER_SDL_TOUCH_H
#define POCKETTRACKER_SDL_TOUCH_H

#include <cmath>  // before <SDL.h> — see sdl-audio-engine.h (M_PI / C4005)

#include <SDL.h>

#include "ui/buttons.h"
#include "ui/touch_layout.h"

#include <cstdint>
#include <unordered_map>

class SdlInput;

using pt::ui::Button;

class SdlTouch {
public:
    /** Whether this platform has a touchscreen worth drawing a gamepad on. Set once at boot — a phone
     *  yes, a desktop no; the SDL analogue of DeviceAdapter choosing a touch layout at all. */
    void set_enabled(bool on) { enabled_ = on; }

    /** Same env var as the input trace (POCKETTRACKER_INPUT_TRACE): print one line per finger, and what
     *  it mapped to (or that it hit no button). The touch half of P4b's "no tool covers the layer
     *  between the hardware and a ButtonEvent" — a tap that lands in a gap must say so POSITIVELY. */
    void set_trace(bool on) { trace_ = on; }

    /** True when the panels are actually on screen: enabled AND the letterbox bars are wide enough to
     *  hold a usable box. A narrow window (or a frame that fills the output) shows none. */
    bool active() const { return active_; }

    /**
     * Recompute the box geometry from the current frame rect and output size — cheap, called every
     * frame BEFORE the event poll so a finger that arrives this frame hits the current layout. A
     * rotate or a resize is absorbed for free the next frame.
     */
    void layout(const SDL_Rect& frame, int outW, int outH);

    /**
     * The PORTRAIT branch of `layout` — hit-test the PORTRAIT2 button cluster instead of the landscape
     * letterbox bars. `PortraitSkin` has already computed this exact geometry to DRAW it (band 4's rect
     * + the ten box-local `portrait2_rects`), so the shell hands it in here rather than recomputing:
     * ONE source of truth, so a press can never disagree with what is on screen. This object turns
     * fingers into presses only — the DRAWING stays PortraitSkin's. Called INSTEAD of `layout()` on a
     * portrait frame (see app.cpp), and it shares `handle_finger` / the finger map / slide-off with the
     * landscape path unchanged: only which rects get hit-tested differs.
     */
    void layout_portrait2(const SDL_Rect& cluster, const pt::ui::touch_layout::BoxRects& rects,
                          int outW, int outH);

    /** Feed one SDL_FINGER{DOWN,MOTION,UP}. Down that hits a button presses it; up releases it; a slide
     *  off the button it went down on releases it (Kotlin's per-button gesture cancels the same way). */
    void handle_finger(const SDL_Event& e, SdlInput& input, uint64_t now_ms);

    /** Draw the panels onto the renderer — called by `SdlVideo::present` after the frame, before the
     *  flip. A held button (from ANY source, via `SdlInput`) is drawn pressed. */
    void draw(SDL_Renderer* r, const SdlInput& input) const;

    /**
     * A fingerprint of what `draw` would produce — the held buttons plus the geometry. `present`'s C7
     * pixel gate compares the 640×480 canvas only, so without this a press highlight (a change OUTSIDE
     * that canvas) would be skipped; feeding this into the gate is what makes the highlight appear.
     */
    uint64_t signature(const SdlInput& input) const;

private:
    /** Window-pixel point → the Button whose rect contains it, if any (searches both boxes). */
    bool hit_window(int px, int py, Button& out) const;

    bool enabled_ = false;
    bool active_  = false;
    bool trace_   = false;

    int      outW_ = 0, outH_ = 0;
    SDL_Rect leftBox_{0, 0, 0, 0};   // the left letterbox bar, in window pixels
    SDL_Rect rightBox_{0, 0, 0, 0};  // the right bar
    pt::ui::touch_layout::BoxRects left_;   // box-local rects (add the box origin to place them)
    pt::ui::touch_layout::BoxRects right_;

    // PORTRAIT2 (layout_portrait2): the skinned cluster's rect in window pixels, and the ten button
    // rects LOCAL to it. While `portrait_` is set, `hit_window` tests these instead of the two bars and
    // `draw` no-ops (PortraitSkin owns the cluster's pixels). One frame's geometry, handed from there.
    bool                           portrait_ = false;
    SDL_Rect                       portraitCluster_{0, 0, 0, 0};
    pt::ui::touch_layout::BoxRects portrait_rects_;

    // Which finger is holding which button, so an UP releases the right one and multi-touch holds (L+A)
    // work. A finger stays bound until it lifts or slides off.
    std::unordered_map<SDL_FingerID, Button> finger_;
};

#endif  // POCKETTRACKER_SDL_TOUCH_H
