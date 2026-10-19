// Linux MIDI output over ALSA rawmidi. rawmidi, not seq: seq needs the `snd-seq` module, which CFW
// kernels do not guarantee (the cost: no virtual ports). dlopen, not -lasound: the PortMaster build
// container has no libasound-dev, and a CFW without the library still starts — MIDI just shows
// `OFF  NO PORTS`. The price is hand-copied prototypes (alsa-rawmidi.h).
// `send` runs on the sender thread and the frame loop (a panic), serialised by `ExternalConsumer`'s
// mutex. ⚠️ Opened blocking: a non-blocking write that answers -EAGAIN loses the bytes, and a lost
// note-off sounds until the gear is power-cycled. A 4 KB buffer never fills at MIDI rates; an unplugged
// device fails with -ENODEV. Never call from an audio callback.

#include "linux/midi-out-alsa.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <cstdio>
#include <cstring>

namespace ptshell {

using alsa_detail::AlsaApi;

AlsaMidiOut::AlsaMidiOut() { lib_ = alsa_detail::load_alsa(a_, "OUT"); }

AlsaMidiOut::~AlsaMidiOut() {
    close();
    // ⚠️ Not dlclose()d: SDL's ALSA backend still holds the library.
}

int AlsaMidiOut::device_count() {
    // Re-enumerated on every call — hot-plug. ⚠️ `STREAM_OUTPUT` keeps a MIDI keyboard off this list.
    if (lib_) alsa_detail::scan_rawmidi(a_, alsa_detail::STREAM_OUTPUT, devices_);
    else      devices_.clear();
    return static_cast<int>(devices_.size());
}

std::string AlsaMidiOut::device_name(int index) {
    if (index < 0 || index >= static_cast<int>(devices_.size())) return std::string();
    return devices_[index].name;
}

bool AlsaMidiOut::open(int index) {
    close();
    if (!lib_) return false;
    if (index < 0 || index >= static_cast<int>(devices_.size())) return false;

    void*     out = nullptr;
    const int rc  = a_.rawmidi_open(nullptr, &out, devices_[index].hw.c_str(), 0);   // BLOCKING
    if (rc < 0 || !out) {
        std::printf("midi:    open %s failed: %s\n", devices_[index].hw.c_str(),
                    a_.strerror_fn(rc));
        return false;
    }
    out_       = out;
    openIndex_ = index;
    broken_.store(false, std::memory_order_relaxed);
    return true;
}

void AlsaMidiOut::close() {
    if (!out_) return;

    // winmm's `midiOutReset` done by hand (MidiOutBase::panic_all_channels).
    panic_all_channels();
    a_.rawmidi_drain(out_);   // wait for them: closing first discards the buffer
    a_.rawmidi_close(out_);
    out_       = nullptr;
    openIndex_ = -1;
}

void AlsaMidiOut::send(const uint8_t* data, int len) {
    if (!out_ || len <= 0 || len > songcore::NOTE_COMMAND_BYTES) return;
    const ptrdiff_t n   = a_.rawmidi_write(out_, data, static_cast<size_t>(len));
    const bool      bad = n != static_cast<ptrdiff_t>(len);
    if (bad) ++errors_;
    // Blocking, so a negative write is a device gone (-ENODEV); only a reopen brings it back.
    if (n < 0) broken_.store(true, std::memory_order_relaxed);
    trace_message(data, len, bad);
}

}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
