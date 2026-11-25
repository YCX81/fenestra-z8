/* SPDX-License-Identifier: MIT
 * Clean-room Optical HDF Multi-Scale Scatter Kernel (z8_hdf) for Nikon Z8.
 * Zero malloc, zero writable static data/bss, zero floating-point instructions,
 * <= 512 KiB caller-owned deterministic workspace.
 */
#include "z8_hdf.h"
#include "z8_hdf_kernels.h"

#define HDF_GRID_MAX_DIM   344U
#define HDF_GRID_MAX_STRIDE 352U
#define HDF_GRID_N0_CAP    68608U
#define HDF_GRID_N1_CAP    19200U
#define HDF_GRID_N2_CAP     6144U
#define HDF_VROW_PAD         40U

typedef struct {
    uintptr_t first, end;
} HdfRange;

typedef struct {
    uint16_t k_q12;
    uint8_t  w1, w2, w3;
    uint8_t  pad;
    uint16_t knee_q16;
    uint16_t boost_quad_q14;
    uint16_t boost_core_q14;
} HdfLevelPreset;

/* Monotonic 4-level optical HDF progression (~1/8, ~1/4, ~1/2 mist filters) */
static const HdfLevelPreset hdf_presets[4] = {
    {   0U,   0U,  0U,  0U, 0U, 22528U,     0U,     0U }, /* OFF */
    { 328U, 140U, 84U, 32U, 0U, 22528U, 26214U, 19661U }, /* WEAK   (k= 8.0%) */
    { 592U, 124U, 92U, 40U, 0U, 22528U, 32768U, 24576U }, /* MEDIUM (k=14.5%) */
    { 920U, 108U, 98U, 50U, 0U, 22528U, 37683U, 27853U }, /* STRONG (k=22.5%) */
};

typedef struct {
    uint32_t block_size;
    uint32_t gw, gh, gws;
    uint32_t gw1, gh1, gws1;
    uint32_t gw2, gh2, gws2;
    uint32_t source_index;
    uint32_t k_q12;
    uint32_t w1, w2, w3;
    uint32_t white_scale_q8[3];
    hdf_extract_params extract;

    /* Persistent 16-bit Q14 linear scatter grids (3 channels) */
    uint16_t _Alignas(16) grid_r[HDF_GRID_N0_CAP];
    uint16_t _Alignas(16) grid_g[HDF_GRID_N0_CAP];
    uint16_t _Alignas(16) grid_b[HDF_GRID_N0_CAP];

    /* Disjoint per-stage scratch union */
    union {
        struct {
            uint8_t  _Alignas(16) r_row[Z8_HDF_MAX_WIDTH];
            uint8_t  _Alignas(16) g_row[Z8_HDF_MAX_WIDTH];
            uint8_t  _Alignas(16) b_row[Z8_HDF_MAX_WIDTH];
            uint16_t _Alignas(16) lr_row[Z8_HDF_MAX_WIDTH];
            uint16_t _Alignas(16) lg_row[Z8_HDF_MAX_WIDTH];
            uint16_t _Alignas(16) lb_row[Z8_HDF_MAX_WIDTH];
            uint32_t _Alignas(16) acc_r[HDF_GRID_MAX_STRIDE];
            uint32_t _Alignas(16) acc_g[HDF_GRID_MAX_STRIDE];
            uint32_t _Alignas(16) acc_b[HDF_GRID_MAX_STRIDE];
        } s1;

        struct {
            uint16_t _Alignas(16) lvl1[HDF_GRID_N1_CAP];
            uint16_t _Alignas(16) lvl2[HDF_GRID_N2_CAP];
            uint16_t _Alignas(16) hblur_row[HDF_GRID_MAX_STRIDE + HDF_VROW_PAD];
            uint16_t _Alignas(16) vblur_col[HDF_GRID_MAX_STRIDE * 8U];
            uint16_t _Alignas(16) gx1[HDF_GRID_MAX_STRIDE];
            uint8_t  _Alignas(16) fx1[HDF_GRID_MAX_STRIDE];
            uint16_t _Alignas(16) gx2[HDF_GRID_MAX_STRIDE];
            uint8_t  _Alignas(16) fx2[HDF_GRID_MAX_STRIDE];
            uint16_t _Alignas(16) vrow1[HDF_GRID_MAX_STRIDE + HDF_VROW_PAD];
            uint16_t _Alignas(16) vrow2[HDF_GRID_MAX_STRIDE + HDF_VROW_PAD];
            uint16_t _Alignas(16) s2_row[HDF_GRID_MAX_STRIDE];
            uint16_t _Alignas(16) s3_row[HDF_GRID_MAX_STRIDE];
        } s2;

        struct {
            uint16_t _Alignas(16) gx_arr[Z8_HDF_MAX_WIDTH];
            uint8_t  _Alignas(16) fx_arr[Z8_HDF_MAX_WIDTH];
            uint16_t _Alignas(16) vrow_r[HDF_GRID_MAX_STRIDE + HDF_VROW_PAD];
            uint16_t _Alignas(16) vrow_g[HDF_GRID_MAX_STRIDE + HDF_VROW_PAD];
            uint16_t _Alignas(16) vrow_b[HDF_GRID_MAX_STRIDE + HDF_VROW_PAD];
            uint8_t  _Alignas(16) tile_scratch[HDF_STILL_TILE_SCRATCH_BYTES];
            int16_t  _Alignas(16) dcb_stash[Z8_HDF_MAX_WIDTH / 2U];
            int16_t  _Alignas(16) dcr_stash[Z8_HDF_MAX_WIDTH / 2U];
        } s3;
    } u;
} HdfWorkspace;

_Static_assert(sizeof(HdfWorkspace) <= Z8_HDF_WORKSPACE_BYTES,
               "HdfWorkspace must fit inside 512 KiB Z8_HDF_WORKSPACE_BYTES");

static int make_range(const void *p, size_t bytes, HdfRange *r) {
    uintptr_t a = (uintptr_t)p;
    if (!p || !bytes || bytes > UINTPTR_MAX - a) return 0;
    r->first = a;
    r->end = a + bytes;
    return 1;
}

static int overlaps(HdfRange a, HdfRange b) {
    return a.first < b.end && b.first < a.end;
}

static int plane_extent(uint32_t stride, uint32_t rows, uint32_t width, size_t *n) {
    if (!rows || stride < width || (size_t)(rows - 1U) > (SIZE_MAX - width) / stride) return 0;
    *n = (size_t)(rows - 1U) * stride + width;
    return 1;
}

static int is_cancelled(const z8_hdf_job *job) {
    return job->cancelled && job->cancelled(job->cancel_context);
}

static int validate_job(const z8_hdf_job *job, const z8_hdf_optics *optics,
                        void *work, size_t work_bytes, uint32_t *out_source_idx) {
    HdfRange images[Z8_HDF_MAX_PLANES * 2U];
    HdfRange w, jr, opt_r;
    uint32_t ranges = 0;

    if (!job || job->abi != Z8_HDF_ABI || job->level > Z8_HDF_STRONG || !job->count ||
        job->count > Z8_HDF_MAX_PLANES || job->analysis_index >= job->count ||
        job->monochrome > 1U || job->exposure_known > 1U ||
        !job->white[0] || !job->white[1] || !job->white[2]) {
        return Z8_HDF_E_ARGUMENT;
    }
    if (optics && optics->neutrality_q8 > 256U) {
        return Z8_HDF_E_ARGUMENT;
    }
    if (!job->shot_id) {
        return Z8_HDF_E_SHOT;
    }
    if (!work || ((uintptr_t)work & (Z8_HDF_WORKSPACE_ALIGNMENT - 1U)) ||
        work_bytes < Z8_HDF_WORKSPACE_BYTES || !make_range(work, work_bytes, &w)) {
        return Z8_HDF_E_WORKSPACE;
    }
    w.end = w.first + Z8_HDF_WORKSPACE_BYTES;
    if (!make_range(job, sizeof(*job), &jr) || overlaps(jr, w)) {
        return Z8_HDF_E_ALIAS;
    }
    if (optics) {
        if (!make_range(optics, sizeof(*optics), &opt_r) || overlaps(opt_r, w)) {
            return Z8_HDF_E_ALIAS;
        }
    }

    uint64_t max_area = 0;
    uint32_t max_idx = 0;
    for (uint32_t i = 0; i < job->count; ++i) {
        const z8_hdf_plane *p = &job->planes[i];
        size_t yn, cn;
        HdfRange yr, cr, declared;
        if (!p->width || p->width > Z8_HDF_MAX_WIDTH ||
            !p->height || p->height > Z8_HDF_MAX_HEIGHT ||
            (p->width & 3U) || (p->y_stride & 3U) || (p->c_stride & 3U) ||
            ((uintptr_t)p->y & 3U) || ((uintptr_t)p->c & 3U) ||
            p->format > Z8_HDF_YCBCR420) {
            return Z8_HDF_E_FORMAT;
        }
        if (p->shot_id != job->shot_id) {
            return Z8_HDF_E_SHOT;
        }
        uint32_t ch = p->format == Z8_HDF_YCBCR420 ? (p->height + 1U) / 2U : p->height;
        if (!plane_extent(p->y_stride, p->height, p->width, &yn) ||
            !plane_extent(p->c_stride, ch, p->width, &cn) ||
            p->y_bytes < yn || p->c_bytes < cn ||
            !make_range(p->y, p->y_bytes, &declared) ||
            !make_range(p->c, p->c_bytes, &declared) ||
            !make_range(p->y, yn, &yr) ||
            !make_range(p->c, cn, &cr)) {
            return Z8_HDF_E_CAPACITY;
        }
        if (overlaps(yr, cr) || overlaps(yr, w) || overlaps(cr, w) ||
            overlaps(yr, jr) || overlaps(cr, jr)) {
            return Z8_HDF_E_ALIAS;
        }
        if (optics && (overlaps(yr, opt_r) || overlaps(cr, opt_r))) {
            return Z8_HDF_E_ALIAS;
        }
        for (uint32_t k = 0; k < ranges; ++k) {
            if (overlaps(images[k], yr) || overlaps(images[k], cr)) {
                return Z8_HDF_E_ALIAS;
            }
        }
        images[ranges++] = yr;
        images[ranges++] = cr;

        uint64_t area = (uint64_t)p->width * p->height;
        if (area > max_area) {
            max_area = area;
            max_idx = i;
        }
    }

    if (job->count > 1U) {
        const z8_hdf_plane *ap = &job->planes[job->analysis_index];
        uint64_t ap_area = (uint64_t)ap->width * ap->height;
        int is_720p_analysis = (ap->width == 720U && ap->height == 480U);
        if (!is_720p_analysis && ap_area < max_area) {
            return Z8_HDF_E_FORMAT;
        }
    }

    *out_source_idx = max_idx;
    return Z8_HDF_OK;
}

static void build_upsample_map(uint32_t dst_len, uint32_t src_len,
                               uint16_t *gx_arr, uint8_t *fx_arr) {
    uint64_t den = 2ULL * dst_len;
    uint64_t right_lim = (uint64_t)(2U * src_len - 1U) * dst_len;
    for (uint32_t x = 0; x < dst_len; ++x) {
        uint64_t p = (uint64_t)(2U * x + 1U) * src_len;
        if (src_len <= 1U || p <= dst_len) {
            gx_arr[x] = 0U;
            fx_arr[x] = 0U;
        } else if (p >= right_lim) {
            gx_arr[x] = (uint16_t)(src_len - 1U);
            fx_arr[x] = 0U;
        } else {
            uint64_t num = p - dst_len;
            uint32_t g = (uint32_t)(num / den);
            uint64_t rem = num % den;
            uint32_t fx = (uint32_t)((rem * 256ULL + dst_len) / den);
            if (fx >= 256U) {
                g += 1U;
                fx = 0U;
            }
            if (g >= src_len) {
                g = src_len - 1U;
                fx = 0U;
            }
            gx_arr[x] = (uint16_t)g;
            fx_arr[x] = (uint8_t)fx;
        }
    }
}

static void vcoord_map(uint32_t y, uint32_t dst_h, uint32_t src_h,
                       uint32_t *gy0, uint32_t *gy1, uint32_t *fy_q8) {
    uint64_t p = (uint64_t)(2U * y + 1U) * src_h;
    uint64_t right_lim = (uint64_t)(2U * src_h - 1U) * dst_h;
    if (src_h <= 1U || p <= dst_h) {
        *gy0 = 0U;
        *gy1 = 0U;
        *fy_q8 = 0U;
    } else if (p >= right_lim) {
        *gy0 = src_h - 1U;
        *gy1 = src_h - 1U;
        *fy_q8 = 0U;
    } else {
        uint64_t den = 2ULL * dst_h;
        uint64_t num = p - dst_h;
        uint32_t g = (uint32_t)(num / den);
        uint64_t rem = num % den;
        uint32_t fy = (uint32_t)((rem * 256ULL + dst_h) / den);
        if (fy >= 256U) {
            g += 1U;
            fy = 0U;
        }
        if (g >= src_h - 1U) {
            *gy0 = src_h - 1U;
            *gy1 = src_h - 1U;
            *fy_q8 = 0U;
        } else {
            *gy0 = g;
            *gy1 = g + 1U;
            *fy_q8 = fy;
        }
    }
}

static int blur_grid_3pass(uint16_t *grid, uint16_t *h_tmp, uint16_t *v_tmp,
                           uint32_t w, uint32_t h, uint32_t stride, uint32_t radius,
                           const z8_hdf_job *job) {
    if (!radius || (w <= 1U && h <= 1U)) return Z8_HDF_OK;
    for (uint32_t pass = 0; pass < 3U; ++pass) {
        if (w > 1U) {
            hdf_blur_h_grid_u16(grid, h_tmp, w, h, stride, radius);
            if (is_cancelled(job)) return Z8_HDF_CANCELLED;
        }
        if (h > 1U) {
            hdf_blur_v_grid_u16(grid, v_tmp, w, h, stride, radius);
            if (is_cancelled(job)) return Z8_HDF_CANCELLED;
        }
    }
    return Z8_HDF_OK;
}

static int build_channel_pyramid(HdfWorkspace *ws, uint16_t *grid0,
                                 uint32_t r1, uint32_t r2, uint32_t r3,
                                 uint32_t white_scale_q8, const z8_hdf_job *job) {
    uint32_t gw = ws->gw, gh = ws->gh, gws = ws->gws;
    uint32_t gw1 = ws->gw1, gh1 = ws->gh1, gws1 = ws->gws1;
    uint32_t gw2 = ws->gw2, gh2 = ws->gh2, gws2 = ws->gws2;
    int rc;

    /* Capture dyadic levels before blurring Level 0 in-place */
    hdf_downsample_2x2_u16(grid0, gw, gh, gws, ws->u.s2.lvl1, gw1, gh1, gws1);
    hdf_downsample_2x2_u16(ws->u.s2.lvl1, gw1, gh1, gws1, ws->u.s2.lvl2, gw2, gh2, gws2);

    if (is_cancelled(job)) return Z8_HDF_CANCELLED;

    /* 3-pass separable cubic B-spline Gaussian at each spatial scale */
    rc = blur_grid_3pass(grid0, ws->u.s2.hblur_row, ws->u.s2.vblur_col, gw, gh, gws, r1, job);
    if (rc != Z8_HDF_OK) return rc;

    rc = blur_grid_3pass(ws->u.s2.lvl1, ws->u.s2.hblur_row, ws->u.s2.vblur_col, gw1, gh1, gws1, r2, job);
    if (rc != Z8_HDF_OK) return rc;

    rc = blur_grid_3pass(ws->u.s2.lvl2, ws->u.s2.hblur_row, ws->u.s2.vblur_col, gw2, gh2, gws2, r3, job);
    if (rc != Z8_HDF_OK) return rc;

    /* Upsample Scale 2 and Scale 3 onto Level 0, blend w1*S1 + w2*S2 + w3*S3,
     * and fold k_q12 into Q15 scatter sk = (mix * k_q12 + 1024) >> 11 for Stage 3.
     */
    build_upsample_map(gw, gw1, ws->u.s2.gx1, ws->u.s2.fx1);
    build_upsample_map(gw, gw2, ws->u.s2.gx2, ws->u.s2.fx2);

    for (uint32_t y = 0; y < gh; ++y) {
        if ((y & 15U) == 0U && is_cancelled(job)) return Z8_HDF_CANCELLED;
        uint32_t y1_0, y1_1, fy1, y2_0, y2_1, fy2;
        vcoord_map(y, gh, gh1, &y1_0, &y1_1, &fy1);
        vcoord_map(y, gh, gh2, &y2_0, &y2_1, &fy2);

        hdf_vlerp_row_u16(ws->u.s2.lvl1 + (size_t)y1_0 * gws1,
                          ws->u.s2.lvl1 + (size_t)y1_1 * gws1,
                          ws->u.s2.vrow1, gw1, fy1, HDF_VROW_PAD);
        hdf_vlerp_row_u16(ws->u.s2.lvl2 + (size_t)y2_0 * gws2,
                          ws->u.s2.lvl2 + (size_t)y2_1 * gws2,
                          ws->u.s2.vrow2, gw2, fy2, HDF_VROW_PAD);

        hdf_hlerp_row_u16(ws->u.s2.vrow1, ws->u.s2.gx1, ws->u.s2.fx1, ws->u.s2.s2_row, gw);
        hdf_hlerp_row_u16(ws->u.s2.vrow2, ws->u.s2.gx2, ws->u.s2.fx2, ws->u.s2.s3_row, gw);

        uint16_t *row0 = grid0 + (size_t)y * gws;
        hdf_blend_3scales_k_row_u16(row0, ws->u.s2.s2_row, ws->u.s2.s3_row,
                                    gw, ws->w1, ws->w2, ws->w3,
                                    white_scale_q8, ws->k_q12);
        for (uint32_t x = gw; x < gws; ++x) {
            row0[x] = row0[gw - 1U];
        }
    }
    return Z8_HDF_OK;
}

int z8_hdf_apply_ex(const z8_hdf_job *job, const z8_hdf_optics *optics,
                    void *workspace, size_t workspace_bytes) {
    if (!job) return Z8_HDF_E_ARGUMENT;
    if (job->level == Z8_HDF_OFF) return Z8_HDF_DISABLED;

    uint32_t source_idx = 0;
    int rc = validate_job(job, optics, workspace, workspace_bytes, &source_idx);
    if (rc != Z8_HDF_OK) return rc;
    if (is_cancelled(job)) return Z8_HDF_CANCELLED;

    HdfWorkspace *ws = (HdfWorkspace *)workspace;
    const z8_hdf_plane *src = &job->planes[source_idx];

    /* Choose smallest integer block size B >= 1 fitting inside HDF_GRID_N0_CAP */
    uint32_t b = 1U;
    uint32_t gw, gh, gws;
    for (;;) {
        gw = (src->width + b - 1U) / b;
        gh = (src->height + b - 1U) / b;
        gws = (gw + 7U) & ~7U;
        if (gw <= HDF_GRID_MAX_DIM && gh <= HDF_GRID_MAX_DIM &&
            (uint64_t)gws * gh <= HDF_GRID_N0_CAP) {
            break;
        }
        ++b;
    }

    ws->block_size = b;
    ws->gw = gw;
    ws->gh = gh;
    ws->gws = gws;
    ws->gw1 = (gw + 1U) / 2U;
    ws->gh1 = (gh + 1U) / 2U;
    ws->gws1 = (ws->gw1 + 7U) & ~7U;
    ws->gw2 = (ws->gw1 + 1U) / 2U;
    ws->gh2 = (ws->gh1 + 1U) / 2U;
    ws->gws2 = (ws->gw2 + 7U) & ~7U;
    ws->source_index = source_idx;

    const HdfLevelPreset *pr = &hdf_presets[job->level];
    ws->k_q12 = pr->k_q12;
    ws->w1 = pr->w1;
    ws->w2 = pr->w2;
    ws->w3 = pr->w3;

    for (uint32_t c = 0; c < 3U; ++c) {
        ws->white_scale_q8[c] = job->white[c] == 255U
                                    ? 256U
                                    : ((uint32_t)job->white[c] * 256U + 127U) / 255U;
    }

    uint32_t ev_gain_q8 = 256U;
    if (job->exposure_known) {
        int32_t dev = 2560 - job->scene_ev256;
        if (dev < -1536) dev = -1536;
        if (dev >  1536) dev =  1536;
        ev_gain_q8 = (uint32_t)(256 + ((dev * 32) >> 10));
    }

    ws->extract.knee_q16 = (optics && optics->knee_q16) ? optics->knee_q16 : pr->knee_q16;
    ws->extract.boost_quad_q14 = pr->boost_quad_q14;
    ws->extract.boost_core_q14 = pr->boost_core_q14;
    ws->extract.ev_gain_q8 = ev_gain_q8;
    ws->extract.neutrality_q8 = optics ? optics->neutrality_q8 : 96U;

    /* ---- Stage 1: Strided (4x4 / 2x2) or Full Area-Integrating Downsample */
    uint32_t s1_step  = (b >= 12U) ? 4U : ((b >= 6U) ? 2U : 1U);
    uint32_t s1_sh    = (s1_step == 4U) ? 2U : ((s1_step == 2U) ? 1U : 0U);
    uint32_t s1_mask  = s1_step - 1U;
    uint32_t s1_sub_w = src->width >> s1_sh;

    for (uint32_t gy = 0; gy < gh; ++gy) {
        uint32_t y_start = gy * b;
        uint32_t y_end = y_start + b;
        if (y_end > src->height) y_end = src->height;
        uint32_t bh = y_end - y_start;

        for (uint32_t gx = 0; gx < gws; ++gx) {
            ws->u.s1.acc_r[gx] = 0U;
            ws->u.s1.acc_g[gx] = 0U;
            ws->u.s1.acc_b[gx] = 0U;
        }

        uint32_t n_rows = 0U;
        if (s1_step > 1U) {
            uint32_t y0 = y_start + ((bh >= s1_step) ? (s1_step >> 1) : 0U);
            for (uint32_t y = y0; y < y_end; y += s1_step) {
                if (is_cancelled(job)) return Z8_HDF_CANCELLED;
                const uint8_t *y_row = src->y + (size_t)y * src->y_stride;
                uint32_t cy = src->format == Z8_HDF_YCBCR420 ? (y >> 1) : y;
                const uint8_t *c_row = src->c + (size_t)cy * src->c_stride;

                hdf_s1_subsample_ycc_to_rgb_row(y_row, c_row,
                                                ws->u.s1.r_row, ws->u.s1.g_row, ws->u.s1.b_row,
                                                src->width, s1_step, job->monochrome);
                hdf_srgb_to_lin_row(ws->u.s1.r_row, ws->u.s1.lr_row, s1_sub_w);
                hdf_srgb_to_lin_row(ws->u.s1.g_row, ws->u.s1.lg_row, s1_sub_w);
                hdf_srgb_to_lin_row(ws->u.s1.b_row, ws->u.s1.lb_row, s1_sub_w);

                hdf_extract_energy_row(ws->u.s1.lr_row, ws->u.s1.lg_row, ws->u.s1.lb_row,
                                       ws->u.s1.lr_row, ws->u.s1.lg_row, ws->u.s1.lb_row,
                                       s1_sub_w, &ws->extract);
                hdf_accum_blocks_row_sub(ws->u.s1.lr_row, ws->u.s1.lg_row, ws->u.s1.lb_row,
                                         ws->u.s1.acc_r, ws->u.s1.acc_g, ws->u.s1.acc_b,
                                         src->width, b, s1_step);
                ++n_rows;
            }
        } else {
            for (uint32_t y = y_start; y < y_end; ++y) {
                if (is_cancelled(job)) return Z8_HDF_CANCELLED;
                const uint8_t *y_row = src->y + (size_t)y * src->y_stride;
                uint32_t cy = src->format == Z8_HDF_YCBCR420 ? (y >> 1) : y;
                const uint8_t *c_row = src->c + (size_t)cy * src->c_stride;

                hdf_ycc_to_rgb_row(y_row, c_row,
                                   ws->u.s1.r_row, ws->u.s1.g_row, ws->u.s1.b_row,
                                   src->width, job->monochrome);
                hdf_srgb_to_lin_row(ws->u.s1.r_row, ws->u.s1.lr_row, src->width);
                hdf_srgb_to_lin_row(ws->u.s1.g_row, ws->u.s1.lg_row, src->width);
                hdf_srgb_to_lin_row(ws->u.s1.b_row, ws->u.s1.lb_row, src->width);

                hdf_extract_energy_row(ws->u.s1.lr_row, ws->u.s1.lg_row, ws->u.s1.lb_row,
                                       ws->u.s1.lr_row, ws->u.s1.lg_row, ws->u.s1.lb_row,
                                       src->width, &ws->extract);
                hdf_accum_blocks_row(ws->u.s1.lr_row, ws->u.s1.lg_row, ws->u.s1.lb_row,
                                     ws->u.s1.acc_r, ws->u.s1.acc_g, ws->u.s1.acc_b,
                                     src->width, b);
            }
            n_rows = bh;
        }

        uint16_t *gr = ws->grid_r + (size_t)gy * gws;
        uint16_t *gg = ws->grid_g + (size_t)gy * gws;
        uint16_t *gb = ws->grid_b + (size_t)gy * gws;
        for (uint32_t gx = 0; gx < gw; ++gx) {
            uint32_t area;
            if (s1_step > 1U) {
                uint32_t x0 = gx * b;
                uint32_t x1 = x0 + b;
                if (x1 > src->width) x1 = src->width;
                uint32_t k0 = (x0 + s1_mask) >> s1_sh;
                uint32_t k1 = (x1 + s1_mask) >> s1_sh;
                uint32_t cnt_x = (k1 > k0) ? (k1 - k0) : 1U;
                area = cnt_x * n_rows;
            } else {
                uint32_t bw = src->width - gx * b;
                if (bw > b) bw = b;
                area = bw * bh;
            }
            if (area == 1U) {
                gr[gx] = (uint16_t)ws->u.s1.acc_r[gx];
                gg[gx] = (uint16_t)ws->u.s1.acc_g[gx];
                gb[gx] = (uint16_t)ws->u.s1.acc_b[gx];
            } else {
                uint32_t half = area >> 1;
                uint32_t mul = (uint32_t)((1ULL << 32) / area + 1ULL);
                gr[gx] = (uint16_t)(((uint64_t)(ws->u.s1.acc_r[gx] + half) * mul) >> 32);
                gg[gx] = (uint16_t)(((uint64_t)(ws->u.s1.acc_g[gx] + half) * mul) >> 32);
                gb[gx] = (uint16_t)(((uint64_t)(ws->u.s1.acc_b[gx] + half) * mul) >> 32);
            }
        }
        for (uint32_t gx = gw; gx < gws; ++gx) {
            gr[gx] = gr[gw - 1U];
            gg[gx] = gg[gw - 1U];
            gb[gx] = gb[gw - 1U];
        }
    }

    /* ---- Stage 2: 3-Scale Separable Optical Pyramid --------------------- */
    uint32_t max_g = gw > gh ? gw : gh;
    uint32_t r1 = 1U;
    uint32_t r2 = (max_g * 3U + 150U) / 300U;
    if (r2 < 1U) r2 = 1U;
    uint32_t r3 = (max_g * 5U + 150U) / 300U;
    if (r3 < 2U) r3 = 2U;

    rc = build_channel_pyramid(ws, ws->grid_r, r1, r2, r3, ws->white_scale_q8[0], job);
    if (rc != Z8_HDF_OK) return rc;

    if (!job->monochrome) {
        rc = build_channel_pyramid(ws, ws->grid_g, r1, r2, r3, ws->white_scale_q8[1], job);
        if (rc != Z8_HDF_OK) return rc;
        rc = build_channel_pyramid(ws, ws->grid_b, r1, r2, r3, ws->white_scale_q8[2], job);
        if (rc != Z8_HDF_OK) return rc;
    }

    if (is_cancelled(job)) return Z8_HDF_CANCELLED;

    /* ---- Stage 3: Fused L1-Tiled Bilinear Upsample & Shoulder Composite - */
    int dirty = 0;
    for (uint32_t pi = 0; pi < job->count; ++pi) {
        const z8_hdf_plane *p = &job->planes[pi];
        build_upsample_map(p->width, gw, ws->u.s3.gx_arr, ws->u.s3.fx_arr);

        for (uint32_t y = 0; y < p->height; ++y) {
            if (dirty && is_cancelled(job)) {
                return Z8_HDF_CANCELLED_DIRTY;
            }
            uint32_t gy0, gy1, fy_q8;
            vcoord_map(y, p->height, gh, &gy0, &gy1, &fy_q8);

            hdf_vlerp_row_u16(ws->grid_r + (size_t)gy0 * gws,
                              ws->grid_r + (size_t)gy1 * gws,
                              ws->u.s3.vrow_r, gw, fy_q8, HDF_VROW_PAD);
            if (!job->monochrome) {
                hdf_vlerp_row_u16(ws->grid_g + (size_t)gy0 * gws,
                                  ws->grid_g + (size_t)gy1 * gws,
                                  ws->u.s3.vrow_g, gw, fy_q8, HDF_VROW_PAD);
                hdf_vlerp_row_u16(ws->grid_b + (size_t)gy0 * gws,
                                  ws->grid_b + (size_t)gy1 * gws,
                                  ws->u.s3.vrow_b, gw, fy_q8, HDF_VROW_PAD);
            }

            uint8_t *y_row = p->y + (size_t)y * p->y_stride;
            uint32_t cy = p->format == Z8_HDF_YCBCR420 ? (y >> 1) : y;
            uint8_t *c_row = p->c + (size_t)cy * p->c_stride;

            uint32_t cmode = HDF_CHROMA_422_DIRECT;
            if (p->format == Z8_HDF_YCBCR420) {
                if ((y & 1U) == 0U) {
                    cmode = (y + 1U < p->height) ? HDF_CHROMA_420_EVEN : HDF_CHROMA_422_DIRECT;
                } else {
                    cmode = HDF_CHROMA_420_ODD;
                }
            }

            hdf_still_composite_row_fast(y_row, c_row,
                                         ws->u.s3.vrow_r, ws->u.s3.vrow_g, ws->u.s3.vrow_b,
                                         ws->u.s3.gx_arr, ws->u.s3.fx_arr,
                                         ws->u.s3.dcb_stash, ws->u.s3.dcr_stash,
                                         ws->u.s3.tile_scratch, p->width,
                                         job->level, job->monochrome, cmode);
            dirty = 1;
        }
    }

    return Z8_HDF_OK;
}

int z8_hdf_apply(const z8_hdf_job *job, void *workspace, size_t workspace_bytes) {
    return z8_hdf_apply_ex(job, NULL, workspace, workspace_bytes);
}

/* ---- V71r2 Live Fast Optical HDF (LIVE_256_YC: 1024x{680,682} NV12) ----- */

#define HDF_LIVE_W      1024U
#define HDF_LIVE_CW      512U
#define HDF_EVF_W       1280U
#define HDF_EVF_CW       640U
#define HDF_LIVE_GW      256U
#define HDF_LIVE_G0_CAP (256U * 171U) /* 43,776 */
#define HDF_LIVE_G1_CAP (128U *  86U) /* 11,008 */
#define HDF_LIVE_G2_CAP ( 64U *  43U) /*  2,752 */

typedef struct {
    /* Stored in Q13 (0..32767) during Stage 1-2; Stage 2 blend folds k_q12 so
     * Stage 3 reads k*4*S directly in Q15 (0..29440) without per-pixel k multiply.
     * grid_y doubles as the Stage 2 lvl1/lvl2 ping-pong blur scratch before build_grid_y.
     * r256/g256/b256 + edge masks capture the exact 256x{170,171} averaged sRGB
     * during Stage 1 (zero extra memory beyond the existing 512 KiB workspace) so
     * Stage 3B computes chroma deltas directly on the 256x171 grid without re-reading
     * Y/C or running 3-channel RGB vlerp, falling back to full chroma res only on edge chunks.
     */
    uint16_t _Alignas(16) grid_r[HDF_LIVE_G0_CAP];
    uint16_t _Alignas(16) grid_g[HDF_LIVE_G0_CAP];
    uint16_t _Alignas(16) grid_b[HDF_LIVE_G0_CAP];
    uint16_t _Alignas(16) grid_y[HDF_LIVE_G0_CAP];
    uint8_t  _Alignas(16) r256[HDF_LIVE_G0_CAP];
    uint8_t  _Alignas(16) g256[HDF_LIVE_G0_CAP];
    uint8_t  _Alignas(16) b256[HDF_LIVE_G0_CAP];
    uint32_t _Alignas(16) c_edge_mask[172];
    uint32_t _Alignas(16) y_edge_mask[172];
    uint32_t _Alignas(16) evf_cancel_flag;

    union {
        struct {
            uint8_t  _Alignas(16) boost_lo[64];
            uint8_t  _Alignas(16) boost_hi[64];
            uint8_t  _Alignas(16) boost_scratch[3072];
            uint8_t  _Alignas(16) evf_y640[3U * HDF_EVF_CW];
            uint8_t  _Alignas(16) evf_cb640[3U * HDF_EVF_CW];
            uint8_t  _Alignas(16) evf_cr640[3U * HDF_EVF_CW];
        } s1;
        struct {
            uint8_t  _Alignas(16) boost_lo[64];
            uint8_t  _Alignas(16) boost_hi[64];
            struct {
                uint8_t _Alignas(16) boost_scratch[3072];
                uint8_t _Alignas(16) evf_y640[3U * HDF_EVF_CW];
                uint8_t _Alignas(16) evf_cb640[3U * HDF_EVF_CW];
                uint8_t _Alignas(16) evf_cr640[3U * HDF_EVF_CW];
            } w[3];
        } s1_evf;
        struct {
            uint16_t _Alignas(16) lvl1[HDF_LIVE_G1_CAP];
            uint16_t _Alignas(16) lvl2[HDF_LIVE_G2_CAP];
            int16_t  _Alignas(16) htmp[272];
            int16_t  _Alignas(16) vrow1[144];
            int16_t  _Alignas(16) vrow2[80];
        } s2;
        struct {
            uint16_t _Alignas(16) vrow_y_640[HDF_EVF_CW];
            int16_t  _Alignas(16) vrow_y_pad[272];
            uint8_t  _Alignas(16) r_out[HDF_LIVE_GW];
            uint8_t  _Alignas(16) g_out[HDF_LIVE_GW];
            uint8_t  _Alignas(16) b_out[HDF_LIVE_GW];
            uint8_t  _Alignas(16) fb_buf[6144];
            uint8_t  _Alignas(16) exact_uv[2048];
        } s3;
        struct {
            struct {
                uint16_t _Alignas(16) vrow_y_640[HDF_EVF_CW];
                int16_t  _Alignas(16) vrow_y_pad[272];
                uint8_t  _Alignas(16) r_out[HDF_LIVE_GW];
                uint8_t  _Alignas(16) g_out[HDF_LIVE_GW];
                uint8_t  _Alignas(16) b_out[HDF_LIVE_GW];
                uint8_t  _Alignas(16) fb_buf[6144];
            } w[3];
        } s3_evf;
    } u;
} LiveHdfWs;

_Static_assert(sizeof(LiveHdfWs) <= Z8_HDF_WORKSPACE_BYTES,
               "LiveHdfWs must fit inside Z8_HDF_WORKSPACE_BYTES");

static inline int live_is_cancelled(int (*cancelled)(void *), void *cancel_ctx) {
    return cancelled && cancelled(cancel_ctx);
}

typedef struct {
    uint32_t dst_h, src_h, den, right_lim, p, g, rem, inv_den;
} LiveVCoordStep;

static inline void live_vcoord_init(LiveVCoordStep *st, uint32_t dst_h, uint32_t src_h) {
    st->dst_h = dst_h;
    st->src_h = src_h;
    st->den = 2U * dst_h;
    st->right_lim = (2U * src_h - 1U) * dst_h;
    st->p = src_h;
    st->g = 0U;
    st->rem = 0U;
    /* Exact reciprocal for x / den over x <= 200000, den <= 1024 */
    st->inv_den = (uint32_t)((1ULL << 32) / st->den) + 1U;
}

static inline void live_vcoord_next(LiveVCoordStep *st,
                                    uint32_t *gy0, uint32_t *gy1, uint32_t *fy_q8) {
    uint32_t p = st->p;
    st->p = p + 2U * st->src_h;
    if (p <= st->dst_h) {
        *gy0 = 0U;
        *gy1 = 0U;
        *fy_q8 = 0U;
        return;
    }
    if (p >= st->right_lim) {
        *gy0 = st->src_h - 1U;
        *gy1 = st->src_h - 1U;
        *fy_q8 = 0U;
        return;
    }
    uint32_t num = p - st->dst_h;
    uint32_t g = st->g;
    uint32_t base = g * st->den;
    uint32_t rem = num - base;
    if (rem >= st->den) {
        rem -= st->den;
        ++g;
    }
    st->g = g;
    uint32_t fy = (uint32_t)(((uint64_t)(rem * 256U + st->dst_h) * st->inv_den) >> 32);
    if (fy >= 256U) {
        g += 1U;
        fy = 0U;
    }
    if (g >= st->src_h - 1U) {
        *gy0 = st->src_h - 1U;
        *gy1 = st->src_h - 1U;
        *fy_q8 = 0U;
    } else {
        *gy0 = g;
        *gy1 = g + 1U;
        *fy_q8 = fy;
    }
}

static int live_build_channel_pyramid(LiveHdfWs *ws, uint16_t *grid0, uint32_t gh,
                                      const HdfLevelPreset *pr,
                                      int (*cancelled)(void *), void *cancel_ctx) {
    const uint32_t gw = HDF_LIVE_GW;
    const uint32_t gw1 = 128U;
    const uint32_t gh1 = (gh + 1U) >> 1;
    const uint32_t gw2 = 64U;
    const uint32_t gh2 = (gh1 + 1U) >> 1;

    hdf_live_ds2x2_u16(grid0, gw, gh, ws->u.s2.lvl1, gw1, gh1);
    hdf_live_ds2x2_u16(ws->u.s2.lvl1, gw1, gh1, ws->u.s2.lvl2, gw2, gh2);
    if (live_is_cancelled(cancelled, cancel_ctx)) return Z8_HDF_CANCELLED;

    /* Scale 1 (256x171): fused [1,2,2,2,1]/8 + [1,2,1]/4 -> exact var=2.0 (matches 3x r=1 box) */
    hdf_live_blur_sym53_2d(grid0, (uint16_t *)ws->u.s2.htmp, gw, gh);
    if (live_is_cancelled(cancelled, cancel_ctx)) return Z8_HDF_CANCELLED;

    /* Scale 2 (128x86): 2-pass pre-scaled 16-bit box r=3,4 (var=10.67);
     * Scale 3 (64x43):  2-pass pre-scaled 16-bit box r=5,5 (var=20.00, exact match to 3x r=4).
     * ws->grid_y is completely unused during Stage 2 pyramid construction.
     */
    hdf_live_blur_box16_1pass(ws->u.s2.lvl1, ws->grid_y, ws->u.s2.htmp, gw1, gh1, 3U);
    hdf_live_blur_box16_1pass(ws->u.s2.lvl1, ws->grid_y, ws->u.s2.htmp, gw1, gh1, 4U);
    hdf_live_blur_box16_1pass(ws->u.s2.lvl2, ws->grid_y, ws->u.s2.htmp, gw2, gh2, 5U);
    hdf_live_blur_box16_1pass(ws->u.s2.lvl2, ws->grid_y, ws->u.s2.htmp, gw2, gh2, 5U);
    if (live_is_cancelled(cancelled, cancel_ctx)) return Z8_HDF_CANCELLED;

    /* Fold k_q12 into the 3-scale blend weights: w_eff = round(w * k_q12 / 8) in Q15 */
    int16_t w1k = (int16_t)(((uint32_t)pr->w1 * (uint32_t)pr->k_q12 + 4U) >> 3);
    int16_t w2k = (int16_t)(((uint32_t)pr->w2 * (uint32_t)pr->k_q12 + 4U) >> 3);
    int16_t w3k = (int16_t)(((uint32_t)pr->w3 * (uint32_t)pr->k_q12 + 4U) >> 3);

    LiveVCoordStep vc1, vc2;
    live_vcoord_init(&vc1, gh, gh1);
    live_vcoord_init(&vc2, gh, gh2);
    for (uint32_t y = 0; y < gh; ++y) {
        uint32_t y1_0, y1_1, fy1, y2_0, y2_1, fy2;
        live_vcoord_next(&vc1, &y1_0, &y1_1, &fy1);
        live_vcoord_next(&vc2, &y2_0, &y2_1, &fy2);
        hdf_live_vlerp_q13_padded(ws->u.s2.lvl1 + (size_t)y1_0 * gw1,
                                  ws->u.s2.lvl1 + (size_t)y1_1 * gw1,
                                  ws->u.s2.vrow1, gw1, fy1);
        hdf_live_vlerp_q13_padded(ws->u.s2.lvl2 + (size_t)y2_0 * gw2,
                                  ws->u.s2.lvl2 + (size_t)y2_1 * gw2,
                                  ws->u.s2.vrow2, gw2, fy2);
        hdf_live_s2_blend_k_row_256(grid0 + (size_t)y * gw,
                                    ws->u.s2.vrow1, ws->u.s2.vrow2,
                                    w1k, w2k, w3k);
    }
    return Z8_HDF_OK;
}

static void live_init_boost_lut(LiveHdfWs *ws, const HdfLevelPreset *pr) {
    /* Precompute 64-entry Q13 highlight boost LUT (0..24575) */
    for (uint32_t k = 0; k < 64U; ++k) {
        uint32_t ldrv = k * 1024U + 512U;
        if (ldrv > 65535U) ldrv = 65535U;
        uint32_t over = ldrv > pr->knee_q16 ? ldrv - pr->knee_q16 : 0U;
        uint32_t t = (over * 1490U) >> 16;
        uint32_t quad = (t * t) >> 10;
        uint32_t cover = ldrv > HDF_CORE_KNEE_Q16 ? ldrv - HDF_CORE_KNEE_Q16 : 0U;
        uint32_t tc = (cover * 5U) >> 6;
        uint32_t core = (tc * tc) >> 10;
        uint32_t raw = quad * pr->boost_quad_q14 + core * pr->boost_core_q14;
        uint32_t boost = ((raw >> 10) * 256U + 128U) >> 8;
        if (boost > 49151U) boost = 49151U;
        uint32_t bh = boost >> 1;
        ws->u.s1.boost_lo[k] = (uint8_t)(bh & 0xFFU);
        ws->u.s1.boost_hi[k] = (uint8_t)(bh >> 8);
    }
}

int z8_hdf_live_nv12_1024(uint8_t *y, uint32_t y_stride,
                          uint8_t *c, uint32_t c_stride,
                          uint32_t height, uint32_t level,
                          uint32_t monochrome,
                          int (*cancelled)(void *), void *cancel_ctx,
                          void *workspace, size_t workspace_bytes) {
    if (level == Z8_HDF_OFF) {
        return Z8_HDF_DISABLED;
    }
    if (level > Z8_HDF_STRONG || monochrome > 1U || !y || !c) {
        return Z8_HDF_E_ARGUMENT;
    }
    if ((height != 680U && height != 682U) ||
        y_stride < HDF_LIVE_W || c_stride < HDF_LIVE_W) {
        return Z8_HDF_E_FORMAT;
    }
    if (!workspace || ((uintptr_t)workspace & (Z8_HDF_WORKSPACE_ALIGNMENT - 1U)) ||
        workspace_bytes < Z8_HDF_WORKSPACE_BYTES) {
        return Z8_HDF_E_WORKSPACE;
    }

    const uint32_t ch = height >> 1;
    size_t yn, cn;
    HdfRange yr, cr, wr;
    if (!plane_extent(y_stride, height, HDF_LIVE_W, &yn) ||
        !plane_extent(c_stride, ch, HDF_LIVE_W, &cn) ||
        !make_range(y, yn, &yr) || !make_range(c, cn, &cr)) {
        return Z8_HDF_E_CAPACITY;
    }
    if (!make_range(workspace, Z8_HDF_WORKSPACE_BYTES, &wr)) {
        return Z8_HDF_E_WORKSPACE;
    }
    if (overlaps(yr, cr) || overlaps(yr, wr) || overlaps(cr, wr)) {
        return Z8_HDF_E_ALIAS;
    }
    if (live_is_cancelled(cancelled, cancel_ctx)) {
        return Z8_HDF_CANCELLED;
    }

    LiveHdfWs *ws = (LiveHdfWs *)workspace;
    const HdfLevelPreset *pr = &hdf_presets[level];
    const uint32_t gw = HDF_LIVE_GW;
    const uint32_t gh = (ch + 1U) >> 1;

    live_init_boost_lut(ws, pr);

    /* ---- Stage 1: Fused Half-Res Q15 Extract -> 256x{170,171} Q13 Grid -- */
    for (uint32_t gy = 0; gy < gh; ++gy) {
        if ((gy & 3U) == 0U && live_is_cancelled(cancelled, cancel_ctx)) {
            return Z8_HDF_CANCELLED;
        }
        uint32_t cy0 = gy * 2U;
        const uint8_t *y0 = y + (size_t)(2U * cy0) * y_stride;
        const uint8_t *y1 = y + (size_t)(2U * cy0 + 1U) * y_stride;
        const uint8_t *c0 = c + (size_t)cy0 * c_stride;
        const uint8_t *y2 = NULL, *y3 = NULL, *c1 = NULL;
        if (cy0 + 1U < ch) {
            uint32_t cy1 = cy0 + 1U;
            y2 = y + (size_t)(2U * cy1) * y_stride;
            y3 = y + (size_t)(2U * cy1 + 1U) * y_stride;
            c1 = c + (size_t)cy1 * c_stride;
        }
        size_t goff = (size_t)gy * gw;
        hdf_live_s1_rowpair_256(y0, y1, c0, y2, y3, c1,
                                ws->u.s1.boost_lo, ws->u.s1.boost_hi,
                                ws->grid_r + goff,
                                ws->grid_g + goff,
                                ws->grid_b + goff,
                                ws->r256 + goff,
                                ws->g256 + goff,
                                ws->b256 + goff,
                                &ws->c_edge_mask[gy],
                                &ws->y_edge_mask[gy],
                                ws->u.s1.boost_scratch,
                                monochrome);
    }

    /* ---- Stage 2: 3-Scale Q13 Sigma-Matched Optical Pyramid + k Fold ---- */
    int rc = live_build_channel_pyramid(ws, ws->grid_r, gh, pr, cancelled, cancel_ctx);
    if (rc != Z8_HDF_OK) return rc;

    if (!monochrome) {
        rc = live_build_channel_pyramid(ws, ws->grid_g, gh, pr, cancelled, cancel_ctx);
        if (rc != Z8_HDF_OK) return rc;
        rc = live_build_channel_pyramid(ws, ws->grid_b, gh, pr, cancelled, cancel_ctx);
        if (rc != Z8_HDF_OK) return rc;
        if (live_is_cancelled(cancelled, cancel_ctx)) return Z8_HDF_CANCELLED;
        hdf_live_build_grid_y(ws->grid_r, ws->grid_g, ws->grid_b, ws->grid_y, gw * gh);
    }

    if (live_is_cancelled(cancelled, cancel_ctx)) {
        return Z8_HDF_CANCELLED;
    }

    /* ---- Stage 3: 256x{170,171} Direct Chroma Delta + Full-Res Y -------- */
    const uint16_t *gy_grid = monochrome ? ws->grid_r : ws->grid_y;
    LiveVCoordStep vc_y;
    live_vcoord_init(&vc_y, ch, gh);
    for (uint32_t gy = 0; gy < gh; ++gy) {
        if (gy > 0U && (gy & 3U) == 0U && live_is_cancelled(cancelled, cancel_ctx)) {
            return Z8_HDF_CANCELLED_DIRTY;
        }
        uint32_t cy0 = gy * 2U;
        uint8_t *y0 = y + (size_t)(2U * cy0) * y_stride;
        uint8_t *y1 = y + (size_t)(2U * cy0 + 1U) * y_stride;
        uint8_t *c0 = c + (size_t)cy0 * c_stride;
        uint8_t *y2 = NULL, *y3 = NULL, *c1 = NULL;
        if (cy0 + 1U < ch) {
            uint32_t cy1 = cy0 + 1U;
            y2 = y + (size_t)(2U * cy1) * y_stride;
            y3 = y + (size_t)(2U * cy1 + 1U) * y_stride;
            c1 = c + (size_t)cy1 * c_stride;
        }

        if (!monochrome) {
            size_t goff = (size_t)gy * gw;
            const uint16_t *gr_row = ws->grid_r + goff;
            const uint16_t *gg_row = ws->grid_g + goff;
            const uint16_t *gb_row = ws->grid_b + goff;
            uint32_t emask = ws->c_edge_mask[gy];
            uint32_t ychk  = ws->y_edge_mask[gy] & ~emask;
            if (ychk != 0U) {
                emask |= hdf_live_s3_check_schroma_mask(gr_row, gg_row, gb_row, ychk);
            }
            if (emask == 0xFFFFFFFFU) {
                hdf_live_s3_chroma_exact_masked(y0, y1, c0, y2, y3, c1,
                                                gr_row, gg_row, gb_row,
                                                emask, level, ws->u.s3.fb_buf, NULL);
            } else {
                if (emask != 0U) {
                    hdf_live_s3_chroma_exact_masked(y0, y1, c0, y2, y3, c1,
                                                    gr_row, gg_row, gb_row,
                                                    emask, level, ws->u.s3.fb_buf,
                                                    ws->u.s3.exact_uv);
                }
                const uint8_t *rin = ws->r256 + goff;
                const uint8_t *gin = ws->g256 + goff;
                const uint8_t *bin = ws->b256 + goff;
                hdf_live_s3_comp_chroma_256(rin, gin, bin,
                                            ws->u.s3.r_out, ws->u.s3.g_out, ws->u.s3.b_out,
                                            gr_row, gg_row, gb_row, level);
                hdf_live_s3_apply_chroma_256_2x2(c0, c1,
                                                 rin, gin, bin,
                                                 ws->u.s3.r_out, ws->u.s3.g_out, ws->u.s3.b_out);
                if (emask != 0U) {
                    hdf_live_s3_patch_exact_uv(c0, c1, ws->u.s3.exact_uv, emask);
                }
            }
        }

        uint32_t gy0, gy1, fy;
        live_vcoord_next(&vc_y, &gy0, &gy1, &fy);
        hdf_live_vlerp_hlerp_y_512(gy_grid + (size_t)gy0 * gw,
                                   gy_grid + (size_t)gy1 * gw,
                                   ws->u.s3.vrow_y_640, fy);
        hdf_live_s3_comp_y_rowpair(y0, y1, ws->u.s3.vrow_y_640, level);

        if (y2) {
            live_vcoord_next(&vc_y, &gy0, &gy1, &fy);
            hdf_live_vlerp_hlerp_y_512(gy_grid + (size_t)gy0 * gw,
                                       gy_grid + (size_t)gy1 * gw,
                                       ws->u.s3.vrow_y_640, fy);
            hdf_live_s3_comp_y_rowpair(y2, y3, ws->u.s3.vrow_y_640, level);
        }
    }

    return Z8_HDF_OK;
}

/* ---- V71.7-r4/r5 EVF Fast Optical HDF (1280x{850,852,854,856} NV12) ----- */

typedef void (*Z8HdfSmpStripeFn)(void *ctx, uint32_t row_begin, uint32_t row_end, unsigned worker_idx);

/* Weak default stubs for standalone host builds; strongly overridden by lut_core/smp.c in EL1 */
__attribute__((weak)) int v71r7_smp_ensure_started(void)
{
    return 0;
}

__attribute__((weak)) int v71r7_smp_run_stripes_copy(uint64_t helper_owner, uint32_t total_rows,
                                                     uint32_t stripe_rows, Z8HdfSmpStripeFn fn,
                                                     const void *ctx_src, size_t ctx_bytes,
                                                     unsigned caller_hb_slot)
{
    (void)helper_owner;
    (void)stripe_rows;
    (void)ctx_bytes;
    (void)caller_hb_slot;
    if (fn && total_rows > 0U) {
        fn((void *)(uintptr_t)ctx_src, 0U, total_rows, 0U);
    }
    return 0;
}

/* Closed-form exact equivalent of live_vcoord_next() for arbitrary row index cy (0-diff) */
static inline void live_vcoord_at(uint32_t cy, uint32_t dst_h, uint32_t src_h,
                                  uint32_t *gy0, uint32_t *gy1, uint32_t *fy_q8)
{
    uint32_t den = 2U * dst_h;
    uint32_t right_lim = (2U * src_h - 1U) * dst_h;
    uint32_t p = (2U * cy + 1U) * src_h;
    if (p <= dst_h) {
        *gy0 = 0U; *gy1 = 0U; *fy_q8 = 0U;
        return;
    }
    if (p >= right_lim) {
        *gy0 = src_h - 1U; *gy1 = src_h - 1U; *fy_q8 = 0U;
        return;
    }
    uint32_t num = p - dst_h;
    uint32_t g = num / den;
    uint32_t rem = num - g * den;
    uint32_t inv_den = (uint32_t)((1ULL << 32) / den) + 1U;
    uint32_t fy = (uint32_t)(((uint64_t)(rem * 256U + dst_h) * inv_den) >> 32);
    if (fy >= 256U) {
        g += 1U;
        fy = 0U;
    }
    if (g >= src_h - 1U) {
        *gy0 = src_h - 1U; *gy1 = src_h - 1U; *fy_q8 = 0U;
    } else {
        *gy0 = g; *gy1 = g + 1U; *fy_q8 = fy;
    }
}

typedef struct {
    LiveHdfWs *ws;
    uint8_t   *y;
    uint8_t   *c;
    int      (*cancelled)(void *);
    void      *cancel_ctx;
    uint32_t   y_stride;
    uint32_t   c_stride;
    uint32_t   ch;
    uint32_t   gh;
    uint32_t   level;
    uint32_t   monochrome;
} EvfHdfStripeJob;

static void evf_hdf_s1_stripe_cb(void *opaque, uint32_t gy_begin, uint32_t gy_end, unsigned worker_idx)
{
    const EvfHdfStripeJob *j = (const EvfHdfStripeJob *)opaque;
    uint32_t widx = (worker_idx < 3U) ? worker_idx : 0U;
    LiveHdfWs *ws = j->ws;
    const uint32_t gw = HDF_LIVE_GW;

    for (uint32_t gy = gy_begin; gy < gy_end; ++gy) {
        if ((gy & 3U) == 0U) {
            if (__atomic_load_n(&ws->evf_cancel_flag, __ATOMIC_ACQUIRE) != 0U) {
                return;
            }
            if (worker_idx == 0U && live_is_cancelled(j->cancelled, j->cancel_ctx)) {
                __atomic_store_n(&ws->evf_cancel_flag, 1U, __ATOMIC_RELEASE);
                return;
            }
        }
        uint32_t cy0 = (gy * j->ch) / j->gh;
        uint32_t cy1 = ((gy + 1U) * j->ch) / j->gh;
        uint32_t n_crows = cy1 - cy0;
        const uint8_t *y_rows[6];
        const uint8_t *c_rows[3];
        for (uint32_t r = 0U; r < n_crows; ++r) {
            uint32_t cy = cy0 + r;
            y_rows[2U * r]      = j->y + (size_t)(2U * cy) * j->y_stride;
            y_rows[2U * r + 1U] = j->y + (size_t)(2U * cy + 1U) * j->y_stride;
            c_rows[r]           = j->c + (size_t)cy * j->c_stride;
        }
        size_t goff = (size_t)gy * gw;
        hdf_live_evf_s1_rowgroup_256(y_rows, c_rows, n_crows,
                                     ws->u.s1_evf.boost_lo, ws->u.s1_evf.boost_hi,
                                     ws->grid_r + goff,
                                     ws->grid_g + goff,
                                     ws->grid_b + goff,
                                     ws->r256 + goff,
                                     ws->g256 + goff,
                                     ws->b256 + goff,
                                     &ws->c_edge_mask[gy],
                                     &ws->y_edge_mask[gy],
                                     ws->u.s1_evf.w[widx].boost_scratch,
                                     ws->u.s1_evf.w[widx].evf_y640,
                                     ws->u.s1_evf.w[widx].evf_cb640,
                                     ws->u.s1_evf.w[widx].evf_cr640,
                                     j->monochrome);
    }
}

static void evf_hdf_s3_stripe_cb(void *opaque, uint32_t gy_begin, uint32_t gy_end, unsigned worker_idx)
{
    const EvfHdfStripeJob *j = (const EvfHdfStripeJob *)opaque;
    uint32_t widx = (worker_idx < 3U) ? worker_idx : 0U;
    LiveHdfWs *ws = j->ws;
    const uint32_t gw = HDF_LIVE_GW;
    const uint16_t *gy_grid = j->monochrome ? ws->grid_r : ws->grid_y;

    for (uint32_t gy = gy_begin; gy < gy_end; ++gy) {
        if (gy > gy_begin && (gy & 3U) == 0U) {
            if (__atomic_load_n(&ws->evf_cancel_flag, __ATOMIC_ACQUIRE) != 0U) {
                return;
            }
            if (worker_idx == 0U && live_is_cancelled(j->cancelled, j->cancel_ctx)) {
                __atomic_store_n(&ws->evf_cancel_flag, 1U, __ATOMIC_RELEASE);
                return;
            }
        }
        uint32_t cy0 = (gy * j->ch) / j->gh;
        uint32_t cy1 = ((gy + 1U) * j->ch) / j->gh;
        uint32_t n_crows = cy1 - cy0;
        uint8_t *y_rows[6];
        uint8_t *c_rows[3];
        for (uint32_t r = 0U; r < n_crows; ++r) {
            uint32_t cy = cy0 + r;
            y_rows[2U * r]      = j->y + (size_t)(2U * cy) * j->y_stride;
            y_rows[2U * r + 1U] = j->y + (size_t)(2U * cy + 1U) * j->y_stride;
            c_rows[r]           = j->c + (size_t)cy * j->c_stride;
        }

        if (!j->monochrome) {
            size_t goff = (size_t)gy * gw;
            const uint16_t *gr_row = ws->grid_r + goff;
            const uint16_t *gg_row = ws->grid_g + goff;
            const uint16_t *gb_row = ws->grid_b + goff;
            uint32_t emask = ws->c_edge_mask[gy];
            uint32_t ychk  = ws->y_edge_mask[gy] & ~emask;
            if (ychk != 0U) {
                emask |= hdf_live_s3_check_schroma_mask(gr_row, gg_row, gb_row, ychk);
            }
            if (emask != 0xFFFFFFFFU) {
                const uint8_t *rin = ws->r256 + goff;
                const uint8_t *gin = ws->g256 + goff;
                const uint8_t *bin = ws->b256 + goff;
                hdf_live_s3_comp_chroma_256(rin, gin, bin,
                                            ws->u.s3_evf.w[widx].r_out,
                                            ws->u.s3_evf.w[widx].g_out,
                                            ws->u.s3_evf.w[widx].b_out,
                                            gr_row, gg_row, gb_row, j->level);
                hdf_live_evf_s3_apply_chroma_256_to_640(c_rows, n_crows,
                                                        rin, gin, bin,
                                                        ws->u.s3_evf.w[widx].r_out,
                                                        ws->u.s3_evf.w[widx].g_out,
                                                        ws->u.s3_evf.w[widx].b_out,
                                                        emask);
            }
            if (emask != 0U) {
                hdf_live_evf_s3_chroma_exact_masked((const uint8_t *const *)y_rows,
                                                    c_rows, n_crows,
                                                    gr_row, gg_row, gb_row,
                                                    emask, j->level,
                                                    ws->u.s3_evf.w[widx].fb_buf);
            }
        }

        for (uint32_t r = 0U; r < n_crows; ++r) {
            uint32_t gy0, gy1, fy;
            live_vcoord_at(cy0 + r, j->ch, j->gh, &gy0, &gy1, &fy);
            hdf_live_vlerp_q13_padded(gy_grid + (size_t)gy0 * gw,
                                      gy_grid + (size_t)gy1 * gw,
                                      ws->u.s3_evf.w[widx].vrow_y_pad, gw, fy);
            hdf_live_evf_hlerp_256_to_640(ws->u.s3_evf.w[widx].vrow_y_pad,
                                          ws->u.s3_evf.w[widx].vrow_y_640);
            hdf_live_s3_comp_y_rowpair_w(y_rows[2U * r], y_rows[2U * r + 1U],
                                         ws->u.s3_evf.w[widx].vrow_y_640, HDF_EVF_W, j->level);
        }
    }
}

int z8_hdf_live_nv12_1280(uint8_t *y, uint32_t y_stride,
                          uint8_t *c, uint32_t c_stride,
                          uint32_t height, uint32_t level,
                          uint32_t monochrome,
                          int (*cancelled)(void *), void *cancel_ctx,
                          void *workspace, size_t workspace_bytes) {
    if (level == Z8_HDF_OFF) {
        return Z8_HDF_DISABLED;
    }
    if (level > Z8_HDF_STRONG || monochrome > 1U || !y || !c) {
        return Z8_HDF_E_ARGUMENT;
    }
    if (height < 850U || height > 856U || (height & 1U) != 0U ||
        y_stride < HDF_EVF_W || c_stride < HDF_EVF_W) {
        return Z8_HDF_E_FORMAT;
    }
    if (!workspace || ((uintptr_t)workspace & (Z8_HDF_WORKSPACE_ALIGNMENT - 1U)) ||
        workspace_bytes < Z8_HDF_WORKSPACE_BYTES) {
        return Z8_HDF_E_WORKSPACE;
    }

    const uint32_t ch = height >> 1;
    size_t yn, cn;
    HdfRange yr, cr, wr;
    if (!plane_extent(y_stride, height, HDF_EVF_W, &yn) ||
        !plane_extent(c_stride, ch, HDF_EVF_W, &cn) ||
        !make_range(y, yn, &yr) || !make_range(c, cn, &cr)) {
        return Z8_HDF_E_CAPACITY;
    }
    if (!make_range(workspace, Z8_HDF_WORKSPACE_BYTES, &wr)) {
        return Z8_HDF_E_WORKSPACE;
    }
    if (overlaps(yr, cr) || overlaps(yr, wr) || overlaps(cr, wr)) {
        return Z8_HDF_E_ALIAS;
    }
    if (live_is_cancelled(cancelled, cancel_ctx)) {
        return Z8_HDF_CANCELLED;
    }

    LiveHdfWs *ws = (LiveHdfWs *)workspace;
    const HdfLevelPreset *pr = &hdf_presets[level];
    const uint32_t gw = HDF_LIVE_GW;
    const uint32_t gh = (ch == 425U) ? 170U : 171U;

    live_init_boost_lut(ws, pr);

    /* Ensure SMP pool is started lazily even when LUT is OFF (0 B Class 0x24) */
    (void)v71r7_smp_ensure_started();

    __atomic_store_n(&ws->evf_cancel_flag, 0U, __ATOMIC_RELAXED);
    EvfHdfStripeJob sjob = {
        ws, y, c, cancelled, cancel_ctx,
        y_stride, c_stride, ch, gh, level, monochrome
    };

    /* ---- Stage 1: Fused 1280x{850..856} Q15 Extract -> 256x{170,171} Q13 Grid (SMP CPU2+CPU1) */
    if (v71r7_smp_run_stripes_copy(UINT64_C(0x02000000), gh, 16U,
                                   evf_hdf_s1_stripe_cb, &sjob, sizeof(sjob), 4U) != 0) {
        return Z8_HDF_HUNG_SMP;
    }
    if (__atomic_load_n(&ws->evf_cancel_flag, __ATOMIC_ACQUIRE) != 0U) {
        return Z8_HDF_CANCELLED;
    }

    /* ---- Stage 2: 3-Scale Q13 Sigma-Matched Optical Pyramid + k Fold ---- */
    int rc = live_build_channel_pyramid(ws, ws->grid_r, gh, pr, cancelled, cancel_ctx);
    if (rc != Z8_HDF_OK) return rc;

    if (!monochrome) {
        rc = live_build_channel_pyramid(ws, ws->grid_g, gh, pr, cancelled, cancel_ctx);
        if (rc != Z8_HDF_OK) return rc;
        rc = live_build_channel_pyramid(ws, ws->grid_b, gh, pr, cancelled, cancel_ctx);
        if (rc != Z8_HDF_OK) return rc;
        if (live_is_cancelled(cancelled, cancel_ctx)) return Z8_HDF_CANCELLED;
        hdf_live_build_grid_y(ws->grid_r, ws->grid_g, ws->grid_b, ws->grid_y, gw * gh);
    }

    if (live_is_cancelled(cancelled, cancel_ctx)) {
        return Z8_HDF_CANCELLED;
    }

    /* ---- Stage 3: 256x{170,171} Direct Chroma Delta + Crisp Native 1280 Y (SMP CPU2+CPU1) */
    if (v71r7_smp_run_stripes_copy(UINT64_C(0x02000000), gh, 16U,
                                   evf_hdf_s3_stripe_cb, &sjob, sizeof(sjob), 4U) != 0) {
        return Z8_HDF_HUNG_SMP;
    }
    if (__atomic_load_n(&ws->evf_cancel_flag, __ATOMIC_ACQUIRE) != 0U) {
        return Z8_HDF_CANCELLED_DIRTY;
    }

    return Z8_HDF_OK;
}
