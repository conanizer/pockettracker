#include "ui/modules/confirm_dialog.h"

#include "ui/helpers.h"

#include <algorithm>

namespace pt::ui {

namespace {

// The fill; the border grows outward from it (helpers.h).
constexpr int BOX_W = 260;
constexpr int BOX_H = 55;
constexpr int BOX_X = (DESIGN_W - BOX_W) / 2;
constexpr int BOX_Y = (DESIGN_H - BOX_H) / 2;

// A bigger font than the editors.
constexpr int DLG_FONT_SCALE   = 3;
constexpr int DLG_CHAR_SPACING = 2;

// Two lines a fixed distance apart, the pair centred.
constexpr int DLG_LINE_PITCH = 22;
constexpr int DLG_GLYPH_H    = 5 * DLG_FONT_SCALE;
constexpr int DLG_CONTENT_H  = DLG_LINE_PITCH + DLG_GLYPH_H;
constexpr int DLG_TITLE_Y    = (BOX_H - DLG_CONTENT_H) / 2;
constexpr int DLG_INSTR_Y    = DLG_TITLE_Y + DLG_LINE_PITCH;

}  // namespace

std::string confirm_dialog_title(ConfirmDialogState::Kind kind) {
    switch (kind) {
        case ConfirmDialogState::Kind::CLEAN_SEQ:   return "CLEAN SEQ?";
        case ConfirmDialogState::Kind::CLEAN_INST:  return "CLEAN INST?";
        case ConfirmDialogState::Kind::NEW_PROJECT: return "NEW PROJECT?";
        case ConfirmDialogState::Kind::CHANGE_TYPE: return "CHANGE TYPE?";
        case ConfirmDialogState::Kind::EXIT:        return "EXIT?";
        case ConfirmDialogState::Kind::RECOVER:     return "RECOVER WORK?";
        case ConfirmDialogState::Kind::COLLECT_EXISTS: return "ALREADY IN SONGS";
        case ConfirmDialogState::Kind::NONE:        break;
    }
    return "";
}

void draw_confirm_box(Canvas& c, const std::string& title, const Theme& t) {
    const std::string instruction = "A=YES  B=NO";

    draw_modal_box(c, BOX_X, BOX_Y, BOX_W, BOX_H, t);

    const int titleW = Canvas::text_width(title, DLG_CHAR_SPACING, DLG_FONT_SCALE);
    const int instrW = Canvas::text_width(instruction, DLG_CHAR_SPACING, DLG_FONT_SCALE);

    c.draw_text(title, BOX_X + (BOX_W - titleW) / 2, BOX_Y + DLG_TITLE_Y, t.textTitle,
                DLG_CHAR_SPACING, DLG_FONT_SCALE);
    // ⚠️ The YES line takes TXT VALUE, the one ink role bright in every palette.
    c.draw_text(instruction, BOX_X + (BOX_W - instrW) / 2, BOX_Y + DLG_INSTR_Y, t.textValue,
                DLG_CHAR_SPACING, DLG_FONT_SCALE);
}

namespace {

// The question, and its answers as cursor cells in place of "A=YES  B=NO". As wide as its text needs.
void draw_choice_box(Canvas& c, const std::string& question,
                     std::initializer_list<const char*> answers, int chosen, const Theme& t) {
    constexpr int GAP = 24;   // between answers
    constexpr int PAD = 24;   // either side of the widest line

    const auto width_of = [](const std::string& s) {
        return Canvas::text_width(s, DLG_CHAR_SPACING, DLG_FONT_SCALE);
    };

    int width = -GAP;
    for (const char* a : answers) width += width_of(a) + GAP;

    const int boxW = std::max(BOX_W, std::max(width_of(question), width) + 2 * PAD);
    const int boxX = (DESIGN_W - boxW) / 2;
    draw_modal_box(c, boxX, BOX_Y, boxW, BOX_H, t);

    c.draw_text(question, boxX + (boxW - width_of(question)) / 2, BOX_Y + DLG_TITLE_Y, t.textTitle,
                DLG_CHAR_SPACING, DLG_FONT_SCALE);

    int x = boxX + (boxW - width) / 2;
    int i = 0;
    for (const char* a : answers) {
        draw_cursor_cell(c, a, x, BOX_Y + DLG_INSTR_Y, i == chosen, t.textValue, t);
        x += width_of(a) + GAP;
        ++i;
    }
}

}  // namespace

void draw_confirm_dialog(Canvas& c, const ConfirmDialogState& s, const Theme& t) {
    if (!s.is_open()) return;
    draw_modal_backdrop(c);
    if (s.kind == ConfirmDialogState::Kind::COLLECT_EXISTS)
        draw_choice_box(c, confirm_dialog_title(s.kind), {"REPLACE", "NEW COPY", "CANCEL"}, s.arg, t);
    else
        draw_confirm_box(c, confirm_dialog_title(s.kind), t);
}

}  // namespace pt::ui
