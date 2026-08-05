#pragma once

// ─── config.json — the `controller` and `keyboard` sections ──────────────────────────────────────
//
// The two INPUT sections of the file `folder_config.h` describes. They live apart from `folders`
// because they have a different consumer: `folders` is read by pt-ui's dispatcher, while both of
// these are read by the SHELL (`sdl-input.cpp`) — it is the only layer that has an opinion about a
// keycode or a game-controller button.
//
// ⚠️ AND YET THE PARSE IS HERE, NOT IN THE SHELL, AND THAT SPLIT IS THE POINT. What comes out of this
// file is key NAMES — strings — never `SDL_Keycode`s. pt-ui must not know SDL exists (`ui/buttons.h`),
// so the shell does the one step that needs SDL: `SDL_GetKeyFromName`. Everything above it — the JSON
// shape, the button vocabulary, replace-vs-inherit, every rejection — is portable and is exercised by
// `ptdispatch` with no window, no audio device and no controller plugged in.
//
// SCHEMA:
//   { "controller": { "abxy": "auto" | "xbox" | "nintendo" },
//     "keyboard":   { "A": ["K", "Return"], "DPAD_UP": ["W", "Up"], … } }
//
// Both sections are OPTIONAL, and so is every key inside them. Absent, malformed, or of the wrong
// type → the built-in default stands. A config file is not a document: losing one is worth the
// factory settings and a working app, never a dialog.

#include "ui/buttons.h"
#include "ui/filesystem.h"

#include <optional>
#include <string>
#include <vector>

namespace pt::ui {

/**
 * Which way round the pad's face buttons are.
 *
 * ⚠️ READ THIS BEFORE CHANGING THE DEFAULT — it is not the tautology it looks like.
 *
 * SDL does NOT hand us positional buttons. `SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS` defaults to
 * **1**, meaning "report the face buttons by LABEL instead of position" — so for a pad SDL recognises
 * as Nintendo, `SDL_CONTROLLER_BUTTON_A` is already the button *printed* A (the right one), and the
 * app's plain `A → Button::A` is correct with nothing to configure. That is why an Android handheld
 * and a genuine Switch pad both work today and always have.
 *
 * The override exists for the pad SDL CANNOT classify: an 8BitDo (and most third-party pads) in
 * **XInput mode enumerates as an Xbox 360 controller**, so SDL's type is Xbox, the label hint has
 * nothing to swap, and we receive the *positional* bottom button as `SDL_CONTROLLER_BUTTON_A` while
 * the plastic under the user's thumb says B. SDL cannot fix that — the device is misreporting what it
 * is — and no amount of probing here can either. It has to be a human saying which pad they hold.
 *
 * So the values name **what is printed on the pad**, and are only consulted for a real controller:
 *   • AUTO     — trust SDL. The default, and right on every platform that already works.
 *   • NINTENDO — the label A is the RIGHT button. Swaps the two face-button pairs.
 *   • XBOX     — the label A is the BOTTOM button. Explicitly today's positional reading.
 *
 * ⚠️ AUTO and XBOX are deliberately NOT collapsed even though they currently behave identically.
 * They are different claims — "I have not told you" versus "I have told you, and it is Xbox" — and a
 * later SDL that classifies these pads correctly would want to change one and not the other.
 */
enum class AbxyLayout { AUTO, XBOX, NINTENDO };

/** Round-trip the layout names used in the file. */
const char* abxy_name(AbxyLayout layout);
bool        abxy_from_name(const std::string& name, AbxyLayout& out);

/**
 * The key names bound to each button, indexed by `Button`.
 *
 * `std::nullopt` and an EMPTY VECTOR mean different things, and both are reachable from the file:
 *   • nullopt      — the button was not listed. It keeps its built-in keys.
 *   • empty vector — the button was listed as `[]`. It is UNBOUND, on purpose.
 *
 * That distinction is why a listed button REPLACES rather than adds to its defaults. Merging would
 * read more forgiving and would make one thing impossible: freeing a key that is in your way. You
 * cannot bind `K` to something else while `K` is still nailed to A.
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

/** One rejected entry, so the shell can print it. A silent skip would leave the user with a file that
 *  looks applied and is not — the single most expensive way for a hand-edited config to fail. */
struct InputConfigWarning {
    std::string text;
};

/**
 * Read config.json's `controller` and `keyboard` sections into `out`.
 *
 * Returns false when there is no file (the common case, and NOT an error) or when it does not parse;
 * `out` is untouched in both cases, so the caller's defaults stand. A present, valid file fills only
 * what it carries — an absent section leaves that half of `out` alone.
 *
 * Anything rejected is APPENDED to `warnings` rather than dropped: an unknown button name, a
 * non-array value, a non-string inside the array, an unrecognised `abxy`. Note this cannot catch an
 * unknown KEY name — only the shell knows which strings `SDL_GetKeyFromName` accepts, so it warns
 * about those itself at the point it resolves them.
 */
bool load_input_config(FileSystem& fs, InputConfig& out, std::vector<InputConfigWarning>& warnings);

}  // namespace pt::ui
