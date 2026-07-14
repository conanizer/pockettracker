#include "ui/modules/theme_editor.h"

#include <algorithm>

#include "ui/helpers.h"

namespace pt::ui {

namespace {

/** The title line's height plus the 14px of air Kotlin puts under it, from the panel's top. */
constexpr int ROW_AREA_TOP = TEXT_PADDING + ROW_HEIGHT + 14;   // 3 + 21 + 14 = 38

/** Row 0 is the THEME row; the rest are colours. */
int total_rows() { return 1 + static_cast<int>(theme_color_rows().size()); }   // 18

}  // namespace

int ThemeEditorModule::visible_row_count() {
    return std::max(1, (HEIGHT - ROW_AREA_TOP) / ROW_HEIGHT);   // (392 − 38) / 21 = 16
}

int ThemeEditorModule::scroll_offset(int cursor_row) {
    const int visible   = visible_row_count();
    const int max_scroll = std::max(0, total_rows() - visible);   // 18 − 16 = 2
    const int wanted    = (cursor_row >= visible) ? cursor_row - visible + 1 : 0;
    return std::min(std::max(wanted, 0), max_scroll);
}

void ThemeEditorModule::draw(Canvas& c, int x, int y, const ThemeState& s) const {
    const Theme&            t  = s.theme;
    const ThemeEditorState& es = s.editor;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    c.draw_text("THEME EDIT", x + NAME_COL_X, y + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);

    // The colour list is taller than the panel, so the rows below the title scroll to keep the cursor
    // in view — the same idea as the song screen and the file browser.
    const int visible = visible_row_count();
    const int scroll  = scroll_offset(es.cursorRow);

    const auto row_visible = [&](int logical) {
        return logical >= scroll && logical < scroll + visible;
    };
    const auto row_top = [&](int logical) {
        return y + ROW_AREA_TOP + (logical - scroll) * ROW_HEIGHT;
    };

    // One rule for every value on a row, and it is worth naming once rather than writing eleven times:
    // the cursor's own CHANNEL is `textCursor`, the other channels of the cursor's ROW are `textValue`
    // (so you can read the colour you are dialling), and every row you are not on is `textParam`.
    const auto value_color = [&](bool on_row, int channel) {
        if (on_row && es.cursorChannel == channel) return t.textCursor;
        if (on_row)                                return t.textValue;
        return t.textParam;
    };

    // ── Row 0: THEME — the built-in cycle, SAVE, LOAD ────────────────────────────────────────────
    if (row_visible(0)) {
        const bool on_row = (es.cursorRow == 0);
        const int  ry     = row_top(0);
        const int  ty     = ry + TEXT_PADDING;

        if (on_row) c.fill_rect(x, ry, WIDTH, ROW_HEIGHT, t.rowCursor);

        c.draw_text("THEME", x + NAME_COL_X, ty,
                    on_row ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);

        c.draw_text(t.name, x + THEME_NAME_X, ty, value_color(on_row, 0), CHAR_SPACING, FONT_SCALE);
        c.draw_text("SAVE",  x + SAVE_LABEL_X, ty, value_color(on_row, 1), CHAR_SPACING, FONT_SCALE);
        c.draw_text("LOAD",  x + LOAD_LABEL_X, ty, value_color(on_row, 2), CHAR_SPACING, FONT_SCALE);
    }

    // ── Rows 1..17: the colours ──────────────────────────────────────────────────────────────────
    const auto& rows = theme_color_rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        const int logical = static_cast<int>(i) + 1;
        if (!row_visible(logical)) continue;

        const ThemeColorRow& row    = rows[i];
        const Argb           color  = t.*(row.field);
        const bool           on_row = (es.cursorRow == logical);
        const int            ry     = row_top(logical);
        const int            ty     = ry + TEXT_PADDING;

        if (on_row) c.fill_rect(x, ry, WIDTH, ROW_HEIGHT, t.rowCursor);

        c.draw_text(row.label, x + NAME_COL_X, ty,
                    on_row ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);

        const int r = static_cast<int>((color >> 16) & 0xFF);
        const int g = static_cast<int>((color >> 8) & 0xFF);
        const int b = static_cast<int>(color & 0xFF);

        c.draw_text(hex2(r), x + R_COL_X, ty, value_color(on_row, 0), CHAR_SPACING, FONT_SCALE);
        c.draw_text(hex2(g), x + G_COL_X, ty, value_color(on_row, 1), CHAR_SPACING, FONT_SCALE);
        c.draw_text(hex2(b), x + B_COL_X, ty, value_color(on_row, 2), CHAR_SPACING, FONT_SCALE);

        // The swatch — the only reason the R/G/B columns are usable at all. It is drawn with the
        // colour ITSELF, which makes this the one module in the app whose output is not a function of
        // the theme's text roles.
        c.fill_rect(x + SWATCH_X, ry, SWATCH_W, ROW_HEIGHT, color);
    }
}

}  // namespace pt::ui
