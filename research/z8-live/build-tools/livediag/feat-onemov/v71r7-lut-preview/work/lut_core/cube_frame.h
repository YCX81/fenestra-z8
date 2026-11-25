/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 NV16 (4:2:2) 4 MiB Tagged YCbCr Cache Row Kernels (Scalar & AArch64 NEON).
 * Adapted from Fenestra-Z8-C311-source/modules/jpeg_lut/cube_frame.h.
 */
#ifndef V71R7_CUBE_FRAME_H
#define V71R7_CUBE_FRAME_H

#include "cube.h"

enum {
    CUBE_FRAME_CACHE_WORDS = 1u << 20, /* 1,048,576 words = 4 MiB */
    CUBE_FRAME_CACHE_BYTES = CUBE_FRAME_CACHE_WORDS * 4u
};

/* Pre-warm cache entries in [first, end) for central chroma (96..159). */
void jpeg_cube_cache_warm(const JpegCube *cube, uint32_t *cache, uint32_t first, uint32_t end);

/* Grade one NV16 (4:2:2) row of even width using the 4 MiB tagged YCbCr cache.
 * Output planes may alias input planes (in-place) or be disjoint.
 * Scalar and NEON implementations are bit-exact (0-diff). */
void     jpeg_cube_cache_row(const JpegCube *cube, uint32_t *cache,
                             const uint8_t *sy, const uint8_t *sc,
                             uint8_t *dy, uint8_t *dc, uint32_t width);
uint32_t jpeg_cube_cache_row_scalar(const JpegCube *cube, uint32_t *cache,
                                    const uint8_t *sy, const uint8_t *sc,
                                    uint8_t *dy, uint8_t *dc, uint32_t width);
uint32_t jpeg_cube_cache_row_neon(const JpegCube *cube, uint32_t *cache,
                                  const uint8_t *sy, const uint8_t *sc,
                                  uint8_t *dy, uint8_t *dc, uint32_t width);

#endif /* V71R7_CUBE_FRAME_H */
