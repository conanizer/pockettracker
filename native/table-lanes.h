#pragma once

#include <cstdint>

// ─── The three playheads of one table ───────────────────────────────────────────────────────────
// Lane 0 runs transpose, volume and FX1; lanes 1 and 2 run FX2 and FX3. Each has its own row and rate.
// ⚠️ The lane index is the FX slot minus one everywhere; `TableRamp::paramSlot` (1-3) is converted
// only in `applyTableRamps`. One struct for both voice types.
struct TableLane {
    int   row           =  0;   // the row this lane is standing on (0-15)
    int   lastProcessed = -1;   // the last row whose effects were applied; -1 = none yet
    int   ticRate       =  6;   // 01-FB musical tics per row; 00 trigger, FC octave, FE note, FF 200 Hz
    double frameAccum   =  0.0;    // frames into the row in force; negative until the note's onset
    int   hopRepeat     =  0;   // jumps left on an active `HOP XY` (X > 0)
    uint16_t ausEaten   =  0;   // rows whose AUS a CHA ate on their last pass — ramp off
    int   hopTarget     = -1;   // its target row; -1 = no HOP counting

    // `HOP FF` stops this lane; `tableId` goes to -1 only once all three have stopped.
    bool  active        = true;
};

inline constexpr int TABLE_LANES = 3;

// What a hit picked up left of each INS switch on its way: semitones add, volume multiplies, an FX is
// a write the sounding table can overwrite. Steering, KIL and the switch's own cells are not carried.
inline constexpr int TABLE_CARRY_FX_MAX = 8;   // two slots left of an INS, on up to four routers
struct TableCarry {
    float   semitones = 0.0f;
    float   volume    = 1.0f;
    int     fxCount   = 0;
    uint8_t fxType[TABLE_CARRY_FX_MAX]  = {};
    uint8_t fxValue[TABLE_CARRY_FX_MAX] = {};
};

// The "no table" arguments.
inline constexpr int TABLE_TICS_DEFAULT[TABLE_LANES] = {6, 6, 6};
inline constexpr int TABLE_ROWS_TOP[TABLE_LANES]     = {0, 0, 0};

// Places the three lanes for a starting note — the only lane write at a trigger. TICFC/TICFE lanes
// are placed by the note; the rest at `startRows` (a TIC00 lane resumes — see `tic00Cursor`).
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
