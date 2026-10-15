#pragma once

// The palette and the four built-ins. ⚠️ The field names are the `.ptt` file's keys — a rename breaks
// the format.

#include <cstdint>
#include <string>
#include <vector>

namespace pt::ui {

using Argb = uint32_t;  // 0xAARRGGBB

enum class VisualizerType { SCOPE, FLAT, OCTA, OCTA_FULL, SPECTRUM, SPECTRUM_PEAKS };

struct Theme {
    std::string name = "CLASSIC";

    // ── Row backgrounds ──────────────────────────────────────────────────────────────────────────
    Argb background   = 0xFF0A0A0A;  // module fill + default row
    Argb rowEvery4th  = 0xFF151515;  // beat-accent rows (every 4th)

    // The palette's accent: the cursor bar, every "you are here" mark, the EQ curve. `background` is
    // the ink on it.
    Argb rowCursor    = 0xFFFFFF00;
    // Paints nothing: the seed of `textPlayhead`'s default for a `.ptt` without TXT PLAY.
    Argb rowPlayback  = 0xFF004400;

    // A second bright ground, near the cursor's (`rowEvery4th` is the ink on it).
    Argb rowSelection = 0xFF00CC00;

    // ── Text roles ───────────────────────────────────────────────────────────────────────────────
    Argb textTitle  = 0xFF00FFFF;  // screen headers (cyan)
    Argb textParam  = 0xFF808080;  // inactive param label
    Argb textValue  = 0xFFFFFFFF;  // inactive param value
    Argb textEmpty  = 0xFF666666;  // empty / placeholder

    // ⚠️ Drawn by nothing, but still `.ptt` keys — parsed and written, so they stay.
    Argb textCursor = 0xFFFFFF00;
    Argb textSelection = 0xFF00FF00;  // = vizWave

    // The `>` playback marker. Its default is lifted from `rowPlayback`; once in a file, used as typed.
    Argb textPlayhead = 0xFF00E000;

    // ── Visualizer (oscilloscope bar) ────────────────────────────────────────────────────────────
    Argb vizBackground = 0xFF0A0A0A;
    Argb vizCenterLine = 0xFF333333;
    Argb vizWave       = 0xFF00FF00;  // waveform line / bar fill

    // ── The EQ editor's panel: CLASSIC's values of `derive_borrowed_colors`, which is the authority ──
    // `eqBg` has no editor row: it is always a copy of `background` or `vizBackground`.
    Argb eqBg     = 0xFF0A0A0A;  // = vizBackground
    Argb eqFill   = 0xFF222222;  // = darken(textParam, 0.27f)
    Argb eqBorder = 0xFF808080;  // = textParam — the spectrum outline AND the 0 dB line
    Argb eqTxt    = 0xFF333333;  // = vizCenterLine

    // ── Mixer dBFS meters ────────────────────────────────────────────────────────────────────────
    Argb meterBackground = 0xFF1A1A1A;
    Argb meterLow        = 0xFF00CC00;
    Argb meterMid        = 0xFFCCCC00;
    Argb meterHigh       = 0xFFCC0000;
    Argb meterBorder     = 0xFF444444;

    // ── Visualizer mode ──────────────────────────────────────────────────────────────────────────
    VisualizerType visualizerType = VisualizerType::SCOPE;
};

// Scale the RGB channels by `factor`; alpha kept.
inline Argb darken(Argb c, float factor) {
    auto ch = [&](int shift) {
        const int v = static_cast<int>(static_cast<float>((c >> shift) & 0xFF) * factor);
        return static_cast<Argb>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    return (c & 0xFF000000u) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// A dark seed lifted to a readable ink of the same hue (pure black: `fallback`). Only ever a default,
// never applied to a typed value.
inline Argb lift_seed_to_ink(Argb seed, Argb fallback) {
    constexpr int TARGET = 0xE0;
    const int r = (seed >> 16) & 0xFF, g = (seed >> 8) & 0xFF, b = seed & 0xFF;
    const int peak = (r > g ? r : g) > b ? (r > g ? r : g) : b;
    if (peak == 0) return fallback;
    if (peak >= TARGET) return 0xFF000000u | (seed & 0x00FFFFFFu);
    return darken(seed, static_cast<float>(TARGET) / static_cast<float>(peak));
}

// The default of six keys, derived from the rest of the palette. `parse_theme` fills in what a file
// lacks and `serialize_theme` omits what still equals it.
// ⚠️ Every producer of a Theme calls it last.
inline void derive_borrowed_colors(Theme& t) {
    t.eqBg     = t.vizBackground;
    t.eqFill   = darken(t.textParam, 0.27f);
    t.eqBorder = t.textParam;
    t.eqTxt    = t.vizCenterLine;

    t.textSelection = t.vizWave;
    t.textPlayhead  = lift_seed_to_ink(t.rowPlayback, t.textCursor);
}

// ─── The theme editor's rows ─────────────────────────────────────────────────────────────────────
// Five fields have none: `meterBorder`, `rowPlayback` (a seed), `textCursor` and `textSelection`
// (unused), `eqBg` (a copy). ⚠️ A row's position is its number in the help text and the tests: append.

struct ThemeColorRow {
    const char* label;
    Argb Theme::* field;
};

inline const std::vector<ThemeColorRow>& theme_color_rows() {
    static const std::vector<ThemeColorRow> rows = {
        {"BACKGROUND", &Theme::background},
        {"ROW 4TH",    &Theme::rowEvery4th},
        {"ROW CURSOR", &Theme::rowCursor},
        {"ROW SELECT", &Theme::rowSelection},
        {"TXT TITLE",  &Theme::textTitle},
        {"TXT PARAM",  &Theme::textParam},
        {"TXT VALUE",  &Theme::textValue},
        {"TXT EMPTY",  &Theme::textEmpty},
        {"TXT PLAY",   &Theme::textPlayhead},
        {"VIZ BG",     &Theme::vizBackground},
        {"VIZ LINE",   &Theme::vizCenterLine},
        {"VIZ WAVE",   &Theme::vizWave},
        {"MTR BG",     &Theme::meterBackground},
        {"MTR LOW",    &Theme::meterLow},
        {"MTR MID",    &Theme::meterMid},
        {"MTR HIGH",   &Theme::meterHigh},
        {"EQ FILL",    &Theme::eqFill},
        {"EQ BORDER",  &Theme::eqBorder},
        {"EQ TXT",     &Theme::eqTxt},
    };
    return rows;
}

inline Theme theme_classic() {
    Theme t;
    derive_borrowed_colors(t);
    return t;
}

inline Theme theme_amber() {
    Theme t;
    t.name          = "AMBER";
    t.background    = 0xFF0A0808;
    t.rowEvery4th   = 0xFF151212;
    t.rowCursor     = 0xFFFFBB00;
    t.rowPlayback   = 0xFF332200;
    t.rowSelection  = 0xFFCC6600;
    t.textTitle     = 0xFFFFBB00;
    t.textParam     = 0xFF806040;
    t.textValue     = 0xFFEECC88;
    t.textEmpty     = 0xFF664422;
    t.vizBackground = 0xFF0A0808;
    t.vizCenterLine = 0xFF382404;
    t.vizWave       = 0xFFFF8800;
    t.meterBackground = 0xFF1A1515;
    t.meterLow      = 0xFFCC8800;
    t.meterMid      = 0xFFCC4400;
    t.meterHigh     = 0xFFCC0000;
    derive_borrowed_colors(t);
    return t;
}

inline Theme theme_blue() {
    Theme t;
    t.name          = "BLUE";
    t.rowCursor     = 0xFF66AEDC;
    t.rowPlayback   = 0xFF002266;
    t.rowSelection  = 0xFF3E7FA8;
    t.textTitle     = 0xFF88CEFF;
    t.textParam     = 0xFF4486AA;
    t.textValue     = 0xFFAADDFF;
    t.textEmpty     = 0xFF224466;
    t.vizCenterLine = 0xFF112244;
    t.vizWave       = 0xFF0082BA;
    t.meterBackground = 0xFF151515;
    t.meterLow      = 0xFF0082BA;
    t.meterMid      = 0xFF004499;
    t.meterHigh     = 0xFF6050A0;
    derive_borrowed_colors(t);
    // After the derive, which would overwrite these two.
    t.eqBorder      = 0xFF4488AA;
    t.textPlayhead  = 0xFF224466;
    return t;
}

inline Theme theme_mono() {
    Theme t;
    t.name          = "MONO";
    t.rowCursor     = 0xFFE8E8E8;
    t.rowPlayback   = 0xFF444444;
    t.rowSelection  = 0xFFA8A8A8;
    t.textTitle     = 0xFFFFFFFF;
    t.textParam     = 0xFF8B8E8E;   // a shade off TXT VALUE on purpose
    t.textValue     = 0xFFC0C0C0;
    t.textEmpty     = 0xFF444444;
    t.vizCenterLine = 0xFF222222;
    t.vizWave       = 0xFFCCCCCC;
    t.meterLow      = 0xFFB0B0B0;
    t.meterMid      = 0xFF808080;
    t.meterHigh     = 0xFF444444;
    derive_borrowed_colors(t);
    return t;
}

// The built-ins, in cycle order. `visualizerType` is the user's, not the theme's: a swap keeps it.
inline std::vector<Theme> theme_builtins() {
    return {theme_classic(), theme_amber(), theme_blue(), theme_mono()};
}

// A first launch's look: BLUE with the OCTA bars. (`Theme{}` is CLASSIC.)
inline Theme theme_default() {
    Theme t          = theme_blue();
    t.visualizerType = VisualizerType::OCTA;
    return t;
}

// A built-in by name, keeping `visualizer`; unknown reads as CLASSIC.
inline Theme theme_by_name(const std::string& name, VisualizerType visualizer) {
    Theme found = theme_classic();
    for (const Theme& t : theme_builtins()) {
        if (t.name == name) { found = t; break; }
    }
    found.visualizerType = visualizer;
    return found;
}

}  // namespace pt::ui
