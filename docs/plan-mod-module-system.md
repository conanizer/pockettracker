# Plan: Modulation Module System

## Goal

Split modulation processing logic out of `audio-engine.cpp` into focused header
files under `mods/`, mirroring the `effects/primitives/` + `effects/modules/`
pattern that already exists for the audio effects chain.

This makes `audio-engine.cpp` smaller, makes each modulator independently
readable and testable, and makes adding new mod types or oscillator shapes a
localised edit instead of a buried `switch` inside a 280-line function.

---

## Scope

### In scope (C++ state machines that write into `modSourceValues[]`)

| Current location | What it does | Target file |
|---|---|---|
| `updateVoiceModulation()` — `type==1/4` branch | AHD / DRUM envelope stages | `mods/modules/ahd-module.h` |
| `updateVoiceModulation()` — `type==2/5` branch | ADSR / TRIG envelope stages | `mods/modules/adsr-module.h` |
| `updateVoiceModulation()` — `type==3` branch | LFO phase advance + shaping | `mods/modules/lfo-module.h` |
| `updateVoicePitchMod()` — pitch slide | PSL / PBN linear slide state machine | `mods/modules/pitch-slide-module.h` |
| `updateVoicePitchMod()` — vibrato LFO | PVB / PVX sine LFO applied to pitch | `mods/modules/vibrato-module.h` |
| `updateVoiceModulation()` — mod-to-mod + processRoutes + bridge | Orchestration | `mods/mod-runner.h` |

### Shared primitive

Both `lfo-module.h` and `vibrato-module.h` need a phase-accumulator oscillator
that maps phase → output value for multiple waveform shapes. Extracting it lets
vibrato inherit all LFO shapes for free instead of being hardcoded to sine.

| Primitive | Used by |
|---|---|
| `mods/primitives/lfo-oscillator.h` | `lfo-module.h`, `vibrato-module.h` |

### Out of scope

- **Arpeggio, random, CHA, DEL** — Kotlin-side per-tick effects in
  `PlaybackController`. No C++ state machine to extract.
- **Table processing** — inline in `processAudioBlock`; tracked separately in
  development-status.md architecture debt note.
- **`mod-system.h`** — stays unchanged. It defines the data layer
  (`ParamBus`, `ModRoute`, `processRoutes`, `IAudioVoice`, `VoiceModSlot`).
  The `mods/` folder adds *processing logic* on top of it.

### SCALAR (type=6) — not included

Type=6 was added as an experiment for injecting a static scalar value (e.g.,
instrument volume 00-FF, a fixed transpose) as a modulation source. It is
currently a branch in `updateVoiceModulation` that just sets `envValue =
mod.amount`. The idea is not fully resolved — it may be better handled as a
dedicated `ModSourceId` (a static per-note source slot, like `MOD_SRC_VELOCITY`)
rather than a mod *type*. Left as-is for now; document the confusion in a comment
inside `lfo-module.h` so it gets revisited before the feature is used.

---

## Target Structure

```
app/src/main/cpp/
  mod-system.h                  ← unchanged (data structures, processRoutes, IAudioVoice)
  mods/
    primitives/
      lfo-oscillator.h          ← phase + shaping: lfoShape(phase, oscShape) → float
    modules/
      ahd-module.h              ← tickAHD(VoiceModSlot&, numFrames, rMult)
      adsr-module.h             ← tickADSR(VoiceModSlot&, numFrames, rMult)
      lfo-module.h              ← tickLFO(VoiceModSlot&, numFrames, sr, rMult)
      vibrato-module.h          ← tickVibrato(Voice&, numFrames, sr)
      pitch-slide-module.h      ← tickPitchSlide(Voice&, numFrames)
    mod-runner.h                ← runModMatrix(IAudioVoice&, numFrames, sr)
```

All files are header-only (`inline` free functions). No new `.cpp` files.
No CMakeLists.txt changes required.

---

## File Contracts

### `mods/primitives/lfo-oscillator.h`

```cpp
// lfoShape(phase, shape) — returns the oscillator output for the given phase.
//   phase : 0.0 to 2π
//   shape : 0=TRI  1=SIN  2=RMP+  3=RMP-  6=SQU+  7=SQU-
//           (EXP+/EXP-/RND/DRUNK not yet implemented — fall back to SIN)
// Returns −1.0 to +1.0 for all shapes.
inline float lfoShape(float phase, int shape) { ... }
```

Current LFO shape table is in `updateVoiceModulation()` starting at the
`switch (mod.oscShape)` line. Vibrato will call `lfoShape(vibratoPhase, shape)`
and drop its hardcoded `sinf(vibratoPhase)`.

---

### `mods/modules/ahd-module.h`

```cpp
// tickAHD — advance one audio block for an AHD or DRUM mod slot (type 1 or 4).
// rMult : effective rate multiplier from mod-to-mod routing (1.0 = no mod).
// Updates mod.envValue and mod.stage in place.
inline void tickAHD(VoiceModSlot& mod, int numFrames, float rMult) { ... }
```

Stages: 1=Attack (0→1), 2=Hold (stay at 1), 3=Decay (1→0), 4=done.
Source: `updateVoiceModulation()` lines tagged `type == 1 || type == 4`.

---

### `mods/modules/adsr-module.h`

```cpp
// tickADSR — advance one audio block for an ADSR or TRIG mod slot (type 2 or 5).
// rMult : effective rate multiplier from mod-to-mod routing.
// Updates mod.envValue and mod.stage in place.
inline void tickADSR(VoiceModSlot& mod, int numFrames, float rMult) { ... }
```

Stages: 1=Attack, 2=Decay, 3=Sustain, 4=Release, 5=done.
Source: `updateVoiceModulation()` lines tagged `type == 2 || type == 5`.

---

### `mods/modules/lfo-module.h`

```cpp
// tickLFO — advance one audio block for an LFO mod slot (type 3).
// sr    : sample rate (needed for Hz → radians/frame conversion).
// rMult : rate multiplier from mod-to-mod routing.
// Updates mod.lfoPhase and mod.envValue in place.
inline void tickLFO(VoiceModSlot& mod, int numFrames, float sr, float rMult) { ... }
```

Uses `lfoShape(mod.lfoPhase, mod.oscShape)` from the primitive.

SCALAR (type=6) stays as its own short branch in `mod-runner.h` rather than
here, with a comment flagging it for future rethinking.

---

### `mods/modules/pitch-slide-module.h`

```cpp
// tickPitchSlide — advance pitch slide / bend state (PSL / PBN effects).
// Updates voice.pitchOffset and voice.pitchSliding in place.
// Must be called before updateVoiceModulation so MOD_SRC_PITCH_SLIDE is
// written before processRoutes runs.
inline void tickPitchSlide(Voice& voice, int numFrames) { ... }
```

Source: `updateVoicePitchMod()` pitch-slide block.
Writes `voice.modSourceValues[MOD_SRC_PITCH_SLIDE]` at the end (same as now).

---

### `mods/modules/vibrato-module.h`

```cpp
// tickVibrato — advance vibrato LFO state (PVB / PVX effects).
// Updates voice.vibratoPhase in place; writes MOD_SRC_VIBRATO.
// Uses lfoShape() so all oscillator shapes are available (currently sine only,
// but the primitive is now in place for shape selection).
inline void tickVibrato(Voice& voice, int numFrames, float sr) { ... }
```

Source: `updateVoicePitchMod()` vibrato block.
Writes `voice.modSourceValues[MOD_SRC_VIBRATO]`.

---

### `mods/mod-runner.h`

```cpp
// runModMatrix — full modulation block update for one voice.
// Replaces the body of AudioEngine::updateVoiceModulation().
// Call order within processAudioBlock must not change:
//   1. updateVoicePitchMod (writes PITCH_SLIDE + VIBRATO sources)
//   2. runModMatrix (reads all sources, advances ENV/LFO, calls processRoutes)
inline void runModMatrix(IAudioVoice& voice, int numFrames, float sr) { ... }
```

Sequence inside `runModMatrix`:
1. Snapshot `prevModDestValues`
2. Clear dynamic source slots (ENV0-3, LFO0-3)
3. Mod-to-mod routing — compute `effectiveAmt` / `effectiveRateMult` per slot
4. Tick each active mod slot: `tickAHD` / `tickADSR` / `tickLFO` / SCALAR branch
5. Write `envValue` → `modSourceValues[]`
6. Build routes array (user routes + 4 fixed sequencer routes)
7. `processRoutes()`
8. Bridge: `params.mod[p] = modDestValues[p]` for all params

---

## After the Refactor: Thin Shells in `audio-engine.cpp`

```cpp
void AudioEngine::updateVoiceModulation(IAudioVoice& voice, int numFrames, float sr) {
    runModMatrix(voice, numFrames, sr);
}

void AudioEngine::updateVoicePitchMod(Voice& voice, int numFrames, float sr) {
    tickPitchSlide(voice, numFrames);
    tickVibrato(voice, numFrames, sr);
}
```

`audio-engine.cpp` loses ~300 lines of switch-case logic. The function
declarations in `audio-engine.h` stay unchanged — callers don't need to know
about the split.

---

## Implementation Order

Each step is independent and buildable on its own. Test between steps.

- [x] **Step 1** — Create `mods/primitives/lfo-oscillator.h`
  - Extract the `switch (mod.oscShape)` block from `updateVoiceModulation`
  - Leave original code in place (the module will call it in step 4)
  - Verify: file compiles standalone with `#include "mod-system.h"`

- [x] **Step 2** — Create `mods/modules/ahd-module.h`
  - Extract the `type==1 || type==4` branch from `updateVoiceModulation`
  - In `updateVoiceModulation`, replace that branch with `tickAHD(mod, numFrames, rMult)`
  - Build, test AHD modulation still works

- [x] **Step 3** — Create `mods/modules/adsr-module.h`
  - Extract the `type==2 || type==5` branch
  - Replace in `updateVoiceModulation` with `tickADSR(mod, numFrames, rMult)`
  - Build, test ADSR and TRIG release still work

- [x] **Step 4** — Create `mods/modules/lfo-module.h`
  - Extract the `type==3` branch, call `lfoShape()` from the primitive
  - Replace in `updateVoiceModulation` with `tickLFO(mod, numFrames, sr, rMult)`
  - Build, test LFO modulation still works

- [x] **Step 5** — Create `mods/modules/pitch-slide-module.h` and `vibrato-module.h`
  - Extract both halves of `updateVoicePitchMod()`
  - Vibrato switches from `sinf(vibratoPhase)` to `lfoShape(vibratoPhase, 1)` (shape 1 = SIN)
  - Replace body of `updateVoicePitchMod()` with `tickPitchSlide` + `tickVibrato`
  - Build, test PSL/PBN and PVB/PVX effects still work

- [x] **Step 6** — Create `mods/mod-runner.h`
  - Move remaining orchestration from `updateVoiceModulation()` into `runModMatrix()`
  - `updateVoiceModulation()` becomes a one-liner
  - Build, full playback test

---

## Non-Goals

- No behavior changes of any kind — this is pure reorganisation
- No new modulation types or destinations
- No changes to `mod-system.h`, `VoiceModSlot`, `ModRoute`, or `IAudioVoice`
- No changes to `sampler-voice.h`, `soundfont-voice.h`, or any Kotlin code
- No CMakeLists.txt changes (all header-only)
