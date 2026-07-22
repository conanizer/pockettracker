// button_feedback.h — the click + haptic a VIRTUAL button gives back (convergence D, Android-only).
//
// The last of the Android device features the SDL shell had no twin for. It is the ONE outward hook
// the convergence plan's Phase-E table names: "its TRIGGER moves into the shell's touch path (one
// outward JNI hook)". Everything a phone user feels when they tap the on-screen gamepad — the SoundPool
// click and the Vibrator pulse — is Java's, because both are Android system services; what is portable
// is only the DECISION to fire, and that lives in `sdl-touch.cpp` beside the press it already emits.
//
// ⚠️ WHY AN INTERFACE AND NOT AN `#ifdef` IN sdl-touch.cpp. The same reason `AudioBackend` is one:
// sdl-touch.cpp is SHARED, compiled on desktop and on Android both, and it must not learn the word
// `jni`. The platform's `main` constructs the concrete feedback (Android → a JNI shim; desktop → none
// at all, the pointer stays null) and hands it down through `AppConfig`, exactly as it hands down the
// audio backend and the filesystem. A build with no touchscreen constructs nothing and pays nothing.
//
// ⚠️ THIS IS PURELY UI FEEDBACK, NOT PART OF THE INPUT MEANING. The click sounds on the raw finger
// down/up, NOT on the synthetic key-repeats a held D-pad produces, and NOT on physical pad/keyboard
// buttons — a handheld with real buttons makes no UI click. That is why the trigger sits in the TOUCH
// layer (`handle_finger`) and not in `SdlInput` or the dispatcher: it mirrors Kotlin exactly, where
// `LocalButtonEventCallback` is wired only into the VirtualControls composables.

#ifndef POCKETTRACKER_BUTTON_FEEDBACK_H
#define POCKETTRACKER_BUTTON_FEEDBACK_H

#include "ui/buttons.h"

namespace ptshell {

/**
 * The user's four BTN SOUND / BTN VIBRO scalars, snapshotted from `SettingsValues`. Passed on every
 * event rather than pushed on change: a finger event is human-paced (a few a second), so re-reading
 * four ints costs nothing and there is no observer to keep in step — the SETTINGS screen can move any
 * of them live and the next tap already has the new value.
 */
struct ButtonFeedbackSettings {
    bool soundEnabled = false;
    int  soundVolume  = 255;  // 0..255, as the SETTINGS hex byte; the impl scales to 0..1
    bool vibroEnabled = false;
    int  vibroPower   = 255;  // 0..255
};

/**
 * Play the click and/or the haptic for one virtual-button transition.
 *
 * @param button the virtual button — its ordinal matches Kotlin's `VirtualButton` exactly (buttons.h),
 *               which is what lets the JNI shim pass it straight through as an int.
 * @param down   true on FINGERDOWN (press feel), false on release (a lift OR a slide-off).
 * @param s      the live settings; an implementation still checks the enable flags itself, so a
 *               disabled feature is a cheap early return rather than a reason not to call.
 */
class ButtonFeedback {
public:
    virtual ~ButtonFeedback() = default;
    virtual void play(pt::ui::Button button, bool down, const ButtonFeedbackSettings& s) = 0;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_BUTTON_FEEDBACK_H
