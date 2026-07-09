# PocketTracker — Technical Architecture

How PocketTracker is built. This document describes the system as it currently works.

**Audience:** developers and contributors.

---

## Table of Contents

1. [Overview](#overview)
2. [Source Layout](#source-layout)
3. [Audio Engine](#audio-engine)
4. [Adding a New Engine Call](#adding-a-new-engine-call)
5. [SoundFont (SF2) Engine](#soundfont-sf2-engine)
6. [Data Model](#data-model)
7. [Rendering System](#rendering-system)
8. [Theme System](#theme-system)
9. [Screen Overlay System](#screen-overlay-system)
10. [Navigation System](#navigation-system)
11. [File Management](#file-management)
12. [Modulation Engine](#modulation-engine)
13. [Technology Stack](#technology-stack)
14. [Coding Conventions](#coding-conventions)

---

## Overview

PocketTracker is split into a **platform-agnostic core** and **platform adapters**. The core (data
model, business logic, the C++ audio/DSP engine) has no Android dependencies; Android-specific code
(Oboe, scoped storage, Compose UI, input device handling) lives behind interfaces. The Android app is
one set of adapters over that core; a Linux build would add a second set without touching the core.

The two halves mirror each other by name:

- **Kotlin:** `IAudioBackend` (interface) ← `OboeAudioBackend` (Android impl). `AudioEngine` (core, portable) calls the interface.
- **C++:** `AudioEngine` (portable core — all voices, scheduling, DSP) ← `OboeAudioEngine` (Android backend, the only Oboe-coupled translation unit).

---

## Source Layout

```
core/                              # Platform-agnostic Kotlin — NO Android imports
├── audio/
│   ├── IAudioBackend.kt           Audio backend interface
│   └── AudioEngine.kt             Platform-agnostic coordinator/facade over the backend
├── data/
│   └── TrackerData.kt             Pure data classes (Note, PhraseStep, Phrase, Chain, Instrument, Project…)
├── logic/
│   ├── TrackerController.kt       Navigation, screen + cursor state
│   ├── InputController.kt         Increment/decrement, selection system
│   ├── PlaybackController.kt      Phrase/chain/song scheduling, tics
│   ├── EffectProcessor.kt         Effect constants + calculations
│   ├── InstrumentController.kt    Sample management, resampling
│   ├── FileController.kt          Save/load orchestration, autosave, migration
│   ├── SongTraversal.kt           Shared song-walk helper
│   └── ClipboardManager.kt        Copy/paste
├── resources/
│   └── IResourceLoader.kt         Sample/asset loading interface
└── storage/
    ├── IFileSystem.kt             File I/O interface
    ├── FileInfo.kt                Platform-agnostic file metadata
    ├── WavWriter.kt / WavStreamWriter.kt   WAV export
    └── …

input/
├── AppInputDispatcher.kt          Button-handler logic; wired via AppControllers + AppStateRefs
├── ButtonHandlers.kt              Input mapping, key combos, key repeat
└── CursorContext.kt              Value-type system + factory methods

platform/android/                  # Android imports allowed here
├── OboeAudioBackend.kt            IAudioBackend impl (JNI to the native engine)
├── AndroidResourceLoader.kt       Asset loader
├── AndroidFileSystem.kt           Scoped-storage IFileSystem impl
└── DeviceAdapter.kt               Android InputDevice API, layout modes

ui/
├── EditorHelpers.kt               Shared rendering utilities (toHex2, rowBgColor, darken()…)
├── PixelPerfectRenderer.kt        Compose Canvas rendering + pixel font + glyph atlas
├── ScreenLayouts.kt               Top-level screen composition, overlay compositing, virtual controls
└── modules/                       One module per screen (Phrase, Chain, Song, Instrument,
                                   InstrumentPool, SampleEditor, Table, Groove, Modulation, Mixer,
                                   Effect, Eq, Settings, Project, FileBrowser, Oscilloscope…)

MainActivity.kt                    Thin coordinator: creates backends/controllers/modules, wires
                                   AppControllers + AppStateRefs + AppInputDispatcher, renders UI

app/src/main/cpp/                  # C++ audio engine
├── audio-engine.cpp / .h          PORTABLE core (no Oboe): processAudioBlock, processLiveBlock, renderOffline
├── oboe-audio-engine.cpp / .h     Android backend: owns the Oboe stream, onAudioReady → core.processLiveBlock
├── jni-bridge.cpp                 JNI entry points (thin thunks)
├── native-audio.cpp               Thin stub redirect
├── sampler-voice.h                Per-voice state for sample-playback voices
├── soundfont-voice.h / .cpp       Per-voice state for SF2 / TinySoundFont voices
├── note-queue.h                   Sample-accurate note + param scheduling queues
├── audio-defs.h                   Constants (MAX_VOICES, DECLICK_SAMPLES, FX_*), logging shim
├── mods/                          Modulation engine
│   ├── mod-system.h               Routing (modSourceValues → modDestValues)
│   ├── mod-runner.h               runModMatrix() orchestration
│   ├── modules/                   AHD/ADSR/LFO/pitch-slide/vibrato tick functions
│   └── primitives/                lfo-oscillator.h (shared LFO/vibrato shaping)
├── vendor/                        Third-party (tsf, dr_mp3, dr_flac, stb_vorbis, opus, opusfile)
└── effects/                       DSP module system (three-layer)
    ├── instrument-chain.h         Per-voice chain: Crush → Drive → Filter
    ├── send-chain.h               Stereo send buses: reverb (DaisySP ReverbSc) + stereo delay
    ├── master-chain.h             Output bus: masterEq → OttModule | DustChain → LimiterModule
    ├── primitives/                biquad.h, filter.h (Audio EQ Cookbook), vendored DaisySP
    └── modules/                   FilterModule (LP/HP/BP via daisysp::Svf), DriveModule, BitcrushModule
```

**The hard rule:** `core/**` must contain no Android imports. Platform specifics go behind the
interfaces in `core/audio`, `core/storage`, `core/resources`, implemented under `platform/android`.

---

## Audio Engine

A sample-accurate queue system in C++.

- Oboe-based real-time output at 44.1 kHz (OpenSL ES preferred, AAudio fallback). The stream-open
  ladder tries OpenSL ES Exclusive → Shared → None/Shared → AAudio Exclusive.
- Audio init runs off the main thread (`Dispatchers.IO`) so startup never freezes the UI.
- Sample-accurate note scheduling driven by a global frame counter: note onsets start at their
  exact target frame (mid-block triggers render from an intra-block offset). Kills and param
  updates are applied at block granularity — up to one audio burst early. ~<50 ms latency on
  tested hardware.
- 8-voice polyphony with per-track voice stealing.
- Linear interpolation for pitch-shifting (no aliasing).
- Per-voice DSP chain: Downsample (pre-interpolation) → Interpolate → Crush → Drive → Filter → Volume.
- Resonant SVF / biquad filters (LP/HP/BP, Audio EQ Cookbook coefficients).
- Real-time waveform capture for the oscilloscope and spectrum visualizers.

### Engine / backend split

- **`audio-engine.{h,cpp}` — portable core (`class AudioEngine`):** voices, note scheduling, the
  sample-accurate queues, and **all** DSP (`processAudioBlock`). No `<oboe/*>` or `<android/*>`; logging
  goes through the `audio-defs.h` shim. It caches the device rate (`setDeviceSampleRate`) and wakes a
  paused stream through a `resumeHook` rather than touching a stream object directly.
- **`oboe-audio-engine.{h,cpp}` — Android backend (`class OboeAudioEngine : oboe::AudioStreamDataCallback`):**
  the only Oboe-coupled translation unit. Owns the `oboe::AudioStream`, runs the device-specific
  stream-open ladder, and in `onAudioReady` forwards the buffer to `core->processLiveBlock(...)`.

A different platform adds a sibling backend (e.g. an ALSA/JACK class) that owns its own stream and
calls the same `processLiveBlock()`; the core compiles unchanged.

### Audio processing chain rule

**All audio processing lives in `processAudioBlock()` in `audio-engine.cpp`.** `onAudioReady()` (live)
and `renderOffline()` (WAV export) are thin wrappers that call `processAudioBlock()` and add only
output-specific work on top:

| Step | processAudioBlock | onAudioReady only | renderOffline only |
|------|:-:|:-:|:-:|
| Kill / note / param queues | ✅ | | |
| Table ticks | ✅ | | |
| Pitch / ADSR / LFO modulation | ✅ | | |
| Per-voice DSP chain + mix | ✅ | | |
| Send buses + master bus + limiter | ✅ | | |
| Waveform / spectrum capture | | ✅ | |
| Peak-meter tracking | | ✅ | |
| Offline-silence gate (`isOfflineRendering`) | | ✅ | |
| Chunked render loop | | | ✅ |

If you add a new audio feature (effect, modulation destination, …), add it to `processAudioBlock()` —
never to `onAudioReady`/`renderOffline`, or it will be missing from one of the two outputs.

### Bus structure

- **Per voice:** instrument chain (Crush → Drive → Filter) + constant-power pan.
- **Send buses:** stereo reverb (DaisySP ReverbSc) and stereo delay; delay output can feed the
  reverb input with no extra latency.
- **Master bus:** `masterEq` → OTT 3-band compressor **or** DUST lo-fi chain (switchable) → soft
  peak limiter. The limiter is always on.

The DSP code is organised in three layers: **primitives** (stateless math / coefficient helpers),
**modules** (one effect each, with state), and **chains** (ordered module pipelines).

---

## Adding a New Engine Call

The portability seam is wide: `IAudioBackend` has ~100 methods, `OboeAudioBackend` mirrors each as an
`external fun`, `jni-bridge.cpp` has a hand-written thunk per call, and many `AudioEngine` methods are
one-line forwards. Adding one engine feature means touching the same files in lockstep. Because the JNI
link resolves lazily, a missed step is an `UnsatisfiedLinkError` at runtime, not a compile error.

To add `fooBar(id: Int, gain: Float)`, edit in this order:

1. **C++ engine (`audio-engine.{h,cpp}`)** — implement the behaviour as an `AudioEngine` method. All
   DSP must live in `processAudioBlock` (see the chain rule); `fooBar` only mutates state the mix loop
   reads. Guard audio-thread-shared state with the existing mutex discipline.
2. **JNI thunk (`jni-bridge.cpp`)** — add `Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1fooBar(...)`
   that marshals args and calls `engine->fooBar(...)`. The symbol must match the package path; `_` in
   the Kotlin name becomes `_1`; array args need `Get/ReleaseFloatArrayElements` (see `native_loadSample`).
3. **Backend impl (`platform/android/OboeAudioBackend.kt`)** — `private external fun native_fooBar(...)`
   plus the interface override `override fun fooBar(...) = native_fooBar(...)`.
4. **Interface (`core/audio/IAudioBackend.kt`)** — add `fun fooBar(...)` so core code can call it
   portably and any future backend must implement it.
5. **Facade (`core/audio/AudioEngine.kt`)** — the method core/UI actually calls; logic here (no Android
   imports), or a pure pass-through.

**Verification:** run the app and exercise the call — a clean compile does not prove the thunk name is correct.

---

## SoundFont (SF2) Engine

Implemented with **TinySoundFont (TSF)** — a single-header C++ SF2 synthesizer, with a small fork patch
for per-channel rendering.

- One shared `tsf*` handle per SF2 file slot (up to 8 active SF2 files at once; true-LRU eviction).
- Each track maps to a MIDI channel on the slot's handle (track 0 → ch 0 … track 7 → ch 7).
- `tsf_render_float_channel(h, t, buf, frames, 0)` (forked) renders one MIDI channel at a time into a
  per-track buffer, enabling per-instrument post-processing.
- Memory ≈ 1× the SF2 file size (one handle per file). Instruments sharing one file are de-duplicated
  onto the same handle and stay isolated via per-note bank/preset and ADSR overrides.
- Full modulation parity with the sampler: the same `updateVoiceModulation()` runs for `SoundfontVoice`
  (AHD/ADSR/LFO/DRUM/TRIG, all destinations), and table effects and pitch slides work identically.
- Per-instrument effects (filter/drive/bitcrush) are applied to each track's SF buffer post-render.
- SF preset overrides (ATK/DEC/SUS/REL/filterCut/filterRes) are patched into TSF regions via
  `applySoundfontEnvelopeOverrides()` at note trigger.

---

## Data Model

`core/data/TrackerData.kt` is plain, serializable Kotlin data classes with no Android dependencies —
it serializes to JSON for `.ptp` project files and could be mirrored as C++ structs for a Linux build.

```kotlin
@Serializable
data class Note(val pitch: Int, val octave: Int) { /* toMidi(), toFrequency() */ }

@Serializable
data class PhraseStep(
    var note: Note = Note.EMPTY,
    var instrument: Int = 0x00,
    var volume: Int = 0xFF,
    var fx1Type: Int = 0x00, var fx1Value: Int = 0x00,   // + fx2, fx3
)

@Serializable data class Phrase(val id: Int, val steps: Array<PhraseStep>)
@Serializable data class Chain(val id: Int, val phraseRefs: IntArray, val transposeValues: IntArray)
@Serializable data class Instrument(/* sampleId, volume, pan, filters, loop, modSlots… */)
@Serializable data class Project(
    var name: String, var tempo: Int,
    val phrases: Array<Phrase>, val chains: Array<Chain>,
    val instruments: Array<Instrument>, val tracks: Array<Track>,
)
```

*(Illustrative — `TrackerData.kt` is authoritative for exact fields, types, and defaults, and for the
pool sizes set on `Project`.)* Projects carry a `version` field; `FileController.migrateProject()`
forward-migrates older files after deserialization.

**Compose note:** state changes must produce new objects (`copy(...)`), never in-place mutation, or
recomposition won't fire.

---

## Rendering System

Pixel-perfect Canvas rendering at a fixed **640×480** design resolution, integer-scaled and
letterboxed to the device screen. `PixelPerfectRenderer` computes the scale, then draws each screen's
module.

- **5×5 bitmap font**, scaled 3× = 15×15 px glyphs. A glyph atlas pre-renders the 128 ASCII glyphs once
  into a single bitmap and stamps each with one tinted `drawImage` (avoids thousands of draw ops/frame).
- **`TrackerModule`** interface: every screen implements `draw(...)` plus `width`/`height`. Modules read
  state objects and render independently; drawing logic itself is platform-pure (only `DrawScope`/`Canvas`
  are Compose-specific).
- The oscilloscope/visualizer refresh loop only forces redraws while audio is audible; when idle, the
  Canvas repaints solely on real state changes (cursor move, value edit, playback row), which keeps the
  handheld from re-rendering 60×/sec on a static screen.

Shared layout constants (`SCREEN_WIDTH`, `FONT_SCALE`, `CHAR_WIDTH`, …) and helpers (`toHex2`,
`rowBgColor`, `darken`) live in `EditorHelpers.kt`.

---

## Theme System

`AppTheme` (`AppTheme.kt`) is a `@Serializable` data class of ARGB `Long` color fields plus a
`visualizerType`, saved to/from `.ptt` theme files. Built-in palettes: **CLASSIC** (green-on-black),
**AMBER**, **BLUE**, **MONO**.

It flows top-down via the `LocalAppTheme` CompositionLocal → `drawLayout(appTheme)` → each module
state's `copy(appTheme = …)`. Inside a draw function: `val t = state.appTheme` → `Color(t.fieldName)`.
`Long.darken(factor)` scales RGB (preserving alpha) for cursor-shadow backgrounds.

---

## Screen Overlay System

PNG overlays (e.g. CRT scanlines) can be layered over the tracker screen at runtime.

- **Assets:** any PNG in `app/src/main/assets/overlays/`; the Settings OVERLAY row lists them
  automatically via `assets.list("overlays")`. Adding an overlay needs no code change.
- **Loading (`MainActivity.kt`):** decoded as-is to an `ImageBitmap`; `STR` (00–FF) is the only runtime control.
- **Compositing (`ScreenLayouts.kt`):** `Modifier.drawWithContent { drawContent(); drawImage(bitmap, alpha = STR/255f) }`
  shares the same canvas as the tracker draw, so the overlay blends over the already-rendered pixels.
- `overlayName` / `overlayStrength` live in `AppStateRefs`, are delegated in `AppInputDispatcher`, and
  persist via SharedPreferences (`overlay_name` / `overlay_strength`).

---

## Navigation System

Screens are arranged on a 5×5 grid; **R + D-pad** moves between them and releasing R jumps to the
selection.

```
Row 0:        —      SCALE   INST_POOL  (INST)*
Row 1:    PROJECT   GROOVE     MODS     PROJECT
Row 2:     SONG     CHAIN    PHRASE   INSTRUMENT  TABLE
Row 3:    MIXER     MIXER    MIXER      MIXER     MIXER
Row 4:   EFFECTS   EFFECTS  EFFECTS    EFFECTS   EFFECTS
```

Navigation is pure logic in `TrackerController` / `NavigationMap` (no Android). SETTINGS and
SAMPLE_EDITOR are popup screens opened contextually, not grid cells.

**Instrument Pool fast-jump:** `(INST)*` at row 0 / col 4 is a contextual cell shown only on INST_POOL
(or the instrument reached from it). From INST_POOL: R+RIGHT → INSTRUMENT, R+LEFT → PHRASE, R+DOWN →
MODS; from that row-0 instrument: R+LEFT → INST_POOL, R+DOWN → MODS. Driven by
`TrackerController.instrumentFromPool` (set on the R+RIGHT jump, cleared in the `currentScreen` setter
on leaving INSTRUMENT). The normal row-2 INSTRUMENT keeps its usual navigation.

---

## File Management

File I/O is behind `IFileSystem`; `AndroidFileSystem` implements it over scoped storage.

```kotlin
interface IFileSystem {
    fun getProjectsDirectory(): String
    fun getSamplesDirectory(): String
    fun listFiles(path: String): List<FileInfo>
    fun readFile(path: String): String
    fun writeFile(path: String, content: String)
    fun deleteFile(path: String): Boolean
    fun createDirectory(path: String): Boolean
    // …
}
```

- **Projects** save/load as `.ptp` (JSON) via `FileController`, default location
  `/Documents/PocketTracker/Projects/`.
- **Instruments** save/load as `.pti` presets (`/Documents/PocketTracker/Instruments/`).
- **WAV export** writes to `/Documents/PocketTracker/Renders/` (full mix), with per-track stems in a
  per-project subfolder.
- **Autosave:** a debounced, app-private `autosave.ptp` is written while there is unsaved work and
  cleared on a clean save/load/new. Its presence at launch signals an unclean exit and drives the
  crash-recovery prompt (`AutosaveManager` + `FileController`).
- **Samples** are loaded by path and decoded natively (no Java-heap copy): WAV directly, and
  MP3/FLAC/OGG/Opus via the vendored decoders. M4A/AAC uses the OS MediaCodec extractor.

---

## Modulation Engine

**Files:** `audio-engine.cpp` + `mods/` headers (C++), `AudioEngine.kt`, `IAudioBackend.kt`,
`TrackerData.kt`.

Each instrument has 4 modulation slots (`modSlots: Array<ModSlot>` on `Instrument`). On `scheduleNote()`
the Kotlin layer pushes the current mod params to C++ (`pushInstrumentModulation`), which copies them
onto the triggered voice. The engine updates each slot once per audio callback
(`updateVoiceModulation`), computing an `envValue` applied to the destination parameter in the mix loop.

### Data model (`TrackerData.kt`)

```kotlin
data class ModSlot(
    var type: ModType = ModType.NONE,    // envelope/LFO type
    var dest: ModDest = ModDest.NONE,    // target parameter
    var amount: Int = 0xFF,              // modulation depth 0x00–0xFF
    // Envelope (AHD, ADSR): attack, hold, decay, sustain, release (ticks / level)
    // LFO: oscShape (TRI/SIN/RMP±/EXP±/SQU±/RND/DRNK), lfoTrigMode, lfoFreq
)
```

- **ModType:** NONE=0, AHD=1, ADSR=2, LFO=3, DRUM=4, TRIG=5 (TRACKING / SCALAR not yet implemented).
- **ModDest:** NONE=0, VOLUME=1, PAN=2, PITCH=3, FINE_PITCH=4, FILTER_CUTOFF=5, FILTER_RES=6,
  SAMPLE_START=7, MOD_AMT=8, MOD_RATE=9, MOD_BOTH=10.

### C++ runtime

- `InstrumentModSlot[256][4]` is the static store, updated from Kotlin before each note.
- `VoiceModSlot[4]` on each voice is copied from the store at trigger time and holds runtime state
  (`stage`, `envValue`, `lfoPhase`, `stageCounter`).
- **AHD:** Attack → Hold → Decay → done (one-shot). **ADSR:** Attack → Decay → Sustain → Release → done;
  `PlaybackController.scheduleNoteOff()` soft-kills at step end, and ADSR/TRIG voices auto-stop once the
  VOL mod reaches the final stage. **DRUM** shares the AHD machine (transient/body/tail framing);
  **TRIG** shares the ADSR machine.
- **LFO** runs continuously; phase advances per callback and resets on each new note (retrigger).
- **Envelope interpolation:** `prevEnvValue` is snapshotted and lerped per-sample on falling
  transitions, which removes stepping/crackle on short decays.
- **Destinations:** VOL (per-sample after DSP), PITCH (accumulated per callback, applied via
  `getModulatedPlaybackRate`), PAN (`basePan` + offset, pan law recomputed per callback), FILTER
  (cut/res offsets recompute biquad coefficients when active), plus mod-to-mod routing (dest 8/9/10,
  N→N+1 circular).
- **Offline render** applies the same modulation per frame so exports match playback.

---

## Technology Stack

| Layer | Technology | Notes |
|-------|------------|-------|
| UI | Jetpack Compose | Android |
| Language | Kotlin 1.9+ | |
| Audio | C++ with Oboe (OpenSL ES / AAudio) | Portable core; Oboe only in `OboeAudioEngine` |
| SF2 synth | TinySoundFont | Single-header, vendored at `vendor/tsf/tsf.h` |
| Decoders | dr_mp3, dr_flac, stb_vorbis, libopus/opusfile | Vendored native decoders |
| Build | Gradle 8.x | |
| Native build | CMake 3.22.1+ | |
| Serialization | Kotlinx Serialization | JSON `.ptp` / `.pti` / `.ptt` |
| Min Android | API 26 (Android 8.0) | 64-bit only (arm64-v8a, x86_64) |

---

## Coding Conventions

**Kotlin** — `PascalCase` classes, `camelCase` functions, `SCREAMING_SNAKE_CASE` constants,
`camelCase` private members (no prefix).

**Comments** — explain *why* when it's non-obvious (a hidden constraint, a surprising invariant, a
bug workaround). Don't restate what well-named code already says. Section separators
(`// ═══`) are fine to group utilities in long files, not inside individual functions.

**Screen modules (`*Module.kt`)** — hex formatting via `.toHex2()`/`.toHex1()`; list-row backgrounds
via `rowBgColor()`; `getCursorContext()` uses `CursorContextFactory.*` exclusively; `draw()` casts
state with an early return (`val s = state as? FooState ?: return`).

**C++** — `UPPER_SNAKE_CASE` constants, `PascalCase` classes, `camelCase` functions and members.
Prefer `std::vector` over raw arrays; guard audio-thread-shared state with the existing mutexes.

**Portability** — never import Android types into `core/**`. New platform capabilities go behind an
interface in `core/`, implemented under `platform/android/`.
