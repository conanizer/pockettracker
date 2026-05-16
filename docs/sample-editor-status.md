Sample Editor: What's Done vs What's Missing

✅ FULLY IMPLEMENTED

UI & Rendering (SampleEditorModule.kt):
- All 20-row layout rendered correctly (rows 0-2 header, 3-7 waveform, 8-19 content)
- Selection S/E markers and real-time playback position marker on waveform
- Zoomed waveform via getSampleWaveformRange() JNI
- Confirm-close dialog (unsaved changes guard)
- Slice markers drawn on waveform in TRANSIENT mode (active marker highlighted) and in OFF mode
  (read-only display of WAV cue markers, no detection)

Cursor & Navigation:
- Up/down row skip (spacers 9, 12, 15, 17 and waveform rows 3-7)
- Left/right column clamp per row (maxColForRow)
- rowAbove/rowBelow helpers all correct

All Destructive Sample Ops (wired to C++ JNI):
- CROP, COPY, CUT, DUPLICATE, PASTE, DELETE (row 13)
- NORM, FADE+, FADE-, SILENCE, REVERSE, UNDO (row 14)
- Backup/undo snapshot system (backupSample / undoSample)

Fields (display + editing):
- ZOOM (1×–16×), SOURCE, RATE, PITCH, DURATION, SNAP
- SLICE method (TRANSIENT/DIVIDE/OFF), SENS/BY, slice index/position
- EFFECT type + value (OTT/DUST/DRIVE/EQ)

Save/Load:
- LOAD → opens file browser
- SAVE → writes new WAV (auto-increments filename)
- OVERWRITE → writes to existing path
- NAME rename via QWERTY overlay
- CHOP → exports each slice as a separate WAV into Samples/Chops/{name}/

Opening:
- EDIT button on Instrument screen row 3 col 3 → opens full-screen editor (MainActivity.kt:2008)
- Full-screen rendering (no oscilloscope, no nav map)

START preview playback — plays selection range with pitch offset applied

Transient detection (SLICE=TRANSIENT):
- KissFFT-based spectral flux onset detection via detectTransients() JNI
- SENS field (00-FF) controls threshold; A+DPAD to edit
- Auto-detects when switching to TRANSIENT mode with empty markers
- Sensitivity changes clear markers first so detection re-runs
- Detected markers shown as tick marks on waveform; active marker highlighted

Slice marker persistence:
- sliceMarkers: List<Long> field on Instrument (absolute frame positions)
- Read from WAV cue chunk by loadSampleFromFile() and reloadProjectSamples()
- Written to WAV cue chunk on SAVE/OVERWRITE from sample editor
- Not overwritten when opening editor (read-only display in OFF mode)
- Not overwritten on editor exit without saving (B = discard)
- Synced to instrument only on successful WAV write

WAV cue chunk:
- WavWriter.writeWav() embeds a cue  chunk with all slice marker positions
- WavWriter.readCuePoints() reads cue  chunk; frame 0 excluded (implicit sample start)
- Standard format compatible with M8, Blackbox, Reaper, Logic, Adobe Audition

Note-to-slice playback (Instrument screen: SLICE row):
- SLICE field on instrument screen row 14 (alongside LOOP): OFF / CUT / TRU
- OFF: normal pitch playback (unchanged behavior)
- CUT: phrase note selects slice (C-4 = slice 0 relative to ROOT); plays from slice start
  to next marker then stops. Loop disabled for CUT slices.
- TRU: same slice selection as CUT; plays from slice start to sample end.
- Slice index = (note MIDI − root MIDI).coerceIn(0, markers.size); N markers → N+1 slices.
  Slice 0 always starts at frame 0.
- Pitch locked to root (rate = 1.0×) when slicing is active.
- C++ endPointOverride enforces CUT boundary; params.base[PARAM_SAMPLE_END] updated so
  the per-block modulation recalculation preserves the boundary.
- sliceMarkers populated at sample-load time (loadSampleFromFile / reloadProjectSamples),
  so slicing works without opening the sample editor.

  ---
❌ NOT IMPLEMENTED

1. FX APPLY (Row 16, col 2) ✅ ALL DONE
   C++ applySampleFx(id, fxType, fxValue, sampleRate): OTT via OttModule.resetForRender+process,
   DUST via DustChain, DRIVE via DriveModule.processMono, EQ via EqModule from eqPresets[slot].
   EQ: fxValue = slot 0-127; cursor context limits to 0-127. No-op if slot has no active bands.
   EQ editor: SELECT on FX row (fxType==3) opens EQ editor overlay (EqCallerContext.SampleEditorFx).
   SELECT closes EQ editor. B+LEFT/RIGHT scrolls presets. fxValue stays in sync via applyCallerEqSlotChange.

   Non-destructive FX preview: START button applies FX in-place to a temp buffer (saveFxPreviewBackup saves
   clean copy; applySampleFx processes it). Each START press restores previous preview first so value changes
   are heard immediately. restoreFxPreviewBackup() called before APPLY, UNDO, all destructive ops, and exit
   so the clean sample is always restored before anything permanent happens.

2. REPITCH destructive ✅ DONE
   SYNC/RPITCH on FX row (row 16, col APPLY). Calculates semitones from DURATION+BPM and calls pitchShiftSample() immediately.
   Root note not modified. Backup taken before apply so UNDO works.

3. SNAP / Zero-Crossing
   Toggle exists in state and UI but is never applied. Selection marker editing (A+UP/DOWN in waveform rows) doesn't invoke any zero-crossing
   scan. No C++ findZeroCrossing().

4. BPM display
   DURATION field cycles bar values, but BPM is never calculated from (numBars * 4 * 60 * sampleRate) / totalFrames. Header row just shows
   sampleRate and duration in MM:SS format, not BPM.

5. RATE downsampling on save
   rateMode (HIGH/NORM/LOFI) is stored in state but SAVE/OVERWRITE always write at the original sample rate regardless.

  ---
Summary by priority

┌────────────────────────────────────────────────────────────────┬────────────┐
│                            Feature                             │   Status   │
├────────────────────────────────────────────────────────────────┼────────────┤
│ Waveform display + zoom                                        │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ All selection ops (crop/copy/cut/paste/del/norm/fade/rev/undo) │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ LOAD / SAVE / OVERWRITE / NAME                                 │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ SLICE UI (rows 10–11)                                          │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ Transient auto-slice (KissFFT onset detection)                 │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ Slice markers on waveform                                      │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ Slice marker persistence (WAV cue chunk + Instrument field)    │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ WAV cue  chunk read/write                                      │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ Save slices as separate files (CHOP)                           │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ Note-to-slice playback (SLICE OFF/CUT/TRU on instrument)       │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ FX APPLY OTT/DUST/DRIVE offline                                │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ FX APPLY EQ offline                                            │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ Destructive REPITCH (SYNC/RPITCH on FX row via APPLY)          │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ EQ editor open from FX row (SELECT → EQ overlay)              │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ Non-destructive FX preview (START plays with FX, not baked)   │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ SNAP zero-crossing                                             │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ BPM detection/display                                          │ ❌ Missing │
├────────────────────────────────────────────────────────────────┼────────────┤
│ RATE downsampling on save                                      │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ SOURCE stereo playback + channel selection                     │ ✅ Done    │
└────────────────────────────────────────────────────────────────┴────────────┘

The core editing workflow is fully working. The sample editor is feature-complete for MVP
except for BPM display.
