#pragma once

// The panel the guide is drawn in (ui/guide.h): three lines in place of the visualizer, mascot at the
// left. `HELP_MAX_CHARS` derives from its width.
// Colours: VIZ BG under the lines' own inks, TXT TITLE mascot — readable wherever the scope is.

#include "ui/canvas.h"
#include "ui/guide.h"
#include "ui/help_text.h"
#include "ui/helpers.h"
#include "ui/mascot_sprite.h"
#include "ui/theme.h"

namespace pt::ui {

class HelpPanelModule {
  public:
    static constexpr int WIDTH  = 620;
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

    void draw(Canvas& c, int x, int y, const GuideLines& lines, const Theme& t) const;
};

}  // namespace pt::ui
