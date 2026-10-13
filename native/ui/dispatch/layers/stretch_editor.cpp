// The TSTRETCH box: SYNC's stretch settings over the sample editor, raised by A on the TSTRETCH cell.
// It owns the buttons while it is up, but lets SELECT through — its cells have help.

#include "ui/dispatch/dispatch_common.h"

namespace pt::ui {

GestureResult InputDispatcher::stretch_editor_layer(Gesture g) {
    SampleEditorState& se = s_.sampleEditor;
    switch (g) {
        case Gesture::DPAD_UP:    se.stretchCursor = stretch_step(se.stretchCursor, -1, 0); break;
        case Gesture::DPAD_DOWN:  se.stretchCursor = stretch_step(se.stretchCursor, +1, 0); break;
        case Gesture::DPAD_LEFT:  se.stretchCursor = stretch_step(se.stretchCursor, 0, -1); break;
        case Gesture::DPAD_RIGHT: se.stretchCursor = stretch_step(se.stretchCursor, 0, +1); break;

        case Gesture::A_UP:    stretch_editor_edit(pt::ui::increment_fast); break;
        case Gesture::A_DOWN:  stretch_editor_edit(pt::ui::decrement_fast); break;
        case Gesture::A_LEFT:  stretch_editor_edit(pt::ui::decrement); break;
        case Gesture::A_RIGHT: stretch_editor_edit(pt::ui::increment); break;
        case Gesture::A_B:     stretch_editor_edit(pt::ui::on_a_b); break;

        case Gesture::A:
            if (se.stretchCursor == StretchCell::PREVIEW) {
                sample_editor_audition();
            } else if (se.stretchCursor == StretchCell::APPLY) {
                se.stretchOpen = false;
                before_long_operation();
                apply_sample_sync();
            }
            break;

        // The editor's own START: it plays the sample as these settings would leave it.
        case Gesture::START: sample_editor_audition(); break;

        // The settings stay for APPLY on the FX row.
        case Gesture::B: se.stretchOpen = false; break;

        case Gesture::SELECT:
            return GestureResult::PASS;

        default: break;
    }
    return GestureResult::TAKEN;
}

void InputDispatcher::open_stretch_editor() {
    s_.sampleEditor.stretchOpen   = true;
    s_.sampleEditor.stretchCursor = StretchCell::DURATION;
}

void InputDispatcher::stretch_editor_edit(InputAction (*fn)(const CursorContext&)) {
    const int         tempo  = s_.project->tempo;
    const InputAction action = fn(stretch_cursor_context(s_.sampleEditor, tempo));
    // The editor's session, not the document: nothing to mark modified.
    stretch_handle_input(s_.sampleEditor, action, tempo);
}

}  // namespace pt::ui
