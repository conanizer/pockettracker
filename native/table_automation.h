#ifndef POCKETTRACKER_TABLE_AUTOMATION_H
#define POCKETTRACKER_TABLE_AUTOMATION_H

// ─── AUS / AUF on a TABLE row ────────────────────────────────────────────────────────────────────
//
// The same feature as phrase automation and a different mechanism, because a table is a different
// machine. `songcore/automation.h` pairs AUS with AUF in STEP space over a phrase, and the SCHEDULER
// emits the span's tics as bus events. A table has neither: it runs inside `AudioEngine`, per voice,
// on the audio thread, off `voice.tableRow`, over 16 rows, with a HOP that can jump backwards.
//
// ⭐⭐ **THE RAMP'S POSITION FOLLOWS THE ROW, and nothing about it is stored.** Progress is re-derived
// every audio block from the row the voice is standing on and the table's own cells, so a HOP back to
// the AUS row restarts the ramp, a HOP into the middle of the span resumes there, and a HOP past the
// AUF ends it. Every alternative needs a ramp origin in per-voice state, which a backwards HOP
// desyncs — the same argument that keeps a phrase ramp out of `TrackState`.
//
// ⚠️ **A SPAN NEVER WRAPS.** `ausRow < aufRow`, always. A table wraps to row 0 and a phrase does not,
// so a ramp from row 14 to an AUF at row 02 would have to mean something different depending on where
// the voice entered the table — which is precisely the stored state the rule above exists to avoid.
//
// ⚠️ **ONE IMPLEMENTATION, TWO CALLERS ON OPPOSITE SIDES OF THE SONGCORE SEAM** — `AudioEngine`, which
// runs the ramp, and the TABLE editor, which dims a cell no ramp uses. They hold different row types
// (`::TableRow`, 8 packed bytes; `songcore::TableRow`, six ints), so the walk is a template over the
// three FX slots and nothing else. Two implementations would let the grid dim a fade the engine
// plays, which is the one lie `find_ramp_cells` was built to prevent on the phrase screen.
//
// So this header includes NOTHING but the curve, which is dependency-free for the same reason.

#include "songcore/automation_curve.h"

namespace table_automation {

// ─── The vocabulary ──────────────────────────────────────────────────────────────────────────────
//
// ⚠️ **THE CODES ARE LITERALS HERE, AND THEY ARE PINNED FROM BOTH SIDES.** This header is included by
// the engine (which may not see `songcore/effects.h`) and by the editor (which may not see
// `audio-defs.h`), so it can name neither side's constants. `songcore/engine_consumer.h` is the one
// file that sees all three lists, and it asserts every row of this table against both of them —
// including the two flags, which are NOT a judgement call:
//
//   • `rampable` must equal "songcore's AUTOMATABLE_PARAMS admits this effect";
//   • `eqPreset`  must equal "…and its endpoints are preset SLOTS rather than values".
//
// So the set a table AUS may ramp is the INTERSECTION of two lists that already exist — the arms
// `AudioEngine::processEffect` has, and the automation registry — and it is derived rather than
// written down. It is five today (`VOL CUT RES EQN EQM`); give `OFF` a registry row, or give the
// table a `PAN` arm, and that effect joins on its own with no edit here beyond its `rampable` flag.
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
    { 0x23, true,  true  },  // EQN
    { 0x24, true,  true  },  // EQM
    { 0x2F, true,  false },  // CUT
    { 0x30, true,  false },  // RES
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

/**
 * Every ramp one table declares.
 *
 * A fixed array, not a vector: this is read on the audio thread, once per voice per block. Sixteen
 * rows can hold at most eight spans, because a span needs a row for each end — so the capacity is
 * the shape of the data rather than a limit anyone can reach.
 */
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

/**
 * Every ramp `rows` declares, in the order they open.
 *
 * The pairing rules come over from `songcore::find_ramps` unchanged — a table row has exactly three
 * FX slots, same as a phrase step, so the POSITIONAL rule ports literally:
 *
 *  • **AUS looks LEFT on its own row** for the nearest ramp-able effect. That effect's type is the
 *    parameter and its value is the start byte, so the number the ramp begins at is a number visible
 *    in the grid — and the cell still applies on its own tic, so the authored start and the ramp's
 *    first value agree by construction rather than by arithmetic.
 *  • **An AUS with nothing ramp-able to its left is INERT** — it does not open, and does not close or
 *    replace a ramp already open. A curve on its own names no parameter.
 *  • **AUF must be on a LATER row**, and the FIRST one closes. An AUF sharing a row with its AUS is
 *    inert and leaves the ramp open: slot order inside a row is not a time order.
 *  • **An EQ-preset endpoint that is not a slot is INERT at either end**, and leaves the AUS open for
 *    a later, legal AUF.
 *  • **The last AUS wins** — a second AUS before any AUF replaces the open ramp rather than nesting.
 *  • **An unclosed AUS produces nothing**, and row 15 is the end: see the no-wrap rule above.
 */
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

/**
 * The ramp's position for a voice standing on `row`, `rowFraction` of the way through it, or −1 when
 * this ramp does not cover that row.
 *
 * ⚠️ **THE TIME BASE IS ROWS, sub-interpolated on the row's own tics.** The span is `aufRow − ausRow`;
 * the fraction inside a row is how far the voice has got through it, so a slow morph is smooth rather
 * than sixteen stair steps. A `TIC` change mid-span stretches the ramp exactly as it stretches
 * everything else in the table — the rate is the TABLE's, not the ramp's, and nothing here has to
 * know it changed. In the three non-advancing TIC modes (`TIC00` trigger, `TICFC` octave map,
 * `TICFE` note map) the fraction is 0 and the ramp holds its value at that row, which is the only
 * thing "the position follows the row" can mean when the row does not move.
 */
inline double table_ramp_position(const TableRamp& r, int row, double rowFraction) {
    if (row < r.ausRow || row > r.aufRow) return -1.0;
    const int span = r.aufRow - r.ausRow;
    if (span <= 0) return -1.0;
    const double t = ((row - r.ausRow) + rowFraction) / span;
    return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

// ─── Which cells a ramp uses — the question the editor asks ──────────────────────────────────────

/**
 * The AUS/AUF cells of a table that a ramp really uses, indexed `[row][slot]` with slots 1-3.
 *
 * Unlike a phrase, a table has exactly one context: it is not placed in a chain, and its rows pair
 * among themselves. So this answer is exact rather than a union over contexts.
 */
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

    /**
     * Does this FX cell take part in a ramp?
     *
     * Only AUS and AUF can answer no — every other effect stands on its own. The ways to write an
     * inert AUS or AUF all reduce to the same thing: the pairing did not use this cell.
     */
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
