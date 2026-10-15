#pragma once

// The groove editor: 16 steps of tick counts — how long each phrase step lasts (swing, shuffle).
//   −1    end of pattern ("--"): loops back to step 0
//   0x00  skip: the row is not triggered and takes no time
//   0x01+ the step lasts this many ticks (0x0C = one plain step in the default ruler)
// A panel right of the ticks holds name, SAVE/LOAD, QNT, swing and PPQ.
// ⚠️ Two row cursors, both live: the swing readout reports the tick cursor's group, so walking into
// the panel to change QNT must not move it.

#include <string>

#include "songcore/model.h"
#include "songcore/timing.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

// Column 0 is the row number.
inline constexpr int GROOVE_COL_TICK  = 1;
inline constexpr int GROOVE_COL_PANEL = 2;

inline constexpr int GROOVE_PANEL_NAME = 0;
inline constexpr int GROOVE_PANEL_FILE = 1;  // SAVE · LOAD
inline constexpr int GROOVE_PANEL_QNT  = 2;
inline constexpr int GROOVE_PANEL_SWG  = 3;
inline constexpr int GROOVE_PANEL_PPQ  = 4;  // the ruler the tick column is written in
inline constexpr int GROOVE_PANEL_ROWS = 5;

// The PPQ cycle (24 · 48 · 96), in `songcore::GROOVE_RULERS` order.
inline int groove_ruler_index(const songcore::Groove& g) {
    const int ruler = songcore::groove_ticks_per_step(g);
    for (int i = 0; i < songcore::GROOVE_RULER_COUNT; ++i)
        if (songcore::GROOVE_RULERS[i] == ruler) return i;
    return 1;
}

// Only the FILE row has two cells.
inline int groove_panel_cell_count(int panel_row) {
    return panel_row == GROOVE_PANEL_FILE ? 2 : 1;
}

// What a bare A does in the panel; the geometry stays in this file.
enum class GrooveFileAction { NONE, SAVE, LOAD };

inline GrooveFileAction groove_file_action(int panel_row, int panel_column) {
    if (panel_row != GROOVE_PANEL_FILE) return GrooveFileAction::NONE;
    return panel_column == 0 ? GrooveFileAction::SAVE : GrooveFileAction::LOAD;
}

// ─── The quantize pointer ────────────────────────────────────────────────────────────────────────
// Not a setting: how A+DPAD edits a step; OFF on start and on load. Armed, every edit moves a pair.
// ⚠️ OFF keeps raw editing reachable — without it no step could change alone, and `00` could not be set.

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

// The partner distance N; the group is 2N steps. ⚠️ OFF answers 1: the swing readout still needs a
// group.
inline int groove_partner_distance(int quantize) {
    switch (quantize) {
        case 2:  return 2;   // 1/8
        case 3:  return 4;   // 1/4
        case 4:  return 8;   // 1/2
        default: return 1;   // 1/16, and OFF
    }
}

// The step a paired edit moves the other way: N along, wrapped in the group.
inline int groove_partner_step(int step, int quantize) {
    const int n     = groove_partner_distance(quantize);
    const int group = 2 * n;
    const int start = (step / group) * group;
    return start + ((step - start + n) % group);
}

// The group's first-half share, in tenths of a percent (straight 500, triplet 667). Tenths, so it
// does not contradict the preset names, which round both ways. Blanks read as the neutral length.
inline int groove_swing_tenths(const songcore::Groove& g, int step, int quantize) {
    const int n     = groove_partner_distance(quantize);
    const int group = 2 * n;
    const int start = (step / group) * group;

    int first = 0, second = 0;
    for (int i = 0; i < group; ++i) {
        const int at = start + i;
        if (at >= static_cast<int>(g.steps.size())) break;
        const int v = g.steps[static_cast<size_t>(at)];
        (i < n ? first : second) += (v < 0 ? songcore::groove_ticks_per_step(g) : v);
    }
    const int total = first + second;
    if (total <= 0) return 500;
    return (first * 2000 + total) / (total * 2);  // round(1000 × first / total), integer-only
}

// Move `step` by `delta` and its partner the other way, so the group's total (the bar line) holds.
// Blanks in the group are materialized first. ⚠️ Refused whole (false, nothing changed) if either
// side would leave 0..255 — a half edit breaks the total.
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
        return v < 0 ? songcore::groove_ticks_per_step(g) : v;
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
    bool rulerRefused = false;  // the PPQ cell was turned and a step would have passed FF
};

class GrooveModule {
public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const GrooveState& s) const;

    CursorContext cursor_context(const GrooveState& s) const;

    // ⚠️ `groove` and `s.groove` are the same object: const for the draw, mutable for the edit.
    GrooveInputResult handle_input(songcore::Groove& groove, const GrooveState& s,
                                   const InputAction& action) const;
};

}  // namespace pt::ui
