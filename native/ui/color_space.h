#pragma once

// ─── Perceptual colour ───────────────────────────────────────────────────────────────────────────
//
// OKLab/OKLCh and the WCAG contrast ratio, over the `Argb` the theme stores.
//
// ⚠️ **THE CONSTANTS ARE COPIED, NOT DERIVED HERE** — from `soreja/colorm` (public domain, the
// Unlicense), commit `4d41b6e0fbc32b3237e7e7171850e5f461368050`, keeping only the conversions and the
// contrast ratio. Its CSS colour parser is what the rest of that header is for, and it drags
// `<sstream>` and `<iomanip>` in with it; nothing in this app parses a colour string. A digit changed
// in a matrix below is unverifiable without that provenance, which is why it is written down.
//
// ⭐ **Why a second colour space at all:** OKLab's L is perceptually uniform, so "a shade lighter than
// the background" is one number that means the same thing on a dark palette and a light one — the
// thing HSL's L does not give. Chroma is independent of it, so "another shade of the same colour" is
// literally "hold H, move L and C".
//
// ⚠️ **TWO DIFFERENT LIGHTNESSES LIVE HERE AND THEY ARE NOT INTERCHANGEABLE.** `OkLab::L` is
// perceptual and is what the palette's ladders are built on; `relative_luminance` is the WCAG
// quantity and is only ever a step inside `wcag_contrast`. Legibility bounds are ratios of the
// second; separation bounds are differences of the first.

#include <cmath>
#include <cstdint>

#include "ui/theme.h"   // Argb

namespace pt::ui {

struct OkLab { double L = 0.0, a = 0.0, b = 0.0; };
struct OkLch { double L = 0.0, C = 0.0, H = 0.0; };   // H in degrees, 0..360

namespace color_detail {

/** sRGB transfer function, both directions. 0..1 in, 0..1 out. */
inline double to_linear(double v) {
    return (v <= 0.04045) ? (v / 12.92) : std::pow((v + 0.055) / 1.055, 2.4);
}
inline double to_gamma(double v) {
    return (v <= 0.0031308) ? (v * 12.92) : (1.055 * std::pow(v, 1.0 / 2.4) - 0.055);
}

inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

/** Is this linear-RGB triple inside the sRGB cube? A hair of slack, so a round trip is not a miss. */
inline bool in_gamut(double r, double g, double b) {
    constexpr double EPS = 1e-6;
    return r >= -EPS && r <= 1.0 + EPS && g >= -EPS && g <= 1.0 + EPS && b >= -EPS && b <= 1.0 + EPS;
}

/** OKLab → linear RGB, before any clamping. */
inline void oklab_to_linear(const OkLab& c, double& r, double& g, double& b) {
    const double l_ = c.L + 0.3963377774 * c.a + 0.2158037573 * c.b;
    const double m_ = c.L - 0.1055613458 * c.a - 0.0638541728 * c.b;
    const double s_ = c.L - 0.0894841775 * c.a - 1.2914855480 * c.b;
    const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    r =  4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}

}  // namespace color_detail

// ─── sRGB ↔ OKLab ────────────────────────────────────────────────────────────────────────────────

inline OkLab to_oklab(Argb c) {
    using namespace color_detail;
    const double r = to_linear(static_cast<double>((c >> 16) & 0xFF) / 255.0);
    const double g = to_linear(static_cast<double>((c >> 8) & 0xFF) / 255.0);
    const double b = to_linear(static_cast<double>(c & 0xFF) / 255.0);

    const double l = std::cbrt(0.4122214708018041 * r + 0.5363325363454300 * g + 0.0514459928527659 * b);
    const double m = std::cbrt(0.2119034982505858 * r + 0.6806995451361225 * g + 0.1073969566132915 * b);
    const double s = std::cbrt(0.0883024618887421 * r + 0.2817188376235317 * g + 0.6299787004877261 * b);

    return {0.2104542682745812 * l + 0.7936177747300267 * m - 0.0040720430046080 * s,
            1.9779985323885080 * l - 2.4285922419362860 * m + 0.4505937095477779 * s,
            0.0259040424876582 * l + 0.7827717124269177 * m - 0.8086757549145759 * s};
}

/**
 * OKLab → `Argb`, always opaque.
 *
 * ⚠️ **OUT-OF-GAMUT COLOURS LOSE CHROMA, NEVER LIGHTNESS.** Most of the OKLab cube is not printable
 * in sRGB, and clamping the three channels is the obvious answer and the wrong one: it shifts the hue
 * and quietly moves L, so a generator that asked for a given lightness would not get it and the
 * validator would then disagree with the generator about what it built. Desaturating toward the grey
 * of the same L keeps both L and H exactly and gives up only what could not be shown anyway.
 */
inline Argb to_argb(const OkLab& c) {
    using namespace color_detail;

    double r = 0.0, g = 0.0, b = 0.0;
    oklab_to_linear(c, r, g, b);

    if (!in_gamut(r, g, b)) {
        // Binary search on the chroma scale. 18 steps is finer than one 8-bit code, so the result is
        // the same as an exact solve once it is quantized.
        double lo = 0.0, hi = 1.0;
        for (int i = 0; i < 18; ++i) {
            const double mid = 0.5 * (lo + hi);
            const OkLab trial{c.L, c.a * mid, c.b * mid};
            oklab_to_linear(trial, r, g, b);
            if (in_gamut(r, g, b)) lo = mid; else hi = mid;
        }
        const OkLab best{c.L, c.a * lo, c.b * lo};
        oklab_to_linear(best, r, g, b);
    }

    const auto ch = [](double v) {
        return static_cast<Argb>(std::lround(clamp01(to_gamma(clamp01(v))) * 255.0));
    };
    return 0xFF000000u | (ch(r) << 16) | (ch(g) << 8) | ch(b);
}

// ─── OKLab ↔ OKLCh ───────────────────────────────────────────────────────────────────────────────

inline OkLch to_oklch(const OkLab& c) {
    const double chroma = std::sqrt(c.a * c.a + c.b * c.b);
    // A grey has no hue to report, and returning atan2(0, 0) as one makes a hue rotation of grey
    // produce a colour. Hue stays 0 and chroma stays 0, so the round trip is still grey.
    if (chroma < 1e-9) return {c.L, 0.0, 0.0};
    double hue = std::atan2(c.b, c.a) * 180.0 / 3.14159265358979323846;
    if (hue < 0.0) hue += 360.0;
    return {c.L, chroma, hue};
}

inline OkLab to_oklab(const OkLch& c) {
    const double rad = c.H * 3.14159265358979323846 / 180.0;
    const double chroma = c.C < 0.0 ? 0.0 : c.C;
    return {c.L, chroma * std::cos(rad), chroma * std::sin(rad)};
}

inline OkLch to_oklch(Argb c)          { return to_oklch(to_oklab(c)); }
inline Argb  to_argb(const OkLch& c)   { return to_argb(to_oklab(c)); }

// ─── The two measurements the rules are written in ───────────────────────────────────────────────

/** Perceptual lightness, 0 (black) .. 1 (white). The palette's ladders and the polarity read this. */
inline double ok_lightness(Argb c) { return to_oklab(c).L; }

/** Straight-line distance in OKLab — "are these two the same colour to look at". */
inline double ok_delta_e(Argb x, Argb y) {
    const OkLab p = to_oklab(x), q = to_oklab(y);
    const double dL = p.L - q.L, da = p.a - q.a, db = p.b - q.b;
    return std::sqrt(dL * dL + da * da + db * db);
}

/** WCAG relative luminance — a step inside `wcag_contrast`, and not a lightness. See the header. */
inline double relative_luminance(Argb c) {
    using color_detail::to_linear;
    return 0.2126390058715102 * to_linear(static_cast<double>((c >> 16) & 0xFF) / 255.0) +
           0.7151686787677560 * to_linear(static_cast<double>((c >> 8) & 0xFF) / 255.0) +
           0.0721923153607337 * to_linear(static_cast<double>(c & 0xFF) / 255.0);
}

/** The WCAG 2.0 contrast ratio, 1.0 .. 21.0. Symmetric — the lighter one is found, not assumed. */
inline double wcag_contrast(Argb x, Argb y) {
    const double a = relative_luminance(x) + 0.05;
    const double b = relative_luminance(y) + 0.05;
    return (a > b) ? (a / b) : (b / a);
}

}  // namespace pt::ui
