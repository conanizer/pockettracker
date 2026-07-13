#pragma once

// ─── MIXER ───────────────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/MixerModule.kt. Eight stereo track meters, a master meter, the two send
// returns (REV / DEL), and the master strip: MIX volume, EQ slot, OTT/DUST depth, LIM pre-gain.
//
// ⚠️ THE SECOND STATEFUL MODULE IN THE UI, after the oscilloscope — and for the same reason: the
// peak-hold markers are a function of the PREVIOUS frame, not of the project. Its `draw` is therefore
// non-const, and `TrackerLayout` holds it by value like the scope.
//
// ⚠️ **The peak-hold advances on NEW DATA, not on a frame.** `PEAK_HOLD_FRAMES = 45` counts *draws*
// in Kotlin — where a draw is a Compose recomposition, and the only thing that triggers one on this
// screen is the peak poll's `stateVersion++` every 60 ms. So 45 "frames" is ~2.7 seconds of hang. The
// SDL shell redraws at 60 Hz, so counting its draws would expire the hold in 0.75 s and make the
// meters fall visibly faster than Android's. `peaksVersion` is what closes that gap: the feed bumps it
// once per 60 ms poll (ui/engine_feed.h), and the hold only steps when it moves. Same constants, same
// wall-clock behaviour, on both platforms — see the note on `advance` in the .cpp.
//
// The CURSOR here is (mixerMasterRow, cursorColumn) and it is not a grid — see ui/cursor_move.h:
//   row 0: cols 0..7 = track volumes, col 8 = master MIX
//   row 1: col 0 = REV wet, col 1 = DEL wet, col 8 = master EQ slot
//   row 2: col 8 = OTT/DUST depth   (which of the two is a function of masterBusFx)
//   row 3: col 8 = LIM pre-gain
// Rows 2 and 3 exist ONLY in column 8. Every other (row, column) pair is unreachable by navigation,
// and `cursor_context` answers `none()` there — which is what makes the unreachable states harmless.

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

struct MixerState {
    const songcore::Project& project;

    int cursorColumn   = 0;  // 0..7 = tracks, 8 = master
    int mixerMasterRow = 0;  // 0 = volumes, 1 = sends / EQ, 2 = OTT|DUST, 3 = LIM

    // The engine's meters, as ui/engine_feed.h last read them. Null-safe by construction: a zeroed
    // array is silence, which is exactly what `ptshot` draws with (it has no engine at all).
    const float* trackPeaks  = nullptr;  // 16 — L/R per track
    const float* masterPeaks = nullptr;  // 2
    const float* reverbPeaks = nullptr;  // 2
    const float* delayPeaks  = nullptr;  // 2

    /** Bumped once per peak poll (60 ms). The peak-hold steps when it moves — see the header note. */
    unsigned peaksVersion = 0;

    Theme theme = theme_classic();
};

struct MixerInputResult {
    bool modified = false;
};

class MixerModule {
public:
    static constexpr int WIDTH  = 620;
    static constexpr int HEIGHT = 392;

    /** ⚠️ NOT const: the peak-hold state is the module's own, and it moves as it draws. */
    void draw(Canvas& c, int x, int y, const MixerState& s);

    CursorContext cursor_context(const MixerState& s) const;

    /**
     * Apply a resolved action. Takes the PROJECT, not a cell: the mixer's cells live in six different
     * places on it (a track's volume, the master volume, two send wets, the master strip), and which
     * one an action lands on is the cursor's business, not the caller's.
     */
    MixerInputResult handle_input(songcore::Project& project, int cursor_row, int cursor_column,
                                  const InputAction& action) const;

private:
    // Peak-hold, in unscaled pixels. 0..15 = track L/R (i*2, i*2+1), 16..17 = master, 18..19 = reverb,
    // 20..21 = delay — the same index space Kotlin uses, because the meters are drawn in that order.
    static constexpr int PEAK_SLOTS = 22;
    float peakHoldPx_[PEAK_SLOTS]   = {};
    int   peakCounters_[PEAK_SLOTS] = {};

    /**
     * The peaksVersion the last draw saw. Starts at a value no version can be, so the FIRST draw always
     * advances the hold once — without which a single-shot renderer (ptshot) would show bars with no
     * peak markers above them.
     */
    unsigned lastPeaksVersion_ = static_cast<unsigned>(-1);

    void draw_stereo_meter(Canvas& c, int x, int y, int h, float level_l, float level_r,
                           bool is_selected, bool is_muted, const Theme& t, int peak_idx_l,
                           int peak_idx_r, bool advance);
    void draw_segmented_bar(Canvas& c, int x, int y, int h, int bar_h_px, const Theme& t) const;
    void draw_peak_marker(Canvas& c, int x, int y, int h, int peak_idx, const Theme& t) const;
    void update_peak(int idx, int level_px, bool is_muted);
};

}  // namespace pt::ui
