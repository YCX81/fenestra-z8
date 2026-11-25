/* SPDX-License-Identifier: MIT
 * Unified EL1 Freestanding Streaming Film Simulation Pipeline (z8_filmsim_pipeline).
 *
 * Strict Optical Chain Order (V71.3 Fuji-stripped):
 *   Stage 1: Ricoh Optical HDF (z8_hdf: Off / Weak / Medium / Strong)
 *   Stage 2: Nikon Zf 18-level Silver-Halide Grain (z8_zf_grain)
 *
 * Constraints:
 * - Freestanding C11, scalar-only orchestrator (-mgeneral-regs-only), zero FP/SIMD.
 * - Zero malloc, zero writable .data/.bss, <= 512 KiB shared caller workspace.
 * - Any Off stage = strict 0-byte copy bypass.
 * - NEF RAW strictly zero-touch (operates only on private YCbCr NV16 / NV12 / I420 planes).
 */
#ifndef Z8_FILMSIM_PIPELINE_H
#define Z8_FILMSIM_PIPELINE_H

#include <stddef.h>
#include <stdint.h>
#include "z8_filmsim_abi.h"

#define Z8_FILMSIM_PIPELINE_ABI      1U
#define Z8_FILMSIM_MAX_PLANES        7U
#define Z8_FILMSIM_MAX_WIDTH         8256U
#define Z8_FILMSIM_MAX_HEIGHT        8256U
#define Z8_FILMSIM_WORKSPACE_BYTES   524288U /* 512 KiB shared across stages */
#define Z8_FILMSIM_WORKSPACE_ALIGN   16U
#define Z8_FILMSIM_GRAIN_WS_MIN      98304U  /* 96 KiB max workspace for Grain stage */

/* Stage execution bitmask reported via stages_applied_out */
#define Z8_FS_STAGE_HDF              (1U << 0)
#define Z8_FS_STAGE_LUT              (1U << 1)
#define Z8_FS_STAGE_GRAIN            (1U << 2)
#define Z8_FS_STAGE_GRAIN_MISSING    (1U << 9)

enum z8_filmsim_format {
    Z8_FS_FMT_NV16 = 0U, /* 8-bit Y + interleaved CbCr (4:2:2, Z8 photo/preview) */
    Z8_FS_FMT_NV12 = 1U, /* 8-bit Y + interleaved CbCr (4:2:0) */
    Z8_FS_FMT_I420 = 2U  /* 8-bit Y + planar U (Cb) + planar V (Cr) (4:2:0 Live) */
};

enum z8_filmsim_result {
    Z8_FS_OK              =  0,
    Z8_FS_BYPASS          =  1, /* Dormant / all stages Off: 0 bytes touched */
    Z8_FS_CANCELLED       =  2, /* Cancelled before any pixel write */
    Z8_FS_CANCELLED_DIRTY =  3, /* Cancelled mid-pipeline after partial writes */
    Z8_FS_HUNG_SMP        =  4, /* SMP helper timed out & quarantined: caller must quarantine buffers */
    Z8_FS_E_ARGUMENT      = -1,
    Z8_FS_E_FORMAT        = -2,
    Z8_FS_E_CAPACITY      = -3,
    Z8_FS_E_ALIAS         = -4,
    Z8_FS_E_WORKSPACE     = -5,
    Z8_FS_E_ASSET         = -6, /* Required asset missing (e.g. ZF_GRAIN.BIN) */
    Z8_FS_E_INTERNAL      = -7
};

/* Owned, writable CPU YCbCr plane descriptor.
 * - NV16/NV12: y, c non-NULL; u/v ignored.
 * - I420: y, u, v non-NULL; c ignored.
 * - is_full_res: 1 for primary full-res photo plane (e.g. 8256x5504 MAIN),
 *                0 for previews, thumbnails, and Live video (selects Zf reduced-res LUT).
 */
typedef struct {
    uint8_t *y;
    uint8_t *c;
    uint8_t *u;
    uint8_t *v;
    size_t   y_bytes;
    size_t   c_bytes;
    size_t   u_bytes;
    size_t   v_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t y_stride;
    uint32_t c_stride;
    uint32_t u_stride;
    uint32_t v_stride;
    uint32_t format;
    uint8_t  is_full_res;
    uint8_t  reserved[3];
} z8_filmsim_plane;

/* Optional validated card-loaded runtime assets & XM6 bridge handle.
 * zf_maps points to validated z8_zf_grain_maps (from A:\FENESTRA\ZF_GRAIN.BIN).
 * xm6_bridge points to heap-resident struct z8_xm6_bridge (NULL -> pure ARM NEON).
 * xm6_zf_raw_buf points to raw 27,588 B ZF_GRAIN.BIN (map0=+32, map1=+32+13778).
 * Exact 24B ABI size preserved inside z8_filmsim_job so filmsim_runtime_context
 * diagnostic offsets (pool_suspended == 0x1000) never shift.
 */
typedef struct {
    const void *zf_maps;
    void       *xm6_bridge;
    const void *xm6_zf_raw_buf;
} z8_filmsim_assets;

_Static_assert(sizeof(z8_filmsim_assets) == 24U, "z8_filmsim_assets ABI size must remain 24 B");

typedef struct {
    uint32_t          abi;
    uint32_t          count;
    uint32_t          analysis_index;
    uint64_t          shot_id; /* Nonzero exposure/frame identity */
    Z8FilmSimConfig   cfg;
    z8_filmsim_assets assets;
    z8_filmsim_plane  planes[Z8_FILMSIM_MAX_PLANES];
    /* Optional temp interleaved CbCr scratch for I420 when HDF is active:
     * must be >= ceil(width/2) * ceil(height/2) * 2 bytes and 4-byte aligned. */
    uint8_t          *i420_cbcr_scratch;
    size_t            i420_cbcr_bytes;
    int             (*cancelled)(void *ctx);
    void             *cancel_ctx;
    uint32_t         *stages_applied_out;
} z8_filmsim_job;

/* --- Config & CRC32 utilities (freestanding, zero libc) --- */
uint32_t z8_filmsim_crc32_ieee(const void *data, size_t bytes);
uint32_t z8_filmsim_cfg_crc32(const Z8FilmSimConfig *cfg);
void     z8_filmsim_cfg_default(Z8FilmSimConfig *cfg);
void     z8_filmsim_cfg_finalize(Z8FilmSimConfig *cfg, uint32_t seq);
int      z8_filmsim_cfg_validate(const Z8FilmSimConfig *cfg);
/* Normalize legacy FLMSIM0/1.CFG (style 2->0, 3->1, clear unused Fuji fields; preserves seq/HDF/Zf/Live). */
void     z8_filmsim_cfg_normalize(Z8FilmSimConfig *cfg);

/* Returns 1 if no optical/grain stage is enabled (strict Dormant Bypass). */
int      z8_filmsim_cfg_is_dormant(const Z8FilmSimConfig *cfg);
int      z8_filmsim_cfg_needs_hdf(const Z8FilmSimConfig *cfg);
int      z8_filmsim_cfg_needs_grain(const Z8FilmSimConfig *cfg);

/* --- Unified Pipeline Entry Points --- */

/* Execute Ricoh HDF -> Nikon Zf Grain across 1..7 planes.
 * Uses ONE caller-owned scratch workspace (<= 512 KiB). */
int z8_filmsim_apply_group(const z8_filmsim_job *job,
                           void *workspace, size_t workspace_bytes);

/* Convenience helper for single contiguous Live I420 frame (Y + U + V, width/2 chroma).
 * Always uses reduced-res grain profile (is_full_res = 0). */
int z8_filmsim_apply_live_i420(uint8_t *i420,
                               uint32_t width, uint32_t height,
                               uint64_t frame_id,
                               const Z8FilmSimConfig *cfg,
                               const z8_filmsim_assets *assets,
                               void *workspace, size_t workspace_bytes,
                               uint8_t *cbcr_scratch, size_t cbcr_scratch_bytes,
                               int (*cancelled)(void *ctx), void *cancel_ctx,
                               uint32_t *stages_applied_out);

/* V71.3 Native-NV12 Live pipeline entry: executes Stage 1 Ricoh HDF (auto-dispatches
 * LIVE_256_YC on 1024x{682,680} NV12). Does NOT run Zf Grain at 1024x682 because
 * Zf Grain is fused at final 1280x852 output inside CPU0's NV12 scaler/copy. */
int z8_filmsim_apply_live_nv12(uint8_t *y, uint32_t y_stride,
                               uint8_t *c, uint32_t c_stride,
                               uint32_t width, uint32_t height,
                               uint64_t frame_id,
                               const Z8FilmSimConfig *cfg,
                               void *workspace, size_t workspace_bytes,
                               int (*cancelled)(void *ctx), void *cancel_ctx,
                               uint32_t *stages_applied_out);

#endif /* Z8_FILMSIM_PIPELINE_H */
