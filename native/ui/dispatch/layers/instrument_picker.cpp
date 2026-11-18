// The instrument list over a phrase step's instrument cell (ui/instrument_picker.h). ⚠️ It opens on A's
// release, which the mapper sends straight after: the layer must ignore that A_RELEASE.

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

}  // namespace pt::ui
