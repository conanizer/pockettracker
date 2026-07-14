#pragma once

// ─── THE QWERTY KEYBOARD ─────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/overlays/QwertyKeyboardOverlay.kt (the state) and
// PixelPerfectRenderer.drawQwertyKeyboard (the pixels). A modal on-screen keyboard: it is how every
// piece of TEXT in the app gets typed on a device with eight buttons and no letters on any of them.
//
// It is an OVERLAY, not a screen: it draws on top of whatever is behind it, and while it is open it
// owns every button. That makes it the app's first true modal — and the one thing S6a needs that the
// modal system (still unbuilt) would otherwise have owned.
//
// ── Why it lands with the file browser rather than after it ──────────────────────────────────────
//
// Because the browser cannot rename or create a folder without it. Kotlin's `BrowserMode.RENAME` /
// `CREATE` — an in-place character editor driven by the CHARACTER cursor context — look like the
// alternative, but they are DEAD CODE: nothing on Android assigns either mode, and SELECT+A / SELECT+R
// have opened this keyboard instead since it was written. See ui/modules/file_browser.h.
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
 * What APPLY does with the text. The keyboard itself knows none of this — it collects a string and
 * hands it back, and the dispatcher acts on the purpose it opened the keyboard WITH.
 *
 * ⚠️ SCOPE: the five S6a can honour. Kotlin has ten. The other five belong to screens the port has not
 * reached (PROJECT_NAME → the PROJECT screen; SAMPLE_NAME / SAMPLE_SAVE → the sample editor, S6b;
 * THEME_SAVE → the theme editor; RESAMPLE → the render path) and land WITH them. VIDEO_EXTRACT lands
 * never: the port plan's 2026-07-11 amendment deletes the video→WAV converter from both platforms.
 */
enum class QwertyContext {
    FILE_RENAME,      // rename `contextExtra` (an absolute path), keeping its extension
    FOLDER_CREATE,    // create a folder named `text` inside `contextExtra` (a directory)
    INSTRUMENT_NAME,  // the current instrument's name; blank reverts it to "INSTxx"
    INSTRUMENT_SAVE,  // write the current instrument to `contextExtra`/<text>.pti
    SAMPLE_NAME,      // the sample editor's NAME row — renames the editor's sample AND its instrument
    SAMPLE_SAVE,      // SAVE-AS: write the edited sample to `contextExtra`/<text>.wav, de-duplicating
    PROJECT_NAME,     // the project's name (PROJECT row 2) — plain text, no file touched
    THEME_SAVE        // write the live theme to `contextExtra`/<text>.ptt, and name it <text> (S9)
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
     * INSERT MODE (a SETTINGS row on Android). True: A inserts BEFORE the cursor and B backspaces
     * (terminal-style). False: A inserts AFTER it and B forward-deletes (typewriter-style). It is one
     * flag, and it flips the meaning of BOTH buttons — which is why they read it rather than each
     * hard-coding a direction.
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
// Free functions over the state, as the Kotlin extension functions are — so a golden can drive them
// with nothing but a state struct, and `tools/ptinput` does exactly that.

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

/** Kotlin's `text.trimEnd()` — what APPLY hands back. */
std::string trimmed_text(const QwertyKeyboardState& s);

// ─── The overlay ─────────────────────────────────────────────────────────────────────────────────

class QwertyKeyboardOverlay {
  public:
    void draw(Canvas& c, const QwertyKeyboardState& s, const Theme& t) const;
};

}  // namespace pt::ui
