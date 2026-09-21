#pragma once

// ─── The theme generator ─────────────────────────────────────────────────────────────────────────
//
// A palette is built as a LADDER, not as eighteen independent colours: the grounds are steps of
// lightness away from `background`, and the inks are steps of loudness against the ground each one
// lands on. Generated independently they give the classic bad palette — a beat stripe darker than
// the ground it stripes, a placeholder louder than the value beside it.
//
// ⚠️⚠️ **EVERY ONE-SIDED BOUND IS SIGNED BY `theme_polarity`, NEVER BY "lighter".** A palette built
// on a light ground runs every step the other way, and an assumption baked in here is what would make
// the generator produce only dark themes while passing every test written on dark themes.
//
// ⚠️ **A LOCK IS AN INPUT, NOT AN EXCLUSION.** Locked roles are fixed and everything else is solved
// around them, so a lock set can be unsatisfiable — the caller is told, and the palette it already
// has is left alone.

#include <cmath>
#include <cstdint>
#include <vector>

#include "ui/color_space.h"
#include "ui/theme.h"
#include "ui/theme_rules.h"

namespace pt::ui {

/**
 * How the palette's hues relate to each other — the classic colour-wheel relationships, in the
 * order the RANDOMIZE cell walks them.
 *
 * ⭐ `ALL` is the one that constrains nothing, and it is first because it is the plain answer to
 * *"just give me a palette"*. The rest each pick their hues from a fixed set of angles off one base.
 */
enum class ThemeScheme { ALL, MONO, ANALOG, COMP, SPLIT, TRIAD, TETRAD };

/// ⚠️ The ring the cell steps through, and the bound `ptshot` searches by name. Beside the enum so
/// the two cannot drift; a scheme added above without touching this is one nothing can reach.
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
 * xorshift32 — a generator whose whole job is to be the same sequence everywhere.
 *
 * ⚠️ NOT `<random>`: its engines are portable but its DISTRIBUTIONS are not, so the same seed gives
 * different palettes on different standard libraries. A seed that does not reproduce a palette across
 * the Android, Linux and Windows builds is a seed that cannot be reported in a bug.
 */
struct Rng {
    uint32_t s;

    /**
     * ⚠️ **THE SEED IS MIXED BEFORE IT IS USED, AND IT HAS TO BE.** xorshift32 avalanches poorly on
     * its first outputs, so nearby seeds give nearly the same first few numbers — and the first
     * number this generator draws is the palette's HUE. Seeds 3 and 19 produced the same maroon.
     * One round of a splitmix-style finalizer makes the first draw depend on every bit of the seed.
     */
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
 * The scheme's hue wheel — the base plus the offsets that scheme allows.
 *
 * ⚠️ A SCHEME NAMES THE ANGLES, NOT A COUNT OF COLOURS. Every role that wants a hue calls this and
 * gets one of the scheme's angles, so a three-hue scheme spread over nineteen roles is nineteen
 * draws from a set of three — which is what makes the palette read as one family rather than as
 * three swatches.
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
 *
 * ⭐ **CHROMA IS WHAT IS GIVEN UP, NEVER LIGHTNESS OR HUE** — the same trade `to_argb`'s gamut map
 * makes, for the same reason: the ladder owns L and the scheme owns H, so an ink that quietly moved
 * either would break the thing that put it there. If no chroma at all clears the floor the caller
 * gets the best it could do and the validator reports the miss; silently walking L here is how a
 * generator ends up disagreeing with its own checker.
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
 * What the HELD rows say the palette already IS — its hue, and how much colour it carries.
 *
 * ⭐⭐ **A LOCK IS AN INPUT TO THE SOLVER, AND THAT MEANS ITS COLOUR, NOT ONLY ITS SLOT.** Holding a
 * blue row and rolling in MONOCHROME has to give shades of that blue, and re-rolling one row of a
 * grey palette has to give grey — in both cases the held rows are the only statement of what the
 * palette looks like, and a roll that ignores them is a slot machine with one cell taped over.
 *
 * ⚠️⚠️ **SEVEN ROWS DO NOT COUNT, AND EXCLUDING THEM IS WHAT MAKES THIS USABLE.** Two groups, two
 * different reasons, and both were found by rolling real palettes:
 *
 *   * **the four grounds** are near-grey by construction — MONO's and BLUE's backgrounds are the same
 *     `0x0A0A0A` — so a held BACKGROUND would report "this palette has no colour in it" and turn the
 *     next roll monochrome. Holding a background says nothing about hue;
 *   * **the three meter bars** are a SIGNAL ramp, not palette colour. AMBER's MTR HIGH is `0xCC0000`
 *     at chroma 0.218 — the most chromatic row in that palette, and a red one, where every other
 *     colour in it sits around amber. It won the hue every time and made a re-roll of any row come
 *     out red or pink. ⭐ Nothing on screen tries to match a meter bar, which is the whole point of
 *     one: loud is *meant* to leave the palette.
 *
 * ⚠️ The polarity is NOT read here — it comes from `background` as it always has, so a held light
 * ground still builds a light palette whether or not it says anything about hue.
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
            continue;   // a ground, or a meter bar — see the header: neither names the palette
        }
        any = true;
        // ⭐ THE MOST COLOURFUL HELD ROW NAMES THE HUE, not an average of them: averaging hues around
        // a wheel is meaningless, and the loudest colour is the one anyone would say the palette "is".
        const OkLch c = to_oklch(base.*f);
        if (c.C > best) { best = c.C; ctx.hue = c.H; }
    }
    if (!any) return ctx;   // nothing held: the roll is free, as it has always been

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

/**
 * Roll a palette.
 *
 * `base` supplies the locked rows and `visualizerType`, which is NOT part of a theme's identity and
 * must survive any palette swap. The name is the caller's to set.
 */
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
     *
     * ⚠️ **A FIELD WITH NO EDITOR ROW HAS NO LOCK, AND THAT IS NOT THE SAME AS BEING FREE.** `eqBg`
     * is a byte copy of `background` or of `vizBackground`, so once both of those are held the EQ
     * panel's ground is held with them — and reading it as free is what made every single-row
     * re-roll impossible, since the EQ inks are then measured against a ground nothing can move.
     * `meterBorder`, the other rowless field with a rule, IS rolled fresh every attempt.
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

    // ⚠️ THE ATTEMPTS ARE THE SOLVER. Each one re-rolls only the free roles; a locked one is copied
    // from `base` every time, so a lock the rules cannot accommodate makes every attempt fail rather
    // than drifting the locked value.
    constexpr int MAX_ATTEMPTS = 200;
    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        Rng rng(seed + static_cast<uint32_t>(attempt) * 0x9E3779B9u);
        Theme t = base;

        // ⚠️ THE HUE IS DRAWN EVEN WHEN THE CONTEXT OVERRIDES IT, so that a seed produces the same
        // ladder whether or not rows are held — otherwise every lock shifts the whole stream and two
        // rolls of "the same palette with one more row held" have nothing in common.
        const double rolledHue = rng.range(0.0, 360.0);
        const double baseHue   = ctx.hasHue ? ctx.hue : rolledHue;

        // ⭐ Every chroma the ladder asks for is scaled by what the held rows carry, so re-rolling one
        // row of a grey palette gives grey rather than the pink the free roll would have picked.
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

        // ⭐ The sharing group: VIZ BG is either the screen's ground or a lifted panel, and EQ BG is
        // a COPY of one of the two. Three shapes, and the shape is part of the roll.
        // ⚠️ THE LIFT STOPS SHORT OF THE BEAT STRIPE'S BAND ABOVE. If the panel can land on the
        // stripe's own value, the EQ grid lines drawn in that stripe disappear into the panel.
        if (!locked_field(&Theme::vizBackground)) {
            t.vizBackground = rng.chance(0.4)
                                  ? to_argb(OkLch{step(0.015, 0.033), chroma(0.0, 0.03), baseHue})
                                  : t.background;
        }

        // ⭐⭐ ONE HELPER OWNS EVERY INK'S LIGHTNESS, AND IT ONLY EVER MOVES AWAY FROM THE GROUND.
        // The ladder's steps used to be "solve the floor, then nudge toward the ground to make this
        // role quieter" — which destroys the floor that was just solved for, and is why a role can
        // come out failing a rule the generator believed it had satisfied. A quieter role is one with
        // a LOWER FLOOR, never one moved back toward the ground.
        const auto away = [&](Argb ground, double floorRatio, double extra) {
            return lightness_for(ground, floorRatio, polarity, ok_lightness(ground)) +
                   polarity * extra;
        };
        /** The ground an ink is hardest to read on: furthest along the direction ink travels. */
        const auto harder = [&](Argb p, Argb q) {
            return (ok_lightness(p) * polarity > ok_lightness(q) * polarity) ? p : q;
        };

        // ── The two accents, which are grounds whose INK is read off the palette ─────────────────
        // ⚠️ Each is BOTH a ground and an ink. `background` reads inside the cursor bar, `rowEvery4th`
        // inside the selection, and both accents are also drawn as marks on `meterBackground` — so
        // each clears FLOOR_TEXT against the harder of its two, never against the other accent.
        const double cursorExtra = rng.range(0.06, 0.18);
        if (!locked_field(&Theme::rowCursor)) {
            const Argb g = harder(t.background, t.meterBackground);
            t.rowCursor  = ink_at(away(g, FLOOR_TEXT, cursorExtra), scheme_hue(rng, scheme, baseHue),
                                  chroma(0.08, 0.20), g, FLOOR_TEXT);
        }
        if (!locked_field(&Theme::rowSelection)) {
            const Argb g = harder(t.rowEvery4th, t.meterBackground);
            // A selection and a cursor are two shades of "the thing you are working on": told apart,
            // never far apart. The gap is a step of the ladder, not a second free colour.
            t.rowSelection = ink_at(away(g, FLOOR_TEXT, cursorExtra + rng.range(0.04, 0.14)),
                                    scheme_hue(rng, scheme, baseHue), chroma(0.08, 0.20),
                                    g, FLOOR_TEXT);
        }

        // ── The ink ladder, loudest first ────────────────────────────────────────────────────────
        // ⚠️ EVERY TEXT ROLE LANDS ON FOUR GROUNDS, so it is solved against the HARDEST of them and
        // the other three come free. The ladder pushes ink away from `background`, so the hardest
        // ground is the one furthest along that same direction — the closest one to the ink, not the
        // closest one to `background`.
        Argb hardest = t.background;
        for (Argb g : {t.rowEvery4th, t.vizBackground, t.meterBackground}) {
            if (ok_lightness(g) * polarity > ok_lightness(hardest) * polarity) hardest = g;
        }

        // ⚠️ The ORDER of the ladder is the order of the floors, and the extra step on top only ever
        // widens it: TXT EMPTY sits at its floor, TXT PARAM a step past it, TXT VALUE and TXT TITLE
        // past the higher floor again. That is what stops a placeholder coming out louder than the
        // value beside it.
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
            // The one loud role that is allowed real colour — it is how a palette says its hue.
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
            // ⚠️ OCTA paints the strip in `background` and every other mode in `vizBackground`, so
            // the wave has TWO grounds at once and must clear the harder of them.
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

        // ⚠️ AN ORDERED TRIPLE, NOT THREE COLOURS — three unrelated hues on one meter reads as a
        // fault. The ramp may run either way in lightness (MONO's shipped ramp DARKENS as it gets
        // loud), so only the ordering is fixed, and the direction is part of the roll.
        {
            // ⚠️⚠️ **THE RAMP'S TWO ENDS ARE BOTH SCHEME HUES, AND THE MIDDLE IS BETWEEN THEM.** A
            // free ±110° spread off one hue was the one place in the generator that ignored the
            // scheme: MONOCHROME came out with a meter ramping through a hundred degrees, which is
            // the one thing MONOCHROME means it will not do. Drawing both ends through `scheme_hue`
            // keeps the single statement of what a scheme allows — MONO gives one hue twice, TRIADIC
            // gives a real ramp — and the interpolated middle is what stops three loose hues.
            const double hLow  = scheme_hue(rng, scheme, baseHue);
            const double hHigh = scheme_hue(rng, scheme, baseHue);
            // ⚠️ The SHORT way round: 350° to 10° passes through 0, not through 180.
            const double arc   = std::fmod(hHigh - hLow + 540.0, 360.0) - 180.0;
            const double hMid  = hLow + 0.5 * arc;
            const double base   = rng.range(0.02, 0.08);
            const double rise   = rng.range(0.03, 0.09);
            // ⚠️ The ramp may run either way — MONO's shipped meters get DARKER as they get loud — so
            // only the ORDER is fixed and the direction is part of the roll. Every rung is still
            // measured away from the trough, so neither end can fall through its floor.
            const bool   up     = rng.chance(0.5);
            const double eL = base + (up ? 0.0 : 2.0 * rise);
            const double eM = base + rise;
            const double eH = base + (up ? 2.0 * rise : 0.0);
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
        // ⚠️⚠️ THE DERIVE RUNS LAST AND OVERWRITES EVERY BORROWED KEY, so anything the generator owns
        // that the derive also computes must be put back after it — the same order BLUE and MONO dial
        // their own EQ colours in. ⚠️ TXT PLAY is the one that is easy to miss: its seed `rowPlayback`
        // has no editor row and is therefore never rolled, so the derive quietly restored CLASSIC's
        // green marker into every generated palette until this line existed.
        const Argb eqGround = rng.chance(0.5) ? t.background : t.vizBackground;
        const Argb rolledPlayhead = t.textPlayhead;
        derive_borrowed_colors(t);
        t.eqBg         = eqGround;
        t.textPlayhead = rolledPlayhead;
        // ⚠️ EQ FILL is the wash under the curve AND the panel's gridlines, so it is the one decor
        // role that must actually be seen. EQ BORDER is drawn over it as well as over the panel, so
        // it clears its floor against the harder of the two.
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

        // ⚠️⚠️ **A ROLL IS ONLY ANSWERABLE FOR PAIRS IT CAN STILL MOVE.** Judging the whole palette
        // means a pair of LOCKED colours — two values the user is holding on purpose — can condemn
        // every attempt, and the single-row re-roll is that case with eighteen of the nineteen rows
        // held: measured over the four built-ins it succeeded 0 times out of 19, because each of
        // them already misses a floor or two somewhere the roll was never going to reach.
        //
        // ⭐ A pair with one free end is still the solver's to satisfy, and still fails honestly when
        // the locked half makes it impossible — which is the message doing its job rather than
        // firing by construction.
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
