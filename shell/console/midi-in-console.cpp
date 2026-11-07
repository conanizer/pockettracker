#include "console/midi-in-console.h"

#include "midi/midi-in-base.h"
#include "songcore/host.h"

namespace ptshell {

void print_midi_in_report(MidiInBase* base, songcore::SongcoreHost& host, const MidiInConsole& midiInConsole) {
    const songcore::MidiInputRouter& r = host.midi_in_router();
    std::printf("midi in: %llu bytes at the port (%llu callbacks, %llu port errors), %llu drained, "
                "%llu messages\n"
                "         routed %llu, dropped: %llu non-channel, %llu no-instrument, "
                "%llu unsupported\n"
                "         queue overflow %llu bytes, parser orphans %llu, messages with no record %llu\n"
                "         dropped during an export %llu bytes, handled messages the UI never saw %llu\n"
                // What the engine and the cable were handed; differs from `routed` by the thru suppression.
                "         injected %llu into the engine; thru %s: %llu to the cable, %llu suppressed\n",
                static_cast<unsigned long long>(base->bytes_received()),
                static_cast<unsigned long long>(base->callbacks()),
                static_cast<unsigned long long>(base->port_errors()),
                static_cast<unsigned long long>(host.midi_in_bytes()),
                static_cast<unsigned long long>(host.midi_in_messages()),
                static_cast<unsigned long long>(r.routed()),
                static_cast<unsigned long long>(r.nonChannel()),
                static_cast<unsigned long long>(r.noInstrument()),
                static_cast<unsigned long long>(r.unsupported()),
                static_cast<unsigned long long>(host.midi_in_sink().dropped()),
                static_cast<unsigned long long>(host.midi_in_parser().orphan_bytes()),
                static_cast<unsigned long long>(midiInConsole.silent),
                static_cast<unsigned long long>(host.midi_in_pipeline().discarded()),
                static_cast<unsigned long long>(host.midi_in_pipeline().seen_dropped()),
                static_cast<unsigned long long>(host.midi_in_injected()),
                host.midi_in_thru() ? "on" : "OFF (loopback)",
                static_cast<unsigned long long>(host.midi_in_thru_sent()),
                static_cast<unsigned long long>(host.midi_in_thru_suppressed()));

    // SYNC IN's clock: the ticks never reach the router or the trace.
    std::printf("         clock: %llu ticks, %llu transport events lost\n",
                static_cast<unsigned long long>(host.midi_in_pipeline().sync().clocks()),
                static_cast<unsigned long long>(host.midi_in_pipeline().sync().events_dropped()));

    if (midiInConsole.ccTotal > 0) {
        const uint64_t spanMs = midiInConsole.ccLastMs - midiInConsole.ccFirstMs;
        std::printf("         CC: %llu in %.2f s = %.0f/s mean; PEAK %llu/s (busiest second), "
                    "%llu per 100 ms, %llu per 16 ms, %llu per drain; %llu drains carried one\n",
                    static_cast<unsigned long long>(midiInConsole.ccTotal),
                    spanMs / 1000.0,
                    spanMs > 0 ? midiInConsole.ccTotal * 1000.0 / spanMs : 0.0,
                    static_cast<unsigned long long>(midiInConsole.w1000.max),
                    static_cast<unsigned long long>(midiInConsole.w100.max),
                    static_cast<unsigned long long>(midiInConsole.w16.max),
                    static_cast<unsigned long long>(midiInConsole.ccMaxPerDrain),
                    static_cast<unsigned long long>(midiInConsole.ccDrains));
    }
}

}  // namespace ptshell
