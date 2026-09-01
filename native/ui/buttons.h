#pragma once

// ─── THE VIRTUAL GAMEPAD ─────────────────────────────────────────────────────────────────────────
//
// The ten buttons the app knows about, the modifiers held at the instant of an event, and the event
// itself. Nothing else. No SDL, no POSIX, no window — this header includes `<cstdint>` and stops.
//
// ── WHY THIS MOVED (convergence C0.1) ────────────────────────────────────────────────────────────
//
// These four types were declared in `shell/sdl-input.h` from Phase 3 S1 until convergence C0, and
// `ui/input_dispatcher.h`'s own header says in as many words why there is no `handle(ButtonEvent)`
// on the dispatcher: *"pt-ui must not know SDL exists (a `ButtonEvent` is an SDL-side type)"*.
//
// ⚠️ **That sentence was true about the FILE and never about the TYPE.** Read the definitions below:
// not one of them names an SDL type, includes an SDL header, or depends on one. What made a
// `ButtonEvent` "SDL-side" was the `#include <SDL.h>` at the top of the header it happened to be
// declared in — a fact about where the text sat, which then got written down as a fact about the
// design and used to justify keeping the combo matrix out of the shared tree.
//
// The distinction stops being academic in Phase C: Android needs the identical button model and the
// identical matrix, and the alternative to moving these is a second copy of both — which is exactly
// the drift Phase 3 spent itself killing. So the rule stands, restated the way it was always meant:
// **pt-ui must not know SDL exists.** It still does not. `SdlInput` — the half that reads a keycode
// and a game-controller axis — stays in the shell, which is the only place that can have an opinion
// about them, and it is the only part of the input chain that is platform-specific.
//
// The keyboard/gamepad mapping, the key repeat and the held-state machine live with `SdlInput`
// (shell-side); what a press MEANS is `ui/button_mapper.h` (here); what the meaning DOES is
// `ui/input_dispatcher.h` (here). Kotlin fuses the first two into one `InputMapper` class and the
// split is the only structural difference between the two chains.

#include <cstdint>
#include <cstring>

namespace pt::ui {

/** The virtual gamepad. Identical to Kotlin's `VirtualButton` — every input source maps onto it. */
enum class Button {
    DPAD_UP,
    DPAD_DOWN,
    DPAD_LEFT,
    DPAD_RIGHT,
    A,
    B,
    L_SHIFT,
    R_SHIFT,
    SELECT,
    START,
    COUNT
};

enum class ButtonAction { PRESSED, RELEASED };

/** The four directions, as one question — beside the enum, so a new member cannot miss a call site. */
inline bool is_dpad(Button b) {
    return b == Button::DPAD_UP || b == Button::DPAD_DOWN || b == Button::DPAD_LEFT ||
           b == Button::DPAD_RIGHT;
}

/**
 * The one spelling of each button's name — indexed by `Button`, so it tracks the enum's order.
 *
 * ⚠️ THESE NAMES ARE USER-FACING AND THEREFORE FROZEN. They are the keys a user types into
 * config.json's `"keyboard"` section, so renaming one silently breaks every config file already on a
 * disk somewhere. Same identity rule as a `SettingsRow` value or an FX code: append, never rename.
 *
 * It lives here rather than in `sdl-input.cpp` (where the input trace's copy used to be) so the file
 * a user edits and the trace they read while debugging it cannot drift apart — the whole point of the
 * trace is to tell you which name your key produced, and it can only do that if it is the same table.
 */
inline const char* button_name(Button b) {
    switch (b) {
        case Button::DPAD_UP:    return "DPAD_UP";
        case Button::DPAD_DOWN:  return "DPAD_DOWN";
        case Button::DPAD_LEFT:  return "DPAD_LEFT";
        case Button::DPAD_RIGHT: return "DPAD_RIGHT";
        case Button::A:          return "A";
        case Button::B:          return "B";
        case Button::L_SHIFT:    return "L";
        case Button::R_SHIFT:    return "R";
        case Button::SELECT:     return "SELECT";
        case Button::START:      return "START";
        case Button::COUNT:      break;
    }
    return "?";
}

/**
 * The inverse. False for a name no button answers to — which is a REPORTABLE event, not a silent
 * skip: a typo'd key in config.json must say so, or the user is left with a file that looks applied
 * and is not (the "print SKIPPED when a patch misses" rule, one layer out).
 *
 * Derived from `button_name` rather than a second hand-written table, so the two cannot disagree.
 */
inline bool button_from_name(const char* name, Button& out) {
    if (!name) return false;
    for (int i = 0; i < static_cast<int>(Button::COUNT); ++i) {
        const Button b = static_cast<Button>(i);
        if (std::strcmp(name, button_name(b)) == 0) { out = b; return true; }
    }
    return false;
}

/**
 * Which modifiers were down AT THE MOMENT the event happened.
 *
 * ⚠️ Not a convenience — a correctness requirement, and the reason `is_held()` must NOT be used to
 * resolve a combo. SDL hands us every event since the last frame at once, so by the time the queue is
 * drained the held flags describe the END of the frame, not the instant of each event. Roll B and A
 * down inside one 16 ms frame and a poll-time read sees A already held when it processes the B press
 * — firing A+B (delete!) on a press Kotlin would have treated as a plain B.
 *
 * Kotlin's InputMapper has no such gap: it evaluates each event the instant it arrives, against the
 * state as of that instant. Snapshotting here reproduces that exactly. Synthetic key-repeats snapshot
 * the CURRENT state when they fire, which is also what Kotlin does — "the app re-reads the modifiers
 * when the repeat fires", so holding A+UP keeps editing and holding UP alone keeps moving.
 */
struct ButtonMods {
    bool a = false, b = false, l = false, r = false, select = false;
};

struct ButtonEvent {
    Button       button;
    ButtonAction action;
    ButtonMods   mods;
};

}  // namespace pt::ui
