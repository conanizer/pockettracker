#ifndef POCKETTRACKER_SHELL_MIDI_ANDROID_DEVICES_H
#define POCKETTRACKER_SHELL_MIDI_ANDROID_DEVICES_H

// What both Android MIDI directions call on MainActivity: the device list, a name, open, close, then
// the direction's own transfer methods. Nothing off Android.

#ifdef __ANDROID__

#include <jni.h>

#include <string>

#include "android/android-jni.h"

namespace ptshell {

struct JavaMethod {
    const char* name;
    const char* sig;
    const char* otherwise;   // what a miss costs, for the log
};

class AndroidMidiDevices {
  public:
    enum Slot { COUNT, NAME, OPEN, CLOSE, OWN };   // OWN: the direction's first own method
    static constexpr int MAX = 6;

    // `methods` in Slot order, then the direction's own; the first `required` must resolve, or the
    // direction is off ("no devices").
    template <int N>
    AndroidMidiDevices(const char* label, const JavaMethod (&methods)[N], int required)
        : label_(label), methods_(methods), count_(N), required_(required) {
        static_assert(N <= MAX, "raise MAX");
    }

    // The method ids, or null when the direction is off. Resolved by name on the first call.
    const jmethodID* ids(const Attached& a);

    // Re-enumerates every call (hot-plug); the Kotlin snapshot refreshes with it, so indices hold.
    int         count();
    std::string name(int index);   // from the snapshot `count` just took
    bool        open(const Attached& a, int index);
    void        close(const Attached& a);

  private:
    const char*       label_;
    const JavaMethod* methods_;
    int               count_;
    int               required_;
    jmethodID         ids_[MAX] = {};
    bool              resolved_ = false;
    bool              ok_       = false;
};

}  // namespace ptshell

#endif  // __ANDROID__
#endif  // POCKETTRACKER_SHELL_MIDI_ANDROID_DEVICES_H
