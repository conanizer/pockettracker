# PocketTracker Development Status

## Last Updated
2026-01-22

## Current Phase
**Phase A Complete** → **Architecture Refactoring (Phases 1-4 Complete ✅)** → Effects System (TOP-5 In Progress) → Copy/Paste → MVP Release

## What's Working ✅

### Core Systems
- ✅ Pixel-perfect rendering at 640×480 with letterboxing
- ✅ Virtual controls with device detection (gaming handheld vs touchscreen)
- ✅ Generic input handler (A+direction for value editing)
- ✅ File management (save/load .ptp projects)
- ✅ Navigation system (5×5 screen grid with SHIFT+DPAD)

### Audio Engine
- ✅ **Platform-agnostic architecture** (IAudioBackend + IResourceLoader interfaces) 🆕
- ✅ **Sample-accurate note queue system** (C++ priority queue, <0.02ms jitter)
- ✅ **Linear interpolation** (eliminates aliasing artifacts during pitch-shifting)
- ✅ **Frame-precise scheduling** (matches M8/LGPT/Picotracker timing precision)
- ✅ Native C++ audio engine with Oboe
- ✅ 8-voice polyphony with per-track voice stealing
- ✅ 256 sample slots (00-FF)
- ✅ Stereo/mono WAV loading (auto-converts stereo to mono)
- ✅ Automatic sample rate compensation (44100Hz, 48000Hz, etc.)
- ✅ Real-time playback with pitch shifting
- ✅ Sample preview functionality
- ✅ Advanced playback: start/end points, reverse, looping (fwd/ping-pong)
- ✅ **Queue-based playback**: Phrase, Chain (with transpose), Song (8-track polyphonic)
- ✅ **Continuous buffering**: 2-phrase lookahead for smooth playback
- ✅ **50ms startup latency**: Instant playback feel
- ✅ **Accurate playback cursors**: Frame-based position tracking
- ✅ **Real-time waveform capture**: Oscilloscope displays actual mixed audio output

### Screens & Modules
- ✅ **Oscilloscope** - Real-time audio waveform visualization
  - Captures actual mixed audio output from native engine
  - Scrolling waveform display (right-to-left)
  - Adjustable downsampling rate (time window: 14ms-700ms)
  - Adjustable gain (amplitude scaling)
  - 60 FPS refresh rate on all screens

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
- ✅ **Platform-agnostic** - No Android dependencies in data classes!

## Professional Audio Engine Overhaul (2025-12-28) ✅

### Sample-Accurate Timing System
**COMPLETE REWRITE** - Moved from Kotlin millisecond timing to C++ frame-precise scheduling!

**Phase 1: Foundation**
- ✅ **Linear interpolation** - Eliminates aliasing artifacts during pitch-shifting
- ✅ **Note queue infrastructure** - C++ priority queue with frame-precise triggering
- ✅ **Global frame counter** - Tracks total audio frames for accurate scheduling
- ✅ **Thread-safe design** - Mutex-protected queue, audio thread vs UI thread coordination

**Phase 2: Phrase Playback**
- ✅ **Queue-based playback** - Sample-accurate note scheduling (<0.02ms jitter)
- ✅ **Continuous buffering** - 2-phrase lookahead maintains smooth playback
- ✅ **50ms startup latency** - Instant playback feel (was 250ms, reduced for responsiveness)
- ✅ **Clean queue management** - Automatic cleanup on stop/restart prevents note stacking
- ✅ **Frame-based cursor** - Playback position calculated from audio frames, not delays

**Phase 3: Full Sequencer**
- ✅ **Chain playback** - Multi-phrase sequencing with per-row transpose support
- ✅ **Song playback** - 8-track polyphonic with perfect synchronization
- ✅ **Accurate cursors** - Both chain and song show exact playing position (not scheduling position)
- ✅ **Voice assignment** - Per-track voice stealing (trackId 0-7)

**Technical Achievements:**
- Timing precision: 100x improvement (1-2ms → <0.02ms jitter)
- Audio quality: Professional-grade linear interpolation
- Architecture: Portable C++ audio core (ready for Linux port)
- Removed: ~215 lines of obsolete Kotlin timing code

**Result:** Now matches M8/LGPT/Picotracker timing and quality standards!

## Recent Polish & Refinements (2025-12-26)

### Sample Persistence & Project Loading ✅
- ✅ **Custom samples reload automatically** when loading projects
- ✅ **Instrument parameters restored** (ROOT, DETUNE, START, END, REVERSE, LOOP)
- ✅ **Empty instruments** are truly empty (sampleId = -1, not defaulting to kick)
- ✅ **First 12 instruments** (00-0B) still default to resource samples

### Perfect Pitch for Common Sample Rates ✅
- ✅ **Oboe stream set to 44100 Hz** (most common audio sample rate)
- ✅ **44100 Hz samples**: Perfect pitch, zero compensation error!
- ✅ **48000 Hz samples**: Still compensated correctly
- ✅ **Mathematical precision**: No more 0.06 Hz rounding errors

### Low-Latency Audio Performance ✅
- ✅ **LowLatency performance mode** enabled (MMAP fast audio path)
- ✅ **Exclusive sharing mode** for dedicated audio stream
- ✅ **Reduced buffer sizes** for faster response
- ✅ **Tighter timing**: Much less jitter between notes

### Smart Cursor State Memory ✅
**Bidirectional sync** - cursor remembers context when navigating:
- ✅ **Phrase ↔ Instrument**: Jump to instrument shows last used/edited instrument
- ✅ **Chain ↔ Phrase**: Jump to phrase shows last used/edited phrase
- ✅ **Song ↔ Chain**: Jump to chain shows last used/edited chain
- ✅ **Captures on navigation**: Remembers value under cursor when leaving screen
- ✅ **Updates on edit**: Tracks last edited values via A+direction combos

### Quick Insert Feature ✅
**A button on empty rows** inserts last-used values:
- ✅ **Phrase screen**: Insert last note/instrument/volume
- ✅ **Chain screen**: Insert last phrase/transpose
- ✅ **Song screen**: Insert last chain
- ✅ Speeds up composition workflow significantly!

## Testing Devices

### Primary Device ✅
**Miyoo Flip**
- RAM: 1GB
- OS: Android 13 (GammaCoreOS)
- Resolution: 640×480
- Status: All features working smoothly!

### Secondary Device ✅
**Ayaneo Pocket Air Mini**
- RAM: 3GB
- OS: Android 11
- Resolution: Working with scaling adjustments
- Status: All features working! Project file transfer between devices working ✅

### Minimum Requirements (Verified)
- Android 8.0+ (API 26)
- 64-bit processor
- **~512MB total RAM** (works on 1GB Miyoo Flip!)
- ~50MB storage
- 640×480 minimum resolution

## Known Issues
- ⚠️ Generic input warning spam after device restart (harmless, goes away after reboot)

## Recent Fixes

### Cross-Device Project File Transfer (2026-01-18) ✅
**Problem:** Project files copied via USB/file manager from another device were invisible to the app.

**Root Cause:** Android scoped storage (API 29+) restricts apps to only see files they created. Files copied externally aren't registered in the app's file ownership.

**Solution:** Added `MANAGE_EXTERNAL_STORAGE` permission for Android 11+ (API 30+):
- Added permission to `AndroidManifest.xml`
- Added runtime check in `MainActivity.kt` that redirects to Settings if not granted
- Updated `AndroidFileSystem.hasStoragePermission()` to check the new permission
- On first launch, user is prompted to enable "All Files Access" in Settings (one-time setup)

## What's NOT Implemented Yet (MVP Remaining Work)

### 1. Architecture Refactoring (1-2 weeks) ⚠️
**Status:** Starting next!

**Why refactoring before features?**
- Prepares for Linux port (mentor joining post-MVP)
- Separates Android-specific code from business logic
- Makes effects and copy/paste easier to implement
- Clean architecture = easier debugging

**What needs refactoring:**
- ✅ **Phase 1: Audio Backend Abstraction** (COMPLETE - 2026-01-03)
  - ✅ Created `IAudioBackend` interface (core/audio/)
  - ✅ Created `OboeAudioBackend` implementation (platform/android/)
  - ✅ Created `AudioEngine` wrapper (core/audio/, platform-agnostic)
  - ✅ Updated MainActivity + controllers to use new architecture
  - ✅ Removed legacy TrackerAudioEngine.kt
  - ✅ Audio engine now 100% portable (zero Android dependencies)

- ✅ **Phase 2: Resource Loading Abstraction** (COMPLETE - 2026-01-03)
  - ✅ Created `IResourceLoader` interface (core/resources/)
  - ✅ Created `AndroidResourceLoader` implementation (platform/android/)
  - ✅ Integrated with AudioEngine
  - **Note:** Completed as part of Phase 1 (steps 1.7-1.9)

- ✅ **Phase 3: File I/O Abstraction** (COMPLETE - 2026-01-03)
  - ✅ Created `IFileSystem` interface (core/storage/)
  - ✅ Created `FileInfo` data class (platform-agnostic file metadata)
  - ✅ Created `AndroidFileSystem` implementation (platform/android/)
  - ✅ Updated FileManager to use IFileSystem (zero Context dependencies)
  - ✅ FileManager now 100% portable
  - **Result:** Ready for Linux port! 🎉

- ✅ **Phase 4: Business Logic Extraction** (COMPLETE - 2026-01-03)
  - ✅ Created `InputController` (core/logic/ - button handling, hex input)
  - ✅ Created `PlaybackController` (core/logic/ - phrase/chain/song playback)
  - ✅ Created `InstrumentController` (core/logic/ - sample management)
  - ✅ Created `TrackerController` (core/logic/ - navigation, coordination)
  - ✅ Created `EditorHelpers.kt` (UI helper functions - 217 lines)
  - ✅ Added `handleInput()` to all 5 screen modules:
    - PhraseEditorModule.handleInput() - note/instrument/volume editing
    - ChainEditorModule.handleInput() - phrase refs/transpose editing
    - SongEditorModule.handleInput() - chain reference editing
    - ProjectModule.handleInput() - tempo/transpose/name editing
    - InstrumentModule.handleInput() - all instrument parameter editing
  - ✅ Removed old apply functions from MainActivity
  - ✅ MainActivity reduced from 2668 → 1862 lines (806 lines removed!)
  - **Result:** Input handling now lives in modules, not MainActivity!

**See:** `REFACTORING_ROADMAP.md` for detailed step-by-step guide

### 2. Effects System (1-2 weeks) ✅ COMPLETE!
**TOP-5 Effects in PHRASE screen only:**
- [x] Arpeggio (Axx) - Note pattern automation ✅ WORKING (2026-01-20)
- [x] Offset (Oxx) - Sample start point ✅ WORKING
- [x] Volume (Vxx) - Volume automation ✅ WORKING
- [x] Kill (K00) - Stop sample immediately ✅ WORKING
- [x] Repeat (Rxx) - Retrigger with full persistence ✅ WORKING (2026-01-19)

**Architecture:** Centralized in EffectProcessor.resolveStepParams() (2026-01-10)

**TIC SYSTEM (added 2026-01-18):**
- TICS_PER_STEP = 12 (configurable in future Groove screen)
- REPEAT uses tic-interval approach (LGPT/M8 style)

**REPEAT EFFECT - Full Feature Set (2026-01-19):**

*Sub-step intervals (R01-R0B):*
  - R01 = retrig every 1 tic = 12 triggers/step (fastest)
  - R03 = retrig every 3 tics = 4 triggers/step (triplets!)
  - R06 = retrig every 6 tics = 2 triggers/step

*Multi-step intervals (R0C+):*
  - R0C (12) = every 1 step
  - R12 (18) = every 1.5 steps (dotted notes!)
  - R18 (24) = every 2 steps
  - R30 (48) = every 4 steps (4 kicks in 16-step phrase!)
  - R60 (96) = every 8 steps

*Persistence (LGPT/M8 style):*
  - REPEAT persists until cancelled by:
    1. New note on the same track
    2. Any effect in the same FX column
    3. KILL effect (K00) in any column
  - Cross-phrase persistence in Chain/Song mode!
  - Example: Phrase 00 with `C-4 R30` → Phrase 01 empty → kicks continue!

**Table screen effects:** Early Post-MVP

**See:** `MVP_ROADMAP.md` Milestone 2 for implementation details

### 3. Copy/Paste System (4-5 days) ✅ COMPLETE!
**M8-style workflow:**
- [x] Selection mode (L+B to enter/cycle: CELL → ROW → SCREEN) ✅
- [x] Copy selection (B in selection mode) ✅
- [x] Paste (L+A outside selection) ✅
- [x] Cut (L+A in selection mode) ✅
- [x] Delete selection (A+B in selection mode) ✅
- [x] Cancel selection (L alone) ✅
- [x] Copy/paste phrase steps ✅
- [x] Copy/paste chain rows ✅
- [x] Copy/paste song cells ✅
- [x] Selection auto-exits on screen navigation ✅

**Controls summary:**
| Control | Action |
|---------|--------|
| L+B | Enter/cycle selection (CELL → ROW → SCREEN) |
| B (in selection) | Copy + exit |
| L+A (in selection) | Cut (copy + delete) + exit |
| L+A (outside) | Paste at cursor |
| A+B (in selection) | Delete (no clipboard) + exit |
| L alone | Cancel selection (no copy) |

**Screens supported:** PHRASE, CHAIN, SONG

**Advanced features:** Early Post-MVP (instrument settings copy, etc.)

**See:** `MVP_ROADMAP.md` Milestone 2.5 for implementation details

### 4. Testing & Polish (1 week)
- [ ] "Hello world" song usability test (<5 min)
- [ ] Bug hunting on both devices
- [ ] Performance verification (stable 30-60fps)
- [ ] Example project creation

### 5. Documentation (3-5 days)
- [ ] README finalization
- [ ] Controls guide (including copy/paste)
- [ ] Short demo video
- [ ] Known issues list

## Post-MVP Features (Future Ideas)

### Early Post-MVP (With Mentor)
1. **Table Screen** - Effects for instruments
2. **Advanced Copy/Paste** - Instrument settings
3. **Linux Port** - GTK/Qt UI with same controllers
4. **Braids Synthesizers** - Mutable Instruments integration
5. **Remaining Effects** - Pitch, pan, filter, etc.

### Later Features
- Undo/redo
- WAV export
- Advanced filters
- Mixer screen
- Themes/polish

### System Settings (Global, Not Per-Project)
**Location:** Accessible from Project screen

Planned global settings:
- [ ] **Visualizer Module Selection** - Choose which visualizer displays
- [ ] **Theme/Color Scheme** - UI customization
- [ ] **Font Selection** - Alternative bitmap fonts
- [ ] **Song Template** - Default project structure
- [ ] **Auto-save Settings** - Frequency and behavior

### Alternative Visualizer Modules (620×70 pixel area)

**1. Oscilloscope - Waveform** ✅ **(Current)**
- Scrolling waveform display
- Adjustable gain and time window

**2. EQ Spectrum - Wave Style**
- [ ] Real-time FFT frequency analysis
- [ ] Smooth wave-like visualization

**3. EQ Spectrum - Pixel Bars** (Retro Style)
- [ ] Classic LED meter look
- [ ] 16 vertical pixel bars

**4. DB Meter - Multi-Track**
- [ ] 8 vertical meters for tracks
- [ ] Stereo master L/R meters

**5. Spectrogram** (Advanced)
- [ ] Frequency vs time visualization

## Next Steps (Priority Order)

### ✅ ARCHITECTURE REFACTORING COMPLETE!
### ✅ CODE CLEANUP COMPLETE! (2026-01-13)

**Completed this session:**
- ✅ Fixed PROJECT screen cursor LEFT/RIGHT navigation
- ✅ Fixed INSTRUMENT screen cursor LEFT/RIGHT navigation with column skipping
- ✅ Cleaned up EditorHelpers.kt: removed 15 unused functions (292→61 lines)
- ✅ All orphaned code removed, verified no broken references

**Result:** Navigation fully bidirectional, codebase cleaner! 🎉

### ✅ COPY/PASTE SYSTEM COMPLETE! (2026-01-22)

**Effects System COMPLETE:**
- ✅ OFFSET (Oxx) - Sample start point offset
- ✅ VOLUME (Vxx) - Volume automation
- ✅ KILL (K00) - Stop sample immediately
- ✅ REPEAT (Rxx) - Full implementation with persistence and multi-step intervals!
- ✅ ARPEGGIO (Axx) - Note pattern automation with ARC config!

**All TOP-5 effects done!** Moving to copy/paste system.

**REPEAT is now feature-complete:** Sub-step (R01-R0B), multi-step (R0C+), persistence, cross-phrase!

**ARPEGGIO EFFECT - Full Feature Set (2026-01-20):**

*ARP (Axx) - Arpeggio intervals:*
  - A00 = cancel arpeggio
  - A37 = minor chord (root, +3, +7)
  - A47 = major chord (root, +4, +7)
  - ACC = double octave (root, +12, +12)

*ARC (Cxx) - Arpeggio config (NEW):*
  - High nibble = mode: 0=UP, 1=DOWN, 2=PINGPONG, 3=RANDOM
  - Low nibble = speed in tics (4=default, 1=fast, 6=slow)
  - Example: C14 = DOWN mode, speed 4 tics

*Persistence (LGPT/M8 style):*
  - ARP persists until cancelled by: new note, ARP00, KILL, or same-column FX
  - ARC config persists until another ARC command
  - Cross-step phase continuity (arpeggio pattern continues across steps!)

*TODO (Post-MVP): Additional ARC modes:*
  - 4 = UP_OCT, 5 = DOWN_OCT, 6 = CHORD, 7 = SHUFFLE

**ALL TOP-5 EFFECTS COMPLETE!** Ready for copy/paste milestone.

**See:** `MVP_ROADMAP.md` Milestone 2 for full effects implementation

### This Week:
1. ✅ **Code Cleanup** - COMPLETE!
   - ✅ Delete dead code from EditorHelpers.kt
   - ✅ Verify no broken references
   - ✅ All compilation errors resolved

2. ✅ **Effects System** (Milestone 2) - COMPLETE!
   - ✅ ARPEGGIO (Axx) with ARC config (Cxx)
   - ✅ REPEAT (Rxx) with persistence
   - ✅ OFFSET, VOLUME, KILL

### Next 2 Weeks:
2. ✅ **Copy/Paste System** (Milestone 2.5) - COMPLETE!
   - ✅ M8-style selection mode (L+B to cycle)
   - ✅ Copy/paste for PHRASE, CHAIN, SONG screens
   - ✅ Selection auto-exits on screen navigation

### Following Weeks:
3. **Testing & Polish** (Milestone 3-4)
4. **Documentation & Video**
5. **MVP Release!**

## Timeline to MVP

**Realistic Timeline:** Mid-Late February 2025 (6-8 weeks from now)

```
✅ Weeks 1-2:   Refactoring (clean architecture) - COMPLETE!
Weeks 3-4:      Effects system (TOP-5 in phrase)  ← NEXT
Week 5:         Copy/paste system (M8-style)
Week 6:         Integration & testing
Weeks 7-8:      Bug fixes & performance
Week 9:         Documentation & video
```

**Status:** Ahead of schedule! Refactoring complete in 2 weeks.

**See:** `MVP_ROADMAP.md` for complete vertical slice breakdown

## Technical Notes

### Audio Engine Details
- Sample rate: 44100 Hz (forced via Oboe builder)
- Performance mode: LowLatency (enables MMAP fast audio path)
- Sharing mode: Exclusive (dedicated audio stream)
- Format: Float32, stereo output
- Buffer size: Auto-selected by Oboe for low latency (~192-480 frames)
- All samples stored as mono (stereo mixed during load)
- Base frequency stored per instrument for pitch calculation
- Playback rate = target_frequency / base_frequency
- Sample rate compensation: Automatic for non-44100Hz samples

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

## Questions for Claude Code

When starting work:
1. Read `DEVELOPMENT_STATUS.md` (this file) - know current progress
2. Read `SESSION_HISTORY.md` - see recent session details and pending tasks
3. Read `MVP_ROADMAP.md` - see complete path to MVP

**Current task:** Code Cleanup (dead code removal + state consolidation)
**See:** `SESSION_HISTORY.md` "Session 2026-01-10" for detailed TODO list

### Next Session Quick Start

1. **Delete dead code from EffectProcessor.kt** (lines 176-364):
   - `applyEffects()`, `applyEffect()`, `applyArpeggio()`, `applyOffset()`,
   - `applyVolume()`, `applyKill()`, `applyRepeat()`

2. **Consolidate MainActivity state** - replace `by remember` with controller reads:
   ```kotlin
   // BEFORE (duplicate state):
   var cursorRow by remember { mutableIntStateOf(0) }

   // AFTER (read from controller):
   val cursorRow = stateVersion.let { trackerController.cursorRow }
   ```

3. **Then implement remaining effects**: ARPEGGIO (Axx), REPEAT (Rxx)
