/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 NV12 (4:2:0) 4 MiB Tagged YCbCr Cache Scalar Frame Kernel.
 * Adapted from Fenestra-Z8-C311-source/modules/lut_preview/layer0_color.c.
 * Strictly scalar (-mgeneral-regs-only), zero FP, zero SIMD, zero .data/.bss.
 */
#include "layer0_color.h"

static __attribute__((noinline)) uint32_t cache_miss(
    const JpegCube *cube, uint32_t *slot,
    unsigned luma, unsigned cb, unsigned cr, uint32_t key)
{
    uint32_t value = __atomic_load_n(slot, __ATOMIC_RELAXED);
    if ((value & UINT32_C(0xff000000)) != key) {
        value = key | jpeg_cube_sample(cube, luma, jpeg_cube_chroma(cb, cr));
        __atomic_store_n(slot, value, __ATOMIC_RELAXED);
    }
    return value;
}

static inline __attribute__((always_inline)) void grade_pair(
    const JpegCube *cube, uint32_t *cache,
    const uint8_t *src_y, const uint8_t *src_y1, const uint8_t *src_uv,
    uint8_t *dst_y, uint8_t *dst_y1, uint8_t *dst_uv, unsigned width)
{
    for (unsigned x = 0u; x < width; x += 2u) {
        unsigned cb = src_uv[x];
        unsigned cr = src_uv[x + 1u];
        uint32_t key = (1u + (cb >> 6) + ((cr >> 6) << 2)) << 24;
        uint32_t *page = cache + (((cr & 63u) << 14) | ((cb & 63u) << 8));
        unsigned y00 = src_y[x],  y01 = src_y[x + 1u];
        unsigned y10 = src_y1[x], y11 = src_y1[x + 1u];

        uint32_t a = __atomic_load_n(&page[y00], __ATOMIC_RELAXED);
        uint32_t b = __atomic_load_n(&page[y01], __ATOMIC_RELAXED);
        uint32_t c = __atomic_load_n(&page[y10], __ATOMIC_RELAXED);
        uint32_t d = __atomic_load_n(&page[y11], __ATOMIC_RELAXED);

        if (__builtin_expect((a & UINT32_C(0xff000000)) != key, 0)) {
            a = cache_miss(cube, &page[y00], y00, cb, cr, key);
        }
        if (__builtin_expect((b & UINT32_C(0xff000000)) != key, 0)) {
            b = cache_miss(cube, &page[y01], y01, cb, cr, key);
        }
        if (__builtin_expect((c & UINT32_C(0xff000000)) != key, 0)) {
            c = cache_miss(cube, &page[y10], y10, cb, cr, key);
        }
        if (__builtin_expect((d & UINT32_C(0xff000000)) != key, 0)) {
            d = cache_miss(cube, &page[y11], y11, cb, cr, key);
        }

        dst_y[x]      = (uint8_t)a;
        dst_y[x + 1u] = (uint8_t)b;
        dst_y1[x]     = (uint8_t)c;
        dst_y1[x + 1u]= (uint8_t)d;

        unsigned sum_cb = (unsigned)(uint8_t)(a >> 8)  + (uint8_t)(b >> 8) +
                          (unsigned)(uint8_t)(c >> 8)  + (uint8_t)(d >> 8);
        unsigned sum_cr = (unsigned)(uint8_t)(a >> 16) + (uint8_t)(b >> 16) +
                          (unsigned)(uint8_t)(c >> 16) + (uint8_t)(d >> 16);
        dst_uv[x]      = (uint8_t)((sum_cb + 2u) >> 2);
        dst_uv[x + 1u] = (uint8_t)((sum_cr + 2u) >> 2);
    }
}

void layer0_grade_nv12_pair_scalar(const JpegCube *cube, uint32_t *cache,
                                   const uint8_t *src_y, const uint8_t *src_y1,
                                   const uint8_t *src_uv,
                                   uint8_t *dst_y, uint8_t *dst_y1,
                                   uint8_t *dst_uv, unsigned width)
{
    if (!cube || !cache || !src_y || !src_y1 || !src_uv || !dst_y || !dst_y1 || !dst_uv) {
        return;
    }
    grade_pair(cube, cache, src_y, src_y1, src_uv, dst_y, dst_y1, dst_uv, width & ~1u);
}

void layer0_grade_nv12_planes(const JpegCube *cube, uint32_t *cache,
                              const Layer0Frame *frame, unsigned first, unsigned end)
{
    if (!cube || !cache || !frame || !frame->src_y || !frame->src_uv ||
        !frame->dst_y || !frame->dst_uv) {
        return;
    }
    size_t sy_stride  = frame->src_y_stride  ? frame->src_y_stride  : frame->width;
    size_t suv_stride = frame->src_uv_stride ? frame->src_uv_stride : frame->width;
    size_t dy_stride  = frame->dst_y_stride  ? frame->dst_y_stride  : frame->width;
    size_t duv_stride = frame->dst_uv_stride ? frame->dst_uv_stride : frame->width;
    unsigned width = frame->width & ~1u;
    if (end > frame->height) {
        end = frame->height;
    }
    end &= ~1u;

    for (unsigned row = first & ~1u; row < end; row += 2u) {
        size_t sy_off  = (size_t)row * sy_stride;
        size_t suv_off = (size_t)(row >> 1) * suv_stride;
        size_t dy_off  = (size_t)row * dy_stride;
        size_t duv_off = (size_t)(row >> 1) * duv_stride;
        grade_pair(cube, cache,
                   frame->src_y + sy_off, frame->src_y + sy_off + sy_stride,
                   frame->src_uv + suv_off,
                   frame->dst_y + dy_off, frame->dst_y + dy_off + dy_stride,
                   frame->dst_uv + duv_off, width);
    }
}

int layer0_grade_nv12_rows(const JpegCube *cube, uint32_t *cache,
                           const uint8_t *src_nv12, size_t src_bytes,
                           uint8_t *dst_nv12, size_t dst_bytes,
                           unsigned first_row, unsigned end_row)
{
    if (!cube || !cache || !src_nv12 || !dst_nv12 ||
        cube->grid < 2u || cube->grid > CUBE_MAX_GRID ||
        src_bytes < REAR_NV12 || dst_bytes < REAR_NV12 ||
        ((first_row | end_row) & 1u) != 0u ||
        first_row > end_row || end_row > REAR_H) {
        return 0;
    }
    const Layer0Frame frame = {
        .src_y = src_nv12, .src_uv = src_nv12 + (size_t)REAR_W * REAR_H,
        .dst_y = dst_nv12, .dst_uv = dst_nv12 + (size_t)REAR_W * REAR_H,
        .width = REAR_W,   .height = REAR_H,
        .src_y_stride = REAR_W, .src_uv_stride = REAR_W,
        .dst_y_stride = REAR_W, .dst_uv_stride = REAR_W
    };
    layer0_grade_nv12_planes(cube, cache, &frame, first_row, end_row);
    return 1;
}

int layer0_grade_nv12_rows_neon(const JpegCube *cube, uint32_t *cache,
                                const uint8_t *src_nv12, size_t src_bytes,
                                uint8_t *dst_nv12, size_t dst_bytes,
                                unsigned first_row, unsigned end_row,
                                unsigned width, unsigned height)
{
    size_t y_bytes = (size_t)width * height;
    if (!cube || !cache || !src_nv12 || !dst_nv12 ||
        cube->grid < 2u || cube->grid > CUBE_MAX_GRID ||
        !width || ((width | height | first_row | end_row) & 1u) != 0u ||
        first_row > end_row || end_row > height ||
        y_bytes > src_bytes || y_bytes / 2u > src_bytes - y_bytes ||
        y_bytes > dst_bytes || y_bytes / 2u > dst_bytes - y_bytes) {
        return 0;
    }
    const Layer0Frame frame = {
        .src_y = src_nv12, .src_uv = src_nv12 + y_bytes,
        .dst_y = dst_nv12, .dst_uv = dst_nv12 + y_bytes,
        .width = width,    .height = height,
        .src_y_stride = width, .src_uv_stride = width,
        .dst_y_stride = width, .dst_uv_stride = width
    };
    layer0_grade_nv12_planes_neon(cube, cache, &frame, first_row, end_row);
    return 1;
}

/* ---- V71.7-r7 528 KiB L2-Resident 64x64x33 Pre-Baked YCbCr 3D LUT (Scalar) ---- */

void layer0_bake_ycbcr_64x64x33(const JpegCube *cube, uint32_t *table,
                                uint32_t first_cr, uint32_t end_cr)
{
    if (!cube || !table || first_cr >= end_cr) {
        return;
    }
    if (end_cr > LAYER0_BAKED_CHROMA) {
        end_cr = LAYER0_BAKED_CHROMA;
    }
    for (uint32_t cr6 = first_cr; cr6 < end_cr; ++cr6) {
        unsigned cr = cr6 << 2;
        for (uint32_t cb6 = 0u; cb6 < LAYER0_BAKED_CHROMA; ++cb6) {
            unsigned cb = cb6 << 2;
            JpegCubeChroma off = jpeg_cube_chroma(cb, cr);
            uint32_t *col = table + (((cr6 << 6) | cb6) * LAYER0_BAKED_LUMA);
            for (uint32_t y5 = 0u; y5 < LAYER0_BAKED_LUMA; ++y5) {
                unsigned luma = (y5 < 32u) ? (y5 << 3) : 255u;
                col[y5] = jpeg_cube_sample(cube, luma, off);
            }
        }
    }
}

static inline uint32_t baked_lerp_pixel(const uint32_t *col, unsigned y)
{
    unsigned k = y >> 3;
    unsigned w1 = (y == 255u) ? 8u : (y & 7u);
    unsigned w0 = 8u - w1;
    uint32_t p0 = col[k];
    uint32_t p1 = col[k + 1u];
    unsigned oy  = ((p0 & 255u) * w0 + (p1 & 255u) * w1 + 4u) >> 3;
    unsigned ocb = (((p0 >> 8) & 255u) * w0 + ((p1 >> 8) & 255u) * w1 + 4u) >> 3;
    unsigned ocr = (((p0 >> 16) & 255u) * w0 + ((p1 >> 16) & 255u) * w1 + 4u) >> 3;
    return oy | (ocb << 8) | (ocr << 16);
}

void layer0_grade_nv12_baked(const uint32_t *table, const Layer0Frame *frame,
                             unsigned first, unsigned end)
{
    if (!table || !frame || !frame->src_y || !frame->src_uv ||
        !frame->dst_y || !frame->dst_uv) {
        return;
    }
    size_t sy_stride  = frame->src_y_stride  ? frame->src_y_stride  : frame->width;
    size_t suv_stride = frame->src_uv_stride ? frame->src_uv_stride : frame->width;
    size_t dy_stride  = frame->dst_y_stride  ? frame->dst_y_stride  : frame->width;
    size_t duv_stride = frame->dst_uv_stride ? frame->dst_uv_stride : frame->width;
    unsigned width = frame->width & ~1u;
    if (end > frame->height) {
        end = frame->height;
    }
    end &= ~1u;

    for (unsigned row = first & ~1u; row < end; row += 2u) {
        const uint8_t *sy0 = frame->src_y  + (size_t)row * sy_stride;
        const uint8_t *sy1 = sy0 + sy_stride;
        const uint8_t *suv = frame->src_uv + (size_t)(row >> 1) * suv_stride;
        uint8_t       *dy0 = frame->dst_y  + (size_t)row * dy_stride;
        uint8_t       *dy1 = dy0 + dy_stride;
        uint8_t       *duv = frame->dst_uv + (size_t)(row >> 1) * duv_stride;
        unsigned d_even = (row & 2u) ? 3u : 0u;
        unsigned d_odd  = (row & 2u) ? 1u : 2u;

        for (unsigned x = 0u; x < width; x += 2u) {
            unsigned d = (x & 2u) ? d_odd : d_even;
            unsigned cb_d = (unsigned)suv[x] + d;
            unsigned cr_d = (unsigned)suv[x + 1u] + d;
            if (cb_d > 255u) cb_d = 255u;
            if (cr_d > 255u) cr_d = 255u;
            const uint32_t *col = table + ((((cr_d >> 2) << 6) | (cb_d >> 2)) * LAYER0_BAKED_LUMA);

            uint32_t a = baked_lerp_pixel(col, sy0[x]);
            uint32_t b = baked_lerp_pixel(col, sy0[x + 1u]);
            uint32_t c = baked_lerp_pixel(col, sy1[x]);
            uint32_t e = baked_lerp_pixel(col, sy1[x + 1u]);

            dy0[x]      = (uint8_t)a;
            dy0[x + 1u] = (uint8_t)b;
            dy1[x]      = (uint8_t)c;
            dy1[x + 1u] = (uint8_t)e;

            unsigned sum_cb = (unsigned)(uint8_t)(a >> 8)  + (uint8_t)(b >> 8) +
                              (unsigned)(uint8_t)(c >> 8)  + (uint8_t)(e >> 8);
            unsigned sum_cr = (unsigned)(uint8_t)(a >> 16) + (uint8_t)(b >> 16) +
                              (unsigned)(uint8_t)(c >> 16) + (uint8_t)(e >> 16);
            duv[x]      = (uint8_t)((sum_cb + 2u) >> 2);
            duv[x + 1u] = (uint8_t)((sum_cr + 2u) >> 2);
        }
    }
}
