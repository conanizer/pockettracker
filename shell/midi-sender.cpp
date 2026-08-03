#include "midi-sender.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "audio-engine.h"
#include "songcore/host.h"

namespace ptshell {

namespace {

// ── The two cadences, and why they are these numbers ─────────────────────────────────────────────
//
// BUSY: 1 ms while messages are queued. `pump` is late-never-early, so the tick interval IS the
// worst-case lateness this thread can add — and one millisecond is under a tenth of a phase-C clock
// tick (19.5 ms at 128 BPM, 24 PPQN), which is the tightest deadline the plan has.
//
// IDLE: 4 ms when the queue is empty. ⚠️ Not a micro-optimisation: this thread runs for the whole
// session, and on ANDROID it keeps running while the activity is paused (SDL blocks the frame loop on
// pause; it does not block this one). A 1 kHz wakeup that has nothing to do is battery burned in the
// background. An empty queue can still owe a LEN gate, so idling is 4 ms rather than "stop" — a
// note-off 4 ms late is not a thing anyone can hear, and a note-off that never comes is the one bug
// this whole file exists to avoid.
constexpr Uint32 BUSY_MS = 1;
constexpr Uint32 IDLE_MS = 4;

}  // namespace

int64_t monotonic_us() {
    const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64 c    = SDL_GetPerformanceCounter();
    if (freq == 0) return static_cast<int64_t>(SDL_GetTicks64()) * 1000;
    // ⚠️ Split, not `c * 1000000 / freq`. The performance counter is ticks since BOOT: at a 10 MHz QPC
    // frequency a machine up for a fortnight is already at ~1.2e13, and multiplying that by 1e6
    // overflows a 64-bit integer — silently, and into a NEGATIVE time that would make every interval
    // in the instrument below nonsense.
    return static_cast<int64_t>((c / freq) * 1000000ull + (c % freq) * 1000000ull / freq);
}

// ─── The instrument ──────────────────────────────────────────────────────────────────────────────

void MidiJitterRecorder::on_released(const songcore::MidiMessage& m, int64_t nowFrame, bool sent) {
    if (!sent) ++unsent_;
    if (m.frame == songcore::MidiMessage::UNSCHEDULED) {
        ++unscheduled_;
        return;
    }
    if (recs_.size() >= MAX_RECS) {
        ++overflow_;
        return;
    }
    recs_.push_back(Rec{m.frame, monotonic_us(), nowFrame, m.bytes[0], sent});
}

void MidiJitterRecorder::report(const char* label, int sampleRate, int tempo) const {
    std::printf("\n== MIDI send timing: %s ==\n", label);
    std::printf("   records %d (unscheduled/panic %d, no-port %d, dropped %d)\n",
                static_cast<int>(recs_.size()), unscheduled_, unsent_, overflow_);

    // ⚠️ ONE POINT PER DISTINCT DUE FRAME. A note-on arrives with its program change, its CC defaults
    // and its pan — four messages, one due frame, released inside one `pump` call — so counting each
    // would put four near-identical points on top of each other, inflate `n`, and hand the
    // consecutive-interval metric a pile of zero-length intervals to average the real errors away in.
    // The question being asked is "when did the app act on time T", and that has one answer per T.
    struct P { double f, w; };
    std::vector<P> pts;
    for (size_t i = 0; i < recs_.size(); ++i) {
        if (i > 0 && recs_[i].dueFrame == recs_[i - 1].dueFrame) continue;
        pts.push_back(P{static_cast<double>(recs_[i].dueFrame), static_cast<double>(recs_[i].wallUs)});
    }
    report_clock(sampleRate, tempo);

    if (pts.size() < 8) {
        // A maximum computed from two samples is not a measurement. Say so instead of printing 0.00.
        std::printf("   TOO FEW SCHEDULED MESSAGES TO JUDGE - %d distinct due frames, want 8+\n",
                    static_cast<int>(pts.size()));
        std::printf("   (nothing played? an EXTERNAL instrument and a transport start are both needed)\n");
        std::fflush(stdout);
        return;
    }

    // ⚠️ CENTRE BEFORE FITTING. Frames run to ~1e6 and the wall clock to ~1e10 microseconds; the
    // cross-product sums of the raw values reach ~1e19, where a double's last bit is worth about a
    // millisecond — the very quantity being measured. Centred on the first point, every term is small
    // and the residuals are exact to nanoseconds.
    const double f0 = pts.front().f, w0 = pts.front().w;
    double sf = 0, sw = 0, sff = 0, sfw = 0;
    const double n = static_cast<double>(pts.size());
    for (const P& p : pts) {
        const double f = p.f - f0, w = p.w - w0;
        sf += f; sw += w; sff += f * f; sfw += f * w;
    }
    const double denom = n * sff - sf * sf;
    if (!(std::fabs(denom) > 0.0)) {
        std::printf("   NO SPREAD IN DUE FRAMES - every message was due at the same instant\n");
        std::fflush(stdout);
        return;
    }
    const double slope     = (n * sfw - sf * sw) / denom;          // microseconds per frame
    const double intercept = (sw - slope * sf) / n;

    double              maxAbs = 0, sumAbs = 0;
    std::vector<double> resid(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        const double r = (pts[i].w - w0) - (intercept + slope * (pts[i].f - f0));
        resid[i] = r;
        sumAbs += std::fabs(r);
        maxAbs = std::max(maxAbs, std::fabs(r));
    }

    // The SECOND, independent reading: how wrong was each consecutive interval? Uses the nominal rate
    // rather than the fitted slope, so it cannot inherit an error from the fit.
    const double usPerFrame = 1000000.0 / (sampleRate > 0 ? sampleRate : 44100);
    double maxIntervalErr = 0;
    for (size_t i = 1; i < pts.size(); ++i) {
        const double wantUs = (pts[i].f - pts[i - 1].f) * usPerFrame;
        const double gotUs  = pts[i].w - pts[i - 1].w;
        maxIntervalErr = std::max(maxIntervalErr, std::fabs(gotUs - wantUs));
    }

    // The fitted rate against the device's: a sanity term on the FIT itself. Wildly off means the run
    // was not one continuous take (a stop and restart leaves a gap the line cannot follow) and the
    // residuals below are measuring that gap, not the sender.
    const double fittedRate = slope > 0 ? 1000000.0 / slope : 0.0;

    std::printf("   points  %d distinct due frames over %.2f s\n", static_cast<int>(pts.size()),
                (pts.back().w - pts.front().w) / 1e6);
    std::printf("   fit     %.3f us/frame -> %.0f Hz (device says %d Hz)\n", slope, fittedRate, sampleRate);
    std::printf("   LATENESS JITTER   mean %.3f ms   MAX %.3f ms      <- the B3 number\n",
                sumAbs / n / 1000.0, maxAbs / 1000.0);
    std::printf("   interval error    MAX %.3f ms                    <- independent check\n",
                maxIntervalErr / 1000.0);

    // ⚠️ **WHERE the worst residuals ARE, not just how big they are** — because one number cannot tell
    // a cadence problem from a single structural outlier, and this measurement has a known one. The
    // FIRST message of a take is queued with its due frame ALREADY IN THE PAST: the scheduler stamps it
    // from the block-quantised `getCurrentFrame()`, which trails the interpolated clock by up to one
    // audio block (11.6 ms), and `pump` is late-never-early — so it is released on the first tick after
    // it exists and reads as ~one block late however good the sender is. If the worst points are #0 and
    // the start of each later take, the sender is not what they are measuring; if they are scattered
    // through the run, it is.
    std::vector<size_t> order(pts.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&resid](size_t a, size_t b) { return std::fabs(resid[a]) > std::fabs(resid[b]); });
    std::printf("   worst points     ");
    for (size_t k = 0; k < 3 && k < order.size(); ++k) {
        const size_t i = order[k];
        std::printf(" #%d %+.3f ms", static_cast<int>(i), resid[i] / 1000.0);
    }
    std::printf("   (of %d)\n", static_cast<int>(pts.size()));
    std::fflush(stdout);
}

// ─── The clock stream, measured on its own (phase C) ─────────────────────────────────────────────
//
// ⚠️ **A SEPARATE BLOCK BECAUSE THE CLOCK IS A DIFFERENT CLAIM FROM THE NOTES, and the fit above
// cannot tell them apart.** With sync on, 0xF8 outnumbers every other message roughly fifty to one, so
// a combined residual is a clock measurement wearing a note measurement's label — and the one
// structural outlier the notes have (message #0 of a take, due before it was queued) is invisible
// inside it. Two streams, two verdicts.
//
// ⭐⭐ **AND THE BPM IS THE POINT OF THIS BLOCK, not the residual.** A least-squares fit reports how
// well the ticks sat on A line; it says nothing about whether that line is the RIGHT one. A clock with
// a period wrong by 5% would fit beautifully and drive a drum machine 5% fast. So this derives the
// tempo two ways — from the WALL CLOCK (`monotonic_us`, which knows nothing about frames, audio or
// `frames_per_step`) and from the DUE FRAMES — and prints both beside the project's own TEMPO. The
// wall figure is the anchor that lives outside every piece of arithmetic phase C added; agreement of
// all three is the check, and any two of them agreeing while the third does not names the culprit.
void MidiJitterRecorder::report_clock(int sampleRate, int tempo) const {
    std::vector<const Rec*> ticks;
    for (const Rec& r : recs_)
        if (r.status == 0xF8) ticks.push_back(&r);

    if (ticks.size() < 24) {
        std::printf("   clock   %d ticks - too few to judge (sync out off, or nothing played)\n",
                    static_cast<int>(ticks.size()));
        return;
    }

    const double sr        = sampleRate > 0 ? sampleRate : 44100;
    const double n         = static_cast<double>(ticks.size() - 1);
    const double wallSpan  = static_cast<double>(ticks.back()->wallUs   - ticks.front()->wallUs);
    const double frameSpan = static_cast<double>(ticks.back()->dueFrame - ticks.front()->dueFrame);

    // 24 ticks to the quarter, so a quarter is 24 tick intervals and BPM is 60 s divided by that.
    const double bpmWall  = wallSpan  > 0 ? 60e6 * n / (wallSpan * 24.0) : 0.0;
    const double bpmFrame = frameSpan > 0 ? 60.0 * sr * n / (frameSpan * 24.0) : 0.0;

    // Lateness of each tick against the grid the WALL says it should be on, anchored on the first tick.
    // Deliberately not a fitted line: a fit would absorb a systematically wrong period into its slope,
    // which is the one error this block exists to catch.
    const double usPerFrame = 1e6 / sr;
    double sumAbs = 0, maxAbs = 0, maxGap = 0, minGap = 1e18;
    for (size_t i = 1; i < ticks.size(); ++i) {
        const double gotUs  = static_cast<double>(ticks[i]->wallUs - ticks[i - 1]->wallUs);
        const double wantUs = static_cast<double>(ticks[i]->dueFrame - ticks[i - 1]->dueFrame) * usPerFrame;
        const double err    = gotUs - wantUs;
        sumAbs += std::fabs(err);
        maxAbs  = std::max(maxAbs, std::fabs(err));
        maxGap  = std::max(maxGap, gotUs);
        minGap  = std::min(minGap, gotUs);
    }

    std::printf("   CLOCK   %d ticks over %.2f s\n", static_cast<int>(ticks.size()), wallSpan / 1e6);
    std::printf("     tempo   %.2f BPM by WALL clock, %.2f BPM by due frame (project says %d)\n",
                bpmWall, bpmFrame, tempo);
    std::printf("     tick    mean err %.3f ms   MAX %.3f ms   gap %.2f..%.2f ms (nominal %.2f)\n",
                sumAbs / n / 1000.0, maxAbs / 1000.0, minGap / 1000.0, maxGap / 1000.0,
                frameSpan * usPerFrame / n / 1000.0);
}

// ─── The thread ──────────────────────────────────────────────────────────────────────────────────

MidiSender::MidiSender(AudioEngine& engine, songcore::SongcoreHost& host, int sampleRate)
    : engine_(engine), host_(host), clock_(sampleRate) {}

MidiSender::~MidiSender() { stop(); }

bool MidiSender::start() {
    if (thread_) return true;
    quit_.store(false);
    // ⚠️ SDL_CreateThread, not std::thread — on Android SDL's thread entry attaches the JVM, which the
    // MidiManager backend needs before it may make a single JNI call. See the header.
    thread_ = SDL_CreateThread(&MidiSender::thread_entry, "pt-midi-sender", this);
    if (!thread_) {
        std::printf("midi:    sender thread FAILED to start (%s) - falling back to the 60 Hz frame loop\n",
                    SDL_GetError());
        std::fflush(stdout);
        return false;
    }
    // The unconditional "I woke up" line. A working sender thread is invisible by design; without this
    // there is no way to tell it apart from one that was never created.
    std::printf("midi:    sender thread ready (%u ms busy / %u ms idle tick, %d Hz clock, %lld us lead cap)\n",
                BUSY_MS, IDLE_MS, clock_.sample_rate(), 30000LL);
    std::fflush(stdout);
    host_.set_midi_pump_external(true);   // and poll() stops pumping: one owner of the release
    return true;
}

void MidiSender::stop() {
    if (!thread_) return;
    quit_.store(true);
    SDL_WaitThread(thread_, nullptr);
    thread_ = nullptr;
    host_.set_midi_pump_external(false);
    // ⚠️ The NUMBERS beside the verdict: `ready` above only proves the thread was CREATED. A tick count
    // proves it ran, and the busy share proves it saw work — a sender that ticked 100k times with zero
    // busy ticks never released a message and would otherwise look identical to a working one.
    // ⚠️ "busy", not "with a queue" — since phase C a busy tick is one that owed a CLOCK or a message,
    // and a pure sync-out song is busy for its whole length with an empty queue at every instant.
    std::printf("midi:    sender thread stopped (%lld ticks, %lld busy, worst busy tick gap "
                "%.3f ms)\n",
                ticks_.load(), busyTicks_.load(), maxBusyGap_.load() / 1000.0);
    std::fflush(stdout);
}

int MidiSender::thread_entry(void* self) {
    static_cast<MidiSender*>(self)->run();
    return 0;
}

void MidiSender::run() {
    // HIGH, not TIME_CRITICAL: see the header. If the platform refuses, we simply run at normal
    // priority and the jitter instrument will say so.
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);

    songcore::ExternalConsumer& ext = host_.midi_out();
    int64_t prevBusyUs = 0;   // the last BUSY tick's timestamp — see maxBusyGap_ below
    while (!quit_.load()) {
        // The estimator is read ONCE per tick and both halves come from the same instant — reading the
        // wall clock after the frame counter would attribute the gap between them to elapsed time.
        const int64_t wallUs = monotonic_us();
        const int64_t frame  = engine_.getCurrentFrame();
        ext.pump(clock_.estimate(frame, wallUs));

        ticks_.fetch_add(1, std::memory_order_relaxed);
        // ⚠️ `needs_fast_pump`, NOT `pending_count() > 0` — phase C added a producer with no queue (the
        // clock generates its ticks inside `pump`), so the old test read "idle" through an entire song
        // of sync out. midi_out.h explains at length; the short version is that the predicate now comes
        // from what is OWED rather than from what happens to be in a vector.
        const bool busy = ext.needs_fast_pump();
        if (busy) {
            busyTicks_.fetch_add(1, std::memory_order_relaxed);
            // ⚠️ **THE THREAD'S OWN CADENCE, MEASURED — and it is what ATTRIBUTES the jitter.** `pump`
            // is late-never-early, so this thread cannot beat its own tick interval: if the jitter
            // instrument reports 6 ms of lateness and this says the ticks were 6 ms apart, the sleep is
            // the whole story (on Windows `Sleep(1)` honours the process timer resolution, which is
            // 15.6 ms unless something raises it). If the ticks were 1 ms apart and the lateness is
            // still 6 ms, the fault is somewhere else entirely and this is what says so. Only BUSY
            // ticks count — an idle tick is 4 ms by design and would mask the number being asked for.
            if (prevBusyUs != 0) {
                const int64_t gap = wallUs - prevBusyUs;
                if (gap > maxBusyGap_.load(std::memory_order_relaxed))
                    maxBusyGap_.store(gap, std::memory_order_relaxed);
            }
            prevBusyUs = wallUs;
        } else {
            prevBusyUs = 0;   // an idle stretch is not a gap in the busy cadence
        }
        SDL_Delay(busy ? BUSY_MS : IDLE_MS);
    }
}

}  // namespace ptshell
