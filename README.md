# PocketTracker

**Free, open-source music tracker for Android gaming handhelds**

PocketTracker is a portable music tracker in the tradition of M8, LSDJ, and Picotracker — designed to run on affordable Android handhelds and touchscreen devices, with a 640×480 pixel-art interface and a sample-accurate C++ audio engine.

> Compose and perform music anywhere, on a $60 handheld.

**Status:** Testing & Polish — public release date TBD  
**License:** [GPL-3.0-or-later](LICENSE)

---

## Features

### Audio engine
- Sample-accurate note scheduling (<0.02 ms jitter) via Oboe / C++
- 8-voice polyphony with per-track voice stealing
- 256 instrument slots — WAV (8/16/24/32-bit PCM + float), true stereo or mono
- SoundFont (SF2) instruments via TinySoundFont — full mod-matrix parity with sampler
- Resonant SVF filters (LP / HP / BP) per instrument + bitcrush + drive
- Constant-power stereo pan; per-instrument volume chain (inst × phrase × track × master)
- Stereo send buses — reverb (Schroeder-Moorer) + ping-pong delay with EQ and cross-routing
- Master bus — 3-band OTT compressor + peak-tracking soft limiter
- Modulation engine — 4 slots per instrument: AHD, ADSR, LFO, DRUM, TRIG  
  Destinations: VOL, PAN, PITCH, FILTER CUT/RES, SAMPLE START, mod-to-mod routing
- Groove quantization (256 grooves, per-track assignment)

### Composition screens
- **Phrase editor** — 16 steps, note / volume / instrument / 3 FX columns
- **Chain editor** — 16 phrase references with per-slot transpose
- **Song editor** — 8 tracks × 256 rows with page navigation
- **Table screen** — 16-row mini-sequencer per instrument (transpose, volume, 3 FX)
- **Groove screen** — step-timing patterns for swing / shuffle
- **Modulation screen** — 4-slot envelope / LFO editor per instrument
- **Mixer screen** — 8 track + master volumes with true dBFS meters and REV / DEL returns
- **Effects screen** — global send bus config (SIZE / DAMP / TIME / FDBK / REV-send / EQ per bus)
- **EQ editor** — 3-band parametric EQ with real-time spectrum analyzer (master / track / instrument)

### Phrase effects
`ARP` `ARC` `OFF` `VOL` `KIL` `REP` `PSL` `PBN` `PVB` `PVX` `DEL` `CHA` `RND` `RNL` `TBL` `THO` `GRV` `TIC` `HOP`

### Sample editor
- Waveform view with zoom; SOURCE mode — LEFT / RIGHT / STEREO / MONO (non-destructive)
- Destructive ops: crop, copy / cut / paste, normalize, fade in/out, silence, reverse, undo
- Non-destructive RATE mode (HIGH / NORM / LOFI)
- SYNC mode: **RPITCH** (pitch-shift to BPM) and **TSTRETCH** (SOLA time-stretch to BPM, Akai-cyclic)
- Offline FX: EQ, DUST (lo-fi chain), DRIVE (tape saturation), OTT
- Transient detection + slice markers with WAV `cue ` chunk round-trip (M8 / Blackbox / Reaper compatible)
- SLICE playback modes on instrument screen (OFF / CUT / TRU); CHOP export

### Workflow
- Project save / load (`.ptp` — JSON with forward migration)
- File browser with WAV / video audio preview
- Full-song WAV export (offline render, all 8 tracks)
- Selection resampling — render a phrase selection to a new instrument slot
- M8-style copy / paste (CELL → ROW → SCREEN selection)
- CLEAN — remove unused sequences or instruments (with confirmation)

### Input & display
- Pixel-perfect 640×480 rendering (integer scaling, letterboxed)
- 4 layout modes: FULL, TOUCH PORTRAIT, TOUCH LANDSCAPE, TOUCH PORTRAIT2
- Auto-switch portrait ↔ landscape on rotation; layout persisted across restarts
- Physical gamepad / handheld button support (auto-detected via Android InputDevice)
- Virtual on-screen controls with full multi-touch combo support

---

## Supported devices

| Device | RAM | Android | Status |
|---|---|---|---|
| Miyoo Flip (primary) | 1 GB | 13 (GammaCoreOS) | ✅ All features |
| Ayaneo Pocket Air Mini | 3 GB | 11 | ✅ All features |
| Any Android phone / tablet | — | 8.0+ | ✅ Virtual controls |

**Minimum requirements:** Android 8.0 (API 26) · 64-bit · ~512 MB RAM · ~50 MB storage · 640×480 screen

---

## Installation

### APK (recommended for end users)

APK download will be posted here at public release.

### Build from source

**Requirements:** Android Studio Hedgehog+, NDK 25.1+, CMake 3.22.1+

```bash
git clone https://github.com/conanizer/pockettracker.git
cd pockettracker
./gradlew assembleDebug          # build
./gradlew installDebug           # build + install to connected device
```

Output APK: `app/build/outputs/apk/debug/`

---

## Documentation

| Document | Contents |
|---|---|
| [`docs/manual-en.md`](docs/manual-en.md) | Full user manual |
| [`docs/input-system.md`](docs/input-system.md) | Complete controls reference |
| [`docs/dsp-settings-guide.md`](docs/dsp-settings-guide.md) | Internal DSP tuning constants |
| [`docs/technical-architecture.md`](docs/technical-architecture.md) | Architecture overview |
| [`docs/development-status.md`](docs/development-status.md) | Feature status & known issues |

---

## Architecture

Portable core + platform adapters, so the C++ audio engine and business logic are shared:

```
core/audio/        IAudioBackend, AudioEngine (platform-agnostic)
core/logic/        TrackerController, PlaybackController, EffectProcessor, …
core/storage/      IFileSystem, WavWriter
platform/android/  OboeAudioBackend, AndroidFileSystem, …
app/src/main/cpp/  audio-engine.cpp — all DSP runs here (<0.02 ms jitter)
```

A Linux port is planned post-MVP (all interfaces already abstracted).

---

## Contributing

Not yet accepting external contributions — approaching first public release. After release:

- Bug reports → GitHub Issues
- Feature requests → GitHub Discussions
- Code / docs / examples welcome

---

## Credits

### Libraries

| Library | License | Use in PocketTracker |
|---|---|---|
| [Oboe](https://github.com/google/oboe) | Apache 2.0 | Low-latency Android audio stream |
| [DaisySP](https://github.com/electro-smith/DaisySP) | MIT | SVF filter, ReverbSc, DelayLine, Compressor, Limiter, BitCrush |
| [TinySoundFont](https://github.com/schellingb/TinySoundFont) | MIT | SF2 / SoundFont2 synthesizer (with per-channel rendering fork) |
| [KissFFT](https://github.com/mborgerding/kissfft) | BSD-3-Clause | FFT for spectrum analyzer and transient detection |
| [Soundpipe](https://github.com/PaulBatchelor/Soundpipe) (pareq stub) | MIT | Parametric EQ biquad |
| DustChain (skoomaDust) | GPL-3.0 | Lo-fi effect chain; includes APComp FET compressor by Alain Paul (BSD-3-Clause) |
| [Jetpack Compose](https://developer.android.com/jetpack/compose) | Apache 2.0 | Android UI toolkit |
| [Kotlinx Serialization](https://github.com/Kotlin/kotlinx.serialization) | Apache 2.0 | JSON project save / load |

### DSP algorithm references

- **SOLA time-stretch** — Roucos & Wilgus (1985), Verhelst & Roelands (1993). The "Akai-cyclic" mode matches the algorithm used in Akai S950 / S1000 samplers; the characteristic grit on jungle breaks is intentional.
- **Biquad filter design** — Robert Bristow-Johnson, *Audio EQ Cookbook* (1994, rev. 2016).
- **OTT 3-band compressor** — Parameters matched to the Xfer Records vitOTT plugin. Downward: −27 dBFS / 8:1; upward: −35 dBFS / 4:1; ~8 dB neutral zone.
- **Spectral flux transient detection** — Brossier et al., "Fast labelling of notes in music signals," ICASSP 2004.

### Inspiration

- [Dirtywave M8](https://dirtywave.com/) — the portable tracker that proved the concept
- [LGPT / Picotracker](https://github.com/Mdashdotdashn/LittleGPTracker) — open-source tracker heritage
- [LSDJ](https://www.littlesounddj.com/) — Game Boy tracker that defined the form

---

## License

PocketTracker — free open-source Android music tracker  
Copyright © 2025–2026 conanizer

This program is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License v3.0** as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but **without any warranty**; without even the implied warranty of merchantability or fitness for a particular purpose. See the [GNU General Public License](https://www.gnu.org/licenses/gpl-3.0.html) for details.

Full license text: [`LICENSE`](LICENSE)
