#pragma once

// ─── The confirm dialog ──────────────────────────────────────────────────────────────────────────
//
// A 261×57 box on a dimmed screen: a question and "A=YES  B=NO" — the thing standing between the
// user and every destructive action in the app.
//
// ⚠️ ONE STATE, NOT A FLAG PER QUESTION: every handler that must not fire under a modal tests
// `is_open()`, and adding a question does not change that. A modal one handler forgets is a button
// that misfires once and reads as a mis-press.
//
// It has no cursor: the dialog draws a fixed "A=YES  B=NO" and the buttons ARE the answer.

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
         * RECOVER WORK? — raised at boot when an autosave survived to launch; the only question the
         * user did not ask for.
         *
         * ⚠️ AND THE ONLY ONE WHOSE `B` DOES REAL WORK: it discards the unsaved work and must DELETE
         * the autosave, or the same question returns every launch. Every other Kind's B is a pure
         * cancel, so `confirm_cancel()` is not one.
         */
        RECOVER,
    };

    Kind kind = Kind::NONE;

    /**
     * What the pending answer needs that its Kind does not — today only CHANGE_TYPE's direction
     * (+1 for A+RIGHT, −1 for A+LEFT). With three types the direction must survive the round trip
     * through the box. Reset on `close()`.
     */
    int arg = 0;

    bool is_open() const     { return kind != Kind::NONE; }
    void open(Kind k, int a = 0) { kind = k; arg = a; }
    void close()             { kind = Kind::NONE; arg = 0; }
};

/** "CLEAN SEQ?" / "NEW PROJECT?" / … — the question the box asks. */
std::string confirm_dialog_title(ConfirmDialogState::Kind kind);

/**
 * THE confirm box — geometry, frame and the "A=YES  B=NO" line — asking `title`.
 * Separate from `draw_confirm_dialog` so the SAMPLE EDITOR can ask its own "ARE YOU SURE?" in the
 * same box. ⚠️ It does not dim the screen: `draw_confirm_dialog` does that; the sample editor covers
 * its own surface instead.
 */
void draw_confirm_box(Canvas& c, const std::string& title, const Theme& t);

void draw_confirm_dialog(Canvas& c, const ConfirmDialogState& s, const Theme& t);

}  // namespace pt::ui
