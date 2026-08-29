#include "ui/modules/instrument_pool.h"

#include "ui/helpers.h"

namespace pt::ui {

using songcore::Instrument;

namespace {

constexpr int VISIBLE_ROWS   = 16;
constexpr int NAME_MAX_CHARS = 12;

// Column offsets from the module's left edge. The module is drawn at screen-x 10 and CLIPPED at
// screen-x 509 (ui/layout.h), so the visible table spans 10..509 — the four value columns are packed
// 50px apart and the block sits left of centre so the EQ cell on the selected row still has room for
// its trailing ">" before the clip.
constexpr int ID_X   = 14;
constexpr int NAME_X = 56;
constexpr int VOL_X  = 297;
constexpr int REV_X  = 347;
constexpr int DEL_X  = 397;
constexpr int EQ_X   = 447;

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

void InstrumentPoolModule::draw(Canvas& c, int x, int y, const InstrumentPoolState& s) const {
    const Theme& t     = s.theme;
    const auto&  pool  = s.project.instruments;
    const int    count = static_cast<int>(pool.size());

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    c.draw_text("INST.POOL", x + ID_X, y + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);

    // ── USED RAM, in the gap the title leaves ────────────────────────────────────────────────────
    // The same total PROJECT shows, on the screen where the samples that make it up are listed. It
    // is a readout, not a row: the cursor never reaches it and nothing here can edit it.
    //
    // ⚠️ Gated with PROJECT's pair, on the same flag and for the same reason (project_editor.cpp) —
    // one number on two screens must appear and disappear on both at once, or a release build hides
    // a figure the manual could still point at.
    if (s.caps.debug) {
        c.draw_text("RAM", x + RAM_LABEL_X, y + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(megabytes_str(s.sampleRamBytes), x + RAM_VALUE_X, y + TEXT_PADDING, t.textValue,
                    CHAR_SPACING, FONT_SCALE);
    }

    // The headers say WHICH COLUMN the cursor is in, as a grid's do — the cursor itself is one cell
    // wide here, and four of the five columns are two hex digits that look alike.
    const int  headerY = y + ROW_HEIGHT + 14;
    const auto head    = [&](int col) { return header_color(s.cursorColumn, col, col, t); };
    c.draw_text("# ",   x + ID_X,   headerY, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("NAME", x + NAME_X, headerY, head(0), CHAR_SPACING, FONT_SCALE);
    c.draw_text("V",    x + VOL_X,  headerY, head(1), CHAR_SPACING, FONT_SCALE);
    c.draw_text("RV",   x + REV_X,  headerY, head(2), CHAR_SPACING, FONT_SCALE);
    c.draw_text("DE",   x + DEL_X,  headerY, head(3), CHAR_SPACING, FONT_SCALE);
    c.draw_text("EQ",   x + EQ_X,   headerY, head(4), CHAR_SPACING, FONT_SCALE);

    // The scroll is DERIVED, not stored: the selected row is kept centred, so there is no scroll state
    // to get out of sync with the selection. (Contrast SONG, whose viewport is its own field because
    // its cursor and its playhead move independently.)
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

    // The slot number is this table's row-number gutter: it lights across the whole selected row, and
    // it is what says which row while the cursor itself covers one cell of it.
    c.draw_text(hex2(slot), x + ID_X, textY, selected ? t.textCursor : t.textParam, CHAR_SPACING,
                FONT_SCALE);

    // An unnamed slot draws a dim underscore run the full width of the name column, so an empty pool
    // reads as a list of blanks rather than as 128 rows of "INST00, INST01, …" that all look occupied.
    const bool        unnamed = songcore::instrument_has_default_name(ins);
    const std::string name    = unnamed ? std::string(NAME_MAX_CHARS, '_')
                                        : Canvas::clip_text(ins.name, NAME_MAX_CHARS);
    draw_cursor_cell(c, name, x + NAME_X, textY, on_col(0),
                     unnamed ? t.textEmpty : t.textValue, t);

    draw_cursor_cell(c, hex2(ins.volume),     x + VOL_X, textY, on_col(1), t.textValue, t);
    draw_cursor_cell(c, hex2(ins.reverbSend), x + REV_X, textY, on_col(2), t.textValue, t);
    draw_cursor_cell(c, hex2(ins.delaySend),  x + DEL_X, textY, on_col(3), t.textValue, t);

    // The ">" only on the selected row — it is the one row with space for it before the clip, and the
    // one row where the cell can actually be opened.
    draw_eq_cell(c, x + EQ_X, textY, ins.eqSlot, selected && s.cursorColumn == 4, t,
                 /*show_arrow=*/selected);
}

// ─── Cursor context ──────────────────────────────────────────────────────────────────────────────

CursorContext InstrumentPoolModule::cursor_context(const InstrumentPoolState& s) const {
    const Instrument& ins = s.project.instruments[static_cast<size_t>(s.selectedInstrument)];
    switch (s.cursorColumn) {
        case 1: return cc::hex_byte(ins.volume, 0, 255, -1, false, false, false, /*def=*/0xFF);
        case 2: return cc::hex_byte(ins.reverbSend, 0, 255, -1, false, false, false, /*def=*/0x00);
        case 3: return cc::hex_byte(ins.delaySend, 0, 255, -1, false, false, false, /*def=*/0x00);
        case 4: return cc::hex_byte(ins.eqSlot < 0 ? 0 : ins.eqSlot, /*min=*/0, /*max=*/127,
                                    /*empty_value=*/-1, /*can_delete=*/ins.eqSlot >= 0,
                                    /*can_insert=*/ins.eqSlot < 0);
        // NAME: A loads a source into an empty slot and A+B clears it — both the dispatcher's, neither
        // a value edit. read_only() is what stops the five generic handlers touching the row.
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
        case 4:
            if (isSet)                                          { ins.eqSlot = clamp(action.value, 0, 127); return true; }
            if (action.type == ActionType::DELETE)              { ins.eqSlot = -1; return true; }
            if (action.type == ActionType::INSERT_DEFAULT)      { ins.eqSlot = 0;  return true; }
            break;
        default: break;
    }
    return false;
}

}  // namespace pt::ui
