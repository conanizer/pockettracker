#pragma once

// ─── MODULATION (MODS) ───────────────────────────────────────────────────────────────────────────
//
// The four modulation slots of one instrument, drawn as two PAIRS: MOD1|MOD2 above, MOD3|MOD4 below.
//
// ── THE CURSOR HAS NO COLUMNS ────────────────────────────────────────────────────────────────────
//
// It is (pair, side, row): LEFT/RIGHT cross to the OTHER SLOT of the pair, not along a row.
//   • How far down you can go depends on the type: NONE is 1 row, ADSR 7. Crossing from a deep slot
//     to a shallow one CLAMPS the row, or the cursor lands on a row that is not drawn.
//   • A row's meaning depends on the type too: row 4 is HOLD on AHD, DEC on ADSR, TRIG on an LFO.
//
// ── WHAT IS HIDDEN, AND WHY ──────────────────────────────────────────────────────────────────────
//
// The TYPE cycle offers six of the eight ModTypes. SCALAR is internal (the engine's volume routes)
// and TRACKING has no engine implementation. Both stay in the enum so a project saved with one still
// loads and displays ("SCL", "TRK").

#include <string>
#include <vector>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

/** The LFO's ten shapes. 8 and 9 (RND / DRNK) are clock-seeded, so not reproducible in a render. */
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

/**
 * Does this type lay its rows out as AHD does — ATK, HOLD, DEC rather than ATK, DEC, SUS, REL?
 * ⚠️ For the SCREEN, not the engine: DRUM is its own envelope with the same three times. Every
 * reader of a MODS row below row 2 asks this, so it is asked in one place.
 */
inline bool is_ahd_shaped(songcore::ModType t) {
    return t == songcore::ModType::AHD || t == songcore::ModType::DRUM;
}

/**
 * The row labels for a slot of this type; its size is `mod_slot_row_count`. By reference to a
 * static: the screen asks up to 28 times a frame.
 */
const std::vector<std::string>& mod_row_labels(songcore::ModType type);

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

    /** Edits slot `slot_index` of `ins`. */
    ModulationInputResult handle_input(songcore::Instrument& ins, int slot_index, int cursor_row,
                                       const InputAction& action) const;
};

}  // namespace pt::ui
