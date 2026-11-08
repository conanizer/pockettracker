#ifndef POCKETTRACKER_TABLE_AUTOMATION_H
#define POCKETTRACKER_TABLE_AUTOMATION_H

// AUS / AUF on a TABLE row. A table runs per voice on the audio thread, with a HOP that can jump
// backwards, so it pairs here rather than in automation.h.
// ⚠️ The ramp's position FOLLOWS THE ROW and nothing is stored: re-derived every block from the row
// the voice is on, so a HOP back restarts it and a HOP past the AUF ends it.
// ⚠️ A span never wraps: `ausRow < aufRow`.
// ⚠️ One implementation for the engine and the TABLE editor, which hold different row types — a
// template over the three FX slots, including nothing but the curve.

#include "automation_curve.h"

namespace table_automation {

// The commands a table row runs. `engine_consumer.h` checks every row's flags against the automation
// registry.
struct Arm {
    int  code;
    bool rampable;   // the registry admits it → a table AUS to its right can ramp it
    bool eqPreset;   // …and its endpoints are `Project::eqPresets` slots, not values
};

inline constexpr Arm ARMS[] = {
    { songcore::FX_HOP,    false, false },
    { songcore::FX_TIC,    false, false },
    { songcore::FX_KILL,   false, false },
    { songcore::FX_OFFSET, false, false },  // no CC id and no absolute path, so not in the registry
    { songcore::FX_THO,    false, false },
    { songcore::FX_VOLUME, true,  false },
    { songcore::FX_PAN,    true,  false },
    { songcore::FX_EQN,    true,  true  },
    { songcore::FX_EQM,    true,  true  },
    { songcore::FX_CUT,    true,  false },
    { songcore::FX_RES,    true,  false },
    { songcore::FX_LPF,    true,  false },
    { songcore::FX_HPF,    true,  false },
    { songcore::FX_BPF,    true,  false },
    { songcore::FX_DRV,    true,  false },
    { songcore::FX_CRU,    false, false },  // a packed pair, deliberately out of the registry
    { songcore::FX_FIN,    true,  false },
    { songcore::FX_LPO,    false, false },  // a relative STEP, so a ramp over it has nothing to interpolate
    { songcore::FX_TIM,    true,  false },  // the delay's echo time; a ramp over it is the tape swoop
};
inline constexpr int ARM_COUNT = static_cast<int>(sizeof(ARMS) / sizeof(Arm));

inline constexpr int EQ_PRESET_SLOTS = songcore::RAMP_EQ_PRESET_SLOTS;

inline constexpr const Arm* arm_for(int fxCode) {
    for (int i = 0; i < ARM_COUNT; ++i)
        if (ARMS[i].code == fxCode) return &ARMS[i];
    return nullptr;
}

/** The same question as a yes/no, for the compile-time checks. */
inline constexpr bool has_arm(int fxCode) { return arm_for(fxCode) != nullptr; }

/** Is this byte a slot an EQ-preset ramp may use as an endpoint? */
inline constexpr bool is_eq_slot(int value) { return value >= 0 && value < EQ_PRESET_SLOTS; }

// ─── One declared ramp ───────────────────────────────────────────────────────────────────────────

struct TableRamp {
    int  fxCode    = 0;
    int  ausRow    = -1;
    int  aufRow    = -1;
    int  paramSlot = 0;   // 1-3: the slot AUS read its start value from
    int  ausSlot   = 0;   // 1-3: where AUS itself sits
    int  aufSlot   = 0;   // 1-3: where the AUF that closed it sits
    int  startByte = 0;   // a VALUE, or a preset SLOT when `eqPreset`
    int  destByte  = 0;   // …likewise
    int  curveByte = songcore::AUS_CURVE_LINEAR;
    bool eqPreset  = false;
};

// Every ramp one table declares — a fixed array for the audio thread; 16 rows hold at most 8 spans.
struct TableRampSet {
    static constexpr int CAPACITY = 8;
    TableRamp items[CAPACITY];
    int       count = 0;
};

// Every ramp `rows` declares, by `songcore::pair_ramps`.
template <typename RowT>
inline TableRampSet find_table_ramps(const RowT* rows, int rowCount) {
    TableRampSet out;
    songcore::pair_ramps(
            rowCount < 16 ? rowCount : 16,
            [&](int row) { return songcore::ramp_row_fx(rows[row]); },
            [](int type) {
                const Arm* a = arm_for(type);
                if (a == nullptr || !a->rampable) return songcore::RampArm::NONE;
                return a->eqPreset ? songcore::RampArm::EQ_SLOT : songcore::RampArm::VALUE;
            },
            [&](const songcore::RampPair& p) {
                if (out.count >= TableRampSet::CAPACITY) return;
                TableRamp& r = out.items[out.count++];
                r.fxCode    = p.paramType;
                r.eqPreset  = p.arm == songcore::RampArm::EQ_SLOT;
                r.ausRow    = p.ausPos;
                r.aufRow    = p.aufPos;
                r.paramSlot = p.paramSlot;
                r.ausSlot   = p.ausSlot;
                r.aufSlot   = p.aufSlot;
                r.startByte = p.startByte;
                r.destByte  = p.destByte;
                r.curveByte = p.curveByte;
            });
    return out;
}

// ─── Where in a ramp a voice is ──────────────────────────────────────────────────────────────────

// The ramp's position for a voice `rowFraction` through `row`, or −1 outside it. The time base is
// ROWS, sub-interpolated within one, so a TIC change stretches the ramp like the rest of the table;
// in a non-advancing TIC mode the fraction is 0 and the ramp holds.
inline double table_ramp_position(const TableRamp& r, int row, double rowFraction) {
    if (row < r.ausRow || row > r.aufRow) return -1.0;
    const int span = r.aufRow - r.ausRow;
    if (span <= 0) return -1.0;
    const double t = ((row - r.ausRow) + rowFraction) / span;
    return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

// ─── Which cells a ramp uses — the question the editor asks ──────────────────────────────────────

// The AUS/AUF cells a ramp really uses, `[row][slot]` with slots 1-3 (a table has one context).
struct TableRampCells {
    bool used[16][4] = {};

    void mark(const TableRampSet& ramps) {
        for (int i = 0; i < ramps.count; ++i) {
            const TableRamp& r = ramps.items[i];
            if (r.ausRow >= 0 && r.ausRow < 16 && r.ausSlot >= 1 && r.ausSlot <= 3)
                used[r.ausRow][r.ausSlot] = true;
            if (r.aufRow >= 0 && r.aufRow < 16 && r.aufSlot >= 1 && r.aufSlot <= 3)
                used[r.aufRow][r.aufSlot] = true;
        }
    }

    // Does this FX cell take part? Only AUS and AUF can answer no.
    bool active(int fxType, int row, int slot) const {
        if (fxType != songcore::FX_AUS && fxType != songcore::FX_AUF) return true;
        if (row < 0 || row >= 16 || slot < 1 || slot > 3) return false;
        return used[row][slot];
    }
};

template <typename RowT>
inline TableRampCells find_table_ramp_cells(const RowT* rows, int rowCount) {
    TableRampCells cells;
    cells.mark(find_table_ramps(rows, rowCount));
    return cells;
}

}  // namespace table_automation

#endif  // POCKETTRACKER_TABLE_AUTOMATION_H
