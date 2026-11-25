/* SPDX-License-Identifier: MIT
 * Stage 2 Adapter: Nikon Zf 18-level Silver-Halide Grain (z8_zf_grain)
 * and V71 Live pre-baked 512x256 twisted-torus grain tile baking.
 * Operates strictly on 8-bit luma Plane 0 (Y); Cb/Cr/U/V are strictly untouched.
 * Freestanding C11, scalar-only (-mgeneral-regs-only), zero malloc, zero .data/.bss.
 */
#include "z8_filmsim_stages.h"
#include "z8_zf_grain.h"

static int resolve_grain_preset(uint32_t zf_intensity,
                                uint32_t zf_size,
                                uint32_t zf_blur_mode,
                                uint8_t reduced_res,
                                z8_zf_grain_preset *out)
{
    out->intensity = 0U;
    out->size = Z8_ZF_GRAIN_SIZE_MIN;
    out->blur_mode = Z8_ZF_GRAIN_BLUR_SHARP;
    out->reduced_res = reduced_res;

    if (zf_intensity == 0U) {
        return Z8_FS_INT_BYPASS;
    }
    if (zf_intensity > Z8_ZF_GRAIN_INTENSITY_MAX ||
        zf_size < Z8_ZF_GRAIN_SIZE_MIN || zf_size > Z8_ZF_GRAIN_SIZE_MAX ||
        zf_blur_mode > Z8_ZF_GRAIN_BLUR_SHARP) {
        return Z8_FS_INT_E_ARGUMENT;
    }
    out->intensity = (uint8_t)zf_intensity;
    out->size = (uint8_t)zf_size;
    out->blur_mode = (uint8_t)zf_blur_mode;
    out->reduced_res = reduced_res;
    return Z8_FS_INT_OK;
}

static int map_grain_rc(int rc)
{
    switch (rc) {
    case Z8_ZF_GRAIN_OK:              return Z8_FS_INT_OK;
    case Z8_ZF_GRAIN_DISABLED:        return Z8_FS_INT_BYPASS;
    case Z8_ZF_GRAIN_CANCELLED:       return Z8_FS_INT_CANCELLED;
    case Z8_ZF_GRAIN_CANCELLED_DIRTY: return Z8_FS_INT_CANCELLED_DIRTY;
    case Z8_ZF_GRAIN_E_FORMAT:        return Z8_FS_INT_E_FORMAT;
    case Z8_ZF_GRAIN_E_CAPACITY:      return Z8_FS_INT_E_CAPACITY;
    case Z8_ZF_GRAIN_E_ALIAS:         return Z8_FS_INT_E_ALIAS;
    case Z8_ZF_GRAIN_E_WORKSPACE:     return Z8_FS_INT_E_WORKSPACE;
    case Z8_ZF_GRAIN_E_MAPS:          return Z8_FS_INT_E_ASSET;
    case Z8_ZF_GRAIN_E_ARGUMENT:      return Z8_FS_INT_E_ARGUMENT;
    default:                          return Z8_FS_INT_E_INTERNAL;
    }
}

int z8_fs_stage_grain_run(uint32_t zf_intensity,
                          uint32_t zf_size,
                          uint32_t zf_blur_mode,
                          const void *zf_maps,
                          uint64_t shot_id,
                          uint32_t count,
                          const z8_fs_stage_plane *planes,
                          void *workspace, size_t workspace_bytes,
                          int (*cancelled)(void *), void *cancel_ctx)
{
    if (!planes || count == 0U || count > Z8_ZF_GRAIN_MAX_PLANES || shot_id == 0U) {
        return Z8_FS_INT_E_ARGUMENT;
    }

    z8_zf_grain_preset preset;
    int prc = resolve_grain_preset(zf_intensity, zf_size, zf_blur_mode, 0U, &preset);
    if (prc != Z8_FS_INT_OK) {
        return prc;
    }

    if (!zf_maps) {
        return Z8_FS_INT_E_ASSET;
    }
    if (!workspace || workspace_bytes < Z8_ZF_GRAIN_WORKSPACE_BYTES) {
        return Z8_FS_INT_E_WORKSPACE;
    }

    z8_zf_grain_group_job job;
    job.abi = Z8_ZF_GRAIN_ABI;
    job.count = count;
    job.shot_id = shot_id;
    job.maps = (const z8_zf_grain_maps *)zf_maps;
    job.preset = preset;
    job.cancelled = cancelled;
    job.cancel_context = cancel_ctx;

    for (uint32_t i = 0U; i < count; ++i) {
        const z8_fs_stage_plane *sp = &planes[i];
        z8_zf_grain_plane *dp = &job.planes[i];
        dp->y = sp->y;
        dp->y_bytes = sp->y_bytes;
        dp->width = sp->width;
        dp->height = sp->height;
        dp->y_stride = sp->y_stride;
        dp->res_mode = sp->is_full_res ? Z8_ZF_GRAIN_RES_FULL : Z8_ZF_GRAIN_RES_REDUCED;
        dp->reserved[0] = 0U;
        dp->reserved[1] = 0U;
        dp->reserved[2] = 0U;
    }

    int rc = z8_zf_grain_apply_group(&job, workspace, workspace_bytes);
    return map_grain_rc(rc);
}

int z8_fs_stage_grain_live_bake(uint32_t zf_intensity, uint32_t zf_size,
                                uint32_t zf_blur_mode,
                                const void *zf_maps, uint64_t seed,
                                int8_t *tile_out, size_t tile_bytes,
                                void *workspace, size_t workspace_bytes)
{
    z8_zf_grain_preset preset;
    int prc = resolve_grain_preset(zf_intensity, zf_size, zf_blur_mode, 1U, &preset);
    if (prc != Z8_FS_INT_OK) {
        return prc;
    }

    if (!zf_maps) {
        return Z8_FS_INT_E_ASSET;
    }
    if (!tile_out) {
        return Z8_FS_INT_E_ARGUMENT;
    }
    if (tile_bytes < Z8_ZF_LIVE_TILE_BYTES) {
        return Z8_FS_INT_E_CAPACITY;
    }
    if (!workspace || workspace_bytes < Z8_ZF_GRAIN_WORKSPACE_BYTES) {
        return Z8_FS_INT_E_WORKSPACE;
    }

    int rc = z8_zf_grain_live_bake_tile(&preset,
                                        (const z8_zf_grain_maps *)zf_maps,
                                        seed,
                                        tile_out, tile_bytes,
                                        workspace, workspace_bytes);
    return map_grain_rc(rc);
}

int z8_fs_stage_grain_run_raw_maps(uint32_t zf_intensity,
                                   uint32_t zf_size,
                                   uint32_t zf_blur_mode,
                                   uint8_t is_full_res,
                                   const int16_t *map0,
                                   const int16_t *map1,
                                   uint32_t prng_seed,
                                   uint8_t *y, uint32_t y_stride,
                                   uint32_t width, uint32_t height,
                                   void *workspace, size_t workspace_bytes)
{
    z8_zf_grain_preset preset;
    int prc = resolve_grain_preset(zf_intensity, zf_size, zf_blur_mode,
                                   is_full_res ? 0U : 1U, &preset);
    if (prc != Z8_FS_INT_OK) {
        return prc;
    }
    if (!map0 || !map1) {
        return Z8_FS_INT_E_ASSET;
    }
    if (!y || !width || !height || y_stride < width) {
        return Z8_FS_INT_E_FORMAT;
    }
    if (!workspace || workspace_bytes < Z8_ZF_GRAIN_WORKSPACE_BYTES) {
        return Z8_FS_INT_E_WORKSPACE;
    }

    z8_zf_grain_dsp_params dsp;
    int rrc = z8_zf_grain_resolve_preset(&preset, &dsp);
    if (rrc != Z8_ZF_GRAIN_OK) {
        return map_grain_rc(rrc);
    }

    z8_zf_grain_maps maps;
    maps.map0 = map0;
    maps.map1 = map1;

    z8_zf_grain_plane_job pj;
    pj.abi            = Z8_ZF_GRAIN_ABI;
    pj.maps           = &maps;
    pj.params         = dsp;
    pj.src_y          = y;
    pj.src_y_bytes    = (size_t)y_stride * height;
    pj.src_y_stride   = y_stride;
    pj.dst_y          = y;
    pj.dst_y_bytes    = (size_t)y_stride * height;
    pj.dst_y_stride   = y_stride;
    pj.width          = width;
    pj.height         = height;
    pj.prng_seed      = prng_seed ? prng_seed : Z8_ZF_GRAIN_DEFAULT_PRNG_SEED;
    pj.cancelled      = NULL;
    pj.cancel_context = NULL;

    int rc = z8_zf_grain_process_plane(&pj, workspace, workspace_bytes);
    return map_grain_rc(rc);
}
