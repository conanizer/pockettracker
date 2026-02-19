# MVP Extension Pack 3: Modulation, Groove, Effects & Polish

**Target Duration:** 3 weeks (Late February - Mid March 2026)
**Prerequisites:** MVP Extension Pack 2 COMPLETE

---

## Overview

Extension Pack 3 adds deep sound design capabilities (modulation), timing flexibility (groove), powerful new effects, and critical UX improvements.

**5 Major Areas:**
1. Fixes & UX Updates (key repeat, volume fix, selection increment)
2. New Effects (REP rework, CHA, DEL, RND, RNL, TBL, THO, GRV)
3. Groove Screen (per-step tick configuration)
4. Modulation Screen (4 mod slots per instrument, C++ engine)
5. Selection Resampling (render song selection to new instrument)

---

## Phase 1: Fixes & UX Updates (Days 1-4)

### 1.1 Table Volume Range Fix

**Problem:** Table volume uses 0xFF as "empty/no change", making FF unusable as a volume value. Same issue that was already fixed for phrase references in chain screen.

**Solution:** Use -1 internally for "empty/no change", allow 00-FF as valid volume values.

**Files to change:**

**TrackerData.kt:**
- `TableRow.volume` default: `0xFF` → `-1` (empty = no change)
- Add migration: existing projects with volume=0xFF → volume=-1

**TableModule.kt:**
- Display: `-1` → "--", `0x00-0xFF` → hex string
- CursorContext: `hexByte(volume, emptyValue = -1, canDelete = true)`
- DELETE action: reset to -1 (not 0xFF)
- SET_VALUE: `coerceIn(0, 255)`

**native-audio.cpp:**
- Table volume processing: check for -1 (no change) instead of 0xFF
- JNI table loading: pass -1 sentinel through to C++

**ClipboardManager.kt:**
- Table row copy/paste: handle -1 volume correctly

### 1.2 FX Type Bidirectional Cycling

**Problem:** A+DOWN from FX_NONE (empty/first) can't reach end of list. FX type only cycles forward.

**Solution:** Make cycling wrap in both directions through `EFFECT_TYPES` list.

**Files to change:**

**InputController.kt (or CursorContext.kt):**
- `incrementValue()` for EFFECT_TYPE: when at index 0 and decrementing, wrap to last index
- `decrementValue()` for EFFECT_TYPE: when at last index and incrementing, wrap to index 0
- Verify both A+UP (increment) and A+DOWN (decrement) wrap correctly

### 1.3 Key Repeat System

**Problem:** No built-in key repeat. Each button press is atomic. Users must repeatedly tap for each step.

**Solution:** Add timer-based key repeat for D-PAD, A+DPAD, and B+DPAD combos.

**Behavior:**
- **Initial delay:** ~400ms after key down before first repeat
- **Repeat interval:** ~100ms (10 repeats/second)
- **Cancel:** On key up, cancel repeat timer
- **Applies to:**
  - D-PAD alone (cursor navigation)
  - A+DPAD (value increment/decrement)
  - B+DPAD (item cycling: previous/next phrase/chain/instrument/table)
- **Does NOT apply to:**
  - L+DPAD (screen navigation) - intentional single-press
  - R+DPAD (screen navigation) - intentional single-press
  - Button-only presses (A, B, START, SELECT)

**Implementation approach:**

**ButtonHandlers.kt:**
- Track key down timestamps per direction
- On key down: fire immediately, start repeat timer
- On key up: cancel repeat timer
- Detect held modifiers (A held, B held) to determine which combo is active
- Use `Handler.postDelayed()` or coroutine-based timer for repeats

**Alternative (more portable):**
- Add `KeyRepeatManager` class in core/logic/ with platform-agnostic timer interface
- Platform layer (MainActivity/ButtonHandlers) feeds key events
- Manager fires repeat callbacks at configured intervals

### 1.4 Selection Increment

**Problem:** Can't increment/decrement multiple selected values at once.

**Solution:** When selection is active and A+DPAD is pressed, apply increment/decrement to all selected cells in the active cursor column.

**Behavior:**
- Selection must be active (CELL/ROW/SCREEN mode)
- A+UP/DOWN: increment/decrement all selected values in the cursor's column by smallStep
- A+LEFT/RIGHT: increment/decrement by largeStep
- Each cell uses its own CursorContext for wrapping/clamping rules
- Only affects single column (column where cursor is)
- Works on: PHRASE, CHAIN, SONG, TABLE screens

**Files to change:**

**InputController.kt:**
- In A+DPAD handler: check if selection is active
- If active: iterate over selected rows in current column
- For each row: get CursorContext, apply increment/decrement
- Return list of changes to apply

**PhraseEditorModule.kt, ChainEditorModule.kt, SongEditorModule.kt, TableModule.kt:**
- `handleInput()`: handle batch SET_VALUE actions from selection increment
- Apply changes to each affected row

---

## Phase 2: New Effects (Days 5-10)

### Effect Code Assignments

| Effect | Code | Format | Context |
|--------|------|--------|---------|
| CHA | 0x04 | CHA XY | Phrase + Table |
| DEL | 0x05 | DEL XX | Phrase + Table |
| GRV | 0x07 | GRV XX | Phrase only |
| RND | 0x10 | RND XY | Phrase + Table |
| RNL | 0x11 | RNL XY | Phrase + Table |
| TBL | 0x14 | TBL XX | Phrase only |
| THO | 0x15 | THO XX | Table only |

**EffectProcessor.kt updates:**
- Add constants: `FX_CHA = 0x04`, `FX_DEL = 0x05`, `FX_GRV = 0x07`, `FX_RND = 0x10`, `FX_RNL = 0x11`, `FX_TBL = 0x14`, `FX_THO = 0x15`
- Add to `EFFECT_TYPES` list for UI cycling
- Add display names: "CHA", "DEL", "GRV", "RND", "RNL", "TBL", "THO"

### 2.1 REP Volume Ramping (Rework)

**Current behavior:** REP XX - full byte as retrig interval (R01-R0B sub-step, R0C+ multi-step)

**New M8-style behavior:** REP XY
- **Y = 0 (single retrig mode):** X = number of ticks to wait (0-F). R00 = cancel.
- **Y != 0 (volume ramping mode):** Y = retrig interval in ticks (1-F), X = volume change per retrig.
  - X = 0: no volume change (pure retrig)
  - X = 1-7: decrease volume each retrig (1=subtle, 7=aggressive)
  - X = 8-F: increase volume each retrig (8=subtle, F=aggressive)

**Breaking change:** Max retrig interval reduced from 0xFF (255 ticks) to 0x0F (15 ticks). Multi-step retriggers (R0C+) no longer supported. DEL effect covers similar use cases.

**Files to change:**

**EffectProcessor.kt:**
- Update `resolveStepParams()` to parse XY format
- Return new fields: `repeatTicInterval`, `repeatVolumeRamp` (signed)

**PlaybackController.kt:**
- Rework `processRepeatEffect()`:
  - Extract X and Y nibbles: `X = value shr 4`, `Y = value and 0x0F`
  - If Y == 0: simple retrig every X ticks (same as old sub-step mode)
  - If Y != 0: retrig every Y ticks, apply volume ramp per retrig
- Volume ramp calculation:
  - Per retrig: `volume *= (1.0 + rampFactor)` where rampFactor depends on X
  - X=0: factor=0, X=1-7: negative ramp, X=8-F: positive ramp
  - Clamp volume to 0.0-1.0

**TrackState:**
- Add: `repeatVolumeRamp: Int = 0` (the X nibble)
- Add: `repeatCurrentVolume: Float = 1.0f` (accumulates ramp)

### 2.2 DEL XX (Delay)

**In phrase:** Delays the entire row by XX ticks. The note and all effects on that row trigger XX ticks later.

**In table:** Delays the table playhead by XX ticks.

**Implementation:**

**PlaybackController.kt:**
- In `scheduleStepWithEffects()`: if DEL is present, add `delayTicks * framesPerTic` to the step's trigger frame
- All effects on the same row inherit the delay
- DEL 00 = no delay (same as no DEL)

**native-audio.cpp (for table DEL):**
- In table row processing: when DEL encountered, pause table advancement for XX ticks
- Add `tableDelayCounter` to voice table state

### 2.3 CHA XY (Chance)

**In phrase:** Probability gate. X = probability for left side (0-F), Y = probability for right side (0-F).
- 0 = never trigger (0%)
- F = always trigger (100%)
- Left side: note + instrument + volume + FX columns to the left of CHA
- Right side: FX columns to the right of CHA (and CHA's own column effects)

**In table:** XX = probability for everything to the left of CHA (00 = never, FF = always).

**Example:** CHA in FX2 column with value 1F:
- Left (note + FX1): ~6.7% chance (1/15)
- Right (FX3): 100% chance (F/F)

**Implementation:**

**EffectProcessor.kt / PlaybackController.kt:**
- During effect resolution, if CHA present:
  - Generate random float 0.0-1.0
  - Compare against X/15 for left, Y/15 for right
  - If below threshold, null out the respective effects/note
- Use `kotlin.random.Random` (seeded per step for deterministic behavior in renders)

**Phrase context:**
- CHA in FX1: left = {note, instrument, volume}, right = {FX2, FX3}
- CHA in FX2: left = {note, instrument, volume, FX1}, right = {FX3}
- CHA in FX3: left = {note, instrument, volume, FX1, FX2}, right = {}

**Table context:**
- Full byte 00-FF as probability for everything left of CHA column

### 2.4 RND XY (Random)

**Randomizes the previously active FX command** in phrase/table.

"Previously active" = the FX that was set in a prior step and is still persistent (REPEAT, ARPEGGIO, PITCH BEND, VIBRATO, etc.), OR the FX in the same step that was already processed.

**X** = downward randomization range (0 = none, F = full range)
**Y** = upward randomization range (0 = none, F = full range)

**Implementation:**

**PlaybackController.kt:**
- Track "last active FX" type and value per track
- When RND encountered: apply randomization to last active FX value
  - `randomizedValue = lastValue + random(-X_range, +Y_range)`
  - Range scaling: `X_range = (X / 15.0) * lastValue`, `Y_range = (Y / 15.0) * (maxValue - lastValue)`
- Re-apply the randomized FX value for this step

### 2.5 RNL XY (Randomize Left Command)

**Randomizes the FX command to the LEFT** in phrase/table.

- RNL in FX2: randomizes FX1's value
- RNL in FX3: randomizes FX2's value
- RNL in FX1 (first column): randomizes note and instrument (phrase) or note and velocity (table)

**X/Y** = same range semantics as RND.

**Implementation:**

**PlaybackController.kt:**
- During effect resolution, process columns left-to-right
- When RNL encountered: look at the FX column immediately to the left
- If RNL in FX1: randomize note (pitch ± range) and instrument number
- Apply same range calculation as RND

### 2.6 TBL XX (Table Set)

**Sets the table number** for the current instrument's note.

**XX** = table ID (00-FF). Overrides the instrument's `tableId` for this note.

**Implementation:**

**EffectProcessor.kt:**
- Add `tableOverride` field to ResolvedStepParams

**PlaybackController.kt:**
- When scheduling note: if TBL present, use TBL's value as tableId instead of instrument's tableId
- Pass overridden tableId to `scheduleNoteWithTable()`

### 2.7 THO XX (Table Hop)

**Jump to specific table position.** This is like HOP but specifically for tables.

**0X** = target row (0-F). Hops all columns when used inside a table.

**Implementation:**

**native-audio.cpp:**
- When THO encountered during table processing: set `tableRow = targetRow`
- All table columns jump to the target row simultaneously
- THO 00 at end of table section = loop back to row 0

### 2.8 GRV XX (Groove Assign)

**Assigns a groove to the current track.**

**XX** = groove ID (00-FF). Takes effect immediately.

**Implementation:**

**PlaybackController.kt:**
- Per-track groove ID stored in TrackState: `grooveId: Int = 0x00`
- When GRV encountered: set `trackState.grooveId = value`
- Groove lookup affects tics-per-step for subsequent steps on this track

---

## Phase 3: Groove Screen (Days 11-13)

### 3.1 Data Model

**TrackerData.kt additions:**

```kotlin
@Serializable
data class Groove(
    val id: Int,                          // 00-FF
    val rows: IntArray = IntArray(16) { -1 }  // -1 = empty, 01-FF = ticks for this step
) {
    companion object {
        fun createDefault(): Groove {
            // Default groove: row 0 = 0C (12 ticks), rest empty (loop from 0)
            val rows = IntArray(16) { -1 }
            rows[0] = 0x0C  // 12 ticks = standard speed
            return Groove(id = 0, rows = rows)
        }
    }
}
```

**Project additions:**
```kotlin
val grooves: Array<Groove> = Array(256) { Groove(id = it) }
```

**Groove behavior:**
- Empty rows (-1 / "--") = end of groove pattern, loop from row 0
- Row values = number of ticks for that phrase step
- Default groove 00: single row of 0C (12 ticks) = standard tempo
- Groove assignment per track via GRV effect

**Examples (in 12-tic system):**

| Pattern | Row 0 | Row 1 | Description |
|---------|-------|-------|-------------|
| Standard | 0C | -- | 12 ticks/step (normal) |
| 2x Speed | 06 | -- | 6 ticks/step (double speed) |
| Swing | 0E | 0A | 14+10=24 ticks per 2 steps (swing feel) |
| Triplets | 08 | 08 | 8+8+8=24 ticks per 3 steps, row 3=00 (stop) |

### 3.2 Groove Screen UI (GrooveModule.kt)

**Position:** Row 1, Column 2 in navigation grid (GROOVE, above PHRASE)

**Layout (620×392px):**
- Header: "GROOVE XX" + assigned tracks info
- 16 rows, 2 columns:
  - Column 0: Step number (0-F, read-only)
  - Column 1: Tick value (hex, 01-FF or -- for empty)

**Cursor navigation:**
- UP/DOWN: move between rows
- A+UP/DOWN: increment/decrement tick value by 1
- A+LEFT/RIGHT: increment/decrement by 16
- A+B: delete (set to -1 / empty)
- B+LEFT/RIGHT: navigate to previous/next groove

**Display:**
- Active row highlighted during playback (like phrase/table)
- Empty rows shown as "--" (dimmed)
- Row 0 of groove 00 shows default 0C

### 3.3 Playback Integration

**PlaybackController.kt changes:**

**TrackState additions:**
```kotlin
var grooveId: Int = 0x00        // Current groove for this track
var grooveRow: Int = 0          // Current position in groove
```

**Step timing modification:**
- Replace constant `TICS_PER_STEP` with per-step lookup:
```kotlin
fun getTicsForStep(trackId: Int): Int {
    val groove = project.grooves[trackState.grooveId]
    val ticsValue = groove.rows[trackState.grooveRow]

    // Advance groove row
    trackState.grooveRow++
    if (trackState.grooveRow >= 16 || groove.rows[trackState.grooveRow] == -1) {
        trackState.grooveRow = 0  // Loop
    }

    return if (ticsValue == -1) TICS_PER_STEP else ticsValue
}
```

**Frame calculation:**
- `framesPerStep` becomes dynamic: `framesPerTic * getTicsForStep(trackId)`
- Each step may have a different duration
- Lookahead buffer must account for variable step lengths

**Interaction with TIC effect:**
- Groove controls PHRASE step duration (how long each phrase row takes)
- TIC controls TABLE tick rate (how fast table rows advance within a phrase step)
- They are independent: groove changes step timing, TIC changes table sub-timing

---

## Phase 4: Modulation Screen & Engine (Days 14-20)

### 4.1 Data Model

**TrackerData.kt additions:**

```kotlin
// Modulation types
enum class ModType(val displayName: String) {
    NONE("---"),
    AHD("AHD ENV"),
    ADSR("ADSR ENV"),
    DRUM("DRUM ENV"),
    LFO("LFO"),
    TRIG("TRIG ENV"),
    TRACKING("TRACKING")
}

// Modulation destinations
enum class ModDest(val displayName: String) {
    NONE("---"),
    VOLUME("VOLUME"),
    PAN("PAN"),
    PITCH("PITCH"),
    FINE_PITCH("FINE"),
    FILTER_CUTOFF("CUTOFF"),
    FILTER_RES("RES"),
    SAMPLE_START("SMPSTRT"),
    MOD_AMT("MOD AMT"),      // Modulate neighbor mod's amount
    MOD_RATE("MOD RATE"),     // Modulate neighbor mod's rate
    MOD_BOTH("MOD BOTH")      // Both amount and rate
}

// Unified modulation slot (parameters vary by type)
@Serializable
data class ModSlot(
    var type: ModType = ModType.NONE,
    var dest: ModDest = ModDest.NONE,
    var amount: Int = 0x80,         // 00-FF (80 = center/default)

    // Envelope params (AHD, ADSR, DRUM, TRIG)
    var attack: Int = 0x00,         // 00-FF (ticks)
    var hold: Int = 0x00,           // 00-FF (ticks) - AHD, TRIG
    var decay: Int = 0x00,          // 00-FF (ticks)
    var sustain: Int = 0x80,        // 00-FF - ADSR only
    var release: Int = 0x00,        // 00-FF (ticks) - ADSR only
    var peak: Int = 0x80,           // 00-FF - DRUM only
    var body: Int = 0x00,           // 00-FF (ticks) - DRUM only

    // LFO params
    var oscShape: Int = 0x00,       // Shape index (TRI, SIN, RAMP_DN, etc.)
    var trigMode: Int = 0x00,       // 0=FREE, 1=RETRIG, 2=HOLD, 3=ONCE
    var frequency: Int = 0x40,      // 00-FF (rate)

    // TRIG/TRACKING source
    var source: Int = 0x00          // Instrument ID (00-7F) or track (80-87)
                                    // TRACKING: 0=NOTE, 1=VELOCITY, 2=VEL_TAKEOVER
    // TRACKING range
    var lowValue: Int = 0x00,       // 00-FF
    var highValue: Int = 0xFF       // 00-FF
)
```

**Instrument additions:**
```kotlin
data class Instrument(
    // ... existing fields ...
    var modSlots: Array<ModSlot> = Array(4) { ModSlot() }
)
```

**LFO Shapes (index mapping):**
```
0x00: TRI       (Triangle)
0x01: SIN       (Sine)
0x02: RAMP_DN   (Ramp Down)
0x03: RAMP_UP   (Ramp Up)
0x04: EXP_DN    (Exponential Down)
0x05: EXP_UP    (Exponential Up)
0x06: SQU_DN    (Square Down)
0x07: SQU_UP    (Square Up)
0x08: RANDOM    (Random)
0x09: DRUNK     (Random Walk)
0x0A-0x13: Tick-rate variants of above (TRI_T, SIN_T, etc.)
```

### 4.2 Modulation Screen UI (ModulationModule.kt)

**Position:** Row 1, Column 3 in navigation grid (MODS, above INSTRUMENT)
**Navigation:** R+UP from instrument screen

**Layout (620×392px):**

Header: "MODS" + instrument name/number

4 display columns: PARAM_NAME_1 | VALUE_1 | PARAM_NAME_2 | VALUE_2

Rows layout (MOD1+MOD2 paired, MOD3+MOD4 paired):
```
Row 00: [MOD1 TYPE: AHD    ] [MOD2 TYPE: LFO    ]
Row 01: [MOD1 DEST: VOLUME ] [MOD2 DEST: CUTOFF ]
Row 02: [MOD1 AMT:  80     ] [MOD2 AMT:  40     ]
Row 03: [MOD1 ATK:  10     ] [MOD2 OSC:  TRI    ]
Row 04: [MOD1 HOLD: 20     ] [MOD2 TRIG: RETRIG ]
Row 05: [MOD1 DEC:  30     ] [MOD2 FREQ: 08     ]
Row 06: [--- spacer ---                          ]
Row 07: [MOD3 TYPE: ADSR   ] [MOD4 TYPE: ---    ]
Row 08: [MOD3 DEST: PAN    ] [MOD4 DEST: ---    ]
Row 09: [MOD3 AMT:  80     ] [MOD4 AMT:  --     ]
Row 10: [MOD3 ATK:  10     ] [MOD4 param: --     ]
Row 11: [MOD3 DEC:  20     ] [MOD4 param: --     ]
Row 12: [MOD3 SUS:  80     ] [MOD4 param: --     ]
Row 13: [MOD3 REL:  30     ] [MOD4 param: --     ]
```

**Note:** ADSR and TRIG need 7 rows (TYPE+DEST+AMT+4 type-specific). Other types need 6 rows. When ADSR/TRIG is selected, that pair gets 7 rows instead of 6, spacer adjusts.

**Parameter display adapts to mod type:**
- NONE: Only TYPE row shown, rest empty
- AHD: TYPE, DEST, AMT, ATK, HOLD, DEC (6 rows)
- ADSR: TYPE, DEST, AMT, ATK, DEC, SUS, REL (7 rows)
- DRUM: TYPE, DEST, AMT, PEAK, BODY, DEC (6 rows)
- LFO: TYPE, DEST, AMT, OSC, TRIG, FREQ (6 rows)
- TRIG: TYPE, DEST, AMT, SRC, ATK, HOLD, DEC (7 rows)
- TRACKING: TYPE, DEST, SRC, LVAL, HVAL (5 rows)

**Cursor navigation:**
- UP/DOWN: move between rows
- LEFT/RIGHT: switch between MOD1↔MOD2 (rows 0-6) or MOD3↔MOD4 (rows 7-13)
- A+UP/DOWN: edit value (small step)
- A+LEFT/RIGHT: edit value (large step)
- A+B: reset to default
- B+LEFT/RIGHT: navigate to prev/next instrument

**Shortcuts:**
- START: play/stop phrase
- A+START: preview instrument

### 4.3 C++ Modulation Engine

**native-audio.cpp additions:**

**Per-voice modulation state:**
```cpp
struct ModulatorState {
    int type;           // ModType enum
    int dest;           // ModDest enum
    float amount;       // Normalized 0.0-1.0 (from 00-FF)

    // Envelope state
    int stage;          // 0=idle, 1=attack, 2=hold/decay/sustain, 3=decay/release, 4=done
    float envValue;     // Current envelope output (0.0-1.0)
    int tickCounter;    // Ticks elapsed in current stage

    // Stage parameters (in ticks)
    int attackTicks;
    int holdTicks;
    int decayTicks;
    int sustainLevel;   // 0-255 for ADSR
    int releaseTicks;   // ADSR only
    int peakShape;      // DRUM only
    int bodyTicks;      // DRUM only

    // LFO state
    float lfoPhase;     // 0.0 - 1.0
    int oscShape;       // Shape index
    int trigMode;       // FREE/RETRIG/HOLD/ONCE
    float lfoFrequency; // Cycles per step (derived from FREQ param)

    // TRIG state
    int trigSource;     // Source instrument/track
    bool triggered;     // Was triggered this frame

    // TRACKING state
    int trackSrc;       // NOTE/VELOCITY/VEL_TAKEOVER
    int lowVal;
    int highVal;

    // Output
    float output;       // Current modulation output (-1.0 to +1.0)
};

// Add to Voice struct:
ModulatorState modulators[4];

// Base parameter values (set on note trigger, before modulation)
float baseVolume;
float basePan;
float basePitch;        // Semitones offset
float baseFilterCut;
float baseFilterRes;
float baseSampleStart;
```

**Modulation processing (per audio frame, per voice):**
```
1. For each active modulator (type != NONE):
   a. Advance envelope/LFO state by tick fraction
   b. Calculate raw output (0.0-1.0 for envelopes, -1.0 to +1.0 for LFO)
   c. Scale by amount
   d. Store in modulator.output

2. For each destination, sum all modulator outputs targeting it:
   float volMod = 0, panMod = 0, pitchMod = 0, cutMod = 0, resMod = 0, startMod = 0;
   for each modulator:
       switch(dest):
           VOLUME: volMod += output
           PAN: panMod += output
           PITCH: pitchMod += output (in semitones)
           FINE_PITCH: pitchMod += output * 0.01 (cents)
           FILTER_CUTOFF: cutMod += output
           FILTER_RES: resMod += output
           SAMPLE_START: startMod += output
           MOD_AMT/RATE/BOTH: apply to neighbor modulator

3. Apply modulated values:
   voice.volume = clamp(baseVolume + volMod, 0.0, 1.0)
   voice.pan = clamp(basePan + panMod, 0.0, 1.0)
   pitchOffset += pitchMod  (added to existing pitch effects)
   voice.filterCut = clamp(baseFilterCut + cutMod * 255, 0, 255)
   voice.filterRes = clamp(baseFilterRes + resMod * 255, 0, 255)
   // Recalculate biquad coefficients if filter params changed
```

**Envelope Generators:**

**AHD:** Attack (ramp 0→1), Hold (stay at 1), Decay (ramp 1→0)
- Tick-based: each stage duration in ticks (synced to tempo)
- One-shot: runs once on note trigger

**ADSR:** Attack (0→1), Decay (1→sustain), Sustain (hold at level), Release (sustain→0)
- Sustain holds until note OFF (or KILL)
- Release triggered by note OFF event
- Need to add note-off tracking per voice

**DRUM:** Sharp peak transient → body hold → decay
- PEAK controls initial spike shape (fast attack, immediate peak)
- BODY = hold time after peak
- DEC = decay time from body level to 0

**LFO:**
- Shape function: `lfoValue = shapeFunction(phase)` where phase cycles 0→1
- Phase advance per tick: `1.0 / frequencyInSteps`
- Trigger modes:
  - FREE: Phase continues across notes, never resets
  - RETRIG: Reset phase to 0 on each note trigger
  - HOLD: Stop at end of first cycle, hold last value
  - ONCE: Play through once, return to 0
- Tick-rate shapes (suffix T): frequency in ticks instead of steps

**TRIG Envelope:**
- Same as AHD but triggered by external source
- Bipolar amount (can go negative)
- Source monitoring: check if source instrument/track triggered this frame

**TRACKING:**
- On note trigger: read source value (note MIDI number or velocity)
- Map to output: `output = lerp(lowVal, highVal, sourceNormalized)`
- Static value (doesn't change until next note)

**JNI Interface:**

New JNI functions:
```
setModulation(voiceId/trackId, slotIndex, type, dest, amount, params...)
clearModulation(voiceId/trackId, slotIndex)
triggerNoteOff(trackId)  // For ADSR release stage
```

**Kotlin bridge (IAudioBackend.kt):**
```kotlin
fun setModulation(trackId: Int, slot: Int, type: Int, dest: Int,
                  amount: Int, params: IntArray)
fun clearModulation(trackId: Int, slot: Int)
fun triggerNoteOff(trackId: Int)
```

### 4.4 Mod-to-Mod Routing

When DEST is set to MOD AMT, MOD RATE, or MOD BOTH:
- Mod 1 affects Mod 2
- Mod 2 affects Mod 3
- Mod 3 affects Mod 4
- Mod 4 affects Mod 1

**MOD AMT:** Scales the target mod's amount (0%-100%)
**MOD RATE:** Multiplies the target mod's time parameters (speed up/slow down)
**MOD BOTH:** Applies both amount scaling and rate multiplication

**Implementation:**
- Process modulators in order 1→2→3→4
- When mod-to-mod detected, modify the NEXT modulator's amount/rate before processing it
- Circular dependency (mod 4 → mod 1): process in two passes if needed

### 4.5 Integration with PlaybackController

**PlaybackController.kt changes:**

On note trigger:
1. Look up instrument's modSlots
2. Convert ModSlot parameters to C++ format
3. Call `setModulation()` for each active slot
4. Set base parameter values on the voice

On note off (for ADSR release):
1. Call `triggerNoteOff(trackId)`
2. ADSR modulators transition to release stage

---

## Phase 5: Selection Resampling (Days 18-21)

### 5.1 Trigger: Double Tap A in Song Selection

**SongEditorModule.kt / InputController.kt:**

- During selection mode in SONG screen, detect double-tap A (within 300ms)
- On double-tap A: show confirmation dialog

**Dialog:**
```
RESAMPLE SELECTION?
> YES
  NO
```

- A/DPAD to select YES/NO
- A to confirm
- B to cancel (same as NO)

### 5.2 Rendering Selected Tracks

**RenderController.kt additions:**

```kotlin
fun renderSelection(
    project: Project,
    startRow: Int,          // Song row start
    endRow: Int,            // Song row end (inclusive)
    selectedTracks: Set<Int>, // Which tracks to render (0-7)
    tempo: Int,
    onProgress: (Float) -> Unit
): FloatArray  // Stereo interleaved samples
```

**Behavior:**
- Render only selected tracks (respect track solo/mute implied by selection)
- Apply full mixer settings (track volumes, panning, master volume)
- Stereo output (same as WAV export from project screen)
- Use same playback scheduling as song mode (with effects, tables, grooves)
- Progress callback for UI feedback

### 5.3 WAV Output

**Output path:** `Documents/PocketTracker/Samples/Resampled/`

**Filename:** `Resample_001.wav`, `Resample_002.wav`, ... (auto-increment, check existing files)

**Format:** 16-bit stereo WAV, 44100 Hz (same as WAV export)

**WavWriter.kt** already handles this format - reuse existing code.

### 5.4 New Instrument Creation

After WAV render completes:
1. Find first empty instrument slot (sampleId == -1)
2. Load rendered WAV as a new sample
3. Create instrument with default settings:
   - Root: C-4
   - Volume: 0xFF
   - Pan: 0x80 (center)
   - All other params: default
4. Set as "default instrument" for new note entry
5. Show status message: "RESAMPLED TO INST XX"

**InstrumentController.kt additions:**
```kotlin
fun createResampledInstrument(wavPath: String): Int  // Returns instrument ID
```

---

## Implementation Order & Timeline

```
Week 1 (Days 1-7):
├── Phase 1: Fixes & UX (Days 1-4)
│   ├── 1.1 Table volume range fix
│   ├── 1.2 FX bidirectional cycling
│   ├── 1.3 Key repeat system
│   └── 1.4 Selection increment
│
└── Phase 2 Start: New Effects (Days 5-7)
    ├── 2.1 REP volume ramping rework
    ├── 2.2 DEL effect
    └── 2.3 CHA effect

Week 2 (Days 8-14):
├── Phase 2 Finish: New Effects (Days 8-10)
│   ├── 2.4 RND effect
│   ├── 2.5 RNL effect
│   ├── 2.6 TBL effect
│   ├── 2.7 THO effect
│   └── 2.8 GRV effect
│
├── Phase 3: Groove Screen (Days 11-13)
│   ├── 3.1 Groove data model
│   ├── 3.2 GrooveModule UI
│   └── 3.3 Playback integration
│
└── Phase 4 Start: Modulation (Day 14)
    ├── 4.1 Modulation data model
    └── 4.2 ModulationModule UI (start)

Week 3 (Days 15-21):
├── Phase 4 Finish: Modulation (Days 15-19)
│   ├── 4.2 ModulationModule UI (finish)
│   ├── 4.3 C++ modulation engine
│   ├── 4.4 Mod-to-mod routing
│   └── 4.5 PlaybackController integration
│
└── Phase 5: Selection Resampling (Days 19-21)
    ├── 5.1 Double-tap A detection + dialog
    ├── 5.2 Selection rendering
    ├── 5.3 WAV output
    └── 5.4 New instrument creation
```

---

## Data Model Migration

**Backward compatibility:** Projects saved before Extension Pack 3 need migration:

1. **Table volume:** `0xFF` → `-1` (empty marker change)
2. **Groove:** Default groove 00 created with row 0 = 0x0C
3. **Modulation:** Empty ModSlot arrays added to instruments
4. **REP effect:** Existing REP values may need interpretation adjustment
   - Old R06 (retrig every 6 ticks) → New R60 (Y=0, X=6, retrig every 6 ticks)
   - Migration function in FileController

**Project version bump:** Add version field to Project for future migrations.

---

## Files Created/Modified Summary

### New Files
- `GrooveModule.kt` - Groove screen UI
- `ModulationModule.kt` - Modulation screen UI
- `KeyRepeatManager.kt` - Key repeat timer logic (core/logic/)

### Major Modifications
- `TrackerData.kt` - Groove, ModSlot, ModType, ModDest, Instrument.modSlots
- `EffectProcessor.kt` - 7 new effect types, updated EFFECT_TYPES list
- `PlaybackController.kt` - Groove integration, REP rework, CHA/DEL/RND/RNL processing
- `InputController.kt` - Key repeat, selection increment, FX cycling fix
- `ButtonHandlers.kt` - Key repeat event handling
- `native-audio.cpp` - Modulation engine, THO table hop, DEL table delay, volume fix
- `TableModule.kt` - Volume range fix (-1 empty)
- `SongEditorModule.kt` - Resampling trigger (double-tap A)
- `RenderController.kt` - Selection rendering
- `InstrumentController.kt` - Resampled instrument creation
- `CursorContext.kt` - Updated factory methods
- `NavigationMapModule.kt` - Ensure GROOVE and MODS screens navigate correctly
- `IAudioBackend.kt` - New modulation JNI functions
- `OboeAudioBackend.kt` - JNI bridge for modulation

---

## Testing Checklist

### Phase 1 Tests
- [ ] Table volume cycles 00→01→...→FE→FF→00 (full range)
- [ ] Table volume A+B sets to "--" (empty, stored as -1)
- [ ] FX type: A+DOWN from NONE wraps to last effect (PVX)
- [ ] FX type: A+UP from last effect wraps to NONE
- [ ] Key repeat: hold D-PAD UP, cursor moves continuously after delay
- [ ] Key repeat: hold A+RIGHT, value increments continuously
- [ ] Key repeat: hold B+RIGHT, cycles through phrases/chains
- [ ] Key repeat: release stops repeating immediately
- [ ] Selection: select 3 notes, A+UP increments all 3 by one semitone

### Phase 2 Tests
- [ ] REP 03: retrig every 3 ticks (old sub-step behavior preserved for Y=0, X=3)
- [ ] REP 31: decrease volume, retrig every 1 tick
- [ ] REP F3: increase volume aggressively, retrig every 3 ticks
- [ ] DEL 06: note delayed by 6 ticks (half step)
- [ ] CHA F0: note always plays, effects never trigger
- [ ] CHA 0F: note never plays, effects always trigger
- [ ] RND 0F: randomize previous FX upward only
- [ ] RNL in FX1: randomizes note pitch
- [ ] TBL 05: overrides instrument's table with table 05
- [ ] THO 00: table jumps to row 0

### Phase 3 Tests
- [ ] Groove 00 default: all steps play at normal speed
- [ ] Swing groove: alternating long/short steps
- [ ] 2x speed groove: all steps half duration
- [ ] GRV effect: changes track groove mid-playback
- [ ] Groove + TIC: independent (groove affects phrase, TIC affects table)

### Phase 4 Tests
- [ ] AHD envelope on volume: attack ramp up, hold, decay ramp down
- [ ] ADSR on filter cutoff: full envelope cycle with sustain
- [ ] LFO on pitch: audible vibrato at various shapes/speeds
- [ ] LFO RETRIG vs FREE: retrig resets phase, free continues
- [ ] DRUM envelope: sharp transient on percussive sounds
- [ ] TRACKING NOTE→CUTOFF: higher notes = brighter filter
- [ ] Mod-to-mod: MOD1 (LFO) → MOD AMT affects MOD2 depth
- [ ] Multiple mods on same destination: outputs sum correctly

### Phase 5 Tests
- [ ] Song selection → double-tap A → dialog appears
- [ ] B cancels dialog
- [ ] YES renders selected tracks to stereo WAV
- [ ] WAV saved to Resampled/ folder with auto-increment name
- [ ] New instrument created in first empty slot
- [ ] New instrument has C-4 root, default settings
- [ ] Rendered audio matches live playback quality

---

## Risk Assessment

| Risk | Impact | Mitigation |
|------|--------|------------|
| C++ modulation engine complexity | High | Start with AHD (simplest), iterate |
| REP format change breaks existing songs | Medium | Migration function + version check |
| Key repeat timing feels wrong | Low | Adjustable delay/interval constants |
| Groove + effects interaction bugs | Medium | Extensive testing with combined features |
| 3-week timeline tight | Medium | Modulation UI can ship before full C++ engine |

---

**Version:** 1.0
**Created:** 2026-02-18
**Status:** PLANNING
