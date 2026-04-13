# Module System — Architecture Plan

**Status:** Planning  
**Branch:** `claude/audit-audio-module-system-RSQ3K`  
**Prerequisite reading:** `docs/unified-audio-abstraction.md`

---

## Goal

A true modular audio architecture where **value-changing modules** (LFO, ADSR,
scalar, table FX) connect to **sound module parameters** (pitch, volume, pan,
filter, drive, bitcrush) *exclusively* through the `ParamBus`.

The key property: adding a new sound module (synth, SF2, future wavetable) costs
only exposing its parameters in the bus. Every existing modulation source works
on it immediately, with no per-source special-casing.

---

## What We Have Now vs What We Need

### Current state (problems)

| Path | How it works now | Problem |
|---|---|---|
| ADSR/LFO → sampler pitch | `addMod(PARAM_PITCH)` ✅ | Working |
| ADSR/LFO → sampler vol | Per-sample `finalVol` loop ⚠️ | Correct behavior, but bypasses ParamBus |
| Table Vxx → sampler vol | Direct `voice.tableVolume` field ❌ | Bypasses ParamBus entirely |
| Table Oxx → sample start | Direct `voice.position` field ❌ | Sampler-specific, no bus path |
| Drive/crush base values | `InstrumentParams` → direct fields ❌ | Not in ParamBus, can't be modulated |
| ADSR/LFO → SF pitch | `applyPitchMod()` separate path ❌ | Bypasses ParamBus |
| ADSR/LFO → SF vol/pan/filter | Not connected at all ❌ | No path exists |
| Table FX → SF | Not connected at all ❌ | No path exists |

### Target state

Every modulation source calls `voice->params.addMod(paramId, delta)`.  
Every sound module reads `voice->params.get(paramId)` in its render path.  
No branching on instrument type in effect code.

---

## Architecture

### ParamBus (expanded)

```cpp
enum ParamId {
    // Audio parameters (sound modules read these)
    PARAM_VOL           = 0,   // 0.0–1.0 (per-sample in mix loop; base only in bus)
    PARAM_PAN           = 1,   // 0.0=left, 0.5=center, 1.0=right
    PARAM_PITCH         = 2,   // semitone offset (±N); ALL pitch sources write here
    PARAM_FILTER_CUT    = 3,   // 0–255
    PARAM_FILTER_RES    = 4,   // 0–255
    PARAM_DRIVE         = 5,   // 0–255 (pre-gain / overdrive)
    PARAM_CRUSH         = 6,   // 0–15  (bit depth reduction)
    PARAM_DOWNSAMPLE    = 7,   // 0–15  (sample rate reduction)
    PARAM_TABLE_VOL     = 8,   // 0.0–1.0 (table Vxx row volume multiplier)
    PARAM_TABLE_PITCH   = 9,   // semitone offset from table row transpose
    PARAM_SAMPLE_START  = 10,  // 0–255 (sample playback start point)
    PARAM_SAMPLE_END    = 11,  // 0–255 (sample playback end point)
    PARAM_LOOP_START    = 12,  // 0–255 (loop restart point)

    // Mod-to-mod meta-parameters (mod slots read these)
    PARAM_MOD0_AMP      = 13,  // Scales slot 0 effectiveAmt
    PARAM_MOD0_RATE     = 14,  // Scales slot 0 effectiveRateMult
    PARAM_MOD1_AMP      = 15,
    PARAM_MOD1_RATE     = 16,
    PARAM_MOD2_AMP      = 17,
    PARAM_MOD2_RATE     = 18,
    PARAM_MOD3_AMP      = 19,
    PARAM_MOD3_RATE     = 20,

    PARAM_COUNT         = 21
};
```

**Rule:** `base[]` holds the user-set (static) value. `mod[]` is the per-block
accumulator. Final value = `clamp(base[id] + mod[id])`. Reset `mod[]` at the
start of each audio block.

### Value-changing module types (VoiceModSlot)

```
Type 0  NONE     — disabled slot
Type 1  AHD      — attack / hold / decay envelope
Type 2  ADSR     — attack / decay / sustain / release envelope
Type 3  LFO      — oscillator (TRI, SIN, SAW, SQR, RND)
Type 4  SCALAR   — constant value, 00–FF mapped to destination range
Type 5  DRUM     — one-shot amplitude decay curve
Type 6  TRIG     — envelope triggered on note-on, immediate release
```

**NEW: SCALAR type.**  
A mod slot that outputs a fixed value every block. Used for:
- Static parameter offsets ("always shift pitch +7 semitones")
- Providing a constant "carrier" that other mod slots can then modulate
- Replacing magic numbers in instrument params with visible, routeable values

SCALAR slot configuration: `value` (0–255), `dest` (any ParamId).  
Another slot can target `MOD_N_AMP` to make the scalar drift over time.

### Destination mapping (slot `dest` field)

```
0  NONE
1  VOL           → PARAM_VOL
2  PAN           → PARAM_PAN
3  PITCH         → PARAM_PITCH        (semitones, ±12)
4  FINE_PITCH    → PARAM_PITCH        (cents, value ÷ 100)
5  FILTER_CUT    → PARAM_FILTER_CUT
6  FILTER_RES    → PARAM_FILTER_RES
7  SAMPLE_START  → PARAM_SAMPLE_START (0–255 normalized)
8  DRIVE         → PARAM_DRIVE
9  CRUSH         → PARAM_CRUSH
10 DOWNSAMPLE    → PARAM_DOWNSAMPLE
11 TABLE_VOL     → PARAM_TABLE_VOL
12 TABLE_PITCH   → PARAM_TABLE_PITCH
13 SAMPLE_END    → PARAM_SAMPLE_END
14 LOOP_START    → PARAM_LOOP_START
15 MOD0_AMP      → PARAM_MOD0_AMP
16 MOD0_RATE     → PARAM_MOD0_RATE
17 MOD1_AMP      → PARAM_MOD1_AMP
18 MOD1_RATE     → PARAM_MOD1_RATE
19 MOD2_AMP      → PARAM_MOD2_AMP
20 MOD2_RATE     → PARAM_MOD2_RATE
21 MOD3_AMP      → PARAM_MOD3_AMP
22 MOD3_RATE     → PARAM_MOD3_RATE
```

---

## Per-Sample vs Per-Block

This is the trickiest constraint in the implementation.

| Parameter | Update rate | Why |
|---|---|---|
| VOL | **Per-sample** | Amplitude steps at block boundaries cause audible clicks |
| PAN | Per-block | Stereo position change inaudible at ~5ms |
| PITCH | Per-block | Pitch recalculated at block start via `getModulatedPlaybackRate()` |
| FILTER_CUT / RES | Per-block | Biquad coefficient recalculation is cheap but not needed per-sample |
| DRIVE / CRUSH / DOWNSAMPLE | Per-block | Effect strength change inaudible at ~5ms |

**VOL special handling:**  
VOL modulation must remain per-sample for click-free fades. The current
interpolation (`prevEnvValue` → `envValue` over the block) must be preserved.

Two options:
1. Keep per-sample VOL loop separate, add `params.mod[PARAM_VOL]` as an
   *additional* multiplier read once per block on top of the per-sample path.
2. Route `tableVolume` into the per-sample loop as a third multiplier.

**Recommended: option 2** — merge `tableVolume` into the mix-loop volume chain
rather than fighting the per-sample vs per-block impedance mismatch.

---

## Implementation Phases

### Phase 1 — Fix existing gaps (no new features, pure correctness)

**Goal:** Make the system behave as the UAA comments already claim.

1. **Fix table volume (Vxx):**  
   Instead of `voice.tableVolume = fxValue / 255f`, call  
   `voice.params.addMod(PARAM_VOL_TABLE, delta)` — but since we're keeping
   per-sample VOL, the simplest fix is: store a `tableVolumeTarget` and apply it
   in the per-sample mix loop alongside the existing envelope chain.  
   Remove the `* voice.tableVolume` multiplier at line 2029 and integrate it as
   a fourth volume multiplier in the per-sample loop.

2. **Route drive/crush/downsample through ParamBus:**  
   At trigger time: `params.setBase(PARAM_DRIVE, instrParams.drive)`, etc.  
   In the mix loop, read `params.get(PARAM_DRIVE)` instead of `voice.drive`.  
   Enables future LFO/ADSR modulation of drive and crush.

3. **Call updateVoiceModulation on sfVoices[]:**  
   SF voices have `params` (ParamBus) and `voiceMods[]` but updateVoiceModulation
   is never called for them.  
   Add a second loop: `for sfVoices[t] if active → updateVoiceModulation()`.  
   SF render path then reads `params.get(PARAM_PITCH)` and passes to
   `applyPitchMod()` instead of its own state.

4. **Unify pitch contributions into PARAM_PITCH:**  
   Currently `getModulatedPlaybackRate()` merges two separate sources:
   ```cpp
   float pitchMod = voice.pitchOffset            // PSL/PBN/vibrato (state machine)
                  + voice.params.get(PARAM_PITCH); // LFO/ADSR (ParamBus)
   ```
   Instead, the PSL/PBN/vibrato update loop should call
   `addMod(PARAM_PITCH, delta)` each block alongside LFO/ADSR. The state
   machines (slide target, rate, vibrato phase) are preserved — only the
   *output* of each machine routes through the bus.  
   `getModulatedPlaybackRate()` then reads only `params.get(PARAM_PITCH)`.  
   **Benefit:** SF voices and future synths get PSL/PBN/vibrato for free, and
   any mod slot can modulate vibrato depth via `MOD_N_AMP` targeting a vibrato
   slot.

5. **Route table row transpose through PARAM_TABLE_PITCH:**  
   Instead of `voice.tableTranspose → direct playbackRate write`, each table
   tick calls `addMod(PARAM_TABLE_PITCH, semitones)`. The pitch recalculation
   in `getModulatedPlaybackRate()` includes `params.get(PARAM_TABLE_PITCH)`.

**Files changed:** `native-audio.cpp` only.  
**Risk:** Low. No new data structures, just routing corrections.  
**Testing:** Table Vxx still works on sampler. SF notes respond to instrument
mod slot LFO on pitch. PSL/PBN slides still function correctly.

---

### Phase 2 — Expand ParamBus and add PARAM_DRIVE/CRUSH/DOWNSAMPLE

**Goal:** Make drive, crush, and downsample modulatable by existing mod slots.

1. Extend `ParamId` enum to 16 values (audio params + mod meta-params).
2. Update `ParamBus` constructor with sensible defaults for new params.
3. In `processAudioBlock` mix loop: read `params.get(PARAM_DRIVE)` etc. instead
   of `voice.drive` directly.
4. In `updateVoiceModulation`: add cases for destinations DRIVE, CRUSH,
   DOWNSAMPLE that call `addMod()`.

**Result:** You can now add a VoiceModSlot with `dest=DRIVE, type=LFO` and get
tremolo-like drive modulation.

Also connect the sample point params:
- `PARAM_SAMPLE_START` — `ModDest.SAMPLE_START` was already defined in Kotlin
  but never implemented in C++. Now route it: mix-loop reads
  `(int)(params.get(PARAM_SAMPLE_START) / 255f * sampleLength)` as dynamic
  `actualStart`. Enables wavetable-style scanning when an LFO targets
  `SAMPLE_START`.
- `PARAM_SAMPLE_END` and `PARAM_LOOP_START` — same pattern. Makes loop point
  modulation possible (morphing loop length with an envelope).

These three are **sampler-only** params — SF voices ignore them (no-op in SF
render path).

**Files changed:** `native-audio.cpp`, `IAudioBackend.kt` (dest constants).  
**Risk:** Low.

---

### Phase 3 — SCALAR module type

**Goal:** Add a static-value mod slot that behaves like a "constant modulation
source." This is the "00–FF scale" concept.

**Implementation:**
```cpp
// In updateVoiceModulation, new case:
case MOD_SCALAR:
    if (mod.stage != 0) {  // stage=1 means active
        // value is stored in mod.amount, already normalized to dest range
        // effectiveAmt applies scaling from mod-to-mod
        voice.params.addMod(paramId, mod.amount * mod.effectiveAmt);
    }
    break;
```

**SCALAR slot data:**
- `type = 4` (SCALAR)
- `amount` = the constant value (0.0–1.0 normalized, maps to destination range)
- `dest` = any ParamId
- `stage = 1` when active, `0` when disabled
- Can be targeted by `MOD_N_AMP` from another slot to scale its contribution

**Use case example:**  
Slot 0: `SCALAR, dest=FILTER_CUT, amount=0.5` → sets cutoff to midpoint  
Slot 1: `LFO, dest=MOD0_AMP, rate=2Hz, amount=0.3` → makes cutoff oscillate

**Files changed:** `native-audio.cpp`, mod type constants in Kotlin.  
**Risk:** Low.

---

### Phase 4 — Formal mod-to-mod via ParamBus meta-params

**Goal:** Replace the current `effectiveAmt` / `effectiveRateMult` fields with
values read from `params.get(PARAM_MOD_N_AMP/RATE)`. This makes mod-to-mod
routing go through the same bus as everything else.

**Current system (problematic):**  
`updateVoiceModulation` iterates slots in order, accumulates `effectiveAmt` and
`effectiveRateMult` directly on the target slot's fields. This is a two-pass hack
inside a single function.

**New system:**

Pass 1 (meta-param slots): Process slots whose `dest` is `MOD_N_AMP` or
`MOD_N_RATE`. These write to `params.mod[PARAM_MOD_N_*]`.

Pass 2 (audio param slots): Process remaining slots. Each slot reads its
effective amplitude as:
```cpp
float slotBase = mod.amount;  // user-set depth
float slotScale = params.get(PARAM_MOD_N_AMP);  // contribution from pass 1
float effective = slotBase * slotScale;
params.addMod(audioParamId, modValue * effective);
```

**Cycle prevention:** A slot targeting `MOD_N_AMP` where N = its own slot index
is ignored (self-modulation disabled). Deeper cycles (0→1→0) are prevented by
the two-pass ordering: meta-param slots always run first.

**Result:** One LFO can modulate the rate or amplitude of any other slot by
targeting `MOD_N_AMP` / `MOD_N_RATE`, and this works uniformly for all module
types.

**Files changed:** `native-audio.cpp`, `updateVoiceModulation` function.  
**Risk:** Medium. Ordering change could affect existing instruments.  
**Mitigation:** Test with existing mod slot configurations. The two-pass approach
should be backward-compatible since pass-1 slots were already handled first
implicitly.

---

### Phase 5 — VoiceModSlot on SoundfontVoice

**Goal:** SF instruments get the same 4 mod slots as sampler, so all modulation
sources automatically work with SF2.

**Current state:** `SoundfontVoice` has `params` (ParamBus) but no `voiceMods[]`.

**Implementation:**
1. Move `VoiceModSlot voiceMods[4]` up to `IAudioVoice`.
2. `updateVoiceModulation()` signature takes `IAudioVoice&` (or a shared
   base struct), runs the same loop for both voice types.
3. `SoundfontVoice` render path reads `params.get(PARAM_PITCH)` and feeds to
   `applyPitchMod()`. Reads `params.get(PARAM_VOL)` for per-block volume scale.
4. Drive/crush/downsample on SF are applied post-render (to the SF output buffer
   before mixing), not per-sample.

**Result:** Assigning an ADSR to `dest=PITCH` works on SF instruments without
any SF-specific code.

**Files changed:** `native-audio.cpp` (significant refactor of IAudioVoice,
Voice, SoundfontVoice).  
**Risk:** Medium-high. Biggest structural change in this plan.  
**Mitigation:** Implement Phase 1–4 first. Phase 5 is independent and can be
deferred without blocking the other phases.

---

### Phase 6 — UI Shell (modulation matrix)

**Goal:** A unified UI that lets the user configure all 4 mod slots for an
instrument and see the parameter routing clearly.

**Design:**

The instrument screen gains a "MOD" section (separate scroll region or tab).

```
┌──────────────────────────────────────────────────┐
│ MOD MATRIX                                        │
│                                                   │
│ SLOT  TYPE    DEST      VALUE   RATE    DEPTH     │
│  0    LFO     PITCH     ---     4 Hz    0.5       │
│  1    ADSR    VOL       ---     A2/D4   1.0       │
│  2    SCALAR  FILTER_CUT 80     ---     1.0       │
│  3    LFO     MOD0_AMP  ---     0.1Hz   0.8       │
└──────────────────────────────────────────────────┘
```

Cursor navigation: DPAD moves between cells. A+DPAD edits value.  
TYPE cycles through: NONE → AHD → ADSR → LFO → SCALAR  
DEST cycles through all ParamId values (shown as human-readable names)

**00–FF display:** All values shown in hex (00–FF) as is standard in trackers.
`DEPTH 0x80` = 50% modulation depth. `SCALAR VALUE 0xFF` = full-scale.

**Live display:** During playback, show current `params.get(paramId)` as a
small bar to the right of each DEST label. This makes modulation visible.

**Files changed:** `InstrumentModule.kt`, `TrackerData.kt` (if mod slot data
moves to serialized instrument), `InstrumentController.kt`.  
**Risk:** Medium (UI complexity). Can be done incrementally — show existing mod
slots first, then add SCALAR, then add live display.

---

## What Does NOT Change

These stay as sequencer-level operations, not modulation sources:

- **Arpeggio (Axx)** — schedules new noteOn events at different pitches
- **Repeat (Rxx)** — schedules retrigger events
- **Kill (Kxx)** — schedules voice stop
- **HOP (Hxx), TIC (Txx), THO** — table flow control
- **PSL / PBN / PVB / PVX** — pitch state machines (persist across blocks, not
  accumulated in mod[])

These are "time-domain" effects that change what notes exist, not "value
domain" effects that modulate a running voice's parameters.

---

## Data Model Impact

`VoiceModSlot` is currently serialized as part of `Instrument` (via Kotlin data
classes). Adding SCALAR type requires:

- New `type` constant (4) in Kotlin `ModSlotType` enum/object
- New field `scalarValue: Int` (0–255) in `InstrumentModSlot` data class
- Existing saved projects will deserialize with `type=SCALAR` as `NONE` (safe
  default when unknown type is deserialized)

---

## Recommended Implementation Order

```
Phase 1  →  Phase 2  →  Phase 3  →  Phase 6 (UI)
                ↓
            Phase 4  →  Phase 5
```

Phases 1–3 + 6 are a coherent MVP deliverable: all existing effects properly
routed, drive/crush modulatable, scalar type available in UI.

Phases 4–5 are the "deep" mod-to-mod and SF-parity work — higher value but
higher risk, best done after the simpler phases are tested.

---

## Risk Summary

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Per-sample VOL click regression | Medium | High | Preserve existing VOL interpolation, only add tableVolume to the chain |
| SF crash on Miyoo Flip (1GB RAM) | Low | High | Phase 5 adds no new allocations — modsSlots are stack-allocated |
| Backward compat on serialized mods | Low | Medium | New type=SCALAR deserializes as NONE on old builds |
| Mod ordering change breaks presets | Low | Low | Two-pass is compatible with current single-pass behavior |
| Circular mod-to-mod | Low | Medium | Prevent by design: slot N cannot target MOD_N_AMP |
