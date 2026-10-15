// The click and haptic of a virtual button (Android). An interface, not an #ifdef, so the shared
// sdl-touch.cpp never learns the word `jni`: Android's `main` hands in a JNI shim, elsewhere it is null.
// Fired on the raw finger down/up only — not repeats, not physical buttons.

#ifndef POCKETTRACKER_BUTTON_FEEDBACK_H
#define POCKETTRACKER_BUTTON_FEEDBACK_H

#include "ui/buttons.h"

namespace ptshell {

// BTN SOUND / BTN VIBRO, passed with every event (a few a second) so a SETTINGS edit applies on the
// next tap with nothing to keep in step.
struct ButtonFeedbackSettings {
    bool soundEnabled = false;
    int  soundVolume  = 255;  // 0..255, the SETTINGS byte
    bool vibroEnabled = false;
    int  vibroPower   = 255;  // 0..255
};

// One virtual-button transition. `button`'s ordinal matches Kotlin's `VirtualButton` (passed through
// JNI as an int); `down` is the press, false a lift or slide-off. The implementation checks the
// enable flags itself.
class ButtonFeedback {
public:
    virtual ~ButtonFeedback() = default;
    virtual void play(pt::ui::Button button, bool down, const ButtonFeedbackSettings& s) = 0;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_BUTTON_FEEDBACK_H
