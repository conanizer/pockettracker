#include "ui/modules/theme_editor.h"

#include <algorithm>
#include <vector>

#include "ui/helpers.h"
#include "ui/theme_rules.h"

namespace pt::ui {

namespace {

/** The title line's height plus the 14px of air Kotlin puts under it, from the panel's top. */
constexpr int ROW_AREA_TOP = TEXT_PADDING + ROW_HEIGHT + 14;   // 3 + 21 + 14 = 38

/** THEME and RANDOMIZE on top; the rest are colours. */
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

    // ⚠️ THE MESSAGE TAKES THE ACCENT, NEVER RED. A red bar in this app means something is about to
    // be destroyed; a palette that is merely hard to read is not that, and spending the alarm colour
    // here is how it stops meaning anything where it matters.
    // A failed roll is an event and outranks the cursor's own reading until something else happens.
    //
    // ⚠️ SEVENTEEN CHARACTERS, and the phrasing is chosen to fit the LONGEST row label rather than
    // the one in front of whoever is reading: "BLENDS " + `MTR BORDER` is exactly the budget. A
    // fuller sentence clipped to "CLASHES WITH TXT…" names no colour at all, which is the one thing
    // the line exists to do.
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
    // the other channels of the cursor's ROW are `textValue` (so you can read the colour you are
    // dialling) and every row you are not on is `textParam`. ⚠️ The cursor's OWN channel is not a case
    // here — it is a cell, and a cell's ink comes from the painter, which inverts it against the bar.
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

        // ⚠️ CLIPPED, and the budget is the gap to the SAVE column. A name is user-typed and
        // unbounded; unclipped it does not merely spill off the panel, it paints over the cells this
        // screen is exited through. Derived from the two X constants, so moving a column cannot leave
        // the budget behind.
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

        // ⭐ The scheme is a CELL rather than a hidden mode so that the row says, before anything is
        // rolled, which relationship the hues will have.
        draw_cursor_cell(c, theme_scheme_label(es.scheme), x + SCHEME_LABEL_X, ty,
                         on_cell(on_row, 0), value_color(on_row, 0), t);
        draw_cursor_cell(c, "ROLL", x + ROLL_LABEL_X, ty, on_cell(on_row, 1),
                         value_color(on_row, 1), t);
    }

    // ── The colour rows ──────────────────────────────────────────────────────────────────────────
    const auto& rows = theme_color_rows();

    // ⚠️ ONE validator pass for the whole panel. Asking per row would run the same rules over the
    // same palette nineteen times, every frame the editor is up.
    //
    // ⚠️⚠️ **A CONTRAST MISS MARKS THE INK, NEVER THE GROUND.** Marking both ends is the obvious
    // reading and it makes the panel useless: one placeholder too dim to read lights up BACKGROUND,
    // ROW 4TH, VIZ BG and MTR BG as well, because a text role lands on all four — and none of those
    // four is the colour anyone would change. The ink is the culprit; the ground is named in the
    // message instead. A separation or distinctness miss IS symmetric and marks both.
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

        // ⚠️ ONE CHARACTER, TWO FACTS, AND THE LOCK WINS. A locked row cannot be re-rolled, so
        // whatever it clashes with is a thing the user is choosing to keep — saying so every frame
        // would be nagging about a decision already made.
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

        // The swatch — the only reason the R/G/B columns are usable at all. It is drawn with the
        // colour ITSELF, which makes this the one module in the app whose output is not a function of
        // the theme's text roles.
        c.fill_rect(x + SWATCH_X, ry, SWATCH_W, ROW_HEIGHT, color);
    }
}

}  // namespace pt::ui
