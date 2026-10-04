#ifndef POCKETTRACKER_SONGCORE_AUTOMATION_CURVE_H
#define POCKETTRACKER_SONGCORE_AUTOMATION_CURVE_H

// ─── The shape of a ramp ─────────────────────────────────────────────────────────────────────────
//
// AUS/AUF's curve and the EQ-morph band rule. Split from `automation.h` because the table path in
// `AudioEngine`, below the songcore seam, uses it too and must not see `model.h`.
// ⚠️ `<cstdint>` only, ever — one implementation keeps phrase and table morphs identical.

#include <cstdint>

namespace songcore {

// ─── The curve ───────────────────────────────────────────────────────────────────────────────────
//
// AUS's byte picks a shape between three anchors.
// ⚠️ Polynomial only (`+ − ×`, never `pow`/`exp`): the emitted values must be bit-identical on every
// platform.
constexpr int AUS_CURVE_EASE_IN  = 0x00;  // slow to leave, fast to arrive — cubic up
constexpr int AUS_CURVE_LINEAR   = 0x80;
constexpr int AUS_CURVE_EASE_OUT = 0xFF;  // fast to leave, slow to arrive

/** The eased position, `t` and result in [0,1]. 0x80 is exactly `t`; each side blends linearly
 *  towards its cubic anchor, so the family is continuous through the middle. */
inline double automation_shape(int curveByte, double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    if (curveByte <= AUS_CURVE_LINEAR) {
        const double w = curveByte / 128.0;                 // 00 → all ease-in, 80 → all linear
        return (1.0 - w) * (t * t * t) + w * t;
    }
    const double w = (curveByte - AUS_CURVE_LINEAR) / 127.0;  // 80 → all linear, FF → all ease-out
    const double u = 1.0 - t;
    return (1.0 - w) * t + w * (1.0 - u * u * u);
}

/** The byte a ramp holds at position `t`, rounded half-up (never negative, so no sign case). */
inline int automation_value_byte(int startByte, int destByte, int curveByte, double t) {
    const double v = startByte + (destByte - startByte) * automation_shape(curveByte, t);
    const int    b = static_cast<int>(v + 0.5);
    return b < 0 ? 0 : (b > 255 ? 255 : b);
}

// ─── One EQ band, morphed ────────────────────────────────────────────────────────────────────────
//
// The band in authored hex — the domain both paths interpolate in (log-frequency is linear in hex).
struct AutomationEqBand {
    int type = 0;     // 0 OFF | 1 LOSHELF | 2 LOWCUT | 3 BELL | 4 HISHELF | 5 HICUT
    int freq = 128;   // 00-FF → 20-20000 Hz, log
    int gain = 120;   // 0-240 → −12.0..+12.0 dB (120 = flat)
    int q    = 128;   // 00-FF → 0.1-10.0, log
};

/**
 * One band of an EQ-preset morph at position `t`.
 *
 * ⚠️ The START preset's band type holds for the whole span, arrival included — types cannot be
 * interpolated. When both ends agree the arrival is exactly the destination; when they differ, the
 * ramp rests on a setting no preset holds (write `EQM xx` after the AUF to land on it).
 * A band OFF at the start stays off, so `chain.eq.active` cannot change mid-sweep. Fade a band by
 * ramping its gain to 0 dB (0x78).
 */
inline AutomationEqBand automation_eq_band_at(const AutomationEqBand& from, const AutomationEqBand& to,
                                              int curveByte, double t) {
    AutomationEqBand m;
    m.type = from.type < 0 ? 0 : (from.type > 255 ? 255 : from.type);
    m.freq = automation_value_byte(from.freq, to.freq, curveByte, t);
    m.gain = automation_value_byte(from.gain, to.gain, curveByte, t);
    m.q    = automation_value_byte(from.q,    to.q,    curveByte, t);
    return m;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_AUTOMATION_CURVE_H
