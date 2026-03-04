# PocketTracker User Manual

**Version:** 1.0 (Draft)
**App State:** MVP Extension Pack 3

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Installation](#2-installation)
3. [Interface Overview](#3-interface-overview)
4. [Navigation](#4-navigation)
5. [Controls Reference](#5-controls-reference)
6. [Song Structure](#6-song-structure)
7. [SONG Screen](#7-song-screen)
8. [CHAIN Screen](#8-chain-screen)
9. [PHRASE Screen](#9-phrase-screen)
10. [INSTRUMENT Screen](#10-instrument-screen)
11. [TABLE Screen](#11-table-screen)
12. [GROOVE Screen](#12-groove-screen)
13. [MODULATION Screen](#13-modulation-screen)
14. [MIXER Screen](#14-mixer-screen)
15. [PROJECT Screen](#15-project-screen)
16. [Effects Reference](#16-effects-reference)
17. [Modulation Reference](#17-modulation-reference)
18. [File Management](#18-file-management)
19. [Workflow Tips](#19-workflow-tips)

---

## 1. Introduction

PocketTracker is a sample-based music tracker for Android handhelds and budget Android devices. It is inspired by the M8 Headless tracker and Little GP Tracker (LGPT/Picotracker), and designed to run natively at **640×480** — the resolution of devices like the Miyoo Flip.

If you have never used a tracker before: a tracker is a music sequencer where notes are arranged in a grid, top to bottom, with time flowing downward. Each row is a step, each column carries a parameter (note, instrument, volume, effect). Songs are built by chaining together small patterns called **phrases**, which are grouped into **chains**, which are arranged in a **song**.

PocketTracker stores everything in a single project file (`.ptp`). Sounds are loaded from standard `.wav` files.

### Target devices

| Device | RAM | OS | Controls |
|---|---|---|---|
| Miyoo Flip | 1 GB | Android 13 | Physical buttons |
| Ayaneo Pocket Air Mini | 3 GB | Android 11 | Physical buttons |
| Any Android 8.0+ phone/tablet | 512 MB+ | Android 8+ | Touchscreen |

---

## 2. Installation

1. Download the latest `.apk` from the project releases page.
2. On your Android device, enable **Install from unknown sources** in Settings → Security.
3. Open the downloaded `.apk` and tap **Install**.
4. On first launch the app may ask for **All Files Access** permission (Android 11+). Grant it — this is required to read and write project and sample files.

### Sample files

PocketTracker has no bundled default samples — all instrument slots start empty. Copy your own `.wav` files to device storage and load them from the **INSTRUMENT** screen using the file browser.

### Project files

Projects are saved as `.ptp` files in:

```
/Documents/PocketTracker/Projects/
```

WAV exports are saved to:

```
/Documents/PocketTracker/Renders/
```

Resampled instruments are saved to:

```
/Documents/PocketTracker/Samples/Resampled/
```

---

## 3. Interface Overview

The entire UI renders at a fixed **640×480** pixel canvas, letterboxed on larger screens. The layout is always the same regardless of which screen you are on.

```
┌────────────────────────────────────────────────┐
│  OSCILLOSCOPE  (620×70 px)                     │
├──────────────────────────────┬─────────────────┤
│                              │  NAV MAP        │
│  MAIN EDITOR                 │  (80×105 px)    │
│  (varies by screen)          │                 │
│                              │  STATUS LINE    │
│                              │                 │
└──────────────────────────────┴─────────────────┘
```

**Oscilloscope** — real-time waveform of the mixed audio output. Always visible. Scrolls right-to-left.

**Main editor** — the active screen (PHRASE, CHAIN, SONG, etc.).

**Navigation map** — a miniature 5×5 grid showing your current position in the screen layout.

**Status line** — brief messages (e.g., "SAVED", "RESAMPLED TO INST 0C") that auto-dismiss after 5 seconds.

---

## 4. Navigation

All screens are arranged in a **5×5 grid**. You navigate this grid by holding **R** and pressing the D-pad.

```
     Col 0      Col 1      Col 2      Col 3      Col 4
     ─────      ─────      ─────      ─────      ─────
Row 0  ---        ---       SCALE    INST POOL    ---
Row 1  PROJ       PROJ      GROOVE     MODS       ---
Row 2  SONG      CHAIN     PHRASE     INST       TABLE
Row 3  MIXER     MIXER     MIXER      MIXER      MIXER
Row 4  EFFECTS  EFFECTS   EFFECTS   EFFECTS    EFFECTS
```

- **R + RIGHT / LEFT** — move left/right along Row 2 (the main editing screens)
- **R + UP / DOWN** — move to the screen above or below your current column
- The navigation map in the top-right corner always shows where you are

> **Tip:** The most common flow is PHRASE (col 2) ↔ INSTRUMENT (col 3) ↔ TABLE (col 4), all on Row 2. Press **R+UP** from any of these to reach context screens (GROOVE, MODS).

---

## 5. Controls Reference

### 5.1 Button Layout

#### Physical gamepad (Android handhelds)

| Physical button | Function |
|---|---|
| D-pad | Move cursor |
| A | Confirm / Insert |
| B | Cancel / Delete |
| L1 / L2 | L modifier |
| R1 / R2 | R modifier |
| SELECT / MENU | SELECT modifier |
| START / BACK | Play / Stop |

#### Keyboard (for testing on PC or with a Bluetooth keyboard)

| Key | Function |
|---|---|
| W / S / A / D or Arrow keys | D-pad |
| K or Enter | A button |
| J or Escape | B button |
| U | L button |
| I | R button |
| Left Shift | SELECT |
| Spacebar | START |

Both keyboard and gamepad work simultaneously.

---

### 5.2 Basic Actions

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A | Insert value (on an empty cell, inserts the last-used value) |
| B | Delete value / cancel |
| SELECT | Context delete (varies by screen) |
| START | Play / Stop |

---

### 5.3 Value Editing — A + D-pad

Hold **A** and press a direction to edit the value under the cursor:

| Combo | Step |
|---|---|
| A + UP | +1 (small step) |
| A + DOWN | −1 (small step) |
| A + RIGHT | +16 / +1 octave (large step) |
| A + LEFT | −16 / −1 octave (large step) |
| A + B | Delete / clear value |

- **Key repeat is active:** hold the combo for ~400 ms and it starts repeating at ~10 times/second.
- For **note values**, large step = ±12 semitones (one octave).
- For **hex byte values**, large step = ±0x10.

---

### 5.4 Context Navigation — L + D-pad

Hold **L** and press LEFT/RIGHT to navigate between items of the same type:

| Screen | L + LEFT / RIGHT |
|---|---|
| CHAIN | Previous / next chain (00–FF) |
| PHRASE | Previous / next phrase (00–FF) |
| INSTRUMENT | Previous / next instrument (00–FF) |
| TABLE | Previous / next table (00–FF) |
| GROOVE | Previous / next groove (00–FF) |

---

### 5.5 Screen Navigation — R + D-pad

Hold **R** and press a direction to move in the screen grid:

| Combo | Movement |
|---|---|
| R + LEFT / RIGHT | Move left / right (stays on current row) |
| R + UP | Move to screen above current column |
| R + DOWN | Move to screen below current column |

---

### 5.6 Copy / Paste

PocketTracker uses an M8-style selection system. Works on PHRASE, CHAIN, SONG, and TABLE screens.

| Input | Action |
|---|---|
| L + B | Enter selection mode (tap again to cycle: CELL → ROW → SCREEN) |
| B (in selection) | Copy selection, exit selection mode |
| L + A (in selection) | Cut (copy + clear), exit selection mode |
| L + A (outside selection) | Paste clipboard at cursor |
| A + B (in selection) | Delete selection (no clipboard), exit selection mode |
| L alone | Cancel selection (nothing copied) |

**Selection modes:**
- **CELL** — single cell under cursor
- **ROW** — full row (all columns)
- **SCREEN** — all rows visible

---

### 5.7 Playback Controls

| Input | Action |
|---|---|
| START | Play / Stop (plays from current screen context) |
| START on SONG | Play full song |
| START on CHAIN | Play current chain |
| START on PHRASE | Play current phrase (loops) |
| START on INSTRUMENT | Preview current instrument sample |

---

## 6. Song Structure

PocketTracker organizes music in a four-level hierarchy:

```
PROJECT
  └── SONG  (8 tracks, each is a column of chain IDs)
        └── CHAIN  (up to 16 phrase references + per-row transpose)
              └── PHRASE  (16 steps)
                    └── STEP  (Note + Instrument + Volume + 3 FX slots)
```

Think of it this way:

- A **STEP** is a single note event with optional effects.
- A **PHRASE** is a short pattern of 16 steps — like a bar of music.
- A **CHAIN** is a sequence of up to 16 phrases, played one after another. Each phrase slot can have a **transpose** value to shift pitch without duplicating the phrase.
- The **SONG** arranges chains across 8 tracks in a vertical list. Each row of the song plays all 8 tracks simultaneously.

All values (chain IDs, phrase IDs, instrument IDs, etc.) are hexadecimal, ranging from `00` to `FF`.

---

## 7. SONG Screen

The SONG screen arranges chains across 8 tracks. Each column is a track (T0–T7), each row is a song position.

```
     T0   T1   T2   T3   T4   T5   T6   T7
00   04   --   08   --   --   01   --   --
01   04   --   08   --   --   01   --   --
02   05   --   09   --   --   02   --   --
...
```

- `--` means the track is silent at that song position.
- Numbers are chain IDs (hex).

### Controls

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A | Insert last-used chain ID |
| A + UP/DOWN | Increment / decrement chain ID |
| A + RIGHT/LEFT | Increment / decrement by 16 |
| A + B | Delete (set to --) |
| START | Play song from top |

---

## 8. CHAIN Screen

A chain is a sequence of up to 16 phrase references. Each slot has:
- **PHR** — phrase ID (00–FF) or `--` (empty/silent)
- **TRN** — transpose (+/−) in semitones (00 = no transpose; values above 7F = negative transpose)

```
     PHR  TRN
00   04   00
01   04   00
02   05   0C   ← +12 semitones (one octave up)
03   05   00
...
```

When played, the chain loops: after the last filled slot it returns to slot 00.

### Controls

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A | Insert last-used value |
| A + UP/DOWN | Increment / decrement |
| A + LEFT/RIGHT | Increment / decrement by 16 (or ±12 semitones for TRN) |
| A + B | Delete slot |
| L + LEFT/RIGHT | Switch to previous / next chain |
| START | Play current chain |

---

## 9. PHRASE Screen

The phrase is where notes are written. It has 16 rows (steps 00–0F) and 5 columns:

```
     N    V    I    FX1      FX2      FX3
00   C-4  80   03   ---  00  ---  00  ---  00
01   ---  --   --   ---  00  ---  00  ---  00
02   E-4  80   03   ---  00  ---  00  ---  00
...
```

| Column | Meaning |
|---|---|
| N | Note (e.g., `C-4`, `F#3`). `---` = no note. |
| V | Volume (`00`–`FF`). `--` = use instrument default. |
| I | Instrument ID (`00`–`FF`). `--` = use last instrument. |
| FX1/FX2/FX3 | Effect type + value (e.g., `REP 03`, `ARP 47`) |

### Notes

Notes are written as pitch + octave: `C-4`, `C#4`, `D-4`, … `B-9`. The note range is **C-0 to B-9**.

Middle C is `C-4` (MIDI note 60).

### Controls

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A | Insert last-used note/value |
| A + UP/DOWN | +1 / −1 semitone (note), +1 / −1 (other values) |
| A + LEFT/RIGHT | +1 / −1 octave (note), +16 / −16 (other values) |
| A + B | Delete value at cursor |
| L + LEFT/RIGHT | Switch to previous / next phrase |
| START | Play current phrase (loops) |

### FX columns

Each FX slot has two parts: **type** (3-letter code) and **value** (2-digit hex). Use A+UP/DOWN on the type to cycle through available effects. Use A+UP/DOWN on the value to edit the parameter.

---

## 10. INSTRUMENT Screen

The INSTRUMENT screen configures how a sample is played. Navigate here with **R+RIGHT** from PHRASE, or **R+UP** from TABLE.

### Parameters

| Parameter | Range | Description |
|---|---|---|
| NAME | — | Instrument name (auto-generated or editable) |
| SAMPLE | path | WAV file path. Press A to open file browser. |
| ROOT | C-0 – B-9 | The pitch of the sample as recorded. Adjusts playback pitch relative to notes played. |
| DETUNE | 00–FF | Fine tuning. `80` = center (no detune). High nibble = whole semitones (±8), low nibble = 1/16 semitone steps. |
| VOL | 00–FF | Instrument base volume. `FF` = full. Multiplied with phrase V column and track/master volumes. |
| PAN | 00–FF | Stereo panning. `00` = full left, `80` = center, `FF` = full right. |
| START | 00–FF | Sample start point offset (fraction of total sample length). |
| END | 00–FF | Sample end point offset. |
| LOOP | OFF / FWD / PNG | Loop mode: off, forward loop, ping-pong loop. |
| REV | OFF / ON | Reverse playback. |

### Volume chain

The final volume of each note is:

```
Instrument VOL × Phrase V column × Track volume (Mixer) × Master volume (Mixer)
```

### Navigating instruments

- **L + LEFT/RIGHT** — switch between instruments 00–FF
- **R+UP** from INSTRUMENT → MODULATION screen for that instrument
- **A** on SAMPLE field → opens file browser

### File browser controls

| Input | Action |
|---|---|
| D-pad UP/DOWN | Move through files/folders |
| A | Select file (load sample) or enter folder |
| B | Go up one directory level |
| START | Preview highlighted WAV file |

---

## 11. TABLE Screen

A table is a 16-row micro-sequencer attached to an instrument. When a note plays, the table runs in parallel at a configurable tick rate, applying per-row transpose, volume, and effects on top of the phrase.

Tables are ideal for: drum rolls, note slides, automatic arpeggios, per-note LFO-style automation.

Each row:

```
     TRN  VOL  FX1      FX2      FX3
00   00   --   ---  00  ---  00  ---  00
01   03   --   ---  00  ---  00  ---  00
02   07   --   ---  00  ---  00  ---  00
...
```

| Column | Range | Description |
|---|---|---|
| TRN | 00–FF | Transpose in semitones. `00` = no shift. Values above `7F` are negative (e.g., `FC` = −4 semitones). |
| VOL | 00–FF / -- | Volume multiplier for this row. `--` = no change. |
| FX1–FX3 | same as phrase | Effects applied on this table tick. |

### TIC rate

The header shows **TIC XX** — how many phrase ticks pass per table row. Use the **TIC** effect in a phrase to change this value.

- Default: `0C` (12 ticks = one phrase step per table row)
- Lower = table advances faster; higher = slower
- Special values: `FC`=pingpong, `FE`=random, `FF`=stop

### Table–instrument link

By default, instrument N uses table N (e.g., instrument `03` → table `03`). Override per-note using the **TBL** phrase effect.

### Controls

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A + UP/DOWN | Edit value |
| A + B | Delete value |
| B + LEFT/RIGHT | Previous / next table |
| L + B / copy / paste | Selection, copy, paste (same as phrase) |
| START | Preview instrument with table |

---

## 12. GROOVE Screen

The GROOVE screen lets you control the timing of each phrase step individually. This is how you create **swing**, **shuffle**, double-time feels, triplets, and other rhythmic variations.

Navigate here: **R+UP** from CHAIN or PHRASE (column 2).

A groove is a list of up to 16 tick values. When a track plays, it cycles through the groove: the first step takes as many ticks as groove row 0, the second step takes groove row 1, and so on. When the groove list is exhausted it loops from the beginning.

```
     TIC
00   0C    ← 12 ticks (normal)
01   --    ← empty = end of list, loop from 00
```

### Swing example (groove 01)

```
     TIC
00   0E    ← 14 ticks (long)
01   0A    ← 10 ticks (short)
02   --    ← loop
```

This alternates long and short steps — the classic swing feel.

### Triplet example

```
     TIC
00   08
01   08
02   08
03   --    ← loop (3 × 8 = 24 ticks per 3 steps, same as 2 × 12)
```

### Default

Groove `00`, row 0 = `0C` — all steps are exactly 12 ticks (standard, no swing).

### Assigning grooves

- Each track plays groove `00` by default.
- Use the **GRV XX** phrase effect to switch a track to groove `XX` at that step.

### Controls

| Input | Action |
|---|---|
| D-pad UP/DOWN | Move between rows |
| A + UP/DOWN | Edit tick value (+1/−1) |
| A + LEFT/RIGHT | Edit tick value (+16/−16) |
| A + B | Clear row (set to --) |
| B + LEFT/RIGHT | Previous / next groove |

---

## 13. MODULATION Screen

The MODULATION screen (MODS) adds up to **4 modulation slots** per instrument. Each slot applies an automated envelope or LFO to a parameter destination.

Navigate here: **R+UP** from INSTRUMENT (column 3).

### Modulation types

| Type | Description |
|---|---|
| `---` | Off (no modulation) |
| AHD | One-shot envelope: Attack → Hold → Decay |
| ADSR | Classic envelope: Attack → Decay → Sustain → Release |
| DRUM | Percussive envelope: sharp peak → body hold → decay |
| LFO | Cyclic oscillator (triangle, sine, ramp, square, random, drunk) |
| TRIG | AHD envelope triggered by another instrument/track |
| TRACKING | Maps note pitch or velocity to a destination value (static) |

### Modulation destinations

| Dest | Affects |
|---|---|
| VOLUME | Amplitude |
| PAN | Stereo position |
| PITCH | Pitch (in semitones) |
| FINE | Pitch (in cents, subtle) |
| CUTOFF | Filter cutoff frequency |
| RES | Filter resonance |
| SMPSTRT | Sample start point |
| MOD AMT | Depth of the next mod slot |
| MOD RATE | Speed of the next mod slot |
| MOD BOTH | Both depth and speed of the next mod slot |

### Layout

The screen shows two mod slots side by side (MOD1+MOD2, then MOD3+MOD4). Parameters displayed change based on the selected type.

**AHD parameters:**
- TYPE, DEST, AMT (amount/depth), ATK (attack ticks), HOLD (hold ticks), DEC (decay ticks)

**ADSR parameters:**
- TYPE, DEST, AMT, ATK, DEC, SUS (sustain level), REL (release ticks)

**DRUM parameters:**
- TYPE, DEST, AMT, PEAK (transient shape), BODY (hold ticks), DEC

**LFO parameters:**
- TYPE, DEST, AMT, OSC (oscillator shape), TRIG (trigger mode), FREQ (rate)

**LFO trigger modes:**
- FREE — phase never resets (LFO runs continuously)
- RETRIG — phase resets to 0 on each new note
- HOLD — plays once, holds last value
- ONCE — plays one cycle, returns to 0

**LFO shapes:** TRI, SIN, RAMP DN, RAMP UP, EXP DN, EXP UP, SQU DN, SQU UP, RANDOM, DRUNK
(Shapes with `_T` suffix advance per tick instead of per step)

**TRACKING parameters:**
- TYPE, DEST, SRC (NOTE or VELOCITY), LVAL (low output), HVAL (high output)

### Mod-to-mod routing

When DEST is set to **MOD AMT**, **MOD RATE**, or **MOD BOTH**:
- Mod 1 modulates Mod 2
- Mod 2 modulates Mod 3
- Mod 3 modulates Mod 4
- Mod 4 modulates Mod 1 (circular)

Example: MOD1 (LFO) → MOD AMT → MOD2 (AHD): the LFO rhythmically swells the envelope depth.

### Controls

| Input | Action |
|---|---|
| D-pad UP/DOWN | Move between parameters |
| D-pad LEFT/RIGHT | Switch between paired mod slots (MOD1↔MOD2 or MOD3↔MOD4) |
| A + UP/DOWN | Edit value (small step) |
| A + LEFT/RIGHT | Edit value (large step) |
| A + B | Reset to default |
| B + LEFT/RIGHT | Previous / next instrument |

---

## 14. MIXER Screen

The MIXER screen shows all 8 tracks plus a master channel with real-time peak meters.

Navigate here: **R+DOWN** from any screen on Row 2.

```
  T0   T1   T2   T3   T4   T5   T6   T7   MST
  ██   ██   ██   ██   --   --   --   --   ██
  ██   ██   ██   --                       ██
  ██   --   --
  80   80   80   80   80   80   80   80   80
```

Each column shows a peak meter (dBFS scale) and the current volume value (`00`–`FF`, where `80` is 0 dB / unity).

### Volume scale

| Value | Level |
|---|---|
| `00` | Silent |
| `80` | Unity (0 dB) |
| `FF` | Maximum (+6 dB) |

### Meter zones

| Color | Range | Meaning |
|---|---|---|
| Red | ≥ 0 dBFS | Clipping |
| Yellow | −6 dBFS to 0 dBFS | Hot |
| Green | below −6 dBFS | Safe |

### Controls

| Input | Action |
|---|---|
| D-pad LEFT/RIGHT | Select track |
| A + UP/DOWN | Increase / decrease volume by 1 |
| A + LEFT/RIGHT | Increase / decrease volume by 16 |

---

## 15. PROJECT Screen

The PROJECT screen contains global project settings and file operations.

Navigate here: **R+UP** from SONG or CHAIN (column 0 or 1).

### Settings

| Parameter | Description |
|---|---|
| NAME | Project name (up to 12 characters). A+UP/DOWN cycles characters. |
| TEMPO | BPM (beats per minute). |
| TRANSPOSE | Global semitone offset applied to all tracks. |

### File operations

| Button / Action | Description |
|---|---|
| **SAVE** (cursor on SAVE + A) | Save project to `.ptp` file |
| **LOAD** (cursor on LOAD + A) | Open file browser to load a project |
| **WAV MIX** (cursor on WAV MIX + A) | Render the full song to a stereo 16-bit WAV file |

WAV export renders offline (faster than real-time) and saves to `/Documents/PocketTracker/Renders/` with auto-incremented filenames (`ProjectName_0001.wav`).

---

## 16. Effects Reference

Effects are placed in the **FX1**, **FX2**, and **FX3** columns of a phrase step, or in the FX columns of a table row. Each effect has a 3-letter code and a 2-digit hex value.

Effects persist until cancelled (by a new note on the same track, a new effect in the same column, or a KILL effect), unless stated otherwise.

---

### ARP `XX` — Arpeggio

Rapid cycling through multiple pitches to simulate chords.

`XX` encodes up to two interval offsets in semitones:
- High nibble (`X`) = first interval above root
- Low nibble (`Y`) = second interval above root

**Common values:**

| Value | Pattern | Sound |
|---|---|---|
| `37` | root, +3, +7 | Minor chord |
| `47` | root, +4, +7 | Major chord |
| `4B` | root, +4, +11 | Major 7th |
| `3A` | root, +3, +10 | Minor 7th |
| `CC` | root, +12, +12 | Octave doubling |
| `00` | (cancel) | Stop arpeggio |

Arpeggio persists across steps. Use `A00` to stop it. Configure the arpeggio pattern with the **ARC** effect.

---

### ARC `XX` — Arpeggio Config

Configures how the arpeggio cycles.

- High nibble (`X`) = mode: `0`=UP, `1`=DOWN, `2`=PINGPONG, `3`=RANDOM
- Low nibble (`Y`) = speed in ticks (`4` = default)

Example: `ARC 14` = DOWN mode, speed 4 ticks.

---

### CHA `XY` — Chance

Probability gate. Rolls a random number each time the step plays.

- `X` (high nibble) = probability for the **left side** of this FX column (note + earlier FX) to play. `0` = never, `F` = always.
- `Y` (low nibble) = probability for the **right side** (later FX columns) to play.

Example: `CHA F4` in FX2 = note always plays, FX3 plays ~27% of the time.

---

### DEL `XX` — Delay

Delays the step by `XX` ticks. The note and all effects on that row trigger `XX` ticks later than normal.

`DEL 00` = no delay (same as no DEL effect).

---

### GRV `XX` — Groove Assign

Switches the current track to use groove `XX` from this step onward.

See [GROOVE Screen](#12-groove-screen) for groove editing.

---

### HOP `XY` — Hop / Jump

Jumps the playhead to a different position.

- In a **phrase**: jumps to phrase step `Y`, limited to `X` times. After `X` hops it falls through.
- In a **table**: jumps to table row `Y`.

`HOP 00` at the end of a phrase section = loop that section indefinitely.

---

### KIL `00` — Kill

Immediately stops the sample on this track. Cancels any persistent effects (ARP, REP, pitch effects).

---

### OFF `XX` — Offset

Jumps the sample playback start point to offset `XX` (fraction of total sample length). Useful for accessing different portions of a long sample.

---

### PSL `XX` — Pitch Slide (Portamento)

Slides pitch from the previous note to the current note over `XX` ticks.

`PSL 00` = instant pitch change (no slide).

---

### PBN `XX` — Pitch Bend

Continuous pitch bend. Persists until cancelled.

- `00`–`7F` = bend UP (higher = faster)
- `80`–`FF` = bend DOWN (higher values from `80` = faster downward)
- `PBN 00` = cancel

---

### PVB `XY` — Vibrato

Oscillates pitch up and down.

- `X` = speed (`0`–`F`, higher = faster, range ~2–9.5 Hz)
- `Y` = depth (`0`–`F`, higher = wider, up to ~1.9 semitones)

Persists until `PVB 00` or new note or KILL.

---

### PVX `XY` — Extreme Vibrato

Same as PVB but 4× deeper and 2× faster. For dramatic wobble and pitch LFO effects.

---

### REP `XY` — Repeat (Retrigger)

Retriggering the note within a step.

- **Y = 0 (simple retrig mode):** Retrigger every `X` ticks. `R30` = retrig every 3 ticks.
- **Y ≠ 0 (volume ramp mode):** Retrigger every `Y` ticks, with volume ramping each hit.
  - `X` = 0: no volume change
  - `X` = 1–7: decrease volume each retrig (1=subtle, 7=aggressive fade-out)
  - `X` = 8–F: increase volume each retrig (fade-in effect)

Persists across steps. Cancel with a new note, new FX in same column, or KILL.

---

### RND `XY` — Randomize

Randomizes the **previously active FX** value on this track.

- `X` = downward randomization range (`0`=none, `F`=maximum)
- `Y` = upward randomization range

---

### RNL `XY` — Randomize Left

Randomizes the FX value **in the column immediately to the left**.

- `RNL` in FX1 = randomizes note pitch and instrument
- `RNL` in FX2 = randomizes FX1 value
- `RNL` in FX3 = randomizes FX2 value

`X`/`Y` = same range semantics as RND.

---

### TBL `XX` — Table Set

Overrides the instrument's default table, using table `XX` for this note instead.

---

### THO `XX` — Table Hop

Jumps the table playhead to row `0X`. Works like HOP but for table rows.

`THO 00` at the end of a table section = loop that section.

---

### TIC `XX` — Tick Rate

Sets the table tick rate (how many phrase ticks per table row advance).

- `TIC 0C` = 12 ticks per row (one row per phrase step — default)
- `TIC 06` = 6 ticks per row (table advances twice as fast)
- `TIC FC` = ping-pong (table bounces up and down)
- `TIC FE` = random row each tick
- `TIC FF` = stop table

---

### VOL `XX` — Volume Automation

Overrides the step volume with `XX` at the exact tick this command fires. Can appear in a table row to animate volume over time.

---

### VIB (see PVB/PVX above)

---

## 17. Modulation Reference

See [MODULATION Screen](#13-modulation-screen) for how to edit mod slots.

### AHD Envelope

One-shot envelope triggered on each note.

```
     ┌──────┐
     │      │
  ATK│ HOLD │DEC
     │      └────
─────┘            ─ (silence)
```

- **ATK** (`00`–`FF`) — attack time in ticks (0 = instant)
- **HOLD** (`00`–`FF`) — how long to hold peak
- **DEC** (`00`–`FF`) — decay time to silence

### ADSR Envelope

Classic synthesizer envelope. Release is triggered when the note ends (KILL or voice steal).

```
        ┌──┐
     /  │  │ \
   /ATK DEC│REL\
  /        │SUS │
```

- **ATK** — attack time
- **DEC** — decay to sustain level
- **SUS** (`00`–`FF`) — sustain level (00 = silence, FF = full)
- **REL** — release time after note off

### DRUM Envelope

For percussive sounds. Sharp peak, short body, long decay.

- **PEAK** — shape of the initial transient
- **BODY** — how long to hold after peak
- **DEC** — tail decay time

### LFO

Cyclic modulation. Rate is controlled by **FREQ**.

- **OSC** — shape (TRI, SIN, RAMP DN, RAMP UP, EXP DN, EXP UP, SQU DN, SQU UP, RANDOM, DRUNK)
- **TRIG** — FREE / RETRIG / HOLD / ONCE
- **FREQ** (`00`–`FF`) — rate (higher = faster cycling)

Shapes ending in `_T` use ticks as the time base (tempo-synced feel); others use musical steps.

### TRIG Envelope

Like AHD but triggered by another instrument or track, not by its own note.

- **SRC** — source: instrument `00`–`7F` or track `80`–`87`
- ATK, HOLD, DEC — same as AHD

### TRACKING

Reads note pitch or velocity and maps it to a destination.

- **SRC** — NOTE (pitch), VELOCITY, or VEL_TAKEOVER
- **LVAL** — output value when source is at minimum
- **HVAL** — output value when source is at maximum

Example: TRACKING NOTE → CUTOFF, LVAL `20`, HVAL `E0` = low notes have a dark filter, high notes are bright.

---

## 18. File Management

### Project files

- Format: `.ptp` (JSON internally)
- Default location: `/Documents/PocketTracker/Projects/`
- Save: PROJECT screen → SAVE
- Load: PROJECT screen → LOAD

### Sample files

- Format: `.wav` (mono or stereo; stereo is auto-converted to mono on load)
- Sample rates: any — PocketTracker auto-compensates pitch for non-44100 Hz samples
- Loaded from: INSTRUMENT screen → SAMPLE field → A button → file browser

### WAV exports

- Format: 16-bit stereo WAV, 44100 Hz
- Location: `/Documents/PocketTracker/Renders/`
- Filename: `ProjectName_0001.wav`, `_0002.wav`, … (auto-incremented)
- Triggered from: PROJECT screen → WAV MIX

### Resampled instruments

Created via **Selection Resampling** (see below):
- Format: 16-bit stereo WAV
- Location: `/Documents/PocketTracker/Samples/Resampled/`
- Filename: `Resample_0001.wav`, … (auto-incremented)

### Selection Resampling

You can render a selection of tracks from the SONG screen directly into a new instrument slot:

1. On the SONG screen, enter selection mode (**L + B**).
2. Select the rows and tracks you want to render.
3. **Double-tap A** — a confirmation dialog appears.
4. Choose **YES** — the selected tracks render offline to a WAV file.
5. A new instrument is created in the first empty slot, loaded with the rendered audio.
6. A status message shows the new instrument number (e.g., `RESAMPLED TO INST 0C`).

---

## 19. Workflow Tips

### Making your first phrase

1. Open **PHRASE** screen (R+RIGHT from SONG).
2. Cursor on row 00, column N (Note).
3. Press **A** — inserts default note (C-4, instrument 00).
4. Use **A+UP/DOWN** to change pitch.
5. Use **A+RIGHT/LEFT** to change octave.
6. Move cursor right to the **I** column, use **A+UP/DOWN** to select an instrument.
7. Press **START** to hear the phrase loop.

### Building a basic beat

1. Load a kick on instrument `00` (use default or load your own WAV).
2. Load a snare on instrument `01`, hihat on `02`.
3. In PHRASE `00`: place kick on steps 00, 04, 08, 0C (every 4 steps).
4. Place snare on steps 04, 0C.
5. Place hihat on steps 00, 02, 04, 06, 08, 0A, 0C, 0E (every 2 steps).
6. Create a CHAIN (`00`) pointing to PHRASE `00`.
7. In the SONG, put CHAIN `00` on track 0, row 00.
8. Hit START on the SONG screen.

### Using transpose in chains

Rather than duplicating a phrase at a different pitch, add a transpose value in the chain:
- In CHAIN, set the **TRN** column to `07` for +7 semitones (a fifth up).
- The same phrase plays higher without any copy.

### Swing with groove

1. Navigate to GROOVE screen (R+UP from PHRASE).
2. Create groove `01` with rows: `0E`, `0A`, `--`.
3. In your phrase, add `GRV 01` in the FX column of step 00.
4. The track now plays with swing timing.

### Table-based drum roll

1. On TABLE `03` (used by instrument 03 — snare), set rows 00–07 to TRN `00` and FX1 `REP 01`.
2. This causes the snare to retrigger on every table tick.
3. Set the TIC rate low (`TIC 03` in phrase) for a fast roll.

### Modulation: Volume fade-in on a pad

1. Open INSTRUMENT for your pad, then navigate to MODS (R+UP).
2. Set MOD1 TYPE = `AHD`.
3. Set MOD1 DEST = `VOLUME`.
4. Set ATK = `40`, HOLD = `20`, DEC = `60`.
5. Every time this instrument triggers, its volume envelope shapes the amplitude.

### WAV export

1. Build your complete song.
2. Navigate to PROJECT screen.
3. Move cursor to `WAV MIX` and press **A**.
4. The song renders offline. A status message shows the output filename when done.

---

## Appendix A: Hexadecimal Quick Reference

PocketTracker uses hexadecimal (base-16) for most values. Here's a quick conversion:

| Decimal | Hex | Decimal | Hex |
|---|---|---|---|
| 0 | `00` | 128 | `80` |
| 16 | `10` | 160 | `A0` |
| 32 | `20` | 192 | `C0` |
| 64 | `40` | 224 | `E0` |
| 96 | `60` | 255 | `FF` |

Half of `FF` (full) = `80` (center/unity). This is the default for VOL and PAN.

---

## Appendix B: Note Names

```
C  C# D  D# E  F  F# G  G# A  A# B
00 01 02 03 04 05 06 07 08 09 0A 0B  (semitone offset within octave)
```

Middle C = `C-4` = MIDI note 60.

Full range: `C-0` (lowest) to `B-9` (highest).

---

## Appendix C: Instrument Slots

All 256 instrument slots (00–FF) start empty in a new project. There are no bundled default samples — load your own `.wav` files via the INSTRUMENT screen.

Slot names are auto-generated as `INST00`–`INSTFF` and can be changed. An instrument with no loaded sample plays silence.

---

## Known Issues (Draft Build)

- On some devices, a generic input warning may appear in the log after device restart. This is harmless and disappears after a reboot.

---

*PocketTracker is an open-source project. Contributions and bug reports are welcome.*
