# PocketTracker

**A free, open-source music tracker for Android.**

PocketTracker turns an Android device you already own — your phone, or an inexpensive gaming handheld — into a complete, self-contained music studio. Sketch an idea on the bus, sample something on the couch, and finish a whole track without a desktop or any extra gear.

It carries on the spirit of pocket trackers like LSDJ and the Dirtywave M8, with one difference: it's free, open-source, and runs on hardware you've probably already got. The idea is simple — put a real music-making tool in anyone's pocket, with no cost, no account, and nothing standing between you and the first note.

> **Note:** This project was developed with AI assistance. If that bothers you, this project isn't for you.

**Status:** 0.9.0 — first public release (pre-1.0)
**License:** [GPL-3.0-or-later](LICENSE)

---

## Features

### A sampler that eats anything
Load WAV, MP3, M4A, FLAC, OGG or Opus straight in as instruments — most trackers stop at WAV. You can even pull audio out of a video: screen-record a track or some gameplay, then extract its sound to a sample without leaving the app or hunting for a converter. SoundFonts (SF2) load and play like any other instrument too.

### Chop, stretch & slice
A full sample editor lives on the device. Repitch a sample to your project's tempo, or time-stretch it to fit without changing its pitch — handy for lining up breaks and loops. Detect transients to slice a break into individual hits, trigger them across the keyboard, and bake your edits and effects right into the sample.

### Plays like a handheld
PocketTracker is built around physical buttons and combos — you can make an entire track without touching the screen, the way LSDJ feels on a Game Boy. On a phone or any touchscreen, the same workflow runs on on-screen controls.

### Effects with character
Shape each track with overdrive, bitcrushing, filters and EQ, plus reverb and ping-pong delay on sends. On the master, reach for OTT — the aggressive "soundgoodizer" finish — or DUST, a lo-fi chain from [@skoomabwoy](https://github.com/skoomabwoy) that squashes your mix into something warmer and dirtier.

### Eight stereo tracks, a real mixer & stems
Arrange your song across eight stereo tracks, balance everything on a proper mixer with sends and true dBFS meters, then export the finished mix as a WAV — or as separate stems to carry into another DAW.

### Resample on the fly
Record whatever the sequencer is playing straight back into a new sample — layer up drums, freeze a chord into a pad, or flatten a whole section to build on.

### Make it yours
Recolour every element of the interface with the built-in theme editor (Amiga-style look included), choose from six top-bar visualizers, and adapt the layout to your device.

➡️ Full, detailed feature list: [`docs/features.md`](docs/features.md)

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
