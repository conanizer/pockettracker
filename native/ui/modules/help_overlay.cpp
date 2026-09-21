#include "ui/modules/help_overlay.h"

#include <string>

namespace pt::ui {

using namespace help_overlay_geometry;

namespace {

/**
 * The large sprite as horizontal RUNS of lit pixels — one `fill_rect` per run, not per pixel, for the
 * compact panel's reason (`help_panel.cpp`): the canvas clips and blends per call.
 */
void draw_large_mascot(Canvas& c, int x, int y, const MascotInk& ink) {
    // The compact panel's rule (`help_panel.cpp`): one fill for the patch, before the runs.
    if (ink.inverted) c.fill_rect(x, y, MASCOT_LARGE_W, MASCOT_LARGE_H, ink.backdrop);
    const Argb color = ink.figure;

    for (int row = 0; row < MASCOT_LARGE_H; ++row) {
        int runStart = -1;
        // ⚠️ MASCOT_LARGE_W inclusive, so the last column closes a run that reaches the right edge.
        for (int col = 0; col <= MASCOT_LARGE_W; ++col) {
            const bool lit = (col < MASCOT_LARGE_W) && mascot_large_pixel(col, row);
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

void HelpOverlayModule::draw(Canvas& c, HelpTopic topic, const Theme& t) const {
    const HelpEntry& e = help_entry(topic);

    const int boxH = box_h(help_overlay_flow(e, detail::HelpFlowNoEmit{}));
    const int boxY = box_y(boxH);

    c.reset_clip();
    draw_modal_backdrop(c);
    draw_modal_box(c, BOX_X, boxY, BOX_W, boxH, t);

    // ⚠️ The modal's ground is `meterBackground`, not `background` — that is the panel this one
    // stands on, and the one it must invert against.
    draw_large_mascot(c, MASCOT_X, boxY + MASCOT_DY, mascot_ink(t, t.meterBackground));

    const std::string title(e.line1, static_cast<size_t>(help_title_length(e.line1)));
    c.draw_text(title, row_x(0), boxY + row_dy(0) + TEXT_PADDING, t.textTitle, CHAR_SPACING,
                FONT_SCALE);

    help_overlay_flow(e, [&](int row, int col, const char* word, int bytes, bool isKey) {
        c.draw_text(std::string(word, static_cast<size_t>(bytes)), row_x(row) + col * CHAR_W,
                    boxY + row_dy(row) + TEXT_PADDING, isKey ? t.textEmpty : t.textValue,
                    CHAR_SPACING, FONT_SCALE);
    });

    constexpr const char* HINT = "ANY BUTTON CLOSES";
    const int hintW = static_cast<int>(std::char_traits<char>::length(HINT)) * CHAR_W - CHAR_SPACING;
    c.draw_text(HINT, BOX_X + (BOX_W - hintW) / 2, boxY + hint_dy(boxH) + TEXT_PADDING, t.textEmpty,
                CHAR_SPACING, FONT_SCALE);
}

}  // namespace pt::ui
