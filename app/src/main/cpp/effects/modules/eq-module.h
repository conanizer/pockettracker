#pragma once
#include <cmath>
#include "../soundpipe/soundpipe.h"
#include "../primitives/daisysp/svf.h"

// ===========================================================================
// EqModule — 3-band parametric EQ.
//
// Band types (match Kotlin EqBand.type / EQ_BAND_TYPE_NAMES):
//   0 = off      (bypass)
//   1 = loShelf  (sp_pareq mode 1)
//   2 = lowcut   (highpass biquad — sp_pareq cannot do HP/LP)
//   3 = bell     (sp_pareq mode 0, peaking)
//   4 = hiShelf  (sp_pareq mode 2)
//   5 = hicut    (lowpass biquad)
//
// Parameters:
//   freqHz  — centre/corner frequency, 20–20000 Hz
//   gainDb  — gain in dB, −12..+12 (ignored for lowcut/hicut)
//   q       — bandwidth/Q, 0.1–10.0
// ===========================================================================

// One EQ band: sp_pareq for shelf/bell, DaisySP SVF for HP/LP.
// SVF is double-sampled so it stays stable near Nyquist.
// Q → SVF resonance: damp = 1/Q in SVF terms, res = 1 − 1/(2Q), clamped to [0, 1).
struct EqBandModule {
    sp_pareq pL, pR;
    sp_data  sp;
    bool bypass   = true;
    bool useHpLp  = false;  // true for type 2 (lowcut) and 5 (hicut)
    int  hpLpType = 0;      // 2=HP 5=LP, used in processMono/processStereo
    daisysp::Svf svfL, svfR;

    void reset(float sr) {
        sp.sr = sr;
        sp_pareq_init(&sp, &pL);
        sp_pareq_init(&sp, &pR);
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
            // damp = 1/Q in SVF internals; DaisySP maps res → damp = 2*(1-res)
            // → res = 1 - 1/(2Q), clamped so damp stays positive
            float res = fmaxf(0.0f, fminf(0.99f, 1.0f - 1.0f / (2.0f * q)));
            svfL.SetFreq(hz); svfL.SetRes(res); svfL.SetDrive(0.0f);
            svfR.SetFreq(hz); svfR.SetRes(res); svfR.SetDrive(0.0f);
        } else {
            // sp_pareq: mode 0=bell, 1=loShelf, 2=hiShelf
            int pareqMode = (type == 1) ? 1 : (type == 4) ? 2 : 0;
            float v    = powf(10.0f, gainDb / 20.0f);
            float mode = (float)pareqMode;
            pL.fc = freqHz; pL.v = v; pL.q = q; pL.mode = mode;
            pL.prv_fc = pL.prv_v = pL.prv_q = -1.0f;
            pR.fc = freqHz; pR.v = v; pR.q = q; pR.mode = mode;
            pR.prv_fc = pR.prv_v = pR.prv_q = -1.0f;
        }
    }

    inline float processMono(float in) {
        if (bypass) return in;
        if (useHpLp) {
            svfL.Process(in);
            return (hpLpType == 2) ? svfL.High() : svfL.Low();
        }
        SPFLOAT out, s = in;
        sp_pareq_compute(&sp, &pL, &s, &out);
        return out;
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
        SPFLOAT outL, outR, sL = L, sR = R;
        sp_pareq_compute(&sp, &pL, &sL, &outL);
        sp_pareq_compute(&sp, &pR, &sR, &outR);
        L = outL; R = outR;
    }
};

// Three-band parametric EQ.
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
