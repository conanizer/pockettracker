# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

PocketTracker is an Android music tracker application inspired by M8 and LSDJ, designed for gaming handhelds and budget Android devices with a target resolution of 640×480.

**Key Technologies:**
- Kotlin with Jetpack Compose for UI
- Native C++ audio engine using Oboe library
- JNI bridge between Kotlin and C++
- Kotlinx serialization for project save/load
- Custom pixel-perfect rendering system

**Minimum Requirements:**
- Android 8.0 (API 26)
- Target architectures: arm64-v8a, x86_64 only (64-bit)

## Build Commands

```bash
# Build the project (use Android Studio or Gradle)
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

## Architecture

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
- `Instrument`: Sample ID + volume + pan settings
- `Project`: Root container with all data, serializable to JSON

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
- `OscilloscopeModule`: 620×70px waveform display
- `PhraseEditorModule`: 620×392px phrase editing interface
- `ChainEditorModule`: Chain editing screen
- `SongEditorModule`: Song arrangement screen
- `ProjectModule`: Project settings (tempo, name, save/load)
- `NavigationMapModule`: 80×105px navigation grid display

Each module receives state objects and renders itself independently.

### Audio Engine: JNI Bridge to C++

**Kotlin Layer (TrackerAudioEngine.kt):**
- Loads 4 WAV samples from resources (kick, snare, hihat, bass)
- Converts WAV to float arrays, passes to native code
- Calculates note frequencies from MIDI values
- Manages waveform buffer for visualization (620 samples)

**Native Layer (native-audio.cpp):**
- Oboe-based real-time audio engine
- 8-voice polyphony with per-track voice stealing
- Sample playback with pitch shifting (frequency/baseFrequency ratio)
- Stereo mixing in audio callback

**JNI Methods:**
- `native_create()`: Initialize Oboe stream
- `native_loadSample(id, floatArray)`: Load sample into slot 0-3
- `native_triggerNote(sampleId, trackId, freq, baseFreq, volume)`: Play note
- `native_stopAll()`: Kill all active voices

### Navigation System: 5×5 Screen Grid

Screens are organized in a 5-column × 5-row grid:

```
Row 0:         -      SCALE   INST_POOL    -
Row 1:     PROJECT   GROOVE     MODS     PROJECT
Row 2:      SONG     CHAIN    PHRASE   INSTRUMENT  TABLE
Row 3:     MIXER     MIXER    MIXER      MIXER     MIXER
Row 4:    EFFECTS   EFFECTS  EFFECTS    EFFECTS   EFFECTS
```

**Navigation Rules:**
- SHIFT + D-PAD: Navigate between screens
- Columns 0-4 have distinct screens in Row 2 (main row)
- PROJECT, MIXER, EFFECTS are "shared rooms" spanning multiple columns
- `previousColumn` tracks which column you came from when in shared rooms

**Key Constants:**
- `MAIN_ROW_SCREENS`: Song, Chain, Phrase, Instrument, Table
- Helper functions in MainActivity.kt: `navigateUp/Down/Left/Right`

### Device Adaptation Layer

**DeviceAdapter.kt** detects device type and calculates optimal layout:

**Detection Logic:**
- Checks for physical D-pad/gamepad via `InputDevice` API
- Measures screen resolution and aspect ratio
- Determines if virtual buttons are needed

**Layout Configurations:**
1. **Gaming Handheld** (physical buttons): Full-screen 640×480 renderer
2. **Touchscreen Landscape**: Renderer + left/right virtual button columns
3. **Touchscreen Portrait**: Renderer + bottom virtual button grid

**Virtual Controls (VirtualControls.kt):**
- D-pad (Up/Down/Left/Right)
- Face buttons (A, B, Select, Start)
- Shoulder buttons (L as SHIFT, R for quick navigation)

### File Management

**FileManager.kt:**
- Saves/loads projects as JSON files
- Default extension: `.ptp` (PocketTracker Project)
- Storage location: `{app_files_dir}/Songs/`
- Uses Kotlinx serialization for Project serialization

**Important:** All TrackerData classes are marked `@Serializable` and handle custom equals/hashCode for arrays.

## Common Development Tasks

### Adding a New Screen Module

1. Create new file implementing `TrackerModule` interface
2. Define state data class for the module
3. Implement `draw()` function using `DrawScope`
4. Register in `TrackerLayout.drawLayout()` with position
5. Add screen type to `ScreenType` enum
6. Update navigation logic in `MainActivity.kt`

### Modifying the Audio Engine

**Kotlin changes:**
- Edit `TrackerAudioEngine.kt` for JNI interface changes
- Update native method declarations

**C++ changes:**
- Edit `native-audio.cpp` for audio processing logic
- Rebuild native library: `./gradlew build`
- JNI method signatures must match exactly

### Adding Audio Samples

1. Place WAV file in `app/src/main/res/raw/`
2. Add resource ID to `TrackerAudioEngine.loadAllSamples()`
3. Increase sample array size in both Kotlin and C++ if needed
4. Set base frequency for the sample (default: C-4 = 261.63 Hz)

### Testing on Different Devices

**Emulator testing:**
- Use AVD with 640×480 resolution for gaming handheld simulation
- Test touch controls on standard phone resolution

**Physical device testing:**
- Gaming handhelds: Anbernic RG353V, Retroid Pocket, etc.
- Regular phones: Should show virtual controls

**Debugging:**
- Check Logcat for `DeviceAdapter` output to see detection results
- Look for `TrackerAudioEngine` and `NativeAudio` logs

## Code Style Notes

- Extensive comments in MainActivity.kt explain architecture for newcomers
- Module files use section separators with `===` for visual organization
- Logging uses descriptive tags and emoji for readability (✅ ❌ 📁)
- Navigation logic is centralized in helper functions, not scattered

## Key Design Decisions

1. **Pixel-perfect rendering**: All modules work in design pixels (640×480), scaled at runtime
2. **Modular screens**: Each screen is a self-contained module with its own state
3. **64-bit only**: Oboe prefab package works best with 64-bit architectures
4. **Integer scaling**: Maintains crisp pixel art aesthetic, letterbox for non-matching aspect ratios
5. **Native audio**: Low-latency requirements necessitate C++ audio engine
6. **State-driven UI**: Compose state variables trigger automatic redraws

## Important Files

- **TrackerData.kt**: All data structures (read this first to understand the data model)
- **MainActivity.kt**: Main app logic, navigation, button handlers (heavily documented)
- **PixelPerfectRenderer.kt**: Rendering system and module layout
- **TrackerAudioEngine.kt**: JNI bridge to audio engine
- **native-audio.cpp**: Real-time audio processing with Oboe
- **DeviceAdapter.kt**: Device detection and layout calculation
- **FileManager.kt**: Project save/load system

## Current Development Phase

Phase A: UI completion without physical hardware
- Mouse/touch navigation working
- File management system functional
- All screen modules being implemented progressively
- Song → Chain → Phrase data flow connected

Refer to DEVELOPMENT_STATUS.md for what's currently being worked on and what's next.
