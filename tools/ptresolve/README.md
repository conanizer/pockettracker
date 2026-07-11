# ptresolve — songcore S3 pure-piece parity conformance

A host tool (no device / NDK) that proves the C++ ports of the sequencer's **pure, stateless
pieces** are equivalent to their Kotlin originals — the layer moved to shared C++ in songcore
Phase 1 **S3** (`linux-port-plan.md` §4.3):

- `native/songcore/timing.h` — `framesPerStep` (Double→Long truncation), `framesPerTic`,
  `byteToSignedSemitones`, and the groove math (`activeLength` / `getTicksForStep` / the per-step
  duration composition).
- `native/songcore/effects.h` — `resolveStepParams` + the `ResolvedStepParams` bundle + the `FX_*`
  codes.
- `native/songcore/traversal.h` — `collectUsedInstruments` (loads the real `/testdata` `.ptp`
  projects through the S2 reader).

## How it proves parity

The JVM `S3UnitGoldenTest` emits `testdata/units/s3-units.txt` from the **real Kotlin functions** —
one `<inputs> => <outputs>` line per case (a matrix of tempos/sample-rates, grooves, transpose
bytes, an FX sweep over every resolvable effect, multi-slot last-wins cases, and
`collectUsedInstruments` over the golden projects). ptresolve re-parses each line's **inputs**,
recomputes the RHS in C++, and byte-compares against the golden RHS. This is where the
`framesPerStep` double rounding and the binary32 `volume` divide are proven **bit-identical** across
the language boundary (floats are compared as their raw binary32 bits, per `event-schema.md`).

The golden must exist first — run the JVM test (writes it if missing):

```
gradlew.bat :app:testDebugUnitTest --tests "com.conanizer.pockettracker.trace.S3UnitGoldenTest"
```

## Build & run

The four conformance tools are one CMake project (`tools/CMakeLists.txt`), wired to ctest. CI runs
exactly this on every push, on gcc/x86-64, MSVC/x86-64 and clang/arm64:

```
cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/build --config Release
ctest --test-dir tools/build --output-on-failure -C Release
```

This tool alone is the **`s3-pure-units`** test — `ctest --test-dir tools/build -R s3-pure-units
--output-on-failure` — or invoke the built binary directly with the goldens directory as `argv[1]`
(`ptresolve testdata`).

Exit code `0` = all green, `1` = any mismatch. Expected output ends in `ALL GREEN`.
