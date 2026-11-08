#include "android/midi-android-devices.h"

#ifdef __ANDROID__

#include <android/log.h>

namespace ptshell {

const jmethodID* AndroidMidiDevices::ids(const Attached& a) {
    if (!resolved_) {
        resolved_ = true;
        ok_       = true;
        for (int i = 0; i < count_; ++i) {
            ids_[i] = a.method(methods_[i].name, methods_[i].sig, methods_[i].otherwise);
            if (i < required_ && !ids_[i]) ok_ = false;
        }
        // ⚠️ Logged both ways: with nothing plugged in, "resolved" and "R8 renamed them" look the same.
        __android_log_print(ok_ ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, "PocketTracker",
                            "midi: MidiManager %s hooks %s", label_,
                            ok_ ? "resolved" : "NOT FOUND - disabled");
    }
    return ok_ ? ids_ : nullptr;
}

int AndroidMidiDevices::count() {
    Attached a;
    if (!a.ok()) return 0;
    const jmethodID* m = ids(a);
    const int        n = m ? a.call<int>(m[COUNT]) : 0;
    return n < 0 ? 0 : n;
}

std::string AndroidMidiDevices::name(int index) {
    Attached a;
    if (!a.ok()) return std::string();
    const jmethodID* m = ids(a);
    return m ? a.call<std::string>(m[NAME], index) : std::string();
}

bool AndroidMidiDevices::open(const Attached& a, int index) {
    const jmethodID* m = ids(a);
    return m && a.call<bool>(m[OPEN], index);
}

void AndroidMidiDevices::close(const Attached& a) {
    if (const jmethodID* m = ids(a)) a.call<void>(m[CLOSE]);
}

}  // namespace ptshell

#endif  // __ANDROID__
