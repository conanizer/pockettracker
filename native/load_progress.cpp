#include "load_progress.h"

// A .cpp rather than a header inline, for `platform_memory.cpp`'s linkage reason: the engine is a
// SHARED library on Android and the shell is a second one, so the sink has to be one symbol resolved
// through the dynamic table. An `inline` variable would give the two libraries a copy each — the
// shell would install into its own and the decoders would read an empty one, silently.

namespace pt {
namespace {

LoadTick g_tick;
bool     g_cancelled = false;

// The slice of the whole job the current ticks belong to. 0..1 = "this load is the whole job".
float g_lo = 0.0f;
float g_hi = 1.0f;

}  // namespace

void set_load_tick(LoadTick fn) { g_tick = std::move(fn); }

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
