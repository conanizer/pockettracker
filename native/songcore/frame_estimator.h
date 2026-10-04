#ifndef POCKETTRACKER_SONGCORE_FRAME_ESTIMATOR_H
#define POCKETTRACKER_SONGCORE_FRAME_ESTIMATOR_H

// ─── Where is the audio device RIGHT NOW? ────────────────────────────────────────────────────────
//
// `AudioEngine::getCurrentFrame()` is a staircase, updated once per audio block (~11.6 ms at
// 512/44.1 kHz). This turns it into a line for MIDI timing: the last observed step is an anchor, plus
// the wall time since. Wall microseconds arrive as an argument — songcore owns no clock, so tests
// can feed a synthetic one.
//
//   1. LEAD CAP — audio is the clock, not the wall. If the device stalls (backgrounding, an xrun
//      storm), MIDI freezes with it instead of running ahead for good. Queued note-offs wait too;
//      `SongcoreHost::stop()`'s panic is the backstop.
//   2. MONOTONIC between anchors: a LEN gate compares against this clock, and jitter going backwards
//      could close a gate twice.
//   3. A NEW ANCHOR resets the floor, even backwards: `resetFrameCounter()` zeroes the counter at each
//      offline render, and a floor kept across that would release every later message instantly.

#include <cstdint>

namespace songcore {

/** A smooth frame position from a block-quantised counter plus a wall clock. One owner thread. */
class FrameEstimator {
  public:
    /**
     * `maxLeadUs` is rule 1's cap. It must exceed one audio block, or the estimate freezes near the
     * end of every block; and stay small enough that a stall cannot drift MIDI from the audio.
     */
    explicit FrameEstimator(int sampleRate, int64_t maxLeadUs = 30000)
        : sampleRate_(sampleRate > 0 ? sampleRate : 44100), maxLeadUs_(maxLeadUs > 0 ? maxLeadUs : 1) {}

    /**
     * `engineFrame` is `getCurrentFrame()` now; `wallUs` a monotonic clock in µs.
     * ⚠️ The anchor time is when the step was NOTICED, so the estimate runs late by up to one call
     * interval — a constant bias (the MIDI screen's OFFSET absorbs it). Calling often keeps it small.
     */
    int64_t estimate(int64_t engineFrame, int64_t wallUs) {
        if (engineFrame != anchorFrame_ || !started_) {
            // Any counter change is a new anchor, forwards or backwards (rule 3).
            anchorFrame_ = engineFrame;
            anchorUs_    = wallUs;
            last_        = engineFrame;
            started_     = true;
            return engineFrame;
        }

        int64_t elapsedUs = wallUs - anchorUs_;
        if (elapsedUs < 0) elapsedUs = 0;                    // a non-monotonic wall clock
        if (elapsedUs > maxLeadUs_) elapsedUs = maxLeadUs_;   // rule 1

        int64_t est = anchorFrame_ + elapsedUs * sampleRate_ / 1000000;
        if (est < last_) est = last_;                         // rule 2
        last_ = est;
        return est;
    }

    /** The last value handed out — for diagnostics, never for a decision. */
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
