# PocketTracker — Quick Start Guide

From a fresh install to a playing beat in about ten minutes. This guide covers the minimum: install the app, load a few samples, and sequence your first pattern. Everything here is explained in depth in the [full manual](manual-en.md).

---

## 1. Install

**Recommended — Obtainium:** install [Obtainium](https://github.com/ImranR98/Obtainium), then add PocketTracker from the badge in the [README](../README.md) (or add `https://github.com/conanizer/pockettracker` as an app source inside Obtainium). Updates arrive automatically.

**Manual:** download the latest `.apk` from the [releases page](https://github.com/conanizer/pockettracker/releases), open it on your device, and tap **Install** (allow "install from unknown sources" if asked).

PocketTracker asks for **no permissions**. Instead, the first time you open a file browser it shows one row — `ADD FOLDER...` — and pressing **A** on it opens Android's folder picker. The folder you choose becomes PocketTracker's home, and it creates `Projects/`, `Samples/`, `Renders/`, `Soundfonts/`, `Instruments/` and `Themes/` inside it. `Documents/PocketTracker` is a tidy choice; if you have used PocketTracker before, choose that same folder and everything is where you left it.

---

## 2. The five controls you need

PocketTracker is built for gamepad buttons (phones get the same buttons on screen; a Bluetooth keyboard works too — see the [controls reference](input-system.md)).

| Button | What it does |
|---|---|
| **D-pad** | Move the cursor |
| **A** | Insert a value; **hold A + D-pad** edits the value under the cursor (LEFT/RIGHT = small step, UP/DOWN = big step) |
| **B** | Delete a value / go back |
| **R + D-pad** | Switch screens — the mini-map in the top-right shows where you are |
| **START** | Play / stop |

The main screens sit side by side on one row: **SONG · CHAIN · PHRASE · INSTRUMENT · TABLE**. Hold **R** and tap LEFT/RIGHT to move between them.

---

## 3. Get some samples onto your device

PocketTracker ships with **no bundled sounds** — every instrument slot starts empty, and you fill them with your own files. Copy a few one-shot samples (a kick, a snare, a hi-hat to start) into the `Samples/` folder inside the folder you granted above. Anything outside it works too — climb to the top of the browser with **R+LEFT**, and `ADD FOLDER...` will grant PocketTracker a second folder anywhere on the device.

Supported sample formats: **WAV**, MP3, FLAC, OGG, Opus, M4A. **SoundFont (SF2)** files work too, and give you multi-sampled instruments from a single file. Any free sample pack or SF2 from the internet is fine.

> [!TIP]
> No samples at hand? Screen-record a few seconds of anything on your phone — PocketTracker converts video files (`.mp4`, `.mkv`, `.webm`, …) to samples right in its file browser.

---

## 4. Make a place for the music

PocketTracker works from the top down. The song holds chains, a chain holds phrases, and a phrase holds
the sixteen steps you write notes into:

```
SONG  (8 tracks)  →  CHAIN  (list of phrases)  →  PHRASE  (16 steps)
```

The app opens on the **SONG** screen. Make one of each before you load a sound:

1. The cursor starts on track 1, row `00`. Press **A** — a chain appears.
2. Hold **R** and tap RIGHT — you are inside that chain. Press **A** on slot `00` — a phrase appears.
3. Hold **R** and tap RIGHT again — you are inside that phrase, an empty 16-step grid.

> [!IMPORTANT]
> **The app follows the song.** Holding **R** and tapping RIGHT only opens a chain the song is actually
> using, and a phrase that chain is actually using — so place it first, then step into it. On an empty
> cell the press simply does nothing, and the empty cell under the cursor is the reason.
> (`SETTINGS → NAV = POOL` turns this off and lets you reach all 256 slots directly.)

---

## 5. Load your first instrument

1. From PHRASE, hold **R** and tap RIGHT once more to reach the **INSTRUMENT** screen.
2. The top row is **TYPE**, and it carries two buttons to the right of its value: **LOAD** and **EDIT >**.
   Move the cursor right onto **LOAD** and press **A** — the file browser opens.
3. Navigate to your samples: **A** enters a folder, **B** goes up one level, **START** previews the
   highlighted file.
4. Press **A** on your kick sample to load it into instrument `00`.
5. Press **START** — you should hear it.

Now load the rest of the kit: press **B + RIGHT** to switch to instrument `01`, load the snare the same
way, then `02` for the hi-hat.

> [!TIP]
> If a melodic sample plays in the wrong key, set **ROOT** to the pitch the sample was recorded at — it
> is the most important tuning parameter.

> [!TIP]
> **EDIT >**, beside LOAD, opens the [sample editor](manual-en.md#11-sample-editor-screen) — trimming,
> slicing, chopping and time-stretching all live in there.

---

## 6. Write your first phrase

Hold **R** and tap LEFT to go back to the **PHRASE** screen — the one you made in step 4. A phrase is a
16-step pattern, one bar of music, read downwards.

1. With the cursor on step `00`, note column, press **A** — it inserts `C-4` playing instrument `00`
   (your kick).
2. Move down to step `04`. Press **A** again, then move right to the **I** column and hold **A + RIGHT**
   to change the instrument to `01` (snare).
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

   Pressing **A** on an empty step repeats the last note you placed — place the first hat, and the rest
   are two button presses each.
4. Press **START** — the phrase plays and loops. Edit while it plays; changes are live.

On the note column, **hold A + LEFT/RIGHT** moves in semitones and **A + UP/DOWN** in octaves — that's
all you need to turn a copy of this workflow into a bassline or melody later.

---

## 7. Grow it into a song

You already have the structure from step 4 — one chain on track 1, one phrase inside it. Now make it
longer.

1. Hold **R** and tap LEFT to reach the **CHAIN** screen. Press **A** on slot `01` to add a second
   phrase to the chain, and again on `02` and `03` if you want a longer section. A chain loops back to
   slot `00` after its last filled slot.
2. Use the **TSP** column to play the same phrase transposed — `0C` is one octave up. One phrase can be
   a verse and a chorus without being copied.
3. Hold **R** and tap LEFT again for the **SONG** screen. Press **A** on track 2, row `00` to start a
   second track, then walk right into it the same way as step 4 and give it a bassline.
4. Press **START** — the song plays from the top.

Each of the 8 tracks is a column, and all 8 start together when you press START. From then on each one
moves at its own pace: a 2-row chain beside a 16-row one simply comes round eight times. A `>` marker
shows where each track has got to.

---

## 8. Save your work

1. From SONG, hold **R** and tap UP to open the **PROJECT** screen.
2. Cursor on **NAME**, press **A** to name the project.
3. Cursor on **SAVE**, press **A**.

Projects are saved as `.ptp` files in `Projects/`, inside the folder you granted. When your track is finished, **EXPORT — MIX** on the same screen renders it to a WAV in `Renders/`.

---

## Where to go next

- **Copy / paste and selections** — [manual §5.6](manual-en.md#56-copy--paste): duplicate phrases and build variations fast.
- **Step effects** — [manual §21](manual-en.md#21-effects-reference): `ARP`, `RPT` (retrigger), pitch slides, per-note reverb/delay sends — three FX columns per step.
- **Sample editor** — [manual §11](manual-en.md#11-sample-editor-screen): trim, repitch, time-stretch, slice breaks, offline FX.
- **Modulation** — [manual §14](manual-en.md#14-modulation-screen): envelopes and LFOs per instrument.
- **Mixer & master bus** — [manual §15](manual-en.md#15-mixer-screen) / [§16](manual-en.md#16-effects-screen): levels, sends, OTT/DUST.
- **Full controls cheat sheet** — [input-system.md](input-system.md).
