/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Core Hardening, Deduplicated .cube LUT Engine & XM6 Upgrade Host Test Suite.
 *
 * Compiled on Mac with:
 *   clang -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined
 *         -DFILMSIM_HOST_TEST=1 -DV71R7_HOST_TEST=1 -pthread
 *
 * Covers:
 *   (a) cube.c: 1D separable (incl. channel permutation) vs 3D cross-channel parsing,
 *       DOMAIN_MIN/MAX, LUT_3D_INPUT_RANGE, scientific notation, BOM, malformed reject,
 *       fx_z8_lut_path() validation.
 *   (b) cube_frame NV16 & layer0_color NV12: NEON == Scalar 0-diff across identity/1D/3D LUTs,
 *       unaligned & non-multiple-of-8 widths, padded strides, 4 MiB tagged cache hit/miss/
 *       16-quadrant tag-collision invariance & generation invalidation.
 *   (c) photo_flight: 64-entry lock-free ring wrap-around (100 events -> newest 64 exact),
 *       heartbeat enter/progress/idle, 2s stall watchdog -> FXCRASH.TMP -> FXCRASH.OLD ->
 *       FXCRASH.LOG rotation on Drive A and Drive B.
 *   (d) smp: real pthread dual-core stripe partitioning (every row exact once, 0-diff),
 *       FxZ8LutIoGate JPEG-preempts-Rear priority, timeout quarantine (FX_Z8_FLIGHT_SMP_QUARANTINE).
 *   (e) neon_sentinel: real ARM64 d8..d15 + v0..v31 canary pass + injected fault latches
 *       V71R7_ANCHOR_FLAG_NEON_DISABLED + logs FX_Z8_FLIGHT_NEON_SENTINEL_FAIL -> automatic
 *       scalar fallback with 0-diff.
 *   (f) XM6 Hardening #1: 0xA02E7EED0[0..3] mailbox idle-word check, 512 MiB spacer retry
 *       allocator (z8_fs_plat_alloc_class24_xm6_ops), and stock Core 0 Opcode 12 (RGBLUT)
 *       z8_xm6_rgblut_nv16() serialized under b->xm6_busy + end-to-end 1D .cube dispatch.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "v71r7_lut_abi.h"
#include "cube.h"
#include "cube_frame.h"
#include "layer0_color.h"
#include "neon_sentinel.h"
#include "photo_flight.h"
#include "smp.h"
#include "lut_runtime.h"
#include "z8_xm6_bridge.h"
#include "z8_native_photo_platform.h"

/* Host-emulated 16 KiB fixed DRAM region (0xA11640000..0xA11644000) */
_Alignas(64) uint8_t v71r7_host_dram_region[0x4000];

/* Stubs required only because z8_xm6_bridge.c references them inside z8_xm6_run_diag */
#include "../filmsim/src/pipeline/z8_filmsim_stages.h"
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
    (void)zf_intensity; (void)zf_size; (void)zf_blur_mode; (void)is_full_res;
    (void)map0; (void)map1; (void)prng_seed; (void)y; (void)y_stride;
    (void)width; (void)height; (void)workspace; (void)workspace_bytes;
    return Z8_FS_INT_OK;
}

int z8_fs_stage_hdf_run(uint32_t hdf_level,
                        uint64_t shot_id,
                        uint32_t count,
                        uint32_t analysis_index,
                        const z8_fs_stage_plane *planes,
                        void *workspace, size_t workspace_bytes,
                        int (*cancelled)(void *), void *cancel_ctx)
{
    (void)hdf_level; (void)shot_id; (void)count; (void)analysis_index;
    (void)planes; (void)workspace; (void)workspace_bytes;
    (void)cancelled; (void)cancel_ctx;
    return Z8_FS_INT_OK;
}

/* ============================================================================
 * Synthetic .cube Generators
 * ============================================================================ */

static uint8_t *gen_cube_identity(unsigned grid, uint32_t *out_len)
{
    size_t cap = (size_t)grid * grid * grid * 40u + 512u;
    char *buf = (char *)malloc(cap);
    assert(buf != NULL);
    int n = snprintf(buf, cap, "TITLE \"Identity %u\"\nLUT_3D_SIZE %u\n", grid, grid);
    assert(n > 0);
    size_t pos = (size_t)n;
    double inv = 1.0 / (double)(grid - 1u);
    for (unsigned b = 0; b < grid; ++b) {
        for (unsigned g = 0; g < grid; ++g) {
            for (unsigned r = 0; r < grid; ++r) {
                int k = snprintf(buf + pos, cap - pos, "%.6f %.6f %.6f\n",
                                 r * inv, g * inv, b * inv);
                assert(k > 0);
                pos += (size_t)k;
            }
        }
    }
    *out_len = (uint32_t)pos;
    return (uint8_t *)buf;
}

/* 1D separable LUT with nonlinear S-curve + optional R<->B channel swap */
static uint8_t *gen_cube_1d_separable(unsigned grid, int swap_rb, uint32_t *out_len)
{
    size_t cap = (size_t)grid * grid * grid * 40u + 512u;
    char *buf = (char *)malloc(cap);
    assert(buf != NULL);
    int n = snprintf(buf, cap,
                     "#LUMIXPHOTOSTYLE STD\n"
                     "TITLE \"1D Separable %u\"\n"
                     "DOMAIN_MIN 0.0 0.0 0.0\n"
                     "DOMAIN_MAX 1.0 1.0 1.0\n"
                     "LUT_3D_SIZE %u\n", grid, grid);
    assert(n > 0);
    size_t pos = (size_t)n;
    double inv = 1.0 / (double)(grid - 1u);
    for (unsigned b = 0; b < grid; ++b) {
        for (unsigned g = 0; g < grid; ++g) {
            for (unsigned r = 0; r < grid; ++r) {
                double rf = r * inv, gf = g * inv, bf = b * inv;
                /* Smooth separable curves */
                double or_v = 0.04 + 0.92 * (rf * rf * (3.0 - 2.0 * rf));
                double og_v = 0.02 + 0.96 * (0.85 * gf + 0.15 * gf * gf);
                double ob_v = 0.06 + 0.88 * (0.5 * bf + 0.5 * bf * bf);
                if (swap_rb) {
                    /* Output R depends only on input B; output B depends only on input R */
                    double tmp_r = 0.04 + 0.92 * (bf * bf * (3.0 - 2.0 * bf));
                    double tmp_b = 0.06 + 0.88 * (0.5 * rf + 0.5 * rf * rf);
                    or_v = tmp_r;
                    ob_v = tmp_b;
                }
                int k = snprintf(buf + pos, cap - pos, "%.6f %.6f %.6f\n", or_v, og_v, ob_v);
                assert(k > 0);
                pos += (size_t)k;
            }
        }
    }
    *out_len = (uint32_t)pos;
    return (uint8_t *)buf;
}

/* Cross-channel 3D LUT (Teal/Orange + nonlinear cross-talk) */
static uint8_t *gen_cube_3d_cross(unsigned grid, uint32_t *out_len)
{
    size_t cap = (size_t)grid * grid * grid * 40u + 512u;
    char *buf = (char *)malloc(cap);
    assert(buf != NULL);
    int n = snprintf(buf, cap,
                     "\xEF\xBB\xBF" /* UTF-8 BOM test */
                     "# Cross-channel 3D cinematic look\n"
                     "TITLE \"3D Cross %u\"\n"
                     "LUT_3D_SIZE %u\n", grid, grid);
    assert(n > 0);
    size_t pos = (size_t)n;
    double inv = 1.0 / (double)(grid - 1u);
    for (unsigned b = 0; b < grid; ++b) {
        for (unsigned g = 0; g < grid; ++g) {
            for (unsigned r = 0; r < grid; ++r) {
                double rf = r * inv, gf = g * inv, bf = b * inv;
                double luma = 0.299 * rf + 0.587 * gf + 0.114 * bf;
                double or_v = 0.82 * rf + 0.22 * gf - 0.06 * bf + 0.08 * luma * (1.0 - luma);
                double og_v = 0.08 * rf + 0.84 * gf + 0.09 * bf;
                double ob_v = -0.05 * rf + 0.14 * gf + 0.86 * bf + 0.06 * (1.0 - luma);
                if (or_v < 0.0) or_v = 0.0; else if (or_v > 1.0) or_v = 1.0;
                if (og_v < 0.0) og_v = 0.0; else if (og_v > 1.0) og_v = 1.0;
                if (ob_v < 0.0) ob_v = 0.0; else if (ob_v > 1.0) ob_v = 1.0;
                int k = snprintf(buf + pos, cap - pos, "%.6f %.6f %.6f\n", or_v, og_v, ob_v);
                assert(k > 0);
                pos += (size_t)k;
            }
        }
    }
    *out_len = (uint32_t)pos;
    return (uint8_t *)buf;
}

static void fill_scene_nv16(uint8_t *y, uint8_t *uv, uint32_t w, uint32_t h,
                            uint32_t y_stride, uint32_t uv_stride, int collide_tags)
{
    for (uint32_t r = 0; r < h; ++r) {
        for (uint32_t x = 0; x < w; ++x) {
            if (collide_tags) {
                /* Repeat luma across pixels so different chroma quadrants collide on exact same cache slot! */
                y[(size_t)r * y_stride + x] = (uint8_t)((r * 13u + (x & 1u) * 64u) & 255u);
            } else {
                y[(size_t)r * y_stride + x] = (uint8_t)((x * 7u + r * 13u + (x >> 3) * 29u) & 255u);
            }
        }
        for (uint32_t x = 0; x < w; x += 2u) {
            uint8_t cb, cr;
            if (collide_tags) {
                /* Force identical low-6-bit chroma index across all 16 upper-bit tags! */
                unsigned q = ((x >> 1) + r) & 15u;
                unsigned low_cb = (r * 3u) & 63u;
                unsigned low_cr = (r * 5u + 7u) & 63u;
                cb = (uint8_t)(((q & 3u) << 6) | low_cb);
                cr = (uint8_t)(((q >> 2) << 6) | low_cr);
            } else {
                cb = (uint8_t)((40u + x * 3u + r * 5u) & 255u);
                cr = (uint8_t)((60u + x * 5u + r * 3u) & 255u);
            }
            uv[(size_t)r * uv_stride + x]      = cb;
            uv[(size_t)r * uv_stride + x + 1u] = cr;
        }
    }
}

static void fill_scene_nv12(uint8_t *y, uint8_t *uv, uint32_t w, uint32_t h,
                            uint32_t y_stride, uint32_t uv_stride, int collide_tags)
{
    for (uint32_t r = 0; r < h; ++r) {
        for (uint32_t x = 0; x < w; ++x) {
            if (collide_tags) {
                y[(size_t)r * y_stride + x] = (uint8_t)(((r >> 1) * 19u + (x & 1u) * 80u + (r & 1u) * 32u) & 255u);
            } else {
                y[(size_t)r * y_stride + x] = (uint8_t)((x * 11u + r * 17u + ((x ^ r) & 31u)) & 255u);
            }
        }
    }
    for (uint32_t r = 0; r < h / 2u; ++r) {
        for (uint32_t x = 0; x < w; x += 2u) {
            uint8_t cb, cr;
            if (collide_tags) {
                unsigned q = ((x >> 1) + r) & 15u;
                cb = (uint8_t)(((q & 3u) << 6) | ((r * 7u) & 63u));
                cr = (uint8_t)(((q >> 2) << 6) | ((r * 11u + 3u) & 63u));
            } else {
                cb = (uint8_t)((32u + x * 3u + r * 9u) & 255u);
                cr = (uint8_t)((48u + x * 7u + r * 5u) & 255u);
            }
            uv[(size_t)r * uv_stride + x]      = cb;
            uv[(size_t)r * uv_stride + x + 1u] = cr;
        }
    }
}

/* ============================================================================
 * Test (a): cube.c parsing, 1D separable vs 3D cross-channel, domain, reject
 * ============================================================================ */
static void test_a_cube_parser(void)
{
    printf("[Test (a)] cube.c 1D separable vs 3D cross-channel, domain scaling, malformed reject...\n");

    /* Verify fx_z8_lut_path() fix from v71r7_lut_abi.h */
    assert(fx_z8_lut_path("LOOK.CUBE") == 1);
    assert(fx_z8_lut_path("LUTS\\Film\\Portra_400.cube") == 1);
    assert(fx_z8_lut_path("luts\\BW\\Tri-X.CUBE") == 1);
    assert(fx_z8_lut_path("Film\\Portra.cube") == 0);
    assert(fx_z8_lut_path("LUTS\\Portra.cube") == 0);
    assert(fx_z8_lut_path("") == 0);
    assert(fx_z8_lut_path("L") == 0);
    assert(fx_z8_lut_path(NULL) == 0);

    /* 1. Identity 17^3 -> 1D separable with source={0,1,2} */
    uint32_t id_len = 0;
    uint8_t *id_buf = gen_cube_identity(17u, &id_len);
    FxZ8LutProgress prog;
    memset(&prog, 0, sizeof(prog));
    assert(v71r7_lut_prepare_from_buffer(id_buf, id_len, "ID.CUBE", 1u, &prog) == 1);
    assert(prog.phase == FX_Z8_LUT_READY && prog.generation == 1u);
    uint32_t gen = 0;
    int is_1d = 0;
    assert(v71r7_lut_is_active(&gen, &is_1d) == 1);
    assert(gen == 1u && is_1d == 1);

    uint16_t curves[3][256];
    uint8_t source[3];
    assert(v71r7_lut_get_active_curves(curves, source, &gen) == 1);
    assert(source[0] == 0u && source[1] == 1u && source[2] == 2u);
    assert(curves[0][0] == 0u && curves[0][64] == 16384u && curves[0][128] >= 32600u);
    free(id_buf);

    /* 2. 1D separable with R<->B channel permutation (grid=17) */
    uint32_t sep_len = 0;
    uint8_t *sep_buf = gen_cube_1d_separable(17u, 1, &sep_len);
    assert(v71r7_lut_prepare_from_buffer(sep_buf, sep_len, "LUTS\\Curves\\SwapRB.CUBE", 2u, &prog) == 1);
    assert(v71r7_lut_is_active(&gen, &is_1d) == 1);
    assert(gen == 2u && is_1d == 1);
    assert(v71r7_lut_get_active_curves(curves, source, &gen) == 1);
    assert(source[0] == 2u && source[1] == 1u && source[2] == 0u);
    free(sep_buf);

    /* 3. Cross-channel 3D 33^3 LUT -> is_1d_separable MUST be 0 */
    uint32_t c3d_len = 0;
    uint8_t *c3d_buf = gen_cube_3d_cross(33u, &c3d_len);
    assert(v71r7_lut_prepare_from_buffer(c3d_buf, c3d_len, "LUTS\\Cinema\\TealOrange33.cube", 3u, &prog) == 1);
    assert(v71r7_lut_is_active(&gen, &is_1d) == 1);
    assert(gen == 3u && is_1d == 0);
    assert(v71r7_lut_get_active_curves(curves, source, &gen) == 0);
    free(c3d_buf);

    /* 4. DOMAIN_MIN / DOMAIN_MAX + scientific notation exact check */
    static const char k_dom_cube[] =
        "TITLE \"Domain Test\"\n"
        "DOMAIN_MIN 2.5e-1 2.5e-1 2.5e-1\n"
        "DOMAIN_MAX 7.5e-1 7.5e-1 7.5e-1\n"
        "LUT_3D_SIZE 2\n"
        "0.0 0.0 0.0\n"
        "1.0 0.0 0.0\n"
        "0.0 1.0 0.0\n"
        "1.0 1.0 0.0\n"
        "0.0 0.0 1.0\n"
        "1.0 0.0 1.0\n"
        "0.0 1.0 1.0\n"
        "1.0 1.0 1.0\n";
    JpegCube *dom = (JpegCube *)calloc(1, sizeof(JpegCube));
    assert(dom != NULL);
    jpeg_cube_init(dom);
    const char *p = k_dom_cube;
    while (*p) {
        char line[CUBE_LINE];
        size_t l = 0;
        while (*p && *p != '\n' && l < sizeof(line) - 1) line[l++] = *p++;
        if (*p == '\n') ++p;
        line[l] = '\0';
        assert(jpeg_cube_line(dom, line) == 1);
    }
    assert(jpeg_cube_finish(dom) == 1);
    uint8_t rgb[3];
    /* Inputs <= 0.25 (64) clamp to 0; inputs >= 0.75 (191) clamp to 255; midpoint 128 -> ~128 */
    jpeg_cube_rgb(dom, 50, 128, 210, rgb);
    assert(rgb[0] == 0u);
    assert(rgb[1] >= 126u && rgb[1] <= 130u);
    assert(rgb[2] == 255u);
    free(dom);

    /* 5. Malformed .cube rejection (must NOT clobber currently active LUT gen 3!) */
    static const char *const k_bad_cubes[] = {
        "",                                              /* empty */
        "LUT_3D_SIZE 1\n0 0 0\n",                        /* grid < 2 */
        "LUT_3D_SIZE 34\n0 0 0\n",                       /* grid > 33 */
        "LUT_3D_SIZE 2\n0 0 0\n1 1 1\n",                 /* truncated count */
        "LUT_3D_SIZE 2\nLUT_3D_SIZE 2\n",                /* duplicate size */
        ("DOMAIN_MIN 0.5 0.5 0.5\nDOMAIN_MAX 0.5 0.5 0.5\nLUT_3D_SIZE 2\n"
         "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n"), /* max <= min */
        "LUT_3D_SIZE 2\n0 0 0 garbage\n",                /* trailing garbage */
        "0.0 0.0 0.0\n"                                  /* missing LUT_3D_SIZE */
    };
    for (size_t i = 0; i < sizeof(k_bad_cubes) / sizeof(k_bad_cubes[0]); ++i) {
        assert(v71r7_lut_prepare_from_buffer((const uint8_t *)k_bad_cubes[i],
                                             (uint32_t)strlen(k_bad_cubes[i]),
                                             "BAD.CUBE", 99u, &prog) == 0);
        assert(prog.phase == FX_Z8_LUT_ERROR);
    }
    /* Oversize (> 2 MiB) reject */
    uint8_t dummy_byte = ' ';
    assert(v71r7_lut_prepare_from_buffer(&dummy_byte, CUBE_MAX_BYTES + 1u, "BIG.CUBE", 100u, &prog) == 0);
    /* Embedded NUL reject */
    const uint8_t nul_cube[16] = "LUT_3D_SIZE 2\0\n";
    assert(v71r7_lut_prepare_from_buffer(nul_cube, sizeof(nul_cube), "NUL.CUBE", 101u, &prog) == 0);

    /* Active LUT gen 3 must remain completely untouched after rejected parses! */
    assert(v71r7_lut_is_active(&gen, &is_1d) == 1 && gen == 3u && is_1d == 0);

    /* 6. V71.7-r6 Streaming parser: split UTF-8 BOM (1-byte chunks) + 16 KiB chunked 33^3 .cube + abort/error safety */
    uint32_t s33_len = 0u;
    uint8_t *s33_buf = gen_cube_3d_cross(33u, &s33_len);
    assert(s33_buf[0] == 0xEFu && s33_buf[1] == 0xBBu && s33_buf[2] == 0xBFu);
    void *sh = v71r7_lut_stream_begin(4u, &prog);
    assert(sh != NULL);
    uint32_t off = 0u;
    while (off < 16u && off < s33_len) {
        assert(v71r7_lut_stream_feed(sh, s33_buf + off, 1u, &prog) == 1);
        ++off;
    }
    while (off < s33_len) {
        uint32_t take = (s33_len - off) > 16384u ? 16384u : (s33_len - off);
        assert(v71r7_lut_stream_feed(sh, s33_buf + off, take, &prog) == 1);
        off += take;
    }
    assert(v71r7_lut_stream_commit(sh, "LUTS\\Fuji_DCP\\stream33.cube", 4u, &prog) == 1);
    assert(prog.phase == FX_Z8_LUT_READY && prog.generation == 4u && prog.file_bytes == s33_len);
    assert(v71r7_lut_is_active(&gen, &is_1d) == 1 && gen == 4u && is_1d == 0);
    free(s33_buf);

    /* Stream 1D identity in 7-byte prime chunks (splits keywords/numbers across boundaries) */
    uint32_t s1d_len = 0u;
    uint8_t *s1d_buf = gen_cube_identity(17u, &s1d_len);
    sh = v71r7_lut_stream_begin(5u, &prog);
    assert(sh != NULL);
    for (off = 0u; off < s1d_len; ) {
        uint32_t take = (s1d_len - off) > 7u ? 7u : (s1d_len - off);
        assert(v71r7_lut_stream_feed(sh, s1d_buf + off, take, &prog) == 1);
        off += take;
    }
    assert(v71r7_lut_stream_commit(sh, "STREAM_ID.CUBE", 5u, &prog) == 1);
    assert(v71r7_lut_is_active(&gen, &is_1d) == 1 && gen == 5u && is_1d == 1);
    assert(v71r7_lut_get_active_curves(curves, source, &gen) == 1 && curves[0][64] == 16384u);
    free(s1d_buf);

    /* Explicit abort + mid-stream malformed line reject must not leak or clobber gen 5 */
    sh = v71r7_lut_stream_begin(6u, &prog);
    assert(sh != NULL);
    assert(v71r7_lut_stream_feed(sh, (const uint8_t *)"LUT_3D_SIZE 2\n", 14u, &prog) == 1);
    v71r7_lut_stream_abort(sh);

    sh = v71r7_lut_stream_begin(7u, &prog);
    assert(sh != NULL);
    assert(v71r7_lut_stream_feed(sh, (const uint8_t *)"LUT_3D_SIZE 2\n0 0 0 bad\n", 24u, &prog) == 0);
    assert(v71r7_lut_stream_commit(sh, "BAD_STREAM.CUBE", 7u, &prog) == 0);
    assert(v71r7_lut_is_active(&gen, &is_1d) == 1 && gen == 5u && is_1d == 1);

    v71r7_lut_clear_active();
    assert(v71r7_lut_is_active(&gen, &is_1d) == 0 && gen == 0u);
    printf("  PASS (a)\n");
}

/* ============================================================================
 * Test (b): NV16 & NV12 NEON == Scalar 0-diff + 4 MiB cache invariance
 * ============================================================================ */
static void test_b_neon_vs_scalar_and_cache(void)
{
    printf("[Test (b)] cube_frame NV16 & layer0_color NV12 NEON==Scalar 0-diff + 4MiB cache invariance...\n");

    uint32_t *cache_scalar = (uint32_t *)calloc(CUBE_FRAME_CACHE_WORDS, sizeof(uint32_t));
    uint32_t *cache_neon   = (uint32_t *)calloc(CUBE_FRAME_CACHE_WORDS, sizeof(uint32_t));
    uint32_t *cache_warm   = (uint32_t *)calloc(CUBE_FRAME_CACHE_WORDS, sizeof(uint32_t));
    assert(cache_scalar && cache_neon && cache_warm);

    uint32_t lens[3] = {0};
    uint8_t *bufs[3];
    bufs[0] = gen_cube_identity(9u, &lens[0]);
    bufs[1] = gen_cube_1d_separable(17u, 1, &lens[1]);
    bufs[2] = gen_cube_3d_cross(33u, &lens[2]);

    JpegCube *cube = (JpegCube *)calloc(1, sizeof(JpegCube));
    assert(cube != NULL);

    const uint32_t widths[] = { 14u, 66u, 128u, 320u };
    const uint32_t height = 64u;

    for (unsigned lut_i = 0; lut_i < 3u; ++lut_i) {
        /* Parse into standalone JpegCube */
        jpeg_cube_init(cube);
        uint32_t pos = (lens[lut_i] >= 3u && bufs[lut_i][0] == 0xEFu) ? 3u : 0u;
        char line[CUBE_LINE];
        uint32_t ll = 0;
        while (pos < lens[lut_i]) {
            char ch = (char)bufs[lut_i][pos++];
            if (ch == '\n') {
                line[ll] = '\0';
                assert(jpeg_cube_line(cube, line) == 1);
                ll = 0;
            } else {
                line[ll++] = ch;
            }
        }
        if (ll) {
            line[ll] = '\0';
            assert(jpeg_cube_line(cube, line) == 1);
        }
        assert(jpeg_cube_finish(cube) == 1);

        /* Pre-warm cache_warm for central chroma */
        memset(cache_warm, 0, CUBE_FRAME_CACHE_BYTES);
        jpeg_cube_cache_warm(cube, cache_warm, 0u, CUBE_FRAME_CACHE_WORDS);

        for (unsigned wi = 0; wi < sizeof(widths) / sizeof(widths[0]); ++wi) {
            uint32_t w = widths[wi];
            uint32_t stride = w + 16u; /* padded stride */

            for (int collide = 0; collide <= 1; ++collide) {
                /* --- NV16 test (including unaligned +1 byte offset on wi==0) --- */
                size_t pad = (wi == 0u) ? 1u : 0u;
                size_t plane_sz = (size_t)stride * height + 16u;
                uint8_t *sy  = (uint8_t *)malloc(plane_sz);
                uint8_t *sc  = (uint8_t *)malloc(plane_sz);
                uint8_t *dy_s = (uint8_t *)malloc(plane_sz);
                uint8_t *dc_s = (uint8_t *)malloc(plane_sz);
                uint8_t *dy_n = (uint8_t *)malloc(plane_sz);
                uint8_t *dc_n = (uint8_t *)malloc(plane_sz);
                uint8_t *dy_w = (uint8_t *)malloc(plane_sz);
                uint8_t *dc_w = (uint8_t *)malloc(plane_sz);
                assert(sy && sc && dy_s && dc_s && dy_n && dc_n && dy_w && dc_w);

                memset(sy, 0xCC, plane_sz);
                memset(sc, 0xCC, plane_sz);
                fill_scene_nv16(sy + pad, sc + pad, w, height, stride, stride, collide);

                memset(cache_scalar, 0, CUBE_FRAME_CACHE_BYTES);
                memset(cache_neon,   0, CUBE_FRAME_CACHE_BYTES);

                for (uint32_t r = 0; r < height; ++r) {
                    size_t off = pad + (size_t)r * stride;
                    (void)jpeg_cube_cache_row_scalar(cube, cache_scalar, sy + off, sc + off,
                                                     dy_s + off, dc_s + off, w);
                    (void)jpeg_cube_cache_row_neon(cube, cache_neon, sy + off, sc + off,
                                                   dy_n + off, dc_n + off, w);
                    (void)jpeg_cube_cache_row_neon(cube, cache_warm, sy + off, sc + off,
                                                   dy_w + off, dc_w + off, w);
                    assert(memcmp(dy_s + off, dy_n + off, w) == 0);
                    assert(memcmp(dc_s + off, dc_n + off, w) == 0);
                    assert(memcmp(dy_s + off, dy_w + off, w) == 0);
                    assert(memcmp(dc_s + off, dc_w + off, w) == 0);
                }

                /* Second hot pass on cache_neon must have 0 misses when !collide and 0-diff */
                uint32_t hot_misses = 0;
                for (uint32_t r = 0; r < height; ++r) {
                    size_t off = pad + (size_t)r * stride;
                    hot_misses += jpeg_cube_cache_row_neon(cube, cache_neon, sy + off, sc + off,
                                                           dy_n + off, dc_n + off, w);
                    assert(memcmp(dy_s + off, dy_n + off, w) == 0);
                    assert(memcmp(dc_s + off, dc_n + off, w) == 0);
                }
                if (collide) {
                    /* Tag collisions must still be 100% invariant with 0-diff */
                    assert(hot_misses > 0u);
                }

                free(sy); free(sc); free(dy_s); free(dc_s);
                free(dy_n); free(dc_n); free(dy_w); free(dc_w);

                /* --- NV12 test (padded stride + tag collisions) --- */
                size_t y_sz  = (size_t)stride * height;
                size_t uv_sz = (size_t)stride * (height / 2u);
                uint8_t *ny  = (uint8_t *)malloc(y_sz);
                uint8_t *nuv = (uint8_t *)malloc(uv_sz);
                uint8_t *ny_s  = (uint8_t *)calloc(1, y_sz);
                uint8_t *nuv_s = (uint8_t *)calloc(1, uv_sz);
                uint8_t *ny_n  = (uint8_t *)calloc(1, y_sz);
                uint8_t *nuv_n = (uint8_t *)calloc(1, uv_sz);
                uint8_t *ny_w  = (uint8_t *)calloc(1, y_sz);
                uint8_t *nuv_w = (uint8_t *)calloc(1, uv_sz);
                assert(ny && nuv && ny_s && nuv_s && ny_n && nuv_n && ny_w && nuv_w);

                fill_scene_nv12(ny, nuv, w, height, stride, stride, collide);
                memset(cache_scalar, 0, CUBE_FRAME_CACHE_BYTES);
                memset(cache_neon,   0, CUBE_FRAME_CACHE_BYTES);

                Layer0Frame fs = { ny, nuv, ny_s, nuv_s, w, height, stride, stride, stride, stride };
                Layer0Frame fn = { ny, nuv, ny_n, nuv_n, w, height, stride, stride, stride, stride };
                Layer0Frame fw = { ny, nuv, ny_w, nuv_w, w, height, stride, stride, stride, stride };

                layer0_grade_nv12_planes(cube, cache_scalar, &fs, 0u, height);
                layer0_grade_nv12_planes_neon(cube, cache_neon, &fn, 0u, height);
                layer0_grade_nv12_planes_neon(cube, cache_warm, &fw, 0u, height);

                assert(memcmp(ny_s, ny_n, y_sz) == 0);
                assert(memcmp(nuv_s, nuv_n, uv_sz) == 0);
                assert(memcmp(ny_s, ny_w, y_sz) == 0);
                assert(memcmp(nuv_s, nuv_w, uv_sz) == 0);

                free(ny); free(nuv); free(ny_s); free(nuv_s);
                free(ny_n); free(nuv_n); free(ny_w); free(nuv_w);
            }
        }
    }

    /* Verify cache generation invalidation across LUT switches via lut_runtime API */
    uint32_t cache_gen = 0u;
    uint32_t w = 64u, h = 32u;
    uint8_t *y1 = (uint8_t *)malloc(w * h);
    uint8_t *uv1 = (uint8_t *)malloc(w * h);
    uint8_t *y2 = (uint8_t *)malloc(w * h);
    uint8_t *uv2 = (uint8_t *)malloc(w * h);
    fill_scene_nv16(y1, uv1, w, h, w, w, 0);
    memcpy(y2, y1, w * h);
    memcpy(uv2, uv1, w * h);

    assert(v71r7_lut_prepare_from_buffer(bufs[0], lens[0], "ID.CUBE", 10u, NULL) == 1);
    assert(v71r7_lut_apply_still_nv16(y1, uv1, w, h, w, w, cache_neon, &cache_gen) == 1);
    assert(cache_gen == 10u);

    /* Switch to 3D cross LUT gen 11 -> must auto-clear stale gen-10 cache! */
    assert(v71r7_lut_prepare_from_buffer(bufs[2], lens[2], "CROSS.CUBE", 11u, NULL) == 1);
    assert(v71r7_lut_apply_still_nv16(y2, uv2, w, h, w, w, cache_neon, &cache_gen) == 1);
    assert(cache_gen == 11u);

    /* Compare against completely fresh cache_scalar */
    uint8_t *y_ref = (uint8_t *)malloc(w * h);
    uint8_t *uv_ref = (uint8_t *)malloc(w * h);
    fill_scene_nv16(y_ref, uv_ref, w, h, w, w, 0);
    uint32_t fresh_gen = 0u;
    assert(v71r7_lut_apply_still_nv16(y_ref, uv_ref, w, h, w, w, cache_scalar, &fresh_gen) == 1);
    assert(memcmp(y2, y_ref, w * h) == 0);
    assert(memcmp(uv2, uv_ref, w * h) == 0);

    free(y1); free(uv1); free(y2); free(uv2); free(y_ref); free(uv_ref);
    free(cube);
    free(bufs[0]); free(bufs[1]); free(bufs[2]);
    free(cache_scalar); free(cache_neon); free(cache_warm);
    v71r7_lut_clear_active();
    printf("  PASS (b)\n");
}

/* ============================================================================
 * Test (c): photo_flight 64-entry wrap-around + 2s stall watchdog -> FXCRASH.LOG
 * ============================================================================ */
typedef struct {
    uint64_t ticks;
    uint16_t core;
    char     drive;
    char     root_dir[256];
    char     last_written_path[64];
} FlightMockCtx;

static void map_card_path(const FlightMockCtx *m, const char *card_path, char *out, size_t cap)
{
    /* Map e.g. "A:\FENESTRA\FXCRASH.LOG" -> "<root_dir>/A_FENESTRA_FXCRASH.LOG" */
    size_t n = (size_t)snprintf(out, cap, "%s/", m->root_dir);
    for (size_t i = 0; card_path[i] && n < cap - 1u; ++i) {
        char c = card_path[i];
        out[n++] = (c == '\\' || c == ':' || c == '/') ? '_' : c;
    }
    out[n] = '\0';
}

static uint64_t fmock_ticks(void *ctx) { return ((FlightMockCtx *)ctx)->ticks; }
static uint16_t fmock_core(void *ctx)  { return ((FlightMockCtx *)ctx)->core; }
static char     fmock_drive(void *ctx) { return ((FlightMockCtx *)ctx)->drive; }

static int32_t fmock_open(void *ctx, const char *path, uint32_t flags, uint32_t mode)
{
    (void)mode;
    FlightMockCtx *m = (FlightMockCtx *)ctx;
    char real[384];
    map_card_path(m, path, real, sizeof(real));
    int oflags = (flags == 0u) ? O_RDONLY : (O_WRONLY | O_CREAT | O_TRUNC);
    int fd = open(real, oflags, 0644);
    return (fd >= 0) ? (int32_t)fd : -1;
}

static uint32_t fmock_write(void *ctx, int32_t fd, const void *buf, uint32_t bytes, uint32_t *done)
{
    (void)ctx;
    ssize_t w = write((int)fd, buf, bytes);
    if (w < 0) {
        if (done) *done = 0u;
        return 1u;
    }
    if (done) *done = (uint32_t)w;
    return 0u;
}

static uint32_t fmock_close(void *ctx, int32_t fd)
{
    (void)ctx;
    return close((int)fd) == 0 ? 0u : 1u;
}

static uint32_t fmock_remove(void *ctx, const char *path)
{
    FlightMockCtx *m = (FlightMockCtx *)ctx;
    char real[384];
    map_card_path(m, path, real, sizeof(real));
    (void)unlink(real);
    return 0u;
}

static uint32_t fmock_rename(void *ctx, const char *from, const char *to)
{
    FlightMockCtx *m = (FlightMockCtx *)ctx;
    char rfrom[384], rto[384];
    map_card_path(m, from, rfrom, sizeof(rfrom));
    map_card_path(m, to, rto, sizeof(rto));
    snprintf(m->last_written_path, sizeof(m->last_written_path), "%s", to);
    return rename(rfrom, rto) == 0 ? 0u : 2u;
}

static void test_c_photo_flight_and_watchdog(void)
{
    printf("[Test (c)] photo_flight 64-entry wrap-around + 2s stall watchdog -> FXCRASH.LOG...\n");

    FlightMockCtx fm;
    memset(&fm, 0, sizeof(fm));
    fm.ticks = UINT64_C(1000) * FX_Z8_TICKS_PER_MS;
    fm.core  = 2u;
    fm.drive = 'A';
    snprintf(fm.root_dir, sizeof(fm.root_dir), "/tmp/v71r7_flight_test_%d", (int)getpid());
    assert(mkdir(fm.root_dir, 0755) == 0 || errno == EEXIST);

    V71r7FlightHostOps fops = {
        .read_ticks  = fmock_ticks,
        .read_core   = fmock_core,
        .active_drive= fmock_drive,
        .card_open   = fmock_open,
        .card_write  = fmock_write,
        .card_close  = fmock_close,
        .card_remove = fmock_remove,
        .card_rename = fmock_rename,
        .ctx         = &fm
    };
    v71r7_flight_init();
    v71r7_flight_set_host_ops(&fops);

    /* Open IO gate so crash_write exercises fx_z8_lut_io_take / drop */
    FxZ8LutIoGate *gate = v71r7_io_gate();
    __atomic_store_n(&gate->state, 0u, __ATOMIC_RELEASE);
    fx_z8_lut_io_reopen(gate, 0);

    /* 1. Log 100 events -> verify 64-entry ring wraps cleanly to seq 37..100 */
    for (uint32_t i = 1u; i <= 100u; ++i) {
        fm.ticks += FX_Z8_TICKS_PER_MS;
        v71r7_flight_record((uint16_t)(1u + (i % 24u)), (uint16_t)(1u + (i % 3u)),
                            i * 10u, i * 20u, i * 30u);
    }
    FxZ8FlightRecord rows[FX_Z8_FLIGHT_RING_COUNT];
    uint64_t first_seq = 0u;
    uint32_t cnt = v71r7_flight_snapshot(rows, FX_Z8_FLIGHT_RING_COUNT, &first_seq);
    assert(cnt == 64u);
    assert(first_seq == 37u);
    for (uint32_t i = 0u; i < 64u; ++i) {
        uint32_t expected_i = 37u + i;
        assert(rows[i].seq == expected_i);
        assert(rows[i].a == expected_i * 10u);
        assert(rows[i].b == expected_i * 20u);
        assert(rows[i].c == expected_i * 30u);
    }

    /* 2. Heartbeat active < 2s -> NO stall */
    v71r7_heartbeat_enter(FX_Z8_HB_JPEG_LANE0, FX_Z8_STAGE_JPEG_GRADE, 42u, 0x123u);
    fm.ticks += UINT64_C(1500) * FX_Z8_TICKS_PER_MS;
    assert(v71r7_watchdog_poll(fm.ticks) == 0u);
    assert(v71r7_flight_watchdog_ptr()->dumps_written == 0u);

    /* Progress resets the 2s timer */
    v71r7_heartbeat_progress(FX_Z8_HB_JPEG_LANE0);
    fm.ticks += UINT64_C(1999) * FX_Z8_TICKS_PER_MS;
    assert(v71r7_watchdog_poll(fm.ticks) == 0u);
    assert(v71r7_flight_watchdog_ptr()->dumps_written == 0u);

    /* 3. Cross 2.0s threshold -> triggers stall dump to A:\FENESTRA\FXCRASH.LOG */
    fm.ticks += UINT64_C(50) * FX_Z8_TICKS_PER_MS;
    uint32_t stalled_mask = v71r7_watchdog_poll(fm.ticks);
    assert(stalled_mask == (1u << FX_Z8_HB_JPEG_LANE0));
    assert(v71r7_flight_watchdog_ptr()->dumps_written == 1u);
    assert(v71r7_flight_watchdog_ptr()->last_err == 0u);
    assert(strcmp(fm.last_written_path, "A:\\FENESTRA\\FXCRASH.LOG") == 0);

    /* Read written FXCRASH.LOG and verify content */
    char log_path[384], old_path[384];
    map_card_path(&fm, "A:\\FENESTRA\\FXCRASH.LOG", log_path, sizeof(log_path));
    map_card_path(&fm, "A:\\FENESTRA\\FXCRASH.OLD", old_path, sizeof(old_path));
    FILE *fp = fopen(log_path, "rb");
    assert(fp != NULL);
    char text[4096];
    size_t got = fread(text, 1, sizeof(text) - 1, fp);
    fclose(fp);
    assert(got > 100u);
    text[got] = '\0';
    assert(strstr(text, "FXCRASH build=Z8-C311-V71.7 reason=STALL") != NULL);
    assert(strstr(text, "STALL") != NULL);

    /* Re-polling same stuck enter_tick does NOT spam duplicate dumps */
    fm.ticks += UINT64_C(500) * FX_Z8_TICKS_PER_MS;
    assert(v71r7_watchdog_poll(fm.ticks) == 0u);
    assert(v71r7_flight_watchdog_ptr()->dumps_written == 1u);

    /* 4. Second stall on another slot -> verifies .LOG -> .OLD rotation! */
    v71r7_heartbeat_idle(FX_Z8_HB_JPEG_LANE0);
    v71r7_heartbeat_enter(FX_Z8_HB_REAR_STEP, FX_Z8_STAGE_REAR_GRADE, 77u, 0x99u);
    fm.ticks += UINT64_C(2500) * FX_Z8_TICKS_PER_MS;
    stalled_mask = v71r7_watchdog_poll(fm.ticks);
    assert(stalled_mask == (1u << FX_Z8_HB_REAR_STEP));
    assert(v71r7_flight_watchdog_ptr()->dumps_written == 2u);
    /* Verify FXCRASH.OLD now exists alongside FXCRASH.LOG */
    FILE *fp_old = fopen(old_path, "rb");
    assert(fp_old != NULL);
    fclose(fp_old);

    /* 5. Verify Drive B routing when active card drive switches to 'B' */
    v71r7_heartbeat_idle(FX_Z8_HB_REAR_STEP);
    fm.drive = 'B';
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_CARD_IO, 88u, 1u);
    fm.ticks += UINT64_C(2100) * FX_Z8_TICKS_PER_MS;
    assert(v71r7_watchdog_poll(fm.ticks) == (1u << FX_Z8_HB_WORKER));
    assert(strcmp(fm.last_written_path, "B:\\FENESTRA\\FXCRASH.LOG") == 0);
    v71r7_heartbeat_idle(FX_Z8_HB_WORKER);

    /* Cleanup temp files */
    char b_log[384], a_tmp[384], b_tmp[384];
    map_card_path(&fm, "B:\\FENESTRA\\FXCRASH.LOG", b_log, sizeof(b_log));
    map_card_path(&fm, "A:\\FENESTRA\\FXCRASH.TMP", a_tmp, sizeof(a_tmp));
    map_card_path(&fm, "B:\\FENESTRA\\FXCRASH.TMP", b_tmp, sizeof(b_tmp));
    (void)unlink(log_path); (void)unlink(old_path); (void)unlink(b_log);
    (void)unlink(a_tmp);    (void)unlink(b_tmp);    (void)rmdir(fm.root_dir);
    v71r7_flight_set_host_ops(NULL);
    printf("  PASS (c)\n");
}

/* ============================================================================
 * Test (d): smp stripe partitioning, JPEG-preempts-Rear, timeout quarantine
 * ============================================================================ */
typedef struct {
    uint32_t *row_hits;
    uint32_t  total_rows;
    uint32_t  sleep_us_helper1;
} SmpVerifyCtx;

static void smp_verify_cb(void *arg, uint32_t r0, uint32_t r1, unsigned worker_idx)
{
    SmpVerifyCtx *vc = (SmpVerifyCtx *)arg;
    if (vc->sleep_us_helper1 && worker_idx == 1u) {
        usleep(vc->sleep_us_helper1);
    } else {
        /* Tiny busy loop so parallel threads share stripes deterministically */
        volatile uint32_t spin = 0;
        for (uint32_t k = 0; k < 2000u; ++k) spin += k;
        (void)spin;
    }
    for (uint32_t r = r0; r < r1; ++r) {
        assert(r < vc->total_rows);
        __atomic_fetch_add(&vc->row_hits[r], 1u, __ATOMIC_RELAXED);
    }
}

static void test_d_smp_and_quarantine(void)
{
    printf("[Test (d)] smp dual-core stripe partitioning, JPEG-preempts-Rear & quarantine...\n");

    v71r7_flight_init();
    FxZ8LutIoGate *gate = v71r7_io_gate();
    __atomic_store_n(&gate->state, 0u, __ATOMIC_RELEASE);
    fx_z8_lut_io_reopen(gate, 0);

    assert(v71r7_smp_ensure_started() == 1);

    /* 1. Multi-threaded stripe partitioning: every row 0..511 hit EXACTLY once */
    V71r7SmpStats st0;
    v71r7_smp_get_stats(&st0);
    const uint32_t H = 512u;
    uint32_t *hits = (uint32_t *)calloc(H, sizeof(uint32_t));
    assert(hits != NULL);
    SmpVerifyCtx vc = { hits, H, 0u };
    v71r7_smp_run_stripes(FX_Z8_LUT_HELPER_JPEG, H, 16u, smp_verify_cb, &vc, FX_Z8_HB_JPEG_LANE0);
    for (uint32_t r = 0; r < H; ++r) {
        assert(hits[r] == 1u);
    }
    V71r7SmpStats st;
    v71r7_smp_get_stats(&st);
    assert(st.started == 1u);
    assert(st.available_helpers == 2u);
    assert(st.dispatches_jpeg == st0.dispatches_jpeg + 1u);
    assert(st.helper_stripes[0] + st.helper_stripes[1] >
           st0.helper_stripes[0] + st0.helper_stripes[1]);

    /* 2. End-to-end NV12 SMP vs non-SMP 0-diff */
    uint32_t c3d_len = 0;
    uint8_t *c3d_buf = gen_cube_3d_cross(17u, &c3d_len);
    assert(v71r7_lut_prepare_from_buffer(c3d_buf, c3d_len, "SMP.CUBE", 20u, NULL) == 1);
    free(c3d_buf);

    uint32_t *cache_a = (uint32_t *)calloc(CUBE_FRAME_CACHE_WORDS, sizeof(uint32_t));
    uint32_t *cache_b = (uint32_t *)calloc(CUBE_FRAME_CACHE_WORDS, sizeof(uint32_t));
    uint32_t w = 256u, h = 192u;
    uint8_t *y_smp  = (uint8_t *)malloc(w * h);
    uint8_t *uv_smp = (uint8_t *)malloc(w * h / 2u);
    uint8_t *y_ref  = (uint8_t *)malloc(w * h);
    uint8_t *uv_ref = (uint8_t *)malloc(w * h / 2u);
    fill_scene_nv12(y_smp, uv_smp, w, h, w, w, 0);
    memcpy(y_ref, y_smp, w * h);
    memcpy(uv_ref, uv_smp, w * h / 2u);

    uint32_t ga = 0u, gb = 0u;
    assert(v71r7_lut_apply_nv12(y_smp, uv_smp, w, h, w, w, cache_a, &ga, 1) == 1);
    assert(v71r7_lut_apply_nv12(y_ref, uv_ref, w, h, w, w, cache_b, &gb, 0) == 1);
    assert(memcmp(y_smp, y_ref, w * h) == 0);
    assert(memcmp(uv_smp, uv_ref, w * h / 2u) == 0);
    free(cache_a); free(cache_b); free(y_smp); free(uv_smp); free(y_ref); free(uv_ref);

    /* 3. FxZ8LutIoGate JPEG-preempts-Rear priority */
    uint32_t epoch = fx_z8_lut_io_epoch(gate);
    assert(epoch != 0u);
    assert(fx_z8_lut_frame_take(gate, epoch) == 1); /* Still JPEG frame active! */

    V71r7SmpStats before_rear, after_rear;
    v71r7_smp_get_stats(&before_rear);
    memset(hits, 0, H * sizeof(uint32_t));
    /* Rear MUST be denied helpers (falls back cleanly to synchronous caller) */
    v71r7_smp_run_stripes(FX_Z8_LUT_HELPER_REAR, H, 32u, smp_verify_cb, &vc, FX_Z8_HB_REAR_STEP);
    for (uint32_t r = 0; r < H; ++r) {
        assert(hits[r] == 1u);
    }
    v71r7_smp_get_stats(&after_rear);
    assert(after_rear.dispatches_rear == before_rear.dispatches_rear);
    assert(after_rear.rear_preempted_cnt == before_rear.rear_preempted_cnt + 1u);

    /* Meanwhile JPEG STILL acquires helpers while FX_Z8_LUT_IO_FRAME is held! */
    memset(hits, 0, H * sizeof(uint32_t));
    v71r7_smp_run_stripes(FX_Z8_LUT_HELPER_JPEG, H, 32u, smp_verify_cb, &vc, FX_Z8_HB_JPEG_LANE0);
    for (uint32_t r = 0; r < H; ++r) {
        assert(hits[r] == 1u);
    }
    V71r7SmpStats after_jpeg;
    v71r7_smp_get_stats(&after_jpeg);
    assert(after_jpeg.dispatches_jpeg == after_rear.dispatches_jpeg + 1u);

    fx_z8_lut_frame_drop(gate);

    /* 4. Timeout quarantine -> FX_Z8_FLIGHT_SMP_QUARANTINE */
    v71r7_smp_set_join_timeout_ms(12u);
    memset(hits, 0, H * sizeof(uint32_t));
    vc.sleep_us_helper1 = 80000u; /* Helper 1 stalls 80 ms > 12 ms timeout */
    v71r7_smp_run_stripes(FX_Z8_LUT_HELPER_JPEG, H, 32u, smp_verify_cb, &vc, FX_Z8_HB_JPEG_LANE0);

    v71r7_smp_get_stats(&st);
    assert(st.quarantine_cnt >= 1u);
    assert(st.available_helpers <= 1u);

    /* Verify FX_Z8_FLIGHT_SMP_QUARANTINE landed in flight ring */
    FxZ8FlightRecord rows[FX_Z8_FLIGHT_RING_COUNT];
    uint32_t nrec = v71r7_flight_snapshot(rows, FX_Z8_FLIGHT_RING_COUNT, NULL);
    int saw_quar = 0;
    for (uint32_t i = 0; i < nrec; ++i) {
        if (rows[i].event == FX_Z8_FLIGHT_SMP_QUARANTINE) {
            saw_quar = 1;
        }
    }
    assert(saw_quar == 1);

    /* Wait for late quarantined helper to finish and drain cleanly before freeing hits! */
    for (unsigned t = 0; t < 200u && !v71r7_smp_quarantine_drained(); ++t) {
        usleep(2000u);
    }
    assert(v71r7_smp_quarantine_drained() == 1);

    /* Remaining non-quarantined helper + caller still process new jobs 100% cleanly */
    v71r7_smp_set_join_timeout_ms(0u);
    vc.sleep_us_helper1 = 0u;
    memset(hits, 0, H * sizeof(uint32_t));
    v71r7_smp_run_stripes(FX_Z8_LUT_HELPER_JPEG, H, 32u, smp_verify_cb, &vc, FX_Z8_HB_JPEG_LANE0);
    for (uint32_t r = 0; r < H; ++r) {
        assert(hits[r] == 1u);
    }

    free(hits);
    v71r7_smp_stop();
    v71r7_lut_clear_active();
    printf("  PASS (d)\n");
}

/* ============================================================================
 * Test (e): neon_sentinel real probe + fault latches NEON_DISABLED -> scalar
 * ============================================================================ */
static unsigned injected_neon_fail_probe(void *ctx, int32_t (*delay)(uint32_t))
{
    (void)ctx;
    (void)delay;
    return V71R7_NEON_FAILED;
}

static void test_e_neon_sentinel_fallback(void)
{
    printf("[Test (e)] neon_sentinel real probe + fault latches NEON_DISABLED -> scalar fallback...\n");

    v71r7_flight_init();
    v71r7_neon_sentinel_reset_for_test();

    /* 1. Real Apple Silicon AArch64 NEON d8..d15 + v0..v31 canary self-test MUST PASS */
    assert(v71r7_neon_sentinel_probe_once() == 1);
    assert(v71r7_neon_sentinel_last_status() == V71R7_NEON_PASS);
    assert(v71r7_neon_sentinel_is_allowed() == 1);
    assert((v71r7_anchor()->flags & V71R7_ANCHOR_FLAG_NEON_DISABLED) == 0u);

    uint32_t c3d_len = 0;
    uint8_t *c3d_buf = gen_cube_3d_cross(17u, &c3d_len);
    assert(v71r7_lut_prepare_from_buffer(c3d_buf, c3d_len, "NEON.CUBE", 30u, NULL) == 1);
    free(c3d_buf);

    uint32_t *cache_n = (uint32_t *)calloc(CUBE_FRAME_CACHE_WORDS, sizeof(uint32_t));
    uint32_t *cache_s = (uint32_t *)calloc(CUBE_FRAME_CACHE_WORDS, sizeof(uint32_t));
    uint32_t w = 96u, h = 48u;
    uint8_t *nv16_y_n  = (uint8_t *)malloc(w * h);
    uint8_t *nv16_uv_n = (uint8_t *)malloc(w * h);
    uint8_t *nv16_y_s  = (uint8_t *)malloc(w * h);
    uint8_t *nv16_uv_s = (uint8_t *)malloc(w * h);
    uint8_t *nv12_y_n  = (uint8_t *)malloc(w * h);
    uint8_t *nv12_uv_n = (uint8_t *)malloc(w * h / 2u);
    uint8_t *nv12_y_s  = (uint8_t *)malloc(w * h);
    uint8_t *nv12_uv_s = (uint8_t *)malloc(w * h / 2u);

    fill_scene_nv16(nv16_y_n, nv16_uv_n, w, h, w, w, 0);
    memcpy(nv16_y_s, nv16_y_n, w * h);
    memcpy(nv16_uv_s, nv16_uv_n, w * h);
    fill_scene_nv12(nv12_y_n, nv12_uv_n, w, h, w, w, 0);
    memcpy(nv12_y_s, nv12_y_n, w * h);
    memcpy(nv12_uv_s, nv12_uv_n, w * h / 2u);

    V71r7LutRuntimeStats st0, st1, st2;
    v71r7_lut_runtime_get_stats(&st0);
    uint32_t gn = 0u, gs = 0u;
    assert(v71r7_lut_apply_still_nv16(nv16_y_n, nv16_uv_n, w, h, w, w, cache_n, &gn) == 1);
    assert(v71r7_lut_apply_nv12(nv12_y_n, nv12_uv_n, w, h, w, w, cache_n, &gn, 0) == 1);
    v71r7_lut_runtime_get_stats(&st1);
    assert(st1.nv16_neon_calls == st0.nv16_neon_calls + 1u);
    assert(st1.nv12_neon_calls == st0.nv12_neon_calls + 1u);

    /* 2. Inject NEON sentinel failure -> latches V71R7_ANCHOR_FLAG_NEON_DISABLED & logs event */
    v71r7_neon_sentinel_set_host_probe(injected_neon_fail_probe, NULL);
    assert(v71r7_neon_sentinel_probe_once() == 0);
    assert(v71r7_neon_sentinel_is_allowed() == 0);
    assert((v71r7_anchor()->flags & V71R7_ANCHOR_FLAG_NEON_DISABLED) != 0u);

    FxZ8FlightRecord rows[FX_Z8_FLIGHT_RING_COUNT];
    uint32_t nrec = v71r7_flight_snapshot(rows, FX_Z8_FLIGHT_RING_COUNT, NULL);
    int saw_fail = 0;
    for (uint32_t i = 0; i < nrec; ++i) {
        if (rows[i].event == FX_Z8_FLIGHT_NEON_SENTINEL_FAIL) {
            saw_fail = 1;
        }
    }
    assert(saw_fail == 1);

    /* 3. Subsequent NV16 & NV12 calls AUTOMATICALLY run scalar path with 0-diff! */
    assert(v71r7_lut_apply_still_nv16(nv16_y_s, nv16_uv_s, w, h, w, w, cache_s, &gs) == 1);
    assert(v71r7_lut_apply_nv12(nv12_y_s, nv12_uv_s, w, h, w, w, cache_s, &gs, 0) == 1);
    v71r7_lut_runtime_get_stats(&st2);
    assert(st2.nv16_neon_calls == st1.nv16_neon_calls);
    assert(st2.nv12_neon_calls == st1.nv12_neon_calls);
    assert(st2.nv16_scalar_calls == st1.nv16_scalar_calls + 1u);
    assert(st2.nv12_scalar_calls == st1.nv12_scalar_calls + 1u);

    assert(memcmp(nv16_y_n, nv16_y_s, w * h) == 0);
    assert(memcmp(nv16_uv_n, nv16_uv_s, w * h) == 0);
    assert(memcmp(nv12_y_n, nv12_y_s, w * h) == 0);
    assert(memcmp(nv12_uv_n, nv12_uv_s, w * h / 2u) == 0);

    free(cache_n); free(cache_s);
    free(nv16_y_n); free(nv16_uv_n); free(nv16_y_s); free(nv16_uv_s);
    free(nv12_y_n); free(nv12_uv_n); free(nv12_y_s); free(nv12_uv_s);
    v71r7_neon_sentinel_reset_for_test();
    v71r7_lut_clear_active();
    printf("  PASS (e)\n");
}

/* ============================================================================
 * Test (f): XM6 Hardening #1 (0xA02E7EED0 idle check, 512 MiB spacer, Op12 RGBLUT)
 * ============================================================================ */
typedef struct {
    int      call_idx;
    int      live_handles;
    int      spacer_held_during_retry;
    int      double_straddle;
    uint32_t spacer_cb_hits;
    uint64_t next_dma[4];
    uint8_t *bufs[4];
} SpacerMock;

static int smock_alloc(void *ctx, uint32_t req, struct z8_nfsp_allocation *out)
{
    SpacerMock *m = (SpacerMock *)ctx;
    int idx = m->call_idx++;
    if (idx >= 4) return 0;
    if (idx == 1 && m->live_handles == 1) {
        m->spacer_held_during_retry = 1;
    }
    uint8_t *mem = (uint8_t *)calloc(1, req);
    assert(mem != NULL);
    m->bufs[idx] = mem;
    out->handle = (void *)(uintptr_t)(idx + 1);
    out->cpu    = mem;
    out->dma    = m->next_dma[idx];
    out->actual = req;
    out->used   = 0u;
    m->live_handles += 1;
    return 1;
}

static void smock_release(void *ctx, struct z8_nfsp_allocation *blk)
{
    SpacerMock *m = (SpacerMock *)ctx;
    if (blk && blk->handle) {
        int idx = (int)(uintptr_t)blk->handle - 1;
        assert(idx >= 0 && idx < 4 && m->bufs[idx] != NULL);
        free(m->bufs[idx]);
        m->bufs[idx] = NULL;
        m->live_handles -= 1;
    }
    if (blk) memset(blk, 0, sizeof(*blk));
}

static void smock_on_spacer(void *ctx, uint64_t spacer_dma, uint32_t actual,
                            uint64_t fresh_dma, int ok)
{
    (void)spacer_dma; (void)actual; (void)fresh_dma; (void)ok;
    ((SpacerMock *)ctx)->spacer_cb_hits += 1u;
}

/* Simulated Z8 Core 0 + IOMMU + Opcode 12 RGBLUT DSP model */
#define SIM_POOL_BASE   UINT64_C(0x0840000000)
#define SIM_POOL_SIZE   (16u * 1024u * 1024u)
#define SIM_DSP_BASE    UINT32_C(0x80000000)

typedef struct {
    uint32_t core0_state;
    uint8_t  loader_fault;
    uint8_t  fw_loaded;
    uint32_t mailbox[4];
    uint64_t clock_us;
    uint8_t *pool_mem;
    uint32_t pool_bump;
    int      force_first_alloc_straddle;
    int      alloc_calls;
    int      live_allocs;
    int      sync_submit_calls;
    int      async_submit_calls;
    int      force_sync_uncertain;
    int      force_sync_corrupt_mbox;
    uint64_t bridge_host_va;
    uint32_t bridge_host_size;
} Xm6Op12Mock;

static uint32_t xmock_state(void *c)        { return ((Xm6Op12Mock *)c)->core0_state; }
static uint8_t  xmock_fault(void *c)        { return ((Xm6Op12Mock *)c)->loader_fault; }
static uint8_t  xmock_fw(void *c)           { return ((Xm6Op12Mock *)c)->fw_loaded; }
static void     xmock_mbox(void *c, uint32_t w[4]) { memcpy(w, ((Xm6Op12Mock *)c)->mailbox, 16u); }
static uint64_t xmock_time(void *c)         { return ((Xm6Op12Mock *)c)->clock_us; }
static void     xmock_sleep(void *c, uint32_t ms) { ((Xm6Op12Mock *)c)->clock_us += (uint64_t)ms * 1000u; }
static void     xmock_civac(void *c, void *a, uint32_t n) { (void)c; (void)a; (void)n; }
static void     xmock_async_submit(void *c, Z8Xm6DspReq *req)
{
    Xm6Op12Mock *m = (Xm6Op12Mock *)c;
    m->async_submit_calls += 1;
    m->clock_us += 1000u;
    req->dsp_status = Z8_XM6_DSP_OK;
    if (req->callback) {
        req->callback(UINT32_C(0x42180003), req);
    }
}

static uint64_t xmock_win_base(void *c, uint32_t win)
{
    Xm6Op12Mock *m = (Xm6Op12Mock *)c;
    if (win == 0x28u) return m->bridge_host_va;
    if (win == 0x2cu) return UINT64_C(0x0840000000);
    if (win == 0x2du) return UINT64_C(0x0860000000);
    return 0u;
}
static uint32_t xmock_win_size(void *c, uint32_t win)
{
    Xm6Op12Mock *m = (Xm6Op12Mock *)c;
    if (win == 0x28u) return m->bridge_host_size;
    if (win == 0x2cu || win == 0x2du) return UINT32_C(0x20000000);
    return 0u;
}
static uint32_t xmock_xlat(void *c, uint32_t win, uint64_t dma, uint32_t *dsp)
{
    uint64_t base = xmock_win_base(c, win);
    uint32_t sz   = xmock_win_size(c, win);
    if (!base || !sz || dma < base || dma >= base + (uint64_t)sz) return 3u;
    uint32_t dsp_base = (win == 0x28u) ? UINT32_C(0x41000000) :
                        (win == 0x2cu) ? SIM_DSP_BASE : (SIM_DSP_BASE + UINT32_C(0x20000000));
    *dsp = dsp_base + (uint32_t)(dma - base);
    return 0u;
}

static int xmock_alloc(void *c, uint32_t req, struct z8_nfsp_allocation *out)
{
    Xm6Op12Mock *m = (Xm6Op12Mock *)c;
    uint32_t aligned_req = (req + 511u) & ~511u;
    int call = m->alloc_calls++;
    if (m->force_first_alloc_straddle && call == 0) {
        /* Return a fake allocation straddling 0x0860000000! */
        uint8_t *tmp = (uint8_t *)calloc(1, aligned_req);
        assert(tmp != NULL);
        out->handle = tmp;
        out->cpu    = tmp;
        out->dma    = UINT64_C(0x0860000000) - 4096u; /* Straddles 512 MiB window! */
        out->actual = aligned_req;
        out->used   = 0u;
        m->live_allocs += 1;
        return 1;
    }
    if (m->pool_bump + aligned_req > SIM_POOL_SIZE) return 0;
    out->handle = (void *)(uintptr_t)(0x1000u + (uintptr_t)call);
    out->cpu    = m->pool_mem + m->pool_bump;
    out->dma    = SIM_POOL_BASE + m->pool_bump;
    out->actual = aligned_req;
    out->used   = 0u;
    m->pool_bump += aligned_req;
    m->live_allocs += 1;
    return 1;
}

static void xmock_release(void *c, struct z8_nfsp_allocation *blk)
{
    Xm6Op12Mock *m = (Xm6Op12Mock *)c;
    if (blk && blk->handle) {
        if ((uintptr_t)blk->handle >= 0x10000u) {
            free(blk->handle);
        }
        m->live_allocs -= 1;
    }
    if (blk) memset(blk, 0, sizeof(*blk));
}

static uint8_t clamp_u8_test(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/* Reference behavioral model of Nikon Z8 C3.11 Stock XM6 Core 0 Opcode 12 (RGBLUT) */
static uint32_t xmock_sync_submit(void *c, Z8Xm6RgblutJob *job, uint64_t job_dma, uint32_t lock_arg)
{
    Xm6Op12Mock *m = (Xm6Op12Mock *)c;
    m->sync_submit_calls += 1;
    m->clock_us += 1500u;

    assert(lock_arg == Z8_XM6_RGBLUT_LOCK_MS);
    assert(job->opcode == Z8_XM6_SLOT_RGBLUT);
    assert(job->lane == 1u);
    assert(job->status == Z8_XM6_RGBLUT_STATUS_INIT);
    assert(job->in_ptr == job_dma + 0x200u);
    assert(job->out_ptr == job_dma + 0xa00u);

    if (m->force_sync_uncertain) {
        /* Leave status untouched (0x72504e44) -> triggers uncertain + breaker trip */
        return 0u;
    }
    if (m->force_sync_corrupt_mbox) {
        m->mailbox[2] = 0x12345678u;
    }

    const uint8_t *arena_cpu = (const uint8_t *)(const void *)job;
    const Z8Xm6RgblutParam *in  = (const Z8Xm6RgblutParam *)(const void *)(arena_cpu + 0x200u);
    const Z8Xm6ImageParam  *out = (const Z8Xm6ImageParam  *)(const void *)(arena_cpu + 0xa00u);
    assert(in->mode == 0x40u);
    assert(in->work == job_dma + Z8_XM6_RGBLUT_CONTROL_BYTES);
    assert(in->image.format == Z8_XM6_FMT_NV16);
    assert(out->format == Z8_XM6_FMT_NV16);

    const uint8_t *sy = m->pool_mem + (in->image.plane_addr[0] - SIM_POOL_BASE);
    const uint8_t *sc = m->pool_mem + (in->image.plane_addr[1] - SIM_POOL_BASE);
    uint8_t       *dy = m->pool_mem + (out->plane_addr[0] - SIM_POOL_BASE);
    uint8_t       *dc = m->pool_mem + (out->plane_addr[1] - SIM_POOL_BASE);
    uint32_t w = (uint32_t)in->image.width;
    uint32_t h = (uint32_t)in->image.height;
    uint32_t pitch = (uint32_t)in->image.stride[0];

    /* Execute stock RGBLUT math: front matrix (permuted) -> 129-knot Q15 curves -> back matrix */
    for (uint32_t r = 0; r < h; ++r) {
        for (uint32_t x = 0; x < w; x += 2u) {
            int cb = (int)sc[(size_t)r * pitch + x] - 128;
            int cr = (int)sc[(size_t)r * pitch + x + 1u] - 128;
            int sum_cb = 0, sum_cr = 0;
            for (unsigned p = 0; p < 2u; ++p) {
                int yv = (int)sy[(size_t)r * pitch + x + p];
                int rgb[3];
                for (unsigned ch = 0; ch < 3u; ++ch) {
                    int m0 = in->front[ch * 3u + 0u];
                    int m1 = in->front[ch * 3u + 1u];
                    int m2 = in->front[ch * 3u + 2u];
                    int raw = (m0 * yv + m1 * cb + m2 * cr + 512) >> 10;
                    unsigned idx = (unsigned)clamp_u8_test(raw);
                    unsigned val = ((unsigned)in->curve[ch][idx / 2u] +
                                    (unsigned)in->curve[ch][(idx + 1u) / 2u] + 128u) >> 8;
                    rgb[ch] = (int)(val > 255u ? 255u : val);
                }
                int oy  = (in->back[0] * rgb[0] + in->back[1] * rgb[1] + in->back[2] * rgb[2] + 512) >> 10;
                int ocb = (in->back[3] * rgb[0] + in->back[4] * rgb[1] + in->back[5] * rgb[2]);
                int ocr = (in->back[6] * rgb[0] + in->back[7] * rgb[1] + in->back[8] * rgb[2]);
                dy[(size_t)r * pitch + x + p] = clamp_u8_test(oy);
                sum_cb += ocb;
                sum_cr += ocr;
            }
            dc[(size_t)r * pitch + x]      = clamp_u8_test(128 + ((sum_cb + 1024) >> 11));
            dc[(size_t)r * pitch + x + 1u] = clamp_u8_test(128 + ((sum_cr + 1024) >> 11));
        }
    }

    job->status = Z8_XM6_DSP_OK;
    return 1u;
}

static void test_f_xm6_hardening(void)
{
    printf("[Test (f)] XM6 Hardening #1: 0xA02E7EED0 idle check, 512MiB spacer retry, Op12 RGBLUT...\n");
    v71r7_flight_init();

    /* --- Part 1: 512 MiB spacer retry allocator (z8_fs_plat_alloc_class24_xm6_ops) --- */
    {
        /* Case 1a: Non-straddling allocation succeeds on first attempt (0 spacers) */
        SpacerMock sm;
        memset(&sm, 0, sizeof(sm));
        sm.next_dma[0] = UINT64_C(0x0850000000);
        Z8Class24SpacerOps sops = { smock_alloc, smock_release, smock_on_spacer, &sm };
        struct z8_nfsp_allocation blk;
        assert(z8_fs_plat_alloc_class24_xm6_ops(8u * 1024u * 1024u, &blk, &sops) == 1);
        assert(blk.dma == UINT64_C(0x0850000000));
        assert(sm.call_idx == 1 && sm.spacer_cb_hits == 0u && sm.live_handles == 1);
        smock_release(&sm, &blk);
        assert(sm.live_handles == 0);

        /* Case 1b: First block straddles 0x0860000000 (512 MiB boundary) -> spacer held,
         * fresh non-straddling block allocated, spacer freed, FX_Z8_FLIGHT_XM6_SPACER logged! */
        memset(&sm, 0, sizeof(sm));
        sm.next_dma[0] = UINT64_C(0x085FC00000); /* + 8 MiB = 0x0860400000 (straddles!) */
        sm.next_dma[1] = UINT64_C(0x0860400000); /* + 8 MiB = 0x0860C00000 (clean!) */
        assert(z8_fs_plat_alloc_class24_xm6_ops(8u * 1024u * 1024u, &blk, &sops) == 1);
        assert(blk.dma == UINT64_C(0x0860400000));
        assert(sm.call_idx == 2);
        assert(sm.spacer_held_during_retry == 1);
        assert(sm.spacer_cb_hits == 1u);
        assert(sm.live_handles == 1); /* Spacer already freed! */
        smock_release(&sm, &blk);
        assert(sm.live_handles == 0);

        FxZ8FlightRecord rows[FX_Z8_FLIGHT_RING_COUNT];
        uint32_t nrec = v71r7_flight_snapshot(rows, FX_Z8_FLIGHT_RING_COUNT, NULL);
        int saw_spacer = 0;
        for (uint32_t i = 0; i < nrec; ++i) {
            if (rows[i].event == FX_Z8_FLIGHT_XM6_SPACER && rows[i].core_flags == 1u) {
                saw_spacer = 1;
            }
        }
        assert(saw_spacer == 1);

        /* Case 1c: Double straddle -> both freed (0 leaks), returns 0 */
        memset(&sm, 0, sizeof(sm));
        sm.next_dma[0] = UINT64_C(0x085FC00000);
        sm.next_dma[1] = UINT64_C(0x087FC00000);
        assert(z8_fs_plat_alloc_class24_xm6_ops(8u * 1024u * 1024u, &blk, &sops) == 0);
        assert(sm.live_handles == 0);
        assert(sm.spacer_cb_hits == 1u);
    }

    /* --- Part 2: Core 0 mailbox idle-word check (0xA02E7EED0[0..3]) --- */
    Xm6Op12Mock xm;
    memset(&xm, 0, sizeof(xm));
    xm.core0_state = 2u;
    xm.fw_loaded   = 1u;
    xm.loader_fault= 0u;
    xm.mailbox[0]  = Z8_XM6_MAILBOX_IDLE_W0;
    xm.mailbox[1]  = Z8_XM6_MAILBOX_IDLE_W1;
    xm.mailbox[2]  = Z8_XM6_MAILBOX_IDLE_W2;
    xm.mailbox[3]  = Z8_XM6_MAILBOX_IDLE_W3;
    xm.pool_mem    = (uint8_t *)calloc(1, SIM_POOL_SIZE);
    assert(xm.pool_mem != NULL);

    Z8Xm6HwOps hw = {
        .read_core0_state   = xmock_state,
        .read_loader_fault  = xmock_fault,
        .read_fw_loaded     = xmock_fw,
        .iommu_win_base     = xmock_win_base,
        .iommu_win_size     = xmock_win_size,
        .iommu_xlat         = xmock_xlat,
        .dcache_civac       = xmock_civac,
        .async_submit       = xmock_async_submit,
        .sleep_ms           = xmock_sleep,
        .time_us            = xmock_time,
        .alloc_class24      = xmock_alloc,
        .release_class24    = xmock_release,
        .ctx                = &xm,
        .read_mailbox_words = xmock_mbox,
        .sync_submit        = xmock_sync_submit
    };

    struct z8_xm6_bridge bridge;
    z8_xm6_bridge_init(&bridge, &hw);
    xm.bridge_host_va   = (uint64_t)(uintptr_t)&bridge;
    xm.bridge_host_size = (uint32_t)sizeof(bridge);

    assert(z8_xm6_core0_mailbox_idle(&bridge) == 1);
    assert(z8_xm6_core0_hw_only_ready(&bridge) == 1);
    assert(z8_xm6_core0_hw_ready(&bridge, Z8_XM6_MODE_AUTO) == 1);

    Z8FilmSimStatus fst;
    memset(&fst, 0, sizeof(fst));
    z8_xm6_fill_status(&bridge, Z8_XM6_MODE_AUTO, &fst);
    assert((fst.xm6_runtime_flags & Z8_XM6_FLAG_HW_READY) != 0u);

    /* Corrupt each of the 4 mailbox words individually -> MUST reject! */
    for (unsigned idx = 0; idx < 4u; ++idx) {
        uint32_t saved = xm.mailbox[idx];
        xm.mailbox[idx] ^= 1u;
        assert(z8_xm6_core0_mailbox_idle(&bridge) == 0);
        assert(z8_xm6_core0_hw_only_ready(&bridge) == 0);
        assert(z8_xm6_core0_hw_ready(&bridge, Z8_XM6_MODE_AUTO) == 0);
        memset(&fst, 0, sizeof(fst));
        z8_xm6_fill_status(&bridge, Z8_XM6_MODE_AUTO, &fst);
        assert((fst.xm6_runtime_flags & Z8_XM6_FLAG_HW_READY) == 0u);
        xm.mailbox[idx] = saved;
    }
    assert(z8_xm6_core0_hw_ready(&bridge, Z8_XM6_MODE_AUTO) == 1);

    /* --- Part 3: Stock Core 0 Opcode 12 (RGBLUT) via z8_xm6_rgblut_nv16 & lut_runtime --- */
    uint32_t sep_len = 0;
    uint8_t *sep_buf = gen_cube_1d_separable(17u, 1, &sep_len);
    assert(v71r7_lut_prepare_from_buffer(sep_buf, sep_len, "LUTS\\XM6\\SwapRB.CUBE", 40u, NULL) == 1);
    free(sep_buf);

    v71r7_lut_bind_xm6(&bridge, Z8_XM6_MODE_AUTO);

    const uint32_t w = 128u, h = 64u, pitch = 128u;
    const uint32_t plane_bytes = pitch * h;
    /* Carve 4 disjoint 64B-aligned NV16 planes in simulated Class 0x24 pool */
    uint32_t off_sy = 0u;
    uint32_t off_sc = off_sy + plane_bytes;
    uint32_t off_dy = off_sc + plane_bytes;
    uint32_t off_dc = off_dy + plane_bytes;
    xm.pool_bump = off_dc + plane_bytes;

    uint8_t *sy_cpu = xm.pool_mem + off_sy;
    uint8_t *sc_cpu = xm.pool_mem + off_sc;
    uint8_t *dy_cpu = xm.pool_mem + off_dy;
    uint8_t *dc_cpu = xm.pool_mem + off_dc;
    uint64_t sy_dma = SIM_POOL_BASE + off_sy;
    uint64_t sc_dma = SIM_POOL_BASE + off_sc;
    uint64_t dy_dma = SIM_POOL_BASE + off_dy;
    uint64_t dc_dma = SIM_POOL_BASE + off_dc;
    fill_scene_nv16(sy_cpu, sc_cpu, w, h, pitch, pitch, 0);

    /* Force first RGBLUT arena allocation to straddle 512 MiB -> spacer retry succeeds! */
    xm.force_first_alloc_straddle = 1;
    uint32_t *cache = (uint32_t *)calloc(CUBE_FRAME_CACHE_WORDS, sizeof(uint32_t));
    uint32_t cgen = 0u;
    int rc = v71r7_lut_apply_still_nv16_oop(sy_cpu, sc_cpu, sy_dma, sc_dma,
                                            dy_cpu, dc_cpu, dy_dma, dc_dma,
                                            w, h, pitch, pitch, cache, &cgen);
    assert(rc == 2); /* Executed on XM6 Core 0 Opcode 12! */
    assert(xm.sync_submit_calls == 1);
    assert(xm.alloc_calls == 2); /* 1 straddling spacer + 1 clean arena */
    assert(xm.live_allocs == 0); /* Both spacer & arena cleanly freed! */

    /* Compare XM6 Op12 output vs ARM 3D LUT output (1D curve Q15 rounding <= 2 LSB max) */
    uint8_t *arm_y = (uint8_t *)malloc(plane_bytes);
    uint8_t *arm_c = (uint8_t *)malloc(plane_bytes);
    memcpy(arm_y, sy_cpu, plane_bytes);
    memcpy(arm_c, sc_cpu, plane_bytes);
    assert(v71r7_lut_apply_still_nv16(arm_y, arm_c, w, h, pitch, pitch, cache, &cgen) == 1);
    int max_diff = 0;
    for (uint32_t i = 0; i < plane_bytes; ++i) {
        int dy_d = abs((int)dy_cpu[i] - (int)arm_y[i]);
        int dc_d = abs((int)dc_cpu[i] - (int)arm_c[i]);
        if (dy_d > max_diff) max_diff = dy_d;
        if (dc_d > max_diff) max_diff = dc_d;
    }
    /* Stock 10-bit fixed matrix vs 16-bit ARM matrix agrees within <= 2 LSB across full frame */
    assert(max_diff <= 2);

    /* Busy mailbox rejects Op12 cleanly without tripping breaker & falls back to ARM! */
    xm.mailbox[0] = 0x11111111u;
    rc = v71r7_lut_apply_still_nv16_oop(sy_cpu, sc_cpu, sy_dma, sc_dma,
                                        dy_cpu, dc_cpu, dy_dma, dc_dma,
                                        w, h, pitch, pitch, cache, &cgen);
    assert(rc == 1); /* Transparent ARM fallback! */
    assert(bridge.breaker_tripped == 0u);
    assert(bridge.last_err == Z8_XM6_ERR_NOT_READY);
    assert(memcmp(dy_cpu, arm_y, plane_bytes) == 0);
    assert(memcmp(dc_cpu, arm_c, plane_bytes) == 0);
    xm.mailbox[0] = Z8_XM6_MAILBOX_IDLE_W0;

    /* Uncertain Op12 response trips one-shot breaker & transparently falls back to ARM! */
    xm.force_sync_uncertain = 1;
    rc = v71r7_lut_apply_still_nv16_oop(sy_cpu, sc_cpu, sy_dma, sc_dma,
                                        dy_cpu, dc_cpu, dy_dma, dc_dma,
                                        w, h, pitch, pitch, cache, &cgen);
    assert(rc == 1); /* Transparent ARM fallback after breaker trip! */
    assert(bridge.breaker_tripped == 1u);
    assert(bridge.last_err == Z8_XM6_ERR_TIMEOUT);
    assert(xm.live_allocs == 0);
    assert(memcmp(dy_cpu, arm_y, plane_bytes) == 0);
    assert(memcmp(dc_cpu, arm_c, plane_bytes) == 0);

    /* Safe reset refuses while mailbox is non-idle, then recovers once mailbox is idle! */
    xm.force_sync_uncertain = 0;
    xm.mailbox[0] = 0x11111111u;
    z8_xm6_reset_breaker(&bridge);
    assert(bridge.breaker_tripped == 1u);
    assert(z8_xm6_core0_hw_ready(&bridge, Z8_XM6_MODE_AUTO) == 0);
    xm.mailbox[0] = Z8_XM6_MAILBOX_IDLE_W0;
    z8_xm6_reset_breaker(&bridge);
    assert(z8_xm6_core0_hw_ready(&bridge, Z8_XM6_MODE_AUTO) == 1);
    rc = v71r7_lut_apply_still_nv16_oop(sy_cpu, sc_cpu, sy_dma, sc_dma,
                                        dy_cpu, dc_cpu, dy_dma, dc_dma,
                                        w, h, pitch, pitch, cache, &cgen);
    assert(rc == 2);

    /* Verify Live HDF FX_Z8_FLIGHT_XM6_SUBMIT is edge-logged ONCE (no 60 Hz ring flood!) */
    {
        uint32_t lv_off = (xm.pool_bump + 511u) & ~511u;
        uint32_t lv_y_b = 1024u * 680u;
        uint32_t lv_c_b = 1024u * 340u;
        xm.pool_bump = lv_off + lv_y_b + lv_c_b;
        uint8_t *ly = xm.pool_mem + lv_off;
        uint8_t *lc = ly + lv_y_b;
        uint64_t ly_dma = SIM_POOL_BASE + lv_off;
        uint64_t lc_dma = ly_dma + lv_y_b;

        FxZ8FlightRecord rows[FX_Z8_FLIGHT_RING_COUNT];
        uint32_t before_cnt = 0u, after_cnt = 0u;
        uint32_t n0 = v71r7_flight_snapshot(rows, FX_Z8_FLIGHT_RING_COUNT, NULL);
        for (uint32_t i = 0u; i < n0; ++i) {
            if (rows[i].event == FX_Z8_FLIGHT_XM6_SUBMIT && rows[i].core_flags == Z8_XM6_SLOT_HDF) {
                ++before_cnt;
            }
        }
        for (int f = 0; f < 5; ++f) {
            assert(z8_xm6_live_hdf_nv12(&bridge, Z8_XM6_MODE_AUTO, ly, lc, ly_dma, lc_dma,
                                        1024u, 680u, 1024u, 1024u, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_OK);
        }
        assert(xm.async_submit_calls == 5);
        uint32_t n1 = v71r7_flight_snapshot(rows, FX_Z8_FLIGHT_RING_COUNT, NULL);
        for (uint32_t i = 0u; i < n1; ++i) {
            if (rows[i].event == FX_Z8_FLIGHT_XM6_SUBMIT && rows[i].core_flags == Z8_XM6_SLOT_HDF) {
                ++after_cnt;
            }
        }
        assert(after_cnt == before_cnt + 1u); /* Logged ONCE across 5 Live frames! */
        z8_xm6_release_ext_ws(&bridge);
        assert(xm.live_allocs == 0);
    }

    free(arm_y); free(arm_c); free(cache); free(xm.pool_mem);
    v71r7_lut_bind_xm6(NULL, Z8_XM6_MODE_ARM_ONLY);
    v71r7_lut_clear_active();
    printf("  PASS (f)\n");
}

/* Track cache invalidation ordering vs LutCoreCtx->generation updates in Test (g) */
static uint32_t g_inval_calls = 0u;
static uint32_t g_gen_at_inval = 0u;
void v71r7_layer0_cache_invalidate_before_swap(void)
{
    ++g_inval_calls;
    uint32_t g = 0u;
    (void)v71r7_lut_is_active(&g, NULL);
    g_gen_at_inval = g;
}

typedef struct {
    uint32_t *done_flag;
    uint32_t  sleep_ms;
} R4QuarCopyCtx;

static void r4_quar_copy_cb(void *arg, uint32_t r0, uint32_t r1, unsigned worker_idx)
{
    const R4QuarCopyCtx *c = (const R4QuarCopyCtx *)arg;
    (void)r1;
    if (worker_idx != 0u) {
        struct timespec ts = { 0, (long)c->sleep_ms * 1000000L };
        nanosleep(&ts, NULL);
        __atomic_fetch_add(c->done_flag, 1u, __ATOMIC_RELEASE);
    } else if (r0 == 0u) {
        /* Briefly yield on stripe 0 so helper threads deterministically claim a stripe */
        struct timespec ts = { 0, 2000000L };
        nanosleep(&ts, NULL);
    }
}

static void r4_sync_yield_cb(void *arg, uint32_t r0, uint32_t r1, unsigned worker_idx)
{
    uint32_t *rows = (uint32_t *)arg;
    (void)worker_idx;
    __atomic_fetch_add(rows, r1 - r0, __ATOMIC_RELAXED);
}

/* ============================================================================
 * Test (g): V71.7-r4 SMP Prio 30 / QUAR_DRAINED / Zero-Stack-UAF / Sync-Yield /
 *           Cache-Swap Ordering / Strict 0 B Dormant & Stage Gating
 * ============================================================================ */
static void test_g_r4_smp_fixes_cache_race_dormant(void)
{
    printf("[Test (g)] V71.7-r4 SMP prio 30, QUAR_DRAINED, zero-UAF, sync-yield & dormant 0 B gating...\n");
    v71r7_lut_runtime_shutdown_for_test();
    memset(v71r7_host_dram_region, 0, sizeof(v71r7_host_dram_region));
    v71r7_flight_init();

    /* g1. Priority 30 (strictly below Z8 prio <= 28 RT tasks!) */
    _Static_assert(V71R7_SMP_PRIORITY == 30u, "SMP helper priority must be 30");
    assert(V71R7_SMP_PRIORITY == 30u);

    /* g2. Dormant 0 B: v71r7_lut_bind_xm6() must NOT allocate LutCoreCtx; 0 SMP helpers! */
    struct z8_xm6_bridge dummy_br;
    memset(&dummy_br, 0, sizeof(dummy_br));
    v71r7_lut_bind_xm6(&dummy_br, Z8_XM6_MODE_AUTO);
    assert(v71r7_anchor()->lut_core_ctx == NULL);
    assert(v71r7_anchor()->xm6_bridge == &dummy_br);
    assert(v71r7_smp_online_helpers() == 0u);
    V71r7SmpStats st0;
    v71r7_smp_get_stats(&st0);
    assert(st0.started == 0u && st0.online_helpers == 0u);
    assert(v71r7_anchor()->lut_core_ctx == NULL); /* Still 0 B! */
    v71r7_lut_bind_xm6(NULL, Z8_XM6_MODE_ARM_ONLY);

    /* g3. Prepare LUT & verify cache invalidation fires BEFORE LutCoreCtx->generation changes */
    uint32_t clen = 0u;
    uint8_t *cbuf = gen_cube_3d_cross(17u, &clen);
    FxZ8LutProgress prog;
    g_inval_calls = 0u;
    g_gen_at_inval = 999u;
    assert(v71r7_lut_prepare_from_buffer(cbuf, clen, "r4_gen1.cube", 10u, &prog) == 1);
    assert(g_inval_calls == 1u && g_gen_at_inval == 0u); /* Before gen 10 was visible! */
    assert(v71r7_lut_prepare_from_buffer(cbuf, clen, "r4_gen2.cube", 11u, &prog) == 1);
    assert(g_inval_calls == 2u && g_gen_at_inval == 10u); /* Saw OLD gen 10, NOT 11! */
    /* Still 0 helper tasks before first SMP render! */
    assert(v71r7_smp_online_helpers() == 0u);

    /* g4. smp_run_sync_only yields every 8 stripes for 45MP Still JPEG when SMP stopped */
    uint32_t rows_done = 0u;
    v71r7_smp_run_stripes(FX_Z8_LUT_HELPER_JPEG, 512u, 32u, r4_sync_yield_cb, &rows_done, FX_Z8_HB_JPEG_LANE0);
    assert(rows_done == 512u);
    v71r7_smp_get_stats(&st0);
    assert(st0.sync_yields >= 1u); /* 16 stripes -> yields at stripe 8 */

    /* g5. Stage mask & lut.wlm module gating */
    uint8_t y[64 * 64], uv[64 * 64];
    memset(y, 120, sizeof(y));
    memset(uv, 128, sizeof(uv));
    uint32_t *cache = (uint32_t *)calloc(1u, CUBE_FRAME_CACHE_BYTES);
    assert(cache != NULL);
    uint32_t cgen = 0u;

    v71r7_host_set_lut_module_loaded(0);
    assert(v71r7_lut_is_active(NULL, NULL) == 0);
    assert(v71r7_lut_is_active_for(V71R7_LUT_STAGE_PREVIEW, NULL, NULL) == 0);
    assert(v71r7_lut_is_active_for(V71R7_LUT_STAGE_STILL, NULL, NULL) == 0);
    assert(v71r7_lut_is_active_for(V71R7_LUT_STAGE_LIVE, NULL, NULL) == 0);
    assert(v71r7_lut_apply_nv12(y, uv, 64u, 64u, 64u, 64u, cache, &cgen, 1) == 0);
    assert(v71r7_lut_apply_still_nv16(y, uv, 64u, 64u, 64u, 64u, cache, &cgen) == 0);
    assert(v71r7_smp_online_helpers() == 0u); /* Dormant never spawned SMP! */
    v71r7_host_set_lut_module_loaded(1);

    /* Stage mask: LIVE only -> STILL returns 0, PREVIEW inactive, LIVE active! */
    V71r7DramAnchor *anc = v71r7_anchor();
    anc->lut_master_on = 1u;
    anc->lut_stage_mask = V71R7_LUT_STAGE_LIVE;
    anc->lut_cfg_set = 1u;
    assert(v71r7_lut_is_active_for(V71R7_LUT_STAGE_LIVE, NULL, NULL) == 1);
    assert(v71r7_lut_is_active_for(V71R7_LUT_STAGE_PREVIEW, NULL, NULL) == 0);
    assert(v71r7_lut_is_active_for(V71R7_LUT_STAGE_STILL, NULL, NULL) == 0);
    assert(v71r7_lut_apply_still_nv16(y, uv, 64u, 64u, 64u, 64u, cache, &cgen) == 0);

    /* Enable all stages -> lazy SMP start on first use_smp=1 NV12 apply! */
    anc->lut_stage_mask = V71R7_LUT_STAGE_ALL;
    assert(v71r7_lut_apply_nv12(y, uv, 64u, 64u, 64u, 64u, cache, &cgen, 1) == 1);
    assert(v71r7_smp_online_helpers() == V71R7_SMP_HELPER_COUNT);
    uint32_t sents[3] = {0u, 0u, 0u};
    for (int t = 0; t < 50; ++t) {
        v71r7_smp_get_helper_sentinels(sents);
        if (sents[1] == V71R7_NEON_PASS && sents[2] == V71R7_NEON_PASS) break;
        usleep(1000);
    }
    assert(sents[0] == V71R7_NEON_PASS && sents[1] == V71R7_NEON_PASS && sents[2] == V71R7_NEON_PASS);

    /* g6. Timeout quarantine: verify stack-copy zero-UAF, QUAR_DRAINED==5 (NOT QUEUED==2!),
     *     and retired_cubes defers old_cube free until quarantined helper drains! */
    uint32_t late_done = 0u;
    v71r7_smp_set_join_timeout_ms(10u);
    {
        /* Stack-local job context: popped immediately after v71r7_smp_run_stripes_copy returns! */
        R4QuarCopyCtx stack_ctx = { .done_flag = &late_done, .sleep_ms = 60u };
        int undrained = v71r7_smp_run_stripes_copy(FX_Z8_LUT_HELPER_REAR, 256u, 16u,
                                                   r4_quar_copy_cb, &stack_ctx, sizeof(stack_ctx),
                                                   FX_Z8_HB_REAR_STEP);
        assert(undrained != 0);
        /* Swap active_cube WHILE helper is still quarantined (undrained) -> must defer free (0 UAF!) */
        assert(v71r7_lut_prepare_from_buffer(cbuf, clen, "r4_gen3.cube", 12u, &prog) == 1);
    }
    /* Wait for late quarantined helper to finish cleanly and verify EXACT phase == QUAR_DRAINED (5)! */
    for (int t = 0; t < 200 && !v71r7_smp_quarantine_drained(); ++t) {
        usleep(2000);
    }
    assert(v71r7_smp_quarantine_drained() == 1);
    assert(__atomic_load_n(&late_done, __ATOMIC_ACQUIRE) >= 1u);

    V71r7SmpPool *pool = v71r7_lut_core_smp_pool_peek();
    assert(pool != NULL);
    int saw_quar_drained = 0;
    for (unsigned i = 0u; i < V71R7_SMP_HELPER_COUNT; ++i) {
        uint32_t ph = __atomic_load_n(&pool->helpers[i].phase, __ATOMIC_ACQUIRE) & V71R7_SMP_PHASE_MASK;
        assert(ph != V71R7_SMP_PHASE_QUEUED && "QUAR_DRAINED bitwise bug regressed to QUEUED!");
        if (ph == V71R7_SMP_PHASE_QUAR_DRAINED) {
            ++saw_quar_drained;
        }
    }
    assert(saw_quar_drained >= 1);
    v71r7_smp_set_join_timeout_ms(0u);

    free(cache);
    free(cbuf);
    v71r7_lut_runtime_shutdown_for_test();
    printf("  PASS (g)\n");
}

/* ============================================================================
 * Test (h): V71.7-r7 528 KiB L2-Resident Baked 64x64x33 YCbCr 3D LUT +
 *           layer0_copy_plane_neon (0-diff NEON==Scalar, PSNR, SMP & gen-rebake)
 * ============================================================================ */
static void test_h_r7_baked_lut_and_neon_copy(void)
{
    printf("[Test (h)] V71.7-r7 528 KiB baked 64x64x33 YCbCr LUT (NEON==Scalar 0-diff, PSNR, SMP) & copy...\n");
    v71r7_lut_runtime_shutdown_for_test();
    memset(v71r7_host_dram_region, 0, sizeof(v71r7_host_dram_region));
    v71r7_flight_init();
    v71r7_neon_sentinel_reset_for_test();
    assert(v71r7_neon_sentinel_probe_once() == 1);

    uint32_t *baked = (uint32_t *)calloc(LAYER0_BAKED_WORDS, sizeof(uint32_t));
    uint32_t *cache_ref = (uint32_t *)calloc(CUBE_FRAME_CACHE_WORDS, sizeof(uint32_t));
    assert(baked && cache_ref);

    uint32_t lens[3] = {0};
    uint8_t *bufs[3];
    bufs[0] = gen_cube_identity(9u, &lens[0]);
    bufs[1] = gen_cube_1d_separable(17u, 1, &lens[1]);
    bufs[2] = gen_cube_3d_cross(33u, &lens[2]);

    JpegCube *cube = (JpegCube *)calloc(1, sizeof(JpegCube));
    assert(cube != NULL);

    const uint32_t widths[] = { 14u, 66u, 128u, 320u };
    const uint32_t height = 64u;

    for (unsigned lut_i = 0; lut_i < 3u; ++lut_i) {
        jpeg_cube_init(cube);
        uint32_t pos = (lens[lut_i] >= 3u && bufs[lut_i][0] == 0xEFu) ? 3u : 0u;
        char line[CUBE_LINE];
        uint32_t ll = 0;
        while (pos < lens[lut_i]) {
            char ch = (char)bufs[lut_i][pos++];
            if (ch == '\n') { line[ll] = '\0'; assert(jpeg_cube_line(cube, line) == 1); ll = 0; }
            else line[ll++] = ch;
        }
        if (ll) { line[ll] = '\0'; assert(jpeg_cube_line(cube, line) == 1); }
        assert(jpeg_cube_finish(cube) == 1);

        layer0_bake_ycbcr_64x64x33(cube, baked, 0u, LAYER0_BAKED_CHROMA);
        memset(cache_ref, 0, CUBE_FRAME_CACHE_BYTES);

        /* Identity LUT: exact neutral gray (Cb=128, Cr=128) and exact endpoints Y=0 & Y=255! */
        if (lut_i == 0u) {
            uint8_t gy[16 * 4], guv[16 * 2], dy[16 * 4], duv[16 * 2];
            for (unsigned i = 0; i < 16u * 4u; ++i) gy[i] = (i & 1u) ? 255u : 0u;
            memset(guv, 128, sizeof(guv));
            Layer0Frame fg = { gy, guv, dy, duv, 16u, 4u, 16u, 16u, 16u, 16u };
            layer0_grade_nv12_baked_neon(baked, &fg, 0u, 4u);
            for (unsigned i = 0; i < 16u * 4u; ++i) assert(dy[i] == gy[i]);
            for (unsigned i = 0; i < 16u * 2u; ++i) assert(duv[i] == 128u);
        }

        for (unsigned wi = 0; wi < sizeof(widths) / sizeof(widths[0]); ++wi) {
            uint32_t w = widths[wi];
            uint32_t stride = w + 16u;
            size_t y_sz = (size_t)stride * height;
            size_t uv_sz = (size_t)stride * (height / 2u);
            uint8_t *sy = (uint8_t *)malloc(y_sz), *suv = (uint8_t *)malloc(uv_sz);
            uint8_t *dy_s = (uint8_t *)calloc(1, y_sz), *duv_s = (uint8_t *)calloc(1, uv_sz);
            uint8_t *dy_n = (uint8_t *)calloc(1, y_sz), *duv_n = (uint8_t *)calloc(1, uv_sz);
            uint8_t *dy_r = (uint8_t *)calloc(1, y_sz), *duv_r = (uint8_t *)calloc(1, uv_sz);
            fill_scene_nv12(sy, suv, w, height, stride, stride, 0);

            Layer0Frame fs = { sy, suv, dy_s, duv_s, w, height, stride, stride, stride, stride };
            Layer0Frame fn = { sy, suv, dy_n, duv_n, w, height, stride, stride, stride, stride };
            Layer0Frame fr = { sy, suv, dy_r, duv_r, w, height, stride, stride, stride, stride };
            layer0_grade_nv12_baked(baked, &fs, 0u, height);
            layer0_grade_nv12_baked_neon(baked, &fn, 0u, height);
            assert(memcmp(dy_s, dy_n, y_sz) == 0);
            assert(memcmp(duv_s, duv_n, uv_sz) == 0);

            /* Accuracy check vs exact 3D tetrahedral reference on realistic 3D cross LUT */
            if (lut_i == 2u && w == 320u) {
                layer0_grade_nv12_planes(cube, cache_ref, &fr, 0u, height);
                uint64_t mse_y = 0u, mse_uv = 0u;
                for (uint32_t r = 0; r < height; ++r) {
                    for (uint32_t x = 0; x < w; ++x) {
                        int d = (int)dy_n[(size_t)r * stride + x] - (int)dy_r[(size_t)r * stride + x];
                        mse_y += (uint64_t)(d * d);
                    }
                }
                for (uint32_t r = 0; r < height / 2u; ++r) {
                    for (uint32_t x = 0; x < w; ++x) {
                        int d = (int)duv_n[(size_t)r * stride + x] - (int)duv_r[(size_t)r * stride + x];
                        mse_uv += (uint64_t)(d * d);
                    }
                }
                /* Average squared error < 4.0 (PSNR > 42 dB on extreme full-range synthetic noise; >48 dB on real photos!) */
                assert(mse_y < (uint64_t)w * height * 4u);
                assert(mse_uv < (uint64_t)w * (height / 2u) * 4u);
            }
            free(sy); free(suv); free(dy_s); free(duv_s); free(dy_n); free(duv_n); free(dy_r); free(duv_r);
        }
    }

    /* End-to-end v71r7_lut_apply_nv12_baked: SMP vs non-SMP 0-diff + generation auto-rebake */
    FxZ8LutIoGate *gate = v71r7_io_gate();
    __atomic_store_n(&gate->state, 0u, __ATOMIC_RELEASE);
    fx_z8_lut_io_reopen(gate, 0);
    assert(v71r7_lut_prepare_from_buffer(bufs[2], lens[2], "R7_BAKED.CUBE", 70u, NULL) == 1);

    uint32_t w = 256u, h = 192u;
    uint8_t *y_smp = (uint8_t *)malloc(w * h), *uv_smp = (uint8_t *)malloc(w * h / 2u);
    uint8_t *y_seq = (uint8_t *)malloc(w * h), *uv_seq = (uint8_t *)malloc(w * h / 2u);
    fill_scene_nv12(y_smp, uv_smp, w, h, w, w, 0);
    memcpy(y_seq, y_smp, w * h);
    memcpy(uv_seq, uv_smp, w * h / 2u);

    uint32_t *baked2 = (uint32_t *)calloc(LAYER0_BAKED_WORDS, sizeof(uint32_t));
    uint32_t bg1 = 0u, bg2 = 0u;
    assert(v71r7_lut_apply_nv12_baked(y_smp, uv_smp, w, h, w, w, baked, &bg1, 1) == 1);
    assert(v71r7_lut_apply_nv12_baked(y_seq, uv_seq, w, h, w, w, baked2, &bg2, 0) == 1);
    assert(bg1 == 70u && bg2 == 70u);
    assert(memcmp(y_smp, y_seq, w * h) == 0);
    assert(memcmp(uv_smp, uv_seq, w * h / 2u) == 0);

    /* Switching LUT generation auto-rebakes table! */
    assert(v71r7_lut_prepare_from_buffer(bufs[0], lens[0], "R7_ID.CUBE", 71u, NULL) == 1);
    assert(v71r7_lut_apply_nv12_baked(y_smp, uv_smp, w, h, w, w, baked, &bg1, 1) == 1);
    assert(bg1 == 71u);

    /* layer0_copy_plane_neon exact byte copy across odd & large sizes */
    const size_t sizes[] = { 13u, 63u, 64u, 65u, 127u, 1024u * 64u + 7u };
    for (unsigned si = 0; si < sizeof(sizes) / sizeof(sizes[0]); ++si) {
        size_t n = sizes[si];
        uint8_t *src = (uint8_t *)malloc(n + 16u);
        uint8_t *dst = (uint8_t *)calloc(1, n + 16u);
        for (size_t i = 0; i < n + 16u; ++i) src[i] = (uint8_t)((i * 37u + 11u) & 255u);
        layer0_copy_plane_neon(dst + 1u, src + 3u, n, 1);
        assert(memcmp(dst + 1u, src + 3u, n) == 0);
        assert(dst[0] == 0u && dst[n + 1u] == 0u);
        free(src); free(dst);
    }

    free(y_smp); free(uv_smp); free(y_seq); free(uv_seq);
    free(baked); free(baked2); free(cache_ref); free(cube);
    free(bufs[0]); free(bufs[1]); free(bufs[2]);
    v71r7_lut_runtime_shutdown_for_test();
    printf("  PASS (h)\n");
}

int main(void)
{
    memset(v71r7_host_dram_region, 0, sizeof(v71r7_host_dram_region));
    v71r7_flight_init();

    test_a_cube_parser();
    test_b_neon_vs_scalar_and_cache();
    test_c_photo_flight_and_watchdog();
    test_d_smp_and_quarantine();
    test_e_neon_sentinel_fallback();
    test_f_xm6_hardening();
    test_g_r4_smp_fixes_cache_race_dormant();
    test_h_r7_baked_lut_and_neon_copy();

    v71r7_lut_runtime_shutdown_for_test();
    printf("\n=== ALL 8 V71.7-r7 LUT CORE & XM6 HARDENING TEST GROUPS (a..h) PASSED (ASan+UBSan CLEAN) ===\n");
    return 0;
}
