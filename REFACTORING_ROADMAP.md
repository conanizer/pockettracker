# PocketTracker - Refactoring Roadmap

## Document Purpose
**Step-by-step guide** for refactoring PocketTracker from Android-specific to portable architecture.

Each step is **independent and testable** - you can complete them gradually without breaking the app.

**Target:** Prepare codebase for Linux port while continuing MVP development.

**Timeline:** 1-2 weeks (2-4 hours/day)

---

## Table of Contents
1. [Refactoring Philosophy](#refactoring-philosophy)
2. [Pre-Refactoring Checklist](#pre-refactoring-checklist)
3. [Phase 1: Audio Backend Abstraction](#phase-1-audio-backend-abstraction)
4. [Phase 2: Resource Loading Abstraction](#phase-2-resource-loading-abstraction)
5. [Phase 3: File I/O Abstraction](#phase-3-file-io-abstraction)
6. [Phase 4: Business Logic Extraction](#phase-4-business-logic-extraction)
7. [Testing Strategy](#testing-strategy)
8. [Git Workflow](#git-workflow)

---

## Refactoring Philosophy

### Core Principles

1. **Incremental refactoring** - One step at a time, always working code
2. **Test after each step** - Run app, verify nothing broke
3. **Git commit after each step** - Easy to rollback if needed
4. **Keep old code commented** - Don't delete immediately, mark as deprecated

### What "Portable" Means

**Portable code characteristics:**
```kotlin
// ✅ PORTABLE - No Android dependencies
fun calculateNoteFrequency(midiNote: Int): Float {
    return 440f * Math.pow(2.0, (midiNote - 69) / 12.0).toFloat()
}

// ❌ NOT PORTABLE - Uses Android Context
fun loadSample(context: Context, resourceId: Int): FloatArray {
    context.resources.openRawResource(resourceId).use { ... }
}

// ✅ PORTABLE (after refactoring) - Uses abstraction
fun loadSample(loader: IResourceLoader, name: String): FloatArray {
    return loader.loadWav(name)
}
```

### Success Criteria

After refactoring:
- ✅ Core logic has ZERO Android imports
- ✅ Audio engine accessed through interface
- ✅ File I/O accessed through interface
- ✅ Resources loaded through interface
- ✅ All existing features still work
- ✅ App runs on physical device without issues

---

## Pre-Refactoring Checklist

### Step 0: Backup and Prepare

**Before starting ANY refactoring:**

1. **Commit all current work**
   ```bash
   git add .
   git commit -m "Pre-refactoring checkpoint: Working state before portability refactor"
   git tag v0.9-pre-refactor
   ```

2. **Create refactoring branch**
   ```bash
   git checkout -b refactor/portable-architecture
   ```

3. **Verify everything works**
   - [ ] App builds successfully
   - [ ] App runs on physical device
   - [ ] Phrase playback works
   - [ ] Chain playback works
   - [ ] Song playback works
   - [ ] Sample loading works
   - [ ] Project save/load works
   - [ ] All screens navigate correctly

4. **Document current issues** (if any)
   - Write down any bugs BEFORE refactoring
   - This helps distinguish new bugs from old ones

5. **Set up testing checklist** (copy to notes)
   ```
   After each refactoring step, verify:
   [ ] App builds without errors
   [ ] App runs on device
   [ ] Can navigate to all screens
   [ ] Can play phrase/chain/song
   [ ] Can load sample from file browser
   [ ] Can save/load project
   [ ] No crashes on common operations
   ```

---

## Phase 1: Audio Backend Abstraction

**Goal:** Wrap Oboe in an interface so we can swap it for ALSA/PulseAudio later.

**Time estimate:** 2-3 days (6-12 hours)

**Why this first?** Audio is the most complex system - if we can abstract this, everything else is easier.

---

### Step 1.1: Create IAudioBackend Interface

**Location:** Create new file `core/audio/IAudioBackend.kt`

```kotlin
package com.example.pockettracker.core.audio

/**
 * Platform-agnostic audio backend interface.
 * 
 * Implementations:
 * - Android: OboeAudioBackend (wraps native Oboe C++ code)
 * - Linux: ALSAAudioBackend (future)
 */
interface IAudioBackend {
    /**
     * Initialize the audio stream.
     * @return true if successful, false otherwise
     */
    fun create(): Boolean
    
    /**
     * Load a sample into the specified slot.
     * @param id Sample slot (0-255)
     * @param samples Float array of audio samples (-1.0 to 1.0)
     */
    fun loadSample(id: Int, samples: FloatArray)
    
    /**
     * Schedule a note to play at a specific audio frame.
     * @param frame Absolute audio frame number (from getCurrentFrame())
     * @param sampleId Which sample to play (0-255)
     * @param trackId Which track this note belongs to (0-7, for voice stealing)
     * @param freq Target playback frequency in Hz
     * @param baseFreq Base frequency of the sample (for pitch calculation)
     * @param vol Volume (0.0 to 1.0)
     */
    fun scheduleNote(
        frame: Long,
        sampleId: Int,
        trackId: Int,
        freq: Float,
        baseFreq: Float,
        vol: Float
    )
    
    /**
     * Get current audio frame counter.
     * Used for sample-accurate scheduling.
     */
    fun getCurrentFrame(): Long
    
    /**
     * Clear all scheduled notes from the queue.
     * Used when stopping playback.
     */
    fun clearScheduledNotes()
    
    /**
     * Resume the audio stream after it was paused.
     */
    fun resumeStream()
    
    /**
     * Stop all currently playing voices immediately.
     */
    fun stopAll()
    
    /**
     * Get the actual sample rate of the audio stream.
     * @return Sample rate in Hz (typically 44100 or 48000)
     */
    fun getSampleRate(): Int
    
    /**
     * Update the waveform visualization buffer.
     * This captures the current mixed audio output.
     * @param buffer Float array to fill with waveform data
     */
    fun updateWaveform(buffer: FloatArray)
    
    /**
     * Release audio resources and close stream.
     */
    fun close()
}
```

**Test:** Code should compile (interface doesn't break anything yet).

**Git commit:**
```bash
git add core/audio/IAudioBackend.kt
git commit -m "[Refactor Step 1.1] Add IAudioBackend interface"
```

---

### Step 1.2: Create OboeAudioBackend Implementation

**Location:** Create new file `platform/android/OboeAudioBackend.kt`

```kotlin
package com.example.pockettracker.platform.android

import com.example.pockettracker.core.audio.IAudioBackend
import android.util.Log

/**
 * Android implementation of IAudioBackend using Oboe library.
 * 
 * This wraps the native C++ audio engine (native-audio.cpp) via JNI.
 * All the actual audio processing happens in C++ for maximum performance.
 */
class OboeAudioBackend : IAudioBackend {
    
    private val TAG = "OboeAudioBackend"
    
    init {
        // Load native library (C++ audio engine)
        try {
            System.loadLibrary("pockettracker")
            Log.d(TAG, "✅ Native library loaded successfully")
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "❌ Failed to load native library: ${e.message}")
            throw e
        }
    }
    
    // ═══════════════════════════════════════════════════════════
    // JNI DECLARATIONS (native methods implemented in native-audio.cpp)
    // ═══════════════════════════════════════════════════════════
    
    private external fun native_create(): Boolean
    private external fun native_loadSample(id: Int, samples: FloatArray)
    private external fun native_scheduleNote(
        frame: Long,
        sampleId: Int,
        trackId: Int,
        freq: Float,
        baseFreq: Float,
        vol: Float
    )
    private external fun native_getCurrentFrame(): Long
    private external fun native_clearScheduledNotes()
    private external fun native_resumeStream()
    private external fun native_stopAll()
    private external fun native_getSampleRate(): Int
    private external fun native_updateWaveform(buffer: FloatArray)
    private external fun native_close()
    
    // ═══════════════════════════════════════════════════════════
    // INTERFACE IMPLEMENTATION (just forward calls to JNI)
    // ═══════════════════════════════════════════════════════════
    
    override fun create(): Boolean {
        val success = native_create()
        if (success) {
            Log.d(TAG, "✅ Oboe audio stream created (${getSampleRate()} Hz)")
        } else {
            Log.e(TAG, "❌ Failed to create Oboe audio stream")
        }
        return success
    }
    
    override fun loadSample(id: Int, samples: FloatArray) {
        native_loadSample(id, samples)
        Log.d(TAG, "📦 Loaded sample $id (${samples.size} samples)")
    }
    
    override fun scheduleNote(
        frame: Long,
        sampleId: Int,
        trackId: Int,
        freq: Float,
        baseFreq: Float,
        vol: Float
    ) {
        native_scheduleNote(frame, sampleId, trackId, freq, baseFreq, vol)
    }
    
    override fun getCurrentFrame(): Long = native_getCurrentFrame()
    
    override fun clearScheduledNotes() {
        native_clearScheduledNotes()
        Log.d(TAG, "🗑️ Cleared scheduled notes")
    }
    
    override fun resumeStream() {
        native_resumeStream()
        Log.d(TAG, "▶️ Resumed audio stream")
    }
    
    override fun stopAll() {
        native_stopAll()
        Log.d(TAG, "⏹️ Stopped all voices")
    }
    
    override fun getSampleRate(): Int = native_getSampleRate()
    
    override fun updateWaveform(buffer: FloatArray) {
        native_updateWaveform(buffer)
    }
    
    override fun close() {
        native_close()
        Log.d(TAG, "🔌 Closed audio stream")
    }
}
```

**Test:** Code should compile. Don't use it yet.

**Git commit:**
```bash
git add platform/android/OboeAudioBackend.kt
git commit -m "[Refactor Step 1.2] Add OboeAudioBackend implementation"
```

---

### Step 1.3: Add Missing native_close() to C++

**Location:** Edit `native-audio.cpp`

Add this at the end of the file:

```cpp
// Close and cleanup (called when app exits)
extern "C" JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1close(
    JNIEnv* env,
    jobject /* this */) {
    
    if (stream) {
        stream->stop();
        stream->close();
        stream.reset();
    }
}
```

**Note:** Update package path in JNI function name to match `platform.android.OboeAudioBackend`.

**Test:** Build should succeed (C++ compiles).

**Git commit:**
```bash
git add app/src/main/cpp/native-audio.cpp
git commit -m "[Refactor Step 1.3] Add native_close() method to C++ audio engine"
```

---

### Step 1.4: Update CMakeLists.txt

**Location:** Edit `CMakeLists.txt`

**No changes needed** - JNI functions are discovered automatically by package name.

Just verify it still builds:
```bash
./gradlew assembleDebug
```

---

### Step 1.5: Refactor TrackerAudioEngine to Use IAudioBackend

**Location:** Edit `TrackerAudioEngine.kt`

**Current structure:**
```kotlin
class TrackerAudioEngine(private val context: Context) {
    init { System.loadLibrary("pockettracker") }
    private external fun native_create(): Boolean
    // ... all JNI methods
}
```

**New structure:**
```kotlin
package com.example.pockettracker.core.audio

import com.example.pockettracker.platform.android.AndroidResourceLoader
import android.content.Context
import android.util.Log

/**
 * Platform-agnostic audio engine.
 * 
 * Manages sample loading, playback scheduling, and waveform visualization.
 * Uses IAudioBackend for actual audio I/O (platform-specific).
 * Uses IResourceLoader for loading samples (platform-specific).
 */
class AudioEngine(
    private val backend: IAudioBackend,
    private val resourceLoader: IResourceLoader
) {
    private val TAG = "AudioEngine"
    
    // Waveform buffer for visualization (620 samples for 620px width oscilloscope)
    val waveformBuffer = FloatArray(620) { 0f }
    
    // Sample metadata (base frequencies and sample rate compensation ratios)
    private val sampleBaseFrequencies = mutableMapOf<Int, Float>()
    private val sampleRateRatios = mutableMapOf<Int, Float>()
    
    /**
     * Initialize audio engine and load default samples.
     * @return true if successful
     */
    fun create(): Boolean {
        val success = backend.create()
        if (success) {
            Log.d(TAG, "✅ Audio engine created")
            loadAllSamples()
        } else {
            Log.e(TAG, "❌ Failed to create audio engine")
        }
        return success
    }
    
    /**
     * Load all 12 default samples from resources.
     */
    private fun loadAllSamples() {
        try {
            // List of default sample names (matches resource files in res/raw/)
            val sampleNames = listOf(
                "kick", "snare", "hihat", "bass",
                "shimmer", "tambo", "lofi", "choirstring",
                "apache162", "copta162", "funky162", "eightoeight"
            )
            
            // Load each sample
            sampleNames.forEachIndexed { index, name ->
                val (samples, baseFreq) = resourceLoader.loadWav(name)
                backend.loadSample(index, samples)
                sampleBaseFrequencies[index] = baseFreq
                
                // Calculate sample rate compensation ratio
                val ratio = baseFreq / 261.63f  // 261.63 = C-4 frequency
                sampleRateRatios[index] = ratio
            }
            
            Log.d(TAG, "✅ Loaded ${sampleNames.size} default samples")
        } catch (e: Exception) {
            Log.e(TAG, "❌ Error loading samples: ${e.message}")
        }
    }
    
    /**
     * Load a custom WAV sample from file path.
     * @param sampleId Slot to load into (0-255, typically 12-255 for user samples)
     * @param filePath Absolute path to WAV file
     * @return Base frequency (adjusted for sample rate), or null if failed
     */
    fun loadCustomSample(sampleId: Int, filePath: String): Float? {
        return try {
            val (samples, baseFreq) = resourceLoader.loadWavFromFile(filePath)
            backend.loadSample(sampleId, samples)
            sampleBaseFrequencies[sampleId] = baseFreq
            sampleRateRatios[sampleId] = baseFreq / 261.63f
            
            Log.d(TAG, "✅ Loaded custom sample $sampleId from $filePath")
            baseFreq
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to load sample $sampleId: ${e.message}")
            null
        }
    }
    
    /**
     * Get device sample rate.
     */
    fun getDeviceSampleRate(): Int = backend.getSampleRate()
    
    /**
     * Get base frequency for a sample (adjusted for sample rate compensation).
     */
    fun getSampleBaseFrequency(sampleId: Int): Float {
        return sampleBaseFrequencies[sampleId] ?: 261.63f
    }
    
    /**
     * Update waveform visualization buffer from audio output.
     */
    fun updateWaveform() {
        backend.updateWaveform(waveformBuffer)
    }
    
    /**
     * Play a note immediately.
     * @param sampleId Which sample (0-255)
     * @param trackId Which track (0-7, for voice stealing)
     * @param frequency Target playback frequency in Hz
     * @param volume Volume (0.0-1.0)
     */
    fun playNote(sampleId: Int, trackId: Int, frequency: Float, volume: Float) {
        val frame = backend.getCurrentFrame()
        val baseFreq = sampleBaseFrequencies[sampleId] ?: 261.63f
        val vol = (volume / 255f).coerceIn(0f, 1f)
        
        backend.scheduleNote(frame, sampleId, trackId, frequency, baseFreq, vol)
    }
    
    /**
     * Schedule a note to play at specific audio frame.
     * Used for sample-accurate sequencer playback.
     */
    fun scheduleNote(
        frame: Long,
        sampleId: Int,
        trackId: Int,
        frequency: Float,
        volume: Float
    ) {
        val baseFreq = sampleBaseFrequencies[sampleId] ?: 261.63f
        val vol = (volume / 255f).coerceIn(0f, 1f)
        
        backend.scheduleNote(frame, sampleId, trackId, frequency, baseFreq, vol)
    }
    
    /**
     * Get current audio frame counter (for scheduling).
     */
    fun getCurrentFrame(): Long = backend.getCurrentFrame()
    
    /**
     * Clear all scheduled notes.
     */
    fun clearScheduledNotes() {
        backend.clearScheduledNotes()
    }
    
    /**
     * Resume audio stream.
     */
    fun resumeStream() {
        backend.resumeStream()
    }
    
    /**
     * Stop all currently playing voices.
     */
    fun stopAll() {
        backend.stopAll()
    }
    
    /**
     * Close audio engine and release resources.
     */
    fun close() {
        backend.close()
    }
}
```

**Test:** Code compiles but don't run yet.

**Git commit:**
```bash
git add core/audio/AudioEngine.kt
git commit -m "[Refactor Step 1.5] Refactor TrackerAudioEngine to AudioEngine with IAudioBackend"
```

---

### Step 1.6: Update MainActivity to Use New Architecture

**Location:** Edit `MainActivity.kt` - find the `PocketTrackerApp` function

**OLD CODE:**
```kotlin
@Composable
fun PocketTrackerApp(layoutConfig: DeviceAdapter.LayoutConfig) {
    val context = LocalContext.current
    
    // Audio Engine setup
    val audioEngine = remember { TrackerAudioEngine(context) }
    
    LaunchedEffect(Unit) {
        audioEngine.create()
    }
    
    // ... rest of the code
}
```

**NEW CODE:**
```kotlin
@Composable
fun PocketTrackerApp(layoutConfig: DeviceAdapter.LayoutConfig) {
    val context = LocalContext.current
    
    // Create platform-specific backends
    val audioBackend = remember { OboeAudioBackend() }
    val resourceLoader = remember { AndroidResourceLoader(context) }
    
    // Create platform-agnostic audio engine
    val audioEngine = remember { AudioEngine(audioBackend, resourceLoader) }
    
    LaunchedEffect(Unit) {
        audioEngine.create()
    }
    
    // Add cleanup when app closes
    DisposableEffect(Unit) {
        onDispose {
            audioEngine.close()
        }
    }
    
    // ... rest of the code stays the same
}
```

**CRITICAL:** Don't change anything else in MainActivity yet! Just this audio engine initialization.

**Test:** 
1. Build and run app
2. Navigate to phrase screen
3. Play a phrase - sound should work!
4. Check logcat for "✅ Audio engine created" and "✅ Loaded 12 default samples"

**If it works:**
```bash
git add MainActivity.kt
git commit -m "[Refactor Step 1.6] Update MainActivity to use new AudioEngine architecture"
```

**If it doesn't work:** Debug before proceeding! Check:
- JNI package name matches in native-audio.cpp
- OboeAudioBackend loads library correctly
- Resource loading still works

---

### Step 1.7: Create IResourceLoader Interface

**Location:** Create new file `core/resources/IResourceLoader.kt`

```kotlin
package com.example.pockettracker.core.resources

/**
 * Platform-agnostic resource loading interface.
 * 
 * Implementations:
 * - Android: AndroidResourceLoader (loads from R.raw.*)
 * - Linux: FileResourceLoader (loads from /usr/share/pockettracker/assets/)
 */
interface IResourceLoader {
    /**
     * Load a WAV file by name from default resources.
     * @param name Resource name (e.g. "kick", "snare")
     * @return Pair of (samples as FloatArray, base frequency adjusted for sample rate)
     */
    fun loadWav(name: String): Pair<FloatArray, Float>
    
    /**
     * Load a WAV file from absolute file path.
     * @param path Full path to WAV file
     * @return Pair of (samples as FloatArray, base frequency adjusted for sample rate)
     */
    fun loadWavFromFile(path: String): Pair<FloatArray, Float>
}
```

**Git commit:**
```bash
git add core/resources/IResourceLoader.kt
git commit -m "[Refactor Step 1.7] Add IResourceLoader interface"
```

---

### Step 1.8: Create AndroidResourceLoader

**Location:** Create new file `platform/android/AndroidResourceLoader.kt`

```kotlin
package com.example.pockettracker.platform.android

import android.content.Context
import com.example.pockettracker.R
import com.example.pockettracker.core.resources.IResourceLoader
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Android implementation of IResourceLoader.
 * Loads samples from res/raw/ and from external file paths.
 */
class AndroidResourceLoader(private val context: Context) : IResourceLoader {
    
    // Map resource names to R.raw.* IDs
    private val resourceMap = mapOf(
        "kick" to R.raw.kick,
        "snare" to R.raw.snare,
        "hihat" to R.raw.hihat,
        "bass" to R.raw.bass,
        "shimmer" to R.raw.shimmer,
        "tambo" to R.raw.tambo,
        "lofi" to R.raw.lofi,
        "choirstring" to R.raw.choirstring,
        "apache162" to R.raw.apache162,
        "copta162" to R.raw.copta162,
        "funky162" to R.raw.funky162,
        "eightoeight" to R.raw.eightoeight
    )
    
    override fun loadWav(name: String): Pair<FloatArray, Float> {
        val resourceId = resourceMap[name] 
            ?: error("Unknown resource: $name. Available: ${resourceMap.keys}")
        
        return context.resources.openRawResource(resourceId).use { inputStream ->
            parseWavFile(inputStream.readBytes())
        }
    }
    
    override fun loadWavFromFile(path: String): Pair<FloatArray, Float> {
        val file = File(path)
        if (!file.exists()) error("File not found: $path")
        if (!file.canRead()) error("File not readable: $path")
        
        return parseWavFile(file.readBytes())
    }
    
    /**
     * Parse WAV file bytes into float samples + base frequency.
     * Handles stereo->mono conversion and sample rate compensation.
     */
    private fun parseWavFile(buffer: ByteArray): Pair<FloatArray, Float> {
        // Read WAV header
        val channels = ByteBuffer.wrap(buffer, 22, 2)
            .order(ByteOrder.LITTLE_ENDIAN)
            .short.toInt()
        
        val sampleRate = ByteBuffer.wrap(buffer, 24, 4)
            .order(ByteOrder.LITTLE_ENDIAN)
            .int
        
        // Skip WAV header (44 bytes = standard WAV header size)
        val dataStart = 44
        val audioDataSize = buffer.size - dataStart
        val shortBuffer = ByteBuffer.wrap(buffer, dataStart, audioDataSize)
            .order(ByteOrder.LITTLE_ENDIAN)
            .asShortBuffer()
        
        // Convert 16-bit samples to float (-1.0 to 1.0)
        val rawSamples = FloatArray(shortBuffer.remaining()) { i ->
            shortBuffer.get(i) / 32768f
        }
        
        // If stereo, mix down to mono by averaging L+R channels
        val monoSamples = if (channels == 2) {
            FloatArray(rawSamples.size / 2) { i ->
                (rawSamples[i * 2] + rawSamples[i * 2 + 1]) / 2f
            }
        } else {
            rawSamples
        }
        
        // Calculate base frequency with sample rate compensation
        // If sample is 44100Hz and device is 48000Hz, samples play faster (higher pitch)
        // Adjust base frequency: baseFreq * (deviceRate / sampleRate) to compensate
        val deviceSampleRate = getDeviceSampleRate()
        val sampleRateRatio = deviceSampleRate.toFloat() / sampleRate.toFloat()
        val adjustedBaseFreq = 261.63f * sampleRateRatio  // C-4 frequency
        
        return Pair(monoSamples, adjustedBaseFreq)
    }
    
    /**
     * Get device sample rate (we need AudioEngine reference - hacky!)
     * TODO: Better way to access this?
     */
    private fun getDeviceSampleRate(): Int {
        // For now, assume 44100Hz (we'll fix this later)
        // Proper solution: Pass device sample rate to IResourceLoader
        return 44100
    }
}
```

**Note:** The `getDeviceSampleRate()` is a temporary hack. We'll fix it in next steps.

**Git commit:**
```bash
git add platform/android/AndroidResourceLoader.kt
git commit -m "[Refactor Step 1.8] Add AndroidResourceLoader implementation"
```

---

### Step 1.9: Fix Sample Rate Issue in ResourceLoader

We need device sample rate BEFORE creating AudioEngine, so there's a chicken-and-egg problem.

**Solution:** Create AudioBackend first, query its sample rate, then pass to ResourceLoader.

**Update AudioEngine constructor:**

```kotlin
class AudioEngine(
    private val backend: IAudioBackend,
    private val resourceLoader: IResourceLoader
) {
    // ... same as before
}
```

**Update AndroidResourceLoader to accept device sample rate:**

```kotlin
class AndroidResourceLoader(
    private val context: Context,
    private val deviceSampleRate: Int  // Add this parameter!
) : IResourceLoader {
    
    // Remove getDeviceSampleRate() hack
    
    private fun parseWavFile(buffer: ByteArray): Pair<FloatArray, Float> {
        // ... same code until sample rate compensation
        
        // Use deviceSampleRate from constructor
        val sampleRateRatio = deviceSampleRate.toFloat() / sampleRate.toFloat()
        val adjustedBaseFreq = 261.63f * sampleRateRatio
        
        return Pair(monoSamples, adjustedBaseFreq)
    }
}
```

**Update MainActivity:**

```kotlin
@Composable
fun PocketTrackerApp(layoutConfig: DeviceAdapter.LayoutConfig) {
    val context = LocalContext.current
    
    // Create audio backend first
    val audioBackend = remember { OboeAudioBackend() }
    
    // Initialize backend to get sample rate
    LaunchedEffect(Unit) {
        audioBackend.create()
    }
    
    // Get device sample rate (after backend created!)
    val deviceSampleRate = audioBackend.getSampleRate()
    
    // Create resource loader with device sample rate
    val resourceLoader = remember(deviceSampleRate) {
        AndroidResourceLoader(context, deviceSampleRate)
    }
    
    // Create audio engine
    val audioEngine = remember(audioBackend, resourceLoader) {
        AudioEngine(audioBackend, resourceLoader).also {
            it.create()  // This loads samples now
        }
    }
    
    // ... rest stays same
}
```

**Wait, this is getting messy!** Let's simplify - AudioEngine can handle initialization itself.

**Better approach:**

```kotlin
class AudioEngine(
    private val backend: IAudioBackend,
    private val context: Context  // Just for now, we'll remove this later
) {
    private lateinit var resourceLoader: IResourceLoader
    
    fun create(): Boolean {
        val success = backend.create()
        if (success) {
            // NOW we can get device sample rate
            val deviceSampleRate = backend.getSampleRate()
            
            // Create resource loader with correct sample rate
            resourceLoader = AndroidResourceLoader(context, deviceSampleRate)
            
            // Load samples
            loadAllSamples()
        }
        return success
    }
    
    // ... rest stays same
}
```

**Even better - pass IResourceLoader factory:**

Actually, let's keep it simple for MVP. We can refine later.

**Final decision:** AudioEngine takes Context temporarily, creates ResourceLoader internally.

**Git commit:**
```bash
git add platform/android/AndroidResourceLoader.kt
git add core/audio/AudioEngine.kt
git commit -m "[Refactor Step 1.9] Fix sample rate handling in resource loader"
```

---

**(I'll stop here with detailed steps - the full refactoring guide would be 50+ pages)**

**Continue with Phases 2-4 following same pattern...**

---

## Phase 2: Resource Loading Abstraction

*Steps 2.1 - 2.5: Extract resource loading to IResourceLoader*

## Phase 3: File I/O Abstraction

*Steps 3.1 - 3.8: Extract file I/O to IFileSystem*

# REFACTORING_ROADMAP.md - PHASE 4 COMPLETE ADDITION

## Replace line 955 "Steps 4.1 - 4.15: Extract MainActivity logic to TrackerController" with:

---

## Phase 4: Business Logic Extraction

**Goal:** Extract business logic from MainActivity into separate, portable controllers

**Duration:** 5-7 days

**Why separate controllers instead of one TrackerController?**
- Each controller ~200-300 lines (manageable)
- Clear responsibilities (easier to understand)
- Parallel work possible (mentor + developer)
- Easier testing (test controllers independently)
- Better for Linux port (swap implementations easily)

**Target structure:**
```
core/logic/
├── TrackerController.kt      # Main coordinator (owns state, delegates)
├── InputController.kt         # Button handling, selection mode
├── PlaybackController.kt      # Playback scheduling
├── EffectProcessor.kt         # Effect calculations
├── InstrumentController.kt    # Sample management
├── FileController.kt          # Save/load
└── ClipboardManager.kt        # Copy/paste
```

---

### Step 4.1: Create InputController

**Goal:** Extract all button handling and cursor logic from MainActivity

**What to extract:**
- Button press handlers (A, B, Start, Select, L/R, D-pad)
- Cursor movement logic
- Value editing (A+direction)
- Selection mode (for copy/paste)
- Quick insert (A on empty row)

**Create file:** `core/logic/InputController.kt`

```kotlin
package com.yourpackage.pockettracker.core.logic

import androidx.compose.runtime.*
import com.yourpackage.pockettracker.core.data.*

/**
 * Handles all user input (buttons, navigation, value editing)
 * 
 * NO ANDROID DEPENDENCIES - Platform-agnostic!
 */
class InputController(
    private val project: Project,
    private val cursorContext: CursorContext,
    private val clipboard: ClipboardManager
) {
    // ========================================
    // SELECTION STATE (for copy/paste)
    // ========================================
    
    var selectionMode by mutableStateOf(false)
        private set
    
    var selectionStart: CursorPosition? by mutableStateOf(null)
        private set
    
    var selectionEnd: CursorPosition? by mutableStateOf(null)
        private set
    
    // ========================================
    // BUTTON HANDLERS
    // ========================================
    
    /**
     * Handle D-pad UP press
     * - Normal mode: Move cursor up
     * - Selection mode: Expand selection up
     * - Edit mode (A held): Increment value
     */
    fun handleDpadUp(isAButtonHeld: Boolean) {
        if (selectionMode) {
            expandSelectionUp()
        } else if (isAButtonHeld) {
            incrementValue()
        } else {
            moveCursorUp()
        }
    }
    
    /**
     * Handle D-pad DOWN press
     */
    fun handleDpadDown(isAButtonHeld: Boolean) {
        if (selectionMode) {
            expandSelectionDown()
        } else if (isAButtonHeld) {
            decrementValue()
        } else {
            moveCursorDown()
        }
    }
    
    /**
     * Handle D-pad LEFT press
     */
    fun handleDpadLeft(isAButtonHeld: Boolean) {
        if (selectionMode) {
            expandSelectionLeft()
        } else if (isAButtonHeld) {
            decrementValueFast() // Octave change for notes, cycle for others
        } else {
            moveCursorLeft()
        }
    }
    
    /**
     * Handle D-pad RIGHT press
     */
    fun handleDpadRight(isAButtonHeld: Boolean) {
        if (selectionMode) {
            expandSelectionRight()
        } else if (isAButtonHeld) {
            incrementValueFast()
        } else {
            moveCursorRight()
        }
    }
    
    /**
     * Handle A button press
     * - On empty row: Quick insert
     * - Otherwise: Enter edit mode (handled by A+direction combos)
     */
    fun handleAButton() {
        if (isCurrentRowEmpty()) {
            quickInsert()
        }
        // Edit mode is handled by A+direction combinations above
    }
    
    /**
     * Handle B button press
     * - Normal mode: Cancel/back
     * - Selection mode: Copy and exit
     */
    fun handleBButton() {
        if (selectionMode) {
            copySelectionAndExit()
        } else {
            // Cancel/back action (handled by screen logic)
        }
    }
    
    /**
     * Handle SELECT+B combination
     * Enters selection mode OR exits if already in selection mode
     */
    fun handleSelectB() {
        if (selectionMode) {
            exitSelectionMode()
        } else {
            enterSelectionMode()
        }
    }
    
    /**
     * Handle SELECT+A combination
     * Paste clipboard contents at cursor
     */
    fun handleSelectA() {
        pasteAtCursor()
    }
    
    /**
     * Handle A+B combination
     * Cut selection (copy + delete)
     */
    fun handleAB() {
        if (selectionMode) {
            cutSelection()
        }
    }
    
    // ========================================
    // CURSOR MOVEMENT
    // ========================================
    
    private fun moveCursorUp() {
        // Implementation depends on current screen
        // Update cursorContext.row, handle wrapping, etc.
    }
    
    private fun moveCursorDown() {
        // Similar to up
    }
    
    private fun moveCursorLeft() {
        // Column movement
    }
    
    private fun moveCursorRight() {
        // Column movement
    }
    
    // ========================================
    // VALUE EDITING
    // ========================================
    
    private fun incrementValue() {
        // Get current cell value
        // Increment (with wrapping)
        // Update project data
    }
    
    private fun decrementValue() {
        // Similar to increment
    }
    
    private fun incrementValueFast() {
        // For notes: +1 octave
        // For other values: +16 or cycle
    }
    
    private fun decrementValueFast() {
        // Similar to incrementValueFast
    }
    
    // ========================================
    // QUICK INSERT
    // ========================================
    
    private fun isCurrentRowEmpty(): Boolean {
        // Check if current cursor row is empty
        return false // Placeholder
    }
    
    private fun quickInsert() {
        // Insert last-used values at current position
        // Phrase: note + instrument + volume
        // Chain: phrase + transpose
        // Song: chain
    }
    
    // ========================================
    // SELECTION MODE (for copy/paste)
    // ========================================
    
    fun enterSelectionMode() {
        selectionMode = true
        selectionStart = getCurrentCursorPosition()
        selectionEnd = getCurrentCursorPosition()
    }
    
    fun exitSelectionMode() {
        selectionMode = false
        selectionStart = null
        selectionEnd = null
    }
    
    private fun expandSelectionUp() {
        val end = selectionEnd ?: return
        selectionEnd = end.copy(row = (end.row - 1).coerceAtLeast(0))
    }
    
    private fun expandSelectionDown() {
        val end = selectionEnd ?: return
        selectionEnd = end.copy(row = (end.row + 1).coerceAtMost(15))
    }
    
    private fun expandSelectionLeft() {
        val end = selectionEnd ?: return
        selectionEnd = end.copy(column = (end.column - 1).coerceAtLeast(0))
    }
    
    private fun expandSelectionRight() {
        val end = selectionEnd ?: return
        selectionEnd = end.copy(column = (end.column + 1).coerceAtMost(5))
    }
    
    /**
     * Get normalized selection (top-left to bottom-right)
     */
    fun getSelection(): Selection? {
        val start = selectionStart ?: return null
        val end = selectionEnd ?: return null
        
        return Selection(
            topLeft = CursorPosition(
                row = minOf(start.row, end.row),
                column = minOf(start.column, end.column),
                phraseId = start.phraseId
            ),
            bottomRight = CursorPosition(
                row = maxOf(start.row, end.row),
                column = maxOf(start.column, end.column),
                phraseId = start.phraseId
            )
        )
    }
    
    // ========================================
    // COPY/PASTE/CUT
    // ========================================
    
    private fun copySelectionAndExit() {
        val selection = getSelection() ?: return
        
        // Extract data from selection
        val data = extractSelectionData(selection)
        
        // Copy to clipboard
        clipboard.copy(
            type = determineClipboardType(selection),
            data = data,
            width = selection.width,
            height = selection.height
        )
        
        exitSelectionMode()
    }
    
    private fun pasteAtCursor() {
        clipboard.paste(
            project = project,
            target = getCurrentCursorPosition()
        )
    }
    
    private fun cutSelection() {
        copySelectionAndExit()
        deleteSelection()
    }
    
    private fun deleteSelection() {
        // Clear cells in selection
    }
    
    // ========================================
    // HELPERS
    // ========================================
    
    private fun getCurrentCursorPosition(): CursorPosition {
        return CursorPosition(
            row = cursorContext.row,
            column = cursorContext.column,
            phraseId = cursorContext.currentPhraseId
        )
    }
    
    private fun extractSelectionData(selection: Selection): Any {
        // Extract phrase steps, chain rows, etc. based on current screen
        return emptyList<Any>() // Placeholder
    }
    
    private fun determineClipboardType(selection: Selection): ClipboardType {
        // Based on current screen and selection
        return ClipboardType.PHRASE_STEPS
    }
}

// Data classes
data class CursorPosition(
    val row: Int,
    val column: Int,
    val phraseId: Int = 0
)

data class Selection(
    val topLeft: CursorPosition,
    val bottomRight: CursorPosition
) {
    val width: Int get() = bottomRight.column - topLeft.column + 1
    val height: Int get() = bottomRight.row - topLeft.row + 1
}
```

**Update MainActivity.kt:**

```kotlin
class MainActivity : ComponentActivity() {
    // Create controllers
    private lateinit var inputController: InputController
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        // Initialize controllers
        inputController = InputController(
            project = project,
            cursorContext = cursorContext,
            clipboard = clipboardManager
        )
        
        // ... rest of setup
    }
    
    // Delegate button handling to InputController
    private fun handleDpadUp() {
        inputController.handleDpadUp(isAButtonHeld = virtualControls.isAPressed)
    }
    
    // ... similar for all other buttons
}
```

**Test checklist:**
- [ ] Cursor movement works on all screens
- [ ] A+direction editing works
- [ ] Quick insert works
- [ ] Selection mode works (visual highlighting)
- [ ] Copy/paste buttons trigger correct actions

**Commit:**
```bash
git commit -m "[Refactor 4.1] Extract InputController from MainActivity

Created InputController.kt in core/logic/:
- All button handling logic
- Cursor movement
- Value editing (A+direction)
- Selection mode for copy/paste
- Quick insert feature

MainActivity now delegates to InputController.
NO Android dependencies in InputController!

Tested: ✅ All cursor movement works
Tested: ✅ Value editing works
Tested: ✅ Selection mode triggers"
```

**Time:** 1-2 days

---

### Step 4.2: Create PlaybackController

**Goal:** Extract playback scheduling logic from MainActivity

**What to extract:**
- `playPhrase()` - Schedule phrase notes
- `playChain()` - Schedule chain with transpose
- `playSong()` - Schedule 8-track song
- `stopPlayback()` - Clear queue
- Playback state tracking

**Create file:** `core/logic/PlaybackController.kt`

```kotlin
package com.yourpackage.pockettracker.core.logic

import androidx.compose.runtime.*
import com.yourpackage.pockettracker.core.data.*
import com.yourpackage.pockettracker.core.audio.IAudioBackend

/**
 * Handles all playback scheduling (phrase, chain, song)
 * 
 * NO ANDROID DEPENDENCIES - Platform-agnostic!
 */
class PlaybackController(
    private val audioBackend: IAudioBackend
) {
    // ========================================
    // PLAYBACK STATE
    // ========================================
    
    var isPlaying by mutableStateOf(false)
        private set
    
    var currentPlaybackType by mutableStateOf(PlaybackType.NONE)
        private set
    
    var playbackCursor by mutableStateOf(PlaybackCursor())
        private set
    
    // ========================================
    // PHRASE PLAYBACK
    // ========================================
    
    /**
     * Play a single phrase with looping
     */
    fun playPhrase(project: Project, phraseId: Int, tempo: Int) {
        stopPlayback() // Clear any existing playback
        
        val phrase = project.phrases[phraseId]
        val stepDuration = calculateFramesPerStep(tempo)
        val startFrame = audioBackend.getCurrentFrame()
        
        // Schedule all 16 steps
        phrase.steps.forEachIndexed { stepIndex, step ->
            if (step.note != Note.EMPTY) {
                val triggerFrame = startFrame + (stepIndex * stepDuration)
                val instrument = project.instruments[step.instrument]
                
                audioBackend.scheduleNote(
                    frame = triggerFrame,
                    sampleId = instrument.sampleId,
                    trackId = 0,
                    frequency = step.note.toFrequency(),
                    volume = step.volume.toFloat() / 255f
                )
            }
        }
        
        isPlaying = true
        currentPlaybackType = PlaybackType.PHRASE
    }
    
    // ========================================
    // CHAIN PLAYBACK
    // ========================================
    
    /**
     * Play a chain (16 phrase references with transpose)
     */
    fun playChain(project: Project, chainId: Int, tempo: Int) {
        stopPlayback()
        
        val chain = project.chains[chainId]
        val stepDuration = calculateFramesPerStep(tempo)
        val startFrame = audioBackend.getCurrentFrame()
        
        chain.rows.forEachIndexed { rowIndex, row ->
            if (row.phraseId != 0xFF) { // 0xFF = empty
                val phrase = project.phrases[row.phraseId]
                val transposeOffset = row.transpose // Semitones
                
                phrase.steps.forEachIndexed { stepIndex, step ->
                    if (step.note != Note.EMPTY) {
                        val globalStep = rowIndex * 16 + stepIndex
                        val triggerFrame = startFrame + (globalStep * stepDuration)
                        val instrument = project.instruments[step.instrument]
                        
                        // Apply transpose
                        val transposedNote = step.note.transpose(transposeOffset)
                        
                        audioBackend.scheduleNote(
                            frame = triggerFrame,
                            sampleId = instrument.sampleId,
                            trackId = 0,
                            frequency = transposedNote.toFrequency(),
                            volume = step.volume.toFloat() / 255f
                        )
                    }
                }
            }
        }
        
        isPlaying = true
        currentPlaybackType = PlaybackType.CHAIN
    }
    
    // ========================================
    // SONG PLAYBACK
    // ========================================
    
    /**
     * Play full song (8 tracks polyphonic)
     */
    fun playSong(project: Project, tempo: Int) {
        stopPlayback()
        
        val song = project.song
        val stepDuration = calculateFramesPerStep(tempo)
        val startFrame = audioBackend.getCurrentFrame()
        
        // For each track (0-7)
        song.tracks.forEachIndexed { trackId, track ->
            track.chainRefs.forEachIndexed { chainIndex, chainRef ->
                if (chainRef.chainId != 0xFF) { // 0xFF = empty
                    val chain = project.chains[chainRef.chainId]
                    
                    chain.rows.forEachIndexed { rowIndex, row ->
                        if (row.phraseId != 0xFF) {
                            val phrase = project.phrases[row.phraseId]
                            val transposeOffset = row.transpose
                            
                            phrase.steps.forEachIndexed { stepIndex, step ->
                                if (step.note != Note.EMPTY) {
                                    // Calculate global position
                                    val globalStep = 
                                        chainIndex * 256 +  // Chains before this
                                        rowIndex * 16 +      // Phrase rows before this
                                        stepIndex            // Step in phrase
                                    
                                    val triggerFrame = startFrame + (globalStep * stepDuration)
                                    val instrument = project.instruments[step.instrument]
                                    val transposedNote = step.note.transpose(transposeOffset)
                                    
                                    audioBackend.scheduleNote(
                                        frame = triggerFrame,
                                        sampleId = instrument.sampleId,
                                        trackId = trackId,  // Track-specific voice
                                        frequency = transposedNote.toFrequency(),
                                        volume = step.volume.toFloat() / 255f
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
        
        isPlaying = true
        currentPlaybackType = PlaybackType.SONG
    }
    
    // ========================================
    // STOP PLAYBACK
    // ========================================
    
    fun stopPlayback() {
        audioBackend.stopAll()
        audioBackend.clearScheduledNotes()
        isPlaying = false
        currentPlaybackType = PlaybackType.NONE
    }
    
    // ========================================
    // HELPERS
    // ========================================
    
    /**
     * Calculate frames per step based on tempo
     * 
     * At 120 BPM: 1 beat = 0.5 seconds = 22050 frames (at 44100Hz)
     * 1 step = 1 beat (simplification for now)
     */
    private fun calculateFramesPerStep(tempo: Int): Long {
        val sampleRate = audioBackend.getSampleRate()
        val secondsPerBeat = 60.0 / tempo
        return (secondsPerBeat * sampleRate).toLong()
    }
}

// Playback state types
enum class PlaybackType {
    NONE,
    PHRASE,
    CHAIN,
    SONG
}

data class PlaybackCursor(
    val phrase: Int = 0,
    val step: Int = 0,
    val chain: Int = 0,
    val track: Int = 0
)
```

**Update MainActivity.kt:**

```kotlin
class MainActivity : ComponentActivity() {
    private lateinit var playbackController: PlaybackController
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        // Initialize
        playbackController = PlaybackController(audioBackend = oboeAudioBackend)
        
        // ... rest of setup
    }
    
    // Delegate playback to PlaybackController
    private fun playPhrase() {
        playbackController.playPhrase(project, cursorContext.currentPhraseId, project.tempo)
    }
    
    private fun playChain() {
        playbackController.playChain(project, cursorContext.currentChainId, project.tempo)
    }
    
    private fun playSong() {
        playbackController.playSong(project, project.tempo)
    }
    
    private fun stopPlayback() {
        playbackController.stopPlayback()
    }
}
```

**Test checklist:**
- [ ] Phrase playback works
- [ ] Chain playback works (with transpose)
- [ ] Song playback works (8 tracks)
- [ ] Stop works (clears queue)
- [ ] Audio quality unchanged

**Commit:**
```bash
git commit -m "[Refactor 4.2] Extract PlaybackController from MainActivity

Created PlaybackController.kt in core/logic/:
- Phrase playback scheduling
- Chain playback with transpose
- Song playback (8 tracks)
- Frame-precise note triggering

MainActivity now delegates to PlaybackController.
Uses IAudioBackend interface (portable!).

Tested: ✅ Phrase playback works
Tested: ✅ Chain playback works
Tested: ✅ Song playback works"
```

**Time:** 2 days

---

### Step 4.3: Create EffectProcessor

**Goal:** Prepare effect processing system (effects not implemented yet, but structure ready)

**What to create:**
- Effect type enum
- Effect processor class (stubs for now)
- Integration points with playback

**Create file:** `core/logic/EffectProcessor.kt`

```kotlin
package com.yourpackage.pockettracker.core.logic

import com.yourpackage.pockettracker.core.data.*
import com.yourpackage.pockettracker.core.audio.IAudioBackend

/**
 * Processes effects commands and applies them to scheduled notes
 * 
 * NO ANDROID DEPENDENCIES - Platform-agnostic!
 * 
 * NOTE: Effects implementation comes in MVP Milestone 2
 * This is just the structure/stubs for now
 */
class EffectProcessor(
    private val audioBackend: IAudioBackend
) {
    /**
     * Apply effects from a phrase step
     * 
     * @param step The phrase step with effect commands
     * @param baseFrame When this step triggers
     * @param stepDuration Duration of step in frames
     * @param trackId Which track (0-7)
     */
    fun applyEffects(
        step: PhraseStep,
        baseFrame: Long,
        stepDuration: Long,
        trackId: Int,
        baseFrequency: Float,
        baseVolume: Float,
        sampleId: Int
    ) {
        // Process FX1
        if (step.fx1Type != 0x00) {
            applyEffect(
                type = step.fx1Type,
                value = step.fx1Value,
                frame = baseFrame,
                duration = stepDuration,
                trackId = trackId,
                frequency = baseFrequency,
                volume = baseVolume,
                sampleId = sampleId
            )
        }
        
        // Process FX2
        if (step.fx2Type != 0x00) {
            applyEffect(
                type = step.fx2Type,
                value = step.fx2Value,
                frame = baseFrame,
                duration = stepDuration,
                trackId = trackId,
                frequency = baseFrequency,
                volume = baseVolume,
                sampleId = sampleId
            )
        }
        
        // Process FX3
        if (step.fx3Type != 0x00) {
            applyEffect(
                type = step.fx3Type,
                value = step.fx3Value,
                frame = baseFrame,
                duration = stepDuration,
                trackId = trackId,
                frequency = baseFrequency,
                volume = baseVolume,
                sampleId = sampleId
            )
        }
    }
    
    /**
     * Apply single effect
     */
    private fun applyEffect(
        type: Int,
        value: Int,
        frame: Long,
        duration: Long,
        trackId: Int,
        frequency: Float,
        volume: Float,
        sampleId: Int
    ) {
        when (type) {
            FX_ARPEGGIO -> applyArpeggio(value, frame, duration, trackId, frequency, volume, sampleId)
            FX_OFFSET -> applyOffset(value, frame, trackId, frequency, volume, sampleId)
            FX_VOLUME -> applyVolume(value, frame, duration, trackId, frequency, sampleId)
            FX_KILL -> applyKill(frame, trackId)
            FX_REPEAT -> applyRepeat(value, frame, duration, trackId, frequency, volume, sampleId)
            // More effects added in Milestone 2
        }
    }
    
    // ========================================
    // EFFECT IMPLEMENTATIONS (STUBS FOR NOW)
    // ========================================
    
    /**
     * Arpeggio: Play note + semitone1 + semitone2 in sequence
     * Value format: high nibble = semitone1, low nibble = semitone2
     */
    private fun applyArpeggio(
        value: Int,
        frame: Long,
        duration: Long,
        trackId: Int,
        baseFreq: Float,
        volume: Float,
        sampleId: Int
    ) {
        // TODO: Implement in Milestone 2
        // Extract semitones from value
        // Schedule 3 notes within step duration
    }
    
    /**
     * Offset: Override sample start point
     * Value: 0x00-0xFF maps to 0%-100% of sample length
     */
    private fun applyOffset(
        value: Int,
        frame: Long,
        trackId: Int,
        frequency: Float,
        volume: Float,
        sampleId: Int
    ) {
        // TODO: Implement in Milestone 2
        // Calculate new start point
        // Schedule note with offset
    }
    
    /**
     * Volume: Volume automation within step
     * Value: Target volume (0x00-0xFF)
     */
    private fun applyVolume(
        value: Int,
        frame: Long,
        duration: Long,
        trackId: Int,
        frequency: Float,
        sampleId: Int
    ) {
        // TODO: Implement in Milestone 2
        // Apply volume ramp over step duration
    }
    
    /**
     * Kill: Stop voice immediately
     */
    private fun applyKill(frame: Long, trackId: Int) {
        // TODO: Implement in Milestone 2
        // Stop voice at frame
    }
    
    /**
     * Repeat: Retrigger sample N times within step
     * Value: Repeat count (0x01-0xFF)
     */
    private fun applyRepeat(
        value: Int,
        frame: Long,
        duration: Long,
        trackId: Int,
        frequency: Float,
        volume: Float,
        sampleId: Int
    ) {
        // TODO: Implement in Milestone 2
        // Schedule N triggers within duration
    }
    
    companion object {
        // Effect type constants
        const val FX_NONE = 0x00
        const val FX_ARPEGGIO = 0x01
        const val FX_OFFSET = 0x02
        const val FX_VOLUME = 0x03
        const val FX_KILL = 0x04
        const val FX_REPEAT = 0x05
        // More effects added later
    }
}
```

**Update PlaybackController.kt:**

```kotlin
class PlaybackController(
    private val audioBackend: IAudioBackend,
    private val effectProcessor: EffectProcessor  // Add this
) {
    fun playPhrase(project: Project, phraseId: Int, tempo: Int) {
        // ... existing code ...
        
        phrase.steps.forEachIndexed { stepIndex, step ->
            if (step.note != Note.EMPTY) {
                val triggerFrame = startFrame + (stepIndex * stepDuration)
                val instrument = project.instruments[step.instrument]
                val frequency = step.note.toFrequency()
                val volume = step.volume.toFloat() / 255f
                
                // Schedule base note
                audioBackend.scheduleNote(
                    frame = triggerFrame,
                    sampleId = instrument.sampleId,
                    trackId = 0,
                    frequency = frequency,
                    volume = volume
                )
                
                // Apply effects (if any)
                effectProcessor.applyEffects(
                    step = step,
                    baseFrame = triggerFrame,
                    stepDuration = stepDuration,
                    trackId = 0,
                    baseFrequency = frequency,
                    baseVolume = volume,
                    sampleId = instrument.sampleId
                )
            }
        }
    }
}
```

**Test checklist:**
- [ ] Code compiles
- [ ] Playback still works (effects are stubs, so no change expected)
- [ ] No crashes

**Commit:**
```bash
git commit -m "[Refactor 4.3] Create EffectProcessor structure

Created EffectProcessor.kt in core/logic/:
- Effect type constants
- applyEffects() entry point
- Stub implementations (filled in Milestone 2)

PlaybackController now calls EffectProcessor.
Ready for effects implementation!

Tested: ✅ Compiles
Tested: ✅ Playback unchanged (stubs only)"
```

**Time:** 1 day

---

### Step 4.4: Create InstrumentController

**Goal:** Extract instrument and sample management logic

**Create file:** `core/logic/InstrumentController.kt`

```kotlin
package com.yourpackage.pockettracker.core.logic

import com.yourpackage.pockettracker.core.data.*
import com.yourpackage.pockettracker.core.audio.IAudioBackend
import com.yourpackage.pockettracker.core.resources.IResourceLoader

/**
 * Manages instruments and sample loading
 * 
 * NO ANDROID DEPENDENCIES - Platform-agnostic!
 */
class InstrumentController(
    private val project: Project,
    private val audioBackend: IAudioBackend,
    private val resourceLoader: IResourceLoader
) {
    /**
     * Load sample from file into instrument
     */
    fun loadSampleIntoInstrument(
        instrumentId: Int,
        samplePath: String
    ): LoadResult {
        return try {
            val (samples, baseFreq) = resourceLoader.loadWavFromFile(samplePath)
            
            // Load into audio engine
            audioBackend.loadSample(instrumentId, samples)
            
            // Update instrument
            val instrument = project.instruments[instrumentId]
            instrument.sampleId = instrumentId
            instrument.samplePath = samplePath
            instrument.baseFrequency = baseFreq
            
            LoadResult.Success
        } catch (e: Exception) {
            LoadResult.Error(e.message ?: "Unknown error")
        }
    }
    
    /**
     * Preview instrument (play at ROOT note)
     */
    fun previewInstrument(instrumentId: Int) {
        val instrument = project.instruments[instrumentId]
        
        audioBackend.scheduleNote(
            frame = audioBackend.getCurrentFrame(),
            sampleId = instrument.sampleId,
            trackId = 0,
            frequency = 261.63f, // C-4
            volume = 1.0f
        )
    }
    
    sealed class LoadResult {
        object Success : LoadResult()
        data class Error(val message: String) : LoadResult()
    }
}
```

**Update MainActivity.kt:**

```kotlin
private lateinit var instrumentController: InstrumentController

instrumentController = InstrumentController(
    project = project,
    audioBackend = oboeAudioBackend,
    resourceLoader = androidResourceLoader
)

// Use it
fun loadSampleIntoSlot(path: String, slotId: Int) {
    when (val result = instrumentController.loadSampleIntoInstrument(slotId, path)) {
        is Success -> showMessage("Sample loaded!")
        is Error -> showMessage("Error: ${result.message}")
    }
}
```

**Time:** 1 day

---

### Step 4.5: Create FileController

**Goal:** Extract save/load logic

**Create file:** `core/logic/FileController.kt`

```kotlin
package com.yourpackage.pockettracker.core.logic

import com.yourpackage.pockettracker.core.data.Project
import com.yourpackage.pockettracker.core.storage.IFileSystem
import kotlinx.serialization.json.Json
import kotlinx.serialization.encodeToString
import kotlinx.serialization.decodeFromString

/**
 * Handles project save/load operations
 * 
 * NO ANDROID DEPENDENCIES - Platform-agnostic!
 */
class FileController(
    private val fileSystem: IFileSystem
) {
    private val json = Json {
        prettyPrint = true
        ignoreUnknownKeys = true
    }
    
    /**
     * Save project to file
     */
    fun saveProject(project: Project, filename: String): SaveResult {
        return try {
            val jsonString = json.encodeToString(project)
            val path = "${fileSystem.getProjectsDirectory()}/$filename.ptp"
            
            fileSystem.writeFile(path, jsonString)
            
            SaveResult.Success(path)
        } catch (e: Exception) {
            SaveResult.Error(e.message ?: "Save failed")
        }
    }
    
    /**
     * Load project from file
     */
    fun loadProject(filename: String): LoadResult {
        return try {
            val path = "${fileSystem.getProjectsDirectory()}/$filename.ptp"
            val jsonString = fileSystem.readFile(path)
            val project = json.decodeFromString<Project>(jsonString)
            
            LoadResult.Success(project)
        } catch (e: Exception) {
            LoadResult.Error(e.message ?: "Load failed")
        }
    }
    
    sealed class SaveResult {
        data class Success(val path: String) : SaveResult()
        data class Error(val message: String) : SaveResult()
    }
    
    sealed class LoadResult {
        data class Success(val project: Project) : LoadResult()
        data class Error(val message: String) : LoadResult()
    }
}
```

**Time:** 1 day

---

### Step 4.6: Create ClipboardManager

**Goal:** Handle copy/paste operations (implementation in Milestone 2.5)

**Create file:** `core/logic/ClipboardManager.kt`

```kotlin
package com.yourpackage.pockettracker.core.logic

import com.yourpackage.pockettracker.core.data.*

/**
 * Manages clipboard for copy/paste operations
 * 
 * NO ANDROID DEPENDENCIES - Platform-agnostic!
 */
class ClipboardManager {
    data class ClipboardData(
        val type: ClipboardType,
        val data: Any,
        val width: Int,
        val height: Int
    )
    
    enum class ClipboardType {
        PHRASE_STEPS,
        PHRASE,
        CHAIN_ROWS,
        CHAIN
    }
    
    var clipboard: ClipboardData? = null
        private set
    
    /**
     * Copy data to clipboard
     */
    fun copy(type: ClipboardType, data: Any, width: Int, height: Int) {
        clipboard = ClipboardData(type, data, width, height)
    }
    
    /**
     * Paste clipboard contents (implementation in Milestone 2.5)
     */
    fun paste(project: Project, target: CursorPosition): PasteResult {
        val clip = clipboard ?: return PasteResult.NoClipboard
        
        // TODO: Implement in Milestone 2.5
        return PasteResult.Success(0)
    }
    
    sealed class PasteResult {
        object NoClipboard : PasteResult()
        data class Success(val itemsPasted: Int) : PasteResult()
        data class Error(val message: String) : PasteResult()
    }
}
```

**Time:** 1 day

---

### Step 4.7: Create TrackerController (Coordinator)

**Goal:** Main coordinator that owns all state and delegates to specialist controllers

**Create file:** `core/logic/TrackerController.kt`

```kotlin
package com.yourpackage.pockettracker.core.logic

import androidx.compose.runtime.*
import com.yourpackage.pockettracker.core.data.*

/**
 * Main coordinator for all tracker logic
 * Owns state, delegates operations to specialist controllers
 * 
 * NO ANDROID DEPENDENCIES - Platform-agnostic!
 */
class TrackerController(
    val inputController: InputController,
    val playbackController: PlaybackController,
    val effectProcessor: EffectProcessor,
    val instrumentController: InstrumentController,
    val fileController: FileController,
    val clipboardManager: ClipboardManager
) {
    // ========================================
    // GLOBAL STATE (owned by coordinator)
    // ========================================
    
    var project by mutableStateOf(Project())
    var currentScreen by mutableStateOf(ScreenType.PHRASE)
    var cursorContext by mutableStateOf(CursorContext())
    
    // ========================================
    // DELEGATION (pass to specialist controllers)
    // ========================================
    
    // Input delegation
    fun handleDpadUp(isAHeld: Boolean) = 
        inputController.handleDpadUp(isAHeld)
    
    fun handleDpadDown(isAHeld: Boolean) = 
        inputController.handleDpadDown(isAHeld)
    
    // ... all other button handlers
    
    // Playback delegation
    fun playPhrase() = 
        playbackController.playPhrase(project, cursorContext.currentPhraseId, project.tempo)
    
    fun playChain() = 
        playbackController.playChain(project, cursorContext.currentChainId, project.tempo)
    
    fun playSong() = 
        playbackController.playSong(project, project.tempo)
    
    fun stopPlayback() = 
        playbackController.stopPlayback()
    
    // File delegation
    fun saveProject(filename: String) = 
        fileController.saveProject(project, filename)
    
    fun loadProject(filename: String) = 
        fileController.loadProject(filename)
    
    // Instrument delegation
    fun loadSample(path: String, slotId: Int) = 
        instrumentController.loadSampleIntoInstrument(slotId, path)
    
    fun previewInstrument(id: Int) = 
        instrumentController.previewInstrument(id)
    
    // Screen navigation (coordinator responsibility)
    fun navigateToScreen(screen: ScreenType) {
        currentScreen = screen
    }
}
```

**Update MainActivity.kt:**

```kotlin
class MainActivity : ComponentActivity() {
    // Replace individual controllers with single coordinator
    private lateinit var trackerController: TrackerController
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        // Create all controllers
        val clipboard = ClipboardManager()
        val inputController = InputController(project, cursorContext, clipboard)
        val effectProcessor = EffectProcessor(oboeAudioBackend)
        val playbackController = PlaybackController(oboeAudioBackend, effectProcessor)
        val instrumentController = InstrumentController(project, oboeAudioBackend, androidResourceLoader)
        val fileController = FileController(androidFileSystem)
        
        // Create coordinator
        trackerController = TrackerController(
            inputController = inputController,
            playbackController = playbackController,
            effectProcessor = effectProcessor,
            instrumentController = instrumentController,
            fileController = fileController,
            clipboardManager = clipboard
        )
        
        // Now MainActivity is THIN - just delegates!
    }
    
    // All button handlers delegate to coordinator
    private fun handleDpadUp() {
        trackerController.handleDpadUp(virtualControls.isAPressed)
    }
    
    // ... etc
}
```

**Test checklist:**
- [ ] All features still work
- [ ] MainActivity is now <500 lines (was 2570!)
- [ ] No Android imports in any controller
- [ ] Code is clean and organized

**Commit:**
```bash
git commit -m "[Refactor 4.7] Create TrackerController coordinator

Created TrackerController.kt - main coordinator:
- Owns global state (project, screen, cursor)
- Delegates to 6 specialist controllers
- MainActivity now THIN (~300 lines)

Refactoring Phase 4 COMPLETE! ✅

Controllers created:
- TrackerController (coordinator)
- InputController (buttons)
- PlaybackController (playback)
- EffectProcessor (effects structure)
- InstrumentController (samples)
- FileController (save/load)
- ClipboardManager (copy/paste structure)

All platform-agnostic (NO Android dependencies)!

Tested: ✅ All features work
Tested: ✅ Code much cleaner"
```

**Time:** 1-2 days

---

**PHASE 4 COMPLETE!** ✅

**Results:**
- MainActivity: 2570 lines → ~300 lines
- 7 clean, focused controllers
- All code portable (ready for Linux)
- Effects and copy/paste structures ready

**Next:** Implement effects system (Milestone 2) and copy/paste (Milestone 2.5)!

---

## END OF PHASE 4 ADDITION


---

## Testing Strategy

### After EACH Step

**Minimal Testing (Every Commit):**
```
1. Code compiles without errors ✅
2. App installs and launches ✅
3. No immediate crashes ✅
```

### After EACH Phase

**Full Functionality Testing:**
```
[ ] Navigate to all screens (SONG, CHAIN, PHRASE, INSTRUMENT, PROJECT)
[ ] Phrase playback works
[ ] Chain playback works
[ ] Song playback works (8 tracks)
[ ] Load sample from file browser
[ ] Sample preview in file browser works
[ ] Sample preview in instrument screen works
[ ] Save project
[ ] Load project
[ ] Cursor navigation works on all screens
[ ] A+direction editing works
[ ] Virtual buttons work (on touchscreen)
[ ] Physical buttons work (on handheld)
```

### Final Integration Test (After All Phases)

**Comprehensive Workflow Test:**
1. Create new project
2. Load 3 custom samples
3. Create phrase with 8 notes using different instruments
4. Create chain with 4 phrases + transpose
5. Create song with 3 chains on different tracks
6. Play song - verify all tracks play correctly
7. Save project as "test_refactor.ptp"
8. Close app
9. Reopen app
10. Load "test_refactor.ptp"
11. Verify all data loaded correctly
12. Play song again - should sound identical

**Success Criteria:**
- ✅ All features work EXACTLY as before refactoring
- ✅ No new crashes or bugs
- ✅ Performance is same or better
- ✅ Code is cleaner and more organized

---

## Git Workflow

### Branch Strategy

```
main
 └── refactor/portable-architecture
      ├── refactor/phase-1-audio
      ├── refactor/phase-2-resources
      ├── refactor/phase-3-files
      └── refactor/phase-4-logic
```

### Commit Messages Format

```
[Refactor Step X.Y] Brief description

Detailed explanation:
- What changed
- Why it changed
- What was tested

Status: ✅ Tested and working / ⚠️ Needs more testing
```

### Example Commits

```bash
git commit -m "[Refactor Step 1.1] Add IAudioBackend interface

Created platform-agnostic audio backend interface.
This will allow swapping Oboe (Android) for ALSA (Linux) later.

No functional changes yet - just added interface definition.

Status: ✅ Compiles successfully"
```

```bash
git commit -m "[Refactor Step 1.6] Update MainActivity to use new AudioEngine

- Replaced TrackerAudioEngine with AudioEngine + OboeAudioBackend
- Added DisposableEffect for proper cleanup
- Tested phrase/chain/song playback

Status: ✅ Fully tested on RG353V, all playback works"
```

### Recovery Plan (If Something Breaks)

**If step breaks app:**
```bash
# Option 1: Revert last commit
git revert HEAD

# Option 2: Reset to previous working commit
git reset --hard HEAD~1

# Option 3: Go back to pre-refactor tag
git checkout v0.9-pre-refactor
```

**Then:** Debug the issue, fix it, re-commit.

---

## Progress Tracking

Copy this checklist to track progress:

```
PHASE 1: AUDIO BACKEND ABSTRACTION
[ ] Step 1.1: Create IAudioBackend interface
[ ] Step 1.2: Create OboeAudioBackend
[ ] Step 1.3: Add native_close() to C++
[ ] Step 1.4: Verify CMake builds
[ ] Step 1.5: Refactor TrackerAudioEngine to AudioEngine
[ ] Step 1.6: Update MainActivity
[ ] Step 1.7: Create IResourceLoader interface
[ ] Step 1.8: Create AndroidResourceLoader
[ ] Step 1.9: Fix sample rate handling
[ ] PHASE 1 COMPLETE ✅ - Test all audio features!

PHASE 2: RESOURCE LOADING ABSTRACTION
[ ] Step 2.1: ...
[ ] (continue...)

PHASE 3: FILE I/O ABSTRACTION
[ ] Step 3.1: ...

PHASE 4: BUSINESS LOGIC EXTRACTION
[ ] Step 4.1: ...

REFACTORING COMPLETE! 🎉
[ ] All tests pass
[ ] Code review with mentor
[ ] Merge to main
[ ] Tag as v1.0-portable
```

---

## What to Do If You Get Stuck

1. **Don't panic** - refactoring is hard, getting stuck is normal
2. **Commit current state** - even if broken, save your work
3. **Ask for help** - use Claude Code, your mentor, or community
4. **Take a break** - fresh eyes help solve problems
5. **Rollback if needed** - it's okay to undo and try different approach

**Remember:** The goal is not speed, it's correctness. Take your time!

---

## Next Document

See **MVP_ROADMAP.md** for vertical slices to implement AFTER refactoring.

---

**Version History:**
- v1.0 (2025-01-01): Initial refactoring roadmap
