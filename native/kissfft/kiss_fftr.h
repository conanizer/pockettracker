/*
 *  Copyright (c) 2003-2010, Mark Borgerding. All rights reserved.
 *  This file is part of KISS FFT - https://github.com/mborgerding/kissfft
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *  See licenses/THIRD-PARTY-NOTICES.md for the full licence text.
 */

#ifndef KISS_FFTR_H
#define KISS_FFTR_H

#include "kiss_fft.h"

#ifdef __cplusplus
extern "C" {
#endif

/*  Real-input FFT.
    Operates on nfft real samples and outputs nfft/2+1 complex frequency bins.
    nfft must be a power of 2.
    inverse_fft: 0 = real→complex, 1 = complex→real (inverse). */
typedef struct kiss_fftr_state* kiss_fftr_cfg;

kiss_fftr_cfg kiss_fftr_alloc(int nfft, int inverse_fft, void* mem, size_t* lenmem);

/*  Forward real FFT: timedata[nfft] → freqdata[nfft/2+1]. */
void kiss_fftr(kiss_fftr_cfg cfg, const kiss_fft_scalar* timedata, kiss_fft_cpx* freqdata);

#define kiss_fftr_free free

#ifdef __cplusplus
}
#endif

#endif /* KISS_FFTR_H */
