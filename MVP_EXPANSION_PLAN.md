# MVP Expansion Plan: Mixer, Volume/Pan, and WAV Export

## Document Purpose
Implementation plan for expanding MVP scope with extra development time.
Created: 2026-01-23

**Target Completion:** ~2 weeks from start

---

## Overview

### Features to Add (Priority Order)
1. **Instrument VOL/PAN UI** - Expose existing data in instrument screen
2. **Track Volume System** - Add per-track volume to data model
3. **Volume Multiplication Chain** - Implement proper gain staging
4. **Stereo Pan in Audio Engine** - C++ stereo output with pan
5. **Mixer Screen** - Visual mixer with 8 tracks + master
6. **WAV Export** - Render song to stereo WAV file

### Architecture Principle
All new features follow existing portable architecture:
- Business logic in `core/` (no Android imports)
- Audio processing in C++ (native-audio.cpp)
- Platform-specific code only in `platform/android/`

---

## Phase 1: Data Model Updates (Day 1)

### 1.1 Update TrackerData.kt

**File:** `app/src/main/java/com/example/pockettracker/core/data/TrackerData.kt`

**Changes to Instrument class:**
```kotlin
@Serializable
data class Instrument(
    val id: Int,
    var name: String = "INST${id.toString(16).padStart(2,'0').uppercase()}",
    var sampleId: Int = -1,

    // Volume/Pan - Changed from Float to Int for hex UI consistency
    var volume: Int = 0xFF,      // 00-FF (FF = max, maps to 1.0)
    var pan: Int = 0x80,         // 00-FF (00=left, 80=center, FF=right)

    // ... rest unchanged
)
```

**Changes to Track class:**
```kotlin
@Serializable
data class Track(
    val id: Int,
    val chainRefs: MutableList<Int> = mutableListOf(),
    var volume: Int = 0xFF,      // NEW: Track volume 00-FF
    var mute: Boolean = false    // NEW: Track mute (optional for MVP)
)
```

**Changes to Project class:**
```kotlin
@Serializable
data class Project(
    var name: String = "UNTITLED",
    var tempo: Int = 128,
    var transpose: Int = 0,
    var masterVolume: Int = 0xFF,  // Changed: Int 00-FF instead of Float
    // ... rest unchanged
)
```

### 1.2 Add Volume Helper Functions

**File:** `app/src/main/java/com/example/pockettracker/core/data/TrackerData.kt`

```kotlin
// Volume conversion utilities
object VolumeUtils {
    // Convert 00-FF hex to 0.0-1.0 float
    fun hexToFloat(hex: Int): Float = (hex and 0xFF) / 255f

    // Convert 0.0-1.0 float to 00-FF hex
    fun floatToHex(f: Float): Int = (f.coerceIn(0f, 1f) * 255).toInt()

    // Calculate final volume with gain staging
    fun calculateFinalVolume(
        instrumentVol: Int,
        phraseVol: Int,
        trackVol: Int,
        masterVol: Int
    ): Float {
        return hexToFloat(instrumentVol) *
               hexToFloat(phraseVol) *
               hexToFloat(trackVol) *
               hexToFloat(masterVol)
    }

    // Convert 00-FF pan to left/right gains (constant power pan law)
    fun panToGains(pan: Int): Pair<Float, Float> {
        val p = hexToFloat(pan)  // 0.0 = left, 1.0 = right
        val angle = p * (Math.PI / 2)
        val leftGain = kotlin.math.cos(angle).toFloat()
        val rightGain = kotlin.math.sin(angle).toFloat()
        return Pair(leftGain, rightGain)
    }
}
```

### Definition of Done - Phase 1
- [ ] Instrument.volume changed to Int (00-FF)
- [ ] Instrument.pan changed to Int (00-FF, 80=center)
- [ ] Track.volume added (00-FF)
- [ ] Track.mute added (Boolean)
- [ ] Project.masterVolume changed to Int (00-FF)
- [ ] VolumeUtils object created with helper functions
- [ ] Existing projects still load (migration: Float→Int conversion)
- [ ] Compiles without errors

---

## Phase 2: Instrument Screen VOL/PAN UI (Days 1-2)

### 2.1 Update InstrumentModule Layout

**File:** `app/src/main/java/com/example/pockettracker/ui/modules/InstrumentModule.kt`

**New layout with VOL/PAN:**
```
INSTRUMENT 00: KICK
─────────────────────────────────────────
SAMPLE: kick.wav
ROOT: C-4    DETUNE: 80    VOL: FF    PAN: 80
FILT: off    CUT: 00       RES: 00
START: 00    END: FF       REV: off   LOOP: off
```

**Column positions (character units):**
- ROOT: col 0
- DETUNE: col 1
- VOL: col 2
- PAN: col 3
- FILT: col 0 (row +1)
- CUT: col 1 (row +1)
- RES: col 2 (row +1)

### 2.2 Add Cursor Navigation for New Fields

**Cursor columns to add:**
- Column 2 on row 1: VOL
- Column 3 on row 1: PAN

**Input handling:**
- A + UP/DOWN: Increment/decrement by 1
- A + LEFT/RIGHT: Increment/decrement by 16 (0x10)

### Definition of Done - Phase 2
- [ ] VOL field visible in instrument screen
- [ ] PAN field visible in instrument screen
- [ ] Cursor can navigate to VOL/PAN
- [ ] A+direction edits VOL (00-FF)
- [ ] A+direction edits PAN (00-FF)
- [ ] Values save/load correctly in .ptp files
- [ ] Values display as 2-digit hex

---

## Phase 3: Audio Engine Stereo Pan (Days 2-3)

### 3.1 Update C++ Audio Engine

**File:** `app/src/main/cpp/native-audio.cpp`

**Current state:** Mono output duplicated to stereo
**Target state:** True stereo with per-voice pan

**Changes to ScheduledNote struct:**
```cpp
struct ScheduledNote {
    long triggerFrame;
    int sampleId;
    int trackId;
    float frequency;
    float volume;
    float pan;          // NEW: 0.0 = left, 0.5 = center, 1.0 = right
    float leftGain;     // NEW: Pre-calculated left channel gain
    float rightGain;    // NEW: Pre-calculated right channel gain
    // ... rest unchanged
};
```

**Changes to Voice struct:**
```cpp
struct Voice {
    // ... existing fields
    float pan;
    float leftGain;
    float rightGain;
};
```

**Changes to audio callback:**
```cpp
// Apply pan in stereo mix
float sample = /* interpolated sample */ * voice.volume;
outputLeft += sample * voice.leftGain;
outputRight += sample * voice.rightGain;
```

### 3.2 Update JNI Interface

**File:** `app/src/main/cpp/native-audio.cpp`

**Update scheduleNote function:**
```cpp
extern "C" JNIEXPORT void JNICALL
Java_com_example_pockettracker_core_audio_OboeAudioBackend_scheduleNote(
    JNIEnv *env, jobject thiz,
    jlong triggerFrame,
    jint sampleId,
    jint trackId,
    jfloat frequency,
    jfloat volume,
    jfloat pan,           // NEW parameter
    jint startPoint,
    jint endPoint,
    jboolean reverse,
    jint loopMode
) {
    // Calculate constant-power pan
    float angle = pan * M_PI_2;
    float leftGain = cos(angle);
    float rightGain = sin(angle);

    // Schedule with pan
    // ...
}
```

### 3.3 Update Kotlin Audio Backend

**File:** `app/src/main/java/com/example/pockettracker/platform/android/OboeAudioBackend.kt`

**Update scheduleNote signature:**
```kotlin
external fun scheduleNote(
    triggerFrame: Long,
    sampleId: Int,
    trackId: Int,
    frequency: Float,
    volume: Float,
    pan: Float,           // NEW parameter
    startPoint: Int,
    endPoint: Int,
    reverse: Boolean,
    loopMode: Int
)
```

### 3.4 Update PlaybackController

**File:** `app/src/main/java/com/example/pockettracker/core/logic/PlaybackController.kt`

**Pass instrument pan to audio engine:**
```kotlin
fun scheduleNoteWithParams(/* ... */) {
    val instrument = project.instruments[instrumentId]
    val pan = VolumeUtils.hexToFloat(instrument.pan)

    audioBackend.scheduleNote(
        triggerFrame = frame,
        sampleId = instrument.sampleId,
        trackId = trackId,
        frequency = freq,
        volume = finalVolume,
        pan = pan,              // NEW: pass pan
        startPoint = startPoint,
        endPoint = endPoint,
        reverse = instrument.reverse,
        loopMode = loopModeInt
    )
}
```

### Definition of Done - Phase 3
- [ ] C++ scheduleNote accepts pan parameter
- [ ] Voice struct stores pan and L/R gains
- [ ] Audio callback mixes with correct pan
- [ ] JNI signature updated
- [ ] Kotlin backend passes pan
- [ ] PlaybackController reads instrument pan
- [ ] Pan 0x00 = full left verified by ear
- [ ] Pan 0x80 = center verified by ear
- [ ] Pan 0xFF = full right verified by ear
- [ ] No audio artifacts or clicks

---

## Phase 4: Volume Chain Implementation (Day 3-4)

### 4.1 Update PlaybackController Volume Calculation

**File:** `app/src/main/java/com/example/pockettracker/core/logic/PlaybackController.kt`

**Current:** Uses only phrase step volume
**Target:** Multiplies instrument × phrase × track × master

```kotlin
fun calculateNoteVolume(
    project: Project,
    instrumentId: Int,
    phraseVolume: Int,
    trackId: Int
): Float {
    val instVol = project.instruments[instrumentId].volume
    val trackVol = project.tracks[trackId].volume
    val masterVol = project.masterVolume

    return VolumeUtils.calculateFinalVolume(
        instrumentVol = instVol,
        phraseVol = phraseVolume,
        trackVol = trackVol,
        masterVol = masterVol
    )
}
```

### 4.2 Apply to All Playback Modes

Update volume calculation in:
- `scheduleStepWithEffects()` - phrase playback
- Chain playback loop
- Song playback loop

### Definition of Done - Phase 4
- [ ] Volume calculation uses 4-stage multiplication
- [ ] Instrument vol affects all notes using that instrument
- [ ] Track vol affects all notes on that track
- [ ] Master vol affects entire mix
- [ ] Phrase vol still works as before (now relative to chain)
- [ ] Full volume (all FF) = same loudness as before
- [ ] Volume system tested in phrase/chain/song modes

---

## Phase 5: Mixer Screen (Days 4-6)

### 5.1 Create MixerModule

**File:** `app/src/main/java/com/example/pockettracker/ui/modules/MixerModule.kt`

**Layout (620×392 pixels, same as other editor modules):**
```
┌────────────────────────────────────────────────────────────┐
│ MIXER                                            120BPM   │
├────────────────────────────────────────────────────────────┤
│                                                            │
│   01    02    03    04    05    06    07    08    MSTR    │
│  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌────┐  │
│  │▓▓│  │▓▓│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │▓▓▓▓│  │
│  │▓▓│  │▓▓│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │▓▓▓▓│  │
│  │▓▓│  │▓▓│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │▓▓▓▓│  │
│  │▓▓│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │▓▓▓▓│  │
│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░░░│  │
│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░░░│  │
│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░░░│  │
│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░│  │░░░░│  │
│  └──┘  └──┘  └──┘  └──┘  └──┘  └──┘  └──┘  └──┘  └────┘  │
│   FF    FF    FF    FF    FF    FF    FF    FF     FF     │
│                                                            │
│  Cursor: Track 01 VOL                                     │
└────────────────────────────────────────────────────────────┘
```

**Meter dimensions:**
- Track meters: 20px wide × 200px tall each
- Master meter: 40px wide × 200px tall
- Spacing: 60px between track centers
- Starting X: 30px from left

### 5.2 Mixer State and Input

**Cursor navigation:**
- LEFT/RIGHT: Move between tracks (01-08) and MSTR
- Columns: 0-7 = tracks, 8 = master

**Input handling:**
- A + UP: Increase volume (+1)
- A + DOWN: Decrease volume (-1)
- A + LEFT: Decrease volume (-16/0x10)
- A + RIGHT: Increase volume (+16/0x10)

### 5.3 Peak Meter Implementation

**Add to C++ audio engine (native-audio.cpp):**
```cpp
// Peak level tracking per track
float trackPeaks[8] = {0};
float masterPeakL = 0;
float masterPeakR = 0;

// In audio callback, after mixing:
for (int t = 0; t < 8; t++) {
    // Update peak with decay
    trackPeaks[t] = max(trackPeaks[t] * 0.95f, currentTrackLevel[t]);
}
masterPeakL = max(masterPeakL * 0.95f, abs(outputLeft));
masterPeakR = max(masterPeakR * 0.95f, abs(outputRight));
```

**JNI functions to add:**
```cpp
extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_example_pockettracker_core_audio_OboeAudioBackend_getTrackPeaks(
    JNIEnv *env, jobject thiz
);

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_example_pockettracker_core_audio_OboeAudioBackend_getMasterPeak(
    JNIEnv *env, jobject thiz
);
```

### 5.4 Meter Visualization

**Drawing algorithm:**
```kotlin
fun drawMeter(
    drawScope: DrawScope,
    x: Float,
    y: Float,
    width: Float,
    height: Float,
    level: Float  // 0.0 - 1.0
) {
    // Background (dark gray)
    drawRect(color = COLOR_METER_BG, x, y, width, height)

    // Level bar (from bottom up)
    val levelHeight = height * level
    val levelY = y + height - levelHeight

    // Color based on level
    val color = when {
        level > 0.95f -> COLOR_METER_RED     // Clipping
        level > 0.75f -> COLOR_METER_YELLOW  // Hot
        else -> COLOR_METER_GREEN            // Normal
    }

    drawRect(color = color, x, levelY, width, levelHeight)
}
```

### Definition of Done - Phase 5
- [ ] MixerModule.kt created
- [ ] Mixer screen navigable via L/R+DPAD from row 4
- [ ] 8 track columns + 1 master column displayed
- [ ] Volume values (00-FF) shown below each meter
- [ ] Cursor moves between columns
- [ ] A+direction edits track volumes
- [ ] A+direction edits master volume
- [ ] Peak meters update during playback
- [ ] Meter colors: green/yellow/red zones
- [ ] Meters decay smoothly when audio stops
- [ ] Track mute toggle (B button) - optional

---

## Phase 6: WAV Export (Days 6-8)

### 6.1 Create RenderController

**File:** `app/src/main/java/com/example/pockettracker/core/logic/RenderController.kt`

```kotlin
class RenderController(
    private val project: Project,
    private val audioEngine: AudioEngine,
    private val fileSystem: IFileSystem
) {
    // Render state
    var isRendering by mutableStateOf(false)
    var renderProgress by mutableStateOf(0f)  // 0.0 - 1.0
    var renderMessage by mutableStateOf("")

    // Render song to WAV file
    suspend fun renderSongToWav(
        outputDir: String = "Documents/PocketTracker/Renders"
    ): RenderResult {
        isRendering = true
        renderProgress = 0f

        try {
            // 1. Find song bounds (first to last used row)
            val (startRow, endRow) = findSongBounds()
            if (startRow < 0) {
                return RenderResult.Error("Song is empty")
            }

            // 2. Calculate total frames
            val framesPerStep = audioEngine.getFramesPerStep(project.tempo)
            val totalSteps = (endRow - startRow + 1) * 16 * 16  // rows × chain steps × phrase steps
            val totalFrames = totalSteps * framesPerStep

            // 3. Allocate stereo buffer
            val leftChannel = FloatArray(totalFrames.toInt())
            val rightChannel = FloatArray(totalFrames.toInt())

            // 4. Render offline
            renderMessage = "Rendering..."
            renderOffline(startRow, endRow, leftChannel, rightChannel) { progress ->
                renderProgress = progress
            }

            // 5. Generate filename with auto-increment
            val filename = generateFilename(outputDir)

            // 6. Write WAV file
            renderMessage = "Writing file..."
            writeWavFile(filename, leftChannel, rightChannel)

            return RenderResult.Success(filename)

        } catch (e: Exception) {
            return RenderResult.Error(e.message ?: "Unknown error")
        } finally {
            isRendering = false
            renderProgress = 0f
            renderMessage = ""
        }
    }

    private fun findSongBounds(): Pair<Int, Int> {
        var firstUsed = -1
        var lastUsed = -1

        for (row in 0 until 256) {
            val hasContent = project.tracks.any { track ->
                row < track.chainRefs.size && track.chainRefs[row] != 0xFF
            }
            if (hasContent) {
                if (firstUsed < 0) firstUsed = row
                lastUsed = row
            }
        }

        return Pair(firstUsed, lastUsed)
    }

    private fun generateFilename(outputDir: String): String {
        val baseName = project.name.replace(" ", "_")
        var index = 1
        var filename: String

        do {
            filename = "$outputDir/${baseName}_${index.toString().padStart(4, '0')}.wav"
            index++
        } while (fileSystem.fileExists(filename))

        return filename
    }
}

sealed class RenderResult {
    data class Success(val filename: String) : RenderResult()
    data class Error(val message: String) : RenderResult()
}
```

### 6.2 Offline Render Implementation

**Add to C++ (native-audio.cpp):**
```cpp
// Offline rendering function
extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_example_pockettracker_core_audio_OboeAudioBackend_renderFrames(
    JNIEnv *env, jobject thiz,
    jint numFrames
) {
    // Render specified number of frames offline
    // Returns interleaved stereo float array [L0, R0, L1, R1, ...]

    jfloatArray result = env->NewFloatArray(numFrames * 2);
    std::vector<float> buffer(numFrames * 2);

    for (int i = 0; i < numFrames; i++) {
        float left = 0, right = 0;

        // Process all voices
        processVoices(left, right);

        buffer[i * 2] = left;
        buffer[i * 2 + 1] = right;

        globalFrameCounter++;
    }

    env->SetFloatArrayRegion(result, 0, numFrames * 2, buffer.data());
    return result;
}
```

### 6.3 WAV File Writer

**File:** `app/src/main/java/com/example/pockettracker/core/storage/WavWriter.kt`

```kotlin
object WavWriter {
    fun writeWav(
        fileSystem: IFileSystem,
        path: String,
        leftChannel: FloatArray,
        rightChannel: FloatArray,
        sampleRate: Int = 44100,
        bitsPerSample: Int = 16
    ) {
        val numSamples = leftChannel.size
        val numChannels = 2
        val byteRate = sampleRate * numChannels * (bitsPerSample / 8)
        val blockAlign = numChannels * (bitsPerSample / 8)
        val dataSize = numSamples * blockAlign
        val fileSize = 36 + dataSize

        val buffer = ByteBuffer.allocate(44 + dataSize)
        buffer.order(ByteOrder.LITTLE_ENDIAN)

        // RIFF header
        buffer.put("RIFF".toByteArray())
        buffer.putInt(fileSize)
        buffer.put("WAVE".toByteArray())

        // fmt chunk
        buffer.put("fmt ".toByteArray())
        buffer.putInt(16)  // chunk size
        buffer.putShort(1)  // PCM format
        buffer.putShort(numChannels.toShort())
        buffer.putInt(sampleRate)
        buffer.putInt(byteRate)
        buffer.putShort(blockAlign.toShort())
        buffer.putShort(bitsPerSample.toShort())

        // data chunk
        buffer.put("data".toByteArray())
        buffer.putInt(dataSize)

        // Write samples (interleaved)
        for (i in 0 until numSamples) {
            val leftSample = (leftChannel[i].coerceIn(-1f, 1f) * 32767).toInt().toShort()
            val rightSample = (rightChannel[i].coerceIn(-1f, 1f) * 32767).toInt().toShort()
            buffer.putShort(leftSample)
            buffer.putShort(rightSample)
        }

        fileSystem.writeFile(path, buffer.array())
    }
}
```

### 6.4 Update Project Screen UI

**File:** `app/src/main/java/com/example/pockettracker/ui/modules/ProjectModule.kt`

**Add "WAV MIX" to EXPORT row:**
```
PROJECT: MY_SONG
─────────────────────────────────────────
NAME:     MY_SONG
TEMPO:    128
TRANSP:   00

SAVE      LOAD      NEW

EXPORT:   WAV MIX   ---       ---
```

**Input handling:**
- When cursor on "WAV MIX" and A pressed: Start render
- Show progress: "Rendering... 45%"
- On completion: "Saved: MY_SONG_0001.wav"

### Definition of Done - Phase 6
- [ ] RenderController.kt created
- [ ] Offline render function in C++
- [ ] WavWriter.kt writes valid 16-bit stereo WAV
- [ ] "WAV MIX" button visible in Project screen
- [ ] A button triggers render
- [ ] Progress text shows during render
- [ ] Completion message shows filename
- [ ] Renders correct song range (first to last used row)
- [ ] All 8 tracks mixed correctly
- [ ] Pan applied in render
- [ ] Volume chain applied in render
- [ ] Auto-increment prevents overwriting (ProjectName_0001.wav, _0002.wav)
- [ ] Renders directory created if not exists
- [ ] Rendered WAV plays correctly in external player

---

## Phase 7: Integration & Testing (Days 8-10)

### 7.1 Test Cases

**Volume System:**
- [ ] Instrument vol 80, phrase vol FF → output = 50%
- [ ] Instrument vol FF, phrase vol 80 → output = 50%
- [ ] Track vol 80, all else FF → output = 50%
- [ ] Master vol 80, all else FF → output = 50%
- [ ] All volumes at 80 → output = 6.25% (0.5^4)

**Pan System:**
- [ ] Pan 00 = full left (right channel silent)
- [ ] Pan FF = full right (left channel silent)
- [ ] Pan 80 = center (equal L/R)
- [ ] Multiple instruments with different pans mix correctly

**Mixer:**
- [ ] Navigate to Mixer screen from any main screen
- [ ] All 8 track meters visible
- [ ] Master meter visible
- [ ] Meters respond during playback
- [ ] Meters decay when stopped
- [ ] Volume edits affect playback in real-time

**WAV Export:**
- [ ] Empty song shows error
- [ ] Single-phrase song renders correctly
- [ ] Multi-track song renders all tracks
- [ ] Pan positions correct in rendered file
- [ ] Volume levels correct in rendered file
- [ ] File naming increments correctly
- [ ] Render can be cancelled (stretch goal)

### 7.2 Performance Testing

- [ ] Mixer meters don't cause frame drops
- [ ] Render completes in reasonable time (< 30 seconds for typical song)
- [ ] Memory usage acceptable during render

### Definition of Done - Phase 7
- [ ] All test cases pass
- [ ] No audio clicks/pops
- [ ] No UI freezes
- [ ] Tested on Miyoo Flip
- [ ] Tested on Ayaneo (when available)

---

## Summary Timeline

| Day | Phase | Tasks |
|-----|-------|-------|
| 1 | 1, 2 | Data model updates, Instrument VOL/PAN UI |
| 2 | 2, 3 | Finish Instrument UI, Start C++ pan |
| 3 | 3, 4 | Finish pan, Volume chain implementation |
| 4 | 5 | Mixer screen layout and navigation |
| 5 | 5 | Mixer input handling, peak meters (C++) |
| 6 | 5, 6 | Finish Mixer, Start WAV export |
| 7 | 6 | Offline render, WAV writer |
| 8 | 6 | Project screen integration, testing |
| 9 | 7 | Integration testing, bug fixes |
| 10 | 7 | Final testing, polish |

**Buffer:** 4 days for unexpected issues

---

## Files to Create/Modify

### New Files
- `app/src/main/java/com/example/pockettracker/ui/modules/MixerModule.kt`
- `app/src/main/java/com/example/pockettracker/core/logic/RenderController.kt`
- `app/src/main/java/com/example/pockettracker/core/storage/WavWriter.kt`

### Modified Files
- `app/src/main/java/com/example/pockettracker/core/data/TrackerData.kt`
- `app/src/main/java/com/example/pockettracker/ui/modules/InstrumentModule.kt`
- `app/src/main/java/com/example/pockettracker/ui/modules/ProjectModule.kt`
- `app/src/main/java/com/example/pockettracker/core/logic/PlaybackController.kt`
- `app/src/main/java/com/example/pockettracker/platform/android/OboeAudioBackend.kt`
- `app/src/main/cpp/native-audio.cpp`
- `app/src/main/java/com/example/pockettracker/MainActivity.kt` (navigation to Mixer)

---

## Post-MVP Enhancements (Future)

These are explicitly NOT in MVP scope:

**Mixer:**
- Stereo meters per track
- Per-track pan control
- Mute/solo buttons
- Effect sends (reverb, delay, chorus)
- Limiter on master
- EQ on master

**Render:**
- Render menu screen (M8-style)
- Per-track stem export
- Effects wet/dry toggle
- Render range selection
- Real-time render option
- MP3/OGG export

**Instrument:**
- Volume envelope (ADSR)
- Per-instrument effects

---

## How to Use This Document

### Starting a New Claude Code Session

1. Read this document first: `MVP_EXPANSION_PLAN.md`
2. Check current progress against Definition of Done checkboxes
3. Continue from the next incomplete phase
4. Update checkboxes as tasks complete

### Tracking Progress

After completing each phase, mark checkboxes in this document:
```markdown
- [x] Task completed
- [ ] Task pending
```

### Questions to Ask

If unclear on implementation:
1. Which phase are we on?
2. What's the next unchecked item?
3. Are there any blockers?

---

**Document Version:** 1.0
**Created:** 2026-01-23
**Author:** Claude Code + Developer collaboration
