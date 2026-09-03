#include "load_progress.h"

#include <thread>

// A .cpp rather than a header inline, for `platform_memory.cpp`'s linkage reason: the engine is a
// SHARED library on Android and the shell is a second one, so the sink has to be one symbol resolved
// through the dynamic table. An `inline` variable would give the two libraries a copy each — the
// shell would install into its own and the decoders would read an empty one, silently.

namespace pt {
namespace {

LoadTick        g_tick;
std::thread::id g_tickThread{};   // whose loads the sink belongs to — see set_load_tick

// ⚠️ **PER-THREAD, because a SoundFont preset now loads on a WORKER while the screen keeps drawing.**
// A cancel, and the slice of the job the current ticks belong to, are properties of ONE load. Shared,
// the background load would inherit a cancel the user aimed at a foreground one, and its spans would
// walk the progress bar the foreground load is drawing. Thread-local, a worker's load simply has no
// progress and no cancel, which is what a load with no bar on screen should have.
thread_local bool  g_cancelled = false;
thread_local float g_lo        = 0.0f;
thread_local float g_hi        = 1.0f;

}  // namespace

void set_load_tick(LoadTick fn) {
    g_tick       = std::move(fn);
    g_tickThread = std::this_thread::get_id();
}

void begin_load() {
    g_cancelled = false;
    g_lo        = 0.0f;
    g_hi        = 1.0f;
}

void end_load() {
    g_lo = 0.0f;
    g_hi = 1.0f;
    // ⚠️ `g_cancelled` deliberately SURVIVES the close: the caller reads it *after* the load returns
    // to tell a cancel from an out-of-memory when it picks the message. `begin_load` is what clears
    // it, so the flag can never outlive into the next load.
}

bool load_tick(float fraction) {
    if (g_cancelled) return false;
    if (!g_tick) return true;
    // ⚠️ The sink draws, so only the thread that installed it may reach it. A background load reports
    // nothing and is never cancelled — "keep going" is the whole of its progress contract.
    if (std::this_thread::get_id() != g_tickThread) return true;

    // An inner "unknown" inside a narrowed span still places the job: the bar sits at the start of
    // this item's slice. It is only unknown overall when this item IS the whole job.
    const float span   = g_hi - g_lo;
    const float mapped = (fraction < 0.0f) ? (span < 1.0f ? g_lo : -1.0f)
                                           : g_lo + fraction * span;

    if (!g_tick(mapped)) {
        g_cancelled = true;
        return false;
    }
    return true;
}

bool load_cancelled() { return g_cancelled; }

LoadSpan::LoadSpan(float lo, float hi) : prevLo_(g_lo), prevHi_(g_hi) {
    // Nested spans compose: an inner slice is measured inside the outer one, not against the screen.
    const float outer = prevHi_ - prevLo_;
    g_lo = prevLo_ + lo * outer;
    g_hi = prevLo_ + hi * outer;
}

LoadSpan::~LoadSpan() {
    g_lo = prevLo_;
    g_hi = prevHi_;
}

}  // namespace pt
