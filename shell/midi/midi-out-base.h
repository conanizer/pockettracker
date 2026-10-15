#ifndef POCKETTRACKER_SHELL_MIDI_OUT_BASE_H
#define POCKETTRACKER_SHELL_MIDI_OUT_BASE_H

// What every MIDI output backend shares: the device list, the env-var spec resolver, the test note,
// the byte trace and the counters. ⚠️ `IMidiOut::send` returns void, so the counters are the only way
// to tell a failing send from a working one.

#include <cstdint>
#include <string>

#include "songcore/midi_out.h"

namespace ptshell {

class MidiOutBase : public songcore::IMidiOut {
  public:
    // An index, a case-insensitive name fragment, or empty/"list" → open it. Prints the device list
    // first, so "nothing happened" differs from "no device".
    bool open_by_spec(const std::string& spec);

    // One C-4 on channel 1 for `holdMs` — the TEST row. It bypasses the bus, so a failure is the port.
    void test_note(int holdMs);

    // POCKETTRACKER_MIDI_TRACE=1: print every message as it leaves.
    void set_trace(bool on) { trace_ = on; }

    int open_index() const { return openIndex_; }

  protected:
    // CC 123 on all 16 channels, before every close. ⚠️ A port closed with notes sounding leaves the
    // device holding them; songcore's panic covers only its own channels. Windows has `midiOutReset`.
    void panic_all_channels();

    // The console monitor, after every send: "the synth is silent" vs "nothing was sent".
    void trace_message(const uint8_t* data, int len, bool rejected) const;

    int  openIndex_ = -1;
    int  errors_    = 0;
    bool trace_     = false;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_SHELL_MIDI_OUT_BASE_H
