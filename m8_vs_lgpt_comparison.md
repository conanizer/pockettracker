# M8 vs LGPT Input System Comparison

## Executive Summary

**M8** uses a single SHIFT modifier with sequential button combinations.
**LGPT** uses dual modifiers (LT/RT shoulder buttons) with simpler combinations.
**PocketTracker** should use LGPT's dual-modifier approach with M8's advanced editing patterns.

---

## Button Inventory

### M8 Tracker (8 buttons total)
- **D-PAD**: 4 directions
- **EDIT**: Primary action (like A or Enter)
- **OPTION**: Secondary action (like B or Escape)
- **SHIFT**: Single modifier key
- **PLAY**: Playback control

### LGPT (LittleGPTracker) - Handheld Layout
- **D-PAD**: 4 directions
- **A**: Primary action (insert/confirm)
- **B**: Secondary action (cancel/back)
- **START**: Menu/playback
- **SELECT**: Context menu
- **LT** (Left Trigger): Left shoulder modifier
- **RT** (Right Trigger): Right shoulder modifier

### PocketTracker (Current Layout)
- **D-PAD**: 4 directions (WASD on keyboard)
- **A**: K key (primary action)
- **B**: J key (secondary action)
- **L**: U key (left shoulder/shift)
- **R**: I key (right shoulder/shift)
- **SELECT**: Left Shift
- **START**: Space

---

## Core Philosophy Differences

### M8: Sequential Combination System
- Uses **temporal sequences**: Press SHIFT, then OPTION, then EDIT
- Single modifier means more sequential presses
- Very context-aware actions (same buttons do different things per screen)

**Example:**
```
SHIFT + OPTION, then EDIT = clone chain
SHIFT + OPTION, then EDIT, EDIT = deep clone (chain + phrases)
SHIFT + OPTION (hold) + OPTION again = cycle selection modes
```

### LGPT: Dual-Modifier System
- Uses **simultaneous combinations**: LT + A, RT + direction
- Two modifiers allow more direct combos
- Often uses "opposite hand" logic (LT with right-side buttons, RT with left-side buttons)

**Example:**
```
A, A = insert next unused
LT + A = paste
LT + B = selection mode
RT + direction = navigate screens
LT + RT + SELECT = quit to project screen
```

---

## Detailed Action Comparison

### 1. BASIC EDITING

| Action | M8 | LGPT | Best Choice |
|--------|----|----|-------------|
| Insert value | EDIT on empty | A on empty | **Same** - A on empty |
| Increment | EDIT + UP | Not documented | **M8** - A + UP |
| Decrement | EDIT + DOWN | Not documented | **M8** - A + DOWN |
| Large increment | EDIT + RIGHT | Not documented | **M8** - A + RIGHT |
| Large decrement | EDIT + LEFT | Not documented | **M8** - A + LEFT |
| Delete value | EDIT + OPTION | Not documented | **M8** - A + B |

**Winner: M8's editing system is more sophisticated**

---

### 2. SELECTION & CLIPBOARD

| Action | M8 | LGPT | Best Choice |
|--------|----|----|-------------|
| Enter selection | SHIFT + OPTION | LT + B (tap) | **LGPT** - L + B simpler |
| Cycle selection | OPTION (while holding SHIFT) | LT + B (tap again) | **M8** - More modes |
| Copy selection | OPTION in selection | Release LT | **M8** - Explicit action |
| Cut selection | EDIT + OPTION | Not documented | **M8** - A + B |
| Paste | SHIFT + EDIT | LT + A | **LGPT** - L + A simpler |

**Winner: Hybrid - LGPT for entering selection, M8 for selection modes**

---

### 3. SPECIAL ACTIONS

| Action | M8 | LGPT | Best Choice |
|--------|----|----|-------------|
| Clone item | SHIFT + OPTION, then EDIT | LT + (B, A) | **LGPT** - Simpler combo |
| Deep clone | SHIFT + OPTION, then EDIT, EDIT | Not found | **M8** - Unique feature |
| Insert next unused | Not found | A, A | **LGPT** - Great feature |
| Reset to default | EDIT + OPTION | Not found | **M8** - Useful |

**Winner: M8 has more features, LGPT has simpler execution**

---

### 4. NAVIGATION

| Action | M8 | LGPT | Best Choice |
|--------|----|----|-------------|
| Navigate screens | SHIFT + direction | RT + direction | **LGPT** - Frees up SHIFT |
| Navigate chains | OPTION + UP/DOWN | Not documented | **M8** - Good pattern |
| Navigate instruments | OPTION + LEFT/RIGHT | Not documented | **M8** - Consistent |
| Jump ±16 | Not in global | LT + UP/DOWN (context) | **LGPT** - Useful |

**Winner: LGPT for screen nav, M8 for item nav**

---

### 5. PLAYBACK

| Action | M8 | LGPT | Best Choice |
|--------|----|----|-------------|
| Play/Stop | PLAY | START | **Same** - START |
| Play all tracks | SHIFT + PLAY | RT + START | **Both good** |
| Play from cursor | Not found | RT + START | **LGPT** - Very useful |
| Mute track | OPTION + SHIFT on track | Not documented | **M8** - Essential |
| Solo track | OPTION + PLAY on track | Not documented | **M8** - Essential |

**Winner: M8 has better live performance controls**

---

## Unique Features Analysis

### M8 Unique Features ✨
1. **Interpolation**: SHIFT + EDIT on selection fills between values
2. **Snapshots**: SHIFT + OPTION (create), SHIFT + EDIT (recall) for parameters
3. **Context help**: EDIT + UP/DOWN on effect columns shows help
4. **Mute/Solo system**: Complex track muting during playback
5. **Deep clone**: Clone with all references (SHIFT + OPTION, then EDIT, EDIT)
6. **Selection modes**: Column, row, or all (cycle with OPTION)

### LGPT Unique Features ✨
1. **Double-tap insert**: A, A inserts next unused item (very fast workflow)
2. **Opposite-hand logic**: LT with right buttons, RT with left (ergonomic)
3. **Jump to populated**: LT + UP/DOWN jumps to next non-empty row
4. **Tempo nudge**: LT + LEFT/RIGHT adjusts tempo in real-time
5. **Mode switching**: B + LEFT/RIGHT switches Song/Live modes
6. **Clone shorthand**: LT + (B, A) is faster than M8's sequence

---

## Proposed PocketTracker System

### Core Design Principles

1. **Use LGPT's dual-modifier foundation** (L and R buttons)
2. **Adopt M8's editing precision** (A + directions for steps)
3. **Support both sequential and simultaneous** combos
4. **Keep it learnable** (consistent patterns, not arbitrary)

---

### Button Mapping (Keyboard)

```
D-PAD:      W/A/S/D
A button:   K
B button:   J
L button:   U (left shoulder/shift)
R button:   I (right shoulder/shift)
SELECT:     Left Shift
START:      Space
```

---

### Proposed Input System

#### **TIER 1: Basic Actions (No modifiers)**

```
A (K)                    → Insert value on empty / Enter edit mode
A + UP                   → Increment by small step (+1)
A + DOWN                 → Decrement by small step (-1)
A + RIGHT                → Increment by large step (+16 or +12 for semitones)
A + LEFT                 → Decrement by large step (-16 or -12 for semitones)
A, A (double tap)        → Insert next unused (LGPT style)
B (J)                    → Cancel / Exit / Back
A + B                    → Delete / Clear value
SELECT                   → Quick delete (context-aware)
START                    → Play / Stop
```

#### **TIER 2: L Modifier (Edit & Clipboard)**

```
L + A                    → Paste (LGPT)
L + B                    → Enter selection mode (LGPT)
L + B (tap again)        → Cycle selection mode: column → row → all → exit (M8)
L + SELECT               → Copy selection (explicit)
L + A + B                → Cut selection
L + START                → Play all tracks from beginning (M8)
L + UP/DOWN              → Jump to next/prev populated row (LGPT)
L + LEFT/RIGHT           → Navigate to prev/next chain or phrase
```

#### **TIER 3: R Modifier (Navigation & Performance)**

```
R + UP/DOWN/LEFT/RIGHT   → Navigate between screens (LGPT)
R + A                    → Clone current item (simplified from LGPT's LT+(B,A))
R + A, A                 → Deep clone (with all references) (M8 style)
R + B                    → Reset value to default (M8)
R + START                → Play from current cursor position (LGPT)
R + SELECT               → Context menu / Options
```

#### **TIER 4: L + R Combinations (Advanced)**

```
L + R + SELECT           → Return to project/file screen (LGPT)
L + R + A                → Create snapshot (M8)
L + R + B                → Recall snapshot (M8)
L + R + START            → Panic (stop all audio, reset)
L + R + UP/DOWN          → Navigate ±16 chains/phrases/instruments (LGPT)
```

#### **TIER 5: Contextual Actions**

**In Selection Mode:**
```
A + UP/DOWN              → Interpolate between values (M8)
A + LEFT/RIGHT           → Randomize values (M8 phrase feature)
B                        → Exit selection mode
```

**In Song View:**
```
L + track column         → Mute track (M8)
R + track column         → Solo track (M8)
L + R + track            → Clear all mutes (M8)
```

**In Parameter Screens (Mixer/Effects):**
```
L + R + A                → Save snapshot (M8)
L + R + B                → Load snapshot (M8)
```

---

## Comparison Table: Hybrid System

| Feature | Source | Rationale |
|---------|--------|-----------|
| A + directions for editing | M8 | More precise control |
| Dual modifiers (L/R) | LGPT | More ergonomic, less sequences |
| A, A for next unused | LGPT | Fast workflow |
| Deep clone | M8 | Unique powerful feature |
| Selection mode cycling | M8 | More flexible |
| L + A for paste | LGPT | Simpler than SHIFT + EDIT |
| R + directions for screen nav | LGPT | Logical separation |
| Mute/Solo system | M8 | Essential for performance |
| Jump to populated | LGPT | Great for sparse patterns |
| Snapshots | M8 | Unique to M8 |

---

## Why This Hybrid Works

### ✅ **Advantages**

1. **Familiar to both M8 and LGPT users**
   - M8 users: Recognize editing patterns (A + directions)
   - LGPT users: Recognize modifier layout (L/R)

2. **Ergonomic**
   - LGPT's approach: Use opposite hands (L with right buttons, R with left)
   - Reduces hand strain on keyboard

3. **Discoverable**
   - L = "edit things" (paste, selection, clipboard)
   - R = "navigate and create" (screens, clone, play modes)
   - Consistent patterns reduce memorization

4. **Best of both worlds**
   - M8's editing precision and advanced features
   - LGPT's simpler modifier combinations
   - Adds unique improvements (double-tap, etc.)

### ⚠️ **Potential Issues**

1. **More complex than either system alone**
   - Solution: Progressive disclosure (basic → advanced)

2. **Keyboard has both hands on D-pad side**
   - Solution: Remap for keyboard (already done - U/I are comfortable)

3. **Need to track button states for double-tap**
   - Solution: Already possible with GenericInputHandler

---

## Implementation Priority

### Phase 1: Core Editing (Already Implemented) ✅
- A/B buttons
- A + directions for steps
- SELECT for delete

### Phase 2: L Modifier (Next)
- L + A for paste
- L + B for selection mode
- L + UP/DOWN for navigation

### Phase 3: R Modifier
- R + directions for screen navigation
- R + A for clone
- R + START for play from cursor

### Phase 4: Advanced Combos
- L + R combinations
- Double-tap detection (A, A)
- Contextual actions (selection interpolation, etc.)

### Phase 5: Live Performance
- Mute/solo system
- Snapshots
- Real-time parameter control

---

## Recommendation

**Use the LGPT approach (dual modifiers) with M8's editing patterns.**

This gives you:
- ✅ **Simpler combos** than M8's sequential presses
- ✅ **More features** than basic LGPT
- ✅ **Ergonomic** for both keyboard and hardware
- ✅ **Familiar** to tracker users
- ✅ **Expandable** for future features

The current `GenericInputHandler` is perfect for this - you just need to extend it to handle L/R modifiers and detect button combinations.

---

## Sources

- [M8 Tracker Shortcuts GitHub Gist](https://gist.github.com/devin-dominguez/587720c9ab71b2d9f3c4bd48d9c812ca)
- [M8 Key Shortcuts - ManualsLib](https://www.manualslib.com/manual/2290745/Dirtywave-M8.html?page=55)
- [M8 Tracker Shortcuts Website](https://sites.google.com/view/m8tracker/)
- [LGPT Reference Manual Wiki](http://wiki.littlegptracker.com/doku.php?id=lgpt%3Areference_manual)
- [LittleGPTracker Official Docs](https://www.littlegptracker.com/docs.php)
- [LGPT Quick Start Guide](http://wiki.littlegptracker.com/doku.php?id=lgpt%3Aquick_start_guide)
- [Piggy Tracker Quick Start](https://pyra-handheld.com/boards/threads/piggy-tracker-quick-start-guide.30006/)
