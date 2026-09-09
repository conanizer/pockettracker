#include "ui/modules/effects_editor.h"


#include <algorithm>

#include "effects/modules/delay-presets.h"
#include "ui/helpers.h"

namespace pt::ui {

namespace {

// The two columns. A cell is a label and a value 110 px apart, so a column is about 160 px of glyphs;
// ⚠️ the second one starts at 270 rather than further out because the panel's right edge is not the
// constraint — the visualizer strip beside it is, and TIME's widest synced name ("1/16.") reaches
// nearly 200 px past this.
constexpr int LABEL_X[2] = {10, 270};
constexpr int VALUE_GAP  = 110;

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/** Which preset the delay's three cells are, or `kDelayPresetUser` when they are nobody's. */
int delay_preset_of(const songcore::Project& p) {
    return delay_preset_match(p.delayPong, p.delayTone, p.delayWobble);
}

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

const std::vector<std::string>& EffectModule::delay_type_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (int i = 0; i < kDelayPresetCount; ++i) v.emplace_back(kDelayPresets[i].name);
        // ⚠️ The name for "these cells are nobody's preset", at kDelayPresetUser. It is a LABEL and
        // not a preset: nothing can be applied from it, and the TYPE cell reaches it only by the
        // user turning one of the four cells below.
        v.emplace_back("USER");
        return v;
    }();
    return names;
}

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void EffectModule::draw(Canvas& c, int x, int y, const EffectState& s) const {
    const Theme&             t = s.theme;
    const songcore::Project& p = s.project;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    // Where every row and every header lands, in one walk. Nothing below counts lines for itself.
    const EffectsLayout lay = effects_layout();

    // The title stays put and the lines below it scroll under it, the way SETTINGS' debug rows do.
    // The scroll is DERIVED from the cursor row each frame — no stored scroll state — centring the
    // cursor in the viewport and pinned at both ends by the clamp. The lines fit the panel as the
    // screen stands today, so it is zero throughout; the clip below is what keeps that true, and is
    // what a line added later would scroll under rather than draw over the title.
    c.draw_text("EFFECTS", x + LABEL_X[0], y + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);

    const int firstLineY = y + TEXT_PADDING + ROW_HEIGHT + 14;   // the gap SETTINGS leaves too
    const int viewportH  = HEIGHT - (firstLineY - y);
    const int contentH   = (lay.lineCount - 1) * ROW_HEIGHT;
    const int cursorTop  = (lay.rowLine[static_cast<size_t>(clamp(s.cursorRow, 0, MAX_CURSOR_ROW))] - 1)
                           * ROW_HEIGHT;

    // ⚠️ Scrolled in WHOLE ROWS, unlike SETTINGS' free pixel scroll. This screen is a form of labelled
    // rows and section headers, and a form cut through the middle of a header reads as broken rather
    // than as "there is more above". The row count is rounded UP, so the last row is still reachable
    // at full scroll even though that leaves a little blank below it.
    const int maxRows = (std::max(0, contentH - viewportH) + ROW_HEIGHT - 1) / ROW_HEIGHT;
    const int rows    = clamp((cursorTop + ROW_HEIGHT / 2 - viewportH / 2 + ROW_HEIGHT / 2)
                                  / ROW_HEIGHT,
                              0, maxRows);
    const int scrollY = rows * ROW_HEIGHT;

    const auto rowY = [&](int line) { return firstLineY + (line - 1) * ROW_HEIGHT - scrollY; };

    // Everything below is drawn through `rowY`, which subtracts the scroll — clip it to the viewport
    // so a scrolled row cannot overdraw the title above or spill past the panel edge.
    const Canvas::ClipScope rowsClip(c, x, firstLineY, WIDTH, viewportH);

    const auto header = [&](const char* text, EffectsSection section) {
        c.draw_text(text, x + LABEL_X[0], rowY(lay.sectionHeaderLine[static_cast<int>(section)]),
                    t.textTitle, CHAR_SPACING, FONT_SCALE);
    };

    // A parameter cell, addressed by its CURSOR row: both where it lands on screen and which column
    // it lands in come out of the same table the cursor walks, so a row moved there moves here too
    // and cannot end up drawn in one place and reachable in another.
    const auto param = [&](const char* name, int row, const std::string& text) {
        const int  ry    = rowY(lay.rowLine[static_cast<size_t>(row)]);
        const int  lx    = x + LABEL_X[effects_cell_pos(row).column];
        const bool sel   = (s.cursorRow == row);
        c.draw_text(name, lx, ry, sel ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, text, lx + VALUE_GAP, ry, sel, t.textValue, t);
    };

    /** The same cell, whose value is an EQ slot rather than a number. */
    const auto eq_param = [&](int row, int eq_slot) {
        const int  ry  = rowY(lay.rowLine[static_cast<size_t>(row)]);
        const int  lx  = x + LABEL_X[effects_cell_pos(row).column];
        const bool sel = (s.cursorRow == row);
        c.draw_text("INP EQ", lx, ry, sel ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
        draw_eq_cell(c, lx + VALUE_GAP, ry, eq_slot, sel, t);
    };

    // ── Master bus ───────────────────────────────────────────────────────────────────────────────
    header("MASTER FX", EffectsSection::MASTER);
    param("TYPE", ROW_MASTER_TYPE, p.masterBusFx == 0 ? "OTT" : "DUST");

    // ── Reverb ───────────────────────────────────────────────────────────────────────────────────
    header("REVERB", EffectsSection::REVERB);
    param("SIZE", ROW_REV_SIZE, hex2(p.reverbFeedback));
    param("DAMP", ROW_REV_DAMP, hex2(p.reverbDamp));
    eq_param(ROW_REV_EQ, p.reverbInputEq);

    // ── Delay ────────────────────────────────────────────────────────────────────────────────────
    header("DELAY", EffectsSection::DELAY);
    // ⚠️ The name is READ BACK from the three cells rather than stored, so it says USER the moment any
    // of them is turned by hand. TYPE is a starting place, not a mode.
    param("TYPE", ROW_DLY_TYPE, delay_type_names()[static_cast<size_t>(delay_preset_of(p))]);

    param("PONG", ROW_DLY_PONG, p.delayPong ? "ON" : "OFF");
    // Synced, TIME is a note division rather than a raw byte — the same cell speaking a second
    // vocabulary, which is why its cursor range changes with it (0..B instead of 00..FF).
    param("TIME", ROW_DLY_TIME,
          p.delaySync ? delay_sync_names()[static_cast<size_t>(clamp(p.delayTime, 0, 11))]
                      : hex2(p.delayTime));

    param("TONE", ROW_DLY_TONE,   hex2(p.delayTone));
    param("FDBK", ROW_DLY_FDBK,   hex2(p.delayFeedback));

    param("WOBL", ROW_DLY_WOBBLE, hex2(p.delayWobble));
    param("REV",  ROW_DLY_REV,    hex2(p.delayReverbSend));

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

        case ROW_DLY_TYPE: {
            // A short named list, so it steps and wraps and does nothing else — no fast step over
            // three entries, and no delete, because there is no empty preset.
            //
            // ⚠️ USER is inside the range only while the cells ARE nobody's preset. That is what makes
            // it a place the cursor can LEAVE and never a place it can be sent: from USER, one press
            // either way lands on a real preset, and from a real preset the list is the three names.
            const int cur = delay_preset_of(p);
            return cc::index_cycle(cur, cur == kDelayPresetUser ? kDelayPresetCount + 1
                                                                : kDelayPresetCount);
        }

        case ROW_DLY_PONG:
            return cc::toggle_binary(p.delayPong);

        case ROW_DLY_TIME:
            // The range follows the vocabulary: 12 subdivisions when synced, a full byte when free.
            return p.delaySync ? cc::hex_byte(clamp(p.delayTime, 0, 11), 0, 11)
                               : cc::hex_byte(p.delayTime, 0, 255, -1, false, false, false,
                                              /*def=*/0x40);
        case ROW_DLY_TONE:
            return cc::hex_byte(p.delayTone, 0, 255, -1, false, false, false, /*def=*/0xFF);
        case ROW_DLY_WOBBLE:
            return cc::hex_byte(p.delayWobble, 0, 255, -1, false, false, false, /*def=*/0x00);
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

        case ROW_DLY_TYPE: {
            // ⚠️ **THE PRESET IS APPLIED AND THEN FORGOTTEN** — it writes the three cells and stores no
            // name, so what the row reads afterwards is whatever those three now are. Landing on USER
            // writes nothing, because USER is not a set of values: it is the absence of a match.
            if (!isSet) break;
            const int idx = clamp(action.value, 0, kDelayPresetCount);
            if (idx >= kDelayPresetCount) return {false};
            const DelayPreset& d = kDelayPresets[idx];
            p.delayPong   = d.pong;
            p.delayTone   = d.tone;
            p.delayWobble = d.wobble;
            return {true};
        }

        case ROW_DLY_PONG:
            if (!isSet) break;
            p.delayPong = action.value != 0;
            return {true};

        case ROW_DLY_TIME:
            if (!isSet) break;
            // Clamped into whichever vocabulary is live — a synced TIME may not hold 0x40.
            p.delayTime = p.delaySync ? clamp(action.value, 0, 11) : clamp(action.value, 0, 255);
            return {true};

        case ROW_DLY_TONE:
            if (!isSet) break;
            p.delayTone = clamp(action.value, 0, 255);
            return {true};

        case ROW_DLY_WOBBLE:
            if (!isSet) break;
            p.delayWobble = clamp(action.value, 0, 255);
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
