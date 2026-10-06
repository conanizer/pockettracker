#pragma once

// ─── config.json — the `controller`, `keyboard`, `gamepad` and `repeat` sections ─────────────────
//
// Read by the SHELL (`sdl-input.cpp`), but parsed HERE: what comes out is key and pad-button NAMES,
// never SDL codes — pt-ui must not know SDL (`ui/buttons.h`). The shell does the one SDL step
// (`SDL_GetKeyFromName`, `SDL_GameControllerGetButtonFromString`); the rest is tested headlessly.
//
//   { "controller": { "abxy": "auto" | "xbox" | "nintendo" },
//     "keyboard":   { "A": ["K", "Return"], "DPAD_UP": ["W", "Up"], … },
//     "gamepad":    { "START": ["start", "y"], … },
//     "repeat":     { "delay": 400, "interval": 32 } }
//
// Everything is optional; absent, malformed or wrong-typed → the built-in default. Losing a config is
// worth the factory settings and a working app, never a dialog.

#include "ui/buttons.h"
#include "ui/filesystem.h"

#include <optional>
#include <string>
#include <vector>

namespace pt::ui {

/**
 * Which way round the pad's face buttons are — named for what is PRINTED on the pad.
 * ⚠️ SDL reports face buttons by LABEL by default, so a pad SDL recognises as Nintendo already maps
 * label A to `Button::A` with nothing to configure. The override is for pads SDL cannot classify:
 * most third-party pads in XInput mode enumerate as Xbox 360, and the button under the "B" label then
 * arrives as `SDL_CONTROLLER_BUTTON_A`. Only a human can say which pad they hold.
 *   • AUTO     — trust SDL (the default).
 *   • NINTENDO — label A is the RIGHT button; swaps both face-button pairs.
 *   • XBOX     — label A is the BOTTOM button.
 * ⚠️ AUTO and XBOX behave alike today but are different claims ("not told" vs "told: Xbox"); keep both.
 */
enum class AbxyLayout { AUTO, XBOX, NINTENDO };

/** Round-trip the layout names used in the file. */
const char* abxy_name(AbxyLayout layout);
bool        abxy_from_name(const std::string& name, AbxyLayout& out);

/**
 * The input names bound to each button, indexed by `Button` — keyboard keys or pad buttons.
 *   • nullopt      — not listed: keeps its built-in inputs.
 *   • empty vector — listed as `[]`: UNBOUND on purpose.
 * A listed button REPLACES its defaults rather than adding, and TAKES each input it lists from whatever
 * button had it — so `"START": ["start", "y"]` alone moves Y off B.
 */
struct ButtonBindings {
    std::optional<std::vector<std::string>> keys[static_cast<size_t>(Button::COUNT)];

    const std::optional<std::vector<std::string>>& operator[](Button b) const {
        return keys[static_cast<size_t>(b)];
    }
    std::optional<std::vector<std::string>>& operator[](Button b) {
        return keys[static_cast<size_t>(b)];
    }
};

/**
 * The held-button repeat, in ms: `delay` before the first repeat, then a train that starts at
 * 2 × `interval` and tightens to `interval`. Out-of-range values are clamped, with a warning.
 */
struct RepeatConfig {
    static constexpr int DEFAULT_DELAY    = 400;
    static constexpr int DEFAULT_INTERVAL = 32;
    static constexpr int MIN_DELAY = 100, MAX_DELAY = 2000;
    // ⚠️ Below one 60 Hz frame the cursor moves more than one row per drawn frame and reads as a stutter.
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

/** One rejected entry, for the shell to print: a silent skip leaves a file that looks applied and is
 *  not. */
struct InputConfigWarning {
    std::string text;
};

/**
 * The Miyoo Mini's buttons as controller button names. Its SDL delivers them as a PAD (once the app
 * opens controllers), but its config files name them by their printed names ("X", "L1") or by the
 * keys that SDL sends in its keyboard mode ("Left Shift", "E") — both are rewritten here, any case.
 * `L` and `R` stand for both rows of shoulders. Any other name passes through as a pad name.
 * ⚠️ Only for that device. The shell decides when to call it.
 */
void miyoo_mini_to_pad_names(ButtonBindings& bindings);

/**
 * Turn the curly quotes and full-width `,` `:` that phone and word-processor keyboards type into
 * plain ones, so a hand-edited file still parses. True iff anything changed.
 * ⚠️ Only for a file that did NOT parse: it would also change a curly quote inside a valid string.
 */
bool straighten_typographic_punctuation(std::string& text);

/**
 * Read config.json's input sections into `out`. False when there is no file
 * (the common case) or it does not parse; `out` is then untouched. A valid file fills only what it
 * carries.
 * Every rejection is APPENDED to `warnings`. An unknown key or pad-button name cannot be caught here —
 * the shell warns when SDL refuses it.
 */
bool load_input_config(FileSystem& fs, InputConfig& out, std::vector<InputConfigWarning>& warnings);

}  // namespace pt::ui
