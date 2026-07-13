#pragma once

// ─── EFFECTS ─────────────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/EffectModule.kt: the master-bus FX selector, the reverb's own controls,
// the delay's, and the two input EQ slots. Together with MIXER (which owns the *send levels* into
// these buses) it is the whole of the project's global audio state.
//
// A FORM, like INSTRUMENT — but a much simpler one: a single column of rows, no columns at all, and
// section headers between them. The section headers are what the CURSOR_TO_VIS map exists for: the
// cursor walks 8 EDITABLE rows (0..7) while the screen draws 15 VISUAL ones, and the map is what turns
// one into the other. Keeping them separate is what lets a header be inserted without renumbering
// every cursor row — and it is why the row highlight is drawn from the map rather than from the cursor.
//
// The one place it is stateful-looking but is not: TIME reads as either a hex byte or a note division
// ("1/8T"), depending on `delaySync` — the same cell, two vocabularies. SELECT toggles which; see
// InputDispatcher::on_select.

#include <string>
#include <vector>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

struct EffectState {
    const songcore::Project& project;
    int   cursorRow = 0;   // 0..7 — see the ROW_* constants
    Theme theme     = theme_classic();
};

struct EffectInputResult {
    bool modified = false;
};

class EffectModule {
public:
    static constexpr int WIDTH  = 620;
    static constexpr int HEIGHT = 392;

    // The eight editable rows.
    static constexpr int ROW_MASTER_TYPE = 0;   // OTT / DUST
    static constexpr int ROW_REV_SIZE    = 1;   // reverb feedback
    static constexpr int ROW_REV_DAMP    = 2;
    static constexpr int ROW_REV_EQ      = 3;   // −1 = off, else a slot in the 128-preset bank
    static constexpr int ROW_DLY_TIME    = 4;   // 00..FF free, or 0..B when synced
    static constexpr int ROW_DLY_FDBK    = 5;
    static constexpr int ROW_DLY_REV     = 6;   // delay → reverb send
    static constexpr int ROW_DLY_EQ      = 7;
    static constexpr int MAX_CURSOR_ROW  = 7;

    /** The sync subdivisions, in the order kDelaySyncBeats[] has them in delay-module.h. */
    static const std::vector<std::string>& delay_sync_names();

    void draw(Canvas& c, int x, int y, const EffectState& s) const;

    CursorContext cursor_context(const EffectState& s) const;

    EffectInputResult handle_input(songcore::Project& project, int cursor_row,
                                   const InputAction& action) const;
};

}  // namespace pt::ui
