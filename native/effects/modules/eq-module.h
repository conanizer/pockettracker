#pragma once
#include <cmath>
#include "../soundpipe/soundpipe.h"
#include "../primitives/daisysp/svf.h"
#include "../glide.h"

// The 3-band parametric EQ. Band types — the number is stored in projects, append only:
// 0 off, 1 low shelf, 2 low cut, 3 bell, 4 high shelf, 5 high cut.
// Frequency 20 Hz–20 kHz, gain ±12 dB (not for the cuts), Q 0.1–10.

// One sample through a shelf/bell band: `sp_pareq_compute` derives the coefficients once after a
// change, then the same biquad runs inline.
inline float pareq_tick(sp_data& sp, sp_pareq& p, bool& derive, float in) {
    if (derive) {
        derive = false;
        SPFLOAT out;
        sp_pareq_compute(&sp, &p, &in, &out);
        return out;
    }
    const float yn = p.b0 * in + p.b1 * p.xnm1 + p.b2 * p.xnm2 - p.a1 * p.ynm1 - p.a2 * p.ynm2;
    p.xnm2 = p.xnm1;
    p.xnm1 = in;
    p.ynm2 = p.ynm1;
    p.ynm1 = yn;
    return yn;
}

// One band's filter: sp_pareq for shelves and bell, DaisySP's SVF for the cuts.
struct EqBandFilter {
    sp_pareq pL, pR;
    sp_data  sp;
    // Coefficients owed, per side (a mono voice never runs pR). Set wherever fc, v or q change.
    bool deriveL = true, deriveR = true;
    bool bypass   = true;
    bool useHpLp  = false;  // types 2 and 5
    int  hpLpType = 0;      // 2 = HP, 5 = LP
    daisysp::Svf svfL, svfR;

    void reset(float sr) {
        sp.sr = sr;
        sp_pareq_init(&sp, &pL);
        sp_pareq_init(&sp, &pR);
        deriveL = deriveR = true;
        svfL.Init(sr);
        svfR.Init(sr);
        bypass = true; useHpLp = false; hpLpType = 0;
    }

    // A filter coming back after the other kind ran starts from silence, not from its stale state.
    void set(int type, float freqHz, float gainDb, float q) {
        const bool wasOn = !bypass, wasHpLp = useHpLp;
        bypass  = (type == 0);
        if (bypass) return;

        useHpLp  = (type == 2 || type == 5);
        hpLpType = type;
        const bool cold = !wasOn || wasHpLp != useHpLp;

        if (useHpLp) {
            if (cold) { svfL.Init(sp.sr); svfR.Init(sp.sr); }
            float hz  = fminf(freqHz, sp.sr * 0.45f);
            // damp = 1/Q, and DaisySP's damp is 2(1 − res).
            float res = fmaxf(0.0f, fminf(0.99f, 1.0f - 1.0f / (2.0f * q)));
            svfL.SetFreq(hz); svfL.SetRes(res); svfL.SetDrive(0.0f);
            svfR.SetFreq(hz); svfR.SetRes(res); svfR.SetDrive(0.0f);
        } else {
            if (cold) { sp_pareq_init(&sp, &pL); sp_pareq_init(&sp, &pR); }
            int pareqMode = (type == 1) ? 1 : (type == 4) ? 2 : 0;   // 0 bell, 1 low shelf, 2 high shelf
            float v    = powf(10.0f, gainDb / 20.0f);
            float mode = (float)pareqMode;
            pL.fc = freqHz; pL.v = v; pL.q = q; pL.mode = mode;
            pL.prv_fc = pL.prv_v = pL.prv_q = -1.0f;
            pR.fc = freqHz; pR.v = v; pR.q = q; pR.mode = mode;
            pR.prv_fc = pR.prv_v = pR.prv_q = -1.0f;
            deriveL = deriveR = true;
        }
    }

    inline float processMono(float in) {
        if (bypass) return in;
        if (useHpLp) {
            svfL.Process(in);
            return (hpLpType == 2) ? svfL.High() : svfL.Low();
        }
        return pareq_tick(sp, pL, deriveL, in);
    }

    inline void processStereo(float& L, float& R) {
        if (bypass) return;
        if (useHpLp) {
            svfL.Process(L);
            svfR.Process(R);
            if (hpLpType == 2) { L = svfL.High(); R = svfR.High(); }
            else               { L = svfL.Low();  R = svfR.Low();  }
            return;
        }
        L = pareq_tick(sp, pL, deriveL, L);
        R = pareq_tick(sp, pR, deriveR, R);
    }
};

// One band. The glide rule (glide.h) as a crossfade from the old settings' output; the same settings
// sent again change nothing.
struct EqBandModule {
    EqBandFilter now;
    EqBandFilter was;      // the settings fading out
    Ramp  oldShare;        // was's share of the output, 1 to 0
    int   type   = 0;
    float freqHz = 0.f, gainDb = 0.f, q = 0.f;

    void reset(float sr) {
        now.reset(sr);
        oldShare = Ramp{};
        type = 0;
    }

    // Filtering, or fading out.
    bool on() const { return type != 0 || oldShare.moving(); }

    void setParams(int t, float hz, float db, float qq, bool sounding) {
        if (t == type && (t == 0 || (hz == freqHz && db == gainDb && qq == q))) return;
        if (sounding) {
            was = now;
            oldShare.start(1.0f, 0.0f, GLIDE_SAMPLES);
        }
        now.set(t, hz, db, qq);
        type = t; freqHz = hz; gainDb = db; q = qq;
    }

    inline float processMono(float in) {
        float out = now.processMono(in);
        if (oldShare.moving()) {
            const float k = oldShare.next();
            out = was.processMono(in) * k + out * (1.0f - k);
        }
        return out;
    }

    inline void processStereo(float& L, float& R) {
        if (!oldShare.moving()) { now.processStereo(L, R); return; }
        float oL = L, oR = R;
        now.processStereo(L, R);
        was.processStereo(oL, oR);
        const float k = oldShare.next();
        L = oL * k + L * (1.0f - k);
        R = oR * k + R * (1.0f - k);
    }
};

struct EqModule {
    EqBandModule bands[3];
    bool fresh = true;   // until sound has passed: a band set lands at once

    void reset(float sr) {
        for (auto& b : bands) b.reset(sr);
        fresh = true;
    }

    // Some band on or fading: off, a sample through costs nothing.
    bool active() const { return bands[0].on() || bands[1].on() || bands[2].on(); }

    void setBand(int i, int type, float freqHz, float gainDb, float q) {
        bands[i].setParams(type, freqHz, gainDb, q, !fresh);
    }

    inline float processMono(float in) {
        fresh = false;
        if (!active()) return in;
        in = bands[0].processMono(in);
        in = bands[1].processMono(in);
        return bands[2].processMono(in);
    }

    inline void processStereo(float& L, float& R) {
        fresh = false;
        if (!active()) return;
        bands[0].processStereo(L, R);
        bands[1].processStereo(L, R);
        bands[2].processStereo(L, R);
    }
};
