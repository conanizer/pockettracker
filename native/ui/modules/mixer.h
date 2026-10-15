#pragma once

// The mixer: eight stereo track meters, the master, the two send returns, and the master strip (MIX,
// EQ, OTT/DUST, LIM). Stateful (peak hold), so `draw` is non-const.
// ⚠️ The hold advances per peak poll (60 ms), not per draw: the feed bumps `peaksVersion` for every
// slot that passed, even on another screen, and the hold catches up by as much.
// The cursor is (mixerMasterRow, cursorColumn), not a grid: `ui/mixer_cell_layout.h`.

#include "songcore/midi_map.h"
#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

struct MixerState {
    const songcore::Project& project;

    int cursorColumn   = 0;  // 0..7 = tracks, 8 = master
    int mixerMasterRow = 0;  // 0 = volumes, 1 = sends / EQ, 2 = OTT|DUST, 3 = LIM

    // The meters as the feed last read them; null is silence.
    const float* trackPeaks  = nullptr;  // 16 — L/R per track
    const float* masterPeaks = nullptr;  // 2
    const float* reverbPeaks = nullptr;  // 2
    const float* delayPeaks  = nullptr;  // 2

    // Bumped once per 60 ms peak poll.
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

    void draw(Canvas& c, int x, int y, const MixerState& s);

    // ⚠️ The idle gate must ask: markers fall inside `draw`, so they would hang once it stops.
    bool peaks_at_rest() const;

    CursorContext cursor_context(const MixerState& s) const;

    // For MIDI learn. The EQ slot declines.
    songcore::MapTarget map_target(const MixerState& s) const;

    // Takes the project: the mixer's cells live in six places on it.
    MixerInputResult handle_input(songcore::Project& project, int cursor_row, int cursor_column,
                                  const InputAction& action) const;

private:
    // Peak hold in pixels: 0..15 tracks L/R, 16..17 master, 18..19 reverb, 20..21 delay.
    static constexpr int PEAK_SLOTS = 22;
    float peakHoldPx_[PEAK_SLOTS]   = {};
    int   peakCounters_[PEAK_SLOTS] = {};

    // Starts impossible, so the first draw advances the hold once (a single-frame render shows markers).
    unsigned lastPeaksVersion_ = static_cast<unsigned>(-1);

    void draw_stereo_meter(Canvas& c, int x, int y, int h, float level_l, float level_r,
                           bool is_selected, bool is_muted, const Theme& t, int peak_idx_l,
                           int peak_idx_r, unsigned steps);
    void draw_segmented_bar(Canvas& c, int x, int y, int h, int bar_h_px, const Theme& t) const;
    void draw_peak_marker(Canvas& c, int x, int y, int h, int peak_idx, const Theme& t) const;
    void update_peak(int idx, int level_px, bool is_muted);
};

}  // namespace pt::ui
