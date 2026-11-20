// The instrument list over a phrase step's instrument cell (ui/instrument_picker.h), the preset list
// over a SoundFont's PATCH row (ui/sf_preset_picker.h) and the model list over MACRO's MODEL row.
// ⚠️ All open on A's release, which the mapper sends straight after: the layers must ignore that
// A_RELEASE.

#include "ui/dispatch/dispatch_common.h"

namespace pt::ui {

bool InputDispatcher::on_phrase_instrument_cell() const {
    return s_.currentScreen == ScreenType::PHRASE && s_.phraseCursor.column == 3 && !s_.selection.active;
}

void InputDispatcher::open_instrument_picker() {
    const Project& p = host_.project();
    const int      at = p.phrases[static_cast<size_t>(s_.currentPhrase)]
                            .steps[static_cast<size_t>(s_.phraseCursor.row)].instrument;
    InstrumentPickerState ps;
    ps.isOpen = true;
    ps.count  = static_cast<int>(p.instruments.size());
    // Opened with the cell's instrument a third of the way down, so the ones around it show.
    ps.top    = std::max(0, std::min(at - INSTRUMENT_PICKER_ROWS / 3, ps.count - INSTRUMENT_PICKER_ROWS));
    instrument_picker_move(ps, at, /*page=*/true);
    s_.instrumentPicker = ps;
}

GestureResult InputDispatcher::instrument_picker_layer(Gesture g) {
    InstrumentPickerState& ps = s_.instrumentPicker;
    switch (g) {
        case Gesture::DPAD_UP:    instrument_picker_move(ps, -1, /*page=*/false); break;
        case Gesture::DPAD_DOWN:  instrument_picker_move(ps, +1, /*page=*/false); break;
        case Gesture::DPAD_LEFT:  instrument_picker_move(ps, -INSTRUMENT_PICKER_ROWS, /*page=*/true); break;
        case Gesture::DPAD_RIGHT: instrument_picker_move(ps, +INSTRUMENT_PICKER_ROWS, /*page=*/true); break;
        case Gesture::A:
        case Gesture::A_A: {
            const int chosen = ps.cursor;
            ps.isOpen = false;
            if (on_phrase_instrument_cell() && phrase_edit(InputAction::set_value(chosen))) mark_modified();
            break;
        }
        case Gesture::B: ps.isOpen = false; break;
        // The screen behind may have an audition ringing.
        case Gesture::STOP_PREVIEW: return GestureResult::PASS;
        default: break;
    }
    return GestureResult::TAKEN;
}

void InputDispatcher::open_sf_preset_picker() {
    const int id = s_.currentInstrument;
    SfPresetPickerState ps;
    ps.names  = host_.sf_preset_names(id);
    ps.isOpen = !ps.names.empty();
    ps.count  = static_cast<int>(ps.names.size());
    const int at = host_.sf_preset_index(id);
    ps.top    = std::max(0, std::min(at - INSTRUMENT_PICKER_ROWS / 3, ps.count - INSTRUMENT_PICKER_ROWS));
    instrument_picker_move(ps, at, /*page=*/true);
    s_.sfPresetPicker = std::move(ps);
}

GestureResult InputDispatcher::sf_preset_picker_layer(Gesture g) {
    SfPresetPickerState& ps = s_.sfPresetPicker;
    switch (g) {
        case Gesture::DPAD_UP:    instrument_picker_move(ps, -1, /*page=*/false); break;
        case Gesture::DPAD_DOWN:  instrument_picker_move(ps, +1, /*page=*/false); break;
        case Gesture::DPAD_LEFT:  instrument_picker_move(ps, -INSTRUMENT_PICKER_ROWS, /*page=*/true); break;
        case Gesture::DPAD_RIGHT: instrument_picker_move(ps, +INSTRUMENT_PICKER_ROWS, /*page=*/true); break;
        case Gesture::A:
        case Gesture::A_A: {
            const int id     = s_.currentInstrument;
            const int chosen = ps.cursor;
            ps = SfPresetPickerState{};
            if (chosen == host_.sf_preset_index(id)) break;
            // The sound loads once the row is still, as after a step on the row itself.
            host_.set_sf_preset_by_index(id, chosen);
            mark_modified();
            break;
        }
        case Gesture::B: ps = SfPresetPickerState{}; break;
        case Gesture::STOP_PREVIEW: return GestureResult::PASS;
        default: break;
    }
    return GestureResult::TAKEN;
}

void InputDispatcher::open_macro_model_picker() {
    const int at = host_.project().instruments[static_cast<size_t>(s_.currentInstrument)].macroModel;
    InstrumentPickerState ps;
    ps.isOpen = true;
    ps.count  = songcore::MACRO_MODEL_COUNT;
    ps.top    = std::max(0, std::min(at - INSTRUMENT_PICKER_ROWS / 3, ps.count - INSTRUMENT_PICKER_ROWS));
    instrument_picker_move(ps, at, /*page=*/true);
    s_.macroModelPicker = ps;
}

GestureResult InputDispatcher::macro_model_picker_layer(Gesture g) {
    InstrumentPickerState& ps = s_.macroModelPicker;
    switch (g) {
        case Gesture::DPAD_UP:    instrument_picker_move(ps, -1, /*page=*/false); break;
        case Gesture::DPAD_DOWN:  instrument_picker_move(ps, +1, /*page=*/false); break;
        case Gesture::DPAD_LEFT:  instrument_picker_move(ps, -INSTRUMENT_PICKER_ROWS, /*page=*/true); break;
        case Gesture::DPAD_RIGHT: instrument_picker_move(ps, +INSTRUMENT_PICKER_ROWS, /*page=*/true); break;
        case Gesture::A:
        case Gesture::A_A: {
            const int chosen = ps.cursor;
            ps.isOpen = false;
            // Through the row's own edit, so the name follows the model as it does on a step.
            if (instrument_edit(InputAction::set_value(chosen))) mark_modified();
            break;
        }
        case Gesture::B: ps.isOpen = false; break;
        case Gesture::STOP_PREVIEW: return GestureResult::PASS;
        default: break;
    }
    return GestureResult::TAKEN;
}

}  // namespace pt::ui
