# Module System — Architecture Plan

**Status:** Planning  
**Branch:** `claude/audit-audio-module-system-RSQ3K`  
**Prerequisite reading:** `docs/unified-audio-abstraction.md`  
**Reference designs:** SunVox 1.3b source, Surge XT, Polyhedrus (Serum-style), VCV Rack

---

## Goal

Restructure the internal audio architecture so that every value-changing module
(LFO, ADSR, table effects, pitch slides, vibrato) writes to a shared source array,
and every sound module (sampler, SF2, future synth) reads from a shared
destination array. The two arrays are connected by a routing table.

**The key property:** adding a new sound module or a new modulation source
costs one enum entry and one read/write. Nothing else needs to change.
No existing effect code is touched. No duplicated wiring.

**The UI does not change.** The instrument screen still shows 4 mod slots with
TYPE / DEST / VALUE / RATE / DEPTH fields. This is purely an internal
restructuring.

---

## Why the current code is not this yet

The current `ParamBus` (`base[]` + `mod[]`) is the right idea but sources still
reach destinations through three separate paths:

| Source | Current path | Problem |
|---|---|---|
| LFO / ADSR mod slots | `addMod(dest)` via `updateVoiceModulation` | Only called for sampler voices |
| Table Vxx | Direct `voice.tableVolume` field write | Bypasses bus |
| Table row transpose | Direct `voice.playbackRate` write | Bypasses bus |
| Table Oxx | Direct `voice.position` write | Bypasses bus |
| Vibrato (PVB/PVX) | `vibratoPhase/Speed/Depth` state, merged in `getModulatedPlaybackRate()` | Parallel system |
| PSL / PBN pitch slides | `pitchOffset` state, merged in `getModulatedPlaybackRate()` | Parallel system |
| Drive / crush / downsample | Direct `voice.drive/crush/downsample` fields | Never modulated at all |
| SF voices | All modulation paths separate from sampler | No shared code |

**Target:** all of the above write to `modSourceValues[id]`. The route table
accumulates them into `modDestValues[id]`. Every sound module reads only
from `modDestValues[]`. The routing loop is 20 lines of code with no
type-switching.

---

## Core Architecture (Polyhedrus / Surge XT pattern)

### Two arrays per voice

```cpp
// Every modulation source writes its current value here once per block.
float modSourceValues[MOD_SRC_COUNT];

// Route accumulation output. Every sound module reads from here.
// Equivalent to the current params.mod[] but populated via the route table.
float modDestValues[PARAM_COUNT];
```

This is the pattern from Polyhedrus (open-source Serum-style synth) and
Surge XT. Sources write, routes accumulate, destinations read. No source ever
"knows" what it's modulating. No destination ever "knows" what is driving it.

### ModSourceId — what produces values

```cpp
enum ModSourceId {
    MOD_SRC_NONE = 0,

    // Per-voice dynamic sources (state advances each audio block)
    MOD_SRC_LFO0, MOD_SRC_LFO1, MOD_SRC_LFO2, MOD_SRC_LFO3,
    MOD_SRC_ENV0, MOD_SRC_ENV1, MOD_SRC_ENV2, MOD_SRC_ENV3,

    // Per-note static sources (captured at note-on, constant for note's lifetime)
    MOD_SRC_VELOCITY,   // 0.0–1.0
    MOD_SRC_KEYTRACK,   // (midiNote - rootNote) / 12.0, bipolar
    MOD_SRC_RANDOM,     // random value sampled at note-on, 0.0–1.0

    // Sequencer-driven sources (written by playback system each block)
    MOD_SRC_TABLE_VOL,    // 0.0–1.0 from table row volume column
    MOD_SRC_TABLE_PITCH,  // semitones from table row transpose column
    MOD_SRC_PITCH_SLIDE,  // semitones from PSL/PBN state machine
    MOD_SRC_VIBRATO,      // −1..+1 sine from PVB/PVX state machine

    MOD_SRC_COUNT
};
```

### ParamId — what receives values (destinations)

```cpp
enum ParamId {
    // Universal audio parameters (all sound modules)
    PARAM_VOL           = 0,   // 0.0–1.0
    PARAM_PAN           = 1,   // 0.0=left, 0.5=center, 1.0=right
    PARAM_PITCH         = 2,   // semitone offset (all pitch sources converge here)
    PARAM_FILTER_CUT    = 3,   // 0–255
    PARAM_FILTER_RES    = 4,   // 0–255
    PARAM_DRIVE         = 5,   // 0–255
    PARAM_CRUSH         = 6,   // 0–15
    PARAM_DOWNSAMPLE    = 7,   // 0–15

    // Sampler-only parameters (SF / synth voices ignore these)
    PARAM_SAMPLE_START  = 8,   // 0–255 — enables wavetable-style LFO scanning
    PARAM_SAMPLE_END    = 9,   // 0–255
    PARAM_LOOP_START    = 10,  // 0–255

    PARAM_COUNT         = 11
};
```

### ModRoute — the connection (pure data, no behavior)

Directly from Polyhedrus `ModMatrix.h`:

```cpp
struct ModRoute {
    ModSourceId source;    // which source drives this route
    ParamId     dest;      // which parameter receives the value
    float       depth;     // signed scale: −1.0..+1.0
    ModSourceId via;       // optional secondary modulator (MOD_SRC_NONE = unused)
    float       viaAmount; // 0.0 = ignore via, 1.0 = via fully controls depth
};
```

**ViaSource is how "modulate the modulation" works — no special system needed.**
Example: velocity controls how much an LFO affects pitch:
```
route = { LFO0, PITCH, depth=1.0, via=VELOCITY, viaAmount=1.0 }
// result: pitch += LFO0_value * VELOCITY_value * 1.0
```
The formula (from Polyhedrus `ApplyRoute`):
```cpp
float val = source * ((1.0f - viaAmount) + via * viaAmount);
modDestValues[dest] += val * depth;
```
No two-pass ordering. No meta-parameter destinations. No cycle risk.
This replaces the current `MOD_AMT` / `MOD_RATE` / `MOD_BOTH` destination system
entirely.

### Route processing loop (the entire modulation engine)

```cpp
void processRoutes(
    const float* srcValues,     // modSourceValues[MOD_SRC_COUNT]
    float*       dstValues,     // modDestValues[PARAM_COUNT]
    const ModRoute* routes,     // instrument's route table (user + fixed)
    int          routeCount)
{
    memset(dstValues, 0, sizeof(float) * PARAM_COUNT);
    for (int i = 0; i < routeCount; i++) {
        if (routes[i].source == MOD_SRC_NONE) continue;
        float src = srcValues[routes[i].source];
        float via = srcValues[routes[i].via];   // 0.0 when via == MOD_SRC_NONE
        float val = src * ((1.0f - routes[i].viaAmount) + via * routes[i].viaAmount);
        dstValues[routes[i].dest] += val * routes[i].depth;
    }
}
```

This is the complete hot path. ~20 instructions × 4 routes. No branches on
instrument type. No virtual dispatch. Adding a new source = add to the enum,
write to `modSourceValues[]`. Adding a new destination = add to the enum,
read from `modDestValues[]`. The loop above never changes.

### Fixed routes vs user routes (Surge XT pattern)

```
Fixed routes (always active, instrument-type-independent):
    TABLE_VOL   → PARAM_VOL    depth=1.0   (table Vxx row volume)
    TABLE_PITCH → PARAM_PITCH  depth=1.0   (table row transpose)
    PITCH_SLIDE → PARAM_PITCH  depth=1.0   (PSL/PBN output)
    VIBRATO     → PARAM_PITCH  depth=vibratoDepth  (PVB/PVX output)

User routes (4 slots, configured in instrument mod matrix, stored in Instrument):
    Slot 0..3: { source, dest, depth, via, viaAmount }
```

Fixed routes never appear in the UI — they just exist and fire when the
sequencer activates them. User routes are the 4 mod slots the user configures
in the instrument screen. Nothing about the UI layout changes.

---

## Per-Sample vs Per-Block — Sub-Block Interpolation

All parameters are computed at block rate by the route processing loop. A
**sub-block interpolation layer** then smooths every parameter between
consecutive block values, giving per-sample precision without per-sample
modulation logic.

### How it works

```cpp
// VoiceModState carries both current and previous block values:
float modDestValues[PARAM_COUNT];      // filled by processRoutes() each block
float prevModDestValues[PARAM_COUNT];  // snapshot from previous block

// At block start: snapshot previous values
memcpy(prevModDestValues, modDestValues, sizeof(float) * PARAM_COUNT);

// processRoutes() fills modDestValues[] with new accumulated values.

// Per-sample mix loop: interpolate between prev and current
float t = (float)(i + 1) / (float)numFrames;  // 0.0 → 1.0 across block

float vol   = prev[PARAM_VOL]   + (curr[PARAM_VOL]   - prev[PARAM_VOL])   * t;
float pitch = prev[PARAM_PITCH] + (curr[PARAM_PITCH] - prev[PARAM_PITCH]) * t;
float cut   = prev[PARAM_FILTER_CUT] + (curr[PARAM_FILTER_CUT] - prev[PARAM_FILTER_CUT]) * t;
// ... all PARAM_COUNT values
```

This is the identical formula already used in the existing `prevEnvValue →
envValue` VOL interpolation — generalized to every parameter at once.

### What this replaces

- The "dual-layer VOL" complexity is gone. All parameters use one path.
- The `prevEnvValue` / `envValue` fields on `VoiceModSlot` become redundant:
  interpolation now happens at the destination, not the source.
- No per-parameter special-casing. Adding `PARAM_DRIVE` or `PARAM_CRUSH`
  automatically gets smooth interpolation for free.

### Bonus: filter and pan also smooth

Currently filter cutoff and pan update at block rate — audible as stepping at
high LFO speeds. Sub-block interpolation makes them smooth automatically.

### Cost

One extra `float prevModDestValues[PARAM_COUNT]` per voice = 44 bytes.
Interpolation: 11 multiply-adds per sample per voice ≈ 0.1% CPU on Miyoo Flip.

---

## File Structure

`native-audio.cpp` is 3700 lines containing everything: filter math, sampler,
SF2, table engine, modulation, audio loop, and JNI. Each logical module belongs
in its own file — this makes the code navigable, testable per-module, and
enforces the portability boundary (no Android imports in audio logic).

```
app/src/main/cpp/
│
│  ── Portable audio logic (no Android / Oboe / JNI dependencies) ──────────
│
├── filter.h              (header-only)
│     calculateBiquadCoeffs(), applyBiquad(), BiquadState struct
│     Currently: lines 48–125
│
├── effects.h             (header-only)
│     applyDrive(), applyCrush(), applyDownsample()
│     Currently: inline inside AudioEngine mix loop
│
├── note-queue.h          (header-only)
│     ScheduledNote, ScheduledKill, NoteQueue, KillQueue
│     Currently: lines 147–346
│
├── table-engine.h/.cpp
│     TableRow, Table structs + table tick/advance logic
│     Currently: lines 369–408 + embedded in AudioEngine
│
├── mod-system.h/.cpp
│     ModSourceId, ParamId, ParamBus, ModRoute, VoiceModState
│     processRoutes(), LFO tick, ADSR tick
│     Currently: lines 409–452 + updateVoiceModulation() in AudioEngine
│
├── audio-voice.h         (header-only)
│     IAudioVoice abstract class, InstrumentModSlot, InstrumentParams
│     Currently: lines 325–508
│
├── sampler-voice.h/.cpp
│     Voice : IAudioVoice  (sampler implementation)
│     Currently: lines 509–899
│
├── soundfont-voice.h/.cpp
│     SoundfontVoice : IAudioVoice, SoundfontEntry, soundfonts[] pool
│     Currently: lines 126–146 + 900–1108
│
│  ── Android-specific (Oboe + JNI) ─────────────────────────────────────
│
├── audio-engine.h/.cpp
│     AudioEngine : oboe::AudioStreamDataCallback
│     processAudioBlock(), triggerNote(), mix loop
│     Includes all modules above, owns the per-track buffers
│     Currently: lines 1109–3003 (~1900 lines → shrinks as modules move out)
│
├── jni-bridge.cpp
│     All JNIEXPORT functions — thin wrappers calling AudioEngine methods
│     Currently: lines 3004–3699
│
└── CMakeLists.txt        (add all new .cpp files here)
```

### Dependency order (no cycles)

```
filter.h ──────────────────────────────────────────────────┐
effects.h ─────────────────────────────────────────────────┤
note-queue.h ──────────────────────────────────────────────┤
mod-system.h ──────────────────────────────────────────────┤
                                                            ▼
table-engine ──── mod-system.h                        audio-voice.h
audio-voice.h ─── mod-system.h, filter.h                   │
sampler-voice ─── audio-voice.h, filter.h, effects.h        ├── sampler-voice
soundfont-voice ─ audio-voice.h, tsf.h                      ├── soundfont-voice
                                                            │
                                            audio-engine ───┤ (owns everything)
                                            jni-bridge ─────┘
```

Every file above the `audio-engine` line has zero Android/Oboe/JNI includes —
fully portable, compilable on Linux without the NDK. `jni-bridge.cpp` is the
only file that needs to know about JNI types.

### Split is independent of the module system refactor

The file split can be done before, during, or after the modulation phases —
it is purely mechanical (move code, update includes, add to CMakeLists.txt).
No behavior changes. Recommended to do it first so each phase touches a
single focused file rather than one 3700-line monolith.

---

## Implementation Phases

### Phase 1 — Source array infrastructure

**Goal:** Add `modSourceValues[]` and `modDestValues[]` arrays to voices.
Wire all existing code to write/read through them.

1. Add `float modSourceValues[MOD_SRC_COUNT]` and `float modDestValues[PARAM_COUNT]`
   to `Voice` and `SoundfontVoice`.

2. At note-on, populate static sources:
   ```cpp
   modSourceValues[MOD_SRC_VELOCITY] = note.volume;
   modSourceValues[MOD_SRC_KEYTRACK] = (midiNote - rootMidi) / 12.0f;
   modSourceValues[MOD_SRC_RANDOM]   = randomFloat();
   ```

3. Route existing `VoiceModSlot` evaluation through the new arrays:
   - LFO/ADSR/AHD compute their value → write to `modSourceValues[MOD_SRC_LFO0 + i]`
     or `modSourceValues[MOD_SRC_ENV0 + i]`
   - Remove the current `addMod(dest, value)` call inside `updateVoiceModulation`
   - Instead, the route processing loop handles `addMod` after all sources
     have written their values

4. Call `processRoutes()` after all source values are updated, before mix loop.

5. Mix loop and pitch calculation read from `modDestValues[]` instead of
   `params.mod[]` directly. (`params.base[]` is still used for user-set
   instrument defaults.)

**Files:** `native-audio.cpp`  
**Risk:** Medium — refactors `updateVoiceModulation` core, but behavior unchanged.

---

### Phase 2 — Route all bypass paths

**Goal:** Make table effects, pitch slides, and vibrato write through the source
array instead of directly to voice fields.

1. **Table Vxx:** `voice.tableVolume = x` → `voice.modSourceValues[MOD_SRC_TABLE_VOL] = x`
2. **Table row transpose:** direct `playbackRate` write →
   `voice.modSourceValues[MOD_SRC_TABLE_PITCH] = semitones`
3. **PSL/PBN:** `pitchOffset` field → `voice.modSourceValues[MOD_SRC_PITCH_SLIDE] = offset`
   (State machine logic unchanged; only the *output* routes through the array.)
4. **PVB/PVX:** `vibratoPhase/Speed/Depth` state →
   `voice.modSourceValues[MOD_SRC_VIBRATO] = sinf(phase) * depth`

All four become fixed routes (depth=1.0) from the source to `PARAM_PITCH` or
`PARAM_VOL`. The `pitchOffset` field and `tableVolume` field are removed.
`getModulatedPlaybackRate()` reads only `modDestValues[PARAM_PITCH]`.

**Result:** SF voices and future synths automatically receive table transpose,
pitch slides, and vibrato — the route processing loop handles them the same as
LFO or envelope, because all are just values in `modSourceValues[]`.

**Files:** `native-audio.cpp`  
**Risk:** Low — logic identical, only write target changes.

---

### Phase 3 — Expand destination parameters

**Goal:** Make drive, crush, downsample, and sample points modulatable.

1. Extend `ParamId` to include `PARAM_DRIVE`, `PARAM_CRUSH`, `PARAM_DOWNSAMPLE`,
   `PARAM_SAMPLE_START`, `PARAM_SAMPLE_END`, `PARAM_LOOP_START`.
2. Set their base values from `InstrumentParams` at note-on:
   `params.setBase(PARAM_DRIVE, instrParams.drive)` etc.
3. Mix loop reads `params.base[PARAM_DRIVE] + modDestValues[PARAM_DRIVE]` for
   actual drive value. Same for crush and downsample.
4. `PARAM_SAMPLE_START`: mix loop reads `(int)(base + dest) / 255 * sampleLength`
   as dynamic `actualStart` — enables wavetable-style LFO scanning.

**Result:** Any existing LFO/ADSR can now target DRIVE or CRUSH just by changing
`dest` in a route. No other code changes.

**SF2 note on DRIVE / FILTER / CRUSH:**  
TSF's `tsf_channel_midi_control()` does NOT implement CC#74 (filter cutoff),
CC#71 (filter resonance), or any envelope/LFO CCs. SF2 synthesis parameters
are baked into the preset and not runtime-controllable via TSF's API.

The only path for filter/drive/bitcrush on SF voices is **post-render**: apply
our own biquad / tanh / bitcrush to the TSF output buffer after `tsf_render_float()`
returns. This is implemented in Phase 5 alongside the SF `modDestValues` read.  
Limitation: currently `tsf_render_float()` mixes all 8 tracks together, so
effects are per-SF2-file rather than per-track. Per-track isolation requires
Phase 2b (per-track TSF buffers) from `unified-audio-abstraction.md`.

`PARAM_SAMPLE_START`, `PARAM_SAMPLE_END`, `PARAM_LOOP_START` are silently
ignored by SF voices — no special-casing needed, SF voice simply doesn't read
those params.

**Files:** `native-audio.cpp`  
**Risk:** Low.

---

### Phase 4 — SCALAR source type

**Goal:** Add a source that outputs a constant 00–FF value every block.

`SCALAR` is a new source that writes a fixed value to `modSourceValues[MOD_SRC_LFOx]`
(it reuses an LFO slot with `type=SCALAR`, since it is just a degenerate LFO
that never oscillates). Implementation in `updateVoiceModulation`:

```cpp
case MOD_SCALAR:
    mod.envValue = mod.amount;  // constant; no advance needed
    break;
```

Then it writes to `modSourceValues` like any other type.

**Use case:** "Always add +7 semitones to pitch" — a SCALAR mod slot with
`source=LFO0 (type=SCALAR, value=7/12)` and `dest=PITCH`. Another route can
`via=ENV0` to make the scalar fade in with an envelope.

**Data model:** New `type=SCALAR` constant in Kotlin `ModType`; new field
`scalarValue: Int` in `InstrumentModSlot`. Existing saves deserialize
unknown types as `NONE`.

**Files:** `native-audio.cpp`, `TrackerData.kt`, mod type constants.  
**Risk:** Low.

---

### Phase 5 — SF and future voices read modDestValues[]

**Goal:** `SoundfontVoice` reads `modDestValues[]` for all parameters,
making every modulation source automatically work with SF2 instruments.

1. Move `VoiceModSlot voiceMods[4]` up to `IAudioVoice` base (or a shared
   struct), so `updateVoiceModulation()` runs identically for sampler and SF.
2. `SoundfontVoice::applyPitchMod()` reads
   `params.base[PARAM_PITCH] + modDestValues[PARAM_PITCH]` for total pitch offset.
3. SF volume reads `params.base[PARAM_VOL] * (1 + modDestValues[PARAM_VOL])`.
4. Drive/crush/downsample applied post-render on SF output buffer.

**Result:** An ADSR targeting VOL works on SF instruments with zero SF-specific
code. Table transpose, pitch slides, vibrato — all arrive via fixed routes
already written by Phase 2.

**Files:** `native-audio.cpp` (significant IAudioVoice, Voice, SoundfontVoice change).  
**Risk:** Medium-high. Implement last, after Phases 1–4 are tested.

---

### Phase 6 — Per-channel TSF rendering (TSF fork patch)

**Goal:** Each track gets its own output buffer from TSF, enabling per-track
filter/drive/bitcrush on SF instruments.

**The fix:** `tsf_render_float` iterates all voices. Each `tsf_voice` already
has a `playingChannel` field. Add one function to `tsf.h`:

```c
TSFDEF void tsf_render_float_channel(tsf* f, int channel,
                                     float* buffer, int samples, int flag_mixing)
{
    struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
    if (!flag_mixing) TSF_MEMSET(buffer, 0, (f->outputmode == TSF_MONO ? 1 : 2)
                                            * sizeof(float) * samples);
    for (; v != vEnd; v++)
        if (v->playingPreset != -1 && v->playingChannel == channel)
            tsf_voice_render(f, v, buffer, samples);
}
```

4 lines added to `tsf.h`. Zero extra memory, no separate TSF instances, no
crash risk on Miyoo Flip. One shared TSF instance, 8 separate channel renders.

**In `processAudioBlock`:**  
Replace the single `tsf_render_float()` call with a loop calling
`tsf_render_float_channel(h, t, trackBuffers[t], numFrames, 0)` for each active
track. Each track buffer is then available for per-track effect processing.

**Files:** `tsf.h` (fork patch), `native-audio.cpp`.  
**Risk:** Low.

---

### Phase 7 — Apply instrument effects to per-channel SF buffers

**Goal:** SF instruments get the same filter/drive/bitcrush as sampler instruments,
configured per-instrument, not shared across the whole SF2 file.

**No new data model.** `Instrument` already has `filterType`, `filterCut`,
`filterRes`, `drive`, `crush`, `downsample`. These are the instrument's effects —
they belong to the instrument, not to a track layer.

**The problem Phase 6 solves:** Previously `tsf_render_float()` mixed all 8
tracks into one buffer, so track 1's filter affected track 2's audio too (they
shared the same SF2 file). With `tsf_render_float_channel()` each track has its
own buffer.

**What Phase 7 adds:** After rendering track `t`'s SF buffer, look up which
instrument is playing on that track and apply its `filter/drive/crush` to the
buffer before mixing:

```cpp
for (int t = 0; t < 8; t++) {
    if (!sfVoices[t].isActive) continue;
    tsf_render_float_channel(h, t, trackBuffer[t], numFrames, 0);

    // Apply the instrument's effects to this track's output
    const InstrumentParams& p = sfVoices[t].instrParams;
    if (p.drive > 0)    applyDrive(trackBuffer[t], numFrames, p.drive);
    if (p.crush > 0)    applyCrush(trackBuffer[t], numFrames, p.crush);
    if (p.filterType)   applyBiquad(trackBuffer[t], numFrames, sfVoices[t].filterState, p);

    // Mix into master
    mixTrackBuffer(trackBuffer[t], numFrames, t);
}
```

Track 1 using a trumpet preset and track 2 using strings from the same SF2 file
now have fully independent filter and drive settings, because effects are applied
per-channel buffer, not to the combined output.

**Memory:** 8 track float buffers × numFrames × 2ch × 4 bytes ≈ 16KB. Negligible.

**Files:** `native-audio.cpp` only — `SoundfontVoice` gains a `filterState`
biquad struct and `instrParams` copy (same fields `Voice` already has).  
**Risk:** Low.

---

### Phase 8 — SF preset parameter editing

**Goal:** Expose SF2 preset parameters (envelopes, filter, modulation routing)
on the instrument screen for static editing.

**How it works:** TSF parses SF2 generators into `tsf_region` structs at load
time. Each region contains:
- `ampenv` / `modenv` — `{delay, attack, hold, decay, sustain, release}` structs
- `initialFilterFc` / `initialFilterQ` — filter cutoff and resonance defaults
- `modEnvToPitch`, `modEnvToFilterFc`, `modLfoToFilterFc`, `modLfoToVolume` —
  modulation routing amounts baked into the preset
- `pan`, `attenuation`, `transpose`, `tune`

Add two functions to the TSF fork:

```c
// Read first region's parameters (for display as defaults)
const struct tsf_region* tsf_get_preset_region(tsf* f, int bank, int preset_number);

// Patch all regions for a preset with user overrides (-1 / NULL = keep original)
void tsf_preset_apply_overrides(tsf* f, int bank, int preset_number,
                                const struct tsf_envelope* ampenv,
                                int filterFc, int filterQ);
```

**In Instrument data model:**
```kotlin
data class SFOverrides(
    val ampAttack:  Int = -1,  // -1 = use SF2 default
    val ampDecay:   Int = -1,
    val ampSustain: Int = -1,
    val ampRelease: Int = -1,
    val filterCut:  Int = -1,
    val filterRes:  Int = -1
)
// Instrument gains: val sfOverrides: SFOverrides = SFOverrides()
```

Overrides applied by calling `tsf_preset_apply_overrides` after SF2 loads or
when the instrument is edited.

**These parameters are static** — they set the starting values inside TSF's
synthesis engine. They are not part of the real-time modulation system (cannot
be targeted by mod slots). Think of them as "preset customization" rather than
"modulation destinations."

**Benefit:** Useful SF2 instruments can be tuned per-instrument (e.g., shorten
a piano's release for a staccato version) without needing to edit the SF2 file.

**Files:** `tsf.h` (fork), `native-audio.cpp`, `TrackerData.kt`.  
**Risk:** Low.

---

## What Does NOT Change

### Sequencer-level effects (no phase handles these — they stay as-is)

These schedule *events*, not continuous parameter changes. They are not
value-changing modules.

- **Arpeggio (Axx)** — schedules new noteOn calls at different pitches
- **Repeat (Rxx)** — schedules retrigger events
- **Kill (Kxx)** — schedules voice stop
- **HOP / TIC / THO** — table row flow control
- **Table Oxx** — sample position seek (one-shot, not continuous modulation)

### The UI

The instrument screen (mod slot editor) looks and behaves identically. The 4
slots still show TYPE / DEST / VALUE / RATE / DEPTH. The only future-facing
addition would be a `VIA` column for the via-source, which is optional and can
be added whenever it's useful without architectural changes.

---

## Data Model Changes Summary

| Change | Location | Backward compat |
|---|---|---|
| Add `modSourceValues[16]` + `modDestValues[11]` to Voice | `native-audio.cpp` | Internal only |
| New `ModSourceId` enum | `native-audio.cpp` | Internal only |
| Remove `pitchOffset`, `tableVolume`, `tableTranspose` fields | `native-audio.cpp` | Internal only |
| Add `PARAM_DRIVE/CRUSH/etc.` to `ParamId` | `native-audio.cpp` | Internal only |
| New `type=SCALAR` in `ModType` | `TrackerData.kt` | Unknown types → NONE |
| Add `scalarValue: Int` to `InstrumentModSlot` | `TrackerData.kt` | Defaults to 0 |
| `ModRoute` struct replacing `VoiceModSlot` destination mapping | `native-audio.cpp` | Internal only |

Saved `.ptp` project files are unaffected except for the SCALAR field (safe default).

---

## Implementation Order

```
Phase 1 (source arrays + route loop infrastructure)
    ↓
Phase 2 (route bypass paths: table, PSL/PBN, vibrato)
    ↓
Phase 3 (expand ParamId: drive, crush, sample points)
    ↓
Phase 4 (SCALAR source type)
    ↓
Phase 5 (SF parity — defer until 1–4 tested on hardware)
```

Phases 1–4 are the complete MVP. After Phase 4 the architecture fully satisfies
the "anything connects to anything" property for sampler instruments. Phase 5
extends it to SF2 and, by the same pattern, to any future synth voice.

---

## Reference Designs

| System | Key pattern borrowed |
|---|---|
| **SunVox 1.3b** | Control signals are plain values, separate from audio signal path; modules are opaque state blobs + function pointer |
| **Surge XT** | Fixed routes (always-on) vs user routes (configurable); per-voice vs scene-level source distinction |
| **Polyhedrus (Serum-style)** | `modSourceValues[]` + `modDestValues[]` two-array pattern; `ViaSource` formula for depth modulation without meta-params |
| **VCV Rack** | 1-sample delay between write and read = no graph traversal needed; process sources first, then accumulate, then synthesize |
