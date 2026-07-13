#pragma once

// ─── The FX helper overlay ───────────────────────────────────────────────────────────────────────
//
// A 1:1 port of ui/overlays/FxHelperOverlay.kt — the modal grid-picker that opens when A+UP or
// A+DOWN is pressed while the cursor sits on an FX *type* column (PHRASE cols 4/6/8, TABLE cols
// 3/5/7). 28 effects in a 6×5 grid, with the effect's own documentation above it.
//
// It exists because a tracker's FX column is otherwise unusable: A+UP steps blindly through 28
// three-letter codes with nothing on screen to say what "PVX" or "THO" does. Holding A and reading is
// how you find an effect; releasing A is how you pick it.
//
//   A + DPAD    move in the grid
//   release A   commit the highlighted effect and close  (dispatcher's `on_a_released`)
//
// ⚠️ THE LAST ROW IS CENTRED, AND ITS EDGE CELLS ARE UNREACHABLE. 28 effects fill four rows of six
// (24) and leave four over, which are drawn centred in the last row — columns 1..4, with 0 and 5
// empty. Every navigation function below has a special case for it, and they are not decoration: land
// the cursor on last-row column 0 and it highlights a cell that holds no effect, so releasing A would
// commit `EFFECT_TYPES[24 + (0 - 1)]` — index 23, an effect the user never pointed at.
//
// This header is PURE — no canvas, no theme. The drawing is ui/modules/fx_helper_overlay.h. That
// split is what lets `ptinput` golden the navigation without linking a renderer.

#include <string>
#include <vector>

#include "songcore/effects.h"

namespace pt::ui {

inline constexpr int FX_GRID_COLS = 6;
inline constexpr int FX_GRID_ROWS = 5;

/** The first four rows are full: 24 cells. */
inline constexpr int FX_FULL_CELLS = (FX_GRID_ROWS - 1) * FX_GRID_COLS;
inline constexpr int FX_LAST_ROW   = FX_GRID_ROWS - 1;

/** What is left over for the centred last row — derived, so adding an effect re-centres it. */
inline constexpr int fx_last_row_count() {
    const int n = songcore::EFFECT_TYPE_COUNT - FX_FULL_CELLS;
    return n < 0 ? 0 : (n > FX_GRID_COLS ? FX_GRID_COLS : n);
}
inline constexpr int FX_LAST_ROW_COUNT     = fx_last_row_count();                         // 4
inline constexpr int FX_LAST_ROW_FIRST_COL = (FX_GRID_COLS - FX_LAST_ROW_COUNT) / 2;      // 1
inline constexpr int FX_LAST_ROW_LAST_COL  = FX_LAST_ROW_FIRST_COL + FX_LAST_ROW_COUNT - 1;  // 4

struct FxCell {
    int row = 0;
    int col = 0;
};

/** Linear effect index → its cell. The last row is centred, so its first effect sits at col 1. */
inline FxCell fx_index_to_cell(int index) {
    if (index < FX_FULL_CELLS) return FxCell{index / FX_GRID_COLS, index % FX_GRID_COLS};
    return FxCell{FX_LAST_ROW, FX_LAST_ROW_FIRST_COL + (index - FX_FULL_CELLS)};
}

/** Columns reachable on `row` — the last row excludes its empty edge cells. */
inline int fx_clamp_col_for_row(int row, int col) {
    if (row != FX_LAST_ROW) return col;
    if (col < FX_LAST_ROW_FIRST_COL) return FX_LAST_ROW_FIRST_COL;
    if (col > FX_LAST_ROW_LAST_COL) return FX_LAST_ROW_LAST_COL;
    return col;
}

struct FxHelperState {
    bool isOpen    = false;
    int  cursorRow = 0;
    int  cursorCol = 0;

    /** Linear index into songcore::EFFECT_TYPES for the highlighted cell. */
    int cursor_index() const {
        if (cursorRow < FX_LAST_ROW) return cursorRow * FX_GRID_COLS + cursorCol;
        return FX_FULL_CELLS + (cursorCol - FX_LAST_ROW_FIRST_COL);
    }

    /** The effect CODE under the cursor — what a release of A commits. */
    int selected_effect_code() const { return songcore::effect_type_at(cursor_index()); }
};

/** Open with the cursor on the cell holding `effect_index` (the FX column's current value). */
inline FxHelperState fx_helper_opened_at(int effect_index) {
    const int clamped = effect_index < 0 ? 0
                        : (effect_index > songcore::EFFECT_TYPE_COUNT - 1
                               ? songcore::EFFECT_TYPE_COUNT - 1
                               : effect_index);
    const FxCell c = fx_index_to_cell(clamped);
    return FxHelperState{true, c.row, c.col};
}

// ─── Navigation ──────────────────────────────────────────────────────────────────────────────────
//
// Moving vertically OUT of an edge column (0 or 5) rounds INTO the nearest reachable last-row cell
// (1 or 4). From inside the last row, up/down move straight in the same column — which is 1..4, and
// therefore valid on every row.

inline void fx_move_up(FxHelperState& s) {
    if (s.cursorRow == 0) {  // wrap to the last row, rounding an unreachable column inward
        s.cursorRow = FX_LAST_ROW;
        s.cursorCol = fx_clamp_col_for_row(FX_LAST_ROW, s.cursorCol);
    } else if (s.cursorRow == FX_LAST_ROW) {
        s.cursorRow = FX_LAST_ROW - 1;  // straight up, same column
    } else {
        s.cursorRow -= 1;
    }
}

inline void fx_move_down(FxHelperState& s) {
    if (s.cursorRow == FX_LAST_ROW - 1) {
        s.cursorRow = FX_LAST_ROW;
        s.cursorCol = fx_clamp_col_for_row(FX_LAST_ROW, s.cursorCol);
    } else if (s.cursorRow == FX_LAST_ROW) {
        s.cursorRow = 0;  // wrap to the top, same column
    } else {
        s.cursorRow += 1;
    }
}

inline void fx_move_left(FxHelperState& s) {
    if (s.cursorRow == FX_LAST_ROW) {
        s.cursorCol = (s.cursorCol <= FX_LAST_ROW_FIRST_COL) ? FX_LAST_ROW_LAST_COL
                                                             : s.cursorCol - 1;
    } else {
        s.cursorCol = (s.cursorCol == 0) ? FX_GRID_COLS - 1 : s.cursorCol - 1;
    }
}

inline void fx_move_right(FxHelperState& s) {
    if (s.cursorRow == FX_LAST_ROW) {
        s.cursorCol = (s.cursorCol >= FX_LAST_ROW_LAST_COL) ? FX_LAST_ROW_FIRST_COL
                                                            : s.cursorCol + 1;
    } else {
        s.cursorCol = (s.cursorCol + 1) % FX_GRID_COLS;
    }
}

// ─── The documentation ───────────────────────────────────────────────────────────────────────────
//
// EFFECT_DESCRIPTIONS, indexed to match songcore::EFFECT_TYPES. 2–4 lines each:
//   [0] "SHORT: what it is"   [1] what the value (or its first nibble) does
//   [2] what the second nibble does (optional)   [3] table-specific behaviour (optional)
// Max ~31 chars a line — the overlay is 580 px at font scale 3, and a longer line runs off the box.

inline const std::vector<std::vector<std::string>>& effect_descriptions() {
    static const std::vector<std::vector<std::string>> d = {
        /* 00 --- */ {"---: No effect", "Empty FX slot"},
        /* 01 ARC */ {"ARC: Arpeggio config", "x=mode(0=UP 1=DN 2=PP 3=RND)", "y=speed in ticks"},
        /* 02 CHA */ {"CHA: Probability gate", "x=prob(0=never F=always 8=50%)", "y=target(0=note 1-3=FX slot)"},
        /* 03 LAT */ {"LAT: Latency (delay trigger)", "xx=ticks before note fires"},
        /* 04 GRV */ {"GRV: Groove assign", "xx=groove ID (00=disable)"},
        /* 05 HOP */ {"HOP: Phrase/table jump", "y=target row (FF=stop track)", "table: x=repeat count"},
        /* 06 TIC */ {"TIC: Table tick rate", "01-FB=ticks per row", "FC-FF=special modes"},
        /* 07 ARP */ {"ARP: Arpeggio", "x=+semitones 1st note", "y=+semitones 2nd note", "configure speed with ARC"},
        /* 08 KIL */ {"KIL: Kill voice", "xx=ticks of latency before stop", "00=immediate, 0C=next step"},
        /* 09 OFF */ {"OFF: Sample offset", "xx=start point (00-FF)"},
        /* 10 RND */ {"RND: Randomize FX", "randomizes previous FX column", "x=min nibble  y=max nibble"},
        /* 11 RNL */ {"RNL: Randomize left FX", "same as RND but targets", "FX column to the left"},
        /* 12 RPT */ {"RPT: Retrigger", "RX0: retrig every x ticks", "RXY(Y!=0): retrig y+vol ramp x"},
        /* 13 TBL */ {"TBL: Table override", "xx=table ID for this note"},
        /* 14 THO */ {"THO: Table hop", "xx=target row in current table"},
        /* 15 VOL */ {"VOL: Volume", "xx=volume (00=silent FF=max)"},
        /* 16 PSL */ {"PSL: Pitch slide", "xx=duration(01=fast FF=slow)", "slides pitch from previous note"},
        /* 17 PBN */ {"PBN: Pitch bend", "01-7F=bend up  80-FF=bend down", "00=stop bending"},
        /* 18 PVB */ {"PVB: Vibrato", "x=speed(0-F Hz)", "y=depth(0-F in 1/8 semitone)"},
        /* 19 PVX */ {"PVX: Extreme vibrato", "4x depth and 2x speed", "same format as PVB"},
        /* 20 PIT */ {"PIT: Pitch semitone offset", "00-7F=+0..+127 semitones", "80-FF=-128..-1 semitones", "does not affect slice index"},
        /* 21 SLI */ {"SLI: Slice index override", "xx=slice index (00-FF)", "works even when SLICE=OFF"},
        /* 22 PAN */ {"PAN: Per-note pan", "00=left 80=center FF=right", "this note only; next reverts"},
        /* 23 BCK */ {"BCK: Playback direction", "00=reverse 01=forward", "sampler; toggle live to scratch"},
        /* 24 REV */ {"REV: Per-note reverb send", "xx=send amount (00-FF)", "this note only"},
        /* 25 DEL */ {"DEL: Per-note delay send", "xx=send amount (00-FF)", "this note only"},
        /* 26 EQN */ {"EQN: Per-note EQ slot", "xx=EQ preset slot (00-7F)", "this note only"},
        /* 27 EQM */ {"EQM: Master/mixer EQ slot", "xx=EQ preset slot (00-7F)", "holds till next EQM", "resets to mixer EQ on stop"},
    };
    return d;
}

/** The lines for the highlighted effect. Kotlin's `getOrElse { listOf("---", "No effect") }`. */
inline std::vector<std::string> fx_description_lines(const FxHelperState& s) {
    const auto& all = effect_descriptions();
    const int   i   = s.cursor_index();
    if (i < 0 || i >= static_cast<int>(all.size())) return {"---", "No effect"};
    return all[static_cast<size_t>(i)];
}

}  // namespace pt::ui
