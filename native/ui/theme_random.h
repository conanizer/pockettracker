#pragma once

// The theme generator. A palette is a ladder: grounds are steps of lightness away from `background`,
// inks steps of loudness against the ground each lands on.
// ⚠️ Every one-sided bound is signed by `theme_polarity`, never "lighter", or light themes break.
// Locked rows are fixed and the rest solved around them, so a lock set can be unsatisfiable.

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "ui/color_space.h"
#include "ui/theme.h"
#include "ui/theme_rules.h"

namespace pt::ui {

// The hue relationship, in the order RANDOMIZE walks them. `ALL` constrains nothing; the rest pick
// from fixed angles off one base.
enum class ThemeScheme { ALL, MONO, ANALOG, COMP, SPLIT, TRIAD, TETRAD };
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

// One lock per editor colour row.
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

// xorshift32 — the same sequence on every build (`<random>`'s distributions differ between libraries).
struct Rng {
    uint32_t s;

    // The seed is mixed first, or nearby seeds would give the same first hue.
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
    // Uniform in [lo, hi).
    double range(double lo, double hi) {
        return lo + (hi - lo) * (static_cast<double>(next() >> 8) / 16777216.0);
    }
    int pick(int n) { return static_cast<int>(next() % static_cast<uint32_t>(n)); }
    bool chance(double p) { return range(0.0, 1.0) < p; }
};

// A hue the scheme allows: the base plus one of its angles.
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

// The colour at lightness `L` with the most chroma that still clears `floorRatio` against `ground`.
// Only chroma gives way (the ladder owns L, the scheme H); if grey misses too, the validator reports it.
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

// Walk L away from `ground`, in the polarity's direction, until the pair clears `floorRatio`.
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

// What the held rows say the palette already is: its hue and how much colour it carries — so holding a
// blue row and rolling MONOCHROME gives shades of that blue.
// The grounds (near-grey) and the meter bars (a signal ramp) do not count.
struct PaletteContext {
    bool   hasHue      = false;  // something held carries a visible tint
    double hue         = 0.0;
    double chromaScale = 1.0;    // 1 when nothing is held; 0 when everything held is grey
};

inline constexpr double CONTEXT_MIN_CHROMA = 0.02;    // below this a tint reads as grey
inline constexpr double CONTEXT_FULL_CHROMA = 0.20;   // a fully coloured palette's accent

inline PaletteContext theme_context(const Theme& base, const ThemeLocks& locks) {
    PaletteContext ctx;

    const auto& rows = theme_color_rows();
    bool   any  = false;
    double best = 0.0;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!locks.locked(static_cast<int>(i))) continue;
        Argb Theme::* const f = rows[i].field;
        if (!theme_color(f).hue) continue;
        any = true;
        // The most colourful held row names the hue.
        const OkLch c = to_oklch(base.*f);
        if (c.C > best) { best = c.C; ctx.hue = c.H; }
    }
    if (!any) return ctx;

    ctx.hasHue      = best >= CONTEXT_MIN_CHROMA;
    ctx.chromaScale = (best < CONTEXT_FULL_CHROMA) ? (best / CONTEXT_FULL_CHROMA) : 1.0;
    return ctx;
}

namespace random_detail {

inline bool field_locked(const ThemeLocks& locks, Argb Theme::* f) {
    const auto& rows = theme_color_rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].field == f) return locks.locked(static_cast<int>(i));
    }
    return false;
}

// One attempt's state. ⚠️ A seed's palette is its ORDER of draws: every step below draws in a fixed
// order, never two draws inside one argument list (that order is the compiler's).
struct Roll {
    Rng                   rng;
    ThemeScheme           scheme;
    const PaletteContext& ctx;
    const ThemeLocks&     locks;
    Theme                 t;
    double                baseHue  = 0.0;
    int                   polarity = 1;

    bool is_free(Argb Theme::* f) const { return !field_locked(locks, f); }
    // Scaled by what the held rows carry (a grey palette re-rolls grey).
    double chroma(double lo, double hi) { return rng.range(lo, hi) * ctx.chromaScale; }
    // An ink's lightness, only ever moving away from the ground: a quieter role gets a lower floor,
    // never a nudge back.
    double away(Argb ground, double floorRatio, double extra) const {
        return lightness_for(ground, floorRatio, polarity, ok_lightness(ground)) + polarity * extra;
    }
    // The ground an ink is hardest to read on.
    Argb harder(Argb p, Argb q) const {
        return (ok_lightness(p) * polarity > ok_lightness(q) * polarity) ? p : q;
    }
    // One ink on `ground`: its lift, hue and chroma drawn in that order.
    Argb ink(Argb ground, double floorRatio, double liftLo, double liftHi, double cLo, double cHi) {
        const double lift = rng.range(liftLo, liftHi);
        const double hue  = scheme_hue(rng, scheme, baseHue);
        const double c    = chroma(cLo, cHi);
        return ink_at(away(ground, floorRatio, lift), hue, c, ground, floorRatio);
    }
};

// The ground ladder, which sets the polarity everything after it is signed by.
inline void roll_grounds(Roll& r) {
    Theme& t = r.t;
    if (r.is_free(&Theme::background)) {
        const bool dark = r.rng.chance(0.75);
        t.background = to_argb(OkLch{dark ? r.rng.range(0.04, 0.22) : r.rng.range(0.86, 0.97),
                                     r.chroma(0.0, 0.03), r.baseHue});
    }
    r.polarity = theme_polarity(t);
    const double groundL = ok_lightness(t.background);
    const auto step = [&](double lo, double hi) { return groundL + r.polarity * r.rng.range(lo, hi); };

    if (r.is_free(&Theme::rowEvery4th)) {
        t.rowEvery4th = to_argb(OkLch{step(0.050, 0.085), r.chroma(0.0, 0.03), r.baseHue});
    }
    if (r.is_free(&Theme::meterBackground)) {
        t.meterBackground = to_argb(OkLch{step(0.055, 0.115), r.chroma(0.0, 0.03), r.baseHue});
    }
    // VIZ BG is the ground or a lifted panel; EQ BG copies one of the two. ⚠️ The lift stays short of
    // the beat stripe's, or the EQ grid lines vanish into the panel.
    if (r.is_free(&Theme::vizBackground)) {
        t.vizBackground = r.rng.chance(0.4)
                              ? to_argb(OkLch{step(0.015, 0.033), r.chroma(0.0, 0.03), r.baseHue})
                              : t.background;
    }
}

// The two accents: each a ground and an ink, so it clears the harder of its two grounds.
inline void roll_accents(Roll& r) {
    Theme& t = r.t;
    const double cursorExtra = r.rng.range(0.06, 0.18);
    if (r.is_free(&Theme::rowCursor)) {
        const Argb   g   = r.harder(t.background, t.meterBackground);
        const double hue = scheme_hue(r.rng, r.scheme, r.baseHue);
        const double c   = r.chroma(0.08, 0.20);
        t.rowCursor = ink_at(r.away(g, FLOOR_TEXT, cursorExtra), hue, c, g, FLOOR_TEXT);
    }
    if (r.is_free(&Theme::rowSelection)) {
        // A ladder step past the cursor, not a second free colour.
        t.rowSelection = r.ink(r.harder(t.rowEvery4th, t.meterBackground), FLOOR_TEXT, cursorExtra + 0.04,
                               cursorExtra + 0.14, 0.08, 0.20);
    }
}

// The ink ladder: every text role lands on four grounds, so it is solved on the hardest. The floors
// give the order — EMPTY, PARAM, then VALUE and TITLE — so a placeholder is never louder than its value.
inline void roll_text(Roll& r) {
    Theme& t = r.t;
    Argb hardest = t.background;
    for (Argb g : {t.rowEvery4th, t.vizBackground, t.meterBackground}) hardest = r.harder(g, hardest);

    if (r.is_free(&Theme::textEmpty))    t.textEmpty    = r.ink(hardest, FLOOR_SUPPORT, 0.00, 0.03, 0.0, 0.05);
    if (r.is_free(&Theme::textParam))    t.textParam    = r.ink(hardest, FLOOR_SUPPORT, 0.07, 0.13, 0.0, 0.05);
    if (r.is_free(&Theme::textValue))    t.textValue    = r.ink(hardest, FLOOR_TEXT,    0.04, 0.12, 0.0, 0.06);
    // The one loud role allowed real colour.
    if (r.is_free(&Theme::textTitle))    t.textTitle    = r.ink(hardest, FLOOR_TEXT,    0.00, 0.10, 0.10, 0.22);
    if (r.is_free(&Theme::textPlayhead)) t.textPlayhead = r.ink(hardest, FLOOR_SUPPORT, 0.04, 0.14, 0.06, 0.20);
}

inline void roll_visualizer(Roll& r) {
    Theme& t = r.t;
    if (r.is_free(&Theme::vizWave)) {
        // OCTA draws on `background`, the other modes on `vizBackground`.
        t.vizWave = r.ink(r.harder(t.vizBackground, t.background), FLOOR_TEXT, 0.02, 0.12, 0.10, 0.25);
    }
    if (r.is_free(&Theme::vizCenterLine)) {
        t.vizCenterLine = to_argb(OkLch{r.away(t.vizBackground, FLOOR_DECOR, r.rng.range(0.0, 0.04)),
                                        r.chroma(0.0, 0.04), r.baseHue});
    }
}

// The meter is an ordered ramp: both ends scheme hues, the middle between them.
inline void roll_meter(Roll& r) {
    Theme& t = r.t;
    const double hLow  = scheme_hue(r.rng, r.scheme, r.baseHue);
    const double hHigh = scheme_hue(r.rng, r.scheme, r.baseHue);
    // The short way round: 350° to 10° passes through 0.
    const double arc    = std::fmod(hHigh - hLow + 540.0, 360.0) - 180.0;
    const double hMid   = hLow + 0.5 * arc;
    const double trough = r.rng.range(0.02, 0.08);
    const double rise   = r.rng.range(0.03, 0.09);
    // Lightness may run either way; every rung is measured from the trough, above its floor.
    const bool   up = r.rng.chance(0.5);
    const double eL = trough + (up ? 0.0 : 2.0 * rise);
    const double eM = trough + rise;
    const double eH = trough + (up ? 2.0 * rise : 0.0);
    const auto bar = [&](double extra, double hue) {
        return ink_at(r.away(t.meterBackground, FLOOR_SUPPORT, extra), hue, 0.16 * r.ctx.chromaScale,
                      t.meterBackground, FLOOR_SUPPORT);
    };
    if (r.is_free(&Theme::meterLow))  t.meterLow  = bar(eL, hLow);
    if (r.is_free(&Theme::meterMid))  t.meterMid  = bar(eM, hMid);
    if (r.is_free(&Theme::meterHigh)) t.meterHigh = bar(eH, hHigh);

    t.meterBorder = to_argb(OkLch{r.away(t.background, FLOOR_DECOR, r.rng.range(0.0, 0.05)),
                                  r.chroma(0.0, 0.03), r.baseHue});
}

// The EQ panel, and the borrowed keys derived. ⚠️ The derive overwrites every borrowed key, so what
// the roll owns (EQ BG, TXT PLAY) and every locked row are put back after it.
inline void roll_eq(Roll& r, const Theme& base) {
    Theme& t = r.t;
    const bool eqOnBackground = r.rng.chance(0.5);
    const Argb eqGround = !r.is_free(&Theme::eqBg) ? base.eqBg
                          : eqOnBackground       ? t.background
                                                 : t.vizBackground;
    const Argb rolledPlayhead = t.textPlayhead;
    derive_borrowed_colors(t);
    t.eqBg         = eqGround;
    t.textPlayhead = rolledPlayhead;
    const auto& rows = theme_color_rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (r.locks.locked(static_cast<int>(i))) t.*(rows[i].field) = base.*(rows[i].field);
    }
    // EQ FILL is also the gridlines; EQ BORDER is drawn over it and the panel.
    if (r.is_free(&Theme::eqFill)) {
        t.eqFill = to_argb(OkLch{r.away(eqGround, FLOOR_DECOR, r.rng.range(0.0, 0.04)),
                                 r.chroma(0.0, 0.05), r.baseHue});
    }
    if (r.is_free(&Theme::eqBorder)) {
        const Argb   g    = r.harder(eqGround, t.eqFill);
        const double lift = r.rng.range(0.02, 0.10);
        const double hue  = scheme_hue(r.rng, r.scheme, r.baseHue);
        t.eqBorder = ink_at(r.away(g, FLOOR_SUPPORT, lift), hue, 0.10 * r.ctx.chromaScale, g, FLOOR_SUPPORT);
    }
    if (r.is_free(&Theme::eqTxt)) {
        const double lift = r.rng.range(0.0, 0.06);
        const double hue  = scheme_hue(r.rng, r.scheme, r.baseHue);
        t.eqTxt = ink_at(r.away(eqGround, FLOOR_SUPPORT, lift), hue, 0.04 * r.ctx.chromaScale, eqGround,
                         FLOOR_SUPPORT);
    }
}

}  // namespace random_detail

// Roll a palette, or nothing when no attempt satisfies the rules. `base` supplies the locked rows, the
// name and `visualizerType`.
inline std::optional<Theme> theme_roll(const Theme& base, const ThemeLocks& locks, ThemeScheme scheme,
                                       uint32_t seed) {
    using namespace random_detail;

    // Beyond the roll's reach: locked, or copied from locked rows (a free `eqBg` copies one of two grounds).
    const auto held = [&](Argb Theme::* f) {
        if (f == &Theme::eqBg && !field_locked(locks, f)) {
            return field_locked(locks, &Theme::background) && field_locked(locks, &Theme::vizBackground);
        }
        return field_locked(locks, f);
    };

    const PaletteContext ctx = theme_context(base, locks);

    // The attempts are the solver: each re-rolls the free roles only.
    constexpr int MAX_ATTEMPTS = 200;
    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        Roll r{Rng(seed + static_cast<uint32_t>(attempt) * 0x9E3779B9u), scheme, ctx, locks, base};

        // Drawn even when the context overrides it, so a seed gives the same ladder either way.
        const double rolledHue = r.rng.range(0.0, 360.0);
        r.baseHue = ctx.hasHue ? ctx.hue : rolledHue;

        roll_grounds(r);
        roll_accents(r);
        roll_text(r);
        roll_visualizer(r);
        roll_meter(r);
        roll_eq(r, base);
        r.t.visualizerType = base.visualizerType;
        r.t.name           = base.name;

        // A roll answers only for pairs it can move: two held colours that clash were held on purpose.
        size_t bad = 0;
        for (const ThemeViolation& v : theme_violations(r.t, /*generator=*/true)) {
            if (!held(v.rule->a) || !held(v.rule->b)) ++bad;
        }
        if (bad == 0) return r.t;
    }
    return std::nullopt;
}

}  // namespace pt::ui
