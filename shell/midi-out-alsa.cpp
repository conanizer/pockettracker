// midi-out-alsa.{h,cpp} — the LINUX implementation of songcore::IMidiOut. Everything above
// `IMidiOut` (serializer, note lifecycle, release queue, scaling) is songcore/midi_out.h; the console
// half is midi-out-base. This file is the whole Linux difference.
//
// ── WHY rawmidi AND NOT ALSA seq ─────────────────────────────────────────────────────────────────
//
// seq needs the `snd-seq` kernel module, which the PortMaster CFW kernels do not guarantee. rawmidi
// is the floor that works everywhere: a USB MIDI interface appears via `snd-usb-audio` as
// /dev/snd/midiC*D*. The cost is no virtual ports on Linux (a desktop user can load `snd-virmidi`).
//
// ── WHY dlopen AND NOT -lasound ──────────────────────────────────────────────────────────────────
//
//   1. The PortMaster build container (ubuntu:20.04, the glibc floor, cross-compiling to aarch64)
//      has no libasound-dev; adding it means multiarch dpkg for a sixteen-function dependency.
//   2. With -lasound a CFW without the library cannot even start the app; with dlopen MIDI is just
//      unavailable and the OUTPUT row draws `OFF  NO PORTS`.
//   3. The build stays SDL2 and nothing else; at runtime libasound is everywhere SDL2 audio is.
// The price: hand-copied prototypes (alsa-rawmidi.h).
//
// ── THREADING AND BLOCKING ───────────────────────────────────────────────────────────────────────
//
// `send` is called by the sender thread (midi-sender.cpp) and by the frame loop for a panic's
// immediate note-offs, serialised by `ExternalConsumer`'s mutex — held across the write. The port is
// opened BLOCKING: a non-blocking write returning -EAGAIN LOSES the bytes, and a lost note-off sounds
// until the gear is power-cycled. Blocking cannot bite: a 4 KB driver buffer against a few hundred
// bytes a second, and an unplugged device fails with -ENODEV. Never from an audio callback.

#include "midi-out-alsa.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <cstdio>
#include <cstring>

namespace ptshell {

using alsa_detail::AlsaApi;

AlsaMidiOut::AlsaMidiOut() { lib_ = alsa_detail::load_alsa(a_, "OUT"); }

AlsaMidiOut::~AlsaMidiOut() {
    close();
    // ⚠️ Not dlclose()d: SDL's ALSA audio backend still holds the library. The process exit does it.
}

int AlsaMidiOut::device_count() {
    // Re-enumerates on every call: MIDI is hot-pluggable, and a port list is true only when read.
    //
    // ⚠️ `STREAM_OUTPUT` is the filter that keeps a MIDI KEYBOARD off this list — see alsa-rawmidi.h.
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

    // The equivalent of winmm's `midiOutReset`; ALSA has no such call, so the bytes are ours to write.
    // Shared with the Android backend — see MidiOutBase::panic_all_channels for why it must happen.
    panic_all_channels();
    a_.rawmidi_drain(out_);   // and WAIT for them: closing first would discard the buffer
    a_.rawmidi_close(out_);
    out_       = nullptr;
    openIndex_ = -1;
}

void AlsaMidiOut::send(const uint8_t* data, int len) {
    if (!out_ || len <= 0 || len > 3) return;
    ++sent_;
    const ptrdiff_t n   = a_.rawmidi_write(out_, data, static_cast<size_t>(len));
    const bool      bad = n != static_cast<ptrdiff_t>(len);
    if (bad) ++errors_;
    // The port is opened blocking, so a negative write is never "try again": it is a device that has
    // gone (-ENODEV on a pulled USB cable), and only a reopen brings it back.
    if (n < 0) broken_.store(true, std::memory_order_relaxed);
    trace_message(data, len, bad);
}

}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
