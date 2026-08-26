# Input System

PocketTracker uses a hybrid input system combining M8's editing precision with LGPT's dual-modifier
approach. One generic input handler serves every screen, so the same kind of value behaves the same
way wherever you meet it.

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

### Physical Gamepad

```
D-PAD:    Physical D-pad
A/B:      A and B face buttons (X/Y also map to A/B)
L/R:      L1 / R1 shoulder buttons
SELECT:   SELECT (SDL's "back" button)
START:    START
```

**L2/R2 and the analog sticks are not mapped.** Everything the app does is reachable from the ten
buttons above, and a trigger or a drifting stick therefore does nothing rather than something
surprising.

On Android the hardware **Back** key is always **B**, whatever else you rebind — B is the app's
universal cancel, so Back closes the file browser, aborts the keyboard and leaves an editor.

Keyboard and gamepad work simultaneously.

### Changing any of it — `config.json`

Every binding above is a default. The face-button layout and all ten key bindings can be changed by
hand in `config.json` in your PocketTracker folder; the app writes a starter copy on first launch and
then never touches the file again. See **[the manual, section 25](manual-en.md#25-configuration-file-configjson)**
for the schema, the key-name spellings and the 8BitDo/XInput case that makes A and B swap.

---

## Design Philosophy

**Modifier roles:**
- **A button** = "Edit this value" (hold for increment/decrement)
- **B button** = "Which item am I looking at" (previous/next phrase, chain, instrument…)
- **L button** = "Clipboard and selection"
- **R button** = "Navigate screens"
- **SELECT** = "Context action" — opens whatever the cursor is sitting on

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
- **B button** - Cancel / back / copy a selection
- **SELECT** - Context action: opens the sub-screen the cursor is on (SAMPLE EDITOR, EQ EDITOR, the
  name keyboard), closes those overlays again, and toggles delay SYNC on the EFFECTS TIME row.
  ⚠️ SELECT does **not** delete — clearing a value is always **A + B**.
- **START** - Play/Stop sequencer

### Key Repeat
- Hold D-PAD, A+DPAD, or B+DPAD for continuous input (400ms delay, 100ms interval)
- The modifiers are re-read as the repeat fires, so pressing A while UP is already repeating turns a
  cursor move into a value edit without letting go.

---

## A + Direction (Value Editing)

Hold A and press directions to edit values:

### Small Steps
- **A + RIGHT** - Increment by 1
- **A + LEFT** - Decrement by 1

### Large Steps
- **A + UP** - Increment by 16 (hex) or 12 semitones (notes)
- **A + DOWN** - Decrement by 16 (hex) or 12 semitones (notes)

### Delete
- **A + B** - Delete the value at the cursor, or reset it to its default where the cell cannot be
  empty (PAN, DRIVE, the sends…)

### Insert
- **A** on an empty cell inserts the **last-edited** chain/phrase.
- **A, A** (a double-tap inside 300 ms) inserts the next **unused** one instead — the fast way to
  start a fresh phrase.

---

## Value Types

The tracker automatically adjusts behavior based on what you're editing:

| Type | Range | Small Step | Large Step | Past the end |
|------|-------|-----------|-----------|----------|
| HEX_BYTE | 00-FF | 1 | 16 | Wraps (FF+1=00) |
| PHRASE_REF | 00-FF | 1 | 16 | Wraps |
| CHAIN_REF | 00-FF | 1 | 16 | Wraps |
| INSTRUMENT_REF | 00-7F | 1 | 16 | Wraps |
| VOLUME | 00-7F | 1 | 16 | Wraps |
| SEMITONE_OFFSET | 00-FF | 1 semitone | 12 semitones | Wraps |
| NOTE | C-0 to G-9 | 1 semitone | 12 semitones | Clamps |
| HEX_NIBBLE | 0-F | 1 | 4 | Clamps |
| EFFECT_TYPE | the effect list | 1 | 1 | Wraps |
| EFFECT_VALUE | 00-FF (per effect) | 1 | 16 | Wraps |
| GAIN (EQ, dB) | −12.0 … +12.0 | 0.1 dB | 1.0 dB | Clamps |
| FREQ (EQ, Hz) | 20 Hz … 20 kHz | 1 | 16 | Clamps |
| CHARACTER | A-Z 0-9 _ - space | 1 | 1 | Wraps |
| TOGGLE | the option list | 1 | 1 | Wraps |

The split is deliberate: discrete values **wrap**, so a four-button device can dial one in by holding
a direction. Continuous physical units — EQ gain in dB, EQ frequency in Hz — **clamp**, because
wrapping +12 dB round to −12 dB would be a trap rather than a convenience. HEX_NIBBLE clamps for the
same reason: holding A+RIGHT on CRUSH stops at F instead of quietly undoing what you were dialling in.

---

## R + Direction (Screen Navigation)

Hold R and press directions to navigate the 5×5 screen grid:

|  | col 0 | col 1 | col 2 | col 3 | col 4 |
|---|---|---|---|---|---|
| **row 0** | | | SCALE | INST.POOL | |
| **row 1** | PROJECT | PROJECT | GROOVE | MODS | PROJECT |
| **row 2** | SONG | CHAIN | PHRASE | INSTRUMENT | TABLE |
| **row 3** | MIXER | MIXER | MIXER | MIXER | MIXER |
| **row 4** | EFFECTS | EFFECTS | EFFECTS | EFFECTS | EFFECTS |

- The main screens (SONG/CHAIN/PHRASE/INSTRUMENT/TABLE) are row 2, and R+LEFT/RIGHT walks along it.
- PROJECT, MIXER and EFFECTS are shared: they sit in every column and have none of their own, so
  leaving one of them is answered relative to the column you entered it from — R+UP goes to the
  screen above it in that column, and R+LEFT/RIGHT to the main-row screen one column to either side.
- Row 0 and row 1 are column-specific — R+UP from PHRASE reaches GROOVE and then SCALE, R+UP from
  INSTRUMENT reaches MODS and then INST.POOL.

**R elsewhere:** in the file browser **R+LEFT** goes up one directory level and **R+UP/DOWN** cycles
the sort mode. In the QWERTY overlay **R+LEFT/RIGHT** scrolls the text cursor, accelerating from 1 to
4 characters per repeat, and **R+UP/DOWN** switches between the letter and number layouts. In the
SAMPLE EDITOR **R+UP/DOWN** zooms the waveform from wherever the cursor is.

The full-screen overlays (SAMPLE EDITOR, EQ EDITOR, THEME EDITOR) own no cell in the grid above, so
R+DPAD never navigates out of one — anything not listed here is swallowed. **B** is the way out, and
on a modified sample it asks first.

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
  slots browse .sf2 and .sf3); the slot is auto-named from the file.
- **A + B** on the NAME column — clear the slot (keeps its instrument type).
- **A** (tap) or **SELECT** on the EQ column — open the per-instrument EQ editor (A+DPAD still picks
  the slot; the open is deferred to A-release so the two don't clash). Inside the editor, **B** closes it.
- **START** — preview the selected instrument (when stopped).

---

## B + Direction (Item Navigation)

Hold B and press directions to change *which* item the screen is showing, without leaving it:

| Control | Action |
|---------|--------|
| **B + LEFT / RIGHT** | Previous / next phrase, chain, instrument, table or groove — whichever the screen edits |
| **B + LEFT / RIGHT** (EQ EDITOR) | Previous / next EQ preset slot |
| **B + UP / DOWN** (SONG) | Page up / down 16 rows |
| **B + UP / DOWN** (INST.POOL) | Jump ±16 slots |

### Under `SETTINGS -> NAV = SONG` (the default)

B + D-pad on CHAIN and PHRASE walks the **arrangement** instead of the pools. The cursor is a song
cell, so the chain and phrase on screen are whichever ones that cell names, and the CHAIN and PHRASE
headers show it (`CHAIN 20  S01 T3`).

| Control | Action |
|---------|--------|
| **B + LEFT / RIGHT** (CHAIN) | Nearest filled song cell left / right in the same song row. Clamps |
| **B + UP / DOWN** (CHAIN) | Nearest filled song cell up / down the same track column, skipping gaps. Clamps |
| **B + LEFT / RIGHT** (PHRASE) | Nearest track left / right whose chain also has a phrase at this chain row. Clamps |
| **B + UP / DOWN** (PHRASE) | Previous / next filled row of the current chain. Never leaves the chain |
| **UP / DOWN** (PHRASE) | Off step 00 / 0F, spills into the previous / next filled chain row |
| **R + RIGHT** (SONG, CHAIN) | Does nothing when the cell under the cursor is empty |

The other screens are unchanged: SONG still pages by 16, and INSTRUMENT, MODS, TABLE, GROOVE and
INST.POOL still walk their pools.

---

## Copy/Paste (M8-Style)

| Control | Action |
|---------|--------|
| **L+B** | Enter selection mode; tap again inside 500 ms to widen it (CELL -> ROW -> SCREEN) |
| **D-PAD (in selection)** | Expand/contract selection |
| **B (in selection)** | Copy + exit |
| **L+A (in selection)** | Cut (copy + delete) + exit |
| **L+A (outside selection)** | Paste at cursor |
| **A+B (in selection)** | Delete (no clipboard) + exit |
| **L+B+A** | Deep-clone the chain/phrase under the cursor into free slots |
| **L+R** | Leave selection mode — the copy buffer survives. Pressed *outside* a selection it clears the buffer instead, which is how you dismiss the clipboard readout in the top strip. |

**Screens supported:** PHRASE, CHAIN, SONG, TABLE

**Selection increment:** A+DPAD applies to all selected rows in active column.

---

## Mute & Solo — a chord that is momentary or latching

| Control | Action |
|---------|--------|
| **R+B** | Toggle mute on the track under the cursor (SONG, MIXER) |
| **R+A** | Toggle solo, same targeting |
| **either, over a selection** | Applies to every track the selection's columns cover |
| **L+R** | Restore full playback |

**Which button comes up first is the gesture.** The chord acts on the press; releasing **R** first
latches it, and releasing **A** or **B** first undoes everything that chord did. One chord is
therefore both a mute you set and a mute you hold through a bar.

The revert restores all eight tracks, not the one under the cursor: a chord over a selection touches
several, and a solo changes what every other track is heard doing.

On the MIXER, column 8 is the master strip and the chord is a no-op there. On every screen other
than SONG and MIXER it stays the consumed no-op it has always been.

---

## File Browser

| Control | Action |
|---------|--------|
| **D-PAD UP/DOWN** | Move through files and folders |
| **A** | Load the file / enter the folder |
| **A** on `..` | Up one directory level |
| **R+LEFT** | Up one directory level |
| **B** | Close the browser |
| **START** | Preview the highlighted audio file |
| **SELECT+A** | Rename the file or folder under the cursor (opens the keyboard) |
| **SELECT+B** | Delete it — arms an `A=YES B=NO` confirm; nothing is deleted on the press itself |
| **SELECT+R** | Create a folder here (opens the keyboard) |
| **L+B** | Start a file selection; tap again inside 500 ms to select all |
| **B (in selection)** | Copy the selected files |
| **L+A (in selection)** | Cut the selected files |
| **L+A (outside selection)** | Paste them here |
| **L+R** | Cancel the file selection |

**On Android's granted-folders list** (the top of the browser, `pt://roots`), the three SELECT chords
mean something else, because a granted folder is a *permission* rather than a file: rename, delete and
create-here are all refused there, and the top bar says so.

| | |
|---|---|
| **SELECT+A** | Make the folder under the cursor the home folder — where the app keeps `Projects/`, `Samples/` and the rest. Arms an `A=YES B=NO` confirm. Nothing on disk moves. |
| **SELECT+B** | Forget it: hand the access back to Android and drop the row. Arms the same confirm. ⚠️ Not a delete — no file is touched, and re-granting the folder restores it. |

The folder in use is drawn `(HOME)`; one whose directory no longer exists is drawn `(MISSING)` and is
never chosen as the home.

---

## All Button Combinations

### Tier 1: Basic Actions (No modifiers)

```
A                       Insert value on empty / Enter edit mode
A, A                    Insert the next UNUSED chain/phrase (double-tap, 300 ms)
A + RIGHT               Increment by small step (+1)
A + LEFT                Decrement by small step (-1)
A + UP                  Increment by large step (+16 or +12)
A + DOWN                Decrement by large step (-16 or -12)
A + B                   Delete value / reset it to its default
B                       Cancel / Exit / Back / Copy a selection
SELECT                  Context action (open or close what the cursor is on)
START                   Play / Stop
```

### Tier 2: B Modifier (Item Navigation)

```
B + LEFT/RIGHT          Previous / next phrase, chain, instrument, table, groove
B + UP/DOWN             Page the SONG screen / jump 16 instrument slots
```

### Tier 3: L Modifier (Selection & Clipboard)

```
L + A                   Paste (outside selection) / Cut (in selection)
L + B                   Enter selection mode / widen it
L + B + A               Clone current item (deep clone)
L + R                   Restore muted tracks / leave selection mode / clear the copy buffer
```

**L + R undoes one thing per press, most recent first.** Two things are on it: the mute and solo
state, and the selection with its copy buffer. Whichever was touched last is cleared first, and a
press falls through to the other when the first has nothing to clear.

### Tier 4: R Modifier (Navigation)

```
R + UP/DOWN/LEFT/RIGHT  Navigate between screens
R + LEFT                Up one directory (file browser)
R + LEFT/RIGHT          Scroll the text cursor (QWERTY overlay)
R + B                   Mute the track under the cursor  (SONG, MIXER)
R + A                   Solo it                          (SONG, MIXER)
```

### Tier 5: SELECT Modifier (File management)

```
SELECT + A              Rename                  (file browser)
SELECT + B              Delete, with a confirm  (file browser)
SELECT + R              New folder              (file browser)
```

### Reserved chords

These are deliberately **consumed and do nothing**, so that holding a modifier cannot fire an edit
underneath it. They are listed so they aren't mistaken for broken controls:

```
L + R + A, L + R + B, L + R + SELECT
```

`L + START` and `R + START` are consumed the same way everywhere except the SONG screen in LIVE
mode, where they queue a row and queue a channel's stop. They never toggle playback and never
silence a preview.

---

## Architecture

### Cursor Context System

Instead of checking which screen we're on, the system checks **what type of data the cursor is on**.

Each screen module implements `cursor_context(state)`, which answers "the cursor is on a NOTE, it is
empty, it can be inserted, its range is 12..127, a small step is 1 and a large step is 12". One
generic handler turns button presses into actions from that answer alone, so every screen gets
increment / decrement / fast-step / delete / insert for free.

**Key files:**
- `native/ui/cursor.h` — the value types, capabilities, stepping rules and the five button→action functions
- `native/ui/button_mapper.h` — the combo matrix: which of the ~30 named handlers a press means
- `native/ui/input_dispatcher.{h,cpp}` — what each handler does
- `shell/sdl-input.cpp` — the only platform-specific part: SDL keys and pad buttons → the ten buttons

### Adding Input to a New Screen

1. Return a `CursorContext` from your module's `cursor_context(state)` for each cursor position
2. Use the `cc::` factories (`hex_byte`, `note`, `toggle_ternary`, …) rather than filling the struct
3. All A+direction and A+B combos then work automatically

---

## M8 vs LGPT Design Decisions

PocketTracker takes the best of both systems:

| Feature | Source | Rationale |
|---------|--------|-----------|
| A + directions for editing | M8 | More precise control |
| LEFT/RIGHT small step, UP/DOWN large step | LGPT | The axis split LGPT users already have in their fingers |
| Dual modifiers (L/R) | LGPT | More ergonomic |
| Deep clone | M8 | Powerful unique feature |
| Selection mode cycling | M8 | More flexible |
| L + A for paste | LGPT | Simpler than SHIFT+EDIT |
| R + directions for screen nav | LGPT | Logical separation |

### Sources
- [M8 Tracker Shortcuts](https://gist.github.com/devin-dominguez/587720c9ab71b2d9f3c4bd48d9c812ca)
- [LGPT Reference Manual](http://wiki.littlegptracker.com/doku.php?id=lgpt%3Areference_manual)
