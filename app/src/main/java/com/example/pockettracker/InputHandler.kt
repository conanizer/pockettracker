package com.example.pockettracker

import android.util.Log

/**
 * GENERIC INPUT HANDLER
 *
 * Handles button presses based on cursor context instead of checking which screen we're on.
 * This creates a unified input system that works the same way across all screens.
 *
 * Philosophy:
 * - A button = increase/edit value
 * - B button = decrease value
 * - A+B = delete/clear
 * - A+A = create new item
 *
 * The specific behavior depends on the CursorContext provided by each module.
 */
class GenericInputHandler {

    private val TAG = "GenericInputHandler"

    companion object {
        /**
         * Allowed characters for text editing (project names, etc.)
         * Order: A-Z, 0-9, underscore, dash, space
         */
        private val ALLOWED_CHARS = ('A'..'Z') + ('0'..'9') + '_' + '-' + ' '
    }

    /**
     * Handle A button press (PRIMARY ACTION - Edit/Increase)
     *
     * @param context What the cursor is on
     * @return Action to perform, or null if no change
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
     * @return Action to perform, or null if no change
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

    // =========================================================================
    // HELPER FUNCTIONS
    // =========================================================================

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

            // References, hex bytes, and volume wrap around (00 -> max -> 00)
            CursorValueType.PHRASE_REF,
            CursorValueType.CHAIN_REF,
            CursorValueType.HEX_BYTE,
            CursorValueType.SEMITONE_OFFSET,
            CursorValueType.VOLUME -> {
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

            // References, hex bytes, and volume wrap around (00 -> max -> 00)
            CursorValueType.PHRASE_REF,
            CursorValueType.CHAIN_REF,
            CursorValueType.HEX_BYTE,
            CursorValueType.SEMITONE_OFFSET,
            CursorValueType.VOLUME -> {
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
