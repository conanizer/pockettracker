// sdl-input.{h,cpp} — raw SDL input → the virtual button model.
//
// The source half of input/ButtonHandlers.kt's `InputMapper`: it turns keyboard and gamepad events
// into presses and releases of the ten buttons the app actually knows about, and it owns the custom
// key repeat. What it deliberately does NOT do is decide what a button MEANS — the combo matrix
// (A+DPAD edits, B+DPAD cycles the item, L+B selection, R+DPAD screen navigation, the A,A double-tap,
// the press-vs-release deferral) is `ui/button_mapper.h`, and what a meaning DOES is
// `ui/input_dispatcher.h`. Splitting there is not a shortcut: THIS half — a keycode, a game-controller
// button, an axis — is the only part of the input chain that is platform-specific, and it is the only
// part left in this directory.
//
// ⚠️ The button model itself (`Button`, `ButtonMods`, `ButtonEvent`) was declared HERE until
// convergence C0.1 and now lives in `ui/buttons.h`, because none of those four types ever named an
// SDL type — see that header for why "a ButtonEvent is an SDL-side type" was a fact about this file
// rather than about the design, and what it was costing.
//
// The keyboard map is copied from the Kotlin one exactly (WASD/arrows, K/Enter = A, J/Esc = B, U/I =
// shoulders, LShift = SELECT, Space = START), so a dev's muscle memory transfers between the two
// builds and a bug report about "the K key" means the same thing on both.

#ifndef POCKETTRACKER_SDL_INPUT_H
#define POCKETTRACKER_SDL_INPUT_H

#include <cmath>  // before <SDL.h> — see sdl-audio-engine.h (M_PI / C4005)

#include <SDL.h>

#include "ui/buttons.h"

#include <cstdint>
#include <deque>
#include <vector>

// The button model is `pt::ui`'s (C0.1). Named unqualified here because every file in this directory
// is shell code, there is nothing else in it called `Button`, and the alternative was ~40 mechanical
// edits across sdl-input.cpp on the same day the combo matrix moved — two changes at once in the one
// part of the tree no golden covers. These four are the whole of the leak, and it stops at the shell.
using pt::ui::Button;
using pt::ui::ButtonAction;
using pt::ui::ButtonEvent;
using pt::ui::ButtonMods;

class SdlInput {
public:
    /** Open every attached controller. Safe to call with none — a dev box is keyboard-only. */
    void open_controllers();
    void close_controllers();

    /**
     * Print one line per input event: what SDL delivered, and what this class did with it — a Button,
     * or nothing at all. Off by default; the shell turns it on for POCKETTRACKER_INPUT_TRACE=1.
     *
     * ⚠️ This exists because of a specific, repeated failure. The port's two input tools (ptinput,
     * ptdispatch) both BEGIN at a `ButtonEvent`, so everything above them — SDL, the CFW's controller
     * mapping, the launch script — has no coverage at all. That is exactly where P4b's bug lived: the
     * app was correct and its ENVIRONMENT fed it phantom input, and only 1 of the 4 collisions was
     * ever reported, because `press()` silently drops a button already held and hides a duplicate
     * wherever the two paths agree.
     *
     * ⭐ And it is what makes "L2/R2 and the stick are inert" a real check rather than a vacuous one.
     * That claim's pass condition is NOTHING HAPPENING, which cannot tell "correctly ignored" from
     * "the device never sent it" from "the app is wedged". With the trace, the same press produces
     * POSITIVE evidence either way: an axis line with no button after it means SDL delivered it and
     * this class dropped it on purpose. The control is built in — the buttons that DO work print their
     * mapping on the same run, so a silent trace is a broken trace, not a passing test.
     */
    void set_trace(bool on) { trace_ = on; }

    /**
     * Feed one SDL event. Presses/releases land in the queue; everything else is ignored.
     *
     * The clock is passed IN rather than read from SDL_GetTicks64() inside, and that is not
     * ceremony: a press arms the key repeat, so the class's whole behaviour is a function of time,
     * and a class that reads a global clock cannot be tested — you can only watch it and hope.
     * With the clock injected, the press/release/repeat semantics are checkable exactly.
     */
    void handle_event(const SDL_Event& e, uint64_t now_ms);

    /**
     * Once per frame. Emits the synthetic repeat presses — 400 ms to the first, then one every
     * 100 ms, exactly as `InputMapper.startKeyRepeat` does.
     */
    void tick(uint64_t now_ms);

    /** Drain one queued event; false when empty. */
    bool poll(ButtonEvent& out);

    bool is_held(Button b) const { return held_[static_cast<size_t>(b)]; }

    /** Drop all held state and any repeat in flight (window focus loss — a stuck modifier
     *  silently reroutes every later DPAD press into the wrong combo). Kotlin's `InputMapper.reset()`. */
    void reset();

    /**
     * D4: a virtual (touch) button feeds the SAME press/release machinery as a key or a pad, so it
     * inherits all three properties without a line of new code for fingers — the `ButtonMods` snapshot
     * stamped at queue time (P3-S3), the ONE shared key-repeat engine (a held virtual D-pad repeats
     * like a physical one), and the held-button de-dup (a finger on virtual A and a pad's physical A
     * collide safely instead of doubling). The hit-test that turns a finger into a `Button` is
     * `sdl-touch.*`; this is only the seam it pushes through. Kotlin routes its virtual buttons through
     * the identical `InputMapper.onVirtualButton` for exactly this reason (VirtualControls.kt).
     */
    void touch_press(Button b, uint64_t now_ms) { press(b, now_ms); }
    void touch_release(Button b)                { release(b); }

    /** How many controllers are open. The shell uses it to choose FULL (physical buttons, no on-screen
     *  controls) vs a touch layout — the SDL analogue of `DeviceAdapter.hasPhysicalGameButtons()`. */
    size_t controller_count() const { return controllers_.size(); }

private:
    void press(Button b, uint64_t now_ms);
    void release(Button b);

    /** The modifiers as they stand right now — stamped onto each event as it is queued. */
    ButtonMods mods_now() const;

    /** One trace line. `mapped` false prints the reason it went nowhere. */
    void trace(const char* source, const char* what, bool mapped, Button b) const;

    static constexpr uint64_t REPEAT_INITIAL_DELAY = 400;  // ms before the first repeat
    static constexpr uint64_t REPEAT_INTERVAL      = 100;  // ms between repeats

    bool held_[static_cast<size_t>(Button::COUNT)] = {false};

    // ONE repeat at a time, and only ever a DPAD button — the same shape as Kotlin, which keeps a
    // single `repeatRunnable` and cancels it before starting another. The app re-reads the modifiers
    // when the repeat fires, so holding A+UP keeps editing and holding UP alone keeps moving, with no
    // need to remember *which* action started the repeat.
    //
    // Releasing A or B cancels it (ButtonHandlers.kt:443). That matters: without it, letting go of A
    // mid-repeat would carry on hammering the value edit while the user thinks they have stopped.
    bool     repeatActive_ = false;
    Button   repeatButton_ = Button::DPAD_UP;
    uint64_t repeatNextMs_ = 0;

    bool trace_ = false;

    std::deque<ButtonEvent>     queue_;
    std::vector<SDL_GameController*> controllers_;
};

#endif  // POCKETTRACKER_SDL_INPUT_H
