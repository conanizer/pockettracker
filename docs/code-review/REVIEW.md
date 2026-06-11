# PocketTracker Code Review

Staged review by Claude. Each stage is documented here so progress survives context compaction.
Severity legend: 🔴 bug / correctness · 🟠 inconsistency or duplication · 🟡 optimization · 🔵 structure/readability · 💡 idea

## Stage Tracker

| # | Stage | Status |
|---|-------|--------|
| 1 | Data model + serialization (TrackerData, FileController) | ✅ done |
| 2 | Input system (ButtonHandlers, AppInputDispatcher, InputController, CursorContext) | ✅ done |
| 3 | Logic controllers (Playback, Tracker, Instrument, Effect, Clipboard, Render) | ✅ done |
| 4 | Audio Kotlin layer (AudioEngine, IAudioBackend, OboeAudioBackend) | ✅ done |
| 5 | C++ audio engine (audio-engine.cpp, jni-bridge, modules) | ✅ done |
| 6 | UI rendering + modules (PixelPerfectRenderer, modules, EditorHelpers) | ✅ done |
| 7 | Cross-cutting: structure, build, consistency, memory | ✅ done |

Status values: ⏳ pending · 🔄 in progress · ✅ done

---

<!-- Findings appended per stage below -->

## Stage 1 — Data model + serialization

Files: `core/data/TrackerData.kt`, `core/logic/FileController.kt`

### 1.1 🔴 Three different transpose decoders disagree at the 0x80 boundary
`TrackerData.kt` has three sign-decode helpers for the same hex-to-semitone concept:
- `Chain.getTransposeSemitones()` (L110): `value <= 0x80` → **0x80 = +128**
- `Project.getTransposeSemitones()` (L573): `transpose <= 0x80` → **0x80 = +128**
- `TableRow.transposeToSemitones()` (L193): `transpose < 0x80` → **0x80 = −128**

Same byte (0x80) means +128 in chain/project transpose but −128 in a table row. A user
nudging transpose down from 0x00 into 0xFF…0x80 territory gets a 256-semitone discontinuity
in one place but not the others. **Fix:** one shared `fun byteToSignedSemitones(b: Int): Int`
in core and call it from all three. Pick the boundary deliberately (0x80 = −128 is the more
conventional two's-complement reading) and document it.

### 1.2 🔴 (latent) `Project.equals()`/`hashCode()` only compare `name` + `tempo`
`Project` (L577-590) ignores phrases, chains, instruments, tracks, etc. Two completely
different songs with the same name & tempo compare equal. Not currently biting because the
app holds `project` as a plain `var` in `TrackerController` and recomposes via a `projectVersion`
counter — but the moment anyone puts a `Project` in a `Set`/`Map`, dirty-checks two projects, or
moves the project into a `mutableStateOf` (default `structuralEqualityPolicy`), recomposition/save
prompts will silently misbehave. The array data classes (`Phrase`, `Chain`, …) need custom
equals because of `Array`/`IntArray`, but `Project` has no array-identity excuse for comparing
only 2 of ~30 fields. **Fix:** delete the custom `equals`/`hashCode` on `Project` (or make it
compare all fields). At minimum add a `// WARNING: identity by name+tempo only` comment.

### 1.3 🟠 Hex-format / signed-decode logic duplicated across layers
`id.toString(16).padStart(2,'0').uppercase()` is inlined in `Table.name`, `Instrument.name`,
and `Project.instruments` init; `VolumeUtils.formatHex()` is a 4th copy of the same; the UI layer
has yet another (`toHex2()` in `EditorHelpers`). Core can't depend on the UI helper, but it could
host one `fun Int.toHex2()` in `core/data` and have both `VolumeUtils` and the UI re-use it.
Low priority but it's the kind of duplication that drifts.

### 1.4 🟠 Stale comments contradict the data model
- `PhraseStep.instrument` (L61) and `Instrument.id` (L373) say `// 00-7F`, but there are **256**
  instruments (`Array(256)`), i.e. 00-FF. Misleads anyone wiring bounds checks.
- `Project.reverbWet` (L528) comment says `// 00-FF dry/wet mix`, but dev-status says WET was
  removed and this is now **return gain** (the `delayWet` comment on L535 correctly says so).

### 1.5 🟡 `Note.toFrequency()` uses `java.lang.Math.pow` while the rest of the file uses `kotlin.math`
L45 `Math.pow(...)` vs `kotlin.math.cos/sin/PI` in `VolumeUtils`. Harmless, but for the planned
Linux/portable build prefer `kotlin.math.pow` consistently (and it avoids a `Double` boxing detour).

### 1.6 🟡 (robustness) No array-size validation after `decodeFromString`
`FileController.loadProject/loadTemplate/loadInstrumentPreset` deserialize with
`ignoreUnknownKeys=true` but never check that `phrases.size==256`, `tracks.size==8`,
`eqPresets.size==128`, etc. A truncated/hand-edited/corrupt `.ptp` (e.g. `"tracks":[]`) decodes
fine, then the first `project.tracks[7]` access crashes deep in playback with an opaque
`IndexOutOfBounds`. **Fix:** a small `normalizeProject()` in `migrateProject()` that pads/truncates
arrays to expected sizes and logs a warning — turns a hard crash into a recoverable load.

### 1.7 🔵 `loadProject(filename)`, `loadProject(fileInfo)`, `loadTemplate()` duplicate decode+migrate+log
Three copies of `readFile → decodeFromString → migrateProject → log → LoadResult.Success`
(FileController L66-159). Extract `private fun decodeAndMigrate(json: String, label: String): LoadResult`.
Minor, but it's where load-time validation (1.6) should live so it applies to all entry points.

### 1.8 💡 Migration `row.volume == 0xFF → -1` is lossy
`migrateProject` (L208) rewrites **every** table row whose volume is exactly 0xFF to "empty".
A v0 project where the user deliberately set a row to max (FF) loses that on load. Acceptable
given the default changed, but worth a one-line note in the migration log / release notes.

## Stage 2 — Input system

Files: `input/ButtonHandlers.kt`, `input/AppInputDispatcher.kt`, `core/logic/InputController.kt`, `input/CursorContext.kt`

### 2.1 🔴 Divergent modal-guard chains let buttons act "behind" a dialog
Each top-level handler re-implements its own guard chain for open dialogs/overlays, and they
**disagree on which dialogs they guard**:
- `handleButtonA` (L949): guards qwerty, clean, newProject, instrType, sampleEditor-confirm, eq, theme
- `handleButtonB` (L1673): guards qwerty, clean, newProject, instrType, theme (no top-level eq)
- `handleSelect` (L1735): guards **only** qwerty, theme, eq — **not** clean/newProject/instrType

Concrete bug: with the CLEAN-confirm / NEW-PROJECT / INSTR-TYPE dialog open, pressing **SELECT**
falls through to the active screen's select handler — e.g. on SONG it calls `clearSongChainRef(...)`,
a destructive edit performed invisibly behind the modal. **Fix:** one source of truth, e.g.
`private fun consumeOverlayButton(btn): Boolean` (or an `activeOverlay` enum) that every handler
calls first and returns early on. Removes ~40 lines of duplication and the whole class of "I forgot
to guard dialog X in handler Y" bugs. **High value — directly user-facing.**

### 2.2 🔵 God-methods: `handleButtonA` ≈720 lines, `handleGenericInput` ≈300, `handleStart` ≈217
`handleButtonA` (949-1673) is one nested `when(currentScreen)` with all dialog handling, file-browser
loading, per-screen edit logic inline. It's the hardest file in the project to navigate. The module
system already has the right seam — modules own `getCursorContext()`/`handleInput()`. Consider giving
modules an `onConfirm()/onPrimary()` hook so each screen's A-button logic lives with that screen,
leaving the dispatcher to handle only routing + global dialogs. Big readability win for "future you."

### 2.3 🔵 ~180 lines of dead commented-out code in `ButtonHandlers.kt`
L757-938 are commented-out usage/test examples that reference a stale constructor
(`InputMapper(handlers, logInput = true)` — the real param is `logger: ILogger?`). Misleading to a
new contributor who copies it. Move to `docs/input-system.md` or delete.

### 2.4 🟡 Dead combos silently swallow input
`handleButtonAction` returns on L+R+SELECT, L+R+A, L+R+B (L507-519), L+START (L573), R+A, R+B,
R+START (L630-643) with only a TODO — the combo is consumed but does nothing. Either implement,
or remove the branch so the buttons keep their normal single-press meaning. Minor, but a user who
discovers "R+A does nothing" can't tell if it's broken or unbound.

### 2.5 🟠 `InputController` increment/decrement duplicate the same 10-case `when` twice
`incrementValue` (L233) and `decrementValue` (L265) are mirror images with an identical case list;
only `+step`/`-step` and the wrap direction differ. Could be one `stepValue(current, signedStep, ctx)`.
Also `CursorContext.isEmpty()` (L127) duplicates `capabilities.isEmpty` — its own docstring admits two
sources of truth. Pick one. Low priority.

## Stage 3 — Logic controllers

Files: `PlaybackController.kt`, `EffectProcessor.kt`, `ClipboardManager.kt`, `InstrumentController.kt`,
`TrackerController.kt`, `RenderController.kt`. (Render scheduling delegates to `scheduleStepWithEffects`,
so realtime/offline share one path — good, no duplication there. `core/` has **zero** Android imports —
portability rule holds.)

### 3.1 🔴 Retrigger volume-ramp delta table is duplicated and can drift
The 16-entry ramp table exists twice in `PlaybackController.kt`:
- L805-806 (compact, used to preserve ramp volume across RPT→RPT transitions)
- L1356-1373 (verbose w/ comments, used to actually apply the retrig ramp)

They're identical today, but they're two hand-maintained copies of the same magic numbers. Edit one
(say, to make decreases gentler) and RPT-to-RPT carryover silently disagrees with the live ramp →
audible volume glitch that's painful to trace. **Fix:** one `companion object` constant
`val REPEAT_RAMP_DELTAS = floatArrayOf(...)` referenced from both sites.

### 3.2 🔵 God-method `scheduleStepWithEffects` (~810 lines, L769-1579)
Single function handling REPEAT/ARP cancellation, CHA gating, RND/RNL, effect resolution, note
scheduling, arpeggio, retrigger. Organised with `// STEP N` banners (which the project's own style
guide says were removed elsewhere — see dev-status "Module Code Style Unification"). Inconsistent with
the rest of the codebase and very hard to modify safely. Candidate for extraction into
`applyChanceGate()`, `applyRandomize()`, `applyRetrigger()`, `applyArpeggio()` helpers.

### 3.3 🟠 `ClipboardManager`: `cutXxx` re-implements `deleteXxx` instead of composing
`cutPhraseSteps` (L537) does `copyPhraseSteps(...)` then inlines the exact per-column clearing loop that
`deletePhraseSteps` (L699) already implements. Same for Chain/Song/Table — 4 duplicated clear-loops.
`cut = copy + delete` would remove ~80 lines and guarantee cut and delete always clear identically
(right now a change to "what empty means" must be made in 8 places, not 4). The 16 parallel
copy/paste/cut/delete methods are inherent to 4 screens × 4 ops, but the cut/delete overlap is pure waste.

### 3.4 🟡 Verbose per-step logging always builds strings during playback
`EffectProcessor.resolveStepParams` and `scheduleStepWithEffects` call `logger.d(TAG, "…$x…")` for
every effect on every scheduled step (e.g. EffectProcessor L131-293, ~20 sites). The string args are
built every scheduling pass regardless of whether logcat is showing them — wasted work on the Miyoo
Flip during playback. **Fix:** gate behind a `private const val TRACE = false` and `if (TRACE) logger.d(...)`,
or give `ILogger` a lazy `d(tag) { "..." }` overload. (Mirrors the C++ side's `AUDIO_TRACE=0` flag —
the Kotlin scheduler has no equivalent.)

## Stage 4 — Audio Kotlin layer

Files: `core/audio/AudioEngine.kt`, `core/audio/IAudioBackend.kt`, `platform/android/OboeAudioBackend.kt`.
Overall: strong. `scheduleNote` slice/PIT/ARP math is careful and bounds-checked; `parseWavBuffer`
(L107-202) is genuinely robust (chunk-scans for `fmt `/`data`, handles WAVE_FORMAT_EXTENSIBLE,
16/24/32-bit PCM + float, guards zero sampleRate/bits). Few findings.

### 4.1 🔵 The JNI facade is the project's biggest boilerplate surface (3 hops, no logic)
`IAudioBackend` has **101** methods, `OboeAudioBackend` has **101** matching `external fun`s, and
`jni-bridge.cpp` has **115** `Java_com_…` functions. On top of that, ~50 `AudioEngine` methods
(L757-848) are pure 1-line forwards `fun x(...) = backend.x(...)` adding a *third* hop with zero logic
(mostly sample-editor ops). The interface is justified for portability, but the `AudioEngine` pass-through
layer for sample-editor calls buys nothing — UI could call `backend` directly for those, or they could be
grouped behind one `ISampleEditorBackend` to shrink the surface. Not urgent; just the place where adding
one engine feature means touching 4 files.

### 4.2 🟡 Hot-path string logging in `scheduleNote`
L656 `logger.d(TAG, "📋 scheduleNote: … vol=${"%.4f".format(volume)} …")` runs and formats a string for
**every scheduled note** regardless of log visibility — `%.4f` formatting on the Miyoo Flip per note.
Same fix as 3.4 (gate behind a trace flag).

### 4.3 🟡 Magic C-4 frequency `261.63f` and the framesPerTic formula are duplicated
`261.63f` appears as the base-frequency fallback and slice-mode recompute (AudioEngine L658, L694, L192)
— hoist to `const val C4_HZ`. `framesPerTic = sampleRate / (tempo/60f * 4f * TICS_PER_STEP)` is computed
identically in the SF path (L600) and sampler path (L723) — extract a tiny helper. Also `Math.pow` (L693)
vs `kotlin.math` — same consistency point as 1.5.

## Stage 5 — C++ audio engine

Files: `audio-engine.h/.cpp`, `sample-editor.cpp`, `jni-bridge.cpp`, `effects/*`, `mods/*`.
The DSP is well-organised (unified `processAudioBlock`, per-module headers, try-lock discipline on
`sampleEditMutex` to avoid use-after-free during buffer swaps, no heap allocation in the mix loop).

### 5.1 🔴 Destructive sample edits corrupt/crash STEREO samples (right channel not maintained)
The engine stores stereo as `samples[id]` (L) + `samplesRight[id]` (R) but tracks **one** length,
`sampleLengths[id]`, and the mix loop reads BOTH channels at the same `idx`
(`audio-engine.cpp` L1123-1124: `sampleData[idx]` / `sampleDataRight[idx]`, plus `idx+1`).

Almost every op in `sample-editor.cpp` mutates **only the left buffer**:
- Length-changing (`cropSample` L221, `deleteSampleRegion` L236, `pasteRegion` L266, `downsampleSample`
  L286, `pitchShiftSample` L340, `applyRateMode` L300) resize `samples[id]` and update `sampleLengths[id]`
  but leave `samplesRight[id]` at its **old** allocation/length.
- In-place (`normalizeSample`, `fadeIn/Out`, `silenceRegion`, `reverseSample` L124-171) edit left only.
- `backupSample`/`undoSample` (L173-193) back up/restore left only.
- Only `timeStretchSample` (L375) correctly stretches `samplesRight` too.

Consequences on a SOURCE=STEREO sample:
- **Growth ops** (paste, pitch-DOWN → `newLen = oldLen/ratio` with ratio<1) make left longer than the
  right allocation → `sampleDataRight[idx]` reads **out of bounds** → garbage audio or **SIGSEGV**.
- **Shrink ops** (crop/delete/downsample/pitch-up) → right is stale/longer → plays the *un-edited* right
  channel against the edited left → broken stereo image.
- **In-place + undo** → L and R desync.

This is a shipped feature (true-stereo editing, dev-status 2026-05-16). **Fix options:** (a) make every
length/content op apply the same transform to `samplesRight[id]` when non-null (as `timeStretchSample`
already does), and back it up in `backupSample`/`undoSample`; or (b) if the editor is meant to be mono-only,
collapse to mono on edit (drop `samplesRight[id]`) so the stereo read path is never entered with a stale R.
Option (a) preserves the feature. **Highest-priority bug in this review.**

### 5.2 🟠 `volumeMutex` locked per-sample inside the inner mix loop
`processAudioBlock` takes `std::lock_guard<std::mutex>(volumeMutex)` (L1105-1109) **once per sample per
voice** just to read `trackVolumes[]`/`masterVolume` (plain floats). At 44.1 kHz × up to 8 voices that's
~350k lock/unlock per second per voice — pure overhead, and a genuine real-time hazard: if the Kotlin
thread is inside `setTrackVolume`/`setMasterVolume` holding the lock, the audio callback blocks → dropout.
**Fix:** snapshot the 8 track volumes + master once per block (before the voice loop), or make them
`std::atomic<float>` and read lock-free. Removes the mutex from the hottest path entirely.

### 5.3 🔵 Wide JNI bridge is mechanical boilerplate (ties to 4.1)
`jni-bridge.cpp` has 115 `Java_com_…` thunks, each a hand-written marshal to one engine method. Inherent
to JNI, but combined with 4.1 it means each new engine call = edits in jni-bridge.cpp + OboeAudioBackend +
IAudioBackend + AudioEngine.kt. Worth a one-paragraph "how to add an engine call" note in
`technical-architecture.md` so the four-file ritual is documented rather than rediscovered.

### 5.4 ℹ️ Already-tracked debt confirmed
The duplicated sampler-vs-SF table-advance loops and the LFO `sinf` hot-path cost are already listed in
dev-status "Architecture Debt (Post-MVP)" — confirmed still present, no need to re-file. The `sinf`-per-block
point pairs naturally with 5.2 as "hot-loop cost reduction" if you do a perf pass for the Miyoo Flip.

## Stage 6 — UI rendering + modules

Files: `ui/PixelPerfectRenderer.kt`, `ui/EditorHelpers.kt`, `ui/modules/*`.
**Positive:** the "Module Code Style Unification" the dev-status claims is real — grep finds **zero**
residual `.toString(16).padStart(...)` or old per-module theme-var patterns in `ui/`; everything uses
`toHex2()`/`rowBgColor()`/`val t = state.appTheme`. `EditorHelpers` is a clean shared home. The
`PixelPerfectRenderer.drawLayout` per-screen `when` (L639-885) is a reasonable central dispatch.

### 6.1 🟠 (portability) `core/logic` depends on the `ui` package
`PlaybackController.kt` (L13) does `import com.conanizer.pockettracker.ui.getEffectTypeName` and uses it
at L929/L978-979. That's a **core → ui** edge — exactly what the architecture rules forbid for the planned
Linux port. It passed the "no `import android…`" check because it's a same-language cross-package import,
not an Android one. The earlier "core has zero Android imports" result (Stage 3) is still true, but this
is the same class of leak one layer over. **Fix:** move `getEffectTypeName` into `EffectProcessor`
(core) — see 6.2, which solves both at once.

### 6.2 🟠 Effect code↔name is a second source of truth with hardcoded hex
`getEffectTypeName` (`EditorHelpers.kt` L86-111) maps `0x03 -> "ARC"`, `0x04 -> "CHA"`, … using **literal
hex**, parallel to `EffectProcessor`'s `FX_ARC = 0x03` / `EFFECT_TYPES` list. Adding or renumbering an
effect now requires editing two unrelated files, and the literals here can silently drift from the `FX_*`
constants. **Fix:** put the name map in `EffectProcessor` keyed off the `FX_*` constants (e.g.
`fun nameOf(code): String` or a `FX_NAMES: Map<Int,String>`), and have UI + PlaybackController call it.
Resolves 6.1 too.

### 6.3 🟠 FX-slot ↔ `fxNType/fxNValue` boilerplate scattered across ≥5 files
The `when (slot) { 1 -> step.fx1Type to step.fx1Value; 2 -> …; 3 -> … }` shape (and its setter twin)
is hand-written in `EffectProcessor.resolveStepParams`, `PlaybackController.scheduleStepWithEffects`
(CHA/RND loops), `ClipboardManager`, `EditorHelpers.clearEffect`, and `CursorContext`. It exists because
`PhraseStep` exposes 3 flat pairs instead of an indexable structure. **Fix:** give `PhraseStep` either
`fun fx(slot): Pair<Int,Int>` + `fun setFx(slot, type, value)` accessors, or model FX as
`val fx: Array<FxSlot>` (size 3). Collapses ~6 duplicated `when` blocks and removes a whole category of
"I updated slot 1 and 2 but forgot 3" bugs. (`TableRow` has the same 3-pair shape and would benefit too.)

### 6.4 🔵 Minor: stale doc strings
`CursorContext` enum comment (L47) still lists the old effect set "(---, ARP, KIL, OFF, RPT, VOL)";
`hexNibble` docstring (CursorContext L328) cites BPM as a use case though BPM is 3-digit. Cosmetic.

## Stage 7 — Cross-cutting: structure, build, security, memory

**Positives:** `.cxx/` and `build/` are gitignored and untracked (build artifacts don't pollute history);
only **13** TODO/FIXME markers across all source (healthy); **no hardcoded secrets** — the GitHub crash-report
PAT is read from gitignored `local.properties` → `BuildConfig`, **release builds inject an empty token**
(`app/build.gradle` L76), and `GitHubIssueSender` no-ops on a blank token. Security posture here is good.

### 7.1 🟡 (security) Debug APK embeds the GitHub PAT as a plaintext `BuildConfig` string
`buildConfigField("String", "GITHUB_TOKEN", "\"$githubToken\"")` (debug, L73) bakes the token into the
dex; anyone with a debug APK can `strings` it out. Fine for solo dev, but if you ever share a debug build,
that PAT can be lifted and used to spam issues. **Mitigation:** scope the PAT to issues-only on the single
repo, and treat debug APKs as semi-sensitive (don't post them publicly).

### 7.2 🔵 Eight files exceed 1000 lines; the real problem is god-methods, not file size
`AppInputDispatcher.kt` (2713), `audio-engine.cpp` (2064), `PlaybackController.kt` (1748),
`PixelPerfectRenderer.kt` (1493), `AudioEngine.kt` (1192), `jni-bridge.cpp` (1092),
`TrackerController.kt` (1062), `MainActivity.kt` (1024). File size alone is acceptable for a tracker, but
the three worst (`handleButtonA` ~720, `scheduleStepWithEffects` ~810, plus the JNI surface) concentrate
risk. Addressing 2.2 and 3.2 shrinks the two scariest.

### 7.3 🟠 Recurring theme — "indexed slot" knowledge is copy-pasted instead of centralized
Several findings are the same root cause: a small indexed concept gets hand-expanded everywhere instead of
living in one place. Worth fixing as a group because each is cheap and removes a bug class:
- **FX slots 1/2/3** → 6.3 (`PhraseStep`/`TableRow` accessors)
- **Transpose byte → semitones** → 1.1 (one shared decoder)
- **Retrigger ramp deltas** → 3.1 (one constant)
- **Effect code → name** → 6.2 (one map in core)
- **"empty value" per column** → spread across ClipboardManager cut/delete + EditorHelpers (3.3)

---

## Executive Summary & Suggested Priority

**Overall:** mature, thoughtfully-architected codebase. Portability discipline is largely real, the audio
engine is carefully written, and the recent style-unification work genuinely landed. The issues below are
refinements, not signs of trouble. Nothing here blocks the app from working today.

### Fix before release (correctness, user-facing)
1. **5.1 🔴 Stereo sample edits corrupt/crash the right channel** — the one finding that can SIGSEGV or
   produce broken audio on a shipped feature. Highest priority.
2. **2.1 🔴 SELECT (and other buttons) act behind open dialogs** — destructive edits invisibly applied;
   centralize the modal-guard chain.
3. **1.1 🔴 Transpose decoders disagree at 0x80** — table vs chain/project transpose differ by 256 semitones.
4. **3.1 🔴 Duplicated retrigger ramp table** — silent audio glitch waiting to happen on any future edit.

### Robustness / latent
5. **1.2** `Project.equals` compares only name+tempo (latent recomposition/dirty-check trap).
6. **1.6** validate array sizes after project load (turn corrupt-file crashes into graceful failures).
7. **5.2 🟠 per-sample `volumeMutex` in the mix loop** — real-time dropout hazard + waste; snapshot per block.

### Consistency / structure (future-you & contributors)
8. **6.1/6.2** move `getEffectTypeName` into core (fixes a core→ui layering leak + a 2nd source of truth).
9. **6.3** `PhraseStep`/`TableRow` FX-slot accessors (kills the most-duplicated pattern).
10. **3.3** `cut = copy + delete` in `ClipboardManager`.
11. **2.2 / 3.2** decompose `handleButtonA` and `scheduleStepWithEffects`.

### Performance (Miyoo Flip)
12. **3.4 / 4.2** gate per-step/per-note string logging behind a trace flag (mirror the C++ `AUDIO_TRACE`).
13. **5.2 + dust/sinf** if you do a perf pass, the hot-loop mutex and `sinf` LFO are the measurable wins.

### Low / cosmetic
14. **2.3** delete ~180 lines of dead example code in `ButtonHandlers.kt`.
15. **2.4** implement-or-remove the dead TODO combos.
16. **1.4 / 6.4** stale comments (instrument 00-7F vs 00-FF, reverbWet "dry/wet", CursorContext effect list).
17. **7.1** scope the debug PAT minimally.

---

## Fix Log

Scope chosen by developer: **correctness + safe cleanups first**, grouped ~3-4 per batch. Large refactors
(2.2/3.2 god-methods, 6.3 FX-slot model) deferred. ⬜ = not started · 🔧 = code changed, awaiting device test · ✅ = tested.

### Batch A — Stage 1 (TrackerData.kt, FileController.kt) — 🔧 awaiting test
- **1.1 🔧** Added `byteToSignedSemitones(b)` (two's-complement, 0x80 = −128) in `TrackerData.kt`;
  `Chain.getTransposeSemitones`, `Project.getTransposeSemitones`, `TableRow.transposeToSemitones` now all
  delegate to it. ⚠️ Behaviour change: a transpose byte of exactly `0x80` now means −128 (was +128 for
  chain/project). Only that single extreme value changes; normal ±-small transposes are identical. Verify
  chain/project transpose still sounds right on a test song.
- **1.2 🔧** Removed `Project.equals`/`hashCode` (was name+tempo only); now uses data-class default. Comment added.
- **1.3 🔧** Added core `Int.toHex2()`; `Table.name`/`Instrument.name`/`Project.instruments`/`VolumeUtils.formatHex`
  now use it (4 inline copies → 1). UI's `EditorHelpers.toHex2` left as-is (core can't depend on ui).
- **1.4 🔧** Fixed stale comments: `PhraseStep.instrument` & `Instrument.id` 00-7F → 00-FF; `reverbWet` "dry/wet" → "return gain".
- **1.5 🔧** `Note.toFrequency()` now uses `kotlin.math.pow` (added import) instead of `java.lang.Math.pow`.
- **1.6 🔧** Added `validateProjectStructure()` — load now rejects wrong-sized arrays with a clear message
  instead of crashing later in playback.
- **1.7 🔧** Extracted `decodeAndMigrate(json, label)`; `loadProject`×2 + `loadTemplate` share it (validation lives here).
- **1.8** Left as-is (migration already logs the 0xFF→-1 count); release-note item only.

**Test focus for Batch A:** load an existing project + template (still loads), save/reload round-trip,
chain & project transpose behave, instrument names still show INSTxx, table names TBLxx.

### Batch B — Stage 3 (PlaybackController.kt, EffectProcessor.kt, ClipboardManager.kt) — 🔧 awaiting test
- **3.1 🔧** Added `PlaybackController.REPEAT_RAMP_DELTAS` companion constant; both former inline copies
  (RPT→RPT carry-over + live retrig ramp) now reference it. No behaviour change (values identical).
- **3.4 🔧** Added `EffectProcessor.TRACE = false`; every `logger.d` in `resolveStepParams` is now
  `if (TRACE) logger.d(...)`, so per-step effect log strings aren't built in shipped builds. No behaviour change.
- **3.3 🔧** `cutPhraseSteps/cutChainRows/cutSongCells/cutTableRows` now do `copy + deleteXxx(...)` instead
  of re-implementing the clearing loop (~80 lines removed). Cut and delete now provably clear identically;
  item counts come from the delete result.

**Test focus for Batch B:** RPT (Rxy) retrigger still ramps volume up/down as before, including across
consecutive RPT steps; copy/cut/paste/delete on PHRASE/CHAIN/SONG/TABLE in selection mode (L+B → B copy,
L+A cut, L+A paste, A+B delete) still works and cut clears the right cells.

### Batch C — Stage 6 (EffectProcessor.kt, EditorHelpers.kt, PlaybackController.kt, CursorContext.kt) — 🔧 awaiting test
- **6.2 🔧** Added `EffectProcessor.FX_NAMES` map + `effectName(code)` in core, keyed off the `FX_*`
  constants (single source of truth for code→name). `EditorHelpers.getEffectTypeName()` now delegates to it.
- **6.1 🔧** `PlaybackController` no longer imports `getEffectTypeName` from `ui` — uses
  `EffectProcessor.effectName()` instead. **core/logic → ui dependency removed** (portability rule restored).
- **6.4 🔧** Fixed stale `CursorContext` comments (effect-set list → "see EFFECT_TYPES"; hexNibble BPM mention).

**Test focus for Batch C:** PHRASE/TABLE FX columns still show the correct 3-letter effect names
(ARP/RPT/PIT/etc.); no functional change expected.

### Batch D — Stage 4 (AudioEngine.kt) — 🔧 awaiting test
- **4.3 🔧** Added `AudioEngine.C4_HZ` const; replaced all 9 `261.63f` literals. Added private
  `framesPerTicAt(tempo)`; the 3 duplicated framesPerTic computations (SF + sampler + preview paths)
  now call it. Replaced all 5 `Math.pow(2.0, …/12)` detune calcs with `kotlin.math.pow`. No behaviour change.
- **4.2 🔧** Gated the per-note `scheduleNote` log behind `AudioEngine.TRACE = false`.

**Test focus for Batch D:** sample playback pitch is unchanged (notes in tune, detune/ROOT correct);
pitch effects (PSL/PBN/PVB) still behave; SF and sampler instruments both play correctly.

### Batch E — Stage 2 (AppInputDispatcher.kt, ButtonHandlers.kt) — 🔧 awaiting test
- **2.1 🔧** Added `confirmDialogOpen()` (CLEAN / NEW PROJECT / INSTR TYPE) guard; `handleSelect` and
  `handleStart` now return early when one is open, so SELECT can no longer clear a chain ref (etc.) and
  START can't start playback behind the dialog. (A/B still confirm/cancel via handleButtonA/B as before;
  they close the dialog before any combo can form, so other entry points don't need the guard.)
- **2.3 🔧** Removed ~183 lines of dead commented-out example/test code at the end of `ButtonHandlers.kt`
  (truncated after `inputHandler`, LF preserved, no BOM); fixed the garbled doc comment above it.
- **2.5 / 2.4 ⏭️ Deferred:** increment/decrement unification touches the core value-edit path for marginal
  gain (regression risk); the dead TODO combos (R+A, L+START, …) are intentional placeholders for planned
  features. Left as-is.

**Test focus for Batch E:** open CLEAN / NEW PROJECT / INSTR-TYPE dialog, press SELECT and START — nothing
should happen behind the dialog; A confirms, B cancels as before. Normal SELECT (clear cell) and START
(play/stop) still work when no dialog is open. Keyboard/gamepad input unaffected by the ButtonHandlers trim.

### Batch F — Stage 5 C++ (audio-engine.h/.cpp, sample-editor.cpp) — 🔧 awaiting test
- **5.1 🔧 (full stereo, the critical bug)** Every destructive sample-editor op now maintains the right
  channel in lockstep with the left, so SOURCE=STEREO samples are edited properly (no more stale R / OOB
  crash). Specifics:
  - New right-channel buffers: `sampleBackupsRight[256]` (undo), `originalSamplesRight[256]` (RATE cache),
    `sampleClipboardRight` (copy/paste), `fxPreviewBackupRight` (FX preview). Init in ctor, freed in dtor /
    `clearAllSamples` / on reload, mirroring the left buffers.
  - New private helper `setSampleBuffers(id, newL, newR, newLen)` swaps both buffers + the shared length
    atomically — all length-changing ops (crop/delete/paste/downsample/pitchShift/applyRateMode/undo/
    timeStretch) route through it, so L/R length can never desync.
  - In-place ops (normalize/fadeIn/fadeOut/silence/reverse) now transform the right region too;
    normalize peaks across both channels to preserve the stereo image.
  - `copyRegion`/`pasteRegion` carry a stereo clipboard (mono clip pasted into a stereo sample is
    duplicated to centre it). `backupSample`/`undoSample` and the FX-preview backup save/restore both
    channels. `applySampleFx` refactored to a per-channel lambda applied to L and R independently.
- **5.2 🔧** `processAudioBlock` snapshots the 8 track volumes + master once per block under a single
  `volumeMutex` lock; the four former per-sample/per-voice/per-trigger lock sites now read the lock-free
  snapshot. Removes the mutex from the hot mix loop (~350k locks/s/voice) and the dropout hazard.

**Test focus for Batch F (most important — load a STEREO wav, SOURCE=STEREO):** crop, delete, copy→paste,
normalize, fade in/out, silence, reverse, downsample (RATE LOFI↔NORM↔HIGH), pitch shift, time-stretch, and
the OTT/DUST/DRIVE/EQ offline FX — after each, playback should be clean stereo (no crash, no garbled/with R
channel wrong), and UNDO should restore both channels. Then repeat on a MONO sample (must behave exactly as
before). Also: live mixer track/master volume changes during playback still take effect smoothly.

---

## Round 2 — post-test fixes (developer reported after testing batches A–F)

Batches A–F confirmed working on device. Follow-up issues found during testing, all now 🔧 awaiting re-test:

### R2.1 🔧 SF pitch: ROOT was inverted vs sampler; detune had no effect at all
`AudioEngine.scheduleNote` SF path:
- **ROOT** transpose was `root.toMidi() - 60` (lower ROOT → lower pitch) — opposite of the sampler.
  Changed to `60 - root.toMidi()` so lower ROOT raises pitch on both. (C-4 neutral.)
- **Detune** was never applied to SF (sampler bakes it into baseFreq; SF is MIDI-based). Added a
  fractional detune path: new `detuneSemitones` field on `SoundfontVoice` (folded into `pitchMod`,
  survives PSL/PBN), threaded through `ScheduledNote` → C++ `scheduleSoundfontNote` → JNI →
  `OboeAudioBackend`/`IAudioBackend` → `AudioEngine.kt` (same hex encoding as sampler, ±8 st, PITCH_RANGE=48).
  **Test:** SF instrument ROOT direction now matches sampler; nudging DETUNE on an SF instrument bends pitch.

### R2.2 🔧 SELECT no longer deletes the value under the cursor
`handleSelect` cleared the cell on SONG/CHAIN/PHRASE — deleting is A+B / selection-delete only.
Those three cases are now no-ops (SELECT stays free for context actions). Other screens' SELECT
context actions (PROJECT name, EQ editors, EFFECTS toggles, sample/instrument name) unchanged.
**Test:** SELECT on a phrase note/value does nothing; A+B still deletes; SELECT+A paste still works.

### R2.3 🔧 R+DPAD now blocked while a confirm dialog is open
The 2.1 guard covered SELECT/START but not R-navigation, so you could leave the screen with a
CLEAN/NEW-PROJECT/INSTR-TYPE dialog still up. Added `confirmDialogOpen()` early-return to
`handleRUp/RDown/RLeft/RRight`. **Test:** with a confirm dialog open, R+DPAD does nothing.

### R2.4 🔧 Sample-editor selection markers after length-changing ops
- **SYNC (RPITCH + TSTRETCH)** is a whole-sample op; it now resets the selection to the full new
  sample (`0..newLen`) instead of proportionally scaling the old selection (which left the end marker
  at a confusing spot).
- **UNDO** resets the selection to the full restored sample (`0..newLen`); the slice marker is clamped.
  (Originally clamped the selection, but that left a partial selection after undoing a length-shrinking
  op such as a SYNC that shortened the sample — undo after SYNC kept the synced end marker instead of
  going full. Reset is consistent across all undo cases.)
  Forward crop/delete/paste already reset via `afterResize()` — unchanged.

**Files touched (Round 2):** `AudioEngine.kt`, `AppInputDispatcher.kt`, `IAudioBackend.kt`,
`OboeAudioBackend.kt`, `audio-engine.h/.cpp`, `soundfont-voice.h/.cpp`, `note-queue.h`, `jni-bridge.cpp`.

## Round 3 — follow-up after Round 2 testing (SF pitch still off + SF/sample resource leak)

### R3.1 🔧 SF detune was silently wiped every block
The Round-2 detune threading was correct, but `SoundfontVoice::applyPitchMod` (soundfont-voice.cpp L147)
early-returns and re-centres the TSF pitch wheel whenever there's no slide/vibrato/PITCH-mod — it didn't
know about `detuneSemitones`, so a static detune was reset to centre every callback. Added
`&& detuneSemitones == 0.0f` to that guard so a non-zero detune reaches the wheel. **Now SF DETUNE works
(phrase + preview).**

### R3.2 🔧 SF instrument preview ignored ROOT (always played C-4)
`previewInstrument` auditions the instrument by passing `note = instrument.root`. With the Round-2 phrase
transpose `midiNote = baseMidi + (60 - root)`, that collapses to a fixed 60 (C-4) — so ROOT had no audible
effect in the instrument-screen preview (it did work on the phrase screen). Added an `isRootAudition`
flag to `AudioEngine.scheduleNote`; the SF path skips the root transpose when set, so the preview plays the
root note itself (and DETUNE applies). Phrase/note-preview unchanged.
**Correction (post-test):** the instrument-screen preview actually runs through
`InstrumentController.previewInstrument` (and TABLE/MODS through `AudioEngine.previewInstrument`), not the
one spot first patched — so the flag was added to `InstrumentController.previewInstrument`'s SF
`scheduleNote` call too. Both SF preview paths now pass `isRootAudition = true`. (Kotlin-only.)

### R3.3 🔧 Sample editor reachable from a SoundFont instrument
Instrument row 3 / col 3 ("EDIT", already hidden for SF in the renderer) still opened the sample editor on
A-press regardless of type. Gated the open on `instrumentType != SOUNDFONT` so SF can't reach it.
**Cursor reachability (follow-up):** `TrackerController.getInstrumentCursorRightColumn` row 3 now caps at
col 2 for SF (was col 3), so DPAD-RIGHT from the SF LOAD button no longer moves onto the hidden EDIT
column (which made the cursor disappear). Sampler still reaches col 3 (EDIT).

### R3.4 🔧 WAV sample stayed in memory after switching a slot to SoundFont
`setInstrumentType(→SOUNDFONT)` nulled `sampleFilePath` but never freed the C++ sample buffers, so the
WAV data lingered (wasteful on the 1GB Miyoo Flip). Added `clearSample(id)` through the full stack
(`AudioEngine` C++ frees samples/right/backups/originals for the slot → JNI → `OboeAudioBackend` →
`IAudioBackend` → `AudioEngine.kt` also drops cached base-freq/ratio metadata); `setInstrumentType` calls
it when switching to SoundFont.

**Test (Round 3):** SF instrument — DETUNE audibly bends pitch (phrase + the root preview), ROOT changes
the preview pitch (matching the sampler direction). On an SF instrument, the EDIT button does nothing / is
absent. Switch a sampler with a big WAV loaded to SoundFont → memory drops (sample freed); switching back
shows an empty sample slot (expected — the WAV was discarded, as `sampleFilePath` was already cleared).

**Files touched (Round 3):** `soundfont-voice.cpp`, `AudioEngine.kt`, `AppInputDispatcher.kt`,
`InstrumentController.kt`, `audio-engine.h/.cpp`, `jni-bridge.cpp`, `OboeAudioBackend.kt`, `IAudioBackend.kt`.

### R3.5 🔧 Sampler DETUNE direction was inverted on playback (vs preview + SF)
After R3.1/R3.2, detune matched between sample and SF in the instrument **preview**, but on phrase
**playback** the sampler bent pitch the opposite way to SF. Cause: the preview puts detune in the target
frequency (`targetFreq = rootFreq × detuneMult` → sharper = higher), but playback baked it into the base
frequency as `rootFreq × detuneMult × ratio`, and since `rate = noteFreq / baseFreq`, a sharper detune
*raised* baseFreq and *lowered* the pitch. Changed both sampler-playback base-frequency computations
(`calculateInstrumentBaseFrequency` and the slice-mode `effectiveBaseFreq`) to **divide** by
`detuneMultiplier`, so sharper detune raises pitch — matching the preview and SF. Default detune (0x80 →
×1.0) is unchanged, so existing projects are unaffected. **Kotlin-only.**

**Test (R3.5):** on a sampler instrument, raising DETUNE above 0x80 in a phrase now bends the note **up**
(same direction as the preview and as SF), and below 0x80 bends **down**.

## Round 4 — deferred refactors

### 6.3 ✅ FX-slot accessors (done)
`PhraseStep` gained `fx(slot)` / `fxType(slot)` / `setFx(slot,type,value)` / `setFxValue(slot,value)`.
All hand-expanded `when(slot)`/`when(col)`/`when(target)` blocks over the flat `fxNType/fxNValue` fields
are gone — replaced across `EffectProcessor.resolveStepParams`, `PlaybackController` (CHA/RND/RNL reads &
writes, the REPEAT/ARP cancellation column checks, and the RND per-column FX memory loop),
`EditorHelpers.clearEffect` (now a one-liner), and `TrackerController` cleanup. Serialized fields stay flat
(no save-format change). **Kotlin-only.** Test: CHA/RND/RNL effects + RPT/ARP cancellation still behave;
copy/clear FX still works.

### 3.2 ◐ Partial god-method extraction (done what's clean)
Extracted the CHA + RND/RNL preprocessing out of `scheduleStepWithEffects` into a focused
`applyChanceAndRandomize(step, trackState): Pair<PhraseStep, Boolean>` (returns the modified step +
skipNote). `scheduleStepWithEffects` shrank ~110 lines; the gate/randomize logic is now a named unit.
Pure mechanical move (logic unchanged). **Kotlin-only.** Test: CHA gating, RND recall, RNL left/FX1
note+instrument randomization unchanged.

### 2.5 ✅ InputController increment/decrement unified (done)
`incrementValue`/`decrementValue` (identical case lists differing only by sign) collapsed into one
`stepValue(current, signedStep, context)`; callers pass `±smallStep`/`±largeStep`. Behaviour-preserving
(verified per value type: CHARACTER wrap incl. not-found first/last, numeric wrap, coerceIn). **Kotlin-only.**

### 2.2 / rest of 3.2 — NOT done (deferred per developer; need more tests)
`handleButtonA` (~720 lines) and the `scheduleStepWithEffects` retrigger block (STEP 3, ~165 lines, real-time
+ side-effecting) are the two true god-methods left. Readability-only, in the two most critical paths, no
automated tests, 150-250-line bodies — extracting via blind text edits is fragile/high-risk. Best done
incrementally in an IDE. Developer chose to skip `handleButtonA` ("needs more tests"). 2.4 (dead combos) also
left (intentional placeholders).

---

## Round 4 Test List (uncommitted refactors — all behaviour-preserving, Kotlin-only)

These are pure refactors: the goal is **no behaviour change**. Regression-test the affected features.

**6.3 + applyChanceAndRandomize (effect engine):**
- [ ] Phrase with **CHA** gate: `CHA F0` always plays the note; `CHA 00` never; `CHA 80` ~50%; `CHA 82`
      gates only FX2 (note still plays).
- [ ] **RND**: a column that had a real FX on a previous step, then `RND xy` on a later step → recalls that
      FX with a randomized value in range x0..yF.
- [ ] **RNL**: `RNL` in FX2/FX3 randomizes the value of the column to its left; `RNL` in FX1 with a note
      randomizes note ±x and instrument ±y.
- [ ] **RPT/ARP cancellation**: a persistent RPT (or ARP) is cancelled when any FX is placed in the same
      FX column on a later step (and by a new note / KIL).
- [ ] FX columns still display the correct 3-letter names (ARP/RPT/PIT/etc.).
- [ ] **CLEAN unused** on Project screen still keeps tables/grooves referenced via `TBL`/`GRV` effects.
- [ ] Copy / clear a FX cell (A+B delete, selection delete) still clears the right slot.

**2.5 (value editing — test on several screens):**
- [ ] **A + UP/DOWN** (small step) and **A + LEFT/RIGHT** (large step) edit values on PHRASE
      (note ±1 / ±octave, instrument, volume, FX type cycles, FX value ±1/±16).
- [ ] **Note** wraps/clamps at C-0 / B-9 as before; **transpose** steps by 1 / 12 and wraps.
- [ ] **Hex byte** (e.g. chain/phrase ref, instrument) wraps 00↔FF the same as before.
- [ ] **Toggles** (loop off/fwd/png, boolean settings) cycle correctly both directions.
- [ ] **Project name / sample name** character editing (A+up/down cycles A→…→Z→0-9→_→-→space) both
      directions, including from an unset/space char.
- [ ] Key-repeat while holding A+DPAD still steps continuously.

If all of the above behave exactly as before the refactor, the change is good to commit.

---

## Round 5 — post-test fixes (from Round 4 device testing)

Round 4 testing passed except two items; both turned out to be **pre-existing bugs surfaced by the
test list**, not regressions from the refactor.

### CHA 00 silenced — 🔧 awaiting re-test (tested OK by developer)
`PlaybackController.applyChanceAndRandomize` gated the chance check on `fxType == FX_CHA && fxValue > 0`,
so `CHA 00` (probability 0 = never) was skipped entirely and the note always played. Dropped the
`&& fxValue > 0` guard — `CHA 00` now gates correctly (roll < 0 → note skipped). Other CHA values
(F0/80/82) unchanged; empty FX slots are `FX_NONE` so unaffected. (The `fxValue > 0` line was unchanged
context in the 3.2 extraction diff, confirming it pre-dated the refactor.) **Developer confirmed working.**

### Note octave range C--1..B-8 → C-0..G-9 — 🔧 awaiting test
The note cursor edited midi 0..119, which under the C-4=middle-C convention (`Note.fromMidi`:
`octave = midi/12 - 1`) displayed as **C--1..B-8** (ugly double-dash negative octave). MIDI-number→
note-name is a convention choice; the app uses scientific notation (midi 60 = C-4, so midi 0 = C--1 is
genuinely correct, just ugly). Developer chose to **keep C-4 = middle C** and hide the bottom octave by
limiting the editable range, capping the top at the real MIDI ceiling (127 = G-9) — rather than
relabelling (which would have made midi 0 = C-0 but middle C = C-5).
- `CursorContext.note()` range `0..119` → `12..127` (C-0..G-9).
- `Note.fromMidi` guard `midi > 119` → `midi > 127` (standard MIDI range) so G-9 round-trips.
- `PlaybackController` RNL note clamp and `AppInputDispatcher` sample-pitch ROOT clamp `0..119` → `0..127`.
- Fixed the stale `Note.pitch` comment (it's a 0-11 chromatic index, never 0-119).

**No saved-project impact:** notes store their `octave`/`pitch` fields and `toMidi()` is unchanged, so
existing notes play identically. Only caveat: you can no longer *dial in* new notes below C-0 (old
C--1..B-1 sub-bass; any already present still play). Nothing exceeds MIDI 127 now.

**Test focus (Round 5):** in PHRASE, scroll a note all the way down → bottoms out at **C-0** (not C--1);
all the way up → **G-9** (not B-8); C-4 still sounds like middle C; INSTRUMENT ROOT edits over the same
C-0..G-9 range; load an existing project → all notes sound exactly as before.

