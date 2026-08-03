#ifndef POCKETTRACKER_SONGCORE_FRAME_ESTIMATOR_H
#define POCKETTRACKER_SONGCORE_FRAME_ESTIMATOR_H

// ─── Where is the audio device RIGHT NOW? (MIDI plan phase B3) ────────────────────────────────────
//
// `AudioEngine::getCurrentFrame()` is a STAIRCASE. It is written once per audio block, at the end of
// `processAudioBlock` — 512 frames on the SDL backend, ~11.6 ms at 44.1 kHz — so reading it a thousand
// times a second gives a thousand readings with only ~86 distinct values. That is the whole reason a
// sender thread alone does not fix B3's problem: swapping a 60 Hz caller (16.7 ms of quantisation) for
// a 1 kHz one buys 16.7 → 11.6 ms, which is not the order of improvement phase C's clock needs.
//
// This turns the staircase back into a line: hold the last OBSERVED step as an anchor, and add the
// wall time elapsed since. `pump(now)` then gets a frame position with sub-millisecond resolution
// while still being anchored to the device rather than to the wall.
//
// ⚠️ **SONGCORE STILL OWNS NO CLOCK, and that is why this file has no `#include <chrono>`.** Wall
// microseconds arrive as an ARGUMENT, exactly as `ExternalConsumer::pump` takes its `now` — which is
// what makes both of them testable with a synthetic clock in `tools/ptmidi` and keeps the one real
// clock in the shell, where the platform lives.
//
// ── THE THREE RULES, each of which exists because leaving it out is a real failure ────────────────
//
//   1. **A LEAD CAP — audio is the clock, not the wall.** Without one, an audio device that stalls or
//      suspends (Android backgrounding, a CFW's power menu, an xrun storm) leaves the wall clock
//      running and the frame counter frozen, so MIDI would keep playing in real time against silent
//      audio and stay permanently ahead of it afterwards. Capped, MIDI freezes with the audio and
//      resumes in step. The cost is stated rather than hidden: during a stall, queued note-offs wait,
//      and `SongcoreHost::stop()`'s panic remains the backstop that ends them.
//   2. **MONOTONIC BETWEEN ANCHORS.** `pump` releases everything due at or before `now` and a frame
//      that went backwards would not un-send them — but a LEN gate compares against the same clock,
//      and a clock that stutters backwards under jitter can close a gate twice.
//   3. **A NEW ANCHOR RESETS THE FLOOR, INCLUDING BACKWARDS.** `AudioEngine::resetFrameCounter()`
//      puts the counter back to 0 at the start of every offline render. Rule 2 applied blindly across
//      that would pin the estimate at the pre-render value for the life of the process — the estimator
//      would be permanently past the end of time, and every message queued afterwards would release
//      instantly. (The cable is detached for a render, so this is not a live defect today. It is one
//      the moment anything else resets that counter, and the reset is not this file's to see.)

#include <cstdint>

namespace songcore {

/**
 * A smooth frame position from a block-quantised counter plus a wall clock.
 *
 * Not thread-safe and does not need to be: exactly one thread (the sender) owns one of these.
 */
class FrameEstimator {
  public:
    /**
     * `maxLeadUs` is rule 1's cap — how far past the last observed device position the estimate may
     * ever run. It must exceed the audio callback's own period (11.6 ms at 512/44.1 kHz) or the
     * estimate would freeze near the end of every normal block and hand back the staircase again; and
     * it must be small enough that a real stall cannot walk MIDI away from the audio. 30 ms is a
     * little over two SDL blocks, and about one Oboe burst pair.
     */
    explicit FrameEstimator(int sampleRate, int64_t maxLeadUs = 30000)
        : sampleRate_(sampleRate > 0 ? sampleRate : 44100), maxLeadUs_(maxLeadUs > 0 ? maxLeadUs : 1) {}

    /**
     * `engineFrame` — whatever `getCurrentFrame()` says now. `wallUs` — a monotonic wall clock in
     * microseconds. Returns the frame the device is estimated to be on.
     *
     * ⚠️ The anchor's wall time is the moment we NOTICED the step, not the moment it happened, so the
     * estimate is systematically LATE by up to one call interval. That is a bias, not jitter: it shifts
     * every message by the same amount and the MIDI screen's OFFSET row already exists to absorb it.
     * Calling this often is therefore not merely a precision knob — it is what keeps the bias small.
     */
    int64_t estimate(int64_t engineFrame, int64_t wallUs) {
        if (engineFrame != anchorFrame_ || !started_) {
            // Any change of the counter is a new anchor — forwards (the next block) or backwards (a
            // render reset). Rule 3: the monotonic floor moves with it, in both directions.
            anchorFrame_ = engineFrame;
            anchorUs_    = wallUs;
            last_        = engineFrame;
            started_     = true;
            return engineFrame;
        }

        int64_t elapsedUs = wallUs - anchorUs_;
        if (elapsedUs < 0) elapsedUs = 0;                    // a non-monotonic clock, defended against
        if (elapsedUs > maxLeadUs_) elapsedUs = maxLeadUs_;   // rule 1

        int64_t est = anchorFrame_ + elapsedUs * sampleRate_ / 1000000;
        if (est < last_) est = last_;                         // rule 2
        last_ = est;
        return est;
    }

    /** The last value handed out — for the instrument, never for a decision. */
    int64_t last() const { return last_; }
    int     sample_rate() const { return sampleRate_; }

  private:
    int     sampleRate_;
    int64_t maxLeadUs_;

    bool    started_     = false;
    int64_t anchorFrame_ = 0;
    int64_t anchorUs_    = 0;
    int64_t last_        = 0;
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_FRAME_ESTIMATOR_H
