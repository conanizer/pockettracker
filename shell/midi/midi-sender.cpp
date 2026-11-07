#include "midi/midi-sender.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>

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

// ─── The thread ──────────────────────────────────────────────────────────────────────────────────

MidiSender::MidiSender(AudioEngine& engine, songcore::SongcoreHost& host) : engine_(engine), host_(host) {}

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
    std::printf("midi:    sender thread ready (%u ms busy / %u ms idle tick, on the device's frame)\n",
                BUSY_MS, IDLE_MS);
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
        // ⚠️ The steady clock, the engine's: SDL's performance counter is another clock on Linux.
        const int64_t wallUs = songcore::steady_us();
        ext.pump(engine_.getDeviceFrame(wallUs));

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
