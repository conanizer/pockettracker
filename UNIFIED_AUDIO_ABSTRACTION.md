# Unified Audio Abstraction Plan

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

---

## Goal

A single audio event pipeline where the sound **source** is a plugin and everything else
(effects, volume, metering, scheduling) is source-agnostic.

---

## Architecture

### Core Abstractions (C++)

```
IAudioSource
├── SamplerSource      — current WAV voice pool
├── SoundfontSource    — wraps one TSF instance, renders one channel per track
└── SynthSource        — future (wavetable, FM, …)

AudioVoice
├── sourceType: SAMPLER | SOUNDFONT | SYNTH
├── trackId, volume, pan
├── tableId, effects state
└── render(float* buf, int frames) → float peak
```

### Event Pipeline (same for all source types)

```
ScheduledNote (unified struct)
    ↓
NoteQueue (existing, unchanged)
    ↓
processAudioBlock per-frame loop
    ↓
VoiceManager.trigger(note)
    ├── SamplerSource  → assign voice slot, load params
    ├── SoundfontSource → tsf_channel_set_bank_preset + note_on on isolated instance
    └── SynthSource    → set oscillator params
    ↓
per-frame effect tick (EffectProcessor — same code for all)
    ├── Vxx → voice.setVolume(v)
    ├── K00 → voice.noteOff() or voice.hardStop()
    ├── Rxx → voice.retrigger()
    ├── Axx → voice.setNote(arpeggioNote)
    ├── PSL/PBN/PVB → voice.setPitchMod(params)
    └── Oxx → voice.setStartPoint(offset)   [no-op for SF/Synth]
    ↓
voice.render(trackBuf, frames) → peak
    ↓
trackPeak[trackId] = max(trackPeak[trackId], peak)  ← per-track, always accurate
    ↓
mix trackBuf → output (apply trackVolumes[trackId] × masterVolume)
```

---

## Key Design Decisions

### 1. One TSF instance per active SF track (not per file)

**Current**: one TSF per SF2 file, renders all channels together.
**Problem**: can't get per-track audio, can't apply per-track effects.
**New**: load the SF2 file data once, then create one `tsf*` clone per active instrument slot.

TSF supports loading from memory (`tsf_load_memory`), so:
```
sfFileData = load_file_to_memory(path)          // load once
sfHandle[instrumentId] = tsf_load_memory(data)  // one clone per instrument
```

Each instrument gets its own `tsf*` that renders independently.
Memory cost: TSF is ~100–500KB per instance (presets + voice state). With 256 slots but
only a handful active at once, this is manageable. Consider a max of 8–16 loaded instances,
evicting LRU when exceeded (same strategy as current sfSlotMap).

### 2. SoundfontSource.render() uses tsf_render_float on one instance

Since each active instrument has its own `tsf*`:
- `render(buf, frames)` calls `tsf_render_float` on that instrument's `tsf*`
- Output is mono/stereo for just that instrument
- Peak is measured directly from the output
- Track volume and effects applied uniformly by VoiceManager

### 3. Effects via unified VoiceInterface

```cpp
class AudioVoice {
    virtual void noteOn(int midiNote, float vel) = 0;
    virtual void noteOff() = 0;
    virtual void hardStop() = 0;
    virtual void setVolume(float v) = 0;
    virtual void setPan(float p) = 0;
    virtual void setNote(int midiNote) = 0;     // for arpeggio
    virtual void retrigger(int startPoint) = 0; // for Repeat
    virtual float render(float* buf, int frames) = 0; // returns peak
};
```

`EffectProcessor` calls `voice->setVolume()`, `voice->setNote()`, etc. — same code path for
Sampler, SF, and future synths. No branching on instrument type.

### 4. Track volume applied at mix time (not per-note)

Remove `trackVolumes` from per-note scheduling. Apply it once at mix time:
```cpp
for each track t:
    float peak = voices[t]->render(trackBuf, frames);
    trackPeaks[t] = max(trackPeaks[t], peak);
    for i in 0..frames*2:
        output[i] += trackBuf[i] * trackVolumes[t] * masterVolume;
```

This means changing the mixer volume takes effect immediately on any playing voice
without any per-voice bookkeeping.

---

## Migration Plan

### Phase 1 — Interface extraction (no behavior change)

1. Define `IAudioVoice` C++ abstract class
2. Wrap existing `Voice` struct as `SamplerVoice : IAudioVoice`
3. All existing code still works, just through the interface

### Phase 2 — SoundfontSource refactor

1. Change SF loading: one `tsf*` per instrument slot (max 8 loaded), not per file
2. Implement `SoundfontVoice : IAudioVoice` wrapping a `tsf*` instance
3. Route SF notes through VoiceManager alongside sampler voices
4. Remove the separate SF mixing block; SF now goes through the same per-track mix loop

**Result**: per-track SF metering and volume work without any approximation heuristics.

### Phase 3 — Effect processor unification

1. Move `EffectProcessor` calls to operate on `IAudioVoice*` instead of `Voice&`
2. Implement the effect methods in `SoundfontVoice`:
   - `setVolume()` → `tsf_channel_set_volume`
   - `setNote()` → `tsf_channel_note_off` + `tsf_channel_note_on` on new MIDI note (arpeggio)
   - `retrigger()` → `tsf_channel_note_off` + `tsf_channel_note_on` (Repeat)
   - `hardStop()` → `tsf_channel_note_off`
   - `setStartPoint()` → no-op (not applicable to SF)

**Result**: all top-5 effects work on SF instruments.

### Phase 4 — Kotlin layer cleanup

1. Remove SF-specific branches from `PlaybackController` (`InstrumentType.SOUNDFONT` when block)
2. `scheduleStepWithEffects` becomes source-agnostic
3. Effect processor Kotlin side already source-agnostic (it just calls `scheduleNote`)

### Phase 5 — Synth foundation (future)

1. Implement `SynthVoice : IAudioVoice` with oscillator + envelope
2. No changes needed to effect processor, mixer, or peak meters
3. Add `InstrumentType.SYNTH` + UI parameters

---

## Files Affected

| File | Change |
|------|--------|
| `native-audio.cpp` | Add IAudioVoice, SamplerVoice, SoundfontVoice; unify processAudioBlock |
| `TrackerData.kt` | Add SF clone slot field to Instrument (optional) |
| `InstrumentController.kt` | Update loadSoundfont to create per-instrument tsf clone |
| `PlaybackController.kt` | Remove SOUNDFONT/SAMPLER branch in scheduleStepWithEffects |
| `IAudioBackend.kt` | Remove SF-specific JNI methods that are no longer needed |
| `OboeAudioBackend.kt` | Remove corresponding native_ declarations |

---

## Estimated Effort

| Phase | Effort | Risk |
|-------|--------|------|
| 1 — Interface extraction | 1–2 days | Low |
| 2 — SoundfontSource refactor | 3–4 days | Medium (TSF cloning needs testing) |
| 3 — Effect unification | 2–3 days | Medium |
| 4 — Kotlin cleanup | 1 day | Low |
| 5 — Synth foundation | Post-MVP | — |

Total for Phase 1–4: ~1.5–2 weeks.

---

## Current Workarounds (until refactor)

These are approximate fixes that will be replaced by the proper abstraction:

- **Per-track metering**: proportional by note volume (not actual per-voice render)
- **Track volume on SF**: stored `sfChannelNoteVolume[8]` applied at change time
- **Effects on SF**: Kill and Volume work; Arpeggio/Repeat are pass-through (may work partially)
- **Table FX on SF**: not applied
