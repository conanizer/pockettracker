# PocketTracker Code Review #3 (2026-06-15)

Third full-repo pass. Reviews #1 (`REVIEW.md`) and #2 (`REVIEW-2.md`) are fully fixed/merged,
so the easy findings are gone — this round is deliberately about **fresh** issues the first two
passes didn't reach. Focus, per the developer's request: bugs, inconsistency, duplication, and
above all **memory on low-end devices (Miyoo Flip, 1 GB / 128 MB Java heap clamp)**, plus a
concrete plan for the structure work that's been deferred twice.

Findings already fixed in #1/#2, or already listed in dev-status "Architecture Debt", are NOT
re-reported except where I can add a concrete next step.

Severity legend: 🔴 bug / correctness · 🟠 inconsistency or duplication · 🟡 perf / memory · 🔵 structure/readability · 💡 idea

---

## Stage 1 — Memory & buffer lifetime (the priority)

The C++ static footprint is fine: `instrumentParams[256]`, `instrumentModSlots[256][4]`,
`tables[256]`, `eqPresets[128]`, the 4×4096-float spectrum rings and the `[9][620]` OCTA buffers
total only a few hundred KB and never grow. **All the real memory is in the per-slot sample
buffers** — and that's exactly where the lifetime discipline is incomplete.

Per slot the engine can hold up to **six** full-length float buffers: working L/R
(`samples`/`samplesRight`), undo L/R (`sampleBackups`/`sampleBackupsRight`), and the RATE-mode
HIGH cache L/R (`originalSamples`/`originalSamplesRight`) — plus the shared single `fxPreviewBackup`
and `sampleClipboard`. A 30 s stereo sample is ~10 MB working; with undo + a LOFI RATE cache that's
~30 MB resident **for one edited slot**.

### 1.1 🟡 (memory) Undo + RATE-original buffers are never freed when the sample editor closes
`backupSample(id)` (sample-editor.cpp:222) allocates a full copy of the slot before *every*
destructive op; `applyRateMode(id, factor>1)` (sample-editor.cpp:428-437) caches a full HIGH-rate
copy. Closing the editor (`AppInputDispatcher.kt:1750`, the clean-close branch) calls **only**
`audioEngine.restoreFxPreviewBackup()` — the undo cache (`sampleBackups[id]`) and the RATE cache
(`originalSamples[id]`) stay allocated for the **lifetime of the slot** (until the slot is reloaded,
switched to SF, or the app dies). Edit a handful of samples on a 1 GB device and you're carrying
several ×2-stereo backup copies that nothing can ever read again.

This is REVIEW-2's idea **7.5**, still unimplemented — but note the asymmetry that makes part of it
*free*: **undo is only reachable inside the editor**, so freeing `sampleBackups[id]` on close can
never lose anything a user could have triggered. **Fix:** a small `freeSampleEditCaches(id)` in the
engine (delete+null `sampleBackups[id]`/`sampleBackupsRight[id]`/`originalSamples[id]`/
`originalSamplesRight[id]`, zero their lengths) called on **both** editor-exit paths — the clean
close at `:1750` and the discard-confirm "yes" at `AppInputDispatcher.kt:994`. The RATE cache has one
subtlety: after freeing it, a later "RATE → HIGH" would need to reload the WAV from disk
(`sampleFilePath` is still on the instrument) instead of restoring losslessly from RAM — an
acceptable trade for ~⅓ of the per-edited-slot memory, but call it out so the reload path gets wired.

### 1.2 🔴 (bug + leak) `loadSample` / `loadSampleStereo` free the RATE cache but not the undo cache on slot reuse
`loadSample` (audio-engine.cpp:155-169) and `loadSampleStereo` (:183-196) both free
`samples`/`samplesRight`/`originalSamples`/`originalSamplesRight` before installing the new buffer —
but **neither touches `sampleBackups[id]` / `sampleBackupsRight[id]` / `sampleBackupLengths[id]`**.
`clearSample` (:206-224) frees all six correctly; the two load paths are the odd ones out. Two
consequences:

- **Stale-undo data corruption (🔴):** load a new WAV into a slot that was edited in an earlier
  session (its undo cache still resident — see 1.1), open the editor, press **UNDO before making any
  edit** → `undoSample` (sample-editor.cpp:237) sees a non-null `sampleBackups[id]` and copies the
  *previous* sample's data, at the *previous* length, over the freshly-loaded sample. Reachable
  precisely because backups outlive both editor-close (1.1) and slot-reload.
- **Leak (🟡):** until the next `backupSample`/`clearSample`, the old backup is dead weight.

**Fix:** mirror the `originalSamples` handling already sitting three lines above — free + null
`sampleBackups[id]` / `sampleBackupsRight[id]` and zero `sampleBackupLengths[id]` in both load
functions. (1.1 makes this rarer by freeing on close; 1.2 is the belt-and-braces that also fixes the
correctness case.)

### 1.3 🟡 (memory/size) Project / preset / theme files are written with `prettyPrint = true`
`FileController.json` (FileController.kt:27-30) sets `prettyPrint = true` for `.ptp`, `.pti` and
`.ptt`. These are machine-read formats over the fixed 256-slot model (256 phrases × 16 steps, 256
chains, 256 tables × 16 rows, 256 grooves, 128 EQ presets). `encodeDefaults` is left at its `false`
default so all-default objects already serialize compactly — but pretty-printing still puts every one
of those *thousands* of array elements on its own indented line, inflating both the on-disk file and,
more importantly on a 128 MB-clamped heap, the **transient `String`** held during `encodeToString`
(save) and `readFile`→`decodeFromString` (load). **Fix:** `prettyPrint = false` for the project/
preset/theme `Json` (or gate it behind a debug flag if you ever want to eyeball a file). Files still
load unchanged (whitespace-insensitive, `ignoreUnknownKeys`). Smaller saves, less GC churn at load.

---

## Stage 2 — Correctness

### 2.1 🔴 (bug) The resample slot-allocator can clobber a configured SoundFont instrument
`InstrumentController.createResampledInstrument` finds its target slot with
`project.instruments.indexOfFirst { it.sampleFilePath == null }` (InstrumentController.kt:540). But a
configured **SoundFont** instrument *also* has `sampleFilePath == null` — `setInstrumentType(SOUNDFONT)`
explicitly nulls it (InstrumentController.kt:759). So the "first empty slot" search can land on a slot
that's a working SF instrument, load a WAV into it, and set `sampleFilePath`/`sampleId`/root/vol/pan
(:554-559) **without** resetting `instrumentType` back to `SAMPLER` or clearing `soundfontPath` → a
broken hybrid: a slot flagged `SOUNDFONT` with a sample buffer behind it. Very reachable — e.g.
instrument `00` is an SF and the user resamples a selection; `00` is the first `sampleFilePath == null`
slot, so the resample silently overwrites their SoundFont.

**Root cause** is 2.3 below: `sampleFilePath == null` means two different things. **Fix:** search with
a real "free slot" predicate — `instrumentType == SAMPLER && sampleFilePath == null && soundfontPath == null`
— and, defensively, reset `instrumentType = SAMPLER` / `soundfontPath = null` when claiming the slot.

### 2.2 🟠 (inconsistency / bug) `renderSelectionToWav` ignores the DUST master bus
`renderSongToWav` correctly branches on the master-bus FX selector (RenderController.kt:118-122):
OTT *or* DUST + limiter. `renderSelectionToWav` (the selection-resample path) hardcodes
`setOttDepthForRender(project.ottDepth)` (RenderController.kt:192) and never calls `setMasterFx` /
`setDustDepthForRender`. So for a project whose master bus is set to **DUST**, a selection resample
bakes in OTT (or nothing) instead — the resampled audio doesn't match playback. **Fix:** use the same
`if (masterBusFx == 0) … else …` block as `renderSongToWav` (a 4-line copy, or factor it into a shared
`applyMasterBusForRender(project)` helper — see 3.1).

---

## Stage 3 — Duplication & consistency

### 3.1 🟠 (duplication) The song→chain→phrase→step walk is hand-rolled in 8 files
The nested traversal `track.chainRefs[row] → project.chains[chainId].phraseRefs[slot] →
project.phrases[phraseId].steps`, with its bounds guards (`in 0..255`, `row < chainRefs.size`), is
re-implemented across **PlaybackController, RenderController (three times: `setupInstrumentParams`,
the stems used-instrument scan, the active-tracks filter), AppInputDispatcher, InstrumentController,
ClipboardManager, EditorHelpers, SongEditorModule**. Each copy re-derives the same bounds checks —
the kind of thing where one site forgets `row < chainRefs.size` and crashes only on a particular
song. **Fix:** a tiny portable `core/logic/SongTraversal.kt` (or `Project` extensions):
`Project.forEachStepInRange(startRow, endRow) { trackId, row, slot, step -> … }`,
`Project.collectUsedInstruments(startRow, endRow): Set<Int>`, and
`Project.collectUsedTablesAndGrooves(): …`. Collapses ~6 copies and puts the bounds logic in one
tested place. (RenderController alone has three near-identical copies — `setupInstrumentParams`,
`renderStemsToWav`'s used-instr scan, and the reverb/delay `hasXSend` checks.)

### 3.2 🟠 (consistency) Dual base-frequency caches in `AudioEngine.kt` (still live)
`sampleBaseFrequencies` and `sampleRateRatios` (AudioEngine.kt:68-69) are two caches derived from one
underlying value, and every RATE / downsample op has to update **both** by hand — the comment block
at AudioEngine.kt:871-901 ("scale this, also scale that, here's why playback would otherwise be
permanently wrong") is the scar tissue of exactly that. This is the precise root cause REVIEW-2's
RATE-pitch bug (R3.1) traced to, and it was explicitly left as a post-MVP cleanup. Re-confirming it's
still present: phrase playback reads `sampleBaseFrequencies` while previews recompute from
`sampleRateRatios`, so the two can (and did) silently diverge. **Fix (post-MVP, but worth it before
adding any more pitch features):** keep only `sampleRateRatios` and compute
`baseFreq = C4_HZ × ratio × detune` at schedule time; delete `sampleBaseFrequencies`.

### 3.3 🔵 (consistency) "Empty instrument" is an overloaded, scattered predicate
`sampleFilePath == null` is used as the "empty slot" test in four places — `AudioEngine.kt:700`,
`RenderController.kt:414`, `InstrumentController.kt:302` and `:540` — but since SoundFont instruments
null it too (2.1), it actually means "empty sampler slot **or** configured SoundFont". `RenderController`
:414 even *relies* on that overload (treats `sampleFilePath == null` as "this is an SF instrument"),
which silently also catches genuinely-empty sampler slots. The dev-status / memory note that
"`sampleFilePath == null` is THE empty signal" predates SoundFont and is now incomplete. **Fix:** put
the vocabulary on the data class — `Instrument.isSoundfont()`, `Instrument.isEmptySampler()` (=
`SAMPLER && sampleFilePath == null`), `Instrument.isFree()` (= empty of *both*) — and replace the four
ad-hoc checks with the one that's actually meant at each site. Fixes 2.1 and de-fuzzes 3.3 in one go.

---

## Stage 4 — Structure / professionalism

### 4.1 🔵 `handleButtonA` (~760 lines) — a concrete, low-risk decomposition plan
`AppInputDispatcher.handleButtonA` (lines 966-~1729) is one nested `when (currentScreen)` mixing
global-dialog handling, file-browser load actions, and every screen's confirm/insert logic. It's been
deferred from both prior reviews as "too risky to text-edit blind, no tests" — correct, but it's also
the single biggest "make it look professional" item left, and the module system already has the right
seam. **Plan that needs no big-bang edit:**
1. Add an optional `fun onConfirmA(state: Any?, ctx: …): Boolean` to the `TrackerModule` interface
   (default returns `false` = "not handled, fall through"), mirroring the existing
   `getCursorContext()` / `handleInput()` hooks each module already owns.
2. Move **one** screen's A-button body from the dispatcher into its module's `onConfirmA`, returning
   `true`. Leave the dispatcher branch as `if (module.onConfirmA(...)) return`.
3. Device-test that one screen, commit, repeat. The dispatcher shrinks to routing + the global modal
   dialogs; each screen's primary-action logic lives next to that screen's draw/cursor logic.

This is incremental, each step is independently testable on the device, and it directly addresses the
"god-method" debt without the all-at-once risk that's blocked it twice. The retrigger block in
`PlaybackController.scheduleStepWithEffects` is the harder twin (real-time, side-effecting) and can
stay deferred — the input dispatcher is the higher-value, lower-risk target.

### 4.2 🔵 (cleanup) Confirmed dead code still in tree
`getActiveTrackMask()` (C++, noted as dead in the Batch-4b memory log) and `updatePlaybackParams`
(InstrumentController.kt:576, self-labelled `@deprecated`) are both unused. Low-stakes, but a release
"polish" pass is the moment to delete dead `@deprecated` helpers rather than ship them.

---

## Stage 5 — Ideas (open-ended, low priority)

### 5.1 💡 Sample-memory readout on the project screen (echo of REVIEW-2 7.5)
On a 1 GB handheld users *will* hit the wall. A single "SAMPLE RAM: 42 MB" line (sum of all
non-null `sampleLengths` ×4 bytes × channels, + backups/originals) turns a mystery OOM-kill into an
understandable limit — and makes the 1.1/1.2 wins visible. Cheap: one JNI getter summing the arrays.

### 5.2 💡 int16 storage for the undo / RATE caches (pairs with 1.1)
`sampleBackups` and `originalSamples` exist only to restore — store them as `int16_t` (half the
memory; bit-exact for the 16-bit-sourced WAVs that dominate, inaudible otherwise). Combined with 1.1
(free on close) it roughly halves again the resident cost of an editing session.

### 5.3 💡 Crash-safe autosave (echo of REVIEW-2 7.6, still not done)
ACRA catches JVM crashes but the user still loses edits (and native SIGSEGVs bypass ACRA entirely —
see REVIEW-2's note). A periodic `autosave.ptp` off the main thread when `projectVersion` changed,
plus a "recover?" prompt on launch, is cheap insurance and pairs well with release testing.

---

## Stage 6 — Round 2: device-reported bugs (2026-06-15)

Two issues the developer hit on-device after the first pass. Both root-caused; fixes applied
(awaiting device test).

### 6.1 🔴 (UI) Selection highlight freezes while expanding — a regression from REVIEW-2's 6.1 idle-redraw
**Symptom (reported):** in selection mode the on-screen selection doesn't grow as you press DPAD —
"it copies if I navigate blindly, but on screen it remains still."

**Root cause:** the single `Canvas` draw lambda (`PixelPerfectRenderer.kt:313`) reads exactly one
snapshot state — `oscilloscopeTicker`. Every other value (cursorRow, `selectionInfo`,
`isCellSelected`) is captured when the `PixelPerfectTracker` composable **recomposes**, which only
happens when one of its params changes. Expanding a selection changes **no param**: the cursor is
anchored in selection mode (REVIEW-2 R2.3), the scope label stays `"SEL:CELL"`, and `isCellSelectedFn`
is a compiler-memoized lambda with stable identity — the growing bounds live *inside* that lambda,
invisible to Compose's skip check. So the composable is skipped, the Canvas lambda isn't rebuilt, and
`oscilloscopeTicker` doesn't tick while idle (the whole point of REVIEW-2's 6.1) → no redraw. Before
6.1 the 60 fps idle ticker repainted constantly and masked this; 6.1 made idle truly idle and exposed
it. Normal cursor movement still redraws because it changes the `cursorRow` param.

**Fix (applied, `MainActivity.kt`):** key the selection lambda on the bounds —
`remember(selStart, selEnd) { { row, col -> inputController.isCellSelected(row, col) } }`. Now each
expand hands `PixelPerfectTracker` a fresh `isCellSelected` instance → param changed → recomposition →
Canvas rebuilt → redraw. Single-file; deliberately **not** `projectVersion++` (that drives the
`isProjectDirty` "unsaved changes" prompt at `AppInputDispatcher.kt:1220`, and a pure copy-select must
not mark the project modified).

### 6.2 🔴 (memory / crash) Loading a large WAV still OOM-kills — REVIEW-2 R2.1 was only a half-fix
**Symptom (reported):** loading a 35 MB stereo render OOM-crashed even on the 3 GB Ayaneo (release
build). Logcat: `Forcing collection of SoftReferences for 35MB allocation` → `OutOfMemoryError …
growth limit 100663296` (a **96 MB** heap cap), failing allocation ~37.6 MB, heap already ~77 MB.

**Root cause:** the WAV decode happens **entirely on the Java heap**, which is capped (~96 MB here)
even though the device has 3 GB. For a 35 MB stereo file, `loadWavFileFromPath` holds the whole file
as a `ByteArray` (`AudioEngine.kt:124`, ~35.9 MB, alive through decode because the `decode` closure
reads from it) **plus** `left` + `right` FloatArrays (`:230-231`, ~37.6 MB each) = **~111 MB live at
once** → over the cap. REVIEW-2 R2.1 removed only the *fourth* copy (the old full-size `rawSamples`
intermediate); the fundamental Java-heap round-trip (file bytes + both float channels) is still there.
The native engine (`loadSampleStereo`) just copies these into native memory — so the floats *need* to
exist on the Java heap only because the decode is written in Kotlin.

**Fix applied (immediate, `AndroidManifest.xml`):** `android:largeHeap="true"`. Lifts the per-process
cap well above 111 MB on the 3 GB Ayaneo (typically 256–512 MB) and meaningfully on the 1 GB Miyoo —
resolving the reported cases. Legitimate for an offline-audio app that deliberately loads multi-MB
samples (not masking a leak).

**Proper fix (APPLIED):** WAV decoding moved into C++ so sample data never touches the Java heap. New
`AudioEngine::loadSampleFromWavFile(id, path)` (`audio-engine.cpp`) `fopen`s the file, streams the data
chunk in 16k-frame blocks, and de-interleaves straight into `samples[id]`/`samplesRight[id]` in native
memory — returning the WAV sample rate (Kotlin still computes the base-frequency compensation). The
`decodeWavSample` helper mirrors `parseWavBuffer`'s `decode()` byte-for-byte (16/24/32-bit PCM +
32-bit float + WAVE_FORMAT_EXTENSIBLE) so loads are bit-identical. Wired through the 4-file JNI ritual
(`native_loadSampleFromWav` thunk → `IAudioBackend.loadSampleFromWav` → `OboeAudioBackend`); both
`AudioEngine.loadSampleFromFile` and `previewSampleFile` route through it, so every file-load caller
benefits (browser load, resample, project-load `reloadProjectSamples`, video-audio extraction). Peak
Java heap for sample data is now ~0 → fixes *any* file size, not just what largeHeap covers. As a bonus
the native loader frees the stale undo backup on slot reuse (incidentally addressing **1.2** for the
file-load path). The old Kotlin `loadWavFileFromPath`/`parseWavBuffer` are now dead (kept one release as
a reference, `@Suppress("unused")`; delete once the native path is device-confirmed). largeHeap stays
as cheap headroom. This is exactly how SF2 already loads (`tsf_load_filename`).

*(Latent aside spotted here: `loadWavFileFromPath` uses `inputStream.read(buffer)` (`AudioEngine.kt:125`),
which is not guaranteed to fill `buffer` in one call — use `readBytes()` or a read loop. Harmless for
local files today, but a correctness trap. Moot once decoding moves native.)*

---

## Executive Summary & Suggested Priority

The codebase remains in good shape — two thorough review passes have clearly landed, the audio
hot-path and sample-editor locking are solid, and nothing here is architectural. Review #3's harvest
is smaller and more specific: a couple of genuine bugs in the **less-trodden** instrument/render/
slot-management paths, the **sample-buffer lifetime** gap that's the real low-end-memory lever, and a
concrete way to finally chip at the `handleButtonA` god-method.

### Fix before release (correctness / data safety)
1. **1.2 🔴** `loadSample`/`loadSampleStereo` don't free the undo cache → stale-undo can overwrite a
   freshly-loaded sample (data corruption) + a leak. Three-line symmetry fix; mirrors `clearSample`.
2. **2.1 🔴** resample slot-allocator treats a configured SoundFont as "empty" and clobbers it.
   Fix with a real `isFree()` predicate (3.3).
3. **2.2 🟠** `renderSelectionToWav` bakes OTT even when the project's master bus is DUST.

### Memory (Miyoo Flip) — the developer's priority
4. **1.1 🟡** free the undo + RATE caches when the sample editor closes (undo half is provably
   safe; biggest per-edited-slot win). REVIEW-2's 7.5, now with the safe-scope spelled out.
5. **1.3 🟡** `prettyPrint = false` for `.ptp`/`.pti`/`.ptt` — smaller files + less transient heap
   at save/load.
6. **5.2 / 5.1 💡** int16 undo/RATE caches; a sample-RAM readout to make the wall visible.

### Structure / consistency (when convenient)
7. **3.1 🟠** one shared song-traversal helper (kills ~6 hand-rolled copies + their bounds bugs).
8. **3.3 🔵** `Instrument.isFree()/isSoundfont()/isEmptySampler()` — fixes 2.1's root cause and
   de-fuzzes four scattered checks.
9. **4.1 🔵** decompose `handleButtonA` one screen at a time via a `TrackerModule.onConfirmA` hook —
   finally tractable, each step device-testable.
10. **3.2 🟠** collapse the dual base-frequency caches (post-MVP, but before any new pitch feature).
11. **4.2 🔵** delete the confirmed dead `getActiveTrackMask` / `updatePlaybackParams`.

---

## Fix Log

⬜ = not started · 🔧 = code changed, awaiting device test · ✅ = tested

### Round 2 — device-reported bugs (2026-06-15) — 🔧 awaiting device test

- **6.1 🔧 Selection highlight repaint** (`MainActivity.kt`) — `isCellSelectedFn` now
  `remember(selStart, selEnd)`d so its identity changes on each selection expand, forcing the
  PixelPerfectTracker recomposition that the post-6.1 idle-redraw model otherwise skips.
  *Test:* L+B to select on PHRASE/CHAIN/SONG/TABLE, hold DPAD — the highlighted region must grow
  **visibly** in real time (not just when something else triggers a redraw). Copy/cut/paste/delete
  still operate on the right cells. SONG selection past row 15 still scrolls. Normal (non-selection)
  cursor movement unchanged. No extra battery cost when idle and not selecting.

- **6.2a ✅ Large-WAV OOM mitigation** (`AndroidManifest.xml`) — `android:largeHeap="true"`.
  Device-tested: 35 MB render loads on Ayaneo + Miyoo. Kept as cheap headroom alongside the real fix.

- **6.2b ✅ Native WAV decode** (`audio-engine.h/.cpp`, `jni-bridge.cpp`, `IAudioBackend.kt`,
  `OboeAudioBackend.kt`, `AudioEngine.kt`) — file loads now decode in C++; sample data never touches
  the capped Java heap, so any file size loads. `decodeWavSample` mirrors `parseWavBuffer` exactly
  (16/24/32-bit PCM + float + EXTENSIBLE); data chunk streamed in 16k-frame blocks; stale undo/rate
  caches freed on reuse (also covers **1.2** for the file-load path). Device-tested: big files load
  without OOM, all formats/paths correct. The dead Kotlin `loadWavFileFromPath`/`parseWavBuffer` (+ now
  unused `ByteBuffer`/`ByteOrder` imports) were deleted after confirmation.

### Stage 1–5 batch 1 — 2.1 + 1.1 + 3.3 (2026-06-15) — 🔧 awaiting device test

- **2.1 + 3.3 🔧 Resample no longer clobbers a SoundFont; `Instrument.isFree()/isSoundfont()` added**
  (`TrackerData.kt`, `InstrumentController.kt`, `RenderController.kt`). New `Instrument.isSoundfont()`
  and `isFree()` (= no WAV path, no SF path, type SAMPLER). `createResampledInstrument` searches with
  `isFree()` instead of the overloaded `sampleFilePath == null` (which also matches a configured
  SoundFont), and resets type/soundfontPath defensively when claiming the slot.
  `RenderController.setupInstrumentParams` now branches on `isSoundfont()` (was `sampleFilePath == null`,
  which also caught empty sampler slots). `AudioEngine` sampler-path check left as-is — it's already
  past the SF early-return, so `sampleFilePath == null` there unambiguously means empty sampler.
  *Test:* configure a SoundFont on instrument 00, make a short song, resample a selection → the resample
  lands in a *different* empty slot and 00 stays a working SoundFont (previously 00 was overwritten into
  a broken SOUNDFONT-typed slot with a sample). Normal resample into an empty project still works; SF +
  sampler render/playback unchanged.

- **1.1 🔧 Free the sample-editor undo backup on close** (`audio-engine.h`/`sample-editor.cpp`,
  `jni-bridge.cpp`, `IAudioBackend.kt`, `OboeAudioBackend.kt`, `AudioEngine.kt`, `AppInputDispatcher.kt`).
  New `freeSampleUndo(id)` frees `sampleBackups[id]` (+R) — a full-length copy made before every
  destructive edit — when the editor closes (both the clean B-exit and the discard-confirm exit; B is the
  only way out since R-nav is blocked in the editor). Undo is unreachable after close, so this is
  behavior-neutral and also eliminates a stale cross-session undo. The RATE-HIGH cache (`originalSamples`)
  is intentionally NOT freed: it's null at HIGH and needed for lossless restore at LOFI/NORM, so freeing
  it would only break RATE.
  *Test:* edit a sample (crop/normalize/etc.), close the editor → slot memory drops by ~one sample copy;
  reopen → sample intact, UNDO before any new edit does nothing (no stale restore), a fresh edit + UNDO
  still reverts. RATE HIGH↔LOFI↔NORM still restores losslessly across open/close. Edit several samples in
  a session and confirm memory doesn't climb per closed editor.

### Stage 1–5 batch 2 — 2.2 (2026-06-16) — 🔧 awaiting device test

- **2.2 🔧 Selection resample honors the DUST master bus** (`RenderController.kt`). The full-song render
  branched on `masterBusFx` (OTT *or* DUST) but `renderSelectionToWav` hardcoded
  `setOttDepthForRender(project.ottDepth)` — so resampling a selection in a DUST-master-bus project
  baked in OTT (or nothing) instead of DUST, and the resample didn't match playback. Extracted the
  shared `applyMasterBusForRender(project)` (setMasterFx + OTT/DUST-for-render + limiter) and call it
  from both render paths. Stems intentionally don't use it (they bypass the master bus via setStemsMode).
  Kotlin-only; the song-render path is behavior-identical (pure DRY).
  *Test:* set the master bus to DUST (Effects screen), make a short song, resample a selection → the
  resampled WAV should have the DUST character (matching playback), not OTT. Repeat with OTT master bus
  (unchanged). Full-song WAV MIX still correct for both OTT and DUST. Stems still render dry.

### Stage 1–5 batch 3 — 3.1 (2026-06-16) — 🔧 awaiting device test

- **3.1 🔧 Shared song-traversal helper** (new `core/logic/SongTraversal.kt`, `RenderController.kt`).
  Added `Project.forEachStepInSongRange(startRow, endRow, includeMuted) { step -> }` (the bounds-guarded
  nested walk) + `Project.collectUsedInstruments(startRow, endRow)` built on it. RenderController's two
  byte-identical "collect used instruments in range" walks (`setupInstrumentParams` + the stems
  send-routing scan) now call `collectUsedInstruments`, so the guards can't drift between copies.
  Kotlin-only, behavior-identical (same logic, just centralized; −34 lines net in RenderController).
  *Scope correction:* the finding listed ~6–8 sites, but on inspection only these two were genuinely the
  same walk. The live scheduler (`PlaybackController`) walks by playback position / HOP / checkpoints,
  and CLEAN (`TrackerController.collectUsedRefs`) spans the *whole* song and counts muted tracks as used
  — different traversals with different semantics, so folding them in would change behavior. Correctly
  left alone; the rest of the grep hits were single-cell chain/clipboard ops.
  *Test (pure refactor — any difference is a bug):* WAV MIX (full song), selection resample, and stems
  all render identically — same instruments audible, reverb/delay stems appear only when those sends are
  used, muted tracks still excluded.

### Stage 1–5 batch 4 — 4.2 (2026-06-16) — 🔧 awaiting device test

- **4.2 🔧 Deleted confirmed dead code** (`AudioEngine.kt`, `TrackerController.kt`,
  `InstrumentController.kt`, −71 lines). The finding named `getActiveTrackMask` + `updatePlaybackParams`;
  checking callers first showed the real dead set is a chain that had to go together:
  - `AudioEngine.getActiveTrackMask()` — no callers (it was Kotlin, not C++ as the finding said).
  - `TrackerController.updateInstrumentPlaybackParams(Int)` → `InstrumentController.updatePlaybackParams`
    — both dead (the TrackerController wrapper had no callers, and it was the *only* caller of the
    InstrumentController method, so deleting just the latter would have broken compilation).
  - `InstrumentController.updateParameter` + the `InstrumentParameter` enum — the rest of the same
    "Generic Update (legacy)" section, also dead (`updateParameter` had no callers; the enum was used
    only by it).
  Verified zero remaining references to any deleted symbol. Pure deletion — the live update path
  (`audioEngine.updateInstrumentPlaybackParams`, called from ~20 sites) is untouched.
  *Test (lowest-risk batch — if it compiles, nothing changed):* smoke-test instrument editing
  (ROOT/DETUNE/VOL/PAN/filter/loop/start-end update audio live), resample, project load.

### Stage 1–5 batch 5 — 4.1 (2026-06-17) — 🔧 awaiting device test

- **4.1 🔧 Decomposed the `handleButtonA` god-method** (`AppInputDispatcher.kt`). The ~740-line
  `when (currentScreen)` is now a thin dispatch table; each screen's confirm/insert body moved verbatim
  into its own `private fun handleConfirmA<Screen>()` on the same class. Pure cut-paste — behaviour
  identical, braces balanced, 8 call-sites ↔ 8 methods.
  - **Mechanism diverged from the finding's `onConfirmA`-on-`TrackerModule` proposal — deliberately.**
    Reading all 740 lines showed 7 of 8 bodies are dispatcher *orchestration* (screen transitions,
    file/audio I/O, coroutine renders, and ~10 private dialog states: `previousScreen`,
    `fileBrowserState`, `qwertyKeyboardState`, `sampleEditorState`, `instrumentFileBrowserAction`, …).
    Moving those into `ui/` modules would force a context object exposing audio+file+dialog internals to
    the ui layer — a portability regression against the Linux-port goal. Private methods on the dispatcher
    give the same readability win (router + named pieces) at zero plumbing and zero behaviour risk, and
    keep orchestration where it belongs. (Developer chose this over the hybrid split.)
  - The global modal-dialog guards (qwerty / clean / new-project / instr-type / sample-confirm-close /
    eq / theme) stay at the top of `handleButtonA`; only the per-screen `when` branches were extracted.
  - The two bare `return`s inside the SAMPLE_EDITOR body stay correct: the `when` was the last statement
    in `handleButtonA`, so returning from the extracted method is equivalent to falling off the branch.
  *Test (touch one A-action per screen):* PROJECT save / load / WAV+STEMS export / NEW / SETTINGS-enter;
  SETTINGS toggles (layout, scaling, btn-sound/vibro, theme, template); INSTRUMENT load preset/source /
  save preset / open sample-editor; SAMPLE_EDITOR crop/cut/copy/paste/delete, normalize/fade/reverse/undo,
  FX apply, SYNC pitch/stretch, name, load, save / save-as / chop; FILE_BROWSER load + delete; and
  PHRASE/CHAIN/SONG quick-insert (A on an empty cell duplicates the last-edited value).

### Stage 1–5 batch 6 — 3.2 (2026-06-17) — 🔧 awaiting device test

- **3.2 🔧 Collapsed the dual base-frequency caches** (`AudioEngine.kt`, `InstrumentController.kt`,
  `AppInputDispatcher.kt`, `technical-architecture.md`). `sampleBaseFrequencies` was a second cache
  holding `ROOT × rateRatio / detune` that every RATE / downsample / ROOT / DETUNE op had to update by
  hand alongside `sampleRateRatios` — the exact desync behind REVIEW-2's RATE-pitch bug (R3.1). Removed
  it entirely; the playback base frequency is now derived on demand from `sampleRateRatios` via the
  existing `calculateInstrumentBaseFrequency(instrument)`.
  - **Investigation first (audio hot path):** of the three cache readers, only one is live — the phrase
    `scheduleNote`. The other two (`playNote`, the Kotlin `scheduleNoteWithTable` overload) were dead
    repo-wide and were deleted. So the only behaviour-affecting line is the live reader, and it now
    computes exactly what the *synced* cache held (`ROOT × ratio / detune`) — behaviour-preserving in
    the correct case, self-healing in the stale case the finding describes.
  - Deleted the writer `updateInstrumentBaseFrequency` + its 6 call sites (5 in InstrumentController,
    1 in AppInputDispatcher), each of which already re-ran `updateInstrumentPlaybackParams` (untouched).
  - Deleted the manual base-freq scaling in `downsampleSample` / `applyRateMode` (the "scale this too or
    pitch is permanently wrong" scar tissue) — scaling the single ratio is now the whole job.
  - The finding's one-line formula `C4 × ratio × detune` was an oversimplification (drops ROOT, inverts
    detune direction); the real `ROOT × ratio / detune` already lived in `calculateInstrumentBaseFrequency`,
    so this reuses it rather than reimplementing.
  *Test (pitch is the thing to verify):* play a phrase across several instruments at different ROOT and
  DETUNE and confirm pitch matches each instrument's preview; change RATE (HIGH→lower) and confirm phrase
  playback pitch tracks it (the original R3.1 bug); resample, then save/reload a project and confirm pitch
  is unchanged. SoundFont instruments use a separate path and are unaffected.

### Stage 1–5 batch 7 — 5.2 (2026-06-17) — 🔧 awaiting device test

- **5.2 🔧 int16 storage for the undo / RATE-HIGH caches** (`audio-engine.h`, `sample-editor.cpp`;
  C++-only). The four restore-only buffers — undo `sampleBackups`(+R) and RATE-HIGH `originalSamples`(+R) —
  are now `int16_t*` instead of `float*`, halving their RAM. They never feed the mix loop (they only ever
  copy back into the working `samples[]` on undo / RATE-restore), so the working/playback path stays full
  float and is untouched. Two file-local converters in `sample-editor.cpp`: `f32ToCacheI16` clamps to
  [-1,1] then `lround(f*32768)` (so an over-unity working sample — post-normalize/gain — restores at full
  scale instead of wrapping), `cacheI16ToF32` divides by 32768 to match the WAV decoder. The round trip is
  therefore **bit-exact for the 16-bit-sourced WAVs that dominate** and an inaudible ~-96 dBFS
  requantization otherwise. Converted at every store (`backupSample`, `applyRateMode` cache-fill) and every
  restore (`undoSample`, `applyRateMode` HIGH-restore + decimation read); `audio-engine.cpp`'s 24 touch
  sites are all `delete[]`/null/length, correct unchanged on `int16_t*`. Pairs with 1.1 (undo freed on
  close) and the still-resident-at-LOFI RATE cache: an editing session's restore buffers now cost half.
  - *Note:* `fxPreviewBackup` is intentionally left float — it's a different, single-slot buffer the finding
    doesn't name, and it round-trips through live FX preview rather than pure restore.
  - **C++-only change: the Kotlin pre-commit hook does NOT compile it** — needs an NDK build + device test.
  *Test:* edit a sample then UNDO → reverts correctly; RATE HIGH→LOFI→NORM→HIGH still restores cleanly
  (16-bit WAV: bit-identical; verify no audible change). Stereo + mono samples both. Normalize to full scale
  then UNDO → no wrap/garbage. Confirm per-edited-slot memory drops vs. before for the undo + LOFI-cache case.

### Stage 5 — 5.3 Phase A — crash-recovery autosave: the write path (2026-06-17) — 🔧 awaiting device test

5.3 split into three device-testable phases: **A** the write path (this), **B** the launch recovery
prompt, **C** the onStop background flush. Phase A is Kotlin-only (the pre-commit hook compiles it).

- **5.3.A 🔧 Autosave write path** (new `core/logic/AutosaveManager.kt`; `IFileSystem`/`AndroidFileSystem`,
  `FileController`, `TrackerController`, `MainActivity`). The working project is serialized to an
  app-private `autosave.ptp` (in `filesDir`, like the existing template — never shown in the browser)
  a few seconds after the last edit.
  - **Trigger** = `LaunchedEffect(projectVersion)` in `PocketTrackerApp`. Every edit bumps
    `projectVersion`, re-keying the effect and cancelling the pending `delay(AUTOSAVE_DEBOUNCE_MS=3000)`,
    so an edit burst coalesces into one write. Guarded by `isProjectDirty` **before and after** the delay
    (a save during the window clears dirty + deletes the file but doesn't bump `projectVersion`, so the
    post-delay re-check stops it from re-creating a false-recovery file). Playback-agnostic by design.
  - **Memory/thread**: serialize runs on the main thread (the project's sole mutator → tear-free; ~550 KB
    real saves measured, ~1–4 MB transient, GC'd fast), the file write on `Dispatchers.IO`. No PCM is
    serialized (samples are native, referenced by path), so cost is independent of sample size. The audio
    callback is pure-native (no JNI upcalls) so GC can't pause it, and notes are buffered 2 phrases ahead
    — autosave can't glitch playback.
  - **Lifetime**: `FileController.clearAutosave()` is called on every clean save / load / new (the
    `savedProjectVersion = projectVersion` points), so the file exists **iff** there is unsaved work →
    its presence at next launch will drive Phase B's recovery prompt.
  - No recovery UI yet — Phase A only proves the file is written/cleared correctly and costs nothing
    audible.
  *Test:* edit something, wait ~3 s, confirm `<filesDir>/autosave.ptp` appears (adb: `run-as <pkg> ls
  files/`); keep editing → it updates but only ~3 s after you pause. Save / load / New → file is deleted.
  **Edit while a song plays on the Miyoo → zero audio dropout** (the key bar). Rapid editing doesn't
  stutter the UI. Save *during* the 3 s window → no autosave file reappears afterward.

### Stage 5 — 5.3 Phase B — crash-recovery autosave: the launch recovery prompt (2026-06-17) — 🔧 awaiting device test

- **5.3.B 🔧 Recovery prompt** (`FileController`, `TrackerController`, `AppInputDispatcher`,
  `MainActivity`, `ScreenLayouts`, `PixelPerfectRenderer`). At launch, if `autosave.ptp` still exists
  (Phase A clears it on every clean save/load/new, so its survival == an unclean prior exit), a
  `"RECOVER WORK?"` modal offers to restore it.
  - **Same overlay as "NEW PROJECT?"** — reuses `drawSimpleConfirmDialog` (dimmed screen + centered box
    + `A=YES  B=NO`), just a different title. New `showRecoveryDialog` state threaded through the exact
    `showNewProjectDialog` path (MainActivity state → `AppStateRefs` → dispatcher `by refs` + the A/B
    guards + `confirmDialogOpen()`; ScreenLayouts params → `PixelPerfectTracker` → draw).
  - **A = recover**: `TrackerController.recoverFromAutosave()` loads the autosave into the working
    project, bumps `projectVersion` but **not** `savedProjectVersion` (so it stays *dirty* → the user is
    nudged to Save it for real) and **does not** delete the file; the dispatcher then calls
    `reloadProjectSamples()` (the autosave stores sample *paths*, not PCM — same reload as a normal load).
  - **B = discard**: `clearAutosave()` deletes the file and dismisses.
  - **Launch check** = a `LaunchedEffect(audioReady)` gated on the engine being up (so A=recover can
    reload samples). Doesn't re-fire on warm resume (audioReady doesn't re-flip). The startup template
    load sets `project` without bumping `projectVersion`, so a fresh launch is *not* dirty → no spurious
    autosave and no false prompt.
  - Kotlin-only (pre-commit compiles it). Phase C (onStop flush) still pending.
  - **Two device-test fixes (round 2):**
    1. *Phantom recovery after a normal load.* The two project-load handlers bump `projectVersion++`
       after `loadProject` (to force a post-`reloadProjectSamples` redraw), which left the freshly-loaded
       project reading **dirty** — so autosave fired with no real edit and the next launch falsely
       prompted. (Pre-existing latent bug — it also caused a spurious "unsaved changes" prompt on NEW
       after a load — that Phase A exposed.) Fix: new `TrackerController.markProjectClean()` called after
       each load's redraw bump (the two `loadProject(fileInfo)` sites only; instrument/preset loads
       correctly stay dirty; `recoverFromAutosave` stays dirty by design).
    2. *Pressing NO minimized the app.* On the Miyoo the **B button is `KEYCODE_BACK`**
       (ButtonHandlers.kt:298); `handleKeyEvent` only consumes it when the input view has focus, but the
       prompt appears gated on `audioReady`, which can flip after a focus-losing recomposition — so the
       BACK fell through to the system (minimize) instead of dismissing. Fix: request focus as the dialog
       shows (`delay(50)` then `requestFocus`, matching the existing layout-change re-focus) so A *and* B
       reach the app, plus a `BackHandler(enabled = showRecoveryDialog)` safety net that turns a
       still-unfocused BACK into NO/discard rather than a minimize.
  *Test (simulate a crash):* edit, then `adb shell am force-stop com.conanizer.pockettracker`, relaunch →
  **"RECOVER WORK?"** appears. **A** → edits + samples restored, project shows dirty/"RECOVERED", a real
  Save then clears it. **B** → discarded (no minimize), no prompt next launch. **Load a project, don't
  edit, relaunch → no prompt** (fix 1). Clean save then relaunch → no prompt. Recover a project whose
  instruments use WAVs → samples audible after recover. Dialog blocks other input (SELECT/START/R-nav).

### Stage 5 — 5.3 Phase C — crash-recovery autosave: the onStop flush (2026-06-17) — 🔧 awaiting device test

- **5.3.C 🔧 onStop background flush** (`FileController`, `MainActivity`). The 3 s debounce can lose the
  last edits if Android kills the *backgrounded* app (common on the 1 GB Miyoo) before it fires. A
  `DisposableEffect` registers a `LifecycleEventObserver` on the activity's lifecycle; on `ON_STOP`,
  if `isProjectDirty`, it flushes **synchronously** via new `FileController.saveAutosave(project)`
  (= `writeAutosave(serializeProject(project))`). onStop runs on the main thread and the process may be
  killed right after, so it can't await a coroutine; main is the project's sole mutator, so the direct
  serialize+write is tear-free.
  - **No `LocalLifecycleOwner`**: with Compose BOM 2025.11.01 and no `lifecycle-runtime-compose` dep,
    neither package's `LocalLifecycleOwner` is reliably available — so the lifecycle is taken from
    `LocalContext.current as? ComponentActivity` (`MainActivity` is one). `Lifecycle`/`LifecycleEventObserver`
    come from the present `lifecycle-runtime-ktx`.
  - Completes 5.3. Kotlin-only (pre-commit compiles it).
  *Test:* edit, **background the app** (home), then `adb shell am kill com.conanizer.pockettracker` (or let
  the system reclaim it) → relaunch → **"RECOVER WORK?"** with the *latest* edit (not just up to the last
  debounce). Confirm the onStop write fires immediately on background (logcat `✅ Wrote file: …/autosave.ptp`
  the moment you home out with unsaved edits), and **not** when the project is clean (just saved/loaded).

5.3 (crash-safe autosave) complete across Phases A/B/C. **3.2 / 4.1 / 4.2 / 5.2 / 5.3 (A+B+C)** done.
5.1 (sample-RAM readout) is now in progress — **Stage 1 shipped** (see below); it immediately surfaced a
SoundFont memory leak (see "5.1 finding"), which is the next fix.

### 5.3 follow-up ✅ DONE (RESUME setting) — session resume on app-killing ROMs

Device testing surfaced a UX gap that is **OS-side, not app-side**. The Miyoo Flip and Ayaneo Pocket
Air Mini (AOSP-ish ROMs) **kill the process when the app is backgrounded**, so it cold-starts on return;
the Xiaomi 12T Pro (MIUI) keeps it warm and resumes seamlessly. (Confirmed by the developer comparing all
three. Proof it's a true cold-start, not a code reset: the recovery prompt is only ever set by
`LaunchedEffect(audioReady)` when `audioReady` flips false→true, which can't happen on a warm resume.)
Because the app holds the whole session only in memory and doesn't reopen the last project on launch, a
home-out→return on the handhelds lands on a blank project (template/empty) — and, for unsaved work, the
"RECOVER WORK?" prompt. The prompt is the right default on a warm phone but noise on a device that kills
the app every background, so the choice is now **per-device and user-selectable**.

- **Shipped:** a **RESUME** setting (SETTINGS row 11, persisted in `SharedPreferences` so it differs per
  device, *not* in the project file):
  - **ASK** (default) → the "RECOVER WORK?" prompt (unchanged Phase-B path). Right for warm-resume phones.
  - **AUTO** → on launch, silently `recoverFromAutosave()` + reload samples, no prompt (status flashes
    "RECOVERED"). The recovered project stays dirty, so it keeps re-autosaving and survives the *next*
    kill too; a corrupt autosave is dropped so AUTO can't loop on it. Right for the Miyoo/Ayaneo, where it
    makes a home-out→return land back on the in-progress work, replicating the Xiaomi warm-resume feel.
  - The Phase C onStop flush still ensures the latest edits are captured before the OS kill, for both modes.
  - Code: `MainActivity` `LaunchedEffect(audioReady)` branches on `autosaveResumeAuto`; the setting is
    wired through the usual `SettingsModule` → `AppInputDispatcher`/`AppStateRefs` → `ScreenLayouts` →
    `PixelPerfectRenderer` path (mirrors `notePreviewEnabled`).
- **Still deferred (the rolling "last session" part):** AUTO resumes *unsaved/recovered* work only. After a
  clean **Save** + background-kill, AUTO still lands blank, because a clean save clears the autosave and the
  onStop flush skips a non-dirty project. Closing that gap means making the autosave a rolling last-session
  (written on edits *and* on save, cleared only by NEW) so AUTO also resumes saved sessions — a moderate
  change (autosave shifts from crash-prompt to full session-persistence; the dirty-flag-after-resume detail
  needs settling). **Parked — revisit if the saved-then-blank case proves annoying in practice.**

### 5.1 ✅ Stage 1 DONE — sample-RAM readout on the PROJECT screen

Staged (agreed with the developer): **Stage 1** readout → **Stage 2** live `USED / LIMIT` + device-memory
diagnostics to calibrate the real crash wall per device → **Stage 3** load-blocking once calibrated.

**Stage 1 (shipped, device-tested):** a read-only `SAMPLE RAM  xx.x MB` line below the SYSTEM row on the
PROJECT screen (not a cursor row). Instead of summing native buffers by hand, it uses Android's own number:
a startup baseline is snapshotted with `Debug.getNativeHeapAllocatedSize()` right after the engine's DSP is
allocated but *before* any samples load; while the PROJECT screen is shown, a 500 ms poll displays
`current − baseline` ≈ the PCM of the user's loaded samples **and** soundfonts (TSF allocates native heap, so
SF is counted automatically — no SF proxy, no C++ getter). Integer tenths-of-a-MB formatting avoids locale
issues; the value only recomposes when it actually changes.
- Files (all Kotlin): `MainActivity` (baseline capture in the engine-create effect + PROJECT poll + state),
  `ScreenLayouts` + `PixelPerfectRenderer` (display pass-through, mirrors `renderProgress`), `ProjectModule`
  (the line).
- On device: ~3.5–6 MB at rest, grows on sample/SF load, drops on NEW.
- Caveat: it measures *native growth since launch*, so it can include minor engine warm-up (lazy DSP on first
  playback). Stage 2's diagnostics will quantify that; the baseline can be refined then if needed.

### 5.1 finding ✅ FIXED — SoundFont memory leak (surfaced by the Stage 1 readout)

The readout earned its keep immediately. Loading a 101 MB SF2 shows ~206 MB (TSF expands 16-bit samples to
32-bit float ≈ 2× file size — correct, not double-counted). **NEW (or loading another project) does not
reclaim it** — the number stays at 206 MB, reproducibly. On a 1 GB device this is the single biggest OOM risk.

**Root cause (three facts):**
1. `unloadSoundfont()` exists (native `tsf_close`, `jni-bridge.cpp:843`; backend wrapper; interface decl) but
   has **zero callers** anywhere in Kotlin.
2. `newProject()` frees **PCM only** — `clearAllSamples()` clears the `samples[]` slots (why PCM drops), but
   nothing touches the `soundfonts[]` handles, and `sfSlotMap` is never cleared.
3. SoundFonts live in a **4-slot LRU cache** (`MAX_SOUNDFONTS = 4`) reclaimed only by *eviction* — i.e. when a
   **5th** distinct SF2 is loaded. So NEW/load leaves the `tsf` handle + its float samples fully resident.

Parallel gap found while tracing: the **load-project path never calls `clearAllSamples` either** (only
`newProject` does), so loading project B after A frees A's PCM only for the instrument slots B reuses;
soundfonts always leak.

**Fix (shipped, device-tested):** added native `clearAllSoundfonts()` (loops all slots, detaching voices then
`tsf_close` under `soundfonts[slot].mutex` — the same body `unloadSoundfont` uses and the same lock the audio
render holds, so it's safe to call during playback) → `IAudioBackend`/`OboeAudioBackend` →
`InstrumentController.clearAllSoundfonts()` (+ `sfSlotMap.clear()`). Called in `newProject()`, and
`clearAllSamples()` + `clearAllSoundfonts()` now run at the **top of `reloadProjectSamples()`** so every
load/recovery path starts from a clean native slate (this also closes the PCM-on-load gap above). Verified on
device: **NEW now drops `SAMPLE RAM` back toward baseline** instead of stranding the SF2. Still open (minor,
not the leak): `reloadProjectSamples` has no SF **dedup** — two instruments sharing one SF2 load it twice into
two slots — worth fixing later.

**Two more free-path gaps surfaced by the readout (both fixed, device-tested; Kotlin-only, reusing the
existing `unloadSoundfont` / `reloadProjectSamples`):**
1. **Changing instrument type away from SoundFont didn't free the SF2.** `setInstrumentType` freed the WAV
   when switching *to* SoundFont (`clearSample`), but the reverse branch only nulled `soundfontPath` — the SF2
   stayed resident. Fixed: the to-sampler branch now `unloadSoundfont`s the slot + drops the `sfSlotMap`
   entry, **guarded** so it frees only when no other instrument still references that `.sf2`.
2. **COMPACT INST didn't reclaim memory until a save + reload.** `cleanUnusedInst()` replaced unused
   instruments with empty ones in the data model but left their native sample/SF2 buffers loaded. Fixed: the
   COMPACT-INST confirm now calls `reloadProjectSamples()` after `cleanUnusedInst()` (INST only, not SEQ),
   which clears all native buffers and reloads only what the compacted project still references — RAM drops
   immediately instead of waiting for a save + reload.

### 5.1 finding ✅ FIXED — SF2 de-duplication (RAM) + the volume-sharing trap (avoided)

Today `loadSoundfont` never de-dups: every SoundFont instrument loads its **own** `tsf` handle, so N instruments
referencing one `.sf2` cost N× its (already ~2×-file-size) RAM. With `MAX_SOUNDFONTS = 4`, the worst case is
4× one SF2 — a real wall on a 1 GB device. De-dup (one handle per unique file) is the obvious win, but a prior
attempt caused **different presets of one SF2 on two tracks to share volume**, so it was reverted.

**Why it shared, exactly.** The per-channel rendering fork already isolates almost everything per track —
`tsf_channel_set_volume/_pan/_bank_preset/_pitchwheel(h, trackId, …)` are all keyed by track, and filter/drive
ride `instrumentParams[instrumentId]` copied into the per-track voice. The **one** exception is the **ADSR
envelope override** (ATK/DEC/SUS/REL): `tsf_preset_apply_overrides` patches `presets[].regions[].ampenv`, which
is **shared per-`(bank,preset)` on the handle** — and `SUS` is a *sustain gain*, i.e. a volume level. With each
instrument on its own handle that never collides; share a handle and instruments on the same preset fight over
that patch. It's applied at *schedule* time (≈2 phrases early), making it racy too.

**The enabler for a clean fix.** TSF *captures* the envelope into the voice at note-on
(`tsf_voice_envelope_setup(&voice->ampenv, &region->ampenv, …)`) and renders from the voice's own `ampenv`, not
the live region — so a region patch only affects the **next** note-on; playing voices are immune.

**Fix (shipped, device-tested):**
1. **De-dup the handle.** `native_loadSoundfont` first scans for a slot already holding that `filePath` and
   returns it (LRU-touched) instead of loading a second copy. Frees stay reference-guarded (the
   `setInstrumentType` "no other instrument uses this path" check, generalized).
2. **Make the envelope override atomic per-trigger, keyed by instrument id.** Store ATK/DEC/SUS/REL per
   instrument in C++ (`sfEnvOverrides[256]`, indexed by `sampleId` → *always unique*, so two instruments never
   collide in storage). At trigger, `SoundfontVoice::triggerNote` calls `tsf_preset_apply_overrides(...)` **right
   before `tsf_channel_note_on`, under the slot mutex it already holds** — so note-on captures *this* instrument's
   envelope atomically; the next trigger re-patches and re-captures; simultaneous notes on a shared handle each
   capture their own. `native_setSoundfontEnvelopeOverrides` becomes a per-instrument *store* (no schedule-time
   region patch), so the old racy path is gone.

   Result: de-dup gives 1× RAM while different presets/overrides of one SF2 on different tracks stay fully
   independent. **Verified on device** against the original repro: two instruments sharing one SF2 now show ~1×
   its RAM (was ~N×), and two tracks with different presets + different ATK/SUS stay independent — the
   volume-sharing trap is gone. Single-instrument behaviour (incl. KIL→REL release) is unchanged: the same
   override values are applied, just atomically at trigger instead of as a persistent schedule-time region patch.
   Touched C++ (`audio-engine.{h,cpp}`, `soundfont-voice.{h,cpp}`, `jni-bridge`) + Kotlin backend
   (`IAudioBackend`/`OboeAudioBackend`/`AudioEngine`); C++ isn't validated by the pre-commit hook, so the gate
   was a clean build + that repro.
