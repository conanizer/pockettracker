package com.conanizer.pockettracker.core.logic

import com.conanizer.pockettracker.CursorContext
import com.conanizer.pockettracker.CursorValueType
import com.conanizer.pockettracker.core.logging.ILogger

/**
 * INPUT CONTROLLER
 *
 * Handles all user input based on cursor context.
 * Uses a unified input system that works the same way across all screens.
 *
 * ✅ PLATFORM-AGNOSTIC - No Android dependencies!
 *
 * Philosophy:
 * - A button = increase/decrease/edit value
 * - A+B = delete/clear
 * - A+A = create new item
 * - L+B = enter/cycle selection mode (CELL → ROW → SCREEN)
 * - B (in selection) = copy
 * - L+A (in selection) = cut
 * - L+A (outside selection) = paste
 *
 * The specific behavior depends on the CursorContext provided by each module.
 *
 * Migrated from GenericInputHandler during Phase 4 refactoring.
 */
class InputController(
    private val logger: ILogger,
    private val stateObserver: StateObserver
) {
    private val TAG = "InputController"

    companion object {
        /**
         * Allowed characters for text editing (project names, etc.)
         * Order: A-Z, 0-9, underscore, dash, space
         */
        private val ALLOWED_CHARS = ('A'..'Z') + ('0'..'9') + '_' + '-' + ' '

        /**
         * Multi-tap detection window in milliseconds.
         * Taps within this window cycle through selection modes.
         */
        private const val MULTI_TAP_WINDOW = 500L
    }

    // ========================================
    // SELECTION MODE STATE
    // ========================================

    /**
     * Selection scope states for LGPT-style selection.
     */
    enum class SelectionScope {
        NONE,       // Not in selection mode
        CELL,       // Single cell selected (just cursor position)
        ROW,        // Entire row selected
        SCREEN      // All 16 rows selected
    }

    /**
     * Selection bounds for multi-cell selection.
     */
    data class SelectionBounds(
        val topLeftRow: Int,
        val topLeftColumn: Int,
        val bottomRightRow: Int,
        val bottomRightColumn: Int
    ) {
        val width: Int get() = bottomRightColumn - topLeftColumn + 1
        val height: Int get() = bottomRightRow - topLeftRow + 1

        fun contains(row: Int, column: Int): Boolean {
            return row in topLeftRow..bottomRightRow &&
                   column in topLeftColumn..bottomRightColumn
        }
    }

    /**
     * Current selection scope.
     */
    var selectionScope = SelectionScope.NONE
        private set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    /**
     * Whether selection mode is active.
     */
    var selectionMode = false
        private set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    /**
     * Selection cursor position (row, column).
     */
    data class CursorPosition(val row: Int, val column: Int)

    /**
     * Selection start position.
     */
    var selectionStart: CursorPosition? = null
        private set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    /**
     * Selection end position.
     */
    var selectionEnd: CursorPosition? = null
        private set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    /**
     * Timestamp of last SELECT+B press for multi-tap detection.
     */
    private var lastSelectBTime: Long = 0

    // ========================================
    // SELECTION MODE OPERATIONS
    // ========================================

    /**
     * Handle SELECT+B combination.
     * Implements LGPT-style multi-tap selection:
     * - 1st tap: CELL mode (cursor position only)
     * - 2nd tap within window: ROW mode (full row)
     * - 3rd tap within window: SCREEN mode (all 16 rows)
     * - 4th tap: wraps back to CELL
     * - Tap after window expires: exit selection mode
     *
     * @param cursorRow Current cursor row
     * @param cursorColumn Current cursor column
     * @param maxColumn Maximum column for current screen (e.g., 9 for phrase, 2 for chain)
     */
    fun handleSelectB(cursorRow: Int, cursorColumn: Int, maxColumn: Int) {
        val now = System.currentTimeMillis()

        if (selectionScope == SelectionScope.NONE) {
            // First tap - enter CELL mode
            selectionScope = SelectionScope.CELL
            initializeSelectionForScope(cursorRow, cursorColumn, maxColumn)
            logger.d(TAG, "📋 Selection: CELL mode at ($cursorRow, $cursorColumn)")
        } else if (now - lastSelectBTime < MULTI_TAP_WINDOW) {
            // Multi-tap within window - cycle through modes
            selectionScope = when (selectionScope) {
                SelectionScope.CELL -> SelectionScope.ROW
                SelectionScope.ROW -> SelectionScope.SCREEN
                SelectionScope.SCREEN -> SelectionScope.CELL
                else -> SelectionScope.CELL
            }
            initializeSelectionForScope(cursorRow, cursorColumn, maxColumn)
            logger.d(TAG, "📋 Selection: ${selectionScope.name} mode")
        } else {
            // Too slow - exit selection mode
            exitSelectionMode()
            logger.d(TAG, "📋 Selection: exited (tap timeout)")
        }

        lastSelectBTime = now
    }

    /**
     * Initialize selection bounds for current scope.
     */
    private fun initializeSelectionForScope(cursorRow: Int, cursorColumn: Int, maxColumn: Int) {
        when (selectionScope) {
            SelectionScope.CELL -> {
                selectionStart = CursorPosition(cursorRow, cursorColumn)
                selectionEnd = CursorPosition(cursorRow, cursorColumn)
            }
            SelectionScope.ROW -> {
                selectionStart = CursorPosition(cursorRow, 1)  // Column 1 is first editable
                selectionEnd = CursorPosition(cursorRow, maxColumn)
            }
            SelectionScope.SCREEN -> {
                selectionStart = CursorPosition(0, 1)
                selectionEnd = CursorPosition(15, maxColumn)
            }
            else -> { }
        }
        selectionMode = true
    }

    /**
     * Expand or contract selection in given direction.
     *
     * @param direction "UP", "DOWN", "LEFT", "RIGHT"
     * @param maxRow Maximum row (15 for phrase/chain)
     * @param maxColumn Maximum column for current screen
     */
    fun expandSelection(direction: String, maxRow: Int, maxColumn: Int) {
        if (!selectionMode || selectionEnd == null) return

        val end = selectionEnd!!
        val newEnd = when (direction) {
            "UP" -> CursorPosition(maxOf(0, end.row - 1), end.column)
            "DOWN" -> CursorPosition(minOf(maxRow, end.row + 1), end.column)
            "LEFT" -> CursorPosition(end.row, maxOf(1, end.column - 1))
            "RIGHT" -> CursorPosition(end.row, minOf(maxColumn, end.column + 1))
            else -> end
        }
        selectionEnd = newEnd
        logger.d(TAG, "📋 Selection expanded: ${selectionStart} to ${selectionEnd}")
    }

    /**
     * Get current selection bounds.
     */
    fun getSelectionBounds(): SelectionBounds? {
        val start = selectionStart ?: return null
        val end = selectionEnd ?: return null

        return SelectionBounds(
            topLeftRow = minOf(start.row, end.row),
            topLeftColumn = minOf(start.column, end.column),
            bottomRightRow = maxOf(start.row, end.row),
            bottomRightColumn = maxOf(start.column, end.column)
        )
    }

    /**
     * Check if a cell is within the current selection.
     */
    fun isCellSelected(row: Int, column: Int): Boolean {
        if (!selectionMode) return false
        return getSelectionBounds()?.contains(row, column) ?: false
    }

    /**
     * Exit selection mode.
     */
    fun exitSelectionMode() {
        selectionMode = false
        selectionScope = SelectionScope.NONE
        selectionStart = null
        selectionEnd = null
        logger.d(TAG, "📋 Exited selection mode")
    }

    /**
     * Check if selection mode is active.
     */
    fun isSelectionModeActive(): Boolean = selectionMode

    /**
     * Get selection info for UI display.
     */
    fun getSelectionInfo(): String {
        if (!selectionMode) return ""

        return when (selectionScope) {
            SelectionScope.CELL -> "SEL:CELL"
            SelectionScope.ROW -> "SEL:ROW"
            SelectionScope.SCREEN -> "SEL:ALL"
            else -> ""
        }
    }

    /**
     * Handle SELECT+A combination.
     * - In selection mode: returns CUT action
     * - Outside selection mode: returns PASTE action
     */
    fun handleSelectA(): InputAction {
        return if (selectionMode) {
            logger.d(TAG, "📋 SELECT+A: Cut (in selection mode)")
            InputAction.CUT
        } else {
            logger.d(TAG, "📋 SELECT+A: Paste (outside selection mode)")
            InputAction.PASTE
        }
    }

    /**
     * Handle B button press in selection mode (COPY).
     * @return true if handled (was in selection mode), false otherwise
     */
    fun handleCopyInSelection(): Boolean {
        if (!selectionMode) return false
        logger.d(TAG, "📋 B pressed in selection: Copy")
        return true  // Signal that copy should happen
    }

    // ========================================
    // BUTTON HANDLERS (based on CursorContext)
    // ========================================

    /**
     * Handle A button press (PRIMARY ACTION - Edit/Increase)
     *
     * @param context What the cursor is on
     * @return Action to perform
     */
    fun handleAButton(context: CursorContext): InputAction {
        // Read-only or invalid position - do nothing
        if (!context.isEditable()) {
            logger.d(TAG, "A button pressed on non-editable position")
            return InputAction.NONE
        }

        // A on empty - Insert default value
        if (context.capabilities.isEmpty && context.capabilities.canInsert) {
            logger.d(TAG, "A button on empty - inserting default value")
            return InputAction.INSERT_DEFAULT
        }

        // A on value - Increment by small step
        if (context.capabilities.canIncrement) {
            val newValue = incrementValue(context.currentValue, context.smallStep, context)
            logger.d(TAG, "A button - increment: ${context.currentValue} → $newValue")
            return InputAction.SET_VALUE(newValue)
        }

        return InputAction.NONE
    }

    /**
     * Handle B button press (SECONDARY ACTION - Decrease)
     *
     * @param context What the cursor is on
     * @return Action to perform
     */
    fun handleBButton(context: CursorContext): InputAction {
        // Read-only or invalid position - do nothing
        if (!context.isEditable()) {
            return InputAction.NONE
        }

        // B on empty - do nothing
        if (context.capabilities.isEmpty) {
            return InputAction.NONE
        }

        // B on value - Decrement by small step
        if (context.capabilities.canDecrement) {
            val newValue = decrementValue(context.currentValue, context.smallStep, context)
            logger.d(TAG, "B button - decrement: ${context.currentValue} → $newValue")
            return InputAction.SET_VALUE(newValue)
        }

        return InputAction.NONE
    }

    /**
     * Handle A+RIGHT combination (FAST INCREMENT)
     *
     * @param context What the cursor is on
     * @return Action to perform
     */
    fun handleARight(context: CursorContext): InputAction {
        if (!context.isEditable() || context.capabilities.isEmpty) {
            return InputAction.NONE
        }

        if (context.capabilities.canIncrementFast) {
            val newValue = incrementValue(context.currentValue, context.largeStep, context)
            logger.d(TAG, "A+RIGHT - fast increment: ${context.currentValue} → $newValue (step=${context.largeStep})")
            return InputAction.SET_VALUE(newValue)
        }

        return InputAction.NONE
    }

    /**
     * Handle A+LEFT combination (FAST DECREMENT)
     *
     * @param context What the cursor is on
     * @return Action to perform
     */
    fun handleALeft(context: CursorContext): InputAction {
        if (!context.isEditable() || context.capabilities.isEmpty) {
            return InputAction.NONE
        }

        if (context.capabilities.canDecrementFast) {
            val newValue = decrementValue(context.currentValue, context.largeStep, context)
            logger.d(TAG, "A+LEFT - fast decrement: ${context.currentValue} → $newValue (step=${context.largeStep})")
            return InputAction.SET_VALUE(newValue)
        }

        return InputAction.NONE
    }

    /**
     * Handle A+B combination (DELETE)
     *
     * @param context What the cursor is on
     * @return Action to perform
     */
    fun handleABCombo(context: CursorContext): InputAction {
        if (context.capabilities.canDelete) {
            logger.d(TAG, "A+B - deleting value at cursor")
            return InputAction.DELETE
        }
        return InputAction.NONE
    }

    /**
     * Handle A+A double-tap (CREATE NEW - Insert next unused item)
     *
     * Creates next unused phrase/chain/instrument in sequence:
     * - PHRASE screen: Find next unused phrase slot (0-255)
     * - CHAIN screen: Find next unused chain slot (0-255)
     * - INSTRUMENT screen: Find next empty instrument slot (0C-FF)
     *
     * ⏳ Implementation: Pending double-tap detection in InputMapper
     *
     * @param context What the cursor is on
     * @return Action to perform
     */
    fun handleAACombo(context: CursorContext): InputAction {
        if (context.capabilities.canCreate) {
            logger.d(TAG, "A+A - creating next unused item")
            return InputAction.CREATE_NEW
        }
        return InputAction.NONE
    }

    /**
     * Handle DPAD UP (Navigate within screen)
     *
     * Moves cursor up within current screen with wrapping.
     * Different behavior per screen (rows vary: PHRASE=16, PROJECT=7, etc.)
     *
     * @return Action to perform (dispatched by MainActivity via applyInputAction)
     */
    fun handleDPadUp(): InputAction {
        logger.d(TAG, "DPAD UP - navigate up within screen")
        return InputAction.NAVIGATE_UP
    }

    /**
     * Handle DPAD DOWN (Navigate within screen)
     *
     * Moves cursor down within current screen with wrapping.
     * Different behavior per screen (rows vary: PHRASE=16, PROJECT=7, etc.)
     *
     * @return Action to perform (dispatched by MainActivity via applyInputAction)
     */
    fun handleDPadDown(): InputAction {
        logger.d(TAG, "DPAD DOWN - navigate down within screen")
        return InputAction.NAVIGATE_DOWN
    }

    /**
     * Handle DPAD LEFT (Navigate within screen or between screens)
     *
     * Behavior depends on current screen:
     * - Most screens: Move cursor left within screen
     * - With R modifier (future): Move to previous screen
     *
     * @return Action to perform (dispatched by MainActivity via applyInputAction)
     */
    fun handleDPadLeft(): InputAction {
        logger.d(TAG, "DPAD LEFT - navigate left within screen or to previous screen")
        return InputAction.NAVIGATE_LEFT
    }

    /**
     * Handle DPAD RIGHT (Navigate within screen or between screens)
     *
     * Behavior depends on current screen:
     * - Most screens: Move cursor right within screen
     * - With R modifier (future): Move to next screen
     *
     * @return Action to perform (dispatched by MainActivity via applyInputAction)
     */
    fun handleDPadRight(): InputAction {
        logger.d(TAG, "DPAD RIGHT - navigate right within screen or to next screen")
        return InputAction.NAVIGATE_RIGHT
    }

    /**
     * Handle SELECT button (context-specific quick action)
     *
     * @param context What the cursor is on
     * @return Action to perform
     */
    fun handleSelect(context: CursorContext): InputAction {
        // For now, SELECT acts as delete on non-empty values
        if (!context.capabilities.isEmpty && context.capabilities.canDelete) {
            logger.d(TAG, "SELECT - deleting value")
            return InputAction.DELETE
        }
        return InputAction.NONE
    }

    // ========================================
    // HELPER FUNCTIONS
    // ========================================

    /**
     * Increment a value with proper wrapping/clamping
     */
    private fun incrementValue(current: Int, step: Int, context: CursorContext): Int {
        return when (context.valueType) {
            // Character: cycle through allowed character set
            CursorValueType.CHARACTER -> {
                val currentChar = current.toChar()
                val currentIndex = ALLOWED_CHARS.indexOf(currentChar)

                // If character not in allowed set, start at beginning
                if (currentIndex == -1) {
                    ALLOWED_CHARS[0].code
                } else {
                    // Cycle to next character with wrap-around
                    val nextIndex = (currentIndex + step) % ALLOWED_CHARS.size
                    ALLOWED_CHARS[nextIndex].code
                }
            }

            // References, hex bytes, volume, effects, and toggles wrap around (00 -> max -> 00)
            CursorValueType.PHRASE_REF,
            CursorValueType.CHAIN_REF,
            CursorValueType.HEX_BYTE,
            CursorValueType.SEMITONE_OFFSET,
            CursorValueType.VOLUME,
            CursorValueType.EFFECT_TYPE,
            CursorValueType.EFFECT_VALUE,
            CursorValueType.INSTRUMENT_REF,
            CursorValueType.TOGGLE_BINARY,
            CursorValueType.TOGGLE_TERNARY -> {
                var newVal = current + step
                while (newVal > context.maxValue) {
                    newVal -= (context.maxValue - context.minValue + 1)
                }
                newVal
            }
            // Most values clamp at boundaries
            else -> (current + step).coerceIn(context.minValue, context.maxValue)
        }
    }

    /**
     * Decrement a value with proper wrapping/clamping
     */
    private fun decrementValue(current: Int, step: Int, context: CursorContext): Int {
        return when (context.valueType) {
            // Character: cycle through allowed character set backward
            CursorValueType.CHARACTER -> {
                val currentChar = current.toChar()
                val currentIndex = ALLOWED_CHARS.indexOf(currentChar)

                // If character not in allowed set, start at end
                if (currentIndex == -1) {
                    ALLOWED_CHARS.last().code
                } else {
                    // Cycle to previous character with wrap-around
                    val prevIndex = (currentIndex - step + ALLOWED_CHARS.size) % ALLOWED_CHARS.size
                    ALLOWED_CHARS[prevIndex].code
                }
            }

            // References, hex bytes, volume, effects, and toggles wrap around (00 -> max -> 00)
            CursorValueType.PHRASE_REF,
            CursorValueType.CHAIN_REF,
            CursorValueType.HEX_BYTE,
            CursorValueType.SEMITONE_OFFSET,
            CursorValueType.VOLUME,
            CursorValueType.EFFECT_TYPE,
            CursorValueType.EFFECT_VALUE,
            CursorValueType.INSTRUMENT_REF,
            CursorValueType.TOGGLE_BINARY,
            CursorValueType.TOGGLE_TERNARY -> {
                var newVal = current - step
                while (newVal < context.minValue) {
                    newVal += (context.maxValue - context.minValue + 1)
                }
                newVal
            }
            // Most values clamp at boundaries
            else -> (current - step).coerceIn(context.minValue, context.maxValue)
        }
    }
}

/**
 * Result of input handling
 *
 * Sealed class ensures we handle all possible cases.
 * Each action type carries the data needed to perform the action.
 */
sealed class InputAction {
    /** No action needed */
    object NONE : InputAction()

    /** Set value at cursor to specific number */
    data class SET_VALUE(val value: Int) : InputAction()

    /** Delete/clear value at cursor (set to empty) */
    object DELETE : InputAction()

    /** Insert default value at empty cursor position */
    object INSERT_DEFAULT : InputAction()

    /** Create new item (phrase/chain/instrument) */
    object CREATE_NEW : InputAction()

    /** Navigate to previous chain/phrase/instrument */
    object NAVIGATE_UP : InputAction()

    /** Navigate to next chain/phrase/instrument */
    object NAVIGATE_DOWN : InputAction()

    /** Navigate to previous screen */
    object NAVIGATE_LEFT : InputAction()

    /** Navigate to next screen */
    object NAVIGATE_RIGHT : InputAction()

    /** Copy selection to clipboard */
    object COPY : InputAction()

    /** Cut selection to clipboard (copy + delete) */
    object CUT : InputAction()

    /** Paste clipboard at cursor */
    object PASTE : InputAction()
}
