#ifndef KISS_FFT_H
#define KISS_FFT_H

#include <stdlib.h>
#include <math.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef kiss_fft_scalar
#define kiss_fft_scalar float
#endif

typedef struct {
    kiss_fft_scalar r;
    kiss_fft_scalar i;
} kiss_fft_cpx;

typedef struct kiss_fft_state* kiss_fft_cfg;

/*  Allocate a forward or inverse FFT config for nfft points.
    nfft must be a power of 2.
    inverse_fft: 0 = forward, 1 = inverse.
    Pass mem=NULL to allocate internally; free with kiss_fft_free().
    Pass mem=NULL and lenmem!=NULL to query required buffer size. */
kiss_fft_cfg kiss_fft_alloc(int nfft, int inverse_fft, void* mem, size_t* lenmem);

/*  Perform the FFT on fin[], writing to fout[].
    fin and fout may alias (in-place). */
void kiss_fft(kiss_fft_cfg cfg, const kiss_fft_cpx* fin, kiss_fft_cpx* fout);

void kiss_fft_stride(kiss_fft_cfg cfg, const kiss_fft_cpx* fin, kiss_fft_cpx* fout, int fin_stride);

#define kiss_fft_free free

#ifdef __cplusplus
}
#endif

#endif /* KISS_FFT_H */
