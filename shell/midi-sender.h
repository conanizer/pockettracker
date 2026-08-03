#ifndef POCKETTRACKER_SHELL_MIDI_SENDER_H
#define POCKETTRACKER_SHELL_MIDI_SENDER_H

// midi-sender.{h,cpp} — the JUST-IN-TIME MIDI sender thread (MIDI plan phase B3).
//
// ── WHAT B3 IS FOR, in one paragraph ─────────────────────────────────────────────────────────────
//
// Every MIDI message is queued against a target FRAME and released by `ExternalConsumer::pump(now)`,
// late-never-early. Until B3 the only caller was `SongcoreHost::poll()`, on the 60 Hz frame loop — so
// every note left the app 0–16.7 ms after it was due, and *how much* depended on where in the frame
// interval it fell. That is inaudible on a note and disqualifying on a CLOCK: phase C's 24 PPQN sync
// is 19.5 ms per tick at 128 BPM, so 16 ms of quantisation is most of a tick. This thread makes the
// release cadence ~1 kHz instead, and interpolates the audio device's frame position between blocks
// (`songcore::FrameEstimator`) so the extra cadence has something precise to compare against.
//
// ⚠️ **THE THREAD ALONE WOULD NOT HAVE BEEN ENOUGH, and it is worth knowing why before touching this
// file.** `getCurrentFrame()` is written once per audio block — 512 frames, 11.6 ms — so a 1 kHz
// caller reading it raw would swap 16.7 ms of quantisation for 11.6 ms and call that a fix. The
// estimator is the half of B3 that actually buys the order of magnitude; the thread is what gives the
// estimator somewhere to be read from.
//
// ── ⚠️ ANDROID: THIS THREAD TALKS TO THE JVM, AND THAT IS WHY IT IS AN SDL THREAD ─────────────────
//
// `midi-out-android.cpp` reaches `MidiManager` through JNI, and a native thread must be ATTACHED to
// the JVM before it may make a JNI call at all. `SDL_CreateThread` is the one way to get that for
// free: SDL's thread entry calls `Android_JNI_SetupThread()`, which attaches and registers the
// detach-on-exit — a raw `std::thread` would work on Windows and Linux and then abort the VM on the
// device, which is the platform the feature actually ships on. (`midi-out-android.cpp`'s own THREADING
// note said "every method here runs on the SDL thread"; that was true when it was written and B3 is
// the layer that invalidated it. Its comment has been corrected, and `MidiOutManager.midiSend` is now
// `@Synchronized` so the Kotlin side's one reusable byte buffer is safe against two callers rather
// than against one convention.)
//
// ── WHAT THIS THREAD MAY AND MAY NOT DO ──────────────────────────────────────────────────────────
//
// It may: read the engine's frame counter (atomic), and call `pump`. That is all. It must NOT touch
// the project, the UI state, the sequencer or the engine's queues — `pump`'s reach is `midi_out.h`'s
// own state plus `IMidiOut::send`, and `ExternalConsumer`'s mutex is what makes even that safe.
//
// It is NOT a real-time thread in the audio sense: it does no DSP, it misses no deadline that matters
// if it is late by a millisecond, and it is allowed to block in a port write. `SDL_THREAD_PRIORITY_HIGH`
// rather than TIME_CRITICAL for exactly that reason — asking a handheld's scheduler for RT priority to
// deliver three bytes is a good way to make the audio callback wait behind it.

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
 * The B3 INSTRUMENT: how late did each message actually leave?
 *
 * ⚠️ **THIS EXISTS BECAUSE NOTHING ELSE IN THE TREE CAN ANSWER THAT QUESTION, and a B3 with no answer
 * is a B3 nobody can believe.** The byte stream is identical whether a note left 1 ms or 40 ms late;
 * ptmidi drives a synthetic clock and so measures the arithmetic rather than the delivery; the trace
 * prints bytes with no time. What this does: stamp a wall clock beside each message's DUE FRAME, then
 * fit wall against frame and report the RESIDUALS in milliseconds. The fit's slope is measured from the
 * data, not assumed, so a drifting audio clock lands in the slope and not in the verdict.
 *
 * ⚠️ It reports two numbers from the same data ON PURPOSE — the residual of a least-squares fit, and
 * the error of each consecutive INTERVAL. They are independent enough to disagree, and if they do, the
 * instrument is what is wrong (the guardrails: when two instruments disagree, go to the artifact).
 *
 * ⚠️ **It needs NO PORT, NO CABLE and NO SYNTH** — `ExternalConsumer::emit` tells the observer about a
 * released message whether or not a device was open. What B3 changes is WHEN the queue releases, which
 * is upstream of every platform backend, so the measurement is a plain desktop run with no hardware.
 */
class MidiJitterRecorder : public songcore::IMidiSendObserver {
  public:
    void on_released(const songcore::MidiMessage& m, int64_t nowFrame, bool sent) override;

    /**
     * Print the verdict, with the numbers beside it. `label` names the cadence being measured so two
     * runs can be compared by eye (the B3 control is the same binary with the thread turned off).
     *
     * Refuses to judge below 8 distinct due frames and says so: a maximum residual computed from two
     * samples is a lying instrument, and a run where nothing played would otherwise report 0.00 ms and
     * look like a triumph.
     *
     * `tempo` is the project's, and it is phase C's anchor rather than decoration — see `report_clock`.
     */
    void report(const char* label, int sampleRate, int tempo) const;

    bool empty() const { return recs_.empty(); }

  private:
    /**
     * Phase C: the 0xF8 stream on its own, with the tempo derived from a WALL clock as the anchor.
     * Split out because a combined fit is a clock measurement mislabelled — sync out puts fifty ticks
     * on the wire for every note — and because a residual cannot see a period that is simply wrong.
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
 *
 * `start()` prints one unconditional ready line and `stop()` prints the tick and pump counts — because
 * a component whose correct behaviour is SILENCE cannot be told from one that never ran, and this one's
 * correct behaviour is that nobody notices it. The counts are what distinguish "the thread was created"
 * from "the thread ran": a thread that started and died immediately prints ready and then 0 ticks.
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
