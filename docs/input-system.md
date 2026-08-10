# Input System

PocketTracker uses a hybrid input system combining M8's editing precision with LGPT's dual-modifier approach. The generic input handler ensures consistent behavior across all screens.

**Last Updated:** 2026-08-05

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

### Changing any of it — `config.json`

Every binding above is a default, and all of them can be changed by hand in `config.json` in your
PocketTracker folder. The app writes a starter copy on first launch and then never touches the file
again, so what you edit stays edited. See **[Configuration file](#configuration-file)** below.

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
| NOTE | C-0 to G-9 | 1 semitone | 12 semitones | No (clamps) |
| HEX_NIBBLE | 0-F | 1 | - | Yes |

---

## R + Direction (Screen Navigation)

Hold R and press directions to navigate the 5x5 screen grid:

```
Row 0:         -      SCALE   INST_POOL  (INST)*
Row 1:     PROJECT   GROOVE     MODS     PROJECT
Row 2:      SONG     CHAIN    PHRASE   INSTRUMENT  TABLE
Row 3:     MIXER     MIXER    MIXER      MIXER     MIXER
Row 4:    EFFECTS   EFFECTS  EFFECTS    EFFECTS   EFFECTS
```

- Main screens (SONG/CHAIN/PHRASE/INSTRUMENT/TABLE) are on Row 2
- PROJECT, MIXER, EFFECTS span multiple columns
- `(INST)*` is a contextual fast-jump cell shown only while on INST_POOL (see below)

### Instrument Pool fast-jump (INST_POOL ↔ INSTRUMENT)

The Instrument Pool (row 0 / col 3) pairs with a contextual INSTRUMENT cell to its right for quickly
bouncing between the pool and the instrument view:

- **From INST_POOL:** R+RIGHT → INSTRUMENT, R+LEFT → PHRASE, R+DOWN → MODS.
- **From the instrument reached that way:** R+LEFT → back to INST_POOL, R+DOWN → MODS, R+UP/R+RIGHT stay.
- The normal (row-2) INSTRUMENT is unchanged: R+LEFT → PHRASE, R+RIGHT → TABLE, R+UP → MODS, R+DOWN → MIXER.

### Instrument Pool screen controls

A list of all 128 instrument slots with a short mixer strip per slot: `## NAME V RV DE EQ`. The
selected row IS the project's current instrument (shared with the INSTRUMENT view).

- **UP / DOWN** — move the selection (wraps 00↔7F); **B+UP / B+DOWN** — fast-scroll ±16 (clamps at ends).
- **LEFT / RIGHT** — move between columns (NAME → V → RV → DE → EQ).
- **A + DPAD** — edit the value under the cursor (V/RV/DE = 00–FF, EQ = 00–7F).
- **A** on the NAME column of an **empty** slot — load a source (sampler slots browse .wav, SoundFont
  slots browse .sf2/.sf3); the slot is auto-named from the file.
- **A + B** on the NAME column — clear the slot (keeps its instrument type).
- **A** (tap) or **SELECT** on the EQ column — open the per-instrument EQ editor (A+DPAD still picks
  the slot; the open is deferred to A-release so the two don't clash). Inside the editor, **B** closes it.
- **START** — preview the selected instrument (when stopped).

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

**Screens supported:** PHRASE, CHAIN, SONG, TABLE

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
L + B + A               Clone current item (deep clone)
L + LEFT/RIGHT          Navigate to prev/next chain or phrase
L + UP/DOWN             Jump to next/prev populated row
```

### Tier 3: R Modifier (Navigation)

```
R + UP/DOWN/LEFT/RIGHT  Navigate between screens
```

### Planned (placeholders in code, not yet wired)

These combinations exist as TODO stubs in `ButtonHandlers.kt` but are currently no-ops. They're listed
here so they aren't mistaken for working controls:

```
L + START               Play all tracks from beginning      (planned)
R + START               Play from current cursor position   (planned)
R + B                   Reset value to default              (planned)
L + R + SELECT          Return to project/file screen       (planned)
L + R + A               Create snapshot                     (planned)
L + R + B               Recall snapshot                     (planned)
```

---

## Configuration file

`config.json` sits in your PocketTracker folder, beside the `Projects` and `Samples` directories. It is
the opposite of `settings.json`: that one is written by the app from the SETTINGS screen, while
**`config.json` is yours** — the app reads it at startup and never rewrites it.

The app writes a starter copy listing every option at its current value — on first launch, or on
Android on the first launch after you grant it a folder — so the file is a working example rather
than something you have to compose from scratch. Editing it takes effect
on the next launch. Every key is optional; delete a line to go back to the built-in default. A file
that is missing, empty or invalid costs you nothing — the defaults simply stand.

### `controller` — face-button layout

```json
"controller": { "abxy": "auto" }
```

| Value | Meaning |
|---|---|
| `auto` | **Default.** Trust the controller's own report. |
| `nintendo` | The button printed **A** is the **right** one in the ABXY cluster. |
| `xbox` | The button printed **A** is the **bottom** one. |

**When you need this.** SDL normally reports face buttons *by label*, so a built-in handheld pad or a
real Switch Pro controller is already correct on `auto` — there is nothing to configure and never was.

The exception is a pad that misreports what it is. Many third-party controllers — **8BitDo pads in
XInput mode are the common case** — enumerate as an Xbox 360 controller, so the labels the app is told
about are Xbox's positional ones while the plastic under your thumb says something else. The symptom
is unmistakable: **A and B are swapped**, and so are X and Y. Set `"abxy": "nintendo"` and both pairs
move together.

No amount of probing can detect this, because the pad is answering the question wrongly — that is why
it is a setting and not automatic.

> **On a handheld:** PortMaster has its own x360/nintendo option that does the same job one layer
> lower. Use one or the other. Setting **both** applies the swap twice and puts you back where you
> started.

### `keyboard` — key bindings

```json
"keyboard": {
  "A":     ["K", "Return"],
  "B":     ["J", "Escape"],
  "DPAD_UP":    ["W", "Up"],
  "DPAD_DOWN":  ["S", "Down"],
  "DPAD_LEFT":  ["A", "Left"],
  "DPAD_RIGHT": ["D", "Right"],
  "L":      ["U"],
  "R":      ["I"],
  "SELECT": ["Left Shift"],
  "START":  ["Space"]
}
```

Button names are exactly the ten above. Each maps to a list of keys, and a key list can be any length.

**A button listed here replaces its defaults; a button left out keeps them.** That matters when you
want a key that is already taken: binding `K` to something else only works if you also rebind `A`,
because otherwise `K` is still nailed to it. Listing a button as `[]` unbinds it entirely.

Key names are SDL's, and the spelling is not guessable — `"Left Shift"` is spaced but `"PageUp"` is
not. Letters and digits are capitalised (`"K"`, `"Z"`, `"1"`); then `"Left Shift"`, `"Right Ctrl"`,
`"Left Alt"`, `"Return"`, `"Escape"`, `"Space"`, `"Tab"`, `"Backspace"`, `"Insert"`, `"Delete"`, the
arrows `"Up"` / `"Down"` / `"Left"` / `"Right"`, `"Home"` / `"End"` / `"PageUp"` / `"PageDown"`,
`"CapsLock"`, `"F1"`–`"F12"`, and `"Keypad 0"`–`"Keypad 9"` / `"Keypad Enter"`. The full table is in
the manual (§25); the starter file the app writes is the reliable reference.

**A name the app does not recognise is reported in its log and skipped** — that one binding is lost,
nothing else is. If a rebind seems not to have happened, the log says why. (`"Ctrl"` is a common
miss; it is `"Left Ctrl"` or `"Right Ctrl"`.)

Binding one key to two different buttons is not rejected, but only one of them will fire. Don't.

**Scope:** the keyboard section is read on every desktop and handheld build. It is naturally inert on
a device with no keyboard. On Android the hardware **Back** key stays wired to **B** regardless of
what this section says, so a config file edited on a desktop cannot strand you on a phone.

### `folders` — where a load browse starts

```json
"folders": {
  "samples":     "/path/to/your/samples",
  "soundfonts":  "...",
  "instruments": "...",
  "projects":    "...",
  "themes":      "..."
}
```

Sets the folder each **load** browse opens at, so samples kept outside the PocketTracker folder don't
mean climbing out of it every time. A path that doesn't exist is ignored and that category falls back
to its default — a typo costs you one convenience, never a broken browser.

This does **not** change where anything is saved. Renders, exports and sample-editor saves keep their
own folders.

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
