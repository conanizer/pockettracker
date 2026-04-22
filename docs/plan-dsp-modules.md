# DSP Module Architecture

**Status:** Instrument chain — implemented (April 2026). Send/master chains — stubs. Bus modulation — post-MVP.  
**Purpose:** Define how audio effects are structured internally — composable, reusable, and modulation-ready at every point in the signal chain.  
**Implementation guide:** `docs/guide-adding-effects.md`

---

## Why This Exists

The current `plan-audio-effects.md` uses flat data classes (`InstrumentFX`, `SendFX`, `MasterFX`)
with hardcoded parameter structs. This works for the planned effect set but breaks down when:

- **Composed effects are needed**: OTT requires 3 compressors + crossover filters. There is no
  clean place for this without duplicating or tangling code.
- **Primitives want to be shared**: A filter on an instrument and a filter on a send bus are the
  same DSP. The flat model forces separate implementations.
- **Bus-level modulation is added later**: The voice mod matrix writes to `modDestValues[]` and
  drives per-instrument params. A global LFO modulating reverb depth has no equivalent hook in
  a flat design.

The DSP module architecture solves this by separating *what does the DSP* (primitives),
*what the user configures* (modules), and *where it runs* (chain contexts).

---

## Terminology Note

Three separate things in this codebase share "module" as a word. They are not related:

| Name | Location | What it is |
|------|----------|------------|
| `TrackerModule` | Kotlin UI | A screen panel (PhraseEditorModule, MixerModule, etc.) |
| Mod system | C++ | Voice modulation routing (`modSourceValues[]` → `modDestValues[]`) |
| **DSP Module** | C++ | An audio effect with params + process() — this document |

Whenever this document says "module" it means the DSP Module layer.

---

## Three-Layer Model

```
┌─────────────────────────────────────────────────┐
│  Layer 3: Chain Context                         │  ← WHERE it runs
│  (Instrument / Track / Send / Master)           │
├─────────────────────────────────────────────────┤
│  Layer 2: DSP Module                            │  ← WHAT the user configures
│  (FilterModule, OTTModule, ReverbModule, …)     │
├─────────────────────────────────────────────────┤
│  Layer 1: DSP Primitive                         │  ← HOW it's built
│  (BiquadState, CompressorEnv, DelayLine, …)     │
└─────────────────────────────────────────────────┘
```

The key principle: **primitives compose into modules; modules are instantiated inside chain
contexts; the same module type can appear at any chain level.**

A `FilterModule` on a per-instrument chain and a `FilterModule` on a track chain are the same
class. A `CompressorModule` on the master bus reuses the same `EnvelopeFollower` primitive as
the upward compressor inside `OTTModule`.

---

## Layer 1: DSP Primitives

A primitive is a minimal, stateful DSP building block. It is:
- Just state + a process function
- Never directly exposed to the user
- Not responsible for parameter management or allocation
- Reusable across any number of modules

Think of primitives as transistors — useful only when assembled into something larger.

```cpp
// Primitive: biquad filter state.
// Coefficients are computed by the caller module, passed at process time.
struct BiquadState {
    float x1=0, x2=0, y1=0, y2=0;
    void reset() { x1=x2=y1=y2=0; }
};

void biquadProcess(float* buf, int n, BiquadState& s,
                   float b0, float b1, float b2, float a1, float a2);

// Primitive: envelope follower (used inside compressor modules).
struct EnvelopeFollower {
    float env = 0.0f;
    float follow(float input, float attackCoeff, float releaseCoeff);
};

// Primitive: interpolated delay line (used inside reverb, chorus, delay modules).
struct DelayLine {
    std::vector<float> buf;
    int writePos = 0;
    void init(int maxSamples);
    float readAt(float delaySamples);  // linear interpolation
    void write(float sample);
};
```

**Rules for primitives:**
- No `sampleRate` stored as a member — caller passes it when computing coefficients
- No `base[]` / `mod[]` parameter arrays — parameters are function arguments
- No output buffer allocation — caller provides buffers
- A primitive represents exactly one type of DSP operation

---

## Layer 2: DSP Modules

A module wraps one or more primitives into a user-facing effect. It:
- Owns a fixed set of float parameters via `base[]` + `mod[]` arrays
- Exposes a uniform `process(L, R, numFrames)` interface
- Is self-contained (no external dependencies beyond sampleRate at init)

The `base[]` + `mod[]` pattern is identical to the existing voice `ParamBus`:
final value = `base[i] + mod[i]`, same as `params.base[PARAM_VOL] + params.mod[PARAM_VOL]`.

```cpp
class FilterModule {
public:
    enum Param { CUTOFF=0, RES=1, TYPE=2, DRIVE=3, PARAM_COUNT };

    float base[PARAM_COUNT] = {};   // user-configured values
    float mod[PARAM_COUNT]  = {};   // accumulated per block, reset each block

    void init(float sampleRate);
    void resetMods();                   // call at start of each audio block
    void process(float* L, float* R, int numFrames);

private:
    float sr;
    BiquadState stateL, stateR;         // one biquad per channel
};
```

### Composed module example: OTT

OTT is a multiband upward+downward compressor. Internally:
1. Two crossover biquad filters split the signal into low/mid/high bands
2. Three independent compressors process each band
3. A mix stage recombines bands with the dry signal

```cpp
class OTTModule {
public:
    enum Param { DEPTH=0, TIME=1, UPWARD_GAIN=2, PARAM_COUNT };

    float base[PARAM_COUNT] = {};
    float mod[PARAM_COUNT]  = {};

    void init(float sampleRate);
    void resetMods();
    void process(float* L, float* R, int numFrames);

private:
    float sr;
    SvfState lowCrossL, lowCrossR;   // low/mid crossover (stereo)
    SvfState highCrossL, highCrossR; // mid/high crossover (stereo)
    EnvelopeFollower compEnvLow[2];     // per-channel per-band envelope followers
    EnvelopeFollower compEnvMid[2];
    EnvelopeFollower compEnvHigh[2];
    float lowBuf[MAX_BUF], midBuf[MAX_BUF], highBuf[MAX_BUF];
};
```

From outside, `OTTModule` looks identical to `FilterModule` — same `base[]`, `mod[]`,
`process()` interface. Internal complexity is hidden.

### Composed module example: gritty filter

A gritty filter is a resonant filter followed by a soft clipper — the resonance peak
clips, producing harmonic distortion:

```cpp
class GrittyFilterModule {
public:
    enum Param { CUTOFF=0, RES=1, GRIT=2, PARAM_COUNT };

    float base[PARAM_COUNT] = {};
    float mod[PARAM_COUNT]  = {};

    void init(float sampleRate);
    void resetMods();
    void process(float* L, float* R, int numFrames);

private:
    BiquadState stateL, stateR;  // filter primitive
    // soft clipper is stateless — no primitive state needed
};
```

`FilterModule` now uses `SvfState` (see `effects/primitives/svf.h`), which gives
LP/HP/BP/notch/peak from one `process()` call. The module interface is unchanged.

---

## Layer 3: Chain Contexts

A chain context defines WHERE modules run in the signal flow. Four contexts:

```
Voice synthesis (sampler / SF2 / future synth)
    │
    ▼
Instrument Chain  ← per-instrument, mono
    │                 modules: FilterModule, DriveModule, BitcrushModule
    ▼
Track Bus  ──────  [future: Track Chain per track, stereo]
    │
    ├──────────────→ Send Chain A (reverb bus)
    ├──────────────→ Send Chain B (delay bus)
    └──────────────→ Send Chain C (chorus bus)
    │                 (parallel — processes sends, returns wet signal)
    ▼  (+ returns)
Master Chain      ← final output stage, stereo
    │                 modules: EQModule, CompressorModule, OTTModule, LimiterModule
    ▼
Output
```

Each context calls its modules during `processAudioBlock`:

```cpp
// Instrument chain — mono, per instrument
void processInstrumentChain(int instrId, float* mono, int numFrames);

// Send chain — stereo, one per send bus, parallel
void processSendChain(SendBusId bus, float* inL, float* inR,
                      float* outL, float* outR, int numFrames);

// Master chain — stereo, final pass
void processMasterChain(float* L, float* R, int numFrames);
```

The same module class can appear in any context. An `EQModule` on the master bus and an
`EQModule` on a per-instrument insert are the same class, initialized at the same sample rate.

---

## Modulation Matrix Integration

### Voice level — already implemented

The per-voice mod matrix (`modSourceValues[]` → `processRoutes()` → `modDestValues[]`) is
complete. ADSR/LFO targets `PARAM_FILTER_CUT`, `PARAM_DRIVE`, etc.

An instrument chain module reads from the voice's `modDestValues[]` — it uses
`modDestValues[PARAM_FILTER_CUT]` as the cutoff, which already contains the sum of all
mod contributions. The module's own `mod[CUTOFF]` accumulator is populated by copying from
`modDestValues` before `process()` is called.

**The existing mod matrix already drives instrument chain modules. No new wiring is needed
for the instrument context.**

### Bus level — post-MVP

Send and master chains have no voice, so there is no `modSourceValues[]` to read from.
Bus-level modulation would require a separate lightweight context:

```cpp
// A global modulation context shared by all bus-level chains.
struct BusModContext {
    float lfoPhase = 0.0f;
    float lfoValue = 0.0f;  // -1..+1 sine

    // Called once per processAudioBlock before any chain processes.
    void tick(float lfoHz, int numFrames, float sampleRate);
};

// A bus-level mod route (mirrors the per-voice ModRoute struct).
struct BusModRoute {
    BusModSrcId source;   // e.g., BUS_MOD_LFO, BUS_MOD_SIDECHAIN
    int         moduleIndex;
    int         paramIndex;
    float       depth;
};
```

Example use: global LFO modulating reverb depth. The `BusModContext.lfoValue` writes to
`ReverbModule.mod[DEPTH]` each block. The modulation loop is identical to the per-voice
`processRoutes()` — different scope, same pattern.

**For MVP: send and master chain params are static (user-set, not modulated). Bus-level
modulation is post-MVP.**

---

## Relationship to Existing Plans

### `plan-module-system.md`

That plan describes the per-voice modulation routing system. It is the *modulation layer*
that drives DSP module parameters at the instrument chain level. The two are complementary:

```
plan-module-system.md  →  modDestValues[PARAM_FILTER_CUT]
                                    ↓
plan-dsp-modules.md    →  FilterModule.mod[CUTOFF] = modDestValues[PARAM_FILTER_CUT]
                                    ↓
                           FilterModule.process() uses base[CUTOFF] + mod[CUTOFF]
```

---

## Implementation Status

### Done (April 2026)

- ✅ `effects/primitives/biquad.h` — `BiquadState` (state-only, coefficients passed at call time; kept for future use)
- ✅ `effects/primitives/svf.h` — `SvfState` (two-integrator-loop SVF; LP/HP/BP/notch/peak from one process() call)
- ✅ `effects/modules/filter-module.h` — `FilterModule` (LP/HP/BP SVF; `setParams()` + `processMono/Stereo`)
- ✅ `effects/modules/drive-module.h` — `DriveModule` (tanh soft clipper, stateless)
- ✅ `effects/modules/crush-module.h` — `BitcrushModule` (bit-depth quantizer, stateless)
- ✅ `effects/instrument-chain.h` — `InstrumentChain` wiring all three modules (Crush → Drive → Filter)
- ✅ `effects/send-chain.h` — stub (silence output)
- ✅ `effects/master-chain.h` — stub (no-op pass-through)
- ✅ All sampler and SF voices use `InstrumentChain`; audio-engine.cpp call sites unchanged

### Post-MVP

- `EnvelopeFollower` primitive (needed for compressor/OTT)
- `DelayLine` primitive (needed for reverb/delay/chorus)
- Send chain modules: `ReverbModule`, `DelayModule`, `ChorusModule`
- Master chain modules: `EQModule`, `CompressorModule`, `LimiterModule`
- `OTTModule` (composed from SvfState crossovers + EnvelopeFollower)
- Track chain context (after per-track buffers from `unified-audio-abstraction.md`)
- `BusModContext` + bus-level mod routes
- DaisySP SVF swap in `FilterModule` (see swap comment in `filter-module.h`)

---

## Summary

| Layer | Name | Role | User-visible? | Modulation target? |
|-------|------|------|---------------|--------------------|
| 1 | Primitive | Minimal stateful DSP | No | No |
| 2 | DSP Module | Effect with params + process() | Yes (via FX screen) | Yes (base[] + mod[]) |
| 3 | Chain Context | Where the module runs | Yes (routing) | Indirectly |

Adding a new effect = write one module (possibly reusing existing primitives). It slots
into any chain context without touching any other code. The modulation matrix wires to it
at voice level for free; bus-level routing extends the same pattern post-MVP.
