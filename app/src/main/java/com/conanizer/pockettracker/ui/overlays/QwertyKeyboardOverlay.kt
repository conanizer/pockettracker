package com.conanizer.pockettracker.ui.overlays

/**
 * QWERTY KEYBOARD OVERLAY
 *
 * A modal on-screen keyboard for text input, triggered by pressing SELECT
 * on any character cell in a text input row (e.g., NAME row in PROJECT screen).
 *
 * Controls:
 *   DPAD         — move keyboard cursor (3 aligned 10-key rows, then SPACE, then ABORT/APPLY)
 *   A            — type the highlighted character; on the ABORT/APPLY row, cancel/apply
 *   B            — delete (backspace or forward-delete, depends on INSERT MODE setting)
 *   R+DPAD DOWN  — switch to numbers/symbols layout
 *   R+DPAD UP    — switch back to letters layout
 *   R+DPAD LEFT  — move text cursor left
 *   R+DPAD RIGHT — move text cursor right
 *   SELECT       — cancel (discard changes, close keyboard) — same as the ABORT button
 *   START        — apply (save text, close keyboard) — same as the APPLY button
 *
 * TODO: L+B — enter copy/paste text mode (future)
 */

// ============================================================================
// KEYBOARD LAYOUT DATA
// ============================================================================

// Three equal 10-wide key rows so columns line up — DPAD up/down stays in one column instead of
// drifting diagonally. The space row + the virtual ABORT/APPLY action row are special-cased in nav.

/** QWERTY letter rows (layout = 0) */
val QWERTY_ROWS_LETTERS: List<List<Char>> = listOf(
    listOf('Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P'),
    listOf('A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', '"'),
    listOf('Z', 'X', 'C', 'V', 'B', 'N', 'M', '-', '_', '/'),
    listOf(' ')   // Space bar
)

/** Numbers + supported symbols row (layout = 1) — same 10-wide shape as the letters layout */
val QWERTY_ROWS_SYMBOLS: List<List<Char>> = listOf(
    listOf('1', '2', '3', '4', '5', '6', '7', '8', '9', '0'),
    listOf('!', '#', '%', '/', '=', '+', '?', '.', '-', '_'),
    listOf('<', '>', '(', ')', '[', ']', ':', '|', '"', ','),
    listOf(' ')   // Space bar
)

/** Number of buttons on the virtual ABORT/APPLY action row (col 0 = ABORT, col 1 = APPLY). */
const val QWERTY_ACTION_COLS = 2

/** Returns the key rows for a given layout index (0=letters, 1=symbols). */
fun qwertyRowsForLayout(layout: Int): List<List<Char>> =
    if (layout == 0) QWERTY_ROWS_LETTERS else QWERTY_ROWS_SYMBOLS

/** Returns the label string shown for the current layout (e.g. "ABC" or "123"). */
fun qwertyLayoutLabel(layout: Int): String = if (layout == 0) "ABC" else "123"

// ============================================================================
// CONTEXT
// ============================================================================

/**
 * Determines what action START performs when the keyboard is confirmed.
 */
enum class QwertyContext {
    PROJECT_NAME,   // Apply text as project name
    INSTRUMENT_NAME, // Apply text as the current instrument's name (blank reverts to "INSTxx")
    FILE_RENAME,    // Rename file at contextExtra path (keep original extension)
    FOLDER_CREATE,  // Create folder in contextExtra directory with typed name
    RESAMPLE,       // Render selection to WAV with typed name as base name
    INSTRUMENT_SAVE, // Save instrument as .pti with typed name; contextExtra = instruments directory
    SAMPLE_NAME,     // Rename sample in sample editor; contextExtra = samplesDirectory
    SAMPLE_SAVE,     // Save sample editor buffer to WAV; contextExtra = samples directory
    THEME_SAVE,      // Save current theme as .ptt; contextExtra = themes directory
    VIDEO_EXTRACT    // Extract audio from video; contextExtra = source video file path
}

// ============================================================================
// STATE
// ============================================================================

/**
 * All state needed to display and operate the QWERTY keyboard overlay.
 *
 * @param isOpen          Whether the keyboard overlay is visible
 * @param text            The string currently being edited (max [maxLength] chars)
 * @param maxLength       Maximum number of characters allowed
 * @param textCursor      Insertion-point index in [text] (0 = before first char, text.length = after last)
 * @param keyCursorRow    Which keyboard row the keyboard cursor is on (0-3)
 * @param keyCursorCol    Which key column the keyboard cursor is on
 * @param layout          0 = letters (QWERTY), 1 = numbers/symbols
 * @param fieldLabel      Context-sensitive label shown as header (e.g. "PROJECT NAME:", "FOLDER NAME:")
 * @param originalText    Snapshot of the text when the keyboard was opened — used for cancel
 * @param insertBefore    If true: A inserts BEFORE textCursor (terminal-style, cursor advances).
 *                        If false: A inserts AFTER textCursor (typewriter-style, cursor advances).
 * @param clearOnFirstB   If true: first B press clears all text instead of deleting one char
 * @param context         What action START performs (see [QwertyContext])
 * @param contextExtra    Extra data for context (file path for RENAME, directory for FOLDER_CREATE)
 */
data class QwertyKeyboardState(
    val isOpen: Boolean = false,
    val text: String = "",
    val maxLength: Int = 20,
    val textCursor: Int = 0,
    val keyCursorRow: Int = 0,
    val keyCursorCol: Int = 0,
    val layout: Int = 0,
    val fieldLabel: String = "PROJECT NAME:",
    val originalText: String = "",
    val insertBefore: Boolean = true,
    val clearOnFirstB: Boolean = false,
    val context: QwertyContext = QwertyContext.PROJECT_NAME,
    val contextExtra: String = ""
)

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/** Row index of the virtual ABORT/APPLY action row — one past the last char row (the space row). */
fun QwertyKeyboardState.actionRowIndex(): Int = qwertyRowsForLayout(layout).size

/** True when the keyboard cursor is on the ABORT/APPLY action row. */
fun QwertyKeyboardState.isOnActionRow(): Boolean = keyCursorRow == actionRowIndex()

/** Total navigable rows = char rows (incl. the space row) + the action row. */
fun QwertyKeyboardState.totalRows(): Int = qwertyRowsForLayout(layout).size + 1

/** Number of columns on the row the cursor is on (the action row has [QWERTY_ACTION_COLS]). */
private fun QwertyKeyboardState.currentRowCols(): Int =
    if (isOnActionRow()) QWERTY_ACTION_COLS
    else qwertyRowsForLayout(layout).getOrNull(keyCursorRow)?.size ?: 1

/**
 * Returns the character at the keyboard cursor position for the given state.
 * The space row returns ' '; the action row has no character (returns ' ', but A is special-cased
 * to ABORT/APPLY in the dispatcher and never inserts here).
 */
fun QwertyKeyboardState.currentKey(): Char {
    if (isOnActionRow()) return ' '
    val rows = qwertyRowsForLayout(layout)
    val row = rows.getOrNull(keyCursorRow) ?: return ' '
    return row.getOrElse(keyCursorCol.coerceIn(0, row.size - 1)) { ' ' }
}

/**
 * Clamps [keyCursorCol] to fit within the row that [keyCursorRow] points to.
 * Called whenever keyCursorRow changes so the column stays valid.
 */
fun QwertyKeyboardState.withClampedCol(): QwertyKeyboardState {
    val clamped = keyCursorCol.coerceIn(0, currentRowCols() - 1)
    return if (clamped == keyCursorCol) this else copy(keyCursorCol = clamped)
}

/**
 * Move the keyboard cursor up. Wraps from row 0 to the action row (last navigable row).
 */
fun QwertyKeyboardState.moveCursorUp(): QwertyKeyboardState {
    val newRow = if (keyCursorRow == 0) totalRows() - 1 else keyCursorRow - 1
    return copy(keyCursorRow = newRow).withClampedCol()
}

/**
 * Move the keyboard cursor down. Wraps from the action row back to row 0.
 */
fun QwertyKeyboardState.moveCursorDown(): QwertyKeyboardState {
    val newRow = (keyCursorRow + 1) % totalRows()
    return copy(keyCursorRow = newRow).withClampedCol()
}

/**
 * Move the keyboard cursor left. Wraps from the leftmost key to the rightmost key of the same row.
 */
fun QwertyKeyboardState.moveCursorLeft(): QwertyKeyboardState {
    val rowSize = currentRowCols()
    val newCol = if (keyCursorCol == 0) rowSize - 1 else keyCursorCol - 1
    return copy(keyCursorCol = newCol)
}

/**
 * Move the keyboard cursor right. Wraps from the rightmost key to the leftmost key of the same row.
 */
fun QwertyKeyboardState.moveCursorRight(): QwertyKeyboardState {
    val rowSize = currentRowCols()
    val newCol = (keyCursorCol + 1) % rowSize
    return copy(keyCursorCol = newCol)
}

/**
 * Insert the current key character into [text] at the insertion point.
 *
 * If [insertBefore] is true: character is inserted at [textCursor] (text shifts right),
 * text cursor advances by 1.
 * If [insertBefore] is false: character is inserted at [textCursor]+1 (after current position),
 * text cursor advances by 1.
 */
fun QwertyKeyboardState.insertCurrentKey(): QwertyKeyboardState {
    if (isOnActionRow()) return this   // ABORT/APPLY buttons don't type a character
    if (text.length >= maxLength) return this
    val ch = currentKey()
    val insertAt = if (insertBefore) textCursor else (textCursor + 1).coerceAtMost(text.length)
    val newText = text.substring(0, insertAt) + ch + text.substring(insertAt)
    return copy(text = newText, textCursor = insertAt + 1)
}

/**
 * Delete a character from [text].
 *
 * If [clearOnFirstB] is true: clears all text and resets the flag.
 * If [insertBefore] is true: deletes the char before [textCursor] (backspace),
 * text cursor decreases by 1.
 * If [insertBefore] is false: deletes the char at [textCursor] (forward-delete),
 * text cursor stays (or clamps to new length).
 */
fun QwertyKeyboardState.deleteChar(): QwertyKeyboardState {
    if (clearOnFirstB) {
        return copy(text = "", textCursor = 0, clearOnFirstB = false)
    }
    return if (insertBefore) {
        // Backspace: remove char before cursor
        if (textCursor <= 0 || text.isEmpty()) return this
        val newText = text.removeRange(textCursor - 1, textCursor)
        copy(text = newText, textCursor = textCursor - 1)
    } else {
        // Forward delete: remove char at cursor
        if (textCursor >= text.length || text.isEmpty()) return this
        val newText = text.removeRange(textCursor, textCursor + 1)
        copy(text = newText, textCursor = textCursor.coerceAtMost(newText.length))
    }
}

/**
 * Move the text-insertion cursor one position to the left.
 */
fun QwertyKeyboardState.moveTextCursorLeft(): QwertyKeyboardState {
    if (textCursor <= 0) return this
    return copy(textCursor = textCursor - 1)
}

/**
 * Move the text-insertion cursor one position to the right.
 */
fun QwertyKeyboardState.moveTextCursorRight(): QwertyKeyboardState {
    if (textCursor >= text.length) return this
    return copy(textCursor = textCursor + 1)
}
