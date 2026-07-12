# ptnondet — is a project's render a pure function of the project?

A host tool (no device / NDK) that answers one question about a `.ptp`: **can it be byte-compared at
all?**

## Why this exists

It was written *after* an A/B went sideways, which is the honest reason to keep it.

The S7 device test asked for the standard check — render a project under `ENG = KT`, render it under
`ENG = C++`, and the two WAVs must be byte-identical. They were not: 1.4 MB of 5.0 MB differed. That
looks exactly like a release-blocking sequencer regression, and it cost a full investigation to find that
the project had a **`CHA 40` on one note** — a coin flip, added while testing the random FX earlier in the
same session. Both engines were correct. A byte-identical render would in fact have meant the randomness
was *fake*.

The check is trivial. Nobody had automated it, so it wasn't run.

## The two modes

**Diagnostic** — `ptnondet some.ptp`. Lists every clock-seeded element in the project. Run this **before
any KT-vs-C++ render comparison.** If it reports anything, the comparison is meaningless: the two engines
draw from different generators, and even one engine rendered twice will differ.

```
$ ptnondet testdata/device/DNB_TEST.ptp
project "DNB_TEST"  tempo=162

  RANDOM FX   phrase 03 step 06 FX1 = CHA 40  (chance gate)

NON-DETERMINISTIC — 1 source(s) of randomness above.
```

**Test** — `ptnondet <testdata>`, wired to ctest as `s7-determinism`. Asserts every golden project is on
the side of the line it belongs on: g1..g7 deterministic, g8-random not.

This guards a dependency that is otherwise **invisible**: `ptrender` asserts that two renders of
`g7-audio` are **byte-identical**, and that only holds while g7 contains nothing clock-seeded. Add a CHA
to g7 and `s6-render` goes red with *"the two renders differ"* — true, useless, and a long way from the
cause. `s7-determinism` fails first, and names it. The reverse is guarded too: strip the randomness out of
`g8-random` and `ptrandom` would be measuring nothing and would pass **vacuously**, which is worse than
failing.

## What counts as clock-seeded

| | where | why it is nondeterministic |
|---|---|---|
| **CHA / RND / RNL / ARC mode 3** | phrase FX | the random effects. Seeded from the platform (`native/songcore/rng.h`), exactly as `kotlin.random.Random.Default` is — on **both** engines. |
| **`oscShape >= 8`** (RND / DRNK LFO) | instrument mod slots | drawn from the engine's `noteSeedEntropy`, which is re-seeded from the wall clock at **every render, on purpose**. The per-render variation is the feature. |
| **DUST on the master bus** | `masterBusFx == 1` | a random-walk drift. |

Any one of them makes a render unreproducible — on Kotlin as much as on C++. That is not a defect to be
fixed; it is what those effects *are*. The only mistake is comparing bytes across one.

## Build & run

```
ctest --test-dir tools/build -R s7-determinism --output-on-failure -C Release
tools/build/ptnondet path/to/project.ptp        # the diagnostic
```

Exit 0 = deterministic / all goldens on the right side; 1 = randomness found / a golden has drifted.
