#pragma once

// ─── THE QWERTY KEYBOARD ─────────────────────────────────────────────────────────────────────────
//
// A modal on-screen keyboard: how every piece of TEXT in the app is typed on a device with eight
// buttons. An OVERLAY drawn over whatever is behind it, owning every button while open.
//
//   DPAD          move the key cursor (3 key rows, then SPACE, then the ABORT/APPLY row)
//   A             type the key under the cursor — or, on the action row, cancel / confirm
//   B             delete (backspace or forward-delete, per the INSERT MODE setting)
//   R+DOWN / UP   switch layout: letters ↔ numbers+symbols
//   R+LEFT/RIGHT  move the TEXT cursor (not the key cursor)
//   SELECT        cancel      — the ABORT button, as a chord
//   START         apply       — the APPLY button, as a chord

#include "ui/canvas.h"
#include "ui/theme.h"

#include <string>
#include <vector>

namespace pt::ui {

/**
 * What APPLY does with the text. The keyboard collects a string and hands it back; the dispatcher
 * acts on the purpose it opened the keyboard WITH. Appended to, never reordered.
 */
enum class QwertyContext {
    FILE_RENAME,      // rename `contextExtra` (an absolute path), keeping its extension
    FOLDER_CREATE,    // create a folder named `text` inside `contextExtra` (a directory)
    INSTRUMENT_NAME,  // the current instrument's name; blank reverts it to "INSTxx"
    INSTRUMENT_SAVE,  // write the current instrument to `contextExtra`/<text>.pti
    SAMPLE_NAME,      // the sample editor's NAME row — renames the editor's sample AND its instrument
    SAMPLE_SAVE,      // SAVE-AS: write the edited sample to `contextExtra`/<text>.wav, de-duplicating
    PROJECT_NAME,     // the project's name (PROJECT row 2) — plain text, no file touched
    THEME_SAVE,       // write the live theme to `contextExtra`/<text>.ptt, and name it <text>
    RESAMPLE,         // render the SONG selection to `<Resampled>/<text>.wav`, then auto-instrument it
    SCALE_SAVE,       // write the SCALE screen's slot to `contextExtra`/<text>.pts, and name it <text>
    GROOVE_SAVE       // write the GROOVE screen's slot to `contextExtra`/<text>.ptg, and name it <text>
};

/** 3 key rows of 10, then the space bar. The action row (ABORT / APPLY) is virtual — see below. */
const std::vector<std::vector<char>>& qwerty_rows(int layout);

/** "ABC" / "123" — the layout indicator the box draws. */
inline const char* qwerty_layout_label(int layout) { return layout == 0 ? "ABC" : "123"; }

/** The virtual action row has exactly two buttons: col 0 = ABORT, col 1 = APPLY. */
inline constexpr int QWERTY_ACTION_COLS = 2;

struct QwertyKeyboardState {
    bool        isOpen    = false;
    std::string text;
    int         maxLength = 20;

    /** The INSERTION POINT in `text` — 0 is before the first char, text.size() is after the last. */
    int textCursor = 0;

    int keyCursorRow = 0;
    int keyCursorCol = 0;
    int layout       = 0;   // 0 = letters, 1 = numbers/symbols

    std::string fieldLabel = "NAME:";

    /**
     * INSERT MODE. True: A inserts BEFORE the cursor and B backspaces (terminal-style). False: A
     * inserts AFTER it and B forward-deletes. One flag flips both buttons.
     */
    bool insertBefore = true;

    /** True: the FIRST B clears the whole field instead of deleting one character. A "SAVE AS" that
     *  suggests a name wants this — the user is renaming, not editing. Consumed on that first press. */
    bool clearOnFirstB = false;

    QwertyContext context      = QwertyContext::FILE_RENAME;
    std::string   contextExtra;   // a path or a directory, per `context`

    // ── Geometry of the cursor ──────────────────────────────────────────────────────────────────

    /** One past the last key row — the virtual ABORT/APPLY row, which has no characters on it. */
    int action_row_index() const { return static_cast<int>(qwerty_rows(layout).size()); }
    bool is_on_action_row() const { return keyCursorRow == action_row_index(); }
    int  total_rows() const { return action_row_index() + 1; }
    int  current_row_cols() const;

    /** The character under the key cursor. ' ' on the action row (A is special-cased there). */
    char current_key() const;
};

// ─── The verbs ───────────────────────────────────────────────────────────────────────────────────
//
// Free functions over the state, so they can be driven with nothing but a state struct.

/** Keep `keyCursorCol` inside the row `keyCursorRow` points at. Called after any row change. */
void clamp_col(QwertyKeyboardState& s);

void move_key_cursor_up(QwertyKeyboardState& s);    // wraps row 0 → the action row
void move_key_cursor_down(QwertyKeyboardState& s);  // wraps the action row → row 0
void move_key_cursor_left(QwertyKeyboardState& s);  // wraps within the row
void move_key_cursor_right(QwertyKeyboardState& s);

/** Type the key under the cursor. A no-op on the action row and at `maxLength`. */
void insert_current_key(QwertyKeyboardState& s);

/** Backspace or forward-delete, per `insertBefore` — or clear the field, per `clearOnFirstB`. */
void delete_char(QwertyKeyboardState& s);

void move_text_cursor_left(QwertyKeyboardState& s);
void move_text_cursor_right(QwertyKeyboardState& s);

/**
 * A horizontal scroll window over `text` that keeps `textCursor` visible; either end can be
 * clipped, marked by a "…".
 *   • text fits             → all of it, no markers.
 *   • cursor near the end   → pin right: the tail, "…" on the left.
 *   • cursor near the start → pin left: the head, "…" on the right.
 *   • cursor in the middle  → the cursor sits at the box centre, "…" both sides.
 * A "…" takes ONE column from its side; `first` and `cols` describe the character region only.
 */
struct QwertyTextWindow {
    int  first     = 0;      // first visible character index
    int  cols      = 0;      // character columns available (chars + the phantom end-cursor slot)
    bool clipLeft  = false;  // hidden characters before `first` — draw a leading "…"
    bool clipRight = false;  // hidden characters at/after the window — draw a trailing "…"
};
QwertyTextWindow qwerty_text_window(int textLen, int textCursor, int windowCols);

/** `text` with trailing whitespace dropped — what APPLY hands back. */
std::string trimmed_text(const QwertyKeyboardState& s);

// ─── The overlay ─────────────────────────────────────────────────────────────────────────────────

class QwertyKeyboardOverlay {
  public:
    void draw(Canvas& c, const QwertyKeyboardState& s, const Theme& t) const;
};

}  // namespace pt::ui
