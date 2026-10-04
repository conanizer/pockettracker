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

// BUSY: 1 ms while anything is owed — `pump` is late-never-early, so the tick IS the worst lateness
// added, under a tenth of a 24 PPQN clock tick.
// IDLE: 4 ms when nothing is queued. This thread runs all session (on Android even while the
// activity is paused), so a 1 kHz idle wakeup is battery; 4 ms rather than stopping, because an
// empty queue can still owe a LEN gate.
constexpr Uint32 BUSY_MS = 1;
constexpr Uint32 IDLE_MS = 4;

}  // namespace

int64_t monotonic_us() {
    const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64 c    = SDL_GetPerformanceCounter();
    if (freq == 0) return static_cast<int64_t>(SDL_GetTicks64()) * 1000;
    // ⚠️ Split, not `c * 1000000 / freq`: the counter is ticks since boot, and multiplying by 1e6
    // overflows 64 bits after about a fortnight's uptime.
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

    // ⚠️ ONE POINT PER DISTINCT DUE FRAME: a note-on and its program change, CCs and pan share one
    // due frame, and counting each would stack points and drown the interval metric in zeros.
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

    // ⚠️ CENTRE BEFORE FITTING: raw cross-product sums reach ~1e19, where a double's last bit is
    // worth a millisecond — the very quantity measured.
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

    // ⚠️ WHERE the worst residuals are, not just how big: a take's FIRST message is queued with its
    // due frame already past (stamped from the block-quantised counter), so it reads ~one block late
    // however good the sender is. Worst points at #0 and each take's start are that; scattered ones
    // are the sender.
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

// ─── The clock stream, measured on its own ───────────────────────────────────────────────────────
//
// ⚠️ A SEPARATE BLOCK: the clock outnumbers the notes ~50:1, so a combined residual would be a clock
// measurement under a note label. ⭐⭐ The BPM is the point: a fit says the ticks sat on A line, not
// the RIGHT one. So tempo is derived from the WALL clock and from the DUE FRAMES and printed beside
// the project's TEMPO — any two agreeing while the third does not names the culprit.
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
    // ⚠️ The numbers beside the verdict: `ready` proves the thread was CREATED, the tick count that
    // it RAN, the busy share that it saw work. "Busy" means it owed a clock or a message — a sync-out
    // song is busy throughout with an empty queue.
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
        // ⚠️ `needs_fast_pump`, NOT `pending_count() > 0`: the clock generates its ticks inside
        // `pump` with no queue, so the predicate comes from what is OWED (midi_out.h).
        const bool busy = ext.needs_fast_pump();
        if (busy) {
            busyTicks_.fetch_add(1, std::memory_order_relaxed);
            // ⚠️ THE THREAD'S OWN CADENCE, MEASURED — it attributes the jitter: if lateness matches
            // the tick gaps, the sleep is the whole story (Windows `Sleep(1)` follows the timer
            // resolution, 15.6 ms unless raised); if the ticks were 1 ms apart, the fault is elsewhere.
            // BUSY ticks only.
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
