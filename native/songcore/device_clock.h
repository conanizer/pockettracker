#ifndef POCKETTRACKER_SONGCORE_DEVICE_CLOCK_H
#define POCKETTRACKER_SONGCORE_DEVICE_CLOCK_H

// Which frame the audio device is taking at a given moment. MIDI out sends a frame's message then,
// and MIDI in stamps an arrival with it, so both directions mean the same moment by the same frame.
// Each callback says its first frame was asked for when it began. A device taking sound at a steady
// pace would put all those moments on one straight line; callbacks run back to back to fill one
// portion come early, above it. So the line is drawn under them, through the lowest of the last
// second or two.
// ⚠️ Not the frame counter: that is the end of what has been rendered, up to a whole portion ahead.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace songcore {

// The steady clock in µs: what the clock below is told and asked in, and what MIDI in stamps with.
inline int64_t steady_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

class DeviceClock {
  public:
    // From the audio thread, at the top of each callback: its first frame, when it began (µs on the
    // steady clock), its frames and the rate.
    void on_callback(int64_t frame, int64_t us, int frames, int rate) {
        const int64_t base = frame - us * rate / 1000000;   // the line this start alone would give
        // A callback that does not carry on from the last (an export moved the counter, a new rate)
        // starts a new line.
        if (frame != end_ || rate != rate_) {
            rate_ = rate;
            low_ = lastLow_ = base;
            portion_ = lastPortion_ = frames;
            second_ = wake_ = frame;
        } else if (frame - second_ >= rate_) {
            // Each second starts over, so one late callback long ago does not hold the line down.
            lastLow_     = low_;
            lastPortion_ = portion_;
            low_         = base;
            portion_     = 0;
            second_      = frame;
        }
        low_ = std::min(low_, base);
        const int64_t line = std::min(low_, lastLow_);
        // The device wakes the app once per portion, and the callbacks that fill it run straight after
        // one another. So a callback that begins half a callback or more after the last one began is a
        // wake-up. ⚠️ Not "a start on the line": one late wake-up holds the line down for a second, every
        // wake-up after it looks early, and the portion read as seconds long.
        if ((us - lastUs_) * rate >= int64_t(frames) * 500000) {
            portion_ = std::max(portion_, frame - wake_);
            wake_    = frame;
        }
        lastUs_ = us;
        end_    = frame + frames;

        line_.store(line, std::memory_order_relaxed);
        rendered_.store(end_, std::memory_order_relaxed);
        block_.store(static_cast<int>(std::max(portion_, lastPortion_)), std::memory_order_relaxed);
        rateNow_.store(rate, std::memory_order_release);
    }

    // Any thread. The frame the device is taking at `us` — never past what it has been given, so a
    // stalled device holds MIDI back with it. False before the first callback.
    bool frame_at(int64_t us, int64_t& frame) const {
        const int rate = rateNow_.load(std::memory_order_acquire);
        if (rate <= 0) return false;
        frame = std::min(rendered_.load(std::memory_order_relaxed),
                         line_.load(std::memory_order_relaxed) + us * rate / 1000000);
        return true;
    }

    // The most frames the device took at once lately — its real portion, whatever the callback size.
    int block_frames() const { return block_.load(std::memory_order_relaxed); }

  private:
    // The audio thread's own.
    int     rate_ = 0;
    int64_t end_ = -1, low_ = 0, lastLow_ = 0, second_ = 0, wake_ = 0, portion_ = 0, lastPortion_ = 0;
    int64_t lastUs_ = 0;

    // What the other threads read.
    std::atomic<int64_t> line_{0}, rendered_{0};
    std::atomic<int>     block_{0}, rateNow_{0};
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_DEVICE_CLOCK_H
