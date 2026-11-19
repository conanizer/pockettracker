#pragma once

// The palette and the four built-ins. ⚠️ The field names are the `.ptt` file's keys — a rename breaks
// the format.

#include <cstdint>
#include <string>
#include <vector>

#include "ui/help_topic.h"

namespace pt::ui {

using Argb = uint32_t;  // 0xAARRGGBB

// What the top panel shows (SETTINGS > TOP PANEL): a visualizer, nothing, or the guide — a line about
// the cell under the cursor and two about its buttons (ui/guide.h).
// ⚠️ The number is what settings.json stores: append, never reorder.
enum class VisualizerType { SCOPE, FLAT, OCTA, OCTA_FULL, SPECTRUM, SPECTRUM_PEAKS, GUIDE };
inline constexpr int VISUALIZER_TYPE_COUNT = 7;

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

// ─── Every colour, in the `.ptt` file's order ─────────────────────────────────────────────────────
// ⚠️ The order and the keys are the file format. `borrowed`: its default is derived from the rest
// (`derive_borrowed_colors`), so the file omits it while it equals that. `hue`: a held row of this
// colour may name the hue a theme roll keeps — false for the grounds and the meters.
struct ThemeColor {
    const char* key;
    Argb Theme::* field;
    bool borrowed;
    bool hue;
};

inline constexpr ThemeColor THEME_COLORS[] = {
    {"background",      &Theme::background,      false, false},
    {"rowEvery4th",     &Theme::rowEvery4th,     false, false},
    {"rowCursor",       &Theme::rowCursor,       false, true},
    {"rowPlayback",     &Theme::rowPlayback,     false, true},
    {"rowSelection",    &Theme::rowSelection,    false, true},
    {"textTitle",       &Theme::textTitle,       false, true},
    {"textParam",       &Theme::textParam,       false, true},
    {"textValue",       &Theme::textValue,       false, true},
    {"textCursor",      &Theme::textCursor,      false, true},
    {"textEmpty",       &Theme::textEmpty,       false, true},
    {"vizBackground",   &Theme::vizBackground,   false, false},
    {"vizCenterLine",   &Theme::vizCenterLine,   false, true},
    {"vizWave",         &Theme::vizWave,         false, true},
    {"meterBackground", &Theme::meterBackground, false, false},
    {"meterLow",        &Theme::meterLow,        false, false},
    {"meterMid",        &Theme::meterMid,        false, false},
    {"meterHigh",       &Theme::meterHigh,       false, false},
    {"meterBorder",     &Theme::meterBorder,     false, false},
    {"eqBg",            &Theme::eqBg,            true,  false},
    {"eqFill",          &Theme::eqFill,          true,  true},
    {"eqBorder",        &Theme::eqBorder,        true,  true},
    {"eqTxt",           &Theme::eqTxt,           true,  true},
    {"textSelection",   &Theme::textSelection,   true,  true},
    {"textPlayhead",    &Theme::textPlayhead,    true,  true},
};

inline const ThemeColor& theme_color(Argb Theme::* field) {
    for (const ThemeColor& c : THEME_COLORS)
        if (c.field == field) return c;
    return THEME_COLORS[0];
}

// ─── The theme editor's rows ─────────────────────────────────────────────────────────────────────
// Four colours have none: `meterBorder`, `rowPlayback` (a seed), `textCursor` and `textSelection`
// (unused). ⚠️ A row's position is its number in the tests: append.

struct ThemeColorRow {
    const char* label;
    Argb Theme::* field;
    HelpTopic help;
};

inline const std::vector<ThemeColorRow>& theme_color_rows() {
    static const std::vector<ThemeColorRow> rows = {
        {"BACKGROUND", &Theme::background,      HelpTopic::THEME_BACKGROUND},
        {"ROW 4TH",    &Theme::rowEvery4th,     HelpTopic::THEME_ROW_4TH},
        {"ROW CURSOR", &Theme::rowCursor,       HelpTopic::THEME_ROW_CURSOR},
        {"ROW SELECT", &Theme::rowSelection,    HelpTopic::THEME_ROW_SELECT},
        {"TXT TITLE",  &Theme::textTitle,       HelpTopic::THEME_TXT_TITLE},
        {"TXT PARAM",  &Theme::textParam,       HelpTopic::THEME_TXT_PARAM},
        {"TXT VALUE",  &Theme::textValue,       HelpTopic::THEME_TXT_VALUE},
        {"TXT EMPTY",  &Theme::textEmpty,       HelpTopic::THEME_TXT_EMPTY},
        {"TXT PLAY",   &Theme::textPlayhead,    HelpTopic::THEME_TXT_PLAY},
        {"VIZ BG",     &Theme::vizBackground,   HelpTopic::THEME_VIZ_BG},
        {"VIZ LINE",   &Theme::vizCenterLine,   HelpTopic::THEME_VIZ_LINE},
        {"VIZ WAVE",   &Theme::vizWave,         HelpTopic::THEME_VIZ_WAVE},
        {"MTR BG",     &Theme::meterBackground, HelpTopic::THEME_MTR_BG},
        {"MTR LOW",    &Theme::meterLow,        HelpTopic::THEME_MTR_LOW},
        {"MTR MID",    &Theme::meterMid,        HelpTopic::THEME_MTR_MID},
        {"MTR HIGH",   &Theme::meterHigh,       HelpTopic::THEME_MTR_HIGH},
        {"EQ FILL",    &Theme::eqFill,          HelpTopic::THEME_EQ_FILL},
        {"EQ BORDER",  &Theme::eqBorder,        HelpTopic::THEME_EQ_BORDER},
        {"EQ TXT",     &Theme::eqTxt,           HelpTopic::THEME_EQ_TXT},
        {"EQ BG",      &Theme::eqBg,            HelpTopic::THEME_EQ_BG},
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

// AMBER on a black ground, for OLED screens.
inline Theme theme_amber_oled() {
    Theme t = theme_amber();
    t.name          = "AMBER OLED";
    t.background    = 0xFF000000;
    t.textValue     = 0xFFCC8800;
    t.vizBackground = 0xFF000000;
    derive_borrowed_colors(t);
    return t;
}

// BLUE on a black ground, for OLED screens.
inline Theme theme_blue_oled() {
    Theme t = theme_blue();
    t.name            = "BLUE OLED";
    t.background      = 0xFF000000;
    t.rowEvery4th     = 0xFF0A0A0A;
    t.vizBackground   = 0xFF000000;
    t.meterBackground = 0xFF0A0A0A;
    derive_borrowed_colors(t);
    // After the derive, which would overwrite these two.
    t.eqBorder      = 0xFF4488AA;
    t.textPlayhead  = 0xFF224466;
    return t;
}

inline Theme theme_bubblegum() {
    Theme t;
    t.name          = "BUBBLEGUM";
    t.background    = 0xFF0D100F;
    t.rowEvery4th   = 0xFF081F1C;
    t.rowCursor     = 0xFF00BBAA;
    t.rowPlayback   = 0xFF002266;
    t.rowSelection  = 0xFFFF8DAC;
    t.textTitle     = 0xFF39B0A1;
    t.textParam     = 0xFF9C7F85;
    t.textValue     = 0xFFBC949D;
    t.textEmpty     = 0xFF5E7A75;
    t.vizBackground = 0xFF0D100F;
    t.vizCenterLine = 0xFF213330;
    t.vizWave       = 0xFFBC6A80;
    t.meterBackground = 0xFF11201E;
    t.meterLow      = 0xFF00C3B1;
    t.meterMid      = 0xFFAF9000;
    t.meterHigh     = 0xFFC84D74;
    t.meterBorder   = 0xFF333535;
    derive_borrowed_colors(t);
    // After the derive, which would overwrite these.
    t.eqFill        = 0xFF233935;
    t.eqBorder      = 0xFFC6788C;
    t.eqTxt         = 0xFF557772;
    t.textPlayhead  = 0xFF489086;
    return t;
}

// The built-ins, written into the Themes folder once (`seed_theme_bank`). `visualizerType` is the
// user's, not the theme's: a swap keeps it.
// ⚠️ A name is what `settings.json` stores, so a renamed built-in reads back as CLASSIC.
inline std::vector<Theme> theme_builtins() {
    return {theme_classic(), theme_amber(), theme_blue(), theme_mono(),
            theme_bubblegum(), theme_blue_oled(), theme_amber_oled()};
}

// A built-in's name is the app's word (shown in the set letter case); a saved theme's is the user's.
inline bool theme_name_is_builtin(const std::string& name) {
    for (const Theme& t : theme_builtins())
        if (t.name == name) return true;
    return false;
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
