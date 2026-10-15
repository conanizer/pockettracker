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

// ⚠️ The codes are literals (this header sees neither side's constants); `engine_consumer.h` asserts
// every row, flags included, against both the engine's arms and the automation registry.
struct Arm {
    int  code;
    bool rampable;   // the registry admits it → a table AUS to its right can ramp it
    bool eqPreset;   // …and its endpoints are `Project::eqPresets` slots, not values
};

inline constexpr Arm ARMS[] = {
    { 0x08, false, false },  // HOP
    { 0x09, false, false },  // TIC
    { 0x0B, false, false },  // KIL
    { 0x0F, false, false },  // OFF — no CC id and no absolute path, so not in the registry
    { 0x15, false, false },  // THO
    { 0x16, true,  false },  // VOL
    { 0x1F, true,  false },  // PAN
    { 0x23, true,  true  },  // EQN
    { 0x24, true,  true  },  // EQM
    { 0x2F, true,  false },  // CUT
    { 0x30, true,  false },  // RES
    { 0x33, true,  false },  // LPF
    { 0x34, true,  false },  // HPF
    { 0x35, true,  false },  // BPF
    { 0x37, true,  false },  // DRV
    { 0x38, false, false },  // CRU — a packed pair, deliberately out of the registry (effects.h)
    { 0x39, true,  false },  // FIN
    { 0x3B, false, false },  // LPO — a relative STEP, so a ramp over it has nothing to interpolate
    { 0x3D, true,  false },  // TIM — the delay's echo time; a ramp over it is the tape swoop
};
inline constexpr int ARM_COUNT = static_cast<int>(sizeof(ARMS) / sizeof(Arm));

inline constexpr int FX_AUS_CODE = 0x2D;   // xx = the curve
inline constexpr int FX_AUF_CODE = 0x2E;   // xx = the destination
inline constexpr int EQ_PRESET_SLOTS = 128;

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

// A table row's three FX slots, from either side's row type. Both spell the fields the same way and
// differ only in width, so one accessor covers both.
template <typename RowT> inline int fx_type(const RowT& r, int slot) {
    return slot == 1 ? static_cast<int>(r.fx1Type)
         : slot == 2 ? static_cast<int>(r.fx2Type)
                     : static_cast<int>(r.fx3Type);
}
template <typename RowT> inline int fx_value(const RowT& r, int slot) {
    return slot == 1 ? static_cast<int>(r.fx1Value)
         : slot == 2 ? static_cast<int>(r.fx2Value)
                     : static_cast<int>(r.fx3Value);
}

// Every ramp `rows` declares, by `songcore::find_ramps`' rules: AUS takes the nearest ramp-able effect
// to its LEFT (none: inert), the first AUF on a LATER row closes, the last AUS wins, an unclosed AUS
// produces nothing.
template <typename RowT>
inline TableRampSet find_table_ramps(const RowT* rows, int rowCount) {
    TableRampSet out;
    TableRamp    open;
    bool         isOpen = false;

    const int last = rowCount < 16 ? rowCount : 16;
    for (int row = 0; row < last; ++row) {
        for (int slot = 1; slot <= 3; ++slot) {
            const int type = fx_type(rows[row], slot);

            if (type == FX_AUS_CODE) {
                for (int left = slot - 1; left >= 1; --left) {
                    const Arm* a = arm_for(fx_type(rows[row], left));
                    if (a == nullptr || !a->rampable) continue;
                    const int startValue = fx_value(rows[row], left);
                    // The nearest ramp-able effect IS the parameter, so an endpoint it cannot
                    // express ends the search rather than continuing it.
                    if (a->eqPreset && !is_eq_slot(startValue)) break;
                    open           = TableRamp{};
                    open.fxCode    = a->code;
                    open.eqPreset  = a->eqPreset;
                    open.ausRow    = row;
                    open.paramSlot = left;
                    open.ausSlot   = slot;
                    open.startByte = startValue;
                    open.curveByte = fx_value(rows[row], slot);
                    isOpen         = true;
                    break;
                }
            } else if (type == FX_AUF_CODE && isOpen && row > open.ausRow) {
                // ⚠️ Before anything that closes the ramp: an endpoint that is not a slot leaves the
                // AUS open for a later AUF, and this cell unused (so the editor dims it).
                if (open.eqPreset && !is_eq_slot(fx_value(rows[row], slot))) continue;
                open.aufRow  = row;
                open.aufSlot = slot;
                open.destByte = fx_value(rows[row], slot);
                if (out.count < TableRampSet::CAPACITY) out.items[out.count++] = open;
                isOpen = false;
            }
        }
    }
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
        if (fxType != FX_AUS_CODE && fxType != FX_AUF_CODE) return true;
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
