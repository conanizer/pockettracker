// Where a gesture goes: to the top LAYER (a dialog or editor over the screen, ui/overlays.h) if one is
// up, then to the SCREEN's own handler if it has one, then to the gesture's generic path.

#include "ui/dispatch/dispatch_common.h"

namespace pt::ui {

// The handler of each overlay (ui/overlays.h), whose list is the stack.
GestureResult InputDispatcher::layer_handle(Overlay o, Gesture g) {
    switch (o) {
        case Overlay::LOADING:       return loading_layer(g);
        case Overlay::CONFIRM:       return confirm_layer(g);
        case Overlay::SAMPLE_CLOSE:  return sample_close_layer(g);
        case Overlay::RENDER_DIALOG: return render_dialog_layer(g);
        case Overlay::HELP_FULL:     return help_layer(g);
        case Overlay::QWERTY:        return qwerty_layer(g);
        case Overlay::THEME:         return theme_layer(g);
        case Overlay::EQ:            return eq_layer(g);
        case Overlay::SEND:          return send_editor_layer(g);
        case Overlay::STRETCH:       return stretch_editor_layer(g);
        case Overlay::FX_HELPER:     return fx_helper_layer(g);
        case Overlay::MAP_PICKER:    return map_picker_layer(g);
        case Overlay::INSTRUMENT_PICKER: return instrument_picker_layer(g);
        case Overlay::SF_PRESET_PICKER:  return sf_preset_picker_layer(g);
    }
    return GestureResult::PASS;
}

// A gesture a screen's handler passes runs the generic path, which asks the same row for the cell under
// the cursor and for the edit.
using D = InputDispatcher;
const D::ScreenHandler D::SCREENS[] = {
    {ScreenType::SAMPLE_EDITOR, &D::sample_editor_screen, &D::sample_editor_context, &D::sample_editor_edit, nullptr},
    {ScreenType::FILE_BROWSER,  &D::file_browser_screen,  nullptr,                   nullptr,                nullptr},
    {ScreenType::INSTRUMENT,    &D::instrument_screen,    &D::instrument_context,    &D::instrument_edit,    &D::instrument_knob},
    {ScreenType::INST_POOL,     &D::pool_screen,          &D::pool_context,          &D::pool_edit,          nullptr},
    {ScreenType::MODS,          &D::mods_screen,          &D::mods_context,          &D::mods_edit,          nullptr},
    {ScreenType::PROJECT,       &D::project_screen,       &D::project_context,       &D::project_edit,       nullptr},
    {ScreenType::SETTINGS,      &D::settings_screen,      &D::settings_context,      &D::settings_edit,      nullptr},
    {ScreenType::MIDI,          &D::midi_screen,          &D::midi_context,          &D::midi_edit,          nullptr},
    {ScreenType::MIDI_MAP,      &D::midi_map_screen,      &D::midi_map_context,      &D::midi_map_edit,      nullptr},
    {ScreenType::SONG,          &D::song_screen,          &D::song_context,          &D::song_edit,          nullptr},
    {ScreenType::CHAIN,         &D::chain_screen,         &D::chain_context,         &D::chain_edit,         nullptr},
    {ScreenType::PHRASE,        &D::phrase_screen,        &D::phrase_context,        &D::phrase_edit,        nullptr},
    {ScreenType::TABLE,         &D::table_screen,         &D::table_context,         &D::table_edit,         nullptr},
    {ScreenType::GROOVE,        &D::groove_screen,        &D::groove_context,        &D::groove_edit,        nullptr},
    {ScreenType::SCALE,         &D::scale_screen,         &D::scale_context,         &D::scale_edit,         nullptr},
    {ScreenType::MIXER,         &D::mixer_screen,         &D::mixer_context,         &D::mixer_edit,         &D::mixer_knob},
    {ScreenType::EFFECTS,       &D::effects_screen,       &D::effects_context,       &D::effects_edit,       &D::effects_knob},
};

const InputDispatcher::ScreenHandler* InputDispatcher::screen() const {
    for (const ScreenHandler& h : SCREENS)
        if (h.id == s_.currentScreen) return &h;
    return nullptr;
}

bool InputDispatcher::route(Gesture g) {
    if (const OverlayRow* top = top_overlay(s_))
        if (layer_handle(top->id, g) == GestureResult::TAKEN) return true;
    const ScreenHandler* h = screen();
    return h && (this->*h->handle)(g) == GestureResult::TAKEN;
}

}  // namespace pt::ui
