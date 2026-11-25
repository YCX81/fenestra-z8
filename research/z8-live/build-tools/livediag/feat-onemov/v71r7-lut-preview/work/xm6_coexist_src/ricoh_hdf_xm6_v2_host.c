/* SPDX-License-Identifier: MIT
 * Nikon Z8 C3.11 EXPEED 7 CEVA-XM6 DSP — Ricoh Optical HDF v2 Reference Kernel (Core 0 Slot 41)
 *
 * Pure-integer, zero-float, zero-malloc, zero-mutable-global, DMA-tiled C11 reference kernel:
 *   - Engine A (LIVE_256_YC):   100% bit-exact 0-diff vs V71.5 z8_hdf_live_nv12_1024()
 *   - Engine B (STILL_GENERAL): 100% bit-exact 0-diff vs V71.5 z8_hdf_apply_ex()
 *
 * Hardware resource fit on Z8 Core 0 (z8_xm6_elf0.elf):
 *   - FW_APP_WORKBUF (0x00008000, 192 KiB IDM):
 *       STILL S1: 113,664 B (ping-pong 2x Y/C lines + RGB/Lin + accum + grid staging)
 *       STILL S2: 195,728 B (100% IDM-resident 3-scale pyramid + row-major O(1) V-blur!)
 *       STILL S3: 120,048 B (ping-pong 2-row 2D strips + 512-px tile scratch + grid cache)
 *       LIVE  S1/S2/S3: max 138,080 B (100% IDM-resident 3-scale Q13 pyramid)
 *   - FW_EXT_WORKBUF (0x41930000, 512 KiB DRAM):
 *       STILL: 411,648 B (3x 68,608 u16 scatter grids)
 *       LIVE:  482,912 B (grid_r/g/b/y + r256/g256/b256 + edge masks)
 */
#include "ricoh_hdf_xm6_v2.h"
#include <string.h>

/* ============================================================================
 * Optional Cycle Counter Porting Hook (for CEVA xm6sim / MSS Clock Profiling)
 * ============================================================================ */

#if defined(XM6_SIM_CYCLE_COUNTER)
#ifdef XM6_CUSTOM_CYCLES_HEADER
#include XM6_CUSTOM_CYCLES_HEADER
#else
extern uint64_t xm6_read_cycles(void);
#endif
#else
static inline uint64_t xm6_read_cycles(void)
{
    return 0ULL;
}
#endif

/* ============================================================================
 * Shared Optical HDF Constants & Read-Only Tables (.rodata)
 * ============================================================================ */

#define HDF_RGB_R_CR         359
#define HDF_RGB_G_CB          88
#define HDF_RGB_G_CR         183
#define HDF_RGB_B_CB         454

#define HDF_CORE_KNEE_Q16    53248U

#define HDF_GRID_MAX_DIM     344U
#define HDF_GRID_MAX_STRIDE  352U
#define HDF_GRID_N0_CAP      68608U
#define HDF_GRID_N1_CAP      19200U
#define HDF_GRID_N2_CAP       6144U
#define HDF_VROW_PAD            40U
#define HDF_VBLUR_RING_SLOTS     8U /* supports radius <= 6 (r+2 <= 8) for all max_g <= 344 */
#define HDF_STILL_TILE_PX      512U

#define HDF_LIVE_W           1024U
#define HDF_LIVE_CW           512U
#define HDF_LIVE_GW           256U
#define HDF_LIVE_G0_CAP      (256U * 171U) /* 43,776 */
#define HDF_LIVE_G1_CAP      (128U *  86U) /* 11,008 */
#define HDF_LIVE_G2_CAP      ( 64U *  43U) /*  2,752 */

enum {
    HDF_CHROMA_422_DIRECT = 0U,
    HDF_CHROMA_420_EVEN   = 1U,
    HDF_CHROMA_420_ODD    = 2U
};

typedef struct {
    uint16_t k_q12;
    uint8_t  w1, w2, w3;
    uint8_t  pad;
    uint16_t knee_q16;
    uint16_t boost_quad_q14;
    uint16_t boost_core_q14;
} HdfPreset;

/* Monotonic 4-level optical HDF progression (~1/8, ~1/4, ~1/2 mist filters) */
static const HdfPreset k_hdf_presets[4] = {
    {   0U,   0U,  0U,  0U, 0U, 22528U,     0U,     0U }, /* OFF */
    { 328U, 140U, 84U, 32U, 0U, 22528U, 26214U, 19661U }, /* WEAK   (k= 8.0%) */
    { 592U, 124U, 92U, 40U, 0U, 22528U, 32768U, 24576U }, /* MEDIUM (k=14.5%) */
    { 920U, 108U, 98U, 50U, 0U, 22528U, 37683U, 27853U }, /* STRONG (k=22.5%) */
};

/* 256-entry exact sRGB -> Q16 Linear LUT (512 B; used by STILL Stage 1) */
static const uint16_t k_srgb_to_lin_u16[256] = {
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

/* 16-knot sRGB -> Linear Q15 (0..32767) LUTs (48 B; primary XM6 VPU vlut target!) */
static const uint8_t k_s2l16_lin_blo[16] = {
      0U, 176U, 192U, 184U, 148U,  72U, 228U, 152U,
    144U, 156U, 236U, 120U,  92U, 164U,  73U, 151U,
};
static const uint8_t k_s2l16_lin_bhi[16] = {
      0U,   0U,   1U,   3U,   6U,  10U,  14U,  20U,
     27U,  35U,  44U,  55U,  67U,  80U,  95U, 111U,
};
static const uint8_t k_s2l16_lin_slp[16] = {
      4U,   8U,  16U,  22U,  28U,  36U,  46U,  56U,
     64U,  74U,  84U,  95U, 106U, 117U, 129U, 140U,
};

/* 16-knot sRGB -> (1-k)*Linear Q15 per level [0=WEAK, 1=MEDIUM, 2=STRONG] (144 B) */
static const uint8_t k_s2l16_invk_blo[3][16] = {
    {   0U, 138U, 167U, 112U, 251U, 103U, 183U,   8U,
       89U, 197U,  72U,  13U, 255U,  41U, 178U, 161U },
    {   0U, 134U, 138U,  45U, 145U, 185U, 194U, 171U,
      144U, 124U, 108U, 120U, 170U, 253U, 137U, 111U },
    {   0U, 126U,  98U, 228U,   7U, 232U, 135U, 254U,
       89U, 163U, 215U, 254U,  62U, 131U, 227U, 121U },
};
static const uint8_t k_s2l16_invk_bhi[3][16] = {
    {   0U,   0U,   1U,   3U,   5U,   9U,  13U,  19U,
       25U,  32U,  41U,  51U,  61U,  74U,  87U, 102U },
    {   0U,   0U,   1U,   3U,   5U,   8U,  12U,  17U,
       23U,  30U,  38U,  47U,  57U,  68U,  81U,  95U },
    {   0U,   0U,   1U,   2U,   5U,   7U,  11U,  15U,
       21U,  27U,  34U,  42U,  52U,  62U,  73U,  86U },
};
static const uint8_t k_s2l16_invk_slp[3][16] = {
    {   5U,   9U,  14U,  20U,  27U,  34U,  42U,  50U,
       59U,  68U,  78U,  87U,  97U, 108U, 118U, 129U },
    {   4U,   8U,  13U,  19U,  25U,  32U,  39U,  47U,
       55U,  63U,  72U,  81U,  90U, 100U, 110U, 120U },
    {   4U,   7U,  12U,  17U,  23U,  29U,  36U,  43U,
       50U,  57U,  65U,  74U,  82U,  91U, 100U, 109U },
};

/* Q15 CLZ+UMULL64 Linear Q15 (0..32767) -> sRGB (0..255) LUTs (96 B) */
static const uint8_t k_clz_base[16] = {
    187U, 187U, 137U,  99U,  71U,  49U,  34U,  22U,
     13U,   6U,   3U,   2U,   1U,   0U,   0U,   0U,
};
static const uint8_t k_clz_scale[16] = {
     91U,  91U,  67U,  51U,  37U,  29U,  20U,  15U,
     11U,   9U,   4U,   1U,   0U,   2U,   0U,   0U,
};
static const uint8_t k_clz_dm[64] = {
      2U,   6U,   9U,  13U,  16U,  20U,  24U,  27U,
     31U,  34U,  37U,  41U,  44U,  48U,  51U,  54U,
     58U,  61U,  64U,  67U,  70U,  74U,  77U,  80U,
     83U,  86U,  89U,  92U,  95U,  98U, 101U, 104U,
    107U, 110U, 113U, 116U, 119U, 121U, 124U, 127U,
    130U, 133U, 136U, 138U, 141U, 144U, 147U, 149U,
    152U, 155U, 157U, 160U, 163U, 165U, 168U, 170U,
    173U, 176U, 178U, 181U, 183U, 186U, 188U, 191U,
};

/* ============================================================================
 * Explicit 1D & 2D DMA Wrapper (Zero Global State, Profile Instrumented)
 * ============================================================================ */

typedef struct {
    const Z8Xm6DmaOps *ops;
    Z8Xm6HdfProfile   *prof;
} HdfDmaCtx;

static void hdf_dma_1d_r(const HdfDmaCtx *d, void *idm_dst, uint64_t ext_src_addr, uint32_t bytes)
{
    if (!bytes) return;
    if (d->ops && d->ops->dma_1d_ext_to_idm) {
        d->ops->dma_1d_ext_to_idm(d->ops->ctx, idm_dst, ext_src_addr, bytes);
    } else {
        memcpy(idm_dst, (const void *)(uintptr_t)ext_src_addr, bytes);
    }
    if (d->prof) {
        d->prof->dma_ext_to_idm_bytes += bytes;
        d->prof->dma_ext_to_idm_calls += 1U;
    }
}

static void hdf_dma_1d_w(const HdfDmaCtx *d, uint64_t ext_dst_addr, const void *idm_src, uint32_t bytes)
{
    if (!bytes) return;
    if (d->ops && d->ops->dma_1d_idm_to_ext) {
        d->ops->dma_1d_idm_to_ext(d->ops->ctx, ext_dst_addr, idm_src, bytes);
    } else {
        memcpy((void *)(uintptr_t)ext_dst_addr, idm_src, bytes);
    }
    if (d->prof) {
        d->prof->dma_idm_to_ext_bytes += bytes;
        d->prof->dma_idm_to_ext_calls += 1U;
    }
}

static void hdf_dma_2d_r(const HdfDmaCtx *d,
                         void *idm_dst, uint32_t idm_stride,
                         uint64_t ext_src_addr, uint32_t ext_stride,
                         uint32_t width_bytes, uint32_t height_rows)
{
    if (!width_bytes || !height_rows) return;
    if (d->ops && d->ops->dma_2d_ext_to_idm) {
        d->ops->dma_2d_ext_to_idm(d->ops->ctx, idm_dst, idm_stride,
                                  ext_src_addr, ext_stride, width_bytes, height_rows);
        if (d->prof) {
            d->prof->dma_ext_to_idm_bytes += (uint64_t)width_bytes * height_rows;
            d->prof->dma_ext_to_idm_calls += 1U;
        }
        return;
    }
    uint8_t *dst_p = (uint8_t *)idm_dst;
    for (uint32_t r = 0U; r < height_rows; ++r) {
        hdf_dma_1d_r(d, dst_p + (size_t)r * idm_stride,
                     ext_src_addr + (uint64_t)r * ext_stride, width_bytes);
    }
}

static void hdf_dma_2d_w(const HdfDmaCtx *d,
                         uint64_t ext_dst_addr, uint32_t ext_stride,
                         const void *idm_src, uint32_t idm_stride,
                         uint32_t width_bytes, uint32_t height_rows)
{
    if (!width_bytes || !height_rows) return;
    if (d->ops && d->ops->dma_2d_idm_to_ext) {
        d->ops->dma_2d_idm_to_ext(d->ops->ctx, ext_dst_addr, ext_stride,
                                  idm_src, idm_stride, width_bytes, height_rows);
        if (d->prof) {
            d->prof->dma_idm_to_ext_bytes += (uint64_t)width_bytes * height_rows;
            d->prof->dma_idm_to_ext_calls += 1U;
        }
        return;
    }
    const uint8_t *src_p = (const uint8_t *)idm_src;
    for (uint32_t r = 0U; r < height_rows; ++r) {
        hdf_dma_1d_w(d, ext_dst_addr + (uint64_t)r * ext_stride,
                     src_p + (size_t)r * idm_stride, width_bytes);
    }
}

/* ============================================================================
 * Portable Bit-Exact Fixed-Point Primitives (1:1 Map to CEVA-XM6 VPU Ops)
 * ============================================================================ */

static inline int32_t ashr_s32(int32_t x, uint32_t sh)
{
    if (x >= 0) return (int32_t)((uint32_t)x >> sh);
    return (int32_t)(~((~(uint32_t)x) >> sh));
}

static inline uint8_t clamp_u8(int32_t v)
{
    if (v < 0) return 0U;
    if (v > 255) return 255U;
    return (uint8_t)v;
}

static inline int16_t sqadd_s16(int16_t a, int16_t b)
{
    int32_t s = (int32_t)a + (int32_t)b;
    if (s > 32767) return 32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
}

/* Saturating rounding doubling multiply high (Q15 * Q15 -> Q15): maps to XM6 vmpyfr */
static inline int16_t sqrdmulh_s16(int16_t a, int16_t b)
{
    if (a == -32768 && b == -32768) return 32767;
    int64_t p = (int64_t)a * (int64_t)b * 2 + 32768;
    int64_t q = (p >= 0) ? (p / 65536) : -((-p + 65535) / 65536);
    return (int16_t)q;
}

static inline int16_t srshr1_s16(int16_t a)
{
    int32_t v = (int32_t)a + 1;
    return (int16_t)((v >= 0) ? (v / 2) : -((-v + 1) / 2));
}

static inline int16_t srshr2_s16(int16_t a)
{
    int32_t v = (int32_t)a + 2;
    return (int16_t)((v >= 0) ? (v / 4) : -((-v + 3) / 4));
}

static inline uint8_t uhadd_u8(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint32_t)a + (uint32_t)b) >> 1);
}

static inline uint8_t urhadd_u8(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint32_t)a + (uint32_t)b + 1U) >> 1);
}

static inline uint16_t uhadd_u16(uint16_t a, uint16_t b)
{
    return (uint16_t)(((uint32_t)a + (uint32_t)b) >> 1);
}

static inline uint16_t urhadd_u16(uint16_t a, uint16_t b)
{
    return (uint16_t)(((uint32_t)a + (uint32_t)b + 1U) >> 1);
}

static inline uint32_t clz16(uint16_t v)
{
    if (v == 0U) return 16U;
    uint32_t n = 0U;
    if ((v & 0xFF00U) == 0U) { n += 8U; v = (uint16_t)(v << 8); }
    if ((v & 0xF000U) == 0U) { n += 4U; v = (uint16_t)(v << 4); }
    if ((v & 0xC000U) == 0U) { n += 2U; v = (uint16_t)(v << 2); }
    if ((v & 0x8000U) == 0U) { n += 1U; }
    return n;
}

static inline uint32_t ctz32_portable(uint32_t v)
{
    uint32_t n = 0U;
    if ((v & 0x0000FFFFU) == 0U) { n += 16U; v >>= 16; }
    if ((v & 0x000000FFU) == 0U) { n +=  8U; v >>=  8; }
    if ((v & 0x0000000FU) == 0U) { n +=  4U; v >>=  4; }
    if ((v & 0x00000003U) == 0U) { n +=  2U; v >>=  2; }
    if ((v & 0x00000001U) == 0U) { n +=  1U; }
    return n;
}

static inline uint16_t knot16_eval(uint8_t x,
                                   const uint8_t blo[16],
                                   const uint8_t bhi[16],
                                   const uint8_t slp[16])
{
    uint32_t hi  = (uint32_t)x >> 4;
    uint32_t lo2 = ((uint32_t)x & 15U) << 1;
    uint32_t b   = (uint32_t)blo[hi] | ((uint32_t)bhi[hi] << 8);
    return (uint16_t)(b + (uint32_t)slp[hi] * lo2);
}

static inline uint8_t clz_umull64_lin15_to_srgb(uint16_t o)
{
    uint32_t lz  = clz16(o);
    uint16_t n   = (uint16_t)((uint32_t)o << lz);
    uint8_t n_hi = (uint8_t)(n >> 8);
    uint8_t m_idx = (uint8_t)(((uint8_t)(n_hi + n_hi)) >> 2);
    uint8_t bv   = (lz < 16U) ? k_clz_base[lz]  : 0U;
    uint8_t sv   = (lz < 16U) ? k_clz_scale[lz] : 0U;
    uint8_t dv   = k_clz_dm[m_idx];
    uint32_t d8  = ((uint32_t)sv * (uint32_t)dv + 128U) >> 8;
    uint32_t out = (uint32_t)bv + d8;
    return (uint8_t)((out > 255U) ? 255U : out);
}

/* Core Stage 3 pixel fusion: knot16 (1-k)*L + sk_q15 + cold shoulder + clz Lin->sRGB */
static inline uint8_t hdf_comp_px_q15(uint8_t vx, uint16_t sk, uint32_t lvl_idx)
{
    uint16_t t1 = knot16_eval(vx, k_s2l16_invk_blo[lvl_idx],
                              k_s2l16_invk_bhi[lvl_idx],
                              k_s2l16_invk_slp[lvl_idx]);
    uint16_t u15 = (uint16_t)(t1 + sk);
    uint16_t o15 = u15;
    if (u15 > 25600U) {
        uint16_t l15 = knot16_eval(vx, k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
        uint32_t pa  = 25600U;
        uint32_t uk2 = (u15 < 35840U) ? u15 : 35840U;
        uint32_t pb  = (uk2 - 25600U) >> 1;
        uint32_t utop = (u15 < 52224U) ? u15 : 52224U;
        uint32_t pc  = (utop > 35840U) ? ((utop - 35840U) >> 3) : 0U;
        uint16_t p15 = (uint16_t)(pa + pb + pc);
        o15 = (u15 > l15) ? ((l15 > p15) ? l15 : p15) : u15;
        if (o15 > 32767U) o15 = 32767U;
    }
    return clz_umull64_lin15_to_srgb(o15);
}

/* ============================================================================
 * Engine A: LIVE_256_YC (1024x{680,682} NV12) — Bit-Exact vs z8_hdf_live_nv12_1024
 * ============================================================================ */

typedef struct {
    uint16_t grid_r[HDF_LIVE_G0_CAP]; /* 87,552 B */
    uint16_t grid_g[HDF_LIVE_G0_CAP]; /* 87,552 B */
    uint16_t grid_b[HDF_LIVE_G0_CAP]; /* 87,552 B */
    uint16_t grid_y[HDF_LIVE_G0_CAP]; /* 87,552 B */
    uint8_t  r256[HDF_LIVE_G0_CAP];   /* 43,776 B */
    uint8_t  g256[HDF_LIVE_G0_CAP];   /* 43,776 B */
    uint8_t  b256[HDF_LIVE_G0_CAP];   /* 43,776 B */
    uint32_t c_edge_mask[172];        /*    688 B */
    uint32_t y_edge_mask[172];        /*    688 B */
} Xm6LiveV2ExtWs; /* 482,912 B <= 512 KiB FW_EXT_WORKBUF */

typedef union {
    struct {
        uint8_t  y_rows[2][4][HDF_LIVE_W]; /* 8,192 B ping-pong 2D strips */
        uint8_t  c_rows[2][2][HDF_LIVE_W]; /* 4,096 B ping-pong 2D strips */
        uint8_t  boost_lo[64];
        uint8_t  boost_hi[64];
        uint16_t gr_row[HDF_LIVE_GW];
        uint16_t gg_row[HDF_LIVE_GW];
        uint16_t gb_row[HDF_LIVE_GW];
        uint8_t  r256_row[HDF_LIVE_GW];
        uint8_t  g256_row[HDF_LIVE_GW];
        uint8_t  b256_row[HDF_LIVE_GW];
    } s1; /* 14,720 B */
    struct {
        uint16_t grid_ch[HDF_LIVE_G0_CAP]; /* 87,552 B (full channel in IDM!) */
        uint16_t lvl1[HDF_LIVE_G1_CAP];    /* 22,016 B */
        uint16_t lvl2[HDF_LIVE_G2_CAP];    /*  5,504 B */
        uint16_t box_tmp[HDF_LIVE_G1_CAP]; /* 22,016 B */
        int16_t  htmp[272];                /*    544 B */
        int16_t  vrow1[144];               /*    288 B */
        int16_t  vrow2[80];                /*    160 B */
    } s2; /* 138,080 B <= 192 KiB FW_APP_WORKBUF */
    struct {
        uint16_t r_row[HDF_LIVE_GW];
        uint16_t g_row[HDF_LIVE_GW];
        uint16_t b_row[HDF_LIVE_GW];
        uint16_t y_row[HDF_LIVE_GW];
    } s2_gy; /* 2,048 B: DMA-staged grid_y builder */
    struct {
        uint8_t  y_rows[2][4][HDF_LIVE_W]; /* 8,192 B ping-pong */
        uint8_t  c_rows[2][2][HDF_LIVE_W]; /* 4,096 B ping-pong */
        uint16_t gr_row[HDF_LIVE_GW];
        uint16_t gg_row[HDF_LIVE_GW];
        uint16_t gb_row[HDF_LIVE_GW];
        uint16_t gy_r0[HDF_LIVE_GW];
        uint16_t gy_r1[HDF_LIVE_GW];
        uint16_t vrow_y_512[HDF_LIVE_CW];
        uint8_t  r_in[HDF_LIVE_GW];
        uint8_t  g_in[HDF_LIVE_GW];
        uint8_t  b_in[HDF_LIVE_GW];
        uint8_t  r_out[HDF_LIVE_GW];
        uint8_t  g_out[HDF_LIVE_GW];
        uint8_t  b_out[HDF_LIVE_GW];
        uint8_t  exact_uv[2048];
    } s3; /* 19,456 B */
} Xm6LiveV2IdmWs;

typedef char z8_hdf_v2_chk_live_ext[(sizeof(Xm6LiveV2ExtWs) <= Z8_XM6_HDF_EXT_BYTES) ? 1 : -1];
typedef char z8_hdf_v2_chk_live_idm[(sizeof(Xm6LiveV2IdmWs) <= Z8_XM6_HDF_IDM_BYTES) ? 1 : -1];

typedef struct {
    uint32_t dst_h, src_h, den, right_lim, p, g, rem, inv_den;
} LiveVCoordStep;

static void live_vcoord_init(LiveVCoordStep *st, uint32_t dst_h, uint32_t src_h)
{
    st->dst_h     = dst_h;
    st->src_h     = src_h;
    st->den       = 2U * dst_h;
    st->right_lim = (2U * src_h - 1U) * dst_h;
    st->p         = src_h;
    st->g         = 0U;
    st->rem       = 0U;
    st->inv_den   = (uint32_t)((1ULL << 32) / st->den) + 1U;
}

static void live_vcoord_next(LiveVCoordStep *st, uint32_t *gy0, uint32_t *gy1, uint32_t *fy_q8)
{
    uint32_t p = st->p;
    st->p = p + 2U * st->src_h;
    if (p <= st->dst_h) {
        *gy0 = 0U; *gy1 = 0U; *fy_q8 = 0U;
        return;
    }
    if (p >= st->right_lim) {
        *gy0 = st->src_h - 1U; *gy1 = st->src_h - 1U; *fy_q8 = 0U;
        return;
    }
    uint32_t num = p - st->dst_h;
    uint32_t g   = st->g;
    uint32_t rem = num - g * st->den;
    if (rem >= st->den) {
        rem -= st->den;
        ++g;
    }
    st->g = g;
    uint32_t fy = (uint32_t)(((uint64_t)(rem * 256U + st->dst_h) * st->inv_den) >> 32);
    if (fy >= 256U) {
        g += 1U;
        fy = 0U;
    }
    if (g >= st->src_h - 1U) {
        *gy0 = st->src_h - 1U; *gy1 = st->src_h - 1U; *fy_q8 = 0U;
    } else {
        *gy0 = g; *gy1 = g + 1U; *fy_q8 = fy;
    }
}

static void live_s1_halfrow(const uint8_t *yr0, const uint8_t *yr1, const uint8_t *cr,
                            const uint8_t *boost_lo, const uint8_t *boost_hi,
                            uint32_t cx_base, uint32_t monochrome,
                            uint16_t *pr_out, uint16_t *pg_out, uint16_t *pb_out,
                            uint8_t r_px[2], uint8_t g_px[2], uint8_t b_px[2])
{
    uint16_t er[2], eg[2], eb[2];
    for (uint32_t j = 0U; j < 2U; ++j) {
        uint32_t cx = cx_base + j;
        uint8_t ys = uhadd_u8(urhadd_u8(yr0[2U * cx], yr0[2U * cx + 1U]),
                              urhadd_u8(yr1[2U * cx], yr1[2U * cx + 1U]));
        if (monochrome) {
            uint16_t lr = knot16_eval(ys, k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
            uint32_t bidx = (uint32_t)lr >> 9;
            int16_t bh = (int16_t)((uint16_t)boost_lo[bidx] | ((uint16_t)boost_hi[bidx] << 8));
            uint32_t v = (((uint32_t)lr + 2U) >> 2) + (uint16_t)sqrdmulh_s16(bh, (int16_t)lr);
            er[j] = (uint16_t)((v > 32767U) ? 32767U : v);
        } else {
            int16_t cb  = (int16_t)((int32_t)cr[2U * cx] - 128);
            int16_t crv = (int16_t)((int32_t)cr[2U * cx + 1U] - 128);
            int32_t dr = (int32_t)crv + sqrdmulh_s16(crv, (int16_t)(103 << 7));
            int32_t db = (int32_t)cb  + sqrdmulh_s16(cb,  (int16_t)(198 << 7));
            int32_t dg = -(sqrdmulh_s16(cb, (int16_t)(88 << 7)) +
                           sqrdmulh_s16(crv, (int16_t)(183 << 7)));
            uint8_t vr = clamp_u8((int32_t)ys + dr);
            uint8_t vg = clamp_u8((int32_t)ys + dg);
            uint8_t vb = clamp_u8((int32_t)ys + db);
            r_px[j] = vr; g_px[j] = vg; b_px[j] = vb;
            uint16_t lr = knot16_eval(vr, k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
            uint16_t lg = knot16_eval(vg, k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
            uint16_t lb = knot16_eval(vb, k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
            uint16_t lmax = (lr > lg) ? lr : lg;
            if (lb > lmax) lmax = lb;
            uint16_t llum = (uint16_t)sqadd_s16(
                sqadd_s16(sqrdmulh_s16((int16_t)lr, (int16_t)(77 << 7)),
                          sqrdmulh_s16((int16_t)lg, (int16_t)(150 << 7))),
                sqrdmulh_s16((int16_t)lb, (int16_t)(29 << 7)));
            uint16_t lpeak7 = (uint16_t)(lmax - (lmax >> 3));
            uint16_t ldrv = (llum >= lpeak7) ? llum : lpeak7;
            uint32_t bidx = (uint32_t)ldrv >> 9;
            int16_t bh = (int16_t)((uint16_t)boost_lo[bidx] | ((uint16_t)boost_hi[bidx] << 8));
            int16_t nt = sqrdmulh_s16((int16_t)ldrv, (int16_t)(96 << 7));
            int16_t mr = sqadd_s16(sqrdmulh_s16((int16_t)lr, (int16_t)(160 << 7)), nt);
            int16_t mg = sqadd_s16(sqrdmulh_s16((int16_t)lg, (int16_t)(160 << 7)), nt);
            int16_t mb = sqadd_s16(sqrdmulh_s16((int16_t)lb, (int16_t)(160 << 7)), nt);
            uint32_t vr_e = (((uint32_t)lr + 2U) >> 2) + (uint16_t)sqrdmulh_s16(bh, mr);
            uint32_t vg_e = (((uint32_t)lg + 2U) >> 2) + (uint16_t)sqrdmulh_s16(bh, mg);
            uint32_t vb_e = (((uint32_t)lb + 2U) >> 2) + (uint16_t)sqrdmulh_s16(bh, mb);
            er[j] = (uint16_t)((vr_e > 32767U) ? 32767U : vr_e);
            eg[j] = (uint16_t)((vg_e > 32767U) ? 32767U : vg_e);
            eb[j] = (uint16_t)((vb_e > 32767U) ? 32767U : vb_e);
        }
    }
    *pr_out = (uint16_t)(er[0] + er[1]);
    if (!monochrome) {
        *pg_out = (uint16_t)(eg[0] + eg[1]);
        *pb_out = (uint16_t)(eb[0] + eb[1]);
    }
}

/* XM6 VPU Target: Live Stage 1 4x4 Downsample + Q15 Highlight Extract + Edge Masks */
static void live_s1_rowpair_256(const uint8_t *y0, const uint8_t *y1, const uint8_t *c0,
                                const uint8_t *y2, const uint8_t *y3, const uint8_t *c1,
                                const uint8_t *boost_lo, const uint8_t *boost_hi,
                                uint16_t *gr_out, uint16_t *gg_out, uint16_t *gb_out,
                                uint8_t *r256_out, uint8_t *g256_out, uint8_t *b256_out,
                                uint32_t *c_edge_mask_out, uint32_t *y_edge_mask_out,
                                uint32_t monochrome)
{
    if (monochrome) {
        *c_edge_mask_out = 0U;
        *y_edge_mask_out = 0U;
        for (uint32_t gx = 0U; gx < HDF_LIVE_GW; ++gx) {
            uint16_t p0r = 0U, p0g = 0U, p0b = 0U;
            uint8_t dummy[2];
            live_s1_halfrow(y0, y1, c0, boost_lo, boost_hi, 2U * gx, 1U,
                            &p0r, &p0g, &p0b, dummy, dummy, dummy);
            if (y2) {
                uint16_t p1r = 0U, p1g = 0U, p1b = 0U;
                live_s1_halfrow(y2, y3, c1, boost_lo, boost_hi, 2U * gx, 1U,
                                &p1r, &p1g, &p1b, dummy, dummy, dummy);
                gr_out[gx] = (uint16_t)(urhadd_u16(p0r, p1r) >> 1);
            } else {
                gr_out[gx] = (uint16_t)((p0r + 1U) >> 1);
            }
        }
        return;
    }

    uint32_t c_mask = 0U, y_mask = 0U;
    for (uint32_t chunk = 0U; chunk < HDF_LIVE_GW / 8U; ++chunk) {
        uint8_t ys0_a[16], ys1_a[16], cb0_a[16], cr0_a[16], cb1_a[16], cr1_a[16];
        uint32_t c_span_max = 0U, y_span_max = 0U, sat_max = 0U, y_max_all = 0U;
        for (uint32_t j = 0U; j < 8U; ++j) {
            uint32_t gx = chunk * 8U + j;
            for (uint32_t d = 0U; d < 2U; ++d) {
                uint32_t cx  = 2U * gx + d;
                uint32_t idx = 2U * j + d;
                ys0_a[idx] = uhadd_u8(urhadd_u8(y0[2U * cx], y0[2U * cx + 1U]),
                                      urhadd_u8(y1[2U * cx], y1[2U * cx + 1U]));
                cb0_a[idx] = c0[2U * cx];
                cr0_a[idx] = c0[2U * cx + 1U];
                if (y2) {
                    ys1_a[idx] = uhadd_u8(urhadd_u8(y2[2U * cx], y2[2U * cx + 1U]),
                                          urhadd_u8(y3[2U * cx], y3[2U * cx + 1U]));
                    cb1_a[idx] = c1[2U * cx];
                    cr1_a[idx] = c1[2U * cx + 1U];
                }
            }
            uint32_t cbmin = 255U, cbmax = 0U, crmin = 255U, crmax = 0U, ymin = 255U, ymax = 0U;
            uint32_t n_s = y2 ? 4U : 2U;
            uint8_t cbs[4] = { cb0_a[2U*j], cb0_a[2U*j+1U], y2 ? cb1_a[2U*j] : 0U, y2 ? cb1_a[2U*j+1U] : 0U };
            uint8_t crs[4] = { cr0_a[2U*j], cr0_a[2U*j+1U], y2 ? cr1_a[2U*j] : 0U, y2 ? cr1_a[2U*j+1U] : 0U };
            uint8_t yss[4] = { ys0_a[2U*j], ys0_a[2U*j+1U], y2 ? ys1_a[2U*j] : 0U, y2 ? ys1_a[2U*j+1U] : 0U };
            for (uint32_t k = 0U; k < n_s; ++k) {
                if (cbs[k] < cbmin) cbmin = cbs[k];
                if (cbs[k] > cbmax) cbmax = cbs[k];
                if (crs[k] < crmin) crmin = crs[k];
                if (crs[k] > crmax) crmax = crs[k];
                if (yss[k] < ymin)  ymin  = yss[k];
                if (yss[k] > ymax)  ymax  = yss[k];
            }
            if (ymax > y_max_all) y_max_all = ymax;
            uint32_t cs = ((cbmax - cbmin) > (crmax - crmin)) ? (cbmax - cbmin) : (crmax - crmin);
            uint32_t yd = ymax - ymin;
            if (cs > c_span_max) c_span_max = cs;
            if (yd > y_span_max) y_span_max = yd;
            uint32_t s0 = (cbmax >= 128U) ? (cbmax - 128U) : (128U - cbmax);
            uint32_t s1 = (cbmin >= 128U) ? (cbmin - 128U) : (128U - cbmin);
            uint32_t s2 = (crmax >= 128U) ? (crmax - 128U) : (128U - crmax);
            uint32_t s3 = (crmin >= 128U) ? (crmin - 128U) : (128U - crmin);
            if (s0 > sat_max) sat_max = s0;
            if (s1 > sat_max) sat_max = s1;
            if (s2 > sat_max) sat_max = s2;
            if (s3 > sat_max) sat_max = s3;
        }
        if (y_span_max > 48U) y_mask |= (1U << chunk);
        if (c_span_max > 16U || (y_span_max > 48U && sat_max > 16U)) c_mask |= (1U << chunk);

        if ((y_max_all + 2U * sat_max <= 158U) || (y_span_max <= 40U && c_span_max <= 16U)) {
            uint8_t r8_a[8], g8_a[8], b8_a[8];
            uint32_t max_rgb = 0U;
            for (uint32_t j = 0U; j < 8U; ++j) {
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
                int32_t dr = (int32_t)crv + sqrdmulh_s16(crv, (int16_t)(103 << 7));
                int32_t db = (int32_t)cb  + sqrdmulh_s16(cb,  (int16_t)(198 << 7));
                int32_t dg = -(sqrdmulh_s16(cb, (int16_t)(88 << 7)) +
                               sqrdmulh_s16(crv, (int16_t)(183 << 7)));
                uint8_t r8 = clamp_u8((int32_t)y256 + dr);
                uint8_t g8 = clamp_u8((int32_t)y256 + dg);
                uint8_t b8 = clamp_u8((int32_t)y256 + db);
                r8_a[j] = r8; g8_a[j] = g8; b8_a[j] = b8;
                r256_out[gx] = r8; g256_out[gx] = g8; b256_out[gx] = b8;
                if (r8 > max_rgb) max_rgb = r8;
                if (g8 > max_rgb) max_rgb = g8;
                if (b8 > max_rgb) max_rgb = b8;
            }
            if (max_rgb <= 158U) {
                for (uint32_t j = 0U; j < 8U; ++j) {
                    uint32_t gx = chunk * 8U + j;
                    gr_out[gx] = (uint16_t)((knot16_eval(r8_a[j], k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp) + 2U) >> 2);
                    gg_out[gx] = (uint16_t)((knot16_eval(g8_a[j], k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp) + 2U) >> 2);
                    gb_out[gx] = (uint16_t)((knot16_eval(b8_a[j], k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp) + 2U) >> 2);
                }
            } else {
                for (uint32_t j = 0U; j < 8U; ++j) {
                    uint32_t gx = chunk * 8U + j;
                    uint16_t lr = knot16_eval(r8_a[j], k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
                    uint16_t lg = knot16_eval(g8_a[j], k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
                    uint16_t lb = knot16_eval(b8_a[j], k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
                    uint16_t lmax = (lr > lg) ? lr : lg;
                    if (lb > lmax) lmax = lb;
                    uint16_t llum = (uint16_t)sqadd_s16(
                        sqadd_s16(sqrdmulh_s16((int16_t)lr, (int16_t)(77 << 7)),
                                  sqrdmulh_s16((int16_t)lg, (int16_t)(150 << 7))),
                        sqrdmulh_s16((int16_t)lb, (int16_t)(29 << 7)));
                    uint16_t ldrv = (uint16_t)(lmax - (lmax >> 3));
                    if (llum > ldrv) ldrv = llum;
                    uint32_t bidx = (uint32_t)ldrv >> 9;
                    int16_t bh = (int16_t)((uint16_t)boost_lo[bidx] | ((uint16_t)boost_hi[bidx] << 8));
                    int16_t nt = sqrdmulh_s16((int16_t)ldrv, (int16_t)(96 << 7));
                    uint32_t er = (((uint32_t)lr + 2U) >> 2) +
                        (uint16_t)sqrdmulh_s16(bh, sqadd_s16(sqrdmulh_s16((int16_t)lr, (int16_t)(160 << 7)), nt));
                    uint32_t eg = (((uint32_t)lg + 2U) >> 2) +
                        (uint16_t)sqrdmulh_s16(bh, sqadd_s16(sqrdmulh_s16((int16_t)lg, (int16_t)(160 << 7)), nt));
                    uint32_t eb = (((uint32_t)lb + 2U) >> 2) +
                        (uint16_t)sqrdmulh_s16(bh, sqadd_s16(sqrdmulh_s16((int16_t)lb, (int16_t)(160 << 7)), nt));
                    gr_out[gx] = (uint16_t)((er > 32767U) ? 32767U : er);
                    gg_out[gx] = (uint16_t)((eg > 32767U) ? 32767U : eg);
                    gb_out[gx] = (uint16_t)((eb > 32767U) ? 32767U : eb);
                }
            }
        } else {
            for (uint32_t j = 0U; j < 8U; ++j) {
                uint32_t gx = chunk * 8U + j;
                uint16_t p0r = 0U, p0g = 0U, p0b = 0U;
                uint8_t r0[2] = {0, 0}, g0[2] = {0, 0}, b0[2] = {0, 0};
                live_s1_halfrow(y0, y1, c0, boost_lo, boost_hi, 2U * gx, 0U,
                                &p0r, &p0g, &p0b, r0, g0, b0);
                if (y2) {
                    uint16_t p1r = 0U, p1g = 0U, p1b = 0U;
                    uint8_t r1[2] = {0, 0}, g1[2] = {0, 0}, b1[2] = {0, 0};
                    live_s1_halfrow(y2, y3, c1, boost_lo, boost_hi, 2U * gx, 0U,
                                    &p1r, &p1g, &p1b, r1, g1, b1);
                    gr_out[gx] = (uint16_t)(urhadd_u16(p0r, p1r) >> 1);
                    gg_out[gx] = (uint16_t)(urhadd_u16(p0g, p1g) >> 1);
                    gb_out[gx] = (uint16_t)(urhadd_u16(p0b, p1b) >> 1);
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
}

static void live_ds2x2_u16(const uint16_t *src, uint32_t sw, uint32_t sh,
                           uint16_t *dst, uint32_t dw, uint32_t dh)
{
    for (uint32_t y = 0U; y < dh; ++y) {
        uint32_t y0 = 2U * y;
        uint32_t y1 = (y0 + 1U < sh) ? (y0 + 1U) : (sh - 1U);
        const uint16_t *r0 = src + (size_t)y0 * sw;
        const uint16_t *r1 = src + (size_t)y1 * sw;
        uint16_t *out = dst + (size_t)y * dw;
        for (uint32_t x = 0U; x < dw; ++x) {
            out[x] = uhadd_u16(urhadd_u16(r0[2U * x], r0[2U * x + 1U]),
                               urhadd_u16(r1[2U * x], r1[2U * x + 1U]));
        }
    }
}

static void live_blur_sym53_2d(uint16_t *grid, uint16_t *tmp, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0U; y < h; ++y) {
        uint16_t *row = grid + (size_t)y * w;
        for (uint32_t x = 0U; x < w; ++x) {
            uint16_t l2 = row[(x >= 2U) ? (x - 2U) : 0U];
            uint16_t l1 = row[(x >= 1U) ? (x - 1U) : 0U];
            uint16_t c  = row[x];
            uint16_t r1 = row[(x + 1U < w) ? (x + 1U) : (w - 1U)];
            uint16_t r2 = row[(x + 2U < w) ? (x + 2U) : (w - 1U)];
            tmp[x] = urhadd_u16(uhadd_u16(urhadd_u16(l2, r2), c), urhadd_u16(l1, r1));
        }
        for (uint32_t x = 0U; x < w; ++x) {
            uint16_t l1 = tmp[(x >= 1U) ? (x - 1U) : 0U];
            uint16_t c  = tmp[x];
            uint16_t r1 = tmp[(x + 1U < w) ? (x + 1U) : (w - 1U)];
            row[x] = uhadd_u16(c, urhadd_u16(l1, r1));
        }
    }
    for (uint32_t x = 0U; x < w; ++x) {
        for (uint32_t y = 0U; y < h; ++y) {
            uint16_t l2 = grid[(size_t)((y >= 2U) ? (y - 2U) : 0U) * w + x];
            uint16_t l1 = grid[(size_t)((y >= 1U) ? (y - 1U) : 0U) * w + x];
            uint16_t c  = grid[(size_t)y * w + x];
            uint16_t r1 = grid[(size_t)((y + 1U < h) ? (y + 1U) : (h - 1U)) * w + x];
            uint16_t r2 = grid[(size_t)((y + 2U < h) ? (y + 2U) : (h - 1U)) * w + x];
            tmp[y] = urhadd_u16(uhadd_u16(urhadd_u16(l2, r2), c), urhadd_u16(l1, r1));
        }
        for (uint32_t y = 0U; y < h; ++y) {
            uint16_t l1 = tmp[(y >= 1U) ? (y - 1U) : 0U];
            uint16_t c  = tmp[y];
            uint16_t r1 = tmp[(y + 1U < h) ? (y + 1U) : (h - 1U)];
            grid[(size_t)y * w + x] = uhadd_u16(c, urhadd_u16(l1, r1));
        }
    }
}

static void live_blur_box16_1pass(uint16_t *grid, uint16_t *tmp_grid, int16_t *htmp,
                                  uint32_t w, uint32_t h, uint32_t r)
{
    uint32_t kt   = 2U * r + 1U;
    int16_t inv_k = (int16_t)(32767U / kt);
    for (uint32_t y = 0U; y < h; ++y) {
        const uint16_t *row = grid + (size_t)y * w;
        uint16_t *dst_row   = tmp_grid + (size_t)y * w;
        for (uint32_t x = 0U; x < w; ++x) {
            htmp[8U + x] = sqrdmulh_s16((int16_t)row[x], inv_k);
        }
        int16_t first = htmp[8U], last = htmp[8U + w - 1U];
        for (uint32_t p = 0U; p < 8U; ++p) htmp[p] = first;
        for (uint32_t p = 0U; p < 16U; ++p) htmp[8U + w + p] = last;
        const int16_t *base = htmp + 8U - r;
        for (uint32_t x = 0U; x < w; ++x) {
            int32_t sum = 0;
            for (uint32_t t = 0U; t < kt; ++t) sum += (int32_t)base[x + t];
            dst_row[x] = (uint16_t)sqrdmulh_s16((int16_t)sum, inv_k);
        }
    }
    for (uint32_t x = 0U; x < w; ++x) {
        int32_t sum = 0;
        for (int32_t d = -(int32_t)r; d <= (int32_t)r; ++d) {
            uint32_t ry = (d < 0) ? 0U : (((uint32_t)d >= h) ? (h - 1U) : (uint32_t)d);
            sum += (int32_t)(int16_t)tmp_grid[(size_t)ry * w + x];
        }
        grid[x] = (uint16_t)(int16_t)sum;
        for (uint32_t y = 1U; y < h; ++y) {
            int32_t li = (int32_t)y - 1 - (int32_t)r;
            int32_t ei = (int32_t)y + (int32_t)r;
            uint32_t yl = (li < 0) ? 0U : (((uint32_t)li >= h) ? (h - 1U) : (uint32_t)li);
            uint32_t ye = (ei < 0) ? 0U : (((uint32_t)ei >= h) ? (h - 1U) : (uint32_t)ei);
            sum += (int32_t)(int16_t)tmp_grid[(size_t)ye * w + x]
                 - (int32_t)(int16_t)tmp_grid[(size_t)yl * w + x];
            grid[(size_t)y * w + x] = (uint16_t)(int16_t)sum;
        }
    }
}

static void live_vlerp_q13_padded(const uint16_t *r0, const uint16_t *r1,
                                  int16_t *buf_with_pad, uint32_t sw, uint32_t fy_q8)
{
    int16_t *out = buf_with_pad + 8U;
    int16_t fy15 = (int16_t)(fy_q8 << 7);
    for (uint32_t x = 0U; x < sw; ++x) {
        int16_t a = (int16_t)r0[x];
        int16_t b = (int16_t)r1[x];
        out[x] = (int16_t)(a + sqrdmulh_s16((int16_t)(b - a), fy15));
    }
    out[-1] = out[0];
    int16_t last = out[sw - 1U];
    for (uint32_t p = 0U; p < 8U; ++p) out[sw + p] = last;
}

static void live_s2_blend_k_row_256(uint16_t *row0,
                                    const int16_t *vrow1_padded, const int16_t *vrow2_padded,
                                    int16_t w1k, int16_t w2k, int16_t w3k)
{
    static const int16_t w4_tbl[4] = { 20480, 28672, 4096, 12288 };
    const int16_t *vr1 = vrow1_padded + 8U;
    const int16_t *vr2 = vrow2_padded + 8U;
    for (uint32_t x = 0U; x < HDF_LIVE_GW; ++x) {
        int32_t k = (int32_t)(x >> 1);
        uint16_t cur = (uint16_t)vr1[k];
        uint16_t nbr = (uint16_t)vr1[k + ((x & 1U) ? 1 : -1)];
        int16_t s2 = (int16_t)urhadd_u16(cur, uhadd_u16(cur, nbr));

        int32_t g2 = (int32_t)(x >> 2) - ((x & 2U) ? 0 : 1);
        int16_t a2 = vr2[g2], b2 = vr2[g2 + 1];
        int16_t s3 = (int16_t)(a2 + sqrdmulh_s16((int16_t)(b2 - a2), w4_tbl[x & 3U]));

        int16_t a = (int16_t)row0[x];
        int16_t mix = sqadd_s16(
            sqadd_s16(sqrdmulh_s16(a, w1k), sqrdmulh_s16(s2, w2k)),
            sqrdmulh_s16(s3, w3k));
        row0[x] = (uint16_t)mix;
    }
}

/* Stage 2 executes 100% inside zero-wait-state IDM for each channel! */
static void live_build_channel_pyramid_idm(Xm6LiveV2IdmWs *idm, uint16_t *ext_grid,
                                           uint32_t gh, const HdfPreset *pr,
                                           const HdfDmaCtx *dma)
{
    const uint32_t gw = HDF_LIVE_GW;
    const uint32_t gw1 = 128U, gh1 = (gh + 1U) >> 1;
    const uint32_t gw2 = 64U,  gh2 = (gh1 + 1U) >> 1;
    uint32_t g0_bytes = gw * gh * 2U;

    hdf_dma_1d_r(dma, idm->s2.grid_ch, (uint64_t)(uintptr_t)ext_grid, g0_bytes);

    live_ds2x2_u16(idm->s2.grid_ch, gw, gh, idm->s2.lvl1, gw1, gh1);
    live_ds2x2_u16(idm->s2.lvl1, gw1, gh1, idm->s2.lvl2, gw2, gh2);

    live_blur_sym53_2d(idm->s2.grid_ch, (uint16_t *)idm->s2.htmp, gw, gh);
    live_blur_box16_1pass(idm->s2.lvl1, idm->s2.box_tmp, idm->s2.htmp, gw1, gh1, 3U);
    live_blur_box16_1pass(idm->s2.lvl1, idm->s2.box_tmp, idm->s2.htmp, gw1, gh1, 4U);
    live_blur_box16_1pass(idm->s2.lvl2, idm->s2.box_tmp, idm->s2.htmp, gw2, gh2, 5U);
    live_blur_box16_1pass(idm->s2.lvl2, idm->s2.box_tmp, idm->s2.htmp, gw2, gh2, 5U);

    int16_t w1k = (int16_t)(((uint32_t)pr->w1 * (uint32_t)pr->k_q12 + 4U) >> 3);
    int16_t w2k = (int16_t)(((uint32_t)pr->w2 * (uint32_t)pr->k_q12 + 4U) >> 3);
    int16_t w3k = (int16_t)(((uint32_t)pr->w3 * (uint32_t)pr->k_q12 + 4U) >> 3);

    LiveVCoordStep vc1, vc2;
    live_vcoord_init(&vc1, gh, gh1);
    live_vcoord_init(&vc2, gh, gh2);
    for (uint32_t y = 0U; y < gh; ++y) {
        uint32_t y1_0, y1_1, fy1, y2_0, y2_1, fy2;
        live_vcoord_next(&vc1, &y1_0, &y1_1, &fy1);
        live_vcoord_next(&vc2, &y2_0, &y2_1, &fy2);
        live_vlerp_q13_padded(idm->s2.lvl1 + (size_t)y1_0 * gw1,
                              idm->s2.lvl1 + (size_t)y1_1 * gw1,
                              idm->s2.vrow1, gw1, fy1);
        live_vlerp_q13_padded(idm->s2.lvl2 + (size_t)y2_0 * gw2,
                              idm->s2.lvl2 + (size_t)y2_1 * gw2,
                              idm->s2.vrow2, gw2, fy2);
        live_s2_blend_k_row_256(idm->s2.grid_ch + (size_t)y * gw,
                                idm->s2.vrow1, idm->s2.vrow2, w1k, w2k, w3k);
    }

    hdf_dma_1d_w(dma, (uint64_t)(uintptr_t)ext_grid, idm->s2.grid_ch, g0_bytes);
}

static uint32_t live_s3_check_schroma_mask(const uint16_t *gr, const uint16_t *gg,
                                           const uint16_t *gb, uint32_t y_check_mask)
{
    uint32_t out = 0U;
    uint32_t m = y_check_mask;
    while (m != 0U) {
        uint32_t k = ctz32_portable(m);
        m &= m - 1U;
        uint32_t off = k << 3;
        uint32_t max_d = 0U;
        for (uint32_t j = 0U; j < 8U; ++j) {
            uint32_t r = gr[off + j], g = gg[off + j], b = gb[off + j];
            uint32_t mx = (r > g) ? r : g;
            if (b > mx) mx = b;
            uint32_t mn = (r < g) ? r : g;
            if (b < mn) mn = b;
            if (mx - mn > max_d) max_d = mx - mn;
        }
        if (max_d > 384U) out |= (1U << k);
    }
    return out;
}

static void live_s3_chroma_exact_masked(const uint8_t *y0, const uint8_t *y1, uint8_t *c0,
                                        const uint8_t *y2, const uint8_t *y3, uint8_t *c1,
                                        const uint16_t *gr, const uint16_t *gg, const uint16_t *gb,
                                        uint32_t mask, uint32_t lvl_idx, uint8_t *exact_uv_out)
{
    uint32_t n_rows = c1 ? 2U : 1U;
    uint32_t m = mask;
    uint32_t i = 0U;
    while (m != 0U) {
        uint32_t chunk = ctz32_portable(m);
        m &= m - 1U;
        uint32_t cx_base = chunk << 4;
        for (uint32_t row = 0U; row < n_rows; ++row) {
            const uint8_t *yr0 = (row == 0U) ? y0 : y2;
            const uint8_t *yr1 = (row == 0U) ? y1 : y3;
            uint8_t *cr = (row == 0U) ? c0 : c1;
            uint8_t *cdst = exact_uv_out ? (exact_uv_out + (size_t)i * 64U + (size_t)row * 32U) : NULL;
            for (uint32_t d = 0U; d < 16U; ++d) {
                uint32_t cx = cx_base + d;
                uint32_t gx = cx >> 1;
                uint8_t yh = uhadd_u8(urhadd_u8(yr0[2U * cx], yr0[2U * cx + 1U]),
                                      urhadd_u8(yr1[2U * cx], yr1[2U * cx + 1U]));
                int16_t cb  = (int16_t)((int32_t)cr[2U * cx] - 128);
                int16_t crv = (int16_t)((int32_t)cr[2U * cx + 1U] - 128);
                int32_t dr = (int32_t)crv + sqrdmulh_s16(crv, (int16_t)(103 << 7));
                int32_t db = (int32_t)cb  + sqrdmulh_s16(cb,  (int16_t)(198 << 7));
                int32_t dg = -(sqrdmulh_s16(cb, (int16_t)(88 << 7)) +
                               sqrdmulh_s16(crv, (int16_t)(183 << 7)));
                uint8_t ri = clamp_u8((int32_t)yh + dr);
                uint8_t gi = clamp_u8((int32_t)yh + dg);
                uint8_t bi = clamp_u8((int32_t)yh + db);
                uint8_t ro = hdf_comp_px_q15(ri, gr[gx], lvl_idx);
                uint8_t go = hdf_comp_px_q15(gi, gg[gx], lvl_idx);
                uint8_t bo = hdf_comp_px_q15(bi, gb[gx], lvl_idx);
                int16_t dR = (int16_t)((int32_t)ro - (int32_t)ri);
                int16_t dG = (int16_t)((int32_t)go - (int32_t)gi);
                int16_t dB = (int16_t)((int32_t)bo - (int32_t)bi);
                int32_t dcb = srshr1_s16(dB)
                            - sqrdmulh_s16(dR, (int16_t)(43 << 7))
                            - sqrdmulh_s16(dG, (int16_t)(85 << 7));
                int32_t dcr = srshr1_s16(dR)
                            - sqrdmulh_s16(dG, (int16_t)(107 << 7))
                            - sqrdmulh_s16(dB, (int16_t)(21 << 7));
                uint8_t out_cb = clamp_u8((int32_t)cr[2U * cx] + dcb);
                uint8_t out_cr = clamp_u8((int32_t)cr[2U * cx + 1U] + dcr);
                if (cdst) {
                    cdst[2U * d]      = out_cb;
                    cdst[2U * d + 1U] = out_cr;
                } else {
                    cr[2U * cx]      = out_cb;
                    cr[2U * cx + 1U] = out_cr;
                }
            }
        }
        ++i;
    }
}

static void live_vlerp_hlerp_y_512(const uint16_t *r0, const uint16_t *r1,
                                   uint16_t *out_512, uint32_t fy_q8)
{
    uint16_t tmp[HDF_LIVE_GW];
    int16_t fy15 = (int16_t)(fy_q8 << 7);
    for (uint32_t x = 0U; x < HDF_LIVE_GW; ++x) {
        int16_t a = (int16_t)r0[x], b = (int16_t)r1[x];
        tmp[x] = (uint16_t)(int16_t)(a + sqrdmulh_s16((int16_t)(b - a), fy15));
    }
    for (uint32_t k = 0U; k < HDF_LIVE_GW; ++k) {
        uint16_t cur = tmp[k];
        uint16_t l   = tmp[(k > 0U) ? (k - 1U) : 0U];
        uint16_t r   = tmp[(k + 1U < HDF_LIVE_GW) ? (k + 1U) : (HDF_LIVE_GW - 1U)];
        out_512[2U * k]      = urhadd_u16(cur, uhadd_u16(cur, l));
        out_512[2U * k + 1U] = urhadd_u16(cur, uhadd_u16(cur, r));
    }
}

static int32_t run_live_256_yc(const Z8Xm6HdfParams *in,
                               const Z8Xm6HdfOutParam *out,
                               Xm6LiveV2IdmWs *idm,
                               Xm6LiveV2ExtWs *ext,
                               const HdfDmaCtx *dma,
                               Z8Xm6HdfProfile *prof)
{
    uint32_t height  = (uint32_t)in->src.height;
    uint32_t ch      = height >> 1;
    uint32_t gw      = HDF_LIVE_GW;
    uint32_t gh      = (ch + 1U) >> 1;
    uint32_t level   = in->level;
    uint32_t lvl_idx = (level >= 1U && level <= 3U) ? (level - 1U) : 0U;
    uint32_t mono    = in->monochrome ? 1U : 0U;
    const HdfPreset *pr = &k_hdf_presets[level];

    uint64_t sy_base = in->src.plane_addr[0], sc_base = in->src.plane_addr[1];
    uint64_t dy_base = out->plane_addr[0],    dc_base = out->plane_addr[1];
    uint32_t sy_st   = (uint32_t)in->src.stride[0], sc_st = (uint32_t)in->src.stride[1];
    uint32_t dy_st   = (uint32_t)out->stride[0],    dc_st = (uint32_t)out->stride[1];

    if (prof) {
        prof->engine_used = Z8_XM6_HDF_MODE_LIVE_256_YC;
        prof->grid_b      = 4U;
        prof->s1_step     = 2U;
        prof->gw          = gw;
        prof->gh          = gh;
        prof->r1          = 1U;
        prof->r2          = 4U;
        prof->r3          = 5U;
    }

    uint64_t c_s1_start = xm6_read_cycles();

    /* 1. Precompute 64-entry Q13 highlight boost LUT */
    for (uint32_t k = 0U; k < 64U; ++k) {
        uint32_t ldrv  = k * 1024U + 512U;
        if (ldrv > 65535U) ldrv = 65535U;
        uint32_t over  = (ldrv > pr->knee_q16) ? (ldrv - pr->knee_q16) : 0U;
        uint32_t t     = (over * 1490U) >> 16;
        uint32_t quad  = (t * t) >> 10;
        uint32_t cover = (ldrv > HDF_CORE_KNEE_Q16) ? (ldrv - HDF_CORE_KNEE_Q16) : 0U;
        uint32_t tc    = (cover * 5U) >> 6;
        uint32_t core  = (tc * tc) >> 10;
        uint32_t raw   = quad * pr->boost_quad_q14 + core * pr->boost_core_q14;
        uint32_t boost = ((raw >> 10) * 256U + 128U) >> 8;
        if (boost > 49151U) boost = 49151U;
        uint32_t bh    = boost >> 1;
        idm->s1.boost_lo[k] = (uint8_t)(bh & 0xFFU);
        idm->s1.boost_hi[k] = (uint8_t)(bh >> 8);
    }

    /* 2. Stage 1: Ping-pong 2D DMA of 4 Y rows + 2 C rows -> 256x{170,171} Q13 scatter grids */
    for (uint32_t gy = 0U; gy < gh; ++gy) {
        uint32_t bank     = gy & 1U;
        uint32_t cy0      = gy * 2U;
        uint32_t has_pair = (cy0 + 1U < ch) ? 1U : 0U;
        uint32_t y_rows_n = has_pair ? 4U : 2U;
        uint32_t c_rows_n = has_pair ? 2U : 1U;

        hdf_dma_2d_r(dma, idm->s1.y_rows[bank][0], HDF_LIVE_W,
                     sy_base + (uint64_t)(2U * cy0) * sy_st, sy_st,
                     HDF_LIVE_W, y_rows_n);
        if (!mono) {
            hdf_dma_2d_r(dma, idm->s1.c_rows[bank][0], HDF_LIVE_W,
                         sc_base + (uint64_t)cy0 * sc_st, sc_st,
                         HDF_LIVE_W, c_rows_n);
        }

        uint32_t cmask = 0U, ymask = 0U;
        live_s1_rowpair_256(idm->s1.y_rows[bank][0], idm->s1.y_rows[bank][1], idm->s1.c_rows[bank][0],
                            has_pair ? idm->s1.y_rows[bank][2] : NULL,
                            has_pair ? idm->s1.y_rows[bank][3] : NULL,
                            (has_pair && !mono) ? idm->s1.c_rows[bank][1] : NULL,
                            idm->s1.boost_lo, idm->s1.boost_hi,
                            idm->s1.gr_row, idm->s1.gg_row, idm->s1.gb_row,
                            idm->s1.r256_row, idm->s1.g256_row, idm->s1.b256_row,
                            &cmask, &ymask, mono);

        size_t goff = (size_t)gy * gw;
        hdf_dma_1d_w(dma, (uint64_t)(uintptr_t)(ext->grid_r + goff), idm->s1.gr_row, gw * 2U);
        if (!mono) {
            hdf_dma_1d_w(dma, (uint64_t)(uintptr_t)(ext->grid_g + goff), idm->s1.gg_row, gw * 2U);
            hdf_dma_1d_w(dma, (uint64_t)(uintptr_t)(ext->grid_b + goff), idm->s1.gb_row, gw * 2U);
            hdf_dma_1d_w(dma, (uint64_t)(uintptr_t)(ext->r256 + goff), idm->s1.r256_row, gw);
            hdf_dma_1d_w(dma, (uint64_t)(uintptr_t)(ext->g256 + goff), idm->s1.g256_row, gw);
            hdf_dma_1d_w(dma, (uint64_t)(uintptr_t)(ext->b256 + goff), idm->s1.b256_row, gw);
        }
        ext->c_edge_mask[gy] = cmask;
        ext->y_edge_mask[gy] = ymask;
    }

    uint64_t c_s2_start = xm6_read_cycles();

    /* 3. Stage 2: 3-Scale IDM-Resident Optical Pyramid + k Fold + DMA-Staged grid_y */
    live_build_channel_pyramid_idm(idm, ext->grid_r, gh, pr, dma);
    if (!mono) {
        live_build_channel_pyramid_idm(idm, ext->grid_g, gh, pr, dma);
        live_build_channel_pyramid_idm(idm, ext->grid_b, gh, pr, dma);
        for (uint32_t y = 0U; y < gh; ++y) {
            size_t goff = (size_t)y * gw;
            hdf_dma_1d_r(dma, idm->s2_gy.r_row, (uint64_t)(uintptr_t)(ext->grid_r + goff), gw * 2U);
            hdf_dma_1d_r(dma, idm->s2_gy.g_row, (uint64_t)(uintptr_t)(ext->grid_g + goff), gw * 2U);
            hdf_dma_1d_r(dma, idm->s2_gy.b_row, (uint64_t)(uintptr_t)(ext->grid_b + goff), gw * 2U);
            for (uint32_t x = 0U; x < gw; ++x) {
                int16_t r = (int16_t)idm->s2_gy.r_row[x];
                int16_t g = (int16_t)idm->s2_gy.g_row[x];
                int16_t b = (int16_t)idm->s2_gy.b_row[x];
                idm->s2_gy.y_row[x] = (uint16_t)sqadd_s16(
                    sqadd_s16(sqrdmulh_s16(r, (int16_t)(77 << 7)),
                              sqrdmulh_s16(g, (int16_t)(150 << 7))),
                    sqrdmulh_s16(b, (int16_t)(29 << 7)));
            }
            hdf_dma_1d_w(dma, (uint64_t)(uintptr_t)(ext->grid_y + goff), idm->s2_gy.y_row, gw * 2U);
        }
    }

    uint64_t c_s3_start = xm6_read_cycles();

    /* 4. Stage 3: Ping-Pong 2D DMA Chroma Delta + Full-Res Y Fusion */
    const uint16_t *gy_grid = mono ? ext->grid_r : ext->grid_y;
    LiveVCoordStep vc_y;
    live_vcoord_init(&vc_y, ch, gh);

    for (uint32_t gy = 0U; gy < gh; ++gy) {
        uint32_t bank     = gy & 1U;
        uint32_t cy0      = gy * 2U;
        uint32_t has_pair = (cy0 + 1U < ch) ? 1U : 0U;
        uint32_t y_rows_n = has_pair ? 4U : 2U;
        uint32_t c_rows_n = has_pair ? 2U : 1U;

        hdf_dma_2d_r(dma, idm->s3.y_rows[bank][0], HDF_LIVE_W,
                     sy_base + (uint64_t)(2U * cy0) * sy_st, sy_st,
                     HDF_LIVE_W, y_rows_n);

        if (!mono) {
            hdf_dma_2d_r(dma, idm->s3.c_rows[bank][0], HDF_LIVE_W,
                         sc_base + (uint64_t)cy0 * sc_st, sc_st,
                         HDF_LIVE_W, c_rows_n);
            uint8_t *c1 = has_pair ? idm->s3.c_rows[bank][1] : NULL;
            size_t goff = (size_t)gy * gw;
            hdf_dma_1d_r(dma, idm->s3.gr_row, (uint64_t)(uintptr_t)(ext->grid_r + goff), gw * 2U);
            hdf_dma_1d_r(dma, idm->s3.gg_row, (uint64_t)(uintptr_t)(ext->grid_g + goff), gw * 2U);
            hdf_dma_1d_r(dma, idm->s3.gb_row, (uint64_t)(uintptr_t)(ext->grid_b + goff), gw * 2U);

            uint32_t emask = ext->c_edge_mask[gy];
            uint32_t ychk  = ext->y_edge_mask[gy] & ~emask;
            if (ychk != 0U) {
                emask |= live_s3_check_schroma_mask(idm->s3.gr_row, idm->s3.gg_row, idm->s3.gb_row, ychk);
            }

            if (emask == 0xFFFFFFFFU) {
                live_s3_chroma_exact_masked(idm->s3.y_rows[bank][0], idm->s3.y_rows[bank][1],
                                            idm->s3.c_rows[bank][0],
                                            has_pair ? idm->s3.y_rows[bank][2] : NULL,
                                            has_pair ? idm->s3.y_rows[bank][3] : NULL, c1,
                                            idm->s3.gr_row, idm->s3.gg_row, idm->s3.gb_row,
                                            emask, lvl_idx, NULL);
            } else {
                if (emask != 0U) {
                    live_s3_chroma_exact_masked(idm->s3.y_rows[bank][0], idm->s3.y_rows[bank][1],
                                                idm->s3.c_rows[bank][0],
                                                has_pair ? idm->s3.y_rows[bank][2] : NULL,
                                                has_pair ? idm->s3.y_rows[bank][3] : NULL, c1,
                                                idm->s3.gr_row, idm->s3.gg_row, idm->s3.gb_row,
                                                emask, lvl_idx, idm->s3.exact_uv);
                }
                hdf_dma_1d_r(dma, idm->s3.r_in, (uint64_t)(uintptr_t)(ext->r256 + goff), gw);
                hdf_dma_1d_r(dma, idm->s3.g_in, (uint64_t)(uintptr_t)(ext->g256 + goff), gw);
                hdf_dma_1d_r(dma, idm->s3.b_in, (uint64_t)(uintptr_t)(ext->b256 + goff), gw);
                for (uint32_t gx = 0U; gx < gw; ++gx) {
                    idm->s3.r_out[gx] = hdf_comp_px_q15(idm->s3.r_in[gx], idm->s3.gr_row[gx], lvl_idx);
                    idm->s3.g_out[gx] = hdf_comp_px_q15(idm->s3.g_in[gx], idm->s3.gg_row[gx], lvl_idx);
                    idm->s3.b_out[gx] = hdf_comp_px_q15(idm->s3.b_in[gx], idm->s3.gb_row[gx], lvl_idx);
                }
                for (uint32_t gx = 0U; gx < gw; ++gx) {
                    int16_t dR = (int16_t)((int32_t)idm->s3.r_out[gx] - (int32_t)idm->s3.r_in[gx]);
                    int16_t dG = (int16_t)((int32_t)idm->s3.g_out[gx] - (int32_t)idm->s3.g_in[gx]);
                    int16_t dB = (int16_t)((int32_t)idm->s3.b_out[gx] - (int32_t)idm->s3.b_in[gx]);
                    int32_t dcb = srshr1_s16(dB)
                                - sqrdmulh_s16(dR, (int16_t)(43 << 7))
                                - sqrdmulh_s16(dG, (int16_t)(85 << 7));
                    int32_t dcr = srshr1_s16(dR)
                                - sqrdmulh_s16(dG, (int16_t)(107 << 7))
                                - sqrdmulh_s16(dB, (int16_t)(21 << 7));
                    uint32_t cx0 = gx << 1;
                    for (uint32_t dx = 0U; dx < 2U; ++dx) {
                        uint32_t off = (cx0 + dx) << 1;
                        idm->s3.c_rows[bank][0][off]      = clamp_u8((int32_t)idm->s3.c_rows[bank][0][off]      + dcb);
                        idm->s3.c_rows[bank][0][off + 1U] = clamp_u8((int32_t)idm->s3.c_rows[bank][0][off + 1U] + dcr);
                        if (c1) {
                            c1[off]      = clamp_u8((int32_t)c1[off]      + dcb);
                            c1[off + 1U] = clamp_u8((int32_t)c1[off + 1U] + dcr);
                        }
                    }
                }
                if (emask != 0U) {
                    uint32_t m = emask, i = 0U;
                    while (m != 0U) {
                        uint32_t k = ctz32_portable(m);
                        m &= m - 1U;
                        uint32_t boff = k << 5;
                        const uint8_t *src = idm->s3.exact_uv + (size_t)i * 64U;
                        memcpy(idm->s3.c_rows[bank][0] + boff, src, 32U);
                        if (c1) memcpy(c1 + boff, src + 32U, 32U);
                        ++i;
                    }
                }
            }
            hdf_dma_2d_w(dma, dc_base + (uint64_t)cy0 * dc_st, dc_st,
                         idm->s3.c_rows[bank][0], HDF_LIVE_W, HDF_LIVE_W, c_rows_n);
        } else if (dc_base != sc_base && dc_base != 0U && sc_base != 0U) {
            hdf_dma_2d_r(dma, idm->s3.c_rows[bank][0], HDF_LIVE_W,
                         sc_base + (uint64_t)cy0 * sc_st, sc_st, HDF_LIVE_W, c_rows_n);
            hdf_dma_2d_w(dma, dc_base + (uint64_t)cy0 * dc_st, dc_st,
                         idm->s3.c_rows[bank][0], HDF_LIVE_W, HDF_LIVE_W, c_rows_n);
        }

        /* Luma rowpair 0 (rows 2*cy0, 2*cy0+1) */
        uint32_t gy0, gy1, fy;
        live_vcoord_next(&vc_y, &gy0, &gy1, &fy);
        hdf_dma_1d_r(dma, idm->s3.gy_r0, (uint64_t)(uintptr_t)(gy_grid + (size_t)gy0 * gw), gw * 2U);
        hdf_dma_1d_r(dma, idm->s3.gy_r1, (uint64_t)(uintptr_t)(gy_grid + (size_t)gy1 * gw), gw * 2U);
        live_vlerp_hlerp_y_512(idm->s3.gy_r0, idm->s3.gy_r1, idm->s3.vrow_y_512, fy);
        for (uint32_t x = 0U; x < HDF_LIVE_W; x += 2U) {
            uint16_t sk = idm->s3.vrow_y_512[x >> 1];
            idm->s3.y_rows[bank][0][x]      = hdf_comp_px_q15(idm->s3.y_rows[bank][0][x],      sk, lvl_idx);
            idm->s3.y_rows[bank][0][x + 1U] = hdf_comp_px_q15(idm->s3.y_rows[bank][0][x + 1U], sk, lvl_idx);
            idm->s3.y_rows[bank][1][x]      = hdf_comp_px_q15(idm->s3.y_rows[bank][1][x],      sk, lvl_idx);
            idm->s3.y_rows[bank][1][x + 1U] = hdf_comp_px_q15(idm->s3.y_rows[bank][1][x + 1U], sk, lvl_idx);
        }
        if (prof) prof->total_s3_tiles += 2U;

        if (has_pair) {
            live_vcoord_next(&vc_y, &gy0, &gy1, &fy);
            hdf_dma_1d_r(dma, idm->s3.gy_r0, (uint64_t)(uintptr_t)(gy_grid + (size_t)gy0 * gw), gw * 2U);
            hdf_dma_1d_r(dma, idm->s3.gy_r1, (uint64_t)(uintptr_t)(gy_grid + (size_t)gy1 * gw), gw * 2U);
            live_vlerp_hlerp_y_512(idm->s3.gy_r0, idm->s3.gy_r1, idm->s3.vrow_y_512, fy);
            for (uint32_t x = 0U; x < HDF_LIVE_W; x += 2U) {
                uint16_t sk = idm->s3.vrow_y_512[x >> 1];
                idm->s3.y_rows[bank][2][x]      = hdf_comp_px_q15(idm->s3.y_rows[bank][2][x],      sk, lvl_idx);
                idm->s3.y_rows[bank][2][x + 1U] = hdf_comp_px_q15(idm->s3.y_rows[bank][2][x + 1U], sk, lvl_idx);
                idm->s3.y_rows[bank][3][x]      = hdf_comp_px_q15(idm->s3.y_rows[bank][3][x],      sk, lvl_idx);
                idm->s3.y_rows[bank][3][x + 1U] = hdf_comp_px_q15(idm->s3.y_rows[bank][3][x + 1U], sk, lvl_idx);
            }
            if (prof) prof->total_s3_tiles += 2U;
        }

        hdf_dma_2d_w(dma, dy_base + (uint64_t)(2U * cy0) * dy_st, dy_st,
                     idm->s3.y_rows[bank][0], HDF_LIVE_W, HDF_LIVE_W, y_rows_n);
    }

    uint64_t c_end = xm6_read_cycles();
    if (prof) {
        prof->stage1_cycles = (c_s2_start >= c_s1_start) ? (c_s2_start - c_s1_start) : 0ULL;
        prof->stage2_cycles = (c_s3_start >= c_s2_start) ? (c_s3_start - c_s2_start) : 0ULL;
        prof->stage3_cycles = (c_end >= c_s3_start)      ? (c_end - c_s3_start)      : 0ULL;
        prof->total_cycles  = (c_end >= c_s1_start)      ? (c_end - c_s1_start)      : 0ULL;
    }

    return Z8_XM6_DSP_OK;
}

/* ============================================================================
 * Engine B: STILL_GENERAL (up to 45.4MP 8256x5504 NV16/NV12) — 0-diff V71.5
 * ============================================================================ */

typedef struct {
    uint16_t grid_r[HDF_GRID_N0_CAP]; /* 137,216 B */
    uint16_t grid_g[HDF_GRID_N0_CAP]; /* 137,216 B */
    uint16_t grid_b[HDF_GRID_N0_CAP]; /* 137,216 B */
} Xm6StillV2ExtWs; /* 411,648 B <= 512 KiB FW_EXT_WORKBUF */

typedef union {
    struct {
        uint8_t  y_line[2][Z8_XM6_HDF_MAX_WIDTH];  /* 16,512 B ping-pong RX */
        uint8_t  c_line[2][Z8_XM6_HDF_MAX_WIDTH];  /* 16,512 B ping-pong RX */
        uint8_t  r_row[Z8_XM6_HDF_MAX_WIDTH];      /*  8,256 B */
        uint8_t  g_row[Z8_XM6_HDF_MAX_WIDTH];      /*  8,256 B */
        uint8_t  b_row[Z8_XM6_HDF_MAX_WIDTH];      /*  8,256 B */
        uint16_t lr_row[Z8_XM6_HDF_MAX_WIDTH];     /* 16,512 B */
        uint16_t lg_row[Z8_XM6_HDF_MAX_WIDTH];     /* 16,512 B */
        uint16_t lb_row[Z8_XM6_HDF_MAX_WIDTH];     /* 16,512 B */
        uint32_t acc_r[HDF_GRID_MAX_STRIDE];       /*  1,408 B */
        uint32_t acc_g[HDF_GRID_MAX_STRIDE];       /*  1,408 B */
        uint32_t acc_b[HDF_GRID_MAX_STRIDE];       /*  1,408 B */
        uint16_t gr_row[HDF_GRID_MAX_STRIDE];      /*    704 B */
        uint16_t gg_row[HDF_GRID_MAX_STRIDE];      /*    704 B */
        uint16_t gb_row[HDF_GRID_MAX_STRIDE];      /*    704 B */
    } s1; /* 113,664 B <= 192 KiB */

    struct {
        uint16_t grid_ch[HDF_GRID_N0_CAP];         /* 137,216 B */
        uint16_t lvl1[HDF_GRID_N1_CAP];            /*  38,400 B */
        uint16_t lvl2[HDF_GRID_N2_CAP];            /*  12,288 B */
        /* Disjoint sub-union for blur vs blend -> total 195,728 B <= 196,608 B (192 KiB)! */
        union {
            struct {
                uint16_t hblur_row[HDF_GRID_MAX_STRIDE + HDF_VROW_PAD];          /*   784 B */
                uint32_t vblur_sum[HDF_GRID_MAX_STRIDE];                         /* 1,408 B */
                uint16_t vblur_ring[HDF_VBLUR_RING_SLOTS][HDF_GRID_MAX_STRIDE];  /* 5,632 B */
            } blur; /* 7,824 B: 100% row-major O(1) sliding-window V-blur! */
            struct {
                uint16_t gx1[HDF_GRID_MAX_STRIDE];
                uint8_t  fx1[HDF_GRID_MAX_STRIDE];
                uint16_t gx2[HDF_GRID_MAX_STRIDE];
                uint8_t  fx2[HDF_GRID_MAX_STRIDE];
                uint16_t vrow1[HDF_GRID_MAX_STRIDE + HDF_VROW_PAD];
                uint16_t vrow2[HDF_GRID_MAX_STRIDE + HDF_VROW_PAD];
                uint16_t s2_row[HDF_GRID_MAX_STRIDE];
                uint16_t s3_row[HDF_GRID_MAX_STRIDE];
            } blend; /* 5,088 B */
        } work;
    } s2; /* 195,728 B <= 196,608 B (192 KiB FW_APP_WORKBUF!) */

    struct {
        uint8_t  y_strip[2][2][Z8_XM6_HDF_MAX_WIDTH];  /* 33,024 B ping-pong 2-row 2D strips */
        uint8_t  c_strip[2][2][Z8_XM6_HDF_MAX_WIDTH];  /* 33,024 B ping-pong 2-row 2D strips */
        uint16_t gx_arr[Z8_XM6_HDF_MAX_WIDTH];         /* 16,512 B */
        uint8_t  fx_arr[Z8_XM6_HDF_MAX_WIDTH];         /*  8,256 B */
        uint16_t gr_r0[HDF_GRID_MAX_STRIDE];           /*    704 B cached DMA grid rows */
        uint16_t gr_r1[HDF_GRID_MAX_STRIDE];           /*    704 B */
        uint16_t gg_r0[HDF_GRID_MAX_STRIDE];           /*    704 B */
        uint16_t gg_r1[HDF_GRID_MAX_STRIDE];           /*    704 B */
        uint16_t gb_r0[HDF_GRID_MAX_STRIDE];           /*    704 B */
        uint16_t gb_r1[HDF_GRID_MAX_STRIDE];           /*    704 B */
        uint16_t vrow_r[HDF_GRID_MAX_STRIDE + HDF_VROW_PAD];
        uint16_t vrow_g[HDF_GRID_MAX_STRIDE + HDF_VROW_PAD];
        uint16_t vrow_b[HDF_GRID_MAX_STRIDE + HDF_VROW_PAD];
        uint16_t tile_s[3][HDF_STILL_TILE_PX];         /*  3,072 B: 512-px Q15 hlerp */
        uint8_t  tile_rgb_in[3][HDF_STILL_TILE_PX];    /*  1,536 B */
        uint8_t  tile_rgb_out[3][HDF_STILL_TILE_PX];   /*  1,536 B */
        int16_t  dcb_stash[Z8_XM6_HDF_MAX_WIDTH / 2U]; /*  8,256 B */
        int16_t  dcr_stash[Z8_XM6_HDF_MAX_WIDTH / 2U]; /*  8,256 B */
    } s3; /* 120,048 B <= 192 KiB */
} Xm6StillV2IdmWs;

typedef char z8_hdf_v2_chk_still_ext[(sizeof(Xm6StillV2ExtWs) <= Z8_XM6_HDF_EXT_BYTES) ? 1 : -1];
typedef char z8_hdf_v2_chk_still_idm[(sizeof(Xm6StillV2IdmWs) <= Z8_XM6_HDF_IDM_BYTES) ? 1 : -1];

static void still_build_upsample_map(uint32_t dst_len, uint32_t src_len,
                                     uint16_t *gx_arr, uint8_t *fx_arr)
{
    uint64_t den       = 2ULL * dst_len;
    uint64_t right_lim = (uint64_t)(2U * src_len - 1U) * dst_len;
    for (uint32_t x = 0U; x < dst_len; ++x) {
        uint64_t p = (uint64_t)(2U * x + 1U) * src_len;
        if (src_len <= 1U || p <= dst_len) {
            gx_arr[x] = 0U;
            fx_arr[x] = 0U;
        } else if (p >= right_lim) {
            gx_arr[x] = (uint16_t)(src_len - 1U);
            fx_arr[x] = 0U;
        } else {
            uint64_t num = p - dst_len;
            uint32_t g   = (uint32_t)(num / den);
            uint64_t rem = num % den;
            uint32_t fx  = (uint32_t)((rem * 256ULL + dst_len) / den);
            if (fx >= 256U) { g += 1U; fx = 0U; }
            if (g >= src_len) { g = src_len - 1U; fx = 0U; }
            gx_arr[x] = (uint16_t)g;
            fx_arr[x] = (uint8_t)fx;
        }
    }
}

static void still_vcoord_map(uint32_t y, uint32_t dst_h, uint32_t src_h,
                             uint32_t *gy0, uint32_t *gy1, uint32_t *fy_q8)
{
    uint64_t p         = (uint64_t)(2U * y + 1U) * src_h;
    uint64_t right_lim = (uint64_t)(2U * src_h - 1U) * dst_h;
    if (src_h <= 1U || p <= dst_h) {
        *gy0 = 0U; *gy1 = 0U; *fy_q8 = 0U;
    } else if (p >= right_lim) {
        *gy0 = src_h - 1U; *gy1 = src_h - 1U; *fy_q8 = 0U;
    } else {
        uint64_t den = 2ULL * dst_h;
        uint64_t num = p - dst_h;
        uint32_t g   = (uint32_t)(num / den);
        uint64_t rem = num % den;
        uint32_t fy  = (uint32_t)((rem * 256ULL + dst_h) / den);
        if (fy >= 256U) { g += 1U; fy = 0U; }
        if (g >= src_h - 1U) {
            *gy0 = src_h - 1U; *gy1 = src_h - 1U; *fy_q8 = 0U;
        } else {
            *gy0 = g; *gy1 = g + 1U; *fy_q8 = fy;
        }
    }
}

static void still_downsample_2x2(const uint16_t *src, uint32_t sw, uint32_t sh, uint32_t ss,
                                 uint16_t *dst, uint32_t dw, uint32_t dh, uint32_t ds)
{
    for (uint32_t y = 0U; y < dh; ++y) {
        uint32_t y0 = 2U * y;
        uint32_t y1 = (y0 + 1U < sh) ? (y0 + 1U) : (sh - 1U);
        const uint16_t *r0 = src + (size_t)y0 * ss;
        const uint16_t *r1 = src + (size_t)y1 * ss;
        uint16_t *out = dst + (size_t)y * ds;
        for (uint32_t x = 0U; x < dw; ++x) {
            uint32_t x0 = 2U * x;
            uint32_t x1 = (x0 + 1U < sw) ? (x0 + 1U) : (sw - 1U);
            uint32_t s  = (uint32_t)r0[x0] + r0[x1] + r1[x0] + r1[x1];
            out[x] = (uint16_t)((s + 2U) >> 2);
        }
        for (uint32_t p = dw; p < ds; ++p) out[p] = out[dw - 1U];
    }
}

/* Row-major O(1) sliding-window vertical box blur:
 * 100% contiguous horizontal vector loads/stores (zero column-stride bank conflicts on XM6!),
 * bit-exact vs V71.5 hdf_blur_v_grid_u16().
 */
static void still_blur_v_row_major_o1(uint16_t *grid,
                                      uint32_t *col_sum,
                                      uint16_t ring[HDF_VBLUR_RING_SLOTS][HDF_GRID_MAX_STRIDE],
                                      uint32_t w, uint32_t h, uint32_t stride,
                                      uint32_t radius, uint32_t mul)
{
    uint32_t ring_slots = radius + 2U; /* <= 8 for all radius <= 6 */

    /* Initialize running column sums for y = 0 */
    memset(col_sum, 0, (size_t)w * sizeof(uint32_t));
    for (int32_t d = -(int32_t)radius; d <= (int32_t)radius; ++d) {
        uint32_t ry = (d < 0) ? 0U : (((uint32_t)d >= h) ? (h - 1U) : (uint32_t)d);
        const uint16_t *rptr = grid + (size_t)ry * stride;
        for (uint32_t x = 0U; x < w; ++x) {
            col_sum[x] += rptr[x];
        }
    }

    /* Save unmodified row 0 into ring[0], then overwrite row 0 */
    memcpy(ring[0], grid, (size_t)w * sizeof(uint16_t));
    for (uint32_t x = 0U; x < w; ++x) {
        grid[x] = (uint16_t)(((uint64_t)(col_sum[x] + radius) * mul) >> 32);
    }

    /* Sweep y = 1..h-1 in strictly contiguous row-major order (XM6 16-lane SIMD friendly!) */
    for (uint32_t y = 1U; y < h; ++y) {
        int32_t li = (int32_t)y - 1 - (int32_t)radius;
        int32_t ei = (int32_t)y + (int32_t)radius;
        uint32_t y_leave = (li < 0) ? 0U : (uint32_t)li;
        uint32_t y_enter = ((uint32_t)ei >= h) ? (h - 1U) : (uint32_t)ei;

        const uint16_t *leave_row = ring[y_leave % ring_slots];
        const uint16_t *enter_row = grid + (size_t)y_enter * stride;
        uint16_t *cur_row         = grid + (size_t)y * stride;
        uint16_t *save_slot       = ring[y % ring_slots];

        for (uint32_t x = 0U; x < w; ++x) {
            col_sum[x] = col_sum[x] + (uint32_t)enter_row[x] - (uint32_t)leave_row[x];
        }
        /* Preserve unmodified cur_row before overwriting; slot != leave_row's slot */
        memcpy(save_slot, cur_row, (size_t)w * sizeof(uint16_t));
        for (uint32_t x = 0U; x < w; ++x) {
            cur_row[x] = (uint16_t)(((uint64_t)(col_sum[x] + radius) * mul) >> 32);
        }
    }
}

static void still_blur_grid_3pass(uint16_t *grid,
                                  uint16_t *h_tmp,
                                  uint32_t *v_sum,
                                  uint16_t v_ring[HDF_VBLUR_RING_SLOTS][HDF_GRID_MAX_STRIDE],
                                  uint32_t w, uint32_t h, uint32_t stride, uint32_t radius)
{
    if (!radius || (w <= 1U && h <= 1U)) return;
    uint32_t kt  = 2U * radius + 1U;
    uint32_t mul = (uint32_t)((1ULL << 32) / kt + 1ULL);

    for (uint32_t pass = 0U; pass < 3U; ++pass) {
        if (w > 1U) {
            for (uint32_t y = 0U; y < h; ++y) {
                uint16_t *row = grid + (size_t)y * stride;
                for (uint32_t x = 0U; x < w; ++x) {
                    uint32_t sum = 0U;
                    for (int32_t d = -(int32_t)radius; d <= (int32_t)radius; ++d) {
                        int32_t idx = (int32_t)x + d;
                        if (idx < 0) idx = 0;
                        else if ((uint32_t)idx >= w) idx = (int32_t)w - 1;
                        sum += row[idx];
                    }
                    h_tmp[x] = (uint16_t)(((uint64_t)(sum + radius) * mul) >> 32);
                }
                memcpy(row, h_tmp, (size_t)w * sizeof(uint16_t));
                uint16_t last = h_tmp[w - 1U];
                for (uint32_t p = w; p < stride; ++p) row[p] = last;
            }
        }
        if (h > 1U) {
            still_blur_v_row_major_o1(grid, v_sum, v_ring, w, h, stride, radius, mul);
        }
    }
}

static void still_vlerp_row(const uint16_t *r0, const uint16_t *r1,
                            uint16_t *out, uint32_t gw, uint32_t fy_q8, uint32_t pad_tail)
{
    uint32_t w0 = 256U - fy_q8;
    for (uint32_t x = 0U; x < gw; ++x) {
        out[x] = (uint16_t)((w0 * r0[x] + fy_q8 * r1[x] + 128U) >> 8);
    }
    uint16_t last = gw ? out[gw - 1U] : 0U;
    for (uint32_t p = 0U; p < pad_tail; ++p) out[gw + p] = last;
}

static void still_hlerp_row(const uint16_t *vrow, const uint16_t *gx, const uint8_t *fx,
                            uint16_t *out, uint32_t w)
{
    for (uint32_t x = 0U; x < w; ++x) {
        uint32_t g = gx[x], f = fx[x];
        out[x] = (uint16_t)(((256U - f) * vrow[g] + f * vrow[g + 1U] + 128U) >> 8);
    }
}

static void still_build_channel_pyramid_idm(Xm6StillV2IdmWs *idm, uint16_t *ext_grid,
                                            uint32_t gw, uint32_t gh, uint32_t gws,
                                            uint32_t gw1, uint32_t gh1, uint32_t gws1,
                                            uint32_t gw2, uint32_t gh2, uint32_t gws2,
                                            uint32_t r1, uint32_t r2, uint32_t r3,
                                            uint32_t w1, uint32_t w2, uint32_t w3,
                                            uint32_t white_scale_q8, uint32_t k_q12,
                                            const HdfDmaCtx *dma)
{
    uint32_t g0_bytes = gws * gh * 2U;
    hdf_dma_1d_r(dma, idm->s2.grid_ch, (uint64_t)(uintptr_t)ext_grid, g0_bytes);

    still_downsample_2x2(idm->s2.grid_ch, gw, gh, gws, idm->s2.lvl1, gw1, gh1, gws1);
    still_downsample_2x2(idm->s2.lvl1, gw1, gh1, gws1, idm->s2.lvl2, gw2, gh2, gws2);

    still_blur_grid_3pass(idm->s2.grid_ch, idm->s2.work.blur.hblur_row,
                          idm->s2.work.blur.vblur_sum, idm->s2.work.blur.vblur_ring,
                          gw, gh, gws, r1);
    still_blur_grid_3pass(idm->s2.lvl1, idm->s2.work.blur.hblur_row,
                          idm->s2.work.blur.vblur_sum, idm->s2.work.blur.vblur_ring,
                          gw1, gh1, gws1, r2);
    still_blur_grid_3pass(idm->s2.lvl2, idm->s2.work.blur.hblur_row,
                          idm->s2.work.blur.vblur_sum, idm->s2.work.blur.vblur_ring,
                          gw2, gh2, gws2, r3);

    still_build_upsample_map(gw, gw1, idm->s2.work.blend.gx1, idm->s2.work.blend.fx1);
    still_build_upsample_map(gw, gw2, idm->s2.work.blend.gx2, idm->s2.work.blend.fx2);

    for (uint32_t y = 0U; y < gh; ++y) {
        uint32_t y1_0, y1_1, fy1, y2_0, y2_1, fy2;
        still_vcoord_map(y, gh, gh1, &y1_0, &y1_1, &fy1);
        still_vcoord_map(y, gh, gh2, &y2_0, &y2_1, &fy2);

        still_vlerp_row(idm->s2.lvl1 + (size_t)y1_0 * gws1,
                        idm->s2.lvl1 + (size_t)y1_1 * gws1,
                        idm->s2.work.blend.vrow1, gw1, fy1, HDF_VROW_PAD);
        still_vlerp_row(idm->s2.lvl2 + (size_t)y2_0 * gws2,
                        idm->s2.lvl2 + (size_t)y2_1 * gws2,
                        idm->s2.work.blend.vrow2, gw2, fy2, HDF_VROW_PAD);
        still_hlerp_row(idm->s2.work.blend.vrow1,
                        idm->s2.work.blend.gx1, idm->s2.work.blend.fx1,
                        idm->s2.work.blend.s2_row, gw);
        still_hlerp_row(idm->s2.work.blend.vrow2,
                        idm->s2.work.blend.gx2, idm->s2.work.blend.fx2,
                        idm->s2.work.blend.s3_row, gw);

        uint16_t *row0 = idm->s2.grid_ch + (size_t)y * gws;
        for (uint32_t x = 0U; x < gw; ++x) {
            uint32_t mix = (w1 * row0[x] + w2 * idm->s2.work.blend.s2_row[x]
                            + w3 * idm->s2.work.blend.s3_row[x] + 128U) >> 8;
            if (white_scale_q8 != 256U) mix = (mix * white_scale_q8 + 128U) >> 8;
            if (mix > 65535U) mix = 65535U;
            row0[x] = (uint16_t)((mix * k_q12 + 1024U) >> 11);
        }
        for (uint32_t x = gw; x < gws; ++x) row0[x] = row0[gw - 1U];
    }

    hdf_dma_1d_w(dma, (uint64_t)(uintptr_t)ext_grid, idm->s2.grid_ch, g0_bytes);
}

/* Stage 3 512-px tile Q15 horizontal lerp + non-zero scatter detector */
static uint32_t still_tile_hlerp_q15(const uint16_t *vr, const uint16_t *vg, const uint16_t *vb,
                                     const uint16_t *gx, const uint8_t *fx,
                                     uint16_t sr[HDF_STILL_TILE_PX],
                                     uint16_t sg[HDF_STILL_TILE_PX],
                                     uint16_t sb[HDF_STILL_TILE_PX],
                                     uint32_t tn, uint32_t mono)
{
    uint16_t sor = 0U;
    for (uint32_t i = 0U; i < tn; ++i) {
        uint32_t g   = gx[i];
        int16_t fx15 = (int16_t)((uint16_t)fx[i] << 7);
        int16_t ar   = (int16_t)vr[g], br = (int16_t)vr[g + 1U];
        uint16_t rv  = (uint16_t)(int16_t)(ar + sqrdmulh_s16((int16_t)(br - ar), fx15));
        sr[i] = rv;
        sor  |= rv;
        if (!mono) {
            int16_t ag = (int16_t)vg[g], bg = (int16_t)vg[g + 1U];
            int16_t ab = (int16_t)vb[g], bb = (int16_t)vb[g + 1U];
            uint16_t gv = (uint16_t)(int16_t)(ag + sqrdmulh_s16((int16_t)(bg - ag), fx15));
            uint16_t bv = (uint16_t)(int16_t)(ab + sqrdmulh_s16((int16_t)(bb - ab), fx15));
            sg[i] = gv;
            sb[i] = bv;
            sor  |= (uint16_t)(gv | bv);
        }
    }
    return (sor != 0U) ? 1U : 0U;
}

/* Exact dark zero-halo predicate (matches V71.5 NEON hdf_still_tile_is_black:
 * guarantees r,g,b <= 2, where hdf_comp_px_q15(v, 0, lvl) == v for all levels 1..3!) */
static uint32_t still_tile_is_black(const uint8_t *y_row, const uint8_t *c_row,
                                    uint32_t tn, uint32_t mono)
{
    uint32_t ym = 0U, cm = 0U;
    for (uint32_t i = 0U; i < tn; ++i) {
        uint32_t yv = y_row[i];
        if (yv > ym) ym = yv;
        if (!mono) {
            uint32_t cv = c_row[i];
            uint32_t d  = (cv >= 128U) ? (cv - 128U) : (128U - cv);
            if (d > cm) cm = d;
        }
    }
    return ((ym + 2U * cm) <= 2U) ? 1U : 0U;
}

/* Fused 512-px tiled Stage 3 row composite (0-diff vs V71.5 hdf_still_composite_row_fast).
 * Returns 1 if any pixel/chroma in the row was modified (or needed commit), 0 if entire row
 * took the dark zero-halo fast-path with zero output changes.
 */
static uint32_t still_s3_composite_row_tiled(uint8_t *y_row, uint8_t *c_row,
                                             Xm6StillV2IdmWs *idm,
                                             uint32_t width, uint32_t lvl_idx,
                                             uint32_t mono, uint32_t cmode,
                                             Z8Xm6HdfProfile *prof)
{
    uint32_t row_modified = 0U;

    for (uint32_t x = 0U; x < width; ) {
        uint32_t rem = width - x;
        uint32_t tn  = (rem > HDF_STILL_TILE_PX) ? HDF_STILL_TILE_PX : rem;
        if (prof) prof->total_s3_tiles += 1U;

        uint32_t s_nz = still_tile_hlerp_q15(idm->s3.vrow_r, idm->s3.vrow_g, idm->s3.vrow_b,
                                             idm->s3.gx_arr + x, idm->s3.fx_arr + x,
                                             idm->s3.tile_s[0], idm->s3.tile_s[1], idm->s3.tile_s[2],
                                             tn, mono);

        if (!s_nz && (mono || cmode != HDF_CHROMA_420_ODD) &&
            still_tile_is_black(y_row + x, c_row + x, tn, mono)) {
            if (!mono && cmode == HDF_CHROMA_420_EVEN) {
                memset(idm->s3.dcb_stash + (x >> 1), 0, (size_t)(tn >> 1) * sizeof(int16_t));
                memset(idm->s3.dcr_stash + (x >> 1), 0, (size_t)(tn >> 1) * sizeof(int16_t));
            }
            if (prof) prof->dark_fastpath_tiles += 1U;
            x += tn;
            continue;
        }

        row_modified = 1U;

        if (mono) {
            for (uint32_t i = 0U; i < tn; ++i) {
                y_row[x + i] = hdf_comp_px_q15(y_row[x + i], idm->s3.tile_s[0][i], lvl_idx);
            }
        } else {
            /* Sub-pass 1: BT.601 sqrdmulh YCbCr -> planar RGB inside 512-px IDM tile */
            for (uint32_t i = 0U; i + 2U <= tn; i += 2U) {
                int16_t cb  = (int16_t)((int32_t)c_row[x + i] - 128);
                int16_t crv = (int16_t)((int32_t)c_row[x + i + 1U] - 128);
                int32_t off_r = (int32_t)crv + sqrdmulh_s16(crv, (int16_t)(103 << 7));
                int32_t off_b = (int32_t)cb  + sqrdmulh_s16(cb,  (int16_t)(198 << 7));
                int32_t off_g = -(sqrdmulh_s16(cb, (int16_t)(88 << 7)) +
                                  sqrdmulh_s16(crv, (int16_t)(183 << 7)));
                int32_t y0 = (int32_t)y_row[x + i];
                int32_t y1 = (int32_t)y_row[x + i + 1U];
                idm->s3.tile_rgb_in[0][i]      = clamp_u8(y0 + off_r);
                idm->s3.tile_rgb_in[1][i]      = clamp_u8(y0 + off_g);
                idm->s3.tile_rgb_in[2][i]      = clamp_u8(y0 + off_b);
                idm->s3.tile_rgb_in[0][i + 1U] = clamp_u8(y1 + off_r);
                idm->s3.tile_rgb_in[1][i + 1U] = clamp_u8(y1 + off_g);
                idm->s3.tile_rgb_in[2][i + 1U] = clamp_u8(y1 + off_b);
            }

            /* Sub-pass 2: Flat knot16 + cold shoulder + CLZ Lin->sRGB (XM6 16-lane VPU hot loop!) */
            for (uint32_t ch = 0U; ch < 3U; ++ch) {
                const uint8_t  *src_p = idm->s3.tile_rgb_in[ch];
                const uint16_t *sk_p  = idm->s3.tile_s[ch];
                uint8_t        *dst_p = idm->s3.tile_rgb_out[ch];
                for (uint32_t i = 0U; i < tn; ++i) {
                    dst_p[i] = hdf_comp_px_q15(src_p[i], sk_p[i], lvl_idx);
                }
            }

            /* Sub-pass 3: Exact RGB delta -> YCbCr update + 4:2:0 chroma stash */
            for (uint32_t i = 0U; i + 2U <= tn; i += 2U) {
                int16_t dR0 = (int16_t)((int32_t)idm->s3.tile_rgb_out[0][i]      - (int32_t)idm->s3.tile_rgb_in[0][i]);
                int16_t dG0 = (int16_t)((int32_t)idm->s3.tile_rgb_out[1][i]      - (int32_t)idm->s3.tile_rgb_in[1][i]);
                int16_t dB0 = (int16_t)((int32_t)idm->s3.tile_rgb_out[2][i]      - (int32_t)idm->s3.tile_rgb_in[2][i]);
                int16_t dR1 = (int16_t)((int32_t)idm->s3.tile_rgb_out[0][i + 1U] - (int32_t)idm->s3.tile_rgb_in[0][i + 1U]);
                int16_t dG1 = (int16_t)((int32_t)idm->s3.tile_rgb_out[1][i + 1U] - (int32_t)idm->s3.tile_rgb_in[1][i + 1U]);
                int16_t dB1 = (int16_t)((int32_t)idm->s3.tile_rgb_out[2][i + 1U] - (int32_t)idm->s3.tile_rgb_in[2][i + 1U]);

                int32_t dy0 = (int32_t)sqrdmulh_s16(dR0, (int16_t)(77 << 7)) +
                              (int32_t)sqrdmulh_s16(dG0, (int16_t)(150 << 7)) +
                              (int32_t)sqrdmulh_s16(dB0, (int16_t)(29 << 7));
                int32_t dy1 = (int32_t)sqrdmulh_s16(dR1, (int16_t)(77 << 7)) +
                              (int32_t)sqrdmulh_s16(dG1, (int16_t)(150 << 7)) +
                              (int32_t)sqrdmulh_s16(dB1, (int16_t)(29 << 7));
                y_row[x + i]      = clamp_u8((int32_t)y_row[x + i]      + dy0);
                y_row[x + i + 1U] = clamp_u8((int32_t)y_row[x + i + 1U] + dy1);

                int16_t pR = (int16_t)(dR0 + dR1);
                int16_t pG = (int16_t)(dG0 + dG1);
                int16_t pB = (int16_t)(dB0 + dB1);
                int16_t dcb = (int16_t)(srshr2_s16(pB) -
                                        sqrdmulh_s16(pR, (int16_t)(43 << 6)) -
                                        sqrdmulh_s16(pG, (int16_t)(85 << 6)));
                int16_t dcr = (int16_t)(srshr2_s16(pR) -
                                        sqrdmulh_s16(pG, (int16_t)(107 << 6)) -
                                        sqrdmulh_s16(pB, (int16_t)(21 << 6)));

                uint32_t pair_idx = (x + i) >> 1;
                if (cmode == HDF_CHROMA_420_EVEN) {
                    idm->s3.dcb_stash[pair_idx] = dcb;
                    idm->s3.dcr_stash[pair_idx] = dcr;
                } else {
                    int16_t cb_d = dcb;
                    int16_t cr_d = dcr;
                    if (cmode == HDF_CHROMA_420_ODD) {
                        cb_d = srshr1_s16((int16_t)(idm->s3.dcb_stash[pair_idx] + cb_d));
                        cr_d = srshr1_s16((int16_t)(idm->s3.dcr_stash[pair_idx] + cr_d));
                    }
                    c_row[x + i]      = clamp_u8((int32_t)c_row[x + i]      + (int32_t)cb_d);
                    c_row[x + i + 1U] = clamp_u8((int32_t)c_row[x + i + 1U] + (int32_t)cr_d);
                }
            }
        }
        x += tn;
    }

    if (!row_modified && prof) {
        prof->dark_skipped_rows += 1U;
    }
    return row_modified;
}

static int32_t run_still_general(const Z8Xm6HdfParams *in,
                                 const Z8Xm6HdfOutParam *out,
                                 Xm6StillV2IdmWs *idm,
                                 Xm6StillV2ExtWs *ext,
                                 const HdfDmaCtx *dma,
                                 Z8Xm6HdfProfile *prof)
{
    uint32_t width   = (uint32_t)in->src.width;
    uint32_t height  = (uint32_t)in->src.height;
    uint32_t is_nv12 = (in->src.format == Z8_XM6_FMT_NV12) ? 1U : 0U;
    uint32_t mono    = in->monochrome ? 1U : 0U;
    uint32_t level   = in->level;
    uint32_t lvl_idx = (level >= 1U && level <= 3U) ? (level - 1U) : 0U;
    const HdfPreset *pr = &k_hdf_presets[level];

    /* 1. Compute smallest grid block size b >= 1 fitting inside HDF_GRID_N0_CAP (exact V71.5!) */
    uint32_t b = 1U, gw, gh, gws;
    for (;;) {
        gw  = (width  + b - 1U) / b;
        gh  = (height + b - 1U) / b;
        gws = (gw + 7U) & ~7U;
        if (gw <= HDF_GRID_MAX_DIM && gh <= HDF_GRID_MAX_DIM &&
            (uint64_t)gws * gh <= HDF_GRID_N0_CAP) {
            break;
        }
        ++b;
    }
    uint32_t gw1 = (gw  + 1U) / 2U, gh1 = (gh  + 1U) / 2U, gws1 = (gw1 + 7U) & ~7U;
    uint32_t gw2 = (gw1 + 1U) / 2U, gh2 = (gh1 + 1U) / 2U, gws2 = (gw2 + 7U) & ~7U;

    uint32_t max_g = (gw > gh) ? gw : gh;
    uint32_t r1    = 1U;
    uint32_t r2    = (max_g * 3U + 150U) / 300U;
    if (r2 < 1U) r2 = 1U;
    uint32_t r3    = (max_g * 5U + 150U) / 300U;
    if (r3 < 2U) r3 = 2U;

    uint32_t s1_step  = (b >= 12U) ? 4U : ((b >= 6U) ? 2U : 1U);
    uint32_t s1_sh    = (s1_step == 4U) ? 2U : ((s1_step == 2U) ? 1U : 0U);
    uint32_t s1_mask  = s1_step - 1U;
    uint32_t s1_sub_w = width >> s1_sh;

    if (prof) {
        prof->engine_used = Z8_XM6_HDF_MODE_STILL_GENERAL;
        prof->grid_b      = b;
        prof->s1_step     = s1_step;
        prof->gw          = gw;
        prof->gh          = gh;
        prof->r1          = r1;
        prof->r2          = r2;
        prof->r3          = r3;
    }

    uint32_t white_scale_q8[3];
    for (uint32_t c = 0U; c < 3U; ++c) {
        uint8_t wv = (in->white[c] == 0U) ? 255U : in->white[c];
        white_scale_q8[c] = (wv == 255U) ? 256U : (((uint32_t)wv * 256U + 127U) / 255U);
    }

    uint32_t knee_q16   = in->knee_q16 ? (uint32_t)in->knee_q16 : (uint32_t)pr->knee_q16;
    uint32_t ev_gain_q8 = in->ev_gain_q8 ? (uint32_t)in->ev_gain_q8 : 256U;
    uint32_t neu_q8     = (in->neutrality_q8 == 0xFFFFU) ? 96U
                        : ((in->neutrality_q8 > 256U) ? 256U : (uint32_t)in->neutrality_q8);
    uint32_t src_q8     = 256U - neu_q8;

    uint64_t sy_base = in->src.plane_addr[0], sc_base = in->src.plane_addr[1];
    uint64_t dy_base = out->plane_addr[0],    dc_base = out->plane_addr[1];
    uint32_t sy_st   = (uint32_t)in->src.stride[0], sc_st = (uint32_t)in->src.stride[1];
    uint32_t dy_st   = (uint32_t)out->stride[0],    dc_st = (uint32_t)out->stride[1];

    uint32_t in_place = (dy_base == sy_base && dy_st == sy_st &&
                         (mono || (dc_base == sc_base && dc_st == sc_st))) ? 1U : 0U;

    uint64_t c_s1_start = xm6_read_cycles();

    /* 2. Stage 1: V71.5 Strided (4x4 / 2x2 / 1x1) Highlight Extract + Ping-Pong Line DMA */
    uint32_t ping = 0U;
    for (uint32_t gy = 0U; gy < gh; ++gy) {
        uint32_t y_start = gy * b;
        uint32_t y_end   = y_start + b;
        if (y_end > height) y_end = height;
        uint32_t bh = y_end - y_start;

        memset(idm->s1.acc_r, 0, (size_t)gws * sizeof(uint32_t));
        memset(idm->s1.acc_g, 0, (size_t)gws * sizeof(uint32_t));
        memset(idm->s1.acc_b, 0, (size_t)gws * sizeof(uint32_t));

        uint32_t n_rows = 0U;
        if (s1_step > 1U) {
            uint32_t y0 = y_start + ((bh >= s1_step) ? (s1_step >> 1) : 0U);
            for (uint32_t y = y0; y < y_end; y += s1_step) {
                uint32_t bank = ping & 1U;
                ++ping;
                hdf_dma_1d_r(dma, idm->s1.y_line[bank], sy_base + (uint64_t)y * sy_st, width);
                if (!mono) {
                    uint32_t cy = is_nv12 ? (y >> 1) : y;
                    hdf_dma_1d_r(dma, idm->s1.c_line[bank], sc_base + (uint64_t)cy * sc_st, width);
                }
                const uint8_t *yl = idm->s1.y_line[bank];
                const uint8_t *cl = idm->s1.c_line[bank];

                for (uint32_t k = 0U; k < s1_sub_w; ++k) {
                    uint32_t x = k << s1_sh;
                    uint8_t rv, gv, bv;
                    if (mono) {
                        rv = gv = bv = yl[x];
                    } else {
                        int16_t cb  = (int16_t)((int32_t)cl[x] - 128);
                        int16_t crv = (int16_t)((int32_t)cl[x + 1U] - 128);
                        int32_t dr  = (int32_t)crv + sqrdmulh_s16(crv, (int16_t)(103 << 7));
                        int32_t db  = (int32_t)cb  + sqrdmulh_s16(cb,  (int16_t)(198 << 7));
                        int32_t dg  = -(sqrdmulh_s16(cb, (int16_t)(88 << 7)) +
                                        sqrdmulh_s16(crv, (int16_t)(183 << 7)));
                        int32_t yv = (int32_t)yl[x];
                        rv = clamp_u8(yv + dr);
                        gv = clamp_u8(yv + dg);
                        bv = clamp_u8(yv + db);
                    }
                    uint32_t lr = k_srgb_to_lin_u16[rv];
                    uint32_t lg = k_srgb_to_lin_u16[gv];
                    uint32_t lb = k_srgb_to_lin_u16[bv];
                    uint32_t lmax = (lr > lg) ? lr : lg;
                    if (lb > lmax) lmax = lb;
                    uint32_t llum   = (77U * lr + 150U * lg + 29U * lb + 128U) >> 8;
                    uint32_t lpeak7 = (lmax * 7U) >> 3;
                    uint32_t ldrv   = (llum >= lpeak7) ? llum : lpeak7;
                    uint32_t over   = (ldrv > knee_q16) ? (ldrv - knee_q16) : 0U;
                    uint32_t t      = (over * 1490U) >> 16;
                    uint32_t quad   = (t * t) >> 10;
                    uint32_t cover  = (ldrv > HDF_CORE_KNEE_Q16) ? (ldrv - HDF_CORE_KNEE_Q16) : 0U;
                    uint32_t tc     = (cover * 5U) >> 6;
                    uint32_t core   = (tc * tc) >> 10;
                    uint32_t raw    = quad * pr->boost_quad_q14 + core * pr->boost_core_q14;
                    uint32_t boost  = ((raw >> 10) * ev_gain_q8 + 128U) >> 8;
                    if (boost > 49151U) boost = 49151U;
                    uint32_t mr = (src_q8 * lr + neu_q8 * ldrv + 128U) >> 8;
                    uint32_t mg = (src_q8 * lg + neu_q8 * ldrv + 128U) >> 8;
                    uint32_t mb = (src_q8 * lb + neu_q8 * ldrv + 128U) >> 8;
                    uint32_t er = ((lr + 2U) >> 2) + ((boost * mr + 32768U) >> 16);
                    uint32_t eg = ((lg + 2U) >> 2) + ((boost * mg + 32768U) >> 16);
                    uint32_t eb = ((lb + 2U) >> 2) + ((boost * mb + 32768U) >> 16);
                    idm->s1.lr_row[k] = (uint16_t)((er > 65535U) ? 65535U : er);
                    idm->s1.lg_row[k] = (uint16_t)((eg > 65535U) ? 65535U : eg);
                    idm->s1.lb_row[k] = (uint16_t)((eb > 65535U) ? 65535U : eb);
                }

                uint32_t gx = 0U;
                for (uint32_t x0 = 0U; x0 < width; x0 += b, ++gx) {
                    uint32_t x1 = x0 + b;
                    if (x1 > width) x1 = width;
                    uint32_t k0 = (x0 + s1_mask) >> s1_sh;
                    uint32_t k1 = (x1 + s1_mask) >> s1_sh;
                    if (k0 >= k1) { k0 = (k1 > 0U) ? (k1 - 1U) : 0U; k1 = k0 + 1U; }
                    uint32_t sr = 0U, sg = 0U, sb = 0U;
                    for (uint32_t k = k0; k < k1; ++k) {
                        sr += idm->s1.lr_row[k];
                        sg += idm->s1.lg_row[k];
                        sb += idm->s1.lb_row[k];
                    }
                    idm->s1.acc_r[gx] += sr;
                    idm->s1.acc_g[gx] += sg;
                    idm->s1.acc_b[gx] += sb;
                }
                ++n_rows;
            }
        } else {
            for (uint32_t y = y_start; y < y_end; ++y) {
                uint32_t bank = ping & 1U;
                ++ping;
                hdf_dma_1d_r(dma, idm->s1.y_line[bank], sy_base + (uint64_t)y * sy_st, width);
                if (!mono) {
                    uint32_t cy = is_nv12 ? (y >> 1) : y;
                    hdf_dma_1d_r(dma, idm->s1.c_line[bank], sc_base + (uint64_t)cy * sc_st, width);
                }
                const uint8_t *yl = idm->s1.y_line[bank];
                const uint8_t *cl = idm->s1.c_line[bank];

                if (mono) {
                    for (uint32_t x = 0U; x < width; ++x) {
                        uint8_t v = yl[x];
                        idm->s1.r_row[x] = v;
                        idm->s1.g_row[x] = v;
                        idm->s1.b_row[x] = v;
                    }
                } else {
                    for (uint32_t x = 0U; x + 2U <= width; x += 2U) {
                        int32_t c  = (int32_t)cl[x] - 128;
                        int32_t r  = (int32_t)cl[x + 1U] - 128;
                        int32_t dr = ashr_s32(HDF_RGB_R_CR * r + 128, 8);
                        int32_t dg = -ashr_s32(HDF_RGB_G_CB * c + HDF_RGB_G_CR * r + 128, 8);
                        int32_t db = ashr_s32(HDF_RGB_B_CB * c + 128, 8);
                        int32_t y0 = (int32_t)yl[x];
                        int32_t y1 = (int32_t)yl[x + 1U];
                        idm->s1.r_row[x]      = clamp_u8(y0 + dr);
                        idm->s1.g_row[x]      = clamp_u8(y0 + dg);
                        idm->s1.b_row[x]      = clamp_u8(y0 + db);
                        idm->s1.r_row[x + 1U] = clamp_u8(y1 + dr);
                        idm->s1.g_row[x + 1U] = clamp_u8(y1 + dg);
                        idm->s1.b_row[x + 1U] = clamp_u8(y1 + db);
                    }
                }
                for (uint32_t x = 0U; x < width; ++x) {
                    uint32_t lr = k_srgb_to_lin_u16[idm->s1.r_row[x]];
                    uint32_t lg = k_srgb_to_lin_u16[idm->s1.g_row[x]];
                    uint32_t lb = k_srgb_to_lin_u16[idm->s1.b_row[x]];
                    uint32_t lmax = (lr > lg) ? lr : lg;
                    if (lb > lmax) lmax = lb;
                    uint32_t llum   = (77U * lr + 150U * lg + 29U * lb + 128U) >> 8;
                    uint32_t lpeak7 = (lmax * 7U) >> 3;
                    uint32_t ldrv   = (llum >= lpeak7) ? llum : lpeak7;
                    uint32_t over   = (ldrv > knee_q16) ? (ldrv - knee_q16) : 0U;
                    uint32_t t      = (over * 1490U) >> 16;
                    uint32_t quad   = (t * t) >> 10;
                    uint32_t cover  = (ldrv > HDF_CORE_KNEE_Q16) ? (ldrv - HDF_CORE_KNEE_Q16) : 0U;
                    uint32_t tc     = (cover * 5U) >> 6;
                    uint32_t core   = (tc * tc) >> 10;
                    uint32_t raw    = quad * pr->boost_quad_q14 + core * pr->boost_core_q14;
                    uint32_t boost  = ((raw >> 10) * ev_gain_q8 + 128U) >> 8;
                    if (boost > 49151U) boost = 49151U;
                    uint32_t mr = (src_q8 * lr + neu_q8 * ldrv + 128U) >> 8;
                    uint32_t mg = (src_q8 * lg + neu_q8 * ldrv + 128U) >> 8;
                    uint32_t mb = (src_q8 * lb + neu_q8 * ldrv + 128U) >> 8;
                    uint32_t er = ((lr + 2U) >> 2) + ((boost * mr + 32768U) >> 16);
                    uint32_t eg = ((lg + 2U) >> 2) + ((boost * mg + 32768U) >> 16);
                    uint32_t eb = ((lb + 2U) >> 2) + ((boost * mb + 32768U) >> 16);
                    idm->s1.lr_row[x] = (uint16_t)((er > 65535U) ? 65535U : er);
                    idm->s1.lg_row[x] = (uint16_t)((eg > 65535U) ? 65535U : eg);
                    idm->s1.lb_row[x] = (uint16_t)((eb > 65535U) ? 65535U : eb);
                }
                uint32_t gx = 0U;
                for (uint32_t x = 0U; x < width; x += b, ++gx) {
                    uint32_t bw = width - x;
                    if (bw > b) bw = b;
                    uint32_t sr = 0U, sg = 0U, sb = 0U;
                    for (uint32_t k = 0U; k < bw; ++k) {
                        sr += idm->s1.lr_row[x + k];
                        sg += idm->s1.lg_row[x + k];
                        sb += idm->s1.lb_row[x + k];
                    }
                    idm->s1.acc_r[gx] += sr;
                    idm->s1.acc_g[gx] += sg;
                    idm->s1.acc_b[gx] += sb;
                }
            }
            n_rows = bh;
        }

        for (uint32_t gx = 0U; gx < gw; ++gx) {
            uint32_t area;
            if (s1_step > 1U) {
                uint32_t x0 = gx * b;
                uint32_t x1 = x0 + b;
                if (x1 > width) x1 = width;
                uint32_t k0    = (x0 + s1_mask) >> s1_sh;
                uint32_t k1    = (x1 + s1_mask) >> s1_sh;
                uint32_t cnt_x = (k1 > k0) ? (k1 - k0) : 1U;
                area = cnt_x * n_rows;
            } else {
                uint32_t bw = width - gx * b;
                if (bw > b) bw = b;
                area = bw * bh;
            }
            if (area == 1U) {
                idm->s1.gr_row[gx] = (uint16_t)idm->s1.acc_r[gx];
                idm->s1.gg_row[gx] = (uint16_t)idm->s1.acc_g[gx];
                idm->s1.gb_row[gx] = (uint16_t)idm->s1.acc_b[gx];
            } else {
                uint32_t half = area >> 1;
                uint32_t mul  = (uint32_t)((1ULL << 32) / area + 1ULL);
                idm->s1.gr_row[gx] = (uint16_t)(((uint64_t)(idm->s1.acc_r[gx] + half) * mul) >> 32);
                idm->s1.gg_row[gx] = (uint16_t)(((uint64_t)(idm->s1.acc_g[gx] + half) * mul) >> 32);
                idm->s1.gb_row[gx] = (uint16_t)(((uint64_t)(idm->s1.acc_b[gx] + half) * mul) >> 32);
            }
        }
        for (uint32_t gx = gw; gx < gws; ++gx) {
            idm->s1.gr_row[gx] = idm->s1.gr_row[gw - 1U];
            idm->s1.gg_row[gx] = idm->s1.gg_row[gw - 1U];
            idm->s1.gb_row[gx] = idm->s1.gb_row[gw - 1U];
        }

        size_t goff = (size_t)gy * gws;
        hdf_dma_1d_w(dma, (uint64_t)(uintptr_t)(ext->grid_r + goff), idm->s1.gr_row, gws * 2U);
        if (!mono) {
            hdf_dma_1d_w(dma, (uint64_t)(uintptr_t)(ext->grid_g + goff), idm->s1.gg_row, gws * 2U);
            hdf_dma_1d_w(dma, (uint64_t)(uintptr_t)(ext->grid_b + goff), idm->s1.gb_row, gws * 2U);
        }
    }

    uint64_t c_s2_start = xm6_read_cycles();

    /* 3. Stage 2: 100% IDM-Resident 3-Scale Separable Optical Pyramid + Row-Major O(1) V-Blur */
    still_build_channel_pyramid_idm(idm, ext->grid_r, gw, gh, gws, gw1, gh1, gws1, gw2, gh2, gws2,
                                    r1, r2, r3, pr->w1, pr->w2, pr->w3, white_scale_q8[0], pr->k_q12, dma);
    if (!mono) {
        still_build_channel_pyramid_idm(idm, ext->grid_g, gw, gh, gws, gw1, gh1, gws1, gw2, gh2, gws2,
                                        r1, r2, r3, pr->w1, pr->w2, pr->w3, white_scale_q8[1], pr->k_q12, dma);
        still_build_channel_pyramid_idm(idm, ext->grid_b, gw, gh, gws, gw1, gh1, gws1, gw2, gh2, gws2,
                                        r1, r2, r3, pr->w1, pr->w2, pr->w3, white_scale_q8[2], pr->k_q12, dma);
    }

    uint64_t c_s3_start = xm6_read_cycles();

    /* 4. Stage 3: Double-Buffered 2D Strip DMA + 512-px Tiled Q15 Hlerp + Dark Fast-Path */
    still_build_upsample_map(width, gw, idm->s3.gx_arr, idm->s3.fx_arr);

    uint32_t cached_gy0 = 0xFFFFFFFFU;
    uint32_t cached_gy1 = 0xFFFFFFFFU;

    for (uint32_t y_strip = 0U; y_strip < height; y_strip += 2U) {
        uint32_t strip_h = (y_strip + 2U <= height) ? 2U : 1U;
        uint32_t bank    = (y_strip >> 1) & 1U;

        /* 2D DMA read of 1 or 2 Y rows into ping-pong strip bank */
        hdf_dma_2d_r(dma, idm->s3.y_strip[bank][0], Z8_XM6_HDF_MAX_WIDTH,
                     sy_base + (uint64_t)y_strip * sy_st, sy_st, width, strip_h);

        /* 2D DMA read of corresponding CbCr row(s): 1 row for NV12 pair, strip_h rows for NV16 */
        uint32_t c_rows_in_strip = is_nv12 ? 1U : strip_h;
        uint32_t cy_strip        = is_nv12 ? (y_strip >> 1) : y_strip;
        if (!mono || (dc_base != sc_base && dc_base != 0U && sc_base != 0U)) {
            hdf_dma_2d_r(dma, idm->s3.c_strip[bank][0], Z8_XM6_HDF_MAX_WIDTH,
                         sc_base + (uint64_t)cy_strip * sc_st, sc_st, width, c_rows_in_strip);
        }

        uint32_t y_mod[2] = { 0U, 0U };
        uint32_t c_mod[2] = { 0U, 0U };

        for (uint32_t dy = 0U; dy < strip_h; ++dy) {
            uint32_t y = y_strip + dy;
            uint32_t gy0, gy1, fy_q8;
            still_vcoord_map(y, height, gh, &gy0, &gy1, &fy_q8);

            /* Cached DMA fetch of Stage 2 scatter grid rows (26x reuse at 45.4MP!) */
            if (gy0 != cached_gy0) {
                hdf_dma_1d_r(dma, idm->s3.gr_r0, (uint64_t)(uintptr_t)(ext->grid_r + (size_t)gy0 * gws), gw * 2U);
                if (!mono) {
                    hdf_dma_1d_r(dma, idm->s3.gg_r0, (uint64_t)(uintptr_t)(ext->grid_g + (size_t)gy0 * gws), gw * 2U);
                    hdf_dma_1d_r(dma, idm->s3.gb_r0, (uint64_t)(uintptr_t)(ext->grid_b + (size_t)gy0 * gws), gw * 2U);
                }
                cached_gy0 = gy0;
            }
            if (gy1 != cached_gy1) {
                if (gy1 == gy0) {
                    memcpy(idm->s3.gr_r1, idm->s3.gr_r0, (size_t)gw * 2U);
                    if (!mono) {
                        memcpy(idm->s3.gg_r1, idm->s3.gg_r0, (size_t)gw * 2U);
                        memcpy(idm->s3.gb_r1, idm->s3.gb_r0, (size_t)gw * 2U);
                    }
                } else {
                    hdf_dma_1d_r(dma, idm->s3.gr_r1, (uint64_t)(uintptr_t)(ext->grid_r + (size_t)gy1 * gws), gw * 2U);
                    if (!mono) {
                        hdf_dma_1d_r(dma, idm->s3.gg_r1, (uint64_t)(uintptr_t)(ext->grid_g + (size_t)gy1 * gws), gw * 2U);
                        hdf_dma_1d_r(dma, idm->s3.gb_r1, (uint64_t)(uintptr_t)(ext->grid_b + (size_t)gy1 * gws), gw * 2U);
                    }
                }
                cached_gy1 = gy1;
            }

            still_vlerp_row(idm->s3.gr_r0, idm->s3.gr_r1, idm->s3.vrow_r, gw, fy_q8, HDF_VROW_PAD);
            if (!mono) {
                still_vlerp_row(idm->s3.gg_r0, idm->s3.gg_r1, idm->s3.vrow_g, gw, fy_q8, HDF_VROW_PAD);
                still_vlerp_row(idm->s3.gb_r0, idm->s3.gb_r1, idm->s3.vrow_b, gw, fy_q8, HDF_VROW_PAD);
            }

            uint32_t cmode = HDF_CHROMA_422_DIRECT;
            if (is_nv12) {
                if ((y & 1U) == 0U) {
                    cmode = (y + 1U < height) ? HDF_CHROMA_420_EVEN : HDF_CHROMA_422_DIRECT;
                } else {
                    cmode = HDF_CHROMA_420_ODD;
                }
            }

            uint8_t *y_ptr = idm->s3.y_strip[bank][dy];
            uint8_t *c_ptr = idm->s3.c_strip[bank][is_nv12 ? 0U : dy];

            uint32_t mod = still_s3_composite_row_tiled(y_ptr, c_ptr, idm,
                                                        width, lvl_idx, mono, cmode, prof);
            y_mod[dy] = mod;
            if (!mono) {
                if (is_nv12) {
                    if (cmode != HDF_CHROMA_420_EVEN && mod) c_mod[0] = 1U;
                } else {
                    c_mod[dy] = mod;
                }
            }
        }

        /* 2D / selective DMA writeback: skip unmodified dark rows when operating in-place! */
        if (!in_place || (strip_h == 2U && y_mod[0] && y_mod[1])) {
            hdf_dma_2d_w(dma, dy_base + (uint64_t)y_strip * dy_st, dy_st,
                         idm->s3.y_strip[bank][0], Z8_XM6_HDF_MAX_WIDTH, width, strip_h);
        } else {
            for (uint32_t dy = 0U; dy < strip_h; ++dy) {
                if (y_mod[dy]) {
                    hdf_dma_1d_w(dma, dy_base + (uint64_t)(y_strip + dy) * dy_st,
                                 idm->s3.y_strip[bank][dy], width);
                }
            }
        }

        if (!mono) {
            if (is_nv12) {
                if (!in_place || c_mod[0]) {
                    hdf_dma_1d_w(dma, dc_base + (uint64_t)cy_strip * dc_st,
                                 idm->s3.c_strip[bank][0], width);
                }
            } else {
                if (!in_place || (strip_h == 2U && c_mod[0] && c_mod[1])) {
                    hdf_dma_2d_w(dma, dc_base + (uint64_t)y_strip * dc_st, dc_st,
                                 idm->s3.c_strip[bank][0], Z8_XM6_HDF_MAX_WIDTH, width, strip_h);
                } else {
                    for (uint32_t dy = 0U; dy < strip_h; ++dy) {
                        if (c_mod[dy]) {
                            hdf_dma_1d_w(dma, dc_base + (uint64_t)(y_strip + dy) * dc_st,
                                         idm->s3.c_strip[bank][dy], width);
                        }
                    }
                }
            }
        } else if (dc_base != sc_base && dc_base != 0U && sc_base != 0U) {
            hdf_dma_2d_w(dma, dc_base + (uint64_t)cy_strip * dc_st, dc_st,
                         idm->s3.c_strip[bank][0], Z8_XM6_HDF_MAX_WIDTH, width, c_rows_in_strip);
        }
    }

    uint64_t c_end = xm6_read_cycles();
    if (prof) {
        prof->stage1_cycles = (c_s2_start >= c_s1_start) ? (c_s2_start - c_s1_start) : 0ULL;
        prof->stage2_cycles = (c_s3_start >= c_s2_start) ? (c_s3_start - c_s2_start) : 0ULL;
        prof->stage3_cycles = (c_end >= c_s3_start)      ? (c_end - c_s3_start)      : 0ULL;
        prof->total_cycles  = (c_end >= c_s1_start)      ? (c_end - c_s1_start)      : 0ULL;
    }

    return Z8_XM6_DSP_OK;
}

/* ============================================================================
 * Public Entry Points
 * ============================================================================ */

void z8_xm6_hdf_init_params(Z8Xm6HdfParams *in,
                            uint8_t level,
                            uint8_t monochrome,
                            uint8_t mode)
{
    memset(in, 0, sizeof(*in));
    in->bypass_flag   = (level == Z8_XM6_HDF_LEVEL_OFF) ? 1 : 0;
    in->level         = level;
    in->monochrome    = monochrome;
    in->mode          = mode;
    in->knee_q16      = 0U;      /* preset default (22528) */
    in->neutrality_q8 = 0xFFFFU; /* default 96 */
    in->ev_gain_q8    = 256U;    /* default 1.0x */
    in->white[0]      = 255U;
    in->white[1]      = 255U;
    in->white[2]      = 255U;
}

uint16_t z8_xm6_hdf_ev_gain_from_ev256(int32_t scene_ev256)
{
    int32_t dev = 2560 - scene_ev256;
    if (dev < -1536) dev = -1536;
    if (dev >  1536) dev =  1536;
    return (uint16_t)(256 + ashr_s32(dev * 32, 10U));
}


/* ============================================================================
 * Host C11 Reference for LIVE_EVF_256_YC (1280x{850,852,854,856} NV12)
 * 100% bit-exact with CEVA-XM6 Slot 41 LIVE_EVF_256_YC & ARM z8_hdf_live_nv12_1280.
 * ============================================================================ */
#define HDF_EVF_W  1280U
#define HDF_EVF_CW  640U

static inline void evf_reduce_5to2_u8(const uint8_t *src20, uint8_t out8[8])
{
    for (uint32_t k = 0U; k < 4U; ++k) {
        uint32_t p = 5U * k;
        out8[2U * k]      = urhadd_u8(src20[p + 1U], urhadd_u8(src20[p + 0U], uhadd_u8(src20[p + 0U], src20[p + 2U])));
        out8[2U * k + 1U] = urhadd_u8(src20[p + 3U], urhadd_u8(src20[p + 4U], uhadd_u8(src20[p + 4U], src20[p + 2U])));
    }
}

static inline void evf_reduce_5to2_u16(const uint16_t *e20, uint16_t out8[8])
{
    for (uint32_t k = 0U; k < 4U; ++k) {
        uint32_t p = 5U * k;
        out8[2U * k]      = urhadd_u16(e20[p + 1U], urhadd_u16(e20[p + 0U], uhadd_u16(e20[p + 0U], e20[p + 2U])));
        out8[2U * k + 1U] = urhadd_u16(e20[p + 3U], urhadd_u16(e20[p + 4U], uhadd_u16(e20[p + 4U], e20[p + 2U])));
    }
}

static int32_t run_live_evf_256_yc(const Z8Xm6HdfParams *in,
                                   const Z8Xm6HdfOutParam *out,
                                   Xm6LiveV2IdmWs *idm,
                                   Xm6LiveV2ExtWs *ext,
                                   const HdfDmaCtx *dma,
                                   Z8Xm6HdfProfile *prof)
{
    uint32_t height  = (uint32_t)in->src.height;
    uint32_t ch      = height >> 1;
    uint32_t gw      = HDF_LIVE_GW;
    uint32_t gh      = (ch == 425U) ? 170U : 171U;
    uint32_t level   = in->level;
    uint32_t lvl_idx = (level >= 1U && level <= 3U) ? (level - 1U) : 0U;
    uint32_t mono    = in->monochrome ? 1U : 0U;
    const HdfPreset *pr = &k_hdf_presets[level];

    uint64_t sy_base = in->src.plane_addr[0], sc_base = in->src.plane_addr[1];
    uint64_t dy_base = out->plane_addr[0],    dc_base = out->plane_addr[1];
    uint32_t sy_st   = (uint32_t)in->src.stride[0], sc_st = (uint32_t)in->src.stride[1];
    uint32_t dy_st   = (uint32_t)out->stride[0],    dc_st = (uint32_t)out->stride[1];

    if (prof) {
        prof->engine_used = Z8_XM6_HDF_MODE_LIVE_EVF_256_YC;
        prof->grid_b = 5U; prof->s1_step = 2U; prof->gw = gw; prof->gh = gh;
        prof->r1 = 1U; prof->r2 = 4U; prof->r3 = 5U;
    }

    uint8_t boost_lo[64], boost_hi[64];
    for (uint32_t k = 0U; k < 64U; ++k) {
        uint32_t ldrv  = k * 1024U + 512U;
        if (ldrv > 65535U) ldrv = 65535U;
        uint32_t over  = (ldrv > pr->knee_q16) ? (ldrv - pr->knee_q16) : 0U;
        uint32_t t     = (over * 1490U) >> 16;
        uint32_t quad  = (t * t) >> 10;
        uint32_t cover = (ldrv > HDF_CORE_KNEE_Q16) ? (ldrv - HDF_CORE_KNEE_Q16) : 0U;
        uint32_t tc    = (cover * 5U) >> 6;
        uint32_t core  = (tc * tc) >> 10;
        uint32_t raw   = quad * pr->boost_quad_q14 + core * pr->boost_core_q14;
        uint32_t boost = ((raw >> 10) * 256U + 128U) >> 8;
        if (boost > 49151U) boost = 49151U;
        uint32_t bh = boost >> 1;
        boost_lo[k] = (uint8_t)(bh & 0xFFU);
        boost_hi[k] = (uint8_t)(bh >> 8);
    }

    uint8_t y_buf[6][HDF_EVF_W], c_buf[3][HDF_EVF_W];
    uint8_t evf_y640[3][HDF_EVF_CW], evf_cb640[3][HDF_EVF_CW], evf_cr640[3][HDF_EVF_CW];

    for (uint32_t gy = 0U; gy < gh; ++gy) {
        uint32_t cy0 = (gy * ch) / gh;
        uint32_t cy1 = ((gy + 1U) * ch) / gh;
        uint32_t n_crows = cy1 - cy0;
        hdf_dma_2d_r(dma, y_buf[0], HDF_EVF_W, sy_base + (uint64_t)(2U * cy0) * sy_st, sy_st, HDF_EVF_W, 2U * n_crows);
        if (!mono) {
            hdf_dma_2d_r(dma, c_buf[0], HDF_EVF_W, sc_base + (uint64_t)cy0 * sc_st, sc_st, HDF_EVF_W, n_crows);
        }
        for (uint32_t r = 0U; r < n_crows; ++r) {
            for (uint32_t cx = 0U; cx < HDF_EVF_CW; ++cx) {
                evf_y640[r][cx] = uhadd_u8(urhadd_u8(y_buf[2U * r][2U * cx], y_buf[2U * r][2U * cx + 1U]),
                                           urhadd_u8(y_buf[2U * r + 1U][2U * cx], y_buf[2U * r + 1U][2U * cx + 1U]));
                if (!mono) {
                    evf_cb640[r][cx] = c_buf[r][2U * cx];
                    evf_cr640[r][cx] = c_buf[r][2U * cx + 1U];
                }
            }
        }

        size_t goff_row = (size_t)gy * gw;
        uint16_t *gr_out = ext->grid_r + goff_row;
        uint16_t *gg_out = ext->grid_g + goff_row;
        uint16_t *gb_out = ext->grid_b + goff_row;
        uint8_t  *r256_out = ext->r256 + goff_row;
        uint8_t  *g256_out = ext->g256 + goff_row;
        uint8_t  *b256_out = ext->b256 + goff_row;

        if (mono) {
            ext->c_edge_mask[gy] = 0U;
            ext->y_edge_mask[gy] = 0U;
            for (uint32_t chunk = 0U; chunk < 32U; ++chunk) {
                uint32_t cx0 = chunk * 20U, goff = chunk << 3;
                uint32_t ymax = 0U;
                for (uint32_t r = 0U; r < n_crows; ++r)
                    for (uint32_t j = 0U; j < 20U; ++j)
                        if (evf_y640[r][cx0 + j] > ymax) ymax = evf_y640[r][cx0 + j];
                if (ymax <= 158U) {
                    uint8_t yv[20], y8[8];
                    for (uint32_t j = 0U; j < 20U; ++j) {
                        uint8_t a = evf_y640[0][cx0 + j], b = evf_y640[1][cx0 + j];
                        yv[j] = (n_crows == 2U) ? urhadd_u8(a, b) : urhadd_u8(b, uhadd_u8(a, evf_y640[2][cx0 + j]));
                    }
                    evf_reduce_5to2_u8(yv, y8);
                    for (uint32_t j = 0U; j < 8U; ++j)
                        gr_out[goff + j] = (uint16_t)((knot16_eval(y8[j], k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp) + 2U) >> 2);
                } else {
                    uint16_t eg[3][8];
                    for (uint32_t r = 0U; r < n_crows; ++r) {
                        uint16_t e20[20];
                        for (uint32_t j = 0U; j < 20U; ++j) {
                            uint16_t lr = knot16_eval(evf_y640[r][cx0 + j], k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
                            uint32_t bidx = (uint32_t)lr >> 9;
                            int16_t bh = (int16_t)((uint16_t)boost_lo[bidx] | ((uint16_t)boost_hi[bidx] << 8));
                            uint32_t v = (((uint32_t)lr + 2U) >> 2) + (uint16_t)sqrdmulh_s16(bh, (int16_t)lr);
                            e20[j] = (uint16_t)(v > 32767U ? 32767U : v);
                        }
                        evf_reduce_5to2_u16(e20, eg[r]);
                    }
                    for (uint32_t j = 0U; j < 8U; ++j)
                        gr_out[goff + j] = (n_crows == 2U) ? urhadd_u16(eg[0][j], eg[1][j]) : urhadd_u16(eg[1][j], uhadd_u16(eg[0][j], eg[2][j]));
                }
            }
            continue;
        }

        uint32_t c_mask = 0U, y_mask = 0U;
        for (uint32_t chunk = 0U; chunk < 32U; ++chunk) {
            uint32_t cx0 = chunk * 20U, goff = chunk << 3;
            uint32_t y_span_max = 0U, c_span_max = 0U, sat_max = 0U, y_max_all = 0U;
            for (uint32_t q = 0U; q < 5U; ++q) {
                uint32_t ymin = 255U, ymax = 0U, cbmin = 255U, cbmax = 0U, crmin = 255U, crmax = 0U;
                for (uint32_t r = 0U; r < n_crows; ++r) {
                    for (uint32_t d = 0U; d < 4U; ++d) {
                        uint8_t yv = evf_y640[r][cx0 + 4U * q + d];
                        uint8_t cbv = evf_cb640[r][cx0 + 4U * q + d];
                        uint8_t crv = evf_cr640[r][cx0 + 4U * q + d];
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
                    if (n_crows == 2U) {
                        yv[j]  = urhadd_u8(evf_y640[0][cx0 + j],  evf_y640[1][cx0 + j]);
                        cbv[j] = urhadd_u8(evf_cb640[0][cx0 + j], evf_cb640[1][cx0 + j]);
                        crv[j] = urhadd_u8(evf_cr640[0][cx0 + j], evf_cr640[1][cx0 + j]);
                    } else {
                        yv[j]  = urhadd_u8(evf_y640[1][cx0 + j],  uhadd_u8(evf_y640[0][cx0 + j],  evf_y640[2][cx0 + j]));
                        cbv[j] = urhadd_u8(evf_cb640[1][cx0 + j], uhadd_u8(evf_cb640[0][cx0 + j], evf_cb640[2][cx0 + j]));
                        crv[j] = urhadd_u8(evf_cr640[1][cx0 + j], uhadd_u8(evf_cr640[0][cx0 + j], evf_cr640[2][cx0 + j]));
                    }
                }
                evf_reduce_5to2_u8(yv, y8); evf_reduce_5to2_u8(cbv, cb8); evf_reduce_5to2_u8(crv, cr8);
                for (uint32_t j = 0U; j < 8U; ++j) {
                    int16_t cb = (int16_t)((int32_t)cb8[j] - 128), cr = (int16_t)((int32_t)cr8[j] - 128);
                    int32_t dr = (int32_t)cr + sqrdmulh_s16(cr, (int16_t)(103 << 7));
                    int32_t db = (int32_t)cb + sqrdmulh_s16(cb, (int16_t)(198 << 7));
                    int32_t dg = -(sqrdmulh_s16(cb, (int16_t)(88 << 7)) + sqrdmulh_s16(cr, (int16_t)(183 << 7)));
                    uint8_t r8 = clamp_u8((int32_t)y8[j] + dr), g8 = clamp_u8((int32_t)y8[j] + dg), b8 = clamp_u8((int32_t)y8[j] + db);
                    r256_out[goff + j] = r8; g256_out[goff + j] = g8; b256_out[goff + j] = b8;
                    uint16_t lr = knot16_eval(r8, k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
                    uint16_t lg = knot16_eval(g8, k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
                    uint16_t lb = knot16_eval(b8, k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
                    uint16_t lmax = lr > lg ? lr : lg; if (lb > lmax) lmax = lb;
                    uint16_t llum = (uint16_t)sqadd_s16(sqadd_s16(sqrdmulh_s16((int16_t)lr, (int16_t)(77 << 7)),
                                                                  sqrdmulh_s16((int16_t)lg, (int16_t)(150 << 7))),
                                                        sqrdmulh_s16((int16_t)lb, (int16_t)(29 << 7)));
                    uint16_t ldrv = (uint16_t)(lmax - (lmax >> 3)); if (llum > ldrv) ldrv = llum;
                    uint32_t bidx = (uint32_t)ldrv >> 9;
                    int16_t bh = (int16_t)((uint16_t)boost_lo[bidx] | ((uint16_t)boost_hi[bidx] << 8));
                    int16_t nt = sqrdmulh_s16((int16_t)ldrv, (int16_t)(96 << 7));
                    uint32_t er = (((uint32_t)lr + 2U) >> 2) + (uint32_t)(uint16_t)sqrdmulh_s16(bh, sqadd_s16(sqrdmulh_s16((int16_t)lr, (int16_t)(160 << 7)), nt));
                    uint32_t eg = (((uint32_t)lg + 2U) >> 2) + (uint32_t)(uint16_t)sqrdmulh_s16(bh, sqadd_s16(sqrdmulh_s16((int16_t)lg, (int16_t)(160 << 7)), nt));
                    uint32_t eb = (((uint32_t)lb + 2U) >> 2) + (uint32_t)(uint16_t)sqrdmulh_s16(bh, sqadd_s16(sqrdmulh_s16((int16_t)lb, (int16_t)(160 << 7)), nt));
                    gr_out[goff + j] = (uint16_t)(er > 32767U ? 32767U : er);
                    gg_out[goff + j] = (uint16_t)(eg > 32767U ? 32767U : eg);
                    gb_out[goff + j] = (uint16_t)(eb > 32767U ? 32767U : eb);
                }
            } else {
                uint8_t r_rows[3][8], g_rows[3][8], b_rows[3][8];
                uint16_t gr_r[3][8], gg_r[3][8], gb_r[3][8];
                for (uint32_t r = 0U; r < n_crows; ++r) {
                    uint8_t r20[20], g20[20], b20[20];
                    uint16_t er20[20], eg20[20], eb20[20];
                    for (uint32_t j = 0U; j < 20U; ++j) {
                        uint8_t ys = evf_y640[r][cx0 + j];
                        int16_t cb = (int16_t)((int32_t)evf_cb640[r][cx0 + j] - 128);
                        int16_t cr = (int16_t)((int32_t)evf_cr640[r][cx0 + j] - 128);
                        int32_t dr = (int32_t)cr + sqrdmulh_s16(cr, (int16_t)(103 << 7));
                        int32_t db = (int32_t)cb + sqrdmulh_s16(cb, (int16_t)(198 << 7));
                        int32_t dg = -(sqrdmulh_s16(cb, (int16_t)(88 << 7)) + sqrdmulh_s16(cr, (int16_t)(183 << 7)));
                        uint8_t vr = clamp_u8((int32_t)ys + dr), vg = clamp_u8((int32_t)ys + dg), vb = clamp_u8((int32_t)ys + db);
                        r20[j] = vr; g20[j] = vg; b20[j] = vb;
                        uint16_t lr = knot16_eval(vr, k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
                        uint16_t lg = knot16_eval(vg, k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
                        uint16_t lb = knot16_eval(vb, k_s2l16_lin_blo, k_s2l16_lin_bhi, k_s2l16_lin_slp);
                        uint16_t lmax = lr > lg ? lr : lg; if (lb > lmax) lmax = lb;
                        uint16_t llum = (uint16_t)sqadd_s16(sqadd_s16(sqrdmulh_s16((int16_t)lr, (int16_t)(77 << 7)),
                                                                      sqrdmulh_s16((int16_t)lg, (int16_t)(150 << 7))),
                                                            sqrdmulh_s16((int16_t)lb, (int16_t)(29 << 7)));
                        uint16_t ldrv = (uint16_t)(lmax - (lmax >> 3)); if (llum > ldrv) ldrv = llum;
                        uint32_t bidx = (uint32_t)ldrv >> 9;
                        int16_t bh = (int16_t)((uint16_t)boost_lo[bidx] | ((uint16_t)boost_hi[bidx] << 8));
                        int16_t nt = sqrdmulh_s16((int16_t)ldrv, (int16_t)(96 << 7));
                        uint32_t er = (((uint32_t)lr + 2U) >> 2) + (uint32_t)(uint16_t)sqrdmulh_s16(bh, sqadd_s16(sqrdmulh_s16((int16_t)lr, (int16_t)(160 << 7)), nt));
                        uint32_t eg = (((uint32_t)lg + 2U) >> 2) + (uint32_t)(uint16_t)sqrdmulh_s16(bh, sqadd_s16(sqrdmulh_s16((int16_t)lg, (int16_t)(160 << 7)), nt));
                        uint32_t eb = (((uint32_t)lb + 2U) >> 2) + (uint32_t)(uint16_t)sqrdmulh_s16(bh, sqadd_s16(sqrdmulh_s16((int16_t)lb, (int16_t)(160 << 7)), nt));
                        er20[j] = (uint16_t)(er > 32767U ? 32767U : er);
                        eg20[j] = (uint16_t)(eg > 32767U ? 32767U : eg);
                        eb20[j] = (uint16_t)(eb > 32767U ? 32767U : eb);
                    }
                    evf_reduce_5to2_u8(r20, r_rows[r]); evf_reduce_5to2_u8(g20, g_rows[r]); evf_reduce_5to2_u8(b20, b_rows[r]);
                    evf_reduce_5to2_u16(er20, gr_r[r]); evf_reduce_5to2_u16(eg20, gg_r[r]); evf_reduce_5to2_u16(eb20, gb_r[r]);
                }
                for (uint32_t j = 0U; j < 8U; ++j) {
                    if (n_crows == 2U) {
                        r256_out[goff + j] = urhadd_u8(r_rows[0][j], r_rows[1][j]);
                        g256_out[goff + j] = urhadd_u8(g_rows[0][j], g_rows[1][j]);
                        b256_out[goff + j] = urhadd_u8(b_rows[0][j], b_rows[1][j]);
                        gr_out[goff + j]   = urhadd_u16(gr_r[0][j], gr_r[1][j]);
                        gg_out[goff + j]   = urhadd_u16(gg_r[0][j], gg_r[1][j]);
                        gb_out[goff + j]   = urhadd_u16(gb_r[0][j], gb_r[1][j]);
                    } else {
                        r256_out[goff + j] = urhadd_u8(r_rows[1][j], uhadd_u8(r_rows[0][j], r_rows[2][j]));
                        g256_out[goff + j] = urhadd_u8(g_rows[1][j], uhadd_u8(g_rows[0][j], g_rows[2][j]));
                        b256_out[goff + j] = urhadd_u8(b_rows[1][j], uhadd_u8(b_rows[0][j], b_rows[2][j]));
                        gr_out[goff + j]   = urhadd_u16(gr_r[1][j], uhadd_u16(gr_r[0][j], gr_r[2][j]));
                        gg_out[goff + j]   = urhadd_u16(gg_r[1][j], uhadd_u16(gg_r[0][j], gg_r[2][j]));
                        gb_out[goff + j]   = urhadd_u16(gb_r[1][j], uhadd_u16(gb_r[0][j], gb_r[2][j]));
                    }
                }
            }
        }
        ext->c_edge_mask[gy] = c_mask;
        ext->y_edge_mask[gy] = y_mask;
    }

    live_build_channel_pyramid_idm(idm, ext->grid_r, gh, pr, dma);
    if (!mono) {
        live_build_channel_pyramid_idm(idm, ext->grid_g, gh, pr, dma);
        live_build_channel_pyramid_idm(idm, ext->grid_b, gh, pr, dma);
        for (uint32_t y = 0U; y < gh; ++y) {
            size_t goff = (size_t)y * gw;
            for (uint32_t x = 0U; x < gw; ++x) {
                int16_t r = (int16_t)ext->grid_r[goff + x];
                int16_t g = (int16_t)ext->grid_g[goff + x];
                int16_t b = (int16_t)ext->grid_b[goff + x];
                ext->grid_y[goff + x] = (uint16_t)sqadd_s16(
                    sqadd_s16(sqrdmulh_s16(r, (int16_t)(77 << 7)),
                              sqrdmulh_s16(g, (int16_t)(150 << 7))),
                    sqrdmulh_s16(b, (int16_t)(29 << 7)));
            }
        }
    }

    const uint16_t *gy_grid = mono ? ext->grid_r : ext->grid_y;
    LiveVCoordStep vc_y;
    live_vcoord_init(&vc_y, ch, gh);

    for (uint32_t gy = 0U; gy < gh; ++gy) {
        uint32_t cy0 = (gy * ch) / gh;
        uint32_t cy1 = ((gy + 1U) * ch) / gh;
        uint32_t n_crows = cy1 - cy0;
        hdf_dma_2d_r(dma, y_buf[0], HDF_EVF_W, sy_base + (uint64_t)(2U * cy0) * sy_st, sy_st, HDF_EVF_W, 2U * n_crows);
        if (!mono || (dc_base != sc_base && dc_base != 0U && sc_base != 0U)) {
            hdf_dma_2d_r(dma, c_buf[0], HDF_EVF_W, sc_base + (uint64_t)cy0 * sc_st, sc_st, HDF_EVF_W, n_crows);
        }
        if (!mono) {
            size_t goff = (size_t)gy * gw;
            const uint16_t *gr_row = ext->grid_r + goff;
            const uint16_t *gg_row = ext->grid_g + goff;
            const uint16_t *gb_row = ext->grid_b + goff;
            uint32_t emask = ext->c_edge_mask[gy];
            uint32_t ychk  = ext->y_edge_mask[gy] & ~emask;
            if (ychk != 0U) emask |= live_s3_check_schroma_mask(gr_row, gg_row, gb_row, ychk);

            if (emask != 0xFFFFFFFFU) {
                const uint8_t *rin = ext->r256 + goff, *gin = ext->g256 + goff, *bin = ext->b256 + goff;
                uint8_t rout[256], gout[256], bout[256];
                for (uint32_t gx = 0U; gx < gw; ++gx) {
                    rout[gx] = hdf_comp_px_q15(rin[gx], gr_row[gx], lvl_idx);
                    gout[gx] = hdf_comp_px_q15(gin[gx], gg_row[gx], lvl_idx);
                    bout[gx] = hdf_comp_px_q15(bin[gx], gb_row[gx], lvl_idx);
                }
                for (uint32_t chunk = 0U; chunk < 32U; ++chunk) {
                    if (emask & (1U << chunk)) continue;
                    int16_t dcb[8], dcr[8];
                    for (uint32_t j = 0U; j < 8U; ++j) {
                        uint32_t gx = (chunk << 3) + j;
                        int16_t dR = (int16_t)((int32_t)rout[gx] - (int32_t)rin[gx]);
                        int16_t dG = (int16_t)((int32_t)gout[gx] - (int32_t)gin[gx]);
                        int16_t dB = (int16_t)((int32_t)bout[gx] - (int32_t)bin[gx]);
                        dcb[j] = (int16_t)(srshr1_s16(dB) - sqrdmulh_s16(dR, (int16_t)(43 << 7)) - sqrdmulh_s16(dG, (int16_t)(85 << 7)));
                        dcr[j] = (int16_t)(srshr1_s16(dR) - sqrdmulh_s16(dG, (int16_t)(107 << 7)) - sqrdmulh_s16(dB, (int16_t)(21 << 7)));
                    }
                    uint32_t cx_base = chunk * 20U;
                    for (uint32_t k = 0U; k < 4U; ++k) {
                        int16_t cb0 = dcb[2U * k], cb1 = dcb[2U * k + 1U];
                        int16_t cr0 = dcr[2U * k], cr1 = dcr[2U * k + 1U];
                        int16_t cbm = srshr1_s16((int16_t)(cb0 + cb1));
                        int16_t crm = srshr1_s16((int16_t)(cr0 + cr1));
                        int16_t cb5[5] = { cb0, cb0, cbm, cb1, cb1 };
                        int16_t cr5[5] = { cr0, cr0, crm, cr1, cr1 };
                        for (uint32_t r = 0U; r < n_crows; ++r) {
                            uint8_t *crp = c_buf[r] + (cx_base + 5U * k) * 2U;
                            for (uint32_t d = 0U; d < 5U; ++d) {
                                crp[2U * d]      = clamp_u8((int32_t)crp[2U * d]      + (int32_t)cb5[d]);
                                crp[2U * d + 1U] = clamp_u8((int32_t)crp[2U * d + 1U] + (int32_t)cr5[d]);
                            }
                        }
                    }
                }
            }
            if (emask != 0U) {
                for (uint32_t chunk = 0U; chunk < 32U; ++chunk) {
                    if (!(emask & (1U << chunk))) continue;
                    uint32_t goff_c = chunk << 3, cx_base = chunk * 20U;
                    uint16_t sr20[20], sg20[20], sb20[20];
                    for (uint32_t k = 0U; k < 4U; ++k) {
                        uint16_t r0 = gr_row[goff_c + 2U * k], r1 = gr_row[goff_c + 2U * k + 1U];
                        uint16_t g0 = gg_row[goff_c + 2U * k], g1 = gg_row[goff_c + 2U * k + 1U];
                        uint16_t b0 = gb_row[goff_c + 2U * k], b1 = gb_row[goff_c + 2U * k + 1U];
                        uint32_t p = 5U * k;
                        sr20[p+0]=r0; sr20[p+1]=r0; sr20[p+2]=urhadd_u16(r0,r1); sr20[p+3]=r1; sr20[p+4]=r1;
                        sg20[p+0]=g0; sg20[p+1]=g0; sg20[p+2]=urhadd_u16(g0,g1); sg20[p+3]=g1; sg20[p+4]=g1;
                        sb20[p+0]=b0; sb20[p+1]=b0; sb20[p+2]=urhadd_u16(b0,b1); sb20[p+3]=b1; sb20[p+4]=b1;
                    }
                    for (uint32_t r = 0U; r < n_crows; ++r) {
                        for (uint32_t d = 0U; d < 20U; ++d) {
                            uint32_t cx = cx_base + d;
                            uint8_t yh = uhadd_u8(urhadd_u8(y_buf[2U * r][2U * cx], y_buf[2U * r][2U * cx + 1U]),
                                                  urhadd_u8(y_buf[2U * r + 1U][2U * cx], y_buf[2U * r + 1U][2U * cx + 1U]));
                            int16_t cb = (int16_t)((int32_t)c_buf[r][2U * cx] - 128);
                            int16_t crv = (int16_t)((int32_t)c_buf[r][2U * cx + 1U] - 128);
                            int32_t dr = (int32_t)crv + sqrdmulh_s16(crv, (int16_t)(103 << 7));
                            int32_t db = (int32_t)cb  + sqrdmulh_s16(cb,  (int16_t)(198 << 7));
                            int32_t dg = -(sqrdmulh_s16(cb, (int16_t)(88 << 7)) + sqrdmulh_s16(crv, (int16_t)(183 << 7)));
                            uint8_t ri = clamp_u8((int32_t)yh + dr), gi = clamp_u8((int32_t)yh + dg), bi = clamp_u8((int32_t)yh + db);
                            uint8_t ro = hdf_comp_px_q15(ri, sr20[d], lvl_idx);
                            uint8_t go = hdf_comp_px_q15(gi, sg20[d], lvl_idx);
                            uint8_t bo = hdf_comp_px_q15(bi, sb20[d], lvl_idx);
                            int16_t dR = (int16_t)((int32_t)ro - (int32_t)ri);
                            int16_t dG = (int16_t)((int32_t)go - (int32_t)gi);
                            int16_t dB = (int16_t)((int32_t)bo - (int32_t)bi);
                            int32_t dcb = srshr1_s16(dB) - sqrdmulh_s16(dR, (int16_t)(43 << 7)) - sqrdmulh_s16(dG, (int16_t)(85 << 7));
                            int32_t dcr = srshr1_s16(dR) - sqrdmulh_s16(dG, (int16_t)(107 << 7)) - sqrdmulh_s16(dB, (int16_t)(21 << 7));
                            c_buf[r][2U * cx]      = clamp_u8((int32_t)c_buf[r][2U * cx]      + dcb);
                            c_buf[r][2U * cx + 1U] = clamp_u8((int32_t)c_buf[r][2U * cx + 1U] + dcr);
                        }
                    }
                }
            }
        }

        for (uint32_t r = 0U; r < n_crows; ++r) {
            uint32_t gy0, gy1, fy;
            live_vcoord_next(&vc_y, &gy0, &gy1, &fy);
            int16_t vr_pad[272];
            int16_t *vr = vr_pad + 8;
            int16_t fy15 = (int16_t)(fy << 7);
            for (uint32_t x = 0U; x < gw; ++x) {
                int16_t a = (int16_t)gy_grid[(size_t)gy0 * gw + x];
                int16_t b = (int16_t)gy_grid[(size_t)gy1 * gw + x];
                vr[x] = (int16_t)(a + sqrdmulh_s16((int16_t)(b - a), fy15));
            }
            vr[-1] = vr[0]; vr[256] = vr[255];
            uint16_t sy640[HDF_EVF_CW];
            for (uint32_t b = 0U; b < 128U; ++b) {
                int16_t vm1 = vr[(int32_t)(2U * b) - 1], ev = vr[2U * b], od = vr[2U * b + 1U], vp2 = vr[2U * b + 2U];
                uint32_t x = 5U * b;
                sy640[x+0] = (uint16_t)(int16_t)(vm1 + sqrdmulh_s16((int16_t)(ev - vm1), (int16_t)(179 << 7)));
                sy640[x+1] = (uint16_t)(int16_t)(ev  + sqrdmulh_s16((int16_t)(od - ev),  (int16_t)( 26 << 7)));
                sy640[x+2] = urhadd_u16((uint16_t)ev, (uint16_t)od);
                sy640[x+3] = (uint16_t)(int16_t)(ev  + sqrdmulh_s16((int16_t)(od - ev),  (int16_t)(230 << 7)));
                sy640[x+4] = (uint16_t)(int16_t)(od  + sqrdmulh_s16((int16_t)(vp2 - od), (int16_t)( 77 << 7)));
            }
            for (uint32_t x = 0U; x < HDF_EVF_W; x += 2U) {
                uint16_t sk = sy640[x >> 1];
                y_buf[2U * r][x]          = hdf_comp_px_q15(y_buf[2U * r][x],          sk, lvl_idx);
                y_buf[2U * r][x + 1U]     = hdf_comp_px_q15(y_buf[2U * r][x + 1U],     sk, lvl_idx);
                y_buf[2U * r + 1U][x]     = hdf_comp_px_q15(y_buf[2U * r + 1U][x],     sk, lvl_idx);
                y_buf[2U * r + 1U][x + 1U]= hdf_comp_px_q15(y_buf[2U * r + 1U][x + 1U],sk, lvl_idx);
            }
        }

        if (!mono || (dc_base != sc_base && dc_base != 0U && sc_base != 0U)) {
            hdf_dma_2d_w(dma, dc_base + (uint64_t)cy0 * dc_st, dc_st, c_buf[0], HDF_EVF_W, HDF_EVF_W, n_crows);
        }
        hdf_dma_2d_w(dma, dy_base + (uint64_t)(2U * cy0) * dy_st, dy_st, y_buf[0], HDF_EVF_W, HDF_EVF_W, 2U * n_crows);
    }
    return Z8_XM6_DSP_OK;
}

int32_t ricoh_hdf_xm6_v2_run(const Z8Xm6HdfParams *in,
                             const Z8Xm6HdfOutParam *out,
                             void *idm_workspace,
                             size_t idm_workspace_bytes,
                             void *ext_workspace,
                             size_t ext_workspace_bytes,
                             const Z8Xm6DmaOps *dma_ops,
                             Z8Xm6HdfProfile *profile)
{
    if (profile) {
        memset(profile, 0, sizeof(*profile));
    }
    if (!in || !out || !idm_workspace || !ext_workspace ||
        idm_workspace_bytes < Z8_XM6_HDF_IDM_BYTES ||
        ext_workspace_bytes < Z8_XM6_HDF_EXT_BYTES) {
        return Z8_XM6_DSP_ERR_INVALID_PARAM;
    }
    if (in->bypass_flag == 1 || in->level == Z8_XM6_HDF_LEVEL_OFF) {
        return Z8_XM6_DSP_OK;
    }
    if (in->level > Z8_XM6_HDF_LEVEL_STRONG || in->monochrome > 1U ||
        in->mode > Z8_XM6_HDF_MODE_LIVE_EVF_256_YC) {
        return Z8_XM6_DSP_ERR_INVALID_PARAM;
    }
    if (in->src.width <= 0 || in->src.height <= 0 ||
        (uint32_t)in->src.width > Z8_XM6_HDF_MAX_WIDTH ||
        (uint32_t)in->src.height > Z8_XM6_HDF_MAX_HEIGHT ||
        ((uint32_t)in->src.width & 3U) != 0U) {
        return Z8_XM6_DSP_ERR_INVALID_PARAM;
    }
    if (in->src.format != Z8_XM6_FMT_NV16 &&
        in->src.format != Z8_XM6_FMT_NV12 &&
        !(in->monochrome && in->src.format == Z8_XM6_FMT_Y_ONLY)) {
        return Z8_XM6_DSP_ERR_INVALID_PARAM;
    }
    if (in->src.plane_addr[0] == 0U || out->plane_addr[0] == 0U ||
        (uint32_t)in->src.stride[0] < (uint32_t)in->src.width ||
        (uint32_t)out->stride[0] < (uint32_t)in->src.width) {
        return Z8_XM6_DSP_ERR_INVALID_PARAM;
    }
    if (!in->monochrome) {
        if (in->src.plane_addr[1] == 0U || out->plane_addr[1] == 0U ||
            (uint32_t)in->src.stride[1] < (uint32_t)in->src.width ||
            (uint32_t)out->stride[1] < (uint32_t)in->src.width) {
            return Z8_XM6_DSP_ERR_INVALID_PARAM;
        }
    }

    HdfDmaCtx dma;
    dma.ops  = dma_ops;
    dma.prof = profile;

    int is_live_dim = (in->src.width == (int32_t)HDF_LIVE_W &&
                       (in->src.height == 680 || in->src.height == 682) &&
                       in->src.format == Z8_XM6_FMT_NV12);
    int is_evf_dim  = (in->src.width == (int32_t)HDF_EVF_W &&
                       in->src.height >= 850 && in->src.height <= 856 &&
                       ((uint32_t)in->src.height & 1U) == 0U &&
                       in->src.format == Z8_XM6_FMT_NV12);

    if (in->mode == Z8_XM6_HDF_MODE_LIVE_256_YC) {
        if (!is_live_dim) return Z8_XM6_DSP_ERR_INVALID_PARAM;
        return run_live_256_yc(in, out, (Xm6LiveV2IdmWs *)idm_workspace,
                               (Xm6LiveV2ExtWs *)ext_workspace, &dma, profile);
    }
    if (in->mode == Z8_XM6_HDF_MODE_LIVE_EVF_256_YC) {
        if (!is_evf_dim) return Z8_XM6_DSP_ERR_INVALID_PARAM;
        return run_live_evf_256_yc(in, out, (Xm6LiveV2IdmWs *)idm_workspace,
                                   (Xm6LiveV2ExtWs *)ext_workspace, &dma, profile);
    }
    if (in->mode == Z8_XM6_HDF_MODE_AUTO) {
        if (is_live_dim) {
            return run_live_256_yc(in, out, (Xm6LiveV2IdmWs *)idm_workspace,
                                   (Xm6LiveV2ExtWs *)ext_workspace, &dma, profile);
        }
        if (is_evf_dim) {
            return run_live_evf_256_yc(in, out, (Xm6LiveV2IdmWs *)idm_workspace,
                                       (Xm6LiveV2ExtWs *)ext_workspace, &dma, profile);
        }
    }
    return run_still_general(in, out, (Xm6StillV2IdmWs *)idm_workspace,
                             (Xm6StillV2ExtWs *)ext_workspace, &dma, profile);
}

#ifdef XM6_TARGET_Z8_ELF0
/* Direct Core 0 Slot 41 (0x0003955c) firmware entry for Z8 C3.11 z8_xm6_elf0.elf.
 * Uses FW_APP_WORKBUF (0x00008000, 192 KiB IDM) + FW_EXT_WORKBUF (0x41930000, 512 KiB DRAM).
 * Remote CEVA-XM6 build may define XM6_SLOT41_DMA_OPS_FN to supply hardware NikonDMA ops.
 */
#ifdef XM6_SLOT41_DMA_OPS_FN
extern const Z8Xm6DmaOps *XM6_SLOT41_DMA_OPS_FN(void);
#endif

int32_t Z8_RicohHdf_Slot41(const Z8Xm6HdfParams *in,
                           Z8Xm6HdfOutParam *out)
{
    void *idm_buf = (void *)(uintptr_t)Z8_XM6_HDF_IDM_ADDR;
    void *ext_buf = (in && in->ext_work_addr)
                    ? (void *)(uintptr_t)in->ext_work_addr
                    : (void *)(uintptr_t)Z8_XM6_HDF_EXT_ADDR;
    size_t ext_len = (in && in->ext_work_addr && in->ext_work_bytes >= Z8_XM6_HDF_EXT_BYTES)
                     ? (size_t)in->ext_work_bytes
                     : (size_t)Z8_XM6_HDF_EXT_BYTES;
#ifdef XM6_SLOT41_DMA_OPS_FN
    const Z8Xm6DmaOps *dma_ops = XM6_SLOT41_DMA_OPS_FN();
#else
    const Z8Xm6DmaOps *dma_ops = NULL;
#endif
    return ricoh_hdf_xm6_v2_run(in, out, idm_buf, Z8_XM6_HDF_IDM_BYTES,
                                ext_buf, ext_len, dma_ops, NULL);
}
#endif
