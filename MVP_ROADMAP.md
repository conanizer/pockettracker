# PocketTracker - MVP Roadmap

## Document Purpose
**Vertical slices** and **milestones** to reach feature-complete MVP.

Each slice is a complete feature from UI to audio engine - fully working and testable.

**Target Date:** March/April 2026
**Status:** ALL MILESTONES COMPLETE — Testing & Polish → Documentation → Release

---

## Table of Contents
1. [MVP Definition](#mvp-definition)
2. [Current Status](#current-status)
3. [Development Strategy](#development-strategy)
4. [Vertical Slices](#vertical-slices)
5. [Timeline](#timeline)

---

## MVP Definition

### What is MVP?

**MVP = Minimum Viable Product**

The smallest version of PocketTracker that:
- ✅ Solves the core problem (M8-style tracker on affordable Android handhelds)
- ✅ Provides complete creative workflow (compose → arrange → export)
- ✅ Works reliably on target hardware
- ✅ Is usable by early adopters (even if not perfect)

### MVP vs Full Product

| Feature | MVP | Post-MVP |
|---------|-----|----------|
| **Phrase editing** | ✅ 16 steps, note/vol/inst | Advanced: Undo/redo, copy/paste ranges |
| **Copy/Paste** | ✅ M8-style selection, clipboard | Advanced clipboard features |
| **Effects** | ✅ TOP-5 (Arp/Offset/Vol/Kill/Repeat) in PHRASE | All effects, TABLE screen |
| **Instruments** | ✅ Sample loading, basic params | ADSR, advanced filters, Braids synth |
| **File management** | ✅ Save/load, browse, organize | Cloud sync, import/export |
| **Playback** | ✅ Phrase/chain/song, sample-accurate | Groove quantization, swing |
| **UI/UX** | ✅ Functional, readable | Themes, animations, polished |
| **Export** | ❌ (Post-MVP) | WAV render, MIDI export |
| **Documentation** | ✅ README, controls guide, demo video | Full manual, video tutorials |

---

## Current Status (March 2026) ✅ ALL COMPLETE

### Completed Systems ✅

**Audio Engine (100% Complete):**
- ✅ Sample-accurate queue system (<0.02ms jitter)
- ✅ 8-voice polyphony with per-track voice stealing + 3-step allocator (no exhaustion)
- ✅ Linear interpolation (professional audio quality)
- ✅ Resonant biquad filters (LP/HP/BP)
- ✅ Sample playback: start/end points, reverse, loop modes
- ✅ Oboe integration (LowLatency + Exclusive mode, 44.1kHz)
- ✅ Waveform capture for oscilloscope
- ✅ Stereo output with constant-power pan law
- ✅ Modulation engine (AHD, ADSR, LFO, DRUM, TRIG — all destinations)
- ✅ Table tick processing (per-voice, per-note)
- ✅ Offline WAV render (processAudioBlock unified DSP)
- ✅ Groove quantization (per-track groove assignments)

**Data Model (100% Complete):**
- ✅ Hierarchical structure: Project → Song → Chain → Phrase → Step
- ✅ 256 phrases, 256 chains, 8 tracks
- ✅ 256 instruments, 256 tables, 256 grooves
- ✅ ModSlot[4] per instrument (modulation envelopes/LFOs)
- ✅ JSON serialization/deserialization with forward migration

**UI Screens (100% Complete):**
- ✅ **Oscilloscope** - Real-time waveform visualization
- ✅ **Phrase Editor** - 16-step editing with N/V/I/FX columns
- ✅ **Chain Editor** - 16 phrase references with transpose
- ✅ **Song Editor** - 8-track arrangement, 256-row, B+UP/DOWN page jump
- ✅ **Instrument Screen** - Full parameter set including VOL/PAN
- ✅ **Project Screen** - Name, tempo, save/load, CLEAN SEQ/INST, layout mode switcher
- ✅ **File Browser** - Navigation, sorting, preview
- ✅ **Mixer Screen** - 8 tracks + master, true dBFS meters
- ✅ **Table Screen** - 16-row mini-sequencer per instrument
- ✅ **Groove Screen** - 16-step groove pattern editor (00-FF grooves)
- ✅ **Modulation Screen** - 4-slot envelope/LFO editor per instrument

**Effects (100% Complete):**
- ✅ ARP/ARC, OFF, VOL, KIL, REP (TOP-5, phrase screen)
- ✅ PSL, PBN, PVB, PVX (pitch effects)
- ✅ TIC, HOP (table control)
- ✅ DEL, CHA, RND, RNL, TBL, THO, GRV (Extension Pack 3)
- ✅ REP XY rework (single-retrig Y=0, volume ramp Y≠0)

**Layout System (100% Complete):**
- ✅ 4 layout modes: FULL, TOUCH_PORTRAIT, TOUCH_LANDSCAPE, TOUCH_PORTRAIT2
- ✅ Auto-switch on device rotation (Activity survives config change)
- ✅ Layout + scaling mode persisted via SharedPreferences
- ✅ Portrait2 with correct 4.5X/5.2X geometry and 0.1X outer spacers

**Pixel-Perfect Rendering (100% Complete):**
- ✅ INTEGER/BILINEAR/NEAREST scaling modes
- ✅ Font rendering uses anti-alias=false + horizontal run merging (zero pixel gaps)
- ✅ All dialogs (resample, clean) in pixel-art style

**Controls & Input (100% Complete):**
- ✅ Generic input (physical buttons + touchscreen unified via InputMapper)
- ✅ Key repeat (hold D-PAD / A+DPAD / B+DPAD)
- ✅ Selection increment (A+DPAD applies to all selected rows)
- ✅ Virtual controls: multi-touch combos, all layouts

**File Management (100% Complete):**
- ✅ Save/load .ptp projects (JSON format, with migration)
- ✅ WAV export (offline render → Documents/PocketTracker/Renders/)
- ✅ Selection resampling (SONG selection → Samples/Resampled/)
- ✅ File browser with sorting (name/date/size)
- ✅ MANAGE_EXTERNAL_STORAGE for cross-device project transfer

### Remaining Work

1. **Documentation** (3-5 days)
   - [ ] README (installation, basic usage)
   - [ ] Controls guide (full reference including all new effects)
   - [ ] Short demo video
   - [ ] Known issues list

2. **Testing & Polish** (1 week)
   - [ ] "Hello world" song usability test (<5 min)
   - [ ] Bug hunting on both devices
   - [ ] Performance verification
   - [ ] Example project creation

---

## Development Strategy

### Parallel Tracks

We can work on **two tracks simultaneously**:

**Track 1: Refactoring (Background)** ← Can be done independently
- Audio abstraction
- File I/O abstraction
- Logic extraction

**Track 2: Features (Foreground)** ← MVP blockers
- Effects system
- Copy/paste system

**Why parallel?**
- Refactoring doesn't block features development
- Features can be implemented in current architecture, then moved to refactored architecture
- Keeps momentum on MVP features while improving codebase

### Vertical Slice Approach

Each "slice" = **one complete feature from UI to audio**

**NOT a vertical slice:**
- "Write all the UI for effects" ← horizontal (only UI layer)
- "Implement all effects in C++" ← horizontal (only audio layer)

**YES, a vertical slice:**
- "Implement Arpeggio effect end-to-end" ✅
  - Parse "A01" command from phrase step
  - Pass to C++ audio engine
  - Trigger note pattern at audio callback
  - Test: Enter "A01" in phrase, hear arpeggiated note

---

## Vertical Slices

### MILESTONE 1: Architecture Refactoring

**Goal:** Prepare codebase for Linux port

**Duration:** 1-2 weeks

**Decision:** Option B (Foundational) - Separate controllers from Day 1

**Slices:**

#### Slice R1: Audio Backend Abstraction
**DoD (Definition of Done):**
- [ ] `IAudioBackend` interface created
- [ ] `OboeAudioBackend` implements interface
- [ ] `AudioEngine` uses `IAudioBackend` instead of JNI directly
- [ ] All audio features still work (phrase/chain/song playback)
- [ ] No regression in audio quality or latency

**Effort:** 2-3 days

#### Slice R2: Resource Loading Abstraction
**DoD:**
- [ ] `IResourceLoader` interface created
- [ ] `AndroidResourceLoader` implements interface
- [ ] Default samples load correctly
- [ ] Custom samples from file browser work
- [ ] Sample rate compensation still works

**Effort:** 1 day

#### Slice R3: File I/O Abstraction
**DoD:**
- [ ] `IFileSystem` interface created
- [ ] `AndroidFileSystem` implements interface
- [ ] `FileManager` uses `IFileSystem` instead of Android File API
- [ ] Project save/load works
- [ ] File browser works
- [ ] No permission issues

**Effort:** 1-2 days

#### Slice R4: Business Logic Extraction
**DoD:**
- [ ] 6 separate controllers created:
  - [ ] `TrackerController` (main coordinator)
  - [ ] `InputController` (button handling)
  - [ ] `PlaybackController` (playback logic)
  - [ ] `EffectProcessor` (effect calculations)
  - [ ] `InstrumentController` (sample/instrument management)
  - [ ] `FileController` (save/load operations)
- [ ] `MainActivity` is THIN (just creates backends + UI)
- [ ] All screens navigate correctly
- [ ] All button handlers work
- [ ] Cursor movement works
- [ ] Value editing (A+direction) works

**Effort:** 5-7 days

**MILESTONE 1 COMPLETE:** Codebase ready for Linux port, mentor can start working on it

---

### MILESTONE 2: Effects System Foundation

**Goal:** Implement TOP-5 essential effects in PHRASE screen

**Duration:** 1-2 weeks

**Why these 5 effects first?**
> "Most effects automate existing parameters. Once we implement one automation effect, the rest follow the same pattern. These 5 provide maximum creative expression with minimal code duplication."

**Note:** Table screen effects come in early Post-MVP

---

#### Slice E1: Effect Parser Infrastructure

**Goal:** Parse FX commands from phrase steps and route to audio engine

**DoD:**
- [ ] `EffectType` enum created (00=NONE, A=ARPEGGIO, O=OFFSET, V=VOLUME, K=KILL, R=REPEAT)
- [ ] Effect parser: "A01" → type=ARPEGGIO, value=0x01
- [ ] Effect data passed to C++ via JNI
- [ ] Test: Set FX1="A01" in phrase step, verify value passed to C++
- [ ] Logging shows effect commands being recognized

**Implementation Notes:**
```kotlin
// PhraseStep already has fx1Type, fx1Value fields! ✅
data class PhraseStep(
    var note: Note = Note.EMPTY,
    var instrument: Int = 0x00,
    var volume: Int = 0xFF,
    val fx1Type: Int = 0x00,     // 0x00=none, 0x01=arpeggio, 0x02=offset, etc.
    val fx1Value: Int = 0x00,    // Effect parameter (0x00-0xFF)
    val fx2Type: Int = 0x00,
    val fx2Value: Int = 0x00,
    val fx3Type: Int = 0x00,
    val fx3Value: Int = 0x00
)
```

**UI Integration:**
- Cursor can move to FX1/FX2/FX3 columns (already works!)
- A+UP/DOWN cycles through effect types
- A+LEFT/RIGHT increments/decrements value

**Effort:** 2 days

---

#### Slice E2: Arpeggio Effect (A00-AFF) ✅ COMPLETE

**Goal:** Implement first automation effect - proves the system works!

**Effect Behavior:**
```
ARP (Axx) - Arpeggio intervals:
  A00 = cancel arpeggio
  A37 = minor chord (root, +3, +7)
  A47 = major chord (root, +4, +7)
  ACC = double octave (root, +12, +12)

ARC (Cxx) - Arpeggio config (NEW EFFECT!):
  High nibble = mode: 0=UP, 1=DOWN, 2=PINGPONG, 3=RANDOM
  Low nibble = speed in tics (4=default, 1=fast, 6=slow)
  Example: C14 = DOWN mode, speed 4 tics
```

**DoD:**
- [x] Parse arpeggio value: high nibble = semitone 1, low nibble = semitone 2
- [x] ARC config effect: mode and speed parameters
- [x] Retrigger at tic intervals with pitch offset
- [x] Cross-step phase continuity (arpeggio pattern continues!)
- [x] Persistence: ARP persists until cancelled (LGPT/M8 style)
- [x] Test: Phrase with arpeggios across multiple steps

**Implementation:** Kotlin in PlaybackController.kt
- scheduleArpeggioNotes() calculates frame-based positions
- getArpeggioNote() returns correct MIDI note for pattern position
- TrackState tracks: arpeggioValue, arpeggioMode, arpeggioSpeed, arpeggioStartFrame

**TODO (Post-MVP):** Additional ARC modes: UP_OCT, DOWN_OCT, CHORD, SHUFFLE

**Effort:** 2 days (completed 2026-01-20)

---

#### Slice E3: Offset Effect (O00-OFF) ✅ COMPLETE

**Goal:** Automate sample start point

**Effect Behavior:**
```
O00 = play from default start point
O80 = play from 50% into sample
OFF = play from near end of sample
```

**DoD:**
- [x] Parse offset value (0x00-0xFF maps to 0%-100% of sample length)
- [x] C++ effect processor: override sample start point
- [x] Existing start/end point system used
- [x] Test: Drum sample with different offset values (kick → snare-like tail)
- [x] Verify no clicks/pops at offset start

**Implementation:**
```cpp
if (step.fx1Type == FX_OFFSET && step.fx1Value != 0x00) {
    // Calculate new start point based on effect value
    float offsetRatio = step.fx1Value / 255.0f;
    int newStartPoint = instrument.startPoint + 
                        (int)(offsetRatio * (instrument.endPoint - instrument.startPoint));
    
    // Override start point for this note only
    playNoteWithStartPoint(sampleId, trackId, freq, vol, newStartPoint);
}
```

**Effort:** 1-2 days

---

#### Slice E4: Volume Effect (V00-VFF)

**Goal:** Automate volume changes within step

**Effect Behavior:**
```
V00 = fade to silence
V80 = fade to 50% volume
VFF = fade to full volume
```

**DoD:**
- [ ] Parse volume target (0x00-0xFF)
- [ ] C++ effect processor: linear volume ramp over step duration
- [ ] Test: Long note with V00 (fade out)
- [ ] Test: Silent note with VFF (fade in)
- [ ] No clicks/pops during volume changes

**Implementation:**
```cpp
if (step.fx1Type == FX_VOLUME && step.fx1Value != 0x00) {
    float targetVol = step.fx1Value / 255.0f;
    float currentVol = vol;
    long stepDuration = getStepDurationInFrames(tempo);
    
    // Apply volume ramp in audio callback
    for each frame in step {
        float t = (currentFrame - stepStartFrame) / (float)stepDuration;
        float rampedVol = currentVol + (targetVol - currentVol) * t;
        // Apply to voice
    }
}
```

**Effort:** 2 days

---

#### Slice E5: Kill Effect (K00) ✅ COMPLETE

**Goal:** Stop sample immediately

**Effect Behavior:**
```
K00 = stop voice immediately (no fade)
```

**DoD:**
- [x] Kill effect recognized (no value needed, just K00)
- [x] C++ effect processor: stop voice instantly
- [x] Test: Long sample with K00 on step 8 (cuts off mid-sample)
- [x] Verify no audio artifacts

**Implementation:**
```cpp
if (step.fx1Type == FX_KILL) {
    // Stop voice immediately
    voices[trackId].active = false;
    voices[trackId].position = 0;
}
```

**Effort:** 1 day (easiest effect!)

---

#### Slice E6: Repeat Effect (R01-RFF) ✅ COMPLETE

**Goal:** Retrigger sample with persistence and multi-step intervals (LGPT/M8 style)

**Effect Behavior - Sub-step intervals (R01-R0B):**
```
R01 = retrig every 1 tic = 12 triggers/step (fastest)
R02 = retrig every 2 tics = 6 triggers/step
R03 = retrig every 3 tics = 4 triggers/step (triplets!)
R06 = retrig every 6 tics = 2 triggers/step
```

**Effect Behavior - Multi-step intervals (R0C+):**
```
R0C (12) = every 1 step
R12 (18) = every 1.5 steps (dotted notes!)
R18 (24) = every 2 steps
R30 (48) = every 4 steps (4 kicks in 16-step phrase!)
R60 (96) = every 8 steps
```

**Persistence (LGPT/M8 style):**
- REPEAT persists until cancelled by:
  1. New note on the same track
  2. Any effect in the same FX column where REPEAT was set
  3. KILL effect (K00) in any column
- Cross-phrase persistence in Chain/Song mode!

**DoD:**
- [x] Parse repeat tic interval (0x01-0xFF)
- [x] Sub-step intervals: multiple triggers within one step
- [x] Multi-step intervals: triggers across multiple steps
- [x] Persistence: effect continues until cancelled
- [x] Cross-phrase persistence in Chain/Song playback
- [x] Test: Kick drum with R03 (triplet stutter)
- [x] Test: R30 for 4 evenly-spaced kicks in phrase
- [x] Test: Chain with persistent REPEAT across phrases

**Implementation:** Frame-based tracking in PlaybackController.kt
- TrackState tracks: lastNote, lastInstrument, repeatStartFrame, repeatTicInterval
- scheduleStepWithEffects handles cancellation conditions
- Multi-step uses absolute frame arithmetic for cross-phrase accuracy

**Effort:** 2 days (completed 2026-01-19)

---

**MILESTONE 2 COMPLETE:** TOP-5 effects working in phrase screen! 🎉

---

### MILESTONE 2.5: Copy/Paste System ✅ COMPLETE

**Goal:** Implement M8-style selection mode and clipboard workflow

**Duration:** 4-5 days

**Why in MVP?**
> Copy/paste is essential for efficient music production workflow. Users need to duplicate patterns, create variations, and organize phrases quickly. Without copy/paste, creating a full song becomes tedious.

---

#### Slice CP1: Selection Mode Infrastructure

**Goal:** Allow selecting multiple steps/rows in phrase/chain editors

**DoD:**
- [x] L+B enters selection mode on grid views
- [x] D-pad expands/contracts selection while in selection mode
- [x] Visual highlight shows selected area (inverted colors or border)
- [x] B exits selection mode without copying
- [x] Selection works in phrase editor (vertical and horizontal)
- [x] Selection works in chain editor
- [x] Selection works in song editor
- [x] Selection state persists in cursor context
- [x] Can select single item or multi-item ranges

**Implementation Notes:**
```kotlin
// core/logic/InputController.kt
class InputController(
    private val project: Project,
    private val clipboard: ClipboardManager
) {
    // Selection state
    var selectionMode by mutableStateOf(false)
    var selectionStart: CursorPosition? = null
    var selectionEnd: CursorPosition? = null
    
    // Enter selection mode (SELECT+B on grid)
    fun enterSelectionMode() {
        selectionMode = true
        selectionStart = currentCursor.copy()
        selectionEnd = currentCursor.copy()
    }
    
    // Expand/contract selection with D-pad
    fun handleDpadInSelection(direction: Direction) {
        if (!selectionMode) return
        
        val newEnd = when (direction) {
            UP -> selectionEnd!!.copy(row = row - 1)
            DOWN -> selectionEnd!!.copy(row = row + 1)
            LEFT -> selectionEnd!!.copy(column = column - 1)
            RIGHT -> selectionEnd!!.copy(column = column + 1)
        }
        
        // Clamp to valid ranges
        selectionEnd = newEnd.clamped()
    }
    
    // Get normalized selection (start always before end)
    fun getSelection(): Selection {
        val start = selectionStart!!
        val end = selectionEnd!!
        
        return Selection(
            topLeft = Position(
                min(start.row, end.row),
                min(start.column, end.column)
            ),
            bottomRight = Position(
                max(start.row, end.row),
                max(start.column, end.column)
            )
        )
    }
}

data class Selection(
    val topLeft: Position,
    val bottomRight: Position
) {
    val width: Int get() = bottomRight.column - topLeft.column + 1
    val height: Int get() = bottomRight.row - topLeft.row + 1
    val count: Int get() = width * height
}
```

**Visual Feedback:**
```kotlin
// In rendering module, highlight selected area
if (inputController.selectionMode) {
    val sel = inputController.getSelection()
    
    // Draw selection rectangle
    drawRect(
        left = sel.topLeft.column * CHAR_WIDTH,
        top = sel.topLeft.row * CHAR_HEIGHT,
        right = (sel.bottomRight.column + 1) * CHAR_WIDTH,
        bottom = (sel.bottomRight.row + 1) * CHAR_HEIGHT,
        color = COLOR_SELECTION,  // Semi-transparent overlay
        style = Stroke(width = 2f)
    )
}
```

**Effort:** 2 days

---

#### Slice CP2: Copy/Paste Operations ✅ COMPLETE

**Goal:** Implement clipboard operations for selected data

**DoD:**
- [x] B in selection mode copies selection and exits selection mode ✅
- [x] **L+A pastes clipboard contents at cursor position** ✅
- [x] L+A in selection cuts (copy + delete) ✅
- [x] A+B in selection deletes (no clipboard) ✅
- [x] L alone cancels selection mode ✅
- [x] Copy/paste phrase steps works correctly
- [x] Copy/paste between different phrases works
- [x] Copy/paste chain rows works
- [x] Paste validates and adjusts to target context
- [x] Clipboard persists across screen changes
- [x] Can paste multiple times (clipboard stays populated)

**M8 Control Mapping (CORRECTED):**
```
On grid views (SONG, CHAIN, PHRASE):

L+B          → Enter selection mode
                    (cursor freezes, selection starts at current position)

D-PAD (in select) → Expand/contract selection
                    (visual highlight updates in real-time)

B (in select)     → Copy selection to clipboard and exit selection mode
                    (status shows "Copied X items")

L+A (in select)   → Cut selection (copy + delete) and exit ✅ IMPLEMENTED
                    (copies to clipboard, then deletes original)

L+A (outside)     → Paste clipboard contents at current cursor ✅ IMPLEMENTED
                    (validates target, shows error if incompatible)

A+B (in select)   → Delete selection (no clipboard) and exit ✅ IMPLEMENTED
                    (deletes original WITHOUT copying to clipboard)

L alone           → Cancel selection mode without copying ✅ IMPLEMENTED
```

**Implementation:**
```kotlin
// core/logic/ClipboardManager.kt
class ClipboardManager {
    data class ClipboardData(
        val type: ClipboardType,
        val data: Any,  // Type depends on ClipboardType
        val width: Int,
        val height: Int,
        val timestamp: Long = System.currentTimeMillis()
    )
    
    enum class ClipboardType {
        PHRASE_STEPS,     // List<PhraseStep>
        PHRASE,           // Phrase
        CHAIN_ROWS,       // List<ChainRow>
        CHAIN,            // Chain
        // Future: INSTRUMENT, TABLE_ROWS, etc.
    }
    
    var clipboard: ClipboardData? = null
        private set
    
    // Copy operation
    fun copy(
        type: ClipboardType,
        data: Any,
        width: Int = 1,
        height: Int = 1
    ) {
        clipboard = ClipboardData(type, data, width, height)
    }
    
    // Paste operation
    fun paste(
        project: Project,
        screen: ScreenType,
        target: CursorPosition
    ): PasteResult {
        val clip = clipboard ?: return PasteResult.NoClipboard
        
        return when (clip.type) {
            PHRASE_STEPS -> pastePhraseSteps(project, target, clip)
            PHRASE -> pastePhrase(project, target, clip)
            CHAIN_ROWS -> pasteChainRows(project, target, clip)
            CHAIN -> pasteChain(project, target, clip)
        }
    }
    
    // Cut operation (copy + delete)
    fun cut(/* ... */): Boolean {
        val copied = copy(/* ... */)
        if (copied) {
            deleteSelection(/* ... */)
            return true
        }
        return false
    }
    
    // Paste phrase steps into target phrase
    private fun pastePhraseSteps(
        project: Project,
        target: CursorPosition,
        clip: ClipboardData
    ): PasteResult {
        val steps = clip.data as List<PhraseStep>
        val targetPhrase = project.phrases[target.phraseId]
        
        // Validate: can we fit the clipboard data?
        if (target.row + clip.height > 16) {
            return PasteResult.OutOfBounds
        }
        
        // Paste each step
        steps.forEachIndexed { index, step ->
            val targetRow = target.row + index
            targetPhrase.steps[targetRow] = step.copy()  // Deep copy!
        }
        
        return PasteResult.Success(clip.height)
    }
    
    // Similar implementations for other types...
}

sealed class PasteResult {
    object NoClipboard : PasteResult()
    object OutOfBounds : PasteResult()
    object TypeMismatch : PasteResult()
    data class Success(val itemsPasted: Int) : PasteResult()
}
```

**Integration with InputController:**
```kotlin
class InputController(
    private val clipboard: ClipboardManager,
    /* ... */
) {
    // B in selection mode → Copy and exit
    fun handleCopyInSelection() {
        if (!selectionMode) return
        
        val selection = getSelection()
        val data = extractSelectionData(selection)
        
        clipboard.copy(
            type = determineClipboardType(currentScreen, selection),
            data = data,
            width = selection.width,
            height = selection.height
        )
        
        exitSelectionMode()
        
        // Show feedback
        statusMessage = "Copied ${selection.count} items"
    }
    
    // SELECT+A → Paste ✅ CORRECTED
    fun handlePaste() {
        val result = clipboard.paste(project, currentScreen, currentCursor)
        
        when (result) {
            is Success -> statusMessage = "Pasted ${result.itemsPasted} items"
            is OutOfBounds -> statusMessage = "Not enough space"
            is TypeMismatch -> statusMessage = "Invalid paste target"
            is NoClipboard -> statusMessage = "Clipboard empty"
        }
    }
    
    // A+B → Cut
    fun handleCut() {
        handleCopyInSelection()
        deleteSelection()
        statusMessage = "Cut ${selection.count} items"
    }
    
    // Extract data from selection
    private fun extractSelectionData(selection: Selection): Any {
        return when (currentScreen) {
            PHRASE -> {
                val phrase = project.phrases[currentCursor.phraseId]
                phrase.steps.slice(selection.topLeft.row..selection.bottomRight.row)
            }
            CHAIN -> {
                val chain = project.chains[currentCursor.chainId]
                // Extract chain rows...
            }
            // ...
        }
    }
}
```

**Effort:** 2 days

---

#### Slice CP3: Clipboard Indicator ✅ COMPLETE

**Goal:** Show clipboard status in header row (persistent indicator)

**DoD:**
- [x] Header shows clipboard contents when something is copied
- [x] Format matches PocketTracker style (compact, readable)
- [x] Updates immediately when copying
- [x] Persists across screen changes
- [x] Doesn't interfere with other header information
- [x] Clears when clipboard is empty/invalidated

**Implementation:**
```kotlin
// In rendering (header row, right side)
fun drawHeaderRow(
    drawScope: DrawScope,
    clipboard: ClipboardManager,
    currentScreen: ScreenType,
    tempo: Int
) {
    // Left side: Screen name
    drawText("PHRASE", x = 4, y = 4)
    
    // Center: Tempo
    drawText("${tempo}BPM", x = 280, y = 4)
    
    // Right side: Clipboard indicator
    val clipboardText = formatClipboardIndicator(clipboard.clipboard)
    if (clipboardText.isNotEmpty()) {
        val xPos = SCREEN_WIDTH - (clipboardText.length * CHAR_WIDTH) - 8
        drawText(
            text = clipboardText,
            x = xPos,
            y = 4,
            color = COLOR_HIGHLIGHT  // Different color to stand out
        )
    }
}

// Format clipboard indicator (compact representation)
fun formatClipboardIndicator(clip: ClipboardData?): String {
    if (clip == null) return ""
    
    return when (clip.type) {
        PHRASE_STEPS -> {
            if (clip.height == 1) "📋 1 STEP"
            else "📋 ${clip.height} STEPS"
        }
        PHRASE -> {
            val phrase = clip.data as Phrase
            "📋 PHR ${phrase.id.toString(16).uppercase().padStart(2, '0')}"
        }
        CHAIN_ROWS -> {
            if (clip.height == 1) "📋 1 ROW"
            else "📋 ${clip.height} ROWS"
        }
        CHAIN -> {
            val chain = clip.data as Chain
            "📋 CHN ${chain.id.toString(16).uppercase().padStart(2, '0')}"
        }
    }
}
```

**Alternative format (simpler):**
```
Clipboard empty:       [no indicator]
1 phrase step copied:  📋 STEP
5 phrase steps:        📋 5 STEPS  
Phrase 03:             📋 P03
Chain 0A:              📋 C0A
```

**Visual Example:**
```
┌────────────────────────────────────────┐
│ PHRASE           120BPM      📋 3 STEPS│ ← Header row
├────────────────────────────────────────┤
│ 00 C-4 01 FF A0C --- ---               │
│ 01 --- -- -- --- --- ---               │
│ 02 E-4 01 80 --- --- ---               │
│ 03 --- -- -- --- --- ---               │
```

**Effort:** 1 day

---

**MILESTONE 2.5 COMPLETE:** Copy/paste workflow working! 🎉

---

### MILESTONE 3: Effects & Copy/Paste UI Testing

**Goal:** Make effects and copy/paste easy to use and reliable

**Duration:** 1 week

---

#### Slice T1: Effect Editing UI

**Goal:** Smooth workflow for entering effect commands

**DoD:**
- [x] Cursor navigates to FX1/FX2/FX3 columns
- [x] A+UP/DOWN cycles effect types (NONE → ARP → OFFSET → VOL → KILL → REPEAT)
- [x] A+LEFT/RIGHT increments/decrements value (00 → 01 → ... → FF)
- [x] Visual feedback (cursor highlights current effect column)

**Implementation:**
```kotlin
// In PhraseEditorModule.kt
when (cursorColumn) {
    4 -> { // FX1 type
        if (isAButtonHeld && dpadUp) {
            currentStep.fx1Type = (currentStep.fx1Type + 1) % 6  // Cycle through types
        }
    }
    5 -> { // FX1 value
        if (isAButtonHeld && dpadUp) {
            currentStep.fx1Value = (currentStep.fx1Value + 1).coerceIn(0, 255)
        }
    }
}
```

**Effort:** 1-2 days

---

#### Slice T2: Copy/Paste Workflow Testing ✅ COMPLETE

**Goal:** Ensure copy/paste feels natural and works reliably

**Test Cases:**
- [x] Enter selection mode (L+B)
- [x] Expand selection in all 4 directions
- [x] Copy selection (B in selection mode)
- [x] Paste into same phrase (L+A) ✅
- [x] Paste into different phrase (L+A) ✅
- [x] Cut selection (L+A in selection) ✅
- [x] Delete selection (A+B in selection) ✅
- [x] Cancel selection (L alone) ✅
- [x] Multiple pastes from same clipboard
- [x] Clipboard indicator updates correctly
- [x] Selection highlights correctly
- [x] Edge cases: paste beyond bounds, empty clipboard

**DoD:**
- [x] All test cases pass
- [x] No crashes during copy/paste operations
- [x] Visual feedback clear and helpful
- [x] Works in both phrase and chain editors

**Effort:** 1 day

---

#### Slice T3: Effects Testing & Refinement

**Goal:** Ensure effects work reliably in all scenarios

**Test Cases:**
- [x] Single effect per step works
- [x] Multiple effects (FX1 + FX2 + FX3) work simultaneously
- [x] Effects work in phrase playback
- [x] Effects work in chain playback (with transpose)
- [x] Effects work in song playback (8 tracks)
- [x] Effects save/load correctly
- [x] Edge cases: A00 (no arp), R01 (no repeat), K00 on empty step (no crash)

**DoD:**
- [x] All test cases pass
- [x] No audio glitches or clicks
- [x] No crashes or hangs
- [ ] Performance impact acceptable (<5% CPU increase)

**Effort:** 2-3 days

---

#### Slice T4: Integration Testing ✅ COMPLETE

**Goal:** Verify effects and copy/paste work together

**Test Scenarios:**
- [x] Copy phrase with effects, paste to different phrase
- [x] Effects values preserved during copy/paste
- [x] Can copy/paste effect-only changes (no note changes)
- [x] Undo/clear effects in selection (future: undo/redo)

**DoD:**
- [x] Effects + copy/paste integration solid
- [x] No data corruption
- [x] Clipboard correctly handles effect data

**Effort:** 1 day

---

**MILESTONE 3 COMPLETE:** Effects and copy/paste production-ready!

---

### MILESTONE 4: Polish & Documentation

**Goal:** Make MVP ready for public release

**Duration:** 1-2 weeks

---

#### Slice P1: Example Project

**Goal:** Demonstrate all features in one project

**DoD:**
- [ ] "demo.ptp" project created
- [ ] Uses all TOP-5 effects creatively
- [ ] Shows phrase → chain → song workflow
- [ ] Demonstrates copy/paste workflow (phrase variations)
- [ ] Completable in <5 min by new user (following tutorial)
- [ ] Sounds good! (not just technical demo)

**Content:**
- Song: 16 bars, 120 BPM, lo-fi beat style
- Track 1: Kick drum (basic pattern)
- Track 2: Snare with offset variations
- Track 3: Hi-hat with repeat rolls
- Track 4: Bass with arpeggio melody
- Track 5-8: Simple pads/fx

**Effort:** 1-2 days (fun!)

---

#### Slice P2: README Documentation

**Goal:** Clear installation and usage instructions

**DoD:**
- [ ] Installation section (APK download, permissions)
- [ ] Quick start guide (load example project, play it)
- [ ] Controls reference (button mappings with copy/paste)
- [ ] Features list (what works, what doesn't)
- [ ] FAQ (common issues)
- [ ] License (GPL)
- [ ] Credits (Oboe, mentor, contributors)

**Effort:** 1 day

---

#### Slice P3: Demo Video

**Goal:** Show don't tell - demonstrate workflow visually

**DoD:**
- [ ] 5-10 minute video recorded
- [ ] Shows: Load sample → Create phrase → Copy/paste variations → Chain phrases → Build song → Play!
- [ ] Demonstrates TOP-5 effects
- [ ] Demonstrates copy/paste workflow
- [ ] Clear audio/video quality
- [ ] Uploaded to YouTube
- [ ] Linked in README

**Effort:** 1-2 days (recording + editing)

---

#### Slice P4: Performance Verification

**Goal:** Verify stable performance on target hardware

**DoD:**
- [ ] Test on Miyoo Flip (1GB RAM, primary device)
- [ ] Test on Ayaneo Pocket Air Mini (3GB RAM, secondary device)
- [ ] Identify any bottlenecks
- [ ] Optimize if needed (rendering, audio)
- [ ] Target: Stable framerate during 8-track playback
- [ ] Acceptable: 30fps minimum, 60fps preferred

**Effort:** 2-3 days

---

#### Slice P5: Bug Hunting & Fixes

**Goal:** Squash known issues before release

**DoD:**
- [ ] Test on multiple devices (2+ handhelds, 2+ phones)
- [ ] Test all edge cases (empty project, full project, rapid button presses)
- [ ] Fix critical bugs (crashes, data loss)
- [ ] Document known issues (minor bugs, limitations)
- [ ] No known crash bugs
- [ ] <5 known minor bugs

**Effort:** 2-3 days

---

**MILESTONE 4 COMPLETE:** MVP ready for public beta! 🚀

---

## Timeline

### Actual Timeline

**Late 2025 – January 2026:**
- ✅ Architecture refactoring (Milestone 1)
- ✅ Effects system — TOP-5 (Milestone 2)
- ✅ Copy/paste system (Milestone 2.5)

**January–February 2026:**
- ✅ MVP Expansion: Mixer, WAV Export, VOL/PAN, Stereo
- ✅ Extension Pack 2: Tables, TIC/HOP, Pitch effects

**February–March 2026:**
- ✅ Extension Pack 3: Groove, Modulation, Resampling, Layout system, Polish

**March–April 2026:**
- 🚧 Testing & Polish
- 📝 Documentation & video
- 🚀 MVP Release

**Target:** April 2026 public release

### Critical Path

**Must be done in order:**
1. Refactoring (enables clean feature development)
2. Effects System (core musical feature)
3. Copy/Paste System (workflow essential)
4. Testing & Polish (quality assurance)
5. Documentation (user enablement)

**Can be parallelized:**
- Example project creation (while documentation is written)
- Video tutorial (after example project exists)
- Performance optimization (ongoing during development)

---

## Priorities & Flexibility

### Must-Have for MVP ✅

1. **Effects System** - Absolutely critical
   - TOP-5 effects minimum (phrase screen only)
   - Table screen effects = early Post-MVP

2. **Copy/Paste** - Essential workflow feature
   - M8-style selection and clipboard
   - Copy/paste between phrases
   - Advanced features = early Post-MVP

3. **Architecture Refactoring** - Critical for future
   - Can delay if time pressure (but recommended NOW)
   - Makes Linux port and mentor collaboration easier

4. **Testing** - Don't ship broken software
   - All core features work
   - No crash bugs
   - Tested on real hardware

5. **Documentation** - Users need guidance
   - README with installation
   - Example project
   - Controls reference (including copy/paste)
   - Short demo video

### Nice-to-Have (Can be Post-MVP) 💚

1. **Performance Optimization** - If "good enough" works
   - 30fps acceptable for MVP
   - Can optimize post-release

2. **Advanced Copy/Paste** - Basic works for MVP
   - Instrument settings copy/paste
   - Multiple clipboard slots
   - Cross-screen pasting enhancements

3. **Table Screen** - Effects work without it
   - Can be early Post-MVP Phase 2 feature
   - Same effects, different application method

### Can Slip to Post-MVP ⚠️

- Undo/redo
- Advanced clipboard features
- WAV export
- Advanced filters
- Mixer screen with visual faders
- Themes/color schemes

**Philosophy:** Ship working core features → iterate based on feedback

---

## Success Metrics

### How We Know MVP is Done

**Functional Criteria:**
- ✅ Can create phrase with notes and effects
- ✅ Can copy/paste phrase steps and selections
- ✅ Can chain phrases with transpose
- ✅ Can arrange song with 8 tracks
- ✅ Can save and load projects
- ✅ All TOP-5 effects work correctly (phrase screen)
- ✅ M8-style copy/paste workflow works
- ✅ No crash bugs in common workflows

**Usability Criteria:**
- ✅ New user can complete "hello world" song in <5 min (with tutorial)
- ✅ Example project demonstrates all features
- ✅ README explains installation and basic usage
- ✅ Demo video shows complete workflow

**Technical Criteria:**
- ✅ Works on Android 8.0+ (API 26+)
- ✅ Works on 640×480 minimum resolution
- ✅ Works on 1GB RAM devices (Miyoo Flip tested)
- ✅ Audio latency <50ms
- ✅ Performance acceptable (30-60fps)

**Community Criteria:**
- ✅ Beta testers can use it productively
- ✅ At least one person makes music with it
- ✅ GitHub repo is public and documented

---

## Post-MVP Roadmap (Future Work)

After MVP release, priorities determined by:
1. **User feedback** - What do people actually want?
2. **Bug reports** - What's broken?
3. **Platform requests** - What devices need support?
4. **Community contributions** - What are people building?

**Tentative Early Post-MVP Order:**
1. **Table screen** (effects for instruments)
2. **Advanced copy/paste** (instrument settings)
3. **Architecture refactoring** (if skipped in MVP)
4. **Linux port** (mentor joins!)
5. **Remaining effects** (pitch, pan, filter, etc.)
6. **Undo/redo** (most requested?)
7. **Copy/paste enhancements** (workflow efficiency)
8. **WAV export** (sharing songs)
9. **Braids synthesizers** (with mentor)
10. **Mixer screen** (visual mixing)
11. **Themes/polish** (aesthetics)

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
- [x] **User can apply effects (TOP-5 in phrase screen)** ✅ COMPLETE!
- [x] **User can copy/paste phrase steps (selection mode)** ✅ COMPLETE!
- [x] **User can copy/paste between different phrases** ✅ COMPLETE!
- [x] **User can cut/delete selections with L+A/A+B** ✅ COMPLETE!

### File Management ✅
- [x] User can save project to `.ptp` file
- [x] User can load previously saved project
- [x] User can browse folders and navigate filesystem
- [x] User can organize samples in subfolders

### Performance ⚠️
- [x] Audio latency is under 50ms (tested on Miyoo Flip)
- [x] No audio glitches or dropouts during playback
- [x] Works on Android 8.0+ (API 26+)
- [x] Works on 1GB RAM devices (Miyoo Flip) ✅
- [x] App runs at stable framerate on target hardware - needs verification on Ayaneo

### Controls ✅
- [x] All D-pad directions work (cursor navigation)
- [x] A/B buttons work (confirm/cancel, value editing)
- [x] Start button works (play/stop toggle)
- [x] L/R shoulders work (screen navigation, modifiers)
- [x] Virtual controls work on touchscreen devices
- [x] **Copy/paste controls work (L+B, L+A, A+B, L alone)** ✅ COMPLETE!
- [x] **Selection mode works (visual highlight)** ✅
- [x] **Clipboard indicator shows in header** ✅

### Usability ⚠️
- [ ] User can complete "hello world" song in under 5 minutes
- [ ] Navigation between screens is intuitive (no getting lost)
- [x] Status messages explain what happened (save success/fail, etc.)
- [x] App doesn't crash on common user errors - needs testing

### Documentation 📝
- [ ] README explains how to install and use
- [ ] Example project included (demo song with effects and copy/paste)
- [ ] Controls documented (button mappings including copy/paste)
- [ ] Demo video (showcasing workflow)

---

## Final Notes

### For Claude Code AI

When working on vertical slices:
1. **Read DEVELOPMENT_STATUS.md** first - know what's already done
2. **Read TECHNICAL_ARCHITECTURE.md** - understand system design
3. **Follow REFACTORING_ROADMAP.md** - if doing architecture work
4. **One slice at a time** - don't jump ahead
5. **Test before committing** - verify DoD is met
6. **Update DEVELOPMENT_STATUS.md** - track progress

### For Developer (You!)

- **Don't rush** - Quality > Speed
- **Take breaks** - Avoid burnout
- **Ask for help** - Claude Code, mentor, community
- **Celebrate progress** - Each slice is a win!
- **Iterate** - First version doesn't need to be perfect

### When You Feel Overwhelmed

1. Look at DEVELOPMENT_STATUS.md - What's the CURRENT slice?
2. Look at that slice's DoD - What's the NEXT checkbox?
3. Do JUST that one thing
4. Check the box
5. Repeat

**One checkbox at a time = MVP gets done!** 🎯

---

**Version History:**
- v1.3 (2026-03-13): Updated status — all milestones complete, Testing & Polish phase
- v1.2 (2026-01-27): Updated with Expansion Pack completions and revised timeline
- v1.1 (2025-01-01): Added copy/paste milestone, corrected paste control (SELECT+A)
- v1.0 (2025-01-01): Initial MVP roadmap with vertical slices
