#include "ui/modules/table_editor.h"

#include "ui/helpers.h"

namespace pt::ui {

using songcore::TableRow;

void TableModule::draw(Canvas& c, int x, int y, const TableState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    int       colX       = x + 10;
    const int stepX      = colX; colX += 30 + 10;
    const int transposeX = colX; colX += 45 + 15;
    const int volX       = colX; colX += 30 + 15;
    const int fx1NameX   = colX; colX += 45 + 10;
    const int fx1ValueX  = colX; colX += 30 + 15;
    const int fx2NameX   = colX; colX += 45 + 10;
    const int fx2ValueX  = colX; colX += 30 + 15;
    const int fx3NameX   = colX; colX += 45 + 10;
    const int fx3ValueX  = colX;

    int rowY = y + TEXT_PADDING;
    c.draw_text("TABLE " + hex2(s.table.id), x + 10, rowY, t.textTitle, CHAR_SPACING, FONT_SCALE);
    // The tic rate belongs to the INSTRUMENT, not the table — a table run by two instruments runs at
    // two speeds. It is shown here because this is where you feel it.
    c.draw_text(hex2(s.ticRate) + " TIC", x + WIDTH - 120, rowY, t.textParam, CHAR_SPACING, FONT_SCALE);

    rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING;
    c.draw_text("N",   transposeX, rowY, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("V",   volX,       rowY, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX1", fx1NameX,   rowY, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX2", fx2NameX,   rowY, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX3", fx3NameX,   rowY, t.textParam, CHAR_SPACING, FONT_SCALE);

    for (int i = 0; i < static_cast<int>(s.table.rows.size()); ++i) {
        draw_row(c, x, y, i, s.table.rows[static_cast<size_t>(i)], s, stepX, transposeX, volX,
                 fx1NameX, fx1ValueX, fx2NameX, fx2ValueX, fx3NameX, fx3ValueX);
    }
}

void TableModule::draw_row(Canvas& c, int x, int y, int index, const TableRow& row,
                           const TableState& s, int stepX, int transposeX, int volX, int fx1NameX,
                           int fx1ValueX, int fx2NameX, int fx2ValueX, int fx3NameX,
                           int fx3ValueX) const {
    const Theme& t = s.theme;

    const int dataRowY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (index * ROW_HEIGHT);

    bool isRowSelected = false;
    if (s.selectionMode) {
        for (int col = 1; col <= 8 && !isRowSelected; ++col) isRowSelected = s.isCellSelected(index, col);
    }

    c.fill_rect(x, dataRowY, WIDTH, ROW_HEIGHT,
                row_bg_color(index, s.cursorRow, s.playbackRow, s.playbackRow >= 0, isRowSelected, t));

    const int textY = dataRowY + TEXT_PADDING;

    const auto cur = [&](int col) { return index == s.cursorRow && s.cursorColumn == col; };
    const auto sel = [&](int col) { return s.selectionMode && s.isCellSelected(index, col); };

    // No every-4th accent: a table row is a tic, not a beat.
    c.draw_text(hex1(index), stepX, textY, cur(0) ? t.textCursor : t.textEmpty, CHAR_SPACING,
                FONT_SCALE);

    // Transpose is always shown — 0x00 is "no transpose", drawn dim, but it is still a value.
    draw_cell(c, hex2(row.transpose), transposeX, textY, cur(1), sel(1),
              /*is_empty=*/row.transpose == 0x00, t.textValue, t);

    // Volume −1 IS empty: "leave the note's own volume alone".
    draw_cell(c, row.volume == -1 ? "--" : hex2(row.volume), volX, textY, cur(2), sel(2),
              /*is_empty=*/row.volume == -1, t.textValue, t);

    // Both FX cells are textValue here — see the header. FX1 = cols 3/4, FX2 = 5/6, FX3 = 7/8.
    const bool fx1Empty = (row.fx1Type == 0);
    draw_cell(c, effect_name(row.fx1Type), fx1NameX,  textY, cur(3), sel(3), fx1Empty, t.textValue, t);
    draw_cell(c, hex2(row.fx1Value),       fx1ValueX, textY, cur(4), sel(4), fx1Empty, t.textValue, t);
    const bool fx2Empty = (row.fx2Type == 0);
    draw_cell(c, effect_name(row.fx2Type), fx2NameX,  textY, cur(5), sel(5), fx2Empty, t.textValue, t);
    draw_cell(c, hex2(row.fx2Value),       fx2ValueX, textY, cur(6), sel(6), fx2Empty, t.textValue, t);
    const bool fx3Empty = (row.fx3Type == 0);
    draw_cell(c, effect_name(row.fx3Type), fx3NameX,  textY, cur(7), sel(7), fx3Empty, t.textValue, t);
    draw_cell(c, hex2(row.fx3Value),       fx3ValueX, textY, cur(8), sel(8), fx3Empty, t.textValue, t);
}

CursorContext TableModule::cursor_context(const TableState& s) const {
    const songcore::TableRow& row = s.table.rows[static_cast<size_t>(s.cursorRow)];
    switch (s.cursorColumn) {
        case 0: return cc::read_only();

        case 1: {
            // The SAME semitone context the chain's TSP uses, so A+LEFT/RIGHT is ±1 octave on both.
            // It was a plain hex_byte with a ±16 large step, which had drifted from the chain.
            CursorContext ctx            = cc::transpose(row.transpose);
            ctx.capabilities.canDelete   = (row.transpose != 0x00);  // deletable back to 00 = no transpose
            return ctx;
        }

        case 2: return cc::hex_byte(row.volume == -1 ? 0 : row.volume, /*min=*/0, /*max=*/255,
                                    /*empty_value=*/-1, /*can_delete=*/row.volume != -1,
                                    /*can_insert=*/true);

        case 3: return cc::effect_type(row.fx1Type, 1);
        case 4: return cc::hex_byte(row.fx1Value, 0, effect_value_max(row.fx1Type));
        case 5: return cc::effect_type(row.fx2Type, 2);
        case 6: return cc::hex_byte(row.fx2Value, 0, effect_value_max(row.fx2Type));
        case 7: return cc::effect_type(row.fx3Type, 3);
        case 8: return cc::hex_byte(row.fx3Value, 0, effect_value_max(row.fx3Type));

        default: return cc::none();
    }
}

TableInputResult TableModule::handle_input(songcore::Table& table, int cursor_row, int cursor_column,
                                           const InputAction& action) const {
    TableRow&        row = table.rows[static_cast<size_t>(cursor_row)];
    TableInputResult r;

    const auto clamp255 = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };

    switch (action.type) {
        case ActionType::SET_VALUE:
            switch (cursor_column) {
                case 1: row.transpose = clamp255(action.value); break;
                case 2: row.volume    = clamp255(action.value); break;
                // The FX type columns store an INDEX into EFFECT_TYPES; convert back to the code.
                case 3: row.fx1Type  = songcore::effect_type_at(action.value); break;
                case 4: row.fx1Value = clamp255(action.value); break;
                case 5: row.fx2Type  = songcore::effect_type_at(action.value); break;
                case 6: row.fx2Value = clamp255(action.value); break;
                case 7: row.fx3Type  = songcore::effect_type_at(action.value); break;
                case 8: row.fx3Value = clamp255(action.value); break;
                default: break;
            }
            break;

        case ActionType::DELETE:
            switch (cursor_column) {
                case 1: row.transpose = 0x00; break;  // back to no transpose
                case 2: row.volume    = -1;   break;  // back to no volume change
                case 3: row.fx1Type = 0; row.fx1Value = 0; break;
                case 5: row.fx2Type = 0; row.fx2Value = 0; break;
                case 7: row.fx3Type = 0; row.fx3Value = 0; break;
                default: break;
            }
            break;

        default:
            break;
    }

    r.modified = (action.type != ActionType::NONE);
    return r;
}

}  // namespace pt::ui
