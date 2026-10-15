#include "ui/modules/modulation.h"

#include "ui/helpers.h"

namespace pt::ui {

using songcore::Instrument;
using songcore::ModDest;
using songcore::ModSlot;
using songcore::ModType;

namespace {

// The two slots of a pair, side by side.
constexpr int NAME_X1 = 10;   // left slot: labels
constexpr int VAL_X1  = 80;   // left slot: values
constexpr int NAME_X2 = 320;  // right slot
constexpr int VAL_X2  = 390;


// "???" if the stored index is out of range.
std::string at_or_unknown(const std::vector<std::string>& list, int index) {
    if (index < 0 || index >= static_cast<int>(list.size())) return "???";
    return list[static_cast<size_t>(index)];
}

}  // namespace

const std::vector<std::string>& mod_row_labels(ModType type) {
    static const std::vector<std::string> kNone{"TYPE"};
    static const std::vector<std::string> kAhd{"TYPE", "DEST", "AMT", "ATK", "HOLD", "DEC"};
    static const std::vector<std::string> kAdsr{"TYPE", "DEST", "AMT", "ATK", "DEC", "SUS", "REL"};
    static const std::vector<std::string> kLfo{"TYPE", "DEST", "AMT", "OSC", "TRIG", "FREQ"};
    static const std::vector<std::string> kTracking{"TYPE", "DEST", "AMT", "ATK", "DEC"};
    static const std::vector<std::string> kScalar{"TYPE", "DEST", "AMT"};
    switch (type) {
        case ModType::NONE:     return kNone;
        case ModType::AHD:      return kAhd;
        case ModType::ADSR:     return kAdsr;
        case ModType::LFO:      return kLfo;
        case ModType::DRUM:     return kAhd;    // DRUM shares AHD's rows
        case ModType::TRIG:     return kAdsr;   // TRIG shares ADSR's rows
        case ModType::TRACKING: return kTracking;
        case ModType::SCALAR:   return kScalar;
    }
    return kNone;
}

std::string mod_row_value(const ModSlot& slot, int row_index, int slot_index) {
    switch (row_index) {
        case 0:
            return songcore::mod_type_display_name(slot.type);

        case 1: {
            // A slot routed to another slot names it: "→M3 AMT", circular, so MOD4 modulates MOD1.
            const bool modTarget = slot.dest == ModDest::MOD_AMT || slot.dest == ModDest::MOD_RATE ||
                                   slot.dest == ModDest::MOD_BOTH;
            if (slot_index >= 0 && modTarget) {
                const int target = ((slot_index + 1) % 4) + 1;   // 1-based
                const std::string m = "\xE2\x86\x92" "M" + std::to_string(target);   // U+2192 RIGHTWARDS ARROW
                if (slot.dest == ModDest::MOD_AMT)  return m + " AMT";
                if (slot.dest == ModDest::MOD_RATE) return m + " RTE";
                return m + " B";                                  // MOD_BOTH
            }
            return songcore::mod_dest_display_name(slot.dest);
        }

        case 2: return hex2(slot.amount);

        // From here down the row's meaning depends on the type.
        case 3:
            return (slot.type == ModType::LFO) ? at_or_unknown(osc_shapes(), slot.oscShape)
                                               : hex2(slot.attack);
        case 4:
            if (slot.type == ModType::LFO)  return at_or_unknown(trig_modes(), slot.lfoTrigMode);
            if (is_ahd_shaped(slot.type))   return hex2(slot.hold);
            return hex2(slot.decay);
        case 5:
            if (slot.type == ModType::LFO)  return hex2(slot.lfoFreq);
            if (is_ahd_shaped(slot.type))   return hex2(slot.decay);
            return hex2(slot.sustain);

        case 6: return hex2(slot.release);

        default: return "--";
    }
}

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void ModulationModule::draw(Canvas& c, int x, int y, const ModulationState& s) const {
    const Theme&      t   = s.theme;
    const Instrument& ins = s.instrument;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    c.draw_text("MOD " + hex2(ins.id), x + NAME_X1, y + TEXT_PADDING, t.textTitle, CHAR_SPACING,
                FONT_SCALE);

    // The second pair starts where the first ended: its depth depends on the types.
    int pairTopY = y + TEXT_PADDING + ROW_HEIGHT + 6;

    for (int pair = 0; pair < 2; ++pair) {
        const ModSlot& left  = ins.modSlots[static_cast<size_t>(pair * 2)];
        const ModSlot& right = ins.modSlots[static_cast<size_t>(pair * 2 + 1)];

        const int leftRows  = songcore::mod_slot_row_count(left);
        const int rightRows = songcore::mod_slot_row_count(right);
        const int pairRows  = leftRows > rightRows ? leftRows : rightRows;

        // The pair's headers dim when the cursor is in the other pair.
        const Argb headerColor = (pair == s.cursorPair) ? t.textParam : t.textEmpty;
        c.draw_text("MOD" + std::to_string(pair * 2 + 1), x + NAME_X1, pairTopY, headerColor,
                    CHAR_SPACING, FONT_SCALE);
        c.draw_text("MOD" + std::to_string(pair * 2 + 2), x + NAME_X2, pairTopY, headerColor,
                    CHAR_SPACING, FONT_SCALE);

        const int dataStartY = pairTopY + ROW_HEIGHT;

        for (int rowIdx = 0; rowIdx < pairRows; ++rowIdx) {
            const int  textY       = dataStartY + rowIdx * ROW_HEIGHT;
            const bool isCursorRow = (pair == s.cursorPair && rowIdx == s.cursorRow);

            const bool activePair = (pair == s.cursorPair);

            const auto draw_side = [&](const ModSlot& slot, int rows, int side, int nameX, int valX) {
                if (rowIdx >= rows) return;   // this slot is shallower than the pair
                const std::vector<std::string>& labels = mod_row_labels(slot.type);
                const std::string label = (rowIdx < static_cast<int>(labels.size()))
                                              ? labels[static_cast<size_t>(rowIdx)]
                                              : std::string();
                const std::string value = mod_row_value(slot, rowIdx, pair * 2 + side);

                // The cursor is the value cell alone; the label says which row and which slot.
                const bool onCell = isCursorRow && activePair && (s.cursorSide == side);

                c.draw_text(label, x + nameX, textY, onCell ? cursor_mark_ink(t) : t.textParam,
                            CHAR_SPACING, FONT_SCALE);
                draw_cursor_cell(c, value, x + valX, textY, onCell, t.textValue, t);
            };

            draw_side(left,  leftRows,  0, NAME_X1, VAL_X1);
            draw_side(right, rightRows, 1, NAME_X2, VAL_X2);
        }

        // Past the header, the rows and a blank row.
        pairTopY += ROW_HEIGHT + pairRows * ROW_HEIGHT + ROW_HEIGHT;
    }
}

// ─── Cursor context ──────────────────────────────────────────────────────────────────────────────

CursorContext ModulationModule::cursor_context(const ModulationState& s) const {
    const ModSlot& slot = s.active_slot();
    const int      rows = songcore::mod_slot_row_count(slot);

    switch (s.cursorRow) {
        case 0: {  // TYPE
            int index = 0;
            const std::vector<ModType>& types = user_mod_types();
            for (size_t i = 0; i < types.size(); ++i)
                if (types[i] == slot.type) { index = static_cast<int>(i); break; }
            // A hidden type (from an older project) reads as index 0, so stepping moves to NONE.
            return cc::index_cycle(index, static_cast<int>(types.size()));
        }

        case 1:  // DEST — nothing to route without a type
            if (slot.type == ModType::NONE) return cc::read_only();
            return cc::index_cycle(static_cast<int>(slot.dest), songcore::MOD_DEST_COUNT);

        case 2:  // AMT
            if (slot.type == ModType::NONE) return cc::read_only();
            return cc::plain_byte(slot.amount, 0xFF);

        // Rows 3..6 exist only for deep types; `rows` bounds the cursor, so this is a backstop.
        case 3:
            if (rows < 4) return cc::read_only();
            if (slot.type == ModType::LFO)
                return cc::index_cycle(slot.oscShape, static_cast<int>(osc_shapes().size()));
            return cc::plain_byte(slot.attack, 0x00);

        case 4:
            if (rows < 5) return cc::read_only();
            if (slot.type == ModType::LFO)
                return cc::index_cycle(slot.lfoTrigMode, static_cast<int>(trig_modes().size()));
            if (is_ahd_shaped(slot.type))
                return cc::plain_byte(slot.hold, 0x00);
            return cc::plain_byte(slot.decay, 0x00);

        case 5:
            if (rows < 6) return cc::read_only();
            if (slot.type == ModType::LFO)
                return cc::plain_byte(slot.lfoFreq, 0x40);
            if (is_ahd_shaped(slot.type))
                return cc::plain_byte(slot.decay, 0x00);
            return cc::plain_byte(slot.sustain, 0x80);

        case 6:
            if (rows < 7) return cc::read_only();
            return cc::plain_byte(slot.release, 0x00);

        default:
            return cc::read_only();
    }
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

ModulationInputResult ModulationModule::handle_input(Instrument& ins, int slot_index, int cursor_row,
                                                     const InputAction& action) const {
    ModulationInputResult r;
    if (slot_index < 0 || slot_index >= static_cast<int>(ins.modSlots.size())) return r;

    ModSlot& slot = ins.modSlots[static_cast<size_t>(slot_index)];

    if (action.type == ActionType::DELETE) {
        // ⚠️ Unreachable today: no MODS context sets `canDelete`. Kept so one that does gets the
        // whole-slot reset.
        slot = ModSlot{};
        r.modified = true;
        return r;
    }

    if (action.type != ActionType::SET_VALUE) return r;   // modified stays false

    const int  v    = action.value;
    const int  rows = songcore::mod_slot_row_count(slot);
    const bool lfo  = (slot.type == ModType::LFO);

    switch (cursor_row) {
        case 0: {
            const std::vector<ModType>& types = user_mod_types();
            const int idx = clamp(v, 0, static_cast<int>(types.size()) - 1);
            slot.type = types[static_cast<size_t>(idx)];
            break;
        }
        case 1:
            if (slot.type != ModType::NONE)
                slot.dest = static_cast<ModDest>(clamp(v, 0, songcore::MOD_DEST_COUNT - 1));
            break;
        case 2:
            if (slot.type != ModType::NONE) slot.amount = clamp(v, 0, 255);
            break;
        case 3:
            if (rows < 4) break;
            if (lfo) slot.oscShape = clamp(v, 0, static_cast<int>(osc_shapes().size()) - 1);
            else     slot.attack   = clamp(v, 0, 255);
            break;
        case 4:
            if (rows < 5) break;
            if (lfo)                       slot.lfoTrigMode = clamp(v, 0, static_cast<int>(trig_modes().size()) - 1);
            else if (is_ahd_shaped(slot.type)) slot.hold    = clamp(v, 0, 255);
            else                               slot.decay   = clamp(v, 0, 255);
            break;
        case 5:
            if (rows < 6) break;
            if (lfo)                       slot.lfoFreq = clamp(v, 0, 255);
            else if (is_ahd_shaped(slot.type)) slot.decay   = clamp(v, 0, 255);
            else                               slot.sustain = clamp(v, 0, 255);
            break;
        case 6:
            if (rows >= 7) slot.release = clamp(v, 0, 255);
            break;
        default:
            break;
    }

    r.modified = true;
    return r;
}

}  // namespace pt::ui
