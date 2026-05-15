#include "_kiss_fft_guts.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* Bit-reversal of x using log2n bits. */
static unsigned int bit_reverse(unsigned int x, int log2n) {
    unsigned int result = 0;
    for (int i = 0; i < log2n; i++) {
        result = (result << 1) | (x & 1u);
        x >>= 1;
    }
    return result;
}

kiss_fft_cfg kiss_fft_alloc(int nfft, int inverse_fft, void* mem, size_t* lenmem) {
    /* Require power-of-2 nfft */
    if (nfft <= 0 || (nfft & (nfft - 1)) != 0) return NULL;

    size_t memneeded = offsetof(struct kiss_fft_state, twiddles)
                       + sizeof(kiss_fft_cpx) * (size_t)nfft;

    if (lenmem != NULL) {
        if (mem == NULL || *lenmem < memneeded) {
            *lenmem = memneeded;
            return NULL;
        }
    }

    kiss_fft_cfg st = (kiss_fft_cfg)(mem ? mem : malloc(memneeded));
    if (!st) return NULL;

    st->nfft    = nfft;
    st->inverse = inverse_fft;

    /* Compute log2(nfft) */
    int log2n = 0;
    int tmp = nfft;
    while (tmp > 1) { tmp >>= 1; log2n++; }
    st->log2n = log2n;

    /* Precompute twiddle factors: W_N^k = exp(sign * j * 2*pi*k / N)
       Forward FFT sign = -1; inverse sign = +1. */
    double sign = inverse_fft ? 1.0 : -1.0;
    for (int k = 0; k < nfft; k++) {
        double phase = sign * 2.0 * M_PI * k / nfft;
        st->twiddles[k].r = (kiss_fft_scalar)cos(phase);
        st->twiddles[k].i = (kiss_fft_scalar)sin(phase);
    }

    return st;
}

void kiss_fft_stride(kiss_fft_cfg cfg, const kiss_fft_cpx* fin, kiss_fft_cpx* fout, int fin_stride) {
    const int N     = cfg->nfft;
    const int log2n = cfg->log2n;

    /* Bit-reversal permutation into output buffer. */
    for (int i = 0; i < N; i++) {
        fout[bit_reverse((unsigned)i, log2n)] = fin[i * fin_stride];
    }

    /* Iterative Cooley-Tukey butterfly stages.
       Stage s processes sub-DFTs of length m = 2^s.
       Each butterfly uses twiddle W_N^(j * N/m) = twiddles[j * twiddle_step]. */
    for (int s = 1; s <= log2n; s++) {
        const int m            = 1 << s;
        const int m2           = m >> 1;
        const int twiddle_step = N >> s;   /* = N / m */

        for (int k = 0; k < N; k += m) {
            for (int j = 0; j < m2; j++) {
                const kiss_fft_cpx* tw = &cfg->twiddles[(unsigned)j * twiddle_step];
                kiss_fft_cpx* a = &fout[k + j];
                kiss_fft_cpx* b = &fout[k + j + m2];

                kiss_fft_cpx t;
                C_MUL(t, *tw, *b);

                b->r = a->r - t.r;
                b->i = a->i - t.i;
                a->r = a->r + t.r;
                a->i = a->i + t.i;
            }
        }
    }

    /* Normalize for inverse FFT (unitary convention). */
    if (cfg->inverse) {
        float scale = 1.0f / (float)N;
        for (int i = 0; i < N; i++) {
            fout[i].r *= scale;
            fout[i].i *= scale;
        }
    }
}

void kiss_fft(kiss_fft_cfg cfg, const kiss_fft_cpx* fin, kiss_fft_cpx* fout) {
    kiss_fft_stride(cfg, fin, fout, 1);
}
