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
