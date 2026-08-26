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

    // ⚠️ NOT AN INDEPENDENT DEFAULT — `derive_borrowed_colors` computes it, as it does the EQ four
    // below, and that function is the authority. The literal here is only what a bare `Theme t;`
    // gets, and it is CLASSIC's own `vizWave`.
    Argb textSelection = 0xFF00FF00;  // = vizWave — a selected cell's ink

    // ── Visualizer (oscilloscope bar) ────────────────────────────────────────────────────────────
    Argb vizBackground = 0xFF0A0A0A;
    Argb vizCenterLine = 0xFF333333;
    Argb vizWave       = 0xFF00FF00;  // waveform line / bar fill

    // ── The EQ editor's spectrum panel ───────────────────────────────────────────────────────────
    //
    // Four colours the screen used to BORROW — the panel from vizBackground, the outline and its
    // shaded fill from textParam, the frequency labels from vizCenterLine. The fill was the one that
    // hurt: it was `darken(textParam, 0.27f)`, a shade with no key of its own, so a light palette got
    // a muddy grey wash under its own curve and no row to fix it on.
    //
    // ⚠️ THESE VALUES ARE NOT INDEPENDENT DEFAULTS — they are what `derive_borrowed_colors` computes
    // for the CLASSIC palette, and that function is the authority. Every producer of a Theme runs it;
    // the literals here are only what a bare `Theme t;` gets, and they are the same four numbers.
    Argb eqBg     = 0xFF0A0A0A;  // = vizBackground
    Argb eqFill   = 0xFF222222;  // = darken(textParam, 0.27f)
    Argb eqBorder = 0xFF808080;  // = textParam
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

/** Multiply the RGB channels by `factor` (0..1 darker, >1 brighter); alpha preserved. Int.darken(). */
inline Argb darken(Argb c, float factor) {
    auto ch = [&](int shift) {
        const int v = static_cast<int>(static_cast<float>((c >> shift) & 0xFF) * factor);
        return static_cast<Argb>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    return (c & 0xFF000000u) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// ─── The colours a theme has not named ───────────────────────────────────────────────────────────
//
// ⚠️ THESE FIVE KEYS ARE THE ONLY ONES WHOSE DEFAULT IS A FUNCTION OF THE THEME, and it has to be:
// their default is *what the screen drew before they existed*, which was five other fields of the
// same palette. A constant default would restyle every `.ptt` already on an SD card the moment it
// loaded into a build that has these keys — the EQ fill of a light theme would jump from that theme's
// own shaded param colour to CLASSIC's dark grey, and its selected cells from its own wave colour to
// CLASSIC's green.
//
// So this one function is the authority, and BOTH ends read it: `parse_theme` fills in whichever of
// the five a file does not carry, and `serialize_theme` omits whichever still equals it. That is the
// same encodeDefaults=false bargain the other seventeen colours get, with the yardstick derived per
// theme instead of read off `Theme{}` — and it keeps a saved theme's bytes identical to what an older
// build wrote until the user actually dials one of these rows.
//
// ⚠️ ONE FUNCTION, NOT TWO, AND EVERY SITE CALLS IT LAST. A second derive beside this one is a call
// every future producer of a Theme has to remember, and the cost of forgetting is a palette that
// looks right in four screens and wrong in the fifth.
//
// 0.27f is the shade the EQ fill was hardcoded to. It stays here and nowhere else.
inline void derive_borrowed_colors(Theme& t) {
    t.eqBg     = t.vizBackground;
    t.eqFill   = darken(t.textParam, 0.27f);
    t.eqBorder = t.textParam;
    t.eqTxt    = t.vizCenterLine;

    t.textSelection = t.vizWave;
}

// ─── The editable colours ────────────────────────────────────────────────────────────────────────
//
// The THEME EDITOR's row list — Kotlin's `ThemeEditorModule.COLOR_ROWS`, in the same order, with the
// same labels. It lives HERE, next to the fields it projects, rather than in the module: it is a view
// of `Theme`'s own field list, and a table that can drift out of step with the struct it describes is
// a bug waiting for someone to add a colour. Three consumers read it (the module draws it, the
// dispatcher's colour nudge indexes it, and the ptinput golden sweeps it) and none may re-derive it.
//
// ⚠️ TWENTY-TWO ROWS, TWENTY-THREE COLOURS — `meterBorder` HAS NO ROW. It is a field on the theme, it
// is serialized into a `.ptt`, it is read by the mixer's meter frames, and there is simply no way to
// edit it in the UI. Left that way rather than "fixed": it is the one colour whose row would sit in
// the golden with nothing on either side of it to say what it did.
//
// ⚠️ A NEW ROW IS APPENDED, NEVER INSERTED, even when it would read better beside its neighbours. A
// row's position IS its number to the dispatcher's colour nudge, and the golden records that number
// in every recorded line — so inserting TXT SELECT next to TXT EMPTY would silently re-point every
// swept line below it at a different colour.
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
        {"EQ BG",      &Theme::eqBg},
        {"EQ FILL",    &Theme::eqFill},
        {"EQ BORDER",  &Theme::eqBorder},
        {"EQ TXT",     &Theme::eqTxt},
        {"TXT SELECT", &Theme::textSelection},
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
    derive_borrowed_colors(t);   // AFTER the palette — it reads four of the fields set above
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
    derive_borrowed_colors(t);
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
    derive_borrowed_colors(t);
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

/**
 * The palette and visualizer a first launch comes up in — BLUE with the OCTA bars.
 *
 * ⚠️ Not `Theme{}`. `Theme{}` IS the CLASSIC palette (`theme_classic` returns the field defaults), so moving
 * the app's opening look into the struct's field defaults would redefine one of the four built-ins
 * rather than choose between them. This picks; it does not edit.
 *
 * It applies to a launch with no settings.json and nothing else: `load_settings` replaces both the
 * palette and the visualizer from the file, so a user who has ever quit the app keeps what they had.
 */
inline Theme theme_default() {
    Theme t          = theme_blue();
    t.visualizerType = VisualizerType::OCTA;
    return t;
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
