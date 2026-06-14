# PocketTracker Code Review #2 (2026-06-12)

Second full-repo review pass (first review: `REVIEW.md`, all findings fixed via PR #8).
Focus this round: fresh bugs/incorrectness, **performance & memory for low-end devices**
(Miyoo Flip 1GB), and structure/optimization ideas. Findings already fixed in Review #1
or already tracked in dev-status "Architecture Debt" are NOT re-reported.

Severity legend: 🔴 bug / correctness · 🟠 inconsistency or duplication · 🟡 perf / memory · 🔵 structure/readability · 💡 idea

## Stage Tracker

| # | Stage | Status |
|---|-------|--------|
| 1 | C++ engine hot path (audio-engine.cpp/h, sampler-voice, note-queue, mods/, soundfont-voice) | ✅ done |
| 2 | C++ sample editor + DSP effects + JNI bridge (sample-editor.cpp, effects/*, jni-bridge.cpp) | ✅ done |
| 3 | Kotlin core logic (Playback, Tracker, Instrument, Effect, Clipboard, File, Render controllers) | ✅ done |
| 4 | Kotlin audio layer + storage (AudioEngine.kt, backends, WavWriter, AndroidFileSystem) | ✅ done |
| 5 | Input layer (AppInputDispatcher, ButtonHandlers, InputController, CursorContext, VirtualControls) | ✅ done |
| 6 | UI / Compose perf (PixelPerfectRenderer, ScreenLayouts, modules, MainActivity) | ✅ done |
| 7 | Build config, memory footprint, cross-cutting + ideas; Executive Summary | ✅ done |

Status values: ⏳ pending · 🔄 in progress · ✅ done

---

<!-- Findings appended per stage below -->

## Stage 1 — C++ engine hot path

Files: `audio-engine.cpp/h`, `sampler-voice.h`, `note-queue.h`, `audio-defs.h`, `mods/mod-runner.h`.
**Positive:** the unified `processAudioBlock` discipline is intact; FZ denormal handling, per-sample
envelope/pan interpolation, the 3-step voice allocator, and the try-lock sample-edit protection are
all solid. The findings below are mostly about the *remaining* real-time hazards and per-block waste
that matter on the Miyoo Flip.

### 1.1 🔴 (latent crash) `processAudioBlock` has no guard for `numFrames > MAX_BLOCK` (1024)
All the per-block stack arrays (`revSendBufL/R`, `dlySendBufL/R`, `trackWaveAccumL/R`,
`instrSpectrumTempL`, `revWetL/R`, `dlyWetL/R`, and `sfBuf[2048]` = 1024 frames) are sized to
`MAX_BLOCK = 1024`, but `onAudioReady` passes Oboe's `numFrames` straight through with no clamp
(`audio-engine.cpp:1530`). Oboe normally delivers 192–960 frames, but with the None/Shared fallback
path (attempt 3) or unusual ROMs, callbacks **can** exceed 1024 frames — then `revSendBufL[i] += …`
writes past the array → silent stack corruption or crash that would be near-impossible to debug on
a user's device. **Fix (cheap):** chunk inside `onAudioReady` the same way `renderOffline` already
does (`while (rendered < numFrames) processAudioBlock(..., min(1024, remaining), ...)`) — 5 lines,
removes the entire class of bug.

### 1.2 🟡 ~84 KB of stack arrays are zero-initialized on EVERY audio callback
`processAudioBlock` declares `trackWaveAccumL/R[8][1024]` (64 KB), `revSendBufL/R` + `dlySendBufL/R`
(16 KB), `instrSpectrumTempL` (4 KB) with `= {}` — the compiler memsets the **full** arrays each
block regardless of `numFrames` (audio-engine.cpp:379-388). At ~230 callbacks/s that's ~19 MB/s of
pointless memset on a low-end ARM core, plus ~110 KB of audio-thread stack. Fixes, in order of value:
1. Zero only `numFrames` entries (`memset(buf, 0, numFrames * sizeof(float))`), or make them engine
   members zeroed up to `numFrames`.
2. The 64 KB `trackWaveAccum` pair exists **only** for the OCTA visualizer — gate both the zeroing
   and the per-voice `trackWaveAccumL[t][i] += …` accumulation (2 adds/frame/voice in the hot loop)
   behind an `std::atomic<bool> octaCaptureEnabled` that the UI sets only when the visualizer mode
   is OCTA. Most sessions never pay for it.

### 1.3 🟡 Audio thread locks 3 queue mutexes per FRAME (~400k lock ops/sec)
The per-frame event loop calls `paramUpdateQueue.hasUpdateAt()`, `killQueue.hasKillAt()`,
`noteQueue.hasNoteAt()` every frame (audio-engine.cpp:395-434), and each call takes/releases that
queue's mutex — 3 × 44100 ≈ 132k lock/unlock per second *even when all queues are empty* (plus one
more per pop). This is the same class of overhead as the `volumeMutex` fixed in review #1, one layer
up. **Fix:** drain once per block — under one lock per queue, pop everything with
`targetFrame < globalFrameCounter + numFrames` into a small local array (sorted; the heap pops in
order), then dispatch from the local array inside the frame loop with zero locking. Alternatively a
cached `std::atomic<int64_t> nextEventFrame` per queue lets the common empty case skip the lock
entirely.

### 1.4 🔴 (real-time hazard) always-on `LOGD` inside `NoteQueue/KillQueue::schedule()` **while holding the queue mutex**
`LOGD` is unconditional `__android_log_print` (audio-defs.h:5) — a syscall that can take tens of µs.
`NoteQueue::schedule` (note-queue.h:95-100) and `KillQueue::schedule` (note-queue.h:152-156) log
every scheduled note/kill *inside* `lock_guard` on the same mutex the audio callback takes every
frame (1.3). With the 2-phrase lookahead + retriggers/arps scheduling dozens of events per phrase,
the Kotlin thread repeatedly holds the audio thread's lock during a logging syscall → genuine
priority-inversion / dropout hazard. It also undoes review #1's per-note log gating (3.4/4.2) — the
Kotlin side stopped logging per note, but the C++ side still does. **Fix:** demote both to `LOGT`,
or at minimum move the log outside the lock.

### 1.5 🔴 Integer overflow in `Voice::trigger()` start/end math for samples ≳ 3.2 minutes
`actualStart = (effectiveStartPoint * length) / 255` (sampler-voice.h:123-125) and
`retrigger()`'s `(startPoint * sampleLength) / 255` (sampler-voice.h:310) overflow 32-bit int when
`length > ~8.4M` frames (≈ 190 s at 44.1 kHz) and the point value is large. The per-block float
recompute in `processAudioBlock` (audio-engine.cpp:1101-1107) papers over it after the first block,
but the initial `position` comes from the overflowed value — long samples (easy to create via the
**video-audio extraction** feature) start playing from the wrong place, or from 0 with START ignored.
**Fix:** cast to `int64_t` (or use the same float math as the per-block path) in both places.

### 1.6 🟡 `float position` precision limit on very long samples
A 32-bit float can't represent every integer above 16.7M (~6.3 min at 44.1 kHz); `position +=
modulatedRate` stalls/jitters near the tail of very long samples. Not worth changing the engine for
a tracker, but worth (a) a documented limit, or (b) clamping/warning at sample-load time for files
beyond ~5 minutes — cheaper than chasing a "playback freezes near the end of my 8-min stem" report.

### 1.7 🟠 `getSampleRate()` fallback is 48000; every other fallback in the engine is 44100
audio-engine.cpp:330 returns 48000 when the stream is null, while `triggerNote`, `setPitchSlide`,
`setPitchBend`, `updateVoiceModulation`'s default, and the Kotlin layer all fall back to 44100.
If Kotlin caches this value before the stream opens, every rate/pitch computation is ~8.8% off.
One-line consistency fix.

### 1.8 🟡 `globalFrameCounter` is a plain `int64_t` shared across threads
Written by the audio thread (`processAudioBlock`), read by the Kotlin scheduler via
`getCurrentFrame()` JNI — formally a data race (benign on arm64, but the project is also
targeting a future Linux port on unknown hardware). `std::atomic<int64_t>` with relaxed ordering
costs nothing on arm64 and makes it correct.

### 1.9 🟡 KissFFT config allocated + freed on every spectrum call
`computeSpectrumFFT` (audio-engine.cpp:1769-1772) runs `kiss_fftr_alloc` / `kiss_fftr_free` per call
— a malloc plus twiddle-table trig init at ~20 fps whenever the EQ screen is visible. Cache one
`kiss_fftr_cfg` (static or member, guarded by the existing call pattern — all callers are the UI
poll thread). Free win on the Miyoo Flip while the EQ editor is open.

### 1.10 🟠 Spectrum ring-buffer writes run every block even when no visualizer is open, under a blocking mutex
`processAudioBlock` writes the delay + reverb + instrument spectrum rings every block
(audio-engine.cpp:1460-1505), and `onAudioReady` writes the master ring (1544-1550) — all under
`lock_guard(spectrumMutex)`, the same mutex the UI thread holds while copying 2048 samples out.
Two issues: (a) wasted writes ~99% of the time (EQ visualizer closed); (b) the audio side **blocks**
if the UI holds the lock (unlike the sample-edit path, which try-locks). **Fix:** an
`std::atomic<bool> spectrumCaptureEnabled` toggled by the EQ screen open/close, and `try_to_lock` on
the audio side (dropping one block of visualizer data on contention is invisible).

### 1.11 💡 Voice allocation: silent note drop when the pool is exhausted
When all 8 slots are active and none is fading, the note is dropped with only a (compiled-out) LOGT
(audio-engine.cpp:728-730). With 8 tracks + retrigs + fading tails this is reachable. Consider
last-resort stealing the *quietest* or *oldest* voice instead of dropping — droppped notes on the
downbeat are much more audible than a 1.45 ms fade on a tail. (Behavior choice, not a bug.)

## Stage 2 — C++ sample editor + DSP + JNI bridge

Files: `sample-editor.cpp`, `jni-bridge.cpp`, `effects/instrument-chain.h` (+ spot checks).
**Positive:** the review-#1 stereo lockstep work is genuinely consistent — every op maintains
`samplesRight` and routes length changes through `setSampleBuffers()`. JNI array pinning is balanced
everywhere (every `Get*ArrayElements` has a matching `Release`, correctly using `JNI_ABORT` for
read-only access). `InstrumentChain` is a clean module seam.

### 2.1 🔴 (crash) `cropSample` / `deleteSampleRegion` / `pasteRegion` / `downsampleSample` free buffers WITHOUT `sampleEditMutex` and without stopping voices
The locking discipline exists in `applyRateMode`, `pitchShiftSample`, `timeStretchSample`,
`applySampleFx`, `undoSample` (stop voices reading the slot → `lock_guard(sampleEditMutex)` → swap),
but these four length-changing ops (sample-editor.cpp:270-372) call `setSampleBuffers()` —
which `delete[]`s the old buffers — with **no lock and no voice stop**. If the sample is audible at
that moment (note playing in the background song, or the user previews then immediately crops), the
audio thread's `try_to_lock` **succeeds** and the mix loop reads freed memory → garbage audio or
SIGSEGV. This is the same crash class as review #1's 5.1, surviving via the locking side instead of
the channel side. The in-place ops (`normalizeSample`, `fadeIn/Out`, `silenceRegion`,
`reverseSample`) also run lock-free — they can't crash (no free) but can produce a block of scrambled
audio mid-edit. **Fix:** one helper used by *every* editor op, e.g.
`std::lock_guard<std::mutex> prepareEdit(int id)` pattern: stop voices whose `sampleData ==
samples[id]`, then return the held lock. Removes the whole "which ops remembered the ritual" class.

### 2.2 🔴 (crash, small window) SoundFont handle lifecycle races with eviction
Two spots in `processAudioBlock` touch a `tsf*` outside its slot mutex:
- The render pass captures `tsf* h = soundfonts[sv.sfSlot].handle` **before** taking
  `soundfonts[slot].mutex` (audio-engine.cpp:1369-1375). If `loadSoundfont`'s eviction path
  (jni-bridge.cpp:723-743) runs `tsf_close` between the read and the lock, the audio thread then
  calls `tsf_render_float_channel` on a freed handle.
- Pass 1 calls `tsf_channel_set_volume(h, …)` (audio-engine.cpp:1325-1328) with **no lock at all**,
  mutating TSF state concurrently with a possible `tsf_close`.
Low probability (requires loading a 5th SF2 while older ones are playing) but it's a hard crash.
**Fix:** read the handle *inside* the lock in both places; or keep an `std::atomic<tsf*>` and have
eviction first null the pointer, then detach voices, then close after the current block.

### 2.3 🟡 Per-poll heap allocations in JNI getters
`native_getTrackWaveforms` does `new float[4960]` per UI frame when OCTA is active
(jni-bridge.cpp:408); the two spectrum getters allocate `new float[numBins]` + a fresh
`jfloatArray` per call (~20 fps with the EQ open). Small, but it's per-frame churn on a 1 GB device
where the GC already competes with Compose. Stack buffers (`numBins ≤ 64`?) or reusable members fix
it for free. (Pairs with 1.9's FFT-cfg realloc.)

### 2.4 💡 SF2 eviction "oldest = smallest instrumentId" isn't oldest
jni-bridge.cpp:724-731 evicts the slot with the smallest instrument index, which is just "lowest
slot number in the instrument list" — e.g. the SF2 on instrument 00 gets evicted first even if it's
the one playing right now. A simple monotonic `lastUsedCounter` per slot (bumped on note trigger)
gives true LRU for one int per slot.

## Stage 3 — Kotlin core logic

Files: `PlaybackController.kt` (full), `FileController.kt` (full), `RenderController.kt` (targeted),
spot checks elsewhere. **Positive:** `FileController` is now genuinely tidy — shared
`decodeAndMigrate` + `validateProjectStructure` mean every load path is protected; the render
scheduler reuses `schedulePhrase` so realtime/offline can't drift; checkpoint-based buffer rollback
(`notifyDataChanged`) is a nice design.

### 3.1 🔴 (OOM) WAV export holds ~4 full-song float copies in memory at once
`RenderController` renders the **entire song in one call**: `audioBackend.renderFrames(totalFrames)`
(RenderController.kt:86, :167, :288…). At that moment the following all coexist:
C++ `std::vector<float>(totalFrames*2)` (jni-bridge.cpp:365) → the returned `jfloatArray` (same
size) → Kotlin `audio` → then `leftChannel` + `rightChannel` copies (RenderController.kt:91-92).
A 4-minute song at 44.1 kHz stereo float is ~84 MB per copy → **~300+ MB peak** during export.
On the Miyoo Flip (1 GB total RAM) that's an OOM kill for any non-trivial song — and stems export
does it repeatedly. **Fix:** render in chunks (e.g. 10-second blocks: `renderFrames(441000)` in a
loop) and stream each chunk to disk; `WavWriter` needs a streaming mode (write data chunks
sequentially, patch sizes in the header at the end — standard approach). This also makes a progress
bar possible for free.

### 3.2 🟡 `PlaybackController.TRACE` exists but is never used — all 53 `logger.d` calls run ungated
The companion defines `TRACE = false` with a comment saying it gates the per-step scheduling trace
(PlaybackController.kt:196-199), but **no call site in the file references it** (`grep "if (TRACE)"`
= 0 hits, `logger.d` = 53 hits). Review #1's logging fix landed in `EffectProcessor` and
`AudioEngine.kt` but not here — so during playback the scheduler still builds strings per note
(volume chain log with two `"%.4f".format()` at :1125-1127), per retrigger (:1394-1395), per arp
batch, per CHA/RND roll (:821, :830, :857-860), etc. On the Miyoo Flip with retrig/arp-heavy
patterns this is the single biggest remaining Kotlin hot-path waste. **Fix:** mechanical — wrap the
per-step/per-note ones in `if (TRACE)`; keep lifecycle logs (play/stop/load) ungated.

### 3.3 🟠 GRV pre-scan still hand-expands FX slots
`schedulePhrase` (PlaybackController.kt:564-566) uses
`when (fxSlot) { 1 -> step.fx1Type; … }` — the exact pattern the review-#1 `fx(slot)` accessors
(6.3) were added to eliminate. One-line cleanup: `val (fxType, fxValue) = step.fx(fxSlot)`.

### 3.4 🔵 `applyChanceAndRandomize` reads CHA from `step` but RND/RNL from `effectiveStep`
Correct today (CHA never rewrites itself), but the asymmetry is easy to trip over when adding a
fourth meta-effect. A comment, or reading both loops from `effectiveStep`, would make the intent
explicit. Cosmetic.

## Stage 4 — Kotlin audio layer + storage

Files: `AudioEngine.kt` (targeted), `WavWriter.kt` (full), `OboeAudioBackend.kt` /
`IAudioBackend.kt` (spot), `RenderController` interplay. **Positive:** the polling buffers in
`AudioEngine.kt` (waveform, track waveforms, spectrum) are preallocated and reused — the out-array
pattern is the right one. `WavWriter` correctly guards the 2 GB WAV size field with a Long check,
and `readCuePoints` does proper chunk-skipping with even-padding.

### 4.1 🟡 `WavWriter.readCuePoints` loads the ENTIRE WAV into memory just to find the cue chunk
`File(path).readBytes()` (WavWriter.kt:167) reads the whole file — potentially tens of MB — to scan
a few chunk headers. It's called on **every sample load** (`loadSampleFromFile`) and again for every
instrument in `reloadProjectSamples` on project load, exactly when the parser has *also* just loaded
the full file → transient 2× file-size allocations per sample, GC pressure on the 1 GB Miyoo during
project load. **Fix:** scan with `RandomAccessFile` — read 8-byte chunk headers, `seek()` past
chunk bodies; only the cue chunk body (a few hundred bytes) ever gets read. ~20 lines.

### 4.2 🟡 `WavWriter.writeWav` buffers the whole file in one `ByteBuffer`
`ByteBuffer.allocate(totalFileBytes)` (WavWriter.kt:66) is fine for samples, but it's the second
half of the render-OOM problem (3.1): song export currently needs full-song floats AND the full
16-bit file in memory simultaneously. The streaming render fix needs a `WavStreamWriter` (open →
append sample blocks → finalize header). Worth doing as one unit with 3.1.

### 4.3 🔵 Per-poll allocations in the remaining getters
`getSpectrumMagnitudes*` returns a fresh `FloatArray` allocated on the C++ side per call (~20 fps
with EQ open), and `getActiveTrackNotes()` builds a new `List<Note>` per UI frame during playback.
The codebase already has the out-array pattern for waveform/peaks — extending it to these two
removes the last per-frame allocations in the audio polling path. Minor, pairs with 2.3.

## Stage 5 — Input layer

Files: `AppInputDispatcher.kt` (targeted — review #1 already covered this file deeply),
`SampleEditorModule.kt` (dialog state). Lighter pass by design; the focus was whether review #1's
modal-guard fix has gaps.

### 5.1 🔴 Sample editor's "discard changes?" dialog is NOT covered by the modal guard
`confirmDialogOpen()` (AppInputDispatcher.kt:1754-1755) guards CLEAN / NEW PROJECT / INSTR TYPE —
but **not** `sampleEditorState.showConfirmClose` (the close-confirm dialog the sample editor shows
when edits would be discarded). A (:975) and B (:1728) handle it, but with that dialog open:
- **SELECT** falls into the `ScreenType.SAMPLE_EDITOR` branch (:1786-1809) and can open the **EQ
  editor** (cursor on FX row) or the **sample-name keyboard** on top of the confirm dialog;
- **START** falls through to the preview/playback action;
- **R+DPAD** navigation guards likewise don't know about it.
This is exactly the bug class review #1's 2.1 fixed — one dialog didn't make the list. **Fix:** add
`|| sampleEditorState.showConfirmClose` to `confirmDialogOpen()` (the guard is only used by non-A/B
handlers, so A-confirm / B-cancel keep working). Also consider asserting the rule somewhere central:
every `show*Dialog`-style state must appear in this predicate — a comment listing them all helps the
next dialog not repeat this.

### 5.2 🔵 (note) No other guard gaps found
`handleSelect`/`handleStart`/`handleRUp/RDown/RLeft/RRight` all check `confirmDialogOpen()` first,
and qwerty/theme/EQ overlays consume SELECT before screen dispatch — consistent with the review-#1
fix. The SELECT-on-EFFECTS delay-sync toggle correctly re-pushes params to the backend.

## Stage 6 — UI / Compose performance

Files: `PixelPerfectRenderer.kt`, `BitmapFont5x5.kt`, `EditorHelpers.kt`, frame-loop structure.
**Positive:** the single-Canvas design (no BoxWithConstraints/SubcomposeLayout) with the
documented RenderThread-SEGV rationale is solid engineering; `TrackerLayout` is correctly
`remember`ed outside the draw lambda; the font's horizontal run-merging is real. The findings
below are where the remaining frame budget on the Miyoo Flip goes.

### 6.1 🟡 The WHOLE screen redraws at 60 fps unconditionally, even when idle
`oscilloscopeTicker` increments every 16 ms forever (PixelPerfectRenderer.kt:209-214) and is read
inside the draw lambda, so the full 640×480 layout — every text cell of the phrase grid, every
module — re-renders 60×/sec even when nothing is playing and no input has occurred. On a
battery-powered handheld this is the single biggest CPU/GPU/battery cost in the app. Two tiers of
fix:
1. **Cheap:** throttle the ticker when `!isPlaying` (e.g. 10 fps after the waveform has decayed to
   silence — `decayWaveform` already exists, so the scope goes flat quickly); restore 60 fps on
   playback or input.
2. **Structural (bigger win):** give the oscilloscope/visualizer its own small Canvas that ticks at
   60 fps, and let the main layout Canvas redraw only when state actually changes (cursor,
   playback row, projectVersion). Compose already gives this for free once the ticker stops being
   read by the big canvas. The phrase grid is static 99% of the time.

### 6.2 🟡 Font rendering: per-char HashMap lookup + up to ~7 Skia rects per glyph, ~5k draw calls/frame
`drawBitmapChar` does `FONT_5X5[char]` — a `Map<Char, ByteArray>` lookup with Char boxing — then
draws merged runs (PixelPerfectRenderer.kt:1454-1491). A full phrase screen is roughly 700+ glyphs
× ~5-7 rects = thousands of `canvas.drawRect` calls per frame, 60×/sec (see 6.1). Two-step fix:
1. **Cheap:** replace the map with `Array(128) { ByteArray }` indexed by `char.code` (no hash, no
   boxing) — mechanical change, single file.
2. **Real win:** pre-render the font into a glyph-atlas `ImageBitmap` once per (scale, fontScale)
   at startup and `drawImage` one quad per glyph with a tint — cuts draw calls ~7× and lets Skia
   batch them. White atlas + `ColorFilter.tint(color)` keeps theme colors working.

### 6.3 🟡 Per-frame string allocation churn (`toHex2` etc. at 60 fps)
Every visible hex cell calls `Int.toHex2()` → `toString(16).uppercase().padStart()` (3 string
allocations) per cell per frame. With ~100+ hex cells on screen at 60 fps that's tens of thousands
of short-lived strings/sec — and the project's own comment (PixelPerfectRenderer.kt:267-270)
documents that GC pressure has *already* crashed the RenderThread on Snapdragon drivers once.
**Fix (tiny):** precompute `val HEX2_CACHE = Array(256) { it.toHex2() }` and make `Int.toHex2()`
return `HEX2_CACHE[this and 0xFF]` — zero allocation forever, no call sites change. Same trick for
note names (128 possible) if they're formatted per frame. (Fully fixed only in combination with
6.1 — without the ticker, idle frames stop allocating at all.)

### 6.4 🔵 `Log.d` in the composable body
PixelPerfectRenderer.kt:195-197 logs (with string interpolation) on **every recomposition** while
the file browser is open. Remove or gate it.

## Stage 7 — Build config, memory footprint, ideas

Files: `app/build.gradle.kts`, `CMakeLists.txt`, cross-cutting.

### 7.1 🔴 Crash reporter repo name has a trailing dot: `"pockettracker."`
`buildConfigField("String", "GITHUB_REPO_NAME", "\"pockettracker.\"")` (build.gradle.kts:46) —
GitHub repo names cannot end with a period, and `GitHubIssueSenderFactory` passes this straight to
the API. Unless the real repo is named differently, **every crash report silently 404s**. Verify
against the actual repo name; almost certainly a typo. One-character fix, but it defeats the whole
crash-reporting feature on testers' devices.

### 7.2 🟡 Release build ships unminified (`isMinifyEnabled = false`)
For a 1 GB device, R8 matters: smaller dex → faster cold start, less code pinned in RAM, smaller
APK (Compose apps typically shrink 30-50%). The JNI keep rules are already in `proguard-rules.pro`
(added in PR #6), so the risky part is done. **Suggest:** `isMinifyEnabled = true` +
`isShrinkResources = true` for release, then one full smoke test on the Miyoo (watch for
reflection/serialization issues — add `@Keep`/rules for kotlinx-serialization if R8 strips
serializers; the standard rule block is 4 lines).

### 7.3 🟡 Native build: no explicit optimization flags beyond `-ffast-math`
CMakeLists.txt relies on the NDK default (`-O2` for release, **`-O0` for debug**) — so every debug
build you test on the Miyoo runs the DSP unoptimized, which skews "is this fast enough" judgments.
Suggest `target_compile_options(... PRIVATE -O2)` regardless of build type for the audio library
(debugging C++ at -O2 is rarely needed here), and optionally `-flto` for release. Also: the comment
above `-ffast-math` says it's for flush-to-zero, but FTZ is actually done correctly via the FPCR
asm in audio-engine.cpp — the comment oversells what the flag does (it mainly enables FP
reassociation; keep it, fix the comment).

### 7.4 🔵 `buildFeatures {}` declared three times
build.gradle.kts:26, :60, :91 — merge into one block. Cosmetic.

### 7.5 💡 Sample memory budget on the 1 GB target (idea, not a bug)
Worst case per slot the engine holds float32 ×: working L/R + undo backup L/R + RATE original L/R
(+ shared clipboard + FX preview) — up to ~6 copies of a sample. A 30 s stereo sample ≈ 10 MB
working set → ~30 MB with backups. Ideas, cheapest first:
- Free `sampleBackups`/`originalSamples` for a slot when the sample editor **closes** (undo is
  only reachable inside the editor anyway).
- Store undo/original caches as int16 (half the memory; they're bit-exact for 16-bit-sourced WAVs,
  inaudible otherwise).
- Show a "memory used" readout on the project screen — on a 1 GB handheld users will hit the wall
  and a number turns a mystery crash into an understandable limit.

### 7.6 💡 Product ideas (open-ended, from the review)
- **Idle/low-power mode:** pair 6.1 with a Settings row (e.g. UI FPS: 60/30/AUTO). Handheld users
  notice battery life more than desktop-class smoothness on static tracker screens.
- **Crash-safe autosave:** ACRA catches the crash, but the user still loses edits. A periodic
  autosave to `autosave.ptp` (e.g. every 2 min when `projectVersion` changed, off the main thread)
  + "recover autosave?" prompt on launch is cheap insurance and pairs well with release testing.
- **Render progress + cancel:** falls out of the chunked render fix (3.1) — long WAV exports on
  the Miyoo currently look frozen.

---

## Executive Summary & Suggested Priority

**Overall:** the codebase has visibly matured since review #1 — the stereo lockstep discipline,
modal guards, validation-on-load, and style unification all held up under re-inspection. This
pass found **no architecture-level problems**; what it found is (a) a handful of crash-class
races/overflows in paths review #1 didn't dig into, and (b) a clear, itemized performance/memory
budget for the 1 GB Miyoo Flip, which was this review's focus.

### Fix before release (crash / correctness)
1. **2.1 🔴 crop/delete/paste/downsample free sample buffers without the edit lock** — same crash
   class as review #1's top bug; reachable from normal use (edit while a song plays). Cheapest
   high-value fix in this review (one shared helper).
2. **1.1 🔴 no `numFrames > 1024` guard in the audio callback** — latent stack corruption on
   unusual Oboe burst sizes; 5-line chunking fix.
3. **5.1 🔴 sample-editor "discard changes?" dialog missing from the modal guard** — SELECT/START
   act behind it; one-line fix.
4. **1.4 🔴 always-on LOGD inside the note-queue mutex** — priority inversion against the audio
   thread; demote to LOGT.
5. **2.2 🔴 SF2 handle read outside its mutex** (and unlocked `tsf_channel_set_volume`) — rare but
   hard crash when a 5th SF2 evicts a playing one.
6. **7.1 🔴 crash-reporter repo name typo** (`"pockettracker."`) — crash reporting likely dead.
7. **1.5 🔴 int overflow in `Voice::trigger` start/end math** for samples > ~3 min (realistic via
   video-audio extraction).
8. **3.1 🔴 WAV export OOM** — whole-song × ~4 copies in RAM; chunked render + streaming WavWriter
   (with 4.2).

### Performance (Miyoo Flip) — ordered by expected win
9. **6.1** stop the unconditional 60 fps full-screen redraw when idle (biggest CPU/battery win).
10. **3.2** gate PlaybackController's 53 ungated `logger.d` calls behind its own (currently unused!)
    `TRACE` flag.
11. **1.2** stop zeroing ~84 KB of stack per audio callback; gate OCTA capture behind its flag.
12. **6.2 / 6.3** font glyph atlas (or at least array-indexed font + `HEX2_CACHE`) — kills both
    draw-call count and the documented GC-pressure crash vector.
13. **1.3** drain scheduling queues once per block instead of 3 mutex locks per frame.
14. **1.10** gate spectrum capture + try-lock on the audio side.
15. **1.9 / 2.3 / 4.3** cache the FFT cfg; remove per-poll JNI allocations.
16. **7.2 / 7.3** R8 minify for release; explicit `-O2` for the native lib in all build types.

### Robustness / consistency (when convenient)
17. **1.7** 48000-vs-44100 fallback mismatch; **1.8** atomic frame counter; **4.1** stream the cue
    scan; **3.3** GRV pre-scan accessor; **2.4** true-LRU SF eviction; **6.4 / 7.4** cosmetics.

### Ideas worth a thought (7.5 / 7.6 / 1.6 / 1.11)
Sample-memory budget + readout, idle FPS setting, autosave, render progress, long-sample limits,
steal-quietest voice policy.

---

## Fix Log

⬜ = not started · 🔧 = code changed, awaiting device test · ✅ = tested

### Batch 1 — crash/correctness items 1-6 from the Executive Summary — ✅ tested (with Rounds 2-3)

- **2.1 🔧 Sample-editor lock discipline** (`sample-editor.cpp`, `audio-engine.h`)
  New private helper `beginSampleEdit(id)`: stops voices whose `sampleData == samples[id]`, then
  returns a held `unique_lock(sampleEditMutex)`. Applied to the four length-changing ops that were
  missing it (`cropSample`, `deleteSampleRegion`, `pasteRegion`, `downsampleSample`). The five
  in-place ops (`normalizeSample`, `fadeIn/OutSample`, `silenceRegion`, `reverseSample`) now take a
  plain `lock_guard` (nothing is freed there, so no voice-stop — the lock just stops the mix loop
  reading a half-edited buffer). The ops that already had the ritual (applyRateMode, pitchShift,
  timeStretch, applySampleFx, undo) are untouched; migrating them to the helper is a later cleanup.

- **1.1 🔧 Audio callback chunking** (`audio-engine.h`, `audio-engine.cpp`)
  `MAX_BLOCK = 1024` promoted from a local in `processAudioBlock` to a class constant.
  `onAudioReady` now processes `numFrames` in ≤ MAX_BLOCK chunks (same pattern `renderOffline`
  already used), so an oversized Oboe burst can no longer overrun the per-block stack buffers.
  Note: in the (rare) multi-chunk case the per-block peak meters reflect the last chunk only —
  cosmetic, meters decay anyway.

- **5.1 🔧 Modal guard covers the sample-editor discard dialog** (`AppInputDispatcher.kt`)
  `confirmDialogOpen()` now includes `sampleEditorState.showConfirmClose`, so SELECT (EQ editor /
  name keyboard), START (preview), and R+DPAD are swallowed while it is open. A/B handling is
  untouched (those handlers process the dialog before this guard is consulted). Comment now states
  the rule: every new modal `show*` state must be added to this predicate.

- **1.4 🔧 Queue logging demoted** (`note-queue.h`)
  `NoteQueue::schedule` and `KillQueue::schedule` per-event logs changed LOGD → LOGT (compiled out
  unless `AUDIO_TRACE=1`), removing the logging syscall held under the mutex the audio thread takes
  every frame.

- **2.2 🔧 SF2 handle reads moved inside the slot mutex** (`audio-engine.cpp`, `soundfont-voice.cpp`)
  Four spots no longer touch a `tsf*` that eviction could close concurrently:
  pass-1 `tsf_channel_set_volume` (now locked), the render pass (handle read inside the existing
  lock instead of before it), `SoundfontVoice::triggerNote` and `applyPitchMod` (lock held for the
  whole function; both are short and the lock is uncontended except during an actual SF2 load).
  No lock-ordering risk: these are leaf locks, and eviction detaches voices before reloading.

- **7.1 ✅ (pre-existing fix found in working tree)** `GITHUB_REPO_NAME` trailing dot was already
  corrected to `"pockettracker"` in the uncommitted `build.gradle.kts` change — matches
  `git remote` (github.com/conanizer/pockettracker). Include it in this batch's commit.

**Test focus for Batch 1:**
1. **Sample editor under playback (the big one):** load a WAV, put it in a phrase, START song/phrase
   playback, open the sample editor on that same instrument and — while it plays — crop, delete a
   region, copy→paste, downsample (RATE), normalize, fade in/out, silence, reverse. Expect at most a
   ~10 ms hiccup per op; **no crash, no garbled audio**. Then same ops with playback stopped (must
   behave exactly as before). UNDO after each still restores.
2. **Discard dialog:** edit a sample, press B → "discard changes?" appears. Press SELECT (on the FX
   row and on the name row) and START — **nothing** should happen behind the dialog; R+DPAD does
   nothing; A confirms, B cancels as before.
3. **SoundFont:** play SF instruments on several tracks; while playing, load SF2s into new
   instruments until a 5th forces eviction — no crash, evicted instrument goes silent. DETUNE /
   pitch slides / vibrato on SF still work (applyPitchMod path).
4. **General audio regression:** normal phrase/chain/song playback, retrigs, arps — unchanged; no
   new crackling (the chunking is a no-op for normal burst sizes).
5. **Crash reporting (if convenient):** trigger a test crash in a debug build and confirm a GitHub
   issue actually appears now that the repo name is fixed.

### Batch 1 — Round 2 (fixes from device testing, 2026-06-13) — ✅ tested (R2.1 crash + R2.2 dialogs confirmed; R2.3 superseded by R3.1)

Developer test results: items 4 (general audio) OK; 7.1 repo name fixed (no reports expected for
the native crash — see note below). Three issues found, all fixed:

- **R2.1 🔧 SF eviction crash — `sfSlot` TOCTOU race** (`audio-engine.cpp`, `soundfont-voice.cpp`)
  Root cause from the logcat (SIGSEGV in `tsf_channel_set_volume` right after "Evicted soundfont
  slot 0"): every SF lock site checked `sfSlot >= 0` and then **re-read the member** to index
  `soundfonts[]` — but eviction's `detach()` (JNI thread) sets `sfSlot = -1` between the two reads,
  so the audio thread indexed `soundfonts[-1]` → garbage `tsf*` → crash. The Batch-1 locks were
  necessary but didn't close this; the pointer was garbage before the lock was even taken.
  **Fix:** snapshot `int slot = sfSlot;` once, validate the local, index with the local only —
  applied to all 9 sites: `processAudioBlock` pass-1 volume + render pass, `setTrackVolume`,
  and `hardStop` / `noteOff` / `setVolume` / `setPan` / `setMidiNote` / `applyPitchMod` in
  `soundfont-voice.cpp`. A stale-but-valid local is harmless: the handle under that slot's mutex is
  either null (skip) or the newly-loaded SF2 (benign one-block channel-volume write).

- **R2.2 🔧 DPAD navigated behind confirm dialogs** (`AppInputDispatcher.kt`)
  Tester observed START/SELECT/R+DPAD correctly blocked under the discard dialog, but plain DPAD
  still moved the sample-editor cursor behind it. The DPAD handlers only guarded `showCleanDialog`
  (and only Up/Down). All four dialogs are pure A-confirm/B-cancel (verified: `cleanDialogCursor`
  is set once and never moved), so all four DPAD handlers now early-return on `confirmDialogOpen()`.

- **R2.3 🔧 RATE doubled pitch during playback** (`AppInputDispatcher.kt`)
  The Kotlin ratio compensation in `AudioEngine.applyRateMode` is correct, but the 2-phrase
  lookahead had already scheduled notes with the old base frequency — those played the freshly
  decimated buffer at the old rate (double pitch) for up to ~3 phrases. Fix: call
  `playbackController.notifyDataChanged()` right after `applyRateMode`, rolling the schedule buffer
  back to the next phrase boundary so upcoming notes use the new ratio. Remaining notes of the
  *currently sounding* phrase still play at the old pitch (≤ 1 phrase) — unavoidable without
  killing mid-phrase audio.

- **Note on "no crash reports" (7.1):** expected for this crash — ACRA catches **JVM** exceptions
  only; a native SIGSEGV kills the process below ACRA's handler. The repo-name fix matters for
  Kotlin crashes. Native crash reporting would need a breakpad/crashpad-style handler — logged as
  an idea, not planned for MVP.

**Re-test focus (Round 2):**
1. **SF eviction under load:** repeat the exact crash repro — 8 tracks playing different SF
   instruments, then load more SF2s during playback (forcing repeated evictions, past the 9th).
   No crash; evicted instruments go silent or play the replacement SF2 until retriggered.
2. **RATE during playback:** with a sampled instrument playing in a loop, switch RATE
   HIGH→NORM→LOFI→HIGH. Pitch should be correct from the next phrase boundary (≤ 1 phrase of
   wrong pitch right at the switch is expected). RATE while stopped: unchanged.
3. **Dialogs:** discard-changes dialog (and CLEAN / NEW PROJECT / INSTR TYPE) — DPAD now does
   nothing while open; A/B still confirm/cancel; cursor position behind the dialog unchanged after
   closing.

### Batch 1 — Round 3 (RATE pitch root cause, 2026-06-13) — ✅ tested (RATE pitch correct on device)

Round 2 re-test: SF eviction crash ✅ fixed, dialogs ✅ fixed, **RATE still wrong** — because the
Round-2 diagnosis (stale lookahead notes) was incomplete:

- **R3.1 🔧 RATE/downsample never updated the `sampleBaseFrequencies` cache** (`AudioEngine.kt`)
  The phrase-playback `scheduleNote` path reads `sampleBaseFrequencies[sampleId]` (a cache of
  ROOT × ratio / detune, set at sample load and refreshed only by ROOT/DETUNE edits via
  `updateInstrumentBaseFrequency`). `applyRateMode` and `downsampleSample` updated only
  `sampleRateRatios` — so playback pitch was **permanently** wrong after a RATE change, while the
  preview paths (which recompute `C4_HZ × ratio` fresh) sounded correct. That mismatch is exactly
  what the tester heard. **Fix:** `applyRateMode` now scales `sampleBaseFrequencies[id]` by the
  ratio change (old→new), and `downsampleSample` scales it by `factor`. The Round-2
  `notifyDataChanged()` call stays — it makes already-buffered notes pick up the corrected base
  frequency at the next phrase boundary.
  *(Two caches deriving from one value is the underlying smell — a post-MVP cleanup could compute
  baseFreq from `sampleRateRatios` at schedule time and delete `sampleBaseFrequencies`.)*

**Re-test focus (Round 3):** with a sampled instrument looping in a phrase, switch RATE
HIGH→NORM→LOFI→HIGH — pitch must be correct (and identical to HIGH) from the next phrase boundary
after each switch, and must STAY correct. Also: RATE while stopped, then play — correct pitch;
ROOT/DETUNE edits after a RATE change still behave; destructive PITCH/TSTRETCH unchanged.

### Batch 2 — remaining "fix before release" items 7-8 — 🔧 awaiting test

- **1.5 🔧 int64 start/end math in `Voice::trigger` / `retrigger`** (`sampler-voice.h`)
  `(point × length) / 255` now computed in `int64_t` — was overflowing int32 for samples longer
  than ~3 minutes (realistic via video-audio extraction), making START/END/LOOP-START and the
  initial playback position wrong on long samples.

- **3.1 + 4.2 🔧 WAV export rewritten as chunked streaming render**
  (`RenderController.kt`, new `core/storage/WavStreamWriter.kt`)
  - New `WavStreamWriter`: 16-bit PCM streaming writer — writes a placeholder header, appends
    converted chunks, patches sizes + atomically renames `path.tmp → path` on `finish()`
    (no half-written .wav after a failed/aborted render). `abort()` cleans up. Same java.io-only
    portability level as `WavWriter.readCuePoints`. >2 GB guard preserved from `writeWav`.
  - `RenderController`: one shared `renderToWavFile(totalFrames, …)` helper renders in
    `RENDER_CHUNK_FRAMES` (220 500 ≈ 5 s) slices — `renderFrames(chunk)` → append → repeat.
    Replaces all **5** former whole-song blocks (full mix, selection resample, N track stems,
    reverb stem, delay stem). Peak memory drops from ~4 full-song copies (~300+ MB for a 4-min
    song — OOM on the 1 GB Miyoo) to ~2 chunk copies (~4 MB), independent of song length.
  - Bonus: progress now advances smoothly through the render (was stuck at "Rendering audio…"
    30%→85% in one jump); stems report per-pass sub-progress.
  - Chunked output is bit-identical to the single-call render: the C++ engine keeps its frame
    counter and note queue across `renderFrames` calls (renderOffline already chunked internally
    at 256 frames).
  - `WavWriter.writeWav` (in-memory) is unchanged — still used for sample-editor SAVE/CHOP where
    files are small and the cue chunk is needed.

**Test focus for Batch 2:**
1. **WAV MIX (full song):** render a song with reverb/delay/OTT — file plays correctly in an
   external player, length matches, no clicks at ~5 s boundaries (chunk seams), loudness identical
   to a pre-change render of the same song if one exists. Progress bar moves smoothly.
2. **Long song:** render the longest song available (ideally 3-4+ min) on the Miyoo Flip — must
   complete without the app being killed (this was the OOM case).
3. **Selection resample:** resample a selection → file appears in Samples/Resampled, loads and
   plays at correct pitch.
4. **Stems:** render stems — each track WAV + reverb/delay stems sound dry/correct as before;
   per-track progress visible.
5. **Long sample START/END (1.5):** load/extract a > 3-minute WAV, set START well into the sample
   and END before the end — note starts/stops at the set points (previously START was ignored or
   wrong on long samples). Reverse + loop on the same sample.

### Batch 2 — Round 2 (fixes from device testing, 2026-06-14) — 🔧 awaiting test

Batch-2 device test results: **1 WAV MIX ✅, 3 selection resample ✅, 4 stems ✅** — the chunked
streaming render works. **2 long song:** render *completes* fine (this was the render-side OOM, now
fixed), but loading the resulting 29 MB render back **as a sample** OOM-crashed. Testing also
surfaced two pre-existing bugs unrelated to the Batch-2 changes (3 and 5 below). All three fixed:

- **R2.1 🔧 OOM loading a large WAV as a sample** (`AudioEngine.kt` `parseWavBuffer`)
  Loading a 29 MB stereo render crashed with `OutOfMemoryError` at the L/R split (the engine's
  Java heap is clamped to 128 MB on the Miyoo Flip). Root cause: `parseWavBuffer` decoded the whole
  file into a **full-size intermediate `FloatArray` (`rawSamples`)** and *then* allocated separate
  `left`/`right` copies — so the 29 MB byte buffer + ~58 MB `rawSamples` + ~58 MB L/R were all live
  at once (~145 MB > 128 MB). **Fix:** dropped the intermediate — a local `fun decode(idx)` (a named
  local function, not a lambda, so the `Int`/`Float` stay primitive — no per-sample boxing/GC churn)
  reads one sample straight from the byte buffer, and the channels are de-interleaved directly into
  `left`/`right` (mono → one buffer). Peak heap for the float data halves (~58 MB), clearing the OOM.
  Format validation moved to a single up-front check. This is the sample-*load* counterpart to the
  render-side streaming fix (3.1/4.2); a future improvement could stream the file via
  `RandomAccessFile` to also drop the 29 MB byte-buffer copy (pairs with 4.1's cue-scan streaming).

- **R2.2 🔧 Sample-editor preview ignored START/END markers on stereo samples** (`AppInputDispatcher.kt`)
  Pressing START in the sample editor played the **whole sample** instead of the selection, for any
  *stereo* sample (mono worked). Root cause: the default `sourceMode` is LEFT (0), so for stereo data
  `prepareSampleEditorSourcePreview` routes the preview through scratch slot **254** — but the
  selection START/END were pushed via `updateInstrumentPlaybackParams` **before** `inst.sampleId` was
  swapped to 254, so slot 254 kept its default 0/255 params and played the entire sample. **Fix:**
  push the playback params **after** the slot swap, so they land on the slot that actually plays
  (254 for LEFT/RIGHT/MONO, the real slot for STEREO/mono). The post-preview restore (100 ms later)
  is unchanged.

- **R2.3 🔧 Song-screen selection couldn't scroll past row 15** (`InputController.kt`,
  `AppInputDispatcher.kt`, `TrackerController.kt`)
  Selection on the SONG screen (256 rows, 16 visible) only worked within the top 16 rows — expanding
  down stopped at row 15 and the window never scrolled. PHRASE/CHAIN/TABLE (single 16-row screens)
  were correct. Root cause: `expandSelection` was called with a hardcoded `maxRow = 15`, and
  selection mode keeps the cursor anchored (so nothing drove the scroll). **Fix:** pass
  `maxRow = 255` for SONG; new `TrackerController.scrollSongToRow()` scrolls the 16-row window to
  follow the growing selection edge (`selectionEnd.row`) on each DPAD expand. Also threaded `maxRow`
  into `handleSelectB`/`initializeSelectionForScope` so SCREEN-scope "select all" covers the whole
  song (0..255), not just the visible 16 rows. (Continuous 60 fps redraw — review 6.1 — means the
  scrolled view repaints without an explicit invalidation.)

**Re-test focus (Round 2):**
1. **Load large render as sample (R2.1):** render a 3–4 min song (29 MB+ stereo WAV), then load it
   into an instrument and preview it from the file browser — no OOM, plays correctly. Also load
   several large samples in one session (watch for cumulative heap pressure). Normal small WAV
   loads (16/24/32-bit, mono + stereo) still load and play at correct pitch/length.
2. **Stereo START/END preview (R2.2):** load a **stereo** sample, open the sample editor, make a
   selection, press START — preview plays **only the selection** in every SOURCE mode (LEFT, RIGHT,
   STEREO, MONO). Repeat on a mono sample (should still work). After preview, the instrument's
   real START/END are restored (no stuck slice on the instrument screen).
3. **Song selection scroll (R2.3):** on SONG, L+B to start a selection, hold DPAD-DOWN past row 15 —
   the window scrolls and the selection extends (try down to ~row 100 and back up). Cut / copy /
   paste / delete operate on the full multi-screen selection. SCREEN-scope (cycle L+B to ALL)
   selects the whole song. PHRASE/CHAIN/TABLE selection unchanged.

### Batch 3 — low-risk performance + cleanup (2026-06-14) — 🔧 awaiting test

Mechanical perf/memory wins for the 1 GB Miyoo Flip — no real-time mix-loop restructure and no
behavioral change. (The structural 6.1 idle-redraw and the audio-hot-path items 1.2/1.3/1.10 are
deliberately deferred to their own batches so each can be device-tested in isolation.)

- **3.2 🔧 PlaybackController per-step/per-note logging gated** (`PlaybackController.kt`)
  The companion `const val TRACE = false` existed but **no call site referenced it** — all 53
  `logger.d` ran ungated, building strings every note/step during playback (the per-note volume-chain
  log alone did two `"%.4f".format()`). Wrapped the 42 per-step/per-note/per-schedule calls in
  `if (TRACE)`; since `TRACE` is a compile-time `const false`, the whole statement (string building
  included) is dead-code-eliminated. The 10 lifecycle logs (play / stop / data-changed / phrase·chain·
  song init) stay ungated. Flip `TRACE = true` for note-by-note debugging.

- **6.3 🔧 Hex-string caches** (`EditorHelpers.kt`)
  `Int.toHex2()` (called for every visible hex cell every frame) allocated 3 short-lived strings per
  call — the documented Snapdragon RenderThread GC-pressure vector. Now `HEX2_CACHE` (256 entries) +
  `HEX1_CACHE` (16) are precomputed once; both extensions index the cache → allocation-free. No call
  sites change. (Core `TrackerData.toHex2` left alone — used at construction, not per frame.)

- **6.2 🔧 Array-indexed font lookup** (`BitmapFont5x5.kt`, `PixelPerfectRenderer.kt`)
  `drawBitmapChar` did `FONT_5X5[char]` — a `Map<Char,ByteArray>` hash + Char boxing per glyph
  (~700+ glyphs/frame). New `FONT_5X5_ASCII: Array<ByteArray?>` (0..127, uppercase fallback baked in)
  is indexed directly by `char.code`; non-ASCII (arrows ↑↓←→, code >127) still uses the map. The
  glyph-atlas (6.2 "real win") is a bigger change, deferred. Visual output is identical.

- **6.4 🔧 Removed per-recomposition `Log.d`** (`PixelPerfectRenderer.kt`)
  The FILE_BROWSER `Log.d` (with string interpolation) ran on every recomposition while the browser
  was open. Removed; the `Log` import stays (still used by a `Log.e` null-guard).

- **1.9 🔧 Cached the KissFFT config** (`audio-engine.cpp`)
  `computeSpectrumFFT` did `kiss_fftr_alloc` + `kiss_fftr_free` per call (malloc + twiddle-table trig
  init at ~20 fps with the EQ open). Now a function-local `static kiss_fftr_cfg` (FFT size is constant;
  all callers are the single UI poll thread, so C++11 once-init is safe; lives for the process).

- **1.7 🔧 Sample-rate fallback 48000 → 44100** (`audio-engine.cpp`)
  `getSampleRate()` returned 48000 when the stream was null while every other fallback uses 44100 — an
  ~8.8% rate/pitch error if Kotlin read it before the stream opened. Now 44100, consistent.

- **7.4 🔧 Merged the three `buildFeatures {}` blocks** (`build.gradle.kts`) into one
  (`prefab` + `compose` + `buildConfig`). Cosmetic.

**Test focus for Batch 3:**
1. **Playback regression:** phrase/chain/song playback, retrig/arp/CHA/RND/groove/pitch FX all behave
   exactly as before (logging is the only change — verify nothing audibly differs). Optionally flip
   `TRACE = true` once and confirm the per-step logs reappear in logcat.
2. **Rendering:** all screens render identically — phrase grid hex values, instrument/sample/mixer
   text, file browser, and the arrow glyphs (↑↓←→) in keyboard/hints still draw correctly.
3. **EQ spectrum:** open the EQ editor, confirm the analyzer still animates correctly (FFT cfg cache).
4. **General:** app builds for release (one `buildFeatures` block); audio pitch correct at startup.
