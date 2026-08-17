#ifndef POCKETTRACKER_SONGCORE_AUTOMATION_CURVE_H
#define POCKETTRACKER_SONGCORE_AUTOMATION_CURVE_H

// ─── The shape of a ramp, and nothing else ───────────────────────────────────────────────────────
//
// AUS/AUF's curve, the byte it produces at a position, and the one rule an EQ-preset morph adds on
// top. `automation.h` holds everything ELSE about automation — the registry, the pairing, the phrase
// walk — and includes this.
//
// ⚠️ **IT IS SPLIT OUT BECAUSE IT HAS TWO CALLERS ON OPPOSITE SIDES OF THE SONGCORE SEAM.** The
// phrase path reaches it through `automation.h`; the TABLE path reaches it from inside `AudioEngine`,
// which sits below the seam and must not see `model.h`. A second implementation over there would be a
// second thing to keep bit-identical, and a table morph that felt different from a phrase morph over
// the same two presets is exactly the inconsistency the feature exists to remove.
//
// So: `<cstdint>` and nothing else, ever.

#include <cstdint>

namespace songcore {

// ─── The curve ───────────────────────────────────────────────────────────────────────────────────
//
// AUS's value byte picks the shape, as a continuous family between three anchors. It is a POLYNOMIAL
// on purpose — `+ − ×` only, never `pow` or `exp`: the scheduling TUs compile with no fast-math and
// `-ffp-contract=off`, and the emitted values have to be bit-identical on every platform, which is
// the same reason note→Hz is vendored rather than called (event-schema.md).
constexpr int AUS_CURVE_EASE_IN  = 0x00;  // slow to leave, fast to arrive — cubic up
constexpr int AUS_CURVE_LINEAR   = 0x80;
constexpr int AUS_CURVE_EASE_OUT = 0xFF;  // fast to leave, slow to arrive

/**
 * The eased position, `t` and the result both in [0,1]. 0x80 is exactly `t`; either side blends
 * linearly towards the cubic anchor, so the family is continuous through the middle and the two
 * halves meet at the same value.
 */
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

/**
 * The byte a ramp holds at position `t`. Rounded half-up — both ends are 0-255 and the shape never
 * leaves [0,1], so the value is never negative and the rounding needs no sign case.
 */
inline int automation_value_byte(int startByte, int destByte, int curveByte, double t) {
    const double v = startByte + (destByte - startByte) * automation_shape(curveByte, t);
    const int    b = static_cast<int>(v + 0.5);
    return b < 0 ? 0 : (b > 255 ? 255 : b);
}

// ─── One EQ band, morphed ────────────────────────────────────────────────────────────────────────
//
// The band as AUTHORED HEX, which is the domain both morph paths interpolate in: hex is what makes a
// frequency sweep linear in log-frequency, and it is what the project file and the FX cells hold. The
// engine's own `EqBandsHex` and the model's `EqBand` both convert to and from this shape a band at a
// time, so neither type has to be visible here.
struct AutomationEqBand {
    int type = 0;     // 0 OFF | 1 LOSHELF | 2 LOWCUT | 3 BELL | 4 HISHELF | 5 HICUT
    int freq = 128;   // 00-FF → 20-20000 Hz, log
    int gain = 120;   // 0-240 → −12.0..+12.0 dB (120 = flat)
    int q    = 128;   // 00-FF → 0.1-10.0, log
};

/**
 * One band of an EQ-preset morph at position `t`.
 *
 * ⚠️ **THE START PRESET'S BAND TYPE SURVIVES THE WHOLE SPAN, ARRIVAL INCLUDED.** A type is not an
 * interpolable quantity — there is no continuous path from BELL to HISHELF, and LOWCUT/HICUT run
 * through the SVF and have no gain at all — so one of the two ends has to win, for every tick. The
 * start wins, and the morph never snaps:
 *
 *   • Types that AGREE (the normal case, and the one to write) make the arrival exactly the
 *     destination preset, by construction rather than by rounding.
 *   • Types that DIFFER still sweep that band's frequency and gain under the start's type, and the
 *     ramp rests on a setting no preset holds. To land on the real destination, write it — `EQM 12`
 *     on the step after the AUF is one cell, visible in the grid, and then the discontinuity is the
 *     author's rather than the ramp's.
 *
 * ⭐ A band OFF in the start preset stays off for the whole ramp, whatever the destination says — so
 * `chain.eq.active` (derived from "some type ≠ 0") cannot change mid-sweep and no band can pop in or
 * out. Fade a band out by ramping its GAIN to 0 dB (0x78) instead.
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
