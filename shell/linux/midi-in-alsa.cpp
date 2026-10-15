// Linux MIDI input over ALSA rawmidi (a USB keyboard appears as /dev/snd/midiC*D*), dlopen'd as in
// midi-out-alsa.cpp. rawmidi has no callback, so a reader thread (plain std::thread — no JVM here)
// runs `snd_rawmidi_read` → `deliver`.
// ⚠️ NONBLOCK, unlike the output: a non-blocking read loses nothing, and a blocking one would leave
// `close()` unable to free a thread parked inside libasound. A 1 ms poll while open; each pass drains
// the burst. Only `deliver` and the once-per-session fatal line run on the thread.

#include "linux/midi-in-alsa.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <cerrno>
#include <chrono>
#include <cstdio>

namespace ptshell {

namespace {

// After a read that found nothing.
constexpr int POLL_MS = 1;

// Generous: `MidiInQueue` is a byte ring and resyncs a split message, so a small buffer only adds trips.
constexpr int READ_BUF = 256;

}  // namespace

AlsaMidiIn::AlsaMidiIn() { lib_ = alsa_detail::load_alsa(a_, "IN"); }

AlsaMidiIn::~AlsaMidiIn() { close(); }

int AlsaMidiIn::device_count() {
    // Re-enumerated on every call — MIDI is hot-pluggable. ⚠️ `STREAM_INPUT` is the filter; backwards,
    // the INPUT row would list the user's synth and hide their keyboard.
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

    // ⚠️ Last: the thread reads `in_` on its first iteration.
    thread_ = std::thread(&AlsaMidiIn::reader, this);
    return true;
}

void AlsaMidiIn::close() {
    if (!in_) return;

    // ⚠️ The thread is usually inside a read on `in_`: stop it, join it, then close. NONBLOCK bounds the wait.
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
            continue;   // drain the burst before sleeping
        }

        // -EAGAIN is "nothing yet", -EINTR a signal on this thread (the shell's SIGTERM handler).
        if (n == -EAGAIN || n == -EINTR || n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
            continue;
        }

        // Anything else (-ENODEV: a cable pulled) repeats forever, so the thread stops — saying so, or
        // a dead reader looks like a keyboard nobody plays.
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
