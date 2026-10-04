#ifndef POCKETTRACKER_SHELL_MIDI_IN_BASE_H
#define POCKETTRACKER_SHELL_MIDI_IN_BASE_H

// midi-in-base.{h,cpp} — the part of a `songcore::IMidiIn` backend that is NOT per-platform: the
// device list, the spec resolver and the counters, shared by winmm, ALSA rawmidi and `MidiManager`.
//
// ⚠️ The backends differ in how a byte arrives: winmm and ALSA PUSH (a system callback, our reader
// thread); Android is POLLED through `pump()`. A pushed byte reaches the engine's next block on its
// own, a polled one waits for the loop's tick — `polled()` keeps the loop fast for that backend.
//
// ⚠️ IT OWNS THE SINK, AND THAT IS NOT TIDINESS: `deliver()` is the ONE door bytes come through, so
// it counts them. An input port whose correct behaviour is silence is otherwise indistinguishable
// from one that never fired; `bytes_received()` and `callbacks()` tell them apart.
//
// ── ⚠️⚠️ WHAT MAY RUN ON THE CALLBACK THREAD ─────────────────────────────────────────────────────
//
// Almost nothing: winmm's MIDI callback may call only a short list of functions, from a context
// where the CRT's locks are unavailable — a `printf` or `malloc` there is a DEADLOCK. So `deliver()`
// is an atomic add and `MidiInQueue::on_bytes` (a mutex and a memcpy); printing happens in
// `SongcoreHost::poll()`, on the app's own thread.

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
     * ⚠️ IT RESOLVES AND DOES NOT OPEN (unlike `MidiOutBase::open_by_spec`): the sink does not exist
     * when `main` reads its environment, so the env var hands a NAME to the settings and only the
     * dispatcher's `boot_midi_in_port` ever calls `open` — one owner of which port is open.
     *
     * Returns "" for no match. Prints the device list first: "0 devices" and "3, none matched" are
     * different problems.
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
     * Called once a tick by the frame loop. A no-op for a backend that PUSHES.
     *
     * ⚠️ FOR ANDROID: `MidiManager` delivers on a binder thread to the Kotlin side, and this backend
     * fetches from there (midi-in-android.cpp says why). A pumped byte waits for this call before the
     * audio thread's drain sees it; `polled()` below tells the loop to keep ticking fast.
     */
    virtual void pump() {}

    /** True for a backend whose bytes only arrive through `pump()` — the frame loop must keep ticking
     *  fast while such a port is open. False for a backend that pushes from its own thread. */
    virtual bool polled() const { return false; }

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
