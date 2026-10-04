#pragma once

// ─── What makes a palette readable ───────────────────────────────────────────────────────────────
//
// One table of rules over `Theme` and one function that measures a palette against it — read by the
// randomizer (solves against it), the theme editor's clash marker and the tests. None may restate it.
// ⚠️ The contrast edges are read out of the drawing code, and a ground can be conditional (`vizWave` is
// on `background` in OCTA, `vizBackground` otherwise); a missing edge is the likely fault here.
// ⚠️ A RULE IS A WARNING, never a clamp: nothing rejects a value the user typed. Only the randomizer
// refuses — its own outcomes.

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
    /// Contrast only: the ΔE under which the pair is genuinely indistinguishable (the other kinds
    /// already measure perceptually and leave it 0).
    double  blendValue = 0.0;
};

/**
 * ⭐ Which way "away from the ground" points, derived ONCE from `background`: +1 on a dark palette
 * (lighter), −1 on a light one. Never a hardcoded "lighter" — BLUE's cursor cell is deliberately dark
 * ink on a bright bar.
 */
inline int theme_polarity(const Theme& t) { return ok_lightness(t.background) < 0.5 ? +1 : -1; }

// ─── Floors, chosen by what the ink IS ───────────────────────────────────────────────────────────
//
// ⚠️⚠️ One floor for everything fails every palette the app ships: a value you read, a placeholder meant to
// recede, a meter bar you glance at and a gridline that must stay out of the way are different jobs.
// Gridlines and washes are DECORATION (WCAG-exempt): they need only not vanish.
// ⚠️ The floors are published thresholds, NOT fitted to the built-in palettes (dialled by eye — fitting
// would certify their misses); the built-ins are measured against them like any other.
inline constexpr double FLOOR_TEXT = 4.5;
///< WCAG AA for text. A value, a name, a heading — read, not glanced at.

inline constexpr double FLOOR_SUPPORT = 3.0;
///< WCAG AA for large text, and WCAG 1.4.11 for a graphic you must perceive: labels, placeholders, the
///< playback marker, meter bars, the EQ outline. ⚠️ Meant to recede — at 4.5 a placeholder could not
///< differ from a value.

inline constexpr double FLOOR_DECOR = 1.3;
///< Decorative, WCAG-exempt: gridlines, the EQ wash, frames, the scope's centre line. The edge must
///< merely be visible.

// ─── And the BLEND thresholds, a different question ──────────────────────────────────────────────
//
// ⚠️⚠️ "Comfortable to read" and "tellable apart" differ. A WCAG ratio is luminance only and blind to hue
// (navy on black measures 1.26 and is plainly visible). The generator aims at the floors; the LIVE
// warning instead measures perceptual distance (OKLab ΔE) and fires only when a difference could be a
// rendering artefact rather than a choice. ~0.02 ΔE is one just-noticeable step between large patches;
// small antialiased text needs several.
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
        // ⚠️ No ordinary ink meets `rowSelection`: the file browser's selected rows invert like every grid
        // (ink `rowEvery4th`, below).
        quiet(&Theme::textParam, &Theme::background);
        quiet(&Theme::textParam, &Theme::meterBackground);
        quiet(&Theme::textPlayhead, &Theme::background);
        quiet(&Theme::textPlayhead, &Theme::rowEvery4th);

        // ── The cursor and the selection, which invert ───────────────────────────────────────────
        // The cell's ink is the GROUND read back (`background` in the cursor bar, `rowEvery4th` in a
        // selected cell), and the "you are here" mark is `rowCursor` on the plain ground. The loudest pairs.
        loud(&Theme::background, &Theme::rowCursor);
        loud(&Theme::rowEvery4th, &Theme::rowSelection);
        loud(&Theme::rowCursor, &Theme::background);
        loud(&Theme::rowCursor, &Theme::meterBackground);

        // ── A background role used as ink ────────────────────────────────────────────────────────
        // ⚠️⚠️ Neither of `rowEvery4th`'s ink jobs is a contrast edge: the EQ GRID LINES are a guide (a
        // SEPARATION rule below), and the LOADING BAR's trough needs none (outline and fill have rules).
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
        // ⚠️ The ceiling matters as much as the floor: a stripe far from its ground reads as a ladder, not
        // a beat.
        r.push_back({RuleKind::Separation, &Theme::background, &Theme::rowEvery4th,
                     0.03, 0.09, true, false});
        r.push_back({RuleKind::Separation, &Theme::background, &Theme::vizBackground,
                     0.02, 0.06, true, true});
        r.push_back({RuleKind::Separation, &Theme::background, &Theme::meterBackground,
                     0.04, 0.12, true, false});
        // The two accents both carry a cursor; a big gap reads as two unrelated states.
        r.push_back({RuleKind::Separation, &Theme::rowCursor, &Theme::rowSelection,
                     0.04, 0.30, false, false});
        // ⚠️ The EQ grid lines, like the beat stripe: present, never ruled. Matters only when EQ BG took the
        // visualizer's ground (otherwise it is the rule above).
        r.push_back({RuleKind::Separation, &Theme::eqBg, &Theme::rowEvery4th,
                     0.015, 0.12, false, false});

        // ── Roles read side by side ──────────────────────────────────────────────────────────────
        // ⚠️ NOT every pair: value, label and placeholder describe one cell, so two of them alike stops the
        // screen saying which is which. Others may collide on purpose (AMBER's accent IS its header).
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
 * What a palette breaks. `generator` runs every rule with contrast as a WCAG ratio — the target a rolled
 * palette meets. False asks only "can these be told apart at all": contrast becomes a perceptual
 * distance against `blendValue`, and separation ceilings and directions (statements about generating,
 * not reading) are dropped.
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

/** What this editor row's colour clashes with, or `nullptr` — the message beside the title. Naming the
 *  other colour is what makes it fixable. */
inline const char* theme_row_clash_partner(const Theme& t, int colorRowIndex) {
    const auto& rows = theme_color_rows();
    if (colorRowIndex < 0 || colorRowIndex >= static_cast<int>(rows.size())) return nullptr;
    Argb Theme::* const field = rows[static_cast<size_t>(colorRowIndex)].field;

    // The WORST miss, by how far under its own floor — the one comparison meaningful across kinds.
    const char* worst = nullptr;
    double deficit = 0.0;
    for (const ThemeViolation& v : theme_violations(t, /*generator=*/false)) {
    // ⚠️ A ground is not the culprit of a contrast miss, so standing on one gives that row no message.
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
