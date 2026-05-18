# Refactoring Fix Plan
**Generated from:** Full codebase audit (May 2026)  
**Audit algorithm:** `docs/refactoring-audit-algorithm.md`  
**Status:** Ready to execute

---

## Findings Summary

Six confirmed issues. No crash bugs or audio-output bugs. All items are architectural/portability concerns that must be resolved before a Linux port is possible, except item 6 which is cosmetic.

---

## Items — Prioritized

### ITEM 1 — Divergence time bomb: `TICS_PER_STEP` defined in two places
**Priority:** HIGH (divergence — will cause silent audio bug if timing resolution ever changes)  
**Files:**
- `core/logic/PlaybackController.kt:273` — `const val TICS_PER_STEP = 12` (authoritative)
- `core/audio/AudioEngine.kt:1196` — `val ticsPerStep = 12.0f` (independent local, NOT referencing the const)

**Problem:** `AudioEngine.pushInstrumentModulation()` computes how many audio frames correspond to one modulation tic. It uses `12.0f` hardcoded. `PlaybackController` also uses `TICS_PER_STEP = 12` for note scheduling. If `TICS_PER_STEP` were ever changed (e.g., to 16 for finer groove), modulation envelope timing (AHD attack/decay lengths) would immediately be wrong relative to note timing — envelopes would be too long/short — but only playback scheduling would be correct.

**Fix:**  
In `AudioEngine.kt:1196`, replace:
```kotlin
val ticsPerStep = 12.0f
```
with:
```kotlin
val ticsPerStep = PlaybackController.TICS_PER_STEP.toFloat()
```
This requires adding the import for `PlaybackController` to `AudioEngine.kt`. `TICS_PER_STEP` is a `companion object const`, so there is no circular dependency.

**Test:** Compile only. No behavior change at current value of 12.

---

### ITEM 2 — Bypassed abstraction: `android.util.Log` in `core/audio/AudioEngine.kt`
**Priority:** HIGH (portability blocker for Linux port)  
**File:** `core/audio/AudioEngine.kt`  
**Lines with violations:**
- Line 539: `android.util.Log.w("AudioEngine", "❌ Invalid instrumentId=...")`
- Line 614: `android.util.Log.d("AudioEngine", "📋 scheduleNote: ...")`
- Line 937: `android.util.Log.d(TAG, "📋 Loading table ...")`
- Line 974: `android.util.Log.d(TAG, "🔄 Invalidated table ...")`
- Line 1026: `android.util.Log.w("AudioEngine", "❌ Invalid instrumentId=...")`
- Line 1055: `android.util.Log.d("AudioEngine", "📋 scheduleNoteWithTable: ...")`

**Problem:** `ILogger` interface exists in `core/logging/ILogger.kt` and is already injected into every other `core/logic/` class. `AudioEngine` bypasses it with 6 direct Android SDK calls, making it impossible to compile `AudioEngine` outside Android.

**Fix:**
1. Add `private val logger: ILogger` as a constructor parameter to `AudioEngine`.
2. Add `import com.conanizer.pockettracker.core.logging.ILogger` to `AudioEngine.kt`.
3. Replace all 6 `android.util.Log.d/w(...)` calls with `logger.d/w(...)`.
4. In `MainActivity.kt` where `AudioEngine` is constructed, pass the existing `logger` instance.

**Test:** Compile only. No behavior change.

---

### ITEM 3 — Bypassed abstraction: `android.util.Log` in `core/logic/RenderController.kt`
**Priority:** HIGH (portability blocker)  
**File:** `core/logic/RenderController.kt:3`  
```kotlin
import android.util.Log
```
Used at lines 72, 112, 157, 162.

**Problem:** Same issue as Item 2. `RenderController` is in `core/logic/` and already receives `ILogger` via injection in every peer class. These `Log.d/e` calls are the only Android dependency in the file.

**Fix:**
1. Add `private val logger: ILogger` as a constructor parameter to `RenderController`.
2. Remove `import android.util.Log`.
3. Replace `Log.d(TAG, ...)` and `Log.e(TAG, ...)` with `logger.d(TAG, ...)` and `logger.e(TAG, ...)`.
4. In `MainActivity.kt` where `RenderController` is constructed, pass the existing `logger` instance.

**Test:** Compile only. No behavior change.

---

### ITEM 4 — Wrong dependency direction + misplaced file: `FileManager.kt`
**Priority:** HIGH (architectural — `core/logic/` imports root UI package)  
**Files involved:**
- `com.conanizer.pockettracker.FileManager` — root package (wrong location)
- `core/logic/FileController.kt:4` — `import com.conanizer.pockettracker.FileManager`

**Problem:** `FileController` lives in `core/logic/` but imports `FileManager` from the root package (the same package as `MainActivity`, `PhraseEditorModule`, etc.). This means core logic depends on the root UI package — a violation of the layering rule. `FileManager` itself is also a portability problem: it uses `android.util.Log` directly instead of `ILogger`.

`FileManager` is a thin 247-line wrapper that only delegates every call to `IFileSystem` plus does JSON serialization. There is no reason for it to exist as a separate class — `FileController` can own these responsibilities directly.

**Fix plan (must be done in order — each step is verifiable):**

**Step 4a — Copy `FileManager`'s JSON logic into `FileController`**
- Move the `Json` serializer setup, `saveProject`, `loadProject`, `listProjects`, `deleteProject`, `saveInstrumentPreset`, `loadInstrumentPreset`, `renameFile`, `createFolder`, `deleteFileOrFolder`, `hasStoragePermission`, `getProjectsDirectory`, `getSamplesDirectory`, `getInstrumentsDirectory`, `getSoundfontsDirectory`, `sortFiles` methods into `FileController`.
- `FileController` already has `ILogger logger` — replace all `Log.d/e(...)` calls with `logger.d/e(...)`.
- Change `FileController`'s constructor to accept `IFileSystem` directly (removing the `FileManager` dependency).
- Keep `FileManager` in place and unchanged while doing this step.

**Step 4b — Update `MainActivity.kt`**
- Find where `FileManager` is constructed (line ~287): `val fileManager = remember { FileManager(fileSystem) }`
- Find where `FileController` is constructed: `FileController(fileManager, logger)`
- Change to: `FileController(fileSystem, logger)`
- Remove the `FileManager` construction line.

**Step 4c — Delete `FileManager.kt`**
- Verify nothing imports `FileManager` (should be zero after 4b).
- Delete the file.

**Test:** After 4c, full project load/save/delete cycle must work. Export WAV must work. File browser must work.

---

### ITEM 5 — Wrong placement: Compose types in `core/ui/DeviceTheme.kt`
**Priority:** MEDIUM (portability — blocks Linux renderer from using `DeviceTheme`)  
**File:** `core/ui/DeviceTheme.kt:10-12`
```kotlin
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.text.font.FontFamily
```

**Problem:** `DeviceTheme` is a data class in `core/ui/` that holds the visual theme (colors, fonts, background images). Its fields use `Color`, `ImageBitmap`, and `FontFamily` — all Compose types. A Linux renderer would use SDL, OpenGL, or similar and cannot use these types. If the Linux port ever needs theming, it would need to duplicate or rewrite `DeviceTheme`.

**Fix (two options — pick one):**

**Option A (simpler):** Move `DeviceTheme.kt` to the root package. It's not core business logic — it's a rendering concern. Remove it from `core/`.

**Option B (cleaner for Linux port):** Split into two:
- `core/ui/CoreTheme.kt` — theme as plain data: `Int` colors (ARGB), font name strings, image path strings. No Compose imports.
- Root package `ComposeTheme.kt` — converts `CoreTheme` → `DeviceTheme` with actual Compose types. Loaded only on Android.

Option A is the right choice for now (MVP). Option B is the right choice if a Linux port is actively planned.

**Test:** Compile only. No behavior change.

---

### ITEM 6 — Stale comment: `MAX_VOICES = 8` in `audio-defs.h`
**Priority:** LOW (cosmetic / potential config error)  
**File:** `app/src/main/cpp/audio-defs.h:18`
```cpp
const int MAX_VOICES = 8;  // Reduced for testing
```

**Problem:** The comment "Reduced for testing" implies this was temporarily lowered from a higher value and never restored. Unclear if 8 is the intended production polyphony. The app currently ships with 8-voice polyphony.

**Action needed:** Decide if 8 is correct for production (it matches M8's 8-track polyphony model). If yes, update the comment:
```cpp
const int MAX_VOICES = 8;  // One voice per track (8 tracks)
```
If it was supposed to be higher, restore and test.

**Test:** If value changes — full playback test with dense phrases to verify no voice-stealing artifacts.

---

### ITEM 7 — Missing feature: Table playback row tracking (not a bug, future work)
**Priority:** LOW / POST-MVP  
**File:** `MainActivity.kt:1351`
```kotlin
playbackRow = null,  // TODO: Table playback row tracking
```

During playback, the TABLE screen does not scroll/highlight the currently-executing table row. This means a user watching the TABLE screen during playback sees a static cursor, not a moving playback cursor (unlike PHRASE/CHAIN/SONG screens which all have live playback row tracking).

**Fix:** Add `currentTablePlaybackRow: Int?` to `PlaybackController`'s state output. Expose it via `TrackerController`. Wire it into `TableModule` state.

---

## Execution Order

```
1 → trivial one-liner, do first (resolves divergence time bomb)
2 → adds ILogger to AudioEngine constructor (touch constructor + 6 call sites)
3 → adds ILogger to RenderController constructor (touch constructor + 4 call sites)
4a → copy logic into FileController, update constructor signature
4b → update MainActivity construction call
4c → delete FileManager.kt
5 → move DeviceTheme (Option A: relocate file, fix imports)
6 → comment-only change
7 → post-MVP
```

Items 1–3 are independent and can be done in any order.  
Items 4a → 4b → 4c must be done in sequence.  
Item 5 is independent.

---

## Files Modified Per Item

| Item | Files Modified |
|------|---------------|
| 1 | `core/audio/AudioEngine.kt` |
| 2 | `core/audio/AudioEngine.kt`, `MainActivity.kt` |
| 3 | `core/logic/RenderController.kt`, `MainActivity.kt` |
| 4 | `core/logic/FileController.kt`, `MainActivity.kt`, DELETE `FileManager.kt` |
| 5 | `core/ui/DeviceTheme.kt` → move to root package; update all importers |
| 6 | `cpp/audio-defs.h` |
| 7 | `PlaybackController.kt`, `TrackerController.kt`, `TableModule.kt`, `MainActivity.kt` |

---

**Version:** 1.0  
**Last Updated:** 2026-05-18
