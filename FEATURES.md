# PocketTracker — Feature Overview

What you can do, what's coming before release, and what's planned for later.

---

## In v1.0

### Making Music

- Write melodies and rhythms in a 16-step phrase editor, chain phrases into longer patterns, arrange everything in an 8-track song
- LGPT-style controls: directional buttons + modifier combos, fast editing with A+direction for value changes, key repeat for scrolling through values quickly
- Transpose phrases per chain slot — sequence the same phrase in different keys
- Select cells, rows, or entire screens and copy, cut, paste, or delete them (M8-style selection)
- Set swing and shuffle per track or globally with groove patterns
- 256 phrases, 256 chains, 8 tracks, 256 grooves
- Use HOP to jump between phrases mid-sequence — create odd time signatures and generative loops

### Sequence Effects

Write these into any phrase step to shape how a note plays:

- **Arpeggio** — cycle through note intervals automatically (up, down, ping-pong, random)
- **Volume** — ramp volume within a step
- **Kill** — stop the note immediately
- **Retrigger** — stutter the sample with optional volume ramp
- **Pitch slide** — glide to the next note (portamento)
- **Pitch bend** — continuous pitch movement up or down
- **Vibrato** — wobble pitch at standard or extreme depth
- **Delay** — push a note forward by N ticks
- **Chance** — probability gate: set odds the note actually plays
- **Randomize** — randomize any other FX value on the fly
- **Table override** — switch which table an instrument follows
- **Groove assign** — set groove pattern per track from a phrase step
- **Tick rate** — control how fast the instrument table advances

### Instrument Tables

Each instrument has its own 16-row mini-sequencer. It loops continuously while the note plays and lets you automate volume, pitch, and effects row by row — great for programmed arpeggios, tremolo, and rhythmic gating without using up phrase FX slots.

### Instruments

- **Sampler**: load any WAV file (mono or stereo); set root note, detune, volume, pan
- **SoundFont**: load SF2 files; edit envelope (attack, decay, sustain, release), filter cutoff and resonance
- Loop modes: no loop, forward loop, ping-pong loop
- Reverse playback
- Non-destructive start and end point trimming
- Per-instrument real-time effects: low/high/band-pass filter, 3-band EQ, overdrive, bitcrusher, sample rate reduction (Lo-Fi)
- 4 modulation slots per instrument: envelope shapes (AHD, ADSR) and LFO targeting volume, pitch, filter, pan, and more
- Save and load instruments as preset files (.pti) — bundles all parameters, table, and modulation settings together

### Sample Editor

- Waveform display with zoom and selection
- Stereo source mode: use the left channel, right channel, both stereo, or averaged mono — without modifying the file
- Non-destructive editing: crop, copy, cut, paste within the waveform
- Reverse, normalize, fade in, fade out, silence a selection
- Undo last destructive operation
- Apply effects permanently: overdrive, bitcrusher, 3-band EQ, OTT compressor, dust (vinyl noise/wear)
- Pitch-shift to match a BPM target without changing length (repitch)
- Time-stretch to match a BPM target without changing pitch — Akai SOLA algorithm (same "jungle chop" character as the S950/S1000)
- Auto-detect transients to place slice markers, or divide manually
- Slice markers embedded in the WAV cue chunk — compatible with M8, Blackbox, Reaper, Logic
- Export all slices as separate WAV files (CHOP)
- Assign slice playback mode on the instrument: trigger individual slices by note, play from slice to end, or standard pitch mode

### Resampling

Record what's currently playing in the sequencer into a new sample — capture a phrase or a whole section and resample it into an instrument slot.

### Mixing & Effects

- Mixer screen: volume for each of the 8 tracks and the master, plus send amounts to reverb and delay
- True dBFS peak meters per track
- Two send effects: reverb (Schroeder-Moorer algorithm) and ping-pong stereo delay
- Route delay output into the reverb — wet delay signal feeds the reverb input with no extra latency
- Reverb and delay each have their own 3-band EQ
- Master bus: OTT 3-band compressor or DUST (lofi/vinyl texture) — switchable, with wet/dry depth control — followed by a soft peak limiter
- Per-instrument EQ also accessible from the instrument screen

### Export

- Export the full song mix as a stereo WAV file
- Offline render: same DSP chain as real-time playback, sample-accurate

### File Management

- Built-in file browser: navigate folders, sort files, preview samples before loading
- Extract audio from video files directly in the browser
- Rename files (SELECT+A to enter rename with on-screen QWERTY)
- Delete files and folders (SELECT+B with confirmation)
- On-screen QWERTY keyboard for naming projects and files
- Projects saved as .ptp files in /Documents/PocketTracker/Projects/
- Instruments saved as .pti files in /Documents/PocketTracker/Instruments/
- Samples stored wherever you put them; paths stored in the project

### Look & Feel

- Theme editor: full RGB palette for every UI color; save and load themes as .ptt files
- Several built-in themes to start from
- 8 visualizer modes for the top bar: oscilloscope, octascope (one scope per active track), bars, peaks with hold, mirror, flat, frequency spectrum, spectrum with peak hold

### Controls

- Full physical button support (tested on Miyoo Flip and Ayaneo Pocket Air Mini)
- Touch layout: virtual buttons in portrait orientation
- Virtual button clicks: choose a sound and volume
- Haptic feedback on button press (toggle on/off)
- Auto-detects physical vs touchscreen device on launch

---

## Planned Before Release

These are not yet in the app but are targeted for v1.0:

- **Per-track WAV export** — export each track as its own separate stereo WAV file instead of only a full mix
- **File move** — move files and folders to a different location from within the browser
- **Limiter settings on the mixer screen** — expose threshold, ceiling, or similar limiter parameters where it's easy to reach during a session
- **Default project template** — load the app with a usable starting state (some instruments, a tempo) instead of a blank project

---

## Planned After Release

Everything here is intentionally out of scope for v1.0.

### Performance & Live Use
- Live mode — trigger patterns and chains in real time during a performance
- Live effects screen — tweak send levels, filters, and effects hands-on while the song plays

### Synthesis
- Built-in synth voices (Braids/Mutable Instruments style synthesis, without needing a sample or SF2)

### Sequencing
- Scales screen — lock notes to a musical scale, see note names in context
- More sequence effects — additional FX commands beyond the current set
- Groove presets — built-in swing/shuffle patterns to choose from, plus save and load custom grooves as preset files

### Editing
- Undo/redo across the whole session
- Copy/paste individual instrument settings between instruments
- File copy in the browser

### Audio
- Mic input for recording samples directly into the app
- Audio effects presets — save and recall your favourite reverb, delay, or EQ settings

### MIDI
- MIDI input — play notes and trigger the sequencer from an external keyboard or controller
- MIDI output — drive external hardware from PocketTracker

### Appearance
- Font choices — pick from more than one bitmap font
- Landscape touch layout — a proper horizontal button arrangement for widescreen devices
- Random name generator for projects and instruments

### Platform
- Linux port

