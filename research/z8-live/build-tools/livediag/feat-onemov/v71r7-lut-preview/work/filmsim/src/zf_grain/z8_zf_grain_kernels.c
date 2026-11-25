/* SPDX-License-Identifier: MIT
 * Nikon Zf C3.01 Film Grain — Scalar C11 & __aarch64__ NEON row kernels +
 * V71 Live Pre-Baked 512x256 Twisted-Torus Grain Tile Apply kernel.
 *
 * Properties:
 *   - Zero dynamic allocation, zero writable .data/.bss, zero floating-point insns,
 *     zero external symbol dependencies.
 *   - 100% bit-identical integer arithmetic between NEON and Scalar (-DZF_GRAIN_SCALAR).
 *   - 100% SCTLR_EL1.A == 1 safe: all external pointers (src_y, dst_y, y, tile512x256,
 *     map_a) use element-aligned inline-asm ld1/st1 wrappers (never assume 16B alignment).
 */
#include "z8_zf_grain.h"
#include "z8_zf_grain_kernels.h"

#if ZF_GRAIN_NEON
#include <arm_neon.h>
#endif

#if defined(__clang__)
#define ZF_NO_VEC_TAIL _Pragma("clang loop vectorize(disable) interleave(disable)")
#define ZF_NO_UNROLL   _Pragma("clang loop unroll(disable)")
#else
#define ZF_NO_VEC_TAIL
#define ZF_NO_UNROLL
#endif

static inline int32_t asr_s32(int32_t v, unsigned shift)
{
    uint32_t u = (uint32_t)v;
    uint32_t sign = (v < 0) ? (~0U << (32U - shift)) : 0U;
    return (int32_t)((u >> shift) | sign);
}

static inline int32_t round_sym_s32(int32_t v, unsigned shift)
{
    int32_t half = (int32_t)(1U << (shift - 1U));
    int32_t adj = (v < 0) ? -1 : 0;
    return asr_s32(v + half + adj, shift);
}

static inline uint8_t clamp_u8(int32_t v)
{
    if (v < 0) return 0U;
    if (v > 255) return 255U;
    return (uint8_t)v;
}

static inline int16_t clamp_s16(int32_t v)
{
    if (v < -32768) return (int16_t)-32768;
    if (v > 32767) return (int16_t)32767;
    return (int16_t)v;
}

static inline uint32_t xorshift32_step(uint32_t s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static inline int16_t apply_sign_s16(int16_t g, uint32_t bit)
{
    /* bit==1 -> +g; bit==0 -> -g with exact 16-bit two's-complement wrap (-(-32768) == -32768). */
    return bit ? g : (int16_t)(uint16_t)(0U - (uint16_t)g);
}

#if ZF_GRAIN_NEON

/* Element-aligned inline-asm ld1/st1 wrappers (1B align for u8/s8, 2B align for s16):
 * 100% SCTLR_EL1.A == 1 safe under -mstrict-align with zero scalarization.
 */
static inline uint8x16_t zf_ld1_u8(const uint8_t *p)
{
    uint8x16_t v;
    __asm__("ld1 {%0.16b}, [%1]" : "=w"(v) : "r"(p) : "memory");
    return v;
}

static inline void zf_st1_u8(uint8_t *p, uint8x16_t v)
{
    __asm__ volatile("st1 {%0.16b}, [%1]" : : "w"(v), "r"(p) : "memory");
}

static inline uint8x16_t zf_ld1_u8_inc(const uint8_t **p)
{
    uint8x16_t v;
    __asm__("ld1 {%0.16b}, [%1], #16" : "=w"(v), "+r"(*p) : : "memory");
    return v;
}

static inline void zf_st1_u8_inc(uint8_t **p, uint8x16_t v)
{
    __asm__ volatile("st1 {%1.16b}, [%0], #16" : "+r"(*p) : "w"(v) : "memory");
}

static inline int8x16_t zf_ld1_s8_inc(const int8_t **p)
{
    int8x16_t v;
    __asm__("ld1 {%0.16b}, [%1], #16" : "=w"(v), "+r"(*p) : : "memory");
    return v;
}

static inline int16x8_t zf_ld1_s16_inc(const int16_t **p)
{
    int16x8_t v;
    __asm__("ld1 {%0.8h}, [%1], #16" : "=w"(v), "+r"(*p) : : "memory");
    return v;
}

static inline void zf_st1_s16(int16_t *p, int16x8_t v)
{
    __asm__ volatile("st1 {%0.8h}, [%1]" : : "w"(v), "r"(p) : "memory");
}

static inline void zf_st1_s16_inc(int16_t **p, int16x8_t v)
{
    __asm__ volatile("st1 {%1.8h}, [%0], #16" : "+r"(*p) : "w"(v) : "memory");
}

/* Strictly 16-byte aligned loads: ONLY called after explicit ((uintptr_t)p & 15U) == 0U check
 * (or on 16-byte aligned .rodata constants).
 */
static inline uint8x16_t ld16a_u8(const uint8_t *p)
{
    return *(const uint8x16_t *)__builtin_assume_aligned(p, 16);
}

static inline int16x8_t ld8a_s16(const int16_t *p)
{
    return *(const int16x8_t *)__builtin_assume_aligned(p, 16);
}

static const uint8_t k_bit_mask16[16] __attribute__((aligned(16))) = {
    1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U,
    1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U
};

static inline int16x8_t gather8_gm(const int16_t *gm_row, const uint8_t *xp)
{
    int16x8_t v = vdupq_n_s16(0);
    v = vld1q_lane_s16(gm_row + xp[0], v, 0);
    v = vld1q_lane_s16(gm_row + xp[1], v, 1);
    v = vld1q_lane_s16(gm_row + xp[2], v, 2);
    v = vld1q_lane_s16(gm_row + xp[3], v, 3);
    v = vld1q_lane_s16(gm_row + xp[4], v, 4);
    v = vld1q_lane_s16(gm_row + xp[5], v, 5);
    v = vld1q_lane_s16(gm_row + xp[6], v, 6);
    v = vld1q_lane_s16(gm_row + xp[7], v, 7);
    return v;
}

/* Core 16-pixel NEON Live grain application (standard Zf knees 32..223, srshr #6). */
static inline uint8x16_t live_grain_apply16_srshr(uint8x16_t y, int8x16_t g, uint8x16_t v64)
{
    uint8x16_t inv = vmvnq_u8(y);
    uint8x16_t lo  = vqaddq_u8(y, y);
    uint8x16_t hi  = vqaddq_u8(inv, inv);
    int8x16_t  t   = vreinterpretq_s8_u8(vminq_u8(vminq_u8(lo, hi), v64));
    int16x8_t  plo = vrshrq_n_s16(vmull_s8(vget_low_s8(g), vget_low_s8(t)), 6);
    int16x8_t  phi = vrshrq_n_s16(vmull_high_s8(g, t), 6);
    int16x8_t  slo = vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(plo), vget_low_u8(y)));
    int16x8_t  shi = vreinterpretq_s16_u16(vaddw_high_u8(vreinterpretq_u16_s16(phi), y));
    return vcombine_u8(vqmovun_s16(slo), vqmovun_s16(shi));
}

#endif /* ZF_GRAIN_NEON */

void zf_grain_copy_u8_fast(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    uint32_t i = 0U;
#if ZF_GRAIN_NEON
    const uint8_t *ps = src;
    uint8_t *pd = dst;
    ZF_NO_UNROLL
    for (; i + 64U <= n; i += 64U) {
        __asm__ volatile(
            "prfm pldl1strm, [%0, #256]\n\t"
            "ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%0], #64\n\t"
            "st1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%1], #64"
            : "+r"(ps), "+r"(pd)
            :
            : "v0", "v1", "v2", "v3", "memory");
    }
    ZF_NO_UNROLL
    for (; i + 16U <= n; i += 16U) {
        zf_st1_u8_inc(&pd, zf_ld1_u8_inc(&ps));
    }
    ZF_NO_VEC_TAIL
#endif
    for (; i < n; ++i) {
        dst[i] = src[i];
    }
}

void zf_grain_scale_map_s16(const int16_t *map_a, uint32_t count,
                            uint32_t blend_w, int16_t *out)
{
    uint32_t i = 0U;
#if ZF_GRAIN_NEON
    const int16_t *ps = map_a;
    int16_t *pd = out;
    int16_t w16 = (int16_t)blend_w;
    ZF_NO_UNROLL
    for (; i + 8U <= count; i += 8U) {
        int16x8_t v = zf_ld1_s16_inc(&ps);
        int32x4_t plo = vmull_n_s16(vget_low_s16(v), w16);
        int32x4_t phi = vmull_high_n_s16(v, w16);
        zf_st1_s16_inc(&pd, vcombine_s16(vshrn_n_s32(plo, 4), vshrn_n_s32(phi, 4)));
    }
    ZF_NO_VEC_TAIL
#endif
    for (; i < count; ++i) {
        int32_t prod = (int32_t)blend_w * (int32_t)map_a[i];
        out[i] = (int16_t)(uint16_t)(uint32_t)asr_s32(prod, 4U);
    }
}

#if ZF_GRAIN_NEON
/* V71.3: 100% bit-exact 83-entry vqtbl4+vqtbx2 NEON gather for |gm_row| <= 127
 * (covers Intensity 1, 2, and 3-Map0). Uses 8-bit saturating add/sub for [32..223]
 * midtones, 0 Q-spills, 0 stack buffers, and 1-byte-aligned ld2 (SCTLR_EL1.A==1 safe). */
static __attribute__((noinline)) uint32_t apply_line_exact_s8_neon(
    const uint8_t *src_y, uint8_t *dst_noisy, const uint8_t *x_idx,
    uint32_t x, uint32_t width, uint32_t state,
    uint8x16x4_t t0_lo, uint8x16x2_t t1_lo)
{
    uint8x16_t v_mask = ld16a_u8(k_bit_mask16);
    uint8x16_t v_32   = vdupq_n_u8(32U);
    uint8x16_t v_64   = vdupq_n_u8(64U);
    int8x16_t  v_zero = vdupq_n_s8(0);
    ZF_NO_UNROLL
    for (; x + 32U <= width; x += 32U) {
        state = xorshift32_step(state);
        uint32_t bits = state;
        for (uint32_t h = 0U; h < 2U; ++h) {
            uint32_t off = x + h * 16U;
            uint8x16_t y16 = zf_ld1_u8(src_y + off);

            uint8_t b0 = (uint8_t)(bits & 0xFFU);
            uint8_t b1 = (uint8_t)((bits >> 8) & 0xFFU);
            bits >>= 16;
            uint8x16_t mb = vtstq_u8(vcombine_u8(vdup_n_u8(b0), vdup_n_u8(b1)), v_mask);

            uint8x16_t idx = zf_ld1_u8(x_idx + off);
            int8x16_t g8 = vreinterpretq_s8_u8(
                vqtbx2q_u8(vqtbl4q_u8(t0_lo, idx), t1_lo, vsubq_u8(idx, v_64)));
            int8x16_t sg8 = vbslq_s8(mb, g8, vnegq_s8(g8));

            if (__builtin_expect(vmaxvq_u8(vsubq_u8(y16, v_32)) <= 191U, 1)) {
                /* All 16 pixels in [32..223]: tone==64 -> delta==sg8 exactly; |sg8|<=127 */
                uint8x16_t pos = vreinterpretq_u8_s8(vmaxq_s8(sg8, v_zero));
                uint8x16_t neg = vreinterpretq_u8_s8(vmaxq_s8(vnegq_s8(sg8), v_zero));
                zf_st1_u8(dst_noisy + off, vqsubq_u8(vqaddq_u8(y16, pos), neg));
            } else {
                uint8x16_t inv = vmvnq_u8(y16);
                uint8x16_t t16 = vminq_u8(vminq_u8(vqaddq_u8(y16, y16), vqaddq_u8(inv, inv)), v_64);
                int16x8_t sg0 = vmovl_s8(vget_low_s8(sg8));
                int16x8_t sg1 = vmovl_high_s8(sg8);
                int16x8_t tv0 = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(t16)));
                int16x8_t tv1 = vreinterpretq_s16_u16(vmovl_high_u8(t16));
                int16x8_t d0 = vcombine_s16(vshrn_n_s32(vmull_s16(vget_low_s16(sg0), vget_low_s16(tv0)), 6),
                                            vshrn_n_s32(vmull_high_s16(sg0, tv0), 6));
                int16x8_t d1 = vcombine_s16(vshrn_n_s32(vmull_s16(vget_low_s16(sg1), vget_low_s16(tv1)), 6),
                                            vshrn_n_s32(vmull_high_s16(sg1, tv1), 6));
                int16x8_t yv0 = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(y16)));
                int16x8_t yv1 = vreinterpretq_s16_u16(vmovl_high_u8(y16));
                zf_st1_u8(dst_noisy + off, vcombine_u8(vqmovun_s16(vqaddq_s16(yv0, d0)),
                                                       vqmovun_s16(vqaddq_s16(yv1, d1))));
            }
        }
    }
    return state;
}

/* Full 16-bit 83-entry vqtbl4+vqtbx2 NEON gather for |gm_row| > 127 (Intensity 4..6).
 * Passes gm_row pointer (not 12 Q-regs by value) to guarantee 0 Q-register stack traffic. */
static __attribute__((noinline)) uint32_t apply_line_exact_s16_neon(
    const uint8_t *src_y, uint8_t *dst_noisy, const int16_t *gm_row, const uint8_t *x_idx,
    uint32_t x, uint32_t width, uint32_t state)
{
    const uint8_t *gp = (const uint8_t *)gm_row;
    uint8x16x2_t d0 = vld2q_u8(gp + 0U);
    uint8x16x2_t d1 = vld2q_u8(gp + 32U);
    uint8x16x2_t d2 = vld2q_u8(gp + 64U);
    uint8x16x2_t d3 = vld2q_u8(gp + 96U);
    uint8x16x2_t d4 = vld2q_u8(gp + 128U);
    uint8x16_t d5_lo = vdupq_n_u8(0U), d5_hi = vdupq_n_u8(0U);
    d5_lo = vsetq_lane_u8(gp[160], d5_lo, 0); d5_hi = vsetq_lane_u8(gp[161], d5_hi, 0);
    d5_lo = vsetq_lane_u8(gp[162], d5_lo, 1); d5_hi = vsetq_lane_u8(gp[163], d5_hi, 1);
    d5_lo = vsetq_lane_u8(gp[164], d5_lo, 2); d5_hi = vsetq_lane_u8(gp[165], d5_hi, 2);
    uint8x16x4_t t0_lo = {{ d0.val[0], d1.val[0], d2.val[0], d3.val[0] }};
    uint8x16x2_t t1_lo = {{ d4.val[0], d5_lo }};
    uint8x16x4_t t0_hi = {{ d0.val[1], d1.val[1], d2.val[1], d3.val[1] }};
    uint8x16x2_t t1_hi = {{ d4.val[1], d5_hi }};

    uint8x16_t v_mask = ld16a_u8(k_bit_mask16);
    uint8x16_t v_32   = vdupq_n_u8(32U);
    uint8x16_t v_64   = vdupq_n_u8(64U);
    ZF_NO_UNROLL
    for (; x + 32U <= width; x += 32U) {
        state = xorshift32_step(state);
        uint32_t bits = state;
        for (uint32_t h = 0U; h < 2U; ++h) {
            uint32_t off = x + h * 16U;
            uint8x16_t y16 = zf_ld1_u8(src_y + off);

            uint8_t b0 = (uint8_t)(bits & 0xFFU);
            uint8_t b1 = (uint8_t)((bits >> 8) & 0xFFU);
            bits >>= 16;
            uint8x16_t mb = vtstq_u8(vcombine_u8(vdup_n_u8(b0), vdup_n_u8(b1)), v_mask);

            uint8x16_t idx = zf_ld1_u8(x_idx + off);
            uint8x16_t idx64 = vsubq_u8(idx, v_64);
            uint8x16_t glo = vqtbx2q_u8(vqtbl4q_u8(t0_lo, idx), t1_lo, idx64);
            uint8x16_t ghi = vqtbx2q_u8(vqtbl4q_u8(t0_hi, idx), t1_hi, idx64);
            int16x8_t g0 = vreinterpretq_s16_u8(vzip1q_u8(glo, ghi));
            int16x8_t g1 = vreinterpretq_s16_u8(vzip2q_u8(glo, ghi));

            uint16x8_t m0 = vreinterpretq_u16_s16(vmovl_s8(vget_low_s8(vreinterpretq_s8_u8(mb))));
            uint16x8_t m1 = vreinterpretq_u16_s16(vmovl_high_s8(vreinterpretq_s8_u8(mb)));
            int16x8_t sg0 = vbslq_s16(m0, g0, vnegq_s16(g0));
            int16x8_t sg1 = vbslq_s16(m1, g1, vnegq_s16(g1));

            int16x8_t d0, d1;
            if (__builtin_expect(vmaxvq_u8(vsubq_u8(y16, v_32)) <= 191U, 1)) {
                d0 = sg0;
                d1 = sg1;
            } else {
                uint8x16_t inv = vmvnq_u8(y16);
                uint8x16_t t16 = vminq_u8(vminq_u8(vqaddq_u8(y16, y16), vqaddq_u8(inv, inv)), v_64);
                int16x8_t tv0 = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(t16)));
                int16x8_t tv1 = vreinterpretq_s16_u16(vmovl_high_u8(t16));
                d0 = vcombine_s16(vshrn_n_s32(vmull_s16(vget_low_s16(sg0), vget_low_s16(tv0)), 6),
                                  vshrn_n_s32(vmull_high_s16(sg0, tv0), 6));
                d1 = vcombine_s16(vshrn_n_s32(vmull_s16(vget_low_s16(sg1), vget_low_s16(tv1)), 6),
                                  vshrn_n_s32(vmull_high_s16(sg1, tv1), 6));
            }
            int16x8_t yv0 = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(y16)));
            int16x8_t yv1 = vreinterpretq_s16_u16(vmovl_high_u8(y16));
            zf_st1_u8(dst_noisy + off, vcombine_u8(vqmovun_s16(vqaddq_s16(yv0, d0)),
                                                   vqmovun_s16(vqaddq_s16(yv1, d1))));
        }
    }
    return state;
}
#endif

uint32_t zf_grain_apply_line_exact(const uint8_t *src_y, uint8_t *dst_noisy,
                                   const int16_t *gm_row, const uint8_t *x_idx,
                                   const uint8_t *tone_lut, uint32_t width,
                                   uint32_t prng_state, uint32_t bit_offset,
                                   int std_knees)
{
    uint32_t x = 0U;
    uint32_t state = prng_state;

    if (bit_offset != 0U && x < width) {
        state = xorshift32_step(state);
        for (uint32_t b = bit_offset; b < 32U && x < width; ++b, ++x) {
            uint8_t yv = src_y[x];
            int16_t sg = apply_sign_s16(gm_row[x_idx[x]], (state >> b) & 1U);
            int32_t delta = asr_s32((int32_t)sg * (int32_t)tone_lut[yv], 6U);
            dst_noisy[x] = clamp_u8((int32_t)yv + delta);
        }
    }

#if ZF_GRAIN_NEON
    if (std_knees && x + 32U <= width) {
        /* Deinterleave 83-entry int16_t gm_row into lo/hi byte TBL tables (1-byte aligned ld2). */
        const uint8_t *gp = (const uint8_t *)gm_row;
        uint8x16x2_t d0 = vld2q_u8(gp + 0U);
        uint8x16x2_t d1 = vld2q_u8(gp + 32U);
        uint8x16x2_t d2 = vld2q_u8(gp + 64U);
        uint8x16x2_t d3 = vld2q_u8(gp + 96U);
        uint8x16x2_t d4 = vld2q_u8(gp + 128U);
        uint8x16_t d5_lo = vdupq_n_u8(0U), d5_hi = vdupq_n_u8(0U);
        d5_lo = vsetq_lane_u8(gp[160], d5_lo, 0); d5_hi = vsetq_lane_u8(gp[161], d5_hi, 0);
        d5_lo = vsetq_lane_u8(gp[162], d5_lo, 1); d5_hi = vsetq_lane_u8(gp[163], d5_hi, 1);
        d5_lo = vsetq_lane_u8(gp[164], d5_lo, 2); d5_hi = vsetq_lane_u8(gp[165], d5_hi, 2);

        uint8x16_t v_80 = vdupq_n_u8(0x80U);
        uint8x16_t bad = vorrq_u8(
            veorq_u8(d0.val[1], vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_u8(d0.val[0]), 7))),
            vceqq_u8(d0.val[0], v_80));
        bad = vorrq_u8(bad, vorrq_u8(
            veorq_u8(d1.val[1], vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_u8(d1.val[0]), 7))),
            vceqq_u8(d1.val[0], v_80)));
        bad = vorrq_u8(bad, vorrq_u8(
            veorq_u8(d2.val[1], vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_u8(d2.val[0]), 7))),
            vceqq_u8(d2.val[0], v_80)));
        bad = vorrq_u8(bad, vorrq_u8(
            veorq_u8(d3.val[1], vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_u8(d3.val[0]), 7))),
            vceqq_u8(d3.val[0], v_80)));
        bad = vorrq_u8(bad, vorrq_u8(
            veorq_u8(d4.val[1], vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_u8(d4.val[0]), 7))),
            vceqq_u8(d4.val[0], v_80)));
        bad = vorrq_u8(bad, vorrq_u8(
            veorq_u8(d5_hi, vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_u8(d5_lo), 7))),
            vceqq_u8(d5_lo, v_80)));

        if (__builtin_expect(vmaxvq_u8(bad) == 0U, 1)) {
            uint8x16x4_t t0_lo = {{ d0.val[0], d1.val[0], d2.val[0], d3.val[0] }};
            uint8x16x2_t t1_lo = {{ d4.val[0], d5_lo }};
            state = apply_line_exact_s8_neon(src_y, dst_noisy, x_idx, x, width, state, t0_lo, t1_lo);
        } else {
            state = apply_line_exact_s16_neon(src_y, dst_noisy, gm_row, x_idx, x, width, state);
        }
        x += ((width - x) & ~31U);
    }
    ZF_NO_VEC_TAIL
#else
    (void)std_knees;
#endif

    while (x < width) {
        state = xorshift32_step(state);
        for (uint32_t b = 0U; b < 32U && x < width; ++b, ++x) {
            uint8_t yv = src_y[x];
            int16_t sg = apply_sign_s16(gm_row[x_idx[x]], (state >> b) & 1U);
            int32_t delta = asr_s32((int32_t)sg * (int32_t)tone_lut[yv], 6U);
            dst_noisy[x] = clamp_u8((int32_t)yv + delta);
        }
    }
    return state;
}

uint32_t zf_grain_apply_line_sharp_fast(const uint8_t *src_y, int16_t *dst_delta,
                                        const int16_t *gm_row, const uint8_t *x_idx,
                                        const uint8_t *tone_lut, uint32_t width,
                                        uint32_t prng_state, uint32_t bit_offset,
                                        int std_knees, int s16_mul_safe)
{
    uint32_t x = 0U;
    uint32_t state = prng_state;

    if (bit_offset != 0U && x < width) {
        state = xorshift32_step(state);
        for (uint32_t b = bit_offset; b < 32U && x < width; ++b, ++x) {
            uint8_t yv = src_y[x];
            int16_t sg = apply_sign_s16(gm_row[x_idx[x]], (state >> b) & 1U);
            int32_t delta = round_sym_s32((int32_t)sg * (int32_t)tone_lut[yv], 6U);
            dst_delta[x] = clamp_s16(delta);
        }
    }

#if ZF_GRAIN_NEON
    if (std_knees) {
        uint8x16_t v_mask = ld16a_u8(k_bit_mask16);
        uint8x16_t v_64   = vdupq_n_u8(64U);
        if (s16_mul_safe) {
            /* |gm| <= 511 -> |sg * tone| <= 32704 <= 32767 (exact int16x8, 0 int32 widening). */
            int16x8_t v_half16 = vdupq_n_s16(32);
            ZF_NO_UNROLL
            for (; x + 32U <= width; x += 32U) {
                state = xorshift32_step(state);
                uint32_t bits = state;
                for (uint32_t h = 0U; h < 2U; ++h) {
                    uint32_t off = x + h * 16U;
                    uint8x16_t y16 = zf_ld1_u8(src_y + off);
                    uint8x16_t inv = vmvnq_u8(y16);
                    uint8x16_t t16 = vminq_u8(vminq_u8(vqaddq_u8(y16, y16), vqaddq_u8(inv, inv)), v_64);

                    uint8_t b0 = (uint8_t)(bits & 0xFFU);
                    uint8_t b1 = (uint8_t)((bits >> 8) & 0xFFU);
                    bits >>= 16;
                    uint8x16_t bv = vcombine_u8(vdup_n_u8(b0), vdup_n_u8(b1));
                    uint8x16_t mb = vtstq_u8(bv, v_mask);

                    int16x8_t g0 = gather8_gm(gm_row, x_idx + off);
                    int16x8_t g1 = gather8_gm(gm_row, x_idx + off + 8U);
                    uint16x8_t m0 = vreinterpretq_u16_s16(vmovl_s8(vget_low_s8(vreinterpretq_s8_u8(mb))));
                    uint16x8_t m1 = vreinterpretq_u16_s16(vmovl_high_s8(vreinterpretq_s8_u8(mb)));
                    int16x8_t sg0 = vbslq_s16(m0, g0, vnegq_s16(g0));
                    int16x8_t sg1 = vbslq_s16(m1, g1, vnegq_s16(g1));

                    if (__builtin_expect(vminvq_u8(t16) == 64U, 1)) {
                        zf_st1_s16(dst_delta + off,      sg0);
                        zf_st1_s16(dst_delta + off + 8U, sg1);
                    } else {
                        int16x8_t tv0 = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(t16)));
                        int16x8_t tv1 = vreinterpretq_s16_u16(vmovl_high_u8(t16));
                        int16x8_t p0  = vmulq_s16(sg0, tv0);
                        int16x8_t p1  = vmulq_s16(sg1, tv1);
                        int16x8_t d0  = vshrq_n_s16(vaddq_s16(vsraq_n_s16(p0, p0, 15), v_half16), 6);
                        int16x8_t d1  = vshrq_n_s16(vaddq_s16(vsraq_n_s16(p1, p1, 15), v_half16), 6);
                        zf_st1_s16(dst_delta + off,      d0);
                        zf_st1_s16(dst_delta + off + 8U, d1);
                    }
                }
            }
        } else {
            int32x4_t v_half = vdupq_n_s32(32);
            ZF_NO_UNROLL
            for (; x + 32U <= width; x += 32U) {
                state = xorshift32_step(state);
                uint32_t bits = state;
                for (uint32_t h = 0U; h < 2U; ++h) {
                    uint32_t off = x + h * 16U;
                    uint8x16_t y16 = zf_ld1_u8(src_y + off);
                    uint8x16_t inv = vmvnq_u8(y16);
                    uint8x16_t t16 = vminq_u8(vminq_u8(vqaddq_u8(y16, y16), vqaddq_u8(inv, inv)), v_64);
                    uint8_t b0 = (uint8_t)(bits & 0xFFU);
                    uint8_t b1 = (uint8_t)((bits >> 8) & 0xFFU);
                    bits >>= 16;
                    uint8x16_t mb = vtstq_u8(vcombine_u8(vdup_n_u8(b0), vdup_n_u8(b1)), v_mask);
                    int16x8_t g0 = gather8_gm(gm_row, x_idx + off);
                    int16x8_t g1 = gather8_gm(gm_row, x_idx + off + 8U);
                    int16x8_t sg0 = vbslq_s16(
                        vreinterpretq_u16_s16(vmovl_s8(vget_low_s8(vreinterpretq_s8_u8(mb)))),
                        g0, vnegq_s16(g0));
                    int16x8_t sg1 = vbslq_s16(
                        vreinterpretq_u16_s16(vmovl_high_s8(vreinterpretq_s8_u8(mb))),
                        g1, vnegq_s16(g1));
                    int16x8_t tv0 = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(t16)));
                    int16x8_t tv1 = vreinterpretq_s16_u16(vmovl_high_u8(t16));
                    int32x4_t p00 = vmull_s16(vget_low_s16(sg0), vget_low_s16(tv0));
                    int32x4_t p01 = vmull_high_s16(sg0, tv0);
                    int32x4_t p10 = vmull_s16(vget_low_s16(sg1), vget_low_s16(tv1));
                    int32x4_t p11 = vmull_high_s16(sg1, tv1);
                    zf_st1_s16(dst_delta + off, vcombine_s16(
                        vqshrn_n_s32(vaddq_s32(vsraq_n_s32(p00, p00, 31), v_half), 6),
                        vqshrn_n_s32(vaddq_s32(vsraq_n_s32(p01, p01, 31), v_half), 6)));
                    zf_st1_s16(dst_delta + off + 8U, vcombine_s16(
                        vqshrn_n_s32(vaddq_s32(vsraq_n_s32(p10, p10, 31), v_half), 6),
                        vqshrn_n_s32(vaddq_s32(vsraq_n_s32(p11, p11, 31), v_half), 6)));
                }
            }
        }
    }
    ZF_NO_VEC_TAIL
#else
    (void)std_knees;
    (void)s16_mul_safe;
#endif

    while (x < width) {
        state = xorshift32_step(state);
        for (uint32_t b = 0U; b < 32U && x < width; ++b, ++x) {
            uint8_t yv = src_y[x];
            int16_t sg = apply_sign_s16(gm_row[x_idx[x]], (state >> b) & 1U);
            int32_t delta = round_sym_s32((int32_t)sg * (int32_t)tone_lut[yv], 6U);
            dst_delta[x] = clamp_s16(delta);
        }
    }
    return state;
}

uint32_t zf_grain_apply_line_sharp(const uint8_t *src_y, int16_t *dst_delta,
                                   const int16_t *gm_row, const uint8_t *x_idx,
                                   const uint8_t *tone_lut, uint32_t width,
                                   uint32_t prng_state, uint32_t bit_offset,
                                   int std_knees)
{
    return zf_grain_apply_line_sharp_fast(src_y, dst_delta, gm_row, x_idx, tone_lut,
                                          width, prng_state, bit_offset, std_knees, 0);
}

#if ZF_GRAIN_NEON
static inline void vsum3_u8_16(const uint8_t *p0, const uint8_t *p1, const uint8_t *p2,
                               uint16x8_t *vlo, uint16x8_t *vhi)
{
    uint8x16_t a0 = ld16a_u8(p0);
    uint8x16_t a1 = ld16a_u8(p1);
    uint8x16_t a2 = ld16a_u8(p2);
    *vlo = vaddq_u16(vaddl_u8(vget_low_u8(a0), vget_low_u8(a2)), vshll_n_u8(vget_low_u8(a1), 1));
    *vhi = vaddq_u16(vaddl_high_u8(a0, a2), vshll_high_n_u8(a1, 1));
}
#endif

void zf_grain_blur3_u8_row(const uint8_t *r0, const uint8_t *r1, const uint8_t *r2,
                           uint8_t *out, uint32_t width)
{
    ptrdiff_t x = 0;
    ptrdiff_t w = (ptrdiff_t)width;
#if ZF_GRAIN_NEON
    uintptr_t a0 = (uintptr_t)r0 & 15U;
    uintptr_t a1 = (uintptr_t)r1 & 15U;
    uintptr_t a2 = (uintptr_t)r2 & 15U;

    if (a0 == a1 && a1 == a2) {
        if (a0 == 0U && w >= 48) {
            /* r0,r1,r2 all 16B aligned. Peel x=0..15 scalar so we never read < r[-1]. */
            ZF_NO_VEC_TAIL
            for (; x < 16; ++x) {
                uint32_t vl = (uint32_t)r0[x - 1] + ((uint32_t)r1[x - 1] << 1) + r2[x - 1];
                uint32_t vc = (uint32_t)r0[x]     + ((uint32_t)r1[x]     << 1) + r2[x];
                uint32_t vr = (uint32_t)r0[x + 1] + ((uint32_t)r1[x + 1] << 1) + r2[x + 1];
                out[x] = (uint8_t)((vl + (vc << 1) + vr) >> 4);
            }
            uint16x8_t p_lo, p_hi, c_lo, c_hi;
            vsum3_u8_16(r0 + 0,  r1 + 0,  r2 + 0,  &p_lo, &p_hi);
            vsum3_u8_16(r0 + 16, r1 + 16, r2 + 16, &c_lo, &c_hi);
            ZF_NO_UNROLL
            for (; x + 32 <= w; x += 16) {
                uint16x8_t n_lo, n_hi;
                vsum3_u8_16(r0 + x + 16, r1 + x + 16, r2 + x + 16, &n_lo, &n_hi);
                uint16x8_t l0  = vextq_u16(p_hi, c_lo, 7);
                uint16x8_t r0v = vextq_u16(c_lo, c_hi, 1);
                uint16x8_t l1  = vextq_u16(c_lo, c_hi, 7);
                uint16x8_t r1v = vextq_u16(c_hi, n_lo, 1);
                uint16x8_t acc0 = vaddq_u16(vaddq_u16(l0, r0v), vshlq_n_u16(c_lo, 1));
                uint16x8_t acc1 = vaddq_u16(vaddq_u16(l1, r1v), vshlq_n_u16(c_hi, 1));
                zf_st1_u8(out + x, vcombine_u8(vshrn_n_u16(acc0, 4), vshrn_n_u16(acc1, 4)));
                p_hi = c_hi;
                c_lo = n_lo;
                c_hi = n_hi;
            }
        } else if (a0 == 1U && w >= 31) {
            /* r-1 is 16B aligned (in-bounds!), 0 prologue peel. */
            uint16x8_t c_lo, c_hi;
            vsum3_u8_16(r0 - 1, r1 - 1, r2 - 1, &c_lo, &c_hi);
            ZF_NO_UNROLL
            for (; x + 31 <= w; x += 16) {
                uint16x8_t n_lo, n_hi;
                vsum3_u8_16(r0 + x + 15, r1 + x + 15, r2 + x + 15, &n_lo, &n_hi);
                uint16x8_t c0  = vextq_u16(c_lo, c_hi, 1);
                uint16x8_t r0v = vextq_u16(c_lo, c_hi, 2);
                uint16x8_t c1  = vextq_u16(c_hi, n_lo, 1);
                uint16x8_t r1v = vextq_u16(c_hi, n_lo, 2);
                uint16x8_t acc0 = vaddq_u16(vaddq_u16(c_lo, r0v), vshlq_n_u16(c0, 1));
                uint16x8_t acc1 = vaddq_u16(vaddq_u16(c_hi, r1v), vshlq_n_u16(c1, 1));
                zf_st1_u8(out + x, vcombine_u8(vshrn_n_u16(acc0, 4), vshrn_n_u16(acc1, 4)));
                c_lo = n_lo;
                c_hi = n_hi;
            }
        }
    }
    ZF_NO_VEC_TAIL
#endif
    for (; x < w; ++x) {
        uint32_t vl = (uint32_t)r0[x - 1] + ((uint32_t)r1[x - 1] << 1) + (uint32_t)r2[x - 1];
        uint32_t vc = (uint32_t)r0[x]     + ((uint32_t)r1[x]     << 1) + (uint32_t)r2[x];
        uint32_t vr = (uint32_t)r0[x + 1] + ((uint32_t)r1[x + 1] << 1) + (uint32_t)r2[x + 1];
        out[x] = (uint8_t)((vl + (vc << 1) + vr) >> 4);
    }
}

#if ZF_GRAIN_NEON
static inline void vsum5_u8_16(const uint8_t *p0, const uint8_t *p1, const uint8_t *p2,
                               const uint8_t *p3, const uint8_t *p4,
                               uint16x8_t *s0_lo, uint16x8_t *s0_hi,
                               uint16x8_t *s1_lo, uint16x8_t *s1_hi,
                               uint16x8_t *s2_lo, uint16x8_t *s2_hi)
{
    uint8x16_t v0 = ld16a_u8(p0), v1 = ld16a_u8(p1), v2 = ld16a_u8(p2),
               v3 = ld16a_u8(p3), v4 = ld16a_u8(p4);
    *s0_lo = vmovl_u8(vget_low_u8(v2));
    *s0_hi = vmovl_high_u8(v2);
    *s1_lo = vaddl_u8(vget_low_u8(v1), vget_low_u8(v3));
    *s1_hi = vaddl_high_u8(v1, v3);
    *s2_lo = vaddl_u8(vget_low_u8(v0), vget_low_u8(v4));
    *s2_hi = vaddl_high_u8(v0, v4);
}

static inline uint8x8_t blur5_u16x8_core(
    uint16x8_t s0_m2, uint16x8_t s0_m1, uint16x8_t s0_0, uint16x8_t s0_p1, uint16x8_t s0_p2,
    uint16x8_t s1_m2, uint16x8_t s1_m1, uint16x8_t s1_0, uint16x8_t s1_p1, uint16x8_t s1_p2,
    uint16x8_t s2_m1, uint16x8_t s2_0, uint16x8_t s2_p1)
{
    uint16x8_t t26 = vaddq_u16(vaddq_u16(s0_m1, s0_p1), s1_0);
    uint16x8_t t15 = vaddq_u16(s1_m1, s1_p1);
    uint16x8_t t7  = vaddq_u16(vaddq_u16(s0_m2, s0_p2), s2_0);
    uint16x8_t t3  = vaddq_u16(vaddq_u16(s1_m2, s1_p2), vaddq_u16(s2_m1, s2_p1));
    uint16x8_t acc = vmulq_n_u16(s0_0, 40U);
    acc = vmlaq_n_u16(acc, t26, 26U);
    acc = vmlaq_n_u16(acc, t15, 15U);
    acc = vmlaq_n_u16(acc, t7, 7U);
    acc = vmlaq_n_u16(acc, t3, 3U);
    return vshrn_n_u16(acc, 8);
}
#endif

void zf_grain_blur5_u8_row(const uint8_t *r0, const uint8_t *r1, const uint8_t *r2,
                           const uint8_t *r3, const uint8_t *r4,
                           uint8_t *out, uint32_t width)
{
    ptrdiff_t x = 0;
    ptrdiff_t w = (ptrdiff_t)width;
#if ZF_GRAIN_NEON
    uintptr_t a0 = (uintptr_t)r0 & 15U;
    if (a0 == ((uintptr_t)r1 & 15U) && a0 == ((uintptr_t)r2 & 15U) &&
        a0 == ((uintptr_t)r3 & 15U) && a0 == ((uintptr_t)r4 & 15U)) {
        if (a0 == 2U && w >= 30) {
            /* r-2 is 16B aligned (in-bounds!), 0 prologue peel. */
            uint16x8_t c0_lo, c0_hi, c1_lo, c1_hi, c2_lo, c2_hi;
            vsum5_u8_16(r0 - 2, r1 - 2, r2 - 2, r3 - 2, r4 - 2,
                        &c0_lo, &c0_hi, &c1_lo, &c1_hi, &c2_lo, &c2_hi);
            ZF_NO_UNROLL
            for (; x + 30 <= w; x += 16) {
                uint16x8_t n0_lo, n0_hi, n1_lo, n1_hi, n2_lo, n2_hi;
                vsum5_u8_16(r0 + x + 14, r1 + x + 14, r2 + x + 14, r3 + x + 14, r4 + x + 14,
                            &n0_lo, &n0_hi, &n1_lo, &n1_hi, &n2_lo, &n2_hi);
                uint8x8_t lo = blur5_u16x8_core(
                    c0_lo, vextq_u16(c0_lo, c0_hi, 1), vextq_u16(c0_lo, c0_hi, 2),
                    vextq_u16(c0_lo, c0_hi, 3), vextq_u16(c0_lo, c0_hi, 4),
                    c1_lo, vextq_u16(c1_lo, c1_hi, 1), vextq_u16(c1_lo, c1_hi, 2),
                    vextq_u16(c1_lo, c1_hi, 3), vextq_u16(c1_lo, c1_hi, 4),
                    vextq_u16(c2_lo, c2_hi, 1), vextq_u16(c2_lo, c2_hi, 2), vextq_u16(c2_lo, c2_hi, 3));
                uint8x8_t hi = blur5_u16x8_core(
                    c0_hi, vextq_u16(c0_hi, n0_lo, 1), vextq_u16(c0_hi, n0_lo, 2),
                    vextq_u16(c0_hi, n0_lo, 3), vextq_u16(c0_hi, n0_lo, 4),
                    c1_hi, vextq_u16(c1_hi, n1_lo, 1), vextq_u16(c1_hi, n1_lo, 2),
                    vextq_u16(c1_hi, n1_lo, 3), vextq_u16(c1_hi, n1_lo, 4),
                    vextq_u16(c2_hi, n2_lo, 1), vextq_u16(c2_hi, n2_lo, 2), vextq_u16(c2_hi, n2_lo, 3));
                zf_st1_u8(out + x, vcombine_u8(lo, hi));
                c0_lo = n0_lo; c0_hi = n0_hi;
                c1_lo = n1_lo; c1_hi = n1_hi;
                c2_lo = n2_lo; c2_hi = n2_hi;
            }
        } else {
            ptrdiff_t lead = (ptrdiff_t)((16U - a0) & 15U);
            ptrdiff_t x_start = lead + 16;
            if (w >= x_start + 32) {
                ZF_NO_VEC_TAIL
                for (; x < x_start; ++x) {
                    uint32_t s0_0 = r2[x], s0_m1 = r2[x - 1], s0_p1 = r2[x + 1],
                             s0_m2 = r2[x - 2], s0_p2 = r2[x + 2];
                    uint32_t s1_m2 = (uint32_t)r1[x - 2] + r3[x - 2];
                    uint32_t s1_m1 = (uint32_t)r1[x - 1] + r3[x - 1];
                    uint32_t s1_0  = (uint32_t)r1[x]     + r3[x];
                    uint32_t s1_p1 = (uint32_t)r1[x + 1] + r3[x + 1];
                    uint32_t s1_p2 = (uint32_t)r1[x + 2] + r3[x + 2];
                    uint32_t s2_m1 = (uint32_t)r0[x - 1] + r4[x - 1];
                    uint32_t s2_0  = (uint32_t)r0[x]     + r4[x];
                    uint32_t s2_p1 = (uint32_t)r0[x + 1] + r4[x + 1];
                    uint32_t acc = 40U * s0_0 + 26U * (s0_m1 + s0_p1 + s1_0) + 15U * (s1_m1 + s1_p1)
                                 +  7U * (s0_m2 + s0_p2 + s2_0) + 3U * (s1_m2 + s1_p2 + s2_m1 + s2_p1);
                    out[x] = (uint8_t)(acc >> 8);
                }
                uint16x8_t p0_lo, p0_hi, p1_lo, p1_hi, p2_lo, p2_hi;
                uint16x8_t c0_lo, c0_hi, c1_lo, c1_hi, c2_lo, c2_hi;
                vsum5_u8_16(r0 + lead, r1 + lead, r2 + lead, r3 + lead, r4 + lead,
                            &p0_lo, &p0_hi, &p1_lo, &p1_hi, &p2_lo, &p2_hi);
                vsum5_u8_16(r0 + x_start, r1 + x_start, r2 + x_start, r3 + x_start, r4 + x_start,
                            &c0_lo, &c0_hi, &c1_lo, &c1_hi, &c2_lo, &c2_hi);
                ZF_NO_UNROLL
                for (; x + 32 <= w; x += 16) {
                    uint16x8_t n0_lo, n0_hi, n1_lo, n1_hi, n2_lo, n2_hi;
                    vsum5_u8_16(r0 + x + 16, r1 + x + 16, r2 + x + 16, r3 + x + 16, r4 + x + 16,
                                &n0_lo, &n0_hi, &n1_lo, &n1_hi, &n2_lo, &n2_hi);
                    uint8x8_t lo = blur5_u16x8_core(
                        vextq_u16(p0_hi, c0_lo, 6), vextq_u16(p0_hi, c0_lo, 7), c0_lo,
                        vextq_u16(c0_lo, c0_hi, 1), vextq_u16(c0_lo, c0_hi, 2),
                        vextq_u16(p1_hi, c1_lo, 6), vextq_u16(p1_hi, c1_lo, 7), c1_lo,
                        vextq_u16(c1_lo, c1_hi, 1), vextq_u16(c1_lo, c1_hi, 2),
                        vextq_u16(p2_hi, c2_lo, 7), c2_lo, vextq_u16(c2_lo, c2_hi, 1));
                    uint8x8_t hi = blur5_u16x8_core(
                        vextq_u16(c0_lo, c0_hi, 6), vextq_u16(c0_lo, c0_hi, 7), c0_hi,
                        vextq_u16(c0_hi, n0_lo, 1), vextq_u16(c0_hi, n0_lo, 2),
                        vextq_u16(c1_lo, c1_hi, 6), vextq_u16(c1_lo, c1_hi, 7), c1_hi,
                        vextq_u16(c1_hi, n1_lo, 1), vextq_u16(c1_hi, n1_lo, 2),
                        vextq_u16(c2_lo, c2_hi, 7), c2_hi, vextq_u16(c2_hi, n2_lo, 1));
                    zf_st1_u8(out + x, vcombine_u8(lo, hi));
                    p0_hi = c0_hi; p1_hi = c1_hi; p2_hi = c2_hi;
                    c0_lo = n0_lo; c0_hi = n0_hi;
                    c1_lo = n1_lo; c1_hi = n1_hi;
                    c2_lo = n2_lo; c2_hi = n2_hi;
                }
            }
        }
    }
    ZF_NO_VEC_TAIL
#endif
    for (; x < w; ++x) {
        uint32_t s0_m2 = r2[x - 2], s0_m1 = r2[x - 1], s0_0 = r2[x], s0_p1 = r2[x + 1], s0_p2 = r2[x + 2];
        uint32_t s1_m2 = (uint32_t)r1[x - 2] + r3[x - 2];
        uint32_t s1_m1 = (uint32_t)r1[x - 1] + r3[x - 1];
        uint32_t s1_0  = (uint32_t)r1[x]     + r3[x];
        uint32_t s1_p1 = (uint32_t)r1[x + 1] + r3[x + 1];
        uint32_t s1_p2 = (uint32_t)r1[x + 2] + r3[x + 2];
        uint32_t s2_m1 = (uint32_t)r0[x - 1] + r4[x - 1];
        uint32_t s2_0  = (uint32_t)r0[x]     + r4[x];
        uint32_t s2_p1 = (uint32_t)r0[x + 1] + r4[x + 1];

        uint32_t acc = 40U * s0_0
                     + 26U * (s0_m1 + s0_p1 + s1_0)
                     + 15U * (s1_m1 + s1_p1)
                     +  7U * (s0_m2 + s0_p2 + s2_0)
                     +  3U * (s1_m2 + s1_p2 + s2_m1 + s2_p1);
        out[x] = (uint8_t)(acc >> 8);
    }
}

#if ZF_GRAIN_NEON
static inline int16x8_t vsum3_s16_8(const int16_t *p0, const int16_t *p1, const int16_t *p2)
{
    int16x8_t a0 = ld8a_s16(p0);
    int16x8_t a1 = ld8a_s16(p1);
    int16x8_t a2 = ld8a_s16(p2);
    return vaddq_s16(vaddq_s16(a0, a2), vshlq_n_s16(a1, 1));
}
#endif

void zf_grain_blur3_s16_row_fast(const int16_t *r0, const int16_t *r1, const int16_t *r2,
                                 int16_t *out, uint32_t width)
{
    ptrdiff_t x = 0;
    ptrdiff_t w = (ptrdiff_t)width;
#if ZF_GRAIN_NEON
    uintptr_t a0 = (uintptr_t)r0 & 15U;
    if (a0 == ((uintptr_t)r1 & 15U) && a0 == ((uintptr_t)r2 & 15U)) {
        int16x8_t v_half = vdupq_n_s16(8);
        if (a0 == 0U && w >= 24) {
            /* r0,r1,r2 all 16B aligned. Peel x=0..7 scalar. */
            ZF_NO_VEC_TAIL
            for (; x < 8; ++x) {
                int32_t vl = (int32_t)r0[x - 1] + 2 * (int32_t)r1[x - 1] + (int32_t)r2[x - 1];
                int32_t vc = (int32_t)r0[x]     + 2 * (int32_t)r1[x]     + (int32_t)r2[x];
                int32_t vr = (int32_t)r0[x + 1] + 2 * (int32_t)r1[x + 1] + (int32_t)r2[x + 1];
                out[x] = clamp_s16(round_sym_s32(vl + 2 * vc + vr, 4U));
            }
            int16x8_t v_prev = vsum3_s16_8(r0 + 0, r1 + 0, r2 + 0);
            int16x8_t v_curr = vsum3_s16_8(r0 + 8, r1 + 8, r2 + 8);
            ZF_NO_UNROLL
            for (; x + 16 <= w; x += 8) {
                int16x8_t v_next = vsum3_s16_8(r0 + x + 8, r1 + x + 8, r2 + x + 8);
                int16x8_t vl  = vextq_s16(v_prev, v_curr, 7);
                int16x8_t vr  = vextq_s16(v_curr, v_next, 1);
                int16x8_t acc = vaddq_s16(vaddq_s16(vl, vr), vshlq_n_s16(v_curr, 1));
                int16x8_t res = vshrq_n_s16(vaddq_s16(vsraq_n_s16(acc, acc, 15), v_half), 4);
                zf_st1_s16(out + x, res);
                v_prev = v_curr;
                v_curr = v_next;
            }
        } else if (a0 == 2U && w >= 15) {
            /* r-1 is 16B aligned (in-bounds!), 0 prologue peel. */
            int16x8_t v_curr = vsum3_s16_8(r0 - 1, r1 - 1, r2 - 1);
            ZF_NO_UNROLL
            for (; x + 15 <= w; x += 8) {
                int16x8_t v_next = vsum3_s16_8(r0 + x + 7, r1 + x + 7, r2 + x + 7);
                int16x8_t vc  = vextq_s16(v_curr, v_next, 1);
                int16x8_t vr  = vextq_s16(v_curr, v_next, 2);
                int16x8_t acc = vaddq_s16(vaddq_s16(v_curr, vr), vshlq_n_s16(vc, 1));
                int16x8_t res = vshrq_n_s16(vaddq_s16(vsraq_n_s16(acc, acc, 15), v_half), 4);
                zf_st1_s16(out + x, res);
                v_curr = v_next;
            }
        }
    }
    ZF_NO_VEC_TAIL
#endif
    for (; x < w; ++x) {
        int32_t vl = (int32_t)r0[x - 1] + 2 * (int32_t)r1[x - 1] + (int32_t)r2[x - 1];
        int32_t vc = (int32_t)r0[x]     + 2 * (int32_t)r1[x]     + (int32_t)r2[x];
        int32_t vr = (int32_t)r0[x + 1] + 2 * (int32_t)r1[x + 1] + (int32_t)r2[x + 1];
        out[x] = clamp_s16(round_sym_s32(vl + 2 * vc + vr, 4U));
    }
}

#if ZF_GRAIN_NEON
static inline int16x8_t blur3_s32_8(int32x4_t vl0, int32x4_t vc0, int32x4_t vr0,
                                    int32x4_t vl1, int32x4_t vc1, int32x4_t vr1, int32x4_t v_half)
{
    int32x4_t a0 = vaddq_s32(vaddq_s32(vl0, vr0), vshlq_n_s32(vc0, 1));
    int32x4_t a1 = vaddq_s32(vaddq_s32(vl1, vr1), vshlq_n_s32(vc1, 1));
    return vcombine_s16(vqshrn_n_s32(vaddq_s32(vsraq_n_s32(a0, a0, 31), v_half), 4),
                        vqshrn_n_s32(vaddq_s32(vsraq_n_s32(a1, a1, 31), v_half), 4));
}
#endif

void zf_grain_blur3_s16_row(const int16_t *r0, const int16_t *r1, const int16_t *r2,
                            int16_t *out, uint32_t width)
{
    ptrdiff_t x = 0;
    ptrdiff_t w = (ptrdiff_t)width;
#if ZF_GRAIN_NEON
    uintptr_t a0 = (uintptr_t)r0 & 15U;
    if (a0 == ((uintptr_t)r1 & 15U) && a0 == ((uintptr_t)r2 & 15U)) {
        int32x4_t v_half = vdupq_n_s32(8);
        if (a0 == 0U && w >= 24) {
            ZF_NO_VEC_TAIL
            for (; x < 8; ++x) {
                int32_t vl = (int32_t)r0[x - 1] + 2 * (int32_t)r1[x - 1] + (int32_t)r2[x - 1];
                int32_t vc = (int32_t)r0[x]     + 2 * (int32_t)r1[x]     + (int32_t)r2[x];
                int32_t vr = (int32_t)r0[x + 1] + 2 * (int32_t)r1[x + 1] + (int32_t)r2[x + 1];
                out[x] = clamp_s16(round_sym_s32(vl + 2 * vc + vr, 4U));
            }
            int16x8_t x0 = ld8a_s16(r0), x1 = ld8a_s16(r1), x2 = ld8a_s16(r2);
            int32x4_t p_hi = vaddq_s32(vaddl_high_s16(x0, x2), vshll_high_n_s16(x1, 1));
            x0 = ld8a_s16(r0 + 8); x1 = ld8a_s16(r1 + 8); x2 = ld8a_s16(r2 + 8);
            int32x4_t c_lo = vaddq_s32(vaddl_s16(vget_low_s16(x0), vget_low_s16(x2)),
                                       vshll_n_s16(vget_low_s16(x1), 1));
            int32x4_t c_hi = vaddq_s32(vaddl_high_s16(x0, x2), vshll_high_n_s16(x1, 1));
            ZF_NO_UNROLL
            for (; x + 16 <= w; x += 8) {
                x0 = ld8a_s16(r0 + x + 8); x1 = ld8a_s16(r1 + x + 8); x2 = ld8a_s16(r2 + x + 8);
                int32x4_t n_lo = vaddq_s32(vaddl_s16(vget_low_s16(x0), vget_low_s16(x2)),
                                           vshll_n_s16(vget_low_s16(x1), 1));
                int32x4_t n_hi = vaddq_s32(vaddl_high_s16(x0, x2), vshll_high_n_s16(x1, 1));
                zf_st1_s16(out + x, blur3_s32_8(
                    vextq_s32(p_hi, c_lo, 3), c_lo, vextq_s32(c_lo, c_hi, 1),
                    vextq_s32(c_lo, c_hi, 3), c_hi, vextq_s32(c_hi, n_lo, 1), v_half));
                p_hi = c_hi;
                c_lo = n_lo;
                c_hi = n_hi;
            }
        } else if (a0 == 2U && w >= 15) {
            int16x8_t x0 = ld8a_s16(r0 - 1), x1 = ld8a_s16(r1 - 1), x2 = ld8a_s16(r2 - 1);
            int32x4_t c_lo = vaddq_s32(vaddl_s16(vget_low_s16(x0), vget_low_s16(x2)),
                                       vshll_n_s16(vget_low_s16(x1), 1));
            int32x4_t c_hi = vaddq_s32(vaddl_high_s16(x0, x2), vshll_high_n_s16(x1, 1));
            ZF_NO_UNROLL
            for (; x + 15 <= w; x += 8) {
                x0 = ld8a_s16(r0 + x + 7); x1 = ld8a_s16(r1 + x + 7); x2 = ld8a_s16(r2 + x + 7);
                int32x4_t n_lo = vaddq_s32(vaddl_s16(vget_low_s16(x0), vget_low_s16(x2)),
                                           vshll_n_s16(vget_low_s16(x1), 1));
                int32x4_t n_hi = vaddq_s32(vaddl_high_s16(x0, x2), vshll_high_n_s16(x1, 1));
                zf_st1_s16(out + x, blur3_s32_8(
                    c_lo, vextq_s32(c_lo, c_hi, 1), vextq_s32(c_lo, c_hi, 2),
                    c_hi, vextq_s32(c_hi, n_lo, 1), vextq_s32(c_hi, n_lo, 2), v_half));
                c_lo = n_lo;
                c_hi = n_hi;
            }
        }
    }
    ZF_NO_VEC_TAIL
#endif
    for (; x < w; ++x) {
        int32_t vl = (int32_t)r0[x - 1] + 2 * (int32_t)r1[x - 1] + (int32_t)r2[x - 1];
        int32_t vc = (int32_t)r0[x]     + 2 * (int32_t)r1[x]     + (int32_t)r2[x];
        int32_t vr = (int32_t)r0[x + 1] + 2 * (int32_t)r1[x + 1] + (int32_t)r2[x + 1];
        out[x] = clamp_s16(round_sym_s32(vl + 2 * vc + vr, 4U));
    }
}

#if ZF_GRAIN_NEON
static inline int16x8_t blur5_s16x8_core(
    int16x8_t s0_m2, int16x8_t s0_m1, int16x8_t s0_0, int16x8_t s0_p1, int16x8_t s0_p2,
    int16x8_t s1_m2, int16x8_t s1_m1, int16x8_t s1_0, int16x8_t s1_p1, int16x8_t s1_p2,
    int16x8_t s2_m1, int16x8_t s2_0,  int16x8_t s2_p1, int32x4_t v_half)
{
    int16x8_t t26 = vaddq_s16(vaddq_s16(s0_m1, s0_p1), s1_0);
    int16x8_t t15 = vaddq_s16(s1_m1, s1_p1);
    int16x8_t t7  = vaddq_s16(vaddq_s16(s0_m2, s0_p2), s2_0);
    int16x8_t t3  = vaddq_s16(vaddq_s16(s1_m2, s1_p2), vaddq_s16(s2_m1, s2_p1));

    int32x4_t alo = vmull_n_s16(vget_low_s16(s0_0), 40);
    alo = vmlal_n_s16(alo, vget_low_s16(t26), 26);
    alo = vmlal_n_s16(alo, vget_low_s16(t15), 15);
    alo = vmlal_n_s16(alo, vget_low_s16(t7),  7);
    alo = vmlal_n_s16(alo, vget_low_s16(t3),  3);

    int32x4_t ahi = vmull_high_n_s16(s0_0, 40);
    ahi = vmlal_high_n_s16(ahi, t26, 26);
    ahi = vmlal_high_n_s16(ahi, t15, 15);
    ahi = vmlal_high_n_s16(ahi, t7,  7);
    ahi = vmlal_high_n_s16(ahi, t3,  3);

    int16x4_t rlo = vqshrn_n_s32(vaddq_s32(vsraq_n_s32(alo, alo, 31), v_half), 8);
    int16x4_t rhi = vqshrn_n_s32(vaddq_s32(vsraq_n_s32(ahi, ahi, 31), v_half), 8);
    return vcombine_s16(rlo, rhi);
}
#endif

void zf_grain_blur5_s16_row_fast(const int16_t *r0, const int16_t *r1, const int16_t *r2,
                                 const int16_t *r3, const int16_t *r4,
                                 int16_t *out, uint32_t width)
{
    ptrdiff_t x = 0;
    ptrdiff_t w = (ptrdiff_t)width;
#if ZF_GRAIN_NEON
    uintptr_t a0 = (uintptr_t)r0 & 15U;
    if ((a0 & 1U) == 0U &&
        a0 == ((uintptr_t)r1 & 15U) && a0 == ((uintptr_t)r2 & 15U) &&
        a0 == ((uintptr_t)r3 & 15U) && a0 == ((uintptr_t)r4 & 15U)) {
        int32x4_t v_half = vdupq_n_s32(128);
        if (a0 == 4U && w >= 14) {
            /* r-2 is 16B aligned (in-bounds!), 0 prologue peel. */
            int16x8_t c0 = ld8a_s16(r2 - 2);
            int16x8_t c1 = vaddq_s16(ld8a_s16(r1 - 2), ld8a_s16(r3 - 2));
            int16x8_t c2 = vaddq_s16(ld8a_s16(r0 - 2), ld8a_s16(r4 - 2));
            ZF_NO_UNROLL
            for (; x + 14 <= w; x += 8) {
                int16x8_t n0 = ld8a_s16(r2 + x + 6);
                int16x8_t n1 = vaddq_s16(ld8a_s16(r1 + x + 6), ld8a_s16(r3 + x + 6));
                int16x8_t n2 = vaddq_s16(ld8a_s16(r0 + x + 6), ld8a_s16(r4 + x + 6));
                int16x8_t res = blur5_s16x8_core(
                    c0, vextq_s16(c0, n0, 1), vextq_s16(c0, n0, 2), vextq_s16(c0, n0, 3), vextq_s16(c0, n0, 4),
                    c1, vextq_s16(c1, n1, 1), vextq_s16(c1, n1, 2), vextq_s16(c1, n1, 3), vextq_s16(c1, n1, 4),
                    vextq_s16(c2, n2, 1), vextq_s16(c2, n2, 2), vextq_s16(c2, n2, 3), v_half);
                zf_st1_s16(out + x, res);
                c0 = n0; c1 = n1; c2 = n2;
            }
        } else {
            ptrdiff_t lead = (ptrdiff_t)(((16U - a0) & 15U) >> 1);
            ptrdiff_t x_start = lead + 8;
            if (w >= x_start + 16) {
                ZF_NO_VEC_TAIL
                for (; x < x_start; ++x) {
                    int32_t s0_m2 = r2[x - 2], s0_m1 = r2[x - 1], s0_0 = r2[x],
                            s0_p1 = r2[x + 1], s0_p2 = r2[x + 2];
                    int32_t s1_m2 = (int32_t)r1[x - 2] + (int32_t)r3[x - 2];
                    int32_t s1_m1 = (int32_t)r1[x - 1] + (int32_t)r3[x - 1];
                    int32_t s1_0  = (int32_t)r1[x]     + (int32_t)r3[x];
                    int32_t s1_p1 = (int32_t)r1[x + 1] + (int32_t)r3[x + 1];
                    int32_t s1_p2 = (int32_t)r1[x + 2] + (int32_t)r3[x + 2];
                    int32_t s2_m1 = (int32_t)r0[x - 1] + (int32_t)r4[x - 1];
                    int32_t s2_0  = (int32_t)r0[x]     + (int32_t)r4[x];
                    int32_t s2_p1 = (int32_t)r0[x + 1] + (int32_t)r4[x + 1];
                    int32_t acc = 40 * s0_0 + 26 * (s0_m1 + s0_p1 + s1_0) + 15 * (s1_m1 + s1_p1)
                                +  7 * (s0_m2 + s0_p2 + s2_0) + 3 * (s1_m2 + s1_p2 + s2_m1 + s2_p1);
                    out[x] = clamp_s16(round_sym_s32(acc, 8U));
                }
                int16x8_t p0 = ld8a_s16(r2 + lead);
                int16x8_t p1 = vaddq_s16(ld8a_s16(r1 + lead), ld8a_s16(r3 + lead));
                int16x8_t p2 = vaddq_s16(ld8a_s16(r0 + lead), ld8a_s16(r4 + lead));
                int16x8_t c0 = ld8a_s16(r2 + x_start);
                int16x8_t c1 = vaddq_s16(ld8a_s16(r1 + x_start), ld8a_s16(r3 + x_start));
                int16x8_t c2 = vaddq_s16(ld8a_s16(r0 + x_start), ld8a_s16(r4 + x_start));
                ZF_NO_UNROLL
                for (; x + 16 <= w; x += 8) {
                    int16x8_t n0 = ld8a_s16(r2 + x + 8);
                    int16x8_t n1 = vaddq_s16(ld8a_s16(r1 + x + 8), ld8a_s16(r3 + x + 8));
                    int16x8_t n2 = vaddq_s16(ld8a_s16(r0 + x + 8), ld8a_s16(r4 + x + 8));
                    int16x8_t res = blur5_s16x8_core(
                        vextq_s16(p0, c0, 6), vextq_s16(p0, c0, 7), c0, vextq_s16(c0, n0, 1), vextq_s16(c0, n0, 2),
                        vextq_s16(p1, c1, 6), vextq_s16(p1, c1, 7), c1, vextq_s16(c1, n1, 1), vextq_s16(c1, n1, 2),
                        vextq_s16(p2, c2, 7), c2, vextq_s16(c2, n2, 1), v_half);
                    zf_st1_s16(out + x, res);
                    p0 = c0; p1 = c1; p2 = c2;
                    c0 = n0; c1 = n1; c2 = n2;
                }
            }
        }
    }
    ZF_NO_VEC_TAIL
#endif
    for (; x < w; ++x) {
        int32_t s0_m2 = r2[x - 2], s0_m1 = r2[x - 1], s0_0 = r2[x], s0_p1 = r2[x + 1], s0_p2 = r2[x + 2];
        int32_t s1_m2 = (int32_t)r1[x - 2] + (int32_t)r3[x - 2];
        int32_t s1_m1 = (int32_t)r1[x - 1] + (int32_t)r3[x - 1];
        int32_t s1_0  = (int32_t)r1[x]     + (int32_t)r3[x];
        int32_t s1_p1 = (int32_t)r1[x + 1] + (int32_t)r3[x + 1];
        int32_t s1_p2 = (int32_t)r1[x + 2] + (int32_t)r3[x + 2];
        int32_t s2_m1 = (int32_t)r0[x - 1] + (int32_t)r4[x - 1];
        int32_t s2_0  = (int32_t)r0[x]     + (int32_t)r4[x];
        int32_t s2_p1 = (int32_t)r0[x + 1] + (int32_t)r4[x + 1];

        int32_t acc = 40 * s0_0
                    + 26 * (s0_m1 + s0_p1 + s1_0)
                    + 15 * (s1_m1 + s1_p1)
                    +  7 * (s0_m2 + s0_p2 + s2_0)
                    +  3 * (s1_m2 + s1_p2 + s2_m1 + s2_p1);
        out[x] = clamp_s16(round_sym_s32(acc, 8U));
    }
}

void zf_grain_blur5_s16_row(const int16_t *r0, const int16_t *r1, const int16_t *r2,
                            const int16_t *r3, const int16_t *r4,
                            int16_t *out, uint32_t width)
{
    /* Full-range fallback for adversarial |r| > 4095 (exact up to +-32768). */
    ptrdiff_t x = 0;
    ptrdiff_t w = (ptrdiff_t)width;
    ZF_NO_VEC_TAIL
    for (; x < w; ++x) {
        int32_t s0_m2 = r2[x - 2], s0_m1 = r2[x - 1], s0_0 = r2[x], s0_p1 = r2[x + 1], s0_p2 = r2[x + 2];
        int32_t s1_m2 = (int32_t)r1[x - 2] + (int32_t)r3[x - 2];
        int32_t s1_m1 = (int32_t)r1[x - 1] + (int32_t)r3[x - 1];
        int32_t s1_0  = (int32_t)r1[x]     + (int32_t)r3[x];
        int32_t s1_p1 = (int32_t)r1[x + 1] + (int32_t)r3[x + 1];
        int32_t s1_p2 = (int32_t)r1[x + 2] + (int32_t)r3[x + 2];
        int32_t s2_m1 = (int32_t)r0[x - 1] + (int32_t)r4[x - 1];
        int32_t s2_0  = (int32_t)r0[x]     + (int32_t)r4[x];
        int32_t s2_p1 = (int32_t)r0[x + 1] + (int32_t)r4[x + 1];

        int32_t acc = 40 * s0_0
                    + 26 * (s0_m1 + s0_p1 + s1_0)
                    + 15 * (s1_m1 + s1_p1)
                    +  7 * (s0_m2 + s0_p2 + s2_0)
                    +  3 * (s1_m2 + s1_p2 + s2_m1 + s2_p1);
        out[x] = clamp_s16(round_sym_s32(acc, 8U));
    }
}

void zf_grain_add_delta_u8_row(const uint8_t *src_y, const int16_t *delta,
                               uint8_t *dst_y, uint32_t width)
{
    uint32_t x = 0U;
#if ZF_GRAIN_NEON
    const uint8_t *ps = src_y;
    const int16_t *pd = delta;
    uint8_t *po = dst_y;
    ZF_NO_UNROLL
    for (; x + 16U <= width; x += 16U) {
        uint8x16_t y16 = zf_ld1_u8_inc(&ps);
        int16x8_t  d0  = zf_ld1_s16_inc(&pd);
        int16x8_t  d1  = zf_ld1_s16_inc(&pd);
        int16x8_t  y0  = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(y16)));
        int16x8_t  y1  = vreinterpretq_s16_u16(vmovl_high_u8(y16));
        zf_st1_u8_inc(&po, vcombine_u8(vqmovun_s16(vqaddq_s16(y0, d0)),
                                       vqmovun_s16(vqaddq_s16(y1, d1))));
    }
    ZF_NO_VEC_TAIL
#endif
    for (; x < width; ++x) {
        dst_y[x] = clamp_u8((int32_t)src_y[x] + (int32_t)delta[x]);
    }
}

void z8_zf_grain_live_apply_tile_y_rows_clean(uint8_t *y, uint32_t y_stride,
                                              uint32_t width,
                                              uint32_t first_row, uint32_t end_row,
                                              const int8_t *tile512x256,
                                              int clean_dcache)
{
    if (!y || !tile512x256 || width == 0U || first_row >= end_row || y_stride < width) {
        return;
    }

#if ZF_GRAIN_NEON
    uint8x16_t v64 = vdupq_n_u8(64U);
#endif
    const uint32_t tw_mask = Z8_ZF_LIVE_TILE_W - 1U;
    const uint32_t th_mask = Z8_ZF_LIVE_TILE_H - 1U;

    for (uint32_t row_y = first_row; row_y < end_row; ++row_y) {
        uint8_t *row = y + (size_t)row_y * y_stride;
        const int8_t *trow = tile512x256 + (size_t)(row_y & th_mask) * Z8_ZF_LIVE_TILE_W;
        uint32_t tx = ((row_y >> 8) * Z8_ZF_LIVE_TILE_TWIST) & tw_mask;
        uint32_t x = 0U;
        uint32_t rem = width;

        while (rem > 0U) {
            uint32_t seg = Z8_ZF_LIVE_TILE_W - tx;
            if (seg > rem) seg = rem;

            uint8_t *prow = row + x;
            const int8_t *pg = trow + tx;
            uint32_t i = 0U;

#if ZF_GRAIN_NEON
            const uint8_t *ps = prow;
            uint8_t *pd = prow;
            const int8_t *pg_n = pg;
            ZF_NO_UNROLL
            for (; i + 32U <= seg; i += 32U) {
                if ((i & 63U) == 0U && seg - i >= 160U) {
                    __builtin_prefetch(ps + 128U, 1, 0);
                    __builtin_prefetch(pg_n + 128U, 0, 1);
                }
                /* 2-stream interleaved 32-px NEON (hides Cortex-A53 RAW latency, 0 Q-spills, 0-diff) */
                uint8x16_t y0   = zf_ld1_u8_inc(&ps);
                uint8x16_t y1   = zf_ld1_u8_inc(&ps);
                int8x16_t  g0   = zf_ld1_s8_inc(&pg_n);
                int8x16_t  g1   = zf_ld1_s8_inc(&pg_n);

                uint8x16_t inv0 = vmvnq_u8(y0);
                uint8x16_t inv1 = vmvnq_u8(y1);
                uint8x16_t lo0  = vqaddq_u8(y0, y0);
                uint8x16_t lo1  = vqaddq_u8(y1, y1);
                uint8x16_t hi0  = vqaddq_u8(inv0, inv0);
                uint8x16_t hi1  = vqaddq_u8(inv1, inv1);
                int8x16_t  t0   = vreinterpretq_s8_u8(vminq_u8(vminq_u8(lo0, hi0), v64));
                int8x16_t  t1   = vreinterpretq_s8_u8(vminq_u8(vminq_u8(lo1, hi1), v64));

                int16x8_t  p0lo = vrshrq_n_s16(vmull_s8(vget_low_s8(g0), vget_low_s8(t0)), 6);
                int16x8_t  p1lo = vrshrq_n_s16(vmull_s8(vget_low_s8(g1), vget_low_s8(t1)), 6);
                int16x8_t  p0hi = vrshrq_n_s16(vmull_high_s8(g0, t0), 6);
                int16x8_t  p1hi = vrshrq_n_s16(vmull_high_s8(g1, t1), 6);

                int16x8_t  s0lo = vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(p0lo), vget_low_u8(y0)));
                int16x8_t  s1lo = vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(p1lo), vget_low_u8(y1)));
                int16x8_t  s0hi = vreinterpretq_s16_u16(vaddw_high_u8(vreinterpretq_u16_s16(p0hi), y0));
                int16x8_t  s1hi = vreinterpretq_s16_u16(vaddw_high_u8(vreinterpretq_u16_s16(p1hi), y1));

                zf_st1_u8_inc(&pd, vcombine_u8(vqmovun_s16(s0lo), vqmovun_s16(s0hi)));
                zf_st1_u8_inc(&pd, vcombine_u8(vqmovun_s16(s1lo), vqmovun_s16(s1hi)));
            }
            if (i + 16U <= seg) {
                uint8x16_t y0 = zf_ld1_u8_inc(&ps);
                int8x16_t  g0 = zf_ld1_s8_inc(&pg_n);
                zf_st1_u8_inc(&pd, live_grain_apply16_srshr(y0, g0, v64));
                i += 16U;
            }
            ZF_NO_VEC_TAIL
#endif
            for (; i < seg; ++i) {
                uint32_t yv = prow[i];
                uint32_t lo = yv << 1;
                uint32_t hi = (255U - yv) << 1;
                uint32_t t = (lo < hi) ? lo : hi;
                if (t > 64U) t = 64U;
                int32_t delta = asr_s32((int32_t)pg[i] * (int32_t)t + 32, 6U);
                prow[i] = clamp_u8((int32_t)yv + delta);
            }

            x += seg;
            rem -= seg;
            tx = (tx + seg) & tw_mask;
        }
#if defined(__aarch64__) && !defined(FILMSIM_HOST_TEST) && !defined(V71R7_HOST_TEST)
        if (clean_dcache) {
            uintptr_t cp = (uintptr_t)row & ~(uintptr_t)63U;
            uintptr_t cend = ((uintptr_t)row + width + 63U) & ~(uintptr_t)63U;
            for (; cp < cend; cp += 64U) {
                __asm__ volatile("dc cvac, %0" :: "r"(cp) : "memory");
            }
        }
#endif
    }
#if defined(__aarch64__) && !defined(FILMSIM_HOST_TEST) && !defined(V71R7_HOST_TEST)
    if (clean_dcache) {
        __asm__ volatile("dsb sy" ::: "memory");
    }
#else
    (void)clean_dcache;
#endif
}

void z8_zf_grain_live_apply_tile_y_rows(uint8_t *y, uint32_t y_stride,
                                        uint32_t width,
                                        uint32_t first_row, uint32_t end_row,
                                        const int8_t *tile512x256)
{
    z8_zf_grain_live_apply_tile_y_rows_clean(y, y_stride, width, first_row, end_row, tile512x256, 0);
}

void z8_zf_grain_live_apply_tile_y(uint8_t *y, uint32_t y_stride,
                                   uint32_t width, uint32_t height,
                                   const int8_t *tile512x256)
{
    z8_zf_grain_live_apply_tile_y_rows_clean(y, y_stride, width, 0U, height, tile512x256, 0);
}
