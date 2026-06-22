# PocketTracker User Manual

**Version:** 1.2  
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
20. [THEME EDITOR](#20-theme-editor)
21. [Effects Reference](#21-effects-reference)
22. [Modulation Reference](#22-modulation-reference)
23. [File Management](#23-file-management)
24. [Workflow Tips](#24-workflow-tips)
25. [Appendix A: Hex Quick Reference](#appendix-a-hexadecimal-quick-reference)
26. [Appendix B: Note Names](#appendix-b-note-names)
27. [Appendix C: Instrument Slots](#appendix-c-instrument-slots)
28. [Appendix D: Controls Cheat Sheet](#appendix-d-controls-cheat-sheet)
29. [Appendix E: Parameter Reference — units & ranges](#appendix-e-parameter-reference--units--ranges)

---

## 1. Introduction

PocketTracker is a sample-based music tracker for Android handhelds and budget Android devices. It is inspired by M8, LSDJ, and Little GP Tracker (LGPT/Picotracker), and designed to run natively at **640×480** — the resolution of devices like the Miyoo Flip.

**New to trackers?** A tracker is a music sequencer where notes are arranged in a grid, with time flowing downward. Each row is a step, each column carries a parameter (note, instrument, volume, effect). Songs are built by chaining together short patterns called **phrases**, grouped into **chains**, arranged in a **song**. Think of it as building music from small, reusable blocks.

PocketTracker stores everything in a single project file (`.ptp`). Sounds come from standard `.wav` files or **SoundFont (SF2)** files that you load yourself — there are no bundled samples.

### Minimum requirements

- **Android** 8.0 (API 26) or later
- **RAM:** 512 MB
- **Screen:** 640×480 or larger
- **Controls:** Physical buttons or touchscreen

---

## 2. Installation

1. Download the latest `.apk` from the project releases page.
2. On your Android device, enable **Install from unknown sources** in Settings → Security.
3. Open the downloaded `.apk` and tap **Install**.
4. On first launch the app may ask for **All Files Access** permission (Android 11+). Grant it — this is required to read and write project and sample files.

> [!IMPORTANT]
> If you deny the All Files Access permission, PocketTracker cannot load samples, save projects, or export WAV files. If you accidentally denied it, go to Android Settings → Apps → PocketTracker → Permissions and grant it manually.

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
│  VISUALIZER  (620×70 px)                       │
├──────────────────────────────┬─────────────────┤
│                              │  NAV MAP        │
│  MAIN EDITOR                 │  (80×105 px)    │
│  (varies by screen)          │                 │
│                              │  STATUS LINE    │
│                              │                 │
└──────────────────────────────┴─────────────────┘
```

**Visualizer** — the top bar displays real-time audio. It has six display modes you can switch in SETTINGS:

| Mode | What it shows |
|---|---|
| SCOPE | Classic oscilloscope waveform (ProTracker-style pixel dots) |
| FLAT | Blank bar (saves battery / CPU) |
| OCTA | Mini-scopes side by side, one per active track |
| OCTA.F | All 8 track scopes at once (active or not) |
| SPECT | 40-bin FFT spectrum |
| SPCT.P | FFT spectrum with peak-hold dots |

**Main editor** — the active screen (PHRASE, CHAIN, SONG, etc.).

**Navigation map** — a miniature 5×5 grid in the top-right showing your position in the screen layout.

**Status line** — brief messages (e.g., `SAVED`, `RESAMPLED TO INST 0C`) that auto-dismiss after a few seconds.

Some screens (SAMPLE EDITOR, EQ EDITOR, THEME EDITOR) open as full-screen overlays that temporarily replace the main layout.

> [!TIP]
> If you're on a low-power device and notice audio hiccups, switch the visualizer to **FLAT** — it disables the real-time waveform rendering and frees up CPU for the audio engine.

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

> **SCALE** and **INST POOL** (Row 0) are reserved for future features and not yet active.  
> **GROOVE** and **MODS** (Row 1) only appear in column 2 and 3 respectively.  
> **MIXER** and **EFFECTS** (rows 3–4) only appear in the column you are currently on — move left/right first, then navigate up/down.

| Combo | Action |
|---|---|
| R + RIGHT / LEFT | Move left / right along Row 2 |
| R + UP | Move to the screen above in the current column |
| R + DOWN | Move to the screen below in the current column |

The navigation map always shows where you are.

**Popup screens** — not in the grid, opened contextually:

| Screen | How to open |
|---|---|
| SAMPLE EDITOR | INSTRUMENT screen → cursor on SAMPLE → SELECT |
| EQ EDITOR | INSTRUMENT / MIXER / EFFECTS screen → cursor on EQ row → SELECT |
| SETTINGS | PROJECT screen → cursor on SETTINGS row → A |
| THEME EDITOR | SETTINGS screen → cursor on THEME row → A |

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
| SELECT | Context action |
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

> [!TIP]
> Pressing **A** on an empty note cell re-inserts the last note you placed — same pitch, same instrument. This is the fastest way to place a drum pattern: move to the row, press A, move on.

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

### 5.4 Context Navigation — B + D-pad

Hold **B** and press LEFT/RIGHT to switch between items of the same type without leaving the screen:

| Screen | B + LEFT / RIGHT |
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

> [!TIP]
> Use **SCREEN** selection mode to duplicate an entire phrase or chain quickly: enter SCREEN mode → B to copy → navigate to an empty phrase/chain → L+A to paste.

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
- The **SONG** arranges chains across 8 tracks. Each song row plays all 8 tracks simultaneously.

All values (chain IDs, phrase IDs, instrument IDs, etc.) are hexadecimal, ranging from `00` to `FF`.

> [!NOTE]
> All values in PocketTracker are **hexadecimal** (base-16). Decimal 16 = hex `10`, decimal 255 = hex `FF`. See Appendix A for a quick conversion table.

---

## 7. SONG Screen

The SONG screen arranges chains across 8 tracks. Each column is a track (1–8), each row is a song position.

```
     1    2    3    4    5    6    7    8
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

> [!TIP]
> You can start playback from any row — not just the beginning. Move the cursor to the row where you want playback to start, then press **START**. Useful for jumping to a specific section while mixing.

---

## 8. CHAIN Screen

A chain is a sequence of up to 16 phrase references. Each slot has:
- **PH** — phrase ID (`00`–`FF`) or `--` (empty)
- **TSP** — transpose in semitones (`00` = no transpose; values above `7F` are negative)

```
     PH   TSP
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
| A + LEFT/RIGHT | ±16 (PH) or ±12 semitones (TSP) |
| A + B | Delete slot |
| B + LEFT/RIGHT | Switch to previous / next chain |
| START | Play current chain |

> [!TIP]
> Use **TSP** to play the same phrase at multiple pitches without copying it. One phrase can become a verse, chorus, and bridge by giving it different TSP values across chain slots — `07` = +7 semitones (a perfect fifth up), `0C` = +12 (one octave up).

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
| V | Volume (`00`–`FF`). Always set — `FF` = full, applied on top of instrument VOL. |
| I | Instrument ID (`00`–`FF`). Always set — no empty state. |
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
| B + LEFT/RIGHT | Switch to previous / next phrase |
| START | Play current phrase (loops) |

### FX columns

Each FX slot has two parts: **type** (3-letter code) and **value** (2-digit hex). Use A+UP/DOWN on the type to cycle through available effects. Effects are listed in §21.

> [!WARNING]
> Some effects (**ARP**, **REP**, **PBN**, **PVB**, **PVX**) **persist across steps that have no note** — they keep running on empty rows. They are cancelled by: a new note on the same track, any effect in the same FX column, setting the effect to `00`, or **KIL**.

---

## 10. INSTRUMENT Screen

The INSTRUMENT screen configures how a sample or SF2 preset is played.

Navigate here with **R+RIGHT** from PHRASE. Use **B+LEFT/RIGHT** to switch between instruments without leaving the screen.

### WAV instrument parameters

| Parameter | Range | Description |
|---|---|---|
| NAME | — | Instrument name. |
| SAMPLE | path | WAV or SF2 file. Press A to open file browser; SELECT to open SAMPLE EDITOR. |
| ROOT | C-0 – B-9 | The pitch of the sample as recorded. |
| DETUNE | 00–FF | Fine tuning. `80` = center (no detune). |
| VOL | 00–FF | Base volume. `FF` = full. |
| PAN | 00–FF | Stereo pan. `00` = full left, `80` = center, `FF` = full right. |
| START | 00–FF | Sample start point (fraction of sample length). |
| END | 00–FF | Sample end point. |
| LOOP | OFF / FWD / PNG | Loop mode: off, forward, ping-pong. |
| LOOP ST | 00–FF | Loop start point (fraction of sample length). |
| LOOP END | 00–FF | Loop end point. `FF` = sample end. Loop region is [LOOP ST, LOOP END] — see below. |
| REVERSE | OFF / ON | Reverse playback. |
| SLICE | OFF / CUT / TRU | Slice playback mode (see below). |
| FILTER | LP / HP / BP / OFF | Resonant SVF filter type. |
| CUT | 00–FF | Filter cutoff frequency. `FF` = open. |
| RES | 00–FF | Filter resonance. `00` = none. |
| DRIVE | 00–FF | Soft-clipping overdrive. `00` = off. |
| CRUSH | 00–FF | Bit-depth crusher. `00` = off. |
| EQ | — | Press SELECT to open the EQ EDITOR for this instrument. |

> [!TIP]
> **ROOT** is the most important tuning parameter. Set it to the actual pitch of your sample (e.g., `A-4` for a 440 Hz sine). If notes sound in the wrong octave, ROOT is usually the reason.

> [!TIP]
> **DRIVE** and **CRUSH** are subtle at low values (`10`–`30`) and very aggressive near `FF`. Both are per-instrument — start low and increase by ear.

### SF2 instrument parameters

When the loaded file is an SF2 SoundFont, additional override fields appear for the preset's internal envelope and filter. Setting these to `--` uses the SF2's built-in values.

| Parameter | Description |
|---|---|
| ATK | Envelope attack time override |
| DEC | Envelope decay time override |
| SUS | Envelope sustain level override |
| REL | Envelope release time override |
| CUT | Filter cutoff override |
| RES | Filter resonance override |

### Volume chain

Volume is applied in this order:

```
Instrument VOL × Phrase V column × Track volume (Mixer) × Master volume (Mixer)
```

> [!NOTE]
> If a note is unexpectedly silent, check all four stages of the volume chain: instrument VOL, phrase V column, track volume in MIXER, and master volume. Any one of them being `00` will silence the output.

### Slice playback

When slice markers exist on the sample (set via the SAMPLE EDITOR), the SLICE parameter controls how notes select slices:

| Mode | Behaviour |
|---|---|
| OFF | Normal pitch playback — markers are ignored. |
| CUT | Note pitch selects a slice (C-4 relative to ROOT = slice 0). Plays from slice start to the next marker, then stops. |
| TRU | Same slice selection; plays from slice start to end of sample. |

### Loop region & release tail

When **LOOP** is FWD or PNG, playback flows **START → LOOP END** once, then repeats the region **[LOOP ST, LOOP END]**. The sample's **END** no longer bounds the loop — it bounds the *release tail*.

If the instrument also has an **ADSR** volume envelope (MODULATION screen), releasing the note — the note-off at the end of its step, or a **KIL** (`K00`) effect — leaves the loop and plays **LOOP END → END** once as the release tail, under the ADSR release stage. The sample therefore splits into three regions:

| Region | Role |
|---|---|
| START → LOOP ST | Intro — played once. |
| LOOP ST → LOOP END | Sustain loop — repeats while the note is held. |
| LOOP END → END | Release tail — played once on note-off (ADSR only). |

Without an ADSR envelope the loop repeats indefinitely until the voice is killed or stolen, and the release tail is never used.

> [!NOTE]
> Set **LOOP END** below **END** to reserve a release tail. Leaving **LOOP END = FF** makes the loop run to the sample end (the classic behaviour) with no separate tail.

### Navigating instruments

- **B + LEFT/RIGHT** — switch between instruments 00–FF
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

> [!WARNING]
> **UNDO** reverts only the **last single operation** — it is not a full history. If you apply NORMALIZE then FADE OUT, pressing UNDO reverts only the FADE OUT.

> [!WARNING]
> **OVERWRITE** writes to disk immediately and **cannot be undone**. Once you overwrite, the previous file content is permanently gone. Use **SAVE** (which creates a new numbered file) while experimenting, and only OVERWRITE when you are certain of the result.

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
| DRIVE | Soft-clipping overdrive. |
| OTT | 3-band bidirectional compressor. |

> [!TIP]
> The DUST offline FX is a great one-click "make it lo-fi" button for drum samples. Apply it to a clean break, then SAVE to a new file — you preserve the original and get the processed version as a separate instrument.

### Transient detection and slices

The SLICE row controls how slice markers are managed:

| Mode | Behaviour |
|---|---|
| OFF | Show existing WAV cue markers (read-only). No detection. |
| TRANSIENT | Run spectral-flux transient detection. SENS (`00`–`FF`) controls threshold. |
| DIVIDE | Divide the sample into equal slices. |

Slice markers are stored in the WAV `cue ` chunk — compatible with M8, Blackbox, Reaper, Logic, and Adobe Audition.

**CHOP** — exports each slice as a separate WAV file to `Samples/Chops/{name}/`.

> [!TIP]
> After using CHOP, you need to manually load the individual slice files onto new instrument slots. There is no automatic batch-load — but using numbered filenames (which CHOP generates) makes the process fast.

### Save

| Action | Description |
|---|---|
| SAVE | Write to a new file (auto-incremented name). |
| OVERWRITE | Replace the existing file on disk. Applies SOURCE at write time (LEFT/RIGHT = mono export from that channel; STEREO = 2-channel WAV; MONO = averaged mono). |

---

## 12. TABLE Screen

A table is a 16-row micro-sequencer attached to an instrument. When a note plays, the table runs in parallel at a configurable tick rate, applying per-row transpose, volume, and effects.

Tables are great for: drum rolls, note slides, automatic arpeggios, per-note automation.

```
     N    VOL  FX1      FX2      FX3
00   00   --   ---  00  ---  00  ---  00
01   03   --   ---  00  ---  00  ---  00
02   07   --   ---  00  ---  00  ---  00
```

| Column | Range | Description |
|---|---|---|
| N | 00–FF | Transpose in semitones. `00` = no shift. Values above `7F` are negative (e.g., `FC` = −4 st). |
| VOL | 00–FF / -- | Volume multiplier for this row. `--` = no change. |
| FX1–FX3 | same as phrase | Effects applied on this table tick. |

### TIC rate

The header shows **XX TIC** — how many phrase ticks pass per table row. Use the **TIC** phrase effect to change this value.

- Default: `06` (6 ticks per row — two rows per phrase step)
- Lower values = table advances faster
- Special values: `00` = trigger mode, `FC` = octave map, `FE` = note map, `FF` = 200Hz mode

> [!TIP]
> Use **HOP** in the last row of a table section to loop just part of the table. For example: rows 00–03 with `HOP 00` in row 03 will loop those 4 rows indefinitely, ignoring rows 04–0F.

> [!NOTE]
> By default, instrument N uses table N. To override this per-note, place a **TBL XX** effect in the phrase FX column. The table switches immediately and stays active for subsequent notes on that track.

### Table–instrument link

By default, instrument N uses table N. Override per-note with the **TBL** phrase effect.

### Controls

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A + UP/DOWN | Edit value |
| A + B | Delete value |
| B + LEFT/RIGHT | Previous / next table |
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

> [!TIP]
> To reset a track back to even timing after a groove section, place `GRV 00` in an FX column. Groove `00` defaults to `0C` per step, which is perfectly even.

> [!WARNING]
> For the grooved track to stay in sync, each complete cycle of your groove list should **average 12 ticks per entry**. A 2-entry swing sums to 24 (14+10 ✓). A 4-entry groove should sum to 48. If the average is not 12, the track will drift against un-grooved tracks over time.

### Controls

| Input | Action |
|---|---|
| D-pad UP/DOWN | Move between rows |
| A + UP/DOWN | Edit tick value |
| A + LEFT/RIGHT | Edit tick value (large step) |
| A + B | Clear row |
| B + LEFT/RIGHT | Previous / next groove |

---

## 14. MODULATION Screen

The MODULATION screen (MODS) adds up to **4 modulation slots** per instrument. Each slot runs an envelope or LFO targeting a destination parameter — great for volume shapes, pitch vibrato, filter sweeps, and more.

Navigate here: **R+UP** from INSTRUMENT (column 3).

### Modulation types

| Type | Description |
|---|---|
| `---` | Off |
| AHD | One-shot envelope: Attack → Hold → Decay |
| ADSR | Envelope: Attack → Decay → Sustain → Release |
| DRUM | Percussive envelope: sharp peak → body hold → tail decay |
| LFO | Cyclic oscillator |
| TRIG | Envelope triggered by note, behaves like ADSR |

### Modulation destinations

| Dest | Affects |
|---|---|
| VOLUME | Amplitude |
| PAN | Stereo position |
| PITCH | Pitch in semitones |
| FINE | Fine pitch (same range as PITCH) |
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

### Mod-to-mod routing

When DEST is **MOD AMT**, **MOD RATE**, or **MOD BOTH**, the slot modulates the next slot (circular: slot 4 → slot 1).

Example: MOD1 (LFO) with DEST=MOD AMT targeting MOD2 (AHD) — the LFO rhythmically swells the envelope depth.

> [!TIP]
> Mod-to-mod routing is **circular** — slot 4 targets slot 1. Plan your slot order before setting up complex chains: the modulator should always be a lower-numbered slot than its target, except for the wraparound case.

> [!TIP]
> **LFO RETRIG** mode resets the phase on every new note, giving a predictable and consistent modulation shape on each hit. Use **FREE** only when you want the LFO to drift independently of your notes — useful for slow pad movement but unpredictable on drums.

### Controls

| Input | Action |
|---|---|
| D-pad UP/DOWN | Move between parameters |
| D-pad LEFT/RIGHT | Switch between paired slots (MOD1↔MOD2 or MOD3↔MOD4) |
| A + UP/DOWN | Edit value |
| A + LEFT/RIGHT | Edit value (large step) |
| A + B | Reset to default |
| B + LEFT/RIGHT | Previous / next instrument |

---

## 15. MIXER Screen

The MIXER screen shows all 8 tracks plus a master column with real-time dBFS peak meters. This is where you balance levels, control reverb/delay return volumes, and open per-track EQs.

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

> [!WARNING]
> Red meters mean the master limiter is working hard. The output won't clip, but heavy limiting can colour the sound. Lower individual track volumes to give the limiter more headroom.

> [!TIP]
> Start all tracks at `80` (unity), balance them by ear, then bring the master down if needed. It's easier to level-match tracks at unity than to compensate after boosting everything.

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
| TIME | Delay time. **SYNC off:** free time `00`–`FF` = 0–2000 ms. **SYNC on:** `00`–`0B` selects a BPM-locked subdivision (1/1 … 1/16.). See [Appendix E](#appendix-e-parameter-reference--units--ranges). |
| FDBK | Feedback amount (`00`–`FF`). Higher = more repeats. |
| REV | Amount of delay output sent into the reverb bus (`00`–`FF`). Delay is processed before reverb, so this cross-routing is zero-latency. |
| EQ | Press SELECT to open the EQ EDITOR for the delay return. |

The delay return volume is set on the MIXER screen (DEL row in master column).

> [!TIP]
> Setting **Delay REV** above `00` feeds the delay output into the reverb — this creates a "delay into reverb" effect popular in ambient, dub, and post-rock music. Start around `40` and adjust to taste.

> [!WARNING]
> **FDBK** values near `FF` create near-infinite delay tails that can clip the output. Start around `60`–`80` and increase carefully while listening to the master meter.

### Master bus

| Parameter | Description |
|---|---|
| FX | Select master bus effect: OTT (3-band compressor) or DUST (lo-fi chain). |
| DEPTH | Wet/dry depth of the selected effect (`00` = bypass, `FF` = full). |

Per-instrument effects (filter, drive, crush) are set on the INSTRUMENT screen.

---

## 17. EQ EDITOR

The EQ EDITOR is a full-screen overlay that opens when you press **SELECT** on an EQ row in the INSTRUMENT, MIXER, or EFFECTS screens.

It applies a 3-band parametric equalizer (biquad filter, per the Audio EQ Cookbook). A real-time spectrum analyzer (KissFFT, ~20 fps) shows the signal relevant to the current EQ context — instrument output when opened from INSTRUMENT, delay bus from EFFECTS delay, reverb bus from EFFECTS reverb, or master bus from MIXER/master — with the computed frequency response curve overlaid.

### Layout

- **Top row** — EQ slot, calling context, hint
- **Center** — real-time spectrum + frequency response curve
- **Bottom third** — 3-column band editor (one column per band)

Each band has 4 parameters: TYPE, FREQ, GAIN, Q.

### Parameters

| Param | Range | Unit | Notes |
|---|---|---|---|
| TYPE | — | — | Band shape (see table below). |
| FREQ | `00`–`FF` | 20 Hz – 20 kHz (log) | A single A+UP/DOWN step always changes the displayed Hz. |
| GAIN | `00`–`F0` | **−12.0 … +12.0 dB** | Small step **0.1 dB**, large step **1.0 dB**. `0.0 dB` is the default. |
| Q | `00`–`FF` | 0.1 – 10.0 (log) | Bandwidth; higher = narrower. |

See [Appendix E](#appendix-e-parameter-reference--units--ranges) for the full mapping.

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
| A + UP/DOWN | Edit value (small step — GAIN: ±0.1 dB) |
| A + LEFT/RIGHT | Edit value (large step — GAIN: ±1.0 dB) |
| A + B | Reset parameter to default (FREQ mid · GAIN 0 dB · Q mid) |
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

> [!WARNING]
> **CLEAN SEQ** and **CLEAN INST** are **permanent** — there is no undo. Save your project before running them, in case you remove something you still needed.

> [!TIP]
> **WAV MIX** renders faster than real-time. A 3-minute song typically exports in a few seconds. The status line shows the output filename when done.

---

## 19. SETTINGS Screen

The SETTINGS screen is opened from the PROJECT screen (cursor on SETTINGS row, press A). Press **B** to return to PROJECT.

| Setting | Options | Description |
|---|---|---|
| LAYOUT | FULLSCREEN / T.PORT / TOUCH LANDSCAPE / AMIGA PORTRAIT | UI layout mode. FULLSCREEN = no virtual controls (physical buttons only). Touch layouts add on-screen buttons; AMIGA PORTRAIT is a themed retro skin for 20:9 phones. |
| SCALING | INT / BILINEAR | Screen scaling algorithm. INT = crisp pixel-perfect integer scaling. BILINEAR = smooth subpixel scaling. |
| BTN SOUND | ON / OFF | Play a click sound on button press. |
| BTN VOL | 00–FF | Click sound volume. |
| BTN VIBRO | ON / OFF | Haptic feedback on button press (where supported). |
| VIBRO POW | 00–FF | Vibration intensity. `FF` = strongest. |
| KB INSERT | BEFORE / AFTER | Where the QWERTY keyboard inserts characters in name fields. |
| CURSOR | REMEMBER / REFRESH | Whether cursor position is preserved when switching between screens. |
| NOTE PREV | ON / OFF | Play the note at its pitch when you insert it on the PHRASE screen — useful for hearing what you're placing without pressing START. |
| VISUALIZER | SCOPE / FLAT / OCTA / OCTA.F / SPECT / SPCT.P | Visualizer mode for the top bar (see §3 for descriptions). |
| THEME | theme name > | Shows the current theme name. Press A to open the THEME EDITOR. |
| TEMPLATE | SAVE / CLEAR | SAVE stores the current project as a template for new projects. CLEAR removes the saved template. |

Layout and scaling mode are persisted across app restarts. The auto-detected layout on startup depends on whether physical gamepad buttons are detected.

> [!TIP]
> **NOTE PREV** is especially useful when building melodies — you can hear each note as you place it without needing to start playback.

---

## 20. THEME EDITOR

The THEME EDITOR lets you customize the entire color scheme of the app, or switch between built-in themes. Open it from SETTINGS → THEME row → press A.

Press **B** to close and return to SETTINGS. All color changes apply immediately so you can see them live.

### Row 0 — THEME (built-in selection + SAVE/LOAD)

The top row lets you cycle through built-in themes and save or load custom themes.

| Cursor position (channel) | Action |
|---|---|
| 0 — theme name | A+UP/DOWN to cycle built-in themes: CLASSIC, AMBER, BLUE, MONO |
| 1 — SAVE | Press A to save the current theme to a `.ptt` file |
| 2 — LOAD | Press A to load a theme from a `.ptt` file |

Move between positions with D-pad LEFT/RIGHT.

### Rows 1–16 — Color parameters

Each row edits one color in the theme. The color preview swatch is shown on the right. Cursor moves between **R**, **G**, **B** channels with D-pad LEFT/RIGHT.

| Label | What it colors |
|---|---|
| BACKGROUND | Module fill and default row background |
| ROW 4TH | Beat-accent rows (every 4th step) |
| ROW CURSOR | The row the cursor is on |
| ROW PLAY | The currently playing step during playback |
| ROW SELECT | Selected region during copy/paste |
| TXT TITLE | Screen header text (e.g., "PHRASE", "INSTRUMENT") |
| TXT PARAM | Inactive parameter labels |
| TXT VALUE | Inactive parameter values |
| TXT CURSOR | Text on the cursor row |
| TXT EMPTY | Empty / placeholder cells |
| VIZ BG | Visualizer background |
| VIZ LINE | Visualizer center line |
| VIZ WAVE | Waveform / bar fill color |
| MTR LOW | Meter green zone (below −6 dBFS) |
| MTR MID | Meter yellow zone (−6 to 0 dBFS) |
| MTR HIGH | Meter red zone (≥ 0 dBFS) |

### Controls

| Input | Action |
|---|---|
| D-pad UP/DOWN | Move between color rows |
| D-pad LEFT/RIGHT | Move between R / G / B channels (on color rows), or between theme name / SAVE / LOAD (on row 0) |
| A + UP/DOWN | +1 / −1 to the selected channel |
| A + LEFT/RIGHT | +16 / −16 to the selected channel |
| B | Close theme editor |

### Built-in themes

| Name | Character |
|---|---|
| CLASSIC | Dark background, green wave, cyan headers, yellow cursor — the default look |
| AMBER | Warm amber/orange tones, reminiscent of an old CRT monitor |
| BLUE | Cool blue tones with bright cyan accents |
| MONO | Grayscale — pure black/white/grey, no color |

---

## 21. Effects Reference

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

> [!WARNING]
> ARP **persists** across steps that have no note. It is cancelled by: placing a new note on the same track, placing any effect in the same FX column, `ARP 00`, or **KIL**.

> [!TIP]
> Place **ARC** once at step 00 to configure the arpeggio mode and speed for the whole phrase. It only needs to be set once — subsequent steps just use the same ARC config.

---

### ARC `XX` — Arpeggio Config

- High nibble = mode: `0`=UP, `1`=DOWN, `2`=PINGPONG, `3`=RANDOM
- Low nibble = speed in ticks (`4` = default)

---

### CHA `XY` — Chance

Probability gate. Rolls a random number each time the step plays.

- `X` (high nibble) = probability (`0`=never, `F`=always)
- `Y` (low nibble) = target: `0`=note, `1`=FX1, `2`=FX2, `3`=FX3

CHA can appear in any FX column and gates any specific target independently of its own position.

> [!TIP]
> `CHA 82` anywhere on the step = ~53% chance FX2 fires. `CHA 40` = ~25% chance the note plays at all. Mix multiple CHA slots to gate different targets with different probabilities.

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

### PIT `XX` — Pitch Offset

Instantly shifts the pitch of the note by a signed number of semitones.

- `00`–`7F` = +0 to +127 semitones up
- `80`–`FF` = −128 to −1 semitones down (e.g., `FF` = −1 st, `F4` = −12 st)

Unlike PSL/PBN, PIT snaps the pitch immediately at note trigger. It does **not** affect which slice is selected when SLICE mode is active — use SLI for that.

Useful for: playing the same phrase at multiple pitch offsets without chain transpose, or layering detuned copies.

---

### PSL `XX` — Pitch Slide (Portamento)

Slides pitch from the previous note to the current note over `XX` ticks. `PSL 00` = instant.

---

### PBN `XX` — Pitch Bend

Continuous pitch bend. Persists until cancelled.

- `00`–`7F` = bend UP (higher = faster)
- `80`–`FF` = bend DOWN (`80` from the top = fastest downward)
- `PBN 00` = cancel

> [!WARNING]
> PBN **persists indefinitely** — the pitch will keep bending until `PBN 00` or a new note on the same track. An uncancelled bend will pitch the track up or down until it sounds completely wrong.

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

> [!WARNING]
> REP **persists** across steps that have no note. A new note, any effect in the same FX column, or **KIL** will cancel it. Steps with a note trigger a fresh sample play and end the retrigger sequence.

---

### RND `XY` — Randomize

Randomizes the **previously active FX** value on this track.

- `X` = downward range, `Y` = upward range

---

### RNL `XY` — Randomize Left

Randomizes the FX value in the column immediately to the left. Same `X`/`Y` semantics as RND.

---

### SLI `XX` — Slice Index

Directly sets the slice to play, bypassing note-based selection.

- `XX` = slice index `00`–`FF`
- Works regardless of the instrument's SLICE mode — even with SLICE=OFF
- Overrides the slice that the note pitch would normally select

This is useful when you want precise control over which slice plays without having to map it through note pitch. For example: `SLI 03` always plays slice 3, whatever note is in the N column.

Combine with PIT to pitch-shift a specific slice without changing the slice selection.

---

### TBL `XX` — Table Set

Overrides the instrument's default table, using table `XX` for this note.

---

### THO `XX` — Table Hop

Jumps the table playhead to row `0X`. `THO 00` = loop current section.

---

### TIC `XX` — Tick Rate

Sets the table tick rate:

- `TIC 06` = default (6 ticks per row, two rows per phrase step)
- `TIC 03` = twice as fast
- `TIC 0C` = half speed (one row per phrase step)
- `TIC 00` = trigger mode — row is set by the note that triggered the instrument, no auto-advance
- `TIC FC` = octave map — table row = octave of the triggered note (0–9)
- `TIC FE` = note map — table row = pitch of the triggered note (C=0, C#=1 … B=11)
- `TIC FF` = 200 Hz mode — table advances approximately one row every 5 ms, independent of tempo

---

### VOL `XX` — Volume Automation

Sets the step volume to `XX` at the exact tick this command fires. Useful in table rows for volume animation.

---

## 22. Modulation Reference

See §14 for how to edit mod slots, and [Appendix E](#appendix-e-parameter-reference--units--ranges)
for exact units and ranges (envelope times are in **tics**, so they track project BPM).

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

Behaves identically to ADSR — same ATK/DEC/SUS/REL parameters.

---

## 23. File Management

### Project files

- Format: `.ptp` (JSON with version field; old projects are migrated automatically on load)
- Location: `/Documents/PocketTracker/Projects/`
- Save: PROJECT screen → SAVE
- Load: PROJECT screen → LOAD

### Sample files

- Format: `.wav` (8/16/24/32-bit PCM or float; mono or stereo)
- Stereo WAV files are supported natively — SOURCE mode on the instrument or sample editor selects LEFT / RIGHT / STEREO / MONO non-destructively
- Sample rates: any — PocketTracker compensates pitch for non-44100 Hz files automatically
- Loaded via: INSTRUMENT screen → SAMPLE field → A button → file browser
- SF2 files are loaded the same way
- `.mp3` files load directly as samples — decoded to PCM in memory, with **no WAV file written** and no slice markers. The instrument remembers the `.mp3` path, so it is re-decoded automatically each time the project is reopened. No length limit is enforced at present (testing stage) — very large files may run out of memory on low-RAM devices.
- Audio from video/container files (`.mp4`, `.mkv`, `.m4a`, …) can also be loaded — that path *extracts* the audio, prompts for a name, and saves it as a WAV in the Samples folder, then loads that WAV (so it gains a reusable file and survives reload without re-decoding).

### WAV exports

- Format: 16-bit stereo WAV, 44100 Hz
- Location: `/Documents/PocketTracker/Renders/`
- Filenames: `ProjectName_0001.wav`, `_0002.wav`, … (auto-incremented)
- Triggered from: PROJECT screen → WAV MIX

### Theme files

- Format: `.ptt` (JSON color theme)
- Saved/loaded from: SETTINGS → THEME → open THEME EDITOR → SAVE / LOAD

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

> [!NOTE]
> CHOP exports do not automatically load into instrument slots. After chopping, open the INSTRUMENT screen and use the file browser to load the individual slice files onto new instruments.

---

## 24. Workflow Tips

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

Rather than duplicating a phrase at a different pitch, set the TSP column in the CHAIN. `07` = +7 semitones (a perfect fifth). The same phrase plays higher with no copy.

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

### Customizing your theme

1. Navigate to SETTINGS (PROJECT → SETTINGS row → A).
2. Move to the THEME row and press **A** to open the THEME EDITOR.
3. On row 0, use A+UP/DOWN on the theme name to cycle through built-in themes (CLASSIC, AMBER, BLUE, MONO).
4. Move down to any color row, then LEFT/RIGHT to select R/G/B, and A+UP/DOWN to adjust.
5. When you are happy with the look, move back to row 0, move RIGHT to SAVE, and press A.

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

## Appendix D: Controls Cheat Sheet

*Print this page and keep it handy.*

---

### UNIVERSAL — work on every screen

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A | Insert / confirm |
| A + UP / DOWN | Edit value (+1 / −1) |
| A + RIGHT / LEFT | Edit value (+16 / −16, or ±1 octave for notes) |
| A + B | Delete / clear value |
| B | Cancel / back / delete |
| START | Play / Stop |

---

### SCREEN NAVIGATION

| Input | Action |
|---|---|
| R + RIGHT / LEFT | Move between columns (SONG → CHAIN → PHRASE → INST → TABLE) |
| R + UP | Move to screen above in current column |
| R + DOWN | Move to screen below in current column |

**Quick nav:**

| From | To | How |
|---|---|---|
| Any Row 2 screen | GROOVE | R + UP (column 2) |
| INSTRUMENT | MODULATION | R + UP (column 3) |
| Any Row 2 screen | MIXER | R + DOWN |
| MIXER | EFFECTS | R + DOWN |
| SONG or CHAIN | PROJECT | R + UP (column 0 or 1) |

**Popup screens:**

| Screen | How to open |
|---|---|
| SAMPLE EDITOR | INSTRUMENT → cursor on SAMPLE → SELECT |
| EQ EDITOR | INSTRUMENT / MIXER / EFFECTS → cursor on EQ → SELECT |
| SETTINGS | PROJECT → cursor on SETTINGS → A |
| THEME EDITOR | SETTINGS → cursor on THEME → A |

---

### CONTEXT NAVIGATION — B + D-pad

| Input | Screen | Action |
|---|---|---|
| B + LEFT / RIGHT | PHRASE | Previous / next phrase |
| B + LEFT / RIGHT | CHAIN | Previous / next chain |
| B + LEFT / RIGHT | INSTRUMENT | Previous / next instrument |
| B + LEFT / RIGHT | TABLE | Previous / next table |
| B + LEFT / RIGHT | GROOVE | Previous / next groove |
| B + LEFT / RIGHT | MODULATION | Previous / next instrument (mods follow) |
| B + UP / DOWN | SONG | Page up / down (16 rows) |

---

### COPY / PASTE — PHRASE, CHAIN, SONG, TABLE

| Input | Action |
|---|---|
| L + B | Enter selection mode (tap again: CELL → ROW → SCREEN) |
| B *(in selection)* | Copy, exit selection |
| L + A *(in selection)* | Cut (copy + clear), exit selection |
| L + A *(outside selection)* | Paste at cursor |
| A + B *(in selection)* | Delete selection (no clipboard), exit |
| A + UP / DOWN *(in selection)* | Increment / decrement all selected values |
| L | Cancel selection |

---

### PLAYBACK

| Input | Action |
|---|---|
| START | Play / Stop |
| START on SONG | Play from current row |
| START on CHAIN | Play current chain |
| START on PHRASE | Play current phrase (loops) |
| START on INSTRUMENT | Preview instrument |
| START on SAMPLE EDITOR | Preview sample |
| START in file browser | Preview highlighted WAV |

---

### FILE BROWSER

| Input | Action |
|---|---|
| D-pad UP / DOWN | Move through files / folders |
| A | Load file / enter folder |
| B | Go up one directory level |
| START | Preview highlighted WAV |

---

### SONG SCREEN

| Input | Action |
|---|---|
| A | Insert last-used chain ID |
| A + UP / DOWN | ±1 chain ID |
| A + LEFT / RIGHT | ±16 chain IDs |
| A + B | Delete (set to --) |
| B + UP / DOWN | Page up / down (16 rows) |

---

### CHAIN SCREEN

| Input | Action |
|---|---|
| A | Insert last-used phrase / TSP |
| A + UP / DOWN | ±1 |
| A + LEFT / RIGHT | ±16 (PHR), ±12 semitones (TSP) |
| A + B | Delete slot |
| B + LEFT / RIGHT | Previous / next chain |

---

### PHRASE SCREEN

| Input | Action |
|---|---|
| A | Insert last-used note / value |
| A + UP / DOWN | ±1 semitone (note), ±1 (other) |
| A + LEFT / RIGHT | ±1 octave (note), ±16 (other) |
| A + B | Delete value |
| B + LEFT / RIGHT | Previous / next phrase |

---

### INSTRUMENT SCREEN

| Input | Action |
|---|---|
| A (on SAMPLE) | Open file browser |
| SELECT (on SAMPLE) | Open SAMPLE EDITOR |
| SELECT (on EQ) | Open EQ EDITOR |
| A + UP / DOWN | Edit current parameter |
| A + B | Reset to default |
| B + LEFT / RIGHT | Previous / next instrument |

---

### SAMPLE EDITOR

| Input | Action |
|---|---|
| A + LEFT / RIGHT | Zoom in / out |
| D-pad LEFT / RIGHT | Scroll (when zoomed) |
| D-pad (on marker row) | Move selection marker |
| START | Preview sample |
| B | Close (discard unsaved changes) |

---

### TABLE SCREEN

| Input | Action |
|---|---|
| A + UP / DOWN | Edit value |
| A + B | Delete value |
| B + LEFT / RIGHT | Previous / next table |
| L + B | Enter selection mode |

---

### GROOVE SCREEN

| Input | Action |
|---|---|
| D-pad UP / DOWN | Move between rows |
| A + UP / DOWN | Edit tick value |
| A + LEFT / RIGHT | Large step |
| A + B | Clear row |
| B + LEFT / RIGHT | Previous / next groove |

---

### MODULATION SCREEN

| Input | Action |
|---|---|
| D-pad UP / DOWN | Move between parameters |
| D-pad LEFT / RIGHT | Switch between paired slots |
| A + UP / DOWN | Edit value |
| A + LEFT / RIGHT | Large step |
| A + B | Reset to default |
| B + LEFT / RIGHT | Previous / next instrument |

---

### MIXER SCREEN

| Input | Action |
|---|---|
| D-pad LEFT / RIGHT | Select track column |
| D-pad UP / DOWN | Move between rows |
| A + UP / DOWN | ±1 |
| A + LEFT / RIGHT | ±16 |
| SELECT | Open EQ EDITOR for this track |

---

### EQ EDITOR

| Input | Action |
|---|---|
| D-pad LEFT / RIGHT | Switch between bands 1–3 |
| D-pad UP / DOWN | Move between parameters |
| A + UP / DOWN | Edit value |
| A + LEFT / RIGHT | Large step |
| B | Close and apply |

---

### THEME EDITOR

| Input | Action |
|---|---|
| D-pad UP / DOWN | Move between color rows |
| D-pad LEFT / RIGHT | Move between R / G / B (color rows) or name / SAVE / LOAD (row 0) |
| A + UP / DOWN | ±1 to selected channel |
| A + LEFT / RIGHT | ±16 to selected channel |
| B | Close |

---

### EFFECTS QUICK REFERENCE

| Code | Name | Value | Notes |
|---|---|---|---|
| ARP | Arpeggio | `XY` = intervals | Persists — cancel with `ARP 00` |
| ARC | Arpeggio Config | `XY` | High nibble=mode (0=UP 1=DN 2=PP 3=RND), low=speed |
| CHA | Chance | `XY` | X=probability (0=never F=always), Y=target (0=note 1=FX1 2=FX2 3=FX3) |
| DEL | Delay | `XX` ticks | Delays row trigger |
| GRV | Groove | `XX` | Assigns groove to this track |
| HOP | Hop/Jump | `XY` | Jumps to step Y, X times max |
| KIL | Kill | `00` | Stops sample, cancels all persistent FX |
| OFF | Offset | `XX` | Sample start position jump |
| PIT | Pitch Offset | `XX` signed | 00–7F up, 80–FF down |
| PSL | Pitch Slide | `XX` ticks | Portamento |
| PBN | Pitch Bend | `XX` | 00–7F up, 80–FF down — **persists** |
| PVB | Vibrato | `XY` | X=speed, Y=depth — **persists** |
| PVX | Extreme Vibrato | `XY` | 4× deeper, 2× faster than PVB |
| REP | Repeat/Retrigger | `XY` | Y=0: every X ticks; Y≠0: fade — **persists** |
| RND | Randomize | `XY` | Randomizes previous FX value |
| RNL | Randomize Left | `XY` | Randomizes FX in column to the left |
| SLI | Slice Index | `XX` | Direct slice selection |
| TBL | Table Set | `XX` | Override instrument's table |
| THO | Table Hop | `XX` | Jump table to row 0X |
| TIC | Tick Rate | `XX` | Table speed (00=trigger 06=default FC=octave FE=note FF=200Hz) |
| VOL | Volume | `XX` | Immediate volume at this tick |

---

### HEX / NOTE QUICK REFERENCE

```
 Dec  Hex  |  Dec  Hex
   0   00  |  128   80
  16   10  |  160   A0
  32   20  |  192   C0
  64   40  |  224   E0
  96   60  |  255   FF
```

```
Note offsets:  C   C#  D   D#  E   F   F#  G   G#  A   A#  B
               00  01  02  03  04  05  06  07  08  09  0A  0B
```

- **Middle C** = `C-4` = MIDI 60
- **VOL/PAN center** = `80` (unity / center pan)
- **+1 octave** = +12 semitones = `0C`
- **+1 perfect fifth** = +7 semitones = `07`

---

## Appendix E: Parameter Reference — units & ranges

What every numeric parameter actually means: the value you edit on screen (its hex/raw range)
and the real-world unit it maps to. For the DSP internals behind these — the constants you can
change in source to reshape an effect's character — see `docs/dsp-settings-guide.md`.

> **How to read the "raw" column:** unless noted, parameters are edited as two hex digits
> `00`–`FF` (0–255). A+UP/DOWN steps by the small step; A+LEFT/RIGHT by the large step;
> A+B resets to the default.

### Time parameters

| Parameter | Where | Raw | Maps to | Notes |
|---|---|---|---|---|
| Mod **ATK / HOLD / DEC / REL** | MODULATION (AHD/ADSR/DRUM/TRIG) | `00`–`FF` | value × 1 **tic** | Tempo-relative. 1 tic = 1/12 of a step ≈ **10 ms at 120 BPM** (scales inversely with BPM). `00` = instant. Max (`FF`) ≈ 2.7 s at 120 BPM. ADSR/TRIG have no HOLD; AHD/DRUM have no SUS/REL. |
| Mod **SUS** | MODULATION (ADSR/TRIG) | `00`–`FF` | 0–100 % level | Sustain **level**, not a time (`FF` = hold at full depth). |
| Mod **AMT** | MODULATION (all) | `00`–`FF` | 0–100 % depth | How much the destination is moved. |
| Delay **TIME** (free) | EFFECTS, SYNC off | `00`–`FF` | 0–2000 ms | ≈ 7.8 ms per step. Default `40` ≈ 500 ms. |
| Delay **TIME** (sync) | EFFECTS, SYNC on | `00`–`0B` | 12 subdivisions | BPM-locked: `00`=1/1 `01`=1/2 `02`=1/4 `03`=1/8 `04`=1/16 `05`=1/32 `06`=1/4T `07`=1/8T `08`=1/16T `09`=1/4. `0A`=1/8. `0B`=1/16. |
| Delay **FDBK** | EFFECTS | `00`–`FF` | 0–100 % feedback | Near `FF` ≈ near-infinite repeats. |
| Instrument / Table **TIC** | INSTRUMENT, TABLE | — | tics per step | See §12; sets how many tics each table/retrigger step lasts. |

> **Beat-sync note:** all modulation envelope times are expressed in **tics**, so they already
> follow project BPM — speeding up the tempo shortens every envelope proportionally. Only the
> delay has an explicit fraction-sync (`1/8`, `1/4T`, …); a fraction-sync mode for LFO **FREQ**
> is a possible future addition (not in this build).

### Frequency parameters

| Parameter | Where | Raw | Maps to | Notes |
|---|---|---|---|---|
| Filter **CUT** | INSTRUMENT | `00`–`FF` | 20 Hz – 20 kHz, log | `20 × 1000^(v/255)`. ≈ +2.7 % per step. |
| Filter **RES** | INSTRUMENT | `00`–`FF` | 0.0 – 1.0, linear | Resonance amount. |
| EQ **FREQ** | EQ EDITOR | `00`–`FF` | 20 Hz – 20 kHz, log | Same curve as filter CUT. ≈ +2.7 % per step, so a single small step always advances the displayed Hz (display-aware stepping). |
| EQ **GAIN** | EQ EDITOR | `00`–`F0` (0–240) | **−12.0 … +12.0 dB** | **0.1 dB per small step**, 1.0 dB per large step. `120` (`78`) = 0 dB (default). |
| EQ **Q** | EQ EDITOR | `00`–`FF` | 0.1 – 10.0, log | `0.1 × 100^(v/255)`. Higher = narrower band. |
| Reverb **DAMP** | EFFECTS | `00`–`FF` | 200 Hz – 20 kHz, log | `200 × 100^(v/255)` HF damping cutoff. Lower = darker. |
| LFO **FREQ** | MODULATION | `00`–`FF` | ≈ 0.08 – 20 Hz, linear | `(v+1) × 20 / 256`. |

### Gain / level parameters

| Parameter | Where | Raw | Maps to | Notes |
|---|---|---|---|---|
| Instrument **VOL** | INSTRUMENT | `00`–`FF` | 0 – max | `FF` default (full). |
| Instrument **PAN** | INSTRUMENT | `00`–`FF` | L … center … R | `80` = center (default). |
| Mixer track / master **VOL** | MIXER | `00`–`FF` | silent … +6 dB | `80` = unity (0 dB). |
| Reverb **SIZE** | EFFECTS | `00`–`FF` | 0.0 – 1.0 feedback | Not a time — higher feedback = longer tail. |
| Master **DEPTH** (OTT/DUST) | EFFECTS | `00`–`FF` | 0–100 % wet | `00` = bypass. |
| Sample-editor **LIM** pre-gain | SAMPLE EDITOR (offline LIM FX) | `00`–`FF` | +0 … +12 dB | `1.0 + (v/255)×3.0` linear (×1 … ×4). The always-on master-bus limiter is fixed (not user-set) — see `dsp-settings-guide.md`. |

---

*PocketTracker is open-source (GPL-3.0). Contributions and bug reports welcome.*
