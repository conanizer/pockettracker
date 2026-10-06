// sdl-input.{h,cpp} — raw SDL input → the virtual button model.
//
// Turns keyboard and gamepad events into presses and releases of the ten buttons, and owns the key
// repeat. What a button MEANS — the combo matrix — is `ui/button_mapper.h`, and what a meaning DOES
// is `ui/input_dispatcher.h`; this half (a keycode, a controller button, an axis) is the only
// platform-specific part of the input chain. The button model itself is `ui/buttons.h`.
//
// Default keyboard map: WASD/arrows, K/Enter = A, J/Esc = B, U/I = shoulders, LShift = SELECT,
// Space = START.

#ifndef POCKETTRACKER_SDL_INPUT_H
#define POCKETTRACKER_SDL_INPUT_H

#include <cmath>  // before <SDL.h> — see sdl-audio-engine.h (M_PI / C4005)

#include <SDL.h>

#include "ui/buttons.h"
#include "ui/input_config.h"

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

// The button model is `pt::ui`'s; named unqualified because nothing else in the shell is called
// `Button`. These four are the whole of the leak, and it stops at the shell.
using pt::ui::Button;
using pt::ui::ButtonAction;
using pt::ui::ButtonEvent;
using pt::ui::ButtonMods;

class SdlInput {
public:
    SdlInput();

    /** Open every attached controller. Safe to call with none — a dev box is keyboard-only. */
    void open_controllers();
    void close_controllers();

    /**
     * The built-in keyboard map, as key NAMES — what `config.json`'s `keyboard` section is seeded from.
     *
     * ⭐ It walks the same `KEY_DEFAULTS` table `key_to_button` dispatches through, so the template
     * cannot claim a binding the app does not have — a starter template must be TRUE.
     *
     * Names come from `SDL_GetKeyName`, so they round-trip through the `SDL_GetKeyFromName` that reads
     * them back. `SDLK_AC_BACK` is deliberately absent — see `apply_input_config`.
     */
    static pt::ui::ButtonBindings default_keyboard_bindings();

    /** The built-in pad map, as SDL's controller button names — config.json's `gamepad` seed. */
    static pt::ui::ButtonBindings default_gamepad_bindings();

    /**
     * True on the Miyoo Mini, told by the video driver: only that device's SDL fork has `mmiyoo`.
     * Its config names buttons its own way, and its template is the launcher's. ⚠️ After SDL_Init.
     */
    static bool on_miyoo_mini();

    /**
     * Apply config.json's input sections: `controller`, `keyboard`, `gamepad`, `repeat`.
     *
     * Call ONCE at boot, before the first event. Anything the config does not mention keeps its
     * built-in value, so this is safe to call with a default-constructed `InputConfig` (the no-file
     * case) and does nothing.
     *
     * ⚠️ An unresolvable name is REPORTED on stdout and skipped, never silently dropped — or the user
     * re-reads their correct-looking JSON instead of the line saying `Ctrl` is not a key name.
     */
    void apply_input_config(const pt::ui::InputConfig& cfg);

    /**
     * The face-button swap, live - the SETTINGS row calls this every frame.
     *
     * The same value `apply_input_config` seeds from config.json at boot; the row owns it from there.
     */
    void set_abxy(pt::ui::AbxyLayout layout) { abxy_ = layout; }

    /**
     * Print one line per input event: what SDL delivered, and what this class did with it — a Button,
     * or nothing at all. Off by default; the shell turns it on for POCKETTRACKER_INPUT_TRACE=1.
     *
     * ⚠️ Everything between the hardware and a `ButtonEvent` (SDL, the CFW's controller mapping, the
     * launch script) has no other coverage, and `press()` silently drops a button already held,
     * hiding duplicates. A claim like "L2/R2 and the stick are inert" passes by NOTHING HAPPENING;
     * the trace turns it into positive evidence — an axis line with no button after it — and the
     * working buttons printing their mapping on the same run prove the trace is live.
     */
    void set_trace(bool on) { trace_ = on; }

    /**
     * Feed one SDL event. Presses/releases land in the queue; everything else is ignored.
     *
     * The clock is passed IN: a press arms the key repeat, so behaviour is a function of time, and
     * an injected clock makes the press/release/repeat semantics testable exactly.
     */
    void handle_event(const SDL_Event& e, uint64_t now_ms);

    /**
     * Once per loop tick. Emits the synthetic repeat presses — a delay to the first, then a train
     * that TIGHTENS with the hold: see `repeat_interval`.
     */
    void tick(uint64_t now_ms);

    /** Drain one queued event; false when empty. */
    bool poll(ButtonEvent& out);

    bool is_held(Button b) const { return held_[static_cast<size_t>(b)]; }

    /** Window focus loss: drop any repeat in flight and RELEASE every held button (a stuck modifier
     *  reroutes every later DPAD press into the wrong combo, and an unseen release leaves the
     *  FX-helper overlay up forever). */
    void reset();

    /**
     * A virtual (touch) button feeds the SAME press/release machinery as a key or a pad, inheriting
     * the `ButtonMods` snapshot stamped at queue time, the one key-repeat engine and the held-button
     * de-dup (virtual A plus physical A collide safely). The finger hit-test is `sdl-touch.*`.
     */
    void touch_press(Button b, uint64_t now_ms) { press(b, now_ms); }
    void touch_release(Button b)                { release(b); }

    /** How many controllers are open — the shell uses it to choose FULL (physical buttons, no
     *  on-screen controls) vs a touch layout. */
    size_t controller_count() const { return controllers_.size(); }

    /**
     * Let a held B repeat, the way a held D-pad does. OFF everywhere except the qwerty overlay, and
     * that scope is the whole point: outside the keyboard B is COPY, BACK, CANCEL and the modifier of
     * B+DPAD, and none of those may fire sixty times because a thumb rested on the button. Inside the
     * keyboard B is a backspace, where holding to erase a word is the expected gesture.
     *
     * ⚠️ It also DISARMS a B repeat already in flight, so the overlay closing under a held B (B on the
     * last character applies nothing, but A does) cannot leave the repeat hammering whatever screen is
     * revealed. The shell re-states it every frame from the live overlay flag, so there is no
     * open/close call site that can forget to.
     */
    void set_b_repeatable(bool on) {
        bRepeatable_ = on;
        if (!on && repeatActive_ && repeatButton_ == Button::B) repeatActive_ = false;
    }

private:
    void press(Button b, uint64_t now_ms);
    void release(Button b);

    /** Keycode → button, against the LIVE map (defaults, as edited by config.json). */
    bool key_to_button(SDL_Keycode k, Button& out) const;

    /** Controller button → button, against the LIVE pad map, after the face-button layout swap. */
    bool pad_to_button(Uint8 b, Button& out) const;

    /** The modifiers as they stand right now — stamped onto each event as it is queued. */
    ButtonMods mods_now() const;

    /** One trace line. `mapped` false prints the reason it went nowhere. */
    void trace(const char* source, const char* what, bool mapped, Button b) const;

    // A held button speeds UP; it never moves more than one step at a time. That distinction is the
    // whole design: a multiplied step jumps 2 then 4 rows per frame, which reads as a stutter however
    // fast the frames come, while a tightening interval stays one row a step and simply arrives more
    // often. The train starts at 2 × the configured interval and ramps linearly down to it.
    // ⚠️ `tick` emits AT MOST ONE repeat per call, so the real cadence is the interval rounded up to
    // the loop's poll period (4 ms while input is arriving).
    static constexpr uint64_t REPEAT_RAMP_MS = 1200;  // ms of repeating to get from slow to fast

    uint64_t repeatDelayMs_ = pt::ui::RepeatConfig::DEFAULT_DELAY;     // ms before the first repeat
    uint64_t repeatFastMs_  = pt::ui::RepeatConfig::DEFAULT_INTERVAL;  // ms once the ramp has run out

    /** The gap to the next repeat, for a train that has been running `repeating_ms`. */
    uint64_t repeat_interval(uint64_t repeating_ms) const;

    bool held_[static_cast<size_t>(Button::COUNT)] = {false};
    bool triggerHeld_[2] = {false, false};   // left, right — triggers arrive as axes

    // ONE repeat at a time. A DPAD button always, plus B while `bRepeatable_` (the qwerty backspace);
    // a D-pad press mid-backspace steals the slot. The modifiers are re-read when the repeat fires, so
    // holding A+UP keeps editing and UP alone keeps moving. Releasing A or B cancels it — otherwise
    // letting go of A mid-repeat would keep hammering the edit.
    bool     repeatActive_ = false;
    Button   repeatButton_ = Button::DPAD_UP;
    uint64_t repeatNextMs_ = 0;

    /** When the repeat TRAIN starts — the press plus the initial delay, not the press. The ramp is
     *  measured from here, so the silent delay does not count towards it. */
    uint64_t repeatTrainMs_ = 0;

    /** Whether B currently counts as repeatable — see set_b_repeatable. */
    bool bRepeatable_ = false;

    bool trace_ = false;

    /**
     * The live keyboard map: seeded from `KEY_DEFAULTS`, then per-button REPLACED by config.json.
     *
     * A flat vector scanned linearly (~14 entries, once per key event). Each key appears at most
     * once: a config binding takes the key from whatever button held it.
     */
    std::vector<std::pair<SDL_Keycode, Button>> keyMap_;

    /** The live pad map, built the same way from `PAD_DEFAULTS` and config.json's `gamepad`. Keyed by
     *  the button's PRINTED name: `pad_to_button` applies the ABXY swap before looking it up. */
    std::vector<std::pair<SDL_GameControllerButton, Button>> padMap_;

    /** Which way round the pad's face buttons are printed. See `ui/input_config.h` — AUTO is right on
     *  every platform whose pad SDL can classify, which is every platform except a desktop with a
     *  third-party pad in XInput mode. */
    pt::ui::AbxyLayout abxy_ = pt::ui::AbxyLayout::AUTO;

    std::deque<ButtonEvent>     queue_;
    std::vector<SDL_GameController*> controllers_;
};

#endif  // POCKETTRACKER_SDL_INPUT_H
