#pragma once

// MODS: the four modulation slots of one instrument, as two pairs (MOD1|MOD2, MOD3|MOD4).
// The cursor is (pair, side, row), not columns: LEFT/RIGHT cross to the other slot of the pair. The
// depth and a row's meaning depend on the type, so crossing to a shallower slot clamps the row.
// The TYPE cycle hides SCALAR (internal) and TRACKING (unimplemented); both stay in the enum so a
// saved one still loads ("SCL", "TRK").

#include <string>
#include <vector>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

// The LFO's ten shapes. 8 and 9 (RND / DRNK) are clock-seeded, so a render is not reproducible.
inline const std::vector<std::string>& osc_shapes() {
    static const std::vector<std::string> v{"TRI",  "SIN",  "RMP+", "RMP-", "EXP+",
                                            "EXP-", "SQU+", "SQU-", "RND",  "DRK"};
    return v;
}

inline const std::vector<std::string>& trig_modes() {
    static const std::vector<std::string> v{"FREE", "RETG", "HOLD", "ONCE"};
    return v;
}

// Every ModType except the two hidden ones.
inline const std::vector<songcore::ModType>& user_mod_types() {
    static const std::vector<songcore::ModType> v{
        songcore::ModType::NONE, songcore::ModType::AHD,  songcore::ModType::ADSR,
        songcore::ModType::LFO,  songcore::ModType::DRUM, songcore::ModType::TRIG};
    return v;
}

// The type lays its rows out as AHD (ATK, HOLD, DEC). ⚠️ For the screen: DRUM shares the layout.
inline bool is_ahd_shaped(songcore::ModType t) {
    return t == songcore::ModType::AHD || t == songcore::ModType::DRUM;
}

// Sized `mod_slot_row_count`. A static, by reference: asked up to 28 times a frame.
const std::vector<std::string>& mod_row_labels(songcore::ModType type);

// The value a row displays. `slot_index` is only for "→M3 AMT": a slot modulating the next slot.
std::string mod_row_value(const songcore::ModSlot& slot, int row_index, int slot_index);

struct ModulationState {
    const songcore::Instrument& instrument;
    int   cursorRow  = 0;   // within the active slot
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
