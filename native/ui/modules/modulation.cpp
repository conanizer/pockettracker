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

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool is_ahd_shaped(ModType t) { return t == ModType::AHD || t == ModType::DRUM; }

/** A list entry, or "???" if the stored index is out of range — Kotlin's `getOrElse { "???" }`. */
std::string at_or_unknown(const std::vector<std::string>& list, int index) {
    if (index < 0 || index >= static_cast<int>(list.size())) return "???";
    return list[static_cast<size_t>(index)];
}

}  // namespace

std::vector<std::string> mod_row_labels(ModType type) {
    switch (type) {
        case ModType::NONE:     return {"TYPE"};
        case ModType::AHD:      return {"TYPE", "DEST", "AMT", "ATK", "HOLD", "DEC"};
        case ModType::ADSR:     return {"TYPE", "DEST", "AMT", "ATK", "DEC", "SUS", "REL"};
        case ModType::LFO:      return {"TYPE", "DEST", "AMT", "OSC", "TRIG", "FREQ"};
        case ModType::DRUM:     return {"TYPE", "DEST", "AMT", "ATK", "HOLD", "DEC"};
        case ModType::TRIG:     return {"TYPE", "DEST", "AMT", "ATK", "DEC", "SUS", "REL"};
        case ModType::TRACKING: return {"TYPE", "DEST", "AMT", "ATK", "DEC"};
        case ModType::SCALAR:   return {"TYPE", "DEST", "AMT"};
    }
    return {"TYPE"};
}

std::string mod_row_value(const ModSlot& slot, int row_index, int slot_index) {
    switch (row_index) {
        case 0:
            return songcore::mod_type_display_name(slot.type);

        case 1: {
            // A slot whose destination is ANOTHER slot names its target: "→M3 AMT". The target is
            // circular from this slot's own position, so MOD4 modulates MOD1 — every slot has one, and
            // no slot can point at itself.
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

        // From here down the row's MEANING depends on the type — the LFO has an oscillator where an
        // envelope has an attack, and an AHD has a hold where an ADSR has a decay.
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

    // The pairs stack, and the second one starts wherever the first one ENDED — a pair with two ADSRs
    // in it is 7 rows tall where a pair of NONEs is 1, so this cannot be a fixed offset.
    int pairTopY = y + TEXT_PADDING + ROW_HEIGHT + 6;

    for (int pair = 0; pair < 2; ++pair) {
        const ModSlot& left  = ins.modSlots[static_cast<size_t>(pair * 2)];
        const ModSlot& right = ins.modSlots[static_cast<size_t>(pair * 2 + 1)];

        const int leftRows  = songcore::mod_slot_row_count(left);
        const int rightRows = songcore::mod_slot_row_count(right);
        const int pairRows  = leftRows > rightRows ? leftRows : rightRows;

        // The pair's headers dim when the cursor is in the other pair — the only cue that says which
        // half of the screen your keypresses are going to.
        const Argb headerColor = (pair == s.cursorPair) ? t.textParam : t.textEmpty;
        c.draw_text("MOD" + std::to_string(pair * 2 + 1), x + NAME_X1, pairTopY, headerColor,
                    CHAR_SPACING, FONT_SCALE);
        c.draw_text("MOD" + std::to_string(pair * 2 + 2), x + NAME_X2, pairTopY, headerColor,
                    CHAR_SPACING, FONT_SCALE);

        const int dataStartY = pairTopY + ROW_HEIGHT;

        for (int rowIdx = 0; rowIdx < pairRows; ++rowIdx) {
            const int  textY       = dataStartY + rowIdx * ROW_HEIGHT;
            const bool isCursorRow = (pair == s.cursorPair && rowIdx == s.cursorRow);

            // The highlight spans BOTH slots of the pair — it marks the row, and the cursor colour on
            // one side or the other is what marks the slot.
            if (isCursorRow) c.fill_rect(x, textY - TEXT_PADDING, WIDTH, ROW_HEIGHT, t.rowCursor);

            const bool activePair = (pair == s.cursorPair);

            const auto draw_side = [&](const ModSlot& slot, int rows, int side, int nameX, int valX) {
                if (rowIdx >= rows) return;   // this slot is shallower than the pair — nothing here
                const std::vector<std::string> labels = mod_row_labels(slot.type);
                const std::string label = (rowIdx < static_cast<int>(labels.size()))
                                              ? labels[static_cast<size_t>(rowIdx)]
                                              : std::string();
                const std::string value = mod_row_value(slot, rowIdx, pair * 2 + side);

                const bool active     = activePair && (s.cursorSide == side);
                const Argb labelColor = (isCursorRow && active) ? t.textCursor : t.textParam;
                const Argb valueColor = (isCursorRow && active) ? t.textCursor : t.textValue;

                c.draw_text(label, x + nameX, textY, labelColor, CHAR_SPACING, FONT_SCALE);
                c.draw_text(value, x + valX,  textY, valueColor, CHAR_SPACING, FONT_SCALE);
            };

            draw_side(left,  leftRows,  0, NAME_X1, VAL_X1);
            draw_side(right, rightRows, 1, NAME_X2, VAL_X2);
        }

        // Past this pair's header, its rows, and a blank row before the next.
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
            // A slot holding a HIDDEN type (SCALAR / TRACKING, from an older project) finds no match
            // and reads as index 0 — so stepping it moves to NONE rather than staying stuck.
            return cc::index_cycle(index, static_cast<int>(types.size()));
        }

        case 1:  // DEST — nothing to route until there is a type to route
            if (slot.type == ModType::NONE) return cc::read_only();
            return cc::index_cycle(static_cast<int>(slot.dest), songcore::MOD_DEST_COUNT);

        case 2:  // AMT
            if (slot.type == ModType::NONE) return cc::read_only();
            return cc::hex_byte(slot.amount, 0, 255, -1, false, false, false, /*def=*/0xFF);

        // Rows 3..6 exist only if the type is deep enough. `rows` is the guard, and it is the same
        // number the cursor uses to decide how far down it may walk — so a read_only() here is a cell
        // the cursor cannot reach anyway. It is the belt to that braces.
        case 3:
            if (rows < 4) return cc::read_only();
            if (slot.type == ModType::LFO)
                return cc::index_cycle(slot.oscShape, static_cast<int>(osc_shapes().size()));
            return cc::hex_byte(slot.attack, 0, 255, -1, false, false, false, /*def=*/0x00);

        case 4:
            if (rows < 5) return cc::read_only();
            if (slot.type == ModType::LFO)
                return cc::index_cycle(slot.lfoTrigMode, static_cast<int>(trig_modes().size()));
            if (is_ahd_shaped(slot.type))
                return cc::hex_byte(slot.hold, 0, 255, -1, false, false, false, /*def=*/0x00);
            return cc::hex_byte(slot.decay, 0, 255, -1, false, false, false, /*def=*/0x00);

        case 5:
            if (rows < 6) return cc::read_only();
            if (slot.type == ModType::LFO)
                return cc::hex_byte(slot.lfoFreq, 0, 255, -1, false, false, false, /*def=*/0x40);
            if (is_ahd_shaped(slot.type))
                return cc::hex_byte(slot.decay, 0, 255, -1, false, false, false, /*def=*/0x00);
            return cc::hex_byte(slot.sustain, 0, 255, -1, false, false, false, /*def=*/0x80);

        case 6:
            if (rows < 7) return cc::read_only();
            return cc::hex_byte(slot.release, 0, 255, -1, false, false, false, /*def=*/0x00);

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
        // "A+B resets the WHOLE slot" — which is what this screen's code says, in both languages, and
        // ⚠️ **it never happens.** No cursor context on MODS ever sets `canDelete`: the TYPE and DEST
        // rows are bare cycles (no delete, no default), and every parameter row is a `hex_byte` with a
        // DEFAULT — and `on_a_b` prefers a default over a delete. So A+B on a MODS row either does
        // nothing (TYPE / DEST) or resets that ONE field to its default (AMT / ATK / …), and this arm
        // is unreachable.
        //
        // Carried anyway, and not quietly deleted, for two reasons: it is what the Kotlin has (and
        // bug-for-bug parity is the rule the goldens enforce — `tools/ptinput` pins all five buttons on
        // all seven rows of all eight mod types), and it is the behaviour the screen would get back the
        // moment a context here declares canDelete. Found by the S4 harness, which asserted the
        // documented behaviour and was right to be surprised.
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
