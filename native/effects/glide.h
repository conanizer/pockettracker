#pragma once
#include <cmath>

// ⚠️ One rule for everything sound passes through — a note's settings and fades, its sends and pan, the
// mixer's faders, mutes and send returns. A setter only says where a level should go; the place that
// touches the sound gets there over a few milliseconds, and jumps only while no sound passes yet, so a
// note starts on its own settings. Anything new follows it, with a length and a form from this file.
constexpr int GLIDE_SAMPLES       = 128;  // a setting or a send on a sounding note, ~2.9 ms at 44.1 kHz
constexpr int DECLICK_SAMPLES     = 64;   // a note's own start, and a steal: the new note masks it
constexpr int KILL_FADE_SAMPLES   = 256;  // KIL, STOP, an audition let go of: at 64 these still tick
constexpr int MUTE_GATE_SAMPLES   = 256;  // MUTE/SOLO, both ways
constexpr int FADER_GLIDE_SAMPLES = 256;  // a fader's full swing, by hand, VTR or VMV; a short move is quicker
constexpr int PAN_GLIDE_SAMPLES   = 256;  // at 128 a low note's swing still clicks
constexpr int PAN_GLIDE_STEP      = 64;   // a pan glides in pieces this long: TSF pans once a render

// The voice's form: a number read once a sample (`next`) or once a piece (`skip`).
struct Ramp {
    float goal = 0.0f;   // where it is going

    // `sounding`: sound passes through it already, so it moves over `frames`; otherwise it lands at once.
    void set(float v, bool sounding, int frames = GLIDE_SAMPLES) {
        if (v == goal && left_ == 0) return;
        from_   = sounding ? value() : v;
        left_   = sounding ? frames : 0;
        frames_ = frames;
        goal    = v;
    }

    // From `from` to `to` over `frames`, wherever it was: a fade that starts at a known level.
    void start(float from, float to, int frames) {
        from_ = from;
        goal  = to;
        left_ = frames_ = frames;
    }

    // This sample's value, then one sample on.
    float next() {
        if (left_ == 0) return goal;
        const float v = value();
        --left_;
        return v;
    }

    // `n` samples on; the value reached.
    float skip(int n) {
        left_ = n >= left_ ? 0 : left_ - n;
        return value();
    }

    float value() const {
        if (left_ == 0) return goal;
        const float k = static_cast<float>(left_) / static_cast<float>(frames_);
        return from_ * k + goal * (1.0f - k);
    }

    bool moving() const { return left_ > 0; }

    // Worth mixing: above zero, or on its way down to it.
    bool audible() const { return goal > 0.0f || left_ > 0; }

  private:
    float from_   = 0.0f;
    int   left_   = 0;
    int   frames_ = GLIDE_SAMPLES;
};

// The mixer's form, for a level several readers share in one block — a fader, a mute, a send return's
// WET. It moves once a block, at most `step` (the block's share of a full swing), and every reader ramps
// across the block from where it was to where it is now.
inline float glide_toward(float from, float target, float step) {
    return from < target ? fminf(target, from + step) : fmaxf(target, from - step);
}
