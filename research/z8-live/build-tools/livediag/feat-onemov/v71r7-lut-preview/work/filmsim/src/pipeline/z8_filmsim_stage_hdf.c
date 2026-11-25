/* SPDX-License-Identifier: MIT
 * Stage 1 Adapter: Ricoh Optical HDF (z8_hdf).
 * Isolated translation unit to keep z8_hdf.h enum identifiers disjoint from z8_filmsim_abi.h.
 * Freestanding C11, scalar-only (-mgeneral-regs-only), zero malloc, zero .data/.bss.
 */
#include "z8_filmsim_stages.h"
#include "z8_hdf.h"

static int map_hdf_rc(int rc)
{
    switch (rc) {
    case Z8_HDF_OK:              return Z8_FS_INT_OK;
    case Z8_HDF_DISABLED:        return Z8_FS_INT_BYPASS;
    case Z8_HDF_CANCELLED:       return Z8_FS_INT_CANCELLED;
    case Z8_HDF_CANCELLED_DIRTY: return Z8_FS_INT_CANCELLED_DIRTY;
    case Z8_HDF_HUNG_SMP:        return Z8_FS_INT_HUNG_SMP;
    case Z8_HDF_E_FORMAT:        return Z8_FS_INT_E_FORMAT;
    case Z8_HDF_E_CAPACITY:      return Z8_FS_INT_E_CAPACITY;
    case Z8_HDF_E_ALIAS:         return Z8_FS_INT_E_ALIAS;
    case Z8_HDF_E_WORKSPACE:     return Z8_FS_INT_E_WORKSPACE;
    case Z8_HDF_E_ARGUMENT:
    case Z8_HDF_E_SHOT:          return Z8_FS_INT_E_ARGUMENT;
    default:                     return Z8_FS_INT_E_INTERNAL;
    }
}

int z8_fs_stage_hdf_run(uint32_t hdf_level,
                        uint64_t shot_id,
                        uint32_t count,
                        uint32_t analysis_index,
                        const z8_fs_stage_plane *planes,
                        void *workspace, size_t workspace_bytes,
                        int (*cancelled)(void *), void *cancel_ctx)
{
    if (hdf_level == Z8_HDF_OFF) {
        return Z8_FS_INT_BYPASS;
    }
    if (!planes || hdf_level > Z8_HDF_STRONG || count == 0U ||
        count > Z8_HDF_MAX_PLANES || analysis_index >= count || shot_id == 0U) {
        return Z8_FS_INT_E_ARGUMENT;
    }
    if (!workspace || workspace_bytes < Z8_HDF_WORKSPACE_BYTES) {
        return Z8_FS_INT_E_WORKSPACE;
    }

    if (count == 1U && analysis_index == 0U && !planes[0].is_full_res &&
        planes[0].format == Z8_FS_INT_FMT_NV12 &&
        planes[0].width == 1024U && (planes[0].height == 682U || planes[0].height == 680U) &&
        planes[0].y_stride >= 1024U && planes[0].c_stride >= 1024U &&
        planes[0].y_bytes >= (size_t)planes[0].y_stride * planes[0].height &&
        planes[0].c_bytes >= (size_t)planes[0].c_stride * (planes[0].height >> 1)) {
        int rc = z8_hdf_live_nv12_1024(planes[0].y, planes[0].y_stride,
                                       planes[0].c, planes[0].c_stride,
                                       planes[0].height, hdf_level, 0U,
                                       cancelled, cancel_ctx,
                                       workspace, workspace_bytes);
        return map_hdf_rc(rc);
    }

    if (count == 1U && analysis_index == 0U && !planes[0].is_full_res &&
        planes[0].format == Z8_FS_INT_FMT_NV12 &&
        planes[0].width == 1280U &&
        planes[0].height >= 850U && planes[0].height <= 856U && (planes[0].height & 1U) == 0U &&
        planes[0].y_stride >= 1280U && planes[0].c_stride >= 1280U &&
        planes[0].y_bytes >= (size_t)planes[0].y_stride * planes[0].height &&
        planes[0].c_bytes >= (size_t)planes[0].c_stride * (planes[0].height >> 1)) {
        int rc = z8_hdf_live_nv12_1280(planes[0].y, planes[0].y_stride,
                                       planes[0].c, planes[0].c_stride,
                                       planes[0].height, hdf_level, 0U,
                                       cancelled, cancel_ctx,
                                       workspace, workspace_bytes);
        return map_hdf_rc(rc);
    }

    z8_hdf_job job;
    job.abi = Z8_HDF_ABI;
    job.level = hdf_level;
    job.count = count;
    job.analysis_index = analysis_index;
    job.shot_id = shot_id;
    job.monochrome = 0U;
    job.exposure_known = 0U;
    job.scene_ev256 = 0;
    job.white[0] = 255U;
    job.white[1] = 255U;
    job.white[2] = 255U;
    job.cancelled = cancelled;
    job.cancel_context = cancel_ctx;

    for (uint32_t i = 0U; i < count; ++i) {
        const z8_fs_stage_plane *sp = &planes[i];
        z8_hdf_plane *dp = &job.planes[i];
        if (sp->format != Z8_FS_INT_FMT_NV16 && sp->format != Z8_FS_INT_FMT_NV12) {
            return Z8_FS_INT_E_FORMAT;
        }
        dp->y = sp->y;
        dp->c = sp->c;
        dp->y_bytes = sp->y_bytes;
        dp->c_bytes = sp->c_bytes;
        dp->width = sp->width;
        dp->height = sp->height;
        dp->y_stride = sp->y_stride;
        dp->c_stride = sp->c_stride;
        dp->format = (sp->format == Z8_FS_INT_FMT_NV12) ? Z8_HDF_YCBCR420 : Z8_HDF_YCBCR422;
        dp->shot_id = shot_id;
    }

    int rc = z8_hdf_apply(&job, workspace, workspace_bytes);
    return map_hdf_rc(rc);
}
