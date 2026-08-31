# PocketTracker — Feature Overview

Everything you can do with PocketTracker.

---

## Making Music

- Write melodies and rhythms in a 16-step phrase editor, chain phrases into longer patterns, arrange everything in an 8-track song
- LGPT-style controls: directional buttons + modifier combos, fast editing with A+direction for value changes, key repeat for scrolling through values quickly
- Transpose phrases per chain slot — sequence the same phrase in different keys
- Select cells, rows, or entire screens and copy, cut, paste, or delete them (M8-style selection)
- Set swing and shuffle per track or globally with groove patterns
- Build up to 16 scales per project by switching each of the twelve notes on or off, and set the song's key. A track using a scale plays only the notes in it: anything out of the scale is moved to the nearest note that is in, both as you type and as the song plays, so a phrase written before you chose the scale falls into it. The scale screen marks the note being heard; sliced instruments are left alone, and any instrument can be taken out of transposition entirely with its TSP switch
- 256 phrases, 256 chains, 8 tracks, 128 grooves
- Use HOP to jump between phrases mid-sequence — create odd time signatures and generative loops
- Every track runs its own column of the song at its own pace: all 8 start together, then each moves on as soon as its own chain ends, so a 2-row chain beside a 16-row one loops eight times instead of waiting. A `>` marker on the song, chain and phrase screens shows where each track has got to
- LIVE mode (B+LEFT/RIGHT on the song screen): the song grid becomes a scene launcher. START queues the chain under the cursor to start when that channel's current chain ends, pressed again to start at the next bar instead; a launched chain repeats until you queue something else. L+START launches the whole row as one scene, R+START silences a single channel, and a blinking marker shows what is waiting

## Sequence Effects

Write these into any phrase step to shape how a note plays:

- **Arpeggio** — cycle through note intervals automatically (up, down, ping-pong, random)
- **Volume** — ramp volume within a step
- **Pan** — set a note's stereo position; the next note reverts to the instrument pan
- **Kill** — stop the note, immediately or after a set number of ticks
- **Retrigger** — stutter the sample with optional volume ramp
- **Playback direction** — play a sampler note backward or forward; flip it live to "scratch"
- **Pitch slide** — glide to the next note (portamento)
- **Pitch bend** — continuous pitch movement up or down
- **Vibrato** — wobble pitch at standard or extreme depth
- **Pitch offset** — transpose a note by a fixed number of semitones (never affects slice index)
- **Slice index** — jump straight to a specific slice (works even when slice mode is off)
- **Latency** — push a note's trigger forward by N ticks
- **Reverb / delay send** — send a single note to the reverb or delay bus, independent of the instrument
- **Filter cutoff / resonance** — move the instrument's own filter from a phrase step, on that note only;
  the same two cells work in a table, so a filter envelope can be written once per instrument
- **EQ (per note / mixer)** — apply an EQ preset to one note, or automate the master EQ across the song
- **Chance** — probability gate: set odds the note actually plays
- **Randomize** — randomize any other FX value on the fly
- **Table override** — switch which table an instrument follows
- **Groove assign** — set groove pattern per track from a phrase step
- **Scale (track / global)** — move one track, or all eight, onto one of the project's scales and a key
- **Tick rate** — control how fast the instrument table advances, per FX column
- **Three table playheads** — each of a table's FX columns runs at its own speed and loops on its own,
  so one table can drive cross-rhythms
- **Effect automation** — mark a start and a finish step and the value between them glides, on a
  choice of curve; the span may cross into a later phrase of the same chain. Volume, pan, the two
  sends, both faders and the filter all fade this way — and so do the two EQ effects, which slide
  the *contents* of one EQ preset into another rather than stepping between preset numbers
- **Track and master faders** — move a mixer fader from a phrase step; the move persists as the song
  plays on, and stopping restores what the mixer had

## Instrument Tables

Each instrument has its own 16-row mini-sequencer. It loops continuously while the note plays and lets you automate volume, pitch, and effects row by row — great for programmed arpeggios, tremolo, and rhythmic gating without using up phrase FX slots.

- EQ per note and master EQ from a table row, alongside the filter cells
- Effect automation in a table too — mark a start and a finish **row** and the value between them glides; HOP steers the fade, so a looped section can restart or continue it

## Instruments

- **Sampler**: load any WAV, MP3, FLAC, OGG, Opus or M4A file (mono or stereo); set root note, detune, volume, pan
- **SoundFont**: load SF2 and SF3 files; edit envelope (attack, decay, sustain, release), filter cutoff and resonance
- Loop modes: no loop, forward loop, ping-pong loop
- Reverse playback
- Non-destructive start and end point trimming
- Per-instrument real-time effects: low/high/band-pass filter, 3-band EQ, overdrive, bitcrusher, sample rate reduction (Lo-Fi)
- 4 modulation slots per instrument: envelope shapes (AHD, ADSR) and LFO targeting volume, pitch, filter, pan, and more
- Save and load instruments as preset files (.pti) — bundles all parameters, table, and modulation settings together

## Sample Editor

- Waveform display with zoom and selection
- Stereo source mode: use the left channel, right channel, both stereo, or averaged mono — without modifying the file
- Non-destructive editing: crop, copy, cut, paste within the waveform
- Reverse, normalize, fade in, fade out, silence a selection
- Undo last destructive operation
- Apply effects permanently: overdrive, bitcrusher, 3-band EQ, OTT compressor, dust (vinyl noise/wear)
- Read the sample's own tempo off the header — the BPM it plays at for the bar count on the DURATION row
- Pitch-shift to match a BPM target without changing length (repitch)
- Time-stretch to match a BPM target without changing pitch — Akai SOLA algorithm (same "jungle chop" character as the S950/S1000)
- Auto-detect transients to place slice markers, or divide into equal slices
- Place slices by hand — drag any boundary, delete one, or press START and tap A on every hit to chop by ear. A sample that already carries slices opens on them, ready to edit
- Slice markers embedded in the WAV cue chunk — compatible with M8, Blackbox, Reaper, Logic
- Export all slices as separate WAV files (CHOP)
- Assign slice playback mode on the instrument: trigger individual slices by note, play from slice to end, or standard pitch mode

## Resampling

Record what's currently playing in the sequencer into a new sample — capture a phrase or a whole section and resample it into an instrument slot.

## Mixing & Effects

- Mixer screen: volume for each of the 8 tracks and the master, plus send amounts to reverb and delay
- Mute and solo any track from the song or mixer screen, on the cursor or across a selection — the chord latches or stays momentary depending on which button you release first
- True dBFS peak meters per track
- Two send effects: reverb (Schroeder-Moorer algorithm) and a stereo delay
- Route delay output into the reverb — wet delay signal feeds the reverb input with no extra latency
- Reverb and delay each have their own 3-band EQ
- Master bus: OTT 3-band compressor or DUST (lofi/vinyl texture) — switchable, with wet/dry depth control — followed by a soft peak limiter
- Per-instrument EQ also accessible from the instrument screen

## Export

- Export the full song mix as a stereo WAV file
- Export each track as a separate stereo WAV stem, with reverb and delay send returns rendered alongside
- Offline render: same DSP chain as real-time playback, sample-accurate

## File Management

- Built-in file browser: navigate folders, sort files, preview samples before loading
- Convert video files to samples — extract the audio track (`.mp4`, `.mkv`, `.webm`, `.3gp`, `.mov`) to a WAV right in the file browser, with a preview before converting
- Rename files (SELECT+A to enter rename with on-screen QWERTY)
- Delete files and folders (SELECT+B with confirmation)
- On-screen QWERTY keyboard for naming projects and files
- Load a project while the sequencer is running — the new song takes over from its first row, so you
  can switch material mid-set
- Projects saved as .ptp files in `Projects/`, inside PocketTracker's home folder
- Instruments saved as .pti files in `Instruments/`, beside it
- Samples stored wherever you put them; paths stored in the project
- File move — move files and folders to a different location from within the browser

## Look & Feel

- Theme editor: full RGB palette for every UI color; save and load themes as .ptt files
- Several built-in themes to start from
- 6 visualizer modes for the top bar: oscilloscope, flat line, octascope (one scope per active track), full octascope (all 8 track scopes forced on), frequency spectrum, spectrum with peak hold

## Controls

- Help on SELECT: a tap describes the cell under the cursor in the visualizer strip
- Song-relative navigation, on by default: B+D-pad walks the arrangement instead of the 00–FF pools, so the chain and phrase on screen are always the ones the song plays (SETTINGS → NAV = POOL restores the old behaviour)
- Full physical button support (tested on Miyoo Flip and Ayaneo Pocket Air Mini)
- Touch layout: virtual buttons in portrait orientation
- Virtual button clicks: choose a sound and volume
- Haptic feedback on button press (toggle on/off)
- Auto-detects physical vs touchscreen device on launch
