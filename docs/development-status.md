# Development Status

**Last Updated:** 2026-03-19

## Current Phase

**All Extension Packs COMPLETE!** -> Testing & Polish -> Documentation -> MVP Release

**Target Release:** April 2026

---

## Timeline

```
Weeks 1-2:   Refactoring                                          COMPLETE
Weeks 3-4:   Effects system                                       COMPLETE
Week 5:      Copy/paste system                                    COMPLETE
Weeks 6-7:   MVP Expansion (Mixer + WAV Export)                   COMPLETE
Weeks 8-10:  Extension Pack 2 (Tables, HOP/TIC, Pitch Effects)   COMPLETE
Weeks 11-13: Extension Pack 3 (Groove, Modulation, Resampling)   COMPLETE
Week 14:     Testing & Polish                                     CURRENT
Week 15:     Documentation & video
Week 16:     MVP Release
```

---

## What's Working

### Core Systems
- Pixel-perfect rendering at 640x480 with letterboxing
- Virtual controls with device detection (gaming handheld vs touchscreen)
- Generic input handler (A+direction for value editing)
- File management (save/load .ptp projects)
- Navigation system (5x5 screen grid with SHIFT+DPAD)
- Key repeat (hold D-PAD / A+DPAD / B+DPAD)

### Audio Engine
- Platform-agnostic architecture (IAudioBackend + IResourceLoader interfaces)
- Sample-accurate note queue system (C++ priority queue, <0.02ms jitter)
- Linear interpolation (eliminates aliasing artifacts during pitch-shifting)
- Native C++ audio engine with Oboe (44.1kHz, LowLatency, Exclusive mode)
- 8-voice polyphony with per-track voice stealing + 3-step allocator
- 256 sample slots with stereo/mono WAV loading (auto-converts)
- Automatic sample rate compensation
- Advanced playback: start/end points, reverse, looping (fwd/ping-pong)
- Queue-based playback: phrase, chain (with transpose), song (8-track)
- Continuous buffering with 2-phrase lookahead
- <50ms startup latency
- Accurate playback cursors (frame-based position tracking)
- Real-time waveform capture for oscilloscope
- Stereo output with constant-power pan law
- Modulation engine (AHD, ADSR, LFO, DRUM, TRIG on all destinations)
- Table tick processing (per-voice, per-note)
- Offline WAV render (processAudioBlock unified DSP)
- Groove quantization (per-track groove assignments)
- Resonant biquad filters (LP/HP/BP)

### Screens & Modules
- **Oscilloscope** - Real-time waveform visualization (60 FPS)
- **Phrase Editor** - 16-step editing with N/V/I/FX columns
- **Chain Editor** - 16 phrase references with transpose
- **Song Editor** - 8-track arrangement, 256 rows, B+UP/DOWN page jump
- **Instrument Screen** - Full parameter set (sample, ROOT, DETUNE, VOL, PAN, filters, loop, start/end)
- **Project Screen** - Name, tempo, save/load, CLEAN SEQ/INST, layout mode switcher
- **File Browser** - Navigation, sorting, preview, WAV/video audio extraction
- **Mixer Screen** - 8 tracks + master with true dBFS meters
- **Table Screen** - 16-row mini-sequencer per instrument
- **Groove Screen** - Step-timing patterns for swing/shuffle (256 grooves)
- **Modulation Screen** - 4-slot envelope/LFO editor per instrument

### Effects (All Complete)
- **ARP/ARC** - Arpeggio with UP/DOWN/PINGPONG/RANDOM modes and speed control
- **OFF** - Sample start point offset
- **VOL** - Volume automation
- **KIL** - Kill voice immediately
- **REP** - Retrigger (single shot or volume-ramp mode)
- **PSL** - Pitch slide / portamento
- **PBN** - Pitch bend (continuous up or down)
- **PVB/PVX** - Vibrato (standard and extreme)
- **DEL** - Delay note by N ticks
- **CHA** - Probability gate
- **RND/RNL** - Randomize FX values
- **TBL** - Override table ID
- **THO** - Table hop to row
- **GRV** - Groove assign per track
- **TIC** - Table tick rate + special modes
- **HOP** - Phrase/table jump (odd time signatures)

### Copy/Paste (M8-Style)
- Selection mode (L+B to enter/cycle: CELL -> ROW -> SCREEN)
- Copy (B in selection), Cut (L+A in selection), Paste (L+A outside)
- Delete selection (A+B in selection)
- Works on PHRASE, CHAIN, SONG screens
- Selection increment (A+DPAD applies to all selected rows)

### Layout System
- 4 layout modes: FULL, TOUCH_PORTRAIT, TOUCH_LANDSCAPE, TOUCH_PORTRAIT2
- Auto-switch on device rotation
- Layout + scaling mode persisted via SharedPreferences
- Pixel-perfect font rendering (anti-alias=false + horizontal run merging)

### Data Model
- Hierarchical: Project -> Song -> Chain -> Phrase -> Step
- 256 phrases, 256 chains, 8 tracks
- 256 instruments, 256 tables, 256 grooves
- ModSlot[4] per instrument
- JSON serialization with forward migration
- Platform-agnostic (no Android dependencies)

---

## Testing Devices

| Device | RAM | OS | Resolution | Status |
|--------|-----|-----|-----------|--------|
| Miyoo Flip (primary) | 1GB | Android 13 (GammaCoreOS) | 640x480 | All features working |
| Ayaneo Pocket Air Mini | 3GB | Android 11 | Scaling adjusted | All features working |

**Minimum Requirements:** Android 8.0+ (API 26), 64-bit, ~512MB RAM, ~50MB storage, 640x480 minimum

---

## Known Issues

- Generic input warning spam after device restart (harmless, goes away after reboot)

---

## Remaining Work

### Testing & Polish (Current)
- [ ] "Hello world" song usability test (<5 min)
- [ ] Bug hunting on both devices
- [ ] Performance verification (stable 30-60fps)
- [ ] Example project creation

### Documentation
- [ ] README finalization
- [ ] Controls guide (full reference)
- [ ] Short demo video
- [ ] Known issues list

---

## Completed Milestones

### Architecture Refactoring (Complete)
- IAudioBackend interface + OboeAudioBackend implementation
- IResourceLoader interface + AndroidResourceLoader implementation
- IFileSystem interface + AndroidFileSystem implementation
- InputController, PlaybackController, InstrumentController, TrackerController, EffectProcessor, FileController, ClipboardManager
- MainActivity reduced from 2668 to 1862 lines

### MVP Expansion #1 (Complete - 2026-01-27)
- Mixer Screen with dBFS meters
- WAV Export (multi-track rendering)
- Instrument VOL/PAN
- Stereo pan in audio engine
- Volume chain: instrument x phrase x track x master

### Extension Pack 2 (Complete - 2026-02-05)
- Table data model, screen UI, and audio processing
- TIC effect (table tick rate + special modes)
- HOP effect (phrase/table jump)
- Pitch effects (PSL, PBN, PVB, PVX)

### Extension Pack 3 (Complete - 2026-03-13)
- Fixes & UX updates (table vol range, FX cycling, key repeat, selection increment)
- New effects (DEL, CHA, RND, RNL, TBL, THO, GRV, REP XY rework)
- Groove screen
- Modulation screen & engine (AHD, ADSR, LFO, DRUM, TRIG)
- Selection resampling
- Audio bug fixes (oscilloscope SIGSEGV, AHD crackling, voice-steal click)
- Layout system (touchscreen modes, orientation auto-switch)
- Polish (CLEAN dialog, pixel-perfect font, SharedPreferences persistence)

---

## Post-MVP Features

### Early Post-MVP (With Mentor)
- Advanced copy/paste (instrument settings)
- Linux port (GTK/Qt UI with same controllers)
- Braids synthesizers (Mutable Instruments integration)
- Filter automation (CUT, RES effects)

### Later Features
- Undo/redo
- Per-track stem export
- Advanced filters (EQ, compressor)
- Stereo meters per track
- Effect sends (reverb, delay, chorus)
- Themes/color schemes
- Alternative visualizers (EQ spectrum, spectrogram, dB meters)

---

## Technical Notes

### Audio Engine Details
- Sample rate: 44100 Hz (forced via Oboe builder)
- Performance mode: LowLatency (MMAP fast audio path)
- Sharing mode: Exclusive
- Format: Float32, stereo output
- Buffer size: Auto-selected (~192-480 frames)
- All samples stored as mono
- Playback rate = target_frequency / base_frequency

### MIDI Note Convention
- C-4 = MIDI 60 (middle C)
- Formula: `(octave + 1) * 12 + pitch`
- Frequency: `440 * 2^((midi - 69) / 12)`

### Instrument Slots
- All 256 slots (00-FF) are identical in structure
- All start empty (`sampleFilePath = null`) — no bundled default samples
- Users load samples into any slot via the file browser
- Each instrument has independent ROOT+DETUNE tuning

---

## MVP Definition of Done

### Core Functionality (All Complete)
- [x] Create phrases with notes and effects
- [x] Copy/paste phrase steps (M8-style selection)
- [x] Chain phrases with transpose
- [x] Arrange songs with 8 tracks
- [x] Save and load projects
- [x] All effects working (17+ effects)
- [x] Modulation system (4 slots per instrument)
- [x] Tables and grooves

### Remaining for Release
- [ ] User can complete "hello world" song in <5 min
- [ ] README explains how to install and use
- [ ] Example project included
- [ ] Demo video recorded
- [ ] No known crash bugs
