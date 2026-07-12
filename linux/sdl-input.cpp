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

void SdlInput::press(Button b, uint64_t now_ms) {
    const size_t i = static_cast<size_t>(b);
    if (held_[i]) return;  // the OS auto-repeat is ignored; the repeat below is ours
    held_[i] = true;
    queue_.push_back({b, ButtonAction::PRESSED});

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
    queue_.push_back({b, ButtonAction::RELEASED});

    // Cancel the repeat when the repeating DPAD is let go — or when A or B is, because those are the
    // modifiers that gave it its meaning.
    if (repeatActive_ && (b == repeatButton_ || b == Button::A || b == Button::B)) {
        repeatActive_ = false;
    }
}

void SdlInput::handle_event(const SDL_Event& e, uint64_t now) {
    Button b{};

    switch (e.type) {
        case SDL_KEYDOWN:
            // e.key.repeat: the OS repeat, which is deliberately dropped. The app's repeat cadence is
            // 400/100 ms and must be the same on a keyboard and on a handheld's D-pad, where there is
            // no OS repeat at all.
            if (e.key.repeat == 0 && key_to_button(e.key.keysym.sym, b)) press(b, now);
            break;

        case SDL_KEYUP:
            if (key_to_button(e.key.keysym.sym, b)) release(b);
            break;

        case SDL_CONTROLLERBUTTONDOWN:
            if (pad_to_button(e.cbutton.button, b)) press(b, now);
            break;

        case SDL_CONTROLLERBUTTONUP:
            if (pad_to_button(e.cbutton.button, b)) release(b);
            break;

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
    queue_.push_back({repeatButton_, ButtonAction::PRESSED});
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
