// The MAP PICKER: the destination list raised by A+UP/DOWN on a mapping's GROUP or PARAMETER cell. It
// lives only while A is held — A+DPAD walks the list and releasing A points the mapping at the
// highlighted destination and closes.
// ⚠️ `currentScreen` and the cursor still name the mapping row underneath; the commit writes through them.

#include "ui/dispatch/dispatch_common.h"

namespace pt::ui {

LayerResult InputDispatcher::map_picker_layer(Gesture g) {
    switch (g) {
        case Gesture::A_UP:    map_picker_move_up(s_.mapPicker); break;
        case Gesture::A_DOWN:  map_picker_move_down(s_.mapPicker); break;
        case Gesture::A_LEFT:  map_picker_move_left(s_.mapPicker); break;
        case Gesture::A_RIGHT: map_picker_move_right(s_.mapPicker); break;

        // Commits on RELEASE, so you can hold A, read the list, and let go on your choice.
        case Gesture::A_RELEASE: apply_map_picker_choice(); break;

        default: break;
    }
    return LayerResult::TAKEN;
}

void InputDispatcher::open_map_picker() {
    s_.mapPicker = map_picker_opened_at(static_cast<songcore::MapDestId>(
        s_.project->midiMappings[static_cast<size_t>(s_.midiMapCursorRow)].dest));
}

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

}  // namespace pt::ui
