 PocketTracker

**Free, open-source Android music tracker** inspired by professional hardware trackers (M8, LSDJ) but designed to run on affordable retro gaming handhelds.

> "M8-level tracker not tied to specific hardware"

**Status:** Feature-complete v1.0 candidate | **Target:** April 2026 public release

---

## 🎯 What is PocketTracker?

A **music tracker** is a sequencer where you compose music in a grid, entering notes, instruments, and effects step-by-step. Think of it as a spreadsheet for music composition.

**Why PocketTracker?**
- ✅ **Affordable** - Run on $60 handhelds (vs $500+ M8)
- ✅ **Professional** - Sample-accurate audio engine (<0.02ms jitter)
- ✅ **Portable** - Fits in your pocket, create music anywhere
- ✅ **Open Source** - GPL v3.0, free forever

---

## 📱 Supported Devices

### Tested Devices

**Primary:**
- Miyoo Flip (1GB RAM, Android 13, GammaCoreOS)

**Secondary:**
- Ayaneo Pocket Air Mini (3GB RAM, Android 11)

**Minimum Requirements:**
- Android 8.0+ (API 26)
- ~512MB total RAM (works on 1GB Miyoo Flip!)
- 640×480 minimum resolution
- ~50MB storage

---

## ✨ Features

### Audio Engine
- ✅ Professional sample-accurate playback (<0.02ms jitter)
- ✅ 8-voice polyphony with per-track voice stealing
- ✅ 256 sample slots with automatic pitch correction
- ✅ Advanced sample playback (start/end points, reverse, loop modes: off/fwd/ping-pong)
- ✅ Real-time waveform visualization (oscilloscope)
- ✅ True stereo output with constant-power pan law
- ✅ Resonant biquad filters (LP/HP/BP) per instrument
- ✅ Modulation engine (4 slots per instrument: AHD, ADSR, LFO, DRUM, TRIG)
- ✅ Groove quantization (256 grooves, per-track assignment)

### Composition
- ✅ **Phrase editor** — 16 steps with N/V/I/FX1/FX2/FX3 columns
- ✅ **Chain editor** — 16 phrase references with per-slot transpose
- ✅ **Song editor** — 8-track arrangement, 256 rows, page navigation
- ✅ **Instrument editor** — Full parameter set (sample, ROOT, DETUNE, VOL, PAN, filters, loop, start/end)
- ✅ **Table screen** — 16-row mini-sequencer per instrument (transpose, volume, 3 FX columns)
- ✅ **Groove screen** — Step-timing patterns for swing/shuffle (256 grooves)
- ✅ **Modulation screen** — 4-slot envelope/LFO editor per instrument
- ✅ **Mixer screen** — 8 track volumes + master with true dBFS meters
- ✅ Sample-accurate playback at any tempo (20–999 BPM)

### Effects (Phrase Screen)
- ✅ **ARP/ARC** — Arpeggio with UP/DOWN/PINGPONG/RANDOM modes and speed control
- ✅ **OFF** — Sample start point offset (O00–OFF)
- ✅ **VOL** — Volume automation (V00–VFF)
- ✅ **KIL** — Kill voice immediately (K00)
- ✅ **REP** — Retrigger (single shot or volume-ramp mode)
- ✅ **PSL** — Pitch slide / portamento
- ✅ **PBN** — Pitch bend (continuous up or down)
- ✅ **PVB/PVX** — Vibrato (standard and extreme)
- ✅ **DEL** — Delay note by N ticks
- ✅ **CHA** — Probability gate (chance left / chance right)
- ✅ **RND/RNL** — Randomize FX values
- ✅ **TBL** — Override table ID for current note
- ✅ **GRV** — Assign groove pattern to track

### Table Effects
- ✅ **TIC** — Table tick rate + special modes (TIC00/FC/FE/FF)
- ✅ **HOP** — Phrase/table jump (odd time signatures: 3/4, 5/4, 7/8, etc.)
- ✅ **THO** — Table hop to row

### Workflow
- ✅ Project save/load (.ptp files, JSON format)
- ✅ File browser with WAV preview
- ✅ WAV Export — render full song to stereo WAV file
- ✅ Selection Resampling — render song selection to new instrument slot
- ✅ M8-style copy/paste (CELL / ROW / SCREEN selection modes)
- ✅ CLEAN — remove unused sequences or instruments (with confirmation)
- ✅ Smart cursor memory (remembers context between screens)
- ✅ Quick insert (A button on empty rows)
- ✅ Cross-device project transfer (MANAGE_EXTERNAL_STORAGE)

### Display & Input
- ✅ Pixel-perfect rendering at 640×480 (crisp font, zero sub-pixel gaps)
- ✅ 4 layout modes: FULL, TOUCH PORTRAIT, TOUCH LANDSCAPE, TOUCH PORTRAIT2
- ✅ Layout and scaling mode persisted across app restarts
- ✅ Auto-switch portrait ↔ landscape on device rotation
- ✅ Virtual controls with multi-touch combo support
- ✅ Physical gamepad/handheld button support (auto-detected)

---

## 🎮 Controls

### Physical Buttons (Gaming Handhelds)

**Navigation:**
- **D-PAD** — Move cursor
- **A** — Confirm, enter edit mode, quick insert
- **B** — Cancel, exit mode
- **START** — Play/stop
- **SELECT** — Context actions
- **L/R** — Navigate screens (SHIFT+DPAD for navigation map)

**Value Editing:**
- **A + UP/DOWN** — Increment/decrement value
- **A + LEFT/RIGHT** — Change octave (notes) or cycle options
- **B + LEFT/RIGHT** — Navigate tables/instruments/grooves

**Copy/Paste (M8-style):**
- **L+B** — Enter/cycle selection mode (CELL → ROW → SCREEN)
- **D-PAD (in selection)** — Expand/contract selection
- **B (in selection)** — Copy and exit
- **L+A (in selection)** — Cut (copy + delete) and exit
- **L+A (outside selection)** — Paste at cursor
- **A+B (in selection)** — Delete (no clipboard) and exit
- **L alone** — Cancel selection

**Key Repeat:**
- Hold D-PAD, A+DPAD, or B+DPAD for continuous input (400ms delay, 100ms interval)

### Touchscreen (Smartphones/Tablets)

Virtual controls automatically appear on touchscreen-only devices. Multi-touch combos (hold L + tap DPAD, hold A + tap DPAD, etc.) all work naturally.

Layout modes available:
- **TOUCH PORTRAIT** — screen above, full-width button panel below
- **TOUCH LANDSCAPE** — screen centered, button panels on left and right
- **TOUCH PORTRAIT2** — compact 4×4 grid layout

---

## 🧭 Screen Navigation

```
      0         1         2         3         4
┌─────────────────────────────────────────────────┐
│  ----     ----     SCALE    INST_POOL   ----   │ 0
│  PROJ     PROJ     GROOVE     MODS      ----   │ 1
│  SONG     CHAIN    PHRASE     INST     TABLE   │ 2  ← Main editing row
│  MIXER    MIXER    MIXER      MIXER    MIXER   │ 3
│ EFFECTS  EFFECTS  EFFECTS   EFFECTS  EFFECTS  │ 4
└─────────────────────────────────────────────────┘
```

Navigate with **L/R + DPAD** (hold L or R, move DPAD, release to jump).

---

## 🚀 Installation

### For End Users (Coming Soon)

**APK download available at release (April 2026)**

1. Download `PocketTracker-v1.0.apk`
2. Enable "Install from unknown sources" on your device
3. Install APK
4. Grant storage permissions (including "All Files Access" on Android 11+)
5. Launch and create music!

### For Developers (Build from Source)

**Prerequisites:**
- Android Studio Hedgehog or newer
- Android SDK (API 26+)
- NDK (25.1.8937393 or newer)
- CMake (3.22.1+)

**Build steps:**
```bash
# Clone repository
git clone https://github.com/yourusername/PocketTracker.git
cd PocketTracker

# Debug build
./gradlew assembleDebug

# Install to connected device
./gradlew installDebug
```

**Output:** `app/build/outputs/apk/`

---

## 📖 Quick Start Guide

**Full guide coming with release!**

Will include:
- Example project walkthrough
- Video tutorial (5–10 min)
- Controls reference card
- "Hello world" song in <5 minutes

For now, see:
- `docs/development-status.md` — Current progress
- `docs/input-system.md` — Complete controls reference

---

## 🏗️ Project Structure

```
PocketTracker/
├── app/src/main/
│   ├── java/com/example/pockettracker/
│   │   ├── core/
│   │   │   ├── audio/           # IAudioBackend, AudioEngine
│   │   │   ├── logic/           # TrackerController, PlaybackController, etc.
│   │   │   ├── resources/       # IResourceLoader
│   │   │   └── storage/         # IFileSystem, FileInfo
│   │   ├── platform/android/    # OboeAudioBackend, AndroidFileSystem, etc.
│   │   ├── *Module.kt           # Screen modules (phrase, chain, song, etc.)
│   │   ├── PixelPerfectRenderer.kt
│   │   ├── MainActivity.kt
│   │   ├── DeviceAdapter.kt
│   │   └── VirtualControls.kt
│   │
│   └── cpp/
│       ├── native-audio.cpp     # C++ audio engine (Oboe)
│       └── CMakeLists.txt
│
├── docs/                        # Documentation
│   ├── development-status.md   # Detailed current progress
│   ├── technical-architecture.md # System design
│   ├── input-system.md         # Controls reference
│   └── ...
└── README.md                   # This file
```

---

## 🎵 Philosophy & Inspiration

**Inspired by:**
- **Dirtywave M8** — Professional portable tracker ($500)
- **LSDJ** — Game Boy classic tracker
- **LGPT (Picotracker)** — PSP/handheld tracker

**Our approach:**
- Hardware-agnostic (not tied to specific device)
- Open source (community-driven development)
- Affordable (run on budget handhelds)
- Professional audio quality (sample-accurate engine)
- Architecture ready for Linux port (clean interface abstractions)

---

## 🤝 Contributing

**Not accepting contributions until MVP release**, but you can:
- ⭐ Star the repo to follow progress
- 🐛 Report bugs (after public beta)
- 💡 Suggest features (via GitHub Discussions)
- 📢 Spread the word!

**Post-MVP:** Contributions welcome (code, docs, examples, tutorials)

---

## 🙏 Credits

**Development:**
- Solo developer: Conan
- Mentor: joining post-MVP for advanced features

**Libraries & Tools:**
- [Oboe](https://github.com/google/oboe) — Low-latency audio (Apache 2.0)
- [Jetpack Compose](https://developer.android.com/jetpack/compose) — Modern UI toolkit
- [Kotlinx Serialization](https://github.com/Kotlin/kotlinx.serialization) — JSON save/load

**Inspiration:**
- Dirtywave M8 Tracker
- LSDJ (Little Sound DJ)
- LGPT (Little Game Park Tracker)
- Picotracker

**Special Thanks:**
- Claude AI (Anthropic) — Development assistant
- M8/LSDJ/LGPT communities — Inspiration and guidance

---

## 📄 License

**GNU General Public License v3.0**

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

See `LICENSE` file for full text.

---

## 📞 Contact & Community

**GitHub:** [Repository link when public]
**Discord:** [Server link after MVP]
**Reddit:** r/MusicTrackers

---

## ❓ FAQ

**Q: When will it be ready?**
A: Public release targeting April 2026.

**Q: Will it work on my device?**
A: If it runs Android 8.0+ with ~512MB RAM, give it a try!

**Q: How much will it cost?**
A: FREE forever! Open source GPL v3.0.

**Q: Can I use my own samples?**
A: Yes! Load any WAV file from device storage (8/16/24/32-bit PCM and float).

**Q: Can I export my songs to WAV?**
A: Yes! WAV export is implemented — renders the full song to stereo WAV.

**Q: Will there be a Linux version?**
A: Yes! Architecture is already prepared (IAudioBackend, IFileSystem interfaces). Planned post-MVP.

**Q: How does it compare to M8?**
A: Core workflow is similar (phrase → chain → song). Features like ADSR envelopes, LFO modulation, groove patterns, tables, and pitch effects are all working.

---

## 📊 Current Status

**All core features:** ✅ Complete
**Audio Engine:** ✅ Production-ready
**Testing & Polish:** 🚧 In progress
**Documentation:** 🚧 In progress

**See `docs/development-status.md` for detailed progress**

---

**Version:** 1.0-rc
**Last Updated:** 2026-03-13
**License:** GPL v3.0
