#include "kiss_fftr.h"
#include "_kiss_fft_guts.h"
#include <stdlib.h>
#include <math.h>

struct kiss_fftr_state {
    kiss_fft_cfg  substate;         /* N/2-point complex FFT config */
    kiss_fft_cpx *tmpbuf;           /* N/2 complex working buffer   */
    kiss_fft_cpx *super_twiddles;   /* N/4+1 twiddle factors        */
    int           ncfft;            /* = nfft/2                     */
};

kiss_fftr_cfg kiss_fftr_alloc(int nfft, int inverse_fft, void* mem, size_t* lenmem) {
    if (nfft < 2 || (nfft & (nfft - 1)) != 0) return NULL; /* require power-of-2 >= 2 */

    int ncfft = nfft >> 1;

    /* Two-pass sizing: first get the substate size. */
    size_t substate_size = 0;
    kiss_fft_alloc(ncfft, inverse_fft, NULL, &substate_size);

    size_t st_size  = sizeof(struct kiss_fftr_state);
    size_t tw_size  = sizeof(kiss_fft_cpx) * (size_t)(ncfft / 2 + 1);
    size_t tmp_size = sizeof(kiss_fft_cpx) * (size_t)ncfft;

    /* Pad substate to kiss_fft_cpx alignment */
    size_t sub_pad   = (substate_size + sizeof(kiss_fft_cpx) - 1) & ~(sizeof(kiss_fft_cpx) - 1);
    size_t memneeded = st_size + sub_pad + tw_size + tmp_size;

    if (lenmem != NULL) {
        if (mem == NULL || *lenmem < memneeded) {
            *lenmem = memneeded;
            return NULL;
        }
    }

    char* base = (char*)(mem ? mem : malloc(memneeded));
    if (!base) return NULL;

    kiss_fftr_cfg st      = (kiss_fftr_cfg)base;
    char* sub_buf         = base + st_size;
    char* tw_buf          = sub_buf + sub_pad;
    char* tmp_buf         = tw_buf  + tw_size;

    st->ncfft          = ncfft;
    st->super_twiddles = (kiss_fft_cpx*)tw_buf;
    st->tmpbuf         = (kiss_fft_cpx*)tmp_buf;
    st->substate       = kiss_fft_alloc(ncfft, inverse_fft, sub_buf, NULL);

    /* Super twiddles: W_N^k = exp(-j * 2*pi*k / N) = exp(-j * pi*k / ncfft).
       Precomputed for k = 0 .. ncfft/2 (used in post-processing). */
    double sign = inverse_fft ? 1.0 : -1.0;
    for (int k = 0; k <= ncfft / 2; k++) {
        double phase = sign * M_PI * k / ncfft;
        st->super_twiddles[k].r = (kiss_fft_scalar)cos(phase);
        st->super_twiddles[k].i = (kiss_fft_scalar)sin(phase);
    }

    return st;
}

/* Forward real FFT: N real inputs → N/2+1 complex frequency bins.
 *
 * Algorithm (even-odd split):
 *   1. Pack x[2k] + j*x[2k+1] into N/2 complex values.
 *   2. Run N/2-point complex FFT → Z[0..N/2-1].
 *   3. Post-process to recover X[0..N/2]:
 *        DC:      X[0]      = Z[0].r + Z[0].i   (always real)
 *        Nyquist: X[N/2]    = Z[0].r - Z[0].i   (always real)
 *        General (k=1..N/4): let
 *          fpk   = Z[k]
 *          fpnk  = conj(Z[N/2-k])
 *          A     = fpk + fpnk               (even part × 2)
 *          B     = fpk - fpnk               (odd part × 2)
 *          C     = W_N^k * B                (modulate odd part)
 *          X[k]        = (A.r + C.i)/2 + j*(A.i - C.r)/2
 *          X[N/2-k]    = conj(X[k])
 */
void kiss_fftr(kiss_fftr_cfg st, const kiss_fft_scalar* timedata, kiss_fft_cpx* freqdata) {
    const int ncfft = st->ncfft;

    /* Pack N real samples as N/2 complex: z[k] = x[2k] + j*x[2k+1]. */
    for (int k = 0; k < ncfft; k++) {
        st->tmpbuf[k].r = timedata[2 * k];
        st->tmpbuf[k].i = timedata[2 * k + 1];
    }

    /* N/2-point complex forward FFT → freqdata[0..ncfft-1]. */
    kiss_fft(st->substate, st->tmpbuf, freqdata);

    /* DC and Nyquist bins (both are real for real input). */
    float dc = freqdata[0].r;
    float ny = freqdata[0].i;
    freqdata[0].r    = dc + ny;   freqdata[0].i    = 0.0f;
    freqdata[ncfft].r = dc - ny;   freqdata[ncfft].i = 0.0f;

    /* General bins k = 1 .. ncfft/2 (symmetric pairs). */
    for (int k = 1; k <= ncfft / 2; k++) {
        kiss_fft_cpx fpk     = freqdata[k];
        kiss_fft_cpx fpnk_c  = { freqdata[ncfft - k].r, -freqdata[ncfft - k].i }; /* conj */

        kiss_fft_cpx A, B, C;
        A.r = fpk.r + fpnk_c.r;  A.i = fpk.i + fpnk_c.i;
        B.r = fpk.r - fpnk_c.r;  B.i = fpk.i - fpnk_c.i;

        /* C = W_N^k * B where W_N^k = super_twiddles[k]. */
        C_MUL(C, st->super_twiddles[k], B);

        /* X[k] = (A - j*C) / 2.  Note: -(j*C).r = C.i, -(j*C).i = -C.r. */
        freqdata[k].r = HALF_OF(A.r + C.i);
        freqdata[k].i = HALF_OF(A.i - C.r);

        /* X[ncfft-k] = conj(X[k]) by conjugate symmetry of real input. */
        if (k != ncfft - k) {
            freqdata[ncfft - k].r =  freqdata[k].r;
            freqdata[ncfft - k].i = -freqdata[k].i;
        }
    }
}
