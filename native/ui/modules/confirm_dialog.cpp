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

// Two lines a fixed distance apart, the pair centred.
constexpr int DLG_LINE_PITCH = 22;
constexpr int DLG_GLYPH_H    = ROW_HEIGHT - 2 * TEXT_PADDING;
constexpr int DLG_CONTENT_H  = DLG_LINE_PITCH + DLG_GLYPH_H;
constexpr int DLG_TITLE_Y    = (BOX_H - DLG_CONTENT_H) / 2;
constexpr int DLG_INSTR_Y    = DLG_TITLE_Y + DLG_LINE_PITCH;

}  // namespace

std::string confirm_dialog_title(ConfirmDialogState::Kind kind) {
    switch (kind) {
        case ConfirmDialogState::Kind::CLEAN_SEQ:   return "CLEAN SEQ?";
        case ConfirmDialogState::Kind::CLEAN_INST:  return "CLEAN INST?";
        case ConfirmDialogState::Kind::NEW_PROJECT: return "NEW PROJECT?";
        case ConfirmDialogState::Kind::CHANGE_TYPE: return "KEEP SETTINGS?";
        case ConfirmDialogState::Kind::EXIT:        return "EXIT?";
        case ConfirmDialogState::Kind::RECOVER:     return "RECOVER WORK?";
        case ConfirmDialogState::Kind::COLLECT_EXISTS: return "ALREADY IN SONGS";
        case ConfirmDialogState::Kind::PRESET_UNSAVED: return "PRESET NOT SAVED";
        case ConfirmDialogState::Kind::TOP_PANEL:   return "SHOW HINTS AT THE TOP?";
        case ConfirmDialogState::Kind::NONE:        break;
    }
    return "";
}

void draw_confirm_box(Canvas& c, const std::string& question, const Theme& t) {
    const std::string title       = c.cased(question);
    const std::string instruction = c.cased("A=YES  B=NO");

    draw_modal_box(c, BOX_X, BOX_Y, BOX_W, BOX_H, t);

    const int titleW = c.text_width(title);
    const int instrW = c.text_width(instruction);

    c.text(title, BOX_X + (BOX_W - titleW) / 2, BOX_Y + DLG_TITLE_Y, t.textTitle);
    // ⚠️ The YES line takes TXT VALUE, the one ink role bright in every palette.
    c.text(instruction, BOX_X + (BOX_W - instrW) / 2, BOX_Y + DLG_INSTR_Y, t.textValue);
}

std::vector<const char*> confirm_dialog_note(ConfirmDialogState::Kind kind) {
    if (kind == ConfirmDialogState::Kind::TOP_PANEL) return {"CHANGE IT LATER IN", "SETTINGS > TOP PANEL"};
    return {};
}

std::vector<const char*> confirm_choices(ConfirmDialogState::Kind kind) {
    if (kind == ConfirmDialogState::Kind::COLLECT_EXISTS) return {"REPLACE", "NEW COPY", "CANCEL"};
    if (kind == ConfirmDialogState::Kind::PRESET_UNSAVED) return {"SAVE", "DISCARD", "CANCEL"};
    if (kind == ConfirmDialogState::Kind::CHANGE_TYPE)    return {"DISCARD", "KEEP", "CANCEL"};
    if (kind == ConfirmDialogState::Kind::TOP_PANEL)      return {"YES", "NO"};
    return {};
}

namespace {

// The question, the note's lines under it, and the answers as cursor cells in place of
// "A=YES  B=NO". As wide as its text needs; each note line makes it a line taller.
void draw_choice_box(Canvas& c, const std::string& question, const std::vector<const char*>& note,
                     const std::vector<const char*>& answers, int chosen, const Theme& t) {
    constexpr int GAP = 24;   // between answers
    constexpr int PAD = 24;   // either side of the widest line

    const std::string title = c.cased(question);
    const int titleW = c.text_width(title);
    int width = -GAP;
    for (const char* a : answers) width += c.text_width(c.cased(a)) + GAP;

    int textW = std::max(titleW, width);
    for (const char* line : note) textW = std::max(textW, c.text_width(c.cased(line)));

    constexpr int NOTE_GAP = 8;   // sets the answers apart from the note above them
    const int noteH = note.empty() ? 0 : static_cast<int>(note.size()) * DLG_LINE_PITCH + NOTE_GAP;
    const int boxW = std::max(BOX_W, textW + 2 * PAD);
    const int boxH = BOX_H + noteH;
    const int boxX = (DESIGN_W - boxW) / 2;
    const int boxY = (DESIGN_H - boxH) / 2;
    draw_modal_box(c, boxX, boxY, boxW, boxH, t);

    c.text(title, boxX + (boxW - titleW) / 2, boxY + DLG_TITLE_Y, t.textTitle);
    int y = boxY + DLG_TITLE_Y + DLG_LINE_PITCH;
    for (const char* line : note) {
        const std::string text = c.cased(line);
        c.text(text, boxX + (boxW - c.text_width(text)) / 2, y, t.textParam);
        y += DLG_LINE_PITCH;
    }

    const int answersY = boxY + DLG_INSTR_Y + noteH;
    int x = boxX + (boxW - width) / 2;
    for (size_t i = 0; i < answers.size(); ++i) {
        const std::string label = c.cased(answers[i]);
        draw_cursor_cell(c, label, x, answersY, static_cast<int>(i) == chosen, t.textValue, t);
        x += c.text_width(label) + GAP;
    }
}

}  // namespace

void draw_confirm_dialog(Canvas& c, const ConfirmDialogState& s, const Theme& t) {
    if (!s.is_open()) return;
    draw_modal_backdrop(c);
    const std::vector<const char*> answers = confirm_choices(s.kind);
    if (answers.empty()) draw_confirm_box(c, confirm_dialog_title(s.kind), t);
    else draw_choice_box(c, confirm_dialog_title(s.kind), confirm_dialog_note(s.kind), answers, s.arg, t);
}

}  // namespace pt::ui
