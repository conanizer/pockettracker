#ifndef POCKETTRACKER_SHELL_MIDI_SENDER_H
#define POCKETTRACKER_SHELL_MIDI_SENDER_H

// The MIDI sender thread. Messages are queued against a frame and released late-never-early by
// `ExternalConsumer::pump`; from the 60 Hz loop a note would leave up to 16.7 ms late. This pumps at
// ~1 kHz against the frame the audio device is taking (`AudioEngine::getDeviceFrame`).
// ⚠️ An SDL thread: on Android it makes JNI calls, and SDL's entry attaches it to the JVM.
// It may read the frame counter and call `pump`, nothing else. It may block in a port write, so HIGH
// priority, not TIME_CRITICAL (which would make the audio callback wait).

#include <atomic>
#include <cstdint>
#include <vector>

#include "songcore/midi_out.h"

struct SDL_Thread;
class AudioEngine;
namespace songcore { class SongcoreHost; }

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

// Built after the port is attached, before the loop starts. Ready line at start, tick counts at stop.
class MidiSender {
  public:
    MidiSender(AudioEngine& engine, songcore::SongcoreHost& host);
    ~MidiSender();

    MidiSender(const MidiSender&)            = delete;
    MidiSender& operator=(const MidiSender&) = delete;

    // False if the thread could not be created — `poll()` then keeps pumping.
    bool start();
    void stop();

    bool running() const { return thread_ != nullptr; }

  private:
    static int thread_entry(void* self);
    void       run();

    AudioEngine&            engine_;
    songcore::SongcoreHost& host_;

    SDL_Thread*        thread_ = nullptr;
    std::atomic<bool>  quit_{false};
    std::atomic<long long> ticks_{0};
    std::atomic<long long> busyTicks_{0};
    std::atomic<long long> maxBusyGap_{0};   // µs; the thread's own worst cadence
};

}  // namespace ptshell

#endif  // POCKETTRACKER_SHELL_MIDI_SENDER_H
