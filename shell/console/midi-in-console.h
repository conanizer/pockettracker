#ifndef POCKETTRACKER_SHELL_MIDI_IN_CONSOLE_H
#define POCKETTRACKER_SHELL_MIDI_IN_CONSOLE_H

#include <SDL.h>

#include <cstdint>
#include <cstdio>

#include "songcore/midi_in.h"

namespace songcore { class SongcoreHost; }

namespace ptshell {

class MidiInBase;

// Counts what MIDI input delivered, printed at exit — inside the app a working input and a dead one
// look the same. Runs on the frame loop's thread, so it may print.
struct MidiInConsole : songcore::IMidiInObserver {
    bool     trace    = false;   // POCKETTRACKER_MIDI_IN_TRACE=1
    uint64_t messages = 0;
    uint64_t records  = 0;
    uint64_t silent   = 0;       // messages that produced no bus record at all

    // CC arrival rates while a knob turns: per poll, per 16 ms frame, peak per second. ⚠️ Every
    // controller number counts, so two knobs read as one stream; one poll shares one timestamp.
    uint64_t ccTotal   = 0;
    uint64_t ccFirstMs = 0;      // wall clock of the first and last CC — the span the rate is over
    uint64_t ccLastMs  = 0;
    uint64_t ccDrains  = 0;      // drains that carried at least one CC
    uint64_t ccMaxPerDrain = 0;
    uint64_t drain     = 0;      // the poll that took the messages back: bumped above host.poll()

    // Most CCs inside one fixed `width`-ms window; a burst across a boundary counts twice, so a floor.
    struct Window {
        uint64_t width;
        uint64_t id  = ~0ull;    // a value no window index can have, so the first CC opens one
        uint64_t n   = 0;
        uint64_t max = 0;

        void add(uint64_t nowMs) {
            const uint64_t w = nowMs / width;
            if (w != id) { id = w; n = 0; }
            if (++n > max) max = n;
        }
    };
    Window w16{16}, w100{100}, w1000{1000};

    void count_cc(uint64_t nowMs) {
        if (ccTotal++ == 0) ccFirstMs = nowMs;
        ccLastMs = nowMs;

        if (drain != ccDrain_) { ccDrain_ = drain; ccInDrain_ = 0; ++ccDrains; }
        ++ccInDrain_;
        if (ccInDrain_ > ccMaxPerDrain) ccMaxPerDrain = ccInDrain_;

        w16.add(nowMs);
        w100.add(nowMs);
        w1000.add(nowMs);
    }

    void on_midi_in(const songcore::MidiInMessage& m, const songcore::Event* ev, int n) override {
        ++messages;
        records += static_cast<uint64_t>(n < 0 ? 0 : n);
        if (n <= 0) ++silent;
        if (m.status == songcore::EV_CC) count_cc(static_cast<uint64_t>(SDL_GetTicks64()));
        if (!trace) return;

        // Rebuilt from the parsed message, so a line that disagrees with the cable is a parser bug.
        const unsigned status = m.is_channel() ? (m.status | m.channel) : m.status;
        if (m.status == songcore::MIDI_SYSEX) {
            std::printf("midi ->  note command ch %d note %02X id %d value %02X %s -> %d event(s)", m.channel + 1,
                        m.data1, m.id, m.data2, m.kind == songcore::NOTE_COMMAND_WITH_NOTE ? "(with the note)" : "(now)", n);
        } else {
            std::printf("midi ->  %02X", status);
            if (m.len > 1) std::printf(" %02X", m.data1);
            if (m.len > 2) std::printf(" %02X", m.data2);
            std::printf("%*s-> %d event(s)", (m.len > 2 ? 3 : (m.len > 1 ? 6 : 9)), "", n);
        }
        for (int i = 0; i < n; ++i)
            std::printf("  [track %d inst %d]", ev[i].track, static_cast<int>(ev[i].instrument));
        std::printf("\n");
        std::fflush(stdout);
    }

  private:
    uint64_t ccDrain_ = ~0ull, ccInDrain_ = 0;
};

// The MIDI-in report at exit: a count per stage, so a break can be located.
void print_midi_in_report(MidiInBase* base, songcore::SongcoreHost& host, const MidiInConsole& midiInConsole);

}  // namespace ptshell

#endif  // POCKETTRACKER_SHELL_MIDI_IN_CONSOLE_H
