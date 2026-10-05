// The layer stack: the dialogs and editors that sit over a screen and own the buttons while they are
// up. Only the TOP layer is asked, and a gesture it does not answer goes nowhere.

#include "ui/dispatch/dispatch_common.h"

namespace pt::ui {

// ⚠️ The ORDER is the stack — the first open entry is the top. The pairs that can be open together:
//   • LOADING above everything, the confirm included: a load can start behind another modal (the sample
//     editor's LOAD from its confirm), and only finishing or cancelling ends it. `running`, not
//     `shown` — a load owns the buttons from its first moment.
//   • RENDER is up while a render runs; a load inside it ranks above.
//   • the full HELP opens over the browser and the two in-place editors, never over a confirm, the
//     keyboard or the FX picker.
//   • QWERTY above THEME: the theme editor's SAVE raises the keyboard without closing.
// A null handler means the layer is still answered inside each gesture's own body (`overlay_swallows`).
const InputDispatcher::Layer InputDispatcher::LAYERS[] = {
    {Overlay::LOADING,   &InputDispatcher::load_running,       &InputDispatcher::loading_layer},
    {Overlay::CONFIRM,   &InputDispatcher::confirm_open,       &InputDispatcher::confirm_layer},
    {Overlay::RENDER,    &InputDispatcher::render_dialog_open, &InputDispatcher::render_dialog_layer},
    {Overlay::HELP,      &InputDispatcher::help_full_open,     nullptr},
    {Overlay::QWERTY,    &InputDispatcher::qwerty_open,        &InputDispatcher::qwerty_layer},
    {Overlay::THEME,     &InputDispatcher::theme_open,         &InputDispatcher::theme_layer},
    {Overlay::EQ,        &InputDispatcher::eq_open,            &InputDispatcher::eq_layer},
    {Overlay::FX_HELPER, &InputDispatcher::fx_helper_open,     &InputDispatcher::fx_helper_layer},
    {Overlay::MAP_PICK,  &InputDispatcher::map_picker_open,    nullptr},
    {Overlay::BROWSER,   &InputDispatcher::on_browser,         nullptr},
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

bool InputDispatcher::layer_takes(Gesture g) {
    const Layer* top = top_layer();
    return top && top->handle && (this->*top->handle)(g) == LayerResult::TAKEN;
}

}  // namespace pt::ui
