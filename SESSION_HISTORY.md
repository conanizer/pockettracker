# PocketTracker Development Session History

This file tracks development sessions with Claude Code to maintain context across conversations.

## Purpose
- Document what was attempted in each session
- Record lessons learned and successful patterns
- Track current state and next steps
- Help Claude understand context when resuming work

---

## Session 2025-12-26: Polish & Production Ready

### Context
- Instrument screen functional but needed polish
- Audio timing and pitch issues discovered
- Workflow needed improvement (cursor memory, quick insert)

### Goals
1. Polish instrument screen functions
2. Fix audio issues (pitch, timing, sample persistence)
3. Improve workflow with smart cursor state memory
4. Add quick insert feature for faster composition

### What Was Accomplished ✅

#### 1. Sample Persistence Fixed
**Problem**: Custom samples loaded in instruments weren't reloading when loading projects
- JSON saved `sampleFilePath` but WAV files weren't reloaded into audio engine
- Instrument parameters (ROOT, DETUNE, START, END, REVERSE, LOOP) not restored

**Solution**:
- Added `reloadProjectSamples()` function in MainActivity.kt
- Iterates through all instruments with `sampleFilePath != null`
- Calls `loadSampleFromFile()` + `updateInstrumentBaseFrequency()` + `updateInstrumentPlaybackParams()`
- Now all custom samples and parameters restore perfectly on project load

#### 2. Perfect Pitch for 44100 Hz Samples
**Problem**: Sample rate compensation had rounding errors (284.76 × 0.919 = 261.69444, not 261.63)
- Mathematical imprecision felt wrong
- 0.06 Hz error could accumulate

**Solution**:
- Set Oboe stream to **44100 Hz** (most common sample rate) via `builder.setSampleRate(44100)`
- **44100 Hz samples**: Perfect pitch with zero error (ratio = 1.0 exactly!)
- **48000 Hz samples**: Still compensated correctly (ratio = 44100/48000 = 0.91875)
- Best of both worlds - precision for common case, compensation for others

#### 3. Low-Latency Audio for Tight Timing
**Problem**: Playback timing still had jitter between notes despite hybrid timing approach
- Notes triggered from UI thread → JNI → C++ → audio callback (variable latency)

**Solution**:
- Enabled `PerformanceMode::LowLatency` in native-audio.cpp
- Changed to `SharingMode::Exclusive` for dedicated audio stream
- Enables MMAP (Android's fast audio path)
- Dramatically reduced buffer sizes and latency
- Result: Much tighter, more consistent timing

#### 4. File Browser Preview Pitch Fixed
**Problem**: Samples in file browser played ~3 semitones wrong
- Was passing `adjustedBaseFreq` as target frequency (backwards!)
- Rate = 261.63 / 284.76 = 0.919 (too slow) → then fixed → 1.088 (too fast)

**Solution**:
- Corrected parameter order in `previewSampleFile()`:
  - `targetFreq = c4Freq` (261.63 Hz - what we want to hear)
  - `baseFreq = adjustedBaseFreq` (284.76 Hz - compensated reference)
  - `rate = 261.63 / 284.76 = 0.919` ✓ (plays slower to compensate for device sample rate)

#### 5. Instrument Preview Pitch Fixed
**Problem**: Instrument preview also played too high
- `calculateInstrumentBaseFrequency()` included sample rate ratio in target frequency
- Function used for both target (shouldn't include ratio) and base (should include ratio)

**Solution**:
- Rewrote `previewInstrument()` to calculate target without sample rate ratio
- Uses compensated base frequency for correct playback
- Now matches file browser preview pitch perfectly

#### 6. Smart Cursor State Memory (Bidirectional)
**Problem**: Cursor only remembered last EDITED values, not cursor position
- Jumping from Song→Chain didn't show the chain you were on
- One-directional only (couldn't go Instrument→Phrase and remember phrase)

**Solution**:
- Added tracking variables: `lastEditedInstrument`, `lastEditedNote`, `lastEditedVolume`, `lastEditedTranspose`
- **Capture on navigation**: When leaving screen, capture value under cursor
- **Restore on navigation**: When entering screen, restore last captured value
- **Update on edit**: Track when values change via A+direction or manual entry
- Implemented in both R+LEFT and R+RIGHT handlers
- Works in both directions: Phrase↔Instrument, Chain↔Phrase, Song↔Chain

#### 7. Quick Insert Feature
**Problem**: Repetitive data entry - constantly re-entering same values

**Solution**:
- A button on empty row inserts last-used values:
  - **Phrase screen**: Insert last note/instrument/volume
  - **Chain screen**: Insert last phrase/transpose
  - **Song screen**: Insert last chain
- Dramatically speeds up composition workflow
- Natural workflow: compose once, then quick-insert variations

#### 8. Empty Instruments Fixed
**Problem**: Instruments 0C-FF defaulted to `sampleId = 0` (kick.wav)
- Empty instruments had a sample loaded

**Solution**:
- Changed default `sampleId` from 0 to -1 in TrackerData.kt
- Instruments 00-0B still initialize with resource samples (as intended)
- Instruments 0C-FF are truly empty until loaded

#### 9. Last Edited Value Tracking in Song/Chain
**Problem**: Song and Chain screens didn't track last edited values properly
- Press A → insert chain 00, press A+UP → becomes 01, move row, press A → still inserts 00 ❌

**Solution**:
- Added `lastEditedChain/Phrase/Transpose` tracking in `applySongInputAction()` and `applyChainInputAction()`
- Updates when values change via A+direction combos
- Now correctly inserts last edited value

### Files Modified
**Native Code (C++):**
- `native-audio.cpp` - Set sample rate to 44100Hz, enabled LowLatency + Exclusive modes

**Kotlin:**
- `MainActivity.kt` - Added reloadProjectSamples(), cursor state tracking, quick insert, last edited tracking
- `TrackerAudioEngine.kt` - Fixed preview pitch calculations for both file browser and instrument
- `TrackerData.kt` - Changed default sampleId from 0 to -1
- `PixelPerfectRenderer.kt` - (Already had hybrid timing from previous session)

**Documentation:**
- `DEVELOPMENT_STATUS.md` - Updated with all new features and fixes
- `SESSION_HISTORY.md` - Added this session

### Key Lessons Learned

#### ✅ Proper Audio Configuration Matters
- Setting Oboe to correct sample rate (44100Hz) eliminates precision issues
- LowLatency mode makes huge difference in timing consistency
- Exclusive mode gives best performance for music apps

#### ✅ Sample Rate Compensation Done Right
- Force stream to common sample rate (44100Hz)
- Compensate only when sample rate differs
- Gives perfect pitch for 99% of samples, correct pitch for rest

#### ✅ Bidirectional State Tracking
- Capture value when LEAVING screen, not just when editing
- Restore value when ENTERING screen
- Creates natural, intuitive workflow

#### ✅ Quick Insert Patterns
- Track "last used" values automatically
- Single button to reuse = huge workflow speedup
- Inspired by tracker conventions (M8, LSDJ)

### Current State (End of Session)
- ✅ All instrument screen functions polished
- ✅ Audio pitch perfect for 44100Hz samples
- ✅ Audio timing tight with low-latency mode
- ✅ Sample persistence working perfectly
- ✅ Smart cursor memory in both directions
- ✅ Quick insert feature speeds up composition
- ✅ Empty instruments are truly empty
- ✅ Ready for commit and push!

### Next Steps
- Commit all changes with descriptive message
- Push to GitHub
- Consider implementing:
  - Copy/paste for phrases/chains
  - Effect commands (volume, pitch, filter)
  - Table screen for arpeggios
  - Mixer screen

---

## Session 2024-12-22: State Refactoring Attempt & Rollback

### Context
- Continued from previous session that ran out of context
- Previous session had created a refactoring plan (see `.claude/plans/unified-napping-gosling.md`)
- Goal: Refactor MainActivity.kt state management (1,852 lines → ~800 lines)

### What Was Attempted
1. **State Consolidation**: Convert 26+ scattered state variables to unified `AppState` data class
2. **Immutable Updates**: Replace direct mutation + `projectVersion++` with immutable `.copy()` pattern
3. **NavigationController**: Extract 175 lines of navigation logic into separate class

### What Happened
1. Started refactoring by migrating handlers to use `appState`
2. **Breakage Discovered**: Most button combos stopped working
   - A+DPad combos not working
   - B+DPad editing wrong cell (row 0, col 0 regardless of cursor position)
   - File browser combos broken
3. **Root Causes Identified**:
   - Mixed state: `appState` and old variables independent, not synced
   - ButtonHandlers wrapped in `remember()` captured stale initial state
   - Layout functions still reading old variables
4. **Attempted Fixes**: Added sync code, removed `remember` wrapper → **More breakage**

### The Rollback
1. Decided to rollback to working state before refactoring
2. **Git Rollback Issues**:
   - `git reset --hard 532b0a2` → **Compilation errors** (commit was broken)
   - `git reset --hard 1ac1edc` → Too far back, missing file browser
3. **Discovery**: Commit 532b0a2 had never compiled - was committed incomplete
4. **Solution**: User restored working version from **Android Studio Local History** ✅
5. Pushed restored version to GitHub (commit `22ffee0`)

### Key Lessons Learned

#### ❌ What NOT to Do
- **Big-bang refactoring** - Don't change core architecture all at once
- **Mixed patterns** - Don't have old and new state systems coexisting without clear migration path
- **Commit broken code** - Even WIP commits should compile
- **Fix forward when fundamentally broken** - Rollback first, analyze second

#### ✅ What WORKS
- **Android Studio Local History** - Excellent safety net for recent changes
- **Small incremental changes** - Test after each logical step
- **Compile-Test-Commit cycle** - Verify app works before committing
- **Git tags for known-good states** - Tag commits verified to work

#### ✅ Better Approaches for Future Refactoring

**1. Incremental Migration Pattern**
```
Phase 1: Add new system alongside old (read-only observation)
Phase 2: Migrate ONE screen to new system
Phase 3: Test thoroughly (all buttons, navigation, editing)
Phase 4: Repeat for other screens
Phase 5: Remove old system when migration complete
```

**2. Feature Flag Pattern**
```kotlin
const val USE_NEW_STATE = false

if (USE_NEW_STATE) {
    // New architecture
} else {
    // Old architecture (working)
}
```

**3. Compile-Test-Commit Workflow**
```
1. Make change
2. ./gradlew assembleDebug (must succeed)
3. Smoke test: Launch app, move cursor, press A/B, navigate screens
4. Git commit if all working
5. Git tag for verified working state
```

### Current State (End of Session)
- ✅ Working version restored with complete file browser functionality
- ✅ Commit: `22ffee0` on branch `claude/keyboard-input-layout-01KisvUqQtDHG9cSjHA353c8`
- ✅ All button combos working correctly
- ⏸️ State refactoring plan shelved - current architecture works fine
- 📁 File browser fully functional with navigation, rename, delete modes

### Files Modified Today (Restored Version)
- CursorContext.kt - Added `browserLine()` and `character()` methods
- FileBrowserModule.kt - Complete file browser implementation
- FileManager.kt - File operations
- InputHandler.kt - Generic input handling
- NavigationMapModule.kt - Added FILE_BROWSER cases
- ScreenLayouts.kt - Added fileBrowserState parameter
- ScreenType.kt - Added FILE_BROWSER enum
- SongEditorModule.kt - Updates
- TrackerAudioEngine.kt - Updates

### Next Steps (Recommendations)
1. **Don't refactor state management yet** - Current system works, not worth the risk
2. **Focus on features** - Add new screens/functionality using existing patterns
3. **If refactoring needed later**:
   - Use incremental migration pattern
   - Test after EVERY small change
   - Keep old system working throughout migration
   - Use feature flags to toggle between old/new

### Notes for Future Claude Sessions
- Read this file at session start to understand what's been tried
- The current state management (scattered vars + `projectVersion++`) works fine despite being "antipattern"
- **CursorContext system** (CursorContext.kt, GenericInputHandler) is excellent - DON'T change it
- **ButtonHandlers data class** works well - DON'T wrap in `remember()`
- Android Studio Local History is better than git for recent recovery
- User prefers working code over "clean" architecture

---

## Session Template for Future Entries

```markdown
## Session YYYY-MM-DD: [Brief Description]

### Context
- What was the starting state?
- What problem were we trying to solve?

### What Was Attempted
- List of changes/features attempted

### What Happened
- Results (success/failure)
- Issues encountered

### Lessons Learned
- What worked well
- What didn't work
- Better approaches discovered

### Current State (End of Session)
- Working/broken?
- Latest commit/branch
- Files modified

### Next Steps
- Recommendations for next session
```

---

## Quick Reference: Current Architecture (Don't Change These!)

### ✅ Keep As-Is (Working Well)
- **CursorContext system** - Generic input handling based on data type
- **GenericInputHandler** - Converts cursor context to input actions
- **ButtonHandlers data class** - Contains all button press lambdas
- **TrackerModule interface** - Screen module system
- **State variables + projectVersion** - Yes it's an antipattern, but it WORKS

### 📋 Current State Management Pattern
```kotlin
// In MainActivity.kt:
var project by remember { mutableStateOf(Project()) }
var cursorRow by remember { mutableIntStateOf(0) }
var projectVersion by remember { mutableIntStateOf(0) }
// ... 20+ more variables

// When data changes:
project.phrases[i].steps[j].note = newNote
projectVersion++  // Force recomposition
```

**Why it works**: Predictable, simple, every screen uses same pattern

**Why not refactor**: High risk, low reward - would need to change ALL screens simultaneously

### 🎯 Development Best Practices (Learned from Experience)

1. **Test Frequently**
   - After any change, run `./gradlew assembleDebug`
   - Basic smoke test: Launch → Move cursor → Press A/B → Navigate screens

2. **Commit Working Code**
   - Only commit code that compiles
   - Add descriptive commit messages
   - Tag verified working states: `git tag working-YYYY-MM-DD`

3. **Use Local History**
   - Android Studio → Right-click file → Local History → Show History
   - Better than git for recovering recent working states

4. **Incremental Changes**
   - One logical change at a time
   - Test before moving to next change
   - Don't batch unrelated changes

5. **When Things Break**
   - Don't try to "fix forward" complex breakage
   - Rollback to last working state
   - Analyze what went wrong
   - Try again with smaller changes

---

## Session 2025-12-23: Instrument Screen Completion & Critical Bug Fixes

### Context
- Started with instrument screen implementation (from previous plan)
- File browser visibility issues resolved from previous session
- Instrument screen UI mostly complete but had bugs and missing audio functionality

### What Was Implemented

#### 1. Instrument Screen Features
- **L+LEFT/RIGHT Navigation**: Cycle through instruments 00-FF
- **Sample Loading**: Load WAV files from file browser into instrument slots
- **Sample Name Display**: Shows loaded filename (read-only, not character editing)
- **ROOT Parameter**: Note value that defines sample's base pitch
- **DETUNE Parameter**: Fine-tuning (±8 semitones with 1/16 precision)
  - Format: 0x00-0xFF hex, high nibble = semitones, low nibble = sixteenths
  - Center at 0x80 = no detune
- **Sample Preview**: START button plays instrument with ROOT+DETUNE applied
- **Status Messages**: Auto-dismiss after 5 seconds (matches project screen)
- **12 Hardcoded Samples**: Instruments 00-0B initialized with resource samples

#### 2. File Browser Enhancement
- **Sample Preview**: START button plays WAV at C-4 reference pitch
- Helps audition samples before loading

#### 3. Audio Engine Improvements
- **256 Sample Slots**: Expanded from 12 to 256 (native C++ code)
- **Stereo/Mono Support**: Auto-detects and mixes stereo → mono during load
- **Preview Functions**:
  - `previewSampleFile()` - Preview WAV from file browser
  - `previewInstrument()` - Preview with ROOT+DETUNE applied
  - `calculateInstrumentBaseFrequency()` - Combines ROOT+DETUNE
  - `updateInstrumentBaseFrequency()` - Updates stored base frequency

### Critical Bugs Fixed

#### Bug #1: Stereo/Mono WAV Loading (1 Octave Low)
**Symptoms**: All stereo samples played 1 octave too low
**Root Cause**:
- Stereo WAVs have interleaved L/R samples
- Code loaded all samples sequentially (doubled sample count)
- Playing 2x samples at same rate = 2x duration = half speed = 1 octave down

**Fix**:
- Parse WAV header bytes 22-23 to read channel count
- If stereo: Mix to mono by averaging L+R channels
- Result: Correct playback pitch for all samples

**Files**: `TrackerAudioEngine.kt` - `loadWavFile()`, `loadWavFileFromPath()`

#### Bug #2: MIDI Note Conversion (1 Octave Off)
**Symptoms**: All notes calculated 1 octave too low (C-4 = 130.81 Hz instead of 261.63 Hz)
**Root Cause**:
- `toMidi()` formula: `octave * 12 + pitch` (missing +1 offset)
- `fromMidi()` formula: `octave = midi / 12` (missing -1 offset)
- Standard MIDI: C-4 = 60, formula should be `(octave + 1) * 12 + pitch`

**Fix**:
- `toMidi()`: Changed to `(octave + 1) * 12 + pitch`
- `fromMidi()`: Changed to `octave = midi / 12 - 1`
- Now C-4 correctly = MIDI 60 = 261.63 Hz

**Files**: `TrackerData.kt` - Note class

#### Bug #3: File Browser Preview Crash (SIGSEGV)
**Symptoms**: Fatal crash when previewing multiple samples rapidly
**Root Cause**: Race condition
- Main thread: Load new sample → delete old slot 255 → allocate new memory
- Audio thread: Still playing old slot 255 → accesses deleted memory → CRASH

**Fix**: Call `native_stopAll()` before loading preview sample
**Files**: `TrackerAudioEngine.kt` - `previewSampleFile()`

### Other Fixes & Improvements

#### Instrument Value as Hex Byte
- Changed from cycling 0-3 to full 00-FF range with A+LEFT/RIGHT
- Uses same `hexByte()` cursor context as volume
**Files**: `CursorContext.kt`, `MainActivity.kt`

#### Default Directories
- Project screen: `/Documents/PocketTracker/Projects/`
- Instrument screen: `/Documents/PocketTracker/Samples/`
**Files**: `FileManager.kt`

#### Sample-to-Instrument Mapping
- Fixed playback to use `instrument.sampleId` instead of wrapping to 0-11
- Added `project` parameter to `playNote()` for proper instrument lookup
**Files**: `TrackerAudioEngine.kt`, `PixelPerfectRenderer.kt`

### Lessons Learned

#### ✅ What Worked Well

1. **Debugging Audio Issues Systematically**
   - Added detailed logging at each step
   - Traced frequency calculations from Note → MIDI → Frequency
   - Found exact point where values were wrong

2. **Understanding Native/Kotlin Bridge**
   - Identified array size limits in C++ (12 vs 256 slots)
   - Fixed race conditions between threads
   - Proper synchronization with `native_stopAll()`

3. **WAV File Format Knowledge**
   - Learned to parse WAV headers properly
   - Understood stereo interleaving
   - Implemented proper channel mixing

4. **Incremental Testing**
   - Fixed one bug at a time
   - Verified each fix with logs before moving on
   - User tested each build to confirm

#### 🐛 Bug Patterns to Watch For

1. **Sample Rate Mismatches**: Always check WAV header, don't assume format
2. **Thread Safety**: Audio runs on separate thread - protect shared data
3. **Array Bounds**: Keep Kotlin and C++ array sizes synchronized
4. **MIDI Conventions**: Different standards exist, verify with known values (C-4 = 60)

### Current State (End of Session)

#### ✅ Fully Working
- Instrument screen with all parameters functional
- Sample loading and preview (file browser + instrument screen)
- Stereo/mono WAV support
- Correct pitch calculation for all notes
- 256 instrument slots with independent tuning
- L+LEFT/RIGHT instrument navigation
- Status message system

#### ⏸️ UI-Only (Audio TODO)
- Sample start/end points
- Loop modes (off/fwd/png)
- Reverse playback

#### 📋 Files Modified
- `TrackerAudioEngine.kt` - Stereo/mono, preview functions, base frequency management
- `TrackerData.kt` - MIDI conversion fix, 12-sample initialization
- `MainActivity.kt` - START button handlers, instrument navigation, auto-dismiss
- `InstrumentModule.kt` - Sample name display, cursor contexts
- `native-audio.cpp` - Expanded to 256 slots
- `CursorContext.kt` - Instrument as hex byte
- `PixelPerfectRenderer.kt` - Pass project to playNote()
- `FileManager.kt` - Sample directory helper

#### 🔄 Latest Commit
Branch: `claude/keyboard-input-layout-01KisvUqQtDHG9cSjHA353c8`
Ready to commit with message about instrument screen completion and bug fixes

### Next Steps (Priority Order)

1. **Commit Today's Work**
   - Comprehensive commit message
   - Push to GitHub
   - Tag as working state

2. **Effect System** (Next Major Feature)
   - Define effect types (pitch, volume, filter, etc.)
   - Implement effect processing in C++ audio callback
   - Add FX editing UI to phrase screen

3. **Sample Playback Parameters** (Audio Engine)
   - Implement start/end points in C++
   - Add loop support (forward, ping-pong)
   - Add reverse playback

4. **Table Screen**
   - Arpeggio tables
   - Volume/pitch envelopes
   - Reference from phrase FX commands

### Notes for Future Claude Sessions

#### 🎯 Key Technical Details
- **MIDI Convention**: C-4 = 60 (middle C), formula `(octave+1)*12+pitch`
- **Detune Format**: High nibble = semitones, low nibble = 1/16 semitone, center = 0x80
- **Sample Slots**: 00-0B = hardcoded resources, 0C-FF = user samples
- **Stereo Handling**: All samples mixed to mono during load (line-by-line L+R average)
- **Thread Safety**: Always `stopAll()` before modifying sample data from main thread

#### ✅ Architecture Patterns That Work
- Generic input system (CursorContext) - works perfectly, don't change
- Status message auto-dismiss with LaunchedEffect
- Sample preview using temporary slot 255
- Base frequency stored per instrument, not per sample

#### ⚠️ Known Quirks
- Input event warnings: Harmless, goes away after device restart
- Focus requester needs small delay on first request
- Native audio engine runs on separate thread - be careful with sample memory

---

*Last Updated: 2025-12-23*
