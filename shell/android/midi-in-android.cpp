// The Android songcore::IMidiIn: five JNI calls into MidiInManager.kt (to RECEIVE you open the
// device's OUTPUT port — the opposite of output).
//
// ⚠️ Polled, where the other backends push: `pump()` on the frame loop asks Kotlin for what arrived
// on its binder thread. It costs a tick — the drain runs on the audio thread, so a byte waits for
// the next pump (kept fast while the port is open, `polled()`) — but every native↔Kotlin call stays
// an up-call by name under one `-keep` pattern, and nothing in Java holds a native pointer. While
// the activity is paused nothing pumps, and MidiInManager's ring drops the newest bytes.
//
// Every method runs on the SDL thread. Method ids are resolved by name: a missing Kotlin side is
// "no devices" plus a log line.

#include "android/midi-in-android.h"

#ifdef __ANDROID__

#include <android/log.h>

namespace ptshell {

namespace {

// One pump's capacity — far above a 60 Hz frame of MIDI (~52 bytes), so the first pump after a
// pause empties MidiInManager's whole ring.
constexpr int READ_BUF = 1024;

constexpr const char* kOff = "MIDI in disabled";
constexpr JavaMethod  kMethods[] = {
    {"midiInDeviceCount", "()I", kOff},
    {"midiInDeviceName", "(I)Ljava/lang/String;", kOff},
    {"midiInOpenDevice", "(I)Z", kOff},
    {"midiInCloseDevice", "()V", kOff},
    {"midiInRead", "([B)I", kOff},
};
constexpr int READ = AndroidMidiDevices::OWN;

}  // namespace

AndroidMidiIn::AndroidMidiIn() : devices_("INPUT", kMethods, 5) {}
AndroidMidiIn::~AndroidMidiIn() { close(); }

int         AndroidMidiIn::device_count() { return devices_.count(); }
std::string AndroidMidiIn::device_name(int index) { return devices_.name(index); }

bool AndroidMidiIn::open(int index) {
    close();
    Attached a;
    if (!a.ok() || !devices_.ids(a)) return false;

    // ⚠️ A global ref, allocated once: no kilobyte allocated sixty times a second. Freed in `close()`.
    if (!buffer_) {
        jbyteArray local = a.env->NewByteArray(READ_BUF);
        if (local) {
            buffer_ = static_cast<jbyteArray>(a.env->NewGlobalRef(local));
            a.env->DeleteLocalRef(local);
        }
    }
    if (!buffer_) {
        __android_log_print(ANDROID_LOG_WARN, "PocketTracker", "midi: could not allocate the MIDI in buffer");
        return false;
    }

    open_ = devices_.open(a, index);
    if (open_) openIndex_ = index;
    return open_;
}

void AndroidMidiIn::close() {
    Attached a;
    if (a.ok()) {
        if (open_) devices_.close(a);
        // ⚠️ Freed even when the port never opened: `open()` allocates it first.
        if (buffer_) {
            a.env->DeleteGlobalRef(buffer_);
            buffer_ = nullptr;
        }
    }
    open_      = false;
    openIndex_ = -1;
}

void AndroidMidiIn::pump() {
    if (!open_ || !buffer_) return;

    Attached a;
    if (!a.ok()) return;
    const jmethodID* m = devices_.ids(a);
    if (!m) return;

    const int n = a.call<int>(m[READ], static_cast<jobject>(buffer_));
    if (n <= 0) return;

    // `GetByteArrayRegion`: a copy, nothing pinned, no Release to forget. `n` is clamped anyway — a
    // wrong length here is a stack smash.
    jbyte     raw[READ_BUF];
    const int len = n > READ_BUF ? READ_BUF : n;
    a.env->GetByteArrayRegion(buffer_, 0, len, raw);
    if (a.failed()) return;

    // Through `deliver`, the one door that counts. `callbacks()` here means "pumps that found something".
    deliver(reinterpret_cast<const uint8_t*>(raw), len);
}

}  // namespace ptshell

#endif  // __ANDROID__
