#include "ui/modules/fx_helper_overlay.h"

#include "ui/helpers.h"

namespace pt::ui {

namespace {

// Fixed geometry: the canvas is always the design size (canvas.h).
constexpr int BOX_W = 580;
// 4 description rows + 8 + header + 8 + the group stack, with 8 px of air above and below.
// ⚠️ DERIVED from the row count, which depends on the groups this build shows (fx_helper.h); a
// hard-coded height would draw the last row outside the box or leave a band of dead space.
constexpr int CONTENT_PAD = 8;
constexpr int content_h(int gridRows) {
    return 4 * ROW_HEIGHT + 8 + ROW_HEIGHT + 8 + gridRows * ROW_HEIGHT;
}
constexpr int box_h(int gridRows) { return content_h(gridRows) + 2 * CONTENT_PAD; }
constexpr int BOX_X   = (DESIGN_W - BOX_W) / 2;
constexpr int INNER_X = BOX_X + 10;
constexpr int CELL_W  = 80;

// The one place the layout's height meets the screen it must fit on — derived, not a copied count.
static_assert(box_h(FX_MAX_LAYOUT_ROWS) + 2 * (MODAL_BORDER - 1) <= DESIGN_H,
              "the FX helper is taller than the screen — its last row, or the border "
              "around it, would draw outside the canvas");

/**
 * ⚠️ The advance of an N-character run INCLUDING the trailing gap. `Canvas::text_width` drops that
 * gap (it measures ink) and would centre these runs 1 px left.
 */
constexpr int run_advance(int chars) { return chars * CHAR_W; }

}  // namespace

void draw_fx_helper(Canvas& c, const FxHelperState& s, const Theme& t) {
    if (!s.isOpen || s.layout.count() == 0) return;

    // The overlay is modal: it must not be clipped by whatever editor was drawing when it opened.
    c.reset_clip();

    // ⚠️ The box's HEIGHT follows the open group, its TOP does not: the top is where the tallest group
    // would put it, so the text stays still as the cursor opens one group after another.
    const int stackRows = s.layout.count() + s.open_group()->rows();
    const int BOX_H     = box_h(stackRows);
    const int BOX_Y     = (DESIGN_H - box_h(s.layout.total_rows())) / 2;

    draw_modal_backdrop(c);
    draw_modal_box(c, BOX_X, BOX_Y, BOX_W, BOX_H, t);

    const int CONTENT_Y = BOX_Y + CONTENT_PAD;

    // ── The effect's documentation: up to four lines ──────────────────────────────────────────────
    const std::vector<std::string>& desc = fx_description_lines(s);
    int textY = CONTENT_Y;
    for (int i = 0; i < 4; ++i) {
        if (i >= static_cast<int>(desc.size())) break;  // a short entry stops
        c.draw_text(desc[static_cast<size_t>(i)], INNER_X, textY + TEXT_PADDING, t.textValue,
                    CHAR_SPACING, FONT_SCALE);
        textY += ROW_HEIGHT;
    }

    // ── "EFFECT", centred ────────────────────────────────────────────────────────────────────────
    // At a fixed offset (four description rows down), so the grid never moves between effects.
    const int headerY = CONTENT_Y + 4 * ROW_HEIGHT + 8;
    const int headerX = BOX_X + (BOX_W - run_advance(6)) / 2;
    c.draw_text("EFFECT", headerX, headerY + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);

    // ── The groups — a heading each, and six columns of cells under the open one ─────────────────
    // Each heading lands where the group above left off. The box is sized for the tallest group
    // (fx_helper.h), so a shorter one leaves air at the bottom.
    const int gridY = headerY + ROW_HEIGHT + 8;
    const int gridX = BOX_X + (BOX_W - FX_GRID_COLS * CELL_W) / 2;

    int row = 0;
    for (int gi = 0; gi < s.layout.count(); ++gi) {
        const FxGroup& g    = s.layout.groups[static_cast<size_t>(gi)];
        const bool     open = (gi == s.group);

        // ⚠️ The arrows are raw UTF-8 in the source (canvas.h decodes it, font5x5.h has the glyphs).
        // A Unicode escape would not survive MSVC without /utf-8: it converts to the system codepage.
        c.draw_text(std::string(g.title) + (open ? " ↓" : " →"), gridX,
                    gridY + row * ROW_HEIGHT + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);
        ++row;
        if (!open) continue;

        for (int i = 0; i < g.size(); ++i) {
            const int cellRow = i / FX_GRID_COLS;
            const int cellCol = i % FX_GRID_COLS;
            const int cellX   = gridX + cellCol * CELL_W;
            const int cellY   = gridY + (row + cellRow) * ROW_HEIGHT;

            const bool isCursor = (s.cursorRow == cellRow && s.cursorCol == cellCol);
            const int  code     = g.codes[static_cast<size_t>(i)];

            if (isCursor) c.fill_rect(cellX, cellY, CELL_W, ROW_HEIGHT, t.rowCursor);

            const Argb color = isCursor                     ? cursor_cell_ink(t)
                               : (code == songcore::FX_NONE) ? t.textEmpty
                                                             : t.textValue;
            const int nameX = cellX + (CELL_W - run_advance(3)) / 2;
            c.draw_text(songcore::effect_name(code), nameX, cellY + TEXT_PADDING, color,
                        CHAR_SPACING, FONT_SCALE);
        }
        row += g.rows();
    }
}

}  // namespace pt::ui
