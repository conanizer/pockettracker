#pragma once

// The confirm dialog: a question and "A=YES  B=NO" on a dimmed screen. No cursor — the buttons are
// the answer. ⚠️ One state, not a flag per question: every handler tests `is_open()`.

#include <string>

#include "ui/canvas.h"
#include "ui/theme.h"

namespace pt::ui {

struct ConfirmDialogState {
    enum class Kind {
        NONE,
        CLEAN_SEQ,     // PROJECT → COMPACT → SEQ
        CLEAN_INST,    // PROJECT → COMPACT → INST
        NEW_PROJECT,   // PROJECT → NEW, when DIRTY
        CHANGE_TYPE,   // INSTRUMENT → TYPE, when the slot already has a source loaded
        EXIT,          // PROJECT → EXIT, when DIRTY (the shell only)

        // RECOVER WORK? at boot, after an autosave survived. ⚠️ The only B that does work: it must
        // delete the autosave, or the question returns every launch.
        RECOVER,
    };

    Kind kind = Kind::NONE;

    // What the answer needs beyond its Kind: CHANGE_TYPE's direction (±1). Reset on `close()`.
    int arg = 0;

    bool is_open() const     { return kind != Kind::NONE; }
    void open(Kind k, int a = 0) { kind = k; arg = a; }
    void close()             { kind = Kind::NONE; arg = 0; }
};

std::string confirm_dialog_title(ConfirmDialogState::Kind kind);

// The box itself, asking `title`; the sample editor uses it alone. ⚠️ It does not dim the screen —
// `draw_confirm_dialog` does.
void draw_confirm_box(Canvas& c, const std::string& title, const Theme& t);

void draw_confirm_dialog(Canvas& c, const ConfirmDialogState& s, const Theme& t);

}  // namespace pt::ui
