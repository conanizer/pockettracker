# THEME_EDITOR_PLAN.md
# Theme Editor — Implementation Plan

**Status:** Planned (post-Extension-Pack-2)
**Created:** 2026-02-27

---

## Overview

A two-level settings overlay accessible from the PROJECT screen via the `SYSTEM`
row. Provides a unified color scheme system, visualizer selector, and theme
save/load as shareable `.ptt` files.

Three overlays, stacked like M8:

```
PROJECT screen
    └─► SYSTEM SETTINGS  (full screen overlay, level 1)
            ├── FONT       — stubbed, post-MVP
            ├── VISUALIZER — cycles inline
            └── THEME ──►  THEME EDITOR  (full screen overlay, level 2)
```

B button always pops one level back. A new `overlayStack: List<OverlayType>`
replaces the current single `activeOverlay` flag so multi-level navigation is
clean and extensible.

---

## Part 1: Color Centralization

All modules currently hardcode colors inline. Before the editor can exist, extract
them into a single `AppTheme` object that flows top-down through all rendering.

**New file: `AppTheme.kt`**

```kotlin
@Serializable
data class AppTheme(
    val name: String = "CLASSIC",

    // Backgrounds
    val background: Long    = 0xFF0A0A0A,  // module bg
    val rowCursor: Long     = 0xFF333333,  // cursor row highlight
    val rowPlayback: Long   = 0xFF004400,  // current playback row
    val rowSelection: Long  = 0xFF1A3A1A,  // selection region

    // Text roles
    val textTitle: Long     = 0xFF00FFFF,  // screen headers
    val textParam: Long     = 0xFF888888,  // inactive param names
    val textValue: Long     = 0xFFFFFFFF,  // default values
    val textCursor: Long    = 0xFFFFFF00,  // cursor-highlighted text
    val textEmpty: Long     = 0xFF666666,  // empty/placeholder

    // Visualizer bar
    val vizBackground: Long = 0xFF0A0A0A,
    val vizCenterLine: Long = 0xFF333333,
    val vizWave: Long       = 0xFF00FF00,  // waveform / bars / peaks

    // Mixer meters
    val meterBackground: Long = 0xFF1A1A1A,
    val meterLow: Long        = 0xFF00CC00,
    val meterMid: Long        = 0xFFCCCC00,
    val meterHigh: Long       = 0xFFCC0000,

    // Options
    val visualizerType: VisualizerType = VisualizerType.SCOPE
)

enum class VisualizerType { SCOPE, BARS, PEAKS, MIRROR, FLAT }
```

`TrackerModule.draw()` gains a `theme: AppTheme` parameter. Every hardcoded color
in every module becomes a theme lookup. This is mechanical but essential — it's
what makes live preview work as you edit colors.

### Bundled themes

Defined as constants in `AppTheme` companion object (no files required — they work
even with no Themes folder present):

| Name      | Description                          |
|-----------|--------------------------------------|
| `CLASSIC` | Current green-on-black (default)     |
| `AMBER`   | Orange/brown tones, warm CRT feel    |
| `BLUE`    | Cyan/blue palette                    |
| `MONO`    | Pure white-on-black, no color        |

User-saved themes live in `Documents/PocketTracker/Themes/*.ptt`.
File format: JSON via the existing Kotlinx Serialization + `IFileSystem` pipeline
(same as `.ptp` project files — no new infrastructure needed).

---

## Part 2: Font System

**Decision: bitmap fonts only, font changing is post-MVP.**

The 5×5 pixel grid is already the minimum needed to draw readable Latin glyphs.
There is no viable "slim" or "narrow" variant at this resolution — going narrower
breaks legibility. A full font-switching system would require rethinking the
renderer's column math across every module, which is a large refactor that does
not belong in this feature.

**What ships with this feature:**
- The current `BitmapFont5x5.kt` (CLASSIC), unchanged
- A `FONT` row in the System Settings screen that is visible but inactive,
  labelled to signal future expansion:

```
FONT        CLASSIC  [POST-MVP]
```

The row is non-interactive (cursor skips over it, or lands but does nothing on
A press). This keeps the System Settings menu honest — the slot exists — without
shipping a broken or misleading feature.

**Post-MVP font work (separate plan, separate branch):**
- Research alternative bitmap font formats (e.g. 6×8 or 8×8) that allow
  meaningful stylistic variation
- Evaluate switching from custom bitmap renderer to Android `Paint.setTypeface()`
  for the non-tracker-critical screens
- Implement per-font column width constants so layout reflows automatically

---

## Part 3: Visualizer System

Refactor `OscilloscopeModule` → `VisualizerModule`. Same 620×70px slot, same
position in `TrackerLayout`. Dispatches to one of five render functions based on
`theme.visualizerType`. All modes receive the same `FloatArray` waveform data
from the audio engine — no new audio-side work needed.

### VIZ 1: SCOPE (existing oscilloscope)

Connected line segments across the full 620px width. No changes beyond color
substitution (`Color(0xFF00ff00)` → `Color(theme.vizWave)`).

```
╔══════════════════════════════════════╗
║  ╭─╮     ╭──╮    ╭─╮                ║
║──╯  ╰─────╯  ╰────╯  ╰──────────────║
╚══════════════════════════════════════╝
```

### VIZ 2: BARS

Divide the 620px width into **40 vertical bars** (~15px wide, 1px gap). For each
bar, average the absolute amplitude of its waveform chunk and draw a filled
rectangle from the center outward, symmetrically up and down. Color = `vizWave`.

```
╔══════════════════════════════════════╗
║   ▄  ▄▄  ▄   ▄▄▄ ▄  ▄               ║
║  ██ ███ ██  ████ ██ ██              ║
║  ██ ███ ██  ████ ██ ██              ║
║   ▀  ▀▀  ▀   ▀▀▀ ▀  ▀               ║
╚══════════════════════════════════════╝
```

### VIZ 3: PEAKS

Same as BARS but with a 1px peak-hold indicator line per bar. Each peak line
holds for ~30 frames then decays 1px toward center per frame.

Requires `VisualizerModule` to own state between frames:
- `FloatArray(40)` — current peak heights
- `IntArray(40)` — frames since peak was set

```
╔══════════════════════════════════════╗
║  ─  ──  ─   ─── ─  ─                ║  ← peak hold lines
║   ▄  ▄▄  ▄   ▄▄▄ ▄  ▄               ║
║  ██ ███ ██  ████ ██ ██              ║
╚══════════════════════════════════════╝
```

### VIZ 4: MIRROR

Draw the waveform line in the top half (35px), then again reflected vertically in
the bottom half. Creates a symmetric butterfly shape. Especially interesting with
sustained synth audio. Implementation: render the waveform twice; second pass
mirrors Y coordinates around the center line.

```
╔══════════════════════════════════════╗
║  ╭─╮     ╭──╮    ╭─╮                ║  ← top half
║──╯  ╰─────╯  ╰────╯  ╰──────────────║  ← center
║──╮  ╭─────╮  ╭────╮  ╭──────────────║  ← reflected
║  ╰─╯     ╰──╯    ╰─╯                ║
╚══════════════════════════════════════╝
```

### VIZ 5: FLAT

Background fill + a single 1px separator line at the bottom of the 70px slot.
No audio computation whatsoever. For users who find the visualizer distracting.

```
╔══════════════════════════════════════╗
║                                      ║
║                                      ║
║──────────────────────────────────────║
╚══════════════════════════════════════╝
```

---

## Part 4: System Settings Screen

**New file: `SystemSettingsModule.kt`**

Full screen: **620×392px** (same width as MixerModule to use the full canvas
width — System Settings is a global overlay, not tied to a specific column).

Opened from PROJECT screen → `SYSTEM` row → A press.

### Screen layout

```
SYSTEM                              ← cyan header

FONT        CLASSIC  [POST-MVP]     ← non-interactive, placeholder only
VISUALIZER  SCOPE                   ← cycles: SCOPE / BARS / PEAKS / MIRROR / FLAT
THEME       CLASSIC  >              ← current theme name, > = sub-screen entry
```

The `>` chevron on THEME signals a sub-screen entry point, matching M8 convention.

**FONT row:** Cursor can land here. A press does nothing (or shows a brief
`[POST-MVP]` flash in the status area). Makes the intent clear without misleading
the user.

**VISUALIZER row:** A+UP / A+DOWN cycles through `VisualizerType` values. The
visualizer bar at the top of the screen updates immediately (live preview).

**THEME row:**
- A → push `THEME_EDITOR` onto overlay stack, open Theme Editor
- A+UP / A+DOWN → cycle through built-in themes without entering the editor
  (quick preview mode)

**Navigation:**
- Cursor: UP/DOWN across 3 rows
- B → pop overlay stack → return to PROJECT screen

---

## Part 5: Theme Editor Screen

**New file: `ThemeEditorModule.kt`**

Full screen: **620×392px**. Pushed onto overlay stack from System Settings.

### Screen layout

```
THEME                               ← cyan header

BACKGROUND    0A0A0A
ROW CURSOR    333333
ROW PLAYBACK  004400
ROW SELECT    1A3A1A
TEXT TITLE    00FFFF
TEXT PARAM    888888
TEXT VALUE    FFFFFF
TEXT CURSOR   FFFF00
TEXT EMPTY    666666
VIZ BG        0A0A0A
VIZ LINE      333333
VIZ WAVE      00FF00
METER LOW     00CC00
METER MID     CCCC00
METER HIGH    CC0000

THEME         LOAD   SAVE           ← same pattern as PROJECT / LOAD / SAVE
```

15 color rows + 1 action row. At 21px per row + header (35px) = 386px total.
Fits in 392px with a 6px bottom margin.

### Color editing controls

Values are displayed and stored as 24-bit RGB hex (6 characters, no alpha prefix
shown to user — alpha is always FF).

| Input | Effect |
|-------|--------|
| A + UP / DOWN | Red component +/- 0x10 (large step) |
| A + LEFT / RIGHT | Green component +/- 0x10 (large step) |
| L + A + UP / DOWN | Red component +/- 0x01 (fine step) |
| L + A + LEFT / RIGHT | Green component +/- 0x01 (fine step) |
| SELECT + A + UP / DOWN | Blue component +/- 0x10 (large step) |
| SELECT + A + LEFT / RIGHT | Blue component +/- 0x01 (fine step) |

All components wrap 0x00–0xFF. Changes are visible in real-time across all
currently rendered modules (the oscilloscope wave color, row highlights, text
colors all update as you move the value).

### THEME row

- Cursor on `LOAD` → A → opens `FILE_BROWSER` filtered to `*.ptt` files in
  `Documents/PocketTracker/Themes/`
- Cursor on `SAVE` → A → saves current `AppTheme` to
  `Documents/PocketTracker/Themes/{theme.name}.ptt`
- To rename before saving: edit `theme.name` — add a NAME row above LOAD/SAVE
  if needed (same per-character editing as PROJECT / NAME row)

**Navigation:**
- Cursor scrolls: when cursor reaches top or bottom row, the list scrolls
- B → pop overlay stack → return to System Settings

---

## Part 6: ThemeManager

**New file: `core/logic/ThemeManager.kt`** (no Android imports — portable):

```kotlin
class ThemeManager(private val fileManager: FileManager) {
    private val themesPath = "Themes/"

    fun saveTheme(theme: AppTheme, filename: String): Boolean
    fun loadTheme(filename: String): AppTheme?
    fun listThemes(): List<String>          // returns .ptt filenames
    fun getBuiltinThemes(): List<AppTheme>  // CLASSIC, AMBER, BLUE, MONO
}
```

`.ptt` files are JSON, serialized via the existing `Json { prettyPrint = true }`
instance in `FileManager`. No new infrastructure — just a new call site.

---

## Part 7: ProjectModule Change

Replace the `SYSTEM` placeholder (currently renders `"---"` as value):

```
SYSTEM       >                      ← > = sub-screen indicator, always white
```

Input handling in the controller: when cursor is on SYSTEM row and A is pressed,
push `OverlayType.SYSTEM_SETTINGS` onto the overlay stack. System Settings does
not appear in the navigation grid — it is always entered from PROJECT.

---

## Overlay Stack

Replace the current `activeOverlay: OverlayType?` with a proper stack:

```kotlin
// In TrackerController (or InputController)
val overlayStack = mutableStateListOf<OverlayType>()

fun pushOverlay(type: OverlayType) { overlayStack.add(type) }
fun popOverlay() { if (overlayStack.isNotEmpty()) overlayStack.removeLast() }
fun currentOverlay(): OverlayType? = overlayStack.lastOrNull()
```

`TrackerLayout.drawLayout()` renders based on `currentOverlay()`:
- `null` → render the normal screen (PHRASE, CHAIN, etc.)
- `SYSTEM_SETTINGS` → render `SystemSettingsModule`
- `THEME_EDITOR` → render `ThemeEditorModule`
- `FILE_BROWSER` → existing behavior (unchanged)

B input: when any overlay is active, B pops the stack instead of its normal function.

---

## Implementation Phases

| # | Phase | Tasks | Risk |
|---|-------|-------|------|
| 1 | **Color centralization** | Create `AppTheme.kt`; add `theme` param to `TrackerModule.draw()`; replace all hardcoded colors in all modules | Low — pure substitution, no logic changes |
| 2 | **Visualizer system** | Rename `OscilloscopeModule` → `VisualizerModule`; implement BARS, PEAKS, MIRROR, FLAT | Low — all use existing waveform data |
| 3 | **Overlay stack** | Replace `activeOverlay` flag with stack in `TrackerController`; update B-button handling | Low — small refactor |
| 4 | **System Settings** | Create `SystemSettingsModule.kt`; wire SYSTEM row in `ProjectModule` | Low — simple module |
| 5 | **Theme Editor** | Create `ThemeEditorModule.kt`; implement color editing input; scrolling cursor | Medium — input combos need care |
| 6 | **ThemeManager + file I/O** | Create `ThemeManager.kt`; `.ptt` save/load; wire LOAD/SAVE buttons | Low — follows existing patterns |
| 7 | **Bundled themes** | Define CLASSIC, AMBER, BLUE, MONO constants; test all four | Low |
| 8 | **Integration + polish** | Live preview verification; edge cases (missing Themes folder, corrupt .ptt); status messages | Medium |

---

## What This Delivers (Definition of Done)

- [ ] All module colors driven by `AppTheme`, no hardcoded color values remain
- [ ] PROJECT → SYSTEM → A opens System Settings (full screen)
- [ ] System Settings shows FONT (stub), VISUALIZER (functional), THEME (functional)
- [ ] Cycling VISUALIZER changes the top bar in real-time (all 5 modes work)
- [ ] THEME → A opens Theme Editor (full screen)
- [ ] All 15 color parameters editable with live preview
- [ ] SAVE writes a `.ptt` file; LOAD reads one back correctly
- [ ] 4 bundled themes available via A+UP/DOWN on THEME row in System Settings
- [ ] B pops overlays correctly at both levels
- [ ] FONT row is visible, non-interactive, clearly labelled as post-MVP

## Out of Scope (Post-MVP, Separate Plan)

- Multiple bitmap font designs / font style switching
- Per-track colors
- Animated or beat-reactive visualizers
- Theme inheritance or partial overrides
- Font face loading from file
