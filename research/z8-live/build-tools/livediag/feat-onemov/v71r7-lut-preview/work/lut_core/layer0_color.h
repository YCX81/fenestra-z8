/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 NV12 (4:2:0) 4 MiB Tagged YCbCr Cache Frame Kernels (Scalar & AArch64 NEON).
 * Adapted from Fenestra-Z8-C311-source/modules/lut_preview/layer0_color.h.
 * Extended to support arbitrary y_stride / uv_stride (LCD 1024x682, EVF 1280x852, Live).
 */
#ifndef V71R7_LAYER0_COLOR_H
#define V71R7_LAYER0_COLOR_H

#include <stddef.h>
#include <stdint.h>
#include "cube_frame.h"

enum {
    REAR_W     = 1024u,
    REAR_H     = 682u,
    REAR_NV12  = REAR_W * REAR_H * 3u / 2u,
    REAR_CACHE = CUBE_FRAME_CACHE_WORDS * 4u,
    /* V71.7-r7 L2-resident 64x64x33 pre-baked YCbCr 3D LUT (528 KiB):
     * 0 tag checks, 0 cache misses, exact neutral gray (128,128 -> idx 32),
     * 2x2 Bayer chroma dither (0 banding) + 3-bit Y-linear interpolation! */
    LAYER0_BAKED_CHROMA = 64u,
    LAYER0_BAKED_LUMA   = 33u,
    LAYER0_BAKED_WORDS  = LAYER0_BAKED_CHROMA * LAYER0_BAKED_CHROMA * LAYER0_BAKED_LUMA,
    LAYER0_BAKED_BYTES  = LAYER0_BAKED_WORDS * 4u
};

typedef struct {
    const uint8_t *src_y, *src_uv;
    uint8_t       *dst_y, *dst_uv;
    uint32_t       width, height;
    uint32_t       src_y_stride, src_uv_stride; /* 0 -> defaults to width */
    uint32_t       dst_y_stride, dst_uv_stride; /* 0 -> defaults to width */
} Layer0Frame;

/* Grade NV12 rows [first, end) (even boundaries) using the 4 MiB tagged YCbCr cache.
 * Scalar and NEON implementations are bit-exact (0-diff). */
void layer0_grade_nv12_planes(const JpegCube *cube, uint32_t *cache,
                              const Layer0Frame *frame, unsigned first, unsigned end);
void layer0_grade_nv12_planes_neon(const JpegCube *cube, uint32_t *cache,
                                   const Layer0Frame *frame, unsigned first, unsigned end);

/* Packed 1024x682 NV12 convenience wrapper (scalar) */
int  layer0_grade_nv12_rows(const JpegCube *cube, uint32_t *cache,
                            const uint8_t *src_nv12, size_t src_bytes,
                            uint8_t *dst_nv12, size_t dst_bytes,
                            unsigned first_row, unsigned end_row);

/* Packed arbitrary-even-dimension NV12 convenience wrapper (NEON) */
int  layer0_grade_nv12_rows_neon(const JpegCube *cube, uint32_t *cache,
                                 const uint8_t *src_nv12, size_t src_bytes,
                                 uint8_t *dst_nv12, size_t dst_bytes,
                                 unsigned first_row, unsigned end_row,
                                 unsigned width, unsigned height);

/* Scalar fallback for 2-row NV12 quads (called by NEON on cache tag miss / tail) */
void layer0_grade_nv12_pair_scalar(const JpegCube *cube, uint32_t *cache,
                                   const uint8_t *src_y, const uint8_t *src_y1,
                                   const uint8_t *src_uv,
                                   uint8_t *dst_y, uint8_t *dst_y1,
                                   uint8_t *dst_uv, unsigned width);

/* V71.7-r7 528 KiB L2-Resident 64x64x33 Baked YCbCr 3D LUT (Scalar & NEON 0-diff) */
void layer0_bake_ycbcr_64x64x33(const JpegCube *cube, uint32_t *table,
                                uint32_t first_cr, uint32_t end_cr);
void layer0_grade_nv12_baked(const uint32_t *table, const Layer0Frame *frame,
                             unsigned first, unsigned end);
void layer0_grade_nv12_baked_neon(const uint32_t *table, const Layer0Frame *frame,
                                  unsigned first, unsigned end);

/* V71.7-r7 AArch64 NEON streaming copy with optional inline dc ivac + dsb sy (0 Q-spills) */
void layer0_copy_plane_neon(uint8_t *dst, const uint8_t *src, size_t bytes, int do_dc_ivac);

#endif /* V71R7_LAYER0_COLOR_H */
