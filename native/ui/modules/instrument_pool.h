#pragma once

// The instrument pool: all 128 slots with volume, sends and EQ side by side. R+UP from INSTRUMENT,
// R+RIGHT back. ⚠️ Its cursor row is `currentInstrument` itself (`move_pool_selection`); only the
// column lives here. Columns: 0 NAME · 1 V · 2 RV · 3 DE · 4 EQ. On NAME, A loads, A+B clears.

#include <cstdint>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/platform_caps.h"
#include "ui/theme.h"

namespace pt::ui {

struct InstrumentPoolState {
    const songcore::Project& project;
    int   selectedInstrument = 0;   // the cursor row
    int64_t sampleRamBytes   = 0;   // the header's RAM total, debug builds only
    int   cursorColumn       = 0;
    PlatformCaps caps{};
    Theme theme              = theme_classic();
};

class InstrumentPoolModule {
public:
    static constexpr int WIDTH  = 620;
    static constexpr int HEIGHT = 392;

    // Public so layout.h can assert the widest total clears the right bar.
    static constexpr int RAM_LABEL_X   = 280;
    static constexpr int RAM_VALUE_X   = 348;
    static constexpr int RAM_MAX_CHARS = 9;   // "1234.5 MB" — four digits before the point

    void draw(Canvas& c, int x, int y, const InstrumentPoolState& s) const;

    CursorContext cursor_context(const InstrumentPoolState& s) const;

    // True if anything changed. `instrument` is the selected slot.
    bool handle_input(songcore::Instrument& instrument, int cursor_column,
                      const InputAction& action) const;

private:
    void draw_row(Canvas& c, int x, int row_y, int slot, const songcore::Instrument& ins,
                  const InstrumentPoolState& s, const Theme& t) const;
};

}  // namespace pt::ui
