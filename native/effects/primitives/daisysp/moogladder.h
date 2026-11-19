/*
Copyright (c) 2023 Electrosmith, Corp, Victor Lazzarini, John ffitch (fast tanh), Bob Moog

Use of this source code is governed by the LGPL V2.1
license that can be found in the LICENSE file or at
https://opensource.org/license/lgpl-2-1/

Changed for PocketTracker, 2026-10-24, from DaisySP-LGPL c89d380 Filters/moogladder.{h,cpp}: one header;
the cutoff and resonance coefficients are computed by `Coeffs()` and passed to `Process`, so a caller can
interpolate them per sample. 2026-10-25: the second oversampled step takes the input again, as Csound's
original does (it took the third stage's output); the resonance no longer cuts the bass — the input
enters at (1 + res4), so the passband stays at unity and the peak keeps its height over it. 2026-10-28:
`my_tanh` is a continuous rational curve (it jumped at 0.5, which crackled on loud input).
*/
#pragma once
#include <cmath>

namespace daisysp
{
/** The Csound moogladder: four saturating stages, 2× oversampled, with the passband held at unity. */
class MoogLadder
{
  public:
    struct Coeffs
    {
        float tune = 0.0f, res4 = 0.0f;
    };

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        for(int i = 0; i < 6; i++)
            delay_[i] = 0.0f;
        for(int i = 0; i < 3; i++)
            tanhstg_[i] = 0.0f;
    }

    /** `freq` in Hz, `res` 0..1. */
    Coeffs MakeCoeffs(float freq, float res) const
    {
        if(res < 0)
            res = 0;
        const float fc  = freq / sample_rate_;
        const float f   = 0.5f * fc;
        const float fc2 = fc * fc;
        const float fc3 = fc2 * fc2;
        const float fcr = 1.8730f * fc3 + 0.4955f * fc2 - 0.6490f * fc + 0.9988f;
        const float acr = -3.9364f * fc2 + 1.8409f * fc + 0.9968f;
        Coeffs c;
        c.tune = (1.0f - expf(-((2 * 3.14159265358979f) * f * fcr))) / THERMAL;
        c.res4 = 4.0f * res * acr;
        return c;
    }

    /** ⚠️ Its stages saturate near ±1/THERMAL (Csound's 16-bit scale): a ±1 signal must be scaled up. */
    float Process(float in, const Coeffs& c)
    {
        float* delay   = delay_;
        float* tanhstg = tanhstg_;
        float  stg[4];
        const float drive = in * (1.0f + c.res4);
        for(int j = 0; j < 2; j++)
        {
            float x  = drive - c.res4 * delay[5];
            delay[0] = stg[0]
                = delay[0] + c.tune * (my_tanh(x * THERMAL) - tanhstg[0]);
            for(int k = 1; k < 4; k++)
            {
                x      = stg[k - 1];
                stg[k] = delay[k]
                         + c.tune
                               * ((tanhstg[k - 1] = my_tanh(x * THERMAL))
                                  - (k != 3 ? tanhstg[k]
                                            : my_tanh(delay[k] * THERMAL)));
                delay[k] = stg[k];
            }
            delay[5] = (stg[3] + delay[4]) * 0.5f;
            delay[4] = stg[3];
        }
        return delay[5];
    }

    static constexpr float THERMAL = 0.000025f;

  private:
    float delay_[6] = {}, tanhstg_[3] = {}, sample_rate_ = 48000.0f;

    // tanh to within ~2 %, reaching ±1 at ±3 with zero slope. ⚠️ Must stay continuous: a step in
    // the curve is a click on every crossing, heard as crackle on a loud kick.
    static float my_tanh(float x)
    {
        if(x > 3.0f)
            return 1.0f;
        if(x < -3.0f)
            return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }
};
} // namespace daisysp
