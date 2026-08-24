#include "ui/modules/groove_editor.h"

#include "songcore/timing.h"
#include "ui/helpers.h"

namespace pt::ui {

void GrooveModule::draw(Canvas& c, int x, int y, const GrooveState& s) const {
    const Theme&            t      = s.theme;
    const songcore::Groove& groove = s.groove;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    const int stepX = x + 10;
    const int tickX = x + 10 + 40;

    // ── Header ───────────────────────────────────────────────────────────────────────────────────
    const int headerY = y + TEXT_PADDING;
    // The active length is the sequencer's own answer (songcore/timing.h) — the steps before the first
    // −1. The UI must not re-derive it: a second definition of "where does this groove end" is exactly
    // the kind of drift that makes the picture disagree with what you hear.
    const int activeLen = songcore::groove_active_length(groove);

    c.draw_text("GROOVE " + hex2(groove.id), x + 10, headerY, t.textTitle, CHAR_SPACING, FONT_SCALE);

    std::string lenText = std::to_string(activeLen);
    if (lenText.size() < 2) lenText = " " + lenText;  // padStart(2, ' ')
    c.draw_text("LEN:" + lenText, x + WIDTH - 130, headerY, t.textParam, CHAR_SPACING, FONT_SCALE);

    // ── Column header ────────────────────────────────────────────────────────────────────────────
    // It lights for the column the cursor is in, as the other grids' headers do. With one value
    // column that is nearly always lit — the point is that GROOVE reads the same as PHRASE.
    c.draw_text("TIC", tickX, y + ROW_HEIGHT + 14 + TEXT_PADDING,
                header_color(s.cursorColumn, 1, 1, t), CHAR_SPACING, FONT_SCALE);

    // ── 16 data rows ─────────────────────────────────────────────────────────────────────────────
    const int dataStartY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + TEXT_PADDING;

    for (int index = 0; index < 16; ++index) {
        const int  tickValue   = groove.steps[static_cast<size_t>(index)];
        const int  rowY        = dataStartY + (index * ROW_HEIGHT);  // the TEXT y, not the row's top
        const bool isCursor    = (index == s.cursorRow);
        const bool isEndMarker = (tickValue == -1);
        const bool isPastEnd   = (index >= activeLen);  // past the first −1: inactive, drawn dim

        // The cursor is a background under ONE cell, as on every other grid; the row number lights
        // across the cursor row to say which row, the header to say which column.
        draw_cell(c, hex1(index), stepX, rowY, isCursor && s.cursorColumn == 0,
                  /*is_selected=*/false, /*is_empty=*/false,
                  isCursor ? t.textCursor : t.textEmpty, t);

        draw_cell(c, isEndMarker ? "--" : hex2(tickValue), tickX, rowY,
                  isCursor && s.cursorColumn == 1, /*is_selected=*/false,
                  /*is_empty=*/isEndMarker || isPastEnd, t.textValue, t);
    }
}

CursorContext GrooveModule::cursor_context(const GrooveState& s) const {
    switch (s.cursorColumn) {
        case 0: return cc::read_only();
        case 1: {
            const int tickValue = s.groove.steps[static_cast<size_t>(s.cursorRow)];
            return cc::hex_byte(tickValue, /*min=*/0,  // 00 = skip the step
                                /*max=*/255,
                                /*empty_value=*/-1,  // −1 = the end-of-pattern marker
                                /*can_delete=*/tickValue != -1,
                                /*can_insert=*/tickValue == -1);
        }
        default: return cc::none();
    }
}

GrooveInputResult GrooveModule::handle_input(songcore::Groove& groove, int cursor_row,
                                             int cursor_column, const InputAction& action) const {
    GrooveInputResult r;

    if (cursor_column == 1) {
        int& step = groove.steps[static_cast<size_t>(cursor_row)];
        switch (action.type) {
            case ActionType::SET_VALUE:
                step = action.value < 0 ? 0 : (action.value > 255 ? 255 : action.value);
                break;
            case ActionType::DELETE:
                step = -1;  // A+B: back to the end-of-pattern marker
                break;
            case ActionType::INSERT_DEFAULT:
                step = 0x0C;  // A on "--": the standard 12 tics per step
                break;
            default:
                break;
        }
    }

    r.modified = (action.type != ActionType::NONE);
    return r;
}

}  // namespace pt::ui
