# PocketTracker Input Combinations Guide

**Last Updated:** December 2024
**Current Implementation Status:** Generic input system working on 4 main screens

---

## Control Layout

### Keyboard Mapping
```
D-PAD:    W (up) / S (down) / A (left) / D (right)
          Arrow keys also work

A button: K or Enter
B button: J or Escape
L button: U (left shoulder/modifier)
R button: I (right shoulder/modifier)
SELECT:   Left Shift
START:    Spacebar
```

### Physical Gamepad (Android Handhelds)
```
D-PAD:    Physical D-pad
A/B:      A and B face buttons (X/Y also map to A/B)
L/R:      L1/L2 shoulder buttons (both map to L)
          R1/R2 shoulder buttons (both map to R)
SELECT:   SELECT or MENU button
START:    START button or BACK
```

✅ **Both keyboard and gamepad work simultaneously!**

---

## ✅ Basic Controls (Fully Implemented)

### Cursor Navigation
- **D-PAD** - Move cursor up/down/left/right
- **UP at row 0** - Wraps to last row (15 or 6 depending on screen)
- **DOWN at last row** - Wraps to row 0

### Basic Actions
- **A button (K)** - Insert value on empty cell
- **B button (J)** - Cancel / Back to previous screen
- **SELECT (Shift)** - Quick delete (screen-specific behavior)
- **START (Space)** - Play/Stop sequencer

---

## ✅ A + Direction Combinations (M8-Style Value Editing)

**Hold A (K key) and press directions to edit values:**

### Small Steps
- **A + UP** - Increment by 1 (00→01, FE→FF, FF→00 with wrapping)
- **A + DOWN** - Decrement by 1 (01→00, 00→FF with wrapping)

### Large Steps (Octaves/0x10)
- **A + RIGHT** - Increment by 16 (00→10, F0→00 with wrapping)
- **A + LEFT** - Decrement by 16 (10→00, 00→F0 with wrapping)

**For musical values:**
- **A + RIGHT** - Increment by 12 semitones (1 octave up)
- **A + LEFT** - Decrement by 12 semitones (1 octave down)

### Delete Combo
- **A + B** - Delete/clear value at cursor (sets to empty/default)

---

## ✅ Value Type Behaviors (Generic Input System)

The tracker automatically adjusts behavior based on what you're editing:

### HEX_BYTE (00-FF) - Most Parameters
- **Range:** 00-FF with wrapping
- **Steps:** 1 (small), 16 (large)
- **Wrapping:** FF + 1 = 00, 00 - 1 = FF
- **Examples:** Chain IDs, generic hex values

### PHRASE_REF (00-FF) - Phrase References in Chains
- **Range:** 00-FF with wrapping
- **Steps:** 1 (small), 16 (large)
- **Wrapping:** FE→FF→00 (can access all 256 phrases)
- **Empty:** Use A+B to delete, can't reach FF by increment

### CHAIN_REF (00-FF) - Chain References in Songs
- **Range:** 00-FF with wrapping
- **Steps:** 1 (small), 16 (large)
- **Wrapping:** FF + 1 = 00, 00 - 1 = FF
- **Empty:** -1 (shown as --)

### VOLUME (00-FF) - Volume Parameters
- **Range:** 00-FF with wrapping
- **Steps:** 1 (small), 16 (large)
- **Wrapping:** FF + 1 = 00, 00 - 1 = FF

### SEMITONE_OFFSET (00-FF) - Transpose Values
- **Range:** 00-FF with wrapping
- **Steps:** 1 semitone (small), 12 semitones/octave (large)
- **Default:** 00 (no transpose)
- **Wrapping:** FF + 1 = 00, 00 - 1 = FF

### NOTE (C-0 to B-9) - Musical Notes
- **Range:** C-0 to B-9 (120 notes)
- **Steps:** 1 semitone (small), 12 semitones/octave (large)
- **Clamping:** Stops at C-0 and B-9 (no wrapping)

### HEX_NIBBLE (0-F) - Single Characters
- **Range:** 0-F or custom (e.g., letters in project name)
- **Steps:** 1 per press
- **Example:** Project name character editing

---

## ✅ R + Direction Combinations (Screen Navigation)

**Hold R (I key) and press directions to navigate the 5×5 screen grid:**

### Grid Navigation
- **R + UP** - Navigate to screen above
- **R + DOWN** - Navigate to screen below
- **R + LEFT** - Navigate to screen on left (main row)
- **R + RIGHT** - Navigate to screen on right (main row)

### Screen Grid Layout
```
Row 0:         -      SCALE   INST_POOL    -
Row 1:     PROJECT   GROOVE     MODS     PROJECT
Row 2:      SONG     CHAIN    PHRASE   INSTRUMENT  TABLE
Row 3:     MIXER     MIXER    MIXER      MIXER     MIXER
Row 4:    EFFECTS   EFFECTS  EFFECTS    EFFECTS   EFFECTS
```

**Notes:**
- Main screens (SONG/CHAIN/PHRASE/INSTRUMENT/TABLE) are always on Row 2
- Context screens appear above/below based on current column
- PROJECT, MIXER, EFFECTS span multiple columns

---

## ✅ L + Direction Combinations (Context Navigation)

**Hold L (U key) and press directions to navigate within context:**

### Item Navigation
- **L + LEFT** - Previous chain/phrase/instrument (depending on screen)
- **L + RIGHT** - Next chain/phrase/instrument (depending on screen)

**Examples:**
- On CHAIN screen: L+LEFT/RIGHT switches between chains 00-FF
- On PHRASE screen: L+LEFT/RIGHT switches between phrases 00-FF

---

## 📋 Current Implementation Status

### ✅ Fully Working Screens (with Generic Input)
- **SONG** - Chain references in 8 tracks (A+direction editing works)
- **CHAIN** - 16 phrase slots + transpose (A+direction editing works)
- **PHRASE** - Note/volume/instrument editing (A+direction editing works)
- **PROJECT** - Tempo/transpose/name editing (A+direction editing works)

### ✅ Working Features
- A + direction value editing (all value types)
- A + B delete combo (all screens)
- R + direction screen navigation (5×5 grid)
- L + LEFT/RIGHT context navigation (chains/phrases)
- Cursor wrapping (top ↔ bottom)
- Value wrapping (hex bytes, refs, volume, transpose)
- Physical gamepad support (Android handhelds)
- Dual keyboard/gamepad input

### ⏳ Planned Features (Infrastructure Ready)
- **L + A** - Paste clipboard contents
- **L + B** - Enter selection mode (tap again to cycle modes)
- **L + UP/DOWN** - Jump to next/previous populated row
- **L + START** - Play all tracks from beginning
- **R + A** - Clone current item
- **R + A, A** - Deep clone (with all references)
- **R + B** - Reset value to default
- **R + START** - Play from current cursor position
- **A, A** - Double-tap to insert next unused item
- **L + R + SELECT** - Return to project screen
- **L + R + A** - Create parameter snapshot
- **L + R + B** - Recall parameter snapshot

### ⏳ Screens Not Yet Implemented
- INSTRUMENT, TABLE, GROOVE, SCALE, MODS, INST_POOL, MIXER, EFFECTS

---

## 🎮 Usage Examples

### Example 1: Edit a volume value in Phrase Editor
1. Navigate cursor to volume column (column 2)
2. Hold **A (K key)**
3. Press **UP** repeatedly to increment by 1 (00→01→02...)
4. Or press **RIGHT** to jump +16 (00→10→20...)
5. Press **A + B** to reset to 00

### Example 2: Navigate to Chain Editor from Phrase Editor
1. Hold **R (I key)**
2. Press **LEFT** once
3. Release **R**
4. You're now on CHAIN screen

### Example 3: Switch to next phrase
1. From PHRASE screen
2. Hold **L (U key)**
3. Press **RIGHT**
4. Current phrase increments (00→01→02...)

### Example 4: Cycle through chain references with wrapping
1. On CHAIN screen, cursor on phrase reference showing FF
2. Hold **A (K key)**
3. Press **UP** once
4. Value wraps to 00 (because capabilities prevent increment when empty)
5. Continue pressing **A + UP** to cycle: 00→01→...→FE→FF→00

### Example 5: Use gamepad on handheld
1. Connect Android handheld with physical controls
2. Use D-pad to move cursor
3. Hold **L1** shoulder button
4. Press **RIGHT** on D-pad to go to next phrase
5. All combinations work identically to keyboard!

---

## 🎯 Design Philosophy

**This system combines:**
- **M8's editing precision** - A + directions for incremental steps
- **LGPT's dual-modifier approach** - L/R buttons for different purposes
- **Generic value system** - Same controls work everywhere

**Modifier roles:**
- **A button** = "Edit this value" (hold for increment/decrement)
- **L button** = "Edit context" (clipboard, selection, item navigation)
- **R button** = "Navigate screens" (screen grid, clone, playback modes)

This creates a **consistent, learnable pattern** where:
- You don't memorize different controls per screen
- The same value type behaves the same everywhere
- Modifiers have clear, distinct purposes

---

## 🔍 Testing & Debugging

To verify input is working:

1. Open Android Logcat
2. Filter by tag: `PocketInput`
3. Press buttons and check logs show correct detection
4. Look for: "A+UP (increment by small step)" etc.

**Common issues:**
- If buttons don't respond: Make sure app has focus
- If wrapping doesn't work: Check value type in module's `getCursorContext()`
- If gamepad isn't detected: Check device has physical controls in InputDevice API

---

## 📚 Related Documentation

- **m8_vs_lgpt_comparison.md** - Deep dive into design decisions
- **CLAUDE.md** - Project architecture and data flow
- **CursorContext.kt** - Value type definitions (source of truth)
- **InputHandler.kt** - Generic input logic and wrapping behavior
- **ButtonHandlers.kt** - Input mapping and combination detection

---

## 🚀 Next Steps for Development

To add generic input to a new screen:

1. Create `getCursorContext(state)` method in your module
2. Return appropriate `CursorContextFactory` for each cursor position
3. Wire up `onAUp/Down/Left/Right` handlers in MainActivity
4. Create `applyInputAction()` function to update your data
5. All A+direction and A+B combos work automatically!

No need to duplicate input logic - the generic system handles everything.
