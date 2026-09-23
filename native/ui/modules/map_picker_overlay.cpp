#include "ui/modules/map_picker_overlay.h"

#include <string>

#include "ui/helpers.h"

namespace pt::ui {

namespace {

// Nine glyphs is the widest name (`TRACK VOL`), which is 153 px at CHAR_W; the rest is the column.
constexpr int CELL_W = 180;
constexpr int GRID_W = MAP_PICKER_COLS * CELL_W;
constexpr int BOX_W  = GRID_W + 40;
constexpr int BOX_X  = (DESIGN_W - BOX_W) / 2;

constexpr int CONTENT_PAD = 8;

// The header, air, then the stack: a heading per section plus the open one's cells.
constexpr int content_h(int stackRows) { return ROW_HEIGHT + 8 + stackRows * ROW_HEIGHT; }
constexpr int box_h(int stackRows) { return content_h(stackRows) + 2 * CONTENT_PAD; }

// ⚠️ The catalogue decides the height, so this is a CEILING on the tallest section rather than a
// copy of it — a number copied from the data is the mistake the FX picker's own note describes.
constexpr int MAX_STACK_ROWS = MAP_SECTION_COUNT + 16;
static_assert(box_h(MAX_STACK_ROWS) + 2 * (MODAL_BORDER - 1) <= DESIGN_H,
              "the destination picker is taller than the screen");

/** Kotlin's run advance — `length * charW`, trailing gap included. See the FX overlay's note. */
constexpr int run_advance(int chars) { return chars * CHAR_W; }

void centred(Canvas& c, const std::string& text, int x, int width, int y, Argb color) {
    c.draw_text(text, x + (width - run_advance(static_cast<int>(text.size()))) / 2, y + TEXT_PADDING,
                color, CHAR_SPACING, FONT_SCALE);
}

// ⚠️ The CELLS are left-aligned where the FX picker's are centred, and the difference is the content:
// its cells are all exactly three characters, so centring them IS a column. These run from three
// glyphs to nine, and centring made a list that reads as a ragged cloud rather than three columns.
constexpr int CELL_PAD = 6;

}  // namespace

void draw_map_picker(Canvas& c, const MapPickerState& s, const Theme& t) {
    if (!s.isOpen) return;

    // Modal: it must not be clipped by whatever editor was drawing when it opened.
    c.reset_clip();

    // ⚠️ THE BOX'S HEIGHT FOLLOWS THE OPEN SECTION, ITS TOP EDGE DOES NOT — the FX picker's rule and
    // its reason: measured on its own height, the box would slide up and down under the reader every
    // time the cursor crossed a heading.
    const int stackRows = MAP_SECTION_COUNT + map_section_rows(s.map_section());
    const int BOX_H     = box_h(stackRows);
    const int BOX_Y     = (DESIGN_H - box_h(MAP_SECTION_COUNT + map_picker_max_rows())) / 2;

    draw_modal_backdrop(c);
    draw_modal_box(c, BOX_X, BOX_Y, BOX_W, BOX_H, t);

    const int contentY = BOX_Y + CONTENT_PAD;
    centred(c, "DESTINATION", BOX_X, BOX_W, contentY, t.textTitle);

    const int gridY = contentY + ROW_HEIGHT + 8;
    const int gridX = BOX_X + (BOX_W - GRID_W) / 2;

    int row = 0;
    for (int si = 0; si < MAP_SECTION_COUNT; ++si) {
        const auto section = static_cast<MapSection>(si);
        const bool open    = (si == s.section);

        // ⚠️ BOTH ARROWS ARE RAW UTF-8, exactly as the FX picker's are: canvas.h decodes UTF-8 and
        // font5x5.h carries the glyphs, while a Unicode ESCAPE would be converted to the system
        // codepage by MSVC (this tree is compiled with no /utf-8 flag and no BOM).
        c.draw_text(std::string(map_section_name(section)) + (open ? " ↓" : " →"), gridX,
                    gridY + row * ROW_HEIGHT + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);
        ++row;
        if (!open) continue;

        const std::vector<MapPickerRow>& rows = map_section_layout(section);
        for (int cellRow = 0; cellRow < static_cast<int>(rows.size()); ++cellRow) {
            const MapPickerRow& cells = rows[static_cast<size_t>(cellRow)];
            for (int cellCol = 0; cellCol < static_cast<int>(cells.size()); ++cellCol) {
                const int  cellX    = gridX + cellCol * CELL_W;
                const int  cellY    = gridY + (row + cellRow) * ROW_HEIGHT;
                const bool isCursor = (s.cursorRow == cellRow && s.cursorCol == cellCol);

                if (isCursor) c.fill_rect(cellX, cellY, CELL_W, ROW_HEIGHT, t.rowCursor);
                c.draw_text(cells[static_cast<size_t>(cellCol)]->name, cellX + CELL_PAD,
                            cellY + TEXT_PADDING, isCursor ? cursor_cell_ink(t) : t.textValue,
                            CHAR_SPACING, FONT_SCALE);
            }
        }
        row += map_section_rows(section);
    }
}

}  // namespace pt::ui
