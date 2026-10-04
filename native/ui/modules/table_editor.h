#pragma once

// ─── TABLE EDITOR ────────────────────────────────────────────────────────────────────────────────
//
// 16 rows of per-tic automation an instrument runs under its own notes:
// Step | Transpose | Vol | FX1 | FX2 | FX3.
//
// It looks like the phrase editor and is not, in three ways a shared-drawing refactor would erase:
//   • Both FX cells (name and value) are `textValue`; the phrase editor uses `textTitle`/`textParam`.
//   • The step column has no beat accent — a table row is a tic, not a beat.
//   • The transpose is never "--": `0x00` is drawn dim. Only the volume has an empty (−1, "--").
//
// ⚠️ THREE PLAYBACK ROWS, one per FX column, each with its own playhead and tic rate. They are the
// rows the ENGINE is standing on, resolved per frame (engine_feed.h); their gutters set the gaps
// between the FX columns.

#include "songcore/model.h"
#include "table-lanes.h"
#include "songcore/table_automation.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

#include <functional>

namespace pt::ui {

struct TableState {
    const songcore::Table& table;
    int  cursorRow    = 0;
    int  cursorColumn = 1;   // starts on transpose
    // One per FX column; −1 = that column is not playing this table. The columns have separate
    // playheads, so a table can be sounding with only one, two or three markers on screen.
    int  playbackRows[TABLE_LANES] = {-1, -1, -1};
    int  ticRate      = 0x06;
    bool selectionMode = false;
    std::function<bool(int, int)> isCellSelected = [](int, int) { return false; };
    Theme theme = theme_classic();

    /** How far up songcore::EFFECT_TYPES an FX-type cell may be stepped — see cc::effect_type. */
    int effectTypeCount = songcore::EFFECT_TYPE_COUNT;
};

struct TableInputResult {
    bool modified = false;
};

class TableModule {
public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const TableState& s) const;

    CursorContext cursor_context(const TableState& s) const;

    TableInputResult handle_input(songcore::Table& table, int cursor_row, int cursor_column,
                                  const InputAction& action) const;

private:
    void draw_row(Canvas& c, int x, int y, int index, const songcore::TableRow& row,
                  const TableState& s, const table_automation::TableRampCells& rampCells,
                  int stepX, int transposeX, int volX, int fx1NameX, int fx1ValueX, int fx2NameX,
                  int fx2ValueX, int fx3NameX, int fx3ValueX) const;
};

}  // namespace pt::ui
