# UI Scaling Refactor Plan
## 320×240 Base Canvas + 7×7 Bitmap Font

**Status:** Planning
**Branch:** `claude/ui-scaling-refactor-plan-ZIsnd`

---

## Why This Refactor?

**Problem:** The current 640×480 design canvas with integer-only scaling leaves many devices under-served:

| Device | Resolution | Current scale | Displayed | Gap |
|--------|-----------|--------------|-----------|-----|
| Miyoo Flip | 640×480 | 1× | 640×480 | none |
| TrimUI Brick | 1024×768 | 1× | 640×480 | 384×288 wasted |
| Phone landscape | 1920×1080 | 2× | 1280×960 | letterbox |
| Phone portrait | 1440×2560 | 2× | 1280×960 | lots wasted |

TrimUI Brick and most phones cannot use scale=2 (would need 1280×960, which doesn't fit), so they're stuck at 1× — the same apparent size as the tiny Miyoo Flip.

**Solution:** Halve the design canvas to **320×240**. Now far more integer scale steps become available:

| Device | Resolution | New scale | Displayed | Result |
|--------|-----------|----------|-----------|--------|
| Miyoo Flip | 640×480 | 2× | 640×480 | same as before |
| TrimUI Brick | 1024×768 | 3× | 960×720 | **1.5× bigger!** |
| Phone landscape | 1920×1080 | 4× | 1280×960 | crisp pixel-perfect |
| Phone portrait | 1440×2560 | 4× | 1280×960 | fits nicely |

**Font bonus:** Switching from 5×5 to **7×7** bitmap font gives 49 pixels per glyph (vs 25 for 5×5), enabling far more expressive custom font designs. The 7×7 grid also has a true center pixel at (3,3), which 5×5 shares but 6×6 does not.

---

## The Rendering Model

Current system:
```
Design canvas: 640×480
Font: 5×5 bitmap × fontScale=3 → 15px chars
Device scale: min(deviceW/640, deviceH/480)  [integer]
On Miyoo Flip: chars appear 15px on screen
```

New system:
```
Design canvas: 320×240
Font: 7×7 bitmap × fontScale=1 → 7px chars in design space
Device scale: min(deviceW/320, deviceH/240)  [integer]
On Miyoo Flip (×2): chars appear 7×2 = 14px on screen
On TrimUI Brick (×3): chars appear 7×3 = 21px on screen
```

The design canvas is the "map" — all coordinates are halved. The device scale multiplies everything to physical pixels. The `drawBitmapChar` function's pixel draw size becomes `scale × fontScale = scale × 1 = scale` — one design pixel per font pixel.

---

## New Global Constants

### Canvas (PixelPerfectRenderer.kt)
```kotlin
DESIGN_WIDTH_PX  = 320   // was 640
DESIGN_HEIGHT_PX = 240   // was 480
SCREEN_SPACER    = 3     // was 6
SIDE_SPACER      = 5     // was 10
```

### Font (per-module and in drawBitmapChar)
```kotlin
CHAR_BASE    = 7    // was 5 (new font grid size)
FONT_SCALE   = 1    // was 3 (7×7 × 1 = 7px in design space)
CHAR_SPACING = 1    // was 2
CHAR_ADVANCE = 8    // was 17 (7+1 spacing)
ROW_HEIGHT   = 10   // was 21
TEXT_PADDING = 1    // was 3
```

### Layout Stack (verified to sum to 240px)
```
y=3:   OscilloscopeModule   310×35   (was 620×70)
y=41:  Editor modules       255×196  (was 510×392)
       (3px bottom gap to 240)
```

---

## Row Count: Why 16 Stays 16

16 rows represents **one bar of 16th notes** — this is intentional and must be preserved.

With the new constants:
```
Editor height: 196px
Layout: header(10) + spacer(7) + col_header(10) + 16 rows × 10 = 187px used
Remaining: 196 - 187 = 9px → bottom padding (empty space inside module)
```

The code already hardcodes 16 steps per phrase (`for (step in 0..15)`), so no logic changes needed. The 9px of bottom padding just appears as extra breathing room at the bottom of the editor area.

---

## Module Dimension Changes

| Module | Old (w×h) | New (w×h) | Notes |
|--------|-----------|-----------|-------|
| OscilloscopeModule | 620×70 | 310×35 | Simple halving |
| PhraseEditorModule | 510×392 | 255×196 | See column layout below |
| ChainEditorModule | 510×392 | 255×196 | |
| SongEditorModule | 510×392 | 255×196 | |
| InstrumentModule | 510×392 | 255×196 | |
| ProjectModule | 510×392 | 255×196 | |
| TableModule | 510×392 | 255×196 | |
| MixerModule | 620×392 | 310×196 | See meter layout below |
| FileBrowserModule | 640×480 | 320×240 | Full canvas |
| NavigationMapModule | 115×105 | 80×60 | ⚠️ Special case |

### NavigationMapModule — Special Case

Cannot be strictly halved: cells need to fit 2-char labels. At halved size (57×52px), each of 5 cells would be ~11px wide — too narrow for 2 chars at 8px advance.

Solution: Use **80×60px** with 16×12 cells:
```kotlin
// NavigationMapModule
override val width  = 80     // was 115
override val height = 60     // was 105
CELL_WIDTH  = 16             // was 23 (fits 2 chars: 2×8=16px exactly)
CELL_HEIGHT = 12             // was 21 (fits 7px char + 5px padding)

// Position in TrackerLayout:
navX = 320 - 80 - 5 = 235   // was 515
navY = 240 - 60 - 3 = 177   // was 369
```

---

## Phrase Editor Column Layout

All column X-offsets halved. **All 3 FX columns still fit** in 255px — no columns removed, no horizontal scrolling needed.

```kotlin
// PhraseEditorModule - column positions from module left edge
var colX = x + 5                              // was x+10
val stepX     = colX; colX += 15 + 5         // was 30+10 (2 chars)
val noteX     = colX; colX += 22 + 10        // was 45+20 (3 chars)
val volX      = colX; colX += 15 + 7         // was 30+15 (2 chars)
val instX     = colX; colX += 15 + 7         // was 30+15 (2 chars)
val fx1NameX  = colX; colX += 22 + 5         // was 45+10 (3 chars: ARP, KIL, ---)
val fx1ValueX = colX; colX += 15 + 7         // was 30+15 (2 chars: 00-FF)
val fx2NameX  = colX; colX += 22 + 5         // was 45+10
val fx2ValueX = colX; colX += 15 + 7         // was 30+15
val fx3NameX  = colX; colX += 22 + 5         // was 45+10
val fx3ValueX = colX                          // ends at ~241px, within 255px ✓
```

Verification:
- `stepX` = 5, `noteX` = 25, `volX` = 57, `instX` = 79, `fx1NameX` = 101,
  `fx1ValueX` = 128, `fx2NameX` = 150, `fx2ValueX` = 177, `fx3NameX` = 199,
  `fx3ValueX` = 226 → ends at ~240px within 255px module ✓

---

## Mixer Module Constants

```kotlin
// MixerModule
override val width   = 310   // was 620
override val height  = 196   // was 392
METER_WIDTH    = 15          // was 30
METER_HEIGHT   = 100         // was 200
METER_SPACING  = 28          // was 55 (27.5 → 28, rounded to even)
FIRST_METER_X  = 5           // was 10
// 9 tracks × 28px = 252 + 5 offset = 257px < 310px ✓
```

---

## Font File: BitmapFont7x7.kt

### Phase 1 — Pad existing 5×5 glyphs to 7×7

Fastest path to working code: add 1 zero-row at top, 1 zero-row at bottom, and 1 zero-bit on each side of every row. Glyphs look identical to current, with 1px border built into the bitmap.

```kotlin
// Current 5×5 '0':
'0' to byteArrayOf(0b11111, 0b10001, 0b10001, 0b10001, 0b11111)

// Expanded to 7×7 (add 0b0 row top/bottom, shift bits left + pad right):
'0' to byteArrayOf(
    0b0000000,  // top padding row
    0b0111110,  // original row 1: 0b11111 → shifted: 0b0_11111_0
    0b0100010,  // original row 2: 0b10001 → shifted: 0b0_10001_0
    0b0100010,
    0b0100010,
    0b0111110,
    0b0000000,  // bottom padding row
)
```

Conversion rule for each row `r` in `[0..4]`:
```kotlin
newRow[r + 1] = (oldRow[r].toInt() shl 1).toByte()  // shift left 1 bit, right bit is already 0
// newRow[0] = 0, newRow[6] = 0
```

This can be automated for all ~95 characters with a small utility function.

### Phase 2 — Proper 7×7 glyph designs (future)

Once the system works, individual glyphs can be replaced with hand-crafted 7×7 designs that use the full pixel grid for richer characters. The font map is just a `Map<Char, ByteArray>`, so swapping is trivial and enables future user font themes.

### Rendering Loop Change (PixelPerfectRenderer.kt)

```kotlin
// OLD: drawBitmapChar iterates 5×5
for (row in 0..4) {
    val rowData = FONT_5X5[char]!![row]
    for (col in 0..4) {
        val isSet = (rowData and (1 shl (4 - col))) != 0

// NEW: drawBitmapChar iterates 7×7
for (row in 0..6) {
    val rowData = FONT_7X7[char]!![row]
    for (col in 0..6) {
        val isSet = (rowData.toInt() and (1 shl (6 - col))) != 0
```

No other structural changes to the render pipeline are needed.

---

## Implementation Phases

### Phase 1 — BitmapFont7x7.kt (New File)
- Create `BitmapFont7x7.kt` alongside `BitmapFont5x5.kt`
- Generate all 95 printable ASCII chars using the padding rule above
- Add a few character-specific tweaks if any look wrong after padding

**Files:**
- `app/src/main/java/com/example/pockettracker/BitmapFont7x7.kt` ← new

### Phase 2 — Renderer Core (PixelPerfectRenderer.kt)
- Update 4 global constants: `DESIGN_WIDTH_PX`, `DESIGN_HEIGHT_PX`, `SCREEN_SPACER`, `SIDE_SPACER`
- Update `drawBitmapChar()`: loop bounds 0..4 → 0..6, bit shift 4 → 6, font map reference
- Update `drawBitmapText()`: no structural change, but uses updated char size

**Files:**
- `app/src/main/java/com/example/pockettracker/PixelPerfectRenderer.kt`

### Phase 3 — TrackerLayout Module Positions (PixelPerfectRenderer.kt)
- Oscilloscope: x=5, y=3
- Main editors: x=5, y=41
- NavigationMap: x=235, y=177
- All `module.draw(x, y, scale, state)` calls are unchanged (positions update via new constants)

**Files:**
- `app/src/main/java/com/example/pockettracker/PixelPerfectRenderer.kt`

### Phase 4 — Module Files (one at a time, test after each)

| Order | Module | Key changes |
|-------|--------|-------------|
| 1 | OscilloscopeModule.kt | width=310, height=35 |
| 2 | NavigationMapModule.kt | width=80, height=60, CELL_WIDTH=16, CELL_HEIGHT=12, all cell position math |
| 3 | PhraseEditorModule.kt | FONT_SCALE=1, CHAR_SPACING=1, ROW_HEIGHT=10, TEXT_PADDING=1, all column X positions halved |
| 4 | ChainEditorModule.kt | same font/row constants, halved positions |
| 5 | SongEditorModule.kt | same |
| 6 | InstrumentModule.kt | same |
| 7 | ProjectModule.kt | same |
| 8 | TableModule.kt | same |
| 9 | MixerModule.kt | width=310, METER_WIDTH=15, METER_HEIGHT=100, METER_SPACING=28, FIRST_METER_X=5 |
| 10 | FileBrowserModule.kt | width=320, height=240, ROW_HEIGHT=10, recalculate VISIBLE_ROWS |

### Phase 5 — DeviceAdapter.kt
- Update `SCREEN_WIDTH = 320`, `SCREEN_HEIGHT = 240`
- Virtual button layout recalculates from new screen dimensions

### Phase 6 — Test on Device
- Miyoo Flip (640×480): expect scale=2, chars 14px on screen, 16 phrase rows visible
- TrimUI Brick (1024×768): expect scale=3, chars 21px on screen — 1.4× bigger than before
- Phone emulator: expect scale=4+, large crisp text

---

## Risk Areas

| Risk | Impact | Mitigation |
|------|--------|-----------|
| NavigationMap cell text overflow | Minor visual | Using 80×60 non-halved dims with 16px cell width (fits 2 chars exactly) |
| Hardcoded pixel values in `drawLayout()` | Layout break | Search for any inline integer positions not derived from module constants |
| `labelWidth` calculations using old char math | Text alignment off | Update all `label.length * 5 * FONT_SCALE` → `label.length * 7 * FONT_SCALE` (or `label.length * CHAR_ADVANCE`) |
| FileBrowserModule VISIBLE_ROWS | Scroll miscalculation | Recalculate: `(height - headers) / ROW_HEIGHT` |
| DeviceAdapter button layout | Virtual buttons misaligned | Test on portrait phone after updating SCREEN_WIDTH/HEIGHT |
| `byteArrayOf` sign issues in 7×7 font | Rendering glitches | 7-bit rows (0..127) fit in signed byte — no issue. But use `.toInt()` in bit operations |

---

## Files to Modify (Complete List)

```
app/src/main/java/com/example/pockettracker/
├── BitmapFont7x7.kt          ← NEW
├── PixelPerfectRenderer.kt   ← constants + drawBitmapChar + TrackerLayout positions
├── OscilloscopeModule.kt
├── NavigationMapModule.kt
├── PhraseEditorModule.kt
├── ChainEditorModule.kt
├── SongEditorModule.kt
├── InstrumentModule.kt
├── ProjectModule.kt
├── TableModule.kt
├── MixerModule.kt
├── FileBrowserModule.kt
└── DeviceAdapter.kt          ← SCREEN_WIDTH, SCREEN_HEIGHT
```

**Not touched:** All core logic (`core/logic/`, `core/data/`, `core/audio/`), JNI/C++, serialization. This refactor is **purely UI rendering**.

---

## Alternative Considered: Float Scaling / "Stretch Mode"

The simplest alternative is to remove the integer-only constraint and allow the 640×480 canvas to scale by a float factor (e.g., 1.6× for TrimUI Brick → 1024×480 displayed). This is how most retro game emulators offer a "stretch to fill" mode.

**Pros:**
- Near-zero code changes (change `min(scaleX, scaleY)` to a float)
- Works immediately on all devices
- Screen is always maximally used

**Cons:**
- Non-integer scaling (e.g., 1.6×) produces **uneven pixels** with nearest-neighbor: some pixels render at 2×2px, others at 1×2px. Text looks jagged and inconsistent.
- With bilinear interpolation: text looks **blurry** — unacceptable for a pixel-art tracker
- No improvement in information density or font quality
- Not pixel-perfect

**Verdict:** Stretch mode is a quick workaround but conflicts with the pixel-perfect aesthetic that defines PocketTracker. The 320×240 refactor is more work but delivers genuinely better results — both in device compatibility and future font flexibility. Could be added as an optional "integer scale only OFF" toggle in settings post-MVP, but should not be the default.

---

*Plan written: 2026-02-28*
