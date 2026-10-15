#include "ui/modules/instrument_pool.h"

#include "ui/helpers.h"

namespace pt::ui {

using songcore::Instrument;

namespace {

constexpr int VISIBLE_ROWS   = 16;
constexpr int NAME_MAX_CHARS = 12;

// Column offsets; the block sits left of centre so the selected row's EQ cell has room for its ">".
constexpr int ID_X   = 14;
constexpr int NAME_X = 56;
constexpr int VOL_X  = 297;
constexpr int REV_X  = 347;
constexpr int DEL_X  = 397;
constexpr int EQ_X   = 447;


}  // namespace

void InstrumentPoolModule::draw(Canvas& c, int x, int y, const InstrumentPoolState& s) const {
    const Theme& t     = s.theme;
    const auto&  pool  = s.project.instruments;
    const int    count = static_cast<int>(pool.size());

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    c.draw_text("INST.POOL", x + ID_X, y + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);

    // ── USED RAM, in the gap the title leaves — PROJECT's total, behind the same flag ────────────
    if (s.caps.debug) {
        c.draw_text("RAM", x + RAM_LABEL_X, y + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(megabytes_str(s.sampleRamBytes), x + RAM_VALUE_X, y + TEXT_PADDING, t.textValue,
                    CHAR_SPACING, FONT_SCALE);
    }

    // The headers say which column: the cursor is one cell, and four columns look alike.
    const int  headerY = y + ROW_HEIGHT + 14;
    const auto head    = [&](int col) { return header_color(s.cursorColumn, col, col, t); };
    c.draw_text("# ",   x + ID_X,   headerY, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("NAME", x + NAME_X, headerY, head(0), CHAR_SPACING, FONT_SCALE);
    c.draw_text("V",    x + VOL_X,  headerY, head(1), CHAR_SPACING, FONT_SCALE);
    c.draw_text("RV",   x + REV_X,  headerY, head(2), CHAR_SPACING, FONT_SCALE);
    c.draw_text("DE",   x + DEL_X,  headerY, head(3), CHAR_SPACING, FONT_SCALE);
    c.draw_text("EQ",   x + EQ_X,   headerY, head(4), CHAR_SPACING, FONT_SCALE);

    // The selected row is kept centred; no scroll state.
    const int maxScroll = (count - VISIBLE_ROWS) < 0 ? 0 : (count - VISIBLE_ROWS);
    const int scroll    = clamp(s.selectedInstrument - VISIBLE_ROWS / 2, 0, maxScroll);

    const int dataTop = y + ROW_HEIGHT + 14 + ROW_HEIGHT;
    for (int i = 0; i < VISIBLE_ROWS; ++i) {
        const int slot = scroll + i;
        if (slot >= count) break;
        draw_row(c, x, dataTop + i * ROW_HEIGHT, slot, pool[static_cast<size_t>(slot)], s, t);
    }
}

void InstrumentPoolModule::draw_row(Canvas& c, int x, int rowY, int slot, const Instrument& ins,
                                    const InstrumentPoolState& s, const Theme& t) const {
    const bool selected = (slot == s.selectedInstrument);

    const int textY = rowY + TEXT_PADDING;

    const auto on_col = [&](int col) { return selected && s.cursorColumn == col; };

    // The slot number lights across the selected row.
    c.draw_text(hex2(slot), x + ID_X, textY, selected ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING,
                FONT_SCALE);

    // An unnamed slot draws a dim underscore run, so an empty pool reads as blanks, not 128 "INSTxx".
    const bool        unnamed = songcore::instrument_has_default_name(ins);
    const std::string name    = unnamed ? std::string(NAME_MAX_CHARS, '_')
                                        : Canvas::clip_text(ins.name, NAME_MAX_CHARS);
    draw_cursor_cell(c, name, x + NAME_X, textY, on_col(0),
                     unnamed ? t.textEmpty : t.textValue, t);

    draw_cursor_cell(c, hex2(ins.volume),     x + VOL_X, textY, on_col(1), t.textValue, t);
    draw_cursor_cell(c, hex2(ins.reverbSend), x + REV_X, textY, on_col(2), t.textValue, t);
    draw_cursor_cell(c, hex2(ins.delaySend),  x + DEL_X, textY, on_col(3), t.textValue, t);

    // The ">" only on the selected row: the one with room, and the one that can open it.
    draw_eq_cell(c, x + EQ_X, textY, ins.eqSlot, selected && s.cursorColumn == 4, t,
                 /*show_arrow=*/selected);
}

// ─── Cursor context ──────────────────────────────────────────────────────────────────────────────

CursorContext InstrumentPoolModule::cursor_context(const InstrumentPoolState& s) const {
    const Instrument& ins = s.project.instruments[static_cast<size_t>(s.selectedInstrument)];
    switch (s.cursorColumn) {
        case 1: return cc::plain_byte(ins.volume, 0xFF);
        case 2: return cc::plain_byte(ins.reverbSend, 0x00);
        case 3: return cc::plain_byte(ins.delaySend, 0x00);
        case 4: return cc::hex_byte(ins.eqSlot < 0 ? 0 : ins.eqSlot, /*min=*/0, /*max=*/127,
                                    /*empty_value=*/-1, /*can_delete=*/ins.eqSlot >= 0,
                                    /*can_insert=*/ins.eqSlot < 0);
        // NAME: A loads a source, A+B clears — the dispatcher's. read_only() keeps the generic edits off.
        default: return cc::read_only();
    }
}

bool InstrumentPoolModule::handle_input(Instrument& ins, int cursor_column,
                                        const InputAction& action) const {
    const bool isSet = (action.type == ActionType::SET_VALUE);

    switch (cursor_column) {
        case 1: if (isSet) { ins.volume     = clamp(action.value, 0, 255); return true; } break;
        case 2: if (isSet) { ins.reverbSend = clamp(action.value, 0, 255); return true; } break;
        case 3: if (isSet) { ins.delaySend  = clamp(action.value, 0, 255); return true; } break;
        case 4: return apply_optional_byte(ins.eqSlot, 127, action);
        default: break;
    }
    return false;
}

}  // namespace pt::ui
