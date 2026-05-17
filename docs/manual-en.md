# PocketTracker User Manual

**Version:** 1.1  
**App state:** All Extension Packs complete — Testing & Polish

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
11. [SAMPLE EDITOR Screen](#11-sample-editor-screen)
12. [TABLE Screen](#12-table-screen)
13. [GROOVE Screen](#13-groove-screen)
14. [MODULATION Screen](#14-modulation-screen)
15. [MIXER Screen](#15-mixer-screen)
16. [EFFECTS Screen](#16-effects-screen)
17. [EQ EDITOR](#17-eq-editor)
18. [PROJECT Screen](#18-project-screen)
19. [SETTINGS Screen](#19-settings-screen)
20. [Effects Reference](#20-effects-reference)
21. [Modulation Reference](#21-modulation-reference)
22. [File Management](#22-file-management)
23. [Workflow Tips](#23-workflow-tips)

---

## 1. Introduction

PocketTracker is a sample-based music tracker for Android handhelds and budget Android devices. It is inspired by M8, LSDJ, and Little GP Tracker (LGPT/Picotracker), and designed to run natively at **640×480** — the resolution of devices like the Miyoo Flip.

If you have never used a tracker before: a tracker is a music sequencer where notes are arranged in a grid, top to bottom, with time flowing downward. Each row is a step, each column carries a parameter (note, instrument, volume, effect). Songs are built by chaining together small patterns called **phrases**, which are grouped into **chains**, which are arranged in a **song**.

PocketTracker stores everything in a single project file (`.ptp`). Sounds are loaded from standard `.wav` files, or from **SoundFont (SF2)** files.

### Target devices

| Device | RAM | OS | Controls |
|---|---|---|---|
| Miyoo Flip | 1 GB | Android 13 | Physical buttons |
| Ayaneo Pocket Air Mini | 3 GB | Android 11 | Physical buttons |
| Any Android 8.0+ phone / tablet | 512 MB+ | Android 8+ | Touchscreen |

---

## 2. Installation

1. Download the latest `.apk` from the project releases page.
2. On your Android device, enable **Install from unknown sources** in Settings → Security.
3. Open the downloaded `.apk` and tap **Install**.
4. On first launch the app may ask for **All Files Access** permission (Android 11+). Grant it — this is required to read and write project and sample files.

### Sample files

PocketTracker has no bundled default samples — all instrument slots start empty. Copy your own `.wav` files to device storage and load them from the **INSTRUMENT** screen using the file browser. SF2 files are loaded the same way.

### Project files

Projects are saved as `.ptp` files in:

```
/Documents/PocketTracker/Projects/
```

WAV exports are saved to:

```
/Documents/PocketTracker/Renders/
```

Resampled instruments and CHOP exports are saved to:

```
/Documents/PocketTracker/Samples/Resampled/
/Documents/PocketTracker/Samples/Chops/{name}/
```

---

## 3. Interface Overview

The entire UI renders at a fixed **640×480** pixel canvas, letterboxed on larger screens.

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

**Oscilloscope** — real-time waveform of the mixed audio output. Always visible.

**Main editor** — the active screen (PHRASE, CHAIN, SONG, etc.).

**Navigation map** — a miniature 5×5 grid showing your current position in the screen layout.

**Status line** — brief messages (e.g., `SAVED`, `RESAMPLED TO INST 0C`) that auto-dismiss after a few seconds.

Some screens (SAMPLE EDITOR, EQ EDITOR) open as full-screen overlays that temporarily replace the main layout.

---

## 4. Navigation

All screens are arranged in a **5×5 grid**. Navigate by holding **R** and pressing the D-pad.

```
     Col 0      Col 1      Col 2      Col 3      Col 4
     ─────      ─────      ─────      ─────      ─────
Row 0  ---        ---       SCALE    INST POOL    ---
Row 1  PROJ       PROJ      GROOVE     MODS       ---
Row 2  SONG      CHAIN     PHRASE     INST       TABLE
Row 3                       MIXER
Row 4                      EFFECTS
```

> Row 0 context screens (SCALE, INST POOL) only appear when you are on PHRASE or INSTRUMENT column. MIXER and EFFECTS (rows 3–4) only appear in your **current column** — move left/right first, then up/down.

| Combo | Action |
|---|---|
| R + RIGHT / LEFT | Move left / right along Row 2 |
| R + UP | Move to the screen above in current column |
| R + DOWN | Move to the screen below in current column |

The navigation map in the top-right corner always shows where you are.

**Popup screens** — not in the grid, opened contextually:

| Screen | How to open |
|---|---|
| SAMPLE EDITOR | INSTRUMENT screen → cursor on SAMPLE → SELECT |
| EQ EDITOR | INSTRUMENT / MIXER / EFFECTS screen → cursor on EQ row → SELECT |
| SETTINGS | PROJECT screen → cursor on SETTINGS row → A |

> **Tip:** The most common flow is PHRASE (col 2) ↔ INSTRUMENT (col 3) ↔ TABLE (col 4), all on Row 2. Press **R+UP** to reach GROOVE or MODS above.

---

## 5. Controls Reference

### 5.1 Button Layout

#### Physical gamepad (Android handhelds)

| Physical button | Function |
|---|---|
| D-pad | Move cursor |
| A | Confirm / Insert |
| B | Cancel / Delete |
| L | L modifier |
| R | R modifier |
| SELECT | SELECT modifier |
| START | Play / Stop |

#### Keyboard (Bluetooth keyboard or testing on PC)

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
| SELECT | Context action (varies by screen) |
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

- **Key repeat is active:** hold the combo for ~400 ms and it starts repeating at ~10/s.
- For **note values**, large step = ±12 semitones (one octave).
- For **hex byte values**, large step = ±0x10.

---

### 5.4 Context Navigation — L + D-pad

Hold **L** and press LEFT/RIGHT to switch between items of the same type:

| Screen | L + LEFT / RIGHT |
|---|---|
| CHAIN | Previous / next chain (00–FF) |
| PHRASE | Previous / next phrase (00–FF) |
| INSTRUMENT | Previous / next instrument (00–FF) |
| TABLE | Previous / next table (00–FF) |
| GROOVE | Previous / next groove (00–FF) |

---

### 5.5 Screen Navigation — R + D-pad

Hold **R** and press a direction to move in the screen grid (see §4).

---

### 5.6 Copy / Paste

Works on PHRASE, CHAIN, SONG, and TABLE screens.

| Input | Action |
|---|---|
| L + B | Enter selection mode (tap again to cycle: CELL → ROW → SCREEN) |
| B (in selection) | Copy selection, exit selection mode |
| L + A (in selection) | Cut (copy + clear), exit selection mode |
| L + A (outside selection) | Paste clipboard at cursor |
| A + B (in selection) | Delete selection (no clipboard), exit selection mode |
| L alone | Cancel selection (nothing copied) |

**Selection increment:** In selection mode, **A + UP/DOWN** increments or decrements all selected values simultaneously.

**Selection modes:**
- **CELL** — single cell under cursor
- **ROW** — full row (all columns)
- **SCREEN** — all rows visible

---

### 5.7 Playback Controls

| Input | Action |
|---|---|
| START | Play / Stop (context-aware) |
| START on SONG | Play full song from top |
| START on CHAIN | Play current chain |
| START on PHRASE | Play current phrase (loops) |
| START on INSTRUMENT | Preview current instrument |
| START on SAMPLE EDITOR | Preview edited sample |

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

- A **STEP** is a single note event with optional effects.
- A **PHRASE** is a short pattern of 16 steps — like a bar of music.
- A **CHAIN** is a sequence of up to 16 phrases. Each phrase slot can have a **transpose** value to shift pitch without duplicating the phrase.
- The **SONG** arranges chains across 8 tracks. Each row plays all 8 tracks simultaneously.

All values (chain IDs, phrase IDs, instrument IDs, etc.) are hexadecimal, ranging from `00` to `FF`.

---

## 7. SONG Screen

The SONG screen arranges chains across 8 tracks. Each column is a track (T0–T7), each row is a song position.

```
     T0   T1   T2   T3   T4   T5   T6   T7
00   04   --   08   --   --   01   --   --
01   04   --   08   --   --   01   --   --
02   05   --   09   --   --   02   --   --
```

`--` means the track is silent at that position. Numbers are chain IDs (hex).

### Controls

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A | Insert last-used chain ID |
| A + UP/DOWN | Increment / decrement chain ID |
| A + LEFT/RIGHT | Increment / decrement by 16 |
| A + B | Delete (set to --) |
| B + UP/DOWN | Page up / down (jump 16 rows) |
| START | Play song from current row |

---

## 8. CHAIN Screen

A chain is a sequence of up to 16 phrase references. Each slot has:
- **PHR** — phrase ID (`00`–`FF`) or `--` (empty)
- **TRN** — transpose in semitones (`00` = no transpose; values above `7F` = negative)

```
     PHR  TRN
00   04   00
01   04   00
02   05   0C   ← +12 semitones (one octave up)
03   05   00
```

When played, the chain loops from slot 00 after the last filled slot.

### Controls

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A | Insert last-used value |
| A + UP/DOWN | Increment / decrement |
| A + LEFT/RIGHT | ±16 (PHR) or ±12 semitones (TRN) |
| A + B | Delete slot |
| L + LEFT/RIGHT | Switch to previous / next chain |
| START | Play current chain |

---

## 9. PHRASE Screen

The phrase editor has 16 rows (steps 00–0F) and 5 columns:

```
     N    V    I    FX1      FX2      FX3
00   C-4  80   03   ---  00  ---  00  ---  00
01   ---  --   --   ---  00  ---  00  ---  00
02   E-4  80   03   ARP  47  ---  00  ---  00
```

| Column | Meaning |
|---|---|
| N | Note (`C-4`, `F#3`, etc.). `---` = no note. |
| V | Volume (`00`–`FF`). `--` = use instrument default. |
| I | Instrument ID (`00`–`FF`). `--` = use last instrument. |
| FX1/FX2/FX3 | Effect type + value (e.g., `REP 03`, `ARP 47`) |

Notes are written as pitch + octave: `C-4`, `C#4`, `D-4`, … `B-9`. Range is **C-0 to B-9**. Middle C = `C-4` (MIDI note 60).

### Controls

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A | Insert last-used note / value |
| A + UP/DOWN | +1 / −1 semitone (note), +1 / −1 (other values) |
| A + LEFT/RIGHT | ±1 octave (note), ±16 (other values) |
| A + B | Delete value at cursor |
| L + LEFT/RIGHT | Switch to previous / next phrase |
| START | Play current phrase (loops) |

### FX columns

Each FX slot has two parts: **type** (3-letter code) and **value** (2-digit hex). Use A+UP/DOWN on the type to cycle through available effects. Effects are listed in §20.

---

## 10. INSTRUMENT Screen

The INSTRUMENT screen configures how a sample or SF2 preset is played.

Navigate here with **R+RIGHT** from PHRASE, or **L+LEFT/RIGHT** to switch instruments.

### WAV instrument parameters

| Parameter | Range | Description |
|---|---|---|
| NAME | — | Instrument name. |
| SAMPLE | path | WAV or SF2 file. Press A to open file browser; SELECT to open SAMPLE EDITOR. |
| ROOT | C-0 – B-9 | The pitch of the sample as recorded. |
| DETUNE | 00–FF | Fine tuning. `80` = center. |
| VOL | 00–FF | Base volume. `FF` = full. |
| PAN | 00–FF | Stereo pan. `00` = full left, `80` = center, `FF` = full right. |
| START | 00–FF | Sample start point (fraction of sample length). |
| END | 00–FF | Sample end point. |
| LOOP | OFF / FWD / PNG | Loop mode: off, forward, ping-pong. |
| REV | OFF / ON | Reverse playback. |
| SLICE | OFF / CUT / TRU | Slice playback mode (see below). |
| FILTER | LP / HP / BP / OFF | Resonant SVF filter type. |
| CUT | 00–FF | Filter cutoff frequency. `FF` = open. |
| RES | 00–FF | Filter resonance. `00` = none. |
| DRIVE | 00–FF | Tape-style saturation. `00` = off. |
| CRUSH | 00–FF | Bit-depth crusher. `00` = off. |
| EQ | — | Press SELECT to open the EQ EDITOR for this instrument. |

### SF2 instrument parameters

When the loaded sample is an SF2 file, the instrument screen additionally shows override fields for the SoundFont's internal envelope and filter. Setting these to `--` uses the SF2 preset's built-in values.

| Parameter | Description |
|---|---|
| ATK | Envelope attack time override |
| DEC | Envelope decay time override |
| SUS | Envelope sustain level override |
| REL | Envelope release time override |
| CUT | Filter cutoff override |
| RES | Filter resonance override |

### Volume chain

```
Instrument VOL × Phrase V column × Track volume (Mixer) × Master volume (Mixer)
```

### Slice playback

When slice markers exist on the sample (set via the SAMPLE EDITOR), the SLICE parameter controls how notes select slices:

| Mode | Behaviour |
|---|---|
| OFF | Normal pitch playback — markers are ignored. |
| CUT | Note pitch selects a slice (C-4 relative to ROOT = slice 0). Plays from slice start to next marker, then stops. |
| TRU | Same slice selection; plays from slice start to end of sample. |

### Navigating instruments

- **L + LEFT/RIGHT** — switch between instruments 00–FF
- **R+UP** from INSTRUMENT → MODULATION screen
- **A** on SAMPLE field → opens file browser
- **SELECT** on SAMPLE field → opens SAMPLE EDITOR

### File browser controls

| Input | Action |
|---|---|
| D-pad UP/DOWN | Move through files / folders |
| A | Load file or enter folder |
| B | Go up one directory level |
| START | Preview highlighted WAV file |

---

## 11. SAMPLE EDITOR Screen

The SAMPLE EDITOR is a full-screen waveform editor for the currently loaded WAV. Open it from the INSTRUMENT screen by moving the cursor to the SAMPLE row and pressing **SELECT**.

Press **B** to close and discard unsaved changes. Use **SAVE / OVERWRITE** inside the editor to write changes to disk.

### Waveform view

The waveform fills the top portion of the screen. A playback cursor shows the current read position during preview.

| Input | Action |
|---|---|
| A + LEFT/RIGHT | Zoom in / out |
| D-pad LEFT/RIGHT | Scroll the view (when zoomed in) |
| START | Preview current sample (respects SOURCE mode) |

### Selection

Move the selection start and end markers to define a region for operations.

| Input | Action |
|---|---|
| D-pad (on start/end marker row) | Move the active marker |
| A + LEFT/RIGHT | Jump marker by large step |

### Non-destructive parameters

These change playback behaviour without modifying the waveform data:

| Parameter | Options | Description |
|---|---|---|
| SOURCE | LEFT / RIGHT / STEREO / MONO | Which channel(s) of a stereo WAV to use. Non-destructive — never alters the file. SAVE/OVERWRITE applies SOURCE at write time. |
| RATE | HIGH / NORM / LOFI | Sample rate mode. NORM = original. LOFI = 8-bit lo-fi downsampling. |

### Destructive operations

These modify the waveform in memory (UNDO is available after each operation).

| Operation | Description |
|---|---|
| CROP | Trim sample to current selection. |
| COPY | Copy selection to clipboard. |
| CUT | Copy selection to clipboard and silence it. |
| PASTE | Insert clipboard at selection start. |
| NORMALIZE | Scale amplitude so peak = 0 dBFS. |
| FADE IN | Apply a linear fade-in over the selection. |
| FADE OUT | Apply a linear fade-out over the selection. |
| SILENCE | Zero out the selection. |
| REVERSE | Reverse the selection. |
| UNDO | Revert to state before the last destructive operation. |

### SYNC mode

SYNC applies time or pitch transformations to match the sample to the current project BPM.

| Sub-mode | Description |
|---|---|
| RPITCH | Pitch-shifts the selection to align its length to a target beat count, without changing duration. |
| TSTRETCH | Time-stretches the selection to a target beat duration without changing pitch. Uses SOLA (Synchronized Overlap-Add), Akai-cyclic mode — the same algorithm as the Akai S950/S1000. |

### Offline FX

Applied to the whole sample (or selection) offline — rendered immediately, with UNDO available.

| FX | Description |
|---|---|
| EQ | Apply the EQ settings (opens EQ EDITOR first). |
| DUST | Lo-fi effect chain: shelf EQ → low-pass → tube saturation → FET compression → wow/drift → bitcrush → soft-clip. |
| DRIVE | Tape-style saturation. |
| OTT | 3-band bidirectional compressor. |

### Transient detection and slices

The SLICE row controls how slice markers are managed:

| Mode | Behaviour |
|---|---|
| OFF | Show existing WAV cue markers (read-only). No detection. |
| TRANSIENT | Run spectral-flux transient detection. SENS (`00`–`FF`) controls threshold. |
| DIVIDE | Divide the sample into equal slices. |

Slice markers are stored in the WAV `cue ` chunk — compatible with M8, Blackbox, Reaper, Logic, and Adobe Audition.

**CHOP** — exports each slice as a separate WAV file to `Samples/Chops/{name}/`.

### Save

| Action | Description |
|---|---|
| SAVE | Write to a new file (auto-incremented name). |
| OVERWRITE | Replace the existing file on disk. Applies SOURCE at write time (LEFT/RIGHT = mono export from that channel; STEREO = 2-channel WAV; MONO = averaged mono). |

---

## 12. TABLE Screen

A table is a 16-row micro-sequencer attached to an instrument. When a note plays, the table runs in parallel at a configurable tick rate, applying per-row transpose, volume, and effects.

Tables are useful for: drum rolls, note slides, automatic arpeggios, per-note automation.

```
     TRN  VOL  FX1      FX2      FX3
00   00   --   ---  00  ---  00  ---  00
01   03   --   ---  00  ---  00  ---  00
02   07   --   ---  00  ---  00  ---  00
```

| Column | Range | Description |
|---|---|---|
| TRN | 00–FF | Transpose in semitones. `00` = no shift. Values above `7F` are negative (e.g., `FC` = −4 st). |
| VOL | 00–FF / -- | Volume multiplier for this row. `--` = no change. |
| FX1–FX3 | same as phrase | Effects applied on this table tick. |

### TIC rate

The header shows **TIC XX** — how many phrase ticks pass per table row. Use the **TIC** phrase effect to change this value.

- Default: `0C` (12 ticks = one phrase step per table row)
- Lower = table advances faster
- Special values: `FC` = ping-pong, `FE` = random row, `FF` = stop

### Table–instrument link

By default, instrument N uses table N. Override per-note with the **TBL** phrase effect.

### Controls

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A + UP/DOWN | Edit value |
| A + B | Delete value |
| L + LEFT/RIGHT | Previous / next table |
| L + B / copy / paste | Selection, copy, paste (same as phrase) |

---

## 13. GROOVE Screen

Groove controls the timing of each phrase step individually — this is how you create swing, shuffle, triplets, and other rhythmic feels.

Navigate here: **R+UP** from PHRASE or CHAIN (column 2).

A groove is a list of up to 16 tick values. The track cycles through the list: step 1 takes groove row 0 ticks, step 2 takes groove row 1 ticks, and so on. The list loops when exhausted.

```
     TIC
00   0C    ← 12 ticks (even)
01   --    ← end of list, loop
```

### Swing example

```
     TIC
00   0E    ← 14 ticks (long)
01   0A    ← 10 ticks (short)
02   --
```

### Triplet example

```
     TIC
00   08
01   08
02   08
03   --    ← 3 × 8 = 24 ticks = same total as 2 × 12
```

Default: groove `00`, row 0 = `0C` (even timing, no swing).

### Assigning grooves

Each track uses groove `00` by default. Use the **GRV XX** phrase effect to switch a track to groove `XX`.

### Controls

| Input | Action |
|---|---|
| D-pad UP/DOWN | Move between rows |
| A + UP/DOWN | Edit tick value |
| A + LEFT/RIGHT | Edit tick value (large step) |
| A + B | Clear row |
| L + LEFT/RIGHT | Previous / next groove |

---

## 14. MODULATION Screen

The MODULATION screen (MODS) adds up to **4 modulation slots** per instrument. Each slot runs an envelope or LFO targeting a destination parameter.

Navigate here: **R+UP** from INSTRUMENT (column 3).

### Modulation types

| Type | Description |
|---|---|
| `---` | Off |
| AHD | One-shot envelope: Attack → Hold → Decay |
| ADSR | Envelope: Attack → Decay → Sustain → Release |
| DRUM | Percussive envelope: sharp peak → body hold → tail decay |
| LFO | Cyclic oscillator |
| TRIG | Envelope triggered by note, behaves like ADSR (external source triggering is planned post-MVP) |

### Modulation destinations

| Dest | Affects |
|---|---|
| VOLUME | Amplitude |
| PAN | Stereo position |
| PITCH | Pitch in semitones |
| FINE | Pitch (fine, same range as PITCH — naming may change post-MVP) |
| CUTOFF | Filter cutoff |
| RES | Filter resonance |
| SMPSTRT | Sample start point |
| MOD AMT | Depth of the next mod slot |
| MOD RATE | Speed of the next mod slot |
| MOD BOTH | Both depth and speed of the next mod slot |

### Layout

The screen shows two mod slots side by side (MOD1+MOD2, then MOD3+MOD4).

**AHD parameters:** TYPE, DEST, AMT, ATK, HOLD, DEC

**ADSR / TRIG parameters:** TYPE, DEST, AMT, ATK, DEC, SUS, REL

**DRUM parameters:** TYPE, DEST, AMT, ATK (peak), HOLD (body), DEC

**LFO parameters:** TYPE, DEST, AMT, OSC, TRIG (trigger mode), FREQ

**LFO trigger modes:**
- FREE — phase never resets
- RETRIG — phase resets to 0 on each new note

**LFO shapes:** TRI, SIN, RMP+, RMP−, EXP+, EXP−, SQU+, SQU−, RANDOM, DRUNK  
*(EXP/RANDOM/DRUNK currently fall back to SIN — full shapes coming post-MVP)*

### Mod-to-mod routing

When DEST is **MOD AMT**, **MOD RATE**, or **MOD BOTH**, the slot modulates the next slot (circular: slot 4 → slot 1).

Example: MOD1 (LFO) → MOD AMT → MOD2 (AHD) — the LFO rhythmically swells the envelope depth.

### Controls

| Input | Action |
|---|---|
| D-pad UP/DOWN | Move between parameters |
| D-pad LEFT/RIGHT | Switch between paired slots (MOD1↔MOD2 or MOD3↔MOD4) |
| A + UP/DOWN | Edit value |
| A + LEFT/RIGHT | Edit value (large step) |
| A + B | Reset to default |
| L + LEFT/RIGHT | Previous / next instrument |

---

## 15. MIXER Screen

The MIXER screen shows all 8 tracks plus a master column with real-time dBFS peak meters.

Navigate here: **R+DOWN** from any Row 2 screen.

```
  T0   T1   T2   T3   T4   T5   T6   T7   MST
  ██   ██   ██   ██   --   --   --   --   ██
  ██   ██   ██   --                       REV ██
  ██   --   --                            DEL ██
  80   80   80   80   80   80   80   80   80
```

Each track column shows a peak meter and a volume value (`00`–`FF`, `80` = 0 dB / unity).

The **master column** has two additional rows above the volume:
- **REV** — return gain for the reverb send bus (`00`–`FF`)
- **DEL** — return gain for the delay send bus (`00`–`FF`)

### Volume scale

| Value | Level |
|---|---|
| `00` | Silent |
| `80` | Unity (0 dB) |
| `FF` | Maximum (+6 dB) |

### Meter zones

| Color | Range |
|---|---|
| Red | ≥ 0 dBFS (clipping) |
| Yellow | −6 dBFS to 0 dBFS |
| Green | below −6 dBFS |

The master column also has stereo send peak meters showing REV and DEL bus levels.

### Controls

| Input | Action |
|---|---|
| D-pad LEFT/RIGHT | Select track / column |
| D-pad UP/DOWN | Move between rows (track volume, or REV/DEL/VOL in master) |
| A + UP/DOWN | Increase / decrease value by 1 |
| A + LEFT/RIGHT | Increase / decrease value by 16 |
| SELECT (on any row) | Open EQ EDITOR for that track |

---

## 16. EFFECTS Screen

The EFFECTS screen configures the global stereo send buses (reverb and delay) and selects the master bus effect.

Navigate here: **R+DOWN** from MIXER, or **R+DOWN** twice from any Row 2 screen.

### Reverb section

| Parameter | Description |
|---|---|
| SIZE | Room size (`00`–`FF`). Higher = longer reverb tail. |
| DAMP | High-frequency damping (`00`–`FF`). Higher = darker reverb. |
| EQ | Press SELECT to open the EQ EDITOR for the reverb return. |

The reverb return volume is set on the MIXER screen (REV row in master column).

### Delay section

| Parameter | Description |
|---|---|
| TIME | Delay time (`00`–`FF`) in musical subdivisions. |
| FDBK | Feedback amount (`00`–`FF`). Higher = more repeats. |
| REV | Amount of delay output sent into the reverb bus (`00`–`FF`). Delay is processed first, so this cross-routing is zero-latency. |
| EQ | Press SELECT to open the EQ EDITOR for the delay return. |

The delay return volume is set on the MIXER screen (DEL row in master column).

### Master bus

| Parameter | Description |
|---|---|
| FX | Select master bus effect: OTT (3-band compressor) or DUST (lo-fi chain). |
| DEPTH | Wet/dry depth of the selected effect (`00` = bypass, `FF` = full). |

Per-instrument effects (filter, drive, crush) are set on the INSTRUMENT screen.

---

## 17. EQ EDITOR

The EQ EDITOR is a full-screen overlay that opens when you press **SELECT** on an EQ row in the INSTRUMENT, MIXER, or EFFECTS screens.

The EQ applies a 3-band parametric equalizer (biquad filter, per the Audio EQ Cookbook). A real-time spectrum analyzer (KissFFT, ~20 fps) shows the master output with the computed frequency response curve overlaid.

### Layout

- **Top row** — EQ slot, calling context, hint
- **Center** — real-time spectrum + frequency response curve
- **Bottom third** — 3-column band editor (one column per band)

Each band has 4 parameters: TYPE, FREQ, GAIN, Q.

### Band types

| Type | Description |
|---|---|
| PEAK | Boost or cut at FREQ with width Q |
| LOW SHELF | Shelving EQ below FREQ |
| HIGH SHELF | Shelving EQ above FREQ |
| LP | Low-pass filter at FREQ |
| HP | High-pass filter at FREQ |
| NOTCH | Notch (band-reject) at FREQ |
| OFF | Bypass this band |

### Controls

| Input | Action |
|---|---|
| D-pad LEFT/RIGHT | Switch between bands (1–3) |
| D-pad UP/DOWN | Move between parameters (TYPE, FREQ, GAIN, Q) |
| A + UP/DOWN | Edit value |
| A + LEFT/RIGHT | Edit value (large step) |
| B | Close EQ EDITOR and apply changes |

---

## 18. PROJECT Screen

The PROJECT screen contains global settings and file operations.

Navigate here: **R+UP** from SONG or CHAIN.

### Settings

| Parameter | Description |
|---|---|
| NAME | Project name (up to 12 characters). A+UP/DOWN cycles characters. |
| TEMPO | BPM. |
| TRANSPOSE | Global semitone offset applied to all tracks. |

### File operations

| Row | Action |
|---|---|
| SAVE | Save project to `.ptp` file (press A to confirm). |
| LOAD | Open file browser to load a project. |
| WAV MIX | Render full song to stereo WAV (offline, faster than real-time). |
| CLEAN SEQ | Remove unused chains and phrases (with confirmation dialog). |
| CLEAN INST | Remove unused instruments (with confirmation dialog). |
| SETTINGS | Open the SETTINGS screen (press A). |

WAV exports are saved to `/Documents/PocketTracker/Renders/` with auto-incremented filenames (`ProjectName_0001.wav`).

---

## 19. SETTINGS Screen

The SETTINGS screen is opened from the PROJECT screen (cursor on SETTINGS row, press A).

| Setting | Options | Description |
|---|---|---|
| LAYOUT | FULL / T.PORT / T.LAND / T.PORT2 | UI layout mode. FULL = no virtual controls. Touch modes add on-screen buttons. |
| SCALING | INTEGER / BILINEAR / NEAREST | Screen scaling algorithm. INTEGER = crisp pixel-perfect. |
| BTN SOUND | ON / OFF | Play a click sound on button press. |
| BTN VOL | 00–FF | Click sound volume. |
| VIBRATION | ON / OFF | Haptic feedback on button press. |
| KB INSERT | ON / OFF | QWERTY keyboard insert mode (for name fields). |
| CURSOR MEM | ON / OFF | Remember cursor position when switching between screens. |

Layout and scaling mode are persisted across app restarts. The auto-detected layout on startup depends on whether physical gamepad buttons are detected.

---

## 20. Effects Reference

Effects are placed in the **FX1**, **FX2**, and **FX3** columns of a phrase step, or in the FX columns of a table row. Each has a 3-letter code and a 2-digit hex value.

Effects persist until cancelled (new note on same track, new effect in same column, or KIL), unless stated otherwise.

---

### ARP `XX` — Arpeggio

Rapid cycling through multiple pitches to simulate chords.

`XX` encodes two interval offsets: high nibble = first interval (semitones), low nibble = second interval.

| Value | Pattern | Sound |
|---|---|---|
| `37` | root, +3, +7 | Minor chord |
| `47` | root, +4, +7 | Major chord |
| `4B` | root, +4, +11 | Major 7th |
| `3A` | root, +3, +10 | Minor 7th |
| `CC` | root, +12, +12 | Octave doubling |
| `00` | (cancel) | Stop arpeggio |

Persists across steps. Configure with **ARC**.

---

### ARC `XX` — Arpeggio Config

- High nibble = mode: `0`=UP, `1`=DOWN, `2`=PINGPONG, `3`=RANDOM
- Low nibble = speed in ticks (`4` = default)

---

### CHA `XY` — Chance

Probability gate. Rolls a random number each time the step plays.

- `X` = probability for note + FX to the left to play (`0`=never, `F`=always)
- `Y` = probability for FX to the right to play

---

### DEL `XX` — Delay

Delays the step by `XX` ticks. All events on that row trigger later than normal.

---

### GRV `XX` — Groove Assign

Switches the current track to use groove `XX` from this step onward.

---

### HOP `XY` — Hop / Jump

- In a **phrase**: jumps to phrase step `Y`, limited to `X` times before falling through.
- In a **table**: jumps to table row `Y`.

`HOP 00` at the end of a section = infinite loop of that section.

---

### KIL `00` — Kill

Immediately stops the sample on this track and cancels all persistent effects (ARP, REP, pitch effects).

---

### OFF `XX` — Offset

Jumps the sample playback start point to offset `XX` (fraction of total length).

---

### PSL `XX` — Pitch Slide (Portamento)

Slides pitch from the previous note to the current note over `XX` ticks. `PSL 00` = instant.

---

### PBN `XX` — Pitch Bend

Continuous pitch bend. Persists until cancelled.

- `00`–`7F` = bend UP (higher = faster)
- `80`–`FF` = bend DOWN (`80` from the top = fastest downward)
- `PBN 00` = cancel

---

### PVB `XY` — Vibrato

- `X` = speed (`0`–`F`)
- `Y` = depth (`0`–`F`, up to ~1.9 semitones)

Persists until `PVB 00`, new note, or KIL.

---

### PVX `XY` — Extreme Vibrato

Same as PVB but 4× deeper and 2× faster.

---

### REP `XY` — Repeat (Retrigger)

- **Y = 0 (simple mode):** retrigger every `X` ticks.
- **Y ≠ 0 (volume ramp mode):** retrigger every `Y` ticks.
  - `X` 1–7: decrease volume each retrig (fade-out)
  - `X` 8–F: increase volume each retrig (fade-in)

Persists across steps. Cancel with new note, new FX in same column, or KIL.

---

### RND `XY` — Randomize

Randomizes the **previously active FX** value on this track.

- `X` = downward range, `Y` = upward range

---

### RNL `XY` — Randomize Left

Randomizes the FX value in the column immediately to the left. Same `X`/`Y` semantics as RND.

---

### TBL `XX` — Table Set

Overrides the instrument's default table, using table `XX` for this note.

---

### THO `XX` — Table Hop

Jumps the table playhead to row `0X`. `THO 00` = loop current section.

---

### TIC `XX` — Tick Rate

Sets the table tick rate:

- `TIC 0C` = default (one table row per phrase step)
- `TIC 06` = twice as fast
- `TIC FC` = ping-pong
- `TIC FE` = random row each tick
- `TIC FF` = stop table

---

### VOL `XX` — Volume Automation

Sets the step volume to `XX` at the exact tick this command fires. Useful in table rows for volume animation.

---

## 21. Modulation Reference

See §14 for how to edit mod slots.

### AHD Envelope

One-shot envelope triggered on each note. `AMT` controls how much the destination is affected.

```
     ┌──────┐
     │      │
  ATK│ HOLD │DEC
     │      └────
─────┘            ─ (returns to base)
```

- **ATK** — attack time in ticks (0 = instant)
- **HOLD** — hold duration at peak
- **DEC** — decay time to zero

### ADSR Envelope

Release is triggered when the note ends (voice steal or KILL).

- **ATK** — attack time
- **DEC** — decay to sustain level
- **SUS** (`00`–`FF`) — sustain level
- **REL** — release time after note off

### DRUM Envelope

Percussive shape: transient → body → tail. Identical stage machine to AHD; the name signals intent.

- **ATK** — transient attack time (typically `00`)
- **HOLD** — body duration ("thud")
- **DEC** — tail decay

### LFO

Cyclic modulation. Phase resets to 0 on each new note (RETRIG mode).

- **OSC** — shape: TRI, SIN, RMP+, RMP−, EXP+, EXP−, SQU+, SQU−, RANDOM, DRUNK
- **FREQ** (`00`–`FF`) — rate (~0.08 Hz at `00`, ~20 Hz at `FF`)

### TRIG Envelope

Currently behaves identically to ADSR — same ATK/DEC/SUS/REL parameters. The planned feature (triggering from an external instrument or track source) is deferred to post-MVP.

---

## 22. File Management

### Project files

- Format: `.ptp` (JSON with version field; old projects are migrated automatically on load)
- Location: `/Documents/PocketTracker/Projects/`
- Save: PROJECT screen → SAVE
- Load: PROJECT screen → LOAD

### Sample files

- Format: `.wav` (8/16/24/32-bit PCM or float; mono or stereo)
- Stereo WAV files are supported natively — SOURCE mode on the instrument or sample editor selects LEFT / RIGHT / STEREO / MONO non-destructively
- Sample rates: any — PocketTracker compensates pitch for non-44100 Hz files
- Loaded via: INSTRUMENT screen → SAMPLE field → A button → file browser
- SF2 files are loaded the same way

### WAV exports

- Format: 16-bit stereo WAV, 44100 Hz
- Location: `/Documents/PocketTracker/Renders/`
- Filenames: `ProjectName_0001.wav`, `_0002.wav`, … (auto-incremented)
- Triggered from: PROJECT screen → WAV MIX

### Resampled instruments

Created via Selection Resampling:

1. On the SONG screen, enter selection mode (**L+B**).
2. Select the rows and tracks you want to render.
3. **Double-tap A** — a confirmation dialog appears.
4. Choose YES — selected tracks render offline to a WAV file.
5. A new instrument is created in the first empty slot with the rendered audio.

Output: `/Documents/PocketTracker/Samples/Resampled/Resample_0001.wav`, …

### CHOP exports

When slice markers are set in the SAMPLE EDITOR, use CHOP to export each slice as a separate WAV:

Output: `/Documents/PocketTracker/Samples/Chops/{instrument_name}/`

---

## 23. Workflow Tips

### Making your first phrase

1. Open **PHRASE** (R+RIGHT from SONG).
2. Cursor on row 00, column N. Press **A** — inserts default note (C-4, instrument 00).
3. Use **A+UP/DOWN** to change pitch; **A+LEFT/RIGHT** to change octave.
4. Move to the I column, use **A+UP/DOWN** to select an instrument.
5. Press **START** to hear the phrase loop.

### Building a basic beat

1. Load a kick on instrument `00`, snare on `01`, hihat on `02`.
2. In PHRASE `00`: place kick at steps 00, 04, 08, 0C; snare at 04, 0C; hihat at every even step.
3. Create CHAIN `00` pointing to PHRASE `00`.
4. In SONG, put CHAIN `00` on track 0, row 00. Hit **START**.

### Transpose in chains

Rather than duplicating a phrase at a different pitch, set the TRN column in the CHAIN. `07` = +7 semitones (a perfect fifth). The same phrase plays higher with no copy.

### Swing with groove

1. Navigate to GROOVE (R+UP from PHRASE).
2. Create groove `01`: rows `0E`, `0A`, `--`.
3. In your phrase, add `GRV 01` in an FX column of step 00.
4. The track now swings.

### Modulation: Volume fade-in on a pad

1. Open INSTRUMENT for your pad, then MODS (R+UP).
2. MOD1: TYPE=AHD, DEST=VOLUME, ATK=`40`, HOLD=`20`, DEC=`60`.
3. Every note now has a volume envelope shaping its amplitude.

### Using the sample editor for sliced breaks

1. Load a drum loop WAV on an instrument.
2. Press SELECT on the SAMPLE field to open SAMPLE EDITOR.
3. Set SLICE mode to TRANSIENT, adjust SENS until markers land on drum hits.
4. Press SAVE / OVERWRITE to embed the cue chunk in the WAV file.
5. Back on INSTRUMENT screen, set SLICE = CUT.
6. Now each note in a phrase selects a different slice — C-4 = hit 0, C#4 = hit 1, etc.

### WAV export

1. Build your complete song.
2. Navigate to PROJECT screen.
3. Move cursor to WAV MIX and press **A**. The song renders offline — a status message shows the output filename when done.

---

## Appendix A: Hexadecimal Quick Reference

| Decimal | Hex | Decimal | Hex |
|---|---|---|---|
| 0 | `00` | 128 | `80` |
| 16 | `10` | 160 | `A0` |
| 32 | `20` | 192 | `C0` |
| 64 | `40` | 224 | `E0` |
| 96 | `60` | 255 | `FF` |

Half of `FF` (full) = `80` (center / unity). Default for VOL and PAN.

---

## Appendix B: Note Names

```
C  C# D  D# E  F  F# G  G# A  A# B
00 01 02 03 04 05 06 07 08 09 0A 0B
```

Middle C = `C-4` = MIDI note 60. Full range: `C-0` to `B-9`.

---

## Appendix C: Instrument Slots

All 256 slots (00–FF) start empty in a new project. There are no bundled default samples. Slots without a loaded sample play silence. Slot names are auto-generated as `INST00`–`INSTFF`.

---

## Known Issues

- Generic input warning may appear in the log after device restart — harmless, disappears after a reboot.

---

*PocketTracker is open-source (GPL-3.0). Contributions and bug reports welcome.*
