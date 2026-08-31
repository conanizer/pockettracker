#pragma once

// ─── SCALE EDITOR ────────────────────────────────────────────────────────────────────────────────
//
// Which of the twelve chromatic intervals belong to one of the project's 16 scales, and the KEY the
// whole song sits in. The first screen with no Kotlin twin — the Kotlin UI was deleted before scales
// existed — so it is written to the tree's own grain rather than ported.
//
// Its 13 rows are ONE cursor column, as GROOVE's are, but the top row is not a degree:
//
//   row 0       KEY — the root, 0-11, shown as a note name. Global to the project, not to the slot.
//   rows 1..12  the twelve degrees ABOVE the key, each ON or off.
//
// ⚠️ THE DEGREE ROWS ARE COUNTED FROM THE KEY, NOT FROM C. Row 1 is always the root, whatever the key
// is, so changing the key re-labels every row and changes no stored degree — which is the property
// that makes a slot a SHAPE (major, dorian) that the key positions, rather than a fixed set of notes.
//
// ⚠️ ROW 1 — THE ROOT — CANNOT BE TURNED OFF, and neither can the last surviving degree. A scale with
// nothing in it is a scale in which no note can be typed, and the note cursor would have nowhere to
// step; refusing it here is cheaper than teaching every consumer to survive it.

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

/** Row 0 is the KEY; the twelve degrees follow it. */
inline constexpr int SCALE_KEY_ROW   = 0;
inline constexpr int SCALE_ROW_COUNT = 13;

/** The degree a cursor row edits, or −1 for the KEY row. */
inline int scale_row_degree(int row) { return (row >= 1 && row < SCALE_ROW_COUNT) ? row - 1 : -1; }

struct ScaleState {
    const songcore::Scale& scale;
    int   key       = 0;   // the project's, 0-11
    int   cursorRow = 0;

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

    ScaleInputResult handle_input(songcore::Scale& scale, int key, int cursor_row,
                                  const InputAction& action) const;
};

}  // namespace pt::ui
