#include "ui/modules/help_panel.h"

#include "ui/helpers.h"
#include "ui/mascot_sprite.h"

namespace pt::ui {

namespace {

// The sprite as horizontal runs: one `fill_rect` per pixel would be ~1460 calls a frame.
void draw_mascot(Canvas& c, int x, int y, const MascotInk& ink) {
    // ⚠️ One fill for the patch, before the runs — per row would leave a seam.
    if (ink.inverted) c.fill_rect(x, y, MASCOT_W, MASCOT_H, ink.backdrop);
    const Argb color = ink.figure;

    for (int row = 0; row < MASCOT_H; ++row) {
        int runStart = -1;
        // ⚠️ Inclusive, so a run reaching the right edge closes.
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

void HelpPanelModule::draw(Canvas& c, int x, int y, HelpTopic topic, const Theme& t,
                           int box_height) const {
    c.fill_rect(x, y, WIDTH, box_height, t.vizBackground);

    // A taller box gets the surplus as air; clamped so a shorter one cannot push the mascot off.
    const int top = y + (box_height > HEIGHT ? (box_height - HEIGHT) / 2 : 0);

    draw_mascot(c, x + MASCOT_MARGIN, top + MASCOT_MARGIN, mascot_ink(t, t.vizBackground));

    const HelpEntry& e     = help_entry(topic);
    const char*      lines[3] = {e.line1, e.line2, e.line3};
    for (int i = 0; i < 3; ++i) {
        if (lines[i][0] == '\0') continue;
        c.draw_text(lines[i], x + TEXT_X, top + TEXT_TOP + i * ROW_HEIGHT, t.vizWave, CHAR_SPACING,
                    FONT_SCALE);
    }
}

}  // namespace pt::ui
