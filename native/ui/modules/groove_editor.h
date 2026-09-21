#pragma once

// ─── GROOVE EDITOR ───────────────────────────────────────────────────────────────────────────────
//
// A 16-step pattern of tick counts: how long each phrase step lasts, which is how swing and shuffle
// are written.
//
//   −1    end of pattern ("--") — the groove loops back to step 0 from here
//   0x00  skip: the phrase row is not triggered and takes no time
//   0x01+ that step lasts this many ticks (0x0C = 12 = the default, one step = one beat/4)
//
// The tick rows own the left of the screen. A PANEL sits to their right, on the first four data rows:
// the preset name, SAVE/LOAD, the quantize pointer, and the swing percentage. It is a table the
// cursor walks — see `groove_panel_cell_count`.
//
// ⚠️ TWO ROW CURSORS, AND THE SECOND ONE IS NOT REDUNDANT. `cursorRow` is the tick row and
// `panelRow` is the panel row, both live at once. The swing percentage reports on the group the TICK
// cursor is in, so walking into the panel to change QNT must not move the number QNT governs.
//
// The only grid editor with NO playback marker and NO selection — so it has no row background at all,
// and no `row_bg_color` priority ladder to run. Its rows are laid out from a `dataStartY` that
// already includes TEXT_PADDING (the others add it per row), which is why the y passed to `draw_cell`
// needs no adjusting here.

#include <string>

#include "songcore/model.h"
#include "songcore/timing.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

/** The two cursor columns: the tick grid, and the panel beside it. Column 0 is the row number. */
inline constexpr int GROOVE_COL_TICK  = 1;
inline constexpr int GROOVE_COL_PANEL = 2;

/** The panel's rows. */
inline constexpr int GROOVE_PANEL_NAME = 0;
inline constexpr int GROOVE_PANEL_FILE = 1;  // SAVE · LOAD
inline constexpr int GROOVE_PANEL_QNT  = 2;
inline constexpr int GROOVE_PANEL_SWG  = 3;
inline constexpr int GROOVE_PANEL_ROWS = 4;

/** How many cells a panel row holds. The FILE row is the only one with two. */
inline int groove_panel_cell_count(int panel_row) {
    return panel_row == GROOVE_PANEL_FILE ? 2 : 1;
}

/** What a bare A does in the panel. The dispatcher's arm, so the geometry stays in one file. */
enum class GrooveFileAction { NONE, SAVE, LOAD };

inline GrooveFileAction groove_file_action(int panel_row, int panel_column) {
    if (panel_row != GROOVE_PANEL_FILE) return GrooveFileAction::NONE;
    return panel_column == 0 ? GrooveFileAction::SAVE : GrooveFileAction::LOAD;
}

// ─── The quantize pointer ────────────────────────────────────────────────────────────────────────
//
// NOT a setting: it tells A+DPAD how to edit a step, and resets to OFF on app start and on loading a
// project. Nothing about it is written to the song or to settings.
//
// ⚠️ OFF IS IN THE LIST BECAUSE RAW EDITING HAS TO STAY REACHABLE. Armed, every edit moves a PAIR, so
// without OFF no step could be lengthened on its own and `00` (skip this row) would be unreachable —
// its partner would have to absorb twelve ticks in one press.

inline constexpr int GROOVE_QUANTIZE_COUNT = 5;  // OFF · 1/16 · 1/8 · 1/4 · 1/2

inline const char* groove_quantize_label(int quantize) {
    switch (quantize) {
        case 1:  return "1/16";
        case 2:  return "1/8 ";
        case 3:  return "1/4 ";
        case 4:  return "1/2 ";
        default: return "OFF ";
    }
}

/**
 * The partner distance N: how far apart the two steps of an edit are. The group is 2N steps.
 *
 * ⚠️ OFF ANSWERS 1, NOT 0. Nothing pairs when OFF — the caller checks that separately — but the swing
 * percentage still has to report on something, and reading as 1/16 is the useful answer whether or
 * not the editing aid is armed.
 */
inline int groove_partner_distance(int quantize) {
    switch (quantize) {
        case 2:  return 2;   // 1/8
        case 3:  return 4;   // 1/4
        case 4:  return 8;   // 1/2
        default: return 1;   // 1/16, and OFF
    }
}

/** The step a paired edit moves the other way: N along, wrapped inside this step's own group. */
inline int groove_partner_step(int step, int quantize) {
    const int n     = groove_partner_distance(quantize);
    const int group = 2 * n;
    const int start = (step / group) * group;
    return start + ((step - start + n) % group);
}

/**
 * The swing percentage for the group `step` sits in — the first half's share of the whole group — in
 * TENTHS of a percent. Straight is 500; classic triplet swing is 667.
 *
 * ⚠️ THE TENTH IS NOT DECORATION, IT IS WHAT STOPS THE READOUT CONTRADICTING THE PRESET BESIDE IT.
 * The named swing rungs are thirds and halves of 24 tics — 54.17, 58.33, 62.5, 66.67, 70.83 — and the
 * names the world uses for them round in both directions (66 down, 71 up). A whole-number readout
 * would print 67 next to a preset called 16TH 66 and read as a bug; 66.7 reads as the same number.
 *
 * A blank step reads as the neutral length, so an empty groove reports 500 rather than dividing by
 * zero.
 */
inline int groove_swing_tenths(const songcore::Groove& g, int step, int quantize) {
    const int n     = groove_partner_distance(quantize);
    const int group = 2 * n;
    const int start = (step / group) * group;

    int first = 0, second = 0;
    for (int i = 0; i < group; ++i) {
        const int at = start + i;
        if (at >= static_cast<int>(g.steps.size())) break;
        const int v = g.steps[static_cast<size_t>(at)];
        (i < n ? first : second) += (v < 0 ? songcore::TICS_PER_STEP : v);
    }
    const int total = first + second;
    if (total <= 0) return 500;
    return (first * 2000 + total) / (total * 2);  // round(1000 × first / total), integer-only
}

/**
 * Move `step` by `delta` ticks and its partner the other way, so the group's total — and therefore
 * the bar line at the end of it — does not move. Every blank step in the group is materialized to the
 * neutral length first, which can extend the groove's active length to the end of the group.
 *
 * ⚠️ REFUSED WHOLE IF EITHER SIDE WOULD LEAVE 0..255, and nothing is materialized either. A
 * half-applied edit breaks the group's total, which is the one invariant this gesture exists to hold.
 * `0` is a real value here (skip the row), so the floor is a floor and not a sentinel.
 *
 * Returns false when it refused.
 */
inline bool groove_apply_paired(songcore::Groove& g, int step, int delta, int quantize) {
    const int partner = groove_partner_step(step, quantize);
    if (partner == step) return false;

    const int n     = groove_partner_distance(quantize);
    const int group = 2 * n;
    const int start = (step / group) * group;
    const int size  = static_cast<int>(g.steps.size());
    if (start + group > size) return false;

    auto materialized = [&](int at) {
        const int v = g.steps[static_cast<size_t>(at)];
        return v < 0 ? songcore::TICS_PER_STEP : v;
    };

    const int a = materialized(step) + delta;
    const int b = materialized(partner) - delta;
    if (a < 0 || a > 255 || b < 0 || b > 255) return false;

    for (int i = start; i < start + group; ++i)
        g.steps[static_cast<size_t>(i)] = materialized(i);
    g.steps[static_cast<size_t>(step)]    = a;
    g.steps[static_cast<size_t>(partner)] = b;
    return true;
}

struct GrooveState {
    const songcore::Groove& groove;
    int   cursorRow    = 0;                // the tick grid, 0..15
    int   cursorColumn = GROOVE_COL_TICK;  // GROOVE_COL_TICK or GROOVE_COL_PANEL
    int   panelRow     = 0;
    int   panelColumn  = 0;                // the FILE row only
    int   quantize     = 0;                // 0 = OFF; see groove_quantize_label
    Theme theme        = theme_classic();
};

struct GrooveInputResult {
    bool modified    = false;
    int  newQuantize = -1;  // ≥ 0 → the QNT cell moved; the pointer lives in AppState, not the song
};

class GrooveModule {
public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const GrooveState& s) const;

    CursorContext cursor_context(const GrooveState& s) const;

    /**
     * ⚠️ `groove` and `s.groove` are the SAME object — the state carries it const for drawing and the
     * edit needs it mutable. Handing both in keeps the module a pure function of what is on screen.
     */
    GrooveInputResult handle_input(songcore::Groove& groove, const GrooveState& s,
                                   const InputAction& action) const;
};

}  // namespace pt::ui
