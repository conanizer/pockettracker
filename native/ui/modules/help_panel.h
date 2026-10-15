#pragma once

// The compact help panel: three lines about the cell under the cursor, drawn in place of the
// visualizer strip (or the sample editor's waveform), mascot at the left.
// ⚠️ It replaces its box: the status message and selection readout stand down while it is up.
// Both boxes are 620 wide, so `HELP_MAX_CHARS` derives from the width alone; the taller one is air.
// Colours: VIZ BG, VIZ WAVE text, TXT TITLE mascot — readable wherever the scope is.
// ⚠️ The file browser cannot show it: no box to spare, and SELECT is its own chord there.

#include "ui/canvas.h"
#include "ui/help_text.h"
#include "ui/helpers.h"
#include "ui/mascot_sprite.h"
#include "ui/theme.h"

namespace pt::ui {

class HelpPanelModule {
  public:
    static constexpr int WIDTH  = 620;
    // The smallest box the panel fills.
    static constexpr int HEIGHT = 70;

    // 64 + 3 + 3 = 70: snug top and bottom.
    static constexpr int MASCOT_MARGIN = 3;
    static constexpr int MASCOT_GUTTER = 7;

    // Three 21px rows in 70, the odd pixel on top.
    static constexpr int TEXT_TOP = 7;

    // From the panel's left edge.
    static constexpr int TEXT_X = MASCOT_MARGIN + MASCOT_W + MASCOT_GUTTER;

    // ⚠️ HELP_MAX_CHARS lives in help_text.h; this pins it to the geometry.
    static_assert(TEXT_X + HELP_MAX_CHARS * CHAR_W - CHAR_SPACING <= WIDTH,
                  "HELP_MAX_CHARS no longer fits beside the mascot");
    static_assert(TEXT_X + (HELP_MAX_CHARS + 1) * CHAR_W - CHAR_SPACING > WIDTH,
                  "HELP_MAX_CHARS is short by a character or more - widen it");

    // Fills `box_height` from `y`. The 70px block is centred, so both boxes show the same panel.
    void draw(Canvas& c, int x, int y, HelpTopic topic, const Theme& t,
              int box_height = HEIGHT) const;
};

}  // namespace pt::ui
