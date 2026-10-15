#pragma once

#include <functional>

// A long load (decoding an .sf3 or a compressed sample) reports progress and can be cancelled.
// A global sink rather than a parameter, because the vendored `tsf_load` takes no callback.
// Only the shell installs one; elsewhere `load_tick` just says "keep going".
namespace pt {

// `fraction` is 0..1, or < 0 when the total is unknown. Return false to cancel; the load unwinds
// through its normal failure path.
using LoadTick = std::function<bool(float fraction)>;

// The shell's alone.
void set_load_tick(LoadTick fn);

// Opened and closed in pairs; opening clears the cancel flag and the span.
void begin_load();
void end_load();

// Progress from inside a load. False: the user cancelled, stop.
bool load_tick(float fraction);

// True once a tick was refused, until the next `begin_load()` — tells a cancel from out-of-memory.
bool load_cancelled();

// The slice of the whole job the next ticks belong to, so twelve instruments make one bar that
// only moves forward. Spans nest.
struct LoadSpan {
    LoadSpan(float lo, float hi);
    ~LoadSpan();

    LoadSpan(const LoadSpan&)            = delete;
    LoadSpan& operator=(const LoadSpan&) = delete;

  private:
    float prevLo_, prevHi_;
};

}  // namespace pt
