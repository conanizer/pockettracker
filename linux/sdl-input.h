// sdl-input.{h,cpp} — raw SDL input → the virtual button model.
//
// The source half of input/ButtonHandlers.kt's `InputMapper`: it turns keyboard and gamepad events
// into presses and releases of the ten buttons the app actually knows about, and it owns the custom
// key repeat. What it deliberately does NOT do is decide what a button MEANS — the combo matrix
// (A+DPAD edits, B+DPAD cycles the item, L+B selection, R+DPAD screen navigation, the A,A double-tap,
// the press-vs-release deferral) belongs to `AppInputDispatcher`, which is ~3200 lines of Kotlin and
// lands with its own session of the port. Splitting there is not a shortcut: the mapper half is the
// only part that is platform-specific, and it is the only part in this directory.
//
// The keyboard map is copied from the Kotlin one exactly (WASD/arrows, K/Enter = A, J/Esc = B, U/I =
// shoulders, LShift = SELECT, Space = START), so a dev's muscle memory transfers between the two
// builds and a bug report about "the K key" means the same thing on both.

#ifndef POCKETTRACKER_SDL_INPUT_H
#define POCKETTRACKER_SDL_INPUT_H

#include <cmath>  // before <SDL.h> — see sdl-audio-engine.h (M_PI / C4005)

#include <SDL.h>

#include <cstdint>
#include <deque>
#include <vector>

/** The virtual gamepad. Identical to Kotlin's `VirtualButton` — every input source maps onto it. */
enum class Button {
    DPAD_UP,
    DPAD_DOWN,
    DPAD_LEFT,
    DPAD_RIGHT,
    A,
    B,
    L_SHIFT,
    R_SHIFT,
    SELECT,
    START,
    COUNT
};

enum class ButtonAction { PRESSED, RELEASED };

struct ButtonEvent {
    Button       button;
    ButtonAction action;
};

class SdlInput {
public:
    /** Open every attached controller. Safe to call with none — a dev box is keyboard-only. */
    void open_controllers();
    void close_controllers();

    /**
     * Feed one SDL event. Presses/releases land in the queue; everything else is ignored.
     *
     * The clock is passed IN rather than read from SDL_GetTicks64() inside, and that is not
     * ceremony: a press arms the key repeat, so the class's whole behaviour is a function of time,
     * and a class that reads a global clock cannot be tested — you can only watch it and hope.
     * With the clock injected, the press/release/repeat semantics are checkable exactly.
     */
    void handle_event(const SDL_Event& e, uint64_t now_ms);

    /**
     * Once per frame. Emits the synthetic repeat presses — 400 ms to the first, then one every
     * 100 ms, exactly as `InputMapper.startKeyRepeat` does.
     */
    void tick(uint64_t now_ms);

    /** Drain one queued event; false when empty. */
    bool poll(ButtonEvent& out);

    bool is_held(Button b) const { return held_[static_cast<size_t>(b)]; }

    /** Drop all held state and any repeat in flight (window focus loss — a stuck modifier
     *  silently reroutes every later DPAD press into the wrong combo). Kotlin's `InputMapper.reset()`. */
    void reset();

private:
    void press(Button b, uint64_t now_ms);
    void release(Button b);

    static constexpr uint64_t REPEAT_INITIAL_DELAY = 400;  // ms before the first repeat
    static constexpr uint64_t REPEAT_INTERVAL      = 100;  // ms between repeats

    bool held_[static_cast<size_t>(Button::COUNT)] = {false};

    // ONE repeat at a time, and only ever a DPAD button — the same shape as Kotlin, which keeps a
    // single `repeatRunnable` and cancels it before starting another. The app re-reads the modifiers
    // when the repeat fires, so holding A+UP keeps editing and holding UP alone keeps moving, with no
    // need to remember *which* action started the repeat.
    //
    // Releasing A or B cancels it (ButtonHandlers.kt:443). That matters: without it, letting go of A
    // mid-repeat would carry on hammering the value edit while the user thinks they have stopped.
    bool     repeatActive_ = false;
    Button   repeatButton_ = Button::DPAD_UP;
    uint64_t repeatNextMs_ = 0;

    std::deque<ButtonEvent>     queue_;
    std::vector<SDL_GameController*> controllers_;
};

#endif  // POCKETTRACKER_SDL_INPUT_H
