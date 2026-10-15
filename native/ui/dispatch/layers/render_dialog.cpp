// The RENDER dialog (PROJECT → EXPORT → MIX / STEMS): the range, the repeat count, and RENDER.

#include "ui/dispatch/dispatch_common.h"

#include <algorithm>

namespace pt::ui {

namespace {

/** A+UP/DOWN: a page of song rows, or one repetition (REPEAT's whole range is OFF..×16). */
int coarse_step(const RenderDialogState& rd) { return rd.is_on(RenderRow::REPEAT) ? 1 : 16; }

}  // namespace

GestureResult InputDispatcher::render_dialog_layer(Gesture g) {
    switch (g) {
        case Gesture::DPAD_UP:   render_dialog_move_cursor(-1); break;
        case Gesture::DPAD_DOWN: render_dialog_move_cursor(+1); break;

        case Gesture::A_UP:    render_dialog_edit(+coarse_step(s_.renderDialog)); break;
        case Gesture::A_DOWN:  render_dialog_edit(-coarse_step(s_.renderDialog)); break;
        case Gesture::A_LEFT:  render_dialog_edit(-1); break;
        case Gesture::A_RIGHT: render_dialog_edit(+1); break;

        // R+UP/DOWN move the range to the previous or next part of the song.
        case Gesture::R_UP:   render_dialog_step_section(-1); break;
        case Gesture::R_DOWN: render_dialog_step_section(+1); break;

        // A fires from the RENDER row alone; the rows above are A+DPAD.
        case Gesture::A:
            if (s_.renderDialog.is_on(RenderRow::RENDER)) render_dialog_fire();
            break;

        // B closes, writing nothing. During a render no press arrives.
        case Gesture::B: s_.renderDialog.isOpen = false; break;

        default: break;
    }
    return GestureResult::TAKEN;
}

void InputDispatcher::open_render_dialog(RenderDialogState::Output output) {
    if (s_.isRendering) return;

    RenderDialogState& rd = s_.renderDialog;
    rd.isOpen    = true;
    rd.output    = output;
    rd.cursorRow = static_cast<int>(RenderRow::SONG_START);
    // ⚠️ The SONG screen's SAVED cursor: raised from PROJECT, the live `cursorRow` is PROJECT's.
    rd.startRow = songcore::song_section_start(host_.project(), s_.songCursorRow);
    rd.endRow   = -1;   // AUTO — follow the section, however it is edited between now and the render
}

void InputDispatcher::render_dialog_move_cursor(int delta) {
    const int last = static_cast<int>(RenderRow::COUNT) - 1;
    s_.renderDialog.cursorRow = std::clamp(s_.renderDialog.cursorRow + delta, 0, last);
}

void InputDispatcher::render_dialog_edit(int delta) {
    RenderDialogState& rd = s_.renderDialog;
    if (s_.isRendering) return;

    switch (static_cast<RenderRow>(rd.cursorRow)) {
        case RenderRow::SONG_START: {
            rd.startRow = std::clamp(rd.startRow + delta, 0, 255);
            // A start past a hand-typed end drags the end with it, so the range never runs backwards.
            if (rd.endRow >= 0 && rd.endRow < rd.startRow) rd.endRow = rd.startRow;
            break;
        }
        case RenderRow::SONG_END: {
            // ⚠️ AUTO sits below SONG START. UP off AUTO lands on the row it resolved to.
            if (rd.endRow < 0) {
                if (delta > 0) rd.endRow = render_dialog_end_row(rd, host_.project());
                break;
            }
            const int next = rd.endRow + delta;
            rd.endRow = (next < rd.startRow) ? -1 : std::min(255, next);
            break;
        }
        case RenderRow::REPEAT: {
            // Same shape: OFF sits below 2. There is no "×1" — that is what OFF says.
            const int next = rd.repeat + delta;
            rd.repeat = next < 2 ? 1 : std::min(RENDER_REPEAT_MAX, next);
            break;
        }
        case RenderRow::RENDER:
        case RenderRow::COUNT:
            break;
    }
}

void InputDispatcher::render_dialog_step_section(int delta) {
    if (s_.isRendering) return;
    RenderDialogState& rd = s_.renderDialog;
    // ⚠️ Moves the whole range from any row: a part is a start and an end, so the end goes back to AUTO.
    rd.startRow = songcore::adjacent_section_start(host_.project(), rd.startRow, delta);
    rd.endRow   = -1;
}

void InputDispatcher::render_dialog_fire() {
    if (s_.isRendering) return;
    export_song(s_.renderDialog.output == RenderDialogState::Output::STEMS);
    // ⚠️ Closed on the way out, worked or not: the answer goes on the status line, which this panel dims.
    s_.renderDialog.isOpen = false;
}

}  // namespace pt::ui
