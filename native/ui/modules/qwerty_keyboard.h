#pragma once

// The on-screen keyboard, a modal that owns every button while open: how all text is typed.
//   DPAD          move the key cursor (3 key rows, SPACE + the layout key, then ABORT/APPLY)
//   A             type the key — or switch layout, cancel, confirm
//   B             delete (backspace or forward-delete, per INSERT MODE)
//   R+DOWN / UP   switch layout (letters ↔ numbers+symbols)
//   R+LEFT/RIGHT  move the text cursor
//   SELECT        cancel · START apply

#include "ui/canvas.h"
#include "ui/theme.h"

#include <string>
#include <vector>

namespace pt::ui {

// What APPLY does with the text; the dispatcher acts on it. Append, never reorder.
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
    GROOVE_SAVE,      // write the GROOVE screen's slot to `contextExtra`/<text>.ptg, and name it <text>
    REVERB_SAVE,      // write EFFECTS' reverb cells to `contextExtra`/<text>.ptr, and name them <text>
    DELAY_SAVE        // write EFFECTS' delay cells to `contextExtra`/<text>.ptd, and name them <text>
};

// 3 key rows of 10, then the space bar. The ABORT/APPLY row is virtual.
const std::vector<std::vector<char>>& qwerty_rows(int layout);

// The layout key shows the one it switches to.
inline const char* qwerty_layout_label(int layout) { return layout == 0 ? "ABC" : "123"; }

// SPACE under key columns 0-7, the layout key under 8-9.
inline constexpr int QWERTY_SPACE_ROW_COLS = 2;
inline constexpr int QWERTY_SPACE_KEY_COLS = 8;

// Col 0 = ABORT, col 1 = APPLY.
inline constexpr int QWERTY_ACTION_COLS = 2;

struct QwertyKeyboardState {
    bool        isOpen    = false;
    std::string text;
    int         maxLength = 20;

    // The insertion point: 0 is before the first char, text.size() after the last.
    int textCursor = 0;

    int keyCursorRow = 0;
    int keyCursorCol = 0;
    int layout       = 0;   // 0 = letters, 1 = numbers/symbols

    std::string fieldLabel = "NAME:";

    // INSERT MODE. True: A inserts before the cursor, B backspaces. False: after, and B deletes forward.
    bool insertBefore = true;

    // The first B clears the whole field (a suggested SAVE AS name). Consumed on that press.
    bool clearOnFirstB = false;

    QwertyContext context      = QwertyContext::FILE_RENAME;
    std::string   contextExtra;   // a path or a directory, per `context`

    // ── Cursor geometry. The action row is one past the last key row ─────────────────────────────
    int action_row_index() const { return static_cast<int>(qwerty_rows(layout).size()); }
    bool is_on_action_row() const { return keyCursorRow == action_row_index(); }
    int  space_row_index() const { return action_row_index() - 1; }
    bool is_on_layout_key() const { return keyCursorRow == space_row_index() && keyCursorCol == 1; }
    int  total_rows() const { return action_row_index() + 1; }
    int  current_row_cols() const;

    // ' ' on the action row.
    char current_key() const;
};

// ─── The verbs, free functions over the state ────────────────────────────────────────────────────

// Keep `keyCursorCol` inside its row; after any row change.

void clamp_col(QwertyKeyboardState& s);

void move_key_cursor_up(QwertyKeyboardState& s);    // wraps row 0 → the action row
void move_key_cursor_down(QwertyKeyboardState& s);  // wraps the action row → row 0
void move_key_cursor_left(QwertyKeyboardState& s);  // wraps within the row
void move_key_cursor_right(QwertyKeyboardState& s);

// A no-op on the action row, the layout key and at `maxLength`.
void insert_current_key(QwertyKeyboardState& s);

// Backspace or forward-delete per `insertBefore`, or clear the field per `clearOnFirstB`.
void delete_char(QwertyKeyboardState& s);

void move_text_cursor_left(QwertyKeyboardState& s);
void move_text_cursor_right(QwertyKeyboardState& s);

// A scroll window over `text` that keeps `textCursor` visible; a clipped end is marked "…" and the
// "…" takes one column. Pinned right near the end, left near the start, centred in between.
struct QwertyTextWindow {
    int  first     = 0;      // first visible character index
    int  cols      = 0;      // character columns available (chars + the phantom end-cursor slot)
    bool clipLeft  = false;  // hidden characters before `first` — draw a leading "…"
    bool clipRight = false;  // hidden characters at/after the window — draw a trailing "…"
};
QwertyTextWindow qwerty_text_window(int textLen, int textCursor, int windowCols);

// What APPLY hands back: trailing whitespace dropped.
std::string trimmed_text(const QwertyKeyboardState& s);

// ─── The overlay ─────────────────────────────────────────────────────────────────────────────────

class QwertyKeyboardOverlay {
  public:
    void draw(Canvas& c, const QwertyKeyboardState& s, const Theme& t) const;
};

}  // namespace pt::ui
