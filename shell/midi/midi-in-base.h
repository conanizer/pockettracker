#ifndef POCKETTRACKER_SHELL_MIDI_IN_BASE_H
#define POCKETTRACKER_SHELL_MIDI_IN_BASE_H

// What every MIDI input backend shares (winmm, ALSA rawmidi, MidiManager): the device list, the spec
// resolver, and `deliver()`, the one door bytes come through, so they are counted.
// winmm and ALSA push from their own thread; Android is polled through `pump()`.
// ⚠️ `deliver()` runs on the platform's callback thread, where winmm forbids printf and malloc
// (deadlock): it stays a clock read, an atomic add and a memcpy. Printing happens in
// `SongcoreHost::poll()`.

#include <atomic>
#include <cstdint>
#include <string>

#include "songcore/midi_in.h"

namespace ptshell {

class MidiInBase : public songcore::IMidiIn {
  public:
    // An index, a case-insensitive name fragment, or empty/"list" → a device NAME ("" if none),
    // printing the device list first. ⚠️ It does not open (unlike `MidiOutBase::open_by_spec`): only
    // the dispatcher's `boot_midi_in_port` opens, once the sink exists.
    std::string resolve_spec(const std::string& spec);

    // Bytes the backend delivered, ever, whether or not anything drained them.
    uint64_t bytes_received() const { return bytes_.load(std::memory_order_relaxed); }
    // How many times the callback fired.
    uint64_t callbacks() const { return callbacks_.load(std::memory_order_relaxed); }
    // Messages the port rejected as malformed (winmm's MIM_ERROR) — a wiring fault.
    uint64_t port_errors() const { return errors_.load(std::memory_order_relaxed); }

    int open_index() const { return openIndex_; }

    // Once a tick from the frame loop; a no-op for a pushing backend. Android's bytes arrive only here.
    virtual void pump() {}

    // Bytes arrive only through `pump()`, so the loop keeps ticking fast while open.
    virtual bool polled() const { return false; }

    // ⚠️ Atomic: `set_sink(nullptr)` stops delivery at shutdown while the port may still run.
    void set_sink(songcore::IMidiInSink* sink) override {
        sink_.store(sink, std::memory_order_release);
    }

  protected:
    // What a backend's callback calls with its bytes. ⚠️ Callback-thread rules: no allocation,
    // printing or blocking.
    void deliver(const uint8_t* data, int len);

    // Counted, never printed from here.
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
