// Where a gesture goes: to the top LAYER (a dialog or editor over the screen) if one is up, then to the
// SCREEN's own handler if it has one, then to the gesture's generic path.

#include "ui/dispatch/dispatch_common.h"

namespace pt::ui {

// ⚠️ The ORDER is the stack — the first open entry is the top. The pairs that can be open together:
//   • LOADING above everything, the confirm included: a load can start behind another modal (the sample
//     editor's LOAD from its confirm), and only finishing or cancelling ends it. `running`, not
//     `shown` — a load owns the buttons from its first moment.
//   • RENDER is up while a render runs; a load inside it ranks above.
//   • the full HELP opens over the two in-place editors, never over a confirm, the keyboard or the FX
//     picker.
//   • QWERTY above THEME: the theme editor's SAVE raises the keyboard without closing.
const InputDispatcher::Layer InputDispatcher::LAYERS[] = {
    {&InputDispatcher::load_running,       &InputDispatcher::loading_layer},
    {&InputDispatcher::confirm_open,       &InputDispatcher::confirm_layer},
    {&InputDispatcher::sample_close_open,  &InputDispatcher::sample_close_layer},
    {&InputDispatcher::render_dialog_open, &InputDispatcher::render_dialog_layer},
    {&InputDispatcher::help_full_open,     &InputDispatcher::help_layer},
    {&InputDispatcher::qwerty_open,        &InputDispatcher::qwerty_layer},
    {&InputDispatcher::theme_open,         &InputDispatcher::theme_layer},
    {&InputDispatcher::eq_open,            &InputDispatcher::eq_layer},
    {&InputDispatcher::fx_helper_open,     &InputDispatcher::fx_helper_layer},
    {&InputDispatcher::map_picker_open,    &InputDispatcher::map_picker_layer},
};

const InputDispatcher::Layer* InputDispatcher::top_layer() const {
    for (const Layer& layer : LAYERS)
        if ((this->*layer.isOpen)()) return &layer;
    return nullptr;
}

// The grids: SONG, CHAIN and PHRASE share one cursor; TABLE keeps its own.
constexpr GridCursor SONG_GRID{&AppState::cursorRow, &AppState::cursorColumn, 255, 8};
constexpr GridCursor CHAIN_GRID{&AppState::cursorRow, &AppState::cursorColumn, 15, 2};
constexpr GridCursor PHRASE_GRID{&AppState::cursorRow, &AppState::cursorColumn, 15, 9};
constexpr GridCursor TABLE_GRID{&AppState::tableCursorRow, &AppState::tableCursorColumn, 15, 8};

// Every screen: a gesture its handler passes on runs the gesture's generic path, which asks the same
// row for the cell under the cursor and for the edit.
using D = InputDispatcher;
const D::ScreenHandler D::SCREENS[] = {
    {ScreenType::SAMPLE_EDITOR, &D::sample_editor_screen, &D::sample_editor_context, &D::sample_editor_edit, nullptr, nullptr},
    {ScreenType::FILE_BROWSER,  &D::file_browser_screen,  nullptr,                   nullptr,                nullptr, nullptr},
    {ScreenType::INSTRUMENT,    &D::instrument_screen,    &D::instrument_context,    &D::instrument_edit,    &D::instrument_knob, nullptr},
    {ScreenType::INST_POOL,     &D::pool_screen,          &D::pool_context,          &D::pool_edit,          nullptr, nullptr},
    {ScreenType::MODS,          &D::mods_screen,          &D::mods_context,          &D::mods_edit,          nullptr, nullptr},
    {ScreenType::PROJECT,       &D::project_screen,       &D::project_context,       &D::project_edit,       nullptr, nullptr},
    {ScreenType::SETTINGS,      &D::settings_screen,      &D::settings_context,      &D::settings_edit,      nullptr, nullptr},
    {ScreenType::MIDI,          &D::midi_screen,          &D::midi_context,          &D::midi_edit,          nullptr, nullptr},
    {ScreenType::MIDI_MAP,      &D::midi_map_screen,      &D::midi_map_context,      &D::midi_map_edit,      nullptr, nullptr},
    {ScreenType::SONG,          &D::song_screen,          &D::song_context,          &D::song_edit,          nullptr, &SONG_GRID},
    {ScreenType::CHAIN,         &D::chain_screen,         &D::chain_context,         &D::chain_edit,         nullptr, &CHAIN_GRID},
    {ScreenType::PHRASE,        &D::phrase_screen,        &D::phrase_context,        &D::phrase_edit,        nullptr, &PHRASE_GRID},
    {ScreenType::TABLE,         &D::table_screen,         &D::table_context,         &D::table_edit,         nullptr, &TABLE_GRID},
    {ScreenType::GROOVE,        &D::groove_screen,        &D::groove_context,        &D::groove_edit,        nullptr, nullptr},
    {ScreenType::SCALE,         &D::scale_screen,         &D::scale_context,         &D::scale_edit,         nullptr, nullptr},
    {ScreenType::MIXER,         &D::mixer_screen,         &D::mixer_context,         &D::mixer_edit,         &D::mixer_knob, nullptr},
    {ScreenType::EFFECTS,       &D::effects_screen,       &D::effects_context,       &D::effects_edit,       &D::effects_knob, nullptr},
};

const InputDispatcher::ScreenHandler* InputDispatcher::screen() const {
    for (const ScreenHandler& h : SCREENS)
        if (h.id == s_.currentScreen) return &h;
    return nullptr;
}

const GridCursor* InputDispatcher::grid() const {
    const ScreenHandler* h = screen();
    return h ? h->grid : nullptr;
}

bool InputDispatcher::route(Gesture g) {
    if (const Layer* top = top_layer())
        if ((this->*top->handle)(g) == GestureResult::TAKEN) return true;
    const ScreenHandler* h = screen();
    return h && (this->*h->handle)(g) == GestureResult::TAKEN;
}

}  // namespace pt::ui
