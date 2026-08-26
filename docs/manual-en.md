# PocketTracker User Manual

**Manual revision:** 1.3  
**Covers:** PocketTracker 0.9.6

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
25. [Configuration File (config.json)](#25-configuration-file-configjson)
26. [Appendix: Controls Cheat Sheet](#appendix-controls-cheat-sheet)

---

## 1. Introduction

PocketTracker is a sample-based music tracker for Android handhelds and budget Android devices. It is inspired by M8, LSDJ, and Little GP Tracker (LGPT/Picotracker), and designed to run natively at **640×480** — the resolution of devices like the Miyoo Flip.

**New to trackers?** A tracker is a music sequencer where notes are arranged in a grid, with time flowing downward. Each row is a step, each column carries a parameter (note, instrument, volume, effect). Songs are built by chaining together short patterns called **phrases**, grouped into **chains**, arranged in a **song**. Think of it as building music from small, reusable blocks.

PocketTracker stores everything in a single project file (`.ptp`). Sounds come from standard `.wav` files or **SoundFont (SF2 / SF3)** files that you load yourself — there are no bundled samples.

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
4. Launch the app and choose a folder for it to work in — see below. PocketTracker asks for **no permissions at all**.

### Choosing your folder

PocketTracker holds no storage permission, so it can only see folders you hand it. The first time you open a file browser — **PROJECT → LOAD**, or **LOAD** on the INSTRUMENT screen — it opens on a list with a single row:

```
> ADD FOLDER...
```

Press **A** on it. Android's own folder picker opens; pick a folder and confirm. That folder becomes PocketTracker's home, and the app creates its six sub-folders inside it the moment it needs them:

```
Projects/  Samples/  Renders/  Soundfonts/  Instruments/  Themes/
```

Anything already in the folder is left exactly as it is — so if you have used PocketTracker before, pick your existing `Documents/PocketTracker` and every project, sample and theme is where it was.

> [!TIP]
> You can add more folders at any time. Climb to the top of the browser — **R+LEFT**, or **A** on the `..` row, repeated until it stops — and you will see every folder you have granted, with `ADD FOLDER...` at the end — that is how you reach a sample library, an SD card or a video in DCIM that lives outside your home folder. Adding a folder never moves your projects: the home folder only changes when you change it yourself.

Android will not let any app be granted a whole volume (`Internal storage`) or the `Download` folder itself. Pick a real folder — `Documents/PocketTracker` is a tidy choice.

### Changing your home folder, and dropping one

That top-level list of granted folders is a screen with two gestures of its own, shown on its top bar whenever the cursor is on a folder:

| | |
|---|---|
| **SELECT + A** | **Make this the home folder.** The app's six sub-folders move to it — meaning it starts *looking* in the new place; nothing on disk is moved or copied. Confirm with **A**. |
| **SELECT + B** | **Forget this folder.** Hands the access back to Android and removes the row. Confirm with **A**. |

The folder currently in use is marked `(HOME)`. One whose directory has been deleted or unplugged since you granted it is marked `(MISSING)` — the app will not use a missing folder as its home, and `SELECT + B` is how you clear the row.

> [!IMPORTANT]
> **FORGET is not DELETE.** It gives up a permission; every file in the folder stays exactly where it is, and you can grant the same folder again at any time. Nothing in this list can delete your files — `SELECT + B` means DELETE on ordinary files and folders, but on a granted folder it can only ever mean forget.

### Sample files

PocketTracker has no bundled default samples — all instrument slots start empty. Copy your own `.wav` files into a folder you have granted, and load them from the **INSTRUMENT** screen using the file browser. SF2 and SF3 files are loaded the same way.

> A large SoundFont of either kind takes a moment to load, and the screen does not redraw while it does. SF3 stores its samples compressed, so its files are much smaller on disk and it holds less memory once loaded, but it takes longer to open than the SF2 of the same bank.

### Project files

Below, `<home>` is the folder you chose above.

Projects are saved as `.ptp` files in:

```
<home>/Projects/
```

WAV exports are saved to:

```
<home>/Renders/
```

Resampled instruments and CHOP exports are saved to:

```
<home>/Samples/Resampled/
<home>/Samples/Chops/{name}/
```

On Linux, Windows and PortMaster handhelds there is no folder to choose: the app owns a `PocketTracker` folder of its own and the same six sub-folders live in it.

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
| EQ EDITOR | INSTRUMENT / INST.POOL / MIXER / EFFECTS → cursor on EQ cell → **A** (or SELECT). Close with **B** (or SELECT). |
| SETTINGS | PROJECT screen → cursor on SETTINGS row → A |
| THEME EDITOR | SETTINGS screen → cursor on THEME row → A |

> [!NOTE]
> **A opens, B goes back.** On cells that lead to another screen — the **EQ** cell and the **NAME**
> cell on PROJECT/INSTRUMENT — a quick **tap of A** opens the editor, while **holding A + a direction**
> still edits the value on that same cell (e.g. the EQ slot number). The open fires when you *release* A,
> so a held A never opens by accident. Inside the EQ EDITOR, **B closes** it and **B + LEFT/RIGHT** still
> cycles the EQ preset slot.

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
| A + RIGHT | +1 (small step) |
| A + LEFT | −1 (small step) |
| A + UP | +16 / +1 octave (large step) |
| A + DOWN | −16 / −1 octave (large step) |
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
| INSTRUMENT | Previous / next instrument (00–7F) |
| TABLE | Previous / next table (00–7F) |
| GROOVE | Previous / next groove (00–7F) |

**`SETTINGS → NAV` is `SONG` by default**, and B + D-pad on CHAIN and PHRASE walks the arrangement instead
of the pools. The cursor becomes a song cell, and the chain or phrase you are looking at is whichever
one that cell holds — the CHAIN and PHRASE headers show it (`CHAIN 20  S01 T3`).

| Screen | B + LEFT / RIGHT | B + UP / DOWN |
|---|---|---|
| CHAIN | Nearest filled cell left / right in the same song row | Nearest filled cell up / down the same track, skipping empty rows |
| PHRASE | Nearest track left / right whose chain also has a phrase at this chain row | Previous / next filled row of the chain you are in |

Nothing to that side means the press does nothing. On PHRASE, plain UP/DOWN off step `00` or `0F` now
moves to the previous or next filled row of the chain instead of wrapping inside the same phrase, and
**R+RIGHT does nothing when the cell under the cursor is empty** — you reach a chain by putting it in
the song first.

> [!IMPORTANT]
> Under `NAV = SONG` a chain or phrase that is not placed in the arrangement cannot be opened at all.
> Nothing is lost — set NAV back to `POOL` and every slot is reachable again.

The other screens are unchanged: SONG still pages by 16, and INSTRUMENT, MODS, TABLE, GROOVE and
INST.POOL still walk their pools.

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
| L + B + A | Deep-clone the chain or phrase under the cursor into the next free slot |
| L + R | Leave selection mode (nothing copied) |

**Selection increment:** In selection mode, **A + LEFT/RIGHT** increments or decrements all selected values simultaneously.

**Clearing the clipboard:** the copy buffer deliberately survives leaving a selection — you can select
again by accident without losing what you copied. Press **L + R** when you are *not* selecting to clear
it, which is also how you dismiss the clipboard readout in the top strip.

**L + R** also restores muted and soloed tracks (§5.7). It undoes one thing per press, most recent
first: if you muted a channel after making a selection, the first press brings the channel back and
the second clears the selection — and the other way round if the selection came last.

**Selection modes:**
- **CELL** — single cell under cursor
- **ROW** — full row (all columns)
- **SCREEN** — all rows visible

> [!TIP]
> Use **SCREEN** selection mode to duplicate an entire phrase or chain quickly: enter SCREEN mode → B to copy → navigate to an empty phrase/chain → L+A to paste.

---

### 5.7 Mute & Solo

Works on the SONG and MIXER screens, while playing or stopped.

| Input | Action |
|---|---|
| R + B | Mute / unmute the track under the cursor |
| R + A | Solo / unsolo it |
| R + B or R + A over a selection | Applies to every track the selection covers |
| L + R | Restore full playback on all tracks |

The sound stops the instant you press, notes already ringing included.

**Hold or latch.** Which button you let go of first decides what happens:

- release **R** first — the change stays;
- release **A** or **B** first — everything that chord did is undone.

So the same chord gives you a momentary drop you can hold through a bar, and a mute you set and walk
away from.

Soloing is additive: solo a second track and both play. A track that is muted stays silent even when
soloed.

A track that is making no sound draws its numbers dimmed — its chain IDs on SONG, its fader value on
MIXER — so soloing one track dims the other seven.

---

### 5.8 Playback Controls

| Input | Action |
|---|---|
| START | Play / Stop (context-aware) |
| START on SONG | Play full song from top |
| START on CHAIN | Play current chain |
| START on PHRASE | Play current phrase (loops) |
| START on INSTRUMENT | Preview current instrument |
| START on SAMPLE EDITOR | Preview edited sample |
| START on SONG in LIVE mode | Queue the chain under the cursor ([LIVE Mode](#live-mode)) |

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
- The **SONG** arranges chains across 8 tracks. All 8 start together on the row you press START from, and each then moves down its own column as its own chains end.

All values (chain IDs, phrase IDs, instrument IDs, etc.) are hexadecimal, ranging from `00` to `FF`.

> [!NOTE]
> All values in PocketTracker are **hexadecimal** (base-16). Decimal 16 = hex `10`, decimal 255 = hex `FF`. The appendix at the end has a [conversion table](#hex--note-quick-reference).

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

During playback a `>` appears to the left of the cell each track is on. The eight move independently, so they are rarely all on the same row.

A `>` also marks the playing row on the CHAIN and PHRASE screens. It is only drawn where the screen is showing something that is actually playing — audition a phrase on its own and the chain and song screens stay unmarked, because that phrase need not belong to any chain.

### Controls

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A | Insert last-used chain ID |
| A + LEFT/RIGHT | Increment / decrement chain ID |
| A + UP/DOWN | Increment / decrement by 16 |
| A + B | Delete (set to --) |
| B + UP/DOWN | Page up / down (jump 16 rows) |
| B + LEFT/RIGHT | Switch between SONG and LIVE mode |
| START | Play song from current row |

> [!TIP]
> You can start playback from any row — not just the beginning. Move the cursor to the row where you want playback to start, then press **START**. Useful for jumping to a specific section while mixing.

### LIVE Mode

Press **B + LEFT** or **B + RIGHT** on the SONG screen to switch between the two transport modes. The title changes from `SONG:` to `LIVE:`.

In LIVE mode the song grid is a scene launcher. Each channel plays one chain and repeats it until you queue something else, and START no longer starts or stops the transport — it queues.

| Input | Action |
|---|---|
| START | Queue the chain under the cursor on that channel |
| START again on the same cell | Start it at the next bar instead of waiting for the chain |
| L + START | Queue the whole cursor row — every channel at once |
| R + START | Queue the channel under the cursor to fall silent |
| B + LEFT/RIGHT | Back to SONG mode |

A queued launch blinks a `>` on the row it will jump to, while the solid `>` stays where the channel is now. A queued stop blinks a `_` in place of the channel's marker. Slow blink means it is waiting for the chain to end; fast blink means the next bar.

A blank cell in a queued row silences that channel, so a row sounds the way it looks. With the transport stopped, START and L+START begin playing immediately.

Switching modes never jumps or silences anything: each channel keeps its place, and starts repeating — or resumes walking down its column — from its next chain boundary. LIVE mode lasts for the session and is not saved with the project.

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
| A + LEFT/RIGHT | Increment / decrement |
| A + UP/DOWN | ±16 (PH) or ±12 semitones (TSP) |
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
| I | Instrument ID (`00`–`7F`). Always set — no empty state. |
| FX1/FX2/FX3 | Effect type + value (e.g., `RPT 03`, `ARP 47`) |

Notes are written as pitch + octave: `C-4`, `C#4`, `D-4`, … `G-9`. Range is **C-0 to G-9**. Middle C = `C-4` (MIDI note 60).

### Controls

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A | Insert last-used note / value |
| A + LEFT/RIGHT | +1 / −1 semitone (note), +1 / −1 (other values) |
| A + UP/DOWN | ±1 octave (note), ±16 (other values) |
| A + B | Delete value at cursor |
| B + LEFT/RIGHT | Switch to previous / next phrase |
| START | Play current phrase (loops) |

### FX columns

Each FX slot has two parts: **type** (3-letter code) and **value** (2-digit hex). Use A+LEFT/RIGHT on the type to step through the available effects one at a time, or A+UP/DOWN to open the effect picker and choose from the grid. Effects are listed in §21.

> [!WARNING]
> Some effects (**ARP**, **RPT**, **PBN**, **PVB**, **PVX**) **persist across steps that have no note** — they keep running on empty rows. They are cancelled by: a new note on the same track, any effect in the same FX column, setting the effect to `00`, or **KIL**.

The mixer faders written from a phrase (**VTR**, **VMV**) persist differently: they are not attached to a
note at all, so nothing cancels them but the next `VTR`/`VMV` — and stopping playback, which puts the
faders back to the values on the MIXER screen.

**AUS** and **AUF** are the only effects the grid can draw **dimmed**, in the colour of an empty cell.
That means the pair did not come out: the fade will not play, and §21 lists the reasons. A pair that
works stays lit even when its two halves are in different phrases of the same chain.

---

## 10. INSTRUMENT Screen

The INSTRUMENT screen configures how a sample or SF2 preset is played.

Navigate here with **R+RIGHT** from PHRASE. Use **B+LEFT/RIGHT** to switch between instruments without leaving the screen.

### WAV instrument parameters

| Parameter | Range | Description |
|---|---|---|
| NAME | — | Instrument name. Press A (or SELECT) to edit it on the keyboard overlay. |
| SAMPLE | path | WAV, SF2 or SF3 file. Press A to open file browser; SELECT to open SAMPLE EDITOR. |
| ROOT | C-0 – G-9 | The pitch of the sample as recorded. |
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
| EQ | — | Press A (or SELECT) to open the EQ EDITOR for this instrument. A + LEFT/RIGHT picks the EQ slot. |

> [!TIP]
> **ROOT** is the most important tuning parameter. Set it to the actual pitch of your sample (e.g., `A-4` for a 440 Hz sine). If notes sound in the wrong octave, ROOT is usually the reason.

> [!TIP]
> **DRIVE** and **CRUSH** are subtle at low values (`10`–`30`) and very aggressive near `FF`. Both are per-instrument — start low and increase by ear.

### SF2 instrument parameters

When the loaded file is a SoundFont, additional override fields appear for the preset's internal envelope and filter. Setting these to `--` uses the SoundFont's built-in values.

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

**Where the reverb and delay sends sit in that chain.** A send is tapped after the instrument's own
volume — instrument VOL, the phrase `V` column and any VOL modulation — but *before* the track fader.
So turning a track down in the MIXER thins the dry signal while leaving its reverb and delay tails
where they were, which is the usual way to push a part back into the space rather than out of it.

The **master** fader is the exception, and works the way a master should: the send returns are summed
into the mix before it, so master volume takes the reverb and delay down with everything else. Pulling
the master to `00` really does end in silence, tails included.

The last two stages can also be written from a phrase: `VTR` replaces the track fader and `VMV` the
master one for as long as the song is playing (§21).

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

A project has **128 instrument slots**, `00`–`7F`, and in a new project every one of them is empty —
PocketTracker bundles no samples. An empty slot plays silence and is named `INST00` … `INST7F` until
you load something into it, at which point it takes the file's name unless you have typed one of your
own.

- **B + LEFT/RIGHT** — switch between instruments 00–7F
- **R+UP** from INSTRUMENT → MODULATION screen
- **A** on SAMPLE field → opens file browser
- **SELECT** on SAMPLE field → opens SAMPLE EDITOR

### File browser controls

| Input | Action |
|---|---|
| D-pad UP/DOWN | Move through files / folders |
| A | Load file or enter folder |
| A on `..` | Go up one directory level |
| R + LEFT | Go up one directory level |
| B | Close the browser |
| START | Preview highlighted audio (WAV / MP3 / FLAC / OGG / OPUS / M4A) or video file |

The browser can also manage files — see [section 23](#23-file-management).

Compressed audio (MP3 / FLAC / OGG / Opus / M4A) loads as a sample too — it decodes in place, with no
WAV written. Video files are instead **converted** to a WAV in the Samples folder. See §23 →
*Audio from video files*.

---

## 11. SAMPLE EDITOR Screen

The SAMPLE EDITOR is a full-screen waveform editor for the currently loaded WAV. Open it from the INSTRUMENT screen by moving the cursor to the SAMPLE row and pressing **SELECT**.

Press **B** to close. If you have edited the waveform, B raises **ARE YOU SURE?** first — A discards the edit and leaves, B stays in the editor. Use **SAVE / OVERWRITE** inside the editor to write changes to disk. B is the only way out: R+DPAD cannot leave the editor, so an unsaved edit cannot be lost by walking off the screen.

### Waveform view

The waveform fills the top portion of the screen. A playback cursor shows the current read position during preview.

| Input | Action |
|---|---|
| R + UP/DOWN | Zoom in / out, from any row |
| A + LEFT/RIGHT | Zoom in / out (cursor on the ZOOM cell) |
| D-pad LEFT/RIGHT | Scroll the view (when zoomed in) |
| START | Preview current sample (respects SOURCE mode) |

### Selection

Move the selection start and end markers to define a region for operations.

| Input | Action |
|---|---|
| D-pad (on start/end marker row) | Move the active marker |
| A + UP/DOWN | Jump marker by large step |

### Non-destructive parameters

These change playback behaviour without modifying the waveform data:

| Parameter | Options | Description |
|---|---|---|
| SOURCE | LEFT / RIGHT / STEREO / MONO | Which channel(s) of a stereo WAV to use. Non-destructive — never alters the file. SAVE/OVERWRITE applies SOURCE at write time. Opens on **STEREO** whenever the file has a right channel; a mono file reads MONO and the cell cannot be moved. |
| RATE | HIGH / NORM / LOFI | Sample rate mode. NORM = original. LOFI = 8-bit lo-fi downsampling. |
| SNAP | ON / OFF | With SNAP on, a selection edge you move lands on the nearest **zero crossing** instead of the exact frame — the quietest place to cut, and the way to avoid a click at the seam of a loop or a crop. It looks in the signal the cut will actually be made in: LEFT and RIGHT search their own channel, MONO searches the downmix it will save, and STEREO looks for the frame where **both** channels are quiet. Turn it off when you want a frame exactly where you put it. |

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

The header shows the sample's own tempo beside its rate and length — the BPM it plays at if it holds
exactly the bar count on the DURATION row. When that number matches the project's TEMPO the sample is
already on the grid. `---BPM` means there is no sample, or that DURATION is set to a length this
sample cannot be.

| Sub-mode | Description |
|---|---|
| RPITCH | Pitch-shifts the selection to align its length to a target beat count, without changing duration. |
| TSTRETCH | Time-stretches the selection to a target beat duration without changing pitch. Uses SOLA (Synchronized Overlap-Add), Akai-cyclic mode — the same algorithm as the Akai S950/S1000. |

### Offline FX

Applied to the whole sample (or selection) offline — rendered immediately, with UNDO available.

| FX | Description |
|---|---|
| EQ | Tap A (or SELECT) on the slot cell to open the EQ EDITOR and dial it in; then APPLY bakes the EQ into the sample. |
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
| MANUAL | Place the boundaries yourself. Opens on the sample's existing markers, or empty if it has none. No SENS/BY parameter. |

Slice markers are stored in the WAV `cue ` chunk — compatible with M8, Blackbox, Reaper, Logic, and Adobe Audition.

The row below shows which slice you are on and where it starts. Stepping it selects that slice, so you
can dial through them and hear each one with START.

### Moving and making slice boundaries

The **position** cell on that row is editable under every mode but OFF, so a detected transient or an
equal division can be nudged. **MANUAL** is the mode with no computed set of its own, and the only one
where a plain A makes a new boundary.

| Input | Action |
|---|---|
| A + LEFT/RIGHT | Move the boundary by a fine step |
| A + UP/DOWN | Move it by a coarse step |
| A + B | MANUAL: delete it. TRANSIENT / DIVIDE: put it back where the mode had placed it |
| A *(while the sample plays)* | MANUAL: cut a boundary at the playhead |

Both steps scale with the zoom, as the selection edges do, and **SNAP** applies the same way.

Under MANUAL the slice counter reaches one past the last slice: that slot is the next boundary, sitting
on the one to its left, and moving it off is what creates it. A boundary may be dragged past its
neighbours — its number follows its position — and deleting one renumbers the rest.

**Chopping by ear:** with the cursor on that row, press START and tap A on every hit.

> [!NOTE]
> Anything that changes the sample's **length** — CROP, CUT, DUPL, PASTE, DEL, UNDO, a resizing SYNC —
> clears every slice boundary, including the ones the file arrived with.

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

**TIC `00` — trigger mode.** The table advances one row **per note** instead of with time: each note
the track plays reads the next row down, so a sixteen-row table is a sixteen-note sequence. `HOP`
loops a section of it as usual, and PLAY, STOP and a render each start again from row `00`.

> [!TIP]
> Use **HOP** in the last row of a table section to loop just part of the table. For example: rows 00–03 with `HOP 00` in row 03 will loop those 4 rows indefinitely, ignoring rows 04–0F.

> [!NOTE]
> By default, instrument N uses table N. To override this per-note, place a **TBL XX** effect in the phrase FX column. The table switches immediately and stays active for subsequent notes on that track.

### Three playheads

Each FX column runs at its own speed and loops independently. There are three playheads, marked by a
`>` before each FX column. The FX1 playhead carries the N and VOL columns too, so it is also shown
beside the row number.

- **`HOP` steers only the column it is written in.** `HOP 00` in FX2 loops FX2 while N, VOL and FX1
  keep walking all sixteen rows.
- **`TIC` sets only that column's speed.** Write it in a column's last row to set that column's rate
  for the whole table, or anywhere else to change it as the playhead passes.
- **`HOP FF` stops only its own column.** The table ends when all three have stopped.

```
     N    VOL  FX1      FX2      FX3
00   00   --   CUT 40   ---  00   ---  00
01   00   --   CUT 60   HOP 00   ---  00   ← FX2 loops rows 00-01 for ever
…
0F   00   --   ---  00  TIC 03   ---  00   ← …twice as fast as everything else
```

Two columns at different rates give you cross-rhythms out of a single table — a filter moving in
threes under a volume moving in fours.

> [!NOTE]
> `THO` written in a **phrase** moves all three playheads, since it belongs to no column. `THO` inside
> the table moves the column it is typed in.

### Fades in a table

`AUS` and `AUF` work on a table row as they do on a phrase step (§21), with **rows** as the span:

```
     N    VOL  FX1      FX2      FX3
02   00   --   CUT 20   AUS 80   ---  00   ← start at 20, linear
0A   00   --   ---  00  AUF F0   ---  00   ← arrive at F0 eight rows later
```

A table `AUS` can move **VOL**, **CUT**, **RES**, **EQN** and **EQM**; anything else to its left is
passed over. The fade follows the playhead of the column holding the value it is moving — `CUT` in
FX1 above, so that fade runs at FX1's speed whichever column the `AUS` sits in. **HOP steers it** —
back to the `AUS` row restarts it, into the middle picks it up there, past the `AUF` ends it. A `TIC`
change stretches it along with the rest of that column.

⚠️ A span never wraps past row `0F`: write the `AUF` on a later row than the `AUS`.

### Table–instrument link

By default, instrument N uses table N. Override per-note with the **TBL** phrase effect.

### Controls

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A + LEFT/RIGHT | Edit value |
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

Default: all grooves start empty. An empty groove plays even timing (12 ticks per step, no swing).

### Assigning grooves

Each track uses groove `00` by default. Use the **GRV XX** phrase effect to switch a track to groove `XX`.

> [!TIP]
> To reset a track back to even timing after a groove section, place `GRV 00` in an FX column. Groove `00` is empty by default, which plays perfectly even — but it is a real, editable groove: if you put tick values into groove `00`, every track that hasn't been assigned another groove will swing with it.

> [!WARNING]
> For the grooved track to stay in sync, each complete cycle of your groove list should **average 12 ticks per entry**. A 2-entry swing sums to 24 (14+10 ✓). A 4-entry groove should sum to 48. If the average is not 12, the track drifts against un-grooved tracks within each phrase; in SONG mode all tracks re-align at every chain row, so an off-average groove produces a gap (or a cut) at the end of each phrase rather than unbounded drift.

### Controls

| Input | Action |
|---|---|
| D-pad UP/DOWN | Move between rows |
| A + LEFT/RIGHT | Edit tick value |
| A + UP/DOWN | Edit tick value (large step) |
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
| A + LEFT/RIGHT | Edit value |
| A + UP/DOWN | Edit value (large step) |
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

The master volume is applied to everything below it — the eight tracks **and** the reverb and delay
returns — so pulling MST down takes the tails with it.

Both kinds of fader can also be moved from a phrase while the song plays: `VTR` writes the fader of the
track it is on, `VMV` writes the master (§21), and `AUS`/`AUF` fade either of them smoothly. The numbers
on this screen keep showing what you typed — they are where playback *starts*, and stopping puts the
faders back there.

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
| A + LEFT/RIGHT | Increase / decrease value by 1 |
| A + UP/DOWN | Increase / decrease value by 16 |
| A or SELECT (on the master EQ cell) | Open EQ EDITOR. A + LEFT/RIGHT picks the EQ slot. |

---

## 16. EFFECTS Screen

The EFFECTS screen configures the global stereo send buses (reverb and delay) and selects the master bus effect.

Navigate here: **R+DOWN** from MIXER, or **R+DOWN** twice from any Row 2 screen.

### Reverb section

| Parameter | Description |
|---|---|
| SIZE | Room size (`00`–`FF`). Higher = longer reverb tail. |
| DAMP | High-frequency damping (`00`–`FF`). Higher = darker reverb. |
| EQ | Press A (or SELECT) to open the EQ EDITOR for the reverb return. |

The reverb return volume is set on the MIXER screen (REV row in master column).

### Delay section

| Parameter | Description |
|---|---|
| TIME | Delay time. **SYNC off:** free time `00`–`FF` = 0–2000 ms. **SYNC on:** `00`–`0B` selects a BPM-locked subdivision (1/1 … 1/16.). Press **SELECT** on this row to switch between the two. |
| FDBK | Feedback amount (`00`–`FF`). Higher = more repeats. |
| REV | Amount of delay output sent into the reverb bus (`00`–`FF`). Delay is processed before reverb, so this cross-routing is zero-latency. |
| EQ | Press A (or SELECT) to open the EQ EDITOR for the delay return. |

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

The EQ EDITOR is a full-screen overlay that opens when you press **A** (or SELECT) on an EQ cell in the INSTRUMENT, INST.POOL, MIXER, EFFECTS, or SAMPLE EDITOR (EQ-effect slot) screens.

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
| FREQ | `00`–`FF` | 20 Hz – 20 kHz (log) | A single A+LEFT/RIGHT step always changes the displayed Hz. |
| GAIN | `00`–`F0` | **−12.0 … +12.0 dB** | Small step **0.1 dB**, large step **1.0 dB**. `0.0 dB` is the default. |
| Q | `00`–`FF` | 0.1 – 10.0 (log) | Bandwidth; higher = narrower. |

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
| A + LEFT/RIGHT | Edit value (small step — GAIN: ±0.1 dB) |
| A + UP/DOWN | Edit value (large step — GAIN: ±1.0 dB) |
| A + B | Reset parameter to default (FREQ mid · GAIN 0 dB · Q mid) |
| B + LEFT/RIGHT | Switch EQ preset slot (the slot shown in the top row) |
| B | Close EQ EDITOR and apply changes (SELECT also closes) |


---

## 18. PROJECT Screen

The PROJECT screen contains global settings and file operations.

Navigate here: **R+UP** from SONG or CHAIN.

### Settings

| Parameter | Description |
|---|---|
| NAME | Project name. Tap A (or SELECT) to edit it on the keyboard overlay; hold A + LEFT/RIGHT cycles the character under the cursor in place. |
| TEMPO | BPM. |
| TRANSPOSE | Global semitone offset applied to all tracks. |

### File operations

| Row | Action |
|---|---|
| SAVE | Save project to `.ptp` file (press A to confirm). |
| LOAD | Open file browser to load a project. |
| EXPORT — MIX | Render the full song to a stereo WAV (offline, faster than real-time). |
| EXPORT — STEMS | Render each active track to its own stereo WAV stem, plus reverb and delay send returns. |
| CLEAN SEQ | Remove unused chains and phrases (with confirmation dialog). |
| CLEAN INST | Remove unused instruments (with confirmation dialog). |
| SETTINGS | Open the SETTINGS screen (press A). |

WAV exports are saved to `<home>/Renders/` — `<home>` being PocketTracker's home folder (section 2) — with auto-incremented filenames (`ProjectName_0001.wav`). Stems are written to a per-project subfolder — `Renders/ProjectName/ProjectName_1.wav`, `_2.wav`, … — plus `_reverb.wav` / `_delay.wav` when those sends are in use.

> [!WARNING]
> **CLEAN SEQ** and **CLEAN INST** are **permanent** — there is no undo. Save your project before running them, in case you remove something you still needed.

> [!TIP]
> **EXPORT → MIX** renders faster than real-time. A 3-minute song typically exports in a few seconds. The status line shows the output filename when done.

---

## 19. SETTINGS Screen

The SETTINGS screen is opened from the PROJECT screen (cursor on SETTINGS row, press A). Press **B** to return to PROJECT.

All value rows are edited with **A + D-pad**. A single **A** press is reserved for the two action rows: THEME (opens the editor) and TEMPLATE (SAVE / CLEAR).

| Setting | Options | Description |
|---|---|---|
| LAYOUT | PORTRAIT (+ SKIN) | On a touchscreen device the app lays itself out as the portrait device skin, and the row's real control is the **SKIN** column beside it: **NORM** (beige casing, dark labels) or **DARK** (slate casing, white labels). PocketTracker picks portrait, landscape or fullscreen for you from the screen shape and from whether it finds physical buttons; a mode column appears only on a device that has both a touchscreen and a controller (see below). |
| SCALING | INT / BILINEAR | Screen scaling algorithm. INT = crisp pixel-perfect integer scaling. BILINEAR = smooth subpixel scaling. |
| BTN SOUND | ON / OFF (+ VOL) | Play a click sound on button press. The **VOL** sub-column to its right sets click volume (`00`–`FF`). |
| BTN VIBRO | ON / OFF (+ POW) | Haptic feedback on button press (where supported). The **POW** sub-column to its right sets vibration intensity (`00`–`FF`). |
| KB INSERT | BEFORE / AFTER | Where the QWERTY keyboard inserts characters in name fields. |
| CURSOR | REMEMBER / REFRESH | Whether cursor position is preserved when switching between screens. |
| NAV | POOL / SONG | What B + D-pad walks. **SONG** (the default) walks the arrangement: the cursor is a song cell, and the chain and phrase on screen are the ones that cell holds. **POOL** steps through the 00–FF chain and phrase pools instead, which is what earlier versions did — see §5.4. |
| FOLDER | REMEMBER / REFRESH | With REMEMBER, a sample load reopens at the folder you last loaded a sample from, for as long as the app is running. With REFRESH it always starts at the default (or at whatever `config.json` names — see section 25). |
| NOTE PREV | ON / OFF | Play the note at its pitch when you insert it on the PHRASE screen — useful for hearing what you're placing without pressing START. |
| VISUALIZER | SCOPE / FLAT / OCTA / OCTA.F / SPECT / SPCT.P | Visualizer mode for the top bar (see §3 for descriptions). |
| THEME | theme name > | Shows the current theme name. Press A to open the THEME EDITOR. |
| TEMPLATE | SAVE / CLEAR | SAVE stores the current project as a template for new projects. CLEAR removes the saved template. |
| ABXY | AUTO / XBOX / NINTENDO | Which face button your controller has **printed** A. Appears only while a controller is attached. **AUTO** trusts the controller and is right for a handheld's built-in pad and for a real Switch pad. Use **NINTENDO** if A is the right-hand button but the app reads it as B - common with 8BitDo pads in XInput mode, which report themselves as Xbox controllers. **XBOX** = A is the bottom button. Keyboard keys are never affected. |
| RESUME | ASK / AUTO | What happens to unsaved work after the app is killed in the background. **ASK** shows a "RECOVER WORK?" prompt on the next launch; **AUTO** silently restores the autosave (use on ROMs that kill the app when backgrounded, e.g. Miyoo Flip / Ayaneo). |

Layout and scaling mode are persisted across app restarts. The auto-detected layout on startup depends on whether physical gamepad buttons are detected.

**Phone held upright with a clip-on controller:** when a phone is in portrait and a physical
controller is attached, PocketTracker moves the tracker into the upper half of the screen instead
of centring it, so a grip like the GameSir Pocket Taco or the 8BitDo FlipPad does not cover it.
Unclip the controller or turn the phone and it centres again.

**Keeping the on-screen buttons with a controller attached:** attaching a controller normally turns
the on-screen buttons off. On a device that has both, the LAYOUT row offers **FULL** (no on-screen
buttons) and **PORTRAIT** (keep them), so you can use the touchscreen and the controller together.
BTN SOUND and BTN VIBRO appear only while the on-screen buttons are actually shown.

**Not every row is on every device.** LAYOUT, BTN SOUND and BTN VIBRO configure a touchscreen and its
virtual buttons, so they appear on Android and not on the Windows, Linux or handheld builds — where
there are no on-screen buttons to sound, shake or lay out. The rest are the same everywhere. A row
that would configure nothing is not shown rather than shown and inert.

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
| 0 — theme name | A+LEFT/RIGHT to cycle built-in themes: CLASSIC, AMBER, BLUE, MONO |
| 1 — SAVE | Press A to save the current theme to a `.ptt` file |
| 2 — LOAD | Press A to load a theme from a `.ptt` file |

Move between positions with D-pad LEFT/RIGHT.

### Rows 1–22 — Color parameters

Each row edits one color in the theme. The color preview swatch is shown on the right. Cursor moves between **R**, **G**, **B** channels with D-pad LEFT/RIGHT.

| Label | What it colors |
|---|---|
| BACKGROUND | Module fill and default row background |
| ROW 4TH | Beat-accent rows (every 4th step) |
| ROW CURSOR | The cell the cursor is on in a grid (SONG, CHAIN, PHRASE, TABLE, GROOVE); the whole row on list screens such as SETTINGS |
| ROW PLAY | The `>` playback marker. It is drawn brightened, so a dark value still reads as ink |
| ROW SELECT | The selected cells during copy/paste |
| TXT TITLE | Screen header text (e.g., "PHRASE", "INSTRUMENT") |
| TXT PARAM | Inactive parameter labels |
| TXT VALUE | Inactive parameter values |
| TXT CURSOR | Text under the cursor, and the row number and column heading that mark where it is |
| TXT EMPTY | Empty / placeholder cells |
| VIZ BG | Visualizer background |
| VIZ LINE | Visualizer center line |
| VIZ WAVE | Waveform / bar fill color |
| MTR BG | Meter background |
| MTR LOW | Meter green zone (below −6 dBFS) |
| MTR MID | Meter yellow zone (−6 to 0 dBFS) |
| MTR HIGH | Meter red zone (≥ 0 dBFS) |
| EQ BG | EQ EDITOR spectrum panel background |
| EQ FILL | Shading under the EQ EDITOR's spectrum curve |
| EQ BORDER | The EQ EDITOR's spectrum curve itself |
| EQ TXT | Frequency labels on the EQ EDITOR's spectrum |
| TXT SELECT | Text in the selected cells |

A theme file saved before these rows existed loads with them set to what the screen drew previously — the EQ rows from the theme's own colors, TXT SELECT from VIZ WAVE — so an older `.ptt` looks unchanged until you edit them.

### Controls

| Input | Action |
|---|---|
| D-pad UP/DOWN | Move between color rows |
| D-pad LEFT/RIGHT | Move between R / G / B channels (on color rows), or between theme name / SAVE / LOAD (on row 0) |
| A + LEFT/RIGHT | +1 / −1 to the selected channel |
| A + UP/DOWN | +16 / −16 to the selected channel |
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

### LAT `XX` — Latency

Delays the step trigger by `XX` ticks. All events on that row fire later than normal.

---

### GRV `XX` — Groove Assign

Switches the current track to use groove `XX` from this step onward.

---

### HOP `XY` — Hop / Jump

- In a **phrase**: ends the phrase at this step; the **next** phrase starts at row `Y` (`X` is ignored).
- In a **table**: jumps **its own FX column's** playhead to table row `Y`, `X` times before falling through (`X` = 0: forever).
- `HOP FF` in a **phrase**: **stops the track** — in SONG mode until the next song row; in PHRASE/CHAIN playback the track stays silent until you stop.
- `HOP FF` in a **table**: stops **that FX column** only. The table ends when all three have stopped.

`HOP 00` at the end of a section = infinite loop of that section.

---

### KIL `XX` — Kill

Stops the sample on this track and cancels all persistent effects (ARP, RPT, pitch effects).

`XX` delays the stop by that many ticks:

- `KIL 00` = stop on this row.
- `KIL 06` = stop half a step later (6 of 12 ticks).
- `KIL 0C` = stop one full step later (the start of the next row).

If the instrument has an **ADSR** volume envelope, KIL triggers its **release** stage instead (see §11).

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

### RPT `XY` — Repeat (Retrigger)

- **Y = 0 (simple mode):** retrigger every `X` ticks.
- **Y ≠ 0 (volume ramp mode):** retrigger every `Y` ticks.
  - `X` 1–7: decrease volume each retrig (fade-out)
  - `X` 8–F: increase volume each retrig (fade-in)

Persists across steps. Cancel with new note, new FX in same column, or KIL.

> [!WARNING]
> RPT **persists** across steps that have no note. A new note, any effect in the same FX column, or **KIL** will cancel it. Steps with a note trigger a fresh sample play and end the retrigger sequence.

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

In a **table** it moves its own FX column's playhead; in a **phrase** it moves all three.

---

### TIC `XX` — Tick Rate

Sets the tick rate of the FX column it is written in — each of the three has its own. In a table's
**last row** it sets that column's rate from the start; anywhere else it takes effect as that
column's playhead passes.

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

### PAN `XX` — Pan

Overrides the stereo pan for **this note only**. `00` = hard left, `80` = center, `FF` = hard right. The
next note on the track (without a PAN) reverts to the pan set on the INSTRUMENT screen. On an empty step it
moves the currently-playing voice.

---

### BCK `0X` — Playback Direction

Sets the sampler playback direction live (sampler instruments only):

- `BCK 00` = play **backward**. On a step with a note it starts from the sample's end.
- `BCK 01` = play **forward**.

Placed on an **empty step** it flips direction from the voice's **current position** (it does not restart),
so toggling `BCK 00` / `BCK 01` on successive rows lets you "scratch" a sample back and forth.

---

### REV `XX` — Reverb Send (per note)

Sends **this note only** to the reverb bus at level `XX` (`00`–`FF`), independent of the instrument's own
reverb send and without affecting later notes. Configure the reverb itself on the EFFECTS screen.

---

### DEL `XX` — Delay Send (per note)

Sends **this note only** to the delay bus at level `XX` (`00`–`FF`), independent of the instrument's own
delay send.

---

### EQN `XX` — EQ (per note)

Applies EQ preset slot `XX` (`00`–`7F`) to **this note only**. Edit the presets in the EQ EDITOR (§17).
A preset with all bands off = no EQ. Works in a **table row** too, like `CUT` and `RES`.

---

### EQM `XX` — EQ (mixer / master)

Switches the **master bus** EQ to preset slot `XX` (`00`–`7F`) during playback. It **persists** until the
next `EQM`, then resets to the master EQ configured on the MIXER screen when playback stops. Use it to
automate the master EQ across a song (e.g. a filter-sweep build-up). Works in a **table row** too.

---

### VTR `XX` — Track Fader

Sets the MIXER fader of **the track this effect sits on** to `XX` (`00`–`FF`). It moves the whole track,
every voice on it — not just the note on this step. The reverb and delay sends are tapped *above* the
fader, so a track faded out with `VTR` leaves its tails ringing.

It **replaces** the fader rather than scaling it, exactly as `VOL` replaces the instrument's volume, and
it **persists** until the next `VTR` on that track. The MIXER screen keeps showing the value you typed
there: the fader on screen is where the song *starts*, `VTR` is where the song has moved it to. Stopping
playback puts the fader back.

There is one `VTR`, and it always means the track it is written on — to move another track's fader, write
it in a phrase playing on that track.

---

### VMV `XX` — Master Fader

Sets the **master** fader to `XX` (`00`–`FF`) — the whole mix, including the reverb and delay returns.
Like `VTR` it replaces rather than scales, persists until the next `VMV`, and is restored when playback
stops.

It belongs to no track, so it works from any of them: a `VMV` on track 8 fades the same master fader as a
`VMV` on track 1.

---

### AUS `XX` — Automation Start · AUF `XX` — Automation Finish

`AUS` and `AUF` are a **pair**, and together they fade a parameter smoothly from one value to another
instead of stepping it once per row.

Write the parameter at its **starting value**, put `AUS` in the slot **to its right**, and put `AUF` on a
**later step** carrying the value to arrive at:

```
STEP  FX1      FX2      FX3
00    VOL 00   AUS 80            ← start at 00, linear
08             AUF FF            ← arrive at FF eight steps later
```

`AUS`'s value is the **curve**, not a level:

| Value | Shape |
|---|---|
| `00` | ease-in — slow to leave, fast to arrive |
| `80` | linear |
| `FF` | ease-out — fast to leave, slow to arrive |

Anything between blends towards the neighbouring shape, so `40` is a gentle ease-in and `C0` a gentle
ease-out.

**What can be ramped.** `AUS` automates the nearest automatable effect **to its left on the same step**,
skipping any that are not:

| Effect | What fades |
|---|---|
| `VOL` | the step volume |
| `PAN` | the stereo position |
| `REV` | the reverb send |
| `DEL` | the delay send |
| `VTR` | this track's fader |
| `VMV` | the master fader |
| `CUT` | the filter cutoff |
| `RES` | the filter resonance |
| `EQN` | this track's EQ — **between two presets** (see below) |
| `EQM` | the master EQ — **between two presets** (see below) |

In `VOL 20  PSL 40  AUS 00`, the `AUS` ramps the `VOL` — `PSL` is not automatable, so it is passed over.

**Fading between two EQ presets.** `EQN` and `EQM` are different from every other row above: their
value is a **preset number**, not a level. So the pair does not slide the number — it slides the
**contents**. Write the starting preset, `AUS` beside it, and the destination preset on the `AUF`:

```
00    EQM 05   AUS 80            ← start on preset 05, linear
08             AUF 12            ← arrive at preset 12 eight steps later
```

Over those eight steps the **FREQ, GAIN and Q of all three bands** slide from what preset 05 holds to
what preset 12 holds. The frequency sweep is even across the keyboard, not bunched at the top.

⚠️ **Give both presets the same three band TYPES.** A type cannot be faded — there is no halfway
between a BELL and a HI SHELF — so the **starting** preset's types are kept for the whole fade. Match
them and you arrive exactly on the destination preset. Leave them mismatched and that band still sweeps
its frequency and gain, but under the start's type, and the fade ends on a setting that is not quite
either preset. Nothing clicks either way.

To land on the real destination preset, write it: `EQM 12` on the step after the `AUF`.

A band switched **OFF** in the starting preset stays off for the whole fade. To fade a band out, ramp
its **GAIN to 0.0 dB** instead.

⚠️ A preset number is `00`–`7F`. An `AUF` above `7F` on an EQ fade names no preset, so it is ignored and
drawn dimmed — and the `AUS` stays open for the next `AUF` you write.

⚠️ `EQM` fades the master bus and keeps going on its own. `EQN` follows the **note** — over a silent
track there is nothing to filter, and each new note picks the fade back up a moment after it starts. For
a long, obvious EQ sweep, use `EQM`.

**A fade may cross phrases.** The `AUF` can sit in a **later phrase of the same chain**, which is how a
fade longer than one phrase is written — up to the sixteen rows of the chain. It does **not** cross into
the next chain: pairing stops at the end of the chain the `AUS` was written in.

**A fade may also be written in a TABLE**, over rows rather than steps and with a smaller set of
parameters — see §12 → *Fades in a table*.

**When nothing happens.** A cell that is not part of a working pair is drawn **dimmed**, the same way an
empty cell is. That is the editor telling you the fade will not play. The reasons are:

- `AUS` with nothing automatable to its left, or with no `AUF` after it in the chain;
- `AUF` on the **same step** as its `AUS` — the pair needs a later step to have any duration;
- a second `AUF` after the pair has already closed;
- a second `AUS` before an `AUF`, which replaces the first one — the last `AUS` wins, pairs do not nest;
- on an `EQN`/`EQM` fade, an endpoint above `7F` — it is not a preset number, so it names nothing.

One ramp runs at a time on a track, so to fade two parameters at once, write them on two tracks.

---

### CUT `XX` — Filter Cutoff · RES `XX` — Filter Resonance

Move the **instrument's own filter** on **this note only**: `CUT` sets the cutoff frequency and `RES` the
resonance, both `00`–`FF`, the same two values as the FREQ and RES cells on the INSTRUMENT screen (§9).
The next note starts from the instrument's values again.

> ⚠️ **The instrument must have a FILTER TYPE.** `CUT` and `RES` move the filter the instrument declares —
> they do not switch one on. On an instrument whose FILTER is `OFF` they do nothing at all. Set FILTER to
> `LP`, `HP` or `BP` on the INSTRUMENT screen first, and the FX column takes it from there.

Cutoff is exponential across the byte: `00` is 20 Hz, `FF` is 20 kHz, and each `+0x33` is roughly one
decade. On a low-pass, small numbers are dark and large ones are open.

Both can be **ramped** with `AUS`/`AUF`, which is what a filter sweep is:

```
    00    CUT 20   AUS 80            ← start closed, linear
    08             AUF E0            ← open over eight steps
```

Both also work in a **table**, once per tic, so a sweep written once follows every note that instrument
plays — the shortest way to give a sample a filter envelope without spending a modulation slot.

---

## 22. Modulation Reference

See §14 for how to edit mod slots. Envelope times are in **tics**, so they track project BPM.

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

Throughout this section `<home>` is PocketTracker's home folder: on Android the folder you granted it
(section 2), and elsewhere the `PocketTracker` folder the app keeps beside itself.

### Managing files in the browser

You do not need a separate file manager. Any file browser — the one PROJECT → LOAD opens, or the one
the INSTRUMENT SAMPLE field opens — can rename, delete, move and copy, using **SELECT** and **L** as
modifiers.

| Input | Action |
|---|---|
| SELECT + A | Rename the file or folder under the cursor. A keyboard opens with the current name. |
| SELECT + B | Delete it. An `A=YES  B=NO` confirm appears first — nothing is deleted on the press itself. |
| SELECT + R | Create a folder here. A keyboard opens for the name. |

**Moving and copying several files** works like the tracker's own copy/paste:

1. **L + B** starts a selection at the cursor. Tap **L + B** again inside half a second to select
   everything in the folder.
2. **D-pad UP/DOWN** stretches the selection.
3. **B** copies the selected files, or **L + A** cuts them. Either way the selection ends and the
   status line reports how many files are on the clipboard.
4. Navigate to the destination folder and press **L + A** to paste. A cut **moves** the files; a copy
   **duplicates** them.

**L + R** cancels a selection without copying anything.

Two details worth knowing:

- **A paste never overwrites.** If the destination already holds a file of that name, the new one
  arrives as `kick_2.wav`, `kick_3.wav`, and so on.
- **A cut clipboard is spent once pasted** — the originals have moved. A copied one survives, so you
  can paste the same files into several folders in a row.

> [!TIP]
> On Android the destination can be any folder you have granted, including one on an SD card — so
> this is also how you move a project or a sample between two granted folders.

### Project files

- Format: `.ptp` (JSON with version field; old projects are migrated automatically on load)
- Location: `<home>/Projects/`
- Save: PROJECT screen → SAVE
- Load: PROJECT screen → LOAD

### Sample files

- Format: `.wav` (8/16/24/32-bit PCM or float; mono or stereo)
- Stereo WAV files are supported natively — SOURCE mode on the instrument or sample editor selects LEFT / RIGHT / STEREO / MONO non-destructively
- Sample rates: any — PocketTracker compensates pitch for non-44100 Hz files automatically
- Loaded via: INSTRUMENT screen → SAMPLE field → A button → file browser
- SF2 and SF3 files are loaded the same way
- **Compressed audio** (`.mp3`, `.flac`, `.ogg`, `.opus`, `.m4a`) loads directly as a sample — decoded into memory with **no WAV file written** and no slice markers. The instrument remembers the original file path, so it is re-decoded automatically each time the project is reopened. FLAC is lossless, so it is the best choice when you want a small file with no quality loss. (`.opus` and `.m4a` are common voice-recording formats — phone memos, messaging-app exports.)
- **Video files** (`.mp4`, `.mkv`, `.webm`, `.3gp`, `.mov`) can instead be **converted** to samples — the audio track is extracted and saved as a WAV. See *Audio from video files* below.

### Audio from video files

PocketTracker can pull the audio track out of a video (or audio-container) file and turn it into a
sample — handy for grabbing a sound straight from a clip without a separate converter app.

- **Supported containers:** `.mp4`, `.mkv`, `.webm`, `.3gp`, `.mov`. The audio track is decoded
  with the device's built-in codecs. (`.m4a` is **not** here — it loads in place as a sample instead;
  see *Sample files* above.)
- **How:** INSTRUMENT (or INST_POOL) screen → **SAMPLE** field → **A** → file browser → highlight a video
  file → **A**. A keyboard appears, pre-filled with `<filename>_audio`; edit the name if you like, then
  confirm.
- **What happens:** the first audio track is extracted, **saved as a WAV** in
  `<home>/Samples/` (stereo preserved, at the file's own sample rate) and loaded into
  the current instrument. The status line shows `CONVERTED: <NAME>.WAV`.
- Because a real WAV is written, the instrument gets a normal, reusable sample file and reopens without
  re-decoding — the key difference from compressed audio (MP3 / FLAC / OGG / M4A), which is decoded in
  memory on every load.
- **Preview first:** highlight a video file and press **START** to hear it before converting.
- **Length limits:** conversion extracts up to **60 seconds**; preview is capped at **30 seconds**.
  Longer files report *"Audio too long"*. A file with no audio track — or a codec the device can't
  decode — reports an error and nothing is saved.

### WAV exports

- Format: 16-bit stereo WAV, 44100 Hz
- Location: `<home>/Renders/`
- Filenames: `ProjectName_0001.wav`, `_0002.wav`, … (auto-incremented)
- Triggered from: PROJECT screen → EXPORT → MIX

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

Output: `<home>/Samples/Resampled/Resample_0001.wav`, …

### CHOP exports

When slice markers are set in the SAMPLE EDITOR, use CHOP to export each slice as a separate WAV:

Output: `<home>/Samples/Chops/{instrument_name}/`

> [!NOTE]
> CHOP exports do not automatically load into instrument slots. After chopping, open the INSTRUMENT screen and use the file browser to load the individual slice files onto new instruments.

### The log file

- Location: `<home>/pockettracker-log.txt`
- One session, rewritten each launch, capped at 512 KB

Plain text, and readable on the device itself. It records the whole start-up — which folders were
found, how many samples loaded, which controller was detected — which is exactly what a bug report
needs and what is hardest to describe from memory. If something is wrong, attach this file.

---

## 24. Workflow Tips

### Making your first phrase

1. Open **PHRASE** (R+RIGHT from SONG).
2. Cursor on row 00, column N. Press **A** — inserts default note (C-4, instrument 00).
3. Use **A+LEFT/RIGHT** to change pitch; **A+UP/DOWN** to change octave.
4. Move to the I column, use **A+LEFT/RIGHT** to select an instrument.
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

### Fading a track in over four bars

1. On the CHAIN screen, put four phrases on rows `00`–`03`.
2. Open the phrase on row `00`. On step `00` write `VTR 00` in FX1 and `AUS 80` in FX2 — the track
   fader starts silent, on a linear curve.
3. Open the phrase on row `03`. On step `0F` write `AUF FF` — the fader arrives at full over the whole
   four bars.
4. Both cells stay lit: the pair works across the chain. Delete the `AUF` and the `AUS` dims, because
   there is nothing to fade towards.

Swap `VTR` for `VMV` to fade the whole mix instead, or for `REV` to open a reverb send.

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
3. Move cursor to EXPORT, select MIX and press **A**. The song renders offline — a status message shows the output filename when done.

### Customizing your theme

1. Navigate to SETTINGS (PROJECT → SETTINGS row → A).
2. Move to the THEME row and press **A** to open the THEME EDITOR.
3. On row 0, use A+LEFT/RIGHT on the theme name to cycle through built-in themes (CLASSIC, AMBER, BLUE, MONO).
4. Move down to any color row, then LEFT/RIGHT to select R/G/B, and A+LEFT/RIGHT to adjust.
5. When you are happy with the look, move back to row 0, move RIGHT to SAVE, and press A.

---

## 25. Configuration File (config.json)

Some things are easier to set in a text file than on a 640×480 screen. `config.json` lives in
PocketTracker's home folder, beside the `Projects` and `Samples` directories, and covers three of them:
**your controller's button layout**, **your keyboard bindings**, and **which folder a load browse
opens at**.

It is the opposite of `settings.json`, which the app writes whenever you change something on the
SETTINGS screen. **`config.json` is yours** — the app reads it once at startup and never writes to it
again.

**Finding it.** The app creates a starter copy filled in with everything at its current value — on
first launch, or on Android on the first launch after you have granted it a folder to live in. So the file already shows you the exact shape and spelling of every option, and as
seeded it changes nothing. Open it in any text editor, change what you want, and **restart the app**.

Every key is optional. Delete a line to go back to the built-in default. A missing, empty or
malformed file costs you nothing — the defaults simply stand.

### Controller button layout

```json
"controller": { "abxy": "auto" }
```

| Value | Meaning |
|---|---|
| `auto` | **Default.** Trust the controller's own report. |
| `nintendo` | The button printed **A** is the **right** one in the ABXY cluster. |
| `xbox` | The button printed **A** is the **bottom** one. |

Most controllers report their buttons correctly and `auto` is right — a handheld's built-in pad and a
real Switch Pro controller both work with nothing configured.

The exception is a pad that misreports itself. Many third-party controllers, and **8BitDo pads in
XInput mode in particular**, tell the computer they are Xbox 360 controllers. Their buttons are
printed the Nintendo way round, but the app is told the Xbox layout, and the result is unmistakable:
**A and B do each other's jobs** (as do X and Y).

Set `"abxy": "nintendo"` and both pairs swap back together.

This can't be detected automatically — the controller is answering the question wrongly, and nothing
downstream can tell. That is why it's a setting.

> **On a handheld:** PortMaster already offers an x360/nintendo choice that does the same thing. Use
> one or the other. Setting **both** swaps twice and leaves you exactly where you started.

### Keyboard bindings

Relevant on the Windows and Linux desktop builds. The defaults are:

```json
"keyboard": {
  "DPAD_UP":    ["W", "Up"],
  "DPAD_DOWN":  ["S", "Down"],
  "DPAD_LEFT":  ["A", "Left"],
  "DPAD_RIGHT": ["D", "Right"],
  "A":      ["K", "Return"],
  "B":      ["J", "Escape"],
  "L":      ["U"],
  "R":      ["I"],
  "SELECT": ["Left Shift"],
  "START":  ["Space"]
}
```

Those ten names are the complete set. Each takes a list of keys of any length.

**A button you list replaces its defaults. A button you leave out keeps them.** This matters when you
want a key that's already in use: to put `K` somewhere else you must also rebind `A`, or `K` stays
attached to it. To free a button entirely, list it as `[]`.

**Key names** are SDL's, and their spelling is not guessable — some are spaced and some are not.
Copy them from here:

| | |
|---|---|
| Letters, digits | `"K"` `"Z"` `"1"` — always capitalised |
| Modifiers | `"Left Shift"` `"Right Shift"` `"Left Ctrl"` `"Right Ctrl"` `"Left Alt"` `"Left GUI"` |
| Editing | `"Return"` `"Escape"` `"Space"` `"Tab"` `"Backspace"` `"Insert"` `"Delete"` |
| Arrows | `"Up"` `"Down"` `"Left"` `"Right"` |
| Navigation | `"Home"` `"End"` `"PageUp"` `"PageDown"` — note: **no space** |
| Locks etc. | `"CapsLock"` `"PrintScreen"` — also no space |
| Function | `"F1"` … `"F12"` |
| Keypad | `"Keypad 0"` … `"Keypad 9"`, `"Keypad Enter"`, `"Keypad +"` — these **do** have a space |

If you're unsure, the starter file the app wrote is the reliable reference: anything appearing there
is known to work.

**If a rebind doesn't seem to happen, read the app's log.** An unrecognised key name is reported by
name and skipped; that one binding is lost and nothing else is. `"Ctrl"` is the usual culprit — it
has to be `"Left Ctrl"` or `"Right Ctrl"`.

Binding the same key to two buttons isn't rejected, but only one of them will fire. Avoid it.

On Android the hardware **Back** key is always **B**, whatever this section says — so a file you
edited on a desktop can't leave you unable to back out of a screen on a phone.

### Default folders

```json
"folders": {
  "samples":     "Samples",
  "soundfonts":  "Soundfonts",
  "instruments": "Instruments",
  "projects":    "Projects",
  "themes":      "Themes"
}
```

Sets the folder each **load** browse starts in. If your samples live somewhere outside the
PocketTracker folder, this saves climbing out of it every time.

**A plain name is inside your PocketTracker folder**, and can be several levels deep:
`"samples": "Samples/Packs/Breaks"` starts a sample load two folders down. This is how the starter
file is written, and it is what makes the file portable — copy it to another device and it still
means the same thing.

**A path beginning with `/` (or a Windows drive, `C:\Music`) is used exactly as written**, so a
sample library that lives nowhere near PocketTracker is still one line away.

A folder that doesn't exist — or that this device cannot read — is ignored, and that category quietly
falls back to its default. A typo, a folder you've since moved, or a config file carried over from
another machine costs one convenience, never a broken file browser.

**A path written under someone else's PocketTracker folder still works.** If the file came off another
device, or was written before this one moved its home folder, a value like
`/storage/emulated/0/Documents/PocketTracker/Samples` is matched up to its `PocketTracker/` part and
re-read against *your* home folder — the same thing that happens to the sample paths inside a project
you copy between devices. Only the part below `PocketTracker/` is kept, so a folder that was never
inside one falls back to the default instead of being guessed at.

> **On Android**, a folder outside your home folder has to be granted before the app can see it —
> `ADD FOLDER...` at the top of the browser (section 2). Reaching one that way is easier than naming
> it here: the granted path is not something you can type, and a plain `/storage/emulated/0/...` path
> is not readable at all unless it is inside the folder you granted.

This changes only where browsing **starts**. It does not move where anything is **saved** — renders,
exports and sample-editor saves keep their own folders.

> **Related:** the SETTINGS screen's **FOLDER** row (`REMEMBER` / `REFRESH`) is a separate, smaller
> convenience — with `REMEMBER`, a sample load reopens at the folder you last loaded a sample from,
> for that session. It takes priority over `folders.samples` once you've loaded something.

---

## Appendix: Controls Cheat Sheet

*Print this page and keep it handy.*

---

### UNIVERSAL — work on every screen

| Input | Action |
|---|---|
| D-pad | Move cursor |
| A | Insert / confirm |
| A + LEFT / RIGHT | Edit value (+1 / −1) |
| A + UP / DOWN | Edit value (+16 / −16, or ±1 octave for notes) |
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
| EQ EDITOR | INSTRUMENT / INST.POOL / MIXER / EFFECTS → cursor on EQ → A (or SELECT); close with B (or SELECT) |
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
| A + LEFT / RIGHT *(in selection)* | Increment / decrement all selected values |
| L + B + A | Deep-clone the chain / phrase under the cursor |
| L + R *(in selection)* | Leave selection mode; the copy buffer survives |
| L + R *(outside selection)* | Clear the copy buffer |

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
| A on `..`, or R + LEFT | Go up one directory level |
| B | Close the browser |
| START | Preview highlighted WAV |
| SELECT + A | Rename |
| SELECT + B | Delete (asks first) |
| SELECT + R | New folder |
| L + B | Start a file selection (tap again: select all) |
| B *(in selection)* | Copy the selected files |
| L + A *(in selection)* | Cut them |
| L + A *(outside selection)* | Paste them here |
| L + R | Cancel the file selection |

---

### SONG SCREEN

| Input | Action |
|---|---|
| A | Insert last-used chain ID |
| A + LEFT / RIGHT | ±1 chain ID |
| A + UP / DOWN | ±16 chain IDs |
| A + B | Delete (set to --) |
| B + UP / DOWN | Page up / down (16 rows) |

---

### CHAIN SCREEN

| Input | Action |
|---|---|
| A | Insert last-used phrase / TSP |
| A + LEFT / RIGHT | ±1 |
| A + UP / DOWN | ±16 (PHR), ±12 semitones (TSP) |
| A + B | Delete slot |
| B + LEFT / RIGHT | Previous / next chain |

---

### PHRASE SCREEN

| Input | Action |
|---|---|
| A | Insert last-used note / value |
| A + LEFT / RIGHT | ±1 semitone (note), ±1 (other) |
| A + UP / DOWN | ±1 octave (note), ±16 (other) |
| A + B | Delete value |
| B + LEFT / RIGHT | Previous / next phrase |

---

### INSTRUMENT SCREEN

| Input | Action |
|---|---|
| A (on SAMPLE) | Open file browser |
| SELECT (on SAMPLE) | Open SAMPLE EDITOR |
| A or SELECT (on NAME) | Edit instrument name |
| A or SELECT (on EQ) | Open EQ EDITOR |
| A + LEFT / RIGHT | Edit current parameter |
| A + B | Reset to default |
| B + LEFT / RIGHT | Previous / next instrument |

---

### SAMPLE EDITOR

| Input | Action |
|---|---|
| R + UP / DOWN | Zoom in / out, from any row |
| A + LEFT / RIGHT | Zoom in / out (cursor on the ZOOM cell) |
| D-pad LEFT / RIGHT | Scroll (when zoomed) |
| D-pad (on marker row) | Move selection marker |
| A + D-pad (on slice position) | Move the boundary — UP/DOWN fine, LEFT/RIGHT coarse |
| A + B (on slice row) | Delete the boundary (MANUAL), or reset it (TRANSIENT / DIVIDE) |
| A (on slice row, while playing) | Cut a boundary at the playhead (MANUAL) |
| A or SELECT (on EQ-effect slot) | Open EQ EDITOR |
| START | Preview sample |
| B | Close (asks first if there are unsaved changes) |

---

### TABLE SCREEN

| Input | Action |
|---|---|
| A + LEFT / RIGHT | Edit value |
| A + B | Delete value |
| B + LEFT / RIGHT | Previous / next table |
| L + B | Enter selection mode |

---

### GROOVE SCREEN

| Input | Action |
|---|---|
| D-pad UP / DOWN | Move between rows |
| A + LEFT / RIGHT | Edit tick value |
| A + UP / DOWN | Large step |
| A + B | Clear row |
| B + LEFT / RIGHT | Previous / next groove |

---

### MODULATION SCREEN

| Input | Action |
|---|---|
| D-pad UP / DOWN | Move between parameters |
| D-pad LEFT / RIGHT | Switch between paired slots |
| A + LEFT / RIGHT | Edit value |
| A + UP / DOWN | Large step |
| A + B | Reset to default |
| B + LEFT / RIGHT | Previous / next instrument |

---

### MIXER SCREEN

| Input | Action |
|---|---|
| D-pad LEFT / RIGHT | Select track column |
| D-pad UP / DOWN | Move between rows |
| A + LEFT / RIGHT | ±1 |
| A + UP / DOWN | ±16 |
| A or SELECT (on master EQ) | Open EQ EDITOR |

---

### EQ EDITOR

Open with **A** (or SELECT) on an EQ cell.

| Input | Action |
|---|---|
| D-pad LEFT / RIGHT | Switch between bands 1–3 |
| D-pad UP / DOWN | Move between parameters |
| A + LEFT / RIGHT | Edit value |
| A + UP / DOWN | Large step |
| B + LEFT / RIGHT | Switch EQ preset slot |
| B | Close and apply (SELECT also closes) |

---

### THEME EDITOR

| Input | Action |
|---|---|
| D-pad UP / DOWN | Move between color rows |
| D-pad LEFT / RIGHT | Move between R / G / B (color rows) or name / SAVE / LOAD (row 0) |
| A + LEFT / RIGHT | ±1 to selected channel |
| A + UP / DOWN | ±16 to selected channel |
| B | Close |

---

### EFFECTS QUICK REFERENCE

| Code | Name | Value | Notes |
|---|---|---|---|
| ARP | Arpeggio | `XY` = intervals | Persists — cancel with `ARP 00` |
| ARC | Arpeggio Config | `XY` | High nibble=mode (0=UP 1=DN 2=PP 3=RND), low=speed |
| CHA | Chance | `XY` | X=probability (0=never F=always), Y=target (0=note 1=FX1 2=FX2 3=FX3) |
| LAT | Latency | `XX` ticks | Delays row trigger |
| GRV | Groove | `XX` | Assigns groove to this track |
| HOP | Hop/Jump | `XY` | Phrase: next phrase starts at row Y (FF=stop track). Table: jump to row Y, X times (0=forever) |
| KIL | Kill | `XX` ticks | Stop after XX ticks (00=now, 0C=next step) |
| OFF | Offset | `XX` | Sample start position jump |
| PIT | Pitch Offset | `XX` signed | 00–7F up, 80–FF down |
| PSL | Pitch Slide | `XX` ticks | Portamento |
| PBN | Pitch Bend | `XX` | 00–7F up, 80–FF down — **persists** |
| PVB | Vibrato | `XY` | X=speed, Y=depth — **persists** |
| PVX | Extreme Vibrato | `XY` | 4× deeper, 2× faster than PVB |
| RPT | Repeat/Retrigger | `XY` | Y=0: every X ticks; Y≠0: fade — **persists** |
| RND | Randomize | `XY` | Randomizes previous FX value |
| RNL | Randomize Left | `XY` | Randomizes FX in column to the left |
| SLI | Slice Index | `XX` | Direct slice selection |
| TBL | Table Set | `XX` | Override instrument's table |
| THO | Table Hop | `XX` | Jump table to row 0X |
| TIC | Tick Rate | `XX` | Table speed (00=trigger 06=default FC=octave FE=note FF=200Hz) |
| VOL | Volume | `XX` | Immediate volume at this tick |
| PAN | Pan | `XX` | Per-note pan (00=L 80=center FF=R); next note reverts |
| BCK | Direction | `0X` | Sampler: 00=reverse 01=forward; flip live to scratch |
| REV | Reverb Send | `XX` | Per-note reverb send level |
| DEL | Delay Send | `XX` | Per-note delay send level |
| EQN | EQ (note) | `XX` | Per-note EQ preset slot (00–7F) |
| EQM | EQ (mixer) | `XX` | Master EQ preset slot; holds till next EQM, resets on stop |
| VTR | Track Fader | `XX` | This track's MIXER fader; replaces it, **persists**, restored on stop |
| VMV | Master Fader | `XX` | The master fader, from any track; replaces it, **persists**, restored on stop |
| AUS | Automation Start | `XX` = curve | Fades the automatable effect to its LEFT (00=ease-in 80=linear FF=ease-out) |
| AUF | Automation Finish | `XX` | Destination value; a later step, may be a later phrase of the same chain |
| CUT | Filter Cutoff | `XX` | This note's filter cutoff (20 Hz–20 kHz, log). Needs a FILTER TYPE on the instrument |
| RES | Filter Resonance | `XX` | This note's filter resonance. Needs a FILTER TYPE on the instrument |

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

*PocketTracker is open-source (GPL-3.0). Contributions and bug reports welcome.*
