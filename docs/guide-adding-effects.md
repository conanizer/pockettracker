# Guide: Adding a New DSP Effect Module

**Audience:** Developers adding a new audio effect to PocketTracker  
**Architecture overview:** `docs/plan-dsp-modules.md`  
**Last updated:** 2026-04-22

---

## Quick orientation

Effects live in `app/src/main/cpp/effects/`:

```
effects/
├── instrument-chain.h      ← per-voice chain (edit this to add a module)
├── send-chain.h            ← parallel send buses (stub — future reverb/delay/chorus)
├── master-chain.h          ← final output bus (stub — future EQ/compressor/limiter)
├── primitives/
│   └── biquad.h            ← state-only biquad (use in any filter-based module)
└── modules/
    ├── filter-module.h     ← FilterModule: LP/HP/BP biquad
    ├── drive-module.h      ← DriveModule: tanh soft clipper (stateless)
    └── crush-module.h      ← BitcrushModule: bit-depth quantizer (stateless)
```

**Rule:** All processing must go through `processAudioBlock()` in `audio-engine.cpp`. Do not add processing directly to `onAudioReady()` or `renderOffline()`.

---

## Layer 1: Decide if you need a new primitive

A **primitive** (`effects/primitives/`) is pure DSP state with no parameter management:
- Use an existing primitive if you can (e.g. `BiquadState` for any biquad-based filter)
- Write a new primitive only when you need state that multiple future modules will share
  (e.g. `EnvelopeFollower` if you're building a compressor family, `DelayLine` for delay/reverb/chorus)

A **stateless transform** (like drive or crush) needs no primitive at all — write it directly in the module.

---

## Layer 2: Write the module

Create `effects/modules/your-module.h`. Follow the pattern of an existing module.

### Stateless module (no persistent state)

Copy the pattern from `drive-module.h`:

```cpp
#pragma once
#include <cmath>

struct YourModule {
    int param = 0;   // 0 = bypass

    void reset() { param = 0; }

    bool enabled() const { return param > 0; }

    inline float processMono(float in) const {
        if (!enabled()) return in;
        // ... your DSP here
        return out;
    }

    inline void processStereo(float& L, float& R) const {
        if (!enabled()) return;
        // ... your DSP here (same formula for both channels)
    }
};
```

### Stateful module (e.g. filter, chorus, delay)

Copy the pattern from `filter-module.h`. For a filter built on `BiquadState`:

```cpp
#pragma once
#include "../primitives/biquad.h"

struct YourFilterModule {
    // Parameters (set once per block when they change)
    int   type = 0;
    float b0=1, b1=0, b2=0, a1=0, a2=0;
    BiquadState stateL;   // mono or left channel
    BiquadState stateR;   // right channel (stereo only)

    void reset() {
        type = 0;
        b0=1; b1=b2=a1=a2=0;
        stateL.reset();
        stateR.reset();
    }

    // Call once per audio block when parameters change
    void setParams(int newType, int cutoff, int resonance, float sampleRate) {
        type = newType;
        // compute b0/b1/b2/a1/a2 from cutoff and resonance
    }

    bool enabled() const { return type != 0; }

    inline float processMono(float in) {
        if (!enabled()) return in;
        return stateL.process(in, b0, b1, b2, a1, a2);
    }

    inline void processStereo(float& L, float& R) {
        if (!enabled()) return;
        L = stateL.process(L, b0, b1, b2, a1, a2);
        R = stateR.process(R, b0, b1, b2, a1, a2);
    }
};
```

**Key design rules:**
- One `BiquadState` per channel. Sampler voices are mono and use only `stateL`; SF voices are stereo and use both.
- `setParams()` is called once per audio block, not per sample. Move coefficient computation there.
- `processMono()` / `processStereo()` are called per sample inside the mix loop — keep them branchless and inline where possible.

---

## Layer 3: Wire into InstrumentChain

Edit `effects/instrument-chain.h`:

```cpp
#pragma once
#include "modules/filter-module.h"
#include "modules/drive-module.h"
#include "modules/crush-module.h"
#include "modules/your-module.h"   // ← ADD THIS

// Signal order: Crush → Drive → Filter → Your Module
struct InstrumentChain {
    BitcrushModule crush;
    DriveModule    drive;
    FilterModule   filter;
    YourModule     your;           // ← ADD THIS

    void reset() {
        crush.reset();
        drive.reset();
        filter.reset();
        your.reset();              // ← ADD THIS
    }

    inline float processMono(float in) {
        in = crush.processMono(in);
        in = drive.processMono(in);
        in = filter.processMono(in);
        return your.processMono(in);   // ← ADD THIS at the right position
    }

    inline void processStereo(float& L, float& R) {
        crush.processStereo(L, R);
        drive.processStereo(L, R);
        filter.processStereo(L, R);
        your.processStereo(L, R);      // ← ADD THIS
    }
};
```

**Call sites in `audio-engine.cpp` do not change** — `processMono()` and `processStereo()` cover all voices.

---

## Step 4: Expose the parameter from audio-engine.cpp

### For modulation-driven params (ADSR/LFO can target them)

Add a `PARAM_YOUR_THING` constant in `audio-defs.h`. Then in the per-block setup section of `processAudioBlock()` (near where `effDrive` and `effCrush` are computed for sampler voices):

```cpp
int effYour = std::max(0, std::min(MAX_YOUR, (int)(
    voice.params.base[PARAM_YOUR_THING] + voice.modDestValues[PARAM_YOUR_THING])));
voice.chain.your.param = effYour;
```

For SF voices, set `sv.chain.your.param = sv.instrParams.yourThing` at trigger time (in the SF trigger block around line 390 of `audio-engine.cpp`).

### For static instrument params (no modulation)

Set the param directly at trigger time from `instrParams`:

```cpp
// Sampler voice trigger (in the scheduleNote path)
voice.chain.your.param = instrParams.yourThing;

// SF voice trigger (around line 390)
sv.chain.your.param = sv.instrParams.yourThing;
```

### For stateful modules with coefficients (like FilterModule)

Call `setParams()` at trigger time to initialize, and again inside `processAudioBlock()` when modulation is active:

```cpp
// At trigger
voice.chain.your.setParams(type, param1, param2, sampleRate);

// Per block in processAudioBlock, when modulation changes the params
if (voice.chain.your.enabled() && someModIsActive) {
    voice.chain.your.setParams(voice.chain.your.type, modParam1, modParam2, sampleRate);
}
```

---

## Checklist

- [ ] `effects/modules/your-module.h` created
- [ ] `effects/instrument-chain.h` updated: `#include`, member, `reset()`, `processMono()`, `processStereo()`
- [ ] Parameter exposed in `audio-engine.cpp` (sampler block setup + SF trigger)
- [ ] If using `BiquadState`: `reset()` clears state, `setParams()` recomputes coefficients once per block

---

## Known unfinished items (post-MVP)

### Sampler as a DSP module

The sampler (sample playback, interpolation, loop/reverse logic) is today hardcoded inline in `processAudioBlock()`. In the three-layer architecture, it should be a DSP module — a `SamplerModule` wrapping the sample read + interpolation into a `processMono()` call, so it composes cleanly with other modules. This refactoring is deferred post-MVP; the inline code stays where it is for now.

### Track chain context

`send-chain.h` and `master-chain.h` exist as stubs. A per-track chain context (stereo, between instrument chains and the mix bus) does not exist yet. It would allow per-track EQ, compression, and effects inserts. Unblocked by the per-track stereo buffers described in `docs/unified-audio-abstraction.md`.

### Bus-level modulation

Send and master chains currently have no modulation. A `BusModContext` (global LFO/sidechain feeding `mod[]` arrays on send/master modules) is the post-MVP design — see `docs/plan-dsp-modules.md` for the design.

### Send chain modules

`send-chain.h` is a silence-output stub. Reverb, delay, and chorus are planned as the first send chain modules. Each will need at least a `DelayLine` primitive.

### DaisySP SVF swap

`FilterModule` currently uses the biquad from `filter.h` (Audio EQ Cookbook). The header documents a clean swap path to DaisySP `Svf`: replace `BiquadState` members with `daisysp::Svf`, update `setParams()` and `processMono/Stereo`. `InstrumentChain` and all call sites in `audio-engine.cpp` stay unchanged.
