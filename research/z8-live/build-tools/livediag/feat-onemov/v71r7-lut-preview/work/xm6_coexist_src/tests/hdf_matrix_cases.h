#ifndef HDF_MATRIX_CASES_H
#define HDF_MATRIX_CASES_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    const char *name;
    uint32_t w, h;
    uint32_t y_stride, c_stride;
    uint8_t  is_nv12;
    uint8_t  mono;
    uint8_t  level;       /* 1=WEAK, 2=MED, 3=STRONG */
    uint8_t  is_live;     /* 0=STILL_GENERAL, 1=LIVE_256_YC */
    uint8_t  in_place;    /* 0=out!=in, 1=in==out */
    uint8_t  pattern;     /* 0=diag, 1=sparse-black, 2=extreme-shoulder */
    uint8_t  white[3];
    uint16_t knee_q16;    /* 0 = default */
    uint16_t neutrality_q8; /* 0xFFFF = default 96 */
    int32_t  scene_ev256; /* only used when exposure_known=1 */
    uint8_t  exposure_known;
} HdfMatrixCase;

static const HdfMatrixCase k_hdf_cases[] = {
    /* 0: Exact Nikon Z8 XM6_DIAG.LOG case! (b=1, step=1, use_vperm=0, out!=in) */
    { "128x128_nv16_med_diag_oop",   128, 128,  128,  128, 0, 0, 2, 0, 0, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 1: 128x128 NV16 MED in-place */
    { "128x128_nv16_med_diag_inp",   128, 128,  128,  128, 0, 0, 2, 0, 1, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 2: 128x128 NV16 MONO WEAK out-of-place */
    { "128x128_nv16_mono_weak_oop",  128, 128,  128,  128, 0, 1, 1, 0, 0, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 3: 128x128 NV12 STRONG in-place + extreme shoulder/saturation stress */
    { "128x128_nv12_str_ext_inp",    128, 128,  128,  128, 1, 0, 3, 0, 1, 2, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 4: 128x128 NV16 custom optics (white!=255, custom knee, neutrality, EV gain) */
    { "128x128_nv16_custom_opt",     128, 128,  128,  128, 0, 0, 3, 0, 0, 0, {240,255,220}, 18000,    180, 1024, 1 },
    /* 5: 128x128 NV12 sparse dark/black tiles + 1 bright spot */
    { "128x128_nv12_sparse_dark",    128, 128,  128,  128, 1, 0, 2, 0, 1, 1, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 6: 140x129 NV12 odd height + non-mult-32 width + stride>width (160) */
    { "140x129_nv12_oddh_stride",    140, 129,  160,  160, 1, 0, 2, 0, 0, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 7: 160x120 NV16 b=1, r=1,2,3 */
    { "160x120_nv16_weak_b1",        160, 120,  160,  160, 0, 0, 1, 0, 1, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 8: 576x384 NV16 b=2, step=1, step_k=2, use_vperm=0 */
    { "576x384_nv16_med_b2",         576, 384,  576,  576, 0, 0, 2, 0, 0, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 9: 1028x68 NV16 b=3, ODD gw=343 (686B -> 2mod4 before gws*2U fix!), w%32==4 */
    { "1028x68_nv16_oddgw_b3",      1028,  68, 1040, 1040, 0, 0, 2, 0, 0, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 10: 1028x68 NV16 MONO b=3, use_vperm=1 mono path + ODD gw=343 */
    { "1028x68_nv16_mono_b3",       1028,  68, 1028, 1028, 0, 1, 3, 0, 1, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 11: 1344x96 NV16 b=4, step=1, step_k=4, use_vperm=1 */
    { "1344x96_nv16_str_b4",        1344,  96, 1344, 1344, 0, 0, 3, 0, 0, 2, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 12: 1620x96 NV16 b=5, step=1, w%32==20 */
    { "1620x96_nv16_med_b5",        1620,  96, 1632, 1632, 0, 0, 2, 0, 1, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 13: 2048x96 NV16 b=6, step=2, step_k=3, use_vperm=1 */
    { "2048x96_nv16_med_b6_s2",     2048,  96, 2048, 2048, 0, 0, 2, 0, 0, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 14: 2400x96 NV12 b=7, step=2, (b&1)!=0, ODD gw=343 */
    { "2400x96_nv12_weak_b7_s2",    2400,  96, 2400, 2400, 1, 0, 1, 0, 0, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 15: 4128x96 NV16 b=12, step=4, step_k=3, multi-tile */
    { "4128x96_nv16_med_b12_s4",    4128,  96, 4128, 4128, 0, 0, 2, 0, 0, 2, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 16: 6048x96 NV16 b=19, step=4, ODD gw=319 (exact 24MP Z8 medium JPEG width!) */
    { "6048x96_nv16_med_b19_s4",    6048,  96, 6048, 6048, 0, 0, 2, 0, 0, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 17: 6200x96 NV16 b=19, step=4, ODD gw=327, custom white */
    { "6200x96_nv16_str_b19_s4",    6200,  96, 6208, 6208, 0, 0, 3, 0, 0, 0, {248,255,236},     0, 0xFFFF,    0, 0 },
    /* 18: 8256x128 NV16 b=24, step=4, step_k=6, 45.4MP full width 5-tile */
    { "8256x128_nv16_med_b24_s4",   8256, 128, 8256, 8256, 0, 0, 2, 0, 0, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 19: 128x8256 NV16 portrait b=24, gw=6, gw2=2 < r3=6 H-blur edge stress */
    { "128x8256_nv16_med_portrait",  128,8256,  128,  128, 0, 0, 2, 0, 0, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 20: 1024x680 NV12 LIVE_256_YC regression (both out!=in tested here) */
    { "1024x680_nv12_live_reg",     1024, 680, 1024, 1024, 1, 0, 2, 1, 0, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 21: 1024x680 NV12 LIVE_256_YC in-place (exact real LCD mode!) */
    { "1024x680_nv12_live_inp",     1024, 680, 1024, 1024, 1, 0, 2, 1, 1, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 22: 1280x852 NV12 LIVE_EVF_256_YC MEDIUM out-of-place (fast + slow S1 + masked S3) */
    { "1280x852_nv12_evf_med_oop",  1280, 852, 1280, 1280, 1, 0, 2, 2, 0, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 23: 1280x852 NV12 LIVE_EVF_256_YC STRONG in-place extreme shoulder + checker stress */
    { "1280x852_nv12_evf_str_inp",  1280, 852, 1280, 1280, 1, 0, 3, 2, 1, 2, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 24: 1280x850 NV12 LIVE_EVF_256_YC WEAK in-place (gh=170, sparse dark + bright spot) */
    { "1280x850_nv12_evf_weak_inp", 1280, 850, 1280, 1280, 1, 0, 1, 2, 1, 1, {255,255,255},     0, 0xFFFF,    0, 0 },
    /* 25: 1280x852 NV12 LIVE_EVF_256_YC MONO STRONG out-of-place (mono hi + copy_c_mono) */
    { "1280x852_nv12_evf_mono_oop", 1280, 852, 1280, 1280, 1, 1, 3, 2, 0, 0, {255,255,255},     0, 0xFFFF,    0, 0 },
};

#define NUM_HDF_CASES ((uint32_t)(sizeof(k_hdf_cases) / sizeof(k_hdf_cases[0])))

static inline void hdf_fill_pattern(uint8_t *y, uint8_t *c,
                                    uint32_t w, uint32_t h,
                                    uint32_t ys, uint32_t cs,
                                    uint8_t is_nv12, uint8_t pat)
{
    uint32_t ch = is_nv12 ? ((h + 1U) >> 1) : h;
    memset(y, 0x5A, (size_t)ys * h);
    memset(c, 0x5A, (size_t)cs * ch);

    if (pat == 1U) {
        for (uint32_t r = 0U; r < h; ++r) {
            uint8_t *yr = y + (size_t)r * ys;
            memset(yr, 0, w);
            if (r >= 8U && r < 16U) {
                for (uint32_t x = 0U; x < w; ++x) yr[x] = (uint8_t)(x & 2U);
            } else if (r >= h / 2U && r < h / 2U + 12U) {
                for (uint32_t x = w / 2U; x < w / 2U + 16U && x < w; ++x) yr[x] = 250U;
            }
        }
        for (uint32_t r = 0U; r < ch; ++r) {
            uint8_t *cr = c + (size_t)r * cs;
            memset(cr, 128, w);
            if (r == 4U) {
                for (uint32_t x = 0U; x < w; x += 2U) { cr[x] = 129U; cr[x + 1U] = 127U; }
            } else if (r >= (ch / 2U) && r < (ch / 2U) + 6U) {
                for (uint32_t x = w / 2U; x < w / 2U + 16U && x < w; x += 2U) { cr[x] = 100U; cr[x + 1U] = 160U; }
            }
        }
        return;
    }

    if (pat == 2U) {
        for (uint32_t r = 0U; r < h; ++r) {
            uint8_t *yr = y + (size_t)r * ys;
            for (uint32_t x = 0U; x < w; ++x) {
                uint32_t v;
                if (((x >> 4) + (r >> 4)) & 1U) {
                    v = 230U + ((x * 7U + r * 11U) % 26U);
                } else {
                    v = (x * 13U + r * 29U) & 255U;
                }
                yr[x] = (uint8_t)v;
            }
        }
        for (uint32_t r = 0U; r < ch; ++r) {
            uint8_t *cr = c + (size_t)r * cs;
            for (uint32_t x = 0U; x < w; x += 2U) {
                cr[x]      = (uint8_t)((x * 19U + r * 31U) & 255U);
                cr[x + 1U] = (uint8_t)((x * 23U + r * 17U + 64U) & 255U);
            }
        }
        return;
    }

    for (uint32_t r = 0U; r < h; ++r) {
        uint8_t *yr = y + (size_t)r * ys;
        for (uint32_t x = 0U; x < w; ++x) {
            uint32_t v;
            if ((x >= w / 8U && x < w / 4U && r >= h / 8U && r < h / 4U) ||
                (x >= (w * 5U) / 8U && x < (w * 6U) / 8U && r >= h / 3U && r < h / 2U)) {
                v = 242U + ((x + r) & 13U);
                if (v > 255U) v = 255U;
            } else if (x < w / 3U && r >= (h * 2U) / 3U) {
                v = 12U + ((x ^ r) & 15U);
            } else {
                v = ((x * 3U + r * 5U) ^ ((x >> 2) * 17U)) & 255U;
            }
            yr[x] = (uint8_t)v;
        }
    }
    for (uint32_t r = 0U; r < ch; ++r) {
        uint8_t *cr = c + (size_t)r * cs;
        for (uint32_t x = 0U; x < w; x += 2U) {
            uint8_t cb = (uint8_t)(80U + (((x * 3U + r * 7U) + ((x & 32U) ? 64U : 0U)) & 95U));
            uint8_t cv = (uint8_t)(88U + (((x * 5U + r * 3U) + ((r & 16U) ? 48U : 0U)) & 95U));
            cr[x]      = cb;
            cr[x + 1U] = cv;
        }
    }
}

static inline uint64_t hdf_fnv1a_rows(const uint8_t *buf, uint32_t w, uint32_t rows, uint32_t stride)
{
    uint64_t h = 14695981039346656037ULL;
    for (uint32_t r = 0U; r < rows; ++r) {
        const uint8_t *p = buf + (size_t)r * stride;
        for (uint32_t x = 0U; x < w; ++x) {
            h ^= (uint64_t)p[x];
            h *= 1099511628211ULL;
        }
    }
    return h;
}
#endif
