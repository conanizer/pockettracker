// The Android songcore::IMidiOut: JNI calls into MidiOutManager.kt (which says why the port is in
// Kotlin, and why to SEND you open the device's INPUT port).
//
// `send` runs on the MIDI sender thread and on the frame loop for a panic's note-offs, serialised
// by `ExternalConsumer`'s mutex; the rest on the frame loop. `Attached` attaches whichever thread
// asks. `open` blocks up to ~3 s while Kotlin waits for the device. Method ids are resolved by name:
// a missing Kotlin side is "no devices" plus a log line.

#include "android/midi-out-android.h"

#ifdef __ANDROID__

namespace ptshell {

namespace {

constexpr const char* kOff = "MIDI out disabled";
constexpr JavaMethod  kMethods[] = {
    {"midiDeviceCount", "()I", kOff},
    {"midiDeviceName", "(I)Ljava/lang/String;", kOff},
    {"midiOpenDevice", "(I)Z", kOff},
    {"midiCloseDevice", "()V", kOff},
    {"midiSend", "(IIII)Z", kOff},
    {"midiSendBytes", "([BI)Z", "a SysEx does not go out"},   // optional: a SysEx, whole
};
constexpr int SEND = AndroidMidiDevices::OWN, SEND_BYTES = AndroidMidiDevices::OWN + 1;

}  // namespace

AndroidMidiOut::AndroidMidiOut() : devices_("OUTPUT", kMethods, 5) {}
AndroidMidiOut::~AndroidMidiOut() { close(); }

int         AndroidMidiOut::device_count() { return devices_.count(); }
std::string AndroidMidiOut::device_name(int index) { return devices_.name(index); }

bool AndroidMidiOut::open(int index) {
    close();
    Attached a;
    if (!a.ok()) return false;
    open_ = devices_.open(a, index);
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
        devices_.close(a);
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
    const jmethodID* m = devices_.ids(a);
    if (!m) return;

    bool ok = false;
    if (len > 3) {
        // A SysEx whole, not three bytes at a time: a USB packer may not join the pieces.
        if (m[SEND_BYTES] && longBuf_) {
            a.env->SetByteArrayRegion(longBuf_, 0, len, reinterpret_cast<const jbyte*>(data));
            ok = !a.failed() && a.call<bool>(m[SEND_BYTES], static_cast<jobject>(longBuf_), len);
        }
    } else {
        // Three ints, not a byte[]: allocation-free.
        ok = a.call<bool>(m[SEND], len >= 1 ? int{data[0]} : 0, len >= 2 ? int{data[1]} : 0,
                          len >= 3 ? int{data[2]} : 0, len);
    }
    const bool bad = !ok;
    if (bad) ++errors_;
    trace_message(data, len, bad);
}

}  // namespace ptshell

#endif  // __ANDROID__
