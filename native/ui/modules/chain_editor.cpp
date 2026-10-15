#include "ui/modules/chain_editor.h"

#include "ui/helpers.h"

namespace pt::ui {

void ChainEditorModule::draw(Canvas& c, int x, int y, const ChainEditorState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    int       colX  = x + 10;
    const int stepX = colX; colX += 30 + 10;
    const int phX   = colX; colX += 30 + 20;
    const int tspX  = colX;

    int rowY = y + TEXT_PADDING;
    c.draw_text("CHAIN " + hex2(s.chain.id), x + 10, rowY, t.textTitle, CHAR_SPACING, FONT_SCALE);
    // The song cell in the title's dim colour: context, not a second title.
    if (s.songRow >= 0 && s.songTrack >= 0) {
        c.draw_text("S" + hex2(s.songRow) + " T" + std::to_string(s.songTrack + 1), x + 170, rowY,
                    t.textParam, CHAR_SPACING, FONT_SCALE);
    }

    // The header lights for the cursor's column; the row number lights for its row.
    rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING;
    c.draw_text("PH",  phX,  rowY, header_color(s.cursorColumn, 1, 1, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("TSP", tspX, rowY, header_color(s.cursorColumn, 2, 2, t), CHAR_SPACING, FONT_SCALE);

    for (int index = 0; index < 16; ++index) draw_row(c, x, y, index, s, stepX, phX, tspX);
}

void ChainEditorModule::draw_row(Canvas& c, int x, int y, int index, const ChainEditorState& s,
                                 int stepX, int phX, int tspX) const {
    const Theme& t = s.theme;

    const int  dataRowY  = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (index * ROW_HEIGHT);
    const int  phraseRef = s.chain.phraseRefs[static_cast<size_t>(index)];
    const bool isEmpty   = (phraseRef == -1);

    c.fill_rect(x, dataRowY, WIDTH, ROW_HEIGHT, row_bg_color(index, t));

    const int textY = dataRowY + TEXT_PADDING;

    // Left to right: RowCells joins a selected run across the gutters only in layout order.
    RowCells cells(c, textY, t);

    const auto cur = [&](int col) { return index == s.cursorRow && s.cursorColumn == col; };
    const auto sel = [&](int col) { return s.selectionMode && s.isCellSelected(index, col); };

    // The cursor row's number lights whatever the column. Column 0 is a real cursor position.
    const Argb stepColor = (index == s.cursorRow) ? cursor_mark_ink(t)
                           : (index % 4 == 0)     ? t.textParam
                                                  : t.textEmpty;
    cells.cell(hex1(index), stepX, cur(0), /*is_selected=*/false, /*is_empty=*/false, stepColor);

    // The marker sits beside the one-character row number. Two tracks on one row share the glyph.
    for (const TrackPlayhead& ph : s.playheads)
        if (ph.chainId == s.chain.id && ph.chainRow == index) draw_playhead(c, stepX + CHAR_W, textY, t);

    cells.cell(isEmpty ? "--" : hex2(phraseRef), phX, cur(1), sel(1), isEmpty, t.textValue);

    // An empty slot has no transpose to show.
    const int transposeValue = s.chain.transposeValues[static_cast<size_t>(index)];
    cells.cell(isEmpty ? "--" : hex2(transposeValue), tspX, cur(2), sel(2), isEmpty, t.textParam);
}

CursorContext ChainEditorModule::cursor_context(const ChainEditorState& s) const {
    const size_t row = static_cast<size_t>(s.cursorRow);
    switch (s.cursorColumn) {
        case 0: return cc::read_only();
        case 1: return cc::phrase_ref(s.chain.phraseRefs[row], /*can_create=*/true);
        case 2: return cc::transpose(s.chain.transposeValues[row],
                                     /*is_empty=*/s.chain.phraseRefs[row] == -1, /*def=*/0x00);
        default: return cc::none();
    }
}

ChainInputResult ChainEditorModule::handle_input(songcore::Chain& chain, int cursor_row,
                                                 int cursor_column,
                                                 const InputAction& action) const {
    ChainInputResult r;
    const size_t     row = static_cast<size_t>(cursor_row);

    // Both fields: DELETE clears the transpose with the ref, and an empty ref can carry a transpose.
    const int beforeRef       = chain.phraseRefs[row];
    const int beforeTranspose = chain.transposeValues[row];

    switch (action.type) {
        case ActionType::SET_VALUE:
            if (cursor_column == 1) {
                chain.phraseRefs[row] = action.value;
                r.hasPhrase           = true;
                r.lastEditedPhrase    = action.value;
            } else if (cursor_column == 2) {
                chain.transposeValues[row] = action.value;
                r.hasTranspose             = true;
                r.lastEditedTranspose      = action.value;
            }
            break;

        case ActionType::DELETE:
            // Clearing the slot clears its transpose, so a reused slot inherits none.
            if (cursor_column == 1) {
                chain.phraseRefs[row]      = -1;
                chain.transposeValues[row] = 0x00;
            }
            break;

        case ActionType::INSERT_DEFAULT:
            if (cursor_column == 1) {
                chain.phraseRefs[row]      = 0;
                chain.transposeValues[row] = 0x00;
                r.hasPhrase                = true;
                r.lastEditedPhrase         = 0;
                r.hasTranspose             = true;
                r.lastEditedTranspose      = 0;
            }
            break;

        default:
            break;
    }

    // ⚠️ A before/after answer: a phrase-ref cell sends DELETE while already empty, and a phantom
    // dirty flag means a phantom RECOVER WORK?.
    r.modified = (chain.phraseRefs[row] != beforeRef) ||
                 (chain.transposeValues[row] != beforeTranspose);
    return r;
}

}  // namespace pt::ui
