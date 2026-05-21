# plan-theme-editor.md
# Theme Editor — Implementation Plan (Updated)

**Original:** 2026-02-27
**Reviewed + revised:** 2026-05-21 (3 months after original; approach updated to match actual codebase)
**Status:** Phases 1-3 and 5-9 COMPLETE. Phase 4 (visualizer modes) in progress.

---

## What Changed vs. the Original Plan

| Original plan | Actual approach (implemented) |
|---|---|
| `overlayStack: List<OverlayType>` | EqEditorOverlay pattern: `ThemeEditorState(isOpen=Bool)` |
| New "System Settings" screen | Rows 9-10 added to existing SettingsModule |
| Add `theme: AppTheme` to `TrackerModule.draw()` interface | `LocalAppTheme` CompositionLocal → state objects |
| `ThemeManager` class | SAVE/LOAD inlined in AppInputDispatcher via IFileSystem |
| Rename `OscilloscopeModule` → `VisualizerModule` | Keep filename; dispatch on `appTheme.visualizerType` |
| "System Settings" FONT row | Skipped (post-MVP, no stub added) |
| MainActivity | AppInputDispatcher — all new handlers live here |

---

## Implementation Phases

| # | Phase | Status | Notes |
|---|-------|--------|-------|
| 1 | Create `AppTheme.kt` + `VisualizerType` enum | ✅ DONE | 19 fields, ARGB Long, `@Serializable` |
| 2 | Thread theme: `LocalAppTheme` → `drawLayout` → state objects | ✅ DONE | Every module state has `appTheme: AppTheme` |
| 3 | Replace all hardcoded colors in all modules | ✅ DONE | `val t = state.appTheme` / `t.field` everywhere; `darken()` helper in EditorHelpers.kt |
| 4 | Visualizer modes in OscilloscopeModule (BARS, PEAKS, MIRROR, FLAT, OCTA) | ✅ DONE | Dispatch on `t.visualizerType`; PEAKS uses instance-level peak state; OCTA uses per-track C++ waveform buffers |
| 5 | Add VISUALIZER + THEME rows to SettingsModule (rows 9-10) | ✅ DONE | Rows 9=VISUALIZER, 10=THEME; cursor limit 0-10 in TrackerController |
| 6 | `ThemeEditorState` + `ThemeEditorModule` full-screen overlay | ✅ DONE | 16 color rows + THEME/SAVE/LOAD row; same overlay pattern as EqModule |
| 7 | Wire in AppInputDispatcher + AppStateRefs | ✅ DONE | `adjustThemeColor()`, `cycleNextBuiltinTheme()`, SAVE→QWERTY, LOAD→FileBrowser |
| 8 | Theme file I/O using IFileSystem | ✅ DONE | `getThemesDirectory()` on IFileSystem; save: `fileSystem.writeFile(path, Json.encode(theme))`; load: FileBrowser → `LOAD_THEME` action decodes JSON |
| 9 | Bundled themes | ✅ DONE | `AppTheme.BUILTINS = listOf(CLASSIC, AMBER, BLUE, MONO)` |

---

## Phase 4: Visualizer Modes — Spec

`OscilloscopeModule` dispatches on `appTheme.visualizerType`. All modes share the same background rect draw. SCOPE is the existing mode. PEAKS requires instance-level state (stored as module fields — the module instance lives inside `TrackerLayout` which is `remember`ed).

### Layout constants

```
width = 620, height = 70
NUM_BARS = 40
BAR_W = 14      // (620 - 39 gaps) / 40 = 14px, bars centered with 10px margin each side
BAR_GAP = 1
MAX_AMP = height/2 - 2   // = 33px, headroom for 1px center line + 1px edge
PEAK_HOLD_FRAMES = 30
PEAK_DECAY_PX = 1
```

### VIZ 1: SCOPE (existing)
Center line + connected waveform path. Already implemented.

### VIZ 2: BARS
40 bars, symmetric around center. Each bar = average |sample| × WAVEFORM_GAIN over its chunk.

```
barAmp = avg(abs(chunk)) * WAVEFORM_GAIN  clamped 0..1
barH   = (barAmp * MAX_AMP * 2).toInt()   // full height
draw filled rect: centerY - barH/2 .. centerY + barH/2
```

Color = `vizWave`. No center line (bars cover it).

### VIZ 3: PEAKS
Same as BARS, plus:
- Per-bar peak track: if `barAmp > peakValues[i]` → update + reset decay
- Else: increment `peakDecayCounters[i]`; after PEAK_HOLD_FRAMES frames, `peakValues[i] -= PEAK_DECAY_PX / MAX_AMP` per frame
- Draw 1px horizontal line at peak position above AND below center: `color = vizWave` (same color, slightly dimmed via `darken(0.5f)`)

State stored as module instance fields (`private val peakValues = FloatArray(NUM_BARS)` etc.).

### VIZ 4: MIRROR
Top half: waveform mapped to y ∈ [y, centerY].
Bottom half: same waveform reflected (sample negated), mapped to y ∈ [centerY, y+height].
Both halves share a 1px center line. One `Path` per half.

```
topY    = centerY + sample * MAX_AMP   // normal
bottomY = centerY - sample * MAX_AMP   // reflected
```

### VIZ 5: FLAT
Background rect only (already drawn). Add a single 1px horizontal separator at `y + height - 1`.
Color = `vizCenterLine`. No waveform computation.

---

## Architecture Notes (current state)

### AppTheme flow

```
PocketTrackerApp
  └─ CompositionLocalProvider(LocalAppTheme = appTheme)
       └─ PixelPerfectTracker
            └─ drawLayout(appTheme = appTheme)          ← TrackerLayout.drawLayout()
                 ├─ OscilloscopeState(waveformBuffer, appTheme)
                 ├─ PhraseEditorState(..., appTheme = appTheme)
                 ├─ ... (all module states)
                 └─ ThemeEditorDrawState(theme = appTheme, editorState = themeEditorState)
```

Every draw function: `val t = <state>.appTheme` → `Color(t.fieldName)`.

### ThemeEditorModule input controls

| Input | Effect |
|---|---|
| DPAD UP/DOWN | Move cursor row (wraps 0 ↔ MAX_ROW=16) |
| DPAD LEFT/RIGHT | Move cursor channel (0=R, 1=G, 2=B; on row 0: 0=theme name, 1=SAVE, 2=LOAD) |
| A + DPAD UP | Red/G/B component +0x10 (large step) — via `adjustThemeColor()` |
| A + DPAD DOWN | − large step |
| A + DPAD LEFT/RIGHT | ± fine step (0x01) |
| A (on row 0, ch=0) | Cycle next builtin theme |
| A (on row 0, ch=1) | Open QWERTY to name + SAVE .ptt |
| A (on row 0, ch=2) | Open FileBrowser in Themes dir |
| B | Close ThemeEditorState |

### SettingsModule rows

```
Row 0:  LAYOUT      FULLSCREEN / T.PORT / T.LAND / AMIGA PORTRAIT
Row 1:  SCALING     INT / BILINEAR
Row 2:  BTN SOUND   ON / OFF
Row 3:  BTN VOL     00-FF
Row 4:  BTN VIBRO   ON / OFF
Row 5:  VIBRO POW   00-FF
Row 6:  KB INSERT   BEFORE / AFTER
Row 7:  CURSOR      REMEMBER / REFRESH
Row 8:  NOTE PREV   ON / OFF
Row 9:  VISUALIZER  SCOPE / BARS / PEAKS / MIRROR / FLAT   ← A cycles
Row 10: THEME       <name> >                                ← A opens ThemeEditorModule
```

Cursor limit: `settingsCursorRow ∈ 0..10` (TrackerController).

### File I/O

- **Save:** SAVE on ThemeEditorModule row 0 → QWERTY for filename → `QwertyContext.THEME_SAVE` → `fileSystem.writeFile("$themesDir/$name.ptt", Json.encode(theme))`
- **Load:** LOAD on row 0 → FileBrowser filtered to `.ptt` → `instrumentFileBrowserAction = "LOAD_THEME"` → on confirm: `Json.decode<AppTheme>(fileSystem.readFile(path))` → `appTheme = loaded`
- **Directory:** `IFileSystem.getThemesDirectory()` → `Documents/PocketTracker/Themes/`

---

## Definition of Done

- [x] All module colors driven by AppTheme, no hardcoded colors
- [x] SETTINGS → row 9 VISUALIZER cycles types
- [x] SETTINGS → row 10 THEME opens ThemeEditorModule
- [x] All 16 color parameters editable with live preview
- [x] SAVE writes .ptt; LOAD reads one back correctly
- [x] 4 bundled themes cycle via A on THEME row in Settings
- [x] B closes ThemeEditorModule
- [x] BARS visualizer mode
- [x] PEAKS visualizer mode (with peak hold/decay)
- [x] MIRROR visualizer mode
- [x] FLAT visualizer mode
- [x] OCTA visualizer mode (per-track ProTracker-style quadrascope; active tracks stretch to fill width)
- [x] Visualizer switches immediately on A-press in Settings row 9

## Out of Scope (Post-MVP)

- Font switching (FONT row stub not added — skipped)
- Per-track colors
- Theme inheritance or partial overrides
