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

// ⚠️ The catalogue decides the height, so this is a CEILING on the tallest section, not a copy of it.
constexpr int MAX_STACK_ROWS = MAP_SECTION_COUNT + 16;
static_assert(box_h(MAX_STACK_ROWS) + 2 * (MODAL_BORDER - 1) <= DESIGN_H,
              "the destination picker is taller than the screen");

/** The run advance: `length * CHAR_W`, trailing gap included. */
constexpr int run_advance(int chars) { return chars * CHAR_W; }

void centred(Canvas& c, const std::string& text, int x, int width, int y, Argb color) {
    c.draw_text(text, x + (width - run_advance(static_cast<int>(text.size()))) / 2, y + TEXT_PADDING,
                color, CHAR_SPACING, FONT_SCALE);
}

// The cells are left-aligned (the FX picker centres its three-character cells): these run from three
// glyphs to nine, and centred they read as a ragged cloud.
constexpr int CELL_PAD = 6;

}  // namespace

void draw_map_picker(Canvas& c, const MapPickerState& s, const Theme& t) {
    if (!s.isOpen) return;

    // Modal: it must not be clipped by whatever editor was drawing when it opened.
    c.reset_clip();

    // ⚠️ The height follows the open section, the top edge does not — or the box slides under the
    // reader at every heading.
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

        // ⚠️ Raw UTF-8 arrows: a Unicode escape would be converted to the codepage by MSVC (no /utf-8).
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
