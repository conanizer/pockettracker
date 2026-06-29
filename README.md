# PocketTracker

**A free, open-source music tracker for Android gaming handhelds and phones.**

PocketTracker is a sample-based tracker built around the classic **Song → Chain → Phrase → Table** workflow — the lineage of the legendary LSDJ, carried forward by its modern reinterpretation, the Dirtywave M8. Make full tracks on the device in your pocket: write patterns, chop and mangle your own samples, shape them with envelopes and effects, mix it all down, and export — no desktop required.

Play it with the physical buttons on your Android handheld, or with on-screen touch controls on your phone — complete with an Amiga-style theme.

> **Note:** This project was developed with AI assistance. If that bothers you, this project isn't for you.

**Status:** 0.9.0 — first public release (pre-1.0)
**License:** [GPL-3.0-or-later](LICENSE)

---

## Features

### Sampler that keeps up with you
Load **WAV, MP3, FLAC, OGG, Opus, or M4A** as instruments — mono or stereo. Set root note, tuning, volume, and pan; loop forward or ping-pong; play forward or reversed; trim start and end non-destructively.

### SoundFont (SF2) instruments
Drop in any SoundFont and play it like a sampler instrument, with editable attack/decay/sustain/release, filter cutoff, and resonance.

### Built-in video → WAV converter
Screen-record something off YouTube or your favourite game, then **extract its audio to a sample right inside the file browser** — preview before you convert. Instant crate-digging.

### Sample editor
A full waveform editor to polish and abuse your samples: crop, copy/paste, normalize, fade, silence, reverse. Repitch to a BPM, or **time-stretch** with an Akai-style SOLA algorithm (yes — that jungle-break grit is on purpose). Detect transients and **slice** your breaks — trigger slices as one-shots, or just drop slice markers (saved into the WAV, compatible with M8, Blackbox, Reaper). Bake effects straight into the sample.

### Modulation
Four slots per instrument — **AHD, ADSR, and LFO** envelopes — routed to volume, pitch, filter, pan, and more. Shape every note.

### Grooves
Per-track swing and shuffle patterns. Triplets and wonky off-grid timing, confirmed.

### Effects, all the way down
Overdrive, bitcrusher, resonant filters, and 3-band EQ per instrument. **Reverb and ping-pong delay** as send channels. A master bus with **OTT** (the aggressive "soundgoodizer") or **DUST** (a lo-fi blend from Skoomabwoy that squishes your track into something warmer and grittier). Every one of these can also be printed onto a sample in the editor.

### 8 stereo tracks + a real mixer
Arrange everything across 8 stereo channels, then balance it on a proper mixer screen with per-track volume, sends, and true dBFS meters.

### Resample & bounce
Capture whatever the sequencer is playing back into a new sample — layer up drums, freeze a chord into a pad, or flatten a whole section.

### Export
Render the full song to a **stereo WAV**, or export each track as a **separate stem**.

### Make it yours
A theme editor with a full colour palette (save/load `.ptt` themes), six visualizer modes for the top bar, multiple layouts for handhelds and phones, and a deep settings screen to bend the app around your workflow.

➡️ Full, detailed list: [`docs/features.md`](docs/features.md)

---

## Under the hood

For the curious: PocketTracker runs a custom sample-accurate C++ audio engine (8-voice polyphony, all DSP native) behind a pixel-perfect 640×480 Compose UI, with a portable core kept separate from the Android layer (a Linux port is planned). Details in [`docs/technical-architecture.md`](docs/technical-architecture.md).

---

## Privacy

PocketTracker requests **no network access** — it has no internet permission at all. No accounts, no analytics, no telemetry, no ads. Everything you make stays on your device.

---

## Supported devices

Almost any Android gaming handheld or phone.

**Minimum requirements:** Android 8.0 (API 26) · 64-bit (arm64-v8a / x86_64) · ~512 MB RAM · ~50 MB storage · 640×480 screen

Tested on the **Miyoo Flip** (1 GB RAM, Android 13) and **Ayaneo Pocket Air Mini** (3 GB RAM, Android 11).

---

## Installation

### APK (recommended for most people)

An APK will be posted to [Releases](https://github.com/conanizer/pockettracker/releases) at public release.

### Build from source

**Requirements:** Android Studio (Hedgehog+), Android NDK 25.1+, CMake 3.22.1+

```bash
git clone https://github.com/conanizer/pockettracker.git
cd pockettracker
./gradlew assembleDebug          # build
./gradlew installDebug           # build + install to a connected device
```

Output APK: `app/build/outputs/apk/debug/`

---

## Documentation

| Document | Contents |
|---|---|
| [`docs/manual-en.md`](docs/manual-en.md) | Full user manual |
| [`docs/input-system.md`](docs/input-system.md) | Complete controls reference |
| [`docs/features.md`](docs/features.md) | Feature overview |
| [`docs/technical-architecture.md`](docs/technical-architecture.md) | Architecture overview |

---

## Contributing

Not yet accepting external code contributions — PocketTracker is approaching its first public release. After release:

- Bug reports → GitHub Issues
- Feature requests / questions → GitHub Discussions
- Code, docs, and example projects welcome

---

## Credits

PocketTracker is built on excellent open-source work — Oboe, DaisySP, TinySoundFont, KissFFT, dr_libs, libopus, and more, plus the [skoomaDust](https://github.com/skoomabwoy/skoomaDust) lo-fi chain by [@skoomabwoy](https://github.com/skoomabwoy). Inspired by **M8**, **LGPT**, and **LSDJ**.

Full attributions, licenses, and DSP algorithm references: [`CREDITS.md`](CREDITS.md).

---

## License

PocketTracker — free, open-source Android music tracker.
Copyright © 2025–2026 conanizer

This program is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License v3.0** as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but **without any warranty**; without even the implied warranty of merchantability or fitness for a particular purpose. See the [GNU General Public License](https://www.gnu.org/licenses/gpl-3.0.html) for details.

Full license text: [`LICENSE`](LICENSE).
