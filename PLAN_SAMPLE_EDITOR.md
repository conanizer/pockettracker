# Sample Editor with Waveform Visualization

## Document Purpose
Implementation plan for adding a full-screen sample editor to PocketTracker with waveform visualization, destructive editing operations, slicing system, and KissFFT-powered spectrum analysis.

**Created:** 2026-02-18
**Target Completion:** ~5-6 weeks
**Prerequisite:** MVP Extension Pack 2 (Tables, HOP/TIC, Pitch Effects) COMPLETE

---

## Overview

### What We're Building

A full-screen sample editor accessible from the instrument screen, inspired by the [M8 tracker's sample editor](https://www.manualslib.com/manual/2290745/Dirtywave-M8.html?page=47) with enhancements from [Renoise's waveform editor](https://tutorials.renoise.com/wiki/Sampler_Waveform) (channel source selection) and our own additions (KissFFT spectrum visualization).

### Key Design Decisions

1. **Destructive editing** — Operations modify the sample buffer directly. Changes are permanent only when saved to storage. All changes are reversible until rendered/saved.
2. **DSP in C++** — All sample processing happens in native code for performance. Kotlin handles UI and file I/O.
3. **Full-screen (640×480)** — Sub-screen from instrument, like FILE_BROWSER. No oscilloscope, no nav map.
4. **Separate from instrument params** — Sample editor start/end define editing regions for crop/save. Instrument screen start/end are playback parameters. They are independent values.
5. **Per-instrument slicing** — Slice markers are stored per-instrument in the project file, not embedded in WAV files.
6. **KissFFT integration** — Used for spectrum visualization and transient detection. Foundation for future audio effects.

### Features Summary

| # | Feature | Complexity | Days |
|---|---------|------------|------|
| 1 | KissFFT Build Integration | Medium | 2-3 |
| 2 | C++ Sample Editor Infrastructure | Medium | 2-3 |
| 3 | Data Model + Controller | Low-Medium | 2 |
| 4 | Waveform Display (SampleEditorModule) | High | 3-4 |
| 5 | Selection System | Medium | 2 |
| 6 | Basic DSP Operations | Medium | 3-4 |
| 7 | File Operations (Save/Overwrite/Rename/Duplicate/Delete) | Medium | 2 |
| 8 | Slice System (Manual + Auto) | High | 4-5 |
| 9 | Slice Playback Mode | Medium | 2-3 |
| 10 | Spectrum View + Loop Visualization | Medium | 2-3 |
| 11 | Integration Testing | Medium | 3-4 |

**Total Estimated:** ~27-35 days (with buffer)

### Architecture Principle
All new features follow existing portable architecture:
- Business logic in `core/` (no Android imports)
- Audio/DSP processing in C++ (native-audio.cpp + new sample-editor.cpp)
- Platform-specific code only in `platform/android/`
- KissFFT compiled as part of native library

---

## Phase 1: KissFFT Build Integration (Days 1-3)

### 1.1 Add KissFFT Source

KissFFT is a lightweight FFT library — single C file + header, no dependencies. BSD license.

**Source:** https://github.com/mborgerding/kissfft

**Files to add:**
```
app/src/main/cpp/
├── kissfft/
│   ├── kiss_fft.c          # Core FFT implementation
│   ├── kiss_fft.h          # Core header
│   ├── kiss_fftr.c         # Real-valued FFT (what we need for audio)
│   ├── kiss_fftr.h         # Real FFT header
│   └── _kiss_fft_guts.h    # Internal header
├── native-audio.cpp        # Existing audio engine
└── sample-editor.cpp       # NEW: sample editing functions
```

### 1.2 Update CMakeLists.txt

**File:** `app/src/main/cpp/CMakeLists.txt`

Add KissFFT sources to the build:
```cmake
add_library(native-audio SHARED
    native-audio.cpp
    sample-editor.cpp          # NEW
    kissfft/kiss_fft.c         # NEW
    kissfft/kiss_fftr.c        # NEW
)
```

### 1.3 Verify Build

- KissFFT compiles without warnings
- Existing native-audio.cpp still works
- sample-editor.cpp skeleton compiles (empty file with includes)

### Definition of Done - Phase 1
- [ ] KissFFT source files added to project
- [ ] CMakeLists.txt updated with new sources
- [ ] sample-editor.cpp skeleton created
- [ ] Project compiles without errors
- [ ] Existing audio engine unaffected

---

## Phase 2: C++ Sample Editor Infrastructure (Days 4-6)

### 2.1 Sample Editor C++ Module

**File:** `app/src/main/cpp/sample-editor.cpp`

This module provides all DSP operations on loaded samples. It accesses the existing `samples[]` and `sampleLengths[]` arrays from native-audio.cpp via extern declarations.

### 2.2 Waveform Decimation (Min/Max)

The core function for waveform display. Returns min/max pairs for each pixel column:

```cpp
// Get waveform overview for display
// Returns numBins pairs of (min, max) values
// Efficient: only one JNI call per frame, ~1200 floats regardless of sample length
extern "C" JNIEXPORT void JNICALL
Java_..._getSampleWaveform(
    JNIEnv *env, jobject thiz,
    jint sampleId,
    jint startFrame,       // View start (based on zoom/scroll)
    jint endFrame,         // View end
    jint numBins,          // Number of pixel columns (typically 600)
    jfloatArray outMinMax  // Output: [min0, max0, min1, max1, ...] size = numBins*2
) {
    if (sampleId < 0 || sampleId >= 256 || !samples[sampleId]) return;

    jfloat* out = env->GetFloatArrayElements(outMinMax, nullptr);
    int length = sampleLengths[sampleId];
    float* data = samples[sampleId];

    // Clamp range
    startFrame = std::max(0, std::min(startFrame, length - 1));
    endFrame = std::max(startFrame + 1, std::min(endFrame, length));

    int framesPerBin = (endFrame - startFrame) / numBins;
    if (framesPerBin < 1) framesPerBin = 1;

    for (int bin = 0; bin < numBins; bin++) {
        int binStart = startFrame + (bin * (endFrame - startFrame)) / numBins;
        int binEnd = startFrame + ((bin + 1) * (endFrame - startFrame)) / numBins;
        binEnd = std::min(binEnd, endFrame);

        float minVal = 1.0f, maxVal = -1.0f;
        for (int i = binStart; i < binEnd; i++) {
            float s = data[i];
            if (s < minVal) minVal = s;
            if (s > maxVal) maxVal = s;
        }

        out[bin * 2] = minVal;
        out[bin * 2 + 1] = maxVal;
    }

    env->ReleaseFloatArrayElements(outMinMax, out, 0);
}
```

### 2.3 Sample Info Functions

```cpp
// Get sample length in frames
extern "C" JNIEXPORT jint JNICALL
Java_..._getSampleLength(JNIEnv *env, jobject thiz, jint sampleId) {
    if (sampleId < 0 || sampleId >= 256 || !samples[sampleId]) return 0;
    return sampleLengths[sampleId];
}

// Get raw sample data for WAV writing
extern "C" JNIEXPORT void JNICALL
Java_..._getSampleData(
    JNIEnv *env, jobject thiz,
    jint sampleId,
    jint startFrame,
    jint length,
    jfloatArray outBuffer
) {
    if (sampleId < 0 || sampleId >= 256 || !samples[sampleId]) return;

    jfloat* out = env->GetFloatArrayElements(outBuffer, nullptr);
    int actualLen = std::min(length, sampleLengths[sampleId] - startFrame);

    for (int i = 0; i < actualLen; i++) {
        out[i] = samples[sampleId][startFrame + i];
    }

    env->ReleaseFloatArrayElements(outBuffer, out, 0);
}
```

### 2.4 Kotlin Interface

**File:** `app/src/main/java/com/example/pockettracker/core/audio/IAudioBackend.kt`

```kotlin
interface IAudioBackend {
    // ... existing methods ...

    // Sample Editor
    fun getSampleWaveform(sampleId: Int, startFrame: Int, endFrame: Int, numBins: Int, outMinMax: FloatArray)
    fun getSampleLength(sampleId: Int): Int
    fun getSampleData(sampleId: Int, startFrame: Int, length: Int, outBuffer: FloatArray)
}
```

### Definition of Done - Phase 2
- [ ] sample-editor.cpp created with waveform decimation function
- [ ] getSampleWaveform() returns correct min/max pairs
- [ ] getSampleLength() returns correct frame count
- [ ] getSampleData() returns raw sample data
- [ ] JNI functions registered in OboeAudioBackend.kt
- [ ] IAudioBackend interface updated
- [ ] Compiles without errors

---

## Phase 3: Data Model + Controller (Days 7-8)

### 3.1 Sample Editor State

**File:** `app/src/main/java/com/example/pockettracker/core/data/SampleEditorState.kt`

```kotlin
/**
 * State for the sample editor screen.
 *
 * Cursor zones (navigated with UP/DOWN):
 *   0 = Waveform (scroll/zoom, place markers)
 *   1 = Operations menu (CROP, SAVE, NORM, etc.)
 *   2 = Config row (SLICE mode, sensitivity, channel source, etc.)
 */
data class SampleEditorState(
    val instrumentId: Int = 0,
    val sampleId: Int = -1,

    // Waveform view
    val viewStartFrame: Int = 0,          // Left edge of visible region
    val viewEndFrame: Int = 0,            // Right edge of visible region
    val totalFrames: Int = 0,             // Total sample length
    val cursorFrame: Int = 0,             // Cursor position in sample
    val zoomLevel: Int = 0,               // 0 = full view, higher = more zoomed

    // Cursor zone
    val cursorZone: Int = 0,              // 0=waveform, 1=operations, 2=config
    val operationIndex: Int = 0,          // Selected operation in ops menu
    val configIndex: Int = 0,             // Selected config parameter

    // Selection
    val selectionStart: Int = -1,         // -1 = no selection
    val selectionEnd: Int = -1,
    val selectionMode: Boolean = false,

    // Channel source (for stereo files)
    val channelSource: ChannelSource = ChannelSource.BOTH,

    // Slice state
    val sliceMode: SliceMode = SliceMode.OFF,
    val sliceSensitivity: Int = 0x40,     // 00-FF for auto modes
    val sliceCount: Int = 8,              // For equal mode

    // Sample info
    val sampleRate: Int = 44100,
    val fileName: String = "",
    val isStereoSource: Boolean = false,  // Original file is stereo

    // Waveform display buffer (updated each frame)
    val waveformMinMax: FloatArray = FloatArray(0)
)

enum class ChannelSource { BOTH, LEFT, RIGHT }

enum class SliceMode { OFF, MANUAL, TRANSIENT, SILENCE, EQUAL }

enum class SampleOperation {
    CROP, SAVE, OVERWRITE, RENAME, DUPLICATE, DELETE,
    NORMALIZE, FADE_IN, FADE_OUT, SILENCE, REVERSE, INVERT, DOWNSAMPLE
}
```

### 3.2 Instrument Data Model Updates

**File:** `app/src/main/java/com/example/pockettracker/core/data/TrackerData.kt`

Add slice data to Instrument:
```kotlin
@Serializable
data class Instrument(
    // ... existing fields ...

    // Slice parameters (NEW)
    var sliceMarkers: MutableList<Int> = mutableListOf(),  // Frame positions, sorted
    var sliceEnabled: Boolean = false                       // When true, notes map to slices
)
```

**Max slice markers:** 128 (matching M8's practical limit, sufficient for any use case)

### 3.3 Sample Editor Controller

**File:** `app/src/main/java/com/example/pockettracker/core/logic/SampleEditorController.kt`

```kotlin
class SampleEditorController(
    private val audioEngine: AudioEngine,
    private val fileSystem: IFileSystem
) {
    // State
    var editorState by mutableStateOf(SampleEditorState())
        private set

    // Waveform display buffer (reused to avoid allocation)
    private val waveformBuffer = FloatArray(WAVEFORM_BINS * 2)  // min/max pairs

    companion object {
        const val WAVEFORM_BINS = 600        // Pixel columns for waveform
        const val MAX_ZOOM_LEVELS = 16       // Number of zoom steps
        const val MAX_SLICE_MARKERS = 128
    }

    // Enter sample editor from instrument screen
    fun enterEditor(project: Project, instrumentId: Int) {
        val instrument = project.instruments[instrumentId]
        val sampleLength = audioEngine.backend.getSampleLength(instrument.sampleId)

        editorState = SampleEditorState(
            instrumentId = instrumentId,
            sampleId = instrument.sampleId,
            totalFrames = sampleLength,
            viewStartFrame = 0,
            viewEndFrame = sampleLength,
            cursorFrame = 0,
            fileName = instrument.sampleFilePath?.substringAfterLast('/') ?: "---",
            sampleRate = 44100  // TODO: store sample rate in instrument
        )
    }

    // Update waveform display data (called each frame)
    fun updateWaveform() { ... }

    // Navigation
    fun scrollLeft() { ... }
    fun scrollRight() { ... }
    fun zoomIn() { ... }
    fun zoomOut() { ... }
    fun moveCursorZone(direction: Int) { ... }  // UP/DOWN between zones

    // Selection
    fun enterSelectionMode() { ... }
    fun expandSelection(direction: Int) { ... }
    fun clearSelection() { ... }

    // Operations (dispatched from operations menu)
    fun executeOperation(op: SampleOperation) { ... }

    // Slice operations
    fun placeSliceMarker(frame: Int) { ... }
    fun removeSliceMarker(index: Int) { ... }
    fun autoSliceTransients(sensitivity: Int) { ... }
    fun autoSliceSilence(threshold: Int) { ... }
    fun autoSliceEqual(count: Int) { ... }
}
```

### 3.4 Entry Point from Instrument Screen

**Instrument screen integration:**
- Add "EDIT" button/row to instrument screen (exact row placement to be determined during implementation — will require rearranging existing rows to fit)
- When cursor is on EDIT and A is pressed: if sample is loaded → enter sample editor; if no sample → show status message "NO SAMPLE"
- B from sample editor → return to instrument screen

**ScreenType addition:**
```kotlin
enum class ScreenType(...) {
    // ... existing ...
    SAMPLE_EDITOR("SAMPLE EDITOR", "SE")  // Popup screen like FILE_BROWSER
}
```

### Definition of Done - Phase 3
- [ ] SampleEditorState data class created
- [ ] Instrument data model updated with sliceMarkers and sliceEnabled
- [ ] SampleEditorController skeleton created
- [ ] SAMPLE_EDITOR added to ScreenType
- [ ] EDIT entry point added to instrument screen (row placement TBD)
- [ ] B exits sample editor back to instrument screen
- [ ] Existing projects still load (new fields have defaults)
- [ ] Compiles without errors

---

## Phase 4: Waveform Display — SampleEditorModule (Days 9-12)

### 4.1 Full-Screen Layout (640×480)

The sample editor takes the full 640×480 canvas, drawn at (0, 0) like FILE_BROWSER.

```
Y=0   ┌──────────────────────────────────────────────────┐
      │ SAMPLE EDITOR                 44100Hz M 00:02.34 │ Header (21px)
      │ kick_808.wav                            I:0A S:0A│ Info (21px)
Y=42  ├──────────────────────────────────────────────────┤
      │                                                   │
      │                                                   │
      │            WAVEFORM DISPLAY                       │ Waveform (280px)
      │            (600px wide × 280px tall)              │
      │                                                   │
      │   S▕  ▕  ▕  ▕  ▕  ▕  ▕  ▕                    E  │ Slice markers
      │                                                   │
Y=322 ├──────────────────────────────────────────────────┤
      │ ZOOM ████████░░░░░░░  x4    POS 04A0/2F40       │ Zoom bar (21px)
Y=343 ├──────────────────────────────────────────────────┤
      │ SEL 0000A000-0000F000  LEN 00005000              │ Selection info (21px)
Y=364 ├──────────────────────────────────────────────────┤
      │>CROP SAVE OVWR RENM DUPL DEL                     │ Operations row 1 (21px)
      │ NORM FDE+ FDE- SLNC REV  INV  DNSM              │ Operations row 2 (21px)
Y=406 ├──────────────────────────────────────────────────┤
      │ SLC:MAN  CNT:08  SENS:40  SRC:BOTH              │ Config row (21px)
Y=427 ├──────────────────────────────────────────────────┤
      │ 00▕01▕02▕03▕04▕05▕06▕07▕                        │ Slice overview (21px)
Y=448 ├──────────────────────────────────────────────────┤
      │ SPECTRUM ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░│ Spectrum (32px)
Y=480 └──────────────────────────────────────────────────┘
```

**Layout elements:**
| Element | Y Position | Height | Content |
|---------|-----------|--------|---------|
| Header | 0 | 21px | "SAMPLE EDITOR", sample rate, mono/stereo, duration |
| Info | 21 | 21px | Filename, instrument ID (I:XX), sample ID (S:XX) |
| Waveform | 42 | 280px | Min/max waveform, cursor, selection, markers |
| Zoom bar | 322 | 21px | Visual zoom level + position indicator |
| Selection | 343 | 21px | Selection start/end/length in hex |
| Operations 1 | 364 | 21px | CROP SAVE OVWR RENM DUPL DEL |
| Operations 2 | 385 | 21px | NORM FDE+ FDE- SLNC REV INV DNSM |
| Config | 406 | 21px | Slice mode, count, sensitivity, channel source |
| Slice view | 427 | 21px | Slice marker overview (numbered) |
| Spectrum | 448 | 32px | KissFFT spectrum of visible region |

### 4.2 Waveform Rendering

```kotlin
class SampleEditorModule : TrackerModule {
    override val width = 640
    override val height = 480

    companion object {
        const val WAVEFORM_X = 20           // Left margin
        const val WAVEFORM_WIDTH = 600      // Pixel columns for waveform
        const val WAVEFORM_Y = 42           // Top of waveform area
        const val WAVEFORM_HEIGHT = 280     // Height of waveform area
        const val WAVEFORM_CENTER = WAVEFORM_Y + WAVEFORM_HEIGHT / 2  // 0dB line
    }

    fun drawWaveform(drawScope: DrawScope, state: SampleEditorState, scale: Int) {
        val minMax = state.waveformMinMax
        if (minMax.isEmpty()) return

        val bins = minMax.size / 2
        val centerY = WAVEFORM_CENTER * scale
        val halfHeight = (WAVEFORM_HEIGHT / 2) * scale

        for (i in 0 until bins) {
            val minVal = minMax[i * 2]
            val maxVal = minMax[i * 2 + 1]
            val x = (WAVEFORM_X + i) * scale

            // Draw vertical line from min to max (waveform envelope)
            val yTop = centerY - (maxVal * halfHeight).toInt()
            val yBottom = centerY - (minVal * halfHeight).toInt()

            drawLine(
                color = Color.Green,
                start = Offset(x.toFloat(), yTop.toFloat()),
                end = Offset(x.toFloat(), yBottom.toFloat()),
                strokeWidth = scale.toFloat()
            )
        }

        // Draw center line (0dB reference)
        drawLine(
            color = Color(0xFF333333),
            start = Offset((WAVEFORM_X * scale).toFloat(), centerY.toFloat()),
            end = Offset(((WAVEFORM_X + WAVEFORM_WIDTH) * scale).toFloat(), centerY.toFloat())
        )
    }
}
```

### 4.3 Zoom System

```kotlin
// Zoom levels: each step halves the visible range
// Level 0 = full sample visible
// Level 1 = half the sample
// Level N = sample_length / 2^N
fun calculateViewRange(totalFrames: Int, zoomLevel: Int, centerFrame: Int): Pair<Int, Int> {
    val visibleFrames = totalFrames shr zoomLevel  // totalFrames / 2^zoom
    val minVisible = 64  // Never zoom below 64 frames visible
    val actualVisible = maxOf(visibleFrames, minVisible)

    val halfVisible = actualVisible / 2
    val start = (centerFrame - halfVisible).coerceIn(0, totalFrames - actualVisible)
    val end = start + actualVisible

    return Pair(start, end.coerceAtMost(totalFrames))
}
```

**Zoom controls:**
- A+UP: Zoom in (center on cursor)
- A+DOWN: Zoom out
- LEFT/RIGHT: Scroll waveform (when in waveform zone)
- A+LEFT/RIGHT: Fine cursor movement

### 4.4 Cursor Zones Navigation

UP/DOWN moves between zones:
```
Zone 0: Waveform     ← LEFT/RIGHT scrolls, A+DPAD for zoom/cursor
Zone 1: Operations   ← LEFT/RIGHT selects operation, A activates
Zone 2: Config       ← LEFT/RIGHT selects param, A+UP/DOWN changes value
```

### Definition of Done - Phase 4
- [ ] SampleEditorModule.kt created (640×480 full screen)
- [ ] Waveform displays correctly using min/max decimation
- [ ] Zoom in/out works (A+UP/DOWN)
- [ ] Scroll works (LEFT/RIGHT in waveform zone)
- [ ] Cursor position shown on waveform (vertical line)
- [ ] Zone navigation works (UP/DOWN between waveform/ops/config)
- [ ] Header shows filename, sample rate, mono/stereo, duration
- [ ] Zoom bar shows current zoom level visually
- [ ] PixelPerfectRenderer routes SAMPLE_EDITOR to new module
- [ ] Entering from instrument screen shows correct sample

---

## Phase 5: Selection System (Days 13-14)

### 5.1 Selection in Waveform Zone

Reuse existing selection mode mechanic adapted for 1D waveform range:

**Controls:**
- L+B: Enter selection mode (places start at cursor)
- LEFT/RIGHT (in selection mode): Expand/contract selection end
- B: Confirm selection and exit selection mode (keep selection highlighted)
- L+R: Cancel selection

### 5.2 Selection Display

```kotlin
fun drawSelection(drawScope: DrawScope, state: SampleEditorState, scale: Int) {
    if (state.selectionStart < 0 || state.selectionEnd < 0) return

    // Convert frame positions to pixel positions
    val startPx = frameToPixel(state.selectionStart, state)
    val endPx = frameToPixel(state.selectionEnd, state)

    // Draw selection highlight (semi-transparent overlay)
    drawRect(
        color = Color(0x4400FF00),  // Semi-transparent green
        topLeft = Offset(startPx, (WAVEFORM_Y * scale).toFloat()),
        size = Size((endPx - startPx), (WAVEFORM_HEIGHT * scale).toFloat())
    )

    // Draw selection boundaries (bright vertical lines)
    drawLine(color = Color.Green, ...)  // Start marker
    drawLine(color = Color.Green, ...)  // End marker
}
```

### 5.3 Selection Info Row

Shows `SEL 0000A000-0000F000 LEN 00005000` with 8-digit hex frame positions.

When no selection: `SEL --------  --------  LEN --------`

### Definition of Done - Phase 5
- [ ] L+B enters selection mode in waveform zone
- [ ] LEFT/RIGHT expands selection in selection mode
- [ ] Selection highlighted with semi-transparent overlay
- [ ] Selection start/end shown as bright boundary lines
- [ ] Selection info row displays hex frame positions
- [ ] B confirms selection, L+R cancels
- [ ] Operations can target selection (or whole sample if no selection)

---

## Phase 6: Basic DSP Operations (Days 15-18)

### 6.1 C++ DSP Functions

**File:** `app/src/main/cpp/sample-editor.cpp`

All operations work on a range (startFrame to endFrame). If called with full sample range, they affect the entire sample.

```cpp
// =============================
// CROP - Remove everything outside selection
// =============================
extern "C" JNIEXPORT void JNICALL
Java_..._cropSample(JNIEnv *env, jobject thiz,
    jint sampleId, jint startFrame, jint endFrame
) {
    if (!validSample(sampleId)) return;

    int newLength = endFrame - startFrame;
    float* newData = new float[newLength];

    for (int i = 0; i < newLength; i++) {
        newData[i] = samples[sampleId][startFrame + i];
    }

    delete[] samples[sampleId];
    samples[sampleId] = newData;
    sampleLengths[sampleId] = newLength;
}

// =============================
// NORMALIZE - Scale to max amplitude
// =============================
extern "C" JNIEXPORT void JNICALL
Java_..._normalizeSample(JNIEnv *env, jobject thiz,
    jint sampleId, jint startFrame, jint endFrame
) {
    if (!validSample(sampleId)) return;

    float peak = 0.0f;
    for (int i = startFrame; i < endFrame; i++) {
        float abs = fabsf(samples[sampleId][i]);
        if (abs > peak) peak = abs;
    }

    if (peak < 0.0001f || peak >= 0.999f) return;  // Already normalized or silent

    float gain = 1.0f / peak;
    for (int i = startFrame; i < endFrame; i++) {
        samples[sampleId][i] *= gain;
    }
}

// =============================
// FADE IN - Cosine fade from silence
// =============================
extern "C" JNIEXPORT void JNICALL
Java_..._fadeSample(JNIEnv *env, jobject thiz,
    jint sampleId, jint startFrame, jint endFrame, jboolean fadeIn
) {
    if (!validSample(sampleId)) return;

    int length = endFrame - startFrame;
    for (int i = 0; i < length; i++) {
        // Cosine fade for smooth curve
        float t = (float)i / (float)(length - 1);
        float gain;
        if (fadeIn) {
            gain = 0.5f * (1.0f - cosf(M_PI * t));  // 0→1
        } else {
            gain = 0.5f * (1.0f + cosf(M_PI * t));  // 1→0
        }
        samples[sampleId][startFrame + i] *= gain;
    }
}

// =============================
// SILENCE - Zero out region
// =============================
extern "C" JNIEXPORT void JNICALL
Java_..._silenceSample(JNIEnv *env, jobject thiz,
    jint sampleId, jint startFrame, jint endFrame
) {
    if (!validSample(sampleId)) return;

    for (int i = startFrame; i < endFrame; i++) {
        samples[sampleId][i] = 0.0f;
    }
}

// =============================
// REVERSE - Reverse region in-place
// =============================
extern "C" JNIEXPORT void JNICALL
Java_..._reverseSample(JNIEnv *env, jobject thiz,
    jint sampleId, jint startFrame, jint endFrame
) {
    if (!validSample(sampleId)) return;

    int left = startFrame;
    int right = endFrame - 1;
    while (left < right) {
        float temp = samples[sampleId][left];
        samples[sampleId][left] = samples[sampleId][right];
        samples[sampleId][right] = temp;
        left++;
        right--;
    }
}

// =============================
// INVERT - Phase invert (multiply by -1)
// =============================
extern "C" JNIEXPORT void JNICALL
Java_..._invertSample(JNIEnv *env, jobject thiz,
    jint sampleId, jint startFrame, jint endFrame
) {
    if (!validSample(sampleId)) return;

    for (int i = startFrame; i < endFrame; i++) {
        samples[sampleId][i] = -samples[sampleId][i];
    }
}

// =============================
// DOWNSAMPLE - Reduce sample rate by factor
// =============================
extern "C" JNIEXPORT void JNICALL
Java_..._downsampleSample(JNIEnv *env, jobject thiz,
    jint sampleId, jint factor  // 2, 4, 8, etc.
) {
    if (!validSample(sampleId) || factor < 2) return;

    int oldLength = sampleLengths[sampleId];
    int newLength = oldLength / factor;
    float* newData = new float[newLength];

    // Simple decimation (take every Nth sample)
    // For better quality, could add anti-aliasing filter first
    for (int i = 0; i < newLength; i++) {
        newData[i] = samples[sampleId][i * factor];
    }

    delete[] samples[sampleId];
    samples[sampleId] = newData;
    sampleLengths[sampleId] = newLength;
}
```

### 6.2 Channel Source Selection

Channel source is handled at load time. When a stereo WAV is loaded, the user can switch between L/R/Both views. The selected channel is used when any operation is performed or when saving.

**Implementation approach:**
- Store both channels in memory when a stereo file is loaded (separate from the main mono sample buffer)
- `channelSource` parameter determines which channel feeds the working buffer
- Switching channels reloads the working buffer from the stored stereo data
- Once the user saves/crops/processes, the result is mono (from the selected channel)

```cpp
// Stereo buffer storage (only used during sample editor session)
float* stereoLeft[256] = {nullptr};
float* stereoRight[256] = {nullptr};
int stereoLengths[256] = {0};

extern "C" JNIEXPORT void JNICALL
Java_..._loadStereoSample(JNIEnv *env, jobject thiz,
    jint sampleId, jfloatArray leftChannel, jfloatArray rightChannel, jint length
) {
    // Store both channels for sample editor switching
    ...
}

extern "C" JNIEXPORT void JNICALL
Java_..._selectChannel(JNIEnv *env, jobject thiz,
    jint sampleId, jint channel  // 0=both(mix), 1=left, 2=right
) {
    // Copy selected channel into main samples[] buffer
    ...
}
```

### 6.3 Operations Menu UI

The operations menu is a horizontal list in two rows. Cursor highlights the current operation. A activates it.

Operations that require a selection (CROP, FADE IN/OUT, SILENCE, REVERSE, INVERT) show "NO SEL" status if no selection exists and operate on the full sample.

### 6.4 Kotlin Interface Updates

**File:** `IAudioBackend.kt`
```kotlin
interface IAudioBackend {
    // ... existing ...

    // Sample Editor DSP Operations
    fun cropSample(sampleId: Int, startFrame: Int, endFrame: Int)
    fun normalizeSample(sampleId: Int, startFrame: Int, endFrame: Int)
    fun fadeSample(sampleId: Int, startFrame: Int, endFrame: Int, fadeIn: Boolean)
    fun silenceSample(sampleId: Int, startFrame: Int, endFrame: Int)
    fun reverseSample(sampleId: Int, startFrame: Int, endFrame: Int)
    fun invertSample(sampleId: Int, startFrame: Int, endFrame: Int)
    fun downsampleSample(sampleId: Int, factor: Int)
    fun loadStereoSample(sampleId: Int, left: FloatArray, right: FloatArray, length: Int)
    fun selectChannel(sampleId: Int, channel: Int)
}
```

### Definition of Done - Phase 6
- [ ] All 7 DSP operations implemented in C++ (crop, normalize, fade, silence, reverse, invert, downsample)
- [ ] Operations menu navigable with LEFT/RIGHT
- [ ] A activates selected operation
- [ ] Operations work on selection (or full sample if no selection)
- [ ] Waveform updates immediately after operation
- [ ] Channel source switching works for stereo files
- [ ] Status messages shown after operations ("NORMALIZED", "CROPPED", etc.)
- [ ] No crashes on edge cases (empty selection, 0-length sample)

---

## Phase 7: File Operations (Days 19-20)

### 7.1 Save (Write New WAV)

Reuse WAV writing logic from existing RenderController/WavWriter.

```kotlin
fun saveSample(project: Project, instrumentId: Int, fileName: String) {
    val instrument = project.instruments[instrumentId]
    val sampleLength = audioEngine.backend.getSampleLength(instrument.sampleId)

    // Get sample data from C++
    val buffer = FloatArray(sampleLength)
    audioEngine.backend.getSampleData(instrument.sampleId, 0, sampleLength, buffer)

    // Write WAV file
    val samplesDir = fileSystem.getSamplesDirectory()
    val filePath = "$samplesDir/$fileName.wav"
    WavWriter.writeMonoWav(filePath, buffer, 44100)

    // Update instrument reference
    instrument.sampleFilePath = filePath
}
```

### 7.2 Overwrite

Save back to the same file path (`instrument.sampleFilePath`).

### 7.3 Rename

```kotlin
fun renameSample(project: Project, instrumentId: Int, newName: String) {
    val instrument = project.instruments[instrumentId]
    val oldPath = instrument.sampleFilePath ?: return
    val dir = oldPath.substringBeforeLast('/')
    val newPath = "$dir/$newName.wav"

    fileSystem.renameFile(oldPath, newPath)
    instrument.sampleFilePath = newPath
}
```

### 7.4 Duplicate

Copy sample to a new instrument slot:
```kotlin
fun duplicateSample(project: Project, sourceInstrumentId: Int, targetInstrumentId: Int) {
    val source = project.instruments[sourceInstrumentId]
    val sampleLength = audioEngine.backend.getSampleLength(source.sampleId)

    // Get sample data
    val buffer = FloatArray(sampleLength)
    audioEngine.backend.getSampleData(source.sampleId, 0, sampleLength, buffer)

    // Load into new slot
    audioEngine.backend.loadSample(targetInstrumentId, buffer)

    // Copy instrument params
    project.instruments[targetInstrumentId].apply {
        sampleId = targetInstrumentId
        sampleFilePath = source.sampleFilePath  // Points to same file until saved separately
        root = source.root
        detune = source.detune
        // ... other relevant params
    }
}
```

### 7.5 Delete

Remove sample from instrument slot and optionally delete the WAV file from storage.

### 7.6 Rename UI

For rename, a simple text entry screen similar to project name editing:
- Shows current name with character cursor
- A+LEFT/RIGHT cycles characters
- A+UP/DOWN changes character value
- B confirms

### Definition of Done - Phase 7
- [ ] Save writes new WAV file to Samples directory
- [ ] Overwrite saves to existing file path
- [ ] Rename changes file name on disk
- [ ] Duplicate copies sample buffer to new instrument slot
- [ ] Delete removes sample from instrument (with confirmation)
- [ ] Status messages shown for all operations
- [ ] File paths updated in instrument data model

---

## Phase 8: Slice System (Days 21-25)

### 8.1 Slice Marker Data

Slice markers are stored per-instrument as sorted frame positions:

```kotlin
// In Instrument data class
var sliceMarkers: MutableList<Int> = mutableListOf()  // Up to 128 frame positions
var sliceEnabled: Boolean = false
```

### 8.2 Manual Slice Markers

**Controls (in waveform zone):**
- SELECT+A: Place marker at cursor position
- SELECT+B: Remove nearest marker to cursor
- During playback preview: SELECT+A drops marker at playhead ("lazy chop")

```kotlin
fun placeSliceMarker(frame: Int, instrument: Instrument) {
    if (instrument.sliceMarkers.size >= MAX_SLICE_MARKERS) return

    // Insert sorted
    val insertIndex = instrument.sliceMarkers.indexOfFirst { it > frame }
    if (insertIndex < 0) {
        instrument.sliceMarkers.add(frame)
    } else {
        instrument.sliceMarkers.add(insertIndex, frame)
    }
}
```

### 8.3 Auto-Slice by Transients (KissFFT)

Uses spectral flux for onset detection — more accurate than simple amplitude envelope:

```cpp
// Transient detection using spectral flux
// 1. Compute STFT with overlapping windows
// 2. Calculate spectral flux (sum of positive frequency bin differences)
// 3. Apply threshold based on sensitivity
// 4. Return frame positions of detected onsets

extern "C" JNIEXPORT jint JNICALL
Java_..._detectTransients(
    JNIEnv *env, jobject thiz,
    jint sampleId,
    jint sensitivity,        // 0x00-0xFF (low=few markers, high=many)
    jintArray outMarkers,    // Output: frame positions
    jint maxMarkers          // Max markers to return (128)
) {
    if (!validSample(sampleId)) return 0;

    const int FFT_SIZE = 1024;
    const int HOP_SIZE = 256;

    kiss_fftr_cfg cfg = kiss_fftr_alloc(FFT_SIZE, 0, nullptr, nullptr);

    float* data = samples[sampleId];
    int length = sampleLengths[sampleId];
    int numFrames = (length - FFT_SIZE) / HOP_SIZE;

    // Compute spectral flux
    std::vector<float> flux(numFrames, 0.0f);
    std::vector<kiss_fft_cpx> prevSpectrum(FFT_SIZE / 2 + 1);
    std::vector<kiss_fft_cpx> currSpectrum(FFT_SIZE / 2 + 1);
    float window[FFT_SIZE];

    // Hann window
    for (int i = 0; i < FFT_SIZE; i++) {
        window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / FFT_SIZE));
    }

    float timeInput[FFT_SIZE];

    for (int f = 0; f < numFrames; f++) {
        int offset = f * HOP_SIZE;

        // Window the input
        for (int i = 0; i < FFT_SIZE; i++) {
            timeInput[i] = data[offset + i] * window[i];
        }

        // FFT
        kiss_fftr(cfg, timeInput, currSpectrum.data());

        if (f > 0) {
            // Spectral flux = sum of positive magnitude differences
            float sum = 0.0f;
            for (int b = 0; b < FFT_SIZE / 2 + 1; b++) {
                float prevMag = sqrtf(prevSpectrum[b].r * prevSpectrum[b].r +
                                      prevSpectrum[b].i * prevSpectrum[b].i);
                float currMag = sqrtf(currSpectrum[b].r * currSpectrum[b].r +
                                      currSpectrum[b].i * currSpectrum[b].i);
                float diff = currMag - prevMag;
                if (diff > 0) sum += diff;
            }
            flux[f] = sum;
        }

        std::swap(prevSpectrum, currSpectrum);
    }

    // Adaptive threshold: median flux * sensitivity factor
    // sensitivity 0x00 = very high threshold (few markers)
    // sensitivity 0xFF = very low threshold (many markers)
    float thresholdFactor = 2.0f - (sensitivity / 255.0f) * 1.8f;  // 2.0 → 0.2

    // Find peaks above threshold
    // ... peak picking with minimum distance between onsets ...

    kiss_fftr_free(cfg);
    return markerCount;
}
```

### 8.4 Auto-Slice by Silence

```cpp
// Detect silence boundaries
// Places markers at transitions from silence to sound
extern "C" JNIEXPORT jint JNICALL
Java_..._detectSilence(
    JNIEnv *env, jobject thiz,
    jint sampleId,
    jint sensitivity,        // Threshold: higher = more sensitive
    jintArray outMarkers,
    jint maxMarkers
) {
    if (!validSample(sampleId)) return 0;

    float* data = samples[sampleId];
    int length = sampleLengths[sampleId];

    // Silence threshold: sensitivity maps to amplitude
    float threshold = 0.001f + (1.0f - sensitivity / 255.0f) * 0.05f;

    // Compute RMS energy in sliding windows
    const int WINDOW = 512;
    bool wasSilent = true;
    int markerCount = 0;

    jint* markers = env->GetIntArrayElements(outMarkers, nullptr);

    for (int i = 0; i < length - WINDOW; i += WINDOW / 4) {
        float rms = 0.0f;
        for (int j = 0; j < WINDOW && (i + j) < length; j++) {
            rms += data[i + j] * data[i + j];
        }
        rms = sqrtf(rms / WINDOW);

        bool isSilent = rms < threshold;

        // Transition from silence → sound = onset marker
        if (wasSilent && !isSilent) {
            if (markerCount < maxMarkers) {
                markers[markerCount++] = i;
            }
        }
        wasSilent = isSilent;
    }

    env->ReleaseIntArrayElements(outMarkers, markers, 0);
    return markerCount;
}
```

### 8.5 Auto-Slice Equal Division

Simple — no C++ needed, done in Kotlin:
```kotlin
fun autoSliceEqual(count: Int, instrument: Instrument, totalFrames: Int) {
    instrument.sliceMarkers.clear()

    for (i in 0 until count) {
        val frame = (i.toLong() * totalFrames / count).toInt()
        instrument.sliceMarkers.add(frame)
    }
}
```

### 8.6 Slice Marker Display on Waveform

Markers shown as numbered vertical lines on the waveform:
- Color: Cyan (distinct from green waveform and green selection)
- Label: 2-digit hex (00-7F) drawn at top of marker line
- Current/selected marker highlighted in white

### 8.7 Config Row UI

The config row at the bottom of the screen:

```
SLC:MAN  CNT:08  SENS:40  SRC:BOTH
```

| Parameter | Values | Description |
|-----------|--------|-------------|
| SLC | OFF/MAN/TRN/SIL/EQL | Slice mode |
| CNT | 02-80 (hex) | Marker count (for equal mode) / current count (for others) |
| SENS | 00-FF | Sensitivity (for TRN and SIL modes) |
| SRC | BOTH/LEFT/RGHT | Channel source |

Changing SLC mode to TRN/SIL/EQL auto-generates markers immediately. Changing SENS recalculates markers in real-time for TRN/SIL modes.

### Definition of Done - Phase 8
- [ ] Manual slice marker placement (SELECT+A)
- [ ] Manual slice marker removal (SELECT+B)
- [ ] "Lazy chop" during playback (SELECT+A at playhead)
- [ ] Auto-slice by transients using KissFFT spectral flux
- [ ] Auto-slice by silence using RMS energy
- [ ] Auto-slice by equal division
- [ ] Sensitivity parameter affects transient/silence detection
- [ ] Markers displayed as numbered vertical lines on waveform
- [ ] Config row shows slice mode, count, sensitivity, channel source
- [ ] Up to 128 slice markers per instrument
- [ ] Markers stored in Instrument data model (persisted with project)

---

## Phase 9: Slice Playback Mode (Days 26-28)

### 9.1 Instrument Slice Parameter

When `sliceEnabled = true` on an instrument, note input maps to slices:

- C-1 = slice 00 (first marker → second marker)
- C#1 = slice 01
- D-1 = slice 02
- ...up to B-9 (covering 128 slices across MIDI range)

The slice start frame overrides the instrument's start point. The slice end frame (next marker position, or sample end for last slice) overrides the instrument's end point.

### 9.2 Playback Integration

**File:** `app/src/main/java/com/example/pockettracker/core/logic/PlaybackController.kt`

```kotlin
fun scheduleNoteForInstrument(
    frame: Long,
    note: Note,
    instrument: Instrument,
    trackId: Int,
    volume: Float,
    pan: Float
) {
    if (instrument.sliceEnabled && instrument.sliceMarkers.isNotEmpty()) {
        // Map note to slice index
        val sliceIndex = note.toMidi() - Note.fromString("C-1").toMidi()
        val clampedIndex = sliceIndex.coerceIn(0, instrument.sliceMarkers.size - 1)

        // Get slice boundaries
        val sliceStart = instrument.sliceMarkers[clampedIndex]
        val sliceEnd = if (clampedIndex + 1 < instrument.sliceMarkers.size) {
            instrument.sliceMarkers[clampedIndex + 1]
        } else {
            audioEngine.backend.getSampleLength(instrument.sampleId)
        }

        // Schedule with slice start/end overrides
        audioEngine.backend.scheduleNoteWithSlice(
            frame, instrument.sampleId, trackId,
            instrument.root.toFrequency(),  // Play at original pitch
            instrument.root.toFrequency(),  // Base freq = root (no pitch change)
            volume, pan,
            sliceStart, sliceEnd
        )
    } else {
        // Normal note scheduling (existing code)
        scheduleNote(...)
    }
}
```

### 9.3 C++ Slice Support

```cpp
// New JNI function: schedule note with explicit start/end frame overrides
extern "C" JNIEXPORT void JNICALL
Java_..._scheduleNoteWithSlice(
    JNIEnv *env, jobject thiz,
    jlong frame, jint sampleId, jint trackId,
    jfloat freq, jfloat baseFreq, jfloat vol, jfloat pan,
    jint sliceStartFrame, jint sliceEndFrame
) {
    // Similar to scheduleNote but uses absolute frame positions
    // instead of normalized 0-255 start/end points
    ...
}
```

### 9.4 Instrument Screen Display

When sliceEnabled is true, show "SLICE ON" in the instrument screen. The PLAY mode could show "SLC" as an option alongside FWD/REV/LOOP.

### Definition of Done - Phase 9
- [ ] sliceEnabled parameter in Instrument data model
- [ ] Note-to-slice mapping works (C-1 = slice 00, etc.)
- [ ] Slice start/end override instrument start/end during playback
- [ ] scheduleNoteWithSlice() C++ function works
- [ ] Sliced playback sounds correct (each note plays different region)
- [ ] Instrument screen shows slice state
- [ ] Works with all playback modes (phrase, chain, song)

---

## Phase 10: Spectrum View + Loop Visualization (Days 29-31)

### 10.1 KissFFT Spectrum Computation

```cpp
// Compute spectrum of visible region for display
extern "C" JNIEXPORT void JNICALL
Java_..._getSampleSpectrum(
    JNIEnv *env, jobject thiz,
    jint sampleId,
    jint centerFrame,        // Center of analysis window
    jint fftSize,            // 1024 or 2048
    jint numBins,            // Output bins (e.g., 600 for display width)
    jfloatArray outBins      // Output: magnitude per bin (log scale)
) {
    if (!validSample(sampleId)) return;

    kiss_fftr_cfg cfg = kiss_fftr_alloc(fftSize, 0, nullptr, nullptr);

    float* data = samples[sampleId];
    int length = sampleLengths[sampleId];
    int startFrame = std::max(0, centerFrame - fftSize / 2);

    // Window and FFT
    float timeInput[fftSize];
    kiss_fft_cpx freqOutput[fftSize / 2 + 1];

    for (int i = 0; i < fftSize; i++) {
        int idx = startFrame + i;
        float window = 0.5f * (1.0f - cosf(2.0f * M_PI * i / fftSize));
        timeInput[i] = (idx < length) ? data[idx] * window : 0.0f;
    }

    kiss_fftr(cfg, timeInput, freqOutput);

    // Convert to log-scale magnitude bins
    jfloat* out = env->GetFloatArrayElements(outBins, nullptr);

    int spectrumBins = fftSize / 2 + 1;
    for (int b = 0; b < numBins; b++) {
        // Map display bin to frequency bin (log scale)
        float logPos = powf((float)b / numBins, 2.0f) * spectrumBins;
        int freqBin = (int)logPos;
        freqBin = std::min(freqBin, spectrumBins - 1);

        float mag = sqrtf(freqOutput[freqBin].r * freqOutput[freqBin].r +
                          freqOutput[freqBin].i * freqOutput[freqBin].i);

        // Convert to dB, normalize to 0.0-1.0 range
        float db = 20.0f * log10f(std::max(mag, 0.0001f));
        out[b] = std::max(0.0f, (db + 80.0f) / 80.0f);  // -80dB → 0dB mapped to 0→1
    }

    env->ReleaseFloatArrayElements(outBins, out, 0);
    kiss_fftr_free(cfg);
}
```

### 10.2 Spectrum Display

The spectrum bar at the bottom of the screen (32px tall, 600px wide):
- Shows frequency content of the visible region (or around cursor)
- Log-scale frequency axis (low freqs on left, high on right)
- Color: Cyan bars on dark background
- Updates when cursor moves or view scrolls

**Toggle:** Press SELECT in waveform zone to toggle spectrum on/off. When off, the space can show additional info or simply be empty.

### 10.3 Loop Marker Visualization

Display the instrument's loop points on the waveform as visual reference:
- Loop start: Yellow vertical dashed line
- Loop end: Yellow vertical dashed line (calculated from loopStart + region)
- These are read from the current instrument's params
- Visually distinct from slice markers (cyan) and selection (green)

**Note:** Loop markers are displayed for reference. Whether they should be editable from the sample editor (updating the instrument's loopStart/loopMode params) can be decided during implementation based on UX testing.

### Definition of Done - Phase 10
- [ ] KissFFT spectrum computation works
- [ ] Spectrum bar displays at bottom of screen
- [ ] Log-scale frequency axis
- [ ] SELECT toggles spectrum on/off
- [ ] Loop markers shown on waveform (yellow dashed lines)
- [ ] Loop markers correspond to instrument's loop parameters
- [ ] Spectrum updates when scrolling/zooming

---

## Phase 11: Integration Testing (Days 32-35)

### 11.1 Sample Editor Core Tests

| Test | Steps | Expected Result |
|------|-------|-----------------|
| Enter editor | Load sample in INST, navigate to EDIT, press A | Sample editor opens with waveform |
| Exit editor | Press B | Returns to instrument screen |
| No sample | Press EDIT with no sample loaded | Status: "NO SAMPLE" |
| Zoom in | A+UP multiple times | Waveform zooms in, detail visible |
| Zoom out | A+DOWN | Waveform zooms out to full view |
| Scroll | LEFT/RIGHT when zoomed | View scrolls through sample |
| Cursor | A+LEFT/RIGHT | Cursor moves, position shown |

### 11.2 Selection Tests

| Test | Steps | Expected Result |
|------|-------|-----------------|
| Select region | L+B, move RIGHT, B | Selection highlighted on waveform |
| Clear selection | L+R | Selection removed |
| Selection info | Select region | Hex start/end/length shown |

### 11.3 DSP Operation Tests

| Test | Steps | Expected Result |
|------|-------|-----------------|
| Crop | Select region, CROP | Sample trimmed to selection only |
| Normalize | NORM on quiet sample | Peak reaches 0dB |
| Fade in | Select start, FDE+ | Amplitude ramps from 0 |
| Fade out | Select end, FDE- | Amplitude ramps to 0 |
| Silence | Select region, SLNC | Region zeroed out |
| Reverse | Select region, REV | Region plays backwards |
| Invert | Select region, INV | Phase inverted |
| Downsample | DNSM | Sample reduced, lo-fi sound |
| Channel L | SRC:LEFT on stereo sample | Only left channel displayed/used |
| Channel R | SRC:RGHT on stereo sample | Only right channel displayed/used |

### 11.4 File Operation Tests

| Test | Steps | Expected Result |
|------|-------|-----------------|
| Save | SAVE | New WAV file created in Samples dir |
| Overwrite | OVWR | Existing file updated |
| Rename | RENM | File renamed on disk |
| Duplicate | DUPL | Sample copied to new instrument slot |
| Delete | DEL | Sample removed from instrument |

### 11.5 Slice Tests

| Test | Steps | Expected Result |
|------|-------|-----------------|
| Manual marker | SELECT+A at cursor | Marker placed, numbered |
| Remove marker | SELECT+B near marker | Marker removed |
| Lazy chop | Play sample, SELECT+A | Marker at playhead position |
| Auto transient | SLC:TRN, adjust SENS | Markers at onsets |
| Auto silence | SLC:SIL, adjust SENS | Markers at silence→sound boundaries |
| Equal slice | SLC:EQL, CNT:08 | 8 equally spaced markers |
| Slice playback | Enable slice, play C-1 to C-2 | Each note plays different slice |
| Sensitivity | Change SENS in TRN mode | More/fewer markers detected |

### 11.6 Spectrum + Loop Tests

| Test | Steps | Expected Result |
|------|-------|-----------------|
| Spectrum view | SELECT toggle | Spectrum bar appears/disappears |
| Spectrum content | View sine wave | Single peak at correct frequency |
| Loop markers | Set loop in INST, open editor | Yellow loop markers visible on waveform |

### 11.7 Cross-Feature Tests

| Test | Steps | Expected Result |
|------|-------|-----------------|
| Slice + playback | Slice a breakbeat, play in phrase | Correct slices triggered by notes |
| Edit + save | Crop, normalize, save | New WAV matches edited version |
| Stereo → crop | Load stereo, pick L, crop, save | Mono WAV from left channel |
| Large sample | Load 60-second sample | No lag, smooth scrolling |
| Editor + tables | Sliced instrument with table | Table effects apply per-slice |

### Definition of Done - Phase 11
- [ ] All core tests pass
- [ ] All DSP operations produce correct results
- [ ] File operations work reliably
- [ ] Slicing works in all modes
- [ ] Performance acceptable (no frame drops on Miyoo Flip)
- [ ] No crashes on edge cases
- [ ] Tested on real device

---

## Files to Create/Modify

### New Files
| File | Purpose |
|------|---------|
| `app/src/main/cpp/sample-editor.cpp` | C++ DSP operations + KissFFT integration |
| `app/src/main/cpp/kissfft/` (directory) | KissFFT source files |
| `app/src/main/java/.../ui/modules/SampleEditorModule.kt` | Full-screen waveform editor UI |
| `app/src/main/java/.../core/logic/SampleEditorController.kt` | Editor business logic |
| `app/src/main/java/.../core/data/SampleEditorState.kt` | Editor state data classes |

### Modified Files
| File | Changes |
|------|---------|
| `app/src/main/cpp/CMakeLists.txt` | Add sample-editor.cpp and KissFFT sources |
| `app/src/main/cpp/native-audio.cpp` | Extern declarations for samples[] access from sample-editor.cpp |
| `app/src/main/java/.../core/data/TrackerData.kt` | Add sliceMarkers, sliceEnabled to Instrument |
| `app/src/main/java/.../core/data/ScreenType.kt` | Add SAMPLE_EDITOR |
| `app/src/main/java/.../core/audio/IAudioBackend.kt` | New DSP + waveform methods |
| `app/src/main/java/.../platform/android/OboeAudioBackend.kt` | JNI implementations |
| `app/src/main/java/.../core/logic/TrackerController.kt` | Sample editor entry/exit |
| `app/src/main/java/.../core/logic/PlaybackController.kt` | Slice playback support |
| `app/src/main/java/.../InstrumentModule.kt` | EDIT button + row rearrangement |
| `app/src/main/java/.../PixelPerfectRenderer.kt` | Route SAMPLE_EDITOR to module |
| `app/src/main/java/.../ScreenLayouts.kt` | Add sample editor state params |
| `app/src/main/java/.../MainActivity.kt` | Sample editor state management |
| `app/src/main/java/.../core/audio/AudioEngine.kt` | Stereo loading support |

---

## Summary Timeline

| Week | Phase | Tasks |
|------|-------|-------|
| 1 | 1, 2 | KissFFT integration, C++ infrastructure |
| 2 | 3, 4 | Data model, controller, waveform display |
| 3 | 5, 6 | Selection system, DSP operations |
| 4 | 7, 8 | File operations, slice system |
| 5 | 9, 10 | Slice playback, spectrum + loop visualization |
| 6 | 11 | Integration testing, polish |

**Buffer:** 4-5 days for unexpected issues

---

## Post-MVP Ideas (NOT in This Extension)

These are explicitly deferred:

**Undo System:**
- Single-level undo (store previous buffer state before each operation)
- Would require ~2x memory per sample during editing

**Time-Stretch:**
- Phase vocoder using KissFFT (STFT → modify → ISTFT)
- Complex to implement correctly without artifacts
- Repitch is simpler alternative (already possible via root note adjustment)

**Crossfade Loop:**
- Smooth loop point transition by crossfading end→start region
- Eliminates loop clicks in sustained samples

**8-Bit Convert:**
- Quantize sample to 8-bit depth
- Already covered by real-time CRUSH parameter in instrument screen

**Snap Marker to Tempo:**
- Align slice markers to musical subdivisions (1/4, 1/8, 1/16)
- Requires tempo context from project settings

**WAV Marker Embedding:**
- Save slice markers inside WAV file metadata (like M8 does)
- Enables marker sharing across projects/instruments

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
3. Are there any blockers from previous phases?

---

**Document Version:** 1.0
**Created:** 2026-02-18
**Author:** Claude Code + Developer collaboration
**References:**
- [M8 Operation Manual (Sample Editor)](https://www.manualslib.com/manual/2290745/Dirtywave-M8.html?page=47)
- [M8 Firmware Changelog](https://github.com/Dirtywave/M8Firmware/blob/main/changelog.txt)
- [Renoise Sampler Waveform](https://tutorials.renoise.com/wiki/Sampler_Waveform)
- [KissFFT Library](https://github.com/mborgerding/kissfft)
