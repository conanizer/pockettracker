#pragma once

// ─── THE HELP PANEL — the compact one ────────────────────────────────────────────────────────────
//
// Three lines about the cell under the cursor, drawn IN PLACE OF whatever 620-wide box the screen can
// spare: the visualizer strip on the fourteen screens that have one, and the WAVEFORM panel on the
// sample editor, which has no strip. The mascot sits at the left, the text to its right.
//
// ⚠️ IT REPLACES ITS BOX RATHER THAN COVERING IT: on the strip, the status message (top-left) and the
// selection readout (top-right) stand down while it is up (layout.cpp) — three 21px lines fill it.
//
// ⚠️ THE WIDTH IS 620 IN BOTH BOXES, SO THERE IS ONE TABLE: `HELP_MAX_CHARS` derives from the width
// alone. Only the height differs (70 vs 155); the extra is air — a fourth line would fit on one only.
//
// ⚠️ THREE COLOURS: VIZ BG behind, VIZ WAVE for the text, TXT TITLE for the mascot. The first two
// are the strip's own keys, so a palette that makes the scope readable makes this readable too; the
// mascot takes the header colour so it reads as a figure (one bit deep — mascot_sprite.h).
//
// The FULL overlay is a separate screen and not this file. ⚠️ One screen can never show this one:
// FILE_BROWSER is 640×480 of file rows with no box to spare, and SELECT is its rename/delete chord.

#include "ui/canvas.h"
#include "ui/help_text.h"
#include "ui/helpers.h"        // CHAR_W / CHAR_SPACING — the static_asserts below are geometry
#include "ui/mascot_sprite.h"  // MASCOT_W — TEXT_X is measured past it
#include "ui/theme.h"

namespace pt::ui {

class HelpPanelModule {
  public:
    /** The visualizer strip's width, which is the waveform panel's too — this draws in place of one. */
    static constexpr int WIDTH  = 620;
    /** The strip's height, and the SMALLEST box the panel is ever asked to fill. */
    static constexpr int HEIGHT = 70;

    /** Air between the mascot and the panel edges. 64 + 3 + 3 = 70 = HEIGHT, so it is snug top and bottom. */
    static constexpr int MASCOT_MARGIN = 3;
    /** Air between the mascot and the first character. */
    static constexpr int MASCOT_GUTTER = 7;

    /**
     * Top of the first line of glyphs. Three 21px rows is 63 of the 70, leaving 7 above and 6 below —
     * the odd pixel goes on top, where a cap looks better with air over it than under it.
     */
    static constexpr int TEXT_TOP = 7;

    /** Where the text starts, measured from the panel's own left edge. */
    static constexpr int TEXT_X = MASCOT_MARGIN + MASCOT_W + MASCOT_GUTTER;

    // ⚠️ HELP_MAX_CHARS is written in help_text.h; this pins the geometry it came from, so moving the
    // mascot cannot make every help line one character too long.
    static_assert(TEXT_X + HELP_MAX_CHARS * CHAR_W - CHAR_SPACING <= WIDTH,
                  "HELP_MAX_CHARS no longer fits beside the mascot");
    static_assert(TEXT_X + (HELP_MAX_CHARS + 1) * CHAR_W - CHAR_SPACING > WIDTH,
                  "HELP_MAX_CHARS is short by a character or more - widen it");

    /**
     * Draw the panel filling `box_height` pixels down from `y`, WIDTH across.
     * ⚠️ The text and mascot are a fixed 70px block CENTRED in a taller box, so the strip and the
     * waveform show the identical panel.
     */
    void draw(Canvas& c, int x, int y, HelpTopic topic, const Theme& t,
              int box_height = HEIGHT) const;
};

}  // namespace pt::ui
