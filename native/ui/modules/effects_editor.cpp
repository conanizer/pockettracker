#include "ui/modules/effects_editor.h"


#include <algorithm>

#include "ui/fx_preset_bank.h"
#include "ui/helpers.h"

namespace pt::ui {

namespace {

// Two columns, label and value 110 px apart. ⚠️ The second starts at 270 because the visualizer
// strip beside the panel, not its edge, is the constraint: TIME's widest synced name reaches far.
constexpr int LABEL_X[2] = {10, 270};
constexpr int VALUE_GAP  = 110;

// A preset line: the name where a left-column label would be, SAVE and LOAD on the right column's
// label and value. ⚠️ The `*` takes the name's column and pushes the name one right, as on SCALE.
constexpr int PRESET_SAVE_X = LABEL_X[1];
constexpr int PRESET_LOAD_X = LABEL_X[1] + VALUE_GAP;

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/** A preset line's name cell: the bank's own cycle, with A+B back to row 0 (NORMAL, the defaults). */
CursorContext preset_name_context(int cycle_index, int count) {
    CursorContext c = cc::index_cycle(cycle_index, count);
    c.defaultValue  = 0;
    return c;
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

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

namespace {

constexpr const char* SECTION_NAMES[EFFECTS_SECTION_COUNT] = {"MASTER FX", "REVERB", "DELAY"};

/**
 * The cells of every section `lay` gives a line to (`only` = that one section, without its header;
 * −1 = all three). `line1_y` is the text y of line 1; `x` the left edge the two columns hang off.
 * A cell's position and column come from the table the cursor walks, so it cannot be drawn in one
 * place and reachable in another.
 */
void draw_cells(Canvas& c, int x, int line1_y, const EffectsLayout& lay, int only,
                const EffectState& s) {
    const Theme&             t = s.theme;
    const songcore::Project& p = s.project;

    const auto rowY  = [&](int line) { return line1_y + (line - 1) * ROW_HEIGHT; };
    const auto drawn = [&](EffectsSection section) {
        return only < 0 || static_cast<int>(section) == only;
    };

    const auto header = [&](EffectsSection section) {
        if (only >= 0) return;   // alone, its name is the title
        c.draw_text(SECTION_NAMES[static_cast<int>(section)], x + LABEL_X[0],
                    rowY(lay.sectionHeaderLine[static_cast<int>(section)]), t.textTitle, CHAR_SPACING,
                    FONT_SCALE);
    };

    /** `unread` greys a cell the chosen algorithm ignores. It still holds its value and still edits. */
    const auto param = [&](const char* name, int row, const std::string& text, bool unread = false) {
        const int  ry    = rowY(lay.rowLine[static_cast<size_t>(row)]);
        const int  lx    = x + LABEL_X[effects_cell_pos(row).column];
        const bool sel   = (s.cursorRow == row);
        c.draw_text(name, lx, ry, sel ? cursor_mark_ink(t) : unread ? t.textEmpty : t.textParam,
                    CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, text, lx + VALUE_GAP, ry, sel, unread ? t.textEmpty : t.textValue, t);
    };

    /** The same cell, whose value is an EQ slot rather than a number. */
    const auto eq_param = [&](int row, int eq_slot) {
        const int  ry  = rowY(lay.rowLine[static_cast<size_t>(row)]);
        const int  lx  = x + LABEL_X[effects_cell_pos(row).column];
        const bool sel = (s.cursorRow == row);
        c.draw_text("INP EQ", lx, ry, sel ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
        draw_eq_cell(c, lx + VALUE_GAP, ry, eq_slot, sel, t);
    };

    /**
     * A send's preset line: the name, SAVE, LOAD. `shown` empty = hand-set cells no built-in matches
     * (`----`); `drifted` = named after a built-in the cells no longer are (`*`).
     */
    const auto preset_line = [&](int name_row, int save_row, int load_row, std::string shown,
                                 bool drifted) {
        const int  ry    = rowY(lay.rowLine[static_cast<size_t>(name_row)]);
        const bool named = !shown.empty();
        if (!named) shown = "----";
        if (drifted) c.draw_text("*", x + LABEL_X[0], ry, t.textPlayhead, CHAR_SPACING, FONT_SCALE);
        draw_cell(c, shown, x + LABEL_X[0] + (drifted ? CHAR_W : 0), ry,
                  /*is_cursor=*/s.cursorRow == name_row, /*is_selected=*/false, /*is_empty=*/!named,
                  t.textValue, t);
        draw_cell(c, "SAVE", x + PRESET_SAVE_X, ry, /*is_cursor=*/s.cursorRow == save_row,
                  /*is_selected=*/false, /*is_empty=*/false, t.textParam, t);
        draw_cell(c, "LOAD", x + PRESET_LOAD_X, ry, /*is_cursor=*/s.cursorRow == load_row,
                  /*is_selected=*/false, /*is_empty=*/false, t.textParam, t);
    };

    // ── Master bus ───────────────────────────────────────────────────────────────────────────────
    if (drawn(EffectsSection::MASTER)) {
        header(EffectsSection::MASTER);
        param("TYPE", EffectModule::ROW_MASTER_TYPE, p.masterBusFx == 0 ? "OTT" : "DUST");
    }

    // ── Reverb ───────────────────────────────────────────────────────────────────────────────────
    if (drawn(EffectsSection::REVERB)) {
        header(EffectsSection::REVERB);
        preset_line(EffectModule::ROW_REV_TYPE, EffectModule::ROW_REV_SAVE, EffectModule::ROW_REV_LOAD,
                    reverb_preset_display_name(p), reverb_preset_differs_from_its_name(p));

        // Which reverb reads the cells below. ⚠️ Turning it by hand writes none of them, so the
        // numbers stay what the user typed and only the sound changes.
        const bool unread = !reverb_algo_reads_size_and_mod(p.reverbAlgo);
        param("ALGO", EffectModule::ROW_REV_ALGO, reverb_algo_name(p.reverbAlgo));
        param("SIZE", EffectModule::ROW_REV_SIZE, hex2(p.reverbSize), unread);

        param("PRE",  EffectModule::ROW_REV_PRE,   hex2(p.reverbPreDelay));
        param("DCAY", EffectModule::ROW_REV_DECAY, hex2(p.reverbFeedback));

        param("WIDE", EffectModule::ROW_REV_WIDE, hex2(p.reverbWidth));
        param("DAMP", EffectModule::ROW_REV_DAMP, hex2(p.reverbDamp));

        eq_param(EffectModule::ROW_REV_EQ, p.reverbInputEq);
        param("MOD",  EffectModule::ROW_REV_MOD, hex2(p.reverbMod), unread);
    }

    // ── Delay ────────────────────────────────────────────────────────────────────────────────────
    if (drawn(EffectsSection::DELAY)) {
        header(EffectsSection::DELAY);
        preset_line(EffectModule::ROW_DLY_TYPE, EffectModule::ROW_DLY_SAVE, EffectModule::ROW_DLY_LOAD,
                    delay_preset_display_name(p), delay_preset_differs_from_its_name(p));

        param("PONG", EffectModule::ROW_DLY_PONG, p.delayPong ? "ON" : "OFF");
        // Synced, TIME is a note division rather than a raw byte — the same cell speaking a second
        // vocabulary, which is why its cursor range changes with it (0..B instead of 00..FF).
        const auto& divisions = EffectModule::delay_sync_names();
        param("TIME", EffectModule::ROW_DLY_TIME,
              p.delaySync ? divisions[static_cast<size_t>(clamp(p.delayTime, 0, 11))]
                          : hex2(p.delayTime));

        param("TONE", EffectModule::ROW_DLY_TONE, hex2(p.delayTone));
        param("FDBK", EffectModule::ROW_DLY_FDBK, hex2(p.delayFeedback));

        param("WOBL", EffectModule::ROW_DLY_WOBBLE, hex2(p.delayWobble));
        param("REV",  EffectModule::ROW_DLY_REV,    hex2(p.delayReverbSend));

        eq_param(EffectModule::ROW_DLY_EQ, p.delayInputEq);
    }
}

// ── The send editor's box ────────────────────────────────────────────────────────────────────────
// The two columns as EFFECTS spaces them, and room for TIME's widest synced name past the second.
constexpr int BOX_W       = 480;
constexpr int BOX_X       = (DESIGN_W - BOX_W) / 2;
constexpr int CONTENT_PAD = 8;
constexpr int TITLE_GAP   = 8;

/** The title, air, then the section's lines, with 8 px above and below. */
constexpr int box_h(int lines) { return ROW_HEIGHT + TITLE_GAP + lines * ROW_HEIGHT + 2 * CONTENT_PAD; }

static_assert(box_h(EFFECTS_LINE_COUNT) + 2 * (MODAL_BORDER - 1) <= DESIGN_H,
              "the send editor is taller than the screen");

}  // namespace

void EffectModule::draw(Canvas& c, int x, int y, const EffectState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    // Where every row and every header lands, in one walk. Nothing below counts lines for itself.
    const EffectsLayout lay = effects_layout();

    // The lines below the title scroll under it, derived from the cursor row each frame.
    c.draw_text("EFFECTS", x + LABEL_X[0], y + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);

    const int firstLineY = y + TEXT_PADDING + ROW_HEIGHT + 14;   // the gap SETTINGS leaves too
    const int viewportH  = HEIGHT - (firstLineY - y);
    const int contentH   = (lay.lineCount - 1) * ROW_HEIGHT;
    const int cursorTop  = (lay.rowLine[static_cast<size_t>(clamp(s.cursorRow, 0, MAX_CURSOR_ROW))] - 1)
                           * ROW_HEIGHT;

    // ⚠️ Scrolled in WHOLE ROWS: a form cut through a header reads as broken. Rounded up, so the
    // last row is reachable at full scroll.
    const int maxRows = (std::max(0, contentH - viewportH) + ROW_HEIGHT - 1) / ROW_HEIGHT;
    const int rows    = clamp((cursorTop + ROW_HEIGHT / 2 - viewportH / 2 + ROW_HEIGHT / 2)
                                  / ROW_HEIGHT,
                              0, maxRows);

    // Clipped to the viewport, so a scrolled row cannot overdraw the title above or spill past the
    // panel edge.
    const Canvas::ClipScope rowsClip(c, x, firstLineY, WIDTH, viewportH);
    draw_cells(c, x, firstLineY - rows * ROW_HEIGHT, lay, /*only=*/-1, s);
}

void draw_send_editor(Canvas& c, const SendEditorState& s, const songcore::Project& project,
                      const Theme& t) {
    if (!s.isOpen) return;

    // Modal: it must not be clipped by whatever editor was drawing when it opened.
    c.reset_clip();

    const int section =
        static_cast<int>(effects_row_section(static_cast<EffectsRow>(
            clamp(s.cursorRow, 0, EffectModule::MAX_CURSOR_ROW))));
    const EffectsLayout lay   = effects_layout(section);
    const int           BOX_H = box_h(lay.lineCount - 1);
    const int           BOX_Y = (DESIGN_H - BOX_H) / 2;

    draw_modal_backdrop(c);
    draw_modal_box(c, BOX_X, BOX_Y, BOX_W, BOX_H, t);

    // ⚠️ `text_width` measures ink and drops the trailing gap; the run's advance centres it.
    const std::string title = SECTION_NAMES[section];
    const int         textY = BOX_Y + CONTENT_PAD + TEXT_PADDING;
    c.draw_text(title, BOX_X + (BOX_W - static_cast<int>(title.size()) * CHAR_W) / 2, textY,
                t.textTitle, CHAR_SPACING, FONT_SCALE);

    EffectState es{project};
    es.cursorRow = s.cursorRow;
    es.theme     = t;
    draw_cells(c, BOX_X, textY + ROW_HEIGHT + TITLE_GAP, lay, section, es);
}

// ─── Cursor ──────────────────────────────────────────────────────────────────────────────────────

CursorContext EffectModule::cursor_context(const EffectState& s) const {
    const songcore::Project& p = s.project;

    switch (s.cursorRow) {
        case ROW_MASTER_TYPE: {
            // ⚠️ Built by hand: a two-state toggle gets increment and decrement and nothing else —
            // no fast step, no delete.
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

        case ROW_REV_TYPE:
            return preset_name_context(reverb_bank_cycle_index(p), kReverbPresetCount);
        case ROW_DLY_TYPE:
            return preset_name_context(delay_bank_cycle_index(p), kDelayPresetCount);

        // ⚠️ READ-ONLY rather than absent: they answer a bare A, and a `none()` context would also
        // switch off the cursor box that says which of the line's three cells you are on.
        case ROW_REV_SAVE:
        case ROW_REV_LOAD:
        case ROW_DLY_SAVE:
        case ROW_DLY_LOAD:
            return cc::read_only();

        case ROW_REV_ALGO:
            // A short named list that steps and wraps: every value is a real reverb.
            return cc::index_cycle(clamp(p.reverbAlgo, 0, kReverbAlgoCount - 1), kReverbAlgoCount);

        case ROW_REV_DECAY:
            return cc::hex_byte(p.reverbFeedback, 0, 255, -1, false, false, false, /*def=*/0x60);
        case ROW_REV_SIZE:
            return cc::hex_byte(p.reverbSize, 0, 255, -1, false, false, false, /*def=*/0x60);
        case ROW_REV_DAMP:
            return cc::hex_byte(p.reverbDamp, 0, 255, -1, false, false, false, /*def=*/0x80);
        case ROW_REV_PRE:
            return cc::hex_byte(p.reverbPreDelay, 0, 255, -1, false, false, false, /*def=*/0x00);
        case ROW_REV_WIDE:
            return cc::hex_byte(p.reverbWidth, 0, 255, -1, false, false, false, /*def=*/0x80);
        case ROW_REV_MOD:
            return cc::hex_byte(p.reverbMod, 0, 255, -1, false, false, false, /*def=*/0x40);
        case ROW_REV_EQ:
            return cc::hex_byte(p.reverbInputEq < 0 ? -1 : p.reverbInputEq, 0, 127,
                                /*empty_value=*/-1, /*can_delete=*/true, /*can_insert=*/true);

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

// ─── What the cursor is standing on, by NAME ─────────────────────────────────────────────────────

songcore::MapTarget EffectModule::map_target(const EffectState& s) const {
    using songcore::MapDestId;
    switch (static_cast<EffectsRow>(s.cursorRow)) {
        case EffectsRow::REV_DECAY:  return {MapDestId::REV_DCAY, 0};
        case EffectsRow::REV_DAMP:   return {MapDestId::REV_DAMP, 0};
        case EffectsRow::REV_PRE:    return {MapDestId::REV_PRE,  0};
        case EffectsRow::REV_WIDE:   return {MapDestId::REV_WIDE, 0};
        case EffectsRow::REV_MOD:    return {MapDestId::REV_MOD,  0};
        case EffectsRow::REV_SIZE:   return {MapDestId::REV_SIZE, 0};
        case EffectsRow::DLY_TIME:   return {MapDestId::DLY_TIME, 0};
        case EffectsRow::DLY_FDBK:   return {MapDestId::DLY_FDBK, 0};
        case EffectsRow::DLY_REV:    return {MapDestId::DLY_SEND, 0};
        case EffectsRow::DLY_TONE:   return {MapDestId::DLY_TONE, 0};
        case EffectsRow::DLY_WOBBLE: return {MapDestId::DLY_WOBL, 0};
        // ⚠️ The two WET cells are NOT here. They are the send returns and they live on the MIXER,
        // which is where the catalogue's `REV WET` and `DLY WET` are learned.
        case EffectsRow::MASTER_TYPE:
        case EffectsRow::REV_EQ:
        case EffectsRow::DLY_EQ:
        case EffectsRow::DLY_TYPE:
        case EffectsRow::DLY_PONG:
        case EffectsRow::REV_TYPE:
        case EffectsRow::REV_ALGO:
        case EffectsRow::REV_SAVE:
        case EffectsRow::REV_LOAD:
        case EffectsRow::DLY_SAVE:
        case EffectsRow::DLY_LOAD:
            break;
    }
    return {};
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

        case ROW_REV_DECAY:
            if (!isSet) break;
            p.reverbFeedback = clamp(action.value, 0, 255);
            return {true};

        case ROW_REV_SIZE:
            if (!isSet) break;
            p.reverbSize = clamp(action.value, 0, 255);
            return {true};

        case ROW_REV_ALGO:
            // ⚠️ It writes nothing but itself, so switching back and forth loses no cell.
            if (!isSet) break;
            p.reverbAlgo = clamp(action.value, 0, kReverbAlgoCount - 1);
            return {true};

        case ROW_REV_DAMP:
            if (!isSet) break;
            p.reverbDamp = clamp(action.value, 0, 255);
            return {true};

        case ROW_REV_TYPE: {
            if (!isSet || action.value < 0 || action.value >= kReverbPresetCount) break;
            // ⚠️ Compared before the write: landing back on the preset already shown must not bump the
            // dirty counter (a phantom autosave and RECOVER WORK?).
            if (reverb_bank_match(p) == action.value &&
                (p.reverbPreset.empty() || p.reverbPreset == kReverbPresets[action.value].name))
                break;
            reverb_apply_bank(p, action.value);
            return {true};
        }

        case ROW_REV_PRE:
            if (!isSet) break;
            p.reverbPreDelay = clamp(action.value, 0, 255);
            return {true};

        case ROW_REV_WIDE:
            if (!isSet) break;
            p.reverbWidth = clamp(action.value, 0, 255);
            return {true};

        case ROW_REV_MOD:
            if (!isSet) break;
            p.reverbMod = clamp(action.value, 0, 255);
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
            // As the reverb's, dirty counter included.
            if (!isSet || action.value < 0 || action.value >= kDelayPresetCount) break;
            if (delay_bank_match(p) == action.value &&
                (p.delayPreset.empty() || p.delayPreset == kDelayPresets[action.value].name))
                break;
            delay_apply_bank(p, action.value);
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
