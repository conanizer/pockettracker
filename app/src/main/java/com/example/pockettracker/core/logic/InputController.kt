package com.example.pockettracker.core.logic

import android.util.Log
import androidx.compose.runtime.*
import com.example.pockettracker.CursorContext
import com.example.pockettracker.CursorValueType

/**
 * INPUT CONTROLLER
 *
 * Handles all user input based on cursor context.
 * Uses a unified input system that works the same way across all screens.
 *
 * ✅ PLATFORM-AGNOSTIC - No Android dependencies (except Log which will be abstracted later)
 *
 * Philosophy:
 * - A button = increase/edit value
 * - B button = decrease value
 * - A+B = delete/clear
 * - A+A = create new item
 * - SELECT+B = enter/exit selection mode (for copy/paste)
 * - SELECT+A = paste
 *
 * The specific behavior depends on the CursorContext provided by each module.
 *
 * Migrated from GenericInputHandler during Phase 4 refactoring.
 */
class InputController {
    private val TAG = "InputController"

    companion object {
        /**
         * Allowed characters for text editing (project names, etc.)
         * Order: A-Z, 0-9, underscore, dash, space
         */
        private val ALLOWED_CHARS = ('A'..'Z') + ('0'..'9') + '_' + '-' + ' '
    }

    // ========================================
    // SELECTION STATE (for copy/paste)
    // ========================================

    /**
     * Whether selection mode is active (SELECT+B to enter).
     */
    var selectionMode by mutableStateOf(false)
        private set

    /**
     * Selection start/end positions.
     */
    data class CursorPosition(val row: Int, val column: Int)

    var selectionStart: CursorPosition? by mutableStateOf(null)
        private set

    var selectionEnd: CursorPosition? by mutableStateOf(null)
        private set

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
            Log.d(TAG, "A button pressed on non-editable position")
            return InputAction.NONE
        }

        // A on empty - Insert default value
        if (context.capabilities.isEmpty && context.capabilities.canInsert) {
            Log.d(TAG, "A button on empty - inserting default value")
            return InputAction.INSERT_DEFAULT
        }

        // A on value - Increment by small step
        if (context.capabilities.canIncrement) {
            val newValue = incrementValue(context.currentValue, context.smallStep, context)
            Log.d(TAG, "A button - increment: ${context.currentValue} → $newValue")
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
            Log.d(TAG, "B button - decrement: ${context.currentValue} → $newValue")
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
            Log.d(TAG, "A+RIGHT - fast increment: ${context.currentValue} → $newValue (step=${context.largeStep})")
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
            Log.d(TAG, "A+LEFT - fast decrement: ${context.currentValue} → $newValue (step=${context.largeStep})")
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
            Log.d(TAG, "A+B - deleting value at cursor")
            return InputAction.DELETE
        }
        return InputAction.NONE
    }

    /**
     * Handle A+A double-tap (CREATE NEW)
     *
     * @param context What the cursor is on
     * @return Action to perform
     */
    fun handleAACombo(context: CursorContext): InputAction {
        if (context.capabilities.canCreate) {
            Log.d(TAG, "A+A - creating new item")
            return InputAction.CREATE_NEW
        }
        return InputAction.NONE
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
            Log.d(TAG, "SELECT - deleting value")
            return InputAction.DELETE
        }
        return InputAction.NONE
    }

    // ========================================
    // SELECTION MODE (for copy/paste - Milestone 2.5)
    // ========================================

    /**
     * Handle SELECT+B combination.
     * Enters selection mode OR exits if already in selection mode.
     *
     * TODO: Full implementation in Milestone 2.5
     */
    fun handleSelectB() {
        if (selectionMode) {
            exitSelectionMode()
        } else {
            enterSelectionMode()
        }
    }

    /**
     * Handle SELECT+A combination.
     * Paste clipboard contents at cursor.
     *
     * TODO: Full implementation in Milestone 2.5
     */
    fun handleSelectA(): InputAction {
        Log.d(TAG, "⏳ SELECT+A - paste (stub, implementation in Milestone 2.5)")
        return InputAction.NONE
    }

    /**
     * Enter selection mode.
     */
    private fun enterSelectionMode() {
        selectionMode = true
        Log.d(TAG, "📋 Entered selection mode")
    }

    /**
     * Exit selection mode.
     */
    private fun exitSelectionMode() {
        selectionMode = false
        selectionStart = null
        selectionEnd = null
        Log.d(TAG, "📋 Exited selection mode")
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

        val start = selectionStart ?: return "SELECTING..."
        val end = selectionEnd ?: return "SELECTING..."

        val width = kotlin.math.abs(end.column - start.column) + 1
        val height = kotlin.math.abs(end.row - start.row) + 1

        return "SEL: ${width}x${height}"
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

            // References, hex bytes, volume, and toggles wrap around (00 -> max -> 00)
            CursorValueType.PHRASE_REF,
            CursorValueType.CHAIN_REF,
            CursorValueType.HEX_BYTE,
            CursorValueType.SEMITONE_OFFSET,
            CursorValueType.VOLUME,
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

            // References, hex bytes, volume, and toggles wrap around (00 -> max -> 00)
            CursorValueType.PHRASE_REF,
            CursorValueType.CHAIN_REF,
            CursorValueType.HEX_BYTE,
            CursorValueType.SEMITONE_OFFSET,
            CursorValueType.VOLUME,
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
}
