#include "ui/modules/phrase_editor.h"

#include "songcore/scales.h"
#include "ui/fx_columns.h"
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

    // A pair dims when unset or switched off, an AUS/AUF when no ramp uses it.
    const int nameX[]  = {fx1NameX, fx2NameX, fx3NameX};
    const int valueX[] = {fx1ValueX, fx2ValueX, fx3ValueX};
    for (int slot = 1; slot <= songcore::FX_SLOTS; ++slot) {
        const songcore::FxSlot& f = songcore::fx_slot(step, slot);
        const int  col = fx_type_column(slot, PHRASE_FX_COLUMN);
        const bool dim = (cutOff && !(index == hopRow && f.type == songcore::FX_HOP)) || f.type == 0x00 ||
                         f.off || !rampCells.active(f.type, index, slot);
        cells.cell(effect_name(f.type), nameX[slot - 1],  cur(col),     sel(col),     dim, t.textValue);
        cells.cell(hex2(f.value),       valueX[slot - 1], cur(col + 1), sel(col + 1), dim, t.textParam);
    }
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
        default: break;
    }
    if (const int slot = fx_slot_of_column(s.cursorColumn, PHRASE_FX_COLUMN)) {
        const songcore::FxSlot& f = songcore::fx_slot(step, slot);
        return is_fx_type_column(s.cursorColumn, PHRASE_FX_COLUMN)
                   ? cc::effect_type(f.type, slot, s.effectTypeCount)
                   : cc::effect_value(f.value, slot, effect_value_max(f.type));
    }
    return cc::none();
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
                default:
                    // The type columns store an index into EFFECT_TYPES; the code is what the step keeps.
                    if (const int slot = fx_slot_of_column(cursor_column, PHRASE_FX_COLUMN)) {
                        songcore::FxSlot& f = songcore::fx_slot_ref(step, slot);
                        if (is_fx_type_column(cursor_column, PHRASE_FX_COLUMN))
                            f.type = songcore::effect_type_at(action.value);
                        else
                            f.value = action.value;
                    }
                    break;
            }
            break;

        case ActionType::DELETE:
            switch (cursor_column) {
                case 1: step.note = Note::EMPTY(); break;
                default:
                    if (is_fx_type_column(cursor_column, PHRASE_FX_COLUMN))
                        clear_effect(step, fx_slot_of_column(cursor_column, PHRASE_FX_COLUMN));
                    break;
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
