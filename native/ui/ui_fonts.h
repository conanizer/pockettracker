#pragma once

// The fonts the FONT setting offers, by the name settings.json stores. A narrower font keeps its own
// letter spacing; every one's step is at most CHAR_W, so columns stay where they are.

#include <string>

#include "canvas.h"
#include "fonts/ptslim.h"
#include "fonts/unscii.h"

namespace pt::ui {

// Drawn at full size, so it cannot be scaled: the EQ marks borrow the plain 5×5 at 2×.
inline constexpr UiFont UI_FONT_PTSLIM{&fonts::PTSLIM, {1, 0, 15}, {2, 2, 10, &fonts::PT5X5}, {1, 0, 18}, {1, 0, 15}};
inline constexpr UiFont UI_FONT_UNSCII{&fonts::UNSCII, {2, 0, 15}, {1, 1, 10}, {2, 0, 20}, {2, 0, 15}};

struct UiFontChoice {
    const char*   name;   // shown on SETTINGS and stored in settings.json
    const UiFont* font;
};

// ⚠️ The first is the default, and what a name no build knows falls back to.
inline constexpr UiFontChoice UI_FONTS[] = {
    {"5X5", &UI_FONT_PT5X5},
    {"UNSCII", &UI_FONT_UNSCII},
    {"SLIM", &UI_FONT_PTSLIM},
};
inline constexpr int UI_FONT_COUNT = static_cast<int>(sizeof(UI_FONTS) / sizeof(UI_FONTS[0]));

inline int ui_font_index(const std::string& name) {
    for (int i = 0; i < UI_FONT_COUNT; ++i)
        if (name == UI_FONTS[i].name) return i;
    return 0;
}

inline const UiFont& ui_font_named(const std::string& name) { return *UI_FONTS[ui_font_index(name)].font; }

// Whether the letter case can be set: a font without small letters draws a–z as capitals.
inline bool ui_font_has_lowercase(const std::string& name) {
    return ui_font_named(name).glyphs->hasLowercase;
}

// The FONT row's second column, by TextCase — each written in its own case.
inline constexpr const char* TEXT_CASE_NAMES[] = {"UPPER", "lower", "Title"};

}  // namespace pt::ui
