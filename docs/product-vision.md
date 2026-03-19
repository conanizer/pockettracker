# PocketTracker - Product Vision

## Document Purpose
This document defines **WHAT** PocketTracker is and **WHY** it exists. It serves as the north star for all product decisions and should be consulted whenever there's a question about scope, priorities, or direction.

**Last Updated:** 2025-01-01  
**Version:** 1.0  
**Author:** Conan (Product Owner)

---

## The Origin Story

### The Problem That Started It All

"I saw M8 and their open-source software, wanted to buy a handheld to run M8 on it, but was disappointed because it requires a Teensy microcontroller. The only alternative was LGPT. I tried it on PSP - the functionality seemed weak."

**Core frustration:** 
- M8 has the workflow and features I want, but it's tied to specific hardware
- LGPT is portable but lacks the synthesizers and sophisticated sample handling
- The ideal tracker (M8-level features + hardware-agnostic) doesn't exist

### The Emotional Drive

**"I want a tracker with M8 functionality that's not tied to specific hardware, limited only by tracker functionality itself."**

This isn't just about making another tracker - it's about **freedom of choice**:
- Freedom to use the device you already own
- Freedom to run the same software on different platforms
- Freedom from expensive, hard-to-find hardware

---

## Target User

### Primary User (The Bull's Eye 🎯)

**Profile:**
> A musician who wants M8 or Polyend Mini, and is considering buying a retro handheld, but didn't have enough reasons yet. A good tracker becomes the tipping point for the purchase.

**Characteristics:**
- Already interested in tracker workflow (not a complete beginner)
- Budget-conscious but willing to invest in right tools
- Values portability and "dawless" music production
- Attracted to retro aesthetics and chiptune/lo-fi genres
- Probably already owns or wants to own: Miyoo Mini, Anbernic RG353V, or similar

**User Journey:**
1. "I've been watching M8 videos on YouTube, it looks amazing"
2. "But $500+ for M8 is too much, and it's always out of stock"
3. "I've been thinking about getting a retro gaming handheld anyway..."
4. "Wait, PocketTracker gives me M8-like features on a $60 device?"
5. **PURCHASE DECISION** ← This is where PocketTracker tips the scale

### Secondary Users

1. **Tracker Musicians** - Already use LSDJ/LGPT/M8, want portable option
2. **Chiptune Creators** - Making music in Game Boy/NES/C64 style
3. **Electronic Music Hobbyists** - Want to sketch ideas on the go
4. **Budget-Conscious Students** - Can't afford professional DAWs

### NOT For (Important to Define)

- ❌ Professional DAW users needing VST plugins and complex mixing
- ❌ People who prefer piano roll over tracker interface
- ❌ iOS users (Android-first, maybe Linux later, no iOS plans)
- ❌ People not interested in tracker music workflow
- ❌ Live instrument musicians (no input sampling in MVP)

---

## Success Criteria

### What Does "Success" Look Like?

**Primary Success Indicator:**
> The project is actively discussed in tracker musician communities, and YouTube videos are being recorded showing how to use it.

**Specific Metrics:**
- 1000+ downloads within 6 months of MVP release
- Active discussion threads on r/tracker, r/chiptunes, M8 Discord
- At least 5 YouTube tutorial/demo videos from community members
- At least one person completes a full album/EP using PocketTracker

**Personal Success (Developer Perspective):**
- Learned Kotlin/Android development through real project
- Created something genuinely useful that people want to use
- Maintained work-life balance (project should be sustainable, not burn out)

---

## Product Philosophy

### What PocketTracker IS

**Foundation Reference:** LGPT
> LGPT represents the MVP baseline - core tracker functionality that works reliably.

**Ultimate Goal:** M8 (and beyond)
> M8 is where the project should arrive ideally, and even surpass in some areas.

**Core Principles:**
1. **Hardware Agnostic** - Run on any Android device, eventually Linux
2. **Sample-Accurate Timing** - Professional-grade audio precision
3. **Tracker Workflow** - Stay true to tracker paradigm (no piano roll hybrid)
4. **Open Source** - GPL license, community can contribute
5. **Performance First** - Work smoothly on budget devices (no bloat)

### What Makes PocketTracker Different

**Unique Value Propositions:**

✅ **FREE and open-source** (GPL license)  
✅ **Optimized for 4:3 gaming handhelds** with auto-fit for other ratios  
✅ **Physical button support** (D-pad, face buttons, shoulders - no touch required)  
✅ **Touchscreen fallback** for smartphones without physical controls  
✅ **Lightweight** (works on Android 8.0+, low-end hardware)  
✅ **Sample-accurate timing** (no drift, professional precision)  
✅ **First solid LSDJ-type tracker for Android**

**What We Learn From Competitors:**

From **M8/LGPT/PicoTracker:**
- Basic LSDJ-type tracker concept (phrases, chains, patterns - proven workflow) ✅
- Sample-based playback (WAV files, pitch shifting - industry standard) ✅
- File formats (JSON projects, standard WAV - compatibility) ✅

**What We Do Better:**

vs **M8:**
- ✅ Not tied to expensive Teensy hardware ($500+ vs free software on $60 handheld)
- ✅ Runs on devices you already own or can easily buy
- ✅ Open source - community can add features

vs **LGPT:**
- ✅ Modern synthesizers (Braids integration planned)
- ✅ Better sample handling (start/end points, reverse, loop modes)
- ✅ More sophisticated effects system
- ✅ Native Android support (LGPT only has old PSP/Linux builds)

---

## Competitive Landscape

### Direct Competitors

| Product | Platform | Price | Pros | Cons |
|---------|----------|-------|------|------|
| **M8 Tracker** | Teensy hardware | $500+ | Amazing features, active dev | Expensive, hardware-locked, limited availability |
| **picoTracker** | DIY kit / Advance | $75-$350 | Affordable DIY option | DIY build required, or expensive pre-built, limited features |
| **LGPT** | Linux/PSP/Win32 | Free | Free, proven workflow | No Android, weak synths, basic sample handling |
| **SunVox** | Android/iOS/Desktop | Free | Modular synth, powerful | Steeper learning curve, not tracker-focused |
| **Nanoloop** | iOS/Android | $4-15 | Simple, fun | Limited features, more like a toy |

### Market Positioning

```
           High Features
                 │
        M8       │     PocketTracker (goal)
                 │
     ────────────┼────────────────────
                 │
      LGPT       │     Nanoloop
                 │
           Low Features
```

**PocketTracker Goal:** Upper-right quadrant (high features + accessible platform)

---

## MVP Scope Definition

### Core MVP Features (Cannot Remove Without Losing Identity)

#### 1. Step Sequencer
- 16-step phrase editing
- Note input (C-0 to B-9, 10 octaves)
- Volume per step (0-255)
- Instrument assignment per step
- Visual playback cursor

#### 2. Sample Playback
- Load WAV files (mono/stereo, any sample rate)
- 256 instrument slots
- Pitch shifting (chromatic playback)
- Volume control per instrument
- Sample preview before loading
- **Start/end points** ✅ (already implemented)
- **Loop modes** (forward/ping-pong) ✅ (already implemented)
- **Reverse playback** ✅ (already implemented)

#### 3. Hierarchical Arrangement
- **Phrases:** 16-step patterns (256 slots)
- **Chains:** Sequence of phrases with transpose (256 slots)
- **Song:** 8 tracks playing chains simultaneously
- Real-time playback at any tempo (20-999 BPM)

#### 4. Effects System ⚠️ (MVP CRITICAL!)
**TOP-5 Essential Effects:**
1. **Arpeggio (Axx)** - Note pattern automation
2. **Offset (Oxx)** - Sample start point automation
3. **Volume (Vxx)** - Volume automation
4. **Kill (K00)** - Stop sample immediately
5. **Repeat (Rxx)** - Retrigger sample within step

**Why these 5?**
> Most effects automate existing parameters. Once we implement one automation effect, the rest follow the same pattern. These 5 provide maximum creative expression with minimal code duplication.

**Post-MVP Effects (Phase 2):**
- Pitch (Pxx) - Pitch slide up/down
- Pan (Nxx) - Stereo positioning
- Filter (Fxx) - Cutoff automation
- Crush (Cxx) - Bitcrusher
- Drive (Dxx) - Distortion
- Vibrato (Vxx) - Pitch wobble

#### 5. Save/Load Projects
- `.ptp` file format (JSON-based)
- Stores all phrases, chains, songs, instruments
- Default location: `/Documents/PocketTracker/Projects/`

#### 6. Controls
- D-pad navigation (up/down/left/right)
- A/B buttons (confirm/cancel, value editing)
- Start button (play/stop)
- Select button (quick actions)
- L/R shoulders (modifiers, navigation)
- Works with touchscreen OR handheld buttons

#### 7. File Management ✅ (MVP REQUIRED!)
- Browse filesystem
- Create folders
- Sort by name/date/size
- Navigate hierarchies
- Delete/rename files
- WAV sample preview

**Note:** Simple file picker would NOT be sufficient - musicians need proper file organization for large sample libraries.

### 8. Copy/Paste System

**M8-style workflow:**
- Selection mode (L+B to enter)
- Visual selection highlighting
- Copy selection (B in selection mode)
- Paste at cursor (L+A)
- Cut selection (L+A in selection mode)
- Clipboard indicator in header row

**Scope:**
- Copy/paste phrase steps
- Copy/paste between different phrases
- Copy/paste chain rows
- Copy/paste entire chains
---

## Post-MVP Features (Planned Phases)

### Phase 1 (Months 1-2)

**With Mentor:**
- Table screen (effects for instruments)
- Advanced copy/paste features
- Braids synthesizers integration (Android)
- Remaining effects

### Post-MVP Phase 2 (Months 3-4)

**Linux Port:**
- GTK or Qt UI layer
- ALSA audio backend
- Linux file system implementation
- Braids already integrated from Phase 1!

### Phase 2: Extended Effects System
- Remaining effects (Pitch, Pan, Filter, Crush, Drive, Vibrato)
- Effect tables for complex automation
- ADSR envelopes (Mods screen)

### Phase 3: Advanced Instruments
- Braids synthesizer integration (with mentor's help)
- Per-instrument ADSR
- Advanced filter types (LP/HP/BP with resonance)
- Pan control per instrument

### Phase 4: Workflow Enhancements
- Copy/paste (phrases, chains, patterns)
- Undo/redo
- Pattern selection
- Clone instrument
- Groove quantization

### Phase 5: Export & Sharing
- Render to WAV file
- Export individual tracks
- MIDI export (maybe?)

### Phase 6: Linux Port 🐧
- Shared C++ audio core
- GTK/Qt/SDL2 UI
- ALSA/PulseAudio/JACK audio backend
- (See REFACTORING_ROADMAP.md for technical details)

### Phase 7: Polish
- Mixer screen (visual faders)
- Alternative visualizers (EQ spectrum, spectrogram, etc.)
- Themes/color schemes
- Key bindings customization

---

## Definition of Done (MVP Complete)

MVP is considered **DONE** when ALL of the following are true:

### Core Functionality ✅
- [x] User can create a 16-step phrase with notes (any pitch, any volume)
- [x] User can load custom WAV samples from device storage
- [x] User can assign instruments to notes
- [x] User can chain 4+ phrases together with transpose
- [x] User can create a song with 2+ chains on different tracks
- [x] User can play back their song at any tempo (20-999 BPM)
- [x] Playback is sample-accurate (no drift, no timing jitter)
- [ ] **User can apply effects (Arpeggio, Offset, Volume, Kill, Repeat)** ⚠️

### File Management ✅
- [x] User can save project to `.ptp` file
- [x] User can load previously saved project
- [x] User can browse folders and navigate filesystem
- [x] User can organize samples in subfolders

### Performance ⚠️
- [x] Audio latency is under 50ms (acceptable for live input) - tested on real device
- [x] No audio glitches or dropouts during playback
- [x] Works on Android 8.0+ (API 26+)
- [x] Works on 480p handheld screens (640×480 minimum)
- [ ] App runs at stable 60fps on target hardware - needs optimization

### Controls ✅
- [x] All D-pad directions work (cursor navigation)
- [x] A/B buttons work (confirm/cancel, value editing)
- [x] Start button works (play/stop toggle)
- [x] L/R shoulders work (screen navigation, modifiers)
- [x] Virtual controls work on touchscreen devices

### Usability ⚠️
- [ ] User can complete "hello world" song in under 5 minutes
- [ ] Navigation between screens is intuitive (no getting lost)
- [x] Status messages explain what happened (save success/fail, etc.)
- [ ] App doesn't crash on common user errors - needs testing

### Documentation 📝
- [ ] README explains how to install and use
- [ ] Example project included (demo song)
- [ ] Controls documented (button mappings)
- [ ] Video tutorial (created by developer)

---

## Development Timeline

### Current Status (January 2025)
- **Phase A: UI & Core Systems** - 95% complete ✅
- **Audio Engine: Sample-accurate queue** - 100% complete ✅
- **Physical device testing** - In progress ✅
- **Code refactoring for Linux port** - Not started ⚠️

### Target: MVP by February 2025

**Realistic Assessment:**
> "I think feature-complete MVP, but maybe I'm too optimistic. We're moving quite fast so far."

**Current Pace:** 2-4 hours per day, but planning to slow down for work-life balance

**Remaining MVP Work:**
1. **Effects System** (1-2 weeks) - BIG task
   - Effect command parser (FX1/FX2/FX3 columns)
   - Implement TOP-5 effects in audio engine
   - Table screen for automation (optional for MVP?)
2. **Architecture Refactoring** (1-2 weeks) - CRITICAL for Linux port
   - See REFACTORING_ROADMAP.md for details
3. **Testing & Polish** (1 week)
   - Usability testing ("hello world" song in 5 min)
   - Bug fixes
   - Example project creation
4. **Documentation & Tutorial** (3-5 days)
   - README finalization
   - Video tutorial recording

**Adjusted Timeline:**
- **Late February:** Feature-complete MVP with effects
- **Early March:** Refactoring complete, ready for Linux port prep
- **Mid March:** Public beta release with documentation and tutorial

**Post-MVP:** Mentor joins to help with Braids integration and advanced features

---

## Community Strategy

### Launch Plan

**Phase 1: Soft Launch (MVP release)**
- Post on Reddit: r/tracker, r/chiptunes, r/SBCGaming
- Post on Discord: M8 community, tracker communities
- GitHub release with clear README
- No marketing push yet - let early adopters test

**Phase 2: Content Creation (1-2 months after MVP)**
- **Tutorial video series** (created by developer):
  1. "Getting Started with PocketTracker"
  2. "Making Your First Beat"
  3. "Advanced Techniques: Chains and Effects"
- Demo songs showcasing features
- "Making of" videos showing workflow

**Phase 3: Community Growth**
- Monitor feedback and feature requests
- Encourage community contributions (GPL license)
- Highlight user-created content

**Where to Gather Community:**
- GitHub Discussions (primary)
- Discord server (maybe, if demand exists)
- Reddit r/pockettracker (if community grows)

**Success Indicator:**
> When community members start creating their own tutorial videos WITHOUT being asked.

---

## Design References & Vibe

### Functionality References
- **LGPT** - MVP reference, workflow baseline
- **M8 Tracker** - Ultimate feature goal, oscilloscope module, Braids synths
- **Polyend Tracker** - Inspiration for advanced features (chance, random, timestretch)

### Visual References
- **M8 Tracker UI** - Clean pixel art, high contrast, readable fonts
- **PICO-8** - 5×5 bitmap font aesthetic (already using!)
- **Game Boy** - Limited palette, chunky pixels, retro feel
- **Amiga/DOS trackers** - Functional, information-dense layouts

### Project Vibe
- **Nostalgic** - Feels like classic tracker from the '90s
- **Immediate** - No loading screens, instant response
- **Tactile** - Physical buttons feel responsive and satisfying
- **"Game-like"** - Music creation should be fun and playful
- **Focused** - Minimal distractions, just the music

---

## Constraints & Non-Goals

### Technical Constraints
- Target: Budget Android handhelds (640×480 minimum)
- Must work on Android 8.0+ (can't use newest APIs)
- Audio latency target: <50ms (acceptable on budget hardware)
- No cloud features (local-only for MVP)
- 64-bit architectures only (arm64-v8a, x86_64)

### Scope Constraints (What We WON'T Do)
- ❌ No iOS version (Android-first, Linux later)
- ❌ No online collaboration (local-only)
- ❌ No VST plugin support (not a DAW replacement)
- ❌ No video export / visualization rendering
- ❌ No real-time audio input sampling (file import instead)
- ❌ No multi-touch gestures (button-focused interface)

### Why These Constraints?
> "I've learned to prioritize compatibility with target hardware over complex functionality that only works on premium devices. Pragmatic decisions like abandoning internal audio recording in favor of file import keep the project achievable."

---

## Risk Management & Mitigation

### What Could Kill This Project?

**High-Risk Items:**
1. **Developer burnout** 
   - Mitigation: Set sustainable pace (2-4h/day), take breaks, work-life balance
2. **Scope creep** 
   - Mitigation: Strict MVP definition, say NO to non-essential features
3. **Audio quality issues on cheap devices** 
   - Mitigation: Already tested on real hardware, Oboe working well ✅
4. **Architecture becomes unmaintainable** 
   - Mitigation: Refactoring plan before adding more features (see REFACTORING_ROADMAP.md)

**Medium-Risk Items:**
1. **Linux port becomes too hard** 
   - Mitigation: Proper abstraction layers NOW, mentor helps later
2. **Community doesn't adopt it** 
   - Mitigation: YouTube tutorials, active engagement, listen to feedback
3. **Performance issues** 
   - Mitigation: Profile and optimize, target 60fps minimum

---

## Conclusion

PocketTracker exists to solve a real problem: **musicians want M8-level tracker features without being locked to expensive, hard-to-find hardware.**

**Success means:**
- Musicians choosing budget handhelds specifically because PocketTracker exists
- Active community creating music and sharing knowledge
- Eventually matching and surpassing M8 capabilities
- Becoming the de-facto tracker for portable Android/Linux devices

**The journey:**
1. **Now → Feb 2025:** Feature-complete MVP with effects
2. **Feb → Mar 2025:** Architecture refactoring for portability
3. **Mar 2025:** Public beta release with tutorials
4. **Post-MVP:** Linux port, Braids synths, advanced features

This is a **marathon, not a sprint**. Sustainable development with proper architecture ensures the project will still be alive and thriving years from now.

---

**Version History:**
- v1.0 (2025-01-01): Initial vision document based on developer interviews
