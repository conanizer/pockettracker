# Unified Audio Abstraction Plan

**Status (April 2026):**
| Phase | Description | Status |
|-------|-------------|--------|
| 1 | IAudioVoice interface extraction | ✅ Done |
| 2a | ParamBus — unified parameter + modulation accumulator | ⏳ Deferred post-MVP |
| 2b | Per-instrument TSF clones + per-track render buffers | ⏳ Deferred post-MVP |
| 3 | Effect processor unification (all effects on SF via ParamBus) | ⏳ Deferred post-MVP |
| 4 | Kotlin layer cleanup (remove SOUNDFONT branches) | ⏳ Deferred post-MVP |
| 5 | Per-track stem export (WAV per track) | ⏳ Deferred post-MVP |
| 6 | Synth foundation (wavetable, FM) | 🔮 Future |

**Current workaround:** One shared `tsf*` per SF2 file, tracks mapped to MIDI channels 0-7.
Stable and crash-free, but per-track meters, tables, and arpeggio/repeat effects don't apply to SF instruments.

---

## ⚠️ Why We Are Here — The Miyoo Flip Crash History

**This section is critical context.** The current shared-handle architecture is not what was originally planned (UAA Phase 2 calls for per-instrument clones). It exists because the original per-track clone approach caused hard crashes on the Miyoo Flip, and the shared-handle approach was the stable fix. **Phase 2b must not blindly revert to the old clone strategy.**

### Original architecture (crashed)

Each track that played an SF2 note created its own `tsf*` clone via `tsf_load_memory()`:
```cpp
// On note trigger (audio thread):
if (sv.handle == nullptr) {
    sv.handle = tsf_load_memory(fileData.data(), fileData.size()); // ← THE PROBLEM
}
tsf_note_on(sv.handle, preset, midiNote, velocity);
```

**Two fatal problems:**

1. **`tsf_load_memory()` on the audio thread.** TSF parses the entire SF2 file on every clone creation — this is an expensive allocation (~milliseconds for a large SF2). Called from the real-time audio callback, it caused buffer underruns, lag, and eventual crashes on the Miyoo Flip (1 GB RAM, weak CPU).

2. **Race condition between JNI thread and audio thread.** `clearPitchMod()` was called from a JNI thread (Kotlin side) while `applyPitchMod()` ran on the audio thread — both touching the same `tsf*` handle without synchronisation. TSF is not thread-safe.

### First attempted fix (wrong root cause)

Initial diagnosis blamed the race condition in `clearPitchMod`. A `needsPitchReset` bool was added to defer the pitch reset to the audio thread. This did not fix the crashes — the real cause was `tsf_load_memory()` on the audio thread.

### Actual fix (current architecture)

Replaced per-track clones with **one shared `tsf*` per SF2 file**:
- SF2 loaded once via `tsf_load_filename()` at instrument load time (not on audio thread)
- Each track uses a dedicated MIDI channel (0-7) on the shared handle
- `tsf_render_float()` called once per slot per audio block — renders all channels mixed
- No cloning, no per-note allocation, no audio-thread file I/O

**Result:** Stable on Miyoo Flip. No crashes.

### What Phase 2b must do differently

UAA Phase 2b plans per-instrument `tsf*` clones for isolated per-track rendering. To avoid repeating the crash:

- **Load SF2 file data once** into a memory buffer on the loading thread, keep it alive
- **Create clones via `tsf_load_memory(buffer)`** from the JNI/loading thread, before any audio starts — never from the audio callback
- **Never call `tsf_load_memory()` or `tsf_load_filename()` from the audio callback**
- **Each clone is owned by one track** — no shared access, no mutex needed at render time
- **Test on Miyoo Flip specifically** — it's the weakest target device and where this class of bug surfaces first

---

## Problem

PocketTracker currently has two parallel audio code paths that share no code:

| Concern              | Sampler (WAV)               | SoundFont (SF2/SF3)              |
|---------------------|-----------------------------|----------------------------------|
| Voice lifecycle     | `Voice` pool in C++         | TSF internal voices              |
| Note trigger        | `noteQueue` → voice slot    | `noteQueue` → `tsf_channel_note_on` |
| Effects (Vxx, K00…) | `EffectProcessor` + `Voice` | Partial (kill/vol only)          |
| Track volume        | `trackVolumes[t]` at render | Set at note-on + on change       |
| Peak meters         | Per-voice at render         | Proportional estimate from SF mix|
| Note-off            | ADSR release or hard stop   | `tsf_channel_note_off`           |
| Table FX            | Supported                   | Not supported                    |
| Arpeggio / Repeat   | Supported                   | Not supported                    |

Future synth voices (wavetable, FM, subtractive) would add a third path, making maintenance O(N×effects).

The original Phase 3 plan addressed this with typed setters on `IAudioVoice` (`setVolume()`,
`setPan()`, `setPitchMod()`, etc.). This still pushes the branching into every `SoundfontVoice`
method — O(N×effects) in a different place. The **ParamBus** approach eliminates the branching
entirely: modulation sources write to a shared accumulator, each voice type reads and translates
once in `render()`.

---

## Goal

A single audio event pipeline where the sound **source** is a plugin and everything else
(effects, volume, metering, modulation, scheduling) is source-agnostic.

---

## Architecture

### ParamBus — Unified Parameter + Modulation Accumulator

Every continuously-controllable value in the system (volume, pan, pitch, filter cutoff, etc.)
is represented as a slot in a `ParamBus`. Each slot has a **base** value (set by the user /
instrument settings) and a **mod accumulator** (written by all active modulation sources
during a frame). The final value is `clamp(base + mod)`.

```cpp
enum ParamId {
    PARAM_VOL = 0,
    PARAM_PAN,
    PARAM_PITCH,       // semitones offset from note
    PARAM_FILTER_CUT,
    PARAM_FILTER_RES,
    PARAM_DRIVE,
    PARAM_CRUSH,
    PARAM_DOWNSAMPLE,
    PARAM_START_POINT, // no-op for SF/Synth
    PARAM_COUNT
};

struct ParamBus {
    float base[PARAM_COUNT];  // user-set values, written at instrument load / note trigger
    float mod[PARAM_COUNT];   // frame accumulator — reset to 0 at start of each audio block

    void resetMods()                        { memset(mod, 0, sizeof(mod)); }
    void addMod(ParamId id, float delta)    { mod[id] += delta; }
    void setBase(ParamId id, float value)   { base[id] = value; }
    float get(ParamId id) const             { return base[id] + mod[id]; }
};
```

**All modulation sources write to the bus** — tables, ADSR, LFO, effect columns:
```cpp
// Table volume column
voice->params.addMod(PARAM_VOL, tableVolNormalized);

// LFO targeting filter cutoff
voice->params.addMod(PARAM_FILTER_CUT, lfoValue * depth);

// Arpeggio offsetting pitch
voice->params.addMod(PARAM_PITCH, arpSemitones);

// Repeat ramp ramping volume
voice->params.addMod(PARAM_VOL, repeatRampGain);
```

Zero branching on source type in any of these. The **translation to source-specific API** lives
only in each voice's `render()` — one place, once.

### IAudioVoice — Simplified Interface

With ParamBus handling all continuous parameters, the interface only needs event methods
and render:

```cpp
class IAudioVoice {
public:
    ParamBus params;

    // Events — still explicit because they are not continuous values
    virtual void noteOn(int midiNote, float vel) = 0;
    virtual void noteOff() = 0;
    virtual void hardStop() = 0;
    virtual void retrigger(int startPoint) = 0;  // Repeat effect

    // Render — reads params internally, returns peak level
    virtual float render(float* buf, int frames) = 0;

    virtual ~IAudioVoice() = default;
};
```

Compare to the previous plan's 8+ typed setters (`setVolume`, `setPan`, `setNote`,
`setPitchMod`, `setStartPoint`, …). ParamBus replaces all of them with a single uniform
write path.

### SamplerVoice — Existing Logic, Now Reads from ParamBus

`SamplerVoice` already does all of this internally (the `Voice` struct has all these fields).
Phase 2a is just plumbing: replace direct field reads with `params.get(PARAM_*)` calls.
No behavior change. The existing signal chain (downsample → crush → interpolate → drive →
filter → volume → pan) stays exactly the same.

### SoundfontVoice — ParamBus → TSF API Translation

```cpp
class SoundfontVoice : public IAudioVoice {
    tsf* handle;     // one clone per active SF instrument (see Phase 2b)
    int  preset;

public:
    void noteOn(int midiNote, float vel) override {
        tsf_note_on(handle, preset, midiNote, vel);
    }
    void noteOff() override {
        tsf_note_off(handle, preset, lastMidiNote);
    }
    void hardStop() override {
        tsf_note_off(handle, preset, lastMidiNote);  // TSF has no hard-stop; note-off is close enough
    }
    void retrigger(int /*startPoint*/) override {
        tsf_note_off(handle, preset, lastMidiNote);
        tsf_note_on(handle, preset, lastMidiNote, lastVel);
    }

    float render(float* buf, int frames) override {
        // Translate ParamBus to TSF API — one place, no branching elsewhere
        int midiNote = baseMidiNote + (int)roundf(params.get(PARAM_PITCH));
        if (midiNote != lastAppliedNote) {
            tsf_note_off(handle, preset, lastAppliedNote);
            tsf_note_on(handle, preset, midiNote, lastVel);
            lastAppliedNote = midiNote;
        }
        tsf_channel_set_volume(handle, 0, clamp01(params.get(PARAM_VOL)));
        tsf_channel_set_pan(handle, 0, (clampf(params.get(PARAM_PAN), -1, 1) + 1.0f) * 0.5f);

        tsf_render_float(handle, buf, frames, 0);

        // Measure peak from rendered buffer
        float peak = 0;
        for (int i = 0; i < frames * 2; i++) peak = fmaxf(peak, fabsf(buf[i]));
        return peak;
    }
};
```

Effects like arpeggio, vibrato, volume ramp, table — all handled by `params.addMod()` calls
upstream. `SoundfontVoice::render()` just reads the accumulated result and calls TSF. No
effect-specific code inside `SoundfontVoice`.

### Core Abstractions Overview

```
IAudioVoice (ParamBus params + event methods + render)
├── SamplerVoice    — existing Voice logic, reads from params
├── SoundfontVoice  — tsf* clone per instrument, translates params → TSF API
└── SynthVoice      — future (wavetable, FM, …)
```

### Event Pipeline (same for all source types)

```
ScheduledNote (unified struct)
    ↓
NoteQueue (existing, unchanged)
    ↓
processAudioBlock per-frame loop
    ↓
voice->params.resetMods()           ← clear accumulator each block
    ↓
VoiceManager.trigger(note)
    ├── SamplerVoice  → assign slot, set params.base values
    └── SoundfontVoice → tsf_note_on on isolated clone, set params.base
    ↓
per-frame modulation tick (source-agnostic — just writes to params)
    ├── Table tick       → params.addMod(PARAM_VOL, col)
    │                    → params.addMod(PARAM_PITCH, transpose)
    ├── ADSR/LFO         → params.addMod(dest, envValue * depth)
    └── Effect columns:
        ├── Vxx          → params.addMod(PARAM_VOL, v)
        ├── Axx          → params.addMod(PARAM_PITCH, arpSemitones)
        ├── PSL/PBN/PVB  → params.addMod(PARAM_PITCH, pitchMod)
        ├── Oxx          → params.addMod(PARAM_START_POINT, offset)  [no-op in SF]
        ├── K00          → voice->hardStop()                         [event]
        └── Rxx          → voice->retrigger(startPoint)              [event]
    ↓
peak = voice->render(trackBuf[track], frames)   ← reads params, returns peak
    ↓
trackPeak[track] = max(trackPeak[track], peak)  ← per-track, always accurate
    ↓
mix trackBuf[track] → output (apply trackVolumes[track] × masterVolume)
```

---

## Key Design Decisions

### 1. ParamBus separates "what to do" from "how to do it"

Modulation sources (EffectProcessor, tables, ADSR, LFO) express intent by writing to the bus.
Voice types translate intent to their own API in `render()`. Adding a new modulation source
or effect only requires writing to the bus — no changes to any voice type. Adding a new voice
type only requires implementing `render()` — no changes to any effect code.

### 2. Events vs continuous values

ParamBus handles continuously-valued parameters. Discrete events (`noteOn`, `noteOff`,
`hardStop`, `retrigger`) remain explicit methods on `IAudioVoice` because they are not
additive — they represent state transitions, not magnitude adjustments.

### 3. One TSF instance per active SF instrument (Phase 2b)

**Current**: one TSF per SF2 file, renders all channels together.
**Problem**: can't get per-track audio, can't apply per-track effects.
**New**: load SF2 file data once to a memory buffer, create one `tsf*` clone per active
instrument slot (max 8 loaded, LRU eviction — same strategy as current `sfSlotMap`).

```cpp
// On instrument load — JNI/loading thread, never audio thread:
sfFileBuffers[path] = readFileToVector(path);          // load file data once
sfClones[instrId]   = tsf_load_memory(
    sfFileBuffers[path].data(), sfFileBuffers[path].size()
);
tsf_set_output(sfClones[instrId], TSF_STEREO_INTERLEAVED, 44100, 0);

// On audio thread — no allocation, no file I/O:
tsf_note_on(sfClones[instrId], preset, midiNote, vel);
tsf_render_float(sfClones[instrId], trackBuf, frames, 0);
```

Memory per clone: ~1× SF2 file size (TSF stores sample data internally). With 8 slots
and typical SF2 files at 1–10 MB, total SF memory is 8–80 MB — acceptable on Miyoo Flip.

### 4. Per-track render buffers enable stem export

Once each voice renders to its own `trackBuf[track]` (instead of the shared SF mix),
per-track stem export is trivial:

```cpp
for (int t = 0; t < NUM_TRACKS; t++) {
    float peak = voices[t]->render(trackBuf[t], frames);
    trackPeaks[t] = fmaxf(trackPeaks[t], peak);

    // Stem export: write trackBuf[t] to WAV for track t
    if (renderingStems) stemWriters[t].write(trackBuf[t], frames);

    // Master mix
    for (int i = 0; i < frames * 2; i++)
        output[i] += trackBuf[t][i] * trackVolumes[t] * masterVolume;
}
```

No architectural changes needed after Phase 2b — stems fall out automatically.

### 5. Track volume applied at mix time (not per-note)

`trackVolumes[t]` applied once during the master mix loop, not baked into notes.
Mixer fader changes take effect immediately on any playing voice without per-voice bookkeeping.

---

## Migration Plan

### Phase 1 — Interface extraction ✅ Done

1. Define `IAudioVoice` C++ abstract class
2. Wrap existing `Voice` struct as `SamplerVoice : IAudioVoice`
3. All existing code still works, just through the interface

### Phase 2a — ParamBus (additive, low risk)

1. Add `ParamBus` struct to `native-audio.cpp` (or a new `ParamBus.h`)
2. Add `ParamBus params` field to `IAudioVoice`
3. At the start of each `processAudioBlock`, call `voice->params.resetMods()` per active voice
4. Replace `EffectProcessor` direct field writes with `params.addMod()` calls
5. Replace table-tick and envelope field writes with `params.addMod()` calls
6. `SamplerVoice::render()` reads `params.get(*)` instead of direct Voice fields
7. Verify: sampler audio is bit-for-bit identical before and after (offline render comparison)

**No behavior change. SoundfontVoice not yet involved.**

### Phase 2b — Per-instrument TSF clones (medium risk, test on Miyoo Flip)

1. In `InstrumentController.loadSoundfont()`:
   - Read SF2 file to `sfFileBuffers[path]` (keep in memory)
   - Create `sfClones[instrId] = tsf_load_memory(...)` on the loading thread
   - Replace current `sfSlotMap` with `sfClones` keyed by instrument ID
2. Implement `SoundfontVoice : IAudioVoice` wrapping one `tsf*` clone
3. Route SF notes through `VoiceManager` alongside sampler voices
4. Remove the separate SF mixing block from `processAudioBlock`; SF now renders into `trackBuf[track]`
5. Allocate per-track float buffers (`trackBuf[0..7]`) in `processAudioBlock`

**Result:** Per-track SF metering and mixer volume work without approximation.
Test thoroughly on Miyoo Flip before merging.

### Phase 3 — Effect unification (falls mostly out of Phase 2a)

After Phase 2a, all effect code writes to `params`. The only remaining work:
1. Implement arpeggio in `SoundfontVoice::render()` via MIDI note swap (note-off + note-on on new pitch)
2. Implement `retrigger()` in `SoundfontVoice`
3. `setStartPoint()` / `PARAM_START_POINT` → no-op in `SoundfontVoice` (already handled by addMod writing to bus; SF just ignores it in render)
4. Table tick and ADSR/LFO already source-agnostic after Phase 2a — no changes needed

**Result:** All top-5 effects and modulation work on SF instruments.

### Phase 4 — Kotlin layer cleanup

1. Remove `InstrumentType.SOUNDFONT` branches from `PlaybackController.scheduleStepWithEffects`
2. `scheduleStepWithEffects` becomes fully source-agnostic
3. Remove SF-specific JNI methods from `IAudioBackend` / `OboeAudioBackend` that are no longer needed

### Phase 5 — Per-track stem export

1. Add `stemWriters: Array<WavWriter?>` to the render path
2. When stem export is active, write each `trackBuf[t]` to a per-track WAV file
3. UI: "STEM MIX" button in Project screen alongside existing "WAV MIX"
4. Output to `Documents/PocketTracker/Renders/ProjectName_stem_T1.wav`, etc.

No new audio architecture needed — this is pure I/O layered on top of Phase 2b's per-track buffers.

### Phase 6 — Synth foundation (future)

1. Implement `SynthVoice : IAudioVoice` with oscillator + envelope
2. `SynthVoice::render()` reads `params.get(PARAM_PITCH)` etc. — ParamBus works identically
3. No changes needed to EffectProcessor, tables, ADSR/LFO, mixer, or peak meters
4. Add `InstrumentType.SYNTH` + UI parameters

---

## Files Affected

| File | Change |
|------|--------|
| `native-audio.cpp` | Add `ParamBus`, `IAudioVoice`, `SamplerVoice`, `SoundfontVoice`; per-track buffers; unify `processAudioBlock` |
| `ParamBus.h` (new, optional) | `ParamBus` struct + `ParamId` enum — can stay in `native-audio.cpp` if preferred |
| `InstrumentController.kt` | `loadSoundfont` reads file to buffer + creates clone on loading thread |
| `PlaybackController.kt` | Remove `SOUNDFONT`/`SAMPLER` branch in `scheduleStepWithEffects` |
| `IAudioBackend.kt` | Remove SF-specific JNI methods no longer needed |
| `OboeAudioBackend.kt` | Remove corresponding `native_*` declarations |
| `RenderController.kt` | Add stem export mode (Phase 5) |

---

## Estimated Effort

| Phase | Effort | Risk |
|-------|--------|------|
| 2a — ParamBus | 2–3 days | Low (additive, verifiable via offline render comparison) |
| 2b — TSF clones + per-track buffers | 3–4 days | Medium (must test on Miyoo Flip) |
| 3 — Effect unification | 1–2 days | Low (mostly falls out of 2a) |
| 4 — Kotlin cleanup | 1 day | Low |
| 5 — Stem export | 1–2 days | Low (I/O only, no DSP changes) |
| 6 — Synth foundation | Post-MVP | — |

Total for Phase 2a–5: ~1.5–2 weeks.

---

## Current Workarounds (until Phase 2a–4 land)

- **Per-track metering**: all tracks sharing one SF2 slot show the same combined peak (`tsf_render_float` mixes all channels); true per-track isolation requires Phase 2b
- **Track volume on SF**: `noteVolume × trackVolume` baked into TSF channel volume at note-on; `setTrackVolume()` updates the channel live
- **Effects on SF**: Kill (note-off) and Volume work; Arpeggio, Repeat, Table FX not applied
- **Memory**: one `tsf*` per SF2 file (~1× file size), not per instrument
