#include "sdl/sdl-input.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

using pt::ui::AbxyLayout;
using pt::ui::button_name;
using pt::ui::ButtonBindings;

namespace {

// The built-in keyboard map. A table because config.json's template is generated from it, so the file
// tells the user what the app dispatches. Each button's primary key first.
struct KeyDefault {
    SDL_Keycode key;
    Button      button;
};

constexpr KeyDefault KEY_DEFAULTS[] = {
    // D-pad: WASD and the arrow keys
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

// The built-in pad map by printed name (the ABXY swap is applied before lookup); a table for the same
// reason as `KEY_DEFAULTS`.
struct PadDefault {
    SDL_GameControllerButton pad;
    Button                   button;
};

constexpr PadDefault PAD_DEFAULTS[] = {
    {SDL_CONTROLLER_BUTTON_DPAD_UP,    Button::DPAD_UP},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN,  Button::DPAD_DOWN},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT,  Button::DPAD_LEFT},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, Button::DPAD_RIGHT},

    // X and Y alias A and B: face-button layouts differ across handhelds, and listening to two of four
    // is one bad SDL mapping from unusable.
    {SDL_CONTROLLER_BUTTON_A, Button::A}, {SDL_CONTROLLER_BUTTON_X, Button::A},
    {SDL_CONTROLLER_BUTTON_B, Button::B}, {SDL_CONTROLLER_BUTTON_Y, Button::B},

    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  Button::L_SHIFT},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, Button::R_SHIFT},
    {SDL_CONTROLLER_BUTTON_BACK,          Button::SELECT},
    {SDL_CONTROLLER_BUTTON_START,         Button::START},

    // ⚠️ No default for L2/R2 (bindable as lefttrigger/righttrigger) or the stick: axes that differ
    // per CFW.
};

// One config section onto a live map: a listed button loses its own entries, then takes each input it
// lists from whoever holds it. One input listed under two buttons goes to the later in enum order, and
// is reported. Returns how many buttons the section listed.
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

// The triggers are axes in SDL, so they get codes past its last button.
const auto PAD_LEFT_TRIGGER  = static_cast<SDL_GameControllerButton>(SDL_CONTROLLER_BUTTON_MAX);
const auto PAD_RIGHT_TRIGGER = static_cast<SDL_GameControllerButton>(SDL_CONTROLLER_BUTTON_MAX + 1);

bool pad_from_name(const std::string& name, SDL_GameControllerButton& out) {
    const SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(name.c_str());
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) { out = PAD_LEFT_TRIGGER; return true; }
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) { out = PAD_RIGHT_TRIGGER; return true; }
    out = SDL_GameControllerGetButtonFromString(name.c_str());
    return out != SDL_CONTROLLER_BUTTON_INVALID;
}

// SDL returns NULL for an enum it does not know; "%s" with NULL is UB.
const char* or_unknown(const char* s) { return s ? s : "?"; }

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

    // One summary line per section: the seeded template lists all ten buttons, so per-button lines
    // would claim a change on every launch.
    // ⚠️ The Miyoo Mini's buttons arrive as a pad, never as keys (its SDL posts keys only with no
    // controller subsystem up, and there SELECT fires only on release). Its config names them under
    // `keyboard`, so on the Mini that section is applied to the pad; `gamepad` wins for a button both list.
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
    // ⚠️ Android's Back would close the app mid-edit; once trapped (android-main.cpp) it is B, the
    // universal cancel. Hard-wired ahead of the map, so rebinding B on a desktop cannot take it away.
    if (k == SDLK_AC_BACK) { out = Button::B; return true; }

    for (const std::pair<SDL_Keycode, Button>& e : keyMap_) {
        if (e.first == k) { out = e.second; return true; }
    }
    return false;
}

bool SdlInput::pad_to_button(Uint8 b, Button& out) const {
    // NINTENDO: turn the raw button into its printed name first. ⚠️ Both pairs swap together, or a
    // config line naming "x" would mean the wrong button.
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
        // ⚠️ Dropped (OS auto-repeat too). On a handheld it means a second source is pressing — an
        // injected copy (gptokeyb) is absorbed silently here.
        if (trace_) {
            std::printf("input:              ^ ABSORBED: %s was already held - a SECOND source pressed it\n",
                        button_name(b));
        }
        return;
    }
    held_[i] = true;
    // After the flag, so a press of A reports A as held.
    queue_.push_back({b, ButtonAction::PRESSED, mods_now()});

    // B repeats only while `set_b_repeatable` (the qwerty backspace); elsewhere it fires once.
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

    // Releasing the repeating button, or A or B (the modifiers that gave it its meaning), cancels it.
    if (repeatActive_ && (b == repeatButton_ || b == Button::A || b == Button::B)) {
        repeatActive_ = false;
    }
}

void SdlInput::trace(const char* source, const char* what, bool mapped, Button b) const {
    if (!trace_) return;
    // ⚠️ The unmapped line matters most: it turns "the stick does nothing" from an absence into a
    // reading, and the mapped lines beside it prove the trace is alive.
    std::printf("input:   %-10s %-24s -> %s\n", source, what,
                mapped ? button_name(b) : "(ignored: not mapped)");
}

void SdlInput::handle_event(const SDL_Event& e, uint64_t now) {
    Button b{};

    switch (e.type) {
        case SDL_KEYDOWN:
            // The OS repeat is dropped: the app's repeat must match a D-pad's.
            if (e.key.repeat != 0) break;
            {
                const bool mapped = key_to_button(e.key.keysym.sym, b);
                // ⚠️ A handheld should produce no key events; one here names a layer injecting them.
                trace("KEYDOWN", or_unknown(SDL_GetKeyName(e.key.keysym.sym)), mapped, b);
                if (mapped) press(b, now);
            }
            break;

        case SDL_KEYUP:
            if (key_to_button(e.key.keysym.sym, b)) release(b);
            break;

        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            const bool down   = e.type == SDL_CONTROLLERBUTTONDOWN;
            const bool mapped = pad_to_button(e.cbutton.button, b);
            trace(down ? "PAD DOWN" : "PAD UP",
                  or_unknown(SDL_GameControllerGetStringForButton(
                      static_cast<SDL_GameControllerButton>(e.cbutton.button))),
                  mapped, b);
            if (mapped) {
                if (down) press(b, now);
                else release(b);
            }
            break;
        }

        case SDL_CONTROLLERAXISMOTION: {
            // A trigger is a button past half-way; the lower release line stops it chattering.
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
            // Sticks are dropped; the trace line makes "inert" a reading, not an absence.
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
            // ⚠️ A removed pad sends no button-ups, so `reset()` releases what it held (a sleeping pad
            // with L down turns every D-pad press into a screen change). The handle is closed so
            // `controller_count()` is right. `which` is an instance id on removal, an index on ADDED.
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

    // One repeat per tick, the next deadline from now — a catch-up loop would jump a held A+UP by
    // several after a stall. Modifiers as they stand now: pressing A mid-repeat switches to editing.
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
    // ⚠️ Held buttons are released, not forgotten: consumers act on releases (the FX helper closes on
    // A's). Through `release()`, so it looks exactly like the lost key-up.
    queue_.clear();   // presses for a window that no longer has focus
    for (size_t i = 0; i < static_cast<size_t>(Button::COUNT); ++i)
        if (held_[i]) release(static_cast<Button>(i));
    repeatActive_   = false;
    triggerHeld_[0] = triggerHeld_[1] = false;
}
