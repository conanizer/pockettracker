# ptrender — songcore S6b render conformance

A host tool (no device / NDK) that takes a golden `.ptp` all the way to a **WAV**, through the **real
engine**, with no app code at all:

```
push_project(.ptp)  →  load_media(samples + SoundFonts)  →  render_song_to_wav()
```

## Why this exists

The other four tools stop at songcore's edge. They prove the project parses (`ptroundtrip`), the pure
functions compute (`ptresolve`), the scheduler emits the right events (`ptplay`) and the consumer turns
them into the right engine calls (`ptvoice`) — and every one of them is header-only. **None of them
makes a sound.**

ptrender links the engine and renders audio. That gives two things nothing else does:

1. **A regression net for the two render bugs S6b fixed** (see `native/songcore/render.h`) — a render
   that inherited the previous render's reverb tail, and a render that truncated its own.
2. **Standing proof that songcore is platform-free.** The three calls above are exactly what the SDL
   shell will make on Linux, and exactly what Android now makes through JNI. If ptrender builds and
   renders on a bare host, the Linux port has no app-shaped hole left in it.

It is also the first thing in the repo to build the engine as part of another CMake project, which is
why the CI matrix now compiles `native/` on **gcc** and **clang** — not just the NDK's clang.

## Why it is not a byte-compare

Every other golden here is compared bit-for-bit, and the trace goldens *can* be: songcore's own
translation units are pinned to IEEE arithmetic (`-fno-fast-math -ffp-contract=off`, event-schema §5).

Audio cannot be, and this is **measured on all three CI runners, not assumed**. Take the same
`g7-audio.wav` rendered by each and diff it against the MSVC/x86-64 one, sample by sample:

| vs MSVC/x86-64 | samples differing | max delta | first divergence |
|---|---|---|---|
| **gcc/x86-64** (no `-ffast-math`) | 9.7% | **5 LSB** of 32767 (≈ −76 dBFS) | t = 1.627 s |
| **clang/arm64** (`-ffast-math`) | 17.0% | **16 LSB** of 32767 (≈ −66 dBFS) | t = 0.002 s |

Two different mechanisms, and the timings give them away:

- On **gcc/x86-64**, `g1-basics` comes out **byte-identical** — the dry path (sampler + SoundFont) *is*
  bit-reproducible across toolchains. `g7-audio` only starts to drift at **1.627 s**, once the reverb and
  delay have had time to build up: the last bits of `sin`/`exp`/`pow` are a libm implementation detail,
  and a **feedback** chain recirculates them.
- On **clang/arm64**, divergence starts at **sample 140** — 2 ms in, before any tail exists. `-ffast-math`
  reassociates the dry path too, so nothing is bit-identical from the first note onward.

Both are inaudible (16 LSB is −66 dBFS), and every energy measurement still lands inside 1 dB. But a
byte-exact audio golden would be **red on two of the three runners, today**, and demanding that it not be
would buy nothing.

So the checks are the ones that are both toolchain-proof *and* actually catch the bugs:

| | check | catches |
|---|---|---|
| **i** | **Determinism.** The same project, rendered twice through the same engine, must produce a **byte-identical file**. Same binary, so this one *is* exact. | the render inheriting engine state |
| **ii** | **Health.** Not silent; the decay tail was appended; that tail ended because the audio *decayed*, not because render.h's 30 s runaway cap fired; the file does not end at full amplitude. | the render truncating its tail |
| **iii** | **A fingerprint, tolerance-compared** — peak, RMS and per-second RMS in dBFS, ±1.0 dB. | a DSP regression: a send that stopped routing, a bypassed master EQ, an inverted gain |

The tolerance is not arbitrary. Numerical noise between toolchains moves these numbers by less than
0.01 dB; a real regression moves them by many dB. **1.0 dB sits in the empty gap between the two.**

Note the **A → B → A** shape: a *different* project is rendered in between the two `g7-audio` renders.
The bug was precisely that engine state (reverb lines, delay buffers, ReverbSc's random-lineseg LCG,
limiter envelopes) survived from one render into the next, so rendering the same project twice
back-to-back is a strictly weaker test. B also proves `prepare_render` **re-pushes** the project:
`resetEffectState()` leaves every module at its *factory defaults*, so a render that reset but forgot
to re-push would come out with default reverb and delay — and after B, that is g1's settings, not g7's.

**Negative controls, both verified:** comment out `resetEffectState()` in `prepare_render` and the two
g7 renders diverge (the tail comes out a chunk longer, and the byte-compare names the frame). Nudge one
fingerprint number by 3 dB and the tolerance check fails, naming the value. Worth knowing: the RMS drift
from the state leak is only ~0.02 dB, so the **fingerprint does not catch it** — the byte-exact
determinism check is what does the real work, and the tolerances are deliberately loose enough not to
false-positive at that scale.

## The golden media

`testdata/golden/` holds three synthesized files — see `make-golden-media.cpp`, which is their
provenance and regenerates them. The six original goldens have referenced them all along
(`golden/kick.wav`, `golden/pad.wav`, `golden/test.sf2`); the files simply never existed, which is why
those projects are described as "silent by design". Each is shaped to exercise a path:

- **kick.wav** — mono, 44100 Hz: same rate as the render, so no resampling happens.
- **pad.wav** — **stereo, 22050 Hz**: the awkward path on purpose (a second channel buffer, and a
  rate ratio of 2.0, so the resampler runs).
- **test.sf2** — a hand-built minimal SoundFont, bank 0 / preset 5, exactly what the goldens ask for.

Everything decays to silence and **nothing loops** — a sample that sustained forever would stretch the
render's tail out to its 30-second cap and make every assertion about tail length meaningless.

`g7-audio` is the project built for this tool: both send buses, the master bus (OTT, limiter, master
EQ), a per-instrument EQ, drive, a resonant filter under an LFO, a SoundFont voice, the resampled
stereo pad — and a note still ringing on its last step, so a render that truncates is *visibly* wrong.
It must stay free of anything wall-clock-seeded (CHA/RND/RNL/random-ARP, the RND/DRNK LFO shapes, DUST),
or check (i) could never hold; `GoldenProjects.kt` spells that out at the definition.

## Build & run

The five conformance tools are one CMake project (`tools/CMakeLists.txt`), wired to ctest. CI runs
exactly this on every push, on gcc/x86-64, MSVC/x86-64 and clang/arm64:

```
cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/build --config Release
ctest --test-dir tools/build --output-on-failure -C Release
```

This tool alone is the **`s6-render`** test — `ctest --test-dir tools/build -R s6-render
--output-on-failure` — or invoke the binary directly: `ptrender <testdata-dir> <output-dir>`. It leaves
the rendered WAVs in the output directory, so a failure can be *listened to*, not just read.

Exit 0 = all green, 1 = any failure.

Regenerate the fingerprints after an intentional DSP change: delete `testdata/renders/`, re-run, and
commit the new numbers **with** the change that moved them.

⚠️ `AudioEngine` **must be heap-allocated**. Its per-block DSP scratch, spectrum rings and 256-slot
table pool are members, and they blow a 1 MB stack instantly.

## What it does NOT cover

- **Cross-toolchain audio equality** — see above; that is not a thing that can be asserted.
- **DUST** (`masterBusFx = 1`) and the RND/DRNK LFO shapes, whose random walks are seeded per render on
  purpose. A determinism check cannot cover them, by construction.
- **The tables tick engine** — its events are covered by g5's traces; its audio is not rendered here.
- **m4a/AAC media**, which needs MediaCodec and has no host decoder (`AudioFormats.kt`).
