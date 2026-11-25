/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 NV16 (4:2:2) 4 MiB Tagged YCbCr Cache Scalar Row Kernel.
 * Adapted from Fenestra-Z8-C311-source/modules/jpeg_lut/cube_frame.c.
 * Strictly scalar (-mgeneral-regs-only), zero FP, zero SIMD, zero .data/.bss.
 */
#include <stddef.h>
#include "cube_frame.h"

/* 20-bit cache index = ((cr & 63) << 14) | ((cb & 63) << 8) | y.
 * 4-bit nonzero tag (1..16) in bits 24..31 distinguishes the 16 chroma quadrants. */
static inline uint32_t tag_for(unsigned cb, unsigned cr)
{
    return 1u + (cb >> 6) + ((cr >> 6) << 2);
}

_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0),
               "cache words must be single-copy atomic");

void jpeg_cube_cache_warm(const JpegCube *cube, uint32_t *cache, uint32_t first, uint32_t end)
{
    if (!cube || !cache) {
        return;
    }
    if (end > CUBE_FRAME_CACHE_WORDS) {
        end = CUBE_FRAME_CACHE_WORDS;
    }
    while (first < end) {
        uint32_t group = first >> 8;
        uint32_t next = (group + 1u) << 8;
        if (next > end) {
            next = end;
        }
        /* For each 6-bit chroma index, pre-warm central chroma in 96..159. */
        unsigned cb = 96u + ((group + 32u) & 63u);
        unsigned cr = 96u + (((group >> 6) + 32u) & 63u);
        uint32_t tag = tag_for(cb, cr) << 24;
        JpegCubeChroma offset = jpeg_cube_chroma(cb, cr);
        uint32_t *page = cache + (group << 8);
        for (unsigned y = first & 255u; y < next - (group << 8); ++y) {
            __atomic_store_n(page + y, tag | jpeg_cube_sample(cube, y, offset), __ATOMIC_RELAXED);
        }
        first = next;
    }
}

static __attribute__((noinline)) uint64_t fill_pair(const JpegCube *cube, uint32_t *page,
                                                    uint32_t ys, uint32_t uv,
                                                    uint32_t tag, uint32_t a, uint32_t b)
{
    unsigned y0 = ys & 255u;
    unsigned y1 = ys >> 8;
    JpegCubeChroma offset = jpeg_cube_chroma(uv & 255u, uv >> 8);
    if ((a >> 24) != tag) {
        a = (tag << 24) | jpeg_cube_sample(cube, y0, offset);
        __atomic_store_n(page + y0, a, __ATOMIC_RELAXED);
    }
    if (y1 == y0) {
        b = a;
    } else if ((b >> 24) != tag) {
        b = (tag << 24) | jpeg_cube_sample(cube, y1, offset);
        __atomic_store_n(page + y1, b, __ATOMIC_RELAXED);
    }
    return (uint64_t)a | ((uint64_t)b << 32);
}

static inline uint32_t load_pair(const uint8_t *p, int aligned)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (aligned) {
        uint16_t value;
        __builtin_memcpy(&value, __builtin_assume_aligned(p, 2), sizeof(value));
        return value;
    }
#else
    (void)aligned;
#endif
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static inline void store_pair(uint8_t *p, uint32_t value, int aligned)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (aligned) {
        uint16_t pair = (uint16_t)value;
        __builtin_memcpy(__builtin_assume_aligned(p, 2), &pair, sizeof(pair));
        return;
    }
#else
    (void)aligned;
#endif
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static __attribute__((always_inline)) inline uint32_t render_row(
    const JpegCube *cube, uint32_t *cache,
    const uint8_t *sy, const uint8_t *sc,
    uint8_t *dy, uint8_t *dc, uint32_t width, int aligned)
{
    const uint8_t *end = sy + (width & ~1u);
    uint32_t misses = 0u;
    while (sy != end) {
        uint32_t uv = load_pair(sc, aligned);
        uint32_t ys = load_pair(sy, aligned);
        sy += 2;
        sc += 2;
        unsigned cb = uv & 255u;
        unsigned cr = uv >> 8;
        uint32_t *page = cache + (((cr & 63u) << 14) | ((cb & 63u) << 8));
        uint32_t tag = tag_for(cb, cr);
        uint32_t a = __atomic_load_n(page + (ys & 255u), __ATOMIC_RELAXED);
        uint32_t b = __atomic_load_n(page + (ys >> 8), __ATOMIC_RELAXED);
        if (__builtin_expect((a >> 24) != tag || (b >> 24) != tag, 0)) {
            misses += (unsigned)((a >> 24) != tag);
            misses += (unsigned)((ys >> 8) != (ys & 255u) && (b >> 24) != tag);
            uint64_t pair = fill_pair(cube, page, ys, uv, tag, a, b);
            a = (uint32_t)pair;
            b = (uint32_t)(pair >> 32);
        }
        store_pair(dy, (a & 255u) | ((b & 255u) << 8), aligned);
        /* Rounded byte-wise average matching NEON vrhadd_u8 bit-for-bit. */
        uint32_t chroma = (a | b) - (((a ^ b) >> 1) & 0x007f7f00u);
        store_pair(dc, chroma >> 8, aligned);
        dy += 2;
        dc += 2;
    }
    return misses;
}

__attribute__((noinline)) uint32_t jpeg_cube_cache_row_scalar(
    const JpegCube *cube, uint32_t *cache,
    const uint8_t *sy, const uint8_t *sc,
    uint8_t *dy, uint8_t *dc, uint32_t width)
{
    if (!cube || !cache || !sy || !sc || !dy || !dc || width < 2u) {
        return 0u;
    }
    if (((uintptr_t)sy | (uintptr_t)sc | (uintptr_t)dy | (uintptr_t)dc) & 1u) {
        return render_row(cube, cache, sy, sc, dy, dc, width, 0);
    }
    return render_row(cube, cache, sy, sc, dy, dc, width, 1);
}

__attribute__((noinline)) void jpeg_cube_cache_row(
    const JpegCube *cube, uint32_t *cache,
    const uint8_t *sy, const uint8_t *sc,
    uint8_t *dy, uint8_t *dc, uint32_t width)
{
    (void)jpeg_cube_cache_row_scalar(cube, cache, sy, sc, dy, dc, width);
}
