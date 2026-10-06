// midi-in-android.{h,cpp} — the ANDROID implementation of songcore::IMidiIn: five JNI calls into
// MidiInManager.kt. Everything above `IMidiIn` is songcore/midi_in.h; sink, device list and counters
// are midi-in-base. The Java half is unavoidable (see midi-out-android.cpp).
//
// ── ⚠️ THE DIRECTION GOTCHA — the OPPOSITE of the output backend's ───────────────────────────────
//
// To RECEIVE MIDI you open the device's OUTPUT port (names are from the DEVICE's point of view), so
// this list is `outputPortCount > 0` where midi-out-android.cpp asks for `inputPortCount > 0`.
// Backwards, it lists exactly the wrong devices and a port that opens and stays silent.
//
// ── ⚠️⚠️ THIS BACKEND IS POLLED; THE OTHER TWO PUSH ──────────────────────────────────────────────
//
// `MidiReceiver.onSend` arrives on a binder thread; instead of Kotlin calling down from there,
// `pump()` on the frame loop asks Kotlin for what arrived:
//   1. ⚠️ It costs the loop's tick: the drain runs on the AUDIO thread, so a pumped byte first waits
//      for the next pump (`POLL_MS`, kept fast while this port is open — `polled()`). The alternatives
//      (a Kotlin `external fun` from the binder thread, pumping from the sender thread) are untried.
//   2. It keeps every native↔Kotlin call an UP-call resolved by name, under one `-keep` pattern in
//      proguard-rules.pro; a Kotlin `native` method has a different, release-only R8 failure mode.
//   3. Lifetime: nothing on the Java side holds a native pointer, so a close is a close.
// While SDL freezes the native thread (activity paused) nothing pumps; MidiInManager's ring fills
// and drops the newest bytes, counting them.
//
// ── THREADING ────────────────────────────────────────────────────────────────────────────────────
//
// Every method runs on the SDL thread (the frame loop). MidiInManager is `@Synchronized` because ITS
// ring fills from a binder thread. Method ids are resolved BY NAME — a missing Kotlin side degrades
// to "no devices" with one log line, and R8 must not rename them (app/proguard-rules.pro).

#include "android/midi-in-android.h"

#ifdef __ANDROID__

#include <android/log.h>

#include "android/android-jni.h"

namespace ptshell {

namespace {

constexpr const char* kLogTag = "PocketTracker";

/**
 * How many bytes one frame's pump can carry — far above what a 60 Hz frame of MIDI holds (~52), so
 * the first pump after a paused activity resumes empties MidiInManager's whole ring at once.
 */
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

    // ⚠️ UNCONDITIONAL, both ways: with nothing plugged in, "hooks resolved" and "R8 renamed the
    // methods, MIDI in dead in release only" behave identically — this line is the difference.
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

    // Re-enumerates on every call, because MIDI is hot-pluggable and a port list is only true at the
    // moment it is read. The Kotlin side refreshes ITS snapshot here too, so the indices this returns
    // are the ones `device_name` and `open` will resolve against.
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
    if (a.failed() || !raw) return std::string();

    jstring     js    = static_cast<jstring>(raw);
    const char* chars = a.env->GetStringUTFChars(js, nullptr);
    std::string out   = chars ? chars : "";
    if (chars) a.env->ReleaseStringUTFChars(js, chars);
    a.env->DeleteLocalRef(raw);
    return out;
}

bool AndroidMidiIn::open(int index) {
    close();
    Attached a;
    if (!a.ok()) return false;
    const Hooks& h = hooks(a.env, a.activity);
    if (!h.ok) return false;

    // ⚠️ A GLOBAL REF, ALLOCATED ONCE: a fresh `NewByteArray` per pump would allocate a kilobyte
    // sixty times a second on the thread that must not stutter. Released in `close()`.
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
        // ⚠️ Released whether or not the port was open: `open()` allocates it BEFORE the port opens, so
        // a failed open would otherwise leak a global ref per attempt.
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

    // ⚠️ `GetByteArrayRegion` and not `GetByteArrayElements`: the region copy cannot pin the heap and
    // has no Release to forget. `n` is bounded by the array the Kotlin side was handed, but it is
    // clamped anyway — a wrong length here is a stack smash, and the value crosses a language boundary.
    jbyte   raw[READ_BUF];
    const jint len = n > READ_BUF ? READ_BUF : n;
    a.env->GetByteArrayRegion(buffer_, 0, len, raw);
    if (a.failed()) return;

    // ⭐ Through `deliver` like every other backend: it is the ONE door, and therefore the one place
    // that counts. `callbacks()` on this platform means "pumps that found something", which is the
    // honest reading of the same number.
    deliver(reinterpret_cast<const uint8_t*>(raw), static_cast<int>(len));
}

}  // namespace ptshell

#endif  // __ANDROID__
