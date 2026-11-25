/* SPDX-License-Identifier: MIT
 * Host ASan + UBSan Acceptance Test Suite for Nikon Z8 C3.11 EL1 ARM64 <-> CEVA-XM6 Bridge.
 *
 * Mocks Z8 C3.11 ROM entry points (0x40c909a8, 0x40c90bec, 0x40cb6d50, 0x40cb6d0c,
 * 0x40cb6b50, 0x40d2b9c8, 0x40187b00) and dispatches real 32-bit DSP IOMMU VAs into the
 * actual CEVA-XM6 reference kernels:
 *   - Slot 39/40: zf_grain_xm6_run()     (v71r5-xm6-dsp-kit/sim_harness/zf_grain_xm6_ref.c)
 *   - Slot 41:    ricoh_hdf_xm6_v2_run() (v71r5-xm6-hdf-kit/sim_harness/ricoh_hdf_xm6_v2.c)
 *
 * Coverage:
 *   (a) QEMU state!=2 / loader_fault!=0 / fw_loaded==0 / xm6_mode=ARM_ONLY zero-latency NEON fallback
 *   (b) 0x0c4..0x0cb CPU alias stripping + >=0x08c0000000 IOMMU OOB rejection + cross-window rejection
 *       + chunked <= 512 KiB 0x40d2b9c8 D-Cache maintenance
 *   (c) Async polling, enqueue-fail, BAD_PARAM, timeout -> 0x40c90bec cancel -> one-shot circuit breaker
 *       -> pristine source atomic ARM NEON fallback -> Z8_FS_OP_XM6_RESET_BREAKER recovery
 *       + Stage-2 dirty rollback telemetry & stuck req_in_flight protection
 *   (d) Still Out-Of-Place (src -> shadow) HDF + In-Place Grain chaining 0-diff vs ARM NEON
 *       (full-frame NV16, multi-strip width > 1024, Grain-only Y-only, cropped/right-padded ROI,
 *        partial multi-plane fallback with SMALL-before-MEDIUM analysis_index)
 *   (e) Live Mode 2 NV12 HDF (Slot 41 LIVE_256_YC, 1024x680 & 1024x682) pre-clean + post-invalidate 0-diff
 *       + end-to-end filmsim_runtime triple-buffer & timeout skip-1-frame recovery
 *   (f) Z8_FS_OP_XM6_DIAG 4/4 (Grain EXACT, Grain SHARP, HDF STILL, HDF LIVE) diff_bytes == 0
 *       (real ZF_GRAIN.BIN + synthetic maps), no-HW safe ran_mask == 0, OOB sentinel, stopped/suspended guard
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include filmsim_runtime.c directly under FILMSIM_HOST_TEST so we exercise the exact
 * production runtime context, service calls, and Live Mode 2 worker step with zero edits
 * to production headers. */
#define FILMSIM_HOST_TEST_MOCK_IPC 1
#include "../src/runtime/filmsim_runtime.c"

/* Include CEVA-XM6 FilmGrain reference header (no naming collisions with z8_xm6_bridge.h) */
#include "zf_grain_xm6_ref.h"
#include "z8_zf_grain.h"

/* Declare z8_hdf_live_nv12_1024 / 1280 directly to avoid z8_hdf.h vs z8_filmsim_abi.h enum name collision */
extern int z8_hdf_live_nv12_1024(uint8_t *y, uint32_t y_stride,
                                 uint8_t *c, uint32_t c_stride,
                                 uint32_t height, uint32_t level,
                                 uint32_t monochrome,
                                 int (*cancelled)(void *), void *cancel_ctx,
                                 void *workspace, size_t workspace_bytes);
extern int z8_hdf_live_nv12_1280(uint8_t *y, uint32_t y_stride,
                                 uint8_t *c, uint32_t c_stride,
                                 uint32_t height, uint32_t level,
                                 uint32_t monochrome,
                                 int (*cancelled)(void *), void *cancel_ctx,
                                 void *workspace, size_t workspace_bytes);
#include "../src/photo/z8_native_photo_platform.h"

/* Strong host mocks overriding weak live_ipc_service_call / v71r7_lut_ipc_service_call
 * in filmsim_runtime.c to verify unified slots.state 32..79 dispatch. */
static uint32_t g_mock_lp_last_op = 0U;
static int      g_mock_lp_rc = 0;
static uint32_t g_mock_lp_resp_magic = Z8_LP_SLOTS_TUNNEL_MAGIC;
static uint32_t g_mock_lt_last_op = 0U;
static int      g_mock_lt_rc = 0;
static uint32_t g_mock_lt_resp_magic = Z8_LUT_SLOTS_TUNNEL_MAGIC;

int live_ipc_service_call(uint32_t raw_op, const void *req, uint32_t req_bytes,
                          void *resp, uint32_t *resp_bytes)
{
    (void)req;
    (void)req_bytes;
    g_mock_lp_last_op = raw_op;
    if (g_mock_lp_rc != 0) {
        if (resp_bytes) *resp_bytes = 0U;
        return g_mock_lp_rc;
    }
    if (resp && resp_bytes && *resp_bytes >= 8U) {
        uint32_t hdr[2] = { g_mock_lp_resp_magic, raw_op };
        memcpy(resp, hdr, sizeof(hdr));
        *resp_bytes = (uint32_t)sizeof(hdr);
    } else if (resp_bytes) {
        *resp_bytes = 0U;
    }
    return 0;
}

int v71r7_lut_ipc_service_call(uint32_t raw_op, const void *req, uint32_t req_bytes,
                               void *resp, uint32_t *resp_bytes)
{
    (void)req;
    (void)req_bytes;
    g_mock_lt_last_op = raw_op;
    if (g_mock_lt_rc != 0) {
        if (resp_bytes) *resp_bytes = 0U;
        return g_mock_lt_rc;
    }
    if (resp && resp_bytes && *resp_bytes >= 8U) {
        uint32_t hdr[2] = { g_mock_lt_resp_magic, raw_op };
        memcpy(resp, hdr, sizeof(hdr));
        *resp_bytes = (uint32_t)sizeof(hdr);
    } else if (resp_bytes) {
        *resp_bytes = 0U;
    }
    return 0;
}

/* Exact binary-compatible prototype for CEVA-XM6 Ricoh HDF v2 reference kernel
 * (compiled as separate TU from v71r5-xm6-hdf-kit/sim_harness/ricoh_hdf_xm6_v2.c). */
typedef struct {
    void *ctx;
    void (*dma_1d_ext_to_idm)(void *ctx, void *idm_dst, uint64_t ext_src_addr, uint32_t bytes);
    void (*dma_1d_idm_to_ext)(void *ctx, uint64_t ext_dst_addr, const void *idm_src, uint32_t bytes);
    void (*dma_2d_ext_to_idm)(void *ctx, void *idm_dst, uint32_t idm_stride,
                              uint64_t ext_src_addr, uint32_t ext_stride,
                              uint32_t width_bytes, uint32_t height_rows);
    void (*dma_2d_idm_to_ext)(void *ctx, uint64_t ext_dst_addr, uint32_t ext_stride,
                              const void *idm_src, uint32_t idm_stride,
                              uint32_t width_bytes, uint32_t height_rows);
} HostHdfDmaOps;

extern int32_t ricoh_hdf_xm6_v2_run(const Z8Xm6HdfIn *in,
                                    const Z8Xm6ImageParam *out,
                                    void *idm_workspace,
                                    size_t idm_workspace_bytes,
                                    void *ext_workspace,
                                    size_t ext_workspace_bytes,
                                    const HostHdfDmaOps *dma_ops,
                                    void *profile);

_Static_assert(sizeof(Z8Xm6ImageParam) == sizeof(tagDSPImageParam), "ImageParam ABI");
_Static_assert(sizeof(Z8Xm6GrainIn) == sizeof(TFilmGrainInputParam), "GrainIn ABI");
_Static_assert(sizeof(Z8Xm6HdfIn) == 0x60U, "HdfIn ABI");

/* ============================================================================
 * Host Runtime Stubs Required by filmsim_runtime.c under -DFILMSIM_HOST_TEST
 * ============================================================================ */

static _Atomic(void *) g_ctx_slot = NULL;
static char            g_card_drive = 'A';
static int             g_module_loaded = 1;
static uint32_t        g_active_pc_sel = 0U;
static const char     *g_zf_grain_host_path = NULL;
static uint64_t        g_plat_clock_us = 1000000ULL;

/* Host-backed Class 0x24 simulated DRAM pool (placed in Core 0 IOMMU window 0x2c) */
#define HOST_DRAM_SIZE         (16U * 1024U * 1024U) /* 16 MiB */
#define HOST_DRAM_DMA_BASE     UINT64_C(0x0840000000)
#define HOST_DRAM_DSP_BASE     UINT32_C(0x80000000)

static uint8_t *g_host_dram = NULL;
static uint32_t g_host_dram_bump = 0U;
static int      g_plat_alloc_force_oob = 0;

void *filmsim_host_alloc(uint32_t bytes)
{
    void *p = NULL;
    if (posix_memalign(&p, 64U, bytes ? bytes : 64U) != 0) {
        return NULL;
    }
    memset(p, 0, bytes ? bytes : 64U);
    return p;
}

void filmsim_host_free(void *ptr)
{
    free(ptr);
}

void filmsim_host_sleep_ms(uint32_t ms)
{
    g_plat_clock_us += (uint64_t)ms * 1000ULL;
}

char filmsim_host_card_drive(void)
{
    return g_card_drive;
}

int filmsim_host_module_loaded(void)
{
    return g_module_loaded;
}

int filmsim_host_bus_read(Z8FilmSimConfig *out_cfg, uint64_t *out_corr)
{
    (void)out_cfg;
    (void)out_corr;
    return 0;
}

uint32_t filmsim_host_read_pc_sel(void)
{
    return g_active_pc_sel;
}

int filmsim_host_read_sc_recipe(uint32_t sc_index, Z8Np3Recipe *out)
{
    (void)sc_index;
    (void)out;
    return 0;
}

_Atomic(void *) *live_ui_filmsim_context_slot(void)
{
    return &g_ctx_slot;
}

int la_native_cpu2_start(struct la_native_task *t, void (*fn)(void *), void *arg,
                         void *stack, uint32_t bytes, uint32_t prio)
{
    (void)fn;
    (void)arg;
    (void)stack;
    (void)bytes;
    (void)prio;
    if (t) {
        t->id = 1;
        t->native_error = 0;
        t->phase = 1U;
    }
    return 1;
}

void la_native_yield(void)
{
}

static int g_mock_post_roll_active = 0;

int live_runtime_post_roll_active(void)
{
    return g_mock_post_roll_active;
}

/* Host file ops serving real ZF_GRAIN.BIN when requested */
static FILE *g_host_fds[8];

static int host_file_open_read(void *opaque, const char *path, int *fd_out)
{
    (void)opaque;
    if (!path || !fd_out || !g_zf_grain_host_path) {
        return 0;
    }
    if (!strstr(path, "ZF_GRAIN.BIN")) {
        return 0;
    }
    FILE *f = fopen(g_zf_grain_host_path, "rb");
    if (!f) {
        return 0;
    }
    for (int i = 1; i < 8; ++i) {
        if (!g_host_fds[i]) {
            g_host_fds[i] = f;
            *fd_out = i;
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static int host_file_read(void *opaque, int fd, void *buf, size_t n, size_t *got)
{
    (void)opaque;
    if (fd <= 0 || fd >= 8 || !g_host_fds[fd] || !buf || !got) {
        return 0;
    }
    *got = fread(buf, 1U, n, g_host_fds[fd]);
    return 1;
}

static int host_file_close(void *opaque, int fd)
{
    (void)opaque;
    if (fd > 0 && fd < 8 && g_host_fds[fd]) {
        fclose(g_host_fds[fd]);
        g_host_fds[fd] = NULL;
    }
    return 1;
}

void live_native_file_ops(live_file_ops *ops, live_native_file_context *ctx)
{
    memset(ops, 0, sizeof(*ops));
    if (ctx) {
        memset(ctx, 0, sizeof(*ctx));
    }
    ops->opaque = ctx;
    ops->open_read = host_file_open_read;
    ops->read = host_file_read;
    ops->close = host_file_close;
}

int z8_fs_plat_pool_valid(void)
{
    return 1;
}

int z8_fs_plat_alloc_class24(uint32_t requested, struct z8_nfsp_allocation *out)
{
    if (!out || !requested || !g_host_dram) {
        return 0;
    }
    uint32_t aligned_req = (requested + 511U) & ~511U;
    uint32_t off = (g_host_dram_bump + 511U) & ~511U;
    if (off + aligned_req > HOST_DRAM_SIZE) {
        return 0;
    }
    g_host_dram_bump = off + aligned_req;
    memset(out, 0, sizeof(*out));
    out->handle = (void *)(uintptr_t)(0x24000000U + off);
    out->cpu    = g_host_dram + off;
    out->dma    = (g_plat_alloc_force_oob ? UINT64_C(0x08c0000000) : HOST_DRAM_DMA_BASE) + off;
    out->actual = aligned_req;
    out->used   = 0U;
    return 1;
}

int z8_fs_plat_alloc_class24_xm6(uint32_t requested, struct z8_nfsp_allocation *out)
{
    return z8_fs_plat_alloc_class24(requested, out);
}

void z8_fs_plat_release_class24(struct z8_nfsp_allocation *blk)
{
    if (blk) {
        memset(blk, 0, sizeof(*blk));
    }
}

int z8_fs_plat_clean_dcache(uint8_t *cpu, uint32_t bytes)
{
    (void)cpu;
    (void)bytes;
    return 1;
}

uint64_t z8_fs_plat_time_us(void)
{
    g_plat_clock_us += 10ULL;
    return g_plat_clock_us;
}

void z8_nfsp_v66_ops(struct z8_nfsp_ops *ops)
{
    memset(ops, 0, sizeof(*ops));
    ops->time_us = NULL;
}

/* ============================================================================
 * Comprehensive Mock Z8 C3.11 CEVA-XM6 Core 0 + IOMMU + D-Cache Harness
 * ============================================================================ */

typedef struct {
    uint64_t arm_base;
    uint32_t win_size;
    uint32_t dsp_base;
    uint8_t *host_backing;
    uint32_t host_cap;
} MockWin;

typedef struct {
    void    *addr;
    uint32_t bytes;
    uint32_t after_submits;
} DcacheLogEntry;

typedef enum {
    MOCK_DSP_RUN_REAL = 0,       /* Invoke real zf_grain_xm6_run / ricoh_hdf_xm6_v2_run */
    MOCK_DSP_ENQUEUE_FAIL,       /* Synchronous 0x10000000 callback (queue full) */
    MOCK_DSP_BAD_PARAM,          /* 0x10000001 callback (parameter reject) */
    MOCK_DSP_TIMEOUT_CANCEL_OK,  /* Hang until 0x40c90bec cancel, then retire callback */
    MOCK_DSP_TIMEOUT_STUCK,      /* Hang forever even after cancel (req_in_flight stays 1) */
} MockDspBehavior;

typedef struct {
    uint32_t core0_state;
    uint8_t  loader_fault;
    uint8_t  fw_loaded;

    MockWin  wins[8]; /* Indexed by (win - 0x28), covering 0x28..0x2f */

    MockDspBehavior behavior;
    MockDspBehavior slot39_override; /* Optional per-slot override (0 = use behavior) */
    int             use_slot39_override;
    uint32_t        async_delay_ticks; /* 0 = sync callback; >0 = complete after N sleep_ms(1) */
    int             scribble_dst_on_timeout;

    /* Pending async request state */
    Z8Xm6DspReq    *pending_req;
    uint32_t        pending_ticks_left;
    uint32_t        pending_result_status;

    /* Mailbox simulation (0x0a02e7eed0[0..3]):
     *   mailbox_forced_busy: 1 -> always non-idle
     *   mailbox_post_cb_busy_polls: >0 -> stays non-idle for N read_mailbox_words calls after callback */
    int             mailbox_forced_busy;
    uint32_t        mailbox_post_cb_busy_polls;
    uint32_t        mailbox_busy_countdown;
    uint32_t        mailbox_read_calls;
    int             alloc_force_fail;

    /* Telemetry & verification counters */
    uint64_t        clock_us;
    uint32_t        submit_calls;
    uint32_t        cancel_calls;
    uint32_t        sleep_calls;
    uint32_t        time_us_calls;
    uint32_t        slot39_calls;
    uint32_t        slot41_calls;
    uint32_t        ext_work_zero_violations;
    uint32_t        dcache_calls;
    uint32_t        dcache_max_chunk_seen;
    uint32_t        dcache_unaligned_violations;
    uint32_t        dcache_oversize_violations;
    DcacheLogEntry  dcache_log[256];
    uint32_t        dcache_log_count;
    /* Optional: release b->xm6_busy (and optional mailbox_forced_busy) after N sleep_ms calls */
    struct z8_xm6_bridge *release_busy_bridge;
    uint32_t        release_busy_after_sleeps;
    int             clear_mbox_busy_on_release;

    /* Dedicated XM6 Core 0 IDM (192 KiB) & fallback EXT (512 KiB) scratch arenas */
    uint8_t        *idm_ws;
    uint8_t        *ext_ws;
    uint8_t        *active_ext_ws_host;
    uint32_t        active_ext_ws_bytes;
} MockXm6Env;

static void mock_env_init(MockXm6Env *e)
{
    memset(e, 0, sizeof(*e));
    e->core0_state  = 2U;
    e->loader_fault = 0U;
    e->fw_loaded    = 1U;
    e->clock_us     = 500000ULL;

    /* Exact Z8 C3.11 Core 0 default windows 0x2a..0x2f (0x28 & 0x29 bind host heap structs) */
    /* 0x2a: 0x0a00000000 .. 0x0a0fffffff (256 MiB) -> DSP 0x50000000 */
    e->wins[2] = (MockWin){ UINT64_C(0x0a00000000), 0x10000000U, 0x50000000U, NULL, 0U };
    /* 0x2b: 0x0a10000000 .. 0x0a1fffffff (256 MiB) -> DSP 0x60000000 */
    e->wins[3] = (MockWin){ UINT64_C(0x0a10000000), 0x10000000U, 0x60000000U, NULL, 0U };
    /* 0x2c: 0x0840000000 .. 0x085fffffff (512 MiB) -> DSP 0x80000000 (backed by g_host_dram) */
    e->wins[4] = (MockWin){ HOST_DRAM_DMA_BASE, 0x20000000U, HOST_DRAM_DSP_BASE,
                            g_host_dram, HOST_DRAM_SIZE };
    /* 0x2d: 0x0860000000 .. 0x087fffffff (512 MiB) -> DSP 0xa0000000 */
    e->wins[5] = (MockWin){ UINT64_C(0x0860000000), 0x20000000U, 0xa0000000U, NULL, 0U };
    /* 0x2e: 0x0880000000 .. 0x089fffffff (512 MiB) -> DSP 0xc0000000 */
    e->wins[6] = (MockWin){ UINT64_C(0x0880000000), 0x20000000U, 0xc0000000U, NULL, 0U };
    /* 0x2f: 0x08a0000000 .. 0x08bfffffff (512 MiB) -> DSP 0xe0000000 */
    e->wins[7] = (MockWin){ UINT64_C(0x08a0000000), 0x20000000U, 0xe0000000U, NULL, 0U };

    assert(posix_memalign((void **)&e->idm_ws, 64U, 196608U) == 0);
    assert(posix_memalign((void **)&e->ext_ws, 64U, 524288U) == 0);
    memset(e->idm_ws, 0, 196608U);
    memset(e->ext_ws, 0, 524288U);
}

static void mock_env_destroy(MockXm6Env *e)
{
    free(e->idm_ws);
    free(e->ext_ws);
    e->idm_ws = NULL;
    e->ext_ws = NULL;
}

static void mock_env_bind_host_win(MockXm6Env *e, uint32_t win_id,
                                   void *host_ptr, uint32_t cap_bytes, uint32_t dsp_base)
{
    assert(win_id >= 0x28U && win_id <= 0x2FU);
    e->wins[win_id - 0x28U] = (MockWin){
        .arm_base     = (uint64_t)(uintptr_t)host_ptr,
        .win_size     = cap_bytes,
        .dsp_base     = dsp_base,
        .host_backing = (uint8_t *)host_ptr,
        .host_cap     = cap_bytes
    };
}

static uint8_t *mock_dsp_to_host(MockXm6Env *e, uint64_t addr, uint32_t bytes)
{
    /* Allow ricoh_hdf_xm6_v2_run's translated ext_ws_host window strictly during Slot 41 execution */
    if (e->active_ext_ws_host && e->active_ext_ws_bytes) {
        uintptr_t ext0 = (uintptr_t)e->active_ext_ws_host;
        if (addr >= (uint64_t)ext0 && addr <= (uint64_t)ext0 + e->active_ext_ws_bytes &&
            bytes <= e->active_ext_ws_bytes - (uint32_t)(addr - (uint64_t)ext0)) {
            return (uint8_t *)(uintptr_t)addr;
        }
    }

    /* Otherwise MUST be a valid 32-bit DSP VA >= 0x40000000 inside a mapped window */
    assert(addr >= UINT64_C(0x40000000) && addr <= UINT64_C(0xffffffff));
    uint32_t dsp = (uint32_t)addr;
    for (uint32_t i = 0U; i < 8U; ++i) {
        MockWin *w = &e->wins[i];
        if (!w->host_backing || !w->host_cap) {
            continue;
        }
        if (dsp >= w->dsp_base && (uint64_t)(dsp - w->dsp_base) + bytes <= w->host_cap) {
            return w->host_backing + (dsp - w->dsp_base);
        }
    }
    fprintf(stderr, "FATAL: Unmapped DSP VA 0x%08x (bytes=%u)\n", dsp, bytes);
    abort();
}

/* Global active env pointer for zf_grain_xm6_run's context-less Xm6DmaOps */
static MockXm6Env *g_active_grain_env = NULL;

static void mock_grain_dma_r(void *idm_dst, uint64_t ext_src_addr, uint32_t bytes)
{
    const uint8_t *src = mock_dsp_to_host(g_active_grain_env, ext_src_addr, bytes);
    memcpy(idm_dst, src, bytes);
}

static void mock_grain_dma_w(uint64_t ext_dst_addr, const void *idm_src, uint32_t bytes)
{
    uint8_t *dst = mock_dsp_to_host(g_active_grain_env, ext_dst_addr, bytes);
    memcpy(dst, idm_src, bytes);
}

static void mock_hdf_dma_1d_r(void *ctx, void *idm_dst, uint64_t ext_src_addr, uint32_t bytes)
{
    MockXm6Env *e = (MockXm6Env *)ctx;
    const uint8_t *src = mock_dsp_to_host(e, ext_src_addr, bytes);
    memcpy(idm_dst, src, bytes);
}

static void mock_hdf_dma_1d_w(void *ctx, uint64_t ext_dst_addr, const void *idm_src, uint32_t bytes)
{
    MockXm6Env *e = (MockXm6Env *)ctx;
    uint8_t *dst = mock_dsp_to_host(e, ext_dst_addr, bytes);
    memcpy(dst, idm_src, bytes);
}

static uint32_t mock_read_state(void *ctx) { return ((MockXm6Env *)ctx)->core0_state; }
static uint8_t  mock_read_fault(void *ctx) { return ((MockXm6Env *)ctx)->loader_fault; }
static uint8_t  mock_read_fw(void *ctx)    { return ((MockXm6Env *)ctx)->fw_loaded; }

static uint64_t mock_iommu_base(void *ctx, uint32_t win)
{
    MockXm6Env *e = (MockXm6Env *)ctx;
    if (win < 0x28U || win > 0x2FU) return 0U;
    return e->wins[win - 0x28U].arm_base;
}

static uint32_t mock_iommu_size(void *ctx, uint32_t win)
{
    MockXm6Env *e = (MockXm6Env *)ctx;
    if (win < 0x28U || win > 0x2FU) return 0U;
    return e->wins[win - 0x28U].win_size;
}

static uint32_t mock_iommu_xlat(void *ctx, uint32_t win, uint64_t dma_va, uint32_t *out_dsp)
{
    MockXm6Env *e = (MockXm6Env *)ctx;
    if (win < 0x28U || win > 0x2FU) return 3U;
    MockWin *w = &e->wins[win - 0x28U];
    if (!w->win_size || dma_va < w->arm_base || dma_va - w->arm_base >= w->win_size) {
        return 3U;
    }
    *out_dsp = w->dsp_base + (uint32_t)(dma_va - w->arm_base);
    return 0U;
}

static void mock_dcache_civac(void *ctx, void *addr, uint32_t bytes)
{
    MockXm6Env *e = (MockXm6Env *)ctx;
    ++e->dcache_calls;
    if (bytes > e->dcache_max_chunk_seen) {
        e->dcache_max_chunk_seen = bytes;
    }
    if ((((uintptr_t)addr) & 63U) != 0U || (bytes & 63U) != 0U) {
        ++e->dcache_unaligned_violations;
    }
    if (bytes == 0U || bytes > Z8_XM6_DCACHE_MAX_CHUNK) {
        ++e->dcache_oversize_violations;
    }
    if (e->dcache_log_count < 256U) {
        e->dcache_log[e->dcache_log_count++] = (DcacheLogEntry){
            .addr = addr,
            .bytes = bytes,
            .after_submits = e->submit_calls
        };
    }
}

static uint32_t mock_execute_real_dsp(MockXm6Env *e, Z8Xm6DspReq *req)
{
    assert(req->core_id == 0U);
    assert(req->channel == 0U);
    assert(req->callback != NULL);
    assert(req->dsp_status == Z8_XM6_DSP_STATUS_SENTINEL);
    assert(req->in_param >= UINT64_C(0x40000000) && req->in_param <= UINT64_C(0xffffffff));
    assert(req->out_param >= UINT64_C(0x40000000) && req->out_param <= UINT64_C(0xffffffff));

    if (req->slot_id == Z8_XM6_SLOT_GRAIN_STILL || req->slot_id == Z8_XM6_SLOT_GRAIN_LIVE) {
        ++e->slot39_calls;
        const TFilmGrainInputParam *gin =
            (const TFilmGrainInputParam *)mock_dsp_to_host(e, req->in_param, sizeof(*gin));
        const TFilmGrainOutputParam *gout =
            (const TFilmGrainOutputParam *)mock_dsp_to_host(e, req->out_param, sizeof(*gout));
        Xm6DmaOps dops = {
            .dma_ext_to_idm = mock_grain_dma_r,
            .dma_idm_to_ext = mock_grain_dma_w
        };
        g_active_grain_env = e;
        int32_t rc = zf_grain_xm6_run(gin, gout, e->idm_ws, XM6_ZF_IDM_WORKSPACE_BYTES, &dops);
        g_active_grain_env = NULL;
        e->clock_us += 1200ULL;
        return (uint32_t)rc;
    }

    if (req->slot_id == Z8_XM6_SLOT_HDF) {
        ++e->slot41_calls;
        const Z8Xm6HdfIn *hin =
            (const Z8Xm6HdfIn *)mock_dsp_to_host(e, req->in_param, sizeof(*hin));
        const Z8Xm6ImageParam *hout =
            (const Z8Xm6ImageParam *)mock_dsp_to_host(e, req->out_param, sizeof(*hout));
        /* P0 Silicon Safety: ALWAYS enforce dedicated 64B-aligned 512 KiB ext_work_addr != 0
         * (avoids clobbering Nikon FreeRTOS tsk0esc/tsk1esc @ 0x41940000..0x419A0000!) */
        uint8_t *ext_ws_host = e->ext_ws;
        uint32_t ext_ws_bytes = 524288U;
        if (hin->ext_work_addr == 0U || (hin->ext_work_addr & 63U) != 0U ||
            hin->ext_work_bytes < Z8_XM6_HDF_EXT_WS_BYTES) {
            ++e->ext_work_zero_violations;
        } else {
            ext_ws_host = mock_dsp_to_host(e, hin->ext_work_addr, hin->ext_work_bytes);
            ext_ws_bytes = hin->ext_work_bytes;
        }
        HostHdfDmaOps dops;
        memset(&dops, 0, sizeof(dops));
        dops.ctx = e;
        dops.dma_1d_ext_to_idm = mock_hdf_dma_1d_r;
        dops.dma_1d_idm_to_ext = mock_hdf_dma_1d_w;
        e->active_ext_ws_host  = ext_ws_host;
        e->active_ext_ws_bytes = ext_ws_bytes;
        int32_t rc = ricoh_hdf_xm6_v2_run(hin, hout,
                                          e->idm_ws, 196608U,
                                          ext_ws_host, ext_ws_bytes,
                                          &dops, NULL);
        e->active_ext_ws_host  = NULL;
        e->active_ext_ws_bytes = 0U;
        e->clock_us += 3500ULL;
        return (uint32_t)rc;
    }

    return Z8_XM6_DSP_ERR_BAD_PARAM;
}

static void mock_scribble_dst(MockXm6Env *e, Z8Xm6DspReq *req)
{
    const Z8Xm6ImageParam *out =
        (const Z8Xm6ImageParam *)mock_dsp_to_host(e, req->out_param, sizeof(*out));
    if (out->plane_addr[0] && out->width > 0 && out->height > 0 && out->stride[0] >= out->width) {
        uint8_t *y = mock_dsp_to_host(e, out->plane_addr[0], (uint32_t)out->stride[0]);
        memset(y, 0xEE, (size_t)out->width);
    }
}

static void mock_async_submit(void *ctx, Z8Xm6DspReq *req)
{
    MockXm6Env *e = (MockXm6Env *)ctx;
    ++e->submit_calls;
    assert(req && req->callback != NULL);

    MockDspBehavior beh = e->behavior;
    if (e->use_slot39_override &&
        (req->slot_id == Z8_XM6_SLOT_GRAIN_STILL || req->slot_id == Z8_XM6_SLOT_GRAIN_LIVE)) {
        beh = e->slot39_override;
    }

    if (beh == MOCK_DSP_ENQUEUE_FAIL) {
        req->dsp_status = Z8_XM6_DSP_ERR_ABORT;
        req->callback(UINT32_C(0x42180003), req);
        return;
    }
    if (beh == MOCK_DSP_BAD_PARAM) {
        req->dsp_status = Z8_XM6_DSP_ERR_BAD_PARAM;
        req->callback(UINT32_C(0x42180003), req);
        return;
    }
    if (beh == MOCK_DSP_TIMEOUT_CANCEL_OK || beh == MOCK_DSP_TIMEOUT_STUCK) {
        if (e->scribble_dst_on_timeout) {
            mock_scribble_dst(e, req);
        }
        e->pending_req = req;
        e->pending_ticks_left = UINT32_MAX; /* Never completes on its own */
        return;
    }

    uint32_t st = mock_execute_real_dsp(e, req);
    if (e->async_delay_ticks == 0U) {
        req->dsp_status = st;
        e->mailbox_busy_countdown = e->mailbox_post_cb_busy_polls;
        req->callback(UINT32_C(0x42180003), req);
    } else {
        e->pending_req = req;
        e->pending_ticks_left = e->async_delay_ticks;
        e->pending_result_status = st;
    }
}

static void mock_async_cancel(void *ctx, Z8Xm6DspReq *req)
{
    MockXm6Env *e = (MockXm6Env *)ctx;
    ++e->cancel_calls;
    MockDspBehavior beh = e->behavior;
    if (e->use_slot39_override && req &&
        (req->slot_id == Z8_XM6_SLOT_GRAIN_STILL || req->slot_id == Z8_XM6_SLOT_GRAIN_LIVE)) {
        beh = e->slot39_override;
    }
    if (e->pending_req == req && beh == MOCK_DSP_TIMEOUT_CANCEL_OK) {
        e->pending_req = NULL;
        req->dsp_status = Z8_XM6_DSP_ERR_ABORT;
        req->callback(UINT32_C(0x42180003), req);
    }
}

static void mock_sleep_ms(void *ctx, uint32_t ms)
{
    MockXm6Env *e = (MockXm6Env *)ctx;
    ++e->sleep_calls;
    e->clock_us += (uint64_t)ms * 1000ULL;
    if (e->release_busy_bridge && e->release_busy_after_sleeps > 0U) {
        --e->release_busy_after_sleeps;
        if (e->release_busy_after_sleeps == 0U) {
            if (e->clear_mbox_busy_on_release) {
                e->mailbox_forced_busy = 0;
                e->clear_mbox_busy_on_release = 0;
            }
            __atomic_store_n(&e->release_busy_bridge->xm6_busy, 0U, __ATOMIC_RELEASE);
            e->release_busy_bridge = NULL;
        }
    }
    if (e->pending_req && e->pending_ticks_left != UINT32_MAX) {
        if (e->pending_ticks_left <= ms) {
            Z8Xm6DspReq *r = e->pending_req;
            e->pending_req = NULL;
            e->pending_ticks_left = 0U;
            r->dsp_status = e->pending_result_status;
            e->mailbox_busy_countdown = e->mailbox_post_cb_busy_polls;
            r->callback(UINT32_C(0x42180003), r);
        } else {
            e->pending_ticks_left -= ms;
        }
    }
}

static uint64_t mock_time_us(void *ctx)
{
    MockXm6Env *e = (MockXm6Env *)ctx;
    ++e->time_us_calls;
    e->clock_us += 5ULL;
    return e->clock_us;
}

static int mock_alloc_c24(void *ctx, uint32_t req, struct z8_nfsp_allocation *out)
{
    MockXm6Env *e = (MockXm6Env *)ctx;
    if (e && e->alloc_force_fail) {
        if (out) {
            memset(out, 0, sizeof(*out));
        }
        return 0;
    }
    return z8_fs_plat_alloc_class24(req, out);
}

static void mock_release_c24(void *ctx, struct z8_nfsp_allocation *blk)
{
    (void)ctx;
    z8_fs_plat_release_class24(blk);
}

static void mock_read_mailbox_words(void *ctx, uint32_t out_words[4])
{
    MockXm6Env *e = (MockXm6Env *)ctx;
    ++e->mailbox_read_calls;
    if (e->mailbox_forced_busy) {
        out_words[0] = 0x11111111U;
        out_words[1] = 0x22222222U;
        out_words[2] = Z8_XM6_MAILBOX_IDLE_W2;
        out_words[3] = Z8_XM6_MAILBOX_IDLE_W3;
        return;
    }
    if (e->mailbox_busy_countdown > 0U) {
        --e->mailbox_busy_countdown;
        out_words[0] = 0x41000000U;
        out_words[1] = 0x00000000U;
        out_words[2] = Z8_XM6_MAILBOX_IDLE_W2;
        out_words[3] = Z8_XM6_MAILBOX_IDLE_W3;
        return;
    }
    out_words[0] = Z8_XM6_MAILBOX_IDLE_W0;
    out_words[1] = Z8_XM6_MAILBOX_IDLE_W1;
    out_words[2] = Z8_XM6_MAILBOX_IDLE_W2;
    out_words[3] = Z8_XM6_MAILBOX_IDLE_W3;
}

static Z8Xm6HwOps mock_hw_ops(MockXm6Env *e)
{
    Z8Xm6HwOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.read_core0_state   = mock_read_state;
    ops.read_loader_fault  = mock_read_fault;
    ops.read_fw_loaded     = mock_read_fw;
    ops.iommu_win_base     = mock_iommu_base;
    ops.iommu_win_size     = mock_iommu_size;
    ops.iommu_xlat         = mock_iommu_xlat;
    ops.dcache_civac       = mock_dcache_civac;
    ops.async_submit       = mock_async_submit;
    ops.async_cancel       = mock_async_cancel;
    ops.sleep_ms           = mock_sleep_ms;
    ops.time_us            = mock_time_us;
    ops.alloc_class24      = mock_alloc_c24;
    ops.release_class24    = mock_release_c24;
    ops.read_mailbox_words = mock_read_mailbox_words;
    ops.ctx                = e;
    return ops;
}

static void reset_host_dram_bump(struct z8_xm6_bridge *b, uint32_t new_bump)
{
    if (b) {
        z8_xm6_release_ext_ws(b);
    }
    g_host_dram_bump = new_bump;
}

/* ============================================================================
 * Helpers: Load Real ZF_GRAIN.BIN, Scene Generation, Simulated Photo Harness
 * ============================================================================ */

static void load_real_zf_grain(uint8_t *raw_out, z8_zf_grain_maps *maps_out)
{
    const char *path =
        "/Users/ycx/Nikon/build-tools/livediag/feat-onemov/v71r5-xm6-dsp-kit/extracted/ZF_GRAIN.BIN";
    g_zf_grain_host_path = path;
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    size_t got = fread(raw_out, 1U, Z8_XM6_ZFG1_FILE_BYTES, f);
    fclose(f);
    assert(got == Z8_XM6_ZFG1_FILE_BYTES);
    assert(z8_zf_grain_validate_bin(raw_out, got, maps_out) == Z8_ZF_GRAIN_OK);
}

static void fill_scene_nv(uint8_t *y, uint8_t *c, uint32_t w, uint32_t h,
                          uint32_t y_st, uint32_t c_st, int is_nv12, uint32_t tag)
{
    for (uint32_t r = 0U; r < h; ++r) {
        uint8_t *yr = y + (size_t)r * y_st;
        for (uint32_t x = 0U; x < w; ++x) {
            uint32_t v;
            if ((x >= w / 8U && x < w / 4U && r >= h / 8U && r < h / 4U) ||
                (x >= (w * 5U) / 8U && x < (w * 6U) / 8U && r >= h / 3U && r < h / 2U)) {
                v = 240U + ((x + r + tag) & 15U);
            } else if (x < w / 3U && r >= (h * 2U) / 3U) {
                v = 10U + ((x ^ r ^ tag) & 15U);
            } else {
                v = ((x * 5U + r * 3U + tag * 13U) ^ ((x >> 2) * 19U)) & 255U;
            }
            yr[x] = (uint8_t)v;
        }
    }
    if (c) {
        uint32_t ch = is_nv12 ? ((h + 1U) >> 1) : h;
        for (uint32_t r = 0U; r < ch; ++r) {
            uint8_t *cr = c + (size_t)r * c_st;
            for (uint32_t x = 0U; x < w; x += 2U) {
                cr[x]      = (uint8_t)(72U + (((x * 3U + r * 7U + tag) + ((x & 32U) ? 64U : 0U)) & 111U));
                cr[x + 1U] = (uint8_t)(80U + (((x * 5U + r * 3U + tag) + ((r & 16U) ? 56U : 0U)) & 111U));
            }
        }
    }
}

static uint32_t count_diff_2d(const uint8_t *a, const uint8_t *b,
                              uint32_t w, uint32_t h, uint32_t stride)
{
    uint32_t diff = 0U;
    for (uint32_t r = 0U; r < h; ++r) {
        for (uint32_t x = 0U; x < w; ++x) {
            if (a[(size_t)r * stride + x] != b[(size_t)r * stride + x]) {
                ++diff;
            }
        }
    }
    return diff;
}

/* ---- Simulated Z8 Photo Memory Image for End-to-End z8_nfsp_worker Tests ---- */

#define PHOTO_MEM_BASE  UINT64_C(0x0a00100000)
#define PHOTO_MEM_SIZE  (64U * 1024U)

typedef struct {
    uint8_t                meta_mem[PHOTO_MEM_SIZE];
    uint64_t               ctx_addr;
    uint64_t               outer_addr;
    uint64_t               meta_addr;
    struct z8_xm6_bridge  *xm6;
    const z8_zf_grain_maps *zf_maps;
    const void            *zf_raw_buf;
    uint32_t               map_source_calls;
    uint32_t               clean_calls;
    uint32_t               alloc_calls;
    uint32_t               release_calls;
    int                    force_shadow_oob_after; /* >0: force OOB on alloc N */
} PhotoSim;

static void pmem_put16(PhotoSim *s, uint64_t a, uint16_t v)
{
    assert(a >= PHOTO_MEM_BASE && a + 2U <= PHOTO_MEM_BASE + PHOTO_MEM_SIZE);
    uint8_t *p = s->meta_mem + (a - PHOTO_MEM_BASE);
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8);
}

static void pmem_put32(PhotoSim *s, uint64_t a, uint32_t v)
{
    assert(a >= PHOTO_MEM_BASE && a + 4U <= PHOTO_MEM_BASE + PHOTO_MEM_SIZE);
    uint8_t *p = s->meta_mem + (a - PHOTO_MEM_BASE);
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)(v >> 24);
}

static void pmem_put64(PhotoSim *s, uint64_t a, uint64_t v)
{
    pmem_put32(s, a, (uint32_t)(v & UINT32_C(0xffffffff)));
    pmem_put32(s, a + 4U, (uint32_t)(v >> 32));
}

static void pmem_write_desc(PhotoSim *s, uint64_t a,
                            uint16_t y_st, uint16_t c_st,
                            uint16_t sw, uint16_t sh,
                            uint16_t w, uint16_t h,
                            uint16_t ox, uint16_t oy,
                            uint64_t y_dma, uint64_t c_dma)
{
    pmem_put32(s, a + 0U, 1U);  /* format = NV16 */
    pmem_put32(s, a + 4U, 0U);  /* layout = 0 */
    pmem_put16(s, a + 8U, y_st);
    pmem_put16(s, a + 10U, c_st);
    pmem_put16(s, a + 12U, sw);
    pmem_put16(s, a + 14U, sh);
    pmem_put16(s, a + 16U, w);
    pmem_put16(s, a + 18U, h);
    pmem_put16(s, a + 20U, ox);
    pmem_put16(s, a + 22U, oy);
    pmem_put64(s, a + 24U, y_dma);
    pmem_put64(s, a + 32U, c_dma);
}

static int psim_read(void *ctx, uint64_t addr, void *dst, size_t n)
{
    PhotoSim *s = (PhotoSim *)ctx;
    if (addr >= PHOTO_MEM_BASE && addr + n <= PHOTO_MEM_BASE + PHOTO_MEM_SIZE) {
        memcpy(dst, s->meta_mem + (addr - PHOTO_MEM_BASE), n);
        return 1;
    }
    return 0;
}

static int psim_alloc(void *ctx, uint32_t req, struct z8_nfsp_allocation *out)
{
    PhotoSim *s = (PhotoSim *)ctx;
    ++s->alloc_calls;
    int saved = g_plat_alloc_force_oob;
    if (s->force_shadow_oob_after > 0 && (int)s->alloc_calls >= s->force_shadow_oob_after) {
        g_plat_alloc_force_oob = 1;
    }
    int ok = z8_fs_plat_alloc_class24(req, out);
    g_plat_alloc_force_oob = saved;
    return ok;
}

static void psim_release(void *ctx, struct z8_nfsp_allocation *blk)
{
    PhotoSim *s = (PhotoSim *)ctx;
    if (s && blk && (blk->handle || blk->cpu || blk->dma)) {
        ++s->release_calls;
    }
    z8_fs_plat_release_class24(blk);
}

static const uint8_t *psim_map_source(void *ctx, uint64_t dma, uint32_t bytes)
{
    PhotoSim *s = (PhotoSim *)ctx;
    ++s->map_source_calls;
    uint64_t base = (dma >= UINT64_C(0x08c0000000)) ? UINT64_C(0x08c0000000) : HOST_DRAM_DMA_BASE;
    assert(dma >= base && (dma - base) + bytes <= HOST_DRAM_SIZE);
    return g_host_dram + (dma - base);
}

static int psim_clean(void *ctx, uint8_t *cpu, uint32_t bytes)
{
    PhotoSim *s = (PhotoSim *)ctx;
    (void)cpu;
    (void)bytes;
    ++s->clean_calls;
    return 1;
}

static int psim_pool_valid(void *ctx) { (void)ctx; return 1; }
static void psim_yield(void *ctx)     { (void)ctx; }
static uint64_t psim_time_us(void *ctx) { (void)ctx; return z8_fs_plat_time_us(); }

static int psim_acquire_assets(void *ctx, z8_filmsim_assets *out)
{
    PhotoSim *s = (PhotoSim *)ctx;
    memset(out, 0, sizeof(*out));
    out->zf_maps = s->zf_maps;
    out->xm6_bridge = s->xm6;
    out->xm6_zf_raw_buf = s->zf_raw_buf;
    return 1;
}

static void psim_release_assets(void *ctx) { (void)ctx; }

typedef struct {
    uint32_t role;
    uint16_t w, h, sw, sh, y_st, c_st, ox, oy;
    uint64_t y_dma, c_dma;
    uint32_t y_off, c_off;
    uint32_t block_actual;
    uint64_t block_base;
} PhotoPlaneSpec;

static void psim_build_shot(PhotoSim *s, const PhotoPlaneSpec *specs, uint32_t nspecs)
{
    memset(s->meta_mem, 0, sizeof(s->meta_mem));
    s->ctx_addr   = PHOTO_MEM_BASE + 0x0000U;
    s->outer_addr = PHOTO_MEM_BASE + 0x1000U;
    s->meta_addr  = PHOTO_MEM_BASE + 0x2000U;

    pmem_put32(s, s->ctx_addr + 0x00U, 0x11223344U);
    pmem_put64(s, s->ctx_addr + 0x18U, s->outer_addr);
    pmem_put32(s, s->outer_addr + 0x10U, 0x55667788U);
    pmem_put64(s, s->outer_addr + 0x790U, s->meta_addr);
    pmem_put64(s, s->outer_addr + 0x6a8U, PHOTO_MEM_BASE + 0x2800U);
    pmem_put32(s, s->meta_addr + 0x00U, 1U);
    pmem_put32(s, s->meta_addr + 0x294U, 2U);
    pmem_put32(s, s->meta_addr + 0x2fcU, 3U);

    static const uint16_t outer_offs[6] = { 0x688, 0x690, 0x698, 0x6a0, 0x6a0, 0x6a0 };
    static const uint8_t  desc_offs[6]  = { 0x30,  0x30,  0x30,  0x20,  0x48,  0x70  };
    static const uint8_t  flag_offs[6]  = { 0x28,  0x28,  0x28,  0x18,  0x19,  0x1a  };

    uint64_t shared_res345 = PHOTO_MEM_BASE + 0x3600U;
    for (uint32_t i = 0U; i < nspecs; ++i) {
        const PhotoPlaneSpec *sp = &specs[i];
        uint64_t res = (sp->role >= 3U) ? shared_res345 : (PHOTO_MEM_BASE + 0x3000U + sp->role * 0x200U);
        uint64_t blk = PHOTO_MEM_BASE + 0x4000U + i * 0x40U;
        if (sp->role >= 3U) {
            /* Shared resource for SMALL/MEDIUM/THUMBNAIL shares block pointer at res+8 */
            blk = PHOTO_MEM_BASE + 0x4000U + 3U * 0x40U;
        }
        pmem_put64(s, s->outer_addr + outer_offs[sp->role], res);
        pmem_put64(s, res + 8U, blk);
        pmem_put64(s, blk + 0U, sp->block_base);
        pmem_put32(s, blk + 8U, sp->block_actual);
        pmem_write_desc(s, res + desc_offs[sp->role],
                        sp->y_st, sp->c_st, sp->sw, sp->sh,
                        sp->w, sp->h, sp->ox, sp->oy,
                        sp->y_dma, sp->c_dma);
        s->meta_mem[(res + flag_offs[sp->role]) - PHOTO_MEM_BASE] = 1U;
    }
}

static void run_nfsp_shot(struct z8_native_filmsim_photo *p, PhotoSim *s,
                          const Z8FilmSimConfig *cfg, uint32_t shot_seq)
{
    struct z8_nfsp_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.read           = psim_read;
    ops.allocate       = psim_alloc;
    ops.release        = psim_release;
    ops.map_source     = psim_map_source;
    ops.clean          = psim_clean;
    ops.pool_valid     = psim_pool_valid;
    ops.yield          = psim_yield;
    ops.time_us        = psim_time_us;
    ops.acquire_assets = psim_acquire_assets;
    ops.release_assets = psim_release_assets;
    ops.context        = s;

    z8_nfsp_init(p, &ops);
    assert(z8_nfsp_begin(p, s->ctx_addr, cfg, shot_seq) == 1);
    z8_nfsp_wait_return(p, s->outer_addr, 0x80000U, 1U);
    z8_nfsp_wait_return(p, s->outer_addr, 0x40000U, 1U);
    z8_nfsp_wait_return(p, s->outer_addr, 0x200000U, 1U);
    z8_nfsp_wait_return(p, s->outer_addr, 0x40U, 1U);
    z8_nfsp_thumbnail_return(p, s->outer_addr, 1U);
    uint64_t g = z8_nfsp_context_generation(p, s->ctx_addr);
    assert(g != 0U);
    assert(z8_nfsp_prepare_generation(p, s->ctx_addr, g, 100000U, 5000000U) == 1);
    z8_nfsp_worker(p);
    if (p->phase != Z8_NFSP_READY) {
        fprintf(stderr, "run_nfsp_shot failed: shot_seq=%u phase=%u reason=%u kernel_result=%d\n",
                shot_seq, p->phase, p->reason, p->kernel_result);
    }
    assert(p->phase == Z8_NFSP_READY);
}

/* ============================================================================
 * Test Group (a): QEMU state!=2 / loader_fault!=0 / xm6_mode=ARM_ONLY fallback
 * ============================================================================ */

static void test_group_a_hw_gate_and_qemu_fallback(const uint8_t *zf_raw,
                                                   const z8_zf_grain_maps *zf_maps)
{
    printf("[Test (a)] QEMU state!=2 / loader_fault!=0 / fw_loaded==0 / ARM_ONLY zero-latency fallback...\n");
    MockXm6Env env;
    mock_env_init(&env);
    Z8Xm6HwOps hw = mock_hw_ops(&env);

    struct z8_xm6_bridge *b = (struct z8_xm6_bridge *)filmsim_host_alloc(sizeof(*b));
    z8_xm6_bridge_init(b, &hw);
    mock_env_bind_host_win(&env, 0x28U, b, (uint32_t)sizeof(*b), 0x41000000U);

    /* Ready when state==2, loader_fault==0, fw_loaded==1, mode==AUTO */
    assert(z8_xm6_core0_hw_only_ready(b) == 1);
    assert(z8_xm6_core0_hw_ready(b, Z8_XM6_MODE_AUTO) == 1);
    assert(z8_xm6_core0_hw_ready(b, Z8_XM6_MODE_ARM_ONLY) == 0);

    /* QEMU: state=0 or state=1 -> hw_ready==0 */
    env.core0_state = 0U;
    assert(z8_xm6_core0_hw_ready(b, Z8_XM6_MODE_AUTO) == 0);
    env.core0_state = 1U;
    assert(z8_xm6_core0_hw_ready(b, Z8_XM6_MODE_AUTO) == 0);
    env.core0_state = 3U;
    assert(z8_xm6_core0_hw_ready(b, Z8_XM6_MODE_AUTO) == 0);

    /* loader_fault != 0 -> hw_ready==0 & status bit7 set */
    env.core0_state = 2U;
    env.loader_fault = 1U;
    assert(z8_xm6_core0_hw_ready(b, Z8_XM6_MODE_AUTO) == 0);
    Z8FilmSimStatus st;
    memset(&st, 0, sizeof(st));
    z8_xm6_fill_status(b, Z8_XM6_MODE_AUTO, &st);
    assert(st.xm6_hw_state == (2U | Z8_XM6_HW_LOADER_FAULT_BIT));
    assert((st.xm6_runtime_flags & Z8_XM6_FLAG_HW_READY) == 0U);
    assert((st.xm6_runtime_flags & Z8_XM6_FLAG_AUTO_ENABLED) != 0U);

    /* fw_loaded == 0 -> hw_ready==0 */
    env.loader_fault = 0U;
    env.fw_loaded = 0U;
    assert(z8_xm6_core0_hw_ready(b, Z8_XM6_MODE_AUTO) == 0);

    /* Live & Still calls under QEMU state=0 do 0 submits, 0 dcache, 0 sleeps */
    env.core0_state = 0U;
    env.fw_loaded = 1U;
    assert(z8_xm6_live_hdf_nv12(b, Z8_XM6_MODE_AUTO, g_host_dram, g_host_dram + 700000U,
                                HOST_DRAM_DMA_BASE, HOST_DRAM_DMA_BASE + 700000U,
                                1024U, 680U, 1024U, 1024U, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_FALLBACK);
    assert(z8_xm6_still_hdf(b, Z8_XM6_MODE_AUTO,
                            HOST_DRAM_DMA_BASE, HOST_DRAM_DMA_BASE + 65536U,
                            HOST_DRAM_DMA_BASE + 131072U, HOST_DRAM_DMA_BASE + 196608U,
                            128U, 128U, 128U, 128U, 16384U, 16384U,
                            Z8_HDF_MEDIUM, Z8_XM6_FMT_NV16, NULL) == Z8_XM6_STILL_FALLBACK_CLEAN);
    assert(env.submit_calls == 0U && env.dcache_calls == 0U && env.sleep_calls == 0U);
    assert(b->fallback_cnt == 2U && b->last_err == Z8_XM6_ERR_NOT_READY);

    /* ARM_ONLY mode when HW IS ready: 0 submits, 0 fallback_cnt increment */
    env.core0_state = 2U;
    uint32_t prev_fb = b->fallback_cnt;
    assert(z8_xm6_live_hdf_nv12(b, Z8_XM6_MODE_ARM_ONLY, g_host_dram, g_host_dram + 700000U,
                                HOST_DRAM_DMA_BASE, HOST_DRAM_DMA_BASE + 700000U,
                                1024U, 680U, 1024U, 1024U, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_FALLBACK);
    assert(env.submit_calls == 0U && env.dcache_calls == 0U && b->fallback_cnt == prev_fb);

    /* End-to-end Still photo under QEMU state=0 vs ARM_ONLY: identical ARM NEON output */
    g_host_dram_bump = 0U;
    uint32_t w = 128U, h = 128U, y_b = w * h, c_b = w * h;
    uint32_t src_y_off = 0U, src_c_off = y_b;
    g_host_dram_bump = 2U * y_b;
    fill_scene_nv(g_host_dram + src_y_off, g_host_dram + src_c_off, w, h, w, w, 0, 11U);

    PhotoPlaneSpec sp = {
        .role = Z8_PHOTO_MAIN, .w = (uint16_t)w, .h = (uint16_t)h,
        .sw = (uint16_t)w, .sh = (uint16_t)h, .y_st = (uint16_t)w, .c_st = (uint16_t)w,
        .ox = 0U, .oy = 0U,
        .y_dma = HOST_DRAM_DMA_BASE + src_y_off, .c_dma = HOST_DRAM_DMA_BASE + src_c_off,
        .block_base = HOST_DRAM_DMA_BASE, .block_actual = 2U * y_b
    };
    Z8FilmSimConfig cfg;
    z8_filmsim_cfg_default(&cfg);
    cfg.film_style = Z8_STYLE_NIKON_ZF;
    cfg.zf_intensity = 3U;
    cfg.zf_size = 2U;
    cfg.zf_blur_mode = Z8_ZF_BLUR_SHARP;
    cfg.hdf_level = Z8_HDF_MEDIUM;
    cfg.xm6_mode = Z8_XM6_MODE_AUTO;
    z8_filmsim_cfg_finalize(&cfg, 1U);

    env.core0_state = 0U; /* QEMU */
    b->fallback_cnt = 0U;
    PhotoSim psim;
    memset(&psim, 0, sizeof(psim));
    psim.xm6 = b;
    psim.zf_maps = zf_maps;
    psim.zf_raw_buf = zf_raw;
    psim_build_shot(&psim, &sp, 1U);

    struct z8_native_filmsim_photo ph_qemu;
    run_nfsp_shot(&ph_qemu, &psim, &cfg, 7U);
    assert(env.submit_calls == 0U);
    assert(b->still_ok == 0U && b->fallback_cnt == 1U && b->last_err == Z8_XM6_ERR_NOT_READY);

    uint8_t *ref_y = (uint8_t *)malloc(y_b);
    uint8_t *ref_c = (uint8_t *)malloc(c_b);
    memcpy(ref_y, ph_qemu.planes[0].y, y_b);
    memcpy(ref_c, ph_qemu.planes[0].uv, c_b);
    z8_nfsp_retire(&ph_qemu, psim.outer_addr);

    /* Now ARM_ONLY with HW ready */
    env.core0_state = 2U;
    cfg.xm6_mode = Z8_XM6_MODE_ARM_ONLY;
    z8_filmsim_cfg_finalize(&cfg, 2U);
    b->fallback_cnt = 0U;
    struct z8_native_filmsim_photo ph_arm;
    run_nfsp_shot(&ph_arm, &psim, &cfg, 7U);
    assert(env.submit_calls == 0U && b->still_ok == 0U && b->fallback_cnt == 0U);
    assert(memcmp(ph_arm.planes[0].y, ref_y, y_b) == 0);
    assert(memcmp(ph_arm.planes[0].uv, ref_c, c_b) == 0);
    z8_nfsp_retire(&ph_arm, psim.outer_addr);

    free(ref_y);
    free(ref_c);
    filmsim_host_free(b);
    mock_env_destroy(&env);
    printf("  PASS (a)\n");
}

/* ============================================================================
 * Test Group (b): 0x0c4.. Alias Stripping, >=0x08c0000000 OOB, Chunked D-Cache
 * ============================================================================ */

static void test_group_b_iommu_and_dcache(const uint8_t *zf_raw,
                                          const z8_zf_grain_maps *zf_maps)
{
    printf("[Test (b)] 0x0c4.. alias stripping, >=0x08c0000000 IOMMU OOB, cross-window reject, <=512KiB D-Cache...\n");
    MockXm6Env env;
    mock_env_init(&env);
    Z8Xm6HwOps hw = mock_hw_ops(&env);

    struct z8_xm6_bridge *b = (struct z8_xm6_bridge *)filmsim_host_alloc(sizeof(*b));
    z8_xm6_bridge_init(b, &hw);
    mock_env_bind_host_win(&env, 0x28U, b, (uint32_t)sizeof(*b), 0x41000000U);

    uint32_t dsp = 0U;
    /* 0x0c4..0x0cb alias stripping -> matches 0x084..0x08b windows 0x2c..0x2f */
    assert(z8_xm6_core0_arm_to_dsp32(b, UINT64_C(0x0c40010000), 696320U, &dsp) == 1);
    assert(dsp == UINT32_C(0x80010000));

    assert(z8_xm6_core0_arm_to_dsp32(b, UINT64_C(0x0cbffff000), 0x1000U, &dsp) == 1);
    assert(dsp == UINT32_C(0xfffff000));

    /* Spilling 1 byte past 0x0cbfffffff into 0x0cc0000000 (>= 0x08c0000000) -> REJECT */
    assert(z8_xm6_core0_arm_to_dsp32(b, UINT64_C(0x0cbffff000), 0x1001U, &dsp) == 0);
    /* Exact >= 0x08c0000000 and its 0x0cc.. alias -> REJECT */
    assert(z8_xm6_core0_arm_to_dsp32(b, UINT64_C(0x08c0000000), 4096U, &dsp) == 0);
    assert(z8_xm6_core0_arm_to_dsp32(b, UINT64_C(0x08f0000000), 4096U, &dsp) == 0);
    assert(z8_xm6_core0_arm_to_dsp32(b, UINT64_C(0x0cc0000000), 4096U, &dsp) == 0);
    /* Cross-window boundary straddle (0x2c -> 0x2d at 0x0860000000, 0x2a -> 0x2b at 0x0a10000000) -> REJECT */
    assert(z8_xm6_core0_arm_to_dsp32(b, UINT64_C(0x085ffff800), 0x1000U, &dsp) == 0);
    assert(z8_xm6_core0_arm_to_dsp32(b, UINT64_C(0x0c5ffff800), 0x1000U, &dsp) == 0);
    assert(z8_xm6_core0_arm_to_dsp32(b, UINT64_C(0x0a0ffff800), 0x1000U, &dsp) == 0);
    /* Kernel heap 0x0a00100000 -> OK */
    assert(z8_xm6_core0_arm_to_dsp32(b, UINT64_C(0x0a00100000), 27588U, &dsp) == 1);
    assert(dsp == UINT32_C(0x50100000));
    /* Zero / overflow args -> REJECT */
    assert(z8_xm6_core0_arm_to_dsp32(b, 0U, 64U, &dsp) == 0);
    assert(z8_xm6_core0_arm_to_dsp32(b, UINT64_C(0x0a00100000), 0U, &dsp) == 0);

    /* Chunked <= 512 KiB 0x40d2b9c8 D-Cache maintenance on unaligned 2.5 MiB span */
    uint32_t span_2p5m = 2621440U + 13U;
    uint8_t *unaligned_ptr = g_host_dram + 19U;
    z8_xm6_dcache_civac(b, unaligned_ptr, span_2p5m);
    assert(env.dcache_calls == 6U); /* 5 * 512 KiB + 1 tail chunk */
    assert(env.dcache_max_chunk_seen == Z8_XM6_DCACHE_MAX_CHUNK);
    assert(env.dcache_oversize_violations == 0U);
    assert(env.dcache_unaligned_violations == 0U);
    uint64_t total_cleaned = 0U;
    for (uint32_t i = 0U; i < env.dcache_log_count; ++i) {
        total_cleaned += env.dcache_log[i].bytes;
    }
    uintptr_t exp_start = ((uintptr_t)unaligned_ptr) & ~(uintptr_t)63U;
    uintptr_t exp_end   = (((uintptr_t)unaligned_ptr) + span_2p5m + 63U) & ~(uintptr_t)63U;
    assert(total_cleaned == (uint64_t)(exp_end - exp_start));

    /* Live HDF with >= 0x08c0000000 buffer: safe OOB reject, 0 submits, 0 dcache, breaker NOT tripped */
    env.dcache_calls = 0U;
    assert(z8_xm6_live_hdf_nv12(b, Z8_XM6_MODE_AUTO, g_host_dram, g_host_dram + 700000U,
                                UINT64_C(0x08c0000000), UINT64_C(0x08c0100000),
                                1024U, 680U, 1024U, 1024U, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_FALLBACK);
    assert(env.submit_calls == 0U && env.dcache_calls == 0U);
    assert(b->last_err == Z8_XM6_ERR_IOMMU_OOB && b->breaker_tripped == 0U);

    /* End-to-end Still photo with shadow in >= 0x08c0000000: clean fallback to ARM NEON, breaker stays 0 */
    g_host_dram_bump = 0U;
    uint32_t w = 128U, h = 128U, y_b = w * h;
    g_host_dram_bump = 2U * y_b;
    fill_scene_nv(g_host_dram, g_host_dram + y_b, w, h, w, w, 0, 22U);

    PhotoPlaneSpec sp = {
        .role = Z8_PHOTO_MAIN, .w = (uint16_t)w, .h = (uint16_t)h,
        .sw = (uint16_t)w, .sh = (uint16_t)h, .y_st = (uint16_t)w, .c_st = (uint16_t)w,
        .ox = 0U, .oy = 0U,
        .y_dma = HOST_DRAM_DMA_BASE, .c_dma = HOST_DRAM_DMA_BASE + y_b,
        .block_base = HOST_DRAM_DMA_BASE, .block_actual = 2U * y_b
    };
    Z8FilmSimConfig cfg;
    z8_filmsim_cfg_default(&cfg);
    cfg.hdf_level = Z8_HDF_MEDIUM;
    cfg.xm6_mode = Z8_XM6_MODE_AUTO;
    z8_filmsim_cfg_finalize(&cfg, 1U);

    PhotoSim psim;
    memset(&psim, 0, sizeof(psim));
    psim.xm6 = b;
    psim.zf_maps = zf_maps;
    psim.zf_raw_buf = zf_raw;
    psim.force_shadow_oob_after = 1; /* Force shadow Class 0x24 block into 0x08c0000000 */
    psim_build_shot(&psim, &sp, 1U);

    struct z8_native_filmsim_photo ph;
    run_nfsp_shot(&ph, &psim, &cfg, 12U);
    assert(env.submit_calls == 0U);
    assert(b->last_err == Z8_XM6_ERR_IOMMU_OOB && b->breaker_tripped == 0U && b->still_ok == 0U);
    assert(psim.map_source_calls > 0U); /* ARM NEON fallback copied & processed cleanly */
    z8_nfsp_retire(&ph, psim.outer_addr);

    filmsim_host_free(b);
    mock_env_destroy(&env);
    printf("  PASS (b)\n");
}

/* ============================================================================
 * Test Group (c): Async Poll, Timeout -> Cancel -> Breaker -> Pristine Fallback -> Reset
 * ============================================================================ */

static void test_group_c_timeout_cancel_breaker_reset(const uint8_t *zf_raw,
                                                      const z8_zf_grain_maps *zf_maps)
{
    printf("[Test (c)] Async polling, timeout -> 0x40c90bec cancel -> breaker -> pristine fallback -> RESET_BREAKER...\n");
    MockXm6Env env;
    mock_env_init(&env);
    Z8Xm6HwOps hw = mock_hw_ops(&env);

    /* Allocate full runtime_context so we test Z8_FS_OP_XM6_RESET_BREAKER & Z8_FS_OP_STATUS via service_call_ctx */
    struct filmsim_runtime_context *rt =
        (struct filmsim_runtime_context *)filmsim_host_alloc((uint32_t)sizeof(*rt));
    rt->magic = FILMSIM_RUNTIME_MAGIC;
    rt->size  = (uint32_t)sizeof(*rt);
    z8_xm6_bridge_init(&rt->xm6, &hw);
    mock_env_bind_host_win(&env, 0x28U, rt, (uint32_t)sizeof(*rt), 0x41000000U);
    mock_env_bind_host_win(&env, 0x29U, (void *)zf_raw, FS_ZF_BUF_BYTES, 0x42000000U);

    uint32_t w = 128U, h = 128U, y_b = w * h, c_b = w * h;
    reset_host_dram_bump(&rt->xm6, 8U * y_b);
    fill_scene_nv(g_host_dram, g_host_dram + y_b, w, h, w, w, 0, 33U);
    uint8_t *pristine_src = (uint8_t *)malloc(2U * y_b);
    memcpy(pristine_src, g_host_dram, 2U * y_b);

    PhotoPlaneSpec sp = {
        .role = Z8_PHOTO_MAIN, .w = (uint16_t)w, .h = (uint16_t)h,
        .sw = (uint16_t)w, .sh = (uint16_t)h, .y_st = (uint16_t)w, .c_st = (uint16_t)w,
        .ox = 0U, .oy = 0U,
        .y_dma = HOST_DRAM_DMA_BASE, .c_dma = HOST_DRAM_DMA_BASE + y_b,
        .block_base = HOST_DRAM_DMA_BASE, .block_actual = 2U * y_b
    };

    Z8FilmSimConfig cfg;
    z8_filmsim_cfg_default(&cfg);
    cfg.film_style = Z8_STYLE_NIKON_ZF;
    cfg.zf_intensity = 4U;
    cfg.zf_size = 2U;
    cfg.zf_blur_mode = Z8_ZF_BLUR_SHARP;
    cfg.hdf_level = Z8_HDF_MEDIUM;
    cfg.xm6_mode = Z8_XM6_MODE_AUTO;
    z8_filmsim_cfg_finalize(&cfg, 1U);

    /* Compute pure ARM NEON golden reference */
    Z8FilmSimConfig cfg_arm = cfg;
    cfg_arm.xm6_mode = Z8_XM6_MODE_ARM_ONLY;
    z8_filmsim_cfg_finalize(&cfg_arm, 1U);
    PhotoSim psim_ref;
    memset(&psim_ref, 0, sizeof(psim_ref));
    psim_ref.xm6 = &rt->xm6;
    psim_ref.zf_maps = zf_maps;
    psim_ref.zf_raw_buf = zf_raw;
    psim_build_shot(&psim_ref, &sp, 1U);
    struct z8_native_filmsim_photo ph_gold;
    run_nfsp_shot(&ph_gold, &psim_ref, &cfg_arm, 99U);
    uint8_t *gold_y = (uint8_t *)malloc(y_b);
    uint8_t *gold_c = (uint8_t *)malloc(c_b);
    memcpy(gold_y, ph_gold.planes[0].y, y_b);
    memcpy(gold_c, ph_gold.planes[0].uv, c_b);
    z8_nfsp_retire(&ph_gold, psim_ref.outer_addr);

    /* (c1) Async delayed completion (completes after 3 dly_tsk(1) ticks) -> OK, 0-diff */
    env.behavior = MOCK_DSP_RUN_REAL;
    env.async_delay_ticks = 3U;
    PhotoSim psim;
    memset(&psim, 0, sizeof(psim));
    psim.xm6 = &rt->xm6;
    psim.zf_maps = zf_maps;
    psim.zf_raw_buf = zf_raw;
    psim_build_shot(&psim, &sp, 1U);
    struct z8_native_filmsim_photo ph_async;
    run_nfsp_shot(&ph_async, &psim, &cfg, 99U);
    assert(env.submit_calls == 2U && env.sleep_calls >= 6U);
    assert(rt->xm6.still_ok == 1U && rt->xm6.breaker_tripped == 0U);
    assert(memcmp(ph_async.planes[0].y, gold_y, y_b) == 0);
    assert(memcmp(ph_async.planes[0].uv, gold_c, c_b) == 0);
    z8_nfsp_retire(&ph_async, psim.outer_addr);

    /* (c2) Enqueue fail (0x10000000): clean fallback, breaker stays 0 */
    env.async_delay_ticks = 0U;
    env.behavior = MOCK_DSP_ENQUEUE_FAIL;
    rt->xm6.still_ok = 0U;
    rt->xm6.fallback_cnt = 0U;
    assert(z8_xm6_still_hdf(&rt->xm6, Z8_XM6_MODE_AUTO,
                            HOST_DRAM_DMA_BASE, HOST_DRAM_DMA_BASE + y_b,
                            HOST_DRAM_DMA_BASE + 4U * y_b, HOST_DRAM_DMA_BASE + 5U * y_b,
                            w, h, w, w, y_b, c_b, Z8_HDF_MEDIUM, Z8_XM6_FMT_NV16, NULL)
           == Z8_XM6_STILL_FALLBACK_CLEAN);
    assert(rt->xm6.breaker_tripped == 0U && rt->xm6.last_err == Z8_XM6_ERR_ENQUEUE_FAIL);

    /* (c3) BAD_PARAM (0x10000001): trips breaker, returns CLEAN fallback */
    env.behavior = MOCK_DSP_BAD_PARAM;
    assert(z8_xm6_still_hdf(&rt->xm6, Z8_XM6_MODE_AUTO,
                            HOST_DRAM_DMA_BASE, HOST_DRAM_DMA_BASE + y_b,
                            HOST_DRAM_DMA_BASE + 4U * y_b, HOST_DRAM_DMA_BASE + 5U * y_b,
                            w, h, w, w, y_b, c_b, Z8_HDF_MEDIUM, Z8_XM6_FMT_NV16, NULL)
           == Z8_XM6_STILL_FALLBACK_CLEAN);
    assert(rt->xm6.breaker_tripped == 1U && rt->xm6.last_err == Z8_XM6_ERR_DSP_ERR);
    z8_xm6_reset_breaker(&rt->xm6);
    assert(rt->xm6.breaker_tripped == 0U && rt->xm6.last_err == Z8_XM6_ERR_OK);

    /* (c4) Stage 1 Timeout -> 0x40c90bec cancel -> breaker trip -> pristine source -> ARM NEON 0-diff */
    env.behavior = MOCK_DSP_TIMEOUT_CANCEL_OK;
    env.scribble_dst_on_timeout = 1;
    env.submit_calls = 0U;
    env.cancel_calls = 0U;
    rt->xm6.still_ok = 0U;
    rt->xm6.fallback_cnt = 0U;

    struct z8_native_filmsim_photo ph_to;
    run_nfsp_shot(&ph_to, &psim, &cfg, 99U);
    assert(env.submit_calls == 1U);
    assert(env.cancel_calls == 1U);
    assert(rt->xm6.breaker_tripped == 1U);
    assert(rt->xm6.last_err == Z8_XM6_ERR_TIMEOUT);
    assert(rt->xm6.still_ok == 0U);
    /* Source MUST be 100% untouched byte-for-byte */
    assert(memcmp(g_host_dram, pristine_src, 2U * y_b) == 0);
    /* Shadow MUST be 0-diff against ARM NEON golden despite mid-execution scribble */
    assert(memcmp(ph_to.planes[0].y, gold_y, y_b) == 0);
    assert(memcmp(ph_to.planes[0].uv, gold_c, c_b) == 0);
    z8_nfsp_retire(&ph_to, psim.outer_addr);

    /* Next shot while breaker tripped: 0 submits, last_err stays TIMEOUT, 0-diff ARM NEON */
    env.behavior = MOCK_DSP_RUN_REAL;
    env.submit_calls = 0U;
    struct z8_native_filmsim_photo ph_tripped;
    run_nfsp_shot(&ph_tripped, &psim, &cfg, 99U);
    assert(env.submit_calls == 0U);
    assert(rt->xm6.breaker_tripped == 1U && rt->xm6.last_err == Z8_XM6_ERR_TIMEOUT);
    assert(memcmp(ph_tripped.planes[0].y, gold_y, y_b) == 0);
    assert(memcmp(ph_tripped.planes[0].uv, gold_c, c_b) == 0);
    z8_nfsp_retire(&ph_tripped, psim.outer_addr);

    /* Z8_FS_OP_XM6_RESET_BREAKER via filmsim_runtime_service_call_ctx */
    Z8FilmSimStatus st;
    uint32_t st_len = (uint32_t)sizeof(st);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_STATUS, NULL, 0U, &st, &st_len) == 0);
    assert((st.xm6_runtime_flags & Z8_XM6_FLAG_BREAKER_TRIPPED) != 0U);
    assert(st.xm6_last_err == Z8_XM6_ERR_TIMEOUT);

    st_len = (uint32_t)sizeof(st);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_RESET_BREAKER, NULL, 0U, &st, &st_len) == 0);
    assert(st_len == Z8_FILMSIM_STATUS_BYTES);
    assert((st.xm6_runtime_flags & Z8_XM6_FLAG_BREAKER_TRIPPED) == 0U);
    assert(st.xm6_last_err == Z8_XM6_ERR_OK);

    /* Shot after reset resumes XM6 hardware acceleration cleanly */
    env.submit_calls = 0U;
    struct z8_native_filmsim_photo ph_recovered;
    run_nfsp_shot(&ph_recovered, &psim, &cfg, 99U);
    assert(env.submit_calls == 2U && rt->xm6.still_ok == 1U);
    assert(memcmp(ph_recovered.planes[0].y, gold_y, y_b) == 0);
    assert(memcmp(ph_recovered.planes[0].uv, gold_c, c_b) == 0);
    z8_nfsp_retire(&ph_recovered, psim.outer_addr);

    /* (c5) Stage 1 HDF OK -> Stage 2 Grain Timeout (dirty rollback):
     *      source pristine, 0-diff ARM NEON fallback, and still_ok NOT falsely incremented! */
    env.behavior = MOCK_DSP_RUN_REAL;
    env.use_slot39_override = 1;
    env.slot39_override = MOCK_DSP_TIMEOUT_CANCEL_OK;
    env.scribble_dst_on_timeout = 1;
    env.submit_calls = 0U;
    env.cancel_calls = 0U;
    rt->xm6.still_ok = 0U;

    struct z8_native_filmsim_photo ph_s2_to;
    run_nfsp_shot(&ph_s2_to, &psim, &cfg, 99U);
    assert(env.submit_calls == 2U && env.cancel_calls == 1U);
    assert(rt->xm6.breaker_tripped == 1U && rt->xm6.last_err == Z8_XM6_ERR_TIMEOUT);
    assert(rt->xm6.still_ok == 0U); /* Audit Fix #3 verified! */
    assert(memcmp(g_host_dram, pristine_src, 2U * y_b) == 0);
    assert(memcmp(ph_s2_to.planes[0].y, gold_y, y_b) == 0);
    assert(memcmp(ph_s2_to.planes[0].uv, gold_c, c_b) == 0);
    z8_nfsp_retire(&ph_s2_to, psim.outer_addr);

    /* (c6) Stuck cancel (req_in_flight stays 1): V71.7-r4 returns Z8_XM6_STILL_HUNG_DMA (-2)
     *      and latches still_hung_dma=1 so caller NEVER frees shadow DMA buffer while DSP DMA
     *      may still write it! Safe RESET_BREAKER MUST refuse while req_in_flight==1. */
    env.use_slot39_override = 0;
    env.behavior = MOCK_DSP_TIMEOUT_STUCK;
    z8_xm6_reset_breaker(&rt->xm6);
    assert(z8_xm6_still_hdf(&rt->xm6, Z8_XM6_MODE_AUTO,
                            HOST_DRAM_DMA_BASE, HOST_DRAM_DMA_BASE + y_b,
                            HOST_DRAM_DMA_BASE + 4U * y_b, HOST_DRAM_DMA_BASE + 5U * y_b,
                            w, h, w, w, y_b, c_b, Z8_HDF_MEDIUM, Z8_XM6_FMT_NV16, NULL)
           == Z8_XM6_STILL_HUNG_DMA);
    assert(rt->xm6.req_in_flight == 1U);
    assert(rt->xm6.still_hung_dma == 1U);
    assert(rt->xm6.breaker_tripped == 1U);
    z8_xm6_reset_breaker(&rt->xm6);
    /* Safe reset MUST refuse while req_in_flight==1 -> breaker stays 1, err stays TIMEOUT */
    assert(rt->xm6.breaker_tripped == 1U && rt->xm6.last_err == Z8_XM6_ERR_TIMEOUT);
    assert(z8_xm6_still_hdf(&rt->xm6, Z8_XM6_MODE_AUTO,
                            HOST_DRAM_DMA_BASE, HOST_DRAM_DMA_BASE + y_b,
                            HOST_DRAM_DMA_BASE + 4U * y_b, HOST_DRAM_DMA_BASE + 5U * y_b,
                            w, h, w, w, y_b, c_b, Z8_HDF_MEDIUM, Z8_XM6_FMT_NV16, NULL)
           == Z8_XM6_STILL_FALLBACK_CLEAN);
    assert(rt->xm6.last_err == Z8_XM6_ERR_TIMEOUT);
    /* Simulate late callback retirement -> now RESET_BREAKER succeeds and clears still_hung_dma */
    assert(env.pending_req != NULL);
    env.pending_req->dsp_status = Z8_XM6_DSP_ERR_ABORT;
    env.pending_req->callback(UINT32_C(0x42180003), env.pending_req);
    env.pending_req = NULL;
    assert(rt->xm6.req_in_flight == 0U);
    z8_xm6_reset_breaker(&rt->xm6);
    assert(rt->xm6.breaker_tripped == 0U && rt->xm6.still_hung_dma == 0U && rt->xm6.last_err == Z8_XM6_ERR_OK);
    env.behavior = MOCK_DSP_RUN_REAL;
    assert(z8_xm6_still_hdf(&rt->xm6, Z8_XM6_MODE_AUTO,
                            HOST_DRAM_DMA_BASE, HOST_DRAM_DMA_BASE + y_b,
                            HOST_DRAM_DMA_BASE + 4U * y_b, HOST_DRAM_DMA_BASE + 5U * y_b,
                            w, h, w, w, y_b, c_b, Z8_HDF_MEDIUM, Z8_XM6_FMT_NV16, NULL)
           == Z8_XM6_STILL_OK);

    assert(env.ext_work_zero_violations == 0U);
    z8_xm6_release_ext_ws(&rt->xm6);
    free(pristine_src);
    free(gold_y);
    free(gold_c);
    filmsim_host_free(rt);
    mock_env_destroy(&env);
    printf("  PASS (c)\n");
}

/* ============================================================================
 * Test Group (d): Still Out-Of-Place HDF + In-Place Grain Chaining 0-diff
 * ============================================================================ */

static void test_group_d_still_hdf_grain_0diff(const uint8_t *zf_raw,
                                               const z8_zf_grain_maps *zf_maps)
{
    printf("[Test (d)] Still Out-Of-Place (src->shadow) HDF + In-Place Grain chaining 0-diff...\n");
    MockXm6Env env;
    mock_env_init(&env);
    Z8Xm6HwOps hw = mock_hw_ops(&env);

    struct z8_xm6_bridge *b = (struct z8_xm6_bridge *)filmsim_host_alloc(sizeof(*b));
    z8_xm6_bridge_init(b, &hw);
    mock_env_bind_host_win(&env, 0x28U, b, (uint32_t)sizeof(*b), 0x41000000U);
    mock_env_bind_host_win(&env, 0x29U, (void *)zf_raw, FS_ZF_BUF_BYTES, 0x42000000U);

    /* (d1) Full-frame 256x192 NV16 across HDF levels & Grain EXACT/SHARP & intensity 1..6 (incl. map1!) */
    struct { uint8_t hdf, inten, size, blur; } cases[] = {
        { Z8_HDF_WEAK,   1U, 1U, Z8_ZF_BLUR_EXACT },
        { Z8_HDF_MEDIUM, 3U, 2U, Z8_ZF_BLUR_SHARP },
        { Z8_HDF_STRONG, 6U, 3U, Z8_ZF_BLUR_EXACT }, /* intensity=6 selects map1 + blur9! */
        { Z8_HDF_STRONG, 6U, 3U, Z8_ZF_BLUR_SHARP },
        { Z8_HDF_OFF,    5U, 3U, Z8_ZF_BLUR_SHARP }, /* Grain-only Y-only shadow alloc! */
    };

    for (size_t ci = 0U; ci < sizeof(cases) / sizeof(cases[0]); ++ci) {
        uint32_t w = 256U, h = 192U, st = 256U, yb = st * h;
        reset_host_dram_bump(b, 2U * yb);
        fill_scene_nv(g_host_dram, g_host_dram + yb, w, h, st, st, 0, (uint32_t)(40U + ci));
        uint8_t *orig = (uint8_t *)malloc(2U * yb);
        memcpy(orig, g_host_dram, 2U * yb);

        PhotoPlaneSpec sp = {
            .role = Z8_PHOTO_MAIN, .w = (uint16_t)w, .h = (uint16_t)h,
            .sw = (uint16_t)w, .sh = (uint16_t)h, .y_st = (uint16_t)st, .c_st = (uint16_t)st,
            .ox = 0U, .oy = 0U,
            .y_dma = HOST_DRAM_DMA_BASE, .c_dma = HOST_DRAM_DMA_BASE + yb,
            .block_base = HOST_DRAM_DMA_BASE, .block_actual = 2U * yb
        };

        Z8FilmSimConfig cfg_xm6, cfg_arm;
        z8_filmsim_cfg_default(&cfg_xm6);
        cfg_xm6.film_style   = Z8_STYLE_NIKON_ZF;
        cfg_xm6.zf_intensity = cases[ci].inten;
        cfg_xm6.zf_size      = cases[ci].size;
        cfg_xm6.zf_blur_mode = cases[ci].blur;
        cfg_xm6.hdf_level    = cases[ci].hdf;
        cfg_xm6.xm6_mode     = Z8_XM6_MODE_AUTO;
        z8_filmsim_cfg_finalize(&cfg_xm6, (uint32_t)(ci + 1U));

        cfg_arm = cfg_xm6;
        cfg_arm.xm6_mode = Z8_XM6_MODE_ARM_ONLY;
        z8_filmsim_cfg_finalize(&cfg_arm, (uint32_t)(ci + 1U));

        PhotoSim psim_x, psim_a;
        memset(&psim_x, 0, sizeof(psim_x));
        psim_x.xm6 = b; psim_x.zf_maps = zf_maps; psim_x.zf_raw_buf = zf_raw;
        psim_build_shot(&psim_x, &sp, 1U);

        memset(&psim_a, 0, sizeof(psim_a));
        psim_a.xm6 = b; psim_a.zf_maps = zf_maps; psim_a.zf_raw_buf = zf_raw;
        psim_build_shot(&psim_a, &sp, 1U);

        struct z8_native_filmsim_photo ph_x, ph_a;
        run_nfsp_shot(&ph_x, &psim_x, &cfg_xm6, (uint32_t)(100U + ci));
        /* Full-frame XM6 MUST skip copy_rows (0 map_source_calls) and skip CPU D-Cache clean (0 clean_calls)! */
        assert(psim_x.map_source_calls == 0U);
        assert(psim_x.clean_calls == 0U);
        /* Source MUST be 100% untouched! */
        assert(memcmp(g_host_dram, orig, 2U * yb) == 0);

        run_nfsp_shot(&ph_a, &psim_a, &cfg_arm, (uint32_t)(100U + ci));
        assert(count_diff_2d(ph_x.planes[0].y, ph_a.planes[0].y, w, h, st) == 0U);
        if (cases[ci].hdf != Z8_HDF_OFF) {
            assert(count_diff_2d(ph_x.planes[0].uv, ph_a.planes[0].uv, w, h, st) == 0U);
        } else {
            /* Grain-only keeps native uv_address directly without allocating shadow UV! */
            assert(ph_x.planes[0].uv == NULL && ph_x.planes[0].uv_dma == sp.c_dma);
        }

        z8_nfsp_retire(&ph_x, psim_x.outer_addr);
        z8_nfsp_retire(&ph_a, psim_a.outer_addr);
        free(orig);
    }

    /* (d2) Multi-strip width 1152x64 (> 1024 -> exercises GF(2) xorshift jump + halo_prev in-place) */
    {
        uint32_t w = 1152U, h = 64U, st = 1152U, yb = st * h;
        reset_host_dram_bump(b, 2U * yb);
        fill_scene_nv(g_host_dram, g_host_dram + yb, w, h, st, st, 0, 77U);

        PhotoPlaneSpec sp = {
            .role = Z8_PHOTO_MAIN, .w = (uint16_t)w, .h = (uint16_t)h,
            .sw = (uint16_t)w, .sh = (uint16_t)h, .y_st = (uint16_t)st, .c_st = (uint16_t)st,
            .ox = 0U, .oy = 0U,
            .y_dma = HOST_DRAM_DMA_BASE, .c_dma = HOST_DRAM_DMA_BASE + yb,
            .block_base = HOST_DRAM_DMA_BASE, .block_actual = 2U * yb
        };
        for (uint8_t bm = 0U; bm <= 1U; ++bm) {
            Z8FilmSimConfig cfg_x, cfg_a;
            z8_filmsim_cfg_default(&cfg_x);
            cfg_x.film_style = Z8_STYLE_NIKON_ZF;
            cfg_x.zf_intensity = 6U;
            cfg_x.zf_size = 3U; /* blur 9 (r1=2, r2=2, r_tot=4) */
            cfg_x.zf_blur_mode = bm;
            cfg_x.hdf_level = Z8_HDF_MEDIUM;
            cfg_x.xm6_mode = Z8_XM6_MODE_AUTO;
            z8_filmsim_cfg_finalize(&cfg_x, 1U);
            cfg_a = cfg_x;
            cfg_a.xm6_mode = Z8_XM6_MODE_ARM_ONLY;
            z8_filmsim_cfg_finalize(&cfg_a, 1U);

            PhotoSim px, pa;
            memset(&px, 0, sizeof(px)); px.xm6 = b; px.zf_maps = zf_maps; px.zf_raw_buf = zf_raw;
            psim_build_shot(&px, &sp, 1U);
            memset(&pa, 0, sizeof(pa)); pa.xm6 = b; pa.zf_maps = zf_maps; pa.zf_raw_buf = zf_raw;
            psim_build_shot(&pa, &sp, 1U);

            struct z8_native_filmsim_photo phx, pha;
            run_nfsp_shot(&phx, &px, &cfg_x, 200U + bm);
            run_nfsp_shot(&pha, &pa, &cfg_a, 200U + bm);
            assert(count_diff_2d(phx.planes[0].y, pha.planes[0].y, w, h, st) == 0U);
            assert(count_diff_2d(phx.planes[0].uv, pha.planes[0].uv, w, h, st) == 0U);
            z8_nfsp_retire(&phx, px.outer_addr);
            z8_nfsp_retire(&pha, pa.outer_addr);
        }
    }

    /* (d3) Right-padded ROI (ox=0, oy=0, w=128 < sw=160, h=sh=96) verifying Audit Fix #1:
     *      must copy_rows so border columns [128..160) are preserved! */
    {
        uint32_t sw = 160U, sh = 96U, w = 128U, h = 96U, st = 192U, yb = st * sh;
        reset_host_dram_bump(b, 2U * yb);
        memset(g_host_dram, 0x5AU, 2U * yb); /* Distinct border pattern in columns [128..160) */
        fill_scene_nv(g_host_dram, g_host_dram + yb, w, h, st, st, 0, 88U);

        PhotoPlaneSpec sp = {
            .role = Z8_PHOTO_MAIN, .w = (uint16_t)w, .h = (uint16_t)h,
            .sw = (uint16_t)sw, .sh = (uint16_t)sh, .y_st = (uint16_t)st, .c_st = (uint16_t)st,
            .ox = 0U, .oy = 0U,
            .y_dma = HOST_DRAM_DMA_BASE, .c_dma = HOST_DRAM_DMA_BASE + yb,
            .block_base = HOST_DRAM_DMA_BASE, .block_actual = 2U * yb
        };
        Z8FilmSimConfig cfg_x, cfg_a;
        z8_filmsim_cfg_default(&cfg_x);
        cfg_x.film_style = Z8_STYLE_NIKON_ZF; cfg_x.zf_intensity = 3U; cfg_x.zf_size = 2U;
        cfg_x.zf_blur_mode = Z8_ZF_BLUR_SHARP; cfg_x.hdf_level = Z8_HDF_MEDIUM;
        cfg_x.xm6_mode = Z8_XM6_MODE_AUTO;
        z8_filmsim_cfg_finalize(&cfg_x, 1U);
        cfg_a = cfg_x; cfg_a.xm6_mode = Z8_XM6_MODE_ARM_ONLY; z8_filmsim_cfg_finalize(&cfg_a, 1U);

        PhotoSim px, pa;
        memset(&px, 0, sizeof(px)); px.xm6 = b; px.zf_maps = zf_maps; px.zf_raw_buf = zf_raw;
        psim_build_shot(&px, &sp, 1U);
        memset(&pa, 0, sizeof(pa)); pa.xm6 = b; pa.zf_maps = zf_maps; pa.zf_raw_buf = zf_raw;
        psim_build_shot(&pa, &sp, 1U);

        struct z8_native_filmsim_photo phx, pha;
        run_nfsp_shot(&phx, &px, &cfg_x, 300U);
        assert(px.map_source_calls > 0U); /* Audit Fix #1: did NOT skip copy_rows when roi_w < storage_width! */
        run_nfsp_shot(&pha, &pa, &cfg_a, 300U);
        /* Active region [0..128) AND right border [128..160) BOTH 0-diff! */
        assert(count_diff_2d(phx.planes[0].y, pha.planes[0].y, sw, sh, st) == 0U);
        assert(count_diff_2d(phx.planes[0].uv, pha.planes[0].uv, sw, sh, st) == 0U);
        z8_nfsp_retire(&phx, px.outer_addr);
        z8_nfsp_retire(&pha, pa.outer_addr);
    }

    /* (d4) Multi-plane partial XM6 fallback with SMALL before MEDIUM (Audit Fix #2):
     *      MAIN (256x192) succeeds on XM6; SMALL (64x48), MEDIUM (128x96), and letterboxed
     *      THUMBNAIL (160x120, ROI 128x96 @ (16,12)) have source OOB -> ARM fallback.
     *      Verifies hjob.analysis_index picks MEDIUM (not SMALL) so z8_hdf_apply_ex succeeds! */
    {
        uint32_t m_yb = 256U * 192U;
        uint32_t s_yb = 64U * 48U;
        uint32_t md_yb = 128U * 96U;
        uint32_t th_yb = 192U * 120U; /* stride 192, storage 160x120 */
        uint32_t off_main  = 0U;
        uint32_t off_small = (2U * m_yb + 511U) & ~511U;
        uint32_t off_med   = (off_small + 2U * s_yb + 511U) & ~511U;
        uint32_t off_thumb = (off_med + 2U * md_yb + 511U) & ~511U;
        uint32_t end_src   = (off_thumb + 2U * th_yb + 511U) & ~511U;
        reset_host_dram_bump(b, end_src);

        fill_scene_nv(g_host_dram + off_main,  g_host_dram + off_main + m_yb,   256U, 192U, 256U, 256U, 0, 91U);
        fill_scene_nv(g_host_dram + off_small, g_host_dram + off_small + s_yb,   64U,  48U,  64U,  64U, 0, 92U);
        fill_scene_nv(g_host_dram + off_med,   g_host_dram + off_med + md_yb,   128U,  96U, 128U, 128U, 0, 93U);
        memset(g_host_dram + off_thumb, 0x10U, 2U * th_yb);
        fill_scene_nv(g_host_dram + off_thumb + 12U * 192U + 16U,
                      g_host_dram + off_thumb + th_yb + 12U * 192U + 16U,
                      128U, 96U, 192U, 192U, 0, 94U);

        /* Place SMALL, MEDIUM, THUMBNAIL sources at 0x08c0000000+ (IOMMU OOB) while MAIN is at 0x0840000000 */
        PhotoPlaneSpec specs[4] = {
            { .role = Z8_PHOTO_MAIN, .w = 256, .h = 192, .sw = 256, .sh = 192, .y_st = 256, .c_st = 256,
              .y_dma = HOST_DRAM_DMA_BASE + off_main, .c_dma = HOST_DRAM_DMA_BASE + off_main + m_yb,
              .block_base = HOST_DRAM_DMA_BASE, .block_actual = end_src },
            { .role = Z8_PHOTO_SMALL, .w = 64, .h = 48, .sw = 64, .sh = 48, .y_st = 64, .c_st = 64,
              .y_dma = UINT64_C(0x08c0000000) + off_small, .c_dma = UINT64_C(0x08c0000000) + off_small + s_yb,
              .block_base = UINT64_C(0x08c0000000), .block_actual = end_src },
            { .role = Z8_PHOTO_MEDIUM, .w = 128, .h = 96, .sw = 128, .sh = 96, .y_st = 128, .c_st = 128,
              .y_dma = UINT64_C(0x08c0000000) + off_med, .c_dma = UINT64_C(0x08c0000000) + off_med + md_yb,
              .block_base = UINT64_C(0x08c0000000), .block_actual = end_src },
            { .role = Z8_PHOTO_THUMBNAIL, .w = 128, .h = 96, .sw = 160, .sh = 120, .y_st = 192, .c_st = 192,
              .ox = 16, .oy = 12,
              .y_dma = UINT64_C(0x08c0000000) + off_thumb, .c_dma = UINT64_C(0x08c0000000) + off_thumb + th_yb,
              .block_base = UINT64_C(0x08c0000000), .block_actual = end_src },
        };

        Z8FilmSimConfig cfg;
        z8_filmsim_cfg_default(&cfg);
        cfg.film_style = Z8_STYLE_NIKON_ZF; cfg.zf_intensity = 3U; cfg.zf_size = 2U;
        cfg.zf_blur_mode = Z8_ZF_BLUR_SHARP; cfg.hdf_level = Z8_HDF_MEDIUM;
        cfg.xm6_mode = Z8_XM6_MODE_AUTO;
        z8_filmsim_cfg_finalize(&cfg, 1U);

        PhotoSim px;
        memset(&px, 0, sizeof(px)); px.xm6 = b; px.zf_maps = zf_maps; px.zf_raw_buf = zf_raw;
        psim_build_shot(&px, specs, 4U);

        struct z8_native_filmsim_photo phx;
        run_nfsp_shot(&phx, &px, &cfg, 400U); /* Must reach Z8_NFSP_READY, NOT Z8_HDF_E_FORMAT! */
        assert(phx.kernel_result == Z8_FS_OK);
        assert((phx.last_stages_applied & (Z8_FS_STAGE_HDF | Z8_FS_STAGE_GRAIN)) ==
               (Z8_FS_STAGE_HDF | Z8_FS_STAGE_GRAIN));
        z8_nfsp_retire(&phx, px.outer_addr);
    }

    assert(env.ext_work_zero_violations == 0U);
    assert(env.dcache_oversize_violations == 0U && env.dcache_unaligned_violations == 0U);
    z8_xm6_release_ext_ws(b);
    filmsim_host_free(b);
    mock_env_destroy(&env);
    printf("  PASS (d)\n");
}

/* ============================================================================
 * Test Group (e): Live Mode 2 NV12 HDF Slot 41 Pre-Clean + Post-Invalidate 0-diff
 * ============================================================================ */

static void test_group_e_live_mode2_hdf_0diff(void)
{
    printf("[Test (e)] Live Mode 2 NV12 HDF (Slot 41 LIVE_256_YC, 1024x680 & 1024x682) pre-clean + post-inv 0-diff...\n");
    MockXm6Env env;
    mock_env_init(&env);
    Z8Xm6HwOps hw = mock_hw_ops(&env);

    struct filmsim_runtime_context *rt =
        (struct filmsim_runtime_context *)filmsim_host_alloc((uint32_t)sizeof(*rt));
    rt->magic = FILMSIM_RUNTIME_MAGIC;
    rt->size  = (uint32_t)sizeof(*rt);
    z8_xm6_bridge_init(&rt->xm6, &hw);
    mock_env_bind_host_win(&env, 0x28U, rt, (uint32_t)sizeof(*rt), 0x41000000U);

    uint8_t *arm_ws = (uint8_t *)filmsim_host_alloc(Z8_FILMSIM_WORKSPACE_BYTES);
    uint32_t heights[2] = { 680U, 682U };
    uint8_t  levels[3]  = { Z8_HDF_WEAK, Z8_HDF_MEDIUM, Z8_HDF_STRONG };

    for (size_t hi = 0U; hi < 2U; ++hi) {
        uint32_t w = 1024U, h = heights[hi];
        uint32_t y_bytes = w * h;
        uint32_t c_bytes = w * (h >> 1);
        uint32_t uv_off  = (y_bytes + 63U) & ~63U;

        for (size_t li = 0U; li < 3U; ++li) {
            reset_host_dram_bump(&rt->xm6, uv_off + c_bytes);
            uint8_t *xm6_y = g_host_dram;
            uint8_t *xm6_c = g_host_dram + uv_off;
            uint8_t *arm_y = (uint8_t *)malloc(y_bytes);
            uint8_t *arm_c = (uint8_t *)malloc(c_bytes);
            fill_scene_nv(xm6_y, xm6_c, w, h, w, w, 1, (uint32_t)(hi * 10U + li));
            memcpy(arm_y, xm6_y, y_bytes);
            memcpy(arm_c, xm6_c, c_bytes);

            /* Pass 0x0c4.. CPU alias to also verify alias stripping in Live path */
            uint64_t y_alias = HOST_DRAM_DMA_BASE + UINT64_C(0x400000000);
            uint64_t c_alias = y_alias + uv_off;

            env.dcache_log_count = 0U;
            env.submit_calls = 0U;
            uint32_t prev_live_ok = rt->xm6.live_ok;
            int rc = z8_xm6_live_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                          xm6_y, xm6_c, y_alias, c_alias,
                                          w, h, w, w, levels[li]);
            assert(rc == Z8_XM6_LIVE_OK);
            assert(env.submit_calls == 1U);
            assert(rt->xm6.live_ok == prev_live_ok + 1U);
            assert(rt->xm6.last_live_us > 0U);

            /* Verify BOTH pre-submit clean (after_submits==0) AND post-submit invalidate (after_submits==1)
             * covered both y_cpu (chunked <= 512 KiB!) and c_cpu! */
            uint32_t pre_y = 0U, pre_c = 0U, post_y = 0U, post_c = 0U;
            uintptr_t y0 = (uintptr_t)xm6_y, y1 = y0 + y_bytes;
            uintptr_t c0 = (uintptr_t)xm6_c, c1 = c0 + c_bytes;
            for (uint32_t i = 0U; i < env.dcache_log_count; ++i) {
                uintptr_t a0 = (uintptr_t)env.dcache_log[i].addr;
                uintptr_t a1 = a0 + env.dcache_log[i].bytes;
                if (env.dcache_log[i].after_submits == 0U) {
                    if (a0 >= y0 && a1 <= ((y1 + 63U) & ~(uintptr_t)63U)) pre_y += env.dcache_log[i].bytes;
                    if (a0 >= c0 && a1 <= ((c1 + 63U) & ~(uintptr_t)63U)) pre_c += env.dcache_log[i].bytes;
                } else {
                    if (a0 >= y0 && a1 <= ((y1 + 63U) & ~(uintptr_t)63U)) post_y += env.dcache_log[i].bytes;
                    if (a0 >= c0 && a1 <= ((c1 + 63U) & ~(uintptr_t)63U)) post_c += env.dcache_log[i].bytes;
                }
            }
            assert(pre_y >= y_bytes && pre_c >= c_bytes);
            assert(post_y >= y_bytes && post_c >= c_bytes);

            /* Compare 0-diff vs ARM NEON z8_hdf_live_nv12_1024 */
            assert(z8_hdf_live_nv12_1024(arm_y, w, arm_c, w, h, levels[li], 0U,
                                         NULL, NULL, arm_ws, Z8_FILMSIM_WORKSPACE_BYTES) == 0);
            assert(memcmp(xm6_y, arm_y, y_bytes) == 0);
            assert(memcmp(xm6_c, arm_c, c_bytes) == 0);

            free(arm_y);
            free(arm_c);
        }
    }

    /* V71.7-r5:
     *   (1) Live Photo z8_xm6_live_hdf_nv12() on EVF 1280x852 MUST return Z8_XM6_LIVE_FALLBACK
     *       with 0 DSP submits (Live Photo remains strictly 1024x{680,682});
     *   (2) Layer 0 EVF preview z8_xm6_preview_hdf_nv12() on 1280x852 runs 0-diff via Slot 41
     *       LIVE_EVF_256_YC across WEAK/MEDIUM/STRONG against ARM z8_hdf_live_nv12_1280;
     *   (3) z8_xm6_preview_hdf_nv12() on LCD 1024x682 runs 0-diff when idle, uses 0 ms fast-skip
     *       when still_in_flight==1, AND 3 preview timeouts NEVER touch live_soft_disabled or
     *       live_timeout_streak so Live Photo 30fps (z8_xm6_live_hdf_nv12) stays 100% accelerated! */
    {
        uint32_t w = 1280U, h = 852U;
        uint32_t y_bytes = w * h;
        uint32_t c_bytes = w * (h >> 1);
        uint32_t uv_off  = (y_bytes + 63U) & ~63U;
        reset_host_dram_bump(&rt->xm6, uv_off + c_bytes);
        uint64_t y_alias = HOST_DRAM_DMA_BASE + UINT64_C(0x400000000);
        uint64_t c_alias = y_alias + uv_off;

        env.submit_calls = 0U;
        assert(z8_xm6_live_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                    g_host_dram, g_host_dram + uv_off,
                                    y_alias, c_alias, w, h, w, w, Z8_HDF_STRONG) == Z8_XM6_LIVE_FALLBACK);
        assert(env.submit_calls == 0U);

        uint8_t *arm_y = (uint8_t *)malloc(y_bytes);
        uint8_t *arm_c = (uint8_t *)malloc(c_bytes);
        assert(arm_y && arm_c);
        for (size_t li = 0U; li < 3U; ++li) {
            uint8_t *xm6_y = g_host_dram;
            uint8_t *xm6_c = g_host_dram + uv_off;
            fill_scene_nv(xm6_y, xm6_c, w, h, w, w, 1, 91U + (uint32_t)li);
            memcpy(arm_y, xm6_y, y_bytes);
            memcpy(arm_c, xm6_c, c_bytes);

            env.submit_calls = 0U;
            assert(z8_xm6_preview_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                           xm6_y, xm6_c, y_alias, c_alias,
                                           w, h, w, w, levels[li]) == Z8_XM6_LIVE_OK);
            assert(env.submit_calls == 1U);
            assert(z8_hdf_live_nv12_1280(arm_y, w, arm_c, w, h, levels[li], 0U,
                                         NULL, NULL, arm_ws, Z8_FILMSIM_WORKSPACE_BYTES) == 0);
            assert(memcmp(xm6_y, arm_y, y_bytes) == 0);
            assert(memcmp(xm6_c, arm_c, c_bytes) == 0);
        }
        free(arm_y);
        free(arm_c);
    }
    {
        uint32_t w = 1024U, h = 682U;
        uint32_t y_bytes = w * h;
        uint32_t c_bytes = w * (h >> 1);
        uint32_t uv_off  = (y_bytes + 63U) & ~63U;
        reset_host_dram_bump(&rt->xm6, uv_off + c_bytes);
        uint8_t *xm6_y = g_host_dram;
        uint8_t *xm6_c = g_host_dram + uv_off;
        uint8_t *arm_y = (uint8_t *)malloc(y_bytes);
        uint8_t *arm_c = (uint8_t *)malloc(c_bytes);
        fill_scene_nv(xm6_y, xm6_c, w, h, w, w, 1, 77U);
        memcpy(arm_y, xm6_y, y_bytes);
        memcpy(arm_c, xm6_c, c_bytes);
        uint64_t y_alias = HOST_DRAM_DMA_BASE + UINT64_C(0x400000000);
        uint64_t c_alias = y_alias + uv_off;

        /* V71.7-r4: Busy Core 0 WITH still_in_flight==1 -> 0 ms fast-skip (ZERO sleep calls!);
         *           Busy Core 0 WITH still_in_flight==0 -> waits up to Z8_XM6_PREVIEW_WAIT_MS (6 ms) */
        __atomic_store_n(&rt->xm6.xm6_busy, 1U, __ATOMIC_RELEASE);
        __atomic_store_n(&rt->xm6.still_in_flight, 1U, __ATOMIC_RELEASE);
        uint32_t prev_sleeps = env.sleep_calls;
        env.submit_calls = 0U;
        assert(z8_xm6_preview_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                       xm6_y, xm6_c, y_alias, c_alias,
                                       w, h, w, w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_FALLBACK);
        assert(env.submit_calls == 0U && env.sleep_calls == prev_sleeps);

        __atomic_store_n(&rt->xm6.still_in_flight, 0U, __ATOMIC_RELEASE);
        prev_sleeps = env.sleep_calls;
        assert(z8_xm6_preview_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                       xm6_y, xm6_c, y_alias, c_alias,
                                       w, h, w, w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_FALLBACK);
        assert(env.submit_calls == 0U && env.sleep_calls == prev_sleeps + Z8_XM6_PREVIEW_WAIT_MS);
        __atomic_store_n(&rt->xm6.xm6_busy, 0U, __ATOMIC_RELEASE);

        /* Idle Core 0 -> LCD 1024x682 preview runs 0-diff via Slot 41 LIVE_256_YC */
        assert(z8_xm6_preview_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                       xm6_y, xm6_c, y_alias, c_alias,
                                       w, h, w, w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_OK);
        assert(z8_hdf_live_nv12_1024(arm_y, w, arm_c, w, h, Z8_HDF_MEDIUM, 0U,
                                     NULL, NULL, arm_ws, Z8_FILMSIM_WORKSPACE_BYTES) == 0);
        assert(memcmp(xm6_y, arm_y, y_bytes) == 0 && memcmp(xm6_c, arm_c, c_bytes) == 0);

        /* 3 drained preview timeouts -> NEVER writes live_soft_disabled or live_timeout_streak! */
        env.behavior = MOCK_DSP_TIMEOUT_CANCEL_OK;
        for (int t = 0; t < 3; ++t) {
            env.clock_us += Z8_XM6_LIVE_BACKOFF_US + 1000ULL;
            assert(z8_xm6_preview_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                           xm6_y, xm6_c, y_alias, c_alias,
                                           w, h, w, w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_SKIP_DIRTY);
        }
        assert(rt->xm6.breaker_tripped == 0U);
        assert(rt->xm6.live_soft_disabled == 0U && rt->xm6.live_timeout_streak == 0U);
        assert(rt->xm6.preview_timeout_streak == 3U && rt->xm6.preview_backoff_until_us > 0U);

        /* While preview is in its 5s cooldown, Live Photo 30fps STILL uses XM6 immediately! */
        env.behavior = MOCK_DSP_RUN_REAL;
        env.submit_calls = 0U;
        assert(z8_xm6_preview_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                       xm6_y, xm6_c, y_alias, c_alias,
                                       w, h, w, w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_FALLBACK);
        assert(env.submit_calls == 0U);
        assert(z8_xm6_live_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                    xm6_y, xm6_c, y_alias, c_alias,
                                    1024U, 680U, 1024U, 1024U, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_OK);
        assert(env.submit_calls == 1U);

        /* After 5s preview cooldown expires, preview auto-recovers cleanly without menu edit! */
        env.clock_us += Z8_XM6_LIVE_BACKOFF_US + 1000ULL;
        assert(z8_xm6_preview_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                       xm6_y, xm6_c, y_alias, c_alias,
                                       w, h, w, w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_OK);
        assert(rt->xm6.preview_timeout_streak == 0U && rt->xm6.preview_backoff_until_us == 0U);

        free(arm_y);
        free(arm_c);
    }

    /* End-to-end filmsim_runtime Mode 2 triple-buffer + 2-tier soft Live backoff -> ARM NEON recovery */
    reset_host_dram_bump(&rt->xm6, 0U);
    atomic_store_explicit(&g_ctx_slot, rt, memory_order_release);
    atomic_store_explicit(&rt->live_worker_ready, 1U, memory_order_release);
    assert(filmsim_ensure_arena(rt, FS_ARENA_FULL_BYTES) == 1);
    rt->live_ws_ready = 1U;
    Z8FilmSimConfig lcfg;
    z8_filmsim_cfg_default(&lcfg);
    lcfg.hdf_level = Z8_HDF_MEDIUM;
    lcfg.live_link = Z8_LIVE_LINK_ON;
    lcfg.xm6_mode  = Z8_XM6_MODE_AUTO;
    z8_filmsim_cfg_finalize(&lcfg, 1U);
    filmsim_publish_cfg(rt, &lcfg, 1U, 1U, 1U);
    assert(filmsim_runtime_live_mode() == FILMSIM_LIVE_MODE_FULL_CPU2);

    /* Idle step with no pending frame MUST return did_work == 0 (CPU2 hot-spin fix) */
    assert(filmsim_live_worker_step(rt) == 0);

    /* Frame 1: XM6 Live succeeds via filmsim_live_worker_step -> 0-diff */
    uint8_t *sub_y = NULL, *sub_uv = NULL;
    unsigned ys = 0U, uvs = 0U;
    int sl = filmsim_runtime_live_begin_submit_nv12(1024U, 680U, &sub_y, &sub_uv, &ys, &uvs);
    assert(sl >= 0 && ys == 1024U && uvs == 1024U);
    fill_scene_nv(sub_y, sub_uv, 1024U, 680U, 1024U, 1024U, 1, 55U);
    uint8_t *ref_y = (uint8_t *)malloc(1024U * 680U);
    uint8_t *ref_c = (uint8_t *)malloc(1024U * 340U);
    memcpy(ref_y, sub_y, 1024U * 680U);
    memcpy(ref_c, sub_uv, 1024U * 340U);
    filmsim_runtime_live_commit_submit_nv12(sl, 1024U, 680U, 10000U, 1);

    uint32_t prev_submits = env.submit_calls;
    assert(filmsim_live_worker_step(rt) == 1);
    assert(env.submit_calls == prev_submits + 1U);
    const uint8_t *acq_y = NULL, *acq_uv = NULL;
    unsigned aw = 0U, ah = 0U;
    uint64_t agen = 0U;
    assert(filmsim_runtime_live_acquire_nv12(&acq_y, &acq_uv, &aw, &ah, &agen, NULL) == 1);
    assert(aw == 1024U && ah == 680U && agen == 1U);
    assert(z8_hdf_live_nv12_1024(ref_y, 1024U, ref_c, 1024U, 680U, Z8_HDF_MEDIUM, 0U,
                                 NULL, NULL, arm_ws, Z8_FILMSIM_WORKSPACE_BYTES) == 0);
    assert(memcmp(acq_y, ref_y, 1024U * 680U) == 0);
    assert(memcmp(acq_uv, ref_c, 1024U * 340U) == 0);
    filmsim_runtime_live_release();

    /* Frame 2: Live XM6 drained timeout -> skip 1 torn frame, enter Tier-1 soft Live backoff
     *          WITHOUT tripping hard global breaker_tripped! */
    env.behavior = MOCK_DSP_TIMEOUT_CANCEL_OK;
    env.cancel_calls = 0U;
    sl = filmsim_runtime_live_begin_submit_nv12(1024U, 680U, &sub_y, &sub_uv, &ys, &uvs);
    assert(sl >= 0);
    fill_scene_nv(sub_y, sub_uv, 1024U, 680U, 1024U, 1024U, 1, 56U);
    filmsim_runtime_live_commit_submit_nv12(sl, 1024U, 680U, 20000U, 1);
    assert(filmsim_live_worker_step(rt) == 1);
    assert(env.cancel_calls == 1U);
    assert(rt->xm6.breaker_tripped == 0U);
    assert(rt->xm6.live_timeout_streak == 1U && rt->xm6.live_backoff_until_us > 0U);
    assert(atomic_load(&rt->live_skipped) == 1U);

    /* Frame 3: Within 5s soft backoff -> immediate ARM NEON grading succeeds with 0-diff! */
    env.behavior = MOCK_DSP_RUN_REAL;
    prev_submits = env.submit_calls;
    sl = filmsim_runtime_live_begin_submit_nv12(1024U, 680U, &sub_y, &sub_uv, &ys, &uvs);
    assert(sl >= 0);
    fill_scene_nv(sub_y, sub_uv, 1024U, 680U, 1024U, 1024U, 1, 57U);
    memcpy(ref_y, sub_y, 1024U * 680U);
    memcpy(ref_c, sub_uv, 1024U * 340U);
    filmsim_runtime_live_commit_submit_nv12(sl, 1024U, 680U, 30000U, 1);
    assert(filmsim_live_worker_step(rt) == 1);
    assert(env.submit_calls == prev_submits); /* 0 XM6 submits while backed off */
    assert(filmsim_runtime_live_acquire_nv12(&acq_y, &acq_uv, &aw, &ah, &agen, NULL) == 1);
    assert(agen == 2U);
    assert(z8_hdf_live_nv12_1024(ref_y, 1024U, ref_c, 1024U, 680U, Z8_HDF_MEDIUM, 0U,
                                 NULL, NULL, arm_ws, Z8_FILMSIM_WORKSPACE_BYTES) == 0);
    assert(memcmp(acq_y, ref_y, 1024U * 680U) == 0);
    assert(memcmp(acq_uv, ref_c, 1024U * 340U) == 0);
    filmsim_runtime_live_release();

    assert(env.ext_work_zero_violations == 0U);
    assert(env.dcache_oversize_violations == 0U && env.dcache_unaligned_violations == 0U);
    z8_xm6_release_ext_ws(&rt->xm6);

    atomic_store_explicit(&g_ctx_slot, NULL, memory_order_release);
    free(ref_y);
    free(ref_c);
    filmsim_host_free(arm_ws);
    filmsim_host_free(rt);
    mock_env_destroy(&env);
    printf("  PASS (e)\n");
}

/* ============================================================================
 * Test Group (f): Z8_FS_OP_XM6_DIAG 4/4 diff_bytes == 0 + No-HW Safe ran_mask == 0
 * ============================================================================ */

static void test_group_f_xm6_diag(const uint8_t *zf_raw)
{
    printf("[Test (f)] Z8_FS_OP_XM6_DIAG 4/4 diff_bytes == 0 + no-HW safe ran_mask == 0...\n");
    MockXm6Env env;
    mock_env_init(&env);
    Z8Xm6HwOps hw = mock_hw_ops(&env);

    struct filmsim_runtime_context *rt =
        (struct filmsim_runtime_context *)filmsim_host_alloc((uint32_t)sizeof(*rt));
    rt->magic = FILMSIM_RUNTIME_MAGIC;
    rt->size  = (uint32_t)sizeof(*rt);
    z8_xm6_bridge_init(&rt->xm6, &hw);
    mock_env_bind_host_win(&env, 0x28U, rt, (uint32_t)sizeof(*rt), 0x41000000U);

    /* (f1) No-hardware / QEMU states -> safe return ran_mask == 0, 0 submits */
    Z8Xm6DiagResult diag;
    uint32_t dlen = (uint32_t)sizeof(diag);

    /* ctx == NULL (dormant on QEMU) */
    assert(filmsim_runtime_service_call_ctx(NULL, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == 0);
    assert(dlen == Z8_XM6_DIAG_BYTES);
    assert(diag.magic == Z8_XM6_DIAG_MAGIC && diag.version == Z8_XM6_DIAG_VERSION &&
           diag.bytes == Z8_XM6_DIAG_BYTES);
    assert(diag.ran_mask == 0U && diag.passed_mask == 0U);

    /* ctx != NULL, QEMU state=0 */
    env.core0_state = 0U;
    dlen = (uint32_t)sizeof(diag);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == 0);
    assert(diag.hw_state == 0U && diag.ran_mask == 0U && diag.passed_mask == 0U);
    assert(env.submit_calls == 0U);

    /* loader_fault != 0 */
    env.core0_state = 2U;
    env.loader_fault = 1U;
    dlen = (uint32_t)sizeof(diag);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == 0);
    assert(diag.loader_fault == 1U && diag.ran_mask == 0U && diag.passed_mask == 0U);
    assert(env.submit_calls == 0U);

    /* Invalid req_bytes != 0 or undersized resp -> -1 */
    uint8_t dummy = 0U;
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, &dummy, 1U, &diag, &dlen) == -1);
    dlen = 16U;
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == -1);

    /* (f2) Hardware READY WITH real ZF_GRAIN.BIN (loaded into rt->zf_grain_buf) -> 4/4 diff_bytes == 0! */
    env.loader_fault = 0U;
    env.fw_loaded = 1U;
    env.core0_state = 2U;
    rt->zf_grain_buf = filmsim_host_alloc(FS_ZF_BUF_BYTES);
    memcpy(rt->zf_grain_buf, zf_raw, Z8_XM6_ZFG1_FILE_BYTES);
    rt->zf_maps_loaded = 1U;
    mock_env_bind_host_win(&env, 0x29U, rt->zf_grain_buf, FS_ZF_BUF_BYTES, 0x42000000U);

    g_host_dram_bump = 0U;
    env.submit_calls = 0U;
    dlen = (uint32_t)sizeof(diag);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == 0);
    assert(dlen == Z8_XM6_DIAG_BYTES);
    assert(diag.magic == Z8_XM6_DIAG_MAGIC && diag.version == 1U && diag.bytes == 64U);
    assert(diag.hw_state == 2U && diag.loader_fault == 0U && diag.fw_loaded == 1U);
    assert(diag.breaker_tripped == 0U);
    assert(diag.ran_mask == Z8_XM6_DIAG_ALL_MASK);
    assert(diag.passed_mask == Z8_XM6_DIAG_ALL_MASK);
    assert(diag.diff_grain_exact == 0U);
    assert(diag.diff_grain_sharp == 0U);
    assert(diag.diff_hdf_still == 0U);
    assert(diag.diff_hdf_live == 0U);
    assert(diag.us_grain_exact > 0U && diag.us_grain_sharp > 0U &&
           diag.us_hdf_still > 0U && diag.us_hdf_live > 0U);
    assert(diag.dsp_status_last == Z8_XM6_DSP_OK);
    assert(env.submit_calls == 4U && env.slot39_calls == 2U && env.slot41_calls == 2U);

    /* Status flags MUST now report DIAG_RAN | DIAG_PASSED | HW_READY */
    Z8FilmSimStatus st;
    uint32_t slen = (uint32_t)sizeof(st);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_STATUS, NULL, 0U, &st, &slen) == 0);
    assert((st.xm6_runtime_flags & (Z8_XM6_FLAG_HW_READY | Z8_XM6_FLAG_DIAG_RAN | Z8_XM6_FLAG_DIAG_PASSED))
           == (Z8_XM6_FLAG_HW_READY | Z8_XM6_FLAG_DIAG_RAN | Z8_XM6_FLAG_DIAG_PASSED));

    /* (f3) Hardware READY WITHOUT ZF_GRAIN.BIN (synthetic maps in Class 0x24) -> 4/4 diff_bytes == 0! */
    filmsim_host_free(rt->zf_grain_buf);
    rt->zf_grain_buf = NULL;
    rt->zf_maps_loaded = 0U;
    g_zf_grain_host_path = NULL; /* Simulate missing card file */
    g_host_dram_bump = 0U;
    dlen = (uint32_t)sizeof(diag);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == 0);
    assert(diag.ran_mask == Z8_XM6_DIAG_ALL_MASK && diag.passed_mask == Z8_XM6_DIAG_ALL_MASK);
    assert(diag.diff_grain_exact == 0U && diag.diff_grain_sharp == 0U &&
           diag.diff_hdf_still == 0U && diag.diff_hdf_live == 0U);

    /* (f4) Class 0x24 OOB (>= 0x08c0000000): Audit Fix #6 verifies dsp_status_last == 0xDEADBEEF (NOT 0!) */
    g_host_dram_bump = 0U;
    g_plat_alloc_force_oob = 1;
    dlen = (uint32_t)sizeof(diag);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == 0);
    g_plat_alloc_force_oob = 0;
    assert(diag.ran_mask == Z8_XM6_DIAG_ALL_MASK && diag.passed_mask == 0U);
    assert(diag.dsp_status_last == Z8_XM6_DSP_STATUS_SENTINEL);
    assert(diag.diff_grain_exact == UINT32_C(0xffffffff));

    /* (f5) stopped == 1 / pool_suspended == 1 guard: safe return ran_mask == 0 with explicit skip codes */
    atomic_store_explicit(&rt->pool_suspended, 1U, memory_order_release);
    dlen = (uint32_t)sizeof(diag);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == 0);
    assert(diag.ran_mask == 0U && diag.passed_mask == 0U);
    assert(diag.dsp_status_last == Z8_XM6_DIAG_SKIP_SUSPENDED);
    atomic_store_explicit(&rt->pool_suspended, 0U, memory_order_release);

    assert(env.ext_work_zero_violations == 0U);
    assert(env.dcache_oversize_violations == 0U && env.dcache_unaligned_violations == 0U);
    filmsim_host_free(rt->zf_grain_buf);
    filmsim_host_free(rt);
    mock_env_destroy(&env);
    printf("  PASS (f)\n");
}

/* ============================================================================
 * Test Group (g): P0 ext_ws != 0 Lifecycle, 2-Tier Live Breaker, Post-CB Mailbox Wait,
 *                 Hung-DMA Quarantine, & Explicit Diag Skip Codes (0xF1..0xF7)
 * ============================================================================ */

static void test_group_g_silicon_hardening(void)
{
    printf("[Test (g)] P0 ext_ws!=0 lifecycle, 2-tier breaker, post-CB mailbox wait, hung-DMA quarantine & 0xF1..0xF7...\n");
    MockXm6Env env;
    mock_env_init(&env);
    Z8Xm6HwOps hw = mock_hw_ops(&env);

    struct filmsim_runtime_context *rt =
        (struct filmsim_runtime_context *)filmsim_host_alloc((uint32_t)sizeof(*rt));
    rt->magic = FILMSIM_RUNTIME_MAGIC;
    rt->size  = (uint32_t)sizeof(*rt);
    z8_xm6_bridge_init(&rt->xm6, &hw);
    mock_env_bind_host_win(&env, 0x28U, rt, (uint32_t)sizeof(*rt), 0x41000000U);

    uint32_t w = 128U, h = 128U, y_b = w * h, c_b = w * h;
    uint32_t lv_w = 1024U, lv_h = 680U;
    uint32_t lv_y_b = lv_w * lv_h;
    uint32_t lv_uv_off = (lv_y_b + 63U) & ~63U;
    uint32_t lv_c_b = lv_w * (lv_h >> 1);
    uint32_t manual_end = (lv_uv_off + lv_c_b + 8U * y_b + 511U) & ~511U;
    reset_host_dram_bump(&rt->xm6, manual_end);

    uint8_t *lv_y = g_host_dram;
    uint8_t *lv_c = g_host_dram + lv_uv_off;
    uint64_t lv_y_dma = HOST_DRAM_DMA_BASE;
    uint64_t lv_c_dma = HOST_DRAM_DMA_BASE + lv_uv_off;
    uint32_t st_off = (lv_uv_off + lv_c_b + 511U) & ~511U;
    uint64_t st_sy = HOST_DRAM_DMA_BASE + st_off;
    uint64_t st_sc = st_sy + y_b;
    uint64_t st_dy = st_sc + c_b;
    uint64_t st_dc = st_dy + y_b;
    fill_scene_nv(lv_y, lv_c, lv_w, lv_h, lv_w, lv_w, 1, 71U);
    fill_scene_nv(g_host_dram + st_off, g_host_dram + st_off + y_b, w, h, w, w, 0, 72U);

    /* (g1) Lazy 512 KiB ext_ws: 0 B while dormant -> alloc fail never uses 0 -> alloc & reuse -> UAF-safe release */
    assert(rt->xm6.ext_ws_dsp_va == 0U && rt->xm6.ext_ws_blk.handle == NULL);
    env.alloc_force_fail = 1;
    assert(z8_xm6_still_hdf(&rt->xm6, Z8_XM6_MODE_AUTO, st_sy, st_sc, st_dy, st_dc,
                            w, h, w, w, y_b, c_b, Z8_HDF_MEDIUM, Z8_XM6_FMT_NV16, NULL)
           == Z8_XM6_STILL_FALLBACK_CLEAN);
    assert(rt->xm6.last_err == Z8_XM6_ERR_ALLOC_FAIL && env.submit_calls == 0U);
    assert(rt->xm6.ext_ws_dsp_va == 0U);
    env.alloc_force_fail = 0;

    assert(z8_xm6_still_hdf(&rt->xm6, Z8_XM6_MODE_AUTO, st_sy, st_sc, st_dy, st_dc,
                            w, h, w, w, y_b, c_b, Z8_HDF_MEDIUM, Z8_XM6_FMT_NV16, NULL)
           == Z8_XM6_STILL_OK);
    uint32_t first_ext_va = rt->xm6.ext_ws_dsp_va;
    assert(first_ext_va != 0U && (first_ext_va & 63U) == 0U && rt->xm6.ext_ws_blk.handle != NULL);
    /* Second HDF reuses existing ext_ws without reallocating */
    assert(z8_xm6_still_hdf(&rt->xm6, Z8_XM6_MODE_AUTO, st_sy, st_sc, st_dy, st_dc,
                            w, h, w, w, y_b, c_b, Z8_HDF_MEDIUM, Z8_XM6_FMT_NV16, NULL)
           == Z8_XM6_STILL_OK);
    assert(rt->xm6.ext_ws_dsp_va == first_ext_va);
    /* Refuse release while mailbox is busy (UAF guard) */
    env.mailbox_forced_busy = 1;
    z8_xm6_release_ext_ws(&rt->xm6);
    assert(rt->xm6.ext_ws_dsp_va == first_ext_va);
    env.mailbox_forced_busy = 0;
    /* Clean release when idle */
    z8_xm6_release_ext_ws(&rt->xm6);
    assert(rt->xm6.ext_ws_dsp_va == 0U && rt->xm6.ext_ws_blk.handle == NULL);

    /* (g2) 2-Tier Breaker: Live soft backoff (5s streak 1..2, soft-disable at streak 3)
     *      keeps Still HDF 100% active on XM6 (breaker_tripped == 0)! */
    reset_host_dram_bump(&rt->xm6, manual_end);
    env.behavior = MOCK_DSP_TIMEOUT_CANCEL_OK;
    assert(z8_xm6_live_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO, lv_y, lv_c, lv_y_dma, lv_c_dma,
                                lv_w, lv_h, lv_w, lv_w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_SKIP_DIRTY);
    assert(rt->xm6.breaker_tripped == 0U && rt->xm6.live_timeout_streak == 1U &&
           rt->xm6.live_soft_disabled == 0U && rt->xm6.live_backoff_until_us > env.clock_us &&
           rt->xm6.last_err == Z8_XM6_ERR_TIMEOUT);

    /* Immediately backed off -> 0 submits on Live, while Still HDF succeeds on XM6! */
    env.behavior = MOCK_DSP_RUN_REAL;
    uint32_t prev_sub = env.submit_calls;
    assert(z8_xm6_live_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO, lv_y, lv_c, lv_y_dma, lv_c_dma,
                                lv_w, lv_h, lv_w, lv_w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_FALLBACK);
    assert(env.submit_calls == prev_sub);
    assert(z8_xm6_still_hdf(&rt->xm6, Z8_XM6_MODE_AUTO, st_sy, st_sc, st_dy, st_dc,
                            w, h, w, w, y_b, c_b, Z8_HDF_MEDIUM, Z8_XM6_FMT_NV16, NULL)
           == Z8_XM6_STILL_OK);
    assert(env.submit_calls == prev_sub + 1U);

    /* Advance 5s -> streak 2 -> Still active; advance 5s -> streak 3 -> live_soft_disabled=1! */
    env.behavior = MOCK_DSP_TIMEOUT_CANCEL_OK;
    env.clock_us += Z8_XM6_LIVE_BACKOFF_US + 1000ULL;
    assert(z8_xm6_live_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO, lv_y, lv_c, lv_y_dma, lv_c_dma,
                                lv_w, lv_h, lv_w, lv_w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_SKIP_DIRTY);
    assert(rt->xm6.breaker_tripped == 0U && rt->xm6.live_timeout_streak == 2U &&
           rt->xm6.live_soft_disabled == 0U);

    env.clock_us += Z8_XM6_LIVE_BACKOFF_US + 1000ULL;
    assert(z8_xm6_live_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO, lv_y, lv_c, lv_y_dma, lv_c_dma,
                                lv_w, lv_h, lv_w, lv_w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_SKIP_DIRTY);
    assert(rt->xm6.breaker_tripped == 0U && rt->xm6.live_timeout_streak == 3U &&
           rt->xm6.live_soft_disabled == 1U);

    /* Even +20s later, soft-disabled Live skips with 0 submits while Still HDF still works! */
    env.behavior = MOCK_DSP_RUN_REAL;
    env.clock_us += 20000000ULL;
    prev_sub = env.submit_calls;
    assert(z8_xm6_live_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO, lv_y, lv_c, lv_y_dma, lv_c_dma,
                                lv_w, lv_h, lv_w, lv_w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_FALLBACK);
    assert(env.submit_calls == prev_sub);
    assert(z8_xm6_still_hdf(&rt->xm6, Z8_XM6_MODE_AUTO, st_sy, st_sc, st_dy, st_dc,
                            w, h, w, w, y_b, c_b, Z8_HDF_MEDIUM, Z8_XM6_FMT_NV16, NULL)
           == Z8_XM6_STILL_OK);

    /* Config change via Z8_FS_OP_SET auto-clears soft Live backoff -> Live recovers! */
    Z8FilmSimConfig new_cfg;
    z8_filmsim_cfg_default(&new_cfg);
    new_cfg.hdf_level = Z8_HDF_STRONG;
    new_cfg.live_link = Z8_LIVE_LINK_ON;
    new_cfg.xm6_mode = Z8_XM6_MODE_AUTO;
    z8_filmsim_cfg_finalize(&new_cfg, 2U);
    uint32_t rlen = 0U;
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_SET, &new_cfg, Z8_FILMSIM_CFG_BYTES, NULL, &rlen) == 0);
    assert(rt->xm6.live_soft_disabled == 0U && rt->xm6.live_timeout_streak == 0U);
    assert(z8_xm6_live_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO, lv_y, lv_c, lv_y_dma, lv_c_dma,
                                lv_w, lv_h, lv_w, lv_w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_OK);

    /* (g3) Post-callback mailbox race (0x40c90fb0 -> 0x40c90fcc): submit waits for idle! */
    env.mailbox_post_cb_busy_polls = 3U;
    env.mailbox_read_calls = 0U;
    assert(z8_xm6_still_hdf(&rt->xm6, Z8_XM6_MODE_AUTO, st_sy, st_sc, st_dy, st_dc,
                            w, h, w, w, y_b, c_b, Z8_HDF_MEDIUM, Z8_XM6_FMT_NV16, NULL)
           == Z8_XM6_STILL_OK);
    assert(env.mailbox_busy_countdown == 0U && env.mailbox_read_calls >= 4U);
    env.mailbox_post_cb_busy_polls = 0U;

    /* (g4) Hung Live DMA (MOCK_DSP_TIMEOUT_STUCK): returns -2 (Z8_XM6_LIVE_HUNG_DMA),
     *      trips hard global breaker_tripped==1, does NOT post-timeout dcache_civac,
     *      and quarantines triple-buffer slot as FS_TB_QUARANTINED! */
    reset_host_dram_bump(&rt->xm6, 0U);
    atomic_store_explicit(&g_ctx_slot, rt, memory_order_release);
    atomic_store_explicit(&rt->live_worker_ready, 1U, memory_order_release);
    assert(filmsim_ensure_arena(rt, FS_ARENA_FULL_BYTES) == 1);
    rt->live_ws_ready = 1U;
    filmsim_publish_cfg(rt, &new_cfg, 1U, 1U, 2U);

    uint8_t *sub_y = NULL, *sub_uv = NULL;
    unsigned ys = 0U, uvs = 0U;
    int sl = filmsim_runtime_live_begin_submit_nv12(1024U, 680U, &sub_y, &sub_uv, &ys, &uvs);
    assert(sl >= 0);
    fill_scene_nv(sub_y, sub_uv, 1024U, 680U, 1024U, 1024U, 1, 79U);
    filmsim_runtime_live_commit_submit_nv12(sl, 1024U, 680U, 40000U, 1);

    env.behavior = MOCK_DSP_TIMEOUT_STUCK;
    env.dcache_log_count = 0U;
    env.submit_calls = 0U;
    assert(filmsim_live_worker_step(rt) == 1);
    assert(rt->xm6.breaker_tripped == 1U && rt->xm6.req_in_flight == 1U);
    assert(fs_tb_get(atomic_load(&rt->lv_tb_state), (unsigned)sl) == FS_TB_QUARANTINED);
    assert(filmsim_runtime_live_is_busy() == 0U);
    /* Verify ZERO post-submit dcache_civac calls on the hung DMA buffer! */
    for (uint32_t i = 0U; i < env.dcache_log_count; ++i) {
        assert(env.dcache_log[i].after_submits == 0U);
    }
    /* Safe reset refuses while req_in_flight==1, and refuses while mailbox is busy */
    z8_xm6_reset_breaker(&rt->xm6);
    assert(rt->xm6.breaker_tripped == 1U);
    assert(env.pending_req != NULL);
    env.pending_req->dsp_status = Z8_XM6_DSP_ERR_ABORT;
    env.pending_req->callback(UINT32_C(0x42180003), env.pending_req);
    env.pending_req = NULL;
    env.mailbox_forced_busy = 1;
    z8_xm6_reset_breaker(&rt->xm6);
    assert(rt->xm6.breaker_tripped == 1U);
    env.mailbox_forced_busy = 0;
    z8_xm6_reset_breaker(&rt->xm6);
    assert(rt->xm6.breaker_tripped == 0U && rt->xm6.last_err == Z8_XM6_ERR_OK);

    /* (g5) Explicit Z8_FS_OP_XM6_DIAG skip codes 0xF1..0xF7 (and 0 when hw_state != 2) */
    Z8Xm6DiagResult diag;
    uint32_t dlen;

    /* 0xF1: stopped */
    atomic_store_explicit(&rt->stopped, 1U, memory_order_release);
    dlen = (uint32_t)sizeof(diag);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == 0);
    assert(diag.ran_mask == 0U && diag.dsp_status_last == Z8_XM6_DIAG_SKIP_STOPPED);
    atomic_store_explicit(&rt->stopped, 0U, memory_order_release);

    /* 0xF2: pool_suspended */
    atomic_store_explicit(&rt->pool_suspended, 1U, memory_order_release);
    dlen = (uint32_t)sizeof(diag);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == 0);
    assert(diag.ran_mask == 0U && diag.dsp_status_last == Z8_XM6_DIAG_SKIP_SUSPENDED);
    atomic_store_explicit(&rt->pool_suspended, 0U, memory_order_release);

    /* 0xF3: mailbox_busy */
    env.mailbox_forced_busy = 1;
    dlen = (uint32_t)sizeof(diag);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == 0);
    assert(diag.ran_mask == 0U && diag.dsp_status_last == Z8_XM6_DIAG_SKIP_MAILBOX_BUSY);
    env.mailbox_forced_busy = 0;

    /* 0xF4: alloc_fail */
    env.alloc_force_fail = 1;
    dlen = (uint32_t)sizeof(diag);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == 0);
    assert(diag.ran_mask == 0U && diag.dsp_status_last == Z8_XM6_DIAG_SKIP_ALLOC_FAIL);
    env.alloc_force_fail = 0;

    /* 0xF5: core0_busy (xm6_busy held) */
    reset_host_dram_bump(&rt->xm6, 0U);
    __atomic_store_n(&rt->xm6.xm6_busy, 1U, __ATOMIC_RELEASE);
    dlen = (uint32_t)sizeof(diag);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == 0);
    assert(diag.ran_mask == 0U && diag.dsp_status_last == Z8_XM6_DIAG_SKIP_CORE0_BUSY);
    __atomic_store_n(&rt->xm6.xm6_busy, 0U, __ATOMIC_RELEASE);

    /* 0xF6: no_ctx (simulated via z8_xm6_run_diag_ex with caller_skip_reason=0xF6 when HW ready) */
    z8_xm6_run_diag_ex(&rt->xm6, NULL, 0U, Z8_XM6_DIAG_SKIP_NO_CTX, &diag);
    assert(diag.ran_mask == 0U && diag.dsp_status_last == Z8_XM6_DIAG_SKIP_NO_CTX);

    /* 0xF7: req_in_flight */
    __atomic_store_n(&rt->xm6.req_in_flight, 1U, __ATOMIC_RELEASE);
    dlen = (uint32_t)sizeof(diag);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == 0);
    assert(diag.ran_mask == 0U && diag.dsp_status_last == Z8_XM6_DIAG_SKIP_REQ_IN_FLIGHT);
    __atomic_store_n(&rt->xm6.req_in_flight, 0U, __ATOMIC_RELEASE);

    /* hw_state != 2 (QEMU) MUST keep dsp_status_last == 0 even if stopped/suspended/no_ctx! */
    env.core0_state = 0U;
    atomic_store_explicit(&rt->stopped, 1U, memory_order_release);
    dlen = (uint32_t)sizeof(diag);
    assert(filmsim_runtime_service_call_ctx(rt, Z8_FS_OP_XM6_DIAG, NULL, 0U, &diag, &dlen) == 0);
    assert(diag.ran_mask == 0U && diag.dsp_status_last == 0U);
    atomic_store_explicit(&rt->stopped, 0U, memory_order_release);

    assert(env.ext_work_zero_violations == 0U);
    atomic_store_explicit(&g_ctx_slot, NULL, memory_order_release);
    z8_xm6_release_ext_ws(&rt->xm6);
    filmsim_host_free(rt);
    mock_env_destroy(&env);
    printf("  PASS (g)\n");
}

/* ============================================================================
 * Test Group (h): V71.7-r4 Deliverables
 *   (h1) P1 Crash Fix #3: XM6 Grain & HDF un-drained timeout -> hung_dma quarantine
 *        (NEVER calls free_blocks() while DSP DMA may still write d->y_dma!)
 *   (h2) XM6 Live/Preview FPS Optimization:
 *        - is_live != 0 -> spin_budget_us == 0 (sleeps cleanly instead of 2ms busy-spin)
 *        - preview wait_ms = 6 acquires Core 0 mid-wait when still_in_flight == 0
 *        - preview fast-skips (0 sleeps) when still_in_flight == 1 or pool_suspended == 1
 *   (h3) Single-Pass Live->LCD Shared Frame Producer (filmsim_live_try_copy_graded_lcd):
 *        - 0-diff copy (tight + padded stride canary), V71R7_L0_STAGE_* bitmask (no grain bit!)
 *        - freshness / w / h / lut_gen / hdf_level miss checks + pool_suspend invalidation
 *        - Dekker pin concurrency guard prevents CPU0 from overwriting a pinned slot
 *   (h4) Shared slots.state EL1 IPC Router (filmsim_slots_svc_dispatch ops 32..79):
 *        - 32..47 ('FSI1'): Z8_FS_OP_STATUS active_drive ('A'/'B') + xm6_reserved==0
 *        - 48..63 ('LPI1'): routes to live_ipc_service_call(raw_op, ...)
 *        - 64..79 ('LTI1'): routes to v71r7_lut_ipc_service_call(raw_op, ...) + -5 -> FX_BUSY (5)
 *        - invalid magic / z8_op >= 48 -> FX_ARGUMENT (1)
 *   (h5) EVF 1280x852 Stage HDF Dispatcher (z8_fs_stage_hdf_run -> z8_hdf_live_nv12_1280)
 * ============================================================================ */

static void test_group_h_v71r7_r4_deliverables(const uint8_t *zf_raw,
                                             const z8_zf_grain_maps *zf_maps)
{
    printf("[Test (h)] V71.7-r4: Hung-DMA quarantine, XM6 Live/Preview wait+0-spin, Shared LCD copy, IPC router 32..79, EVF stage...\n");
    MockXm6Env env;
    mock_env_init(&env);
    Z8Xm6HwOps hw = mock_hw_ops(&env);

    struct filmsim_runtime_context *rt =
        (struct filmsim_runtime_context *)filmsim_host_alloc((uint32_t)sizeof(*rt));
    rt->magic = FILMSIM_RUNTIME_MAGIC;
    rt->size  = (uint32_t)sizeof(*rt);
    z8_xm6_bridge_init(&rt->xm6, &hw);
    mock_env_bind_host_win(&env, 0x28U, rt, (uint32_t)sizeof(*rt), 0x41000000U);
    mock_env_bind_host_win(&env, 0x29U, (void *)zf_raw, FS_ZF_BUF_BYTES, 0x42000000U);

    /* (h1) P1 Crash Fix #3: XM6 Grain un-drained timeout -> Z8_XM6_STILL_HUNG_DMA &
     *      z8_nfsp_worker MUST quarantine with 0 free_blocks() releases (NO Class 0x24 UAF!) */
    {
        uint32_t w = 128U, h = 128U, y_b = w * h;
        reset_host_dram_bump(&rt->xm6, 2U * y_b);
        fill_scene_nv(g_host_dram, g_host_dram + y_b, w, h, w, w, 0, 71U);

        /* Direct z8_xm6_still_grain stuck timeout -> Z8_XM6_STILL_HUNG_DMA (-2) */
        env.behavior = MOCK_DSP_TIMEOUT_STUCK;
        int grc = z8_xm6_still_grain(&rt->xm6, Z8_XM6_MODE_AUTO,
                                     HOST_DRAM_DMA_BASE, HOST_DRAM_DMA_BASE + 2U * y_b,
                                     w, h, w, y_b,
                                     3U, 2U, Z8_ZF_BLUR_SHARP, 0U,
                                     1234U, zf_raw, NULL);
        assert(grc == Z8_XM6_STILL_HUNG_DMA);
        assert(rt->xm6.req_in_flight == 1U && rt->xm6.still_hung_dma == 1U && rt->xm6.breaker_tripped == 1U);
        assert(rt->xm6.still_in_flight == 0U);
        /* Retire stuck request cleanly so we can test end-to-end z8_nfsp_worker */
        assert(env.pending_req != NULL);
        env.pending_req->dsp_status = Z8_XM6_DSP_ERR_ABORT;
        env.pending_req->callback(UINT32_C(0x42180003), env.pending_req);
        env.pending_req = NULL;
        z8_xm6_reset_breaker(&rt->xm6);
        assert(rt->xm6.breaker_tripped == 0U && rt->xm6.still_hung_dma == 0U);

        /* End-to-end z8_nfsp_worker: Stage 1 HDF OK -> Stage 2 Grain TIMEOUT_STUCK!
         * MUST enter Z8_NFSP_QUARANTINE, NEVER call psim_release (release_calls == 0),
         * and clear z8_nfsp_class24_busy so Class 0x24 does not deadlock! */
        reset_host_dram_bump(&rt->xm6, 2U * y_b);
        fill_scene_nv(g_host_dram, g_host_dram + y_b, w, h, w, w, 0, 72U);
        env.behavior = MOCK_DSP_RUN_REAL;
        env.use_slot39_override = 1;
        env.slot39_override = MOCK_DSP_TIMEOUT_STUCK;

        PhotoPlaneSpec sp = {
            .role = Z8_PHOTO_MAIN, .w = (uint16_t)w, .h = (uint16_t)h,
            .sw = (uint16_t)w, .sh = (uint16_t)h, .y_st = (uint16_t)w, .c_st = (uint16_t)w,
            .y_dma = HOST_DRAM_DMA_BASE, .c_dma = HOST_DRAM_DMA_BASE + y_b,
            .block_base = HOST_DRAM_DMA_BASE, .block_actual = 2U * y_b
        };
        Z8FilmSimConfig cfg;
        z8_filmsim_cfg_default(&cfg);
        cfg.film_style = Z8_STYLE_NIKON_ZF; cfg.zf_intensity = 3U; cfg.zf_size = 2U;
        cfg.zf_blur_mode = Z8_ZF_BLUR_SHARP; cfg.hdf_level = Z8_HDF_MEDIUM;
        cfg.xm6_mode = Z8_XM6_MODE_AUTO;
        z8_filmsim_cfg_finalize(&cfg, 1U);

        PhotoSim psim;
        memset(&psim, 0, sizeof(psim));
        psim.xm6 = &rt->xm6; psim.zf_maps = zf_maps; psim.zf_raw_buf = zf_raw;
        psim_build_shot(&psim, &sp, 1U);

        struct z8_nfsp_ops ops;
        memset(&ops, 0, sizeof(ops));
        ops.read = psim_read; ops.allocate = psim_alloc; ops.release = psim_release;
        ops.map_source = psim_map_source; ops.clean = psim_clean;
        ops.pool_valid = psim_pool_valid; ops.yield = psim_yield; ops.time_us = psim_time_us;
        ops.acquire_assets = psim_acquire_assets; ops.release_assets = psim_release_assets;
        ops.context = &psim;

        struct z8_native_filmsim_photo ph_q;
        z8_nfsp_init(&ph_q, &ops);
        assert(z8_nfsp_begin(&ph_q, psim.ctx_addr, &cfg, 777U) == 1);
        z8_nfsp_wait_return(&ph_q, psim.outer_addr, 0x80000U, 1U);
        z8_nfsp_wait_return(&ph_q, psim.outer_addr, 0x40000U, 1U);
        z8_nfsp_wait_return(&ph_q, psim.outer_addr, 0x200000U, 1U);
        z8_nfsp_wait_return(&ph_q, psim.outer_addr, 0x40U, 1U);
        z8_nfsp_thumbnail_return(&ph_q, psim.outer_addr, 1U);
        uint64_t g = z8_nfsp_context_generation(&ph_q, psim.ctx_addr);
        assert(z8_nfsp_prepare_generation(&ph_q, psim.ctx_addr, g, 100000U, 5000000U) == 1);
        z8_nfsp_worker(&ph_q);

        assert(ph_q.phase == Z8_NFSP_QUARANTINE);
        assert(ph_q.poisoned >= Z8_NFSP_QUARANTINE_THRESHOLD);
        assert(psim.release_calls == 0U); /* CRITICAL: shadow block NOT freed while DSP DMA hung! */
        assert(z8_nfsp_class24_busy(&ph_q) == 0);
        /* Even retiring a quarantined shot MUST NOT free its hung DMA blocks! */
        z8_nfsp_retire(&ph_q, psim.outer_addr);
        assert(psim.release_calls == 0U);

        /* Clean up stuck callback & breaker */
        assert(env.pending_req != NULL);
        env.pending_req->dsp_status = Z8_XM6_DSP_ERR_ABORT;
        env.pending_req->callback(UINT32_C(0x42180003), env.pending_req);
        env.pending_req = NULL;
        env.use_slot39_override = 0;
        env.behavior = MOCK_DSP_RUN_REAL;
        z8_xm6_reset_breaker(&rt->xm6);
    }

    /* (h2) XM6 Live/Preview FPS Optimization:
     *      - is_live != 0 uses spin_budget_us = 0 (no 2ms CPU2 busy-spin!)
     *      - preview wait_ms = 6 acquires Core 0 mid-wait when still_in_flight == 0
     *      - preview & live fast-skip with 0 sleeps when pool_suspended == 1 */
    {
        uint32_t w = 1024U, h = 682U, y_b = w * h, c_b = w * (h >> 1);
        uint32_t uv_off = (y_b + 63U) & ~63U;
        reset_host_dram_bump(&rt->xm6, uv_off + c_b);
        uint8_t *xm6_y = g_host_dram, *xm6_c = g_host_dram + uv_off;
        uint8_t *arm_y = (uint8_t *)malloc(y_b), *arm_c = (uint8_t *)malloc(c_b);
        uint8_t *arm_ws = (uint8_t *)filmsim_host_alloc(Z8_FILMSIM_WORKSPACE_BYTES);
        fill_scene_nv(xm6_y, xm6_c, w, h, w, w, 1, 81U);
        memcpy(arm_y, xm6_y, y_b); memcpy(arm_c, xm6_c, c_b);
        uint64_t y_alias = HOST_DRAM_DMA_BASE + UINT64_C(0x400000000);
        uint64_t c_alias = y_alias + uv_off;

        /* Async 3-tick Live job -> MUST NOT spin 2000us (old code called time_us ~400x!) */
        env.async_delay_ticks = 3U;
        env.time_us_calls = 0U;
        env.sleep_calls = 0U;
        assert(z8_xm6_live_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                    xm6_y, xm6_c, y_alias, c_alias,
                                    w, h, w, w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_OK);
        assert(env.sleep_calls == 3U);
        assert(env.time_us_calls <= 10U); /* 0-spin! Would have been >400 under 2000us busy-spin! */
        env.async_delay_ticks = 0U;
        assert(z8_hdf_live_nv12_1024(arm_y, w, arm_c, w, h, Z8_HDF_MEDIUM, 0U,
                                     NULL, NULL, arm_ws, Z8_FILMSIM_WORKSPACE_BYTES) == 0);
        assert(memcmp(xm6_y, arm_y, y_b) == 0 && memcmp(xm6_c, arm_c, c_b) == 0);

        /* Preview wait_ms = 6 acquires Core 0 cleanly when freed on sleep tick 3! */
        fill_scene_nv(xm6_y, xm6_c, w, h, w, w, 1, 82U);
        memcpy(arm_y, xm6_y, y_b); memcpy(arm_c, xm6_c, c_b);
        __atomic_store_n(&rt->xm6.xm6_busy, 1U, __ATOMIC_RELEASE);
        __atomic_store_n(&rt->xm6.still_in_flight, 0U, __ATOMIC_RELEASE);
        env.release_busy_bridge = &rt->xm6;
        env.release_busy_after_sleeps = 3U;
        env.sleep_calls = 0U;
        assert(z8_xm6_preview_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                       xm6_y, xm6_c, y_alias, c_alias,
                                       w, h, w, w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_OK);
        assert(env.sleep_calls == 3U);
        assert(z8_hdf_live_nv12_1024(arm_y, w, arm_c, w, h, Z8_HDF_MEDIUM, 0U,
                                     NULL, NULL, arm_ws, Z8_FILMSIM_WORKSPACE_BYTES) == 0);
        assert(memcmp(xm6_y, arm_y, y_b) == 0 && memcmp(xm6_c, arm_c, c_b) == 0);

        /* pool_suspended == 1 -> 0-sleep instant fast-skip on both preview and live */
        z8_xm6_set_pool_suspended(&rt->xm6, 1);
        __atomic_store_n(&rt->xm6.xm6_busy, 1U, __ATOMIC_RELEASE);
        env.sleep_calls = 0U;
        env.submit_calls = 0U;
        assert(z8_xm6_preview_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                       xm6_y, xm6_c, y_alias, c_alias,
                                       w, h, w, w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_FALLBACK);
        assert(z8_xm6_live_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                    xm6_y, xm6_c, y_alias, c_alias,
                                    w, h, w, w, Z8_HDF_MEDIUM) == Z8_XM6_LIVE_FALLBACK);
        assert(env.sleep_calls == 0U && env.submit_calls == 0U);
        __atomic_store_n(&rt->xm6.xm6_busy, 0U, __ATOMIC_RELEASE);
        z8_xm6_set_pool_suspended(&rt->xm6, 0);

        free(arm_y); free(arm_c); filmsim_host_free(arm_ws);
    }

    /* (h3) Single-Pass Live->LCD Shared Frame Producer (filmsim_live_try_copy_graded_lcd) */
    {
        reset_host_dram_bump(&rt->xm6, 0U);
        atomic_store_explicit(&g_ctx_slot, rt, memory_order_release);
        atomic_store_explicit(&rt->live_worker_ready, 1U, memory_order_release);
        assert(filmsim_ensure_arena(rt, FS_ARENA_FULL_BYTES) == 1);
        rt->live_ws_ready = 1U;
        Z8FilmSimConfig lcfg;
        z8_filmsim_cfg_default(&lcfg);
        lcfg.hdf_level = Z8_HDF_STRONG;
        lcfg.live_link = Z8_LIVE_LINK_ON;
        lcfg.xm6_mode  = Z8_XM6_MODE_AUTO;
        z8_filmsim_cfg_finalize(&lcfg, 1U);
        filmsim_publish_cfg(rt, &lcfg, 1U, 1U, 1U);

        uint8_t *sub_y = NULL, *sub_uv = NULL;
        unsigned ys = 0U, uvs = 0U;
        int sl = filmsim_runtime_live_begin_submit_nv12(1024U, 682U, &sub_y, &sub_uv, &ys, &uvs);
        assert(sl >= 0);
        fill_scene_nv(sub_y, sub_uv, 1024U, 682U, 1024U, 1024U, 1, 91U);
        uint8_t *ref_y = (uint8_t *)malloc(1024U * 682U);
        uint8_t *ref_c = (uint8_t *)malloc(1024U * 341U);
        uint8_t *arm_ws = (uint8_t *)filmsim_host_alloc(Z8_FILMSIM_WORKSPACE_BYTES);
        memcpy(ref_y, sub_y, 1024U * 682U);
        memcpy(ref_c, sub_uv, 1024U * 341U);
        uint64_t cap_us = g_plat_clock_us;
        filmsim_runtime_live_commit_submit_nv12(sl, 1024U, 682U, cap_us, 1);
        assert(filmsim_live_worker_step(rt) == 1);

        assert(z8_hdf_live_nv12_1024(ref_y, 1024U, ref_c, 1024U, 682U, Z8_HDF_STRONG, 0U,
                                     NULL, NULL, arm_ws, Z8_FILMSIM_WORKSPACE_BYTES) == 0);

        /* Tight stride hit (dst_stride == 1024) */
        uint8_t *dst_y = (uint8_t *)malloc(1088U * 682U);
        uint8_t *dst_c = (uint8_t *)malloc(1088U * 341U);
        uint32_t stages = 0xDEADBEEFU;
        uint32_t seq1 = 0U;
        uint64_t age1 = 0U;
        assert(filmsim_live_try_copy_graded_lcd(1024U, 682U, 0U, Z8_HDF_STRONG, 100000U, 0U,
                                                dst_y, dst_c, 1024U, &stages, &seq1, &age1) == 1);
        /* V71R7_L0_STAGE_HDF == (1<<1); GRAIN (1<<2) MUST be 0 so no double-grain ambiguity! */
        assert(stages == (1U << 1));
        assert(seq1 >= 1U && age1 <= 100000U);
        assert(memcmp(dst_y, ref_y, 1024U * 682U) == 0);
        assert(memcmp(dst_c, ref_c, 1024U * 341U) == 0);

        /* Duplicate seq guard: passing last_seq == seq1 while still fresh MUST return -1 (keep current)! */
        assert(filmsim_live_try_copy_graded_lcd(1024U, 682U, 0U, Z8_HDF_STRONG, 100000U, seq1,
                                                dst_y, dst_c, 1024U, &stages, NULL, NULL) == -1);

        /* Padded stride hit (dst_stride == 1088, last_seq == 0) with canary preservation in cols 1024..1087 */
        memset(dst_y, 0x5AU, 1088U * 682U);
        memset(dst_c, 0xA5U, 1088U * 341U);
        assert(filmsim_live_try_copy_graded_lcd(1024U, 682U, 0U, Z8_HDF_STRONG, 100000U, 0U,
                                                dst_y, dst_c, 1088U, &stages, NULL, NULL) == 1);
        for (uint32_t r = 0U; r < 682U; ++r) {
            assert(memcmp(dst_y + (size_t)r * 1088U, ref_y + (size_t)r * 1024U, 1024U) == 0);
            for (uint32_t p = 1024U; p < 1088U; ++p) assert(dst_y[(size_t)r * 1088U + p] == 0x5AU);
        }
        for (uint32_t r = 0U; r < 341U; ++r) {
            assert(memcmp(dst_c + (size_t)r * 1088U, ref_c + (size_t)r * 1024U, 1024U) == 0);
            for (uint32_t p = 1024U; p < 1088U; ++p) assert(dst_c[(size_t)r * 1088U + p] == 0xA5U);
        }

        /* Mismatch & capture-age staleness checks MUST return 0 (even when last_seq == seq1!) */
        assert(filmsim_live_try_copy_graded_lcd(1024U, 680U, 0U, Z8_HDF_STRONG, 100000U, 0U, dst_y, dst_c, 1024U, &stages, NULL, NULL) == 0);
        assert(filmsim_live_try_copy_graded_lcd(1024U, 682U, 1U, Z8_HDF_STRONG, 100000U, 0U, dst_y, dst_c, 1024U, &stages, NULL, NULL) == 0);
        assert(filmsim_live_try_copy_graded_lcd(1024U, 682U, 0U, Z8_HDF_WEAK,   100000U, 0U, dst_y, dst_c, 1024U, &stages, NULL, NULL) == 0);
        assert(filmsim_live_try_copy_graded_lcd(1024U, 682U, 0U, Z8_HDF_STRONG, 100000U, 0U, dst_y, dst_c, 1000U, &stages, NULL, NULL) == 0);
        g_plat_clock_us += 200000ULL;
        assert(filmsim_live_try_copy_graded_lcd(1024U, 682U, 0U, Z8_HDF_STRONG, 50000U, seq1, dst_y, dst_c, 1024U, &stages, NULL, NULL) == 0);

        /* Dekker pin protection: while slot `sl` is pinned by LCD consumer, CPU0 MUST NOT claim it */
        atomic_store_explicit(&rt->lv_slot_lcd_pins[sl], 1U, memory_order_release);
        int sl2 = filmsim_runtime_live_begin_submit_nv12(1024U, 682U, &sub_y, &sub_uv, &ys, &uvs);
        assert(sl2 >= 0 && sl2 != sl);
        filmsim_runtime_live_commit_submit_nv12(sl2, 1024U, 682U, g_plat_clock_us, 0);
        atomic_store_explicit(&rt->lv_slot_lcd_pins[sl], 0U, memory_order_release);

        /* pool_suspend invalidates via lcd_shared_valid ONLY (single-writer seqlock stays even & untouched!) */
        uint32_t seq_before = atomic_load_explicit(&rt->lcd_shared.seqlock, memory_order_acquire);
        assert((seq_before & 1U) == 0U);
        filmsim_runtime_pool_suspend();
        assert(atomic_load_explicit(&rt->lcd_shared.seqlock, memory_order_acquire) == seq_before);
        assert(atomic_load_explicit(&rt->lcd_shared_valid, memory_order_acquire) == 0U);
        assert(filmsim_live_try_copy_graded_lcd(1024U, 682U, 0U, Z8_HDF_STRONG, 10000000U, 0U, dst_y, dst_c, 1024U, &stages, NULL, NULL) == 0);
        filmsim_runtime_pool_resume();

        free(dst_y); free(dst_c); free(ref_y); free(ref_c); filmsim_host_free(arm_ws);
    }

    /* (h4) Shared slots.state EL1 IPC Router (filmsim_slots_svc_dispatch ops 32..79) */
    {
        uint8_t q_pkt[0x30U + 768U], a_pkt[0x30U + 768U];
        uint32_t rlen = 0U;

        #define CALL_SLOTS(ZOP, MAG, REQ, REQLEN, RESP, RESPLEN_P) do { \
            memset(q_pkt, 0, sizeof(q_pkt)); \
            memset(a_pkt, 0, sizeof(a_pkt)); \
            uint32_t _qb = Z8_FS_SLOTS_HDR_BYTES + (uint32_t)(REQLEN); \
            uint32_t _hdr[4] = { (MAG), (uint32_t)(REQLEN), 0U, 0U }; \
            memcpy(q_pkt + 0x0cU, &_qb, 4U); \
            memcpy(q_pkt + 0x30U, _hdr, sizeof(_hdr)); \
            if ((REQ) && (REQLEN) > 0U) memcpy(q_pkt + 0x30U + Z8_FS_SLOTS_HDR_BYTES, (REQ), (REQLEN)); \
            fx = filmsim_slots_svc_dispatch((ZOP), q_pkt, a_pkt); \
            uint32_t _ab = 0U; \
            memcpy(&_ab, a_pkt + 0x08U, 4U); \
            if (RESPLEN_P) *(RESPLEN_P) = _ab; \
            if ((RESP) && _ab > 0U && _ab <= sizeof(a_pkt) - 0x30U) memcpy((RESP), a_pkt + 0x30U, _ab); \
        } while (0)

        uint32_t fx = 99U;
        uint8_t req_buf[128], resp_buf[128];
        memset(req_buf, 0, sizeof(req_buf));

        /* 32..47 ('FSI1'): Z8_FS_OP_STATUS (z8_op == 1) reports active_drive 'A' and 'B' */
        g_card_drive = 'B';
        Z8FilmSimStatus st;
        memset(&st, 0xFF, sizeof(st));
        CALL_SLOTS(Z8_FS_OP_STATUS, Z8_FS_SLOTS_TUNNEL_MAGIC, NULL, 0U, &st, &rlen);
        assert(fx == 0U && rlen == Z8_FILMSIM_STATUS_BYTES);
        assert(st.active_drive == (uint8_t)'B' && st.xm6_reserved == 0U);

        g_card_drive = 'A';
        CALL_SLOTS(Z8_FS_OP_STATUS, Z8_FS_SLOTS_TUNNEL_MAGIC, NULL, 0U, &st, &rlen);
        assert(fx == 0U && st.active_drive == (uint8_t)'A' && st.xm6_reserved == 0U);

        /* Dormant (ctx == NULL) ALSO reports active_drive cleanly! */
        atomic_store_explicit(&g_ctx_slot, NULL, memory_order_release);
        g_card_drive = 'B';
        CALL_SLOTS(Z8_FS_OP_STATUS, Z8_FS_SLOTS_TUNNEL_MAGIC, NULL, 0U, &st, &rlen);
        assert(fx == 0U && st.active_drive == (uint8_t)'B' && st.xm6_reserved == 0U);
        g_card_drive = 'A';

        /* 48..63 ('LPI1'): z8_op 17 -> raw_op 49 routed to live_ipc_service_call */
        g_mock_lp_last_op = 0U; g_mock_lp_rc = 0; g_mock_lp_resp_magic = Z8_LP_SLOTS_TUNNEL_MAGIC;
        CALL_SLOTS(17U, Z8_LP_SLOTS_TUNNEL_MAGIC, NULL, 0U, resp_buf, &rlen);
        assert(fx == 0U && g_mock_lp_last_op == 49U && rlen == 8U);
        uint32_t hdr[2];
        memcpy(hdr, resp_buf, sizeof(hdr));
        assert(hdr[0] == Z8_LP_SLOTS_TUNNEL_MAGIC && hdr[1] == 49U);

        /* Wrong tunnel magic on 'LPI1' -> FX_ARGUMENT (1) */
        CALL_SLOTS(18U, Z8_FS_SLOTS_TUNNEL_MAGIC, req_buf, 16U, resp_buf, &rlen);
        assert(fx == 1U);

        /* 64..79 ('LTI1'): z8_op 33 -> raw_op 65 routed to v71r7_lut_ipc_service_call */
        g_mock_lt_last_op = 0U; g_mock_lt_rc = 0; g_mock_lt_resp_magic = Z8_LUT_SLOTS_TUNNEL_MAGIC;
        CALL_SLOTS(33U, Z8_LUT_SLOTS_TUNNEL_MAGIC, NULL, 0U, resp_buf, &rlen);
        assert(fx == 0U && g_mock_lt_last_op == 65U && rlen == 8U);
        memcpy(hdr, resp_buf, sizeof(hdr));
        assert(hdr[0] == Z8_LUT_SLOTS_TUNNEL_MAGIC && hdr[1] == 65U);

        /* -5 -> FX_BUSY (5), -3 -> FX_CAPABILITY (3), -2 -> FX_ARGUMENT (1), -1 -> FX_STATE (2) */
        g_mock_lt_rc = -5;
        CALL_SLOTS(36U, Z8_LUT_SLOTS_TUNNEL_MAGIC, NULL, 0U, resp_buf, &rlen);
        assert(fx == 5U && g_mock_lt_last_op == 68U);
        g_mock_lt_rc = -3;
        CALL_SLOTS(36U, Z8_LUT_SLOTS_TUNNEL_MAGIC, NULL, 0U, resp_buf, &rlen);
        assert(fx == 3U);
        g_mock_lt_rc = -2;
        CALL_SLOTS(36U, Z8_LUT_SLOTS_TUNNEL_MAGIC, NULL, 0U, resp_buf, &rlen);
        assert(fx == 1U);
        g_mock_lt_rc = -1;
        CALL_SLOTS(36U, Z8_LUT_SLOTS_TUNNEL_MAGIC, NULL, 0U, resp_buf, &rlen);
        assert(fx == 2U);
        g_mock_lt_rc = 0;

        /* Wrong tunnel magic on 'LTI1' and out-of-range z8_op >= 48 -> FX_ARGUMENT (1) */
        CALL_SLOTS(33U, Z8_LP_SLOTS_TUNNEL_MAGIC, NULL, 0U, resp_buf, &rlen);
        assert(fx == 1U);
        CALL_SLOTS(48U, Z8_LUT_SLOTS_TUNNEL_MAGIC, NULL, 0U, resp_buf, &rlen);
        assert(fx == 1U);
        CALL_SLOTS(99U, Z8_LUT_SLOTS_TUNNEL_MAGIC, NULL, 0U, resp_buf, &rlen);
        assert(fx == 1U);
        #undef CALL_SLOTS
    }

    /* (h5) EVF 1280x852 Stage HDF Dispatcher (z8_fs_stage_hdf_run -> z8_hdf_live_nv12_1280 0-diff) */
    {
        uint32_t w = 1280U, h = 852U, y_b = w * h, c_b = w * (h >> 1);
        uint8_t *y_stg = (uint8_t *)malloc(y_b), *c_stg = (uint8_t *)malloc(c_b);
        uint8_t *y_dir = (uint8_t *)malloc(y_b), *c_dir = (uint8_t *)malloc(c_b);
        uint8_t *ws    = (uint8_t *)filmsim_host_alloc(Z8_FILMSIM_WORKSPACE_BYTES);
        fill_scene_nv(y_stg, c_stg, w, h, w, w, 1, 95U);
        memcpy(y_dir, y_stg, y_b); memcpy(c_dir, c_stg, c_b);

        z8_fs_stage_plane pl = {
            .y = y_stg, .c = c_stg, .y_bytes = y_b, .c_bytes = c_b,
            .width = w, .height = h, .y_stride = w, .c_stride = w,
            .format = Z8_FS_INT_FMT_NV12, .is_full_res = 0U
        };
        assert(z8_fs_stage_hdf_run(Z8_HDF_MEDIUM, 1U, 1U, 0U, &pl, ws, Z8_FILMSIM_WORKSPACE_BYTES, NULL, NULL) == Z8_FS_INT_OK);
        assert(z8_hdf_live_nv12_1280(y_dir, w, c_dir, w, h, Z8_HDF_MEDIUM, 0U, NULL, NULL, ws, Z8_FILMSIM_WORKSPACE_BYTES) == 0);
        assert(memcmp(y_stg, y_dir, y_b) == 0 && memcmp(c_stg, c_dir, c_b) == 0);

        free(y_stg); free(c_stg); free(y_dir); free(c_dir); filmsim_host_free(ws);
    }

    z8_xm6_release_ext_ws(&rt->xm6);
    filmsim_host_free(rt);
    mock_env_destroy(&env);
    printf("  PASS (h)\n");
}

/* ============================================================================
 * Test Group (i): V71.7-r7 Performance & Fluency Fixes
 *   (i1) XM6 Pre-Lock Mailbox-Busy Fix: peer holding xm6_busy + non-idle mailbox MUST
 *        wait cleanly (NOT instant ERR_NOT_READY(5)) and succeed when peer releases!
 *   (i2) Reverse Single-Pass Layer 0 -> Live Sharing (EVF 1280x852 & LCD 1024x682):
 *        push -> layer0_feed_active()==1 -> live_acquire_nv12() 0-diff native 1280x852,
 *        >=18ms cadence gate, stage-parity mismatch instant decoupling, 80ms expiry!
 *   (i3) Post-Shutter 45MP Still Deferral: single-shot defers during post-roll (<1.65s),
 *        clears at >=1.65s or post-roll end, bypasses immediately when burst-full!
 *   (i4) Fused grain clean kernel z8_zf_grain_live_apply_tile_y_rows_clean 0-diff!
 * ============================================================================ */
static void test_group_i_v71r7_r7_fluency_fixes(const uint8_t *zf_raw,
                                                const z8_zf_grain_maps *zf_maps)
{
    printf("[Test (i)] V71.7-r7: XM6 pre-lock wait (0 NOT_READY), Reverse Layer0->Live 1280x852, Post-roll defer, Grain clean...\n");
    MockXm6Env env;
    mock_env_init(&env);
    Z8Xm6HwOps hw = mock_hw_ops(&env);

    struct filmsim_runtime_context *rt =
        (struct filmsim_runtime_context *)filmsim_host_alloc((uint32_t)sizeof(*rt));
    rt->magic = FILMSIM_RUNTIME_MAGIC;
    rt->size  = (uint32_t)sizeof(*rt);
    z8_xm6_bridge_init(&rt->xm6, &hw);
    mock_env_bind_host_win(&env, 0x28U, rt, (uint32_t)sizeof(*rt), 0x41000000U);
    mock_env_bind_host_win(&env, 0x29U, (void *)zf_raw, FS_ZF_BUF_BYTES, 0x42000000U);

    /* (i1) XM6 Pre-Lock Mailbox-Busy Fix: while peer holds xm6_busy AND Core 0 mailbox is
     *      non-idle, BOTH preview and live MUST wait up to 16ms instead of failing with
     *      ERR_NOT_READY(5) at t=0! */
    {
        uint32_t w = 1280U, h = 852U, y_b = w * h, c_b = w * (h >> 1);
        uint32_t uv_off = (y_b + 63U) & ~63U;
        reset_host_dram_bump(&rt->xm6, uv_off + c_b);
        uint8_t *xm6_y = g_host_dram, *xm6_c = g_host_dram + uv_off;
        fill_scene_nv(xm6_y, xm6_c, w, h, w, w, 1, 101U);
        uint64_t y_alias = HOST_DRAM_DMA_BASE + UINT64_C(0x400000000);
        uint64_t c_alias = y_alias + uv_off;

        __atomic_store_n(&rt->xm6.xm6_busy, 1U, __ATOMIC_RELEASE);
        env.mailbox_forced_busy = 1;
        env.release_busy_bridge = &rt->xm6;
        env.release_busy_after_sleeps = 4U;
        env.clear_mbox_busy_on_release = 1;
        env.sleep_calls = 0U;
        uint32_t prev_fb = rt->xm6.fallback_cnt;
        assert(z8_xm6_preview_hdf_nv12(&rt->xm6, Z8_XM6_MODE_AUTO,
                                       xm6_y, xm6_c, y_alias, c_alias,
                                       w, h, w, w, Z8_HDF_STRONG) == Z8_XM6_LIVE_OK);
        assert(env.sleep_calls == 4U);
        assert(rt->xm6.fallback_cnt == prev_fb);
        assert(rt->xm6.waiters == 0U);
    }

    /* (i2) Reverse Single-Pass Layer 0 -> Live Sharing (Native EVF 1280x852 & LCD 1024x682) */
    {
        reset_host_dram_bump(&rt->xm6, 0U);
        atomic_store_explicit(&g_ctx_slot, rt, memory_order_release);
        atomic_store_explicit(&rt->live_worker_ready, 1U, memory_order_release);
        assert(filmsim_ensure_arena(rt, FS_ARENA_FULL_BYTES) == 1);
        rt->live_ws_ready = 1U;
        Z8FilmSimConfig lcfg;
        z8_filmsim_cfg_default(&lcfg);
        lcfg.hdf_level = Z8_HDF_STRONG;
        lcfg.live_link = Z8_LIVE_LINK_ON;
        lcfg.xm6_mode  = Z8_XM6_MODE_AUTO;
        z8_filmsim_cfg_finalize(&lcfg, 1U);
        filmsim_publish_cfg(rt, &lcfg, 1U, 1U, 1U);

        assert(filmsim_runtime_live_layer0_feed_active() == 0);
        assert(filmsim_runtime_live_independent_active() == 1);

        uint32_t w = 1280U, h = 852U;
        uint8_t *evf_y = (uint8_t *)malloc(w * h);
        uint8_t *evf_c = (uint8_t *)malloc(w * (h >> 1));
        fill_scene_nv(evf_y, evf_c, w, h, w, w, 1, 102U);

        /* Push matching EVF 1280x852 frame -> succeeds and activates feed! */
        uint64_t t0 = g_plat_clock_us;
        assert(filmsim_runtime_live_push_layer0_nv12(evf_y, evf_c, w, h, w, w, t0, 0U, Z8_HDF_STRONG) == 1);
        assert(filmsim_runtime_live_layer0_feed_active() == 1);
        assert(filmsim_runtime_live_independent_active() == 0);
        assert(atomic_load(&rt->l0_feed_pushes) == 1U);

        /* CPU0 acquires exact 0-diff native 1280x852 frame directly from LBUF! */
        const uint8_t *ay = NULL, *auv = NULL;
        unsigned aw = 0U, ah = 0U;
        uint64_t agen1 = 0U;
        assert(filmsim_runtime_live_acquire_nv12(&ay, &auv, &aw, &ah, &agen1, NULL) == 1);
        assert(aw == 1280U && ah == 852U && agen1 >= 1U);
        assert(memcmp(ay, evf_y, w * h) == 0 && memcmp(auv, evf_c, w * (h >> 1)) == 0);
        filmsim_runtime_live_release();

        /* < 18ms cadence gate skips immediate second push (60Hz -> 30Hz) */
        g_plat_clock_us += 16000ULL;
        assert(filmsim_runtime_live_push_layer0_nv12(evf_y, evf_c, w, h, w, w, g_plat_clock_us, 0U, Z8_HDF_STRONG) == 0);
        assert(atomic_load(&rt->l0_feed_pushes) == 1U);

        /* >= 18ms cadence pushes new frame and advances generation */
        g_plat_clock_us += 4000ULL;
        evf_y[0] ^= 0x5AU;
        assert(filmsim_runtime_live_push_layer0_nv12(evf_y, evf_c, w, h, w, w, g_plat_clock_us, 0U, Z8_HDF_STRONG) == 1);
        uint64_t agen2 = 0U;
        assert(filmsim_runtime_live_acquire_nv12(&ay, &auv, &aw, &ah, &agen2, NULL) == 1);
        assert(agen2 > agen1 && ay[0] == evf_y[0]);
        filmsim_runtime_live_release();

        /* Stage-parity mismatch (Layer 0 hdf=0 vs Live hdf=3) instantly decouples feed! */
        assert(filmsim_runtime_live_push_layer0_nv12(evf_y, evf_c, w, h, w, w, g_plat_clock_us, 0U, 0U) == 0);
        assert(filmsim_runtime_live_layer0_feed_active() == 0);
        assert(filmsim_runtime_live_independent_active() == 1);

        /* Re-push and verify 80ms freshness window expiry */
        g_plat_clock_us += 30000ULL;
        assert(filmsim_runtime_live_push_layer0_nv12(evf_y, evf_c, w, h, w, w, g_plat_clock_us, 0U, Z8_HDF_STRONG) == 1);
        assert(filmsim_runtime_live_layer0_feed_active() == 1);
        g_plat_clock_us += 90000ULL;
        assert(filmsim_runtime_live_layer0_feed_active() == 0);
        assert(filmsim_runtime_live_independent_active() == 1);

        free(evf_y); free(evf_c);
    }

    /* (i3) Post-Shutter 45MP Still Deferral */
    {
        __atomic_store_n(&rt->photo.phase, Z8_NFSP_PENDING, __ATOMIC_RELEASE);
        __atomic_store_n(&rt->photo_slot1.phase, Z8_NFSP_IDLE, __ATOMIC_RELEASE);
        g_mock_post_roll_active = 1;
        uint64_t st_us = g_plat_clock_us;
        assert(filmsim_photo_should_defer(rt, UINT64_MAX) == 1);
        assert(filmsim_photo_should_defer(rt, st_us) == 1);
        g_plat_clock_us = st_us + 1500000ULL;
        assert(filmsim_photo_should_defer(rt, st_us) == 1); /* Still deferring at 1.50s */
        /* Burst-full (both Slot 0 & Slot 1 non-IDLE) bypasses deferral immediately! */
        __atomic_store_n(&rt->photo_slot1.phase, Z8_NFSP_PENDING, __ATOMIC_RELEASE);
        assert(filmsim_photo_should_defer(rt, st_us) == 0);
        __atomic_store_n(&rt->photo_slot1.phase, Z8_NFSP_IDLE, __ATOMIC_RELEASE);
        /* Hard 1.65s cap releases deferral even if post-roll flag still high */
        g_plat_clock_us = st_us + 1700000ULL;
        assert(filmsim_photo_should_defer(rt, st_us) == 0);
        /* Clearing post-roll releases immediately even at t+10ms */
        g_plat_clock_us = st_us + 10000ULL;
        g_mock_post_roll_active = 0;
        assert(filmsim_photo_should_defer(rt, st_us) == 0);
        __atomic_store_n(&rt->photo.phase, Z8_NFSP_IDLE, __ATOMIC_RELEASE);
    }

    /* (i4) Fused grain clean kernel 0-diff parity */
    {
        uint32_t w = 256U, h = 64U;
        int8_t *gtile = (int8_t *)filmsim_host_alloc(Z8_ZF_LIVE_TILE_BYTES);
        uint8_t *gws = (uint8_t *)filmsim_host_alloc(Z8_ZF_GRAIN_WORKSPACE_BYTES);
        z8_zf_grain_preset pr = { 3U, 2U, Z8_ZF_GRAIN_BLUR_SHARP, 1U };
        assert(z8_zf_grain_live_bake_tile(&pr, zf_maps, FILMSIM_LIVE_GRAIN_SEED,
                                          gtile, Z8_ZF_LIVE_TILE_BYTES,
                                          gws, Z8_ZF_GRAIN_WORKSPACE_BYTES) == Z8_ZF_GRAIN_OK);
        uint8_t *y1 = (uint8_t *)malloc(w * h);
        uint8_t *y2 = (uint8_t *)malloc(w * h);
        for (uint32_t i = 0; i < w * h; ++i) y1[i] = y2[i] = (uint8_t)((i * 17u + 3u) & 255u);
        z8_zf_grain_live_apply_tile_y_rows(y1, w, w, 0U, h, gtile);
        z8_zf_grain_live_apply_tile_y_rows_clean(y2, w, w, 0U, h, gtile, 1);
        assert(memcmp(y1, y2, w * h) == 0);
        free(y1); free(y2); filmsim_host_free(gtile); filmsim_host_free(gws);
    }

    atomic_store_explicit(&g_ctx_slot, NULL, memory_order_release);
    z8_xm6_release_ext_ws(&rt->xm6);
    filmsim_host_free(rt);
    mock_env_destroy(&env);
    printf("  PASS (i)\n");
}

int main(void)
{
    assert(posix_memalign((void **)&g_host_dram, 512U, HOST_DRAM_SIZE) == 0);
    memset(g_host_dram, 0, HOST_DRAM_SIZE);

    uint8_t *zf_raw = (uint8_t *)filmsim_host_alloc(FS_ZF_BUF_BYTES);
    z8_zf_grain_maps zf_maps;
    load_real_zf_grain(zf_raw, &zf_maps);

    test_group_a_hw_gate_and_qemu_fallback(zf_raw, &zf_maps);
    test_group_b_iommu_and_dcache(zf_raw, &zf_maps);
    test_group_c_timeout_cancel_breaker_reset(zf_raw, &zf_maps);
    test_group_d_still_hdf_grain_0diff(zf_raw, &zf_maps);
    test_group_e_live_mode2_hdf_0diff();
    test_group_f_xm6_diag(zf_raw);
    test_group_g_silicon_hardening();
    test_group_h_v71r7_r4_deliverables(zf_raw, &zf_maps);
    test_group_i_v71r7_r7_fluency_fixes(zf_raw, &zf_maps);

    filmsim_host_free(zf_raw);
    free(g_host_dram);

    printf("\n=== ALL 9 XM6 BRIDGE + V71.7-r7 TEST GROUPS (a..i) PASSED (0-DIFF, ASan+UBSan CLEAN) ===\n");
    return 0;
}
