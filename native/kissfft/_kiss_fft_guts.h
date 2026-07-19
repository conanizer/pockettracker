/*
 *  Copyright (c) 2003-2010, Mark Borgerding. All rights reserved.
 *  This file is part of KISS FFT - https://github.com/mborgerding/kissfft
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *  See licenses/THIRD-PARTY-NOTICES.md for the full licence text.
 */

#ifndef KISS_FFT_GUTS_H
#define KISS_FFT_GUTS_H

#include "kiss_fft.h"
#include <stddef.h>

#define MAXFACTORS 32

struct kiss_fft_state {
    int nfft;
    int inverse;
    int log2n;                 /* log2(nfft), used for bit-reversal */
    int _pad;                  /* alignment padding */
    kiss_fft_cpx twiddles[1]; /* variable-length: twiddles[nfft] */
};

#define C_MUL(m, a, b) \
    do { (m).r = (a).r*(b).r - (a).i*(b).i; \
         (m).i = (a).r*(b).i + (a).i*(b).r; } while (0)

#define C_ADD(res, a, b) \
    do { (res).r = (a).r + (b).r; (res).i = (a).i + (b).i; } while (0)

#define C_SUB(res, a, b) \
    do { (res).r = (a).r - (b).r; (res).i = (a).i - (b).i; } while (0)

#define HALF_OF(x) ((x) * 0.5f)

#endif /* KISS_FFT_GUTS_H */
