#pragma once

// ─── The FX helper overlay ───────────────────────────────────────────────────────────────────────
//
// The modal grid-picker that opens when A+UP or A+DOWN is pressed while the cursor sits on an FX
// *type* column (PHRASE cols 4/6/8, TABLE cols 3/5/7): the effects in a six-column grid, with the
// highlighted one's documentation above it.
//
// It exists because a tracker's FX column is otherwise unusable — A+RIGHT steps blindly through dozens
// of three-letter codes with nothing on screen to say what "PVX" or "THO" does. Holding A and reading
// is how you find an effect; releasing A is how you pick it.
//
//   A + DPAD    move in the grid
//   release A   commit the highlighted effect and close  (dispatcher's `on_a_released`)
//
// How many effects the grid holds depends on the build — see FxGrid below.
//
// This header is PURE — no canvas, no theme. The drawing is ui/modules/fx_helper_overlay.h. That
// split is what lets `ptinput` golden the navigation without linking a renderer.

#include <string>
#include <vector>

#include "songcore/effects.h"

namespace pt::ui {

inline constexpr int FX_GRID_COLS = 6;

/**
 * The grid's shape for a given number of visible effects.
 *
 * The COUNT is a parameter because a build with the MIDI surfaces hidden (platform_caps.h `midi`)
 * shows the first songcore::EFFECT_TYPE_COUNT_NO_MIDI effects and needs one row fewer. Everything
 * else is derived from it, so the two shapes are the same code with a different number in it:
 * 42 effects → seven full rows, 36 → six.
 *
 * ⚠️ A LAST ROW THAT DOES NOT FILL IS CENTRED, AND ITS EDGE CELLS ARE UNREACHABLE. With two left
 * over the columns are 2..3 and 0, 1, 4 and 5 hold nothing. Every navigation function has a case for
 * it, and they are not decoration: land on last-row column 0 and the highlight sits on a cell holding
 * no effect, so releasing A would commit the index one below the row's first — an effect the user
 * never pointed at.
 *
 * ⚠️⚠️ **NEITHER SHIPPING COUNT EXERCISES THAT PATH TODAY** — both divide by six exactly, so the
 * centring code is live, reachable and reached by nothing the app itself does. `ptinput` therefore
 * drives SYNTHETIC counts, one per remainder, so the coverage does not come and go with the length of
 * EFFECT_TYPES. Do not conclude from a green app that it works, and do not delete the cases.
 */
struct FxGrid {
    int count        = songcore::EFFECT_TYPE_COUNT;  // visible effects
    int rows         = 0;
    int fullCells    = 0;   // cells in the rows above the last one
    int lastRow      = 0;
    int lastRowCount = 0;   // effects in the centred last row
    int firstCol     = 0;   // its leftmost reachable column
    int lastCol      = 0;   // its rightmost

    static constexpr FxGrid of(int count) {
        FxGrid g;
        g.count        = count;
        g.rows         = (count + FX_GRID_COLS - 1) / FX_GRID_COLS;
        g.lastRow      = g.rows - 1;
        g.fullCells    = g.lastRow * FX_GRID_COLS;
        g.lastRowCount = count - g.fullCells;
        g.firstCol     = (FX_GRID_COLS - g.lastRowCount) / 2;
        g.lastCol      = g.firstCol + g.lastRowCount - 1;
        return g;
    }
};

// Every effect, MIDI included — the default, and what a build with the MIDI surfaces on shows. Which
// grid the app actually uses is decided ONCE, by InputDispatcher::visible_effect_type_count().
inline constexpr FxGrid FX_GRID_FULL = FxGrid::of(songcore::EFFECT_TYPE_COUNT);

// The box the overlay draws is sized from `rows`, so a grid taller than the screen would draw its
// last row outside it. The ceiling is asserted where the height and the screen are both in scope —
// `box_h` in modules/fx_helper_overlay.cpp — rather than as a number copied to here, which is how it
// came to read `<= 6` while the real geometry fits more than twice that.

struct FxCell {
    int row = 0;
    int col = 0;
};

/** Linear effect index → its cell. The last row is centred, so its first effect sits at `firstCol`. */
inline FxCell fx_index_to_cell(int index, const FxGrid& g = FX_GRID_FULL) {
    if (index < g.fullCells) return FxCell{index / FX_GRID_COLS, index % FX_GRID_COLS};
    return FxCell{g.lastRow, g.firstCol + (index - g.fullCells)};
}

/** Columns reachable on `row` — the last row excludes its empty edge cells. */
inline int fx_clamp_col_for_row(int row, int col, const FxGrid& g = FX_GRID_FULL) {
    if (row != g.lastRow) return col;
    if (col < g.firstCol) return g.firstCol;
    if (col > g.lastCol) return g.lastCol;
    return col;
}

struct FxHelperState {
    bool   isOpen    = false;
    int    cursorRow = 0;
    int    cursorCol = 0;
    FxGrid grid      = FX_GRID_FULL;

    /** Linear index into songcore::EFFECT_TYPES for the highlighted cell. */
    int cursor_index() const {
        if (cursorRow < grid.lastRow) return cursorRow * FX_GRID_COLS + cursorCol;
        return grid.fullCells + (cursorCol - grid.firstCol);
    }

    /** The effect CODE under the cursor — what a release of A commits. */
    int selected_effect_code() const { return songcore::effect_type_at(cursor_index()); }
};

/** Open with the cursor on the cell holding `effect_index` (the FX column's current value). */
inline FxHelperState fx_helper_opened_at(int effect_index, const FxGrid& g = FX_GRID_FULL) {
    const int clamped = effect_index < 0 ? 0
                        : (effect_index > g.count - 1 ? g.count - 1 : effect_index);
    const FxCell c = fx_index_to_cell(clamped, g);
    return FxHelperState{true, c.row, c.col, g};
}

// ─── Navigation ──────────────────────────────────────────────────────────────────────────────────
//
// Moving vertically INTO a centred last row rounds an unreachable column inward to the nearest cell
// that holds an effect. From inside that row, up/down move straight in the same column, which is one
// of the reachable ones and therefore valid on every row above it. When the last row is full — which
// both shipping counts give — every clamp here is the identity and the rules collapse to a plain grid.

inline void fx_move_up(FxHelperState& s) {
    if (s.cursorRow == 0) {  // wrap to the last row, rounding an unreachable column inward
        s.cursorRow = s.grid.lastRow;
        s.cursorCol = fx_clamp_col_for_row(s.grid.lastRow, s.cursorCol, s.grid);
    } else if (s.cursorRow == s.grid.lastRow) {
        s.cursorRow = s.grid.lastRow - 1;  // straight up, same column
    } else {
        s.cursorRow -= 1;
    }
}

inline void fx_move_down(FxHelperState& s) {
    if (s.cursorRow == s.grid.lastRow - 1) {
        s.cursorRow = s.grid.lastRow;
        s.cursorCol = fx_clamp_col_for_row(s.grid.lastRow, s.cursorCol, s.grid);
    } else if (s.cursorRow == s.grid.lastRow) {
        s.cursorRow = 0;  // wrap to the top, same column
    } else {
        s.cursorRow += 1;
    }
}

inline void fx_move_left(FxHelperState& s) {
    if (s.cursorRow == s.grid.lastRow) {
        s.cursorCol = (s.cursorCol <= s.grid.firstCol) ? s.grid.lastCol : s.cursorCol - 1;
    } else {
        s.cursorCol = (s.cursorCol == 0) ? FX_GRID_COLS - 1 : s.cursorCol - 1;
    }
}

inline void fx_move_right(FxHelperState& s) {
    if (s.cursorRow == s.grid.lastRow) {
        s.cursorCol = (s.cursorCol >= s.grid.lastCol) ? s.grid.firstCol : s.cursorCol + 1;
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
        /* 28 VTR */ {"VTR: Track mixer fader", "xx=level (00=silent FF=max)", "replaces the MIXER fader", "resets to the MIXER on stop"},
        /* 29 VMV */ {"VMV: Master mixer fader", "xx=level (00=silent FF=max)", "replaces the MASTER fader", "resets to the MIXER on stop"},
        /* 30 AUS */ {"AUS: Automation start", "xx=curve 00=IN 80=LIN FF=OUT", "ramps the FX slot to its left", "to the value of the next AUF"},
        /* 31 AUF */ {"AUF: Automation finish", "xx=destination value", "ends the ramp an AUS opened", "one chain, or one table"},
        /* 32 CUT */ {"CUT: Filter cutoff", "xx=cutoff (00=low FF=high)", "needs a FILTER TYPE in INST", "this note only"},
        /* 33 RES */ {"RES: Filter resonance", "xx=resonance (00-FF)", "needs a FILTER TYPE in INST", "this note only"},
        /* 34 SCA */ {"SCA: Track scale", "x=key (0=C 1=C# .. B=B)", "y=scale slot (0-F)", "holds till next SCA or stop"},
        /* 35 SCG */ {"SCG: Global scale", "x=key (0=C 1=C# .. B=B)", "y=scale slot (0-F)", "moves all 8 tracks at once"},
        /* 36 MPG */ {"MPG: MIDI program change", "xx=program (00-7F)", "external instruments only"},
        /* 37 MPB */ {"MPB: MIDI pitch bend", "00=down 80=centre FF=up", "absolute - external only"},
        // ⚠️ **NO APOSTROPHE AND NO SEMICOLON IN A DESCRIPTION** — the font has neither glyph and draws
        // a BLANK, so "instrument's" renders as "INSTRUMENT S". It is silent: the string is right, the
        // width is right, only the pixels are wrong, and these lines are the only long prose in the UI.
        // ⚠️ Pre-existing, not new: BCK's "sampler; toggle live to scratch" has always drawn as
        // "SAMPLER  TOGGLE…". Caught by ptshot — the one tool here that looks at pixels. Stick to
        // letters, digits, and `: = - ( ) . /`, all of which are proven by the entries above.
        /* 38 CCA */ {"CCA: MIDI CC slot A", "xx=value (00-FF)", "moves the CC number set in", "the instrument CC A row"},
        /* 39 CCB */ {"CCB: MIDI CC slot B", "xx=value (00-FF)", "moves the CC number set in", "the instrument CC B row"},
        /* 40 CCC */ {"CCC: MIDI CC slot C", "xx=value (00-FF)", "moves the CC number set in", "the instrument CC C row"},
        /* 41 CCD */ {"CCD: MIDI CC slot D", "xx=value (00-FF)", "moves the CC number set in", "the instrument CC D row"},
    };
    return d;
}

/**
 * The lines for the highlighted effect, or a two-line placeholder when the cursor is off the table.
 *
 * By reference into `effect_descriptions()`'s static, because the picker redraws this every frame it
 * is up and the 2-4 description lines are past SSO. The out-of-range arm needs a static of its own to
 * have something to bind to.
 */
inline const std::vector<std::string>& fx_description_lines(const FxHelperState& s) {
    static const std::vector<std::string> kNone{"---", "No effect"};
    const auto& all = effect_descriptions();
    const int   i   = s.cursor_index();
    if (i < 0 || i >= static_cast<int>(all.size())) return kNone;
    return all[static_cast<size_t>(i)];
}

}  // namespace pt::ui
