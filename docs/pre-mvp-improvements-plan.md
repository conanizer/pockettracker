# Pre-MVP Improvements — Implementation Plan

Drafted 2026-06-19. Source: developer feature wishlist. **These ship before MVP.**
Status legend: **S**/**M**/**L** = effort.

This plan is grounded in the current codebase (file:line refs below). Each item lists
**Goal → Current state → Changes → Ripple/risk → Decisions → Effort**. Decisions marked
✅ are locked; 🟡 are still open.

**Decisions locked (2026-06-19):**
1. Instruments/tables/grooves: **actually shrink arrays to 128**. Mods are per-instrument, so
   they follow. EQ already 0–127.
2. Velocity: **only the phrase VOL column** → 0–127; mixer + instrument VOL stay 0–255.
   Sampler velocity curve: **squared** `(vel/127)²`. **Vxx effect → wired to the
   instrument-volume stage** (stays 0–255), decoupled from the velocity column.
4. Out of empty slots → **abort + status message**.
7. Reorder ships in **v2**; v1 = view/edit/load/preview.
- **No migration code needed** — developer pre-converts old projects.

Recommended order (low-risk wins first, then the two big ones):
**5 → 4 → 2 → 1 → 3 → 7**, with **6 (MIDI audit)** done as analysis to steer 1/2/6.

**Status (2026-06-20): ALL ITEMS SHIPPED.** #1–#5 done; #7 v1 done (reorder deferred to v2);
#6 audit delivered as `docs/midi-out-readiness.md` (the MIDI-out *feature* itself is post-MVP).

---

## 1. Reduce instrument / table / groove range FF → 7F (resize arrays to 128) ✅

**Goal:** 128 slots (0x00–0x7F) for instruments, tables, grooves — align with MIDI (instrument =
MIDI program 0–127) and trim memory. **Mods are per-instrument so they follow automatically.**

**Current state**
- `Project` allocates `Array(256)` for phrases, chains, instruments, tables, grooves —
  `TrackerData.kt:587-612`.
- **Mods** live on each `Instrument` (`modSlots: Array(4)`), not a separate pool — so 256
  instruments = 256 mod-sets. Reducing instruments to 128 reduces mods to 128 for free.
- Selectable ranges come from `CursorContextFactory`: `instrument()` and `volume()` both
  delegate to `hexByte()` → `maxValue = 255` (`CursorContext.kt:198,239`). Table/groove refs
  use the same hexByte/`-1`-empty pattern.
- EQ slots are **already** 0–127 (`MixerModule.getCursorContext` master EQ `max = 127`) — no change.

**Changes** (decision ✅ locked: resize, not cap-only)
- Shrink backing arrays `Array(256)` → `Array(128)` for **instruments, tables, grooves**
  (`TrackerData.kt:597-612`). Leave **phrases/chains at 256** (not in scope).
- Cap the **selectable range** to `0x7F` for: instrument refs (phrase INST column + pool),
  table refs (`FX_TBL`, instrument `tableId`), groove refs (`FX_GRV`). `CursorContextFactory`
  max changes + per-module contexts that pass an explicit max.

**Ripple / risk**
- No migration code (developer pre-converts projects). A new project can never create a ref
  >127 once the UI is capped, so no out-of-bounds in normal use. Keep index access defensive
  (`getOrNull`/`getOrElse`, already the codebase style) so a stray ref can't crash.
- C++ side: `sfEnvOverrides[256]`, `instrumentParams[...]`, table upload assume 256 — keep the
  C++ bounds at 256 (harmless headroom); only the Kotlin arrays + UI cap need to change.
- `instrument: Int = 0x00` default fine; transpose two's-complement helper
  (`TrackerData.kt:24-29`) is unrelated.

**Effort: M**

---

## 2. Phrase volume 255 → 127, treated as MIDI velocity

**Goal:** Phrase-screen volume column becomes a 0–127 MIDI velocity.

**Current state**
- `PhraseStep.volume: Int = 0xFF` where **FF = max**, not an "empty" sentinel — every step
  always carries a volume (`TrackerData.kt:82`).
- Phrase VOL column context: `CursorContextFactory.volume(step.volume)` → hexByte max 255
  (`PhraseEditorModule.kt:244`, `CursorContext.kt:232`).
- `VolumeUtils.hexToFloat = (hex and 0xFF)/255f` (`TrackerData.kt:485`) — **shared** by
  instrument/track/master gain stages, so it cannot simply switch to /127.

**Changes** (scope ✅ locked: phrase VOL column only; mixer + instrument VOL stay 0–255)
- `volume()` context → `maxValue = 127`. New default `0x7F`. This column is now **velocity**.
- Add a dedicated **velocity→gain** conversion for the step-volume path (squared, below);
  **leave `hexToFloat` (/255) untouched** for instrument/track/master gain stages.
- **No migration code** — developer pre-converts projects.

**Velocity curve** (sampler path) — ✅ **squared** `gain = (vel/127)²`:
- GM/SoundFont-style, more musical contrast than linear (each halving of velocity ≈ −12 dB).
- **SF2 voices:** pass raw `0–127` to `tsf_channel_note_on` — TSF **already applies the
  SoundFont's own dB velocity curve**, so don't double-curve them. Only the **sampler** C++
  voices get the squared curve applied by us; squared ≈ SF2 feel so the two stay consistent.

**Vxx (`FX_VOLUME`) effect** — ✅ **wired to the instrument-volume stage, not velocity**:
- Stays **0–255** (do **not** cap to 127). The VOL column is velocity; Vxx is a separate
  volume-automation control in the instrument-volume gain stage, so the two are independent and
  don't clobber each other.
- Current code routes Vxx into the **phrase-volume** stage: `FX_VOLUME -> { volume = value/255f;
  volumeFromVxx = true }` (`EffectProcessor.kt:149-151`), and on an empty step it calls
  `audioEngine.scheduleTrackPhraseVol(...)` (`PlaybackController.kt:1195-1197`).
- **Retarget** Vxx to the instrument-volume component instead: the note's final gain becomes
  `velocityGain(step.volume, squared) × instrumentVol(possibly overridden by Vxx) × track ×
  master`. Split the note-gain model so velocity (from the VOL column) and instrument-vol (Vxx)
  are distinct multipliers; add the instrument-vol-update analogue of `scheduleTrackPhraseVol`
  for mid-note Vxx on empty steps. Keep Vxx's `/255` linear mapping (matches instrument VOL).

**Ripple / risk**
- Display: VOL renders a hex byte; `7F` shows fine.
- This is the velocity source for MIDI-out (item 6) — keep the mapping in one place.

**Effort: M** (keeping the gain vs. velocity paths separate + the sampler curve is the real work)

---

## 3. Instrument screen — name behaviour + row rearrange

**Goal:** Instrument name auto-fills from the loaded sample/SF2, is editable like the project
name, shows `______` when empty; drop the redundant sample-name display; reorder rows.

**Current state** (`InstrumentModule.kt:21-58` doc block)
- Sampler rows: `0 TYPE/LOAD/SAVE · 1 NAME(read-only) · 2 SPACER · 3 SAMPLE(filename+LOAD+EDIT)
  · 4 ROOT/DET/TIC · 5 VOL/PAN · 6 SPACER · 7 DRIVE/FILTER …`. SF layout parallels with a
  PRESET row.
- Name row is read-only; "empty" convention is `sampleFilePath == null` (per memory).
- Row positions are hardcoded across: the module `draw`, `TrackerController.moveCursorDown/Up`
  INSTRUMENT branch incl. spacer-skips and `dualParamRows`/`tripleRow` sets
  (`TrackerController.kt:800-826`), `getInstrumentCursorLeft/RightColumn`, and
  `handleConfirmAInstrument` (row 3 = load sample / open sample editor, `AppInputDispatcher.kt:1420`).

**Changes**
- **Name**: show `______` when `sampleFilePath == null`; on sample/SF2 load, default
  `instrument.name` to the filename (sans extension) **if** the name is still empty/default;
  make NAME editable via SELECT → new `QwertyContext.INSTRUMENT_NAME` mirroring the
  `PROJECT_NAME` flow (`AppInputDispatcher` project-name handler ~`:1843`).
- **Remove sample-name text** from the SAMPLE/SF section row — keep the label + LOAD + EDIT.
- **Reorder** (sampler): `0 TYPE/LOAD/SAVE · 1 NAME · 2 ROOT/DET/TIC · 3 VOL/PAN · 4 SPACER ·
  5 SMPL+LOAD+EDIT · 6 SPACER · 7 DRIVE/FILTER …` (move old SPACER+SAMPLE down under VOL/PAN).
  Apply the equivalent shift to the SF layout.
- Move the EDIT button x to align with the DRIVE/CRUSH/DWNSMPL **value** column
  (currently `SRC_EDIT_OFFSET = 440`, `InstrumentModule.kt:78`).

**Ripple / risk**
- Every hardcoded INSTRUMENT row index moves — must update the module draw, all three cursor-nav
  helpers, the spacer-skip `when` arms, the dual/triple row sets, and `handleConfirmAInstrument`.
  This is the bulk of the work and the main regression surface (test every row's cursor + A action).
- Decide name-default policy when a user already renamed: don't clobber a custom name on reload.

**Decisions needed**
- 🟡 On loading a **new** sample into a slot that has a custom name — overwrite the name or keep it?
  *Recommendation: only set the name from the file when the current name is empty or the auto
  "INSTxx" default.*

**Effort: M**

---

## 4. Clone (L+B+A) from SONG screen — deep-copy phrases

**Goal:** Cloning a chain from the SONG screen produces a fully independent chain whose phrases
are also cloned, so edits don't bleed into the original.

**Current state** (`AppInputDispatcher.handleLBA`, `:2677-2718`)
- SONG branch copies the chain's `phraseRefs` + `transposeValues` into the next empty chain slot
  (`:2686-2688`) — but the new chain **references the same phrases**, so editing a phrase changes
  both chains.
- CHAIN and PHRASE branches already deep-copy phrase steps (`:2700,2711`).

**Changes**
- In the SONG branch, after finding the empty target chain: for each **unique** non-empty
  `phraseRef` in the source chain, allocate the next empty phrase slot, deep-copy steps
  (`dst.steps[i] = step.copy()`), and rewrite the cloned chain's `phraseRefs` to the new IDs.
  Map duplicate refs within the chain to the **same** new clone (preserve internal reuse).

**Ripple / risk**
- ✅ **Out of empty phrase slots → abort + status message** (e.g. "NO EMPTY PHRASES"); leave the
  original untouched so nothing half-clones. Check capacity *before* mutating anything.
- Keep `lastEditedChain`/`projectVersion` bookkeeping as today.

**Effort: S**

---

## 5. Song START plays from the cursor row

**Goal:** Pressing START on the SONG screen starts playback from the highlighted row (manual
already documents this); today it always starts at row 00.

**Current state**
- `PlaybackController.playSong(project, startRow = 0, loop)` **already supports a start row**
  (`:688,704`); `TrackerController.playSong(startRow = 0, …)` forwards it (`:405`).
- The START handler calls `trackerController.playSong()` with no row
  (`AppInputDispatcher` handleStart SONG branch, ~`:2138`; also the internal dispatch at
  `TrackerController.kt:424`).

**Changes**
- In handleStart's SONG branch, pass `startRow = trackerController.cursorRow`. Leave other screens
  at their current behaviour.

**Ripple / risk**
- Confirm `cursorRow` is the song row (0–255) and that `playSong`'s startRow isn't clamped to 0–15
  (the 0–15 clamp at `PlaybackController.kt:555` is in the chain path, not playSong). Low risk.

**Effort: S**

---

## 6. MIDI-out readiness audit ✅ — see `docs/midi-out-readiness.md`

**Do the analysis now (pre-MVP); the MIDI-out feature itself ships early post-MVP** (developer's
stated timeline). The audit's value pre-MVP is that it steers items 1 (128 programs) and 2
(0–127 velocity) so we don't bake in choices that fight MIDI later.

**Delivered:** `docs/midi-out-readiness.md` — what maps cleanly (channels/note/velocity/program),
the gaps (note-off/duration model is the big one; transport/clock; effect translation), proposed
`IMidiOut` architecture, a full effect→MIDI mapping table, and a phased plan. Confirms #1/#2 shipped
MIDI-compatible.

**Goal:** Identify what's missing to emit MIDI to external gear. No MIDI implementation now — this
is the gap list that steers items 1, 2, and the scheduler design.

**What already maps cleanly**
- **Channels:** 8 tracks → MIDI channels 0–7. Direct. (SF engine already treats tracks as MIDI
  channels.)
- **Pitch:** `Note.toMidi()` is standard MIDI, C-4 = 60 (`TrackerData.kt:59-62`).
- **Velocity:** becomes 0–127 after item 2 — then note-on velocity is a direct copy.
- **Program:** 128 instruments after item 1 → MIDI program change 0–127.

**Gaps / work needed**
- **Note range clamp:** octaves go to B-9 → `toMidi()` can exceed 127 (e.g. B-9 = 131). MIDI-out
  must clamp/skip > 127.
- **Note-off tracking:** the sampler fires one-shots; MIDI needs an explicit note-off per note-on.
  We have `scheduleNoteOff` for ADSR/TRIG (memory) but not for plain one-shots — MIDI-out needs a
  note-duration model (off at next note on the track, or at step/loop end).
- **Timing:** notes are frame-scheduled (`ScheduledNote` carries a frame). MIDI-out needs
  per-event timestamps + a MIDI clock (0xF8) if syncing external gear. The existing priority-queue
  scheduler is a good base — emit MIDI in parallel from `PlaybackController`.
- **Effect → MIDI mapping** (`EffectProcessor`, codes `:64-93`):
  - `PBN` pitch bend → MIDI pitch-bend (14-bit), but our range can exceed the synth's default
    ±2 semitones → send RPN 0 pitch-bend-range or clamp.
  - `PSL` slide → MIDI portamento (CC5 rate + CC65 on), synth-dependent; or emulate via pitch bend.
  - `PVB`/`PVX` vibrato → mod wheel (CC1, synth-dependent) **or** generate our own pitch-bend LFO.
  - `ARP` arpeggio → **no** MIDI primitive; must expand to discrete note-on/off at tick resolution.
  - `PIT` semitone offset → transpose the note number (clean for integer semitones).
  - `VOL` (Vxx mid-note) → CC7/CC11 (velocity can't change after note-on).
  - `OFF`/`SLI`/`KILL`/`REPEAT`/`TBL`/`THO`/`HOP`/`TIC`/`GRV`/`DEL`/`CHA`/`RND` — sampler/sequencer
    internal: `KILL`→note-off, `REPEAT`→retriggered note-ons, the rest have no MIDI form (apply
    internally before emit, or drop).
- **Architecture:** add a portable `IMidiOut` interface in `core/`, Android impl in
  `platform/android/` (Android MIDI API), driven from `PlaybackController` alongside the audio
  queue. Keep `core/logic` Android-free (CLAUDE.md rule).

**Deliverable of this item:** a short design doc + the note-off/duration refactor scoped, so the
real MIDI-out feature is a follow-on.

**Effort: M** (analysis + scoping) — implementation is a separate later feature (**L**).

---

## 7. Instrument Pool screen (M8-style)

**Goal:** A list view of all instrument slots with per-instrument mixer columns
(NAME, VOL, PAN, REV, DEL, EQ), quick load/preview/reorder/copy-paste.

**Current state**
- `ScreenType.INST_POOL` exists and is wired into the nav grid as the screen **above**
  INSTRUMENT (`ScreenType.kt:17,92` + `TrackerController` nav `:529-549`). The R+DPAD grid and
  `NavigationMapModule` already route to it.
- **No module, no render dispatch, no cursor state, no input handling** — it's a scaffold only.
  Renderer dispatches screens via `when (currentScreen)` (`PixelPerfectRenderer.kt:693,787`);
  there is no INST_POOL arm.
- Instruments already have the columns we need (VOL/PAN, REV/DEL sends, eqSlot — see instrument
  rows 5/11–13).

**Changes**
- New `InstrumentPoolModule.kt`: scrollable list (128 after item 1) with columns
  `# · NAME · VOL · PAN · REV · DEL · EQ`; `getCursorContext` + `handleInput` per column.
- New cursor state in `TrackerController` (`poolCursorRow`, `poolColumn`, `poolScroll`) + nav limits.
- Render dispatch arm in `PixelPerfectRenderer` + `ScreenLayoutParams` plumbing (mirror an existing
  module, e.g. Mixer/Settings).
- **Map M8 shortcuts to our scheme** (quote uses M8 buttons; we already have these combos elsewhere):
  - Edit mixer value small step → **A+LEFT/RIGHT**; large step → **A+UP/DOWN** (matches our value editing).
  - On NAME column, **A+UP/DOWN** = reorder instrument up/down the list.
  - **A** on an empty slot = load sample/SF2 (reuse the instrument LOAD flow).
  - Copy value/instrument / paste → our clipboard combos (copy = **B in selection**, paste =
    **L+A**, per the M8-style copy/paste already implemented).
  - Set default / clear instrument → **A+B**.
  - Preview selected instrument with **START** while stopped (reuse `previewInstrument`).
  - **Page nav**: R+DPAD already leaves the screen; add ±8 / first-last paging on a held modifier
    if desired (optional).
- **Reorder semantics:** moving an instrument changes its slot id → must remap every phrase
  `step.instrument` reference (and table/instrument cross-refs) or swap-and-remap. This is the
  hard part.

**Ripple / risk**
- ✅ **Reorder deferred to v2.** v1 = list view + edit mixer columns + load + preview. Reorder is
  the riskiest piece (swapping slots breaks phrase references unless we remap all
  `step.instrument` project-wide) and the rest is independently useful.
- Big surface even without reorder: new module + cursor state + render dispatch + clipboard
  integration + preview. Best done after item 3 (instrument name) and item 1 (128 slots) land.

**Effort: L**

---

## Cross-cutting notes
- Items **1, 2, 6** are the "MIDI alignment" cluster — decide the velocity curve and the 128-slot
  array question together.
- Items **3, 7** are the "instrument management" cluster — do 3 first; 7 reuses its name/empty logic.
- Items **4, 5** are quick, independent wins — good warm-ups, shippable immediately.
- All `core/logic` / `core/data` changes stay Android-free (CLAUDE.md portability rule).
- Each lands as its own `[Feature]`/`[Fix]`/`[UI]` commit on a branch, device-tested before commit
  (project testing rule).
