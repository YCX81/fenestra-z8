/* SPDX-License-Identifier: MIT
 * Internal stage adapters isolating kernel headers (avoiding enum name collisions
 * between z8_filmsim_abi.h, z8_hdf.h, and z8_zf_grain.h).
 */
#ifndef Z8_FILMSIM_STAGES_H
#define Z8_FILMSIM_STAGES_H

#include <stddef.h>
#include <stdint.h>

#define Z8_FS_STAGE_MAX_PLANES 7U

/* Internal format tags matching z8_filmsim_format */
#define Z8_FS_INT_FMT_NV16 0U
#define Z8_FS_INT_FMT_NV12 1U
#define Z8_FS_INT_FMT_I420 2U

/* Stage result codes matching enum z8_filmsim_result */
#define Z8_FS_INT_OK              0
#define Z8_FS_INT_BYPASS          1
#define Z8_FS_INT_CANCELLED       2
#define Z8_FS_INT_CANCELLED_DIRTY 3
#define Z8_FS_INT_HUNG_SMP        4
#define Z8_FS_INT_E_ARGUMENT     -1
#define Z8_FS_INT_E_FORMAT       -2
#define Z8_FS_INT_E_CAPACITY     -3
#define Z8_FS_INT_E_ALIAS        -4
#define Z8_FS_INT_E_WORKSPACE    -5
#define Z8_FS_INT_E_ASSET        -6
#define Z8_FS_INT_E_INTERNAL     -7

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
} z8_fs_stage_plane;

/* Stage 1: Ricoh Optical HDF (NV16 / NV12 planes) */
int z8_fs_stage_hdf_run(uint32_t hdf_level,
                        uint64_t shot_id,
                        uint32_t count,
                        uint32_t analysis_index,
                        const z8_fs_stage_plane *planes,
                        void *workspace, size_t workspace_bytes,
                        int (*cancelled)(void *), void *cancel_ctx);

/* Stage 2: Nikon Zf 18-level Silver-Halide Grain (z8_zf_grain) */
int z8_fs_stage_grain_run(uint32_t zf_intensity,
                          uint32_t zf_size,
                          uint32_t zf_blur_mode,
                          const void *zf_maps,
                          uint64_t shot_id,
                          uint32_t count,
                          const z8_fs_stage_plane *planes,
                          void *workspace, size_t workspace_bytes,
                          int (*cancelled)(void *), void *cancel_ctx);

/* V71 Live fast path: bake seamless 512x256 twisted-torus int8_t Zf grain tile (131,072 B). */
int z8_fs_stage_grain_live_bake(uint32_t zf_intensity, uint32_t zf_size,
                                uint32_t zf_blur_mode,
                                const void *zf_maps, uint64_t seed,
                                int8_t *tile_out, size_t tile_bytes,
                                void *workspace, size_t workspace_bytes);

/* Single-plane Zf grain with explicit raw int16_t[83*83] maps & PRNG seed
 * (used by Z8_FS_OP_XM6_DIAG 0-diff comparison and per-plane XM6->ARM fallback). */
int z8_fs_stage_grain_run_raw_maps(uint32_t zf_intensity,
                                   uint32_t zf_size,
                                   uint32_t zf_blur_mode,
                                   uint8_t is_full_res,
                                   const int16_t *map0,
                                   const int16_t *map1,
                                   uint32_t prng_seed,
                                   uint8_t *y, uint32_t y_stride,
                                   uint32_t width, uint32_t height,
                                   void *workspace, size_t workspace_bytes);

#endif /* Z8_FILMSIM_STAGES_H */
