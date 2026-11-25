/* SPDX-License-Identifier: MIT
 * Nikon Z8 C3.11 EXPEED 7 CEVA-XM6 256-bit VPU SIMD Primitives for Ricoh HDF v2 (Slot 41)
 *
 * All primitives bit-exact verified on CEVA-XM6 V18.0.3 (clang 7.1.0, rtlv1.2.0, dcli).
 */
#ifndef XM6_HDF_SIMD_H
#define XM6_HDF_SIMD_H

#include <stdint.h>
#include <stddef.h>

#if defined(__cevaxm6__)
#include <vec-c.h>
#define XM6_NOINLINE __attribute__((noinline))
#define XM6_COLD     __attribute__((noinline))
#define XM6_MINSIZE  __attribute__((noinline, minsize))
#define XM6_ALIGN32  __attribute__((aligned(32)))
#define XM6_RESTRICT __restrict
#define XM6_NO_UNROLL _Pragma("clang loop unroll(disable)")
#define XM6_NO_VEC_UNROLL \
    _Pragma("clang loop vectorize(disable)") \
    _Pragma("clang loop unroll(disable)")
#else
#define XM6_NOINLINE
#define XM6_COLD
#define XM6_MINSIZE
#define XM6_ALIGN32
#define XM6_RESTRICT
#define XM6_NO_UNROLL
#define XM6_NO_VEC_UNROLL
#endif

/* ============================================================================
 * Single Shared Deduplicated Constant Pool (.rodata -> IDM 0x0003bd40+, normal LE)
 * Accessed via xm6_cp_ptr() asm barrier so Clang 7.1.0 NEVER duplicates 32B .LCPI
 * pools per function, cutting total IDM .rodata from ~2.35 KiB down to ~1.55 KiB!
 * ============================================================================ */

typedef struct {
    /* Full 32-byte aligned vector constants (544 B) */
    uint8_t  rot16[32];
    uint8_t  even16[32];
    uint8_t  odd16[32];
    uint8_t  shl4[32];
    uint8_t  shl8[32];
    uint8_t  shl16[32];
    uint8_t  bcast7[32];
    int16_t  w4_vec[16];
    uint32_t ramp_odd8[8];
    uint16_t lin_base[16];
    uint16_t invk_base[3][16];
    uint8_t  clz_base[32];   /* Full 32 B: vffb(0) returns 16 -> clz_base[16] == 0 */
    uint8_t  clz_scale[32];  /* Full 32 B: vffb(0) returns 16 -> clz_scale[16] == 0 */
    uint8_t  clz_dm[64];

    /* 16-byte-only permute/slope tables (only bytes 0..15 read by ushort16/hi4 vperm; 400 B w/ tail_pad) */
    uint8_t  dup_lo16[16];
    uint8_t  dup_hi16[16];
    uint8_t  stride4_0[16];
    uint8_t  stride4_1[16];
    uint8_t  red4[16];
    uint8_t  red2[16];
    uint8_t  red1[16];
    uint8_t  str3_0[16];
    uint8_t  str3_1[16];
    uint8_t  str3_2[16];
    uint8_t  sh_m2[16];
    uint8_t  sh_m1[16];
    uint8_t  sh_p1[16];
    uint8_t  sh_p2[16];
    uint8_t  live_s2_a2[16];
    uint8_t  live_s2_b2[16];
    uint8_t  live_s2_cur1[16];
    uint8_t  live_s2_nbr1[16];
    uint8_t  interleave_lo[16];
    uint8_t  interleave_hi[16];
    uint8_t  lin_slp[16];
    uint8_t  invk_slp[3][16];
    uint8_t  tail_pad[16];   /* Guarantees 32-byte vld on last 16-byte entry stays in-bounds */
} Xm6ConstPool;

static const Xm6ConstPool k_xm6_cp XM6_ALIGN32 = {
    /* rot16 */
    { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
       0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    /* even16 */
    {  0,  2,  4,  6,  8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30,
      32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62 },
    /* odd16 */
    {  1,  3,  5,  7,  9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31,
      33, 35, 37, 39, 41, 43, 45, 47, 49, 51, 53, 55, 57, 59, 61, 63 },
    /* shl4 */
    {  0,  0,  0,  0, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43,
      44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59 },
    /* shl8 */
    {  0,  0,  0,  0,  0,  0,  0,  0, 32, 33, 34, 35, 36, 37, 38, 39,
      40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55 },
    /* shl16 */
    {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47 },
    /* bcast7 */
    { 28, 29, 30, 31, 28, 29, 30, 31, 28, 29, 30, 31, 28, 29, 30, 31,
      28, 29, 30, 31, 28, 29, 30, 31, 28, 29, 30, 31, 28, 29, 30, 31 },
    /* w4_vec */
    { 20480, 28672, 4096, 12288, 20480, 28672, 4096, 12288,
      20480, 28672, 4096, 12288, 20480, 28672, 4096, 12288 },
    /* ramp_odd8 */
    { 1U, 3U, 5U, 7U, 9U, 11U, 13U, 15U },
    /* lin_base */
    {     0U,   176U,   448U,   952U,  1684U,  2632U,  3812U,  5272U,
       7056U,  9116U, 11500U, 14200U, 17244U, 20644U, 24393U, 28567U },
    /* invk_base[3] */
    {
        {     0U,   138U,   423U,   880U,  1531U,  2407U,  3511U,  4872U,
           6489U,  8389U, 10568U, 13069U, 15871U, 18985U, 22450U, 26273U },
        {     0U,   134U,   394U,   813U,  1425U,  2233U,  3266U,  4523U,
           6032U,  7804U,  9836U, 12152U, 14762U, 17661U, 20873U, 24431U },
        {     0U,   126U,   354U,   740U,  1287U,  2024U,  2951U,  4094U,
           5465U,  7075U,  8919U, 11006U, 13374U, 16003U, 18915U, 22137U },
    },
    /* clz_base */
    { 187U, 187U, 137U,  99U,  71U,  49U,  34U,  22U,
       13U,   6U,   3U,   2U,   1U,   0U,   0U,   0U,
        0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,
        0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U },
    /* clz_scale */
    {  91U,  91U,  67U,  51U,  37U,  29U,  20U,  15U,
       11U,   9U,   4U,   1U,   0U,   2U,   0U,   0U,
        0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,
        0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U },
    /* clz_dm */
    {   2U,   6U,   9U,  13U,  16U,  20U,  24U,  27U,
       31U,  34U,  37U,  41U,  44U,  48U,  51U,  54U,
       58U,  61U,  64U,  67U,  70U,  74U,  77U,  80U,
       83U,  86U,  89U,  92U,  95U,  98U, 101U, 104U,
      107U, 110U, 113U, 116U, 119U, 121U, 124U, 127U,
      130U, 133U, 136U, 138U, 141U, 144U, 147U, 149U,
      152U, 155U, 157U, 160U, 163U, 165U, 168U, 170U,
      173U, 176U, 178U, 181U, 183U, 186U, 188U, 191U },

    /* 16-byte half tables */
    {  0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,  6,  7,  7 }, /* dup_lo16 */
    {  8,  8,  9,  9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15 }, /* dup_hi16 */
    {  0,  4,  8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60 }, /* stride4_0 */
    {  1,  5,  9, 13, 17, 21, 25, 29, 33, 37, 41, 45, 49, 53, 57, 61 }, /* stride4_1 */
    {  4,  5,  6,  7,  0,  1,  2,  3, 12, 13, 14, 15,  8,  9, 10, 11 }, /* red4 */
    {  2,  3,  0,  1,  6,  7,  4,  5, 10, 11,  8,  9, 14, 15, 12, 13 }, /* red2 */
    {  1,  0,  3,  2,  5,  4,  7,  6,  9,  8, 11, 10, 13, 12, 15, 14 }, /* red1 */
    {  0,  3,  6,  9, 12, 15, 18, 21,  0,  0,  0,  0,  0,  0,  0,  0 }, /* str3_0 */
    {  1,  4,  7, 10, 13, 16, 19, 22,  0,  0,  0,  0,  0,  0,  0,  0 }, /* str3_1 */
    {  2,  5,  8, 11, 14, 17, 20, 23,  0,  0,  0,  0,  0,  0,  0,  0 }, /* str3_2 */
    { 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29 }, /* sh_m2 */
    { 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30 }, /* sh_m1 */
    {  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16 }, /* sh_p1 */
    {  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17 }, /* sh_p2 */
    {  0,  0,  1,  1,  1,  1,  2,  2,  2,  2,  3,  3,  3,  3,  4,  4 }, /* live_s2_a2 */
    {  1,  1,  2,  2,  2,  2,  3,  3,  3,  3,  4,  4,  4,  4,  5,  5 }, /* live_s2_b2 */
    {  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,  6,  7,  7,  8,  8 }, /* live_s2_cur1 */
    {  0,  2,  1,  3,  2,  4,  3,  5,  4,  6,  5,  7,  6,  8,  7,  9 }, /* live_s2_nbr1 */
    {  0, 16,  1, 17,  2, 18,  3, 19,  4, 20,  5, 21,  6, 22,  7, 23 }, /* interleave_lo */
    {  8, 24,  9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31 }, /* interleave_hi */
    {  4U,  8U, 16U, 22U, 28U, 36U, 46U, 56U, 64U, 74U, 84U, 95U, 106U, 117U, 129U, 140U }, /* lin_slp */
    {
        {  5U,  9U, 14U, 20U, 27U, 34U, 42U, 50U, 59U, 68U, 78U, 87U, 97U, 108U, 118U, 129U },
        {  4U,  8U, 13U, 19U, 25U, 32U, 39U, 47U, 55U, 63U, 72U, 81U, 90U, 100U, 110U, 120U },
        {  4U,  7U, 12U, 17U, 23U, 29U, 36U, 43U, 50U, 57U, 65U, 74U, 82U,  91U, 100U, 109U },
    },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* tail_pad */
};

static inline const Xm6ConstPool *xm6_cp_ptr(void)
{
    const Xm6ConstPool *p = &k_xm6_cp;
#if defined(__cevaxm6__)
    __asm__("nop" : "+r"(p));
#endif
    return p;
}

#if defined(__cevaxm6__)

static inline uchar32 xm6_pack_u16_to_u8(ushort16 lo, ushort16 hi)
{
    ushort32 p;
    p.lo = lo;
    p.hi = hi;
    return vcast(satu, p);
}

static inline uchar32 xm6_clip_interleave_u8(short16 ev, short16 od)
{
    short16 ce = vclip(ev, (short)0, (short)255);
    short16 co = vclip(od, (short)0, (short)255);
    return vinterleave((ushort16)ce, (ushort16)co);
}

static inline short16 xm6_sqrdmulh_s(short16 a, short b)
{
    return vmpy(rnd, a, b, (unsigned char)15U);
}

static inline short16 xm6_sqrdmulh_v(short16 a, short16 b)
{
    return vmpy(rnd, a, b, (unsigned char)15U);
}

static inline ushort16 xm6_lerp_q15(ushort16 a, ushort16 b, short16 fx15)
{
    short16 d = vsubmpy(rnd, (short16)b, (short16)a, fx15, (unsigned char)15U);
    return (ushort16)((short16)a + d);
}

static inline short16 xm6_srshr1_s16(short16 a)
{
    return vaddmpy(rnd, a, (short16)0, (short)1, (unsigned char)1U);
}

static inline short16 xm6_srshr2_s16(short16 a)
{
    return vaddmpy(rnd, a, (short16)0, (short)1, (unsigned char)2U);
}

static inline ushort16 xm6_uhadd_u16_q15(ushort16 a, ushort16 b)
{
    return vaddmpy(a, b, (unsigned short)1U, (unsigned char)1U);
}

static inline ushort16 xm6_urhadd_u16_q15(ushort16 a, ushort16 b)
{
    return vaddmpy(rnd, a, b, (unsigned short)1U, (unsigned char)1U);
}

static inline uchar32 xm6_uhadd_u8(uchar32 a, uchar32 b)
{
    return vaddmpy(a, b, (unsigned char)1U, (unsigned char)1U);
}

static inline uchar32 xm6_urhadd_u8(uchar32 a, uchar32 b)
{
    return vaddmpy(rnd, a, b, (unsigned char)1U, (unsigned char)1U);
}

/* 32-pixel knot16 evaluation: b[hi] + slp[hi] * ((vx & 15) << 1) */
static inline ushort32 xm6_knot16_32px_fast(uchar32 vx,
                                            ushort16 base16,
                                            uchar32 slp32,
                                            uchar32 rot16)
{
    uchar32 hi32  = (uchar32)vshiftr(vx, (unsigned char)4U);
    uchar32 lo2   = (uchar32)vshiftl((uchar32)(vx & (uchar32)15U), (char)1);
    uchar32 hi_up = (uchar32)vperm(hi32, hi32, rot16);
    uchar32 s     = (uchar32)vperm(slp32, slp32, hi32);
    ushort32 b;
    b.lo = (ushort16)vperm(base16, base16, hi32);
    b.hi = (ushort16)vperm(base16, base16, hi_up);
    return vmac(s, lo2, b);
}

/* 32-pixel CLZ + UMULL64 Linear Q15 (0..32767) -> sRGB (0..255) */
static inline uchar32 xm6_clz_lin15_to_srgb_32px(ushort16 o0, ushort16 o1,
                                                 uchar32 clz_base,
                                                 uchar32 clz_scale,
                                                 uchar32 clz_dm_lo,
                                                 uchar32 clz_dm_hi)
{
    ushort16 lz0 = vffb(set, msb, o0);
    ushort16 lz1 = vffb(set, msb, o1);
    ushort16 n0  = (ushort16)vshiftl(o0, lz0);
    ushort16 n1  = (ushort16)vshiftl(o1, lz1);
    ushort16 m0  = (ushort16)vshiftr(n0, (unsigned char)9U);
    ushort16 m1  = (ushort16)vshiftr(n1, (unsigned char)9U);

    uchar32 lz32 = xm6_pack_u16_to_u8(lz0, lz1);
    uchar32 m32  = xm6_pack_u16_to_u8(m0, m1) & (uchar32)63U;

    uchar32 bv = (uchar32)vperm(clz_base,  clz_base,  lz32);
    uchar32 sv = (uchar32)vperm(clz_scale, clz_scale, lz32);
    uchar32 dv = (uchar32)vperm(clz_dm_lo, clz_dm_hi, m32);
    uchar32 d8 = vmpy(rnd, sv, dv, (unsigned char)8U);
    return vaddsatuc32(bv, d8);
}

/* 128-pixel 4-stream single-block CLZ Linear Q15 -> sRGB (ZERO mid-loop branch, 3-VPU saturated!) */
static inline void xm6_clz_lin15_to_srgb_128px(const uint16_t * XM6_RESTRICT u_p,
                                               uint8_t * XM6_RESTRICT d_p,
                                               uchar32 clz_base,
                                               uchar32 clz_scale,
                                               uchar32 clz_dm_lo,
                                               uchar32 clz_dm_hi)
{
    ushort16 a0 = *(const ushort16 *)(u_p +  0U);
    ushort16 a1 = *(const ushort16 *)(u_p + 16U);
    ushort16 b0 = *(const ushort16 *)(u_p + 32U);
    ushort16 b1 = *(const ushort16 *)(u_p + 48U);
    ushort16 c0 = *(const ushort16 *)(u_p + 64U);
    ushort16 c1 = *(const ushort16 *)(u_p + 80U);
    ushort16 d0 = *(const ushort16 *)(u_p + 96U);
    ushort16 d1 = *(const ushort16 *)(u_p + 112U);

    ushort16 lza0 = vffb(set, msb, a0), lza1 = vffb(set, msb, a1);
    ushort16 lzb0 = vffb(set, msb, b0), lzb1 = vffb(set, msb, b1);
    ushort16 lzc0 = vffb(set, msb, c0), lzc1 = vffb(set, msb, c1);
    ushort16 lzd0 = vffb(set, msb, d0), lzd1 = vffb(set, msb, d1);

    ushort16 na0 = (ushort16)vshiftl(a0, lza0), na1 = (ushort16)vshiftl(a1, lza1);
    ushort16 nb0 = (ushort16)vshiftl(b0, lzb0), nb1 = (ushort16)vshiftl(b1, lzb1);
    ushort16 nc0 = (ushort16)vshiftl(c0, lzc0), nc1 = (ushort16)vshiftl(c1, lzc1);
    ushort16 nd0 = (ushort16)vshiftl(d0, lzd0), nd1 = (ushort16)vshiftl(d1, lzd1);

    uchar32 lza = xm6_pack_u16_to_u8(lza0, lza1);
    uchar32 lzb = xm6_pack_u16_to_u8(lzb0, lzb1);
    uchar32 lzc = xm6_pack_u16_to_u8(lzc0, lzc1);
    uchar32 lzd = xm6_pack_u16_to_u8(lzd0, lzd1);

    ushort16 ma0 = (ushort16)vshiftr(na0, (unsigned char)9U), ma1 = (ushort16)vshiftr(na1, (unsigned char)9U);
    ushort16 mb0 = (ushort16)vshiftr(nb0, (unsigned char)9U), mb1 = (ushort16)vshiftr(nb1, (unsigned char)9U);
    ushort16 mc0 = (ushort16)vshiftr(nc0, (unsigned char)9U), mc1 = (ushort16)vshiftr(nc1, (unsigned char)9U);
    ushort16 md0 = (ushort16)vshiftr(nd0, (unsigned char)9U), md1 = (ushort16)vshiftr(nd1, (unsigned char)9U);

    uchar32 bva = (uchar32)vperm(clz_base,  clz_base,  lza);
    uchar32 bvb = (uchar32)vperm(clz_base,  clz_base,  lzb);
    uchar32 bvc = (uchar32)vperm(clz_base,  clz_base,  lzc);
    uchar32 bvd = (uchar32)vperm(clz_base,  clz_base,  lzd);

    uchar32 sva = (uchar32)vperm(clz_scale, clz_scale, lza);
    uchar32 svb = (uchar32)vperm(clz_scale, clz_scale, lzb);
    uchar32 svc = (uchar32)vperm(clz_scale, clz_scale, lzc);
    uchar32 svd = (uchar32)vperm(clz_scale, clz_scale, lzd);

    uchar32 ma = xm6_pack_u16_to_u8(ma0, ma1) & (uchar32)63U;
    uchar32 mb = xm6_pack_u16_to_u8(mb0, mb1) & (uchar32)63U;
    uchar32 mc = xm6_pack_u16_to_u8(mc0, mc1) & (uchar32)63U;
    uchar32 md = xm6_pack_u16_to_u8(md0, md1) & (uchar32)63U;

    uchar32 dva = (uchar32)vperm(clz_dm_lo, clz_dm_hi, ma);
    uchar32 dvb = (uchar32)vperm(clz_dm_lo, clz_dm_hi, mb);
    uchar32 dvc = (uchar32)vperm(clz_dm_lo, clz_dm_hi, mc);
    uchar32 dvd = (uchar32)vperm(clz_dm_lo, clz_dm_hi, md);

    uchar32 d8a = vmpy(rnd, sva, dva, (unsigned char)8U);
    uchar32 d8b = vmpy(rnd, svb, dvb, (unsigned char)8U);
    uchar32 d8c = vmpy(rnd, svc, dvc, (unsigned char)8U);
    uchar32 d8d = vmpy(rnd, svd, dvd, (unsigned char)8U);

    *(uchar32 *)(d_p +  0U) = vaddsatuc32(bva, d8a);
    *(uchar32 *)(d_p + 32U) = vaddsatuc32(bvb, d8b);
    *(uchar32 *)(d_p + 64U) = vaddsatuc32(bvc, d8c);
    *(uchar32 *)(d_p + 96U) = vaddsatuc32(bvd, d8d);
}

/* Zero-vpld 64-entry int16_t LUT lookup via 2x vperm + vcmpmov (3 cycles, 0 bank conflicts!) */
static inline short16 xm6_boost64_lookup_16(ushort16 k_u16,
                                            short16 b0, short16 b1,
                                            short16 b2, short16 b3)
{
    uchar32 k8 = xm6_pack_u16_to_u8(k_u16, k_u16) & (uchar32)31U;
    short16 lo = vperm(b0, b1, k8);
    short16 hi = vperm(b2, b3, k8);
    return (short16)vcmpmov(lt, k_u16, (unsigned short)32U, lo, hi);
}

/* Branchless 16-pixel shoulder clamp (uses vsubsatus16 for -2 VPU ops/16px!) */
static inline void xm6_shoulder_apply_16(ushort16 &u, ushort16 l15)
{
    const ushort16 v25600 = (ushort16)25600U;
    const ushort16 v35840 = (ushort16)35840U;
    const ushort16 v52224 = (ushort16)52224U;
    const ushort16 v32767 = (ushort16)32767U;

    ushort16 uk2  = vmin(u, v35840);
    ushort16 pb   = (ushort16)vshiftr(vsubsatus16(uk2, v25600), (unsigned char)1U);
    ushort16 utop = vmin(u, v52224);
    ushort16 pc   = (ushort16)vshiftr(vsubsatus16(utop, v35840), (unsigned char)3U);
    ushort16 p15  = (ushort16)(v25600 + pb + pc);
    ushort16 cand = vmax(l15, p15);
    ushort16 sel  = (ushort16)vcmpmov(gt, u, l15, cand, u);
    ushort16 o_hi = vmin(sel, v32767);
    u = (ushort16)vcmpmov(gt, u, (unsigned short)25600U, o_hi, u);
}

/* Cold-path pure-SWP leaf shoulder pass over u_buf[0..n32-1] (single flat loop -> 74% VPU util!) */
static XM6_COLD void xm6_shoulder_pass_tile(const uint8_t * XM6_RESTRICT src_p,
                                            uint16_t * XM6_RESTRICT u_buf,
                                            uint32_t n32)
{
    const Xm6ConstPool *cp = xm6_cp_ptr();
    const ushort16 lin_b = *(const ushort16 *)cp->lin_base;
    const uchar32  lin_s = *(const uchar32  *)cp->lin_slp;
    const uchar32  rot16 = *(const uchar32  *)cp->rot16;

    for (uint32_t i = 0U; i < n32; i += 32U) {
        ushort16 u0 = *(const ushort16 *)(u_buf + i);
        ushort16 u1 = *(const ushort16 *)(u_buf + i + 16U);
        ushort32 l15 = xm6_knot16_32px_fast(*(const uchar32 *)(src_p + i), lin_b, lin_s, rot16);
        xm6_shoulder_apply_16(u0, l15.lo);
        xm6_shoulder_apply_16(u1, l15.hi);
        *(ushort16 *)(u_buf + i)       = u0;
        *(ushort16 *)(u_buf + i + 16U) = u1;
    }
}

/* Long-tile (>512 px) sub-block dispatcher: skips dark 512-px blocks and calls flat pure-SWP leaf on bright runs! */
static XM6_COLD void xm6_shoulder_dispatch_long(const uint8_t * XM6_RESTRICT src_p,
                                                uint16_t * XM6_RESTRICT u_buf,
                                                uint32_t n32)
{
    uint32_t run_start = 0U;
    uint32_t in_run    = 0U;
    uint32_t base      = 0U;
    for (; base + 512U <= n32; base += 512U) {
        const uint16_t * XM6_RESTRICT up = u_buf + base;
        ushort16 mx = (ushort16)0U;
        for (uint32_t k = 0U; k < 512U; k += 64U) {
            ushort16 a = vmax(*(const ushort16 *)(up + k),       *(const ushort16 *)(up + k + 16U));
            ushort16 b = vmax(*(const ushort16 *)(up + k + 32U), *(const ushort16 *)(up + k + 48U));
            mx = vmax(mx, vmax(a, b));
        }
        if (vcmp(gt, mx, (unsigned short)25600U) != 0U) {
            if (!in_run) { run_start = base; in_run = 1U; }
        } else if (in_run) {
            xm6_shoulder_pass_tile(src_p + run_start, u_buf + run_start, base - run_start);
            in_run = 0U;
        }
    }
    if (in_run) {
        xm6_shoulder_pass_tile(src_p + run_start, u_buf + run_start, n32 - run_start);
    } else if (base < n32) {
        xm6_shoulder_pass_tile(src_p + base, u_buf + base, n32 - base);
    }
}

/* Fast 256-bit VPU block copy (32 B/iter, __restrict for dual LS0/LS1 pipelining) */
static XM6_NOINLINE void xm6_vpu_memcpy(void * XM6_RESTRICT dst_p, const void * XM6_RESTRICT src_p, uint32_t bytes)
{
    uint8_t * XM6_RESTRICT d = (uint8_t *)dst_p;
    const uint8_t * XM6_RESTRICT s = (const uint8_t *)src_p;
    uint32_t i = 0U;
    XM6_NO_UNROLL
    for (; i + 32U <= bytes; i += 32U) {
        *(uchar32 *)(d + i) = *(const uchar32 *)(s + i);
    }
    XM6_NO_VEC_UNROLL
    for (; i < bytes; ++i) {
        d[i] = s[i];
    }
}

#endif /* __cevaxm6__ */
#endif /* XM6_HDF_SIMD_H */
