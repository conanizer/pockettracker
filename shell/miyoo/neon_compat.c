/*
 * libneonarmmiyoo.so — our own build of the 25 symbols the bundled SDL2 fork (XK9274/sdl2_miyoo)
 * needs at load time: it carries `DT_NEEDED libneonarmmiyoo.so`, so the file name is fixed and
 * every symbol must exist or the app does not start. ⚠️ Nothing from XK9274/neon-arm-library-miyoo
 * is in it — that repository has no licence and no known author, so nothing built on it can ship.
 *
 * Plain C: libc's armhf memcpy is already NEON. The scalers run only when source and destination
 * sizes differ (never, at 640x480 on a 640x480 panel), but are real so a future size still draws.
 * Contract, from the fork's SDL_render_mmiyoo.c: replicate each pixel `xmul` across and `ymul` down;
 * `sw`/`sh` in pixels, `sp`/`dp` strides in bytes (0 = tightly packed); row padding is not written.
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
 * The 24 exported names. shell/build-miyoo.sh's check 9 reads the required list from the shipped
 * libSDL2, so a missing name fails the build.
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
