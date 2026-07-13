#include "ui/modules/qwerty_keyboard.h"

#include "ui/helpers.h"

#include <algorithm>

namespace pt::ui {

// ─── The layouts ─────────────────────────────────────────────────────────────────────────────────
//
// Three rows of TEN, both layouts, so the columns line up: D-pad UP/DOWN then stays in one column
// instead of drifting diagonally across ragged rows. The space bar is the fourth row and is
// special-cased in the nav (it is one key), and the ABORT/APPLY row is virtual — it has no characters,
// so it is not in this table at all.

const std::vector<std::vector<char>>& qwerty_rows(int layout) {
    static const std::vector<std::vector<char>> letters = {
        {'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P'},
        {'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', '"'},
        {'Z', 'X', 'C', 'V', 'B', 'N', 'M', '-', '_', '/'},
        {' '},
    };
    static const std::vector<std::vector<char>> symbols = {
        {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
        {'!', '#', '%', '/', '=', '+', '?', '.', '-', '_'},
        {'<', '>', '(', ')', '[', ']', ':', '|', '"', ','},
        {' '},
    };
    return layout == 0 ? letters : symbols;
}

// ─── State ───────────────────────────────────────────────────────────────────────────────────────

int QwertyKeyboardState::current_row_cols() const {
    if (is_on_action_row()) return QWERTY_ACTION_COLS;
    const auto& rows = qwerty_rows(layout);
    if (keyCursorRow < 0 || keyCursorRow >= static_cast<int>(rows.size())) return 1;
    return static_cast<int>(rows[static_cast<size_t>(keyCursorRow)].size());
}

char QwertyKeyboardState::current_key() const {
    if (is_on_action_row()) return ' ';
    const auto& rows = qwerty_rows(layout);
    if (keyCursorRow < 0 || keyCursorRow >= static_cast<int>(rows.size())) return ' ';
    const auto& row = rows[static_cast<size_t>(keyCursorRow)];
    if (row.empty()) return ' ';
    const int col = std::min(std::max(keyCursorCol, 0), static_cast<int>(row.size()) - 1);
    return row[static_cast<size_t>(col)];
}

// ─── The verbs ───────────────────────────────────────────────────────────────────────────────────

void clamp_col(QwertyKeyboardState& s) {
    const int cols = s.current_row_cols();
    s.keyCursorCol = std::min(std::max(s.keyCursorCol, 0), cols - 1);
}

void move_key_cursor_up(QwertyKeyboardState& s) {
    s.keyCursorRow = (s.keyCursorRow == 0) ? s.total_rows() - 1 : s.keyCursorRow - 1;
    clamp_col(s);
}

void move_key_cursor_down(QwertyKeyboardState& s) {
    s.keyCursorRow = (s.keyCursorRow + 1) % s.total_rows();
    clamp_col(s);
}

void move_key_cursor_left(QwertyKeyboardState& s) {
    const int cols = s.current_row_cols();
    s.keyCursorCol = (s.keyCursorCol == 0) ? cols - 1 : s.keyCursorCol - 1;
}

void move_key_cursor_right(QwertyKeyboardState& s) {
    const int cols = s.current_row_cols();
    s.keyCursorCol = (s.keyCursorCol + 1) % cols;
}

void insert_current_key(QwertyKeyboardState& s) {
    if (s.is_on_action_row()) return;                              // ABORT/APPLY type nothing
    if (static_cast<int>(s.text.size()) >= s.maxLength) return;

    const char ch = s.current_key();
    const int  len = static_cast<int>(s.text.size());
    // insertBefore: at the cursor. Otherwise: one past it — and the cursor lands after the new
    // character either way, which is what makes both modes feel like typing.
    const int insertAt = s.insertBefore ? std::min(std::max(s.textCursor, 0), len)
                                        : std::min(s.textCursor + 1, len);
    s.text.insert(static_cast<size_t>(insertAt), 1, ch);
    s.textCursor = insertAt + 1;
}

void delete_char(QwertyKeyboardState& s) {
    if (s.clearOnFirstB) {
        s.text.clear();
        s.textCursor    = 0;
        s.clearOnFirstB = false;
        return;
    }
    const int len = static_cast<int>(s.text.size());
    if (s.insertBefore) {
        if (s.textCursor <= 0 || len == 0) return;                 // backspace
        s.text.erase(static_cast<size_t>(s.textCursor - 1), 1);
        s.textCursor -= 1;
    } else {
        if (s.textCursor >= len || len == 0) return;               // forward-delete
        s.text.erase(static_cast<size_t>(s.textCursor), 1);
        s.textCursor = std::min(s.textCursor, static_cast<int>(s.text.size()));
    }
}

void move_text_cursor_left(QwertyKeyboardState& s) {
    if (s.textCursor > 0) s.textCursor -= 1;
}

void move_text_cursor_right(QwertyKeyboardState& s) {
    if (s.textCursor < static_cast<int>(s.text.size())) s.textCursor += 1;
}

std::string trimmed_text(const QwertyKeyboardState& s) {
    size_t end = s.text.size();
    while (end > 0 && (s.text[end - 1] == ' ' || s.text[end - 1] == '\t')) --end;
    return s.text.substr(0, end);
}

// ─── Drawing ─────────────────────────────────────────────────────────────────────────────────────
//
// A 470×228 box, centred. The geometry is PixelPerfectRenderer.drawQwertyKeyboard's, coordinate for
// coordinate — the keys are on a fixed 10-column grid (a ragged row just leaves its trailing columns
// empty), which is the same reason the layout tables are 10 wide.

void QwertyKeyboardOverlay::draw(Canvas& c, const QwertyKeyboardState& s, const Theme& t) const {
    constexpr int FS    = 4;                  // 20×20px keys — this is the one place the font goes big
    constexpr int CS    = 3;
    constexpr int CHARW = 5 * FS + CS;        // 23px per char slot
    constexpr int CELLH = 26;
    constexpr int CELLW = 46;                 // innerW 460 / 10 keys
    constexpr int ROWGAP = 4;

    constexpr int BOXW = 470;
    constexpr int BOXH = 228;
    constexpr int BOXX = (DESIGN_W - BOXW) / 2;   // 85
    constexpr int BOXY = (DESIGN_H - BOXH) / 2;   // 126
    constexpr int INNERX = BOXX + 5;
    constexpr int INNERW = BOXW - 10;

    // The backdrop dims everything behind it — this is a modal, and it should look like one.
    c.fill_rect(0, 0, DESIGN_W, DESIGN_H, 0xCC000000);

    c.fill_rect(BOXX, BOXY, BOXW, BOXH, t.meterBackground);
    c.stroke_rect(BOXX, BOXY, BOXW, BOXH, t.textTitle);

    // ── The header ──────────────────────────────────────────────────────────────────────────────
    const int labelW = static_cast<int>(s.fieldLabel.size()) * CHARW;
    c.draw_text(s.fieldLabel, BOXX + (BOXW - labelW) / 2, BOXY + 5 + 3, t.textParam, CS, FS);

    // ── The text row, centred on the box's midpoint ─────────────────────────────────────────────
    // It re-centres as you type, so the cursor stays near the middle of the screen instead of the
    // text marching off toward one edge.
    const int textRowY = BOXY + 35;
    const int textLen  = static_cast<int>(s.text.size());
    const int startX   = std::max(BOXX + BOXW / 2 - (textLen * CHARW) / 2, INNERX);

    for (int i = 0; i <= textLen; ++i) {   // <=, so the phantom cursor position past the last char draws
        const int  px       = startX + i * CHARW;
        const bool isCursor = (i == s.textCursor);
        if (isCursor) c.fill_rect(px, textRowY, 5 * FS, CELLH, darken(t.textCursor, 0.27f));
        if (i < textLen) {
            c.draw_text(std::string(1, s.text[static_cast<size_t>(i)]), px, textRowY + 3,
                        isCursor ? t.textCursor : t.textValue, CS, FS);
        }
    }

    // ── The key rows ────────────────────────────────────────────────────────────────────────────
    const auto& rows     = qwerty_rows(s.layout);
    const int   rowBaseY = BOXY + 71;
    const int   lastRow  = static_cast<int>(rows.size()) - 1;

    for (int r = 0; r <= lastRow; ++r) {
        const int rowY = rowBaseY + r * (CELLH + ROWGAP);

        if (r == lastRow) {
            // The space bar: one wide key, centred.
            const int  spaceW  = 7 * CELLW;
            const int  spaceX  = BOXX + (BOXW - spaceW) / 2;
            const bool cursor  = (s.keyCursorRow == r);
            c.fill_rect(spaceX, rowY, spaceW, CELLH,
                        cursor ? darken(t.textCursor, 0.27f) : t.meterBackground);
            if (cursor) c.stroke_rect(spaceX, rowY, spaceW, CELLH, t.textCursor);
            c.draw_text("SPACE", spaceX + (spaceW - 5 * CHARW) / 2, rowY + 3,
                        cursor ? t.textCursor : t.textParam, CS, FS);
            continue;
        }

        const auto& row = rows[static_cast<size_t>(r)];
        for (int col = 0; col < static_cast<int>(row.size()); ++col) {
            const int  cellX  = INNERX + col * CELLW;
            const bool cursor = (s.keyCursorRow == r && s.keyCursorCol == col);

            c.fill_rect(cellX, rowY, CELLW - 1, CELLH,
                        cursor ? darken(t.textCursor, 0.27f) : t.meterBackground);
            if (cursor) c.stroke_rect(cellX, rowY, CELLW - 1, CELLH, t.textCursor);

            c.draw_text(std::string(1, row[static_cast<size_t>(col)]),
                        cellX + (CELLW - 1 - 5 * FS) / 2, rowY + 3,
                        cursor ? t.textCursor : t.textValue, CS, FS);
        }
    }

    // ── ABORT / APPLY ───────────────────────────────────────────────────────────────────────────
    // A virtual row: reachable with the D-pad and pressable with A, and ALSO bound to the physical
    // SELECT and START. The labels say so, because a chord nobody can see is a chord nobody uses.
    const int actionRow  = s.action_row_index();
    const int actionRowY = rowBaseY + actionRow * (CELLH + ROWGAP);
    constexpr int AFS    = 3;
    constexpr int ACHARW = 5 * AFS + CS;
    constexpr int AGAP   = 10;
    const int     ABTNW  = (INNERW - AGAP) / 2;

    const char* labels[QWERTY_ACTION_COLS] = {"ABORT(SEL)", "APPLY(START)"};
    for (int col = 0; col < QWERTY_ACTION_COLS; ++col) {
        const int  btnX   = INNERX + col * (ABTNW + AGAP);
        const bool cursor = (s.keyCursorRow == actionRow && s.keyCursorCol == col);

        c.fill_rect(btnX, actionRowY, ABTNW, CELLH,
                    cursor ? darken(t.textCursor, 0.27f) : t.meterBackground);
        if (cursor) c.stroke_rect(btnX, actionRowY, ABTNW, CELLH, t.textCursor);

        const std::string label = labels[col];
        const int         lw    = static_cast<int>(label.size()) * ACHARW;
        c.draw_text(label, btnX + (ABTNW - lw) / 2, actionRowY + (CELLH - 5 * AFS) / 2,
                    cursor ? t.textCursor : t.textParam, CS, AFS);
    }
}

}  // namespace pt::ui
