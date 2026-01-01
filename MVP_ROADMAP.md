# PocketTracker - MVP Roadmap

## Document Purpose
**Vertical slices** and **milestones** to reach feature-complete MVP by February 2025.

Each slice is a complete feature from UI to audio engine - fully working and testable.

**Target Date:** Late February 2025  
**Status:** Phase A ~95% complete, Refactoring + Effects + Copy/Paste remaining

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

## Current Status (January 2025)

### Completed Systems ✅

**Audio Engine (100% Complete):**
- ✅ Sample-accurate queue system (<0.02ms jitter)
- ✅ 8-voice polyphony with per-track voice stealing
- ✅ Linear interpolation (professional audio quality)
- ✅ Biquad filters (LP/HP/BP)
- ✅ Sample playback: start/end points, reverse, loop modes
- ✅ Oboe integration (LowLatency + Exclusive mode, 44.1kHz)
- ✅ Waveform capture for oscilloscope

**Data Model (100% Complete):**
- ✅ Hierarchical structure: Project → Song → Chain → Phrase → Step
- ✅ 256 phrases, 256 chains, 8 tracks
- ✅ 256 instruments with full parameters
- ✅ JSON serialization/deserialization
- ✅ Sample rate compensation (automatic pitch correction)

**UI Screens (95% Complete):**
- ✅ **Oscilloscope** - Real-time waveform visualization
- ✅ **Phrase Editor** - 16-step editing with N/V/I/FX columns
- ✅ **Chain Editor** - 16 phrase references with transpose
- ✅ **Song Editor** - 8-track arrangement
- ✅ **Instrument Screen** - Sample loading, ROOT/DETUNE, playback params
- ✅ **Project Screen** - Name, tempo, save/load
- ✅ **File Browser** - Navigation, sorting, preview
- ⚠️ **Effects not implemented** - FX columns exist but don't work yet
- ⚠️ **Copy/paste not implemented** - Selection mode not yet added

**Controls & Input (100% Complete):**
- ✅ Generic input system (works with physical buttons OR touchscreen)
- ✅ Device detection (gaming handheld vs smartphone)
- ✅ Virtual controls for touchscreen
- ✅ A+direction value editing
- ✅ Quick insert (A on empty row)
- ✅ Cursor context memory (smooth navigation between screens)

**File Management (100% Complete):**
- ✅ Save/load .ptp projects (JSON format)
- ✅ File browser with sorting (name/date/size)
- ✅ Sample preview (in browser and instrument screen)
- ✅ Directory navigation
- ✅ Android scoped storage support

**Playback (100% Complete):**
- ✅ Phrase playback with looping
- ✅ Chain playback with transpose
- ✅ Song playback (8 tracks polyphonic)
- ✅ Continuous buffering (2-phrase lookahead)
- ✅ 50ms startup latency (instant feel)
- ✅ Frame-based cursors (accurate position tracking)

### Remaining MVP Work ⚠️

**Critical for MVP:**
1. **Effects System** - BIG task (1-2 weeks)
   - [ ] Effect command parser (FX1/FX2/FX3 columns)
   - [ ] Implement TOP-5 effects in C++ audio engine
   - [ ] Effects in PHRASE screen only (table screen Post-MVP)

2. **Copy/Paste System** - ESSENTIAL (4-5 days)
   - [ ] M8-style selection mode
   - [ ] Copy/paste phrase steps and selections
   - [ ] Copy/paste between different phrases
   - [ ] Clipboard indicator in header
   - [ ] Cut/delete selections

3. **Architecture Refactoring** - CRITICAL for future (1-2 weeks)
   - [ ] Audio backend abstraction (IAudioBackend)
   - [ ] Resource loading abstraction (IResourceLoader)
   - [ ] File I/O abstraction (IFileSystem)
   - [ ] Business logic extraction (separate controllers)

4. **Testing & Polish** (1 week)
   - [ ] "Hello world" song in <5 min usability test
   - [ ] Bug hunting on real hardware
   - [ ] Performance optimization (stable framerate)
   - [ ] Example project creation

5. **Documentation** (3-5 days)
   - [ ] README (installation, basic usage)
   - [ ] Controls guide (button mappings)
   - [ ] Short demo video
   - [ ] Known issues list

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

#### Slice E2: Arpeggio Effect (A00-AFF)

**Goal:** Implement first automation effect - proves the system works!

**Effect Behavior:**
```
A00 = no arpeggio
A01 = play note, note+1 semitone, note (repeats every 1/3 step)
A0C = play note, note+12 semitones (octave), note (major chord simulation)
A37 = play note, note+3, note+7 (major chord: C-E-G)
```

**DoD:**
- [ ] Parse arpeggio value: high nibble = semitone 1, low nibble = semitone 2
- [ ] C++ effect processor: `processArpeggio(step, value, currentFrame)`
- [ ] Retrigger note at 1/3 step intervals with pitch offset
- [ ] Test song: Phrase with single note, different arpeggio values per step
- [ ] Hear distinct arpeggiated patterns

**Implementation Strategy:**
```cpp
// In audio callback (C++)
if (step.fx1Type == FX_ARPEGGIO && step.fx1Value != 0x00) {
    int semitone1 = (step.fx1Value >> 4) & 0x0F;  // High nibble
    int semitone2 = step.fx1Value & 0x0F;          // Low nibble
    
    // Calculate frame positions for 3 notes within step
    long stepDuration = getStepDurationInFrames(tempo);
    long arpInterval = stepDuration / 3;
    
    // Schedule 3 notes: base, +semitone1, +semitone2
    scheduleNote(frame, sampleId, trackId, baseFreq, vol);
    scheduleNote(frame + arpInterval, sampleId, trackId, baseFreq * semitoneRatio(semitone1), vol);
    scheduleNote(frame + 2*arpInterval, sampleId, trackId, baseFreq * semitoneRatio(semitone2), vol);
}
```

**Effort:** 2-3 days

---

#### Slice E3: Offset Effect (O00-OFF)

**Goal:** Automate sample start point

**Effect Behavior:**
```
O00 = play from default start point
O80 = play from 50% into sample
OFF = play from near end of sample
```

**DoD:**
- [ ] Parse offset value (0x00-0xFF maps to 0%-100% of sample length)
- [ ] C++ effect processor: override sample start point
- [ ] Existing start/end point system used
- [ ] Test: Drum sample with different offset values (kick → snare-like tail)
- [ ] Verify no clicks/pops at offset start

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

#### Slice E5: Kill Effect (K00)

**Goal:** Stop sample immediately

**Effect Behavior:**
```
K00 = stop voice immediately (no fade)
```

**DoD:**
- [ ] Kill effect recognized (no value needed, just K00)
- [ ] C++ effect processor: stop voice instantly
- [ ] Test: Long sample with K00 on step 8 (cuts off mid-sample)
- [ ] Verify no audio artifacts

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

#### Slice E6: Repeat Effect (R01-RFF)

**Goal:** Retrigger sample multiple times within step

**Effect Behavior:**
```
R01 = play once (no effect)
R02 = play twice per step
R04 = play 4 times per step
R10 = play 16 times per step (fast tremolo)
```

**DoD:**
- [ ] Parse repeat count (0x01-0xFF)
- [ ] C++ effect processor: schedule N note triggers within step
- [ ] Test: Kick drum with R04 (stutter effect)
- [ ] Test: Hi-hat with R10 (tremolo/roll)
- [ ] Even timing between repeats

**Implementation:**
```cpp
if (step.fx1Type == FX_REPEAT && step.fx1Value > 0x01) {
    int repeatCount = step.fx1Value;
    long stepDuration = getStepDurationInFrames(tempo);
    long repeatInterval = stepDuration / repeatCount;
    
    // Schedule N note triggers
    for (int i = 0; i < repeatCount; i++) {
        scheduleNote(frame + i * repeatInterval, sampleId, trackId, freq, vol);
    }
}
```

**Effort:** 2 days

---

**MILESTONE 2 COMPLETE:** TOP-5 effects working in phrase screen! 🎉

---

### MILESTONE 2.5: Copy/Paste System

**Goal:** Implement M8-style selection mode and clipboard workflow

**Duration:** 4-5 days

**Why in MVP?**
> Copy/paste is essential for efficient music production workflow. Users need to duplicate patterns, create variations, and organize phrases quickly. Without copy/paste, creating a full song becomes tedious.

---

#### Slice CP1: Selection Mode Infrastructure

**Goal:** Allow selecting multiple steps/rows in phrase/chain editors

**DoD:**
- [ ] SELECT+B enters selection mode on grid views
- [ ] D-pad expands/contracts selection while in selection mode
- [ ] Visual highlight shows selected area (inverted colors or border)
- [ ] B exits selection mode without copying
- [ ] Selection works in phrase editor (vertical and horizontal)
- [ ] Selection works in chain editor
- [ ] Selection state persists in cursor context
- [ ] Can select single item or multi-item ranges

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

#### Slice CP2: Copy/Paste Operations

**Goal:** Implement clipboard operations for selected data

**DoD:**
- [ ] B in selection mode copies selection and exits selection mode
- [ ] **SELECT+A pastes clipboard contents at cursor position** ✅ (CORRECTED)
- [ ] A+B cuts selection (copy + delete)
- [ ] Copy/paste phrase steps works correctly
- [ ] Copy/paste between different phrases works
- [ ] Copy/paste chain rows works
- [ ] Paste validates and adjusts to target context
- [ ] Clipboard persists across screen changes
- [ ] Can paste multiple times (clipboard stays populated)

**M8 Control Mapping (CORRECTED):**
```
On grid views (SONG, CHAIN, PHRASE):

SELECT+B          → Enter selection mode
                    (cursor freezes, selection starts at current position)

D-PAD (in select) → Expand/contract selection
                    (visual highlight updates in real-time)

B (in select)     → Copy selection to clipboard and exit selection mode
                    (status shows "Copied X items")

SELECT+A          → Paste clipboard contents at current cursor ✅ CORRECTED
                    (validates target, shows error if incompatible)

A+B               → Cut selection to clipboard
                    (deletes original, copies to clipboard)

SELECT alone      → Cancel selection mode without copying
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

#### Slice CP3: Clipboard Indicator

**Goal:** Show clipboard status in header row (persistent indicator)

**DoD:**
- [ ] Header shows clipboard contents when something is copied
- [ ] Format matches PocketTracker style (compact, readable)
- [ ] Updates immediately when copying
- [ ] Persists across screen changes
- [ ] Doesn't interfere with other header information
- [ ] Clears when clipboard is empty/invalidated

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
- [ ] Cursor navigates to FX1/FX2/FX3 columns
- [ ] A+UP/DOWN cycles effect types (NONE → ARP → OFFSET → VOL → KILL → REPEAT)
- [ ] A+LEFT/RIGHT increments/decrements value (00 → 01 → ... → FF)
- [ ] Effect displayed as hex: "A0C", "O80", "VFF", "K00", "R04"
- [ ] Visual feedback (cursor highlights current effect column)

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

#### Slice T2: Copy/Paste Workflow Testing

**Goal:** Ensure copy/paste feels natural and works reliably

**Test Cases:**
- [ ] Enter selection mode (SELECT+B)
- [ ] Expand selection in all 4 directions
- [ ] Copy selection (B in selection mode)
- [ ] Paste into same phrase (SELECT+A) ✅
- [ ] Paste into different phrase (SELECT+A) ✅
- [ ] Cut selection (A+B)
- [ ] Multiple pastes from same clipboard
- [ ] Clipboard indicator updates correctly
- [ ] Selection highlights correctly
- [ ] Edge cases: paste beyond bounds, empty clipboard

**DoD:**
- [ ] All test cases pass
- [ ] No crashes during copy/paste operations
- [ ] Visual feedback clear and helpful
- [ ] Works in both phrase and chain editors

**Effort:** 1 day

---

#### Slice T3: Effects Testing & Refinement

**Goal:** Ensure effects work reliably in all scenarios

**Test Cases:**
- [ ] Single effect per step works
- [ ] Multiple effects (FX1 + FX2 + FX3) work simultaneously
- [ ] Effects work in phrase playback
- [ ] Effects work in chain playback (with transpose)
- [ ] Effects work in song playback (8 tracks)
- [ ] Effects save/load correctly
- [ ] Edge cases: A00 (no arp), R01 (no repeat), K00 on empty step (no crash)

**DoD:**
- [ ] All test cases pass
- [ ] No audio glitches or clicks
- [ ] No crashes or hangs
- [ ] Performance impact acceptable (<5% CPU increase)

**Effort:** 2-3 days

---

#### Slice T4: Integration Testing

**Goal:** Verify effects and copy/paste work together

**Test Scenarios:**
- [ ] Copy phrase with effects, paste to different phrase
- [ ] Effects values preserved during copy/paste
- [ ] Can copy/paste effect-only changes (no note changes)
- [ ] Undo/clear effects in selection (future: undo/redo)

**DoD:**
- [ ] Effects + copy/paste integration solid
- [ ] No data corruption
- [ ] Clipboard correctly handles effect data

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

### Realistic Timeline (With Work-Life Balance)

**Assumptions:**
- ~3 hours per day most days
- Flexible breaks when needed
- Account for unexpected issues

**January 2025:**
- Week 1-2: Refactoring (Milestone 1)
- Week 3-4: Effects System (Milestone 2)

**February 2025:**
- Week 1: Copy/Paste System (Milestone 2.5)
- Week 2: Integration & Testing (Milestone 3)
- Week 3-4: Polish & Documentation (Milestone 4)

**Target:** Late February public beta release (8-10 weeks total)

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
- [ ] **User can apply effects (TOP-5 in phrase screen)** ⚠️
- [ ] **User can copy/paste phrase steps (selection mode)** ⚠️
- [ ] **User can copy/paste between different phrases** ⚠️
- [ ] **User can cut/delete selections with A+B** ⚠️

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
- [ ] App runs at stable framerate on target hardware - needs verification on Ayaneo

### Controls ✅
- [x] All D-pad directions work (cursor navigation)
- [x] A/B buttons work (confirm/cancel, value editing)
- [x] Start button works (play/stop toggle)
- [x] L/R shoulders work (screen navigation, modifiers)
- [x] Virtual controls work on touchscreen devices
- [ ] **Copy/paste controls work (SELECT+B, SELECT+A, A+B)** ⚠️
- [ ] **Selection mode works (visual highlight)** ⚠️
- [ ] **Clipboard indicator shows in header** ⚠️

### Usability ⚠️
- [ ] User can complete "hello world" song in under 5 minutes
- [ ] Navigation between screens is intuitive (no getting lost)
- [x] Status messages explain what happened (save success/fail, etc.)
- [ ] App doesn't crash on common user errors - needs testing

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
- v1.1 (2025-01-01): Added copy/paste milestone, corrected paste control (SELECT+A)
- v1.0 (2025-01-01): Initial MVP roadmap with vertical slices
