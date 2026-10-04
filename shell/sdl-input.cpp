#include "sdl-input.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

using pt::ui::AbxyLayout;
using pt::ui::button_name;
using pt::ui::KeyboardBindings;

namespace {

/**
 * The built-in keyboard map. A TABLE because it has a second reader: the config.json starter
 * template is generated from it (`default_keyboard_bindings`), so what the file tells the user is
 * what the app dispatches. Each button's primary key comes first: `"A": ["K", "Return"]`.
 */
struct KeyDefault {
    SDL_Keycode key;
    Button      button;
};

constexpr KeyDefault KEY_DEFAULTS[] = {
    // D-pad: WASD (the PC-gamer cluster) and the arrow keys
    {SDLK_w, Button::DPAD_UP},    {SDLK_UP,    Button::DPAD_UP},
    {SDLK_s, Button::DPAD_DOWN},  {SDLK_DOWN,  Button::DPAD_DOWN},
    {SDLK_a, Button::DPAD_LEFT},  {SDLK_LEFT,  Button::DPAD_LEFT},
    {SDLK_d, Button::DPAD_RIGHT}, {SDLK_RIGHT, Button::DPAD_RIGHT},

    // Face buttons: right-hand home row, plus Enter/Escape
    {SDLK_k, Button::A}, {SDLK_RETURN, Button::A},
    {SDLK_j, Button::B}, {SDLK_ESCAPE, Button::B},

    // Shoulders: the keys above the face buttons
    {SDLK_u, Button::L_SHIFT},
    {SDLK_i, Button::R_SHIFT},

    // System
    {SDLK_LSHIFT, Button::SELECT},
    {SDLK_SPACE,  Button::START},
};

/** SDL returns NULL for an enum it does not recognise, and "%s" with NULL is UB. Never trust it. */
const char* or_unknown(const char* s) { return s ? s : "?"; }

// The always-repeatable buttons are the D-pad; `is_dpad` lives beside the enum in ui/buttons.h.
// B joins them only under `set_b_repeatable` — see press().

}  // namespace

SdlInput::SdlInput() {
    keyMap_.reserve(std::size(KEY_DEFAULTS));
    for (const KeyDefault& d : KEY_DEFAULTS) keyMap_.emplace_back(d.key, d.button);
}

KeyboardBindings SdlInput::default_keyboard_bindings() {
    KeyboardBindings out;
    for (const KeyDefault& d : KEY_DEFAULTS) {
        auto& slot = out[d.button];
        if (!slot) slot.emplace();
        slot->emplace_back(or_unknown(SDL_GetKeyName(d.key)));
    }
    return out;
}

void SdlInput::apply_input_config(const pt::ui::InputConfig& cfg) {
    abxy_ = cfg.abxy;
    if (abxy_ != AbxyLayout::AUTO) {
        std::printf("config:   controller abxy = %s\n", pt::ui::abxy_name(abxy_));
    }

    // ⚠️ ONE SUMMARY LINE, NOT ONE PER BUTTON: the seeded template lists all ten at their defaults, so
    // per-button lines would claim a change on every launch of an untouched install. Which key gave
    // which button is the input trace's job (`set_trace`).
    int bound = 0, skipped = 0;

    for (int i = 0; i < static_cast<int>(Button::COUNT); ++i) {
        const Button b     = static_cast<Button>(i);
        const auto&  names = cfg.keyboard[b];
        if (!names) continue;   // not listed → keeps its defaults
        ++bound;

        // REPLACE, not merge — the header's contract, and the only way to free a key that is in the
        // way. An empty list therefore leaves the button unbound, which is what `[]` plainly says.
        keyMap_.erase(std::remove_if(keyMap_.begin(), keyMap_.end(),
                                     [b](const std::pair<SDL_Keycode, Button>& e) {
                                         return e.second == b;
                                     }),
                      keyMap_.end());

        for (const std::string& name : *names) {
            const SDL_Keycode k = SDL_GetKeyFromName(name.c_str());
            if (k == SDLK_UNKNOWN) {
                // ⚠️ Reported, never silently skipped — see the header.
                std::printf("config:   keyboard.%s: \"%s\" is not an SDL key name - skipped\n",
                            button_name(b), name.c_str());
                ++skipped;
                continue;
            }
            keyMap_.emplace_back(k, b);
        }
    }

    // Unconditional whenever the section was present; `skipped` shares the line so a partially
    // applied file announces itself.
    if (bound > 0) {
        std::printf("config:   keyboard: %d button(s) from config.json, %d key name(s) rejected\n",
                    bound, skipped);
    }
}

bool SdlInput::key_to_button(SDL_Keycode k, Button& out) const {
    // ⚠️ ANDROID'S BACK BUTTON — WITHOUT THIS LINE IT CLOSES THE APP MID-EDIT. It arrives as a key once
    // `SDL_HINT_ANDROID_TRAP_BACK_BUTTON` is armed (android-main.cpp). B, because B is already the
    // universal cancel; the app is still leavable by Home and PROJECT > EXIT.
    // ⚠️ HARD-WIRED, AHEAD OF THE CONFIGURABLE MAP and absent from `KEY_DEFAULTS`: rebinding B on a
    // desktop must not take away the only way back on Android. No desktop keyboard produces AC_BACK.
    if (k == SDLK_AC_BACK) { out = Button::B; return true; }

    for (const std::pair<SDL_Keycode, Button>& e : keyMap_) {
        if (e.first == k) { out = e.second; return true; }
    }
    return false;
}

bool SdlInput::pad_to_button(Uint8 b, Button& out) const {
    // Which face-button pair means A (ui/input_config.h): with NINTENDO the pad's labels run the
    // other way, so A is the pair SDL calls B/Y. ⚠️ BOTH PAIRS SWAP TOGETHER, or the X/Y aliases keep
    // the old meaning.
    const bool nintendo = (abxy_ == AbxyLayout::NINTENDO);

    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    out = Button::DPAD_UP;    return true;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  out = Button::DPAD_DOWN;  return true;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  out = Button::DPAD_LEFT;  return true;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: out = Button::DPAD_RIGHT; return true;

        // X and Y are aliased onto A and B on purpose: face-button layouts differ across handhelds,
        // and a four-button app listening to two is one bad SDL mapping from unusable.
        case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_X:
            out = nintendo ? Button::B : Button::A;
            return true;
        case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_Y:
            out = nintendo ? Button::A : Button::B;
            return true;

        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  out = Button::L_SHIFT; return true;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: out = Button::R_SHIFT; return true;
        case SDL_CONTROLLER_BUTTON_BACK:          out = Button::SELECT;  return true;
        case SDL_CONTROLLER_BUTTON_START:         out = Button::START;   return true;

        // ⚠️ Not mapped: the L2/R2 triggers and the analog stick. Both are axes that differ per CFW
        // and need a real device to verify.
        default: return false;
    }
}

void SdlInput::open_controllers() {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        if (SDL_GameController* c = SDL_GameControllerOpen(i)) {
            controllers_.push_back(c);
            std::printf("controller: %s\n", SDL_GameControllerName(c));
        }
    }
}

void SdlInput::close_controllers() {
    for (SDL_GameController* c : controllers_) SDL_GameControllerClose(c);
    controllers_.clear();
}

ButtonMods SdlInput::mods_now() const {
    ButtonMods m;
    m.a      = held_[static_cast<size_t>(Button::A)];
    m.b      = held_[static_cast<size_t>(Button::B)];
    m.l      = held_[static_cast<size_t>(Button::L_SHIFT)];
    m.r      = held_[static_cast<size_t>(Button::R_SHIFT)];
    m.select = held_[static_cast<size_t>(Button::SELECT)];
    return m;
}

void SdlInput::press(Button b, uint64_t now_ms) {
    const size_t i = static_cast<size_t>(b);
    if (held_[i]) {
        // ⚠️ A press for a button already down is dropped (the OS auto-repeat is ignored), but on a
        // handheld, where one button makes exactly one press, it means SOMETHING ELSE is pressing too
        // — an injected second copy (gptokeyb) is absorbed silently here wherever the paths agree.
        if (trace_) {
            std::printf("input:              ^ ABSORBED: %s was already held - a SECOND source pressed it\n",
                        button_name(b));
        }
        return;
    }
    held_[i] = true;
    // AFTER the flag is set, so a press of A itself reports A as held — modifier state is updated
    // before the combo is resolved.
    queue_.push_back({b, ButtonAction::PRESSED, mods_now()});

    // B joins the D-pad only while `set_b_repeatable` says so — the qwerty overlay, where B is a
    // backspace. Everywhere else B is COPY / BACK / CANCEL and must fire exactly once per press.
    if (is_dpad(b) || (b == Button::B && bRepeatable_)) {
        repeatActive_  = true;
        repeatButton_  = b;
        repeatNextMs_  = now_ms + REPEAT_INITIAL_DELAY;
        repeatTrainMs_ = repeatNextMs_;
    }
}

void SdlInput::release(Button b) {
    const size_t i = static_cast<size_t>(b);
    if (!held_[i]) return;
    held_[i] = false;
    queue_.push_back({b, ButtonAction::RELEASED, mods_now()});

    // Cancel the repeat when the repeating DPAD is let go — or when A or B is, because those are the
    // modifiers that gave it its meaning.
    if (repeatActive_ && (b == repeatButton_ || b == Button::A || b == Button::B)) {
        repeatActive_ = false;
    }
}

void SdlInput::trace(const char* source, const char* what, bool mapped, Button b) const {
    if (!trace_) return;
    // ⚠️ The UNMAPPED line is the load-bearing one, not the mapped one. It is what turns "the stick
    // does nothing" from an absence into a measurement — and the mapped lines beside it are the
    // positive control that proves the trace is alive at all.
    std::printf("input:   %-10s %-24s -> %s\n", source, what,
                mapped ? button_name(b) : "(ignored: not mapped)");
}

void SdlInput::handle_event(const SDL_Event& e, uint64_t now) {
    Button b{};

    switch (e.type) {
        case SDL_KEYDOWN:
            // e.key.repeat: the OS repeat, deliberately dropped — the app's repeat must be the same
            // on a keyboard and on a D-pad with no OS repeat.
            if (e.key.repeat != 0) break;
            {
                const bool mapped = key_to_button(e.key.keysym.sym, b);
                // ⚠️ A handheld should produce NO keyboard events. One here means some layer (gptokeyb
                // in the launch script, a CFW hotkey daemon) injects phantom input — this line names it.
                trace("KEYDOWN", or_unknown(SDL_GetKeyName(e.key.keysym.sym)), mapped, b);
                if (mapped) press(b, now);
            }
            break;

        case SDL_KEYUP:
            if (key_to_button(e.key.keysym.sym, b)) release(b);
            break;

        case SDL_CONTROLLERBUTTONDOWN: {
            const bool mapped = pad_to_button(e.cbutton.button, b);
            trace("PAD DOWN",
                  or_unknown(SDL_GameControllerGetStringForButton(
                      static_cast<SDL_GameControllerButton>(e.cbutton.button))),
                  mapped, b);
            if (mapped) press(b, now);
            break;
        }

        case SDL_CONTROLLERBUTTONUP: {
            const bool mapped = pad_to_button(e.cbutton.button, b);
            trace("PAD UP",
                  or_unknown(SDL_GameControllerGetStringForButton(
                      static_cast<SDL_GameControllerButton>(e.cbutton.button))),
                  mapped, b);
            if (mapped) release(b);
            break;
        }

        case SDL_CONTROLLERAXISMOTION: {
            // ⚠️ DELIBERATELY NO MAPPING: every axis is dropped. This only adds VISIBILITY — triggers
            // and sticks arrive as axes, so without it "they are inert" could only be observed as an
            // absence (ignored? never sent? wedged?). A flood of these means a stick drifting hard.
            if (!trace_) break;
            char what[64];
            std::snprintf(what, sizeof(what), "%s value=%d",
                          or_unknown(SDL_GameControllerGetStringForAxis(
                              static_cast<SDL_GameControllerAxis>(e.caxis.axis))),
                          static_cast<int>(e.caxis.value));
            trace("PAD AXIS", what, false, Button::COUNT);
            break;
        }

        case SDL_CONTROLLERDEVICEADDED:
            if (SDL_GameController* c = SDL_GameControllerOpen(e.cdevice.which)) {
                controllers_.push_back(c);
            }
            break;

        case SDL_CONTROLLERDEVICEREMOVED: {
            // ⚠️ A REMOVED PAD SENDS NO BUTTON-UPS, so its held buttons stay held — and `mods_now()`
            // reads A, B, L, R and SELECT from `held_` (a pad asleep with L down turns every D-pad
            // press into a screen change). `reset()` releases them, as for focus loss.
            // ⚠️ The handle must be CLOSED and dropped: `controller_count()` decides whether the
            // on-screen controls come back, and replugging would leak handles.
            // `e.cdevice.which` is an INSTANCE id on removal (an index only on ADDED).
            for (auto it = controllers_.begin(); it != controllers_.end(); ++it) {
                SDL_Joystick* js = SDL_GameControllerGetJoystick(*it);
                if (js && SDL_JoystickInstanceID(js) == e.cdevice.which) {
                    SDL_GameControllerClose(*it);
                    controllers_.erase(it);
                    break;
                }
            }
            reset();
            break;
        }

        case SDL_WINDOWEVENT:
            // Focus loss eats the KEYUPs, and a stuck modifier reroutes every later DPAD press.
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) reset();
            break;

        default:
            break;
    }
}

uint64_t SdlInput::repeat_interval(uint64_t repeating_ms) {
    if (repeating_ms >= REPEAT_RAMP_MS) return REPEAT_INTERVAL_FAST;
    const uint64_t span = REPEAT_INTERVAL_SLOW - REPEAT_INTERVAL_FAST;
    return REPEAT_INTERVAL_SLOW - span * repeating_ms / REPEAT_RAMP_MS;
}

void SdlInput::tick(uint64_t now_ms) {
    if (!repeatActive_ || now_ms < repeatNextMs_) return;

    // ONE repeat per tick, the next deadline measured from NOW: a catch-up loop would flush several
    // queued repeats after a stall and jump a held A+UP by five. The repeat carries the modifiers as
    // they stand NOW, so pressing A while UP repeats switches from moving to editing.
    queue_.push_back({repeatButton_, ButtonAction::PRESSED, mods_now()});
    repeatNextMs_ = now_ms + repeat_interval(now_ms > repeatTrainMs_ ? now_ms - repeatTrainMs_ : 0);
}

bool SdlInput::poll(ButtonEvent& out) {
    if (queue_.empty()) return false;
    out = queue_.front();
    queue_.pop_front();
    return true;
}

void SdlInput::reset() {
    // ⚠️ A HELD BUTTON IS RELEASED, NOT MERELY FORGOTTEN: consumers act on releases — the FX-helper
    // overlay's ONLY close is `on_a_released()`, and the mapper's deferred latches discharge on one.
    // Emitted HERE, through `release()`, so a synthesised release is indistinguishable from the lost
    // key-up and later consumers get it for free.
    queue_.clear();   // this frame's presses belong to a window that no longer has focus
    for (size_t i = 0; i < static_cast<size_t>(Button::COUNT); ++i)
        if (held_[i]) release(static_cast<Button>(i));
    repeatActive_ = false;
}
