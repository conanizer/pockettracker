# /testdata — songcore conformance goldens

The measuring stick for songcore Phase 1 (`linux-port-plan.md` §4.3, `event-schema.md`).
Everything here is **generated** — by `app/src/test/java/.../trace/GoldenTraceTest.kt` from the
synthetic golden projects in `GoldenProjects.kt`, driven through the real Kotlin sequencer
(`PlaybackController`/`EffectProcessor`/`AudioEngine`) via the `EventTrace` tap; or, for the media and
the render fingerprints, by the generators named below. Do not edit by hand.

- `*.ptp` — the golden projects, serialized with the app's exact Json config. C++ songcore loads these
  same files for its side of the conformance run (`tools/ptroundtrip`, `tools/ptplay`).
- `traces/<project>.<samplerate>.<mode>.trace` — schema-v1 event traces (format frozen in
  `native/songcore/event.h`), recorded per project in render mode and each live mode
  (`song`/`chain`/`phrase` + start arg), at 44100 and 48000 Hz.
- `units/` — the S3 pure-function cases (`s3-units.txt`) and the S5 consumer call sequences
  (`s5-consumer.txt`), replayed by `tools/ptresolve` and `tools/ptvoice`.
- `golden/` — **the media the projects play**: `kick.wav`, `pad.wav`, `test.sf2`. The projects have
  referenced these paths from the beginning and the files did not exist, which is why the goldens were
  "silent by design" — fine for event traces, which stop at the router far above sample loading, and
  useless for `tools/ptrender`, which renders real audio. They are **synthesized**, not sampled:
  `make-golden-media.cpp` is both their provenance and their regenerator. Each exercises a path — the
  kick is mono at the render's own rate (no resampling), the pad is **stereo at 22050** (a second
  channel buffer *and* a rate ratio of 2.0), and `test.sf2` is a hand-built minimal SoundFont at
  bank 0 / preset 5. Everything decays to silence and **nothing loops**, so no voice can run a render's
  decay tail out to its 30-second cap.
- `renders/<project>.<samplerate>.txt` — `tools/ptrender`'s audio fingerprints: peak, RMS and
  per-second RMS in dBFS. **Tolerance-compared (±1 dB), not byte-compared** — the DSP uses
  transcendentals and is built with `-ffast-math` on arm, so toolchains legitimately disagree on the
  last bit of a reverb tail. See `tools/ptrender/README.md` for why that is the strongest honest claim,
  and what carries the exactness instead (a same-binary determinism check).
- `device/` (gitignored, optional) — drop a device-recorded `event.trace` here (any filename
  ending `.trace`, no renaming) and rerun the test. Each PLAY..STOP session identifies its own
  golden from its header (project sha, sr, mode, start arg); render sessions compare
  byte-for-byte, live sessions prefix-compare in emission order with preview-lane noise
  ignored (JVM↔ART float identity check). Step-by-step: `docs/internal/songcore-s1-device-test.md`.

Rules:

1. **Any diff in `traces/` is a sequencing behavior change.** `GoldenTraceTest` fails on drift. If the
   change is deliberate, regenerate (delete `traces/`, rerun tests) and commit the new goldens
   in the same commit as the behavior change, per the event-schema §9 discipline.
2. Traces compare **byte-for-byte** (after the canonical `(frame, track, rank)` stable sort for
   cross-implementation runs). `.gitattributes` pins `-text` — never let EOL conversion touch
   these files.
3. Golden projects are random-free (no CHA/RND/RNL/ARP-RANDOM) per SC-1. Random FX get separate
   statistical tests in a later session. `g7-audio` additionally avoids everything else that is
   seeded from the wall clock (the RND/DRNK LFO shapes, DUST), because ptrender asserts that two
   renders of it are byte-identical.
4. Live SONG/CHAIN/PHRASE traces run to a fixed fake-clock horizon then stop — they intentionally
   include the look-ahead scheduling past the loop point (2-phrase buffer). Deterministic by
   FIX-1 + the fixed drive cadence in `TraceHarness`.
5. **Adding a project to `GoldenProjects.all` writes into three places**, and all of them are
   compare-if-present goldens: its `.ptp`, its `traces/`, and two lines in `units/s3-units.txt`
   (the `collectUsedInstruments` cases). Mirror the new spec into `tools/ptplay`'s `SPECS` table too,
   or the C++ side simply never checks it.
