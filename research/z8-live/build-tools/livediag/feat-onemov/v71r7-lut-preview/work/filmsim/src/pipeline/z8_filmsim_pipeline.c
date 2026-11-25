/* SPDX-License-Identifier: MIT
 * Unified EL1 Freestanding Film Simulation Streaming Pipeline Orchestrator (V71.3).
 * Strict optical chain: Stage 1 (Ricoh Optical HDF) -> Stage 2 (Nikon Zf Film Grain).
 * Zero malloc, zero writable .data/.bss, scalar-only (-mgeneral-regs-only), zero libc.
 */
#include "z8_filmsim_pipeline.h"
#include "z8_filmsim_stages.h"

static void fs_zero(void *dst, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0U; i < n; ++i) {
        p[i] = 0U;
    }
}

static void fs_copy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0U; i < n; ++i) {
        d[i] = s[i];
    }
}

uint32_t z8_filmsim_crc32_ieee(const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0U; i < bytes; ++i) {
        crc ^= (uint32_t)p[i];
        for (uint32_t b = 0U; b < 8U; ++b) {
            uint32_t mask = (uint32_t)(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

uint32_t z8_filmsim_cfg_crc32(const Z8FilmSimConfig *cfg)
{
    uint8_t buf[Z8_FILMSIM_CFG_BYTES];
    fs_copy(buf, cfg, Z8_FILMSIM_CFG_BYTES);
    buf[12] = 0U;
    buf[13] = 0U;
    buf[14] = 0U;
    buf[15] = 0U;
    return z8_filmsim_crc32_ieee(buf, Z8_FILMSIM_CFG_BYTES);
}

void z8_filmsim_cfg_finalize(Z8FilmSimConfig *cfg, uint32_t seq)
{
    if (!cfg) {
        return;
    }
    cfg->magic = Z8_FILMSIM_CFG_MAGIC;
    cfg->version = (uint16_t)Z8_FILMSIM_CFG_VERSION;
    cfg->bytes = (uint16_t)Z8_FILMSIM_CFG_BYTES;
    cfg->seq = seq;
    fs_zero(cfg->reserved, sizeof(cfg->reserved));
    cfg->crc32 = 0U;
    cfg->crc32 = z8_filmsim_cfg_crc32(cfg);
}

void z8_filmsim_cfg_default(Z8FilmSimConfig *cfg)
{
    if (!cfg) {
        return;
    }
    fs_zero(cfg, sizeof(*cfg));
    cfg->zf_size = Z8_ZF_SIZE_DEFAULT;
    cfg->zf_blur_mode = Z8_ZF_BLUR_DEFAULT;
    cfg->fuji_lut_strength = Z8_FUJI_LUT_STRENGTH_DEFAULT;
    z8_filmsim_cfg_finalize(cfg, 0U);
}

int z8_filmsim_cfg_validate(const Z8FilmSimConfig *cfg)
{
    if (!cfg) {
        return 0;
    }
    if (cfg->magic != Z8_FILMSIM_CFG_MAGIC ||
        cfg->version != Z8_FILMSIM_CFG_VERSION ||
        cfg->bytes != Z8_FILMSIM_CFG_BYTES) {
        return 0;
    }
    /* Accept legacy film_style 2 (FUJIFILM) & 3 (FUJI_ZF) + legacy Fuji ranges so
     * existing FLMSIM0/1.CFG files on SD/CF cards pass CRC32 before normalization. */
    if (cfg->film_style > Z8_STYLE_FUJI_ZF ||
        cfg->zf_intensity > Z8_ZF_INTENSITY_MAX ||
        cfg->zf_size < Z8_ZF_SIZE_MIN ||
        cfg->zf_size > Z8_ZF_SIZE_MAX ||
        cfg->zf_blur_mode > Z8_ZF_BLUR_SHARP ||
        cfg->fuji_chrome > Z8_CHROME_STRONG ||
        cfg->fuji_fx_blue > Z8_CHROME_STRONG ||
        cfg->fuji_lut_slot > Z8_FUJI_LUT_SLOT_MAX ||
        cfg->fuji_lut_strength > Z8_FUJI_LUT_STRENGTH_MAX ||
        cfg->fuji_grain > Z8_FUJI_GRAIN_STRONG_L ||
        cfg->hdf_level > Z8_HDF_STRONG ||
        cfg->live_link > Z8_LIVE_LINK_ON ||
        cfg->apply_mode > Z8_FS_APPLY_PER_NP3 ||
        cfg->xm6_mode > Z8_XM6_MODE_ARM_ONLY ||
        cfg->wysiwyg_preview > Z8_WYSIWYG_ON) {
        return 0;
    }
    for (uint32_t i = 0U; i < (uint32_t)sizeof(cfg->reserved); ++i) {
        if (cfg->reserved[i] != 0U) {
            return 0;
        }
    }
    return cfg->crc32 == z8_filmsim_cfg_crc32(cfg);
}

void z8_filmsim_cfg_normalize(Z8FilmSimConfig *cfg)
{
    if (!cfg) {
        return;
    }
    int changed = 0;
    if (cfg->film_style == Z8_STYLE_FUJIFILM) {
        cfg->film_style = Z8_STYLE_OFF;
        changed = 1;
    } else if (cfg->film_style == Z8_STYLE_FUJI_ZF) {
        cfg->film_style = Z8_STYLE_NIKON_ZF;
        changed = 1;
    }
    if (cfg->fuji_chrome != Z8_CHROME_OFF) {
        cfg->fuji_chrome = Z8_CHROME_OFF;
        changed = 1;
    }
    if (cfg->fuji_fx_blue != Z8_CHROME_OFF) {
        cfg->fuji_fx_blue = Z8_CHROME_OFF;
        changed = 1;
    }
    if (cfg->fuji_lut_slot != 0U) {
        cfg->fuji_lut_slot = 0U;
        changed = 1;
    }
    if (cfg->fuji_lut_strength != Z8_FUJI_LUT_STRENGTH_DEFAULT) {
        cfg->fuji_lut_strength = Z8_FUJI_LUT_STRENGTH_DEFAULT;
        changed = 1;
    }
    if (cfg->fuji_grain != Z8_FUJI_GRAIN_OFF) {
        cfg->fuji_grain = Z8_FUJI_GRAIN_OFF;
        changed = 1;
    }
    if (changed) {
        z8_filmsim_cfg_finalize(cfg, cfg->seq);
    }
}

int z8_filmsim_cfg_needs_hdf(const Z8FilmSimConfig *cfg)
{
    return cfg && cfg->hdf_level >= Z8_HDF_WEAK && cfg->hdf_level <= Z8_HDF_STRONG;
}

int z8_filmsim_cfg_needs_grain(const Z8FilmSimConfig *cfg)
{
    if (!cfg) {
        return 0;
    }
    /* Legacy style=3 (FUJI_ZF) maps cleanly to Nikon Zf grain; style=2 (FUJI) maps to OFF. */
    if ((cfg->film_style == Z8_STYLE_NIKON_ZF ||
         cfg->film_style == Z8_STYLE_FUJI_ZF) &&
        cfg->zf_intensity > 0U) {
        return 1;
    }
    return 0;
}

int z8_filmsim_cfg_is_dormant(const Z8FilmSimConfig *cfg)
{
    if (!cfg) {
        return 1;
    }
    return !z8_filmsim_cfg_needs_hdf(cfg) &&
           !z8_filmsim_cfg_needs_grain(cfg);
}

static int is_cancelled(const z8_filmsim_job *job)
{
    return job->cancelled && job->cancelled(job->cancel_ctx);
}

static void i420_interleave_uv(const uint8_t *u, uint32_t u_stride,
                               const uint8_t *v, uint32_t v_stride,
                               uint8_t *cbcr, uint32_t c_stride,
                               uint32_t cw, uint32_t ch)
{
    for (uint32_t y = 0U; y < ch; ++y) {
        const uint8_t *ur = u + (size_t)y * u_stride;
        const uint8_t *vr = v + (size_t)y * v_stride;
        uint8_t *dr = cbcr + (size_t)y * c_stride;
        for (uint32_t x = 0U; x < cw; ++x) {
            dr[2U * x] = ur[x];
            dr[2U * x + 1U] = vr[x];
        }
    }
}

static void i420_deinterleave_uv(const uint8_t *cbcr, uint32_t c_stride,
                                 uint8_t *u, uint32_t u_stride,
                                 uint8_t *v, uint32_t v_stride,
                                 uint32_t cw, uint32_t ch)
{
    for (uint32_t y = 0U; y < ch; ++y) {
        const uint8_t *sr = cbcr + (size_t)y * c_stride;
        uint8_t *ur = u + (size_t)y * u_stride;
        uint8_t *vr = v + (size_t)y * v_stride;
        for (uint32_t x = 0U; x < cw; ++x) {
            ur[x] = sr[2U * x];
            vr[x] = sr[2U * x + 1U];
        }
    }
}

int z8_filmsim_apply_group(const z8_filmsim_job *job,
                           void *workspace, size_t workspace_bytes)
{
    if (job && job->stages_applied_out) {
        *job->stages_applied_out = 0U;
    }
    if (!job || job->abi != Z8_FILMSIM_PIPELINE_ABI ||
        !z8_filmsim_cfg_validate(&job->cfg)) {
        return Z8_FS_E_ARGUMENT;
    }

    /* Strict Dormant / Off Bypass: touch 0 bytes of planes or workspace. */
    if (z8_filmsim_cfg_is_dormant(&job->cfg)) {
        return Z8_FS_BYPASS;
    }

    if (job->count == 0U || job->count > Z8_FILMSIM_MAX_PLANES ||
        job->analysis_index >= job->count || job->shot_id == 0U) {
        return Z8_FS_E_ARGUMENT;
    }

    int want_hdf = z8_filmsim_cfg_needs_hdf(&job->cfg);
    int want_grain = z8_filmsim_cfg_needs_grain(&job->cfg);
    uint32_t stages = 0U;
    int run_grain = 0;

    if (want_grain) {
        if (job->assets.zf_maps != NULL) {
            run_grain = 1;
        } else {
            stages |= Z8_FS_STAGE_GRAIN_MISSING;
        }
    }

    if (!want_hdf && !run_grain) {
        if (job->stages_applied_out) {
            *job->stages_applied_out = stages;
        }
        return Z8_FS_E_ASSET;
    }

    size_t req_ws = want_hdf ? Z8_FILMSIM_WORKSPACE_BYTES : Z8_FILMSIM_GRAIN_WS_MIN;
    if (!workspace || ((uintptr_t)workspace & (Z8_FILMSIM_WORKSPACE_ALIGN - 1U)) != 0U ||
        workspace_bytes < req_ws) {
        return Z8_FS_E_WORKSPACE;
    }

    /* Validate plane descriptors and build stage view */
    z8_fs_stage_plane spl[Z8_FILMSIM_MAX_PLANES];
    int has_i420 = 0;
    for (uint32_t i = 0U; i < job->count; ++i) {
        const z8_filmsim_plane *p = &job->planes[i];
        if (!p->y || p->width == 0U || p->width > Z8_FILMSIM_MAX_WIDTH ||
            p->height == 0U || p->height > Z8_FILMSIM_MAX_HEIGHT ||
            p->y_stride < p->width || p->format > Z8_FS_FMT_I420) {
            return Z8_FS_E_FORMAT;
        }
        size_t min_y = (size_t)(p->height - 1U) * p->y_stride + p->width;
        if (p->y_bytes < min_y) {
            return Z8_FS_E_CAPACITY;
        }
        if (p->format == Z8_FS_FMT_I420) {
            has_i420 = 1;
            uint32_t cw = (p->width + 1U) >> 1;
            uint32_t ch = (p->height + 1U) >> 1;
            if (!p->u || !p->v || p->u_stride < cw || p->v_stride < cw) {
                return Z8_FS_E_FORMAT;
            }
            size_t min_u = (size_t)(ch - 1U) * p->u_stride + cw;
            size_t min_v = (size_t)(ch - 1U) * p->v_stride + cw;
            if (p->u_bytes < min_u || p->v_bytes < min_v) {
                return Z8_FS_E_CAPACITY;
            }
        } else if (want_hdf || p->c != NULL) {
            uint32_t ch = (p->format == Z8_FS_FMT_NV12) ? ((p->height + 1U) >> 1) : p->height;
            if (!p->c || p->c_stride < p->width) {
                return Z8_FS_E_FORMAT;
            }
            size_t min_c = (size_t)(ch - 1U) * p->c_stride + p->width;
            if (p->c_bytes < min_c) {
                return Z8_FS_E_CAPACITY;
            }
        }

        spl[i].y = p->y;
        spl[i].c = p->c;
        spl[i].u = p->u;
        spl[i].v = p->v;
        spl[i].y_bytes = p->y_bytes;
        spl[i].c_bytes = p->c_bytes;
        spl[i].u_bytes = p->u_bytes;
        spl[i].v_bytes = p->v_bytes;
        spl[i].width = p->width;
        spl[i].height = p->height;
        spl[i].y_stride = p->y_stride;
        spl[i].c_stride = p->c_stride;
        spl[i].u_stride = p->u_stride;
        spl[i].v_stride = p->v_stride;
        spl[i].format = p->format;
        spl[i].is_full_res = p->is_full_res;
        spl[i].reserved[0] = 0U;
        spl[i].reserved[1] = 0U;
        spl[i].reserved[2] = 0U;
    }

    /* I420 + HDF requires single-plane scratch CbCr for NV12 interleave */
    int i420_interleaved = 0;
    uint32_t i420_cw = 0U, i420_ch = 0U, i420_cstride = 0U;
    if (want_hdf && has_i420) {
        if (job->count != 1U || (spl[0].width & 3U) != 0U) {
            return Z8_FS_E_FORMAT;
        }
        i420_cw = spl[0].width >> 1;
        i420_ch = (spl[0].height + 1U) >> 1;
        i420_cstride = spl[0].width;
        size_t need_c = (size_t)i420_ch * i420_cstride;
        if (!job->i420_cbcr_scratch ||
            (((uintptr_t)job->i420_cbcr_scratch) & 3U) != 0U ||
            job->i420_cbcr_bytes < need_c) {
            return Z8_FS_E_CAPACITY;
        }
        /* Check scratch does not alias workspace or I420 buffers */
        uintptr_t cs0 = (uintptr_t)job->i420_cbcr_scratch;
        uintptr_t cs1 = cs0 + need_c;
        uintptr_t ws0 = (uintptr_t)workspace;
        uintptr_t ws1 = ws0 + workspace_bytes;
        if (cs0 < ws1 && ws0 < cs1) {
            return Z8_FS_E_ALIAS;
        }
    }

    if (is_cancelled(job)) {
        return Z8_FS_CANCELLED;
    }

    int dirty = 0;

    /* --- Stage 1: Ricoh Optical HDF --- */
    if (want_hdf) {
        if (has_i420) {
            i420_interleave_uv(spl[0].u, spl[0].u_stride,
                               spl[0].v, spl[0].v_stride,
                               job->i420_cbcr_scratch, i420_cstride,
                               i420_cw, i420_ch);
            spl[0].c = job->i420_cbcr_scratch;
            spl[0].c_bytes = (size_t)i420_ch * i420_cstride;
            spl[0].c_stride = i420_cstride;
            spl[0].format = Z8_FS_INT_FMT_NV12;
            i420_interleaved = 1;
        }

        int rc = z8_fs_stage_hdf_run(job->cfg.hdf_level,
                                     job->shot_id,
                                     job->count,
                                     job->analysis_index,
                                     spl,
                                     workspace, workspace_bytes,
                                     job->cancelled, job->cancel_ctx);
        if (rc == Z8_FS_INT_OK) {
            dirty = 1;
            stages |= Z8_FS_STAGE_HDF;
        } else if (rc != Z8_FS_INT_BYPASS) {
            if (i420_interleaved && rc == Z8_FS_INT_CANCELLED_DIRTY) {
                i420_deinterleave_uv(job->i420_cbcr_scratch, i420_cstride,
                                     spl[0].u, spl[0].u_stride,
                                     spl[0].v, spl[0].v_stride,
                                     i420_cw, i420_ch);
            }
            if (job->stages_applied_out) {
                *job->stages_applied_out = stages;
            }
            return rc;
        }
    }

    /* Deinterleave temporary NV12 chroma back to planar I420 U/V before Grain (Y-only) */
    if (i420_interleaved) {
        i420_deinterleave_uv(job->i420_cbcr_scratch, i420_cstride,
                             spl[0].u, spl[0].u_stride,
                             spl[0].v, spl[0].v_stride,
                             i420_cw, i420_ch);
        spl[0].c = NULL;
        spl[0].c_bytes = 0U;
        spl[0].c_stride = 0U;
        spl[0].format = Z8_FS_INT_FMT_I420;
        i420_interleaved = 0;
    }

    if (is_cancelled(job)) {
        if (job->stages_applied_out) {
            *job->stages_applied_out = stages;
        }
        return dirty ? Z8_FS_CANCELLED_DIRTY : Z8_FS_CANCELLED;
    }

    /* --- Stage 2: Nikon Zf 18-level Silver-Halide Grain (z8_zf_grain) --- */
    if (run_grain) {
        int rc = z8_fs_stage_grain_run(job->cfg.zf_intensity,
                                       job->cfg.zf_size,
                                       job->cfg.zf_blur_mode,
                                       job->assets.zf_maps,
                                       job->shot_id,
                                       job->count,
                                       spl,
                                       workspace, workspace_bytes,
                                       job->cancelled, job->cancel_ctx);
        if (rc == Z8_FS_INT_OK) {
            dirty = 1;
            stages |= Z8_FS_STAGE_GRAIN;
        } else if (rc != Z8_FS_INT_BYPASS) {
            if (job->stages_applied_out) {
                *job->stages_applied_out = stages;
            }
            return (rc == Z8_FS_INT_CANCELLED && dirty) ? Z8_FS_CANCELLED_DIRTY : rc;
        }
    }

    if (job->stages_applied_out) {
        *job->stages_applied_out = stages;
    }
    return dirty ? Z8_FS_OK : Z8_FS_BYPASS;
}

int z8_filmsim_apply_live_i420(uint8_t *i420,
                               uint32_t width, uint32_t height,
                               uint64_t frame_id,
                               const Z8FilmSimConfig *cfg,
                               const z8_filmsim_assets *assets,
                               void *workspace, size_t workspace_bytes,
                               uint8_t *cbcr_scratch, size_t cbcr_scratch_bytes,
                               int (*cancelled)(void *ctx), void *cancel_ctx,
                               uint32_t *stages_applied_out)
{
    if (stages_applied_out) {
        *stages_applied_out = 0U;
    }
    if (!i420 || !cfg || width < 4U || height < 2U || (width & 1U) || (height & 1U)) {
        return Z8_FS_E_FORMAT;
    }
    if (!z8_filmsim_cfg_validate(cfg)) {
        return Z8_FS_E_ARGUMENT;
    }
    if (cfg->live_link == Z8_LIVE_LINK_OFF || z8_filmsim_cfg_is_dormant(cfg)) {
        return Z8_FS_BYPASS;
    }

    size_t y_bytes = (size_t)width * height;
    uint32_t cw = width >> 1;
    uint32_t ch = height >> 1;
    size_t uv_bytes = (size_t)cw * ch;

    z8_filmsim_job job;
    fs_zero(&job, sizeof(job));
    job.abi = Z8_FILMSIM_PIPELINE_ABI;
    job.count = 1U;
    job.analysis_index = 0U;
    job.shot_id = frame_id ? frame_id : 1U;
    job.cfg = *cfg;
    if (assets) {
        job.assets = *assets;
    }
    job.planes[0].y = i420;
    job.planes[0].u = i420 + y_bytes;
    job.planes[0].v = i420 + y_bytes + uv_bytes;
    job.planes[0].y_bytes = y_bytes;
    job.planes[0].u_bytes = uv_bytes;
    job.planes[0].v_bytes = uv_bytes;
    job.planes[0].width = width;
    job.planes[0].height = height;
    job.planes[0].y_stride = width;
    job.planes[0].u_stride = cw;
    job.planes[0].v_stride = cw;
    job.planes[0].format = Z8_FS_FMT_I420;
    job.planes[0].is_full_res = 0U; /* Live video always uses reduced-res grain profile */
    job.i420_cbcr_scratch = cbcr_scratch;
    job.i420_cbcr_bytes = cbcr_scratch_bytes;
    job.cancelled = cancelled;
    job.cancel_ctx = cancel_ctx;
    job.stages_applied_out = stages_applied_out;

    return z8_filmsim_apply_group(&job, workspace, workspace_bytes);
}

int z8_filmsim_apply_live_nv12(uint8_t *y, uint32_t y_stride,
                               uint8_t *c, uint32_t c_stride,
                               uint32_t width, uint32_t height,
                               uint64_t frame_id,
                               const Z8FilmSimConfig *cfg,
                               void *workspace, size_t workspace_bytes,
                               int (*cancelled)(void *ctx), void *cancel_ctx,
                               uint32_t *stages_applied_out)
{
    if (stages_applied_out) {
        *stages_applied_out = 0U;
    }
    if (!y || !c || !cfg || width < 4U || height < 2U ||
        width > Z8_FILMSIM_MAX_WIDTH || height > Z8_FILMSIM_MAX_HEIGHT ||
        (width & 1U) || (height & 1U) || y_stride < width || c_stride < width) {
        return Z8_FS_E_FORMAT;
    }
    if (!z8_filmsim_cfg_validate(cfg)) {
        return Z8_FS_E_ARGUMENT;
    }
    if (cfg->live_link == Z8_LIVE_LINK_OFF || !z8_filmsim_cfg_needs_hdf(cfg)) {
        return Z8_FS_BYPASS;
    }

    if (!workspace || ((uintptr_t)workspace & (Z8_FILMSIM_WORKSPACE_ALIGN - 1U)) != 0U ||
        workspace_bytes < Z8_FILMSIM_WORKSPACE_BYTES) {
        return Z8_FS_E_WORKSPACE;
    }

    if (cancelled && cancelled(cancel_ctx)) {
        return Z8_FS_CANCELLED;
    }

    z8_fs_stage_plane pl;
    fs_zero(&pl, sizeof(pl));
    pl.y = y;
    pl.c = c;
    pl.y_bytes = (size_t)y_stride * height;
    pl.c_bytes = (size_t)c_stride * (height >> 1);
    pl.width = width;
    pl.height = height;
    pl.y_stride = y_stride;
    pl.c_stride = c_stride;
    pl.format = Z8_FS_INT_FMT_NV12;
    pl.is_full_res = 0U;

    uint64_t shot_id = frame_id ? frame_id : 1U;
    int rc = z8_fs_stage_hdf_run(cfg->hdf_level, shot_id, 1U, 0U, &pl,
                                 workspace, workspace_bytes,
                                 cancelled, cancel_ctx);
    if (rc == Z8_FS_INT_OK) {
        if (stages_applied_out) {
            *stages_applied_out = Z8_FS_STAGE_HDF;
        }
        return Z8_FS_OK;
    }
    if (rc == Z8_FS_INT_BYPASS) {
        return Z8_FS_BYPASS;
    }
    return rc;
}
