#include "ui/modules/instrument_editor.h"

#include <algorithm>

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

// The TYPE row (0) and the INST PRESET row (5) each carry a label plus two buttons. Their two button
// columns are SHARED so the buttons line up vertically — TYPE's LOAD / EDIT sit directly above PRESET's
// SAVE / LOAD. TYPE's value is pulled left onto the ROOT/VOL column (TRIPLE_V1) to clear room for them.
constexpr int TYPE_VALUE = TRIPLE_V1;  // the TYPE value, under the ROOT/VOL column
constexpr int BTN_COL2   = 300;        // cursor column 2: TYPE-row LOAD, PRESET-row SAVE
constexpr int BTN_COL3   = 400;        // cursor column 3: TYPE-row EDIT, PRESET-row LOAD

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/**
 * "--" for an unset MIDI byte, the hex pair otherwise — BANK, PROG and both halves of a CC slot.
 *
 * −1 is the project's ONE "empty" convention (model.h), and it has to LOOK different from 00 on this
 * screen in particular: `midiProgram = 0` is a real program change and `-1` means send nothing at all,
 * so a cell that drew them alike would hide which of the two the user had dialled in.
 */
std::string midi_opt(int v) { return v < 0 ? "--" : hex2(v); }

}  // namespace

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void InstrumentEditorModule::draw(Canvas& c, int x, int y, const InstrumentEditorState& s) const {
    if (s.is_external()) { draw_external(c, x, y, s); return; }

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
    draw_name_row(c, rowY, nameX, valueX, s, currentRow, t);
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
        draw_dual_row(c, rowY, nameX, valueX, "VOL", hex2(ins.volume), "PAN", hex2(ins.pan),
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
    draw_section_source_row(c, x, rowY, nameX, s.cursorRow, s.cursorColumn, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    // ── 6 (SF only): PATCH ───────────────────────────────────────────────────────────────────────
    if (sf) {
        // "3/128 Acoustic Grand" — the 1-based position in the SF2's patch list, then its name. "--"
        // when the file has no patches (or none is loaded), which is also what an empty SF slot shows.
        // Labelled PATCH, not PRESET, so it is not confused with the INST PRESET (.pti) row above.
        const std::string num = (s.sfPresetCount > 0)
                                    ? std::to_string(s.sfPresetIndex + 1) + "/" +
                                          std::to_string(s.sfPresetCount)
                                    : "--";
        const std::string value = s.sfPresetName.empty() ? num : num + " " + s.sfPresetName;

        const bool onRow = (s.cursorRow == currentRow);
        c.draw_text("PATCH", nameX, rowY + TEXT_PADDING, onRow ? t.textCursor : t.textParam,
                    CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, value, valueX, rowY + TEXT_PADDING, onRow, t.textValue, t);
        rowY += ROW_HEIGHT; currentRow++;
    }

    // ── 6/7: spacer ──────────────────────────────────────────────────────────────────────────────
    rowY += ROW_HEIGHT; currentRow++;

    // ── The DSP block: DRIVE+FILTER, CRUSH+FREQ, DWNSMPL+RES ─────────────────────────────────────
    draw_dual_row(c, rowY, nameX, valueX, "DRIVE", hex2(ins.drive), "FILTER", ins.filterType,
                  s.cursorRow, s.cursorColumn, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    draw_dual_row(c, rowY, nameX, valueX, "CRUSH", hex1(ins.crush), "FREQ", hex2(ins.filterCut),
                  s.cursorRow, s.cursorColumn, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    draw_dual_row(c, rowY, nameX, valueX, "DWNSMPL", hex1(ins.downsample), "RES",
                  hex2(ins.filterRes), s.cursorRow, s.cursorColumn, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    // ── spacer ───────────────────────────────────────────────────────────────────────────────────
    rowY += ROW_HEIGHT; currentRow++;

    // ── The type-specific tail ───────────────────────────────────────────────────────────────────
    if (sf) {
        // The SoundFont has no sample window and no loop, so its sends get a row each and the screen
        // ends at EQ.
        const auto on = [&](int col) { return s.cursorRow == currentRow && s.cursorColumn == col; };

        draw_parameter_row(c, rowY, nameX, valueX, "REV", hex2(ins.reverbSend), on(0), on(1), t);
        rowY += ROW_HEIGHT; currentRow++;

        draw_parameter_row(c, rowY, nameX, valueX, "DEL", hex2(ins.delaySend), on(0), on(1), t);
        rowY += ROW_HEIGHT; currentRow++;

        draw_eq_row(c, rowY, nameX, valueX, ins.eqSlot, s.cursorRow, s.cursorColumn, currentRow, t);

    } else {
        draw_dual_row(c, rowY, nameX, valueX, "REV", hex2(ins.reverbSend), "DEL",
                      hex2(ins.delaySend), s.cursorRow, s.cursorColumn, currentRow, t);
        rowY += ROW_HEIGHT; currentRow++;

        draw_eq_row(c, rowY, nameX, valueX, ins.eqSlot, s.cursorRow, s.cursorColumn, currentRow, t);
        rowY += ROW_HEIGHT; currentRow++;

        draw_dual_row(c, rowY, nameX, valueX, "LOOP", ins.loopMode, "START", hex2(ins.sampleStart),
                      s.cursorRow, s.cursorColumn, currentRow, t);
        rowY += ROW_HEIGHT; currentRow++;

        draw_dual_row(c, rowY, nameX, valueX, "LOOP ST", hex2(ins.loopStart), "END",
                      hex2(ins.sampleEnd), s.cursorRow, s.cursorColumn, currentRow, t);
        rowY += ROW_HEIGHT; currentRow++;

        draw_dual_row(c, rowY, nameX, valueX, "LOOP END", hex2(ins.loopEnd), "REVERSE",
                      ins.reverse ? "on" : "off", s.cursorRow, s.cursorColumn, currentRow, t);
    }

    // Status messages ("SF LOADED", "SRC MISSING") are the global overlay's, drawn on the visualizer
    // header — not inside this module. Same split as the Kotlin.
}

void InstrumentEditorModule::draw_external(Canvas& c, int x, int y,
                                           const InstrumentEditorState& s) const {
    const Theme&      t   = s.theme;
    const Instrument& ins = s.instrument;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    const int nameX  = x + 10;
    const int valueX = x + 150;

    int rowY       = y + TEXT_PADDING;
    int currentRow = 0;

    c.draw_text("INSTRUMENT " + hex2(ins.id), nameX, rowY, t.textTitle, CHAR_SPACING, FONT_SCALE);
    rowY += ROW_HEIGHT + 14;

    // ── 0: TYPE (no LOAD, no EDIT — there is no source) ──────────────────────────────────────────
    draw_type_load_row(c, x, rowY, nameX, valueX, ins, s.cursorRow, s.cursorColumn, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    // ── 1: NAME ──────────────────────────────────────────────────────────────────────────────────
    draw_name_row(c, rowY, nameX, valueX, s, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    // ── 2: CHAN + BANK ───────────────────────────────────────────────────────────────────────────
    // CHAN is shown 1-16 and stored 0-15 — the MIDI convention. The +1 lives here and the −1 lives in
    // handle_input; nothing between them ever sees the display number. BANK is FOUR hex digits: it is
    // 14-bit, and it shares this row with one cell rather than two so the number has room to print.
    draw_dual_row(c, rowY, nameX, valueX,
                  "CHAN", dec2(clamp(ins.midiChannel, 0, 15) + 1),
                  "BANK", ins.midiBank < 0 ? "----" : hex4(ins.midiBank),
                  s.cursorRow, s.cursorColumn, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    // ── 3: PROG + LEN ────────────────────────────────────────────────────────────────────────────
    draw_dual_row(c, rowY, nameX, valueX, "PROG", midi_opt(ins.midiProgram), "LEN",
                  hex2(ins.midiLen), s.cursorRow, s.cursorColumn, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    // ── 4: spacer ────────────────────────────────────────────────────────────────────────────────
    rowY += ROW_HEIGHT; currentRow++;

    // ── 5: INST PRESET ───────────────────────────────────────────────────────────────────────────
    // A .pti round-trips an EXTERNAL patch as readily as a sampler's: the preset carries the whole
    // Instrument through the same emit/parse, so the MIDI fields came along for free with B1.
    draw_section_source_row(c, x, rowY, nameX, s.cursorRow, s.cursorColumn, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    // ── 6: spacer ────────────────────────────────────────────────────────────────────────────────
    rowY += ROW_HEIGHT; currentRow++;

    // ── 7: VOL + PAN ─────────────────────────────────────────────────────────────────────────────
    // The two that survive the cable: VOL scales the note-on velocity (there is no gain stage on our
    // side of it) and PAN leaves as CC 10. Everything else in the sampler's DSP block has no meaning.
    draw_dual_row(c, rowY, nameX, valueX, "VOL", hex2(ins.volume), "PAN", hex2(ins.pan),
                  s.cursorRow, s.cursorColumn, currentRow, t);
    rowY += ROW_HEIGHT; currentRow++;

    // ── 8-11: the four CC slots, number then default value ───────────────────────────────────────
    static const char* const CC_LABELS[] = {"CC A", "CC B", "CC C", "CC D"};
    const int ccRows = std::min(songcore::MIDI_CC_SLOTS, static_cast<int>(ins.midiCC.size()));
    for (int i = 0; i < ccRows; ++i) {
        const songcore::MidiCcSlot& slot = ins.midiCC[static_cast<size_t>(i)];
        draw_dual_row(c, rowY, nameX, valueX, CC_LABELS[i], midi_opt(slot.cc), "VAL",
                      midi_opt(slot.value), s.cursorRow, s.cursorColumn, currentRow, t);
        rowY += ROW_HEIGHT; currentRow++;
    }
}

// ─── Draw helpers ────────────────────────────────────────────────────────────────────────────────

// ⚠️ NO ROW BACKGROUND ANYWHERE ON THIS SCREEN. The cursor is the CELL it is on, as it is on every
// grid; what says WHICH ROW is the label beside it, which takes `textCursor` while the cursor is
// anywhere along its row. A label is only a CELL — only filled — where the cursor can land on it.

void InstrumentEditorModule::draw_parameter_row(Canvas& c, int y, int name_x, int value_x,
                                                const std::string& name, const std::string& value,
                                                bool cursor_on_name, bool cursor_on_value,
                                                const Theme& t) const {
    const int  textY = y + TEXT_PADDING;
    const bool onRow = cursor_on_name || cursor_on_value;

    draw_cursor_cell(c, name, name_x, textY, cursor_on_name,
                     onRow ? t.textCursor : t.textParam, t);
    draw_cursor_cell(c, value, value_x, textY, cursor_on_value, t.textValue, t);
}

void InstrumentEditorModule::draw_dual_row(Canvas& c, int y, int name_x, int value_x,
                                           const std::string& n1, const std::string& v1,
                                           const std::string& n2, const std::string& v2,
                                           int cursor_row, int cursor_column, int this_row,
                                           const Theme& t) const {
    const int  textY = y + TEXT_PADDING;
    const bool onRow = (cursor_row == this_row);

    const int name2X  = name_x + 230;
    const int value2X = value_x + 220;

    const bool c1 = onRow && cursor_column == 1;
    const bool c3 = onRow && cursor_column == 3;

    c.draw_text(n1, name_x, textY, c1 ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    draw_cursor_cell(c, v1, value_x, textY, c1, t.textValue, t);
    c.draw_text(n2, name2X, textY, c3 ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    draw_cursor_cell(c, v2, value2X, textY, c3, t.textValue, t);
}

void InstrumentEditorModule::draw_triple_row(Canvas& c, int x, int y, int name_x,
                                             const std::string& n1, const std::string& v1,
                                             const std::string& n2, const std::string& v2,
                                             const std::string& n3, const std::string& v3,
                                             int cursor_row, int cursor_column, int this_row,
                                             const Theme& t) const {
    const int  textY = y + TEXT_PADDING;
    const bool onRow = (cursor_row == this_row);

    const bool c1 = onRow && cursor_column == 1;
    const bool c3 = onRow && cursor_column == 3;
    const bool c5 = onRow && cursor_column == 5;

    c.draw_text(n1, name_x,        textY, c1 ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    draw_cursor_cell(c, v1, x + TRIPLE_V1, textY, c1, t.textValue, t);
    c.draw_text(n2, x + TRIPLE_N2, textY, c3 ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    draw_cursor_cell(c, v2, x + TRIPLE_V2, textY, c3, t.textValue, t);
    c.draw_text(n3, x + TRIPLE_N3, textY, c5 ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    draw_cursor_cell(c, v3, x + TRIPLE_V3, textY, c5, t.textValue, t);
}

void InstrumentEditorModule::draw_type_load_row(Canvas& c, int x, int y, int name_x, int value_x,
                                                const Instrument& ins, int cursor_row,
                                                int cursor_column, int this_row,
                                                const Theme& t) const {
    (void)value_x;   // the TYPE row aligns to the TRIPLE grid, not the two-column grid
    const int  textY = y + TEXT_PADDING;
    const bool onRow = (cursor_row == this_row);

    const InstrumentType type = ins.instrumentType;

    const bool c1 = onRow && cursor_column == 1;
    const bool c2 = onRow && cursor_column == 2;
    const bool c3 = onRow && cursor_column == 3;

    // Lowercase, as the other cycling cells on this screen are (filter, loop, slice).
    const char* typeText = "sampler";
    switch (type) {
        case InstrumentType::SOUNDFONT: typeText = "soundfont"; break;
        case InstrumentType::EXTERNAL:  typeText = "external";  break;
        case InstrumentType::SAMPLER:   break;
    }

    // TYPE's value sits under the ROOT/VOL column; the source LOAD and EDIT are the two buttons to its
    // right. LOAD and EDIT are BUTTONS — `textValue` even unselected, because a dim label would read as
    // a parameter name rather than as something you can press.
    c.draw_text("TYPE", name_x, textY, c1 ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    draw_cursor_cell(c, typeText, x + TYPE_VALUE, textY, c1, t.textValue, t);

    // ⚠️ Both buttons are drawn only where they DO something, and `instrument_row_layout.h` caps the
    // cursor at the same numbers — one table, so a button that is not drawn is also not reachable.
    // No LOAD on EXTERNAL: it has no source file of any kind, only a channel and a patch number.
    // No EDIT on a SoundFont: there is no single waveform to edit.
    const int maxCol = instrument_name_row_max_column(type);
    if (maxCol >= 2) {
        draw_cursor_cell(c, "LOAD", x + BTN_COL2, textY, c2, t.textValue, t);
    }
    if (maxCol >= 3) {
        draw_cursor_cell(c, "EDIT >", x + BTN_COL3, textY, c3, t.textValue, t);
    }
}

void InstrumentEditorModule::draw_name_row(Canvas& c, int y, int name_x, int value_x,
                                           const InstrumentEditorState& s, int this_row,
                                           const Theme& t) const {
    const int  textY = y + TEXT_PADDING;
    const bool onRow = (s.cursorRow == this_row);

    c.draw_text("NAME", name_x, textY, onRow ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);

    // "______" until a sample or SF2 is loaded (or the user names the slot) — the auto-generated
    // "INSTxx" is treated as "unset", never shown. songcore::instrument_has_default_name is the single
    // test for that, everywhere.
    const std::string display = songcore::instrument_has_default_name(s.instrument)
                                    ? "______"
                                    : s.instrument.name;
    draw_cursor_cell(c, display, value_x, textY, onRow, t.textValue, t);
}

void InstrumentEditorModule::draw_section_source_row(Canvas& c, int x, int y, int name_x,
                                                     int cursor_row, int cursor_column, int this_row,
                                                     const Theme& t) const {
    // It took an `is_soundfont` it never read: a .pti saves and loads ANY instrument type, EXTERNAL
    // included, so both buttons are always drawn. The dead parameter is gone rather than gaining a
    // third value nothing would look at.
    const int  textY = y + TEXT_PADDING;
    const bool onRow = (cursor_row == this_row);

    // The INSTRUMENT PRESET row: SAVE (col 2) and LOAD (col 3) a .pti. Named "INST PRESET" so it is not
    // confused with the source LOAD on the TYPE row above, nor the SoundFont PATCH selector below it.
    // SAVE and LOAD line up under the TYPE row's LOAD and EDIT (BTN_COL2 / BTN_COL3).
    c.draw_text("INST PRESET", name_x, textY, onRow ? t.textCursor : t.textParam, CHAR_SPACING,
                FONT_SCALE);
    draw_cursor_cell(c, "SAVE", x + BTN_COL2, textY, onRow && cursor_column == 2, t.textValue, t);
    draw_cursor_cell(c, "LOAD", x + BTN_COL3, textY, onRow && cursor_column == 3, t.textValue, t);
}

void InstrumentEditorModule::draw_eq_row(Canvas& c, int y, int name_x, int value_x, int eq_slot,
                                         int cursor_row, int cursor_column, int this_row,
                                         const Theme& t) const {
    const int  textY = y + TEXT_PADDING;
    const bool onRow = (cursor_row == this_row);

    c.draw_text("EQ", name_x, textY, onRow ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    // The shared painter, so this cell cannot drift from the pool's or the mixer's: "--" when
    // unassigned, and a trailing ">" that says the cell opens the EQ editor.
    draw_eq_cell(c, value_x, textY, eq_slot, onRow && cursor_column == 1, t);
}

// ─── Cursor context ──────────────────────────────────────────────────────────────────────────────

/**
 * A MIDI byte that can be OFF — BANK, PROG and the two halves of a CC slot.
 *
 * −1 is "send nothing", and the cell's five buttons follow from that with no new machinery: A on an
 * empty cell INSERTs (landing on 0), A+B DELETEs back to −1, and stepping is disabled while empty
 * because `hex_byte` reads `current == empty_value` as empty. Exactly the shape the phrase's chain
 * and instrument reference cells already have.
 */
static CursorContext midi_opt_context(int current, int max) {
    return cc::hex_byte(current, /*min=*/0, /*max=*/max, /*empty_value=*/-1,
                        /*can_delete=*/current >= 0, /*can_insert=*/current < 0);
}

CursorContext InstrumentEditorModule::cursor_context(const InstrumentEditorState& s) const {
    const Instrument& ins = s.instrument;

    if (s.is_external()) {
        const int row = s.cursorRow;
        const int col = s.cursorColumn;

        // Rows 0/1 (TYPE, NAME) and 5 (the .pti buttons) are the dispatcher's on every layout.
        if (row == 0 || row == 1) return cc::read_only();
        if (row == 5) return cc::read_only();

        if (row == 2) {  // CHAN + BANK
            // CHAN is stored 0-15 and shown 1-16. The CONTEXT carries the stored number, so stepping
            // wraps 15→0 the way the wire numbers it; only the drawing adds the 1.
            if (col == 1) return cc::hex_byte(clamp(ins.midiChannel, 0, 15), 0, 15, -1, false, false,
                                              false, /*def=*/0);
            // BANK is 14-bit (CC0 MSB + CC32 LSB, midi_out.h), so its large step is a whole MSB — 16
            // would take 1024 presses to cross the range.
            if (col == 3) {
                CursorContext c = midi_opt_context(ins.midiBank, 16383);
                c.largeStep     = 128;
                return c;
            }
            return cc::none();
        }

        if (row == 3) {  // PROG + LEN
            if (col == 1) return midi_opt_context(ins.midiProgram, 127);
            // LEN 00 is not "empty" — it is gate-to-next, a MODE (midi_out.h), so the cell steps
            // through it like any other value and A+B resets to it rather than deleting.
            if (col == 3) return cc::hex_byte(ins.midiLen, 0, 255, -1, false, false, false, /*def=*/0x00);
            return cc::none();
        }

        if (row == 4 || row == 6) return cc::none();  // spacers

        if (row == 7) {  // VOL + PAN
            if (col == 1) return cc::hex_byte(ins.volume, 0, 255, -1, false, false, false, /*def=*/0xFF);
            if (col == 3) return cc::hex_byte(ins.pan, 0, 255, -1, false, false, false, /*def=*/0x80);
            return cc::none();
        }

        const int ccIndex = row - INSTRUMENT_EXTERNAL_CC_ROW;
        if (ccIndex >= 0 && ccIndex < static_cast<int>(ins.midiCC.size())) {
            const songcore::MidiCcSlot& slot = ins.midiCC[static_cast<size_t>(ccIndex)];
            if (col == 1) return midi_opt_context(slot.cc, 127);
            if (col == 3) return midi_opt_context(slot.value, 127);
        }
        return cc::none();
    }

    const bool sf  = s.is_soundfont();
    const int  off = sf ? 1 : 0;   // the SoundFont's PRESET row pushes everything below it down
    const int  row = s.cursorRow;
    const int  col = s.cursorColumn;

    // Rows 0 and 1 are the TYPE and NAME rows. They are READ_ONLY *to the generic handlers* — A+LEFT/RIGHT on
    // TYPE cycles the three types and A on NAME opens the name editor, and both are dispatcher
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
    if (row == 5) return cc::read_only();  // the INST PRESET SAVE / LOAD buttons — the dispatcher's, not a value

    if (sf && row == 6) {  // PATCH
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

    if (ins.instrumentType == InstrumentType::EXTERNAL) {
        // The three-state MIDI byte: SET writes it, DELETE clears to −1 ("send nothing"), INSERT lands
        // on 0. One lambda, because BANK, PROG and both halves of all four CC slots behave alike.
        const auto midiOpt = [&](int& field, int max) {
            if (isSet)                                          field = clamp(v, 0, max);
            else if (action.type == ActionType::DELETE)         field = -1;
            else if (action.type == ActionType::INSERT_DEFAULT) field = 0;
        };

        if (row == 2) {
            if (col == 1) { if (isSet) ins.midiChannel = clamp(v, 0, 15); }   // stored 0-15, shown 1-16
            else if (col == 3) midiOpt(ins.midiBank, 16383);

        } else if (row == 3) {
            if (col == 1)      midiOpt(ins.midiProgram, 127);
            else if (col == 3) b255(ins.midiLen);

        } else if (row == 7) {
            if (col == 1)      b255(ins.volume);
            else if (col == 3) b255(ins.pan);

        } else {
            const int ccIndex = row - INSTRUMENT_EXTERNAL_CC_ROW;
            if (ccIndex >= 0 && ccIndex < static_cast<int>(ins.midiCC.size())) {
                songcore::MidiCcSlot& slot = ins.midiCC[static_cast<size_t>(ccIndex)];
                if (col == 1)      midiOpt(slot.cc, 127);
                else if (col == 3) midiOpt(slot.value, 127);
            }
        }

        r.modified = (action.type != ActionType::NONE);
        return r;
    }

    // Rows 0/1 (TYPE, NAME) and 4/5 (spacer, INST PRESET buttons) are handled by the dispatcher: the
    // TYPE row's source LOAD/EDIT and the preset row's SAVE/LOAD are host verbs, not field writes.

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
        // PATCH — the module cannot resolve this itself: the bank+preset behind index `v` live in the
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
