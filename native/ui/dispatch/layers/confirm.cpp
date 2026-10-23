// The confirm dialog ("A=YES  B=NO"): A does the thing it asked about, B doesn't. COLLECT_EXISTS
// moves between its answers with LEFT/RIGHT, and A takes the one under the cursor.

#include "ui/dispatch/dispatch_common.h"

#include "ui/lifecycle.h"        // the crash-recovery autosave — clear / load

#include <algorithm>
#include <string>

namespace pt::ui {

GestureResult InputDispatcher::confirm_layer(Gesture g) {
    switch (g) {
        case Gesture::A: confirm_accept(); break;
        case Gesture::B: confirm_cancel(); break;
        case Gesture::DPAD_LEFT:
        case Gesture::DPAD_RIGHT:
            if (s_.confirm.kind == ConfirmDialogState::Kind::COLLECT_EXISTS) {
                const int step = (g == Gesture::DPAD_LEFT) ? -1 : 1;
                s_.confirm.arg = std::clamp(s_.confirm.arg + step, ConfirmDialogState::COLLECT_REPLACE,
                                            ConfirmDialogState::COLLECT_CANCEL);
            }
            break;
        // Silencing a note is not an edit: an audition the dialog opened over must not be left ringing.
        case Gesture::STOP_PREVIEW: return GestureResult::PASS;
        default: break;
    }
    return GestureResult::TAKEN;
}

void InputDispatcher::confirm_accept() {
    const ConfirmDialogState::Kind kind = s_.confirm.kind;
    // ⚠️ Read `arg` before `close()` resets it — the CHANGE_TYPE arm needs the direction.
    const int argv = s_.confirm.arg;
    s_.confirm.close();   // FIRST — every arm below can re-open a dialog, and none should be stacked

    switch (kind) {
        case ConfirmDialogState::Kind::CLEAN_SEQ:
            host_.clean_seq();
            mark_modified();
            set_status(s_, "SEQ CLEANED", true);
            break;

        case ConfirmDialogState::Kind::CLEAN_INST: {
            // clean_inst also reloads the media, so the freed RAM drops now, not at the next open — and
            // the reload can fail like a project load.
            host_.clean_inst(fs_.samples_directory());
            mark_modified();
            const int failed = host_.last_media_load().failed;
            set_status(s_, failed > 0 ? "CLEANED: " + std::to_string(failed) + " MISSING" : "INST CLEANED",
                       failed == 0);
            break;
        }

        case ConfirmDialogState::Kind::NEW_PROJECT:
            start_new_project();
            break;

        case ConfirmDialogState::Kind::CHANGE_TYPE:
            // The direction comes back out of the dialog, so A+LEFT still steps backwards after a yes.
            toggle_instrument_type(argv);
            break;

        case ConfirmDialogState::Kind::EXIT:
            // ⚠️ A confirmed EXIT is the one clean death and the only deliberate discard, so it leaves
            // nothing to recover; every unclean death keeps the work via flush_autosave().
            autosave_clear(fs_);
            s_.shouldQuit = true;
            break;

        case ConfirmDialogState::Kind::RECOVER:
            // A = recover. The document comes back DIRTY and the file STAYS (see recover_from_autosave).
            if (!recover_from_autosave()) autosave_clear(fs_);
            break;

        case ConfirmDialogState::Kind::COLLECT_EXISTS: {
            const std::string folder = collect_folder_name(host_);
            if (argv == ConfirmDialogState::COLLECT_REPLACE)
                collect_song(folder);
            else if (argv == ConfirmDialogState::COLLECT_NEW_COPY)
                collect_song(collect_free_folder_name(fs_, folder));
            break;
        }

        case ConfirmDialogState::Kind::NONE:
            break;
    }
}

void InputDispatcher::confirm_cancel() {
    const ConfirmDialogState::Kind kind = s_.confirm.kind;
    s_.confirm.close();

    // ⚠️ The one NO that acts: it discards the work, or the prompt returns every launch and is
    // dismissed unread.
    if (kind == ConfirmDialogState::Kind::RECOVER) autosave_clear(fs_);
}

}  // namespace pt::ui
