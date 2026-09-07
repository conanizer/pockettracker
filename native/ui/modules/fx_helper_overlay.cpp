#include "ui/modules/fx_helper_overlay.h"

#include "ui/helpers.h"

namespace pt::ui {

namespace {

// The geometry, verbatim from drawFxHelper. Kotlin recomputes these each frame from DESIGN_WIDTH_PX;
// they are constants here because the canvas IS the design (canvas.h) and cannot be another size.
constexpr int BOX_W = 580;
// 4 description rows + 8 + header + 8 + the group stack, with 8 px of air above and below.
// ⚠️ DERIVED from the row count, which is a function of how many groups this build shows and how tall
// the biggest one is (fx_helper.h): a hard-coded height under a stack that can be a row shorter or
// taller is a picker whose last row draws outside its own box, or a box with a band of dead space.
constexpr int CONTENT_PAD = 8;
constexpr int content_h(int gridRows) {
    return 4 * ROW_HEIGHT + 8 + ROW_HEIGHT + 8 + gridRows * ROW_HEIGHT;
}
constexpr int box_h(int gridRows) { return content_h(gridRows) + 2 * CONTENT_PAD; }
constexpr int BOX_X   = (DESIGN_W - BOX_W) / 2;
constexpr int INNER_X = BOX_X + 10;
constexpr int CELL_W  = 80;

// The one place the layout's height meets the screen it has to fit on. Derived rather than a copied
// row count: the same bound written as a number in fx_helper.h read `rows <= 6` long after the box
// had room for more than twice that, which turns "add an effect" into a redesign it never was.
static_assert(box_h(FX_MAX_LAYOUT_ROWS) + 2 * (MODAL_BORDER - 1) <= DESIGN_H,
              "the FX helper is taller than the screen — its last row, or the border "
              "around it, would draw outside the canvas");

/**
 * ⚠️ The advance of an N-character run, Kotlin's way: `length * charW`, INCLUDING the trailing
 * inter-character gap. `Canvas::text_width` subtracts that gap (it measures ink, which is what a
 * right-aligned cell wants) and would centre these two runs 1 px left of where Android puts them.
 * The centring below is the one place the difference is visible, so it is spelled out rather than
 * borrowed.
 */
constexpr int run_advance(int chars) { return chars * CHAR_W; }

}  // namespace

void draw_fx_helper(Canvas& c, const FxHelperState& s, const Theme& t) {
    if (!s.isOpen || s.layout.count() == 0) return;

    // The overlay is modal: it must not be clipped by whatever editor was drawing when it opened.
    c.reset_clip();

    // ⚠️ THE BOX'S HEIGHT FOLLOWS THE OPEN GROUP, ITS TOP EDGE DOES NOT. Two different row counts on
    // purpose: the height is what is actually drawn, so a one-row group leaves no band of dead space;
    // the top is where the TALLEST group would put it, so the description, the header and every
    // heading stay exactly where they are as the cursor opens one group after another. Centre the
    // box on its own height instead and the text a reader is holding A to read slides up and down
    // under them.
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
        if (i >= static_cast<int>(desc.size())) break;  // Kotlin's `?: break` — a short entry stops
        c.draw_text(desc[static_cast<size_t>(i)], INNER_X, textY + TEXT_PADDING, t.textValue,
                    CHAR_SPACING, FONT_SCALE);
        textY += ROW_HEIGHT;
    }

    // ── "EFFECT", centred ────────────────────────────────────────────────────────────────────────
    // The header sits at a FIXED offset — four description rows down — not below however many lines
    // this particular effect happens to have. So the grid never moves as the cursor walks the grid.
    const int headerY = CONTENT_Y + 4 * ROW_HEIGHT + 8;
    const int headerX = BOX_X + (BOX_W - run_advance(6)) / 2;
    c.draw_text("EFFECT", headerX, headerY + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);

    // ── The groups — a heading each, and six columns of cells under the open one ─────────────────
    //
    // The stack is drawn top-down and every group's heading lands wherever the one above it left off,
    // so nothing here needs to know which group is open beyond asking. The box is sized for the
    // TALLEST group (fx_helper.h), so a shorter one leaves air at the bottom rather than resizing the
    // modal under the reader.
    const int gridY = headerY + ROW_HEIGHT + 8;
    const int gridX = BOX_X + (BOX_W - FX_GRID_COLS * CELL_W) / 2;

    int row = 0;
    for (int gi = 0; gi < s.layout.count(); ++gi) {
        const FxGroup& g    = s.layout.groups[static_cast<size_t>(gi)];
        const bool     open = (gi == s.group);

        // ⚠️ BOTH ARROWS ARE RAW UTF-8 in the source, exactly as the help text's are: canvas.h decodes
        // UTF-8 and font5x5.h carries all four arrow glyphs. A Unicode ESCAPE would not work here —
        // this tree is compiled with no /utf-8 flag and no BOM, so MSVC would convert the escape into
        // the system codepage. The raw bytes pass through untouched. The collapsed heading uses the
        // font's own right arrow rather than an ASCII `>`, so the two states are one pair of glyphs.
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

            const Argb color = isCursor                     ? t.textCursor
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
