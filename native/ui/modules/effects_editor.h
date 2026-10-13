#pragma once

// ─── EFFECTS ─────────────────────────────────────────────────────────────────────────────────────
//
// The master-bus FX selector, the reverb's and the delay's controls, and the two input EQ slots.
// With MIXER (which owns the send levels into these buses) it is the project's global audio state.
//
// A FORM, like INSTRUMENT: the cursor walks EDITABLE cells, the screen draws those plus headers and
// blank lines, and `effects_row_layout.h` maps one onto the other — so a header can be inserted
// without renumbering cursor rows, and the highlight and a cell's column both come from that table.
//
// TIME reads as a hex byte or a note division ("1/8T") depending on `delaySync`; B toggles which.
//
// Each send opens with a PRESET line — its name, SAVE, LOAD — laid out like the SCALE screen's name row
// (ui/fx_preset_bank.h for the name, ui/fx_preset_io.h for the file).

#include <string>
#include <vector>

#include "songcore/midi_map.h"
#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/effects_row_layout.h"
#include "ui/theme.h"

namespace pt::ui {

struct EffectState {
    const songcore::Project& project;
    int   cursorRow = 0;   // see the ROW_* constants
    Theme theme     = theme_classic();
};

struct EffectInputResult {
    bool modified = false;
};

class EffectModule {
public:
    static constexpr int WIDTH  = 620;
    static constexpr int HEIGHT = 392;

    // The editable rows, named. ⚠️ The values live in effects_row_layout.h: append, never renumber.
    static constexpr int ROW_MASTER_TYPE = static_cast<int>(EffectsRow::MASTER_TYPE);  // OTT / DUST
    static constexpr int ROW_REV_DECAY   = static_cast<int>(EffectsRow::REV_DECAY);    // the tail's length
    static constexpr int ROW_REV_DAMP    = static_cast<int>(EffectsRow::REV_DAMP);
    static constexpr int ROW_REV_EQ      = static_cast<int>(EffectsRow::REV_EQ);       // −1 = off, else a slot
    static constexpr int ROW_DLY_TIME    = static_cast<int>(EffectsRow::DLY_TIME);     // 00..FF, or 0..B synced
    static constexpr int ROW_DLY_FDBK    = static_cast<int>(EffectsRow::DLY_FDBK);
    static constexpr int ROW_DLY_REV     = static_cast<int>(EffectsRow::DLY_REV);      // delay → reverb send
    static constexpr int ROW_DLY_EQ      = static_cast<int>(EffectsRow::DLY_EQ);
    static constexpr int ROW_DLY_TYPE    = static_cast<int>(EffectsRow::DLY_TYPE);     // the delay preset's name
    static constexpr int ROW_DLY_TONE    = static_cast<int>(EffectsRow::DLY_TONE);
    static constexpr int ROW_DLY_WOBBLE  = static_cast<int>(EffectsRow::DLY_WOBBLE);
    static constexpr int ROW_DLY_PONG    = static_cast<int>(EffectsRow::DLY_PONG);
    static constexpr int ROW_REV_TYPE    = static_cast<int>(EffectsRow::REV_TYPE);     // the reverb preset's name
    static constexpr int ROW_REV_PRE     = static_cast<int>(EffectsRow::REV_PRE);      // pre-delay
    static constexpr int ROW_REV_WIDE    = static_cast<int>(EffectsRow::REV_WIDE);     // 80 = untouched
    static constexpr int ROW_REV_MOD     = static_cast<int>(EffectsRow::REV_MOD);      // 40 = as it shipped
    static constexpr int ROW_REV_SIZE    = static_cast<int>(EffectsRow::REV_SIZE);     // the room; 60 = as shipped
    static constexpr int ROW_REV_ALGO    = static_cast<int>(EffectsRow::REV_ALGO);     // 0 = as it shipped
    static constexpr int ROW_REV_SAVE    = static_cast<int>(EffectsRow::REV_SAVE);     // a bare A, no value
    static constexpr int ROW_REV_LOAD    = static_cast<int>(EffectsRow::REV_LOAD);
    static constexpr int ROW_DLY_SAVE    = static_cast<int>(EffectsRow::DLY_SAVE);
    static constexpr int ROW_DLY_LOAD    = static_cast<int>(EffectsRow::DLY_LOAD);
    static constexpr int MAX_CURSOR_ROW  = EFFECTS_ROW_COUNT - 1;

    /** The sync subdivisions, in the order kDelaySyncBeats[] has them in delay-module.h. */
    static const std::vector<std::string>& delay_sync_names();

    void draw(Canvas& c, int x, int y, const EffectState& s) const;

    CursorContext cursor_context(const EffectState& s) const;

    /**
     * What the row under the cursor is CALLED, for MIDI learn. The rows that pick a PRESET or a MODE
     * — the master FX type, the two EQ slots, the two preset lines, ALGO, PONG — decline: a knob
     * sweeping a list of reverb algorithms is not a parameter, it is a page turn.
     */
    songcore::MapTarget map_target(const EffectState& s) const;

    EffectInputResult handle_input(songcore::Project& project, int cursor_row,
                                   const InputAction& action) const;
};

}  // namespace pt::ui
