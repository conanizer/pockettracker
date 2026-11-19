#include "ui/modules/qwerty_keyboard.h"

#include "ui/helpers.h"

#include <algorithm>

namespace pt::ui {

// ─── The layouts: three rows of ten, so UP/DOWN stays in one column ──────────────────────────────
// The space bar row (case key, SPACE, layout key) and the virtual ABORT/APPLY row are not in the table.

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
    if (keyCursorRow == space_row_index()) return QWERTY_SPACE_ROW_COLS;
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
    return qwerty_key(row[static_cast<size_t>(col)], lower);
}

// ─── The verbs ───────────────────────────────────────────────────────────────────────────────────

void clamp_col(QwertyKeyboardState& s) {
    const int cols = s.current_row_cols();
    s.keyCursorCol = std::min(std::max(s.keyCursorCol, 0), cols - 1);
}

namespace {

// Crossing the SPACE row maps the column by what is drawn: a key column meets the space-row key
// under it; a space-row key meets the column under its left edge, the layout key the last one.
void move_key_cursor_to_row(QwertyKeyboardState& s, int row) {
    const int  spaceRow  = s.space_row_index();
    const bool fromKeys  = s.keyCursorRow < spaceRow;
    const bool fromSpace = s.keyCursorRow == spaceRow;
    const int  lastCol   = static_cast<int>(qwerty_rows(s.layout)[0].size()) - 1;
    if (row == spaceRow && fromKeys) {
        int key = 0, right = QWERTY_SPACE_ROW_SPANS[0];
        while (key + 1 < QWERTY_SPACE_ROW_COLS && s.keyCursorCol >= right) right += QWERTY_SPACE_ROW_SPANS[++key];
        s.keyCursorCol = key;
    } else if (row < spaceRow && fromSpace) {
        int left = 0;
        for (int key = 0; key < s.keyCursorCol; ++key) left += QWERTY_SPACE_ROW_SPANS[key];
        s.keyCursorCol = s.keyCursorCol == QWERTY_SPACE_ROW_COLS - 1 ? lastCol : left;
    }
    s.keyCursorRow = row;
    clamp_col(s);
}

}  // namespace

void move_key_cursor_up(QwertyKeyboardState& s) {
    move_key_cursor_to_row(s, (s.keyCursorRow == 0) ? s.total_rows() - 1 : s.keyCursorRow - 1);
}

void move_key_cursor_down(QwertyKeyboardState& s) {
    move_key_cursor_to_row(s, (s.keyCursorRow + 1) % s.total_rows());
}

void move_key_cursor_left(QwertyKeyboardState& s) {
    const int cols = s.current_row_cols();
    s.keyCursorCol = (s.keyCursorCol == 0) ? cols - 1 : s.keyCursorCol - 1;
}

void move_key_cursor_right(QwertyKeyboardState& s) {
    const int cols = s.current_row_cols();
    s.keyCursorCol = (s.keyCursorCol + 1) % cols;
}

// The text is UTF-8 and every count here is in letters, so an edit never splits one: half a letter
// in a name makes the file it is saved into unreadable.

void insert_current_key(QwertyKeyboardState& s) {
    if (s.is_on_action_row() || s.is_on_case_key() || s.is_on_layout_key()) return;   // they type nothing
    const int len = s.letter_count();
    if (len >= s.maxLength) return;

    // insertBefore: at the cursor, else one past it; either way the cursor lands after the new char.
    const int insertAt = s.insertBefore ? std::min(std::max(s.textCursor, 0), len)
                                        : std::min(s.textCursor + 1, len);
    s.text.insert(Canvas::glyph_offset(s.text, insertAt), 1, s.current_key());
    s.textCursor = insertAt + 1;
}

namespace {

void erase_letter(std::string& text, int letter) {
    const size_t from = Canvas::glyph_offset(text, letter);
    text.erase(from, Canvas::glyph_offset(text, letter + 1) - from);
}

}  // namespace

void delete_char(QwertyKeyboardState& s) {
    if (s.clearOnFirstB) {
        s.text.clear();
        s.textCursor    = 0;
        s.clearOnFirstB = false;
        return;
    }
    const int len = s.letter_count();
    if (s.insertBefore) {
        if (s.textCursor <= 0 || len == 0) return;                 // backspace
        erase_letter(s.text, s.textCursor - 1);
        s.textCursor -= 1;
    } else {
        if (s.textCursor >= len || len == 0) return;               // forward-delete
        erase_letter(s.text, s.textCursor);
        s.textCursor = std::min(s.textCursor, len - 1);
    }
}

void move_text_cursor_left(QwertyKeyboardState& s) {
    if (s.textCursor > 0) s.textCursor -= 1;
}

void move_text_cursor_right(QwertyKeyboardState& s) {
    if (s.textCursor < s.letter_count()) s.textCursor += 1;
}

QwertyTextWindow qwerty_text_window(int textLen, int textCursor, int windowCols) {
    if (windowCols < 1) windowCols = 1;
    if (textLen < 0) textLen = 0;
    textCursor = std::min(std::max(textCursor, 0), textLen);

    QwertyTextWindow w;
    // The phantom end-cursor slot counts, so the tail stays reachable.
    const int content = textLen + 1;
    if (content <= windowCols) {
        w.cols = content;  // everything fits: no scroll, no markers
        return w;
    }

    // Cursor at the centre, clamped to the ends.
    auto place = [&](int cols) {
        const int maxFirst = content - cols;
        int       f        = textCursor - cols / 2;
        if (f < 0) f = 0;
        if (f > maxFirst) f = maxFirst;
        return f;
    };
    constexpr int MARK = 1;  // the "…" is one column wide

    // Iterate to a fixed point: reserving a marker shrinks the window, which can reveal a clip on
    // the near side. Markers only grow (0→1→2); the bound is a guard.
    int  cols = windowCols;
    int  f    = place(cols);
    bool cl   = f > 0;
    bool cr   = f + cols < content;
    for (int guard = 0; guard < windowCols; ++guard) {
        const int next = std::max(1, windowCols - (cl ? MARK : 0) - (cr ? MARK : 0));
        if (next == cols) break;
        cols = next;
        f    = place(cols);
        cl   = f > 0;
        cr   = f + cols < content;
    }

    w.first     = f;
    w.cols      = cols;
    w.clipLeft  = cl;
    w.clipRight = cr;
    return w;
}

std::string trimmed_text(const QwertyKeyboardState& s) {
    size_t end = s.text.size();
    while (end > 0 && (s.text[end - 1] == ' ' || s.text[end - 1] == '\t')) --end;
    return s.text.substr(0, end);
}

// ─── Drawing ─────────────────────────────────────────────────────────────────────────────────────
// A 470×228 box, centred; the keys on a fixed 10-column grid.

void QwertyKeyboardOverlay::draw(Canvas& c, const QwertyKeyboardState& s, const Theme& t) const {
    constexpr int CS    = 3;
    constexpr int CELLH = 26;
    constexpr int CELLW = 46;                 // innerW 460 / 10 keys
    constexpr int ROWGAP = 4;

    // The fill: the border grows outward from it (helpers.h).
    constexpr int BOXW = 470;
    constexpr int BOXH = 228;
    constexpr int BOXX = (DESIGN_W - BOXW) / 2;
    constexpr int BOXY = (DESIGN_H - BOXH) / 2;
    constexpr int INNERX = BOXX + 5;
    constexpr int INNERW = BOXW - 10;

    // The backdrop dims everything behind; the shell dims the letterbox alike.
    draw_modal_backdrop(c);
    draw_modal_box(c, BOXX, BOXY, BOXW, BOXH, t);

    // The stack's rows sit at fixed offsets; only its start is derived, by centring.
    constexpr int TEXTROW_OFF = 30;   // the edited text, below the field label
    constexpr int KEYROWS_OFF = 66;   // the first key row, below the edited text
    const int contentH = KEYROWS_OFF + (s.total_rows() - 1) * (CELLH + ROWGAP) + CELLH;
    const int contentY = BOXY + (BOXH - contentH) / 2;

    // ── The header ──────────────────────────────────────────────────────────────────────────────
    const std::string fieldLabel = c.cased(s.fieldLabel);
    const int         labelW     = c.text_width(fieldLabel, TextSize::LARGE) + CS;
    c.text(fieldLabel, BOXX + (BOXW - labelW) / 2, contentY + 3, t.textParam, TextSize::LARGE);

    // ── The text row: a scroll window that keeps the cursor visible ─────────────────────────────
    // Short text is centred; overflowing, it left-aligns and scrolls with a "…" on the hidden side.
    const std::string      ELLIPSIS   = "\xE2\x80\xA6";   // U+2026, one column
    const int              textRowY   = contentY + TEXTROW_OFF;
    const int              textLen    = s.letter_count();
    const int              step       = c.char_step(TextSize::LARGE);   // the font's own spacing
    const int              glyphW     = c.glyph_width(TextSize::LARGE);
    // The cursor's margin, on both sides: the whole gap, so it reaches each neighbour without covering it.
    const int              pad        = step - glyphW;
    const int              windowCols = INNERW / step;    // whole character columns the box holds
    const QwertyTextWindow win        = qwerty_text_window(textLen, s.textCursor, windowCols);

    const bool fits    = (win.cols == textLen + 1);
    const int  leftPad = win.clipLeft ? 1 : 0;
    // Centre only when it fits, then clamp so the phantom end-cursor stays inside.
    const int  contentRight = textLen * step + glyphW + pad;   // right edge of the phantom cursor from startX
    int        startX       = fits ? BOXX + BOXW / 2 - (textLen * step) / 2 : INNERX;
    if (startX + contentRight > INNERX + INNERW) startX = INNERX + INNERW - contentRight;
    if (startX < INNERX + pad) startX = INNERX + pad;
    const int  textX   = startX + leftPad * step;

    const Argb markerColor = darken(t.textValue, 0.5f);
    if (win.clipLeft) c.text(ELLIPSIS, startX, textRowY + 3, markerColor, TextSize::LARGE);

    size_t at = Canvas::glyph_offset(s.text, win.first);
    for (int k = 0; k < win.cols; ++k) {
        const int  idx      = win.first + k;
        const int  px       = textX + k * step;
        const bool isCursor = (idx == s.textCursor);   // idx can be textLen: the phantom end
        if (isCursor) c.fill_rect(px - pad, textRowY, glyphW + 2 * pad, CELLH, t.rowCursor);
        if (idx < textLen) {
            const size_t next = Canvas::glyph_offset(s.text, idx + 1);
            c.text_as_typed(s.text.substr(at, next - at), px, textRowY + 3,
                   isCursor ? cursor_cell_ink(t) : t.textValue, TextSize::LARGE);
            at = next;
        }
    }

    if (win.clipRight)
        c.text(ELLIPSIS, textX + win.cols * step, textRowY + 3, markerColor, TextSize::LARGE);

    // ── The key rows ────────────────────────────────────────────────────────────────────────────
    const auto& rows     = qwerty_rows(s.layout);
    const int   rowBaseY = contentY + KEYROWS_OFF;
    const int   lastRow  = static_cast<int>(rows.size()) - 1;

    for (int r = 0; r <= lastRow; ++r) {
        const int rowY = rowBaseY + r * (CELLH + ROWGAP);

        if (r == lastRow) {
            // The case and layout keys name what they switch to, as written — "low" in small letters.
            const std::string labels[QWERTY_SPACE_ROW_COLS] = {qwerty_case_label(s.lower), c.cased("SPACE"),
                                                               qwerty_layout_label(1 - s.layout)};
            int keyX = INNERX;
            for (int col = 0; col < QWERTY_SPACE_ROW_COLS; ++col) {
                const int  keyW   = QWERTY_SPACE_ROW_SPANS[col] * CELLW - 1;
                const bool cursor = (s.keyCursorRow == r && s.keyCursorCol == col);
                const int  textW  = c.text_width(labels[col], TextSize::LARGE);
                c.fill_rect(keyX, rowY, keyW, CELLH, cursor ? t.rowCursor : t.background);
                c.text_as_typed(labels[col], keyX + (keyW - textW) / 2, rowY + 3,
                       cursor ? cursor_cell_ink(t) : t.textParam, TextSize::LARGE);
                keyX += QWERTY_SPACE_ROW_SPANS[col] * CELLW;
            }
            continue;
        }

        const auto& row = rows[static_cast<size_t>(r)];
        for (int col = 0; col < static_cast<int>(row.size()); ++col) {
            const int  cellX  = INNERX + col * CELLW;
            const bool cursor = (s.keyCursorRow == r && s.keyCursorCol == col);

            c.fill_rect(cellX, rowY, CELLW - 1, CELLH,
                        cursor ? t.rowCursor : t.background);

            c.text_as_typed(std::string(1, qwerty_key(row[static_cast<size_t>(col)], s.lower)),
                   cellX + (CELLW - 1 - glyphW) / 2, rowY + 3,
                   cursor ? cursor_cell_ink(t) : t.textValue, TextSize::LARGE);
        }
    }

    // ── ABORT / APPLY ───────────────────────────────────────────────────────────────────────────
    // A virtual row, also bound to SELECT and START — the labels say so.
    const int actionRow  = s.action_row_index();
    const int actionRowY = rowBaseY + actionRow * (CELLH + ROWGAP);
    constexpr int AFS    = 3;
    constexpr int AGAP   = 10;
    const int     ABTNW  = (INNERW - AGAP) / 2;

    // The word in the set case, the button that also does it in capitals.
    const std::string labels[QWERTY_ACTION_COLS] = {c.cased("ABORT") + "(SEL)", c.cased("APPLY") + "(START)"};
    for (int col = 0; col < QWERTY_ACTION_COLS; ++col) {
        const int  btnX   = INNERX + col * (ABTNW + AGAP);
        const bool cursor = (s.keyCursorRow == actionRow && s.keyCursorCol == col);

        c.fill_rect(btnX, actionRowY, ABTNW, CELLH,
                    cursor ? t.rowCursor : t.background);

        const std::string& label = labels[col];
        const int         lw    = c.text_width(label, TextSize::BUTTON) + CS;
        c.text(label, btnX + (ABTNW - lw) / 2, actionRowY + (CELLH - 5 * AFS) / 2,
               cursor ? cursor_cell_ink(t) : t.textParam, TextSize::BUTTON);
    }
}

}  // namespace pt::ui
