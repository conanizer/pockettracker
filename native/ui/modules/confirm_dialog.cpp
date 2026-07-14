#include "ui/modules/confirm_dialog.h"

#include "ui/helpers.h"

namespace pt::ui {

namespace {

// The geometry, verbatim from drawSimpleConfirmDialog.
constexpr int BOX_W = 260;
constexpr int BOX_H = 55;
constexpr int BOX_X = (DESIGN_W - BOX_W) / 2;   // 190
constexpr int BOX_Y = (DESIGN_H - BOX_H) / 2;   // 212

constexpr Argb BACKDROP = 0xCC000000;  // translucent — the canvas blends it

// The dialog draws at a bigger font than the editors: fontScale 3, spacing 2.
constexpr int DLG_FONT_SCALE   = 3;
constexpr int DLG_CHAR_SPACING = 2;

}  // namespace

std::string confirm_dialog_title(ConfirmDialogState::Kind kind) {
    switch (kind) {
        case ConfirmDialogState::Kind::CLEAN_SEQ:   return "CLEAN SEQ?";
        case ConfirmDialogState::Kind::CLEAN_INST:  return "CLEAN INST?";
        case ConfirmDialogState::Kind::NEW_PROJECT: return "NEW PROJECT?";
        case ConfirmDialogState::Kind::CHANGE_TYPE: return "CHANGE TYPE?";
        case ConfirmDialogState::Kind::EXIT:        return "EXIT?";
        case ConfirmDialogState::Kind::NONE:        break;
    }
    return "";
}

void draw_confirm_dialog(Canvas& c, const ConfirmDialogState& s, const Theme& t) {
    if (!s.is_open()) return;

    const std::string title       = confirm_dialog_title(s.kind);
    const std::string instruction = "A=YES  B=NO";

    c.fill_rect(0, 0, DESIGN_W, DESIGN_H, BACKDROP);
    c.fill_rect(BOX_X, BOX_Y, BOX_W, BOX_H, t.meterBackground);
    c.stroke_rect(BOX_X, BOX_Y, BOX_W, BOX_H, t.textTitle);

    const int titleW = Canvas::text_width(title, DLG_CHAR_SPACING, DLG_FONT_SCALE);
    const int instrW = Canvas::text_width(instruction, DLG_CHAR_SPACING, DLG_FONT_SCALE);

    c.draw_text(title, BOX_X + (BOX_W - titleW) / 2, BOX_Y + 8, t.textTitle,
                DLG_CHAR_SPACING, DLG_FONT_SCALE);
    c.draw_text(instruction, BOX_X + (BOX_W - instrW) / 2, BOX_Y + 30, t.textCursor,
                DLG_CHAR_SPACING, DLG_FONT_SCALE);
}

}  // namespace pt::ui
