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

// The screens with buttons of their own. Every other screen runs each gesture's generic path.
const InputDispatcher::ScreenHandler InputDispatcher::SCREENS[] = {
    {ScreenType::SAMPLE_EDITOR, &InputDispatcher::sample_editor_screen},
    {ScreenType::FILE_BROWSER,  &InputDispatcher::file_browser_screen},
};

bool InputDispatcher::route(Gesture g) {
    if (const Layer* top = top_layer())
        if ((this->*top->handle)(g) == GestureResult::TAKEN) return true;
    for (const ScreenHandler& screen : SCREENS)
        if (screen.id == s_.currentScreen) return (this->*screen.handle)(g) == GestureResult::TAKEN;
    return false;
}

}  // namespace pt::ui
