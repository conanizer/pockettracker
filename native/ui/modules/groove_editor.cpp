#include "ui/modules/groove_editor.h"

#include "songcore/groove_bank.h"
#include "songcore/timing.h"
#include "ui/helpers.h"

namespace pt::ui {

namespace {

// ── The panel's columns ──────────────────────────────────────────────────────────────────────────
//
// The tick grid ends at x+84 (a two-glyph cell starting at x+50), so the panel starts clear of it
// with a gap wide enough to read as a separate thing rather than a third column of the grid.
//
// ⚠️ THE NAME GETS EVERYTHING TO THE RIGHT EDGE — the longest entry in the bank is "DOUBLETIME" at
// ten characters and the label column is twenty-one wide, so nothing in the bank needs abbreviating.
// A loaded `.ptg` can be named anything, so the draw truncates rather than trusting that.
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
    // The active length is the sequencer's own answer (songcore/timing.h) — the steps before the first
    // −1. The UI must not re-derive it: a second definition of "where does this groove end" is exactly
    // the kind of drift that makes the picture disagree with what you hear.
    const int activeLen = songcore::groove_active_length(groove);

    c.draw_text("GROOVE " + hex2(groove.id), x + 10, headerY, t.textTitle, CHAR_SPACING, FONT_SCALE);

    std::string lenText = std::to_string(activeLen);
    if (lenText.size() < 2) lenText = " " + lenText;  // padStart(2, ' ')
    c.draw_text("LEN:" + lenText, x + WIDTH - 130, headerY, t.textParam, CHAR_SPACING, FONT_SCALE);

    // ── Column header ────────────────────────────────────────────────────────────────────────────
    // It lights for the column the cursor is in, as the other grids' headers do.
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

        // The cursor is a background under ONE cell, as on every other grid; the row number lights
        // across the cursor row to say which row, the header to say which column.
        draw_cell(c, hex1(index), stepX, rowY, /*is_cursor=*/false,
                  /*is_selected=*/false, /*is_empty=*/false,
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

    // NAME. Derived first and stored second — see groove_display_name. `----` is a groove built by
    // hand that the bank has no word for; the `*` is one that has drifted from the name it carries.
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
    draw_cell(c, "SAVE", x + GROOVE_PANEL_X, panelY(GROOVE_PANEL_FILE), here(GROOVE_PANEL_FILE, 0),
              /*is_selected=*/false, /*is_empty=*/false, t.textParam, t);
    draw_cell(c, "LOAD", x + GROOVE_LOAD_X, panelY(GROOVE_PANEL_FILE), here(GROOVE_PANEL_FILE, 1),
              /*is_selected=*/false, /*is_empty=*/false, t.textParam, t);

    // QNT — the editing aid, not a stored setting.
    const int qntY = panelY(GROOVE_PANEL_QNT);
    c.draw_text("QNT", x + GROOVE_PANEL_X, qntY,
                here(GROOVE_PANEL_QNT, 0) ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING,
                FONT_SCALE);
    draw_cursor_cell(c, groove_quantize_label(s.quantize), x + GROOVE_VALUE_X, qntY,
                     here(GROOVE_PANEL_QNT, 0), t.textValue, t);

    // SWG — read-only, and it reports on the group the TICK cursor is in, not the panel cursor.
    const int swgY = panelY(GROOVE_PANEL_SWG);
    c.draw_text("SWG", x + GROOVE_PANEL_X, swgY,
                here(GROOVE_PANEL_SWG, 0) ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING,
                FONT_SCALE);
    draw_cursor_cell(c, swing_text(groove_swing_tenths(groove, s.cursorRow, s.quantize)),
                     x + GROOVE_VALUE_X, swgY, here(GROOVE_PANEL_SWG, 0), t.textParam, t);
}

CursorContext GrooveModule::cursor_context(const GrooveState& s) const {
    if (s.cursorColumn == GROOVE_COL_PANEL) {
        switch (s.panelRow) {
            case GROOVE_PANEL_NAME: {
                // The factory list, cycled the way SCALE's name row cycles the scale bank.
                CursorContext c = cc::index_cycle(songcore::groove_bank_cycle_index(s.groove),
                                                  static_cast<int>(songcore::groove_bank().size()));
                c.defaultValue = 0;  // A+B: back to STRAIGHT, which is also a new groove's own steps
                return c;
            }
            case GROOVE_PANEL_QNT:
                return cc::index_cycle(s.quantize, GROOVE_QUANTIZE_COUNT);
            default:
                // ⚠️ SAVE and LOAD are READ-ONLY rather than absent: they answer a bare A, and a
                // `none()` would also switch off the cursor box that says which one you are on. SWG
                // is a readout and takes the same context for the same reason.
                return cc::read_only();
        }
    }

    // Column 0 is the row-number gutter. Unreachable in the app — no sideways move stops there — but
    // it must still answer, and what it answers is "not a cell".
    if (s.cursorColumn != GROOVE_COL_TICK) return cc::read_only();

    const int tickValue = s.groove.steps[static_cast<size_t>(s.cursorRow)];

    CursorContext c = cc::hex_byte(tickValue, /*min=*/0,  // 00 = skip the step
                                   /*max=*/255,
                                   /*empty_value=*/-1,  // −1 = the end-of-pattern marker
                                   /*can_delete=*/tickValue != -1,
                                   /*can_insert=*/tickValue == -1);

    // ⚠️ ARMED, THE CELL IS STILL EMPTY WHEN IT IS EMPTY, and the insert is why. A blank step answers
    // a bare A by laying a step down at the neutral length, on every screen and at every quantize
    // setting; reporting it as a materialized 12 so that the first press could swing instead took
    // that gesture away whenever the aid was armed. A blank PARTNER is materialized all the same —
    // that happens in the edit, where the whole group is filled at once.
    //
    // What armed does change: both A axes move the pair by ONE tick, and the coarse move is gone.
    // One tick is one named swing rung, so there is no useful larger step, and ±16 on a pair is
    // refused at every quantize setting anyway. Raw editing is what OFF is for, and there 16 stands.
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
        }
        return r;
    }
    if (s.cursorColumn != GROOVE_COL_TICK) return r;  // the row-number gutter edits nothing

    int& step = groove.steps[static_cast<size_t>(s.cursorRow)];
    switch (action.type) {
        case ActionType::SET_VALUE:
            if (s.quantize != 0) {
                // ⚠️ THE DELTA IS DERIVED, NOT PASSED. The generic step already wrapped FF→00 for us,
                // so the difference is taken the short way round the byte; the paired rule then
                // refuses anything that would leave 0..255 rather than letting a wrap through.
                // `step` is never the end marker here: an empty cell answers A with an INSERT.
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
            step = songcore::TICS_PER_STEP;  // A on "--": the standard 12 tics per step
            break;
        default:
            break;
    }

    r.modified = (action.type != ActionType::NONE);
    return r;
}

}  // namespace pt::ui
