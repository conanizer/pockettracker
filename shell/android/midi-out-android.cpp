// midi-out-android.{h,cpp} — the ANDROID implementation of songcore::IMidiOut: five JNI calls into
// MidiOutManager.kt. Everything above `IMidiOut` is songcore/midi_out.h; the console half is
// midi-out-base.
//
// ── ⚠️⚠️ WHY A KOTLIN HALF ───────────────────────────────────────────────────────────────────────
//
//   • `android.media.midi.MidiManager` is the ONLY sanctioned route to USB, virtual and BLE MIDI.
//   • The NDK's AMidi is API 29+ (the floor is 26) and still needs Java to enumerate and open.
//   • Raw `UsbManager` would mean hand-writing the USB-MIDI class driver and losing virtual and BLE.
// Per-platform MIDI I/O is unavoidable everywhere; `IMidiOut` is the portability.
//
// ── ⚠️ THE DIRECTION GOTCHA ──────────────────────────────────────────────────────────────────────
//
// To SEND MIDI you open the device's INPUT port (names are from the DEVICE's point of view), so the
// list is `inputPortCount > 0` and a keyboard correctly does not appear. Backwards, it lists exactly
// the wrong devices and the synth stays silent with no error.
//
// ── THREADING ────────────────────────────────────────────────────────────────────────────────────
//
//   • `send` runs on the MIDI SENDER THREAD (midi-sender.cpp) and on the FRAME LOOP for a panic's
//     immediate note-offs, serialised by `ExternalConsumer`'s mutex; `MidiOutManager.send` is
//     `@Synchronized` too, for its one reusable buffer.
//   • `device_count` / `device_name` / `open` / `close` run on the frame loop only.
// ⚠️ The sender thread is an `SDL_CreateThread` thread FOR THIS FILE: SDL's entry attaches it to
// the JVM; a raw `std::thread` would abort the VM on the first JNI call.
// `open` blocks briefly (bounded, ~3 s): `MidiManager.openDevice` is async and the Kotlin side
// waits, so the OUTPUT row never claims a cable that is not live.
//
// Method ids are resolved BY NAME — a missing Kotlin side degrades to "no devices" with one log
// line, and R8 must not rename them (app/proguard-rules.pro).

#include "android/midi-out-android.h"

#ifdef __ANDROID__

#include <SDL.h>
#include <android/log.h>

#include "android/android-jni.h"   // `Attached` — shared with the INPUT backend

namespace ptshell {

namespace {

constexpr const char* kLogTag = "PocketTracker";

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

    // ⚠️ UNCONDITIONAL, both ways: a working JNI binding is silent, and on a phone with no MIDI
    // device "hooks resolved" and "R8 renamed the methods, MIDI dead in release only" look identical.
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

    // Three ints rather than a byte[]: allocation-free, a trivial signature. The Kotlin side's one
    // reusable buffer is `@Synchronized` — two threads reach this.
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
