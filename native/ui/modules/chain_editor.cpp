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
    // The song cell, in the title's own dim colour so it reads as context rather than as a second title.
    if (s.songRow >= 0 && s.songTrack >= 0) {
        c.draw_text("S" + hex2(s.songRow) + " T" + std::to_string(s.songTrack + 1), x + 170, rowY,
                    t.textParam, CHAR_SPACING, FONT_SCALE);
    }

    // The header lights for the column the cursor is in — it is half of what the row highlight used
    // to say, the row number being the other half.
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

    // Left to right, column by column — RowCells joins a selected run into one block across the
    // gutters, and it can only do that if the cells arrive in the order they are laid out.
    RowCells cells(c, textY, t);

    const auto cur = [&](int col) { return index == s.cursorRow && s.cursorColumn == col; };
    const auto sel = [&](int col) { return s.selectionMode && s.isCellSelected(index, col); };

    // The whole cursor ROW's number lights, whatever column the cursor is in — that is what now says
    // which row is being edited. Column 0 is a real cursor position, so the number goes through
    // the painter and gets the cell background when the cursor is actually on it.
    const Argb stepColor = (index == s.cursorRow) ? t.textCursor
                           : (index % 4 == 0)     ? t.textParam
                                                  : t.textEmpty;
    cells.cell(hex1(index), stepX, cur(0), /*is_selected=*/false, /*is_empty=*/false, stepColor);

    // The gutter holds a ONE-character row number, so the marker sits beside it and no column
    // moves. Any track inside this chain marks its own row; two tracks on the same row overlap on
    // the same glyph, which is the honest picture.
    for (const TrackPlayhead& ph : s.playheads)
        if (ph.chainId == s.chain.id && ph.chainRow == index) draw_playhead(c, stepX + CHAR_W, textY, t);

    cells.cell(isEmpty ? "--" : hex2(phraseRef), phX, cur(1), sel(1), isEmpty, t.textValue);

    // An empty slot has no transpose to show — the emptiness of BOTH cells is the phrase ref's.
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

    // Both fields, because DELETE clears the transpose with the ref and column 2 can leave a
    // transpose on a row whose ref is empty — so a DELETE there really does change something.
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
            // clearChainSlot(): clearing the slot clears its transpose too, so a slot reused later
            // cannot inherit a stale one.
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

    // ⚠️ A before/after answer, not "an action was dispatched" — see SongEditorModule::handle_input
    // for why a phrase-ref cell dispatches DELETE while already empty, and what a phantom dirty flag
    // costs (a phantom "unsaved work?" on EXIT, then a phantom RECOVER WORK? at the next launch).
    r.modified = (chain.phraseRefs[row] != beforeRef) ||
                 (chain.transposeValues[row] != beforeTranspose);
    return r;
}

}  // namespace pt::ui
