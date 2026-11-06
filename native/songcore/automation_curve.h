#ifndef POCKETTRACKER_SONGCORE_AUTOMATION_CURVE_H
#define POCKETTRACKER_SONGCORE_AUTOMATION_CURVE_H

// The ramp's curve, the AUS/AUF pairing and the EQ-morph band rule, shared by phrases and the
// engine's tables.
// ⚠️ `<cstdint>` only, ever — the engine must not see model.h.

#include <cstdint>

namespace songcore {

// AUS's byte picks a shape between three anchors.
// ⚠️ Polynomial only, never `pow`/`exp`: the values must be bit-identical on every platform.
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

/** A byte that is two numbers (PVB / PVX: speed, depth): each nibble ramps on its own, so the low one
 *  never wraps through 0-F on the way. */
inline int automation_value_nibbles(int startByte, int destByte, int curveByte, double t) {
    const int hi = automation_value_byte((startByte >> 4) & 0x0F, (destByte >> 4) & 0x0F, curveByte, t);
    const int lo = automation_value_byte(startByte & 0x0F, destByte & 0x0F, curveByte, t);
    return (hi << 4) | lo;
}

// One EQ band in authored hex — the domain both paths interpolate in.
struct AutomationEqBand {
    int type = 0;     // 0 OFF | 1 LOSHELF | 2 LOWCUT | 3 BELL | 4 HISHELF | 5 HICUT
    int freq = 128;   // 00-FF → 20-20000 Hz, log
    int gain = 120;   // 0-240 → −12.0..+12.0 dB (120 = flat)
    int q    = 128;   // 00-FF → 0.1-10.0, log
};

// One band of an EQ morph at `t`. ⚠️ The START preset's band type holds for the whole span, arrival
// included; a band OFF at the start stays off.
inline AutomationEqBand automation_eq_band_at(const AutomationEqBand& from, const AutomationEqBand& to,
                                              int curveByte, double t) {
    AutomationEqBand m;
    m.type = from.type < 0 ? 0 : (from.type > 255 ? 255 : from.type);
    m.freq = automation_value_byte(from.freq, to.freq, curveByte, t);
    m.gain = automation_value_byte(from.gain, to.gain, curveByte, t);
    m.q    = automation_value_byte(from.q,    to.q,    curveByte, t);
    return m;
}

// ─── The pairing: one walk for a phrase, a chain and a table ─────────────────────────────────────

constexpr int RAMP_AUS_CODE        = 0x2D;   // xx = the curve
constexpr int RAMP_AUF_CODE        = 0x2E;   // xx = the destination
constexpr int RAMP_EQ_PRESET_SLOTS = 128;

// What the effect to the left of an AUS is to a ramp.
enum class RampArm { NONE, VALUE, EQ_SLOT };   // EQ_SLOT: its endpoints are EQ preset slots

// One row's or step's three FX slots, as 1-3 ([0] unused).
struct RampRowFx {
    int type[4]  = {};
    int value[4] = {};
};

template <typename Row> inline RampRowFx ramp_row_fx(const Row& r) {
    RampRowFx fx;
    fx.type[1] = r.fx1Type;  fx.value[1] = r.fx1Value;
    fx.type[2] = r.fx2Type;  fx.value[2] = r.fx2Value;
    fx.type[3] = r.fx3Type;  fx.value[3] = r.fx3Value;
    return fx;
}

// One paired ramp, in the walk's positions.
struct RampPair {
    int     paramType = 0;   // the effect being ramped
    RampArm arm       = RampArm::NONE;
    int     ausPos = -1, paramSlot = 0, ausSlot = 0, startByte = 0, curveByte = AUS_CURVE_LINEAR;
    int     aufPos = -1, aufSlot = 0, destByte = 0;
};

// AUS takes the nearest ramp-able effect to its LEFT (none: inert); the first AUF on a LATER position
// closes it; the last AUS wins (no nesting); an AUS still open at the end produces nothing. On one
// row the AUFs close before the AUSes open, whatever their columns, so a fade can start where one
// ends. An EQ endpoint that is not a slot ends the search at the AUS, and at an AUF leaves the AUS
// open for a later one. `row_at(pos)` → RampRowFx, `arm_of(type)` → RampArm, `emit(const RampPair&)`.
template <typename RowAt, typename ArmOf, typename Emit>
inline void pair_ramps(int count, RowAt row_at, ArmOf arm_of, Emit emit) {
    const auto endpoint_ok = [](RampArm a, int v) {
        return a != RampArm::EQ_SLOT || (v >= 0 && v < RAMP_EQ_PRESET_SLOTS);
    };
    RampPair open;
    bool     isOpen = false;
    for (int pos = 0; pos < count; ++pos) {
        const RampRowFx fx = row_at(pos);
        for (int slot = 1; slot <= 3 && isOpen; ++slot) {
            if (fx.type[slot] != RAMP_AUF_CODE || !endpoint_ok(open.arm, fx.value[slot])) continue;
            open.aufPos   = pos;
            open.aufSlot  = slot;
            open.destByte = fx.value[slot];
            emit(static_cast<const RampPair&>(open));
            isOpen = false;
        }
        for (int slot = 1; slot <= 3; ++slot) {
            if (fx.type[slot] != RAMP_AUS_CODE) continue;
            for (int left = slot - 1; left >= 1; --left) {
                const RampArm arm = arm_of(fx.type[left]);
                if (arm == RampArm::NONE) continue;
                if (!endpoint_ok(arm, fx.value[left])) break;   // the nearest one IS the parameter
                open           = RampPair{};
                open.paramType = fx.type[left];
                open.arm       = arm;
                open.ausPos    = pos;
                open.paramSlot = left;
                open.ausSlot   = slot;
                open.startByte = fx.value[left];
                open.curveByte = fx.value[slot];
                isOpen         = true;
                break;
            }
        }
    }
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_AUTOMATION_CURVE_H
