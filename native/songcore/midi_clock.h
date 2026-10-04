#ifndef POCKETTRACKER_SONGCORE_MIDI_CLOCK_H
#define POCKETTRACKER_SONGCORE_MIDI_CLOCK_H

// ─── SYNC OUT — the 24 PPQN clock and the transport ──────────────────────────────────────────────
//
// Start/stop/continue, song position and 24 clock ticks per quarter note, so external gear follows
// our tempo and transport. Owns no time source: frames arrive as arguments, so a test can drive it
// with a synthetic clock.
//
// The clock follows TEMPO, which is global. It cannot follow GROOVE: groove is per track
// (`TrackState`), and MIDI has one clock stream. A grooved track swings against a steady clock, as
// against a metronome.
//
// ⚠️ A tick's frame is a RATIO from a fixed epoch — `epoch + k * framesPerQuarter / PPQN` — never
// `next += period`. A tick is a fractional number of frames (861.17 at 128 BPM / 44.1 kHz); an
// integer period drifts ~11 ms a minute. The ratio is exact for every k. Overflow: an hour is ~184k
// ticks × ~20k frames, far below int64.
//
// The epoch is the transport's start frame, so tick 6k is step k — the scheduler's grid. A TEMPO
// change moves the epoch to the next tick's frame under the old tempo and restarts the index: no tick
// lost, doubled or moved backwards.
//
// ⚠️ Burst cap: after an audio stall `now` jumps, and "every tick due" would be hundreds of clock bytes
// — a tempo spike at the device. Past `MAX_BURST_TICKS` the backlog is skipped (the index still
// advances, so the grid stays locked) and counted in `dropped_ticks()`. (`FrameEstimator`'s lead cap
// guards the opposite case: the wall running ahead of stalled audio.)

#include <algorithm>
#include <cstdint>

#include "model.h"

namespace songcore {

// ─── The wire ───────────────────────────────────────────────────────────────────────────────────

/**
 * System real-time and system common status bytes. Real-time bytes are single, channel-less and
 * may be interleaved inside other messages. Song Position Pointer is three bytes and legal only while
 * stopped.
 */
constexpr uint8_t MIDI_SPP        = 0xF2,   // + LSB, MSB: 14-bit position in MIDI beats (16th notes)
                  MIDI_RT_CLOCK   = 0xF8,
                  MIDI_RT_START   = 0xFA,
                  MIDI_RT_CONTINUE = 0xFB,
                  MIDI_RT_STOP    = 0xFC;

/** Ticks per quarter note — the MIDI 1.0 constant. */
constexpr int MIDI_PPQN = 24;

/** SPP is 14 bits: 16 384 sixteenth notes ≈ 1024 bars. */
constexpr int MIDI_SPP_MAX = 16383;

// ─── Where in the song does a mid-song start LAND? ──────────────────────────────────────────────

/**
 * The SPP value for a start at song row `startRow`: the 16th notes (phrase steps) before it.
 * ⚠️ Uses the scheduler's own row-length rule (`maxChainLength` over `chain_is_empty`), so the
 * position agrees with where rows really end.
 * ⚠️ NOMINAL: groove or a HOP makes a row's real length differ from 16 steps per chain row, and the
 * truth would need the whole song simulated before the first byte. Exact in the ordinary case.
 */
inline int nominal_spp_beats(const Project& p, int startRow) {
    int64_t steps = 0;
    for (int row = 0; row < startRow; ++row) {
        int maxChainLength = 0;
        for (int t = 0; t < 8 && t < static_cast<int>(p.tracks.size()); ++t) {
            if (row >= static_cast<int>(p.tracks[t].chainRefs.size())) continue;
            const int chainId = p.tracks[t].chainRefs[row];
            if (chainId < 0 || chainId >= static_cast<int>(p.chains.size())) continue;
            const Chain& chain = p.chains[static_cast<size_t>(chainId)];
            int length = 0;
            for (int i = 0; i < 16; ++i)
                if (!chain_is_empty(chain, i)) ++length;
            maxChainLength = std::max(maxChainLength, length);
        }
        steps += static_cast<int64_t>(maxChainLength) * 16;
        if (steps >= MIDI_SPP_MAX) return MIDI_SPP_MAX;
    }
    return static_cast<int>(steps);
}

// ─── The generator ──────────────────────────────────────────────────────────────────────────────

/**
 * The tick grid and the transport messages that frame it.
 * Lives inside `ExternalConsumer` under its `mu_`; owns no lock of its own.
 * Every emission goes through a caller-supplied SINK (`sink(dueFrame, bytes, len)`), so the port,
 * queue and OFFSET stay in `ExternalConsumer` and a test can read the ticks without a port.
 */
class MidiClock {
  public:
    /** Ticks one `pump` may release before the backlog is skipped instead. One quarter: longer than
     *  any jitter, shorter than any real stall. */
    static constexpr int MAX_BURST_TICKS = MIDI_PPQN;

    // ── the switch ───────────────────────────────────────────────────────────────────────────────

    /**
     * Turn sync out on or off. Returns TRUE if the caller must now send a Stop — this object cannot
     * send bytes, and a device left running would run forever.
     * Off by default: ~51 messages a second, and a synth set to external sync sits silent without it.
     */
    bool set_enabled(bool e) {
        enabled_ = e;
        if (e || !running_) return false;
        running_ = false;
        pending_ = 0;
        return true;
    }
    bool enabled() const { return enabled_; }
    bool running() const { return running_; }

    // ── the transport ────────────────────────────────────────────────────────────────────────────

    /**
     * Arm the clock for a transport starting at `startFrame`.
     * `sppBeats == 0` sends **Start** ("rewind and play"); otherwise **SPP then Continue** — Start
     * after an SPP would rewind the device to 0.
     * Nothing is emitted here: the bytes share tick 0's due frame and go out on the first `pump`, so
     * OFFSET applies to them and Start always precedes the first clock.
     */
    void start(int64_t startFrame, int64_t framesPerQuarter, int sppBeats) {
        pending_ = 0;
        if (!enabled_) { running_ = false; return; }

        epochFrame_       = startFrame;
        framesPerQuarter_ = framesPerQuarter > 0 ? framesPerQuarter : 1;
        index_            = 0;
        dropped_          = 0;
        running_          = true;

        const int spp = std::max(0, std::min(sppBeats, MIDI_SPP_MAX));
        if (spp == 0) {
            push(MIDI_RT_START, 0, 0, 1);
        } else {
            push(MIDI_SPP, static_cast<uint8_t>(spp & 0x7F),
                 static_cast<uint8_t>((spp >> 7) & 0x7F), 3);
            push(MIDI_RT_CONTINUE, 0, 0, 1);
        }
    }

    /**
     * The transport ended. Returns TRUE if a Stop is owed, i.e. the clock was running. Derived here
     * because `ExternalConsumer::panic_locked` is reached from five sites, only some with a clock.
     */
    bool stop() {
        pending_ = 0;
        if (!running_) return false;
        running_ = false;
        return true;
    }

    // ── the ticks ────────────────────────────────────────────────────────────────────────────────

    /**
     * Release the transport bytes and every tick due at or before `dueFrame`.
     * `framesPerQuarter` comes on every call because TEMPO is live; a change rebases the grid.
     * `sink(int64_t dueFrame, const uint8_t* bytes, int len)`.
     */
    template <class Sink>
    void pump(int64_t dueFrame, int64_t framesPerQuarter, Sink&& sink) {
        if (!running_) return;

        // Transport bytes go FIRST, at tick 0's frame.
        if (pending_ > 0) {
            if (epochFrame_ > dueFrame) return;   // not yet; no tick can be due before tick 0
            for (int i = 0; i < pending_; ++i) sink(epochFrame_, transport_[i].bytes, transport_[i].len);
            pending_ = 0;
        }

        if (framesPerQuarter > 0 && framesPerQuarter != framesPerQuarter_) rebase(framesPerQuarter);

        // ⚠️ Counted in closed form, never by walking the grid: a caller may pump "past the end of
        // time" (the tests use INT64_MAX / 4), and counting first is what recognises a backlog.
        // `floor(k·fpq / PPQN) <= D` ⟺ `k <= (PPQN·(D+1) − 1) / fpq`. D is clamped against overflow.
        int64_t D = dueFrame - epochFrame_;
        if (D < 0) return;
        const int64_t MAX_D = INT64_MAX / MIDI_PPQN - 1;
        if (D > MAX_D) D = MAX_D;
        const int64_t lastDue = (MIDI_PPQN * (D + 1) - 1) / framesPerQuarter_;

        int64_t due = lastDue + 1 - index_;
        if (due <= 0) return;

        if (due > MAX_BURST_TICKS) {
            // The audio jumped. Skip to the last due tick; the index still counts every tick passed,
            // so the grid stays locked to the song.
            dropped_ += static_cast<int>(due - 1);
            index_ += due - 1;
            due = 1;
        }

        static const uint8_t CLOCK[1] = {MIDI_RT_CLOCK};
        for (int64_t i = 0; i < due; ++i) {
            sink(tick_frame(index_), CLOCK, 1);
            ++index_;
        }
    }

    // ── diagnostics ──────────────────────────────────────────────────────────────────────────────

    /** The position on the grid, counted from the start or the last tempo change. */
    int64_t tick_index() const { return index_; }
    /** Ticks skipped after audio stalls. */
    int     dropped_ticks() const { return dropped_; }
    /** The frame the next tick is due on — for tests, never for a decision. */
    int64_t next_tick_frame() const { return tick_frame(index_); }
    int64_t frames_per_quarter() const { return framesPerQuarter_; }

  private:
    struct Transport {
        uint8_t bytes[3];
        int     len;
    };

    void push(uint8_t a, uint8_t b, uint8_t c, int len) {
        if (pending_ >= 2) return;
        transport_[pending_].bytes[0] = a;
        transport_[pending_].bytes[1] = b;
        transport_[pending_].bytes[2] = c;
        transport_[pending_].len      = len;
        ++pending_;
    }

    int64_t tick_frame(int64_t k) const {
        return epochFrame_ + k * framesPerQuarter_ / MIDI_PPQN;
    }

    /**
     * A new tempo from the next tick: the epoch moves to where that tick would fall under the old
     * tempo and the index restarts. The scheduler reads `project.tempo` on every pass, so both
     * rebase on the same change.
     */
    void rebase(int64_t framesPerQuarter) {
        epochFrame_       = tick_frame(index_);
        index_            = 0;
        framesPerQuarter_ = framesPerQuarter;
    }

    bool    enabled_ = false;
    bool    running_ = false;

    int64_t epochFrame_       = 0;
    int64_t framesPerQuarter_ = 1;
    int64_t index_            = 0;
    int     dropped_          = 0;

    Transport transport_[2] = {};
    int       pending_      = 0;
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MIDI_CLOCK_H
