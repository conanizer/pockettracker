#ifndef POCKETTRACKER_SONGCORE_MIDI_CLOCK_H
#define POCKETTRACKER_SONGCORE_MIDI_CLOCK_H

// ─── SYNC OUT — the 24 PPQN clock and the transport (MIDI plan phase C) ───────────────────────────
//
// What phase B put on the cable was NOTES. This is the other half of driving external gear: the
// TRANSPORT (start/stop/continue, and where in the song we are) and the TEMPO, as a stream of 24
// clock ticks per quarter note. A drum machine fed these plays at our tempo, starts when we start and
// stops when we stop, with no note ever sent to it.
//
// ⚠️ **PLATFORM-FREE AND CLOCK-FREE, like everything else in songcore.** This object owns no time
// source: frames arrive as arguments, exactly as `ExternalConsumer::pump` and `FrameEstimator::estimate`
// take theirs. That is what lets `tools/ptmidi` drive it with a synthetic clock and measure the
// arithmetic to the frame.
//
// ── ⭐ THE PLAN SAID "PHASE-LOCKED THROUGH TEMPO **AND GROOVE** CHANGES". THE GROOVE HALF IS NOT
//    IMPLEMENTABLE, AND THE REASON IS A FACT ABOUT THE SEQUENCER, NOT A SHORTCUT ────────────────────
//
// §10-C reads: *"24 PPQN clock (with TICS_PER_STEP = 12, quarter = 48 tics → one clock per 2 tics,
// phase-locked to the frame clock through tempo/groove changes)"*. Counting the clock in TICS is only
// meaningful if there is ONE tic grid to count on. There is not: **groove is per TRACK**, held in
// `TrackState.grooveId` / `TrackState.grooveStep` (scheduler.h), and eight tracks may be running eight
// different grooves at once, each stretching and shrinking its own steps. "The" groove does not exist,
// so a single global clock cannot follow it — and MIDI has exactly one clock stream.
//
// So the clock follows **TEMPO**, which IS global (`Project::tempo`, one value, live-editable, no
// per-step tempo FX exists anywhere in the tree — grep `setTempo`). A quarter note is
// `frames_per_step(tempo, sr) * 4` frames and gets 24 ticks, phase-locked to `playbackStartFrame_`.
// A grooved track swings against a steady clock, which is what a swung track does against a metronome
// and what every other tracker's sync out does. ⭐ **A mechanism in a ratified plan is a hypothesis**
// (the guardrails' own rule, and B3 hit the same one) — this is the third time that has cashed out.
//
// ── WHY THE TICK FRAME IS A RATIO AND NEVER AN ACCUMULATION ───────────────────────────────────────
//
// `tick_frame(k) = epoch + k * framesPerQuarter / PPQN`, in int64, from a fixed epoch — NOT
// `next += period`. The difference is not stylistic, and here is the measurement rather than the
// argument: at 128 BPM / 44.1 kHz a quarter is **20 668 frames** (four truncated `frames_per_step`s of
// 5167) and a tick is 861.17 of them. An integer `period` of 861 loses a sixth of a frame per tick —
// which ptmidi's control prints as **600 frames, 13.6 ms, over 70 seconds of playing**, or ~11.6 ms a
// minute: a drum machine visibly walking away from us over one song. The ratio form has no accumulator
// to drift and is exact for every k: tick 24k lands on quarter k to the frame, forever.
//
// Overflow is not a concern and here is the number: an hour of playing is ~184 000 ticks against a
// ~20 000-frame quarter, so the product peaks near 3.7e9 — nine orders below int64's roof.
//
// ── THE EPOCH, AND WHAT A TEMPO CHANGE DOES TO IT ────────────────────────────────────────────────
//
// The epoch is the transport's start frame, so tick 0 is the downbeat and tick 6k is step k — the same
// grid the scheduler puts notes on, by construction rather than by luck. When the user turns TEMPO
// while playing, `rebase` moves the epoch forward to the NEXT tick's frame under the OLD tempo and
// restarts the index at 0 under the new one. So the tick about to be emitted keeps its slot and every
// one after it follows the new tempo: continuous, never backwards, no tick lost or doubled.
//
// ── ⚠️ THE BURST CAP, which exists for a case the desktop never sees ──────────────────────────────
//
// `pump` emits every tick due at or before `now`. If the audio device stalls and resumes — an Android
// suspend, a CFW power menu — `now` jumps by the whole stall at once and the honest reading of "every
// tick due" is a burst of hundreds of clock bytes, which a device receives as a tempo spike. Past
// `MAX_BURST_TICKS` the backlog is SKIPPED rather than sent: the index advances (so the grid stays
// phase-locked to the song) and one tick goes out. `dropped_ticks()` counts them, because a component
// that silently swallows work cannot be told from one that had nothing to do.
//
// (`FrameEstimator`'s 30 ms lead cap bounds how far the WALL can run ahead of the audio; it does not
// bound how far the AUDIO can jump when it resumes. Two different stalls, two different guards.)

#include <algorithm>
#include <cstdint>

#include "model.h"

namespace songcore {

// ─── The wire ───────────────────────────────────────────────────────────────────────────────────

/**
 * System real-time and system common status bytes.
 *
 * The real-time four are ONE byte each and carry no channel — they are broadcast to every device on
 * the cable, and the MIDI spec allows them to be interleaved even between the bytes of another
 * message. Song Position Pointer is system COMMON, three bytes, and is legal only while stopped.
 */
constexpr uint8_t MIDI_SPP        = 0xF2,   // + LSB, MSB: 14-bit position in MIDI beats (16th notes)
                  MIDI_RT_CLOCK   = 0xF8,
                  MIDI_RT_START   = 0xFA,
                  MIDI_RT_CONTINUE = 0xFB,
                  MIDI_RT_STOP    = 0xFC;

/** Ticks per quarter note. The MIDI 1.0 constant — not a preference, not configurable. */
constexpr int MIDI_PPQN = 24;

/** SPP is 14 bits: 16 384 sixteenth notes ≈ 1024 bars, past which a position cannot be expressed. */
constexpr int MIDI_SPP_MAX = 16383;

// ─── Where in the song does a mid-song start LAND? ──────────────────────────────────────────────

/**
 * The SPP value for a start at song row `startRow` — the number of 16th notes before it.
 *
 * A MIDI beat is a 16th note, which is exactly one phrase STEP, so this counts steps: each song row
 * occupies (its longest track's chain length) chain rows, and each chain row is a 16-step phrase.
 * ⚠️ **The row-length rule is the SCHEDULER'S** — `updatePlaybackBuffer`'s `maxChainLength` loop, over
 * the same `chain_is_empty` predicate — because a position computed from a different rule than the one
 * that decides when rows actually end is a number that agrees with nothing.
 *
 * ⚠️ **IT IS NOMINAL, AND THAT IS A REAL LIMIT RATHER THAN AN OVERSIGHT.** A row's true length is
 * whatever `schedulePhrase` returns, and GROOVE (a step worth other than 12 tics) or a HOP that cuts a
 * phrase short makes it differ from 16 steps. Computing the truth would mean running the scheduler
 * over every preceding row — with per-track state, RNG and live edits — before a single byte could be
 * sent. The nominal count is exact for the ordinary case and wrong by the accumulated groove drift
 * otherwise; a drum machine that lands one 16th out on a swung song is a better failure than a
 * transport start that has to simulate the song first.
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
 *
 * Not thread-safe, and deliberately owns no lock: it lives inside `ExternalConsumer` and is reached
 * only under that class's `mu_`. One owner, one lock — the same rule the rest of midi_out.h follows.
 *
 * ⚠️ Every emission goes through a SINK the caller supplies (`sink(dueFrame, bytes, len)`) rather than
 * through an `IMidiOut` this object holds. That is what keeps the port, the queue, the send observer
 * and the OFFSET in `ExternalConsumer` — one place that knows how a message reaches the wire — and it
 * is what lets ptmidi read the tick stream without a port at all.
 */
class MidiClock {
  public:
    /**
     * How many ticks may be released in one `pump` before the backlog is skipped instead. One quarter
     * note: a real stall is always longer than this, and normal jitter is always shorter.
     */
    static constexpr int MAX_BURST_TICKS = MIDI_PPQN;

    // ── the switch ───────────────────────────────────────────────────────────────────────────────

    /**
     * Turn sync out on or off. Returns TRUE if the caller must now send a Stop — turning sync off
     * mid-take leaves a device running forever otherwise, and this object cannot send it a byte.
     *
     * Default OFF: clock is ~51 messages a second on a 31 250 baud wire, and a synth set to external
     * sync will sit silent waiting for it. A user who has not asked for sync should not get either.
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
     * Arm the clock for a transport that starts at `startFrame`.
     *
     * `sppBeats == 0` means the take begins at the song's own beginning, and the message for that is
     * **Start** — which every device understands as "rewind and play". Anything else is a mid-song
     * start, and the pair for that is **SPP then Continue**: SPP moves the device's playhead (it is
     * legal only while stopped, which is exactly where we are), Continue resumes from there. Sending
     * Start after an SPP would rewind the device to 0 and throw the position away.
     *
     * Nothing is emitted here. The transport bytes are held with the same due frame as tick 0 and go
     * out on the first `pump` that reaches it, so the OFFSET row applies to them exactly as it applies
     * to a note, and so Start can never arrive after the first clock it is meant to precede.
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
     * The transport ended. Returns TRUE if a Stop is owed — i.e. if this clock was actually running.
     *
     * ⚠️ **THE RETURN VALUE IS THE POINT.** `ExternalConsumer::panic_locked` is reached from a
     * transport stop, a port swap, a render detach, a new take and the destructor, and only some of
     * those had a running clock. Deriving "does a Stop go out?" from THIS object's own state is the
     * guardrails' rule applied — if it rested on each of those five sites remembering, it would already
     * be broken.
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
     *
     * `framesPerQuarter` is passed on EVERY call rather than remembered, because TEMPO is live: the
     * user can turn it on the PROJECT screen mid-take. A change rebases the grid (see the header) and
     * the caller never has to notice it happened.
     *
     * `sink(int64_t dueFrame, const uint8_t* bytes, int len)`.
     */
    template <class Sink>
    void pump(int64_t dueFrame, int64_t framesPerQuarter, Sink&& sink) {
        if (!running_) return;

        // The transport bytes share tick 0's due frame, and they go FIRST — a Start that arrived after
        // the clock it starts is a device that ignores one of them.
        if (pending_ > 0) {
            if (epochFrame_ > dueFrame) return;   // not yet — and no tick can be due before tick 0 either
            for (int i = 0; i < pending_; ++i) sink(epochFrame_, transport_[i].bytes, transport_[i].len);
            pending_ = 0;
        }

        if (framesPerQuarter > 0 && framesPerQuarter != framesPerQuarter_) rebase(framesPerQuarter);

        // How many are actually due? ⚠️ **COUNTED IN CLOSED FORM AND NOT BY WALKING THE GRID.** A caller
        // is entitled to pump past the end of time (`ptmidi` releases its queue with `INT64_MAX / 4`,
        // which is what "nothing may stay queued" means when there is no clock), and a loop that steps
        // one tick at a time would sit there for the age of the universe. Counting first is also what
        // lets a stall's backlog be RECOGNISED as a backlog rather than discovered halfway through
        // sending it.
        //
        // `floor(k·fpq / PPQN) <= D` ⟺ `k <= (PPQN·(D+1) − 1) / fpq`, all integer. D is clamped so the
        // multiply cannot overflow; the clamp is far past any real audio stall and only bites on a
        // synthetic "past the end of time" pump, where the burst cap below handles it anyway.
        int64_t D = dueFrame - epochFrame_;
        if (D < 0) return;
        const int64_t MAX_D = INT64_MAX / MIDI_PPQN - 1;
        if (D > MAX_D) D = MAX_D;
        const int64_t lastDue = (MIDI_PPQN * (D + 1) - 1) / framesPerQuarter_;

        int64_t due = lastDue + 1 - index_;
        if (due <= 0) return;

        if (due > MAX_BURST_TICKS) {
            // The audio device jumped. Skip to the last due tick — the index still counts every tick
            // the song has passed, so the grid stays locked to the song rather than to the resume.
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

    // ── the instrument's window ──────────────────────────────────────────────────────────────────

    /** Ticks emitted since the transport started. `index_` is also the position on the grid. */
    int64_t tick_index() const { return index_; }
    /** Ticks a stalled-and-resumed audio device cost us. Never silently zero — see the header. */
    int     dropped_ticks() const { return dropped_; }
    /** The frame the next tick is due on — for a test, never for a decision. */
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
     * A new tempo, taking effect from the next tick.
     *
     * The epoch moves to where the NEXT tick would have fallen under the old tempo and the index goes
     * back to 0 — so that tick keeps its slot (no jump, no tick backwards, none lost) and every one
     * after it is spaced by the new period. Phase-lock to the step grid survives at the new tempo
     * because the scheduler rebases on the same event: it reads `project.tempo` afresh on every
     * scheduling pass.
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
