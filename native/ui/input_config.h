#pragma once

// ─── config.json — the `controller` and `keyboard` sections ──────────────────────────────────────
//
// Read by the SHELL (`sdl-input.cpp`), but parsed HERE: what comes out is key NAMES, never
// `SDL_Keycode`s — pt-ui must not know SDL (`ui/buttons.h`). The shell does the one SDL step,
// `SDL_GetKeyFromName`; the rest is portable and tested headlessly.
//
//   { "controller": { "abxy": "auto" | "xbox" | "nintendo" },
//     "keyboard":   { "A": ["K", "Return"], "DPAD_UP": ["W", "Up"], … } }
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
 * The key names bound to each button, indexed by `Button`.
 *   • nullopt      — not listed: keeps its built-in keys.
 *   • empty vector — listed as `[]`: UNBOUND on purpose.
 * A listed button REPLACES its defaults rather than adding — otherwise a key could never be freed.
 */
struct KeyboardBindings {
    std::optional<std::vector<std::string>> keys[static_cast<size_t>(Button::COUNT)];

    const std::optional<std::vector<std::string>>& operator[](Button b) const {
        return keys[static_cast<size_t>(b)];
    }
    std::optional<std::vector<std::string>>& operator[](Button b) {
        return keys[static_cast<size_t>(b)];
    }
};

struct InputConfig {
    AbxyLayout       abxy = AbxyLayout::AUTO;
    KeyboardBindings keyboard;
};

/** One rejected entry, for the shell to print: a silent skip leaves a file that looks applied and is
 *  not. */
struct InputConfigWarning {
    std::string text;
};

/**
 * Read config.json's `controller` and `keyboard` sections into `out`. False when there is no file
 * (the common case) or it does not parse; `out` is then untouched. A valid file fills only what it
 * carries.
 * Every rejection is APPENDED to `warnings`. An unknown KEY name cannot be caught here — the shell
 * warns when `SDL_GetKeyFromName` refuses it.
 */
bool load_input_config(FileSystem& fs, InputConfig& out, std::vector<InputConfigWarning>& warnings);

}  // namespace pt::ui
