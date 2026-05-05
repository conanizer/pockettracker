# Development Status

**Last Updated:** 2026-05-05

## Current Phase

**All Extension Packs COMPLETE!** -> Testing & Polish -> Documentation -> MVP Release

**Target Release:** April 2026

---

## Timeline

```
Weeks 1-2:   Refactoring                                          COMPLETE
Weeks 3-4:   Effects system                                       COMPLETE
Week 5:      Copy/paste system                                    COMPLETE
Weeks 6-7:   MVP Expansion (Mixer + WAV Export)                   COMPLETE
Weeks 8-10:  Extension Pack 2 (Tables, HOP/TIC, Pitch Effects)   COMPLETE
Weeks 11-13: Extension Pack 3 (Groove, Modulation, Resampling)   COMPLETE
Week 14:     Testing & Polish                                     CURRENT
Week 15:     Documentation & video
Week 16:     MVP Release
```

---

## What's Working

### Core Systems
- Pixel-perfect rendering at 640x480 with letterboxing
- Virtual controls with device detection (gaming handheld vs touchscreen)
- Generic input handler (A+direction for value editing)
- File management (save/load .ptp projects)
- Navigation system (5x5 screen grid with SHIFT+DPAD)
- Key repeat (hold D-PAD / A+DPAD / B+DPAD)

### Audio Engine
- Platform-agnostic architecture (IAudioBackend + IResourceLoader interfaces)
- Sample-accurate note queue system (C++ priority queue, <0.02ms jitter)
- Linear interpolation (eliminates aliasing artifacts during pitch-shifting)
- Native C++ audio engine with Oboe (44.1kHz, OpenSL ES preferred, AAudio fallback)
- 8-voice polyphony with per-track voice stealing + 3-step allocator
- 256 sample slots with stereo/mono WAV loading (auto-converts)
- Automatic sample rate compensation
- SoundFont (SF2) instruments via TinySoundFont — per-channel rendering (`tsf_render_float_channel` fork), full mod matrix parity with sampler (ADSR/LFO/table/pitch effects), per-instrument filter/drive/crush post-render, SF2 envelope/filter overrides (ATK/DEC/SUS/REL/CUT/RES on instrument screen)
- Advanced playback: start/end points, reverse, looping (fwd/ping-pong)
- Queue-based playback: phrase, chain (with transpose), song (8-track)
- Continuous buffering with 2-phrase lookahead
- <50ms startup latency
- Accurate playback cursors (frame-based position tracking)
- Real-time waveform capture for oscilloscope
- Stereo output with constant-power pan law
- Modulation engine (AHD, ADSR, LFO, DRUM, TRIG on all destinations)
- Table tick processing (per-voice, per-note)
- Offline WAV render (processAudioBlock unified DSP)
- Groove quantization (per-track groove assignments)
- Resonant biquad filters (LP/HP/BP)
- Master output bus (MasterChain): DaisySP peak-tracking soft limiter + OTT 3-band bidirectional compressor (vitOTT-matched, wet/dry depth control)
- **Send effects buses (reverb + delay)**: true stereo buses — sampler voices contribute with per-instrument PAN applied; SF voices use their already-panned stereo buffer. DaisySP ReverbSc (Schroeder-Moorer) + DaisySP DelayLine (ping-pong stereo tap delay). Per-send return gain (00-FF), delay→reverb routing (`delayReverbSend`), per-send input EQ. Delay processed first so its wet output can feed the reverb bus in the same audio block with zero latency.

### Screens & Modules
- **Oscilloscope** - Real-time waveform visualization (60 FPS)
- **Phrase Editor** - 16-step editing with N/V/I/FX columns
- **Chain Editor** - 16 phrase references with transpose
- **Song Editor** - 8-track arrangement, 256 rows, B+UP/DOWN page jump
- **Instrument Screen** - Full parameter set (sample, ROOT, DETUNE, VOL, PAN, filters, loop, start/end); EQ row opens EQ editor via SELECT button (same as mixer and effects screens); filename displayed without extension
- **Project Screen** - Name, tempo, save/load, CLEAN SEQ/INST, layout mode switcher
- **File Browser** - Navigation, sorting, preview, WAV/video audio extraction
- **Effects Screen** - Global send effects config: reverb (SIZE/DAMP/EQ), delay (TIME/FDBK/REV-send/EQ), master bus type selector. WET removed (always 100%); REV row on delay sends delay output into reverb bus.
- **EQ Editor Screen** - Full-screen 3-band parametric EQ editor (TYPE/FREQ/GAIN/Q per band). Opened from mixer master col, effects screen INP EQ rows, or instrument screen EQ row via SELECT.
- **Mixer Screen** - 8 tracks + master with true dBFS meters; REV/DEL return volume (rows 1-2 in master col); stereo peak meters for REV/DEL send channels (`sendPeaks[4]`: revL/revR/delL/delR)
- **Table Screen** - 16-row mini-sequencer per instrument
- **Groove Screen** - Step-timing patterns for swing/shuffle (256 grooves)
- **Modulation Screen** - 4-slot envelope/LFO editor per instrument
- **Settings Screen** - Layout mode, scaling, button sound/volume, vibration, keyboard insert mode, cursor persistence

### Effects (All Complete)
- **ARP/ARC** - Arpeggio with UP/DOWN/PINGPONG/RANDOM modes and speed control
- **OFF** - Sample start point offset
- **VOL** - Volume automation
- **KIL** - Kill voice immediately
- **REP** - Retrigger (single shot or volume-ramp mode)
- **PSL** - Pitch slide / portamento
- **PBN** - Pitch bend (continuous up or down)
- **PVB/PVX** - Vibrato (standard and extreme)
- **DEL** - Delay note by N ticks
- **CHA** - Probability gate
- **RND/RNL** - Randomize FX values
- **TBL** - Override table ID
- **THO** - Table hop to row
- **GRV** - Groove assign per track
- **TIC** - Table tick rate + special modes
- **HOP** - Phrase/table jump (odd time signatures)

### Copy/Paste (M8-Style)
- Selection mode (L+B to enter/cycle: CELL -> ROW -> SCREEN)
- Copy (B in selection), Cut (L+A in selection), Paste (L+A outside)
- Delete selection (A+B in selection)
- Works on PHRASE, CHAIN, SONG screens
- Selection increment (A+DPAD applies to all selected rows)

### Layout System
- 4 layout modes: FULL, TOUCH_PORTRAIT, TOUCH_LANDSCAPE, TOUCH_PORTRAIT2
- Auto-switch on device rotation
- Layout + scaling mode persisted via SharedPreferences
- Pixel-perfect font rendering (anti-alias=false + horizontal run merging)

### Data Model
- Hierarchical: Project -> Song -> Chain -> Phrase -> Step
- 256 phrases, 256 chains, 8 tracks
- 256 instruments, 256 tables, 256 grooves
- ModSlot[4] per instrument
- JSON serialization with forward migration
- Platform-agnostic (no Android dependencies)

---

## Testing Devices

| Device | RAM | OS | Resolution | Status |
|--------|-----|-----|-----------|--------|
| Miyoo Flip (primary) | 1GB | Android 13 (GammaCoreOS) | 640x480 | All features working |
| Ayaneo Pocket Air Mini | 3GB | Android 11 | Scaling adjusted | All features working |

**Minimum Requirements:** Android 8.0+ (API 26), 64-bit, ~512MB RAM, ~50MB storage, 640x480 minimum

---

## Known Issues

- Generic input warning spam after device restart (harmless, goes away after reboot)

## Architecture Debt (Post-MVP)

- **Table processing is duplicated** — sampler and SF voices each have their own table row-advance loop in `audio-engine.cpp`. Both operate on `IAudioVoice` fields and contain identical logic (HOP, TIC, transpose, volume). Should be unified into a single `processTableTick(IAudioVoice&)` called from one loop. `isReleasingOnly` should move up to `IAudioVoice` as part of this. See `docs/plan-module-system.md` for details.

- **Mod-to-mod routing is a fixed ring** — slot M can only modulate slot (M+1)%4. A true 4×4 matrix (each modulator targets any of the other three) would cost just 16 extra bytes per voice. Current ring is intentional for simplicity, but limits expressive patches. Document this clearly in the user manual before release.

- **stageCounter drifts when rMult changes mid-stage** — `tickADSR`/`tickAHD` store elapsed samples in stage-local units. If mod-to-mod routing changes `effectiveRateMult` mid-stage, the already-elapsed counter is in the old unit, causing incorrect stage length. In practice only affects LFO→ENV_RATE patches; normal usage is unaffected. Fix: store normalized 0..1 stage progress instead of absolute samples. Deferred post-MVP.

- **sinf in LFO hot path** — `lfoShape()` calls `sinf()` every audio block for every active sine-wave LFO. On low-end ARM (Miyoo Flip) this is measurable. Fix: 256-entry float sine wavetable (~1 KB) with linear interpolation for the sine case; triangle/ramp/square are already branchless. Only worth doing if profiling shows LFO is a bottleneck.

- **Fine pitch destination (dest=4) naming ambiguity** — PARAM_PITCH dest=4 applies `effectiveAmt * 1.0` semitones, identical range to coarse pitch (dest=3, scale=12). "Fine" conventionally means ±1 semitone (cents). Either cap the scale to 0.01–0.1 or rename the destination to avoid misleading users. Deferred post-MVP.

---

## Remaining Work

### Testing & Polish (Current)
- [ ] "Hello world" song usability test (<5 min)
- [ ] Bug hunting on both devices
- [ ] Performance verification (stable 30-60fps)
- [ ] Example project creation

### Documentation
- [ ] README finalization
- [ ] Controls guide (full reference)
- [ ] Short demo video
- [ ] Known issues list

---

## Completed Milestones

### Module Code Style Unification (Complete - 2026-05-05)

Standardised all screen modules to a single consistent style (new modules led, old modules updated):

- **Hex formatting**: all `.toString(16).padStart(2,'0').uppercase()` chains replaced with `.toHex2()` (defined in `EditorHelpers.kt`) across every module — PhraseEditorModule, ChainEditorModule, SongEditorModule, ModulationModule, GrooveModule, TableModule, SettingsModule.
- **Row background helper**: inline `bgColor when { }` blocks (playing row / selection / cursor / every-4th / default) replaced with `rowBgColor()` from `EditorHelpers.kt` in PhraseEditorModule, ChainEditorModule, SongEditorModule, and TableModule.
- **Comment discipline**: removed all `// ===== STEP N: =====` section separators and verbose docstrings that described WHAT rather than WHY from ChainEditorModule and SongEditorModule. PhraseEditorModule trimmed of column-label comments, keeping only the beat-accent WHY note.
- **EditorHelpers consolidation**: `clearChainSlot()` and `clearSongChainRef()` from EditorHelpers now used in handleInput DELETE cases (was duplicated inline).

### Architecture Refactoring (Complete)
- IAudioBackend interface + OboeAudioBackend implementation
- IResourceLoader interface + AndroidResourceLoader implementation
- IFileSystem interface + AndroidFileSystem implementation
- InputController, PlaybackController, InstrumentController, TrackerController, EffectProcessor, FileController, ClipboardManager
- MainActivity reduced from 2668 to 1862 lines

### MVP Expansion #1 (Complete - 2026-01-27)
- Mixer Screen with dBFS meters
- WAV Export (multi-track rendering)
- Instrument VOL/PAN
- Stereo pan in audio engine
- Volume chain: instrument x phrase x track x master

### Extension Pack 2 (Complete - 2026-02-05)
- Table data model, screen UI, and audio processing
- TIC effect (table tick rate + special modes)
- HOP effect (phrase/table jump)
- Pitch effects (PSL, PBN, PVB, PVX)

### Send Effects System (Complete - 2026-05-04)

- **Stereo send buses**: reverb and delay buses upgraded from mono to stereo L/R pairs. Sampler voices contribute with constant-power PAN applied; SF voices use their already-panned stereo buffer. Instrument PAN now affects reverb/delay positioning.
- **Return gain concept**: `reverbWet`/`delayWet` (00-FF) control how loud the send bus output is in the master mix — not an internal wet/dry ratio. The modules always output 100% processed signal. `reverbReturnGain`/`delayReturnGain` floats in AudioEngine apply the gain in the mix loop.
- **Delay→Reverb routing**: new `delayReverbSend` field (00-FF) in `Project`. Delay is processed first; its wet output is scaled by `delayToReverbSend` and added to the reverb send bus before the reverb processes — zero latency cross-routing.
- **Effects screen restructure**: WET rows removed from both reverb and delay sections (always 100% wet). New REV row added to delay section for `delayReverbSend`. 8 cursor rows (was 9). New `CURSOR_TO_VIS` mapping for visual layout.
- **Mixer REV/DEL return volume**: rows 1-2 in the master column display and edit `reverbWet`/`delayWet`. Separate concept from the internal wet/dry that was removed.
- **Stereo send peak meters**: `sendPeaks[4]` (revL, revR, delL, delR) threaded from C++ `processAudioBlock` through JNI→`OboeAudioBackend`→`AudioEngine`→`TrackerScreenParams`→`PixelPerfectRenderer.drawLayout()`→`MixerState`.
- **Instrument EQ shortcut**: EQ row on instrument screen opens the EQ editor via SELECT button, using `EqCallerContext.InstrumentEq`, matching the workflow on mixer and effects screens.
- **Filename extension stripping**: instrument screen shows sample name without `.wav`/`.sf2` extension.

### Mod Module System Split (Complete - 2026-04-27)
- Extracted all modulation state machines from `audio-engine.cpp` into focused headers under `mods/`
- `mods/primitives/lfo-oscillator.h` — `lfoShape()` shared by LFO and vibrato
- `mods/modules/ahd-module.h` — `tickAHD()` (AHD + DRUM envelopes)
- `mods/modules/adsr-module.h` — `tickADSR()` (ADSR + TRIG envelopes)
- `mods/modules/lfo-module.h` — `tickLFO()` (phase advance + shaping)
- `mods/modules/pitch-slide-module.h` — `tickPitchSlide()` (PSL/PBN)
- `mods/modules/vibrato-module.h` — `tickVibrato()` (PVB/PVX)
- `mods/mod-runner.h` — `runModMatrix()` (orchestration; replaces ~300-line switch in `updateVoiceModulation`)
- `updateVoiceModulation()` and `updateVoicePitchMod()` in `audio-engine.cpp` are now one-liner shells
- **Bug fix**: pitch slide stop condition (`< 100.0f` sentinel removed; slide always stops when target is reached)
- Per-sample envelope interpolation (`prevEnvValue` snapshot) confirmed intact after split

### Audio Module System (Complete - 2026-04-17)
- **Phase 0**: Split `native-audio.cpp` monolith into focused files (`filter.h`, `mod-system.h`, `sampler-voice.h`, `soundfont-voice.h/.cpp`, `audio-engine.h/.cpp`, `jni-bridge.cpp`)
- **Phases 1–3**: `modSourceValues[]` + `modDestValues[]` arrays on all voices; `processRoutes` unified routing loop; all bypass paths (table vol/pitch, PSL/PBN, vibrato) route through source array
- **Phase 5**: SF voices inherit full modulation engine — same `updateVoiceModulation()` as sampler; ADSR/LFO/AHD/DRUM/TRIG work on SF instruments with zero SF-specific code
- **Phase 6**: `tsf_render_float_channel` fork — per-track SF buffers enable per-instrument effects
- **Phase 7**: Per-instrument filter/drive/bitcrush applied to SF output buffer post-render
- **Phase 8**: SF preset parameter overrides (ATK/DEC/SUS/REL/filterCut/filterRes) editable on instrument screen via `SFOverrides`, applied by `applySoundfontEnvelopeOverrides` at note trigger
- **SF bug fixes**: HOP effect now works with SF tables; KIL/REL — ADSR release and TSF-native REL both audible after KIL; table arpeggio continues during release tail (matches sampler behavior)
- **Phase 4 (SCALAR mod type)**: Deferred to post-MVP
- **Master chain (2026-04-24)**: `MasterChain` with `LimiterModule` (DaisySP soft limiter, stereo L+R); `InstrumentChain` per-voice effects replaced inline hard-clipper on final bus
- **OTT multiband compressor (2026-04-25)**: `OttModule` — 3-band bidirectional compressor on the master bus. `LRCrossover` at 120 Hz / 2500 Hz → per-band DaisySP downward + custom `UpwardCompressor`. vitOTT-matched settings: downward −27 dBFS / 8:1, upward −35 dBFS / 4:1 (8 dB neutral zone prevents note-tail boost), band time constants Low 2.8 ms/40 ms · Mid 1.4 ms/28 ms · High 0.7 ms/15 ms, +6 dB post-band output gain. Silence-detection auto-reset (500 ms) starts a 512-sample warmup on every playback START, hiding the LR4 zero-state filter transient. Three bug fixes: pop on START (upward compressor `gainRec` now reset in silence gate), fade-in on START (warmup on every silence→signal transition), OTT depth not updating live (Kotlin `when` block checked `cursorCol < 8` before `ottDepthChanged`)

### Extension Pack 3 (Complete - 2026-03-13)
- Fixes & UX updates (table vol range, FX cycling, key repeat, selection increment)
- New effects (DEL, CHA, RND, RNL, TBL, THO, GRV, REP XY rework)
- Groove screen
- Modulation screen & engine (AHD, ADSR, LFO, DRUM, TRIG)
- Selection resampling
- Audio bug fixes (oscilloscope SIGSEGV, AHD crackling, voice-steal click)
- Layout system (touchscreen modes, orientation auto-switch)
- Polish (CLEAN dialog, pixel-perfect font, SharedPreferences persistence)

---

## Post-MVP Features

### Early Post-MVP (With Mentor)
- Advanced copy/paste (instrument settings)
- Linux port (GTK/Qt UI with same controllers)
- Braids synthesizers (Mutable Instruments integration)
- Filter automation (CUT, RES effects)

### Later Features
- Undo/redo
- Per-track stem export
- Advanced filters (EQ, compressor)
- Stereo meters per track
- Effect sends (reverb, delay, chorus)
- Themes/color schemes
- Alternative visualizers (EQ spectrum, spectrogram, dB meters)

---

## Technical Notes

### Audio Engine Details
- Sample rate: 44100 Hz (forced via Oboe builder; device converts to native rate internally)
- Audio API: OpenSL ES preferred (avoids CCodec enumeration on startup), AAudio fallback
- Stream open order: OpenSL ES Exclusive → OpenSL ES Shared → OpenSL ES None/Shared → AAudio Exclusive
- Performance mode: LowLatency
- Sharing mode: Exclusive where supported, Shared otherwise
- Format: Float32, stereo output
- Buffer size: Auto-selected (~192-480 frames)
- All samples stored as mono
- Playback rate = target_frequency / base_frequency
- Audio init runs off main thread (Dispatchers.IO) to prevent UI freeze on startup
- Hot-path logging gated behind AUDIO_TRACE=0 compile flag (set to 1 for per-note tracing)

### SoundFont (SF2) Details
- TinySoundFont (TSF) header-only synthesizer
- One shared `tsf*` handle per SF2 slot (up to 8 slots = 8 different SF2 files)
- Each track maps to a MIDI channel (track 0 = ch 0 … track 7 = ch 7) on the slot's handle
- `tsf_render_float()` called once per active slot per audio block — no per-track clone
- SF2 loading: `tsf_load_filename()` directly; no file buffer copy kept in memory
- Memory: ~1× SF2 file size (single handle), vs old architecture which used ~8×

### MIDI Note Convention
- C-4 = MIDI 60 (middle C)
- Formula: `(octave + 1) * 12 + pitch`
- Frequency: `440 * 2^((midi - 69) / 12)`

### Instrument Slots
- All 256 slots (00-FF) are identical in structure
- All start empty (`sampleFilePath = null`) — no bundled default samples
- Users load samples into any slot via the file browser
- Each instrument has independent ROOT+DETUNE tuning

---

## MVP Definition of Done

### Core Functionality (All Complete)
- [x] Create phrases with notes and effects
- [x] Copy/paste phrase steps (M8-style selection)
- [x] Chain phrases with transpose
- [x] Arrange songs with 8 tracks
- [x] Save and load projects
- [x] All effects working (17+ effects)
- [x] Modulation system (4 slots per instrument)
- [x] Tables and grooves

### Remaining for Release
- [ ] User can complete "hello world" song in <5 min
- [ ] README explains how to install and use
- [ ] Example project included
- [ ] Demo video recorded
- [ ] No known crash bugs
