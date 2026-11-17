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

void HelpPanelModule::draw(Canvas& c, int x, int y, const GuideLines& lines, const Theme& t) const {
    c.fill_rect(x, y, WIDTH, HEIGHT, t.vizBackground);
    draw_mascot(c, x + MASCOT_MARGIN, y + MASCOT_MARGIN, mascot_ink(t, t.vizBackground));

    for (int i = 0; i < 3; ++i) {
        const GuideLines::Line& line = lines.line[i];
        if (line.text.empty()) continue;
        const int textY = y + TEXT_TOP + i * ROW_HEIGHT;
        if (line.keyInk == 0) {
            c.draw_text(line.text, x + TEXT_X, textY, line.ink, CHAR_SPACING, FONT_SCALE);
            continue;
        }
        // Word by word, so the buttons stand out from what they do.
        const std::string& s = line.text;
        int col = 0;
        for (size_t pos = 0; pos < s.size();) {
            if (s[pos] == ' ') { ++pos; ++col; continue; }
            size_t end = pos;
            while (end < s.size() && s[end] != ' ') ++end;
            col += draw_control_word(c, s.data() + pos, static_cast<int>(end - pos), x + TEXT_X + col * CHAR_W,
                                     textY, line.ink, line.keyInk);
            pos = end;
        }
    }
}

}  // namespace pt::ui
