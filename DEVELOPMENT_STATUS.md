# PocketTracker Development Status

## Last Updated
2025-12-23

## What's Working ✅

### Core Systems
- ✅ Pixel-perfect rendering at 640×480 with letterboxing
- ✅ Virtual controls with device detection (gaming handheld vs touchscreen)
- ✅ Generic input handler (A+direction for value editing)
- ✅ File management (save/load .ptp projects)
- ✅ Navigation system (5×5 screen grid with SHIFT+DPAD)

### Audio Engine
- ✅ Native C++ audio engine with Oboe
- ✅ 8-voice polyphony with per-track voice stealing
- ✅ 256 sample slots (00-FF)
- ✅ Stereo/mono WAV loading (auto-converts stereo to mono)
- ✅ Real-time playback with pitch shifting
- ✅ Sample preview functionality

### Screens & Modules
- ✅ **Phrase Editor** - 16-step note editing with N/V/I/FX columns
  - Cursor navigation (up/down/left/right)
  - Volume, instrument, FX value editing
  - Playback row highlighting

- ✅ **Chain Editor** - 16 phrase references with transpose
  - Phrase reference cycling (A+left/right)
  - Transpose values
  - Playback with transposition

- ✅ **Song Editor** - 8-track arrangement
  - Chain reference editing
  - Multi-track playback

- ✅ **Project Screen** - Project settings
  - Name editing (12 characters)
  - Tempo adjustment
  - Save/load functionality
  - Default directory: `/Documents/PocketTracker/Projects/`

- ✅ **Instrument Screen** - Sample/instrument editing
  - Sample loading from file browser
  - ROOT note configuration (affects pitch)
  - DETUNE parameter (±8 semitones with 1/16 precision)
  - Sample preview (START button)
  - Sample start/end points (UI only, audio engine TODO)
  - Reverse playback toggle (UI only, audio engine TODO)
  - Loop modes: off/fwd/png (UI only, audio engine TODO)
  - Instrument navigation with L+LEFT/RIGHT
  - Status messages with 5-second auto-dismiss

- ✅ **File Browser** - File/folder navigation
  - WAV sample preview (START button)
  - Directory traversal
  - File sorting (date/name/size)
  - Extension filtering (.ptp, .wav)

### Data Model
- ✅ Hierarchical structure: Project → Song → Chain → Phrase → Step
- ✅ 256 phrases, 256 chains, 8 tracks
- ✅ 256 instruments with full parameter set
- ✅ Serialization/deserialization to JSON

## Recent Fixes (2025-12-23)

### Critical Bug Fixes
1. **Stereo/Mono WAV Loading** - Samples were playing 1 octave too low
   - Root cause: Stereo files loaded as sequential mono (doubled sample count)
   - Fix: Parse WAV header for channel count, mix stereo → mono

2. **MIDI Note Conversion** - All notes off by 1 octave
   - Root cause: `toMidi()` and `fromMidi()` missing octave offset
   - Fix: C-4 now correctly = MIDI 60 (was 48)

3. **File Browser Crash** - SIGSEGV when previewing samples
   - Root cause: Race condition (audio thread playing deleted sample)
   - Fix: Call `native_stopAll()` before loading preview sample

### New Features
- Sample preview in file browser (C-4 reference pitch)
- Instrument preview with ROOT+DETUNE applied
- Proper detune calculation (high nibble = semitones, low nibble = 1/16)

## Known Issues
- ⚠️ Sample start/end/loop/reverse parameters UI-only (audio engine TODO)
- ⚠️ Generic input warning spam after device restart (harmless, goes away after reboot)

## What's NOT Implemented Yet

### Audio Features
- [ ] Sample start/end point playback
- [ ] Loop modes (forward, ping-pong)
- [ ] Reverse playback
- [ ] Effect commands (FX1, FX2, FX3)
- [ ] Real-time effect processing

### UI Screens
- [ ] Mixer screen
- [ ] Effects screen
- [ ] Table screen (arpeggio/pitch/volume tables)
- [ ] Groove screen
- [ ] Scale screen
- [ ] Mods screen

### Workflow Features
- [ ] Copy/paste for phrases/chains
- [ ] Clone instrument
- [ ] Undo/redo
- [ ] Pattern selection/editing

## Next Steps (Priority Order)

1. **Effect System Implementation**
   - Define effect types (pitch, volume, filter, etc.)
   - Implement in audio engine
   - Add effect editing UI

2. **Sample Playback Parameters**
   - Implement start/end/loop in C++ audio engine
   - Add reverse playback support

3. **Table Screen**
   - Arpeggio tables for melodic sequences
   - Volume envelopes
   - Pitch modulation

4. **Mixer Screen**
   - Per-track volume/pan
   - Master volume
   - Visualization

## Technical Notes

### Audio Engine Details
- Sample rate: Device native (typically 48000 Hz)
- Format: Float32, stereo output
- Latency: Oboe auto-selects best mode
- All samples stored as mono (stereo mixed during load)
- Base frequency stored per instrument for pitch calculation
- Playback rate = target_frequency / base_frequency

### MIDI Note Convention
- C-4 = MIDI 60 (middle C)
- Formula: `(octave + 1) * 12 + pitch`
- Frequency: `440 * 2^((midi - 69) / 12)`

### Detune Parameter
- Format: 0x00-0xFF hex byte
- High nibble (0-F): Whole semitones
- Low nibble (0-F): 1/16 semitone increments
- Center: 0x80 = no detune
- Range: ±8 semitones with fine control
- Example: 0x93 = +3 semitones + 3/16 = +3.1875 semitones

### Instrument Slots
- 00-0B: Hardcoded resource samples (kick, snare, hihat, bass, etc.)
- 0C-FF: User-loadable samples
- Each instrument has independent ROOT+DETUNE tuning

## Questions for Claude
- None at this time - all major systems working!
