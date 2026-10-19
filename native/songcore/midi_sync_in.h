#ifndef POCKETTRACKER_SONGCORE_MIDI_SYNC_IN_H
#define POCKETTRACKER_SONGCORE_MIDI_SYNC_IN_H

// SYNC IN: an incoming 24 PPQN clock as a line — the frame each tick lands on — and Start, Continue,
// Stop and the song position as events. Each message comes with the frame it arrived on.
// ⚠️ Fed by the MIDI drain on the AUDIO thread: nothing here allocates or locks. The UI thread reads
// `read_line` and `pop_event`.
// The line is a least-squares fit over the last two beats of ticks. Every clock is one tick.

#include <atomic>
#include <cmath>
#include <cstdint>

#include "midi_clock.h"
#include "seqlock.h"

namespace songcore {

// The SYNC IN row: OFF; TEMPO — the clock sets the tempo and the step grid; FULL — and its Start,
// Continue, Stop and song position move the song.
enum MidiSyncInMode : int { SYNC_IN_OFF = 0, SYNC_IN_TEMPO = 1, SYNC_IN_FULL = 2 };

// The clock as the UI thread sees it: tick `tick` lands on `frame`, the next ones `period` apart.
struct MidiClockLine {
    static constexpr int MIN_TICKS = 3;   // fitted to fewer, and the period is a guess

    int64_t tick      = -1;      // −1: no clock heard since the port opened
    double  frame     = 0.0;
    double  period    = 0.0;     // frames per tick
    int64_t ticks     = 0;       // ticks since the clock (re)started
    int     fitted    = 0;       // of them, in the line
    int64_t lastFrame = 0;       // when the last tick arrived
    int64_t downbeat  = -1;      // the running transport's first tick; −1 = stopped

    bool   usable() const { return tick >= 0 && fitted >= MIN_TICKS && period > 0.0; }
    double frame_of(int64_t n) const { return frame + static_cast<double>(n - tick) * period; }
    double bpm(int sampleRate) const {
        return period > 0.0 ? 60.0 * sampleRate / (MIDI_PPQN * period) : 0.0;
    }
};

// A transport change, in the order it arrived.
struct MidiTransportEvent {
    enum Kind : uint8_t { START = 0, STOP = 1 };
    Kind    kind     = START;
    int64_t tick     = 0;   // START: the downbeat — the first clock after the Start or Continue
    int64_t frame    = 0;   // START: when the downbeat arrived
    int     position = 0;   // in 16ths from the song's top (the SPP unit); STOP: where it stopped
};

class MidiSyncIn {
  public:
    static constexpr int EVENT_CAPACITY = 16;   // a power of two
    // A gap of more periods than this is a clock that stopped: the next tick starts a new fit.
    static constexpr int STOPPED_TICKS = 8;
    // The same before a run has a period of its own: 8 ticks at 20 BPM, up to 96 kHz.
    static constexpr double STOPPED_FRAMES = 96000.0;
    // The fit's window: two beats — a second at 120 BPM to follow a tempo change, and quiet enough to
    // aim a step the walk's quarter of a second ahead.
    static constexpr int FIT_TICKS = 2 * MIDI_PPQN;

    // A real-time or song-position message that arrived on `frame`. False for any other.
    bool feed(uint8_t status, uint8_t d1, uint8_t d2, int64_t frame) {
        switch (status) {
            case MIDI_RT_CLOCK:    clock(frame); return true;
            // A Start while running starts again from the top; a Continue then is ignored.
            case MIDI_RT_START:    arm(0); return true;
            case MIDI_RT_CONTINUE: if (!running_) arm(spp_ >= 0 ? spp_ : stopPosition_); return true;
            case MIDI_RT_STOP:     stop(); return true;
            case MIDI_SPP:         position((static_cast<int>(d2) << 7) | static_cast<int>(d1)); return true;
            default:               return false;
        }
    }

    // A new port: nothing heard yet.
    void reset() {
        have_ = false;
        next_ = 0;
        fitN_ = fitHead_ = 0;
        ownPeriod_ = false;
        period_ = predict_ = 0.0;
        ticks_ = lastFrame_ = 0;
        running_ = armed_ = false;
        downbeat_ = -1;
        armedPosition_ = downbeatPosition_ = stopPosition_ = 0;
        spp_ = -1;
        publish();
    }

    // ── the UI thread ──

    // True when `out` now holds a newer line than `seen`.
    bool read_line(MidiClockLine& out, uint32_t& seen) const { return line_.read(out, seen); }

    bool pop_event(MidiTransportEvent& out) {
        const uint32_t tail = evTail_.load(std::memory_order_acquire);
        const uint32_t head = evHead_.load(std::memory_order_relaxed);
        if (head == tail) return false;
        out = events_[head & (EVENT_CAPACITY - 1)];
        evHead_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Ticks heard, ever, and events the UI ring had no room for.
    uint64_t clocks() const { return clocks_.load(std::memory_order_relaxed); }
    uint64_t events_dropped() const { return eventsDropped_.load(std::memory_order_relaxed); }

  private:
    void arm(int position) {
        armed_         = true;
        armedPosition_ = position;
        spp_           = -1;
    }

    // While running it is a jump: stop, and start there on the next tick.
    void position(int pos) {
        if (running_) {
            stop();
            arm(pos);
            return;
        }
        spp_ = pos;
    }

    void stop() {
        armed_ = false;
        if (!running_) return;
        running_  = false;
        downbeat_ = -1;
        // Back to the last 16th begun: six ticks each, from the downbeat.
        const int64_t sixteenths = (next_ - downbeatTick_) / 6;
        const int64_t at = downbeatPosition_ + sixteenths;
        stopPosition_ = static_cast<int>(at > MIDI_SPP_MAX ? MIDI_SPP_MAX : at);
        MidiTransportEvent ev;
        ev.kind     = MidiTransportEvent::STOP;
        ev.tick     = next_;
        ev.position = stopPosition_;
        push(ev);
        publish();
    }

    void clock(int64_t frame) {
        clocks_.fetch_add(1, std::memory_order_relaxed);
        const double f = static_cast<double>(frame);

        // A long gap is a clock that stopped: a new run, and a new fit. Until the run has a period of
        // its own, only a Start's downbeat or a long silence says so — an old, faster period would
        // end every run of a slower clock at its second tick.
        const double gap = f - static_cast<double>(lastFrame_);
        if (!have_ || (ownPeriod_ ? gap > STOPPED_TICKS * period_ : (armed_ || gap > STOPPED_FRAMES))) {
            have_ = true;
            fitN_ = fitHead_ = 0;
            ownPeriod_ = false;
            ticks_ = 0;
        }

        // ⚠️ Counted by message, not by time: a tick that came late would count as lost ones too, and
        // a period guessed from a bunched start then holds at a half or a quarter of the real one.
        const int64_t index = next_;
        if (ticks_ == 0) {
            // A run's first tick waits: a master that starts its clock with the transport may send it
            // late, and one of only three fitted would set a wrong tempo. It joins once the next two
            // put it on their line.
            heldIndex_ = index;
            heldFrame_ = f;
            predict_   = f + period_;
        } else {
            add_to_fit(index, f);
            refit(index, f);
            if (ticks_ == 2 && ownPeriod_) {
                const double onLine = predict_ + static_cast<double>(heldIndex_ - index - 1) * period_;
                if (std::fabs(heldFrame_ - onLine) < 0.25 * period_) {
                    add_to_fit(heldIndex_, heldFrame_);
                    refit(index, f);
                }
            }
        }
        next_      = index + 1;
        lastFrame_ = frame;
        ++ticks_;

        if (armed_) {
            armed_            = false;
            running_          = true;
            downbeat_         = index;
            downbeatTick_     = index;
            downbeatPosition_ = armedPosition_;
            MidiTransportEvent ev;
            ev.kind     = MidiTransportEvent::START;
            ev.tick     = index;
            ev.frame    = frame;
            ev.position = armedPosition_;
            push(ev);
        }
        publish();
    }

    void add_to_fit(int64_t index, double f) {
        fitIndex_[fitHead_] = index;
        fitFrame_[fitHead_] = f;
        fitHead_ = (fitHead_ + 1) % FIT_TICKS;
        if (fitN_ < FIT_TICKS) ++fitN_;
    }

    // Least squares over the window, measured from the newest tick so the sums stay small. With one
    // tick the last period stands as the guess.
    void refit(int64_t index, double f) {
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        for (int i = 0; i < fitN_; ++i) {
            const double x = static_cast<double>(fitIndex_[i] - index);
            const double y = fitFrame_[i] - f;
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const double n   = static_cast<double>(fitN_);
        const double den = n * sxx - sx * sx;
        const double slope = den > 0.0 ? (n * sxy - sx * sy) / den : 0.0;
        if (slope <= 0.0) {
            predict_ = f + period_;
            return;
        }
        period_    = slope;
        ownPeriod_ = true;
        predict_   = f + (sy - slope * sx) / n + slope;   // the line at the next tick
    }

    void publish() {
        MidiClockLine l;
        l.tick      = have_ ? next_ : -1;
        l.frame     = predict_;
        l.period    = period_;
        l.ticks     = ticks_;
        l.fitted    = fitN_;
        l.lastFrame = lastFrame_;
        l.downbeat  = downbeat_;
        line_.publish(l);
    }

    void push(const MidiTransportEvent& ev) {
        const uint32_t head = evHead_.load(std::memory_order_acquire);
        const uint32_t tail = evTail_.load(std::memory_order_relaxed);
        if (tail - head >= static_cast<uint32_t>(EVENT_CAPACITY)) {
            eventsDropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        events_[tail & (EVENT_CAPACITY - 1)] = ev;
        evTail_.store(tail + 1, std::memory_order_release);
    }

    // The clock — the drain's.
    bool    have_      = false;
    int64_t next_      = 0;      // the index the next tick gets
    int64_t fitIndex_[FIT_TICKS] = {};
    double  fitFrame_[FIT_TICKS] = {};
    int     fitN_      = 0;
    int     fitHead_   = 0;
    bool    ownPeriod_ = false;  // `period_` was fitted in this run
    int64_t heldIndex_ = 0;      // the run's first tick, until it joins the line or is dropped
    double  heldFrame_ = 0.0;
    double  period_    = 0.0;    // frames per tick
    double  predict_   = 0.0;    // the frame tick `next_` is due on
    int64_t ticks_     = 0;
    int64_t lastFrame_ = 0;

    // The transport — the drain's.
    bool    running_          = false;
    bool    armed_            = false;   // a Start or Continue waits for its first clock
    int     armedPosition_    = 0;
    int64_t downbeat_         = -1;
    int64_t downbeatTick_     = 0;
    int     downbeatPosition_ = 0;
    int     stopPosition_     = 0;
    int     spp_              = -1;      // a position sent while stopped, for the next Continue

    SeqPublisher<MidiClockLine> line_;

    MidiTransportEvent    events_[EVENT_CAPACITY];
    std::atomic<uint32_t> evHead_{0};   // the UI's
    std::atomic<uint32_t> evTail_{0};   // the drain's
    std::atomic<uint64_t> clocks_{0}, eventsDropped_{0};
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MIDI_SYNC_IN_H
