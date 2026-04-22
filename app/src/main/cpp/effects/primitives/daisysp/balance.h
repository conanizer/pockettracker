/*
Copyright (c) 2023 Electrosmith, Corp, Barry Vercoe, john ffitch, Gabriel Maldonado

Use of this source code is governed by the LGPL V2.1
license that can be found in the LICENSE file or at
https://opensource.org/license/lgpl-2-1/
*/

#pragma once
#ifndef DSY_BALANCE_H
#define DSY_BALANCE_H

#include <stdint.h>
#ifdef __cplusplus

namespace daisysp
{
/** Balances two sound sources. Sig is boosted to the level of comp. */
class Balance
{
  public:
    Balance() {}
    ~Balance() {}
    /** Initializes the balance module.
        \param sample_rate - The sample rate of the audio engine being run. 
    */
    void Init(float sample_rate);


    /** adjust sig level to level of comp
    */
    float Process(float sig, float comp);


    /** adjusts the rate at which level compensation happens
        \param cutoff : Sets half power point of special internal cutoff filter.

        defaults to 10
    */
    inline void SetCutoff(float cutoff) { ihp_ = cutoff; }

  private:
    float sample_rate_, ihp_, c2_, c1_, prvq_, prvr_, prva_;
};
} // namespace daisysp
#endif
#endif
