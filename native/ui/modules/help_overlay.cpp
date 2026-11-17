#include "ui/modules/help_overlay.h"

#include "ui/modules/help_panel.h"

#include <string>

namespace pt::ui {

using namespace help_overlay_geometry;

namespace {

// The large sprite as horizontal runs (see help_panel.cpp).
void draw_large_mascot(Canvas& c, int x, int y, const MascotInk& ink) {
    // One fill for the patch, before the runs.
    if (ink.inverted) c.fill_rect(x, y, MASCOT_LARGE_W, MASCOT_LARGE_H, ink.backdrop);
    const Argb color = ink.figure;

    for (int row = 0; row < MASCOT_LARGE_H; ++row) {
        int runStart = -1;
        // ⚠️ Inclusive, so a run reaching the right edge closes.
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

// One prose word at (x, y), its stars dropped: the stretches inside a highlight in `markInk`.
// `marked` is whether the word starts inside one.
void draw_prose_word(Canvas& c, const char* word, int bytes, int x, int y, bool marked, Argb ink,
                     Argb markInk) {
    int from = 0;
    for (int i = 0; i <= bytes; ++i) {
        if (i < bytes && word[i] != detail::HELP_MARK) continue;
        if (i > from)
            c.draw_text(std::string(word + from, static_cast<size_t>(i - from)),
                        x + detail::help_columns(word, 0, from) * CHAR_W, y, marked ? markInk : ink,
                        CHAR_SPACING, FONT_SCALE);
        marked = !marked;
        from   = i + 1;
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

    // ⚠️ Invert against `meterBackground`, the ground this modal stands on.
    draw_large_mascot(c, MASCOT_X, boxY + MASCOT_DY, mascot_ink(t, t.meterBackground));

    const std::string title(e.line1, static_cast<size_t>(help_title_length(e.line1)));
    c.draw_text(title, row_x(0), boxY + row_dy(0) + TEXT_PADDING, t.textTitle, CHAR_SPACING,
                FONT_SCALE);

    help_overlay_flow(e, [&](int row, int col, const char* word, int bytes, bool isKey, bool marked) {
        const int wx = row_x(row) + col * CHAR_W;
        const int wy = boxY + row_dy(row) + TEXT_PADDING;
        if (isKey) draw_control_word(c, word, bytes, wx, wy, t.textValue, t.vizWave);
        else draw_prose_word(c, word, bytes, wx, wy, marked, t.textValue, t.vizWave);
    });

    constexpr const char* HINT = "ANY BUTTON CLOSES";
    const int hintW = static_cast<int>(std::char_traits<char>::length(HINT)) * CHAR_W - CHAR_SPACING;
    c.draw_text(HINT, BOX_X + (BOX_W - hintW) / 2, boxY + hint_dy(boxH) + TEXT_PADDING, t.textEmpty,
                CHAR_SPACING, FONT_SCALE);
}

}  // namespace pt::ui
