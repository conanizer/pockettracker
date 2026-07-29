// midi-out-android.{h,cpp} — the ANDROID implementation of songcore::IMidiOut (MIDI plan phase B2b).
//
// The third and last of the backends (§4.3 / §4.5): Windows = winmm, Linux = ALSA rawmidi, **Android
// = MidiManager, reached by JNI**. Nothing above `IMidiOut` is per-platform — the serializer, the note
// lifecycle, the release queue and the 0–255 → 0–127 scaling are native/songcore/midi_out.h and are
// shared by all three; the console half (device list, spec resolver, TEST, trace, counters, the
// 16-channel panic before a close) is midi-out-base.{h,cpp}. This file is five JNI calls.
//
// ── ⚠️⚠️ THIS ADDS A KOTLIN FILE BACK, AND THAT IS A DELIBERATE DECISION, NOT AN OVERSIGHT ────────
//
// Convergence Phase E deleted ~35.3k lines of Kotlin and left `app/src/main/java` a four-file shim.
// This makes it five (`platform/android/MidiOutManager.kt`), and the MIDI plan named the cost when it
// was ratified rather than discovering it here. The reason it is unavoidable:
//
//   • `android.media.midi.MidiManager` is the ONLY sanctioned route to USB, virtual and BLE MIDI
//     devices on Android. It is a Java API.
//   • The NDK's **AMidi** C API does not remove the Java half: it is API 29+ (this app's floor is 26)
//     AND it still requires a Java-side `MidiManager` to enumerate and open the device — it only
//     makes the byte I/O native, which is the part that was never the problem.
//   • Raw `UsbManager` bulk transfers would mean hand-writing the USB-MIDI class driver, and would
//     lose virtual (app-to-app) devices and BLE entirely.
//
// So this is the same shape `ButtonFeedback` already has and for the same reason — an Android system
// service with no C++ twin, reached through one narrow outward hook — not a retreat from convergence.
// It costs portability nothing: per-platform MIDI I/O is unavoidable everywhere (Linux = ALSA,
// Windows = winmm, macOS = CoreMIDI), and `IMidiOut` IS the portability.
//
// ── ⚠️ THE DIRECTION GOTCHA, which is the easiest thing in this file to get backwards ────────────
//
// **To SEND MIDI you open the device's INPUT port.** The names are written from the DEVICE's point of
// view: a synth you play has an input port, a keyboard you play FROM has an output port. So the list
// this backend must show is `inputPortCount > 0` — devices that can RECEIVE — and a MIDI keyboard
// correctly does not appear on it. Getting this backwards lists exactly the wrong devices, and the
// symptom is "the port opens and the synth is silent", not an error. It is the exact mirror of the
// ALSA backend's `SND_RAWMIDI_STREAM_OUTPUT` filter; both are in one place each, and both are
// commented, because neither can be caught by a test that does not have hardware attached.
//
// ── THREADING ────────────────────────────────────────────────────────────────────────────────────
//
// Every method here runs on the SDL thread (the frame loop), NOT the Java UI thread —
// `SDL_AndroidGetJNIEnv` attaches it to the JVM and hands back its env. `MidiInputPort.send` is safe
// from any thread. The one method that blocks is `open`: `MidiManager.openDevice` is ASYNCHRONOUS and
// the Kotlin side waits on a latch for it (bounded, ~3 s), so a port pick can stall the frame loop
// briefly. That is a deliberate trade — see MidiOutManager.kt, which explains why the alternative
// (an optimistic `true`) would make the OUTPUT row lie about which cable is live.
//
// Every method id is resolved BY NAME, so a missing Kotlin side degrades to "no devices" with one log
// line rather than a crash — and R8 must not rename them: see app/proguard-rules.pro.

#include "midi-out-android.h"

#ifdef __ANDROID__

#include <SDL.h>
#include <android/log.h>

namespace ptshell {

namespace {

constexpr const char* kLogTag = "PocketTracker";

/**
 * The env + a LOCAL ref to the running activity, released on scope exit.
 *
 * Local refs are not free — the JNI spec only guarantees 16 slots without an explicit frame — and
 * `send` runs per MIDI message, so leaking one per note would eventually abort the VM with a local
 * reference table overflow. RAII rather than five hand-written DeleteLocalRef calls.
 */
struct Attached {
    JNIEnv* env      = nullptr;
    jobject activity = nullptr;

    Attached() {
        env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (env) activity = static_cast<jobject>(SDL_AndroidGetActivity());
    }
    ~Attached() {
        if (env && activity) env->DeleteLocalRef(activity);
    }
    Attached(const Attached&)            = delete;
    Attached& operator=(const Attached&) = delete;

    bool ok() const { return env != nullptr && activity != nullptr; }

    /** Clear any pending exception so the NEXT JNI call is not refused. Returns true if there was one. */
    bool failed() const {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }
};

}  // namespace

AndroidMidiOut::~AndroidMidiOut() { close(); }

AndroidMidiOut::Hooks& AndroidMidiOut::hooks(JNIEnv* env, jobject activity) {
    if (hooks_.resolved) return hooks_;
    hooks_.resolved = true;

    jclass cls = env->GetObjectClass(activity);
    hooks_.count = env->GetMethodID(cls, "midiDeviceCount", "()I");
    hooks_.name  = env->GetMethodID(cls, "midiDeviceName", "(I)Ljava/lang/String;");
    hooks_.open  = env->GetMethodID(cls, "midiOpenDevice", "(I)Z");
    hooks_.close = env->GetMethodID(cls, "midiCloseDevice", "()V");
    hooks_.send  = env->GetMethodID(cls, "midiSend", "(IIII)Z");
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);

    hooks_.ok = hooks_.count && hooks_.name && hooks_.open && hooks_.close && hooks_.send;

    // ⚠️ UNCONDITIONAL, both ways. A working JNI binding is SILENT by nature, and the guardrails have
    // a name for that: a component whose correct behaviour is silence cannot be told from one that
    // never ran. On a phone with no MIDI device attached — which is most phones — "the hooks resolved
    // and there is nothing plugged in" and "R8 renamed the methods and MIDI is dead in release only"
    // produce identical behaviour and identical logs. This line is the difference, and release-only
    // JNI-by-name breakage is precisely the bug class that killed every render in the first v0.9.3 APK.
    __android_log_print(hooks_.ok ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, kLogTag,
                        "midi: MidiManager hooks %s (count=%p name=%p open=%p close=%p send=%p)",
                        hooks_.ok ? "resolved" : "NOT FOUND - MIDI out disabled",
                        hooks_.count, hooks_.name, hooks_.open, hooks_.close, hooks_.send);
    return hooks_;
}

int AndroidMidiOut::device_count() {
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

std::string AndroidMidiOut::device_name(int index) {
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

bool AndroidMidiOut::open(int index) {
    close();
    Attached a;
    if (!a.ok()) return false;
    const Hooks& h = hooks(a.env, a.activity);
    if (!h.ok) return false;

    const jboolean ok = a.env->CallBooleanMethod(a.activity, h.open, static_cast<jint>(index));
    if (a.failed()) return false;
    open_ = (ok == JNI_TRUE);
    if (open_) openIndex_ = index;
    return open_;
}

void AndroidMidiOut::close() {
    if (!open_) return;

    // ⚠️ BEFORE the port goes: the device holds whatever is sounding when the connection drops, and
    // no later message can reach it. `panic_all_channels` goes through `send`, so it must run while
    // `open_` is still true — which is why this line is here and not after the reset below.
    panic_all_channels();

    Attached a;
    if (a.ok()) {
        const Hooks& h = hooks(a.env, a.activity);
        if (h.ok) {
            a.env->CallVoidMethod(a.activity, h.close);
            a.failed();
        }
    }
    open_      = false;
    openIndex_ = -1;
}

void AndroidMidiOut::send(const uint8_t* data, int len) {
    if (!open_ || len <= 0 || len > 3) return;
    Attached a;
    if (!a.ok()) return;
    const Hooks& h = hooks(a.env, a.activity);
    if (!h.ok) return;

    // Three ints rather than a byte[]: it keeps this side allocation-free (no jbyteArray per note, no
    // global ref to manage) and the JNI signature trivial. The Kotlin side owns one reusable 3-byte
    // buffer, which is safe because every call arrives on the one thread that pumps the queue.
    ++sent_;
    const jboolean ok =
            a.env->CallBooleanMethod(a.activity, h.send, static_cast<jint>(len >= 1 ? data[0] : 0),
                                     static_cast<jint>(len >= 2 ? data[1] : 0),
                                     static_cast<jint>(len >= 3 ? data[2] : 0), static_cast<jint>(len));
    const bool bad = a.failed() || ok != JNI_TRUE;
    if (bad) ++errors_;
    trace_message(data, len, bad);
}

}  // namespace ptshell

#endif  // __ANDROID__
