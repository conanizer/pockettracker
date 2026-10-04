#ifndef POCKETTRACKER_SHELL_MIDI_OUT_BASE_H
#define POCKETTRACKER_SHELL_MIDI_OUT_BASE_H

// midi-out-base.{h,cpp} — the part of a `songcore::IMidiOut` backend that is NOT per-platform.
//
// `IMidiOut` is five methods (songcore/midi_out.h); a usable backend also needs a console device
// list, a spec resolver for the env-var override, a test note, a byte trace and a rejected-message
// counter — none of it platform-specific, so written once here for every backend.
//
// ⚠️ The counters are load-bearing: `IMidiOut::send` returns void, so a silently failing send is
// otherwise indistinguishable from one that works. Every console line prints the count.

#include <cstdint>
#include <string>

#include "songcore/midi_out.h"

namespace ptshell {

class MidiOutBase : public songcore::IMidiOut {
  public:
    /**
     * Resolve `spec` — an index, a case-insensitive name fragment, or empty/"list" — and open it.
     *
     * Always prints the device list first: otherwise "nothing happened" is indistinguishable from
     * "there was no device".
     */
    bool open_by_spec(const std::string& spec);

    /**
     * One C-4 on channel 1, held for `holdMs`, then released — the TEST row, reachable from the
     * console before a frame is drawn. It bypasses the bus, so a failure here means the PORT, not
     * songcore.
     */
    void test_note(int holdMs);

    /** Print every message as it leaves (POCKETTRACKER_MIDI_TRACE=1). */
    void set_trace(bool on) { trace_ = on; }

    int error_count() const { return errors_; }
    int sent_count() const { return sent_; }
    int open_index() const { return openIndex_; }

  protected:
    /**
     * All-notes-off (CC 123) on all 16 channels — what EVERY backend must send before it closes.
     *
     * ⚠️ Closing a port with notes sounding leaves the DEVICE holding them for good. songcore's panic
     * covers the channels it knows; this covers the rest. Windows gets it from `midiOutReset`; ALSA
     * and Android write these bytes, once, here.
     */
    void panic_all_channels();

    /**
     * The console monitor, called by every backend's `send` after it has written the bytes.
     *
     * The bring-up instrument for a machine with no MIDI monitor attached: without it, "the synth is
     * silent" cannot be told from "nothing was sent", and those two have completely different causes
     * in completely different files.
     */
    void trace_message(const uint8_t* data, int len, bool rejected) const;

    int  openIndex_ = -1;
    int  sent_      = 0;
    int  errors_    = 0;
    bool trace_     = false;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_SHELL_MIDI_OUT_BASE_H
