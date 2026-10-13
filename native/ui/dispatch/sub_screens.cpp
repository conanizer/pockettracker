// What a cell opens: the cells whose A or B waits for the release, and the EQ and send editors.

#include "ui/dispatch/dispatch_common.h"

#include "ui/instrument_row_layout.h"
#include "ui/mixer_cell_layout.h"

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

bool InputDispatcher::defer_a_to_release() const {
    InputDispatcher& self = const_cast<InputDispatcher&>(*this);

    // ⚠️ No cell opens a sub-screen while a layer is up. The cursor under the EQ editor sits on the EQ
    // cell that raised it, so without this every A inside the editor would be deferred for nothing.
    // The send editor's INP EQ and TIME cells are the exception: a layer's own cells with a plain A.
    if (top_layer()) {
        return send_editor_on_top() &&
               self.effects_release_action(s_.sendEditor.cursorRow, /*peek=*/true);
    }

    // ⚠️ Opens nothing, but deferred too: on row 11 under MANUAL a plain A cuts a boundary at the
    // playhead, and the same A is held for the slice step, the drag and the delete.
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
