#pragma once

#include <functional>

// ─── A LOAD REPORTING ITSELF, AND THE WAY A USER STOPS ONE ───────────────────────────────────────
//
// Opening a file can take longer than a frame when it means DECODING: an .sf3's Vorbis samples, or
// a compressed sample in proportion to its length. The frame loop stops for as long as it takes.
//
// ⭐ An ambient sink, not a parameter, because `tsf_load` is a vendored single-header library that
// takes no callback. The soundfont memory guard (soundfont-voice.cpp) has the same shape.
//
// ⚠️ NOTHING IS INSTALLED BY DEFAULT: `load_tick` is a null check returning "keep going" in tools,
// offline renders and boot. Only the shell installs a sink, while it has a window to draw into.
namespace pt {

/**
 * The report. `fraction` is 0..1 where a total is knowable and **< 0 where it is not** (an mp3 with
 * no Xing header states no length; nothing in an SF3 header states its decoded size until the
 * `shdr` count is reached). Return **false to CANCEL** — the load then unwinds through the failure
 * path it already has.
 */
using LoadTick = std::function<bool(float fraction)>;

/** Install / remove the sink. The shell owns this; nothing else may call it. */
void set_load_tick(LoadTick fn);

/**
 * Open a load. Clears the cancel flag and resets the span, so a cancelled load cannot poison the
 * next one. ⚠️ Every load must be opened and closed in pairs — `LoadSpan` below is what makes the
 * project loop's nesting exception-safe and typo-safe.
 */
void begin_load();
void end_load();

/**
 * Report progress from inside a load. **False means the user cancelled and the caller must stop.**
 *
 * ⚠️ It reuses the memory guard's unwind path (`appendBlock` false, `tsf_load` null, LOAD FAILED);
 * `load_cancelled()` tells a cancel from out-of-memory when the message is chosen.
 */
bool load_tick(float fraction);

/** True once a tick has been refused. Stays true until the next `begin_load()`. */
bool load_cancelled();

/**
 * The slice of the WHOLE job that the ticks until the next call belong to — how a project load of
 * twelve instruments turns twelve inner 0..1 reports into one bar that only moves forward.
 *
 * ⚠️ An inner report of "unknown" inside a narrowed span is NOT unknown overall: the bar sits at the
 * start of that item's slice, which is a true statement about the job even when the item cannot say
 * how far into itself it is.
 */
struct LoadSpan {
    LoadSpan(float lo, float hi);
    ~LoadSpan();

    LoadSpan(const LoadSpan&)            = delete;
    LoadSpan& operator=(const LoadSpan&) = delete;

  private:
    float prevLo_, prevHi_;
};

}  // namespace pt
