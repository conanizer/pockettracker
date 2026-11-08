#pragma once

// PROJECT > TEMPO's TAP: a tempo from the gaps between the last few taps.

#include <algorithm>

namespace pt::ui {

class TapTempo {
  public:
    // A tap at `now_ms`: the tempo in BPM (20..999), or 0 when there is nothing to average yet.
    int tap(long long now_ms) {
        // A gap this long is a pause, not a beat — start counting again from this tap.
        if (lastMs_ == 0 || now_ms - lastMs_ > TIMEOUT_MS) {
            lastMs_ = now_ms;
            count_  = 0;
            return 0;
        }

        const long long gap = now_ms - lastMs_;
        // A bounce: keep the anchor, so the next tap measures from the last real one.
        if (gap < MIN_MS) return 0;
        lastMs_ = now_ms;

        // Shift the ring, newest first; four entries is cheaper than a write cursor.
        for (int i = KEEP - 1; i > 0; --i) gaps_[i] = gaps_[i - 1];
        gaps_[0] = gap;
        if (count_ < KEEP) ++count_;

        long long sum = 0;
        for (int i = 0; i < count_; ++i) sum += gaps_[i];
        const long long meanMs = sum / count_;

        // Rounded, not truncated, so a 500 ms mean reads 120 and not 119.
        return std::clamp(static_cast<int>((60000 + meanMs / 2) / meanMs), 20, 999);
    }

  private:
    static constexpr int       KEEP       = 4;      // gaps averaged
    static constexpr long long TIMEOUT_MS = 3000;   // one beat at 20 BPM; longer starts anew
    static constexpr long long MIN_MS     = 60;     // shorter is a bouncing button

    long long lastMs_     = 0;     // 0 = no tap yet this count
    long long gaps_[KEEP] = {0};   // the most recent gaps, ms
    int       count_      = 0;     // how many are filled (capped at KEEP)
};

}  // namespace pt::ui
