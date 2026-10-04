#pragma once

// ─── The theme generator ─────────────────────────────────────────────────────────────────────────
//
// A palette is built as a LADDER: grounds are steps of lightness away from `background`, inks steps
// of loudness against the ground each lands on. Independent colours give the classic bad palette — a
// beat stripe darker than its ground, a placeholder louder than its value.
// ⚠️⚠️ Every one-sided bound is signed by `theme_polarity`, never "lighter" — or the generator makes only
// dark themes while passing every test written on dark themes.
// ⚠️ A LOCK IS AN INPUT, not an exclusion: locked roles are fixed and the rest solved around them, so a
// lock set can be unsatisfiable — the caller is told and its palette left alone.

#include <cmath>
#include <cstdint>
#include <vector>

#include "ui/color_space.h"
#include "ui/theme.h"
#include "ui/theme_rules.h"

namespace pt::ui {

/** The hue relationship between the palette's colours, in the order the RANDOMIZE cell walks them.
 *  `ALL` constrains nothing ("just give me a palette"); the rest pick from fixed angles off one base. */
enum class ThemeScheme { ALL, MONO, ANALOG, COMP, SPLIT, TRIAD, TETRAD };

/// ⚠️ The ring the cell steps through, and the bound the screenshot test searches by name — beside the enum so a
/// new scheme cannot be unreachable.
inline constexpr int THEME_SCHEME_COUNT = 7;

inline const char* theme_scheme_label(ThemeScheme s) {
    switch (s) {
        case ThemeScheme::ALL:    return "ALL";
        case ThemeScheme::MONO:   return "MONOCHROME";
        case ThemeScheme::ANALOG: return "ANALOGOUS";
        case ThemeScheme::COMP:   return "COMPLEMENT";
        case ThemeScheme::SPLIT:  return "SPLIT-COMP";
        case ThemeScheme::TRIAD:  return "TRIADIC";
        case ThemeScheme::TETRAD: return "TETRADIC";
    }
    return "?";
}

/** One bit per editor colour row. Sized from the table so a new row cannot forget to be lockable. */
struct ThemeLocks {
    std::vector<bool> row;
    ThemeLocks() : row(theme_color_rows().size(), false) {}

    bool locked(int colorRowIndex) const {
        return colorRowIndex >= 0 && colorRowIndex < static_cast<int>(row.size()) &&
               row[static_cast<size_t>(colorRowIndex)];
    }
    void toggle(int colorRowIndex) {
        if (colorRowIndex >= 0 && colorRowIndex < static_cast<int>(row.size())) {
            row[static_cast<size_t>(colorRowIndex)] = !row[static_cast<size_t>(colorRowIndex)];
        }
    }
};

namespace random_detail {

/**
 * xorshift32 — the same sequence everywhere. ⚠️ Not `<random>`: its DISTRIBUTIONS differ across standard
 * libraries, and a seed that does not reproduce across builds cannot be reported in a bug.
 */
struct Rng {
    uint32_t s;

    /** ⚠️ The seed is MIXED first: xorshift32 avalanches poorly at the start, and the first draw is the
     *  palette's HUE — nearby seeds would give the same colour. One splitmix-style round fixes it. */
    explicit Rng(uint32_t seed) {
        uint32_t x = seed + 0x9E3779B9u;
        x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
        x = (x ^ (x >> 13)) * 0xC2B2AE35u;
        x ^= x >> 16;
        s = x ? x : 0x9E3779B9u;
    }

    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    /** Uniform in [lo, hi). */
    double range(double lo, double hi) {
        return lo + (hi - lo) * (static_cast<double>(next() >> 8) / 16777216.0);
    }
    int pick(int n) { return static_cast<int>(next() % static_cast<uint32_t>(n)); }
    bool chance(double p) { return range(0.0, 1.0) < p; }
};

/**
 * The scheme's hue wheel — the base plus the offsets the scheme allows. ⚠️ A scheme names ANGLES, not a
 * count: nineteen roles drawing from a three-hue scheme are nineteen draws from three angles, which is
 * what makes one family rather than three swatches.
 */
inline double scheme_hue(Rng& rng, ThemeScheme scheme, double baseHue) {
    switch (scheme) {
        case ThemeScheme::ALL:    return rng.range(0.0, 360.0);
        case ThemeScheme::MONO:   return baseHue;
        case ThemeScheme::ANALOG: return baseHue + rng.range(-30.0, 30.0);
        case ThemeScheme::COMP:   return baseHue + (rng.chance(0.5) ? 0.0 : 180.0);
        case ThemeScheme::SPLIT: {
            static constexpr double OFF[] = {0.0, 150.0, 210.0};   // 180° ± 30°
            return baseHue + OFF[rng.pick(3)];
        }
        case ThemeScheme::TRIAD:  return baseHue + 120.0 * static_cast<double>(rng.pick(3));
        case ThemeScheme::TETRAD: return baseHue + 90.0 * static_cast<double>(rng.pick(4));
    }
    return baseHue;
}

/**
 * The colour at lightness `L` with the most chroma that still clears `floorRatio` against `ground`.
 * ⭐ CHROMA is given up, never lightness or hue (the ladder owns L, the scheme owns H). If even grey
 * misses the floor, the best effort is returned and the validator reports it — moving L here would
 * make the generator disagree with its checker.
 */
inline Argb ink_at(double L, double hue, double chroma, Argb ground, double floorRatio) {
    Argb best = to_argb(OkLch{L, 0.0, hue});
    if (wcag_contrast(best, ground) < floorRatio) return best;   // grey is the most it can clear

    double lo = 0.0, hi = chroma;
    for (int i = 0; i < 12; ++i) {
        const double mid = 0.5 * (lo + hi);
        const Argb trial = to_argb(OkLch{L, mid, hue});
        if (wcag_contrast(trial, ground) >= floorRatio) { lo = mid; best = trial; } else { hi = mid; }
    }
    return best;
}

/** Walk L away from `ground` until the pair clears `floorRatio`, in the polarity's direction. */
inline double lightness_for(Argb ground, double floorRatio, int polarity, double start) {
    double L = start;
    for (int i = 0; i < 64; ++i) {
        if (wcag_contrast(to_argb(OkLch{L, 0.0, 0.0}), ground) >= floorRatio) return L;
        L += 0.015 * polarity;
        if (L > 1.0) return 1.0;
        if (L < 0.0) return 0.0;
    }
    return L;
}

}  // namespace random_detail

/**
 * What the HELD rows say the palette already is — its hue and how much colour it carries.
 * ⭐⭐ A lock contributes its COLOUR, not just its slot: holding a blue row and rolling MONOCHROME gives
 * shades of that blue; re-rolling a row of a grey palette gives grey.
 * ⚠️⚠️ Seven rows do not count: the four GROUNDS are near-grey by construction (a held background says
 * nothing about hue), and the three METER bars are a signal ramp meant to leave the palette (AMBER's red
 * MTR HIGH would win the hue and turn every re-roll red).
 * The polarity still comes from `background`, held or not.
 */
struct PaletteContext {
    bool   hasHue      = false;  ///< false when nothing held carries a visible tint
    double hue         = 0.0;
    double chromaScale = 1.0;    ///< 1.0 when nothing is held; 0 when everything held is grey
};

/// A tint below this reads as grey and names no hue.
inline constexpr double CONTEXT_MIN_CHROMA = 0.02;
/// The chroma a fully coloured palette's accent reaches — what a held colour is measured against.
inline constexpr double CONTEXT_FULL_CHROMA = 0.20;

inline PaletteContext theme_context(const Theme& base, const ThemeLocks& locks) {
    PaletteContext ctx;

    const auto& rows = theme_color_rows();
    bool   any  = false;
    double best = 0.0;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!locks.locked(static_cast<int>(i))) continue;
        Argb Theme::* const f = rows[i].field;
        if (f == &Theme::background || f == &Theme::rowEvery4th ||
            f == &Theme::vizBackground || f == &Theme::meterBackground ||
            f == &Theme::meterLow || f == &Theme::meterMid || f == &Theme::meterHigh) {
            continue;   // a ground or a meter bar — neither names the palette
        }
        any = true;
        // ⭐ The MOST COLOURFUL held row names the hue — averaging hues around a wheel is meaningless.
        const OkLch c = to_oklch(base.*f);
        if (c.C > best) { best = c.C; ctx.hue = c.H; }
    }
    if (!any) return ctx;   // nothing held: a free roll

    ctx.hasHue      = best >= CONTEXT_MIN_CHROMA;
    ctx.chromaScale = (best < CONTEXT_FULL_CHROMA) ? (best / CONTEXT_FULL_CHROMA) : 1.0;
    return ctx;
}

struct ThemeRollResult {
    Theme  theme;            ///< the palette produced; only meaningful when `ok`
    bool   ok = false;       ///< false when no attempt satisfied the rules
    int    attempts = 0;
    size_t worstViolations = 0;   ///< the best attempt's violation count, for the message
};

/** Roll a palette. `base` supplies the locked rows and `visualizerType` (never part of a theme's
 *  identity). The caller sets the name. */
inline ThemeRollResult theme_roll(const Theme& base, const ThemeLocks& locks, ThemeScheme scheme,
                                  uint32_t seed) {
    using namespace random_detail;

    const auto& rows = theme_color_rows();
    const auto locked_field = [&](Argb Theme::* f) {
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].field == f) return locks.locked(static_cast<int>(i));
        }
        return false;
    };

    /**
     * Is this colour beyond the roll's reach — locked, or standing on something locked?
     * ⚠️ A field with no editor row has no lock, but is not therefore free: `eqBg` copies `background` or
     * `vizBackground`, so with both held it is held too (treated as free, every single-row re-roll was
     * impossible). `meterBorder` IS rolled fresh every attempt.
     */
    const auto held = [&](Argb Theme::* f) {
        if (f == &Theme::eqBg) {
            return locked_field(&Theme::background) && locked_field(&Theme::vizBackground);
        }
        return locked_field(f);
    };

    const PaletteContext ctx = theme_context(base, locks);

    ThemeRollResult result;
    result.worstViolations = static_cast<size_t>(-1);

    // ⚠️ THE ATTEMPTS ARE THE SOLVER: each re-rolls only free roles and copies locked ones from `base`, so
    // an unaccommodatable lock fails every attempt rather than drifting.
    constexpr int MAX_ATTEMPTS = 200;
    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        Rng rng(seed + static_cast<uint32_t>(attempt) * 0x9E3779B9u);
        Theme t = base;

        // ⚠️ The hue is DRAWN even when the context overrides it, so a seed gives the same ladder whether
        // or not rows are held.
        const double rolledHue = rng.range(0.0, 360.0);
        const double baseHue   = ctx.hasHue ? ctx.hue : rolledHue;

        // ⭐ Every chroma is scaled by what the held rows carry (a grey palette re-rolls grey).
        const auto chroma = [&](double lo, double hi) { return rng.range(lo, hi) * ctx.chromaScale; };

        // ── The ground ladder ────────────────────────────────────────────────────────────────────
        if (!locked_field(&Theme::background)) {
            const bool dark = rng.chance(0.75);   // a tracker is a dark room; light palettes are rarer
            t.background = to_argb(OkLch{dark ? rng.range(0.04, 0.22) : rng.range(0.86, 0.97),
                                         chroma(0.0, 0.03), baseHue});
        }
        const int polarity = theme_polarity(t);
        const double groundL = ok_lightness(t.background);
        const auto step = [&](double lo, double hi) {
            return groundL + polarity * rng.range(lo, hi);
        };

        if (!locked_field(&Theme::rowEvery4th)) {
            t.rowEvery4th = to_argb(OkLch{step(0.050, 0.085), chroma(0.0, 0.03), baseHue});
        }
        if (!locked_field(&Theme::meterBackground)) {
            t.meterBackground = to_argb(OkLch{step(0.055, 0.115), chroma(0.0, 0.03), baseHue});
        }

        // ⭐ The sharing group: VIZ BG is the screen's ground or a lifted panel, and EQ BG COPIES one of the
        // two — three shapes, chosen by the roll. ⚠️ The lift stops short of the beat stripe's band, or
        // the EQ grid lines (drawn in that colour) vanish into the panel.
        if (!locked_field(&Theme::vizBackground)) {
            t.vizBackground = rng.chance(0.4)
                                  ? to_argb(OkLch{step(0.015, 0.033), chroma(0.0, 0.03), baseHue})
                                  : t.background;
        }

        // ⭐⭐ ONE helper owns every ink's lightness, and it only moves AWAY from the ground. Nudging back
        // toward the ground to quieten a role destroys the floor just solved; a quieter role has a LOWER
        // FLOOR instead.
        const auto away = [&](Argb ground, double floorRatio, double extra) {
            return lightness_for(ground, floorRatio, polarity, ok_lightness(ground)) +
                   polarity * extra;
        };
        /** The ground an ink is hardest to read on: furthest along the direction ink travels. */
        const auto harder = [&](Argb p, Argb q) {
            return (ok_lightness(p) * polarity > ok_lightness(q) * polarity) ? p : q;
        };

        // ── The two accents: grounds whose INK is read off the palette ───────────────────────────
        // ⚠️ Each is a ground AND an ink (`background` in the cursor bar, `rowEvery4th` in a selection, both
        // as marks on `meterBackground`), so each clears FLOOR_TEXT against the harder of its two.
        const double cursorExtra = rng.range(0.06, 0.18);
        if (!locked_field(&Theme::rowCursor)) {
            const Argb g = harder(t.background, t.meterBackground);
            t.rowCursor  = ink_at(away(g, FLOOR_TEXT, cursorExtra), scheme_hue(rng, scheme, baseHue),
                                  chroma(0.08, 0.20), g, FLOOR_TEXT);
        }
        if (!locked_field(&Theme::rowSelection)) {
            const Argb g = harder(t.rowEvery4th, t.meterBackground);
            // Selection and cursor are two shades of one idea: told apart, never far apart — a ladder
            // step, not a second free colour.
            t.rowSelection = ink_at(away(g, FLOOR_TEXT, cursorExtra + rng.range(0.04, 0.14)),
                                    scheme_hue(rng, scheme, baseHue), chroma(0.08, 0.20),
                                    g, FLOOR_TEXT);
        }

        // ── The ink ladder, loudest first ────────────────────────────────────────────────────────
        // ⚠️ Every text role lands on four grounds, so it is solved against the HARDEST — the one furthest
        // along the ink's direction (closest to the ink).
        Argb hardest = t.background;
        for (Argb g : {t.rowEvery4th, t.vizBackground, t.meterBackground}) {
            if (ok_lightness(g) * polarity > ok_lightness(hardest) * polarity) hardest = g;
        }

        // ⚠️ The ladder's ORDER is the order of the floors, and extra steps only widen it: TXT EMPTY at
        // its floor, TXT PARAM a step past, TXT VALUE and TXT TITLE past the higher floor — so a
        // placeholder can never be louder than its value.
        if (!locked_field(&Theme::textEmpty)) {
            t.textEmpty = ink_at(away(hardest, FLOOR_SUPPORT, rng.range(0.00, 0.03)),
                                 scheme_hue(rng, scheme, baseHue), chroma(0.0, 0.05),
                                 hardest, FLOOR_SUPPORT);
        }
        if (!locked_field(&Theme::textParam)) {
            t.textParam = ink_at(away(hardest, FLOOR_SUPPORT, rng.range(0.07, 0.13)),
                                 scheme_hue(rng, scheme, baseHue), chroma(0.0, 0.05),
                                 hardest, FLOOR_SUPPORT);
        }
        if (!locked_field(&Theme::textValue)) {
            t.textValue = ink_at(away(hardest, FLOOR_TEXT, rng.range(0.04, 0.12)),
                                 scheme_hue(rng, scheme, baseHue), chroma(0.0, 0.06),
                                 hardest, FLOOR_TEXT);
        }
        if (!locked_field(&Theme::textTitle)) {
            // The one loud role allowed real colour — how a palette says its hue.
            t.textTitle = ink_at(away(hardest, FLOOR_TEXT, rng.range(0.00, 0.10)),
                                 scheme_hue(rng, scheme, baseHue), chroma(0.10, 0.22),
                                 hardest, FLOOR_TEXT);
        }
        if (!locked_field(&Theme::textPlayhead)) {
            t.textPlayhead = ink_at(away(hardest, FLOOR_SUPPORT, rng.range(0.04, 0.14)),
                                    scheme_hue(rng, scheme, baseHue), chroma(0.06, 0.20),
                                    hardest, FLOOR_SUPPORT);
        }

        // ── The visualizer and the meters ────────────────────────────────────────────────────────
        if (!locked_field(&Theme::vizWave)) {
            // ⚠️ OCTA paints the strip in `background` and the other modes in `vizBackground`, so the wave
            // must clear the harder of the two.
            const Argb vizGround =
                (ok_lightness(t.vizBackground) * polarity > ok_lightness(t.background) * polarity)
                    ? t.vizBackground : t.background;
            t.vizWave = ink_at(away(vizGround, FLOOR_TEXT, rng.range(0.02, 0.12)),
                               scheme_hue(rng, scheme, baseHue), chroma(0.10, 0.25),
                               vizGround, FLOOR_TEXT);
        }
        if (!locked_field(&Theme::vizCenterLine)) {
            t.vizCenterLine = to_argb(OkLch{away(t.vizBackground, FLOOR_DECOR, rng.range(0.0, 0.04)),
                                            chroma(0.0, 0.04), baseHue});
        }

        // ⚠️ An ORDERED TRIPLE, not three colours (three unrelated hues on one meter reads as a fault).
        {
            // ⚠️⚠️ Both ENDS are scheme hues and the middle lies between them, so MONOCHROME's meter stays
            // one hue and TRIADIC gets a real ramp.
            const double hLow  = scheme_hue(rng, scheme, baseHue);
            const double hHigh = scheme_hue(rng, scheme, baseHue);
            // ⚠️ The SHORT way round: 350° to 10° passes through 0, not 180.
            const double arc   = std::fmod(hHigh - hLow + 540.0, 360.0) - 180.0;
            const double hMid  = hLow + 0.5 * arc;
            const double trough = rng.range(0.02, 0.08);
            const double rise   = rng.range(0.03, 0.09);
            // ⚠️ The ramp may run either way in lightness (MONO's darkens as it gets loud); only the ORDER
            // is fixed. Every rung is measured away from the trough, so no end falls through its floor.
            const bool   up     = rng.chance(0.5);
            const double eL = trough + (up ? 0.0 : 2.0 * rise);
            const double eM = trough + rise;
            const double eH = trough + (up ? 2.0 * rise : 0.0);
            const auto bar = [&](double extra, double hue) {
                return ink_at(away(t.meterBackground, FLOOR_SUPPORT, extra), hue, 0.16 * ctx.chromaScale,
                              t.meterBackground, FLOOR_SUPPORT);
            };
            if (!locked_field(&Theme::meterLow))  t.meterLow  = bar(eL, hLow);
            if (!locked_field(&Theme::meterMid))  t.meterMid  = bar(eM, hMid);
            if (!locked_field(&Theme::meterHigh)) t.meterHigh = bar(eH, hHigh);

            t.meterBorder = to_argb(OkLch{away(t.background, FLOOR_DECOR, rng.range(0.0, 0.05)),
                                          chroma(0.0, 0.03), baseHue});
        }

        // ── The EQ panel ─────────────────────────────────────────────────────────────────────────
        // ⚠️⚠️ The derive runs LAST and overwrites every borrowed key, so what the generator owns that the
        // derive also computes is put back after it. ⚠️ Including TXT PLAY: its seed `rowPlayback` is never
        // rolled, so the derive would otherwise restore CLASSIC's green marker.
        const Argb eqGround = rng.chance(0.5) ? t.background : t.vizBackground;
        const Argb rolledPlayhead = t.textPlayhead;
        derive_borrowed_colors(t);
        t.eqBg         = eqGround;
        t.textPlayhead = rolledPlayhead;
        // ⚠️ EQ FILL is the wash under the curve AND the gridlines — the one decor role that must be seen.
        // EQ BORDER is drawn over it and the panel, so it clears the harder of the two.
        if (!locked_field(&Theme::eqFill)) {
            t.eqFill = to_argb(OkLch{away(eqGround, FLOOR_DECOR, rng.range(0.0, 0.04)),
                                     chroma(0.0, 0.05), baseHue});
        }
        if (!locked_field(&Theme::eqBorder)) {
            const Argb g = harder(eqGround, t.eqFill);
            t.eqBorder = ink_at(away(g, FLOOR_SUPPORT, rng.range(0.02, 0.10)),
                                scheme_hue(rng, scheme, baseHue), 0.10 * ctx.chromaScale, g, FLOOR_SUPPORT);
        }
        if (!locked_field(&Theme::eqTxt)) {
            t.eqTxt = ink_at(away(eqGround, FLOOR_SUPPORT, rng.range(0.0, 0.06)),
                             scheme_hue(rng, scheme, baseHue), 0.04 * ctx.chromaScale, eqGround, FLOOR_SUPPORT);
        }

        t.visualizerType = base.visualizerType;   // never part of a palette's identity
        t.name           = base.name;

        // ⚠️⚠️ A roll answers only for pairs it can still MOVE: a pair of two LOCKED colours (held on
        // purpose) would otherwise condemn every attempt — a single-row re-roll of any built-in would
        // never succeed. A pair with one free end is still the solver's, and still fails honestly.
        size_t bad = 0;
        for (const ThemeViolation& v : theme_violations(t, /*generator=*/true)) {
            if (!held(v.rule->a) || !held(v.rule->b)) ++bad;
        }
        if (bad == 0) {
            result.theme = t;
            result.ok = true;
            result.attempts = attempt + 1;
            result.worstViolations = 0;
            return result;
        }
        if (bad < result.worstViolations) { result.worstViolations = bad; result.theme = t; }
    }

    result.attempts = MAX_ATTEMPTS;
    return result;
}

}  // namespace pt::ui
