#pragma once

// ─── The confirm dialog — the port's SECOND true modal ────────────────────────────────────────────
//
// A 260×55 box in the middle of a dimmed screen: a question, and "A=YES  B=NO". Kotlin's
// `drawSimpleConfirmDialog`, which serves four questions (CLEAN SEQ/INST, NEW PROJECT, CHANGE TYPE,
// RECOVER WORK) and is the thing standing between the user and every destructive action in the app.
//
// ⚠️ ONE STATE, WHERE KOTLIN HAS FOUR BOOLEANS — and that is the whole reason this is a file rather
// than a branch in the dispatcher. Android tracks `showCleanDialog`, `showNewProjectDialog`,
// `showInstrTypeDialog` and `showRecoveryDialog` separately, and every button handler that must not
// fire underneath a modal has to test all four:
//
//     showCleanDialog || showNewProjectDialog || showInstrTypeDialog || showRecoveryDialog || …
//
// with a comment above it warning that "every new show*Dialog-style modal state MUST be added to
// this predicate". A modal one handler forgets is a button that does the wrong thing exactly once —
// a bug nobody reports, because it reads as a mis-press. An enum with an `is_open()` cannot be
// forgotten: there is one thing to test, and adding a question does not change it. (Same move as
// S6a's `BrowserPurpose`, which replaced Android's untyped `instrumentFileBrowserAction` strings.)
//
// ⚠️ IT HAS NO CURSOR. Kotlin's `cleanDialogCursor` (0 = YES, 1 = NO) is dead state: it is passed to
// `drawCleanDialog`, which marks it `@Suppress("UNUSED_PARAMETER")` and forwards a title. The dialog
// draws a fixed "A=YES  B=NO" and the buttons ARE the answer. So there is nothing here to move.

#include <string>

#include "ui/canvas.h"
#include "ui/theme.h"

namespace pt::ui {

struct ConfirmDialogState {
    enum class Kind {
        NONE,
        CLEAN_SEQ,     // PROJECT → COMPACT → SEQ
        CLEAN_INST,    // PROJECT → COMPACT → INST
        NEW_PROJECT,   // PROJECT → NEW, and only when the project is DIRTY
        CHANGE_TYPE,   // INSTRUMENT → TYPE, when the slot already has a source loaded
        EXIT,          // PROJECT → EXIT, and only when the project is DIRTY (the shell only)

        /**
         * RECOVER WORK? — raised at BOOT, by nobody's button, when an autosave survived to launch
         * (S10). The only question in the app the user did not ask for.
         *
         * ⚠️ **AND THE ONLY ONE WHOSE `B` DOES REAL WORK.** Every other Kind's B is a pure cancel:
         * it closes the box and the world is exactly as it was. B here means *discard my unsaved
         * work*, and it has to DELETE the autosave — because the file's whole meaning is "the last
         * session ended badly", and a NO that left it on disk would ask the same question again on
         * the next launch, and the next, forever.
         *
         * So `confirm_cancel()` stops being a one-liner. That is worth saying out loud rather than
         * letting a future reader assume the symmetry still holds: cancelling this dialog is an
         * action, and ptdispatch pins that the other five stayed pure.
         */
        RECOVER,
    };

    Kind kind = Kind::NONE;

    bool is_open() const { return kind != Kind::NONE; }
    void open(Kind k)    { kind = k; }
    void close()         { kind = Kind::NONE; }
};

/** "CLEAN SEQ?" / "NEW PROJECT?" / … — the question the box asks. */
std::string confirm_dialog_title(ConfirmDialogState::Kind kind);

void draw_confirm_dialog(Canvas& c, const ConfirmDialogState& s, const Theme& t);

}  // namespace pt::ui
