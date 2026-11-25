/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 NV12 (4:2:0) 4 MiB Tagged YCbCr Cache AArch64 NEON Frame Kernel.
 * Adapted from Fenestra-Z8-C311-source/modules/lut_preview/layer0_color_neon.c.
 *
 * ONE OF ONLY TWO NEON-ENABLED COMPILATION UNITS in work/lut_core/
 * (the other is cube_frame_neon.c). Gated by v71r7_neon_sentinel_is_allowed().
 * Zero .data/.bss.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "layer0_color.h"

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

static inline uint32x4_t gather_u32(const uint32_t *cache, uint32x4_t index)
{
    uint32x4_t entries = vdupq_n_u32(__atomic_load_n(&cache[vgetq_lane_u32(index, 0)], __ATOMIC_RELAXED));
    entries = vsetq_lane_u32(__atomic_load_n(&cache[vgetq_lane_u32(index, 1)], __ATOMIC_RELAXED), entries, 1);
    entries = vsetq_lane_u32(__atomic_load_n(&cache[vgetq_lane_u32(index, 2)], __ATOMIC_RELAXED), entries, 2);
    return vsetq_lane_u32(__atomic_load_n(&cache[vgetq_lane_u32(index, 3)], __ATOMIC_RELAXED), entries, 3);
}

static inline uint8x16x2_t lookup_8(const uint32_t *cache, uint8x8_t luma, uint16x4x2_t pages)
{
    uint16x8_t y = vmovl_u8(luma);
    uint32x4_t lo = gather_u32(cache, vaddw_u16(vshll_n_u16(pages.val[0], 8), vget_low_u16(y)));
    uint32x4_t hi = gather_u32(cache, vaddw_high_u16(vshll_n_u16(pages.val[1], 8), y));
    return (uint8x16x2_t){{ vreinterpretq_u8_u32(lo), vreinterpretq_u8_u32(hi) }};
}

__attribute__((noinline))
void layer0_grade_nv12_planes_neon(const JpegCube *cube, uint32_t *cache,
                                   const Layer0Frame *frame,
                                   unsigned first_row, unsigned end_row)
{
    if (!cube || !cache || !frame || !frame->src_y || !frame->src_uv ||
        !frame->dst_y || !frame->dst_uv) {
        return;
    }
    static const uint8_t k_tag_bytes[8]  = { 3u, 7u, 11u, 15u, 19u, 23u, 27u, 31u };
    static const uint8_t k_y_bytes_at[8] = { 0u, 4u,  8u, 12u, 16u, 20u, 24u, 28u };
    static const uint8_t k_uv_even[8]    = { 1u, 2u,  9u, 10u, 17u, 18u, 25u, 26u };
    static const uint8_t k_uv_odd[8]     = { 5u, 6u, 13u, 14u, 21u, 22u, 29u, 30u };
    const uint8x8_t tag_bytes  = vld1_u8(k_tag_bytes);
    const uint8x8_t y_bytes_at = vld1_u8(k_y_bytes_at);
    const uint8x8_t uv_even    = vld1_u8(k_uv_even);
    const uint8x8_t uv_odd     = vld1_u8(k_uv_odd);

    unsigned width = frame->width & ~1u;
    size_t sy_stride  = frame->src_y_stride  ? frame->src_y_stride  : frame->width;
    size_t suv_stride = frame->src_uv_stride ? frame->src_uv_stride : frame->width;
    size_t dy_stride  = frame->dst_y_stride  ? frame->dst_y_stride  : frame->width;
    size_t duv_stride = frame->dst_uv_stride ? frame->dst_uv_stride : frame->width;
    if (end_row > frame->height) {
        end_row = frame->height;
    }
    end_row &= ~1u;

    for (unsigned row = first_row & ~1u; row < end_row; row += 2u) {
        const uint8_t *sy0 = frame->src_y  + (size_t)row * sy_stride;
        const uint8_t *sy1 = sy0 + sy_stride;
        const uint8_t *suv = frame->src_uv + (size_t)(row >> 1) * suv_stride;
        uint8_t       *dy0 = frame->dst_y  + (size_t)row * dy_stride;
        uint8_t       *dy1 = dy0 + dy_stride;
        uint8_t       *duv = frame->dst_uv + (size_t)(row >> 1) * duv_stride;

        unsigned x = 0u;
        for (; x < (width & ~7u); x += 8u) {
            if ((x & 63u) == 0u && width - x >= 192u) {
                __builtin_prefetch(sy0 + x + 128u);
                __builtin_prefetch(sy1 + x + 128u);
                __builtin_prefetch(suv + x + 128u);
            }
            uint16x4_t uv = vreinterpret_u16_u8(load_bytes(suv + x));
            uint16x4_t group = vorr_u16(vand_u16(uv, vdup_n_u16(63u)),
                                        vand_u16(vshr_n_u16(uv, 2), vdup_n_u16(63u << 6)));
            uint16x4x2_t pages = vzip_u16(group, group);
            uint8x16x2_t top    = lookup_8(cache, load_bytes(sy0 + x), pages);
            uint8x16x2_t bottom = lookup_8(cache, load_bytes(sy1 + x), pages);
            uint16x4_t tag = vadd_u16(
                vorr_u16(vand_u16(vshr_n_u16(uv, 6), vdup_n_u16(3u)),
                         vand_u16(vshr_n_u16(uv, 12), vdup_n_u16(12u))),
                vdup_n_u16(1u));
            uint8x8_t expected = vreinterpret_u8_u16(vsli_n_u16(tag, tag, 8));
            uint8x8_t wrong = vorr_u8(veor_u8(vqtbl2_u8(top, tag_bytes), expected),
                                      veor_u8(vqtbl2_u8(bottom, tag_bytes), expected));
            if (__builtin_expect(vget_lane_u64(vreinterpret_u64_u8(wrong), 0) != 0u, 0)) {
                layer0_grade_nv12_pair_scalar(cube, cache, sy0 + x, sy1 + x, suv + x,
                                              dy0 + x, dy1 + x, duv + x, 8u);
                continue;
            }
            uint16x8_t sum = vaddl_u8(vqtbl2_u8(top, uv_even), vqtbl2_u8(top, uv_odd));
            sum = vaddw_u8(sum, vqtbl2_u8(bottom, uv_even));
            sum = vaddw_u8(sum, vqtbl2_u8(bottom, uv_odd));
            store_bytes(dy0 + x, vqtbl2_u8(top, y_bytes_at));
            store_bytes(dy1 + x, vqtbl2_u8(bottom, y_bytes_at));
            store_bytes(duv + x, vrshrn_n_u16(sum, 2));
        }
        if (x < width) {
            layer0_grade_nv12_pair_scalar(cube, cache, sy0 + x, sy1 + x, suv + x,
                                          dy0 + x, dy1 + x, duv + x, width - x);
        }
    }
}

/* ---- V71.7-r7 528 KiB L2-Resident 64x64x33 Baked YCbCr 3D LUT (AArch64 NEON, 0 Q-spills) ---- */

static inline uint8x16_t baked_lerp4_neon(const uint32_t *c0, const uint32_t *c1,
                                          uint32_t y0, uint32_t y1,
                                          uint32_t y2, uint32_t y3)
{
    uint32_t k0 = y0 >> 3, k1 = y1 >> 3, k2 = y2 >> 3, k3 = y3 >> 3;
    uint32x4_t lo = vdupq_n_u32(c0[k0]);
    lo = vsetq_lane_u32(c0[k1], lo, 1);
    lo = vsetq_lane_u32(c1[k2], lo, 2);
    lo = vsetq_lane_u32(c1[k3], lo, 3);

    uint32x4_t hi = vdupq_n_u32(c0[k0 + 1u]);
    hi = vsetq_lane_u32(c0[k1 + 1u], hi, 1);
    hi = vsetq_lane_u32(c1[k2 + 1u], hi, 2);
    hi = vsetq_lane_u32(c1[k3 + 1u], hi, 3);

    uint32_t w0 = (y0 == 255u) ? 8u : (y0 & 7u);
    uint32_t w1 = (y1 == 255u) ? 8u : (y1 & 7u);
    uint32_t w2 = (y2 == 255u) ? 8u : (y2 & 7u);
    uint32_t w3 = (y3 == 255u) ? 8u : (y3 & 7u);

    uint32x4_t w1_32 = vdupq_n_u32(w0 * UINT32_C(0x01010101));
    w1_32 = vsetq_lane_u32(w1 * UINT32_C(0x01010101), w1_32, 1);
    w1_32 = vsetq_lane_u32(w2 * UINT32_C(0x01010101), w1_32, 2);
    w1_32 = vsetq_lane_u32(w3 * UINT32_C(0x01010101), w1_32, 3);

    uint8x16_t vw1 = vreinterpretq_u8_u32(w1_32);
    uint8x16_t vw0 = vsubq_u8(vdupq_n_u8(8u), vw1);
    uint8x16_t blo = vreinterpretq_u8_u32(lo);
    uint8x16_t bhi = vreinterpretq_u8_u32(hi);

    uint16x8_t alo = vmlal_u8(vmull_u8(vget_low_u8(blo), vget_low_u8(vw0)),
                              vget_low_u8(bhi), vget_low_u8(vw1));
    uint16x8_t ahi = vmlal_high_u8(vmull_high_u8(blo, vw0), bhi, vw1);
    return vcombine_u8(vrshrn_n_u16(alo, 3), vrshrn_n_u16(ahi, 3));
}

__attribute__((noinline))
void layer0_grade_nv12_baked_neon(const uint32_t *table, const Layer0Frame *frame,
                                  unsigned first_row, unsigned end_row)
{
    if (!table || !frame || !frame->src_y || !frame->src_uv ||
        !frame->dst_y || !frame->dst_uv) {
        return;
    }
    static const uint8_t k_y_bytes[8]  = { 0u, 4u,  8u, 12u, 16u, 20u, 24u, 28u };
    static const uint8_t k_uv_even[8]  = { 1u, 2u,  9u, 10u, 17u, 18u, 25u, 26u };
    static const uint8_t k_uv_odd[8]   = { 5u, 6u, 13u, 14u, 21u, 22u, 29u, 30u };
    static const uint8_t k_bayer0[8]   = { 0u, 0u, 2u, 2u, 0u, 0u, 2u, 2u };
    static const uint8_t k_bayer1[8]   = { 3u, 3u, 1u, 1u, 3u, 3u, 1u, 1u };
    const uint8x8_t y_bytes = vld1_u8(k_y_bytes);
    const uint8x8_t uv_even = vld1_u8(k_uv_even);
    const uint8x8_t uv_odd  = vld1_u8(k_uv_odd);

    unsigned width = frame->width & ~1u;
    size_t sy_stride  = frame->src_y_stride  ? frame->src_y_stride  : frame->width;
    size_t suv_stride = frame->src_uv_stride ? frame->src_uv_stride : frame->width;
    size_t dy_stride  = frame->dst_y_stride  ? frame->dst_y_stride  : frame->width;
    size_t duv_stride = frame->dst_uv_stride ? frame->dst_uv_stride : frame->width;
    if (end_row > frame->height) {
        end_row = frame->height;
    }
    end_row &= ~1u;

    for (unsigned row = first_row & ~1u; row < end_row; row += 2u) {
        const uint8_t *sy0 = frame->src_y  + (size_t)row * sy_stride;
        const uint8_t *sy1 = sy0 + sy_stride;
        const uint8_t *suv = frame->src_uv + (size_t)(row >> 1) * suv_stride;
        uint8_t       *dy0 = frame->dst_y  + (size_t)row * dy_stride;
        uint8_t       *dy1 = dy0 + dy_stride;
        uint8_t       *duv = frame->dst_uv + (size_t)(row >> 1) * duv_stride;
        const uint8x8_t bayer = vld1_u8((row & 2u) ? k_bayer1 : k_bayer0);

        unsigned x = 0u;
#if defined(__clang__)
        #pragma clang loop unroll(disable)
#endif
        for (; x + 8u <= width; x += 8u) {
            if ((x & 63u) == 0u && width - x >= 192u) {
                __builtin_prefetch(sy0 + x + 128u);
                __builtin_prefetch(sy1 + x + 128u);
                __builtin_prefetch(suv + x + 128u);
            }
            uint8x8_t uvd = vqadd_u8(load_bytes(suv + x), bayer);
            uint16x4_t uvp = vreinterpret_u16_u8(vshr_n_u8(uvd, 2));
            uint16x4_t cidx = vorr_u16(vand_u16(uvp, vdup_n_u16(63u)),
                                       vshl_n_u16(vshr_n_u16(uvp, 8), 6));
            uint32_t i0 = (uint32_t)vget_lane_u16(cidx, 0) * LAYER0_BAKED_LUMA;
            uint32_t i1 = (uint32_t)vget_lane_u16(cidx, 1) * LAYER0_BAKED_LUMA;
            uint32_t i2 = (uint32_t)vget_lane_u16(cidx, 2) * LAYER0_BAKED_LUMA;
            uint32_t i3 = (uint32_t)vget_lane_u16(cidx, 3) * LAYER0_BAKED_LUMA;

            const uint32_t *c0 = table + i0;
            const uint32_t *c1 = table + i1;
            const uint32_t *c2 = table + i2;
            const uint32_t *c3 = table + i3;

            uint8x16x2_t top = {{
                baked_lerp4_neon(c0, c1, sy0[x],     sy0[x + 1u], sy0[x + 2u], sy0[x + 3u]),
                baked_lerp4_neon(c2, c3, sy0[x + 4u], sy0[x + 5u], sy0[x + 6u], sy0[x + 7u])
            }};
            uint8x16x2_t bot = {{
                baked_lerp4_neon(c0, c1, sy1[x],     sy1[x + 1u], sy1[x + 2u], sy1[x + 3u]),
                baked_lerp4_neon(c2, c3, sy1[x + 4u], sy1[x + 5u], sy1[x + 6u], sy1[x + 7u])
            }};

            uint16x8_t sum = vaddl_u8(vqtbl2_u8(top, uv_even), vqtbl2_u8(top, uv_odd));
            sum = vaddw_u8(sum, vqtbl2_u8(bot, uv_even));
            sum = vaddw_u8(sum, vqtbl2_u8(bot, uv_odd));
            store_bytes(dy0 + x, vqtbl2_u8(top, y_bytes));
            store_bytes(dy1 + x, vqtbl2_u8(bot, y_bytes));
            store_bytes(duv + x, vrshrn_n_u16(sum, 2));
        }

        if (x < width) {
            const Layer0Frame tail = {
                .src_y = sy0 + x, .src_uv = suv + x,
                .dst_y = dy0 + x, .dst_uv = duv + x,
                .width = width - x, .height = 2u,
                .src_y_stride = (uint32_t)sy_stride, .src_uv_stride = (uint32_t)suv_stride,
                .dst_y_stride = (uint32_t)dy_stride, .dst_uv_stride = (uint32_t)duv_stride
            };
            /* Preserve exact row & column Bayer phase on narrow odd-quad tails. */
            unsigned d_even = (row & 2u) ? 3u : 0u;
            unsigned d_odd  = (row & 2u) ? 1u : 2u;
            (void)tail;
            for (; x < width; x += 2u) {
                unsigned d = (x & 2u) ? d_odd : d_even;
                unsigned cb_d = (unsigned)suv[x] + d;
                unsigned cr_d = (unsigned)suv[x + 1u] + d;
                if (cb_d > 255u) cb_d = 255u;
                if (cr_d > 255u) cr_d = 255u;
                const uint32_t *col = table + ((((cr_d >> 2) << 6) | (cb_d >> 2)) * LAYER0_BAKED_LUMA);
                uint8x16_t res = baked_lerp4_neon(col, col, sy0[x], sy0[x + 1u], sy1[x], sy1[x + 1u]);
                uint32_t a = vgetq_lane_u32(vreinterpretq_u32_u8(res), 0);
                uint32_t b = vgetq_lane_u32(vreinterpretq_u32_u8(res), 1);
                uint32_t c = vgetq_lane_u32(vreinterpretq_u32_u8(res), 2);
                uint32_t e = vgetq_lane_u32(vreinterpretq_u32_u8(res), 3);
                dy0[x] = (uint8_t)a; dy0[x + 1u] = (uint8_t)b;
                dy1[x] = (uint8_t)c; dy1[x + 1u] = (uint8_t)e;
                unsigned sum_cb = (unsigned)(uint8_t)(a >> 8) + (uint8_t)(b >> 8) +
                                  (unsigned)(uint8_t)(c >> 8) + (uint8_t)(e >> 8);
                unsigned sum_cr = (unsigned)(uint8_t)(a >> 16) + (uint8_t)(b >> 16) +
                                  (unsigned)(uint8_t)(c >> 16) + (uint8_t)(e >> 16);
                duv[x] = (uint8_t)((sum_cb + 2u) >> 2);
                duv[x + 1u] = (uint8_t)((sum_cr + 2u) >> 2);
            }
        }
    }
}

__attribute__((noinline))
void layer0_copy_plane_neon(uint8_t *dst, const uint8_t *src, size_t bytes, int do_dc_ivac)
{
    if (!dst || !src || bytes == 0u) {
        return;
    }
#if !defined(V71R7_HOST_TEST)
    if (do_dc_ivac) {
        uintptr_t p = (uintptr_t)src & ~(uintptr_t)63u;
        uintptr_t end_p = ((uintptr_t)src + bytes + 63u) & ~(uintptr_t)63u;
        for (; p + 256u <= end_p; p += 256u) {
            __asm__ volatile(
                "dc ivac, %0\n\t"
                "dc ivac, %1\n\t"
                "dc ivac, %2\n\t"
                "dc ivac, %3"
                :: "r"(p), "r"(p + 64u), "r"(p + 128u), "r"(p + 192u)
                : "memory");
        }
        for (; p < end_p; p += 64u) {
            __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }
#else
    (void)do_dc_ivac;
#endif
    size_t i = 0u;
    const uint8_t *ps = src;
    uint8_t *pd = dst;
#if defined(__clang__)
    #pragma clang loop unroll(disable)
#endif
    for (; i + 64u <= bytes; i += 64u) {
        __asm__ volatile(
            "prfm pldl1strm, [%0, #256]\n\t"
            "ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%0], #64\n\t"
            "st1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%1], #64"
            : "+r"(ps), "+r"(pd)
            :
            : "v0", "v1", "v2", "v3", "memory");
    }
#if defined(__clang__)
    #pragma clang loop unroll(disable)
#endif
    for (; i + 16u <= bytes; i += 16u) {
        __asm__ volatile(
            "ld1 {v0.16b}, [%0], #16\n\t"
            "st1 {v0.16b}, [%1], #16"
            : "+r"(ps), "+r"(pd)
            :
            : "v0", "memory");
    }
#if defined(__clang__)
    #pragma clang loop vectorize(disable) interleave(disable)
#endif
    for (; i < bytes; ++i) {
        dst[i] = src[i];
    }
}

#else /* Non-AArch64 fallback */

void layer0_grade_nv12_planes_neon(const JpegCube *cube, uint32_t *cache,
                                   const Layer0Frame *frame,
                                   unsigned first_row, unsigned end_row)
{
    layer0_grade_nv12_planes(cube, cache, frame, first_row, end_row);
}

void layer0_grade_nv12_baked_neon(const uint32_t *table, const Layer0Frame *frame,
                                  unsigned first_row, unsigned end_row)
{
    layer0_grade_nv12_baked(table, frame, first_row, end_row);
}

void layer0_copy_plane_neon(uint8_t *dst, const uint8_t *src, size_t bytes, int do_dc_ivac)
{
    (void)do_dc_ivac;
    if (dst && src && bytes) {
        memcpy(dst, src, bytes);
    }
}

#endif /* __aarch64__ */

