#pragma once

#include <cstdint>

// ─── THE THREE PLAYHEADS OF ONE TABLE ────────────────────────────────────────────────────────────
//
// A table's columns do not advance together. Lane 0 carries `transpose`, `volume` and FX1; lane 1 is
// FX2 alone and lane 2 is FX3 alone. Each owns a row cursor AND a rate, so `HOP 00` in FX2 loops FX2
// while the rest of the table runs its sixteen rows, and a `TIC` in FX2 changes FX2's speed only.
//
// ⚠️ **THE LANE INDEX IS THE FX SLOT MINUS ONE**, everywhere — the row work, the ramps and the three
// markers on the TABLE screen all key off that one identity. `TableRamp::paramSlot` is 1-3 and is
// converted at exactly one place (`applyTableRamps`).
//
// ⚠️ **IT IS ONE STRUCT FOR BOTH VOICE TYPES** because `processTableTick` is one template over them.
// Eight parallel `[3]` arrays in each of `Voice` and `SoundfontVoice` would be the same state written
// four times, and the reset paths are where they would drift.
struct TableLane {
    int   row           =  0;   // the row this lane is standing on (0-15)
    int   lastProcessed = -1;   // the last row whose effects were applied; -1 = none yet
    int   ticRate       =  6;   // 01-FB musical tics per row; 00 trigger, FC octave, FE note, FF 200 Hz
    double frameAccum   =  0.0;    // frames into the row in force; negative until the note's onset
    int   hopRepeat     =  0;   // jumps left on an active `HOP XY` (X > 0)
    int   hopTarget     = -1;   // its target row; -1 = no HOP counting

    // ⚠️ `HOP FF` freezes THIS LANE, not the table. The voice's `tableId` only goes to -1 once all
    // three are down — a stop typed in FX3 must not silence the note and volume columns.
    bool  active        = true;
};

inline constexpr int TABLE_LANES = 3;

/**
 * What a hit picked up on its way through the INS rows of the tables before the one it sounds with —
 * the transpose, volume and FX to the LEFT of each switch. Each composes the way it already does:
 * semitones add, volume multiplies, and an FX is a write that the sounding table's own rows can
 * overwrite. Steering (HOP, THO, TIC), KIL and the switch's own cells (INS, RNL, CHA) are not carried.
 */
inline constexpr int TABLE_CARRY_FX_MAX = 8;   // two slots left of an INS, on up to four routers
struct TableCarry {
    float   semitones = 0.0f;
    float   volume    = 1.0f;
    int     fxCount   = 0;
    uint8_t fxType[TABLE_CARRY_FX_MAX]  = {};
    uint8_t fxValue[TABLE_CARRY_FX_MAX] = {};
};

// The "no table involved" arguments, so a trigger that passes no table still has trailing defaults.
inline constexpr int TABLE_TICS_DEFAULT[TABLE_LANES] = {6, 6, 6};
inline constexpr int TABLE_ROWS_TOP[TABLE_LANES]     = {0, 0, 0};

/**
 * Place all three lanes for a note that is starting.
 *
 * ⚠️ **BOTH VOICE TYPES CALL THIS AND NOTHING ELSE WRITES A LANE AT TRIGGER.** The octave and note
 * maps need the note, which is captured late in `Voice::trigger`, so the sampler used to reset the
 * cursor in one place and then move it again in another — two sites for one decision, and the second
 * one silently owned the first's result. Deriving the row from the rate here is the whole rule:
 * a lane at TICFC or TICFE is PLACED by the note, every other lane starts where the caller says.
 *
 * `startRows` is per lane so a TIC00 lane can resume where the track's previous note left it while
 * its neighbours start at 0 — see `AudioEngine::tic00Cursor`.
 */
inline void reset_table_lanes(TableLane (&lanes)[TABLE_LANES], const int (&ticRates)[TABLE_LANES],
                              const int (&startRows)[TABLE_LANES], int octave, int pitch) {
    for (int l = 0; l < TABLE_LANES; ++l) {
        TableLane& lane = lanes[l];
        lane = TableLane{};
        lane.ticRate = ticRates[l];
        if (lane.ticRate == 0xFC)      lane.row = octave < 15 ? octave : 15;   // octave map
        else if (lane.ticRate == 0xFE) lane.row = pitch;                       // note map
        else                           lane.row = startRows[l] > 0 ? startRows[l] % 16 : 0;
    }
}
