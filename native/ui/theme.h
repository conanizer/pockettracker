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

    // ⚠️ THE ACCENT OF THE WHOLE PALETTE, not just a row highlight, and that is why it is bright here
    // rather than the near-black grey it was. It is the bar behind the cursor cell, the ink of every
    // "you are here" marker that has no bar (a row number, a column heading, the label of the row the
    // cursor is on), the EQ response curve and the selected meter's frame. `background` is the ink
    // that reads ON it, so anything dark enough to lose that ink makes the cursor cell unreadable.
    Argb rowCursor    = 0xFFFFFF00;  // the accent — cursor bar, active labels, the EQ curve
    // ⚠️ NO EDITOR ROW, AND IT PAINTS NOTHING — it is the SEED for `textPlayhead` below, and that is
    // its whole job since the full-row playback highlight was deleted. A `.ptt` written before TXT PLAY
    // existed carries only this, and `derive_borrowed_colors` is what turns it back into the marker
    // colour that file has always drawn.
    Argb rowPlayback  = 0xFF004400;  // the pre-TXT PLAY seed

    // ⚠️ A SECOND BRIGHT GROUND, ON THE SAME TERMS AS `rowCursor` — `rowEvery4th` is the ink that reads
    // on it. The two grounds are meant to be told apart at a glance and NOT to be far apart: a
    // selection and a cursor are both "the thing you are working on", and a big luminance gap between
    // them reads as two unrelated states.
    Argb rowSelection = 0xFF00CC00;  // selection region

    // ── Text roles ───────────────────────────────────────────────────────────────────────────────
    Argb textTitle  = 0xFF00FFFF;  // screen headers (cyan)
    Argb textParam  = 0xFF808080;  // inactive param label
    Argb textValue  = 0xFFFFFFFF;  // inactive param value
    Argb textEmpty  = 0xFF666666;  // empty / placeholder

    // ── Three colours nothing draws any more ─────────────────────────────────────────────────────
    //
    // ⚠️ DEAD TO THE SCREEN, ALIVE TO THE FILE FORMAT, AND THAT IS WHY THEY ARE STILL HERE. The cursor
    // cell's ink is now `background` and the selected cell's is `rowEvery4th`, both read straight off
    // the ground beside them; the EQ panel fills with `background`. Deleting the fields would delete
    // three keys from the `.ptt`, which is a file-format break for every theme already on an SD card —
    // so they are still parsed, still written on the same terms as before, and simply never read by a
    // draw. `theme_color_rows()` no longer lists them, so nothing can dial them either.
    //
    // ⚠️ `derive_borrowed_colors` STILL COMPUTES `textSelection` AND `eqBg` FROM THE SAME SOURCES, and
    // must keep doing so: it is the yardstick `serialize_theme` omits a key against, so changing it
    // would rewrite the bytes of files a previous build wrote.
    Argb textCursor = 0xFFFFFF00;  // unused; was the cursor cell's ink
    Argb textSelection = 0xFF00FF00;  // unused; = vizWave

    // ⚠️ NOT AN INDEPENDENT DEFAULT EITHER — `derive_borrowed_colors` lifts it out of `rowPlayback`,
    // which is what keeps a `.ptt` that never named this key drawing the marker it has always drawn.
    // The literal is CLASSIC's own seed lifted: 0xFF004400 → 0xFF00E000.
    //
    // ⚠️ THE LIFT IS THE DEFAULT AND NOT THE DRAW. Once the key is in a file, the value is used AS
    // TYPED — a marker the same colour as ROW SELECT is now a marker that vanishes into a selection,
    // which is what a colour row is supposed to let someone do.
    Argb textPlayhead = 0xFF00E000;  // the `>` playback marker's ink

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
    //
    // ⚠️ `eqBorder` DRAWS TWO THINGS — the spectrum's outline and the 0 dB line under the response
    // curve — so it is the panel's whole "reference" colour. They were separate and looked identical:
    // the 0 dB line borrowed vizCenterLine, which on a blue palette is a shade off the curve's own.
    Argb eqBg     = 0xFF0A0A0A;  // unused; = vizBackground
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

/** Multiply the RGB channels by `factor` (0..1 darker, >1 brighter); alpha preserved. Int.darken(). */
inline Argb darken(Argb c, float factor) {
    auto ch = [&](int shift) {
        const int v = static_cast<int>(static_cast<float>((c >> shift) & 0xFF) * factor);
        return static_cast<Argb>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    return (c & 0xFF000000u) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}


/**
 * ROW PLAY's value, lifted to something readable as INK — the default `textPlayhead` takes when a
 * `.ptt` does not name TXT PLAY.
 *
 * ⚠️ IT EXISTS BECAUSE THE SEED IS A BACKGROUND COLOUR. Every value `rowPlayback` has ever held was
 * chosen to sit BEHIND text on a dark screen (CLASSIC's is 0xFF004400), and ink that dark on
 * `background` cannot be read. So the hue is the theme's and the brightness is not: scale all three
 * channels until the strongest reaches `TARGET`. That keeps green green, amber amber and blue blue
 * across the four built-ins and across anything a user typed into an older file.
 *
 * A seed already that bright scales by ~1 and is left alone; pure black has no hue to keep, so it
 * falls back to the cursor colour rather than staying invisible.
 *
 * ⚠️ THIS RUNS ONCE, AS A DEFAULT — never on the value the user typed into TXT PLAY. Lifting on every
 * draw is what made a dark marker unreachable and made two rows set to one colour draw as two.
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
// ⚠️ THESE SIX KEYS ARE THE ONLY ONES WHOSE DEFAULT IS A FUNCTION OF THE THEME, and it has to be:
// their default is *what the screen drew before they existed*, which was five other fields of the
// same palette. A constant default would restyle every `.ptt` already on an SD card the moment it
// loaded into a build that has these keys — the EQ fill of a light theme would jump from that theme's
// own shaded param colour to CLASSIC's dark grey, and its selected cells from its own wave colour to
// CLASSIC's green.
//
// So this one function is the authority, and BOTH ends read it: `parse_theme` fills in whichever of
// the six a file does not carry, and `serialize_theme` omits whichever still equals it. That is the
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
    t.textPlayhead  = lift_seed_to_ink(t.rowPlayback, t.textCursor);
}

// ─── The editable colours ────────────────────────────────────────────────────────────────────────
//
// The THEME EDITOR's row list — Kotlin's `ThemeEditorModule.COLOR_ROWS`, in the same order, with the
// same labels. It lives HERE, next to the fields it projects, rather than in the module: it is a view
// of `Theme`'s own field list, and a table that can drift out of step with the struct it describes is
// a bug waiting for someone to add a colour. Three consumers read it (the module draws it, the
// dispatcher's colour nudge indexes it, and the ptinput golden sweeps it) and none may re-derive it.
//
// ⚠️ NINETEEN ROWS, TWENTY-FOUR COLOURS — five fields have no row, and they are serialized into a
// `.ptt` all the same:
//
//   * `meterBorder` — read by the mixer's meter frames, and simply never given a way to edit it.
//   * `rowPlayback` — the seed TXT PLAY defaults from. A row for it would be a second control over
//     one colour, which is how one of the two becomes a lie.
//   * `textCursor`, `textSelection`, `eqBg` — nothing draws them at all any more (see the struct).
//     Their rows went because the colour each one named is now read off the ground beside it: the
//     cursor cell's ink is `background`, the selected cell's is `rowEvery4th`, the EQ panel's fill is
//     `background`. A row per colour that can only ever hold ONE right answer is a row that can be
//     set wrong, and three of them were.
//
// ⚠️ THE ROW ORDER IS GROUPED BY PREFIX and the groups are what a reader scans by, so a new colour
// joins its group rather than landing at the end. ⚠️⚠️ BUT THE POSITION IS A NUMBER, NOT A LABEL:
// the dispatcher's colour nudge indexes this table and ptinput's THEME sweep records the index in
// every line, so moving a row re-points every swept line at or below it. The recorded lines are then
// a PERMUTATION of the ones already there — the same beds, the same nudges, re-labelled — and never a
// re-recording, which would certify whatever the table happens to say today.
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
    // ⚠️ AFTER THE DERIVE, AND THAT ORDER IS THE POINT — these two are dialled, not borrowed, so the
    // derive would overwrite them. The outline sits a shade off TXT PARAM, and the playback marker is
    // the deep blue TXT EMPTY carries.
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
    t.textParam     = 0xFFC0C0C0;
    t.textValue     = 0xFFC0C0C0;
    t.textEmpty     = 0xFF444444;
    t.vizCenterLine = 0xFF222222;
    t.vizWave       = 0xFFCCCCCC;
    t.meterLow      = 0xFFB0B0B0;
    t.meterMid      = 0xFF808080;
    t.meterHigh     = 0xFF444444;
    derive_borrowed_colors(t);
    // ⚠️ AFTER THE DERIVE — dialled, not borrowed. TXT PARAM is bright here, so the derived fill and
    // outline would both come out too light to read the curve against; these two are set by hand.
    t.eqFill        = 0xFF444444;
    t.eqBorder      = 0xFF808080;
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
