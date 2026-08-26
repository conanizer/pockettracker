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
    // The chain row and the song cell it hangs off — see PhraseEditorState.
    if (s.viaChain >= 0 && s.viaChainRow >= 0) {
        std::string via = "C" + hex2(s.viaChain) + "/" + hex1(s.viaChainRow);
        if (s.songRow >= 0 && s.songTrack >= 0)
            via += "  S" + hex2(s.songRow) + " T" + std::to_string(s.songTrack + 1);
        c.draw_text(via, x + 190, rowY, t.textParam, CHAR_SPACING, FONT_SCALE);
    }

    // The header lights for the column the cursor is in — it is half of what the row highlight used
    // to say, the row number being the other half. An FX header covers its name AND its value cell.
    rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING;
    const int cc = s.cursorColumn;
    c.draw_text("N",   noteX,    rowY, header_color(cc, 1, 1, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("V",   volX,     rowY, header_color(cc, 2, 2, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("I",   instX,    rowY, header_color(cc, 3, 3, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX1", fx1NameX, rowY, header_color(cc, 4, 5, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX2", fx2NameX, rowY, header_color(cc, 6, 7, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX3", fx3NameX, rowY, header_color(cc, 8, 9, t), CHAR_SPACING, FONT_SCALE);

    // Asked once and read three times a row: an AUS/AUF cell no ramp uses draws dimmed (draw_row).
    // It is the pairing the emitter itself runs, over every chain row that plays this phrase, so the
    // grid can neither show a fade the scheduler will not play nor deny one it will — an AUS whose
    // AUF is three phrases further down the chain is doing its job, and dimming it would be a lie in
    // the other direction.
    songcore::RampCells rampCells;
    if (s.project) rampCells = songcore::find_ramp_cells(*s.project, s.phrase.id);
    else           rampCells.mark(songcore::find_ramps(s.phrase));

    for (int i = 0; i < static_cast<int>(s.phrase.steps.size()); ++i) {
        draw_row(c, x, y, i, s.phrase.steps[static_cast<size_t>(i)], s, rampCells, stepX, noteX,
                 volX, instX, fx1NameX, fx1ValueX, fx2NameX, fx2ValueX, fx3NameX, fx3ValueX);
    }
}

void PhraseEditorModule::draw_row(Canvas& c, int x, int y, int index, const PhraseStep& step,
                                  const PhraseEditorState& s,
                                  const songcore::RampCells& rampCells, int stepX,
                                  int noteX, int volX, int instX, int fx1NameX, int fx1ValueX,
                                  int fx2NameX, int fx2ValueX, int fx3NameX, int fx3ValueX) const {
    const Theme& t = s.theme;

    const int dataRowY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (index * ROW_HEIGHT);

    c.fill_rect(x, dataRowY, WIDTH, ROW_HEIGHT, row_bg_color(index, t));

    const int textY = dataRowY + TEXT_PADDING;

    // Left to right, column by column — RowCells joins a selected run into one block across the
    // gutters, and it can only do that if the cells arrive in the order they are laid out.
    RowCells cells(c, textY, t);

    // Every value cell shares the painter.s colour priority (cursor > selection > empty > per-column
    // colour); note-emptiness dims NOTE/VOL/INST, fx-emptiness dims its own name/value pair.
    const auto cur = [&](int col) { return index == s.cursorRow && s.cursorColumn == col; };
    const auto sel = [&](int col) { return s.selectionMode && s.isCellSelected(index, col); };

    // Quarter-note rows (every 4th) are drawn brighter as a beat-accent cue — and the whole cursor
    // ROW's number lights, whatever column the cursor is in, because that is what now says which row
    // is being edited. Column 0 is a real cursor position, so the number goes through the painter and
    // gets the cell background when the cursor is actually on it.
    const Argb stepColor = (index == s.cursorRow) ? t.textCursor
                           : (index % 4 == 0)     ? t.textParam
                                                  : t.textEmpty;
    cells.cell(hex1(index), stepX, cur(0), /*is_selected=*/false, /*is_empty=*/false, stepColor);

    // Beside the one-character row number, in the gutter that was already there - 40px of which a
    // glyph uses 15. No column moves.
    for (const TrackPlayhead& ph : s.playheads)
        if (ph.phraseId == s.phrase.id && ph.step == index) draw_playhead(c, stepX + CHAR_W, textY, t);

    const bool noteEmpty = (step.note == Note::EMPTY());
    cells.cell(note_name(step.note),  noteX, cur(1), sel(1), noteEmpty, t.textValue);
    cells.cell(hex2(step.volume),     volX,  cur(2), sel(2), noteEmpty, t.textParam);
    cells.cell(hex2(step.instrument), instX, cur(3), sel(3), noteEmpty, t.textParam);

    // An FX pair dims when the slot is unset — and an AUS/AUF cell dims when no ramp uses it, which
    // is the only place the editor says that a fade the author thought they wrote is not one. Both
    // reach the painter through the one flag, because an inert cell does exactly what an unset cell
    // does: nothing.
    const auto fxDim = [&](int type, int slot) {
        return type == 0x00 || !rampCells.active(type, index, slot);
    };

    const bool fx1Dim = fxDim(step.fx1Type, 1);
    cells.cell(effect_name(step.fx1Type), fx1NameX,  cur(4), sel(4), fx1Dim, t.textTitle);
    cells.cell(hex2(step.fx1Value),       fx1ValueX, cur(5), sel(5), fx1Dim, t.textParam);
    const bool fx2Dim = fxDim(step.fx2Type, 2);
    cells.cell(effect_name(step.fx2Type), fx2NameX,  cur(6), sel(6), fx2Dim, t.textTitle);
    cells.cell(hex2(step.fx2Value),       fx2ValueX, cur(7), sel(7), fx2Dim, t.textParam);
    const bool fx3Dim = fxDim(step.fx3Type, 3);
    cells.cell(effect_name(step.fx3Type), fx3NameX,  cur(8), sel(8), fx3Dim, t.textTitle);
    cells.cell(hex2(step.fx3Value),       fx3ValueX, cur(9), sel(9), fx3Dim, t.textParam);
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
        case 4: return cc::effect_type(step.fx1Type, 1, s.effectTypeCount);
        case 5: return cc::effect_value(step.fx1Value, 1, effect_value_max(step.fx1Type));
        case 6: return cc::effect_type(step.fx2Type, 2, s.effectTypeCount);
        case 7: return cc::effect_value(step.fx2Value, 2, effect_value_max(step.fx2Type));
        case 8: return cc::effect_type(step.fx3Type, 3, s.effectTypeCount);
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
