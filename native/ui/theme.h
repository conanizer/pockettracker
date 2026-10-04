#pragma once

// ─── The theme ───────────────────────────────────────────────────────────────────────────────────
//
// The palette fields and the four built-ins. ⚠️ The field names are a FILE FORMAT: `.ptt` files are
// JSON keyed by exactly these names, shared between devices — a rename is a format break.

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

    // ⚠️ THE ACCENT OF THE WHOLE PALETTE: the bar behind the cursor cell, every "you are here" mark
    // without a bar (row numbers, column headings, the cursor row's label), the EQ curve, the selected
    // meter's frame. `background` is the ink ON it, so it must stay far from dark.
    Argb rowCursor    = 0xFFFFFF00;  // the accent — cursor bar, active labels, the EQ curve
    // ⚠️ No editor row and paints nothing: it is the SEED `derive_borrowed_colors` turns into
    // `textPlayhead` for a `.ptt` that has no TXT PLAY.
    Argb rowPlayback  = 0xFF004400;  // the pre-TXT PLAY seed

    // ⚠️ A second bright ground on `rowCursor`'s terms (`rowEvery4th` reads on it). Meant to be told
    // apart from the cursor at a glance but NOT far from it — both are "the thing you are working on".
    Argb rowSelection = 0xFF00CC00;  // selection region

    // ── Text roles ───────────────────────────────────────────────────────────────────────────────
    Argb textTitle  = 0xFF00FFFF;  // screen headers (cyan)
    Argb textParam  = 0xFF808080;  // inactive param label
    Argb textValue  = 0xFFFFFFFF;  // inactive param value
    Argb textEmpty  = 0xFF666666;  // empty / placeholder

    // ── Two colours nothing draws ────────────────────────────────────────────────────────────────
    //
    // ⚠️ Dead to the screen, alive to the file format: removing them would drop two keys from every
    // `.ptt`. Still parsed and written, never drawn, and not in `theme_color_rows()`. The cursor cell's
    // ink is `background` and the selected cell's `rowEvery4th`.
    // ⚠️ `derive_borrowed_colors` still computes `textSelection` and `eqBg`: it is the yardstick
    // `serialize_theme` omits against, and changing it would rewrite existing files' bytes.
    Argb textCursor = 0xFFFFFF00;  // unused
    Argb textSelection = 0xFF00FF00;  // unused; = vizWave

    // ⚠️ Its DEFAULT is lifted from `rowPlayback` (`derive_borrowed_colors`), so a `.ptt` without this key
    // draws the marker it always did (CLASSIC: 0xFF004400 → 0xFF00E000). Once in a file it is used AS
    // TYPED — matching ROW SELECT makes the marker vanish in a selection, as the user chose.
    Argb textPlayhead = 0xFF00E000;  // the `>` playback marker's ink

    // ── Visualizer (oscilloscope bar) ────────────────────────────────────────────────────────────
    Argb vizBackground = 0xFF0A0A0A;
    Argb vizCenterLine = 0xFF333333;
    Argb vizWave       = 0xFF00FF00;  // waveform line / bar fill

    // ── The EQ editor's spectrum panel ───────────────────────────────────────────────────────────
    //
    // ⚠️ Not independent defaults: these are what `derive_borrowed_colors` computes for CLASSIC, and that
    // function is the authority (every Theme producer runs it); the literals only serve a bare `Theme t;`.
    // `eqBorder` draws both the spectrum outline and the 0 dB line — the panel's reference colour.
    // `eqBg` has no editor row on purpose: a palette sets it as a COPY of `background` or
    // `vizBackground` (a near-value reads as a rendering fault), so a row would be a second control.
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

/** Multiply the RGB channels by `factor` (0..1 darker, >1 brighter); alpha preserved. */
inline Argb darken(Argb c, float factor) {
    auto ch = [&](int shift) {
        const int v = static_cast<int>(static_cast<float>((c >> shift) & 0xFF) * factor);
        return static_cast<Argb>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    return (c & 0xFF000000u) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}


/**
 * ROW PLAY's value lifted to readable INK — `textPlayhead`'s default when a `.ptt` has no TXT PLAY. The
 * seed was always a dark BACKGROUND colour, so the hue is kept and all three channels scale until the
 * strongest reaches `TARGET` (green stays green, amber amber). Pure black has no hue: the cursor
 * colour instead.
 * ⚠️ Runs ONCE, as a default — never on a value the user typed.
 */
inline Argb lift_seed_to_ink(Argb seed, Argb fallback) {
    constexpr int TARGET = 0xE0;
    const int r = (seed >> 16) & 0xFF, g = (seed >> 8) & 0xFF, b = seed & 0xFF;
    const int peak = (r > g ? r : g) > b ? (r > g ? r : g) : b;
    if (peak == 0) return fallback;
    if (peak >= TARGET) return 0xFF000000u | (seed & 0x00FFFFFFu);
    return darken(seed, static_cast<float>(TARGET) / static_cast<float>(peak));
}
// ─── The colours a theme has not named ───────────────────────────────────────────────────────────
//
// ⚠️ These six keys' DEFAULT is a function of the theme: what the screen drew before they existed was
// other fields of the same palette, and a constant default would restyle every existing `.ptt` (a
// light theme's EQ fill jumping to CLASSIC's grey).
// This one function is the authority for BOTH ends: `parse_theme` fills in whichever a file lacks, and
// `serialize_theme` omits whichever still equals it — so a theme's bytes stay unchanged until one of
// these rows is dialled.
// ⚠️ Every producer of a Theme calls it LAST. 0.27f is the EQ fill's shade, here and nowhere else.
inline void derive_borrowed_colors(Theme& t) {
    t.eqBg     = t.vizBackground;
    t.eqFill   = darken(t.textParam, 0.27f);
    t.eqBorder = t.textParam;
    t.eqTxt    = t.vizCenterLine;

    t.textSelection = t.vizWave;
    t.textPlayhead  = lift_seed_to_ink(t.rowPlayback, t.textCursor);
}

// ─── The editable colours ────────────────────────────────────────────────────────────────────────
//
// The THEME EDITOR's row list, beside the fields it projects. The module draws it, the dispatcher's
// colour nudge indexes it, the input-test golden sweeps it — none may re-derive it.
// ⚠️ Nineteen rows for twenty-four colours; five fields have no row and are still serialized:
//   * `meterBorder` — read by the meter frames, never given a row;
//   * `rowPlayback` — the seed TXT PLAY defaults from (a row would be a second control over one colour);
//   * `textCursor`, `textSelection` — drawn by nothing (see the struct);
//   * `eqBg` — only ever a COPY of another ground.
// ⚠️ Rows are GROUPED by prefix, so a new colour joins its group. ⚠️⚠️ But a POSITION is a number: moving
// a row re-points every recorded THEME test line at or below it — the lines then permute, and are
// never re-recorded.
// A POINTER-TO-MEMBER, not a get/set pair: one member reads and writes the same field by construction,
// and a typo is a compile error rather than a colour that edits its neighbour.

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
    derive_borrowed_colors(t);   // AFTER the palette — it reads five of the fields set above
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
    // ⚠️ AFTER the derive, which would overwrite these two dialled colours: the outline a shade off TXT
    // PARAM, and the marker the deep blue of TXT EMPTY.
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
    // ⚠️ A shade off TXT VALUE on purpose, so value, label and placeholder do not read alike (dialled on a
    // device).
    t.textParam     = 0xFF8B8E8E;
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

/**
 * The built-ins, in the order the theme cycle walks them.
 * ⚠️ `visualizerType` is a field but not part of a theme's identity — the palette is the theme's, the
 * visualizer the user's. Anything that swaps a theme preserves it (`theme_by_name` takes it).
 */
inline std::vector<Theme> theme_builtins() {
    return {theme_classic(), theme_amber(), theme_blue(), theme_mono()};
}

/**
 * A first launch's look — BLUE with the OCTA bars. ⚠️ Not `Theme{}`, which IS the CLASSIC palette:
 * changing the field defaults would redefine a built-in. Only a launch with no settings.json uses this.
 */
inline Theme theme_default() {
    Theme t          = theme_blue();
    t.visualizerType = VisualizerType::OCTA;
    return t;
}

/** A built-in by name, keeping `visualizer`. An unknown name reads as CLASSIC. */
inline Theme theme_by_name(const std::string& name, VisualizerType visualizer) {
    Theme found = theme_classic();
    for (const Theme& t : theme_builtins()) {
        if (t.name == name) { found = t; break; }
    }
    found.visualizerType = visualizer;
    return found;
}

}  // namespace pt::ui
