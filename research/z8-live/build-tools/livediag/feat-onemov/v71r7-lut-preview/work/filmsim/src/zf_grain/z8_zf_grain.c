/* SPDX-License-Identifier: MIT
 * Nikon Zf C3.01 Bit-Exact Film Grain Kernel — Orchestrator, ZFG1 Asset
 * Validator, Preset/Fuji Mapping Helpers, Two-Stage Streaming BlurLineRing,
 * and V71 Live Pre-Baked 512x256 Twisted-Torus Tile Baker.
 *
 * Pure integer freestanding C11:
 *   - Zero malloc, zero writable .data/.bss, zero floating-point, zero SIMD.
 *   - Safe under -mgeneral-regs-only -mstrict-align -ffreestanding -fno-builtin.
 *   - Caller-owned scratch workspace: 80 KiB (<= 96 KiB budget).
 */
#include "z8_zf_grain.h"
#include "z8_zf_grain_kernels.h"

#define ZF_GRAIN_TILE_W       1024U
#define ZF_GRAIN_ROW_PAD      16U
#define ZF_GRAIN_ROW_STRIDE   1072U /* 16 left + 1032 (1024 + 8 halo) + 24 right */
#define ZF_GRAIN_IDX_PERIOD   415U  /* M * P = 83 * 5 */
#define ZF_GRAIN_IDX_LEN      1472U /* >= 415 + 1032, 16-byte multiple */

#define ZF_LIVE_ROW_PAD       16U
#define ZF_LIVE_ROW_STRIDE    544U  /* 16 left + 512 active + 16 right, 16-byte multiple */

typedef struct {
    uintptr_t first;
    uintptr_t end;
} MemRange;

typedef struct {
    int16_t  grain_map[6896];                        /* 83*83 = 6889 padded -> 13,792 B */
    uint8_t  idx_lut[ZF_GRAIN_IDX_LEN];              /* 1,472 B */
    uint8_t  tone_lut[256];                          /* 256 B */
    uint32_t row_seed_jump[32];                      /* 128 B */
    uint32_t gf2_tmp_a[32];                          /* 128 B */
    uint32_t gf2_tmp_b[32];                          /* 128 B */
    uint8_t  halo_prev[Z8_ZF_GRAIN_MAX_HEIGHT * 4U]; /* 33,024 B */
    union {
        uint8_t u8[5][ZF_GRAIN_ROW_STRIDE];
        int16_t s16[5][ZF_GRAIN_ROW_STRIDE];
    } ring1;                                         /* 10,720 B */
    union {
        uint8_t u8[5][ZF_GRAIN_ROW_STRIDE];
        int16_t s16[5][ZF_GRAIN_ROW_STRIDE];
    } ring2;                                         /* 10,720 B */
    uint8_t  src_y_ring[5][ZF_GRAIN_ROW_STRIDE];     /* 5,360 B */
    uint8_t  stage_in_u8[ZF_GRAIN_ROW_STRIDE];       /* 1,072 B */
    int16_t  stage_out_s16[ZF_GRAIN_ROW_STRIDE];     /* 2,144 B */
    uint8_t  s16_blur3_safe;
    uint8_t  s16_blur5_safe;
    uint8_t  s16_mul_safe;
} ZfGrainScratch;

_Static_assert(sizeof(ZfGrainScratch) <= Z8_ZF_GRAIN_WORKSPACE_BYTES,
               "ZfGrainScratch must fit inside 80 KiB Z8_ZF_GRAIN_WORKSPACE_BYTES");

typedef struct {
    int16_t  grain_map[6896];                         /* 13,792 B (offset 0) */
    uint8_t  idx_lut[Z8_ZF_LIVE_TILE_W];              /*    512 B (offset 13,792) */
    uint32_t row_seed[Z8_ZF_LIVE_TILE_H];             /*  1,024 B (offset 14,304) */
    int16_t  raw_ring[5][ZF_LIVE_ROW_STRIDE];         /*  5,440 B (offset 15,328) */
    int16_t  mid_ring[5][ZF_LIVE_ROW_STRIDE];         /*  5,440 B (offset 20,768) */
    int16_t  twist_buf[2][ZF_LIVE_ROW_STRIDE];        /*  2,176 B (offset 26,208) */
    int16_t  out_row[ZF_LIVE_ROW_STRIDE];             /*  1,088 B (offset 28,384) */
    uint8_t  s16_blur3_safe;
    uint8_t  s16_blur5_safe;
} ZfLiveBakeScratch;

_Static_assert(sizeof(ZfLiveBakeScratch) <= Z8_ZF_GRAIN_WORKSPACE_BYTES,
               "ZfLiveBakeScratch must fit inside 80 KiB Z8_ZF_GRAIN_WORKSPACE_BYTES");

/* Read-only 16-entry IEEE 802.3 CRC32 nibble table (64 B in .rodata). */
static const uint32_t k_crc32_nibble[16] = {
    0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
    0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
    0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
    0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU
};

/* ARM64 DspFrameworkWrapper::FilmGrain (FUN_40e44bec) 18-byte DSP LUTs @ RAM 0xa02aec1f8 */
static const uint8_t k_full_weights_q4[6]  = { 2U, 3U, 7U, 11U, 16U, 16U };
static const uint8_t k_full_blur_sizes[3]  = { 3U, 5U, 9U };
static const uint8_t k_small_weights_q4[6] = { 2U, 2U, 3U, 4U, 6U, 9U };
static const uint8_t k_small_blur_sizes[3] = { 3U, 3U, 3U };

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int make_range(const void *ptr, size_t bytes, MemRange *out)
{
    uintptr_t addr = (uintptr_t)ptr;
    if (!ptr || bytes == 0U || bytes > UINTPTR_MAX - addr) return 0;
    out->first = addr;
    out->end = addr + bytes;
    return 1;
}

static int ranges_overlap(MemRange a, MemRange b)
{
    return a.first < b.end && b.first < a.end;
}

static int plane_min_bytes(uint32_t stride, uint32_t rows, uint32_t row_bytes, size_t *out)
{
    if (rows == 0U || row_bytes == 0U || stride < row_bytes) return 0;
    if ((size_t)(rows - 1U) > (SIZE_MAX - (size_t)row_bytes) / (size_t)stride) return 0;
    *out = (size_t)(rows - 1U) * (size_t)stride + (size_t)row_bytes;
    return 1;
}

static void copy_u8(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0U; i < n; ++i) {
        dst[i] = src[i];
    }
}

static uint32_t reflect101(int32_t i, uint32_t n)
{
    if (n <= 1U) return 0U;
    int32_t max_i = (int32_t)(n - 1U);
    while (i < 0 || i > max_i) {
        if (i < 0) i = -i;
        if (i > max_i) i = 2 * max_i - i;
    }
    return (uint32_t)i;
}

static uint32_t xorshift32_step(uint32_t s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static uint32_t gf2_mat_vec(const uint32_t mat[32], uint32_t v)
{
    uint32_t out = 0U;
    for (uint32_t b = 0U; b < 32U; ++b) {
        if ((v >> b) & 1U) {
            out ^= mat[b];
        }
    }
    return out;
}

static void gf2_mat_mul(const uint32_t a[32], const uint32_t b[32], uint32_t out[32])
{
    for (uint32_t j = 0U; j < 32U; ++j) {
        out[j] = gf2_mat_vec(a, b[j]);
    }
}

/* Compute M^steps in GF(2)^32 for xorshift32 row-stride jumping without stack arrays. */
static void build_xorshift_jump_matrix(uint32_t steps, uint32_t out_mat[32],
                                       uint32_t base[32], uint32_t tmp[32])
{
    for (uint32_t i = 0U; i < 32U; ++i) {
        out_mat[i] = UINT32_C(1) << i;
        base[i] = xorshift32_step(UINT32_C(1) << i);
    }
    uint32_t e = steps;
    while (e > 0U) {
        if (e & 1U) {
            gf2_mat_mul(base, out_mat, tmp);
            for (uint32_t i = 0U; i < 32U; ++i) out_mat[i] = tmp[i];
        }
        e >>= 1;
        if (e > 0U) {
            gf2_mat_mul(base, base, tmp);
            for (uint32_t i = 0U; i < 32U; ++i) base[i] = tmp[i];
        }
    }
}

static void pad_row_u8(uint8_t *row, uint32_t len, uint32_t r, int pad_left, int pad_right)
{
    if (pad_left) {
        for (uint32_t k = 1U; k <= r; ++k) {
            row[-(int32_t)k] = row[reflect101(-(int32_t)k, len)];
        }
    }
    if (pad_right) {
        for (uint32_t k = 1U; k <= r; ++k) {
            row[len - 1U + k] = row[reflect101((int32_t)(len - 1U) + (int32_t)k, len)];
        }
    }
}

static void pad_row_s16(int16_t *row, uint32_t len, uint32_t r, int pad_left, int pad_right)
{
    if (pad_left) {
        for (uint32_t k = 1U; k <= r; ++k) {
            row[-(int32_t)k] = row[reflect101(-(int32_t)k, len)];
        }
    }
    if (pad_right) {
        for (uint32_t k = 1U; k <= r; ++k) {
            row[len - 1U + k] = row[reflect101((int32_t)(len - 1U) + (int32_t)k, len)];
        }
    }
}

/* ============================================================================
 * Public Utility, ZFG1 Asset Validator, Preset & Fuji Mapping Helpers
 * ============================================================================ */

uint32_t z8_zf_grain_crc32_update(uint32_t crc, const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = ~crc;
    for (size_t i = 0U; i < bytes; ++i) {
        uint8_t b = p[i];
        c = k_crc32_nibble[(c ^ b) & 0x0FU] ^ (c >> 4);
        c = k_crc32_nibble[(c ^ (b >> 4)) & 0x0FU] ^ (c >> 4);
    }
    return ~c;
}

int z8_zf_grain_validate_bin(const void *buffer, size_t bytes, z8_zf_grain_maps *maps_out)
{
    if (!buffer) return Z8_ZF_GRAIN_E_ARGUMENT;
    if (bytes != Z8_ZF_GRAIN_FILE_BYTES) return Z8_ZF_GRAIN_E_CAPACITY;
    if (((uintptr_t)buffer & 1U) != 0U) return Z8_ZF_GRAIN_E_ALIGNMENT;

    const uint8_t *raw = (const uint8_t *)buffer;
    if (raw[0] != 'Z' || raw[1] != 'F' || raw[2] != 'G' || raw[3] != '1') {
        return Z8_ZF_GRAIN_E_MAGIC;
    }

    uint16_t version       = read_le16(raw + 4U);
    uint16_t header_bytes  = read_le16(raw + 6U);
    uint16_t map_dim       = read_le16(raw + 8U);
    uint16_t map_count     = read_le16(raw + 10U);
    uint32_t payload_bytes = read_le32(raw + 12U);
    uint32_t maps_crc      = read_le32(raw + 16U);
    uint32_t header_crc    = read_le32(raw + 20U);

    if (version != Z8_ZF_GRAIN_FILE_VERSION || header_bytes != Z8_ZF_GRAIN_HEADER_BYTES) {
        return Z8_ZF_GRAIN_E_VERSION;
    }
    if (map_dim != Z8_ZF_GRAIN_MAP_DIM || map_count != Z8_ZF_GRAIN_MAP_COUNT ||
        payload_bytes != Z8_ZF_GRAIN_PAYLOAD_BYTES ||
        raw[24] != Z8_ZF_GRAIN_TILE_PERIOD || raw[25] != Z8_ZF_GRAIN_TILE_STEP ||
        raw[26] != Z8_ZF_GRAIN_DEFAULT_KNEE_LO || raw[27] != Z8_ZF_GRAIN_DEFAULT_KNEE_HI ||
        raw[28] != 0U || raw[29] != 0U || raw[30] != 0U || raw[31] != 0U) {
        return Z8_ZF_GRAIN_E_FORMAT;
    }

    static const uint8_t k_zero4[4] = { 0U, 0U, 0U, 0U };
    uint32_t calc_hdr_crc = z8_zf_grain_crc32_update(0U, raw, 20U);
    calc_hdr_crc = z8_zf_grain_crc32_update(calc_hdr_crc, k_zero4, 4U);
    calc_hdr_crc = z8_zf_grain_crc32_update(calc_hdr_crc, raw + 24U, 8U);
    if (calc_hdr_crc != header_crc) {
        return Z8_ZF_GRAIN_E_CRC;
    }

    uint32_t calc_maps_crc = z8_zf_grain_crc32_update(
        0U, raw + Z8_ZF_GRAIN_HEADER_BYTES, Z8_ZF_GRAIN_PAYLOAD_BYTES);
    if (calc_maps_crc != maps_crc) {
        return Z8_ZF_GRAIN_E_CRC;
    }

    if (maps_out) {
        const int16_t *m0 = (const int16_t *)(const void *)(raw + Z8_ZF_GRAIN_HEADER_BYTES);
        maps_out->map0 = m0;
        maps_out->map1 = m0 + Z8_ZF_GRAIN_MAP_ELEMENTS;
    }
    return Z8_ZF_GRAIN_OK;
}

uint32_t z8_zf_grain_fold_seed(uint64_t shot_id)
{
    uint64_t x = shot_id ^ UINT64_C(0x9E3779B97F4A7C15);
    x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
    x = x ^ (x >> 31);
    uint32_t s = (uint32_t)(x ^ (x >> 32));
    return s ? s : Z8_ZF_GRAIN_DEFAULT_PRNG_SEED;
}

int z8_zf_grain_resolve_preset(const z8_zf_grain_preset *preset,
                               z8_zf_grain_dsp_params *out)
{
    if (!preset || !out) return Z8_ZF_GRAIN_E_ARGUMENT;
    if (preset->intensity > Z8_ZF_GRAIN_INTENSITY_MAX ||
        preset->blur_mode > Z8_ZF_GRAIN_BLUR_SHARP ||
        preset->reduced_res > 1U) {
        return Z8_ZF_GRAIN_E_ARGUMENT;
    }

    if (preset->intensity == 0U) {
        out->bypass     = 1U;
        out->blend_w    = 0U;
        out->blur_size  = 0U;
        out->map_select = 0U;
        out->knee_lo    = Z8_ZF_GRAIN_DEFAULT_KNEE_LO;
        out->knee_hi    = Z8_ZF_GRAIN_DEFAULT_KNEE_HI;
        out->blur_mode  = preset->blur_mode;
        out->reserved   = 0U;
        return Z8_ZF_GRAIN_OK;
    }

    if (preset->size < Z8_ZF_GRAIN_SIZE_MIN || preset->size > Z8_ZF_GRAIN_SIZE_MAX) {
        return Z8_ZF_GRAIN_E_ARGUMENT;
    }

    uint32_t i_idx = (uint32_t)preset->intensity - 1U;
    uint32_t s_idx = (uint32_t)preset->size - 1U;

    out->bypass   = 0U;
    out->knee_lo  = Z8_ZF_GRAIN_DEFAULT_KNEE_LO;
    out->knee_hi  = Z8_ZF_GRAIN_DEFAULT_KNEE_HI;
    out->blur_mode = preset->blur_mode;
    out->reserved = 0U;

    if (preset->reduced_res) {
        out->blend_w    = k_small_weights_q4[i_idx];
        out->blur_size  = k_small_blur_sizes[s_idx];
        out->map_select = 0U;
    } else {
        out->blend_w    = k_full_weights_q4[i_idx];
        out->blur_size  = k_full_blur_sizes[s_idx];
        out->map_select = (preset->intensity == Z8_ZF_GRAIN_INTENSITY_MAX) ? 1U : 0U;
    }
    return Z8_ZF_GRAIN_OK;
}

/* ============================================================================
 * Core Streaming Execution Engine (EXACT & SHARP Modes)
 * ============================================================================ */

static int validate_dsp_params(const z8_zf_grain_dsp_params *p)
{
    if (!p) return Z8_ZF_GRAIN_E_ARGUMENT;
    if (p->bypass > 1U || p->blur_mode > Z8_ZF_GRAIN_BLUR_SHARP || p->reserved != 0U) {
        return Z8_ZF_GRAIN_E_ARGUMENT;
    }
    if (p->bypass == 1U) return Z8_ZF_GRAIN_DISABLED;
    if (p->blend_w > Z8_ZF_GRAIN_MAX_BLEND_W || p->map_select > 1U) {
        return Z8_ZF_GRAIN_E_ARGUMENT;
    }
    if (p->blur_size >= 10U || (((Z8_ZF_GRAIN_BLUR_VALID_MASK >> p->blur_size) & 1U) == 0U)) {
        return Z8_ZF_GRAIN_E_ARGUMENT;
    }
    return Z8_ZF_GRAIN_OK;
}

static int32_t scan_map_max_abs(const int16_t *grain_map)
{
    int32_t max_abs = 0;
    for (uint32_t i = 0U; i < Z8_ZF_GRAIN_MAP_ELEMENTS; ++i) {
        int32_t v = grain_map[i];
        int32_t av = (v < 0) ? -v : v;
        if (av > max_abs) max_abs = av;
    }
    return max_abs;
}

static void prepare_scratch_tables(ZfGrainScratch *sc, const int16_t *map_a,
                                   const z8_zf_grain_dsp_params *p)
{
    /* 1. Scale 83x83 int16 map: grain_map[i] = (w * mapA[i]) >> 4 (mapB == 0). */
    zf_grain_scale_map_s16(map_a, Z8_ZF_GRAIN_MAP_ELEMENTS, p->blend_w, sc->grain_map);
    int32_t max_abs = scan_map_max_abs(sc->grain_map);
    sc->s16_mul_safe   = (max_abs <= 511)  ? 1U : 0U;
    sc->s16_blur3_safe = (max_abs <= 2047) ? 1U : 0U;
    sc->s16_blur5_safe = (max_abs <= 4095) ? 1U : 0U;

    /* 2. Staggered coordinate LUT (period P*M = 5*83 = 415, tiled for contiguous strip reads). */
    for (uint32_t k = 0U; k < ZF_GRAIN_IDX_PERIOD; ++k) {
        sc->idx_lut[k] = (uint8_t)(((k / Z8_ZF_GRAIN_TILE_PERIOD) * Z8_ZF_GRAIN_TILE_STEP +
                                    (k % Z8_ZF_GRAIN_TILE_PERIOD)) % Z8_ZF_GRAIN_MAP_DIM);
    }
    for (uint32_t k = ZF_GRAIN_IDX_PERIOD; k < ZF_GRAIN_IDX_LEN; ++k) {
        sc->idx_lut[k] = sc->idx_lut[k - ZF_GRAIN_IDX_PERIOD];
    }

    /* 3. Q6 piecewise-linear shadow/highlight attenuation LUT [0..64]. */
    uint32_t k_lo = p->knee_lo;
    uint32_t k_hi = p->knee_hi;
    for (uint32_t yv = 0U; yv < 256U; ++yv) {
        uint32_t t = 64U;
        if (k_lo > 0U && yv < k_lo) {
            t = (yv << 6) / k_lo;
        }
        if (k_hi < 255U && yv > k_hi) {
            t = ((255U - yv) << 6) / (255U - k_hi);
        }
        sc->tone_lut[yv] = (uint8_t)t;
    }
}

static void blur_radii_from_size(uint8_t blur_size, uint32_t *r1_out, uint32_t *r2_out)
{
    switch (blur_size) {
    case 3U: *r1_out = 0U; *r2_out = 1U; break;
    case 5U: *r1_out = 0U; *r2_out = 2U; break;
    case 7U: *r1_out = 2U; *r2_out = 1U; break;
    case 9U: *r1_out = 2U; *r2_out = 2U; break;
    default: *r1_out = 0U; *r2_out = 0U; break;
    }
}

static __attribute__((noinline)) int run_no_blur_pass(
    const z8_zf_grain_plane_job *job, ZfGrainScratch *sc,
    uint32_t seed0, int std_knees, int sharp)
{
    uint32_t w = job->width;
    uint32_t h = job->height;
    uint32_t state = seed0;
    int dirty = 0;

    for (uint32_t y = 0U; y < h; ++y) {
        if (job->cancelled && job->cancelled(job->cancel_context)) {
            return dirty ? Z8_ZF_GRAIN_CANCELLED_DIRTY : Z8_ZF_GRAIN_CANCELLED;
        }
        const uint8_t *src_row = job->src_y + (size_t)y * job->src_y_stride;
        uint8_t *dst_row = job->dst_y + (size_t)y * job->dst_y_stride;
        const int16_t *gm_row = sc->grain_map + (size_t)sc->idx_lut[y % ZF_GRAIN_IDX_PERIOD] * Z8_ZF_GRAIN_MAP_DIM;

        for (uint32_t x0 = 0U; x0 < w; x0 += ZF_GRAIN_TILE_W) {
            uint32_t chunk = w - x0;
            if (chunk > ZF_GRAIN_TILE_W) chunk = ZF_GRAIN_TILE_W;
            const uint8_t *x_idx = sc->idx_lut + (x0 % ZF_GRAIN_IDX_PERIOD);
            if (sharp) {
                state = zf_grain_apply_line_sharp_fast(
                    src_row + x0, sc->stage_out_s16, gm_row, x_idx,
                    sc->tone_lut, chunk, state, 0U, std_knees, sc->s16_mul_safe);
                zf_grain_add_delta_u8_row(src_row + x0, sc->stage_out_s16, dst_row + x0, chunk);
            } else {
                state = zf_grain_apply_line_exact(
                    src_row + x0, dst_row + x0, gm_row, x_idx,
                    sc->tone_lut, chunk, state, 0U, std_knees);
            }
        }
        dirty = 1;
    }
    return Z8_ZF_GRAIN_OK;
}

typedef struct {
    const z8_zf_grain_plane_job *job;
    ZfGrainScratch              *sc;
    uint32_t                     x0;
    uint32_t                     strip_w;
    uint32_t                     p1_x0;
    uint32_t                     r2;
    int                          in_place;
    int                          sharp;
    uint32_t                     p2_out_y;
    int                          dirty;
} Stage2Ctx;

static void pump_stage2(Stage2Ctx *c, uint32_t p2_in_y)
{
    uint32_t h = c->job->height;
    uint32_t r2 = c->r2;
    uint32_t off2 = ZF_GRAIN_ROW_PAD;

    while (c->p2_out_y < h && (p2_in_y == h - 1U || c->p2_out_y + r2 <= p2_in_y)) {
        uint32_t oy = c->p2_out_y;
        uint8_t *dst_ptr = c->job->dst_y + (size_t)oy * c->job->dst_y_stride + c->x0;

        if (!c->sharp) {
            if (r2 == 1U) {
                const uint8_t *a0 = c->sc->ring2.u8[reflect101((int32_t)oy - 1, h) % 5U] + off2;
                const uint8_t *a1 = c->sc->ring2.u8[oy % 5U] + off2;
                const uint8_t *a2 = c->sc->ring2.u8[reflect101((int32_t)oy + 1, h) % 5U] + off2;
                zf_grain_blur3_u8_row(a0, a1, a2, dst_ptr, c->strip_w);
            } else {
                const uint8_t *a0 = c->sc->ring2.u8[reflect101((int32_t)oy - 2, h) % 5U] + off2;
                const uint8_t *a1 = c->sc->ring2.u8[reflect101((int32_t)oy - 1, h) % 5U] + off2;
                const uint8_t *a2 = c->sc->ring2.u8[oy % 5U] + off2;
                const uint8_t *a3 = c->sc->ring2.u8[reflect101((int32_t)oy + 1, h) % 5U] + off2;
                const uint8_t *a4 = c->sc->ring2.u8[reflect101((int32_t)oy + 2, h) % 5U] + off2;
                zf_grain_blur5_u8_row(a0, a1, a2, a3, a4, dst_ptr, c->strip_w);
            }
        } else {
            if (r2 == 1U) {
                const int16_t *a0 = c->sc->ring2.s16[reflect101((int32_t)oy - 1, h) % 5U] + off2;
                const int16_t *a1 = c->sc->ring2.s16[oy % 5U] + off2;
                const int16_t *a2 = c->sc->ring2.s16[reflect101((int32_t)oy + 1, h) % 5U] + off2;
                if (c->sc->s16_blur3_safe) {
                    zf_grain_blur3_s16_row_fast(a0, a1, a2, c->sc->stage_out_s16, c->strip_w);
                } else {
                    zf_grain_blur3_s16_row(a0, a1, a2, c->sc->stage_out_s16, c->strip_w);
                }
            } else {
                const int16_t *a0 = c->sc->ring2.s16[reflect101((int32_t)oy - 2, h) % 5U] + off2;
                const int16_t *a1 = c->sc->ring2.s16[reflect101((int32_t)oy - 1, h) % 5U] + off2;
                const int16_t *a2 = c->sc->ring2.s16[oy % 5U] + off2;
                const int16_t *a3 = c->sc->ring2.s16[reflect101((int32_t)oy + 1, h) % 5U] + off2;
                const int16_t *a4 = c->sc->ring2.s16[reflect101((int32_t)oy + 2, h) % 5U] + off2;
                if (c->sc->s16_blur5_safe) {
                    zf_grain_blur5_s16_row_fast(a0, a1, a2, a3, a4, c->sc->stage_out_s16, c->strip_w);
                } else {
                    zf_grain_blur5_s16_row(a0, a1, a2, a3, a4, c->sc->stage_out_s16, c->strip_w);
                }
            }
            const uint8_t *orig_y = c->in_place
                ? c->sc->src_y_ring[oy % 5U]
                : (c->job->src_y + (size_t)oy * c->job->src_y_stride + c->x0);
            zf_grain_add_delta_u8_row(orig_y, c->sc->stage_out_s16, dst_ptr, c->strip_w);
        }

        c->dirty = 1;
        c->p2_out_y++;
    }
}

static __attribute__((noinline)) int run_blur_cascade(
    const z8_zf_grain_plane_job *job, ZfGrainScratch *sc,
    uint32_t r1, uint32_t r2, uint32_t seed0, int std_knees, int in_place, int sharp)
{
    uint32_t w = job->width;
    uint32_t h = job->height;
    uint32_t r_tot = r1 + r2;
    int multi_strip = (w > ZF_GRAIN_TILE_W) ? 1 : 0;

    if (multi_strip) {
        uint32_t b32 = (w + 31U) >> 5;
        build_xorshift_jump_matrix(b32, sc->row_seed_jump, sc->gf2_tmp_a, sc->gf2_tmp_b);
    }

    int any_dirty = 0;
    uint32_t x0 = 0U;

    while (x0 < w) {
        uint32_t rem = w - x0;
        uint32_t strip_w;
        if (rem <= ZF_GRAIN_TILE_W) {
            strip_w = rem;
        } else if (rem < ZF_GRAIN_TILE_W + 32U) {
            strip_w = rem - 32U; /* guarantee final strip width >= 32 */
        } else {
            strip_w = ZF_GRAIN_TILE_W;
        }
        uint32_t x1 = x0 + strip_w;

        uint32_t hx0 = (x0 > 0U) ? (x0 - r_tot) : 0U;
        uint32_t hx1 = (x1 < w)  ? (x1 + r_tot) : w;
        uint32_t noisy_len = hx1 - hx0;

        uint32_t p1_x0 = (x0 > 0U) ? (x0 - r2) : 0U;
        uint32_t p1_x1 = (x1 < w)  ? (x1 + r2) : w;
        uint32_t p1_len = p1_x1 - p1_x0;

        uint32_t start_blk = hx0 >> 5;
        uint32_t bit_off = hx0 & 31U;
        uint32_t s_row = seed0;
        for (uint32_t b = 0U; b < start_blk; ++b) {
            s_row = xorshift32_step(s_row);
        }

        int pad_l = (x0 == 0U) ? 1 : 0;
        int pad_r = (x1 == w)  ? 1 : 0;
        const uint8_t *x_idx = sc->idx_lut + (hx0 % ZF_GRAIN_IDX_PERIOD);
        uint32_t p1_out_y = 0U;
        uint32_t off1      = ZF_GRAIN_ROW_PAD - (x0 - p1_x0);
        uint32_t ring_base = ZF_GRAIN_ROW_PAD - (x0 - hx0);
        uint32_t p2_base   = ZF_GRAIN_ROW_PAD - (x0 - p1_x0);

        Stage2Ctx s2;
        s2.job      = job;
        s2.sc       = sc;
        s2.x0       = x0;
        s2.strip_w  = strip_w;
        s2.p1_x0    = p1_x0;
        s2.r2       = r2;
        s2.in_place = in_place;
        s2.sharp    = sharp;
        s2.p2_out_y = 0U;
        s2.dirty    = any_dirty;

        for (uint32_t y = 0U; y < h; ++y) {
            if (job->cancelled && job->cancelled(job->cancel_context)) {
                return s2.dirty ? Z8_ZF_GRAIN_CANCELLED_DIRTY : Z8_ZF_GRAIN_CANCELLED;
            }

            const uint8_t *src_row = job->src_y + (size_t)y * job->src_y_stride;
            const uint8_t *in_ptr;
            if (in_place && x0 > 0U) {
                uint8_t *base_in = sc->stage_in_u8 + ZF_GRAIN_ROW_PAD;
                copy_u8(base_in - r_tot, sc->halo_prev + (size_t)y * 4U, r_tot);
                zf_grain_copy_u8_fast(base_in, src_row + x0, hx1 - x0);
                in_ptr = base_in - r_tot;
            } else {
                in_ptr = src_row + hx0;
            }

            if (in_place && x1 < w) {
                copy_u8(sc->halo_prev + (size_t)y * 4U, src_row + (x1 - r_tot), r_tot);
            }
            if (sharp && in_place) {
                zf_grain_copy_u8_fast(sc->src_y_ring[y % 5U], src_row + x0, strip_w);
            }

            const int16_t *gm_row = sc->grain_map +
                (size_t)sc->idx_lut[y % ZF_GRAIN_IDX_PERIOD] * Z8_ZF_GRAIN_MAP_DIM;
            uint32_t s_after;

            if (!sharp) {
                uint8_t *dst_slot = (r1 > 0U)
                    ? (sc->ring1.u8[y % 5U] + ring_base)
                    : (sc->ring2.u8[y % 5U] + ring_base);
                s_after = zf_grain_apply_line_exact(
                    in_ptr, dst_slot, gm_row, x_idx, sc->tone_lut,
                    noisy_len, s_row, bit_off, std_knees);
                pad_row_u8(dst_slot, noisy_len, (r1 > 0U) ? r1 : r2, pad_l, pad_r);
            } else {
                int16_t *dst_slot = (r1 > 0U)
                    ? (sc->ring1.s16[y % 5U] + ring_base)
                    : (sc->ring2.s16[y % 5U] + ring_base);
                s_after = zf_grain_apply_line_sharp_fast(
                    in_ptr, dst_slot, gm_row, x_idx, sc->tone_lut,
                    noisy_len, s_row, bit_off, std_knees, sc->s16_mul_safe);
                pad_row_s16(dst_slot, noisy_len, (r1 > 0U) ? r1 : r2, pad_l, pad_r);
            }

            s_row = multi_strip ? gf2_mat_vec(sc->row_seed_jump, s_row) : s_after;

            if (r1 > 0U) {
                while (p1_out_y < h && (y == h - 1U || p1_out_y + r1 <= y)) {
                    uint32_t oy1 = p1_out_y;
                    if (!sharp) {
                        const uint8_t *a0 = sc->ring1.u8[reflect101((int32_t)oy1 - 2, h) % 5U] + off1;
                        const uint8_t *a1 = sc->ring1.u8[reflect101((int32_t)oy1 - 1, h) % 5U] + off1;
                        const uint8_t *a2 = sc->ring1.u8[oy1 % 5U] + off1;
                        const uint8_t *a3 = sc->ring1.u8[reflect101((int32_t)oy1 + 1, h) % 5U] + off1;
                        const uint8_t *a4 = sc->ring1.u8[reflect101((int32_t)oy1 + 2, h) % 5U] + off1;
                        uint8_t *p2_slot = sc->ring2.u8[oy1 % 5U] + p2_base;
                        zf_grain_blur5_u8_row(a0, a1, a2, a3, a4, p2_slot, p1_len);
                        pad_row_u8(p2_slot, p1_len, r2, pad_l, pad_r);
                    } else {
                        const int16_t *a0 = sc->ring1.s16[reflect101((int32_t)oy1 - 2, h) % 5U] + off1;
                        const int16_t *a1 = sc->ring1.s16[reflect101((int32_t)oy1 - 1, h) % 5U] + off1;
                        const int16_t *a2 = sc->ring1.s16[oy1 % 5U] + off1;
                        const int16_t *a3 = sc->ring1.s16[reflect101((int32_t)oy1 + 1, h) % 5U] + off1;
                        const int16_t *a4 = sc->ring1.s16[reflect101((int32_t)oy1 + 2, h) % 5U] + off1;
                        int16_t *p2_slot = sc->ring2.s16[oy1 % 5U] + p2_base;
                        if (sc->s16_blur5_safe) {
                            zf_grain_blur5_s16_row_fast(a0, a1, a2, a3, a4, p2_slot, p1_len);
                        } else {
                            zf_grain_blur5_s16_row(a0, a1, a2, a3, a4, p2_slot, p1_len);
                        }
                        pad_row_s16(p2_slot, p1_len, r2, pad_l, pad_r);
                    }
                    pump_stage2(&s2, oy1);
                    p1_out_y++;
                }
            } else {
                pump_stage2(&s2, y);
            }
        }

        any_dirty = s2.dirty;
        x0 = x1;
    }

    return Z8_ZF_GRAIN_OK;
}

int z8_zf_grain_process_plane(const z8_zf_grain_plane_job *job,
                              void *workspace, size_t workspace_bytes)
{
    if (!job || job->abi != Z8_ZF_GRAIN_ABI) return Z8_ZF_GRAIN_E_ARGUMENT;

    int prc = validate_dsp_params(&job->params);
    if (prc != Z8_ZF_GRAIN_OK) return prc;

    if (!job->maps) return Z8_ZF_GRAIN_E_MAPS;
    const int16_t *map_a = job->params.map_select ? job->maps->map1 : job->maps->map0;
    MemRange map_range;
    if (!map_a || (((uintptr_t)map_a & 1U) != 0U) ||
        !make_range(map_a, Z8_ZF_GRAIN_MAP_BYTES, &map_range)) {
        return Z8_ZF_GRAIN_E_MAPS;
    }

    if (job->width == 0U || job->width > Z8_ZF_GRAIN_MAX_WIDTH ||
        job->height == 0U || job->height > Z8_ZF_GRAIN_MAX_HEIGHT) {
        return Z8_ZF_GRAIN_E_FORMAT;
    }

    MemRange work_range, job_range;
    if (!workspace || (((uintptr_t)workspace & (Z8_ZF_GRAIN_WORKSPACE_ALIGNMENT - 1U)) != 0U) ||
        workspace_bytes < Z8_ZF_GRAIN_WORKSPACE_BYTES ||
        !make_range(workspace, workspace_bytes, &work_range)) {
        return Z8_ZF_GRAIN_E_WORKSPACE;
    }
    work_range.end = work_range.first + Z8_ZF_GRAIN_WORKSPACE_BYTES;

    if (!make_range(job, sizeof(*job), &job_range) ||
        ranges_overlap(job_range, work_range) ||
        ranges_overlap(map_range, work_range)) {
        return Z8_ZF_GRAIN_E_ALIAS;
    }

    size_t src_min = 0U, dst_min = 0U;
    MemRange src_range, dst_range, decl;
    if (!plane_min_bytes(job->src_y_stride, job->height, job->width, &src_min) ||
        !plane_min_bytes(job->dst_y_stride, job->height, job->width, &dst_min) ||
        job->src_y_bytes < src_min || job->dst_y_bytes < dst_min ||
        !make_range(job->src_y, job->src_y_bytes, &decl) ||
        !make_range(job->src_y, src_min, &src_range) ||
        !make_range(job->dst_y, job->dst_y_bytes, &decl) ||
        !make_range(job->dst_y, dst_min, &dst_range)) {
        return Z8_ZF_GRAIN_E_CAPACITY;
    }

    int in_place = (job->src_y == job->dst_y) ? 1 : 0;
    if (in_place) {
        if (job->src_y_stride != job->dst_y_stride) return Z8_ZF_GRAIN_E_ALIAS;
    } else {
        if (ranges_overlap(src_range, dst_range)) return Z8_ZF_GRAIN_E_ALIAS;
    }

    if (ranges_overlap(src_range, work_range) ||
        ranges_overlap(dst_range, work_range) ||
        ranges_overlap(dst_range, map_range)  ||
        ranges_overlap(dst_range, job_range)) {
        return Z8_ZF_GRAIN_E_ALIAS;
    }

    if (job->cancelled && job->cancelled(job->cancel_context)) {
        return Z8_ZF_GRAIN_CANCELLED;
    }

    ZfGrainScratch *sc = (ZfGrainScratch *)workspace;
    prepare_scratch_tables(sc, map_a, &job->params);

    uint32_t seed0 = job->prng_seed ? job->prng_seed : Z8_ZF_GRAIN_DEFAULT_PRNG_SEED;
    int std_knees = (job->params.knee_lo == Z8_ZF_GRAIN_DEFAULT_KNEE_LO &&
                     job->params.knee_hi == Z8_ZF_GRAIN_DEFAULT_KNEE_HI) ? 1 : 0;
    int sharp = (job->params.blur_mode == Z8_ZF_GRAIN_BLUR_SHARP) ? 1 : 0;

    uint32_t r1 = 0U, r2 = 0U;
    blur_radii_from_size(job->params.blur_size, &r1, &r2);

    if (r1 == 0U && r2 == 0U) {
        return run_no_blur_pass(job, sc, seed0, std_knees, sharp);
    }
    return run_blur_cascade(job, sc, r1, r2, seed0, std_knees, in_place, sharp);
}

int z8_zf_grain_apply_group(const z8_zf_grain_group_job *job,
                            void *workspace, size_t workspace_bytes)
{
    if (!job || job->abi != Z8_ZF_GRAIN_ABI ||
        job->count == 0U || job->count > Z8_ZF_GRAIN_MAX_PLANES) {
        return Z8_ZF_GRAIN_E_ARGUMENT;
    }

    z8_zf_grain_dsp_params base_dsp;
    int rc = z8_zf_grain_resolve_preset(&job->preset, &base_dsp);
    if (rc != Z8_ZF_GRAIN_OK) return rc;
    if (base_dsp.bypass) return Z8_ZF_GRAIN_DISABLED;

    if (!job->maps || !job->maps->map0 || !job->maps->map1 ||
        (((uintptr_t)job->maps->map0 & 1U) != 0U) ||
        (((uintptr_t)job->maps->map1 & 1U) != 0U)) {
        return Z8_ZF_GRAIN_E_MAPS;
    }

    MemRange m0_range, m1_range, work_range, job_range;
    if (!make_range(job->maps->map0, Z8_ZF_GRAIN_MAP_BYTES, &m0_range) ||
        !make_range(job->maps->map1, Z8_ZF_GRAIN_MAP_BYTES, &m1_range)) {
        return Z8_ZF_GRAIN_E_MAPS;
    }
    if (!workspace || (((uintptr_t)workspace & (Z8_ZF_GRAIN_WORKSPACE_ALIGNMENT - 1U)) != 0U) ||
        workspace_bytes < Z8_ZF_GRAIN_WORKSPACE_BYTES ||
        !make_range(workspace, workspace_bytes, &work_range)) {
        return Z8_ZF_GRAIN_E_WORKSPACE;
    }
    work_range.end = work_range.first + Z8_ZF_GRAIN_WORKSPACE_BYTES;
    if (!make_range(job, sizeof(*job), &job_range) ||
        ranges_overlap(job_range, work_range) ||
        ranges_overlap(m0_range, work_range) ||
        ranges_overlap(m1_range, work_range)) {
        return Z8_ZF_GRAIN_E_ALIAS;
    }

    MemRange plane_ranges[Z8_ZF_GRAIN_MAX_PLANES];
    uint64_t max_area = 0ULL;

    for (uint32_t i = 0U; i < job->count; ++i) {
        const z8_zf_grain_plane *p = &job->planes[i];
        if (p->res_mode > Z8_ZF_GRAIN_RES_REDUCED ||
            p->reserved[0] != 0U || p->reserved[1] != 0U || p->reserved[2] != 0U) {
            return Z8_ZF_GRAIN_E_ARGUMENT;
        }
        if (p->width == 0U || p->width > Z8_ZF_GRAIN_MAX_WIDTH ||
            p->height == 0U || p->height > Z8_ZF_GRAIN_MAX_HEIGHT) {
            return Z8_ZF_GRAIN_E_FORMAT;
        }
        size_t min_b = 0U;
        MemRange decl;
        if (!plane_min_bytes(p->y_stride, p->height, p->width, &min_b) ||
            p->y_bytes < min_b ||
            !make_range(p->y, p->y_bytes, &decl) ||
            !make_range(p->y, min_b, &plane_ranges[i])) {
            return Z8_ZF_GRAIN_E_CAPACITY;
        }
        if (ranges_overlap(plane_ranges[i], work_range) ||
            ranges_overlap(plane_ranges[i], m0_range) ||
            ranges_overlap(plane_ranges[i], m1_range) ||
            ranges_overlap(plane_ranges[i], job_range)) {
            return Z8_ZF_GRAIN_E_ALIAS;
        }
        for (uint32_t j = 0U; j < i; ++j) {
            if (ranges_overlap(plane_ranges[i], plane_ranges[j])) {
                return Z8_ZF_GRAIN_E_ALIAS;
            }
        }
        uint64_t area = (uint64_t)p->width * (uint64_t)p->height;
        if (area > max_area) max_area = area;
    }

    if (job->cancelled && job->cancelled(job->cancel_context)) {
        return Z8_ZF_GRAIN_CANCELLED;
    }

    int any_dirty = 0;
    for (uint32_t i = 0U; i < job->count; ++i) {
        const z8_zf_grain_plane *p = &job->planes[i];
        z8_zf_grain_preset p_preset = job->preset;
        if (p->res_mode == Z8_ZF_GRAIN_RES_FULL) {
            p_preset.reduced_res = 0U;
        } else if (p->res_mode == Z8_ZF_GRAIN_RES_REDUCED) {
            p_preset.reduced_res = 1U;
        } else if (job->preset.reduced_res == 0U) {
            uint64_t area = (uint64_t)p->width * (uint64_t)p->height;
            p_preset.reduced_res = (area < max_area) ? 1U : 0U;
        }

        z8_zf_grain_dsp_params dsp;
        rc = z8_zf_grain_resolve_preset(&p_preset, &dsp);
        if (rc != Z8_ZF_GRAIN_OK) return rc;

        z8_zf_grain_plane_job pj;
        pj.abi            = Z8_ZF_GRAIN_ABI;
        pj.maps           = job->maps;
        pj.params         = dsp;
        pj.src_y          = p->y;
        pj.src_y_bytes    = p->y_bytes;
        pj.src_y_stride   = p->y_stride;
        pj.dst_y          = p->y;
        pj.dst_y_bytes    = p->y_bytes;
        pj.dst_y_stride   = p->y_stride;
        pj.width          = p->width;
        pj.height         = p->height;
        pj.prng_seed      = z8_zf_grain_fold_seed(
            job->shot_id ^ ((uint64_t)i * UINT64_C(0x9E3779B97F4A7C15)));
        pj.cancelled      = job->cancelled;
        pj.cancel_context = job->cancel_context;

        rc = z8_zf_grain_process_plane(&pj, workspace, workspace_bytes);
        if (rc == Z8_ZF_GRAIN_CANCELLED) {
            return any_dirty ? Z8_ZF_GRAIN_CANCELLED_DIRTY : Z8_ZF_GRAIN_CANCELLED;
        }
        if (rc != Z8_ZF_GRAIN_OK) return rc;
        any_dirty = 1;
    }

    return Z8_ZF_GRAIN_OK;
}

/* ============================================================================
 * V71 Live Pre-Baked Seamless 512x256 Twisted-Torus Tile Baker (<= 80 KiB WS)
 * ============================================================================ */

static void live_pad_periodic_s16(int16_t *row, uint32_t r)
{
    for (uint32_t k = 1U; k <= r; ++k) {
        row[-(int32_t)k] = row[Z8_ZF_LIVE_TILE_W - k];
        row[Z8_ZF_LIVE_TILE_W - 1U + k] = row[k - 1U];
    }
}

static const int16_t *live_fetch_torus_row(const int16_t *src_row, int32_t sy,
                                           uint32_t r, ZfLiveBakeScratch *bs,
                                           uint32_t *twist_slot)
{
    if (sy >= 0 && sy < (int32_t)Z8_ZF_LIVE_TILE_H) {
        return src_row;
    }
    uint32_t sx = (sy < 0)
        ? (Z8_ZF_LIVE_TILE_W - Z8_ZF_LIVE_TILE_TWIST)
        : Z8_ZF_LIVE_TILE_TWIST;
    int16_t *dst = bs->twist_buf[*twist_slot] + ZF_LIVE_ROW_PAD;
    *twist_slot = (*twist_slot + 1U) & 1U;

    uint32_t tail = Z8_ZF_LIVE_TILE_W - sx;
    for (uint32_t x = 0U; x < tail; ++x) {
        dst[x] = src_row[x + sx];
    }
    for (uint32_t x = 0U; x < sx; ++x) {
        dst[tail + x] = src_row[x];
    }
    live_pad_periodic_s16(dst, r);
    return dst;
}

static void live_gen_raw_row(const ZfLiveBakeScratch *bs, uint32_t ty,
                             int16_t *dst_row, uint32_t r)
{
    const int16_t *gm_row = bs->grain_map + (size_t)bs->idx_lut[ty] * Z8_ZF_GRAIN_MAP_DIM;
    uint32_t state = bs->row_seed[ty];

    for (uint32_t x = 0U; x < Z8_ZF_LIVE_TILE_W; x += 32U) {
        state = xorshift32_step(state);
        uint32_t bits = state;
        for (uint32_t b = 0U; b < 32U; ++b) {
            int16_t g = gm_row[bs->idx_lut[x + b]];
            dst_row[x + b] = (bits & 1U) ? g : (int16_t)(uint16_t)(0U - (uint16_t)g);
            bits >>= 1;
        }
    }
    if (r > 0U) {
        live_pad_periodic_s16(dst_row, r);
    }
}

static void live_blur_torus_row(ZfLiveBakeScratch *bs,
                                int16_t (*ring)[ZF_LIVE_ROW_STRIDE],
                                int32_t center_lin, uint32_t center_ty,
                                uint32_t r, int16_t *dst_row)
{
    uint32_t twist_slot = 0U;
    if (r == 1U) {
        const int16_t *a0 = live_fetch_torus_row(
            ring[(uint32_t)(center_lin - 1 + 10) % 5U] + ZF_LIVE_ROW_PAD,
            (int32_t)center_ty - 1, 1U, bs, &twist_slot);
        const int16_t *a1 = ring[(uint32_t)(center_lin + 10) % 5U] + ZF_LIVE_ROW_PAD;
        const int16_t *a2 = live_fetch_torus_row(
            ring[(uint32_t)(center_lin + 1 + 10) % 5U] + ZF_LIVE_ROW_PAD,
            (int32_t)center_ty + 1, 1U, bs, &twist_slot);
        if (bs->s16_blur3_safe) {
            zf_grain_blur3_s16_row_fast(a0, a1, a2, dst_row, Z8_ZF_LIVE_TILE_W);
        } else {
            zf_grain_blur3_s16_row(a0, a1, a2, dst_row, Z8_ZF_LIVE_TILE_W);
        }
    } else {
        const int16_t *a0 = live_fetch_torus_row(
            ring[(uint32_t)(center_lin - 2 + 10) % 5U] + ZF_LIVE_ROW_PAD,
            (int32_t)center_ty - 2, 2U, bs, &twist_slot);
        const int16_t *a1 = live_fetch_torus_row(
            ring[(uint32_t)(center_lin - 1 + 10) % 5U] + ZF_LIVE_ROW_PAD,
            (int32_t)center_ty - 1, 2U, bs, &twist_slot);
        const int16_t *a2 = ring[(uint32_t)(center_lin + 10) % 5U] + ZF_LIVE_ROW_PAD;
        const int16_t *a3 = live_fetch_torus_row(
            ring[(uint32_t)(center_lin + 1 + 10) % 5U] + ZF_LIVE_ROW_PAD,
            (int32_t)center_ty + 1, 2U, bs, &twist_slot);
        const int16_t *a4 = live_fetch_torus_row(
            ring[(uint32_t)(center_lin + 2 + 10) % 5U] + ZF_LIVE_ROW_PAD,
            (int32_t)center_ty + 2, 2U, bs, &twist_slot);
        if (bs->s16_blur5_safe) {
            zf_grain_blur5_s16_row_fast(a0, a1, a2, a3, a4, dst_row, Z8_ZF_LIVE_TILE_W);
        } else {
            zf_grain_blur5_s16_row(a0, a1, a2, a3, a4, dst_row, Z8_ZF_LIVE_TILE_W);
        }
    }
}

static void live_store_clamped_s8(const int16_t *src, int8_t *dst)
{
    for (uint32_t x = 0U; x < Z8_ZF_LIVE_TILE_W; ++x) {
        int32_t v = src[x];
        if (v < -128) v = -128;
        if (v > 127) v = 127;
        dst[x] = (int8_t)v;
    }
}

int z8_zf_grain_live_bake_tile(const z8_zf_grain_preset *preset,
                               const z8_zf_grain_maps *maps,
                               uint64_t seed,
                               int8_t *tile_out, size_t tile_bytes,
                               void *workspace, size_t workspace_bytes)
{
    if (!preset) return Z8_ZF_GRAIN_E_ARGUMENT;

    z8_zf_grain_dsp_params dsp;
    int rc = z8_zf_grain_resolve_preset(preset, &dsp);
    if (rc != Z8_ZF_GRAIN_OK) return rc;
    if (dsp.bypass) return Z8_ZF_GRAIN_DISABLED;

    if (!maps) return Z8_ZF_GRAIN_E_MAPS;
    const int16_t *map_a = dsp.map_select ? maps->map1 : maps->map0;
    MemRange map_range;
    if (!map_a || (((uintptr_t)map_a & 1U) != 0U) ||
        !make_range(map_a, Z8_ZF_GRAIN_MAP_BYTES, &map_range)) {
        return Z8_ZF_GRAIN_E_MAPS;
    }

    if (!tile_out) return Z8_ZF_GRAIN_E_ARGUMENT;
    MemRange tile_range, decl;
    if (tile_bytes < Z8_ZF_LIVE_TILE_BYTES ||
        !make_range(tile_out, tile_bytes, &decl) ||
        !make_range(tile_out, Z8_ZF_LIVE_TILE_BYTES, &tile_range)) {
        return Z8_ZF_GRAIN_E_CAPACITY;
    }

    MemRange work_range;
    if (!workspace || (((uintptr_t)workspace & (Z8_ZF_GRAIN_WORKSPACE_ALIGNMENT - 1U)) != 0U) ||
        workspace_bytes < Z8_ZF_GRAIN_WORKSPACE_BYTES ||
        !make_range(workspace, workspace_bytes, &work_range)) {
        return Z8_ZF_GRAIN_E_WORKSPACE;
    }
    work_range.end = work_range.first + Z8_ZF_GRAIN_WORKSPACE_BYTES;

    if (ranges_overlap(tile_range, work_range) ||
        ranges_overlap(tile_range, map_range)  ||
        ranges_overlap(work_range, map_range)) {
        return Z8_ZF_GRAIN_E_ALIAS;
    }

    ZfLiveBakeScratch *bs = (ZfLiveBakeScratch *)workspace;

    /* 1. Scale 83x83 grain map and compute fast-path overflow guards. */
    zf_grain_scale_map_s16(map_a, Z8_ZF_GRAIN_MAP_ELEMENTS, dsp.blend_w, bs->grain_map);
    int32_t max_abs = scan_map_max_abs(bs->grain_map);
    bs->s16_blur3_safe = (max_abs <= 2047) ? 1U : 0U;
    bs->s16_blur5_safe = (max_abs <= 4095) ? 1U : 0U;

    /* 2. Staggered coordinate LUT tiled to 512. */
    for (uint32_t k = 0U; k < ZF_GRAIN_IDX_PERIOD; ++k) {
        bs->idx_lut[k] = (uint8_t)(((k / Z8_ZF_GRAIN_TILE_PERIOD) * Z8_ZF_GRAIN_TILE_STEP +
                                    (k % Z8_ZF_GRAIN_TILE_PERIOD)) % Z8_ZF_GRAIN_MAP_DIM);
    }
    for (uint32_t k = ZF_GRAIN_IDX_PERIOD; k < Z8_ZF_LIVE_TILE_W; ++k) {
        bs->idx_lut[k] = bs->idx_lut[k - ZF_GRAIN_IDX_PERIOD];
    }

    /* 3. Per-row xorshift32 start states for all 256 tile rows (16 blocks/row). */
    uint32_t st = z8_zf_grain_fold_seed(seed);
    for (uint32_t y = 0U; y < Z8_ZF_LIVE_TILE_H; ++y) {
        bs->row_seed[y] = st;
        for (uint32_t blk = 0U; blk < (Z8_ZF_LIVE_TILE_W >> 5); ++blk) {
            st = xorshift32_step(st);
        }
    }

    uint32_t r1 = 0U, r2 = 0U;
    blur_radii_from_size(dsp.blur_size, &r1, &r2);
    int16_t *out_row = bs->out_row + ZF_LIVE_ROW_PAD;

    if (r1 == 0U && r2 == 0U) {
        for (uint32_t oy = 0U; oy < Z8_ZF_LIVE_TILE_H; ++oy) {
            live_gen_raw_row(bs, oy, out_row, 0U);
            live_store_clamped_s8(out_row, tile_out + (size_t)oy * Z8_ZF_LIVE_TILE_W);
        }
        return Z8_ZF_GRAIN_OK;
    }

    if (r1 == 0U) {
        int32_t raw_next = -(int32_t)r2;
        for (uint32_t oy = 0U; oy < Z8_ZF_LIVE_TILE_H; ++oy) {
            while (raw_next <= (int32_t)oy + (int32_t)r2) {
                uint32_t ty = (uint32_t)raw_next & (Z8_ZF_LIVE_TILE_H - 1U);
                int16_t *dst = bs->raw_ring[(uint32_t)(raw_next + 10) % 5U] + ZF_LIVE_ROW_PAD;
                live_gen_raw_row(bs, ty, dst, r2);
                raw_next++;
            }
            live_blur_torus_row(bs, bs->raw_ring, (int32_t)oy, oy, r2, out_row);
            live_store_clamped_s8(out_row, tile_out + (size_t)oy * Z8_ZF_LIVE_TILE_W);
        }
        return Z8_ZF_GRAIN_OK;
    }

    /* Two-stage twisted-torus cascade (blur7: 5x5->3x3, blur9: 5x5->5x5). */
    int32_t raw_next = -(int32_t)(r1 + r2);
    int32_t mid_next = -(int32_t)r2;

    for (uint32_t oy = 0U; oy < Z8_ZF_LIVE_TILE_H; ++oy) {
        while (mid_next <= (int32_t)oy + (int32_t)r2) {
            while (raw_next <= mid_next + (int32_t)r1) {
                uint32_t rty = (uint32_t)raw_next & (Z8_ZF_LIVE_TILE_H - 1U);
                int16_t *rdst = bs->raw_ring[(uint32_t)(raw_next + 10) % 5U] + ZF_LIVE_ROW_PAD;
                live_gen_raw_row(bs, rty, rdst, r1);
                raw_next++;
            }
            uint32_t mty = (uint32_t)mid_next & (Z8_ZF_LIVE_TILE_H - 1U);
            int16_t *mdst = bs->mid_ring[(uint32_t)(mid_next + 10) % 5U] + ZF_LIVE_ROW_PAD;
            live_blur_torus_row(bs, bs->raw_ring, mid_next, mty, r1, mdst);
            live_pad_periodic_s16(mdst, r2);
            mid_next++;
        }
        live_blur_torus_row(bs, bs->mid_ring, (int32_t)oy, oy, r2, out_row);
        live_store_clamped_s8(out_row, tile_out + (size_t)oy * Z8_ZF_LIVE_TILE_W);
    }

    return Z8_ZF_GRAIN_OK;
}
