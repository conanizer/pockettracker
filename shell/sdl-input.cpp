#include "sdl-input.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

using pt::ui::AbxyLayout;
using pt::ui::button_name;
using pt::ui::KeyboardBindings;

namespace {

/**
 * The built-in keyboard map. Copied key-for-key from InputMapper's `keyboardMapping`.
 *
 * A TABLE rather than the switch it used to be, because it now has a second reader: the config.json
 * starter template is generated from it (`default_keyboard_bindings`), so what the file tells the user
 * their keys are is derived from the same rows the app dispatches through. A switch can be read by the
 * compiler and by a human, and by nothing else.
 *
 * Order matters in one narrow way: `default_keyboard_bindings` emits each button's names in the order
 * they appear here, so the primary key of a pair should come first — the template reads as
 * `"A": ["K", "Return"]`, which is the order a user thinks in.
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

    // ⚠️ ONE SUMMARY LINE, NOT ONE PER BUTTON, and the count is what makes it worth printing.
    //
    // The seeded template lists all ten buttons at their defaults, so the common case is "every button
    // present, nothing actually different". Ten lines saying `rebound` for that is worse than silence:
    // it claims a change on every launch of an untouched install, and a log that cries wolf is a log
    // nobody reads on the launch that matters. Which key produced which button is the INPUT TRACE's
    // job (`set_trace`), and it answers it per press, live.
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
                // ⚠️ Reported, never silently skipped. See the header: the failure mode this prevents
                // is a user re-reading their own correct-looking JSON for an hour.
                std::printf("config:   keyboard.%s: \"%s\" is not an SDL key name - skipped\n",
                            button_name(b), name.c_str());
                ++skipped;
                continue;
            }
            keyMap_.emplace_back(k, b);
        }
    }

    // Unconditional whenever the section was present at all — a component whose correct behaviour is
    // silence cannot be told from one that never ran. `skipped` is on the same line as the count so a
    // partially-applied file announces itself rather than hiding behind a plausible-looking total.
    if (bound > 0) {
        std::printf("config:   keyboard: %d button(s) from config.json, %d key name(s) rejected\n",
                    bound, skipped);
    }
}

bool SdlInput::key_to_button(SDL_Keycode k, Button& out) const {
    // ⚠️ **ANDROID'S BACK BUTTON, AND WITHOUT THIS LINE IT CLOSES THE APP MID-EDIT** (C4).
    //
    // It arrives as an ordinary key once `SDL_HINT_ANDROID_TRAP_BACK_BUTTON` is set — see
    // android-main.cpp, which is where the trap has to be armed, because the UNTRAPPED default is
    // `SDLActivity.onBackPressed()` finishing the activity out from under the frame loop.
    //
    // B, not SELECT and not a quit: B is already this app's universal cancel — it closes the file
    // browser, aborts the keyboard, leaves the EQ and theme editors — so the gesture a phone user
    // arrives with maps onto the verb the UI already has. The app is still leavable by Home (the
    // watcher in app.cpp saves) and by PROJECT > EXIT (`PlatformCaps::sdl().appExit`).
    //
    // ⚠️ HARD-WIRED, AHEAD OF THE CONFIGURABLE MAP, AND DELIBERATELY NOT IN `KEY_DEFAULTS` — so it is
    // neither listed in the starter template nor removable by rebinding B. A user who rebinds B on the
    // desktop build has no idea they are also holding the only way to back out of a screen on Android;
    // config.json must not be able to brick a platform it was not edited on.
    //
    // Harmless on every other platform: no desktop keyboard produces AC_BACK.
    if (k == SDLK_AC_BACK) { out = Button::B; return true; }

    for (const std::pair<SDL_Keycode, Button>& e : keyMap_) {
        if (e.first == k) { out = e.second; return true; }
    }
    return false;
}

bool SdlInput::pad_to_button(Uint8 b, Button& out) const {
    // Which face-button pair means A. See `ui/input_config.h` for why this is a user setting and not
    // something SDL can answer: with NINTENDO the pad's labels run the other way round, so the pair
    // that means A is the one SDL is calling B/Y.
    //
    // ⚠️ BOTH PAIRS SWAP TOGETHER. Swapping only A↔B would leave the X/Y aliases below still pointing
    // the old way, so two of the four face buttons would quietly keep the wrong meaning — the exact
    // half-fix that reads as "it works now" until someone uses the other two buttons.
    const bool nintendo = (abxy_ == AbxyLayout::NINTENDO);

    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    out = Button::DPAD_UP;    return true;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  out = Button::DPAD_DOWN;  return true;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  out = Button::DPAD_LEFT;  return true;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: out = Button::DPAD_RIGHT; return true;

        // X and Y are aliased onto A and B on purpose: the physical face-button layout differs
        // across the handhelds this ships to, and a four-button app that only listens to two of them
        // is one bad SDL mapping away from being unusable. The port plan asks for exactly this.
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

        // ⚠️ Not mapped, and both wait for a real device (Phase 4 bring-up): the L2/R2 TRIGGERS
        // aliased onto L/R, and the analog stick deadzoned onto the D-pad. Both are axes, both differ
        // per CFW, and neither can be verified on a keyboard — writing them blind is how an input
        // layer ships broken.
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
        // ⚠️ THE LINE P4b NEEDED AND DID NOT HAVE. Dropping a repeat press is correct — the OS auto-
        // repeat is ignored and the 400/100 ms one below is ours — but this early return is also what
        // made a launcher bug nearly unfindable: with gptokeyb injecting a second copy of every press,
        // three of the four collisions were ABSORBED right here, in silence, because the two paths
        // happened to agree. Only START, where they disagreed, was ever reported — so the de-dup that
        // hides a fault is the same de-dup that makes it look like a sequencer bug.
        //
        // A press arriving for a button already down is therefore not noise: on a handheld, where one
        // physical button should produce exactly one press, it means SOMETHING ELSE is pressing too.
        if (trace_) {
            std::printf("input:              ^ ABSORBED: %s was already held - a SECOND source pressed it\n",
                        button_name(b));
        }
        return;
    }
    held_[i] = true;
    // AFTER the flag is set, so a press of A itself reports A as held — which is what Kotlin's
    // `handleButtonAction` sees, since it updates the modifier state before it resolves the combo.
    queue_.push_back({b, ButtonAction::PRESSED, mods_now()});

    // B joins the D-pad only while `set_b_repeatable` says so — the qwerty overlay, where B is a
    // backspace. Everywhere else B is COPY / BACK / CANCEL and must fire exactly once per press.
    if (is_dpad(b) || (b == Button::B && bRepeatable_)) {
        repeatActive_ = true;
        repeatButton_ = b;
        repeatNextMs_ = now_ms + REPEAT_INITIAL_DELAY;
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
            // e.key.repeat: the OS repeat, which is deliberately dropped. The app's repeat cadence is
            // 400/100 ms and must be the same on a keyboard and on a handheld's D-pad, where there is
            // no OS repeat at all.
            if (e.key.repeat != 0) break;
            {
                const bool mapped = key_to_button(e.key.keysym.sym, b);
                // ⚠️ A handheld should produce NO keyboard events at all. One appearing here is the
                // P4b signature — some layer (a gptokeyb that crept back into the launch script, a CFW
                // hotkey daemon) injecting phantom input the app never asked for. That bug cost a
                // device session and read as a sequencer fault; this line is how it names itself.
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
            // ⚠️ THERE IS DELIBERATELY NO MAPPING HERE, AND THIS ARM ADDS NONE — it existed as
            // `default: break;` and still drops every axis on the floor. What it adds is VISIBILITY:
            // the L2/R2 triggers and both analog sticks arrive as axes (the CFW's mapping binds
            // `lefttrigger:a2`, `leftx:a0`), so without this line the sweep's "they are inert" row
            // could only ever observe an absence — and an absence is equally consistent with the app
            // ignoring them, the device never sending them, and the app being wedged.
            //
            // A flood of these IS a finding, not noise: it means a stick is drifting hard enough to
            // spam the event queue, which is the axis version of P4b's "a drifting stick moves the
            // cursor".
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
            // ⚠️ **A REMOVED PAD SENDS NO BUTTON-UPS, so every button it had down stays "held"** —
            // and `mods_now()` reads A, B, L, R and SELECT straight out of `held_`. A wireless pad
            // going to sleep with L down turns every later D-pad press into a screen change, and on a
            // fullscreen handheld the only way out is to background the app. `reset()` releases them
            // properly; it is the same repair focus loss needs, for the same reason.
            //
            // ⚠️ And the handle must be CLOSED and dropped, not just forgotten: `controller_count()`
            // is what `compute_has_pad()` falls back to on every platform but Android, so a list that
            // only ever grows means the on-screen controls never come back on a touch device — and a
            // pad unplugged and replugged N times leaks N handles until quit.
            //
            // `e.cdevice.which` is an INSTANCE id on removal (a joystick index only on ADDED), so the
            // handle is found by asking each open controller for its own instance id.
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
            // Focus loss eats the KEYUPs, and a modifier that is stuck "held" reroutes every later
            // DPAD press into the wrong combo. Kotlin hit the identical bug through Compose
            // cancelling its pointer coroutines without firing RELEASED, and fixed it the same way.
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) reset();
            break;

        default:
            break;
    }
}

void SdlInput::tick(uint64_t now_ms) {
    if (!repeatActive_ || now_ms < repeatNextMs_) return;

    // ONE repeat per frame, and the next deadline is measured from NOW rather than from the missed
    // one. A catch-up loop here would be a bug with teeth: stall the loop for half a second — drag the
    // window, hit a slow frame on an A53 — and it would flush five queued repeats in a single frame,
    // so a held A+UP would jump the value by 5 in one go. "At least 100 ms apart, quantised to the
    // frame" is also what Kotlin's Handler.postDelayed actually delivers, since its repeat is posted
    // to the same main-thread message queue the UI is draining.
    // The repeat carries the modifiers as they stand NOW, not as they stood when the D-pad went down.
    // That is deliberate and it is Kotlin's behaviour: press A after UP is already repeating and the
    // repeat starts editing rather than moving, with no need to remember what began it.
    queue_.push_back({repeatButton_, ButtonAction::PRESSED, mods_now()});
    repeatNextMs_ = now_ms + REPEAT_INTERVAL;
}

bool SdlInput::poll(ButtonEvent& out) {
    if (queue_.empty()) return false;
    out = queue_.front();
    queue_.pop_front();
    return true;
}

void SdlInput::reset() {
    // ⚠️ **A HELD BUTTON IS RELEASED, NOT MERELY FORGOTTEN.** A release is not bookkeeping — it is an
    // event consumers act on, and the FX-helper overlay's ONLY close is `on_a_released()`. Dropping it
    // leaves a full-screen picker up that B cannot dismiss and that `any_modal_open()` does not cover,
    // so the D-pad goes on moving the cursor invisibly behind the backdrop and the A press that
    // finally closes it commits the held effect code into whatever cell the cursor reached. The
    // mapper's two deferred-single latches discharge on a release too.
    //
    // Emitted HERE rather than repaired at each consumer, so a consumer written later gets it for
    // free — and through `release()` itself, so a synthesised release is indistinguishable from the
    // key-up that focus loss ate, repeat cancellation included.
    queue_.clear();   // this frame's presses belong to a window that no longer has focus
    for (size_t i = 0; i < static_cast<size_t>(Button::COUNT); ++i)
        if (held_[i]) release(static_cast<Button>(i));
    repeatActive_ = false;
}
