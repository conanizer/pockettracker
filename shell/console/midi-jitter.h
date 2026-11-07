#ifndef POCKETTRACKER_SHELL_MIDI_JITTER_H
#define POCKETTRACKER_SHELL_MIDI_JITTER_H

// POCKETTRACKER_MIDI_JITTER=1: how late each MIDI message left against its due frame, at exit.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "songcore/midi_out.h"

namespace ptshell {

// The jitter instrument: stamps a wall clock beside each message's due frame, fits one against the
// other and reports the residuals (a drifting clock lands in the slope). Consecutive-interval error
// cross-checks the fit. Needs no port: a release is reported with or without a device.
class MidiJitterRecorder : public songcore::IMidiSendObserver {
  public:
    void on_released(const songcore::MidiMessage& m, int64_t nowFrame, bool sent) override;

    // The verdict with its numbers; `label` names the cadence. Below 8 distinct due frames it refuses
    // to judge rather than print 0.00.
    void report(const char* label, int sampleRate, int tempo) const;

    bool empty() const { return recs_.empty(); }

  private:
    // The 0xF8 stream on its own, with tempo from the wall clock: a residual cannot see a wrong period.
    void report_clock(int sampleRate, int tempo) const;

    struct Rec {
        int64_t dueFrame;
        int64_t wallUs;
        int64_t nowFrame;
        uint8_t status;
        bool    sent;
    };

    // Overflow is counted, never wrapped silently. ~7 minutes of dense playing.
    static constexpr size_t MAX_RECS = 20000;

    std::vector<Rec> recs_;
    int              overflow_    = 0;
    int              unscheduled_ = 0;   // panic messages: no due time, not in the fit
    int              unsent_      = 0;   // released with no port open
};

}  // namespace ptshell

#endif  // POCKETTRACKER_SHELL_MIDI_JITTER_H
