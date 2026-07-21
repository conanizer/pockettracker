# pttouch — touch-layout conformance (convergence D1/D3)

A host tool (no device / NDK) that checks the C++ port of the touch layout in `native/ui/touch_layout.h`.
It has **two modes**, because a touch layout is two computations with two different kinds of truth:

| mode | covers | contract |
|---|---|---|
| `pttouch <golden>`    | the **SIZES** (D1) | golden-backed, strict conformance |
| `pttouch --positions` | the **POSITIONS** (D3) | hand-written **oracle**, no golden — the ptmapper/ptdispatch pattern |

## Why this exists

The touch skin is the one piece of the Android app with **no C++ twin** (convergence plan §6). A touch
layout is two computations wearing one coat:

- **SIZES** — "each arrow is X px square, spacers are 0.2X" — plain arithmetic over
  `(availableWidth, availableHeight, density)`. This *can* be recorded from Kotlin.
- **POSITIONS** — "the UP arrow lands at x=253, y=305 inside its box" — produced on Android by
  **Compose's** measure/layout pass (`Column`/`Row` + `Arrangement.Center`). This exists in no Kotlin
  file, so no JVM test could record it. There is **no golden and there never can be.**

## SIZES — `pttouch <golden>`

Phase B2 recorded the sizes from the real Kotlin into `testdata/units/touch-layout.txt`, while Kotlin
was still there to answer. `app/src/test/.../trace/TouchLayoutGoldenTest.kt` drives the real
`TouchLayoutMetrics` over a matrix that brackets the reachable box sizes and straddles the one branch
in the file (`boxRatio < patternRatio`, pinned from both sides plus the exact tie), across every real
Android density; each line records the inputs and Kotlin's outputs (ints decimal, floats as raw
binary32 bits). pttouch re-parses each line's inputs, recomputes the right-hand side through the C++
port, and byte-compares. Same contract as `s3-units` / `s5-consumer` / `p3-input`. **It never
regenerates the golden** — a missing or empty file is a hard error, so deleting it cannot make the
check certify anything.

## POSITIONS — `pttouch --positions`

Where each button *lands* is unrecordable, so this is a hand-written **oracle** over
`touch_layout.h`'s `left_rects`/`right_rects`, exactly like `ptmapper` (the combo matrix) and
`ptdispatch` (the input join): it encodes what the author believes Compose does, having read
`VirtualControls.kt`, and asserts the **structure** the arrangement must have —

- the LEFT box's D-pad is a **cross** (UP above the LEFT·RIGHT row above DOWN, UP centred over the gap,
  the cells flush), with L above it and SELECT below;
- the RIGHT box's A and B are a **diagonal** (A upper-right, B lower-left), R above, START below;
- the block is **vertically centred**, no two rects **overlap**, and every rect is **inside** its box;
- **hit-testing** works: a tap in a rect hits that button, a tap in the cross-hole hits nothing;

plus a handful of **pinned exact coordinates** at the Xiaomi's 716×1220 landscape panel, so an
arithmetic drift prints its number. Checked at two sizes, so a regression can't hide at one resolution.

This is a **weaker claim** than the golden mode — it proves what the author believes, not what Kotlin
did — and the arrangement is *also* eyeball-verified on a device. A wrong *proportion* at an unowned
screen size is what the golden catches; a wrong *arrangement* is what this oracle and the phone catch.
⚠️ D3 covers the two LANDSCAPE boxes; PORTRAIT and PORTRAIT2 join when those modes are lit up.

## Build + run

```
cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/build --config Release
ctest --test-dir tools/build -R d-touch --output-on-failure -C Release   # both d-touch-* ctests
```

Exit 0 = all green, 1 = any mismatch/assertion, 2 = a usage or bad/missing-golden error. Run by CI on
every push via the `d-touch-layout` and `d-touch-positions` ctests (`tools/CMakeLists.txt`).
