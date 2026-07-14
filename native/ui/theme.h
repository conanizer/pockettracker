#pragma once

// ─── The theme ───────────────────────────────────────────────────────────────────────────────────
//
// A 1:1 port of ui/theme/AppTheme.kt — the same field names, the same four built-in palettes, the
// same ARGB values. Kotlin stores each colour as a `Long` because the class is kotlinx-serializable
// straight into a `.ptt` theme file; here it is a uint32_t, which is what the canvas blends anyway.
//
// The field names are load-bearing. `.ptt` files on a user's SD card are JSON keyed by exactly these
// names, and a project (and its themes) must move between an Android device and a handheld without
// conversion — so a rename here is a file-format break, not a refactor. The .ptt reader lands with
// the SETTINGS screen; until then these four built-ins are the whole palette set.

#include <cstdint>
#include <string>
#include <vector>

namespace pt::ui {

using Argb = uint32_t;  // 0xAARRGGBB, straight from the Kotlin literals

enum class VisualizerType { SCOPE, FLAT, OCTA, OCTA_FULL, SPECTRUM, SPECTRUM_PEAKS };

struct Theme {
    std::string name = "CLASSIC";

    // ── Row backgrounds ──────────────────────────────────────────────────────────────────────────
    Argb background   = 0xFF0A0A0A;  // module fill + default row
    Argb rowEvery4th  = 0xFF151515;  // beat-accent rows (every 4th)
    Argb rowCursor    = 0xFF333333;  // cursor row highlight
    Argb rowPlayback  = 0xFF004400;  // current playback row
    Argb rowSelection = 0xFF1A3A1A;  // selection region

    // ── Text roles ───────────────────────────────────────────────────────────────────────────────
    Argb textTitle  = 0xFF00FFFF;  // screen headers (cyan)
    Argb textParam  = 0xFF808080;  // inactive param label
    Argb textValue  = 0xFFFFFFFF;  // inactive param value
    Argb textCursor = 0xFFFFFF00;  // cursor-highlighted cell (yellow)
    Argb textEmpty  = 0xFF666666;  // empty / placeholder

    // ── Visualizer (oscilloscope bar) ────────────────────────────────────────────────────────────
    Argb vizBackground = 0xFF0A0A0A;
    Argb vizCenterLine = 0xFF333333;
    Argb vizWave       = 0xFF00FF00;  // waveform line / bar fill

    // ── Mixer dBFS meters ────────────────────────────────────────────────────────────────────────
    Argb meterBackground = 0xFF1A1A1A;
    Argb meterLow        = 0xFF00CC00;
    Argb meterMid        = 0xFFCCCC00;
    Argb meterHigh       = 0xFFCC0000;
    Argb meterBorder     = 0xFF444444;

    // ── Visualizer mode ──────────────────────────────────────────────────────────────────────────
    VisualizerType visualizerType = VisualizerType::SCOPE;
};

// ─── The editable colours ────────────────────────────────────────────────────────────────────────
//
// The THEME EDITOR's row list — Kotlin's `ThemeEditorModule.COLOR_ROWS`, in the same order, with the
// same labels. It lives HERE, next to the fields it projects, rather than in the module: it is a view
// of `Theme`'s own field list, and a table that can drift out of step with the struct it describes is
// a bug waiting for someone to add a colour. Three consumers read it (the module draws it, the
// dispatcher's colour nudge indexes it, and the ptinput golden sweeps it) and none may re-derive it.
//
// ⚠️ SEVENTEEN ROWS, EIGHTEEN COLOURS — `meterBorder` HAS NO ROW, AND THAT IS KOTLIN'S. It is a field
// on the theme, it is serialized into a `.ptt`, it is read by the mixer's meter frames, and there is
// simply no way to edit it in the UI. Ported as-is rather than "fixed" into a divergence: adding an
// eighteenth row here would make the C++ editor a superset of the Android one and put a row in the
// golden that Kotlin cannot produce. (A parity-ledger entry, not a port decision — see docs.)
//
// ⚠️ AND IT IS A POINTER-TO-MEMBER, NOT A GET/SET PAIR. Kotlin's row carries two lambdas — `get` and a
// copy-based `set` — which are two statements of the same fact and can therefore disagree; that is
// exactly the shape of the bug S8 found in `applyCallerEqSlotChange` (one function wrote the field and
// made the call; the other only made the call). One member pointer reads and writes the same field by
// construction, and a typo is a compile error instead of a colour that edits its neighbour.

struct ThemeColorRow {
    const char* label;
    Argb Theme::* field;
};

inline const std::vector<ThemeColorRow>& theme_color_rows() {
    static const std::vector<ThemeColorRow> rows = {
        {"BACKGROUND", &Theme::background},
        {"ROW 4TH",    &Theme::rowEvery4th},
        {"ROW CURSOR", &Theme::rowCursor},
        {"ROW PLAY",   &Theme::rowPlayback},
        {"ROW SELECT", &Theme::rowSelection},
        {"TXT TITLE",  &Theme::textTitle},
        {"TXT PARAM",  &Theme::textParam},
        {"TXT VALUE",  &Theme::textValue},
        {"TXT CURSOR", &Theme::textCursor},
        {"TXT EMPTY",  &Theme::textEmpty},
        {"VIZ BG",     &Theme::vizBackground},
        {"VIZ LINE",   &Theme::vizCenterLine},
        {"VIZ WAVE",   &Theme::vizWave},
        {"MTR BG",     &Theme::meterBackground},
        {"MTR LOW",    &Theme::meterLow},
        {"MTR MID",    &Theme::meterMid},
        {"MTR HIGH",   &Theme::meterHigh},
    };
    return rows;
}

inline Theme theme_classic() { return Theme{}; }

inline Theme theme_amber() {
    Theme t;
    t.name          = "AMBER";
    t.rowPlayback   = 0xFF332200;
    t.rowSelection  = 0xFF3A2A00;
    t.textTitle     = 0xFFFFCC00;
    t.textParam     = 0xFF806040;
    t.textValue     = 0xFFEECC88;
    t.textCursor    = 0xFFFFFF00;
    t.textEmpty     = 0xFF664422;
    t.vizCenterLine = 0xFF442200;
    t.vizWave       = 0xFFFF8800;
    t.meterLow      = 0xFFCC8800;
    t.meterMid      = 0xFFCC4400;
    t.meterHigh     = 0xFFCC0000;
    return t;
}

inline Theme theme_blue() {
    Theme t;
    t.name          = "BLUE";
    t.rowPlayback   = 0xFF001144;
    t.rowSelection  = 0xFF002266;
    t.textTitle     = 0xFF88CEFF;
    t.textParam     = 0xFF4488AA;
    t.textValue     = 0xFFAADDFF;
    t.textCursor    = 0xFF00FFFF;
    t.textEmpty     = 0xFF224466;
    t.vizCenterLine = 0xFF112244;
    t.vizWave       = 0xFF0088FF;
    t.meterLow      = 0xFF0088CC;
    t.meterMid      = 0xFF0044CC;
    t.meterHigh     = 0xFF8800CC;
    return t;
}

inline Theme theme_mono() {
    Theme t;
    t.name          = "MONO";
    t.rowPlayback   = 0xFF222222;
    t.rowSelection  = 0xFF333333;
    t.textTitle     = 0xFFFFFFFF;
    t.textParam     = 0xFF888888;
    t.textValue     = 0xFF8F8F8F;
    t.textCursor    = 0xFFFFFFFF;
    t.textEmpty     = 0xFF444444;
    t.vizCenterLine = 0xFF222222;
    t.vizWave       = 0xFFCCCCCC;
    t.meterLow      = 0xFFCCCCCC;
    t.meterMid      = 0xFF888888;
    t.meterHigh     = 0xFF444444;
    return t;
}

/**
 * The built-ins, in the order the theme cycle walks them — Kotlin's `AppTheme.BUILTINS`.
 *
 * ⚠️ `visualizerType` is a FIELD on a theme but is NOT part of a theme's identity. Android carries it
 * across a theme change deliberately (`BUILTINS[next].copy(visualizerType = appTheme.visualizerType)`):
 * the palette belongs to the theme, the visualizer belongs to the user. Anything that swaps a theme
 * must preserve it — which is what `theme_by_name` takes it as an argument for.
 */
inline std::vector<Theme> theme_builtins() {
    return {theme_classic(), theme_amber(), theme_blue(), theme_mono()};
}

/** A built-in by name, keeping `visualizer`. An unknown name reads as CLASSIC, as a bad .ptt does. */
inline Theme theme_by_name(const std::string& name, VisualizerType visualizer) {
    Theme found = theme_classic();
    for (const Theme& t : theme_builtins()) {
        if (t.name == name) { found = t; break; }
    }
    found.visualizerType = visualizer;
    return found;
}

}  // namespace pt::ui
