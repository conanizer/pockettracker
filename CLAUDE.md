# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

## 🚨 BEFORE STARTING ANY TASK

**ALWAYS read these files first:**

1. **docs/development-status.md** - What's done, what's remaining
2. **docs/technical-architecture.md** - System design, file structure, rendering & overlay system


**Current Phase:** Testing & Polish

---

## 📋 Quick Reference

### Current Project State (June 2026)

**Status:** All Extension Packs COMPLETE. Testing & Polish phase. Target release: TBD (original April 2026 target missed — see docs/development-status.md).

**Completed:**
- ✅ Audio engine (professional-grade, sample-accurate)
- ✅ All UI screens working
- ✅ Playback system complete
- ✅ Architecture refactoring COMPLETE
- ✅ Effects system (21 effects including pitch, table, groove, modulation)
- ✅ Copy/paste system (M8-style selection)
- ✅ MVP Expansion #1: Mixer, VOL/PAN, Stereo, WAV Export
- ✅ Extension Pack 2: Tables, HOP/TIC, Pitch Effects
- ✅ Extension Pack 3: Groove, Modulation, Resampling, Layout system
- ✅ SoundFont (SF2) instruments via TinySoundFont
- ✅ Audio init off main thread (no startup freeze)
- ✅ OpenSL ES stream priority (reduced log spam)

**Next Steps:**
1. ~~All above~~ ✅ DONE
2. **Testing & polish** ← CURRENT
3. Documentation & demo video
4. MVP release (TBD)

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

**See:** `docs/technical-architecture.md` for complete architecture

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
- **Secondary:** Ayaneo Pocket Air Mini (3GB RAM, Android 11)

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

### ⚠️ DO NOT RUN BUILDS AUTOMATICALLY

**IMPORTANT FOR CLAUDE CODE:**
- **DO NOT** run `./gradlew build` or similar commands automatically
- The developer will build and test manually
- Running builds wastes tokens and time
- Just write the code and let the developer test it
- If asked to verify compilation, ask the developer to do it instead

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
- `Note`: MIDI-based note representation (editable range C-0 to G-9), converts to frequency
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
- **Screen overlay**: PNG files in `assets/overlays/` drawn on top of the tracker screen via `Modifier.drawWithContent` + `DrawScope.drawImage(alpha = STR/255f)` in `TrackerScreen` (`ScreenLayouts.kt`). Selected and configured in Settings row 2 (OVERLAY / STR).

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
- `InstrumentPoolModule`: Overview list of all 128 instrument slots with a per-slot mixer strip (NAME + V/RV/DE/EQ); load/clear/preview/edit. Reached above INSTRUMENT (INST_POOL screen).
- `SampleEditorModule`: Full-screen waveform editor (opened from Instrument screen)
- `MixerModule`: 8 tracks + master with dBFS meters
- `EffectModule`: Global send effects config (reverb/delay/EQ)
- `EqModule`: 3-band parametric EQ editor overlay
- `TableModule`: 16-row mini-sequencer per instrument
- `GrooveModule`: Step-timing patterns (swing/shuffle)
- `ModulationModule`: 4-slot envelope/LFO editor per instrument
- `SettingsModule`: Layout, scaling, screen overlay, button sound/vibration, cursor settings (opened from Project screen). 12 rows (0-11): LAYOUT, SCALING, OVERLAY (+ STR column), BTN SOUND (+ VOL column), BTN VIBRO (+ POW column), KB INSERT, CURSOR, NOTE PREV, VISUALIZER, THEME, TEMPLATE, RESUME.
- `ProjectModule`: Project settings (tempo, name, save/load)
- `NavigationMapModule`: 80×105px navigation grid display
- `FileBrowserModule`: File/folder navigation

Each module receives state objects and renders itself independently.

### Audio Engine: Sample-Accurate Queue System

**Architecture:** Professional-grade audio engine with sample-accurate note scheduling, matching M8/LGPT/Picotracker timing precision (<0.02ms jitter).

**Kotlin Layer (`AudioEngine.kt` + `OboeAudioBackend.kt`):**
- `OboeAudioBackend` implements `IAudioBackend` via JNI (platform/android/)
- `AudioEngine` (core/audio/) is platform-agnostic: high-level functions, note scheduling, 2-phrase lookahead
- High-level functions: `playPhrase()`, `playChain()`, `playSong()`
- Note scheduling with sample-accurate frame timing

**C++ Layer (portable `AudioEngine` core in `audio-engine.cpp` + Android `OboeAudioEngine` backend in `oboe-audio-engine.cpp`):**
- Oboe-based audio stream (44.1kHz, OpenSL ES preferred, AAudio fallback) — owned by `OboeAudioEngine`, the **only** Oboe-coupled TU
- Sample-accurate priority queue (`std::priority_queue<ScheduledNote>`)
- 8-voice polyphony with per-track voice stealing
- Linear interpolation for pitch-shifting (professional quality)
- Sample playback: start/end points, reverse, loop modes (forward/ping-pong)
- True stereo playback (`samplesRight[]` buffer for stereo WAV files)
- Resonant SVF filters (LP/HP/BP via DaisySP) + biquad EQ
- Master bus: OTT 3-band compressor + DaisySP soft limiter
- Stereo send buses: DaisySP ReverbSc + ping-pong delay
- Real-time waveform capture for oscilloscope
- **Engine/Oboe split (REVIEW-4 4.5):** `AudioEngine` (`audio-engine.{h,cpp}`) is platform-agnostic — voices, scheduling, all DSP (`processAudioBlock`), no Oboe/Android deps. `OboeAudioEngine` (`oboe-audio-engine.{h,cpp}`) owns the stream and forwards `onAudioReady → core.processLiveBlock(...)`. A Linux port adds a sibling backend (e.g. `AlsaAudioEngine`) driving the same core. Names mirror the Kotlin layer (`AudioEngine` = portable, `Oboe*` = Android).
- `native-audio.cpp` is a 15-line stub redirect — all DSP/scheduling lives in `audio-engine.cpp`

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
- Loaded via `AndroidFileSystem` (implements `IFileSystem`)
- True stereo WAV playback; SOURCE setting selects LEFT/RIGHT/STEREO/MONO non-destructively
- Automatic sample rate detection and compensation
- 128 instrument slots (0x00–0x7F, all identical, all user-loadable, no bundled defaults — all start empty)

**Current Implementation:**
- `FileController.kt` + `IFileSystem` / `AndroidFileSystem` — portable save/load
- `FileManager.kt` — legacy Android-specific save/load (kept for compatibility)

---

## 🎵 MVP Remaining Work

### ✅ COMPLETED: Effects System

**ALL TOP-5 Effects working in PHRASE screen:**
- ✅ Arpeggio (Axx) - Note pattern automation with ARC config (Cxx)
- ✅ Offset (Oxx) - Sample start point automation
- ✅ Volume (Vxx) - Volume automation within step
- ✅ Kill (K00) - Stop sample immediately
- ✅ Repeat (Rxx) - Full LGPT/M8-style retrigger with persistence

### ✅ COMPLETED: Copy/Paste System

**M8-style workflow fully implemented:**
- ✅ Selection mode: L+B to enter/cycle (CELL → ROW → SCREEN)
- ✅ Copy: B in selection mode
- ✅ Cut: L+A in selection
- ✅ Paste: L+A outside selection
- ✅ Delete: A+B in selection

### ✅ COMPLETE: MVP Expansion

**See:** `docs/development-status.md` for full details

- [x] Instrument VOL/PAN editable in UI
- [x] True stereo pan (constant-power law) in C++ audio engine
- [x] Volume chain: instrument × phrase × track × master
- [x] Mixer screen with 8 tracks + master, dBFS meters
- [x] Stereo send buses: reverb (DaisySP ReverbSc) + delay (ping-pong)
- [x] WAV Export (`RenderController.kt` offline render, `WavWriter.kt`)

### 5. Testing & Polish (1 week)

- Usability testing ("hello world" in <5 min)
- Bug hunting on Miyoo Flip and Ayaneo
- Performance verification
- Example project creation

### 6. Documentation (3-5 days)

- README finalization
- Controls guide (including copy/paste and mixer)
- Short demo video
- Known issues list

---

## 🧭 Navigation Between Screens

**5×5 Navigation Grid (dynamic — content depends on current screen column):**

```
      0         1         2         3         4
┌─────────────────────────────────────────────────┐
│ ----      ----      SCALE   INST_POOL  (INST)* │ 0  (only on PHRASE / INSTRUMENT)
│                                                  │
│ PROJ      PROJ     GROOVE     MODS      ----   │ 1  (GROOVE/MODS only on their column)
│                                                  │
│ SONG     CHAIN     PHRASE     INST     TABLE   │ 2  ← Main editing row (always visible)
│                                                  │
│ ----      ----     MIXER      ----      ----   │ 3  (current column only)
│                                                  │
│ ----      ----    EFFECTS     ----      ----   │ 4  (current column only)
└─────────────────────────────────────────────────┘
```

**Navigation:**
- R + DPAD: Move between screens
- Release R: Jump to selected screen

**Instrument Pool fast-jump:** `(INST)*` at row 0 / col 4 is contextual — shown only on INST_POOL (or
the instrument reached from it). From INST_POOL: R+RIGHT → INSTRUMENT, R+LEFT → PHRASE, R+DOWN → MODS.
From that row-0 instrument: R+LEFT → INST_POOL, R+DOWN → MODS, R+UP/R+RIGHT stay. The normal row-2
INSTRUMENT is unchanged. Driven by `TrackerController.instrumentFromPool` (set on the R+RIGHT jump,
cleared in the `currentScreen` setter when leaving INSTRUMENT).

**Popup screens (not in nav grid — opened contextually):**
- SETTINGS: opened from PROJECT screen
- SAMPLE_EDITOR: opened from INSTRUMENT screen

**Implemented Screens:**
- Row 0: INST_POOL ✅ (above INSTRUMENT)
- Row 1: PROJECT ✅
- Row 2: SONG ✅, CHAIN ✅, PHRASE ✅, INSTRUMENT ✅, TABLE ✅
- Row 3: MIXER ✅
- Row 4: EFFECTS ✅
- Popups: SETTINGS ✅, SAMPLE_EDITOR ✅

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
- L + B: Enter/cycle selection mode (CELL → ROW → SCREEN)
- L + A (in selection): Cut selection
- L + A (outside selection): Paste clipboard
- A + B: Delete/clear selection or value
- R + DPAD: Screen navigation

**Virtual Controls:**
- Automatically enabled on touchscreen-only devices
- Disabled on devices with physical buttons (via DeviceAdapter)

**See:** `docs/input-system.md` for complete reference

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
1. docs/development-status.md - What's done
2. Current task document (relevant plan under docs/internal/, if any)
3. docs/technical-architecture.md - How it should work
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

| Document                      | Purpose                     | When to Read |
|-------------------------------|-----------------------------|--------------|
| **docs/development-status.md**     | Current progress            | Every session start |
| **docs/technical-architecture.md** | System design               | Understanding architecture |
| **docs/input-system.md**           | Controls reference          | Adding input handling |


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

## ✅ Definition of Done (Expanded MVP)

**Original MVP (COMPLETE):**
- [x] All refactoring phases complete (portable architecture)
- [x] TOP-5 effects working in phrase screen
- [x] Copy/paste working (M8-style)

**MVP Expansion (IN PROGRESS):**
- [x] Instrument VOL/PAN editable in UI
- [x] Stereo pan working in audio engine
- [x] Volume chain: instrument × phrase × track × master
- [x] Mixer screen with 8 tracks + master
- [x] Peak meters updating during playback
- [x] WAV Export ("WAV MIX" button renders song)

**Final Steps:**
- [ ] Tested on Miyoo Flip and Ayaneo
- [ ] README complete with installation guide
- [ ] Demo video recorded
- [ ] No known crash bugs

**See:** `docs/development-status.md` for detailed checklist

---

## 🚀 Let's Build!

**Remember:**
- This is a learning project (developer's first major app!)
- Quality > speed
- Clean architecture pays off long-term
- Linux port is real, not hypothetical

**Current priority:** Complete refactoring, then effects, then MVP release!

---

**Version:** 1.3 (Fact-checked against codebase)
**Last Updated:** 2026-06-22
