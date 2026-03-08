 PocketTracker

**Free, open-source Android music tracker** inspired by professional hardware trackers (M8, LSDJ) but designed to run on affordable retro gaming handhelds.

> "M8-level tracker not tied to specific hardware"

**Status:** MVP ~95% complete | **Target:** Late February 2025 public beta

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
- Ayaneo Pocket Air Mini (3GB RAM, Android 11) - arriving soon

**Minimum Requirements:**
- Android 8.0+ (API 26)
- ~512MB total RAM (works on 1GB Miyoo Flip!)
- 640×480 minimum resolution
- ~50MB storage

---

## ✨ Features

### Working Now ✅

**Audio Engine:**
- ✅ Professional sample-accurate playback (<0.02ms jitter)
- ✅ 8-voice polyphony with per-track voice stealing
- ✅ 256 sample slots with automatic pitch correction
- ✅ Advanced sample playback (start/end points, reverse, looping)
- ✅ Real-time waveform visualization (oscilloscope)

**Composition:**
- ✅ Phrase editor (16 steps with N/V/I/FX columns)
- ✅ Chain editor (16 phrase refs with transpose)
- ✅ Song editor (8-track arrangement)
- ✅ Instrument editor (sample loading, ROOT, DETUNE, filters)
- ✅ Sample-accurate playback at any tempo (20-999 BPM)

**Workflow:**
- ✅ Project save/load (.ptp files)
- ✅ File browser with WAV preview
- ✅ Smart cursor memory (remembers context between screens)
- ✅ Quick insert (A button on empty rows)
- ✅ Generic input system (buttons OR touchscreen)

### In Progress (MVP) 🚧

**Effects System:**
- 🚧 TOP-5 effects in phrase screen:
  - Arpeggio (Axx) - Note patterns
  - Offset (Oxx) - Sample start point
  - Volume (Vxx) - Volume automation
  - Kill (K00) - Stop immediately
  - Repeat (Rxx) - Retrigger

**Copy/Paste System:**
- 🚧 M8-style selection mode
- 🚧 Copy/paste phrase steps
- 🚧 Copy/paste between phrases
- 🚧 Clipboard indicator
- 🚧 Cut/delete selections

**Architecture:**
- 🚧 Refactoring for Linux port
- 🚧 Separate controllers (portable code)
- 🚧 Platform abstraction layers

### Planned (Post-MVP) 📋

**Early Post-MVP:**
- Table screen (effects for instruments)
- Advanced copy/paste (instrument settings)
- Linux port (desktop/laptop use)
- Braids synthesizers (Mutable Instruments)
- Remaining effects (pitch, pan, filter automation)

**Later:**
- Undo/redo
- WAV export
- Mixer screen
- Themes/color schemes
- MIDI export

---

## 🎮 Controls

### Physical Buttons (Gaming Handhelds)

**Navigation:**
- **D-PAD** - Move cursor
- **A** - Confirm, enter edit mode
- **B** - Cancel, exit mode
- **START** - Play/stop
- **SELECT** - Context actions
- **L/R** - Navigate screens (SHIFT+DPAD for navigation map)

**Value Editing:**
- **A + UP/DOWN** - Increment/decrement value
- **A + LEFT/RIGHT** - Change octave (notes) or cycle options

**Copy/Paste (M8-style):**
- **SELECT+B** - Enter selection mode
- **D-PAD (in selection)** - Expand/contract selection
- **B (in selection)** - Copy and exit
- **SELECT+A** - Paste at cursor
- **A+B** - Cut selection

### Touchscreen (Smartphones/Tablets)

Virtual controls automatically appear on touchscreen-only devices:
- D-pad (left side)
- A/B buttons (right side)
- START/SELECT buttons (top)
- L/R buttons (top corners)

---

## 🚀 Installation

### For End Users (Coming Soon)

**APK download will be available at MVP release (Late February 2025)**

1. Download `PocketTracker-v1.0.apk`
2. Enable "Install from unknown sources" on your device
3. Install APK
4. Grant storage permissions
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

# Open in Android Studio
# Or build via command line:

# Debug build
./gradlew assembleDebug

# Release build
./gradlew assembleRelease

# Install to connected device
./gradlew installDebug
```

**Output:** `app/build/outputs/apk/`

---

## 📖 Quick Start Guide

**Coming with MVP release!**

Will include:
- Example project walkthrough
- Video tutorial (5-10 min)
- Controls reference card
- "Hello world" song in <5 minutes

For now, see:
- `DEVELOPMENT_STATUS.md` - Current progress
- `MVP_ROADMAP.md` - Feature roadmap
- `INPUT_COMBINATIONS.md` - Complete controls reference

---

## 🏗️ Project Structure

```
PocketTracker/
├── app/src/main/
│   ├── kotlin/
│   │   ├── TrackerData.kt           # Data models (portable!)
│   │   ├── TrackerAudioEngine.kt    # JNI bridge
│   │   ├── MainActivity.kt          # Main activity
│   │   ├── FileManager.kt           # File I/O
│   │   ├── *Module.kt              # Screen modules
│   │   └── PixelPerfectRenderer.kt  # Custom rendering
│   │
│   └── cpp/
│       ├── native-audio.cpp         # C++ audio engine
│       └── CMakeLists.txt
│
├── docs/
│   ├── DEVELOPMENT_STATUS.md       # Current progress
│   ├── MVP_ROADMAP.md              # Feature roadmap
│   ├── REFACTORING_ROADMAP.md      # Architecture plan
│   ├── TECHNICAL_ARCHITECTURE.md   # System design
│   └── CLAUDE.md                   # AI assistant guide
│
└── README.md                       # This file
```

---

## 🎵 Philosophy & Inspiration

**Inspired by:**
- **Dirtywave M8** - Professional portable tracker ($500)
- **LSDJ** - Game Boy classic tracker
- **Little Sound DJ (LGPT)** - PSP tracker

**Our approach:**
- Hardware-agnostic (not tied to specific device)
- Open source (community-driven development)
- Affordable (run on budget handhelds)
- Professional audio quality (sample-accurate engine)
- Eventually multi-platform (Linux port planned)

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
- Solo developer: Conan (learning as I go!)
- Mentor: [Name] (joining post-MVP for advanced features)

**Libraries & Tools:**
- [Oboe](https://github.com/google/oboe) - Low-latency audio (Apache 2.0)
- [Jetpack Compose](https://developer.android.com/jetpack/compose) - Modern UI toolkit
- [Kotlinx Serialization](https://github.com/Kotlin/kotlinx.serialization) - JSON save/load

**Inspiration:**
- Dirtywave M8 Tracker
- LSDJ (Little Sound DJ)
- LGPT (Little Game Park Tracker)
- Picotracker

**Special Thanks:**
- Claude AI (Anthropic) - Development assistant
- DeepSeek AI - Architecture review
- M8/LSDJ/LGPT communities - Inspiration and guidance

---

## 📄 License

**GNU General Public License v3.0**

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

See `LICENSE` file for full text.

**Why GPL?**
- Ensures project stays free forever
- Derivative works must also be open source
- Community benefits from improvements

---

## 📞 Contact & Community

**GitHub:** [Repository link when public]
**Discord:** [Server link after MVP]
**Reddit:** r/MusicTrackers (discuss trackers in general)

**Developer:**
- Prefers Russian for communication
- Available for questions after MVP release
- Looking for beta testers (February 2025)

---

## 🎯 Roadmap Summary

**January 2025:**
- ✅ Phase A complete (audio, UI, playback)
- 🚧 Architecture refactoring (2 weeks)

**February 2025:**
- 🚧 Effects system (1-2 weeks)
- 🚧 Copy/paste system (1 week)
- 🚧 Testing & polish (1 week)
- 🚧 Documentation & video
- 🎉 **Public Beta Release** (Late Feb)

**March 2025+:**
- Mentor joins for advanced features
- Table screen
- Linux port begins
- Braids synthesizers
- Community feedback integration

---

## ❓ FAQ

**Q: When will it be ready?**
A: Public beta late February 2025 (8-10 weeks from now)

**Q: Will it work on my device?**
A: Probably! If it runs Android 8.0+ and has 1GB+ RAM, give it a try.

**Q: How much will it cost?**
A: FREE forever! Open source GPL v3.0.

**Q: Can I use my own samples?**
A: Yes! Load any WAV file from your device storage.

**Q: Will there be a Linux version?**
A: Yes! Planned for post-MVP (March-April 2025)

**Q: Can I export my songs to WAV?**
A: Not yet - planned for post-MVP.

**Q: How does it compare to M8?**
A: MVP will have ~30% of M8 features, focusing on core workflow. Post-MVP will add more.

**Q: Is this better than LGPT?**
A: Different goals - we prioritize audio quality and modern architecture. LGPT is more feature-complete currently.

**Q: Can I help test?**
A: Beta testing starts late February! Join our Discord (link coming).

---

## 📊 Current Status

**Completion:** ~95% to MVP
**Audio Engine:** ✅ Production-ready
**UI Screens:** ✅ Complete
**Remaining:** Effects, Copy/Paste, Polish

**See `DEVELOPMENT_STATUS.md` for detailed progress**

---

## 🚀 Let's Make Music!

PocketTracker is built by a musician frustrated with expensive hardware trackers. The goal is simple:

> **Make professional tracker-style composition accessible to everyone with a budget Android handheld.**

If you're excited about this project:
- ⭐ Star the repo
- 📢 Tell other musicians
- 🎵 Get ready to create!

---

**Version:** 0.9 (Pre-MVP)
**Last Updated:** 2025-01-02
**License:** GPL v3.0
