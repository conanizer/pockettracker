#pragma once

// ─── SCALE EDITOR ────────────────────────────────────────────────────────────────────────────────
//
// Which of the twelve chromatic intervals belong to one of the project's 16 scales, the KEY the whole
// song sits in, and the factory shape the slot was taken from. The first screen with no Kotlin twin —
// the Kotlin UI was deleted before scales existed — so it is written to the tree's own grain rather
// than ported.
//
// Its 14 rows are ONE cursor column, as GROOVE's are, except the top one:
//
//   row 0       NAME — three cells: the factory shape (A+LEFT/RIGHT cycles it), SAVE, LOAD.
//   row 1       KEY — the root, 0-11, shown as a note name. Global to the project, not to the slot.
//   rows 2..13  the twelve degrees ABOVE the key, each ON or off.
//
// ⚠️ THE DEGREE ROWS ARE COUNTED FROM THE KEY, NOT FROM C. Row 2 is always the root, whatever the key
// is, so changing the key re-labels every row and changes no stored degree — which is the property
// that makes a slot a SHAPE (major, dorian) that the key positions, rather than a fixed set of notes.
//
// ⚠️ THE ROOT — AND THE LAST SURVIVING DEGREE — CANNOT BE TURNED OFF. A scale with nothing in it is a
// scale in which no note can be typed, and the note cursor would have nowhere to step; refusing it here
// is cheaper than teaching every consumer to survive it.
//
// ⚠️ THE NAME ROW IS THE ONLY ROW WITH A HORIZONTAL CURSOR, and the app's LEFT/RIGHT for this screen
// is written to say exactly that (`ui/cursor_move.h`). Everywhere else the column is meaningless, which
// is why `ScaleState::cursorColumn` is read on row 0 and ignored below it.
//
// ⚠️ CYCLING THE NAME REPLACES THE TWELVE DEGREES. That is the THEME row's behaviour and it is the same
// bargain: the cycle is how you pick a starting shape, and turning degrees off afterwards is how you
// make it yours. A slot that has drifted from the shape it is named after draws a `*`.

#include <string>

#include "songcore/model.h"
#include "songcore/scale_bank.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

/** Row 0 is the NAME, row 1 the KEY; the twelve degrees follow. */
inline constexpr int SCALE_NAME_ROW  = 0;
inline constexpr int SCALE_KEY_ROW   = 1;
inline constexpr int SCALE_ROW_COUNT = 14;

/** The three cells of the NAME row. */
inline constexpr int SCALE_NAME_COL_NAME  = 0;
inline constexpr int SCALE_NAME_COL_SAVE  = 1;
inline constexpr int SCALE_NAME_COL_LOAD  = 2;
inline constexpr int SCALE_NAME_COL_COUNT = 3;

/** The degree a cursor row edits, or −1 for the NAME and KEY rows. */
inline int scale_row_degree(int row) {
    return (row >= SCALE_KEY_ROW + 1 && row < SCALE_ROW_COUNT) ? row - (SCALE_KEY_ROW + 1) : -1;
}

/** What a bare A does where the cursor is. The dispatcher's arm, so the geometry stays in one file. */
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
    int   cursorColumn = 0;  // NAME row only — see the header note

    /**
     * Which PITCH CLASSES are sounding right now, bit 0 = C — the degrees that get a `>` marker.
     *
     * ⚠️⚠️ **IT MUST BE WHAT IS HEARD, NOT WHAT IS SCHEDULED.** The sequencer runs two phrases ahead,
     * so a marker fed from it would light the degree of a bar nobody has reached. It is filled from
     * the ENGINE's voices (`AppState::trackNotes`), the same source the note monitor uses, which is
     * the only place in the app that knows what is coming out of the speaker.
     *
     * ⚠️ It is a PITCH-CLASS mask rather than a degree mask on purpose: a degree only exists relative
     * to a key, and the whole point of the marker is that it can land on a row this scale has turned
     * OFF — which is how you see a track playing under a different scale, or an instrument with
     * transposing disabled.
     *
     * ⚠️ Handed in rather than read, like `blinkPhaseMs`: a drawing layer with no engine is what
     * makes the marker reproducible in a screenshot.
     */
    unsigned soundingMask = 0;

    Theme theme     = theme_classic();
};

struct ScaleInputResult {
    bool modified = false;
    /** The KEY row edits the PROJECT, not the scale — so the new key travels back out separately. */
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
