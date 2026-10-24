#pragma once

// What makes a palette readable: one table of rules and the function that measures a palette against
// it, for the randomizer, the theme editor's clash marker and the tests.
// ⚠️ The contrast edges mirror the drawing code; a missing edge is the likely fault here.
// A rule is a warning, never a clamp: only the randomizer refuses its own outcomes.

#include <cmath>
#include <vector>

#include "ui/color_space.h"
#include "ui/theme.h"

namespace pt::ui {

enum class RuleKind {
    Contrast,     // an ink on a ground it is drawn on: a WCAG ratio floor
    Separation,   // two grounds told apart: a band of perceptual lightness
    Distinct,     // two roles read side by side: not the same colour
};

struct ThemeRule {
    RuleKind kind;
    Argb Theme::* a;
    Argb Theme::* b;
    double  floorValue;   // Contrast: min ratio. Separation: min |ΔL|. Distinct: min ΔE.
    double  ceilValue;    // Separation: max |ΔL|
    bool    directional;  // Separation: `b` sits away from `a` in the palette's polarity
    bool    shareable;    // Separation: equal values are legal, and skip the rule
    double  blendValue = 0.0;   // Contrast: the ΔE under which the pair is indistinguishable
};

// Which way "away from the ground" points: +1 on a dark palette, −1 on a light one.
inline int theme_polarity(const Theme& t) { return ok_lightness(t.background) < 0.5 ? +1 : -1; }

// ─── Contrast floors (WCAG ratios), by what the ink is for — published thresholds, not fitted ────
inline constexpr double FLOOR_TEXT    = 4.5;   // read: values, names, headings
inline constexpr double FLOOR_SUPPORT = 3.0;   // meant to recede but be found: labels, marks, meters
inline constexpr double FLOOR_DECOR   = 1.3;   // gridlines, washes, frames: merely visible

// ─── Blend thresholds (OKLab ΔE): can the two be told apart at all? ──────────────────────────────
// The generator aims at the floors; the editor's live warning asks this instead — a WCAG ratio is
// blind to hue. ~0.02 is one just-noticeable step.
inline constexpr double BLEND_TEXT    = 0.06;
inline constexpr double BLEND_SUPPORT = 0.05;
inline constexpr double BLEND_DECOR   = 0.03;

// The rules, in the order a reader scans them.
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
        quiet(&Theme::textParam, &Theme::background);
        quiet(&Theme::textParam, &Theme::meterBackground);
        quiet(&Theme::textPlayhead, &Theme::background);
        quiet(&Theme::textPlayhead, &Theme::rowEvery4th);

        // ── The cursor and the selection invert: the ground becomes the ink ──────────────────────
        loud(&Theme::background, &Theme::rowCursor);
        loud(&Theme::rowEvery4th, &Theme::rowSelection);
        loud(&Theme::rowCursor, &Theme::background);
        loud(&Theme::rowCursor, &Theme::meterBackground);

        quiet(&Theme::rowSelection, &Theme::meterBackground);   // the browser's selection hint

        // ── The visualizer, whose ground depends on the mode ─────────────────────────────────────
        loud(&Theme::vizWave, &Theme::vizBackground);
        loud(&Theme::vizWave, &Theme::background);   // OCTA paints the strip in `background`
        decor(&Theme::vizCenterLine, &Theme::vizBackground);

        // ── The EQ panel and the meters ──────────────────────────────────────────────────────────
        decor(&Theme::eqFill, &Theme::eqBg);
        mark(&Theme::eqBorder, &Theme::eqBg);    // the outline and the 0 dB line
        mark(&Theme::eqBorder, &Theme::eqFill);
        mark(&Theme::eqTxt, &Theme::eqBg);       // frequency labels, drawn small
        mark(&Theme::meterLow, &Theme::meterBackground);
        mark(&Theme::meterMid, &Theme::meterBackground);
        mark(&Theme::meterHigh, &Theme::meterBackground);
        decor(&Theme::meterBorder, &Theme::background);

        // ── Grounds told apart without shouting: a stripe far from its ground reads as a ladder ──
        r.push_back({RuleKind::Separation, &Theme::background, &Theme::rowEvery4th,
                     0.03, 0.09, true, false});
        r.push_back({RuleKind::Separation, &Theme::background, &Theme::vizBackground,
                     0.02, 0.06, true, true});
        r.push_back({RuleKind::Separation, &Theme::background, &Theme::meterBackground,
                     0.04, 0.12, true, false});
        // The two accents: near, or they read as unrelated states.
        r.push_back({RuleKind::Separation, &Theme::rowCursor, &Theme::rowSelection,
                     0.04, 0.30, false, false});
        // The EQ grid lines, drawn in `rowEvery4th`.
        r.push_back({RuleKind::Separation, &Theme::eqBg, &Theme::rowEvery4th,
                     0.015, 0.12, false, false});

        // ── Value, label and placeholder describe one cell, so must differ (others may match) ────
        r.push_back({RuleKind::Distinct, &Theme::textValue, &Theme::textParam, 0.02, 0.0, false, false});
        r.push_back({RuleKind::Distinct, &Theme::textValue, &Theme::textEmpty, 0.02, 0.0, false, false});
        r.push_back({RuleKind::Distinct, &Theme::textParam, &Theme::textEmpty, 0.02, 0.0, false, false});

        return r;
    }();
    return rules;
}

struct ThemeViolation {
    const ThemeRule* rule = nullptr;
    double measured = 0.0;   // the ratio, |ΔL| or ΔE found
    double wanted   = 0.0;   // the bound it missed
};

// What a palette breaks. `generator`: every rule, contrast as a WCAG ratio. Otherwise only "can these
// be told apart": contrast against `blendValue`, no separation ceilings or directions.
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
                if (rule.shareable && a == b) break;
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

// The name a message gives a colour.
inline const char* theme_field_label(Argb Theme::* field) {
    for (const ThemeColorRow& row : theme_color_rows()) {
        if (row.field == field) return row.label;
    }
    if (field == &Theme::meterBorder) return "MTR BORDER";
    if (field == &Theme::rowPlayback) return "ROW PLAY";
    if (field == &Theme::textCursor)  return "TXT CURSOR";
    if (field == &Theme::textSelection) return "TXT SELECT";
    return "?";
}

// What this editor row's colour clashes with, or `nullptr` — the message beside the title.
inline const char* theme_row_clash_partner(const Theme& t, int colorRowIndex) {
    const auto& rows = theme_color_rows();
    if (colorRowIndex < 0 || colorRowIndex >= static_cast<int>(rows.size())) return nullptr;
    Argb Theme::* const field = rows[static_cast<size_t>(colorRowIndex)].field;

    // The worst miss, by how far under its own bound.
    const char* worst = nullptr;
    double deficit = 0.0;
    for (const ThemeViolation& v : theme_violations(t, /*generator=*/false)) {
        // A ground is not the culprit of a contrast miss.
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
