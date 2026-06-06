# Changelog

All notable changes to PocketTracker are documented here.

---

## [1.0.0] — 2026 (first public release)

### Audio engine
- Sample-accurate C++ engine via Oboe (<0.02 ms jitter), 8-voice polyphony with per-track voice stealing
- WAV instruments (8–32-bit PCM / float, true stereo); SoundFont (SF2) via TinySoundFont with per-channel rendering
- Per-instrument chain: SVF resonant filter (LP / HP / BP) + bitcrush + drive; constant-power stereo pan
- Stereo send buses — DaisySP ReverbSc + ping-pong delay; master bus — OTT 3-band compressor + DaisySP soft limiter
- Modulation engine: 4 slots per instrument, types AHD / ADSR / LFO / DRUM / TRIG, 10 destinations including mod-to-mod routing
- Groove quantization (256 grooves, per-track assignment)
- Offline WAV export (full-song multi-track render)

### Sample editor
- Waveform view with zoom; non-destructive SOURCE (LEFT / RIGHT / STEREO / MONO) and RATE (HIGH / NORM / LOFI)
- Destructive ops: crop, copy / cut / paste, normalize, fade in/out, silence, reverse; single-level undo
- SYNC mode: RPITCH (pitch-shift to BPM) and TSTRETCH (SOLA time-stretch, Akai-cyclic algorithm)
- Offline FX: parametric EQ, DUST (lo-fi chain), DRIVE (tanh soft clipper), OTT multiband compressor
- Transient detection (KissFFT spectral flux) with slice markers embedded as WAV cue chunks
- CHOP export — slices saved as individual WAV files

### Sequencer
- Hierarchical structure: Song → Chain → Phrase → Step, 256 phrases / 256 chains / 8 tracks
- 17 effects: ARP, ARC, OFF, VOL, KIL, REP, PSL, PBN, PVB, PVX, DEL, CHA, RND, RNL, TBL, THO, GRV, TIC, HOP
- Tables (16-row mini-sequencer per instrument), Grooves (step-timing patterns for swing/shuffle)
- M8-style copy/paste: CELL → ROW → SCREEN selection; copy, cut, paste, delete, selection increment

### Screens
- Phrase editor, Chain editor, Song editor, Instrument screen, Sample editor
- Mixer (8 tracks + master, true dBFS meters, REV/DEL return volume)
- Effects screen (reverb + delay send config, delay→reverb routing)
- EQ editor (3-band parametric, KissFFT real-time spectrum analyzer + biquad curve overlay)
- Table screen, Groove screen, Modulation screen
- Project screen (name, tempo, save/load, CLEAN)
- File browser (WAV / video audio preview, folder navigation)
- Settings screen (layout, scaling, screen overlay, button sound/vibration, cursor persistence, visualizer, theme)

### UI
- Pixel-perfect 640×480 rendering with integer scaling and letterboxing
- 8 visualizer modes: SCOPE, BARS, PEAKS, MIRROR, FLAT, OCTA, SPECT, SPCT.P
- Theme system with 4 built-in themes (CLASSIC / AMBER / BLUE / MONO) and `.ptt` theme file support
- Screen overlay system — PNG overlays (e.g. CRT scanlines) from `assets/overlays/`
- 4 layout modes: FULL, TOUCH PORTRAIT, TOUCH LANDSCAPE, TOUCH PORTRAIT2; auto-rotation
- Virtual gamepad (touchscreen-only devices) + physical button support (gaming handhelds)

### Project
- Save / load as `.ptp` (JSON) with forward migration
- GPL-3.0-or-later license
