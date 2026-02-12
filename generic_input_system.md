# 🎮 Generic Input System Design

## Core Idea: Cursor Context System

Instead of checking which screen we're on, we check **what type of data the cursor is on**.

---

## Implementation Status

✅ **Phase 1 (COMPLETE)**: Core system implemented
- CursorContext.kt - Data structures for cursor context
- InputHandler.kt - Generic input handling logic
- CursorContextFactory - Helper functions for common contexts
- Value wrapping for HEX_BYTE, PHRASE_REF, CHAIN_REF, VOLUME, SEMITONE_OFFSET

✅ **Phase 2 (COMPLETE)**: Chain Editor migration
- ChainEditorModule.getCursorContext() - Returns context for cursor position
- MainActivity - Chain screen uses GenericInputHandler
- A+direction buttons work generically for Chain screen

✅ **Phase 3 (COMPLETE)**: Remaining main screens migrated
- ✅ Phrase Editor - Note/volume/instrument editing with A+direction
- ✅ Song Editor - Chain references with A+direction and wrapping
- ✅ Project Editor - Tempo/transpose/name editing with A+direction
- ⏳ Instrument Editor (future)
- ⏳ Table Editor (future)

✅ **Phase 4 (COMPLETE)**: Screen navigation system
- R+direction for 5×5 screen grid navigation
- L+LEFT/RIGHT for chain/phrase/instrument navigation
- Cursor wrapping (row 0 ↔ row 15)

✅ **Phase 5 (COMPLETE)**: Hardware support
- Physical gamepad support for Android handhelds
- Native KEYCODE mapping (DPAD_*, BUTTON_*)
- Dual keyboard/gamepad input working simultaneously

---

## Architecture

### 1. Define Cursor Context Types

`CursorContext.kt` defines:

**CursorValueType** - What kind of data:
- HEX_BYTE - 00-FF numeric values
- NOTE - Musical notes
- SEMITONE_OFFSET - Transpose values
- PHRASE_REF, CHAIN_REF - References to other data
- READ_ONLY, EMPTY, NONE - Special states

**CursorCapabilities** - What actions are available:
- canIncrement / canDecrement - Basic A/B buttons
- canIncrementFast / canDecrementFast - A+LEFT/RIGHT
- canDelete - A+B or SELECT
- canInsert - A on empty cell
- canCreate - A+A to create new item

**CursorContext** - Complete description:
- valueType - What this is
- capabilities - What you can do
- currentValue, minValue, maxValue - Numeric bounds
- smallStep, largeStep - Increment amounts
- emptyValue - What "empty" means

### 2. Each Module Provides Context

Each screen module implements `getCursorContext(state)`:

```kotlin
fun getCursorContext(state: ChainEditorState): CursorContext {
    return when (state.cursorColumn) {
        0 -> CursorContextFactory.readOnly()
        1 -> CursorContextFactory.phraseRef(phraseRef)
        2 -> CursorContextFactory.transpose(transposeValue)
        else -> CursorContextFactory.none()
    }
}
```

### 3. Generic Input Handler

`GenericInputHandler` handles buttons based on context:

```kotlin
val action = genericInputHandler.handleAButton(context)

when (action) {
    is InputAction.SET_VALUE -> updateValue(action.value)
    is InputAction.INSERT_DEFAULT -> insertDefault()
    is InputAction.DELETE -> clearValue()
    else -> { }
}
```

---

## Benefits

✅ **No repetition** - Write input logic once, works everywhere
✅ **Consistent behavior** - A+UP always means +1, everywhere
✅ **Easy to extend** - Add new screens without changing input code
✅ **Testable** - Can test input logic independently
✅ **Maintainable** - Change behavior in one place

---

## Button Mapping

**Current implementation:**
- **A button** - Insert value on empty cell
- **A + UP** - Increment by small step (1)
- **A + DOWN** - Decrement by small step (1)
- **A + RIGHT** - Increment by large step (16 for hex, 12 for notes)
- **A + LEFT** - Decrement by large step (16 for hex, 12 for notes)
- **A + B** - Delete/clear value at cursor
- **B button** - Cancel / Back
- **SELECT** - Quick delete (screen-specific)
- **R + direction** - Navigate 5×5 screen grid
- **L + LEFT/RIGHT** - Navigate between chains/phrases/instruments

**Future additions (infrastructure ready):**
- A+A - Create new item (double-tap detection exists)
- L+A - Paste clipboard contents
- L+B - Selection mode
- L+UP/DOWN - Jump to populated rows
- R+A - Clone item
- L+R combinations - Snapshots

---

## Example: Chain Editor

**Column 0 (Step):** Read-only
- No editing allowed
- Shows row number in hex

**Column 1 (Phrase Ref):**
- Empty (0xFF): A inserts default phrase
- Has value: A+UP/DOWN increments/decrements by 1
- A+LEFT/RIGHT: Fast jump by 16
- A+B deletes (sets to 0xFF)
- Wraps 00-FF (full 256 phrases)

**Column 2 (Transpose):**
- Only editable if phrase exists
- Small step: 1 semitone (A+UP/DOWN)
- Large step: 12 semitones/octave (A+LEFT/RIGHT)
- Range: 0x00-0xFF with wrapping (default 00)

---

## Migration Guide

To migrate a screen to the generic input system:

1. **Add getCursorContext() to the module:**
   ```kotlin
   fun getCursorContext(state: YourState): CursorContext {
       // Return appropriate context for cursor position
   }
   ```

2. **Update button handlers in MainActivity:**
   ```kotlin
   val context = yourModule.getCursorContext(yourState)
   val action = genericInputHandler.handleAButton(context)
   when (action) { /* apply changes */ }
   ```

3. **Test the behavior** with keyboard input

4. **Repeat for B button, SELECT, etc.**

---

## Future Enhancements

- **Custom key bindings** - Let users remap keys
- ✅ **Gamepad support** - COMPLETE! Physical gamepad working on Android handhelds
- **Gesture support** - Touch gestures for tablets
- **Macro system** - Record and replay input sequences
- **Undo/Redo** - Track input history for undo
- **Selection mode** - Copy/paste/interpolate (infrastructure exists, not wired)
- **Clipboard operations** - L+A paste, L+B selection (infrastructure exists, not wired)
