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

    // Text editing
    CHARACTER,          // Character from allowed set (A-Z, 0-9, _, -)

    // Toggles
    TOGGLE_BINARY,      // Binary toggle (off/on)
    TOGGLE_TERNARY,     // Ternary toggle (3-state: off/fwd/png, etc.)

    // Effects (Milestone 2)
    EFFECT_TYPE,        // Effect type (---, ARP, KIL, OFF, RPT, VOL)
    EFFECT_VALUE,       // Effect value (00-FF)

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
    val emptyValue: Int = 0xFF,             // Value that means "empty"
    val fxSlot: Int = 0                     // For effects: which FX slot (1, 2, or 3)
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

    // ============================================================================
    // HEX BYTE BASED VALUES (00-FF range)
    // ============================================================================
    // These all use hexByte() internally with different empty values:
    // - phraseRef uses 0xFF (255) as empty because Chain stores phrase refs as UByte
    // - chainRef uses -1 as empty because Song stores chain refs as nullable Int
    // - volume has no empty value (always valid)
    // ============================================================================

    /**
     * Phrase reference (00-FF, 0xFF = empty)
     */
    fun phraseRef(currentValue: Int, canCreate: Boolean = true) =
        hexByte(currentValue, emptyValue = 0xFF, canDelete = true, canInsert = true, canCreate = canCreate)
            .copy(valueType = CursorValueType.PHRASE_REF)

    /**
     * Chain reference (00-FF, -1 = empty)
     */
    fun chainRef(currentValue: Int, canCreate: Boolean = true) =
        hexByte(if (currentValue == -1) 0 else currentValue, emptyValue = -1, canDelete = true, canInsert = true, canCreate = canCreate)
            .copy(valueType = CursorValueType.CHAIN_REF)

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
            canInsert = isEmpty,          // A on empty note inserts default (C-4)
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
    fun volume(currentValue: Int) =
        hexByte(currentValue).copy(valueType = CursorValueType.VOLUME)

    /**
     * Instrument reference (00-FF hex byte)
     * Same behavior as volume: A+left/right cycles through full range
     */
    fun instrument(currentValue: Int) =
        hexByte(currentValue).copy(valueType = CursorValueType.INSTRUMENT_REF)

    /**
     * Effect type (cycles through: ---, ARP, KIL, OFF, RPT, VOL)
     * A+UP/DOWN cycles through effect types
     * A+B clears effect (sets to NONE/---)
     * @param currentType Effect type code (0x00=NONE, 0x0A=ARP, etc.)
     * @param fxSlot Which FX slot (1, 2, or 3) - used to identify which effect to modify
     */
    fun effectType(currentType: Int, fxSlot: Int): CursorContext {
        // Effect types list: NONE(0x00), Arpeggio(0x0A), Kill(0x0B), Offset(0x0F), Repeat(0x12), Volume(0x16)
        val effectTypes = listOf(0x00, 0x0A, 0x0B, 0x0F, 0x12, 0x16)
        val currentIndex = effectTypes.indexOf(currentType).takeIf { it >= 0 } ?: 0

        return CursorContext(
            valueType = CursorValueType.EFFECT_TYPE,
            capabilities = CursorCapabilities(
                canIncrement = true,     // Cycle to next effect type
                canDecrement = true,     // Cycle to previous effect type
                canDelete = currentType != 0x00,  // A+B clears effect (but only if not already NONE)
                isEmpty = currentType == 0x00
            ),
            currentValue = currentIndex,  // Store as index (0-5), not effect code
            minValue = 0,
            maxValue = 5,  // 6 effect types total (including NONE)
            smallStep = 1,
            fxSlot = fxSlot  // Store which FX slot this is for
        )
    }

    /**
     * Effect value (00-FF hex byte)
     * A+LEFT/RIGHT changes value
     * @param currentValue Effect value (0-255)
     * @param fxSlot Which FX slot (1, 2, or 3)
     */
    fun effectValue(currentValue: Int, fxSlot: Int) = CursorContext(
        valueType = CursorValueType.EFFECT_VALUE,
        capabilities = CursorCapabilities(
            canIncrement = true,
            canDecrement = true,
            canIncrementFast = true,  // +16
            canDecrementFast = true,  // -16
            isEmpty = false
        ),
        currentValue = currentValue,
        minValue = 0,
        maxValue = 255,
        smallStep = 1,
        largeStep = 16,
        fxSlot = fxSlot
    )

    /**
     * Generic hex byte (00-FF)
     * This is the base function that others delegate to
     */
    fun hexByte(
        currentValue: Int,
        min: Int = 0,
        max: Int = 255,
        emptyValue: Int = -1,
        canDelete: Boolean = false,
        canInsert: Boolean = false,
        canCreate: Boolean = false
    ): CursorContext {
        val isEmpty = currentValue == emptyValue
        return CursorContext(
            valueType = CursorValueType.HEX_BYTE,
            capabilities = CursorCapabilities(
                canIncrement = !isEmpty,
                canDecrement = !isEmpty,
                canIncrementFast = !isEmpty,
                canDecrementFast = !isEmpty,
                canDelete = canDelete && !isEmpty,
                canInsert = canInsert && isEmpty,
                canCreate = canCreate,
                isEmpty = isEmpty
            ),
            currentValue = currentValue,
            minValue = min,
            maxValue = max,
            smallStep = 1,
            largeStep = 16,
            emptyValue = emptyValue
        )
    }

    /**
     * Character (A-Z, 0-9, _, -)
     * For project name editing
     *
     * Cycles through allowed character set: A→B→C...→Z→0→1...→9→_→-
     */
    fun character(currentChar: Char) = CursorContext(
        valueType = CursorValueType.CHARACTER,  // Special character type
        capabilities = CursorCapabilities(
            canIncrement = true,
            canDecrement = true,
            canDelete = true  // Can delete character (replace with space)
        ),
        currentValue = currentChar.code,
        minValue = 0,       // Not used for CHARACTER type
        maxValue = 0,       // Not used for CHARACTER type
        smallStep = 1,      // Move to next character in allowed set
        largeStep = 1,      // No fast increment for characters
        emptyValue = '_'.code
    )

    /**
     * Browser line (file selection)
     * For file browser cursor
     */
    fun browserLine(currentLine: Int, totalLines: Int) = CursorContext(
        valueType = CursorValueType.HEX_BYTE,  // Reuse hex byte
        capabilities = CursorCapabilities(
            canIncrement = currentLine < totalLines - 1,
            canDecrement = currentLine > 0
        ),
        currentValue = currentLine,
        minValue = 0,
        maxValue = totalLines - 1,
        smallStep = 1,
        largeStep = 14,  // Page jump (14 visible rows)
        emptyValue = -1
    )

    /**
     * Binary toggle (off/on)
     * Value: 0 = off, 1 = on
     * Changed with A+DPAD (cycles between states)
     */
    fun toggleBinary(currentValue: Boolean) = CursorContext(
        valueType = CursorValueType.TOGGLE_BINARY,
        capabilities = CursorCapabilities(
            canIncrement = true,
            canDecrement = true
        ),
        currentValue = if (currentValue) 1 else 0,
        minValue = 0,
        maxValue = 1,
        smallStep = 1,
        largeStep = 1,
        emptyValue = -1
    )

    /**
     * Ternary toggle (3-state toggle like off/fwd/png)
     * @param currentValue The current value string (e.g., "off", "fwd", "png")
     * @param options List of valid options in order
     * Value: 0 = first option, 1 = second, 2 = third
     * Changed with A+DPAD (cycles through states with wrap)
     */
    fun toggleTernary(currentValue: String, options: List<String>) = CursorContext(
        valueType = CursorValueType.TOGGLE_TERNARY,
        capabilities = CursorCapabilities(
            canIncrement = true,
            canDecrement = true
        ),
        currentValue = options.indexOf(currentValue).coerceAtLeast(0),
        minValue = 0,
        maxValue = options.size - 1,
        smallStep = 1,
        largeStep = 1,
        emptyValue = -1
    )
}
