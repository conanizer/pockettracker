#ifndef POCKETTRACKER_SONGCORE_MIDI_WHEEL_H
#define POCKETTRACKER_SONGCORE_MIDI_WHEEL_H

// A track's pitch commands as the channel's pitch wheel: PSL / PBN slide, PVB / PVX vibrato, FIN and
// MPB, run as a sampler voice runs them (engine-mix.cpp `startNotePitchFx`) and scaled to the
// instrument's BEND. A closed form from `from`, so a value does not depend on how often it is asked.
// ⚠️ The wheel is per CHANNEL on the device: tracks sharing a channel share every bend.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "effects.h"       // fine_tune_semitones
#include "event.h"         // MIDI_BEND_CENTRE
#include "model.h"         // MIDI_BEND_RANGE_DEFAULT
#include "note_tables.h"   // f32_from_bits
#include "program.h"       // NoteOnPayload
#include "timing.h"

namespace songcore {

// A moving wheel is re-sent this often. ⚠️ A 5-pin cable carries ~1000 such messages a second in all.
constexpr int MIDI_WHEEL_MS = 5;

struct MidiWheel {
    uint8_t channel = 0;
    int     range   = MIDI_BEND_RANGE_DEFAULT;  // semitones at full throw
    int64_t from    = 0;
    float   offset  = 0.0f;                     // the slide, in semitones at `from`…
    float   target  = 0.0f, rate = 0.0f;        // …moving toward `target`, semitones a frame
    double  phase   = 0.0;                      // the vibrato: radians at `from`…
    float   speed   = 0.0f, depth = 0.0f;       // …Hz, semitones; depth 0 = none, phase held
    float   fine    = 0.0f;                     // FIN, semitones
    int     mpb     = 0;                        // MPB, wheel steps off centre; outlives the note
    int64_t next    = INT64_MAX;                // when it is next re-sent; INT64_MAX = still
};

inline float wheel_slide_at(const MidiWheel& w, int64_t frame) {
    if (w.rate == 0.0f) return w.offset;
    const double dt = static_cast<double>(std::max<int64_t>(0, frame - w.from));
    const double v  = w.offset + static_cast<double>(w.rate) * dt;
    return static_cast<float>(w.rate > 0.0f ? std::min(v, static_cast<double>(w.target))
                                            : std::max(v, static_cast<double>(w.target)));
}

inline double wheel_phase_at(const MidiWheel& w, int64_t frame, int sampleRate) {
    constexpr double TWO_PI = 6.283185307179586;
    if (w.depth <= 0.0f) return w.phase;
    const double dt = static_cast<double>(std::max<int64_t>(0, frame - w.from));
    return std::fmod(w.phase + TWO_PI * w.speed * dt / sampleRate, TWO_PI);
}

// The 14-bit wheel position at `frame`.
inline int wheel_value(const MidiWheel& w, int64_t frame, int sampleRate) {
    float semis = wheel_slide_at(w, frame) + w.fine;
    if (w.depth > 0.0f) semis += static_cast<float>(std::sin(wheel_phase_at(w, frame, sampleRate))) * w.depth;
    const long v = std::lround(MIDI_BEND_CENTRE + w.mpb + semis / static_cast<float>(w.range) * 8192.0f);
    return static_cast<int>(std::clamp(v, 0L, 16383L));
}

// The anchor moves to `frame`; the motion goes on from where it has got to.
inline void wheel_settle(MidiWheel& w, int64_t frame, int sampleRate) {
    if (frame <= w.from) return;
    w.offset = wheel_slide_at(w, frame);
    if (w.offset == w.target) w.rate = 0.0f;
    w.phase = wheel_phase_at(w, frame, sampleRate);
    w.from  = frame;
}

// Moving, unless the slide has landed or is pinned at the wheel's end, and there is no vibrato.
inline bool wheel_moves(const MidiWheel& w, int64_t frame, int sampleRate) {
    if (w.depth > 0.0f) return true;
    if (w.rate == 0.0f || wheel_slide_at(w, frame) == w.target) return false;
    const int v = wheel_value(w, frame, sampleRate);
    return w.rate > 0.0f ? v < 16383 : v > 0;
}

inline int64_t wheel_step(int sampleRate) {
    return std::max<int64_t>(1, static_cast<int64_t>(sampleRate) * MIDI_WHEEL_MS / 1000);
}

// The next re-send after one at `frame`. A slide's landing is a point of its own, so it is exact.
inline void wheel_plan_next(MidiWheel& w, int64_t frame, int sampleRate) {
    if (!wheel_moves(w, frame, sampleRate)) {
        w.next = INT64_MAX;
        return;
    }
    int64_t next = frame + wheel_step(sampleRate);
    if (w.rate != 0.0f) {
        const int64_t land = frame + static_cast<int64_t>(std::ceil((w.target - wheel_slide_at(w, frame)) / w.rate));
        if (land > frame && land < next) next = land;
    }
    w.next = next;
}

// A note starts everything over but MPB, as a voice does, from its PSL, PBN and vibrato. A LEG that
// took over a held key slides from `legatoFrom` semitones instead of any PSL, or drops straight to the
// new key.
inline void wheel_start_note(MidiWheel& w, int64_t frame, const NoteOnPayload& n, int tempo, int sampleRate,
                             bool legato = false, float legatoFrom = 0.0f) {
    wheel_settle(w, frame, sampleRate);
    const MidiWheel was = w;
    w = MidiWheel{};
    w.mpb  = was.mpb;
    w.from = frame;
    // The note LEG carries on keeps its FIN and its vibrato, unless this row brings one.
    if (legato) {
        w.fine  = was.fine;
        w.phase = was.phase;
        w.speed = was.speed;
        w.depth = was.depth;
    }

    const float tic    = frames_per_tic_f(tempo, sampleRate);
    const float pslOff = f32_from_bits(n.pslOffBits), pslDur = f32_from_bits(n.pslDurBits);
    if (std::fabs(pslOff) > 0.001f && pslDur > 0.0f) {
        w.offset = pslOff;
        w.rate   = -pslOff / std::max(1.0f, pslDur * tic);
    }
    if (legato) {
        w.offset = 0.0f;
        w.rate   = 0.0f;
        if (n.legatoTicks > 0 && std::fabs(legatoFrom) > 0.001f) {
            w.offset = legatoFrom;
            w.rate   = -legatoFrom / std::max(1.0f, n.legatoTicks * tic);
        }
    }
    // PBN, semitones a step, takes a PSL's slide over from its offset.
    const float pbn = f32_from_bits(n.pbnRateBits);
    if (pbn != 0.0f) {
        w.rate   = pbn / (tic * TICS_PER_STEP);
        w.target = pbn > 0.0f ? 127.0f : -127.0f;
    }
    const float depth = f32_from_bits(n.vibDepBits);
    if (depth > 0.01f) {
        w.speed = f32_from_bits(n.vibSpdBits);
        w.depth = depth;
    }
}

// PBN on an empty step: a new rate from where the slide has got to; 00 holds it there.
inline void wheel_set_rate(MidiWheel& w, float perStep, int tempo, int sampleRate) {
    if (perStep == 0.0f) {
        w.rate = 0.0f;
        return;
    }
    w.rate   = perStep / (frames_per_tic_f(tempo, sampleRate) * TICS_PER_STEP);
    w.target = perStep > 0.0f ? 127.0f : -127.0f;
}

// PVB / PVX on an empty step: the phase runs on; a depth under 0.01 stops the vibrato.
inline void wheel_set_vibrato(MidiWheel& w, float speed, float depth) {
    if (depth < 0.01f) {
        w.depth = 0.0f;
        return;
    }
    w.speed = speed;
    w.depth = depth;
}

// FIN (0..1 on the bus) retunes the sounding note; the next note clears it.
inline void wheel_set_fine(MidiWheel& w, float value01) {
    const int byte = std::clamp(static_cast<int>(value01 * 255.0f + 0.5f), 0, 255);
    w.fine = fine_tune_semitones(byte);
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MIDI_WHEEL_H
