# MIDI-Out Readiness Audit

Drafted 2026-06-20. Pre-MVP item #6 (analysis only — no MIDI code ships pre-MVP). The MIDI-out
**feature** is slated for early post-MVP; this audit maps what already aligns with MIDI and what's
missing, so the feature is a contained follow-on and so the pre-MVP data-model choices (#1 128
instruments, #2 0–127 velocity) don't have to be revisited later.

**Scope:** emitting MIDI to external gear (hardware/soft synths) that mirrors what the tracker plays.
MIDI-**in** (recording/controllers) is explicitly out of scope here.

---

## 1. What already maps cleanly

| MIDI concept | Tracker source | Notes |
|---|---|---|
| **Channel** | 8 tracks → MIDI channels 0–7 | `PlaybackController.trackStates = Array(8)`. The C++ SF engine already treats each track as a MIDI channel, so the mental model is in place. Direct 1:1. |
| **Note number** | `Note.toMidi()` = `(octave+1)*12 + pitch` | `TrackerData.kt`. Standard MIDI, C-4 = 60. The phrase NOTE column is capped to 0–127 (`CursorContextFactory.note` max 127 = G-9), and every transposed/PIT/ARP note is `coerceIn(0,127)` before it reaches a voice (`PlaybackController`, `AudioEngine`). So note numbers handed to the engine are **already valid MIDI**. |
| **Velocity** | `PhraseStep.volume` (0–127 after item #2) | The VOL column is now a 0–127 MIDI velocity. Internally it becomes a squared gain (sampler) / TSF velocity (SF2), but the **raw `step.volume` byte is a ready-made note-on velocity** — emit that, not the gain. |
| **Program** | instrument index (0–127 after item #1) | 128 instruments = MIDI program range. Maps to a Program Change per channel. **Caveat:** our instruments are samples/SoundFonts, not General-MIDI programs, so the *number* maps but the *sound* lives on the external device — see §3. |
| **Timing** | frame-scheduled priority queue | `ScheduledNote` carries a target frame; `PlaybackController` already computes exact frame times for every event (incl. groove, DEL, retrig). MIDI timestamps derive from the same scheduler. |

**Bottom line:** channels, note numbers, velocity, and program are essentially free. The work is in
note lifecycle, timing transport, and effect translation.

---

## 2. Gaps (work the MIDI-out feature must do)

### 2.1 Note-off / duration model — the biggest gap
The sampler fires **one-shots**: a note plays until the sample ends, the next note on the track, or a
KILL. There is **no general note-off**. The only note-offs today are:
- `scheduleNoteOff(frame, trackId)` for **KILL (K00)** and **ADSR/TRIG** instruments
  (`PlaybackController.kt:1185`) — soft-kills / triggers the release.

MIDI requires an explicit **note-off for every note-on**. Phrase steps carry **no duration**, so the
feature must synthesize one. Options (pick per-track or global):
- **Gate to next event:** note-off when the next note (or KILL) lands on that track. Simplest; matches
  the sampler's "new note cuts the old" behaviour.
- **Gate to step/row length:** note-off at the end of the step (or N steps). More MIDI-conventional,
  needs a length model.
- Reuse the existing `scheduleNoteOff` plumbing for the cases that already have it (KILL/ADSR).

This implies a small **active-note tracker** (one held note per channel) so we always know what to
note-off.

### 2.2 Note range
Notes are already `coerceIn(0,127)`, so nothing *crashes*. But clamping means a note that musically
overshoots 127 is emitted as 127 rather than skipped. For MIDI-out, decide: **clamp** (current) vs
**skip out-of-range**. Low stakes; clamp is probably fine.

### 2.3 Transport / clock
For a synth that just plays notes, timestamped note-on/off is enough. For **sync** (arps, LFOs, delay
on the external gear, DAW transport) we'd also emit:
- **MIDI clock** (0xF8) at 24 PPQN, derived from tempo.
- **Start/Stop/Continue** (0xFA/0xFC/0xFB) tied to the tracker's play/stop.
- Optionally **Song Position Pointer** for starting mid-song (we already support start-from-row).

### 2.4 Effect translation
Most effects are sampler/sequencer-internal and have no MIDI primitive — they must be either applied
internally before emit, expanded to discrete notes, or dropped. See the table in §4.

### 2.5 Program-change semantics
Instrument index → Program Change is mechanically trivial, but our instruments aren't GM programs.
Decide the policy: (a) send Program Change = instrument index and let the user map sounds on the
synth; (b) add an optional per-instrument "MIDI program" field; (c) don't send Program Change at all
and treat channels as fixed timbres. Recommend (a) for v1, (b) later.

---

## 3. Proposed architecture

Keep `core/` Android-free (CLAUDE.md portability rule):

```
core/audio/IMidiOut.kt          # interface: noteOn/noteOff/programChange/controlChange/
                                #   pitchBend/clock/start/stop, all with a frame/time stamp
platform/android/AndroidMidiOut # android.media.midi (MidiManager / MidiDevice / MidiInputPort)
```

- **Driven from `PlaybackController`**, alongside the existing audio scheduling. Every place that
  calls `audioEngine.scheduleNote(...)` also emits a MIDI note-on (+ schedules the matching note-off
  per §2.1). A per-channel active-note tracker lives here.
- **MIDI is a parallel sink, not a replacement** — the internal engine keeps rendering audio; MIDI-out
  is an optional fan-out (toggle in Settings, choose target device).
- **Timestamps:** the Android MIDI API takes nanosecond timestamps; convert from the scheduler's
  frame times (`frame / sampleRate`). The 2-phrase lookahead means we can emit slightly ahead with
  accurate timestamps.

---

## 4. Effect → MIDI mapping

(Effect codes from `EffectProcessor.kt`.)

| FX | Meaning | MIDI translation |
|---|---|---|
| **PIT** | pitch offset, integer semitones | Transpose the note number (clean). |
| **PBN** | pitch bend | MIDI pitch-bend (14-bit). Our range can exceed the synth's default ±2 st → send RPN 0 (pitch-bend range) or clamp. |
| **PSL** | pitch slide / portamento | CC5 (portamento time) + CC65 (portamento on), synth-dependent; or emulate with pitch-bend ramps. |
| **PVB / PVX** | vibrato (normal / extreme) | Mod wheel (CC1, synth-dependent) **or** generate our own pitch-bend LFO. |
| **ARP** | arpeggio | **No MIDI primitive** — expand to discrete note-on/off at tick resolution. |
| **VOL (Vxx)** | volume automation mid-note | CC7 or CC11 (velocity can't change after note-on). |
| **KILL (K00)** | stop note | Note-off (already has a frame via `scheduleNoteOff`). |
| **REPEAT (Rxy)** | retrigger | Repeated note-on/off (we already re-schedule these internally — emit each). |
| **OFFSET / SLI** | sample start / slice | Sampler-internal — **no MIDI form** (drop). |
| **TBL / THO / HOP / TIC** | table / sequencer flow | Internal — expand their *effect* (notes/pitch/vol) at emit time; the directives themselves don't map. |
| **GRV** | groove (timing) | Affects *when* events fire — already baked into the scheduler's frame times, so it's reflected in MIDI timing for free. |
| **DEL** | delay row by ticks | Baked into the event frame → reflected in MIDI timing. |
| **ARC / CHA / RND / RNL** | arp config / chance / randomize | Meta — resolved internally before scheduling (`resolveStepParams`), so MIDI just sees the resulting notes. |

**Pattern:** anything that's already resolved into concrete notes/pitch/volume/timing by
`EffectProcessor.resolveStepParams` + `PlaybackController` is emitted for free; the rest (ARP, the
pitch family) needs explicit expansion or CC mapping; sampler-only effects are dropped.

---

## 5. Recommended phasing (for the post-MVP feature)

1. **Core note events** — `IMidiOut`, `AndroidMidiOut`, device pick + enable toggle; note-on with
   velocity, note-off via gate-to-next-event (§2.1), program change = instrument index. Covers the
   majority of musical content.
2. **Transport** — MIDI clock + start/stop/continue for sync.
3. **Pitch family** — PIT (transpose, trivial) → PBN (pitch-bend) → PSL/PVB/PVX.
4. **ARP expansion + CC** — discrete-note arps, Vxx→CC11.
5. **Polish** — optional per-instrument MIDI program field, per-track channel remap, out-of-range
   note policy.

---

## 6. Retrospective: how this audit shaped pre-MVP #1/#2

- **#1 (128 instruments/tables/grooves):** 128 instruments lines up exactly with the MIDI program
  range (0–127). No rework needed for program change.
- **#2 (phrase VOL → 0–127 velocity):** the VOL column is now a literal MIDI velocity byte, so
  note-on velocity is a direct copy — the single most important alignment for MIDI-out.

Both decisions are MIDI-compatible as shipped; the feature can build on them without migration.
