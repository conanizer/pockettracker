# PocketTracker — Technical Architecture

How PocketTracker is built. This document describes the system as it currently works.

**Audience:** developers and contributors.

---

## Table of Contents

1. [Overview](#overview)
2. [Source Layout](#source-layout)
3. [The Seams](#the-seams)
4. [Audio Engine](#audio-engine)
5. [Songcore — the platform-free runtime](#songcore--the-platform-free-runtime)
6. [The Event Bus](#the-event-bus)
7. [Data Model and File Formats](#data-model-and-file-formats)
8. [UI Layer](#ui-layer)
9. [Input Layer](#input-layer)
10. [Rendering and Export](#rendering-and-export)
11. [Platform Shells](#platform-shells)
12. [The Conformance Tools](#the-conformance-tools)
13. [Build and Test](#build-and-test)
14. [Coding Conventions](#coding-conventions)

---

## Overview

PocketTracker is **one C++17 program with four thin platform shells**. The audio engine, the
sequencer, the document model, the file formats and the entire user interface are portable C++ with
no platform dependency at all. A shell provides a window, an audio device, a clock, buttons and a
filesystem root — and nothing else.

```
                    ┌─────────────────────────────────────────────┐
                    │  native/ui   (pt-ui)                        │  the whole UI: screens,
                    │  no SDL, no POSIX, no window, no engine*    │  cursor, input dispatch
                    ├─────────────────────────────────────────────┤
                    │  native/songcore                            │  document, sequencer,
                    │  header-only, platform-free                 │  event bus, render, I/O
                    ├─────────────────────────────────────────────┤
                    │  native/  (the engine)                      │  voices, modulation, DSP
                    └─────────────────────────────────────────────┘
                       ▲            ▲             ▲            ▲
            ┌──────────┴──┐  ┌──────┴──────┐  ┌───┴──────┐  ┌──┴──────────┐
            │ shell/      │  │ shell/      │  │ shell/   │  │ shell/      │
            │ Android SDL │  │ PortMaster  │  │ Linux    │  │ Windows     │
            └─────────────┘  └─────────────┘  └──────────┘  └─────────────┘
```

\* one file in `pt-ui` includes the engine: `ui/engine_feed.h`, the 60 Hz read-back of scope samples
and meters. Every screen module takes plain pointers, and a null pointer draws silence — which is
what lets a headless tool render any screen with no engine in the process.

**Android has no Kotlin UI.** `app/src/main/java` is a six-file shim: `MainActivity` (an
`SDLActivity` subclass that owns the splash, permissions and settings import), the virtual button
skin's sound and haptic managers, and the two MIDI device managers. Everything a user sees is drawn
by `pt-ui` into a software framebuffer.

---

## Source Layout

```
native/                            The portable program
├── audio-engine.cpp / .h          The engine: processAudioBlock, voices, modulation, DSP
├── audio-decoders.cpp / .h        WAV, MP3, FLAC, OGG, Opus, M4A decoding
├── sampler-voice.h                Per-voice state for sample playback
├── soundfont-voice.cpp / .h       Per-voice state for SF2 (TinySoundFont)
├── note-queue.h                   Sample-accurate note + parameter scheduling queues
├── sample-editor.cpp              Destructive waveform operations
├── transient-detector.cpp         Slice-point detection
├── oboe-audio-engine.cpp / .h     Android audio backend (the only Oboe-coupled TU)
├── mods/                          Modulation: routing, runner, AHD/ADSR/LFO/vibrato/tracking
├── effects/                       DSP, three layers:
│   ├── instrument-chain.h           per voice: Crush → Drive → Filter
│   ├── send-chain.h                 send buses: reverb (DaisySP ReverbSc) + stereo delay
│   ├── master-chain.h               output bus: masterEq → OTT | DUST → Limiter
│   ├── primitives/                  biquad, filter, vendored DaisySP
│   └── modules/                     FilterModule, DriveModule, BitcrushModule, DustChain
│
├── songcore/                      Header-only, platform-free. No SDL, no JNI, no POSIX.
│   ├── model.h                      Project, Chain, Phrase, Table, Groove, Instrument, Note
│   ├── project_io.h                 .ptp / .pti parse + emit (byte-exact)
│   ├── project_ops.h                Compact, transitive table walks, slot surgery
│   ├── timing.h                     frames_per_step / _tic, groove timing, transpose
│   ├── effects.h                    Effect codes, names, EFFECT_TYPES, resolve_step_params
│   ├── automation.h                 AUS/AUF: the automatable registry, the curve, the pairing
│   ├── traversal.h                  Song walks, collect_used_instruments
│   ├── rng.h                        PCG32, seeded and bounded like kotlin.random
│   ├── event.h                      The event schema (versioned, frozen)
│   ├── router.h                     MidiRouter, IMidiConsumer, TrackInstruments
│   ├── scheduler.h                  The sequencer: phrases, chains, song, transport
│   ├── engine_consumer.h            Events → engine calls
│   ├── voice_derive.h               Note → voice parameters
│   ├── engine_setup.h               Project → engine push (instruments, buses, EQ bank)
│   ├── host.h                       SongcoreHost — the one runtime object a shell constructs
│   ├── render.h / wav_writer.h      Offline render, WAV output
│   ├── midi_out.h / midi_in.h       The MIDI seams, serializer, parser, router
│   ├── midi_clock.h                 24 PPQN clock, transport, song position
│   ├── trace_writer.h               Conformance trace serializer
│   └── note_tables.h                Baked pitch tables (bit-exact across toolchains)
│
└── ui/                            pt-ui — the UI, portable C++ with no platform anything
    ├── canvas.h / .cpp              FOUR primitives: fill rect, stroke rect, text, clip
    ├── app_state.h                  The one mutable UI state object
    ├── platform_caps.h              Which features this build has (a VALUE, not an #ifdef)
    ├── screen.h / navigation.h      Screens and the R+DPAD navigation grid
    ├── cursor.h / cursor_move.h     CursorContext: what is under the cursor, and how it steps
    ├── input_dispatcher.cpp / .h    Every button and combo, one place
    ├── selection.h / clipboard.*    Multi-cell selection and copy/paste
    ├── layout.cpp / .h              Screen composition, top strip, right bar
    ├── theme.h / theme_io.h         Palettes and the theme file
    ├── settings_store.*             settings.json
    ├── font5x5.h                    The pixel font
    ├── filesystem.h                 The file I/O interface a shell implements
    └── modules/                     One module per screen (song, chain, phrase, table, groove,
                                     instrument, instrument pool, modulation, mixer, effects, eq,
                                     sample editor, settings, project, midi, file browser,
                                     qwerty keyboard, oscilloscope, nav map, fx helper, confirm)

shell/                             The only SDL in the tree
├── main.cpp                       Desktop / handheld entry point
├── android-main.cpp               SDL_main for the Android build
├── app.cpp / .h                   The shared frame loop, signal handling, config
├── sdl-video, sdl-audio-engine, sdl-input, sdl-touch    The device
├── image, font_raster, assets, skin, overlay, portrait2 Presentation
├── midi-{out,in}-{base,winmm,alsa,android}.*            Two backend families, one #ifdef each
├── alsa-rawmidi.*                 Runtime libasound loader (dlopen; no link-time dependency)
├── midi-sender.*                  The MIDI output thread
├── portmaster/                    Launch script and PortMaster metadata
└── build-linux.sh, build-windows.ps1, build-portmaster.sh, Dockerfile.portmaster

app/                               Android: manifest, resources, and a six-file Kotlin shim
tools/                             Conformance tools — see below
testdata/                          Golden projects, traces, unit corpora, synthesized media
```

---

## The Seams

Four interfaces are all a platform has to implement. Everything else is shared code.

| Seam | Header | What a shell provides |
|---|---|---|
| **Canvas** | `ui/canvas.h` | Nothing — the canvas is a software framebuffer the UI owns. The shell scales the finished 640×480 frame onto a texture. |
| **Filesystem** | `ui/filesystem.h` | Directory listing, read, write, and the app root. `ui/std_filesystem.*` is the `std::filesystem` implementation the desktop and handheld shells use. |
| **Audio** | `audio-backend.h` | A device that calls `processAudioBlock`. Oboe on Android, SDL audio elsewhere. |
| **MIDI** | `songcore/midi_out.h`, `midi_in.h` | A port that moves bytes. winmm, ALSA and Android backends exist. |

Two design rules make this hold:

**The canvas has exactly four primitives, permanently.** Fill rect, stroke rect, bitmap text, clip.
That is all the UI has ever used, and keeping it at four is what makes a new platform a rendering
back end rather than a port. Skinning, scaling and post-processing happen shell-side, on the
finished frame.

**Platform differences are a VALUE, not an `#ifdef`** (`ui/platform_caps.h`). Which SETTINGS rows
exist, whether PROJECT has an EXIT row, whether the MIDI surfaces can be authored — all fields on a
`PlatformCaps` struct. The same compiled code answers every platform's question, which is what lets
a headless tool drive any platform's UI and compare the results.

---

## Audio Engine

A sample-accurate queue system in C++.

- **44.1 kHz stereo.** Android uses Oboe (OpenSL ES Exclusive → Shared → None/Shared → AAudio
  Exclusive); every other platform uses SDL audio.
- **The audio device is opened exactly once**, at startup, and never reopened.
- `AudioEngine` **must be heap-allocated** — its DSP scratch buffers, spectrum rings and 256-slot
  table pool blow a 1 MB stack instantly.

### The one processing rule

**All DSP happens in `processAudioBlock`.** `onAudioReady` (Oboe), the SDL callback and
`renderOffline` are thin wrappers around it, and none of them contains signal processing. It is also
the **only** place the note and parameter queues drain. A second drain point is a second timeline.

### Signal path

```
  voice ─► instrument chain (Crush → Drive → Filter) ─► instrument EQ ─┬─► track fader ─┐
                                                                       ├─► reverb bus ─┤
                                                                       └─► delay bus ──┤
                                                                                       ▼
                                    out ◄─ Limiter ◄─ OTT | DUST ◄─ master EQ ◄─ MASTER FADER
```

Reverb and delay are **send buses**, not inserts: a per-note or per-instrument send amount feeds
them, and the delay can additionally feed the reverb. Both have their own input EQ. `-1` is the
documented bypass value for every EQ slot.

Note where the two faders sit relative to that tap. A send is **pre-fader with respect to the track
fader** and post-everything on the instrument, so pulling a track down leaves its tails alone. The
**master fader is downstream of the returns** — it multiplies the summed bus, dry and wet together,
which is what makes it a master rather than a dry-mix gain. ⚠️ It is therefore one multiply over the
whole block and not a per-voice gain: `VMV` and a `VTR`/`VMV` automation ramp both move it through the
parameter queue, which drains once per block, so a per-voice application would buy no extra
resolution and would silently exclude the returns.

### Voices

Two voice types — `SamplerVoice` and `SoundfontVoice` — both driven by the same note path. A
dedicated **preview lane** (voice 9) carries auditions, so hearing a note you are dialling in never
steals a voice from the song.

Note-offs are **KIL-only**, plus a live MIDI key release. There is no step-end note-off: an ADSR or
TRIG voice rings until something explicitly stops it.

---

## Songcore — the platform-free runtime

`native/songcore/` is header-only and has no platform dependency whatsoever. It is the whole program
minus the pixels and the device.

`SongcoreHost` (`host.h`) is the object a shell constructs. It owns the one `Project` in the process,
the sequencer, the engine consumer, the external MIDI consumer and the transport. There is exactly
**one mutable copy of the document** — `AppState` holds a pointer, never a copy, and the UI edits the
host's project in place. Two mutable copies of a document is a desync waiting to happen.

### The sequencer

`scheduler.h` walks the song. It schedules ahead by a lookahead window, tracks per-track state
(current chain, phrase, step, table row, groove position), applies effects, resolves chance and
randomization, and emits **events** rather than engine calls.

Timing is frame-based: `frames_per_step` and `frames_per_tic` derive from the tempo, and grooves
change a step's length per position. Everything downstream is stamped in frames.

### Parameter automation

`AUS` opens a ramp on the automatable effect in the slot to its **left**, taking that cell's value as
the start and its own as the curve; `AUF`, on a later step, carries the destination. The value is
interpolated in the authored 0–255 byte domain by a polynomial — `+ − ×` only, for the determinism
reason above — and emitted **once per tic** as the parameter's own `EV_CC`.

That last point is what keeps automation cheap. A ramp is not a new kind of event: it is the CC the
per-step effect already emits, emitted more often. So a parameter becomes automatable by paying the
price of live control at all — a CC id, an arm in `EngineConsumer::consume`, a queued apply on the
audio thread — plus a row in `automation.h`'s registry, and no `SCHEMA_VERSION` bump. Today's rows are
`VOL`, `PAN`, `REV`, `DEL`, `VTR` and `VMV`; the set is that table, not a property of the feature.

Pairing lives in `automation.h` and is **pure**: it answers "which spans does this walk declare", in
step indices, and never touches frames, tics, grooves or the transport. The emitter already holds each
step's real duration as it walks, so a groove-warped span costs it nothing.

A span may cross phrases, and **the chain is the boundary** — a chain is the unit a track repeats and
re-enters, so pairing that ran past it would have to survive a chain played from two song rows at once
and CHAIN mode's wrap. ⚠️ The open ramp is **re-derived from `(chain, chainRow)` on every call**, never
carried in `TrackState`: a live edit rolls the lookahead back without rewinding track state, a chain
re-entered from a later song row would inherit the previous one's open ramp, and a phrase scheduled
twice would advance it twice. Deriving makes all three unaskable.

The PHRASE editor draws an `AUS`/`AUF` cell that no ramp uses **dimmed**, and it asks the same pairing
code the emitter does, so the grid can neither claim a fade that will not play nor deny one that will.
A phrase has no single playing context — it may sit at several chain rows, in several chains, in none —
so the editor unions the answer over every chain row that plays it (`find_ramp_cells`) and falls back
to pairing within the phrase only for a phrase no chain references.

### Determinism

The floats have to be bit-identical across compilers and architectures, because the conformance
tools compare raw binary32 bits:

- `note_tables.h` bakes all 132 note frequencies and 256 detune values as constants rather than
  computing `pow()` at runtime.
- Songcore's translation unit is compiled with `-ffp-contract=off` and without `-ffast-math`. GCC
  defaults to contracting `a + b*c` into an FMA, which would silently change the last bits.

Random effects (CHA, RND, RNL, random-mode ARP), `oscShape >= 8` RND/DRNK LFOs, and DUST on the
master bus are clock-seeded and therefore **not** byte-reproducible. `tools/ptnondet` answers
"can this project be byte-compared at all?" and should be run before any A/B.

---

## The Event Bus

The sequencer does not call the engine. It publishes **events** on a bus, and consumers turn them
into whatever they are for.

```
   scheduler ──► MidiRouter ──┬──► EngineConsumer    → voices (the internal sound)
                              ├──► ExternalConsumer  → MIDI bytes (the cable)
                              └──► TraceConsumer     → a conformance trace
```

The event vocabulary is MIDI's: note on, note off, control change, program change, pitch bend. That
is the payoff of the seam — one command vocabulary reaches an internal sampler voice and an external
synthesizer without either knowing about the other.

`event.h` holds a **versioned, frozen schema**. Adding a tag, a field, or changing the order requires
a `SCHEMA_VERSION` bump, regenerated goldens and a doc update, together.

`TrackInstruments` (`router.h`) resolves which instrument a track-scoped event belongs to: the last
note-on on a track names it. Both consumers use the same resolver, so they cannot disagree.

### MIDI

All three shipping platforms have MIDI in and out (winmm on Windows, ALSA on Linux, the Android MIDI
API on Android), with a 24 PPQN clock, transport messages and song-position pointer.

**The MIDI authoring surfaces are hidden in release builds** (`PlatformCaps::midi`). A release build
cannot create MIDI data — no MIDI screen, no EXTERNAL instrument type, no MIDI effect commands — but
it displays existing data faithfully, because all three are persisted in a `.ptp` and a build that
drew them as something else would misrepresent the file on disk.

---

## Data Model and File Formats

```
Project
├── name, tempo, transpose
├── song[256][8]          chain references per row per track   (-1 = empty)
├── chains[128]           16 phrase slots + a transpose each
├── phrases[128]          16 steps: note, volume, instrument, 3 × (FX type, FX value)
├── tables[128]           16 rows: transpose, volume, 3 × (FX type, FX value)
├── grooves[32]           16 step lengths in tics
├── instruments[128]      sampler | SoundFont | external
├── eqPresets[128]        the shared EQ slot bank
├── mixer, master bus, reverb, delay
└── midi settings         sync out, program change, per-track input channels
```

**`-1` is the empty value everywhere** — chain references, table volume, every EQ slot. Not `0xFF`.

**An instrument is empty iff `sampleFilePath` is null.** Not `sampleId < 0`, not an empty name. A
note-on on an empty instrument is still a valid *event*; the consumer is what drops it.

### Files

| Extension | Contents |
|---|---|
| `.ptp` | A project. JSON, pretty-printed, defaults omitted. |
| `.pti` | A single instrument preset — any type, including external. |
| `settings.json` | Device and app settings. Shared shape across platforms. |
| `config.json` | Optional folder overrides for the file browser. |
| `.ptt` | A theme. |

`project_io.h` emits **byte-exact** JSON: the field order, the pretty-printing and the
default-omission rules are all pinned, and eight golden projects round-trip byte-for-byte in CI. Any
new serialized field must be default-guarded or those goldens break.

---

## UI Layer

640×480, drawn into a software framebuffer with four primitives. The shell scales the finished frame.

**Sixteen screens**, reached by R+DPAD over a twelve-cell navigation grid plus sub-screens:

| | |
|---|---|
| Grid editors | SONG, CHAIN, PHRASE, TABLE, GROOVE |
| Instruments | INSTRUMENT, INST.POOL, MODS |
| Mix | MIXER, EFFECTS |
| Sample | SAMPLE EDITOR |
| Sub-screens | PROJECT → SETTINGS, PROJECT → MIDI, THEME editor, EQ editor |
| Overlays | file browser, qwerty keyboard, FX helper, confirm dialog |

Screen geometry lives in **one table per screen** that the cursor walks and the module draws. Where
rows are conditional (SETTINGS, PROJECT), a row's **number is its identity** — hidden rows are
skipped, never renumbered, so a stored cursor and a recorded test case keep meaning the same thing.

Two modules are stateful, both because their output is a function of the *previous* frame: the
oscilloscope (peak-hold dots, falling bars) and the mixer (falling peak markers).

**Modals own the buttons while they are up**, and every new modal has to be added to the predicate
that says so.

---

## Input Layer

`ui/input_dispatcher.cpp` is every button and combo in one place.

**Modifiers are snapshotted at EVENT time, not at poll time.** SDL delivers a whole frame's events at
once, so asking "is A held?" while processing a B press describes the *end* of the frame. Roll B and
A down inside one 16 ms frame and a poll-time reader fires A+B on what was a plain B. Each queued
event carries its own modifier snapshot.

**The mapper's check order is the specification**, not style. L+B+A is tested before L+A or a clone
pastes; A+B before a plain A or a delete inserts; R+A is consumed so holding R to change screens
cannot also fire an edit underneath.

Cursor behaviour is a `CursorContext`: what the cell holds, what it can do (increment, decrement,
delete, insert), its range and its step sizes. The five generic handlers (`on_a`, `on_b`,
`on_a_left`, `on_a_right`, `on_a_b`) turn a button into an `InputAction` without ever asking which
screen is up.

---

## Rendering and Export

Offline render (`songcore/render.h`) is **prepare → render → finish**, with scheduling deliberately
outside those verbs. It pushes the project into the engine, walks the requested song rows, and writes
a WAV.

- **A render is deterministic**: the same project rendered twice produces a byte-identical file, with
  a different project rendered in between. Engine state surviving from one render to the next was a
  real bug; back-to-back renders would be the weaker test.
- **The tail is appended**: a render whose last note is still ringing continues until the audio
  decays to zero, rather than stopping dead at full amplitude. A runaway cap bounds it.
- **Live and render must be identical.** `push_live_params` and `prepare_render` push the same
  parameters, and a standing test asserts a live-configured engine and a render-configured engine
  produce byte-identical audio. They did not always: the shell once played a project on the engine's
  factory defaults while rendering it correctly.

Export modes: full mix, per-track stems, and resampling a song selection into a new sample.

---

## Platform Shells

| | Android | PortMaster | Linux desktop | Windows |
|---|---|---|---|---|
| Entry | `android-main.cpp` | `main.cpp` | `main.cpp` | `main.cpp` |
| Audio | Oboe | SDL2 | SDL2 | SDL2 |
| SDL2 | vendored, built in | **the device's own** | system | vendored, static |
| MIDI | Android MIDI (JNI) | ALSA (dlopen) | ALSA (dlopen) | winmm |
| App root | `Documents/PocketTracker` | the port's `data/` | `$XDG_DATA_HOME/PocketTracker` | `Documents\PocketTracker` |
| Exit | — (the launcher owns it) | PROJECT → EXIT | PROJECT → EXIT | PROJECT → EXIT |

**PortMaster links the device's libSDL2, deliberately.** The CFW patched that copy for the hardware's
display (KMSDRM) and audio (ALSA); shipping our own would shadow the one that actually knows the
screen. The build cross-compiles against a *pinned old* SDL2 as a link-time SDK — a compatibility
floor, so reaching for a newer SDL API fails loudly on a dev box instead of as `undefined symbol` on
a stranger's handheld.

**The PortMaster launcher must never run gptokeyb.** The app reads the gamepad itself through SDL's
GameController API *and* reads the keyboard (the same source builds the desktop binary), so gptokeyb
delivers every press twice by two paths with conflicting meanings.

**Renderer creation falls back.** `SDL_RENDERER_ACCELERATED` means *require*, not *prefer*, so
`SDL_CreateRenderer` fails outright on hardware with no accelerated driver. The shell tries
accelerated+vsync → accelerated → anything, and paces the frame itself when there is no vsync (a spun
core is a battery bug on a handheld).

**Signal handling: a handler may only set a flag.** "SIGTERM → autosave" read literally is a heap-lock
deadlock — hundreds of KB of JSON, none of it async-signal-safe — which hangs in exactly the case it
exists for. The handler writes a `volatile sig_atomic_t`; the frame loop does the work. The handler
must be installed *before* `SDL_Init`.

---

## The Conformance Tools

`tools/` holds hand-written tools that compare the program against recorded goldens and stated
invariants. They are the reason this codebase can be refactored: every layer has something pointed
at it, and each tool sees something the others structurally cannot.

| Tool | What it measures |
|---|---|
| `ptroundtrip` | `.ptp` / `.pti` serialization, byte-for-byte |
| `ptresolve` | Timing, effect resolution, song traversal — unit by unit |
| `ptplay` | The **event stream**, against 36 golden traces |
| `ptvoice` | The **engine calls** a note produces — which no trace can see |
| `ptrender` | Audio: determinism, health, a tolerance fingerprint, and `live == render` |
| `ptrandom` | Random effects: draw counts and support sets exactly, distributions statistically |
| `ptnondet` | Whether a project is byte-comparable at all |
| `ptinput` | What a **button press does**: the cursor context, the action, **and the resulting cell** |
| `ptdispatch` | The dispatcher wiring — screen changes, modals, files, lifecycle |
| `ptshot` | **Pixels**: any screen of any project to a PNG, with no window and no engine |
| `ptmidi`, `ptmidiin` | The MIDI serializer, parser and router |
| `ptalsa`, `ptalsain` | The ALSA backends, driven through a fake `libasound` (Linux only) |
| `ptmapper`, `pttouch`, `ptfont`, `ptdecode`, `ptaac` | Input mapping, touch layout, font raster, decoders |

Two things about them are load-bearing:

**Audio cannot be byte-compared across toolchains.** Measured, not argued: gcc/x86-64 differs from
MSVC/x86-64 in 9.7% of samples by at most 5 LSB of 32767; clang/arm64 with fast-math differs in 17.0%
by at most 16 LSB. Both are inaudible. So audio goldens are a tolerance fingerprint, and the
byte-exact claims are made about *events* and *engine calls* instead.

**`ptshot` is the standing proof that `pt-ui` is platform-free.** The day the UI reaches for SDL,
ptshot stops linking.

---

## Build and Test

### Android

```
./gradlew :app:assembleDebug        # the APK
./gradlew :app:buildCMakeDebug      # the native side — assembleDebug does NOT imply this
./gradlew :app:assembleRelease      # the only build that runs R8
```

Two native libraries ship: **`libpockettracker-sdl.so` is the app**; `libpockettracker.so` is the
engine alone and is legitimately older.

⚠️ **Every C++→Kotlin JNI callback resolved by name needs an explicit `-keep` in
`app/proguard-rules.pro`, and this only bites in release** (debug never runs R8). AGP keeps a native
method's *class* but not its *members*. Verify in the DEX, not in `mapping.txt`.

### Desktop / handheld

```
cmake -S shell -B shell/build -DCMAKE_BUILD_TYPE=Release
cmake --build shell/build
```

`CMAKE_BUILD_TYPE` explicitly: a Debug engine may not keep up with a real-time audio callback.

### Tests

```
cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
ctest --test-dir tools/build --output-on-failure
```

Twenty tests on Linux, eighteen on Windows — `ptalsa` and `ptalsain` are Linux-only. CI runs them on
gcc/x86-64, MSVC/x86-64 and clang/arm64, which **is** the test rather than redundancy: the
floating-point guarantees the whole ladder rests on are only real if the compilers agree.

### Packaging

| Platform | Script | Output |
|---|---|---|
| Windows | `shell/build-windows.ps1` | `build/windows/PocketTracker-<v>-windows-x64.zip` |
| Linux | `shell/build-linux.sh` | `build/linux/PocketTracker-<v>-linux-x64.tar.gz` |
| PortMaster | `shell/build-portmaster.sh` (in `Dockerfile.portmaster`) | `build/portmaster/pockettracker.zip` |

Each one verifies the *artifact* — that it is newer than every source file, links what it claims to,
carries the right version — and then reads the finished archive back out. `.github/workflows/release.yml`
runs all three on a `v*` tag and opens a draft release. The signed APK is built locally, because CI
has no access to the signing key and an APK signed with a different key cannot install as an update.

---

## Conventions

`snake_case` for functions and variables in songcore and pt-ui; `camelCase` for model fields, which
mirror the JSON they serialize to.

Screen geometry lives in one table per screen that the cursor walks and the module draws, rather than
in a chain of branches — see [UI Layer](#ui-layer).

**A row's or an enum member's number is its identity: append, never insert.** An FX cell stores an
*index* into `EFFECT_TYPES` while the project file stores the effect *code*, so inserting an entry
renumbers every cell a user has already typed. The same applies to `SettingsRow`, `ProjectRow` and
`InstrumentType`, all of which are persisted or recorded by number. Where a build hides a member, it
hides one from the **end** of the list and the walkers skip it — they never close the gap.
