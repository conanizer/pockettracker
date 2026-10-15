#pragma once
#include <cmath>
#include "../soundpipe/soundpipe.h"
#include "../primitives/daisysp/svf.h"

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

// One band: sp_pareq for shelves and bell, DaisySP's SVF for the cuts.
struct EqBandModule {
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

    void setParams(int type, float freqHz, float gainDb, float q) {
        bypass  = (type == 0);
        if (bypass) return;

        useHpLp  = (type == 2 || type == 5);
        hpLpType = type;

        if (useHpLp) {
            float hz  = fminf(freqHz, sp.sr * 0.45f);
            // damp = 1/Q, and DaisySP's damp is 2(1 − res).
            float res = fmaxf(0.0f, fminf(0.99f, 1.0f - 1.0f / (2.0f * q)));
            svfL.SetFreq(hz); svfL.SetRes(res); svfL.SetDrive(0.0f);
            svfR.SetFreq(hz); svfR.SetRes(res); svfR.SetDrive(0.0f);
        } else {
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

struct EqModule {
    EqBandModule bands[3];
    bool active = false;

    void reset(float sr) {
        for (auto& b : bands) b.reset(sr);
        active = false;
    }

    inline float processMono(float in) {
        if (!active) return in;
        in = bands[0].processMono(in);
        in = bands[1].processMono(in);
        return bands[2].processMono(in);
    }

    inline void processStereo(float& L, float& R) {
        if (!active) return;
        bands[0].processStereo(L, R);
        bands[1].processStereo(L, R);
        bands[2].processStereo(L, R);
    }
};
