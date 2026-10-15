#ifndef POCKETTRACKER_SONGCORE_FRAME_ESTIMATOR_H
#define POCKETTRACKER_SONGCORE_FRAME_ESTIMATOR_H

// Where the audio device is RIGHT NOW, for MIDI timing: the engine's per-block frame staircase as a
// line — the last step seen, plus the wall time since (passed in).
//  1. A lead cap: if the device stalls, MIDI freezes with it rather than running ahead.
//  2. Monotonic between anchors, or a LEN gate could close twice.
//  3. A new anchor resets the floor, even backwards (a render zeroes the counter).

#include <cstdint>

namespace songcore {

/** A smooth frame position from a block-quantised counter plus a wall clock. One owner thread. */
class FrameEstimator {
  public:
    // `maxLeadUs` must exceed one audio block, or the estimate freezes near the end of every block.
    explicit FrameEstimator(int sampleRate, int64_t maxLeadUs = 30000)
        : sampleRate_(sampleRate > 0 ? sampleRate : 44100), maxLeadUs_(maxLeadUs > 0 ? maxLeadUs : 1) {}

    // `wallUs` is a monotonic clock in µs. The estimate runs late by up to one call interval (the step
    // is anchored when noticed); calling often keeps it small.
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
