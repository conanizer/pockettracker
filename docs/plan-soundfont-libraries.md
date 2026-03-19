# Soundfont Libraries Implementation Plan

**Status:** Post-MVP, planning phase only — no code written yet.

---

## Context

PocketTracker currently supports only one instrument type: a WAV sample-based player. This plan adds SoundFont (SF2/SF3) support as a new instrument type, allowing General MIDI-style synthesis alongside custom samples. The architecture must be Android + Linux portable (matching the existing interface-based design).

---

## Library Choice: TinySoundFont (tsf)

**Recommended:** [TinySoundFont](https://github.com/schellingb/TinySoundFont) by Bernhard Schelling

| Criterion | tsf | FluidSynth |
|-----------|-----|-----------|
| License | ✅ MIT | ⚠️ LGPL |
| Integration | ✅ Single header file | ❌ External deps (glib) |
| Android/Linux | ✅ Zero changes needed | ✅ But heavier |
| 512MB RAM target | ✅ Minimal footprint | ❌ Marginal |
| SF2 + SF3 support | ✅ Both | ❌ SF2 only |
| Real-time audio | ✅ Designed for it | ⚠️ Blocking calls |
| Sample-accurate | ⚠️ Buffer-accurate | ❌ Not designed for it |

**Decision:** tsf wins on every criterion that matters. Single `tsf.h` drop-in, MIT license, works identically on Android and Linux. SF3 support is a bonus (10x smaller files via OGG compression).

**Download:** `curl -fsSL https://raw.githubusercontent.com/schellingb/TinySoundFont/master/tsf.h -o app/src/main/cpp/tsf.h`

---

## Architecture Overview

```
[Kotlin: InstrumentController]
    │  loadSoundfont(path, bank, preset)
    ▼
[JNI Bridge: OboeAudioBackend]
    │  native_loadSoundfont(instrumentId, path)
    │  native_setSoundfontPreset(sfSlot, bank, preset)
    │  native_scheduleSoundfontNote(frame, trackId, sfSlot, midiNote, vol, pan)
    ▼
[C++: native-audio.cpp + tsf.h]
    ├── SoundfontEntry[MAX_SOUNDFONTS=4] (per loaded SF2 file)
    │       tsf* handle
    │       std::mutex (protects note_on and render calls)
    └── Audio callback:
            if instrument is SAMPLER → existing Voice path (unchanged)
            if instrument is SOUNDFONT → tsf_render_float() path
```

---

## Phase 1: Data Model Changes

**File:** `core/data/TrackerData.kt`

### 1a. Add InstrumentType enum

```kotlin
enum class InstrumentType { SAMPLER, SOUNDFONT }
```

### 1b. Extend Instrument data class

Add new fields with defaults (preserves JSON deserialization of existing projects):

```kotlin
data class Instrument(
    // ... all existing fields unchanged ...

    // NEW: instrument type selector
    val instrumentType: InstrumentType = InstrumentType.SAMPLER,

    // NEW: soundfont-specific fields (only used when type == SOUNDFONT)
    val soundfontPath: String? = null,   // Absolute path to .sf2 or .sf3 file
    val sfBank: Int = 0,                 // Bank number (0-127)
    val sfPreset: Int = 0,               // Program/preset number (0-127)
)
```

Sampler-specific fields (sampleId, root, detune, drive, crush, etc.) remain in the class — they are simply ignored when `instrumentType == SOUNDFONT`. This avoids a sealed class migration and keeps JSON serialization straightforward.

---

## Phase 2: C++ Audio Engine

**Files to change:**
- `app/src/main/cpp/tsf.h` ← download from GitHub (single header)
- `app/src/main/cpp/native-audio.cpp` ← primary changes
- `app/src/main/cpp/CMakeLists.txt` ← minor additions

### 2a. CMakeLists.txt

Add tsf.h include path (header-only, no new source file) and libm for math:

```cmake
target_include_directories(pockettracker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pockettracker PRIVATE oboe::oboe android log m)
```

### 2b. SoundfontEntry struct (add to native-audio.cpp)

```cpp
#define TSF_IMPLEMENTATION
#include "tsf.h"

struct SoundfontEntry {
    tsf* handle = nullptr;
    std::mutex mutex;       // Protects note_on/off and render calls
    int instrumentId = -1;  // Which Instrument slot owns this
    std::string filePath;
};

static const int MAX_SOUNDFONTS = 4;  // LRU eviction if exceeded
static SoundfontEntry soundfonts[MAX_SOUNDFONTS];
```

### 2c. ScheduledNote extension

Add to existing `ScheduledNote` struct:

```cpp
struct ScheduledNote {
    // ... existing fields unchanged ...
    bool isSoundfont = false;  // NEW
    int sfSlot = -1;           // NEW: which SoundfontEntry to trigger
    int midiNote = 60;         // NEW: MIDI note 0-127
    int midiVelocity = 100;    // NEW
};
```

### 2d. Audio callback integration

After mixing all sampler voices into outputBuffer, mix soundfont renders on top:

```cpp
// After existing sampler voice mixing...

float tempBuffer[BUFFER_SIZE * 2];  // Stereo interleaved
for (int sf = 0; sf < MAX_SOUNDFONTS; sf++) {
    if (soundfonts[sf].handle == nullptr) continue;
    std::lock_guard<std::mutex> lock(soundfonts[sf].mutex);
    tsf_set_output(soundfonts[sf].handle, TSF_STEREO_INTERLEAVED, sampleRate, 0.0f);
    tsf_render_float(soundfonts[sf].handle, tempBuffer, numFrames, 0);
    // Apply track volume + pan + master, then mix into main outputBuffer
}
```

### 2e. Note trigger logic

When processing a scheduled soundfont note:

```cpp
// Note on:
SoundfontEntry* sf = &soundfonts[note.sfSlot];
if (sf->handle) {
    std::lock_guard<std::mutex> lock(sf->mutex);
    tsf_channel_set_presetnumber(sf->handle, note.trackId, note.sfPreset, false);
    tsf_channel_note_on(sf->handle, note.trackId, note.midiNote, note.midiVelocity / 127.0f);
}

// Note off (KILL effect or step end):
tsf_channel_note_off(sf->handle, note.trackId, note.midiNote);
```

### 2f. New JNI functions

```cpp
// Load SF2/SF3 from path, assign to a slot (returns slot index, -1 on error)
Java_..._native_1loadSoundfont(JNIEnv*, jobject, jint instrumentId, jstring path)

// Set active preset/bank for a slot
Java_..._native_1setSoundfontPreset(JNIEnv*, jobject, jint sfSlot, jint bank, jint preset)

// Schedule a soundfont note (adds to existing priority queue with isSoundfont=true)
Java_..._native_1scheduleSoundfontNote(JNIEnv*, jobject,
    jlong frame, jint trackId, jint sfSlot, jint midiNote, jint velocity, jfloat vol, jfloat pan)

// Free a slot's memory
Java_..._native_1unloadSoundfont(JNIEnv*, jobject, jint sfSlot)

// Query preset name string for UI display
Java_..._native_1getSoundfontPresetName(JNIEnv*, jobject, jint sfSlot, jint bank, jint preset)
```

---

## Phase 3: IAudioBackend Interface

**File:** `core/audio/IAudioBackend.kt`

Add soundfont methods:

```kotlin
interface IAudioBackend {
    // ... all existing methods unchanged ...

    // Soundfont support
    fun loadSoundfont(instrumentId: Int, filePath: String): Int  // returns sfSlot, -1 = error
    fun setSoundfontPreset(sfSlot: Int, bank: Int, preset: Int)
    fun scheduleSoundfontNote(frame: Long, trackId: Int, sfSlot: Int,
                              midiNote: Int, velocity: Int, vol: Float, pan: Float)
    fun unloadSoundfont(sfSlot: Int)
    fun getSoundfontPresetName(sfSlot: Int, bank: Int, preset: Int): String
}
```

**File:** `platform/android/OboeAudioBackend.kt` — implement all above by calling new JNI functions.

---

## Phase 4: Kotlin Logic Layer

### 4a. ISoundfontLoader interface (new file for Linux portability)

**New file:** `core/audio/ISoundfontLoader.kt`

```kotlin
interface ISoundfontLoader {
    fun loadFromFile(path: String): ByteArray?
    fun getDefaultSoundfontPath(): String?  // Linux: /usr/share/sounds/sf2/
}
```

### 4b. InstrumentController extensions

**File:** `core/logic/InstrumentController.kt`

Add:
- `loadSoundfont(project, filePath)` — reads path, finds/assigns sfSlot, calls `audio.loadSoundfont()`
- `updateSoundfontPreset(project, bank, preset)` — updates instrument fields + notifies C++
- `previewSoundfontNote(midiNote)` — plays a preview note from current soundfont
- `unloadSoundfontForInstrument(instrumentId)` — frees memory slot when instrument changes
- Internal `sfSlotMap: MutableMap<String, Int>` — file path → cache slot mapping

### 4c. PlaybackController extensions

**File:** `core/logic/PlaybackController.kt`

When scheduling a step note, check `instrument.instrumentType`:

```kotlin
when (instrument.instrumentType) {
    InstrumentType.SAMPLER -> {
        // Existing scheduleNote() call — unchanged
        audio.scheduleNote(frame, instrument.sampleId, ...)
    }
    InstrumentType.SOUNDFONT -> {
        val midiNote = (note.octave * 12) + note.pitch  // C-4 = 60
        val sfSlot = instrumentController.sfSlotMap[instrument.soundfontPath] ?: return
        val velocity = (phraseVolume * 127 / 255).coerceIn(1, 127)
        audio.scheduleSoundfontNote(frame, trackId, sfSlot, midiNote, velocity, vol, pan)
    }
}
```

---

## Phase 5: UI Changes

**File:** `core/rendering/InstrumentModule.kt`

### 5a. TYPE row becomes editable

Currently `TYPE` shows read-only `sample`. Make it editable:
- A+UP/DOWN cycles: `sample` ↔ `soundfont`
- Triggers `instrumentController.setInstrumentType(project, newType)`

### 5b. Conditional parameter layout

When `instrumentType == SOUNDFONT`, replace sampler-specific rows with:

```
TYPE      soundfont    [LOAD SF2]
FILE      FluidR3.sf3                   (truncated filename, tap to browse)
BANK      000          PRESET  000
NAME      Acoustic Grand Piano          (name from tsf, read-only)
ROOT      C-4          VOL     FF       (pitch transpose + volume still apply)
PAN       80           FILTER  off      (pan + filter still apply)
```

When `instrumentType == SAMPLER`, existing layout is unchanged.

### 5c. LOAD SF2 action

On the TYPE row, a secondary label `[LOAD SF2]` or `[BROWSE]` opens `FileBrowserModule` filtered to `.sf2` and `.sf3` extensions. Default start path: `Documents/PocketTracker/Soundfonts/`.

On file selection → `instrumentController.loadSoundfont(project, selectedPath)`.

### 5d. Preset navigation

On PRESET row: A+UP/DOWN cycles 0-127. After each change:
1. Update `instrument.sfPreset` in project state
2. Call `audio.setSoundfontPreset(sfSlot, bank, preset)`
3. Display name string from `audio.getSoundfontPresetName(sfSlot, bank, preset)` on NAME row

---

## Files to Create or Modify

| File | Change |
|------|--------|
| `core/data/TrackerData.kt` | Add `InstrumentType` enum + 3 fields to `Instrument` |
| `core/audio/IAudioBackend.kt` | Add 5 soundfont interface methods |
| `core/audio/ISoundfontLoader.kt` | **New file** — platform-agnostic loader interface |
| `core/logic/InstrumentController.kt` | Add soundfont load/update/preview methods |
| `core/logic/PlaybackController.kt` | Add SOUNDFONT branch in note scheduling |
| `platform/android/OboeAudioBackend.kt` | Implement 5 new JNI soundfont calls |
| `app/src/main/cpp/native-audio.cpp` | Add SoundfontEntry, tsf rendering, 5 JNI functions |
| `app/src/main/cpp/tsf.h` | **New file** — download from GitHub |
| `app/src/main/cpp/CMakeLists.txt` | Add include dir + libm |
| `core/rendering/InstrumentModule.kt` | Conditional UI layout for SOUNDFONT type |

---

## Implementation Milestones

### Milestone 1: Foundation (1 week)
1. Download `tsf.h` to cpp directory
2. Extend `Instrument` data class (`instrumentType`, `soundfontPath`, `sfBank`, `sfPreset`)
3. Add `IAudioBackend` interface methods
4. Implement `loadSoundfont`, `setSoundfontPreset`, `scheduleSoundfontNote` in C++ and JNI
5. Test: trigger SF2 notes from hardcoded path, verify audio output

### Milestone 2: Kotlin Integration (1 week)
1. Implement all soundfont methods in `OboeAudioBackend`
2. Extend `InstrumentController` (load, update, preview, sfSlotMap)
3. Extend `PlaybackController` (SOUNDFONT scheduling branch)
4. Create `ISoundfontLoader` interface

### Milestone 3: UI (1 week)
1. TYPE row editable in InstrumentModule
2. Conditional parameter layout for SOUNDFONT
3. File browser wired up for `.sf2`/`.sf3` selection
4. Preset navigation with name display from tsf

### Milestone 4: Polish (3-5 days)
1. LRU eviction when > 4 soundfonts in memory
2. Error handling: file not found, corrupt SF2, unsupported version
3. KILL effect (K00) working for soundfont notes
4. Volume/pan chain applied to soundfont output correctly
5. Test on Miyoo Flip (1GB RAM)

---

## Linux Portability Notes

- `tsf.h` uses only standard C (`fopen`, `malloc`, `math.h`) — zero platform-specific code
- On Linux: same `tsf.h`, new audio backend (ALSA/PulseAudio instead of Oboe)
- `ISoundfontLoader.getDefaultSoundfontPath()` returns `/usr/share/sounds/sf2/` on Linux
- No additional porting work needed for tsf itself

---

## Verification Checklist

- [ ] Build: CMake builds with `tsf.h`, no link errors
- [ ] Load: Copy a free GM soundfont (e.g., GeneralUser GS) to `Documents/PocketTracker/Soundfonts/`
- [ ] Playback: Phrase with C-4, E-4, G-4, preset 000 → hear piano
- [ ] KILL: K00 on soundfont step stops note immediately
- [ ] Mixed: Track 1 = sampler (kick), Track 2 = soundfont (piano) → both play simultaneously
- [ ] Preset change: PRESET 025 → Acoustic Guitar → playback uses new preset
- [ ] Memory: Load 5 different SF2s → oldest evicted, stays within 4 slots
- [ ] Serialization: Save project with SOUNDFONT instrument, reload → path+preset preserved
- [ ] No regression: Existing sampler instruments unaffected
