# /testdata — the conformance goldens

The measuring stick for songcore and the shell/Windows port (`linux-port-plan.md` §4.3,
`event-schema.md`). Everything here is **generated** — do not edit by hand.

---

## 1. The inventory

⚠️ **Most of these are recorded FROM KOTLIN, and the Kotlin is scheduled for deletion**
(convergence Phase E). Once it is gone no new golden can be recorded from it, and the existing
files silently change job — from **conformance** ("does the C++ match the Kotlin original?") to
**regression** ("does today's C++ still do what last month's C++ did?"). Both are worth having, but
the change is a dated, deliberate reclassification and not something to discover while regenerating.
See §5.

| golden | what it covers | recorded from | dies with Kotlin? |
|---|---|---|---|
| `g1..g8.ptp` (8) | the golden projects, serialized with the app's exact Json config | `GoldenTraceTest` ← `GoldenProjects.kt` | ⚠️ yes |
| `traces/*.trace` (36) | schema-v1 event traces, per project × {44100, 48000} × {render, song, chain, phrase} | `GoldenTraceTest` | ⚠️ yes |
| `units/p3-input.txt` (23,453 lines = 23,313 records + 115 comments + 25 blank) | the INPUT layer. `EDIT` 20,895 (cursor context + resolved action + the cell actually written), `THEME` 1,216, `R00`..`R15` 384 (24 each), `KBD` 384, `FXH` 170, `SEVIEW` 80, `SEL` 64, `SEROW` 60, `CLIP` 28, `THEMEPTT` 14, `THEMECYCLE` 12, `SORT` 6 | `P3InputGoldenTest` ← the real `InputController` + 14 screen modules + `ClipboardManager` + `FxHelperOverlay` | ⚠️ yes |
| `units/s3-units.txt` (317) | the pure functions: `framesPerStep`, groove timing, `byteToSignedSemitones`, `resolveStepParams`, `collectUsedInstruments` | `S3UnitGoldenTest` | ⚠️ yes |
| `units/s5-consumer.txt` (838) | the consumer call sequences a note produces | `S5ConsumerGoldenTest` | ⚠️ yes |
| `units/s7-random.txt` (35) | the random FX: draw COUNT and SUPPORT (exact), plus the distribution law (statistical) | `S7RandomGoldenTest` | ⚠️ yes |
| `units/touch-layout.txt` (675) | the touch layout **sizes** for all four entry points — see the warning below | `TouchLayoutGoldenTest` ← `TouchLayoutMetrics` | ⚠️ yes |
| **`native/songcore/note_tables.h`** (100) | 132 note frequencies + 256 detune multipliers, as raw binary32 bits | `S5NoteTableTest` ← `Note.toFrequency()` / `Instrument.detuneMultiplier()` | ⚠️ yes |
| `renders/*.txt` (2) | `ptrender` audio fingerprints: peak, RMS, per-second RMS in dBFS, tolerance-compared at ±1 dB | `tools/ptrender` (C++) | no |
| `golden/{kick,pad}.wav`, `test.sf2` | the media the projects play | `golden/make-golden-media.cpp` (C++) | no |
| `device/` | optional device-recorded traces (gitignored) | a real device | n/a |

⚠️ **`native/songcore/note_tables.h` does not live in this directory** and an inventory scoped to
`/testdata` misses it entirely. It is a generated C++ **source file** in the songcore tree, written
and guarded by a Kotlin test, and it is the only such file — verified by grepping every
`File(repoRoot(), …)` in `app/src/test/.../trace/` for a destination outside `testdata/`. If that
ever stops being true, this table is wrong.

⚠️ **`units/touch-layout.txt` pins SIZES, not POSITIONS, and the distinction is not a detail.**
`VirtualControls.kt` computes how big each button is and how much space sits between them; *where*
each button lands is computed by Compose's measure/layout pass (`Column`/`Row` +
`Arrangement.Center`, and `Modifier.weight(1f)` in Portrait2). Those positions exist only once
Android lays out a real screen, so no JVM test can record them. A port that matches all 675 lines
can still stack the D-pad in the wrong order. That hole is deliberate: a wrong arrangement is
obvious on a phone, a wrong proportion at an unowned screen size is not.

---

## 2. Regeneration

⚠️⚠️ **"Delete the file and re-run the tests" DOES NOT WORK, and it fails silently in the most
damaging possible way.** The files here are not declared Gradle inputs or outputs, so deleting one
does not invalidate `:app:testDebugUnitTest`. Gradle reports `UP-TO-DATE`, prints
`BUILD SUCCESSFUL`, runs nothing — and the golden stays **deleted**. Follow the old recipe on
`traces/` and you delete 36 files, see green, and commit the deletion.

Measured on 2026-07-20, not argued: delete `units/touch-layout.txt`, run `./gradlew
testDebugUnitTest`, get `UP-TO-DATE` + `BUILD SUCCESSFUL`, and the file is still missing.

**The recipe that works:**

```sh
# regenerate ONE golden (after an intentional behaviour change)
rm testdata/units/p3-input.txt
./gradlew testDebugUnitTest --rerun-tasks        # --rerun-tasks is NOT optional

# regenerate everything Kotlin-sourced
rm -rf testdata/traces testdata/units testdata/*.ptp native/songcore/note_tables.h
./gradlew testDebugUnitTest --rerun-tasks

# verify a run actually happened, and compared rather than regenerated
git status --porcelain testdata/      # empty  => every golden byte-compared against its file
```

Each recorder follows one contract: **missing file → generate; existing file → byte-compare.** That
is what makes the drift guard work, and also what makes a *silently missing* file dangerous — a
regenerating run is always green, whatever it writes.

⚠️ **A green `./gradlew test` proves nothing on its own.** It reports `BUILD SUCCESSFUL` off cached
results; on 2026-07-20 it did so from results dated 2026-07-14. Always confirm the run executed
(`26 actionable tasks: 26 executed`, not `up-to-date`), and confirm `git status testdata/` is clean
afterwards — a green suite that regenerated a golden and a green suite that compared against it look
identical from the exit code.

Commit any regenerated golden **in the same commit as the behaviour change that caused it**, per the
event-schema §9 discipline.

---

## 3. What is NOT covered

Named holes, so nobody has to rediscover them by being bitten:

- **The combo matrix — HALF answered since convergence C0.1 (2026-07-20), and the remaining half is
  the one that matters here.** `InputMapper` (`ButtonHandlers.kt`) turns raw presses into the named
  handlers — combos, key repeat, deferred A/B, held-modifier state. `ptinput` sits one layer below
  (it drives modules directly) and `ptdispatch` sits one layer below too (named handlers).
  - ✅ **The C++ side now has `tools/ptmapper`** (ctest `c0-mapper`). C0.1 moved the matrix out of
    `shell/main.cpp` into `native/ui/button_mapper.h` and templated it on the dispatcher, so a
    recording stub can drive it. 64 assertions over the order-sensitive arms. **Proven non-vacuous:
    reorder the L+B+A arm below the L+… block — the reordering the matrix's own comment warns about —
    and a clone silently PASTES; `ptmapper` is the only one of the ten tests that goes red.**
  - ⚠️ **It is NOT a golden, and this hole is still open on the KOTLIN side.** `ptmapper` is
    hand-written assertions in ptdispatch's sense — what its author believed after reading
    `ButtonHandlers.kt:402` — so it pins the C++ against future drift and proves nothing about
    conformance. Recording the Kotlin still needs a way past the `Handler(Looper.getMainLooper())`
    built in `InputMapper`'s field initialiser, since there is no Robolectric and no
    `testOptions { returnDefaultValues }`. **Writing that recorder is still gated on the tag in §5.**
  - ⚠️ **One divergence is already known and PINNED rather than fixed** (`ptmapper` §8, red-on-fix):
    Kotlin's double-tap reads `System.currentTimeMillis()` — absolute epoch ms — so a first A press
    can never satisfy `now - lastAPress < 300`. The shell passes `SDL_GetTicks64()`, which counts
    from `SDL_Init`, so an A press inside the first 300 ms reads as a double-tap and inserts the next
    UNUSED item where Kotlin inserts the LAST-EDITED one. Arrived with the clock substitution in
    Phase 3 S1, not with C0.1.
- **Touch button POSITIONS** — see §1.
- **`DeviceAdapter.LayoutMode` semantics.** `p3-input.txt`'s own header records that the module edits
  *indices*, so the C++ side is handed `(index, count)` rather than layout modes, because
  `LayoutMode` "would be dead code in a UI with no touch screen". Convergence Phase D makes that
  assumption false. `layoutModeList(hasPhysical)` and `skinsForLayout` are unrecorded.
- **Theme / `DeviceSkin` colour tables.** `THEME` lines cover the theme *editor*; the colour values
  behind each named theme and skin have never been byte-compared.
- **`ptdispatch` is not a golden at all.** It is hand-written assertions — what its author believed
  Kotlin does, having read it — and its own header says so at length. Do not count it as recorded
  behaviour.

None of these are lost if left unrecorded *provided the tag in §5 survives*: the Kotlin can be
checked out and a recorder written against it later. What decays is the practicality — getting a
year-old Gradle build running again gets harder every month.

---

## 4. The files in detail

- `*.ptp` — the golden projects. C++ songcore loads these same files for its side of the run
  (`tools/ptroundtrip`, `tools/ptplay`).
- `traces/<project>.<samplerate>.<mode>.trace` — schema-v1 event traces (format frozen in
  `native/songcore/event.h`), recorded per project in render mode and each live mode
  (`song`/`chain`/`phrase` + start arg), at 44100 and 48000 Hz.
- `units/` — the S3 pure-function cases, the S5 consumer call sequences, the S7 random-FX
  support/law, the P3 input cases, and the touch-layout sizes; replayed by `tools/ptresolve`,
  `tools/ptvoice`, `tools/ptrandom` and `tools/ptinput`.
- `golden/` — **the media the projects play**: `kick.wav`, `pad.wav`, `test.sf2`. The projects
  referenced these paths from the beginning and the files did not exist, which is why the goldens
  were "silent by design" — fine for event traces, which stop at the router far above sample
  loading, and useless for `tools/ptrender`, which renders real audio. They are **synthesized**, not
  sampled: `make-golden-media.cpp` is both their provenance and their regenerator. Each exercises a
  path — the kick is mono at the render's own rate (no resampling), the pad is **stereo at 22050** (a
  second channel buffer *and* a rate ratio of 2.0), and `test.sf2` is a hand-built minimal SoundFont
  at bank 0 / preset 5. Everything decays to silence and **nothing loops**, so no voice can run a
  render's decay tail out to its 30-second cap.
- `renders/<project>.<samplerate>.txt` — `tools/ptrender`'s audio fingerprints. **Tolerance-compared
  (±1 dB), not byte-compared** — the DSP uses transcendentals and is built with `-ffast-math` on arm,
  so toolchains legitimately disagree on the last bit of a reverb tail. See `tools/ptrender/README.md`
  for why that is the strongest honest claim, and what carries the exactness instead (a same-binary
  determinism check).
- `device/` (gitignored, optional) — drop a device-recorded `event.trace` here (any filename ending
  `.trace`, no renaming) and rerun the test. Each PLAY..STOP session identifies its own golden from
  its header (project sha, sr, mode, start arg); render sessions compare byte-for-byte, live sessions
  prefix-compare in emission order with preview-lane noise ignored (JVM↔ART float identity check).
  Step-by-step: `docs/internal/songcore-s1-device-test.md`.

---

## 5. Reclassification — conformance → regression

**Tag: `kotlin-golden-source`, created 2026-07-20 (convergence Phase B3).**

That commit is the last one from which a golden can be recorded from Kotlin — resolve it with
`git rev-parse kotlin-golden-source^{commit}`. Its tag message carries the full regeneration recipe
and the file list. (Referred to by name rather than by SHA on purpose: the tag points at the commit
containing this very paragraph, which no SHA written here could name.)

**Verified at that commit rather than assumed:** all 50 Kotlin-sourced goldens were deleted and
regenerated from scratch, and every one came back **byte-identical** to the committed version (md5
across all 50, corroborated by a clean `git status`). So the goldens committed here *are* what this
tree's Kotlin produces — no stale file is hiding a drift — and the recorders are deterministic.

**The reclassification itself takes effect when convergence Phase E deletes the Kotlin UI, not
today.** Until then these files are still conformance goldens and still answer "does the C++ match
the Kotlin original?". Afterwards they answer only "does today's C++ still do what last month's C++
did?" — which is the weaker claim, and the one that catches a refactor breaking a screen six months
out. Whoever lands Phase E writes the date here.

⚠️ **If any further golden is recorded from Kotlin before Phase E, this tag must MOVE** — a tag that
does not contain the recorder cannot regenerate its golden. Re-point it and update the file list in
its message.

⚠️ **The tag is only as durable as the branch it sits on.** It was created on `sdl-shell`, which is
unpushed by standing decision until the F-Droid MR merges, so today it exists on exactly one disk.
The one irreversible step in the convergence plan currently has no off-site copy. Push the branch
and the tag (`git push origin sdl-shell --tags`) as soon as that decision allows.

---

## 6. Rules

1. **Any diff in `traces/` is a sequencing behavior change.** `GoldenTraceTest` fails on drift. If
   the change is deliberate, regenerate per §2 and commit the new goldens in the same commit as the
   behavior change, per the event-schema §9 discipline.
2. Traces compare **byte-for-byte** (after the canonical `(frame, track, rank)` stable sort for
   cross-implementation runs). `.gitattributes` pins `-text` — never let EOL conversion touch these
   files.
3. Golden projects are random-free (no CHA/RND/RNL/ARP-RANDOM) per SC-1. Random FX get separate
   statistical tests. `g7-audio` additionally avoids everything else that is seeded from the wall
   clock (the RND/DRNK LFO shapes, DUST), because ptrender asserts that two renders of it are
   byte-identical.
4. Live SONG/CHAIN/PHRASE traces run to a fixed fake-clock horizon then stop — they intentionally
   include the look-ahead scheduling past the loop point (2-phrase buffer). Deterministic by FIX-1 +
   the fixed drive cadence in `TraceHarness`.
5. **Adding a project to `GoldenProjects.all` writes into three places**, and all of them are
   compare-if-present goldens: its `.ptp`, its `traces/`, and two lines in `units/s3-units.txt` (the
   `collectUsedInstruments` cases). Mirror the new spec into `tools/ptplay`'s `SPECS` table too, or
   the C++ side simply never checks it.
