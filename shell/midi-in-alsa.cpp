// midi-in-alsa.{h,cpp} — the LINUX implementation of songcore::IMidiIn: a USB MIDI keyboard appears
// via `snd-usb-audio` as /dev/snd/midiC*D*, which `alsa_detail::scan_rawmidi` enumerates. Everything
// above `IMidiIn` is songcore/midi_in.h; the device list, sink and counters are midi-in-base. This
// file is a port, a thread and a read loop. rawmidi and dlopen for the reasons in midi-out-alsa.cpp.
//
// ── ⚠️⚠️ WHY THERE IS A THREAD HERE AND NONE IN THE OUTPUT BACKEND ───────────────────────────────
//
// ALSA rawmidi offers no callback: the only way to learn a byte arrived is to read for it. So one
// thread runs `snd_rawmidi_read` → `MidiInBase::deliver`. A plain `std::thread`, not SDL's: there is
// no JVM to attach on Linux, and it keeps this backend usable without SDL.
//
// ── ⚠️ NONBLOCK — THE OPPOSITE OF THE OUTPUT PORT ────────────────────────────────────────────────
//
// A non-blocking WRITE that returns -EAGAIN has dropped bytes; a non-blocking READ has dropped
// nothing. BLOCKING would leave `close()` unable to unblock a thread parked inside libasound (a quit
// that hangs); NONBLOCK + a 1 ms poll costs a wakeup per ms only while a port is open. ⭐ Each read
// drains to exhaustion before sleeping, so a burst arrives in one pass.
//
// ── ⚠️ WHAT MAY RUN ON THIS THREAD ───────────────────────────────────────────────────────────────
//
// The callback rule, kept on purpose: `deliver` is an atomic add and a memcpy; MIDI-in printing
// happens on the frame loop. The one exception is the fatal-error line below (once per session).

#include "midi-in-alsa.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <cerrno>
#include <chrono>
#include <cstdio>

namespace ptshell {

namespace {

/** How long to wait after a read that found nothing. See the NONBLOCK note in the header comment. */
constexpr int POLL_MS = 1;

/**
 * One read's worth of bytes. Generous on purpose: `MidiInQueue` resyncs a split message by
 * construction (it is a BYTE ring, not a message ring), so the only thing a small buffer would buy is
 * more trips through the loop for the same data.
 */
constexpr int READ_BUF = 256;

}  // namespace

AlsaMidiIn::AlsaMidiIn() { lib_ = alsa_detail::load_alsa(a_, "IN"); }

AlsaMidiIn::~AlsaMidiIn() { close(); }

int AlsaMidiIn::device_count() {
    // Re-enumerates on every call, because MIDI is hot-pluggable and a port list is only true at the
    // moment it is read — `InputDispatcher::refresh_midi_in_devices` walks it that way.
    //
    // ⚠️ `STREAM_INPUT` is the filter, and it is the mirror of the output backend's: get it backwards
    // and the INPUT row lists the user's synth and hides their keyboard. See alsa-rawmidi.h.
    if (lib_) alsa_detail::scan_rawmidi(a_, alsa_detail::STREAM_INPUT, devices_);
    else      devices_.clear();
    return static_cast<int>(devices_.size());
}

std::string AlsaMidiIn::device_name(int index) {
    if (index < 0 || index >= static_cast<int>(devices_.size())) return std::string();
    return devices_[index].name;
}

bool AlsaMidiIn::open(int index) {
    close();
    if (!lib_) return false;
    if (index < 0 || index >= static_cast<int>(devices_.size())) return false;

    void*     in = nullptr;
    const int rc = a_.rawmidi_open(&in, nullptr, devices_[index].hw.c_str(), alsa_detail::NONBLOCK);
    if (rc < 0 || !in) {
        std::printf("midi:    IN open %s failed: %s\n", devices_[index].hw.c_str(), a_.strerror_fn(rc));
        std::fflush(stdout);
        return false;
    }

    in_        = in;
    openIndex_ = index;
    quit_.store(false, std::memory_order_relaxed);
    dead_.store(false, std::memory_order_relaxed);

    // ⚠️ LAST, and it is the whole reason the fields above are set first: the thread reads `in_` on its
    // very first iteration. Starting it before the handle is stored is a read through a null pointer
    // that happens only on a machine slow enough to lose the race.
    thread_ = std::thread(&AlsaMidiIn::reader, this);
    return true;
}

void AlsaMidiIn::close() {
    if (!in_) return;

    // ⚠️⚠️ THE ORDER IS THE WHOLE FUNCTION: the thread is usually inside `snd_rawmidi_read(in_, ...)`,
    // so ask it to stop, WAIT for it, and only then close the handle. NONBLOCK bounds the wait.
    quit_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();

    a_.rawmidi_close(in_);
    in_        = nullptr;
    openIndex_ = -1;
}

void AlsaMidiIn::reader() {
    uint8_t buf[READ_BUF];

    while (!quit_.load(std::memory_order_relaxed)) {
        const ptrdiff_t n = a_.rawmidi_read(in_, buf, sizeof buf);

        if (n > 0) {
            deliver(buf, static_cast<int>(n));
            continue;   // ⭐ drain the burst before sleeping — see the header
        }

        // -EAGAIN is the ordinary "nothing has arrived", and -EINTR is a signal landing on this thread
        // (the desktop shell installs a SIGTERM handler). Neither is an error and neither loses a byte.
        if (n == -EAGAIN || n == -EINTR || n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
            continue;
        }

        // Anything else is the wire or the driver (-ENODEV: a cable pulled), and it repeats forever,
        // so the thread stops — saying so first, or a dead reader looks like a keyboard nobody plays.
        readErrors_.fetch_add(1, std::memory_order_relaxed);
        note_port_error();
        dead_.store(true, std::memory_order_relaxed);
        std::printf("midi in: read failed (%s) - the input port has stopped; it reopens by itself when "
                    "the device is back\n",
                    a_.strerror_fn(static_cast<int>(n)));
        std::fflush(stdout);
        return;
    }
}

}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
