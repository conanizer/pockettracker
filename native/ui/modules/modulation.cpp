#include "ui/modules/modulation.h"

#include "ui/helpers.h"

namespace pt::ui {

using songcore::Instrument;
using songcore::ModDest;
using songcore::ModSlot;
using songcore::ModType;

namespace {

// The two slots of a pair, side by side. A value stands one blank slot past a 4-letter label.
constexpr int NAME_X1 = 10;                     // left slot: labels
constexpr int VAL_X1  = NAME_X1 + 5 * CHAR_W;   // left slot: values
constexpr int NAME_X2 = 260;                    // right slot; its widest value must clear the right bar
constexpr int VAL_X2  = NAME_X2 + 5 * CHAR_W;


// "???" if the stored index is out of range.
std::string at_or_unknown(const std::vector<std::string>& list, int index) {
    if (index < 0 || index >= static_cast<int>(list.size())) return "???";
    return list[static_cast<size_t>(index)];
}

// How a row reads and steps. TIME is an envelope time, 1 ms..10 s.
enum class Kind { TYPE, DEST, BYTE, TIME, CHOICE, CURVE };

struct RowSpec {
    const char* label;
    Kind        kind;
    int ModSlot::* field;                           // BYTE, TIME, CHOICE
    int         def;                                // BYTE, TIME: the A+B value
    ValueUnit   unit;                               // BYTE
    const std::vector<std::string>& (*names)();     // CHOICE
};

// In `ModRow` order.
const RowSpec SPECS[] = {
    {"TYPE", Kind::TYPE,   nullptr,            0,    ValueUnit::NONE,   nullptr},
    {"DEST", Kind::DEST,   nullptr,            0,    ValueUnit::NONE,   nullptr},
    {"AMT",  Kind::BYTE,   &ModSlot::amount,   0xFF, ValueUnit::NONE,   nullptr},
    {"ATK",  Kind::TIME,   &ModSlot::attack,   0x00, ValueUnit::NONE,   nullptr},
    {"HOLD", Kind::TIME,   &ModSlot::hold,     0x00, ValueUnit::NONE,   nullptr},
    {"DEC",  Kind::TIME,   &ModSlot::decay,    0x00, ValueUnit::NONE,   nullptr},
    {"SUS",  Kind::BYTE,   &ModSlot::sustain,  0x80, ValueUnit::NONE,   nullptr},
    {"REL",  Kind::TIME,   &ModSlot::release,  0x00, ValueUnit::NONE,   nullptr},
    {"CRV",  Kind::CURVE,  nullptr,            0,    ValueUnit::NONE,   nullptr},
    {"OSC",  Kind::CHOICE, &ModSlot::oscShape, 0,    ValueUnit::NONE,   osc_shapes},
    {"TRIG", Kind::CHOICE, &ModSlot::lfoTrigMode, 0, ValueUnit::NONE,   trig_modes},
    {"FREQ", Kind::BYTE,   &ModSlot::lfoFreq,  0x40, ValueUnit::LFO_HZ, nullptr},
    {"POL",  Kind::CHOICE, &ModSlot::lfoPolarity, 0, ValueUnit::NONE,   lfo_polarities},
};

const RowSpec& spec(ModRow row) { return SPECS[static_cast<int>(row)]; }

bool has_row(ModType type, ModRow row) {
    const std::vector<ModRow>& rows = mod_rows(type);
    return std::find(rows.begin(), rows.end(), row) != rows.end();
}

}  // namespace

const std::vector<ModRow>& mod_rows(ModType type) {
    using R = ModRow;
    static const std::vector<ModRow> kNone{R::TYPE};
    static const std::vector<ModRow> kAhd{R::TYPE, R::DEST, R::AMT, R::ATK, R::HOLD, R::DEC, R::CRV};
    static const std::vector<ModRow> kAdsr{R::TYPE, R::DEST, R::AMT, R::ATK, R::DEC, R::SUS, R::REL, R::CRV};
    static const std::vector<ModRow> kLfo{R::TYPE, R::DEST, R::AMT, R::OSC, R::TRIG, R::FREQ, R::POL};
    static const std::vector<ModRow> kAmountOnly{R::TYPE, R::DEST, R::AMT};
    static const std::vector<ModRow> kEnv{R::TYPE, R::DEST, R::AMT, R::ATK, R::DEC, R::SUS, R::REL};
    switch (type) {
        case ModType::NONE:
        case ModType::DRUM:     // never in a slot: a file's DRUM and TRIG load as AHD and ADSR
        case ModType::TRIG:     return kNone;
        case ModType::AHD:      return kAhd;
        case ModType::ADSR:     return kAdsr;
        case ModType::LFO:      return kLfo;
        case ModType::TRACKING:
        case ModType::SCALAR:
        case ModType::VELOCITY: return kAmountOnly;
        case ModType::ENV:      return kEnv;    // ADSR without CRV: always curved
    }
    return kNone;
}

ModRow mod_row_at(ModType type, int row_index) {
    const std::vector<ModRow>& rows = mod_rows(type);
    return (row_index >= 0 && row_index < static_cast<int>(rows.size()))
               ? rows[static_cast<size_t>(row_index)]
               : ModRow::TYPE;
}

const char* mod_row_label(ModRow row) { return spec(row).label; }

std::string mod_row_value(const Canvas& c, const ModSlot& slot, int row_index, int slot_index) {
    if (row_index < 0 || row_index >= mod_row_count(slot)) return "--";
    const RowSpec& r = spec(mod_row_at(slot.type, row_index));
    switch (r.kind) {
        case Kind::TYPE:
            return c.cased(songcore::mod_type_display_name(slot.type));

        case Kind::DEST: {
            // A slot routed to another slot names it: "→M3 AMT".
            const songcore::ModTarget to = songcore::mod_target(slot.dest, slot_index < 0 ? 0 : slot_index);
            if (slot_index >= 0 && to.slot >= 0) {
                const std::string m = "\xE2\x86\x92" "M" + std::to_string(to.slot + 1);   // U+2192 RIGHTWARDS ARROW
                if (to.part == songcore::ModTargetPart::AMT)  return m + c.cased(" AMT");
                if (to.part == songcore::ModTargetPart::RATE) return m + c.cased(" RATE");
                return m + c.cased(" BOTH");
            }
            return c.cased(songcore::mod_dest_display_name(slot.dest));
        }

        case Kind::BYTE:
        case Kind::TIME:   return hex2(slot.*r.field);
        case Kind::CHOICE: return c.cased(at_or_unknown(r.names(), slot.*r.field));
        case Kind::CURVE:  return c.cased(at_or_unknown(env_curves(), curve_step(slot.curve)));
    }
    return "--";
}

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void ModulationModule::draw(Canvas& c, int x, int y, const ModulationState& s) const {
    const Theme&      t   = s.theme;
    const Instrument& ins = s.instrument;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    c.text(c.cased("MOD ") + hex2(ins.id), x + NAME_X1, y + TEXT_PADDING, t.textTitle);

    // The second pair starts where the first ended: its depth depends on the types. ⚠️ Two ADSR
    // pairs fill the screen to its last pixel row.
    int pairTopY = y + HEADER_Y + TEXT_PADDING;

    for (int pair = 0; pair < 2; ++pair) {
        const ModSlot& left  = ins.modSlots[static_cast<size_t>(pair * 2)];
        const ModSlot& right = ins.modSlots[static_cast<size_t>(pair * 2 + 1)];

        const int leftRows  = mod_row_count(left);
        const int rightRows = mod_row_count(right);
        const int pairRows  = leftRows > rightRows ? leftRows : rightRows;

        const int dataStartY = pairTopY;

        for (int rowIdx = 0; rowIdx < pairRows; ++rowIdx) {
            const int  textY       = dataStartY + rowIdx * ROW_HEIGHT;
            const bool isCursorRow = (pair == s.cursorPair && rowIdx == s.cursorRow);

            const bool activePair = (pair == s.cursorPair);

            const auto draw_side = [&](const ModSlot& slot, int rows, int side, int nameX, int valX) {
                if (rowIdx >= rows) return;   // this slot is shallower than the pair
                // The TYPE row is labelled with the slot's name: "MOD1 ADSR".
                const std::string label = rowIdx == 0
                                              ? "MOD" + std::to_string(pair * 2 + side + 1)
                                              : mod_row_label(mod_row_at(slot.type, rowIdx));
                const std::string value = mod_row_value(c, slot, rowIdx, pair * 2 + side);

                // The cursor is the value cell alone; the label says which row and which slot.
                const bool onCell = isCursorRow && activePair && (s.cursorSide == side);

                draw_cell_label(c, label, x + nameX, textY, onCell, t);
                draw_cursor_cell(c, value, x + valX, textY, onCell, t.textValue, t);
            };

            draw_side(left,  leftRows,  0, NAME_X1, VAL_X1);
            draw_side(right, rightRows, 1, NAME_X2, VAL_X2);
        }

        // Past the rows and a blank row.
        pairTopY += pairRows * ROW_HEIGHT + ROW_HEIGHT;
    }
}

// ─── Cursor context ──────────────────────────────────────────────────────────────────────────────

CursorContext ModulationModule::cursor_context(const ModulationState& s) const {
    const ModSlot& slot = s.active_slot();
    // `rows` bounds the cursor, so past it is a backstop.
    if (s.cursorRow < 0 || s.cursorRow >= mod_row_count(slot)) return cc::read_only();

    const RowSpec& r = spec(mod_row_at(slot.type, s.cursorRow));
    switch (r.kind) {
        case Kind::TYPE: {
            int index = 0;
            const std::vector<ModType>& types = user_mod_types();
            for (size_t i = 0; i < types.size(); ++i)
                if (types[i] == slot.type) { index = static_cast<int>(i); break; }
            // A type the cycle does not offer (SCL, ENV) reads as NONE's index, so a step leaves it.
            return cc::index_cycle(index, static_cast<int>(types.size()));
        }

        case Kind::DEST: {
            const std::vector<ModDest> dests = slot_mod_dests(s.instrument, s.active_slot_index());
            // A hidden DEST (STA, another type's, a route the next slot no longer takes) reads as index 0.
            const auto it = std::find(dests.begin(), dests.end(), slot.dest);
            const int index = it == dests.end() ? 0 : static_cast<int>(it - dests.begin());
            return cc::index_cycle(index, static_cast<int>(dests.size()));
        }

        case Kind::BYTE: return cc::plain_byte(slot.*r.field, r.def).as(r.unit);
        case Kind::TIME: {
            const int scale = songcore::env_scale(slot);
            const ValueUnit unit = scale == songcore::ENV_SCALE_LOG ? ValueUnit::ENV_TIME
                : static_cast<ValueUnit>(static_cast<int>(ValueUnit::ENV_TIME_LINEAR) + scale);
            return cc::plain_byte(slot.*r.field, r.def).as(unit);
        }
        case Kind::CHOICE:
            return cc::index_cycle(slot.*r.field, static_cast<int>(r.names().size()));
        case Kind::CURVE:
            return cc::index_cycle(curve_step(slot.curve), static_cast<int>(env_curves().size()));
    }
    return cc::read_only();
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

ModulationInputResult ModulationModule::handle_input(Instrument& ins, int slot_index, int cursor_row,
                                                     const InputAction& action) const {
    ModulationInputResult r;
    if (slot_index < 0 || slot_index >= static_cast<int>(ins.modSlots.size())) return r;
    if (action.type != ActionType::SET_VALUE) return r;   // modified stays false

    ModSlot& slot = ins.modSlots[static_cast<size_t>(slot_index)];
    const int v = action.value;
    r.modified = true;
    if (cursor_row < 0 || cursor_row >= mod_row_count(slot)) return r;

    const RowSpec& row = spec(mod_row_at(slot.type, cursor_row));
    switch (row.kind) {
        case Kind::TYPE: {
            const std::vector<ModType>& types = user_mod_types();
            const bool wasEnvelope = has_row(slot.type, ModRow::CRV);
            slot.type = types[static_cast<size_t>(clamp(v, 0, static_cast<int>(types.size()) - 1))];
            if (!wasEnvelope && has_row(slot.type, ModRow::CRV))
                slot.curve = songcore::CURVE_ANALOG;   // a new envelope starts ANALOG
            break;
        }
        case Kind::DEST: {
            const std::vector<ModDest> dests = slot_mod_dests(ins, slot_index);
            slot.dest = dests[static_cast<size_t>(clamp(v, 0, static_cast<int>(dests.size()) - 1))];
            break;
        }
        case Kind::BYTE:
        case Kind::TIME:   slot.*row.field = clamp(v, 0, 255); break;
        case Kind::CHOICE: slot.*row.field = clamp(v, 0, static_cast<int>(row.names().size()) - 1); break;
        case Kind::CURVE:  slot.curve = curve_step(clamp(v, 0, 2)); break;
    }
    return r;
}

}  // namespace pt::ui
