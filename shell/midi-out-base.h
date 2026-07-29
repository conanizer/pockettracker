#ifndef POCKETTRACKER_SHELL_MIDI_OUT_BASE_H
#define POCKETTRACKER_SHELL_MIDI_OUT_BASE_H

// midi-out-base.{h,cpp} — the part of a `songcore::IMidiOut` backend that is NOT per-platform.
//
// `IMidiOut` is five methods and deliberately tiny (see native/songcore/midi_out.h). But a backend
// needs more than five methods to be usable at a desk: a device list on the console, a spec resolver
// for the env-var override, a one-shot test note, a byte trace, and a rejected-message counter.
// None of that touches the platform — it is written entirely in terms of the five virtuals — and
// with three backends coming (winmm, ALSA, Android) it is exactly the kind of thing that gets
// copied twice and then diverges in one of the copies.
//
// ⚠️ The counters in particular are load-bearing rather than decorative. `IMidiOut::send` returns
// void, because nothing above it could do anything useful with a failure mid-phrase — so a send path
// that fails silently is indistinguishable from one that works. Every backend counts, and every
// console line prints the count beside the verdict.

#include <cstdint>
#include <string>

#include "songcore/midi_out.h"

namespace ptshell {

class MidiOutBase : public songcore::IMidiOut {
  public:
    /**
     * Resolve `spec` — an index, a case-insensitive name fragment, or empty/"list" — and open it.
     *
     * Always prints the device list first. That is the console's job: without it, "nothing happened"
     * is indistinguishable from "there was no device", which is the single most common way an hour
     * goes missing on a MIDI bring-up.
     */
    bool open_by_spec(const std::string& spec);

    /**
     * One C-4 on channel 1, held for `holdMs`, then released — the plan's §8.1 TEST row, reachable
     * from the console before a frame is drawn. It is the smallest thing that answers "is the cable
     * alive?", and it deliberately does NOT go through the bus, so a failure here means the PORT and
     * not songcore.
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
     * ⚠️ Not politeness: closing a port with notes sounding leaves the DEVICE holding them, and
     * nothing we can ever send again will stop it. songcore's own panic runs one layer up and covers
     * the channels it knows about; this covers the ones it does not — a channel some earlier session
     * left ringing, or a device's own power-on state.
     *
     * Windows gets this from `midiOutReset` and calls this not at all; ALSA and Android have no reset
     * call, so the bytes are theirs to write, and writing them ONCE here is what stops the two from
     * drifting apart on which channels or which controller number.
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
