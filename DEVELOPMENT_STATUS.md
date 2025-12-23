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
- ✅ Automatic sample rate compensation (44100Hz, 48000Hz, etc.)
- ✅ Real-time playback with pitch shifting
- ✅ Sample preview functionality
- ✅ Advanced playback: start/end points, reverse, looping (fwd/ping-pong)
- ✅ Precise timing system (double precision, no drift)

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
  - Sample start/end points (FULLY WORKING - UI + audio engine)
  - Reverse playback toggle (FULLY WORKING - UI + audio engine)
  - Loop modes: off/fwd/png (FULLY WORKING - UI + audio engine)
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

## Recent Fixes & Features (2025-12-23)

### Advanced Sample Playback Parameters ✅
**FULLY IMPLEMENTED** - Start/end points, reverse, and looping now work in audio engine!
- ✅ **Sample start/end points** - Playback constrained to 0-255 range (UI + audio)
- ✅ **Reverse playback** - Samples play backwards when enabled
- ✅ **Forward loop** - Loops between loopStart and end points
- ✅ **Ping-pong loop** - Bounces back and forth between points
- Implementation: C++ audio callback handles all modes with precise sample-accurate timing

### Sample Rate Compensation ✅
**AUTOMATIC PITCH CORRECTION** - All samples play at correct pitch regardless of sample rate!
- ✅ Reads sample rate from WAV header (44100Hz, 48000Hz, etc.)
- ✅ Queries actual device sample rate from Oboe (not hardcoded!)
- ✅ Calculates compensation ratio: `baseFreq = 261.63 × (deviceRate / sampleRate)`
- ✅ Ratio persists when ROOT/DETUNE parameters change
- Result: 44100Hz samples on 48000Hz device now play at correct pitch

### Playback Timing Precision Fixes ✅
**ROCK-SOLID TEMPO** - Fixed timing jitter and uneven note spacing!

1. **Phrase Note Increment Bug** - A+UP/DOWN was offsetting by -1 octave
   - Root cause: `PhraseEditorModule.kt` used old MIDI formula `octave*12+pitch`
   - Fix: Changed to use `note.toMidi()` with correct formula `(octave+1)*12+pitch`

2. **Precision Loss in Timing** - Phrase loops had inconsistent retrigger timing
   - Root cause: `(stepCounter * stepDurationMs).toLong()` truncated fractional ms
   - At 162 BPM: Lost 0.59ms per step = 9.48ms error per 16-step phrase!
   - Fix: Keep calculations in Double precision until final delay

3. **Uneven Note Timing Within Phrase** - "Floating tempo" inside bars
   - Root cause: Notes played immediately, then delay compensated for drift
   - This made some notes play too soon, others too late
   - Fix: Calculate target time FIRST, wait until precise moment, THEN play note
   - Result: Every note triggers at exact calculated time

### Critical Bug Fixes (Earlier Session)
1. **Stereo/Mono WAV Loading** - Samples were playing 1 octave too low
   - Root cause: Stereo files loaded as sequential mono (doubled sample count)
   - Fix: Parse WAV header for channel count, mix stereo → mono

2. **MIDI Note Conversion** - All notes off by 1 octave
   - Root cause: `toMidi()` and `fromMidi()` missing octave offset
   - Fix: C-4 now correctly = MIDI 60 (was 48)

3. **File Browser Crash** - SIGSEGV when previewing samples
   - Root cause: Race condition (audio thread playing deleted sample)
   - Fix: Call `native_stopAll()` before loading preview sample

### New Features (Earlier Session)
- Sample preview in file browser (C-4 reference pitch)
- Instrument preview with ROOT+DETUNE applied
- Proper detune calculation (high nibble = semitones, low nibble = 1/16)

## Known Issues
- ⚠️ Generic input warning spam after device restart (harmless, goes away after reboot)

## What's NOT Implemented Yet

### Audio Features
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
