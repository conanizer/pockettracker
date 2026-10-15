#include "ui/modules/groove_editor.h"

#include "songcore/groove_bank.h"
#include "songcore/timing.h"
#include "ui/helpers.h"

namespace pt::ui {

namespace {

// ── The panel's columns, clear of the tick grid; a loaded `.ptg` name is truncated ───────────────
constexpr int GROOVE_PANEL_X = 140;
constexpr int GROOVE_STAR_X  = 140;  // the name shifts one glyph right when the marker is up
constexpr int GROOVE_VALUE_X = 140 + 4 * CHAR_W;  // past "QNT " / "SWG "
constexpr int GROOVE_LOAD_X  = 140 + 5 * CHAR_W;  // past "SAVE "
constexpr int GROOVE_NAME_MAX_CHARS = 21;

/** Tenths of a percent as "66.7%". */
std::string swing_text(int tenths) {
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "%";
}

}  // namespace

void GrooveModule::draw(Canvas& c, int x, int y, const GrooveState& s) const {
    const Theme&            t      = s.theme;
    const songcore::Groove& groove = s.groove;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    const int stepX = x + 10;
    const int tickX = x + 10 + 40;

    // ── Header ───────────────────────────────────────────────────────────────────────────────────
    const int headerY = y + TEXT_PADDING;
    // The sequencer's own active length (timing.h), never re-derived here.
    const int activeLen = songcore::groove_active_length(groove);

    c.draw_text("GROOVE " + hex2(groove.id), x + 10, headerY, t.textTitle, CHAR_SPACING, FONT_SCALE);

    std::string lenText = std::to_string(activeLen);
    if (lenText.size() < 2) lenText = " " + lenText;
    c.draw_text("LEN:" + lenText, x + WIDTH - 130, headerY, t.textParam, CHAR_SPACING, FONT_SCALE);

    // ── Column header ────────────────────────────────────────────────────────────────────────────
    // Lit for the cursor's column.
    c.draw_text("TIC", tickX, y + ROW_HEIGHT + 14 + TEXT_PADDING,
                header_color(s.cursorColumn, GROOVE_COL_TICK, GROOVE_COL_TICK, t), CHAR_SPACING,
                FONT_SCALE);

    // ── 16 data rows ─────────────────────────────────────────────────────────────────────────────
    const int dataStartY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + TEXT_PADDING;
    const bool onGrid    = (s.cursorColumn == GROOVE_COL_TICK);

    for (int index = 0; index < 16; ++index) {
        const int  tickValue   = groove.steps[static_cast<size_t>(index)];
        const int  rowY        = dataStartY + (index * ROW_HEIGHT);  // the TEXT y, not the row's top
        const bool isCursor    = (index == s.cursorRow);
        const bool isEndMarker = (tickValue == -1);
        const bool isPastEnd   = (index >= activeLen);  // past the first −1: inactive, drawn dim

        // The cursor is one cell; the row number and header say which row and column.
        draw_cursor_cell(c, hex1(index), stepX, rowY, /*is_cursor=*/false,
                         isCursor ? cursor_mark_ink(t) : t.textEmpty, t);

        draw_cell(c, isEndMarker ? "--" : hex2(tickValue), tickX, rowY, isCursor && onGrid,
                  /*is_selected=*/false, /*is_empty=*/isEndMarker || isPastEnd, t.textValue, t);
    }

    // ── The panel ────────────────────────────────────────────────────────────────────────────────
    const bool onPanel = (s.cursorColumn == GROOVE_COL_PANEL);
    auto panelY = [&](int row) { return dataStartY + (row * ROW_HEIGHT); };
    auto here   = [&](int row, int column) {
        return onPanel && s.panelRow == row && s.panelColumn == column;
    };

    // NAME (groove_display_name). `----` = a hand-built groove; `*` = drifted from its name.
    std::string shown = songcore::groove_display_name(groove);
    const bool  named = !shown.empty();
    if (!named) shown = "----";
    if (static_cast<int>(shown.size()) > GROOVE_NAME_MAX_CHARS)
        shown.resize(GROOVE_NAME_MAX_CHARS);

    const bool marked = songcore::groove_differs_from_its_name(groove);
    if (marked)
        c.draw_text("*", x + GROOVE_STAR_X, panelY(GROOVE_PANEL_NAME), t.textPlayhead, CHAR_SPACING,
                    FONT_SCALE);

    draw_cell(c, shown, x + GROOVE_PANEL_X + (marked ? CHAR_W : 0), panelY(GROOVE_PANEL_NAME),
              here(GROOVE_PANEL_NAME, 0), /*is_selected=*/false, /*is_empty=*/!named, t.textValue, t);

    // SAVE · LOAD
    const int fileY = panelY(GROOVE_PANEL_FILE);
    draw_cursor_cell(c, "SAVE", x + GROOVE_PANEL_X, fileY, here(GROOVE_PANEL_FILE, 0), t.textParam, t);
    draw_cursor_cell(c, "LOAD", x + GROOVE_LOAD_X, fileY, here(GROOVE_PANEL_FILE, 1), t.textParam, t);

    // QNT — the editing aid, not a stored setting.
    const int qntY = panelY(GROOVE_PANEL_QNT);
    c.draw_text("QNT", x + GROOVE_PANEL_X, qntY,
                here(GROOVE_PANEL_QNT, 0) ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING,
                FONT_SCALE);
    draw_cursor_cell(c, groove_quantize_label(s.quantize), x + GROOVE_VALUE_X, qntY,
                     here(GROOVE_PANEL_QNT, 0), t.textValue, t);

    // SWG — read-only, for the tick cursor's group.
    const int swgY = panelY(GROOVE_PANEL_SWG);
    c.draw_text("SWG", x + GROOVE_PANEL_X, swgY,
                here(GROOVE_PANEL_SWG, 0) ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING,
                FONT_SCALE);
    draw_cursor_cell(c, swing_text(groove_swing_tenths(groove, s.cursorRow, s.quantize)),
                     x + GROOVE_VALUE_X, swgY, here(GROOVE_PANEL_SWG, 0), t.textParam, t);

    // PPQ — tick column tics per quarter note.
    const int ppqY = panelY(GROOVE_PANEL_PPQ);
    c.draw_text("PPQ", x + GROOVE_PANEL_X, ppqY,
                here(GROOVE_PANEL_PPQ, 0) ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING,
                FONT_SCALE);
    draw_cursor_cell(c, std::to_string(songcore::groove_ticks_per_step(groove) * 4),
                     x + GROOVE_VALUE_X, ppqY, here(GROOVE_PANEL_PPQ, 0), t.textValue, t);
}

CursorContext GrooveModule::cursor_context(const GrooveState& s) const {
    if (s.cursorColumn == GROOVE_COL_PANEL) {
        switch (s.panelRow) {
            case GROOVE_PANEL_NAME: {
                // The factory list, cycled as SCALE's name row.
                CursorContext c = cc::index_cycle(songcore::groove_bank_cycle_index(s.groove),
                                                  static_cast<int>(songcore::groove_bank().size()));
                c.defaultValue = 0;  // A+B: STRAIGHT, a new groove's own steps
                return c;
            }
            case GROOVE_PANEL_QNT:
                return cc::index_cycle(s.quantize, GROOVE_QUANTIZE_COUNT);
            case GROOVE_PANEL_PPQ: {
                // ⚠️ A nibble, because it clamps: 96 wrapping to 24 would quarter every step.
                CursorContext c = cc::hex_nibble(groove_ruler_index(s.groove), /*def=*/1);
                c.maxValue  = songcore::GROOVE_RULER_COUNT - 1;
                c.largeStep = c.smallStep;
                return c;
            }
            default:
                // ⚠️ Read-only, not absent: SAVE/LOAD answer a bare A, and `none()` would hide the
                // cursor box. SWG, a readout, likewise.
                return cc::read_only();
        }
    }

    // Column 0, the gutter: unreachable, but it must answer "not a cell".
    if (s.cursorColumn != GROOVE_COL_TICK) return cc::read_only();

    const int tickValue = s.groove.steps[static_cast<size_t>(s.cursorRow)];

    CursorContext c = cc::hex_byte(tickValue, /*min=*/0,  // 00 = skip the step
                                   /*max=*/255,
                                   /*empty_value=*/-1,  // −1 = the end-of-pattern marker
                                   /*can_delete=*/tickValue != -1,
                                   /*can_insert=*/tickValue == -1);

    // ⚠️ Armed, an empty cell still reports empty, so A lays down a neutral step (the edit fills
    // the group). Armed, both A axes move the pair one tick; raw ±16 is what OFF is for.
    if (s.quantize != 0) c.largeStep = c.smallStep;
    return c;
}

GrooveInputResult GrooveModule::handle_input(songcore::Groove& groove, const GrooveState& s,
                                             const InputAction& action) const {
    GrooveInputResult r;

    if (s.cursorColumn == GROOVE_COL_PANEL) {
        if (action.type != ActionType::SET_VALUE) return r;
        if (s.panelRow == GROOVE_PANEL_NAME) {
            songcore::groove_apply_bank(groove, action.value);
            r.modified = true;
        } else if (s.panelRow == GROOVE_PANEL_QNT) {
            r.newQuantize = action.value;  // not the song's — the dispatcher owns the pointer
        } else if (s.panelRow == GROOVE_PANEL_PPQ) {
            const int index = action.value < 0 ? 0
                            : (action.value >= songcore::GROOVE_RULER_COUNT ? songcore::GROOVE_RULER_COUNT - 1
                                                                            : action.value);
            const int ruler = songcore::GROOVE_RULERS[index];
            if (ruler == songcore::groove_ticks_per_step(groove)) return r;
            r.modified     = songcore::groove_set_ruler(groove, ruler);
            r.rulerRefused = !r.modified;
        }
        return r;
    }
    if (s.cursorColumn != GROOVE_COL_TICK) return r;  // the row-number gutter edits nothing

    int& step = groove.steps[static_cast<size_t>(s.cursorRow)];
    switch (action.type) {
        case ActionType::SET_VALUE:
            if (s.quantize != 0) {
                // ⚠️ The delta is derived the short way round the byte (the generic step already
                // wrapped FF→00); the paired rule refuses anything outside 0..255.
                const int delta = ((action.value - step + 128) & 0xFF) - 128;
                r.modified      = groove_apply_paired(groove, s.cursorRow, delta, s.quantize);
                return r;
            }
            step = action.value < 0 ? 0 : (action.value > 255 ? 255 : action.value);
            break;
        case ActionType::DELETE:
            step = -1;  // A+B: back to the end-of-pattern marker
            break;
        case ActionType::INSERT_DEFAULT:
            step = songcore::groove_ticks_per_step(groove);  // A on "--": one plain step
            break;
        default:
            break;
    }

    r.modified = (action.type != ActionType::NONE);
    return r;
}

}  // namespace pt::ui
