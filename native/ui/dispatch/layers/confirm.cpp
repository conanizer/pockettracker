// The confirm dialog ("A=YES  B=NO"): A does the thing it asked about, B doesn't.

#include "ui/dispatch/dispatch_common.h"

#include "ui/lifecycle.h"        // the crash-recovery autosave — clear / load

#include <string>

namespace pt::ui {

LayerResult InputDispatcher::confirm_layer(Gesture g) {
    switch (g) {
        case Gesture::A: confirm_accept(); break;
        case Gesture::B: confirm_cancel(); break;
        // Silencing a note is not an edit: an audition the dialog opened over must not be left ringing.
        case Gesture::STOP_PREVIEW: return LayerResult::PASS;
        default: break;
    }
    return LayerResult::TAKEN;
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
            s_.statusMessage = "SEQ CLEANED";
            s_.statusSuccess = true;
            break;

        case ConfirmDialogState::Kind::CLEAN_INST:
            // ⚠️ `clean_inst` also reloads the media: the emptied slots' buffers are still in the
            // engine, and without the reload the RAM would not drop until the next save and open.
            host_.clean_inst(fs_.samples_directory());
            mark_modified();
            {
                // The reload can fail like a project load — see load_project_done.
                const int failed = host_.last_media_load().failed;
                s_.statusMessage =
                    failed > 0 ? "CLEANED: " + std::to_string(failed) + " MISSING" : "INST CLEANED";
                s_.statusSuccess = (failed == 0);
            }
            break;

        case ConfirmDialogState::Kind::NEW_PROJECT:
            start_new_project();
            break;

        case ConfirmDialogState::Kind::CHANGE_TYPE:
            // Switching type frees a loaded source, hence the dialog. ⚠️ The direction comes back out
            // of the dialog (`arg`), so A+LEFT still steps backwards after a yes.
            toggle_instrument_type(argv);
            break;

        case ConfirmDialogState::Kind::EXIT:
            // ⚠️ A confirmed EXIT is the app's one clean death, so it leaves nothing to recover. That
            // is also why EXIT still asks despite the autosave: it is the only way to deliberately
            // discard a session. Every unclean death (SIGTERM, flat battery, crash, F10) keeps the work
            // via `flush_autosave()`.
            autosave_clear(fs_);
            s_.shouldQuit = true;
            break;

        case ConfirmDialogState::Kind::RECOVER:
            // A = recover. The document comes back DIRTY and the file STAYS (see recover_from_autosave).
            if (!recover_from_autosave()) autosave_clear(fs_);
            break;

        case ConfirmDialogState::Kind::NONE:
            break;
    }
}

void InputDispatcher::confirm_cancel() {
    const ConfirmDialogState::Kind kind = s_.confirm.kind;
    s_.confirm.close();

    // ⚠️ The one question whose NO is an action: discard the unsaved work. Leaving the file would bring
    // the prompt back every launch, teaching people to dismiss it unread.
    if (kind == ConfirmDialogState::Kind::RECOVER) autosave_clear(fs_);
}

}  // namespace pt::ui
