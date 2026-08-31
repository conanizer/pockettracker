#include "ui/modules/help_panel.h"

#include "ui/helpers.h"
#include "ui/mascot_sprite.h"

namespace pt::ui {

namespace {

/**
 * The sprite, as horizontal RUNS of lit pixels.
 *
 * ⚠️ A run rather than a pixel, and it is not micro-optimisation: `fill_rect` clips and blends per
 * CALL, so one call per lit pixel would be 1831 of them a frame where 313 will do. The bitmask is
 * walked left to right and a run is closed at the first unlit column or at the right edge.
 */
void draw_mascot(Canvas& c, int x, int y, Argb color) {
    for (int row = 0; row < MASCOT_H; ++row) {
        int runStart = -1;
        // ⚠️ MASCOT_W inclusive, so the last column closes a run that reaches the right edge.
        for (int col = 0; col <= MASCOT_W; ++col) {
            const bool lit = (col < MASCOT_W) && mascot_pixel(MASCOT_NEUTRAL, col, row);
            if (lit && runStart < 0) {
                runStart = col;
            } else if (!lit && runStart >= 0) {
                c.fill_rect(x + runStart, y + row, col - runStart, 1, color);
                runStart = -1;
            }
        }
    }
}

}  // namespace

void HelpPanelModule::draw(Canvas& c, int x, int y, HelpTopic topic, const Theme& t) const {
    c.fill_rect(x, y, WIDTH, HEIGHT, t.vizBackground);

    draw_mascot(c, x + MASCOT_MARGIN, y + MASCOT_MARGIN, t.vizWave);

    const HelpEntry& e     = help_entry(topic);
    const char*      lines[3] = {e.line1, e.line2, e.line3};
    for (int i = 0; i < 3; ++i) {
        if (lines[i][0] == '\0') continue;   // a two-line entry simply leaves the third row empty
        c.draw_text(lines[i], x + TEXT_X, y + TEXT_TOP + i * ROW_HEIGHT, t.vizWave, CHAR_SPACING,
                    FONT_SCALE);
    }
}

}  // namespace pt::ui
