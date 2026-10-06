// sdl-touch.{h,cpp} — the on-screen virtual gamepad (landscape).
//
// The hit-rect LAYOUT is shared, portable C++ (`native/ui/touch_layout.h`); the RENDERING is
// shell-side — the buttons are chrome drawn AROUND the letterboxed 640×480 frame, so pt-ui never
// learns a panel.
//
// This file:
//   • places the two LANDSCAPE boxes in the letterbox bars either side of the frame;
//   • turns an SDL finger into a `Button` by hit-testing the shared rects and feeds it through
//     `SdlInput`'s OWN press/release — inheriting the mods snapshot, the key-repeat engine and the
//     held-button de-dup;
//   • draws the boxes, buttons and labels with the shared 5×5 font, highlighting a held one.
//
// `layout()` draws AND hit-tests the landscape bars; `layout_portrait2()` hit-tests the PORTRAIT2
// grid, which `PortraitSkin` draws.

#ifndef POCKETTRACKER_SDL_TOUCH_H
#define POCKETTRACKER_SDL_TOUCH_H

#include <cmath>  // before <SDL.h> — see sdl-audio-engine.h (M_PI / C4005)

#include <SDL.h>

#include "skin/button_feedback.h"

#include "ui/buttons.h"
#include "ui/touch_layout.h"

#include <cstdint>
#include <unordered_map>

class SdlInput;

using pt::ui::Button;

class SdlTouch {
public:
    /** Whether this platform has a touchscreen worth drawing a gamepad on. Set once at boot. */
    void set_enabled(bool on) { enabled_ = on; }

    /** The click + haptic sink. Null (the default, desktop/handheld) = no feedback; Android hands in
     *  a JNI shim. Set once at boot — see AppConfig::buttonFeedback. */
    void set_feedback(ptshell::ButtonFeedback* fb) { feedback_ = fb; }

    /** The user's live BTN SOUND / BTN VIBRO scalars, pushed each frame from `SettingsValues` so the
     *  next tap plays with whatever the SETTINGS screen currently shows. A no-op cost with no sink. */
    void set_feedback_settings(const ptshell::ButtonFeedbackSettings& s) { fbSettings_ = s; }

    /** Same env var as the input trace (POCKETTRACKER_INPUT_TRACE): one line per finger and what it
     *  mapped to — a tap that lands in a gap must say so POSITIVELY. */
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

    /** Feed one SDL_FINGER{DOWN,MOTION,UP}. Down that hits a button presses it; up releases it; a
     *  slide off the button it went down on releases it. */
    void handle_finger(const SDL_Event& e, SdlInput& input, uint64_t now_ms);

    /** Draw the panels onto the renderer — called by `SdlVideo::present` after the frame, before the
     *  flip. A held button (from ANY source, via `SdlInput`) is drawn pressed. */
    void draw(SDL_Renderer* r, const SdlInput& input) const;

    /**
     * A fingerprint of what `draw` would produce — the held buttons plus the geometry. `present`'s
     * pixel gate compares only the 640×480 canvas, so without this a press highlight (outside it)
     * would be skipped.
     */
    uint64_t signature(const SdlInput& input) const;

private:
    /** Window-pixel point → the Button whose rect contains it, if any (searches both boxes). */
    bool hit_window(int px, int py, Button& out) const;

    /** Fire the click/haptic for one press or release, if a sink is installed. */
    void emit_feedback(Button b, bool down) const {
        if (feedback_) feedback_->play(b, down, fbSettings_);
    }

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

    // The click/haptic sink and the user's current settings for it. Null sink = no feedback; Android
    // hands in a JNI shim (button_feedback.h).
    ptshell::ButtonFeedback*         feedback_ = nullptr;
    ptshell::ButtonFeedbackSettings  fbSettings_;
};

#endif  // POCKETTRACKER_SDL_TOUCH_H
