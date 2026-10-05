// What a cell opens: INSTRUMENT's buttons, the cells whose A or B waits for the release, and the EQ
// editor.

#include "ui/dispatch/dispatch_common.h"

#include "ui/instrument_row_layout.h"

#include <algorithm>

namespace pt::ui {

// The one place the FX list's length is decided — the picker and the FX column both read it.
//
// ⚠️ Two trims off one tail, so they nest: `LPO` sits directly below the MIDI six and can only be
// dropped once they are (songcore/effects.h). A build showing MIDI shows LPO.
int InputDispatcher::visible_effect_type_count() const {
    if (s_.caps.midi)       return songcore::EFFECT_TYPE_COUNT;
    if (s_.caps.loopWindow) return songcore::EFFECT_TYPE_COUNT_NO_MIDI;
    return songcore::EFFECT_TYPE_COUNT_STABLE;
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

}  // namespace pt::ui
