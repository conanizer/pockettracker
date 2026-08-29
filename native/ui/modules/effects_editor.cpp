#include "ui/modules/effects_editor.h"

#include "ui/helpers.h"

namespace pt::ui {

namespace {

constexpr int LABEL_X = 10;
constexpr int VALUE_X = 120;

/**
 * cursor row → VISUAL row. The screen draws headers and spacers the cursor cannot land on:
 *
 *    0  "EFFECTS"            5  "REVERB"           10  "DELAY"
 *    1  ·                    6  SIZE   ← row 1     11  TIME   ← row 4
 *    2  "MASTER FX"          7  DAMP   ← row 2     12  FDBK   ← row 5
 *    3  TYPE   ← row 0       8  INP EQ ← row 3     13  REV    ← row 6
 *    4  ·                    9  ·                  14  INP EQ ← row 7
 */
constexpr int CURSOR_TO_VIS[] = {3, 6, 7, 8, 11, 12, 13, 14};

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

const std::vector<std::string>& EffectModule::delay_sync_names() {
    static const std::vector<std::string> names = {
        "1/1",  "1/2",  "1/4",   "1/8",
        "1/16", "1/32",
        "1/4T", "1/8T", "1/16T",
        "1/4.", "1/8.", "1/16.",
    };
    return names;
}

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void EffectModule::draw(Canvas& c, int x, int y, const EffectState& s) const {
    const Theme&             t = s.theme;
    const songcore::Project& p = s.project;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    const auto rowY = [y](int vis) { return y + TEXT_PADDING + vis * ROW_HEIGHT; };

    const auto header = [&](const char* text, int vis) {
        c.draw_text(text, x + LABEL_X, rowY(vis), t.textTitle, CHAR_SPACING, FONT_SCALE);
    };

    // A parameter row, addressed by its CURSOR row: where it lands on screen comes out of the same
    // map the cursor walks, so a header inserted between two sections cannot move one and not the
    // other. The label says which row the cursor is on, the value is the cell it fills.
    const auto param = [&](const char* name, int row, const std::string& text) {
        const int  vis = CURSOR_TO_VIS[static_cast<size_t>(row)];
        const bool sel = (s.cursorRow == row);
        c.draw_text(name, x + LABEL_X, rowY(vis), sel ? t.textCursor : t.textParam, CHAR_SPACING,
                    FONT_SCALE);
        draw_cursor_cell(c, text, x + VALUE_X, rowY(vis), sel, t.textValue, t);
    };

    /** The same row, whose value is an EQ slot rather than a number. */
    const auto eq_param = [&](int row, int eq_slot) {
        const int  vis = CURSOR_TO_VIS[static_cast<size_t>(row)];
        const bool sel = (s.cursorRow == row);
        c.draw_text("INP EQ", x + LABEL_X, rowY(vis), sel ? t.textCursor : t.textParam, CHAR_SPACING,
                    FONT_SCALE);
        draw_eq_cell(c, x + VALUE_X, rowY(vis), eq_slot, sel, t);
    };

    header("EFFECTS", 0);

    // ── Master bus ───────────────────────────────────────────────────────────────────────────────
    header("MASTER FX", 2);
    param("TYPE", ROW_MASTER_TYPE, p.masterBusFx == 0 ? "OTT" : "DUST");

    // ── Reverb ───────────────────────────────────────────────────────────────────────────────────
    header("REVERB", 5);
    param("SIZE", ROW_REV_SIZE, hex2(p.reverbFeedback));
    param("DAMP", ROW_REV_DAMP, hex2(p.reverbDamp));
    eq_param(ROW_REV_EQ, p.reverbInputEq);

    // ── Delay ────────────────────────────────────────────────────────────────────────────────────
    header("DELAY", 10);
    // Synced, TIME is a note division rather than a raw byte — the same cell speaking a second
    // vocabulary, which is why its cursor range changes with it (0..B instead of 00..FF).
    param("TIME", ROW_DLY_TIME,
          p.delaySync ? delay_sync_names()[static_cast<size_t>(clamp(p.delayTime, 0, 11))]
                      : hex2(p.delayTime));

    param("FDBK", ROW_DLY_FDBK, hex2(p.delayFeedback));
    param("REV",  ROW_DLY_REV,  hex2(p.delayReverbSend));
    eq_param(ROW_DLY_EQ, p.delayInputEq);
}

// ─── Cursor ──────────────────────────────────────────────────────────────────────────────────────

CursorContext EffectModule::cursor_context(const EffectState& s) const {
    const songcore::Project& p = s.project;

    switch (s.cursorRow) {
        case ROW_MASTER_TYPE: {
            // ⚠️ Built by hand rather than through cc::hex_byte, because Kotlin builds it by hand: it is
            // a two-state toggle, so it gets increment and decrement and NOTHING else — no fast step (a
            // large step of 1 that wrapped would be a second way to do the same thing), no delete.
            CursorContext c;
            c.valueType                 = CursorValueType::HEX_BYTE;
            c.capabilities.canIncrement = true;
            c.capabilities.canDecrement = true;
            c.currentValue = p.masterBusFx;
            c.minValue     = 0;
            c.maxValue     = 1;
            c.smallStep    = 1;
            c.largeStep    = 1;
            return c;
        }

        case ROW_REV_SIZE:
            return cc::hex_byte(p.reverbFeedback, 0, 255, -1, false, false, false, /*def=*/0x60);
        case ROW_REV_DAMP:
            return cc::hex_byte(p.reverbDamp, 0, 255, -1, false, false, false, /*def=*/0x80);
        case ROW_REV_EQ:
            return cc::hex_byte(p.reverbInputEq < 0 ? -1 : p.reverbInputEq, 0, 127,
                                /*empty_value=*/-1, /*can_delete=*/true, /*can_insert=*/true);

        case ROW_DLY_TIME:
            // The range follows the vocabulary: 12 subdivisions when synced, a full byte when free.
            return p.delaySync ? cc::hex_byte(clamp(p.delayTime, 0, 11), 0, 11)
                               : cc::hex_byte(p.delayTime, 0, 255, -1, false, false, false,
                                              /*def=*/0x40);
        case ROW_DLY_FDBK:
            return cc::hex_byte(p.delayFeedback, 0, 255, -1, false, false, false, /*def=*/0x60);
        case ROW_DLY_REV:
            return cc::hex_byte(p.delayReverbSend, 0, 255, -1, false, false, false, /*def=*/0x00);
        case ROW_DLY_EQ:
            return cc::hex_byte(p.delayInputEq < 0 ? -1 : p.delayInputEq, 0, 127,
                                /*empty_value=*/-1, /*can_delete=*/true, /*can_insert=*/true);

        default:
            return cc::none();
    }
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

EffectInputResult EffectModule::handle_input(songcore::Project& p, int cursor_row,
                                             const InputAction& action) const {
    const bool isSet = (action.type == ActionType::SET_VALUE);

    switch (cursor_row) {
        case ROW_MASTER_TYPE:
            if (!isSet) break;
            p.masterBusFx = clamp(action.value, 0, 1);
            return {true};

        case ROW_REV_SIZE:
            if (!isSet) break;
            p.reverbFeedback = clamp(action.value, 0, 255);
            return {true};

        case ROW_REV_DAMP:
            if (!isSet) break;
            p.reverbDamp = clamp(action.value, 0, 255);
            return {true};

        case ROW_REV_EQ:
            switch (action.type) {
                case ActionType::SET_VALUE:      p.reverbInputEq = clamp(action.value, 0, 127); break;
                case ActionType::DELETE:         p.reverbInputEq = -1; break;
                case ActionType::INSERT_DEFAULT: p.reverbInputEq = 0;  break;
                default:                         return {false};
            }
            return {true};

        case ROW_DLY_TIME:
            if (!isSet) break;
            // Clamped into whichever vocabulary is live — a synced TIME may not hold 0x40.
            p.delayTime = p.delaySync ? clamp(action.value, 0, 11) : clamp(action.value, 0, 255);
            return {true};

        case ROW_DLY_FDBK:
            if (!isSet) break;
            p.delayFeedback = clamp(action.value, 0, 255);
            return {true};

        case ROW_DLY_REV:
            if (!isSet) break;
            p.delayReverbSend = clamp(action.value, 0, 255);
            return {true};

        case ROW_DLY_EQ:
            switch (action.type) {
                case ActionType::SET_VALUE:      p.delayInputEq = clamp(action.value, 0, 127); break;
                case ActionType::DELETE:         p.delayInputEq = -1; break;
                case ActionType::INSERT_DEFAULT: p.delayInputEq = 0;  break;
                default:                         return {false};
            }
            return {true};

        default:
            break;
    }
    return {false};
}

}  // namespace pt::ui
