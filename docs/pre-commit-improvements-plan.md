# Pre-Commit Improvements — Implementation Plan

Drafted 2026-06-21. Source: developer wishlist (round 2). Grounded in the current codebase
(file:line refs below). Each item: **Goal → Current state → Changes → Ripple/risk → Decision → Effort**.
Effort: **S**/**M**/**L**. Open decisions marked 🟡 (need a call before coding that section).

> Numbering follows the developer's list (there is no item 11).
> Some sub-items the developer already started — flagged **[partly done]** (uncommitted in the working tree:
> `FileBrowserModule`, `InstrumentModule`, `InstrumentPoolModule`, `ProjectModule`).

**Decisions locked (2026-06-21):**
- **#1** — MP3 (MediaCodec) **+ 8-bit PCM WAV**, both this round.
- **#9 LOOP END** — ships as a **real audio feature** with release-tail semantics (see item 9).
- **#13a GAIN** — remap storage to **0–240 = −12.0…+12.0 dB** (exact 0.1 dB steps).
- **#13b FREQ** — **keep 00–FF storage**; only make stepping land on the next distinct *displayed* value
  (no storage/C++ change; ~2.7 % resolution accepted).

---

## Build progress — 2026-06-21

On branch `code-review-3-round2`, each item device-tested before its commit. Build order so far:
**10 → 5 → 12 → 8 → 6 → 7 → 2 → 4 → 1.** Remaining: **9, 13, 3.**

- ✅ **#10 File browser** (`b3e161f`) — date `YY-MM-DD → DD-MM-YY`; shared `FileBrowserModule.clipName()`:
  list names clip to 20 (18+`..`), DELETE prompt to 16 so it stays inside the 640px line.
- ✅ **#5 " >" affordance** (`dec6add`) — generalised past the plan into a shared
  `EditorHelpers.drawEqCell()` so every EQ cell renders identically (value `--`=textEmpty / hex=textValue /
  focused=textCursor, bare `>` that never dims). Wired on instrument, instrument pool (arrow on the
  selected row only; value columns nudged 6px left), mixer master, effects reverb/delay input, and
  sample-editor EQ. SETTINGS `>` / EDIT `>` were the developer's own edits, folded in.
- ✅ **#12 Selection cursor** (`0997460`) — `handleDPadNavigation` moves the cursor with `selectionEnd`
  on PHRASE/CHAIN/SONG (guarded on the edge actually moving); SONG scroll follows the cursor. Copy/cut/
  delete still read the selection bounds and paste re-anchors at the cursor, so behaviour is unchanged.
- ✅ **#8 Sample-editor SNAP** (`9fd3145`) — `findZeroCrossing` gains a `dir` param (forward / backward /
  nearest). A directional search seeded from the already-stepped frame always lands at or past it, so the
  marker can't snap back and stick (worst at 1× zoom). Threaded through JNI → IAudioBackend / AudioEngine
  / OboeAudioBackend; the 4 marker-move handlers pass `dir = ±1`.
- ✅ **#6 AUDIO LOADING screen** (`76042b1`) — removed; the UI renders immediately while the Oboe stream
  opens on an IO thread. `AudioEngine.isReady` (`@Volatile`, set after `backend.create()`) gates the
  visualizer poll methods and START/playback so nothing touches the engine before it exists.
- ✅ **#7 A+B = reset to default** (`556edf1`) — `CursorContext.defaultValue` + `NO_DEFAULT`;
  `handleABCombo` returns `SET_VALUE(default)` for non-deletable cells, reusing each module's existing
  SET_VALUE handler. ~40 cells wired to their data-class defaults (instrument / phrase VOL / mixer / pool
  / effects / mods / chain transpose). Deletable cells unchanged. Chain transpose confirmed
  two's-complement (`00` = no transpose); a stale dead `0x80` display line was removed. **Deferred:**
  EQ band FREQ/GAIN/Q (→ #13), delay TIME sync-mode, groove / settings / sample-editor params.
- ✅ **#2 Visualizers** (`a6495e6`) — enum trimmed to `SCOPE/FLAT/OCTA/OCTA_FULL/SPECTRUM/SPECTRUM_PEAKS`
  (dropped BARS/PEAKS/MIRROR + `drawMirror` + `waveformToBarAmps`; `drawBarAmps` stays for the SPECT modes).
  New **OCTA_FULL** ("OCTA.F") forces all 8 lanes on (mask `0xFF`, no preview lane). **SCOPE** restyled to
  the OCTA pixel-dot wave via a shared `drawWaveDots`. **OCTA gap colour:** the strip fills `t.background`
  and each lane paints its own `t.vizBackground` panel, so gaps read as background. **SPECT release fix:**
  the ticker keeps redrawing for `SPECTRUM_RELEASE_FRAMES` (75 ≈ 1.2 s) after audio stops so the bars +
  peak dots fall to silence on every screen. Old themes naming a removed mode coerce to SCOPE
  (`coerceInputValues = true` on both `AppTheme` decode sites). Cursor-cycle range auto-derives from
  `values().size`; docs (manual + dev-status) updated.
- ✅ **#4 Qwerty overlay** (`d89f6ac`) — letters + symbols are now three equal **10-wide** rows on a fixed
  10-col grid (`innerX + col*cellW`), so DPAD up/down stays in one column. New virtual **ABORT/APPLY action
  row** under SPACE: A selects (col 0 abort = SELECT, col 1 apply = `handleStart()` reused, no dup); labels
  show the `(SEL)`/`(START)` bindings and the physical buttons still work. Cursor model gains
  `actionRowIndex` / `isOnActionRow` / `totalRows` / `currentRowCols`; nav wraps over `totalRows` and clamps
  the 2-button row; `insertCurrentKey` guarded so the action row never types. Symbols row 3 gained `,` to
  reach 10 (every key verified against `BitmapFont5x5`; `;` had no glyph). Box grew 195→228 px to fit the row.
- ✅ **#1 MP3 + 8-bit WAV** (`2d1734d`) — **MP3** decodes via the existing MediaCodec extractor straight
  into the slot (`AudioEngine.loadSampleData` sets the sample-rate ratio so 48 kHz files stay in tune) —
  the plan's literal "loadSampleStereo, skip the WAV parser" path. **No WAV is written**; the instrument
  keeps the `.mp3` path, so it's re-decoded on every open. Wired into load (both confirm branches,
  backgrounded on `Dispatchers.Default`), preview, `reloadProjectSamples`, autosave recovery and `.pti`
  `loadPreset` (now takes the extractor); sample editor + WAV export reuse the in-engine buffer untouched.
  A WAV-materialising route (reusing the video extract→WAV flow) was built first, then reverted per the
  developer's **"in-memory, no file"** choice. Length **cap removed** (`maxDurationSec = 0`) for the
  testing stage so real memory limits can be probed — video-extract paths keep their 60 s/30 s caps; huge
  MP3s may OOM for now. **8-bit WAV:** `decodeWavSample` unsigned 8-bit branch (center 128) + `isPcm` gate,
  making the manual's pre-existing "8-bit" claim real. File browser now surfaces the real decode error.
  - **Follow-up `945e084`** — removing the cap (`maxDurationSec = 0`) exposed a latent overflow: with no
    cap `maxSamples = Int.MAX_VALUE` and `maxSamples * channelCount` wrapped negative, so *every* MP3
    decoded to empty. Fixed by computing the interleaved-sample limit as a `Long`.
  - **Follow-up `5845331` (streaming load)** — the whole-file decode capped loads at ~5–7 MB and left the
    JVM heap inflated (boxed-`Short` accumulator). Reworked to **stream straight into native memory**:
    new C++ `beginSampleLoad`/`fillSampleChunk`/`finalizeSampleLoad`/`cancelSampleLoad` (slot allocated
    from the duration estimate, filled in place, length 0 until finalize so the audio thread can't race),
    `IVideoAudioExtractor.extractAudioToSink` + `PcmSink` (parallel to `decode()`; video/preview untouched),
    `AudioEngine.loadSampleCompressed` (drives the sink, sets the rate ratio, falls back to the whole-file
    decode when there's no duration metadata). MP3 load/reload/autosave/preset all route through it. PCM now
    lives in native memory (freed on clear / new project); Java heap holds one block. Device-tested OK.
  - **Known rough edges / future work (developer: "a bit quirky", revisit later):** no final size cap yet
    (a very long MP3 still allocates a large native buffer — pick a sane cap once limits are characterised);
    **preview** still uses the whole-file boxed path (`extractAudio`), so previewing a huge MP3 still bloats
    the heap transiently; the no-duration fallback also uses the boxed path; duration-estimate clamp can drop
    a few ms of tail and encoder priming samples aren't skipped (minor leading artifact). Characterise the
    exact quirk before the next pass.

---

## 1. Sampler: read MP3 + audit WAV format coverage  **✅ shipped (2d1734d)**

**Goal:** Load `.mp3` files as samples; document/extend which WAV encodings are supported.

**Current state**
- WAV decode is **native**: `audio-engine.cpp` `decodeWavSample` (L207) + `loadSampleFromWavFile`
  (L237-373), reached via `OboeAudioBackend.loadSampleFromWav(id, path)` (`OboeAudioBackend.kt:65`)
  from `InstrumentController` (~L174). The old Kotlin `parseWavBuffer` is dead (REVIEW-3).
- **Supported WAV today:** PCM **16 / 24 / 32-bit**, IEEE **float 32-bit**, `WAVE_FORMAT_EXTENSIBLE`
  (0xFFFE) resolving to PCM/float; **mono or stereo only** (`channels 1..2`, L281).
- **NOT supported:** **8-bit PCM**, >2 channels, ADPCM, A-law/µ-law, and any compressed WAV.
  (DOCS-REVIEW.md:21 already flags the docs' false "8-bit" claim.)
- An Android decode path already exists: `AndroidVideoAudioExtractor` (MediaExtractor + MediaCodec,
  decodes to PCM float) behind `IVideoAudioExtractor` — used for video→audio extraction. MP3 is a
  first-class MediaCodec format, so this is the reuse target.

**Changes**
- **MP3:** generalize the extractor. Either (a) rename/extend `IVideoAudioExtractor` to
  "decode any container/codec to PCM" (it already does exactly this — the "video" name is the only
  thing MP3-specific about it), or (b) add a thin `decodeCompressedToPcm(path)` sibling. Route the
  sample-load path: when the chosen file is `.mp3`, decode to float L/R via MediaCodec →
  `loadSampleStereo`/`loadSample` (skip the native WAV parser). Mirror the WAV sample-rate
  compensation already applied downstream.
- Add `mp3` to the sample browser filter — `handleConfirmAInstrument` (`AppInputDispatcher.kt:1463`)
  and `handleConfirmAInstrumentPool` (`:1418`) currently use `listOf("wav") + VIDEO_EXTENSIONS`.
- **OOM guard:** MediaCodec decodes whole-file into RAM. Reuse the streaming/heap caution from
  REVIEW-2/3 (decode in chunks straight to the native sample buffer if feasible; at minimum cap +
  fail gracefully on huge files, like the WAV path does).
- **WAV 8-bit (optional):** add the `bitsPerSample==8` branch to `decodeWavSample` (8-bit WAV is
  **unsigned**, center 128 → `(p[0]-128)/128f`) + allow it in the `isPcm` gate (L289). Small, closes
  the documented gap.

**Ripple / risk**
- Keep `core/logic` Android-free: MP3 decode is an Android impl behind the existing interface
  (platform/android), called from the controller via the interface — same pattern as video.
- Decoded MP3 has no loop/cue metadata; slice markers simply stay empty (fine).

**Decision** ✅ MP3 via the existing MediaCodec extractor **+ 8-bit PCM WAV branch**, both this round.

**Effort: M** (MP3 wiring), **S** (8-bit WAV).

---

## 2. Visualizers: prune, add, restyle, fix spectrum release  **✅ shipped (a6495e6)**

**Goal:** Drop MIRROR/BARS/PEAKS; add OCTA FULL; restyle SCOPE + OCTA gaps for a ProTracker look;
make SPECT / SPECT.P decay smoothly after stop on every screen.

**Current state**
- `enum class VisualizerType { SCOPE, BARS, PEAKS, MIRROR, FLAT, OCTA, SPECTRUM, SPECTRUM_PEAKS }`
  (`AppTheme.kt:6`); default SCOPE (`:48`).
- Renderers in `OscilloscopeModule.kt`: `drawScope` (continuous `Path` stroke, L80), `drawBarAmps`
  (BARS/PEAKS, L113), `drawMirror` (L171), `drawOcta` (pixel-dot quadrascope, L212). Whole module is
  filled `vizBackground` first (L62-66) — so OCTA's inter-scope gaps are `vizBackground`.
- OCTA active tracks come from `phraseTrackMask` (+preview lane) — `PixelPerfectRenderer.kt:609-612`.
- **SPECT freeze:** the redraw ticker (`PixelPerfectRenderer.kt:241-263`) bumps `oscilloscopeTicker`
  (→ redraw) only while `audible` = `isPlaying || waveformBuffer has signal` (L249-250). The spectrum
  bars' smoothing (`barSmoothed *= BAR_DECAY`) lives in the **module draw** (`OscilloscopeModule.kt:127`),
  so once the master waveform decays the ticker stops and the bars **freeze mid-fall**, only updating on
  the next unrelated redraw. MIXER looks smooth only because it has its own unconditional 60 ms
  `stateVersion++` loop (`MainActivity.kt:831-844`).

**Changes**
- **Enum:** → `{ SCOPE, FLAT, OCTA, OCTA_FULL, SPECTRUM, SPECTRUM_PEAKS }`. Remove the BARS/PEAKS/MIRROR
  draw paths + `waveformToBarAmps` + the now-unused PEAKS/bar state fields.
- **OCTA FULL:** new arm calling `drawOcta` with all 8 lanes forced on (mask `0xFF`) regardless of
  `phraseTrackMask`; add the `isOctaFull` branch to the `activeTrackMask` calc in
  `PixelPerfectRenderer.kt:609`.
- **Gap color (ProTracker):** for OCTA/OCTA_FULL, fill the module with `t.background` first, then draw
  each track scope's own panel rect in `t.vizBackground`; the gaps are now `background`.
- **SCOPE restyle:** replace the continuous-path stroke with the OCTA pixel-dot loop (integer-quantized
  y, one `drawRect` dot per x) so the single scope matches the OCTA wave look.
- **SPECT release fix:** in the ticker, when vizType is SPECTRUM/SPECTRUM_PEAKS, keep bumping the ticker
  for a short **release tail** after audio goes quiet — either gate on `spectrumBuffer` energy, or run
  a fixed ~1 s countdown (≈65 frames at the 0.90 decay reaches silence) after the active→idle edge —
  then stop. (OCTA decays via the per-track waveform ring, which the existing gate already covers; SCOPE
  reads the master waveform directly, also covered.)
- **Settings + persistence:** the VISUALIZER row in `SettingsModule` cycles this enum — update its list.
  Theme `.ptt` save/load preserves visualizer type (dev-status:389): on load, **map removed types →
  SCOPE** so old themes don't crash.

**Ripple / risk**
- Removing enum entries breaks any `when` that's exhaustive on `VisualizerType` — grep + fix (module,
  renderer flags, settings). Manual (`manual-en.md:115-118`) lists MIRROR/BARS/PEAKS — update.

**Decision** ✅ none blocking. (Confirm OCTA FULL shows empty center-lines for silent tracks — yes,
`drawOcta` already draws a center line per lane.)

**Effort: M**

---

## 3. Controls consistency — A opens sub-screens, B goes back (press-vs-release)

**Goal:** Make **A = open / B = back** consistent for the overlay-opening cells, without losing the
A+DPAD (edit) and B+DPAD (preset scroll) combos that share those buttons. Mechanism: a single A/B
press is only "complete" on **release** when no other button was pressed while it was held.

**Current state**
- Single A fires on **PRESS** (`ButtonHandlers.kt:665`); single B on press (`:671`). A release hook
  already exists: `onAReleased()` (`:411`). A+DPAD/A+B and B+DPAD combos fire on the DPAD/B press while
  A/B is held (`:433-499`).
- Overlays currently open on **SELECT**: project name (`AppInputDispatcher.kt:1878`), instrument name
  (`:1944`), EQ editor (`openEqEditor` `:409`, invoked from `handleSelect`); EQ closes on SELECT
  (`:1867`). EQ A+DPAD edits bands (`:2220+`), B+DPAD cycles presets (`:2376/2395`).

**Changes** (generic deferral, module-driven predicate)
- Add `onBReleased()` to `ButtonHandlers` (mirror `onAReleased`).
- In `InputMapper`, track `aPressedAlone` / `bPressedAlone` (set on press, cleared the moment an
  A+combo / B+combo fires). Add two predicates supplied by the dispatcher:
  `deferAToRelease()` / `deferBToRelease()`. On A/B **press**, if the predicate is true, **do not** fire
  the single action; on **release**, if `*PressedAlone` still holds, fire it then.
- Dispatcher wires the predicates from existing state it already reads:
  - `deferAToRelease` → true when the cursor cell "opens a sub-screen on A" (EQ-value cells; project/
    instrument NAME cells). Expose via a new `CursorContext` flag (e.g. `opensSubScreenOnA`) set in the
    relevant `getCursorContext`s, plus the NAME cells.
  - `deferBToRelease` → true when `eqEditorState.isOpen` (B = close).
- Re-point the open actions from SELECT to the deferred-A path (keep SELECT working too, as an alias):
  EQ value → `openEqEditor`; project name → qwerty PROJECT_NAME; instrument name → qwerty
  INSTRUMENT_NAME. EQ close → deferred-B (keep SELECT alias).

**Ripple / risk**
- The qwerty overlay keeps **SELECT=abort / START=apply** (B is "delete char", A is "type char" inside
  it), so the deferral applies only to *opening* it, not while it's open. (See item 4.)
- A,A double-tap (`:653`) is for "insert next unused" on grid cells — those cells are **not** defer
  cells, so no conflict. Verify the tiny added open-latency (you hold A → release) feels right; it's the
  intended hold-to-edit / tap-to-open behavior.

**Decision** ✅ approach locked by developer; only the `CursorContext` flag name is cosmetic.

**Effort: M** (touches InputMapper, ButtonHandlers, CursorContext, several dispatcher sites — test every
A/B on every screen for regressions).

---

## 4. Qwerty overlay — column-aligned layout + on-screen ABORT/APPLY  **✅ shipped (d89f6ac)**

**Goal:** Column-aligned keyboard (no diagonal drift) and visible APPLY/ABORT buttons.

**Current state**
- Layout data: `QwertyKeyboardOverlay.kt:28-41` — letters 10/9/7 + space; separate symbols layout.
  Render: `PixelPerfectRenderer.kt:1153 drawQwertyKeyboard` (SPACE at L1271, key cursor L1288).
  Nav helpers (`moveCursorLeft/Right/Up/Down`, `withClampedCol`) wrap per-row by row size.

**Changes**
- **Layout** → three equal 10-wide rows so columns line up:
  ```
  Q W E R T Y U I O P
  A S D F G H J K L "
  Z X C V B N M - _ /
  ```
  Update `QWERTY_ROWS_LETTERS`; give the symbols layout the same 10-wide shape for consistency.
- **Render:** position keys on a fixed 10-column grid (`col * cellW`), not centered-per-row, so vertical
  DPAD travel stays in one column. `withClampedCol` still guards ragged rows (space/symbol rows).
- **ABORT / APPLY:** add a row under SPACE with two buttons — `ABORT(SEL)` and `APPLY(START)` — selectable
  with A (so single-A on them cancels/applies); keep physical SELECT=abort / START=apply working and label
  them so the binding is discoverable. Extend the keyboard cursor model to include this row.

**Ripple / risk**
- Keep `maxLength`/insert-mode logic intact. The space/ABORT/APPLY rows are special-cased in nav +
  `currentKey()`/`insertCurrentKey()` (they don't insert a char).

**Decision** ✅ layout given by developer.

**Effort: M**

---

## 5. " >" sub-screen affordance on EQ cells  **✅ shipped (dec6add)**

**Goal:** Show " >" where a cell opens a further screen (consistent with the new A-opens convention).

**Current state**
- Developer already added " >" to **SETTINGS** (`ProjectModule.kt`) and **EDIT** (`InstrumentModule.kt:455`),
  uncommitted.
- Not yet: instrument-screen **EQ** value, instrument-pool **EQ** column.

**Changes**
- **Instrument EQ value:** append " >". Color rule: the numeric part is `t.textParam` when empty (`--`)
  and `t.textValue` when set (`00-7F`); the " >" is **always** `t.textValue`. (Render lives in the EQ row
  — which moves to row 12 under item 9, so do this together.)
- **Pool EQ column** (`InstrumentPoolModule.kt`): show " >" **only on the cursor's row**; other rows show
  the bare EQ value.

**Ripple / risk** — purely cosmetic; mind the column-width math so " >" doesn't overflow into the next
column on the pool's tight mixer strip.

**Effort: S**

---

## 6. Remove the "AUDIO LOADING…" startup screen  **✅ shipped (76042b1)**

**Goal:** No loading screen (it lingers on the Miyoo Flip).

**Current state**
- `MainActivity.kt:1089-1101`: `if (!audioReady) { …"AUDIO LOADING…"… return }`. `audioReady` flips true
  (`:303`) after `audioEngine.create()` runs on `Dispatchers.IO` (`:296-304`). It was added because
  AAudio's first open can take many seconds on GammaCoreOS.
- Audio-dependent effects are already keyed on `audioReady` (volume sync `:386`, template/recovery
  loads `:892/:921`), so they wait correctly regardless of this screen.

**Changes**
- Delete the early-return block; render the tracker UI immediately. Modules are pure draw (no audio at
  draw time), so the UI is safe before the stream opens.
- Guard the few **input** paths that need the engine until `audioReady`: START/playback and instrument
  preview (no-op or a brief status if pressed before ready). The backend setters are already null-safe.

**Ripple / risk**
- Low. Pressing START in the first ~1 s does nothing until ready — acceptable, and better than a black
  screen. Keep the IO-thread `create()` exactly as-is.

**Decision** 🟡 — drop the screen entirely (recommended), or replace with an instant non-blocking hint?

**Effort: S**

---

## 7. A+B = reset value to default  **✅ shipped (556edf1)**

**Goal:** A+B snaps a value back to its default. Today A+B = **delete/clear** (`InputController.handleABCombo`,
`:216`) which already does the right thing for cells with an "empty" default; this extends it to
continuous params that have a neutral default but no empty state.

**Current state**
- `handleABCombo` returns `DELETE` iff `capabilities.canDelete`, else `NONE`. So PAN/DRIVE/VOL etc.
  (no `canDelete`) do nothing on A+B today.
- A+B **in selection mode** = delete-selection (handled upstream in the dispatcher, before the context
  path) — must keep priority.

**Changes**
- Add `defaultValue: Int` to `CursorContext` (default = `emptyValue` for back-compat). Factory methods
  that need a real default take a `default` param.
- `handleABCombo`: if `canDelete` → `DELETE` (unchanged); else if a non-sentinel `defaultValue` exists →
  `SET_VALUE(defaultValue)`. Modules already coerce in `handleInput`, so no extra clamping.

**Proposed default table** (the values A+B should reset):

| Screen / cell | Default | Notes |
|---|---|---|
| Phrase **VOL** (velocity) | `7F` | max velocity (`PhraseStep.volume`) |
| Instrument **VOL** | `FF` | (`Instrument.volume`) |
| Instrument **PAN** | `80` | center |
| Instrument **DETUNE** | `80` | neutral |
| Instrument **DRIVE** | `00` | off |
| Instrument **CRUSH** / **DWNSMPL** | `0` | off (nibbles) |
| Instrument **FREQ** (filterCut) | `00` | |
| Instrument **RES** (filterRes) | `00` | |
| Instrument **START** / **LOOP ST** | `00` | |
| Instrument **END** (/ **LOOP END** if added) | `FF` | |
| Instrument **REV** / **DEL** sends | `00` | |
| Instrument **TIC** (tableTicRate) | `06` | |
| Instrument **ROOT** | `C-4` | already A+B-deletable to C-4 |
| EQ **FREQ** / **GAIN** / **Q** | `80` | 0 dB / mid (see item 13) |
| Mixer track **VOL** / master **VOL** | `FF` | |
| Mixer **PAN** | `80` | |
| Mixer/global sends, OTT/DUST/LIM | per-field | audit `MixerModule`/`EffectModule` defaults |
| Mod slot **AMT/ATK/HOLD/DEC/SUS/REL** | per-field | audit `ModulationModule` defaults |
| Cells with empty state (note, inst/phrase/chain ref, FX, EQ slot, table vol) | (unchanged) | A+B already clears to empty |

**Ripple / risk** — audit every `getCursorContext` to set `defaultValue`; anything missed simply keeps
today's behavior (no regression). Verify selection-mode A+B still deletes the selection.

**Decision** 🟡 — confirm the table above (especially FREQ/RES default `00` vs "open" `FF`).

**Effort: M** (mechanic is small; the audit across all modules is the work).

---

## 8. Sample editor — selection marker sticks at 1× zoom with SNAP on  **✅ shipped (9fd3145)**

**Goal:** A small marker move always advances even with SNAP on.

**Current state**
- Marker move (4 handlers): `AppInputDispatcher.kt:2224-2233` (A↑), `:2266` (A↓), `:2306` (A←), `:2330`
  (A→). Each does `raw = pos ± step` then `snapFrame(raw) = findZeroCrossing(id, raw)` when snap is on.
- `findZeroCrossing` (`sample-editor.cpp:628-642`) scans outward **both directions** and returns the
  **nearest** crossing (`searchRadius=512`). When the nearest crossing to `raw` lies **at or behind** the
  pre-move position (common in low-frequency regions / at 1× where the small step is tiny relative to the
  crossing spacing), the marker snaps back to where it started → "stuck". Bigger moves or zoom-in change
  `step` enough to clear the current crossing; SNAP off avoids it entirely.

**Changes**
- Make snapping **directional + progress-guaranteeing**. Add a direction to the search:
  `findZeroCrossing(id, frame, dir)` — `dir>0` scans only forward, `dir<0` only backward — and call it
  with the move direction. Then ensure net progress: if the snapped result doesn't move past the old
  position (no crossing within radius), fall back to the unsnapped `raw`.
- Wire through JNI (`native_findZeroCrossing`, `jni-bridge.cpp:1116`) + `OboeAudioBackend` +
  `AudioEngine`; update the 4 `snapFrame` closures to pass direction.

**Ripple / risk** — keep the old nearest-search signature for any other caller (default `dir=0`). Pure
sample-editor UX fix; no audio-path impact.

**Effort: S–M**

---

## 9. Instrument screen (sampler) — row reorganization

**Goal:** Regroup sampler rows; add SLICE to the VOL row and a new LOOP END row.

**Current state** (actual draw, `InstrumentModule.kt:99-267` — the `InstrumentState` doc block L766-772
is **stale**, ignore it):
```
0 TYPE/LOAD/SAVE · 1 NAME · 2 ROOT/DET/TIC · 3 VOL/PAN · 4 — · 5 SMPL/LOAD/EDIT> · 6 — ·
7 DRIVE/FILTER · 8 CRUSH/FREQ · 9 DWNSMPL/RES · 10 — ·
11 START/REV · 12 END/DEL · 13 REVERSE/EQ · 14 LOOP/SLICE · 15 LOOP ST
```

**Target** (sampler):
```
0 TYPE/LOAD/SAVE · 1 NAME · 2 ROOT/DET/TIC · 3 VOL/SLICE/PAN(triple) · 4 — · 5 SMPL/LOAD/EDIT> · 6 — ·
7 DRIVE/FILTER · 8 CRUSH/FREQ · 9 DWNSMPL/RES · 10 — ·
11 REV/DEL · 12 EQ(>) · 13 START/END · 14 LOOP/REVERSE · 15 LOOP ST/LOOP END
```
Moves: SLICE → row 3 (becomes a triple like ROOT/DET/TIC); REV+DEL → 11; EQ alone → 12; START+END → 13;
LOOP+REVERSE → 14; LOOP ST + **LOOP END** → 15.

**Changes**
- Rewrite the sampler tail in `draw` (use `drawTripleParameterRow` for row 3; regroup 11-15).
- Update `getCursorContext` (`:465-568`) and `handleInput` (`:575-758`) row/col mapping to match.
- Update cursor nav in `TrackerController` (INSTRUMENT `moveCursorUp/Down`, spacer-skips, the
  dual/triple-row sets, `getInstrumentCursorLeft/RightColumn`).
- Update `handleConfirmAInstrument` (`AppInputDispatcher.kt:1424`) — row indices for LOAD/EDIT (still
  row 5) and any moved EQ-open cell.
- Refresh the module's top doc comment + the stale `InstrumentState` doc.

**Ripple / risk** — this is the biggest regression surface: every hardcoded sampler row index moves. SF
layout is separate and unchanged unless we choose to mirror it. Test every row's cursor + A action.

### LOOP END — new audio feature (✅ locked)

`Instrument` today has only `loopStart` (`TrackerData.kt:430`); the loop runs `loopStart → sampleEnd`.
Add `loopEnd` with these **playback semantics** (developer spec):

- **Loop mode on, no ADSR:** play **START → LOOP END** once, then loop the region **[LOOP ST, LOOP END]**
  indefinitely. (`sampleEnd`/END no longer bounds the loop.)
- **Loop mode on + ADSR modulation:** same looping during the held/sustained note; on **note-off / KIL**
  (the existing soft-kill release path), **stop looping and play LOOP END → END** as the release tail,
  under the ADSR release envelope. So the region **[LOOP END, END] is reserved for the ADSR tail.**

Work involved:
- `Instrument.loopEnd` field (`var loopEnd: Int = 0xFF`) + serialization (dev pre-converts old projects).
- C++ sampler-voice loop logic: loop bounds become `[loopStart, loopEnd]`; first pass START→loopEnd;
  wrap at loopEnd back to loopStart (forward + ping-pong).
- Tie into the existing ADSR/TRIG soft-kill (`scheduleNoteOff` → `triggerNoteOff`, per memory): on
  release, switch the voice out of loop mode and let it run loopEnd→sampleEnd while the release envelope
  applies, then stop. Plain (non-ADSR) KIL/note-end behavior unchanged otherwise.
- `InstrumentController.updateLoopEnd`, cursor context/handleInput for the new row-15 cell, A+B default
  `FF` (item 7).

**Effort: L** (UI reorg is **M**; LOOP END audio behavior + ADSR-tail integration is the bulk).

---

## 10. File browser — filename truncation  **✅ shipped (b3e161f)**

**Goal:** Long names truncate instead of running under the size/date columns.

**Current state**
- `FileBrowserModule.kt:454` draws `item.displayName.take(30)` at `x+30`; size at `x+370`, date at
  `x+480`. At fontScale 3 (17 px/char) 30 chars reaches ~x+540 → overlaps size + date.
- Date format already inverted to `dd-MM-yy` (`:72`, uncommitted).

**Changes**
- Truncate to ~20 chars with an ellipsis marker, matching the instrument pool's NAME treatment:
  `if (name.length > 20) name.take(18) + ".." else name`. 20×17 ≈ 340 px (ends ~x+370, clear of size).

**Ripple / risk** — none; verify against the pool's exact cutoff so they look identical.

**Effort: S**

---

## 12. Copy/paste — cursor follows the selection edge  **✅ shipped (0997460)**

**Goal:** In selection mode the cursor moves with the growing edge (so after exiting, the cursor is still
on-screen — fixes the SONG "jump back" past row 16).

**Current state**
- `AppInputDispatcher.handleDPadNavigation` (`:907-935`): in selection mode it calls
  `inputController.expandSelection(...)` (`:923`) but leaves `cursorRow/Column` anchored; SONG papers
  over it with `scrollSongToRow(selectionEnd.row)` (`:924-929`).

**Changes**
- After `expandSelection`, set the active cursor to `selectionEnd` (per-screen cursor: PHRASE/CHAIN/SONG
  use `trackerController.cursorRow/cursorColumn`). The SONG window already follows the cursor, so the
  special-case scroll hack can go.

**Ripple / risk** — confirm copy/cut/paste read the **selection bounds** (`getSelectionBounds`), not the
cursor, so moving the cursor with the edge doesn't change what's copied. (It does — `ClipboardManager`
uses bounds.) Re-test all three screens.

**Effort: S**

---

## 13. Values — EQ FREQ/GAIN in real units; document time/freq params

### 13a. EQ GAIN — step in dB
- **Now:** `EqBand.gain` is `00-FF` → `-12..+12 dB` (`TrackerData.kt:351`; C++ `setEqBand`
  `sample-editor.cpp:649`: `(gainHex/255)*24-12`). 255 steps ⇒ **0.094 dB/step**, displayed at 0.1 dB
  (`EqModule.formatGainDb`) — so single steps sometimes don't change the shown value.
- **Plan (✅ locked — remap storage):** add a `GAIN` `CursorValueType` (unit = dB, small step **0.1**,
  large step **1.0**, range configurable; EQ uses −12..+12). Storage becomes **`0..240` = −12.0..+12.0 dB**
  so a small step is exactly 0.1 dB and a large step 1.0 dB. Default (A+B, item 7) = **120** (0 dB).
- **Ripple of the remap** — everything that reads `EqBand.gain` as `(gain-128)/128*12` must switch to
  `gain/10 - 12`:
  - C++ DSP: `setEqBand` (`sample-editor.cpp:649`).
  - EQ display: `EqModule.drawEditor` value (`EqModule.kt:267`) + viz curve math `bandGainDb`/
    `computeCombinedGainDb` (`:328,319`).
  - Context: gain cell `getCursorContext` (`EqModule.kt:296`) → `GAIN` type, `max=240`.
  - Dev pre-converts existing projects/templates (house rule; no migration code).

### 13b. EQ FREQ — the real discussion
- **Now:** `EqBand.freq` is `00-FF` → `20·1000^(h/255)` Hz (log) (`TrackerData.kt:350`; C++ `setEqBand`
  `:648`). The **fundamental limit:** 255 steps over 20 Hz–20 kHz ⇒ each step ≈ **×1.027 (≈2.7 %)** —
  about **27 Hz at 1 kHz, 270 Hz at 10 kHz**. So readable targets like "1.15 kHz" are **not
  representable** at the current resolution, and single steps already cross display values unevenly.
- **Implication:** to get fine, readable Hz stepping you must **widen `EqBand.freq` storage** (e.g. store
  actual Hz as an int, or 0–1023). That ripples to serialization (dev pre-converts) + C++ `setEqBand`
  signature (pass Hz directly instead of `freqHex/255`).
- **Plan (✅ locked — keep 00-FF):** no storage/C++ change. The only fix is **display-aware stepping**:
  because the kHz display (`formatFreqHz`, `EqModule.kt:474`) is coarser than one 2.7 % step near 1 kHz,
  A+UP/DOWN currently sometimes leaves the readout unchanged. Make the small step keep advancing the
  `0-255` value in the pressed direction until `formatFreqHz` produces a **different** string (bounded by
  min/max). Resolution stays ~2.7 % (≈27 Hz @1 kHz); exact values like 1.15 kHz remain unreachable — accepted.
- Implement as a small helper in the freq cell's step path (no new `CursorValueType` strictly required;
  optional thin `FREQ` type if it's cleaner than special-casing in `EqModule.handleInput`).

### 13c. Document time/freq params (manual)
Audit + explain the units/ranges, and consider a beat-sync option where natural:
- Delay **TIME** (00-FF) — confirm Hz range/step; reverb **SIZE** (ms?); **LIM** pre-gain (dB from?).
- Mod **ATK/HOLD/DEC/SUS/REL** (tics? steps?) and mod **FREQ** (Hz) — add a beat-sync mode (1/2, 1/8, …)
  like delay TIME if feasible.
- Filter **FREQ** (00-FF → which Hz curve).
Deliverable: a "Parameter reference" section in `manual-en.md` (+ `dsp-settings-guide.md` cross-refs).

**Decision** ✅ GAIN = remap to 0–240 (0.1 dB); FREQ = keep 00-FF with display-aware stepping. 13c is doc-only.

**Effort:** 13a **S** · 13b **S** · 13c **S**.

---

## Cross-cutting

- **CursorContext** grows two things this round: `opensSubScreenOnA` (item 3) and `defaultValue` (item 7);
  plus a `GAIN` value type (item 13a; FREQ keeps 0-255 with display-aware stepping). Land item 3 and item 7
  near each other.
- **Instrument** changes cluster in item 9 (rows) + item 5 (EQ " >") + item 7 (defaults) — do 9 first,
  then 5/7 on the settled rows.
- All `core/logic`/`core/data` edits stay Android-free (CLAUDE.md). MP3 decode stays in platform/android.
- Suggested order (low-risk first): **10 → 5 → 12 → 8 → 6 → 2 → 7 → 4 → 1 → 9 → 13 → 3**.
  (3 last — it's the deepest input-system change and easiest to regress.)
- Each ships as its own `[Feature]/[Fix]/[UI]` commit, device-tested before commit (project rule).
