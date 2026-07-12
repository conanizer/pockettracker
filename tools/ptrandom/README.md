# ptrandom — songcore S7 random-FX conformance

A host tool (no device / NDK) that proves the C++ port of the **random effects** — CHA, RND, RNL and
ARP mode RANDOM — behaves like the Kotlin sequencer's. It is the sixth and last tool on the
conformance ladder, and the only one that does not compare bytes.

## Why this exists

The other five tools all rest on the same trick: run the Kotlin implementation, record what it did,
and demand the C++ one reproduce it exactly. That trick does not work here. `kotlin.random.Random` is
seeded from the platform, so the **Kotlin** sequencer does not produce the same event stream twice
either — there is nothing for a `.trace` file to be.

SC-1 took the only sane option and kept these FX out of the golden projects entirely. The hole that
left is the reason this tool exists. songcore shipped with the draws **stubbed** —

```cpp
int rng_int(int bound)        { return 0;  }   // scheduler.h, S4 → S6b
int rng_range(int lo, int hi) { return lo; }
```

— for four sessions, with every check green the whole time. And the stub did not merely bias the
draws: it pinned each one to the **lowest value in its range**. CHA passed whenever its probability
nibble was nonzero. RND and RNL always emitted the bottom of their band. A random arpeggio always
played the root. Nothing we owned was looking.

## What it checks

`app/src/test/.../trace/S7RandomGoldenTest.kt` drives the **real** Kotlin `PlaybackController` over
`testdata/g8-random.ptp` for 300 renders, histograms every random draw, and writes
`testdata/units/s7-random.txt`. ptrandom does the same in C++ and compares. The claim is split in two,
and the split is the whole design:

| | compared | how | catches |
|---|---|---|---|
| **`n=`** | exactly | string equality | both engines ran the same code paths the same number of times (the random FX change *which* value comes out, never *how many* are drawn — so this is deterministic) |
| **`support=`** | exactly | string equality | an off-by-one, a closed interval where Kotlin's is half-open, a stub pinned to one end — **every bug a bounded random draw actually has** |
| **`expect=`** | statistically | chi-square / Bernoulli, each engine against its *own* histogram | a gross shape error: a mode drawn 1/2 of the time instead of 1/3 |

The support is the load-bearing half, and it is *exact* — no tolerance, no flakiness. At these sample
sizes the chance of a reachable value going unobserved is about e^-200. The statistical half is
deliberately loose: it is there for the errors the support cannot see, and its margins are wide enough
that the null hypothesis effectively never fires.

Note what is **not** compared: Kotlin's counts against C++'s counts. Two random samples never agree
exactly, and demanding that they do is how a test becomes flaky. Each engine is held to the *law*,
and the law is not asserted from theory — the Kotlin measurement is what validates it.

## The golden project

`g8-random` puts **one random FX on each of six tracks**, so no two draws can interact, and is built so
that every draw lands in the schema-v1 trace as an **exact integer**. `PIT` is the carrier of choice:
`resolveStepParams` maps its byte to a signed semitone bijectively (`value < 0x80 ? value : value −
256`, no clamp, no rounding) and it rides the NoteOn's `pit=` field — so a randomized FX byte arrives
in the trace as a plain integer, with no float to invert. Every band stays clear of every clamp: let
RNL's ±5-semitone offset saturate at 0 or 127 and probability would pile up on the boundary, making the
"uniform" claim simply false.

Two of the twelve observables are **exact, not statistical**: `CHA 00` must *never* fire and `CHA F0`
must *always* fire, because the roll is `nextInt(15)` → 0..14. They show up in the golden as
`support=0` and `support=1`.

## Reproducibility

ptrandom **seeds the sequencer per render** (`Sequencer::seed_rng`), so unlike the JVM side — which
cannot seed `Random.Default` and really does draw a fresh sample every run — it is bit-deterministic on
every platform. **It can fail, but it cannot flake.**

## Negative controls (both verified)

- **Put the stub back** → 13 failures. Every support collapses to a single value: `rnd-pit support=48`
  where the golden says `48..127`, `arp-random support=0` where it says `0,3,7`, and the CHA gates all
  at `P(1)=1.0000`. The three observables the stub *coincidentally* got right — CHA p=0, CHA p=15, and
  the no-CHA control — correctly still pass.
- **A support-preserving bug**: `notes[rng_int(4) % 3]` in the ARP draw weights the root 2:1 while
  leaving the support at `{0,3,7}` untouched. The exact arm cannot see it; the chi-square catches it
  alone, at **7289.8 against a limit of 24.0**. This is why both arms exist.

## Build & run

The six conformance tools are one CMake project (`tools/CMakeLists.txt`), wired to ctest. CI runs
exactly this on every push, on gcc/x86-64, MSVC/x86-64 and clang/arm64:

```
cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/build --config Release
ctest --test-dir tools/build --output-on-failure -C Release
```

This tool alone is the **`s7-random`** test — `ctest --test-dir tools/build -R s7-random
--output-on-failure` — or invoke the binary directly with /testdata as `argv[1]`.

Exit 0 = all green, 1 = any mismatch; a failure names the observable and prints the Kotlin line against
the C++ one.

Regenerate the golden after an intentional Kotlin change: delete `testdata/units/s7-random.txt`, run
`gradlew :app:testDebugUnitTest`, re-run ptrandom.

⚠️ `RENDERS`, `PHRASE_REPEATS` and `SAMPLE_RATE` are duplicated in `main.cpp` and
`S7RandomGoldenTest.kt` and **must stay equal** — the golden's `n=` counts are derived from them.

## What it does NOT cover

That Android actually *gives* us entropy. `platform_entropy()` mixes the monotonic clock, the wall
clock and an ASLR address, but a constant seed would make every app launch replay the identical
"random" sequence — and ptrandom, which seeds itself on purpose, could never notice. That check is
`docs/internal/songcore-s7-device-test.md` §C: cold-launch the app twice and the pattern must differ.
