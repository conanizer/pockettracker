# PocketTracker

**A free, open-source music tracker for Android.**

PocketTracker turns an Android device you already own — your phone, or an inexpensive gaming handheld — into a self-contained music studio. Write, sample, mix and export complete tracks on the device, with no desktop or extra gear required.

It carries on the spirit of pocket trackers like LSDJ and the Dirtywave M8. It's free, open-source, and runs on hardware you've probably already got — the goal is to put a capable music-making tool in anyone's pocket, with no cost and no account.

> **Note:** This project was developed with AI assistance. If that bothers you, this project isn't for you.

**Status:** 0.9.0 — first public release (pre-1.0)
**License:** [GPL-3.0-or-later](LICENSE)

---

## Features

### Instruments
Two instrument types: a **sampler** that loads WAV, MP3, M4A, FLAC, OGG and Opus files, and a **SoundFont** player for SF2 files.

### Sampling from video
Create a sample from a video: extract its audio to a WAV in the file browser — with a preview before you convert — then load it into the sampler.

### Sample editor
Edit samples on the device: trim, fade, normalise, reverse and more. Match a sample to a tempo, either by repitching it or by time-stretching it without changing pitch. Detect transients to slice a break into separate hits, and bake your edits and effects into the sample.

### Controls
PocketTracker is made for gaming handhelds with physical buttons — the whole app runs on buttons and button combos. For phones and other touchscreen devices, there's an on-screen control layout.

### Effects
Per-instrument overdrive, bitcrushing, filtering and a 3-band EQ. Reverb and delay are available as send effects. The master bus offers OTT (a multiband compressor) or DUST, a lo-fi chain by [@skoomabwoy](https://github.com/skoomabwoy), each with a wet/dry control. Many of these can also be baked into a sample in the editor.

### Tracks, mixing & export
Arrange a song across eight stereo tracks and balance it on a mixer with per-track sends and true dBFS meters. Export the finished mix as a WAV, or export each track as a separate stem.

### Resampling
Record whatever the sequencer is playing into a new sample — for layering drums, freezing a chord into a pad, or flattening a section to build on.

### Appearance
Recolour the interface with the theme editor and save palettes as `.ptt` files; several themes are built in. The top bar has six visualizer modes. The phone portrait layout also comes with an Amiga-style skin.

➡️ Full feature list: [`docs/features.md`](docs/features.md)

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

PocketTracker is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License v3.0 or later** as published by the Free Software Foundation.

It is distributed in the hope that it will be useful, but **without any warranty** — without even the implied warranty of merchantability or fitness for a particular purpose. See the [GNU General Public License](https://www.gnu.org/licenses/gpl-3.0.html) for details.

Full license text: [`LICENSE`](LICENSE).
