#pragma once

// ─── TABLE EDITOR ────────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/TableModule.kt. 16 rows of per-tic automation an instrument runs under
// its own notes: Step | Transpose | Vol | FX1 | FX2 | FX3.
//
// It LOOKS like the phrase editor and it is not, in three ways that a "shared drawCell" refactor
// would quietly erase. Each is inherited verbatim:
//
//   • ITS FX CELLS ARE `textValue`, BOTH OF THEM. The phrase editor paints an FX *name* in `textTitle`
//     and its *value* in `textParam`; the table paints both in `textValue`.
//   • ITS STEP COLUMN HAS NO BEAT ACCENT. Phrase and chain brighten every 4th row number; a table row
//     is a tic, not a beat, so there is nothing to accent and the column is flat `textEmpty`.
//   • ITS TRANSPOSE IS NEVER "--". `0x00` means no transpose and is drawn dim, but it is still drawn —
//     unlike the volume, where −1 is a genuine "leave it alone" and shows "--".
//
// The playback rows are the rows the ENGINE's voice is on, not ones the sequencer reports: the
// layout resolves them per frame from `getVoiceTableId`/`getVoiceTableRows` (see ui/engine_feed.h).
// Hence the −1 sentinel below where the Kotlin has `Int?`.
//
// ⚠️ **THERE ARE THREE OF THEM, one per FX column**, because each column has its own playhead and its
// own tic rate. The gutters they draw in are what set the horizontal gaps between the FX columns —
// see the layout in the .cpp.

#include "songcore/model.h"
#include "table-lanes.h"
#include "table_automation.h"
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
