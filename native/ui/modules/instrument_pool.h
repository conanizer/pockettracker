#pragma once

// ─── INSTRUMENT POOL ─────────────────────────────────────────────────────────────────────────────
// An overview of all 128 instrument slots with the mixer values worth comparing across them (volume,
// the two sends, the EQ slot). R+UP from INSTRUMENT; R+RIGHT jumps back into it.
//
// ⚠️ ITS CURSOR ROW IS NOT ITS OWN: the selected row IS `currentInstrument`, the field INSTRUMENT
// edits and TABLE follows. Only the column lives here (AppState::poolCursorColumn); moving up and
// down changes the project's selected instrument (cursor_move.h, `move_pool_selection`).
//
// Columns: 0 NAME · 1 V (volume) · 2 RV (reverb send) · 3 DE (delay send) · 4 EQ. Column 0 is
// selection-only: A on an empty slot loads a source (the dispatcher's job), A+B clears it.

#include <cstdint>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/platform_caps.h"
#include "ui/theme.h"

namespace pt::ui {

struct InstrumentPoolState {
    const songcore::Project& project;
    int   selectedInstrument = 0;   // …which IS the cursor row
    int64_t sampleRamBytes   = 0;   // the RAM total in the header — `caps.debug` builds only
    int   cursorColumn       = 0;
    PlatformCaps caps{};
    Theme theme              = theme_classic();
};

class InstrumentPoolModule {
public:
    static constexpr int WIDTH  = 620;
    static constexpr int HEIGHT = 392;

    // The RAM readout's offsets from the module's left edge. Public so layout.h can assert the widest
    // total still lands left of the right bar — unconditionally, though the readout is debug-only.
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
