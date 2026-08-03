// midi-in-alsa.{h,cpp} — the LINUX implementation of songcore::IMidiIn (MIDI plan phase E5).
//
// The second of the three input backends, and the first that ships to a real target: a USB-OTG MIDI
// keyboard on a PortMaster handheld appears through the in-tree `snd-usb-audio` driver as
// /dev/snd/midiC*D*, which is what `alsa_detail::scan_rawmidi` enumerates. Everything above
// `IMidiIn` — the byte ring, the MIDI 1.0 parser, the channel→track→instrument router, the injection —
// is native/songcore/midi_in.h and is shared with Windows; the device list, the sink and the counters
// are midi-in-base.{h,cpp}. This file is a port, a thread and a read loop.
//
// rawmidi rather than ALSA *seq*, and dlopen rather than `-lasound`, for the reasons midi-out-alsa.cpp
// sets out at length — the CFW kernels do not guarantee `snd-seq`, and the ubuntu:20.04 PortMaster
// container has no libasound-dev to link against. Nothing about the input direction changes either
// argument.
//
// ── ⚠️⚠️ WHY THERE IS A THREAD HERE AND NONE IN THE OUTPUT BACKEND ───────────────────────────────
//
// The output backend is CALLED; an input backend must WAIT. ALSA rawmidi offers no callback: the only
// way to learn that a byte arrived is to read for it. So this owns one thread whose whole life is
// `snd_rawmidi_read` → `MidiInBase::deliver`, which is the same shape winmm gets from the system (a
// callback on a thread nobody chose) reached by the only route ALSA has.
//
// ⚠️ **`std::thread`, NOT `SDL_CreateThread`, AND THE DIFFERENCE IS ANDROID.** midi-sender.cpp is an
// SDL thread precisely because it makes JNI calls and SDL's thread entry attaches the JVM. This file
// cannot run on Android at all (the `#if` below), so there is no JVM to attach to — and a plain
// `std::thread` keeps this backend linkable into `tools/ptalsain`, which has no SDL and is the only
// thing that can drive it with bytes on a machine with no MIDI hardware.
//
// ── ⚠️ NONBLOCK, AND IT IS THE OPPOSITE DECISION FROM THE OUTPUT PORT'S ──────────────────────────
//
// `AlsaMidiOut` opens BLOCKING because a non-blocking WRITE that returns -EAGAIN has **dropped the
// bytes**, and the byte most worth losing is never the one you lose (a dropped note-off is a note that
// sounds until the gear is power-cycled). A non-blocking READ that returns -EAGAIN has dropped
// nothing: it means "no byte has arrived yet", and the data is still in the driver's buffer if it ever
// comes. The asymmetry is real, so the choice is opposite:
//
//   • BLOCKING would be tidier to write and is what most examples show — and `close()` would then have
//     to unblock a thread parked inside libasound with no documented way to do it. `snd_rawmidi_close`
//     from another thread while a read is in flight is not a promise ALSA makes; a signal is worse.
//     A join that never returns is an app that hangs on quit, on a handheld, in the shutdown path.
//   • NONBLOCK + a 1 ms poll costs one wakeup per millisecond **only while a port is open**, which is
//     only when the user has picked a MIDI keyboard, and it is the same cadence the B3 sender thread
//     already runs at. The latency it can add (1 ms) is a fortieth of the frame the record will wait
//     for anyway (the drain is in `SongcoreHost::poll`, on the 60 Hz loop).
//
// ⭐ The read is drained to exhaustion before any sleep, so a burst — a chord, or a controller dumping
// its state — is delivered in one pass rather than one message per millisecond.
//
// ── ⚠️ WHAT MAY RUN ON THIS THREAD ───────────────────────────────────────────────────────────────
//
// More than on winmm's callback (this is an ordinary thread, not an interrupt-like context), but the
// rule stays the same on purpose: `deliver` is an atomic add and a memcpy under `MidiInQueue`'s lock,
// and every print about MIDI in happens on the frame loop where a MESSAGE rather than a byte is the
// useful unit. The one exception is the fatal-error line below, which prints once per session at most
// and is the only place that can say why the keyboard stopped answering.

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

    // ⚠️⚠️ **THE ORDER IS THE WHOLE OF THIS FUNCTION, and it is one operation split across three
    // lines.** The thread is inside `snd_rawmidi_read(in_, ...)` most of the time; closing the handle
    // first would hand it a freed `snd_rawmidi_t*` — a use-after-free during teardown, which is the
    // least debuggable moment available. So: ask it to stop, WAIT for it to have stopped, and only then
    // take the handle away. NONBLOCK is what bounds that wait at one poll interval (see the header).
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

        // Anything else is the wire or the driver. -ENODEV is a cable pulled out, and it will repeat
        // forever, so this thread stops rather than spinning on it — but it says so first, because a
        // reader thread that has quietly died is indistinguishable from a keyboard nobody is playing,
        // which is the exact ambiguity every counter in this backend exists to remove.
        readErrors_.fetch_add(1, std::memory_order_relaxed);
        note_port_error();
        dead_.store(true, std::memory_order_relaxed);
        std::printf("midi in: read failed (%s) - the input port has stopped; re-pick it on the MIDI "
                    "screen to resume\n",
                    a_.strerror_fn(static_cast<int>(n)));
        std::fflush(stdout);
        return;
    }
}

}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
