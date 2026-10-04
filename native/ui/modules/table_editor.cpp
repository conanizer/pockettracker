#include "ui/modules/table_editor.h"

#include "table_automation.h"
#include "ui/helpers.h"

namespace pt::ui {

using songcore::TableRow;

// ─── Where FX2's and FX3's playback markers stand, and so how wide the gap before them is ───────
// ⚠️ One definition, read by both the column layout and the marker draw. A cell paints its
// background CHAR_SPACING past its text, so the marker needs MARKER_CLEAR either side or the
// cursor's fill on the value to its left touches the `>`.
constexpr int MARKER_CLEAR = 4;
constexpr int GLYPH_W      = CHAR_W - CHAR_SPACING;

/** The gap an FX value cell's x must leave before the NEXT FX column's name x. */
constexpr int marker_gap() { return 2 * CHAR_SPACING + 2 * MARKER_CLEAR + GLYPH_W; }

/** …and where in that gap the marker itself goes, given the name x on its right. */
constexpr int marker_x(int name_x) { return name_x - CHAR_SPACING - MARKER_CLEAR - GLYPH_W; }

void TableModule::draw(Canvas& c, int x, int y, const TableState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    // ⚠️ The gap before each FX column is wider: that is where the playback markers stand (see
    // marker_gap). The extra comes out of the slack at the right edge.
    int       colX       = x + 10;
    const int stepX      = colX; colX += 30 + 10;
    const int transposeX = colX; colX += 45 + 15;
    const int volX       = colX; colX += 2 * CHAR_W + marker_gap();
    const int fx1NameX   = colX; colX += 45 + 10;
    const int fx1ValueX  = colX; colX += 2 * CHAR_W + marker_gap();
    const int fx2NameX   = colX; colX += 45 + 10;
    const int fx2ValueX  = colX; colX += 2 * CHAR_W + marker_gap();
    const int fx3NameX   = colX; colX += 45 + 10;
    const int fx3ValueX  = colX;

    int rowY = y + TEXT_PADDING;
    c.draw_text("TABLE " + hex2(s.table.id), x + 10, rowY, t.textTitle, CHAR_SPACING, FONT_SCALE);
    // The tic rate belongs to the INSTRUMENT, not the table — a table run by two instruments runs at
    // two speeds. It is shown here because this is where you feel it.
    c.draw_text(hex2(s.ticRate) + " TIC", x + WIDTH - 120, rowY, t.textParam, CHAR_SPACING, FONT_SCALE);

    // The header lights for the cursor's column (an FX header covers its name and value cell); the
    // row number lights for its row.
    rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING;
    const int cc = s.cursorColumn;
    c.draw_text("N",   transposeX, rowY, header_color(cc, 1, 1, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("V",   volX,       rowY, header_color(cc, 2, 2, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX1", fx1NameX,   rowY, header_color(cc, 3, 4, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX2", fx2NameX,   rowY, header_color(cc, 5, 6, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX3", fx3NameX,   rowY, header_color(cc, 7, 8, t), CHAR_SPACING, FONT_SCALE);

    // Asked once per draw: an AUS/AUF cell no ramp uses draws dimmed — the same pairing walk the
    // engine runs over the same three FX slots, so the grid agrees with what plays.
    const table_automation::TableRampCells rampCells =
            table_automation::find_table_ramp_cells(s.table.rows.data(),
                                                    static_cast<int>(s.table.rows.size()));

    for (int i = 0; i < static_cast<int>(s.table.rows.size()); ++i) {
        draw_row(c, x, y, i, s.table.rows[static_cast<size_t>(i)], s, rampCells, stepX, transposeX,
                 volX, fx1NameX, fx1ValueX, fx2NameX, fx2ValueX, fx3NameX, fx3ValueX);
    }
}

void TableModule::draw_row(Canvas& c, int x, int y, int index, const TableRow& row,
                           const TableState& s,
                           const table_automation::TableRampCells& rampCells, int stepX,
                           int transposeX, int volX, int fx1NameX, int fx1ValueX, int fx2NameX,
                           int fx2ValueX, int fx3NameX, int fx3ValueX) const {
    const Theme& t = s.theme;

    const int dataRowY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (index * ROW_HEIGHT);

    c.fill_rect(x, dataRowY, WIDTH, ROW_HEIGHT, row_bg_color(index, t));

    const int textY = dataRowY + TEXT_PADDING;

    // Left to right, column by column — RowCells joins a selected run into one block across the
    // gutters, and it can only do that if the cells arrive in the order they are laid out.
    RowCells cells(c, textY, t);

    const auto cur = [&](int col) { return index == s.cursorRow && s.cursorColumn == col; };
    const auto sel = [&](int col) { return s.selectionMode && s.isCellSelected(index, col); };

    // No every-4th accent: a table row is a tic. The number lights on the cursor row and goes through
    // the painter, since column 0 is a real cursor position.
    cells.cell(hex1(index), stepX, cur(0), /*is_selected=*/false, /*is_empty=*/false,
               (index == s.cursorRow) ? cursor_mark_ink(t) : t.textEmpty);

    // Transpose is always shown — 0x00 is "no transpose", drawn dim, but it is still a value.
    cells.cell(hex2(row.transpose), transposeX, cur(1), sel(1),
               /*is_empty=*/row.transpose == 0x00, t.textValue);

    // Volume −1 IS empty: "leave the note's own volume alone".
    cells.cell(row.volume == -1 ? "--" : hex2(row.volume), volX, cur(2), sel(2),
               /*is_empty=*/row.volume == -1, t.textValue);

    // Both FX cells are textValue here (see the header). FX1 = cols 3/4, FX2 = 5/6, FX3 = 7/8. An FX
    // pair dims when unset, and an AUS/AUF cell when no ramp uses it.
    const auto fxDim = [&](int type, int slot) {
        return type == 0x00 || !rampCells.active(type, index, slot);
    };

    const bool fx1Empty = fxDim(row.fx1Type, 1);
    cells.cell(effect_name(row.fx1Type), fx1NameX,  cur(3), sel(3), fx1Empty, t.textValue);
    cells.cell(hex2(row.fx1Value),       fx1ValueX, cur(4), sel(4), fx1Empty, t.textValue);
    const bool fx2Empty = fxDim(row.fx2Type, 2);
    cells.cell(effect_name(row.fx2Type), fx2NameX,  cur(5), sel(5), fx2Empty, t.textValue);
    cells.cell(hex2(row.fx2Value),       fx2ValueX, cur(6), sel(6), fx2Empty, t.textValue);
    const bool fx3Empty = fxDim(row.fx3Type, 3);
    cells.cell(effect_name(row.fx3Type), fx3NameX,  cur(7), sel(7), fx3Empty, t.textValue);
    cells.cell(hex2(row.fx3Value),       fx3ValueX, cur(8), sel(8), fx3Empty, t.textValue);

    // Three playheads, four markers: lanes 1 and 2 one each, ahead of their FX column; lane 0 two —
    // beside the row number (it drives note and volume too) and ahead of FX1, so all three FX columns
    // read alike. A stopped lane reads −1 and has none.
    // ⚠️ Drawn AFTER the cells: a selection spanning a gutter fills it and would erase the marker.
    if (s.playbackRows[0] == index) {
        draw_playhead(c, stepX + CHAR_W, textY, t);
        draw_playhead(c, marker_x(fx1NameX), textY, t);
    }
    if (s.playbackRows[1] == index) draw_playhead(c, marker_x(fx2NameX), textY, t);
    if (s.playbackRows[2] == index) draw_playhead(c, marker_x(fx3NameX), textY, t);
}

CursorContext TableModule::cursor_context(const TableState& s) const {
    const songcore::TableRow& row = s.table.rows[static_cast<size_t>(s.cursorRow)];
    switch (s.cursorColumn) {
        case 0: return cc::read_only();

        case 1: {
            // The same semitone context as the chain's TSP, so A+UP/DOWN is ±1 octave on both.
            CursorContext ctx            = cc::transpose(row.transpose);
            ctx.capabilities.canDelete   = (row.transpose != 0x00);  // deletable back to 00 = no transpose
            return ctx;
        }

        case 2: return cc::hex_byte(row.volume == -1 ? 0 : row.volume, /*min=*/0, /*max=*/255,
                                    /*empty_value=*/-1, /*can_delete=*/row.volume != -1,
                                    /*can_insert=*/true);

        case 3: return cc::effect_type(row.fx1Type, 1, s.effectTypeCount);
        case 4: return cc::hex_byte(row.fx1Value, 0, effect_value_max(row.fx1Type));
        case 5: return cc::effect_type(row.fx2Type, 2, s.effectTypeCount);
        case 6: return cc::hex_byte(row.fx2Value, 0, effect_value_max(row.fx2Type));
        case 7: return cc::effect_type(row.fx3Type, 3, s.effectTypeCount);
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
