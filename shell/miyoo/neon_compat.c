/*
 * libneonarmmiyoo.so — PocketTracker's own build of the 25 symbols the bundled SDL2 fork
 * resolves at load time.
 *
 * ⚠️ THE FILE NAME IS FIXED BY THE BINARY WE SHIP. `libSDL2-2.0.so.0` from XK9274/sdl2_miyoo
 * carries `DT_NEEDED libneonarmmiyoo.so` and 25 undefined symbols — `neon_memcpy` plus 24
 * integer scalers — so the loader stops the app dead if a file of that name does not define
 * every one of them. This is not the upstream library of that name: nothing from
 * XK9274/neon-arm-library-miyoo is in it. That repository carries no licence and its README
 * says the maintainer did not write the source and does not know its origin, so a package
 * built on it cannot be published.
 *
 * ⚠️ Plain C, no NEON, and that costs nothing here. `neon_memcpy` forwards to libc, whose
 * armhf `memcpy` is already the unrolled `vld1.8/vst1.8` ladder. The scalers are dead weight
 * on this app: the fork only calls them from MMIYOO_TryIntegerScaleCopy, which returns early
 * when the source and destination rectangles are the same size, and PocketTracker draws one
 * 640x480 texture onto a 640x480 panel. They are implemented rather than stubbed so that a
 * future window size that is not 1:1 scales instead of drawing nothing.
 *
 * The scaler contract, read off the fork's call site in src/render/mmiyoo/SDL_render_mmiyoo.c:
 * replicate each source pixel `xmul` across and `ymul` down into `dst`; `sw`/`sh` are the
 * source size in pixels, `sp`/`dp` the two strides in BYTES, and a stride of 0 means "tightly
 * packed". The destination gets sw*xmul by sh*ymul pixels; whatever padding `dp` leaves at the
 * end of a row is not written.
 */
#include <stdint.h>
#include <string.h>

void *neon_memcpy(void *dst, const void *src, size_t n);

void *neon_memcpy(void *dst, const void *src, size_t n)
{
    return memcpy(dst, src, n);
}

static void scale_16(const uint8_t *src, uint8_t *dst,
                     uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dp,
                     uint32_t xmul, uint32_t ymul)
{
    if (!sw || !sh) {
        return;
    }
    if (!sp) { sp = sw * 2u; }
    if (!dp) { dp = sw * xmul * 2u; }

    for (uint32_t y = 0; y < sh; ++y) {
        const uint16_t *s = (const uint16_t *)(src + (size_t)y * sp);
        uint8_t *row = dst + (size_t)y * ymul * dp;
        size_t row_bytes = (size_t)sw * xmul * 2u;

        if (xmul == 1u) {
            memcpy(row, s, row_bytes);
        } else {
            uint16_t *d = (uint16_t *)row;
            for (uint32_t x = 0; x < sw; ++x) {
                uint16_t p = s[x];
                for (uint32_t i = 0; i < xmul; ++i) { *d++ = p; }
            }
        }
        for (uint32_t r = 1; r < ymul; ++r) {
            memcpy(row + (size_t)r * dp, row, row_bytes);
        }
    }
}

static void scale_32(const uint8_t *src, uint8_t *dst,
                     uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dp,
                     uint32_t xmul, uint32_t ymul)
{
    if (!sw || !sh) {
        return;
    }
    if (!sp) { sp = sw * 4u; }
    if (!dp) { dp = sw * xmul * 4u; }

    for (uint32_t y = 0; y < sh; ++y) {
        const uint32_t *s = (const uint32_t *)(src + (size_t)y * sp);
        uint8_t *row = dst + (size_t)y * ymul * dp;
        size_t row_bytes = (size_t)sw * xmul * 4u;

        if (xmul == 1u) {
            memcpy(row, s, row_bytes);
        } else {
            uint32_t *d = (uint32_t *)row;
            for (uint32_t x = 0; x < sw; ++x) {
                uint32_t p = s[x];
                for (uint32_t i = 0; i < xmul; ++i) { *d++ = p; }
            }
        }
        for (uint32_t r = 1; r < ymul; ++r) {
            memcpy(row + (size_t)r * dp, row, row_bytes);
        }
    }
}

/*
 * The 24 exported names. Generated, so a grep for `scale2x2_n16` lands here and nowhere else —
 * the guard against a name going missing is check 9 in shell/build-miyoo.sh, which reads the
 * required list out of the shipped libSDL2 rather than out of this file.
 */
#define PT_SCALER(XMUL, YMUL)                                                          \
    void scale##XMUL##x##YMUL##_n16(void *src, void *dst, uint32_t sw, uint32_t sh,    \
                                    uint32_t sp, uint32_t dp)                          \
    {                                                                                  \
        scale_16((const uint8_t *)src, (uint8_t *)dst, sw, sh, sp, dp, XMUL, YMUL);    \
    }                                                                                  \
    void scale##XMUL##x##YMUL##_n32(void *src, void *dst, uint32_t sw, uint32_t sh,    \
                                    uint32_t sp, uint32_t dp)                          \
    {                                                                                  \
        scale_32((const uint8_t *)src, (uint8_t *)dst, sw, sh, sp, dp, XMUL, YMUL);    \
    }

PT_SCALER(1, 1)
PT_SCALER(1, 2)
PT_SCALER(1, 3)
PT_SCALER(1, 4)
PT_SCALER(2, 1)
PT_SCALER(2, 2)
PT_SCALER(2, 3)
PT_SCALER(2, 4)
PT_SCALER(4, 1)
PT_SCALER(4, 2)
PT_SCALER(4, 3)
PT_SCALER(4, 4)

#undef PT_SCALER
