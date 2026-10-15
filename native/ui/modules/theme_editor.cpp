#include "ui/modules/theme_editor.h"

#include <algorithm>
#include <vector>

#include "ui/helpers.h"
#include "ui/theme_rules.h"

namespace pt::ui {

namespace {

constexpr int ROW_AREA_TOP = TEXT_PADDING + ROW_HEIGHT + 14;   // 3 + 21 + 14 = 38

int total_rows() { return THEME_FIRST_COLOR_ROW + static_cast<int>(theme_color_rows().size()); }

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

    // ⚠️ The accent, never red (red means destruction). A failed roll outranks the cursor's reading.
    // Seventeen characters: "BLENDS " + `MTR BORDER` exactly.
    constexpr int MSG_X    = NAME_COL_X + 11 * CHAR_W;   // past "THEME EDIT" and a space
    constexpr int MSG_COLS = (WIDTH - 10 - MSG_X) / CHAR_W;

    std::string message = es.message;
    if (message.empty()) {
        const char* partner = theme_row_clash_partner(t, theme_color_index(es.cursorRow));
        if (partner != nullptr) message = std::string("BLENDS ") + partner;
    }
    if (!message.empty()) {
        c.draw_text(Canvas::clip_text(message, MSG_COLS), x + MSG_X, y + TEXT_PADDING,
                    cursor_mark_ink(t), CHAR_SPACING, FONT_SCALE);
    }

    // The rows below the title scroll to keep the cursor in view.
    const int visible = visible_row_count();
    const int scroll  = scroll_offset(es.cursorRow);

    const auto row_visible = [&](int logical) {
        return logical >= scroll && logical < scroll + visible;
    };
    const auto row_top = [&](int logical) {
        return y + ROW_AREA_TOP + (logical - scroll) * ROW_HEIGHT;
    };

    // The cursor row's other channels are `textValue`, other rows `textParam`; the cursor's own
    // channel is inked by the painter.
    const auto value_color = [&](bool on_row, int /*channel*/) {
        return on_row ? t.textValue : t.textParam;
    };
    const auto on_cell = [&](bool on_row, int channel) {
        return on_row && es.cursorChannel == channel;
    };

    // ── Row 0: THEME — the built-in cycle, SAVE, LOAD ────────────────────────────────────────────
    if (row_visible(THEME_ROW_THEME)) {
        const bool on_row = (es.cursorRow == THEME_ROW_THEME);
        const int  ry     = row_top(THEME_ROW_THEME);
        const int  ty     = ry + TEXT_PADDING;

        c.draw_text("THEME", x + NAME_COL_X, ty,
                    on_row ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);

        // ⚠️ Clipped before SAVE, so a typed name cannot cover the cells.
        constexpr int NAME_COLS = (SAVE_LABEL_X - THEME_NAME_X) / CHAR_W;
        draw_cursor_cell(c, Canvas::clip_text(t.name, NAME_COLS), x + THEME_NAME_X, ty,
                         on_cell(on_row, 0), value_color(on_row, 0), t);
        draw_cursor_cell(c, "SAVE", x + SAVE_LABEL_X, ty, on_cell(on_row, 1),
                         value_color(on_row, 1), t);
        draw_cursor_cell(c, "LOAD", x + LOAD_LABEL_X, ty, on_cell(on_row, 2),
                         value_color(on_row, 2), t);
    }

    // ── Row 1: RANDOMIZE — the colour scheme, and the roll itself ────────────────────────────────
    if (row_visible(THEME_ROW_RANDOM)) {
        const bool on_row = (es.cursorRow == THEME_ROW_RANDOM);
        const int  ry     = row_top(THEME_ROW_RANDOM);
        const int  ty     = ry + TEXT_PADDING;

        c.draw_text("RANDOMIZE", x + NAME_COL_X, ty,
                    on_row ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);

        // The scheme is a cell, so the row says which hue relationship a roll will give.
        draw_cursor_cell(c, theme_scheme_label(es.scheme), x + SCHEME_LABEL_X, ty,
                         on_cell(on_row, 0), value_color(on_row, 0), t);
        draw_cursor_cell(c, "ROLL", x + ROLL_LABEL_X, ty, on_cell(on_row, 1),
                         value_color(on_row, 1), t);
    }

    // ── The colour rows ──────────────────────────────────────────────────────────────────────────
    const auto& rows = theme_color_rows();

    // One validator pass for the whole panel. ⚠️ A contrast miss marks the ink, never the ground (one
    // text role sits on four grounds); the ground is named in the message. A separation or
    // distinctness miss marks both.
    std::vector<bool> clash(rows.size(), false);
    for (const ThemeViolation& v : theme_violations(t, /*generator=*/false)) {
        const bool symmetric = (v.rule->kind != RuleKind::Contrast);
        for (size_t k = 0; k < rows.size(); ++k) {
            if (rows[k].field == v.rule->a || (symmetric && rows[k].field == v.rule->b)) {
                clash[k] = true;
            }
        }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        const int logical = static_cast<int>(i) + THEME_FIRST_COLOR_ROW;
        if (!row_visible(logical)) continue;

        const ThemeColorRow& row    = rows[i];
        const Argb           color  = t.*(row.field);
        const bool           on_row = (es.cursorRow == logical);
        const int            ry     = row_top(logical);
        const int            ty     = ry + TEXT_PADDING;

        c.draw_text(row.label, x + NAME_COL_X, ty,
                    on_row ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);

        // One character, two facts; the lock wins — a locked colour's clash is not flagged.
        if (es.locks.locked(static_cast<int>(i))) {
            c.draw_text("*", x + WARN_COL_X, ty, cursor_mark_ink(t), CHAR_SPACING, FONT_SCALE);
        } else if (clash[i]) {
            c.draw_text("!", x + WARN_COL_X, ty, cursor_mark_ink(t), CHAR_SPACING, FONT_SCALE);
        }

        const int r = static_cast<int>((color >> 16) & 0xFF);
        const int g = static_cast<int>((color >> 8) & 0xFF);
        const int b = static_cast<int>(color & 0xFF);

        draw_cursor_cell(c, hex2(r), x + R_COL_X, ty, on_cell(on_row, 0), value_color(on_row, 0), t);
        draw_cursor_cell(c, hex2(g), x + G_COL_X, ty, on_cell(on_row, 1), value_color(on_row, 1), t);
        draw_cursor_cell(c, hex2(b), x + B_COL_X, ty, on_cell(on_row, 2), value_color(on_row, 2), t);

        // The swatch, in the colour itself.
        c.fill_rect(x + SWATCH_X, ry, SWATCH_W, ROW_HEIGHT, color);
    }
}

}  // namespace pt::ui
