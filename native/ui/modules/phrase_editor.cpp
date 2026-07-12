#include "ui/modules/phrase_editor.h"

#include "ui/helpers.h"

namespace pt::ui {

using songcore::Note;
using songcore::PhraseStep;

void PhraseEditorModule::draw(Canvas& c, int x, int y, const PhraseEditorState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    // Column x positions. The `30 + 10` spellings are kept from the Kotlin verbatim — they read as
    // "a 2-char cell, then a 10px gutter", and collapsing them to 40 would lose that.
    int       colX      = x + 10;
    const int stepX     = colX; colX += 30 + 10;
    const int noteX     = colX; colX += 45 + 20;
    const int volX      = colX; colX += 30 + 15;
    const int instX     = colX; colX += 30 + 15;
    const int fx1NameX  = colX; colX += 45 + 10;
    const int fx1ValueX = colX; colX += 30 + 15;
    const int fx2NameX  = colX; colX += 45 + 10;
    const int fx2ValueX = colX; colX += 30 + 15;
    const int fx3NameX  = colX; colX += 45 + 10;
    const int fx3ValueX = colX;

    int rowY = y + TEXT_PADDING;
    c.draw_text("PHRASE " + hex2(s.phrase.id), x + 10, rowY, t.textTitle, CHAR_SPACING, FONT_SCALE);

    rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING;
    c.draw_text("N",   noteX,    rowY, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("V",   volX,     rowY, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("I",   instX,    rowY, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX1", fx1NameX, rowY, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX2", fx2NameX, rowY, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX3", fx3NameX, rowY, t.textParam, CHAR_SPACING, FONT_SCALE);

    for (int i = 0; i < static_cast<int>(s.phrase.steps.size()); ++i) {
        draw_row(c, x, y, i, s.phrase.steps[static_cast<size_t>(i)], s, stepX, noteX, volX, instX,
                 fx1NameX, fx1ValueX, fx2NameX, fx2ValueX, fx3NameX, fx3ValueX);
    }
}

void PhraseEditorModule::draw_row(Canvas& c, int x, int y, int index, const PhraseStep& step,
                                  const PhraseEditorState& s, int stepX, int noteX, int volX,
                                  int instX, int fx1NameX, int fx1ValueX, int fx2NameX,
                                  int fx2ValueX, int fx3NameX, int fx3ValueX) const {
    const Theme& t = s.theme;

    const int dataRowY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (index * ROW_HEIGHT);

    bool isRowSelected = false;
    if (s.selectionMode) {
        for (int col = 1; col <= 9 && !isRowSelected; ++col) isRowSelected = s.isCellSelected(index, col);
    }

    c.fill_rect(x, dataRowY, WIDTH, ROW_HEIGHT,
                row_bg_color(index, s.cursorRow, s.playbackRow, s.isPlaying, isRowSelected, t));

    const int textY = dataRowY + TEXT_PADDING;

    // Quarter-note rows (every 4th) are drawn brighter as a beat-accent cue.
    const Argb stepColor = (index == s.cursorRow && s.cursorColumn == 0) ? t.textCursor
                           : (index % 4 == 0)                            ? t.textParam
                                                                         : t.textEmpty;
    c.draw_text(hex1(index), stepX, textY, stepColor, CHAR_SPACING, FONT_SCALE);

    // Every value cell shares draw_cell's colour priority (cursor > selection > empty > per-column
    // colour); note-emptiness dims NOTE/VOL/INST, fx-emptiness dims its own name/value pair.
    const auto cur = [&](int col) { return index == s.cursorRow && s.cursorColumn == col; };
    const auto sel = [&](int col) { return s.selectionMode && s.isCellSelected(index, col); };

    const bool noteEmpty = (step.note == Note::EMPTY());
    draw_cell(c, note_name(step.note),  noteX, textY, cur(1), sel(1), noteEmpty, t.textValue, t);
    draw_cell(c, hex2(step.volume),     volX,  textY, cur(2), sel(2), noteEmpty, t.textParam, t);
    draw_cell(c, hex2(step.instrument), instX, textY, cur(3), sel(3), noteEmpty, t.textParam, t);

    const bool fx1Empty = (step.fx1Type == 0x00);
    draw_cell(c, effect_name(step.fx1Type), fx1NameX,  textY, cur(4), sel(4), fx1Empty, t.textTitle, t);
    draw_cell(c, hex2(step.fx1Value),       fx1ValueX, textY, cur(5), sel(5), fx1Empty, t.textParam, t);
    const bool fx2Empty = (step.fx2Type == 0x00);
    draw_cell(c, effect_name(step.fx2Type), fx2NameX,  textY, cur(6), sel(6), fx2Empty, t.textTitle, t);
    draw_cell(c, hex2(step.fx2Value),       fx2ValueX, textY, cur(7), sel(7), fx2Empty, t.textParam, t);
    const bool fx3Empty = (step.fx3Type == 0x00);
    draw_cell(c, effect_name(step.fx3Type), fx3NameX,  textY, cur(8), sel(8), fx3Empty, t.textTitle, t);
    draw_cell(c, hex2(step.fx3Value),       fx3ValueX, textY, cur(9), sel(9), fx3Empty, t.textParam, t);
}

CursorContext PhraseEditorModule::cursor_context(const PhraseEditorState& s) const {
    const PhraseStep& step = s.phrase.steps[static_cast<size_t>(s.cursorRow)];
    switch (s.cursorColumn) {
        case 0: return cc::read_only();
        case 1: {
            const bool isEmpty = (step.note == Note::EMPTY());
            return cc::note(isEmpty ? 0 : songcore::note_to_midi(step.note), isEmpty);
        }
        case 2: return cc::volume(step.volume);
        case 3: return cc::instrument(step.instrument);
        case 4: return cc::effect_type(step.fx1Type, 1);
        case 5: return cc::effect_value(step.fx1Value, 1, effect_value_max(step.fx1Type));
        case 6: return cc::effect_type(step.fx2Type, 2);
        case 7: return cc::effect_value(step.fx2Value, 2, effect_value_max(step.fx2Type));
        case 8: return cc::effect_type(step.fx3Type, 3);
        case 9: return cc::effect_value(step.fx3Value, 3, effect_value_max(step.fx3Type));
        default: return cc::none();
    }
}

PhraseInputResult PhraseEditorModule::handle_input(songcore::Phrase& phrase, int cursor_row,
                                                   int cursor_column,
                                                   const InputAction& action) const {
    PhraseStep& step = phrase.steps[static_cast<size_t>(cursor_row)];
    PhraseInputResult r;

    switch (action.type) {
        case ActionType::SET_VALUE:
            switch (cursor_column) {
                case 1:
                    step.note        = songcore::note_from_midi(action.value);
                    r.hasNote        = true;
                    r.lastEditedNote = step.note;
                    break;
                case 2:
                    step.volume        = action.value;
                    r.hasVolume        = true;
                    r.lastEditedVolume = action.value;
                    break;
                case 3:
                    step.instrument        = action.value;
                    r.hasInstrument        = true;
                    r.lastEditedInstrument = action.value;
                    break;
                // The FX type columns store an INDEX into EFFECT_TYPES; convert back to the code.
                case 4: step.fx1Type  = songcore::effect_type_at(action.value); break;
                case 5: step.fx1Value = action.value; break;
                case 6: step.fx2Type  = songcore::effect_type_at(action.value); break;
                case 7: step.fx2Value = action.value; break;
                case 8: step.fx3Type  = songcore::effect_type_at(action.value); break;
                case 9: step.fx3Value = action.value; break;
                default: break;
            }
            break;

        case ActionType::DELETE:
            switch (cursor_column) {
                case 1: step.note = Note::EMPTY(); break;
                case 4: clear_effect(step, 1); break;
                case 6: clear_effect(step, 2); break;
                case 8: clear_effect(step, 3); break;
                default: break;
            }
            break;

        case ActionType::INSERT_DEFAULT:
            if (cursor_column == 1) {
                step.note        = Note::C4();  // Note.fromString("C-4")
                r.hasNote        = true;
                r.lastEditedNote = step.note;
            }
            break;

        default:
            break;
    }

    r.modified = (action.type != ActionType::NONE);
    return r;
}

}  // namespace pt::ui
