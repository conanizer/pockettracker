// What a cell opens: the cells whose A or B waits for the release, and the editors over a screen.

#include "ui/dispatch/dispatch_common.h"

#include "ui/instrument_row_layout.h"
#include "ui/mixer_cell_layout.h"

#include <algorithm>

namespace pt::ui {

// The FX list's length, read by the picker and the FX column. ⚠️ LPO sits just below the MIDI six and
// drops only with them: a build showing MIDI shows LPO.
int InputDispatcher::visible_effect_type_count() const {
    if (s_.caps.midi)       return songcore::EFFECT_TYPE_COUNT;
    if (s_.caps.loopWindow) return songcore::EFFECT_TYPE_COUNT_NO_MIDI;
    return songcore::EFFECT_TYPE_COUNT_STABLE;
}

bool InputDispatcher::defer_a_to_release() const {
    InputDispatcher& self = const_cast<InputDispatcher&>(*this);

    // ⚠️ No cell opens a sub-screen under a layer (else every A in the EQ editor is deferred), except the
    // send editor's own INP EQ and TIME.
    if (top_layer()) {
        return send_editor_on_top() &&
               self.effects_release_action(s_.sendEditor.cursorRow, /*peek=*/true);
    }

    // ⚠️ Opens nothing, but deferred too: row 11's A under MANUAL is held for the tap, the drag and the
    // delete.
    if (on_slice_tap_cell()) return true;

    switch (s_.currentScreen) {
        case ScreenType::INSTRUMENT:
            if (self.instrument_open_at_cursor(/*peek=*/true)) return true;
            break;
        case ScreenType::INST_POOL:
            if (self.pool_load_into_empty_slot(/*peek=*/true)) return true;
            break;
        case ScreenType::PHRASE:
        case ScreenType::TABLE:
            if (self.fx_insert_last(/*peek=*/true)) return true;
            break;
        default:
            break;
    }
    return self.open_sub_screen_at_cursor(/*peek=*/true);
}

bool InputDispatcher::defer_b_to_release() const {
    // The EQ editor: B is both CLOSE and the slot-cycle modifier, and only the release says which.
    if (eq_open()) return true;

    // ⚠️ And wherever R+B is a chord: a B landing a frame before its R would copy and close the
    // selection, and the chord would act on one cell or channel instead of all of them.
    return mute_solo_chord_live() || fx_off_chord_live();
}

// ─── Cells that open on release, and the EQ editor ───────────────────────────────────────────────

bool InputDispatcher::open_sub_screen_at_cursor(bool peek) {
    const Project& p = *s_.project;

    switch (s_.currentScreen) {
        case ScreenType::PROJECT:
            // A opens the keyboard on the release, like every cell that opens something.
            if (s_.projectCursorRow == static_cast<int>(ProjectRow::NAME)) {
                if (!peek) open_qwerty(QwertyContext::PROJECT_NAME, host_.project().name,
                                       "PROJECT NAME:", "", PROJECT_NAME_MAX_CHARS);
                return true;
            }
            break;

        case ScreenType::INSTRUMENT: {
            const Instrument&    ins  = p.instruments[static_cast<size_t>(s_.currentInstrument)];
            const InstrumentCell cell = instrument_cell_at(ins.instrumentType, s_.instrumentCursorRow,
                                                           s_.instrumentCursorColumn);

            if (cell == InstrumentCell::NAME) {
                if (!peek) {
                    // A default-named slot opens the box EMPTY, not with "INST07" to delete first.
                    const std::string cur =
                        songcore::instrument_has_default_name(ins) ? "" : ins.name;
                    open_qwerty(QwertyContext::INSTRUMENT_NAME, cur, "INSTRUMENT NAME:", "");
                }
                return true;
            }
            if (cell == InstrumentCell::EQ) {
                // ⚠️ An unassigned EQ is −1 (bypass), which is not a slot; the editor opens on slot 0.
                if (!peek) open_eq_editor(std::max(0, ins.eqSlot),
                                          EqCallerContext::instrument(s_.currentInstrument));
                return true;
            }
            // The two sends open the unit they feed.
            if (cell == InstrumentCell::REV || cell == InstrumentCell::DEL) {
                if (!peek) open_send_editor(cell == InstrumentCell::REV ? EffectsSection::REVERB
                                                                        : EffectsSection::DELAY);
                return true;
            }
            break;
        }

        case ScreenType::INST_POOL:
            // Columns 2 and 3 are the same two sends.
            if (s_.poolCursorColumn == 2 || s_.poolCursorColumn == 3) {
                if (!peek) open_send_editor(s_.poolCursorColumn == 2 ? EffectsSection::REVERB
                                                                     : EffectsSection::DELAY);
                return true;
            }
            if (s_.poolCursorColumn == 4) {
                const Instrument& ins = p.instruments[static_cast<size_t>(s_.currentInstrument)];
                if (!peek) open_eq_editor(std::max(0, ins.eqSlot),
                                          EqCallerContext::instrument(s_.currentInstrument));
                return true;
            }
            break;

        case ScreenType::MIXER:
            switch (mixer_cell_at(s_.mixerMasterRow, s_.mixerCursorColumn)) {
                case MixerCell::MASTER_EQ:
                    if (!peek) open_eq_editor(std::max(0, p.masterEqSlot), EqCallerContext::master());
                    return true;
                // The two returns open the unit they bring back.
                case MixerCell::REV_WET:
                    if (!peek) open_send_editor(EffectsSection::REVERB);
                    return true;
                case MixerCell::DLY_WET:
                    if (!peek) open_send_editor(EffectsSection::DELAY);
                    return true;
                case MixerCell::TRACK_VOL:
                case MixerCell::MASTER_VOL:
                case MixerCell::MASTER_FX:
                case MixerCell::LIMITER:
                case MixerCell::NONE:
                    break;
            }
            break;

        case ScreenType::EFFECTS:
            if (effects_release_action(s_.effectsCursorRow, peek)) return true;
            break;

        case ScreenType::SAMPLE_EDITOR: {
            // The FX row's VALUE cell: an EQ slot opens the EQ editor, TSTRETCH its box.
            const SampleEditorState& se = s_.sampleEditor;
            if (se.cursorRow != 16 || se.cursorCol != 1) break;
            if (se.fxType == SampleEditorModule::FX_EQ) {
                if (!peek) {
                    // ⚠️ "--" takes slot 00 on open: START and APPLY read the cell.
                    s_.sampleEditor.fxEqSlot = std::max(0, se.fxEqSlot);
                    open_eq_editor(s_.sampleEditor.fxEqSlot, EqCallerContext::sample_editor_fx());
                }
                return true;
            }
            if (se.fxType == SampleEditorModule::FX_SYNC && se.syncType == SampleEditorModule::SYNC_TSTRETCH) {
                if (!peek) open_stretch_editor();
                return true;
            }
            break;
        }

        default:
            break;
    }
    return false;
}

}  // namespace pt::ui
