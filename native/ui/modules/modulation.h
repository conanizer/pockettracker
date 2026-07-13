#pragma once

// ─── MODULATION (MODS) ───────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/ModulationModule.kt — the four modulation slots of one instrument, drawn
// as two PAIRS: MOD1|MOD2 above, MOD3|MOD4 below.
//
// ── THE CURSOR HAS NO COLUMNS ────────────────────────────────────────────────────────────────────
//
// It is (pair, side, row), not (row, column), and LEFT/RIGHT do not walk along a row — they cross to
// the OTHER SLOT of the pair. That falls out of what the screen is: two independent forms side by
// side, not one table with two halves. Two consequences the port has to honour:
//
//   • HOW FAR DOWN YOU CAN GO DEPENDS ON THE TYPE UNDER YOU. A NONE slot is 1 row (just TYPE); an
//     ADSR is 7. So `mod_slot_row_count` is read on every move, and crossing sideways from a deep
//     slot to a shallow one CLAMPS the row — otherwise the cursor lands on a row that is not drawn.
//   • THE ROW'S MEANING DEPENDS ON THE TYPE TOO. Row 4 is HOLD on an AHD, DEC on an ADSR, and the LFO
//     TRIG mode on an LFO. There is no "the row 4 parameter" to name — every row is a `when(type)`,
//     in the labels, in the values, in the cursor context and in the input handler alike.
//
// ── WHAT IS HIDDEN, AND WHY ──────────────────────────────────────────────────────────────────────
//
// The TYPE cycle offers six of the eight ModTypes. SCALAR is internal (the engine uses it for the
// instrument-volume and phrase-volume routes; it is not a thing a user assigns), and TRACKING has no
// engine implementation at all — the push path clears the slot. Both are hidden from the cycle rather
// than deleted from the enum, because a project saved with one must still LOAD and still DISPLAY
// ("SCL", "TRK"). Hiding is forward-compatible; removing is a migration.

#include <string>
#include <vector>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

/** The LFO's ten shapes. Indices 8 and 9 (RND / DRNK) are the clock-seeded ones — see ptnondet. */
inline const std::vector<std::string>& osc_shapes() {
    static const std::vector<std::string> v{"TRI",  "SIN",  "RMP+", "RMP-", "EXP+",
                                            "EXP-", "SQU+", "SQU-", "RND",  "DRK"};
    return v;
}

/** The LFO's retrigger modes. */
inline const std::vector<std::string>& trig_modes() {
    static const std::vector<std::string> v{"FREE", "RETG", "HOLD", "ONCE"};
    return v;
}

/** The types the TYPE row cycles through — every ModType except the two hidden ones. */
inline const std::vector<songcore::ModType>& user_mod_types() {
    static const std::vector<songcore::ModType> v{
        songcore::ModType::NONE, songcore::ModType::AHD,  songcore::ModType::ADSR,
        songcore::ModType::LFO,  songcore::ModType::DRUM, songcore::ModType::TRIG};
    return v;
}

/** The row labels for a slot of this type. Its size is `mod_slot_row_count`. */
std::vector<std::string> mod_row_labels(songcore::ModType type);

/**
 * The value a row displays. `slot_index` (0..3) is needed for one reason only: a slot that modulates
 * ANOTHER slot draws its destination as "→M3 AMT" — an arrow, and the target's 1-based number — and the
 * target is derived circularly from where this slot sits (`((slot_index + 1) % 4) + 1`).
 */
std::string mod_row_value(const songcore::ModSlot& slot, int row_index, int slot_index);

struct ModulationState {
    const songcore::Instrument& instrument;
    int   cursorRow  = 0;   // the row WITHIN the active slot
    int   cursorPair = 0;   // 0 = MOD1+MOD2, 1 = MOD3+MOD4
    int   cursorSide = 0;   // 0 = left, 1 = right
    Theme theme      = theme_classic();

    int active_slot_index() const { return cursorPair * 2 + cursorSide; }
    const songcore::ModSlot& active_slot() const {
        return instrument.modSlots[static_cast<size_t>(active_slot_index())];
    }
};

struct ModulationInputResult {
    bool modified = false;
};

class ModulationModule {
public:
    static constexpr int WIDTH  = 620;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const ModulationState& s) const;

    CursorContext cursor_context(const ModulationState& s) const;

    /** A+B resets the WHOLE active slot to defaults, which is why this takes the instrument. */
    ModulationInputResult handle_input(songcore::Instrument& ins, int slot_index, int cursor_row,
                                       const InputAction& action) const;
};

}  // namespace pt::ui
