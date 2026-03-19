# Input System

PocketTracker uses a hybrid input system combining M8's editing precision with LGPT's dual-modifier approach. The generic input handler ensures consistent behavior across all screens.

**Last Updated:** 2026-03-19

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

Both keyboard and gamepad work simultaneously.

---

## Design Philosophy

**Modifier roles:**
- **A button** = "Edit this value" (hold for increment/decrement)
- **L button** = "Edit context" (clipboard, selection, item navigation)
- **R button** = "Navigate screens" (screen grid, clone, playback modes)

This creates a consistent, learnable pattern where:
- You don't memorize different controls per screen
- The same value type behaves the same everywhere
- Modifiers have clear, distinct purposes

**Heritage:** M8-style editing precision + LGPT-style dual-modifier ergonomics.

---

## Basic Controls

### Cursor Navigation
- **D-PAD** - Move cursor up/down/left/right
- **UP at row 0** - Wraps to last row
- **DOWN at last row** - Wraps to row 0

### Basic Actions
- **A button** - Insert value on empty cell
- **B button** - Cancel / Back to previous screen
- **SELECT** - Quick delete (screen-specific behavior)
- **START** - Play/Stop sequencer

### Key Repeat
- Hold D-PAD, A+DPAD, or B+DPAD for continuous input (400ms delay, 100ms interval)

---

## A + Direction (Value Editing)

Hold A and press directions to edit values:

### Small Steps
- **A + UP** - Increment by 1
- **A + DOWN** - Decrement by 1

### Large Steps
- **A + RIGHT** - Increment by 16 (hex) or 12 semitones (notes)
- **A + LEFT** - Decrement by 16 (hex) or 12 semitones (notes)

### Delete
- **A + B** - Delete/clear value at cursor

---

## Value Types

The tracker automatically adjusts behavior based on what you're editing:

| Type | Range | Small Step | Large Step | Wrapping |
|------|-------|-----------|-----------|----------|
| HEX_BYTE | 00-FF | 1 | 16 | Yes (FF+1=00) |
| PHRASE_REF | 00-FF | 1 | 16 | Yes |
| CHAIN_REF | 00-FF | 1 | 16 | Yes |
| VOLUME | 00-FF | 1 | 16 | Yes |
| SEMITONE_OFFSET | 00-FF | 1 semitone | 12 semitones | Yes |
| NOTE | C-0 to B-9 | 1 semitone | 12 semitones | No (clamps) |
| HEX_NIBBLE | 0-F | 1 | - | Yes |

---

## R + Direction (Screen Navigation)

Hold R and press directions to navigate the 5x5 screen grid:

```
Row 0:         -      SCALE   INST_POOL    -
Row 1:     PROJECT   GROOVE     MODS     PROJECT
Row 2:      SONG     CHAIN    PHRASE   INSTRUMENT  TABLE
Row 3:     MIXER     MIXER    MIXER      MIXER     MIXER
Row 4:    EFFECTS   EFFECTS  EFFECTS    EFFECTS   EFFECTS
```

- Main screens (SONG/CHAIN/PHRASE/INSTRUMENT/TABLE) are on Row 2
- PROJECT, MIXER, EFFECTS span multiple columns

---

## L + Direction (Context Navigation)

- **L + LEFT** - Previous chain/phrase/instrument (depending on screen)
- **L + RIGHT** - Next chain/phrase/instrument

---

## Copy/Paste (M8-Style)

| Control | Action |
|---------|--------|
| **L+B** | Enter/cycle selection mode (CELL -> ROW -> SCREEN) |
| **D-PAD (in selection)** | Expand/contract selection |
| **B (in selection)** | Copy + exit |
| **L+A (in selection)** | Cut (copy + delete) + exit |
| **L+A (outside selection)** | Paste at cursor |
| **A+B (in selection)** | Delete (no clipboard) + exit |
| **L alone** | Cancel selection (no copy) |

**Screens supported:** PHRASE, CHAIN, SONG

**Selection increment:** A+DPAD applies to all selected rows in active column.

---

## All Button Combinations

### Tier 1: Basic Actions (No modifiers)

```
A                       Insert value on empty / Enter edit mode
A + UP                  Increment by small step (+1)
A + DOWN                Decrement by small step (-1)
A + RIGHT               Increment by large step (+16 or +12)
A + LEFT                Decrement by large step (-16 or -12)
A + B                   Delete / Clear value
B                       Cancel / Exit / Back
SELECT                  Quick delete (context-aware)
START                   Play / Stop
```

### Tier 2: L Modifier (Edit & Clipboard)

```
L + A                   Paste (outside selection) / Cut (in selection)
L + B                   Enter selection mode / Cycle mode
L + LEFT/RIGHT          Navigate to prev/next chain or phrase
L + START               Play all tracks from beginning
L + UP/DOWN             Jump to next/prev populated row
```

### Tier 3: R Modifier (Navigation & Performance)

```
R + UP/DOWN/LEFT/RIGHT  Navigate between screens
R + A                   Clone current item
R + B                   Reset value to default
R + START               Play from current cursor position
```

### Tier 4: L + R Combinations (Advanced)

```
L + R + SELECT          Return to project/file screen
L + R + A               Create snapshot
L + R + B               Recall snapshot
```

---

## Architecture

### Cursor Context System

Instead of checking which screen we're on, the system checks **what type of data the cursor is on**.

**Key files:**
- `CursorContext.kt` - Data structures for cursor context (value type, capabilities, bounds)
- `InputHandler.kt` - Generic input handling logic
- `ButtonHandlers.kt` - Input mapping and combination detection

Each screen module implements `getCursorContext(state)` which returns the appropriate context for the current cursor position. The generic input handler then uses that context to determine behavior.

### Adding Input to a New Screen

1. Create `getCursorContext(state)` method in your module
2. Return appropriate `CursorContextFactory` for each cursor position
3. Wire up handlers via `handleInput()`
4. All A+direction and A+B combos work automatically

---

## M8 vs LGPT Design Decisions

PocketTracker takes the best of both systems:

| Feature | Source | Rationale |
|---------|--------|-----------|
| A + directions for editing | M8 | More precise control |
| Dual modifiers (L/R) | LGPT | More ergonomic |
| Deep clone | M8 | Powerful unique feature |
| Selection mode cycling | M8 | More flexible |
| L + A for paste | LGPT | Simpler than SHIFT+EDIT |
| R + directions for screen nav | LGPT | Logical separation |
| Jump to populated | LGPT | Great for sparse patterns |

### Sources
- [M8 Tracker Shortcuts](https://gist.github.com/devin-dominguez/587720c9ab71b2d9f3c4bd48d9c812ca)
- [LGPT Reference Manual](http://wiki.littlegptracker.com/doku.php?id=lgpt%3Areference_manual)
