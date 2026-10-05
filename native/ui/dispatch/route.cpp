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
// A null handler means the layer is still answered inside each gesture's own body (`overlay_swallows`).
const InputDispatcher::Layer InputDispatcher::LAYERS[] = {
    {Overlay::LOADING,       &InputDispatcher::load_running,       &InputDispatcher::loading_layer},
    {Overlay::CONFIRM,       &InputDispatcher::confirm_open,       &InputDispatcher::confirm_layer},
    {Overlay::SAMPLE_CLOSE,  &InputDispatcher::sample_close_open,  &InputDispatcher::sample_close_layer},
    {Overlay::RENDER,        &InputDispatcher::render_dialog_open, &InputDispatcher::render_dialog_layer},
    {Overlay::HELP,          &InputDispatcher::help_full_open,     nullptr},
    {Overlay::QWERTY,        &InputDispatcher::qwerty_open,        &InputDispatcher::qwerty_layer},
    {Overlay::THEME,         &InputDispatcher::theme_open,         &InputDispatcher::theme_layer},
    {Overlay::EQ,            &InputDispatcher::eq_open,            &InputDispatcher::eq_layer},
    {Overlay::FX_HELPER,     &InputDispatcher::fx_helper_open,     &InputDispatcher::fx_helper_layer},
    {Overlay::MAP_PICK,      &InputDispatcher::map_picker_open,    &InputDispatcher::map_picker_layer},
};

const InputDispatcher::Layer* InputDispatcher::top_layer() const {
    for (const Layer& layer : LAYERS)
        if ((this->*layer.isOpen)()) return &layer;
    return nullptr;
}

InputDispatcher::Overlay InputDispatcher::top_overlay() const {
    const Layer* top = top_layer();
    return top ? top->id : Overlay::NONE;
}

// The screens with buttons of their own. Every other screen runs each gesture's generic path.
const InputDispatcher::ScreenHandler InputDispatcher::SCREENS[] = {
    {ScreenType::SAMPLE_EDITOR, &InputDispatcher::sample_editor_screen},
    {ScreenType::FILE_BROWSER,  &InputDispatcher::file_browser_screen},
};

bool InputDispatcher::route(Gesture g) {
    if (const Layer* top = top_layer()) {
        // ⚠️ No handler yet: the gesture's body answers the layer (`overlay_swallows`), and the screen
        // behind must not be asked.
        if (!top->handle) return false;
        if ((this->*top->handle)(g) == GestureResult::TAKEN) return true;
    }
    for (const ScreenHandler& screen : SCREENS)
        if (screen.id == s_.currentScreen) return (this->*screen.handle)(g) == GestureResult::TAKEN;
    return false;
}

}  // namespace pt::ui
