#pragma once

// ─── INSTRUMENT POOL ─────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/InstrumentPoolModule.kt — an M8-style overview of all 128 instrument
// slots with the handful of mixer values you actually want to compare ACROSS instruments (volume, the
// two sends, the EQ slot). Reached with R+UP from INSTRUMENT, and R+RIGHT jumps back into it.
//
// ⚠️ ITS CURSOR ROW IS NOT ITS OWN. The selected row IS `currentInstrument` — the same field the
// INSTRUMENT screen edits and the TABLE screen follows. That is what makes the pool a NAVIGATOR rather
// than a table: walk down it, jump to INSTRUMENT, and you are on the slot you were looking at. So only
// the COLUMN lives in the pool's state (AppState::poolCursorColumn); moving up and down changes the
// project's selected instrument (ui/cursor_move.h, `move_pool_selection`).
//
// Columns: 0 NAME · 1 V (volume) · 2 RV (reverb send) · 3 DE (delay send) · 4 EQ.
// Column 0 is selection-only — A on an empty slot loads a source into it (the dispatcher's job), and
// A+B clears the slot. The four value columns edit with A+DPAD like anywhere else.
//
// Reorder (M8's EDIT+UP/DOWN on the name column) is deliberately not here, as in the Kotlin.

#include <cstdint>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

struct InstrumentPoolState {
    const songcore::Project& project;
    int   selectedInstrument = 0;   // …which IS the cursor row
    int64_t sampleRamBytes   = 0;   // the USED RAM total, drawn in the header (engine_feed.h)
    int   cursorColumn       = 0;
    Theme theme              = theme_classic();
};

class InstrumentPoolModule {
public:
    static constexpr int WIDTH  = 620;
    static constexpr int HEIGHT = 392;

    // The USED RAM readout in the title row, as offsets from the module's left edge. Public because
    // only layout.h knows both this module's x AND the editor clip, and it asserts there that the
    // widest total this can print still lands left of the right bar.
    static constexpr int RAM_LABEL_X   = 280;
    static constexpr int RAM_VALUE_X   = 348;
    static constexpr int RAM_MAX_CHARS = 9;   // "1234.5 MB" — four digits before the point

    void draw(Canvas& c, int x, int y, const InstrumentPoolState& s) const;

    CursorContext cursor_context(const InstrumentPoolState& s) const;

    /** True if the action changed anything. `instrument` is the selected slot. */
    bool handle_input(songcore::Instrument& instrument, int cursor_column,
                      const InputAction& action) const;

private:
    void draw_row(Canvas& c, int x, int row_y, int slot, const songcore::Instrument& ins,
                  const InstrumentPoolState& s, const Theme& t) const;
};

}  // namespace pt::ui
