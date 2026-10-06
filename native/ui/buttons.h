#pragma once

// ─── THE VIRTUAL GAMEPAD ─────────────────────────────────────────────────────────────────────────
//
// The ten buttons, the modifiers held at the instant of an event, and the event. `<cstdint>` only —
// pt-ui must not know SDL exists. Reading keycodes and controller axes (`SdlInput`) is the shell's;
// what a press MEANS is ui/button_mapper.h; what it DOES is ui/input_dispatcher.h.

#include <cstdint>
#include <cstring>

namespace pt::ui {

/** The virtual gamepad — every input source maps onto it. */
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
 * The one spelling of each button's name, indexed by `Button`.
 * ⚠️ USER-FACING, so FROZEN: they are the keys in config.json's `"keyboard"` section — append, never
 * rename. Shared with the input trace so the name a user types and the name the trace prints match.
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

/** Equality as a hand-typed config.json name means it: any case, and a space or `-` for `_`
 *  ("Dpad down" is DPAD_DOWN). */
inline bool same_name_any_case(const char* a, const char* b) {
    auto fold = [](char c) {
        if (c >= 'a' && c <= 'z') return static_cast<char>(c - 32);
        return (c == ' ' || c == '-') ? '_' : c;
    };
    for (; *a && *b; ++a, ++b) {
        if (fold(*a) != fold(*b)) return false;
    }
    return *a == *b;
}

/**
 * The inverse, in any case. False for an unknown name — which the caller must REPORT: a typo'd key in
 * config.json must say so. Derived from `button_name`, so the two cannot disagree.
 */
inline bool button_from_name(const char* name, Button& out) {
    if (!name) return false;
    for (int i = 0; i < static_cast<int>(Button::COUNT); ++i) {
        const Button b = static_cast<Button>(i);
        if (same_name_any_case(name, button_name(b))) { out = b; return true; }
    }
    return false;
}

/**
 * Which modifiers were down AT THE MOMENT the event happened.
 * ⚠️ Never resolve a combo with `is_held()`: a frame's events arrive at once, so held flags describe the
 * END of the frame — B and A rolled within one frame would fire A+B (delete) on the B press.
 * Synthetic key-repeats snapshot the state when they fire, so holding A+UP keeps editing and UP alone
 * keeps moving.
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
