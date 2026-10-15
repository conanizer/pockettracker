#pragma once

// config.json's input sections, parsed into key and pad-button names (the shell turns them into SDL
// codes):
//   { "controller": { "abxy": "auto" | "xbox" | "nintendo" },
//     "keyboard":   { "A": ["K", "Return"], "DPAD_UP": ["W", "Up"], … },
//     "gamepad":    { "START": ["start", "y"], … },
//     "repeat":     { "delay": 400, "interval": 32 } }
// Everything is optional; anything absent or malformed keeps the built-in default.

#include "ui/buttons.h"
#include "ui/filesystem.h"

#include <optional>
#include <string>
#include <vector>

namespace pt::ui {

// Which way round the face buttons are, by what is printed on the pad. SDL already reports by label
// for pads it knows; the override is for pads it cannot classify (most third-party ones claim Xbox).
// NINTENDO: label A is the right button. AUTO and XBOX behave alike but say different things; keep both.
enum class AbxyLayout { AUTO, XBOX, NINTENDO };

const char* abxy_name(AbxyLayout layout);
bool        abxy_from_name(const std::string& name, AbxyLayout& out);

// The names bound to each button. nullopt: not listed, keeps its defaults; empty: unbound on purpose.
// A listed button replaces its defaults and takes each input from whatever button had it.
struct ButtonBindings {
    std::optional<std::vector<std::string>> keys[static_cast<size_t>(Button::COUNT)];

    const std::optional<std::vector<std::string>>& operator[](Button b) const {
        return keys[static_cast<size_t>(b)];
    }
    std::optional<std::vector<std::string>>& operator[](Button b) {
        return keys[static_cast<size_t>(b)];
    }
};

// The held-button repeat, ms: `delay`, then a train from 2 × `interval` tightening to `interval`.
// Out-of-range values are clamped, with a warning.
struct RepeatConfig {
    static constexpr int DEFAULT_DELAY    = 400;
    static constexpr int DEFAULT_INTERVAL = 32;
    static constexpr int MIN_DELAY = 100, MAX_DELAY = 2000;
    // Below one 60 Hz frame the cursor skips rows between drawn frames.
    static constexpr int MIN_INTERVAL = 16, MAX_INTERVAL = 500;

    int delay    = DEFAULT_DELAY;
    int interval = DEFAULT_INTERVAL;
};

struct InputConfig {
    AbxyLayout     abxy = AbxyLayout::AUTO;
    ButtonBindings keyboard;
    ButtonBindings gamepad;
    RepeatConfig   repeat;
};

// One rejected entry, for the shell to print.
struct InputConfigWarning {
    std::string text;
};

// The Miyoo Mini: its buttons arrive as a pad, but its config names them as printed ("X", "L1") or by
// the keys of SDL's keyboard mode ("Left Shift") — rewritten here to pad names. That device only.
void miyoo_mini_to_pad_names(ButtonBindings& bindings);

// Curly quotes and full-width `,` `:` into plain ones, so a phone-edited file parses. True if changed.
// ⚠️ Only for a file that did not parse: it also changes quotes inside valid strings.
bool straighten_typographic_punctuation(std::string& text);

// False with no file or one that does not parse (`out` untouched); else fills what the file carries.
// Rejections are appended to `warnings`; an unknown key name is the shell's to catch.
bool load_input_config(FileSystem& fs, InputConfig& out, std::vector<InputConfigWarning>& warnings);

}  // namespace pt::ui
