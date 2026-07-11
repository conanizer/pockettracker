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

## Build & run (Windows, on-box MSVC)

No CMake target yet (arrives with the CI lane, S6). Compile the single TU directly — its
`#include`s are relative, so compile it in place:

```
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cl /std:c++17 /EHsc /O2 /nologo tools\ptresolve\main.cpp /Fe:ptresolve.exe
ptresolve.exe testdata
```

`clang++` / `g++` work equally (`-std=c++17 tools/ptresolve/main.cpp -o ptresolve`).

Exit code `0` = all green, `1` = any mismatch. Expected output ends in `ALL GREEN`.
