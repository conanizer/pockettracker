#pragma once

// ─── GROOVE EDITOR ───────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/GrooveModule.kt. A 16-step pattern of tick counts: how long each phrase
// step lasts, which is how swing and shuffle are written.
//
//   −1    end of pattern ("--") — the groove loops back to step 0 from here
//   0x00  skip: the phrase row is not triggered and takes no time
//   0x01+ that step lasts this many ticks (0x0C = 12 = the default, one step = one beat/4)
//
// The simplest module in the app, and the only grid editor with NO playback row and NO selection —
// so its row background is just the cursor, with no `row_bg_color` priority ladder to run. Its rows
// are also laid out from a `dataStartY` that already includes TEXT_PADDING (the others add it per
// row), so the cursor rect subtracts it back off. Same pixels, different spelling; kept as Kotlin
// writes it.

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

struct GrooveState {
    const songcore::Groove& groove;
    int   cursorRow    = 0;
    int   cursorColumn = 1;  // 0 = step (read-only), 1 = tick value
    Theme theme        = theme_classic();
};

struct GrooveInputResult {
    bool modified = false;
};

class GrooveModule {
public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const GrooveState& s) const;

    CursorContext cursor_context(const GrooveState& s) const;

    GrooveInputResult handle_input(songcore::Groove& groove, int cursor_row, int cursor_column,
                                   const InputAction& action) const;
};

}  // namespace pt::ui
