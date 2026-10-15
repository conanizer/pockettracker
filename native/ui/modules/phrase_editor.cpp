#include "ui/modules/phrase_editor.h"

#include "songcore/scales.h"
#include "ui/helpers.h"

namespace pt::ui {

using songcore::Note;
using songcore::PhraseStep;

void PhraseEditorModule::draw(Canvas& c, int x, int y, const PhraseEditorState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    // Column x: a 2-char cell, then a 10px gutter.
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
    // The chain row and its song cell (PhraseEditorState).
    if (s.viaChain >= 0 && s.viaChainRow >= 0) {
        std::string via = "C" + hex2(s.viaChain) + "/" + hex1(s.viaChainRow);
        if (s.songRow >= 0 && s.songTrack >= 0)
            via += "  S" + hex2(s.songRow) + " T" + std::to_string(s.songTrack + 1);
        c.draw_text(via, x + 190, rowY, t.textParam, CHAR_SPACING, FONT_SCALE);
    }

    // The header lights for the cursor's column, the row number for its row.
    rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING;
    const int cc = s.cursorColumn;
    c.draw_text("N",   noteX,    rowY, header_color(cc, 1, 1, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("V",   volX,     rowY, header_color(cc, 2, 2, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("I",   instX,    rowY, header_color(cc, 3, 3, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX1", fx1NameX, rowY, header_color(cc, 4, 5, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX2", fx2NameX, rowY, header_color(cc, 6, 7, t), CHAR_SPACING, FONT_SCALE);
    c.draw_text("FX3", fx3NameX, rowY, header_color(cc, 8, 9, t), CHAR_SPACING, FONT_SCALE);

    // An AUS/AUF cell no ramp uses draws dimmed — the emitter's own pairing over every chain row that
    // plays this phrase, asked once per draw.
    songcore::RampCells rampCells;
    if (s.project) rampCells = songcore::find_ramp_cells(*s.project, s.phrase.id);
    else           rampCells.mark(songcore::find_ramps(s.phrase));

    // Rows a certain HOP cuts off draw dimmed.
    const int hopRow = songcore::phrase_certain_hop_row(s.phrase);

    for (int i = 0; i < static_cast<int>(s.phrase.steps.size()); ++i) {
        draw_row(c, x, y, i, s.phrase.steps[static_cast<size_t>(i)], s, rampCells, hopRow, stepX, noteX,
                 volX, instX, fx1NameX, fx1ValueX, fx2NameX, fx2ValueX, fx3NameX, fx3ValueX);
    }
}

void PhraseEditorModule::draw_row(Canvas& c, int x, int y, int index, const PhraseStep& step,
                                  const PhraseEditorState& s,
                                  const songcore::RampCells& rampCells, int hopRow, int stepX,
                                  int noteX, int volX, int instX, int fx1NameX, int fx1ValueX,
                                  int fx2NameX, int fx2ValueX, int fx3NameX, int fx3ValueX) const {
    const Theme& t = s.theme;

    // The HOP's own row plays nothing, but its HOP cell acts, so it stays lit.
    const bool cutOff = hopRow >= 0 && index >= hopRow;

    const int dataRowY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (index * ROW_HEIGHT);

    c.fill_rect(x, dataRowY, WIDTH, ROW_HEIGHT, row_bg_color(index, t));

    const int textY = dataRowY + TEXT_PADDING;

    // Left to right: RowCells joins a selected run across the gutters only in layout order.
    RowCells cells(c, textY, t);

    // Note-emptiness dims NOTE/VOL/INST; fx-emptiness its own pair.
    const auto cur = [&](int col) { return index == s.cursorRow && s.cursorColumn == col; };
    const auto sel = [&](int col) { return s.selectionMode && s.isCellSelected(index, col); };

    // Every 4th row number is brighter (a beat). Column 0 is a real cursor position.
    const Argb stepColor = (index == s.cursorRow) ? cursor_mark_ink(t)
                           : (index % 4 == 0 && !cutOff) ? t.textParam
                                                         : t.textEmpty;
    cells.cell(hex1(index), stepX, cur(0), /*is_selected=*/false, /*is_empty=*/false, stepColor);

    // The playhead marker, in the gutter beside the row number.
    for (const TrackPlayhead& ph : s.playheads)
        if (ph.phraseId == s.phrase.id && ph.step == index) draw_playhead(c, stepX + CHAR_W, textY, t);

    // An OFF plays nothing: its V and I dim.
    const bool noteEmpty = cutOff || (step.note == Note::EMPTY());
    const bool noNote    = cutOff || !songcore::step_has_note(step);
    cells.cell(note_name(step.note),  noteX, cur(1), sel(1), noteEmpty, t.textValue);
    cells.cell(hex2(step.volume),     volX,  cur(2), sel(2), noNote, t.textParam);
    cells.cell(hex2(step.instrument), instX, cur(3), sel(3), noNote, t.textParam);

    // A pair dims when unset, an AUS/AUF when no ramp uses it.
    const auto fxDim = [&](int type, int slot) {
        if (cutOff && !(index == hopRow && type == songcore::FX_HOP)) return true;
        return type == 0x00 || !rampCells.active(type, index, slot);
    };

    const bool fx1Dim = fxDim(step.fx1Type, 1);
    cells.cell(effect_name(step.fx1Type), fx1NameX,  cur(4), sel(4), fx1Dim, t.textValue);
    cells.cell(hex2(step.fx1Value),       fx1ValueX, cur(5), sel(5), fx1Dim, t.textParam);
    const bool fx2Dim = fxDim(step.fx2Type, 2);
    cells.cell(effect_name(step.fx2Type), fx2NameX,  cur(6), sel(6), fx2Dim, t.textValue);
    cells.cell(hex2(step.fx2Value),       fx2ValueX, cur(7), sel(7), fx2Dim, t.textParam);
    const bool fx3Dim = fxDim(step.fx3Type, 3);
    cells.cell(effect_name(step.fx3Type), fx3NameX,  cur(8), sel(8), fx3Dim, t.textValue);
    cells.cell(hex2(step.fx3Value),       fx3ValueX, cur(9), sel(9), fx3Dim, t.textParam);
}

CursorContext PhraseEditorModule::cursor_context(const PhraseEditorState& s) const {
    const PhraseStep& step = s.phrase.steps[static_cast<size_t>(s.cursorRow)];
    switch (s.cursorColumn) {
        case 0: return cc::read_only();
        case 1: {
            // An OFF has no pitch: A+B clears it, nothing else.
            if (songcore::note_is_off(step.note)) {
                CursorContext off = cc::note(0);
                off.capabilities           = {};
                off.capabilities.canDelete = true;
                return off;
            }
            const bool isEmpty = (step.note == Note::EMPTY());
            // ⚠️ This quantizes typing, not the song: `step.note` is never rewritten. The scale is
            // the one the phrase's own SCA/SCG cells above put this row in. An exempt instrument
            // (sliced, or TSP off) is offered every note.
            unsigned mask = 0x0FFFu;
            int      key  = 0;
            if (s.project != nullptr) {
                const songcore::NoteScale ns = songcore::phrase_note_scale(
                    *s.project, s.phrase, s.cursorRow, isEmpty ? s.insertInstrument : step.instrument);
                if (ns.scale != nullptr) {
                    mask = songcore::scale_mask(*ns.scale);
                    key  = ns.key;
                }
            }
            return cc::note(isEmpty ? 0 : songcore::note_to_midi(step.note), isEmpty, mask, key);
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
                // The type columns store an index into EFFECT_TYPES; the code is what the step keeps.
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
                step.note        = Note::C4();
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
