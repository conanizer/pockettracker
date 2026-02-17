# Extension Pack 3: Video-to-Sample Audio Extraction

## Overview

Add the ability to import audio from video files as samples. Users record their
screen with any app/OS feature, then load the video into PocketTracker to extract
the audio track as a sample. Inspired by Koala Sampler's approach.

**No special permissions required** — Android's `MediaExtractor`/`MediaCodec` APIs
can decode audio from video files using standard file access, which is already
available (app has `MANAGE_EXTERNAL_STORAGE`).

---

## Current State (What Already Exists)

The project has been massively refactored since initial planning. Most of the
infrastructure this feature needs is **already in place**:

- **256 sample slots** in C++ native engine (not 4 anymore)
- **InstrumentModule.kt** — full instrument screen with TYPE/LOAD/NAME/ROOT/
  DETUNE/START/END/REVERSE/LOOP/DRIVE/CRUSH/FILTER parameters
- **FileBrowserModule.kt** — file browser with folder navigation, extension
  filtering, rename/delete/create, sort modes, WAV preview via START button
- **InstrumentController.kt** — `loadSampleFromFile()`, `previewSampleFile()`,
  cursor/navigation, status messages
- **AudioEngine.kt** — `loadSampleFromFile()` with full WAV parsing (stereo→mono,
  sample rate compensation), `previewSampleFile()` with temp slot 255
- **WavWriter.kt** — `writeWavMono()`, `writeWav()`, `writeWavInterleaved()`
  using `IFileSystem` abstraction
- **IFileSystem** — platform-agnostic file ops with `getSamplesDirectory()`,
  `writeBytes()`, `listFiles()`, etc.
- **Instrument.sampleFilePath** — already exists for sample persistence
- **Platform abstractions** — `IAudioBackend`, `IResourceLoader`, `ILogger`,
  `IFileSystem` all ready for Linux port

**The only missing piece is video audio extraction itself.**

The current flow for loading samples is:
```
Instrument screen → LOAD button (row 0, col 3)
  → FileBrowser opens (filtered to .wav extension)
  → User navigates folders, selects WAV
  → InstrumentController.loadSampleFromFile() → AudioEngine.loadSampleFromFile()
  → WAV parsed → FloatArray → native_loadSample() → C++ engine
```

We need to extend this to also handle video files.

---

## Implementation Plan

### Step 1: Video Audio Extractor Interface (core/media/IVideoAudioExtractor.kt)

**Goal:** Platform-abstract interface for extracting audio from video files.

**New file:** `core/media/IVideoAudioExtractor.kt`

```kotlin
package com.example.pockettracker.core.media

/**
 * Platform-agnostic interface for extracting audio from video files.
 *
 * Android: Uses MediaExtractor + MediaCodec
 * Linux:   Uses FFmpeg (future)
 */
interface IVideoAudioExtractor {

    data class ExtractionResult(
        val samples: FloatArray,   // mono, normalized -1.0..1.0
        val sampleRate: Int,       // e.g. 48000
        val durationMs: Long,      // total audio duration
        val sourceFormat: String   // e.g. "AAC 48000Hz stereo"
    )

    sealed class ExtractionError {
        data class NoAudioTrack(val message: String) : ExtractionError()
        data class DecodeFailed(val message: String) : ExtractionError()
        data class FileTooLong(val durationSec: Int, val maxSec: Int) : ExtractionError()
        data class FileNotFound(val path: String) : ExtractionError()
    }

    /**
     * Extract audio from a video file.
     * @param path Absolute path to video file
     * @param maxDurationSec Maximum duration to extract (0 = no limit)
     * @return Result with audio data or error
     */
    fun extractAudio(path: String, maxDurationSec: Int = 60): Result<ExtractionResult>

    /**
     * Check if a file appears to be a supported video format.
     * Quick check based on extension, not file contents.
     */
    fun isSupportedVideo(path: String): Boolean

    companion object {
        val SUPPORTED_EXTENSIONS = listOf("mp4", "mkv", "webm", "3gp", "m4a", "mov")
    }
}
```

**Why a core interface:** Follows the same pattern as `IAudioBackend`, `IFileSystem`,
`IResourceLoader`. The core layer stays Android-free; platform implementations
live under `platform/`.

### Step 2: Android Implementation (platform/android/AndroidVideoAudioExtractor.kt)

**Goal:** Implement the extractor using Android's `MediaExtractor` + `MediaCodec`.

**New file:** `platform/android/AndroidVideoAudioExtractor.kt`

**Implementation details:**

```
MediaExtractor.setDataSource(path)
  → Loop trackCount, find MIME starting with "audio/"
  → selectTrack(audioTrackIndex)
  → Read MediaFormat (sample rate, channels, MIME type)
  → Create MediaCodec.createDecoderByType(mime)
  → Configure decoder with track format
  → Decode loop:
      Feed compressed buffers: MediaExtractor → MediaCodec input
      Collect PCM output: MediaCodec output → ByteBuffer
      Read as 16-bit shorts (or float if format specifies)
  → Convert: stereo→mono, normalize to Float32 -1.0..1.0
  → Release MediaExtractor + MediaCodec
```

**Key considerations:**
- `MediaExtractor` + `MediaCodec` are available since API 16 (our min is 24)
- Zero additional dependencies — both are Android framework classes
- Handles AAC, MP3, Opus, Vorbis, FLAC, PCM audio inside any container
- Synchronous decode is fine — extraction happens on user action, not real-time

**Duration cap:** Default 60s maximum. At 48kHz mono = ~11.5MB float data.
Reasonable for a tracker targeting budget devices with 1GB RAM.

**Error handling:**
- Video has no audio track → `ExtractionError.NoAudioTrack`
- Codec fails → `ExtractionError.DecodeFailed` with codec info
- Audio longer than max → `ExtractionError.FileTooLong` with duration info
- File doesn't exist → `ExtractionError.FileNotFound`

### Step 3: Extend File Browser to Show Video Files

**Goal:** When opening file browser from instrument LOAD, show both WAV and
video files.

**Changes to `MainActivity.kt`** (instrument LOAD button handler, ~line 1164):

Currently:
```kotlin
fileBrowserState = FileBrowserModule.State(
    currentDirectory = samplesDir,
    items = fileBrowserModule.buildItemList(samplesDir, fileExtension = "wav"),
    fileExtension = "wav",  // Only show WAV files
)
```

Change to pass a list of accepted extensions instead of a single extension.

**Changes to `FileBrowserModule.kt`** (`buildItemList` and `State`):

Option A — Multi-extension filter:
- Change `fileExtension: String?` to `fileExtensions: List<String>?`
- Filter: `extensions == null || it.extension in extensions`
- State holds `fileExtensions = listOf("wav", "mp4", "mkv", "webm", "3gp", "mov")`

Option B — Simpler: null filter + let user navigate anywhere:
- Pass `fileExtension = null` from instrument LOAD
- Show all files, browser already shows folders for navigation
- Pro: simplest change. Con: shows irrelevant files.

**Recommended: Option A** — multi-extension filter. Small change, clean UX.

**Visual distinction in file browser:**
- WAV files: existing light gray color (`COLOR_FILE`)
- Video files: different color (e.g., `Color(0xFFFFBB55)` — orange/amber)
- File browser already shows extension in the item, so user can tell them apart

### Step 4: Route Video Files Through Extractor on Selection

**Goal:** When user selects a video file in the browser, extract audio → save
as WAV → load as sample.

**Changes to `MainActivity.kt`** (file selection handler, ~line 1461):

Currently:
```kotlin
if (selectedFile.isFile && selectedFile.extension.lowercase() == "wav") {
    // Load WAV directly
    instrumentController.loadSampleFromFile(project, selectedFile.absolutePath)
}
```

Extend to:
```kotlin
val ext = selectedFile.extension.lowercase()
when {
    ext == "wav" -> {
        // Existing path: load WAV directly
        instrumentController.loadSampleFromFile(project, selectedFile.absolutePath)
    }
    videoExtractor.isSupportedVideo(selectedFile.absolutePath) -> {
        // New path: extract audio from video → save as WAV → load
        instrumentController.loadSampleFromVideo(project, selectedFile.absolutePath, videoExtractor)
    }
}
```

### Step 5: Add Video Loading to InstrumentController

**Goal:** Add `loadSampleFromVideo()` method that orchestrates the full flow.

**Changes to `InstrumentController.kt`:**

```kotlin
fun loadSampleFromVideo(
    project: Project,
    videoPath: String,
    extractor: IVideoAudioExtractor,
    fileSystem: IFileSystem
): LoadResult {
    val instrument = project.instruments[currentInstrument]

    // 1. Extract audio from video
    setStatus("Extracting audio...", success = true)
    val result = extractor.extractAudio(videoPath)

    if (result.isFailure) {
        val error = result.exceptionOrNull()?.message ?: "Unknown error"
        setStatus("Extract failed: $error", success = false)
        return LoadResult.Error(error)
    }

    val audioData = result.getOrThrow()

    // 2. Save extracted audio as WAV in Samples/ directory
    val videoFilename = videoPath.substringAfterLast('/').substringBeforeLast('.')
    val wavFilename = "${videoFilename}_audio.wav"
    val wavPath = "${fileSystem.getSamplesDirectory()}/$wavFilename"

    val writeSuccess = WavWriter.writeWavMono(
        fileSystem, wavPath, audioData.samples, audioData.sampleRate
    )

    if (!writeSuccess) {
        setStatus("Failed to save WAV", success = false)
        return LoadResult.Error("Failed to write WAV file")
    }

    // 3. Load the saved WAV as a sample (reuses existing pipeline)
    return loadSampleFromFile(project, wavPath)
}
```

**Key insight:** After extraction, we save as WAV and then use the existing
`loadSampleFromFile()` path. This means:
- Sample rate compensation works automatically (already in `AudioEngine.parseWavBuffer`)
- Stereo→mono handled (already in AudioEngine, though we extract as mono)
- Instrument persistence works (sampleFilePath points to the saved WAV)
- Project reload works (WAV file exists in Samples/ directory)

### Step 6: Wire Up in MainActivity

**Goal:** Inject the extractor and connect it to the existing flow.

**Changes to `MainActivity.kt`:**

1. Create extractor instance alongside other platform implementations:
```kotlin
val videoExtractor = AndroidVideoAudioExtractor()
```

2. Pass to file selection handler (Step 4 changes)

3. Update START button preview to also handle video files:
```kotlin
// In FILE_BROWSER START handler (~line 1461):
if (selectedFile.isFile) {
    val ext = selectedFile.extension.lowercase()
    when {
        ext == "wav" -> trackerController.previewSampleFile(selectedFile.absolutePath)
        videoExtractor.isSupportedVideo(selectedFile.absolutePath) -> {
            // Optional: extract + preview, or skip preview for videos
            // For now, show status "Press A to extract audio"
        }
    }
}
```

---

## File Changes Summary

| File | Action | Description |
|------|--------|-------------|
| `core/media/IVideoAudioExtractor.kt` | **New** | Platform-abstract extractor interface |
| `platform/android/AndroidVideoAudioExtractor.kt` | **New** | MediaExtractor/MediaCodec implementation |
| `FileBrowserModule.kt` | Modify | Multi-extension filter support |
| `InstrumentController.kt` | Modify | Add `loadSampleFromVideo()` method |
| `MainActivity.kt` | Modify | Wire extractor, extend file selection handler |
| `build.gradle.kts` | **No change** | MediaExtractor/MediaCodec are android.media (no deps) |

**Only 2 new files, 3 modified files.** Everything else is already in place.

---

## Implementation Order

```
Step 1: IVideoAudioExtractor interface (core/media/)
Step 2: AndroidVideoAudioExtractor (platform/android/)
  ↓  These two can be built and unit-tested independently
Step 3: Extend FileBrowser extension filter
Step 4: Route video files through extractor on selection
Step 5: Add loadSampleFromVideo() to InstrumentController
Step 6: Wire up in MainActivity
```

Steps 1-2 are the bulk of the work (~70%). Steps 3-6 are integration glue (~30%).

---

## Technical Considerations

### MediaCodec decode loop (the core algorithm)

```
while (!inputDone || !outputDone) {
    // Feed input
    if (!inputDone) {
        val inputBufferIndex = codec.dequeueInputBuffer(timeoutUs)
        if (inputBufferIndex >= 0) {
            val inputBuffer = codec.getInputBuffer(inputBufferIndex)
            val sampleSize = extractor.readSampleData(inputBuffer, 0)
            if (sampleSize < 0) {
                codec.queueInputBuffer(inputBufferIndex, 0, 0, 0,
                    MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                inputDone = true
            } else {
                codec.queueInputBuffer(inputBufferIndex, 0, sampleSize,
                    extractor.sampleTime, 0)
                extractor.advance()
            }
        }
    }

    // Collect output
    val outputBufferIndex = codec.dequeueOutputBuffer(bufferInfo, timeoutUs)
    if (outputBufferIndex >= 0) {
        val outputBuffer = codec.getOutputBuffer(outputBufferIndex)
        // Read PCM samples from outputBuffer
        // Append to result list
        codec.releaseOutputBuffer(outputBufferIndex, false)
        if (bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) {
            outputDone = true
        }
    }
}
```

### PCM output format from MediaCodec

After decoding, `MediaCodec` outputs raw PCM. The format depends on the codec:
- Most codecs: 16-bit signed integers (same as WAV)
- Some (API 24+): May output float if `KEY_PCM_ENCODING` = `ENCODING_PCM_FLOAT`
- Check `outputFormat.getInteger(MediaFormat.KEY_PCM_ENCODING)` after first output

Our code should handle both cases — check encoding and convert accordingly.

### Memory management

- 60s at 48kHz mono = ~11.5MB float data (acceptable)
- During decode, we accumulate shorts/floats in a growable list, then convert
- Peak memory: ~2x final size during conversion (original + float copy)
- For a 60s clip: ~23MB peak — fine for 1GB devices

### Thread safety

- Extraction runs synchronously on the calling thread (UI thread via button press)
- For long videos, this could freeze the UI briefly
- Acceptable for MVP — videos are typically short screen recordings (5-30s)
- Future improvement: run extraction in a coroutine with progress callback

### Linux portability

Only `AndroidVideoAudioExtractor` is Android-specific. For Linux:
- Create `FFmpegVideoAudioExtractor` implementing `IVideoAudioExtractor`
- Either shell out: `ffmpeg -i input.mp4 -f wav -ac 1 -ar 44100 pipe:1`
- Or use JNI bindings to `libavformat`/`libavcodec`
- The interface, InstrumentController, and FileBrowser logic stays identical

### No new dependencies

- `android.media.MediaExtractor` (API 16+)
- `android.media.MediaCodec` (API 16+)
- Both are Android framework classes — zero additional library dependencies

---

## User Flow (End to End)

```
1. User records screen on their device (any app, OS screen recorder)
2. Open PocketTracker → Instrument screen
3. Navigate to LOAD (row 0, column 3) → press A
4. File browser opens showing WAV + video files
5. Navigate to the video file (in Movies/, Downloads/, etc.)
6. Press A to select → "Extracting audio..." status appears
7. Audio extracted → saved as WAV in Samples/ → loaded into instrument
8. Status shows "Loaded: screen_rec_audio"
9. User can now play this sample via notes in the tracker
```

---

## What This Enables

- Sample any audio source: YouTube, synth apps, other music apps, games
- No microphone noise, no permission dialogs, no cables
- Extracted audio saved as WAV for reuse across projects
- Works on any Android device that can screen-record (all modern devices)
- Portable design ready for Linux port with FFmpeg backend
