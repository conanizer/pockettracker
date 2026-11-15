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

// ─── The cells that are one plain field ──────────────────────────────────────────────────────────
// A byte (00-FF), a nibble (0-F) or an on/off flag: its label, its field, its A+B value and the knob a
// MIDI mapping learns from it. Every other cell is written out in each switch below.

enum class FieldKind { BYTE, NIBBLE, FLAG };

struct FieldCell {
    Cell                 cell;
    const char*          label;
    FieldKind            kind;
    int Instrument::*    number;   // BYTE, NIBBLE
    bool Instrument::*   flag;     // FLAG
    int                  def;
    songcore::MapDestId  knob;
    ValueUnit            unit;     // what the scope strip's hint reads it as
};

constexpr FieldCell byte_cell(Cell c, const char* label, int Instrument::* f, int def,
                              songcore::MapDestId knob = songcore::MapDestId::NONE,
                              ValueUnit unit = ValueUnit::NONE) {
    return {c, label, FieldKind::BYTE, f, nullptr, def, knob, unit};
}
constexpr FieldCell nibble_cell(Cell c, const char* label, int Instrument::* f,
                                songcore::MapDestId knob) {
    return {c, label, FieldKind::NIBBLE, f, nullptr, 0, knob, ValueUnit::NONE};
}
constexpr FieldCell flag_cell(Cell c, const char* label, bool Instrument::* f) {
    return {c, label, FieldKind::FLAG, nullptr, f, 0, songcore::MapDestId::NONE, ValueUnit::NONE};
}

using D = songcore::MapDestId;
constexpr FieldCell FIELD_CELLS[] = {
    byte_cell(Cell::DETUNE,      "DETUNE",   &Instrument::detune,       0x80, D::INS_DETUNE),
    byte_cell(Cell::TIC,         "TIC",      &Instrument::tableTicRate, 0x06, D::NONE, ValueUnit::TABLE_TICS),
    // On EXTERNAL VOL and PAN are velocity and CC 10, the two that survive the cable.
    byte_cell(Cell::VOL,         "VOL",      &Instrument::volume,       0xFF, D::INS_VOL),
    byte_cell(Cell::PAN,         "PAN",      &Instrument::pan,          0x80, D::INS_PAN),
    flag_cell(Cell::TSP,         "TSP",      &Instrument::transposeEnabled),
    byte_cell(Cell::DRIVE,       "DRIVE",    &Instrument::drive,        0x00, D::INS_DRIVE),
    nibble_cell(Cell::CRUSH,     "CRUSH",    &Instrument::crush,              D::INS_CRUSH),
    byte_cell(Cell::FREQ,        "FREQ",     &Instrument::filterCut,    0x00, D::INS_CUT, ValueUnit::CUTOFF_HZ),
    nibble_cell(Cell::DOWNSAMPLE, "DWNSMPL", &Instrument::downsample,         D::INS_DWN),
    byte_cell(Cell::RES,         "RES",      &Instrument::filterRes,    0x00, D::INS_RES),
    byte_cell(Cell::REV,         "REV",      &Instrument::reverbSend,   0x00, D::INS_REV),
    byte_cell(Cell::DEL,         "DEL",      &Instrument::delaySend,    0x00, D::INS_DLY),
    // ⚠️ START and END are not bounded against each other: a byte wraps at its limits, so a floor
    // would fling END to FF mid-trim. An inverted pair has a defined sound.
    byte_cell(Cell::START,       "START",    &Instrument::sampleStart,  0x00),
    byte_cell(Cell::LOOP_START,  "LOOP ST",  &Instrument::loopStart,    0x00),
    byte_cell(Cell::END,         "END",      &Instrument::sampleEnd,    0xFF),
    byte_cell(Cell::LOOP_END,    "LOOP END", &Instrument::loopEnd,      0xFF),
    flag_cell(Cell::REVERSE,     "REVERSE",  &Instrument::reverse),
    // LEN 00 is gate-to-next, a mode, not empty: A+B resets to it.
    byte_cell(Cell::MIDI_LENGTH, "LEN",      &Instrument::midiLen,      0x00, D::NONE, ValueUnit::MIDI_LENGTH),
};

const FieldCell* field_cell(Cell cell) {
    for (const FieldCell& f : FIELD_CELLS)
        if (f.cell == cell) return &f;
    return nullptr;
}

const char* on_off(bool v) { return v ? "on" : "off"; }

const char* cell_label(Cell cell, int cc_slot_index) {
    static const char* const CC_LABELS[] = {"CC A", "CC B", "CC C", "CC D"};
    if (const FieldCell* f = field_cell(cell)) return f->label;
    switch (cell) {
        case Cell::NAME:         return "NAME";
        case Cell::ROOT:         return "ROOT";
        case Cell::FILTER:       return "FILTER";
        case Cell::EQ:           return "EQ";
        case Cell::PATCH:        return "PATCH";
        case Cell::SLICE:        return "SLICE";
        case Cell::LOOP:         return "LOOP";
        case Cell::MIDI_CHANNEL: return "CHAN";
        case Cell::MIDI_BANK:    return "BANK";
        case Cell::MIDI_PROGRAM: return "PROG";
        case Cell::CC_NUMBER:    return CC_LABELS[clamp(cc_slot_index, 0, 3)];
        case Cell::CC_VALUE:     return "VAL";
        case Cell::MIDI_BEND:    return "BEND";
        default:                 return "";   // the table's, or drawn by their own rows
    }
}

std::string cell_text(Cell cell, int cc_slot_index, const InstrumentEditorState& s) {
    const Instrument& ins = s.instrument;
    if (const FieldCell* f = field_cell(cell)) {
        switch (f->kind) {
            case FieldKind::BYTE:   return hex2(ins.*f->number);
            case FieldKind::NIBBLE: return hex1(ins.*f->number);
            case FieldKind::FLAG:   return on_off(ins.*f->flag);
        }
    }
    switch (cell) {
        case Cell::NAME:
            // "______" until something is loaded or named; the generated "INSTxx" is never shown.
            return songcore::instrument_has_default_name(ins) ? "______" : ins.name;
        case Cell::ROOT:       return note_name(ins.root);
        case Cell::FILTER:     return ins.filterType;
        case Cell::PATCH: {
            // "3/128 Acoustic Grand": the 1-based position in the patch list, then its name.
            const std::string num = (s.sfPresetCount > 0) ? std::to_string(s.sfPresetIndex + 1) + "/" +
                                                                std::to_string(s.sfPresetCount)
                                                          : "--";
            return s.sfPresetName.empty() ? num : num + " " + s.sfPresetName;
        }
        case Cell::SLICE:      return slice_modes()[static_cast<size_t>(clamp(ins.slicingMode, 0, 2))];
        case Cell::LOOP:       return ins.loopMode;
        // CHAN is stored 0-15 and shown 1-16.
        case Cell::MIDI_CHANNEL: return dec2(clamp(ins.midiChannel, 0, 15) + 1);
        // 14-bit, so four hex digits.
        case Cell::MIDI_BANK:    return ins.midiBank < 0 ? "----" : hex_padded(ins.midiBank, 4);
        case Cell::MIDI_PROGRAM: return midi_opt(ins.midiProgram);
        // Semitones, in decimal like the device's own menu, so the two can be set alike.
        case Cell::MIDI_BEND:    return dec2(ins.midiBendRange);
        case Cell::CC_NUMBER: {
            const auto* slot = cc_slot(ins, cc_slot_index);
            return slot ? midi_opt(slot->cc) : "";
        }
        case Cell::CC_VALUE: {
            const auto* slot = cc_slot(ins, cc_slot_index);
            return slot ? midi_opt(slot->value) : "";
        }
        // EQ has its own painter; the rest are the table's or drawn by their own rows.
        default:
            return "";
    }
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
        case InstrumentType::MACRO:     typeText = "macro";     break;
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

    if (const FieldCell* f = field_cell(cell)) {
        switch (f->kind) {
            case FieldKind::BYTE:   return cc::plain_byte(ins.*f->number, f->def).as(f->unit);
            case FieldKind::NIBBLE: return cc::hex_nibble(ins.*f->number, f->def);
            case FieldKind::FLAG:   return cc::toggle_binary(ins.*f->flag);
        }
    }

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

        case Cell::PATCH: {
            // The SF2's own list length; 0 with no SoundFont.
            const int maxIdx = (s.sfPresetCount - 1) < 0 ? 0 : (s.sfPresetCount - 1);
            return cc::hex_byte(s.sfPresetIndex, 0, maxIdx);
        }

        case Cell::FILTER:     return cc::toggle_ternary(ins.filterType, filter_types());

        case Cell::EQ:         return cc::eq_slot(ins.eqSlot);

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
        case Cell::MIDI_BEND: {
            CursorContext c = cc::hex_byte(ins.midiBendRange, songcore::MIDI_BEND_RANGE_MIN,
                                           songcore::MIDI_BEND_RANGE_MAX, -1, false, false, false,
                                           songcore::MIDI_BEND_RANGE_DEFAULT);
            c.largeStep = 12;   // an octave
            return c;
        }
        case Cell::CC_NUMBER: {
            const auto* slot = cc_slot(ins, r.ccSlot);
            return slot ? midi_opt_context(slot->cc, 127) : cc::none();
        }
        case Cell::CC_VALUE: {
            const auto* slot = cc_slot(ins, r.ccSlot);
            return slot ? midi_opt_context(slot->value, 127) : cc::none();
        }
        default:
            return cc::none();   // the table's
    }
}

// ─── What the cursor is standing on, by NAME ─────────────────────────────────────────────────────

songcore::MapTarget InstrumentEditorModule::map_target(const InstrumentEditorState& s) const {
    // Switches, modes, slots and boundaries have no knob — nothing a knob sweeps.
    const FieldCell* f = field_cell(instrument_cell_at(s.type(), s.cursorRow, s.cursorColumn));
    return f ? songcore::MapTarget{f->knob, 0} : songcore::MapTarget{};
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

InstrumentInputResult InstrumentEditorModule::handle_input(Instrument& ins, int row, int col,
                                                           const InputAction& action) const {
    InstrumentInputResult r;
    r.modified = (action.type != ActionType::NONE);

    const bool isSet = (action.type == ActionType::SET_VALUE);
    const int  v     = action.value;

    const Cell cell = instrument_cell_at(ins.instrumentType, row, col);
    if (const FieldCell* f = field_cell(cell)) {
        if (!isSet) return r;
        switch (f->kind) {
            case FieldKind::BYTE:   ins.*f->number = clamp(v, 0, 255); break;
            case FieldKind::NIBBLE: ins.*f->number = clamp(v, 0, 15);  break;
            case FieldKind::FLAG:   ins.*f->flag   = (v == 1);         break;
        }
        return r;
    }

    const auto pick = [&](std::string& field, const std::vector<std::string>& list) {
        if (isSet && v >= 0 && v < static_cast<int>(list.size())) field = list[static_cast<size_t>(v)];
    };
    const auto optional = [&](int& field, int max) { apply_optional_byte(field, max, action); };

    const int ccSlot = instrument_row(ins.instrumentType, row).ccSlot;

    switch (cell) {
        case Cell::ROOT:
            if (isSet) ins.root = songcore::note_from_midi(v);
            else if (action.type == ActionType::DELETE) ins.root = Note::C4();
            break;

        // Only the engine has read the SF2's list, so the dispatcher resolves index `v`.
        case Cell::PATCH:
            if (isSet) {
                r.presetIndexChanged = true;
                r.presetIndex        = v;
            }
            break;

        case Cell::FILTER:     pick(ins.filterType, filter_types()); break;
        case Cell::EQ:         optional(ins.eqSlot, 127);          break;
        case Cell::SLICE:      if (isSet) ins.slicingMode = clamp(v, 0, 2); break;
        case Cell::LOOP:       pick(ins.loopMode, loop_modes());   break;

        case Cell::MIDI_CHANNEL: if (isSet) ins.midiChannel = clamp(v, 0, 15); break;  // stored 0-15
        case Cell::MIDI_BANK:    optional(ins.midiBank, 16383);   break;
        case Cell::MIDI_PROGRAM: optional(ins.midiProgram, 127);  break;
        case Cell::MIDI_BEND:
            if (isSet) ins.midiBendRange = clamp(v, songcore::MIDI_BEND_RANGE_MIN, songcore::MIDI_BEND_RANGE_MAX);
            break;
        case Cell::CC_NUMBER:
            if (auto* slot = cc_slot(ins, ccSlot)) optional(slot->cc, 127);
            break;
        case Cell::CC_VALUE:
            if (auto* slot = cc_slot(ins, ccSlot)) optional(slot->value, 127);
            break;

        // TYPE, NAME and the buttons are the dispatcher's: host verbs, not field writes.
        default:
            break;
    }
    return r;
}

}  // namespace pt::ui
