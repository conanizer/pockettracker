// Keyboard and gamepad events → presses and releases of the ten buttons, plus the key repeat. The
// only platform-specific part of the input chain; meanings are `ui/button_mapper.h`.
// Default keys: WASD/arrows, K/Enter = A, J/Esc = B, U/I = shoulders, LShift = SELECT, Space = START.

#ifndef POCKETTRACKER_SDL_INPUT_H
#define POCKETTRACKER_SDL_INPUT_H

#include <cmath>  // before <SDL.h> — see sdl-audio-engine.h

#include <SDL.h>

#include "ui/buttons.h"
#include "ui/input_config.h"

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

// The button model is pt::ui's; nothing else in the shell is called `Button`.
using pt::ui::Button;
using pt::ui::ButtonAction;
using pt::ui::ButtonEvent;
using pt::ui::ButtonMods;

class SdlInput {
public:
    SdlInput();

    // Safe with none attached.
    void open_controllers();
    void close_controllers();

    // The built-in keyboard map as key names — config.json's `keyboard` seed. It walks the table
    // `key_to_button` uses, so the template cannot claim a binding the app does not have.
    static pt::ui::ButtonBindings default_keyboard_bindings();

    // The built-in pad map as SDL controller button names — config.json's `gamepad` seed.
    static pt::ui::ButtonBindings default_gamepad_bindings();

    // The Miyoo Mini, told by its SDL fork's `mmiyoo` video driver. ⚠️ After SDL_Init.
    static bool on_miyoo_mini();

    // config.json's `controller`, `keyboard`, `gamepad`, `repeat`; once, before the first event.
    // Unmentioned buttons keep their defaults. ⚠️ An unresolvable name is printed and skipped, never
    // dropped silently.
    void apply_input_config(const pt::ui::InputConfig& cfg);

    // The face-button swap; config.json seeds it, the SETTINGS row sets it every frame.
    void set_abxy(pt::ui::AbxyLayout layout) { abxy_ = layout; }

    // POCKETTRACKER_INPUT_TRACE: one line per event — what SDL delivered and the Button it became, or
    // why none. Nothing else covers the layer between the hardware and a ButtonEvent.
    void set_trace(bool on) { trace_ = on; }

    // Presses/releases land in the queue; the clock is passed in so repeat timing is testable.
    void handle_event(const SDL_Event& e, uint64_t now_ms);

    // Once per loop tick: the repeat presses, a delay then a tightening train (`repeat_interval`).
    void tick(uint64_t now_ms);

    // Drain one queued event; false when empty.
    bool poll(ButtonEvent& out);

    bool is_held(Button b) const { return held_[static_cast<size_t>(b)]; }

    // Focus loss: drop the repeat and release every held button — a stuck modifier reroutes every
    // later press.
    void reset();

    // A touch button uses the same press/release, repeat and held-button de-dup as a key or a pad.
    void touch_press(Button b, uint64_t now_ms) { press(b, now_ms); }
    void touch_release(Button b)                { release(b); }

    // The shell's FULL-vs-touch-layout question.
    size_t controller_count() const { return controllers_.size(); }

    // A held B repeats only in the on-screen keyboard (backspace); elsewhere B is COPY, BACK, CANCEL
    // and a modifier, which must not fire on a resting thumb. ⚠️ Turning it off also disarms a B repeat
    // in flight. The shell restates it every frame from the overlay flag.
    void set_b_repeatable(bool on) {
        bRepeatable_ = on;
        if (!on && repeatActive_ && repeatButton_ == Button::B) repeatActive_ = false;
    }

private:
    void press(Button b, uint64_t now_ms);
    void release(Button b);

    // Against the live map (defaults, as edited by config.json).
    bool key_to_button(SDL_Keycode k, Button& out) const;

    // Against the live pad map, after the face-button swap.
    bool pad_to_button(Uint8 b, Button& out) const;

    // The modifiers right now — stamped onto each event as it is queued.
    ButtonMods mods_now() const;

    // `mapped` false prints why it went nowhere.
    void trace(const char* source, const char* what, bool mapped, Button b) const;

    // A held button speeds up but never moves more than one step at a time (a multiplied step reads
    // as a stutter): the train starts at 2× the interval and ramps linearly to it over REPEAT_RAMP_MS.
    // ⚠️ At most one repeat per `tick`, so the cadence rounds up to the poll period (4 ms).
    static constexpr uint64_t REPEAT_RAMP_MS = 1200;

    uint64_t repeatDelayMs_ = pt::ui::RepeatConfig::DEFAULT_DELAY;     // before the first repeat
    uint64_t repeatFastMs_  = pt::ui::RepeatConfig::DEFAULT_INTERVAL;  // once the ramp has run out

    uint64_t repeat_interval(uint64_t repeating_ms) const;

    bool held_[static_cast<size_t>(Button::COUNT)] = {false};
    bool triggerHeld_[2] = {false, false};   // left, right — triggers arrive as axes

    // One repeat at a time: a DPAD button, or B while repeatable; a D-pad press steals the slot.
    // Modifiers are re-read on each repeat; releasing A or B cancels it.
    bool     repeatActive_ = false;
    Button   repeatButton_ = Button::DPAD_UP;
    uint64_t repeatNextMs_ = 0;

    // The press plus the initial delay — the ramp is measured from here.
    uint64_t repeatTrainMs_ = 0;

    bool bRepeatable_ = false;

    bool trace_ = false;

    // Seeded from `KEY_DEFAULTS`, then per button replaced by config.json; a key appears at most once.
    std::vector<std::pair<SDL_Keycode, Button>> keyMap_;

    // Likewise from `PAD_DEFAULTS`; keyed by the printed name (the ABXY swap is applied before lookup).
    std::vector<std::pair<SDL_GameControllerButton, Button>> padMap_;

    // How the pad's face buttons are printed (ui/input_config.h).
    pt::ui::AbxyLayout abxy_ = pt::ui::AbxyLayout::AUTO;

    std::deque<ButtonEvent>     queue_;
    std::vector<SDL_GameController*> controllers_;
};

#endif  // POCKETTRACKER_SDL_INPUT_H
