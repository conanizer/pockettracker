// The SEND EDITOR: the reverb's or the delay's cells in a box, raised from a REV or DEL cell. Lets START,
// the audition and SELECT through. ⚠️ currentScreen still names the screen underneath: never read its cursor.

#include "ui/dispatch/dispatch_common.h"

namespace pt::ui {

GestureResult InputDispatcher::send_editor_layer(Gesture g) {
    int& row = s_.sendEditor.cursorRow;
    switch (g) {
        // EFFECTS' own walk, kept inside the one section on screen.
        case Gesture::DPAD_UP:    row = effects_next_row_in_section(row, -1); break;
        case Gesture::DPAD_DOWN:  row = effects_next_row_in_section(row, +1); break;
        case Gesture::DPAD_LEFT:  row = effects_step_column(row, -1); break;
        case Gesture::DPAD_RIGHT: row = effects_step_column(row, +1); break;

        case Gesture::A_UP:    send_editor_edit(pt::ui::increment_fast); break;
        case Gesture::A_DOWN:  send_editor_edit(pt::ui::decrement_fast); break;
        case Gesture::A_LEFT:  send_editor_edit(pt::ui::decrement); break;
        case Gesture::A_RIGHT: send_editor_edit(pt::ui::increment); break;
        case Gesture::A_B:     send_editor_edit(pt::ui::on_a_b); break;

        // The preset line's SAVE and LOAD; INP EQ and TIME answer on the release.
        case Gesture::A:
            if (!effects_preset_action(row)) effects_release_action(row, /*peek=*/false);
            break;

        case Gesture::B: close_send_editor(); break;

        // Heard while dialled; the screen underneath stops its own audition.
        case Gesture::START:
        case Gesture::A_START:
        case Gesture::STOP_PREVIEW:
        case Gesture::SELECT:
            return GestureResult::PASS;

        // R+DPAD included: the editor is not a cell in the screen grid.
        default: break;
    }
    return GestureResult::TAKEN;
}

void InputDispatcher::open_send_editor(EffectsSection section) {
    s_.sendEditor           = SendEditorState{};
    s_.sendEditor.isOpen    = true;
    s_.sendEditor.cursorRow = effects_section_first_row(section);
}

void InputDispatcher::send_editor_edit(InputAction (*fn)(const CursorContext&)) {
    EffectState es{*s_.project};
    es.cursorRow = s_.sendEditor.cursorRow;

    const InputAction action = fn(effects_.cursor_context(es));
    if (action.type == ActionType::NONE) return;
    // `mark_modified` pushes the globals while the editor is open (`on_globals_screen`).
    if (effects_.handle_input(host_.edit_project(), es.cursorRow, action).modified) mark_modified();
}

}  // namespace pt::ui
