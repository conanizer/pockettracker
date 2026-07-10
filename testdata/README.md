# /testdata — songcore conformance goldens

The measuring stick for songcore Phase 1 (`linux-port-plan.md` §4.3, `event-schema.md`).
Everything here is **generated** by `app/src/test/java/.../trace/GoldenTraceTest.kt` from the
synthetic golden projects in `GoldenProjects.kt`, driven through the real Kotlin sequencer
(`PlaybackController`/`EffectProcessor`/`AudioEngine`) via the `EventTrace` tap. Do not edit by
hand.

- `*.ptp` — the golden projects, serialized with the app's exact Json config. C++ songcore
  (Phase 1 S4) loads these same files for its side of the conformance run.
- `traces/<project>.<samplerate>.<mode>.trace` — schema-v1 event traces (format frozen in
  `app/src/main/cpp/songcore/event.h`), recorded per project in render mode and each live mode
  (`song`/`chain`/`phrase` + start arg), at 44100 and 48000 Hz.
- `device/` (gitignored, optional) — drop a device-recorded `event.trace` here (any filename
  ending `.trace`, no renaming) and rerun the test. Each PLAY..STOP session identifies its own
  golden from its header (project sha, sr, mode, start arg); render sessions compare
  byte-for-byte, live sessions prefix-compare in emission order with preview-lane noise
  ignored (JVM↔ART float identity check). Step-by-step: `docs/internal/songcore-s1-device-test.md`.

Rules:

1. **Any diff here is a sequencing behavior change.** `GoldenTraceTest` fails on drift. If the
   change is deliberate, regenerate (delete `traces/`, rerun tests) and commit the new goldens
   in the same commit as the behavior change, per the event-schema §9 discipline.
2. Traces compare **byte-for-byte** (after the canonical `(frame, track, rank)` stable sort for
   cross-implementation runs). `.gitattributes` pins `-text` — never let EOL conversion touch
   these files.
3. Golden projects are random-free (no CHA/RND/RNL/ARP-RANDOM) per SC-1. Random FX get separate
   statistical tests in a later session.
4. Live SONG/CHAIN/PHRASE traces run to a fixed fake-clock horizon then stop — they intentionally
   include the look-ahead scheduling past the loop point (2-phrase buffer). Deterministic by
   FIX-1 + the fixed drive cadence in `TraceHarness`.
