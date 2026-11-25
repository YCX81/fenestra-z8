/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 NV16 (4:2:2) 4 MiB Tagged YCbCr Cache AArch64 NEON Kernel + NEON Sentinel Probe.
 * Adapted from Fenestra-Z8-C311-source/modules/jpeg_lut/cube_frame_neon.c.
 *
 * ONE OF ONLY TWO NEON-ENABLED COMPILATION UNITS in work/lut_core/
 * (the other is layer0_color_neon.c). Gated by v71r7_neon_sentinel_is_allowed().
 * Zero .data/.bss.
 */
#include <stddef.h>
#include <stdint.h>
#include "cube_frame.h"
#include "neon_sentinel.h"

#if defined(__aarch64__)
#include <arm_neon.h>

static inline uint8x8_t load_bytes(const uint8_t *p)
{
#ifdef V71R7_HOST_TEST
    return vld1_u8(p);
#else
    uint8x8_t value;
    __asm__("ld1 {%0.8b}, [%1]" : "=w"(value) : "r"(p), "m"(*(const uint8_t (*)[8])p));
    return value;
#endif
}

static inline void store_bytes(uint8_t *p, uint8x8_t value)
{
#ifdef V71R7_HOST_TEST
    vst1_u8(p, value);
#else
    __asm__("st1 {%1.8b}, [%2]" : "=m"(*(uint8_t (*)[8])p) : "w"(value), "r"(p));
#endif
}

/* AArch64 NEON has no SIMD gather: perform 4 single-copy atomic 32-bit loads. */
static inline uint32x4_t gather_u32(const uint32_t *cache, uint32x4_t index)
{
    uint32x4_t p = vdupq_n_u32(__atomic_load_n(cache + vgetq_lane_u32(index, 0), __ATOMIC_RELAXED));
    p = vsetq_lane_u32(__atomic_load_n(cache + vgetq_lane_u32(index, 1), __ATOMIC_RELAXED), p, 1);
    p = vsetq_lane_u32(__atomic_load_n(cache + vgetq_lane_u32(index, 2), __ATOMIC_RELAXED), p, 2);
    return vsetq_lane_u32(__atomic_load_n(cache + vgetq_lane_u32(index, 3), __ATOMIC_RELAXED), p, 3);
}

uint32_t jpeg_cube_cache_row_neon(const JpegCube *cube, uint32_t *cache,
                                  const uint8_t *sy, const uint8_t *sc,
                                  uint8_t *dy, uint8_t *dc, uint32_t width)
{
    if (!cube || !cache || !sy || !sc || !dy || !dc || width < 2u) {
        return 0u;
    }
    static const uint8_t k_idx_tags[8]  = { 3u, 7u, 11u, 15u, 19u, 23u, 27u, 31u };
    static const uint8_t k_idx_luma[8]  = { 0u, 4u,  8u, 12u, 16u, 20u, 24u, 28u };
    static const uint8_t k_idx_cbcr0[8] = { 1u, 2u,  9u, 10u, 17u, 18u, 25u, 26u };
    static const uint8_t k_idx_cbcr1[8] = { 5u, 6u, 13u, 14u, 21u, 22u, 29u, 30u };
    const uint8x8_t v_idx_tags  = vld1_u8(k_idx_tags);
    const uint8x8_t v_idx_luma  = vld1_u8(k_idx_luma);
    const uint8x8_t v_idx_cbcr0 = vld1_u8(k_idx_cbcr0);
    const uint8x8_t v_idx_cbcr1 = vld1_u8(k_idx_cbcr1);

    uint32_t x = 0u, misses = 0u;
    uint32_t bulk = width & ~7u;
    for (; x < bulk; x += 8u) {
        if ((x & 63u) == 0u && width - x > 128u) {
            __builtin_prefetch(sy + x + 128u);
            __builtin_prefetch(sc + x + 128u);
        }
        uint16x8_t y = vmovl_u8(load_bytes(sy + x));
        uint16x4_t uv = vreinterpret_u16_u8(load_bytes(sc + x));
        uint16x4_t group = vorr_u16(vand_u16(uv, vdup_n_u16(63u)),
                                    vand_u16(vshr_n_u16(uv, 2), vdup_n_u16(63u << 6)));
        uint16x4x2_t pair = vzip_u16(group, group);
        uint32x4_t lo = gather_u32(cache, vaddw_u16(vshll_n_u16(pair.val[0], 8), vget_low_u16(y)));
        uint32x4_t hi = gather_u32(cache, vaddw_u16(vshll_n_u16(pair.val[1], 8), vget_high_u16(y)));
        uint16x4_t tag = vadd_u16(
            vorr_u16(vand_u16(vshr_n_u16(uv, 6), vdup_n_u16(3u)),
                     vand_u16(vshr_n_u16(uv, 12), vdup_n_u16(12u))),
            vdup_n_u16(1u));
        uint8x16x2_t entries = {{ vreinterpretq_u8_u32(lo), vreinterpretq_u8_u32(hi) }};
        uint8x8_t tags = vqtbl2_u8(entries, v_idx_tags);
        uint8x8_t tag_bytes = vreinterpret_u8_u16(vsli_n_u16(tag, tag, 8));
        uint8x8_t error = veor_u8(tag_bytes, tags);
        if (__builtin_expect(vget_lane_u64(vreinterpret_u64_u8(error), 0) != 0u, 0)) {
            misses += jpeg_cube_cache_row_scalar(cube, cache, sy + x, sc + x, dy + x, dc + x, 8u);
            continue;
        }
        uint8x8_t luma  = vqtbl2_u8(entries, v_idx_luma);
        uint8x8_t cbcr0 = vqtbl2_u8(entries, v_idx_cbcr0);
        uint8x8_t cbcr1 = vqtbl2_u8(entries, v_idx_cbcr1);
        store_bytes(dy + x, luma);
        store_bytes(dc + x, vrhadd_u8(cbcr0, cbcr1));
    }
    if (x < width) {
        misses += jpeg_cube_cache_row_scalar(cube, cache, sy + x, sc + x, dy + x, dc + x, width - x);
    }
    return misses;
}

#else /* Non-AArch64 fallback */

uint32_t jpeg_cube_cache_row_neon(const JpegCube *cube, uint32_t *cache,
                                  const uint8_t *sy, const uint8_t *sc,
                                  uint8_t *dy, uint8_t *dc, uint32_t width)
{
    return jpeg_cube_cache_row_scalar(cube, cache, sy, sc, dy, dc, width);
}

#endif /* __aarch64__ */

/* ---- Runtime NEON Sentinel Hardware Probe (lives ONLY in this NEON TU!) ---- */

#if defined(__aarch64__) && !defined(V71R7_HOST_TEST) && !defined(__APPLE__)

V71R7_DEFINE_NEON_SENTINEL_ASM(v71r7_neon_sentinel_hw_probe)

#elif defined(__aarch64__) && defined(V71R7_HOST_TEST)

/* Userspace Apple Silicon / AArch64 NEON canary self-test (verifies real d8..d15
 * across delay() call and real v0..v31 register file preservation). */
unsigned v71r7_neon_sentinel_hw_probe(int32_t (*delay)(uint32_t))
{
    uint64_t expected[64];
    uint64_t observed[64];
    for (uint64_t i = 0u; i < 64u; ++i) {
        expected[i] = UINT64_C(0x5eb19c73d284a60f) ^ (i * UINT64_C(0x9e3779b97f4a7c15));
        observed[i] = 0u;
    }

    /* Phase 1: check PCS callee-saved d8..d15 across delay(1) */
    uint64_t saved_d[8];
    int32_t drc = 0;
    __asm__ volatile(
        "stp d8,  d9,  [%[sav], #0]\n"
        "stp d10, d11, [%[sav], #16]\n"
        "stp d12, d13, [%[sav], #32]\n"
        "stp d14, d15, [%[sav], #48]\n"
        "ldp d8,  d9,  [%[exp], #0]\n"
        "ldp d10, d11, [%[exp], #16]\n"
        "ldp d12, d13, [%[exp], #32]\n"
        "ldp d14, d15, [%[exp], #48]\n"
        "mov w0, #1\n"
        "blr %[fn]\n"
        "mov %w[rc], w0\n"
        "stp d8,  d9,  [%[obs], #0]\n"
        "stp d10, d11, [%[obs], #16]\n"
        "stp d12, d13, [%[obs], #32]\n"
        "stp d14, d15, [%[obs], #48]\n"
        "ldp d8,  d9,  [%[sav], #0]\n"
        "ldp d10, d11, [%[sav], #16]\n"
        "ldp d12, d13, [%[sav], #32]\n"
        "ldp d14, d15, [%[sav], #48]\n"
        : [rc] "=&r"(drc)
        : [sav] "r"(saved_d), [exp] "r"(expected), [obs] "r"(observed), [fn] "r"(delay)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
          "x12", "x13", "x14", "x15", "x16", "x17", "x30",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "cc", "memory"
    );
    if (drc != 0) {
        return V71R7_NEON_FAILED;
    }
    for (unsigned i = 0u; i < 8u; ++i) {
        if (observed[i] != expected[i]) {
            return V71R7_NEON_FAILED;
        }
    }

    /* Phase 2: load all v0..v31 (64 x 64-bit lanes), wait briefly, verify all 64 lanes */
    __asm__ volatile(
        "stp d8,  d9,  [%[sav], #0]\n"
        "stp d10, d11, [%[sav], #16]\n"
        "stp d12, d13, [%[sav], #32]\n"
        "stp d14, d15, [%[sav], #48]\n"
        "mov x9, %[exp]\n"
        "ld1 {v0.16b-v3.16b},   [x9], #64\n"
        "ld1 {v4.16b-v7.16b},   [x9], #64\n"
        "ld1 {v8.16b-v11.16b},  [x9], #64\n"
        "ld1 {v12.16b-v15.16b}, [x9], #64\n"
        "ld1 {v16.16b-v19.16b}, [x9], #64\n"
        "ld1 {v20.16b-v23.16b}, [x9], #64\n"
        "ld1 {v24.16b-v27.16b}, [x9], #64\n"
        "ld1 {v28.16b-v31.16b}, [x9], #64\n"
        "mov w10, #1000\n"
        "1: yield\n"
        "subs w10, w10, #1\n"
        "b.ne 1b\n"
        "mov x9, %[obs]\n"
        "st1 {v0.16b-v3.16b},   [x9], #64\n"
        "st1 {v4.16b-v7.16b},   [x9], #64\n"
        "st1 {v8.16b-v11.16b},  [x9], #64\n"
        "st1 {v12.16b-v15.16b}, [x9], #64\n"
        "st1 {v16.16b-v19.16b}, [x9], #64\n"
        "st1 {v20.16b-v23.16b}, [x9], #64\n"
        "st1 {v24.16b-v27.16b}, [x9], #64\n"
        "st1 {v28.16b-v31.16b}, [x9], #64\n"
        "ldp d8,  d9,  [%[sav], #0]\n"
        "ldp d10, d11, [%[sav], #16]\n"
        "ldp d12, d13, [%[sav], #32]\n"
        "ldp d14, d15, [%[sav], #48]\n"
        :
        : [sav] "r"(saved_d), [exp] "r"(expected), [obs] "r"(observed)
        : "x9", "x10",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "cc", "memory"
    );
    for (unsigned i = 0u; i < 64u; ++i) {
        if (observed[i] != expected[i]) {
            return V71R7_NEON_FAILED;
        }
    }
    return V71R7_NEON_PASS;
}

#else

unsigned v71r7_neon_sentinel_hw_probe(int32_t (*delay)(uint32_t))
{
    return (delay && delay(1u) == 0) ? V71R7_NEON_PASS : V71R7_NEON_FAILED;
}

#endif
