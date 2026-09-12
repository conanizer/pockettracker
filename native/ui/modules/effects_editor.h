#pragma once

// ─── EFFECTS ─────────────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/EffectModule.kt: the master-bus FX selector, the reverb's own controls,
// the delay's, and the two input EQ slots. Together with MIXER (which owns the *send levels* into
// these buses) it is the whole of the project's global audio state.
//
// A FORM, like INSTRUMENT: rows with section headers between them, one column through the master
// section and two through the reverb's and the delay's, where each send's eight cells read as a pair
// of columns under a TYPE. The cursor walks EDITABLE cells while the screen draws those plus the
// headers and the blank lines between them, and `ui/effects_row_layout.h` turns one into the other —
// which is what lets a header be inserted without renumbering every cursor row, and why both the
// highlight and the column a cell is drawn in come from the table rather than from the cursor.
//
// The one place it is stateful-looking but is not: TIME reads as either a hex byte or a note division
// ("1/8T"), depending on `delaySync` — the same cell, two vocabularies. B toggles which; see
// InputDispatcher::on_button_b.

#include <string>
#include <vector>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/effects_row_layout.h"
#include "ui/theme.h"

namespace pt::ui {

struct EffectState {
    const songcore::Project& project;
    int   cursorRow = 0;   // 0..15 — see the ROW_* constants
    Theme theme     = theme_classic();
};

struct EffectInputResult {
    bool modified = false;
};

class EffectModule {
public:
    static constexpr int WIDTH  = 620;
    static constexpr int HEIGHT = 392;

    // The editable rows, named. ⚠️ The VALUES live in ui/effects_row_layout.h and are appended-to
    // there, never renumbered: the recorded EFFECTS cases speak in these numbers.
    static constexpr int ROW_MASTER_TYPE = static_cast<int>(EffectsRow::MASTER_TYPE);  // OTT / DUST
    static constexpr int ROW_REV_SIZE    = static_cast<int>(EffectsRow::REV_SIZE);     // reverb feedback
    static constexpr int ROW_REV_DAMP    = static_cast<int>(EffectsRow::REV_DAMP);
    static constexpr int ROW_REV_EQ      = static_cast<int>(EffectsRow::REV_EQ);       // −1 = off, else a slot
    static constexpr int ROW_DLY_TIME    = static_cast<int>(EffectsRow::DLY_TIME);     // 00..FF, or 0..B synced
    static constexpr int ROW_DLY_FDBK    = static_cast<int>(EffectsRow::DLY_FDBK);
    static constexpr int ROW_DLY_REV     = static_cast<int>(EffectsRow::DLY_REV);      // delay → reverb send
    static constexpr int ROW_DLY_EQ      = static_cast<int>(EffectsRow::DLY_EQ);
    static constexpr int ROW_DLY_TYPE    = static_cast<int>(EffectsRow::DLY_TYPE);     // a delay preset
    static constexpr int ROW_DLY_TONE    = static_cast<int>(EffectsRow::DLY_TONE);
    static constexpr int ROW_DLY_WOBBLE  = static_cast<int>(EffectsRow::DLY_WOBBLE);
    static constexpr int ROW_DLY_PONG    = static_cast<int>(EffectsRow::DLY_PONG);
    static constexpr int ROW_REV_TYPE    = static_cast<int>(EffectsRow::REV_TYPE);     // a reverb preset
    static constexpr int ROW_REV_PRE     = static_cast<int>(EffectsRow::REV_PRE);      // pre-delay
    static constexpr int ROW_REV_WIDE    = static_cast<int>(EffectsRow::REV_WIDE);     // 80 = untouched
    static constexpr int ROW_REV_MOD     = static_cast<int>(EffectsRow::REV_MOD);      // 40 = as it shipped
    static constexpr int ROW_REV_ALGO    = static_cast<int>(EffectsRow::REV_ALGO);     // 0 = as it shipped
    static constexpr int ROW_REV_DECAY   = static_cast<int>(EffectsRow::REV_DECAY);    // MVERB only
    static constexpr int ROW_REV_DENSITY = static_cast<int>(EffectsRow::REV_DENSITY);  // MVERB only
    static constexpr int MAX_CURSOR_ROW  = EFFECTS_ROW_COUNT - 1;

    /** The sync subdivisions, in the order kDelaySyncBeats[] has them in delay-module.h. */
    static const std::vector<std::string>& delay_sync_names();

    /** The delay presets, in the order delay-presets.h has them, plus USER at the end. */
    static const std::vector<std::string>& delay_type_names();

    /** The reverb presets, in the order reverb-presets.h has them, plus USER at the end. */
    static const std::vector<std::string>& reverb_type_names();

    void draw(Canvas& c, int x, int y, const EffectState& s) const;

    CursorContext cursor_context(const EffectState& s) const;

    EffectInputResult handle_input(songcore::Project& project, int cursor_row,
                                   const InputAction& action) const;
};

}  // namespace pt::ui
