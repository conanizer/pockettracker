#ifndef POCKETTRACKER_SONGCORE_MIDI_CLOCK_H
#define POCKETTRACKER_SONGCORE_MIDI_CLOCK_H

// SYNC OUT: the 24 PPQN clock, song position and start/stop/continue. Frames arrive as arguments.
// It follows TEMPO, not groove (one clock stream; groove is per track).
// ⚠️ A tick's frame is a RATIO from the epoch — `epoch + k * framesPerQuarter / PPQN` — never
// `next += period`: a tick is a fractional number of frames, and an integer period drifts.
// The epoch is the transport's start, so tick 6k is step k; a TEMPO change rebases it at the next tick.
// ⚠️ After an audio stall, a backlog past `MAX_BURST_TICKS` is skipped (and counted), not burst out.

#include <algorithm>
#include <cstdint>

#include "model.h"

namespace songcore {

// ─── The wire ───────────────────────────────────────────────────────────────────────────────────

// System real-time and common status bytes. SPP is legal only while stopped.
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

// The SPP for a start at song row `startRow`: the phrase steps before it, by the scheduler's row-length
// rule. Nominal — a groove or HOP makes rows differ, which only simulating the song could tell.
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

// The tick grid and its transport messages, under `ExternalConsumer`'s lock. Every byte goes to a
// caller's sink, so the port and OFFSET stay there.
class MidiClock {
  public:
    /** Ticks one `pump` may release before the backlog is skipped instead. One quarter: longer than
     *  any jitter, shorter than any real stall. */
    static constexpr int MAX_BURST_TICKS = MIDI_PPQN;

    // ── the switch ───────────────────────────────────────────────────────────────────────────────

    // True if the caller must now send a Stop (a device left running would run for ever).
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

    // Arm for a transport at `startFrame`: Start, or SPP then Continue (Start would rewind to 0). The
    // bytes go out on the first `pump`, at tick 0's frame.
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

    // The transport ended; true if a Stop is owed.
    bool stop() {
        pending_ = 0;
        if (!running_) return false;
        running_ = false;
        return true;
    }

    // ── the ticks ────────────────────────────────────────────────────────────────────────────────

    // Release the transport bytes and every tick due by `dueFrame`. A new `framesPerQuarter` rebases.
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

        // Counted in closed form, never by walking the grid (a caller may pump far ahead):
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

    // A new tempo from the next tick: the epoch moves there and the index restarts.
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
