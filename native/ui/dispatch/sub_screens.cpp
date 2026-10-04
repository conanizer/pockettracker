// What a cell opens: the FX helper, the mapping destination picker, INSTRUMENT's buttons, the
// cells whose A or B waits for the release, and the EQ editor.

#include "ui/dispatch/dispatch_common.h"

#include "ui/instrument_row_layout.h"

#include <algorithm>

namespace pt::ui {

// ─── The FX-type column, and the helper it opens ──────────────────────────────────────────────────

bool InputDispatcher::on_fx_type_column() const {
    switch (s_.currentScreen) {
        case ScreenType::PHRASE:
            return s_.cursorColumn == 4 || s_.cursorColumn == 6 || s_.cursorColumn == 8;
        case ScreenType::TABLE:
            return s_.tableCursorColumn == 3 || s_.tableCursorColumn == 5 ||
                   s_.tableCursorColumn == 7;
        default:
            return false;
    }
}

// ─── The mapping destination picker ──────────────────────────────────────────────────────────────

bool InputDispatcher::on_map_dest_cell() const {
    if (s_.currentScreen != ScreenType::MIDI_MAP) return false;
    const Project& p = *s_.project;
    if (s_.midiMapCursorRow < 0 ||
        s_.midiMapCursorRow >= static_cast<int>(p.midiMappings.size()))
        return false;   // the ADD row — a plain A is its whole behaviour
    return s_.midiMapCursorColumn == static_cast<int>(MapCol::GROUP) ||
           s_.midiMapCursorColumn == static_cast<int>(MapCol::PARAM);
}

void InputDispatcher::apply_map_picker_choice() {
    const songcore::MapDest* d = s_.mapPicker.selected();
    s_.mapPicker = MapPickerState{};
    if (d == nullptr || !on_map_dest_cell()) return;

    Project& p = host_.edit_project();
    // ⚠️ `take_dest`, not a field write: a new destination brings its own RANGE and clears its SCOPE.
    if (songcore::take_dest(p.midiMappings[static_cast<size_t>(s_.midiMapCursorRow)], *d))
        mark_dirty_and_arm_autosave();

    // No cursor clamp needed: the picker opens only on GROUP and PARAM, which every destination has.
}

int InputDispatcher::current_fx_type_code() const {
    const Project& p = *s_.project;
    int            code = 0;

    if (s_.currentScreen == ScreenType::PHRASE) {
        const songcore::PhraseStep& step =
            p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
        switch (s_.cursorColumn) {
            case 4: code = step.fx1Type; break;
            case 6: code = step.fx2Type; break;
            case 8: code = step.fx3Type; break;
            default: break;
        }
    } else if (s_.currentScreen == ScreenType::TABLE) {
        const songcore::TableRow& row =
            p.tables[static_cast<size_t>(s_.currentTable)].rows[static_cast<size_t>(s_.tableCursorRow)];
        switch (s_.tableCursorColumn) {
            case 3: code = row.fx1Type; break;
            case 5: code = row.fx2Type; break;
            case 7: code = row.fx3Type; break;
            default: break;
        }
    }
    return code;
}

// The one place the FX list's length is decided — the picker and the FX column both read it.
//
// ⚠️ Two trims off one tail, so they nest: `LPO` sits directly below the MIDI six and can only be
// dropped once they are (songcore/effects.h). A build showing MIDI shows LPO.
int InputDispatcher::visible_effect_type_count() const {
    if (s_.caps.midi)       return songcore::EFFECT_TYPE_COUNT;
    if (s_.caps.loopWindow) return songcore::EFFECT_TYPE_COUNT_NO_MIDI;
    return songcore::EFFECT_TYPE_COUNT_STABLE;
}

void InputDispatcher::apply_fx_type_change(int effect_code) {
    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::PHRASE) {
        songcore::PhraseStep& step =
            p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
        switch (s_.cursorColumn) {
            case 4: step.fx1Type = effect_code; break;
            case 6: step.fx2Type = effect_code; break;
            case 8: step.fx3Type = effect_code; break;
            default: return;
        }
        mark_modified();
    } else if (s_.currentScreen == ScreenType::TABLE) {
        songcore::TableRow& row =
            p.tables[static_cast<size_t>(s_.currentTable)].rows[static_cast<size_t>(s_.tableCursorRow)];
        switch (s_.tableCursorColumn) {
            case 3: row.fx1Type = effect_code; break;
            case 5: row.fx2Type = effect_code; break;
            case 7: row.fx3Type = effect_code; break;
            default: return;
        }
        mark_modified(/*table_touched=*/true);
    }
}

// ─── INSTRUMENT's buttons ────────────────────────────────────────────────────────────────────────

bool InputDispatcher::instrument_open_at_cursor() {
    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::INST_POOL) {
        // A on the pool's NAME column of an EMPTY slot loads a source into it. A loaded slot is
        // managed from INSTRUMENT, where you can see what you would be replacing.
        if (s_.poolCursorColumn != 0) return false;
        const Instrument& ins  = p.instruments[static_cast<size_t>(s_.currentInstrument)];
        const bool        isSF = ins.instrumentType == songcore::InstrumentType::SOUNDFONT;
        // ⚠️ An EXTERNAL slot has no source: without this it would open the SAMPLES browser, and a load
        // would flip the slot's type back.
        if (!instrument_has_source_row(ins.instrumentType)) return false;
        if (isSF ? ins.soundfontPath.has_value() : !songcore::instrument_is_free(ins)) return false;

        open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE,
                          isSF ? browser_dir(BrowserDir::SOUNDFONTS) : browser_dir(BrowserDir::SAMPLES),
                          isSF ? soundfont_extensions() : sample_extensions());
        return true;
    }

    if (s_.currentScreen != ScreenType::INSTRUMENT) return false;

    const Instrument& ins  = p.instruments[static_cast<size_t>(s_.currentInstrument)];
    const bool        isSF = ins.instrumentType == songcore::InstrumentType::SOUNDFONT;
    const int         row  = s_.instrumentCursorRow;
    const int         col  = s_.instrumentCursorColumn;

    // Row 0 — TYPE (col 1), LOAD (col 2) browses for a source, EDIT (col 3) opens the sample editor.
    // EXTERNAL draws neither button; refuse rather than browse for a source it cannot have.
    if (row == 0 && col >= 2 && !instrument_has_source_row(ins.instrumentType)) return true;

    if (row == 0 && col == 2) {
        open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE,
                          isSF ? browser_dir(BrowserDir::SOUNDFONTS) : browser_dir(BrowserDir::SAMPLES),
                          isSF ? soundfont_extensions() : sample_extensions());
        return true;
    }
    // Samplers only: a SoundFont has no single waveform to cut. EDIT is not drawn on SF, so the isSF
    // guard only consumes the press.
    if (row == 0 && col == 3) {
        if (isSF) return true;   // handled: the press is CONSUMED, it just opens nothing
        open_sample_editor();
        return true;
    }

    // Row 5 — the INSTRUMENT PRESET (.pti: params, mods, table, source path). SAVE (col 2), LOAD (col 3).
    if (row == 5 && col == 2) {
        const std::string dir  = fs_.instruments_directory();
        const std::string name = ins.name.empty() ? songcore::default_instrument_name(ins.id) : ins.name;
        open_qwerty(QwertyContext::INSTRUMENT_SAVE, name, "SAVE PRESET:", dir, /*max_length=*/20,
                    /*clear_on_first_b=*/true);
        return true;
    }
    if (row == 5 && col == 3) {
        open_file_browser(AppState::BrowserPurpose::LOAD_PRESET, browser_dir(BrowserDir::INSTRUMENTS),
                          {"pti"});
        return true;
    }

    // Row 1 (NAME) and the EQ cell are not here: their A must wait for the RELEASE, so they live in
    // `open_sub_screen_at_cursor`. These are read-only buttons that fire on the press.
    return false;
}

bool InputDispatcher::defer_a_to_release() const {
    // ⚠️ No cell opens a sub-screen while a modal is up. The cursor under the EQ editor sits on the EQ
    // cell that raised it, so without this every A inside the editor would be deferred for nothing.
    if (any_modal_open()) return false;

    // ⚠️ Opens nothing, but deferred too: on row 11 under MANUAL a plain A cuts a boundary at the
    // playhead, and the same A is held for the slice step, the drag and the delete.
    if (on_slice_tap_cell()) return true;

    return const_cast<InputDispatcher*>(this)->open_sub_screen_at_cursor(/*peek=*/true);
}

bool InputDispatcher::defer_b_to_release() const {
    // The EQ editor: B is both CLOSE and the slot-cycle modifier, and only the release says which.
    if (eq_open()) return true;

    // ⚠️ And wherever R+B is a MUTE: two buttons never arrive on the same frame, so a B landing a frame
    // ahead of its R would COPY and close the selection, and the R+B would then mute one channel
    // instead of the eight highlighted. Holding B until release lets R claim it either way round, and
    // stops B+UP/DOWN paging a selection away.
    return mute_solo_chord_live();
}

// ─── Cells that open on release, and the EQ editor ───────────────────────────────────────────────

bool InputDispatcher::open_sub_screen_at_cursor(bool peek) {
    const Project& p = *s_.project;

    switch (s_.currentScreen) {
        case ScreenType::PROJECT:
            // The NAME row: each character is an in-place cell, so A opens the keyboard, A+RIGHT steps
            // the character and A+B blanks it — the sharpest case for deferring A.
            if (s_.projectCursorRow == static_cast<int>(ProjectRow::NAME) &&
                s_.projectCursorColumn >= 1) {
                if (!peek) open_qwerty(QwertyContext::PROJECT_NAME, host_.project().name,
                                       "PROJECT NAME:", "", PROJECT_NAME_MAX_CHARS);
                return true;
            }
            break;

        case ScreenType::INSTRUMENT: {
            const Instrument& ins = p.instruments[static_cast<size_t>(s_.currentInstrument)];

            if (s_.instrumentCursorRow == 1) {
                if (!peek) {
                    // A default-named slot opens the box EMPTY, not with "INST07" to delete first.
                    const std::string cur =
                        songcore::instrument_has_default_name(ins) ? "" : ins.name;
                    open_qwerty(QwertyContext::INSTRUMENT_NAME, cur, "INSTRUMENT NAME:", "");
                }
                return true;
            }
            // EXTERNAL has no EQ row: `instrument_eq_row` answers −1, which no cursor row matches.
            if (s_.instrumentCursorRow == instrument_eq_row(ins.instrumentType) &&
                s_.instrumentCursorColumn == 1) {
                // ⚠️ An unassigned EQ is −1 (bypass), which is not a slot; the editor opens on slot 0.
                if (!peek) open_eq_editor(std::max(0, ins.eqSlot),
                                          EqCallerContext::instrument(s_.currentInstrument));
                return true;
            }
            break;
        }

        case ScreenType::INST_POOL:
            if (s_.poolCursorColumn == 4) {
                const Instrument& ins = p.instruments[static_cast<size_t>(s_.currentInstrument)];
                if (!peek) open_eq_editor(std::max(0, ins.eqSlot),
                                          EqCallerContext::instrument(s_.currentInstrument));
                return true;
            }
            break;

        case ScreenType::MIXER:
            if (s_.mixerMasterRow == 1 && s_.mixerCursorColumn == 8) {
                if (!peek) open_eq_editor(std::max(0, p.masterEqSlot), EqCallerContext::master());
                return true;
            }
            break;

        case ScreenType::EFFECTS:
            if (s_.effectsCursorRow == EffectModule::ROW_REV_EQ) {
                if (!peek) open_eq_editor(std::max(0, p.reverbInputEq), EqCallerContext::reverb_in());
                return true;
            }
            if (s_.effectsCursorRow == EffectModule::ROW_DLY_EQ) {
                if (!peek) open_eq_editor(std::max(0, p.delayInputEq), EqCallerContext::delay_in());
                return true;
            }
            break;

        case ScreenType::SAMPLE_EDITOR:
            // Only the EQ SLOT cell (row 16, col 1, with the EQ effect selected). Column 2 is APPLY and
            // keeps its own A; A+DPAD on column 1 still dials the slot.
            if (s_.sampleEditor.cursorRow == 16 && s_.sampleEditor.cursorCol == 1 &&
                s_.sampleEditor.fxType == 3) {
                if (!peek) open_eq_editor(std::min(127, std::max(0, s_.sampleEditor.fxValue)),
                                          EqCallerContext::sample_editor_fx());
                return true;
            }
            break;

        default:
            break;
    }
    return false;
}

void InputDispatcher::open_eq_editor(int slot, EqCallerContext caller) {
    s_.eq           = EqEditorState{};
    s_.eq.isOpen    = true;
    s_.eq.slotIndex = std::min(127, std::max(0, slot));
    s_.eq.cursorRow = 0;   // BAND 1, TYPE — the top-left cell, every time
    s_.eq.caller    = caller;
}

void InputDispatcher::eq_move_cursor(int d_band, int d_param) {
    const int band  = std::min(2, std::max(0, s_.eq.cursor_band() + d_band));
    const int param = std::min(3, std::max(0, s_.eq.cursor_param() + d_param));
    s_.eq.cursorRow = band * 4 + param;
}

void InputDispatcher::apply_caller_eq_slot_change(int new_slot) {
    Project& p = host_.edit_project();

    // Five different project fields, one gesture: cycling the slot writes back to the cell that
    // RAISED the editor, which is what `EqCallerContext` records.
    switch (s_.eq.caller.kind) {
        case EqCallerContext::Kind::MASTER:
            p.masterEqSlot = new_slot;
            host_.set_master_eq_slot(new_slot);
            break;
        case EqCallerContext::Kind::REVERB_IN:
            p.reverbInputEq = new_slot;
            host_.set_reverb_input_eq(new_slot);
            break;
        case EqCallerContext::Kind::DELAY_IN:
            p.delayInputEq = new_slot;
            host_.set_delay_input_eq(new_slot);
            break;
        case EqCallerContext::Kind::INSTRUMENT: {
            const int id = s_.eq.caller.instrId;
            if (id >= 0 && id < static_cast<int>(p.instruments.size())) {
                p.instruments[static_cast<size_t>(id)].eqSlot = new_slot;
                host_.set_instrument_eq_slot(id, new_slot);
            }
            break;
        }
        case EqCallerContext::Kind::SAMPLE_EDITOR_FX:
            // No engine call: the sample editor's EQ is applied destructively on APPLY, reading the
            // bank then.
            s_.sampleEditor.fxValue = new_slot;
            break;
    }

    // Dirty AND armed: this path skips mark_modified (its push_globals is too heavy for a fast band
    // dial; the two calls below are the right-sized push), but must not skip the crash autosave.
    mark_dirty_and_arm_autosave();
}

void InputDispatcher::push_eq_band_to_engine() {
    const Project& p       = *s_.project;
    const int      slot    = s_.eq.slotIndex;
    const int      bandIdx = s_.eq.cursor_band();

    if (slot < 0 || slot >= static_cast<int>(p.eqPresets.size())) return;
    const songcore::EqPreset& preset = p.eqPresets[static_cast<size_t>(slot)];
    if (bandIdx < 0 || bandIdx >= static_cast<int>(preset.bands.size())) return;
    const songcore::EqBand& band = preset.bands[static_cast<size_t>(bandIdx)];

    // The BAND, into the engine's 128-slot bank.
    host_.set_eq_band(slot, bandIdx, band.type, band.freq, band.gain, band.q);

    // ⚠️ Then re-hand the caller the slot: `set_eq_band` writes the BANK, and only re-assigning the
    // slot makes the consumer recompile its coefficients. It goes through apply_caller_eq_slot_change
    // so an UNASSIGNED EQ (shown as slot 0) ADOPTS the slot in the project too — otherwise you would
    // hear the EQ while the cell read "--" and a reload discarded it.
    apply_caller_eq_slot_change(slot);
}


}  // namespace pt::ui
