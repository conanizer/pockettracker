// The Android songcore::IMidiOut: five JNI calls into MidiOutManager.kt (which says why the port is
// in Kotlin, and why to SEND you open the device's INPUT port).
//
// `send` runs on the MIDI sender thread and on the frame loop for a panic's note-offs, serialised
// by `ExternalConsumer`'s mutex; the rest on the frame loop. ⚠️ The sender thread is an
// `SDL_CreateThread` thread for this file: SDL attaches it to the JVM, and a raw `std::thread` would
// abort the VM on the first JNI call. `open` blocks up to ~3 s while Kotlin waits for the device.
// Method ids are resolved by name: a missing Kotlin side is "no devices" plus one log line.

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
    hooks_.sendBytes = env->GetMethodID(cls, "midiSendBytes", "([BI)Z");
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);

    hooks_.ok = hooks_.count && hooks_.name && hooks_.open && hooks_.close && hooks_.send;

    // ⚠️ Logged both ways: with no device plugged in, "resolved" and "R8 renamed them" look the same.
    __android_log_print(hooks_.ok ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, kLogTag,
                        "midi: MidiManager hooks %s (count=%p name=%p open=%p close=%p send=%p sendBytes=%p)",
                        hooks_.ok ? "resolved" : "NOT FOUND - MIDI out disabled",
                        hooks_.count, hooks_.name, hooks_.open, hooks_.close, hooks_.send, hooks_.sendBytes);
    return hooks_;
}

int AndroidMidiOut::device_count() {
    Attached a;
    if (!a.ok()) return 0;
    const Hooks& h = hooks(a.env, a.activity);
    if (!h.ok) return 0;

    // Re-enumerates every call (hot-plug); the Kotlin snapshot refreshes with it, so indices hold.
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
    if (a.failed()) return std::string();
    return a.take(raw);
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
    if (open_ && !longBuf_) {
        jbyteArray local = a.env->NewByteArray(songcore::NOTE_COMMAND_BYTES);
        if (local) {
            longBuf_ = static_cast<jbyteArray>(a.env->NewGlobalRef(local));
            a.env->DeleteLocalRef(local);
        }
    }
    return open_;
}

void AndroidMidiOut::close() {
    if (!open_) return;

    // ⚠️ Before the port goes, while `open_` is still true — `panic_all_channels` sends through `send`.
    panic_all_channels();

    Attached a;
    if (a.ok()) {
        const Hooks& h = hooks(a.env, a.activity);
        if (h.ok) {
            a.env->CallVoidMethod(a.activity, h.close);
            a.failed();
        }
        if (longBuf_) {
            a.env->DeleteGlobalRef(longBuf_);
            longBuf_ = nullptr;
        }
    }
    open_      = false;
    openIndex_ = -1;
}

void AndroidMidiOut::send(const uint8_t* data, int len) {
    if (!open_ || len <= 0 || len > songcore::NOTE_COMMAND_BYTES) return;
    Attached a;
    if (!a.ok()) return;
    const Hooks& h = hooks(a.env, a.activity);
    if (!h.ok) return;

    jboolean ok = JNI_FALSE;
    if (len > 3) {
        // A SysEx whole, not three bytes at a time: a USB packer may not join the pieces.
        if (h.sendBytes && longBuf_) {
            a.env->SetByteArrayRegion(longBuf_, 0, len, reinterpret_cast<const jbyte*>(data));
            ok = a.env->CallBooleanMethod(a.activity, h.sendBytes, longBuf_, static_cast<jint>(len));
        }
    } else {
        // Three ints, not a byte[]: allocation-free.
        ok = a.env->CallBooleanMethod(a.activity, h.send, static_cast<jint>(len >= 1 ? data[0] : 0),
                                      static_cast<jint>(len >= 2 ? data[1] : 0),
                                      static_cast<jint>(len >= 3 ? data[2] : 0), static_cast<jint>(len));
    }
    const bool bad = a.failed() || ok != JNI_TRUE;
    if (bad) ++errors_;
    trace_message(data, len, bad);
}

}  // namespace ptshell

#endif  // __ANDROID__
