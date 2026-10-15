#include "load_progress.h"

#include <thread>

// A .cpp: on Android the engine and shell are separate libraries, and an inline variable would
// give each its own sink.

namespace pt {
namespace {

LoadTick        g_tick;
std::thread::id g_tickThread{};   // whose loads the sink belongs to — see set_load_tick

// ⚠️ Per thread: a SoundFont preset loads on a worker, which must not inherit the foreground load's
// cancel or move its bar.
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
    // `g_cancelled` survives: the caller reads it after the load returns.
}

bool load_tick(float fraction) {
    if (g_cancelled) return false;
    if (!g_tick) return true;
    // The sink draws, so only its own thread reaches it; a background load is never cancelled.
    if (std::this_thread::get_id() != g_tickThread) return true;

    // An unknown inside a narrowed span sits at the start of that slice.
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

    const float outer = prevHi_ - prevLo_;
    g_lo = prevLo_ + lo * outer;
    g_hi = prevLo_ + hi * outer;
}

LoadSpan::~LoadSpan() {
    g_lo = prevLo_;
    g_hi = prevHi_;
}

}  // namespace pt
