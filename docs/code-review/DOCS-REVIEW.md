# PocketTracker Documentation Review

Fact-check + improvement suggestions for the doc set, cross-checked against the code read during the
Stage 1-7 code review (see REVIEW.md). Tags: ❌ factually wrong · ⚠️ stale/misleading · 🔁 duplication/drift ·
🔵 structure/clarity · 💡 suggestion · ✅ verified-correct.

## Docs in scope

| Doc | Lines | Status |
|-----|-------|--------|
| README.md | 145 | ✅ |
| CLAUDE.md | 709 | ✅ |
| docs/development-status.md | 467 | ✅ |
| docs/technical-architecture.md | 1182 | ✅ |
| docs/manual-en.md | 1746 | ✅ |

---

## README.md — polished, one factual error

- **❌ "WAV instruments (8–32-bit PCM / float)"** (L18). `parseWavBuffer` (`AudioEngine.kt`) only decodes
  16/24/32-bit PCM and 32-bit IEEE float — there is **no 8-bit case**, so an 8-bit WAV throws
  "Unsupported WAV format". Change to **"16–32-bit PCM / float"**.
- **✅ verified correct:** 4 layout modes, "10 destinations incl. mod-to-mod" (7 + MOD_AMT/RATE/BOTH),
  send buses, SF2 fork, the `core/` + `platform/` architecture block, the library/license credits.
- **💡** Master bus is described as "OTT 3-band compressor + limiter" (L20); the engine also has **DUST**
  as a selectable alternate master FX (`masterBusFx 0=OTT, 1=DUST`). One clause would make it complete.
- **🔵** L5 calls it "Little Piggy Tracker", L131 "LGPT / Picotracker" — pick one name for LGPT.

## CLAUDE.md — accurate in spirit, stale links & dates

- **❌ Broken references:** points to `MVP_EXPANSION_PLAN.md` and `MVP_EXPANSION_PACK_3.md` (multiple
  times) — **neither file exists**. Since CLAUDE.md is the first thing the agent reads, dead links here
  are the most costly. Either restore the files, repoint to `docs/development-status.md`, or drop the refs.
- **⚠️ Stale framing:** header "Current Project State (April 2026)" and "Target release: April 2026"
  while today is June 2026 and `development-status.md` already records "April target missed". Sync the date
  language (dev-status is the one that's current — rev 7, dated 2026-06-09).
- **⚠️ Line-count drift:** "MainActivity reduced … to 1069 lines" — actual is **1024**. (Same class of
  drift as the architecture doc; see cross-cutting note.)
- **🔵** At 709 lines it's long for an agent guide and restates much of dev-status (status checklists,
  effect list, navigation grid). Consider trimming the "what's done" history (link to dev-status instead)
  and keeping CLAUDE.md focused on *rules + where things live + how to work*.

## development-status.md — the most current & accurate doc

- **✅** Dated today (rev 7), matches the code: the "Architecture Debt" entries (duplicated table-advance
  loops, `sinf` LFO hot path, stageCounter drift, dry-preview through master) are all real and confirmed
  during the code review. This is the canonical status doc — keep it as the source of truth.
- **⚠️** The debt section references `docs/plan-module-system.md` and (elsewhere) `plan-sample-editor-v2.md`;
  the first is **missing**, the second **moved to `docs/internal/`**. Fix the paths (see cross-cutting).

## technical-architecture.md — excellent prose, drifted file-tree & data samples

The Audio Engine, SoundFont, Rendering, and **Modulation Engine** sections are accurate and genuinely
useful — the dest=1/dest=3 modulation formulas match `audio-engine.cpp` line-for-line. The problems are
concentrated in the structural/illustrative parts:

- **❌ File-tree is out of date with the package layout** (L63-130). It places:
  - `MainActivity.kt`, `AppInputDispatcher.kt` under `platform/android/` — actually root pkg and **`input/`**
  - `EditorHelpers.kt`, `PixelPerfectRenderer.kt`, the modules, `TrackerData.kt` at "root package" —
    actually **`ui/`**, **`ui/modules/`**, **`core/data/`**
  - `mod-system.h`, `tsf.h`, `filter.h` at top-level `cpp/` — actually **`mods/`**, **`vendor/tsf/`**,
    and there is **no top-level `filter.h`** (biquad lives in `effects/primitives/`)
  A contributor or the agent navigating by this tree will look in the wrong folders.
- **❌ Data Model code samples are wrong in ways that mislead** (L425-465):
  - `Project(… val song: Song)` — **there is no `Song` class**; the real field is `tracks: Array<Track>`
  - `Chain(… val transpose: IntArray)` — real field is **`transposeValues`**
  - `tempo: Int = 120` — real default is **128**
  - `fx1Type`/`fx1Value` shown as `val` — real ones are `var`
  Either fix them or label the blocks "simplified — `TrackerData.kt` is authoritative."
- **❌ Broken refs:** `docs/plan-module-system.md`, `docs/plan-dsp-modules.md` — both missing.
- **⚠️** "AppInputDispatcher.kt … (~2108 lines)" — actual **2713**; "MainActivity … ~1069" — actual 1024.
- **🔵 Redundant "Target Architecture (After Refactoring)" section** (L133-254) still lists aspirational
  names that don't exist (`Sequencer.kt`, `FileManager.kt` as the active save/load) even though the doc
  states refactoring is **complete**. Since "Current Architecture" already documents the real structure,
  the "Target" section now causes confusion — fold the useful Linux-layer diagram into Current and delete
  the rest, or clearly mark it "historical plan".
- **💡** First ~250 lines are motivation + developer quotes; that's vision content that overlaps
  `product-vision.md`. Trimming it makes the technical reference faster to use.

## manual-en.md — thorough and accurate; one naming mismatch

Genuinely good: 24 sections + 4 appendices, effect encodings cross-check cleanly against `EffectProcessor`
(ARP nibbles, CHA probability math `CHA 82 ≈ 53%`, PIT signed range, etc.).

- **❌ Retrigger effect is called "REP" in the manual but the app shows "RPT".** The manual uses `REP`
  everywhere (§21 `### REP`, examples `REP 03`, cheat-sheet L1712), but `getEffectTypeName(0x12)` returns
  **`"RPT"`**, which is what appears on the PHRASE screen. A user reading "place REP 03" will not find
  "REP" in the FX-type cycle. **Pick one name** and make manual + UI agree (the UI string is the one users
  actually see, so either rename the UI to REP or the manual to RPT). Note dev-status and CLAUDE.md also
  say "REP", so the code's "RPT" is the outlier.
- **⚠️** The **HOP** section (L1126-1131) omits the `HOP FF` = *stop track* behavior that `EffectProcessor`
  implements (`value == 0xFF → stop`). Worth one line.
- **✅ Appendix C is correct:** "All 256 slots (00–FF) start empty." This actually **proves the code
  comments wrong**, not the manual — `PhraseStep.instrument`/`Instrument.id` carry stale `// 00-7F`
  comments (code-review finding 1.4); the manual and the real behavior are 00-FF.
- **💡** `manual-ru.md` (Mar 19) is ~3 months behind `manual-en.md` (Jun 9). Either note "RU translation
  may lag" or regenerate it; right now they describe different feature sets.

---

## Cross-cutting documentation issues

### D1 🔁 The feature list & effect list are restated in 5+ places and drift
The effect roster appears in CLAUDE.md, development-status.md, manual-en.md §21, features.md, plus the code
(`EffectProcessor`, `EditorHelpers.getEffectTypeName`). They already disagree (REP vs RPT). **Suggestion:**
make **one** doc canonical for the user-facing effect table (manual §21) and one for status
(development-status.md), and have the others link rather than re-list. The code's `getEffectTypeName` should
be the single source for the on-screen names (and the manual should match it).

### D2 🔗 Do a link/reference audit — several targets moved to docs/internal/ or were deleted
Missing/moved: `MVP_EXPANSION_PLAN.md`, `MVP_EXPANSION_PACK_3.md`, `docs/plan-module-system.md`,
`docs/plan-dsp-modules.md` (all referenced, none present); `plan-sample-editor-v2.md` now lives under
`docs/internal/`. A quick `grep -rno '\][(][^)]*\.md' *.md docs/*.md` + existence check would catch these.

### D3 ⚠️ Stop hard-coding exact line counts in prose
"MainActivity ~1069 lines", "AppInputDispatcher ~2108 lines", "reduced from 2668 to 1862" — these are in
CLAUDE.md, technical-architecture.md, dev-status, and the memory, and they drift the moment any edit lands
(actual now 1024 / 2713). Use qualitative language ("thin coordinator", "the large dispatcher") or accept
they're approximate and stop citing precise numbers.

### D4 💡 Add a one-paragraph "doc map" so each doc states its role and what's authoritative
README already has a doc table; extend the idea: a 3-line header in each doc — *"Authoritative for X. For Y
see Z."* — prevents the same fact (e.g. effect encodings, file layout) being maintained in parallel and
drifting. Pair this with the code-review note 6.2 (move effect names into core) so docs and UI can't
disagree on names again.

<!-- findings appended below -->
