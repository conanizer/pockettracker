#include "sdl-input.h"

#include <cstdio>

namespace {

/** Keyboard → button. Copied key-for-key from InputMapper's `keyboardMapping`. */
bool key_to_button(SDL_Keycode k, Button& out) {
    switch (k) {
        // D-pad: WASD (the PC-gamer cluster) and the arrow keys
        case SDLK_w: case SDLK_UP:    out = Button::DPAD_UP;    return true;
        case SDLK_s: case SDLK_DOWN:  out = Button::DPAD_DOWN;  return true;
        case SDLK_a: case SDLK_LEFT:  out = Button::DPAD_LEFT;  return true;
        case SDLK_d: case SDLK_RIGHT: out = Button::DPAD_RIGHT; return true;

        // Face buttons: right-hand home row, plus Enter/Escape
        case SDLK_k: case SDLK_RETURN: out = Button::A; return true;
        case SDLK_j: case SDLK_ESCAPE: out = Button::B; return true;

        // Shoulders: the keys above the face buttons
        case SDLK_u: out = Button::L_SHIFT; return true;
        case SDLK_i: out = Button::R_SHIFT; return true;

        // System
        case SDLK_LSHIFT: out = Button::SELECT; return true;
        case SDLK_SPACE:  out = Button::START;  return true;

        default: return false;
    }
}

/** Controller → button. */
bool pad_to_button(Uint8 b, Button& out) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    out = Button::DPAD_UP;    return true;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  out = Button::DPAD_DOWN;  return true;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  out = Button::DPAD_LEFT;  return true;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: out = Button::DPAD_RIGHT; return true;

        // X and Y are aliased onto A and B on purpose: the physical face-button layout differs
        // across the handhelds this ships to, and a four-button app that only listens to two of them
        // is one bad SDL mapping away from being unusable. The port plan asks for exactly this.
        case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_X: out = Button::A; return true;
        case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_Y: out = Button::B; return true;

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

/** DPAD buttons are the only repeatable ones — every `startKeyRepeat` call site is a DPAD press. */
bool is_dpad(Button b) {
    return b == Button::DPAD_UP || b == Button::DPAD_DOWN || b == Button::DPAD_LEFT ||
           b == Button::DPAD_RIGHT;
}

/** SDL returns NULL for an enum it does not recognise, and "%s" with NULL is UB. Never trust it. */
const char* or_unknown(const char* s) { return s ? s : "?"; }

/** Names for the trace only. Indexed by Button, so it must track the enum's order. */
const char* button_name(Button b) {
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

}  // namespace

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
            std::printf("input:              ^ ABSORBED: %s was already held — a SECOND source pressed it\n",
                        button_name(b));
        }
        return;
    }
    held_[i] = true;
    // AFTER the flag is set, so a press of A itself reports A as held — which is what Kotlin's
    // `handleButtonAction` sees, since it updates the modifier state before it resolves the combo.
    queue_.push_back({b, ButtonAction::PRESSED, mods_now()});

    if (is_dpad(b)) {
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
    for (bool& h : held_) h = false;
    repeatActive_ = false;
    queue_.clear();
}
