#include "ui/modules/confirm_dialog.h"

#include "ui/helpers.h"

namespace pt::ui {

namespace {

// The geometry: drawSimpleConfirmDialog's 260×55. The border grows outward from it (helpers.h), so
// these are the fill, not the outside edge.
constexpr int BOX_W = 260;
constexpr int BOX_H = 55;
constexpr int BOX_X = (DESIGN_W - BOX_W) / 2;
constexpr int BOX_Y = (DESIGN_H - BOX_H) / 2;

// The dialog draws at a bigger font than the editors: fontScale 3, spacing 2.
constexpr int DLG_FONT_SCALE   = 3;
constexpr int DLG_CHAR_SPACING = 2;

// Two lines of type, a fixed distance apart, and the PAIR is centred in the box. ⚠️ The top offset is
// derived rather than typed: a hand-typed top pad is a pad that stops being centred the moment the
// box's height or the line pitch moves, and it leaves the two lines sitting high with all the slack
// under them — which is exactly what it did.
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
        case ConfirmDialogState::Kind::RECOVER:     return "RECOVER WORK?";   // PixelPerfectRenderer:866
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
    c.draw_text(instruction, BOX_X + (BOX_W - instrW) / 2, BOX_Y + DLG_INSTR_Y, t.textCursor,
                DLG_CHAR_SPACING, DLG_FONT_SCALE);
}

void draw_confirm_dialog(Canvas& c, const ConfirmDialogState& s, const Theme& t) {
    if (!s.is_open()) return;
    draw_modal_backdrop(c);
    draw_confirm_box(c, confirm_dialog_title(s.kind), t);
}

}  // namespace pt::ui
