#pragma once

// The confirm dialog: a question and "A=YES  B=NO" on a dimmed screen. No cursor — the buttons are
// the answer — except where it offers answers to pick with LEFT/RIGHT (`confirm_choices`). ⚠️ One
// state, not a flag per question: every handler tests `is_open()`.

#include <string>
#include <vector>

#include "ui/canvas.h"
#include "ui/theme.h"

namespace pt::ui {

struct ConfirmDialogState {
    enum class Kind {
        NONE,
        CLEAN_SEQ,     // PROJECT → COMPACT → SEQ
        CLEAN_INST,    // PROJECT → COMPACT → INST
        NEW_PROJECT,   // PROJECT → NEW, when DIRTY
        CHANGE_TYPE,   // INSTRUMENT → TYPE, when the slot holds a source or settings of its own
        EXIT,          // PROJECT → EXIT, when DIRTY (the shell only)

        // RECOVER WORK? at boot, after an autosave survived. ⚠️ The only B that does work: it must
        // delete the autosave, or the question returns every launch.
        RECOVER,

        COLLECT_EXISTS,   // PROJECT → COLLECT, when Songs/<name> is already there

        // Once per install, at boot: should the top panel show the guide? B is NO.
        TOP_PANEL,
    };

    // COLLECT_EXISTS's answers, left to right; `arg` is the one under the cursor.
    static constexpr int COLLECT_REPLACE  = 0;
    static constexpr int COLLECT_NEW_COPY = 1;
    static constexpr int COLLECT_CANCEL   = 2;

    // CHANGE_TYPE's answers, left to right. Both changes free the old type's source.
    static constexpr int TYPE_DISCARD = 0;   // the sound starts afresh
    static constexpr int TYPE_KEEP    = 1;
    static constexpr int TYPE_CANCEL  = 2;

    // TOP_PANEL's answers. YES sets SETTINGS > TOP PANEL to HINTS; NO leaves it as it is.
    static constexpr int TOP_PANEL_YES = 0;
    static constexpr int TOP_PANEL_NO  = 1;

    Kind kind = Kind::NONE;

    // A choice's pick. Reset on `close()`.
    int arg = 0;
    // CHANGE_TYPE's direction (±1), so A+LEFT still steps backwards after the answer.
    int step = 0;

    bool is_open() const     { return kind != Kind::NONE; }
    void open(Kind k, int a = 0) { kind = k; arg = a; step = 0; }
    void close()             { kind = Kind::NONE; arg = 0; step = 0; }
};

std::string confirm_dialog_title(ConfirmDialogState::Kind kind);

// Lines under the question, before the answers; empty for most.
std::vector<const char*> confirm_dialog_note(ConfirmDialogState::Kind kind);

// The answers a question offers in place of "A=YES  B=NO", left to right; empty for a yes/no one.
std::vector<const char*> confirm_choices(ConfirmDialogState::Kind kind);

// The box itself, asking `title`; the sample editor uses it alone. ⚠️ It does not dim the screen —
// `draw_confirm_dialog` does.
void draw_confirm_box(Canvas& c, const std::string& title, const Theme& t);

void draw_confirm_dialog(Canvas& c, const ConfirmDialogState& s, const Theme& t);

}  // namespace pt::ui
