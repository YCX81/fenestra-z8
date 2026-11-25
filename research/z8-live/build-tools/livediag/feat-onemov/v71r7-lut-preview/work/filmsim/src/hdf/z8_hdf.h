/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nikon Z8 Optical HDF Multi-Scale Scatter Contributors.
 * Clean-room first-principles optical highlight diffusion kernel (z8_hdf).
 */
#ifndef Z8_HDF_H
#define Z8_HDF_H

#include <stddef.h>
#include <stdint.h>

#define Z8_HDF_ABI 1U
#define Z8_HDF_MAX_PLANES 7U
#define Z8_HDF_MAX_WIDTH 8256U
#define Z8_HDF_MAX_HEIGHT 8256U
#define Z8_HDF_WORKSPACE_BYTES 524288U
#define Z8_HDF_WORKSPACE_ALIGNMENT 16U

enum z8_diffusion_mode {
    Z8_DIFFUSION_BLOOM = 0,
    Z8_DIFFUSION_HDF   = 1
};

enum z8_hdf_level {
    Z8_HDF_OFF    = 0,
    Z8_HDF_WEAK   = 1,
    Z8_HDF_MEDIUM = 2,
    Z8_HDF_STRONG = 3
};

enum z8_hdf_format {
    Z8_HDF_YCBCR422 = 0,
    Z8_HDF_YCBCR420 = 1
};

enum z8_hdf_result {
    Z8_HDF_OK              =  0,
    Z8_HDF_DISABLED        =  1,
    Z8_HDF_CANCELLED       =  2,
    Z8_HDF_CANCELLED_DIRTY =  3,
    Z8_HDF_HUNG_SMP        =  4,
    Z8_HDF_E_ARGUMENT      = -1,
    Z8_HDF_E_FORMAT        = -2,
    Z8_HDF_E_CAPACITY      = -3,
    Z8_HDF_E_ALIAS         = -4,
    Z8_HDF_E_SHOT          = -5,
    Z8_HDF_E_WORKSPACE     = -6,
    Z8_HDF_E_POOL          = -7,
    Z8_HDF_E_INTERNAL      = -8
};

/* Full-range BT.601, 8-bit Y and interleaved CbCr (Cb first: NV16 / NV12).
 * Owned, writable CPU buffers only: never a rotating ISP/DMA/Live source.
 * All pointers and strides must be 4-byte aligned; width multiple of 4.
 * 420 permits an odd luma height: chroma has ceil(height/2) rows.
 * y_bytes/c_bytes include any stride padding and are verified before reads.
 * Binary layout is 100% drop-in compatible with z8_bloom_plane.
 */
typedef struct {
    uint8_t *y, *c;
    size_t y_bytes, c_bytes;
    uint32_t width, height, y_stride, c_stride, format;
    uint64_t shot_id;
} z8_hdf_plane;

/* Exposure-group job descriptor. Binary layout is 100% drop-in compatible
 * with z8_bloom_job so callers can cast or share descriptors directly.
 */
typedef struct {
    uint32_t abi, level, count, analysis_index;
    uint64_t shot_id; /* nonzero caller-assigned exposure identity */
    z8_hdf_plane planes[Z8_HDF_MAX_PLANES];
    uint32_t monochrome, exposure_known;
    int32_t scene_ev256; /* EV100 * 256; ignored when exposure_known == 0 */
    uint8_t white[3];    /* output RGB white point, normally {255, 255, 255} */
    int (*cancelled)(void *context);
    void *cancel_context;
} z8_hdf_job;

/* Optional fine-grained optical mist customization for z8_hdf_apply_ex.
 * Passing NULL uses calibrated Ricoh-HDF-style physical defaults.
 */
typedef struct {
    uint16_t neutrality_q8; /* 0 = pure source chromaticity, 256 = pure neutral white mist (default: 96) */
    uint16_t knee_q16;      /* highlight recovery onset in linear Q16 [0..65535] (0 = default 22528) */
} z8_hdf_optics;

/* Synchronous, reentrant with disjoint jobs/workspaces; zero heap allocation,
 * zero mutable static state, zero floating-point instructions.
 * All planes are validated as an exposure group before any image write.
 * OFF returns Z8_HDF_DISABLED without touching planes or workspace.
 */
int z8_hdf_apply(const z8_hdf_job *job, void *workspace, size_t workspace_bytes);

int z8_hdf_apply_ex(const z8_hdf_job *job, const z8_hdf_optics *optics,
                    void *workspace, size_t workspace_bytes);

/* Live-specialized fast optical HDF for 1024x680 / 1024x682 NV12 frames
 * (LIVE_256_YC: 256x171/170 sigma-matched Q13 pyramid + Y + half-res Chroma).
 * OFF returns Z8_HDF_DISABLED without touching planes or workspace.
 * Checks cancelled(cancel_ctx) every 16 luma rows (Z8_HDF_CANCELLED before
 * touching output, Z8_HDF_CANCELLED_DIRTY after modifying output).
 */
int z8_hdf_live_nv12_1024(uint8_t *y, uint32_t y_stride,
                          uint8_t *c, uint32_t c_stride,
                          uint32_t height, uint32_t level,
                          uint32_t monochrome,
                          int (*cancelled)(void *), void *cancel_ctx,
                          void *workspace, size_t workspace_bytes);

/* EVF-specialized fast optical HDF for 1280x{850,852,854,856} NV12 frames:
 * computes 3-scale optical halo on a 256x{170,171} Q13 grid inside the existing
 * 512 KiB workspace (0 extra Class 0x24 bytes) and NEON-bilinearly composites onto
 * native crisp 1280x{850..856} pixels (~7-10x faster than general z8_hdf_apply). */
int z8_hdf_live_nv12_1280(uint8_t *y, uint32_t y_stride,
                          uint8_t *c, uint32_t c_stride,
                          uint32_t height, uint32_t level,
                          uint32_t monochrome,
                          int (*cancelled)(void *), void *cancel_ctx,
                          void *workspace, size_t workspace_bytes);

#endif /* Z8_HDF_H */
