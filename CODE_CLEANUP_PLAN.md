# Pre-MVP Code Cleanup Plan

**Written:** February 2026
**Scope:** Fix real defects and structural debt before MVP release. No new features, no logic changes.
**Based on:** Deep inspection of ButtonHandlers.kt, InputController.kt, CursorContext.kt, EditorHelpers.kt, FileManager.kt, FileController.kt, RenderController.kt, EffectProcessor.kt, DeviceAdapter.kt, TrackerData.kt.

---

## Summary of Findings

| # | Finding | Severity |
|---|---------|----------|
| 1 | `CursorContext.kt` in root package, imported by `core/logic/InputController.kt` — circular dependency | Structural |
| 2 | `RenderController.kt` has `import android.util.Log` | Portability violation |
| 3 | `FileManager.kt` has `import android.util.Log` | Portability violation |
| 4 | `RenderController.kt` redefines all `FX_*` constants and reimplements `parseStepEffects()` — already exist in `EffectProcessor` | Duplication |
| 5 | `EditorHelpers.clearEffect()` / `clearChainSlot()` / `clearSongChainRef()` directly mutate `var` fields on data classes — Compose won't recompose | Real bug |
| 6 | `FileController.kt` (core/logic) wraps `FileManager.kt` (root) which wraps `IFileSystem` — two redundant layers, wrong dependency direction | Redundancy |
| 7 | `ButtonHandlers.kt` mixes abstract types (no Android deps) with platform types (Compose/Android deps) in one file | Misplacement |
| 8 | `DeviceAdapter.kt` in root package despite being Android-specific | Misplacement |
| 9 | `getEffectTypeName()` in EditorHelpers is a display-name mapping for effect codes — belongs in `EffectProcessor` | Split concern |

---

## Layer 1 — Break the Circular Dependency

**Priority:** Do first. Everything else is easier after this.

**Finding:**
`core/logic/InputController.kt` imports `CursorContext` from the root package. A `core/` file importing from the Android UI root package is a dependency inversion — core should never depend upward.

**Root cause:**
`CursorContext.kt` was never moved to `core/data/` during refactoring. It has zero Android imports and only imports `EffectProcessor` from `core/logic/`. It belongs in `core/data/`.

**Files affected:**
- Move: `CursorContext.kt` → `core/data/CursorContext.kt`
- Contents moved: `CursorValueType`, `CursorCapabilities`, `CursorContext`, `CursorContextFactory`
- Update imports in every file that references these types

**Risk:** Low — mechanical import update, no logic change.
**Test:** Compile only.

---

## Layer 2 — Fix Portability Violations

**Priority:** Quick wins, unblocks Layer 3.

### 2a — RenderController.kt

**File:** `core/logic/RenderController.kt`, line 3
**Problem:** `import android.util.Log` breaks Linux portability.
**Fix:** Add `private val logger: ILogger` to the constructor. Replace all `Log.*` calls with `logger.d()` / `logger.e()`. `ILogger` already exists at `core/logging/ILogger.kt`.

### 2b — FileManager.kt

**File:** `FileManager.kt`, line 3
**Problem:** Same `import android.util.Log` violation. Every `Log.d` / `Log.e` call in `saveProject()`, `loadProject()`, `deleteProject()` needs to go through `ILogger`.
**Fix:** Same as above — add `ILogger` to constructor, replace calls.
**Note:** This also prepares FileManager for the merge in Layer 5.

**Risk:** Very low.
**Test:** Compile. Verify save/load and WAV export still work.

---

## Layer 3 — Eliminate RenderController Duplication

**Priority:** Highest impact. Removes a maintenance bomb.

**Finding:**
`RenderController.kt` lines 37–44 redefines the exact same `FX_*` constants that live in `EffectProcessor.companion`. The comment in the code literally says `// (must match EffectProcessor)`. Every time a new effect is added to `EffectProcessor`, `RenderController` must be manually updated or it silently breaks WAV export.

Additionally:
- `RenderController.RenderEffects` (lines 50–58) is a parallel data struct to `EffectProcessor.ResolvedStepParams` — same fields, different names
- `RenderController.parseStepEffects()` (line 91+) reimplements the same FX-slot loop that `EffectProcessor.resolveStepParams()` already handles

**Actions:**

1. **Delete the `FX_*` constant block** in `RenderController` (lines 37–44). Replace all references with `EffectProcessor.FX_*`.

2. **Delete `private data class RenderEffects`**. Replace usages with `ResolvedStepParams` from `EffectProcessor`, or map from it if the field shapes differ meaningfully.

3. **Delete `parseStepEffects()`**. Inject `EffectProcessor` as a constructor dependency in `RenderController` and call `resolveStepParams()` instead.

**Result:** ~80 lines deleted from `RenderController`. The render path becomes a consumer of `EffectProcessor` rather than a copy of it. New effects added to `EffectProcessor` automatically appear in WAV export.

**Risk:** Medium — touches the render path.
**Test:** WAV export produces identical audio output before and after. Compare waveforms on a test project.

---

## Layer 4 — Fix the Mutation Bugs in EditorHelpers.kt

**Priority:** This is an actual observable bug.

**Finding:**
`EditorHelpers.clearEffect()` does this:
```kotlin
step.fx1Type = 0x00  // Direct mutation — Compose won't recompose
step.fx1Value = 0x00
```

`PhraseStep` is a `data class` with `var` fields (confirmed in `TrackerData.kt` lines 53–61). When Compose holds a reference to a `Project` via `mutableStateOf`, mutating a nested `PhraseStep`'s fields in place does not change the `Project` reference — so Compose sees no state change and does not recompose.

**Symptom:** Pressing A+B to clear an effect appears to do nothing visually until something else happens to trigger a recompose. The data change is real but the UI is stale.

Same problem in:
- `clearChainSlot()` — mutates `chain.phraseRefs[row]` and `chain.transposeValues[row]`
- `clearSongChainRef()` — mutates `track.chainRefs[row]`

This is exactly the anti-pattern documented in `CLAUDE.md` under "Common Pitfalls → Direct Object Mutation."

**Actions:**

1. Convert `clearEffect()` to return a new `PhraseStep`:
   ```kotlin
   fun clearEffect(step: PhraseStep, fxSlot: Int): PhraseStep = when (fxSlot) {
       1 -> step.copy(fx1Type = 0x00, fx1Value = 0x00)
       2 -> step.copy(fx2Type = 0x00, fx2Value = 0x00)
       3 -> step.copy(fx3Type = 0x00, fx3Value = 0x00)
       else -> step
   }
   ```
   Update all callers to use the returned value.

2. Same conversion for `clearChainSlot()` and `clearSongChainRef()`.

3. **Move `getEffectTypeName()` to `EffectProcessor.companion`.**
   It maps effect hex codes to display names (`0x0A → "ARP"`, etc.). The hex codes themselves already live in `EffectProcessor` — the name mapping is the other half of the same concern. Having it in `EditorHelpers` means two files must be kept in sync when a new effect is added. After moving, `EffectProcessor` becomes the single source of truth for everything about effects.

4. Move `EditorHelpers.kt` → `core/logic/EditorHelpers.kt` (it has no Android imports and will have none after this change).

**Risk:** Medium — all callers of the three `clear*` functions need updating.
**Test:** Clear an effect cell with A+B. The cell must immediately display `---` without any other interaction. Also test clear chain slot and clear song row.

---

## Layer 5 — Collapse FileManager into FileController

**Priority:** Removes an unnecessary abstraction layer and fixes a wrong-direction import.

**Finding:**
`FileController.kt` (`core/logic/`) imports `FileManager` from the root package:
```
core/logic/FileController → root/FileManager → core/storage/IFileSystem
```
A core file importing from the root package is wrong direction. More importantly, neither class contains significant logic — `FileManager` is JSON serialization + `IFileSystem` calls, and `FileController` is `FileManager` calls + `SaveResult`/`LoadResult` sealed classes.

**Actions:**

1. Move the JSON serialization logic (`json = Json { ... }`, `encodeToString`, `decodeFromString`) from `FileManager` into `FileController`.
2. Change `FileController`'s constructor to take `IFileSystem` directly (remove `FileManager` dependency).
3. Absorb the file operation methods (`saveProject`, `loadProject`, `deleteProject`, `renameFile`, `createFolder`, `deleteFileOrFolder`, `listProjects`) directly into `FileController`.
4. Delete `FileManager.kt`.
5. Update all callers that currently construct or reference `FileManager`.

**Risk:** Medium — callers that bypass `FileController` and talk to `FileManager` directly need updating.
**Test:** Full cycle: save project, close, load project, verify data identical. Delete project. Browse file list.

---

## Layer 6 — Split ButtonHandlers.kt Along the Platform Boundary

**Priority:** Completes the half-built input abstraction.

**Finding from reading the file:**

| Part | Android/Compose deps | Where it belongs |
|------|---------------------|-----------------|
| `ButtonHandlers` data class | None | `core/data/` |
| `VirtualButton` enum | None | `core/data/` |
| `ButtonAction` enum | None | `core/data/` |
| `InputMapper` class | `android.util.Log`, `androidx.compose.ui.input.key.*` | `platform/android/` |
| `Modifier.inputHandler()` | Compose `Modifier` | `platform/android/` |

The abstract types already have no Android deps — they need to be moved, not changed. `InputMapper` and `inputHandler()` are the only parts that are inherently Android/Compose.

`InputController.kt` (`core/logic/`) already handles the high-level combo routing (what `L+A` means, what `A+UP` means). `InputMapper` handles the low-level translation from Android `KeyEvent`s to `VirtualButton` events. The split formalizes this existing boundary.

**Actions:**

1. Move `ButtonHandlers`, `VirtualButton`, `ButtonAction` → `core/data/InputTypes.kt`
2. Move `InputMapper` and `Modifier.inputHandler()` → `platform/android/AndroidInputHandler.kt`
3. Delete `ButtonHandlers.kt`
4. Update all imports

**Why this matters beyond cleanup:**
Once `InputMapper` is in `platform/android/`, writing a Linux port means writing `LinuxInputHandler.kt` — the rest of the input system is untouched.

**Risk:** Medium-high — the input system is touched by every screen. Do this in one focused session.
**Test:** Every button combination on a real device. Specifically: DPAD, A, B, START, SELECT, L, R, A+DPAD (all 4 directions), A+B, B+LEFT/RIGHT, L+A, L+B, L+R, R+DPAD (all 4 directions), SELECT+A, SELECT+B.

---

## Layer 7 — Move DeviceAdapter to platform/android/

**Priority:** Trivial, do last.

**Finding:**
`DeviceAdapter.kt` imports `android.content.Context`, `android.content.res.Configuration`, `android.view.InputDevice`, `android.view.KeyEvent`, `android.view.WindowManager`. It is Android-specific and sits in the root package instead of `platform/android/`.

**Action:** Move file. Update imports.

**Risk:** Low.
**Test:** Compile. Virtual controls appear on touch-only devices.

---

## Deferred: PlaybackController.kt Split

`PlaybackController.kt` is 1615 lines and mixes step sequencing, chain/song advancement, and playback state management. It would benefit from being split, but it works correctly and touches the core audio scheduling path. Splitting it before MVP carries regression risk disproportionate to the benefit. This is the first task for a post-MVP cleanup session.

---

## Execution Order and Test Gates

```
Layer 1 — CursorContext move          → compile ✅
Layer 2 — Log violations              → compile + save/load + WAV export ✅
Layer 3 — RenderController dupes      → WAV export produces identical audio ✅
Layer 4 — Mutation bugs + helpers     → clear effect cell, verify immediate UI update ✅
Layer 5 — FileManager collapse        → full save/load/delete/browse cycle ✅
Layer 6 — ButtonHandlers split        → every button combo on real device ✅
Layer 7 — DeviceAdapter move          → compile + virtual controls ✅
```

**Never stack two layers in a single session without running the test gate between them.**

---

## Files Changed Summary

| File | Action |
|------|--------|
| `CursorContext.kt` | Move to `core/data/` |
| `core/logic/RenderController.kt` | Replace Log with ILogger; delete duplicate FX constants, RenderEffects struct, parseStepEffects() |
| `FileManager.kt` | Replace Log with ILogger (prep for Layer 5); then delete |
| `EditorHelpers.kt` | Fix mutation bugs; move getEffectTypeName() to EffectProcessor; move file to core/logic/ |
| `core/logic/EffectProcessor.kt` | Add getEffectTypeName() to companion object |
| `core/logic/FileController.kt` | Absorb FileManager logic; take IFileSystem directly |
| `ButtonHandlers.kt` | Delete; contents split into core/data/InputTypes.kt and platform/android/AndroidInputHandler.kt |
| `DeviceAdapter.kt` | Move to `platform/android/` |
| All callers of above | Import updates |

**New files created:** `core/data/InputTypes.kt`, `platform/android/AndroidInputHandler.kt`
**Files deleted:** `FileManager.kt`, `ButtonHandlers.kt`
