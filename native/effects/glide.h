#pragma once

// ⚠️ One rule for every setting of a sounding voice. Set before the note's first sample, a setting is
// where the note starts, or the attack plays with the old one. Set while the note sounds, it moves
// there over GLIDE_FRAMES, or the jump clicks. The filter, crush, drive, downsample, a table's VOL and
// the sends all follow it; a new setting must too.
constexpr int GLIDE_FRAMES = 128;   // ~2.9 ms at 44.1 kHz

// A level that follows the rule — the sends: `set` it from a setter, take `next` once a sample.
struct GlidedLevel {
    float goal = 0.0f;   // where it is going, 0-1

    // `sounding`: the note has played a sample.
    void set(float v, bool sounding) {
        if (v == goal && left_ == 0) return;
        from_ = sounding ? level() : v;
        left_ = sounding ? GLIDE_FRAMES : 0;
        goal  = v;
    }

    float next() {
        if (left_ == 0) return goal;
        const float v = level();
        --left_;
        return v;
    }

    // Worth mixing: above zero, or on its way down to it.
    bool audible() const { return goal > 0.0f || left_ > 0; }

  private:
    float level() const {
        const float k = static_cast<float>(left_) / GLIDE_FRAMES;
        return from_ * k + goal * (1.0f - k);
    }

    float from_ = 0.0f;
    int   left_ = 0;
};
