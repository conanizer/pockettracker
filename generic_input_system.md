# 🎮 Generic Input System Design

## Core Idea: Cursor Context System

Instead of checking which screen we're on, we check **what type of data the cursor is on**.

---

## Step 1: Define Cursor Context Types

Create a new file `CursorContext.kt`:

```kotlin
package com.example.pockettracker

/**
 * CURSOR CONTEXT SYSTEM
 * 
 * Defines what type of data the cursor is currently on,
 * which determines how buttons behave.
 */

/**
 * What kind of value is the cursor on?
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
 */
data class CursorCapabilities(
    val canIncrement: Boolean = false,      // A+UP works
    val canDecrement: Boolean = false,      // A+DOWN works
    val canIncrementFast: Boolean = false,  // A+RIGHT works (±16)
    val canDecrementFast: Boolean = false,  // A+LEFT works (±16)
    val canDelete: Boolean = false,         // A+B works
    val canInsert: Boolean = false,         // A on empty works
    val canCreate: Boolean = false,         // A+A works (create new)
    val isEmpty: Boolean = false            // Is current value empty?
)

/**
 * Complete cursor context - what is cursor on and what can we do?
 */
data class CursorContext(
    val valueType: CursorValueType,
    val capabilities: CursorCapabilities,
    val currentValue: Int = 0,              // Current numeric value
    val minValue: Int = 0,                  // Minimum allowed value
    val maxValue: Int = 255,                // Maximum allowed value
    val smallStep: Int = 1,                 // Step for A+UP/DOWN
    val largeStep: Int = 16,                // Step for A+LEFT/RIGHT
    val emptyValue: Int = 0xFF              // Value that means "empty"
)
```

---

## Step 2: Add Context Provider to Each Module

Each module (Phrase, Chain, Song) provides cursor context based on cursor position.

### Example for ChainEditorModule:

```kotlin
/**
 * Get cursor context for current cursor position
 * This tells the input system what actions are available
 */
fun getCursorContext(state: ChainEditorState): CursorContext {
    return when (state.cursorColumn) {
        // Column 0: Step number (read-only)
        0 -> CursorContext(
            valueType = CursorValueType.READ_ONLY,
            capabilities = CursorCapabilities(
                canInsert = true  // Can insert phrase on this row
            )
        )
        
        // Column 1: Phrase reference
        1 -> {
            val phraseRef = state.chain.phraseRefs[state.cursorRow]
            val isEmpty = phraseRef == 0xFF
            
            CursorContext(
                valueType = CursorValueType.PHRASE_REF,
                capabilities = CursorCapabilities(
                    canIncrement = !isEmpty,
                    canDecrement = !isEmpty,
                    canIncrementFast = !isEmpty,
                    canDecrementFast = !isEmpty,
                    canDelete = !isEmpty,
                    canInsert = isEmpty,
                    canCreate = true,  // A+A creates new phrase
                    isEmpty = isEmpty
                ),
                currentValue = phraseRef,
                minValue = 0,
                maxValue = 254,  // 255 is reserved for "empty"
                smallStep = 1,
                largeStep = 16,
                emptyValue = 0xFF
            )
        }
        
        // Column 2: Transpose
        2 -> {
            val phraseRef = state.chain.phraseRefs[state.cursorRow]
            val isEmpty = phraseRef == 0xFF
            val transposeValue = state.chain.transposeValues[state.cursorRow]
            
            CursorContext(
                valueType = CursorValueType.SEMITONE_OFFSET,
                capabilities = CursorCapabilities(
                    canIncrement = !isEmpty,
                    canDecrement = !isEmpty,
                    canIncrementFast = !isEmpty,  // A+RIGHT = +12 (octave)
                    canDecrementFast = !isEmpty,  // A+LEFT = -12 (octave)
                    isEmpty = isEmpty
                ),
                currentValue = transposeValue,
                minValue = 0,
                maxValue = 255,
                smallStep = 1,      // 1 semitone
                largeStep = 12,     // 1 octave
                emptyValue = 0xFF
            )
        }
        
        else -> CursorContext(
            valueType = CursorValueType.NONE,
            capabilities = CursorCapabilities()
        )
    }
}
```

---

## Step 3: Generic Input Handler

Create a new file `InputHandler.kt`:

```kotlin
package com.example.pockettracker

/**
 * GENERIC INPUT HANDLER
 * 
 * Handles button presses based on cursor context
 * instead of checking which screen we're on.
 */

class InputHandler {
    /**
     * Handle A button press (EDIT in M8)
     * 
     * @param context What the cursor is on
     * @param direction 0=normal press, 1=A+UP, -1=A+DOWN, 2=A+RIGHT, -2=A+LEFT
     * @param doubleTap True if this is A+A double tap
     * @return New value to set, or null if no change
     */
    fun handleAButton(
        context: CursorContext,
        direction: Int = 0,
        doubleTap: Boolean = false
    ): InputAction {
        // A+A double tap - Create new item
        if (doubleTap && context.capabilities.canCreate) {
            return InputAction.CREATE_NEW
        }
        
        // A on empty - Insert default value
        if (direction == 0 && context.capabilities.isEmpty && context.capabilities.canInsert) {
            return InputAction.INSERT_DEFAULT
        }
        
        // A+UP - Increment by small step
        if (direction == 1 && context.capabilities.canIncrement) {
            val newValue = (context.currentValue + context.smallStep)
                .coerceIn(context.minValue, context.maxValue)
            return InputAction.SET_VALUE(newValue)
        }
        
        // A+DOWN - Decrement by small step
        if (direction == -1 && context.capabilities.canDecrement) {
            val newValue = (context.currentValue - context.smallStep)
                .coerceIn(context.minValue, context.maxValue)
            return InputAction.SET_VALUE(newValue)
        }
        
        // A+RIGHT - Increment by large step
        if (direction == 2 && context.capabilities.canIncrementFast) {
            val newValue = (context.currentValue + context.largeStep)
                .coerceIn(context.minValue, context.maxValue)
            return InputAction.SET_VALUE(newValue)
        }
        
        // A+LEFT - Decrement by large step
        if (direction == -2 && context.capabilities.canDecrementFast) {
            val newValue = (context.currentValue - context.largeStep)
                .coerceIn(context.minValue, context.maxValue)
            return InputAction.SET_VALUE(newValue)
        }
        
        return InputAction.NONE
    }
    
    /**
     * Handle A+B combination (delete)
     */
    fun handleABCombo(context: CursorContext): InputAction {
        if (context.capabilities.canDelete) {
            return InputAction.DELETE
        }
        return InputAction.NONE
    }
    
    /**
     * Handle B+UP/DOWN (chain/screen navigation)
     */
    fun handleBNavigation(direction: Int): InputAction {
        return when (direction) {
            1 -> InputAction.NAVIGATE_UP
            -1 -> InputAction.NAVIGATE_DOWN
            2 -> InputAction.NAVIGATE_RIGHT
            -2 -> InputAction.NAVIGATE_LEFT
            else -> InputAction.NONE
        }
    }
}

/**
 * Result of input handling
 */
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
}
```

---

## Step 4: Use in MainActivity

Now the button handlers become much simpler:

```kotlin
// Create input handler instance
val inputHandler = remember { InputHandler() }

// Button A handler
onButtonA = {
    // Get context from current screen
    val context = when (currentScreen) {
        ScreenType.CHAIN -> {
            chainEditor.getCursorContext(
                ChainEditorState(
                    project.chains[currentChain],
                    cursorRow,
                    cursorColumn
                )
            )
        }
        ScreenType.PHRASE -> {
            phraseEditor.getCursorContext(
                PhraseEditorState(
                    project.phrases[currentPhrase],
                    cursorRow,
                    cursorColumn,
                    0,
                    false
                )
            )
        }
        // ... other screens
        else -> CursorContext(
            CursorValueType.NONE,
            CursorCapabilities()
        )
    }
    
    // Handle input based on context
    val action = inputHandler.handleAButton(context, direction = 0)
    
    // Apply the action
    when (action) {
        is InputAction.SET_VALUE -> {
            // Update the value at cursor position
            applyValueChange(currentScreen, cursorRow, cursorColumn, action.value)
        }
        is InputAction.INSERT_DEFAULT -> {
            // Insert default value
            insertDefaultValue(currentScreen, cursorRow, cursorColumn)
        }
        is InputAction.CREATE_NEW -> {
            // Create new item (phrase/chain/instrument)
            createNewItem(currentScreen)
        }
        else -> { }
    }
}
```

---

## Benefits of This System:

✅ **No repetition** - Write input logic once, works everywhere
✅ **Consistent behavior** - A+UP always means +1, everywhere
✅ **Easy to extend** - Add new screens without changing input code
✅ **Testable** - Can test input logic independently
✅ **Maintainable** - Change behavior in one place

---

## What Do You Think?

This is a more complex change, but it would make the codebase **much cleaner**. 

### Pros:
- Generic, reusable system
- No copy-paste code between screens
- Easy to add new screens

### Cons:
- More upfront work to set up
- Needs cursor context for every screen/column
- Can't test until you have device

### Alternative Approach:

We could **keep simple buttons for now** (A=increment, B=decrement) and refactor to this system **later** when you have the device and can test combinations properly.

What do you prefer? 🤔

1. Implement generic system now (better long-term)
2. Keep simple buttons, refactor later (faster to test)
3. Hybrid: Generic value changes, specific actions for special cases
