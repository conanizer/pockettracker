#pragma once

#include <functional>

// ─── A LOAD REPORTING ITSELF, AND THE WAY A USER STOPS ONE ───────────────────────────────────────
//
// Opening a file is the one thing the app does that can take longer than a frame. Most of them do
// not: a 106 MB `.sf2` is read and converted in a quarter of a second, and every `.wav` is a read.
// What is slow is DECODING — a compressed `.sf3` unpacks Vorbis once per sample, and an mp3/flac/
// ogg/opus/m4a sample unpacks in proportion to its duration — and there the frame loop stops for as
// long as it takes.
//
// ⭐ **This is an ambient sink and not a parameter, and `tsf_load` is the reason.** The soundfont
// decode is inside a vendored single-header library that takes no callback and cannot be handed one;
// the only way a report gets out of it is a hook it can reach without being told about. The
// soundfont MEMORY GUARD (soundfont-voice.cpp) is the same shape for the same reason, and this sits
// beside it deliberately.
//
// ⚠️ **NOTHING IS INSTALLED BY DEFAULT.** Every `load_tick` on a tool, an offline render or a boot
// path is a null check that returns "keep going", so a load behaves exactly as it did before this
// existed. Only the shell installs a sink, and only while it has a window to draw into.
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
 * ⚠️ Callers already have an unwind path — the memory guard's — and this deliberately reuses it
 * rather than adding a second one: `appendBlock` returns false, `tsf_load` returns null, and the
 * LOAD FAILED at the end of each already exists and is already tested. `load_cancelled()` is what
 * separates "the user stopped it" from "the machine ran out" at the point the message is chosen.
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
