// midi-in-android.{h,cpp} — the ANDROID implementation of songcore::IMidiIn (MIDI plan phase E5).
//
// The third and last backend, and the mirror of midi-out-android.cpp in every way but one. Everything
// above `IMidiIn` — the byte ring, the parser, the channel→track→instrument router, the injection into
// the engine — is native/songcore/midi_in.h and is shared with Windows and Linux; the sink, the device
// list and the counters are midi-in-base.{h,cpp}. This file is five JNI calls.
//
// The Java half is unavoidable for the reasons MidiOutManager.kt sets out and this direction does not
// change: `android.media.midi.MidiManager` is the only sanctioned route to USB, virtual and BLE MIDI on
// Android, the NDK's AMidi is API 29+ (this app's floor is 26) and still needs the Java side to
// enumerate and open, and raw `UsbManager` would mean hand-writing the USB-MIDI class driver.
//
// ── ⚠️ THE DIRECTION GOTCHA, and it is the OPPOSITE of the output backend's ──────────────────────
//
// **To RECEIVE MIDI you open the device's OUTPUT port.** Port names are written from the DEVICE's point
// of view: a keyboard you play FROM has an output port, a synth you play has an input port. So the list
// this backend shows is `outputPortCount > 0` — devices that can SEND — where midi-out-android.cpp
// asks for `inputPortCount > 0`. Getting either backwards produces a list of exactly the wrong devices
// and a port that opens and stays silent, which reads as a broken app rather than a wrong list. The two
// filters are one line each, in one file each, both commented, because neither can be caught by a test
// without hardware attached — the ALSA pair (STREAM_INPUT/STREAM_OUTPUT) at least has `tools/ptalsain`.
//
// ── ⚠️⚠️ THIS BACKEND IS POLLED, AND THE OTHER TWO PUSH. WHY. ────────────────────────────────────
//
// winmm calls us on its own callback thread and ALSA gets a reader thread of ours; both hand bytes to
// `MidiInBase::deliver` the moment they exist. Android's `MidiReceiver.onSend` arrives on a binder
// thread, and the obvious mirror would be a Kotlin `external fun` calling down into C++ from there.
// This does the opposite — `pump()` on the frame loop asks Kotlin for whatever has arrived — and the
// reasons are, in order:
//
//   1. ⭐ **IT COSTS NOTHING IN LATENCY, which is the argument that decides it.** The bytes a push would
//      deposit in `MidiInQueue` are not looked at until `SongcoreHost::poll()` drains them, and that is
//      once a frame. `pump()` is called from the same loop, immediately before that drain — so a byte
//      that arrives at any point in a frame is parsed in the same frame either way. A push would move
//      the same byte to a different waiting room.
//   2. **It keeps the JNI direction single.** Every native↔Kotlin call in this app is an UP-call
//      resolved by name, with one `-keep` pattern in proguard-rules.pro protecting all of them and one
//      "hooks resolved" log line saying whether R8 broke them. A `native` method declared in Kotlin is a
//      different mechanism with a different R8 failure mode — and release-only JNI-by-name breakage is
//      exactly the bug class that killed every render in the first v0.9.3 APK.
//   3. **Lifetime.** A binder thread calling into a C++ object owned by the frame loop needs that
//      object to outlive every in-flight call; here nothing on the Java side holds a native pointer at
//      all, so a close is a close.
//
// The cost is stated rather than hidden: while SDL freezes the native thread (the activity is paused),
// nothing pumps, and MidiInManager's ring fills and then drops the newest bytes — see its own note. It
// counts them and logs the total.
//
// ── THREADING ────────────────────────────────────────────────────────────────────────────────────
//
// Every method here runs on the SDL thread (the frame loop): `pump` from the loop, the other four from
// a device pick or from boot. `MidiInManager` is `@Synchronized` on the Kotlin side because ITS ring is
// filled from a binder thread — so the lock that matters is the one over there, next to the data it
// protects, rather than a rule this file would have to remember.
//
// Every method id is resolved BY NAME, so a missing Kotlin side degrades to "no devices" with one log
// line rather than a crash — and R8 must not rename them: see app/proguard-rules.pro.

#include "midi-in-android.h"

#ifdef __ANDROID__

#include <android/log.h>

#include "android-jni.h"

namespace ptshell {

namespace {

constexpr const char* kLogTag = "PocketTracker";

/**
 * How many bytes one frame's pump can carry.
 *
 * MIDI 1.0 is 31 250 baud ≈ 3 125 bytes/second, so a 60 Hz frame can physically contain about 52 —
 * two orders of magnitude below this. The size is what it is so that the FIRST pump after a paused
 * activity resumes can empty MidiInManager's whole ring in one call rather than dribbling a stale
 * backlog out over the next twenty frames.
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

    // ⚠️ UNCONDITIONAL, both ways — midi-out-android.cpp's line and its argument, which is even more
    // true here: an input path's correct behaviour with nothing plugged in is COMPLETE SILENCE, so
    // "the hooks resolved and no keyboard is attached" and "R8 renamed the methods and MIDI in is dead
    // in release only" produce identical behaviour and identical logs. This line is the difference.
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

    // ⚠️ THE BUFFER IS A GLOBAL REF AND IT IS ALLOCATED ONCE, HERE. A local ref would die at the end of
    // this function and a fresh `NewByteArray` per pump would allocate a kilobyte sixty times a second
    // for the whole session — garbage-collector pressure on the one thread that must not stutter, in
    // exchange for nothing. It is released in `close()`, which is also the only other place that may
    // touch it.
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
