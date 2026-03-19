# PocketTracker Code Review

**Date:** 2026-03-19
**Reviewers:** skoomabwoy (via Claude Code)
**Scope:** Full codebase review — architecture, audio, data model, input/rendering, build config
**Codebase:** ~22,500 lines Kotlin + ~2,850 lines C++ across 50 Kotlin files and 1 C++ file

> **Framing note:** This review is from a fresh pair of eyes. Some findings may be intentional
> design decisions — we've tried to flag where we suspect that's the case. Items are grouped
> by priority, not by subsystem.

---

## Critical — Must Fix Before Public Release

### 1. Package name is still `com.example.pockettracker`

**Where:** `app/build.gradle.kts` (namespace + applicationId), all source files

The Android Studio template package name is used throughout. Google Play will reject this,
and it looks unprofessional. Changing it later is painful (touches every file, breaks existing
installs on test devices).

**Suggestion:** Pick a production name now (e.g., `com.conanizer.pockettracker`) and do the
rename in Android Studio (Refactor > Rename Package). The longer you wait, the more painful
this gets.

---

### 2. GitHub token embedded in BuildConfig (ACRA crash reporting)

**Where:** `app/build.gradle.kts:36-47`, `GitHubIssueSender.kt:38`

```kotlin
val githubToken: String = localProps.getProperty("github.token=", "")
buildConfigField("String", "GITHUB_TOKEN", "\"$githubToken\"")
```

The token is read from `local.properties` (good — not committed), but it gets baked into
the compiled DEX at build time. Anyone can decompile the release APK and extract it. With
this token they can create/modify/close issues on the repo.

**Options:**
- Route crash reports through a small server you control (holds the token securely)
- Use a GitHub App token with minimal permissions (create issues only)
- Accept that release builds have no token (crash reporting only works in dev)

---

### 3. ProGuard/R8 rules missing — release builds will crash

**Where:** `app/proguard-rules.pro` (empty template), `app/build.gradle.kts:74` (`isMinifyEnabled = false`)

When you enable minification for release (you should — it shrinks APK and obfuscates), R8
will rename the JNI method signatures in `OboeAudioBackend` and the C++ side won't find them.
The app will crash immediately on launch.

**Minimum rules needed:**
```proguard
-keep class com.example.pockettracker.platform.android.OboeAudioBackend {
    native <methods>;
}
-keep class com.example.pockettracker.crash.** { *; }
-keep class org.acra.** { *; }
```

---

### 4. minSdk mismatch — code says 24, README says 26

**Where:** `app/build.gradle.kts:40` (`minSdk = 24`), `README.md:34` ("Android 8.0+ (API 26)")

API 24 = Android 7.0 Nougat. API 26 = Android 8.0 Oreo. These are different devices with
different capabilities. Which is the actual target?

**Question for Conan:** Is there a reason to support API 24, or was 26 always the intent?
If 26, bump the gradle config. If 24, update the README.

---

## High — Architectural Concerns

### 5. MainActivity.kt is a 2,832-line god object

**Where:** `MainActivity.kt` (entire file)

This single file handles: window configuration, permissions, platform initialization, UI
composition, all input routing (~1,600 lines of button handlers), render workflow, dialog
state, file browser logic, copy/paste, layout persistence, and coroutine management.

**Specific risks:**
- **Orphaned coroutines** (lines ~1139, ~1407): Uses `CoroutineScope(Dispatchers.Default).launch`
  instead of `lifecycleScope`. If the Activity is destroyed mid-render, the coroutine keeps
  running and tries to update destroyed UI state.
- **Mixer polling loop** (line ~579): `LaunchedEffect` with `while(true)` — navigating away
  and back creates a second concurrent loop.
- **stateVersion counter** (line ~275): Manual recomposition trigger via `stateVersion++`
  instead of idiomatic Compose `StateFlow` + `collectAsState()`. Works, but fragile and
  non-standard.

**Not suggesting a full rewrite** — but extracting the button handlers (~1,600 lines) into
a separate file and using `lifecycleScope` for coroutines would be high-value, low-risk
changes.

**Question for Conan:** Was the stateVersion counter pattern a deliberate choice to avoid
ViewModel/StateFlow complexity, or something that grew organically? Both are valid — just
want to understand the reasoning.

---

### 6. PlaybackController.kt is 2,155 lines with duplicated logic

**Where:** `core/logic/PlaybackController.kt`

This is the second-largest file and handles: playback state, audio scheduling, continuous
lookahead buffering, persistent per-track effect state (18 fields per track), offline render
scheduling, and arpeggio logic.

**Specific concerns:**
- **Duplicated song scheduling** (~lines 458-551 vs ~890-945): `updatePlaybackBuffer()` and
  `scheduleSongForRender()` contain near-identical song scheduling logic. A bug fix in one
  must be manually replicated in the other.
- **scheduleStepWithEffects()** is 800+ lines of nested conditionals in a single method.
- **TrackState** has 18 mutable fields tracking different persistent effects — could be split
  by effect category for clarity.

**Suggestion:** Extract the shared song scheduling into a helper method. This is the
lowest-effort, highest-value refactor here.

---

### 7. native-audio.cpp is 2,848 lines in a single file with no headers

**Where:** `app/src/main/cpp/native-audio.cpp`

The entire C++ audio engine — voice management, DSP, effects, modulation, JNI bindings — is
in one compilation unit. No `.h` files, no separation.

**Implications:**
- Can't unit test individual C++ components
- Any change risks breaking everything (no interface boundaries)
- Hard for a second developer to navigate

**What's good about it:** The actual DSP code quality is excellent. Thread safety is correct
(proper mutex usage, no allocations in the audio callback, lock-free where it matters). The
voice stealing algorithm is sophisticated (3-step priority fallback). Sample-accurate timing
via priority queue is impressive.

**Question for Conan:** Is keeping it as one file intentional for simplicity? Splitting into
`AudioEngine.h/.cpp`, `VoiceManager.h/.cpp`, and `native-audio-jni.cpp` (JNI wrappers only)
would make it more maintainable without changing any behavior.

---

### 8. Zero test coverage

**Where:** `app/src/test/`, `app/src/androidTest/`

Only auto-generated placeholder tests exist (`addition_isCorrect`, `useAppContext`). For a
"feature-complete v1.0 candidate," this means every change is a manual testing burden.

The codebase is actually well-structured for testing — `EffectProcessor`, `InputController`,
`ClipboardManager`, and the data model have zero Android dependencies and could be tested
with plain JUnit today.

**Suggestion:** Even 10-20 unit tests covering `EffectProcessor.resolveStepParams()` and
`InputController` action dispatch would catch regressions in the most complex logic.

---

### 9. No project file versioning / migration strategy

**Where:** `core/data/TrackerData.kt` — `Project` data class has no `version` field

If a field is added to `Instrument` or `PhraseStep` in a future update, old `.ptp` files
will silently use Kotlinx.serialization defaults. There's no way to detect which version
saved a file, and no migration hook.

**Suggestion:** Add `val version: Int = 1` to `Project`. When loading, check version and
apply any needed transformations before deserialization. This is cheap to add now and
painful to retrofit later.

---

## Medium — Code Quality

### 10. Cursor position explosion in TrackerController

**Where:** `core/logic/TrackerController.kt` (~lines 103-216)

There are 16+ separate cursor position variables: `cursorRow`, `cursorColumn`,
`projectCursorRow`, `instrumentCursorRow`, `tableCursorRow`, `grooveCursorRow`,
`modCursorRow`, `songCursorRow`, `chainCursorRow`, etc.

**Alternative:** `Map<ScreenType, CursorState>` would collapse these into one data structure
and make "remember cursor per screen" automatic for any new screen.

---

### 11. Magic numbers scattered across files

**Where:** Multiple files

- `12` tics per step (PlaybackController, EffectProcessor — hardcoded in multiple places)
- `256` phrases/chains/instruments/tables/grooves (TrackerData, various controllers)
- `16` phrase steps, `8` tracks (everywhere)
- `500L` ms multi-tap window (InputController)
- `150` px minimum button panel (DeviceAdapter)

**Suggestion:** Centralize into a `TrackerConfig` object. Not urgent, but prevents the
"changed in one place, forgot the other" class of bugs.

---

### 12. PhraseStep uses `var` (mutable) fields

**Where:** `core/data/TrackerData.kt:53`

All other data classes use `val` (immutable), but `PhraseStep` uses `var` for `note`,
`instrument`, `volume`. This means `Phrase.equals()` (which uses `contentEquals`) can
silently break if a step is mutated after the phrase is copied.

**Question for Conan:** Is mutability needed for performance in the audio scheduling path,
or can these be switched to `val` with `copy()`?

---

### 13. Duplicate instrument references can desync

**Where:** `TrackerController.currentInstrument` vs `InstrumentController.currentInstrument`

Both controllers track "which instrument is selected." There's a manual sync call (line ~427:
"Sync InstrumentController to TrackerController's currentInstrument before previewing"). If
someone forgets to sync, the wrong instrument plays.

**Suggestion:** Single source of truth — either TrackerController owns it and
InstrumentController reads from there, or vice versa.

---

### 14. No denormal handling in C++ biquad filters

**Where:** `native-audio.cpp` (~lines 372-376, filter state variables `y1`/`y2`)

After silence or very quiet passages, biquad filter state variables can become denormal
(< 1e-38), causing 100-1000x CPU slowdown on some ARM cores. Modern ARM is generally
tolerant, but older Cortex-A53 (common in budget handhelds) can be affected.

**Fix:** Either flush-to-zero CPU flag at thread start, or a periodic `if (fabsf(y1) < 1e-15f) y1 = 0.0f;` check.

**Question for Conan:** Have you seen any audio glitches after long silent passages on the
Miyoo Flip? If not, this might be a non-issue on your target hardware.

---

### 15. Oboe and ACRA versions hardcoded outside version catalog

**Where:** `app/build.gradle.kts:96-99` vs `gradle/libs.versions.toml`

All other dependencies use the version catalog, but Oboe (`1.10.0`) and ACRA (`5.11.3`) are
hardcoded in `build.gradle.kts`. Minor inconsistency.

---

## Low / Observations

### 16. 24 module files in root package (flat structure)

All screen modules (`PhraseEditorModule.kt`, `ChainEditorModule.kt`, `MixerModule.kt`, etc.)
sit in the root `com.example.pockettracker` package alongside `MainActivity.kt`. A `ui/` or
`modules/` sub-package would reduce clutter.

**Note:** This is purely organizational — no behavior change.

---

### 17. Compose + Canvas is the RIGHT choice

We initially questioned whether Compose was being misused, but the
`PixelPerfectRenderer` comments (lines 146-159) explain exactly why:
- `BoxWithConstraints` / `SubcomposeLayout` can destroy and recreate child composables,
  causing SEGV_ACCERR in RenderThread
- Allocating module objects per frame causes GC pressure that crashes Snapdragon GPU drivers
  on Android 11

The single `Canvas` approach with `remember {}` outside the draw lambda is correct and
well-documented. This shows deep understanding of Android graphics internals.

---

### 18. CursorContext pattern is elegant, not overengineered

Each module returns a typed context describing what the cursor is on (note, hex byte, etc.)
and the `InputController` uses it to determine behavior. This eliminates hundreds of lines
of per-screen if/else logic. Well designed.

---

### 19. Platform abstractions are genuinely portable

`IAudioBackend`, `IFileSystem`, `IResourceLoader` have zero Android dependencies. A Linux
port with ALSA/PulseAudio + std::filesystem would be straightforward. The one leak:
`AudioEngine.kt` has a few `android.util.Log.d()` calls that should go through the
`ILogger` interface.

---

### 20. Input system two-layer split is clean

`ButtonHandlers` = "what physical button was pressed?"
`InputController` = "what semantic action should happen given the cursor context?"

This separation allows swapping input sources without touching business logic. Good design.

---

### 21. Audio engine quality is excellent

Despite being in a single file, the C++ audio engine demonstrates:
- Correct mutex usage (no locks held during DSP processing)
- Zero allocations in the audio callback
- Sample-accurate note scheduling via priority queue
- Sophisticated 3-step voice stealing
- Proper JNI usage (JNI_ABORT for read-only, null checks)
- Hard limiter at ±0.9886 to prevent clipping

The one area to watch: no NaN/Inf guards on filter coefficient calculation (could produce
garbage output if cutoff parameter is extreme). Low probability in practice.

---

## Pass 2 — Screen Modules, File/Storage Layer

### 22. Offline rendering has a race condition with live playback

**Where:** `core/logic/RenderController.kt` (~lines 49-110)

`audioBackend.setOfflineRendering(true)` is a global flag. If the user starts playback while
a render is in progress, both paths try to use the same `audioBackend` state. Also, if an
exception occurs between entering offline mode (line ~49) and the finally block (line ~110),
offline mode can remain stuck ON — silently blocking live playback until app restart.

**Suggestion:** Either mutex-protect the render path, or disable the play button while
rendering.

---

### 23. MediaCodec leak in AndroidVideoAudioExtractor

**Where:** `platform/android/AndroidVideoAudioExtractor.kt` (~lines 98-102)

If `codec.configure()` throws, the codec object is never released:

```kotlin
val codec = MediaCodec.createDecoderByType(mime).also {
    it.configure(format, null, null, 0)  // Can throw!
    it.start()
}
```

Should use try-finally to ensure `codec.release()` is called even on configuration failure.

---

### 24. Font constants duplicated 10 times across modules

**Where:** Every `*Module.kt` file

```kotlin
private val FONT_SCALE = 3
private val CHAR_SPACING = 2
private val ROW_HEIGHT = 21
private val TEXT_PADDING = 3
```

These identical 4 lines appear in PhraseEditorModule, ChainEditorModule, SongEditorModule,
InstrumentModule, ProjectModule, FileBrowserModule, MixerModule, TableModule, GrooveModule,
and ModulationModule.

**Suggestion:** Extract to a shared `TrackerFontConstants` object or top-level constants.

---

### 25. Row background color logic duplicated in 4 grid modules

**Where:** PhraseEditorModule, ChainEditorModule, SongEditorModule, TableModule

The exact same `when` block determining row color (playback highlight, selection highlight,
cursor row, alternating stripes) is copy-pasted across all four grid editors. A bug fix in
one must be manually replicated in the other three.

**Suggestion:** Extract to a shared function in `EditorHelpers.kt`.

---

### 26. Hex formatting pattern repeated 50+ times

**Where:** All modules that display hex values

The pattern `value.toString(16).padStart(2, '0').uppercase()` appears throughout. Should be
an extension function: `fun Int.toHex2(): String`.

---

### 27. handleInput signatures are inconsistent across modules

**Where:** All modules with `handleInput()`

Three different patterns exist:
- Most modules: `handleInput(state, action)` — 2 params
- PhraseEditor, Instrument: `handleInput(state, action, instrumentController)` — 3 params
- Mixer: `handleInput(state, action, onProjectModified)` — callback instead of controller

**Question for Conan:** Is this intentional (each module gets only what it needs), or did it
grow organically? Standardizing would make it easier for new contributors to add modules.

---

### 28. Silent fallback to internal storage

**Where:** `platform/android/AndroidFileSystem.kt` (~lines 45-48, 72-74)

If creating `Documents/PocketTracker/` on external storage fails, the code silently falls
back to app-internal storage. The user thinks their files are in Documents (accessible,
survives uninstall) but they're actually in app-private storage (lost on uninstall).

**Suggestion:** At minimum, log a warning. Ideally, surface this to the user via a status
message.

---

### 29. No disk space check before writing files

**Where:** `platform/android/AndroidFileSystem.kt` — `writeFile()` and `writeBytes()`

If the device runs out of space mid-write, the file will be corrupted (partial write). For
project saves, this means data loss. A pre-check or atomic write (write to temp, then
rename) would protect against this.

---

### 30. WavWriter integer overflow for large files

**Where:** `core/storage/WavWriter.kt` (~line 47)

```kotlin
val dataSize = numSamples * blockAlign
```

If `numSamples` exceeds ~500M (a ~3-hour stereo render at 44.1kHz), this overflows `Int`.
The WAV header will have a wrong size and the file will be unreadable.

**Suggestion:** Either cap render length with a clear error, or use `Long` for the
calculation.

---

### 31. GitHubIssueSender never validates HTTP response

**Where:** `crash/GitHubIssueSender.kt` (~line 49)

`conn.responseCode` is read but never checked. If the GitHub API returns 401, 422, or any
error, the crash report silently disappears. No retry, no fallback.

---

### 32. Module interface pattern is clean and consistent

**Positive finding.** All 12 modules correctly implement the `TrackerModule` interface:
- `draw()` signatures are identical
- `getCursorContext()` is present on all interactive modules (10/12)
- Display-only modules (Oscilloscope, NavigationMap) correctly skip input handling
- The module architecture makes adding new screens straightforward

---

### 33. InstrumentModule and ProjectModule size is justified

Both are larger than other modules (1,050 and 752 lines respectively) because they have many
editable parameters (15 and 8 rows). The extracted helper functions
(`drawParameterRow()`, `drawDualParameterRow()`) are well-structured. Not a code smell —
just genuinely complex screens.

---

## Summary

| Category | Count | Key items |
|----------|-------|-----------|
| **Critical** (must fix for release) | 4 | Package name, token security, ProGuard, minSdk |
| **High** (architectural / bugs) | 7 | MainActivity size, PlaybackController duplication, no tests, no versioning, offline render race, MediaCodec leak, silent storage fallback |
| **Medium** (code quality) | 10 | Cursor explosion, magic numbers, mutability, denormals, font constant duplication, row color duplication, hex formatting, handleInput inconsistency, disk space, WAV overflow |
| **Low / Positive** | 8 | Good patterns confirmed (Canvas, CursorContext, input split, audio quality, module interface, platform portability, InstrumentModule structure) |

**Total: 29 actionable findings + 8 positive observations = 37 items across 50 Kotlin + 1 C++ files.**

**Overall assessment:** The core architecture is sound and the audio engine is impressive.
The main risks are around release readiness (package name, ProGuard, token) and long-term
maintainability (large files, no tests, scattered duplication). The platform abstraction
layer is genuinely portable — a Linux port would be realistic.

The codebase shows clear signs of iterative development by a solo developer who understands
audio programming deeply. The suggested improvements are about making it sustainable for
collaborative development, not about fixing broken fundamentals.
