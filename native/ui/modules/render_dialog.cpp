#include "ui/modules/render_dialog.h"

#include <string>

#include "songcore/render.h"
#include "ui/helpers.h"

namespace pt::ui {

namespace {

constexpr int ROWS        = static_cast<int>(RenderRow::COUNT);
constexpr int CONTENT_PAD = 8;

// The title, a gap, then the four rows.
constexpr int TITLE_GAP = 8;
// ⚠️ The width is set by the percentage readout, not the longest label — see the static_assert below.
constexpr int BOX_W     = 420;
constexpr int BOX_H     = 2 * CONTENT_PAD + ROW_HEIGHT + TITLE_GAP + ROWS * ROW_HEIGHT;
constexpr int BOX_X     = (DESIGN_W - BOX_W) / 2;
constexpr int BOX_Y     = (DESIGN_H - BOX_H) / 2;

constexpr int PAD_X   = 14;
constexpr int LABEL_X = BOX_X + PAD_X;
// "SONG START" is the longest label, plus one space; derived so a longer label moves the values.
constexpr int VALUE_X = LABEL_X + 11 * CHAR_W;

// The percentage sits one space past the widest value on the panel ("RENDER" itself, 6 characters).
constexpr int PROGRESS_X = VALUE_X + 7 * CHAR_W;

static_assert(BOX_H + 2 * (MODAL_BORDER - 1) <= DESIGN_H, "the render dialog is taller than the screen");

// ⚠️ The readout appears only while a render runs, so an overflow would go unseen. "100%" is widest.
static_assert(PROGRESS_X - BOX_X + 4 * CHAR_W + PAD_X <= BOX_W,
              "the render percentage draws past the panel's right edge");

int row_y(int row) { return BOX_Y + CONTENT_PAD + ROW_HEIGHT + TITLE_GAP + row * ROW_HEIGHT; }

}  // namespace

int render_dialog_end_row(const RenderDialogState& s, const songcore::Project& project) {
    if (s.endRow >= 0) return s.endRow < s.startRow ? s.startRow : s.endRow;
    return songcore::song_section_end(project, s.startRow);
}

void draw_render_dialog(Canvas& c, const RenderDialogState& s, const songcore::Project& project,
                        bool isRendering, float progress, const Theme& t) {
    if (!s.isOpen) return;

    // Modal: it must not be clipped by whatever screen was drawing when it opened.
    c.reset_clip();
    draw_modal_backdrop(c);
    draw_modal_box(c, BOX_X, BOX_Y, BOX_W, BOX_H, t);

    const std::string title = (s.output == RenderDialogState::Output::STEMS) ? "RENDER STEMS"
                                                                             : "RENDER MIX";
    const int titleW = Canvas::text_width(title, CHAR_SPACING, FONT_SCALE);
    c.draw_text(title, BOX_X + (BOX_W - titleW) / 2, BOX_Y + CONTENT_PAD + TEXT_PADDING, t.textTitle,
                CHAR_SPACING, FONT_SCALE);

    const auto label = [&](RenderRow row, const char* text) {
        c.draw_text(text, LABEL_X, row_y(static_cast<int>(row)) + TEXT_PADDING,
                    s.is_on(row) ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
    };
    const auto value = [&](RenderRow row, const std::string& text) {
        draw_cursor_cell(c, text, VALUE_X, row_y(static_cast<int>(row)) + TEXT_PADDING, s.is_on(row),
                         t.textValue, t);
    };

    label(RenderRow::SONG_START, "SONG START");
    value(RenderRow::SONG_START, hex2(s.startRow));

    label(RenderRow::SONG_END, "SONG END");
    value(RenderRow::SONG_END, s.endRow >= 0 ? hex2(s.endRow) : "AUTO");

    // ⚠️ AUTO shows the row it resolves to, beside itself and dimmed.
    if (s.endRow < 0) {
        c.draw_text(hex2(render_dialog_end_row(s, project)), VALUE_X + 5 * CHAR_W,
                    row_y(static_cast<int>(RenderRow::SONG_END)) + TEXT_PADDING, t.textEmpty,
                    CHAR_SPACING, FONT_SCALE);
    }

    label(RenderRow::REPEAT, "REPEAT");
    value(RenderRow::REPEAT, s.repeat > 1 ? ("x" + std::to_string(s.repeat)) : std::string("OFF"));

    // The RENDER row is a button, and carries the percentage while rendering — the only place the
    // progress is visible, since the panel covers PROJECT's readout.
    value(RenderRow::RENDER, "RENDER");
    if (isRendering) {
        const int pct = static_cast<int>(progress * 100.0f + 0.5f);
        c.draw_text(std::to_string(pct < 0 ? 0 : (pct > 100 ? 100 : pct)) + "%", PROGRESS_X,
                    row_y(static_cast<int>(RenderRow::RENDER)) + TEXT_PADDING, t.textValue,
                    CHAR_SPACING, FONT_SCALE);
    }
}

}  // namespace pt::ui
