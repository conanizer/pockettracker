#ifndef POCKETTRACKER_SHELL_MIDI_IN_BASE_H
#define POCKETTRACKER_SHELL_MIDI_IN_BASE_H

// midi-in-base.{h,cpp} — the part of a `songcore::IMidiIn` backend that is NOT per-platform.
//
// The mirror of midi-out-base.h, written for the same reason and at the same layer: there are three
// backends (winmm at E2, ALSA rawmidi and Android's `MidiManager` at E5), and the device list, the spec
// resolver and the counters are identical for all three. Written once here they cannot diverge in one
// of the copies; written three times they will.
//
// ⚠️ The three differ in ONE thing above the platform line — how a byte gets here. winmm and ALSA PUSH
// (a system callback, a reader thread of ours); Android is POLLED through `pump()` below. That is one
// virtual with an empty default, not a second mechanism — see the note on it.
//
// ⚠️ **IT ALSO OWNS THE SINK, AND THAT IS NOT TIDINESS.** A backend's bytes arrive on a thread nobody
// chose, and `deliver()` below is the ONE door they come through — so it is the one place that can
// count them. Without that count, an input port has the worst failure mode in this repo: a component
// whose correct behaviour is SILENCE, indistinguishable from one whose callback never fired at all.
// `bytes_received()` and `callbacks()` are what tell those two apart, and every console line prints
// them beside the verdict.
//
// ── ⚠️⚠️ WHAT MAY RUN ON THE CALLBACK THREAD ─────────────────────────────────────────────────────
//
// Almost nothing, and the constraint is the platform's rather than ours. winmm documents its low-level
// MIDI callback as being allowed to call only a short list of functions — `EnterCriticalSection`,
// `midiOutShortMsg`, `PostMessage`, `SetEvent` and a handful more — because the callback can run in an
// interrupt-like context where the CRT's locks are not available: **a `printf` or a `malloc` there is a
// deadlock, not a slow path.** So `deliver()` does exactly two things, an atomic add and a call into
// `MidiInQueue::on_bytes` (a `std::mutex` and a memcpy, no allocation), and the TRACE that would be the
// obvious instrument here does not exist: the console prints from `SongcoreHost::poll()`, on the app's
// own thread, where a message rather than a byte is the useful unit anyway.

#include <atomic>
#include <cstdint>
#include <string>

#include "songcore/midi_in.h"

namespace ptshell {

class MidiInBase : public songcore::IMidiIn {
  public:
    /**
     * Resolve `spec` — an index, a case-insensitive name fragment, or empty/"list" — to a device NAME.
     *
     * ⚠️ **IT RESOLVES AND DOES NOT OPEN, WHERE `MidiOutBase::open_by_spec` OPENS.** The difference is
     * the sink: an input port that is open is a port that can deliver bytes, and the object those bytes
     * go into (`SongcoreHost`'s queue) does not exist yet when `main` reads its environment. So the env
     * var hands a NAME to the settings, and the one place that knows how to wire a sink — the
     * dispatcher's `boot_midi_in_port` — is the only thing that ever calls `open`. That is B4.3's "one
     * owner of which port is open", reached from the other direction.
     *
     * Returns "" for no match. Always prints the device list first, for `MidiOutBase`'s reason: on a
     * machine where nothing arrives, "there were 0 devices" and "there were 3 and none matched" are
     * different problems, and the console is where that is cheapest to answer.
     */
    std::string resolve_spec(const std::string& spec);

    /** Bytes delivered by the backend's own thread, ever, whether or not anything drained them. */
    uint64_t bytes_received() const { return bytes_.load(std::memory_order_relaxed); }
    /** How many times the backend's callback has fired. ⭐ The "I woke up" line, as a number. */
    uint64_t callbacks() const { return callbacks_.load(std::memory_order_relaxed); }
    /** Messages the PORT rejected as malformed (winmm's MIM_ERROR). Nonzero means a wiring fault. */
    uint64_t port_errors() const { return errors_.load(std::memory_order_relaxed); }

    int open_index() const { return openIndex_; }

    /**
     * Called once a frame, immediately before the drain. A no-op for a backend that PUSHES.
     *
     * ⚠️ **IT EXISTS FOR ANDROID (E5) AND IT IS NOT A SECOND MECHANISM.** winmm calls us on its own
     * callback thread and ALSA gets a reader thread of ours; `MidiManager` delivers on a binder thread
     * to the Kotlin side, and this backend fetches from there rather than having Kotlin call down —
     * midi-in-android.cpp argues why, and the short version is that it costs nothing, because the bytes
     * a push would deposit are not looked at until `SongcoreHost::poll()` drains them on this same
     * frame anyway.
     *
     * ⚠️ **THE CALL SITE MUST STAY IMMEDIATELY ABOVE `host.poll()`** (app.cpp). Below it, every pumped
     * byte waits a whole extra frame; that would be invisible on the two platforms whose backends
     * ignore this call, which is precisely what makes it worth writing down here.
     */
    virtual void pump() {}

    // ⚠️ Both of these are ATOMIC because they race with the callback thread by construction:
    // `set_sink(nullptr)` is what a shutting-down app uses to guarantee no further delivery, and it is
    // called while the port may still be running.
    void set_sink(songcore::IMidiInSink* sink) override {
        sink_.store(sink, std::memory_order_release);
    }

  protected:
    /**
     * What a backend's callback calls with the bytes it just received. The ONE door.
     *
     * ⚠️ See the header note: this runs under the platform's callback rules, so it must stay an atomic
     * add and a memcpy. Anything that allocates, prints or blocks belongs in the drain instead.
     */
    void deliver(const uint8_t* data, int len);

    /** A message the port itself reported as malformed. Counted, never printed from here. */
    void note_port_error() { errors_.fetch_add(1, std::memory_order_relaxed); }

    int openIndex_ = -1;

  private:
    std::atomic<songcore::IMidiInSink*> sink_{nullptr};
    std::atomic<uint64_t> bytes_{0};
    std::atomic<uint64_t> callbacks_{0};
    std::atomic<uint64_t> errors_{0};
};

}  // namespace ptshell

#endif  // POCKETTRACKER_SHELL_MIDI_IN_BASE_H
