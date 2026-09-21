#pragma once

// ─── What makes a palette readable ───────────────────────────────────────────────────────────────
//
// One table of rules over `Theme`, and one function that measures a palette against it. Three
// consumers read this and none may restate it: the randomizer solves against it, the theme editor's
// clash marker lights up from it, and the tests measure with it. A second list of "colours that
// clash" would drift from this one the first time a bound moved.
//
// ⚠️ **THE CONTRAST EDGES ARE A HYPOTHESIS UNTIL THE PIXEL SWEEP RUNS.** They are read out of the
// drawing code, and a ground can be conditional in ways a code read misses — `vizWave` sits on
// `background` in OCTA mode and on `vizBackground` in every other. The sweep that settles it renders
// a probe palette and looks for sentinels sharing a boundary; until then a missing edge is the
// likely fault here, not a wrong bound.
//
// ⚠️ **A RULE IS A WARNING, NEVER A CLAMP.** Nothing here rejects a value the user typed. The
// randomizer is the only caller that may refuse an outcome, and then it refuses its own.

#include <cmath>
#include <vector>

#include "ui/color_space.h"
#include "ui/theme.h"

namespace pt::ui {

enum class RuleKind {
    Contrast,     ///< an ink on a ground it is actually drawn on: a WCAG ratio floor
    Separation,   ///< two grounds that must be told apart: a band of perceptual lightness
    Distinct,     ///< two roles read side by side: they must not be the same colour
};

struct ThemeRule {
    RuleKind kind;
    Argb Theme::* a;
    Argb Theme::* b;
    double  floorValue;   ///< Contrast: min ratio. Separation: min |ΔL|. Distinct: min ΔE.
    double  ceilValue;    ///< Separation only: max |ΔL|. Ignored otherwise.
    bool    directional;  ///< Separation: `b` must sit AWAY from `a` in the palette's polarity.
    bool    shareable;    ///< Separation: equal values are legal and the rule is then skipped.
    /// Contrast only: the ΔE under which the pair is genuinely indistinguishable. The other two
    /// kinds already measure a perceptual quantity, so they leave it at zero and never read it.
    double  blendValue = 0.0;
};

/**
 * ⭐ Which way "away from the ground" points, derived ONCE from `background` and nothing else.
 *
 * +1 on a dark palette (away means lighter), −1 on a light one. Every one-sided bound in the
 * generator is a signed step against this, never a hardcoded "lighter" — that assumption is what
 * would reject BLUE, whose cursor cell is deliberately dark ink on a bright bar.
 */
inline int theme_polarity(const Theme& t) { return ok_lightness(t.background) < 0.5 ? +1 : -1; }

// ─── Four floors, chosen by what the ink IS ──────────────────────────────────────────────────────
//
// ⚠️⚠️ **ONE FLOOR FOR EVERYTHING IS THE MISTAKE THAT MAKES THIS USELESS.** A value you read, a
// placeholder that is meant to recede, a meter bar you glance at and a gridline that must stay out of
// the way are four different jobs, and holding all four to WCAG's text minimum fails every palette
// this app has ever shipped — including the four the user dialled by eye.
//
// ⚠️ A gridline or a shaded wash is DECORATION. WCAG exempts it on purpose; the only real
// requirement is that it not vanish entirely.
// ⚠️ **THE FLOORS ARE NOT CALIBRATED AGAINST THE BUILT-IN PALETTES.** Those four were dialled by eye
// and are a reference, not a specification — fitting the bounds to them would certify whatever they
// happen to do, including their own misses. Each floor below is a published threshold instead, and
// the built-ins are then measured AGAINST it like any other palette.
inline constexpr double FLOOR_TEXT = 4.5;
///< WCAG AA for text. A value, a name, a heading — read, not glanced at.

inline constexpr double FLOOR_SUPPORT = 3.0;
///< WCAG AA for large text, and WCAG 1.4.11 for a graphic you must perceive. Labels, placeholders,
///< the playback marker, meter bars, the EQ outline. ⚠️ They are MEANT to recede — holding them to
///< 4.5 deletes the difference between a value and a placeholder, which is those roles' whole job.

inline constexpr double FLOOR_DECOR = 1.3;
///< Purely decorative and WCAG-exempt: gridlines, the shaded wash under the EQ curve, a frame, the
///< scope's centre line. The only requirement is that the edge still be visible at all.

// ─── And the three BLEND thresholds, which answer a different question ───────────────────────────
//
// ⚠️⚠️ **"COMFORTABLE TO READ" AND "TELLABLE APART" ARE TWO QUESTIONS WITH DIFFERENT ANSWERS**, and
// the floors above only answer the first. A WCAG ratio is built from luminance alone, so it is blind
// to hue and chroma: navy ink on a black ground measures 1.26 and is perfectly visible, because what
// separates it is colour and not brightness. Every generated palette clears the floors, so the
// generator may aim at them — but a palette dialled by hand and warned about a pair anyone can see
// the difference in is a warning that gets ignored on the day it is right.
//
// ⭐ So the live warning measures the perceptual distance instead, and fires only when the pair is
// close enough that the difference could be a rendering artefact rather than a choice. In OKLab one
// unit of ΔE is the whole black-to-white span and ~0.02 is about a just-noticeable step between two
// large patches; text is small and antialiased, so it needs several of them.
inline constexpr double BLEND_TEXT    = 0.06;   ///< a role that is READ: roughly three steps
inline constexpr double BLEND_SUPPORT = 0.05;   ///< a role meant to recede, but still found
inline constexpr double BLEND_DECOR   = 0.03;   ///< a line or a wash: the edge must merely exist

/** The rules, in the order a reader scans them. */
inline const std::vector<ThemeRule>& theme_rules() {
    static const std::vector<ThemeRule> rules = [] {
        std::vector<ThemeRule> r;

        const auto edge = [&](double floorValue, double blendValue,
                              Argb Theme::* ink, Argb Theme::* ground) {
            r.push_back({RuleKind::Contrast, ink, ground, floorValue, 0.0, false, false, blendValue});
        };
        const auto loud  = [&](Argb Theme::* i, Argb Theme::* g) { edge(FLOOR_TEXT, BLEND_TEXT, i, g); };
        const auto quiet = [&](Argb Theme::* i, Argb Theme::* g) { edge(FLOOR_SUPPORT, BLEND_SUPPORT, i, g); };
        const auto mark  = [&](Argb Theme::* i, Argb Theme::* g) { edge(FLOOR_SUPPORT, BLEND_SUPPORT, i, g); };
        const auto decor = [&](Argb Theme::* i, Argb Theme::* g) { edge(FLOOR_DECOR, BLEND_DECOR, i, g); };

        // ── Text on the grounds it lands on ──────────────────────────────────────────────────────
        for (Argb Theme::* g : {&Theme::background, &Theme::rowEvery4th,
                                &Theme::vizBackground, &Theme::meterBackground}) {
            loud(&Theme::textTitle, g);
        }
        for (Argb Theme::* g : {&Theme::background, &Theme::rowEvery4th,
                                &Theme::vizBackground, &Theme::meterBackground}) {
            loud(&Theme::textValue, g);
            quiet(&Theme::textEmpty, g);
        }
        // ⚠️ NO ORDINARY INK MEETS `rowSelection`. The file browser is the one screen that selects
        // whole ROWS, and it inverts like every grid does — the ink there is `rowEvery4th`, below.
        quiet(&Theme::textParam, &Theme::background);
        quiet(&Theme::textParam, &Theme::meterBackground);
        quiet(&Theme::textPlayhead, &Theme::background);
        quiet(&Theme::textPlayhead, &Theme::rowEvery4th);

        // ── The cursor and the selection, which invert ───────────────────────────────────────────
        // ⚠️ The cell's ink is the GROUND read back off the palette (`background` inside the cursor
        // bar, `rowEvery4th` inside a selected cell) and the "you are here" mark is `rowCursor` on
        // the plain ground. Three pairs, and all three are the app's loudest.
        loud(&Theme::background, &Theme::rowCursor);
        loud(&Theme::rowEvery4th, &Theme::rowSelection);
        loud(&Theme::rowCursor, &Theme::background);
        loud(&Theme::rowCursor, &Theme::meterBackground);

        // ── A background role used as ink ────────────────────────────────────────────────────────
        // ⚠️⚠️ NEITHER OF `rowEvery4th`'s TWO INK JOBS IS A CONTRAST EDGE, and reading them as one is
        // what makes a generator impossible to satisfy:
        //   * the EQ GRID LINES are a guide, wanted a hair off the panel exactly as the beat stripe
        //     is a hair off `background`. What they read against is the spectrum crossing them. They
        //     get a SEPARATION rule below, not a floor.
        //   * the LOADING BAR'S TROUGH gets no rule at all — the bar is defined by its `textParam`
        //     outline and its `textValue` fill, and both of those have rules of their own.
        quiet(&Theme::rowSelection, &Theme::meterBackground);// the browser's selection hint

        // ── The visualizer, whose ground depends on a SETTING ────────────────────────────────────
        loud(&Theme::vizWave, &Theme::vizBackground);
        loud(&Theme::vizWave, &Theme::background);   // OCTA paints the strip in `background`
        decor(&Theme::vizCenterLine, &Theme::vizBackground);

        // ── The EQ panel and the meters, neither of which leaves its own box ─────────────────────
        decor(&Theme::eqFill, &Theme::eqBg);     // the wash under the curve, deliberately subtle
        mark(&Theme::eqBorder, &Theme::eqBg);    // the spectrum outline AND the 0 dB line
        mark(&Theme::eqBorder, &Theme::eqFill);
        mark(&Theme::eqTxt, &Theme::eqBg);       // frequency labels, drawn small
        mark(&Theme::meterLow, &Theme::meterBackground);
        mark(&Theme::meterMid, &Theme::meterBackground);
        mark(&Theme::meterHigh, &Theme::meterBackground);
        decor(&Theme::meterBorder, &Theme::background);

        // ── Grounds that must be told apart without shouting ─────────────────────────────────────
        // ⚠️ The ceiling is as load-bearing as the floor: a beat stripe far from its ground reads as
        // a ladder rather than as a beat.
        r.push_back({RuleKind::Separation, &Theme::background, &Theme::rowEvery4th,
                     0.03, 0.09, true, false});
        r.push_back({RuleKind::Separation, &Theme::background, &Theme::vizBackground,
                     0.02, 0.06, true, true});
        r.push_back({RuleKind::Separation, &Theme::background, &Theme::meterBackground,
                     0.04, 0.12, true, false});
        // The two accents are both grounds and both carry a cursor; a big gap between them reads as
        // two unrelated states rather than as two shades of "the thing you are working on".
        r.push_back({RuleKind::Separation, &Theme::rowCursor, &Theme::rowSelection,
                     0.04, 0.30, false, false});
        // ⚠️ The EQ grid lines, on the same terms as the beat stripe: present, never ruled. The pair
        // matters only when EQ BG took the VISUALIZER's ground — when it took the screen's, this is
        // the `background`/`rowEvery4th` rule above, said twice.
        r.push_back({RuleKind::Separation, &Theme::eqBg, &Theme::rowEvery4th,
                     0.015, 0.12, false, false});

        // ── Roles read side by side ──────────────────────────────────────────────────────────────
        // ⚠️ NOT every pair in the palette. A value, the label beside it and a placeholder are three
        // statements about one cell; if two of them are one colour the screen stops saying which is
        // which. Everything else may collide on purpose — AMBER's accent IS its header colour.
        r.push_back({RuleKind::Distinct, &Theme::textValue, &Theme::textParam, 0.02, 0.0, false, false});
        r.push_back({RuleKind::Distinct, &Theme::textValue, &Theme::textEmpty, 0.02, 0.0, false, false});
        r.push_back({RuleKind::Distinct, &Theme::textParam, &Theme::textEmpty, 0.02, 0.0, false, false});

        return r;
    }();
    return rules;
}

struct ThemeViolation {
    const ThemeRule* rule = nullptr;
    double measured = 0.0;   ///< the ratio, |ΔL| or ΔE actually found
    double wanted   = 0.0;   ///< the bound it missed
};

/**
 * What a palette breaks.
 *
 * `generator` runs every rule and measures a contrast edge as a WCAG ratio — the target a rolled
 * palette is held to. False asks the narrower question the warning is allowed to ask, *"can these
 * two be told apart at all"*, and that changes the MEASURE as well as the rule list: a contrast edge
 * is read as a perceptual distance against `blendValue`, because a ratio is blind to hue and would
 * light up pairs anyone can see the difference in. A separation ceiling and a direction are dropped
 * with it — both are statements about how a palette is GENERATED, not about whether it can be read.
 */
inline std::vector<ThemeViolation> theme_violations(const Theme& t, bool generator) {
    std::vector<ThemeViolation> out;
    const int polarity = theme_polarity(t);

    for (const ThemeRule& rule : theme_rules()) {
        const Argb a = t.*(rule.a);
        const Argb b = t.*(rule.b);

        switch (rule.kind) {
            case RuleKind::Contrast: {
                const double got  = generator ? wcag_contrast(a, b) : ok_delta_e(a, b);
                const double want = generator ? rule.floorValue     : rule.blendValue;
                if (got < want) out.push_back({&rule, got, want});
                break;
            }
            case RuleKind::Distinct: {
                const double got = ok_delta_e(a, b);
                if (got < rule.floorValue) out.push_back({&rule, got, rule.floorValue});
                break;
            }
            case RuleKind::Separation: {
                if (rule.shareable && a == b) break;   // equality is a choice here, not a fault
                const double delta = ok_lightness(b) - ok_lightness(a);
                const double mag   = std::fabs(delta);
                if (mag < rule.floorValue) { out.push_back({&rule, mag, rule.floorValue}); break; }
                if (!generator) break;
                if (mag > rule.ceilValue) { out.push_back({&rule, mag, rule.ceilValue}); break; }
                if (rule.directional && delta * polarity < 0.0) out.push_back({&rule, delta, 0.0});
                break;
            }
        }
    }
    return out;
}

/** The label a message names a colour by. Falls back to the field's editor row where it has one. */
inline const char* theme_field_label(Argb Theme::* field) {
    for (const ThemeColorRow& row : theme_color_rows()) {
        if (row.field == field) return row.label;
    }
    if (field == &Theme::meterBorder) return "MTR BORDER";
    if (field == &Theme::rowPlayback) return "ROW PLAY";
    if (field == &Theme::eqBg)        return "EQ BG";
    if (field == &Theme::textCursor)  return "TXT CURSOR";
    if (field == &Theme::textSelection) return "TXT SELECT";
    return "?";
}

/**
 * What this editor row's colour clashes with, or `nullptr` when it is fine — the message beside the
 * title. ⭐ The mark on the row can only say THAT something is wrong; naming the other colour is what
 * makes it fixable, and the cursor already says which row is meant.
 */
inline const char* theme_row_clash_partner(const Theme& t, int colorRowIndex) {
    const auto& rows = theme_color_rows();
    if (colorRowIndex < 0 || colorRowIndex >= static_cast<int>(rows.size())) return nullptr;
    Argb Theme::* const field = rows[static_cast<size_t>(colorRowIndex)].field;

    // The WORST miss, not the first: a row in two clashes should name the one costing it most, and
    // "how far under its own floor" is the only comparison that means the same thing across kinds.
    const char* worst = nullptr;
    double deficit = 0.0;
    for (const ThemeViolation& v : theme_violations(t, /*generator=*/false)) {
        // ⚠️ The same asymmetry the row marks use: a ground is not the culprit of a contrast miss, so
        // standing on one does not give that row a message of its own.
        const bool mine = (v.rule->a == field) ||
                          (v.rule->b == field && v.rule->kind != RuleKind::Contrast);
        if (!mine) continue;
        const double d = (v.wanted <= 0.0) ? 0.0 : (v.wanted - v.measured) / v.wanted;
        if (worst == nullptr || d > deficit) {
            deficit = d;
            worst = theme_field_label(v.rule->a == field ? v.rule->b : v.rule->a);
        }
    }
    return worst;
}

}  // namespace pt::ui
