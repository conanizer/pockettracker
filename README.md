# PocketTracker

**Free, open-source music tracker for Android gaming handhelds**

PocketTracker is a portable music tracker in the tradition of LSDJ, M8, and Little Piggy Tracker — designed to run on Android handhelds and smartphones.

> **Note:** This project was developed with significant AI assistance (Claude Code). If that's a dealbreaker for you, this project isn't for you — no hard feelings.

**Status:** Testing & Polish — public release date TBD  
**License:** [GPL-3.0-or-later](LICENSE)

---

## Features

### Audio engine
- Sample-accurate C++ engine (<0.02 ms jitter), 8-voice polyphony with per-track voice stealing
- WAV instruments (8–32-bit PCM / float, true stereo) + SoundFont (SF2) via TinySoundFont
- Per-instrument chain: SVF filter (LP / HP / BP) + bitcrush + drive; constant-power pan
- Stereo send buses — reverb + ping-pong delay; master bus — OTT 3-band compressor + limiter
- Modulation engine: 4 slots per instrument (AHD / ADSR / LFO / DRUM / TRIG), 10 destinations including mod-to-mod routing
- Groove quantization (256 grooves, per-track)

### Sample editor
- Waveform view with zoom; non-destructive SOURCE (LEFT / RIGHT / STEREO / MONO) and RATE (HIGH / NORM / LOFI)
- Destructive ops: crop, copy / cut / paste, normalize, fade, silence, reverse, undo
- SYNC: **RPITCH** (pitch-shift to BPM) · **TSTRETCH** (SOLA time-stretch, Akai-cyclic)
- Offline FX: EQ · DUST · DRIVE · OTT; transient detection + slice markers with WAV cue chunk; CHOP export

### Workflow
- Project save / load (`.ptp` JSON), file browser with WAV / video preview, full-song WAV export
- M8-style copy / paste (CELL → ROW → SCREEN), selection resampling, CLEAN
- 4 layout modes (FULL / TOUCH PORTRAIT / TOUCH LANDSCAPE / TOUCH PORTRAIT2), auto-rotation, physical gamepad + virtual controls

---

## Supported devices
 Almost any Android handheld or smartphone

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
| [skoomaDust](https://github.com/skoomabwoy/skoomaDust) | GPL-3.0 | Lo-fi effect chain; includes APComp FET compressor by Alain Paul (BSD-3-Clause) |
| [Jetpack Compose](https://developer.android.com/jetpack/compose) | Apache 2.0 | Android UI toolkit |
| [Kotlinx Serialization](https://github.com/Kotlin/kotlinx.serialization) | Apache 2.0 | JSON project save / load |

### DSP algorithm references

- **SOLA time-stretch** — Roucos & Wilgus (1985), Verhelst & Roelands (1993). The "Akai-cyclic" mode matches the algorithm used in Akai S950 / S1000 samplers; the characteristic grit on jungle breaks is intentional.
- **Biquad filter design** — Robert Bristow-Johnson, *Audio EQ Cookbook* (1994, rev. 2016).
- **OTT 3-band compressor** — Parameters matched to the Xfer Records vitOTT plugin. Downward: −27 dBFS / 8:1; upward: −35 dBFS / 4:1; ~8 dB neutral zone.
- **Spectral flux transient detection** — Brossier et al., "Fast labelling of notes in music signals," ICASSP 2004.

### Contributors

- [@skoomabwoy](https://github.com/skoomabwoy) — authored the [skoomaDust](https://github.com/skoomabwoy/skoomaDust) lo-fi effect chain integrated into PocketTracker; ongoing technical advice throughout the project

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
