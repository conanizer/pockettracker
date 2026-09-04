#pragma once

// ─── THE HELP PANEL — the compact one ────────────────────────────────────────────────────────────
//
// Three lines about the cell under the cursor, drawn IN PLACE OF whatever 620-wide box the screen can
// spare: the visualizer strip on the fourteen screens that have one, and the WAVEFORM panel on the
// sample editor, which has no strip. The mascot sits at the left, the text to its right.
//
// ⚠️ **IT REPLACES ITS BOX RATHER THAN COVERING IT**, and on the strip the two readouts that share it
// — the global status message top-left, and the selection/clipboard readout top-right — stand down
// while it is up (`layout.cpp`). Three lines of 21px fill the strip exactly; there is no corner left
// for either of them.
//
// ⚠️ **THE WIDTH IS THE SAME 620 IN BOTH BOXES, AND THAT IS WHY THERE IS ONE TABLE.** `HELP_MAX_CHARS`
// is derived from the width alone, so the sample editor costs no second budget and no second set of
// lines. Only the HEIGHT differs — 70 on the strip, 155 in the waveform — and the extra 85px is spent
// as air above and below rather than on more text: a fourth line would exist on one screen only.
//
// ⚠️ **TWO COLOURS AND NO MORE: VIZ BG behind, VIZ WAVE in front.** They are the strip's own theme
// keys, so the panel re-themes itself with the visualizer it stands in for, and a palette that made
// the scope readable makes this readable too. The waveform panel's ground is VIZ BG as well, so the
// same pair lands there unchanged — the wave's own TXT VALUE / TXT EMPTY are not borrowed, because
// the guarantee that the text is legible belongs to the viz pair. The mascot is one bit deep for
// exactly this reason — see ui/mascot_sprite.h.
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

    // ⚠️ HELP_MAX_CHARS is written down in ui/help_text.h and checked against the table there. This is
    // the geometry it was derived FROM, pinned so that moving the mascot cannot silently make every
    // help line one character too long.
    static_assert(TEXT_X + HELP_MAX_CHARS * CHAR_W - CHAR_SPACING <= WIDTH,
                  "HELP_MAX_CHARS no longer fits beside the mascot");
    static_assert(TEXT_X + (HELP_MAX_CHARS + 1) * CHAR_W - CHAR_SPACING > WIDTH,
                  "HELP_MAX_CHARS is short by a character or more - widen it");

    /**
     * Draw the panel filling `box_height` pixels down from `y`, WIDTH across.
     *
     * ⚠️ The three lines and the mascot are a fixed 70px block: a taller box is filled with VIZ BG
     * and the block is CENTRED in it, rather than the text growing or spreading out. That keeps the
     * strip and the waveform showing the identical panel, which is why one table serves both.
     */
    void draw(Canvas& c, int x, int y, HelpTopic topic, const Theme& t,
              int box_height = HEIGHT) const;
};

}  // namespace pt::ui
