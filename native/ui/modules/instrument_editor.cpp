#include "ui/modules/instrument_editor.h"

#include "ui/helpers.h"
#include "ui/instrument_row_layout.h"

namespace pt::ui {

using songcore::Instrument;
using songcore::InstrumentType;
using songcore::Note;

namespace {

// Fixed column offsets for the TRIPLE rows, relative to the module's left edge. They are NOT derived
// from the two standard columns: three label+value pairs do not fit on the same grid two do, so the
// Kotlin pins them and so does this.
constexpr int TRIPLE_V1 = 90;   // first value  (ROOT / VOL)
constexpr int TRIPLE_N2 = 185;  // second label (DETUNE / SLICE)
constexpr int TRIPLE_V2 = 305;
constexpr int TRIPLE_N3 = 368;  // third label  (TIC / PAN)
constexpr int TRIPLE_V3 = 438;

// The source section's buttons. LOAD lines up with the DRIVE/CRUSH/DWNSMPL value column.
constexpr int SRC_LOAD = 150;
constexpr int SRC_EDIT = 335;

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void InstrumentEditorModule::draw(Canvas& c, int x, int y, const InstrumentEditorState& s) const {
    const Theme&      t   = s.theme;
    const Instrument& ins = s.instrument;
    const bool        sf  = s.is_soundfont();

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    const int nameX  = x + 10;
    const int valueX = x + 150;

    int rowY       = y + TEXT_PADDING;
    int currentRow = 0;

    c.draw_text("INSTRUMENT " + hex2(ins.id), nameX, rowY, t.textTitle, CHAR_SPACING, FONT_SCALE);
    rowY += ROW_HEIGHT + 14;

    // ── 0: TYPE + LOAD + SAVE ────────────────────────────────────────────────────────────────────
    draw_type_load_row(c, x, rowY, nameX, valueX, ins, s.cursorRow, s.cursorColumn, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    // ── 1: NAME ──────────────────────────────────────────────────────────────────────────────────
    draw_name_row(c, x, rowY, nameX, valueX, s, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    // ── 2: ROOT + DETUNE + TIC ───────────────────────────────────────────────────────────────────
    draw_triple_row(c, x, rowY, nameX,
                    "ROOT",   note_name(ins.root),
                    "DETUNE", hex2(ins.detune),
                    "TIC",    hex2(ins.tableTicRate),
                    s.cursorRow, s.cursorColumn, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    // ── 3: VOL + PAN (SF) / VOL + SLICE + PAN (sampler) ──────────────────────────────────────────
    if (sf) {
        draw_dual_row(c, x, rowY, nameX, valueX, "VOL", hex2(ins.volume), "PAN", hex2(ins.pan),
                      s.cursorRow, s.cursorColumn, currentRow, t);
    } else {
        draw_triple_row(c, x, rowY, nameX,
                        "VOL",   hex2(ins.volume),
                        "SLICE", slice_modes()[static_cast<size_t>(clamp(ins.slicingMode, 0, 2))],
                        "PAN",   hex2(ins.pan),
                        s.cursorRow, s.cursorColumn, currentRow, t);
    }
    rowY += ROW_HEIGHT; currentRow++;

    // ── 4: spacer ────────────────────────────────────────────────────────────────────────────────
    rowY += ROW_HEIGHT; currentRow++;

    // ── 5: the source section ────────────────────────────────────────────────────────────────────
    draw_section_source_row(c, x, rowY, nameX, s.cursorRow, s.cursorColumn, currentRow, sf, t);
    rowY += ROW_HEIGHT; currentRow++;

    // ── 6 (SF only): PRESET ──────────────────────────────────────────────────────────────────────
    if (sf) {
        // "3/128 Acoustic Grand" — the 1-based position in the SF2's list, then its name. "--" when
        // the file has no presets (or none is loaded), which is also what an empty SF slot shows.
        const std::string num = (s.sfPresetCount > 0)
                                    ? std::to_string(s.sfPresetIndex + 1) + "/" +
                                          std::to_string(s.sfPresetCount)
                                    : "--";
        const std::string value = s.sfPresetName.empty() ? num : num + " " + s.sfPresetName;

        const bool onRow = (s.cursorRow == currentRow);
        if (onRow) draw_row_bg(c, x, rowY, t);
        c.draw_text("PRESET", nameX, rowY + TEXT_PADDING, onRow ? t.textCursor : t.textParam,
                    CHAR_SPACING, FONT_SCALE);
        c.draw_text(value, valueX, rowY + TEXT_PADDING, onRow ? t.textCursor : t.textValue,
                    CHAR_SPACING, FONT_SCALE);
        rowY += ROW_HEIGHT; currentRow++;
    }

    // ── 6/7: spacer ──────────────────────────────────────────────────────────────────────────────
    rowY += ROW_HEIGHT; currentRow++;

    // ── The DSP block: DRIVE+FILTER, CRUSH+FREQ, DWNSMPL+RES ─────────────────────────────────────
    draw_dual_row(c, x, rowY, nameX, valueX, "DRIVE", hex2(ins.drive), "FILTER", ins.filterType,
                  s.cursorRow, s.cursorColumn, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    draw_dual_row(c, x, rowY, nameX, valueX, "CRUSH", hex1(ins.crush), "FREQ", hex2(ins.filterCut),
                  s.cursorRow, s.cursorColumn, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    draw_dual_row(c, x, rowY, nameX, valueX, "DWNSMPL", hex1(ins.downsample), "RES",
                  hex2(ins.filterRes), s.cursorRow, s.cursorColumn, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    // ── spacer ───────────────────────────────────────────────────────────────────────────────────
    rowY += ROW_HEIGHT; currentRow++;

    // ── The type-specific tail ───────────────────────────────────────────────────────────────────
    if (sf) {
        // The SoundFont has no sample window and no loop, so its sends get a row each and the screen
        // ends at EQ.
        const auto on = [&](int col) { return s.cursorRow == currentRow && s.cursorColumn == col; };

        draw_parameter_row(c, x, rowY, nameX, valueX, "REV", hex2(ins.reverbSend), on(0), on(1), t);
        rowY += ROW_HEIGHT; currentRow++;

        draw_parameter_row(c, x, rowY, nameX, valueX, "DEL", hex2(ins.delaySend), on(0), on(1), t);
        rowY += ROW_HEIGHT; currentRow++;

        draw_eq_row(c, x, rowY, nameX, valueX, ins.eqSlot, s.cursorRow, s.cursorColumn, currentRow, t);

    } else {
        draw_dual_row(c, x, rowY, nameX, valueX, "REV", hex2(ins.reverbSend), "DEL",
                      hex2(ins.delaySend), s.cursorRow, s.cursorColumn, currentRow, t);
        rowY += ROW_HEIGHT; currentRow++;

        draw_eq_row(c, x, rowY, nameX, valueX, ins.eqSlot, s.cursorRow, s.cursorColumn, currentRow, t);
        rowY += ROW_HEIGHT; currentRow++;

        draw_dual_row(c, x, rowY, nameX, valueX, "LOOP", ins.loopMode, "START", hex2(ins.sampleStart),
                      s.cursorRow, s.cursorColumn, currentRow, t);
        rowY += ROW_HEIGHT; currentRow++;

        draw_dual_row(c, x, rowY, nameX, valueX, "LOOP ST", hex2(ins.loopStart), "END",
                      hex2(ins.sampleEnd), s.cursorRow, s.cursorColumn, currentRow, t);
        rowY += ROW_HEIGHT; currentRow++;

        draw_dual_row(c, x, rowY, nameX, valueX, "LOOP END", hex2(ins.loopEnd), "REVERSE",
                      ins.reverse ? "on" : "off", s.cursorRow, s.cursorColumn, currentRow, t);
    }

    // Status messages ("SF LOADED", "SRC MISSING") are the global overlay's, drawn on the visualizer
    // header — not inside this module. Same split as the Kotlin.
}

// ─── Draw helpers ────────────────────────────────────────────────────────────────────────────────

void InstrumentEditorModule::draw_row_bg(Canvas& c, int x, int y, const Theme& t) const {
    c.fill_rect(x, y, WIDTH, ROW_HEIGHT, t.rowCursor);
}

void InstrumentEditorModule::draw_parameter_row(Canvas& c, int x, int y, int name_x, int value_x,
                                                const std::string& name, const std::string& value,
                                                bool cursor_on_name, bool cursor_on_value,
                                                const Theme& t) const {
    const int  textY = y + TEXT_PADDING;
    const bool onRow = cursor_on_name || cursor_on_value;
    if (onRow) draw_row_bg(c, x, y, t);

    c.draw_text(name, name_x, textY, onRow ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text(value, value_x, textY, cursor_on_value ? t.textCursor : t.textValue, CHAR_SPACING,
                FONT_SCALE);
}

void InstrumentEditorModule::draw_dual_row(Canvas& c, int x, int y, int name_x, int value_x,
                                           const std::string& n1, const std::string& v1,
                                           const std::string& n2, const std::string& v2,
                                           int cursor_row, int cursor_column, int this_row,
                                           const Theme& t) const {
    const int  textY = y + TEXT_PADDING;
    const bool onRow = (cursor_row == this_row);
    if (onRow) draw_row_bg(c, x, y, t);

    const int name2X  = name_x + 230;
    const int value2X = value_x + 220;

    const bool c1 = onRow && cursor_column == 1;
    const bool c3 = onRow && cursor_column == 3;

    c.draw_text(n1, name_x,  textY, c1 ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text(v1, value_x, textY, c1 ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
    c.draw_text(n2, name2X,  textY, c3 ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text(v2, value2X, textY, c3 ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
}

void InstrumentEditorModule::draw_triple_row(Canvas& c, int x, int y, int name_x,
                                             const std::string& n1, const std::string& v1,
                                             const std::string& n2, const std::string& v2,
                                             const std::string& n3, const std::string& v3,
                                             int cursor_row, int cursor_column, int this_row,
                                             const Theme& t) const {
    const int  textY = y + TEXT_PADDING;
    const bool onRow = (cursor_row == this_row);
    if (onRow) draw_row_bg(c, x, y, t);

    const bool c1 = onRow && cursor_column == 1;
    const bool c3 = onRow && cursor_column == 3;
    const bool c5 = onRow && cursor_column == 5;

    c.draw_text(n1, name_x,        textY, c1 ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text(v1, x + TRIPLE_V1, textY, c1 ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
    c.draw_text(n2, x + TRIPLE_N2, textY, c3 ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text(v2, x + TRIPLE_V2, textY, c3 ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
    c.draw_text(n3, x + TRIPLE_N3, textY, c5 ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text(v3, x + TRIPLE_V3, textY, c5 ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
}

void InstrumentEditorModule::draw_type_load_row(Canvas& c, int x, int y, int name_x, int value_x,
                                                const Instrument& ins, int cursor_row,
                                                int cursor_column, int this_row,
                                                const Theme& t) const {
    const int  textY = y + TEXT_PADDING;
    const bool onRow = (cursor_row == this_row);
    if (onRow) draw_row_bg(c, x, y, t);

    const int loadX = name_x + 325;
    const int saveX = value_x + 270;

    const bool c1 = onRow && cursor_column == 1;
    const bool c2 = onRow && cursor_column == 2;
    const bool c3 = onRow && cursor_column == 3;

    const char* typeText =
        (ins.instrumentType == InstrumentType::SOUNDFONT) ? "soundfont" : "sampler";

    c.draw_text("TYPE",  name_x,  textY, c1 ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text(typeText, value_x, textY, c1 ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
    // LOAD and SAVE are BUTTONS — they are `textValue` even unselected, because a dim label would read
    // as a parameter name rather than as something you can press.
    c.draw_text("LOAD",  loadX,   textY, c2 ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
    c.draw_text("SAVE",  saveX,   textY, c3 ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
}

void InstrumentEditorModule::draw_name_row(Canvas& c, int x, int y, int name_x, int value_x,
                                           const InstrumentEditorState& s, int this_row,
                                           const Theme& t) const {
    const int  textY = y + TEXT_PADDING;
    const bool onRow = (s.cursorRow == this_row);
    if (onRow) draw_row_bg(c, x, y, t);

    c.draw_text("NAME", name_x, textY, onRow ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);

    // "______" until a sample or SF2 is loaded (or the user names the slot) — the auto-generated
    // "INSTxx" is treated as "unset", never shown. songcore::instrument_has_default_name is the single
    // test for that, everywhere.
    const std::string display = songcore::instrument_has_default_name(s.instrument)
                                    ? "______"
                                    : s.instrument.name;
    c.draw_text(display, value_x, textY, onRow ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
}

void InstrumentEditorModule::draw_section_source_row(Canvas& c, int x, int y, int name_x,
                                                     int cursor_row, int cursor_column, int this_row,
                                                     bool is_soundfont, const Theme& t) const {
    const int  textY = y + TEXT_PADDING;
    const bool onRow = (cursor_row == this_row);
    if (onRow) draw_row_bg(c, x, y, t);

    // The loaded file's name lives on the NAME row, not here — this row is only its buttons.
    c.draw_text(is_soundfont ? "SF" : "SMPL", name_x, textY, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("LOAD", x + SRC_LOAD, textY,
                (onRow && cursor_column == 2) ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);

    // No EDIT for a SoundFont: there is no waveform to edit. The cursor is capped at column 2 there
    // for the same reason (ui/cursor_move.h).
    if (!is_soundfont) {
        c.draw_text("EDIT >", x + SRC_EDIT, textY,
                    (onRow && cursor_column == 3) ? t.textCursor : t.textValue, CHAR_SPACING,
                    FONT_SCALE);
    }
}

void InstrumentEditorModule::draw_eq_row(Canvas& c, int x, int y, int name_x, int value_x, int eq_slot,
                                         int cursor_row, int cursor_column, int this_row,
                                         const Theme& t) const {
    const int  textY = y + TEXT_PADDING;
    const bool onRow = (cursor_row == this_row);
    if (onRow) draw_row_bg(c, x, y, t);

    c.draw_text("EQ", name_x, textY, onRow ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    // The shared painter, so this cell cannot drift from the pool's or the mixer's: "--" when
    // unassigned, and a trailing ">" that says the cell opens the EQ editor.
    draw_eq_cell(c, value_x, textY, eq_slot, onRow && cursor_column == 1, t);
}

// ─── Cursor context ──────────────────────────────────────────────────────────────────────────────

CursorContext InstrumentEditorModule::cursor_context(const InstrumentEditorState& s) const {
    const Instrument& ins = s.instrument;
    const bool        sf  = s.is_soundfont();
    const int         off = sf ? 1 : 0;   // the SoundFont's PRESET row pushes everything below it down
    const int         row = s.cursorRow;
    const int         col = s.cursorColumn;

    // Rows 0 and 1 are the TYPE and NAME rows. They are READ_ONLY *to the generic handlers* — A+UP on
    // TYPE toggles SAMPLER↔SOUNDFONT and A on NAME opens the name editor, and both are dispatcher
    // business (a type change frees a sample; a name is text, not a number). Read-only here means "the
    // five generic handlers must not touch this", not "nothing happens".
    if (row == 0 || row == 1) return cc::read_only();

    if (row == 2) {  // ROOT + DETUNE + TIC
        switch (col) {
            case 1: {
                const bool empty = (ins.root == Note::EMPTY());
                return cc::note(empty ? 0 : songcore::note_to_midi(ins.root), empty);
            }
            case 3: return cc::hex_byte(ins.detune, 0, 255, -1, false, false, false, /*def=*/0x80);
            case 5: return cc::hex_byte(ins.tableTicRate, 0, 255, -1, false, false, false, /*def=*/0x06);
            default: return cc::none();
        }
    }

    if (row == 3) {  // VOL + PAN (SF) / VOL + SLICE + PAN (sampler)
        if (sf) {
            switch (col) {
                case 1: return cc::hex_byte(ins.volume, 0, 255, -1, false, false, false, /*def=*/0xFF);
                case 3: return cc::hex_byte(ins.pan, 0, 255, -1, false, false, false, /*def=*/0x80);
                default: return cc::none();
            }
        }
        switch (col) {
            case 1: return cc::hex_byte(ins.volume, 0, 255, -1, false, false, false, /*def=*/0xFF);
            case 3: return cc::toggle_ternary(slice_modes()[static_cast<size_t>(clamp(ins.slicingMode, 0, 2))],
                                              slice_modes());
            case 5: return cc::hex_byte(ins.pan, 0, 255, -1, false, false, false, /*def=*/0x80);
            default: return cc::none();
        }
    }

    if (row == 4) return cc::none();       // spacer
    if (row == 5) return cc::read_only();  // the LOAD / EDIT buttons — the dispatcher's, not a value

    if (sf && row == 6) {  // PRESET
        // The index range is the SF2's own list length. With no SoundFont the count is 0, so `maxIdx`
        // is 0 and stepping goes nowhere — correct, and the reason this row is drawable before a file
        // is ever opened.
        if (col != 1) return cc::none();
        const int maxIdx = (s.sfPresetCount - 1) < 0 ? 0 : (s.sfPresetCount - 1);
        return cc::hex_byte(s.sfPresetIndex, 0, maxIdx);
    }

    if (row == 6 + off) return cc::none();  // spacer

    if (row == 7 + off) {  // DRIVE + FILTER
        switch (col) {
            case 1: return cc::hex_byte(ins.drive, 0, 255, -1, false, false, false, /*def=*/0x00);
            case 3: return cc::toggle_ternary(ins.filterType, filter_types());
            default: return cc::none();
        }
    }

    if (row == 8 + off) {  // CRUSH + FREQ
        switch (col) {
            case 1: return cc::hex_nibble(ins.crush, /*def=*/0);
            case 3: return cc::hex_byte(ins.filterCut, 0, 255, -1, false, false, false, /*def=*/0x00);
            default: return cc::none();
        }
    }

    if (row == 9 + off) {  // DWNSMPL + RES
        switch (col) {
            case 1: return cc::hex_nibble(ins.downsample, /*def=*/0);
            case 3: return cc::hex_byte(ins.filterRes, 0, 255, -1, false, false, false, /*def=*/0x00);
            default: return cc::none();
        }
    }

    if (row == 10 + off) return cc::none();  // spacer

    /**
     * The EQ slot cell, shared by both tails. −1 is a genuine "no EQ", and A+B clears back to it.
     *
     * ⚠️ **`can_insert` is INERT here — a Kotlin quirk carried over deliberately, not a porting slip.**
     * `hex_byte` decides emptiness by `current == empty_value`, but the caller substitutes 0 for an
     * unassigned −1 *before* handing it over. `0 == −1` is false, so the cell is never "empty" in the
     * context's eyes and `can_insert && is_empty` collapses to false. (`can_delete` is unaffected: it
     * is gated on `!is_empty`, which is exactly what it wants.)
     *
     * The one visible consequence: **A on an unassigned EQ jumps to slot 1 — slot 0 is unreachable
     * with A** (it steps up from the substituted 0). A+B on an unassigned cell does nothing, having
     * neither a delete nor a default. Both are what the Android app does; `tools/ptinput` pins all five
     * buttons on this cell at −1, 0 and 9. The INSERT_DEFAULT arm in handle_input below is therefore
     * unreachable today, and is kept because it is what the Kotlin has — and what would make the cell
     * correct if the factory were ever fixed to pass −1 through.
     */
    const auto eq_context = [&] {
        return cc::hex_byte(ins.eqSlot < 0 ? 0 : ins.eqSlot, /*min=*/0, /*max=*/127,
                            /*empty_value=*/-1, /*can_delete=*/ins.eqSlot >= 0,
                            /*can_insert=*/ins.eqSlot < 0);
    };

    if (sf) {
        // 12 REV · 13 DEL · 14 EQ — one parameter each, so column 0 (the label) is read-only and any
        // other column is the value.
        if (row == 12) return (col == 0) ? cc::read_only()
                                         : cc::hex_byte(ins.reverbSend, 0, 255, -1, false, false, false, 0x00);
        if (row == 13) return (col == 0) ? cc::read_only()
                                         : cc::hex_byte(ins.delaySend, 0, 255, -1, false, false, false, 0x00);
        if (row == 14) return (col == 0) ? cc::read_only() : eq_context();
        return cc::none();
    }

    switch (row) {
        case 11:  // REV + DEL
            if (col == 1) return cc::hex_byte(ins.reverbSend, 0, 255, -1, false, false, false, 0x00);
            if (col == 3) return cc::hex_byte(ins.delaySend, 0, 255, -1, false, false, false, 0x00);
            return cc::none();

        case 12:  // EQ, alone
            return (col == 1) ? eq_context() : cc::none();

        case 13:  // LOOP + START
            if (col == 1) return cc::toggle_ternary(ins.loopMode, loop_modes());
            if (col == 3) return cc::hex_byte(ins.sampleStart, 0, 255, -1, false, false, false, 0x00);
            return cc::none();

        case 14:  // LOOP ST + END
            if (col == 1) return cc::hex_byte(ins.loopStart, 0, 255, -1, false, false, false, 0x00);
            if (col == 3) return cc::hex_byte(ins.sampleEnd, 0, 255, -1, false, false, false, 0xFF);
            return cc::none();

        case 15:  // LOOP END + REVERSE
            if (col == 1) return cc::hex_byte(ins.loopEnd, 0, 255, -1, false, false, false, 0xFF);
            if (col == 3) return cc::toggle_binary(ins.reverse);
            return cc::none();

        default:
            return cc::none();
    }
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

InstrumentInputResult InstrumentEditorModule::handle_input(Instrument& ins, int row, int col,
                                                           const InputAction& action) const {
    InstrumentInputResult r;
    const bool sf  = (ins.instrumentType == InstrumentType::SOUNDFONT);
    const int  off = sf ? 1 : 0;

    const bool isSet = (action.type == ActionType::SET_VALUE);
    const int  v     = action.value;

    const auto b255 = [&](int& field) { if (isSet) field = clamp(v, 0, 255); };
    const auto b15  = [&](int& field) { if (isSet) field = clamp(v, 0, 15); };

    // Rows 0/1 (TYPE, NAME) and 4/5 (spacer, source buttons) are handled by the dispatcher, exactly as
    // Kotlin leaves them to MainActivity: nothing here writes them.

    if (row == 2) {
        if (isSet && col == 1) ins.root = songcore::note_from_midi(v);
        else if (action.type == ActionType::DELETE && col == 1) ins.root = Note::C4();
        else if (col == 3) b255(ins.detune);
        else if (col == 5) b255(ins.tableTicRate);

    } else if (row == 3) {
        if (col == 1) b255(ins.volume);
        else if (col == 3) {
            if (sf) b255(ins.pan);
            else if (isSet) ins.slicingMode = clamp(v, 0, 2);
        } else if (col == 5 && !sf) b255(ins.pan);

    } else if (sf && row == 6) {
        // PRESET — the module cannot resolve this itself: the bank+preset behind index `v` live in the
        // SF2's list, which only the engine has read. Hand it back to the dispatcher.
        if (isSet && col == 1) {
            r.presetIndexChanged = true;
            r.presetIndex        = v;
        }

    } else if (row == 7 + off) {
        if (col == 1) b255(ins.drive);
        else if (col == 3 && isSet && v >= 0 && v < static_cast<int>(filter_types().size()))
            ins.filterType = filter_types()[static_cast<size_t>(v)];

    } else if (row == 8 + off) {
        if (col == 1) b15(ins.crush);
        else if (col == 3) b255(ins.filterCut);

    } else if (row == 9 + off) {
        if (col == 1) b15(ins.downsample);
        else if (col == 3) b255(ins.filterRes);

    } else if (sf && row == 12) {
        b255(ins.reverbSend);
    } else if (sf && row == 13) {
        b255(ins.delaySend);
    } else if ((sf && row == 14) || (!sf && row == 12)) {
        // The EQ slot. −1 is "no EQ", so DELETE clears to it and INSERT lands on slot 0.
        if (isSet)                                       ins.eqSlot = clamp(v, 0, 127);
        else if (action.type == ActionType::DELETE)      ins.eqSlot = -1;
        else if (action.type == ActionType::INSERT_DEFAULT) ins.eqSlot = 0;

    } else if (!sf && row == 11) {
        if (col == 1) b255(ins.reverbSend);
        else if (col == 3) b255(ins.delaySend);

    } else if (!sf && row == 13) {
        if (col == 1 && isSet && v >= 0 && v < static_cast<int>(loop_modes().size()))
            ins.loopMode = loop_modes()[static_cast<size_t>(v)];
        else if (col == 3) b255(ins.sampleStart);

    } else if (!sf && row == 14) {
        if (col == 1) b255(ins.loopStart);
        else if (col == 3) b255(ins.sampleEnd);

    } else if (!sf && row == 15) {
        if (col == 1) b255(ins.loopEnd);
        else if (col == 3 && isSet) ins.reverse = (v == 1);
    }

    r.modified = (action.type != ActionType::NONE);
    return r;
}

}  // namespace pt::ui
