#pragma once
#include <cmath>
#include "../cell-curves.h"
#include "../glide.h"
#include "../primitives/daisysp/moogladder.h"
#include "../primitives/daisysp/svf.h"
#include "../primitives/k35-filter.h"
#include "../../songcore/voice_codes.h"

// A voice's resonant filter, of a songcore::FilterType: LP to PEAK are taps of one DaisySP
// state-variable filter, then the Korg 35 (sst-filters) and the Moog ladder (DaisySP-LGPL).
// Cutoff 0-255 → 20 Hz–20 kHz exponential, resonance 0-255 → 0..1, drive 0-255 → SetDrive 0..10 (the
// state-variable filter only). `setParams` once per block when one changes.
struct FilterModule {
    // Which filter a type runs: the five state-variable types share one, so they glide into each other.
    enum Engine { NONE, SVF, K35, LADDER };
    static constexpr int engine_of(int t) {
        using namespace songcore;
        return t == FILTER_OFF ? NONE : t <= FILTER_PEAK ? SVF : t == FILTER_K35 ? K35 : LADDER;
    }

    // The engine's coefficients: SVF (freq, damp), K35 (G, k, alpha), LADDER (tune, res4).
    struct Coeffs { float a = 0.25f, b = 0.0f, c = 0.0f; };

    // The glide rule (glide.h): `fresh` until the note's first sample.
    int   type       = songcore::FILTER_OFF;
    int   drive      = 150;
    float sampleRate = 44100.0f;
    daisysp::Svf        svfL, svfR;    // mono or left channel; right channel (SF stereo only)
    K35Filter           k35L, k35R;
    daisysp::MoogLadder ladL, ladR;
    daisysp::MoogLadder::Coeffs ladC;

    // Whose coefficients the sets below hold; they glide only within one engine.
    int    coeffEngine = SVF;
    // The block's start and end coefficients, interpolated per sample.
    Coeffs prev, target;

    bool   fresh = true;
    // The coefficients the last sample used, and a glide away from them: their share, 1 to 0.
    Coeffs last, from;
    Ramp   glide;
    // A type change crossfades from the old type's output (0 = the dry input) to the new one's. The old
    // engine keeps the coefficients it last had.
    int    prevType = songcore::FILTER_OFF;
    Ramp   typeFade;

    void reset() {
        type = songcore::FILTER_OFF;
        fresh = true;
        glide = typeFade = Ramp{};
        initEngines(sampleRate);
        coeffEngine = SVF;
        prev.a = target.a = svfL.GetFreq();
        prev.b = target.b = svfL.GetDamp();
    }

    // Call before setParams each block.
    void snapshotCoeffs() { prev = target; }

    void setParams(int filterType, int cutoff, int resonance, int filterDrive, float sr) {
        const int oldType = type;
        type  = filterType;
        drive = filterDrive;
        const int eng = engine_of(filterType);
        if (sr != sampleRate) {
            sampleRate = sr;
            initEngines(sr);
        } else if (!fresh && eng != NONE && eng != engine_of(oldType)) {
            // Switched on mid-note: its state holds whatever it held when it was last on.
            initEngine(eng, sr);
        }
        float hz  = cutoff_hz(cutoff);
        hz = fminf(hz, sr * 0.45f);
        const float res = resonance / 255.0f;

        // Off keeps computing for the engine it fades out of.
        const int  ce      = eng != NONE ? eng : coeffEngine;
        const bool sameEng = ce == coeffEngine;
        coeffEngine = ce;
        Coeffs next;
        switch (ce) {
            case K35: {
                const K35Filter::Coeffs k = K35Filter::coeffs(hz, res, sr);
                next = {k.G, k.k, k.alpha};
                break;
            }
            case LADDER: {
                const daisysp::MoogLadder::Coeffs k = ladL.MakeCoeffs(hz, res);
                next = {k.tune, k.res4, 0.0f};
                break;
            }
            default: {
                const float drv = filterDrive / 25.5f;
                svfL.SetFreq(hz);  svfL.SetRes(res);  svfL.SetDrive(drv);
                svfR.SetFreq(hz);  svfR.SetRes(res);  svfR.SetDrive(drv);
                next.a = svfL.GetFreq();
                next.b = svf_damp(filterType, res, next.a, svfL.GetDamp());
            }
        }
        const bool bothOn = oldType != songcore::FILTER_OFF && filterType != songcore::FILTER_OFF;
        if (!fresh && sameEng && bothOn && (next.a != target.a || next.b != target.b || next.c != target.c)) {
            from = last;
            glide.start(1.0f, 0.0f, GLIDE_SAMPLES);
        }
        target = next;
        if ((oldType == songcore::FILTER_OFF && filterType != songcore::FILTER_OFF) || !sameEng) {   // nothing to glide from: start where it is set
            prev = last = target;
        }
        if (!sameEng) glide = Ramp{};   // the old engine's coefficients mean nothing to this one
        if (!fresh && oldType != filterType) {
            prevType = oldType;
            typeFade.start(1.0f, 0.0f, GLIDE_SAMPLES);
        }
    }

    // Per sample, before process*: blends the coefficients without recomputing them.
    inline void setInterpolatedCoeffs(float t) {
        if (!enabled() && !typeFade.moving()) return;
        Coeffs k{prev.a + (target.a - prev.a) * t, prev.b + (target.b - prev.b) * t, prev.c + (target.c - prev.c) * t};
        if (glide.moving()) {
            const float g = glide.next();
            k.a = from.a * g + k.a * (1.0f - g);
            k.b = from.b * g + k.b * (1.0f - g);
            k.c = from.c * g + k.c * (1.0f - g);
        }
        last = k;
        switch (coeffEngine) {
            case K35:    k35L.c = k35R.c = {k.a, k.b, k.c}; break;
            case LADDER: ladC = {k.a, k.b}; break;
            default:     svfL.SetCoeffs(k.a, k.b); svfR.SetCoeffs(k.a, k.b);
        }
    }

    bool enabled() const { return type != songcore::FILTER_OFF; }

    inline float processMono(float in) {
        fresh = false;
        if (!enabled() && !typeFade.moving()) return in;
        const int eng = engine_of(type);
        const float y = run(eng, svfL, k35L, ladL, in);
        float out = tap(svfL, type, in, y);
        if (typeFade.moving()) {
            const float k = typeFade.next();
            const int   pe = engine_of(prevType);
            const float yp = pe == eng ? y : run(pe, svfL, k35L, ladL, in);
            out = tap(svfL, prevType, in, yp) * k + out * (1.0f - k);
        }
        return out;
    }

    inline void processStereo(float& L, float& R) {
        fresh = false;
        if (!enabled() && !typeFade.moving()) return;
        const int eng = engine_of(type);
        const float yL = run(eng, svfL, k35L, ladL, L), yR = run(eng, svfR, k35R, ladR, R);
        float outL = tap(svfL, type, L, yL), outR = tap(svfR, type, R, yR);
        if (typeFade.moving()) {
            const float k = typeFade.next();
            const int   pe = engine_of(prevType);
            const float pL = pe == eng ? yL : run(pe, svfL, k35L, ladL, L);
            const float pR = pe == eng ? yR : run(pe, svfR, k35R, ladR, R);
            outL = tap(svfL, prevType, L, pL) * k + outL * (1.0f - k);
            outR = tap(svfR, prevType, R, pR) * k + outR * (1.0f - k);
        }
        L = outL;
        R = outR;
    }

private:
    // The ladder's stages saturate near ±1/THERMAL: a ±1 signal is scaled into that range and back.
    static constexpr float LADDER_SCALE = 30000.0f;

    // One sample through an engine. The state-variable filter's outputs are read by `tap` afterwards.
    inline float run(int eng, daisysp::Svf& s, K35Filter& k35, daisysp::MoogLadder& lad, float in) {
        switch (eng) {
            case SVF:    s.Process(in); return 0.0f;
            case K35:    return k35.process(in);
            case LADDER: return lad.Process(in * LADDER_SCALE, ladC) * (1.0f / LADDER_SCALE);
            default:     return in;
        }
    }

    // BP and NT get their own width over RES: BP 1 octave at 00 (it starts no wider), NT 3 octaves at 00
    // and half an octave at FF — undamped, a notch passes the input unchanged. Kept inside the filter's
    // stability bound, which only a wide notch at a high cutoff reaches.
    static float svf_damp(int t, float res, float freq, float shared) {
        float d;
        if (t == songcore::FILTER_BP)         d = 0.7f * (1.0f - powf(res, 0.9f));
        else if (t == songcore::FILTER_NOTCH) d = 3.0f * powf(0.35f / 3.0f, res);
        else return shared;
        return fminf(d, 0.95f * (2.0f / freq - freq * 0.5f));
    }

    static inline float tap(daisysp::Svf& s, int t, float in, float engineOut) {
        switch (t) {
            case songcore::FILTER_LP:    return s.Low();
            case songcore::FILTER_HP:    return s.High();
            case songcore::FILTER_BP:    return s.Band();
            case songcore::FILTER_NOTCH: return s.Notch();
            case songcore::FILTER_PEAK:  return s.Peak();
            case songcore::FILTER_OFF:   return in;
            default:                     return engineOut;
        }
    }

    void initEngine(int eng, float sr) {
        switch (eng) {
            case SVF:    svfL.Init(sr); svfR.Init(sr); break;
            case K35:    k35L.reset(); k35R.reset(); break;
            case LADDER: ladL.Init(sr); ladR.Init(sr); break;
            default: break;
        }
    }
    void initEngines(float sr) { initEngine(SVF, sr); initEngine(K35, sr); initEngine(LADDER, sr); }
};
