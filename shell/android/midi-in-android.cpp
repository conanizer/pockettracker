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
// "no devices" plus one log line.

#include "android/midi-in-android.h"

#ifdef __ANDROID__

#include <android/log.h>

#include "android/android-jni.h"

namespace ptshell {

namespace {

constexpr const char* kLogTag = "PocketTracker";

// One pump's capacity — far above a 60 Hz frame of MIDI (~52 bytes), so the first pump after a
// pause empties MidiInManager's whole ring.
constexpr int READ_BUF = 1024;

}  // namespace

AndroidMidiIn::~AndroidMidiIn() { close(); }

AndroidMidiIn::Hooks& AndroidMidiIn::hooks(JNIEnv* env, jobject activity) {
    if (hooks_.resolved) return hooks_;
    hooks_.resolved = true;

    jclass cls   = env->GetObjectClass(activity);
    hooks_.count = env->GetMethodID(cls, "midiInDeviceCount", "()I");
    hooks_.name  = env->GetMethodID(cls, "midiInDeviceName", "(I)Ljava/lang/String;");
    hooks_.open  = env->GetMethodID(cls, "midiInOpenDevice", "(I)Z");
    hooks_.close = env->GetMethodID(cls, "midiInCloseDevice", "()V");
    hooks_.read  = env->GetMethodID(cls, "midiInRead", "([B)I");
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);

    hooks_.ok = hooks_.count && hooks_.name && hooks_.open && hooks_.close && hooks_.read;

    // ⚠️ Logged both ways: with nothing plugged in, "resolved" and "R8 renamed them" look the same.
    __android_log_print(hooks_.ok ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, kLogTag,
                        "midi: MidiManager INPUT hooks %s (count=%p name=%p open=%p close=%p read=%p)",
                        hooks_.ok ? "resolved" : "NOT FOUND - MIDI in disabled", hooks_.count,
                        hooks_.name, hooks_.open, hooks_.close, hooks_.read);
    return hooks_;
}

int AndroidMidiIn::device_count() {
    Attached a;
    if (!a.ok()) return 0;
    const Hooks& h = hooks(a.env, a.activity);
    if (!h.ok) return 0;

    // Re-enumerates every call (hot-plug); the Kotlin snapshot refreshes with it, so indices hold.
    const jint n = a.env->CallIntMethod(a.activity, h.count);
    if (a.failed()) return 0;
    return n < 0 ? 0 : static_cast<int>(n);
}

std::string AndroidMidiIn::device_name(int index) {
    Attached a;
    if (!a.ok()) return std::string();
    const Hooks& h = hooks(a.env, a.activity);
    if (!h.ok) return std::string();

    jobject raw = a.env->CallObjectMethod(a.activity, h.name, static_cast<jint>(index));
    if (a.failed()) return std::string();
    return a.take(raw);
}

bool AndroidMidiIn::open(int index) {
    close();
    Attached a;
    if (!a.ok()) return false;
    const Hooks& h = hooks(a.env, a.activity);
    if (!h.ok) return false;

    // ⚠️ A global ref, allocated once: no kilobyte allocated sixty times a second. Freed in `close()`.
    if (!buffer_) {
        jbyteArray local = a.env->NewByteArray(READ_BUF);
        if (local) {
            buffer_ = static_cast<jbyteArray>(a.env->NewGlobalRef(local));
            a.env->DeleteLocalRef(local);
        }
    }
    if (!buffer_) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "midi: could not allocate the MIDI in buffer");
        return false;
    }

    const jboolean ok = a.env->CallBooleanMethod(a.activity, h.open, static_cast<jint>(index));
    if (a.failed()) return false;
    open_ = (ok == JNI_TRUE);
    if (open_) openIndex_ = index;
    return open_;
}

void AndroidMidiIn::close() {
    Attached a;
    if (a.ok()) {
        const Hooks& h = hooks(a.env, a.activity);
        if (h.ok && open_) {
            a.env->CallVoidMethod(a.activity, h.close);
            a.failed();
        }
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
    const Hooks& h = hooks(a.env, a.activity);
    if (!h.ok) return;

    const jint n = a.env->CallIntMethod(a.activity, h.read, buffer_);
    if (a.failed() || n <= 0) return;

    // `GetByteArrayRegion`: a copy, nothing pinned, no Release to forget. `n` is clamped anyway — a
    // wrong length here is a stack smash.
    jbyte   raw[READ_BUF];
    const jint len = n > READ_BUF ? READ_BUF : n;
    a.env->GetByteArrayRegion(buffer_, 0, len, raw);
    if (a.failed()) return;

    // Through `deliver`, the one door that counts. `callbacks()` here means "pumps that found something".
    deliver(reinterpret_cast<const uint8_t*>(raw), static_cast<int>(len));
}

}  // namespace ptshell

#endif  // __ANDROID__
