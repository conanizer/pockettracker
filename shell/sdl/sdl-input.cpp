#include "sdl/sdl-input.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

using pt::ui::AbxyLayout;
using pt::ui::button_name;
using pt::ui::ButtonBindings;

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

/** The built-in pad map, by PRINTED name (the ABXY swap is applied before lookup). A table for the same
 *  reason as `KEY_DEFAULTS`: the config.json template is generated from it. */
struct PadDefault {
    SDL_GameControllerButton pad;
    Button                   button;
};

constexpr PadDefault PAD_DEFAULTS[] = {
    {SDL_CONTROLLER_BUTTON_DPAD_UP,    Button::DPAD_UP},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN,  Button::DPAD_DOWN},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT,  Button::DPAD_LEFT},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, Button::DPAD_RIGHT},

    // X and Y are aliased onto A and B on purpose: face-button layouts differ across handhelds, and a
    // four-button app listening to two is one bad SDL mapping from unusable.
    {SDL_CONTROLLER_BUTTON_A, Button::A}, {SDL_CONTROLLER_BUTTON_X, Button::A},
    {SDL_CONTROLLER_BUTTON_B, Button::B}, {SDL_CONTROLLER_BUTTON_Y, Button::B},

    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  Button::L_SHIFT},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, Button::R_SHIFT},
    {SDL_CONTROLLER_BUTTON_BACK,          Button::SELECT},
    {SDL_CONTROLLER_BUTTON_START,         Button::START},

    // ⚠️ No default for the L2/R2 triggers (bindable in config.json as lefttrigger/righttrigger) or
    // the analog stick: both are axes that differ per CFW and need a real device to verify.
};

/**
 * Apply one config section to a live map. A listed button loses its own entries, then takes each
 * input it lists from whichever button holds it. Buttons go in enum order, so when the FILE lists one
 * input under two buttons the later one wins — reported, since only one of the two lines can work.
 * Returns how many buttons the section listed.
 */
template <typename Code, typename Resolve>
int apply_bindings(std::vector<std::pair<Code, Button>>& map, const ButtonBindings& cfg,
                   const char* section, const char* what, Resolve resolve, int& skipped) {
    std::vector<std::pair<Code, const std::string*>> codes[static_cast<size_t>(Button::COUNT)];
    int listed = 0;
    for (int i = 0; i < static_cast<int>(Button::COUNT); ++i) {
        const Button b = static_cast<Button>(i);
        if (!cfg[b]) continue;
        ++listed;
        map.erase(std::remove_if(map.begin(), map.end(),
                                 [b](const std::pair<Code, Button>& e) { return e.second == b; }),
                  map.end());
        for (const std::string& name : *cfg[b]) {
            Code c{};
            if (!resolve(name, c)) {
                // ⚠️ Reported, never silently skipped — see the header.
                std::printf("config:   %s.%s: \"%s\" is not an SDL %s name - skipped\n", section,
                            button_name(b), name.c_str(), what);
                ++skipped;
                continue;
            }
            codes[i].emplace_back(c, &name);
        }
    }

    for (int i = 0; i < static_cast<int>(Button::COUNT); ++i) {
        const Button b = static_cast<Button>(i);
        for (const auto& [c, name] : codes[i]) {
            for (auto it = map.begin(); it != map.end();) {
                if (it->first != c) { ++it; continue; }
                if (it->second != b && cfg[it->second]) {
                    std::printf("config:   %s: \"%s\" is listed under both %s and %s - %s gets it\n",
                                section, name->c_str(), button_name(it->second), button_name(b),
                                button_name(b));
                }
                it = map.erase(it);
            }
            map.emplace_back(c, b);
        }
    }
    return listed;
}

// The two triggers, bindable like buttons. SDL reports them as AXES, so they get codes past its last
// button; `pad_from_name` and the axis handler are the only places that know.
const auto PAD_LEFT_TRIGGER  = static_cast<SDL_GameControllerButton>(SDL_CONTROLLER_BUTTON_MAX);
const auto PAD_RIGHT_TRIGGER = static_cast<SDL_GameControllerButton>(SDL_CONTROLLER_BUTTON_MAX + 1);

bool pad_from_name(const std::string& name, SDL_GameControllerButton& out) {
    const SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(name.c_str());
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) { out = PAD_LEFT_TRIGGER; return true; }
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) { out = PAD_RIGHT_TRIGGER; return true; }
    out = SDL_GameControllerGetButtonFromString(name.c_str());
    return out != SDL_CONTROLLER_BUTTON_INVALID;
}

/** SDL returns NULL for an enum it does not recognise, and "%s" with NULL is UB. Never trust it. */
const char* or_unknown(const char* s) { return s ? s : "?"; }

// The always-repeatable buttons are the D-pad; `is_dpad` lives beside the enum in ui/buttons.h.
// B joins them only under `set_b_repeatable` — see press().

}  // namespace

SdlInput::SdlInput() {
    keyMap_.reserve(std::size(KEY_DEFAULTS));
    for (const KeyDefault& d : KEY_DEFAULTS) keyMap_.emplace_back(d.key, d.button);
    padMap_.reserve(std::size(PAD_DEFAULTS));
    for (const PadDefault& d : PAD_DEFAULTS) padMap_.emplace_back(d.pad, d.button);
}

ButtonBindings SdlInput::default_gamepad_bindings() {
    ButtonBindings out;
    for (const PadDefault& d : PAD_DEFAULTS) {
        auto& slot = out[d.button];
        if (!slot) slot.emplace();
        slot->emplace_back(or_unknown(SDL_GameControllerGetStringForButton(d.pad)));
    }
    return out;
}

ButtonBindings SdlInput::default_keyboard_bindings() {
    ButtonBindings out;
    for (const KeyDefault& d : KEY_DEFAULTS) {
        auto& slot = out[d.button];
        if (!slot) slot.emplace();
        slot->emplace_back(or_unknown(SDL_GetKeyName(d.key)));
    }
    return out;
}

bool SdlInput::on_miyoo_mini() {
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver && SDL_strcasecmp(driver, "mmiyoo") == 0;
}

void SdlInput::apply_input_config(const pt::ui::InputConfig& cfg) {
    abxy_ = cfg.abxy;
    if (abxy_ != AbxyLayout::AUTO) {
        std::printf("config:   controller abxy = %s\n", pt::ui::abxy_name(abxy_));
    }

    // ⚠️ ONE SUMMARY LINE PER SECTION, NOT ONE PER BUTTON: the seeded template lists all ten at
    // their defaults, so per-button lines would claim a change on every launch of an untouched
    // install. Which input gave which button is the input trace's job (`set_trace`).
    // ⚠️ THE MIYOO MINI'S BUTTONS ARRIVE AS A PAD, NEVER AS KEYS: its SDL posts keys only while no
    // controller subsystem is up (SDL_MMIYOO_INPUT_MODE), and ours is. Its keyboard mode is no answer —
    // there SELECT fires only as a tap on release, and SELECT is a held modifier here. Its config files
    // name the buttons in `keyboard`, so on the Mini that section is applied to the PAD; `gamepad`
    // wins for a button both list.
    ButtonBindings pad      = cfg.gamepad;
    const bool     miyoo    = on_miyoo_mini();
    if (miyoo) {
        for (int i = 0; i < static_cast<int>(Button::COUNT); ++i) {
            const Button b = static_cast<Button>(i);
            if (cfg.keyboard[b] && !pad[b]) pad[b] = cfg.keyboard[b];
        }
        pt::ui::miyoo_mini_to_pad_names(pad);
        std::printf("config:   Miyoo Mini: \"keyboard\" is read as its buttons (A B X Y L1 L2 R1 R2 ...)\n");
    }

    int skipped = 0;
    if (!miyoo) {
        const int keys = apply_bindings(
            keyMap_, cfg.keyboard, "keyboard", "key",
            [](const std::string& name, SDL_Keycode& out) {
                out = SDL_GetKeyFromName(name.c_str());
                return out != SDLK_UNKNOWN;
            },
            skipped);
        if (keys > 0) {
            std::printf("config:   keyboard: %d button(s) from config.json, %d key name(s) rejected\n",
                        keys, skipped);
        }
    }

    skipped = 0;
    const int pads = apply_bindings(padMap_, pad, "gamepad", "controller button", pad_from_name, skipped);
    if (pads > 0) {
        std::printf("config:   gamepad: %d button(s) from config.json, %d name(s) rejected\n", pads,
                    skipped);
    }

    repeatDelayMs_ = static_cast<uint64_t>(cfg.repeat.delay);
    repeatFastMs_  = static_cast<uint64_t>(cfg.repeat.interval);
    if (cfg.repeat.delay != pt::ui::RepeatConfig::DEFAULT_DELAY ||
        cfg.repeat.interval != pt::ui::RepeatConfig::DEFAULT_INTERVAL) {
        std::printf("config:   repeat: delay %d ms, interval %d ms\n", cfg.repeat.delay,
                    cfg.repeat.interval);
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
    // other way, so the raw button is turned into its PRINTED name first. ⚠️ BOTH PAIRS SWAP
    // TOGETHER, or a config line naming "x" would mean the wrong button.
    auto pad = static_cast<SDL_GameControllerButton>(b);
    if (abxy_ == AbxyLayout::NINTENDO) {
        switch (pad) {
            case SDL_CONTROLLER_BUTTON_A: pad = SDL_CONTROLLER_BUTTON_B; break;
            case SDL_CONTROLLER_BUTTON_B: pad = SDL_CONTROLLER_BUTTON_A; break;
            case SDL_CONTROLLER_BUTTON_X: pad = SDL_CONTROLLER_BUTTON_Y; break;
            case SDL_CONTROLLER_BUTTON_Y: pad = SDL_CONTROLLER_BUTTON_X; break;
            default: break;
        }
    }

    for (const std::pair<SDL_GameControllerButton, Button>& e : padMap_) {
        if (e.first == pad) { out = e.second; return true; }
    }
    return false;
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
        repeatNextMs_  = now_ms + repeatDelayMs_;
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
            // A trigger is a button here, pressed past half-way. The lower release line keeps an
            // analog trigger resting near the edge from chattering.
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                const bool left = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT;
                bool&      held = triggerHeld_[left ? 0 : 1];
                const bool down = e.caxis.value > (held ? 8000 : 16000);
                if (down == held) break;
                held = down;
                const bool mapped = pad_to_button(left ? PAD_LEFT_TRIGGER : PAD_RIGHT_TRIGGER, b);
                trace(down ? "PAD DOWN" : "PAD UP", left ? "lefttrigger" : "righttrigger", mapped, b);
                if (mapped) {
                    if (down) press(b, now);
                    else release(b);
                }
                break;
            }
            // Sticks are dropped. This only adds VISIBILITY, so "they are inert" is not an absence
            // (ignored? never sent? wedged?). A flood of these means a stick drifting hard.
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

uint64_t SdlInput::repeat_interval(uint64_t repeating_ms) const {
    if (repeating_ms >= REPEAT_RAMP_MS) return repeatFastMs_;
    const uint64_t slow = 2 * repeatFastMs_;
    return slow - repeatFastMs_ * repeating_ms / REPEAT_RAMP_MS;
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
    repeatActive_   = false;
    triggerHeld_[0] = triggerHeld_[1] = false;
}
