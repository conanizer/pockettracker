#include "midi/midi-sender.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "audio-engine.h"
#include "songcore/host.h"

namespace ptshell {

namespace {

// 1 ms while anything is owed — `pump` is late-never-early, so the tick is the worst lateness added.
// 4 ms idle: the thread runs all session (on Android while paused too), and an empty queue can still
// owe a LEN gate.
constexpr Uint32 BUSY_MS = 1;
constexpr Uint32 IDLE_MS = 4;

}  // namespace

int64_t monotonic_us() {
    const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64 c    = SDL_GetPerformanceCounter();
    if (freq == 0) return static_cast<int64_t>(SDL_GetTicks64()) * 1000;
    // ⚠️ Split: counter × 1e6 overflows 64 bits after about a fortnight's uptime.
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

    // ⚠️ One point per distinct due frame: a note-on and its CCs share one, and stacking them would
    // drown the interval metric in zeros.
    struct P { double f, w; };
    std::vector<P> pts;
    for (size_t i = 0; i < recs_.size(); ++i) {
        if (i > 0 && recs_[i].dueFrame == recs_[i - 1].dueFrame) continue;
        pts.push_back(P{static_cast<double>(recs_[i].dueFrame), static_cast<double>(recs_[i].wallUs)});
    }
    report_clock(sampleRate, tempo);

    if (pts.size() < 8) {
        // A maximum from two samples is not a measurement.
        std::printf("   TOO FEW SCHEDULED MESSAGES TO JUDGE - %d distinct due frames, want 8+\n",
                    static_cast<int>(pts.size()));
        std::printf("   (nothing played? an EXTERNAL instrument and a transport start are both needed)\n");
        std::fflush(stdout);
        return;
    }

    // ⚠️ Centre before fitting: raw sums reach ~1e19, where a double's last bit is worth a millisecond.
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

    // An independent reading: each consecutive interval against the nominal rate, not the fit.
    const double usPerFrame = 1000000.0 / (sampleRate > 0 ? sampleRate : 44100);
    double maxIntervalErr = 0;
    for (size_t i = 1; i < pts.size(); ++i) {
        const double wantUs = (pts[i].f - pts[i - 1].f) * usPerFrame;
        const double gotUs  = pts[i].w - pts[i - 1].w;
        maxIntervalErr = std::max(maxIntervalErr, std::fabs(gotUs - wantUs));
    }

    // Far off the device's rate means the run was not one continuous take, and the residuals measure
    // the gap.
    const double fittedRate = slope > 0 ? 1000000.0 / slope : 0.0;

    std::printf("   points  %d distinct due frames over %.2f s\n", static_cast<int>(pts.size()),
                (pts.back().w - pts.front().w) / 1e6);
    std::printf("   fit     %.3f us/frame -> %.0f Hz (device says %d Hz)\n", slope, fittedRate, sampleRate);
    std::printf("   LATENESS JITTER   mean %.3f ms   MAX %.3f ms      <- the main number\n",
                sumAbs / n / 1000.0, maxAbs / 1000.0);
    std::printf("   interval error    MAX %.3f ms                    <- independent check\n",
                maxIntervalErr / 1000.0);

    // ⚠️ Where the worst residuals are: a take's first message is queued already past due, so it reads
    // ~one block late however good the sender — worst at #0 and take starts is that; scattered is the sender.
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
// Separate: the clock outnumbers the notes ~50:1. A fit says the ticks sat on a line, not the right
// one, so BPM is derived from the wall clock and from the due frames, beside the project's TEMPO —
// two agreeing while the third does not names the culprit.
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

    const double bpmWall  = wallSpan  > 0 ? 60e6 * n / (wallSpan * 24.0) : 0.0;
    const double bpmFrame = frameSpan > 0 ? 60.0 * sr * n / (frameSpan * 24.0) : 0.0;

    // Each tick against the wall grid from the first tick — not a fitted line, which would absorb a
    // wrong period into its slope.
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
    // ⚠️ SDL_CreateThread, not std::thread: on Android SDL's entry attaches the JVM for the JNI backend.
    thread_ = SDL_CreateThread(&MidiSender::thread_entry, "pt-midi-sender", this);
    if (!thread_) {
        std::printf("midi:    sender thread FAILED to start (%s) - falling back to the 60 Hz frame loop\n",
                    SDL_GetError());
        std::fflush(stdout);
        return false;
    }
    // Always printed: a working sender is otherwise invisible.
    std::printf("midi:    sender thread ready (%u ms busy / %u ms idle tick, %d Hz clock, %lld us lead cap)\n",
                BUSY_MS, IDLE_MS, clock_.sample_rate(), 30000LL);
    std::fflush(stdout);
    host_.set_midi_pump_external(true);   // one owner of the release
    return true;
}

void MidiSender::stop() {
    if (!thread_) return;
    quit_.store(true);
    SDL_WaitThread(thread_, nullptr);
    thread_ = nullptr;
    host_.set_midi_pump_external(false);
    // Created (ready line), ran (ticks), saw work (busy — owed a clock or a message).
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
    // HIGH, not TIME_CRITICAL (see the header); a refusal shows in the jitter report.
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);

    songcore::ExternalConsumer& ext = host_.midi_out();
    int64_t prevBusyUs = 0;
    while (!quit_.load()) {
        // Both halves from one instant, or the gap between the reads would count as elapsed time.
        const int64_t wallUs = monotonic_us();
        const int64_t frame  = engine_.getCurrentFrame();
        ext.pump(clock_.estimate(frame, wallUs));

        ticks_.fetch_add(1, std::memory_order_relaxed);
        // ⚠️ `needs_fast_pump`, not the queue size: the clock makes its ticks inside `pump` (midi_out.h).
        const bool busy = ext.needs_fast_pump();
        if (busy) {
            busyTicks_.fetch_add(1, std::memory_order_relaxed);
            // ⚠️ The thread's own busy cadence, to attribute jitter: lateness matching the gaps means
            // the sleep (Windows `Sleep(1)` follows the 15.6 ms timer unless raised).
            if (prevBusyUs != 0) {
                const int64_t gap = wallUs - prevBusyUs;
                if (gap > maxBusyGap_.load(std::memory_order_relaxed))
                    maxBusyGap_.store(gap, std::memory_order_relaxed);
            }
            prevBusyUs = wallUs;
        } else {
            prevBusyUs = 0;   // idle is not a gap in the busy cadence
        }
        SDL_Delay(busy ? BUSY_MS : IDLE_MS);
    }
}

}  // namespace ptshell
