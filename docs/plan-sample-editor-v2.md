# Sample Editor — Feature Plan v2

**Created:** 2026-05-06
**Supersedes:** plan-sample-editor.md (v1, 2026-02-18)
**Scope:** Complete functional, UI, and controls specification.

---

## What Changed from v1

| Topic | v1 | v2 |
|---|---|---|
| Selection range | 00–FF (8-bit normalized) | 00000000–FFFFFFFF (32-bit sample frames) |
| End value default | always FFFFFFFF | actual sample length |
| Selection ops | crop, norm, fade, silence, reverse, invert, downsample | crop, copy, cut, duplicate, paste, delete, norm, fade, silence, reverse, undo |
| Zero-crossing detection | not present | SNAP toggle (row 2) |
| Whole-sample DSP | downsample only | OTT, DUST, DRIVE, EQ via FX row |
| Repitch | post-MVP deferral | PITCH field (row 2) |
| BPM detection | not present | DURATION field (row 2) |
| Slice storage | project file only | WAV `cue ` chunk + project file |
| Save options | save/overwrite/rename | LOAD / SAVE / OVERWRITE (row 19) |
| Save slices | not present | save each slice as separate file |
| Spectrum view | included (bottom bar) | not prioritised — revisit post-v2 |
| Loop visualization | included | not prioritised — revisit post-v2 |
| Note-to-slice playback | included (phase 9) | still included — architecture unchanged |
| UI layout | not specified | 20-row full-screen layout (see §10) |
| Controls | not specified | documented in §11 |

---

## 1. Waveform Visualization

- **Min/max decimation** using KissFFT integration.
  - C++ function takes sampleId, viewStart, viewEnd, numBins → returns float[bins*2] (min, max per column).
  - One JNI call per frame refresh, constant output size regardless of sample length.
- **Zoom (ZOOM field, row 1)**: step-based multiplier — 1×, 2×, 4×, 8×, 16×, … A+DPAD to edit. Each step halves the visible frame range. Minimum visible window: 64 frames.
- **Channel selection (SOURCE field, row 1)**: LEFT, RIGHT, STEREO (mix). Stored per editor session, not persisted.
  - When a stereo WAV was loaded, the engine retains both channels in a side buffer for the editor session.
  - Switching channel reloads the working buffer from that side buffer.
  - All edits and saves act on the currently selected channel view.
- **Selection visualization**: selected region (`selectionStart` to `selectionEnd`) drawn in white; unselected portions drawn in gray. Full sample is white when no selection is active.
- **Display**: center-line at 0, waveform drawn as filled min→max vertical line per pixel column.
- **Markers**: S (start) and E (end) selection markers shown on the waveform. Slice markers shown as vertical tick marks. The active marker is highlighted.
- **Waveform reacts to UI state**: ZOOM and SOURCE changes update the waveform immediately. When SLICE mode is TRANSIENT or DIVIDE, slice boundaries are overlaid as tick marks. When loop points are shown (future), they overlay the same canvas.

---

## 2. Selection System

### 2.1 Selection Range Format

- `selectionStart: Long` — frame index (0-based), inclusive.
- `selectionEnd: Long` — frame index, exclusive. Initialized to `totalFrames`, not a constant.
- Display: 8-digit hex, e.g. `0000A000 – 0000F000`, length `00005000`.
- Range is validated: `selectionStart < selectionEnd`, both within `[0, totalFrames]`.

### 2.2 Zero-Crossing Snap (SNAP)

- When SNAP = ON (row 2), cursor movement and selection boundary placement snap to the nearest zero-crossing in the sample data.
- A zero crossing is defined as a sign change between adjacent samples (or exact zero).
- Snap algorithm: from the current frame, scan ±N frames (e.g. N = 512) for the nearest crossing, pick the closest one.
- Selection start and end both snap independently.
- **Purpose:** prevents audible clicks when looping, cropping, or playing a region that starts/ends mid-cycle.
- **Scope:** cursor mode only — does not destructively alter sample data.

---

## 3. Selection Operations

All operations act on `[selectionStart, selectionEnd)`. If no selection is active, the operation acts on the whole sample. Operations are destructive (modify the in-memory buffer immediately). Changes are permanent only when saved. Every destructive operation can be undone once via the UNDO button.

### 3.1 Crop

- Discards everything outside the selection.
- Result: sample contains only `[selectionStart, selectionEnd)`.
- `totalFrames` = `selectionEnd - selectionStart`.
- Selection cleared after crop.

### 3.2 Copy

- Copies the selected region to an internal sample clipboard (separate from the phrase clipboard).
- Does not modify the sample buffer.
- Clipboard is in-memory only; not persisted across sessions.

**Paste behaviour:**
- **No selection (cursor only):** inserts clipboard content at the cursor position, shifting existing data right. `totalFrames` increases by clipboard length.
- **Region selected:** replaces the selection with clipboard content. If clipboard length differs from selection length, `totalFrames` changes accordingly.

### 3.3 Cut

- Copies the selection to the internal sample clipboard (same as Copy), then deletes the selection from the buffer.
- Equivalent to Copy + Delete in one action.
- Remaining data is shifted left; `totalFrames` decreases by `selectionEnd - selectionStart`.
- After cut: selection cleared.

### 3.4 Duplicate

- Makes a copy of the selected region and inserts it immediately after `selectionEnd`, shifting everything right.
- `totalFrames` increases by `selectionEnd - selectionStart`.
- After duplicate, the inserted copy is selected (so the user can immediately move or operate on it).
- Does not modify the sample clipboard.

### 3.5 Paste

- See §3.2 for paste behaviour. Linked to the Copy/Cut clipboard.

### 3.6 Delete

- Removes the selected region from the sample buffer. Remaining data shifts left.
- `totalFrames` decreases by `selectionEnd - selectionStart`.
- After delete: selection cleared.

### 3.7 Normalise (NORM)

- Scans the selection for peak absolute amplitude.
- If peak < 0.999 and peak > 0.0001, scales all samples in region by `1.0 / peak`.
- No-op if already normalised or silent.

### 3.8 Fade In (FADE+)

- Cosine fade applied to the selection: amplitude 0→1 over the selected range.
- `gain = 0.5 * (1 - cos(π * t))` where t = position within selection, 0→1.

### 3.9 Fade Out (FADE-)

- Cosine fade: amplitude 1→0 over the selected range.
- `gain = 0.5 * (1 + cos(π * t))`.

### 3.10 Silence (SLNC)

- Replaces the selected region with zero-amplitude samples.
- `totalFrames` is unchanged — length preserved, audio content erased.
- Use case: remove a transient or unwanted sound while keeping timing intact.
- No-op if no selection is active.

### 3.11 Reverse

- Reverses the sample order within the selected region in-place.
- `totalFrames` is unchanged. Does not clear selection.

### 3.12 Undo

- Restores the sample buffer from the single-level snapshot (see §8.2).
- The restored buffer becomes the new snapshot, so a second UNDO re-applies the undone operation (toggle behavior).
- Only one undo level. A subsequent destructive operation overwrites the snapshot.

---

## 4. Whole-Sample DSP Effects (FX Row)

The FX row (row 16) applies a single selected effect to the entire sample buffer. Press APPLY (A button on the APPLY column) to execute destructively. The operation can be undone with UNDO.

Four effect types are available, cycled via A+DPAD on the EFFECT column:

### 4.1 OTT

- 3-band bidirectional compressor — same OTT module used on the master bus.
- Parameter (00-FF): depth/wet amount. 00 = bypass, FF = full OTT.
- Applied to the whole sample buffer offline (one-pass, same processing as real-time master bus).

### 4.2 DUST

- DUST effect — same DUST module used on the master bus.
- Parameter (00-FF): effect intensity.
- Applied to the whole sample buffer.

### 4.3 DRIVE

- Soft-clip waveshaper. Formula: `y = tanh(drive * x)` or equivalent.
- Parameter (00-FF): drive amount. 00 = clean, FF = heavy saturation.
- Applied to full sample buffer.

### 4.4 EQ

- Uses the existing EQ editor screen (same screen used in mixer and effects screens).
- Parameter (00-FF): EQ preset number. A+DPAD on the value column cycles presets without opening the EQ screen.
- **SELECT on the FX row (when EQ is selected):** opens the EQ editor screen. START inside the EQ screen plays the sample as a preview. SELECT inside the EQ screen applies the EQ destructively and returns to the sample editor. B inside the EQ screen cancels.
- **APPLY on the FX row:** applies the current EQ preset directly without opening the EQ screen.
- Implementation: offline biquad filter pass over the full sample buffer, same biquad code as `filter.h`.

---

## 5. BPM Detection

### 5.1 Purpose

Given a loop sample of known musical length, calculate its BPM.

### 5.2 User Flow

1. User sets DURATION (row 2) to the sample's bar count: `4 BAR`, `2 BAR`, `1 BAR`, `1/2`, `1/4`, `1/8`, `1/16`, `1/32`.
2. System calculates BPM from `totalFrames` and the assumed bar length at 4/4 time.
3. Formula: `BPM = (numBars * 4 * 60 * sampleRate) / totalFrames`
   - e.g. 1 bar, 44100 Hz, 88200 frames → 60 BPM
   - e.g. 1 bar, 44100 Hz, 44100 frames → 120 BPM
4. Detected BPM displayed in the header (row 0) alongside duration.

### 5.3 Integration with Repitch (PITCH)

- The PITCH field (row 2) sets a target pitch change in semitones for destructive repitch.
- Set DURATION to the sample's bar count, set PITCH to the desired transposition in semitones.
- Repitch algorithm: linear interpolation identical to the audio engine's real-time pitch-shifting.
- `totalFrames` changes proportionally after repitch (half speed = double length, etc.).

### 5.4 Notes

- BPM detection here is **tempo inference from sample length**, not onset-based analysis.
- Advanced transient-based BPM detection is post-v2.

---

## 6. Slicing System

### 6.1 Slice Marker Data

- Up to 128 markers per instrument, stored as `Long` frame positions.
- Markers are sorted ascending. Implicit slice regions: marker[n] → marker[n+1]. Last marker → totalFrames.

### 6.2 Slice Method Selector (Row 10)

Three modes, cycled via A+DPAD on the SLICE column:

**TRANSIENT** — auto-detect slice points by onset detection.
- Parameter: SENS (00-FF). Higher = more markers; lower = fewer.
- Algorithm: KissFFT spectral flux onset detection.

**DIVIDE** — auto-slice into equal-length divisions.
- Parameter (BY): number of divisions (00-FF). `marker[i] = i * totalFrames / N`.

**OFF** — no auto-slicing. Row 11 (slice detail) is blank.

### 6.3 Manual Slice Edit (Row 11)

- Active when SLICE mode is TRANSIENT or DIVIDE.
- Shows: slice index (00) + slice start position (0000A000).
- A+DPAD on the index field cycles through slices. A+DPAD on the position field adjusts the selected marker.
- Blank when SLICE = OFF.

### 6.4 Zero-Crossing Snap for Slices

- When SNAP = ON, auto-generated and manually placed slice markers snap to the nearest zero crossing.
- Prevents click at the start of each slice on playback.

### 6.5 Waveform Overlay

- When SLICE mode is TRANSIENT or DIVIDE, slice marker positions are overlaid on the waveform as vertical tick marks.
- The active marker (selected in row 11) is highlighted.

### 6.6 Slice Storage — WAV `cue ` Chunk

Store markers in `.ptp` per instrument (authoritative for playback) AND in the WAV file's `cue ` chunk so they travel with the file.

The `cue ` chunk is the de facto standard for arbitrary marker positions in WAV files. Used by M8, Blackbox, Reaper, Logic, SoundForge, Adobe Audition, Wavelab, Ocenaudio.

#### `cue ` chunk binary layout (little-endian)

```
Chunk header:
  4 bytes  "cue " (chunk ID, trailing space)
  4 bytes  chunk data size (uint32)
  4 bytes  number of cue points (uint32)

Per cue point (24 bytes each):
  4 bytes  ID             (uint32, unique per cue point)
  4 bytes  samplePosition  (uint32, frame offset from start of audio)
  4 bytes  dataChunkID    (ASCII, always "data")
  4 bytes  chunkStart     (uint32, 0 for simple files)
  4 bytes  blockStart     (uint32, 0 for simple files)
  4 bytes  sampleOffset   (uint32, same as samplePosition for simple files)
```

#### Implementation plan

- Store markers in `.ptp` per instrument as before (authoritative for playback).
- On WAV save: write a `cue ` chunk with the current slice markers.
- On WAV load in file browser: read `cue ` chunk if present and populate instrument's slice markers.

---

## 7. Save / Export

### 7.1 LOAD (Row 19)

- Opens the file browser to select a different sample to load into the editor.
- Replaces the working buffer and side buffers. Clears undo snapshot and selection.

### 7.2 SAVE (Row 19)

- Saves the working buffer as a new file using the current NAME (row 18).
- If the filename already exists in the directory, opens the QWERTY overlay to enter a new name.
- Instrument's `sampleFilePath` updates to the new file on success.
- Includes WAV `cue ` chunk if slice markers are present.
- **Closes the editor on success.**

### 7.3 OVERWRITE (Row 19)

- Saves directly back to the existing `sampleFilePath`, no name prompt.
- **Closes the editor on success.**

### 7.4 RATE (Save Quality, Row 1)

Sets the output sample rate when saving:

| Value | Sample Rate | Description |
|-------|-------------|-------------|
| HIGH  | 44100 Hz    | No change |
| NORM  | 22050 Hz    | Half rate |
| LOFI  | 11025 Hz    | Quarter rate |

When NORM or LOFI is selected, the WAV is downsampled on save. The in-memory working buffer is not changed until the save operation writes the file.

### 7.5 Save Slices as Separate Files

- Exports each slice region as its own WAV file.
- Naming: `originalname_00.wav`, `originalname_01.wav`, etc.
- Destination: same directory as source sample.
- Optionally auto-loads each slice into the next available empty instrument slots.

---

## 8. Architecture Notes

### 8.1 In-Memory Editing Buffer

- The audio engine's working sample buffer (`samples[sampleId]`) is the live edit target.
- All destructive operations modify it directly. The file on disk is unchanged until save.

### 8.2 Single-Level Undo

- Before any destructive operation, a snapshot of the current buffer is saved (`undoBuffer[sampleId]`).
- UNDO restores the buffer from the snapshot and swaps it back (undo/redo toggle).
- Only one level deep. A second destructive operation overwrites the previous snapshot.
- Memory cost: ~2× current sample size during editing. Acceptable for a single sample at a time.
- Snapshot freed when the editor is closed.

### 8.3 Stereo Side Buffer

- When a stereo WAV is opened in the sample editor, both L and R channels are loaded into a temporary side buffer (`stereoLeft[id]`, `stereoRight[id]`).
- SOURCE switching (row 1) copies the selected side into `samples[id]`.
- Side buffer freed when the editor is closed.

### 8.4 32-Bit Frame Positions in C++

- All JNI functions that accept frame positions take `jlong`.
- `sampleLengths[id]` stays as `int` for now; upgrade to `long` only if needed.

### 8.5 DSP Operations in sample-editor.cpp

- All DSP (delete, crop, cut, duplicate, fade, normalize, reverse, silence, OTT, DUST, drive, EQ, repitch, transient detection) lives in `sample-editor.cpp`.
- Accesses `samples[]` and `sampleLengths[]` from `audio-engine.cpp` via `extern` declarations.
- No Android imports; C++ only. KissFFT used for transient detection (spectral flux).

### 8.6 BPM Detection in Kotlin

- BPM calculation is pure arithmetic, lives in `SampleEditorController.kt`.
- Repitch: Kotlin calculates the factor from PITCH semitones, passes to C++ `repitchSample(sampleId, factor)`.

### 8.7 Slice Marker Type

- `sliceMarkers: MutableList<Long>` (was `MutableList<Int>` in v1).
- Project migration: existing `Int` markers load as `Long` automatically (widening cast).

---

## 9. Features Deferred to Post-v2

- **Spectrum view** (KissFFT bar) — useful, not in scope.
- **Loop marker visualization** — display instrument loop points on waveform; deferred.
- **Time-stretch** — phase vocoder; post-MVP.
- **Crossfade loop** — smooth loop point editing; post-MVP.
- **Bitcrush, Downsample (lo-fi hold), Filter** — not in FX row for v2.
- **Advanced BPM detection** — transient-based onset BPM; post-v2.
- **Note-to-slice playback** — architecture unchanged, implementation deferred.

---

## 10. UI Layout

### 10.1 Screen Overview

The sample editor occupies the full 640×480 canvas (no oscilloscope, no navigation map). 20 logical rows; the waveform occupies rows 3–7 (~165px to fill the screen). Exact row heights finalized during implementation.

```
┌──────────────────────────────────────────────────┐
│ SAMPLE EDITOR                 44100Hz   00:02.34 │  Row 0  — Header
│ ZOOM   1X    SOURCE     LEFT    RATE  HIGH       │  Row 1  — View controls
│ PITCH  00    DURATION   1 BAR   SNAP  ON         │  Row 2  — Edit controls
├──────────────────────────────────────────────────┤
│                                                  │  Row 3  ─┐
│                                                  │  Row 4   │
│               WAVEFORM DISPLAY                   │  Row 5   │ Waveform
│                                                  │  Row 6   │ (~165px)
│                                                  │  Row 7  ─┘
├──────────────────────────────────────────────────┤
│ SELECTION   0000A000   0000F000  LENGTH 00005000 │  Row 8  — Selection info
│                                                  │  Row 9  — Spacer
│ SLICE       TRANSIENT  SENS  40                  │  Row 10 — Slice method
│             00         0000A000                  │  Row 11 — Slice detail
│                                                  │  Row 12 — Spacer
│ CROP  COPY   CUT    DUPLICATE  PASTE    DELETE   │  Row 13 — Operations 1
│ NORM  FADE+  FADE-  SILENCE    REVERSE  UNDO     │  Row 14 — Operations 2
│                                                  │  Row 15 — Spacer
│ EFFECT       OTT    00         APPLY             │  Row 16 — FX row
│                                                  │  Row 17 — Spacer
│ NAME         KICK_808                            │  Row 18 — Name row
│              LOAD   SAVE  OVERWRITE              │  Row 19 — Save options
└──────────────────────────────────────────────────┘
```

### 10.2 Row Descriptions

| Row | Content | Notes |
|-----|---------|-------|
| 0 | Screen title, sample rate (Hz), duration (MM:SS.hh) | Read-only |
| 1 | ZOOM value · SOURCE value · RATE value | A+DPAD to edit each value |
| 2 | PITCH semitones · DURATION bar value · SNAP on/off | A+DPAD to edit |
| 3–7 | Full-width waveform display | Selected region = white, rest = gray |
| 8 | SELECTION start (hex) · end (hex) · LENGTH (read-only) | A+DPAD on start/end columns |
| 9 | (spacer) | — |
| 10 | SLICE method · method-specific parameter | TRANSIENT→SENS 00-FF; DIVIDE→BY 00-FF; OFF→nothing |
| 11 | Slice index · slice start position | A+DPAD to edit; blank if SLICE=OFF |
| 12 | (spacer) | — |
| 13 | CROP · COPY · CUT · DUPLICATE · PASTE · DELETE | A to execute focused action |
| 14 | NORM · FADE+ · FADE- · SILENCE · REVERSE · UNDO | A to execute focused action |
| 15 | (spacer) | — |
| 16 | EFFECT type · value (00-FF) · APPLY | A+DPAD on type/value; A on APPLY executes |
| 17 | (spacer) | — |
| 18 | NAME · sample name | SELECT opens QWERTY overlay |
| 19 | LOAD · SAVE · OVERWRITE | A to execute focused action |

### 10.3 Column Layout Details

**Row 1 — View controls:**
```
ZOOM  [1X]    SOURCE  [LEFT]    RATE  [HIGH]
```
Three value fields. LEFT/RIGHT: header labels, not editable. Cursor lands on value fields only.

**Row 2 — Edit controls:**
```
PITCH  [00]    DURATION  [1 BAR]    SNAP  [ON]
```
PITCH = semitones (-24 to +24). DURATION cycles: 4 BAR → 2 BAR → 1 BAR → 1/2 → 1/4 → 1/8 → 1/16 → 1/32. SNAP = ON/OFF.

**Row 8 — Selection info:**
```
SELECTION   [0000A000]   [0000F000]   LENGTH  [00005000]
```
Cursor on start and end hex fields. LENGTH is read-only (always recalculated).

**Row 10 — Slice method:**
```
SLICE  [TRANSIENT]  SENS  [40]      ← when TRANSIENT
SLICE  [DIVIDE]     BY    [08]      ← when DIVIDE
SLICE  [OFF]                        ← when OFF
```
Method field and parameter field. Parameter label changes to match the selected method.

**Row 11 — Slice detail:**
```
       [00]  [0000A000]    ← slice index · start position (when SLICE ≠ OFF)
       (blank)             ← when SLICE = OFF
```

**Rows 13–14 — Operations:**
```
Row 13:  CROP   COPY   CUT   DUPLICATE   PASTE   DELETE
Row 14:  NORM   FADE+  FADE-  SILENCE   REVERSE  UNDO
```
Six action columns per row. Cursor wraps left/right within the row.

**Row 16 — FX row:**
```
EFFECT  [OTT]  [00]  APPLY
```
Three editable fields: type (OTT/DUST/DRIVE/EQ), value (00-FF), and APPLY trigger.

**Row 19 — Save options:**
```
LOAD   SAVE   OVERWRITE
```
Three action columns.

---

## 11. Controls

### 11.1 Opening and Closing

- **Open:** Press A on the "EDIT" label in the instrument screen. Opens full-screen (no oscilloscope, no navigation map).
- **Close via B (unsaved changes):** If the working buffer has been modified since the last save, shows `ARE YOU SURE?` confirmation dialog. A to confirm close (discards unsaved changes). B or DPAD to cancel and return to the editor.
- **Close via B (no unsaved changes):** Closes immediately with no dialog.
- **Close via SAVE:** Editor closes automatically after a successful save.
- **Close via OVERWRITE:** Editor closes automatically after a successful overwrite.

### 11.2 Cursor Navigation

- **DPAD UP / DOWN:** Move between rows (0–19). Stops at row 0 and row 19 (no wrap).
- **DPAD LEFT / RIGHT:** Move between columns within the current row. Wraps within the row.
- Spacer rows (9, 12, 15, 17) are skipped automatically — cursor jumps to the next content row.

### 11.3 Value Editing

- **A + DPAD UP / DOWN:** Increment/decrement the focused value field. Applies to: ZOOM, SOURCE, RATE, PITCH, DURATION, SNAP, selection start, selection end, slice method, slice parameter, slice index, slice position, FX type, FX value.
- Values wrap at min/max where defined (e.g. SOURCE: LEFT → RIGHT → STEREO → LEFT).
- Key repeat applies (same timing as other screens: 400 ms delay, 100 ms interval).

### 11.4 Actions

- **A** (on an operation column in rows 13–14): Execute immediately (CROP, COPY, CUT, DUPLICATE, PASTE, DELETE, NORM, FADE+, FADE-, SILENCE, REVERSE, UNDO).
- **A** (on APPLY in row 16): Apply the selected FX effect destructively to the sample buffer.
- **A** (on LOAD, SAVE, or OVERWRITE in row 19): Execute the save/load operation.
- **SELECT** (on NAME row 18): Opens QWERTY overlay for renaming the sample.
- **SELECT** (on row 16 when EQ is the selected effect type): Opens the EQ editor screen.

### 11.5 Waveform Interaction (Rows 3–7)

- When cursor is on the waveform rows, A+DPAD LEFT/RIGHT moves the selection start or end marker.
- Cursor column within the waveform area determines which marker is active (left side = start, right side = end). Active marker is highlighted.
- SNAP applies to marker placement if SNAP = ON (row 2).

### 11.6 Playback

- **START:** Plays a preview of the current sample (or the selection, if a selection is active). Second START stops playback.
- Playback uses the in-memory working buffer (reflects any unsaved edits).

---

## 12. Open Questions

No open questions remaining for current scope. Implementation decisions (exact pixel row heights, waveform marker column split threshold, repitch apply trigger) will be resolved during development.

---

**Document Version:** 2.2
**Created:** 2026-05-06
**Updated:** 2026-05-10
**References:**
- [M8 Operation Manual (Sample Editor)](https://www.manualslib.com/manual/2290745/Dirtywave-M8.html?page=47)
- [WAV cue chunk spec](http://www.piclist.com/techref/io/serial/midi/wave.html)
- [KissFFT Library](https://github.com/mborgerding/kissfft)
