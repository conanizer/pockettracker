# ptvoice — songcore S5 consumer conformance

A host tool (no device / NDK) that proves the C++ port of the **consumer** — everything *below* the
event-schema seam — reproduces what Kotlin's `AudioEngine.scheduleNote()` does, call for call.

## Why this exists

The conformance trace is captured at the **router**, which sits *above* the consumer. Event-schema §6
says it plainly: "everything below the seam is consumer-side derivation and never rides the trace."

So all 32 golden traces (`tools/ptplay`) can be byte-green while the C++ consumer still computes a
wrong frequency, a wrong slice window, a wrong SoundFont velocity or a wrong envelope — and the only
symptom would be *"it sounds a bit off."* That is the single largest unmeasured risk in the whole
songcore migration, and this tool closes it.

## What it checks

`app/src/test/.../trace/S5ConsumerGoldenTest.kt` drives the **real** Kotlin `AudioEngine.scheduleNote`
over a matrix of instruments and seam arguments, with a backend that records the exact engine calls it
produces, into `testdata/units/s5-consumer.txt` — floats as raw binary32 bits, so nothing passes by
being merely close.

ptvoice re-parses each case's inputs, runs them through the C++ note path
(`native/songcore/voice_derive.h`'s `plan_note_on`, instantiated on a recorder instead of the
`AudioEngine`), and byte-compares the resulting call sequence.

Because `plan_note_on` is a **template over the engine** — `AudioEngine` satisfies it as-is, no
interface, no adapter, no virtuals — this covers the whole note path, not just the arithmetic:

- the derived values: frequency, base frequency (ROOT × sampleRateRatio ÷ detune), the CUT/TRU/SLI
  slice windows, PIT/ARP shifts after slice selection, the SF root transpose + velocity derivation +
  detune-as-pitch-wheel, tick→frame conversions across tempos and sample rates, every modulation
  type × destination;
- **which** engine calls happen, **in what order** (a table push before the mod pushes before the
  note, and the SF path's envelope/filter overrides in between);
- and the **drop paths** — an empty instrument (`sampleFilePath == null`) and an unloaded SoundFont
  must schedule nothing but the tempo push that precedes the check.

## Float exactness

`note→Hz` and `detune→multiplier` come from the **generated** `native/songcore/note_tables.h`, baked
from Kotlin's own `Double pow()` output by `S5NoteTableTest` (132 reachable MIDI numbers, 256 detune
bytes). Nothing guarantees the JVM's `pow` and the device's libm agree to the last bit, and a 1-ULP
frequency error changes the resampling rate — and therefore every rendered byte. Tabulating the two
closed input domains removes the transcendental from the runtime entirely, which is event-schema §5's
"one vendored implementation on every platform" rule in its strongest form.

## Build & run

```
call "...\VC\Auxiliary\Build\vcvars64.bat"
cl /std:c++17 /EHsc /O2 /nologo tools\ptvoice\main.cpp /Fe:ptvoice.exe
ptvoice.exe testdata\units\s5-consumer.txt
```

`clang++` / `g++` work equally (`-std=c++17`). Exit 0 = all green, 1 = any mismatch; a failure prints
the Kotlin line and the C++ line side by side.

Regenerate the golden after an intentional Kotlin change: delete `testdata/units/s5-consumer.txt`,
run `gradlew :app:testDebugUnitTest`, re-run ptvoice.

## What it does NOT cover

The end-to-end audio. A green ptvoice says every note is *derived* correctly; it cannot say the engine
was driven correctly end to end. That check is the device WAV byte-diff in
`docs/internal/songcore-s5-device-test.md` §C: the same project rendered with `ENGINE = KT` and
`ENGINE = C++` must produce byte-identical WAVs.
