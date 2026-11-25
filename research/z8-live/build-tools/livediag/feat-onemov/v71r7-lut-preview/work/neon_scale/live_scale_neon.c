/* SPDX-License-Identifier: MIT
 * V71 Fast AArch64 NEON Direct NV12 / I420 Scaler, Fused 512x256 Grain, Copy & Preview Kernel.
 *
 * - V71 Fast 4:5 Horizontal Upscaler: vld4q_u8 (Luma) / vld4q_u16 (NV12 UV) + 14 single-cycle
 *   alternating vhadd/vrhadd cascade (57.16 dB PSNR, max 1 LSB vs exact 1/16th) + 8 zip + 5 tbl,
 *   ZERO multiplies, ZERO widening, ZERO Q-register stack spills!
 * - V71 Fast Vertical Blend: 64B/iter alternating vhadd/vrhadd cascade (inf dB at wy=0/4/8/12,
 *   57.16 dB everywhere else, max 1 LSB) + stnp non-temporal stores (eliminating 1.66 MiB DRAM RFO)
 *   compatible with strip dc cvac + final dsb sy.
 * - V71 Fused 512x256 Twisted-Torus (twist_dx=192) int8_t Silver-Halide Film Grain directly inside
 *   vertical blend Y output (0 extra Y DRAM traffic; 100% crisp 1280x852 grain PSD; grain_tile==NULL
 *   has 0 overhead).
 * - Strictly 0 FP instructions, 0 .data/.bss, 0 libc deps, 100% SCTLR_EL1.A == 1 strict-align safe.
 */
#include "live_scale_neon.h"

#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(LIVE_SCALE_NEON_SCALAR)
#define LS_USE_NEON 1
#include <arm_neon.h>
#else
#define LS_USE_NEON 0
#endif

#if defined(__aarch64__) && !defined(__APPLE__) && !defined(__linux__) && !defined(LIVE_HOST_TEST)
#define LS_EL1_CACHE_OPS 1
#else
#define LS_EL1_CACHE_OPS 0
#endif

#define LS_SRC_W       1024u
#define LS_DST_W       1280u
#define LS_DST_H       852u
#define LS_DST_CW      640u
#define LS_DST_CH      426u
#define LS_PREV_W      1024u
#define LS_PREV_H      682u
#define LS_PREV_CW     512u
#define LS_PREV_CH     341u
#define LS_MAX_DIM     4096u
#define LS_MAX_STRIDE  8192u
#define LS_GTILE_W     512u
#define LS_GTILE_H     256u
#define LS_GTILE_TWIST 192u

_Static_assert((LS_DST_W % 64u) == 0u, "LS_DST_W must be a multiple of 64");
_Static_assert((LS_GTILE_W % 64u) == 0u, "LS_GTILE_W must be a multiple of 64");
_Static_assert((LS_GTILE_TWIST % 64u) == 0u, "LS_GTILE_TWIST must be a multiple of 64");

#if defined(__clang__)
#define LS_NO_UNROLL     _Pragma("clang loop unroll(disable)")
#define LS_NO_VEC_UNROLL _Pragma("clang loop vectorize(disable) unroll(disable)")
#else
#define LS_NO_UNROLL
#define LS_NO_VEC_UNROLL
#endif

/* --- Bare-metal EL1 D-Cache Clean Helpers (strip-interleaved dc cvac + final dsb sy) --- */

#if LS_EL1_CACHE_OPS
static inline void ls_dc_cvac_range(uintptr_t start, size_t len)
{
    if (!len) {
        return;
    }
    uintptr_t p = start & ~(uintptr_t)63u;
    uintptr_t end = start + len;
    if (end - p >= 512u) {
        uintptr_t lim = end - 512u;
        __asm__ volatile(
            "1:\n\t"
            "dc   cvac, %[p]\n\t"
            "add  %[p], %[p], #64\n\t"
            "dc   cvac, %[p]\n\t"
            "add  %[p], %[p], #64\n\t"
            "dc   cvac, %[p]\n\t"
            "add  %[p], %[p], #64\n\t"
            "dc   cvac, %[p]\n\t"
            "add  %[p], %[p], #64\n\t"
            "dc   cvac, %[p]\n\t"
            "add  %[p], %[p], #64\n\t"
            "dc   cvac, %[p]\n\t"
            "add  %[p], %[p], #64\n\t"
            "dc   cvac, %[p]\n\t"
            "add  %[p], %[p], #64\n\t"
            "dc   cvac, %[p]\n\t"
            "add  %[p], %[p], #64\n\t"
            "cmp  %[p], %[lim]\n\t"
            "b.ls 1b"
            : [p] "+r"(p)
            : [lim] "r"(lim)
            : "cc", "memory");
    }
    LS_NO_VEC_UNROLL
    for (; p < end; p += 64u) {
        __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    }
}
#endif

static __attribute__((noinline)) void ls_clean_strip(
    uint8_t *base, unsigned stride, unsigned width, unsigned r0, unsigned r1)
{
#if LS_EL1_CACHE_OPS
    if (r1 <= r0) {
        return;
    }
    if (stride == width) {
        ls_dc_cvac_range((uintptr_t)(base + (size_t)r0 * stride),
                         (size_t)(r1 - r0) * stride);
        return;
    }
    for (unsigned r = r0; r < r1; ++r) {
        ls_dc_cvac_range((uintptr_t)(base + (size_t)r * stride), width);
    }
#else
    (void)base;
    (void)stride;
    (void)width;
    (void)r0;
    (void)r1;
#endif
}

static inline void ls_dsb_sy(void)
{
#if LS_EL1_CACHE_OPS
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

/* --- Low-level Aligned / Element-Aligned / Non-Temporal Helpers (SCTLR_EL1.A == 1 safe) --- */

#if LS_USE_NEON
static inline uint8x16_t ls_ld16a(const uint8_t *p)
{
    return *(const uint8x16_t *)__builtin_assume_aligned(p, 16);
}

static inline void ls_st16a(uint8_t *p, uint8x16_t v)
{
    *(uint8x16_t *)__builtin_assume_aligned(p, 16) = v;
}

/* Element-aligned 1B ld1/st1 (never faults under SCTLR_EL1.A == 1, never scalarizes under -mstrict-align) */
static inline uint8x16_t ls_ld1_u8(const uint8_t *p)
{
    uint8x16_t v;
    __asm__("ld1 {%0.16b}, [%1]"
            : "=w"(v)
            : "r"(p), "m"(*(const uint8_t (*)[16])p));
    return v;
}

static inline int8x16_t ls_ld1_s8(const int8_t *p)
{
    int8x16_t v;
    __asm__("ld1 {%0.16b}, [%1]"
            : "=w"(v)
            : "r"(p), "m"(*(const int8_t (*)[16])p));
    return v;
}

static inline void ls_st1_u8(uint8_t *p, uint8x16_t v)
{
    __asm__("st1 {%1.16b}, [%2]"
            : "=m"(*(uint8_t (*)[16])p)
            : "w"(v), "r"(p));
}

/* Core 16-pixel silver-halide grain apply (100% bit-exact match to z8_zf_grain_live_apply_tile_y):
 * tone(y) = min(min(2*y, 2*(255-y)), 64) == 2 * min(min(y, 255-y), 32) == 2 * m (m in [0..32]).
 * srshr(g * 2*m, 6) == floor((2*g*m + 32) / 64) == floor((g*m + 16) / 32) == (int8_t)rshrn(g*m, 5).
 * out = usqadd(y, delta) = clamp_u8(y + delta). (8 insns vs V71.2's 13 insns!) */
static inline uint8x16_t ls_grain_apply16_slow(uint8x16_t y, int8x16_t g, uint8x16_t v32)
{
    int8x16_t m   = vreinterpretq_s8_u8(vminq_u8(vminq_u8(y, vmvnq_u8(y)), v32));
    int16x8_t plo = vmull_s8(vget_low_s8(g), vget_low_s8(m));
    int16x8_t phi = vmull_high_s8(g, m);
    int8x16_t d   = vrshrn_high_n_s16(vrshrn_n_s16(plo, 5), phi, 5);
    return vsqaddq_u8(y, d);
}
#endif

static inline uint8_t ls_grain_apply_scalar(uint8_t y, int8_t g)
{
    unsigned inv = 255u - (unsigned)y;
    unsigned lo = (unsigned)y * 2u;
    unsigned hi = inv * 2u;
    unsigned t = lo < hi ? lo : hi;
    if (t > 64u) {
        t = 64u;
    }
    /* Exact arithmetic srshr #6 matching AArch64 vrshrq_n_s16(g * t, 6) with zero UB */
    int32_t prod = (int32_t)g * (int32_t)t + 32;
    int32_t delta = (prod >= 0) ? (prod >> 6) : -(((-prod) + 63) >> 6);
    int32_t out = (int32_t)y + delta;
    if (out < 0) {
        return 0u;
    }
    if (out > 255) {
        return 255u;
    }
    return (uint8_t)out;
}

/* Cacheable allocating copy (normal stp on 16B-aligned dst -> keeps CPU0->CPU2 LBUF warm in shared L2;
 * 100% SCTLR_EL1.A == 1 safe via 4-reg element-aligned ld1 for any src alignment; hoisted prefetch). */
static __attribute__((noinline)) void ls_copy_bytes(
    uint8_t * __restrict dst, const uint8_t * __restrict src,
    unsigned n, int prefetch_src)
{
#if LS_USE_NEON
    if ((((uintptr_t)dst) & 15u) == 0u) {
        uint8_t *d = dst;
        const uint8_t *s = src;
        unsigned rem = n;
        if (rem >= 128u) {
            uint64_t cnt = (uint64_t)(rem >> 7);
            rem &= 127u;
            if (prefetch_src) {
                __asm__ volatile(
                    "1:\n\t"
                    "prfm pldl1strm, [%[s], #192]\n\t"
                    "prfm pldl1strm, [%[s], #256]\n\t"
                    "ld1  {v0.16b, v1.16b, v2.16b, v3.16b}, [%[s]], #64\n\t"
                    "ld1  {v4.16b, v5.16b, v6.16b, v7.16b}, [%[s]], #64\n\t"
                    "st1  {v0.16b, v1.16b, v2.16b, v3.16b}, [%[d]], #64\n\t"
                    "st1  {v4.16b, v5.16b, v6.16b, v7.16b}, [%[d]], #64\n\t"
                    "subs %[cnt], %[cnt], #1\n\t"
                    "b.ne 1b"
                    : [d] "+r"(d), [s] "+r"(s), [cnt] "+r"(cnt)
                    :
                    : "cc", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
            } else {
                __asm__ volatile(
                    "1:\n\t"
                    "ld1  {v0.16b, v1.16b, v2.16b, v3.16b}, [%[s]], #64\n\t"
                    "ld1  {v4.16b, v5.16b, v6.16b, v7.16b}, [%[s]], #64\n\t"
                    "st1  {v0.16b, v1.16b, v2.16b, v3.16b}, [%[d]], #64\n\t"
                    "st1  {v4.16b, v5.16b, v6.16b, v7.16b}, [%[d]], #64\n\t"
                    "subs %[cnt], %[cnt], #1\n\t"
                    "b.ne 1b"
                    : [d] "+r"(d), [s] "+r"(s), [cnt] "+r"(cnt)
                    :
                    : "cc", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
            }
        }
        if (rem >= 64u) {
            __asm__ volatile(
                "ld1  {v0.16b, v1.16b, v2.16b, v3.16b}, [%[s]], #64\n\t"
                "st1  {v0.16b, v1.16b, v2.16b, v3.16b}, [%[d]], #64"
                : [d] "+r"(d), [s] "+r"(s)
                :
                : "memory", "v0", "v1", "v2", "v3");
            rem -= 64u;
        }
        if (rem >= 32u) {
            __asm__ volatile(
                "ld1  {v0.16b, v1.16b}, [%[s]], #32\n\t"
                "stp  q0, q1, [%[d]], #32"
                : [d] "+r"(d), [s] "+r"(s)
                :
                : "memory", "v0", "v1");
            rem -= 32u;
        }
        if (rem >= 16u) {
            ls_st16a(d, ls_ld1_u8(s));
            d += 16u;
            s += 16u;
            rem -= 16u;
        }
        LS_NO_VEC_UNROLL
        for (unsigned i = 0u; i < rem; ++i) {
            d[i] = s[i];
        }
        return;
    }
#else
    (void)prefetch_src;
#endif
    if ((((uintptr_t)dst | (uintptr_t)src) & 7u) == 0u) {
        unsigned i = 0u;
        uint64_t *d64 = (uint64_t *)(void *)dst;
        const uint64_t *s64 = (const uint64_t *)(const void *)src;
        unsigned q = n >> 3;
        LS_NO_VEC_UNROLL
        for (; i < q; ++i) {
            d64[i] = s64[i];
        }
        LS_NO_VEC_UNROLL
        for (i = q << 3; i < n; ++i) {
            dst[i] = src[i];
        }
        return;
    }
    LS_NO_VEC_UNROLL
    for (unsigned i = 0u; i < n; ++i) {
        dst[i] = src[i];
    }
}

/* Non-temporal streaming copy (stnp on 16B-aligned dst -> zero DRAM RFO write-allocate;
 * 100% SCTLR_EL1.A == 1 safe via 4-reg element-aligned ld1 for any src alignment;
 * coherent with strip dc cvac + final dsb sy when clean_dcache != 0). */
static __attribute__((noinline)) void ls_copy_bytes_nt(
    uint8_t * __restrict dst, const uint8_t * __restrict src,
    unsigned n, int prefetch_src)
{
#if LS_USE_NEON
    if ((((uintptr_t)dst) & 15u) == 0u) {
        uint8_t *d = dst;
        const uint8_t *s = src;
        unsigned rem = n;
        if (rem >= 128u) {
            uint64_t cnt = (uint64_t)(rem >> 7);
            rem &= 127u;
            if (prefetch_src) {
                __asm__ volatile(
                    "1:\n\t"
                    "prfm pldl1strm, [%[s], #192]\n\t"
                    "prfm pldl1strm, [%[s], #256]\n\t"
                    "ld1  {v0.16b, v1.16b, v2.16b, v3.16b}, [%[s]], #64\n\t"
                    "ld1  {v4.16b, v5.16b, v6.16b, v7.16b}, [%[s]], #64\n\t"
                    "stnp q0, q1, [%[d]]\n\t"
                    "stnp q2, q3, [%[d], #32]\n\t"
                    "stnp q4, q5, [%[d], #64]\n\t"
                    "stnp q6, q7, [%[d], #96]\n\t"
                    "add  %[d], %[d], #128\n\t"
                    "subs %[cnt], %[cnt], #1\n\t"
                    "b.ne 1b"
                    : [d] "+r"(d), [s] "+r"(s), [cnt] "+r"(cnt)
                    :
                    : "cc", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
            } else {
                __asm__ volatile(
                    "1:\n\t"
                    "ld1  {v0.16b, v1.16b, v2.16b, v3.16b}, [%[s]], #64\n\t"
                    "ld1  {v4.16b, v5.16b, v6.16b, v7.16b}, [%[s]], #64\n\t"
                    "stnp q0, q1, [%[d]]\n\t"
                    "stnp q2, q3, [%[d], #32]\n\t"
                    "stnp q4, q5, [%[d], #64]\n\t"
                    "stnp q6, q7, [%[d], #96]\n\t"
                    "add  %[d], %[d], #128\n\t"
                    "subs %[cnt], %[cnt], #1\n\t"
                    "b.ne 1b"
                    : [d] "+r"(d), [s] "+r"(s), [cnt] "+r"(cnt)
                    :
                    : "cc", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
            }
        }
        if (rem >= 64u) {
            __asm__ volatile(
                "ld1  {v0.16b, v1.16b, v2.16b, v3.16b}, [%[s]], #64\n\t"
                "stnp q0, q1, [%[d]]\n\t"
                "stnp q2, q3, [%[d], #32]\n\t"
                "add  %[d], %[d], #64"
                : [d] "+r"(d), [s] "+r"(s)
                :
                : "memory", "v0", "v1", "v2", "v3");
            rem -= 64u;
        }
        if (rem >= 32u) {
            __asm__ volatile(
                "ld1  {v0.16b, v1.16b}, [%[s]], #32\n\t"
                "stnp q0, q1, [%[d]]\n\t"
                "add  %[d], %[d], #32"
                : [d] "+r"(d), [s] "+r"(s)
                :
                : "memory", "v0", "v1");
            rem -= 32u;
        }
        if (rem >= 16u) {
            ls_st16a(d, ls_ld1_u8(s));
            d += 16u;
            s += 16u;
            rem -= 16u;
        }
        LS_NO_VEC_UNROLL
        for (unsigned i = 0u; i < rem; ++i) {
            d[i] = s[i];
        }
        return;
    }
#endif
    ls_copy_bytes(dst, src, n, prefetch_src);
}

/* =========================================================================
 * V71 Fast Horizontal 4:5 Upscaler: 1024 B -> 1280 B (vld4 + uhadd/urhadd)
 * ========================================================================= */

#if LS_USE_NEON

/* 10-vector tbl4+tbx1 5:4 pack masks (160 B per plane, loaded into v18..v27 once per row).
 * Enables in-place v0..v3 + v4 filter + direct tbl4/tbx1 store with ZERO movs & ZERO Q-spills! */
static const uint8_t k_tbl_luma[10][16] __attribute__((aligned(16))) = {
    {  0, 16, 32, 48,255,  1, 17, 33, 49,255,  2, 18, 34, 50,255,  3},
    { 19, 35, 51,255,  4, 20, 36, 52,255,  5, 21, 37, 53,255,  6, 22},
    { 38, 54,255,  7, 23, 39, 55,255,  8, 24, 40, 56,255,  9, 25, 41},
    { 57,255, 10, 26, 42, 58,255, 11, 27, 43, 59,255, 12, 28, 44, 60},
    {255, 13, 29, 45, 61,255, 14, 30, 46, 62,255, 15, 31, 47, 63,255},
    {255,255,255,255,  0,255,255,255,255,  1,255,255,255,255,  2,255},
    {255,255,255,  3,255,255,255,255,  4,255,255,255,255,  5,255,255},
    {255,255,  6,255,255,255,255,  7,255,255,255,255,  8,255,255,255},
    {255,  9,255,255,255,255, 10,255,255,255,255, 11,255,255,255,255},
    { 12,255,255,255,255, 13,255,255,255,255, 14,255,255,255,255, 15},
};

static const uint8_t k_tbl_uv[10][16] __attribute__((aligned(16))) = {
    {  0,  1, 16, 17, 32, 33, 48, 49,255,255,  2,  3, 18, 19, 34, 35},
    { 50, 51,255,255,  4,  5, 20, 21, 36, 37, 52, 53,255,255,  6,  7},
    { 22, 23, 38, 39, 54, 55,255,255,  8,  9, 24, 25, 40, 41, 56, 57},
    {255,255, 10, 11, 26, 27, 42, 43, 58, 59,255,255, 12, 13, 28, 29},
    { 44, 45, 60, 61,255,255, 14, 15, 30, 31, 46, 47, 62, 63,255,255},
    {255,255,255,255,255,255,255,255,  0,  1,255,255,255,255,255,255},
    {255,255,  2,  3,255,255,255,255,255,255,255,255,  4,  5,255,255},
    {255,255,255,255,255,255,  6,  7,255,255,255,255,255,255,255,255},
    {  8,  9,255,255,255,255,255,255,255,255, 10, 11,255,255,255,255},
    {255,255,255,255, 12, 13,255,255,255,255,255,255,255,255, 14, 15},
};

#define LS_HSCALE_CORE_ASM \
    "uhadd  v5.16b, v3.16b, v4.16b\n\t" \
    "urhadd v4.16b, v4.16b, v5.16b\n\t" \
    "uhadd  v4.16b, v3.16b, v4.16b\n\t" \
    "urhadd v4.16b, v3.16b, v4.16b\n\t" \
    "urhadd v5.16b, v2.16b, v3.16b\n\t" \
    "uhadd  v3.16b, v3.16b, v5.16b\n\t" \
    "urhadd v3.16b, v2.16b, v3.16b\n\t" \
    "urhadd v5.16b, v1.16b, v2.16b\n\t" \
    "uhadd  v5.16b, v1.16b, v5.16b\n\t" \
    "urhadd v2.16b, v2.16b, v5.16b\n\t" \
    "uhadd  v5.16b, v0.16b, v1.16b\n\t" \
    "urhadd v5.16b, v0.16b, v5.16b\n\t" \
    "uhadd  v5.16b, v1.16b, v5.16b\n\t" \
    "urhadd v1.16b, v1.16b, v5.16b\n\t" \
    "tbl    v5.16b,  {v0.16b, v1.16b, v2.16b, v3.16b}, v18.16b\n\t" \
    "tbx    v5.16b,  {v4.16b}, v23.16b\n\t" \
    "tbl    v6.16b,  {v0.16b, v1.16b, v2.16b, v3.16b}, v19.16b\n\t" \
    "tbx    v6.16b,  {v4.16b}, v24.16b\n\t" \
    "tbl    v7.16b,  {v0.16b, v1.16b, v2.16b, v3.16b}, v20.16b\n\t" \
    "tbx    v7.16b,  {v4.16b}, v25.16b\n\t" \
    "tbl    v16.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v21.16b\n\t" \
    "tbx    v16.16b, {v4.16b}, v26.16b\n\t" \
    "tbl    v17.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v22.16b\n\t" \
    "tbx    v17.16b, {v4.16b}, v27.16b\n\t"

#define LS_HSCALE_LUMA_STEP \
    "ld4    {v0.16b, v1.16b, v2.16b, v3.16b}, [%[s]], #64\n\t" \
    "ldr    b4, [%[s]]\n\t" \
    "ext    v4.16b, v0.16b, v4.16b, #1\n\t" \
    LS_HSCALE_CORE_ASM \
    "stp    q5,  q6,  [%[d]], #80\n\t" \
    "stp    q7,  q16, [%[d], #-48]\n\t" \
    "stur   q17,      [%[d], #-16]\n\t"

#define LS_HSCALE_UV_STEP \
    "ld4    {v0.8h, v1.8h, v2.8h, v3.8h}, [%[s]], #64\n\t" \
    "ldr    h4, [%[s]]\n\t" \
    "ext    v4.16b, v0.16b, v4.16b, #2\n\t" \
    LS_HSCALE_CORE_ASM \
    "stp    q5,  q6,  [%[d]], #80\n\t" \
    "stp    q7,  q16, [%[d], #-48]\n\t" \
    "stur   q17,      [%[d], #-16]\n\t"

/* Precondition: dst (1280 B) and src (1024 B) are 16-byte aligned.
 * 3x-unrolled 15-iter loop + peeled 16th tail + post-index stp: ~498 insns/row, 0 stack, 0 Q-spills! */
static __attribute__((noinline)) void ls_hscale_1024_1280_aligned(
    uint8_t * __restrict dst, const uint8_t * __restrict src, int is_uv)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    uint64_t cnt = 5u;
    if (!is_uv) {
        const uint8_t *m = k_tbl_luma[0];
        __asm__ volatile(
            "ldp q18, q19, [%[m]]\n\t"
            "ldp q20, q21, [%[m], #32]\n\t"
            "ldp q22, q23, [%[m], #64]\n\t"
            "ldp q24, q25, [%[m], #96]\n\t"
            "ldp q26, q27, [%[m], #128]\n\t"
            "1:\n\t"
            LS_HSCALE_LUMA_STEP
            LS_HSCALE_LUMA_STEP
            LS_HSCALE_LUMA_STEP
            "subs   %[cnt], %[cnt], #1\n\t"
            "b.ne   1b\n\t"
            "ld4    {v0.16b, v1.16b, v2.16b, v3.16b}, [%[s]], #64\n\t"
            "dup    v4.16b, v3.b[15]\n\t"
            "ext    v4.16b, v0.16b, v4.16b, #1\n\t"
            LS_HSCALE_CORE_ASM
            "stp    q5,  q6,  [%[d]]\n\t"
            "stp    q7,  q16, [%[d], #32]\n\t"
            "str    q17,      [%[d], #64]"
            : [d] "+r"(d), [s] "+r"(s), [cnt] "+r"(cnt)
            : [m] "r"(m)
            : "cc", "memory",
              "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
              "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
              "v24", "v25", "v26", "v27");
        return;
    }

    const uint8_t *m = k_tbl_uv[0];
    __asm__ volatile(
        "ldp q18, q19, [%[m]]\n\t"
        "ldp q20, q21, [%[m], #32]\n\t"
        "ldp q22, q23, [%[m], #64]\n\t"
        "ldp q24, q25, [%[m], #96]\n\t"
        "ldp q26, q27, [%[m], #128]\n\t"
        "1:\n\t"
        LS_HSCALE_UV_STEP
        LS_HSCALE_UV_STEP
        LS_HSCALE_UV_STEP
        "subs   %[cnt], %[cnt], #1\n\t"
        "b.ne   1b\n\t"
        "ld4    {v0.8h, v1.8h, v2.8h, v3.8h}, [%[s]], #64\n\t"
        "dup    v4.8h, v3.h[7]\n\t"
        "ext    v4.16b, v0.16b, v4.16b, #2\n\t"
        LS_HSCALE_CORE_ASM
        "stp    q5,  q6,  [%[d]]\n\t"
        "stp    q7,  q16, [%[d], #32]\n\t"
        "str    q17,      [%[d], #64]"
        : [d] "+r"(d), [s] "+r"(s), [cnt] "+r"(cnt)
        : [m] "r"(m)
        : "cc", "memory",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27");
}

/* =========================================================================
 * V71r4 Fast Vertical Blend (128B/iter ld1 + uhadd/urhadd + stnp) &
 * 100% Bit-Exact Midtone Fast-Path Fused Grain (64B/iter + usqadd / rshrn#5)
 * ========================================================================= */

#define LS_VBLEND_W1(a, b) vrhaddq_u8((a), vhaddq_u8((a), vrhaddq_u8((a), vhaddq_u8((a), (b)))))
#define LS_VBLEND_W2(a, b) vrhaddq_u8((a), vhaddq_u8((a), vrhaddq_u8((a), (b))))
#define LS_VBLEND_W3(a, b) vrhaddq_u8((a), vhaddq_u8((a), vrhaddq_u8((b), vhaddq_u8((a), (b)))))
#define LS_VBLEND_W4(a, b) vrhaddq_u8((a), vhaddq_u8((a), (b)))
#define LS_VBLEND_W5(a, b) vrhaddq_u8((a), vhaddq_u8((b), vrhaddq_u8((a), vhaddq_u8((a), (b)))))
#define LS_VBLEND_W6(a, b) vrhaddq_u8((a), vhaddq_u8((b), vrhaddq_u8((a), (b))))
#define LS_VBLEND_W7(a, b) vrhaddq_u8((a), vhaddq_u8((b), vrhaddq_u8((b), vhaddq_u8((a), (b)))))
#define LS_VBLEND_W8(a, b) vrhaddq_u8((a), (b))

/* Exact V71 alternating uhadd/urhadd cascades on v0..v3 (a) & v4..v7 (b) -> v0..v3 (scratch v16..v19) */
#define LS_VB_ALT_W1 \
    "uhadd  v4.16b,  v0.16b, v4.16b\n\t"  "uhadd  v5.16b,  v1.16b, v5.16b\n\t" \
    "uhadd  v6.16b,  v2.16b, v6.16b\n\t"  "uhadd  v7.16b,  v3.16b, v7.16b\n\t" \
    "urhadd v4.16b,  v0.16b, v4.16b\n\t"  "urhadd v5.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v6.16b,  v2.16b, v6.16b\n\t"  "urhadd v7.16b,  v3.16b, v7.16b\n\t" \
    "uhadd  v4.16b,  v0.16b, v4.16b\n\t"  "uhadd  v5.16b,  v1.16b, v5.16b\n\t" \
    "uhadd  v6.16b,  v2.16b, v6.16b\n\t"  "uhadd  v7.16b,  v3.16b, v7.16b\n\t" \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VB_ALT_W2 \
    "urhadd v4.16b,  v0.16b, v4.16b\n\t"  "urhadd v5.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v6.16b,  v2.16b, v6.16b\n\t"  "urhadd v7.16b,  v3.16b, v7.16b\n\t" \
    "uhadd  v4.16b,  v0.16b, v4.16b\n\t"  "uhadd  v5.16b,  v1.16b, v5.16b\n\t" \
    "uhadd  v6.16b,  v2.16b, v6.16b\n\t"  "uhadd  v7.16b,  v3.16b, v7.16b\n\t" \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VB_ALT_W3 \
    "uhadd  v16.16b, v0.16b, v4.16b\n\t"  "uhadd  v17.16b, v1.16b, v5.16b\n\t" \
    "uhadd  v18.16b, v2.16b, v6.16b\n\t"  "uhadd  v19.16b, v3.16b, v7.16b\n\t" \
    "urhadd v4.16b,  v4.16b, v16.16b\n\t" "urhadd v5.16b,  v5.16b, v17.16b\n\t" \
    "urhadd v6.16b,  v6.16b, v18.16b\n\t" "urhadd v7.16b,  v7.16b, v19.16b\n\t" \
    "uhadd  v4.16b,  v0.16b, v4.16b\n\t"  "uhadd  v5.16b,  v1.16b, v5.16b\n\t" \
    "uhadd  v6.16b,  v2.16b, v6.16b\n\t"  "uhadd  v7.16b,  v3.16b, v7.16b\n\t" \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VB_ALT_W4 \
    "uhadd  v4.16b,  v0.16b, v4.16b\n\t"  "uhadd  v5.16b,  v1.16b, v5.16b\n\t" \
    "uhadd  v6.16b,  v2.16b, v6.16b\n\t"  "uhadd  v7.16b,  v3.16b, v7.16b\n\t" \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VB_ALT_W5 \
    "uhadd  v16.16b, v0.16b, v4.16b\n\t"  "uhadd  v17.16b, v1.16b, v5.16b\n\t" \
    "uhadd  v18.16b, v2.16b, v6.16b\n\t"  "uhadd  v19.16b, v3.16b, v7.16b\n\t" \
    "urhadd v16.16b, v0.16b, v16.16b\n\t" "urhadd v17.16b, v1.16b, v17.16b\n\t" \
    "urhadd v18.16b, v2.16b, v18.16b\n\t" "urhadd v19.16b, v3.16b, v19.16b\n\t" \
    "uhadd  v4.16b,  v4.16b, v16.16b\n\t" "uhadd  v5.16b,  v5.16b, v17.16b\n\t" \
    "uhadd  v6.16b,  v6.16b, v18.16b\n\t" "uhadd  v7.16b,  v7.16b, v19.16b\n\t" \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VB_ALT_W6 \
    "urhadd v16.16b, v0.16b, v4.16b\n\t"  "urhadd v17.16b, v1.16b, v5.16b\n\t" \
    "urhadd v18.16b, v2.16b, v6.16b\n\t"  "urhadd v19.16b, v3.16b, v7.16b\n\t" \
    "uhadd  v4.16b,  v4.16b, v16.16b\n\t" "uhadd  v5.16b,  v5.16b, v17.16b\n\t" \
    "uhadd  v6.16b,  v6.16b, v18.16b\n\t" "uhadd  v7.16b,  v7.16b, v19.16b\n\t" \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VB_ALT_W7 \
    "uhadd  v16.16b, v0.16b, v4.16b\n\t"  "uhadd  v17.16b, v1.16b, v5.16b\n\t" \
    "uhadd  v18.16b, v2.16b, v6.16b\n\t"  "uhadd  v19.16b, v3.16b, v7.16b\n\t" \
    "urhadd v16.16b, v4.16b, v16.16b\n\t" "urhadd v17.16b, v5.16b, v17.16b\n\t" \
    "urhadd v18.16b, v6.16b, v18.16b\n\t" "urhadd v19.16b, v7.16b, v19.16b\n\t" \
    "uhadd  v4.16b,  v4.16b, v16.16b\n\t" "uhadd  v5.16b,  v5.16b, v17.16b\n\t" \
    "uhadd  v6.16b,  v6.16b, v18.16b\n\t" "uhadd  v7.16b,  v7.16b, v19.16b\n\t" \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VB_ALT_W8 \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VBLEND_1280_CASE_ASM(LBL, OP_ASM) \
    LBL ":\n\t" \
    "1:\n\t" \
    "ld1    {v0.16b, v1.16b, v2.16b, v3.16b}, [%[p0]], #64\n\t" \
    "ld1    {v4.16b, v5.16b, v6.16b, v7.16b}, [%[p1]], #64\n\t" \
    OP_ASM \
    "stnp   q0, q1, [%[pd]]\n\t" \
    "stnp   q2, q3, [%[pd], #32]\n\t" \
    "ld1    {v0.16b, v1.16b, v2.16b, v3.16b}, [%[p0]], #64\n\t" \
    "ld1    {v4.16b, v5.16b, v6.16b, v7.16b}, [%[p1]], #64\n\t" \
    OP_ASM \
    "stnp   q0, q1, [%[pd], #64]\n\t" \
    "stnp   q2, q3, [%[pd], #96]\n\t" \
    "add    %[pd], %[pd], #128\n\t" \
    "subs   %[cnt], %[cnt], #1\n\t" \
    "b.ne   1b\n\t" \
    "b      99f\n\t"

/* Precondition: dst, r0, r1 (1280 B) are 16-byte aligned; 128B/iter stnp, 0 stack, 0 Q-spills! */
static __attribute__((noinline)) void ls_vblend_1280_aligned(
    uint8_t * __restrict dst, const uint8_t * __restrict r0,
    const uint8_t * __restrict r1, unsigned wy)
{
    if (wy > 8u) {
        const uint8_t *tmp = r0;
        r0 = r1;
        r1 = tmp;
        wy = 16u - wy;
    }
    uint8_t *pd = dst;
    const uint8_t *p0 = r0;
    const uint8_t *p1 = r1;
    uint64_t cnt = 10u;
    uint64_t wy64 = (uint64_t)wy;
    uint64_t jt;

    __asm__ volatile(
        "adr    %[jt], 80f\n\t"
        "add    %[jt], %[jt], %[wy], lsl #2\n\t"
        "br     %[jt]\n\t"
        "80:\n\t"
        "b      10f\n\t"
        "b      11f\n\t"
        "b      12f\n\t"
        "b      13f\n\t"
        "b      14f\n\t"
        "b      15f\n\t"
        "b      16f\n\t"
        "b      17f\n\t"
        "b      18f\n\t"
        "10:\n\t"
        "1:\n\t"
        "ld1    {v0.16b, v1.16b, v2.16b, v3.16b}, [%[p0]], #64\n\t"
        "ld1    {v4.16b, v5.16b, v6.16b, v7.16b}, [%[p0]], #64\n\t"
        "stnp   q0, q1, [%[pd]]\n\t"
        "stnp   q2, q3, [%[pd], #32]\n\t"
        "stnp   q4, q5, [%[pd], #64]\n\t"
        "stnp   q6, q7, [%[pd], #96]\n\t"
        "add    %[pd], %[pd], #128\n\t"
        "subs   %[cnt], %[cnt], #1\n\t"
        "b.ne   1b\n\t"
        "b      99f\n\t"
        LS_VBLEND_1280_CASE_ASM("11", LS_VB_ALT_W1)
        LS_VBLEND_1280_CASE_ASM("12", LS_VB_ALT_W2)
        LS_VBLEND_1280_CASE_ASM("13", LS_VB_ALT_W3)
        LS_VBLEND_1280_CASE_ASM("14", LS_VB_ALT_W4)
        LS_VBLEND_1280_CASE_ASM("15", LS_VB_ALT_W5)
        LS_VBLEND_1280_CASE_ASM("16", LS_VB_ALT_W6)
        LS_VBLEND_1280_CASE_ASM("17", LS_VB_ALT_W7)
        LS_VBLEND_1280_CASE_ASM("18", LS_VB_ALT_W8)
        "99:"
        : [pd] "+r"(pd), [p0] "+r"(p0), [p1] "+r"(p1), [cnt] "+r"(cnt), [jt] "=&r"(jt)
        : [wy] "r"(wy64)
        : "cc", "memory",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "v16", "v17", "v18", "v19");
}

/* Shared 64B exact silver-halide grain tail for y0..y3 (v0..v3) + g0..g3 (v4..v7), v30 = {32}.
 * Fast path (all 64 px in [32,223] <=> max(y - 32) <= 191): tone == 64 => delta == g => 4x usqadd!
 * Cold path (any px < 32 or > 223): jumps to shared 90: (8 insns/16B: mvn/umin/umin32/smull/rshrn#5/usqadd). */
#define LS_GRAIN_APPLY64_TAIL_ASM \
    "sub    v16.16b, v0.16b, v30.16b\n\t" \
    "sub    v17.16b, v1.16b, v30.16b\n\t" \
    "sub    v18.16b, v2.16b, v30.16b\n\t" \
    "sub    v19.16b, v3.16b, v30.16b\n\t" \
    "umax   v16.16b, v16.16b, v17.16b\n\t" \
    "umax   v18.16b, v18.16b, v19.16b\n\t" \
    "umax   v16.16b, v16.16b, v18.16b\n\t" \
    "umaxv  b16, v16.16b\n\t" \
    "ld1    {v4.16b, v5.16b, v6.16b, v7.16b}, [%[pg]], #64\n\t" \
    "umov   %w[tmp], v16.b[0]\n\t" \
    "cmp    %w[tmp], #191\n\t" \
    "b.hi   2f\n\t" \
    "usqadd v0.16b, v4.16b\n\t" \
    "usqadd v1.16b, v5.16b\n\t" \
    "usqadd v2.16b, v6.16b\n\t" \
    "usqadd v3.16b, v7.16b\n\t" \
    "3:\n\t" \
    "stnp   q0, q1, [%[pd]]\n\t" \
    "stnp   q2, q3, [%[pd], #32]\n\t" \
    "add    %[pd], %[pd], #64\n\t" \
    "subs   %[cnt], %[cnt], #1\n\t" \
    "b.ne   1b\n\t" \
    "cbz    %[rem], 99f\n\t" \
    "mov    %[pg], %[trow]\n\t" \
    "cmp    %[rem], #8\n\t" \
    "csel   %[cnt], %[rem], %[c8], lo\n\t" \
    "sub    %[rem], %[rem], %[cnt]\n\t" \
    "b      1b\n\t" \
    "2:\n\t" \
    "adr    %[ret], 3b\n\t" \
    "b      90f\n\t"

#define LS_VBLEND_GRAIN_CASE_ASM(LBL, OP_ASM) \
    LBL ":\n\t" \
    "1:\n\t" \
    "ld1    {v0.16b, v1.16b, v2.16b, v3.16b}, [%[p0]], #64\n\t" \
    "ld1    {v4.16b, v5.16b, v6.16b, v7.16b}, [%[p1]], #64\n\t" \
    OP_ASM \
    LS_GRAIN_APPLY64_TAIL_ASM

#define LS_GRAIN_SLOW64_BODY_ASM \
    "mvn    v16.16b, v0.16b\n\t" \
    "mvn    v17.16b, v1.16b\n\t" \
    "mvn    v18.16b, v2.16b\n\t" \
    "mvn    v19.16b, v3.16b\n\t" \
    "umin   v16.16b, v0.16b, v16.16b\n\t" \
    "umin   v17.16b, v1.16b, v17.16b\n\t" \
    "umin   v18.16b, v2.16b, v18.16b\n\t" \
    "umin   v19.16b, v3.16b, v19.16b\n\t" \
    "umin   v16.16b, v16.16b, v30.16b\n\t" \
    "umin   v17.16b, v17.16b, v30.16b\n\t" \
    "umin   v18.16b, v18.16b, v30.16b\n\t" \
    "umin   v19.16b, v19.16b, v30.16b\n\t" \
    "smull  v20.8h,  v4.8b,  v16.8b\n\t" \
    "smull2 v21.8h,  v4.16b, v16.16b\n\t" \
    "smull  v22.8h,  v5.8b,  v17.8b\n\t" \
    "smull2 v23.8h,  v5.16b, v17.16b\n\t" \
    "smull  v24.8h,  v6.8b,  v18.8b\n\t" \
    "smull2 v25.8h,  v6.16b, v18.16b\n\t" \
    "smull  v26.8h,  v7.8b,  v19.8b\n\t" \
    "smull2 v27.8h,  v7.16b, v19.16b\n\t" \
    "rshrn  v16.8b,  v20.8h, #5\n\t" \
    "rshrn2 v16.16b, v21.8h, #5\n\t" \
    "rshrn  v17.8b,  v22.8h, #5\n\t" \
    "rshrn2 v17.16b, v23.8h, #5\n\t" \
    "rshrn  v18.8b,  v24.8h, #5\n\t" \
    "rshrn2 v18.16b, v25.8h, #5\n\t" \
    "rshrn  v19.8b,  v26.8h, #5\n\t" \
    "rshrn2 v19.16b, v27.8h, #5\n\t" \
    "usqadd v0.16b,  v16.16b\n\t" \
    "usqadd v1.16b,  v17.16b\n\t" \
    "usqadd v2.16b,  v18.16b\n\t" \
    "usqadd v3.16b,  v19.16b\n\t"

/* Fused vertical blend + 512x256 twisted-torus grain directly into dst (0 extra Y DRAM traffic!).
 * Precondition: dst, r0, r1 (1280 B) are 16-byte aligned; trow is 512 B row (any alignment via ld1);
 * tx0 is multiple of 64 in [0..448]. 64B/iter stnp64 + 100% bit-exact [32,223] fast path, 0 stack, 0 Q-spills! */
static __attribute__((noinline)) void ls_vblend_1280_grain_aligned(
    uint8_t * __restrict dst, const uint8_t * __restrict r0,
    const uint8_t * __restrict r1, unsigned wy,
    const int8_t * __restrict trow, unsigned tx0)
{
    if (wy > 8u) {
        const uint8_t *tmp = r0;
        r0 = r1;
        r1 = tmp;
        wy = 16u - wy;
    }
    uint8_t *pd = dst;
    const uint8_t *p0 = r0;
    const uint8_t *p1 = r1;
    const int8_t *pg = trow + tx0;
    uint64_t cnt = (uint64_t)((LS_GTILE_W - tx0) >> 6);
    uint64_t rem = (uint64_t)(LS_DST_W >> 6) - cnt;
    uint64_t c8 = 8u;
    uint64_t wy64 = (uint64_t)wy;
    uint64_t tmp, ret;

    __asm__ volatile(
        "movi   v30.16b, #32\n\t"
        "adr    %[ret], 80f\n\t"
        "add    %[ret], %[ret], %[wy], lsl #2\n\t"
        "br     %[ret]\n\t"
        "80:\n\t"
        "b      10f\n\t"
        "b      11f\n\t"
        "b      12f\n\t"
        "b      13f\n\t"
        "b      14f\n\t"
        "b      15f\n\t"
        "b      16f\n\t"
        "b      17f\n\t"
        "b      18f\n\t"
        "10:\n\t"
        "1:\n\t"
        "ld1    {v0.16b, v1.16b, v2.16b, v3.16b}, [%[p0]], #64\n\t"
        LS_GRAIN_APPLY64_TAIL_ASM
        LS_VBLEND_GRAIN_CASE_ASM("11", LS_VB_ALT_W1)
        LS_VBLEND_GRAIN_CASE_ASM("12", LS_VB_ALT_W2)
        LS_VBLEND_GRAIN_CASE_ASM("13", LS_VB_ALT_W3)
        LS_VBLEND_GRAIN_CASE_ASM("14", LS_VB_ALT_W4)
        LS_VBLEND_GRAIN_CASE_ASM("15", LS_VB_ALT_W5)
        LS_VBLEND_GRAIN_CASE_ASM("16", LS_VB_ALT_W6)
        LS_VBLEND_GRAIN_CASE_ASM("17", LS_VB_ALT_W7)
        LS_VBLEND_GRAIN_CASE_ASM("18", LS_VB_ALT_W8)
        "90:\n\t"
        LS_GRAIN_SLOW64_BODY_ASM
        "br     %[ret]\n\t"
        "99:"
        : [pd] "+r"(pd), [p0] "+r"(p0), [p1] "+r"(p1), [pg] "+r"(pg),
          [cnt] "+r"(cnt), [rem] "+r"(rem), [tmp] "=&r"(tmp), [ret] "=&r"(ret)
        : [trow] "r"(trow), [c8] "r"(c8), [wy] "r"(wy64)
        : "cc", "memory",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v30");
}

/* Vertical blend + NV12->I420 U/V de-interleave via uzp1/uzp2 (matching alternating uhadd/urhadd). */
#define LS_VBLEND_I420_CASE(OP) do { \
    LS_NO_UNROLL \
    for (unsigned i = 0u; i < LS_DST_CW; i += 16u) { \
        uint8x16_t o0 = OP(ls_ld16a(r0 + 2u * i +  0u), ls_ld16a(r1 + 2u * i +  0u)); \
        uint8x16_t o1 = OP(ls_ld16a(r0 + 2u * i + 16u), ls_ld16a(r1 + 2u * i + 16u)); \
        ls_st16a(du + i, vuzp1q_u8(o0, o1)); \
        ls_st16a(dv + i, vuzp2q_u8(o0, o1)); \
    } \
} while (0)

static __attribute__((noinline)) void ls_vblend_1280_to_i420_uv_aligned(
    uint8_t * __restrict du, uint8_t * __restrict dv,
    const uint8_t * __restrict r0, const uint8_t * __restrict r1, unsigned wy)
{
    if (wy > 8u) {
        const uint8_t *tmp = r0;
        r0 = r1;
        r1 = tmp;
        wy = 16u - wy;
    }
    switch (wy) {
    case 0u:
        LS_NO_UNROLL
        for (unsigned i = 0u; i < LS_DST_CW; i += 16u) {
            uint8x16_t a0 = ls_ld16a(r0 + 2u * i +  0u);
            uint8x16_t a1 = ls_ld16a(r0 + 2u * i + 16u);
            ls_st16a(du + i, vuzp1q_u8(a0, a1));
            ls_st16a(dv + i, vuzp2q_u8(a0, a1));
        }
        return;
    case 8u: LS_VBLEND_I420_CASE(LS_VBLEND_W8); return;
    case 4u: LS_VBLEND_I420_CASE(LS_VBLEND_W4); return;
    case 2u: LS_VBLEND_I420_CASE(LS_VBLEND_W2); return;
    case 6u: LS_VBLEND_I420_CASE(LS_VBLEND_W6); return;
    case 1u: LS_VBLEND_I420_CASE(LS_VBLEND_W1); return;
    case 3u: LS_VBLEND_I420_CASE(LS_VBLEND_W3); return;
    case 5u: LS_VBLEND_I420_CASE(LS_VBLEND_W5); return;
    default: LS_VBLEND_I420_CASE(LS_VBLEND_W7); return;
    }
}

/* Fused 1:1 copy + 512x256 twisted-torus grain for arbitrary even width (100% SCTLR_EL1.A safe).
 * 64B/iter stnp64 + 100% bit-exact [32,223] midtone fast path when dst is 16B-aligned. */
static __attribute__((noinline)) void ls_copy_grain_row(
    uint8_t * __restrict dst, const uint8_t * __restrict src,
    unsigned w, const int8_t * __restrict trow, unsigned tx0)
{
    uint8x16_t v32 = vdupq_n_u8(32u);
    unsigned tx = tx0;
    unsigned x = 0u;
    int dst_aln = (((uintptr_t)dst & 15u) == 0u);

    while (x + 16u <= w) {
        unsigned seg = LS_GTILE_W - tx;
        unsigned rem = (w - x) & ~15u;
        if (seg > rem) {
            seg = rem;
        }
        const uint8_t *ps = src + x;
        const int8_t  *pg = trow + tx;
        uint8_t       *pd = dst + x;
        unsigned i = 0u;
        if (dst_aln) {
            if (seg >= 64u) {
                uint64_t cnt = (uint64_t)(seg >> 6);
                uint64_t tmp;
                __asm__ volatile(
                    "movi   v30.16b, #32\n\t"
                    "1:\n\t"
                    "ld1    {v0.16b, v1.16b, v2.16b, v3.16b}, [%[ps]], #64\n\t"
                    "sub    v16.16b, v0.16b, v30.16b\n\t"
                    "sub    v17.16b, v1.16b, v30.16b\n\t"
                    "sub    v18.16b, v2.16b, v30.16b\n\t"
                    "sub    v19.16b, v3.16b, v30.16b\n\t"
                    "umax   v16.16b, v16.16b, v17.16b\n\t"
                    "umax   v18.16b, v18.16b, v19.16b\n\t"
                    "umax   v16.16b, v16.16b, v18.16b\n\t"
                    "umaxv  b16, v16.16b\n\t"
                    "ld1    {v4.16b, v5.16b, v6.16b, v7.16b}, [%[pg]], #64\n\t"
                    "umov   %w[tmp], v16.b[0]\n\t"
                    "cmp    %w[tmp], #191\n\t"
                    "b.hi   2f\n\t"
                    "usqadd v0.16b, v4.16b\n\t"
                    "usqadd v1.16b, v5.16b\n\t"
                    "usqadd v2.16b, v6.16b\n\t"
                    "usqadd v3.16b, v7.16b\n\t"
                    "3:\n\t"
                    "stnp   q0, q1, [%[pd]]\n\t"
                    "stnp   q2, q3, [%[pd], #32]\n\t"
                    "add    %[pd], %[pd], #64\n\t"
                    "subs   %[cnt], %[cnt], #1\n\t"
                    "b.ne   1b\n\t"
                    "b      4f\n\t"
                    "2:\n\t"
                    LS_GRAIN_SLOW64_BODY_ASM
                    "b      3b\n\t"
                    "4:"
                    : [pd] "+r"(pd), [ps] "+r"(ps), [pg] "+r"(pg),
                      [cnt] "+r"(cnt), [tmp] "=&r"(tmp)
                    :
                    : "cc", "memory",
                      "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
                      "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
                      "v24", "v25", "v26", "v27", "v30");
                i = seg & ~63u;
            }
            LS_NO_UNROLL
            for (; i < seg; i += 16u, ps += 16u, pg += 16u, pd += 16u) {
                ls_st16a(pd, ls_grain_apply16_slow(ls_ld1_u8(ps), ls_ld1_s8(pg), v32));
            }
        } else {
            LS_NO_UNROLL
            for (; i < seg; i += 16u) {
                ls_st1_u8(pd + i, ls_grain_apply16_slow(ls_ld1_u8(ps + i), ls_ld1_s8(pg + i), v32));
            }
        }
        x += seg;
        tx = (tx + seg) & (LS_GTILE_W - 1u);
    }
    for (; x < w; ++x) {
        dst[x] = ls_grain_apply_scalar(src[x], trow[tx]);
        tx = (tx + 1u) & (LS_GTILE_W - 1u);
    }
}

#else /* Portable scalar fallback matching NEON uhadd/urhadd cascade bit-for-bit */

static inline unsigned ls_fh(unsigned a, unsigned b) { return (a + b) >> 1; }
static inline unsigned ls_rh(unsigned a, unsigned b) { return (a + b + 1u) >> 1; }

static inline uint8_t ls_blend_scalar(unsigned a, unsigned b, unsigned wy)
{
    if (wy > 8u) {
        unsigned t = a; a = b; b = t;
        wy = 16u - wy;
    }
    switch (wy) {
    case 0u: return (uint8_t)a;
    case 8u: return (uint8_t)ls_rh(a, b);
    case 4u: return (uint8_t)ls_rh(a, ls_fh(a, b));
    case 2u: return (uint8_t)ls_rh(a, ls_fh(a, ls_rh(a, b)));
    case 6u: return (uint8_t)ls_rh(a, ls_fh(b, ls_rh(a, b)));
    case 1u: return (uint8_t)ls_rh(a, ls_fh(a, ls_rh(a, ls_fh(a, b))));
    case 3u: return (uint8_t)ls_rh(a, ls_fh(a, ls_rh(b, ls_fh(a, b))));
    case 5u: return (uint8_t)ls_rh(a, ls_fh(b, ls_rh(a, ls_fh(a, b))));
    default: return (uint8_t)ls_rh(a, ls_fh(b, ls_rh(b, ls_fh(a, b))));
    }
}

static void ls_hscale_1024_1280_aligned(uint8_t * __restrict dst,
                                        const uint8_t * __restrict src, int is_uv)
{
    if (!is_uv) {
        for (unsigned g = 0u; g < 256u; ++g) {
            unsigned a0 = src[4u * g + 0u];
            unsigned a1 = src[4u * g + 1u];
            unsigned a2 = src[4u * g + 2u];
            unsigned a3 = src[4u * g + 3u];
            unsigned a4 = (g < 255u) ? src[4u * g + 4u] : a3;
            uint8_t *o = dst + 5u * g;
            o[0] = (uint8_t)a0;
            o[1] = (uint8_t)ls_rh(a1, ls_fh(a1, ls_rh(a0, ls_fh(a0, a1))));
            o[2] = (uint8_t)ls_rh(a2, ls_fh(a1, ls_rh(a1, a2)));
            o[3] = (uint8_t)ls_rh(a2, ls_fh(a3, ls_rh(a2, a3)));
            o[4] = (uint8_t)ls_rh(a3, ls_fh(a3, ls_rh(a4, ls_fh(a3, a4))));
        }
        return;
    }
    for (unsigned g = 0u; g < 128u; ++g) {
        unsigned b = 8u * g;
        unsigned u0 = src[b + 0u], v0 = src[b + 1u];
        unsigned u1 = src[b + 2u], v1 = src[b + 3u];
        unsigned u2 = src[b + 4u], v2 = src[b + 5u];
        unsigned u3 = src[b + 6u], v3 = src[b + 7u];
        unsigned u4 = (g < 127u) ? src[b + 8u] : u3;
        unsigned v4 = (g < 127u) ? src[b + 9u] : v3;
        uint8_t *o = dst + 10u * g;
        o[0] = (uint8_t)u0;
        o[1] = (uint8_t)v0;
        o[2] = (uint8_t)ls_rh(u1, ls_fh(u1, ls_rh(u0, ls_fh(u0, u1))));
        o[3] = (uint8_t)ls_rh(v1, ls_fh(v1, ls_rh(v0, ls_fh(v0, v1))));
        o[4] = (uint8_t)ls_rh(u2, ls_fh(u1, ls_rh(u1, u2)));
        o[5] = (uint8_t)ls_rh(v2, ls_fh(v1, ls_rh(v1, v2)));
        o[6] = (uint8_t)ls_rh(u2, ls_fh(u3, ls_rh(u2, u3)));
        o[7] = (uint8_t)ls_rh(v2, ls_fh(v3, ls_rh(v2, v3)));
        o[8] = (uint8_t)ls_rh(u3, ls_fh(u3, ls_rh(u4, ls_fh(u3, u4))));
        o[9] = (uint8_t)ls_rh(v3, ls_fh(v3, ls_rh(v4, ls_fh(v3, v4))));
    }
}

static void ls_vblend_1280_aligned(uint8_t * __restrict dst, const uint8_t * __restrict r0,
                                   const uint8_t * __restrict r1, unsigned wy)
{
    if (wy == 0u) {
        ls_copy_bytes(dst, r0, LS_DST_W, 0);
        return;
    }
    for (unsigned i = 0u; i < LS_DST_W; ++i) {
        dst[i] = ls_blend_scalar(r0[i], r1[i], wy);
    }
}

static void ls_vblend_1280_grain_aligned(
    uint8_t * __restrict dst, const uint8_t * __restrict r0,
    const uint8_t * __restrict r1, unsigned wy,
    const int8_t * __restrict trow, unsigned tx0)
{
    for (unsigned i = 0u; i < LS_DST_W; ++i) {
        uint8_t y = (wy == 0u) ? r0[i] : ls_blend_scalar(r0[i], r1[i], wy);
        dst[i] = ls_grain_apply_scalar(y, trow[(tx0 + i) & (LS_GTILE_W - 1u)]);
    }
}

static void ls_vblend_1280_to_i420_uv_aligned(uint8_t * __restrict du, uint8_t * __restrict dv,
                                              const uint8_t * __restrict r0,
                                              const uint8_t * __restrict r1, unsigned wy)
{
    if (wy == 0u) {
        for (unsigned i = 0u; i < LS_DST_CW; ++i) {
            du[i] = r0[2u * i + 0u];
            dv[i] = r0[2u * i + 1u];
        }
        return;
    }
    for (unsigned i = 0u; i < LS_DST_CW; ++i) {
        du[i] = ls_blend_scalar(r0[2u * i + 0u], r1[2u * i + 0u], wy);
        dv[i] = ls_blend_scalar(r0[2u * i + 1u], r1[2u * i + 1u], wy);
    }
}

static void ls_copy_grain_row(
    uint8_t * __restrict dst, const uint8_t * __restrict src,
    unsigned w, const int8_t * __restrict trow, unsigned tx0)
{
    for (unsigned x = 0u; x < w; ++x) {
        dst[x] = ls_grain_apply_scalar(src[x], trow[(tx0 + x) & (LS_GTILE_W - 1u)]);
    }
}

#endif

static inline void ls_hscale_row(uint8_t *dst_aligned, const uint8_t *src,
                                 uint8_t *stage_1024, int is_uv, int lv_cached)
{
    const uint8_t *p = src;
    if (!lv_cached || (((uintptr_t)src) & 15u) != 0u) {
        ls_copy_bytes(stage_1024, src, LS_SRC_W, 0);
        p = stage_1024;
    }
    ls_hscale_1024_1280_aligned(dst_aligned, p, is_uv);
}

static inline void ls_vblend_row(uint8_t *dst, const uint8_t *r0, const uint8_t *r1,
                                 unsigned wy, uint8_t *stage_1280)
{
    if ((((uintptr_t)dst) & 15u) == 0u) {
        ls_vblend_1280_aligned(dst, r0, r1, wy);
    } else {
        ls_vblend_1280_aligned(stage_1280, r0, r1, wy);
        ls_copy_bytes(dst, stage_1280, LS_DST_W, 0);
    }
}

static inline void ls_vblend_grain_row(uint8_t *dst, const uint8_t *r0, const uint8_t *r1,
                                       unsigned wy, const int8_t *trow, unsigned tx0,
                                       uint8_t *stage_1280)
{
    if ((((uintptr_t)dst) & 15u) == 0u) {
        ls_vblend_1280_grain_aligned(dst, r0, r1, wy, trow, tx0);
    } else {
        ls_vblend_1280_grain_aligned(stage_1280, r0, r1, wy, trow, tx0);
        ls_copy_bytes(dst, stage_1280, LS_DST_W, 0);
    }
}

static inline unsigned ls_cache_slot(int32_t idx[2], unsigned row, int32_t keep, int *miss)
{
    if (idx[0] == (int32_t)row) {
        *miss = 0;
        return 0u;
    }
    if (idx[1] == (int32_t)row) {
        *miss = 0;
        return 1u;
    }
    unsigned slot = (keep >= 0 && idx[0] == keep) ? 1u : 0u;
    idx[slot] = (int32_t)row;
    *miss = 1;
    return slot;
}

/* =========================================================================
 * Public API 1 & 1B: 1024x{682|680} NV12 -> 1280x852 NV12 (+ optional grain)
 * ========================================================================= */

/* Split plane loops keep all hot Bresenham + parity-slot row-cache state in x19..x28
 * with zero per-row stack reloads and exact 1:1 parity mapping (iy & 1 vs (iy+1) & 1). */
static __attribute__((noinline)) void ls_scale_plane_nv12(
    uint8_t * __restrict dst, unsigned dst_stride, unsigned dst_h, unsigned coded_h,
    const uint8_t * __restrict src, unsigned src_h, unsigned src_stride,
    int is_uv, int lv_cached, int clean_dcache, const int8_t * __restrict grain_tile,
    uint8_t hbuf[2][LS_DST_W], uint8_t stage[LS_DST_W])
{
    int32_t slot0 = -1, slot1 = -1;
    uint32_t y_num = (src_h - 1u) << 4;
    uint32_t y_den = dst_h - 1u;
    uint32_t y_qs = y_num / y_den;
    uint32_t y_rs = y_num % y_den;
    uint32_t fy = 0u, y_rem = 0u;
    unsigned last_slot = 0u;
    unsigned strip0 = 0u;
    int src_fast = (lv_cached && ((((uintptr_t)src | (uintptr_t)src_stride) & 15u) == 0u));
    int dst_aln  = ((((uintptr_t)dst | (uintptr_t)dst_stride) & 15u) == 0u);
    uint8_t *dp  = dst;

    for (unsigned y = 0u; y < dst_h; ++y, dp += dst_stride) {
        unsigned iy = fy >> 4;
        unsigned wy = fy & 15u;
        fy += y_qs;
        y_rem += y_rs;
        if (y_rem >= y_den) {
            y_rem -= y_den;
            fy++;
        }
        unsigned s0 = iy & 1u;
        int32_t *p_slot0 = s0 ? &slot1 : &slot0;
        if (*p_slot0 != (int32_t)iy) {
            *p_slot0 = (int32_t)iy;
            const uint8_t *sr0 = src + (size_t)iy * src_stride;
            if (src_fast) {
                ls_hscale_1024_1280_aligned(hbuf[s0], sr0, is_uv);
            } else {
                ls_hscale_row(hbuf[s0], sr0, stage, is_uv, lv_cached);
            }
        }
        unsigned s1 = s0;
        if (wy && (iy + 1u < src_h)) {
            unsigned iy1 = iy + 1u;
            s1 = s0 ^ 1u;
            int32_t *p_slot1 = s1 ? &slot1 : &slot0;
            if (*p_slot1 != (int32_t)iy1) {
                *p_slot1 = (int32_t)iy1;
                const uint8_t *sr1 = src + (size_t)iy1 * src_stride;
                if (src_fast) {
                    ls_hscale_1024_1280_aligned(hbuf[s1], sr1, is_uv);
                } else {
                    ls_hscale_row(hbuf[s1], sr1, stage, is_uv, lv_cached);
                }
            }
        }
        if (grain_tile) {
            unsigned ty = y & (LS_GTILE_H - 1u);
            unsigned tx0 = ((y >> 8) * LS_GTILE_TWIST) & (LS_GTILE_W - 1u);
            const int8_t *trow = grain_tile + (size_t)ty * LS_GTILE_W;
            if (dst_aln) {
                ls_vblend_1280_grain_aligned(dp, hbuf[s0], hbuf[s1], wy, trow, tx0);
            } else {
                ls_vblend_grain_row(dp, hbuf[s0], hbuf[s1], wy, trow, tx0, stage);
            }
        } else {
            if (dst_aln) {
                ls_vblend_1280_aligned(dp, hbuf[s0], hbuf[s1], wy);
            } else {
                ls_vblend_row(dp, hbuf[s0], hbuf[s1], wy, stage);
            }
        }
        last_slot = s0;
        if (clean_dcache && ((y & 7u) == 7u)) {
            ls_clean_strip(dst, dst_stride, LS_DST_W, strip0, y + 1u);
            strip0 = y + 1u;
        }
    }

    /* Replicate final active row (grained when grain_tile != NULL) to coded_h */
    const uint8_t *pad_src = grain_tile
        ? (const uint8_t *)(dst + (size_t)(dst_h - 1u) * dst_stride)
        : hbuf[last_slot];
    for (unsigned py = dst_h; py < coded_h; ++py, dp += dst_stride) {
        if (grain_tile) {
            if (clean_dcache) {
                ls_copy_bytes_nt(dp, pad_src, LS_DST_W, 0);
            } else {
                ls_copy_bytes(dp, pad_src, LS_DST_W, 0);
            }
        } else if (dst_aln) {
            ls_vblend_1280_aligned(dp, pad_src, pad_src, 0u);
        } else {
            ls_vblend_row(dp, pad_src, pad_src, 0u, stage);
        }
        if (clean_dcache && ((py & 7u) == 7u)) {
            ls_clean_strip(dst, dst_stride, LS_DST_W, strip0, py + 1u);
            strip0 = py + 1u;
        }
    }
    if (clean_dcache && strip0 < coded_h) {
        ls_clean_strip(dst, dst_stride, LS_DST_W, strip0, coded_h);
    }
}

int live_scale_1024_to_1280_nv12_neon_grain(
    uint8_t *dst_y, uint8_t *dst_uv, unsigned dst_y_stride, unsigned dst_uv_stride,
    unsigned coded_h, const uint8_t *src_y, const uint8_t *src_uv,
    unsigned src_h, unsigned src_y_stride, unsigned src_uv_stride,
    int lv_cached, int clean_dcache, const int8_t *grain_tile)
{
    if (!dst_y || !dst_uv || !src_y || !src_uv ||
        src_h < 2u || src_h > LS_MAX_DIM || (src_h & 1u) != 0u ||
        coded_h < LS_DST_H || coded_h > LS_MAX_DIM || (coded_h & 1u) != 0u ||
        dst_y_stride < LS_DST_W || dst_uv_stride < LS_DST_W ||
        dst_y_stride > LS_MAX_STRIDE || dst_uv_stride > LS_MAX_STRIDE ||
        src_y_stride < LS_SRC_W || src_uv_stride < LS_SRC_W ||
        src_y_stride > LS_MAX_STRIDE || src_uv_stride > LS_MAX_STRIDE) {
        return 0;
    }

    uint8_t hbuf[2][LS_DST_W] __attribute__((aligned(16)));
    uint8_t stage[LS_DST_W]   __attribute__((aligned(16)));

    /* Luma: 1024 x src_h -> 1280 x 852 + bottom-row pad to coded_h */
    ls_scale_plane_nv12(dst_y, dst_y_stride, LS_DST_H, coded_h,
                        src_y, src_h, src_y_stride,
                        0, lv_cached, clean_dcache, grain_tile, hbuf, stage);

    /* Interleaved NV12 UV: 512 x (src_h/2) -> 640 x 426 + pad to coded_h/2 */
    ls_scale_plane_nv12(dst_uv, dst_uv_stride, LS_DST_CH, coded_h >> 1,
                        src_uv, src_h >> 1, src_uv_stride,
                        1, lv_cached, clean_dcache, 0, hbuf, stage);

    if (clean_dcache) {
        ls_dsb_sy();
    }
    return 1;
}

int live_scale_1024_to_1280_nv12_neon(
    uint8_t *dst_y, uint8_t *dst_uv, unsigned dst_y_stride, unsigned dst_uv_stride,
    unsigned coded_h, const uint8_t *src_y, const uint8_t *src_uv,
    unsigned src_h, unsigned src_y_stride, unsigned src_uv_stride,
    int lv_cached, int clean_dcache)
{
    return live_scale_1024_to_1280_nv12_neon_grain(
        dst_y, dst_uv, dst_y_stride, dst_uv_stride, coded_h,
        src_y, src_uv, src_h, src_y_stride, src_uv_stride,
        lv_cached, clean_dcache, 0);
}

/* =========================================================================
 * Public API 2: 1024x{682|680} NV12 -> 1280x852 Planar I420 (Legacy)
 * ========================================================================= */

int live_scale_1024_to_1280_i420_neon(
    uint8_t *dst_i420, const uint8_t *src_y, const uint8_t *src_uv,
    unsigned src_h, unsigned src_y_stride, unsigned src_uv_stride, int lv_cached)
{
    if (!dst_i420 || !src_y || !src_uv ||
        src_h < 2u || src_h > LS_MAX_DIM || (src_h & 1u) != 0u ||
        src_y_stride < LS_SRC_W || src_uv_stride < LS_SRC_W ||
        src_y_stride > LS_MAX_STRIDE || src_uv_stride > LS_MAX_STRIDE) {
        return 0;
    }

    uint8_t hbuf[2][LS_DST_W] __attribute__((aligned(16)));
    uint8_t stage[LS_DST_W]   __attribute__((aligned(16)));

    uint8_t *dst_y = dst_i420;
    uint8_t *dst_u = dst_i420 + (size_t)LS_DST_W * LS_DST_H;
    uint8_t *dst_v = dst_u + (size_t)LS_DST_CW * LS_DST_CH;

    /* --- Luma --- */
    int32_t row_idx[2] = {-1, -1};
    uint32_t y_num = (src_h - 1u) << 4;
    uint32_t y_den = LS_DST_H - 1u;
    uint32_t y_qs = y_num / y_den;
    uint32_t y_rs = y_num % y_den;
    uint32_t fy = 0u, y_rem = 0u;

    for (unsigned y = 0u; y < LS_DST_H; ++y) {
        unsigned iy = fy >> 4;
        unsigned wy = fy & 15u;
        unsigned iy1 = (iy + 1u < src_h) ? (iy + 1u) : iy;
        fy += y_qs;
        y_rem += y_rs;
        if (y_rem >= y_den) {
            y_rem -= y_den;
            fy++;
        }
        int miss = 0;
        unsigned s0 = ls_cache_slot(row_idx, iy, wy ? (int32_t)iy1 : -1, &miss);
        if (miss) {
            ls_hscale_row(hbuf[s0], src_y + (size_t)iy * src_y_stride, stage, 0, lv_cached);
        }
        unsigned s1 = s0;
        if (wy && iy1 != iy) {
            s1 = ls_cache_slot(row_idx, iy1, (int32_t)iy, &miss);
            if (miss) {
                ls_hscale_row(hbuf[s1], src_y + (size_t)iy1 * src_y_stride, stage, 0, lv_cached);
            }
        }
        ls_vblend_row(dst_y + (size_t)y * LS_DST_W, hbuf[s0], hbuf[s1], wy, stage);
    }

    /* --- Chroma: NV12 UV -> planar U & V --- */
    unsigned sh_c = src_h >> 1;
    row_idx[0] = -1;
    row_idx[1] = -1;
    uint32_t cy_num = (sh_c - 1u) << 4;
    uint32_t cy_den = LS_DST_CH - 1u;
    uint32_t cy_qs = cy_num / cy_den;
    uint32_t cy_rs = cy_num % cy_den;
    uint32_t cfy = 0u, cy_rem = 0u;
    int dst_uv_aligned = ((((uintptr_t)dst_u | (uintptr_t)dst_v) & 15u) == 0u);

    for (unsigned y = 0u; y < LS_DST_CH; ++y) {
        unsigned iy = cfy >> 4;
        unsigned wy = cfy & 15u;
        unsigned iy1 = (iy + 1u < sh_c) ? (iy + 1u) : iy;
        cfy += cy_qs;
        cy_rem += cy_rs;
        if (cy_rem >= cy_den) {
            cy_rem -= cy_den;
            cfy++;
        }
        int miss = 0;
        unsigned s0 = ls_cache_slot(row_idx, iy, wy ? (int32_t)iy1 : -1, &miss);
        if (miss) {
            ls_hscale_row(hbuf[s0], src_uv + (size_t)iy * src_uv_stride, stage, 1, lv_cached);
        }
        unsigned s1 = s0;
        if (wy && iy1 != iy) {
            s1 = ls_cache_slot(row_idx, iy1, (int32_t)iy, &miss);
            if (miss) {
                ls_hscale_row(hbuf[s1], src_uv + (size_t)iy1 * src_uv_stride, stage, 1, lv_cached);
            }
        }
        uint8_t *du = dst_u + (size_t)y * LS_DST_CW;
        uint8_t *dv = dst_v + (size_t)y * LS_DST_CW;
        if (dst_uv_aligned) {
            ls_vblend_1280_to_i420_uv_aligned(du, dv, hbuf[s0], hbuf[s1], wy);
        } else {
            ls_vblend_1280_to_i420_uv_aligned(stage, stage + LS_DST_CW, hbuf[s0], hbuf[s1], wy);
            ls_copy_bytes(du, stage, LS_DST_CW, 0);
            ls_copy_bytes(dv, stage + LS_DST_CW, LS_DST_CW, 0);
        }
    }
    return 1;
}

/* =========================================================================
 * Public API 3 & 3B: 1:1 NV12 Copy (+ optional fused 512x256 grain)
 * ========================================================================= */

int live_copy_nv12_neon_grain(
    uint8_t *dst_y, uint8_t *dst_uv, unsigned w, unsigned h,
    unsigned dst_y_stride, unsigned dst_uv_stride, unsigned coded_h,
    const uint8_t *src_y, const uint8_t *src_uv,
    unsigned src_y_stride, unsigned src_uv_stride,
    int lv_cached, int clean_dcache, const int8_t *grain_tile)
{
    if (!dst_y || !dst_uv || !src_y || !src_uv ||
        w < 2u || h < 2u || w > LS_MAX_DIM || h > LS_MAX_DIM || ((w | h) & 1u) != 0u ||
        coded_h < h || coded_h > LS_MAX_DIM || (coded_h & 1u) != 0u ||
        dst_y_stride < w || dst_uv_stride < w ||
        dst_y_stride > LS_MAX_STRIDE || dst_uv_stride > LS_MAX_STRIDE ||
        src_y_stride < w || src_uv_stride < w ||
        src_y_stride > LS_MAX_STRIDE || src_uv_stride > LS_MAX_STRIDE) {
        return 0;
    }

    unsigned strip0 = 0u;
    if (grain_tile) {
        uint8_t stage[LS_MAX_DIM] __attribute__((aligned(16)));
        for (unsigned y = 0u; y < h; ++y) {
            const uint8_t *sr = src_y + (size_t)y * src_y_stride;
            if (!lv_cached) {
                ls_copy_bytes(stage, sr, w, 0);
                sr = stage;
            }
            unsigned ty = y & (LS_GTILE_H - 1u);
            unsigned tx0 = ((y >> 8) * LS_GTILE_TWIST) & (LS_GTILE_W - 1u);
            ls_copy_grain_row(dst_y + (size_t)y * dst_y_stride, sr, w,
                              grain_tile + (size_t)ty * LS_GTILE_W, tx0);
            if (clean_dcache && ((y & 7u) == 7u)) {
                ls_clean_strip(dst_y, dst_y_stride, w, strip0, y + 1u);
                strip0 = y + 1u;
            }
        }
        const uint8_t *last_y = dst_y + (size_t)(h - 1u) * dst_y_stride;
        for (unsigned py = h; py < coded_h; ++py) {
            if (clean_dcache) {
                ls_copy_bytes_nt(dst_y + (size_t)py * dst_y_stride, last_y, w, 0);
            } else {
                ls_copy_bytes(dst_y + (size_t)py * dst_y_stride, last_y, w, 0);
            }
            if (clean_dcache && ((py & 7u) == 7u)) {
                ls_clean_strip(dst_y, dst_y_stride, w, strip0, py + 1u);
                strip0 = py + 1u;
            }
        }
    } else if (clean_dcache) {
        /* Wave5 DMA slot writes (clean_dcache != 0): non-temporal stnp + strip dc cvac */
        for (unsigned y = 0u; y < h; ++y) {
            ls_copy_bytes_nt(dst_y + (size_t)y * dst_y_stride,
                             src_y + (size_t)y * src_y_stride, w, lv_cached);
            if ((y & 7u) == 7u) {
                ls_clean_strip(dst_y, dst_y_stride, w, strip0, y + 1u);
                strip0 = y + 1u;
            }
        }
        const uint8_t *last_y = src_y + (size_t)(h - 1u) * src_y_stride;
        for (unsigned py = h; py < coded_h; ++py) {
            ls_copy_bytes_nt(dst_y + (size_t)py * dst_y_stride, last_y, w, lv_cached);
            if ((py & 7u) == 7u) {
                ls_clean_strip(dst_y, dst_y_stride, w, strip0, py + 1u);
                strip0 = py + 1u;
            }
        }
    } else {
        /* CPU0 -> CPU2 LBUF writes (clean_dcache == 0): normal allocating stp keeps L2 warm! */
        for (unsigned y = 0u; y < h; ++y) {
            ls_copy_bytes(dst_y + (size_t)y * dst_y_stride,
                          src_y + (size_t)y * src_y_stride, w, lv_cached);
        }
        const uint8_t *last_y = src_y + (size_t)(h - 1u) * src_y_stride;
        for (unsigned py = h; py < coded_h; ++py) {
            ls_copy_bytes(dst_y + (size_t)py * dst_y_stride, last_y, w, lv_cached);
        }
    }
    if (clean_dcache && strip0 < coded_h) {
        ls_clean_strip(dst_y, dst_y_stride, w, strip0, coded_h);
    }

    unsigned ch = h >> 1;
    unsigned coded_ch = coded_h >> 1;
    strip0 = 0u;
    if (clean_dcache) {
        for (unsigned y = 0u; y < ch; ++y) {
            ls_copy_bytes_nt(dst_uv + (size_t)y * dst_uv_stride,
                             src_uv + (size_t)y * src_uv_stride, w, lv_cached);
            if ((y & 7u) == 7u) {
                ls_clean_strip(dst_uv, dst_uv_stride, w, strip0, y + 1u);
                strip0 = y + 1u;
            }
        }
        const uint8_t *last_uv = src_uv + (size_t)(ch - 1u) * src_uv_stride;
        for (unsigned py = ch; py < coded_ch; ++py) {
            ls_copy_bytes_nt(dst_uv + (size_t)py * dst_uv_stride, last_uv, w, lv_cached);
            if ((py & 7u) == 7u) {
                ls_clean_strip(dst_uv, dst_uv_stride, w, strip0, py + 1u);
                strip0 = py + 1u;
            }
        }
        if (strip0 < coded_ch) {
            ls_clean_strip(dst_uv, dst_uv_stride, w, strip0, coded_ch);
        }
        ls_dsb_sy();
    } else {
        for (unsigned y = 0u; y < ch; ++y) {
            ls_copy_bytes(dst_uv + (size_t)y * dst_uv_stride,
                          src_uv + (size_t)y * src_uv_stride, w, lv_cached);
        }
        const uint8_t *last_uv = src_uv + (size_t)(ch - 1u) * src_uv_stride;
        for (unsigned py = ch; py < coded_ch; ++py) {
            ls_copy_bytes(dst_uv + (size_t)py * dst_uv_stride, last_uv, w, lv_cached);
        }
    }
    return 1;
}

int live_copy_nv12_neon(
    uint8_t *dst_y, uint8_t *dst_uv, unsigned w, unsigned h,
    unsigned dst_y_stride, unsigned dst_uv_stride, unsigned coded_h,
    const uint8_t *src_y, const uint8_t *src_uv,
    unsigned src_y_stride, unsigned src_uv_stride,
    int lv_cached, int clean_dcache)
{
    return live_copy_nv12_neon_grain(
        dst_y, dst_uv, w, h, dst_y_stride, dst_uv_stride, coded_h,
        src_y, src_uv, src_y_stride, src_uv_stride, lv_cached, clean_dcache, 0);
}

/* =========================================================================
 * Public API 4: Fast 1280x852 NV12 -> 1024x682 I420 Preview Downscaler
 * ========================================================================= */

#if LS_USE_NEON

/* 5:4 decimation (80 B -> 64 B): keep bytes [0,1,2,3], drop [4] per 5-byte group. */
static const uint8_t k_prev_y_idx[4][16] __attribute__((aligned(16))) = {
    { 0, 1, 2, 3,  5, 6, 7, 8, 10,11,12,13, 15,16,17,18},
    { 4, 5, 6, 7,  9,10,11,12, 14,15,16,17, 19,20,21,22},
    { 8, 9,10,11, 13,14,15,16, 18,19,20,21, 23,24,25,26},
    {12,13,14,15, 17,18,19,20, 22,23,24,25, 27,28,29,30},
};

/* Simultaneous 5:4 decimation + NV12->I420 U de-interleave inside 3-reg (48 B) TBL window.
 * V indices are simply u_idx + 1. */
static const uint8_t k_prev_u_idx[2][16] __attribute__((aligned(16))) = {
    { 0, 2, 4, 6, 10,12,14,16, 20,22,24,26, 30,32,34,36},
    { 8,10,12,14, 18,20,22,24, 28,30,32,34, 38,40,42,44},
};

static __attribute__((noinline)) void ls_prev_decim_y_1280_to_1024(
    uint8_t * __restrict dst, const uint8_t * __restrict src)
{
    uint8x16_t m0 = ls_ld16a(k_prev_y_idx[0]);
    uint8x16_t m1 = ls_ld16a(k_prev_y_idx[1]);
    uint8x16_t m2 = ls_ld16a(k_prev_y_idx[2]);
    uint8x16_t m3 = ls_ld16a(k_prev_y_idx[3]);
    LS_NO_UNROLL
    for (unsigned it = 0u; it < 16u; ++it) {
        const uint8_t *s = src + it * 80u;
        uint8_t *d = dst + it * 64u;
        uint8x16_t v0 = ls_ld16a(s +  0u);
        uint8x16_t v1 = ls_ld16a(s + 16u);
        uint8x16_t v2 = ls_ld16a(s + 32u);
        uint8x16_t v3 = ls_ld16a(s + 48u);
        uint8x16_t v4 = ls_ld16a(s + 64u);
        uint8x16x2_t w01 = {{v0, v1}};
        uint8x16x2_t w12 = {{v1, v2}};
        uint8x16x2_t w23 = {{v2, v3}};
        uint8x16x2_t w34 = {{v3, v4}};
        ls_st16a(d +  0u, vqtbl2q_u8(w01, m0));
        ls_st16a(d + 16u, vqtbl2q_u8(w12, m1));
        ls_st16a(d + 32u, vqtbl2q_u8(w23, m2));
        ls_st16a(d + 48u, vqtbl2q_u8(w34, m3));
    }
}

static __attribute__((noinline)) void ls_prev_decim_uv_1280_to_512(
    uint8_t * __restrict du, uint8_t * __restrict dv, const uint8_t * __restrict src_uv)
{
    uint8x16_t one = vdupq_n_u8(1u);
    uint8x16_t mu0 = ls_ld16a(k_prev_u_idx[0]);
    uint8x16_t mu1 = ls_ld16a(k_prev_u_idx[1]);
    uint8x16_t mv0 = vaddq_u8(mu0, one);
    uint8x16_t mv1 = vaddq_u8(mu1, one);
    LS_NO_UNROLL
    for (unsigned it = 0u; it < 16u; ++it) {
        const uint8_t *s = src_uv + it * 80u;
        uint8_t *u = du + it * 32u;
        uint8_t *v = dv + it * 32u;
        uint8x16_t v0 = ls_ld16a(s +  0u);
        uint8x16_t v1 = ls_ld16a(s + 16u);
        uint8x16_t v2 = ls_ld16a(s + 32u);
        uint8x16_t v3 = ls_ld16a(s + 48u);
        uint8x16_t v4 = ls_ld16a(s + 64u);
        uint8x16x3_t w012 = {{v0, v1, v2}};
        uint8x16x3_t w234 = {{v2, v3, v4}};
        ls_st16a(u +  0u, vqtbl3q_u8(w012, mu0));
        ls_st16a(v +  0u, vqtbl3q_u8(w012, mv0));
        ls_st16a(u + 16u, vqtbl3q_u8(w234, mu1));
        ls_st16a(v + 16u, vqtbl3q_u8(w234, mv1));
    }
}

#endif

void live_preview_nv12_to_i420_1024x680(
    uint8_t *dst_i420, const uint8_t *src_y, const uint8_t *src_uv,
    unsigned src_w, unsigned src_h, unsigned src_y_stride, unsigned src_uv_stride)
{
    if (!dst_i420 || !src_y || !src_uv ||
        src_w < 2u || src_h < 2u || src_w > LS_MAX_DIM || src_h > LS_MAX_DIM ||
        ((src_w | src_h) & 1u) != 0u ||
        src_y_stride < src_w || src_uv_stride < src_w ||
        src_y_stride > LS_MAX_STRIDE || src_uv_stride > LS_MAX_STRIDE) {
        return;
    }

    uint8_t *dst_y = dst_i420;
    uint8_t *dst_u = dst_i420 + (size_t)LS_PREV_W * LS_PREV_H;
    uint8_t *dst_v = dst_u + (size_t)LS_PREV_CW * LS_PREV_CH;

    if (src_w == LS_DST_W) {
        unsigned yq = src_h / LS_PREV_H, yr = src_h % LS_PREV_H, syi = 0u, rem = 0u;
#if LS_USE_NEON
        uint8_t srow[LS_DST_W] __attribute__((aligned(16)));
        uint8_t drow[LS_PREV_W] __attribute__((aligned(16)));
        for (unsigned y = 0u; y < LS_PREV_H; ++y) {
            const uint8_t *rp = src_y + (size_t)syi * src_y_stride;
            syi += yq;
            rem += yr;
            if (rem >= LS_PREV_H) {
                rem -= LS_PREV_H;
                syi++;
            }
            const uint8_t *sp = rp;
            if ((((uintptr_t)rp) & 15u) != 0u) {
                ls_copy_bytes(srow, rp, LS_DST_W, 0);
                sp = srow;
            }
            uint8_t *dp = dst_y + (size_t)y * LS_PREV_W;
            if ((((uintptr_t)dp) & 15u) == 0u) {
                ls_prev_decim_y_1280_to_1024(dp, sp);
            } else {
                ls_prev_decim_y_1280_to_1024(drow, sp);
                ls_copy_bytes(dp, drow, LS_PREV_W, 0);
            }
        }
        unsigned ch = src_h >> 1;
        unsigned cq = ch / LS_PREV_CH, cr = ch % LS_PREV_CH, cyi = 0u, crem = 0u;
        int dst_uv_aligned = ((((uintptr_t)dst_u | (uintptr_t)dst_v) & 15u) == 0u);
        for (unsigned y = 0u; y < LS_PREV_CH; ++y) {
            const uint8_t *rp = src_uv + (size_t)cyi * src_uv_stride;
            cyi += cq;
            crem += cr;
            if (crem >= LS_PREV_CH) {
                crem -= LS_PREV_CH;
                cyi++;
            }
            const uint8_t *sp = rp;
            if ((((uintptr_t)rp) & 15u) != 0u) {
                ls_copy_bytes(srow, rp, LS_DST_W, 0);
                sp = srow;
            }
            uint8_t *du = dst_u + (size_t)y * LS_PREV_CW;
            uint8_t *dv = dst_v + (size_t)y * LS_PREV_CW;
            if (dst_uv_aligned) {
                ls_prev_decim_uv_1280_to_512(du, dv, sp);
            } else {
                ls_prev_decim_uv_1280_to_512(drow, drow + LS_PREV_CW, sp);
                ls_copy_bytes(du, drow, LS_PREV_CW, 0);
                ls_copy_bytes(dv, drow + LS_PREV_CW, LS_PREV_CW, 0);
            }
        }
#else
        for (unsigned y = 0u; y < LS_PREV_H; ++y) {
            const uint8_t *rp = src_y + (size_t)syi * src_y_stride;
            syi += yq;
            rem += yr;
            if (rem >= LS_PREV_H) {
                rem -= LS_PREV_H;
                syi++;
            }
            uint8_t *dp = dst_y + (size_t)y * LS_PREV_W;
            for (unsigned k = 0u; k < 256u; ++k, rp += 5u, dp += 4u) {
                dp[0] = rp[0];
                dp[1] = rp[1];
                dp[2] = rp[2];
                dp[3] = rp[3];
            }
        }
        unsigned ch = src_h >> 1;
        unsigned cq = ch / LS_PREV_CH, cr = ch % LS_PREV_CH, cyi = 0u, crem = 0u;
        for (unsigned y = 0u; y < LS_PREV_CH; ++y) {
            const uint8_t *rp = src_uv + (size_t)cyi * src_uv_stride;
            cyi += cq;
            crem += cr;
            if (crem >= LS_PREV_CH) {
                crem -= LS_PREV_CH;
                cyi++;
            }
            uint8_t *du = dst_u + (size_t)y * LS_PREV_CW;
            uint8_t *dv = dst_v + (size_t)y * LS_PREV_CW;
            for (unsigned k = 0u; k < 128u; ++k, rp += 10u, du += 4u, dv += 4u) {
                du[0] = rp[0]; dv[0] = rp[1];
                du[1] = rp[2]; dv[1] = rp[3];
                du[2] = rp[4]; dv[2] = rp[5];
                du[3] = rp[6]; dv[3] = rp[7];
            }
        }
#endif
        return;
    }

    /* Generic nearest-neighbor NV12 -> 1024x682 I420 fallback (matching V69 preview_push). */
    for (unsigned y = 0u; y < LS_PREV_H; ++y) {
        const uint8_t *row = src_y + (size_t)((uint64_t)y * src_h / LS_PREV_H) * src_y_stride;
        uint8_t *dp = dst_y + (size_t)y * LS_PREV_W;
        LS_NO_VEC_UNROLL
        for (unsigned x = 0u; x < LS_PREV_W; ++x) {
            dp[x] = row[(uint64_t)x * src_w / LS_PREV_W];
        }
    }
    unsigned sw_c = src_w >> 1, sh_c = src_h >> 1;
    for (unsigned y = 0u; y < LS_PREV_CH; ++y) {
        const uint8_t *row = src_uv + (size_t)((uint64_t)y * sh_c / LS_PREV_CH) * src_uv_stride;
        uint8_t *du = dst_u + (size_t)y * LS_PREV_CW;
        uint8_t *dv = dst_v + (size_t)y * LS_PREV_CW;
        LS_NO_VEC_UNROLL
        for (unsigned x = 0u; x < LS_PREV_CW; ++x) {
            unsigned sx = (unsigned)((uint64_t)x * sw_c / LS_PREV_CW);
            du[x] = row[2u * sx + 0u];
            dv[x] = row[2u * sx + 1u];
        }
    }
}

/* =========================================================================
 * Public API 5: V71r3 Fast 5:4 Bilinear Downscaler (1280x852 -> 1024x682 NV12)
 *   - Horizontal: exact 1/8th weights [7/8,1/8], [5/8,3/8], [3/8,5/8], [1/8,7/8]
 *     via paired 3-reg TBL (s0/s4, s1/s3, s2/s2) + exact uhadd/uhadd/urhadd cascade
 *     (0-diff bit-exact to (7*a+b+4)>>3 and (5*a+3*b+4)>>3!) + trn.2d + st4
 *     (2x unrolled, ZERO multiplies, ZERO widening, ZERO spills, ZERO stack!)
 *   - Vertical: exact 1/16th uhadd-inner + urhadd-outer cascade (0-diff bit-exact to
 *     ((16-wy)*r0+wy*r1+8)>>4!) with normal allocating stp (keeps 1.05 MB LBUF[wslot]
 *     warm in shared L2 for CPU2; ZERO stnp, ZERO spills, ZERO stack!)
 *   - <= 950K AArch64 insns (~38% faster than V71r2 1.44M), 0-diff NEON == Scalar!
 * ========================================================================= */

#if LS_USE_NEON

/* Compact 3-vector 3-reg TBL masks (48 B per plane) gathering paired 7/8 & 5/8 operands
 * from 40 input bytes (bytes 0..39 in {v0,v1,v2}); second 40B half adds +8 in-register!
 *   mA = [s0[0..7], s4[0..7]]   (7/8 high-weight lane)
 *   mB = [s1[0..7], s3[0..7]]   (shared: 1/8 low-weight lane AND 5/8 high-weight lane!)
 *   mC = [s2[0..7], s2[0..7]]   (3/8 low-weight lane) */
static const uint8_t k_down_y_m[3][16] __attribute__((aligned(16))) = {
    {  0,  5, 10, 15, 20, 25, 30, 35,   4,  9, 14, 19, 24, 29, 34, 39},
    {  1,  6, 11, 16, 21, 26, 31, 36,   3,  8, 13, 18, 23, 28, 33, 38},
    {  2,  7, 12, 17, 22, 27, 32, 37,   2,  7, 12, 17, 22, 27, 32, 37},
};

static const uint8_t k_down_uv_m[3][16] __attribute__((aligned(16))) = {
    {  0,  1, 10, 11, 20, 21, 30, 31,   8,  9, 18, 19, 28, 29, 38, 39},
    {  2,  3, 12, 13, 22, 23, 32, 33,   6,  7, 16, 17, 26, 27, 36, 37},
    {  4,  5, 14, 15, 24, 25, 34, 35,   4,  5, 14, 15, 24, 25, 34, 35},
};

#define LS_HDOWN_ASM(ST4_SPEC) \
    __asm__ volatile( \
        "ldp    q18, q19, [%[m]]\n\t" \
        "ldr    q20, [%[m], #32]\n\t" \
        "movi   v29.16b, #8\n\t" \
        "add    v21.16b, v18.16b, v29.16b\n\t" \
        "add    v22.16b, v19.16b, v29.16b\n\t" \
        "add    v23.16b, v20.16b, v29.16b\n\t" \
        "1:\n\t" \
        "ldp    q2,  q3,  [%[s], #32]\n\t" \
        "ldp    q4,  q24, [%[s], #64]\n\t" \
        "ldp    q0,  q1,  [%[s]], #160\n\t" \
        "tbl    v5.16b,  {v0.16b, v1.16b, v2.16b}, v18.16b\n\t" \
        "tbl    v6.16b,  {v0.16b, v1.16b, v2.16b}, v19.16b\n\t" \
        "tbl    v7.16b,  {v0.16b, v1.16b, v2.16b}, v20.16b\n\t" \
        "uhadd  v16.16b, v5.16b,  v6.16b\n\t" \
        "uhadd  v16.16b, v5.16b,  v16.16b\n\t" \
        "urhadd v5.16b,  v5.16b,  v16.16b\n\t" \
        "uhadd  v16.16b, v6.16b,  v7.16b\n\t" \
        "uhadd  v16.16b, v7.16b,  v16.16b\n\t" \
        "urhadd v6.16b,  v6.16b,  v16.16b\n\t" \
        "tbl    v7.16b,  {v2.16b, v3.16b, v4.16b}, v21.16b\n\t" \
        "tbl    v16.16b, {v2.16b, v3.16b, v4.16b}, v22.16b\n\t" \
        "tbl    v17.16b, {v2.16b, v3.16b, v4.16b}, v23.16b\n\t" \
        "uhadd  v29.16b, v7.16b,  v16.16b\n\t" \
        "uhadd  v29.16b, v7.16b,  v29.16b\n\t" \
        "urhadd v7.16b,  v7.16b,  v29.16b\n\t" \
        "uhadd  v29.16b, v16.16b, v17.16b\n\t" \
        "uhadd  v29.16b, v17.16b, v29.16b\n\t" \
        "urhadd v16.16b, v16.16b, v29.16b\n\t" \
        "trn1   v0.2d,   v5.2d,   v7.2d\n\t" \
        "trn1   v1.2d,   v6.2d,   v16.2d\n\t" \
        "trn2   v2.2d,   v6.2d,   v16.2d\n\t" \
        "trn2   v3.2d,   v5.2d,   v7.2d\n\t" \
        "st4    " ST4_SPEC ", [%[d]], #64\n\t" \
        "ldp    q25, q26, [%[s], #-64]\n\t" \
        "ldp    q27, q28, [%[s], #-32]\n\t" \
        "tbl    v5.16b,  {v24.16b, v25.16b, v26.16b}, v18.16b\n\t" \
        "tbl    v6.16b,  {v24.16b, v25.16b, v26.16b}, v19.16b\n\t" \
        "tbl    v7.16b,  {v24.16b, v25.16b, v26.16b}, v20.16b\n\t" \
        "uhadd  v16.16b, v5.16b,  v6.16b\n\t" \
        "uhadd  v16.16b, v5.16b,  v16.16b\n\t" \
        "urhadd v5.16b,  v5.16b,  v16.16b\n\t" \
        "uhadd  v16.16b, v6.16b,  v7.16b\n\t" \
        "uhadd  v16.16b, v7.16b,  v16.16b\n\t" \
        "urhadd v6.16b,  v6.16b,  v16.16b\n\t" \
        "tbl    v7.16b,  {v26.16b, v27.16b, v28.16b}, v21.16b\n\t" \
        "tbl    v16.16b, {v26.16b, v27.16b, v28.16b}, v22.16b\n\t" \
        "tbl    v17.16b, {v26.16b, v27.16b, v28.16b}, v23.16b\n\t" \
        "uhadd  v29.16b, v7.16b,  v16.16b\n\t" \
        "uhadd  v29.16b, v7.16b,  v29.16b\n\t" \
        "urhadd v7.16b,  v7.16b,  v29.16b\n\t" \
        "uhadd  v29.16b, v16.16b, v17.16b\n\t" \
        "uhadd  v29.16b, v17.16b, v29.16b\n\t" \
        "urhadd v16.16b, v16.16b, v29.16b\n\t" \
        "trn1   v0.2d,   v5.2d,   v7.2d\n\t" \
        "trn1   v1.2d,   v6.2d,   v16.2d\n\t" \
        "trn2   v2.2d,   v6.2d,   v16.2d\n\t" \
        "trn2   v3.2d,   v5.2d,   v7.2d\n\t" \
        "st4    " ST4_SPEC ", [%[d]], #64\n\t" \
        "subs   %[cnt], %[cnt], #1\n\t" \
        "b.ne   1b" \
        : [d] "+r"(d), [s] "+r"(s), [cnt] "+r"(cnt) \
        : [m] "r"(m) \
        : "cc", "memory", \
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", \
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", \
          "v24", "v25", "v26", "v27", "v28", "v29")

/* Precondition: dst (1024 B) and src (1280 B) are 16-byte aligned.
 * Pure 3-reg tbl + exact 1/8th uhadd/urhadd + trn.2d + st4: 53 insns/128B, 0 stack, 0 Q-spills! */
static __attribute__((noinline)) void ls_hdown_1280_1024_aligned(
    uint8_t * __restrict dst, const uint8_t * __restrict src, int is_uv)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    uint64_t cnt = 8u;
    if (!is_uv) {
        const uint8_t *m = k_down_y_m[0];
        LS_HDOWN_ASM("{v0.16b, v1.16b, v2.16b, v3.16b}");
        return;
    }
    const uint8_t *m = k_down_uv_m[0];
    LS_HDOWN_ASM("{v0.8h, v1.8h, v2.8h, v3.8h}");
}

/* Exact 1/16th uhadd-inner + urhadd-outer cascades: bit-identical to ((16-w)*a + w*b + 8) >> 4
 * for EVERY (a, b) in [0..255]^2! Post-index ldp/stp 128B/iter inline asm: ZERO add insns,
 * ZERO stnp (keeps 1.05 MB LBUF[wslot] warm in shared L2 for CPU2), ZERO stack, ZERO Q-spills! */
#define LS_VB16_W1 \
    "uhadd  v4.16b,  v0.16b, v4.16b\n\t"  "uhadd  v5.16b,  v1.16b, v5.16b\n\t" \
    "uhadd  v6.16b,  v2.16b, v6.16b\n\t"  "uhadd  v7.16b,  v3.16b, v7.16b\n\t" \
    "uhadd  v4.16b,  v0.16b, v4.16b\n\t"  "uhadd  v5.16b,  v1.16b, v5.16b\n\t" \
    "uhadd  v6.16b,  v2.16b, v6.16b\n\t"  "uhadd  v7.16b,  v3.16b, v7.16b\n\t" \
    "uhadd  v4.16b,  v0.16b, v4.16b\n\t"  "uhadd  v5.16b,  v1.16b, v5.16b\n\t" \
    "uhadd  v6.16b,  v2.16b, v6.16b\n\t"  "uhadd  v7.16b,  v3.16b, v7.16b\n\t" \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VB16_W2 \
    "uhadd  v4.16b,  v0.16b, v4.16b\n\t"  "uhadd  v5.16b,  v1.16b, v5.16b\n\t" \
    "uhadd  v6.16b,  v2.16b, v6.16b\n\t"  "uhadd  v7.16b,  v3.16b, v7.16b\n\t" \
    "uhadd  v4.16b,  v0.16b, v4.16b\n\t"  "uhadd  v5.16b,  v1.16b, v5.16b\n\t" \
    "uhadd  v6.16b,  v2.16b, v6.16b\n\t"  "uhadd  v7.16b,  v3.16b, v7.16b\n\t" \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VB16_W3 \
    "uhadd  v16.16b, v0.16b, v4.16b\n\t"  "uhadd  v17.16b, v1.16b, v5.16b\n\t" \
    "uhadd  v18.16b, v2.16b, v6.16b\n\t"  "uhadd  v19.16b, v3.16b, v7.16b\n\t" \
    "uhadd  v4.16b,  v4.16b, v16.16b\n\t" "uhadd  v5.16b,  v5.16b, v17.16b\n\t" \
    "uhadd  v6.16b,  v6.16b, v18.16b\n\t" "uhadd  v7.16b,  v7.16b, v19.16b\n\t" \
    "uhadd  v4.16b,  v0.16b, v4.16b\n\t"  "uhadd  v5.16b,  v1.16b, v5.16b\n\t" \
    "uhadd  v6.16b,  v2.16b, v6.16b\n\t"  "uhadd  v7.16b,  v3.16b, v7.16b\n\t" \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VB16_W4 \
    "uhadd  v4.16b,  v0.16b, v4.16b\n\t"  "uhadd  v5.16b,  v1.16b, v5.16b\n\t" \
    "uhadd  v6.16b,  v2.16b, v6.16b\n\t"  "uhadd  v7.16b,  v3.16b, v7.16b\n\t" \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VB16_W5 \
    "uhadd  v16.16b, v0.16b, v4.16b\n\t"  "uhadd  v17.16b, v1.16b, v5.16b\n\t" \
    "uhadd  v18.16b, v2.16b, v6.16b\n\t"  "uhadd  v19.16b, v3.16b, v7.16b\n\t" \
    "uhadd  v16.16b, v0.16b, v16.16b\n\t" "uhadd  v17.16b, v1.16b, v17.16b\n\t" \
    "uhadd  v18.16b, v2.16b, v18.16b\n\t" "uhadd  v19.16b, v3.16b, v19.16b\n\t" \
    "uhadd  v4.16b,  v4.16b, v16.16b\n\t" "uhadd  v5.16b,  v5.16b, v17.16b\n\t" \
    "uhadd  v6.16b,  v6.16b, v18.16b\n\t" "uhadd  v7.16b,  v7.16b, v19.16b\n\t" \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VB16_W6 \
    "uhadd  v16.16b, v0.16b, v4.16b\n\t"  "uhadd  v17.16b, v1.16b, v5.16b\n\t" \
    "uhadd  v18.16b, v2.16b, v6.16b\n\t"  "uhadd  v19.16b, v3.16b, v7.16b\n\t" \
    "uhadd  v4.16b,  v4.16b, v16.16b\n\t" "uhadd  v5.16b,  v5.16b, v17.16b\n\t" \
    "uhadd  v6.16b,  v6.16b, v18.16b\n\t" "uhadd  v7.16b,  v7.16b, v19.16b\n\t" \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VB16_W7 \
    "uhadd  v16.16b, v0.16b, v4.16b\n\t"  "uhadd  v17.16b, v1.16b, v5.16b\n\t" \
    "uhadd  v18.16b, v2.16b, v6.16b\n\t"  "uhadd  v19.16b, v3.16b, v7.16b\n\t" \
    "uhadd  v16.16b, v4.16b, v16.16b\n\t" "uhadd  v17.16b, v5.16b, v17.16b\n\t" \
    "uhadd  v18.16b, v6.16b, v18.16b\n\t" "uhadd  v19.16b, v7.16b, v19.16b\n\t" \
    "uhadd  v4.16b,  v4.16b, v16.16b\n\t" "uhadd  v5.16b,  v5.16b, v17.16b\n\t" \
    "uhadd  v6.16b,  v6.16b, v18.16b\n\t" "uhadd  v7.16b,  v7.16b, v19.16b\n\t" \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VB16_W8 \
    "urhadd v0.16b,  v0.16b, v4.16b\n\t"  "urhadd v1.16b,  v1.16b, v5.16b\n\t" \
    "urhadd v2.16b,  v2.16b, v6.16b\n\t"  "urhadd v3.16b,  v3.16b, v7.16b\n\t"

#define LS_VBLEND_1024_STP_CASE(OP_ASM) do { \
    const uint8_t *p0 = r0, *p1 = r1; \
    uint8_t *pd = dst; \
    uint64_t cnt = 8u; \
    __asm__ volatile( \
        "1:\n\t" \
        "ld1    {v0.16b, v1.16b, v2.16b, v3.16b}, [%[p0]], #64\n\t" \
        "ld1    {v4.16b, v5.16b, v6.16b, v7.16b}, [%[p1]], #64\n\t" \
        OP_ASM \
        "st1    {v0.16b, v1.16b, v2.16b, v3.16b}, [%[pd]], #64\n\t" \
        "ld1    {v0.16b, v1.16b, v2.16b, v3.16b}, [%[p0]], #64\n\t" \
        "ld1    {v4.16b, v5.16b, v6.16b, v7.16b}, [%[p1]], #64\n\t" \
        OP_ASM \
        "st1    {v0.16b, v1.16b, v2.16b, v3.16b}, [%[pd]], #64\n\t" \
        "subs   %[cnt], %[cnt], #1\n\t" \
        "b.ne   1b" \
        : [pd] "+r"(pd), [p0] "+r"(p0), [p1] "+r"(p1), [cnt] "+r"(cnt) \
        : \
        : "cc", "memory", \
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", \
          "v16", "v17", "v18", "v19"); \
} while (0)

/* Precondition: dst, r0, r1 (1024 B) are 16-byte aligned; wy in 0..15. */
static __attribute__((noinline)) void ls_vblend_1024_16_stp(
    uint8_t * __restrict dst, const uint8_t * __restrict r0,
    const uint8_t * __restrict r1, unsigned wy)
{
    if (wy > 8u) {
        const uint8_t *tmp = r0;
        r0 = r1;
        r1 = tmp;
        wy = 16u - wy;
    }
    switch (wy) {
    case 0u: {
        const uint8_t *p0 = r0;
        uint8_t *pd = dst;
        uint64_t cnt = 8u;
        __asm__ volatile(
            "1:\n\t"
            "ld1    {v0.16b, v1.16b, v2.16b, v3.16b}, [%[p0]], #64\n\t"
            "ld1    {v4.16b, v5.16b, v6.16b, v7.16b}, [%[p0]], #64\n\t"
            "st1    {v0.16b, v1.16b, v2.16b, v3.16b}, [%[pd]], #64\n\t"
            "st1    {v4.16b, v5.16b, v6.16b, v7.16b}, [%[pd]], #64\n\t"
            "subs   %[cnt], %[cnt], #1\n\t"
            "b.ne   1b"
            : [pd] "+r"(pd), [p0] "+r"(p0), [cnt] "+r"(cnt)
            :
            : "cc", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
        return;
    }
    case 8u: LS_VBLEND_1024_STP_CASE(LS_VB16_W8); return;
    case 4u: LS_VBLEND_1024_STP_CASE(LS_VB16_W4); return;
    case 2u: LS_VBLEND_1024_STP_CASE(LS_VB16_W2); return;
    case 6u: LS_VBLEND_1024_STP_CASE(LS_VB16_W6); return;
    case 1u: LS_VBLEND_1024_STP_CASE(LS_VB16_W1); return;
    case 3u: LS_VBLEND_1024_STP_CASE(LS_VB16_W3); return;
    case 5u: LS_VBLEND_1024_STP_CASE(LS_VB16_W5); return;
    default: LS_VBLEND_1024_STP_CASE(LS_VB16_W7); return;
    }
}

#else

static void ls_hdown_1280_1024_aligned(
    uint8_t * __restrict dst, const uint8_t * __restrict src, int is_uv)
{
    if (!is_uv) {
        for (unsigned g = 0u; g < 256u; ++g) {
            unsigned s0 = src[5u * g + 0u];
            unsigned s1 = src[5u * g + 1u];
            unsigned s2 = src[5u * g + 2u];
            unsigned s3 = src[5u * g + 3u];
            unsigned s4 = src[5u * g + 4u];
            uint8_t *o = dst + 4u * g;
            o[0] = (uint8_t)((7u * s0 + 1u * s1 + 4u) >> 3);
            o[1] = (uint8_t)((5u * s1 + 3u * s2 + 4u) >> 3);
            o[2] = (uint8_t)((3u * s2 + 5u * s3 + 4u) >> 3);
            o[3] = (uint8_t)((1u * s3 + 7u * s4 + 4u) >> 3);
        }
        return;
    }
    for (unsigned g = 0u; g < 128u; ++g) {
        const uint8_t *s = src + 10u * g;
        uint8_t *o = dst + 8u * g;
        unsigned u0 = s[0], v0 = s[1];
        unsigned u1 = s[2], v1 = s[3];
        unsigned u2 = s[4], v2 = s[5];
        unsigned u3 = s[6], v3 = s[7];
        unsigned u4 = s[8], v4 = s[9];
        o[0] = (uint8_t)((7u * u0 + 1u * u1 + 4u) >> 3);
        o[1] = (uint8_t)((7u * v0 + 1u * v1 + 4u) >> 3);
        o[2] = (uint8_t)((5u * u1 + 3u * u2 + 4u) >> 3);
        o[3] = (uint8_t)((5u * v1 + 3u * v2 + 4u) >> 3);
        o[4] = (uint8_t)((3u * u2 + 5u * u3 + 4u) >> 3);
        o[5] = (uint8_t)((3u * v2 + 5u * v3 + 4u) >> 3);
        o[6] = (uint8_t)((1u * u3 + 7u * u4 + 4u) >> 3);
        o[7] = (uint8_t)((1u * v3 + 7u * v4 + 4u) >> 3);
    }
}

static void ls_vblend_1024_16_stp(
    uint8_t * __restrict dst, const uint8_t * __restrict r0,
    const uint8_t * __restrict r1, unsigned wy)
{
    if (wy == 0u) {
        ls_copy_bytes(dst, r0, LS_PREV_W, 0);
        return;
    }
    unsigned w0 = 16u - wy;
    for (unsigned i = 0u; i < LS_PREV_W; ++i) {
        dst[i] = (uint8_t)((w0 * r0[i] + wy * r1[i] + 8u) >> 4);
    }
}

#endif

static inline void ls_hdown_row(uint8_t *dst_1024_aligned, const uint8_t *src_1280,
                                uint8_t *stage_1280, int is_uv)
{
    const uint8_t *p = src_1280;
    if ((((uintptr_t)src_1280) & 15u) != 0u) {
        ls_copy_bytes(stage_1280, src_1280, LS_DST_W, 0);
        p = stage_1280;
    }
    ls_hdown_1280_1024_aligned(dst_1024_aligned, p, is_uv);
}

static inline void ls_vblend_down_row(uint8_t *dst_1024, const uint8_t *r0,
                                      const uint8_t *r1, unsigned wy, uint8_t *stage_1024)
{
    if ((((uintptr_t)dst_1024) & 15u) == 0u) {
        ls_vblend_1024_16_stp(dst_1024, r0, r1, wy);
    } else {
        ls_vblend_1024_16_stp(stage_1024, r0, r1, wy);
        ls_copy_bytes(dst_1024, stage_1024, LS_PREV_W, 0);
    }
}

void live_downscale_1280_to_1024_nv12_neon(
    const uint8_t *src_y, const uint8_t *src_uv, uint32_t src_stride,
    uint8_t *dst_y, uint8_t *dst_uv, uint32_t dst_stride)
{
    if (!src_y || !src_uv || !dst_y || !dst_uv ||
        src_stride < LS_DST_W || dst_stride < LS_PREV_W ||
        src_stride > LS_MAX_STRIDE || dst_stride > LS_MAX_STRIDE) {
        return;
    }

    uint8_t hbuf[2][LS_PREV_W] __attribute__((aligned(16)));
    uint8_t stage[LS_DST_W]    __attribute__((aligned(16)));
    int src_aln = ((((uintptr_t)src_y | (uintptr_t)src_uv | (uintptr_t)src_stride) & 15u) == 0u);
    int dst_aln = ((((uintptr_t)dst_y | (uintptr_t)dst_uv | (uintptr_t)dst_stride) & 15u) == 0u);

    /* --- Luma: 1280x852 -> 1024x682 (pixel-center 1/16th vertical Bresenham; iy in 0..850, step 1..2) --- */
    const uint32_t y_num  = LS_DST_H << 4;
    const uint32_t y_den  = LS_PREV_H;
    const uint32_t y_qs   = y_num / y_den;
    const uint32_t y_rs   = y_num % y_den;
    const uint32_t y_init = ((LS_DST_H - LS_PREV_H) << 3) + (y_den >> 1);
    uint32_t fy    = y_init / y_den;
    uint32_t y_rem = y_init % y_den;
    int32_t cached_iy = -1;
    unsigned cur = 0u;
    uint8_t *dy = dst_y;

    for (unsigned y = 0u; y < LS_PREV_H; ++y, dy += dst_stride) {
        unsigned iy = fy >> 4;
        unsigned wy = fy & 15u;
        fy += y_qs;
        y_rem += y_rs;
        if (y_rem >= y_den) {
            y_rem -= y_den;
            fy++;
        }
        unsigned s0;
        if ((int32_t)iy == cached_iy) {
            s0 = cur;
        } else {
            s0 = 0u;
            cur = 0u;
            const uint8_t *sr0 = src_y + (size_t)iy * src_stride;
            if (src_aln) {
                ls_hdown_1280_1024_aligned(hbuf[0], sr0, 0);
            } else {
                ls_hdown_row(hbuf[0], sr0, stage, 0);
            }
        }
        unsigned s1 = s0;
        if (wy) {
            s1 = s0 ^ 1u;
            cur = s1;
            cached_iy = (int32_t)(iy + 1u);
            const uint8_t *sr1 = src_y + (size_t)(iy + 1u) * src_stride;
            if (src_aln) {
                ls_hdown_1280_1024_aligned(hbuf[s1], sr1, 0);
            } else {
                ls_hdown_row(hbuf[s1], sr1, stage, 0);
            }
        } else {
            cached_iy = (int32_t)iy;
        }
        if (dst_aln) {
            ls_vblend_1024_16_stp(dy, hbuf[s0], hbuf[s1], wy);
        } else {
            ls_vblend_down_row(dy, hbuf[s0], hbuf[s1], wy, stage);
        }
    }

    /* --- Interleaved NV12 UV: 640x426 pairs (1280x426 B) -> 512x341 pairs (1024x341 B) --- */
    const uint32_t cy_num  = LS_DST_CH << 4;
    const uint32_t cy_den  = LS_PREV_CH;
    const uint32_t cy_qs   = cy_num / cy_den;
    const uint32_t cy_rs   = cy_num % cy_den;
    const uint32_t cy_init = ((LS_DST_CH - LS_PREV_CH) << 3) + (cy_den >> 1);
    uint32_t cfy    = cy_init / cy_den;
    uint32_t cy_rem = cy_init % cy_den;
    cached_iy = -1;
    cur = 0u;
    uint8_t *duv = dst_uv;

    for (unsigned y = 0u; y < LS_PREV_CH; ++y, duv += dst_stride) {
        unsigned iy = cfy >> 4;
        unsigned wy = cfy & 15u;
        cfy += cy_qs;
        cy_rem += cy_rs;
        if (cy_rem >= cy_den) {
            cy_rem -= cy_den;
            cfy++;
        }
        unsigned s0;
        if ((int32_t)iy == cached_iy) {
            s0 = cur;
        } else {
            s0 = 0u;
            cur = 0u;
            const uint8_t *sr0 = src_uv + (size_t)iy * src_stride;
            if (src_aln) {
                ls_hdown_1280_1024_aligned(hbuf[0], sr0, 1);
            } else {
                ls_hdown_row(hbuf[0], sr0, stage, 1);
            }
        }
        unsigned s1 = s0;
        if (wy) {
            s1 = s0 ^ 1u;
            cur = s1;
            cached_iy = (int32_t)(iy + 1u);
            const uint8_t *sr1 = src_uv + (size_t)(iy + 1u) * src_stride;
            if (src_aln) {
                ls_hdown_1280_1024_aligned(hbuf[s1], sr1, 1);
            } else {
                ls_hdown_row(hbuf[s1], sr1, stage, 1);
            }
        } else {
            cached_iy = (int32_t)iy;
        }
        if (dst_aln) {
            ls_vblend_1024_16_stp(duv, hbuf[s0], hbuf[s1], wy);
        } else {
            ls_vblend_down_row(duv, hbuf[s0], hbuf[s1], wy, stage);
        }
    }
}
