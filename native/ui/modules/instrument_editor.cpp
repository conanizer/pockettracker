#include "ui/modules/instrument_editor.h"

#include <algorithm>

#include "ui/helpers.h"
#include "ui/instrument_row_layout.h"

namespace pt::ui {

using songcore::Instrument;
using songcore::InstrumentType;
using songcore::Note;
using Cell = InstrumentCell;

namespace {

// Column offsets for the TRIPLE rows: three pairs do not fit the two-column grid.
constexpr int TRIPLE_V1 = 90;   // first value  (ROOT / VOL)
constexpr int TRIPLE_N2 = 185;  // second label (DETUNE / TSP)
constexpr int TRIPLE_V2 = 305;
constexpr int TRIPLE_N3 = 368;  // third label  (TIC / PAN)
constexpr int TRIPLE_V3 = 438;

// TYPE (row 0) and INST PRESET (row 5) share their two button columns, so the buttons line up.
constexpr int TYPE_VALUE = TRIPLE_V1;  // the TYPE value, under the ROOT/VOL column
constexpr int BTN_COL2   = 300;        // cursor column 2: TYPE-row LOAD, PRESET-row SAVE
constexpr int BTN_COL3   = 400;        // cursor column 3: TYPE-row EDIT, PRESET-row LOAD

// A DUAL row's second pair, from the first pair's label and value.
constexpr int DUAL_N2 = 230;
constexpr int DUAL_V2 = 220;


// "--" for an unset MIDI byte: −1 sends nothing, 00 is a real program change.
std::string midi_opt(int v) { return v < 0 ? "--" : hex2(v); }

// Null past the slots the instrument carries.
template <typename Ins>
auto* cc_slot(Ins& ins, int slot) {
    return (slot >= 0 && slot < static_cast<int>(ins.midiCC.size()))
               ? &ins.midiCC[static_cast<size_t>(slot)]
               : nullptr;
}

const char* cell_label(Cell cell, int cc_slot_index) {
    static const char* const CC_LABELS[] = {"CC A", "CC B", "CC C", "CC D"};
    switch (cell) {
        case Cell::NAME:         return "NAME";
        case Cell::VOL:          return "VOL";
        case Cell::PAN:          return "PAN";
        case Cell::TSP:          return "TSP";
        case Cell::TIC:          return "TIC";
        case Cell::ROOT:         return "ROOT";
        case Cell::DETUNE:       return "DETUNE";
        case Cell::DRIVE:        return "DRIVE";
        case Cell::FILTER:       return "FILTER";
        case Cell::CRUSH:        return "CRUSH";
        case Cell::FREQ:         return "FREQ";
        case Cell::DOWNSAMPLE:   return "DWNSMPL";
        case Cell::RES:          return "RES";
        case Cell::REV:          return "REV";
        case Cell::DEL:          return "DEL";
        case Cell::EQ:           return "EQ";
        case Cell::PATCH:        return "PATCH";
        case Cell::SLICE:        return "SLICE";
        case Cell::LOOP:         return "LOOP";
        case Cell::START:        return "START";
        case Cell::LOOP_START:   return "LOOP ST";
        case Cell::END:          return "END";
        case Cell::LOOP_END:     return "LOOP END";
        case Cell::REVERSE:      return "REVERSE";
        case Cell::MIDI_CHANNEL: return "CHAN";
        case Cell::MIDI_BANK:    return "BANK";
        case Cell::MIDI_PROGRAM: return "PROG";
        case Cell::MIDI_LENGTH:  return "LEN";
        case Cell::CC_NUMBER:    return CC_LABELS[clamp(cc_slot_index, 0, 3)];
        case Cell::CC_VALUE:     return "VAL";
        // drawn by their own rows
        case Cell::NONE:
        case Cell::TYPE:
        case Cell::SOURCE_LOAD:
        case Cell::SOURCE_EDIT:
        case Cell::PRESET_SAVE:
        case Cell::PRESET_LOAD:
            return "";
    }
    return "";
}

const char* on_off(bool v) { return v ? "on" : "off"; }

std::string cell_text(Cell cell, int cc_slot_index, const InstrumentEditorState& s) {
    const Instrument& ins = s.instrument;
    switch (cell) {
        case Cell::NAME:
            // "______" until something is loaded or named; the generated "INSTxx" is never shown.
            return songcore::instrument_has_default_name(ins) ? "______" : ins.name;
        case Cell::VOL:        return hex2(ins.volume);
        case Cell::PAN:        return hex2(ins.pan);
        case Cell::TSP:        return on_off(ins.transposeEnabled);
        case Cell::TIC:        return hex2(ins.tableTicRate);
        case Cell::ROOT:       return note_name(ins.root);
        case Cell::DETUNE:     return hex2(ins.detune);
        case Cell::DRIVE:      return hex2(ins.drive);
        case Cell::FILTER:     return ins.filterType;
        case Cell::CRUSH:      return hex1(ins.crush);
        case Cell::FREQ:       return hex2(ins.filterCut);
        case Cell::DOWNSAMPLE: return hex1(ins.downsample);
        case Cell::RES:        return hex2(ins.filterRes);
        case Cell::REV:        return hex2(ins.reverbSend);
        case Cell::DEL:        return hex2(ins.delaySend);
        case Cell::PATCH: {
            // "3/128 Acoustic Grand": the 1-based position in the patch list, then its name.
            const std::string num = (s.sfPresetCount > 0) ? std::to_string(s.sfPresetIndex + 1) + "/" +
                                                                std::to_string(s.sfPresetCount)
                                                          : "--";
            return s.sfPresetName.empty() ? num : num + " " + s.sfPresetName;
        }
        case Cell::SLICE:      return slice_modes()[static_cast<size_t>(clamp(ins.slicingMode, 0, 2))];
        case Cell::LOOP:       return ins.loopMode;
        case Cell::START:      return hex2(ins.sampleStart);
        case Cell::LOOP_START: return hex2(ins.loopStart);
        case Cell::END:        return hex2(ins.sampleEnd);
        case Cell::LOOP_END:   return hex2(ins.loopEnd);
        case Cell::REVERSE:    return on_off(ins.reverse);
        // CHAN is stored 0-15 and shown 1-16.
        case Cell::MIDI_CHANNEL: return dec2(clamp(ins.midiChannel, 0, 15) + 1);
        // 14-bit, so four hex digits.
        case Cell::MIDI_BANK:    return ins.midiBank < 0 ? "----" : hex_padded(ins.midiBank, 4);
        case Cell::MIDI_PROGRAM: return midi_opt(ins.midiProgram);
        case Cell::MIDI_LENGTH:  return hex2(ins.midiLen);
        case Cell::CC_NUMBER: {
            const auto* slot = cc_slot(ins, cc_slot_index);
            return slot ? midi_opt(slot->cc) : "";
        }
        case Cell::CC_VALUE: {
            const auto* slot = cc_slot(ins, cc_slot_index);
            return slot ? midi_opt(slot->value) : "";
        }
        // EQ has its own painter; the rest are drawn by their own rows
        case Cell::EQ:
        case Cell::NONE:
        case Cell::TYPE:
        case Cell::SOURCE_LOAD:
        case Cell::SOURCE_EDIT:
        case Cell::PRESET_SAVE:
        case Cell::PRESET_LOAD:
            return "";
    }
    return "";
}

}  // namespace

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void InstrumentEditorModule::draw(Canvas& c, int x, int y, const InstrumentEditorState& s) const {
    const Theme&         t    = s.theme;
    const Instrument&    ins  = s.instrument;
    const InstrumentType type = ins.instrumentType;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    const int nameX  = x + 10;
    const int valueX = x + 150;

    int rowY = y + TEXT_PADDING;

    c.draw_text("INSTRUMENT " + hex2(ins.id), nameX, rowY, t.textTitle, CHAR_SPACING, FONT_SCALE);
    rowY += ROW_HEIGHT + 14;

    const int rows = instrument_row_count(type);
    for (int row = 0; row < rows; ++row, rowY += ROW_HEIGHT) {
        const InstrumentRow r     = instrument_row(type, row);
        const int           textY = rowY + TEXT_PADDING;
        const auto on = [&](int i) {
            return s.cursorRow == row && s.cursorColumn == instrument_row_column(r.kind, i);
        };
        const auto pair = [&](int i, int label_x, int value_x) {
            draw_cell(c, label_x, value_x, textY, r.cells[i], r.ccSlot, on(i), s);
        };

        // A CC row past the slots the instrument carries stays blank.
        if (r.ccSlot >= static_cast<int>(ins.midiCC.size())) continue;

        switch (r.kind) {
            case InstrumentRowKind::NAME:
                draw_type_load_row(c, x, rowY, nameX, ins, s.cursorRow, s.cursorColumn, row, t);
                break;
            case InstrumentRowKind::SOURCE:
                draw_section_source_row(c, x, rowY, nameX, s.cursorRow, s.cursorColumn, row, t);
                break;
            case InstrumentRowKind::SINGLE:
                pair(0, nameX, valueX);
                break;
            case InstrumentRowKind::DUAL:
                pair(0, nameX, valueX);
                pair(1, nameX + DUAL_N2, valueX + DUAL_V2);
                break;
            case InstrumentRowKind::TRIPLE:
                pair(0, nameX, x + TRIPLE_V1);
                pair(1, x + TRIPLE_N2, x + TRIPLE_V2);
                pair(2, x + TRIPLE_N3, x + TRIPLE_V3);
                break;
            case InstrumentRowKind::SPACER:
                break;
        }
    }

    // Status messages are the global overlay's, drawn on the visualizer header.
}

// ─── Draw helpers ────────────────────────────────────────────────────────────────────────────────

// ⚠️ No row background on this screen: the cursor is the cell, and the label beside it takes
// `cursor_mark_ink`. A label is filled only where the cursor can land on it.

void InstrumentEditorModule::draw_cell(Canvas& c, int label_x, int value_x, int text_y, Cell cell,
                                       int cc_slot_index, bool is_cursor,
                                       const InstrumentEditorState& s) const {
    const Theme& t = s.theme;
    c.draw_text(cell_label(cell, cc_slot_index), label_x, text_y,
                is_cursor ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
    // The shared EQ-cell painter: "--" when unassigned, ">" for "opens the editor".
    if (cell == Cell::EQ) {
        draw_eq_cell(c, value_x, text_y, s.instrument.eqSlot, is_cursor, t);
        return;
    }
    draw_cursor_cell(c, cell_text(cell, cc_slot_index, s), value_x, text_y, is_cursor, t.textValue, t);
}

void InstrumentEditorModule::draw_type_load_row(Canvas& c, int x, int y, int name_x,
                                                const Instrument& ins, int cursor_row,
                                                int cursor_column, int this_row,
                                                const Theme& t) const {
    const int  textY = y + TEXT_PADDING;
    const bool onRow = (cursor_row == this_row);

    const InstrumentType type = ins.instrumentType;

    const bool c1 = onRow && cursor_column == 1;
    const bool c2 = onRow && cursor_column == 2;
    const bool c3 = onRow && cursor_column == 3;

    // Lowercase, as the other cycling cells here.
    const char* typeText = "sampler";
    switch (type) {
        case InstrumentType::SOUNDFONT: typeText = "soundfont"; break;
        case InstrumentType::EXTERNAL:  typeText = "external";  break;
        case InstrumentType::SAMPLER:   break;
    }

    // LOAD and EDIT are buttons: `textValue` even unselected, so they do not read as labels.
    c.draw_text("TYPE", name_x, textY, c1 ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
    draw_cursor_cell(c, typeText, x + TYPE_VALUE, textY, c1, t.textValue, t);

    // Drawn only where the type has them, which is also where the cursor stops.
    if (instrument_cell_at(type, this_row, 2) == Cell::SOURCE_LOAD) {
        draw_cursor_cell(c, "LOAD", x + BTN_COL2, textY, c2, t.textValue, t);
    }
    if (instrument_cell_at(type, this_row, 3) == Cell::SOURCE_EDIT) {
        draw_cursor_cell(c, "EDIT >", x + BTN_COL3, textY, c3, t.textValue, t);
    }
}

void InstrumentEditorModule::draw_section_source_row(Canvas& c, int x, int y, int name_x,
                                                     int cursor_row, int cursor_column, int this_row,
                                                     const Theme& t) const {
    const int  textY = y + TEXT_PADDING;
    const bool onRow = (cursor_row == this_row);

    // INST PRESET: SAVE / LOAD a .pti, under TYPE's LOAD / EDIT.
    c.draw_text("INST PRESET", name_x, textY, onRow ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING,
                FONT_SCALE);
    draw_cursor_cell(c, "SAVE", x + BTN_COL2, textY, onRow && cursor_column == 2, t.textValue, t);
    draw_cursor_cell(c, "LOAD", x + BTN_COL3, textY, onRow && cursor_column == 3, t.textValue, t);
}

// ─── Cursor context ──────────────────────────────────────────────────────────────────────────────

// A MIDI byte that can be OFF (−1): A on empty inserts 0, A+B deletes back to −1.
static CursorContext midi_opt_context(int current, int max) {
    return cc::hex_byte(current, /*min=*/0, /*max=*/max, /*empty_value=*/-1,
                        /*can_delete=*/current >= 0, /*can_insert=*/current < 0);
}


CursorContext InstrumentEditorModule::cursor_context(const InstrumentEditorState& s) const {
    const Instrument&   ins  = s.instrument;
    const InstrumentRow r    = instrument_row(ins.instrumentType, s.cursorRow);
    const Cell          cell = instrument_cell_at(ins.instrumentType, s.cursorRow, s.cursorColumn);

    switch (cell) {
        case Cell::NONE:
            return cc::none();

        // Read-only to the generic handlers: the dispatcher owns TYPE, NAME and the buttons.
        case Cell::TYPE:
        case Cell::SOURCE_LOAD:
        case Cell::SOURCE_EDIT:
        case Cell::NAME:
        case Cell::PRESET_SAVE:
        case Cell::PRESET_LOAD:
            return cc::read_only();

        case Cell::ROOT: {
            const bool empty = (ins.root == Note::EMPTY());
            return cc::note(empty ? 0 : songcore::note_to_midi(ins.root), empty);
        }
        case Cell::DETUNE: return cc::plain_byte(ins.detune, 0x80);
        case Cell::TIC:    return cc::plain_byte(ins.tableTicRate, 0x06);
        case Cell::VOL:    return cc::plain_byte(ins.volume, 0xFF);
        case Cell::TSP:    return cc::toggle_binary(ins.transposeEnabled);
        case Cell::PAN:    return cc::plain_byte(ins.pan, 0x80);

        case Cell::PATCH: {
            // The SF2's own list length; 0 with no SoundFont.
            const int maxIdx = (s.sfPresetCount - 1) < 0 ? 0 : (s.sfPresetCount - 1);
            return cc::hex_byte(s.sfPresetIndex, 0, maxIdx);
        }

        case Cell::DRIVE:      return cc::plain_byte(ins.drive, 0x00);
        case Cell::FILTER:     return cc::toggle_ternary(ins.filterType, filter_types());
        case Cell::CRUSH:      return cc::hex_nibble(ins.crush, /*def=*/0);
        case Cell::FREQ:       return cc::plain_byte(ins.filterCut, 0x00);
        case Cell::DOWNSAMPLE: return cc::hex_nibble(ins.downsample, /*def=*/0);
        case Cell::RES:        return cc::plain_byte(ins.filterRes, 0x00);
        case Cell::REV:        return cc::plain_byte(ins.reverbSend, 0x00);
        case Cell::DEL:        return cc::plain_byte(ins.delaySend, 0x00);

        // −1 is "no EQ"; A+B clears back to it. ⚠️ `can_insert` is inert (−1 becomes 0 before the
        // emptiness test), so A on an unassigned EQ lands on slot 1 and INSERT_DEFAULT never arrives.
        case Cell::EQ:
            return cc::hex_byte(ins.eqSlot < 0 ? 0 : ins.eqSlot, /*min=*/0, /*max=*/127,
                                /*empty_value=*/-1, /*can_delete=*/ins.eqSlot >= 0,
                                /*can_insert=*/ins.eqSlot < 0);

        case Cell::SLICE:
            return cc::toggle_ternary(slice_modes()[static_cast<size_t>(clamp(ins.slicingMode, 0, 2))],
                                      slice_modes());

        case Cell::LOOP: {
            CursorContext c = cc::toggle_ternary(ins.loopMode, loop_modes());
            // A ceiling keeps `loop_modes()` the one list; a saved `osc` still reads `osc`, and the
            // cursor can step out of it but not back.
            if (!s.allowOscLoop) c.maxValue = static_cast<int>(loop_modes().size()) - 2;
            return c;
        }
        // ⚠️ START and END are not bounded against each other: a HEX_BYTE wraps at its limits, so a
        // floor would fling END to FF mid-trim. An inverted pair has a defined sound.
        case Cell::START:      return cc::plain_byte(ins.sampleStart, 0x00);
        case Cell::LOOP_START: return cc::plain_byte(ins.loopStart, 0x00);
        case Cell::END:        return cc::plain_byte(ins.sampleEnd, 0xFF);
        case Cell::LOOP_END:   return cc::plain_byte(ins.loopEnd, 0xFF);
        case Cell::REVERSE:    return cc::toggle_binary(ins.reverse);

        // Steps the stored 0-15, wrapping as the wire does.
        case Cell::MIDI_CHANNEL:
            return cc::hex_byte(clamp(ins.midiChannel, 0, 15), 0, 15, -1, false, false, false, /*def=*/0);
        case Cell::MIDI_BANK: {
            // 14-bit, so the large step is a whole MSB.
            CursorContext c = midi_opt_context(ins.midiBank, 16383);
            c.largeStep     = 128;
            return c;
        }
        case Cell::MIDI_PROGRAM: return midi_opt_context(ins.midiProgram, 127);
        // LEN 00 is gate-to-next, a mode, not empty: A+B resets to it.
        case Cell::MIDI_LENGTH:  return cc::plain_byte(ins.midiLen, 0x00);
        case Cell::CC_NUMBER: {
            const auto* slot = cc_slot(ins, r.ccSlot);
            return slot ? midi_opt_context(slot->cc, 127) : cc::none();
        }
        case Cell::CC_VALUE: {
            const auto* slot = cc_slot(ins, r.ccSlot);
            return slot ? midi_opt_context(slot->value, 127) : cc::none();
        }
    }
    return cc::none();
}

// ─── What the cursor is standing on, by NAME ─────────────────────────────────────────────────────

songcore::MapTarget InstrumentEditorModule::map_target(const InstrumentEditorState& s) const {
    using songcore::MapDestId;
    switch (instrument_cell_at(s.type(), s.cursorRow, s.cursorColumn)) {
        case Cell::DETUNE:     return {MapDestId::INS_DETUNE, 0};
        // On EXTERNAL: velocity and CC 10, the two that survive the cable.
        case Cell::VOL:        return {MapDestId::INS_VOL, 0};
        case Cell::PAN:        return {MapDestId::INS_PAN, 0};
        case Cell::DRIVE:      return {MapDestId::INS_DRIVE, 0};
        case Cell::CRUSH:      return {MapDestId::INS_CRUSH, 0};
        case Cell::FREQ:       return {MapDestId::INS_CUT, 0};
        case Cell::DOWNSAMPLE: return {MapDestId::INS_DWN, 0};
        case Cell::RES:        return {MapDestId::INS_RES, 0};
        case Cell::REV:        return {MapDestId::INS_REV, 0};
        case Cell::DEL:        return {MapDestId::INS_DLY, 0};

        // Switches, modes, slots and boundaries — nothing a knob sweeps.
        case Cell::NONE:
        case Cell::TYPE:
        case Cell::SOURCE_LOAD:
        case Cell::SOURCE_EDIT:
        case Cell::NAME:
        case Cell::PRESET_SAVE:
        case Cell::PRESET_LOAD:
        case Cell::TSP:
        case Cell::TIC:
        case Cell::ROOT:
        case Cell::FILTER:
        case Cell::EQ:
        case Cell::PATCH:
        case Cell::SLICE:
        case Cell::LOOP:
        case Cell::START:
        case Cell::LOOP_START:
        case Cell::END:
        case Cell::LOOP_END:
        case Cell::REVERSE:
        case Cell::MIDI_CHANNEL:
        case Cell::MIDI_BANK:
        case Cell::MIDI_PROGRAM:
        case Cell::MIDI_LENGTH:
        case Cell::CC_NUMBER:
        case Cell::CC_VALUE:
            return {};
    }
    return {};
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

InstrumentInputResult InstrumentEditorModule::handle_input(Instrument& ins, int row, int col,
                                                           const InputAction& action) const {
    InstrumentInputResult r;
    r.modified = (action.type != ActionType::NONE);

    const bool isSet = (action.type == ActionType::SET_VALUE);
    const int  v     = action.value;

    const auto b255 = [&](int& field) { if (isSet) field = clamp(v, 0, 255); };
    const auto b15  = [&](int& field) { if (isSet) field = clamp(v, 0, 15); };
    const auto flag = [&](bool& field) { if (isSet) field = (v == 1); };
    const auto pick = [&](std::string& field, const std::vector<std::string>& list) {
        if (isSet && v >= 0 && v < static_cast<int>(list.size())) field = list[static_cast<size_t>(v)];
    };
    const auto optional = [&](int& field, int max) { apply_optional_byte(field, max, action); };

    const int ccSlot = instrument_row(ins.instrumentType, row).ccSlot;

    switch (instrument_cell_at(ins.instrumentType, row, col)) {
        case Cell::ROOT:
            if (isSet) ins.root = songcore::note_from_midi(v);
            else if (action.type == ActionType::DELETE) ins.root = Note::C4();
            break;
        case Cell::DETUNE:     b255(ins.detune);       break;
        case Cell::TIC:        b255(ins.tableTicRate); break;
        case Cell::VOL:        b255(ins.volume);       break;
        case Cell::TSP:        flag(ins.transposeEnabled); break;
        case Cell::PAN:        b255(ins.pan);          break;

        // Only the engine has read the SF2's list, so the dispatcher resolves index `v`.
        case Cell::PATCH:
            if (isSet) {
                r.presetIndexChanged = true;
                r.presetIndex        = v;
            }
            break;

        case Cell::DRIVE:      b255(ins.drive);                    break;
        case Cell::FILTER:     pick(ins.filterType, filter_types()); break;
        case Cell::CRUSH:      b15(ins.crush);                     break;
        case Cell::FREQ:       b255(ins.filterCut);                break;
        case Cell::DOWNSAMPLE: b15(ins.downsample);                break;
        case Cell::RES:        b255(ins.filterRes);                break;
        case Cell::REV:        b255(ins.reverbSend);               break;
        case Cell::DEL:        b255(ins.delaySend);                break;
        case Cell::EQ:         optional(ins.eqSlot, 127);          break;
        case Cell::SLICE:      if (isSet) ins.slicingMode = clamp(v, 0, 2); break;
        case Cell::LOOP:       pick(ins.loopMode, loop_modes());   break;
        case Cell::START:      b255(ins.sampleStart);              break;
        case Cell::LOOP_START: b255(ins.loopStart);                break;
        case Cell::END:        b255(ins.sampleEnd);                break;
        case Cell::LOOP_END:   b255(ins.loopEnd);                  break;
        case Cell::REVERSE:    flag(ins.reverse);                  break;

        case Cell::MIDI_CHANNEL: if (isSet) ins.midiChannel = clamp(v, 0, 15); break;  // stored 0-15
        case Cell::MIDI_BANK:    optional(ins.midiBank, 16383);   break;
        case Cell::MIDI_PROGRAM: optional(ins.midiProgram, 127);  break;
        case Cell::MIDI_LENGTH:  b255(ins.midiLen);               break;
        case Cell::CC_NUMBER:
            if (auto* slot = cc_slot(ins, ccSlot)) optional(slot->cc, 127);
            break;
        case Cell::CC_VALUE:
            if (auto* slot = cc_slot(ins, ccSlot)) optional(slot->value, 127);
            break;

        // The dispatcher's: host verbs, not field writes.
        case Cell::NONE:
        case Cell::TYPE:
        case Cell::SOURCE_LOAD:
        case Cell::SOURCE_EDIT:
        case Cell::NAME:
        case Cell::PRESET_SAVE:
        case Cell::PRESET_LOAD:
            break;
    }
    return r;
}

}  // namespace pt::ui
