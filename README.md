# PocketTracker

<p align="center">
  <img src="docs/images/logo-dark.png" alt="PocketTracker" height="240">
  &nbsp;&nbsp;
  <img src="docs/images/screenshot.png" alt="PocketTracker screenshot" height="240">
</p>

PocketTracker is a music tracker for Android-based devices — a retro gaming handheld or a smartphone will work. It carries on the spirit of trackers like LSDJ and LGPT. It's free, open-source, and runs on hardware you've probably already got — the goal is to put a capable music-making tool in anyone's pocket.

> **Note:** This project was developed with AI assistance. If that bothers you, this project isn't for you.

**Status:** 0.9.2 — public beta

**License:** [GPL-3.0](LICENSE)

---

## Features

### Instruments
Two instrument types: a **sampler** that loads WAV, MP3, M4A, FLAC, OGG and Opus files, and a **SoundFont** player for SF2 files.

### Sampling from video
Just screen record stuff from YouTube or your favourite video games and sample it with the built-in video-to-WAV converter!

### Sample editor
Manipulate the waveform, add effects, repitch, STRETCH (can't wait to hear your jungle tunes), slice (destructively or just add slice markers to your sample).

### Controls
PocketTracker is made for gaming handhelds with physical buttons. For phones and other touchscreen devices, there's an on-screen control layout.

### Effects
Overdrive, bitcrusher, filters, EQs, reverb and delay (as send channels), and a master bus with OTT (aggressive soundgoodizer) or DUST (a special blend from Skoomabwoy to squish your tracks and make them more lofi-ish). All of them can be applied to your samples in the sample editor!

### Mixing & export
Arrange a song across eight stereo tracks and balance it on a mixer with per-track sends and true dBFS meters. Export the finished mix as a WAV, or export each track as a separate stem.

### Resampling
Record selected sequence part into a new sample — for layering drums, freezing a chord into a pad, or flattening a section to build on.

### Appearance
There are a few options to customize the app interface. The top bar has six visualizer modes that vary from a ProTracker2 look to a Pioneer-style stereo tower. There are also interface color themes and a theme editor to make your own. As a bonus, phones come with an Amiga-inspired touchscreen button skin.

➡️ Full feature list: [`docs/features.md`](docs/features.md)

---

## Supported devices

Almost any Android gaming handheld or phone.

**Minimum requirements:** Android 8.0 (API 26) · 64-bit (arm64-v8a / x86_64) · ~512 MB RAM · ~50 MB storage · 640×480 screen

Tested on the **Miyoo Flip** (1 GB RAM, GammaOSCore Android 13), **Ayaneo Pocket Air Mini** (3 GB RAM, Android 11), **Fairphone 6** (8 GB RAM, /e/os v3.0.4 Android 15) and **Xiaomi 12T Pro** (8 GB RAM, LineageOS Android 16)

---

## Installation

**One-click (recommended):** install [Obtainium](https://github.com/ImranR98/Obtainium), then tap the badge below on your device. PocketTracker is added straight from GitHub Releases and Obtainium keeps it up to date automatically.

[<img src="https://raw.githubusercontent.com/ImranR98/Obtainium/main/assets/graphics/badge_obtainium.png" alt="Get it on Obtainium" height="54">](https://apps.obtainium.page/redirect?r=obtainium://add/https://github.com/conanizer/pockettracker)

If the badge doesn't open Obtainium, add this URL as an app source inside Obtainium instead:

```
https://github.com/conanizer/pockettracker
```

**Manual:** download the latest APK from [Releases](https://github.com/conanizer/pockettracker/releases) and open it on your device (allow "install from unknown sources" if asked).

---

## Documentation

| Document | Contents |
|---|---|
| [`docs/quick-start-guide.md`](docs/quick-start-guide.md) | Quick start — from install to your first beat |
| [`docs/manual-en.md`](docs/manual-en.md) | Full user manual |
| [`docs/input-system.md`](docs/input-system.md) | Complete controls reference |
| [`docs/features.md`](docs/features.md) | Feature overview |
| [`docs/technical-architecture.md`](docs/technical-architecture.md) | Architecture overview |

---

## Building from source

A standard Android + NDK project — the native audio engine builds via CMake as part of the normal Gradle build:

1. Clone the repo and open it in a recent **Android Studio**.
2. Let Gradle sync; it provisions the SDK and the pinned **NDK `27.0.12077973`**.
3. Run the **app** configuration on a device/emulator, or build an APK with `./gradlew assembleDebug`.

Debug builds need no signing setup — release builds fall back to the debug key when `keystore.properties` is absent.

---

## Contributing

- Bug reports → GitHub Issues
- Feature requests / questions → GitHub Discussions
- Or reach me on [Discord](https://discord.gg/Va72sWDmVA) for any kind of feedback

---

## Credits

PocketTracker is built on excellent open-source work — Oboe, DaisySP, TinySoundFont, KissFFT, dr_libs, libopus, and more. Inspired by **M8**, **LGPT**, and **LSDJ**.

Full attributions, licenses, and DSP algorithm references: [`CREDITS.md`](CREDITS.md).

---

## License

PocketTracker is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License v3.0 or later** as published by the Free Software Foundation.

It is distributed in the hope that it will be useful, but **without any warranty** — without even the implied warranty of merchantability or fitness for a particular purpose. See the [GNU General Public License](https://www.gnu.org/licenses/gpl-3.0.html) for details.

Full license text: [`LICENSE`](LICENSE).
