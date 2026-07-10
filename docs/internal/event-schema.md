# Event Schema — one vocabulary for the bus, the conformance trace, and the JNI seam

**Status:** RATIFIED 2026-07-10 (drafted same day; one amendment — the phrase V column is
termed **velocity** in all docs, see the §3.1 terminology note). Songcore Phase 1 is open.
This is the Step-2 one-pager from `order-of-work.md`. It serves three masters at once:
`midi-implementation-plan.md` §3 (the bus events), `linux-port-plan.md` §4.3 (the
`(frame, track, event, params)` conformance trace), and the songcore↔UI JNI vocabulary
(Phase 1). Both plans said "the trace format and the MIDI bus are the same artifact" —
this document *is* that artifact. Where either plan disagrees with this doc on a detail,
this doc wins and the plan gets a backlink edit in the same commit.

---

## 1. One record, three views

One C++ POD record (`songcore/event.h`), three encodings:

1. **Bus dispatch** — the in-memory struct `MidiRouter` hands to `IMidiConsumer`s.
2. **Conformance trace** — a text serialization of the same records (§6), compared
   byte-for-byte between the Kotlin sequencer (tap) and C++ songcore.
3. **JNI crossing** — the same record packed as a flat integer array (live-input
   injection down; debug trace out).

Everything in the record is an integer. Analog-valued fields ride as **raw IEEE-754
binary32 bits** (rendered `0xXXXXXXXX` in the trace), so "streams must be exactly equal —
no float fuzz" holds mechanically. This refines MIDI plan §3/§6's "internal 0–255 wide
values": internal CC values are the *derived engine floats* (today's seam values, full
resolution); the authored-byte→float mapping lives in the emitter (FX resolution), and
0–127 port scaling lives only in the EXTERNAL serializer / MIDI-in parser.

## 2. Envelope (every event)

| Field | Type | Domain |
|---|---|---|
| `frame` | i64 | absolute frames at session sample rate (goldens: 44100) |
| `track` | u8 | 0–7 sequencer · 8 live/preview (`PREVIEW_TRACK_ID`) · 0xFF global (ExtMasterEq) |
| `instrument` | i16 | 0–255 routing key · −1 none (track-scoped events: NoteOff, CC, EXT) |
| `type` | u8 | tag per §3 |

## 3. Event catalog

Tags: events with a MIDI form use their MIDI status high byte (0x80–0xE0); tracker-only
events use 0x01–0x7F (never valid MIDI status bytes). Every event either defines its MIDI
byte mapping or is EXT — the EXTERNAL consumer is a pure serializer (MIDI plan §3 rule).

| Tag | Event | Payload | Today's seam call (provenance) | MIDI out |
|---|---|---|---|---|
| 0x90 | NoteOn | §3.1 block | `scheduleNote` / `scheduleNoteWithTable` / `scheduleSoundfontNote` | `0x9n note vel` (vel −1→derive, clamp ≥1) |
| 0x80 | NoteOff | `mode`: 0 RELEASE · 1 CUT | RELEASE = `scheduleNoteOff` (KIL soft / ADSR release); CUT = `scheduleKill`/`killTrack` (declick fade) | `0x8n note 0` (note via active-note tracker) |
| 0xB0 | ControlChange | `param` u8 (CC id per MIDI plan §6) · `value` f32ᵇ | CC 7 = `scheduleTrackPhraseVol` (Vxx mid-note — pushes instrVol × Vxx, not velocity) · CC 10 = `scheduleVoicePan` (PAN) · CC 91 = `scheduleVoiceReverbSend` (REV) · CC 93 = `scheduleVoiceDelaySend` (DEL); more ids flow in MIDI phase D (CCA–CCD, cutoff 74, res 71, …) | `0xBn cc scale7(value)` |
| 0xC0 | ProgramChange | `program` u8 | none yet — MIDI phase D (`MPG`) | `0xCn prog` |
| 0xE0 | PitchBend | `value14` u16 (center 0x2000) | none yet — MIDI phase D (`MPB`, absolute) | `0xEn lsb msb` |
| 0x01 | ExtPitchRate | `rate` f32ᵇ st/tick · `tempo` u16 | `schedulePitchBend` (PBN on empty step — a *rate*, not an absolute bend) | EXT (phase-D option: serializer integrates rate → 0xEn stream at tick cadence) |
| 0x02 | ExtVibrato | `speed` f32ᵇ Hz · `depth` f32ᵇ st | `scheduleVibrato` (PVB/PVX on empty step; atomic pair) | EXT (phase-D option: depth → CC 1) |
| 0x03 | ExtTableRow | `row` u8 0–15 | `scheduleVoiceTableRow` (THO on empty step) | EXT |
| 0x04 | ExtReverse | `reverse` u8 · `restart` u8 | `scheduleVoiceReverse` (BCK) | EXT |
| 0x05 | ExtEqSlot | `slot` i16 −1 bypass · 0–127 | `scheduleVoiceEqSlot` (EQN) | EXT |
| 0x06 | ExtMasterEq | `slot` i16 −1 bypass · 0–127 | `scheduleMasterEqSlotAt` (EQM; track = 0xFF) | EXT |

0x07–0x7F reserved for future EXT events. This is total coverage of today's scheduling
seam: all 10 `ParamUpdateAction`s (`note-queue.h:196`), both kill flavors, all three note
paths. Anything the sequencer schedules that is not one of these events is a schema bug.

### 3.1 NoteOn payload

Sampler vs SoundFont is **routing** (by `instrument` → module type), not two event types.
Fields marked EXT are dropped by the EXTERNAL serializer; ᵇ = binary32 raw bits.

| Field | Form | Source | MIDI out |
|---|---|---|---|
| `note` | u8 0–127 | octave×12+pitch | data1 (after transpose/pit fold, clamped) |
| `velocity` | i8 −1 · 0–127 | the phrase **V column** (`PhraseStep.volume`, 00–7F); −1 = retrig legacy "derive from gain" (IB-19) | data2, clamp ≥1 |
| `velGain` | f32ᵇ | velocity curve (squared) — seam arg `volume` | EXT |
| `volGain` | f32ᵇ | instrument vol × Vxx — seam arg `phraseVol` | EXT |
| `pan` | f32ᵇ 0=L ½=C 1=R | PAN-with-note is baked here, not a CC (IB-12) | EXT v1 |
| `startOffset` / `endOffset` | i32, −1 default | OFF / CUT-slice window | EXT |
| `sliceIndex` | i32, −1 | SLI | EXT |
| `transpose` / `pit` | i32 semitones | chain transpose + ARP offset / PIT | folded into data1 |
| `tableId` · `tableTicRate` · `tableStartRow` | i32 · i32 · i32 −1 | TBL / TIC / THO-with-note | EXT |
| `pslOffset` · `pslDuration` | f32ᵇ st · f32ᵇ ticks | PSL | EXT (phase-D option: CC 5/65) |
| `pbnRate` | f32ᵇ st/tick | PBN-with-note | EXT |
| `vibSpeed` · `vibDepth` | f32ᵇ Hz · f32ᵇ st | PVB/PVX-with-note | EXT |

- **Cross-wiring ledger note:** at today's seam, `velGain` rides the arg *named* `volume`
  (→ `MOD_SRC_INSTR_VOL`) and `volGain` rides `phraseVol` (→ `MOD_SRC_PHRASE_VOL`) — the
  names are historically crossed (`PlaybackController.kt:1146`, `note-queue.h:46`). The
  port copies the **wiring**, not the names. Do not "fix" this during migration.
- **Terminology (ratification amendment 2026-07-10):** docs say **velocity** for the phrase
  V column everywhere; "phrase volume" — the column's historical name from before it became
  MIDI velocity 00–7F (`pre-mvp-improvements-plan.md` §2) — is retired. The UI keeps its
  `V` label. The `phraseVol`/`MOD_SRC_PHRASE_VOL` identifiers stay as historical code names
  and don't even denote the column today (they carry instrVol × Vxx, see above); songcore
  gets clean names: this schema's `velocity`/`velGain`/`volGain`.
- **Instrument-static rule:** anything derivable from `instrument` + project data (SF
  slot/bank/preset, detune, mod slots, TIC-mode octave/pitch) does **not** ride events —
  it is consumer-side lookup, covered by the schema round-trip goldens, not the trace.

## 4. Ordering & timing contract

- **Clock:** `framesPerStep = floor(60000 / tempo / 4 × sampleRate / 1000)`,
  `TICS_PER_STEP = 12` (SC-5). Grooved steps last `floor(framesPerStep/12) × tics`
  (IB-2). FX tick params are 12ths of the *current step's* duration; GRV values are
  global tics (IB-1). The tap freezes the exact arithmetic expression shapes (integer /
  binary64) verbatim in both implementations.
- **Same-frame drain order** (was "engine drain order decides" in IB-12 — now pinned,
  from `audio-engine.cpp:884/978`): at equal `frame`, the engine applies
  **param-class → NoteOff → NoteOn**. Rank: NoteOn = 2, NoteOff = 1, everything else = 0
  (CC/EXT/Program/PitchBend all ride the param queue). So KIL 00 + note on one step =
  NoteOff then NoteOn, guaranteed.
- **Canonical trace order:** records sort **stably** by `(frame, track, rank)` before
  comparison. Relative order of same-key records is *semantic* and must match: it is FX
  slot resolution order 1→3, and last-wins depends on it (IB-21).
- **IB-12 restated as schema law:** note-step voice FX (REV/DEL/BCK/EQN) are emitted at
  `frame+1` so they hit the new voice; PAN-with-note is baked into NoteOn; param-class
  events at frame N apply before that frame's notes.

## 5. Determinism rules

- The event stream is a **pure function of (project, transport command, mode,
  sampleRate)** — FIX-1 restored this; the schema keeps it. Scheduler polling cadence,
  lookahead depth, and checkpoint rollbacks must not change the stream (SC-3).
- **Goldens contain no CHA/RND/RNL/ARP-RANDOM** (SC-1); random FX get separate
  statistical tests. Track 8 (live/preview) is bus-legal but excluded from goldens —
  its frames come from wall-clock `getCurrentFrame()`.
- **Float policy:** f32 fields compare as raw bits. They derive from authored integers
  via pinned binary32 expressions; songcore scheduling TUs compile with **no
  `-ffast-math` and `-ffp-contract=off`** (aarch64 compilers contract `a*b+c` into fma
  *by default*, which changes rounding). Transcendental derivations (note→Hz) use one
  vendored implementation in songcore on every platform — device libm (bionic) and glibc
  differ, and cross-platform golden reuse in CI needs bit-stable results. Escape hatch:
  if a field still won't reproduce, redefine it to its authored-integer form in a schema
  bump.
- Kotlin-vs-C++ Phase 1 comparison runs on the same device (same libm) — the vendored-
  transcendental rule is what keeps the *C++-vs-C++* traces stable across x86 CI / ARM.

## 6. The trace

- **Tap points:** Kotlin = one wrapper around each `AudioEngine.schedule*` call (the one
  allowed zone-C change); C++ = the router itself (bus records serialize directly).
- **Text form**, one record per line, fields in schema order, floats as `0x…`:

  ```
  # schema=1 sr=44100 tempo=120 mode=render project=<sha1> date=2026-07-10
  T PLAY SONG row=00
  1058400 2 0A 90 note=48 vel=100 velGain=0x3F4C4A3D volGain=0x3F800000 pan=0x3F000000 …
  1058401 2 -1 B0 param=91 value=0x3F19999A
  1063691 2 -1 80 mode=0
  T STOP
  ```

- **Meta records** (`#` header, `T` transport) carry session context; they are compared
  too (same project + transport = same header).
- **Comparison:** byte equality after the §4 canonical sort. Goldens live in
  `/testdata`, one trace per (project, mode): render mode skips muted tracks, live mode
  schedules them (IB-10) — both modes are goldened per project.
- Instrument/mod data pushes (`setInstrumentModulation`, sample loads) are not trace
  records — they're covered by the §7 data-push surface + serialization round-trip CI.

## 7. JNI vocabulary (the songcore Phase 1 surface)

Three verb classes down, two up — nothing musical crosses the boundary in any other shape:

| Direction | Verb | Notes |
|---|---|---|
| ↓ data | `pushProject(blob)` · `pushPhrase/Chain/Instrument/Groove/Table/Song(id, blob)` | per-pool-item blobs, the `loadTable` ByteArray precedent; edits push diffs |
| ↓ transport | `play(mode, position)` · `stop()` | modes SONG/CHAIN/PHRASE; tempo edits re-push project (IB-8) |
| ↓ events | `injectEvent(packedRecord)` | live input: preview today (track 8), MIDI-in phase E (tracks 0–7). Same §2/§3 record, packed as a flat int array |
| ↑ feedback | `getPlayheads()` | per-track (chainRow, phraseRow, frame) — replaces `getPlaybackPosition`; never goldened (SC-4) |
| ↑ debug | `setTrace(enabled, path)` | streams §6 text; debug builds only |

Meters/scope keep their existing engine-side path (zone A, unchanged).

## 8. Non-goals

Mods and tables stay engine-internal C++ (modulators, not the note stream — MIDI plan
§5); MIDI clock/transport bytes are generated at the port from the frame clock (phase C),
not bus events; MIDI 1.0 byte serialization details beyond the mapping columns here are
phase B (the columns *are* normative); UI cursor position (SC-4) and meters are not
events; SC-2's checkpoint state smear is invisible to goldens and stays a Phase 1
implementation decision.

## 9. Freeze & change discipline

`SCHEMA_VERSION = 1` lives in `songcore/event.h` and in every trace header. Any change to
tags, payload fields, ordering, or float derivations = version bump + regenerated goldens
+ this doc updated, all in one PR (the port plan §6 CI rule). Open items that Phase 1's
tap-writing freezes into event.h + a doc update, without redesign: exact per-field units
for `startOffset`/`endOffset` (today's seam values verbatim), the packed JNI field order,
and the final trace rendering of each field.
