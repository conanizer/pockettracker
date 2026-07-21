# pttouch — touch-layout SIZE conformance (convergence D1/D3)

A host tool (no device / NDK) that proves the C++ port of the touch-layout **size arithmetic**
reproduces what Android's `TouchLayoutMetrics` — the maths `VirtualControls.kt`'s four composables
actually call — computed, bit for bit.

## Why this exists

The touch skin is the one piece of the Android app with **no C++ twin and no golden** (convergence
plan §6). A touch layout is two computations wearing one coat:

- **SIZES** — "each arrow is X px square, spacers are 0.2X" — plain arithmetic over
  `(availableWidth, availableHeight, density)`.
- **POSITIONS** — "the UP arrow lands at x=340" — produced on Android by **Compose's** measure/layout
  pass (`Column`/`Row` + `Arrangement.Center`; Portrait2's `Modifier.weight`). This exists in no
  Kotlin file, so no JVM test could record it.

Phase B2 recorded the SIZES — the half that *can* be recorded — from the real Kotlin into
`testdata/units/touch-layout.txt`, while Kotlin was still there to answer. Phase E deletes that Kotlin;
after it, this golden is the only surviving statement of what the sizes should be.

## What it checks

`app/src/test/.../trace/TouchLayoutGoldenTest.kt` drives the real `TouchLayoutMetrics` over a matrix
that brackets the reachable box sizes and straddles the one branch in the file (`boxRatio <
patternRatio`, pinned from both sides plus the exact tie), across every real Android density. Each
line records the inputs and Kotlin's outputs — ints decimal, floats as raw binary32 bits.

pttouch re-parses each line's inputs, recomputes the right-hand side through the C++ port
(`native/ui/touch_layout.h`), and byte-compares the field string it produces. Same contract as
`s3-units` / `s5-consumer` / `p3-input`.

## ⚠️ What it does NOT claim

**SIZES only, not POSITIONS.** A port that matches every line here can still stack the D-pad in the
wrong order, off-centre, or overlapping the backing image. That hole is known and accepted: a wrong
*arrangement* is obvious the moment anyone looks at a phone, a wrong *proportion* at a screen size
nobody in the room owns is invisible until a user reports it. The arrangement is ported and
eyeball-verified on a device in a later D increment; this tool covers the failure the eye cannot.

**It never regenerates the golden.** Only the Kotlin recorder does that (missing → generate). Here a
missing or empty golden is a hard error, so deleting the file cannot make the check certify anything.

## Build + run

```
cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/build --config Release
ctest --test-dir tools/build -R d-touch-layout --output-on-failure -C Release
```

Exit 0 = all green, 1 = any mismatch, 2 = a bad or missing golden. Run by CI on every push via the
`d-touch-layout` ctest (`tools/CMakeLists.txt`).
