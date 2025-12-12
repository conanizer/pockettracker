package com.example.pockettracker

/**
 * CURSOR CONTEXT SYSTEM
 *
 * Defines what type of data the cursor is currently on,
 * which determines how buttons behave.
 *
 * Instead of checking which screen we're on, we check what type of data
 * the cursor is on. This creates a generic, reusable input system.
 */

/**
 * What kind of value is the cursor on?
 *
 * Each type of value has different behaviors:
 * - HEX_BYTE: 00-FF range, typical for IDs
 * - NOTE: Musical notes with special cycling behavior
 * - SEMITONE_OFFSET: Transpose values centered at 0x80
 * - etc.
 */
enum class CursorValueType {
    // Numeric values that can be increased/decreased
    HEX_BYTE,           // 00-FF (most common: phrases, chains, instruments)
    HEX_NIBBLE,         // 0-F (single hex digit)
    SEMITONE_OFFSET,    // Transpose values (centered at 80)

    // Musical values
    NOTE,               // Musical note (C-4, D#5, etc.)
    VOLUME,             // Volume (00-FF)

    // Reference types
    PHRASE_REF,         // Reference to a phrase (can be empty --)
    CHAIN_REF,          // Reference to a chain (can be empty --)
    INSTRUMENT_REF,     // Reference to an instrument

    // Special
    EMPTY,              // Empty cell (can insert)
    READ_ONLY,          // Can't edit (like step numbers)
    NONE                // No cursor / invalid position
}

/**
 * What actions are available at cursor position?
 *
 * These capabilities determine which button combinations work:
 * - canIncrement: Basic A button press works
 * - canIncrementFast: A+RIGHT for +16
 * - canDelete: A+B combo works
 * - canInsert: A on empty cell works
 * - etc.
 */
data class CursorCapabilities(
    val canIncrement: Boolean = false,      // A button increases value
    val canDecrement: Boolean = false,      // B button decreases value
    val canIncrementFast: Boolean = false,  // A+RIGHT increases by large step
    val canDecrementFast: Boolean = false,  // A+LEFT decreases by large step
    val canDelete: Boolean = false,         // A+B deletes/clears value
    val canInsert: Boolean = false,         // A on empty inserts default
    val canCreate: Boolean = false,         // A+A creates new item
    val isEmpty: Boolean = false            // Is current value empty?
)

/**
 * Complete cursor context - what is cursor on and what can we do?
 *
 * This is the core data structure that each module returns.
 * The InputHandler uses this to determine how to handle button presses.
 *
 * @param valueType What kind of value this is
 * @param capabilities What actions are possible
 * @param currentValue Current numeric value at cursor
 * @param minValue Minimum allowed value
 * @param maxValue Maximum allowed value
 * @param smallStep Step size for normal increment/decrement (A/B buttons)
 * @param largeStep Step size for fast increment/decrement (A+RIGHT/LEFT)
 * @param emptyValue What value represents "empty" (usually 0xFF)
 */
data class CursorContext(
    val valueType: CursorValueType,
    val capabilities: CursorCapabilities,
    val currentValue: Int = 0,              // Current numeric value
    val minValue: Int = 0,                  // Minimum allowed value
    val maxValue: Int = 255,                // Maximum allowed value
    val smallStep: Int = 1,                 // Step for normal A/B
    val largeStep: Int = 16,                // Step for A+LEFT/RIGHT
    val emptyValue: Int = 0xFF              // Value that means "empty"
) {
    /**
     * Helper: Is the current value empty?
     */
    fun isEmpty(): Boolean = currentValue == emptyValue

    /**
     * Helper: Can we do anything with this cursor position?
     */
    fun isEditable(): Boolean = valueType != CursorValueType.READ_ONLY &&
                                valueType != CursorValueType.NONE
}

/**
 * Convenience functions to create common cursor contexts
 */
object CursorContextFactory {

    /**
     * Read-only context (like step numbers)
     */
    fun readOnly() = CursorContext(
        valueType = CursorValueType.READ_ONLY,
        capabilities = CursorCapabilities()
    )

    /**
     * None/invalid context
     */
    fun none() = CursorContext(
        valueType = CursorValueType.NONE,
        capabilities = CursorCapabilities()
    )

    /**
     * Phrase reference (00-FE, FF = empty)
     */
    fun phraseRef(currentValue: Int, canCreate: Boolean = true) = CursorContext(
        valueType = CursorValueType.PHRASE_REF,
        capabilities = CursorCapabilities(
            canIncrement = currentValue != 0xFF,
            canDecrement = currentValue != 0xFF,
            canIncrementFast = currentValue != 0xFF,
            canDecrementFast = currentValue != 0xFF,
            canDelete = currentValue != 0xFF,
            canInsert = currentValue == 0xFF,
            canCreate = canCreate,
            isEmpty = currentValue == 0xFF
        ),
        currentValue = currentValue,
        minValue = 0,
        maxValue = 254,  // 255 is reserved for "empty"
        smallStep = 1,
        largeStep = 16,
        emptyValue = 0xFF
    )

    /**
     * Chain reference (00-FF, -1 = empty)
     */
    fun chainRef(currentValue: Int, canCreate: Boolean = true) = CursorContext(
        valueType = CursorValueType.CHAIN_REF,
        capabilities = CursorCapabilities(
            canIncrement = currentValue != -1,
            canDecrement = currentValue != -1,
            canIncrementFast = currentValue != -1,
            canDecrementFast = currentValue != -1,
            canDelete = currentValue != -1,
            canInsert = currentValue == -1,
            canCreate = canCreate,
            isEmpty = currentValue == -1
        ),
        currentValue = if (currentValue == -1) 0 else currentValue,
        minValue = 0,
        maxValue = 255,
        smallStep = 1,
        largeStep = 16,
        emptyValue = -1
    )

    /**
     * Transpose value (00-FF, centered at 0x80)
     * Small step: 1 semitone
     * Large step: 12 semitones (1 octave)
     */
    fun transpose(currentValue: Int, isEmpty: Boolean = false) = CursorContext(
        valueType = CursorValueType.SEMITONE_OFFSET,
        capabilities = CursorCapabilities(
            canIncrement = !isEmpty,
            canDecrement = !isEmpty,
            canIncrementFast = !isEmpty,
            canDecrementFast = !isEmpty,
            isEmpty = isEmpty
        ),
        currentValue = currentValue,
        minValue = 0,
        maxValue = 255,
        smallStep = 1,      // 1 semitone
        largeStep = 12,     // 1 octave
        emptyValue = 0x80   // Center value (no transpose)
    )

    /**
     * Musical note
     */
    fun note(currentValue: Int, isEmpty: Boolean = false) = CursorContext(
        valueType = CursorValueType.NOTE,
        capabilities = CursorCapabilities(
            canIncrement = !isEmpty,
            canDecrement = !isEmpty,
            canIncrementFast = !isEmpty,  // +12 = octave up
            canDecrementFast = !isEmpty,  // -12 = octave down
            canDelete = !isEmpty,
            isEmpty = isEmpty
        ),
        currentValue = currentValue,
        minValue = 0,
        maxValue = 119,     // C-0 to B-9
        smallStep = 1,      // 1 semitone
        largeStep = 12,     // 1 octave
        emptyValue = -1
    )

    /**
     * Volume (00-FF)
     */
    fun volume(currentValue: Int) = CursorContext(
        valueType = CursorValueType.VOLUME,
        capabilities = CursorCapabilities(
            canIncrement = true,
            canDecrement = true,
            canIncrementFast = true,
            canDecrementFast = true
        ),
        currentValue = currentValue,
        minValue = 0,
        maxValue = 255,
        smallStep = 16,     // Jump by 0x10
        largeStep = 64,     // Jump by 0x40
        emptyValue = -1
    )

    /**
     * Instrument reference (0-3 for now)
     */
    fun instrument(currentValue: Int) = CursorContext(
        valueType = CursorValueType.INSTRUMENT_REF,
        capabilities = CursorCapabilities(
            canIncrement = true,
            canDecrement = true
        ),
        currentValue = currentValue,
        minValue = 0,
        maxValue = 3,       // 4 instruments for now
        smallStep = 1,
        largeStep = 1,      // No fast increment for small range
        emptyValue = -1
    )

    /**
     * Generic hex byte (00-FF)
     */
    fun hexByte(currentValue: Int, min: Int = 0, max: Int = 255) = CursorContext(
        valueType = CursorValueType.HEX_BYTE,
        capabilities = CursorCapabilities(
            canIncrement = true,
            canDecrement = true,
            canIncrementFast = true,
            canDecrementFast = true
        ),
        currentValue = currentValue,
        minValue = min,
        maxValue = max,
        smallStep = 1,
        largeStep = 16,
        emptyValue = -1
    )
}
