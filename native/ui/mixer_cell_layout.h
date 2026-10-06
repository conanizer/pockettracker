#pragma once

// ─── The MIXER screen's cells ────────────────────────────────────────────────────────────────────
//
// The cursor is two ints, (row, column), over a grid that is not rectangular:
//
//   row 0: columns 0..7 = track volumes, column 8 = master volume
//   row 1: column 0 = REV return, column 1 = DEL return, column 8 = master EQ slot
//   row 2: column 8 = OTT|DUST depth
//   row 3: column 8 = LIM pre-gain
//
// `mixer_cell_at` is the ONE place that says which pair is which cell. Editing, MIDI mapping, help,
// mute and the EQ shortcut all switch on its answer — with no `default`, so a new cell is a compile
// warning at every one of them. Only the cursor's moves (`cursor_move.h`) are written out by hand.

namespace pt::ui {

enum class MixerCell {
    NONE,         // between cells: unreachable by the D-pad, but a pair carried in can land here
    TRACK_VOL,    // the column is the track
    MASTER_VOL,
    REV_WET,
    DLY_WET,
    MASTER_EQ,
    MASTER_FX,    // OTT or DUST depth, whichever the project has switched on
    LIMITER,
};

inline MixerCell mixer_cell_at(int row, int column) {
    if (column < 0 || column > 8) return MixerCell::NONE;
    switch (row) {
        case 0:  return column < 8 ? MixerCell::TRACK_VOL : MixerCell::MASTER_VOL;
        case 1:  return column == 0   ? MixerCell::REV_WET
                      : column == 1   ? MixerCell::DLY_WET
                      : column == 8   ? MixerCell::MASTER_EQ
                                      : MixerCell::NONE;
        case 2:  return column == 8 ? MixerCell::MASTER_FX : MixerCell::NONE;
        case 3:  return column == 8 ? MixerCell::LIMITER : MixerCell::NONE;
        default: return MixerCell::NONE;
    }
}

/** Anything that writes one of the two ints alone can land between cells, where the cursor vanishes. */
inline bool mixer_cell_exists(int row, int column) {
    return mixer_cell_at(row, column) != MixerCell::NONE;
}

}  // namespace pt::ui
