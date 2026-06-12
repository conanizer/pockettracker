package com.conanizer.pockettracker.core.logic

import com.conanizer.pockettracker.input.CursorContext
import com.conanizer.pockettracker.input.CursorValueType
import com.conanizer.pockettracker.core.logging.ILogger

class InputController(
    private val logger: ILogger,
    private val stateObserver: StateObserver
) {
    private val TAG = "InputController"

    companion object {
        private val ALLOWED_CHARS = ('A'..'Z') + ('0'..'9') + '_' + '-' + ' '
        private const val MULTI_TAP_WINDOW = 500L
    }

    enum class SelectionScope {
        NONE, CELL, ROW, SCREEN
    }

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

    var selectionScope = SelectionScope.NONE
        private set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    var selectionMode = false
        private set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    data class CursorPosition(val row: Int, val column: Int)

    var selectionStart: CursorPosition? = null
        private set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    var selectionEnd: CursorPosition? = null
        private set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    private var lastSelectBTime: Long = 0

    fun handleSelectB(cursorRow: Int, cursorColumn: Int, maxColumn: Int) {
        val now = System.currentTimeMillis()

        if (selectionScope == SelectionScope.NONE) {
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

    private fun initializeSelectionForScope(cursorRow: Int, cursorColumn: Int, maxColumn: Int) {
        when (selectionScope) {
            SelectionScope.CELL -> {
                selectionStart = CursorPosition(cursorRow, cursorColumn)
                selectionEnd = CursorPosition(cursorRow, cursorColumn)
            }
            SelectionScope.ROW -> {
                selectionStart = CursorPosition(cursorRow, 1)
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

    fun isCellSelected(row: Int, column: Int): Boolean {
        if (!selectionMode) return false
        return getSelectionBounds()?.contains(row, column) ?: false
    }

    fun exitSelectionMode() {
        selectionMode = false
        selectionScope = SelectionScope.NONE
        selectionStart = null
        selectionEnd = null
        logger.d(TAG, "📋 Exited selection mode")
    }

    fun isSelectionModeActive(): Boolean = selectionMode

    fun getSelectionInfo(): String {
        if (!selectionMode) return ""

        return when (selectionScope) {
            SelectionScope.CELL -> "SEL:CELL"
            SelectionScope.ROW -> "SEL:ROW"
            SelectionScope.SCREEN -> "SEL:ALL"
            else -> ""
        }
    }

    fun handleSelectA(): InputAction {
        return if (selectionMode) {
            logger.d(TAG, "📋 SELECT+A: Cut (in selection mode)")
            InputAction.CUT
        } else {
            logger.d(TAG, "📋 SELECT+A: Paste (outside selection mode)")
            InputAction.PASTE
        }
    }

    fun handleCopyInSelection(): Boolean {
        if (!selectionMode) return false
        logger.d(TAG, "📋 B pressed in selection: Copy")
        return true
    }

    fun handleAButton(context: CursorContext): InputAction {
        if (!context.isEditable()) return InputAction.NONE
        if (context.capabilities.isEmpty && context.capabilities.canInsert) return InputAction.INSERT_DEFAULT
        if (context.capabilities.canIncrement) {
            val newValue = stepValue(context.currentValue, context.smallStep, context)
            return InputAction.SET_VALUE(newValue)
        }
        return InputAction.NONE
    }

    fun handleBButton(context: CursorContext): InputAction {
        if (!context.isEditable()) return InputAction.NONE
        if (context.capabilities.isEmpty) return InputAction.NONE
        if (context.capabilities.canDecrement) {
            val newValue = stepValue(context.currentValue, -context.smallStep, context)
            return InputAction.SET_VALUE(newValue)
        }
        return InputAction.NONE
    }

    fun handleARight(context: CursorContext): InputAction {
        if (!context.isEditable() || context.capabilities.isEmpty) return InputAction.NONE
        if (context.capabilities.canIncrementFast) {
            return InputAction.SET_VALUE(stepValue(context.currentValue, context.largeStep, context))
        }
        return InputAction.NONE
    }

    fun handleALeft(context: CursorContext): InputAction {
        if (!context.isEditable() || context.capabilities.isEmpty) return InputAction.NONE
        if (context.capabilities.canDecrementFast) {
            return InputAction.SET_VALUE(stepValue(context.currentValue, -context.largeStep, context))
        }
        return InputAction.NONE
    }

    fun handleABCombo(context: CursorContext): InputAction {
        return if (context.capabilities.canDelete) InputAction.DELETE else InputAction.NONE
    }

    fun handleAACombo(context: CursorContext): InputAction {
        return if (context.capabilities.canCreate) InputAction.CREATE_NEW else InputAction.NONE
    }

    fun handleDPadUp(): InputAction = InputAction.NAVIGATE_UP
    fun handleDPadDown(): InputAction = InputAction.NAVIGATE_DOWN
    fun handleDPadLeft(): InputAction = InputAction.NAVIGATE_LEFT
    fun handleDPadRight(): InputAction = InputAction.NAVIGATE_RIGHT

    fun handleSelect(context: CursorContext): InputAction {
        if (!context.capabilities.isEmpty && context.capabilities.canDelete) return InputAction.DELETE
        return InputAction.NONE
    }

    // Apply a signed step to the cursor value, honouring the value type's wrap/clamp rules.
    // Positive signedStep increments, negative decrements (replaces the old mirror-image
    // incrementValue/decrementValue pair, which had identical case lists differing only by sign).
    private fun stepValue(current: Int, signedStep: Int, context: CursorContext): Int {
        return when (context.valueType) {
            CursorValueType.CHARACTER -> {
                val size = ALLOWED_CHARS.size
                val currentIndex = ALLOWED_CHARS.indexOf(current.toChar())
                if (currentIndex == -1) {
                    (if (signedStep >= 0) ALLOWED_CHARS.first() else ALLOWED_CHARS.last()).code
                } else {
                    ALLOWED_CHARS[((currentIndex + signedStep) % size + size) % size].code
                }
            }

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
                val range = context.maxValue - context.minValue + 1
                var newVal = current + signedStep
                while (newVal > context.maxValue) newVal -= range
                while (newVal < context.minValue) newVal += range
                newVal
            }
            else -> (current + signedStep).coerceIn(context.minValue, context.maxValue)
        }
    }
}

sealed class InputAction {
    object NONE : InputAction()
    data class SET_VALUE(val value: Int) : InputAction()
    object DELETE : InputAction()
    object INSERT_DEFAULT : InputAction()
    object CREATE_NEW : InputAction()
    object NAVIGATE_UP : InputAction()
    object NAVIGATE_DOWN : InputAction()
    object NAVIGATE_LEFT : InputAction()
    object NAVIGATE_RIGHT : InputAction()
    object COPY : InputAction()
    object CUT : InputAction()
    object PASTE : InputAction()
}
