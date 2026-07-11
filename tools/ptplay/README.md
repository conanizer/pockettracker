# ptplay — songcore S4 spine conformance

A host tool (no device / NDK) that proves the C++ port of the sequencer **spine** — the "doomed"
zone-C Kotlin sequencing rewritten as shared C++ in songcore Phase 1 **S4**
(`linux-port-plan.md` §4.3) — reproduces the Kotlin golden event stream **byte-for-byte**:

- `native/songcore/scheduler.h` — `PlaybackController` + `TrackState`: `schedulePhrase` /
  `scheduleStepWithEffects` / `applyChanceAndRandomize` / `scheduleArpeggioNotes`, the transport
  starts (`playPhrase`/`playChain`/`playSong`), `updatePlaybackBuffer`, and the render-path
  `scheduleSongRowRange`.
- `native/songcore/router.h` — the MIDI event bus/router seam: one method per `AudioEngine.schedule*`
  tap point, each building a `songcore::Event` (the `event.h` bus record) from the seam args.
- `native/songcore/trace_writer.h` — the schema-v1 conformance-trace serializer (the C++ twin of
  `core/trace/EventTrace.kt`).

## How it proves parity

The JVM `GoldenTraceTest` recorded the goldens in `/testdata` from the **real Kotlin sequencer**
through the `EventTrace` tap: the project `.ptp` files plus one trace per (project, sample rate,
mode) — render + live `SONG`/`CHAIN`/`PHRASE`, at 44100 and 48000 Hz. ptplay loads those same
`.ptp` files, drives the C++ `Sequencer` through the **identical** modes and clock cadence as the
Kotlin `TraceHarness` (live start frame 977, clock step 512, per-mode phrase horizon), and compares
its trace against the committed golden after the event-schema §4 **canonical sort**
(`(frame, track, rank)`, stable) — the same comparator `TraceCompare.kt` applies for
cross-implementation runs.

Floats compare as their raw binary32 bits (event-schema §5), so this is where the whole scheduler's
`velGain`/`volGain`/`pan`/PSL/PBN/PVB arithmetic is proven bit-identical across the language boundary
(S3 already proved the shared pure pieces; S4 proves the stateful spine on top of them). The trace
header's `project=` id is SHA-1 of the `.ptp` bytes, which equals the JVM golden's id — so a header
mismatch would also flag a serializer drift (S2 proved the round-trip).

The goldens must exist first — run the JVM test (writes them if missing):

```
gradlew.bat :app:testDebugUnitTest --tests "com.conanizer.pockettracker.trace.GoldenTraceTest"
```

## Build & run

The four conformance tools are one CMake project (`tools/CMakeLists.txt`), wired to ctest. CI runs
exactly this on every push, on gcc/x86-64, MSVC/x86-64 and clang/arm64:

```
cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/build --config Release
ctest --test-dir tools/build --output-on-failure -C Release
```

This tool alone is the **`s4-golden-traces`** test — `ctest --test-dir tools/build -R s4-golden-traces
--output-on-failure` — or invoke the built binary directly with the goldens directory as `argv[1]`
(`ptplay testdata`).

Exit code `0` = all green, `1` = any mismatch. Expected output ends in `ALL GREEN` (32 traces:
6 projects × 2 sample rates × render + their live modes). A mismatch reports the first divergent
canonical line, golden vs C++ — a single-ULP float drift is enough to trip it, which is the point.
