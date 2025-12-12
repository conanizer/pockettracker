# PocketTracker Input Combinations Guide

## Keyboard Layout

```
WASD - D-pad navigation
K    - A button (primary action)
J    - B button (secondary action)
U    - L modifier (edit/clipboard)
I    - R modifier (navigate/create)
Shift- SELECT button
Space- START button
```

---

## Basic Controls (No Modifiers)

### Navigation
- **W/S** - Move cursor up/down
- **A/D** - Move cursor left/right

### Editing
- **K (A button)** - Insert value / Increment by 1
- **J (B button)** - Cancel / Decrement by 1
- **Left Shift (SELECT)** - Delete/clear value
- **Space (START)** - Play/Stop

### Double-Tap
- **K, K (A, A)** - Insert next unused chain/phrase/instrument

---

## L Modifier (U key) - Edit & Clipboard

Hold **U** and press:

### Clipboard Operations
- **U + K (L + A)** - Paste clipboard contents
- **U + J (L + B)** - Enter selection mode (tap again to cycle: column → row → all)
- **U + Shift (L + SELECT)** - Copy selection (explicit)

### Navigation
- **U + W (L + UP)** - Jump to previous populated row
- **U + S (L + DOWN)** - Jump to next populated row
- **U + A (L + LEFT)** - Navigate to previous chain/phrase
- **U + D (L + RIGHT)** - Navigate to next chain/phrase

### Playback
- **U + Space (L + START)** - Play all tracks from beginning

---

## R Modifier (I key) - Navigate & Create

Hold **I** and press:

### Screen Navigation
- **I + W (R + UP)** - Navigate to screen above
- **I + S (R + DOWN)** - Navigate to screen below
- **I + A (R + LEFT)** - Navigate to screen on left
- **I + D (R + RIGHT)** - Navigate to screen on right

### Item Operations
- **I + K (R + A)** - Clone current item
- **I + K, K (R + A, A)** - Deep clone (with all references)
- **I + J (R + B)** - Reset value to default

### Playback
- **I + Space (R + START)** - Play from current cursor position

---

## L + R Combinations (U + I together)

Hold **both U and I** and press:

- **U + I + Shift (L + R + SELECT)** - Return to project/file screen
- **U + I + K (L + R + A)** - Create snapshot (save parameter state)
- **U + I + J (L + R + B)** - Recall snapshot (load parameter state)

---

## Combination Examples

### Example 1: Clone a chain
1. Navigate to chain you want to clone
2. Hold **I** (R button)
3. Press **K** (A button)
4. Release **I**
5. New cloned chain is created

### Example 2: Jump between populated rows
1. Hold **U** (L button)
2. Press **W** or **S** (UP/DOWN) to jump between rows with data
3. Release **U** when you reach desired row

### Example 3: Navigate screens quickly
1. Hold **I** (R button)
2. Use **W/S/A/D** to navigate the screen grid
3. Release **I** when you reach desired screen

### Example 4: Insert next unused phrase
1. Position cursor on empty chain slot
2. Quickly press **K** twice (A, A)
3. Next unused phrase number is inserted automatically

---

## Current Implementation Status

✅ **Implemented:**
- Modifier state tracking (L/R buttons held)
- Button combination detection
- Double-tap detection (300ms window)
- Logging for all combinations

⏳ **TODO (handlers need to be wired up):**
- L + A for paste
- L + B for selection mode
- R + arrows for screen navigation
- R + A for clone
- A, A for insert next unused
- L + R combinations for snapshots

The infrastructure is ready - we just need to connect the handlers to actual actions!

---

## Testing Combinations

To test if combinations are detected:

1. Run the app with `logInput = true` in InputMapper
2. Open Logcat and filter by `PocketInput`
3. Try holding **U** and pressing **K** - you should see "L+A (paste)" in logs
4. Try holding **I** and pressing **W** - you should see "R+UP (navigate screen up)"
5. Try double-tapping **K** quickly - you should see "A,A (insert next unused)"

---

## Design Philosophy

This system combines:
- **LGPT's dual-modifier approach** (L/R buttons for different purposes)
- **M8's editing precision** (incremental steps, advanced features)
- **Ergonomic keyboard layout** (U/I are comfortable for left hand)

**L button** = "Edit things" (clipboard, selection, fine navigation)
**R button** = "Navigate and create" (screens, clone, playback modes)

This creates a consistent, learnable pattern where modifiers have clear roles.

---

## Next Steps

To wire up the handlers:

1. Add handler methods to `ButtonHandlers` data class
2. Pass these handlers from MainActivity
3. Call appropriate handlers in InputMapper's TODO sections
4. Test each combination in Chain Editor
5. Extend to other screens (Phrase, Song, etc.)

See `m8_vs_lgpt_comparison.md` for the complete design rationale.
