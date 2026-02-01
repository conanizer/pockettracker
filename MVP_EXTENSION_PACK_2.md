# MVP Extension Pack 2: Tables, HOP/TIC, and Pitch Effects

## Document Purpose
Implementation plan for extending PocketTracker with Table screen, sequencer control effects (HOP/TIC), pitch automation effects, and bug fixes.

**Created:** 2026-02-01
**Target Completion:** ~4 weeks (1 month)

---

## Overview

### Features to Add (Priority Order)

| # | Feature | Complexity | Days |
|---|---------|------------|------|
| 1 | Bug Fixes & Polish | Low | 2-3 |
| 2 | Table Screen UI | Medium | 3-4 |
| 3 | Table Data Model | Low | 1 |
| 4 | TIC Effect (Table Tick) | Medium | 2-3 |
| 5 | HOP Effect (Jump) | Medium | 2-3 |
| 6 | Real-time Parameter System | High | 3-4 |
| 7 | Pitch Slide (PSL) | Medium | 2 |
| 8 | Pitch Bend (PBN) | Medium | 2 |
| 9 | Vibrato (PVB/PVX) | Medium | 2-3 |
| 10 | Integration Testing | Medium | 3-4 |

**Total Estimated:** ~22-28 days (with buffer)

### Architecture Principle
All new features follow existing portable architecture:
- Business logic in `core/` (no Android imports)
- Audio processing in C++ (native-audio.cpp)
- Platform-specific code only in `platform/android/`

---

## Phase 1: Bug Fixes & Polish (Days 1-3)

### 1.1 Fix Mixer/Oscilloscope Decay When Stopped

**Problem:** Peak meters and oscilloscope freeze when playback stops because decay only happens in audio callback.

**Current Behavior:**
```cpp
// native-audio.cpp - only runs during playback
void onAudioReady(...) {
    trackPeaks[t] *= PEAK_DECAY;  // 0.95 decay
    // ... only called when audio is playing
}
```

**Solution:** C++ decay function callable from Kotlin (portable - no UI code duplication for Linux)

**File:** `app/src/main/cpp/native-audio.cpp`

```cpp
// Add decay function that can be called even when audio stream is not running
extern "C" JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_decayPeaks(
    JNIEnv *env, jobject thiz
) {
    const float DECAY = 0.92f;  // Slightly faster for visual feedback

    for (int t = 0; t < 8; t++) {
        trackPeaks[t] *= DECAY;
        if (trackPeaks[t] < 0.001f) trackPeaks[t] = 0.0f;
    }
    masterPeakL *= DECAY;
    masterPeakR *= DECAY;
    if (masterPeakL < 0.001f) masterPeakL = 0.0f;
    if (masterPeakR < 0.001f) masterPeakR = 0.0f;

    // Also decay oscilloscope buffer (fade to center line)
    for (int i = 0; i < waveformBufferSize; i++) {
        waveformBuffer[i] *= DECAY;
    }
}
```

**Kotlin side:** Call `decayPeaks()` periodically when not playing

**File:** `app/src/main/java/com/example/pockettracker/core/audio/IAudioBackend.kt`

```kotlin
interface IAudioBackend {
    // ... existing
    fun decayPeaks()  // Call when not playing to fade meters/oscilloscope
}
```

**File:** `app/src/main/java/com/example/pockettracker/core/logic/PlaybackController.kt`

```kotlin
// In PlaybackController, start decay loop when stopped
fun stopPlayback() {
    isPlaying = false
    audioBackend.stopPlayback()

    // Start decay coroutine
    decayJob = scope.launch {
        while (!isPlaying) {
            audioBackend.decayPeaks()
            delay(16)  // ~60fps
        }
    }
}
```

### 1.2 Fix Track Volume Not Affecting Playback Immediately

**Problem:** Changing track volume in mixer doesn't affect already-scheduled notes.

**Root Cause:** PlaybackController schedules 2+ phrases ahead with pre-calculated volumes.

**Solution:** Real-time volume in C++ (enables all future real-time parameter changes)

**File:** `app/src/main/cpp/native-audio.cpp`

```cpp
// Live track volumes - can be changed at any time
float trackVolumes[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
float masterVolume = 1.0f;

// JNI function to update track volume in real-time
extern "C" JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_setTrackVolume(
    JNIEnv *env, jobject thiz,
    jint trackId,
    jfloat volume
) {
    if (trackId >= 0 && trackId < 8) {
        trackVolumes[trackId] = volume;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_setMasterVolume(
    JNIEnv *env, jobject thiz,
    jfloat volume
) {
    masterVolume = volume;
}

// In audio callback, multiply by live track volume
// (voice.volume is instrument×phrase, trackVolumes is track level, masterVolume is master)
float sample = voice.sample * voice.volume * trackVolumes[voice.trackId] * masterVolume;
```

**Kotlin interface update:**

**File:** `app/src/main/java/com/example/pockettracker/core/audio/IAudioBackend.kt`

```kotlin
interface IAudioBackend {
    // ... existing
    fun setTrackVolume(trackId: Int, volume: Float)
    fun setMasterVolume(volume: Float)
}
```

**TrackerController integration:**

```kotlin
// When user changes track volume in mixer
fun onTrackVolumeChanged(trackId: Int, volumeHex: Int) {
    project.tracks[trackId].volume = volumeHex
    audioBackend.setTrackVolume(trackId, VolumeUtils.hexToFloat(volumeHex))
}

// When user changes master volume
fun onMasterVolumeChanged(volumeHex: Int) {
    project.masterVolume = volumeHex
    audioBackend.setMasterVolume(VolumeUtils.hexToFloat(volumeHex))
}
```

**Benefits:**
- Volume changes are instant (no audio glitch)
- Foundation for all real-time parameter changes
- Portable (C++ code works on Linux too)

### 1.3 Remove Old Project Migration Script

**Problem:** Migration code converts old 0xFF phrase refs to -1. Projects already migrated.

**File:** `app/src/main/java/com/example/pockettracker/core/storage/FileManager.kt`

**Action:** Remove migration code block in `loadProject()`:
```kotlin
// DELETE THIS BLOCK:
// Migration: Convert old 0xFF empty phrase refs to -1
project.chains.forEach { chain ->
    for (i in chain.phraseRefs.indices) {
        if (chain.phraseRefs[i] == 0xFF) {
            chain.phraseRefs[i] = -1
        }
    }
}
```

### 1.4 Fix L Button Breaking L+A Cut Combo

**Problem:** L alone exits selection mode, so L+A tries to paste instead of cut.

**Current Flow:**
1. User in selection mode
2. User presses L (to start L+A combo)
3. L alone detected → exits selection mode
4. User presses A → paste happens (wrong!)

**Solution:** Change selection exit from L alone to L+R

**File:** `app/src/main/java/com/example/pockettracker/core/logic/InputController.kt`

```kotlin
fun handleSelectionModeInput(event: InputEvent): Boolean {
    when {
        // L+A = Cut (works now since L alone doesn't exit)
        event.isLHeld && event.button == Button.A -> {
            cutSelection()
            return true
        }

        // L+R = Exit selection mode (NEW - replaces L alone)
        event.isLHeld && event.isRHeld -> {
            exitSelectionMode()
            return true
        }

        // B = Copy and exit
        event.button == Button.B && !event.isAnyModifierHeld -> {
            copySelection()
            exitSelectionMode()
            return true
        }

        // A+B = Delete selection (no clipboard)
        event.isAHeld && event.button == Button.B -> {
            deleteSelection()
            exitSelectionMode()
            return true
        }

        // DPAD = Expand/contract selection
        event.direction != null && !event.isAHeld -> {
            expandSelection(event.direction)
            return true
        }
    }
    return false
}
```

**Updated Controls Summary:**
| Control | In Selection Mode | Outside Selection |
|---------|------------------|-------------------|
| L+B | Cycle selection (CELL→ROW→SCREEN) | Enter selection |
| B | Copy + exit | - |
| L+A | Cut + exit | Paste |
| A+B | Delete + exit | - |
| L+R | Exit (no copy) | - |
| DPAD | Expand selection | Move cursor |

### Definition of Done - Phase 1 ✅ COMPLETE (2026-02-01)
- [x] C++ `decayPeaks()` function added to native-audio.cpp
- [x] C++ `decayWaveform()` function added to native-audio.cpp
- [x] `IAudioBackend.decayPeaks()` and `decayWaveform()` methods added
- [x] Mixer meters decay smoothly when playback stops
- [x] Oscilloscope waveform decays when playback stops
- [x] C++ `trackVolumes[]` array for real-time track volume
- [x] C++ `setTrackVolume()` JNI function added
- [x] C++ `setMasterVolume()` JNI function added
- [x] Track volume changes affect playback immediately
- [x] Old migration code removed from FileManager.kt
- [x] L+R exits selection mode (instead of L alone)
- [x] L+A cut works correctly in selection mode
- [x] All changes tested on device ✅

**Phase 1.1 Fix (2026-02-01):** Volume double-application bug
- [x] Fixed: Changing mixer volume caused ~1 bar mute before working
- [x] Root cause: Track × master was baked into scheduled notes AND applied in C++
- [x] Solution: Added `VolumeUtils.calculateNoteVolume(inst, phrase)` for real-time playback
- [x] PlaybackController now only bakes inst × phrase; C++ applies track × master
- [x] Offline render (RenderController) unchanged - still bakes all 4 volumes

**Note:** Always test on real device after completing each phase before proceeding.

---

## Phase 2: Table Data Model (Day 4)

### 2.1 Add Table Data Structure

**File:** `app/src/main/java/com/example/pockettracker/core/data/TrackerData.kt`

```kotlin
/**
 * Table - A mini-sequencer that runs alongside an instrument
 *
 * Tables are powerful tools for:
 * - Arpeggios and chord patterns
 * - Volume slides and envelopes
 * - Pitch automation
 * - Multi-stage effects
 *
 * Each table has 16 rows with: transpose, volume, and 3 FX columns
 * Tables run at their own tick rate (set by TIC effect or instrument setting)
 */
@Serializable
data class Table(
    val id: Int,
    var name: String = "TBL${id.toString(16).padStart(2,'0').uppercase()}",
    val rows: MutableList<TableRow> = MutableList(16) { TableRow() }
)

@Serializable
data class TableRow(
    var transpose: Int = 0x00,      // Note transpose: 00-7F = +semitones, 80-FF = -semitones (signed)
    var volume: Int = 0xFF,         // Volume multiplier: 00-FF (FF = no change)
    var fx1Type: Int = 0x00,        // Effect 1 type
    var fx1Value: Int = 0x00,       // Effect 1 value
    var fx2Type: Int = 0x00,        // Effect 2 type
    var fx2Value: Int = 0x00,       // Effect 2 value
    var fx3Type: Int = 0x00,        // Effect 3 type
    var fx3Value: Int = 0x00        // Effect 3 value
) {
    companion object {
        fun empty() = TableRow()

        // Transpose is signed: 00=0, 01-7F=+1 to +127, 80-FF=-128 to -1
        fun transposeToSemitones(transpose: Int): Int {
            return if (transpose < 0x80) transpose else transpose - 256
        }
    }
}
```

### 2.2 Update Instrument to Reference Table

**File:** `app/src/main/java/com/example/pockettracker/core/data/TrackerData.kt`

```kotlin
@Serializable
data class Instrument(
    val id: Int,
    var name: String = "INST${id.toString(16).padStart(2,'0').uppercase()}",
    var sampleId: Int = -1,

    // Existing parameters...
    var volume: Int = 0xFF,
    var pan: Int = 0x80,
    var root: Note = Note.C4,
    var detune: Int = 0x80,
    // ...

    // NEW: Table parameters
    var tableId: Int = -1,          // -1 = no table, 0-255 = table ID
    var tableTicRate: Int = 0x06    // Default: 6 tics per row (2 rows per step)
)
```

### 2.3 Update Project to Include Tables

**File:** `app/src/main/java/com/example/pockettracker/core/data/TrackerData.kt`

```kotlin
@Serializable
data class Project(
    var name: String = "UNTITLED",
    var tempo: Int = 128,
    var transpose: Int = 0,
    var masterVolume: Int = 0xFF,

    // Existing...
    val phrases: MutableList<Phrase> = MutableList(256) { Phrase(it) },
    val chains: MutableList<Chain> = MutableList(256) { Chain(it) },
    val tracks: MutableList<Track> = MutableList(8) { Track(it) },
    val instruments: MutableList<Instrument> = MutableList(256) { Instrument(it) },

    // NEW: Tables (256 available, like phrases)
    val tables: MutableList<Table> = MutableList(256) { Table(it) }
)
```

### Definition of Done - Phase 2 ✅ COMPLETE (2026-02-02)
- [x] Table data class created with 16 rows
- [x] TableRow has transpose, volume, 3 FX slots
- [x] Instrument has tableId and tableTicRate fields
- [x] Project has 256 tables
- [x] Existing projects still load (tables default to empty - new fields have defaults)
- [x] Compiles without errors

---

## Phase 3: Table Screen UI (Days 5-8)

### 3.1 Create TableModule

**File:** `app/src/main/java/com/example/pockettracker/ui/modules/TableModule.kt`

**Layout (620×392 pixels):**
```
TABLE 00                                    06 TIC  ← Header
──────────────────────────────────────────────────── ← Separator
   N   V   FX1  FX2  FX3                            ← Column headers
────────────────────────────────────────────────────
 0 +00 FF  ---  ---  ---                            ← 16 table rows
 1 +03 FF  ---  ---  ---
 2 +07 FF  ---  ---  ---
 3 +00 80  ---  ---  ---
 4 +00 FF  H00  ---  ---                            ← HOP 00 = loop to row 0
 5 --- -- ---  ---  ---
 ...
 F --- -- ---  ---  ---
```

**Column Layout:**
| Column | Name | Width | Content |
|--------|------|-------|---------|
| 0 | Step | 2 chars | Hex row number (0-F) |
| 1 | Transpose | 3 chars | +00 to +7F, -80 to -01 |
| 2 | Volume | 2 chars | 00-FF hex |
| 3 | FX1 Type | 3 chars | Effect name (ARP, HOP, TIC, etc.) |
| 4 | FX1 Value | 2 chars | Hex value |
| 5 | FX2 Type | 3 chars | Effect name |
| 6 | FX2 Value | 2 chars | Hex value |
| 7 | FX3 Type | 3 chars | Effect name |
| 8 | FX3 Value | 2 chars | Hex value |

### 3.2 Rendering Implementation

```kotlin
class TableModule(
    override val width: Int = 620,
    override val height: Int = 392
) : TrackerModule {

    fun draw(
        drawScope: DrawScope,
        table: Table,
        cursorRow: Int,
        cursorColumn: Int,
        playbackRow: Int?,
        selectedRows: IntRange?,
        ticRate: Int
    ) {
        // Header: "TABLE XX" + "YY TIC"
        drawHeader(drawScope, table.id, ticRate)

        // Column headers
        drawColumnHeaders(drawScope)

        // 16 table rows
        for (row in 0 until 16) {
            val tableRow = table.rows[row]
            val yPos = HEADER_HEIGHT + (row * ROW_HEIGHT)

            // Row background
            val bgColor = when {
                playbackRow == row -> COLOR_PLAYING
                selectedRows?.contains(row) == true -> COLOR_SELECTED
                cursorRow == row -> COLOR_CURSOR
                row % 4 == 0 -> COLOR_BEAT
                else -> COLOR_DEFAULT
            }
            drawRowBackground(drawScope, yPos, bgColor)

            // Row number (hex)
            drawText(row.toString(16).uppercase(), x = 4, y = yPos)

            // Transpose (signed)
            val transposeStr = formatTranspose(tableRow.transpose)
            drawText(transposeStr, x = COL_TRANSPOSE_X, y = yPos)

            // Volume
            val volStr = if (tableRow.volume == 0xFF) "--"
                        else tableRow.volume.toHex2()
            drawText(volStr, x = COL_VOLUME_X, y = yPos)

            // FX columns (same as phrase)
            drawFxColumn(drawScope, tableRow.fx1Type, tableRow.fx1Value,
                        COL_FX1_X, yPos)
            drawFxColumn(drawScope, tableRow.fx2Type, tableRow.fx2Value,
                        COL_FX2_X, yPos)
            drawFxColumn(drawScope, tableRow.fx3Type, tableRow.fx3Value,
                        COL_FX3_X, yPos)
        }

        // Cursor
        drawCursor(drawScope, cursorRow, cursorColumn)
    }

    private fun formatTranspose(transpose: Int): String {
        return when {
            transpose == 0x00 -> "+00"
            transpose < 0x80 -> "+${transpose.toHex2()}"
            else -> "-${(256 - transpose).toHex2()}"
        }
    }
}
```

### 3.3 Navigation Integration

**File:** `app/src/main/java/com/example/pockettracker/core/logic/TrackerController.kt`

```kotlin
// Navigation grid position for Table: Row 2, Column 4 (right of INST)
// Current grid:
//       0         1         2         3         4
// ┌─────────────────────────────────────────────────┐
// │ ----      ----      SCALE   INST_POOL   ----   │ 0
// │ PROJ      PROJ     GROOVE     MODS      ----   │ 1
// │ SONG     CHAIN     PHRASE     INST     TABLE   │ 2  ← TABLE at (2,4)
// │ MIXER    MIXER     MIXER     MIXER     MIXER   │ 3
// │ EFFECTS  EFFECTS  EFFECTS   EFFECTS   EFFECTS  │ 4
// └─────────────────────────────────────────────────┘

enum class ScreenType {
    PROJECT,                              // Row 1
    SONG, CHAIN, PHRASE, INSTRUMENT, TABLE,  // Row 2
    MIXER,                                // Row 3
    // ...
}

// Screen position mapping
val screenPositions = mapOf(
    ScreenType.PROJECT to Pair(1, 0),
    ScreenType.SONG to Pair(2, 0),
    ScreenType.CHAIN to Pair(2, 1),
    ScreenType.PHRASE to Pair(2, 2),
    ScreenType.INSTRUMENT to Pair(2, 3),
    ScreenType.TABLE to Pair(2, 4),      // Right of INST
    ScreenType.MIXER to Pair(3, 0),
    // ... others
)
```

**Navigation from INST to TABLE:**
- R + RIGHT from INST screen → TABLE screen
- R + LEFT from TABLE screen → INST screen

### 3.4 Input Handling

```kotlin
// TableModule input handling
fun handleInput(
    event: InputEvent,
    table: Table,
    cursorRow: Int,
    cursorColumn: Int
): InputResult {
    when (cursorColumn) {
        0 -> { /* Row number - read only */ }

        1 -> { // Transpose
            if (event.isAHeld) {
                when (event.direction) {
                    UP -> table.rows[cursorRow].transpose =
                          (table.rows[cursorRow].transpose + 1) and 0xFF
                    DOWN -> table.rows[cursorRow].transpose =
                           (table.rows[cursorRow].transpose - 1) and 0xFF
                    LEFT -> table.rows[cursorRow].transpose =
                           (table.rows[cursorRow].transpose - 0x10) and 0xFF
                    RIGHT -> table.rows[cursorRow].transpose =
                            (table.rows[cursorRow].transpose + 0x10) and 0xFF
                }
            }
        }

        2 -> { // Volume
            // Same pattern as phrase volume editing
        }

        3, 5, 7 -> { // FX Types
            // Cycle through effect types
        }

        4, 6, 8 -> { // FX Values
            // Increment/decrement hex values
        }
    }
}
```

### Definition of Done - Phase 3
- [ ] TableModule.kt created
- [ ] Table screen displays 16 rows correctly
- [ ] Transpose shows as signed (+00 to +7F, -80 to -01)
- [ ] Volume shows as hex (00-FF)
- [ ] FX columns match phrase screen format
- [ ] Cursor navigation works (row and column)
- [ ] A+direction edits all fields
- [ ] TIC rate displays in header
- [ ] Table screen at position (2,4) - right of INST screen
- [ ] R+RIGHT from INST navigates to TABLE
- [ ] R+LEFT from TABLE navigates back to INST
- [ ] Playback row highlighting works

---

## Phase 4: TIC Effect (Days 9-11)

### 4.1 TIC Effect Definition

```
TIC XX - Table Tick Rate Control

Values:
  TIC00      = Increment table row each time instrument is triggered
  TIC01-TICFB = Number of ticks per row (01=fastest, FB=slowest)
  TICFC      = Octave Map: Maps playing octave to table row
  TICFD      = Velocity Map: Maps velocity to table row (future)
  TICFE      = Note Map: Maps note (0-11) to table row
  TICFF      = Increment table row at 200 Hz (very fast)

Default: TIC06 = 6 tics per row (2 table rows per phrase step at 12 tics/step)
```

### 4.2 Add TIC to EffectType Enum

**File:** `app/src/main/java/com/example/pockettracker/core/data/EffectTypes.kt`

```kotlin
enum class EffectType(val code: Int, val displayName: String) {
    NONE(0x00, "---"),
    ARP(0x01, "ARP"),
    ARC(0x02, "ARC"),
    OFF(0x03, "OFF"),
    VOL(0x04, "VOL"),
    KIL(0x05, "KIL"),
    REP(0x06, "REP"),
    HOP(0x07, "HOP"),    // NEW
    TIC(0x08, "TIC"),    // NEW
    PSL(0x09, "PSL"),    // NEW - Pitch Slide
    PBN(0x0A, "PBN"),    // NEW - Pitch Bend
    PVB(0x0B, "PVB"),    // NEW - Vibrato
    PVX(0x0C, "PVX"),    // NEW - Extreme Vibrato
    TBL(0x0D, "TBL");    // NEW - Table select

    companion object {
        fun fromCode(code: Int): EffectType =
            values().find { it.code == code } ?: NONE
    }
}
```

### 4.3 Table Playback State

**File:** `app/src/main/java/com/example/pockettracker/core/logic/PlaybackController.kt`

```kotlin
// Per-voice table state (tracks table position for each playing note)
data class TableState(
    var tableId: Int = -1,
    var currentRow: Int = 0,
    var ticMode: Int = 0x06,        // TIC value (default = 6 tics/row)
    var ticCounter: Int = 0,        // Counts tics since last row advance
    var triggerCount: Int = 0,      // For TIC00 mode
    var lastTriggerFrame: Long = 0  // Frame when note was triggered
)

// Add to TrackState
data class TrackState(
    // ... existing fields
    var tableState: TableState = TableState()
)
```

### 4.4 Table Processing in PlaybackController

```kotlin
fun processTableForVoice(
    trackId: Int,
    instrument: Instrument,
    currentFrame: Long,
    currentTic: Int
): TableModifiers {
    val state = trackStates[trackId].tableState
    val tableId = state.tableId.takeIf { it >= 0 } ?: instrument.tableId

    if (tableId < 0) return TableModifiers.NONE

    val table = project.tables[tableId]
    val row = table.rows[state.currentRow]

    // Check if we should advance table row
    when (state.ticMode) {
        0x00 -> {
            // TIC00: Advance on each trigger
            // Handled separately in note trigger
        }
        in 0x01..0xFB -> {
            // Standard tic-based advance
            state.ticCounter++
            if (state.ticCounter >= state.ticMode) {
                state.ticCounter = 0
                advanceTableRow(state, table)
            }
        }
        0xFC -> {
            // Octave Map: Row = octave (0-9)
            // Set in note trigger based on note octave
        }
        0xFD -> {
            // Velocity Map: Row = velocity/16 (0-15)
            // Future: when velocity is implemented
        }
        0xFE -> {
            // Note Map: Row = note (0-11)
            // Set in note trigger based on note pitch
        }
        0xFF -> {
            // 200 Hz: ~4.4 rows per frame at 44100Hz
            // Very fast, handled in audio callback
        }
    }

    // Apply table row effects
    return TableModifiers(
        transpose = TableRow.transposeToSemitones(row.transpose),
        volume = row.volume,
        fx1 = Pair(row.fx1Type, row.fx1Value),
        fx2 = Pair(row.fx2Type, row.fx2Value),
        fx3 = Pair(row.fx3Type, row.fx3Value)
    )
}

private fun advanceTableRow(state: TableState, table: Table) {
    state.currentRow = (state.currentRow + 1) % 16

    // Check for HOP effect in current row (handled in effect processor)
}

data class TableModifiers(
    val transpose: Int = 0,
    val volume: Int = 0xFF,
    val fx1: Pair<Int, Int> = Pair(0, 0),
    val fx2: Pair<Int, Int> = Pair(0, 0),
    val fx3: Pair<Int, Int> = Pair(0, 0)
) {
    companion object {
        val NONE = TableModifiers()
    }
}
```

### Definition of Done - Phase 4
- [ ] TIC effect type added to enum
- [ ] TableState tracks table playback per voice
- [ ] TIC00 advances row on trigger
- [ ] TIC01-FB advances row every N tics
- [ ] TICFC maps octave to row
- [ ] TICFE maps note to row
- [ ] TICFF advances at 200Hz
- [ ] TIC effect works in phrase FX columns
- [ ] TIC effect works in table FX columns
- [ ] Table TIC rate displayed in header updates

---

## Phase 5: HOP Effect (Days 12-14)

### 5.1 HOP Effect Definition

```
HOP XY - Jump Playback Position

In Phrase:
  HOP00-HOPFE = Jump to row Y on NEXT phrase in chain
  HOPFF       = Stop playback of current track

In Table:
  HOP00-HOPFE = Jump to row Y for X times (then continue)
  HOPFF       = Stop table playback (voice continues)

Special cases:
  HOP00 at row 0C = Loop table to 12 notes (one octave)
  HOP00 at row 0F = Infinite loop (common use)
```

### 5.2 HOP in Phrase Context

**File:** `app/src/main/java/com/example/pockettracker/core/logic/PlaybackController.kt`

```kotlin
// Add to TrackState for HOP tracking
data class TrackState(
    // ... existing
    var hopTargetRow: Int = -1,     // -1 = no hop, 0-15 = target row
    var trackStopped: Boolean = false
)

fun processHopEffect(
    trackId: Int,
    hopValue: Int,
    context: EffectContext  // PHRASE or TABLE
): HopResult {
    return when (context) {
        EffectContext.PHRASE -> {
            if (hopValue == 0xFF) {
                // HOPFF = Stop track
                trackStates[trackId].trackStopped = true
                HopResult.StopTrack
            } else {
                // HOP XY = Jump to row Y on next phrase
                trackStates[trackId].hopTargetRow = hopValue and 0x0F
                HopResult.JumpOnNextPhrase(hopValue and 0x0F)
            }
        }

        EffectContext.TABLE -> {
            val times = (hopValue shr 4) and 0x0F
            val targetRow = hopValue and 0x0F

            if (hopValue == 0xFF) {
                // HOPFF = Stop table (voice continues)
                HopResult.StopTable
            } else {
                // HOP XY = Jump to row Y, X times
                HopResult.JumpInTable(targetRow, times)
            }
        }
    }
}

sealed class HopResult {
    object None : HopResult()
    object StopTrack : HopResult()
    object StopTable : HopResult()
    data class JumpOnNextPhrase(val targetRow: Int) : HopResult()
    data class JumpInTable(val targetRow: Int, val times: Int) : HopResult()
}
```

### 5.3 HOP in Table Context

```kotlin
// Add to TableState
data class TableState(
    // ... existing
    var hopCounter: Int = 0,        // Counts remaining HOP jumps
    var hopTargetRow: Int = -1      // -1 = no active hop
)

fun processTableHop(
    state: TableState,
    hopValue: Int
): Boolean {  // Returns true if should jump
    if (hopValue == 0xFF) {
        // Stop table
        state.tableId = -1
        return false
    }

    val times = (hopValue shr 4) and 0x0F
    val targetRow = hopValue and 0x0F

    if (times == 0) {
        // HOP 0Y = Infinite loop to row Y
        state.currentRow = targetRow
        return true
    }

    // HOP XY = Jump X times, then continue
    if (state.hopCounter == 0) {
        state.hopCounter = times
        state.hopTargetRow = targetRow
    }

    if (state.hopCounter > 0) {
        state.hopCounter--
        state.currentRow = targetRow
        return true
    }

    // Counter exhausted, continue normally
    state.hopTargetRow = -1
    return false
}
```

### 5.4 Odd Time Signatures with HOP

**Example: 5/4 time signature (20 steps instead of 16)**
```
Phrase 00 (rows 0-F):
  Row 0: C-4 ...
  Row 4: E-4 ...
  Row 8: G-4 ...
  Row C: A-4 ...
  Row F: --- HOP 04  ← Jump to row 4 on next phrase

Phrase 01 (rows 4-F only, due to HOP):
  Row 4: B-4 ...
  Row 8: C-5 ...
  Row C: --- ...
  Row F: --- HOP 00  ← Jump back to row 0
```

### Definition of Done - Phase 5
- [ ] HOP effect type added
- [ ] In phrase: HOPFF stops track
- [ ] In phrase: HOP XY jumps to row Y on next phrase
- [ ] In table: HOP 0Y loops infinitely to row Y
- [ ] In table: HOP XY jumps X times, then continues
- [ ] Odd time signatures work (HOP at row F to row 4)
- [ ] HOP respected in chain playback
- [ ] HOP respected in song playback
- [ ] Track stop persists until new chain starts

---

## Phase 6: Real-time Parameter System (Days 15-18)

### 6.1 C++ Voice Parameter Updates

**File:** `app/src/main/cpp/native-audio.cpp`

```cpp
// Add real-time modifiable parameters to Voice
struct Voice {
    // ... existing

    // Real-time pitch modulation
    float pitchOffset;          // Semitones offset (can be fractional)
    float pitchSlideTarget;     // Target semitones for slide
    float pitchSlideRate;       // Semitones per sample
    bool pitchSliding;

    // Vibrato
    float vibratoPhase;         // Current LFO phase (0-2π)
    float vibratoSpeed;         // Hz
    float vibratoDepth;         // Semitones
    bool vibratoActive;
};

// Update voice parameters every audio callback
void updateVoiceParameters(Voice& voice, int framesPerCallback) {
    // Pitch slide
    if (voice.pitchSliding) {
        float delta = voice.pitchSlideTarget - voice.pitchOffset;
        if (abs(delta) < voice.pitchSlideRate * framesPerCallback) {
            voice.pitchOffset = voice.pitchSlideTarget;
            voice.pitchSliding = false;
        } else {
            voice.pitchOffset += (delta > 0 ? 1 : -1) *
                                 voice.pitchSlideRate * framesPerCallback;
        }
    }

    // Vibrato LFO
    if (voice.vibratoActive) {
        voice.vibratoPhase += (2 * M_PI * voice.vibratoSpeed) / SAMPLE_RATE
                             * framesPerCallback;
        if (voice.vibratoPhase > 2 * M_PI) {
            voice.vibratoPhase -= 2 * M_PI;
        }
    }
}

// Apply pitch modulation to playback rate
float getModulatedPlaybackRate(Voice& voice) {
    float pitchMod = voice.pitchOffset;

    if (voice.vibratoActive) {
        pitchMod += sin(voice.vibratoPhase) * voice.vibratoDepth;
    }

    // Convert semitones to rate multiplier
    float rateMod = pow(2.0f, pitchMod / 12.0f);
    return voice.playbackRate * rateMod;
}
```

### 6.2 JNI Functions for Real-time Control

```cpp
// Set pitch slide for a voice
extern "C" JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_setPitchSlide(
    JNIEnv *env, jobject thiz,
    jint trackId,
    jfloat targetSemitones,
    jfloat durationTicks
) {
    Voice& voice = voices[trackId];
    if (!voice.isActive) return;

    float framesPerTick = SAMPLE_RATE * 60.0f / (currentTempo * TICS_PER_STEP);
    float totalFrames = framesPerTick * durationTicks;

    voice.pitchSlideTarget = targetSemitones;
    voice.pitchSlideRate = abs(targetSemitones - voice.pitchOffset) / totalFrames;
    voice.pitchSliding = true;
}

// Set pitch bend (continuous slide)
extern "C" JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_setPitchBend(
    JNIEnv *env, jobject thiz,
    jint trackId,
    jfloat semitonesPerTick
) {
    Voice& voice = voices[trackId];
    if (!voice.isActive) return;

    float framesPerTick = SAMPLE_RATE * 60.0f / (currentTempo * TICS_PER_STEP);
    voice.pitchSlideRate = semitonesPerTick / framesPerTick;
    voice.pitchSliding = true;
    voice.pitchSlideTarget = semitonesPerTick > 0 ? 127 : -127;  // Slide until stopped
}

// Set vibrato
extern "C" JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_setVibrato(
    JNIEnv *env, jobject thiz,
    jint trackId,
    jfloat speed,
    jfloat depth
) {
    Voice& voice = voices[trackId];
    if (!voice.isActive) return;

    voice.vibratoSpeed = speed;
    voice.vibratoDepth = depth;
    voice.vibratoActive = depth > 0.01f;
}

// Clear all pitch modulation
extern "C" JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_clearPitchMod(
    JNIEnv *env, jobject thiz,
    jint trackId
) {
    Voice& voice = voices[trackId];
    voice.pitchOffset = 0;
    voice.pitchSliding = false;
    voice.vibratoActive = false;
}
```

### 6.3 Kotlin Backend Interface Updates

**File:** `app/src/main/java/com/example/pockettracker/core/audio/IAudioBackend.kt`

```kotlin
interface IAudioBackend {
    // ... existing

    // Real-time pitch control
    fun setPitchSlide(trackId: Int, targetSemitones: Float, durationTicks: Float)
    fun setPitchBend(trackId: Int, semitonesPerTick: Float)
    fun setVibrato(trackId: Int, speed: Float, depth: Float)
    fun clearPitchMod(trackId: Int)
}
```

### Definition of Done - Phase 6
- [ ] Voice struct has pitch modulation fields
- [ ] Pitch slide interpolates smoothly
- [ ] Vibrato LFO runs in audio callback
- [ ] Playback rate affected by pitch mods
- [ ] JNI functions for all pitch controls
- [ ] Kotlin IAudioBackend interface updated
- [ ] No audio clicks during parameter changes
- [ ] Pitch mods cleared on new note trigger

---

## Phase 7: Pitch Effects (Days 19-23)

### 7.1 PSL - Pitch Slide (Portamento)

```
PSL XX - Pitch Slide
  Enables portamento for currently playing instrument.
  XX = duration in ticks (01 = fast, FF = slow)

  Behavior:
  - Note A plays
  - Note B with PSL XX
  - Pitch slides from A to B over XX ticks

  Example:
  Row 0: C-4 01 FF ---
  Row 4: E-4 01 FF PSL 18  ← Slide from C-4 to E-4 over 24 ticks
```

**Implementation:**
```kotlin
// In PlaybackController
fun processPitchSlide(
    trackId: Int,
    currentNote: Note,
    pslValue: Int
) {
    val state = trackStates[trackId]

    if (pslValue == 0) {
        // PSL00 = Instant (no slide)
        return
    }

    // Calculate semitone difference
    val fromMidi = state.lastNote?.toMidi() ?: return
    val toMidi = currentNote.toMidi()
    val semitones = (toMidi - fromMidi).toFloat()

    // Set slide in audio engine
    audioBackend.setPitchSlide(trackId, semitones, pslValue.toFloat())
}
```

### 7.2 PBN - Pitch Bend (Continuous)

```
PBN XX - Pitch Bend
  Continuous pitch slide up or down.
  00-7F = Bend UP by amount
  80-FF = Bend DOWN by amount

  Rate = (XX & 0x7F) / 16 semitones per step

  Example:
  Row 0: C-4 01 FF PBN 10  ← Bend up slowly
  Row 8: --- -- -- PBN 00  ← Stop bending
```

**Implementation:**
```kotlin
fun processPitchBend(trackId: Int, pbnValue: Int) {
    if (pbnValue == 0) {
        // PBN00 = Stop bending
        audioBackend.setPitchBend(trackId, 0f)
        return
    }

    val magnitude = pbnValue and 0x7F
    val direction = if (pbnValue < 0x80) 1f else -1f

    // Rate in semitones per tic
    val semitonesPerTic = (magnitude / 16f) * direction / TICS_PER_STEP

    audioBackend.setPitchBend(trackId, semitonesPerTic)
}
```

### 7.3 PVB - Vibrato

```
PVB XY - Vibrato
  X = Speed (0-F, higher = faster)
  Y = Depth (0-F, higher = wider)

  Speed formula: Hz = 2 + X * 0.5  (2Hz to 9.5Hz)
  Depth formula: semitones = Y * 0.125  (0 to 1.875 semitones)

  Example:
  Row 0: C-4 01 FF PVB 64  ← Medium speed (6), subtle depth (4)
```

**Implementation:**
```kotlin
fun processVibrato(trackId: Int, pvbValue: Int, extreme: Boolean = false) {
    val speedNibble = (pvbValue shr 4) and 0x0F
    val depthNibble = pvbValue and 0x0F

    if (pvbValue == 0) {
        // PVB00 = Stop vibrato
        audioBackend.setVibrato(trackId, 0f, 0f)
        return
    }

    val speed = 2f + speedNibble * 0.5f  // 2Hz - 9.5Hz
    var depth = depthNibble * 0.125f     // 0 - 1.875 semitones

    if (extreme) {
        // PVX = 4x depth and 2x speed
        depth *= 4f
        speed *= 2f
    }

    audioBackend.setVibrato(trackId, speed, depth)
}
```

### 7.4 PVX - Extreme Vibrato

```
PVX XY - Extreme Vibrato
  Same as PVB but with 4x depth and 2x speed

  Example:
  Row 0: C-4 01 FF PVX F8  ← Very fast, very deep wobble
```

### 7.5 Effect Persistence

```kotlin
// Pitch effects persistence rules:
// - PSL: One-shot (doesn't persist)
// - PBN: Persists until PBN00, new note, or KILL
// - PVB/PVX: Persists until PVB00/PVX00, new note, or KILL

data class TrackState(
    // ... existing
    var pitchBendActive: Boolean = false,
    var vibratoActive: Boolean = false
)

fun onNewNoteTrigger(trackId: Int) {
    // Clear pitch mods on new note
    if (trackStates[trackId].pitchBendActive ||
        trackStates[trackId].vibratoActive) {
        audioBackend.clearPitchMod(trackId)
        trackStates[trackId].pitchBendActive = false
        trackStates[trackId].vibratoActive = false
    }
}
```

### Definition of Done - Phase 7
- [ ] PSL effect slides pitch over duration
- [ ] PSL works when note changes (A→B slides)
- [ ] PBN bends pitch continuously up/down
- [ ] PBN00 stops bending
- [ ] PVB adds vibrato with speed/depth control
- [ ] PVX is 4x deeper, 2x faster vibrato
- [ ] All pitch effects respect persistence rules
- [ ] New note clears pitch mods
- [ ] KILL clears pitch mods
- [ ] No audio artifacts during pitch changes

---

## Phase 8: Integration Testing (Days 24-28)

### 8.1 Table + Effect Integration Tests

| Test | Steps | Expected Result |
|------|-------|-----------------|
| Basic table | Instrument with table, play note | Table rows cycle |
| TIC06 | Default tic rate | 2 table rows per step |
| TIC00 | Per-trigger mode | Row advances on each note |
| TICFE | Note map | Row = note (C=0, C#=1, ...) |
| HOP in table | HOP 00 at row 4 | Loops rows 0-4 |
| HOP in phrase | HOP 08 at row F | Next phrase starts at row 8 |
| HOPFF | Stop track | Track silent, others continue |

### 8.2 Pitch Effect Tests

| Test | Steps | Expected Result |
|------|-------|-----------------|
| PSL basic | C-4 then E-4 PSL 18 | Smooth slide |
| PSL fast | PSL 01 | Nearly instant |
| PBN up | PBN 20 | Pitch rises continuously |
| PBN down | PBN A0 | Pitch falls continuously |
| PBN stop | PBN 00 | Pitch stops changing |
| PVB subtle | PVB 44 | Gentle vibrato |
| PVB fast | PVB F4 | Fast, subtle vibrato |
| PVX extreme | PVX FF | Wild wobble |

### 8.3 Bug Fix Verification

| Test | Steps | Expected Result |
|------|-------|-----------------|
| Meter decay | Stop playback | Meters fade to 0 |
| Oscilloscope | Stop playback | Waveform clears |
| Volume immediate | Change track vol while playing | Volume changes |
| L+A cut | In selection, press L+A | Cuts (not paste) |
| L+R exit | In selection, press L+R | Exits selection |

### 8.4 Cross-Feature Tests

| Test | Steps | Expected Result |
|------|-------|-----------------|
| Table + vibrato | PVB in table FX | Vibrato applies to instrument |
| HOP + arpeggio | ARP in hopping phrase | Arpeggio continues across hop |
| Table + repeat | REP in table | Note repeats with table mods |
| Export with tables | WAV export | Tables rendered correctly |

### Definition of Done - Phase 8
- [ ] All unit tests pass
- [ ] All integration tests pass
- [ ] Performance acceptable (no frame drops)
- [ ] Tested on Miyoo Flip
- [ ] Tested on Ayaneo (if available)
- [ ] No new bugs introduced
- [ ] Documentation updated

---

## Summary Timeline

| Week | Phase | Tasks |
|------|-------|-------|
| 1 | 1, 2 | Bug fixes, Table data model |
| 2 | 3, 4 | Table screen UI, TIC effect |
| 3 | 5, 6 | HOP effect, Real-time parameter system |
| 4 | 7, 8 | Pitch effects, Integration testing |

**Buffer:** 4-5 days for unexpected issues

---

## Files to Create/Modify

### New Files
- `app/src/main/java/com/example/pockettracker/ui/modules/TableModule.kt`
- `app/src/main/java/com/example/pockettracker/core/data/Table.kt` (or in TrackerData.kt)

### Modified Files
- `app/src/main/java/com/example/pockettracker/core/data/TrackerData.kt` (Table data)
- `app/src/main/java/com/example/pockettracker/core/data/EffectTypes.kt` (new effects)
- `app/src/main/java/com/example/pockettracker/core/logic/PlaybackController.kt` (table playback)
- `app/src/main/java/com/example/pockettracker/core/logic/EffectProcessor.kt` (new effects)
- `app/src/main/java/com/example/pockettracker/core/logic/InputController.kt` (L+A fix)
- `app/src/main/java/com/example/pockettracker/core/storage/FileManager.kt` (remove migration)
- `app/src/main/java/com/example/pockettracker/ui/modules/MixerModule.kt` (decay fix)
- `app/src/main/java/com/example/pockettracker/core/audio/IAudioBackend.kt` (pitch methods)
- `app/src/main/java/com/example/pockettracker/platform/android/OboeAudioBackend.kt` (pitch JNI)
- `app/src/main/cpp/native-audio.cpp` (real-time pitch, decay fix)
- `app/src/main/java/com/example/pockettracker/core/logic/TrackerController.kt` (navigation)
- `app/src/main/java/com/example/pockettracker/MainActivity.kt` (Table screen integration)

---

## Post-Extension Ideas (Future)

These are explicitly NOT in this extension:

**More Effects:**
- Filter automation (CUT, RES)
- Distortion automation
- Delay/reverb sends

**More TIC Modes:**
- Velocity mapping (when velocity added)
- Custom table sequences

**Advanced Tables:**
- Linked tables (chain tables together)
- Table macros

**UI Improvements:**
- Table preview in instrument screen
- Effect help popup (like M8)

---

## How to Use This Document

### Starting Work

1. Read this document first
2. Check current progress via Phase checkboxes
3. Continue from next incomplete phase
4. Update checkboxes as tasks complete

### Session Quick Start

1. Which phase are we on?
2. What's the next unchecked item?
3. Are there any blockers?

### Questions for Developer

Before starting:
- Confirm L+R vs L delay approach for selection fix?
- Confirm Option A vs B for volume immediate fix?
- Any preference on C++ vs Kotlin meter decay?

---

**Document Version:** 1.0
**Created:** 2026-02-01
**Author:** Claude Code + Developer collaboration
