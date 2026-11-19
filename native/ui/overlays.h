#pragma once

// What can be up over the screen, in one list: the top of the stack first. The first one up owns the
// buttons (the dispatcher's layers, ui/dispatch/route.cpp) and is drawn on top (ui/layout.cpp); the
// shell asks it whether to dim the letterbox and whether ONE HAND should let go.

#include "ui/app_state.h"

#include <cstddef>
#include <iterator>

namespace pt::ui {

enum class Overlay {
    LOADING, CONFIRM, SAMPLE_CLOSE, RENDER_DIALOG, HELP_FULL, QWERTY, THEME, EQ, SEND, STRETCH, FX_HELPER,
    MAP_PICKER, INSTRUMENT_PICKER, SF_PRESET_PICKER,
};

struct OverlayRow {
    Overlay id;
    bool (*up)(const AppState&);
    bool dims;       // a box over the dimmed canvas (`draw_modal_backdrop`)
    bool asks;       // waits for an answer: ONE HAND lets go of its locks, which would garble it
    bool inEditor;   // drawn in the editor's place under the furniture; nothing below it is drawn
};

// ⚠️ The order is the stack. LOADING is above everything, the confirm included, from the load's first
// moment. The full HELP opens over the in-place editors only. QWERTY is above THEME and SEND, and EQ
// above SEND: their SAVE and INP EQ raise them without closing.
inline constexpr OverlayRow OVERLAYS[] = {
    // Its strip dims nothing.
    {Overlay::LOADING,       [](const AppState& s) { return s.loading.running; },          false, false, false},
    {Overlay::CONFIRM,       [](const AppState& s) { return s.confirm.is_open(); },        true,  true,  false},
    // The sample editor's ARE YOU SURE? blanks the editor instead of dimming the frame.
    {Overlay::SAMPLE_CLOSE,  [](const AppState& s) {
         return s.currentScreen == ScreenType::SAMPLE_EDITOR && s.sampleEditor.showConfirmClose;
     },                                                                                     false, true,  false},
    {Overlay::RENDER_DIALOG, [](const AppState& s) { return s.renderDialog.isOpen; },      true,  true,  false},
    {Overlay::HELP_FULL,     [](const AppState& s) { return s.helpFull; },                 true,  false, false},
    {Overlay::QWERTY,        [](const AppState& s) { return s.qwerty.isOpen; },            true,  true,  false},
    {Overlay::THEME,         theme_editor_up,                                               false, false, true},
    {Overlay::EQ,            [](const AppState& s) { return s.eq.isOpen; },                false, false, true},
    {Overlay::SEND,          send_editor_up,                                                true,  false, false},
    {Overlay::STRETCH,       stretch_editor_up,                                             true,  false, false},
    // Not a modal, but it owns the buttons the same way.
    {Overlay::FX_HELPER,     [](const AppState& s) { return s.fxHelper.isOpen; },          true,  false, false},
    {Overlay::MAP_PICKER,    [](const AppState& s) { return s.mapPicker.isOpen; },         true,  false, false},
    {Overlay::INSTRUMENT_PICKER, [](const AppState& s) { return s.instrumentPicker.isOpen; }, true, false, false},
    {Overlay::SF_PRESET_PICKER,  [](const AppState& s) { return s.sfPresetPicker.isOpen; },   true, false, false},
};

// The one on top, or null.
inline const OverlayRow* top_overlay(const AppState& s) {
    for (const OverlayRow& o : OVERLAYS)
        if (o.up(s)) return &o;
    return nullptr;
}

// One past the lowest row drawn: an in-editor overlay that is up hides everything below it.
inline size_t overlays_drawn_end(const AppState& s) {
    for (size_t i = 0; i < std::size(OVERLAYS); ++i)
        if (OVERLAYS[i].inEditor && OVERLAYS[i].up(s)) return i + 1;
    return std::size(OVERLAYS);
}

// Is a dimming box drawn? The shell then dims the letterbox bars too.
inline bool modal_backdrop_active(const AppState& s) {
    for (size_t i = 0; i < overlays_drawn_end(s); ++i)
        if (OVERLAYS[i].dims && OVERLAYS[i].up(s)) return true;
    return false;
}

inline bool modal_question_up(const AppState& s) {
    for (const OverlayRow& o : OVERLAYS)
        if (o.asks && o.up(s)) return true;
    return false;
}

// Is the editor's module replaced (the furniture stays)?
inline bool overlay_in_editor(const AppState& s) {
    for (const OverlayRow& o : OVERLAYS)
        if (o.inEditor && o.up(s)) return true;
    return false;
}

}  // namespace pt::ui
