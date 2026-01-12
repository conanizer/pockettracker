# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

## 🚨 BEFORE STARTING ANY TASK

**ALWAYS read these files first:**

1. **DEVELOPMENT_STATUS.md** - What's done, what's remaining
2. **MVP_ROADMAP.md** - Overall path to MVP

**Current Phase:** Architecture Refactoring (Finalizing)

**Current Task:** Extract controllers from MainActivity

---

## 📋 Quick Reference

### Current Project State (January 2025)

**Status:** ~95% complete to MVP
- ✅ Audio engine (professional-grade, sample-accurate)
- ✅ All UI screens working
- ✅ Playback system complete
- ⚠️ Effects system NOT implemented yet
- ⚠️ Copy/paste NOT implemented yet
- 🚧 **Currently refactoring architecture** (preparing for Linux port)

**Next Steps:**
1. Complete refactoring (Phase 4 - extracting controllers)
2. Implement effects system (TOP-5 effects in phrase screen)
3. Implement copy/paste (M8-style)
4. Testing & polish
5. MVP release (Late February 2025)

---

## 🏗️ Architecture Decision: Option B (Foundational)

**Decision made:** Separate controllers from Day 1

**Why:**
- Linux port planned post-MVP
- Clean architecture from start = easier maintenance
- Write effects code once (portable immediately)

**Target structure:**
```
core/logic/
├── TrackerController.kt      # Main coordinator
├── InputController.kt         # Button handling
├── PlaybackController.kt      # Playback scheduling
├── EffectProcessor.kt         # Effect calculations
├── InstrumentController.kt    # Sample management
├── FileController.kt          # Save/load
└── ClipboardManager.kt        # Copy/paste
```

**See:** `TECHNICAL_ARCHITECTURE.md` for complete architecture

---

## 🎯 Project Overview

PocketTracker is an Android music tracker application inspired by M8 and LSDJ, designed for gaming handhelds and budget Android devices with a target resolution of 640×480.

**Key Technologies:**
- Kotlin with Jetpack Compose for UI
- Native C++ audio engine using Oboe library
- JNI bridge between Kotlin and C++
- Kotlinx serialization for project save/load
- Custom pixel-perfect rendering system

**Minimum Requirements:**
- Android 8.0 (API 26)
- 64-bit only: arm64-v8a, x86_64
- ~512MB RAM (works on 1GB Miyoo Flip!)
- 640×480 minimum resolution

**Tested Devices:**
- **Primary:** Miyoo Flip (1GB RAM, Android 13, GammaCoreOS)
- **Secondary:** Ayaneo Pocket Air Mini (3GB RAM, Android 11) - arriving soon

---

## 🔨 Build Commands

```bash
# Build the project
./gradlew build

# Clean build
./gradlew clean

# Install debug build to connected device/emulator
./gradlew installDebug

# Run tests
./gradlew test

# Assemble release APK
./gradlew assembleRelease
```

**Important:** The project uses CMake for native code compilation. NDK and CMake 3.22.1+ must be installed.

---

## 📐 Architecture

### Data Flow: Hierarchical Tracker Structure

The tracker uses a 4-level hierarchical composition model:

```
Project (global settings, tempo, 256 phrases, 256 chains, 8 tracks)
    ↓
Song (8 tracks, each containing chain references)
    ↓
Chain (16 phrase references + transpose values per slot)
    ↓
Phrase (16 steps, each with note/instrument/volume/effects)
    ↓
PhraseStep (note, instrument, volume, 3 effect slots)
```

**Data Models (TrackerData.kt):**
- `Note`: MIDI-based note representation (C-0 to B-9), converts to frequency
- `PhraseStep`: Single row in phrase (note + instrument + volume + 3 FX slots)
- `Phrase`: 16 steps, identified by ID (0-255)
- `Chain`: 16 phrase references with per-slot transpose values
- `Track`: Sequence of chain references in the song
- `Instrument`: Sample ID + ROOT + DETUNE + playback parameters
- `Project`: Root container with all data, serializable to JSON

**Important:** `TrackerData.kt` is already platform-agnostic (no Android dependencies)!

### Rendering Architecture: Modular Canvas System

The UI uses a custom pixel-perfect rendering system built on Jetpack Compose Canvas:

**Core Components:**
- `PixelPerfectRenderer.kt`: Main renderer with letterboxing and integer scaling
- `TrackerLayout`: Positions and draws all modules on 640×480 canvas
- `TrackerModule` interface: All screen modules implement this (draw function, width/height)

**Design Constants:**
- Screen: 640×480 pixels (4:3 aspect ratio)
- Font: 5×5 bitmap scaled 3× = 15×15 pixel characters
- Modules positioned with 6px spacing between, 10px side margins

**Module System:**
- `OscilloscopeModule`: 620×70px waveform display (top of screen)
- `PhraseEditorModule`: 620×392px phrase editing interface
- `ChainEditorModule`: Chain editing screen
- `SongEditorModule`: Song arrangement screen
- `InstrumentModule`: Instrument editing
- `ProjectModule`: Project settings (tempo, name, save/load)
- `NavigationMapModule`: 80×105px navigation grid display
- `FileBrowserModule`: File/folder navigation

Each module receives state objects and renders itself independently.

### Audio Engine: Sample-Accurate Queue System

**Architecture:** Professional-grade audio engine with sample-accurate note scheduling, matching M8/LGPT/Picotracker timing precision (<0.02ms jitter).

**Kotlin Layer (TrackerAudioEngine.kt):**
- JNI bridge to C++ audio engine
- High-level functions: `playPhrase()`, `playChain()`, `playSong()`
- Note scheduling with sample-accurate frame timing
- 2-phrase lookahead buffering for smooth continuous playback

**C++ Layer (native-audio.cpp):**
- Oboe-based audio stream (44.1kHz, LowLatency, Exclusive mode)
- Sample-accurate priority queue (`std::priority_queue<ScheduledNote>`)
- 8-voice polyphony with per-track voice stealing
- Linear interpolation for pitch-shifting (professional quality)
- Sample playback: start/end points, reverse, loop modes (forward/ping-pong)
- Resonant biquad filters (LP/HP/BP)
- Real-time waveform capture for oscilloscope

**Key Features:**
- Frame-precise triggering (no timing jitter)
- Continuous phrase/chain/song playback
- Automatic sample rate compensation (handles 44100Hz, 48000Hz samples)
- Thread-safe queue management (mutex-protected)
- <50ms startup latency (instant playback feel)

### File Management

**Project Files (.ptp):**
- JSON serialization via Kotlinx Serialization
- Stores: All phrases, chains, song, instruments, settings
- Default location: `/Documents/PocketTracker/Projects/`

**Sample Files (.wav):**
- Loaded via ResourceLoader (default samples) or FileSystem (custom samples)
- Auto-conversion: Stereo → mono
- Automatic sample rate detection and compensation
- 256 instrument slots (00-0B = defaults, 0C-FF = custom)

**Current Implementation:**
- `FileManager.kt` - Handles save/load operations
- **Note:** Will be refactored to use `IFileSystem` interface for portability

---

## 🎵 MVP Remaining Work

### 1. Effects System (1-2 weeks)

**TOP-5 Effects in PHRASE screen only:**
- Arpeggio (Axx) - Note pattern automation
- Offset (Oxx) - Sample start point automation(DONE)
- Volume (Vxx) - Volume automation within step(DONE)
- Kill (K00) - Stop sample immediately(DONE)
- Repeat (Rxx) - Retrigger sample N times per step

**Implementation:**
- Parser: Extract fx1Type/fx1Value from PhraseStep
- Processor: EffectProcessor.kt calculates effect behavior
- Audio: Apply effects in C++ audio callback

**Table screen effects:** Early Post-MVP

**See:** `MVP_ROADMAP.md` Milestone 2 for detailed implementation

### 2. Copy/Paste System (4-5 days)

**M8-style workflow:**
- Selection mode (SELECT+B to enter)
- Copy selection (B in selection mode)
- Paste (SELECT+A)
- Cut (A+B)
- Clipboard indicator in header row

**Implementation:**
- InputController: Manages selection state
- ClipboardManager: Handles copy/paste/cut operations
- UI: Visual selection highlighting

**See:** `MVP_ROADMAP.md` Milestone 2.5 for detailed implementation

### 3. Testing & Polish (1 week)

- Usability testing ("hello world" in <5 min)
- Bug hunting on Miyoo Flip and Ayaneo
- Performance verification
- Example project creation

### 4. Documentation (3-5 days)

- README finalization
- Controls guide (including copy/paste)
- Short demo video
- Known issues list

---

## 🧭 Navigation Between Screens

**5×5 Navigation Grid:**

```
  0       1         2       3       4
┌───────────────────────────────────────┐
│ PROJ    SONG    CHAIN   PHRASE   INST │ 0
│                                        │
│ MIXER   FX      TABLE   GROOVE   NAV  │ 1
│                                        │
│ (empty) (empty) (empty) (empty) SCALE │ 2
│                                        │
│ (empty) (empty) (empty) (empty) MODS  │ 3
│                                        │
│ (empty) (empty) (empty) (empty) (empty)│ 4
└───────────────────────────────────────┘
```

**Navigation:**
- L/R + DPAD: Move between screens
- Release L/R: Jump to selected screen

**Implemented Screens:**
- Row 0: PROJECT, SONG, CHAIN, PHRASE, INSTRUMENT ✅
- Others: Coming in Post-MVP

---

## 🎮 Input System

### Generic Input Handler

**Philosophy:** One input system for both physical buttons AND touchscreen.

**Button Mappings:**
- DPAD: Cursor movement
- A: Confirm, enter edit mode, quick insert
- B: Cancel, exit, copy (in selection)
- START: Play/stop
- SELECT: Context actions, paste (SELECT+A)
- L/R: Screen navigation, modifiers

**Special Combos:**
- A + DPAD: Value editing (increment/decrement)
- SELECT + B: Enter selection mode
- SELECT + A: Paste clipboard
- A + B: Cut selection
- L/R + DPAD: Screen navigation

**Virtual Controls:**
- Automatically enabled on touchscreen-only devices
- Disabled on devices with physical buttons (via DeviceAdapter)

**See:** `INPUT_COMBINATIONS.md` for complete reference

---

## 🎨 Code Style & Conventions

### Kotlin Code

**File Organization:**
```kotlin
// Imports
import androidx.compose.runtime.*
import kotlin.math.*

// Constants (top-level)
const val SCREEN_WIDTH = 640
const val SCREEN_HEIGHT = 480

// Data classes
data class Note(val octave: Int, val pitch: Int)

// Main classes
class TrackerController(/* ... */) {
    // Properties
    var currentScreen by mutableStateOf(ScreenType.PHRASE)
    
    // Public functions
    fun handleInput() { }
    
    // Private helpers
    private fun validate() { }
}
```

**Naming:**
- Classes: PascalCase (`TrackerController`)
- Functions: camelCase (`playPhrase()`)
- Constants: UPPER_SNAKE_CASE (`SCREEN_WIDTH`)
- Private members: camelCase with no prefix

**Comments:**
```kotlin
// Explain WHY, not WHAT (code should be self-documenting)
// Good:
val ratio = deviceRate / sampleRate  // Compensate for sample rate mismatch

// Bad:
val ratio = deviceRate / sampleRate  // Calculate ratio
```

### C++ Code

**Naming:**
```cpp
// Constants: UPPER_SNAKE_CASE
const int MAX_VOICES = 8;

// Classes: PascalCase
class AudioVoice { };

// Functions: camelCase
void scheduleNote(long frame, int sampleId);

// Member variables: camelCase with no prefix
class Voice {
    float position;
    bool active;
};
```

**Memory Management:**
```cpp
// Prefer std::vector over raw arrays
std::vector<float> buffer(size);

// Use mutex for thread-safe access
std::mutex queueMutex;
std::lock_guard<std::mutex> lock(queueMutex);
```

---

## 🚨 Critical Rules for Portability

**NEVER do this in business logic code:**

```kotlin
❌ import android.content.Context
❌ import androidx.compose.ui.*
❌ R.raw.sample_kick
❌ File("/sdcard/...")

// These lock code to Android!
```

**Instead, use interfaces:**

```kotlin
✅ interface IResourceLoader {
    fun loadWav(name: String): FloatArray
}

✅ interface IFileSystem {
    fun readFile(path: String): String
}

✅ class PlaybackController(
    private val audio: IAudioBackend  // Interface, not Android JNI!
)
```

**Layers:**
- **core/logic/** - NO Android imports allowed!
- **platform/android/** - Android imports OK here
- **platform/linux/** (future) - Linux-specific code

---

## 🧪 Testing Strategy

### ⚠️ CRITICAL RULE: NEVER COMMIT WITHOUT TESTING

**Before ANY git commit:**
1. ✅ Code must compile without errors
2. ✅ App must run on real device or emulator
3. ✅ Changed features must be tested and verified working
4. ✅ Check logcat for errors or warnings

**Why this matters:**
- Compilation success ≠ working code
- Runtime errors only appear on device
- Bad commits waste time and break history
- Real testing catches issues immediately

**If you can't test right now:**
- DON'T commit yet
- Ask user to test first
- Wait for confirmation before committing

---

### After Each Code Change:

**Minimal Test:**
1. App compiles without errors
2. App runs on device/emulator without crashing
3. Changed feature still works
4. Check logcat for errors

**Full Functionality Test (After Phase/Milestone):**
1. Create new project
2. Load sample
3. Create phrase with notes
4. Create chain
5. Create song
6. Play song (verify audio)
7. Save project
8. Load project
9. Verify all data identical

## 📝 Git Workflow

**Branch Strategy:**
```
main              ← Stable, working code
  ↓
develop           ← Integration branch
  ↓
feature/effects   ← Feature branches
feature/copy-paste
refactor/phase-4
```

**Commit Message Format:**
```
[Category] Brief description

- What changed
- Why it changed
- What was tested

Categories:
- [Refactor] - Architecture improvements
- [Feature] - New functionality
- [Fix] - Bug fixes
- [Audio] - Audio engine changes
- [UI] - UI/rendering changes
- [Docs] - Documentation updates
```

**Example:**
```
[Refactor 4.2] Extract PlaybackController from MainActivity

- Created PlaybackController.kt in core/logic/
- Moved playPhrase(), playChain(), playSong() functions
- MainActivity now delegates to controller
- Tested: All playback modes work ✅
```

---

## 🎯 Current Task Workflow

### When Starting Work:

**Step 1: Understand Context**
```
Read in this order:
1. DEVELOPMENT_STATUS.md - What's done
2. Current task document:
   - If effects: MVP_ROADMAP.md Milestone 2
   - If copy/paste: MVP_ROADMAP.md Milestone 2.5
3. TECHNICAL_ARCHITECTURE.md - How it should work
```

**Step 2: Ask Questions**
```
Before writing code, verify:
- Which phase/step am I on?
- What's the Definition of Done for this step?
- Are there any dependencies I need?
- What could go wrong?
```

**Step 3: Implement**
```
- Follow roadmap step-by-step
- Don't skip steps
- Test after each logical change
- Comment complex logic
```

**Step 4: Verify**
```
- Compiles without errors
- Feature works as specified
- No regressions in other features
- Ready to commit
```

---

## ⚠️ Common Pitfalls

### 1. Android Dependencies in Core Logic

**Problem:**
```kotlin
// In PlaybackController.kt
import android.content.Context  // ❌ Not portable!

class PlaybackController(context: Context) {
    init {
        context.resources.getString(...)  // ❌ Android-specific
    }
}
```

**Solution:**
```kotlin
// PlaybackController.kt (portable!)
class PlaybackController(
    private val audio: IAudioBackend  // ✅ Interface
) {
    // No Android imports!
}
```

### 2. Direct Object Mutation (Compose Issue)

**Problem:**
```kotlin
val phrase = project.phrases[0]
phrase.steps[0].note = Note.C4  // ❌ Compose won't detect change!
```

**Solution:**
```kotlin
project.phrases[0] = project.phrases[0].copy(
    steps = project.phrases[0].steps.mapIndexed { i, step ->
        if (i == 0) step.copy(note = Note.C4) else step
    }
)
// ✅ New object, Compose recomposes
```

### 3. Skipping Refactoring Steps

**Problem:**
- Trying to extract all controllers at once
- Not testing after each extraction
- Diverging from roadmap

**Solution:**
- Extract ONE controller at a time
- Test after each extraction
- Follow roadmap EXACTLY

---

## 📚 Key Documents Reference

| Document | Purpose | When to Read |
|----------|---------|--------------|
| **DEVELOPMENT_STATUS.md** | Current progress | Every session start |
| **REFACTORING_ROADMAP.md** | Step-by-step refactoring | During Phase 4 |
| **MVP_ROADMAP.md** | Feature roadmap | Planning features |
| **TECHNICAL_ARCHITECTURE.md** | System design | Understanding architecture |
| **INPUT_COMBINATIONS.md** | Controls reference | Adding input handling |
| **REFACTORING_SIMPLE_EXPLANATION.md** | Simple concepts | Explaining to others |

---

## 💡 Tips for Claude Code

1. **Always read task context first** - Don't assume you know what's next
2. **One thing at a time** - Don't try to refactor + add features simultaneously
3. **Test after every change** - Broken code compounds
4. **Ask before major changes** - Confirm approach with developer
5. **Follow the roadmap** - It's there for a reason
6. **Keep portability in mind** - No Android imports in core logic!
7. **Immutable state for Compose** - Always copy, never mutate

---

## ✅ Definition of Done (MVP)

**MVP is complete when:**
- [ ] All refactoring phases complete (portable architecture)
- [ ] TOP-5 effects working in phrase screen
- [ ] Copy/paste working (M8-style)
- [ ] Tested on Miyoo Flip and Ayaneo
- [ ] README complete with installation guide
- [ ] Demo video recorded
- [ ] No known crash bugs

**See:** `MVP_ROADMAP.md` for complete checklist

---

## 🚀 Let's Build!

**Remember:**
- This is a learning project (developer's first major app!)
- Quality > speed
- Clean architecture pays off long-term
- Linux port is real, not hypothetical

**Current priority:** Complete refactoring, then effects, then MVP release!

---

**Version:** 1.1 (Updated for refactoring context)
**Last Updated:** 2025-01-02
