// midi-out-alsa.{h,cpp} — the LINUX implementation of songcore::IMidiOut (MIDI plan phase B2b).
//
// The second of the three backends (§4.3 / §4.5): Windows = winmm, **Linux = ALSA rawmidi**, Android
// = a JNI up-call into MidiManager. Nothing above `IMidiOut` is per-platform — the serializer, the
// note lifecycle, the release queue and the 0–255 → 0–127 scaling are native/songcore/midi_out.h and
// are shared by all three; the console half (device list, spec resolver, TEST, trace, counters) is
// midi-out-base.{h,cpp}. This file is the whole Linux difference, and it is small on purpose.
//
// ── WHY rawmidi AND NOT ALSA seq ─────────────────────────────────────────────────────────────────
//
// The plan (§4.5) settled this: ALSA *seq* — the API that gives you virtual ports and a routing
// graph — needs the `snd-seq` kernel module, and the PortMaster CFW kernels (muOS, ROCKNIX, ArkOS,
// Knulli, AmberELEC) do not guarantee it. rawmidi is the floor that works everywhere: a USB-OTG MIDI
// interface appears through the in-tree `snd-usb-audio` driver as /dev/snd/midiC*D*, which is exactly
// what this file enumerates. The cost is that there are no virtual/app-to-app ports on Linux — on a
// handheld there is nothing to route to anyway, and on a desktop the user can load `snd-virmidi`.
//
// ── WHY dlopen AND NOT -lasound ──────────────────────────────────────────────────────────────────
//
// ⚠️ Three reasons, and the first is the one that decides it:
//
//   1. **The PortMaster build container has no libasound-dev, and giving it one is not free.** The
//      container (shell/Dockerfile.portmaster) is ubuntu:20.04 — pinned there as the GLIBC FLOOR —
//      and it cross-compiles to aarch64. A link-time dependency would need `libasound2-dev:arm64`,
//      i.e. dpkg multiarch inside that image, for a library we call sixteen functions in.
//   2. **A missing libasound would otherwise stop the whole app from starting.** With `-lasound`,
//      a CFW without the library gives the user a dynamic-linker error instead of a tracker. With
//      dlopen, MIDI is simply unavailable: `device_count()` answers 0 and the OUTPUT row draws
//      `OFF  NO PORTS`, which is a state the screen already has a design for.
//   3. It keeps `shell/CMakeLists.txt` honest about the port's headline claim — SDL2 and nothing
//      else. libasound is on every CFW because SDL2's audio backend sits on it, so the dlopen
//      practically always succeeds; it is the *build* that must not require it.
//
// The price is that the prototypes are hand-copied and nothing checks them. That is what
// `tools/ptalsa` is for — see the comment on `AlsaApi`, which since E5 lives in **alsa-rawmidi.h**
// along with the loader and the device walk, both now shared with the INPUT backend (midi-in-alsa.cpp).
//
// ── THREADING AND BLOCKING ───────────────────────────────────────────────────────────────────────
//
// `send` is called from whichever thread pumps the queue — **since B3 that is the sender thread**
// (shell/midi-sender.cpp), plus the frame loop for a panic's immediate note-offs. The two are
// serialised by `ExternalConsumer`'s mutex, which is what lets this backend keep no lock of its own.
// ⚠️ That mutex is held across the write below, so a write that BLOCKS stalls the producer — see the
// blocking argument that follows for why the bound is a few hundred bytes per second and not a stall.
// The port is opened in BLOCKING mode (`snd_rawmidi_open` mode 0) rather than
// SND_RAWMIDI_NONBLOCK, and that is deliberate: non-blocking `snd_rawmidi_write` returns -EAGAIN
// when the driver buffer is full and the bytes are simply **lost**, and the byte most worth losing
// is never the one you lose — a dropped note-off is a note that sounds until the gear is
// power-cycled. Blocking cannot bite here: the rawmidi output buffer defaults to 4 KB against a
// worst case of a few hundred bytes per second, and a device unplugged mid-write fails with -ENODEV
// rather than waiting. It must NOT be called from an audio callback, and nothing does.

#include "midi-out-alsa.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <cstdio>
#include <cstring>

namespace ptshell {

using alsa_detail::AlsaApi;

AlsaMidiOut::AlsaMidiOut() { lib_ = alsa_detail::load_alsa(a_, "OUT"); }

AlsaMidiOut::~AlsaMidiOut() {
    close();
    // ⚠️ The library is NOT dlclose()d here. This object outlives nothing — main.cpp owns it for the
    // process — and unloading libasound while SDL's ALSA audio backend still holds it is the kind of
    // teardown-order question with no upside. The process exit does it.
}

int AlsaMidiOut::device_count() {
    // Re-enumerates on every call, because MIDI is hot-pluggable and a port list is only true at the
    // moment it is read. InputDispatcher::refresh_midi_devices calls this on every entry to the MIDI
    // screen and then walks device_name(0..n-1), so the cache this fills is always the fresh one.
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
    trace_message(data, len, bad);
}

}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
