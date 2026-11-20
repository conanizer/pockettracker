#include "ui/modules/instrument_picker_overlay.h"

#include <string>

#include "ui/helpers.h"
#include "ui/macro_model_text.h"
#include "ui/value_hint.h"

namespace pt::ui {

namespace {

// As wide as the hint. A longer name scrolls under the cursor.
constexpr int TEXT_COLS  = 18;
constexpr int TEXT_INSET = 14;
constexpr int BAR_INSET  = 8;   // the cursor bar
constexpr int BOX_W      = 2 * TEXT_INSET + TEXT_COLS * CHAR_W - CHAR_SPACING;
constexpr int BOX_X      = (DESIGN_W - BOX_W) / 2;
constexpr int GAP        = 6;   // air above and below the list
constexpr int NAME_X     = 3 * CHAR_W;   // after "0C "
static_assert(BOX_W + 2 * (MODAL_BORDER - 1) <= DESIGN_W, "the instrument list is wider than the screen");

// The title, air, the rows, air, the hint.
constexpr int BOX_H = 2 * BAR_INSET + ROW_HEIGHT + GAP + INSTRUMENT_PICKER_ROWS * ROW_HEIGHT + GAP + ROW_HEIGHT;
static_assert(BOX_H + 2 * (MODAL_BORDER - 1) <= DESIGN_H, "the instrument list is taller than the screen");

// The preset list: "128 " and a SoundFont name of up to 20 letters.
constexpr int SF_TEXT_COLS = 24;
constexpr int SF_BOX_W     = 2 * TEXT_INSET + SF_TEXT_COLS * CHAR_W - CHAR_SPACING;
constexpr int SF_BOX_X     = (DESIGN_W - SF_BOX_W) / 2;
static_assert(SF_BOX_W + 2 * (MODAL_BORDER - 1) <= DESIGN_W, "the preset list is wider than the screen");

// The model list: a model's four letters, a space, and its note, as wide as the longest note.
constexpr int macro_longest_about() {
    int most = 0;
    for (const MacroModelText& m : MACRO_MODEL_TEXT) {
        int n = 0;
        while (m.about[n] != '\0') ++n;
        most = n > most ? n : most;
    }
    return most;
}
constexpr int MACRO_ABOUT_COL  = 5;
constexpr int MACRO_TEXT_COLS  = MACRO_ABOUT_COL + macro_longest_about();
constexpr int MACRO_BOX_W      = 2 * TEXT_INSET + MACRO_TEXT_COLS * CHAR_W - CHAR_SPACING;
constexpr int MACRO_BOX_X      = (DESIGN_W - MACRO_BOX_W) / 2;
static_assert(MACRO_TEXT_COLS >= 18, "the model list is narrower than its hint");
static_assert(MACRO_BOX_W + 2 * (MODAL_BORDER - 1) <= DESIGN_W, "the model list is wider than the screen");

}  // namespace

void draw_instrument_picker(Canvas& c, const InstrumentPickerState& s,
                            const std::vector<songcore::Instrument>& instruments, const Theme& t) {
    if (!s.isOpen) return;
    c.reset_clip();

    const int BOX_Y = (DESIGN_H - BOX_H) / 2;
    draw_modal_backdrop(c);
    draw_modal_box(c, BOX_X, BOX_Y, BOX_W, BOX_H, t);

    const int x = BOX_X + TEXT_INSET;
    int       y = BOX_Y + BAR_INSET;
    c.label(c.cased("INSTRUMENT"), x, y + TEXT_PADDING, t.textTitle);
    y += ROW_HEIGHT + GAP;

    const int n = static_cast<int>(instruments.size());
    for (int i = s.top; i < n && i < s.count && i < s.top + INSTRUMENT_PICKER_ROWS; ++i) {
        const songcore::Instrument& ins      = instruments[static_cast<size_t>(i)];
        const int                   ry       = y + (i - s.top) * ROW_HEIGHT;
        const bool                  isCursor = i == s.cursor;
        const std::string           name     = instrument_label(c, ins);
        const bool                  dim      = name == c.cased("EMPTY");
        if (isCursor) c.fill_rect(BOX_X + BAR_INSET, ry, BOX_W - 2 * BAR_INSET, ROW_HEIGHT, t.rowCursor);
        const Argb ink = isCursor ? cursor_cell_ink(t) : dim ? t.textEmpty : t.textValue;
        c.text(hex2(i), x, ry + TEXT_PADDING, isCursor ? ink : t.textParam);
        c.text(c.fit_text(name, TEXT_COLS - 3, isCursor), x + NAME_X, ry + TEXT_PADDING, ink);
    }
    y += INSTRUMENT_PICKER_ROWS * ROW_HEIGHT + GAP;

    c.label(c.cased("A=CHOOSE  B=CANCEL"), x, y + TEXT_PADDING, t.textParam);
}

void draw_sf_preset_picker(Canvas& c, const SfPresetPickerState& s, const Theme& t) {
    if (!s.isOpen) return;
    c.reset_clip();

    const int BOX_Y = (DESIGN_H - BOX_H) / 2;
    draw_modal_backdrop(c);
    draw_modal_box(c, SF_BOX_X, BOX_Y, SF_BOX_W, BOX_H, t);

    const int x = SF_BOX_X + TEXT_INSET;
    int       y = BOX_Y + BAR_INSET;
    c.label(c.cased("PATCH"), x, y + TEXT_PADDING, t.textTitle);
    y += ROW_HEIGHT + GAP;

    // Numbered from 1, as the PATCH row counts them, right-aligned to the longest number.
    const int digits = static_cast<int>(std::to_string(s.count).size());
    const int nameX  = (digits + 1) * CHAR_W;
    for (int i = s.top; i < s.count && i < s.top + INSTRUMENT_PICKER_ROWS; ++i) {
        const int  ry       = y + (i - s.top) * ROW_HEIGHT;
        const bool isCursor = i == s.cursor;
        if (isCursor) c.fill_rect(SF_BOX_X + BAR_INSET, ry, SF_BOX_W - 2 * BAR_INSET, ROW_HEIGHT, t.rowCursor);
        const Argb  ink = isCursor ? cursor_cell_ink(t) : t.textValue;
        std::string num = std::to_string(i + 1);
        num.insert(0, static_cast<size_t>(digits) - num.size(), ' ');
        c.text(num, x, ry + TEXT_PADDING, isCursor ? ink : t.textParam);
        c.text(c.fit_text(s.names[static_cast<size_t>(i)], SF_TEXT_COLS - digits - 1, isCursor), x + nameX,
               ry + TEXT_PADDING, ink);
    }
    y += INSTRUMENT_PICKER_ROWS * ROW_HEIGHT + GAP;

    c.label(c.cased("A=CHOOSE  B=CANCEL"), x, y + TEXT_PADDING, t.textParam);
}

// Each model as the MODEL row shows it: the name in the values' ink, the note in the labels'.
void draw_macro_model_picker(Canvas& c, const InstrumentPickerState& s, const Theme& t) {
    if (!s.isOpen) return;
    c.reset_clip();

    const int BOX_Y = (DESIGN_H - BOX_H) / 2;
    draw_modal_backdrop(c);
    draw_modal_box(c, MACRO_BOX_X, BOX_Y, MACRO_BOX_W, BOX_H, t);

    const int x = MACRO_BOX_X + TEXT_INSET;
    int       y = BOX_Y + BAR_INSET;
    c.label(c.cased("MODEL"), x, y + TEXT_PADDING, t.textTitle);
    y += ROW_HEIGHT + GAP;

    for (int i = s.top; i < s.count && i < s.top + INSTRUMENT_PICKER_ROWS; ++i) {
        const int  ry       = y + (i - s.top) * ROW_HEIGHT;
        const bool isCursor = i == s.cursor;
        if (isCursor) c.fill_rect(MACRO_BOX_X + BAR_INSET, ry, MACRO_BOX_W - 2 * BAR_INSET, ROW_HEIGHT, t.rowCursor);
        const Argb ink = isCursor ? cursor_cell_ink(t) : t.textValue;
        c.text(songcore::macro_model_name(i), x, ry + TEXT_PADDING, ink);
        c.text(MACRO_MODEL_TEXT[i].about, x + MACRO_ABOUT_COL * CHAR_W, ry + TEXT_PADDING,
               isCursor ? ink : t.textParam);
    }
    y += INSTRUMENT_PICKER_ROWS * ROW_HEIGHT + GAP;

    c.label(c.cased("A=CHOOSE  B=CANCEL"), x, y + TEXT_PADDING, t.textParam);
}

}  // namespace pt::ui
