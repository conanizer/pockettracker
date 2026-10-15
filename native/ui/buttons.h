#pragma once

// The virtual gamepad every input maps onto: ten buttons, and an event with the modifiers held at its
// instant. No SDL here.

#include <cstdint>
#include <cstring>

namespace pt::ui {

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

inline bool is_dpad(Button b) {
    return b == Button::DPAD_UP || b == Button::DPAD_DOWN || b == Button::DPAD_LEFT ||
           b == Button::DPAD_RIGHT;
}

// ⚠️ The keys in config.json: append, never rename.
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

// Equal as a hand-typed name means it: any case, a space or `-` for `_` ("Dpad down").
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

// False for an unknown name, which the caller must report.
inline bool button_from_name(const char* name, Button& out) {
    if (!name) return false;
    for (int i = 0; i < static_cast<int>(Button::COUNT); ++i) {
        const Button b = static_cast<Button>(i);
        if (same_name_any_case(name, button_name(b))) { out = b; return true; }
    }
    return false;
}

// The modifiers down at the event's instant. ⚠️ Never resolve a combo from held flags: they describe
// the end of the frame. Key repeats snapshot their own.
struct ButtonMods {
    bool a = false, b = false, l = false, r = false, select = false;
};

struct ButtonEvent {
    Button       button;
    ButtonAction action;
    ButtonMods   mods;
};

}  // namespace pt::ui
