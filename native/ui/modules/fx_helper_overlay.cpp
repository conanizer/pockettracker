#include "ui/modules/fx_helper_overlay.h"

#include "ui/helpers.h"

namespace pt::ui {

namespace {

// The geometry, verbatim from drawFxHelper. Kotlin recomputes these each frame from DESIGN_WIDTH_PX;
// they are constants here because the canvas IS the design (canvas.h) and cannot be another size.
constexpr int BOX_W = 580;
constexpr int BOX_H = 243;  // 4 description rows + header + 5 grid rows
constexpr int BOX_X = (DESIGN_W - BOX_W) / 2;   // 30
constexpr int BOX_Y = (DESIGN_H - BOX_H) / 2;   // 118
constexpr int INNER_X = BOX_X + 10;             // 40
constexpr int CELL_W  = 80;

constexpr Argb BACKDROP = 0xCC000000;  // translucent — the canvas blends it (canvas.cpp)

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
    if (!s.isOpen) return;

    // The overlay is modal: it must not be clipped by whatever editor was drawing when it opened.
    c.reset_clip();

    c.fill_rect(0, 0, DESIGN_W, DESIGN_H, BACKDROP);
    c.fill_rect(BOX_X, BOX_Y, BOX_W, BOX_H, t.meterBackground);
    c.stroke_rect(BOX_X, BOX_Y, BOX_W, BOX_H, t.textTitle);

    // ── The effect's documentation: up to four lines ──────────────────────────────────────────────
    const std::vector<std::string> desc = fx_description_lines(s);
    int textY = BOX_Y + 8;
    for (int i = 0; i < 4; ++i) {
        if (i >= static_cast<int>(desc.size())) break;  // Kotlin's `?: break` — a short entry stops
        c.draw_text(desc[static_cast<size_t>(i)], INNER_X, textY + TEXT_PADDING, t.textValue,
                    CHAR_SPACING, FONT_SCALE);
        textY += ROW_HEIGHT;
    }

    // ── "EFFECT", centred ────────────────────────────────────────────────────────────────────────
    // The header sits at a FIXED offset — four description rows down — not below however many lines
    // this particular effect happens to have. So the grid never moves as the cursor walks the grid.
    const int headerY = BOX_Y + 8 + 4 * ROW_HEIGHT + 8;
    const int headerX = BOX_X + (BOX_W - run_advance(6)) / 2;
    c.draw_text("EFFECT", headerX, headerY + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);

    // ── The 6×5 grid ─────────────────────────────────────────────────────────────────────────────
    const int gridY = headerY + ROW_HEIGHT + 8;
    const int gridX = BOX_X + (BOX_W - FX_GRID_COLS * CELL_W) / 2;

    for (int i = 0; i < songcore::EFFECT_TYPE_COUNT; ++i) {
        const FxCell cell  = fx_index_to_cell(i);  // the last row centres itself
        const int    cellX = gridX + cell.col * CELL_W;
        const int    cellY = gridY + cell.row * ROW_HEIGHT;

        const bool isCursor = (s.cursorRow == cell.row && s.cursorCol == cell.col);
        const int  code     = songcore::EFFECT_TYPES[i];

        if (isCursor) {
            c.fill_rect(cellX, cellY, CELL_W, ROW_HEIGHT, darken(t.textCursor, 0.27f));
            c.stroke_rect(cellX, cellY, CELL_W, ROW_HEIGHT, t.textCursor);
        }

        const Argb color = isCursor                ? t.textCursor
                           : (code == songcore::FX_NONE) ? t.textEmpty
                                                         : t.textValue;
        const int nameX = cellX + (CELL_W - run_advance(3)) / 2;
        c.draw_text(songcore::effect_name(code), nameX, cellY + TEXT_PADDING, color, CHAR_SPACING,
                    FONT_SCALE);
    }
}

}  // namespace pt::ui
