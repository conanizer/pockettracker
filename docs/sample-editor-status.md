Sample Editor: What's Done vs What's Missing

✅ FULLY IMPLEMENTED

UI & Rendering (SampleEditorModule.kt):
- All 20-row layout rendered correctly (rows 0-2 header, 3-7 waveform, 8-19 content)
- Selection S/E markers and real-time playback position marker on waveform
- Zoomed waveform via getSampleWaveformRange() JNI
- Confirm-close dialog (unsaved changes guard)

Cursor & Navigation:
- Up/down row skip (spacers 9, 12, 15, 17 and waveform rows 3-7)
- Left/right column clamp per row (maxColForRow)
- rowAbove/rowBelow helpers all correct

All Destructive Sample Ops (wired to C++ JNI):
- CROP, COPY, CUT, DUPLICATE, PASTE, DELETE (row 13)
- NORM, FADE+, FADE-, SILENCE, REVERSE, UNDO (row 14)
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

Opening:
- EDIT button on Instrument screen row 3 col 3 → opens full-screen editor (MainActivity.kt:2008)
- Full-screen rendering (no oscilloscope, no nav map)

START preview playback — plays selection range with pitch offset applied

  ---
❌ NOT IMPLEMENTED

1. FX APPLY (Row 16, col 2) — completely stubbed
   16 -> if (s.cursorCol == 2) { // APPLY FX — not yet implemented }
   OTT, DUST, DRIVE offline whole-sample DSP are absent. No C++ applySampleFx() JNI function exists.

2. REPITCH destructive ✅ DONE
   SYNC/RPITCH on FX row (row 16, col APPLY). Calculates semitones from DURATION+BPM and calls pitchShiftSample() immediately.
   Root note not modified. Backup taken before apply so UNDO works.

3. SNAP / Zero-Crossing
   Toggle exists in state and UI but is never applied. Selection marker editing (A+UP/DOWN in waveform rows) doesn't invoke any zero-crossing
   scan. No C++ findZeroCrossing().

4. Transient detection (SLICE=TRANSIENT)
   SENS field is rendered and editable, but auto-slicing never runs. No KissFFT in the project, no spectral flux algorithm anywhere in C++.

5. BPM display
   DURATION field cycles bar values, but BPM is never calculated from (numBars * 4 * 60 * sampleRate) / totalFrames. Header row just shows
   sampleRate and duration in MM:SS format, not BPM.

6. Slice markers on waveform
   drawWaveform() draws only the S/E selection markers and the playback cursor. When SLICE=TRANSIENT or DIVIDE, vertical tick marks for all
   slice positions are not drawn.

7. WAV cue  chunk on save
   WavWriter.writeWav() writes a standard PCM WAV with no cue  chunk. Plan §6.6 requires embedding slice markers when saving.

8. RATE downsampling on save
   rateMode (HIGH/NORM/LOFI) is stored in state but SAVE/OVERWRITE always write at the original sample rate regardless.

9. SOURCE stereo channel switching
   Changing sourceMode (LEFT/RIGHT/STEREO) updates state only — nothing reloads the working buffer. The WAV loader already converts
   stereo→mono on load, so there's no stereo side buffer to switch from.

10. Slice marker persistence
    sliceMarkers field doesn't exist on Instrument in TrackerData.kt. Slice state lives only in the editor session
    (SampleEditorState.sliceIndex/slicePosition), not persisted to project or WAV.

11. Save slices as separate files (Plan §7.5)
    Not implemented at all.

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
│ SLICE UI (rows 10–11)                                          │ ✅ UI only │
├────────────────────────────────────────────────────────────────┼────────────┤
│ FX APPLY (OTT/DUST/DRIVE/EQ offline)                           │ ❌ Stubbed │
├────────────────────────────────────────────────────────────────┼────────────┤
│ Destructive REPITCH (SYNC/RPITCH on FX row via APPLY)          │ ✅ Done    │
├────────────────────────────────────────────────────────────────┼────────────┤
│ SNAP zero-crossing                                             │ ❌ Missing │
├────────────────────────────────────────────────────────────────┼────────────┤
│ Transient auto-slice                                           │ ❌ Missing │
├────────────────────────────────────────────────────────────────┼────────────┤
│ BPM detection/display                                          │ ❌ Missing │
├────────────────────────────────────────────────────────────────┼────────────┤
│ Slice markers on waveform                                      │ ❌ Missing │
├────────────────────────────────────────────────────────────────┼────────────┤
│ Slice marker persistence                                       │ ❌ Missing │
├────────────────────────────────────────────────────────────────┼────────────┤
│ WAV cue  chunk                                                 │ ❌ Missing │
├────────────────────────────────────────────────────────────────┼────────────┤
│ RATE downsampling on save                                      │ ❌ Missing │
├────────────────────────────────────────────────────────────────┼────────────┤
│ SOURCE stereo side buffer                                      │ ❌ Missing │
├────────────────────────────────────────────────────────────────┼────────────┤
│ Save slices as separate files                                  │ ❌ Missing │
└────────────────────────────────────────────────────────────────┴────────────┘

The core editing workflow (open → view waveform → select → crop/normalize/fade/reverse/undo → save) is fully working. What's missing is the
more advanced features: FX apply, repitch, SNAP, transient slicing, BPM, and slice persistence.