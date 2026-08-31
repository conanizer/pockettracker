#pragma once

// ─── THE HELP PANEL — the compact one ────────────────────────────────────────────────────────────
//
// Three lines about the cell under the cursor, drawn IN PLACE OF the visualizer, in the same 620×70
// strip and at the same origin. The mascot sits at the left, the text to its right.
//
// ⚠️ **IT REPLACES THE STRIP RATHER THAN COVERING IT**, and the two readouts that share that strip —
// the global status message top-left, and the selection/clipboard readout top-right — stand down
// while it is up (`layout.cpp`). Three lines of 21px fill the strip exactly; there is no corner left
// for either of them.
//
// ⚠️ **TWO COLOURS AND NO MORE: VIZ BG behind, VIZ WAVE in front.** They are the strip's own theme
// keys, so the panel re-themes itself with the visualizer it stands in for, and a palette that made
// the scope readable makes this readable too. The mascot is one bit deep for exactly this reason —
// see ui/mascot_sprite.h.
//
// The FULL overlay is a separate screen and not this file. ⚠️ Two screens can never show this one:
// FILE_BROWSER and SAMPLE_EDITOR are 640×480 and have no strip to put it in.

#include "ui/canvas.h"
#include "ui/help_text.h"
#include "ui/helpers.h"        // CHAR_W / CHAR_SPACING — the static_asserts below are geometry
#include "ui/mascot_sprite.h"  // MASCOT_W — TEXT_X is measured past it
#include "ui/theme.h"

namespace pt::ui {

class HelpPanelModule {
  public:
    /** The visualizer strip, exactly — this draws in its place, not beside it. */
    static constexpr int WIDTH  = 620;
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

    // ⚠️ HELP_MAX_CHARS is written down in ui/help_text.h and checked against the table there. This is
    // the geometry it was derived FROM, pinned so that moving the mascot cannot silently make every
    // help line one character too long.
    static_assert(TEXT_X + HELP_MAX_CHARS * CHAR_W - CHAR_SPACING <= WIDTH,
                  "HELP_MAX_CHARS no longer fits beside the mascot");
    static_assert(TEXT_X + (HELP_MAX_CHARS + 1) * CHAR_W - CHAR_SPACING > WIDTH,
                  "HELP_MAX_CHARS is short by a character or more - widen it");

    void draw(Canvas& c, int x, int y, HelpTopic topic, const Theme& t) const;
};

}  // namespace pt::ui
