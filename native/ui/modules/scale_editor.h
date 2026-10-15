#pragma once

// The scale editor: which of the twelve intervals belong to one of the project's 16 scales, plus
// the project's KEY. Row 0 NAME (factory shape, SAVE, LOAD), row 1 KEY, rows 2..13 the degrees.
// ⚠️ Degrees count from the key, not from C: a key change re-labels rows and stores nothing, so a
// slot is a shape the key positions.
// ⚠️ The root and the last degree on cannot be turned off — an empty scale leaves the note cursor
// nowhere to step.
// Only the NAME row has a column (`ui/cursor_move.h`). Cycling the name replaces the degrees, as the
// THEME row does; a slot drifted from its shape draws a `*`.

#include <string>

#include "songcore/model.h"
#include "songcore/scale_bank.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

inline constexpr int SCALE_NAME_ROW  = 0;
inline constexpr int SCALE_KEY_ROW   = 1;
inline constexpr int SCALE_ROW_COUNT = 14;

inline constexpr int SCALE_NAME_COL_NAME  = 0;
inline constexpr int SCALE_NAME_COL_SAVE  = 1;
inline constexpr int SCALE_NAME_COL_LOAD  = 2;
inline constexpr int SCALE_NAME_COL_COUNT = 3;

// −1 for the NAME and KEY rows.
inline int scale_row_degree(int row) {
    return (row >= SCALE_KEY_ROW + 1 && row < SCALE_ROW_COUNT) ? row - (SCALE_KEY_ROW + 1) : -1;
}

// What a bare A does; the geometry stays in this file.
enum class ScaleNameAction { NONE, SAVE, LOAD };

inline ScaleNameAction scale_name_action(int row, int column) {
    if (row != SCALE_NAME_ROW) return ScaleNameAction::NONE;
    if (column == SCALE_NAME_COL_SAVE) return ScaleNameAction::SAVE;
    if (column == SCALE_NAME_COL_LOAD) return ScaleNameAction::LOAD;
    return ScaleNameAction::NONE;
}

struct ScaleState {
    const songcore::Scale& scale;
    int   key         = 0;   // the project's, 0-11
    int   cursorRow   = 0;
    int   cursorColumn = 0;  // NAME row only

    // The pitch classes sounding now (bit 0 = C), for the `>` marker.
    // ⚠️ What is heard, not scheduled — filled from the engine's voices, as the note monitor is; the
    // sequencer runs ahead. Pitch classes, not degrees, so it can land on an OFF row.
    unsigned soundingMask = 0;

    Theme theme     = theme_classic();
};

struct ScaleInputResult {
    bool modified = false;
    // The KEY row edits the project, so the key travels out separately.
    int  newKey   = -1;
};

class ScaleModule {
public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const ScaleState& s) const;

    CursorContext cursor_context(const ScaleState& s) const;

    ScaleInputResult handle_input(songcore::Scale& scale, int key, int cursor_row, int cursor_column,
                                  const InputAction& action) const;
};

}  // namespace pt::ui
