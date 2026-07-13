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

    rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING;
    c.draw_text("PH",  phX,  rowY, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("TSP", tspX, rowY, t.textParam, CHAR_SPACING, FONT_SCALE);

    for (int index = 0; index < 16; ++index) draw_row(c, x, y, index, s, stepX, phX, tspX);
}

void ChainEditorModule::draw_row(Canvas& c, int x, int y, int index, const ChainEditorState& s,
                                 int stepX, int phX, int tspX) const {
    const Theme& t = s.theme;

    const int  dataRowY  = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (index * ROW_HEIGHT);
    const int  phraseRef = s.chain.phraseRefs[static_cast<size_t>(index)];
    const bool isEmpty   = (phraseRef == -1);

    bool isRowSelected = false;
    if (s.selectionMode) {
        for (int col = 1; col <= 2 && !isRowSelected; ++col) isRowSelected = s.isCellSelected(index, col);
    }

    c.fill_rect(x, dataRowY, WIDTH, ROW_HEIGHT,
                row_bg_color(index, s.cursorRow, s.playbackRow, s.isPlaying, isRowSelected, t));

    const int textY = dataRowY + TEXT_PADDING;

    const Argb stepColor = (index == s.cursorRow && s.cursorColumn == 0) ? t.textCursor
                           : (index % 4 == 0)                            ? t.textParam
                                                                         : t.textEmpty;
    c.draw_text(hex1(index), stepX, textY, stepColor, CHAR_SPACING, FONT_SCALE);

    const auto cur = [&](int col) { return index == s.cursorRow && s.cursorColumn == col; };
    const auto sel = [&](int col) { return s.selectionMode && s.isCellSelected(index, col); };

    draw_cell(c, isEmpty ? "--" : hex2(phraseRef), phX, textY, cur(1), sel(1), isEmpty, t.textValue, t);

    // An empty slot has no transpose to show — the emptiness of BOTH cells is the phrase ref's.
    const int transposeValue = s.chain.transposeValues[static_cast<size_t>(index)];
    draw_cell(c, isEmpty ? "--" : hex2(transposeValue), tspX, textY, cur(2), sel(2), isEmpty,
              t.textParam, t);
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

    r.modified = (action.type != ActionType::NONE);
    return r;
}

}  // namespace pt::ui
