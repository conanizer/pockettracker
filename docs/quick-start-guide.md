# PocketTracker — Quick Start Guide

From a fresh install to a playing beat in about ten minutes. This guide covers the minimum: install the app, load a few samples, and sequence your first pattern. Everything here is explained in depth in the [full manual](manual-en.md).

---

## 1. Install

**Recommended — Obtainium:** install [Obtainium](https://github.com/ImranR98/Obtainium), then add PocketTracker from the badge in the [README](../README.md) (or add `https://github.com/conanizer/pockettracker` as an app source inside Obtainium). Updates arrive automatically.

**Manual:** download the latest `.apk` from the [releases page](https://github.com/conanizer/pockettracker/releases), open it on your device, and tap **Install** (allow "install from unknown sources" if asked).

On first launch PocketTracker asks for **All Files Access**. Grant it — without it the app cannot load samples or save projects. If you accidentally denied it: Android Settings → Apps → PocketTracker → Permissions.

---

## 2. The five controls you need

PocketTracker is built for gamepad buttons (phones get the same buttons on screen; a Bluetooth keyboard works too — see the [controls reference](input-system.md)).

| Button | What it does |
|---|---|
| **D-pad** | Move the cursor |
| **A** | Insert a value; **hold A + D-pad** edits the value under the cursor (UP/DOWN = small step, LEFT/RIGHT = big step) |
| **B** | Delete a value / go back |
| **R + D-pad** | Switch screens — the mini-map in the top-right shows where you are |
| **START** | Play / stop |

The main screens sit side by side on one row: **SONG · CHAIN · PHRASE · INSTRUMENT · TABLE**. Hold **R** and tap LEFT/RIGHT to move between them.

---

## 3. Get some samples onto your device

PocketTracker ships with **no bundled sounds** — every instrument slot starts empty, and you fill them with your own files. Copy a few one-shot samples (a kick, a snare, a hi-hat to start) to anywhere on your device's storage — `Documents/PocketTracker/Samples/` is a tidy place for them.

Supported sample formats: **WAV**, MP3, FLAC, OGG, Opus, M4A. **SoundFont (SF2)** files work too, and give you multi-sampled instruments from a single file. Any free sample pack or SF2 from the internet is fine.

> [!TIP]
> No samples at hand? Screen-record a few seconds of anything on your phone — PocketTracker converts video files (`.mp4`, `.mkv`, `.webm`, …) to samples right in its file browser.

---

## 4. Load your first instrument

1. Hold **R** and tap RIGHT until you reach the **INSTRUMENT** screen.
2. Move the cursor to the **SAMPLE** row and press **A** — the file browser opens.
3. Navigate to your samples: **A** enters a folder, **B** goes up one level, **START** previews the highlighted file.
4. Press **A** on your kick sample to load it into instrument `00`.
5. Press **START** — you should hear it.

Now load the rest of the kit: press **B + RIGHT** to switch to instrument `01`, load the snare the same way, then `02` for the hi-hat.

> [!TIP]
> If a melodic sample plays in the wrong key, set **ROOT** to the pitch the sample was recorded at — it is the most important tuning parameter.

---

## 5. Write your first phrase

A **phrase** is a 16-step pattern — one bar of music. Hold **R** and tap LEFT to move from INSTRUMENT to the **PHRASE** screen.

1. With the cursor on step `00`, note column, press **A** — it inserts `C-4` playing instrument `00` (your kick).
2. Move down to step `04`. Press **A** again, then move right to the **I** column and hold **A + UP** to change the instrument to `01` (snare).
3. Fill out a basic boom-bap bar (N = note, I = instrument):

   | Step | Note | I | Sound |
   |---|---|---|---|
   | `00` | C-4 | 00 | kick |
   | `02` | C-4 | 02 | hat |
   | `04` | C-4 | 01 | snare |
   | `06` | C-4 | 02 | hat |
   | `08` | C-4 | 00 | kick |
   | `0A` | C-4 | 02 | hat |
   | `0C` | C-4 | 01 | snare |
   | `0E` | C-4 | 02 | hat |

   Pressing **A** on an empty step repeats the last note you placed — place the first hat, and the rest are two button presses each.
4. Press **START** — the phrase plays and loops. Edit while it plays; changes are live.

On the note column, **hold A + UP/DOWN** moves in semitones and **A + LEFT/RIGHT** in octaves — that's all you need to turn a copy of this workflow into a bassline or melody later.

---

## 6. Chain it into a song

Phrases are arranged into **chains**, and chains are arranged on the 8-track **SONG** grid:

```
SONG  (8 tracks)  →  CHAIN  (list of phrases)  →  PHRASE  (16 steps)
```

1. Hold **R** and tap LEFT to move to the **CHAIN** screen. Press **A** on slot `00` — it references phrase `00`, the one you just wrote. Add more slots below to make a longer section, or leave it as one looping bar.
2. Hold **R** and tap LEFT again to reach the **SONG** screen. Press **A** on track 1, row `00` — it places chain `00`.
3. Press **START** — the song plays from the top.

That's a complete, playing song structure. Drums on track 1; put a bassline chain on track 2, chords on track 3 — each of the 8 tracks is a column, and every row plays all 8 tracks together.

---

## 7. Save your work

1. From SONG, hold **R** and tap UP to open the **PROJECT** screen.
2. Cursor on **NAME**, press **A** to name the project.
3. Cursor on **SAVE**, press **A**.

Projects are saved as `.ptp` files in `/Documents/PocketTracker/Projects/`. When your track is finished, **EXPORT — MIX** on the same screen renders it to a WAV in `/Documents/PocketTracker/Renders/`.

---

## Where to go next

- **Copy / paste and selections** — [manual §5.6](manual-en.md#56-copy--paste): duplicate phrases and build variations fast.
- **Step effects** — [manual §21](manual-en.md#21-effects-reference): `ARP`, `RPT` (retrigger), pitch slides, per-note reverb/delay sends — three FX columns per step.
- **Sample editor** — [manual §11](manual-en.md#11-sample-editor-screen): trim, repitch, time-stretch, slice breaks, offline FX.
- **Modulation** — [manual §14](manual-en.md#14-modulation-screen): envelopes and LFOs per instrument.
- **Mixer & master bus** — [manual §15](manual-en.md#15-mixer-screen) / [§16](manual-en.md#16-effects-screen): levels, sends, OTT/DUST.
- **Full controls cheat sheet** — [input-system.md](input-system.md).
