# PocketTracker - Technical Architecture

## Document Purpose
This document defines **HOW** PocketTracker is built technically. It covers current architecture, planned refactoring for portability, and technical decisions.

**Last Updated:** 2026-06-06
**Version:** 2.6
**Audience:** Developers, Contributors, Claude Code AI

---

## Table of Contents
1. [Platform Strategy](#platform-strategy)
2. [Current Architecture](#current-architecture)
3. [Target Architecture (After Refactoring)](#target-architecture-after-refactoring)
4. [Audio Engine](#audio-engine)
5. [Data Model](#data-model)
6. [Rendering System](#rendering-system)
7. [Navigation System](#navigation-system)
8. [File Management](#file-management)
9. [Build System](#build-system)
10. [Modulation Engine](#modulation-engine)
11. [Technology Stack](#technology-stack)

---

## Platform Strategy

### Multi-Platform Vision

**Primary Platform:** Android (Kotlin + Jetpack Compose)  
**Future Platform:** Linux (C++ + GTK/Qt/SDL2)  
**Architecture Approach:** Portable Core + Platform Adapters (Variant B)

### Why Multi-Platform Architecture NOW?

**Context from developer:**
> "Linux port is a real plan. My mentor will help after MVP. We should think about how to write code, and if something already went wrong - optimize it."

**Decision:** Implement proper abstraction layers during MVP development to avoid massive rewrite later.

**Benefits:**
- Shared C++ audio core (already exists!)
- Business logic written once, works on both platforms
- Easier to onboard contributors (clear separation of concerns)
- Mentor can work on Linux port without touching Android-specific code

**Trade-offs:**
- MVP takes 1-2 weeks longer due to refactoring
- More upfront architectural complexity
- Need to maintain discipline (don't mix platform code with logic)

**Developer's stance:** "I'm primarily making this for myself, I'm not reporting deadlines to anyone except myself, I have enough enthusiasm, and my mentor isn't going anywhere" → **Refactoring is worth it!**

---

## Current Architecture (Post-Refactoring — March 2026)

### Current State (March 2026 — REFACTORING COMPLETE)

The refactoring is **complete**. The codebase now matches the Target Architecture described below.

```
PocketTracker/
├── core/
│   ├── audio/
│   │   ├── IAudioBackend.kt        ✅ Interface — portable
│   │   └── AudioEngine.kt          ✅ Platform-agnostic coordinator
│   ├── data/
│   │   └── TrackerData.kt          ✅ Pure data structures (PORTABLE)
│   ├── logic/
│   │   ├── TrackerController.kt    ✅ Navigation, screen state
│   │   ├── InputController.kt      ✅ Button handling, selection
│   │   ├── PlaybackController.kt   ✅ Phrase/chain/song scheduling
│   │   ├── EffectProcessor.kt      ✅ All effect calculations
│   │   ├── InstrumentController.kt ✅ Sample management, resampling
│   │   ├── FileController.kt       ✅ Save/load orchestration
│   │   └── ClipboardManager.kt     ✅ Copy/paste
│   ├── resources/
│   │   └── IResourceLoader.kt      ✅ Sample/asset loading interface
│   └── storage/
│       ├── IFileSystem.kt          ✅ File I/O interface
│       └── FileInfo.kt             ✅ Platform-agnostic file metadata
│
├── input/
│   ├── AppInputDispatcher.kt       Button-handler logic; wired via AppControllers + AppStateRefs
│   ├── ButtonHandlers.kt           Input mapping, key combos, key repeat
│   └── CursorContext.kt            Value-type system + factory methods
│
├── platform/android/
│   ├── OboeAudioBackend.kt         ✅ Oboe JNI implementation
│   ├── AndroidResourceLoader.kt    ✅ R.raw.* loader
│   ├── AndroidFileSystem.kt        ✅ Scoped storage implementation
│   └── DeviceAdapter.kt            Android InputDevice API
│
├── ui/
│   ├── EditorHelpers.kt            ✅ Shared rendering utilities (toHex2, toHex8, rowBgColor, darken(), clearEffect…)
│   ├── PixelPerfectRenderer.kt     Compose rendering + pixel font
│   └── modules/
│       ├── PhraseEditorModule.kt   ✅ Portable rendering
│       ├── ChainEditorModule.kt    ✅
│       ├── SongEditorModule.kt     ✅
│       ├── InstrumentModule.kt     ✅
│       ├── InstrumentPoolModule.kt ✅ Instrument Pool overview (NAME + V/RV/DE/EQ per slot)
│       ├── SampleEditorModule.kt   ✅ Full-screen waveform editor
│       ├── TableModule.kt          ✅
│       ├── GrooveModule.kt         ✅
│       ├── ModulationModule.kt     ✅
│       ├── MixerModule.kt          ✅
│       ├── EffectModule.kt         ✅ Global send effects (reverb/delay/EQ config)
│       ├── EqModule.kt             ✅ 3-band parametric EQ editor (overlay screen)
│       ├── SettingsModule.kt       ✅ Layout/scaling/overlay/haptics/cursor settings (12 rows)
│       └── ProjectModule.kt        ✅
│
├── MainActivity.kt                 ✅ Root package — thin coordinator (backends + UI wiring)
│
└── app/src/main/cpp/               C++ audio engine
    ├── audio-engine.cpp / .h       PORTABLE core (no Oboe): processAudioBlock, processLiveBlock, renderOffline
    ├── oboe-audio-engine.cpp / .h  Android backend: owns the Oboe stream, onAudioReady → core.processLiveBlock (only Oboe TU)
    ├── jni-bridge.cpp              JNI entry points only (thin wrapper)
    ├── native-audio.cpp            15-line stub redirect (legacy entry point)
    ├── sampler-voice.h             Per-voice state for sample-playback voices
    ├── soundfont-voice.h / .cpp    Per-voice state for SF2/TinySoundFont voices
    ├── note-queue.h                Sample-accurate note scheduling queue
    ├── audio-defs.h                PARAM_* constants, voice structs
    ├── mods/                       Modulation engine (split out of audio-engine.cpp)
    │   ├── mod-system.h            Routing (modSourceValues → modDestValues)
    │   ├── mod-runner.h            runModMatrix() orchestration
    │   ├── modules/                AHD/ADSR/LFO/pitch-slide/vibrato tick functions
    │   └── primitives/             lfo-oscillator.h (shared LFO/vibrato shaping)
    ├── vendor/
    │   └── tsf/tsf.h               TinySoundFont (single-header SF2 synth)
    └── effects/                    DSP module system (three-layer architecture)
        ├── instrument-chain.h      Per-voice chain: Crush → Drive → Filter
        ├── send-chain.h            Stereo send buses: reverb (DaisySP ReverbSc) + delay (ping-pong)
        ├── master-chain.h          Final output bus: masterEq → OttModule|DustChain → LimiterModule
        ├── primitives/
        │   ├── biquad.h            BiquadState: state-only, coeffs passed at call time
        │   ├── filter.h            calculateBiquadCoeffs() (Audio EQ Cookbook)
        │   └── daisysp/            Vendored DaisySP (MIT): svf.h, svf.cpp, dsp.h
        └── modules/
            ├── filter-module.h     FilterModule: LP/HP/BP via daisysp::Svf, setParams() + processMono/Stereo
            ├── drive-module.h      DriveModule: tanh soft clipper, stateless
            └── crush-module.h      BitcrushModule: bit-depth quantizer, stateless

---

## Target Architecture (After Refactoring)

> **Historical / forward-looking.** This was the original refactoring target and the Linux-port plan.
> The refactoring is done — the **Current Architecture** file tree above is the authoritative present-day
> layout. The diagrams below are kept for the cross-platform layering rationale (some still name
> `native-audio.cpp`, now a 15-line stub; the real engine is `audio-engine.cpp`).

### Layered Architecture with Platform Abstraction

```
┌─────────────────────────────────────────────────────┐
│             PRESENTATION LAYER                      │
│  (Platform-Specific UI)                             │
│                                                      │
│  Android: Jetpack Compose    Linux: GTK/Qt/SDL2     │
│  - MainActivity.kt           - main.cpp             │
│  - VirtualControls.kt        - LinuxUI.cpp          │
│  - DeviceAdapter.kt          - LinuxInput.cpp       │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│             BUSINESS LOGIC LAYER                    │
│  (Platform-Agnostic Kotlin/C++)                     │
│                                                      │
│  - TrackerController.kt ← All button handlers       │
│  - EffectProcessor.kt   ← Effect calculations       │
│  - Sequencer.kt         ← Playback scheduling       │
│  - FileManager.kt       ← Save/load (uses IFileSystem) │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│          PLATFORM ABSTRACTION LAYER                 │
│  (Interfaces defining platform capabilities)        │
│                                                      │
│  - IAudioBackend      ← Audio playback interface    │
│  - IResourceLoader    ← Sample/asset loading        │
│  - IFileSystem        ← File I/O interface          │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│         PLATFORM IMPLEMENTATION LAYER               │
│  (Concrete implementations per platform)            │
│                                                      │
│  Android:                    Linux:                 │
│  - OboeAudioBackend.kt       - ALSAAudioBackend.cpp │
│  - AndroidResourceLoader.kt  - FileResourceLoader   │
│  - AndroidFileSystem.kt      - LinuxFileSystem.cpp  │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│              NATIVE AUDIO CORE                      │
│  (Shared C++ - Already Portable!)                   │
│                                                      │
│  - native-audio.cpp          ✅ DONE                │
│  - Sample-accurate queue                            │
│  - 8-voice polyphony                                │
│  - Linear interpolation                             │
│  - Biquad filters                                   │
└─────────────────────────────────────────────────────┘
```

### File Structure (Target)

```
PocketTracker/
│
├── core/                           # Platform-agnostic code
│   ├── data/
│   │   ├── TrackerData.kt          ✅ Already portable!
│   │   ├── Note.kt
│   │   ├── Phrase.kt
│   │   └── Project.kt
│   │
│   ├── logic/
│   │   ├── TrackerController.kt    ← NEW: All button handlers
│   │   ├── EffectProcessor.kt      ← NEW: Effect calculations
│   │   ├── Sequencer.kt            ← NEW: Playback logic
│   │   └── FileManager.kt          ← REFACTORED: Uses IFileSystem
│   │
│   ├── audio/
│   │   ├── IAudioBackend.kt        ← NEW: Audio interface
│   │   └── AudioEngine.kt          ← REFACTORED: Uses IAudioBackend
│   │
│   ├── resources/
│   │   └── IResourceLoader.kt      ← NEW: Resource loading interface
│   │
│   ├── storage/
│   │   ├── IFileSystem.kt          ← NEW: File I/O interface
│   │   └── FileInfo.kt             ← NEW: Platform-agnostic file metadata
│   │
│   └── rendering/
│       ├── TrackerModule.kt        ✅ Already portable!
│       ├── BitmapFont.kt           ✅ Already portable!
│       ├── PixelPerfectRenderer.kt ← Minor refactoring needed
│       └── All *Module.kt files    ✅ Mostly portable!
│
├── platform/
│   ├── android/
│   │   ├── MainActivity.kt         ← THIN: Just creates backends + UI
│   │   ├── OboeAudioBackend.kt     ← NEW: Oboe implementation
│   │   ├── AndroidResourceLoader.kt ← NEW
│   │   ├── AndroidFileSystem.kt    ← NEW
│   │   ├── DeviceAdapter.kt        ← Android-specific input
│   │   └── jni/
│   │       └── native-audio.cpp    ✅ Shared with Linux!
│   │
│   └── linux/                      # FUTURE (after MVP)
│       ├── main.cpp
│       ├── ALSAAudioBackend.cpp
│       ├── LinuxResourceLoader.cpp
│       ├── LinuxFileSystem.cpp
│       └── GTK_UI.cpp (or Qt/SDL2)
│
└── shared-native/                  # C++ shared between platforms
    ├── audio-engine.cpp            ✅ Already exists!
    ├── audio-engine.h
    ├── effects.cpp                 ← NEW: Effect processing
    ├── effects.h
    └── CMakeLists.txt
```

---

## Audio Processing Chain Rule

**ALL audio processing lives in `processAudioBlock()` in `audio-engine.cpp`.**

`onAudioReady()` and `renderOffline()` are thin wrappers — they call `processAudioBlock()`
and add only output-destination-specific work on top:

| Step | processAudioBlock | onAudioReady only | renderOffline only |
|------|-------------------|-------------------|--------------------|
| Kill/note queue | ✅ | | |
| Table ticks | ✅ | | |
| Pitch/ADSR/LFO mod | ✅ | | |
| DSP chain + mix | ✅ | | |
| Brickwall limiter | ✅ | | |
| Waveform capture | | ✅ | |
| Peak meter tracking | | ✅ | |
| Offline silence gate | | ✅ (isOfflineRendering) | |
| Chunk loop | | | ✅ (BLOCK_SIZE=256) |

**Rule: If you add a new audio processing feature (new effect, new modulation destination, etc.)
add it to `processAudioBlock()`. NEVER add processing logic directly to `onAudioReady` or
`renderOffline` — it will be missing from one of the two outputs.**

---

## Audio Engine

### Current State ✅ (Already Professional-Grade!)

**Architecture:** Sample-accurate queue system in C++

**Key Features:**
- ✅ Oboe-based real-time audio (44.1kHz; OpenSL ES preferred, AAudio fallback)
- ✅ Stream open order: OpenSL ES Exclusive → Shared → None/Shared → AAudio Exclusive
- ✅ Audio init runs off main thread (Dispatchers.IO) — no UI freeze on startup
- ✅ Sample-accurate note scheduling (<0.02ms jitter)
- ✅ Linear interpolation (eliminates aliasing)
- ✅ 8-voice polyphony with per-track voice stealing
- ✅ Global frame counter for precise timing
- ✅ Resonant biquad filters (LP/HP/BP using Audio EQ Cookbook)
- ✅ Effects chain: Downsample (pre-interp, inline) → Interpolate → Crush → Drive → Filter → Volume
- ✅ DSP module system: three-layer (Primitive / Module / Chain)
- ✅ Waveform capture for oscilloscope visualization
- ✅ SoundFont (SF2) instruments via TinySoundFont (TSF) — see below

**Performance:**
- Timing precision: <0.02ms jitter (100x better than Kotlin timing)
- Audio quality: Professional-grade (matches M8/LGPT/Picotracker)
- Latency: <50ms on tested hardware

**Why This is Good for Linux Port:**
> The entire audio engine is already in C++! We just need to:
> 1. Wrap Oboe calls in an interface
> 2. Create ALSA/PulseAudio backend implementing same interface
> 3. Rest of the audio code stays EXACTLY the same!

> **Status (REVIEW-4 4.5 — done):** the C++ engine is now split, so step 1 above is real on both sides.
> - **`audio-engine.{h,cpp}` — portable core (`class AudioEngine`):** voices, note scheduling, the
>   sample-accurate queues and ALL DSP (`processAudioBlock`). No `<oboe/*>` or `<android/*>` — logging
>   goes through the `audio-defs.h` shim. It caches the device rate (`setDeviceSampleRate`) and wakes a
>   paused stream through a `resumeHook` instead of touching a stream object.
> - **`oboe-audio-engine.{h,cpp}` — Android backend (`class OboeAudioEngine : oboe::AudioStreamDataCallback`):**
>   the *only* Oboe-coupled TU. Owns the `std::shared_ptr<oboe::AudioStream>`, runs the device-specific
>   stream-open ladder, and in `onAudioReady` forwards the buffer to `core->processLiveBlock(...)`.
>
> So the Linux port is now a **drop-in backend**: add a sibling (e.g. `AlsaAudioEngine` / `SdlAudioEngine`)
> that owns its own stream and calls the same `processLiveBlock()` — `audio-engine.{h,cpp}` is recompiled
> unchanged. (Naming mirrors the Kotlin layer: `AudioEngine` = portable on both sides, `Oboe*` = Android.)
> The seam was also real on the Kotlin side already (`IAudioBackend`).

### JNI Interface (Before Refactoring — now wrapped behind IAudioBackend/OboeAudioBackend)

```kotlin
// TrackerAudioEngine.kt (old approach — replaced by OboeAudioBackend.kt)
external fun native_create(): Boolean
external fun native_loadSample(id: Int, samples: FloatArray)
external fun native_scheduleNote(frame: Long, sampleId: Int, trackId: Int, freq: Float, baseFreq: Float, vol: Float)
external fun native_getCurrentFrame(): Long
external fun native_clearScheduledNotes()
external fun native_resumeStream()
external fun native_stopAll()
external fun native_getSampleRate(): Int
external fun native_updateWaveform(buffer: FloatArray)
```

### Current Architecture (Post-Refactoring) ✅

```kotlin
// core/audio/IAudioBackend.kt
interface IAudioBackend {
    fun create(): Boolean
    fun loadSample(id: Int, samples: FloatArray)
    fun scheduleNote(frame: Long, sampleId: Int, trackId: Int, freq: Float, baseFreq: Float, vol: Float)
    fun getCurrentFrame(): Long
    fun clearScheduledNotes()
    fun resumeStream()
    fun stopAll()
    fun getSampleRate(): Int
    fun updateWaveform(buffer: FloatArray)
    fun close()
}

// platform/android/OboeAudioBackend.kt
class OboeAudioBackend : IAudioBackend {
    init { System.loadLibrary("pockettracker") }
    
    private external fun native_create(): Boolean
    // ... JNI declarations
    
    override fun create() = native_create()
    override fun loadSample(id: Int, samples: FloatArray) = native_loadSample(id, samples)
    // ... implementations just forward to JNI
}

// core/audio/AudioEngine.kt (platform-agnostic!)
class AudioEngine(
    private val backend: IAudioBackend,
    private val resourceLoader: IResourceLoader
) {
    val waveformBuffer = FloatArray(620)
    
    fun create(): Boolean {
        val success = backend.create()
        if (success) loadAllSamples()
        return success
    }
    
    fun playNote(instrument: Instrument, trackId: Int, freq: Float, vol: Float) {
        val frame = backend.getCurrentFrame()
        // Base freq derived on demand from the sample-rate ratio (no cached copy to keep in sync)
        val baseFreq = calculateInstrumentBaseFrequency(instrument)
        backend.scheduleNote(frame, instrument.sampleId, trackId, freq, baseFreq, vol)
    }
    
    // All logic here - no Android dependencies!
}
```

### Adding a New Engine Call (the four-file ritual)

> **Why this note exists:** the portability seam is deliberate but wide. `IAudioBackend` has ~100
> methods, `OboeAudioBackend` mirrors each as an `external fun`, `jni-bridge.cpp` has ~115 hand-written
> thunks, and many `AudioEngine` methods are 1-line forwards. So adding *one* engine feature means
> touching **four files in lockstep** (five with the C++ engine itself). Miss one and you get an
> `UnsatisfiedLinkError` at runtime, not a compile error. This is the checklist so the ritual is
> documented rather than rediscovered each time.

To add a call `fooBar(id: Int, gain: Float)` that reaches the native engine, edit in this order:

1. **C++ engine — `audio-engine.h` / `audio-engine.cpp`**
   Implement the actual behaviour as a method on the `AudioEngine` C++ class. All DSP must live in
   `processAudioBlock` (see the Audio Processing Chain Rule) — `fooBar` only mutates state the mix loop
   reads. Guard any shared state touched by the audio thread with the existing mutex discipline.

2. **JNI thunk — `jni-bridge.cpp`**
   Add `Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1fooBar(JNIEnv*,
   jobject, jint id, jfloat gain)` that marshals args and calls `engine->fooBar(id, gain)`. Gotchas:
   the function name must match the package path exactly, `_` in the Kotlin name becomes `_1` in the
   symbol, `Long` → `jlong`, and a `FloatArray` arg needs `GetFloatArrayElements` / `ReleaseFloatArrayElements`
   (see `native_loadSample` for the pattern).

3. **Backend impl — `platform/android/OboeAudioBackend.kt`**
   Declare `private external fun native_fooBar(id: Int, gain: Float)` and add the interface override
   `override fun fooBar(id: Int, gain: Float) = native_fooBar(id, gain)`.

4. **Interface — `core/audio/IAudioBackend.kt`**
   Add `fun fooBar(id: Int, gain: Float)` to the interface so core code can call it portably and the
   future `ALSAAudioBackend` is forced to implement it too.

5. **Engine facade — `core/audio/AudioEngine.kt`**
   Add the method core/UI actually calls. If it carries logic, put it here (still no Android imports);
   if it's a pure pass-through, it's just `fun fooBar(id: Int, gain: Float) = backend.fooBar(id, gain)`.

**Verification:** because the JNI link is resolved lazily, the only real check is running the app and
exercising the new call — a clean compile does **not** prove the thunk name is correct. (Sample-editor
ops are the bulk of the pure pass-throughs; finding 4.1 notes they could later be grouped behind an
`ISampleEditorBackend` to shrink this surface, but that refactor is not yet done.)

### SoundFont (SF2) Engine

**Implementation:** TinySoundFont (TSF) — single-header C++ SF2 synthesizer, with a small fork patch for per-channel rendering.

**Current architecture (complete as of April 2026):**
- One shared `tsf*` handle per SF2 file slot (up to 8 active SF2 files simultaneously)
- Each track maps to a MIDI channel on the slot's handle: track 0 → ch 0 … track 7 → ch 7
- **`tsf_render_float_channel(h, t, buf, frames, 0)`** — forked function renders one MIDI channel at a time into a per-track buffer, enabling per-instrument post-processing
- SF2 loaded via `tsf_load_filename()` directly; memory: ~1× SF2 file size (one handle per file)
- Full modulation engine parity with sampler: same `updateVoiceModulation()` runs for `SoundfontVoice`; ADSR/LFO/AHD/DRUM/TRIG, all destinations (VOL/PAN/PITCH/FILTER), table effects and pitch slides all work identically
- Per-instrument effects (filter/drive/bitcrush) applied to each track's SF buffer post-render
- SF preset parameter overrides (ATK/DEC/SUS/REL/filterCut/filterRes) — set on instrument screen, patched into TSF regions via `applySoundfontEnvelopeOverrides()` at note trigger
- KIL/REL: ADSR release and TSF-native release both work after KIL — ADSR path defers `tsf_channel_note_off` until release completes; TSF REL path uses immediate note_off + silence detection
- Table effects (HOP, TIC, transpose, volume) work identically to sampler; table arpeggio continues through release tail

**Module System:**
- Phase 0–3: ✅ File split + source/dest arrays + unified routing loop
- Phase 5–8: ✅ SF mod parity + per-channel render + per-instrument FX + preset overrides
- Phase 4 (SCALAR mod type): ⏳ Deferred post-MVP

**Architecture debt:**
- Table processing loop is still duplicated (sampler loop + SF loop). Unification into `processTableTick(IAudioVoice&)` is the next step.

**Linux Implementation (Future):**
```cpp
// platform/linux/ALSAAudioBackend.cpp
class ALSAAudioBackend : public IAudioBackend {
    bool create() override {
        // Open ALSA device
        snd_pcm_open(&handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
        // ... setup
    }
    
    void scheduleNote(...) override {
        // Same scheduling logic as Oboe!
    }
}
```

---

## Data Model

### Current State ✅ (Already Portable!)

The data model is **already platform-agnostic** - just pure Kotlin data classes with no Android dependencies!

```kotlin
@Serializable
data class Note(val pitch: Int, val octave: Int) {
    fun toMidi(): Int = (octave + 1) * 12 + pitch
    fun toFrequency(): Float = 440f * Math.pow(2.0, (toMidi() - 69) / 12.0).toFloat()
}

@Serializable
data class PhraseStep(
    var note: Note = Note.EMPTY,
    var instrument: Int = 0x00,
    var volume: Int = 0xFF,
    var fx1Type: Int = 0x00,
    var fx1Value: Int = 0x00,
    // ... fx2, fx3
)

@Serializable
data class Phrase(val id: Int, val steps: Array<PhraseStep>)

@Serializable
data class Chain(val id: Int, val phraseRefs: IntArray, val transposeValues: IntArray)

@Serializable
data class Instrument(
    var sampleId: Int = -1,
    var volume: Int = 0xFF,
    var pan: Int = 0x80,
    // ... all parameters
)

@Serializable
data class Project(
    var name: String = "UNTITLED",
    var tempo: Int = 128,
    val phrases: Array<Phrase>,
    val chains: Array<Chain>,
    val instruments: Array<Instrument>,
    val tracks: Array<Track>
)
```

*(Simplified for illustration — `core/data/TrackerData.kt` is authoritative for exact fields, types, and defaults. Pools are sized in `Project`: 256 phrases / 256 chains, 128 instruments / tables / grooves.)*

**Why this is great:**
- ✅ No Context, no Resources, no Android APIs
- ✅ Serializable to JSON (works on any platform)
- ✅ Can be translated to C++ structs if needed for Linux

**Future Linux Port:**
```cpp
// core/data/TrackerData.h (C++ version)
struct Note {
    int pitch;
    int octave;
    int toMidi() const { return (octave + 1) * 12 + pitch; }
    float toFrequency() const { /* ... */ }
};

struct PhraseStep {
    Note note;
    int instrument;
    int volume;
    // ...
};

// Use nlohmann/json for serialization (same JSON format!)
```

---

## Rendering System

### Current State (Mostly Portable!)

**Architecture:** Pixel-perfect Canvas rendering at 640×480

```kotlin
// PixelPerfectRenderer.kt
@Composable
fun PixelPerfectRenderer(
    screenType: ScreenType,
    project: Project,
    cursorContext: CursorContext,
    // ... state
) {
    Canvas(modifier = Modifier.fillMaxSize()) {
        // Calculate scaling and letterboxing
        val scale = calculateScale(size)
        
        // Draw modules
        when (screenType) {
            ScreenType.PHRASE -> PhraseEditorModule.draw(...)
            ScreenType.CHAIN -> ChainEditorModule.draw(...)
            // ...
        }
    }
}
```

**TrackerModule Interface:**
```kotlin
interface TrackerModule {
    val width: Int
    val height: Int
    fun draw(
        drawScope: DrawScope,
        project: Project,
        cursorContext: CursorContext,
        // ... state
    )
}
```

**Portability Status:**
- ✅ Drawing logic is pure (no Android APIs in modules)
- ⚠️ `DrawScope` is Compose-specific
- ⚠️ `Canvas` is Android/Compose

**Refactoring Strategy:**

Option A: Keep Compose for Android, rewrite rendering for Linux
- Pros: Simple, each platform optimized
- Cons: Duplicate rendering code

Option B: Abstract rendering to portable layer
- Pros: Single rendering codebase
- Cons: Complex abstraction, potential performance overhead

**Recommendation: Option A**
> Rendering is not the complex part - button handlers and audio are. 
> Let Android keep Compose (it's great!), Linux can use Cairo/SDL2.
> Share the layout logic and constants, but platform-specific drawing.

**Shared Constants:**
```kotlin
// core/rendering/RenderingConstants.kt
object RenderingConstants {
    const val SCREEN_WIDTH = 640
    const val SCREEN_HEIGHT = 480
    const val FONT_WIDTH = 5
    const val FONT_HEIGHT = 5
    const val FONT_SCALE = 3
    const val CHAR_WIDTH = FONT_WIDTH * FONT_SCALE  // 15px
    const val CHAR_HEIGHT = FONT_HEIGHT * FONT_SCALE  // 15px
}
```

### Theme System

**`AppTheme`** (`AppTheme.kt`) — `@Serializable data class` holding 19 ARGB `Long` color fields + `visualizerType`. Serialized to/from `.ptt` theme files.

```kotlin
data class AppTheme(
    val background: Long,       // module fill
    val rowEvery4th: Long,      // beat accent rows
    val rowCursor: Long,        // cursor row highlight
    val rowPlayback: Long,      // playback row highlight
    val rowSelection: Long,     // selection mode highlight
    val textTitle: Long,        // cyan headers
    val textParam: Long,        // inactive labels
    val textValue: Long,        // inactive values
    val textCursor: Long,       // cursor yellow
    val textEmpty: Long,        // dim placeholder text
    val vizBackground: Long,    // oscilloscope / waveform bg
    val vizCenterLine: Long,    // center line in visualizers
    val vizWave: Long,          // waveform + selection highlight
    val meterBackground: Long,  // dialog bg + meter bg
    val meterLow/Mid/High: Long,// dBFS meter colors
    val meterBorder: Long,      // meter border
    val visualizerType: VisualizerType
)
```

**Injection pattern:** `AppTheme` flows top-down via `LocalAppTheme` CompositionLocal → `drawLayout(appTheme)` → each module state `copy(appTheme = appTheme)`. Inside every draw function: `val t = <state>.appTheme` → `Color(t.fieldName)`.

**`darken()` extension (`EditorHelpers.kt`):** `fun Long.darken(factor: Float): Long` — multiplies RGB channels by factor, preserves alpha. Used for cursor shadow backgrounds (e.g. `Color(t.textCursor.darken(0.27f))`).

**Bundled themes:** CLASSIC (green-on-black), AMBER, BLUE, MONO — defined as companion constants on `AppTheme`.

### Screen Overlay System

PNG overlays (e.g. CRT scanlines) can be layered on top of the tracker screen at runtime.

**Asset convention:** place any PNG in `app/src/main/assets/overlays/`. The Settings screen (OVERLAY row, row 2) lists them automatically via `context.assets.list("overlays")`.

**Loading (`MainActivity.kt`):**
```kotlin
val overlayBitmap: ImageBitmap? = remember(overlayName) {
    if (overlayName == "OFF") null
    else BitmapFactory.decodeStream(assets.open("overlays/$overlayName.png"))?.asImageBitmap()
}
```
The bitmap is loaded as-is — no pixel processing. `STR` (00-FF) is the only runtime control.

**Rendering (`ScreenLayouts.kt` — `TrackerScreen`):**
```kotlin
Modifier.drawWithContent {
    drawContent()                    // draws PixelPerfectTracker
    drawImage(bitmap, ..., alpha = STR / 255f)   // overlay on same canvas
}
```
`Modifier.drawWithContent` is critical: it shares the same canvas as `drawContent()`, so the `alpha` draw happens over the already-rendered game pixels. A separate `graphicsLayer { blendMode }` approach was tried first but failed because it composited against the layer's own background rather than the game content.

**Settings wiring:** `overlayName: String` and `overlayStrength: Int` live in `AppStateRefs`, delegated in `AppInputDispatcher`, and persisted via SharedPreferences keys `overlay_name` / `overlay_strength`. `overlayFiles: List<String>` is a read-only computed list passed through `AppStateRefs` (no MutableState needed).

**Adding new overlays:** drop a PNG into `assets/overlays/` — no code changes required.

---

## Navigation System

### 5×5 Screen Grid

```
Row 0:         -      SCALE   INST_POOL  (INST)*
Row 1:     PROJECT   GROOVE     MODS     PROJECT
Row 2:      SONG     CHAIN    PHRASE   INSTRUMENT  TABLE
Row 3:     MIXER     MIXER    MIXER      MIXER     MIXER
Row 4:    EFFECTS   EFFECTS  EFFECTS    EFFECTS   EFFECTS
```

`(INST)*` at row 0 / col 4 is a **contextual fast-jump cell**, shown only while on INST_POOL (or
on the instrument screen reached from it). See "Instrument Pool fast-jump" below.

### Instrument Pool fast-jump (INST_POOL ↔ INSTRUMENT)

The Instrument Pool sits at row 0 / col 3 (above MODS / INSTRUMENT). It pairs horizontally with a
contextual INSTRUMENT cell at row 0 / col 4 for quickly bouncing between the two views (M8-style):

- **From INST_POOL:** R+RIGHT → INSTRUMENT (the row-0 instrument); R+LEFT → PHRASE; R+DOWN → MODS.
- **From the row-0 instrument** (entered via R+RIGHT from the pool): R+LEFT → back to INST_POOL;
  R+DOWN → MODS; R+UP / R+RIGHT stay put.
- The normal row-2 INSTRUMENT keeps all its usual navigation (R+LEFT → PHRASE, R+RIGHT → TABLE,
  R+UP → MODS, R+DOWN → MIXER).

This is implemented with `TrackerController.instrumentFromPool` — set true only on the pool→instrument
R+RIGHT jump and cleared (in the `currentScreen` setter) the moment you move off INSTRUMENT any other
way. The nav map highlights the row-0 cell (not the row-2 instrument) while the flag is set.

**Navigation Logic (Before Refactoring — was in MainActivity.kt):**
```kotlin
fun navigateUp() {
    currentScreen = when (currentScreen) {
        ScreenType.SONG -> ScreenType.PROJECT
        ScreenType.CHAIN -> ScreenType.GROOVE
        // ...
    }
}
```

**Current Implementation (TrackerController.kt) ✅:**
```kotlin
class TrackerController {
    var currentScreen by mutableStateOf(ScreenType.PHRASE)
    
    fun handleShiftUp() {
        currentScreen = NavigationMap.navigateUp(currentScreen)
    }
}

object NavigationMap {
    fun navigateUp(from: ScreenType): ScreenType = when (from) {
        ScreenType.SONG -> ScreenType.PROJECT
        ScreenType.CHAIN -> ScreenType.GROOVE
        // ... pure logic, no Android!
    }
}
```

---

## File Management

### Before Refactoring (Android-Specific)

```kotlin
// FileManager.kt (old — replaced by IFileSystem + AndroidFileSystem)
class FileManager(private val context: Context) {
    fun saveProject(project: Project, filename: String) {
        val dir = File(context.getExternalFilesDir(null), "Projects")
        val file = File(dir, "$filename.ptp")
        val json = Json.encodeToString(project)
        file.writeText(json)
    }
}
```

**Problem it had:** Uses `Context.getExternalFilesDir()` - Android-specific!

### Current State (Platform-Agnostic) ✅

```kotlin
// core/storage/IFileSystem.kt
interface IFileSystem {
    fun getProjectsDirectory(): String
    fun getSamplesDirectory(): String
    fun listFiles(path: String): List<FileInfo>
    fun readFile(path: String): String
    fun writeFile(path: String, content: String)
    fun deleteFile(path: String): Boolean
    fun createDirectory(path: String): Boolean
}

data class FileInfo(
    val name: String,
    val path: String,
    val isDirectory: Boolean,
    val size: Long,
    val lastModified: Long
)

// core/storage/FileManager.kt (NOW PORTABLE!)
class FileManager(private val fileSystem: IFileSystem) {
    fun saveProject(project: Project, filename: String) {
        val path = "${fileSystem.getProjectsDirectory()}/$filename.ptp"
        val json = Json.encodeToString(project)
        fileSystem.writeFile(path, json)
    }
    
    fun loadProject(filename: String): Project {
        val path = "${fileSystem.getProjectsDirectory()}/$filename.ptp"
        val json = fileSystem.readFile(path)
        return Json.decodeFromString(json)
    }
}

// platform/android/AndroidFileSystem.kt
class AndroidFileSystem(private val context: Context) : IFileSystem {
    override fun getProjectsDirectory(): String =
        "${context.getExternalFilesDir(null)}/Projects"
    
    override fun listFiles(path: String): List<FileInfo> {
        return File(path).listFiles()?.map { file ->
            FileInfo(
                name = file.name,
                path = file.absolutePath,
                isDirectory = file.isDirectory,
                size = file.length(),
                lastModified = file.lastModified()
            )
        } ?: emptyList()
    }
    
    // ... other methods
}

// platform/linux/LinuxFileSystem.cpp (FUTURE)
class LinuxFileSystem : public IFileSystem {
    std::string getProjectsDirectory() override {
        return std::string(getenv("HOME")) + "/.pockettracker/Projects";
    }
    
    // ... POSIX file operations
}
```

---

## Build System

### Current (Android-Only)

```gradle
// app/build.gradle.kts
android {
    compileSdk = 34
    
    defaultConfig {
        minSdk = 26
        targetSdk = 34
    }
    
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }
}
```

### Target (Multi-Platform)

```
PocketTracker/
├── android/
│   ├── app/
│   │   ├── build.gradle.kts
│   │   └── src/main/
│   │       ├── kotlin/      # Platform-specific Android code
│   │       └── cpp/         # JNI bridge only
│   └── settings.gradle.kts
│
├── linux/                   # FUTURE
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp
│   │   └── ui/              # GTK/Qt code
│   └── build.sh
│
├── core/                    # Shared Kotlin (compiles to .jar)
│   ├── build.gradle.kts
│   └── src/
│       └── commonMain/kotlin/
│
└── shared-native/           # Shared C++ (both platforms link to it)
    ├── CMakeLists.txt
    └── src/
        ├── audio-engine.cpp
        └── effects.cpp
```

**Kotlin Multiplatform (Optional Future Enhancement):**
```kotlin
// core/build.gradle.kts
kotlin {
    android()
    linuxX64()
    
    sourceSets {
        val commonMain by getting {
            dependencies {
                implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.6.0")
            }
        }
    }
}
```

---

## Technology Stack

### Current Stack

| Layer | Technology | Notes |
|-------|------------|-------|
| **UI** | Jetpack Compose | Android-specific |
| **Language** | Kotlin 1.9+ | Primary language |
| **Audio** | C++ with Oboe (OpenSL ES / AAudio) | Already portable! ✅ |
| **SF2 Synth** | TinySoundFont (TSF) | Single-header, vendored at `vendor/tsf/tsf.h` |
| **Build** | Gradle 8.x | Android build system |
| **Native Build** | CMake 3.22.1+ | C++ compilation |
| **Serialization** | Kotlinx Serialization | JSON save/load |
| **Min Android** | API 26 (Android 8.0) | Target budget devices |
| **Architectures** | arm64-v8a, x86_64 | 64-bit only |

### Future Stack (Linux)

| Layer | Technology | Options |
|-------|------------|---------|
| **UI** | GTK4 / Qt6 / SDL2 | TBD (mentor will help decide) |
| **Language** | C++17 | For Linux native code |
| **Audio** | ALSA / PulseAudio / JACK | Multiple backend support |
| **Build** | CMake | Same as Android native |
| **Serialization** | nlohmann/json | Same JSON format as Android |

---

## Coding Conventions

### Kotlin Code Style

**File Organization:**
```kotlin
// 1. Package declaration
package com.conanizer.pockettracker

// 2. Imports (grouped: stdlib, third-party, project)
import kotlin.math.*
import kotlinx.serialization.*
import androidx.compose.*

// 3. Constants
private const val TAG = "ModuleName"

// 4. Data classes / interfaces
data class Example(...)

// 5. Main class
class MainClass {
    // Properties first
    private val property = value
    
    // Init blocks
    init { ... }
    
    // Public methods
    fun publicMethod() { ... }
    
    // Private methods
    private fun privateMethod() { ... }
}
```

**Naming:**
- Classes: `PascalCase`
- Functions: `camelCase`
- Constants: `SCREAMING_SNAKE_CASE`
- Private members: `camelCase` (no prefix)

**Comments:**
- Only add a comment when the WHY is non-obvious (a hidden constraint, a surprising invariant, a specific bug workaround). If removing the comment would not confuse a future reader, don't write it.
- Do not write comments that describe WHAT the code does — well-named identifiers already do that.
- Section separators (`// ═══════════════`) are acceptable inside long files (e.g. `EditorHelpers.kt`) to group unrelated utilities, but not inside individual module functions.

**Screen Module Conventions (`*Module.kt`):**
- All hex display formatting uses `.toHex2()` / `.toHex1()` from `EditorHelpers.kt`.
- Row background color in list editors (Phrase, Chain, Song, Table) uses `rowBgColor()` from `EditorHelpers.kt`.
- `getCursorContext()` uses `CursorContextFactory.*` factory methods exclusively — no raw `CursorContext(...)` constructor unless no factory fits.
- `draw()` signature: `override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?)` — cast state with early return: `val s = state as? FooState ?: return`.
- Each module defines its own `private val FONT_SCALE / CHAR_SPACING / ROW_HEIGHT / TEXT_PADDING = …` (mirrors `EditorHelpers.kt` constants; shadowing is intentional for readability).

### C++ Code Style

```cpp
// Constants in ALL_CAPS
constexpr int MAX_VOICES = 8;

// Classes in PascalCase
class AudioEngine {
private:
    // Member variables with m_ prefix
    float m_sampleRate;
    
public:
    // Methods in camelCase
    void playNote(int sampleId, float frequency);
};

// Free functions in camelCase
float noteToFrequency(int midiNote);
```

---

## Next Steps

See **REFACTORING_ROADMAP.md** for detailed step-by-step refactoring plan.

---

**Version History:**
- v2.8 (2026-06-26): Live per-note / mixer FX (PAN, REV, DEL [delay send], BCK, EQN, EQM) + KIL-offset added; all audio-affecting live FX route through the sample-accurate `ParamUpdateQueue` (new `ParamUpdateAction` cases + `scheduleVoice*` wrappers — no new `scheduleNote`/JNI plumbing). Old `DEL` (delay-trigger) renamed `LAT`; new `DEL` = per-note delay send. KIL now fades release-less voices over `DECLICK_SAMPLES` (click-free) instead of hard-stopping. Mods `AMT` default → `0xFF`. FX helper overlay now 6×5 (centered last row). Reload fix: `AppInputDispatcher.pushGlobalEffectsToBackend()` re-pushes the native EQ bank + reverb/delay/master-EQ on project load (these live only in C++ and persist across loads).
- v2.7 (2026-06-22): Doc fact-check (DOCS-REVIEW round 2) — corrected the file tree (`input/`, `ui/`, `ui/modules/`, `core/data/`; C++ `mods/`, `vendor/tsf/`, `effects/primitives/filter.h`); fixed the data-model sample (no `Song` class → `tracks`, `transposeValues`, tempo `128`, fx fields `var`); SettingsModule now 12 rows; removed dead `plan-dsp-modules.md` / `plan-module-system.md` links; modulation files repointed to `audio-engine.cpp` + `mods/`; marked the Target Architecture section historical.
- v2.6 (2026-06-06): Screen overlay system added (Rendering System section); SettingsModule updated to 11 rows; `drawWithContent` compositing pattern documented.
- v2.5 (2026-05-18): Fact-checked against codebase — fixed ToC duplicate "10.", send/master-chain stub labels, ADSR release status, SCALAR mod type, package name, private member convention, Modules/ directory path, SampleEditorModule added to file tree.
- v2.4 (2026-05-05): Module code style unified — `.toHex2()`, `rowBgColor()`, factory-only `getCursorContext()`; EffectModule/EqModule/SettingsModule added to file tree; coding conventions updated to reflect current standards.
- v2.3 (2026-04-22): DSP module system implemented; effects/ directory (primitives + modules + chains); InstrumentChain (Crush→Drive→Filter) wired to all sampler and SF voices; C++ file tree updated in this document; guide-adding-effects.md written
- v2.2 (2026-04-17): Audio module system complete (Phases 0–3, 5–8); SF2 full mod parity; per-channel TSF rendering; SF bug fixes (HOP, KIL/REL, table in release); table abstraction debt noted
- v2.1 (2026-04-07): Added SF2/TSF engine section; OpenSL ES stream priority; async audio init; UAA phase status
- v2.0 (2026-03-13): Updated to reflect complete refactoring; all architecture goals achieved; modulation engine fully implemented
- v1.0 (2025-01-01): Initial architecture document with refactoring plan

---

## Final Architecture Decision

**Status:** FINALIZED (2025-01-02)

**Decision:** Option B (Foundational Architecture)

### Rationale

- Developer preference: "Why not make great basis now?"
- Time available (no external deadlines)
- Long-term Linux port is planned, not hypothetical
- Mentor will appreciate clean, organized codebase
- Better learning opportunity (proper architecture)

### Final Controller Structure
core/logic/
├── TrackerController.kt      # Main coordinator (owns state)
├── InputController.kt         # Button handling, selection mode
├── PlaybackController.kt      # Playback scheduling
├── EffectProcessor.kt         # Effect calculations
├── InstrumentController.kt    # Sample/instrument management
├── FileController.kt          # Save/load operations
└── ClipboardManager.kt        # Copy/paste operations

### Benefits

**For MVP Development:**
- Clear separation makes debugging easier
- Each controller ~200-300 lines (manageable size)
- Can test controllers independently
- Easy to add features (extend relevant controller)

**For Linux Port:**
- All controllers already portable (no Android deps)
- Just need new UI layer calling same controllers
- Audio/file/resource implementations swap easily

**For Mentor Collaboration:**
- Clear boundaries for parallel work
- Mentor can work on InstrumentController (Braids)
- Developer can work on EffectProcessor (new effects)
- No merge conflicts!

### Implementation

See `REFACTORING_ROADMAP.md` Phase 4 for step-by-step implementation guide.

---

## Modulation Engine

**Implemented in:** Phase 4 of MVP Extension Pack 3
**Files:** `audio-engine.cpp` + `mods/` headers (C++), `AudioEngine.kt`, `IAudioBackend.kt`, `TrackerData.kt`

### Overview

Each instrument has 4 modulation slots (`modSlots: Array<ModSlot>` on `Instrument`). When a note is scheduled, the Kotlin layer pushes the current mod params to the C++ engine (`pushInstrumentModulation`), which copies them onto the triggered voice. The C++ engine then updates each slot once per audio callback (`updateVoiceModulation`), computing an `envValue` that is applied to the destination parameter in the mix loop.

---

### Data Model (Kotlin — `TrackerData.kt`)

```kotlin
data class ModSlot(
    var type: ModType = ModType.NONE,   // envelope/LFO type
    var dest: ModDest = ModDest.NONE,   // target parameter
    var amount: Int = 0x80,             // modulation depth, 0x00-0xFF

    // Envelope params (AHD, ADSR)
    var attack: Int  = 0x00,   // ticks
    var hold: Int    = 0x00,   // ticks (AHD only)
    var decay: Int   = 0x00,   // ticks
    var sustain: Int = 0x80,   // 0x00-0xFF sustain level (ADSR only)
    var release: Int = 0x00,   // ticks (ADSR, future)

    // LFO params
    var oscShape:    Int = 0,  // 0=TRI 1=SIN 2=RMP+ 3=RMP- 4=EXP+ 5=EXP- 6=SQU+ 7=SQU- 8=RND 9=DRNK
    var lfoTrigMode: Int = 0,  // 0=FREE 1=RETRIG (phase always resets on new note for now)
    var lfoFreq:     Int = 0x40  // 0x00-0xFF → 0.1 to 20 Hz
)
```

**ModType ordinals:** NONE=0, AHD=1, ADSR=2, LFO=3, DRUM=4, TRIG=5, TRACKING=6 (future), SCALAR=7 (future)
**ModDest ordinals:** NONE=0, VOLUME=1, PAN=2, PITCH=3, FINE_PITCH=4, FILTER_CUTOFF=5, FILTER_RES=6, SAMPLE_START=7, MOD_AMT=8, MOD_RATE=9, MOD_BOTH=10

---

### Kotlin → C++ pipeline (`AudioEngine.pushInstrumentModulation`)

Called once per `scheduleNote()` call, immediately before the note is queued. Converts tick-based timing to audio samples using:

```
framesPerTic = sampleRate / (BPM/60 × 4 steps/beat × 12 tics/step)
```

At 120 BPM, 44100 Hz: `framesPerTic ≈ 229 samples`

**LFO frequency mapping:**
`lfoFreq (0x00–0xFF)` → `lfoHz = (lfoFreq + 1) × 20.0 / 256`
Range: ~0.08 Hz (0x00) → ~20 Hz (0xFF). At default 0x40: ~5 Hz.

**JNI call:**
```kotlin
backend.setInstrumentModulation(sampleId, slotIndex, type, dest, amount,
    attackSamples, holdSamples, decaySamples, sustainLevel, lfoHz, oscShape)
```

---

### C++ engine (per-voice, per-callback)

#### InstrumentModSlot vs VoiceModSlot

- `InstrumentModSlot[256][4]` — static store, updated from Kotlin before each note
- `VoiceModSlot[4]` on each `Voice` — copied from instrument store at note-trigger time; holds runtime state (`stage`, `envValue`, `lfoPhase`, `stageCounter`)

#### Type=1 — AHD

Stages: **1=Attack** (0→1), **2=Hold** (stay at 1), **3=Decay** (1→0), **4=done**
All durations in audio samples. One-shot: runs once, then stays at `envValue=0`.

#### Type=2 — ADSR

Stages: **1=Attack** (0→1), **2=Decay** (1→sustainLevel), **3=Sustain** (hold at sustainLevel), **4=Release** (sustainLevel→0), **5=done**
`sustainLevel` = `slot.sustain / 255.0f`
Release is implemented: `PlaybackController.scheduleNoteOff()` sends a soft-kill at step end; ADSR/TRIG voices auto-stop when stage 5 is reached on VOL mods.

#### Type=4 — DRUM

Identical stage machine to AHD (Attack→Hold→Decay). Semantic difference only:
- **ATK** = spike/transient attack time (typically 0 for instant hit)
- **HOLD** = body duration (the "thud")
- **DEC** = tail decay time

Use on VOL destination for percussive envelope shaping. Future: may diverge with a dedicated peak-shape curve.

#### Type=5 — TRIG

Identical stage machine to ADSR (Attack→Decay→Sustain). Future: will be externally triggered by a source instrument/track ID rather than the note trigger itself.

#### Type=3 — LFO

Always in stage=1 (running). Phase advances each callback:
```
phaseAdvance = 2π × lfoHz / sampleRate × numFrames
```
Phase resets to 0 on every new note trigger (RETRIG behavior by default).

**Oscillator shapes:**

| oscShape | Name | Output range | Formula |
|----------|------|-------------|---------|
| 0 | TRI | -1 to +1 | Triangle, 0 at start, peak at 25%, zero at 50%, trough at 75% |
| 1 | SIN | -1 to +1 | `sinf(phase)` |
| 2 | RMP+ | -1 to +1 | Rising sawtooth: `norm × 2 - 1` |
| 3 | RMP- | -1 to +1 | Falling sawtooth: `1 - norm × 2` |
| 6 | SQU+ | ±1 | Square, starts high: `norm < 0.5 ? +1 : -1` |
| 7 | SQU- | ±1 | Square, starts low: `norm < 0.5 ? -1 : +1` |
| 4,5,8,9 | EXP+/EXP-/RND/DRNK | -1 to +1 | Falls back to SIN (future) |

---

### Destinations

#### dest=1 — VOLUME

Applied per-sample in the mix loop after all DSP effects:

**AHD/ADSR** (`envValue` 0→1→0):
```
finalVol = max(0, finalVol + (envValue - 1) × amount)
```
- `envValue=0`: volume reduced by `amount` (silence if `amount=1.0`)
- `envValue=1`: no change (full volume)
- Creates a fade-in during attack, full during hold, fade-out during decay

**LFO** (`envValue` -1 to +1):
```
finalVol = max(0, finalVol × (1 + envValue × amount))
```
- Tremolo effect; `amount=1.0` → swings between 0× and 2× volume

#### dest=3 — PITCH

Accumulated once per audio callback in `updateVoiceModulation`, applied via `getModulatedPlaybackRate`:

```
modPitchOffset += envValue × amount × 12  (semitones)
pitchMod = pitchOffset + modPitchOffset   (+ vibrato)
rateMod  = 2^(pitchMod / 12)
finalRate = basePlaybackRate × rateMod
```

**Amount → semitone depth table:**

| amount (hex) | normalized | peak swing |
|---|---|---|
| 0x10 (16) | 0.063 | ±0.75 st |
| 0x2B (43) | 0.169 | ±2.0 st |
| 0x55 (85) | 0.333 | ±4.0 st |
| 0x80 (128) | 0.502 | ±6.0 st |
| 0xFF (255) | 1.0 | ±12.0 st (1 octave) |

For AHD/ADSR on PITCH: `envValue` (0→1) means pitch sweeps from 0 up to `+amount×12` semitones.
For LFO on PITCH: `envValue` (±1) means vibrato swings ±`amount×12` semitones around centre.

The scale factor `×12.0` maps full amount to ±1 octave. Typical vibrato: `amount 0x10–0x20`.

---

### What IS implemented (as of 2026-03-13)

- ✅ **All destinations**: VOL, PAN, PITCH, FINE_PITCH, FILTER_CUTOFF, FILTER_RES, SAMPLE_START
- ✅ **Mod-to-mod routing**: dest=8 (MOD_AMT), dest=9 (MOD_RATE), dest=10 (MOD_BOTH); N→N+1 circular
- ✅ **ADSR release**: `scheduleNoteOff` in `PlaybackController` sends soft-kill at step end; ADSR/TRIG voices auto-stop when stage 5 reached on VOL mods
- ✅ **PAN mod**: `Voice.basePan` + `modPanOffset` (±0.5); recalculates pan law per callback
- ✅ **FILTER mod**: `modCutOffset/modResOffset` (±255); recalculates biquad per callback when active
- ✅ **Offline render**: `pushInstrumentModulation` per instrument before `renderOffline`, per-frame mod update applied
- ✅ **Envelope interpolation**: `prevEnvValue` snapshot + per-sample lerp on falling transitions (eliminates AHD crackling)

### Known limitations (Post-MVP)

- EXP+/EXP-/RND/DRNK LFO shapes (currently fall back to SIN)
- TRACKING mod type not yet implemented
- Free-running LFO mode (currently always retriggers on new note)