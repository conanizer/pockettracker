# PocketTracker v1.0 — Feature Lock

This document defines what is in the v1.0 release, what must happen before shipping, and what is deliberately saved for later. Once a feature lands in the **Post-Release** section, it does not get built before release — it goes on the list instead.

---

## ✅ In v1.0 — Complete

### Audio Engine
- ✅ Sample-accurate note queue system (<0.02ms jitter)
- ✅ 8-voice polyphony with per-track voice stealing
- ✅ WAV sample playback (mono and true stereo)
- ✅ SoundFont (SF2) instruments via TinySoundFont
- ✅ Automatic sample rate compensation
- ✅ Advanced playback: start/end points, reverse, forward/ping-pong loop
- ✅ Linear interpolation for pitch-shifting (no aliasing)
- ✅ Stereo output with constant-power pan law
- ✅ 2-phrase lookahead buffering, <50ms startup latency
- ✅ Modulation engine: AHD, ADSR, LFO, DRUM, TRIG envelope types
- ✅ Table tick processing per voice
- ✅ Groove quantization (swing/shuffle)
- ✅ Resonant biquad filters (LP/HP/BP) per instrument
- ✅ Master bus: OTT 3-band compressor + DaisySP soft limiter
- ✅ Stereo send buses: DaisySP ReverbSc + ping-pong delay
- ✅ Delay→reverb cross-routing
- ✅ Offline WAV export (multi-track render)
- ✅ Real-time waveform capture for oscilloscope

### Effects (17 total)
- ✅ ARP/ARC — arpeggio (UP/DOWN/PINGPONG/RANDOM) + speed config
- ✅ OFF — sample start offset
- ✅ VOL — volume automation
- ✅ KIL — kill voice immediately
- ✅ REP — retrigger / retrigger with volume ramp
- ✅ PSL — pitch slide / portamento
- ✅ PBN — pitch bend (continuous)
- ✅ PVB/PVX — vibrato (standard and extreme)
- ✅ DEL — delay note by N ticks
- ✅ CHA — probability gate
- ✅ RND/RNL — randomize FX values
- ✅ TBL — override table ID
- ✅ THO — table hop to row
- ✅ GRV — groove assign per track
- ✅ TIC — table tick rate + special modes
- ✅ HOP — phrase/table jump (odd time signatures)

### Screens
- ✅ Phrase Editor — 16-step editing (note, volume, instrument, 3 FX slots)
- ✅ Chain Editor — 16 phrase references with per-slot transpose
- ✅ Song Editor — 8-track arrangement, 256 rows
- ✅ Instrument Screen — full parameter set (sample, ROOT, DETUNE, VOL, PAN, filters, loop, start/end, SF2 envelope overrides)
- ✅ Sample Editor — waveform view, zoom, selection tools, destructive FX (OTT/DUST/DRIVE/EQ), SYNC (RPITCH pitch-shift, TSTRETCH time-stretch), transient detection + slice markers, CHOP export
- ✅ Mixer — 8 tracks + master with true dBFS meters, REV/DEL return gain
- ✅ Effects — global reverb/delay config, delay→reverb routing
- ✅ EQ Editor — 3-band parametric EQ with KissFFT real-time spectrum analyzer
- ✅ Table Screen — 16-row mini-sequencer per instrument (256 tables)
- ✅ Groove Screen — step-timing patterns (256 grooves)
- ✅ Modulation Screen — 4-slot envelope/LFO editor per instrument
- ✅ Project Screen — name, tempo, save/load, CLEAN dialog
- ✅ Settings Screen — layout, scaling, button sound/vibration, cursor, note preview, visualizer, theme
- ✅ File Browser — navigation, sorting, preview, WAV/video audio extraction
- ✅ Oscilloscope — 8 visualizer modes: SCOPE, BARS, PEAKS, MIRROR, FLAT, OCTA, SPECT, SPCT.P

### Copy/Paste
- ✅ Selection mode cycling: CELL → ROW → SCREEN (L+B)
- ✅ Copy, cut, paste, delete on PHRASE / CHAIN / SONG screens
- ✅ Selection increment (A+DPAD applies to all selected rows)

### Navigation
- ✅ 5×5 navigation grid (R+DPAD)
- ✅ Context-sensitive screen availability per column

### Themes & Layout
- ✅ Theme system with built-in themes + custom theme editor (.ptt files)
- ✅ 4 layout modes: FULL, TOUCH_PORTRAIT, TOUCH_LANDSCAPE, TOUCH_PORTRAIT2
- ✅ Auto-switch on device rotation, persisted via SharedPreferences

### Data & Files
- ✅ Project save/load (.ptp JSON format)
- ✅ Forward migration support
- ✅ WAV cue chunk (slice markers compatible with M8, Blackbox, Reaper)
- ✅ SOURCE setting (LEFT/RIGHT/STEREO/MONO — non-destructive)
- ✅ 256 instrument slots, 256 phrases, 256 chains, 256 tables, 256 grooves
- ✅ Platform-agnostic architecture (IAudioBackend, IFileSystem, IResourceLoader)

---

## 🔲 Before Release — Remaining Tasks

These are not features. They must be done before v1.0 ships.

- [ ] "Hello world" song usability test — a new user should complete a basic song in under 5 minutes
- [ ] Bug hunt on Miyoo Flip (all screens, all effects, all input modes)
- [ ] Bug hunt on Ayaneo Pocket Air Mini
- [ ] Performance verification — stable 30-60fps on both devices
- [ ] Example project bundled with app
- [ ] README with installation guide and controls reference
- [ ] Controls guide (copy/paste, mixer, navigation, input combos)
- [ ] Short demo video
- [ ] Known issues list documented

---

## 🚀 Post-Release — Saved for Later

Everything here is explicitly excluded from v1.0. New ideas go here instead of being implemented now.

### Functionality
- Copy/paste for instrument settings (not just phrase/chain/song data)
- Filter automation effects (CUT, RES effects in phrase step)
- Per-track stem export (separate WAV per track)
- Undo/redo (global or per-screen)
- MIDI input/output
- Braids synthesizers (Mutable Instruments integration)

### Audio Engine
- SCALAR mod type (Phase 4 of Audio Module System — intentionally deferred)
- Mod-to-mod routing as a true 4×4 matrix (currently a fixed ring: slot M → slot (M+1)%4)
- `stageCounter` normalized to 0..1 progress (fix for drift when `rMult` changes mid-stage)
- `sinf` lookup table in LFO hot path (only needed if profiling shows bottleneck on Miyoo Flip)
- Fine pitch destination (dest=4) scale clarification or rename to avoid "fine = ±100 semitones" confusion

### Code Quality / Architecture
- Unify table processing between sampler and SF voices into a shared `processTableTick(IAudioVoice&)` function
- Move `isReleasingOnly` up to `IAudioVoice` as part of the above

### Platform
- Linux port (GTK/Qt UI, same portable controllers)

---

*Last updated: 2026-05-21*
