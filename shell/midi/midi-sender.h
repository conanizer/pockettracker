#ifndef POCKETTRACKER_SHELL_MIDI_SENDER_H
#define POCKETTRACKER_SHELL_MIDI_SENDER_H

// midi-sender.{h,cpp} — the JUST-IN-TIME MIDI sender thread.
//
// Every MIDI message is queued against a target FRAME and released by `ExternalConsumer::pump(now)`,
// late-never-early. Pumped from the 60 Hz frame loop a note would leave 0–16.7 ms late — most of a
// 24 PPQN clock tick. This thread pumps at ~1 kHz against an interpolated frame position
// (`songcore::FrameEstimator`).
//
// ⚠️ THE THREAD ALONE IS NOT ENOUGH: `getCurrentFrame()` moves once per audio block (~11.6 ms), so a
// 1 kHz reader of the raw counter would gain little. The estimator buys the precision; the thread is
// where it is read.
//
// ── ⚠️ ANDROID: AN SDL THREAD BECAUSE IT TALKS TO THE JVM ────────────────────────────────────────
//
// midi-out-android.cpp makes JNI calls, and a native thread must be attached to the JVM first.
// SDL's thread entry attaches it and registers the detach; a raw `std::thread` would abort the VM.
//
// ── WHAT THIS THREAD MAY AND MAY NOT DO ──────────────────────────────────────────────────────────
//
// It may read the engine's frame counter (atomic) and call `pump`; nothing else — not the project,
// the UI, the sequencer or the engine's queues. Not real-time in the audio sense (it may block in a
// port write), so HIGH priority rather than TIME_CRITICAL, which would make the audio callback wait.

#include <atomic>
#include <cstdint>
#include <vector>

#include "songcore/frame_estimator.h"
#include "songcore/midi_out.h"

struct SDL_Thread;
class AudioEngine;
namespace songcore { class SongcoreHost; }

namespace ptshell {

/**
 * The jitter instrument: how late did each message actually leave? Stamps a wall clock beside each
 * message's DUE FRAME, fits wall against frame, and reports the residuals in ms — the slope is
 * measured, so a drifting audio clock lands in the slope, not the verdict. A second, independent
 * reading (consecutive-interval error) cross-checks the fit.
 *
 * ⚠️ Needs no port: `ExternalConsumer::emit` reports a released message whether or not a device is
 * open, so a plain desktop run measures it.
 */
class MidiJitterRecorder : public songcore::IMidiSendObserver {
  public:
    void on_released(const songcore::MidiMessage& m, int64_t nowFrame, bool sent) override;

    /**
     * Print the verdict, with the numbers beside it. `label` names the cadence measured.
     * Refuses to judge below 8 distinct due frames: a run where nothing played would otherwise
     * report 0.00 ms. `tempo` anchors `report_clock`.
     */
    void report(const char* label, int sampleRate, int tempo) const;

    bool empty() const { return recs_.empty(); }

  private:
    /**
     * The 0xF8 clock stream on its own, with tempo derived from a WALL clock: sync out outnumbers
     * the notes fifty to one, and a residual cannot see a period that is simply wrong.
     */
    void report_clock(int sampleRate, int tempo) const;

    struct Rec {
        int64_t dueFrame;
        int64_t wallUs;
        int64_t nowFrame;
        uint8_t status;
        bool    sent;
    };

    // Fixed budget, and the overflow is COUNTED rather than silently wrapped: a report from a truncated
    // sample is fine, a report that does not say it was truncated is not. 20k records is ~7 minutes of
    // dense playing.
    static constexpr size_t MAX_RECS = 20000;

    std::vector<Rec> recs_;
    int              overflow_    = 0;
    int              unscheduled_ = 0;   // panic messages: never had a due time, excluded from the fit
    int              unsent_      = 0;   // released with no port open (the normal case for a measurement)
};

/**
 * The thread itself. Constructed after the port is attached and before the frame loop starts.
 * `start()` prints one unconditional ready line and `stop()` the tick and pump counts: a thread
 * that started and died immediately prints ready and then 0 ticks.
 */
class MidiSender {
  public:
    MidiSender(AudioEngine& engine, songcore::SongcoreHost& host, int sampleRate);
    ~MidiSender();

    MidiSender(const MidiSender&)            = delete;
    MidiSender& operator=(const MidiSender&) = delete;

    /** False if the thread could not be created — the caller then leaves `poll()` pumping. */
    bool start();
    void stop();

    bool running() const { return thread_ != nullptr; }

  private:
    static int thread_entry(void* self);
    void       run();

    AudioEngine&            engine_;
    songcore::SongcoreHost& host_;
    songcore::FrameEstimator clock_;

    SDL_Thread*        thread_ = nullptr;
    std::atomic<bool>  quit_{false};
    std::atomic<long long> ticks_{0};
    std::atomic<long long> busyTicks_{0};
    std::atomic<long long> maxBusyGap_{0};   // microseconds; the thread's own worst cadence
};

/** Wall clock in microseconds, monotonic, overflow-safe. Shared with the recorder. */
int64_t monotonic_us();

}  // namespace ptshell

#endif  // POCKETTRACKER_SHELL_MIDI_SENDER_H
