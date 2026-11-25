/* SPDX-License-Identifier: MIT
 * Pixel & multi-scale optical scatter kernels for z8_hdf.
 * Includes Lever 5 (100% bit-exact shared kernel speedups) and
 * V71 Live Fast LIVE_256_YC Q15/Q13 kernels (1024x{680,682} NV12).
 * Scalar C11 (-DHDF_SCALAR) and AArch64 NEON produce 100% bit-for-bit identical results.
 */
#include "z8_hdf_kernels.h"

#if HDF_NEON
#include <arm_neon.h>

static inline uint8x16_t hdf_ld1_u8(const uint8_t *p) {
    uint8x16_t v;
    __asm__("ld1 {%0.16b}, [%1]" : "=w"(v) : "r"(p) : "memory");
    return v;
}
static inline void hdf_st1_u8(uint8_t *p, uint8x16_t v) {
    __asm__ volatile("st1 {%0.16b}, [%1]" : : "w"(v), "r"(p) : "memory");
}
static inline uint16x8_t hdf_ld1_u16(const uint16_t *p) {
    uint16x8_t v;
    __asm__("ld1 {%0.8h}, [%1]" : "=w"(v) : "r"(p) : "memory");
    return v;
}
static inline void hdf_st1_u16(uint16_t *p, uint16x8_t v) {
    __asm__ volatile("st1 {%0.8h}, [%1]" : : "w"(v), "r"(p) : "memory");
}
static inline int16x8_t hdf_ld1_s16(const int16_t *p) {
    int16x8_t v;
    __asm__("ld1 {%0.8h}, [%1]" : "=w"(v) : "r"(p) : "memory");
    return v;
}
static inline void hdf_st1_s16(int16_t *p, int16x8_t v) {
    __asm__ volatile("st1 {%0.8h}, [%1]" : : "w"(v), "r"(p) : "memory");
}
static inline uint32x4_t hdf_ld1_u32(const uint32_t *p) {
    uint32x4_t v;
    __asm__("ld1 {%0.4s}, [%1]" : "=w"(v) : "r"(p) : "memory");
    return v;
}
static inline void hdf_st1_u32(uint32_t *p, uint32x4_t v) {
    __asm__ volatile("st1 {%0.4s}, [%1]" : : "w"(v), "r"(p) : "memory");
}
/* Strictly 16-byte aligned helpers ONLY for _Alignas(16) L1 stack tiles in hdf_composite_row
 * (zero "memory" clobbers -> enables ldp/stp q & instruction scheduling, 100% SCTLR_EL1.A safe). */
static inline uint8x16_t hdf_ld16a_u8(const uint8_t *p) {
    return *(const uint8x16_t *)__builtin_assume_aligned(p, 16);
}
static inline void hdf_st16a_u8(uint8_t *p, uint8x16_t v) {
    *(uint8x16_t *)__builtin_assume_aligned(p, 16) = v;
}
static inline uint16x8_t hdf_ld8a_u16(const uint16_t *p) {
    return *(const uint16x8_t *)__builtin_assume_aligned(p, 16);
}
static inline void hdf_st8a_u16(uint16_t *p, uint16x8_t v) {
    *(uint16x8_t *)__builtin_assume_aligned(p, 16) = v;
}
#endif

/* ---- .rodata sRGB <-> Linear Q16 Transfer LUTs (Exact Path) ------------- */

const uint16_t _Alignas(16) hdf_srgb_to_lin_u16[256] = {
        0U,    20U,    40U,    60U,    80U,    99U,   119U,   139U,
      159U,   179U,   199U,   219U,   241U,   264U,   288U,   313U,
      340U,   367U,   396U,   427U,   458U,   491U,   526U,   562U,
      599U,   637U,   677U,   718U,   761U,   805U,   851U,   898U,
      947U,   997U,  1048U,  1101U,  1156U,  1212U,  1270U,  1330U,
     1391U,  1453U,  1517U,  1583U,  1651U,  1720U,  1790U,  1863U,
     1937U,  2013U,  2090U,  2170U,  2250U,  2333U,  2418U,  2504U,
     2592U,  2681U,  2773U,  2866U,  2961U,  3058U,  3157U,  3258U,
     3360U,  3464U,  3570U,  3678U,  3788U,  3900U,  4014U,  4129U,
     4247U,  4366U,  4488U,  4611U,  4736U,  4864U,  4993U,  5124U,
     5257U,  5392U,  5530U,  5669U,  5810U,  5953U,  6099U,  6246U,
     6395U,  6547U,  6700U,  6856U,  7014U,  7174U,  7335U,  7500U,
     7666U,  7834U,  8004U,  8177U,  8352U,  8528U,  8708U,  8889U,
     9072U,  9258U,  9445U,  9635U,  9828U, 10022U, 10219U, 10417U,
    10619U, 10822U, 11028U, 11235U, 11446U, 11658U, 11873U, 12090U,
    12309U, 12530U, 12754U, 12980U, 13209U, 13440U, 13673U, 13909U,
    14146U, 14387U, 14629U, 14874U, 15122U, 15371U, 15623U, 15878U,
    16135U, 16394U, 16656U, 16920U, 17187U, 17456U, 17727U, 18001U,
    18277U, 18556U, 18837U, 19121U, 19407U, 19696U, 19987U, 20281U,
    20577U, 20876U, 21177U, 21481U, 21787U, 22096U, 22407U, 22721U,
    23038U, 23357U, 23678U, 24002U, 24329U, 24658U, 24990U, 25325U,
    25662U, 26001U, 26344U, 26688U, 27036U, 27386U, 27739U, 28094U,
    28452U, 28813U, 29176U, 29542U, 29911U, 30282U, 30656U, 31033U,
    31412U, 31794U, 32179U, 32567U, 32957U, 33350U, 33745U, 34143U,
    34544U, 34948U, 35355U, 35764U, 36176U, 36591U, 37008U, 37429U,
    37852U, 38278U, 38706U, 39138U, 39572U, 40009U, 40449U, 40891U,
    41337U, 41785U, 42236U, 42690U, 43147U, 43606U, 44069U, 44534U,
    45002U, 45473U, 45947U, 46423U, 46903U, 47385U, 47871U, 48359U,
    48850U, 49344U, 49841U, 50341U, 50844U, 51349U, 51858U, 52369U,
    52884U, 53401U, 53921U, 54445U, 54971U, 55500U, 56032U, 56567U,
    57105U, 57646U, 58190U, 58737U, 59287U, 59840U, 60396U, 60955U,
    61517U, 62082U, 62650U, 63221U, 63795U, 64372U, 64952U, 65535U,
};

#if HDF_NEON
static const uint8_t _Alignas(16) hdf_srgb_lin_lo[256] = {
      0U,  20U,  40U,  60U,  80U,  99U, 119U, 139U, 159U, 179U, 199U, 219U, 241U,   8U,  32U,  57U,
     84U, 111U, 140U, 171U, 202U, 235U,  14U,  50U,  87U, 125U, 165U, 206U, 249U,  37U,  83U, 130U,
    179U, 229U,  24U,  77U, 132U, 188U, 246U,  50U, 111U, 173U, 237U,  47U, 115U, 184U, 254U,  71U,
    145U, 221U,  42U, 122U, 202U,  29U, 114U, 200U,  32U, 121U, 213U,  50U, 145U, 242U,  85U, 186U,
     32U, 136U, 242U,  94U, 204U,  60U, 174U,  33U, 151U,  14U, 136U,   3U, 128U,   0U, 129U,   4U,
    137U,  16U, 154U,  37U, 178U,  65U, 211U, 102U, 251U, 147U,  44U, 200U, 102U,   6U, 167U,  76U,
    242U, 154U,  68U, 241U, 160U,  80U,   4U, 185U, 112U,  42U, 229U, 163U, 100U,  38U, 235U, 177U,
    123U,  70U,  20U, 227U, 182U, 138U,  97U,  58U,  21U, 242U, 210U, 180U, 153U, 128U, 105U,  85U,
     66U,  51U,  37U,  26U,  18U,  11U,   7U,   6U,   7U,  10U,  16U,  24U,  35U,  48U,  63U,  81U,
    101U, 124U, 149U, 177U, 207U, 240U,  19U,  57U,  97U, 140U, 185U, 233U,  27U,  80U, 135U, 193U,
    254U,  61U, 126U, 194U,   9U,  82U, 158U, 237U,  62U, 145U, 232U,  64U, 156U, 250U,  91U, 190U,
     36U, 141U, 248U, 102U, 215U,  74U, 192U,  57U, 180U,  50U, 179U,  55U, 189U,  70U, 209U,  95U,
    240U, 132U,  27U, 180U,  80U, 239U, 144U,  53U, 220U, 134U,  50U, 226U, 148U,  73U,   1U, 187U,
    121U,  57U, 252U, 194U, 139U,  86U,  37U, 246U, 202U, 161U, 123U,  87U,  55U,  25U, 255U, 231U,
    210U, 192U, 177U, 165U, 156U, 149U, 146U, 145U, 148U, 153U, 161U, 173U, 187U, 204U, 224U, 247U,
     17U,  46U,  78U, 113U, 151U, 192U, 236U,  27U,  77U, 130U, 186U, 245U,  51U, 116U, 184U, 255U,
};

static const uint8_t _Alignas(16) hdf_srgb_lin_hi[256] = {
      0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,   1U,   1U,   1U,
      1U,   1U,   1U,   1U,   1U,   1U,   2U,   2U,   2U,   2U,   2U,   2U,   2U,   3U,   3U,   3U,
      3U,   3U,   4U,   4U,   4U,   4U,   4U,   5U,   5U,   5U,   5U,   6U,   6U,   6U,   6U,   7U,
      7U,   7U,   8U,   8U,   8U,   9U,   9U,   9U,  10U,  10U,  10U,  11U,  11U,  11U,  12U,  12U,
     13U,  13U,  13U,  14U,  14U,  15U,  15U,  16U,  16U,  17U,  17U,  18U,  18U,  19U,  19U,  20U,
     20U,  21U,  21U,  22U,  22U,  23U,  23U,  24U,  24U,  25U,  26U,  26U,  27U,  28U,  28U,  29U,
     29U,  30U,  31U,  31U,  32U,  33U,  34U,  34U,  35U,  36U,  36U,  37U,  38U,  39U,  39U,  40U,
     41U,  42U,  43U,  43U,  44U,  45U,  46U,  47U,  48U,  48U,  49U,  50U,  51U,  52U,  53U,  54U,
     55U,  56U,  57U,  58U,  59U,  60U,  61U,  62U,  63U,  64U,  65U,  66U,  67U,  68U,  69U,  70U,
     71U,  72U,  73U,  74U,  75U,  76U,  78U,  79U,  80U,  81U,  82U,  83U,  85U,  86U,  87U,  88U,
     89U,  91U,  92U,  93U,  95U,  96U,  97U,  98U, 100U, 101U, 102U, 104U, 105U, 106U, 108U, 109U,
    111U, 112U, 113U, 115U, 116U, 118U, 119U, 121U, 122U, 124U, 125U, 127U, 128U, 130U, 131U, 133U,
    134U, 136U, 138U, 139U, 141U, 142U, 144U, 146U, 147U, 149U, 151U, 152U, 154U, 156U, 158U, 159U,
    161U, 163U, 164U, 166U, 168U, 170U, 172U, 173U, 175U, 177U, 179U, 181U, 183U, 185U, 186U, 188U,
    190U, 192U, 194U, 196U, 198U, 200U, 202U, 204U, 206U, 208U, 210U, 212U, 214U, 216U, 218U, 220U,
    223U, 225U, 227U, 229U, 231U, 233U, 235U, 238U, 240U, 242U, 244U, 246U, 249U, 251U, 253U, 255U,
};
#endif

static const uint8_t _Alignas(16) hdf_lin_srgb_t0[64] = {
      0U,   1U,   2U,   3U,   4U,   4U,   5U,   6U,   7U,   8U,   8U,   9U,  10U,  11U,  12U,  12U,
     13U,  14U,  14U,  15U,  16U,  16U,  17U,  17U,  18U,  18U,  19U,  19U,  20U,  20U,  21U,  21U,
     22U,  22U,  23U,  23U,  24U,  24U,  24U,  25U,  25U,  26U,  26U,  26U,  27U,  27U,  28U,  28U,
     28U,  29U,  29U,  29U,  30U,  30U,  30U,  31U,  31U,  31U,  32U,  32U,  32U,  33U,  33U,  33U,
};

static const uint8_t _Alignas(16) hdf_lin_srgb_t1[64] = {
     34U,  34U,  35U,  36U,  36U,  37U,  37U,  38U,  38U,  39U,  40U,  40U,  41U,  41U,  42U,  42U,
     43U,  43U,  43U,  44U,  44U,  45U,  45U,  46U,  46U,  47U,  47U,  48U,  48U,  48U,  49U,  49U,
     50U,  50U,  50U,  51U,  51U,  52U,  52U,  52U,  53U,  53U,  54U,  54U,  54U,  55U,  55U,  55U,
     56U,  56U,  57U,  57U,  57U,  58U,  58U,  58U,  59U,  59U,  59U,  60U,  60U,  60U,  61U,  61U,
};

static const uint8_t _Alignas(16) hdf_lin_srgb_t2[64] = {
     61U,  62U,  63U,  63U,  64U,  65U,  65U,  66U,  66U,  67U,  68U,  68U,  69U,  69U,  70U,  70U,
     71U,  72U,  72U,  73U,  73U,  74U,  74U,  75U,  75U,  76U,  76U,  77U,  77U,  78U,  78U,  79U,
     79U,  80U,  80U,  81U,  81U,  82U,  82U,  83U,  83U,  83U,  84U,  84U,  85U,  85U,  86U,  86U,
     87U,  87U,  87U,  88U,  88U,  89U,  89U,  90U,  90U,  90U,  91U,  91U,  92U,  92U,  92U,  93U,
};

static const uint8_t _Alignas(16) hdf_lin_srgb_cb_lo[64] = {
      0U, 136U, 117U,  36U, 183U, 248U,  79U, 247U,  22U, 199U,  28U,  35U, 232U, 115U, 202U, 244U,
    246U, 211U, 142U,  43U, 172U,  19U,  99U, 156U, 192U, 209U, 208U, 190U, 156U, 107U,  43U, 222U,
    132U,  30U, 172U,  47U, 168U,  22U, 122U, 213U,  40U, 113U, 178U, 236U,  29U,  72U, 107U, 135U,
    157U, 172U, 181U, 183U, 180U, 172U, 158U, 138U, 113U,  83U,  49U,   9U, 221U, 172U, 119U,  62U,
};

static const uint8_t _Alignas(16) hdf_lin_srgb_cb_hi[64] = {
      0U,  33U,  49U,  61U,  70U,  78U,  86U,  92U,  99U, 104U, 110U, 115U, 119U, 124U, 128U, 132U,
    136U, 140U, 144U, 148U, 151U, 155U, 158U, 161U, 164U, 167U, 170U, 173U, 176U, 179U, 182U, 184U,
    187U, 190U, 192U, 195U, 197U, 200U, 202U, 204U, 207U, 209U, 211U, 213U, 216U, 218U, 220U, 222U,
    224U, 226U, 228U, 230U, 232U, 234U, 236U, 238U, 240U, 242U, 244U, 246U, 247U, 249U, 251U, 253U,
};

/* Valid for k = L >> 10 >= 7 (L >= 7168); k < 7 handled by t0/t1/t2. */
static const uint8_t _Alignas(16) hdf_lin_srgb_cs[64] = {
      0U,   0U,   0U,   0U,   0U,   0U,   0U, 196U, 182U, 171U, 161U, 153U, 145U, 139U, 133U, 128U,
    124U, 119U, 116U, 112U, 109U, 106U, 103U, 101U,  98U,  96U,  94U,  92U,  90U,  88U,  86U,  85U,
     83U,  82U,  80U,  79U,  78U,  77U,  75U,  74U,  73U,  72U,  71U,  70U,  69U,  68U,  68U,  67U,
     66U,  65U,  64U,  64U,  63U,  62U,  62U,  61U,  60U,  60U,  59U,  59U,  58U,  57U,  57U,  56U,
};

/* ---- .rodata Q15 Transfer & Permute LUTs (V71r2 LIVE_256_YC Path) ------- */

/* Q15 16-knot sRGB -> Linear Q15 (0..32767): 1-reg TBL (hi = x>>4, lo2 = (x&15)<<1).
 * For hi=0..9 (x<=159), base is a multiple of 4 and slope is even so that
 * ((lin15(x) + 2) >> 2) == q13b(x) bit-for-bit identically!
 */
static const uint8_t _Alignas(16) k_live_s2l16_lin_blo[16] = {
      0U, 176U, 192U, 184U, 148U,  72U, 228U, 152U,
    144U, 156U, 236U, 120U,  92U, 164U,  73U, 151U,
};
static const uint8_t _Alignas(16) k_live_s2l16_lin_bhi[16] = {
      0U,   0U,   1U,   3U,   6U,  10U,  14U,  20U,
     27U,  35U,  44U,  55U,  67U,  80U,  95U, 111U,
};
static const uint8_t _Alignas(16) k_live_s2l16_lin_slp[16] = {
      4U,   8U,  16U,  22U,  28U,  36U,  46U,  56U,
     64U,  74U,  84U,  95U, 106U, 117U, 129U, 140U,
};

#if HDF_NEON
/* Exact Q13 base table for Stage 1 fast path x<=158: hi=x>>4, lo=x&15 (no x+x, no >>2) */
static const uint8_t _Alignas(16) k_live_s2l16_q13b_blo[16] = {
      0U,  44U, 112U, 238U, 165U, 146U, 185U,  38U,
    228U, 231U,   0U,   0U,   0U,   0U,   0U,   0U,
};
static const uint8_t _Alignas(16) k_live_s2l16_q13b_bhi[16] = {
      0U,   0U,   0U,   0U,   1U,   2U,   3U,   5U,
      6U,   8U,   0U,   0U,   0U,   0U,   0U,   0U,
};
static const uint8_t _Alignas(16) k_live_s2l16_q13b_slp[16] = {
      2U,   4U,   8U,  11U,  14U,  18U,  23U,  28U,
     32U,  37U,   0U,   0U,   0U,   0U,   0U,   0U,
};
#endif

/* Q15 16-knot sRGB -> (1-k)*Linear Q15 per HDF level [0=WEAK, 1=MED, 2=STRONG] */
static const uint8_t _Alignas(16) k_live_s2l16_invk_blo[3][16] = {
    {   0U, 138U, 167U, 112U, 251U, 103U, 183U,   8U,
       89U, 197U,  72U,  13U, 255U,  41U, 178U, 161U },
    {   0U, 134U, 138U,  45U, 145U, 185U, 194U, 171U,
      144U, 124U, 108U, 120U, 170U, 253U, 137U, 111U },
    {   0U, 126U,  98U, 228U,   7U, 232U, 135U, 254U,
       89U, 163U, 215U, 254U,  62U, 131U, 227U, 121U },
};
static const uint8_t _Alignas(16) k_live_s2l16_invk_bhi[3][16] = {
    {   0U,   0U,   1U,   3U,   5U,   9U,  13U,  19U,
       25U,  32U,  41U,  51U,  61U,  74U,  87U, 102U },
    {   0U,   0U,   1U,   3U,   5U,   8U,  12U,  17U,
       23U,  30U,  38U,  47U,  57U,  68U,  81U,  95U },
    {   0U,   0U,   1U,   2U,   5U,   7U,  11U,  15U,
       21U,  27U,  34U,  42U,  52U,  62U,  73U,  86U },
};
static const uint8_t _Alignas(16) k_live_s2l16_invk_slp[3][16] = {
    {   5U,   9U,  14U,  20U,  27U,  34U,  42U,  50U,
       59U,  68U,  78U,  87U,  97U, 108U, 118U, 129U },
    {   4U,   8U,  13U,  19U,  25U,  32U,  39U,  47U,
       55U,  63U,  72U,  81U,  90U, 100U, 110U, 120U },
    {   4U,   7U,  12U,  17U,  23U,  29U,  36U,  43U,
       50U,  57U,  65U,  74U,  82U,  91U, 100U, 109U },
};

/* 16-insn Q15 CLZ+UMULL+ADD Linear Q15 (0..32767) -> sRGB (0..255):
 * RMS 0.480 LSB, max_err 1 LSB everywhere, exact 0->0 and 32767->255, true sRGB toe.
 */
static const uint8_t _Alignas(16) k_live_clz_base[16] = {
    187U, 187U, 137U,  99U,  71U,  49U,  34U,  22U,
     13U,   6U,   3U,   2U,   1U,   0U,   0U,   0U,
};
static const uint8_t _Alignas(16) k_live_clz_scale[16] = {
     91U,  91U,  67U,  51U,  37U,  29U,  20U,  15U,
     11U,   9U,   4U,   1U,   0U,   2U,   0U,   0U,
};
static const uint8_t _Alignas(16) k_live_clz_dm[64] = {
      2U,   6U,   9U,  13U,  16U,  20U,  24U,  27U,
     31U,  34U,  37U,  41U,  44U,  48U,  51U,  54U,
     58U,  61U,  64U,  67U,  70U,  74U,  77U,  80U,
     83U,  86U,  89U,  92U,  95U,  98U, 101U, 104U,
    107U, 110U, 113U, 116U, 119U, 121U, 124U, 127U,
    130U, 133U, 136U, 138U, 141U, 144U, 147U, 149U,
    152U, 155U, 157U, 160U, 163U, 165U, 168U, 170U,
    173U, 176U, 178U, 181U, 183U, 186U, 188U, 191U,
};

#if HDF_NEON
/* Permute byte tables for single-load 4x pixel-center Q13 hlerp */
static const uint8_t _Alignas(16) k_live_p4_a0[16] = { 0,1, 0,1, 2,3, 2,3, 2,3, 2,3, 4,5, 4,5 };
static const uint8_t _Alignas(16) k_live_p4_b0[16] = { 2,3, 2,3, 4,5, 4,5, 4,5, 4,5, 6,7, 6,7 };
static const uint8_t _Alignas(16) k_live_p4_a1[16] = { 4,5, 4,5, 6,7, 6,7, 6,7, 6,7, 8,9, 8,9 };
static const uint8_t _Alignas(16) k_live_p4_b1[16] = { 6,7, 6,7, 8,9, 8,9, 8,9, 8,9, 10,11, 10,11 };
static const int16_t _Alignas(16) k_live_w4[8]     = { 20480, 28672, 4096, 12288, 20480, 28672, 4096, 12288 };

/* Permute byte tables for EVF 1280x{850..856} 2.5x horizontal down/upsampling */
static const uint8_t _Alignas(8)  k_evf_r52_u8_a[8]  = {  0,  4,  5,  9, 10, 14, 15, 31 };
static const uint8_t _Alignas(8)  k_evf_r52_u8_b[8]  = {  1,  3,  6,  8, 11, 13, 28, 30 };
static const uint8_t _Alignas(8)  k_evf_r52_u8_c[8]  = {  2,  2,  7,  7, 12, 12, 29, 29 };
static const uint8_t _Alignas(16) k_evf_r52_u16_a[16] = {
     0, 1,  8, 9, 10,11, 18,19, 20,21, 28,29, 30,31, 46,47 };
static const uint8_t _Alignas(16) k_evf_r52_u16_b[16] = {
     2, 3,  6, 7, 12,13, 16,17, 22,23, 26,27, 40,41, 44,45 };
static const uint8_t _Alignas(16) k_evf_r52_u16_c[16] = {
     4, 5,  4, 5, 14,15, 14,15, 24,25, 24,25, 42,43, 42,43 };
static const uint8_t _Alignas(16) k_evf_hl_p0[16] = {
     0, 1,  8, 9, 16,17, 24,25, 32,33,  2, 3, 10,11, 18,19 };
static const uint8_t _Alignas(16) k_evf_hl_p1[16] = {
    26,27, 34,35,  4, 5, 12,13, 20,21, 28,29, 36,37,  6, 7 };
static const uint8_t _Alignas(8)  k_evf_hl_p2[8]  = {
    14,15, 22,23, 30,31, 38,39 };
static const uint8_t _Alignas(16) k_evf_exp_p0[16] = {
     0, 1,  0, 1, 16,17,  2, 3,  2, 3,  4, 5,  4, 5, 18,19 };
static const uint8_t _Alignas(16) k_evf_exp_p1[16] = {
     6, 7,  6, 7,  8, 9,  8, 9, 20,21, 10,11, 10,11, 12,13 };
static const uint8_t _Alignas(8)  k_evf_exp_p2[8]  = {
    12,13, 22,23, 14,15, 14,15 };
#endif

static inline uint8_t clamp_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static inline int16_t sqrdmulh_s16_scalar(int16_t a, int16_t b) {
    if (a == -32768 && b == -32768) return 32767;
    int64_t p = (int64_t)a * (int64_t)b * 2 + 32768;
    int64_t q = p >= 0 ? (p / 65536) : -((-p + 65535) / 65536);
    return (int16_t)q;
}

static inline int16_t srshr1_s16_scalar(int16_t a) {
    int32_t v = (int32_t)a + 1;
    return (int16_t)(v >= 0 ? (v / 2) : -((-v + 1) / 2));
}

static inline int16_t srshr2_s16_scalar(int16_t a) {
    int32_t v = (int32_t)a + 2;
    return (int16_t)(v >= 0 ? (v / 4) : -((-v + 3) / 4));
}

static inline uint32_t clz16_scalar(uint16_t v) {
    if (v == 0U) return 16U;
    uint32_t n = 0U;
    if ((v & 0xFF00U) == 0U) { n += 8U; v = (uint16_t)(v << 8); }
    if ((v & 0xF000U) == 0U) { n += 4U; v = (uint16_t)(v << 4); }
    if ((v & 0xC000U) == 0U) { n += 2U; v = (uint16_t)(v << 2); }
    if ((v & 0x8000U) == 0U) { n += 1U; }
    return n;
}

static inline uint16_t live_knot16_eval_scalar(uint8_t x,
                                               const uint8_t blo[16],
                                               const uint8_t bhi[16],
                                               const uint8_t slp[16]) {
    uint32_t hi  = (uint32_t)x >> 4;
    uint32_t lo2 = ((uint32_t)x & 15U) << 1;
    uint32_t b   = (uint32_t)blo[hi] | ((uint32_t)bhi[hi] << 8);
    return (uint16_t)(b + (uint32_t)slp[hi] * lo2);
}

static inline uint8_t live_clz_umull64_scalar(uint16_t o) {
    uint32_t lz = clz16_scalar(o);
    uint16_t n  = (uint16_t)((uint32_t)o << lz);
    uint8_t n_hi = (uint8_t)(n >> 8);
    uint8_t m_idx = (uint8_t)(((uint8_t)(n_hi + n_hi)) >> 2);
    uint8_t bv = lz < 16U ? k_live_clz_base[lz]  : 0U;
    uint8_t sv = lz < 16U ? k_live_clz_scale[lz] : 0U;
    uint8_t dv = k_live_clz_dm[m_idx];
    uint32_t d8 = ((uint32_t)sv * (uint32_t)dv + 128U) >> 8;
    uint32_t out = (uint32_t)bv + d8;
    return (uint8_t)(out > 255U ? 255U : out);
}

static inline uint8_t live_comp_px_scalar(uint8_t vx, uint16_t sk, uint32_t lvl_idx) {
    uint16_t t1 = live_knot16_eval_scalar(vx, k_live_s2l16_invk_blo[lvl_idx],
                                          k_live_s2l16_invk_bhi[lvl_idx],
                                          k_live_s2l16_invk_slp[lvl_idx]);
    uint16_t u15 = (uint16_t)(t1 + sk);
    uint16_t o15 = u15;
    if (u15 > 25600U) {
        uint16_t l15 = live_knot16_eval_scalar(vx, k_live_s2l16_lin_blo,
                                               k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
        uint32_t pa = 25600U;
        uint32_t uk2 = u15 < 35840U ? u15 : 35840U;
        uint32_t pb = (uk2 - 25600U) >> 1;
        uint32_t utop = u15 < 52224U ? u15 : 52224U;
        uint32_t pc = (utop > 35840U ? (utop - 35840U) : 0U) >> 3;
        uint16_t p15 = (uint16_t)(pa + pb + pc);
        o15 = (u15 > l15) ? (l15 > p15 ? l15 : p15) : u15;
        if (o15 > 32767U) o15 = 32767U;
    }
    return live_clz_umull64_scalar(o15);
}

uint8_t hdf_lin_to_srgb_scalar(uint16_t L) {
    if (L < 1024U) return hdf_lin_srgb_t0[L >> 4];
    if (L < 3072U) return hdf_lin_srgb_t1[(L - 1024U) >> 5];
    if (L < 7168U) return hdf_lin_srgb_t2[(L - 3072U) >> 6];
    uint32_t k = (uint32_t)L >> 10;
    uint32_t rem = (uint32_t)L & 1023U;
    uint32_t b = (uint32_t)hdf_lin_srgb_cb_lo[k] | ((uint32_t)hdf_lin_srgb_cb_hi[k] << 8);
    uint32_t q8 = b + (((uint32_t)hdf_lin_srgb_cs[k] * rem + 64U) >> 7);
    uint32_t v = (q8 + 128U) >> 8;
    return (uint8_t)(v > 255U ? 255U : v);
}

#if HDF_NEON
static inline void neon_ycc16_to_rgb(uint8x16_t vy, uint8x8_t vcb, uint8x8_t vcr,
                                     uint32_t monochrome,
                                     uint8x16_t *vr, uint8x16_t *vg, uint8x16_t *vb) {
    if (monochrome) {
        *vr = vy;
        *vg = vy;
        *vb = vy;
        return;
    }
    int16x8_t c16 = vreinterpretq_s16_u16(vsubl_u8(vcb, vdup_n_u8(128U)));
    int16x8_t r16 = vreinterpretq_s16_u16(vsubl_u8(vcr, vdup_n_u8(128U)));

    int32x4_t c128 = vdupq_n_s32(128);
    int32x4_t dr_lo = vmlal_n_s16(c128, vget_low_s16(r16), HDF_RGB_R_CR);
    int32x4_t dr_hi = vmlal_n_s16(c128, vget_high_s16(r16), HDF_RGB_R_CR);
    int16x8_t dr = vcombine_s16(vshrn_n_s32(dr_lo, 8), vshrn_n_s32(dr_hi, 8));

    int32x4_t dg_lo = vmlal_n_s16(c128, vget_low_s16(c16), HDF_RGB_G_CB);
    dg_lo = vmlal_n_s16(dg_lo, vget_low_s16(r16), HDF_RGB_G_CR);
    int32x4_t dg_hi = vmlal_n_s16(c128, vget_high_s16(c16), HDF_RGB_G_CB);
    dg_hi = vmlal_n_s16(dg_hi, vget_high_s16(r16), HDF_RGB_G_CR);
    int16x8_t dg = vnegq_s16(vcombine_s16(vshrn_n_s32(dg_lo, 8), vshrn_n_s32(dg_hi, 8)));

    int32x4_t db_lo = vmlal_n_s16(c128, vget_low_s16(c16), HDF_RGB_B_CB);
    int32x4_t db_hi = vmlal_n_s16(c128, vget_high_s16(c16), HDF_RGB_B_CB);
    int16x8_t db = vcombine_s16(vshrn_n_s32(db_lo, 8), vshrn_n_s32(db_hi, 8));

    int16x8_t y_lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(vy)));
    int16x8_t y_hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(vy)));

    *vr = vcombine_u8(vqmovun_s16(vaddq_s16(y_lo, vzip1q_s16(dr, dr))),
                      vqmovun_s16(vaddq_s16(y_hi, vzip2q_s16(dr, dr))));
    *vg = vcombine_u8(vqmovun_s16(vaddq_s16(y_lo, vzip1q_s16(dg, dg))),
                      vqmovun_s16(vaddq_s16(y_hi, vzip2q_s16(dg, dg))));
    *vb = vcombine_u8(vqmovun_s16(vaddq_s16(y_lo, vzip1q_s16(db, db))),
                      vqmovun_s16(vaddq_s16(y_hi, vzip2q_s16(db, db))));
}

/* Lever 5.1 / V71.3: Hoisted-LUT 256-entry byte lookup (vqtbx4 + aligned 32B/iter, 0 Q-spills) */
static __attribute__((noinline)) void hdf_lut256_pass(const uint8_t *src, uint8_t *dst,
                                                      uint32_t n16, const uint8_t tbl[256]) {
    uint8x16x4_t t0 = vld1q_u8_x4(tbl);
    uint8x16x4_t t1 = vld1q_u8_x4(tbl + 64);
    uint8x16x4_t t2 = vld1q_u8_x4(tbl + 128);
    uint8x16x4_t t3 = vld1q_u8_x4(tbl + 192);
    uint8x16_t c64 = vdupq_n_u8(64U);
    uint32_t i = 0;
    if ((((uintptr_t)src | (uintptr_t)dst) & 15U) == 0U) {
        for (; i + 32U <= n16; i += 32U) {
            uint8x16_t idx0 = hdf_ld16a_u8(src + i);
            uint8x16_t idx1 = hdf_ld16a_u8(src + i + 16U);
            uint8x16_t r0 = vqtbl4q_u8(t0, idx0);
            uint8x16_t r1 = vqtbl4q_u8(t0, idx1);
            idx0 = vsubq_u8(idx0, c64); idx1 = vsubq_u8(idx1, c64);
            r0 = vqtbx4q_u8(r0, t1, idx0); r1 = vqtbx4q_u8(r1, t1, idx1);
            idx0 = vsubq_u8(idx0, c64); idx1 = vsubq_u8(idx1, c64);
            r0 = vqtbx4q_u8(r0, t2, idx0); r1 = vqtbx4q_u8(r1, t2, idx1);
            idx0 = vsubq_u8(idx0, c64); idx1 = vsubq_u8(idx1, c64);
            r0 = vqtbx4q_u8(r0, t3, idx0); r1 = vqtbx4q_u8(r1, t3, idx1);
            hdf_st16a_u8(dst + i, r0);
            hdf_st16a_u8(dst + i + 16U, r1);
        }
    }
    for (; i < n16; i += 16U) {
        uint8x16_t idx = hdf_ld1_u8(src + i);
        uint8x16_t r = vqtbl4q_u8(t0, idx);
        idx = vsubq_u8(idx, c64); r = vqtbx4q_u8(r, t1, idx);
        idx = vsubq_u8(idx, c64); r = vqtbx4q_u8(r, t2, idx);
        idx = vsubq_u8(idx, c64); r = vqtbx4q_u8(r, t3, idx);
        hdf_st1_u8(dst + i, r);
    }
}

static __attribute__((noinline)) void hdf_lut256_hi_zip_backwards(const uint8_t *src,
                                                                  uint16_t *lin_out,
                                                                  uint32_t n16) {
    uint8x16x4_t t0 = vld1q_u8_x4(hdf_srgb_lin_hi);
    uint8x16x4_t t1 = vld1q_u8_x4(hdf_srgb_lin_hi + 64);
    uint8x16x4_t t2 = vld1q_u8_x4(hdf_srgb_lin_hi + 128);
    uint8x16x4_t t3 = vld1q_u8_x4(hdf_srgb_lin_hi + 192);
    uint8x16_t c64 = vdupq_n_u8(64U);
    const uint8_t *lo_buf = (const uint8_t *)lin_out;
    uint32_t i = n16;
    if ((((uintptr_t)src | (uintptr_t)lin_out) & 15U) == 0U) {
        while (i > 0U) {
            i -= 16U;
            uint8x16_t idx = hdf_ld16a_u8(src + i);
            uint8x16_t lo = hdf_ld16a_u8(lo_buf + i);
            uint8x16_t hi = vqtbl4q_u8(t0, idx);
            idx = vsubq_u8(idx, c64); hi = vqtbx4q_u8(hi, t1, idx);
            idx = vsubq_u8(idx, c64); hi = vqtbx4q_u8(hi, t2, idx);
            idx = vsubq_u8(idx, c64); hi = vqtbx4q_u8(hi, t3, idx);
            hdf_st8a_u16(lin_out + i + 8U, vreinterpretq_u16_u8(vzip2q_u8(lo, hi)));
            hdf_st8a_u16(lin_out + i,      vreinterpretq_u16_u8(vzip1q_u8(lo, hi)));
        }
        return;
    }
    while (i > 0U) {
        i -= 16U;
        uint8x16_t idx = hdf_ld1_u8(src + i);
        uint8x16_t lo = hdf_ld1_u8(lo_buf + i);
        uint8x16_t hi = vqtbl4q_u8(t0, idx);
        idx = vsubq_u8(idx, c64); hi = vqtbx4q_u8(hi, t1, idx);
        idx = vsubq_u8(idx, c64); hi = vqtbx4q_u8(hi, t2, idx);
        idx = vsubq_u8(idx, c64); hi = vqtbx4q_u8(hi, t3, idx);
        hdf_st1_u16(lin_out + i + 8U, vreinterpretq_u16_u8(vzip2q_u8(lo, hi)));
        hdf_st1_u16(lin_out + i,      vreinterpretq_u16_u8(vzip1q_u8(lo, hi)));
    }
}
#endif

/* ---- 1. YCbCr -> sRGB Row ----------------------------------------------- */

void hdf_ycc_to_rgb_row(const uint8_t *y_row, const uint8_t *c_row,
                        uint8_t *r_out, uint8_t *g_out, uint8_t *b_out,
                        uint32_t width, uint32_t monochrome) {
    uint32_t x = 0;
#if HDF_NEON
    for (; x + 16U <= width; x += 16U) {
        uint8x16_t vy = hdf_ld1_u8(y_row + x);
        uint8x16_t vr, vg, vb;
        if (monochrome) {
            vr = vy; vg = vy; vb = vy;
        } else {
            uint8x8x2_t vc = vld2_u8(c_row + x);
            neon_ycc16_to_rgb(vy, vc.val[0], vc.val[1], 0U, &vr, &vg, &vb);
        }
        hdf_st1_u8(r_out + x, vr);
        hdf_st1_u8(g_out + x, vg);
        hdf_st1_u8(b_out + x, vb);
    }
#endif
    if (monochrome) {
        for (; x < width; ++x) {
            uint8_t v = y_row[x];
            r_out[x] = v;
            g_out[x] = v;
            b_out[x] = v;
        }
    } else {
        for (; x + 2U <= width; x += 2U) {
            int c = (int)c_row[x] - 128;
            int r = (int)c_row[x + 1U] - 128;
            int dr = (HDF_RGB_R_CR * r + 128) >> 8;
            int dg = -((HDF_RGB_G_CB * c + HDF_RGB_G_CR * r + 128) >> 8);
            int db = (HDF_RGB_B_CB * c + 128) >> 8;
            int y0 = (int)y_row[x];
            int y1 = (int)y_row[x + 1U];
            r_out[x]      = clamp_u8(y0 + dr);
            g_out[x]      = clamp_u8(y0 + dg);
            b_out[x]      = clamp_u8(y0 + db);
            r_out[x + 1U] = clamp_u8(y1 + dr);
            g_out[x + 1U] = clamp_u8(y1 + dg);
            b_out[x + 1U] = clamp_u8(y1 + db);
        }
        if (x < width) {
            int c = (int)c_row[x] - 128;
            int r = (int)c_row[x + 1U] - 128;
            int y0 = (int)y_row[x];
            r_out[x] = clamp_u8(y0 + ((HDF_RGB_R_CR * r + 128) >> 8));
            g_out[x] = clamp_u8(y0 - ((HDF_RGB_G_CB * c + HDF_RGB_G_CR * r + 128) >> 8));
            b_out[x] = clamp_u8(y0 + ((HDF_RGB_B_CB * c + 128) >> 8));
        }
    }
}

/* ---- 1b. V71.5 Strided Stage 1 YCbCr -> sRGB (step=2 or 4, 0 Q-spills) -- */

#if HDF_NEON
static inline __attribute__((always_inline)) void s1_sqrdmulh_ycc16_to_rgb(
    uint8x16_t vy, uint8x16_t vcb, uint8x16_t vcr,
    uint8x8_t c128_8, uint8x16_t c128_16,
    int16x8_t c103, int16x8_t c198, int16x8_t c88, int16x8_t c183,
    uint8_t *r_dst, uint8_t *g_dst, uint8_t *b_dst) {
    int16x8_t cb0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(vcb), c128_8));
    int16x8_t cb1 = vreinterpretq_s16_u16(vsubl_high_u8(vcb, c128_16));
    int16x8_t cr0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(vcr), c128_8));
    int16x8_t cr1 = vreinterpretq_s16_u16(vsubl_high_u8(vcr, c128_16));
    int16x8_t dr0 = vaddq_s16(cr0, vqrdmulhq_s16(cr0, c103));
    int16x8_t dr1 = vaddq_s16(cr1, vqrdmulhq_s16(cr1, c103));
    int16x8_t db0 = vaddq_s16(cb0, vqrdmulhq_s16(cb0, c198));
    int16x8_t db1 = vaddq_s16(cb1, vqrdmulhq_s16(cb1, c198));
    int16x8_t dg0 = vnegq_s16(vaddq_s16(vqrdmulhq_s16(cb0, c88), vqrdmulhq_s16(cr0, c183)));
    int16x8_t dg1 = vnegq_s16(vaddq_s16(vqrdmulhq_s16(cb1, c88), vqrdmulhq_s16(cr1, c183)));
    uint8x8_t y_lo = vget_low_u8(vy), y_hi = vget_high_u8(vy);
    hdf_st1_u8(r_dst, vcombine_u8(
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dr0), y_lo))),
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dr1), y_hi)))));
    hdf_st1_u8(g_dst, vcombine_u8(
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dg0), y_lo))),
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dg1), y_hi)))));
    hdf_st1_u8(b_dst, vcombine_u8(
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(db0), y_lo))),
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(db1), y_hi)))));
}
#endif

void hdf_s1_subsample_ycc_to_rgb_row(const uint8_t *y_row, const uint8_t *c_row,
                                     uint8_t *r_out, uint8_t *g_out, uint8_t *b_out,
                                     uint32_t width, uint32_t step, uint32_t monochrome) {
    uint32_t sh = (step == 4U) ? 2U : 1U;
    uint32_t sub_w = width >> sh;
    uint32_t k = 0U;

#if HDF_NEON
    if (monochrome) {
        if (step == 4U) {
            for (; k + 16U <= sub_w; k += 16U) {
                uint8x16x4_t vy = vld4q_u8(y_row + (k << 2));
                hdf_st1_u8(r_out + k, vy.val[0]);
                hdf_st1_u8(g_out + k, vy.val[0]);
                hdf_st1_u8(b_out + k, vy.val[0]);
            }
        } else {
            for (; k + 16U <= sub_w; k += 16U) {
                uint8x16x2_t vy = vld2q_u8(y_row + (k << 1));
                hdf_st1_u8(r_out + k, vy.val[0]);
                hdf_st1_u8(g_out + k, vy.val[0]);
                hdf_st1_u8(b_out + k, vy.val[0]);
            }
        }
    } else {
        uint8x8_t  c128_8  = vdup_n_u8(128U);
        uint8x16_t c128_16 = vdupq_n_u8(128U);
        int16x8_t  c103 = vdupq_n_s16(103 << 7);
        int16x8_t  c198 = vdupq_n_s16(198 << 7);
        int16x8_t  c88  = vdupq_n_s16(88 << 7);
        int16x8_t  c183 = vdupq_n_s16(183 << 7);
        if (step == 4U) {
            for (; k + 16U <= sub_w; k += 16U) {
                uint32_t x = k << 2;
                uint8x16x4_t vy = vld4q_u8(y_row + x);
                uint8x16x4_t vc = vld4q_u8(c_row + x);
                s1_sqrdmulh_ycc16_to_rgb(vy.val[0], vc.val[0], vc.val[1],
                                         c128_8, c128_16, c103, c198, c88, c183,
                                         r_out + k, g_out + k, b_out + k);
            }
        } else {
            for (; k + 16U <= sub_w; k += 16U) {
                uint32_t x = k << 1;
                uint8x16x2_t vy = vld2q_u8(y_row + x);
                uint8x16x2_t vc = vld2q_u8(c_row + x);
                s1_sqrdmulh_ycc16_to_rgb(vy.val[0], vc.val[0], vc.val[1],
                                         c128_8, c128_16, c103, c198, c88, c183,
                                         r_out + k, g_out + k, b_out + k);
            }
        }
    }
#endif
    if (monochrome) {
        for (; k < sub_w; ++k) {
            uint8_t v = y_row[k << sh];
            r_out[k] = v;
            g_out[k] = v;
            b_out[k] = v;
        }
    } else {
        for (; k < sub_w; ++k) {
            uint32_t x = k << sh;
            int16_t cb  = (int16_t)((int32_t)c_row[x] - 128);
            int16_t crv = (int16_t)((int32_t)c_row[x + 1U] - 128);
            int32_t dr  = (int32_t)crv + sqrdmulh_s16_scalar(crv, (int16_t)(103 << 7));
            int32_t db  = (int32_t)cb  + sqrdmulh_s16_scalar(cb,  (int16_t)(198 << 7));
            int32_t dg  = -(sqrdmulh_s16_scalar(cb, (int16_t)(88 << 7)) +
                            sqrdmulh_s16_scalar(crv, (int16_t)(183 << 7)));
            int yv = (int)y_row[x];
            r_out[k] = clamp_u8(yv + (int)dr);
            g_out[k] = clamp_u8(yv + (int)dg);
            b_out[k] = clamp_u8(yv + (int)db);
        }
    }
}

/* ---- 2. sRGB -> Linear Q16 Row (Lever 5.1 Bit-Exact Hoisted LUT) -------- */

void hdf_srgb_to_lin_row(const uint8_t *srgb, uint16_t *lin_out, uint32_t count) {
    uint32_t i = 0;
#if HDF_NEON
    uint32_t n16 = count & ~15U;
    if (n16) {
        hdf_lut256_pass(srgb, (uint8_t *)lin_out, n16, hdf_srgb_lin_lo);
        hdf_lut256_hi_zip_backwards(srgb, lin_out, n16);
    }
    i = n16;
#endif
    for (; i < count; ++i) {
        lin_out[i] = hdf_srgb_to_lin_u16[srgb[i]];
    }
}

/* ---- 3. Highlight Energy Extraction Row (Lever 5.2 Bit-Exact Skip) ------ */

static inline void extract_scalar_one(uint32_t lr, uint32_t lg, uint32_t lb,
                                      uint32_t knee_q16, uint32_t bquad_q14,
                                      uint32_t bcore_q14, uint32_t ev_gain_q8,
                                      uint32_t src_q8, uint32_t neu_q8,
                                      uint16_t *er, uint16_t *eg, uint16_t *eb) {
    uint32_t lmax = lr > lg ? lr : lg;
    if (lb > lmax) lmax = lb;
    uint32_t llum = (77U * lr + 150U * lg + 29U * lb + 128U) >> 8;
    uint32_t lpeak7 = (lmax * 7U) >> 3;
    uint32_t ldrv = llum >= lpeak7 ? llum : lpeak7;

    uint32_t over = ldrv > knee_q16 ? ldrv - knee_q16 : 0U;
    uint32_t t = (over * 1490U) >> 16;
    uint32_t quad = (t * t) >> 10;

    uint32_t cover = ldrv > HDF_CORE_KNEE_Q16 ? ldrv - HDF_CORE_KNEE_Q16 : 0U;
    uint32_t tc = (cover * 5U) >> 6;
    uint32_t core = (tc * tc) >> 10;

    uint32_t raw = quad * bquad_q14 + core * bcore_q14;
    uint32_t boost = ((raw >> 10) * ev_gain_q8 + 128U) >> 8;
    if (boost > 49151U) boost = 49151U;

    uint32_t mix_r = (src_q8 * lr + neu_q8 * ldrv + 128U) >> 8;
    uint32_t mix_g = (src_q8 * lg + neu_q8 * ldrv + 128U) >> 8;
    uint32_t mix_b = (src_q8 * lb + neu_q8 * ldrv + 128U) >> 8;

    uint32_t br = (boost * mix_r + 32768U) >> 16;
    uint32_t bg = (boost * mix_g + 32768U) >> 16;
    uint32_t bb = (boost * mix_b + 32768U) >> 16;

    uint32_t vr = ((lr + 2U) >> 2) + br;
    uint32_t vg = ((lg + 2U) >> 2) + bg;
    uint32_t vb = ((lb + 2U) >> 2) + bb;

    *er = (uint16_t)(vr > 65535U ? 65535U : vr);
    *eg = (uint16_t)(vg > 65535U ? 65535U : vg);
    *eb = (uint16_t)(vb > 65535U ? 65535U : vb);
}

#if HDF_NEON
static inline void extract_vec4(uint32x4_t lr, uint32x4_t lg, uint32x4_t lb,
                                uint32_t knee_q16, uint32_t bquad_q14,
                                uint32_t bcore_q14, uint32_t ev_gain_q8,
                                uint32_t src_q8, uint32_t neu_q8,
                                uint32x4_t *er, uint32x4_t *eg, uint32x4_t *eb) {
    uint32x4_t c128 = vdupq_n_u32(128U);
    uint32x4_t lmax = vmaxq_u32(lr, vmaxq_u32(lg, lb));
    uint32x4_t llum = vshrq_n_u32(
        vaddq_u32(vaddq_u32(vmulq_n_u32(lr, 77U), vmulq_n_u32(lg, 150U)),
                  vaddq_u32(vmulq_n_u32(lb, 29U), c128)), 8);
    uint32x4_t lpeak7 = vshrq_n_u32(vmulq_n_u32(lmax, 7U), 3);
    uint32x4_t ldrv = vmaxq_u32(llum, lpeak7);

    uint32x4_t over = vqsubq_u32(ldrv, vdupq_n_u32(knee_q16));
    uint32x4_t t = vshrq_n_u32(vmulq_n_u32(over, 1490U), 16);
    uint32x4_t quad = vshrq_n_u32(vmulq_u32(t, t), 10);

    uint32x4_t cover = vqsubq_u32(ldrv, vdupq_n_u32(HDF_CORE_KNEE_Q16));
    uint32x4_t tc = vshrq_n_u32(vmulq_n_u32(cover, 5U), 6);
    uint32x4_t core = vshrq_n_u32(vmulq_u32(tc, tc), 10);

    uint32x4_t raw = vaddq_u32(vmulq_n_u32(quad, bquad_q14), vmulq_n_u32(core, bcore_q14));
    uint32x4_t boost = vminq_u32(
        vshrq_n_u32(vaddq_u32(vmulq_n_u32(vshrq_n_u32(raw, 10), ev_gain_q8), c128), 8),
        vdupq_n_u32(49151U));

    uint32x4_t nterm = vaddq_u32(vmulq_n_u32(ldrv, neu_q8), c128);
    uint32x4_t mix_r = vshrq_n_u32(vaddq_u32(vmulq_n_u32(lr, src_q8), nterm), 8);
    uint32x4_t mix_g = vshrq_n_u32(vaddq_u32(vmulq_n_u32(lg, src_q8), nterm), 8);
    uint32x4_t mix_b = vshrq_n_u32(vaddq_u32(vmulq_n_u32(lb, src_q8), nterm), 8);

    uint32x4_t c32768 = vdupq_n_u32(32768U);
    uint32x4_t br = vshrq_n_u32(vaddq_u32(vmulq_u32(boost, mix_r), c32768), 16);
    uint32x4_t bg = vshrq_n_u32(vaddq_u32(vmulq_u32(boost, mix_g), c32768), 16);
    uint32x4_t bb = vshrq_n_u32(vaddq_u32(vmulq_u32(boost, mix_b), c32768), 16);

    uint32x4_t c2 = vdupq_n_u32(2U);
    uint32x4_t c65535 = vdupq_n_u32(65535U);
    *er = vminq_u32(vaddq_u32(vshrq_n_u32(vaddq_u32(lr, c2), 2), br), c65535);
    *eg = vminq_u32(vaddq_u32(vshrq_n_u32(vaddq_u32(lg, c2), 2), bg), c65535);
    *eb = vminq_u32(vaddq_u32(vshrq_n_u32(vaddq_u32(lb, c2), 2), bb), c65535);
}
#endif

void hdf_extract_energy_row(const uint16_t *lr, const uint16_t *lg, const uint16_t *lb,
                            uint16_t *er, uint16_t *eg, uint16_t *eb,
                            uint32_t count, const hdf_extract_params *params) {
    uint32_t knee = params->knee_q16;
    uint32_t bquad = params->boost_quad_q14;
    uint32_t bcore = params->boost_core_q14;
    uint32_t evg = params->ev_gain_q8;
    uint32_t neu = params->neutrality_q8 > 256U ? 256U : params->neutrality_q8;
    uint32_t src = 256U - neu;
    uint32_t i = 0;

#if HDF_NEON
    uint16_t skip_thr = (uint16_t)(knee < HDF_CORE_KNEE_Q16 ? knee : HDF_CORE_KNEE_Q16);
    for (; i + 8U <= count; i += 8U) {
        uint16x8_t vlr = hdf_ld1_u16(lr + i);
        uint16x8_t vlg = hdf_ld1_u16(lg + i);
        uint16x8_t vlb = hdf_ld1_u16(lb + i);
        uint16x8_t lmax = vmaxq_u16(vlr, vmaxq_u16(vlg, vlb));
        if (vmaxvq_u16(lmax) <= skip_thr) {
            /* Exact bit-parity: when ldrv <= lmax <= min(knee, 53248), boost == 0 identically */
            hdf_st1_u16(er + i, vrshrq_n_u16(vlr, 2));
            hdf_st1_u16(eg + i, vrshrq_n_u16(vlg, 2));
            hdf_st1_u16(eb + i, vrshrq_n_u16(vlb, 2));
        } else {
            uint32x4_t er0, eg0, eb0, er1, eg1, eb1;
            extract_vec4(vmovl_u16(vget_low_u16(vlr)),
                         vmovl_u16(vget_low_u16(vlg)),
                         vmovl_u16(vget_low_u16(vlb)),
                         knee, bquad, bcore, evg, src, neu, &er0, &eg0, &eb0);
            extract_vec4(vmovl_u16(vget_high_u16(vlr)),
                         vmovl_u16(vget_high_u16(vlg)),
                         vmovl_u16(vget_high_u16(vlb)),
                         knee, bquad, bcore, evg, src, neu, &er1, &eg1, &eb1);
            hdf_st1_u16(er + i, vcombine_u16(vmovn_u32(er0), vmovn_u32(er1)));
            hdf_st1_u16(eg + i, vcombine_u16(vmovn_u32(eg0), vmovn_u32(eg1)));
            hdf_st1_u16(eb + i, vcombine_u16(vmovn_u32(eb0), vmovn_u32(eb1)));
        }
    }
#endif
    for (; i < count; ++i) {
        extract_scalar_one(lr[i], lg[i], lb[i], knee, bquad, bcore, evg, src, neu,
                           &er[i], &eg[i], &eb[i]);
    }
}

/* ---- 4. Full-Coverage Area-Box Block Accumulation (Lever 5.3) ----------- */

void hdf_accum_blocks_row(const uint16_t *er, const uint16_t *eg, const uint16_t *eb,
                          uint32_t *acc_r, uint32_t *acc_g, uint32_t *acc_b,
                          uint32_t width, uint32_t block_size) {
#if HDF_NEON
    if (block_size == 4U) {
        uint32_t x = 0, gx = 0;
        for (; x + 16U <= width; x += 16U, gx += 4U) {
            uint32x4_t r = vpaddq_u32(vpaddlq_u16(hdf_ld1_u16(er + x)),
                                      vpaddlq_u16(hdf_ld1_u16(er + x + 8U)));
            uint32x4_t g = vpaddq_u32(vpaddlq_u16(hdf_ld1_u16(eg + x)),
                                      vpaddlq_u16(hdf_ld1_u16(eg + x + 8U)));
            uint32x4_t b = vpaddq_u32(vpaddlq_u16(hdf_ld1_u16(eb + x)),
                                      vpaddlq_u16(hdf_ld1_u16(eb + x + 8U)));
            hdf_st1_u32(acc_r + gx, vaddq_u32(hdf_ld1_u32(acc_r + gx), r));
            hdf_st1_u32(acc_g + gx, vaddq_u32(hdf_ld1_u32(acc_g + gx), g));
            hdf_st1_u32(acc_b + gx, vaddq_u32(hdf_ld1_u32(acc_b + gx), b));
        }
        for (; x < width; x += 4U, ++gx) {
            uint32_t bw = width - x;
            if (bw > 4U) bw = 4U;
            uint32_t sr = 0, sg = 0, sb = 0;
            for (uint32_t k = 0; k < bw; ++k) {
                sr += er[x + k];
                sg += eg[x + k];
                sb += eb[x + k];
            }
            acc_r[gx] += sr;
            acc_g[gx] += sg;
            acc_b[gx] += sb;
        }
        return;
    }
#endif
    uint32_t gx = 0;
    for (uint32_t x = 0; x < width; x += block_size, ++gx) {
        uint32_t bw = width - x;
        if (bw > block_size) bw = block_size;
        uint32_t sr = 0, sg = 0, sb = 0;
        uint32_t k = 0;
#if HDF_NEON
        if (bw >= 8U) {
            uint32x4_t vr = vdupq_n_u32(0U);
            uint32x4_t vg = vdupq_n_u32(0U);
            uint32x4_t vb = vdupq_n_u32(0U);
            for (; k + 8U <= bw; k += 8U) {
                vr = vpadalq_u16(vr, hdf_ld1_u16(er + x + k));
                vg = vpadalq_u16(vg, hdf_ld1_u16(eg + x + k));
                vb = vpadalq_u16(vb, hdf_ld1_u16(eb + x + k));
            }
            sr = vaddvq_u32(vr);
            sg = vaddvq_u32(vg);
            sb = vaddvq_u32(vb);
        }
#endif
        for (; k < bw; ++k) {
            sr += er[x + k];
            sg += eg[x + k];
            sb += eb[x + k];
        }
        acc_r[gx] += sr;
        acc_g[gx] += sg;
        acc_b[gx] += sb;
    }
}

void hdf_accum_blocks_row_sub(const uint16_t *er, const uint16_t *eg, const uint16_t *eb,
                              uint32_t *acc_r, uint32_t *acc_g, uint32_t *acc_b,
                              uint32_t width, uint32_t block_size, uint32_t step) {
    uint32_t sh = (step == 4U) ? 2U : 1U;
    uint32_t mask = step - 1U;
    uint32_t gx = 0U;
    for (uint32_t x0 = 0U; x0 < width; x0 += block_size, ++gx) {
        uint32_t x1 = x0 + block_size;
        if (x1 > width) x1 = width;
        uint32_t k0 = (x0 + mask) >> sh;
        uint32_t k1 = (x1 + mask) >> sh;
        if (k0 >= k1) {
            k0 = (k1 > 0U) ? (k1 - 1U) : 0U;
            k1 = k0 + 1U;
        }
        uint32_t sr = 0U, sg = 0U, sb = 0U;
        for (uint32_t k = k0; k < k1; ++k) {
            sr += er[k];
            sg += eg[k];
            sb += eb[k];
        }
        acc_r[gx] += sr;
        acc_g[gx] += sg;
        acc_b[gx] += sb;
    }
}

/* ---- 5. Exact 2x2 Pyramid Downsample ------------------------------------ */

void hdf_downsample_2x2_u16(const uint16_t *src, uint32_t src_w, uint32_t src_h, uint32_t src_stride,
                            uint16_t *dst, uint32_t dst_w, uint32_t dst_h, uint32_t dst_stride) {
    for (uint32_t y = 0; y < dst_h; ++y) {
        uint32_t y0 = 2U * y;
        uint32_t y1 = y0 + 1U < src_h ? y0 + 1U : src_h - 1U;
        const uint16_t *r0 = src + (size_t)y0 * src_stride;
        const uint16_t *r1 = src + (size_t)y1 * src_stride;
        uint16_t *out = dst + (size_t)y * dst_stride;
        uint32_t x = 0;
#if HDF_NEON
        for (; x + 8U <= dst_w && (2U * (x + 8U)) <= src_w; x += 8U) {
            uint16x8x2_t a = vld2q_u16(r0 + 2U * x);
            uint16x8x2_t b = vld2q_u16(r1 + 2U * x);
            uint32x4_t s_lo = vaddl_u16(vget_low_u16(a.val[0]), vget_low_u16(a.val[1]));
            s_lo = vaddw_u16(s_lo, vget_low_u16(b.val[0]));
            s_lo = vaddw_u16(s_lo, vget_low_u16(b.val[1]));
            uint32x4_t s_hi = vaddl_u16(vget_high_u16(a.val[0]), vget_high_u16(a.val[1]));
            s_hi = vaddw_u16(s_hi, vget_high_u16(b.val[0]));
            s_hi = vaddw_u16(s_hi, vget_high_u16(b.val[1]));
            uint16x8_t res = vcombine_u16(vrshrn_n_u32(s_lo, 2), vrshrn_n_u32(s_hi, 2));
            hdf_st1_u16(out + x, res);
        }
#endif
        for (; x < dst_w; ++x) {
            uint32_t x0 = 2U * x;
            uint32_t x1 = x0 + 1U < src_w ? x0 + 1U : src_w - 1U;
            uint32_t s = (uint32_t)r0[x0] + r0[x1] + r1[x0] + r1[x1];
            out[x] = (uint16_t)((s + 2U) >> 2);
        }
        for (uint32_t pad = dst_w; pad < dst_stride; ++pad) {
            out[pad] = out[dst_w - 1U];
        }
    }
}

/* ---- 6. Separable Box-Gaussian Blur (Exact 32x32->64 Reciprocal) -------- */

#if HDF_NEON
static inline uint16x8_t neon_div_exact_k(uint32x4_t sum_lo, uint32x4_t sum_hi,
                                          uint32_t half, uint32_t mul) {
    uint32x4_t h = vdupq_n_u32(half);
    uint32x2_t m = vdup_n_u32(mul);
    uint32x4_t a_lo = vaddq_u32(sum_lo, h);
    uint32x4_t a_hi = vaddq_u32(sum_hi, h);
    uint32x2_t q0 = vshrn_n_u64(vmull_u32(vget_low_u32(a_lo), m), 32);
    uint32x2_t q1 = vshrn_n_u64(vmull_u32(vget_high_u32(a_lo), m), 32);
    uint32x2_t q2 = vshrn_n_u64(vmull_u32(vget_low_u32(a_hi), m), 32);
    uint32x2_t q3 = vshrn_n_u64(vmull_u32(vget_high_u32(a_hi), m), 32);
    return vcombine_u16(vmovn_u32(vcombine_u32(q0, q1)),
                        vmovn_u32(vcombine_u32(q2, q3)));
}
#endif

void hdf_blur_h_row_u16(const uint16_t *src_row, uint16_t *dst_row, uint32_t width, uint32_t radius) {
    if (!width) return;
    uint32_t k_taps = 2U * radius + 1U;
    uint32_t mul = (uint32_t)((1ULL << 32) / k_taps + 1ULL);
    uint32_t x = 0;

    /* Left clamped border */
    uint32_t left_end = radius < width ? radius : width;
    for (; x < left_end; ++x) {
        uint32_t sum = 0;
        for (int32_t d = -(int32_t)radius; d <= (int32_t)radius; ++d) {
            int32_t idx = (int32_t)x + d;
            if (idx < 0) idx = 0;
            else if ((uint32_t)idx >= width) idx = (int32_t)width - 1;
            sum += src_row[idx];
        }
        dst_row[x] = (uint16_t)(((uint64_t)(sum + radius) * mul) >> 32);
    }

#if HDF_NEON
    /* Vector interior: 8 cells per iteration */
    for (; x + 8U + radius <= width; x += 8U) {
        uint32x4_t s_lo = vdupq_n_u32(0U);
        uint32x4_t s_hi = vdupq_n_u32(0U);
        const uint16_t *base = src_row + x - radius;
        for (uint32_t t = 0; t < k_taps; ++t) {
            uint16x8_t v = hdf_ld1_u16(base + t);
            s_lo = vaddw_u16(s_lo, vget_low_u16(v));
            s_hi = vaddw_u16(s_hi, vget_high_u16(v));
        }
        hdf_st1_u16(dst_row + x, neon_div_exact_k(s_lo, s_hi, radius, mul));
    }
#endif

    /* Remaining interior + right clamped border */
    for (; x < width; ++x) {
        uint32_t sum = 0;
        for (int32_t d = -(int32_t)radius; d <= (int32_t)radius; ++d) {
            int32_t idx = (int32_t)x + d;
            if (idx < 0) idx = 0;
            else if ((uint32_t)idx >= width) idx = (int32_t)width - 1;
            sum += src_row[idx];
        }
        dst_row[x] = (uint16_t)(((uint64_t)(sum + radius) * mul) >> 32);
    }
}

void hdf_blur_h_grid_u16(uint16_t *grid, uint16_t *h_tmp,
                         uint32_t width, uint32_t height, uint32_t stride, uint32_t radius) {
    if (!width || !height) return;
    for (uint32_t y = 0; y < height; ++y) {
        uint16_t *row = grid + (size_t)y * stride;
        hdf_blur_h_row_u16(row, h_tmp, width, radius);
        uint32_t x = 0;
#if HDF_NEON
        for (; x + 8U <= width; x += 8U) {
            hdf_st1_u16(row + x, hdf_ld1_u16(h_tmp + x));
        }
#endif
        for (; x < width; ++x) {
            row[x] = h_tmp[x];
        }
        uint16_t last = h_tmp[width - 1U];
        for (uint32_t p = width; p < stride; ++p) {
            row[p] = last;
        }
    }
}

void hdf_blur_v_grid_u16(uint16_t *grid, uint16_t *col_scratch,
                         uint32_t width, uint32_t height, uint32_t stride, uint32_t radius) {
    if (!width || !height) return;
    uint32_t k_taps = 2U * radius + 1U;
    uint32_t mul = (uint32_t)((1ULL << 32) / k_taps + 1ULL);
    uint32_t x = 0;

#if HDF_NEON
    /* Process 8 columns at a time with O(1) vertical sliding window */
    for (; x + 8U <= width; x += 8U) {
        uint32x4_t s_lo = vdupq_n_u32(0U);
        uint32x4_t s_hi = vdupq_n_u32(0U);
        for (int32_t d = -(int32_t)radius; d <= (int32_t)radius; ++d) {
            uint32_t ry = d < 0 ? 0U : ((uint32_t)d >= height ? height - 1U : (uint32_t)d);
            uint16x8_t v = hdf_ld1_u16(grid + (size_t)ry * stride + x);
            s_lo = vaddw_u16(s_lo, vget_low_u16(v));
            s_hi = vaddw_u16(s_hi, vget_high_u16(v));
        }
        hdf_st1_u16(col_scratch, neon_div_exact_k(s_lo, s_hi, radius, mul));

        for (uint32_t y = 1U; y < height; ++y) {
            int32_t leave_i = (int32_t)y - 1 - (int32_t)radius;
            int32_t enter_i = (int32_t)y + (int32_t)radius;
            uint32_t y_leave = leave_i < 0 ? 0U : ((uint32_t)leave_i >= height ? height - 1U : (uint32_t)leave_i);
            uint32_t y_enter = enter_i < 0 ? 0U : ((uint32_t)enter_i >= height ? height - 1U : (uint32_t)enter_i);
            uint16x8_t v_leave = hdf_ld1_u16(grid + (size_t)y_leave * stride + x);
            uint16x8_t v_enter = hdf_ld1_u16(grid + (size_t)y_enter * stride + x);
            s_lo = vsubw_u16(vaddw_u16(s_lo, vget_low_u16(v_enter)), vget_low_u16(v_leave));
            s_hi = vsubw_u16(vaddw_u16(s_hi, vget_high_u16(v_enter)), vget_high_u16(v_leave));
            hdf_st1_u16(col_scratch + (size_t)y * 8U, neon_div_exact_k(s_lo, s_hi, radius, mul));
        }

        for (uint32_t y = 0; y < height; ++y) {
            hdf_st1_u16(grid + (size_t)y * stride + x, hdf_ld1_u16(col_scratch + (size_t)y * 8U));
        }
    }
#endif
    /* Scalar columns (used for HDF_SCALAR or non-multiple-of-8 tail) */
    for (; x < width; ++x) {
        uint32_t sum = 0;
        for (int32_t d = -(int32_t)radius; d <= (int32_t)radius; ++d) {
            uint32_t ry = d < 0 ? 0U : ((uint32_t)d >= height ? height - 1U : (uint32_t)d);
            sum += grid[(size_t)ry * stride + x];
        }
        col_scratch[0] = (uint16_t)(((uint64_t)(sum + radius) * mul) >> 32);
        for (uint32_t y = 1U; y < height; ++y) {
            int32_t leave_i = (int32_t)y - 1 - (int32_t)radius;
            int32_t enter_i = (int32_t)y + (int32_t)radius;
            uint32_t y_leave = leave_i < 0 ? 0U : ((uint32_t)leave_i >= height ? height - 1U : (uint32_t)leave_i);
            uint32_t y_enter = enter_i < 0 ? 0U : ((uint32_t)enter_i >= height ? height - 1U : (uint32_t)enter_i);
            sum = sum + grid[(size_t)y_enter * stride + x] - grid[(size_t)y_leave * stride + x];
            col_scratch[y] = (uint16_t)(((uint64_t)(sum + radius) * mul) >> 32);
        }
        for (uint32_t y = 0; y < height; ++y) {
            grid[(size_t)y * stride + x] = col_scratch[y];
        }
    }
}

/* ---- 7. Separable Bilinear Upsampling & Multi-Scale Blend --------------- */

void hdf_vlerp_row_u16(const uint16_t *row0, const uint16_t *row1,
                       uint16_t *vrow_out, uint32_t gw, uint32_t fy_q8, uint32_t pad_tail) {
    uint32_t w0 = 256U - fy_q8;
    uint32_t x = 0;
#if HDF_NEON
    uint16x4_t vw0 = vdup_n_u16((uint16_t)w0);
    uint16x4_t vw1 = vdup_n_u16((uint16_t)fy_q8);
    for (; x + 8U <= gw; x += 8U) {
        uint16x8_t a = hdf_ld1_u16(row0 + x);
        uint16x8_t b = hdf_ld1_u16(row1 + x);
        uint32x4_t lo = vmlal_u16(vmull_u16(vget_low_u16(a), vw0), vget_low_u16(b), vw1);
        uint32x4_t hi = vmlal_u16(vmull_u16(vget_high_u16(a), vw0), vget_high_u16(b), vw1);
        hdf_st1_u16(vrow_out + x, vcombine_u16(vrshrn_n_u32(lo, 8), vrshrn_n_u32(hi, 8)));
    }
#endif
    for (; x < gw; ++x) {
        vrow_out[x] = (uint16_t)((w0 * row0[x] + fy_q8 * row1[x] + 128U) >> 8);
    }
    uint16_t last = gw ? vrow_out[gw - 1U] : 0U;
    for (uint32_t p = 0; p < pad_tail; ++p) {
        vrow_out[gw + p] = last;
    }
}

void hdf_hlerp_row_u16(const uint16_t *vrow, const uint16_t *gx_arr, const uint8_t *fx_arr,
                       uint16_t *out, uint32_t width) {
    uint32_t x = 0;
#if HDF_NEON
    uint16x8_t c0100 = vdupq_n_u16(0x0100U);
    uint8x16_t c2 = vdupq_n_u8(2U);
    uint16x8_t c256 = vdupq_n_u16(256U);
    for (; x + 8U <= width; x += 8U) {
        uint16_t base_g = gx_arr[x];
        uint32_t span = (uint32_t)gx_arr[x + 7U] - (uint32_t)base_g;
        if (span <= 6U) {
            uint8x16_t win = vreinterpretq_u8_u16(hdf_ld1_u16(vrow + base_g));
            uint16x8_t rel = vsubq_u16(hdf_ld1_u16(gx_arr + x), vdupq_n_u16(base_g));
            uint8x16_t idx0 = vreinterpretq_u8_u16(vmlaq_n_u16(c0100, rel, 0x0202U));
            uint8x16_t idx1 = vaddq_u8(idx0, c2);
            uint16x8_t v0 = vreinterpretq_u16_u8(vqtbl1q_u8(win, idx0));
            uint16x8_t v1 = vreinterpretq_u16_u8(vqtbl1q_u8(win, idx1));

            uint8x8_t fx8;
            __asm__("ld1 {%0.8b}, [%1]" : "=w"(fx8) : "r"(fx_arr + x) : "memory");
            uint16x8_t fx16 = vmovl_u8(fx8);
            uint16x8_t w0_16 = vsubq_u16(c256, fx16);

            uint32x4_t lo = vmlal_u16(vmull_u16(vget_low_u16(v0), vget_low_u16(w0_16)),
                                      vget_low_u16(v1), vget_low_u16(fx16));
            uint32x4_t hi = vmlal_u16(vmull_u16(vget_high_u16(v0), vget_high_u16(w0_16)),
                                      vget_high_u16(v1), vget_high_u16(fx16));
            hdf_st1_u16(out + x, vcombine_u16(vrshrn_n_u32(lo, 8), vrshrn_n_u32(hi, 8)));
        } else if (span <= 30U) {
            uint8x16x4_t win = vld1q_u8_x4((const uint8_t *)(vrow + base_g));
            uint16x8_t rel = vsubq_u16(hdf_ld1_u16(gx_arr + x), vdupq_n_u16(base_g));
            uint8x16_t idx0 = vreinterpretq_u8_u16(vmlaq_n_u16(c0100, rel, 0x0202U));
            uint8x16_t idx1 = vaddq_u8(idx0, c2);
            uint16x8_t v0 = vreinterpretq_u16_u8(vqtbl4q_u8(win, idx0));
            uint16x8_t v1 = vreinterpretq_u16_u8(vqtbl4q_u8(win, idx1));

            uint8x8_t fx8;
            __asm__("ld1 {%0.8b}, [%1]" : "=w"(fx8) : "r"(fx_arr + x) : "memory");
            uint16x8_t fx16 = vmovl_u8(fx8);
            uint16x8_t w0_16 = vsubq_u16(c256, fx16);

            uint32x4_t lo = vmlal_u16(vmull_u16(vget_low_u16(v0), vget_low_u16(w0_16)),
                                      vget_low_u16(v1), vget_low_u16(fx16));
            uint32x4_t hi = vmlal_u16(vmull_u16(vget_high_u16(v0), vget_high_u16(w0_16)),
                                      vget_high_u16(v1), vget_high_u16(fx16));
            hdf_st1_u16(out + x, vcombine_u16(vrshrn_n_u32(lo, 8), vrshrn_n_u32(hi, 8)));
        } else {
            for (uint32_t j = 0; j < 8U; ++j) {
                uint32_t g = gx_arr[x + j];
                uint32_t fx = fx_arr[x + j];
                out[x + j] = (uint16_t)(((256U - fx) * vrow[g] + fx * vrow[g + 1U] + 128U) >> 8);
            }
        }
    }
#endif
    for (; x < width; ++x) {
        uint32_t g = gx_arr[x];
        uint32_t fx = fx_arr[x];
        out[x] = (uint16_t)(((256U - fx) * vrow[g] + fx * vrow[g + 1U] + 128U) >> 8);
    }
}

/* Lever 5.4 / V71.3: span==0 zero-TBL broadcast fast path (~72% of 45MP b=25 chunks) + 1-Reg TBL fallback */
void hdf_hlerp_rgb_row_u16(const uint16_t *vr, const uint16_t *vg, const uint16_t *vb,
                           const uint16_t *gx_arr, const uint8_t *fx_arr,
                           uint16_t *sr, uint16_t *sg, uint16_t *sb, uint32_t width) {
    uint32_t x = 0;
#if HDF_NEON
    uint16x8_t c0100 = vdupq_n_u16(0x0100U);
    uint8x16_t c2 = vdupq_n_u8(2U);
    uint16x8_t c256 = vdupq_n_u16(256U);

    for (; x + 8U <= width; x += 8U) {
        uint16_t base_g = gx_arr[x];
        uint32_t span = (uint32_t)gx_arr[x + 7U] - (uint32_t)base_g;
        if (__builtin_expect(span == 0U, 1)) {
            uint8x8_t fx8;
            __asm__("ld1 {%0.8b}, [%1]" : "=w"(fx8) : "r"(fx_arr + x) : "memory");
            uint16x8_t fx16 = vmovl_u8(fx8);
            uint16x8_t w0 = vsubq_u16(c256, fx16);
            uint16x4_t w0_lo = vget_low_u16(w0), w0_hi = vget_high_u16(w0);
            uint16x4_t fx_lo = vget_low_u16(fx16), fx_hi = vget_high_u16(fx16);

            uint16_t a = vr[base_g], b = vr[base_g + 1U];
            uint32x4_t lo = vmlal_n_u16(vmull_n_u16(w0_lo, a), fx_lo, b);
            uint32x4_t hi = vmlal_n_u16(vmull_n_u16(w0_hi, a), fx_hi, b);
            hdf_st1_u16(sr + x, vcombine_u16(vrshrn_n_u32(lo, 8), vrshrn_n_u32(hi, 8)));

            a = vg[base_g]; b = vg[base_g + 1U];
            lo = vmlal_n_u16(vmull_n_u16(w0_lo, a), fx_lo, b);
            hi = vmlal_n_u16(vmull_n_u16(w0_hi, a), fx_hi, b);
            hdf_st1_u16(sg + x, vcombine_u16(vrshrn_n_u32(lo, 8), vrshrn_n_u32(hi, 8)));

            a = vb[base_g]; b = vb[base_g + 1U];
            lo = vmlal_n_u16(vmull_n_u16(w0_lo, a), fx_lo, b);
            hi = vmlal_n_u16(vmull_n_u16(w0_hi, a), fx_hi, b);
            hdf_st1_u16(sb + x, vcombine_u16(vrshrn_n_u32(lo, 8), vrshrn_n_u32(hi, 8)));
        } else if (span <= 6U) {
            uint16x8_t rel = vsubq_u16(hdf_ld1_u16(gx_arr + x), vdupq_n_u16(base_g));
            uint8x16_t idx0 = vreinterpretq_u8_u16(vmlaq_n_u16(c0100, rel, 0x0202U));
            uint8x16_t idx1 = vaddq_u8(idx0, c2);
            uint8x8_t fx8;
            __asm__("ld1 {%0.8b}, [%1]" : "=w"(fx8) : "r"(fx_arr + x) : "memory");
            uint16x8_t fx16 = vmovl_u8(fx8);
            uint16x8_t w0 = vsubq_u16(c256, fx16);

            uint8x16_t wr = vreinterpretq_u8_u16(hdf_ld1_u16(vr + base_g));
            uint8x16_t wg = vreinterpretq_u8_u16(hdf_ld1_u16(vg + base_g));
            uint8x16_t wb = vreinterpretq_u8_u16(hdf_ld1_u16(vb + base_g));

            uint16x8_t v0, v1;
            uint32x4_t lo, hi;

            v0 = vreinterpretq_u16_u8(vqtbl1q_u8(wr, idx0));
            v1 = vreinterpretq_u16_u8(vqtbl1q_u8(wr, idx1));
            lo = vmlal_u16(vmull_u16(vget_low_u16(v0), vget_low_u16(w0)),
                           vget_low_u16(v1), vget_low_u16(fx16));
            hi = vmlal_u16(vmull_u16(vget_high_u16(v0), vget_high_u16(w0)),
                           vget_high_u16(v1), vget_high_u16(fx16));
            hdf_st1_u16(sr + x, vcombine_u16(vrshrn_n_u32(lo, 8), vrshrn_n_u32(hi, 8)));

            v0 = vreinterpretq_u16_u8(vqtbl1q_u8(wg, idx0));
            v1 = vreinterpretq_u16_u8(vqtbl1q_u8(wg, idx1));
            lo = vmlal_u16(vmull_u16(vget_low_u16(v0), vget_low_u16(w0)),
                           vget_low_u16(v1), vget_low_u16(fx16));
            hi = vmlal_u16(vmull_u16(vget_high_u16(v0), vget_high_u16(w0)),
                           vget_high_u16(v1), vget_high_u16(fx16));
            hdf_st1_u16(sg + x, vcombine_u16(vrshrn_n_u32(lo, 8), vrshrn_n_u32(hi, 8)));

            v0 = vreinterpretq_u16_u8(vqtbl1q_u8(wb, idx0));
            v1 = vreinterpretq_u16_u8(vqtbl1q_u8(wb, idx1));
            lo = vmlal_u16(vmull_u16(vget_low_u16(v0), vget_low_u16(w0)),
                           vget_low_u16(v1), vget_low_u16(fx16));
            hi = vmlal_u16(vmull_u16(vget_high_u16(v0), vget_high_u16(w0)),
                           vget_high_u16(v1), vget_high_u16(fx16));
            hdf_st1_u16(sb + x, vcombine_u16(vrshrn_n_u32(lo, 8), vrshrn_n_u32(hi, 8)));
        } else {
            for (uint32_t j = 0; j < 8U; ++j) {
                uint32_t g = gx_arr[x + j], f = fx_arr[x + j], iw = 256U - f;
                sr[x + j] = (uint16_t)((iw * vr[g] + f * vr[g + 1U] + 128U) >> 8);
                sg[x + j] = (uint16_t)((iw * vg[g] + f * vg[g + 1U] + 128U) >> 8);
                sb[x + j] = (uint16_t)((iw * vb[g] + f * vb[g + 1U] + 128U) >> 8);
            }
        }
    }
#endif
    for (; x < width; ++x) {
        uint32_t g = gx_arr[x], f = fx_arr[x], iw = 256U - f;
        sr[x] = (uint16_t)((iw * vr[g] + f * vr[g + 1U] + 128U) >> 8);
        sg[x] = (uint16_t)((iw * vg[g] + f * vg[g + 1U] + 128U) >> 8);
        sb[x] = (uint16_t)((iw * vb[g] + f * vb[g + 1U] + 128U) >> 8);
    }
}

void hdf_blend_3scales_row_u16(uint16_t *s1_inout, const uint16_t *s2_row, const uint16_t *s3_row,
                               uint32_t gw, uint32_t w1, uint32_t w2, uint32_t w3,
                               uint32_t white_scale_q8) {
    uint32_t x = 0;
#if HDF_NEON
    uint16x4_t vw1 = vdup_n_u16((uint16_t)w1);
    uint16x4_t vw2 = vdup_n_u16((uint16_t)w2);
    uint16x4_t vw3 = vdup_n_u16((uint16_t)w3);
    for (; x + 8U <= gw; x += 8U) {
        uint16x8_t a = hdf_ld1_u16(s1_inout + x);
        uint16x8_t b = hdf_ld1_u16(s2_row + x);
        uint16x8_t c = hdf_ld1_u16(s3_row + x);
        uint32x4_t lo = vmlal_u16(vmlal_u16(vmull_u16(vget_low_u16(a), vw1),
                                            vget_low_u16(b), vw2),
                                  vget_low_u16(c), vw3);
        uint32x4_t hi = vmlal_u16(vmlal_u16(vmull_u16(vget_high_u16(a), vw1),
                                            vget_high_u16(b), vw2),
                                  vget_high_u16(c), vw3);
        uint16x8_t mix = vcombine_u16(vrshrn_n_u32(lo, 8), vrshrn_n_u32(hi, 8));
        if (white_scale_q8 != 256U) {
            uint16x4_t vws = vdup_n_u16((uint16_t)white_scale_q8);
            uint32x4_t slo = vmull_u16(vget_low_u16(mix), vws);
            uint32x4_t shi = vmull_u16(vget_high_u16(mix), vws);
            mix = vcombine_u16(vqmovn_u32(vrshrq_n_u32(slo, 8)),
                               vqmovn_u32(vrshrq_n_u32(shi, 8)));
        }
        hdf_st1_u16(s1_inout + x, mix);
    }
#endif
    for (; x < gw; ++x) {
        uint32_t mix = (w1 * s1_inout[x] + w2 * s2_row[x] + w3 * s3_row[x] + 128U) >> 8;
        if (white_scale_q8 != 256U) {
            mix = (mix * white_scale_q8 + 128U) >> 8;
        }
        s1_inout[x] = (uint16_t)(mix > 65535U ? 65535U : mix);
    }
}

void hdf_blend_3scales_k_row_u16(uint16_t *s1_inout, const uint16_t *s2_row, const uint16_t *s3_row,
                                 uint32_t gw, uint32_t w1, uint32_t w2, uint32_t w3,
                                 uint32_t white_scale_q8, uint32_t k_q12) {
    uint32_t x = 0U;
#if HDF_NEON
    uint16x4_t vw1 = vdup_n_u16((uint16_t)w1);
    uint16x4_t vw2 = vdup_n_u16((uint16_t)w2);
    uint16x4_t vw3 = vdup_n_u16((uint16_t)w3);
    uint16x4_t vk  = vdup_n_u16((uint16_t)k_q12);
    for (; x + 8U <= gw; x += 8U) {
        uint16x8_t a = hdf_ld1_u16(s1_inout + x);
        uint16x8_t b = hdf_ld1_u16(s2_row + x);
        uint16x8_t c = hdf_ld1_u16(s3_row + x);
        uint32x4_t lo = vmlal_u16(vmlal_u16(vmull_u16(vget_low_u16(a), vw1),
                                            vget_low_u16(b), vw2),
                                  vget_low_u16(c), vw3);
        uint32x4_t hi = vmlal_u16(vmlal_u16(vmull_u16(vget_high_u16(a), vw1),
                                            vget_high_u16(b), vw2),
                                  vget_high_u16(c), vw3);
        uint16x8_t mix = vcombine_u16(vrshrn_n_u32(lo, 8), vrshrn_n_u32(hi, 8));
        if (white_scale_q8 != 256U) {
            uint16x4_t vws = vdup_n_u16((uint16_t)white_scale_q8);
            uint32x4_t slo = vmull_u16(vget_low_u16(mix), vws);
            uint32x4_t shi = vmull_u16(vget_high_u16(mix), vws);
            mix = vcombine_u16(vqmovn_u32(vrshrq_n_u32(slo, 8)),
                               vqmovn_u32(vrshrq_n_u32(shi, 8)));
        }
        uint32x4_t klo = vmull_u16(vget_low_u16(mix), vk);
        uint32x4_t khi = vmull_u16(vget_high_u16(mix), vk);
        hdf_st1_u16(s1_inout + x, vcombine_u16(vrshrn_n_u32(klo, 11), vrshrn_n_u32(khi, 11)));
    }
#endif
    for (; x < gw; ++x) {
        uint32_t mix = (w1 * s1_inout[x] + w2 * s2_row[x] + w3 * s3_row[x] + 128U) >> 8;
        if (white_scale_q8 != 256U) {
            mix = (mix * white_scale_q8 + 128U) >> 8;
        }
        if (mix > 65535U) mix = 65535U;
        s1_inout[x] = (uint16_t)((mix * k_q12 + 1024U) >> 11);
    }
}

/* ---- 8. Linear Optical Scatter + Soft Shoulder Composite (Lever 5.5) ---- */

static inline uint16_t composite_channel_scalar(uint32_t l_in, uint32_t s_q14, uint32_t k_q12) {
    uint32_t s_q16 = s_q14 << 2;
    uint32_t u = ((4096U - k_q12) * l_in + k_q12 * s_q16 + 2048U) >> 12;
    uint32_t s0 = u < HDF_SHOULDER_K1_Q16 ? u : HDF_SHOULDER_K1_Q16;
    uint32_t m1 = u > HDF_SHOULDER_K1_Q16
                      ? ((u < HDF_SHOULDER_K2_Q16 ? u : HDF_SHOULDER_K2_Q16) - HDF_SHOULDER_K1_Q16)
                      : 0U;
    uint32_t m2 = u > HDF_SHOULDER_K2_Q16
                      ? ((u < HDF_SHOULDER_TOP_Q16 ? u : HDF_SHOULDER_TOP_Q16) - HDF_SHOULDER_K2_Q16)
                      : 0U;
    uint32_t p = s0 + (m1 >> 1) + (m2 >> 3);
    if (p > 65535U) p = 65535U;
    uint32_t l_out = (u > l_in) ? (l_in > p ? l_in : p) : u;
    return (uint16_t)(l_out > 65535U ? 65535U : l_out);
}

#if HDF_NEON
#define HDF_COMP_TILE_PX 256U

static __attribute__((noinline)) void hdf_comp_tile_ycc2rgb(
    const uint8_t *y_row, const uint8_t *c_row,
    uint8_t *rin, uint8_t *gin, uint8_t *bin, uint32_t n16, uint32_t mono) {
    if (mono) {
        for (uint32_t i = 0; i < n16; i += 16U) {
            uint8x16_t vy = hdf_ld1_u8(y_row + i);
            hdf_st16a_u8(rin + i, vy);
            hdf_st16a_u8(gin + i, vy);
            hdf_st16a_u8(bin + i, vy);
        }
        return;
    }
    int32x4_t c128 = vdupq_n_s32(128);
    uint8x8_t u128 = vdup_n_u8(128U);
    for (uint32_t i = 0; i < n16; i += 16U) {
        uint8x16_t vy = hdf_ld1_u8(y_row + i);
        uint8x8x2_t vc = vld2_u8(c_row + i);
        int16x8_t c16 = vreinterpretq_s16_u16(vsubl_u8(vc.val[0], u128));
        int16x8_t r16 = vreinterpretq_s16_u16(vsubl_u8(vc.val[1], u128));
        int16x8_t dr = vcombine_s16(
            vshrn_n_s32(vmlal_n_s16(c128, vget_low_s16(r16), HDF_RGB_R_CR), 8),
            vshrn_n_s32(vmlal_n_s16(c128, vget_high_s16(r16), HDF_RGB_R_CR), 8));
        int32x4_t dg0 = vmlal_n_s16(vmlal_n_s16(c128, vget_low_s16(c16), HDF_RGB_G_CB),
                                    vget_low_s16(r16), HDF_RGB_G_CR);
        int32x4_t dg1 = vmlal_n_s16(vmlal_n_s16(c128, vget_high_s16(c16), HDF_RGB_G_CB),
                                    vget_high_s16(r16), HDF_RGB_G_CR);
        int16x8_t dg = vnegq_s16(vcombine_s16(vshrn_n_s32(dg0, 8), vshrn_n_s32(dg1, 8)));
        int16x8_t db = vcombine_s16(
            vshrn_n_s32(vmlal_n_s16(c128, vget_low_s16(c16), HDF_RGB_B_CB), 8),
            vshrn_n_s32(vmlal_n_s16(c128, vget_high_s16(c16), HDF_RGB_B_CB), 8));
        int16x8_t y0 = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(vy)));
        int16x8_t y1 = vreinterpretq_s16_u16(vmovl_high_u8(vy));
        hdf_st16a_u8(rin + i, vcombine_u8(vqmovun_s16(vaddq_s16(y0, vzip1q_s16(dr, dr))),
                                          vqmovun_s16(vaddq_s16(y1, vzip2q_s16(dr, dr)))));
        hdf_st16a_u8(gin + i, vcombine_u8(vqmovun_s16(vaddq_s16(y0, vzip1q_s16(dg, dg))),
                                          vqmovun_s16(vaddq_s16(y1, vzip2q_s16(dg, dg)))));
        hdf_st16a_u8(bin + i, vcombine_u8(vqmovun_s16(vaddq_s16(y0, vzip1q_s16(db, db))),
                                          vqmovun_s16(vaddq_s16(y1, vzip2q_s16(db, db)))));
    }
}

static inline __attribute__((always_inline)) uint16x8_t hdf_shoulder_slow8(
                                            uint16x8_t l16, uint32x4_t u0, uint32x4_t u1,
                                            uint32x4_t vk1, uint32x4_t vk2,
                                            uint32x4_t vtop, uint32x4_t c65535) {
    uint32x4_t l0 = vmovl_u16(vget_low_u16(l16));
    uint32x4_t l1 = vmovl_high_u16(l16);
    uint32x4_t p0 = vminq_u32(vaddq_u32(vminq_u32(u0, vk1),
        vaddq_u32(vshrq_n_u32(vqsubq_u32(vminq_u32(u0, vk2), vk1), 1),
                  vshrq_n_u32(vqsubq_u32(vminq_u32(u0, vtop), vk2), 3))), c65535);
    uint32x4_t p1 = vminq_u32(vaddq_u32(vminq_u32(u1, vk1),
        vaddq_u32(vshrq_n_u32(vqsubq_u32(vminq_u32(u1, vk2), vk1), 1),
                  vshrq_n_u32(vqsubq_u32(vminq_u32(u1, vtop), vk2), 3))), c65535);
    uint32x4_t o0 = vminq_u32(vbslq_u32(vcgtq_u32(u0, l0), vmaxq_u32(l0, p0), u0), c65535);
    uint32x4_t o1 = vminq_u32(vbslq_u32(vcgtq_u32(u1, l1), vmaxq_u32(l1, p1), u1), c65535);
    return vcombine_u16(vmovn_u32(o0), vmovn_u32(o1));
}

static __attribute__((noinline)) void hdf_comp_tile_shoulder(
    uint16_t *lin_io, const uint16_t *s_q14, uint32_t n16, uint32_t k_q12) {
    uint16x4_t v_invk = vdup_n_u16((uint16_t)(4096U - k_q12));
    uint16x4_t v_4k = vdup_n_u16((uint16_t)(k_q12 << 2));
    uint32x4_t vk1 = vdupq_n_u32(HDF_SHOULDER_K1_Q16);
    uint32x4_t vk2 = vdupq_n_u32(HDF_SHOULDER_K2_Q16);
    uint32x4_t vtop = vdupq_n_u32(HDF_SHOULDER_TOP_Q16);
    uint32x4_t c65535 = vdupq_n_u32(65535U);
    for (uint32_t i = 0; i < n16; i += 8U) {
        uint16x8_t l0 = hdf_ld8a_u16(lin_io + i);
        uint16x8_t s0 = hdf_ld1_u16(s_q14 + i);
        uint32x4_t u00 = vrshrq_n_u32(vmlal_u16(vmull_u16(vget_low_u16(l0), v_invk), vget_low_u16(s0), v_4k), 12);
        uint32x4_t u01 = vrshrq_n_u32(vmlal_u16(vmull_u16(vget_high_u16(l0), v_invk), vget_high_u16(s0), v_4k), 12);
        if (__builtin_expect(vmaxvq_u32(vmaxq_u32(u00, u01)) <= HDF_SHOULDER_K1_Q16, 1)) {
            hdf_st8a_u16(lin_io + i, vcombine_u16(vmovn_u32(u00), vmovn_u32(u01)));
        } else {
            hdf_st8a_u16(lin_io + i, hdf_shoulder_slow8(l0, u00, u01, vk1, vk2, vtop, c65535));
        }
    }
}

static __attribute__((noinline)) void hdf_comp_tile_lin2srgb_low(
    const uint16_t *lin, uint8_t *out, uint32_t total_n16) {
    uint8x16x4_t tt0 = vld1q_u8_x4(hdf_lin_srgb_t0);
    uint8x16x4_t tt1 = vld1q_u8_x4(hdf_lin_srgb_t1);
    uint8x16x4_t tt2 = vld1q_u8_x4(hdf_lin_srgb_t2);
    uint16x8_t c1024 = vdupq_n_u16(1024U);
    uint16x8_t c3072 = vdupq_n_u16(3072U);
    for (uint32_t i = 0; i < total_n16; i += 16U) {
        uint16x8_t l0 = hdf_ld8a_u16(lin + i);
        uint16x8_t l1 = hdf_ld8a_u16(lin + i + 8U);
        if (__builtin_expect(vminvq_u16(vminq_u16(l0, l1)) >= 7168U, 1)) continue;
        uint8x16_t i0 = vcombine_u8(vqmovn_u16(vshrq_n_u16(l0, 4)),
                                    vqmovn_u16(vshrq_n_u16(l1, 4)));
        uint8x16_t i1 = vcombine_u8(vqmovn_u16(vshrq_n_u16(vqsubq_u16(l0, c1024), 5)),
                                    vqmovn_u16(vshrq_n_u16(vqsubq_u16(l1, c1024), 5)));
        uint8x16_t i2 = vcombine_u8(vqmovn_u16(vshrq_n_u16(vqsubq_u16(l0, c3072), 6)),
                                    vqmovn_u16(vshrq_n_u16(vqsubq_u16(l1, c3072), 6)));
        uint8x16_t v0 = vqtbl4q_u8(tt0, i0);
        uint8x16_t v1 = vqtbl4q_u8(tt1, i1);
        uint8x16_t v2 = vqtbl4q_u8(tt2, i2);
        uint8x16_t m3072 = vcombine_u8(vmovn_u16(vcltq_u16(l0, c3072)),
                                       vmovn_u16(vcltq_u16(l1, c3072)));
        uint8x16_t m1024 = vcombine_u8(vmovn_u16(vcltq_u16(l0, c1024)),
                                       vmovn_u16(vcltq_u16(l1, c1024)));
        hdf_st16a_u8(out + i, vbslq_u8(m1024, v0, vbslq_u8(m3072, v1, v2)));
    }
}

static __attribute__((noinline)) void hdf_comp_tile_lin2srgb_high(
    const uint16_t *lin, uint8_t *out_io, uint32_t total_n16) {
    uint8x16x4_t tlo = vld1q_u8_x4(hdf_lin_srgb_cb_lo);
    uint8x16x4_t thi = vld1q_u8_x4(hdf_lin_srgb_cb_hi);
    uint8x16x4_t tcs = vld1q_u8_x4(hdf_lin_srgb_cs);
    uint16x8_t c1023 = vdupq_n_u16(1023U);
    uint16x8_t c7168 = vdupq_n_u16(7168U);
    for (uint32_t i = 0; i < total_n16; i += 16U) {
        uint16x8_t l0 = hdf_ld8a_u16(lin + i);
        uint16x8_t l1 = hdf_ld8a_u16(lin + i + 8U);
        if (vmaxvq_u16(vmaxq_u16(l0, l1)) < 7168U) continue;
        uint8x16_t ik = vcombine_u8(vmovn_u16(vshrq_n_u16(l0, 10)),
                                    vmovn_u16(vshrq_n_u16(l1, 10)));
        uint8x16_t blo = vqtbl4q_u8(tlo, ik);
        uint8x16_t bhi = vqtbl4q_u8(thi, ik);
        uint8x16_t csv = vqtbl4q_u8(tcs, ik);
        uint16x8_t b0 = vreinterpretq_u16_u8(vzip1q_u8(blo, bhi));
        uint16x8_t b1 = vreinterpretq_u16_u8(vzip2q_u8(blo, bhi));
        uint16x8_t cs0 = vmovl_u8(vget_low_u8(csv));
        uint16x8_t cs1 = vmovl_high_u8(csv);
        uint16x8_t r0 = vandq_u16(l0, c1023);
        uint16x8_t r1 = vandq_u16(l1, c1023);
        uint16x8_t d0 = vcombine_u16(
            vrshrn_n_u32(vmull_u16(vget_low_u16(cs0), vget_low_u16(r0)), 7),
            vrshrn_n_u32(vmull_high_u16(cs0, r0), 7));
        uint16x8_t d1 = vcombine_u16(
            vrshrn_n_u32(vmull_u16(vget_low_u16(cs1), vget_low_u16(r1)), 7),
            vrshrn_n_u32(vmull_high_u16(cs1, r1), 7));
        uint8x16_t v3 = vcombine_u8(vqrshrn_n_u16(vqaddq_u16(b0, d0), 8),
                                    vqrshrn_n_u16(vqaddq_u16(b1, d1), 8));
        if (__builtin_expect(vminvq_u16(vminq_u16(l0, l1)) >= 7168U, 1)) {
            hdf_st16a_u8(out_io + i, v3);
        } else {
            uint8x16_t mlt = vcombine_u8(vmovn_u16(vcltq_u16(l0, c7168)),
                                         vmovn_u16(vcltq_u16(l1, c7168)));
            hdf_st16a_u8(out_io + i, vbslq_u8(mlt, hdf_ld16a_u8(out_io + i), v3));
        }
    }
}

static __attribute__((noinline)) void hdf_comp_tile_delta_ycc(
    uint8_t *y_row, uint8_t *c_row,
    const uint8_t *rin, const uint8_t *gin, const uint8_t *bin,
    const uint8_t *rout, const uint8_t *gout, const uint8_t *bout,
    int16_t *dcb_stash, int16_t *dcr_stash, uint32_t n16, uint32_t chroma_mode) {
    int32x4_t c128 = vdupq_n_s32(128);
    int32x4_t c256 = vdupq_n_s32(256);
    for (uint32_t i = 0; i < n16; i += 16U) {
        uint8x16_t vy = hdf_ld1_u8(y_row + i);
        uint8x16_t ri = hdf_ld16a_u8(rin + i), ro = hdf_ld16a_u8(rout + i);
        uint8x16_t gi = hdf_ld16a_u8(gin + i), go = hdf_ld16a_u8(gout + i);
        uint8x16_t bi = hdf_ld16a_u8(bin + i), bo = hdf_ld16a_u8(bout + i);
        int16x8_t dr0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(ro), vget_low_u8(ri)));
        int16x8_t dr1 = vreinterpretq_s16_u16(vsubl_high_u8(ro, ri));
        int16x8_t dg0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(go), vget_low_u8(gi)));
        int16x8_t dg1 = vreinterpretq_s16_u16(vsubl_high_u8(go, gi));
        int16x8_t db0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(bo), vget_low_u8(bi)));
        int16x8_t db1 = vreinterpretq_s16_u16(vsubl_high_u8(bo, bi));

        int32x4_t dy0 = vmlal_n_s16(vmlal_n_s16(vmlal_n_s16(c128, vget_low_s16(dr0), HDF_YCC_Y_R),
                                                vget_low_s16(dg0), HDF_YCC_Y_G),
                                    vget_low_s16(db0), HDF_YCC_Y_B);
        int32x4_t dy1 = vmlal_n_s16(vmlal_n_s16(vmlal_n_s16(c128, vget_high_s16(dr0), HDF_YCC_Y_R),
                                                vget_high_s16(dg0), HDF_YCC_Y_G),
                                    vget_high_s16(db0), HDF_YCC_Y_B);
        int32x4_t dy2 = vmlal_n_s16(vmlal_n_s16(vmlal_n_s16(c128, vget_low_s16(dr1), HDF_YCC_Y_R),
                                                vget_low_s16(dg1), HDF_YCC_Y_G),
                                    vget_low_s16(db1), HDF_YCC_Y_B);
        int32x4_t dy3 = vmlal_n_s16(vmlal_n_s16(vmlal_n_s16(c128, vget_high_s16(dr1), HDF_YCC_Y_R),
                                                vget_high_s16(dg1), HDF_YCC_Y_G),
                                    vget_high_s16(db1), HDF_YCC_Y_B);
        int16x8_t y0 = vaddq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(vy))),
                                 vcombine_s16(vshrn_n_s32(dy0, 8), vshrn_n_s32(dy1, 8)));
        int16x8_t y1 = vaddq_s16(vreinterpretq_s16_u16(vmovl_high_u8(vy)),
                                 vcombine_s16(vshrn_n_s32(dy2, 8), vshrn_n_s32(dy3, 8)));
        hdf_st1_u8(y_row + i, vcombine_u8(vqmovun_s16(y0), vqmovun_s16(y1)));

        int16x8_t pr = vpaddq_s16(dr0, dr1);
        int16x8_t pg = vpaddq_s16(dg0, dg1);
        int16x8_t pb = vpaddq_s16(db0, db1);
        int32x4_t cb0 = vmlsl_n_s16(vmlsl_n_s16(vmlal_n_s16(c256, vget_low_s16(pb), HDF_YCC_CB_B),
                                                vget_low_s16(pr), HDF_YCC_CB_R),
                                    vget_low_s16(pg), HDF_YCC_CB_G);
        int32x4_t cb1 = vmlsl_n_s16(vmlsl_n_s16(vmlal_n_s16(c256, vget_high_s16(pb), HDF_YCC_CB_B),
                                                vget_high_s16(pr), HDF_YCC_CB_R),
                                    vget_high_s16(pg), HDF_YCC_CB_G);
        int16x8_t dcb = vcombine_s16(vshrn_n_s32(cb0, 9), vshrn_n_s32(cb1, 9));

        int32x4_t cr0 = vmlsl_n_s16(vmlsl_n_s16(vmlal_n_s16(c256, vget_low_s16(pr), HDF_YCC_CR_R),
                                                vget_low_s16(pg), HDF_YCC_CR_G),
                                    vget_low_s16(pb), HDF_YCC_CR_B);
        int32x4_t cr1 = vmlsl_n_s16(vmlsl_n_s16(vmlal_n_s16(c256, vget_high_s16(pr), HDF_YCC_CR_R),
                                                vget_high_s16(pg), HDF_YCC_CR_G),
                                    vget_high_s16(pb), HDF_YCC_CR_B);
        int16x8_t dcr = vcombine_s16(vshrn_n_s32(cr0, 9), vshrn_n_s32(cr1, 9));

        uint32_t pi = i >> 1;
        if (chroma_mode == HDF_CHROMA_420_EVEN) {
            hdf_st1_s16(dcb_stash + pi, dcb);
            hdf_st1_s16(dcr_stash + pi, dcr);
        } else {
            uint8x8x2_t vc = vld2_u8(c_row + i);
            if (chroma_mode == HDF_CHROMA_420_ODD) {
                dcb = vrhaddq_s16(hdf_ld1_s16(dcb_stash + pi), dcb);
                dcr = vrhaddq_s16(hdf_ld1_s16(dcr_stash + pi), dcr);
            }
            uint8x8x2_t co;
            co.val[0] = vqmovun_s16(vaddq_s16(vreinterpretq_s16_u16(vmovl_u8(vc.val[0])), dcb));
            co.val[1] = vqmovun_s16(vaddq_s16(vreinterpretq_s16_u16(vmovl_u8(vc.val[1])), dcr));
            vst2_u8(c_row + i, co);
        }
    }
}
#endif

void hdf_composite_row(uint8_t *y_row, uint8_t *c_row,
                       const uint16_t *sr_q14, const uint16_t *sg_q14, const uint16_t *sb_q14,
                       int16_t *dcb_stash, int16_t *dcr_stash,
                       uint32_t width, uint32_t k_q12,
                       uint32_t monochrome, uint32_t chroma_mode) {
    uint32_t x = 0;

#if HDF_NEON
    /* Contiguous 3-channel L1-resident stack tile (~1.5 KiB total) */
    uint8_t  _Alignas(16) rgb_in[3U * HDF_COMP_TILE_PX];
    uint8_t  _Alignas(16) rgb_out[3U * HDF_COMP_TILE_PX];
    uint16_t _Alignas(16) lin_buf[3U * HDF_COMP_TILE_PX];

    for (; x + 16U <= width; ) {
        uint32_t rem = (width - x) & ~15U;
        uint32_t tn = rem > HDF_COMP_TILE_PX ? HDF_COMP_TILE_PX : rem;
        uint8_t *ri = rgb_in, *gi = rgb_in + tn, *bi = rgb_in + 2U * tn;
        uint8_t *ro = rgb_out, *go = rgb_out + tn, *bo = rgb_out + 2U * tn;
        uint16_t *lr = lin_buf, *lg = lin_buf + tn, *lb = lin_buf + 2U * tn;

        hdf_comp_tile_ycc2rgb(y_row + x, c_row + x, ri, gi, bi, tn, monochrome);
        hdf_lut256_pass(rgb_in, (uint8_t *)lin_buf, 3U * tn, hdf_srgb_lin_lo);
        hdf_lut256_hi_zip_backwards(rgb_in, lin_buf, 3U * tn);

        hdf_comp_tile_shoulder(lr, sr_q14 + x, tn, k_q12);
        hdf_comp_tile_shoulder(lg, sg_q14 + x, tn, k_q12);
        hdf_comp_tile_shoulder(lb, sb_q14 + x, tn, k_q12);

        hdf_comp_tile_lin2srgb_low(lin_buf, rgb_out, 3U * tn);
        hdf_comp_tile_lin2srgb_high(lin_buf, rgb_out, 3U * tn);

        hdf_comp_tile_delta_ycc(y_row + x, c_row + x, ri, gi, bi, ro, go, bo,
                                dcb_stash + (x >> 1), dcr_stash + (x >> 1), tn, chroma_mode);
        x += tn;
    }
#endif

    for (; x + 2U <= width; x += 2U) {
        int c = monochrome ? 0 : (int)c_row[x] - 128;
        int r = monochrome ? 0 : (int)c_row[x + 1U] - 128;
        int off_r = monochrome ? 0 : ((HDF_RGB_R_CR * r + 128) >> 8);
        int off_g = monochrome ? 0 : -((HDF_RGB_G_CB * c + HDF_RGB_G_CR * r + 128) >> 8);
        int off_b = monochrome ? 0 : ((HDF_RGB_B_CB * c + 128) >> 8);

        int dr_pair = 0, dg_pair = 0, db_pair = 0;
        for (uint32_t j = 0; j < 2U; ++j) {
            uint32_t px = x + j;
            int yv = (int)y_row[px];
            uint8_t rin = clamp_u8(yv + off_r);
            uint8_t gin = clamp_u8(yv + off_g);
            uint8_t bin = clamp_u8(yv + off_b);

            uint16_t lr = hdf_srgb_to_lin_u16[rin];
            uint16_t lg = hdf_srgb_to_lin_u16[gin];
            uint16_t lb = hdf_srgb_to_lin_u16[bin];

            uint16_t lo_r = composite_channel_scalar(lr, sr_q14[px], k_q12);
            uint16_t lo_g = composite_channel_scalar(lg, sg_q14[px], k_q12);
            uint16_t lo_b = composite_channel_scalar(lb, sb_q14[px], k_q12);

            uint8_t rout = hdf_lin_to_srgb_scalar(lo_r);
            uint8_t gout = hdf_lin_to_srgb_scalar(lo_g);
            uint8_t bout = hdf_lin_to_srgb_scalar(lo_b);

            int dr = (int)rout - (int)rin;
            int dg = (int)gout - (int)gin;
            int db = (int)bout - (int)bin;

            int dy = (HDF_YCC_Y_R * dr + HDF_YCC_Y_G * dg + HDF_YCC_Y_B * db + 128) >> 8;
            y_row[px] = clamp_u8(yv + dy);

            dr_pair += dr;
            dg_pair += dg;
            db_pair += db;
        }

        int16_t dcb = (int16_t)((-HDF_YCC_CB_R * dr_pair - HDF_YCC_CB_G * dg_pair + HDF_YCC_CB_B * db_pair + 256) >> 9);
        int16_t dcr = (int16_t)(( HDF_YCC_CR_R * dr_pair - HDF_YCC_CR_G * dg_pair - HDF_YCC_CR_B * db_pair + 256) >> 9);

        uint32_t pair_idx = x >> 1;
        if (chroma_mode == HDF_CHROMA_420_EVEN) {
            dcb_stash[pair_idx] = dcb;
            dcr_stash[pair_idx] = dcr;
        } else {
            int cb_delta = dcb;
            int cr_delta = dcr;
            if (chroma_mode == HDF_CHROMA_420_ODD) {
                cb_delta = ((int)dcb_stash[pair_idx] + cb_delta + 1) >> 1;
                cr_delta = ((int)dcr_stash[pair_idx] + cr_delta + 1) >> 1;
            }
            c_row[x]      = clamp_u8((int)c_row[x]      + cb_delta);
            c_row[x + 1U] = clamp_u8((int)c_row[x + 1U] + cr_delta);
        }
    }
}

/* ---- 9. V71r2 Live Fast LIVE_256_YC Q15/Q13 Kernels (1024x{680,682} NV12) */

#define HDF_LIVE_W  1024U
#define HDF_LIVE_CW  512U
#define HDF_LIVE_GW  256U

#if HDF_NEON

static inline uint8x16_t live_ld1_u8_inc(const uint8_t **p) {
    uint8x16_t v;
    __asm__("ld1 {%0.16b}, [%1], #16" : "=w"(v), "+r"(*p) : : "memory");
    return v;
}
static inline void live_st1_u8_inc(uint8_t **p, uint8x16_t v) {
    __asm__ volatile("st1 {%1.16b}, [%0], #16" : "+r"(*p) : "w"(v) : "memory");
}
static inline uint8x8_t live_ld1_u8_8(const uint8_t *p) {
    uint8x8_t v;
    __asm__("ld1 {%0.8b}, [%1]" : "=w"(v) : "r"(p) : "memory");
    return v;
}
static inline void live_st1_u8_8(uint8_t *p, uint8x8_t v) {
    __asm__ volatile("st1 {%1.8b}, [%0]" : : "r"(p), "w"(v) : "memory");
}
static inline void live_st1_u16_4(uint16_t *p, uint16x4_t v) {
    __asm__ volatile("st1 {%1.4h}, [%0]" : : "r"(p), "w"(v) : "memory");
}
static inline uint16x8_t live_ld1_u16_inc(const uint16_t **p) {
    uint16x8_t v;
    __asm__("ld1 {%0.8h}, [%1], #16" : "=w"(v), "+r"(*p) : : "memory");
    return v;
}
static inline void live_st1_u16_inc(uint16_t **p, uint16x8_t v) {
    __asm__ volatile("st1 {%1.8h}, [%0], #16" : "+r"(*p) : "w"(v) : "memory");
}
static inline int16x8_t live_ld1_s16_inc(const int16_t **p) {
    int16x8_t v;
    __asm__("ld1 {%0.8h}, [%1], #16" : "=w"(v), "+r"(*p) : : "memory");
    return v;
}
static inline uint16x8x2_t live_ld1_u16_x2_inc(const uint16_t **p) {
    uint16x8x2_t v = vld1q_u16_x2(*p);
    *p += 16U;
    return v;
}
static inline void live_st1_u16_x2_inc(uint16_t **p, uint16x8_t a, uint16x8_t b) {
    uint16x8x2_t v = {{ a, b }};
    vst1q_u16_x2(*p, v);
    *p += 16U;
}
static inline void live_st1_s16_x2_inc(int16_t **p, int16x8_t a, int16x8_t b) {
    int16x8x2_t v = {{ a, b }};
    vst1q_s16_x2(*p, v);
    *p += 16U;
}

typedef struct { uint8x16_t blo, bhi, slp; } LiveKnot16Regs;
typedef struct { uint8x16_t base, scale; uint8x16x4_t dm; } LiveClzUmullRegs;

static inline LiveKnot16Regs live_load_knot16_lin(void) {
    LiveKnot16Regs r;
    r.blo = hdf_ld1_u8(k_live_s2l16_lin_blo);
    r.bhi = hdf_ld1_u8(k_live_s2l16_lin_bhi);
    r.slp = hdf_ld1_u8(k_live_s2l16_lin_slp);
    return r;
}

static inline LiveKnot16Regs live_load_knot16_q13b(void) {
    LiveKnot16Regs r;
    r.blo = hdf_ld1_u8(k_live_s2l16_q13b_blo);
    r.bhi = hdf_ld1_u8(k_live_s2l16_q13b_bhi);
    r.slp = hdf_ld1_u8(k_live_s2l16_q13b_slp);
    return r;
}

static inline LiveKnot16Regs live_load_knot16_invk(uint32_t level) {
    uint32_t idx = (level >= 1U && level <= 3U) ? (level - 1U) : 0U;
    LiveKnot16Regs r;
    r.blo = hdf_ld1_u8(k_live_s2l16_invk_blo[idx]);
    r.bhi = hdf_ld1_u8(k_live_s2l16_invk_bhi[idx]);
    r.slp = hdf_ld1_u8(k_live_s2l16_invk_slp[idx]);
    return r;
}

static inline LiveClzUmullRegs live_load_clz_umull(void) {
    LiveClzUmullRegs r;
    r.base  = hdf_ld1_u8(k_live_clz_base);
    r.scale = hdf_ld1_u8(k_live_clz_scale);
    r.dm    = vld1q_u8_x4(k_live_clz_dm);
    return r;
}

/* 10-insn 1-reg-TBL 16-knot sRGB -> scaled Linear Q15 (guaranteed <= 32767) */
static inline void live_knot16_srgb16_to_u16(uint8x16_t x, const LiveKnot16Regs *k,
                                             uint8x16_t c30,
                                             uint16x8_t *l0, uint16x8_t *l1) {
    uint8x16_t hi  = vshrq_n_u8(x, 4);
    uint8x16_t lo2 = vandq_u8(vaddq_u8(x, x), c30);
    uint8x16_t blo = vqtbl1q_u8(k->blo, hi);
    uint8x16_t bhi = vqtbl1q_u8(k->bhi, hi);
    uint8x16_t slp = vqtbl1q_u8(k->slp, hi);
    uint16x8_t b0  = vreinterpretq_u16_u8(vzip1q_u8(blo, bhi));
    uint16x8_t b1  = vreinterpretq_u16_u8(vzip2q_u8(blo, bhi));
    *l0 = vmlal_u8(b0, vget_low_u8(slp), vget_low_u8(lo2));
    *l1 = vmlal_high_u8(b1, slp, lo2);
}

/* 9-insn Stage 1 fast path for x<=158: hi=x>>4, lo=x&15 -> exact (lin15(x)+2)>>2 */
static inline uint16x8_t live_knot16_q13b_pair(uint8x16_t x, const LiveKnot16Regs *kq,
                                               uint8x16_t c15) {
    uint8x16_t hi  = vshrq_n_u8(x, 4);
    uint8x16_t lo  = vandq_u8(x, c15);
    uint8x16_t blo = vqtbl1q_u8(kq->blo, hi);
    uint8x16_t bhi = vqtbl1q_u8(kq->bhi, hi);
    uint8x16_t slp = vqtbl1q_u8(kq->slp, hi);
    uint16x8_t e0  = vmlal_u8(vreinterpretq_u16_u8(vzip1q_u8(blo, bhi)),
                              vget_low_u8(slp), vget_low_u8(lo));
    uint16x8_t e1  = vmlal_high_u8(vreinterpretq_u16_u8(vzip2q_u8(blo, bhi)), slp, lo);
    return vpaddq_u16(e0, e1);
}

/* 7-insn Stage 1 256-direct path for 8 pixels (all <=158): exact (lin15(x)+2)>>2 */
static inline __attribute__((always_inline)) uint16x8_t live_knot16_q13b_8px(
    uint8x8_t x, const LiveKnot16Regs *kq, uint8x8_t c15) {
    uint8x8_t hi  = vshr_n_u8(x, 4);
    uint8x8_t lo  = vand_u8(x, c15);
    uint8x8_t blo = vqtbl1_u8(kq->blo, hi);
    uint8x8_t bhi = vqtbl1_u8(kq->bhi, hi);
    uint8x8_t slp = vqtbl1_u8(kq->slp, hi);
    uint16x8_t base = vreinterpretq_u16_u8(vzip1q_u8(vcombine_u8(blo, blo), vcombine_u8(bhi, bhi)));
    return vmlal_u8(base, slp, lo);
}

/* 8-insn 8-pixel 16-knot sRGB -> Linear Q15 (0..32767) */
static inline __attribute__((always_inline)) uint16x8_t live_knot16_srgb8_to_u16(
    uint8x8_t x, const LiveKnot16Regs *k, uint8x8_t c30) {
    uint8x8_t hi  = vshr_n_u8(x, 4);
    uint8x8_t lo2 = vand_u8(vadd_u8(x, x), c30);
    uint8x8_t blo = vqtbl1_u8(k->blo, hi);
    uint8x8_t bhi = vqtbl1_u8(k->bhi, hi);
    uint8x8_t slp = vqtbl1_u8(k->slp, hi);
    uint16x8_t base = vreinterpretq_u16_u8(vzip1q_u8(vcombine_u8(blo, blo), vcombine_u8(bhi, bhi)));
    return vmlal_u8(base, slp, lo2);
}

/* 16-insn Linear Q15 (0..32767) -> sRGB (0..255), RMS 0.48 LSB, max 1 LSB, 0->0, 32767->255 */
static inline uint8x16_t live_clz_umull64_lin15_to_srgb(uint16x8_t o0, uint16x8_t o1,
                                                        const LiveClzUmullRegs *c) {
    uint16x8_t lz0 = vclzq_u16(o0);
    uint16x8_t lz1 = vclzq_u16(o1);
    uint16x8_t n0  = vshlq_u16(o0, vreinterpretq_s16_u16(lz0));
    uint16x8_t n1  = vshlq_u16(o1, vreinterpretq_s16_u16(lz1));
    uint8x16_t lz_u8 = vuzp1q_u8(vreinterpretq_u8_u16(lz0), vreinterpretq_u8_u16(lz1));
    uint8x16_t n_hi  = vuzp2q_u8(vreinterpretq_u8_u16(n0),  vreinterpretq_u8_u16(n1));
    uint8x16_t m_idx = vshrq_n_u8(vaddq_u8(n_hi, n_hi), 2);
    uint8x16_t bv = vqtbl1q_u8(c->base,  lz_u8);
    uint8x16_t sv = vqtbl1q_u8(c->scale, lz_u8);
    uint8x16_t dv = vqtbl4q_u8(c->dm,    m_idx);
    uint8x16_t d8 = vcombine_u8(vrshrn_n_u16(vmull_u8(vget_low_u8(sv), vget_low_u8(dv)), 8),
                                vrshrn_n_u16(vmull_high_u8(sv, dv), 8));
    return vqaddq_u8(bv, d8);
}

/* Cold-path always-inline highlight shoulder (by-value return, 0 bl, 0 Q-spills) */
static inline __attribute__((always_inline)) uint16x8x2_t
live_shoulder_apply_16px(uint8x16_t vx, uint16x8_t u0, uint16x8_t u1) {
    LiveKnot16Regs klin = live_load_knot16_lin();
    uint16x8_t l0, l1;
    live_knot16_srgb16_to_u16(vx, &klin, vdupq_n_u8(30U), &l0, &l1);

    uint16x8_t ck1  = vdupq_n_u16(25600U);
    uint16x8_t ck2  = vdupq_n_u16(35840U);
    uint16x8_t ctop = vdupq_n_u16(52224U);
    uint16x8_t cmax = vdupq_n_u16(32767U);

    uint16x8_t p0 = vaddq_u16(vminq_u16(u0, ck1),
        vaddq_u16(vshrq_n_u16(vqsubq_u16(vminq_u16(u0, ck2), ck1), 1),
                  vshrq_n_u16(vqsubq_u16(vminq_u16(u0, ctop), ck2), 3)));
    uint16x8_t p1 = vaddq_u16(vminq_u16(u1, ck1),
        vaddq_u16(vshrq_n_u16(vqsubq_u16(vminq_u16(u1, ck2), ck1), 1),
                  vshrq_n_u16(vqsubq_u16(vminq_u16(u1, ctop), ck2), 3)));

    uint16x8x2_t res;
    res.val[0] = vminq_u16(vbslq_u16(vcgtq_u16(u0, l0), vmaxq_u16(l0, p0), u0), cmax);
    res.val[1] = vminq_u16(vbslq_u16(vcgtq_u16(u1, l1), vmaxq_u16(l1, p1), u1), cmax);
    return res;
}

static inline __attribute__((always_inline)) uint16x8_t live_s1_boost_mono_vec(
    uint8x16_t ys, const LiveKnot16Regs *klin, uint8x16_t c30,
    const uint8x16x4_t *bt_lo, const uint8x16x4_t *bt_hi, uint16x8_t c32767) {
    uint16x8_t lr0, lr1;
    live_knot16_srgb16_to_u16(ys, klin, c30, &lr0, &lr1);
    uint8x16_t bidx = vshrq_n_u8(
        vuzp2q_u8(vreinterpretq_u8_u16(lr0), vreinterpretq_u8_u16(lr1)), 1);
    uint8x16_t blo = vqtbl4q_u8(*bt_lo, bidx);
    uint8x16_t bhi = vqtbl4q_u8(*bt_hi, bidx);
    int16x8_t b0_h = vreinterpretq_s16_u8(vzip1q_u8(blo, bhi));
    int16x8_t b1_h = vreinterpretq_s16_u8(vzip2q_u8(blo, bhi));
    uint16x8_t er0 = vminq_u16(vaddq_u16(vrshrq_n_u16(lr0, 2),
        vreinterpretq_u16_s16(vqrdmulhq_s16(b0_h, vreinterpretq_s16_u16(lr0)))), c32767);
    uint16x8_t er1 = vminq_u16(vaddq_u16(vrshrq_n_u16(lr1, 2),
        vreinterpretq_u16_s16(vqrdmulhq_s16(b1_h, vreinterpretq_s16_u16(lr1)))), c32767);
    return vpaddq_u16(er0, er1);
}

/* Deferred monochrome boost batch: single-halfrow granularity (mode: 0=solo, 1=store, 2=combine) */
static __attribute__((noinline)) void live_s1_boost_mono_deferred(
    const uint8_t *hi_buf, const uint8_t *hi_desc, uint32_t n_hi,
    const uint8_t *boost_lo, const uint8_t *boost_hi, uint16_t *gr_out) {
    LiveKnot16Regs klin = live_load_knot16_lin();
    uint8x16_t c30 = vdupq_n_u8(30U);
    uint8x16x4_t bt_lo = vld1q_u8_x4(boost_lo);
    uint8x16x4_t bt_hi = vld1q_u8_x4(boost_hi);
    uint16x8_t c32767 = vdupq_n_u16(32767U);

    for (uint32_t i = 0; i < n_hi; ++i) {
        uint32_t d = hi_desc[i];
        uint32_t off = (d & 31U) << 3;
        uint32_t mode = d >> 6;
        uint16x8_t p = live_s1_boost_mono_vec(hdf_ld1_u8(hi_buf + (size_t)i * 16U),
                                              &klin, c30, &bt_lo, &bt_hi, c32767);
        if (mode == 0U) {
            hdf_st1_u16(gr_out + off, vrshrq_n_u16(p, 1));
        } else if (mode == 1U) {
            hdf_st1_u16(gr_out + off, p);
        } else {
            hdf_st1_u16(gr_out + off, vshrq_n_u16(vrhaddq_u16(hdf_ld1_u16(gr_out + off), p), 1));
        }
    }
}

/* 8-pixel color boost core: 0 vector spills (neither str q nor st1 [sp]!) */
static inline __attribute__((always_inline)) void live_s1_boost_8px_core(
    uint8x8_t r8, uint8x8_t g8, uint8x8_t b8,
    const LiveKnot16Regs *klin, uint8x8_t c30_8,
    const uint8x16x4_t *bt_lo, const uint8x16x4_t *bt_hi,
    uint16x8_t c32767, int16x8_t c77, int16x8_t c150, int16x8_t c29,
    int16x8_t c160, int16x8_t c96,
    uint16x8_t *er_out, uint16x8_t *eg_out, uint16x8_t *eb_out) {
    uint16x8_t lr_u = live_knot16_srgb8_to_u16(r8, klin, c30_8);
    uint16x8_t lg_u = live_knot16_srgb8_to_u16(g8, klin, c30_8);
    uint16x8_t lb_u = live_knot16_srgb8_to_u16(b8, klin, c30_8);

    int16x8_t lr = vreinterpretq_s16_u16(lr_u);
    int16x8_t lg = vreinterpretq_s16_u16(lg_u);
    int16x8_t lb = vreinterpretq_s16_u16(lb_u);

    uint16x8_t lmax = vmaxq_u16(lr_u, vmaxq_u16(lg_u, lb_u));
    uint16x8_t llum = vreinterpretq_u16_s16(vqaddq_s16(
        vqaddq_s16(vqrdmulhq_s16(lr, c77), vqrdmulhq_s16(lg, c150)),
        vqrdmulhq_s16(lb, c29)));
    uint16x8_t ldrv = vmaxq_u16(llum, vsubq_u16(lmax, vshrq_n_u16(lmax, 3)));

    uint8x8_t bidx = vshr_n_u8(vshrn_n_u16(ldrv, 8), 1);
    uint8x8_t blo  = vqtbl4_u8(*bt_lo, bidx);
    uint8x8_t bhi  = vqtbl4_u8(*bt_hi, bidx);
    int16x8_t bh   = vreinterpretq_s16_u8(vzip1q_u8(vcombine_u8(blo, blo), vcombine_u8(bhi, bhi)));
    int16x8_t nt   = vqrdmulhq_s16(vreinterpretq_s16_u16(ldrv), c96);

    *er_out = vminq_u16(vaddq_u16(vrshrq_n_u16(lr_u, 2),
        vreinterpretq_u16_s16(vqrdmulhq_s16(bh, vqaddq_s16(vqrdmulhq_s16(lr, c160), nt)))), c32767);
    *eg_out = vminq_u16(vaddq_u16(vrshrq_n_u16(lg_u, 2),
        vreinterpretq_u16_s16(vqrdmulhq_s16(bh, vqaddq_s16(vqrdmulhq_s16(lg, c160), nt)))), c32767);
    *eb_out = vminq_u16(vaddq_u16(vrshrq_n_u16(lb_u, 2),
        vreinterpretq_u16_s16(vqrdmulhq_s16(bh, vqaddq_s16(vqrdmulhq_s16(lb, c160), nt)))), c32767);
}

/* Deferred 8-pixel color boost for smooth bright chunks (4x fewer px than 512x341, 0 spills) */
static __attribute__((noinline)) void live_s1_boost_8px_deferred(
    const uint8_t *b8_buf, const uint8_t *b8_chunks, uint32_t n_b8,
    const uint8_t *boost_lo, const uint8_t *boost_hi,
    uint16_t *gr_out, uint16_t *gg_out, uint16_t *gb_out) {
    LiveKnot16Regs klin = live_load_knot16_lin();
    uint8x8_t c30_8 = vdup_n_u8(30U);
    uint8x16x4_t bt_lo = vld1q_u8_x4(boost_lo);
    uint8x16x4_t bt_hi = vld1q_u8_x4(boost_hi);
    uint16x8_t c32767 = vdupq_n_u16(32767U);
    int16x8_t c77  = vdupq_n_s16(77 << 7);
    int16x8_t c150 = vdupq_n_s16(150 << 7);
    int16x8_t c29  = vdupq_n_s16(29 << 7);
    int16x8_t c160 = vdupq_n_s16(160 << 7);
    int16x8_t c96  = vdupq_n_s16(96 << 7);

    for (uint32_t i = 0; i < n_b8; ++i) {
        const uint8_t *src = b8_buf + (size_t)i * 24U;
        uint8x8_t r8 = live_ld1_u8_8(src);
        uint8x8_t g8 = live_ld1_u8_8(src + 8U);
        uint8x8_t b8 = live_ld1_u8_8(src + 16U);
        uint16x8_t er, eg, eb;
        live_s1_boost_8px_core(r8, g8, b8, &klin, c30_8, &bt_lo, &bt_hi,
                               c32767, c77, c150, c29, c160, c96, &er, &eg, &eb);
        uint32_t off = (uint32_t)b8_chunks[i] << 3;
        hdf_st1_u16(gr_out + off, er);
        hdf_st1_u16(gg_out + off, eg);
        hdf_st1_u16(gb_out + off, eb);
    }
}

/* Deferred color boost batch for sharp 512x341 highlight chunks: 2x 8-px halves -> 0 spills */
static __attribute__((noinline)) void live_s1_boost_color_deferred(
    const uint8_t *hi_buf, const uint8_t *hi_desc, uint32_t n_hi,
    const uint8_t *boost_lo, const uint8_t *boost_hi,
    uint16_t *gr_out, uint16_t *gg_out, uint16_t *gb_out) {
    LiveKnot16Regs klin = live_load_knot16_lin();
    uint8x8_t c30_8 = vdup_n_u8(30U);
    uint16x8_t c32767 = vdupq_n_u16(32767U);
    int16x8_t c77  = vdupq_n_s16(77 << 7);
    int16x8_t c150 = vdupq_n_s16(150 << 7);
    int16x8_t c29  = vdupq_n_s16(29 << 7);
    int16x8_t c160 = vdupq_n_s16(160 << 7);
    int16x8_t c96  = vdupq_n_s16(96 << 7);

    for (uint32_t i = 0; i < n_hi; ++i) {
        const uint8_t *src = hi_buf + (size_t)i * 48U;
        uint8x16x4_t bt_lo = vld1q_u8_x4(boost_lo);
        uint8x16x4_t bt_hi = vld1q_u8_x4(boost_hi);
        uint16x8_t er0, eg0, eb0, er1, eg1, eb1;
        live_s1_boost_8px_core(live_ld1_u8_8(src +  0U), live_ld1_u8_8(src + 16U),
                               live_ld1_u8_8(src + 32U), &klin, c30_8, &bt_lo, &bt_hi,
                               c32767, c77, c150, c29, c160, c96, &er0, &eg0, &eb0);
        uint16x4_t pr0 = vpadd_u16(vget_low_u16(er0), vget_high_u16(er0));
        uint16x4_t pg0 = vpadd_u16(vget_low_u16(eg0), vget_high_u16(eg0));
        uint16x4_t pb0 = vpadd_u16(vget_low_u16(eb0), vget_high_u16(eb0));
        live_s1_boost_8px_core(live_ld1_u8_8(src +  8U), live_ld1_u8_8(src + 24U),
                               live_ld1_u8_8(src + 40U), &klin, c30_8, &bt_lo, &bt_hi,
                               c32767, c77, c150, c29, c160, c96, &er1, &eg1, &eb1);

        uint16x8_t pr = vcombine_u16(pr0, vpadd_u16(vget_low_u16(er1), vget_high_u16(er1)));
        uint16x8_t pg = vcombine_u16(pg0, vpadd_u16(vget_low_u16(eg1), vget_high_u16(eg1)));
        uint16x8_t pb = vcombine_u16(pb0, vpadd_u16(vget_low_u16(eb1), vget_high_u16(eb1)));

        uint32_t d = hi_desc[i];
        uint32_t off = (d & 31U) << 3;
        uint32_t mode = d >> 6;
        if (mode == 0U) {
            hdf_st1_u16(gr_out + off, vrshrq_n_u16(pr, 1));
            hdf_st1_u16(gg_out + off, vrshrq_n_u16(pg, 1));
            hdf_st1_u16(gb_out + off, vrshrq_n_u16(pb, 1));
        } else if (mode == 1U) {
            hdf_st1_u16(gr_out + off, pr);
            hdf_st1_u16(gg_out + off, pg);
            hdf_st1_u16(gb_out + off, pb);
        } else {
            hdf_st1_u16(gr_out + off, vshrq_n_u16(vrhaddq_u16(hdf_ld1_u16(gr_out + off), pr), 1));
            hdf_st1_u16(gg_out + off, vshrq_n_u16(vrhaddq_u16(hdf_ld1_u16(gg_out + off), pg), 1));
            hdf_st1_u16(gb_out + off, vshrq_n_u16(vrhaddq_u16(hdf_ld1_u16(gb_out + off), pb), 1));
        }
    }
}

typedef struct {
    uint8x16_t vr, vg, vb, ys, cb, cr;
} LiveYccRgb16;

static inline __attribute__((always_inline)) LiveYccRgb16 live_ycc_to_rgb16_inc(
    const uint8_t **yr0, const uint8_t **yr1, const uint8_t **cr_ptr,
    uint8x8_t c128_8, uint8x16_t c128_16,
    int16x8_t c103, int16x8_t c198, int16x8_t c88, int16x8_t c183) {
    uint8x16x2_t y0 = vld2q_u8(*yr0); *yr0 += 32U;
    uint8x16x2_t y1 = vld2q_u8(*yr1); *yr1 += 32U;
    uint8x16_t ys = vhaddq_u8(vrhaddq_u8(y0.val[0], y0.val[1]),
                              vrhaddq_u8(y1.val[0], y1.val[1]));
    uint8x16x2_t vc = vld2q_u8(*cr_ptr); *cr_ptr += 32U;

    int16x8_t cb0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(vc.val[0]), c128_8));
    int16x8_t cb1 = vreinterpretq_s16_u16(vsubl_high_u8(vc.val[0], c128_16));
    int16x8_t cr0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(vc.val[1]), c128_8));
    int16x8_t cr1 = vreinterpretq_s16_u16(vsubl_high_u8(vc.val[1], c128_16));

    int16x8_t dr0 = vaddq_s16(cr0, vqrdmulhq_s16(cr0, c103));
    int16x8_t dr1 = vaddq_s16(cr1, vqrdmulhq_s16(cr1, c103));
    int16x8_t db0 = vaddq_s16(cb0, vqrdmulhq_s16(cb0, c198));
    int16x8_t db1 = vaddq_s16(cb1, vqrdmulhq_s16(cb1, c198));
    int16x8_t dg0 = vnegq_s16(vaddq_s16(vqrdmulhq_s16(cb0, c88), vqrdmulhq_s16(cr0, c183)));
    int16x8_t dg1 = vnegq_s16(vaddq_s16(vqrdmulhq_s16(cb1, c88), vqrdmulhq_s16(cr1, c183)));

    uint8x8_t ys_lo = vget_low_u8(ys), ys_hi = vget_high_u8(ys);
    LiveYccRgb16 out;
    out.vr = vcombine_u8(
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dr0), ys_lo))),
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dr1), ys_hi))));
    out.vg = vcombine_u8(
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dg0), ys_lo))),
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dg1), ys_hi))));
    out.vb = vcombine_u8(
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(db0), ys_lo))),
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(db1), ys_hi))));
    out.ys = ys;
    out.cb = vc.val[0];
    out.cr = vc.val[1];
    return out;
}

/* Deferred full 512x341 Stage 1 color extract ONLY for highlight/saturated chunks (0 Q-spills) */
static __attribute__((noinline)) void live_s1_hi_chunks_512(
    const uint8_t *y0, const uint8_t *y1, const uint8_t *c0,
    const uint8_t *y2, const uint8_t *y3, const uint8_t *c1,
    const uint8_t *hi_chunks, uint32_t n_hi_c,
    const uint8_t *boost_lo, const uint8_t *boost_hi,
    uint16_t *gr_out, uint16_t *gg_out, uint16_t *gb_out,
    uint8_t *r256_out, uint8_t *g256_out, uint8_t *b256_out,
    uint8_t *boost_scratch) {
    LiveKnot16Regs kq = live_load_knot16_q13b();
    uint8x16_t c15 = vdupq_n_u8(15U);
    uint8x8_t  c128_8  = vdup_n_u8(128U);
    uint8x16_t c128_16 = vdupq_n_u8(128U);
    int16x8_t c103 = vdupq_n_s16(103 << 7);
    int16x8_t c198 = vdupq_n_s16(198 << 7);
    int16x8_t c88  = vdupq_n_s16(88 << 7);
    int16x8_t c183 = vdupq_n_s16(183 << 7);

    uint8_t hi_desc[64];
    uint32_t n_boost = 0U;

    for (uint32_t i = 0; i < n_hi_c; ++i) {
        uint32_t chunk = hi_chunks[i];
        uint32_t boff = chunk << 5;
        uint32_t goff = chunk << 3;
        const uint8_t *py0 = y0 + boff, *py1 = y1 + boff, *pc0 = c0 + boff;
        LiveYccRgb16 a = live_ycc_to_rgb16_inc(&py0, &py1, &pc0, c128_8, c128_16,
                                               c103, c198, c88, c183);
        if (y2) {
            const uint8_t *py2 = y2 + boff, *py3 = y3 + boff, *pc1 = c1 + boff;
            LiveYccRgb16 b = live_ycc_to_rgb16_inc(&py2, &py3, &pc1, c128_8, c128_16,
                                                   c103, c198, c88, c183);
            live_st1_u8_8(r256_out + goff, vrshrn_n_u16(vpadalq_u8(vpaddlq_u8(a.vr), b.vr), 2));
            live_st1_u8_8(g256_out + goff, vrshrn_n_u16(vpadalq_u8(vpaddlq_u8(a.vg), b.vg), 2));
            live_st1_u8_8(b256_out + goff, vrshrn_n_u16(vpadalq_u8(vpaddlq_u8(a.vb), b.vb), 2));

            uint32_t hi0 = vmaxvq_u8(vmaxq_u8(a.vr, vmaxq_u8(a.vg, a.vb))) > 158U;
            uint32_t hi1 = vmaxvq_u8(vmaxq_u8(b.vr, vmaxq_u8(b.vg, b.vb))) > 158U;
            if ((hi0 | hi1) == 0U) {
                uint16x8_t p0r = live_knot16_q13b_pair(a.vr, &kq, c15);
                uint16x8_t p1r = live_knot16_q13b_pair(b.vr, &kq, c15);
                hdf_st1_u16(gr_out + goff, vshrq_n_u16(vrhaddq_u16(p0r, p1r), 1));
                uint16x8_t p0g = live_knot16_q13b_pair(a.vg, &kq, c15);
                uint16x8_t p1g = live_knot16_q13b_pair(b.vg, &kq, c15);
                hdf_st1_u16(gg_out + goff, vshrq_n_u16(vrhaddq_u16(p0g, p1g), 1));
                uint16x8_t p0b = live_knot16_q13b_pair(a.vb, &kq, c15);
                uint16x8_t p1b = live_knot16_q13b_pair(b.vb, &kq, c15);
                hdf_st1_u16(gb_out + goff, vshrq_n_u16(vrhaddq_u16(p0b, p1b), 1));
            } else if (!hi0) {
                hdf_st1_u16(gr_out + goff, live_knot16_q13b_pair(a.vr, &kq, c15));
                hdf_st1_u16(gg_out + goff, live_knot16_q13b_pair(a.vg, &kq, c15));
                hdf_st1_u16(gb_out + goff, live_knot16_q13b_pair(a.vb, &kq, c15));
                uint8_t *d = boost_scratch + (size_t)n_boost * 48U;
                hdf_st1_u8(d +  0U, b.vr); hdf_st1_u8(d + 16U, b.vg); hdf_st1_u8(d + 32U, b.vb);
                hi_desc[n_boost++] = (uint8_t)(chunk | (2U << 6));
            } else if (!hi1) {
                hdf_st1_u16(gr_out + goff, live_knot16_q13b_pair(b.vr, &kq, c15));
                hdf_st1_u16(gg_out + goff, live_knot16_q13b_pair(b.vg, &kq, c15));
                hdf_st1_u16(gb_out + goff, live_knot16_q13b_pair(b.vb, &kq, c15));
                uint8_t *d = boost_scratch + (size_t)n_boost * 48U;
                hdf_st1_u8(d +  0U, a.vr); hdf_st1_u8(d + 16U, a.vg); hdf_st1_u8(d + 32U, a.vb);
                hi_desc[n_boost++] = (uint8_t)(chunk | (2U << 6));
            } else {
                uint8_t *d0 = boost_scratch + (size_t)n_boost * 48U;
                hdf_st1_u8(d0 +  0U, a.vr); hdf_st1_u8(d0 + 16U, a.vg); hdf_st1_u8(d0 + 32U, a.vb);
                hi_desc[n_boost++] = (uint8_t)(chunk | (1U << 6));
                uint8_t *d1 = boost_scratch + (size_t)n_boost * 48U;
                hdf_st1_u8(d1 +  0U, b.vr); hdf_st1_u8(d1 + 16U, b.vg); hdf_st1_u8(d1 + 32U, b.vb);
                hi_desc[n_boost++] = (uint8_t)(chunk | (2U << 6));
            }
        } else {
            live_st1_u8_8(r256_out + goff, vrshrn_n_u16(vpaddlq_u8(a.vr), 1));
            live_st1_u8_8(g256_out + goff, vrshrn_n_u16(vpaddlq_u8(a.vg), 1));
            live_st1_u8_8(b256_out + goff, vrshrn_n_u16(vpaddlq_u8(a.vb), 1));
            if (vmaxvq_u8(vmaxq_u8(a.vr, vmaxq_u8(a.vg, a.vb))) <= 158U) {
                hdf_st1_u16(gr_out + goff, vrshrq_n_u16(live_knot16_q13b_pair(a.vr, &kq, c15), 1));
                hdf_st1_u16(gg_out + goff, vrshrq_n_u16(live_knot16_q13b_pair(a.vg, &kq, c15), 1));
                hdf_st1_u16(gb_out + goff, vrshrq_n_u16(live_knot16_q13b_pair(a.vb, &kq, c15), 1));
            } else {
                uint8_t *d = boost_scratch + (size_t)n_boost * 48U;
                hdf_st1_u8(d +  0U, a.vr); hdf_st1_u8(d + 16U, a.vg); hdf_st1_u8(d + 32U, a.vb);
                hi_desc[n_boost++] = (uint8_t)chunk;
            }
        }
    }

    if (n_boost > 0U) {
        live_s1_boost_color_deferred(boost_scratch, hi_desc, n_boost,
                                     boost_lo, boost_hi, gr_out, gg_out, gb_out);
    }
}

static inline uint16x8_t live_sym5_h_vec(uint16x8_t prev, uint16x8_t cur, uint16x8_t next) {
    uint16x8_t l2 = vextq_u16(prev, cur, 6);
    uint16x8_t l1 = vextq_u16(prev, cur, 7);
    uint16x8_t r1 = vextq_u16(cur, next, 1);
    uint16x8_t r2 = vextq_u16(cur, next, 2);
    return vrhaddq_u16(vhaddq_u16(vrhaddq_u16(l2, r2), cur), vrhaddq_u16(l1, r1));
}

static inline uint16x8_t live_sym3_h_vec(uint16x8_t prev, uint16x8_t cur, uint16x8_t next) {
    uint16x8_t l1 = vextq_u16(prev, cur, 7);
    uint16x8_t r1 = vextq_u16(cur, next, 1);
    return vhaddq_u16(cur, vrhaddq_u16(l1, r1));
}

static inline uint16x8_t live_sym5_v_vec(uint16x8_t rm2, uint16x8_t rm1, uint16x8_t r0,
                                         uint16x8_t rp1, uint16x8_t rp2) {
    return vrhaddq_u16(vhaddq_u16(vrhaddq_u16(rm2, rp2), r0), vrhaddq_u16(rm1, rp1));
}

static inline uint16x8_t live_sym3_v_vec(uint16x8_t prev, uint16x8_t cur, uint16x8_t next) {
    return vhaddq_u16(cur, vrhaddq_u16(prev, next));
}

#else /* !HDF_NEON: 100% bit-exact portable C11 scalar equivalents */

static inline int16_t sqadd_s16_scalar(int16_t a, int16_t b) {
    int32_t s = (int32_t)a + (int32_t)b;
    if (s > 32767) return 32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
}

static inline uint8_t uhadd_u8_s(uint8_t a, uint8_t b) {
    return (uint8_t)(((uint32_t)a + (uint32_t)b) >> 1);
}

static inline uint8_t urhadd_u8_s(uint8_t a, uint8_t b) {
    return (uint8_t)(((uint32_t)a + (uint32_t)b + 1U) >> 1);
}

static inline uint16_t uhadd_u16_s(uint16_t a, uint16_t b) {
    return (uint16_t)(((uint32_t)a + (uint32_t)b) >> 1);
}

static inline uint16_t urhadd_u16_s(uint16_t a, uint16_t b) {
    return (uint16_t)(((uint32_t)a + (uint32_t)b + 1U) >> 1);
}

static inline void live_s1_halfrow_scalar(
    const uint8_t *yr0, const uint8_t *yr1, const uint8_t *cr,
    const uint8_t *boost_lo, const uint8_t *boost_hi,
    uint32_t cx_base, uint32_t monochrome,
    uint16_t *pr_out, uint16_t *pg_out, uint16_t *pb_out,
    uint8_t r_px[2], uint8_t g_px[2], uint8_t b_px[2],
    uint8_t ys_px[2], uint8_t cb_px[2], uint8_t cr_px[2]) {
    uint16_t er[2], eg[2], eb[2];
    for (uint32_t j = 0; j < 2U; ++j) {
        uint32_t cx = cx_base + j;
        uint8_t ys = uhadd_u8_s(urhadd_u8_s(yr0[2U * cx], yr0[2U * cx + 1U]),
                                urhadd_u8_s(yr1[2U * cx], yr1[2U * cx + 1U]));
        ys_px[j] = ys;
        if (monochrome) {
            uint16_t lr = live_knot16_eval_scalar(ys, k_live_s2l16_lin_blo,
                                                  k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
            uint32_t bidx = (uint32_t)lr >> 9;
            int16_t bh = (int16_t)((uint16_t)boost_lo[bidx] | ((uint16_t)boost_hi[bidx] << 8));
            uint32_t v = (((uint32_t)lr + 2U) >> 2) + (uint16_t)sqrdmulh_s16_scalar(bh, (int16_t)lr);
            er[j] = (uint16_t)(v > 32767U ? 32767U : v);
        } else {
            uint8_t cb_u = cr[2U * cx];
            uint8_t cr_u = cr[2U * cx + 1U];
            cb_px[j] = cb_u;
            cr_px[j] = cr_u;
            int16_t cb  = (int16_t)((int32_t)cb_u - 128);
            int16_t crv = (int16_t)((int32_t)cr_u - 128);
            int32_t dr = (int32_t)crv + sqrdmulh_s16_scalar(crv, (int16_t)(103 << 7));
            int32_t db = (int32_t)cb  + sqrdmulh_s16_scalar(cb,  (int16_t)(198 << 7));
            int32_t dg = -(sqrdmulh_s16_scalar(cb, (int16_t)(88 << 7)) +
                           sqrdmulh_s16_scalar(crv, (int16_t)(183 << 7)));
            uint8_t vr = clamp_u8((int)ys + (int)dr);
            uint8_t vg = clamp_u8((int)ys + (int)dg);
            uint8_t vb = clamp_u8((int)ys + (int)db);
            r_px[j] = vr; g_px[j] = vg; b_px[j] = vb;
            uint16_t lr = live_knot16_eval_scalar(vr, k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
            uint16_t lg = live_knot16_eval_scalar(vg, k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
            uint16_t lb = live_knot16_eval_scalar(vb, k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
            uint16_t lmax = lr > lg ? lr : lg;
            if (lb > lmax) lmax = lb;
            uint16_t llum = (uint16_t)sqadd_s16_scalar(
                sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lr, (int16_t)(77 << 7)),
                                 sqrdmulh_s16_scalar((int16_t)lg, (int16_t)(150 << 7))),
                sqrdmulh_s16_scalar((int16_t)lb, (int16_t)(29 << 7)));
            uint16_t lpeak7 = (uint16_t)(lmax - (lmax >> 3));
            uint16_t ldrv = llum >= lpeak7 ? llum : lpeak7;
            uint32_t bidx = (uint32_t)ldrv >> 9;
            int16_t bh = (int16_t)((uint16_t)boost_lo[bidx] | ((uint16_t)boost_hi[bidx] << 8));
            int16_t nt = sqrdmulh_s16_scalar((int16_t)ldrv, (int16_t)(96 << 7));
            int16_t mr = sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lr, (int16_t)(160 << 7)), nt);
            int16_t mg = sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lg, (int16_t)(160 << 7)), nt);
            int16_t mb = sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lb, (int16_t)(160 << 7)), nt);
            uint32_t vr_e = (((uint32_t)lr + 2U) >> 2) + (uint16_t)sqrdmulh_s16_scalar(bh, mr);
            uint32_t vg_e = (((uint32_t)lg + 2U) >> 2) + (uint16_t)sqrdmulh_s16_scalar(bh, mg);
            uint32_t vb_e = (((uint32_t)lb + 2U) >> 2) + (uint16_t)sqrdmulh_s16_scalar(bh, mb);
            er[j] = (uint16_t)(vr_e > 32767U ? 32767U : vr_e);
            eg[j] = (uint16_t)(vg_e > 32767U ? 32767U : vg_e);
            eb[j] = (uint16_t)(vb_e > 32767U ? 32767U : vb_e);
        }
    }
    *pr_out = (uint16_t)(er[0] + er[1]);
    if (!monochrome) {
        *pg_out = (uint16_t)(eg[0] + eg[1]);
        *pb_out = (uint16_t)(eb[0] + eb[1]);
    }
}

#endif /* HDF_NEON */

void hdf_live_s1_rowpair_256(const uint8_t *y0, const uint8_t *y1, const uint8_t *c0,
                             const uint8_t *y2, const uint8_t *y3, const uint8_t *c1,
                             const uint8_t *boost_lo, const uint8_t *boost_hi,
                             uint16_t *gr_out, uint16_t *gg_out, uint16_t *gb_out,
                             uint8_t *r256_out, uint8_t *g256_out, uint8_t *b256_out,
                             uint32_t *c_edge_mask_out, uint32_t *y_edge_mask_out,
                             uint8_t *boost_scratch, uint32_t monochrome) {
#if HDF_NEON
    LiveKnot16Regs kq = live_load_knot16_q13b();
    uint8x16_t c15 = vdupq_n_u8(15U);
    uint8_t hi_desc[64];
    uint32_t n_hi = 0U;

    if (monochrome) {
        *c_edge_mask_out = 0U;
        *y_edge_mask_out = 0U;
        uint16_t *pr_dst = gr_out;
        if (y2) {
            for (uint32_t chunk = 0; chunk < HDF_LIVE_CW / 16U; ++chunk) {
                uint8x16x2_t a0 = vld2q_u8(y0); y0 += 32U;
                uint8x16x2_t a1 = vld2q_u8(y1); y1 += 32U;
                uint8x16_t ys0 = vhaddq_u8(vrhaddq_u8(a0.val[0], a0.val[1]),
                                           vrhaddq_u8(a1.val[0], a1.val[1]));
                uint8x16x2_t b0 = vld2q_u8(y2); y2 += 32U;
                uint8x16x2_t b1 = vld2q_u8(y3); y3 += 32U;
                uint8x16_t ys1 = vhaddq_u8(vrhaddq_u8(b0.val[0], b0.val[1]),
                                           vrhaddq_u8(b1.val[0], b1.val[1]));
                uint32_t hi0 = vmaxvq_u8(ys0) > 158U;
                uint32_t hi1 = vmaxvq_u8(ys1) > 158U;
                if (__builtin_expect((hi0 | hi1) == 0U, 1)) {
                    uint16x8_t p0 = live_knot16_q13b_pair(ys0, &kq, c15);
                    uint16x8_t p1 = live_knot16_q13b_pair(ys1, &kq, c15);
                    hdf_st1_u16(pr_dst, vshrq_n_u16(vrhaddq_u16(p0, p1), 1));
                } else if (!hi0) {
                    hdf_st1_u16(pr_dst, live_knot16_q13b_pair(ys0, &kq, c15));
                    hdf_st1_u8(boost_scratch + (size_t)n_hi * 16U, ys1);
                    hi_desc[n_hi++] = (uint8_t)(chunk | (2U << 6));
                } else if (!hi1) {
                    hdf_st1_u16(pr_dst, live_knot16_q13b_pair(ys1, &kq, c15));
                    hdf_st1_u8(boost_scratch + (size_t)n_hi * 16U, ys0);
                    hi_desc[n_hi++] = (uint8_t)(chunk | (2U << 6));
                } else {
                    hdf_st1_u8(boost_scratch + (size_t)n_hi * 16U, ys0);
                    hi_desc[n_hi++] = (uint8_t)(chunk | (1U << 6));
                    hdf_st1_u8(boost_scratch + (size_t)n_hi * 16U, ys1);
                    hi_desc[n_hi++] = (uint8_t)(chunk | (2U << 6));
                }
                pr_dst += 8U;
            }
        } else {
            for (uint32_t chunk = 0; chunk < HDF_LIVE_CW / 16U; ++chunk) {
                uint8x16x2_t a0 = vld2q_u8(y0); y0 += 32U;
                uint8x16x2_t a1 = vld2q_u8(y1); y1 += 32U;
                uint8x16_t ys0 = vhaddq_u8(vrhaddq_u8(a0.val[0], a0.val[1]),
                                           vrhaddq_u8(a1.val[0], a1.val[1]));
                if (__builtin_expect(vmaxvq_u8(ys0) <= 158U, 1)) {
                    uint16x8_t p0 = live_knot16_q13b_pair(ys0, &kq, c15);
                    hdf_st1_u16(pr_dst, vrshrq_n_u16(p0, 1));
                } else {
                    hdf_st1_u8(boost_scratch + (size_t)n_hi * 16U, ys0);
                    hi_desc[n_hi++] = (uint8_t)chunk;
                }
                pr_dst += 8U;
            }
        }
        if (n_hi > 0U) {
            live_s1_boost_mono_deferred(boost_scratch, hi_desc, n_hi,
                                        boost_lo, boost_hi, gr_out);
        }
        return;
    }

    uint8x8_t  c15_8   = vdup_n_u8(15U);
    uint8x8_t  c128_8  = vdup_n_u8(128U);
    uint8x16_t c128_16 = vdupq_n_u8(128U);
    int16x8_t c103 = vdupq_n_s16(103 << 7);
    int16x8_t c198 = vdupq_n_s16(198 << 7);
    int16x8_t c88  = vdupq_n_s16(88 << 7);
    int16x8_t c183 = vdupq_n_s16(183 << 7);

    const uint8_t *y0_b = y0, *y1_b = y1, *c0_b = c0;
    const uint8_t *y2_b = y2, *y3_b = y3, *c1_b = c1;
    uint8_t hi_chunks[32], b8_chunks[32];
    uint32_t n_hi_c = 0U, n_b8 = 0U;
    uint32_t c_mask = 0U, y_mask = 0U;
    uint16_t *pr_dst = gr_out, *pg_dst = gg_out, *pb_dst = gb_out;
    uint8_t *r256_p = r256_out, *g256_p = g256_out, *b256_p = b256_out;

    if (y2) {
        for (uint32_t chunk = 0; chunk < HDF_LIVE_CW / 16U; ++chunk) {
            uint8x16x2_t a0 = vld2q_u8(y0); y0 += 32U;
            uint8x16x2_t a1 = vld2q_u8(y1); y1 += 32U;
            uint8x16_t ys0 = vhaddq_u8(vrhaddq_u8(a0.val[0], a0.val[1]),
                                       vrhaddq_u8(a1.val[0], a1.val[1]));
            uint8x16x2_t vc0 = vld2q_u8(c0); c0 += 32U;

            uint8x16x2_t b0 = vld2q_u8(y2); y2 += 32U;
            uint8x16x2_t b1 = vld2q_u8(y3); y3 += 32U;
            uint8x16_t ys1 = vhaddq_u8(vrhaddq_u8(b0.val[0], b0.val[1]),
                                       vrhaddq_u8(b1.val[0], b1.val[1]));
            uint8x16x2_t vc1 = vld2q_u8(c1); c1 += 32U;

            uint8x16_t cb_max = vmaxq_u8(vc0.val[0], vc1.val[0]);
            uint8x16_t cb_min = vminq_u8(vc0.val[0], vc1.val[0]);
            uint8x16_t cr_max = vmaxq_u8(vc0.val[1], vc1.val[1]);
            uint8x16_t cr_min = vminq_u8(vc0.val[1], vc1.val[1]);
            uint8x16_t c_max4 = vpmaxq_u8(cb_max, cr_max);
            uint8x16_t c_min4 = vpminq_u8(cb_min, cr_min);
            uint32_t c_span  = vmaxvq_u8(vsubq_u8(c_max4, c_min4));
            uint32_t sat_max = vmaxvq_u8(vmaxq_u8(vabdq_u8(c_max4, c128_16),
                                                  vabdq_u8(c_min4, c128_16)));

            uint8x16_t y_max = vmaxq_u8(ys0, ys1);
            uint8x16_t y_min = vminq_u8(ys0, ys1);
            uint32_t y_max_all = vmaxvq_u8(y_max);
            uint32_t y_span = vmaxvq_u8(vsubq_u8(vpmaxq_u8(y_max, y_max), vpminq_u8(y_min, y_min)));
            if (y_span > 48U) {
                y_mask |= (1U << chunk);
            }
            if (c_span > 16U || (y_span > 48U && sat_max > 16U)) {
                c_mask |= (1U << chunk);
            }

            /* 256x171 fast path for all boost==0 chunks AND smooth bright chunks (4x fewer px!) */
            if (__builtin_expect((y_max_all + 2U * sat_max <= 158U) ||
                                 (y_span <= 40U && c_span <= 16U), 1)) {
                uint8x8_t y256  = vrshrn_n_u16(vpadalq_u8(vpaddlq_u8(ys0), ys1), 2);
                uint8x8_t cb256 = vrshrn_n_u16(vpadalq_u8(vpaddlq_u8(vc0.val[0]), vc1.val[0]), 2);
                uint8x8_t cr256 = vrshrn_n_u16(vpadalq_u8(vpaddlq_u8(vc0.val[1]), vc1.val[1]), 2);
                int16x8_t cbs = vreinterpretq_s16_u16(vsubl_u8(cb256, c128_8));
                int16x8_t crs = vreinterpretq_s16_u16(vsubl_u8(cr256, c128_8));
                int16x8_t dr  = vaddq_s16(crs, vqrdmulhq_s16(crs, c103));
                int16x8_t db  = vaddq_s16(cbs, vqrdmulhq_s16(cbs, c198));
                int16x8_t dg  = vnegq_s16(vaddq_s16(vqrdmulhq_s16(cbs, c88), vqrdmulhq_s16(crs, c183)));
                uint8x8_t r8 = vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dr), y256)));
                uint8x8_t g8 = vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dg), y256)));
                uint8x8_t b8 = vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(db), y256)));
                live_st1_u8_8(r256_p, r8);
                live_st1_u8_8(g256_p, g8);
                live_st1_u8_8(b256_p, b8);
                if (__builtin_expect(vmaxv_u8(vmax_u8(r8, vmax_u8(g8, b8))) <= 158U, 1)) {
                    hdf_st1_u16(pr_dst, live_knot16_q13b_8px(r8, &kq, c15_8));
                    hdf_st1_u16(pg_dst, live_knot16_q13b_8px(g8, &kq, c15_8));
                    hdf_st1_u16(pb_dst, live_knot16_q13b_8px(b8, &kq, c15_8));
                } else {
                    uint8_t *d = boost_scratch + (size_t)n_b8 * 24U;
                    live_st1_u8_8(d +  0U, r8);
                    live_st1_u8_8(d +  8U, g8);
                    live_st1_u8_8(d + 16U, b8);
                    b8_chunks[n_b8++] = (uint8_t)chunk;
                }
            } else {
                hi_chunks[n_hi_c++] = (uint8_t)chunk;
            }
            r256_p += 8U; g256_p += 8U; b256_p += 8U;
            pr_dst += 8U; pg_dst += 8U; pb_dst += 8U;
        }
    } else {
        for (uint32_t chunk = 0; chunk < HDF_LIVE_CW / 16U; ++chunk) {
            uint8x16x2_t a0 = vld2q_u8(y0); y0 += 32U;
            uint8x16x2_t a1 = vld2q_u8(y1); y1 += 32U;
            uint8x16_t ys0 = vhaddq_u8(vrhaddq_u8(a0.val[0], a0.val[1]),
                                       vrhaddq_u8(a1.val[0], a1.val[1]));
            uint8x16x2_t vc0 = vld2q_u8(c0); c0 += 32U;

            uint8x16_t c_max4 = vpmaxq_u8(vc0.val[0], vc0.val[1]);
            uint8x16_t c_min4 = vpminq_u8(vc0.val[0], vc0.val[1]);
            uint32_t c_span  = vmaxvq_u8(vsubq_u8(c_max4, c_min4));
            uint32_t sat_max = vmaxvq_u8(vmaxq_u8(vabdq_u8(c_max4, c128_16),
                                                  vabdq_u8(c_min4, c128_16)));
            uint32_t y_max_all = vmaxvq_u8(ys0);
            uint32_t y_span = vmaxvq_u8(vsubq_u8(vpmaxq_u8(ys0, ys0), vpminq_u8(ys0, ys0)));
            if (y_span > 48U) {
                y_mask |= (1U << chunk);
            }
            if (c_span > 16U || (y_span > 48U && sat_max > 16U)) {
                c_mask |= (1U << chunk);
            }

            if (__builtin_expect((y_max_all + 2U * sat_max <= 158U) ||
                                 (y_span <= 40U && c_span <= 16U), 1)) {
                uint8x8_t y256  = vrshrn_n_u16(vpaddlq_u8(ys0), 1);
                uint8x8_t cb256 = vrshrn_n_u16(vpaddlq_u8(vc0.val[0]), 1);
                uint8x8_t cr256 = vrshrn_n_u16(vpaddlq_u8(vc0.val[1]), 1);
                int16x8_t cbs = vreinterpretq_s16_u16(vsubl_u8(cb256, c128_8));
                int16x8_t crs = vreinterpretq_s16_u16(vsubl_u8(cr256, c128_8));
                int16x8_t dr  = vaddq_s16(crs, vqrdmulhq_s16(crs, c103));
                int16x8_t db  = vaddq_s16(cbs, vqrdmulhq_s16(cbs, c198));
                int16x8_t dg  = vnegq_s16(vaddq_s16(vqrdmulhq_s16(cbs, c88), vqrdmulhq_s16(crs, c183)));
                uint8x8_t r8 = vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dr), y256)));
                uint8x8_t g8 = vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dg), y256)));
                uint8x8_t b8 = vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(db), y256)));
                live_st1_u8_8(r256_p, r8);
                live_st1_u8_8(g256_p, g8);
                live_st1_u8_8(b256_p, b8);
                if (__builtin_expect(vmaxv_u8(vmax_u8(r8, vmax_u8(g8, b8))) <= 158U, 1)) {
                    hdf_st1_u16(pr_dst, live_knot16_q13b_8px(r8, &kq, c15_8));
                    hdf_st1_u16(pg_dst, live_knot16_q13b_8px(g8, &kq, c15_8));
                    hdf_st1_u16(pb_dst, live_knot16_q13b_8px(b8, &kq, c15_8));
                } else {
                    uint8_t *d = boost_scratch + (size_t)n_b8 * 24U;
                    live_st1_u8_8(d +  0U, r8);
                    live_st1_u8_8(d +  8U, g8);
                    live_st1_u8_8(d + 16U, b8);
                    b8_chunks[n_b8++] = (uint8_t)chunk;
                }
            } else {
                hi_chunks[n_hi_c++] = (uint8_t)chunk;
            }
            r256_p += 8U; g256_p += 8U; b256_p += 8U;
            pr_dst += 8U; pg_dst += 8U; pb_dst += 8U;
        }
    }

    *c_edge_mask_out = c_mask;
    *y_edge_mask_out = y_mask;
    if (n_b8 > 0U) {
        live_s1_boost_8px_deferred(boost_scratch, b8_chunks, n_b8,
                                   boost_lo, boost_hi, gr_out, gg_out, gb_out);
    }
    if (n_hi_c > 0U) {
        live_s1_hi_chunks_512(y0_b, y1_b, c0_b, y2_b, y3_b, c1_b,
                              hi_chunks, n_hi_c, boost_lo, boost_hi,
                              gr_out, gg_out, gb_out,
                              r256_out, g256_out, b256_out, boost_scratch);
    }
#else
    (void)boost_scratch;
    if (monochrome) {
        *c_edge_mask_out = 0U;
        *y_edge_mask_out = 0U;
        for (uint32_t gx = 0; gx < HDF_LIVE_GW; ++gx) {
            uint16_t p0r = 0, p0g = 0, p0b = 0;
            uint8_t dummy[2];
            live_s1_halfrow_scalar(y0, y1, c0, boost_lo, boost_hi, 2U * gx, 1U,
                                   &p0r, &p0g, &p0b, dummy, dummy, dummy, dummy, dummy, dummy);
            if (y2) {
                uint16_t p1r = 0, p1g = 0, p1b = 0;
                live_s1_halfrow_scalar(y2, y3, c1, boost_lo, boost_hi, 2U * gx, 1U,
                                       &p1r, &p1g, &p1b, dummy, dummy, dummy, dummy, dummy, dummy);
                gr_out[gx] = (uint16_t)(urhadd_u16_s(p0r, p1r) >> 1);
            } else {
                gr_out[gx] = (uint16_t)((p0r + 1U) >> 1);
            }
        }
        return;
    }

    uint32_t c_mask = 0U, y_mask = 0U;
    for (uint32_t chunk = 0; chunk < HDF_LIVE_GW / 8U; ++chunk) {
        uint8_t ys0_a[16], ys1_a[16], cb0_a[16], cr0_a[16], cb1_a[16], cr1_a[16];
        uint32_t c_span_max = 0U, y_span_max = 0U, sat_max = 0U, y_max_all = 0U;
        for (uint32_t j = 0; j < 8U; ++j) {
            uint32_t gx = chunk * 8U + j;
            for (uint32_t d = 0; d < 2U; ++d) {
                uint32_t cx = 2U * gx + d;
                uint32_t idx = 2U * j + d;
                ys0_a[idx] = uhadd_u8_s(urhadd_u8_s(y0[2U * cx], y0[2U * cx + 1U]),
                                        urhadd_u8_s(y1[2U * cx], y1[2U * cx + 1U]));
                cb0_a[idx] = c0[2U * cx];
                cr0_a[idx] = c0[2U * cx + 1U];
                if (y2) {
                    ys1_a[idx] = uhadd_u8_s(urhadd_u8_s(y2[2U * cx], y2[2U * cx + 1U]),
                                            urhadd_u8_s(y3[2U * cx], y3[2U * cx + 1U]));
                    cb1_a[idx] = c1[2U * cx];
                    cr1_a[idx] = c1[2U * cx + 1U];
                }
            }
            uint32_t cbmin = 255U, cbmax = 0U, crmin = 255U, crmax = 0U, ymin = 255U, ymax = 0U;
            uint32_t n_s = y2 ? 4U : 2U;
            uint8_t cbs[4] = { cb0_a[2U*j], cb0_a[2U*j+1U], y2 ? cb1_a[2U*j] : 0U, y2 ? cb1_a[2U*j+1U] : 0U };
            uint8_t crs[4] = { cr0_a[2U*j], cr0_a[2U*j+1U], y2 ? cr1_a[2U*j] : 0U, y2 ? cr1_a[2U*j+1U] : 0U };
            uint8_t yss[4] = { ys0_a[2U*j], ys0_a[2U*j+1U], y2 ? ys1_a[2U*j] : 0U, y2 ? ys1_a[2U*j+1U] : 0U };
            for (uint32_t k = 0; k < n_s; ++k) {
                if (cbs[k] < cbmin) cbmin = cbs[k]; if (cbs[k] > cbmax) cbmax = cbs[k];
                if (crs[k] < crmin) crmin = crs[k]; if (crs[k] > crmax) crmax = crs[k];
                if (yss[k] < ymin)  ymin  = yss[k]; if (yss[k] > ymax)  ymax  = yss[k];
            }
            if (ymax > y_max_all) y_max_all = ymax;
            uint32_t cs = (cbmax - cbmin) > (crmax - crmin) ? (cbmax - cbmin) : (crmax - crmin);
            uint32_t yd = ymax - ymin;
            if (cs > c_span_max) c_span_max = cs;
            if (yd > y_span_max) y_span_max = yd;
            uint32_t s0 = cbmax >= 128U ? cbmax - 128U : 128U - cbmax;
            uint32_t s1 = cbmin >= 128U ? cbmin - 128U : 128U - cbmin;
            uint32_t s2 = crmax >= 128U ? crmax - 128U : 128U - crmax;
            uint32_t s3 = crmin >= 128U ? crmin - 128U : 128U - crmin;
            if (s0 > sat_max) sat_max = s0; if (s1 > sat_max) sat_max = s1;
            if (s2 > sat_max) sat_max = s2; if (s3 > sat_max) sat_max = s3;
        }
        if (y_span_max > 48U) y_mask |= (1U << chunk);
        if (c_span_max > 16U || (y_span_max > 48U && sat_max > 16U)) c_mask |= (1U << chunk);

        if ((y_max_all + 2U * sat_max <= 158U) || (y_span_max <= 40U && c_span_max <= 16U)) {
            uint8_t r8_a[8], g8_a[8], b8_a[8];
            uint32_t max_rgb = 0U;
            for (uint32_t j = 0; j < 8U; ++j) {
                uint32_t gx = chunk * 8U + j;
                uint8_t y256, cb256, cr256;
                if (y2) {
                    y256  = (uint8_t)(((uint32_t)ys0_a[2U*j] + ys0_a[2U*j+1U] + ys1_a[2U*j] + ys1_a[2U*j+1U] + 2U) >> 2);
                    cb256 = (uint8_t)(((uint32_t)cb0_a[2U*j] + cb0_a[2U*j+1U] + cb1_a[2U*j] + cb1_a[2U*j+1U] + 2U) >> 2);
                    cr256 = (uint8_t)(((uint32_t)cr0_a[2U*j] + cr0_a[2U*j+1U] + cr1_a[2U*j] + cr1_a[2U*j+1U] + 2U) >> 2);
                } else {
                    y256  = (uint8_t)(((uint32_t)ys0_a[2U*j] + ys0_a[2U*j+1U] + 1U) >> 1);
                    cb256 = (uint8_t)(((uint32_t)cb0_a[2U*j] + cb0_a[2U*j+1U] + 1U) >> 1);
                    cr256 = (uint8_t)(((uint32_t)cr0_a[2U*j] + cr0_a[2U*j+1U] + 1U) >> 1);
                }
                int16_t cb  = (int16_t)((int32_t)cb256 - 128);
                int16_t crv = (int16_t)((int32_t)cr256 - 128);
                int32_t dr = (int32_t)crv + sqrdmulh_s16_scalar(crv, (int16_t)(103 << 7));
                int32_t db = (int32_t)cb  + sqrdmulh_s16_scalar(cb,  (int16_t)(198 << 7));
                int32_t dg = -(sqrdmulh_s16_scalar(cb, (int16_t)(88 << 7)) +
                               sqrdmulh_s16_scalar(crv, (int16_t)(183 << 7)));
                uint8_t r8 = clamp_u8((int)y256 + (int)dr);
                uint8_t g8 = clamp_u8((int)y256 + (int)dg);
                uint8_t b8 = clamp_u8((int)y256 + (int)db);
                r8_a[j] = r8; g8_a[j] = g8; b8_a[j] = b8;
                r256_out[gx] = r8; g256_out[gx] = g8; b256_out[gx] = b8;
                if (r8 > max_rgb) max_rgb = r8;
                if (g8 > max_rgb) max_rgb = g8;
                if (b8 > max_rgb) max_rgb = b8;
            }
            if (max_rgb <= 158U) {
                for (uint32_t j = 0; j < 8U; ++j) {
                    uint32_t gx = chunk * 8U + j;
                    gr_out[gx] = (uint16_t)((live_knot16_eval_scalar(r8_a[j], k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp) + 2U) >> 2);
                    gg_out[gx] = (uint16_t)((live_knot16_eval_scalar(g8_a[j], k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp) + 2U) >> 2);
                    gb_out[gx] = (uint16_t)((live_knot16_eval_scalar(b8_a[j], k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp) + 2U) >> 2);
                }
            } else {
                for (uint32_t j = 0; j < 8U; ++j) {
                    uint32_t gx = chunk * 8U + j;
                    uint16_t lr = live_knot16_eval_scalar(r8_a[j], k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
                    uint16_t lg = live_knot16_eval_scalar(g8_a[j], k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
                    uint16_t lb = live_knot16_eval_scalar(b8_a[j], k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
                    uint16_t lmax = lr > lg ? lr : lg;
                    if (lb > lmax) lmax = lb;
                    uint16_t llum = (uint16_t)sqadd_s16_scalar(
                        sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lr, (int16_t)(77 << 7)),
                                         sqrdmulh_s16_scalar((int16_t)lg, (int16_t)(150 << 7))),
                        sqrdmulh_s16_scalar((int16_t)lb, (int16_t)(29 << 7)));
                    uint16_t ldrv = (uint16_t)(lmax - (lmax >> 3));
                    if (llum > ldrv) ldrv = llum;
                    uint32_t bidx = (uint32_t)ldrv >> 9;
                    int16_t bh = (int16_t)((uint16_t)boost_lo[bidx] | ((uint16_t)boost_hi[bidx] << 8));
                    int16_t nt = sqrdmulh_s16_scalar((int16_t)ldrv, (int16_t)(96 << 7));
                    uint32_t er = ((uint32_t)lr + 2U) >> 2;
                    uint32_t eg = ((uint32_t)lg + 2U) >> 2;
                    uint32_t eb = ((uint32_t)lb + 2U) >> 2;
                    er += (uint32_t)(uint16_t)sqrdmulh_s16_scalar(
                        bh, sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lr, (int16_t)(160 << 7)), nt));
                    eg += (uint32_t)(uint16_t)sqrdmulh_s16_scalar(
                        bh, sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lg, (int16_t)(160 << 7)), nt));
                    eb += (uint32_t)(uint16_t)sqrdmulh_s16_scalar(
                        bh, sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lb, (int16_t)(160 << 7)), nt));
                    gr_out[gx] = (uint16_t)(er > 32767U ? 32767U : er);
                    gg_out[gx] = (uint16_t)(eg > 32767U ? 32767U : eg);
                    gb_out[gx] = (uint16_t)(eb > 32767U ? 32767U : eb);
                }
            }
        } else {
            for (uint32_t j = 0; j < 8U; ++j) {
                uint32_t gx = chunk * 8U + j;
                uint16_t p0r = 0, p0g = 0, p0b = 0;
                uint8_t r0[2] = {0, 0}, g0[2] = {0, 0}, b0[2] = {0, 0};
                uint8_t d1[2], d2[2], d3[2];
                live_s1_halfrow_scalar(y0, y1, c0, boost_lo, boost_hi, 2U * gx, 0U,
                                       &p0r, &p0g, &p0b, r0, g0, b0, d1, d2, d3);
                if (y2) {
                    uint16_t p1r = 0, p1g = 0, p1b = 0;
                    uint8_t r1[2] = {0, 0}, g1[2] = {0, 0}, b1[2] = {0, 0};
                    live_s1_halfrow_scalar(y2, y3, c1, boost_lo, boost_hi, 2U * gx, 0U,
                                           &p1r, &p1g, &p1b, r1, g1, b1, d1, d2, d3);
                    gr_out[gx] = (uint16_t)(urhadd_u16_s(p0r, p1r) >> 1);
                    gg_out[gx] = (uint16_t)(urhadd_u16_s(p0g, p1g) >> 1);
                    gb_out[gx] = (uint16_t)(urhadd_u16_s(p0b, p1b) >> 1);
                    r256_out[gx] = (uint8_t)(((uint32_t)r0[0] + r0[1] + r1[0] + r1[1] + 2U) >> 2);
                    g256_out[gx] = (uint8_t)(((uint32_t)g0[0] + g0[1] + g1[0] + g1[1] + 2U) >> 2);
                    b256_out[gx] = (uint8_t)(((uint32_t)b0[0] + b0[1] + b1[0] + b1[1] + 2U) >> 2);
                } else {
                    gr_out[gx] = (uint16_t)((p0r + 1U) >> 1);
                    gg_out[gx] = (uint16_t)((p0g + 1U) >> 1);
                    gb_out[gx] = (uint16_t)((p0b + 1U) >> 1);
                    r256_out[gx] = (uint8_t)(((uint32_t)r0[0] + r0[1] + 1U) >> 1);
                    g256_out[gx] = (uint8_t)(((uint32_t)g0[0] + g0[1] + 1U) >> 1);
                    b256_out[gx] = (uint8_t)(((uint32_t)b0[0] + b0[1] + 1U) >> 1);
                }
            }
        }
    }
    *c_edge_mask_out = c_mask;
    *y_edge_mask_out = y_mask;
#endif
}

void hdf_live_ds2x2_u16(const uint16_t *src, uint32_t sw, uint32_t sh,
                        uint16_t *dst, uint32_t dw, uint32_t dh) {
    for (uint32_t y = 0; y < dh; ++y) {
        uint32_t y0 = 2U * y;
        uint32_t y1 = y0 + 1U < sh ? y0 + 1U : sh - 1U;
        const uint16_t *r0 = src + (size_t)y0 * sw;
        const uint16_t *r1 = src + (size_t)y1 * sw;
        uint16_t *out = dst + (size_t)y * dw;
#if HDF_NEON
        for (uint32_t n = dw >> 3; n > 0U; --n) {
            uint16x8x2_t a = vld2q_u16(r0); r0 += 16U;
            uint16x8x2_t b = vld2q_u16(r1); r1 += 16U;
            live_st1_u16_inc(&out, vhaddq_u16(vrhaddq_u16(a.val[0], a.val[1]),
                                              vrhaddq_u16(b.val[0], b.val[1])));
        }
#else
        for (uint32_t x = 0; x < dw; ++x) {
            out[x] = uhadd_u16_s(urhadd_u16_s(r0[2U * x], r0[2U * x + 1U]),
                                 urhadd_u16_s(r1[2U * x], r1[2U * x + 1U]));
        }
#endif
    }
}

/* Fused 2D [1,2,2,2,1]/8 + [1,2,1]/4 (var=2.0), 2x-unrolled H & V loops with 0 reg moves */
void hdf_live_blur_sym53_2d(uint16_t *grid, uint16_t *tmp, uint32_t w, uint32_t h) {
#if HDF_NEON
    (void)tmp;
    /* Branchless 2x-unrolled fused horizontal pass (w = 256, multiple of 16) */
    for (uint32_t y = 0; y < h; ++y) {
        uint16_t *row = grid + (size_t)y * w;
        const uint16_t *pin = row + 16U;
        uint16_t *pout = row;
        uint16x8_t r0 = hdf_ld1_u16(row);
        uint16x8_t r1 = hdf_ld1_u16(row + 8U);
        uint16x8_t t0 = live_sym5_h_vec(vdupq_laneq_u16(r0, 0), r0, r1);
        uint16x8_t tp = vdupq_laneq_u16(t0, 0);

        for (uint32_t n = (w - 16U) >> 4; n > 0U; --n) {
            uint16x8x2_t rn = live_ld1_u16_x2_inc(&pin);
            uint16x8_t t1 = live_sym5_h_vec(r0, r1, rn.val[0]);
            uint16x8_t o0 = live_sym3_h_vec(tp, t0, t1);
            uint16x8_t t2 = live_sym5_h_vec(r1, rn.val[0], rn.val[1]);
            uint16x8_t o1 = live_sym3_h_vec(t0, t1, t2);
            live_st1_u16_x2_inc(&pout, o0, o1);
            r0 = rn.val[0]; r1 = rn.val[1];
            tp = t1; t0 = t2;
        }
        uint16x8_t rend  = vdupq_laneq_u16(r1, 7);
        uint16x8_t tlast = live_sym5_h_vec(r0, r1, rend);
        uint16x8_t o0    = live_sym3_h_vec(tp, t0, tlast);
        uint16x8_t o1    = live_sym3_h_vec(t0, tlast, vdupq_laneq_u16(tlast, 7));
        live_st1_u16_x2_inc(&pout, o0, o1);
    }

    /* Branchless 16-col (ld1_x2/st1_x2) fused in-place vertical pass (h = 170 or 171 >= 6) */
    for (uint32_t x = 0; x < w; x += 16U) {
        uint16_t *col = grid + x;
        uint16x8x2_t r0 = vld1q_u16_x2(col);
        uint16x8x2_t r1 = vld1q_u16_x2(col + (size_t)1U * w);
        uint16x8x2_t r2 = vld1q_u16_x2(col + (size_t)2U * w);
        uint16x8x2_t r3 = vld1q_u16_x2(col + (size_t)3U * w);
        uint16x8_t t0a = live_sym5_v_vec(r0.val[0], r0.val[0], r0.val[0], r1.val[0], r2.val[0]);
        uint16x8_t t0b = live_sym5_v_vec(r0.val[1], r0.val[1], r0.val[1], r1.val[1], r2.val[1]);
        uint16x8_t t1a = live_sym5_v_vec(r0.val[0], r0.val[0], r1.val[0], r2.val[0], r3.val[0]);
        uint16x8_t t1b = live_sym5_v_vec(r0.val[1], r0.val[1], r1.val[1], r2.val[1], r3.val[1]);
        uint16x8x2_t ov0 = {{ live_sym3_v_vec(t0a, t0a, t1a), live_sym3_v_vec(t0b, t0b, t1b) }};
        vst1q_u16_x2(col, ov0);

        uint16x8_t rm2a = r0.val[0], rm1a = r1.val[0], rca = r2.val[0], rp1a = r3.val[0];
        uint16x8_t rm2b = r0.val[1], rm1b = r1.val[1], rcb = r2.val[1], rp1b = r3.val[1];
        uint16x8_t tpa = t0a, tca = t1a, tpb = t0b, tcb = t1b;
        const uint16_t *p_in = col + (size_t)4U * w;
        uint16_t *p_out = col + w;
        uint32_t rem = h - 4U;
        while (rem >= 2U) {
            uint16x8x2_t rp2 = vld1q_u16_x2(p_in); p_in += w;
            uint16x8_t tn0a = live_sym5_v_vec(rm2a, rm1a, rca, rp1a, rp2.val[0]);
            uint16x8_t tn0b = live_sym5_v_vec(rm2b, rm1b, rcb, rp1b, rp2.val[1]);
            uint16x8x2_t ov0_u = {{ live_sym3_v_vec(tpa, tca, tn0a), live_sym3_v_vec(tpb, tcb, tn0b) }};
            vst1q_u16_x2(p_out, ov0_u); p_out += w;

            uint16x8x2_t rp3 = vld1q_u16_x2(p_in); p_in += w;
            uint16x8_t tn1a = live_sym5_v_vec(rm1a, rca, rp1a, rp2.val[0], rp3.val[0]);
            uint16x8_t tn1b = live_sym5_v_vec(rm1b, rcb, rp1b, rp2.val[1], rp3.val[1]);
            uint16x8x2_t ov1_u = {{ live_sym3_v_vec(tca, tn0a, tn1a), live_sym3_v_vec(tcb, tn0b, tn1b) }};
            vst1q_u16_x2(p_out, ov1_u); p_out += w;

            rm2a = rca; rm1a = rp1a; rca = rp2.val[0]; rp1a = rp3.val[0];
            rm2b = rcb; rm1b = rp1b; rcb = rp2.val[1]; rp1b = rp3.val[1];
            tpa = tn0a; tca = tn1a; tpb = tn0b; tcb = tn1b;
            rem -= 2U;
        }
        if (rem != 0U) {
            uint16x8x2_t rp2 = vld1q_u16_x2(p_in);
            uint16x8_t tna = live_sym5_v_vec(rm2a, rm1a, rca, rp1a, rp2.val[0]);
            uint16x8_t tnb = live_sym5_v_vec(rm2b, rm1b, rcb, rp1b, rp2.val[1]);
            uint16x8x2_t ov = {{ live_sym3_v_vec(tpa, tca, tna), live_sym3_v_vec(tpb, tcb, tnb) }};
            vst1q_u16_x2(p_out, ov); p_out += w;
            rm2a = rm1a; rm1a = rca; rca = rp1a; rp1a = rp2.val[0];
            rm2b = rm1b; rm1b = rcb; rcb = rp1b; rp1b = rp2.val[1];
            tpa = tca; tca = tna; tpb = tcb; tcb = tnb;
        }
        uint16x8_t tm2a = live_sym5_v_vec(rm2a, rm1a, rca, rp1a, rp1a);
        uint16x8_t tm2b = live_sym5_v_vec(rm2b, rm1b, rcb, rp1b, rp1b);
        uint16x8x2_t ov1 = {{ live_sym3_v_vec(tpa, tca, tm2a), live_sym3_v_vec(tpb, tcb, tm2b) }};
        vst1q_u16_x2(p_out, ov1); p_out += w;
        uint16x8_t tm1a = live_sym5_v_vec(rm1a, rca, rp1a, rp1a, rp1a);
        uint16x8_t tm1b = live_sym5_v_vec(rm1b, rcb, rp1b, rp1b, rp1b);
        uint16x8x2_t ov2 = {{ live_sym3_v_vec(tca, tm2a, tm1a), live_sym3_v_vec(tcb, tm2b, tm1b) }};
        vst1q_u16_x2(p_out, ov2); p_out += w;
        uint16x8x2_t ov3 = {{ live_sym3_v_vec(tm2a, tm1a, tm1a), live_sym3_v_vec(tm2b, tm1b, tm1b) }};
        vst1q_u16_x2(p_out, ov3);
    }
#else
    for (uint32_t y = 0; y < h; ++y) {
        uint16_t *row = grid + (size_t)y * w;
        for (uint32_t x = 0; x < w; ++x) {
            uint16_t l2 = row[x >= 2U ? x - 2U : 0U];
            uint16_t l1 = row[x >= 1U ? x - 1U : 0U];
            uint16_t c  = row[x];
            uint16_t r1 = row[x + 1U < w ? x + 1U : w - 1U];
            uint16_t r2 = row[x + 2U < w ? x + 2U : w - 1U];
            tmp[x] = urhadd_u16_s(uhadd_u16_s(urhadd_u16_s(l2, r2), c), urhadd_u16_s(l1, r1));
        }
        for (uint32_t x = 0; x < w; ++x) {
            uint16_t l1 = tmp[x >= 1U ? x - 1U : 0U];
            uint16_t c  = tmp[x];
            uint16_t r1 = tmp[x + 1U < w ? x + 1U : w - 1U];
            row[x] = uhadd_u16_s(c, urhadd_u16_s(l1, r1));
        }
    }
    for (uint32_t x = 0; x < w; ++x) {
        for (uint32_t y = 0; y < h; ++y) {
            uint16_t l2 = grid[(size_t)(y >= 2U ? y - 2U : 0U) * w + x];
            uint16_t l1 = grid[(size_t)(y >= 1U ? y - 1U : 0U) * w + x];
            uint16_t c  = grid[(size_t)y * w + x];
            uint16_t r1 = grid[(size_t)(y + 1U < h ? y + 1U : h - 1U) * w + x];
            uint16_t r2 = grid[(size_t)(y + 2U < h ? y + 2U : h - 1U) * w + x];
            tmp[y] = urhadd_u16_s(uhadd_u16_s(urhadd_u16_s(l2, r2), c), urhadd_u16_s(l1, r1));
        }
        for (uint32_t y = 0; y < h; ++y) {
            uint16_t l1 = tmp[y >= 1U ? y - 1U : 0U];
            uint16_t c  = tmp[y];
            uint16_t r1 = tmp[y + 1U < h ? y + 1U : h - 1U];
            grid[(size_t)y * w + x] = uhadd_u16_s(c, urhadd_u16_s(l1, r1));
        }
    }
#endif
}

/* Pre-scaled 16-bit box blur: prefix-sum H + 16-col V (0 32-bit div, 0 copy-back, 0 Q-spills) */
void hdf_live_blur_box16_1pass(uint16_t *grid, uint16_t *tmp_grid, int16_t *htmp,
                               uint32_t w, uint32_t h, uint32_t r) {
    uint32_t kt = 2U * r + 1U;
    int16_t inv_k = (int16_t)(32767U / kt);
#if HDF_NEON
    int16x8_t v_invk = vdupq_n_s16(inv_k);

    /* H pass: grid -> tmp_grid (outputs ALREADY pre-scaled by inv_k for V pass!) */
    for (uint32_t y = 0; y < h; ++y) {
        const uint16_t *row = grid + (size_t)y * w;
        uint16_t *dst_row = tmp_grid + (size_t)y * w;
        int16_t *hp = htmp + 8U;
        for (uint32_t n = w >> 4; n > 0U; --n) {
            uint16x8x2_t v = live_ld1_u16_x2_inc(&row);
            live_st1_s16_x2_inc(&hp,
                vqrdmulhq_s16(vreinterpretq_s16_u16(v.val[0]), v_invk),
                vqrdmulhq_s16(vreinterpretq_s16_u16(v.val[1]), v_invk));
        }
        int16x8_t vfirst = vdupq_n_s16(htmp[8U]);
        int16x8_t vlast  = vdupq_n_s16(htmp[8U + w - 1U]);
        hdf_st1_s16(htmp, vfirst);
        hdf_st1_s16(htmp + 8U + w, vlast);
        hdf_st1_s16(htmp + 16U + w, vlast);
        hdf_st1_s16(htmp + 24U + w, vlast);

        const int16_t *bp = htmp + 8U - r;
        if (r == 3U) {
            /* kt = 7: prefix-pair reuse (4 ext + 4 add per 8 cells!) */
            int16x8_t v0 = live_ld1_s16_inc(&bp);
            int16x8_t v1 = live_ld1_s16_inc(&bp);
            int16x8_t p2 = vaddq_s16(v0, vextq_s16(v0, v1, 1));
            for (uint32_t n = w >> 3; n > 0U; --n) {
                int16x8_t v2  = live_ld1_s16_inc(&bp);
                int16x8_t p2n = vaddq_s16(v1, vextq_s16(v1, v2, 1));
                int16x8_t p4  = vaddq_s16(p2, vextq_s16(p2, p2n, 2));
                int16x8_t sum = vaddq_s16(vaddq_s16(p4, vextq_s16(p2, p2n, 4)),
                                          vextq_s16(v0, v1, 6));
                live_st1_u16_inc(&dst_row, vreinterpretq_u16_s16(vqrdmulhq_s16(sum, v_invk)));
                v0 = v1; v1 = v2; p2 = p2n;
            }
        } else if (r == 4U) {
            /* kt = 9: prefix-quad reuse (3 ext + 4 add per 8 cells!) */
            int16x8_t v0 = live_ld1_s16_inc(&bp);
            int16x8_t v1 = live_ld1_s16_inc(&bp);
            int16x8_t v2 = live_ld1_s16_inc(&bp);
            int16x8_t p2 = vaddq_s16(v0, vextq_s16(v0, v1, 1));
            int16x8_t p2n = vaddq_s16(v1, vextq_s16(v1, v2, 1));
            int16x8_t p4 = vaddq_s16(p2, vextq_s16(p2, p2n, 2));
            for (uint32_t n = w >> 3; n > 0U; --n) {
                int16x8_t v3  = live_ld1_s16_inc(&bp);
                int16x8_t p2nn = vaddq_s16(v2, vextq_s16(v2, v3, 1));
                int16x8_t p4n = vaddq_s16(p2n, vextq_s16(p2n, p2nn, 2));
                int16x8_t sum = vaddq_s16(vaddq_s16(p4, vextq_s16(p4, p4n, 4)), v1);
                live_st1_u16_inc(&dst_row, vreinterpretq_u16_s16(vqrdmulhq_s16(sum, v_invk)));
                v1 = v2; v2 = v3; p2n = p2nn; p4 = p4n;
            }
        } else if (r == 5U) {
            /* kt = 11: prefix-quad reuse (5 ext + 6 add per 8 cells!) */
            int16x8_t v0 = live_ld1_s16_inc(&bp);
            int16x8_t v1 = live_ld1_s16_inc(&bp);
            int16x8_t v2 = live_ld1_s16_inc(&bp);
            int16x8_t p2 = vaddq_s16(v0, vextq_s16(v0, v1, 1));
            int16x8_t p2n = vaddq_s16(v1, vextq_s16(v1, v2, 1));
            int16x8_t p4 = vaddq_s16(p2, vextq_s16(p2, p2n, 2));
            for (uint32_t n = w >> 3; n > 0U; --n) {
                int16x8_t v3  = live_ld1_s16_inc(&bp);
                int16x8_t p2nn = vaddq_s16(v2, vextq_s16(v2, v3, 1));
                int16x8_t p4n = vaddq_s16(p2n, vextq_s16(p2n, p2nn, 2));
                int16x8_t p8  = vaddq_s16(p4, vextq_s16(p4, p4n, 4));
                int16x8_t sum = vaddq_s16(p8, vaddq_s16(p2n, vextq_s16(v1, v2, 2)));
                live_st1_u16_inc(&dst_row, vreinterpretq_u16_s16(vqrdmulhq_s16(sum, v_invk)));
                v1 = v2; v2 = v3; p2n = p2nn; p4 = p4n;
            }
        } else {
            for (uint32_t x = 0; x < w; x += 8U) {
                int16x8_t sum = vdupq_n_s16(0);
                for (uint32_t t = 0; t < kt; ++t) sum = vaddq_s16(sum, hdf_ld1_s16(bp + x + t));
                hdf_st1_u16(dst_row + x, vreinterpretq_u16_s16(vqrdmulhq_s16(sum, v_invk)));
            }
        }
    }

    /* V pass: 16 columns at a time (ld1_x2/st1_x2), tmp_grid -> grid directly (0 copy-back) */
    for (uint32_t x = 0; x < w; x += 16U) {
        const uint16_t *tcol = tmp_grid + x;
        uint16_t *gcol = grid + x;
        uint16x8x2_t top_u = vld1q_u16_x2(tcol);
        int16x8_t t0 = vreinterpretq_s16_u16(top_u.val[0]);
        int16x8_t t1 = vreinterpretq_s16_u16(top_u.val[1]);
        int16x8_t s0 = t0, s1 = t1;
        for (uint32_t d = 1U; d <= r; ++d) {
            uint32_t ry = d < h ? d : h - 1U;
            uint16x8x2_t v = vld1q_u16_x2(tcol + (size_t)ry * w);
            s0 = vaddq_s16(vaddq_s16(s0, t0), vreinterpretq_s16_u16(v.val[0]));
            s1 = vaddq_s16(vaddq_s16(s1, t1), vreinterpretq_s16_u16(v.val[1]));
        }
        uint16x8x2_t out_v = {{ vreinterpretq_u16_s16(s0), vreinterpretq_u16_s16(s1) }};
        vst1q_u16_x2(gcol, out_v);

        if (h > 2U * r + 1U) {
            const uint16_t *p_ent = tcol + (size_t)(r + 1U) * w;
            uint16_t *p_dst = gcol + w;
            for (uint32_t y = 1U; y <= r; ++y) {
                uint16x8x2_t ve = vld1q_u16_x2(p_ent); p_ent += w;
                s0 = vaddq_s16(s0, vsubq_s16(vreinterpretq_s16_u16(ve.val[0]), t0));
                s1 = vaddq_s16(s1, vsubq_s16(vreinterpretq_s16_u16(ve.val[1]), t1));
                uint16x8x2_t ov = {{ vreinterpretq_u16_s16(s0), vreinterpretq_u16_s16(s1) }};
                vst1q_u16_x2(p_dst, ov); p_dst += w;
            }
            const uint16_t *p_lve = tcol;
            for (uint32_t i = h - 2U * r - 1U; i > 0U; --i) {
                uint16x8x2_t vl = vld1q_u16_x2(p_lve); p_lve += w;
                uint16x8x2_t ve = vld1q_u16_x2(p_ent); p_ent += w;
                s0 = vaddq_s16(s0, vsubq_s16(vreinterpretq_s16_u16(ve.val[0]),
                                             vreinterpretq_s16_u16(vl.val[0])));
                s1 = vaddq_s16(s1, vsubq_s16(vreinterpretq_s16_u16(ve.val[1]),
                                             vreinterpretq_s16_u16(vl.val[1])));
                uint16x8x2_t ov = {{ vreinterpretq_u16_s16(s0), vreinterpretq_u16_s16(s1) }};
                vst1q_u16_x2(p_dst, ov); p_dst += w;
            }
            uint16x8x2_t bot_u = vld1q_u16_x2(tcol + (size_t)(h - 1U) * w);
            int16x8_t b0 = vreinterpretq_s16_u16(bot_u.val[0]);
            int16x8_t b1 = vreinterpretq_s16_u16(bot_u.val[1]);
            for (uint32_t y = 0; y < r; ++y) {
                uint16x8x2_t vl = vld1q_u16_x2(p_lve); p_lve += w;
                s0 = vaddq_s16(s0, vsubq_s16(b0, vreinterpretq_s16_u16(vl.val[0])));
                s1 = vaddq_s16(s1, vsubq_s16(b1, vreinterpretq_s16_u16(vl.val[1])));
                uint16x8x2_t ov = {{ vreinterpretq_u16_s16(s0), vreinterpretq_u16_s16(s1) }};
                vst1q_u16_x2(p_dst, ov); p_dst += w;
            }
        } else {
            for (uint32_t y = 1U; y < h; ++y) {
                int32_t li = (int32_t)y - 1 - (int32_t)r;
                int32_t ei = (int32_t)y + (int32_t)r;
                uint32_t yl = li < 0 ? 0U : ((uint32_t)li >= h ? h - 1U : (uint32_t)li);
                uint32_t ye = ei < 0 ? 0U : ((uint32_t)ei >= h ? h - 1U : (uint32_t)ei);
                uint16x8x2_t vl = vld1q_u16_x2(tcol + (size_t)yl * w);
                uint16x8x2_t ve = vld1q_u16_x2(tcol + (size_t)ye * w);
                s0 = vaddq_s16(s0, vsubq_s16(vreinterpretq_s16_u16(ve.val[0]),
                                             vreinterpretq_s16_u16(vl.val[0])));
                s1 = vaddq_s16(s1, vsubq_s16(vreinterpretq_s16_u16(ve.val[1]),
                                             vreinterpretq_s16_u16(vl.val[1])));
                uint16x8x2_t ov = {{ vreinterpretq_u16_s16(s0), vreinterpretq_u16_s16(s1) }};
                vst1q_u16_x2(gcol + (size_t)y * w, ov);
            }
        }
    }
#else
    for (uint32_t y = 0; y < h; ++y) {
        const uint16_t *row = grid + (size_t)y * w;
        uint16_t *dst_row = tmp_grid + (size_t)y * w;
        for (uint32_t x = 0; x < w; ++x) {
            htmp[8U + x] = sqrdmulh_s16_scalar((int16_t)row[x], inv_k);
        }
        int16_t first = htmp[8U], last = htmp[8U + w - 1U];
        for (uint32_t p = 0; p < 8U; ++p) htmp[p] = first;
        for (uint32_t p = 0; p < 16U; ++p) htmp[8U + w + p] = last;
        const int16_t *base = htmp + 8U - r;
        for (uint32_t x = 0; x < w; ++x) {
            int32_t sum = 0;
            for (uint32_t t = 0; t < kt; ++t) sum += (int32_t)base[x + t];
            dst_row[x] = (uint16_t)sqrdmulh_s16_scalar((int16_t)sum, inv_k);
        }
    }
    for (uint32_t x = 0; x < w; ++x) {
        int32_t sum = 0;
        for (int32_t d = -(int32_t)r; d <= (int32_t)r; ++d) {
            uint32_t ry = d < 0 ? 0U : ((uint32_t)d >= h ? h - 1U : (uint32_t)d);
            sum += (int32_t)(int16_t)tmp_grid[(size_t)ry * w + x];
        }
        grid[x] = (uint16_t)(int16_t)sum;
        for (uint32_t y = 1U; y < h; ++y) {
            int32_t li = (int32_t)y - 1 - (int32_t)r;
            int32_t ei = (int32_t)y + (int32_t)r;
            uint32_t yl = li < 0 ? 0U : ((uint32_t)li >= h ? h - 1U : (uint32_t)li);
            uint32_t ye = ei < 0 ? 0U : ((uint32_t)ei >= h ? h - 1U : (uint32_t)ei);
            sum += (int32_t)(int16_t)tmp_grid[(size_t)ye * w + x]
                 - (int32_t)(int16_t)tmp_grid[(size_t)yl * w + x];
            grid[(size_t)y * w + x] = (uint16_t)(int16_t)sum;
        }
    }
#endif
}

void hdf_live_vlerp_q13_padded(const uint16_t *r0, const uint16_t *r1,
                               int16_t *buf_with_pad, uint32_t sw, uint32_t fy_q8) {
    int16_t *out = buf_with_pad + 8U;
#if HDF_NEON
    int16x8_t vfy = vdupq_n_s16((int16_t)(fy_q8 << 7));
    int16_t *op = out;
    for (uint32_t n = sw >> 4; n > 0U; --n) {
        uint16x8x2_t a = live_ld1_u16_x2_inc(&r0);
        uint16x8x2_t b = live_ld1_u16_x2_inc(&r1);
        int16x8_t a0 = vreinterpretq_s16_u16(a.val[0]), b0 = vreinterpretq_s16_u16(b.val[0]);
        int16x8_t a1 = vreinterpretq_s16_u16(a.val[1]), b1 = vreinterpretq_s16_u16(b.val[1]);
        live_st1_s16_x2_inc(&op,
            vaddq_s16(a0, vqrdmulhq_s16(vsubq_s16(b0, a0), vfy)),
            vaddq_s16(a1, vqrdmulhq_s16(vsubq_s16(b1, a1), vfy)));
    }
    out[-1] = out[0];
    hdf_st1_s16(out + sw, vdupq_n_s16(out[sw - 1U]));
#else
    int16_t fy15 = (int16_t)(fy_q8 << 7);
    for (uint32_t x = 0; x < sw; ++x) {
        int16_t a = (int16_t)r0[x];
        int16_t b = (int16_t)r1[x];
        out[x] = (int16_t)(a + sqrdmulh_s16_scalar((int16_t)(b - a), fy15));
    }
    out[-1] = out[0];
    int16_t last = out[sw - 1U];
    for (uint32_t p = 0; p < 8U; ++p) out[sw + p] = last;
#endif
}

/* Fused 256 -> 512 vertical Q13 lerp + horizontal 2x (vhadd/vrhadd 3/4,1/4) hlerp (2x unrolled, 0 spills) */
void hdf_live_vlerp_hlerp_y_512(const uint16_t *r0, const uint16_t *r1,
                                uint16_t *out_512, uint32_t fy_q8) {
#if HDF_NEON
    int16x8_t vfy = vdupq_n_s16((int16_t)(fy_q8 << 7));
    uint16x8x2_t a0 = live_ld1_u16_x2_inc(&r0);
    uint16x8x2_t b0 = live_ld1_u16_x2_inc(&r1);
    int16x8_t a00 = vreinterpretq_s16_u16(a0.val[0]), b00 = vreinterpretq_s16_u16(b0.val[0]);
    int16x8_t a01 = vreinterpretq_s16_u16(a0.val[1]), b01 = vreinterpretq_s16_u16(b0.val[1]);
    uint16x8_t g0 = vreinterpretq_u16_s16(vaddq_s16(a00, vqrdmulhq_s16(vsubq_s16(b00, a00), vfy)));
    uint16x8_t g1 = vreinterpretq_u16_s16(vaddq_s16(a01, vqrdmulhq_s16(vsubq_s16(b01, a01), vfy)));
    uint16x8_t g_prev = vdupq_laneq_u16(g0, 0);

    for (uint32_t n = (HDF_LIVE_GW >> 4) - 1U; n > 0U; --n) {
        uint16x8x2_t an = live_ld1_u16_x2_inc(&r0);
        uint16x8x2_t bn = live_ld1_u16_x2_inc(&r1);
        int16x8_t an0 = vreinterpretq_s16_u16(an.val[0]), bn0 = vreinterpretq_s16_u16(bn.val[0]);
        int16x8_t an1 = vreinterpretq_s16_u16(an.val[1]), bn1 = vreinterpretq_s16_u16(bn.val[1]);
        uint16x8_t g2 = vreinterpretq_u16_s16(vaddq_s16(an0, vqrdmulhq_s16(vsubq_s16(bn0, an0), vfy)));
        uint16x8_t g3 = vreinterpretq_u16_s16(vaddq_s16(an1, vqrdmulhq_s16(vsubq_s16(bn1, an1), vfy)));

        uint16x8_t e0 = vrhaddq_u16(g0, vhaddq_u16(g0, vextq_u16(g_prev, g0, 7)));
        uint16x8_t o0 = vrhaddq_u16(g0, vhaddq_u16(g0, vextq_u16(g0, g1, 1)));
        live_st1_u16_x2_inc(&out_512, vzip1q_u16(e0, o0), vzip2q_u16(e0, o0));

        uint16x8_t e1 = vrhaddq_u16(g1, vhaddq_u16(g1, vextq_u16(g0, g1, 7)));
        uint16x8_t o1 = vrhaddq_u16(g1, vhaddq_u16(g1, vextq_u16(g1, g2, 1)));
        live_st1_u16_x2_inc(&out_512, vzip1q_u16(e1, o1), vzip2q_u16(e1, o1));

        g_prev = g1; g0 = g2; g1 = g3;
    }
    uint16x8_t e0 = vrhaddq_u16(g0, vhaddq_u16(g0, vextq_u16(g_prev, g0, 7)));
    uint16x8_t o0 = vrhaddq_u16(g0, vhaddq_u16(g0, vextq_u16(g0, g1, 1)));
    live_st1_u16_x2_inc(&out_512, vzip1q_u16(e0, o0), vzip2q_u16(e0, o0));

    uint16x8_t g_end = vdupq_laneq_u16(g1, 7);
    uint16x8_t e1 = vrhaddq_u16(g1, vhaddq_u16(g1, vextq_u16(g0, g1, 7)));
    uint16x8_t o1 = vrhaddq_u16(g1, vhaddq_u16(g1, vextq_u16(g1, g_end, 1)));
    live_st1_u16_x2_inc(&out_512, vzip1q_u16(e1, o1), vzip2q_u16(e1, o1));
#else
    uint16_t tmp[HDF_LIVE_GW];
    int16_t fy15 = (int16_t)(fy_q8 << 7);
    for (uint32_t x = 0; x < HDF_LIVE_GW; ++x) {
        int16_t a = (int16_t)r0[x], b = (int16_t)r1[x];
        tmp[x] = (uint16_t)(int16_t)(a + sqrdmulh_s16_scalar((int16_t)(b - a), fy15));
    }
    for (uint32_t k = 0; k < HDF_LIVE_GW; ++k) {
        uint16_t cur = tmp[k];
        uint16_t l   = tmp[k > 0U ? k - 1U : 0U];
        uint16_t r   = tmp[k + 1U < HDF_LIVE_GW ? k + 1U : HDF_LIVE_GW - 1U];
        out_512[2U * k]      = urhadd_u16_s(cur, uhadd_u16_s(cur, l));
        out_512[2U * k + 1U] = urhadd_u16_s(cur, uhadd_u16_s(cur, r));
    }
#endif
}

/* Fused 2x (vhadd/vrhadd 3/4,1/4) + 4x (1-load) hlerp + 3-scale blend + k_q12 fold */
void hdf_live_s2_blend_k_row_256(uint16_t *row0,
                                 const int16_t *vrow1_padded, const int16_t *vrow2_padded,
                                 int16_t w1k, int16_t w2k, int16_t w3k) {
    const int16_t *vr1 = vrow1_padded + 8U;
    const int16_t *vr2 = vrow2_padded + 8U;
#if HDF_NEON
    uint8x16_t p4a0 = hdf_ld1_u8(k_live_p4_a0);
    uint8x16_t p4b0 = hdf_ld1_u8(k_live_p4_b0);
    uint8x16_t p4a1 = hdf_ld1_u8(k_live_p4_a1);
    uint8x16_t p4b1 = hdf_ld1_u8(k_live_p4_b1);
    int16x8_t  vw4  = hdf_ld1_s16(k_live_w4);
    int16x8_t vw1k = vdupq_n_s16(w1k);
    int16x8_t vw2k = vdupq_n_s16(w2k);
    int16x8_t vw3k = vdupq_n_s16(w3k);

    uint16x8_t g_cur  = hdf_ld1_u16((const uint16_t *)vr1);
    uint16x8_t g_prev = vdupq_laneq_u16(g_cur, 0);
    const uint16_t *p1 = (const uint16_t *)(vr1 + 8U);
    const int16_t *p2 = vr2 - 1;
    uint16_t *pr = row0;
    for (uint32_t n = HDF_LIVE_GW >> 4; n > 0U; --n) {
        uint16x8_t g_next = hdf_ld1_u16(p1);
        p1 += 8U;
        uint16x8_t g_left  = vextq_u16(g_prev, g_cur, 7);
        uint16x8_t g_right = vextq_u16(g_cur, g_next, 1);
        uint16x8_t even = vrhaddq_u16(g_cur, vhaddq_u16(g_cur, g_left));
        uint16x8_t odd  = vrhaddq_u16(g_cur, vhaddq_u16(g_cur, g_right));
        int16x8_t s2_0 = vreinterpretq_s16_u16(vzip1q_u16(even, odd));
        int16x8_t s2_1 = vreinterpretq_s16_u16(vzip2q_u16(even, odd));
        g_prev = g_cur;
        g_cur  = g_next;

        uint8x16_t w4_win = vreinterpretq_u8_s16(hdf_ld1_s16(p2));
        p2 += 4;
        int16x8_t a30 = vreinterpretq_s16_u8(vqtbl1q_u8(w4_win, p4a0));
        int16x8_t b30 = vreinterpretq_s16_u8(vqtbl1q_u8(w4_win, p4b0));
        int16x8_t s3_0 = vaddq_s16(a30, vqrdmulhq_s16(vsubq_s16(b30, a30), vw4));
        int16x8_t a31 = vreinterpretq_s16_u8(vqtbl1q_u8(w4_win, p4a1));
        int16x8_t b31 = vreinterpretq_s16_u8(vqtbl1q_u8(w4_win, p4b1));
        int16x8_t s3_1 = vaddq_s16(a31, vqrdmulhq_s16(vsubq_s16(b31, a31), vw4));

        uint16x8x2_t r0v = vld1q_u16_x2(pr);
        int16x8_t m0 = vqaddq_s16(vqaddq_s16(vqrdmulhq_s16(vreinterpretq_s16_u16(r0v.val[0]), vw1k),
                                             vqrdmulhq_s16(s2_0, vw2k)),
                                  vqrdmulhq_s16(s3_0, vw3k));
        int16x8_t m1 = vqaddq_s16(vqaddq_s16(vqrdmulhq_s16(vreinterpretq_s16_u16(r0v.val[1]), vw1k),
                                             vqrdmulhq_s16(s2_1, vw2k)),
                                  vqrdmulhq_s16(s3_1, vw3k));
        live_st1_u16_x2_inc(&pr, vreinterpretq_u16_s16(m0), vreinterpretq_u16_s16(m1));
    }
#else
    static const int16_t w4_tbl[4] = { 20480, 28672, 4096, 12288 };
    for (uint32_t x = 0; x < HDF_LIVE_GW; ++x) {
        int32_t k = (int32_t)(x >> 1);
        uint16_t cur = (uint16_t)vr1[k];
        uint16_t nbr = (uint16_t)vr1[k + ((x & 1U) ? 1 : -1)];
        int16_t s2 = (int16_t)urhadd_u16_s(cur, uhadd_u16_s(cur, nbr));

        int32_t g2 = (int32_t)(x >> 2) - ((x & 2U) ? 0 : 1);
        int16_t a2 = vr2[g2], b2 = vr2[g2 + 1];
        int16_t s3 = (int16_t)(a2 + sqrdmulh_s16_scalar((int16_t)(b2 - a2), w4_tbl[x & 3U]));

        int16_t a = (int16_t)row0[x];
        int16_t mix = sqadd_s16_scalar(
            sqadd_s16_scalar(sqrdmulh_s16_scalar(a, w1k), sqrdmulh_s16_scalar(s2, w2k)),
            sqrdmulh_s16_scalar(s3, w3k));
        row0[x] = (uint16_t)mix;
    }
#endif
}

void hdf_live_build_grid_y(const uint16_t *grid_r, const uint16_t *grid_g,
                           const uint16_t *grid_b, uint16_t *grid_y,
                           uint32_t total_cells) {
#if HDF_NEON
    int16x8_t c77  = vdupq_n_s16(77 << 7);
    int16x8_t c150 = vdupq_n_s16(150 << 7);
    int16x8_t c29  = vdupq_n_s16(29 << 7);
    for (uint32_t n = total_cells >> 4; n > 0U; --n) {
        uint16x8x2_t rv = live_ld1_u16_x2_inc(&grid_r);
        uint16x8x2_t gv = live_ld1_u16_x2_inc(&grid_g);
        uint16x8x2_t bv = live_ld1_u16_x2_inc(&grid_b);
        int16x8_t y0 = vqaddq_s16(
            vqaddq_s16(vqrdmulhq_s16(vreinterpretq_s16_u16(rv.val[0]), c77),
                       vqrdmulhq_s16(vreinterpretq_s16_u16(gv.val[0]), c150)),
            vqrdmulhq_s16(vreinterpretq_s16_u16(bv.val[0]), c29));
        int16x8_t y1 = vqaddq_s16(
            vqaddq_s16(vqrdmulhq_s16(vreinterpretq_s16_u16(rv.val[1]), c77),
                       vqrdmulhq_s16(vreinterpretq_s16_u16(gv.val[1]), c150)),
            vqrdmulhq_s16(vreinterpretq_s16_u16(bv.val[1]), c29));
        live_st1_u16_x2_inc(&grid_y, vreinterpretq_u16_s16(y0), vreinterpretq_u16_s16(y1));
    }
#else
    for (uint32_t i = 0; i < total_cells; ++i) {
        int16_t r = (int16_t)grid_r[i];
        int16_t g = (int16_t)grid_g[i];
        int16_t b = (int16_t)grid_b[i];
        int16_t y = sqadd_s16_scalar(
            sqadd_s16_scalar(sqrdmulh_s16_scalar(r, (int16_t)(77 << 7)),
                             sqrdmulh_s16_scalar(g, (int16_t)(150 << 7))),
            sqrdmulh_s16_scalar(b, (int16_t)(29 << 7)));
        grid_y[i] = (uint16_t)y;
    }
#endif
}

/* Check scatter chromaticity (> 384 Q15) only on chunks flagged by y_edge_mask */
uint32_t hdf_live_s3_check_schroma_mask(const uint16_t *gr, const uint16_t *gg,
                                        const uint16_t *gb, uint32_t y_check_mask) {
    uint32_t out = 0U;
    uint32_t m = y_check_mask;
    while (m != 0U) {
        uint32_t k = (uint32_t)__builtin_ctz(m);
        m &= m - 1U;
        uint32_t off = k << 3;
#if HDF_NEON
        uint16x8_t vr = hdf_ld1_u16(gr + off);
        uint16x8_t vg = hdf_ld1_u16(gg + off);
        uint16x8_t vb = hdf_ld1_u16(gb + off);
        uint16x8_t smax = vmaxq_u16(vr, vmaxq_u16(vg, vb));
        uint16x8_t smin = vminq_u16(vr, vminq_u16(vg, vb));
        if (vmaxvq_u16(vsubq_u16(smax, smin)) > 384U) {
            out |= (1U << k);
        }
#else
        uint32_t max_d = 0U;
        for (uint32_t j = 0; j < 8U; ++j) {
            uint32_t r = gr[off + j], g = gg[off + j], b = gb[off + j];
            uint32_t mx = r > g ? r : g; if (b > mx) mx = b;
            uint32_t mn = r < g ? r : g; if (b < mn) mn = b;
            if (mx - mn > max_d) max_d = mx - mn;
        }
        if (max_d > 384U) {
            out |= (1U << k);
        }
#endif
    }
    return out;
}

/* Stage 3B Direct 256-wide RGB composite on 256x{170,171} grid (4x fewer px, 0 vlerp, 0 Q-spills) */
void hdf_live_s3_comp_chroma_256(const uint8_t *rin, const uint8_t *gin, const uint8_t *bin,
                                 uint8_t *rout, uint8_t *gout, uint8_t *bout,
                                 const uint16_t *gr, const uint16_t *gg, const uint16_t *gb,
                                 uint32_t level) {
#if HDF_NEON
    LiveKnot16Regs k_invk = live_load_knot16_invk(level);
    LiveClzUmullRegs c64  = live_load_clz_umull();
    uint8x16_t c30 = vdupq_n_u8(30U);

    const uint8_t  *srcs[3] = { rin, gin, bin };
    uint8_t        *dsts[3] = { rout, gout, bout };
    const uint16_t *grds[3] = { gr, gg, gb };

    for (uint32_t ch = 0; ch < 3U; ++ch) {
        const uint8_t  *src = srcs[ch];
        uint8_t        *dst = dsts[ch];
        const uint16_t *gp  = grds[ch];
        for (uint32_t n = HDF_LIVE_GW >> 4; n > 0U; --n) {
            uint16x8x2_t sv = live_ld1_u16_x2_inc(&gp);
            uint8x16_t vx = live_ld1_u8_inc(&src);
            uint16x8_t t0, t1;
            live_knot16_srgb16_to_u16(vx, &k_invk, c30, &t0, &t1);
            uint16x8_t u0 = vaddq_u16(t0, sv.val[0]);
            uint16x8_t u1 = vaddq_u16(t1, sv.val[1]);
            if (__builtin_expect(vmaxvq_u16(vmaxq_u16(u0, u1)) > 25600U, 0)) {
                uint16x8x2_t sh = live_shoulder_apply_16px(vx, u0, u1);
                u0 = sh.val[0];
                u1 = sh.val[1];
            }
            live_st1_u8_inc(&dst, live_clz_umull64_lin15_to_srgb(u0, u1, &c64));
        }
    }
#else
    uint32_t idx = (level >= 1U && level <= 3U) ? (level - 1U) : 0U;
    for (uint32_t gx = 0; gx < HDF_LIVE_GW; ++gx) {
        rout[gx] = live_comp_px_scalar(rin[gx], gr[gx], idx);
        gout[gx] = live_comp_px_scalar(gin[gx], gg[gx], idx);
        bout[gx] = live_comp_px_scalar(bin[gx], gb[gx], idx);
    }
#endif
}

/* Fused 256-grid (dCb,dCr) computation + branchless interleaved uqadd/uqsub 2x2 NV12 UV apply */
void hdf_live_s3_apply_chroma_256_2x2(uint8_t *c0, uint8_t *c1,
                                      const uint8_t *rin, const uint8_t *gin, const uint8_t *bin,
                                      const uint8_t *rout, const uint8_t *gout, const uint8_t *bout) {
#if HDF_NEON
    int16x8_t c43  = vdupq_n_s16(43 << 7);
    int16x8_t c85  = vdupq_n_s16(85 << 7);
    int16x8_t c107 = vdupq_n_s16(107 << 7);
    int16x8_t c21  = vdupq_n_s16(21 << 7);

    /* Unconditional 16-cell loop: 6 ld1 -> 2x (dCb,dCr -> pos/neg interleaved NV12) -> uqadd/uqsub */
    for (uint32_t n = HDF_LIVE_GW >> 4; n > 0U; --n) {
        uint8x16_t ri = live_ld1_u8_inc(&rin), ro = live_ld1_u8_inc(&rout);
        uint8x16_t gi = live_ld1_u8_inc(&gin), go = live_ld1_u8_inc(&gout);
        uint8x16_t bi = live_ld1_u8_inc(&bin), bo = live_ld1_u8_inc(&bout);

        /* Low 8 grid cells -> 16 interleaved UV pairs (32 bytes per row) */
        {
            int16x8_t dR = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(ro), vget_low_u8(ri)));
            int16x8_t dG = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(go), vget_low_u8(gi)));
            int16x8_t dB = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(bo), vget_low_u8(bi)));
            int16x8_t dcb = vsubq_s16(vsubq_s16(vrshrq_n_s16(dB, 1), vqrdmulhq_s16(dR, c43)),
                                      vqrdmulhq_s16(dG, c85));
            int16x8_t dcr = vsubq_s16(vsubq_s16(vrshrq_n_s16(dR, 1), vqrdmulhq_s16(dG, c107)),
                                      vqrdmulhq_s16(dB, c21));
            uint8x8_t pcb = vqmovun_s16(dcb), pcr = vqmovun_s16(dcr);
            uint8x8_t ncb = vqmovun_s16(vqnegq_s16(dcb)), ncr = vqmovun_s16(vqnegq_s16(dcr));
            uint16x8_t puv = vreinterpretq_u16_u8(vzip1q_u8(vcombine_u8(pcb, pcb), vcombine_u8(pcr, pcr)));
            uint16x8_t nuv = vreinterpretq_u16_u8(vzip1q_u8(vcombine_u8(ncb, ncb), vcombine_u8(ncr, ncr)));
            uint8x16_t p0 = vreinterpretq_u8_u16(vzip1q_u16(puv, puv));
            uint8x16_t p1 = vreinterpretq_u8_u16(vzip2q_u16(puv, puv));
            uint8x16_t n0 = vreinterpretq_u8_u16(vzip1q_u16(nuv, nuv));
            uint8x16_t n1 = vreinterpretq_u8_u16(vzip2q_u16(nuv, nuv));

            uint8x16x2_t v0 = vld1q_u8_x2(c0);
            uint8x16x2_t o0 = {{ vqsubq_u8(vqaddq_u8(v0.val[0], p0), n0),
                                 vqsubq_u8(vqaddq_u8(v0.val[1], p1), n1) }};
            vst1q_u8_x2(c0, o0);
            if (c1) {
                uint8x16x2_t v1 = vld1q_u8_x2(c1);
                uint8x16x2_t o1 = {{ vqsubq_u8(vqaddq_u8(v1.val[0], p0), n0),
                                     vqsubq_u8(vqaddq_u8(v1.val[1], p1), n1) }};
                vst1q_u8_x2(c1, o1);
            }
        }

        /* High 8 grid cells -> next 16 interleaved UV pairs (32 bytes per row) */
        {
            int16x8_t dR = vreinterpretq_s16_u16(vsubl_high_u8(ro, ri));
            int16x8_t dG = vreinterpretq_s16_u16(vsubl_high_u8(go, gi));
            int16x8_t dB = vreinterpretq_s16_u16(vsubl_high_u8(bo, bi));
            int16x8_t dcb = vsubq_s16(vsubq_s16(vrshrq_n_s16(dB, 1), vqrdmulhq_s16(dR, c43)),
                                      vqrdmulhq_s16(dG, c85));
            int16x8_t dcr = vsubq_s16(vsubq_s16(vrshrq_n_s16(dR, 1), vqrdmulhq_s16(dG, c107)),
                                      vqrdmulhq_s16(dB, c21));
            uint8x8_t pcb = vqmovun_s16(dcb), pcr = vqmovun_s16(dcr);
            uint8x8_t ncb = vqmovun_s16(vqnegq_s16(dcb)), ncr = vqmovun_s16(vqnegq_s16(dcr));
            uint16x8_t puv = vreinterpretq_u16_u8(vzip1q_u8(vcombine_u8(pcb, pcb), vcombine_u8(pcr, pcr)));
            uint16x8_t nuv = vreinterpretq_u16_u8(vzip1q_u8(vcombine_u8(ncb, ncb), vcombine_u8(ncr, ncr)));
            uint8x16_t p0 = vreinterpretq_u8_u16(vzip1q_u16(puv, puv));
            uint8x16_t p1 = vreinterpretq_u8_u16(vzip2q_u16(puv, puv));
            uint8x16_t n0 = vreinterpretq_u8_u16(vzip1q_u16(nuv, nuv));
            uint8x16_t n1 = vreinterpretq_u8_u16(vzip2q_u16(nuv, nuv));

            uint8x16x2_t v0 = vld1q_u8_x2(c0 + 32U);
            uint8x16x2_t o0 = {{ vqsubq_u8(vqaddq_u8(v0.val[0], p0), n0),
                                 vqsubq_u8(vqaddq_u8(v0.val[1], p1), n1) }};
            vst1q_u8_x2(c0 + 32U, o0);
            if (c1) {
                uint8x16x2_t v1 = vld1q_u8_x2(c1 + 32U);
                uint8x16x2_t o1 = {{ vqsubq_u8(vqaddq_u8(v1.val[0], p0), n0),
                                     vqsubq_u8(vqaddq_u8(v1.val[1], p1), n1) }};
                vst1q_u8_x2(c1 + 32U, o1);
            }
        }
        c0 += 64U;
        if (c1) c1 += 64U;
    }
#else
    for (uint32_t gx = 0; gx < HDF_LIVE_GW; ++gx) {
        int16_t dR = (int16_t)((int32_t)rout[gx] - (int32_t)rin[gx]);
        int16_t dG = (int16_t)((int32_t)gout[gx] - (int32_t)gin[gx]);
        int16_t dB = (int16_t)((int32_t)bout[gx] - (int32_t)bin[gx]);
        int32_t dcb = srshr1_s16_scalar(dB)
                    - sqrdmulh_s16_scalar(dR, (int16_t)(43 << 7))
                    - sqrdmulh_s16_scalar(dG, (int16_t)(85 << 7));
        int32_t dcr = srshr1_s16_scalar(dR)
                    - sqrdmulh_s16_scalar(dG, (int16_t)(107 << 7))
                    - sqrdmulh_s16_scalar(dB, (int16_t)(21 << 7));
        uint32_t cx0 = gx << 1;
        for (uint32_t dx = 0; dx < 2U; ++dx) {
            uint32_t off = (cx0 + dx) << 1;
            c0[off]      = clamp_u8((int)c0[off]      + (int)dcb);
            c0[off + 1U] = clamp_u8((int)c0[off + 1U] + (int)dcr);
            if (c1) {
                c1[off]      = clamp_u8((int)c1[off]      + (int)dcb);
                c1[off + 1U] = clamp_u8((int)c1[off + 1U] + (int)dcr);
            }
        }
    }
#endif
}

/* Cold-path exact 512x341 chroma composite ONLY for masked edge chunks (3-phase, 0 Q-spills).
 * If exact_uv_out != NULL, writes updated 32B strips to exact_uv_out[i*64+{0,32}] without
 * modifying c0/c1 yet (patched after branchless apply_chroma_256_2x2).
 */
void hdf_live_s3_chroma_exact_masked(const uint8_t *y0, const uint8_t *y1, uint8_t *c0,
                                     const uint8_t *y2, const uint8_t *y3, uint8_t *c1,
                                     const uint16_t *gr, const uint16_t *gg, const uint16_t *gb,
                                     uint32_t mask, uint32_t level,
                                     uint8_t *fb_buf, uint8_t *exact_uv_out) {
    uint8_t chunks[32];
    uint32_t cnt = 0U;
    uint32_t m = mask;
    while (m != 0U) {
        chunks[cnt++] = (uint8_t)__builtin_ctz(m);
        m &= m - 1U;
    }
    if (cnt == 0U) return;

#if HDF_NEON
    uint8_t *in_r  = fb_buf + 0U * 1024U;
    uint8_t *in_g  = fb_buf + 1U * 1024U;
    uint8_t *in_b  = fb_buf + 2U * 1024U;
    uint8_t *out_r = fb_buf + 3U * 1024U;
    uint8_t *out_g = fb_buf + 4U * 1024U;
    uint8_t *out_b = fb_buf + 5U * 1024U;

    /* Phase 1: YCbCr -> RGB on masked 16-chroma-px chunks (row0 at i*32, row1 at i*32+16) */
    {
        uint8x8_t  c128_8  = vdup_n_u8(128U);
        uint8x16_t c128_16 = vdupq_n_u8(128U);
        int16x8_t c103 = vdupq_n_s16(103 << 7);
        int16x8_t c198 = vdupq_n_s16(198 << 7);
        int16x8_t c88  = vdupq_n_s16(88 << 7);
        int16x8_t c183 = vdupq_n_s16(183 << 7);
        for (uint32_t i = 0; i < cnt; ++i) {
            uint32_t boff = (uint32_t)chunks[i] << 5;
            uint32_t dst_off = i << 5;
            const uint8_t *py0 = y0 + boff, *py1 = y1 + boff, *pc0 = c0 + boff;
            LiveYccRgb16 a = live_ycc_to_rgb16_inc(&py0, &py1, &pc0, c128_8, c128_16,
                                                   c103, c198, c88, c183);
            hdf_st1_u8(in_r + dst_off, a.vr);
            hdf_st1_u8(in_g + dst_off, a.vg);
            hdf_st1_u8(in_b + dst_off, a.vb);
            if (c1) {
                const uint8_t *py2 = y2 + boff, *py3 = y3 + boff, *pc1 = c1 + boff;
                LiveYccRgb16 b = live_ycc_to_rgb16_inc(&py2, &py3, &pc1, c128_8, c128_16,
                                                       c103, c198, c88, c183);
                hdf_st1_u8(in_r + dst_off + 16U, b.vr);
                hdf_st1_u8(in_g + dst_off + 16U, b.vg);
                hdf_st1_u8(in_b + dst_off + 16U, b.vb);
            }
        }
    }

    /* Phase 2: 3-channel composite on packed masked chunks (loads LUTs once, 0 Q-spills) */
    {
        LiveKnot16Regs k_invk = live_load_knot16_invk(level);
        LiveClzUmullRegs c64  = live_load_clz_umull();
        uint8x16_t c30 = vdupq_n_u8(30U);
        const uint8_t  *srcs[3] = { in_r, in_g, in_b };
        uint8_t        *dsts[3] = { out_r, out_g, out_b };
        const uint16_t *grds[3] = { gr, gg, gb };

        for (uint32_t ch = 0; ch < 3U; ++ch) {
            const uint8_t  *src_base = srcs[ch];
            uint8_t        *dst_base = dsts[ch];
            const uint16_t *grd_base = grds[ch];
            for (uint32_t i = 0; i < cnt; ++i) {
                uint32_t goff = (uint32_t)chunks[i] << 3;
                uint32_t poff = i << 5;
                uint16x8_t sv = hdf_ld1_u16(grd_base + goff);
                uint16x8_t s0 = vzip1q_u16(sv, sv);
                uint16x8_t s1 = vzip2q_u16(sv, sv);

                uint8x16_t vx0 = hdf_ld1_u8(src_base + poff);
                uint16x8_t t0, t1;
                live_knot16_srgb16_to_u16(vx0, &k_invk, c30, &t0, &t1);
                uint16x8_t u0 = vaddq_u16(t0, s0);
                uint16x8_t u1 = vaddq_u16(t1, s1);
                if (__builtin_expect(vmaxvq_u16(vmaxq_u16(u0, u1)) > 25600U, 0)) {
                    uint16x8x2_t sh = live_shoulder_apply_16px(vx0, u0, u1);
                    u0 = sh.val[0]; u1 = sh.val[1];
                }
                hdf_st1_u8(dst_base + poff, live_clz_umull64_lin15_to_srgb(u0, u1, &c64));

                if (c1) {
                    uint8x16_t vx1 = hdf_ld1_u8(src_base + poff + 16U);
                    live_knot16_srgb16_to_u16(vx1, &k_invk, c30, &t0, &t1);
                    u0 = vaddq_u16(t0, s0);
                    u1 = vaddq_u16(t1, s1);
                    if (__builtin_expect(vmaxvq_u16(vmaxq_u16(u0, u1)) > 25600U, 0)) {
                        uint16x8x2_t sh = live_shoulder_apply_16px(vx1, u0, u1);
                        u0 = sh.val[0]; u1 = sh.val[1];
                    }
                    hdf_st1_u8(dst_base + poff + 16U, live_clz_umull64_lin15_to_srgb(u0, u1, &c64));
                }
            }
        }
    }

    /* Phase 3: RGB delta -> CbCr update on masked chunks */
    {
        int16x8_t c43  = vdupq_n_s16(43 << 7);
        int16x8_t c85  = vdupq_n_s16(85 << 7);
        int16x8_t c107 = vdupq_n_s16(107 << 7);
        int16x8_t c21  = vdupq_n_s16(21 << 7);
        for (uint32_t i = 0; i < cnt; ++i) {
            uint32_t boff = (uint32_t)chunks[i] << 5;
            uint32_t poff = i << 5;
            uint32_t n_rows = c1 ? 2U : 1U;
            for (uint32_t row = 0; row < n_rows; ++row) {
                uint32_t r_off = poff + (row << 4);
                const uint8_t *csrc = (row == 0U ? c0 : c1) + boff;
                uint8_t *cdst = exact_uv_out ? (exact_uv_out + (size_t)i * 64U + (size_t)row * 32U)
                                             : ((row == 0U ? c0 : c1) + boff);
                uint8x16_t ri = hdf_ld1_u8(in_r + r_off), ro = hdf_ld1_u8(out_r + r_off);
                uint8x16_t gi = hdf_ld1_u8(in_g + r_off), go = hdf_ld1_u8(out_g + r_off);
                uint8x16_t bi = hdf_ld1_u8(in_b + r_off), bo = hdf_ld1_u8(out_b + r_off);
                int16x8_t dR0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(ro), vget_low_u8(ri)));
                int16x8_t dR1 = vreinterpretq_s16_u16(vsubl_high_u8(ro, ri));
                int16x8_t dG0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(go), vget_low_u8(gi)));
                int16x8_t dG1 = vreinterpretq_s16_u16(vsubl_high_u8(go, gi));
                int16x8_t dB0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(bo), vget_low_u8(bi)));
                int16x8_t dB1 = vreinterpretq_s16_u16(vsubl_high_u8(bo, bi));

                int16x8_t dcb0 = vsubq_s16(vsubq_s16(vrshrq_n_s16(dB0, 1), vqrdmulhq_s16(dR0, c43)),
                                           vqrdmulhq_s16(dG0, c85));
                int16x8_t dcb1 = vsubq_s16(vsubq_s16(vrshrq_n_s16(dB1, 1), vqrdmulhq_s16(dR1, c43)),
                                           vqrdmulhq_s16(dG1, c85));
                int16x8_t dcr0 = vsubq_s16(vsubq_s16(vrshrq_n_s16(dR0, 1), vqrdmulhq_s16(dG0, c107)),
                                           vqrdmulhq_s16(dB0, c21));
                int16x8_t dcr1 = vsubq_s16(vsubq_s16(vrshrq_n_s16(dR1, 1), vqrdmulhq_s16(dG1, c107)),
                                           vqrdmulhq_s16(dB1, c21));
                uint8x16x2_t vc = vld2q_u8(csrc);
                uint8x16x2_t co;
                co.val[0] = vcombine_u8(
                    vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dcb0), vget_low_u8(vc.val[0])))),
                    vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dcb1), vget_high_u8(vc.val[0])))));
                co.val[1] = vcombine_u8(
                    vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dcr0), vget_low_u8(vc.val[1])))),
                    vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dcr1), vget_high_u8(vc.val[1])))));
                vst2q_u8(cdst, co);
            }
        }
    }
#else
    (void)fb_buf;
    uint32_t idx = (level >= 1U && level <= 3U) ? (level - 1U) : 0U;
    uint32_t n_rows = c1 ? 2U : 1U;
    for (uint32_t i = 0; i < cnt; ++i) {
        uint32_t cx_base = (uint32_t)chunks[i] << 4;
        for (uint32_t row = 0; row < n_rows; ++row) {
            const uint8_t *yr0 = (row == 0U) ? y0 : y2;
            const uint8_t *yr1 = (row == 0U) ? y1 : y3;
            uint8_t *cr = (row == 0U) ? c0 : c1;
            uint8_t *cdst = exact_uv_out ? (exact_uv_out + (size_t)i * 64U + (size_t)row * 32U) : NULL;
            for (uint32_t d = 0; d < 16U; ++d) {
                uint32_t cx = cx_base + d;
                uint32_t gx = cx >> 1;
                uint8_t yh = uhadd_u8_s(urhadd_u8_s(yr0[2U * cx], yr0[2U * cx + 1U]),
                                        urhadd_u8_s(yr1[2U * cx], yr1[2U * cx + 1U]));
                int16_t cb  = (int16_t)((int32_t)cr[2U * cx] - 128);
                int16_t crv = (int16_t)((int32_t)cr[2U * cx + 1U] - 128);
                int32_t dr = (int32_t)crv + sqrdmulh_s16_scalar(crv, (int16_t)(103 << 7));
                int32_t db = (int32_t)cb  + sqrdmulh_s16_scalar(cb,  (int16_t)(198 << 7));
                int32_t dg = -(sqrdmulh_s16_scalar(cb, (int16_t)(88 << 7)) +
                               sqrdmulh_s16_scalar(crv, (int16_t)(183 << 7)));
                uint8_t ri = clamp_u8((int)yh + (int)dr);
                uint8_t gi = clamp_u8((int)yh + (int)dg);
                uint8_t bi = clamp_u8((int)yh + (int)db);
                uint8_t ro = live_comp_px_scalar(ri, gr[gx], idx);
                uint8_t go = live_comp_px_scalar(gi, gg[gx], idx);
                uint8_t bo = live_comp_px_scalar(bi, gb[gx], idx);
                int16_t dR = (int16_t)((int32_t)ro - (int32_t)ri);
                int16_t dG = (int16_t)((int32_t)go - (int32_t)gi);
                int16_t dB = (int16_t)((int32_t)bo - (int32_t)bi);
                int32_t dcb = srshr1_s16_scalar(dB)
                            - sqrdmulh_s16_scalar(dR, (int16_t)(43 << 7))
                            - sqrdmulh_s16_scalar(dG, (int16_t)(85 << 7));
                int32_t dcr = srshr1_s16_scalar(dR)
                            - sqrdmulh_s16_scalar(dG, (int16_t)(107 << 7))
                            - sqrdmulh_s16_scalar(dB, (int16_t)(21 << 7));
                uint8_t out_cb = clamp_u8((int)cr[2U * cx]      + (int)dcb);
                uint8_t out_cr = clamp_u8((int)cr[2U * cx + 1U] + (int)dcr);
                if (cdst) {
                    cdst[2U * d]      = out_cb;
                    cdst[2U * d + 1U] = out_cr;
                } else {
                    cr[2U * cx]      = out_cb;
                    cr[2U * cx + 1U] = out_cr;
                }
            }
        }
    }
#endif
}

void hdf_live_s3_patch_exact_uv(uint8_t *c0, uint8_t *c1,
                                const uint8_t *exact_uv, uint32_t mask) {
    uint32_t i = 0U;
    uint32_t m = mask;
    while (m != 0U) {
        uint32_t k = (uint32_t)__builtin_ctz(m);
        m &= m - 1U;
        uint32_t boff = k << 5;
        const uint8_t *src = exact_uv + (size_t)i * 64U;
#if HDF_NEON
        vst1q_u8_x2(c0 + boff, vld1q_u8_x2(src));
        if (c1) {
            vst1q_u8_x2(c1 + boff, vld1q_u8_x2(src + 32U));
        }
#else
        for (uint32_t b = 0; b < 32U; ++b) c0[boff + b] = src[b];
        if (c1) {
            for (uint32_t b = 0; b < 32U; ++b) c1[boff + b] = src[32U + b];
        }
#endif
        ++i;
    }
}

/* Fused 2-row width-wide Y composite reading pre-hlerped s_y_half (width multiple of 16): 0 bl, 0 Q-spills */
void hdf_live_s3_comp_y_rowpair_w(uint8_t *yr0, uint8_t *yr1,
                                  const uint16_t *s_y_half, uint32_t width, uint32_t level) {
#if HDF_NEON
    LiveKnot16Regs k_invk = live_load_knot16_invk(level);
    LiveClzUmullRegs c64  = live_load_clz_umull();
    uint8x16_t c30 = vdupq_n_u8(30U);

    for (uint32_t n = width >> 4; n > 0U; --n) {
        uint16x8_t sp = live_ld1_u16_inc(&s_y_half);
        uint16x8_t s0 = vzip1q_u16(sp, sp);
        uint16x8_t s1 = vzip2q_u16(sp, sp);

        uint8x16_t vx0 = hdf_ld1_u8(yr0);
        uint16x8_t t0, t1;
        live_knot16_srgb16_to_u16(vx0, &k_invk, c30, &t0, &t1);
        uint16x8_t u0 = vaddq_u16(t0, s0);
        uint16x8_t u1 = vaddq_u16(t1, s1);
        if (__builtin_expect(vmaxvq_u16(vmaxq_u16(u0, u1)) > 25600U, 0)) {
            uint16x8x2_t sh = live_shoulder_apply_16px(vx0, u0, u1);
            u0 = sh.val[0]; u1 = sh.val[1];
        }
        live_st1_u8_inc(&yr0, live_clz_umull64_lin15_to_srgb(u0, u1, &c64));

        uint8x16_t vx1 = hdf_ld1_u8(yr1);
        live_knot16_srgb16_to_u16(vx1, &k_invk, c30, &t0, &t1);
        u0 = vaddq_u16(t0, s0);
        u1 = vaddq_u16(t1, s1);
        if (__builtin_expect(vmaxvq_u16(vmaxq_u16(u0, u1)) > 25600U, 0)) {
            uint16x8x2_t sh = live_shoulder_apply_16px(vx1, u0, u1);
            u0 = sh.val[0]; u1 = sh.val[1];
        }
        live_st1_u8_inc(&yr1, live_clz_umull64_lin15_to_srgb(u0, u1, &c64));
    }
#else
    uint32_t idx = (level >= 1U && level <= 3U) ? (level - 1U) : 0U;
    for (uint32_t x = 0; x < width; x += 2U) {
        uint16_t sk = s_y_half[x >> 1];
        yr0[x]      = live_comp_px_scalar(yr0[x],      sk, idx);
        yr0[x + 1U] = live_comp_px_scalar(yr0[x + 1U], sk, idx);
        yr1[x]      = live_comp_px_scalar(yr1[x],      sk, idx);
        yr1[x + 1U] = live_comp_px_scalar(yr1[x + 1U], sk, idx);
    }
#endif
}

void hdf_live_s3_comp_y_rowpair(uint8_t *yr0, uint8_t *yr1,
                                const uint16_t *s_y_512, uint32_t level) {
    hdf_live_s3_comp_y_rowpair_w(yr0, yr1, s_y_512, HDF_LIVE_W, level);
}

/* ---- 9b. V71.7-r4 EVF Fast Optical HDF Kernels (1280x{850..856} NV12) --- */

#define HDF_EVF_W  1280U
#define HDF_EVF_CW  640U

#if HDF_NEON
static inline __attribute__((always_inline)) uint8x8_t live_evf_reduce_5to2_u8(
    uint8x16_t v0, uint8x16_t v1, uint8x8_t ta, uint8x8_t tb, uint8x8_t tc) {
    uint8x16x2_t tbl = {{ v0, v1 }};
    uint8x8_t a = vqtbl2_u8(tbl, ta);
    uint8x8_t b = vqtbl2_u8(tbl, tb);
    uint8x8_t c = vqtbl2_u8(tbl, tc);
    return vrhadd_u8(b, vrhadd_u8(a, vhadd_u8(a, c)));
}

static inline __attribute__((always_inline)) uint16x8_t live_evf_reduce_5to2_u16(
    const uint16_t *e20, uint8x16_t ta, uint8x16_t tb, uint8x16_t tc) {
    uint8x16x3_t tbl = {{
        vreinterpretq_u8_u16(hdf_ld1_u16(e20)),
        vreinterpretq_u8_u16(hdf_ld1_u16(e20 + 8U)),
        vreinterpretq_u8_u16(hdf_ld1_u16(e20 + 12U))
    }};
    uint16x8_t a = vreinterpretq_u16_u8(vqtbl3q_u8(tbl, ta));
    uint16x8_t b = vreinterpretq_u16_u8(vqtbl3q_u8(tbl, tb));
    uint16x8_t c = vreinterpretq_u16_u8(vqtbl3q_u8(tbl, tc));
    return vrhaddq_u16(b, vrhaddq_u16(a, vhaddq_u16(a, c)));
}

static inline __attribute__((always_inline)) void live_evf_ycc16_to_rgb16(
    uint8x16_t ys, uint8x16_t cb_u, uint8x16_t cr_u,
    uint8x8_t c128_8, uint8x16_t c128_16,
    int16x8_t c103, int16x8_t c198, int16x8_t c88, int16x8_t c183,
    uint8x16_t *r_out, uint8x16_t *g_out, uint8x16_t *b_out) {
    int16x8_t cb0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(cb_u), c128_8));
    int16x8_t cb1 = vreinterpretq_s16_u16(vsubl_high_u8(cb_u, c128_16));
    int16x8_t cr0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(cr_u), c128_8));
    int16x8_t cr1 = vreinterpretq_s16_u16(vsubl_high_u8(cr_u, c128_16));

    int16x8_t dr0 = vaddq_s16(cr0, vqrdmulhq_s16(cr0, c103));
    int16x8_t dr1 = vaddq_s16(cr1, vqrdmulhq_s16(cr1, c103));
    int16x8_t db0 = vaddq_s16(cb0, vqrdmulhq_s16(cb0, c198));
    int16x8_t db1 = vaddq_s16(cb1, vqrdmulhq_s16(cb1, c198));
    int16x8_t dg0 = vnegq_s16(vaddq_s16(vqrdmulhq_s16(cb0, c88), vqrdmulhq_s16(cr0, c183)));
    int16x8_t dg1 = vnegq_s16(vaddq_s16(vqrdmulhq_s16(cb1, c88), vqrdmulhq_s16(cr1, c183)));

    uint8x8_t ys_lo = vget_low_u8(ys), ys_hi = vget_high_u8(ys);
    *r_out = vcombine_u8(
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dr0), ys_lo))),
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dr1), ys_hi))));
    *g_out = vcombine_u8(
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dg0), ys_lo))),
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dg1), ys_hi))));
    *b_out = vcombine_u8(
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(db0), ys_lo))),
        vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(db1), ys_hi))));
}

/* Cold-path EVF monochrome highlight boost (20 cols x n_crows -> 8 grid cells, 0 Q-spills) */
static __attribute__((noinline)) void live_evf_s1_mono_hi_deferred(
    const uint8_t *evf_y640, uint32_t n_crows,
    const uint8_t *hi_chunks, uint32_t n_hi,
    const uint8_t *boost_lo, const uint8_t *boost_hi,
    uint16_t *gr_out) {
    LiveKnot16Regs klin = live_load_knot16_lin();
    uint8x16_t c30 = vdupq_n_u8(30U);
    uint8x16x4_t bt_lo = vld1q_u8_x4(boost_lo);
    uint8x16x4_t bt_hi = vld1q_u8_x4(boost_hi);
    uint16x8_t c32767 = vdupq_n_u16(32767U);
    uint16_t _Alignas(16) e20[24];

    for (uint32_t i = 0U; i < n_hi; ++i) {
        uint32_t chunk = hi_chunks[i];
        uint32_t cx0 = chunk * 20U;
        uint32_t goff = chunk << 3;
        uint16x8_t eg0 = vdupq_n_u16(0U), eg1 = eg0, eg2 = eg0;
        for (uint32_t r = 0U; r < n_crows; ++r) {
            const uint8_t *yp = evf_y640 + (size_t)r * HDF_EVF_CW + cx0;
            uint8x16_t ys0 = hdf_ld1_u8(yp);
            uint16x8_t lr0, lr1;
            live_knot16_srgb16_to_u16(ys0, &klin, c30, &lr0, &lr1);
            uint8x16_t bidx = vshrq_n_u8(
                vuzp2q_u8(vreinterpretq_u8_u16(lr0), vreinterpretq_u8_u16(lr1)), 1);
            uint8x16_t blo = vqtbl4q_u8(bt_lo, bidx);
            uint8x16_t bhi = vqtbl4q_u8(bt_hi, bidx);
            int16x8_t b0_h = vreinterpretq_s16_u8(vzip1q_u8(blo, bhi));
            int16x8_t b1_h = vreinterpretq_s16_u8(vzip2q_u8(blo, bhi));
            hdf_st1_u16(e20, vminq_u16(vaddq_u16(vrshrq_n_u16(lr0, 2),
                vreinterpretq_u16_s16(vqrdmulhq_s16(b0_h, vreinterpretq_s16_u16(lr0)))), c32767));
            hdf_st1_u16(e20 + 8U, vminq_u16(vaddq_u16(vrshrq_n_u16(lr1, 2),
                vreinterpretq_u16_s16(vqrdmulhq_s16(b1_h, vreinterpretq_s16_u16(lr1)))), c32767));

            uint8x16_t ys1 = hdf_ld1_u8(yp + 4U);
            live_knot16_srgb16_to_u16(ys1, &klin, c30, &lr0, &lr1);
            bidx = vshrq_n_u8(
                vuzp2q_u8(vreinterpretq_u8_u16(lr0), vreinterpretq_u8_u16(lr1)), 1);
            blo = vqtbl4q_u8(bt_lo, bidx);
            bhi = vqtbl4q_u8(bt_hi, bidx);
            b1_h = vreinterpretq_s16_u8(vzip2q_u8(blo, bhi));
            hdf_st1_u16(e20 + 12U, vminq_u16(vaddq_u16(vrshrq_n_u16(lr1, 2),
                vreinterpretq_u16_s16(vqrdmulhq_s16(b1_h, vreinterpretq_s16_u16(lr1)))), c32767));

            uint16x8_t egr = live_evf_reduce_5to2_u16(e20,
                                                      hdf_ld1_u8(k_evf_r52_u16_a),
                                                      hdf_ld1_u8(k_evf_r52_u16_b),
                                                      hdf_ld1_u8(k_evf_r52_u16_c));
            if (r == 0U) {
                eg0 = egr;
            } else if (r == 1U) {
                eg1 = egr;
            } else {
                eg2 = egr;
            }
        }
        uint16x8_t g_fin = (n_crows == 2U)
                               ? vrhaddq_u16(eg0, eg1)
                               : vrhaddq_u16(eg1, vhaddq_u16(eg0, eg2));
        hdf_st1_u16(gr_out + goff, g_fin);
    }
}

/* Cold-path EVF color hi-chunk Pass 1: YCbCr -> RGB20 + r256/g256/b256 (0 Q-spills) */
static __attribute__((noinline)) void live_evf_s1_hi_ycc_batch(
    const uint8_t *evf_y640, const uint8_t *evf_cb640, const uint8_t *evf_cr640,
    uint32_t n_crows, const uint8_t *chunks, uint32_t bcnt,
    uint8_t *r256_out, uint8_t *g256_out, uint8_t *b256_out,
    uint8_t *batch_rgb) {
    uint8x8_t  c128_8  = vdup_n_u8(128U);
    uint8x16_t c128_16 = vdupq_n_u8(128U);
    int16x8_t c103 = vdupq_n_s16(103 << 7);
    int16x8_t c198 = vdupq_n_s16(198 << 7);
    int16x8_t c88  = vdupq_n_s16(88 << 7);
    int16x8_t c183 = vdupq_n_s16(183 << 7);
    uint8x8_t ta = live_ld1_u8_8(k_evf_r52_u8_a);
    uint8x8_t tb = live_ld1_u8_8(k_evf_r52_u8_b);
    uint8x8_t tc = live_ld1_u8_8(k_evf_r52_u8_c);

    for (uint32_t i = 0U; i < bcnt; ++i) {
        uint32_t chunk = chunks[i];
        uint32_t cx0 = chunk * 20U;
        uint32_t goff = chunk << 3;
        uint8x8_t r_rows[3], g_rows[3], b_rows[3];
        for (uint32_t r = 0U; r < n_crows; ++r) {
            size_t roff = (size_t)r * HDF_EVF_CW + cx0;
            uint8_t *dst = batch_rgb + (size_t)i * 192U + (size_t)r * 64U;
            uint8x16_t r0, g0, b0, r1, g1, b1;
            live_evf_ycc16_to_rgb16(hdf_ld1_u8(evf_y640 + roff),
                                    hdf_ld1_u8(evf_cb640 + roff),
                                    hdf_ld1_u8(evf_cr640 + roff),
                                    c128_8, c128_16, c103, c198, c88, c183,
                                    &r0, &g0, &b0);
            live_evf_ycc16_to_rgb16(hdf_ld1_u8(evf_y640 + roff + 4U),
                                    hdf_ld1_u8(evf_cb640 + roff + 4U),
                                    hdf_ld1_u8(evf_cr640 + roff + 4U),
                                    c128_8, c128_16, c103, c198, c88, c183,
                                    &r1, &g1, &b1);
            hdf_st1_u8(dst +  0U, r0); hdf_st1_u8(dst +  4U, r1);
            hdf_st1_u8(dst + 20U, g0); hdf_st1_u8(dst + 24U, g1);
            hdf_st1_u8(dst + 40U, b0); hdf_st1_u8(dst + 44U, b1);
            r_rows[r] = live_evf_reduce_5to2_u8(r0, r1, ta, tb, tc);
            g_rows[r] = live_evf_reduce_5to2_u8(g0, g1, ta, tb, tc);
            b_rows[r] = live_evf_reduce_5to2_u8(b0, b1, ta, tb, tc);
        }
        uint8x8_t r8, g8, b8;
        if (n_crows == 2U) {
            r8 = vrhadd_u8(r_rows[0], r_rows[1]);
            g8 = vrhadd_u8(g_rows[0], g_rows[1]);
            b8 = vrhadd_u8(b_rows[0], b_rows[1]);
        } else {
            r8 = vrhadd_u8(r_rows[1], vhadd_u8(r_rows[0], r_rows[2]));
            g8 = vrhadd_u8(g_rows[1], vhadd_u8(g_rows[0], g_rows[2]));
            b8 = vrhadd_u8(b_rows[1], vhadd_u8(b_rows[0], b_rows[2]));
        }
        live_st1_u8_8(r256_out + goff, r8);
        live_st1_u8_8(g256_out + goff, g8);
        live_st1_u8_8(b256_out + goff, b8);
    }
}

static __attribute__((noinline)) void live_evf_s1_hi_boost_one_row(
    const uint8_t *rb, const uint8_t *boost_lo, const uint8_t *boost_hi,
    uint16_t *er20, uint16_t *eg20, uint16_t *eb20) {
    LiveKnot16Regs klin = live_load_knot16_lin();
    uint8x8_t c30_8 = vdup_n_u8(30U);
    uint8x16x4_t bt_lo = vld1q_u8_x4(boost_lo);
    uint8x16x4_t bt_hi = vld1q_u8_x4(boost_hi);
    uint16x8_t c32767 = vdupq_n_u16(32767U);
    int16x8_t c77  = vdupq_n_s16(77 << 7);
    int16x8_t c150 = vdupq_n_s16(150 << 7);
    int16x8_t c29  = vdupq_n_s16(29 << 7);
    int16x8_t c160 = vdupq_n_s16(160 << 7);
    int16x8_t c96  = vdupq_n_s16(96 << 7);
    static const uint8_t offs[3] = { 0U, 8U, 12U };

    for (uint32_t k = 0U; k < 3U; ++k) {
        uint32_t o = offs[k];
        uint16x8_t er, eg, eb;
        live_s1_boost_8px_core(live_ld1_u8_8(rb +  0U + o),
                               live_ld1_u8_8(rb + 20U + o),
                               live_ld1_u8_8(rb + 40U + o),
                               &klin, c30_8, &bt_lo, &bt_hi,
                               c32767, c77, c150, c29, c160, c96,
                               &er, &eg, &eb);
        hdf_st1_u16(er20 + o, er);
        hdf_st1_u16(eg20 + o, eg);
        hdf_st1_u16(eb20 + o, eb);
    }
}

/* Cold-path EVF color hi-chunk Pass 2: per-pixel boost -> 5->2 + v-avg -> gr/gg/gb (0 Q-spills) */
static __attribute__((noinline)) void live_evf_s1_hi_boost_batch(
    const uint8_t *batch_rgb, uint32_t n_crows,
    const uint8_t *chunks, uint32_t bcnt,
    const uint8_t *boost_lo, const uint8_t *boost_hi,
    uint16_t *gr_out, uint16_t *gg_out, uint16_t *gb_out) {
    uint16_t _Alignas(16) er20[20], eg20[20], eb20[20];
    uint16_t _Alignas(16) gr_r[3][8], gg_r[3][8], gb_r[3][8];

    for (uint32_t i = 0U; i < bcnt; ++i) {
        uint32_t goff = (uint32_t)chunks[i] << 3;
        for (uint32_t r = 0U; r < n_crows; ++r) {
            const uint8_t *rb = batch_rgb + (size_t)i * 192U + (size_t)r * 64U;
            live_evf_s1_hi_boost_one_row(rb, boost_lo, boost_hi, er20, eg20, eb20);
            uint8x16_t ta = hdf_ld1_u8(k_evf_r52_u16_a);
            uint8x16_t tb = hdf_ld1_u8(k_evf_r52_u16_b);
            uint8x16_t tc = hdf_ld1_u8(k_evf_r52_u16_c);
            hdf_st1_u16(gr_r[r], live_evf_reduce_5to2_u16(er20, ta, tb, tc));
            hdf_st1_u16(gg_r[r], live_evf_reduce_5to2_u16(eg20, ta, tb, tc));
            hdf_st1_u16(gb_r[r], live_evf_reduce_5to2_u16(eb20, ta, tb, tc));
        }
        if (n_crows == 2U) {
            hdf_st1_u16(gr_out + goff, vrhaddq_u16(hdf_ld1_u16(gr_r[0]), hdf_ld1_u16(gr_r[1])));
            hdf_st1_u16(gg_out + goff, vrhaddq_u16(hdf_ld1_u16(gg_r[0]), hdf_ld1_u16(gg_r[1])));
            hdf_st1_u16(gb_out + goff, vrhaddq_u16(hdf_ld1_u16(gb_r[0]), hdf_ld1_u16(gb_r[1])));
        } else {
            hdf_st1_u16(gr_out + goff, vrhaddq_u16(hdf_ld1_u16(gr_r[1]),
                vhaddq_u16(hdf_ld1_u16(gr_r[0]), hdf_ld1_u16(gr_r[2]))));
            hdf_st1_u16(gg_out + goff, vrhaddq_u16(hdf_ld1_u16(gg_r[1]),
                vhaddq_u16(hdf_ld1_u16(gg_r[0]), hdf_ld1_u16(gg_r[2]))));
            hdf_st1_u16(gb_out + goff, vrhaddq_u16(hdf_ld1_u16(gb_r[1]),
                vhaddq_u16(hdf_ld1_u16(gb_r[0]), hdf_ld1_u16(gb_r[2]))));
        }
    }
}
#else /* !HDF_NEON */
static inline void evf_reduce_5to2_u8_scalar(const uint8_t *src20, uint8_t out8[8]) {
    for (uint32_t k = 0U; k < 4U; ++k) {
        uint32_t p = 5U * k;
        out8[2U * k]      = urhadd_u8_s(src20[p + 1U], urhadd_u8_s(src20[p + 0U], uhadd_u8_s(src20[p + 0U], src20[p + 2U])));
        out8[2U * k + 1U] = urhadd_u8_s(src20[p + 3U], urhadd_u8_s(src20[p + 4U], uhadd_u8_s(src20[p + 4U], src20[p + 2U])));
    }
}

static inline void evf_reduce_5to2_u16_scalar(const uint16_t *e20, uint16_t out8[8]) {
    for (uint32_t k = 0U; k < 4U; ++k) {
        uint32_t p = 5U * k;
        out8[2U * k]      = urhadd_u16_s(e20[p + 1U], urhadd_u16_s(e20[p + 0U], uhadd_u16_s(e20[p + 0U], e20[p + 2U])));
        out8[2U * k + 1U] = urhadd_u16_s(e20[p + 3U], urhadd_u16_s(e20[p + 4U], uhadd_u16_s(e20[p + 4U], e20[p + 2U])));
    }
}
#endif /* HDF_NEON */

void hdf_live_evf_s1_rowgroup_256(const uint8_t *const *y_rows,
                                  const uint8_t *const *c_rows,
                                  uint32_t n_crows,
                                  const uint8_t *boost_lo, const uint8_t *boost_hi,
                                  uint16_t *gr_out, uint16_t *gg_out, uint16_t *gb_out,
                                  uint8_t *r256_out, uint8_t *g256_out, uint8_t *b256_out,
                                  uint32_t *c_edge_mask_out, uint32_t *y_edge_mask_out,
                                  uint8_t *boost_scratch,
                                  uint8_t *evf_y640, uint8_t *evf_cb640, uint8_t *evf_cr640,
                                  uint32_t monochrome) {
#if HDF_NEON
    /* Unpack 1280 Y+CbCr rows -> contiguous 640-wide chroma-grid planes (L1 resident) */
    for (uint32_t r = 0U; r < n_crows; ++r) {
        const uint8_t *yr0 = y_rows[2U * r];
        const uint8_t *yr1 = y_rows[2U * r + 1U];
        uint8_t *ydst = evf_y640 + (size_t)r * HDF_EVF_CW;
        if (monochrome) {
            for (uint32_t n = HDF_EVF_CW >> 4; n > 0U; --n) {
                uint8x16x2_t a0 = vld2q_u8(yr0); yr0 += 32U;
                uint8x16x2_t a1 = vld2q_u8(yr1); yr1 += 32U;
                uint8x16_t ys = vhaddq_u8(vrhaddq_u8(a0.val[0], a0.val[1]),
                                          vrhaddq_u8(a1.val[0], a1.val[1]));
                live_st1_u8_inc(&ydst, ys);
            }
        } else {
            const uint8_t *cr = c_rows[r];
            uint8_t *cbdst = evf_cb640 + (size_t)r * HDF_EVF_CW;
            uint8_t *crdst = evf_cr640 + (size_t)r * HDF_EVF_CW;
            for (uint32_t n = HDF_EVF_CW >> 4; n > 0U; --n) {
                uint8x16x2_t a0 = vld2q_u8(yr0); yr0 += 32U;
                uint8x16x2_t a1 = vld2q_u8(yr1); yr1 += 32U;
                uint8x16_t ys = vhaddq_u8(vrhaddq_u8(a0.val[0], a0.val[1]),
                                          vrhaddq_u8(a1.val[0], a1.val[1]));
                uint8x16x2_t vc = vld2q_u8(cr); cr += 32U;
                live_st1_u8_inc(&ydst, ys);
                live_st1_u8_inc(&cbdst, vc.val[0]);
                live_st1_u8_inc(&crdst, vc.val[1]);
            }
        }
    }

    LiveKnot16Regs kq = live_load_knot16_q13b();
    uint8x8_t c15_8 = vdup_n_u8(15U);
    uint8x8_t ta = live_ld1_u8_8(k_evf_r52_u8_a);
    uint8x8_t tb = live_ld1_u8_8(k_evf_r52_u8_b);
    uint8x8_t tc = live_ld1_u8_8(k_evf_r52_u8_c);
    uint8_t hi_chunks[32];
    uint32_t n_hi = 0U;

    if (monochrome) {
        *c_edge_mask_out = 0U;
        *y_edge_mask_out = 0U;
        for (uint32_t chunk = 0U; chunk < 32U; ++chunk) {
            uint32_t cx0 = chunk * 20U;
            uint8x16_t y0a = hdf_ld1_u8(evf_y640 + cx0);
            uint8x16_t y0b = hdf_ld1_u8(evf_y640 + cx0 + 4U);
            uint8x16_t y1a = hdf_ld1_u8(evf_y640 + HDF_EVF_CW + cx0);
            uint8x16_t y1b = hdf_ld1_u8(evf_y640 + HDF_EVF_CW + cx0 + 4U);
            uint8x16_t ymax = vmaxq_u8(vmaxq_u8(y0a, y0b), vmaxq_u8(y1a, y1b));
            uint8x16_t yva, yvb;
            if (n_crows == 2U) {
                yva = vrhaddq_u8(y0a, y1a);
                yvb = vrhaddq_u8(y0b, y1b);
            } else {
                uint8x16_t y2a = hdf_ld1_u8(evf_y640 + 2U * HDF_EVF_CW + cx0);
                uint8x16_t y2b = hdf_ld1_u8(evf_y640 + 2U * HDF_EVF_CW + cx0 + 4U);
                ymax = vmaxq_u8(ymax, vmaxq_u8(y2a, y2b));
                yva = vrhaddq_u8(y1a, vhaddq_u8(y0a, y2a));
                yvb = vrhaddq_u8(y1b, vhaddq_u8(y0b, y2b));
            }
            if (__builtin_expect(vmaxvq_u8(ymax) <= 158U, 1)) {
                uint8x8_t y8 = live_evf_reduce_5to2_u8(yva, yvb, ta, tb, tc);
                hdf_st1_u16(gr_out + (chunk << 3), live_knot16_q13b_8px(y8, &kq, c15_8));
            } else {
                hi_chunks[n_hi++] = (uint8_t)chunk;
            }
        }
        if (n_hi > 0U) {
            live_evf_s1_mono_hi_deferred(evf_y640, n_crows, hi_chunks, n_hi,
                                         boost_lo, boost_hi, gr_out);
        }
        return;
    }

    uint8x8_t  c128_8  = vdup_n_u8(128U);
    uint8x16_t c128_16 = vdupq_n_u8(128U);
    int16x8_t c103 = vdupq_n_s16(103 << 7);
    int16x8_t c198 = vdupq_n_s16(198 << 7);
    int16x8_t c88  = vdupq_n_s16(88 << 7);
    int16x8_t c183 = vdupq_n_s16(183 << 7);

    uint8_t b8_chunks[32];
    uint32_t n_b8 = 0U;
    uint32_t c_mask = 0U, y_mask = 0U;

    for (uint32_t chunk = 0U; chunk < 32U; ++chunk) {
        uint32_t cx0 = chunk * 20U;
        uint32_t goff = chunk << 3;

        /* Vertical min/max & average across n_crows (2 or 3) */
        uint8x16_t y0a = hdf_ld1_u8(evf_y640 + cx0),             y0b = hdf_ld1_u8(evf_y640 + cx0 + 4U);
        uint8x16_t y1a = hdf_ld1_u8(evf_y640 + HDF_EVF_CW + cx0), y1b = hdf_ld1_u8(evf_y640 + HDF_EVF_CW + cx0 + 4U);
        uint8x16_t ym0 = vmaxq_u8(y0a, y1a), ym1 = vmaxq_u8(y0b, y1b);
        uint8x16_t yn0 = vminq_u8(y0a, y1a), yn1 = vminq_u8(y0b, y1b);
        uint8x16_t yva, yvb;
        if (n_crows == 2U) {
            yva = vrhaddq_u8(y0a, y1a);
            yvb = vrhaddq_u8(y0b, y1b);
        } else {
            uint8x16_t y2a = hdf_ld1_u8(evf_y640 + 2U * HDF_EVF_CW + cx0);
            uint8x16_t y2b = hdf_ld1_u8(evf_y640 + 2U * HDF_EVF_CW + cx0 + 4U);
            ym0 = vmaxq_u8(ym0, y2a); ym1 = vmaxq_u8(ym1, y2b);
            yn0 = vminq_u8(yn0, y2a); yn1 = vminq_u8(yn1, y2b);
            yva = vrhaddq_u8(y1a, vhaddq_u8(y0a, y2a));
            yvb = vrhaddq_u8(y1b, vhaddq_u8(y0b, y2b));
        }
        uint8x16_t yq_max = vpmaxq_u8(vpmaxq_u8(ym0, ym1), vpmaxq_u8(ym0, ym1));
        uint8x16_t yq_min = vpminq_u8(vpminq_u8(yn0, yn1), vpminq_u8(yn0, yn1));
        uint32_t y_max_all = vmaxvq_u8(yq_max);
        uint32_t y_span    = vmaxvq_u8(vsubq_u8(yq_max, yq_min));

        uint8x16_t cb0a = hdf_ld1_u8(evf_cb640 + cx0),             cb0b = hdf_ld1_u8(evf_cb640 + cx0 + 4U);
        uint8x16_t cb1a = hdf_ld1_u8(evf_cb640 + HDF_EVF_CW + cx0), cb1b = hdf_ld1_u8(evf_cb640 + HDF_EVF_CW + cx0 + 4U);
        uint8x16_t cbm0 = vmaxq_u8(cb0a, cb1a), cbm1 = vmaxq_u8(cb0b, cb1b);
        uint8x16_t cbn0 = vminq_u8(cb0a, cb1a), cbn1 = vminq_u8(cb0b, cb1b);
        uint8x16_t cbva, cbvb;
        if (n_crows == 2U) {
            cbva = vrhaddq_u8(cb0a, cb1a);
            cbvb = vrhaddq_u8(cb0b, cb1b);
        } else {
            uint8x16_t cb2a = hdf_ld1_u8(evf_cb640 + 2U * HDF_EVF_CW + cx0);
            uint8x16_t cb2b = hdf_ld1_u8(evf_cb640 + 2U * HDF_EVF_CW + cx0 + 4U);
            cbm0 = vmaxq_u8(cbm0, cb2a); cbm1 = vmaxq_u8(cbm1, cb2b);
            cbn0 = vminq_u8(cbn0, cb2a); cbn1 = vminq_u8(cbn1, cb2b);
            cbva = vrhaddq_u8(cb1a, vhaddq_u8(cb0a, cb2a));
            cbvb = vrhaddq_u8(cb1b, vhaddq_u8(cb0b, cb2b));
        }

        uint8x16_t cr0a = hdf_ld1_u8(evf_cr640 + cx0),             cr0b = hdf_ld1_u8(evf_cr640 + cx0 + 4U);
        uint8x16_t cr1a = hdf_ld1_u8(evf_cr640 + HDF_EVF_CW + cx0), cr1b = hdf_ld1_u8(evf_cr640 + HDF_EVF_CW + cx0 + 4U);
        uint8x16_t crm0 = vmaxq_u8(cr0a, cr1a), crm1 = vmaxq_u8(cr0b, cr1b);
        uint8x16_t crn0 = vminq_u8(cr0a, cr1a), crn1 = vminq_u8(cr0b, cr1b);
        uint8x16_t crva, crvb;
        if (n_crows == 2U) {
            crva = vrhaddq_u8(cr0a, cr1a);
            crvb = vrhaddq_u8(cr0b, cr1b);
        } else {
            uint8x16_t cr2a = hdf_ld1_u8(evf_cr640 + 2U * HDF_EVF_CW + cx0);
            uint8x16_t cr2b = hdf_ld1_u8(evf_cr640 + 2U * HDF_EVF_CW + cx0 + 4U);
            crm0 = vmaxq_u8(crm0, cr2a); crm1 = vmaxq_u8(crm1, cr2b);
            crn0 = vminq_u8(crn0, cr2a); crn1 = vminq_u8(crn1, cr2b);
            crva = vrhaddq_u8(cr1a, vhaddq_u8(cr0a, cr2a));
            crvb = vrhaddq_u8(cr1b, vhaddq_u8(cr0b, cr2b));
        }

        uint8x16_t cq_max = vpmaxq_u8(vpmaxq_u8(cbm0, cbm1), vpmaxq_u8(crm0, crm1));
        uint8x16_t cq_min = vpminq_u8(vpminq_u8(cbn0, cbn1), vpminq_u8(crn0, crn1));
        uint32_t c_span   = vmaxvq_u8(vsubq_u8(cq_max, cq_min));
        uint32_t sat_max  = vmaxvq_u8(vmaxq_u8(vabdq_u8(cq_max, c128_16),
                                               vabdq_u8(cq_min, c128_16)));

        if (y_span > 48U) {
            y_mask |= (1U << chunk);
        }
        if (c_span > 16U || (y_span > 48U && sat_max > 16U)) {
            c_mask |= (1U << chunk);
        }

        if (__builtin_expect((y_max_all + 2U * sat_max <= 158U) ||
                             (y_span <= 40U && c_span <= 16U), 1)) {
            uint8x8_t y256  = live_evf_reduce_5to2_u8(yva,  yvb,  ta, tb, tc);
            uint8x8_t cb256 = live_evf_reduce_5to2_u8(cbva, cbvb, ta, tb, tc);
            uint8x8_t cr256 = live_evf_reduce_5to2_u8(crva, crvb, ta, tb, tc);
            int16x8_t cbs = vreinterpretq_s16_u16(vsubl_u8(cb256, c128_8));
            int16x8_t crs = vreinterpretq_s16_u16(vsubl_u8(cr256, c128_8));
            int16x8_t dr  = vaddq_s16(crs, vqrdmulhq_s16(crs, c103));
            int16x8_t db  = vaddq_s16(cbs, vqrdmulhq_s16(cbs, c198));
            int16x8_t dg  = vnegq_s16(vaddq_s16(vqrdmulhq_s16(cbs, c88), vqrdmulhq_s16(crs, c183)));
            uint8x8_t r8 = vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dr), y256)));
            uint8x8_t g8 = vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dg), y256)));
            uint8x8_t b8 = vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(db), y256)));
            live_st1_u8_8(r256_out + goff, r8);
            live_st1_u8_8(g256_out + goff, g8);
            live_st1_u8_8(b256_out + goff, b8);
            if (__builtin_expect(vmaxv_u8(vmax_u8(r8, vmax_u8(g8, b8))) <= 158U, 1)) {
                hdf_st1_u16(gr_out + goff, live_knot16_q13b_8px(r8, &kq, c15_8));
                hdf_st1_u16(gg_out + goff, live_knot16_q13b_8px(g8, &kq, c15_8));
                hdf_st1_u16(gb_out + goff, live_knot16_q13b_8px(b8, &kq, c15_8));
            } else {
                uint8_t *d = boost_scratch + (size_t)n_b8 * 24U;
                live_st1_u8_8(d +  0U, r8);
                live_st1_u8_8(d +  8U, g8);
                live_st1_u8_8(d + 16U, b8);
                b8_chunks[n_b8++] = (uint8_t)chunk;
            }
        } else {
            hi_chunks[n_hi++] = (uint8_t)chunk;
        }
    }

    *c_edge_mask_out = c_mask;
    *y_edge_mask_out = y_mask;
    if (n_b8 > 0U) {
        live_s1_boost_8px_deferred(boost_scratch, b8_chunks, n_b8,
                                   boost_lo, boost_hi, gr_out, gg_out, gb_out);
    }
    for (uint32_t off = 0U; off < n_hi; off += 16U) {
        uint32_t bcnt = n_hi - off;
        if (bcnt > 16U) bcnt = 16U;
        live_evf_s1_hi_ycc_batch(evf_y640, evf_cb640, evf_cr640, n_crows,
                                 hi_chunks + off, bcnt,
                                 r256_out, g256_out, b256_out, boost_scratch);
        live_evf_s1_hi_boost_batch(boost_scratch, n_crows,
                                   hi_chunks + off, bcnt,
                                   boost_lo, boost_hi, gr_out, gg_out, gb_out);
    }
#else
    (void)boost_scratch;
    for (uint32_t r = 0U; r < n_crows; ++r) {
        const uint8_t *yr0 = y_rows[2U * r];
        const uint8_t *yr1 = y_rows[2U * r + 1U];
        uint8_t *ydst = evf_y640 + (size_t)r * HDF_EVF_CW;
        uint8_t *cbdst = evf_cb640 + (size_t)r * HDF_EVF_CW;
        uint8_t *crdst = evf_cr640 + (size_t)r * HDF_EVF_CW;
        for (uint32_t cx = 0U; cx < HDF_EVF_CW; ++cx) {
            ydst[cx] = uhadd_u8_s(urhadd_u8_s(yr0[2U * cx], yr0[2U * cx + 1U]),
                                  urhadd_u8_s(yr1[2U * cx], yr1[2U * cx + 1U]));
            if (!monochrome) {
                cbdst[cx] = c_rows[r][2U * cx];
                crdst[cx] = c_rows[r][2U * cx + 1U];
            }
        }
    }

    if (monochrome) {
        *c_edge_mask_out = 0U;
        *y_edge_mask_out = 0U;
        for (uint32_t chunk = 0U; chunk < 32U; ++chunk) {
            uint32_t cx0 = chunk * 20U;
            uint32_t goff = chunk << 3;
            uint32_t ymax = 0U;
            for (uint32_t r = 0U; r < n_crows; ++r) {
                const uint8_t *yp = evf_y640 + (size_t)r * HDF_EVF_CW + cx0;
                for (uint32_t j = 0U; j < 20U; ++j) {
                    if (yp[j] > ymax) ymax = yp[j];
                }
            }
            if (ymax <= 158U) {
                uint8_t yv[20], y8[8];
                for (uint32_t j = 0U; j < 20U; ++j) {
                    uint8_t a = evf_y640[cx0 + j];
                    uint8_t b = evf_y640[HDF_EVF_CW + cx0 + j];
                    yv[j] = (n_crows == 2U)
                                ? urhadd_u8_s(a, b)
                                : urhadd_u8_s(b, uhadd_u8_s(a, evf_y640[2U * HDF_EVF_CW + cx0 + j]));
                }
                evf_reduce_5to2_u8_scalar(yv, y8);
                for (uint32_t j = 0U; j < 8U; ++j) {
                    gr_out[goff + j] = (uint16_t)((live_knot16_eval_scalar(y8[j],
                        k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp) + 2U) >> 2);
                }
            } else {
                uint16_t eg[3][8];
                for (uint32_t r = 0U; r < n_crows; ++r) {
                    uint16_t e20[20];
                    const uint8_t *yp = evf_y640 + (size_t)r * HDF_EVF_CW + cx0;
                    for (uint32_t j = 0U; j < 20U; ++j) {
                        uint16_t lr = live_knot16_eval_scalar(yp[j], k_live_s2l16_lin_blo,
                                                              k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
                        uint32_t bidx = (uint32_t)lr >> 9;
                        int16_t bh = (int16_t)((uint16_t)boost_lo[bidx] | ((uint16_t)boost_hi[bidx] << 8));
                        uint32_t v = (((uint32_t)lr + 2U) >> 2) + (uint16_t)sqrdmulh_s16_scalar(bh, (int16_t)lr);
                        e20[j] = (uint16_t)(v > 32767U ? 32767U : v);
                    }
                    evf_reduce_5to2_u16_scalar(e20, eg[r]);
                }
                for (uint32_t j = 0U; j < 8U; ++j) {
                    gr_out[goff + j] = (n_crows == 2U)
                                           ? urhadd_u16_s(eg[0][j], eg[1][j])
                                           : urhadd_u16_s(eg[1][j], uhadd_u16_s(eg[0][j], eg[2][j]));
                }
            }
        }
        return;
    }

    uint32_t c_mask = 0U, y_mask = 0U;
    for (uint32_t chunk = 0U; chunk < 32U; ++chunk) {
        uint32_t cx0 = chunk * 20U;
        uint32_t goff = chunk << 3;
        uint32_t y_span_max = 0U, c_span_max = 0U, sat_max = 0U, y_max_all = 0U;
        for (uint32_t q = 0U; q < 5U; ++q) {
            uint32_t ymin = 255U, ymax = 0U, cbmin = 255U, cbmax = 0U, crmin = 255U, crmax = 0U;
            for (uint32_t r = 0U; r < n_crows; ++r) {
                size_t base = (size_t)r * HDF_EVF_CW + cx0 + 4U * q;
                for (uint32_t d = 0U; d < 4U; ++d) {
                    uint8_t yv = evf_y640[base + d];
                    uint8_t cbv = evf_cb640[base + d];
                    uint8_t crv = evf_cr640[base + d];
                    if (yv < ymin) ymin = yv; if (yv > ymax) ymax = yv;
                    if (cbv < cbmin) cbmin = cbv; if (cbv > cbmax) cbmax = cbv;
                    if (crv < crmin) crmin = crv; if (crv > crmax) crmax = crv;
                }
            }
            if (ymax > y_max_all) y_max_all = ymax;
            if (ymax - ymin > y_span_max) y_span_max = ymax - ymin;
            uint32_t cs = (cbmax - cbmin) > (crmax - crmin) ? (cbmax - cbmin) : (crmax - crmin);
            if (cs > c_span_max) c_span_max = cs;
            uint32_t s0 = cbmax >= 128U ? cbmax - 128U : 128U - cbmax;
            uint32_t s1 = cbmin >= 128U ? cbmin - 128U : 128U - cbmin;
            uint32_t s2 = crmax >= 128U ? crmax - 128U : 128U - crmax;
            uint32_t s3 = crmin >= 128U ? crmin - 128U : 128U - crmin;
            if (s0 > sat_max) sat_max = s0; if (s1 > sat_max) sat_max = s1;
            if (s2 > sat_max) sat_max = s2; if (s3 > sat_max) sat_max = s3;
        }
        if (y_span_max > 48U) y_mask |= (1U << chunk);
        if (c_span_max > 16U || (y_span_max > 48U && sat_max > 16U)) c_mask |= (1U << chunk);

        if ((y_max_all + 2U * sat_max <= 158U) || (y_span_max <= 40U && c_span_max <= 16U)) {
            uint8_t yv[20], cbv[20], crv[20], y8[8], cb8[8], cr8[8];
            for (uint32_t j = 0U; j < 20U; ++j) {
                uint8_t ya = evf_y640[cx0 + j],  yb = evf_y640[HDF_EVF_CW + cx0 + j];
                uint8_t ca = evf_cb640[cx0 + j], cb = evf_cb640[HDF_EVF_CW + cx0 + j];
                uint8_t ra = evf_cr640[cx0 + j], rb = evf_cr640[HDF_EVF_CW + cx0 + j];
                if (n_crows == 2U) {
                    yv[j] = urhadd_u8_s(ya, yb);
                    cbv[j] = urhadd_u8_s(ca, cb);
                    crv[j] = urhadd_u8_s(ra, rb);
                } else {
                    yv[j]  = urhadd_u8_s(yb, uhadd_u8_s(ya, evf_y640[2U * HDF_EVF_CW + cx0 + j]));
                    cbv[j] = urhadd_u8_s(cb, uhadd_u8_s(ca, evf_cb640[2U * HDF_EVF_CW + cx0 + j]));
                    crv[j] = urhadd_u8_s(rb, uhadd_u8_s(ra, evf_cr640[2U * HDF_EVF_CW + cx0 + j]));
                }
            }
            evf_reduce_5to2_u8_scalar(yv, y8);
            evf_reduce_5to2_u8_scalar(cbv, cb8);
            evf_reduce_5to2_u8_scalar(crv, cr8);
            uint8_t r8_a[8], g8_a[8], b8_a[8];
            uint32_t max_rgb = 0U;
            for (uint32_t j = 0U; j < 8U; ++j) {
                int16_t cb  = (int16_t)((int32_t)cb8[j] - 128);
                int16_t cr  = (int16_t)((int32_t)cr8[j] - 128);
                int32_t dr  = (int32_t)cr + sqrdmulh_s16_scalar(cr, (int16_t)(103 << 7));
                int32_t db  = (int32_t)cb + sqrdmulh_s16_scalar(cb, (int16_t)(198 << 7));
                int32_t dg  = -(sqrdmulh_s16_scalar(cb, (int16_t)(88 << 7)) +
                                sqrdmulh_s16_scalar(cr, (int16_t)(183 << 7)));
                uint8_t r8 = clamp_u8((int)y8[j] + (int)dr);
                uint8_t g8 = clamp_u8((int)y8[j] + (int)dg);
                uint8_t b8 = clamp_u8((int)y8[j] + (int)db);
                r8_a[j] = r8; g8_a[j] = g8; b8_a[j] = b8;
                r256_out[goff + j] = r8; g256_out[goff + j] = g8; b256_out[goff + j] = b8;
                if (r8 > max_rgb) max_rgb = r8;
                if (g8 > max_rgb) max_rgb = g8;
                if (b8 > max_rgb) max_rgb = b8;
            }
            if (max_rgb <= 158U) {
                for (uint32_t j = 0U; j < 8U; ++j) {
                    gr_out[goff + j] = (uint16_t)((live_knot16_eval_scalar(r8_a[j], k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp) + 2U) >> 2);
                    gg_out[goff + j] = (uint16_t)((live_knot16_eval_scalar(g8_a[j], k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp) + 2U) >> 2);
                    gb_out[goff + j] = (uint16_t)((live_knot16_eval_scalar(b8_a[j], k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp) + 2U) >> 2);
                }
            } else {
                for (uint32_t j = 0U; j < 8U; ++j) {
                    uint16_t lr = live_knot16_eval_scalar(r8_a[j], k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
                    uint16_t lg = live_knot16_eval_scalar(g8_a[j], k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
                    uint16_t lb = live_knot16_eval_scalar(b8_a[j], k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
                    uint16_t lmax = lr > lg ? lr : lg;
                    if (lb > lmax) lmax = lb;
                    uint16_t llum = (uint16_t)sqadd_s16_scalar(
                        sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lr, (int16_t)(77 << 7)),
                                         sqrdmulh_s16_scalar((int16_t)lg, (int16_t)(150 << 7))),
                        sqrdmulh_s16_scalar((int16_t)lb, (int16_t)(29 << 7)));
                    uint16_t ldrv = (uint16_t)(lmax - (lmax >> 3));
                    if (llum > ldrv) ldrv = llum;
                    uint32_t bidx = (uint32_t)ldrv >> 9;
                    int16_t bh = (int16_t)((uint16_t)boost_lo[bidx] | ((uint16_t)boost_hi[bidx] << 8));
                    int16_t nt = sqrdmulh_s16_scalar((int16_t)ldrv, (int16_t)(96 << 7));
                    uint32_t er = (((uint32_t)lr + 2U) >> 2) + (uint32_t)(uint16_t)sqrdmulh_s16_scalar(
                        bh, sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lr, (int16_t)(160 << 7)), nt));
                    uint32_t eg = (((uint32_t)lg + 2U) >> 2) + (uint32_t)(uint16_t)sqrdmulh_s16_scalar(
                        bh, sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lg, (int16_t)(160 << 7)), nt));
                    uint32_t eb = (((uint32_t)lb + 2U) >> 2) + (uint32_t)(uint16_t)sqrdmulh_s16_scalar(
                        bh, sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lb, (int16_t)(160 << 7)), nt));
                    gr_out[goff + j] = (uint16_t)(er > 32767U ? 32767U : er);
                    gg_out[goff + j] = (uint16_t)(eg > 32767U ? 32767U : eg);
                    gb_out[goff + j] = (uint16_t)(eb > 32767U ? 32767U : eb);
                }
            }
        } else {
            uint8_t  r_rows[3][8], g_rows[3][8], b_rows[3][8];
            uint16_t gr_r[3][8], gg_r[3][8], gb_r[3][8];
            for (uint32_t r = 0U; r < n_crows; ++r) {
                uint8_t  r20[20], g20[20], b20[20];
                uint16_t er20[20], eg20[20], eb20[20];
                size_t roff = (size_t)r * HDF_EVF_CW + cx0;
                for (uint32_t j = 0U; j < 20U; ++j) {
                    uint8_t ys = evf_y640[roff + j];
                    int16_t cb = (int16_t)((int32_t)evf_cb640[roff + j] - 128);
                    int16_t cr = (int16_t)((int32_t)evf_cr640[roff + j] - 128);
                    int32_t dr = (int32_t)cr + sqrdmulh_s16_scalar(cr, (int16_t)(103 << 7));
                    int32_t db = (int32_t)cb + sqrdmulh_s16_scalar(cb, (int16_t)(198 << 7));
                    int32_t dg = -(sqrdmulh_s16_scalar(cb, (int16_t)(88 << 7)) +
                                   sqrdmulh_s16_scalar(cr, (int16_t)(183 << 7)));
                    uint8_t vr = clamp_u8((int)ys + (int)dr);
                    uint8_t vg = clamp_u8((int)ys + (int)dg);
                    uint8_t vb = clamp_u8((int)ys + (int)db);
                    r20[j] = vr; g20[j] = vg; b20[j] = vb;
                    uint16_t lr = live_knot16_eval_scalar(vr, k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
                    uint16_t lg = live_knot16_eval_scalar(vg, k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
                    uint16_t lb = live_knot16_eval_scalar(vb, k_live_s2l16_lin_blo, k_live_s2l16_lin_bhi, k_live_s2l16_lin_slp);
                    uint16_t lmax = lr > lg ? lr : lg;
                    if (lb > lmax) lmax = lb;
                    uint16_t llum = (uint16_t)sqadd_s16_scalar(
                        sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lr, (int16_t)(77 << 7)),
                                         sqrdmulh_s16_scalar((int16_t)lg, (int16_t)(150 << 7))),
                        sqrdmulh_s16_scalar((int16_t)lb, (int16_t)(29 << 7)));
                    uint16_t ldrv = (uint16_t)(lmax - (lmax >> 3));
                    if (llum > ldrv) ldrv = llum;
                    uint32_t bidx = (uint32_t)ldrv >> 9;
                    int16_t bh = (int16_t)((uint16_t)boost_lo[bidx] | ((uint16_t)boost_hi[bidx] << 8));
                    int16_t nt = sqrdmulh_s16_scalar((int16_t)ldrv, (int16_t)(96 << 7));
                    uint32_t er = (((uint32_t)lr + 2U) >> 2) + (uint32_t)(uint16_t)sqrdmulh_s16_scalar(
                        bh, sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lr, (int16_t)(160 << 7)), nt));
                    uint32_t eg = (((uint32_t)lg + 2U) >> 2) + (uint32_t)(uint16_t)sqrdmulh_s16_scalar(
                        bh, sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lg, (int16_t)(160 << 7)), nt));
                    uint32_t eb = (((uint32_t)lb + 2U) >> 2) + (uint32_t)(uint16_t)sqrdmulh_s16_scalar(
                        bh, sqadd_s16_scalar(sqrdmulh_s16_scalar((int16_t)lb, (int16_t)(160 << 7)), nt));
                    er20[j] = (uint16_t)(er > 32767U ? 32767U : er);
                    eg20[j] = (uint16_t)(eg > 32767U ? 32767U : eg);
                    eb20[j] = (uint16_t)(eb > 32767U ? 32767U : eb);
                }
                evf_reduce_5to2_u8_scalar(r20, r_rows[r]);
                evf_reduce_5to2_u8_scalar(g20, g_rows[r]);
                evf_reduce_5to2_u8_scalar(b20, b_rows[r]);
                evf_reduce_5to2_u16_scalar(er20, gr_r[r]);
                evf_reduce_5to2_u16_scalar(eg20, gg_r[r]);
                evf_reduce_5to2_u16_scalar(eb20, gb_r[r]);
            }
            for (uint32_t j = 0U; j < 8U; ++j) {
                if (n_crows == 2U) {
                    r256_out[goff + j] = urhadd_u8_s(r_rows[0][j], r_rows[1][j]);
                    g256_out[goff + j] = urhadd_u8_s(g_rows[0][j], g_rows[1][j]);
                    b256_out[goff + j] = urhadd_u8_s(b_rows[0][j], b_rows[1][j]);
                    gr_out[goff + j]   = urhadd_u16_s(gr_r[0][j], gr_r[1][j]);
                    gg_out[goff + j]   = urhadd_u16_s(gg_r[0][j], gg_r[1][j]);
                    gb_out[goff + j]   = urhadd_u16_s(gb_r[0][j], gb_r[1][j]);
                } else {
                    r256_out[goff + j] = urhadd_u8_s(r_rows[1][j], uhadd_u8_s(r_rows[0][j], r_rows[2][j]));
                    g256_out[goff + j] = urhadd_u8_s(g_rows[1][j], uhadd_u8_s(g_rows[0][j], g_rows[2][j]));
                    b256_out[goff + j] = urhadd_u8_s(b_rows[1][j], uhadd_u8_s(b_rows[0][j], b_rows[2][j]));
                    gr_out[goff + j]   = urhadd_u16_s(gr_r[1][j], uhadd_u16_s(gr_r[0][j], gr_r[2][j]));
                    gg_out[goff + j]   = urhadd_u16_s(gg_r[1][j], uhadd_u16_s(gg_r[0][j], gg_r[2][j]));
                    gb_out[goff + j]   = urhadd_u16_s(gb_r[1][j], uhadd_u16_s(gb_r[0][j], gb_r[2][j]));
                }
            }
        }
    }
    *c_edge_mask_out = c_mask;
    *y_edge_mask_out = y_mask;
#endif
}

/* Exact 2.5x pixel-center horizontal bilinear upsample 256 -> 640 (0 Q-spills, 0-diff) */
void hdf_live_evf_hlerp_256_to_640(const int16_t *vrow_padded, uint16_t *out_640) {
    const int16_t *vr = vrow_padded + 8U;
#if HDF_NEON
    int16x8_t w179 = vdupq_n_s16((int16_t)(179 << 7));
    int16x8_t w26  = vdupq_n_s16((int16_t)( 26 << 7));
    int16x8_t w230 = vdupq_n_s16((int16_t)(230 << 7));
    int16x8_t w77  = vdupq_n_s16((int16_t)( 77 << 7));
    uint8x16_t p0  = hdf_ld1_u8(k_evf_hl_p0);
    uint8x16_t p1  = hdf_ld1_u8(k_evf_hl_p1);
    uint8x8_t  p2  = live_ld1_u8_8(k_evf_hl_p2);

    int16x8x2_t cur = vld2q_s16(vr);
    int16x8_t prev_odd = vdupq_n_s16(vr[-1]);
    const int16_t *p_in = vr + 16U;
    uint16_t *p_out = out_640;

    for (uint32_t iter = 0U; iter < 16U; ++iter) {
        int16x8x2_t nxt;
        int16x8_t next_even;
        if (iter < 15U) {
            nxt = vld2q_s16(p_in);
            p_in += 16U;
            next_even = nxt.val[0];
        } else {
            nxt = cur;
            next_even = vdupq_n_s16(vr[256]);
        }

        int16x8_t even = cur.val[0];
        int16x8_t odd  = cur.val[1];
        int16x8_t vm1  = vextq_s16(prev_odd, odd, 7);
        int16x8_t vp2  = vextq_s16(even, next_even, 1);

        int16x8_t d_m = vsubq_s16(even, vm1);
        int16x8_t d_c = vsubq_s16(odd,  even);
        int16x8_t d_p = vsubq_s16(vp2,  odd);

        uint16x8_t o0 = vreinterpretq_u16_s16(vaddq_s16(vm1,  vqrdmulhq_s16(d_m, w179)));
        uint16x8_t o1 = vreinterpretq_u16_s16(vaddq_s16(even, vqrdmulhq_s16(d_c, w26)));
        uint16x8_t o2 = vrhaddq_u16(vreinterpretq_u16_s16(even), vreinterpretq_u16_s16(odd));
        uint16x8_t o3 = vreinterpretq_u16_s16(vaddq_s16(even, vqrdmulhq_s16(d_c, w230)));
        uint16x8_t o4 = vreinterpretq_u16_s16(vaddq_s16(odd,  vqrdmulhq_s16(d_p, w77)));

        uint8x16x3_t lo_tbl = {{
            vreinterpretq_u8_u16(vcombine_u16(vget_low_u16(o0), vget_low_u16(o1))),
            vreinterpretq_u8_u16(vcombine_u16(vget_low_u16(o2), vget_low_u16(o3))),
            vreinterpretq_u8_u16(vcombine_u16(vget_low_u16(o4), vget_low_u16(o4)))
        }};
        hdf_st1_u16(p_out +  0U, vreinterpretq_u16_u8(vqtbl3q_u8(lo_tbl, p0)));
        hdf_st1_u16(p_out +  8U, vreinterpretq_u16_u8(vqtbl3q_u8(lo_tbl, p1)));
        live_st1_u16_4(p_out + 16U, vreinterpret_u16_u8(vqtbl3_u8(lo_tbl, p2)));

        uint8x16x3_t hi_tbl = {{
            vreinterpretq_u8_u16(vcombine_u16(vget_high_u16(o0), vget_high_u16(o1))),
            vreinterpretq_u8_u16(vcombine_u16(vget_high_u16(o2), vget_high_u16(o3))),
            vreinterpretq_u8_u16(vcombine_u16(vget_high_u16(o4), vget_high_u16(o4)))
        }};
        hdf_st1_u16(p_out + 20U, vreinterpretq_u16_u8(vqtbl3q_u8(hi_tbl, p0)));
        hdf_st1_u16(p_out + 28U, vreinterpretq_u16_u8(vqtbl3q_u8(hi_tbl, p1)));
        live_st1_u16_4(p_out + 36U, vreinterpret_u16_u8(vqtbl3_u8(hi_tbl, p2)));

        p_out += 40U;
        prev_odd = odd;
        cur = nxt;
    }
#else
    for (uint32_t b = 0U; b < 128U; ++b) {
        int16_t vm1  = vr[(int32_t)(2U * b) - 1];
        int16_t even = vr[2U * b];
        int16_t odd  = vr[2U * b + 1U];
        int16_t vp2  = vr[2U * b + 2U];
        uint32_t x = 5U * b;
        out_640[x + 0U] = (uint16_t)(int16_t)(vm1  + sqrdmulh_s16_scalar((int16_t)(even - vm1),  (int16_t)(179 << 7)));
        out_640[x + 1U] = (uint16_t)(int16_t)(even + sqrdmulh_s16_scalar((int16_t)(odd  - even), (int16_t)( 26 << 7)));
        out_640[x + 2U] = urhadd_u16_s((uint16_t)even, (uint16_t)odd);
        out_640[x + 3U] = (uint16_t)(int16_t)(even + sqrdmulh_s16_scalar((int16_t)(odd  - even), (int16_t)(230 << 7)));
        out_640[x + 4U] = (uint16_t)(int16_t)(odd  + sqrdmulh_s16_scalar((int16_t)(vp2  - odd),  (int16_t)( 77 << 7)));
    }
#endif
}

/* Apply 256-grid chroma deltas onto 640-wide NV12 chroma rows (skipping masked edge chunks, 0 Q-spills) */
void hdf_live_evf_s3_apply_chroma_256_to_640(uint8_t *const *c_rows, uint32_t n_crows,
                                             const uint8_t *rin, const uint8_t *gin, const uint8_t *bin,
                                             const uint8_t *rout, const uint8_t *gout, const uint8_t *bout,
                                             uint32_t skip_mask) {
#if HDF_NEON
    int16x8_t c43  = vdupq_n_s16(43 << 7);
    int16x8_t c85  = vdupq_n_s16(85 << 7);
    int16x8_t c107 = vdupq_n_s16(107 << 7);
    int16x8_t c21  = vdupq_n_s16(21 << 7);
    uint8x16_t ep0 = hdf_ld1_u8(k_evf_exp_p0);
    uint8x16_t ep1 = hdf_ld1_u8(k_evf_exp_p1);
    uint8x8_t  ep2 = live_ld1_u8_8(k_evf_exp_p2);

    for (uint32_t chunk = 0U; chunk < 32U; ++chunk) {
        if (skip_mask & (1U << chunk)) continue;
        uint32_t goff = chunk << 3;
        uint32_t boff = chunk * 40U;

        int16x8_t dR = vreinterpretq_s16_u16(vsubl_u8(live_ld1_u8_8(rout + goff), live_ld1_u8_8(rin + goff)));
        int16x8_t dG = vreinterpretq_s16_u16(vsubl_u8(live_ld1_u8_8(gout + goff), live_ld1_u8_8(gin + goff)));
        int16x8_t dB = vreinterpretq_s16_u16(vsubl_u8(live_ld1_u8_8(bout + goff), live_ld1_u8_8(bin + goff)));

        int16x8_t dcb = vsubq_s16(vsubq_s16(vrshrq_n_s16(dB, 1), vqrdmulhq_s16(dR, c43)),
                                  vqrdmulhq_s16(dG, c85));
        int16x8_t dcr = vsubq_s16(vsubq_s16(vrshrq_n_s16(dR, 1), vqrdmulhq_s16(dG, c107)),
                                  vqrdmulhq_s16(dB, c21));

        int16x4_t cb_m = vrhadd_s16(vuzp1_s16(vget_low_s16(dcb), vget_high_s16(dcb)),
                                    vuzp2_s16(vget_low_s16(dcb), vget_high_s16(dcb)));
        int16x4_t cr_m = vrhadd_s16(vuzp1_s16(vget_low_s16(dcr), vget_high_s16(dcr)),
                                    vuzp2_s16(vget_low_s16(dcr), vget_high_s16(dcr)));
        int16x8_t dmid = vcombine_s16(cb_m, cr_m);

        uint8x8_t pcb = vqmovun_s16(dcb), pcr = vqmovun_s16(dcr);
        uint8x8_t ncb = vqmovun_s16(vqnegq_s16(dcb)), ncr = vqmovun_s16(vqnegq_s16(dcr));
        uint8x16_t p_grid = vzip1q_u8(vcombine_u8(pcb, pcb), vcombine_u8(pcr, pcr));
        uint8x16_t n_grid = vzip1q_u8(vcombine_u8(ncb, ncb), vcombine_u8(ncr, ncr));

        uint8x8_t pm = vqmovun_s16(dmid);
        uint8x8_t nm = vqmovun_s16(vqnegq_s16(dmid));
        uint8x8_t p_mid8 = vzip1_u8(pm, vext_u8(pm, pm, 4));
        uint8x8_t n_mid8 = vzip1_u8(nm, vext_u8(nm, nm, 4));

        uint8x16x2_t puv = {{ p_grid, vcombine_u8(p_mid8, p_mid8) }};
        uint8x16x2_t nuv = {{ n_grid, vcombine_u8(n_mid8, n_mid8) }};
        uint8x16_t p0 = vqtbl2q_u8(puv, ep0), p1 = vqtbl2q_u8(puv, ep1);
        uint8x8_t  p2 = vqtbl2_u8(puv, ep2);
        uint8x16_t n0 = vqtbl2q_u8(nuv, ep0), n1 = vqtbl2q_u8(nuv, ep1);
        uint8x8_t  n2 = vqtbl2_u8(nuv, ep2);

        for (uint32_t r = 0U; r < n_crows; ++r) {
            uint8_t *cr = c_rows[r] + boff;
            hdf_st1_u8(cr +  0U, vqsubq_u8(vqaddq_u8(hdf_ld1_u8(cr +  0U), p0), n0));
            hdf_st1_u8(cr + 16U, vqsubq_u8(vqaddq_u8(hdf_ld1_u8(cr + 16U), p1), n1));
            live_st1_u8_8(cr + 32U, vqsub_u8(vqadd_u8(live_ld1_u8_8(cr + 32U), p2), n2));
        }
    }
#else
    for (uint32_t chunk = 0U; chunk < 32U; ++chunk) {
        if (skip_mask & (1U << chunk)) continue;
        uint32_t goff = chunk << 3;
        int16_t dcb[8], dcr[8];
        for (uint32_t j = 0U; j < 8U; ++j) {
            uint32_t gx = goff + j;
            int16_t dR = (int16_t)((int32_t)rout[gx] - (int32_t)rin[gx]);
            int16_t dG = (int16_t)((int32_t)gout[gx] - (int32_t)gin[gx]);
            int16_t dB = (int16_t)((int32_t)bout[gx] - (int32_t)bin[gx]);
            dcb[j] = (int16_t)(srshr1_s16_scalar(dB)
                             - sqrdmulh_s16_scalar(dR, (int16_t)(43 << 7))
                             - sqrdmulh_s16_scalar(dG, (int16_t)(85 << 7)));
            dcr[j] = (int16_t)(srshr1_s16_scalar(dR)
                             - sqrdmulh_s16_scalar(dG, (int16_t)(107 << 7))
                             - sqrdmulh_s16_scalar(dB, (int16_t)(21 << 7)));
        }
        uint32_t cx_base = chunk * 20U;
        for (uint32_t k = 0U; k < 4U; ++k) {
            int16_t cb0 = dcb[2U * k], cb1 = dcb[2U * k + 1U];
            int16_t cr0 = dcr[2U * k], cr1 = dcr[2U * k + 1U];
            int16_t cbm = srshr1_s16_scalar((int16_t)(cb0 + cb1));
            int16_t crm = srshr1_s16_scalar((int16_t)(cr0 + cr1));
            int16_t cb5[5] = { cb0, cb0, cbm, cb1, cb1 };
            int16_t cr5[5] = { cr0, cr0, crm, cr1, cr1 };
            for (uint32_t r = 0U; r < n_crows; ++r) {
                uint8_t *crp = c_rows[r] + (cx_base + 5U * k) * 2U;
                for (uint32_t d = 0U; d < 5U; ++d) {
                    crp[2U * d]      = clamp_u8((int)crp[2U * d]      + (int)cb5[d]);
                    crp[2U * d + 1U] = clamp_u8((int)crp[2U * d + 1U] + (int)cr5[d]);
                }
            }
        }
    }
#endif
}

/* Cold-path exact 640-wide chroma composite ONLY for masked edge chunks (batched, 0 Q-spills) */
void hdf_live_evf_s3_chroma_exact_masked(const uint8_t *const *y_rows,
                                         uint8_t *const *c_rows,
                                         uint32_t n_crows,
                                         const uint16_t *gr, const uint16_t *gg, const uint16_t *gb,
                                         uint32_t mask, uint32_t level,
                                         uint8_t *fb_buf) {
    uint8_t chunks[32];
    uint32_t cnt = 0U;
    uint32_t m = mask;
    while (m != 0U) {
        chunks[cnt++] = (uint8_t)__builtin_ctz(m);
        m &= m - 1U;
    }
    if (cnt == 0U) return;

#if HDF_NEON
    /* Process in batches of up to 16 chunks (<= 16 * 3 * 20 * 6 = 5,760 B <= 6,144 B fb_buf) */
    for (uint32_t base_i = 0U; base_i < cnt; base_i += 16U) {
        uint32_t bcnt = cnt - base_i;
        if (bcnt > 16U) bcnt = 16U;
        const uint8_t *bchunks = chunks + base_i;

        uint8_t *in_r  = fb_buf + 0U * 960U;
        uint8_t *in_g  = fb_buf + 1U * 960U;
        uint8_t *in_b  = fb_buf + 2U * 960U;
        uint8_t *out_r = fb_buf + 3U * 960U;
        uint8_t *out_g = fb_buf + 4U * 960U;
        uint8_t *out_b = fb_buf + 5U * 960U;

        /* Phase 1: YCbCr -> RGB on 20 chroma cols (= 40 bytes) per masked chunk */
        {
            uint8x8_t  c128_8  = vdup_n_u8(128U);
            uint8x16_t c128_16 = vdupq_n_u8(128U);
            int16x8_t c103 = vdupq_n_s16(103 << 7);
            int16x8_t c198 = vdupq_n_s16(198 << 7);
            int16x8_t c88  = vdupq_n_s16(88 << 7);
            int16x8_t c183 = vdupq_n_s16(183 << 7);
            for (uint32_t i = 0U; i < bcnt; ++i) {
                uint32_t boff = (uint32_t)bchunks[i] * 40U;
                for (uint32_t r = 0U; r < n_crows; ++r) {
                    uint32_t poff = (i * n_crows + r) * 20U;
                    const uint8_t *py0 = y_rows[2U * r] + boff;
                    const uint8_t *py1 = y_rows[2U * r + 1U] + boff;
                    const uint8_t *pc  = c_rows[r] + boff;
                    LiveYccRgb16 a = live_ycc_to_rgb16_inc(&py0, &py1, &pc, c128_8, c128_16,
                                                           c103, c198, c88, c183);
                    hdf_st1_u8(in_r + poff, a.vr);
                    hdf_st1_u8(in_g + poff, a.vg);
                    hdf_st1_u8(in_b + poff, a.vb);

                    py0 = y_rows[2U * r] + boff + 8U;
                    py1 = y_rows[2U * r + 1U] + boff + 8U;
                    pc  = c_rows[r] + boff + 8U;
                    LiveYccRgb16 b = live_ycc_to_rgb16_inc(&py0, &py1, &pc, c128_8, c128_16,
                                                           c103, c198, c88, c183);
                    hdf_st1_u8(in_r + poff + 4U, b.vr);
                    hdf_st1_u8(in_g + poff + 4U, b.vg);
                    hdf_st1_u8(in_b + poff + 4U, b.vb);
                }
            }
        }

        /* Phase 2: 3-channel composite on packed 20-px rows (loads LUTs once, 0 Q-spills) */
        {
            LiveKnot16Regs k_invk = live_load_knot16_invk(level);
            LiveClzUmullRegs c64  = live_load_clz_umull();
            uint8x16_t c30 = vdupq_n_u8(30U);
            const uint8_t  *srcs[3] = { in_r, in_g, in_b };
            uint8_t        *dsts[3] = { out_r, out_g, out_b };
            const uint16_t *grds[3] = { gr, gg, gb };

            for (uint32_t ch = 0U; ch < 3U; ++ch) {
                const uint8_t  *src_base = srcs[ch];
                uint8_t        *dst_base = dsts[ch];
                const uint16_t *grd_base = grds[ch];
                for (uint32_t i = 0U; i < bcnt; ++i) {
                    uint32_t goff = (uint32_t)bchunks[i] << 3;
                    uint16_t _Alignas(16) s20[20];
                    {
                        uint16x8_t sv = hdf_ld1_u16(grd_base + goff);
                        uint16x4_t sv_md = vrhadd_u16(vuzp1_u16(vget_low_u16(sv), vget_high_u16(sv)),
                                                      vuzp2_u16(vget_low_u16(sv), vget_high_u16(sv)));
                        uint8x16x2_t stbl = {{
                            vreinterpretq_u8_u16(sv),
                            vreinterpretq_u8_u16(vcombine_u16(sv_md, sv_md))
                        }};
                        hdf_st1_u16(s20 +  0U, vreinterpretq_u16_u8(vqtbl2q_u8(stbl, hdf_ld1_u8(k_evf_exp_p0))));
                        hdf_st1_u16(s20 +  8U, vreinterpretq_u16_u8(vqtbl2q_u8(stbl, hdf_ld1_u8(k_evf_exp_p1))));
                        live_st1_u16_4(s20 + 16U, vreinterpret_u16_u8(vqtbl2_u8(stbl, live_ld1_u8_8(k_evf_exp_p2))));
                    }

                    for (uint32_t r = 0U; r < n_crows; ++r) {
                        uint32_t poff = (i * n_crows + r) * 20U;
                        for (uint32_t h = 0U; h < 2U; ++h) {
                            uint32_t so = h << 2; /* 0, then 4 */
                            uint8x16_t vx = hdf_ld1_u8(src_base + poff + so);
                            uint16x8_t t0, t1;
                            live_knot16_srgb16_to_u16(vx, &k_invk, c30, &t0, &t1);
                            uint16x8_t u0 = vaddq_u16(t0, hdf_ld1_u16(s20 + so));
                            uint16x8_t u1 = vaddq_u16(t1, hdf_ld1_u16(s20 + so + 8U));
                            if (__builtin_expect(vmaxvq_u16(vmaxq_u16(u0, u1)) > 25600U, 0)) {
                                uint16x8x2_t sh = live_shoulder_apply_16px(vx, u0, u1);
                                u0 = sh.val[0]; u1 = sh.val[1];
                            }
                            hdf_st1_u8(dst_base + poff + so, live_clz_umull64_lin15_to_srgb(u0, u1, &c64));
                        }
                    }
                }
            }
        }

        /* Phase 3: RGB delta -> CbCr update on 20 chroma cols (load both halves before storing!) */
        {
            int16x8_t c43  = vdupq_n_s16(43 << 7);
            int16x8_t c85  = vdupq_n_s16(85 << 7);
            int16x8_t c107 = vdupq_n_s16(107 << 7);
            int16x8_t c21  = vdupq_n_s16(21 << 7);
            for (uint32_t i = 0U; i < bcnt; ++i) {
                uint32_t boff = (uint32_t)bchunks[i] * 40U;
                for (uint32_t r = 0U; r < n_crows; ++r) {
                    uint32_t poff = (i * n_crows + r) * 20U;
                    uint8_t *cdst = c_rows[r] + boff;
                    uint8x16x2_t vc0 = vld2q_u8(cdst);
                    uint8x16x2_t vc1 = vld2q_u8(cdst + 8U);

                    /* First 16 cols (0..15) */
                    {
                        uint8x16_t ri = hdf_ld1_u8(in_r + poff), ro = hdf_ld1_u8(out_r + poff);
                        uint8x16_t gi = hdf_ld1_u8(in_g + poff), go = hdf_ld1_u8(out_g + poff);
                        uint8x16_t bi = hdf_ld1_u8(in_b + poff), bo = hdf_ld1_u8(out_b + poff);
                        int16x8_t dR0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(ro), vget_low_u8(ri)));
                        int16x8_t dR1 = vreinterpretq_s16_u16(vsubl_high_u8(ro, ri));
                        int16x8_t dG0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(go), vget_low_u8(gi)));
                        int16x8_t dG1 = vreinterpretq_s16_u16(vsubl_high_u8(go, gi));
                        int16x8_t dB0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(bo), vget_low_u8(bi)));
                        int16x8_t dB1 = vreinterpretq_s16_u16(vsubl_high_u8(bo, bi));
                        int16x8_t dcb0 = vsubq_s16(vsubq_s16(vrshrq_n_s16(dB0, 1), vqrdmulhq_s16(dR0, c43)),
                                                   vqrdmulhq_s16(dG0, c85));
                        int16x8_t dcb1 = vsubq_s16(vsubq_s16(vrshrq_n_s16(dB1, 1), vqrdmulhq_s16(dR1, c43)),
                                                   vqrdmulhq_s16(dG1, c85));
                        int16x8_t dcr0 = vsubq_s16(vsubq_s16(vrshrq_n_s16(dR0, 1), vqrdmulhq_s16(dG0, c107)),
                                                   vqrdmulhq_s16(dB0, c21));
                        int16x8_t dcr1 = vsubq_s16(vsubq_s16(vrshrq_n_s16(dR1, 1), vqrdmulhq_s16(dG1, c107)),
                                                   vqrdmulhq_s16(dB1, c21));
                        uint8x16x2_t co;
                        co.val[0] = vcombine_u8(
                            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dcb0), vget_low_u8(vc0.val[0])))),
                            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dcb1), vget_high_u8(vc0.val[0])))));
                        co.val[1] = vcombine_u8(
                            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dcr0), vget_low_u8(vc0.val[1])))),
                            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dcr1), vget_high_u8(vc0.val[1])))));
                        vst2q_u8(cdst, co);
                    }

                    /* Overlapping 16 cols (4..19) using pre-loaded vc1 */
                    {
                        uint32_t poff1 = poff + 4U;
                        uint8x16_t ri = hdf_ld1_u8(in_r + poff1), ro = hdf_ld1_u8(out_r + poff1);
                        uint8x16_t gi = hdf_ld1_u8(in_g + poff1), go = hdf_ld1_u8(out_g + poff1);
                        uint8x16_t bi = hdf_ld1_u8(in_b + poff1), bo = hdf_ld1_u8(out_b + poff1);
                        int16x8_t dR0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(ro), vget_low_u8(ri)));
                        int16x8_t dR1 = vreinterpretq_s16_u16(vsubl_high_u8(ro, ri));
                        int16x8_t dG0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(go), vget_low_u8(gi)));
                        int16x8_t dG1 = vreinterpretq_s16_u16(vsubl_high_u8(go, gi));
                        int16x8_t dB0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(bo), vget_low_u8(bi)));
                        int16x8_t dB1 = vreinterpretq_s16_u16(vsubl_high_u8(bo, bi));
                        int16x8_t dcb0 = vsubq_s16(vsubq_s16(vrshrq_n_s16(dB0, 1), vqrdmulhq_s16(dR0, c43)),
                                                   vqrdmulhq_s16(dG0, c85));
                        int16x8_t dcb1 = vsubq_s16(vsubq_s16(vrshrq_n_s16(dB1, 1), vqrdmulhq_s16(dR1, c43)),
                                                   vqrdmulhq_s16(dG1, c85));
                        int16x8_t dcr0 = vsubq_s16(vsubq_s16(vrshrq_n_s16(dR0, 1), vqrdmulhq_s16(dG0, c107)),
                                                   vqrdmulhq_s16(dB0, c21));
                        int16x8_t dcr1 = vsubq_s16(vsubq_s16(vrshrq_n_s16(dR1, 1), vqrdmulhq_s16(dG1, c107)),
                                                   vqrdmulhq_s16(dB1, c21));
                        uint8x16x2_t co;
                        co.val[0] = vcombine_u8(
                            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dcb0), vget_low_u8(vc1.val[0])))),
                            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dcb1), vget_high_u8(vc1.val[0])))));
                        co.val[1] = vcombine_u8(
                            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dcr0), vget_low_u8(vc1.val[1])))),
                            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dcr1), vget_high_u8(vc1.val[1])))));
                        vst2q_u8(cdst + 8U, co);
                    }
                }
            }
        }
    }
#else
    (void)fb_buf;
    uint32_t idx = (level >= 1U && level <= 3U) ? (level - 1U) : 0U;
    for (uint32_t i = 0U; i < cnt; ++i) {
        uint32_t chunk = chunks[i];
        uint32_t goff = chunk << 3;
        uint32_t cx_base = chunk * 20U;
        uint16_t sr20[20], sg20[20], sb20[20];
        for (uint32_t k = 0U; k < 4U; ++k) {
            uint16_t r0 = gr[goff + 2U * k], r1 = gr[goff + 2U * k + 1U];
            uint16_t g0 = gg[goff + 2U * k], g1 = gg[goff + 2U * k + 1U];
            uint16_t b0 = gb[goff + 2U * k], b1 = gb[goff + 2U * k + 1U];
            uint32_t p = 5U * k;
            sr20[p + 0U] = r0; sr20[p + 1U] = r0; sr20[p + 2U] = urhadd_u16_s(r0, r1); sr20[p + 3U] = r1; sr20[p + 4U] = r1;
            sg20[p + 0U] = g0; sg20[p + 1U] = g0; sg20[p + 2U] = urhadd_u16_s(g0, g1); sg20[p + 3U] = g1; sg20[p + 4U] = g1;
            sb20[p + 0U] = b0; sb20[p + 1U] = b0; sb20[p + 2U] = urhadd_u16_s(b0, b1); sb20[p + 3U] = b1; sb20[p + 4U] = b1;
        }
        for (uint32_t r = 0U; r < n_crows; ++r) {
            const uint8_t *yr0 = y_rows[2U * r];
            const uint8_t *yr1 = y_rows[2U * r + 1U];
            uint8_t *cr = c_rows[r];
            for (uint32_t d = 0U; d < 20U; ++d) {
                uint32_t cx = cx_base + d;
                uint8_t yh = uhadd_u8_s(urhadd_u8_s(yr0[2U * cx], yr0[2U * cx + 1U]),
                                        urhadd_u8_s(yr1[2U * cx], yr1[2U * cx + 1U]));
                int16_t cb  = (int16_t)((int32_t)cr[2U * cx] - 128);
                int16_t crv = (int16_t)((int32_t)cr[2U * cx + 1U] - 128);
                int32_t dr = (int32_t)crv + sqrdmulh_s16_scalar(crv, (int16_t)(103 << 7));
                int32_t db = (int32_t)cb  + sqrdmulh_s16_scalar(cb,  (int16_t)(198 << 7));
                int32_t dg = -(sqrdmulh_s16_scalar(cb, (int16_t)(88 << 7)) +
                               sqrdmulh_s16_scalar(crv, (int16_t)(183 << 7)));
                uint8_t ri = clamp_u8((int)yh + (int)dr);
                uint8_t gi = clamp_u8((int)yh + (int)dg);
                uint8_t bi = clamp_u8((int)yh + (int)db);
                uint8_t ro = live_comp_px_scalar(ri, sr20[d], idx);
                uint8_t go = live_comp_px_scalar(gi, sg20[d], idx);
                uint8_t bo = live_comp_px_scalar(bi, sb20[d], idx);
                int16_t dR = (int16_t)((int32_t)ro - (int32_t)ri);
                int16_t dG = (int16_t)((int32_t)go - (int32_t)gi);
                int16_t dB = (int16_t)((int32_t)bo - (int32_t)bi);
                int32_t dcb = srshr1_s16_scalar(dB)
                            - sqrdmulh_s16_scalar(dR, (int16_t)(43 << 7))
                            - sqrdmulh_s16_scalar(dG, (int16_t)(85 << 7));
                int32_t dcr = srshr1_s16_scalar(dR)
                            - sqrdmulh_s16_scalar(dG, (int16_t)(107 << 7))
                            - sqrdmulh_s16_scalar(dB, (int16_t)(21 << 7));
                cr[2U * cx]      = clamp_u8((int)cr[2U * cx]      + (int)dcb);
                cr[2U * cx + 1U] = clamp_u8((int)cr[2U * cx + 1U] + (int)dcr);
            }
        }
    }
#endif
}

/* ---- 10. V71.5 Fused Still Stage 3 Row Kernel (0 Q-spills, 0-diff) ------ */

#if HDF_NEON

static __attribute__((noinline)) uint32_t hdf_still_tile_hlerp_q15(
    const uint16_t *vr, const uint16_t *vg, const uint16_t *vb,
    const uint16_t *gx, const uint8_t *fx,
    uint16_t *s_tile, uint32_t tn, uint32_t monochrome) {
    uint16_t *sr = s_tile;
    uint16_t *sg = s_tile + tn;
    uint16_t *sb = s_tile + 2U * tn;
    uint16x8_t c0100 = vdupq_n_u16(0x0100U);
    uint8x16_t c2    = vdupq_n_u8(2U);
    uint16x8_t v_or  = vdupq_n_u16(0U);

    for (uint32_t x = 0U; x < tn; x += 8U) {
        uint16_t base_g = gx[x];
        uint32_t span = (uint32_t)gx[x + 7U] - (uint32_t)base_g;
        if (__builtin_expect(span == 0U, 1)) {
            uint8x8_t fx8 = live_ld1_u8_8(fx + x);
            int16x8_t fx15 = vshlq_n_s16(vreinterpretq_s16_u16(vmovl_u8(fx8)), 7);

            int16_t a = (int16_t)vr[base_g];
            int16_t d = (int16_t)((int16_t)vr[base_g + 1U] - a);
            uint16x8_t o = vreinterpretq_u16_s16(
                vaddq_s16(vdupq_n_s16(a), vqrdmulhq_s16(vdupq_n_s16(d), fx15)));
            v_or = vorrq_u16(v_or, o);
            hdf_st8a_u16(sr + x, o);

            if (!monochrome) {
                a = (int16_t)vg[base_g];
                d = (int16_t)((int16_t)vg[base_g + 1U] - a);
                o = vreinterpretq_u16_s16(
                    vaddq_s16(vdupq_n_s16(a), vqrdmulhq_s16(vdupq_n_s16(d), fx15)));
                v_or = vorrq_u16(v_or, o);
                hdf_st8a_u16(sg + x, o);

                a = (int16_t)vb[base_g];
                d = (int16_t)((int16_t)vb[base_g + 1U] - a);
                o = vreinterpretq_u16_s16(
                    vaddq_s16(vdupq_n_s16(a), vqrdmulhq_s16(vdupq_n_s16(d), fx15)));
                v_or = vorrq_u16(v_or, o);
                hdf_st8a_u16(sb + x, o);
            }
        } else if (span <= 6U) {
            uint16x8_t rel = vsubq_u16(hdf_ld1_u16(gx + x), vdupq_n_u16(base_g));
            uint8x16_t idx0 = vreinterpretq_u8_u16(vmlaq_n_u16(c0100, rel, 0x0202U));
            uint8x16_t idx1 = vaddq_u8(idx0, c2);
            uint8x8_t fx8 = live_ld1_u8_8(fx + x);
            int16x8_t fx15 = vshlq_n_s16(vreinterpretq_s16_u16(vmovl_u8(fx8)), 7);

            uint8x16_t win = vreinterpretq_u8_u16(hdf_ld1_u16(vr + base_g));
            int16x8_t v0 = vreinterpretq_s16_u8(vqtbl1q_u8(win, idx0));
            int16x8_t v1 = vreinterpretq_s16_u8(vqtbl1q_u8(win, idx1));
            uint16x8_t o = vreinterpretq_u16_s16(vaddq_s16(v0, vqrdmulhq_s16(vsubq_s16(v1, v0), fx15)));
            v_or = vorrq_u16(v_or, o);
            hdf_st8a_u16(sr + x, o);

            if (!monochrome) {
                win = vreinterpretq_u8_u16(hdf_ld1_u16(vg + base_g));
                v0 = vreinterpretq_s16_u8(vqtbl1q_u8(win, idx0));
                v1 = vreinterpretq_s16_u8(vqtbl1q_u8(win, idx1));
                o = vreinterpretq_u16_s16(vaddq_s16(v0, vqrdmulhq_s16(vsubq_s16(v1, v0), fx15)));
                v_or = vorrq_u16(v_or, o);
                hdf_st8a_u16(sg + x, o);

                win = vreinterpretq_u8_u16(hdf_ld1_u16(vb + base_g));
                v0 = vreinterpretq_s16_u8(vqtbl1q_u8(win, idx0));
                v1 = vreinterpretq_s16_u8(vqtbl1q_u8(win, idx1));
                o = vreinterpretq_u16_s16(vaddq_s16(v0, vqrdmulhq_s16(vsubq_s16(v1, v0), fx15)));
                v_or = vorrq_u16(v_or, o);
                hdf_st8a_u16(sb + x, o);
            }
        } else {
            uint16_t sor = 0U;
            for (uint32_t j = 0U; j < 8U; ++j) {
                uint32_t g = gx[x + j];
                int16_t fx15 = (int16_t)((uint16_t)fx[x + j] << 7);
                int16_t ar = (int16_t)vr[g], br = (int16_t)vr[g + 1U];
                uint16_t r = (uint16_t)(int16_t)(ar + sqrdmulh_s16_scalar((int16_t)(br - ar), fx15));
                sr[x + j] = r;
                sor |= r;
                if (!monochrome) {
                    int16_t ag = (int16_t)vg[g], bg = (int16_t)vg[g + 1U];
                    int16_t ab = (int16_t)vb[g], bb = (int16_t)vb[g + 1U];
                    uint16_t gv = (uint16_t)(int16_t)(ag + sqrdmulh_s16_scalar((int16_t)(bg - ag), fx15));
                    uint16_t bv = (uint16_t)(int16_t)(ab + sqrdmulh_s16_scalar((int16_t)(bb - ab), fx15));
                    sg[x + j] = gv;
                    sb[x + j] = bv;
                    sor |= (uint16_t)(gv | bv);
                }
            }
            v_or = vorrq_u16(v_or, vdupq_n_u16(sor));
        }
    }
    return vmaxvq_u16(v_or) != 0U ? 1U : 0U;
}

static __attribute__((noinline)) uint32_t hdf_still_tile_is_black(
    const uint8_t *y_row, const uint8_t *c_row, uint32_t tn, uint32_t monochrome) {
    uint8x16_t ymax = vdupq_n_u8(0U);
    uint8x16_t cdev = vdupq_n_u8(0U);
    uint8x16_t c128 = vdupq_n_u8(128U);
    for (uint32_t i = 0U; i < tn; i += 16U) {
        ymax = vmaxq_u8(ymax, hdf_ld1_u8(y_row + i));
        if (!monochrome) {
            cdev = vmaxq_u8(cdev, vabdq_u8(hdf_ld1_u8(c_row + i), c128));
        }
    }
    uint32_t ym = (uint32_t)vmaxvq_u8(ymax);
    uint32_t cm = monochrome ? 0U : (uint32_t)vmaxvq_u8(cdev);
    return (ym + 2U * cm) <= 2U ? 1U : 0U;
}

static __attribute__((noinline)) void hdf_still_tile_ycc2rgb(
    const uint8_t *y_row, const uint8_t *c_row, uint8_t *rgb_in, uint32_t tn) {
    uint8_t *ri = rgb_in;
    uint8_t *gi = rgb_in + tn;
    uint8_t *bi = rgb_in + 2U * tn;
    uint8x8_t u128 = vdup_n_u8(128U);
    int16x8_t c103 = vdupq_n_s16(103 << 7);
    int16x8_t c198 = vdupq_n_s16(198 << 7);
    int16x8_t c88  = vdupq_n_s16(88 << 7);
    int16x8_t c183 = vdupq_n_s16(183 << 7);

    for (uint32_t i = 0U; i < tn; i += 16U) {
        uint8x16_t vy = hdf_ld1_u8(y_row + i);
        uint8x8x2_t vc = vld2_u8(c_row + i);
        int16x8_t cb = vreinterpretq_s16_u16(vsubl_u8(vc.val[0], u128));
        int16x8_t cr = vreinterpretq_s16_u16(vsubl_u8(vc.val[1], u128));

        int16x8_t dr = vaddq_s16(cr, vqrdmulhq_s16(cr, c103));
        int16x8_t db = vaddq_s16(cb, vqrdmulhq_s16(cb, c198));
        int16x8_t dg = vnegq_s16(vaddq_s16(vqrdmulhq_s16(cb, c88), vqrdmulhq_s16(cr, c183)));

        uint8x8_t y0 = vget_low_u8(vy), y1 = vget_high_u8(vy);
        hdf_st16a_u8(ri + i, vcombine_u8(
            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(vzip1q_s16(dr, dr)), y0))),
            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(vzip2q_s16(dr, dr)), y1)))));
        hdf_st16a_u8(gi + i, vcombine_u8(
            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(vzip1q_s16(dg, dg)), y0))),
            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(vzip2q_s16(dg, dg)), y1)))));
        hdf_st16a_u8(bi + i, vcombine_u8(
            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(vzip1q_s16(db, db)), y0))),
            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(vzip2q_s16(db, db)), y1)))));
    }
}

/* Fused flat 16-knot sRGB->Lin + sk_q15 add + cold shoulder + CLZ Lin->sRGB (0 Q-spills).
 * Safe for both out-of-place (color 3*tn) and in-place (monochrome src==dst, tn).
 */
static __attribute__((noinline)) void hdf_still_tile_comp_flat(
    const uint8_t *src, const uint16_t *sk, uint8_t *dst, uint32_t n16, uint32_t level) {
    LiveKnot16Regs k_invk = live_load_knot16_invk(level);
    LiveClzUmullRegs c64  = live_load_clz_umull();
    uint8x16_t c30 = vdupq_n_u8(30U);

    for (uint32_t n = n16 >> 4; n > 0U; --n) {
        uint16x8x2_t sv = live_ld1_u16_x2_inc(&sk);
        uint8x16_t vx = live_ld1_u8_inc(&src);
        uint16x8_t t0, t1;
        live_knot16_srgb16_to_u16(vx, &k_invk, c30, &t0, &t1);
        uint16x8_t u0 = vaddq_u16(t0, sv.val[0]);
        uint16x8_t u1 = vaddq_u16(t1, sv.val[1]);
        if (__builtin_expect(vmaxvq_u16(vmaxq_u16(u0, u1)) > 25600U, 0)) {
            uint16x8x2_t sh = live_shoulder_apply_16px(vx, u0, u1);
            u0 = sh.val[0];
            u1 = sh.val[1];
        }
        live_st1_u8_inc(&dst, live_clz_umull64_lin15_to_srgb(u0, u1, &c64));
    }
}

static __attribute__((noinline)) void hdf_still_tile_delta_ycc(
    uint8_t *y_row, uint8_t *c_row,
    const uint8_t *rgb_in, const uint8_t *rgb_out,
    int16_t *dcb_stash, int16_t *dcr_stash,
    uint32_t tn, uint32_t chroma_mode) {
    const uint8_t *ri = rgb_in,  *gi = rgb_in  + tn, *bi = rgb_in  + 2U * tn;
    const uint8_t *ro = rgb_out, *go = rgb_out + tn, *bo = rgb_out + 2U * tn;
    int16x8_t c77  = vdupq_n_s16(77 << 7);
    int16x8_t c150 = vdupq_n_s16(150 << 7);
    int16x8_t c29  = vdupq_n_s16(29 << 7);
    int16x8_t c43  = vdupq_n_s16(43 << 6);
    int16x8_t c85  = vdupq_n_s16(85 << 6);
    int16x8_t c107 = vdupq_n_s16(107 << 6);
    int16x8_t c21  = vdupq_n_s16(21 << 6);

    for (uint32_t i = 0U; i < tn; i += 16U) {
        uint8x16_t vy = hdf_ld1_u8(y_row + i);
        uint8x16_t r0v = hdf_ld16a_u8(ri + i), r1v = hdf_ld16a_u8(ro + i);
        uint8x16_t g0v = hdf_ld16a_u8(gi + i), g1v = hdf_ld16a_u8(go + i);
        uint8x16_t b0v = hdf_ld16a_u8(bi + i), b1v = hdf_ld16a_u8(bo + i);

        int16x8_t dR0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(r1v), vget_low_u8(r0v)));
        int16x8_t dR1 = vreinterpretq_s16_u16(vsubl_high_u8(r1v, r0v));
        int16x8_t dG0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(g1v), vget_low_u8(g0v)));
        int16x8_t dG1 = vreinterpretq_s16_u16(vsubl_high_u8(g1v, g0v));
        int16x8_t dB0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(b1v), vget_low_u8(b0v)));
        int16x8_t dB1 = vreinterpretq_s16_u16(vsubl_high_u8(b1v, b0v));

        int16x8_t dy0 = vaddq_s16(vaddq_s16(vqrdmulhq_s16(dR0, c77), vqrdmulhq_s16(dG0, c150)),
                                  vqrdmulhq_s16(dB0, c29));
        int16x8_t dy1 = vaddq_s16(vaddq_s16(vqrdmulhq_s16(dR1, c77), vqrdmulhq_s16(dG1, c150)),
                                  vqrdmulhq_s16(dB1, c29));
        hdf_st1_u8(y_row + i, vcombine_u8(
            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dy0), vget_low_u8(vy)))),
            vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dy1), vget_high_u8(vy))))));

        int16x8_t pR = vpaddq_s16(dR0, dR1);
        int16x8_t pG = vpaddq_s16(dG0, dG1);
        int16x8_t pB = vpaddq_s16(dB0, dB1);
        int16x8_t dcb = vsubq_s16(vsubq_s16(vrshrq_n_s16(pB, 2), vqrdmulhq_s16(pR, c43)),
                                  vqrdmulhq_s16(pG, c85));
        int16x8_t dcr = vsubq_s16(vsubq_s16(vrshrq_n_s16(pR, 2), vqrdmulhq_s16(pG, c107)),
                                  vqrdmulhq_s16(pB, c21));

        uint32_t pi = i >> 1;
        if (chroma_mode == HDF_CHROMA_420_EVEN) {
            hdf_st1_s16(dcb_stash + pi, dcb);
            hdf_st1_s16(dcr_stash + pi, dcr);
        } else {
            uint8x8x2_t vc = vld2_u8(c_row + i);
            if (chroma_mode == HDF_CHROMA_420_ODD) {
                dcb = vrhaddq_s16(hdf_ld1_s16(dcb_stash + pi), dcb);
                dcr = vrhaddq_s16(hdf_ld1_s16(dcr_stash + pi), dcr);
            }
            uint8x8x2_t co;
            co.val[0] = vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dcb), vc.val[0])));
            co.val[1] = vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vreinterpretq_u16_s16(dcr), vc.val[1])));
            vst2_u8(c_row + i, co);
        }
    }
}
#endif /* HDF_NEON */

void hdf_still_composite_row_fast(uint8_t *y_row, uint8_t *c_row,
                                  const uint16_t *vr_q15, const uint16_t *vg_q15, const uint16_t *vb_q15,
                                  const uint16_t *gx_arr, const uint8_t *fx_arr,
                                  int16_t *dcb_stash, int16_t *dcr_stash,
                                  uint8_t *tile_scratch, uint32_t width,
                                  uint32_t level, uint32_t monochrome, uint32_t chroma_mode) {
    uint32_t x = 0U;
    uint32_t lvl_idx = (level >= 1U && level <= 3U) ? (level - 1U) : 0U;

#if HDF_NEON
    uint16_t *s_tile  = (uint16_t *)(void *)tile_scratch;
    uint8_t  *rgb_in  = tile_scratch + (size_t)HDF_STILL_TILE_PX * 6U;
    uint8_t  *rgb_out = rgb_in + (size_t)HDF_STILL_TILE_PX * 3U;

    for (; x + 16U <= width; ) {
        uint32_t rem = (width - x) & ~15U;
        uint32_t tn = rem > HDF_STILL_TILE_PX ? HDF_STILL_TILE_PX : rem;

        uint32_t s_nz = hdf_still_tile_hlerp_q15(vr_q15, vg_q15, vb_q15,
                                                 gx_arr + x, fx_arr + x,
                                                 s_tile, tn, monochrome);
        if (!s_nz && (monochrome || chroma_mode != HDF_CHROMA_420_ODD) &&
            hdf_still_tile_is_black(y_row + x, c_row + x, tn, monochrome)) {
            if (!monochrome && chroma_mode == HDF_CHROMA_420_EVEN) {
                int16x8_t vz = vdupq_n_s16(0);
                uint32_t pi = x >> 1;
                uint32_t pairs = tn >> 1;
                for (uint32_t j = 0U; j < pairs; j += 8U) {
                    hdf_st1_s16(dcb_stash + pi + j, vz);
                    hdf_st1_s16(dcr_stash + pi + j, vz);
                }
            }
            x += tn;
            continue;
        }

        if (monochrome) {
            hdf_still_tile_comp_flat(y_row + x, s_tile, y_row + x, tn, level);
        } else {
            hdf_still_tile_ycc2rgb(y_row + x, c_row + x, rgb_in, tn);
            hdf_still_tile_comp_flat(rgb_in, s_tile, rgb_out, 3U * tn, level);
            hdf_still_tile_delta_ycc(y_row + x, c_row + x, rgb_in, rgb_out,
                                     dcb_stash + (x >> 1), dcr_stash + (x >> 1),
                                     tn, chroma_mode);
        }
        x += tn;
    }
#else
    (void)tile_scratch;
#endif

    if (monochrome) {
        for (; x < width; ++x) {
            uint32_t g = gx_arr[x];
            int16_t fx15 = (int16_t)((uint16_t)fx_arr[x] << 7);
            int16_t ar = (int16_t)vr_q15[g], br = (int16_t)vr_q15[g + 1U];
            uint16_t sr = (uint16_t)(int16_t)(ar + sqrdmulh_s16_scalar((int16_t)(br - ar), fx15));
            y_row[x] = live_comp_px_scalar(y_row[x], sr, lvl_idx);
        }
        return;
    }

    for (; x + 2U <= width; x += 2U) {
        int16_t cb  = (int16_t)((int32_t)c_row[x] - 128);
        int16_t crv = (int16_t)((int32_t)c_row[x + 1U] - 128);
        int32_t off_r = (int32_t)crv + sqrdmulh_s16_scalar(crv, (int16_t)(103 << 7));
        int32_t off_b = (int32_t)cb  + sqrdmulh_s16_scalar(cb,  (int16_t)(198 << 7));
        int32_t off_g = -(sqrdmulh_s16_scalar(cb, (int16_t)(88 << 7)) +
                          sqrdmulh_s16_scalar(crv, (int16_t)(183 << 7)));

        int16_t pR = 0, pG = 0, pB = 0;
        for (uint32_t j = 0U; j < 2U; ++j) {
            uint32_t px = x + j;
            uint32_t g = gx_arr[px];
            int16_t fx15 = (int16_t)((uint16_t)fx_arr[px] << 7);
            int16_t ar = (int16_t)vr_q15[g], br = (int16_t)vr_q15[g + 1U];
            int16_t ag = (int16_t)vg_q15[g], bg = (int16_t)vg_q15[g + 1U];
            int16_t ab = (int16_t)vb_q15[g], bb = (int16_t)vb_q15[g + 1U];
            uint16_t sr = (uint16_t)(int16_t)(ar + sqrdmulh_s16_scalar((int16_t)(br - ar), fx15));
            uint16_t sg = (uint16_t)(int16_t)(ag + sqrdmulh_s16_scalar((int16_t)(bg - ag), fx15));
            uint16_t sb = (uint16_t)(int16_t)(ab + sqrdmulh_s16_scalar((int16_t)(bb - ab), fx15));

            int yv = (int)y_row[px];
            uint8_t rin = clamp_u8(yv + (int)off_r);
            uint8_t gin = clamp_u8(yv + (int)off_g);
            uint8_t bin = clamp_u8(yv + (int)off_b);

            uint8_t rout = live_comp_px_scalar(rin, sr, lvl_idx);
            uint8_t gout = live_comp_px_scalar(gin, sg, lvl_idx);
            uint8_t bout = live_comp_px_scalar(bin, sb, lvl_idx);

            int16_t dR = (int16_t)((int32_t)rout - (int32_t)rin);
            int16_t dG = (int16_t)((int32_t)gout - (int32_t)gin);
            int16_t dB = (int16_t)((int32_t)bout - (int32_t)bin);

            int32_t dy = (int32_t)sqrdmulh_s16_scalar(dR, (int16_t)(77 << 7)) +
                         (int32_t)sqrdmulh_s16_scalar(dG, (int16_t)(150 << 7)) +
                         (int32_t)sqrdmulh_s16_scalar(dB, (int16_t)(29 << 7));
            y_row[px] = clamp_u8(yv + (int)dy);

            pR = (int16_t)(pR + dR);
            pG = (int16_t)(pG + dG);
            pB = (int16_t)(pB + dB);
        }

        int16_t dcb = (int16_t)(srshr2_s16_scalar(pB) -
                                sqrdmulh_s16_scalar(pR, (int16_t)(43 << 6)) -
                                sqrdmulh_s16_scalar(pG, (int16_t)(85 << 6)));
        int16_t dcr = (int16_t)(srshr2_s16_scalar(pR) -
                                sqrdmulh_s16_scalar(pG, (int16_t)(107 << 6)) -
                                sqrdmulh_s16_scalar(pB, (int16_t)(21 << 6)));

        uint32_t pair_idx = x >> 1;
        if (chroma_mode == HDF_CHROMA_420_EVEN) {
            dcb_stash[pair_idx] = dcb;
            dcr_stash[pair_idx] = dcr;
        } else {
            int16_t cb_d = dcb;
            int16_t cr_d = dcr;
            if (chroma_mode == HDF_CHROMA_420_ODD) {
                cb_d = srshr1_s16_scalar((int16_t)(dcb_stash[pair_idx] + cb_d));
                cr_d = srshr1_s16_scalar((int16_t)(dcr_stash[pair_idx] + cr_d));
            }
            c_row[x]      = clamp_u8((int)c_row[x]      + (int)cb_d);
            c_row[x + 1U] = clamp_u8((int)c_row[x + 1U] + (int)cr_d);
        }
    }
}
