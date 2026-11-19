#pragma once

// MODS: the four modulation slots of one instrument, as two pairs (MOD1|MOD2, MOD3|MOD4).
// The cursor is (pair, side, row), not columns: LEFT/RIGHT cross to the other slot of the pair. The
// depth and a row's meaning depend on the type, so crossing to a shallower slot clamps the row.
// The TYPE cycle offers AHD, ADSR, LFO and TRK; a saved SCL or ENV still loads, plays and shows.

#include <algorithm>
#include <array>
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

// The TYPE cycle.
inline const std::vector<songcore::ModType>& user_mod_types() {
    static const std::vector<songcore::ModType> v{
        songcore::ModType::NONE, songcore::ModType::AHD, songcore::ModType::ADSR,
        songcore::ModType::LFO,  songcore::ModType::TRACKING, songcore::ModType::VELOCITY};
    return v;
}

// The DEST cycle of each instrument type: the DESTs it offers, in enum order.
inline const std::vector<songcore::ModDest>& offered_mod_dests(songcore::InstrumentType t) {
    using namespace songcore;
    static const auto table = [] {
        std::array<std::vector<ModDest>, std::size(INSTRUMENT_TYPE_NAMES)> a;
        for (size_t ty = 0; ty < a.size(); ++ty)
            for (int d = 0; d < MOD_DEST_COUNT; ++d)
                if (mod_dest_offered(static_cast<InstrumentType>(ty), static_cast<ModDest>(d)))
                    a[ty].push_back(static_cast<ModDest>(d));
        return a;
    }();
    return table[std::min(static_cast<size_t>(t), table.size() - 1)];
}

// The DEST cycle of slot `slot_index`: the instrument's, with the routes into the other slots where
// MOD_AMT stands — from the next slot round, only the filled ones, RATE and BOTH only into an LFO. The
// slot's own DEST stays, so emptying the slot it aims at does not make the first step jump.
inline std::vector<songcore::ModDest> slot_mod_dests(const songcore::Instrument& ins, int slot_index) {
    using namespace songcore;
    const ModDest own = ins.modSlots[static_cast<size_t>(slot_index)].dest;
    std::vector<ModDest> v;
    for (ModDest d : offered_mod_dests(ins.instrumentType)) {
        if (d == ModDest::MOD_AMT) {
            if (own == ModDest::MOD_AMT || own == ModDest::MOD_RATE || own == ModDest::MOD_BOTH)
                v.push_back(own);   // not from a file, which names the slot
            for (int k = 1; k < 4; ++k) {
                const int     to   = (slot_index + k) % 4;
                const ModType type = ins.modSlots[static_cast<size_t>(to)].type;
                const bool takesAmt  = type != ModType::NONE;
                const bool takesRate = type == ModType::LFO;
                for (ModTargetPart part : {ModTargetPart::AMT, ModTargetPart::RATE, ModTargetPart::BOTH}) {
                    const ModDest r = mod_target_dest(to, part);
                    if (r == own || (part == ModTargetPart::AMT ? takesAmt : takesRate)) v.push_back(r);
                }
            }
            continue;
        }
        if (mod_target(d, slot_index).slot >= 0) continue;   // the routes are all placed above
        v.push_back(d);
    }
    return v;
}

inline const std::vector<std::string>& lfo_polarities() {
    static const std::vector<std::string> v{"BI", "UNI+", "UNI-"};
    return v;
}

// The CRV row of AHD and ADSR, in the order it steps: gentle to steep.
inline const std::vector<std::string>& env_curves() {
    static const std::vector<std::string> v{"LINEAR", "ANALOG", "FAST"};
    return v;
}

// `ModSlot::curve` ↔ its step in `env_curves`. ⚠️ The stored numbers are a file's (LINEAR 0, FAST 1,
// ANALOG 2), not the step; the swap is its own inverse and serves both ways.
inline int curve_step(int curve) { return curve == 1 ? 2 : curve == 2 ? 1 : 0; }

// What a row of a slot is. Which rows a slot has, top to bottom, follows its type (`mod_rows`).
enum class ModRow { TYPE, DEST, AMT, ATK, HOLD, DEC, SUS, REL, CRV, OSC, TRIG, FREQ, POL };

const std::vector<ModRow>& mod_rows(songcore::ModType type);

// The rows this slot takes on the MODS screen, TYPE included — read by cursor and renderer alike.
inline int mod_row_count(const songcore::ModSlot& s) { return static_cast<int>(mod_rows(s.type).size()); }

// Row `row_index` of a slot of `type`; TYPE past its last row (a backstop: the cursor stays inside).
ModRow mod_row_at(songcore::ModType type, int row_index);

const char* mod_row_label(ModRow row);

// The value a row displays, its words in `c`'s case. `slot_index` is only for "→M3 AMT": a slot
// modulating the next slot.
std::string mod_row_value(const Canvas& c, const songcore::ModSlot& slot, int row_index, int slot_index);

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
