// The on-screen gamepad. Landscape: two boxes in the letterbox bars, drawn and hit-tested here. Portrait:
// only the hit-test (`layout_portrait2`); `PortraitSkin` draws. A finger becomes a press through
// `SdlInput`'s own press/release, so it gets the modifiers, the key repeat and the held-button de-dup.

#ifndef POCKETTRACKER_SDL_TOUCH_H
#define POCKETTRACKER_SDL_TOUCH_H

#include <cmath>  // before <SDL.h> — see sdl-audio-engine.h

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
    void set_enabled(bool on) { enabled_ = on; }

    // Click + haptic; null = none (only Android has one).
    void set_feedback(ptshell::ButtonFeedback* fb) { feedback_ = fb; }

    // The live BTN SOUND / BTN VIBRO values, pushed each frame.
    void set_feedback_settings(const ptshell::ButtonFeedbackSettings& s) { fbSettings_ = s; }

    // POCKETTRACKER_INPUT_TRACE: one line per finger — a tap in a gap says so.
    void set_trace(bool on) { trace_ = on; }

    // Enabled, and the bars wide enough for a usable box.
    bool active() const { return active_; }

    // Recompute the boxes; every frame before the event poll, so a finger hits the current layout.
    void layout(const SDL_Rect& frame, int outW, int outH);

    // Instead of `layout()` on a portrait frame: hit-test the cluster `PortraitSkin` drew, with the
    // rects it drew — so a press can never disagree with the screen. Fingers work as in landscape.
    void layout_portrait2(const SDL_Rect& cluster, const pt::ui::touch_layout::BoxRects& rects,
                          int outW, int outH);

    // Down on a button presses it; up, or sliding off it, releases it.
    void handle_finger(const SDL_Event& e, SdlInput& input, uint64_t now_ms);

    // After the frame, before the flip; a button held from any source is drawn pressed.
    void draw(SDL_Renderer* r, const SdlInput& input) const;

    // Held buttons + geometry, for the pixel gate (a press highlight lies outside the canvas).
    uint64_t signature(const SdlInput& input) const;

private:
    // Window pixel → the button under it, if any.
    bool hit_window(int px, int py, Button& out) const;

    void emit_feedback(Button b, bool down) const {
        if (feedback_) feedback_->play(b, down, fbSettings_);
    }

    bool enabled_ = false;
    bool active_  = false;
    bool trace_   = false;

    int      outW_ = 0, outH_ = 0;
    SDL_Rect leftBox_{0, 0, 0, 0};   // the left letterbox bar, in window pixels
    SDL_Rect rightBox_{0, 0, 0, 0};  // the right bar
    pt::ui::touch_layout::BoxRects left_;   // local to the box
    pt::ui::touch_layout::BoxRects right_;

    // Portrait: the cluster in window pixels and the ten rects local to it. While set, `hit_window`
    // tests these and `draw` does nothing.
    bool                           portrait_ = false;
    SDL_Rect                       portraitCluster_{0, 0, 0, 0};
    pt::ui::touch_layout::BoxRects portrait_rects_;

    // Which finger holds which button, so multi-touch holds (L+A) work; bound until it lifts or slides off.
    std::unordered_map<SDL_FingerID, Button> finger_;

    ptshell::ButtonFeedback*         feedback_ = nullptr;
    ptshell::ButtonFeedbackSettings  fbSettings_;
};

#endif  // POCKETTRACKER_SDL_TOUCH_H
