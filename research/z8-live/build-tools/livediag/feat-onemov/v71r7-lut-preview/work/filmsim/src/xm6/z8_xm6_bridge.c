/* SPDX-License-Identifier: MIT
 * Nikon Z8 C3.11 EL1 ARM64 <-> CEVA-XM6 Core 0 Hardware Acceleration Bridge.
 *
 * Freestanding C11, scalar-only (-mgeneral-regs-only), zero FP, zero SIMD,
 * zero writable .data/.bss, zero libc heap calls.
 */
#include "z8_xm6_bridge.h"
#include "../photo/z8_native_photo_platform.h"
#include "../pipeline/z8_filmsim_stages.h"
#include <string.h>

#define XM6_FN(a, t) ((t)(uintptr_t)(a))

/* Verified Z8 C3.11 main.main bare-metal ROM / DRAM addresses */
#define Z8_ADDR_XM6_LOADER_FAULT  UINT64_C(0x0a02e803ee) /* uint8_t */
#define Z8_ADDR_XM6_CORE0_LOADED  UINT64_C(0x0a02e803e8) /* uint8_t */
#define Z8_ADDR_XM6_CORE0_STATE   UINT64_C(0x0a02e7e734) /* uint32_t (0x0a02e7e6d0 + 0x64) */
#define Z8_ADDR_XM6_CORE0_MAILBOX UINT64_C(0x0a02e7eed0) /* uint32_t[4] idle sentinel words */

#define Z8_ADDR_IOMMU_WIN_BASE    UINT64_C(0x40cb6d50)
#define Z8_ADDR_IOMMU_WIN_SIZE    UINT64_C(0x40cb6d0c)
#define Z8_ADDR_IOMMU_ARM2DSP     UINT64_C(0x40cb6b50)
#define Z8_ADDR_DCACHE_CIVAC      UINT64_C(0x40d2b9c8)
#define Z8_ADDR_DSP_SUBMIT_ASYNC  UINT64_C(0x40c909a8)
#define Z8_ADDR_DSP_SUBMIT_SYNC   UINT64_C(0x40c90aec)
#define Z8_ADDR_DSP_CANCEL_ASYNC  UINT64_C(0x40c90bec)
#define Z8_ADDR_RTOS_DLY_TSK      UINT64_C(0x40187b00)

/* Flight event IDs matching v71r7_lut_abi.h (zero header coupling to lut_core) */
#define Z8_XM6_FLIGHT_SUBMIT      18U /* FX_Z8_FLIGHT_XM6_SUBMIT */
#define Z8_XM6_FLIGHT_TIMEOUT     19U /* FX_Z8_FLIGHT_XM6_TIMEOUT */
#define Z8_XM6_FLIGHT_SPACER      20U /* FX_Z8_FLIGHT_XM6_SPACER */

#if defined(V71R7_HOST_TEST) || !defined(FILMSIM_HOST_TEST)
extern void v71r7_flight_record(uint16_t event, uint16_t core_flags,
                                uint32_t a, uint32_t b, uint32_t c);
#endif

static inline void xm6_flight_log(uint16_t event, uint16_t flags,
                                  uint32_t a, uint32_t b, uint32_t c)
{
#if defined(V71R7_HOST_TEST) || !defined(FILMSIM_HOST_TEST)
    v71r7_flight_record(event, flags, a, b, c);
#else
    (void)event;
    (void)flags;
    (void)a;
    (void)b;
    (void)c;
#endif
}

/* Diag temporary Class 0x24 layout (~4.02 MiB, fits in 1 Z8 11.45 MiB granule):
 *   src_buf  : 1,044,480 B (1024*680 NV12, 512B aligned)
 *   xm6_buf  : 1,044,480 B (off-screen out != in)
 *   arm_buf  : 1,044,480 B (ARM NEON reference output)
 *   arm_ws   :   524,288 B (512 KiB ARM NEON workspace)
 *   syn_map  :    32,768 B (optional synthetic 83x83 int16 grain map if ZF_GRAIN.BIN absent)
 *   hdf_ext  :   524,288 B + 64 B align pad (dedicated Slot 41 external grid workspace; NEVER 0!)
 */
#define XM6_DIAG_FRAME_BYTES      1044480U /* 1024 * 680 * 3 / 2, multiple of 512 */
#define XM6_DIAG_WS_BYTES         524288U
#define XM6_DIAG_SYNMAP_BYTES     32768U
#define XM6_DIAG_HDF_EXT_WS_BYTES Z8_XM6_HDF_EXT_WS_BYTES
#define XM6_DIAG_TOTAL_BYTES      (3U * XM6_DIAG_FRAME_BYTES + XM6_DIAG_WS_BYTES + XM6_DIAG_SYNMAP_BYTES + XM6_DIAG_HDF_EXT_WS_BYTES + 64U)

#define XM6_DIAG_SRC_OFF          0U
#define XM6_DIAG_XM6_OFF          XM6_DIAG_FRAME_BYTES
#define XM6_DIAG_ARM_OFF          (2U * XM6_DIAG_FRAME_BYTES)
#define XM6_DIAG_WS_OFF           (3U * XM6_DIAG_FRAME_BYTES)
#define XM6_DIAG_SYNMAP_OFF       (3U * XM6_DIAG_FRAME_BYTES + XM6_DIAG_WS_BYTES)
#define XM6_DIAG_HDF_EXT_WS_OFF   (3U * XM6_DIAG_FRAME_BYTES + XM6_DIAG_WS_BYTES + XM6_DIAG_SYNMAP_BYTES)

#define XM6_DIAG_STILL_W          128U
#define XM6_DIAG_STILL_H          128U
#define XM6_DIAG_LIVE_W           1024U
#define XM6_DIAG_LIVE_H           680U
#define XM6_DIAG_LIVE_Y_BYTES     (XM6_DIAG_LIVE_W * XM6_DIAG_LIVE_H)
#define XM6_DIAG_LIVE_C_BYTES     (XM6_DIAG_LIVE_W * (XM6_DIAG_LIVE_H >> 1))

/* ---- Low-level hardware / mock dispatchers ---- */

static inline Z8Xm6DmaSlots *xm6_slots(struct z8_xm6_bridge *b)
{
    uintptr_t raw = (uintptr_t)b->dma_param_raw;
    uintptr_t aligned = (raw + 63U) & ~(uintptr_t)63U;
    return (Z8Xm6DmaSlots *)aligned;
}

static uint32_t hw_read_core0_state(const struct z8_xm6_bridge *b)
{
    if (b && b->hw && b->hw->read_core0_state) {
        return b->hw->read_core0_state(b->hw->ctx);
    }
#ifdef FILMSIM_HOST_TEST
    return 0U;
#else
    return *(const volatile uint32_t *)(uintptr_t)Z8_ADDR_XM6_CORE0_STATE;
#endif
}

static uint8_t hw_read_loader_fault(const struct z8_xm6_bridge *b)
{
    if (b && b->hw && b->hw->read_loader_fault) {
        return b->hw->read_loader_fault(b->hw->ctx);
    }
#ifdef FILMSIM_HOST_TEST
    return 0U;
#else
    return *(const volatile uint8_t *)(uintptr_t)Z8_ADDR_XM6_LOADER_FAULT;
#endif
}

static uint8_t hw_read_fw_loaded(const struct z8_xm6_bridge *b)
{
    if (b && b->hw && b->hw->read_fw_loaded) {
        return b->hw->read_fw_loaded(b->hw->ctx);
    }
#ifdef FILMSIM_HOST_TEST
    return 0U;
#else
    return *(const volatile uint8_t *)(uintptr_t)Z8_ADDR_XM6_CORE0_LOADED;
#endif
}

static uint64_t hw_iommu_win_base(const struct z8_xm6_bridge *b, uint32_t win)
{
    if (b && b->hw && b->hw->iommu_win_base) {
        return b->hw->iommu_win_base(b->hw->ctx, win);
    }
#ifdef FILMSIM_HOST_TEST
    (void)win;
    return 0U;
#else
    return XM6_FN(Z8_ADDR_IOMMU_WIN_BASE, uint64_t (*)(uint32_t))(win);
#endif
}

static uint32_t hw_iommu_win_size(const struct z8_xm6_bridge *b, uint32_t win)
{
    if (b && b->hw && b->hw->iommu_win_size) {
        return b->hw->iommu_win_size(b->hw->ctx, win);
    }
#ifdef FILMSIM_HOST_TEST
    (void)win;
    return 0U;
#else
    return XM6_FN(Z8_ADDR_IOMMU_WIN_SIZE, uint32_t (*)(uint32_t))(win);
#endif
}

static uint32_t hw_iommu_xlat(const struct z8_xm6_bridge *b, uint32_t win,
                              uint64_t dma_va, uint32_t *out_dsp_va)
{
    if (b && b->hw && b->hw->iommu_xlat) {
        return b->hw->iommu_xlat(b->hw->ctx, win, dma_va, out_dsp_va);
    }
#ifdef FILMSIM_HOST_TEST
    (void)win;
    (void)dma_va;
    (void)out_dsp_va;
    return 3U;
#else
    return XM6_FN(Z8_ADDR_IOMMU_ARM2DSP, uint32_t (*)(uint32_t, uint64_t, uint32_t *))(
        win, dma_va, out_dsp_va);
#endif
}

static void hw_dcache_civac_chunk(const struct z8_xm6_bridge *b, void *addr, uint32_t bytes)
{
    if (b && b->hw && b->hw->dcache_civac) {
        b->hw->dcache_civac(b->hw->ctx, addr, bytes);
        return;
    }
#ifdef FILMSIM_HOST_TEST
    (void)addr;
    (void)bytes;
#else
    (void)XM6_FN(Z8_ADDR_DCACHE_CIVAC, int (*)(void *, uint32_t))(addr, bytes);
#endif
}

static void hw_async_submit(const struct z8_xm6_bridge *b, Z8Xm6DspReq *req)
{
    if (b && b->hw && b->hw->async_submit) {
        b->hw->async_submit(b->hw->ctx, req);
        return;
    }
#ifdef FILMSIM_HOST_TEST
    if (req) {
        req->dsp_status = Z8_XM6_DSP_ERR_ABORT;
        if (req->callback) {
            req->callback(UINT32_C(0x42180003), req);
        }
    }
#else
    XM6_FN(Z8_ADDR_DSP_SUBMIT_ASYNC, void (*)(Z8Xm6DspReq *))(req);
#endif
}

static void hw_async_cancel(const struct z8_xm6_bridge *b, Z8Xm6DspReq *req)
{
    if (b && b->hw && b->hw->async_cancel) {
        b->hw->async_cancel(b->hw->ctx, req);
        return;
    }
#ifdef FILMSIM_HOST_TEST
    (void)req;
#else
    XM6_FN(Z8_ADDR_DSP_CANCEL_ASYNC, void (*)(Z8Xm6DspReq *))(req);
#endif
}

static void hw_sleep_ms(const struct z8_xm6_bridge *b, uint32_t ms)
{
    if (b && b->hw && b->hw->sleep_ms) {
        b->hw->sleep_ms(b->hw->ctx, ms);
        return;
    }
#ifdef FILMSIM_HOST_TEST
    (void)ms;
#else
    (void)XM6_FN(Z8_ADDR_RTOS_DLY_TSK, int (*)(uint32_t))(ms);
#endif
}

static uint64_t hw_time_us(const struct z8_xm6_bridge *b)
{
    if (b && b->hw && b->hw->time_us) {
        return b->hw->time_us(b->hw->ctx);
    }
#ifdef FILMSIM_HOST_TEST
    return 0U;
#else
    uint64_t tick = 0U, hz = 0U;
    __asm__ volatile("isb; mrs %0, cntvct_el0; mrs %1, cntfrq_el0"
                     : "=r"(tick), "=r"(hz) :: "memory");
    return hz ? (tick / hz * UINT64_C(1000000) +
                 (tick % hz) * UINT64_C(1000000) / hz) : 0U;
#endif
}

static void hw_release_class24(const struct z8_xm6_bridge *b, struct z8_nfsp_allocation *blk)
{
    if (b && b->hw && b->hw->release_class24) {
        b->hw->release_class24(b->hw->ctx, blk);
        return;
    }
#ifdef FILMSIM_HOST_TEST
    if (blk) {
        memset(blk, 0, sizeof(*blk));
    }
#else
    z8_fs_plat_release_class24(blk);
#endif
}

/* 512 MiB window-straddle spacer retry wrapper for XM6 Class 0x24 allocations */
static int hw_alloc_class24_xm6(const struct z8_xm6_bridge *b, uint32_t requested,
                                struct z8_nfsp_allocation *out)
{
    if (!out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (!requested || requested > UINT32_C(0x20000000)) {
        return 0;
    }
    if (b && b->hw && b->hw->alloc_class24) {
        struct z8_nfsp_allocation spacer;
        struct z8_nfsp_allocation fresh;
        memset(&spacer, 0, sizeof(spacer));
        if (!b->hw->alloc_class24(b->hw->ctx, requested, &spacer)) {
            return 0;
        }
        if (!z8_fs_plat_span_straddles_512m(spacer.dma, spacer.actual)) {
            *out = spacer;
            return 1;
        }
        uint64_t spacer_dma = spacer.dma;
        uint32_t spacer_actual = spacer.actual;
        memset(&fresh, 0, sizeof(fresh));
        int fresh_ok = b->hw->alloc_class24(b->hw->ctx, requested, &fresh);
        hw_release_class24(b, &spacer);
        if (fresh_ok && z8_fs_plat_span_straddles_512m(fresh.dma, fresh.actual)) {
            hw_release_class24(b, &fresh);
            fresh_ok = 0;
        }
        xm6_flight_log(Z8_XM6_FLIGHT_SPACER, (uint16_t)(fresh_ok ? 1u : 0u),
                       (uint32_t)(spacer_dma >> 12), spacer_actual,
                       fresh_ok ? (uint32_t)(fresh.dma >> 12) : 0u);
        if (!fresh_ok) {
            return 0;
        }
        *out = fresh;
        return 1;
    }
#ifdef FILMSIM_HOST_TEST
    return 0;
#else
    return z8_fs_plat_alloc_class24_xm6(requested, out);
#endif
}

static uint32_t hw_sync_submit(const struct z8_xm6_bridge *b,
                               Z8Xm6RgblutJob *job_cpu,
                               uint64_t job_dma,
                               uint32_t lock_arg)
{
    if (b && b->hw && b->hw->sync_submit) {
        return b->hw->sync_submit(b->hw->ctx, job_cpu, job_dma, lock_arg);
    }
#ifdef FILMSIM_HOST_TEST
    (void)job_dma;
    (void)lock_arg;
    if (job_cpu) {
        job_cpu->status = Z8_XM6_DSP_ERR_ABORT;
    }
    return 0U;
#else
    (void)job_cpu;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t accepted = XM6_FN(Z8_ADDR_DSP_SUBMIT_SYNC,
                               uint32_t (*)(Z8Xm6RgblutJob *, uint32_t))(
        (Z8Xm6RgblutJob *)(uintptr_t)job_dma, lock_arg);
    __asm__ volatile("dsb sy" ::: "memory");
    return accepted;
#endif
}

/* ---- Core Defense Helpers ---- */

int z8_xm6_core0_mailbox_idle(const struct z8_xm6_bridge *b)
{
    if (b && b->hw && b->hw->read_mailbox_words) {
        uint32_t w[4] = {0U, 0U, 0U, 0U};
        b->hw->read_mailbox_words(b->hw->ctx, w);
        return w[0] == Z8_XM6_MAILBOX_IDLE_W0 &&
               w[1] == Z8_XM6_MAILBOX_IDLE_W1 &&
               w[2] == Z8_XM6_MAILBOX_IDLE_W2 &&
               w[3] == Z8_XM6_MAILBOX_IDLE_W3;
    }
#ifdef FILMSIM_HOST_TEST
    /* Legacy V71.6 host mocks leave read_mailbox_words == NULL; treat as idle */
    return 1;
#else
    const volatile uint32_t *lane =
        (const volatile uint32_t *)(uintptr_t)Z8_ADDR_XM6_CORE0_MAILBOX;
    return lane[0] == Z8_XM6_MAILBOX_IDLE_W0 &&
           lane[1] == Z8_XM6_MAILBOX_IDLE_W1 &&
           lane[2] == Z8_XM6_MAILBOX_IDLE_W2 &&
           lane[3] == Z8_XM6_MAILBOX_IDLE_W3;
#endif
}

void z8_xm6_read_hw_raw(const struct z8_xm6_bridge *b,
                        uint32_t *state_out,
                        uint8_t *loader_fault_out,
                        uint8_t *fw_loaded_out)
{
    uint32_t st = hw_read_core0_state(b);
    uint8_t flt = hw_read_loader_fault(b);
    uint8_t fwl = hw_read_fw_loaded(b);
    if (state_out) {
        *state_out = st;
    }
    if (loader_fault_out) {
        *loader_fault_out = flt;
    }
    if (fw_loaded_out) {
        *fw_loaded_out = fwl;
    }
}

/* Pre-lock hardware presence check (EXCLUDES mailbox_idle so concurrent callers
 * wait cleanly on xm6_busy instead of instant ERR_NOT_READY(5) ARM fallback!). */
static int xm6_core0_hw_only_present(const struct z8_xm6_bridge *b)
{
    if (hw_read_loader_fault(b) != 0U) {
        return 0;
    }
    if (hw_read_fw_loaded(b) == 0U) {
        return 0;
    }
    return hw_read_core0_state(b) == 2U;
}

static int xm6_core0_hw_present_ready(const struct z8_xm6_bridge *b, uint8_t xm6_mode)
{
    if (!b || xm6_mode != Z8_XM6_MODE_AUTO) {
        return 0;
    }
    if (__atomic_load_n(&b->breaker_tripped, __ATOMIC_ACQUIRE) != 0U) {
        return 0;
    }
    return xm6_core0_hw_only_present(b);
}

int z8_xm6_core0_hw_only_ready(const struct z8_xm6_bridge *b)
{
    if (!xm6_core0_hw_only_present(b)) {
        return 0;
    }
    return z8_xm6_core0_mailbox_idle(b);
}

int z8_xm6_core0_hw_ready(const struct z8_xm6_bridge *b, uint8_t xm6_mode)
{
    if (!xm6_core0_hw_present_ready(b, xm6_mode)) {
        return 0;
    }
    return z8_xm6_core0_mailbox_idle(b);
}

int z8_xm6_core0_arm_to_dsp32(const struct z8_xm6_bridge *b,
                              uint64_t arm_va,
                              uint32_t span_bytes,
                              uint32_t *out_dsp_va)
{
    if (!arm_va || !span_bytes || !out_dsp_va) {
        return 0;
    }
    /* Strip Z8 cached CPU alias 0x0c40000000..0x0cbfffffff -> DMA 0x0840000000..0x08bfffffff */
    uint64_t dma = (arm_va >= UINT64_C(0x0c40000000) && arm_va < UINT64_C(0x0cc0000000))
                       ? (arm_va - UINT64_C(0x400000000))
                       : arm_va;
    if (dma > UINT64_MAX - (uint64_t)span_bytes) {
        return 0;
    }

    for (uint32_t w = Z8_XM6_IOMMU_WIN_FIRST; w <= Z8_XM6_IOMMU_WIN_LAST; ++w) {
        uint64_t w_base = hw_iommu_win_base(b, w);
        uint64_t w_size = (uint64_t)hw_iommu_win_size(b, w);
        if (w_size != 0U && dma >= w_base && (dma - w_base) <= w_size &&
            (uint64_t)span_bytes <= w_size - (dma - w_base)) {
            uint32_t dsp = 0U;
            if (hw_iommu_xlat(b, w, dma, &dsp) == 0U &&
                dsp >= UINT32_C(0x40000000) &&
                (uint64_t)dsp + (uint64_t)(span_bytes - 1U) <= UINT32_MAX) {
                *out_dsp_va = dsp;
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

static int xm6_check_iommu_cb(const struct z8_xm6_bridge *b,
                              uint64_t arm_va, uint32_t span_bytes)
{
    uint32_t dsp = 0U;
    return z8_xm6_core0_arm_to_dsp32(b, arm_va, span_bytes, &dsp);
}

void z8_xm6_dcache_civac(const struct z8_xm6_bridge *b, void *addr, uint32_t bytes)
{
    if (!addr || !bytes) {
        return;
    }
    uintptr_t raw = (uintptr_t)addr;
    if (raw > UINTPTR_MAX - bytes || raw + bytes > UINTPTR_MAX - 63U) {
        return;
    }
    uintptr_t start = raw & ~(uintptr_t)63U;
    uintptr_t end = (raw + bytes + 63U) & ~(uintptr_t)63U;
    size_t span = (size_t)(end - start);
    uint8_t *cur = (uint8_t *)start;

    /* Strictly chunk <= 512 KiB (0x80000 < 0x88000 native threshold) */
    while (span > 0U) {
        uint32_t n = (span > Z8_XM6_DCACHE_MAX_CHUNK)
                         ? Z8_XM6_DCACHE_MAX_CHUNK
                         : (uint32_t)span;
        hw_dcache_civac_chunk(b, cur, n);
        cur += n;
        span -= n;
    }
}

/* ---- Exact Zf C3.01 Preset & Seed Helpers (bit-exact with z8_zf_grain.c) ---- */

uint32_t z8_xm6_fold_grain_seed(uint64_t shot_id)
{
    uint64_t x = shot_id ^ UINT64_C(0x9E3779B97F4A7C15);
    x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
    x = x ^ (x >> 31);
    uint32_t s = (uint32_t)(x ^ (x >> 32));
    return s ? s : Z8_XM6_ZF_DEFAULT_SEED;
}

void z8_xm6_fill_grain_preset(Z8Xm6GrainIn *in,
                              uint8_t intensity,
                              uint8_t size,
                              uint8_t reduced_res,
                              uint8_t blur_mode,
                              uint32_t prng_seed,
                              uint32_t map0_dsp_va,
                              uint32_t map1_dsp_va)
{
    static const uint8_t k_full_w[6]    = { 2U, 3U, 7U, 11U, 16U, 16U };
    static const uint8_t k_full_blur[3] = { 3U, 5U, 9U };
    static const uint8_t k_red_w[6]     = { 2U, 2U, 3U,  4U,  6U,  9U };
    static const uint8_t k_red_blur[3]  = { 3U, 3U, 3U };

    if (!in) {
        return;
    }
    in->map_dim       = Z8_XM6_ZF_MAP_DIM;
    in->tile_period   = Z8_XM6_ZF_TILE_PERIOD;
    in->tile_step     = Z8_XM6_ZF_TILE_STEP;
    in->tone_knee_lo  = Z8_XM6_ZF_KNEE_LO;
    in->tone_knee_hi  = Z8_XM6_ZF_KNEE_HI;
    in->z8_mode_flags = (blur_mode <= Z8_ZF_BLUR_SHARP) ? blur_mode : (uint8_t)Z8_ZF_BLUR_SHARP;
    in->prng_seed     = prng_seed ? prng_seed : Z8_XM6_ZF_DEFAULT_SEED;
    in->mapA_dim      = Z8_XM6_ZF_MAP_DIM;
    in->mapB_addr     = 0U;
    in->mapB_dim      = Z8_XM6_ZF_MAP_DIM;

    if (intensity == 0U || intensity > Z8_ZF_INTENSITY_MAX ||
        size < Z8_ZF_SIZE_MIN || size > Z8_ZF_SIZE_MAX) {
        in->bypass_flag = 1;
        in->map_blend_weight = 0U;
        in->blur_kernel_size = 0U;
        in->mapA_addr = (uint64_t)map0_dsp_va;
        return;
    }

    in->bypass_flag = 0;
    if (reduced_res) {
        in->map_blend_weight = k_red_w[intensity - 1U];
        in->blur_kernel_size = k_red_blur[size - 1U];
        in->mapA_addr        = (uint64_t)map0_dsp_va;
    } else {
        in->map_blend_weight = k_full_w[intensity - 1U];
        in->blur_kernel_size = k_full_blur[size - 1U];
        in->mapA_addr        = (uint64_t)((intensity == Z8_ZF_INTENSITY_MAX)
                                              ? map1_dsp_va
                                              : map0_dsp_va);
    }
}

/* ---- Async Submit / Callback / Timeout / Cancel / Breaker Core ---- */

static void xm6_bridge_done_cb(uint32_t code, void *req_ptr)
{
    if (!req_ptr) {
        return;
    }
    Z8Xm6DmaSlots *slots = (Z8Xm6DmaSlots *)req_ptr;
    struct z8_xm6_bridge *b = slots->owner;
    if (!b) {
        return;
    }
    __atomic_store_n(&b->cb_code, code, __ATOMIC_RELAXED);
    __atomic_store_n(&b->req_in_flight, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&b->done_flag, 1U, __ATOMIC_RELEASE);
}

static void xm6_set_last_err(struct z8_xm6_bridge *b, uint16_t err)
{
    if (!b) {
        return;
    }
    /* Preserve the fault error that tripped the circuit breaker until RESET_BREAKER */
    if (__atomic_load_n(&b->breaker_tripped, __ATOMIC_ACQUIRE) != 0U &&
        err != Z8_XM6_ERR_TIMEOUT && err != Z8_XM6_ERR_DSP_ERR) {
        return;
    }
    __atomic_store_n(&b->last_err, (uint32_t)err, __ATOMIC_RELEASE);
}

static int xm6_try_acquire_core0(struct z8_xm6_bridge *b, uint32_t wait_ms)
{
    uint64_t t_start = wait_ms ? hw_time_us(b) : 0ULL;
    uint64_t limit_us = (uint64_t)wait_ms * 1000ULL;
    for (uint32_t t = 0U; ; ++t) {
        if (__atomic_load_n(&b->req_in_flight, __ATOMIC_ACQUIRE) == 0U &&
            __atomic_exchange_n(&b->xm6_busy, 1U, __ATOMIC_ACQ_REL) == 0U) {
            if (__atomic_load_n(&b->req_in_flight, __ATOMIC_ACQUIRE) == 0U) {
                return 1;
            }
            __atomic_store_n(&b->xm6_busy, 0U, __ATOMIC_RELEASE);
        }
        if (wait_ms == 0U || t >= wait_ms) {
            return 0;
        }
        uint64_t now = hw_time_us(b);
        if (now >= t_start && (now - t_start) >= limit_us) {
            return 0;
        }
        hw_sleep_ms(b, 1U);
    }
}

static inline void xm6_os_yield(void)
{
#if !defined(FILMSIM_HOST_TEST) && !defined(V71R7_HOST_TEST)
    (void)XM6_FN(Z8_ADDR_RTOS_DLY_TSK, int (*)(uint32_t))(0U);
#endif
}

static int xm6_try_acquire_core0_live(struct z8_xm6_bridge *b, uint32_t wait_ms)
{
    uint64_t t_start = wait_ms ? hw_time_us(b) : 0ULL;
    uint64_t limit_us = (uint64_t)wait_ms * 1000ULL;
    int counted_waiter = 0;
    for (uint32_t t = 0U; ; ++t) {
        if (__atomic_load_n(&b->req_in_flight, __ATOMIC_ACQUIRE) == 0U &&
            __atomic_load_n(&b->still_in_flight, __ATOMIC_ACQUIRE) == 0U &&
            __atomic_load_n(&b->pool_suspended, __ATOMIC_ACQUIRE) == 0U &&
            __atomic_exchange_n(&b->xm6_busy, 1U, __ATOMIC_ACQ_REL) == 0U) {
            if (__atomic_load_n(&b->req_in_flight, __ATOMIC_ACQUIRE) == 0U &&
                __atomic_load_n(&b->still_in_flight, __ATOMIC_ACQUIRE) == 0U &&
                __atomic_load_n(&b->pool_suspended, __ATOMIC_ACQUIRE) == 0U) {
                if (counted_waiter) {
                    __atomic_fetch_sub(&b->waiters, 1U, __ATOMIC_RELEASE);
                }
                return 1;
            }
            __atomic_store_n(&b->xm6_busy, 0U, __ATOMIC_RELEASE);
        }
        /* Fast-skip immediately (0 ms) whenever a 45MP still owns/awaits XM6,
         * pool_suspend is active, or the hard breaker has tripped! */
        if (wait_ms == 0U || t >= wait_ms ||
            __atomic_load_n(&b->still_in_flight, __ATOMIC_ACQUIRE) != 0U ||
            __atomic_load_n(&b->pool_suspended, __ATOMIC_ACQUIRE) != 0U ||
            __atomic_load_n(&b->breaker_tripped, __ATOMIC_ACQUIRE) != 0U) {
            if (counted_waiter) {
                __atomic_fetch_sub(&b->waiters, 1U, __ATOMIC_RELEASE);
            }
            return 0;
        }
        uint64_t now = hw_time_us(b);
        if (now >= t_start && (now - t_start) >= limit_us) {
            if (counted_waiter) {
                __atomic_fetch_sub(&b->waiters, 1U, __ATOMIC_RELEASE);
            }
            return 0;
        }
        if (!counted_waiter) {
            __atomic_fetch_add(&b->waiters, 1U, __ATOMIC_ACQ_REL);
            counted_waiter = 1;
        }
        hw_sleep_ms(b, 1U);
    }
}

static void xm6_release_core0(struct z8_xm6_bridge *b)
{
    __atomic_store_n(&b->xm6_busy, 0U, __ATOMIC_RELEASE);
    if (__atomic_load_n(&b->waiters, __ATOMIC_ACQUIRE) != 0U) {
        xm6_os_yield();
    }
}

static inline void xm6_cpu_yield(void)
{
#if defined(__aarch64__) && !defined(FILMSIM_HOST_TEST)
    __asm__ volatile("yield" ::: "memory");
#endif
}

/* Close DspFramework 0x40c90fb0 -> 0x40c90fcc post-callback mailbox restore race:
 * xm6_bridge_done_cb() fires right BEFORE DspFramework restores 0x0a02e7eed0[0..1]
 * to {0xdeadbeef, 0x10000000}. Wait bounded <= max_us before releasing xm6_busy. */
static int xm6_wait_mailbox_idle_bounded(const struct z8_xm6_bridge *b, uint32_t max_us)
{
    if (z8_xm6_core0_mailbox_idle(b)) {
        return 1;
    }
    uint64_t t0 = hw_time_us(b);
    uint64_t prev_t = t0;
    uint32_t stall_iters = 0U;
    uint32_t max_ms = (max_us + 999U) / 1000U;
    for (uint32_t sp = 0U; sp < 4096U; ++sp) {
        xm6_cpu_yield();
        if (z8_xm6_core0_mailbox_idle(b)) {
            return 1;
        }
        uint64_t now = hw_time_us(b);
        if (now >= t0 && (now - t0) >= (uint64_t)max_us) {
            return 0;
        }
        if (now == prev_t) {
            if (++stall_iters >= 256U) {
                break;
            }
        } else {
            prev_t = now;
            stall_iters = 0U;
        }
    }
    for (uint32_t m = 0U; m < max_ms; ++m) {
        hw_sleep_ms(b, 1U);
        if (z8_xm6_core0_mailbox_idle(b)) {
            return 1;
        }
        uint64_t now = hw_time_us(b);
        if (now >= t0 && (now - t0) >= (uint64_t)max_us) {
            break;
        }
    }
    return z8_xm6_core0_mailbox_idle(b);
}

/* Lazily allocate a dedicated 64B-aligned 512 KiB Class 0x24 external grid workspace
 * for Slot 41 (Z8_RicohHdf_Slot41). Caller MUST already hold xm6_busy!
 * NEVER falls back to ext_work_addr = 0 (which clobbers Nikon FreeRTOS tsk0esc/tsk1esc!). */
static uint16_t xm6_ensure_ext_ws_locked(struct z8_xm6_bridge *b, uint32_t *dsp_va_out)
{
    if (!b || !dsp_va_out) {
        return Z8_XM6_ERR_ALLOC_FAIL;
    }
    if (b->ext_ws_dsp_va != 0U && b->ext_ws_aligned_ptr != 0 &&
        b->ext_ws_blk.cpu != 0 && b->ext_ws_blk.actual >= Z8_XM6_HDF_EXT_WS_BYTES) {
        *dsp_va_out = b->ext_ws_dsp_va;
        return Z8_XM6_ERR_OK;
    }

    struct z8_nfsp_allocation blk;
    memset(&blk, 0, sizeof(blk));
    if (!hw_alloc_class24_xm6(b, Z8_XM6_HDF_EXT_WS_ALLOC, &blk) ||
        !blk.cpu || !blk.dma || blk.actual < Z8_XM6_HDF_EXT_WS_BYTES) {
        if (blk.handle || blk.cpu) {
            blk.used |= Z8_NFSP_USED_CLEANED_BIT;
            hw_release_class24(b, &blk);
        }
        return Z8_XM6_ERR_ALLOC_FAIL;
    }

    uint64_t dma_aligned = (blk.dma + 63U) & ~UINT64_C(63);
    uint64_t delta = dma_aligned - blk.dma;
    if (delta > blk.actual || (blk.actual - (uint32_t)delta) < Z8_XM6_HDF_EXT_WS_BYTES) {
        blk.used |= Z8_NFSP_USED_CLEANED_BIT;
        hw_release_class24(b, &blk);
        return Z8_XM6_ERR_ALLOC_FAIL;
    }

    uint32_t dsp_va = 0U;
    if (!z8_xm6_core0_arm_to_dsp32(b, dma_aligned, Z8_XM6_HDF_EXT_WS_BYTES, &dsp_va) ||
        dsp_va == 0U || (dsp_va & 63U) != 0U) {
        blk.used |= Z8_NFSP_USED_CLEANED_BIT;
        hw_release_class24(b, &blk);
        return Z8_XM6_ERR_IOMMU_OOB;
    }

    uint8_t *cpu_aligned = blk.cpu + (size_t)delta;
    /* One-time D-Cache clean+invalidate so ARM holds zero dirty lines in DSP scratch */
    z8_xm6_dcache_civac(b, cpu_aligned, Z8_XM6_HDF_EXT_WS_BYTES);
    blk.used |= Z8_NFSP_USED_CLEANED_BIT;

    b->ext_ws_blk         = blk;
    b->ext_ws_aligned_ptr = cpu_aligned;
    b->ext_ws_dsp_va      = dsp_va;
    *dsp_va_out           = dsp_va;
    return Z8_XM6_ERR_OK;
}

void z8_xm6_release_ext_ws(struct z8_xm6_bridge *b)
{
    if (!b) {
        return;
    }
    if (!b->ext_ws_blk.handle && !b->ext_ws_blk.cpu && b->ext_ws_dsp_va == 0U) {
        return;
    }
    if (!xm6_try_acquire_core0(b, 150U)) {
        return;
    }
    /* Strict UAF guard: NEVER release Class 0x24 ext_ws while a DSP job may still touch it!
     * If Core 0 is powered down/absent (hw_state != 2), DSP cannot be doing DMA. */
    uint32_t hw_state = 0U;
    uint8_t lf = 0U, fl = 0U;
    z8_xm6_read_hw_raw(b, &hw_state, &lf, &fl);
    if (__atomic_load_n(&b->req_in_flight, __ATOMIC_ACQUIRE) == 0U &&
        (hw_state != 2U || z8_xm6_core0_mailbox_idle(b))) {
        b->ext_ws_dsp_va = 0U;
        b->ext_ws_aligned_ptr = 0;
        if (b->ext_ws_blk.handle || b->ext_ws_blk.cpu) {
            b->ext_ws_blk.used |= Z8_NFSP_USED_CLEANED_BIT;
            hw_release_class24(b, &b->ext_ws_blk);
        }
        __atomic_store_n(&b->live_flight_logged, 0U, __ATOMIC_RELEASE);
    }
    xm6_release_core0(b);
}

/* Caller MUST already hold xm6_busy! Returns enum Z8Xm6Error (0 == OK). */
static uint16_t xm6_submit_locked(struct z8_xm6_bridge *b,
                                  uint32_t slot_id,
                                  uint32_t in_dsp_va,
                                  uint32_t out_dsp_va,
                                  uint32_t timeout_ms,
                                  int is_live,
                                  uint32_t *dsp_status_out,
                                  uint32_t *elapsed_us_out,
                                  int *hung_dma_out)
{
    if (hung_dma_out) {
        *hung_dma_out = 0;
    }
    Z8Xm6DmaSlots *slots = xm6_slots(b);
    memset(&slots->req, 0, sizeof(slots->req));
    slots->req.core_id    = 0U;
    slots->req.slot_id    = slot_id;
    slots->req.in_param   = (uint64_t)in_dsp_va;
    slots->req.out_param  = (uint64_t)out_dsp_va;
    slots->req.channel    = 0U;
    slots->req.callback   = xm6_bridge_done_cb;
    slots->req.dsp_status = Z8_XM6_DSP_STATUS_SENTINEL;
    slots->owner          = b;

    __atomic_store_n(&b->cb_code, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&b->done_flag, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&b->req_in_flight, 1U, __ATOMIC_RELEASE);

    /* Suppress 60 Hz Live HDF FLIGHT_SUBMIT flood: edge-log once per Live session */
    if (!is_live || __atomic_exchange_n(&b->live_flight_logged, 1U, __ATOMIC_ACQ_REL) == 0U) {
        xm6_flight_log(Z8_XM6_FLIGHT_SUBMIT, (uint16_t)slot_id, in_dsp_va, out_dsp_va, timeout_ms);
    }
    uint64_t t0 = hw_time_us(b);
    hw_async_submit(b, &slots->req);

    /* Fast path: synchronous callback (enqueue failure or immediate mock completion) */
    if (__atomic_load_n(&b->done_flag, __ATOMIC_ACQUIRE) != 0U) {
        uint64_t t1 = hw_time_us(b);
        uint32_t dt = (t1 >= t0) ? (uint32_t)((t1 - t0) > UINT32_MAX ? UINT32_MAX : (t1 - t0)) : 0U;
        (void)xm6_wait_mailbox_idle_bounded(b, 2000U);
        uint32_t st = slots->req.dsp_status;
        __atomic_store_n(&b->last_dsp_status, st, __ATOMIC_RELEASE);
        if (dsp_status_out) {
            *dsp_status_out = st;
        }
        if (elapsed_us_out) {
            *elapsed_us_out = dt;
        }
        if (st == Z8_XM6_DSP_OK) {
            return Z8_XM6_ERR_OK;
        }
        if (is_live) {
            __atomic_store_n(&b->live_flight_logged, 0U, __ATOMIC_RELEASE);
        }
        if (st == Z8_XM6_DSP_ERR_ABORT) {
            xm6_set_last_err(b, Z8_XM6_ERR_ENQUEUE_FAIL);
            return Z8_XM6_ERR_ENQUEUE_FAIL;
        }
        __atomic_store_n(&b->breaker_tripped, 1U, __ATOMIC_RELEASE);
        xm6_set_last_err(b, Z8_XM6_ERR_DSP_ERR);
        return Z8_XM6_ERR_DSP_ERR;
    }

    /* V71.7-r4: For Live & Layer 0 preview (is_live != 0), XM6 HDF runs ~5.16 ms so set
     * spin_budget_us = 0 to eliminate the 2.0 ms CPU2 busy-spin and sleep cleanly via dly_tsk(1).
     * Keep bounded fast-poll prologue (<= 2000 us) ONLY for Still/Diag (is_live == 0). */
    uint64_t budget_us = (uint64_t)timeout_ms * UINT64_C(1000);
    uint64_t spin_budget_us = is_live ? UINT64_C(0)
                                      : ((budget_us < UINT64_C(2000)) ? budget_us : UINT64_C(2000));
    if (spin_budget_us > 0U) {
        uint64_t prev_t = t0;
        uint32_t stall_iters = 0U;
        for (uint32_t sp = 0U; sp < 65536U; ++sp) {
            if (__atomic_load_n(&b->done_flag, __ATOMIC_ACQUIRE) != 0U) {
                break;
            }
            xm6_cpu_yield();
            uint64_t now = hw_time_us(b);
            if (now >= t0 && (now - t0) >= spin_budget_us) {
                break;
            }
            if (now == prev_t) {
                if (++stall_iters >= 512U) {
                    break;
                }
            } else {
                prev_t = now;
                stall_iters = 0U;
            }
        }
    }

    /* Bounded dly_tsk(1) polling wait */
    uint32_t iters = 0U;
    while (__atomic_load_n(&b->done_flag, __ATOMIC_ACQUIRE) == 0U) {
        uint64_t now = hw_time_us(b);
        if ((now >= t0 && (now - t0) >= budget_us) || iters >= timeout_ms) {
            break;
        }
        hw_sleep_ms(b, 1U);
        ++iters;
    }

    uint64_t t1 = hw_time_us(b);
    uint32_t dt = (t1 >= t0) ? (uint32_t)((t1 - t0) > UINT32_MAX ? UINT32_MAX : (t1 - t0)) : 0U;
    if (elapsed_us_out) {
        *elapsed_us_out = dt;
    }

    /* Re-check done_flag right before issuing cancel (closes boundary completion race) */
    if (__atomic_load_n(&b->done_flag, __ATOMIC_ACQUIRE) != 0U) {
        (void)xm6_wait_mailbox_idle_bounded(b, 2000U);
        uint32_t st = slots->req.dsp_status;
        __atomic_store_n(&b->last_dsp_status, st, __ATOMIC_RELEASE);
        if (dsp_status_out) {
            *dsp_status_out = st;
        }
        if (st == Z8_XM6_DSP_OK) {
            return Z8_XM6_ERR_OK;
        }
        if (is_live) {
            __atomic_store_n(&b->live_flight_logged, 0U, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&b->breaker_tripped, 1U, __ATOMIC_RELEASE);
        xm6_set_last_err(b, Z8_XM6_ERR_DSP_ERR);
        return Z8_XM6_ERR_DSP_ERR;
    }

    /* Timeout: issue native cancel (0x40c90bec) and drain until BOTH req_in_flight==0
     * AND Core 0 mailbox is proven idle (120 ms Live / 500 ms Still). */
    hw_async_cancel(b, &slots->req);
    xm6_flight_log(Z8_XM6_FLIGHT_TIMEOUT, (uint16_t)slot_id, timeout_ms, dt, slots->req.dsp_status);

    uint32_t drain_ms = is_live ? Z8_XM6_LIVE_DRAIN_MS : Z8_XM6_STILL_DRAIN_MS;
    for (uint32_t d = 0U; d < drain_ms; ++d) {
        if (__atomic_load_n(&b->req_in_flight, __ATOMIC_ACQUIRE) == 0U &&
            z8_xm6_core0_mailbox_idle(b)) {
            break;
        }
        hw_sleep_ms(b, 1U);
    }

    int drained = (__atomic_load_n(&b->req_in_flight, __ATOMIC_ACQUIRE) == 0U &&
                   z8_xm6_core0_mailbox_idle(b));
    if (hung_dma_out) {
        *hung_dma_out = drained ? 0 : 1;
    }

    if (is_live == 2) {
        /* Layer 0 Preview (isolated): drained timeout enters preview-only 5s cooldown
         * without touching Live Photo's live_timeout_streak or live_soft_disabled!
         * Only a true hung DMA (drained == 0) trips the global hard breaker. */
        if (drained) {
            __atomic_fetch_add(&b->preview_timeout_streak, 1U, __ATOMIC_ACQ_REL);
            uint64_t now = hw_time_us(b);
            uint64_t until = (now <= UINT64_MAX - Z8_XM6_LIVE_BACKOFF_US)
                                 ? (now + Z8_XM6_LIVE_BACKOFF_US)
                                 : UINT64_MAX;
            __atomic_store_n(&b->preview_backoff_until_us, until, __ATOMIC_RELEASE);
            xm6_set_last_err(b, Z8_XM6_ERR_TIMEOUT);
        } else {
            __atomic_store_n(&b->breaker_tripped, 1U, __ATOMIC_RELEASE);
            xm6_set_last_err(b, Z8_XM6_ERR_TIMEOUT);
        }
    } else if (is_live == 1) {
        __atomic_store_n(&b->live_flight_logged, 0U, __ATOMIC_RELEASE);
        if (drained) {
            /* 2-Tier Breaker (Soft Live Backoff): DSP drained cleanly -> alive!
             * Keep global breaker_tripped == 0 so Still HDF/Grain & Op12 stay 100% active. */
            uint32_t streak = __atomic_fetch_add(&b->live_timeout_streak, 1U, __ATOMIC_ACQ_REL) + 1U;
            if (streak >= Z8_XM6_LIVE_SOFT_DISABLE_STREAK) {
                __atomic_store_n(&b->live_soft_disabled, 1U, __ATOMIC_RELEASE);
                __atomic_store_n(&b->live_backoff_until_us, 0U, __ATOMIC_RELEASE);
            } else {
                uint64_t now = hw_time_us(b);
                uint64_t until = (now <= UINT64_MAX - Z8_XM6_LIVE_BACKOFF_US)
                                     ? (now + Z8_XM6_LIVE_BACKOFF_US)
                                     : UINT64_MAX;
                __atomic_store_n(&b->live_backoff_until_us, until, __ATOMIC_RELEASE);
            }
            xm6_set_last_err(b, Z8_XM6_ERR_TIMEOUT);
        } else {
            /* True DSP hang (not drained after 120 ms) -> trip hard global circuit breaker */
            __atomic_store_n(&b->breaker_tripped, 1U, __ATOMIC_RELEASE);
            xm6_set_last_err(b, Z8_XM6_ERR_TIMEOUT);
        }
    } else {
        /* Still HDF / Still Grain / Diag: trip hard global circuit breaker */
        __atomic_store_n(&b->breaker_tripped, 1U, __ATOMIC_RELEASE);
        xm6_set_last_err(b, Z8_XM6_ERR_TIMEOUT);
    }

    uint32_t st = slots->req.dsp_status;
    __atomic_store_n(&b->last_dsp_status, st, __ATOMIC_RELEASE);
    if (dsp_status_out) {
        *dsp_status_out = st;
    }
    return Z8_XM6_ERR_TIMEOUT;
}

/* ---- Internal Dispatchers for Slot 41 (HDF) & Slot 39 (Grain) ---- */

static uint16_t xm6_dispatch_hdf_dsp_locked(struct z8_xm6_bridge *b,
                                            uint32_t src_y_dsp, uint32_t src_c_dsp,
                                            uint32_t dst_y_dsp, uint32_t dst_c_dsp,
                                            uint32_t width, uint32_t height,
                                            uint32_t y_stride, uint32_t c_stride,
                                            uint8_t hdf_level, uint8_t hdf_mode,
                                            uint32_t fmt_code,
                                            uint32_t ext_ws_dsp_va, uint32_t ext_ws_bytes,
                                            uint32_t timeout_ms, int is_live,
                                            uint32_t *dsp_status_out,
                                            uint32_t *elapsed_us_out,
                                            int *hung_dma_out)
{
    Z8Xm6DmaSlots *slots = xm6_slots(b);
    uint32_t in_dsp = 0U, out_dsp = 0U;

    if (ext_ws_dsp_va == 0U || (ext_ws_dsp_va & 63U) != 0U ||
        ext_ws_bytes < Z8_XM6_HDF_EXT_WS_BYTES) {
        xm6_set_last_err(b, Z8_XM6_ERR_IOMMU_OOB);
        return Z8_XM6_ERR_IOMMU_OOB;
    }
    if (!z8_xm6_core0_arm_to_dsp32(b, (uint64_t)(uintptr_t)&slots->hdf_in,
                                   (uint32_t)sizeof(slots->hdf_in), &in_dsp) ||
        !z8_xm6_core0_arm_to_dsp32(b, (uint64_t)(uintptr_t)&slots->out_param,
                                   (uint32_t)sizeof(slots->out_param), &out_dsp)) {
        xm6_set_last_err(b, Z8_XM6_ERR_IOMMU_OOB);
        return Z8_XM6_ERR_IOMMU_OOB;
    }

    memset(&slots->hdf_in, 0, sizeof(slots->hdf_in));
    slots->hdf_in.src.plane_addr[0] = (uint64_t)src_y_dsp;
    slots->hdf_in.src.plane_addr[1] = (uint64_t)src_c_dsp;
    slots->hdf_in.src.width         = (int32_t)width;
    slots->hdf_in.src.height        = (int32_t)height;
    slots->hdf_in.src.stride[0]     = (int32_t)y_stride;
    slots->hdf_in.src.stride[1]     = (int32_t)c_stride;
    slots->hdf_in.src.format        = fmt_code;

    slots->hdf_in.bypass_flag    = 0;
    slots->hdf_in.level          = hdf_level;
    slots->hdf_in.monochrome     = 0U;
    slots->hdf_in.mode           = hdf_mode;
    slots->hdf_in.knee_q16       = 0U;
    slots->hdf_in.neutrality_q8  = 0xFFFFU;
    slots->hdf_in.ev_gain_q8     = 256U;
    slots->hdf_in.white[0]       = 255U;
    slots->hdf_in.white[1]       = 255U;
    slots->hdf_in.white[2]       = 255U;
    /* P0 Silicon Safety: ALWAYS pass dedicated 64B-aligned 512 KiB Class 0x24 workspace
     * (NEVER 0, which clobbers Nikon FreeRTOS tsk0esc/tsk1esc @ 0x41940000..0x419A0000!). */
    slots->hdf_in.ext_work_addr  = (uint64_t)ext_ws_dsp_va;
    slots->hdf_in.ext_work_bytes = ext_ws_bytes;

    memset(&slots->out_param, 0, sizeof(slots->out_param));
    slots->out_param.plane_addr[0] = (uint64_t)dst_y_dsp;
    slots->out_param.plane_addr[1] = (uint64_t)dst_c_dsp;
    slots->out_param.width         = (int32_t)width;
    slots->out_param.height        = (int32_t)height;
    slots->out_param.stride[0]     = (int32_t)y_stride;
    slots->out_param.stride[1]     = (int32_t)c_stride;
    slots->out_param.format        = fmt_code;

    z8_xm6_dcache_civac(b, &slots->hdf_in, (uint32_t)sizeof(slots->hdf_in));
    z8_xm6_dcache_civac(b, &slots->out_param, (uint32_t)sizeof(slots->out_param));

    return xm6_submit_locked(b, Z8_XM6_SLOT_HDF, in_dsp, out_dsp,
                             timeout_ms, is_live, dsp_status_out,
                             elapsed_us_out, hung_dma_out);
}

static uint16_t xm6_dispatch_hdf_locked(struct z8_xm6_bridge *b,
                                        uint64_t src_y_va, uint64_t src_c_va,
                                        uint64_t dst_y_va, uint64_t dst_c_va,
                                        uint32_t width, uint32_t height,
                                        uint32_t y_stride, uint32_t c_stride,
                                        uint32_t y_span, uint32_t c_span,
                                        uint8_t hdf_level, uint8_t hdf_mode,
                                        uint32_t fmt_code,
                                        uint32_t ext_ws_dsp_va, uint32_t ext_ws_bytes,
                                        uint32_t timeout_ms, int is_live,
                                        uint32_t *dsp_status_out,
                                        uint32_t *elapsed_us_out,
                                        int *hung_dma_out)
{
    uint32_t src_y_dsp = 0U, src_c_dsp = 0U, dst_y_dsp = 0U, dst_c_dsp = 0U;
    if (!z8_xm6_core0_arm_to_dsp32(b, src_y_va, y_span, &src_y_dsp) ||
        !z8_xm6_core0_arm_to_dsp32(b, src_c_va, c_span, &src_c_dsp) ||
        !z8_xm6_core0_arm_to_dsp32(b, dst_y_va, y_span, &dst_y_dsp) ||
        !z8_xm6_core0_arm_to_dsp32(b, dst_c_va, c_span, &dst_c_dsp)) {
        xm6_set_last_err(b, Z8_XM6_ERR_IOMMU_OOB);
        return Z8_XM6_ERR_IOMMU_OOB;
    }
    return xm6_dispatch_hdf_dsp_locked(b, src_y_dsp, src_c_dsp, dst_y_dsp, dst_c_dsp,
                                       width, height, y_stride, c_stride,
                                       hdf_level, hdf_mode, fmt_code,
                                       ext_ws_dsp_va, ext_ws_bytes,
                                       timeout_ms, is_live,
                                       dsp_status_out, elapsed_us_out, hung_dma_out);
}

static uint16_t xm6_dispatch_grain_locked(struct z8_xm6_bridge *b,
                                          uint64_t src_y_va, uint64_t dst_y_va,
                                          uint32_t width, uint32_t height,
                                          uint32_t y_stride, uint32_t y_span,
                                          uint8_t intensity, uint8_t size,
                                          uint8_t blur_mode, uint8_t reduced_res,
                                          uint32_t prng_seed,
                                          uint64_t map0_va, uint64_t map1_va,
                                          uint32_t timeout_ms,
                                          uint32_t *dsp_status_out,
                                          uint32_t *elapsed_us_out,
                                          int *hung_dma_out)
{
    if (hung_dma_out) {
        *hung_dma_out = 0;
    }
    Z8Xm6DmaSlots *slots = xm6_slots(b);
    uint32_t src_y_dsp = 0U, dst_y_dsp = 0U;
    uint32_t map0_dsp = 0U, map1_dsp = 0U;
    uint32_t in_dsp = 0U, out_dsp = 0U;

    if (!z8_xm6_core0_arm_to_dsp32(b, src_y_va, y_span, &src_y_dsp) ||
        !z8_xm6_core0_arm_to_dsp32(b, dst_y_va, y_span, &dst_y_dsp) ||
        !z8_xm6_core0_arm_to_dsp32(b, map0_va, Z8_XM6_ZFG1_MAP_BYTES, &map0_dsp) ||
        !z8_xm6_core0_arm_to_dsp32(b, map1_va, Z8_XM6_ZFG1_MAP_BYTES, &map1_dsp) ||
        !z8_xm6_core0_arm_to_dsp32(b, (uint64_t)(uintptr_t)&slots->grain_in,
                                   (uint32_t)sizeof(slots->grain_in), &in_dsp) ||
        !z8_xm6_core0_arm_to_dsp32(b, (uint64_t)(uintptr_t)&slots->out_param,
                                   (uint32_t)sizeof(slots->out_param), &out_dsp)) {
        xm6_set_last_err(b, Z8_XM6_ERR_IOMMU_OOB);
        return Z8_XM6_ERR_IOMMU_OOB;
    }

    memset(&slots->grain_in, 0, sizeof(slots->grain_in));
    slots->grain_in.src.plane_addr[0] = (uint64_t)src_y_dsp;
    slots->grain_in.src.width         = (int32_t)width;
    slots->grain_in.src.height        = (int32_t)height;
    slots->grain_in.src.stride[0]     = (int32_t)y_stride;
    slots->grain_in.src.format        = Z8_XM6_FMT_Y_ONLY;

    z8_xm6_fill_grain_preset(&slots->grain_in, intensity, size, reduced_res,
                             blur_mode, prng_seed, map0_dsp, map1_dsp);

    memset(&slots->out_param, 0, sizeof(slots->out_param));
    slots->out_param.plane_addr[0] = (uint64_t)dst_y_dsp;
    slots->out_param.width         = (int32_t)width;
    slots->out_param.height        = (int32_t)height;
    slots->out_param.stride[0]     = (int32_t)y_stride;
    slots->out_param.format        = Z8_XM6_FMT_Y_ONLY;

    z8_xm6_dcache_civac(b, &slots->grain_in, (uint32_t)sizeof(slots->grain_in));
    z8_xm6_dcache_civac(b, &slots->out_param, (uint32_t)sizeof(slots->out_param));

    return xm6_submit_locked(b, Z8_XM6_SLOT_GRAIN_STILL, in_dsp, out_dsp,
                             timeout_ms, 0, dsp_status_out, elapsed_us_out, hung_dma_out);
}

/* ---- Still Photo Integration Entry Points ---- */

int z8_xm6_still_hdf(struct z8_xm6_bridge *b, uint8_t xm6_mode,
                     uint64_t src_y_dma, uint64_t src_c_dma,
                     uint64_t dst_y_dma, uint64_t dst_c_dma,
                     uint32_t width, uint32_t height,
                     uint32_t y_stride, uint32_t c_stride,
                     uint32_t y_span, uint32_t c_span,
                     uint8_t hdf_level, uint32_t fmt_code,
                     uint32_t *elapsed_us_out)
{
    if (elapsed_us_out) {
        *elapsed_us_out = 0U;
    }
    if (!b || xm6_mode != Z8_XM6_MODE_AUTO) {
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    if (!xm6_core0_hw_present_ready(b, xm6_mode) ||
        (__atomic_load_n(&b->xm6_busy, __ATOMIC_ACQUIRE) == 0U &&
         !z8_xm6_core0_mailbox_idle(b))) {
        z8_xm6_record_fallback(b, Z8_XM6_ERR_NOT_READY);
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    if (!width || !height || width > 8256U || height > 8256U || (width & 3U) != 0U ||
        y_stride < width || c_stride < width ||
        (fmt_code != Z8_XM6_FMT_NV16 && fmt_code != Z8_XM6_FMT_NV12) ||
        hdf_level < Z8_HDF_WEAK || hdf_level > Z8_HDF_STRONG) {
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    uint32_t ch = (fmt_code == Z8_XM6_FMT_NV12) ? ((height + 1U) >> 1) : height;
    if ((uint64_t)y_span < (uint64_t)(height - 1U) * y_stride + width ||
        (uint64_t)c_span < (uint64_t)(ch - 1U) * c_stride + width) {
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    /* Pre-check plane IOMMU bounds before acquiring Core 0 or lazily allocating ext_ws */
    uint32_t src_y_dsp = 0U, src_c_dsp = 0U, dst_y_dsp = 0U, dst_c_dsp = 0U;
    if (!z8_xm6_core0_arm_to_dsp32(b, src_y_dma, y_span, &src_y_dsp) ||
        !z8_xm6_core0_arm_to_dsp32(b, src_c_dma, c_span, &src_c_dsp) ||
        !z8_xm6_core0_arm_to_dsp32(b, dst_y_dma, y_span, &dst_y_dsp) ||
        !z8_xm6_core0_arm_to_dsp32(b, dst_c_dma, c_span, &dst_c_dsp)) {
        z8_xm6_record_fallback(b, Z8_XM6_ERR_IOMMU_OOB);
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    __atomic_fetch_add(&b->still_in_flight, 1U, __ATOMIC_ACQ_REL);
    if (!xm6_try_acquire_core0(b, 30U)) {
        __atomic_fetch_sub(&b->still_in_flight, 1U, __ATOMIC_RELEASE);
        z8_xm6_record_fallback(b, Z8_XM6_ERR_BUSY);
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    if (!xm6_core0_hw_present_ready(b, xm6_mode) ||
        !xm6_wait_mailbox_idle_bounded(b, 2000U)) {
        xm6_release_core0(b);
        __atomic_fetch_sub(&b->still_in_flight, 1U, __ATOMIC_RELEASE);
        z8_xm6_record_fallback(b, Z8_XM6_ERR_NOT_READY);
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }

    uint32_t ext_ws_dsp = 0U;
    uint16_t ws_err = xm6_ensure_ext_ws_locked(b, &ext_ws_dsp);
    if (ws_err != Z8_XM6_ERR_OK) {
        xm6_release_core0(b);
        __atomic_fetch_sub(&b->still_in_flight, 1U, __ATOMIC_RELEASE);
        z8_xm6_record_fallback(b, ws_err);
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }

    uint32_t dsp_st = Z8_XM6_DSP_STATUS_SENTINEL;
    uint32_t dt_us = 0U;
    int hung_dma = 0;
    uint16_t err = xm6_dispatch_hdf_dsp_locked(b, src_y_dsp, src_c_dsp, dst_y_dsp, dst_c_dsp,
                                               width, height, y_stride, c_stride,
                                               hdf_level, Z8_XM6_HDF_MODE_STILL_GENERAL, fmt_code,
                                               ext_ws_dsp, Z8_XM6_HDF_EXT_WS_BYTES,
                                               Z8_XM6_TIMEOUT_STILL_MS, 0,
                                               &dsp_st, &dt_us, &hung_dma);
    xm6_release_core0(b);
    __atomic_fetch_sub(&b->still_in_flight, 1U, __ATOMIC_RELEASE);

    if (elapsed_us_out) {
        *elapsed_us_out = dt_us;
    }
    if (err == Z8_XM6_ERR_OK) {
        return Z8_XM6_STILL_OK;
    }
    __atomic_fetch_add(&b->fallback_cnt, 1U, __ATOMIC_RELAXED);
    if (hung_dma != 0) {
        __atomic_store_n(&b->still_hung_dma, 1U, __ATOMIC_RELEASE);
        return Z8_XM6_STILL_HUNG_DMA;
    }
    if (err == Z8_XM6_ERR_IOMMU_OOB || err == Z8_XM6_ERR_ENQUEUE_FAIL ||
        err == Z8_XM6_ERR_ALLOC_FAIL ||
        (err == Z8_XM6_ERR_DSP_ERR && dsp_st == Z8_XM6_DSP_ERR_BAD_PARAM)) {
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    return Z8_XM6_STILL_FALLBACK_DIRTY;
}

int z8_xm6_still_grain(struct z8_xm6_bridge *b, uint8_t xm6_mode,
                       uint64_t src_y_dma, uint64_t dst_y_dma,
                       uint32_t width, uint32_t height,
                       uint32_t y_stride, uint32_t y_span,
                       uint8_t intensity, uint8_t size,
                       uint8_t blur_mode, uint8_t reduced_res,
                       uint32_t prng_seed, const void *zf_raw_buf,
                       uint32_t *elapsed_us_out)
{
    if (elapsed_us_out) {
        *elapsed_us_out = 0U;
    }
    if (!b || xm6_mode != Z8_XM6_MODE_AUTO || !zf_raw_buf) {
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    if (!xm6_core0_hw_present_ready(b, xm6_mode) ||
        (__atomic_load_n(&b->xm6_busy, __ATOMIC_ACQUIRE) == 0U &&
         !z8_xm6_core0_mailbox_idle(b))) {
        z8_xm6_record_fallback(b, Z8_XM6_ERR_NOT_READY);
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    if (!width || !height || width > 8256U || height > 8256U || y_stride < width ||
        (uint64_t)y_span < (uint64_t)(height - 1U) * y_stride + width ||
        intensity == 0U || intensity > Z8_ZF_INTENSITY_MAX ||
        size < Z8_ZF_SIZE_MIN || size > Z8_ZF_SIZE_MAX) {
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    __atomic_fetch_add(&b->still_in_flight, 1U, __ATOMIC_ACQ_REL);
    if (!xm6_try_acquire_core0(b, 30U)) {
        __atomic_fetch_sub(&b->still_in_flight, 1U, __ATOMIC_RELEASE);
        z8_xm6_record_fallback(b, Z8_XM6_ERR_BUSY);
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    if (!xm6_core0_hw_present_ready(b, xm6_mode) ||
        !xm6_wait_mailbox_idle_bounded(b, 2000U)) {
        xm6_release_core0(b);
        __atomic_fetch_sub(&b->still_in_flight, 1U, __ATOMIC_RELEASE);
        z8_xm6_record_fallback(b, Z8_XM6_ERR_NOT_READY);
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }

    uint64_t raw_base = (uint64_t)(uintptr_t)zf_raw_buf;
    uint64_t map0_va = raw_base + Z8_XM6_ZFG1_HEADER_BYTES;
    uint64_t map1_va = raw_base + Z8_XM6_ZFG1_HEADER_BYTES + Z8_XM6_ZFG1_MAP_BYTES;

    uint32_t dsp_st = Z8_XM6_DSP_STATUS_SENTINEL;
    uint32_t dt_us = 0U;
    int hung_dma = 0;
    uint16_t err = xm6_dispatch_grain_locked(b, src_y_dma, dst_y_dma,
                                             width, height, y_stride, y_span,
                                             intensity, size, blur_mode, reduced_res,
                                             prng_seed, map0_va, map1_va,
                                             Z8_XM6_TIMEOUT_STILL_MS, &dsp_st, &dt_us,
                                             &hung_dma);
    xm6_release_core0(b);
    __atomic_fetch_sub(&b->still_in_flight, 1U, __ATOMIC_RELEASE);

    if (elapsed_us_out) {
        *elapsed_us_out = dt_us;
    }
    if (err == Z8_XM6_ERR_OK) {
        return Z8_XM6_STILL_OK;
    }
    __atomic_fetch_add(&b->fallback_cnt, 1U, __ATOMIC_RELAXED);
    if (hung_dma != 0) {
        __atomic_store_n(&b->still_hung_dma, 1U, __ATOMIC_RELEASE);
        return Z8_XM6_STILL_HUNG_DMA;
    }
    if (err == Z8_XM6_ERR_IOMMU_OOB || err == Z8_XM6_ERR_ENQUEUE_FAIL ||
        (err == Z8_XM6_ERR_DSP_ERR && dsp_st == Z8_XM6_DSP_ERR_BAD_PARAM)) {
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    return Z8_XM6_STILL_FALLBACK_DIRTY;
}

/* ---- Live Mode 2 & Layer 0 LCD Preview NV12 HDF Integration Entry Points ---- */

static int xm6_live_or_preview_hdf_nv12(struct z8_xm6_bridge *b, uint8_t xm6_mode,
                                        uint8_t *y_cpu, uint8_t *c_cpu,
                                        uint64_t y_dma, uint64_t c_dma,
                                        uint32_t width, uint32_t height,
                                        uint32_t y_stride, uint32_t c_stride,
                                        uint8_t hdf_level, int is_preview)
{
    if (!b || xm6_mode != Z8_XM6_MODE_AUTO) {
        return Z8_XM6_LIVE_FALLBACK;
    }
    /* V71.7-r7: Check hardware presence ONLY before lock (NEVER mailbox_idle before acquiring
     * xm6_busy!), so concurrent Live/Preview callers wait cleanly on xm6_busy instead of
     * immediately failing with ERR_NOT_READY(5) while the peer holds Core 0! */
    if (!xm6_core0_hw_present_ready(b, xm6_mode)) {
        z8_xm6_record_fallback(b, Z8_XM6_ERR_NOT_READY);
        return Z8_XM6_LIVE_FALLBACK;
    }
    if (is_preview) {
        /* Preview-only 5s auto-recovering cooldown; NEVER reads/writes live_soft_disabled! */
        uint64_t p_until = __atomic_load_n(&b->preview_backoff_until_us, __ATOMIC_ACQUIRE);
        if (p_until != 0U) {
            uint64_t now = hw_time_us(b);
            if (now < p_until) {
                __atomic_fetch_add(&b->fallback_cnt, 1U, __ATOMIC_RELAXED);
                return Z8_XM6_LIVE_FALLBACK;
            }
            __atomic_store_n(&b->preview_backoff_until_us, 0U, __ATOMIC_RELEASE);
        }
    } else {
        /* 2-Tier Soft Live Backoff: skip Live submit while backed off or soft-disabled,
         * keeping global breaker_tripped == 0 so Still HDF/Grain & Op12 stay active. */
        if (__atomic_load_n(&b->live_soft_disabled, __ATOMIC_ACQUIRE) != 0U) {
            __atomic_fetch_add(&b->fallback_cnt, 1U, __ATOMIC_RELAXED);
            return Z8_XM6_LIVE_FALLBACK;
        }
        uint64_t backoff_until = __atomic_load_n(&b->live_backoff_until_us, __ATOMIC_ACQUIRE);
        if (backoff_until != 0U) {
            uint64_t now = hw_time_us(b);
            if (now < backoff_until) {
                __atomic_fetch_add(&b->fallback_cnt, 1U, __ATOMIC_RELAXED);
                return Z8_XM6_LIVE_FALLBACK;
            }
            __atomic_store_n(&b->live_backoff_until_us, 0U, __ATOMIC_RELEASE);
        }
    }
    /* V71.7-r5:
     *   - 1024x{680,682} NV12: Slot 41 LIVE_256_YC (both Live Photo and Layer 0 LCD).
     *   - 1280x{850,852,854,856} NV12: Slot 41 LIVE_EVF_256_YC (Layer 0 EVF preview only;
     *     dcli-verified 0-diff vs ARM z8_hdf_live_nv12_1280). Live Photo stays 1024 only. */
    int is_live_1024 = (width == 1024U && (height == 680U || height == 682U));
    int is_evf_1280  = (is_preview != 0 && width == 1280U &&
                        height >= 850U && height <= 856U && (height & 1U) == 0U);
    if (!y_cpu || !c_cpu || (!is_live_1024 && !is_evf_1280) ||
        y_stride < width || c_stride < width ||
        hdf_level < Z8_HDF_WEAK || hdf_level > Z8_HDF_STRONG) {
        return Z8_XM6_LIVE_FALLBACK;
    }
    uint8_t hdf_mode = is_evf_1280 ? (uint8_t)Z8_XM6_HDF_MODE_LIVE_EVF_256_YC
                                   : (uint8_t)Z8_XM6_HDF_MODE_LIVE_256_YC;

    uint32_t y_span = y_stride * height;
    uint32_t c_span = c_stride * (height >> 1);
    uint64_t y_addr = y_dma ? y_dma : (uint64_t)(uintptr_t)y_cpu;
    uint64_t c_addr = c_dma ? c_dma : (uint64_t)(uintptr_t)c_cpu;

    /* Pre-check & hoist IOMMU translation before acquiring Core 0, ext_ws, or D-Cache clean */
    uint32_t y_dsp = 0U, c_dsp = 0U;
    if (!z8_xm6_core0_arm_to_dsp32(b, y_addr, y_span, &y_dsp) ||
        !z8_xm6_core0_arm_to_dsp32(b, c_addr, c_span, &c_dsp)) {
        z8_xm6_record_fallback(b, Z8_XM6_ERR_IOMMU_OOB);
        return Z8_XM6_LIVE_FALLBACK;
    }

    /* V71.7-r7: Bounded Core 0 wait (16 ms preview / 16 ms Live), while fast-skipping
     * immediately (0 ms) whenever a 45MP still (still_in_flight) or pool_suspend is active. */
    if (__atomic_load_n(&b->pool_suspended, __ATOMIC_ACQUIRE) != 0U) {
        return Z8_XM6_LIVE_FALLBACK;
    }
    uint32_t still_busy = __atomic_load_n(&b->still_in_flight, __ATOMIC_ACQUIRE);
    if (still_busy != 0U && __atomic_load_n(&b->xm6_busy, __ATOMIC_ACQUIRE) != 0U) {
        z8_xm6_record_fallback(b, Z8_XM6_ERR_BUSY);
        return Z8_XM6_LIVE_FALLBACK;
    }

    /* Hoist pre-submit D-Cache clean+invalidate BEFORE xm6_busy (buffers are caller-private),
     * overlapping 0.2 ms cache flush with any active peer XM6 job and shortening lock hold! */
    z8_xm6_dcache_civac(b, y_cpu, y_span);
    z8_xm6_dcache_civac(b, c_cpu, c_span);

    uint32_t wait_ms = (still_busy != 0U)
                           ? 0U
                           : (is_preview ? Z8_XM6_PREVIEW_WAIT_MS : Z8_XM6_LIVE_WAIT_MS);
    if (!xm6_try_acquire_core0_live(b, wait_ms)) {
        z8_xm6_record_fallback(b, Z8_XM6_ERR_BUSY);
        return Z8_XM6_LIVE_FALLBACK;
    }
    if (!xm6_core0_hw_present_ready(b, xm6_mode) ||
        !xm6_wait_mailbox_idle_bounded(b, 2000U)) {
        xm6_release_core0(b);
        z8_xm6_record_fallback(b, Z8_XM6_ERR_NOT_READY);
        return Z8_XM6_LIVE_FALLBACK;
    }

    uint32_t ext_ws_dsp = 0U;
    uint16_t ws_err = xm6_ensure_ext_ws_locked(b, &ext_ws_dsp);
    if (ws_err != Z8_XM6_ERR_OK) {
        xm6_release_core0(b);
        z8_xm6_record_fallback(b, ws_err);
        return Z8_XM6_LIVE_FALLBACK;
    }

    uint32_t timeout_ms = is_preview ? (is_evf_1280 ? 30U : 25U) : (uint32_t)Z8_XM6_TIMEOUT_LIVE_MS;
    int live_kind = is_preview ? 2 : 1;

    uint32_t dsp_st = Z8_XM6_DSP_STATUS_SENTINEL;
    uint32_t dt_us = 0U;
    int hung_dma = 0;
    uint16_t err = xm6_dispatch_hdf_dsp_locked(b, y_dsp, c_dsp, y_dsp, c_dsp,
                                               width, height, y_stride, c_stride,
                                               hdf_level, hdf_mode,
                                               Z8_XM6_FMT_NV12,
                                               ext_ws_dsp, Z8_XM6_HDF_EXT_WS_BYTES,
                                               timeout_ms, live_kind,
                                               &dsp_st, &dt_us, &hung_dma);
    xm6_release_core0(b);

    if (err == Z8_XM6_ERR_OK) {
        if (is_preview) {
            __atomic_store_n(&b->preview_timeout_streak, 0U, __ATOMIC_RELEASE);
            __atomic_store_n(&b->preview_backoff_until_us, 0U, __ATOMIC_RELEASE);
        } else {
            /* Successful Live HDF resets soft backoff streak */
            __atomic_store_n(&b->live_timeout_streak, 0U, __ATOMIC_RELEASE);
            __atomic_store_n(&b->live_backoff_until_us, 0U, __ATOMIC_RELEASE);
            __atomic_store_n(&b->live_soft_disabled, 0U, __ATOMIC_RELEASE);
        }
        /* Post-submit: invalidate CPU cache so CPU0/Layer0 reads fresh DRAM output */
        z8_xm6_dcache_civac(b, y_cpu, y_span);
        z8_xm6_dcache_civac(b, c_cpu, c_span);
        uint32_t clamped_us = (dt_us > 65535U) ? 65535U : dt_us;
        __atomic_store_n(&b->last_live_us, clamped_us, __ATOMIC_RELAXED);
        __atomic_fetch_add(&b->live_ok, 1U, __ATOMIC_RELAXED);
        return Z8_XM6_LIVE_OK;
    }

    __atomic_fetch_add(&b->fallback_cnt, 1U, __ATOMIC_RELAXED);
    if (err == Z8_XM6_ERR_IOMMU_OOB || err == Z8_XM6_ERR_ENQUEUE_FAIL ||
        err == Z8_XM6_ERR_ALLOC_FAIL ||
        (err == Z8_XM6_ERR_DSP_ERR && dsp_st == Z8_XM6_DSP_ERR_BAD_PARAM)) {
        return Z8_XM6_LIVE_FALLBACK;
    }
    if (hung_dma != 0) {
        /* True DSP hang (not drained after 120 ms): DO NOT dcache_civac live DMA buffer;
         * caller MUST quarantine this buffer/bank instead of reusing or freeing it! */
        return Z8_XM6_LIVE_HUNG_DMA;
    }
    /* Drained timeout / DSP error: invalidate cache and skip 1 torn frame */
    z8_xm6_dcache_civac(b, y_cpu, y_span);
    z8_xm6_dcache_civac(b, c_cpu, c_span);
    return Z8_XM6_LIVE_SKIP_DIRTY;
}

int z8_xm6_live_hdf_nv12(struct z8_xm6_bridge *b, uint8_t xm6_mode,
                         uint8_t *y_cpu, uint8_t *c_cpu,
                         uint64_t y_dma, uint64_t c_dma,
                         uint32_t width, uint32_t height,
                         uint32_t y_stride, uint32_t c_stride,
                         uint8_t hdf_level)
{
    return xm6_live_or_preview_hdf_nv12(b, xm6_mode, y_cpu, c_cpu, y_dma, c_dma,
                                        width, height, y_stride, c_stride, hdf_level, 0);
}

int z8_xm6_preview_hdf_nv12(struct z8_xm6_bridge *b, uint8_t xm6_mode,
                            uint8_t *y_cpu, uint8_t *c_cpu,
                            uint64_t y_dma, uint64_t c_dma,
                            uint32_t width, uint32_t height,
                            uint32_t y_stride, uint32_t c_stride,
                            uint8_t hdf_level)
{
    return xm6_live_or_preview_hdf_nv12(b, xm6_mode, y_cpu, c_cpu, y_dma, c_dma,
                                        width, height, y_stride, c_stride, hdf_level, 1);
}

/* ---- Stock Core 0 Opcode 12 (RGBLUT) 1D Separable .cube Dispatcher ---- */

static inline uint64_t xm6_strip_cpu_alias(uint64_t arm_va)
{
    return (arm_va >= UINT64_C(0x0c40000000) && arm_va < UINT64_C(0x0cc0000000))
               ? (arm_va - UINT64_C(0x400000000))
               : arm_va;
}

static inline int xm6_spans_overlap(uint64_t a, uint64_t a_bytes,
                                    uint64_t b, uint64_t b_bytes)
{
    return a < b + b_bytes && b < a + a_bytes;
}

uint32_t z8_xm6_rgblut_arena_bytes(uint32_t pitch)
{
    if (pitch == 0U || pitch > Z8_XM6_RGBLUT_MAX_PITCH) {
        return 0U;
    }
    uint32_t work = Z8_XM6_RGBLUT_WORK_BASE + Z8_XM6_RGBLUT_WORK_PER_PITCH * pitch;
    return (Z8_XM6_RGBLUT_CONTROL_BYTES + work + 63U) & ~63U;
}

int z8_xm6_rgblut_nv16(struct z8_xm6_bridge *b, uint8_t xm6_mode,
                       const Z8Xm6RgblutCurves *curves,
                       const uint8_t *src_y_cpu, const uint8_t *src_c_cpu,
                       uint64_t src_y_dma, uint64_t src_c_dma,
                       uint8_t *dst_y_cpu, uint8_t *dst_c_cpu,
                       uint64_t dst_y_dma, uint64_t dst_c_dma,
                       uint32_t width, uint32_t height,
                       uint32_t pitch,
                       uint32_t *elapsed_us_out)
{
    static const int16_t k_front[12] = {
        1024, 0, 1436, 1024, -352, -731, 1024, 1815, 0, 0, 0, 0
    };
    static const int16_t k_back[12] = {
        306, 601, 117, -173, -339, 512, 512, -429, -83, 0, 0, 0
    };

    if (elapsed_us_out) {
        *elapsed_us_out = 0U;
    }
    if (!b || xm6_mode != Z8_XM6_MODE_AUTO || !curves) {
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    if (!xm6_core0_hw_present_ready(b, xm6_mode) ||
        (__atomic_load_n(&b->xm6_busy, __ATOMIC_ACQUIRE) == 0U &&
         !z8_xm6_core0_mailbox_idle(b))) {
        z8_xm6_record_fallback(b, Z8_XM6_ERR_NOT_READY);
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    if (curves->source[0] > 2U || curves->source[1] > 2U || curves->source[2] > 2U) {
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    if (width < 2U || (width & 1U) != 0U || width > Z8_XM6_RGBLUT_MAX_PITCH ||
        height < 8U || (height & 7U) != 0U || height > Z8_XM6_RGBLUT_MAX_HEIGHT ||
        pitch < width || pitch > Z8_XM6_RGBLUT_MAX_PITCH || (pitch & 63U) != 0U) {
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }

    uint64_t plane_bytes64 = (uint64_t)pitch * (uint64_t)height;
    if (plane_bytes64 == 0U || plane_bytes64 > UINT32_MAX) {
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    uint32_t plane_bytes = (uint32_t)plane_bytes64;

    uint64_t sy = xm6_strip_cpu_alias(src_y_dma ? src_y_dma : (uint64_t)(uintptr_t)src_y_cpu);
    uint64_t sc = xm6_strip_cpu_alias(src_c_dma ? src_c_dma : (uint64_t)(uintptr_t)src_c_cpu);
    uint64_t dy = xm6_strip_cpu_alias(dst_y_dma ? dst_y_dma : (uint64_t)(uintptr_t)dst_y_cpu);
    uint64_t dc = xm6_strip_cpu_alias(dst_c_dma ? dst_c_dma : (uint64_t)(uintptr_t)dst_c_cpu);
    const uint64_t planes[4] = { sy, sc, dy, dc };

    /* Validate 64B align, single 512 MiB window, Core 0 IOMMU containment & disjointness */
    for (unsigned i = 0U; i < 4U; ++i) {
        uint32_t tmp_dsp = 0U;
        if (!planes[i] || (planes[i] & 63U) != 0U ||
            z8_fs_plat_span_straddles_512m(planes[i], plane_bytes) ||
            !z8_xm6_core0_arm_to_dsp32(b, planes[i], plane_bytes, &tmp_dsp)) {
            z8_xm6_record_fallback(b, Z8_XM6_ERR_IOMMU_OOB);
            return Z8_XM6_STILL_FALLBACK_CLEAN;
        }
        for (unsigned j = 0U; j < i; ++j) {
            if (xm6_spans_overlap(planes[i], plane_bytes, planes[j], plane_bytes)) {
                return Z8_XM6_STILL_FALLBACK_CLEAN;
            }
        }
    }

    uint32_t work_bytes = Z8_XM6_RGBLUT_WORK_BASE + Z8_XM6_RGBLUT_WORK_PER_PITCH * pitch;
    uint32_t arena_bytes = z8_xm6_rgblut_arena_bytes(pitch);
    if (!arena_bytes) {
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }

    struct z8_nfsp_allocation arena_blk;
    memset(&arena_blk, 0, sizeof(arena_blk));
    if (!hw_alloc_class24_xm6(b, arena_bytes, &arena_blk) ||
        !arena_blk.cpu || !arena_blk.dma || arena_blk.actual < arena_bytes) {
        if (arena_blk.handle || arena_blk.cpu) {
            hw_release_class24(b, &arena_blk);
        }
        z8_xm6_record_fallback(b, Z8_XM6_ERR_ALLOC_FAIL);
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }

    uint64_t arena_dma = xm6_strip_cpu_alias(arena_blk.dma);
    uint32_t tmp_arena_dsp = 0U;
    if ((arena_dma & 63U) != 0U ||
        z8_fs_plat_span_straddles_512m(arena_dma, arena_bytes) ||
        !z8_xm6_core0_arm_to_dsp32(b, arena_dma, arena_bytes, &tmp_arena_dsp)) {
        hw_release_class24(b, &arena_blk);
        z8_xm6_record_fallback(b, Z8_XM6_ERR_IOMMU_OOB);
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }
    for (unsigned i = 0U; i < 4U; ++i) {
        if (xm6_spans_overlap(planes[i], plane_bytes, arena_dma, arena_bytes)) {
            hw_release_class24(b, &arena_blk);
            z8_xm6_record_fallback(b, Z8_XM6_ERR_IOMMU_OOB);
            return Z8_XM6_STILL_FALLBACK_CLEAN;
        }
    }

    __atomic_fetch_add(&b->still_in_flight, 1U, __ATOMIC_ACQ_REL);
    if (!xm6_try_acquire_core0(b, 30U)) {
        __atomic_fetch_sub(&b->still_in_flight, 1U, __ATOMIC_RELEASE);
        hw_release_class24(b, &arena_blk);
        z8_xm6_record_fallback(b, Z8_XM6_ERR_BUSY);
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }

    /* Re-verify hardware & stock mailbox idle words under xm6_busy lock (no TOCTOU) */
    if (!xm6_core0_hw_present_ready(b, xm6_mode) ||
        !xm6_wait_mailbox_idle_bounded(b, 2000U)) {
        xm6_release_core0(b);
        __atomic_fetch_sub(&b->still_in_flight, 1U, __ATOMIC_RELEASE);
        hw_release_class24(b, &arena_blk);
        z8_xm6_record_fallback(b, Z8_XM6_ERR_NOT_READY);
        return Z8_XM6_STILL_FALLBACK_CLEAN;
    }

    uint8_t *arena_cpu = arena_blk.cpu;
    memset(arena_cpu, 0, Z8_XM6_RGBLUT_CONTROL_BYTES);

    Z8Xm6RgblutJob   *job = (Z8Xm6RgblutJob *)(void *)arena_cpu;
    Z8Xm6RgblutParam *in  = (Z8Xm6RgblutParam *)(void *)(arena_cpu + 0x200U);
    Z8Xm6ImageParam  *out = (Z8Xm6ImageParam *)(void *)(arena_cpu + 0xa00U);

    uint64_t expected_in_dma   = arena_dma + 0x200U;
    uint64_t expected_out_dma  = arena_dma + 0xa00U;
    uint64_t expected_work_dma = arena_dma + Z8_XM6_RGBLUT_CONTROL_BYTES;

    Z8Xm6ImageParam expected_src;
    memset(&expected_src, 0, sizeof(expected_src));
    expected_src.plane_addr[0] = sy;
    expected_src.plane_addr[1] = sc;
    expected_src.width         = (int32_t)width;
    expected_src.height        = (int32_t)height;
    expected_src.stride[0]     = (int32_t)pitch;
    expected_src.stride[1]     = (int32_t)pitch;
    expected_src.format        = Z8_XM6_FMT_NV16;

    Z8Xm6ImageParam expected_dst = expected_src;
    expected_dst.plane_addr[0]   = dy;
    expected_dst.plane_addr[1]   = dc;

    in->image      = expected_src;
    in->mode       = 0x40U;
    in->work       = expected_work_dma;
    in->work_bytes = work_bytes;
    for (unsigned c = 0U; c < 3U; ++c) {
        memcpy(in->front + c * 3U, k_front + (unsigned)curves->source[c] * 3U,
               3U * sizeof(int16_t));
    }
    in->front[9] = 0;
    in->front[10] = 0;
    in->front[11] = 0;
    memcpy(in->back, k_back, sizeof(k_back));
    memcpy(in->curve, curves->values, sizeof(in->curve));

    *out = expected_dst;

    memset(job, 0, sizeof(*job));
    job->opcode  = Z8_XM6_SLOT_RGBLUT;
    job->in_ptr  = expected_in_dma;
    job->out_ptr = expected_out_dma;
    job->lane    = 1U;
    job->status  = Z8_XM6_RGBLUT_STATUS_INIT;

    /* Pre-submit D-cache maintenance on control block & planes */
    z8_xm6_dcache_civac(b, arena_cpu, Z8_XM6_RGBLUT_CONTROL_BYTES);
    if (src_y_cpu) {
        z8_xm6_dcache_civac(b, (void *)(uintptr_t)src_y_cpu, plane_bytes);
    }
    if (src_c_cpu) {
        z8_xm6_dcache_civac(b, (void *)(uintptr_t)src_c_cpu, plane_bytes);
    }
    if (dst_y_cpu) {
        z8_xm6_dcache_civac(b, dst_y_cpu, plane_bytes);
    }
    if (dst_c_cpu) {
        z8_xm6_dcache_civac(b, dst_c_cpu, plane_bytes);
    }

    xm6_flight_log(Z8_XM6_FLIGHT_SUBMIT, Z8_XM6_SLOT_RGBLUT,
                   (uint32_t)(arena_dma >> 12), width, height);
    uint64_t t0 = hw_time_us(b);
    uint32_t accepted = hw_sync_submit(b, job, arena_dma, Z8_XM6_RGBLUT_LOCK_MS);
    uint64_t t1 = hw_time_us(b);
    uint32_t dt = (t1 >= t0) ? (uint32_t)((t1 - t0) > UINT32_MAX ? UINT32_MAX : (t1 - t0)) : 0U;
    if (elapsed_us_out) {
        *elapsed_us_out = dt;
    }

    /* Post-submit D-cache invalidate on control block & destination planes */
    z8_xm6_dcache_civac(b, arena_cpu, Z8_XM6_RGBLUT_CONTROL_BYTES);
    if (dst_y_cpu) {
        z8_xm6_dcache_civac(b, dst_y_cpu, plane_bytes);
    }
    if (dst_c_cpu) {
        z8_xm6_dcache_civac(b, dst_c_cpu, plane_bytes);
    }

    uint32_t result = job->status;
    __atomic_store_n(&b->last_dsp_status, result, __ATOMIC_RELEASE);

    /* Wait bounded <= 2 ms for DspFramework post-callback mailbox cleanup (0x40c90fcc) */
    int mbox_idle = xm6_wait_mailbox_idle_bounded(b, 2000U);
    int uncertain = !(accepted & 1U) ||
                    result == Z8_XM6_RGBLUT_STATUS_INIT ||
                    !mbox_idle ||
                    job->opcode != Z8_XM6_SLOT_RGBLUT ||
                    job->in_ptr != expected_in_dma ||
                    job->out_ptr != expected_out_dma ||
                    job->lane != 1U ||
                    in->mode != 0x40U ||
                    in->work != expected_work_dma ||
                    in->work_bytes != work_bytes ||
                    memcmp(&in->image, &expected_src, sizeof(expected_src)) != 0 ||
                    memcmp(out, &expected_dst, sizeof(expected_dst)) != 0;

    xm6_release_core0(b);
    __atomic_fetch_sub(&b->still_in_flight, 1U, __ATOMIC_RELEASE);

    if (uncertain) {
        __atomic_store_n(&b->breaker_tripped, 1U, __ATOMIC_RELEASE);
        if (!mbox_idle) {
            __atomic_store_n(&b->still_hung_dma, 1U, __ATOMIC_RELEASE);
        }
        uint16_t err = (result == Z8_XM6_RGBLUT_STATUS_INIT || !mbox_idle)
                           ? Z8_XM6_ERR_TIMEOUT
                           : Z8_XM6_ERR_DSP_ERR;
        xm6_set_last_err(b, err);
        __atomic_fetch_add(&b->fallback_cnt, 1U, __ATOMIC_RELAXED);
        xm6_flight_log(Z8_XM6_FLIGHT_TIMEOUT, Z8_XM6_SLOT_RGBLUT, accepted, result, (uint32_t)mbox_idle);
#ifdef FILMSIM_HOST_TEST
        hw_release_class24(b, &arena_blk);
#else
        /* On bare metal, only free arena if DSP mailbox is proven idle. */
        if (mbox_idle) {
            hw_release_class24(b, &arena_blk);
        }
#endif
        return Z8_XM6_STILL_FALLBACK_DIRTY;
    }

    hw_release_class24(b, &arena_blk);
    if (result == Z8_XM6_DSP_OK) {
        return Z8_XM6_STILL_OK;
    }
    z8_xm6_record_fallback(b, Z8_XM6_ERR_ENQUEUE_FAIL);
    return Z8_XM6_STILL_FALLBACK_CLEAN;
}

/* ---- Telemetry & Control ---- */

void z8_xm6_record_still_ok(struct z8_xm6_bridge *b, uint64_t total_us)
{
    if (!b) {
        return;
    }
    uint64_t ms = (total_us + 500U) / 1000U;
    uint32_t clamped_ms = (ms > 65535U) ? 65535U : (uint32_t)ms;
    __atomic_store_n(&b->last_still_ms, clamped_ms, __ATOMIC_RELAXED);
    __atomic_fetch_add(&b->still_ok, 1U, __ATOMIC_RELAXED);
}

void z8_xm6_record_fallback(struct z8_xm6_bridge *b, uint16_t err_code)
{
    if (!b) {
        return;
    }
    xm6_set_last_err(b, err_code);
    __atomic_fetch_add(&b->fallback_cnt, 1U, __ATOMIC_RELAXED);
}

void z8_xm6_fill_status(const struct z8_xm6_bridge *b, uint8_t xm6_mode, Z8FilmSimStatus *st)
{
    if (!st) {
        return;
    }
    uint32_t hw_state = 0U;
    uint8_t loader_fault = 0U;
    uint8_t fw_loaded = 0U;
    z8_xm6_read_hw_raw(b, &hw_state, &loader_fault, &fw_loaded);

    uint8_t packed_hw = (uint8_t)(hw_state & Z8_XM6_HW_STATE_MASK);
    if (loader_fault != 0U) {
        packed_hw |= (uint8_t)Z8_XM6_HW_LOADER_FAULT_BIT;
    }
    st->xm6_hw_state = packed_hw;

    uint8_t rflags = 0U;
    if (xm6_mode == Z8_XM6_MODE_AUTO) {
        rflags |= (uint8_t)Z8_XM6_FLAG_AUTO_ENABLED;
    }
    if (loader_fault == 0U && fw_loaded != 0U && hw_state == 2U &&
        z8_xm6_core0_mailbox_idle(b)) {
        rflags |= (uint8_t)Z8_XM6_FLAG_HW_READY;
    }
    if (b) {
        if (__atomic_load_n(&b->breaker_tripped, __ATOMIC_ACQUIRE) != 0U) {
            rflags |= (uint8_t)Z8_XM6_FLAG_BREAKER_TRIPPED;
        }
        if (__atomic_load_n(&b->diag_ran, __ATOMIC_ACQUIRE) != 0U) {
            rflags |= (uint8_t)Z8_XM6_FLAG_DIAG_RAN;
        }
        if (__atomic_load_n(&b->diag_passed, __ATOMIC_ACQUIRE) != 0U) {
            rflags |= (uint8_t)Z8_XM6_FLAG_DIAG_PASSED;
        }
        st->xm6_last_err      = (uint16_t)__atomic_load_n(&b->last_err, __ATOMIC_ACQUIRE);
        st->xm6_last_still_ms = (uint16_t)__atomic_load_n(&b->last_still_ms, __ATOMIC_RELAXED);
        st->xm6_still_ok      = __atomic_load_n(&b->still_ok, __ATOMIC_RELAXED);
        st->xm6_live_ok       = __atomic_load_n(&b->live_ok, __ATOMIC_RELAXED);
        st->xm6_fallback_cnt  = __atomic_load_n(&b->fallback_cnt, __ATOMIC_RELAXED);
        st->xm6_last_live_us  = (uint16_t)__atomic_load_n(&b->last_live_us, __ATOMIC_RELAXED);
    } else {
        st->xm6_last_err      = Z8_XM6_ERR_OK;
        st->xm6_last_still_ms = 0U;
        st->xm6_still_ok      = 0U;
        st->xm6_live_ok       = 0U;
        st->xm6_fallback_cnt  = 0U;
        st->xm6_last_live_us  = 0U;
    }
    st->xm6_runtime_flags = rflags;
    st->xm6_reserved      = 0U;
}

void z8_xm6_clear_live_soft_backoff(struct z8_xm6_bridge *b)
{
    if (!b) {
        return;
    }
    __atomic_store_n(&b->live_soft_disabled, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&b->live_timeout_streak, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&b->live_backoff_until_us, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&b->live_flight_logged, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&b->preview_timeout_streak, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&b->preview_backoff_until_us, 0U, __ATOMIC_RELEASE);
}

void z8_xm6_set_pool_suspended(struct z8_xm6_bridge *b, uint32_t suspended)
{
    if (!b) {
        return;
    }
    __atomic_store_n(&b->pool_suspended, suspended ? 1U : 0U, __ATOMIC_RELEASE);
}

void z8_xm6_reset_breaker(struct z8_xm6_bridge *b)
{
    if (!b) {
        return;
    }
    /* Always clear soft Live backoff state */
    z8_xm6_clear_live_soft_backoff(b);
    /* Refuse to clear hard breaker_tripped while a DSP request is still in flight
     * or Core 0 hardware / mailbox is not healthy & idle! */
    if (__atomic_load_n(&b->req_in_flight, __ATOMIC_ACQUIRE) != 0U ||
        !z8_xm6_core0_hw_only_ready(b)) {
        return;
    }
    __atomic_store_n(&b->still_hung_dma, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&b->breaker_tripped, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&b->last_err, Z8_XM6_ERR_OK, __ATOMIC_RELEASE);
}

/* ---- On-Camera 0-Diff Off-Screen Self-Test (Z8_FS_OP_XM6_DIAG, op 10) ---- */

static void diag_fill_pattern(uint8_t *y, uint8_t *c, uint32_t w, uint32_t h, int is_nv12)
{
    /* Deterministic rich scene: bright highlight blocks (240..255), midtone gradients,
     * shadow blocks (10..30), and crisp chroma edges to exercise every Grain & HDF path. */
    for (uint32_t r = 0U; r < h; ++r) {
        uint8_t *yr = y + (size_t)r * w;
        for (uint32_t x = 0U; x < w; ++x) {
            uint32_t v;
            if ((x >= w / 8U && x < w / 4U && r >= h / 8U && r < h / 4U) ||
                (x >= (w * 5U) / 8U && x < (w * 6U) / 8U && r >= h / 3U && r < h / 2U)) {
                v = 242U + ((x + r) & 13U);
                if (v > 255U) {
                    v = 255U;
                }
            } else if (x < w / 3U && r >= (h * 2U) / 3U) {
                v = 12U + ((x ^ r) & 15U);
            } else {
                v = ((x * 3U + r * 5U) ^ ((x >> 2) * 17U)) & 255U;
            }
            yr[x] = (uint8_t)v;
        }
    }
    if (c) {
        uint32_t ch = is_nv12 ? (h >> 1) : h;
        for (uint32_t r = 0U; r < ch; ++r) {
            uint8_t *cr = c + (size_t)r * w;
            for (uint32_t x = 0U; x < w; x += 2U) {
                uint8_t cb = (uint8_t)(80U + (((x * 3U + r * 7U) + ((x & 32U) ? 64U : 0U)) & 95U));
                uint8_t cv = (uint8_t)(88U + (((x * 5U + r * 3U) + ((r & 16U) ? 48U : 0U)) & 95U));
                cr[x] = cb;
                cr[x + 1U] = cv;
            }
        }
    }
}

static void diag_fill_syn_grain_maps(int16_t *map0, int16_t *map1)
{
    uint32_t s = UINT32_C(0x5a38f117);
    uint32_t n = Z8_XM6_ZF_MAP_DIM * Z8_XM6_ZF_MAP_DIM;
    for (uint32_t i = 0U; i < n; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        int32_t v0 = (int32_t)(s & 1023U) - 512;
        int32_t v1 = (int32_t)((s >> 10) & 1023U) - 512;
        map0[i] = (int16_t)v0;
        map1[i] = (int16_t)v1;
    }
}

static uint32_t diag_count_diff(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint32_t diff = 0U;
    for (size_t i = 0U; i < n; ++i) {
        if (a[i] != b[i]) {
            ++diff;
        }
    }
    return diff;
}

void z8_xm6_run_diag_ex(struct z8_xm6_bridge *b,
                        const void *zf_raw_buf,
                        uint32_t zf_raw_loaded,
                        uint32_t caller_skip_reason,
                        Z8Xm6DiagResult *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->magic   = Z8_XM6_DIAG_MAGIC;
    out->version = (uint16_t)Z8_XM6_DIAG_VERSION;
    out->bytes   = (uint16_t)Z8_XM6_DIAG_BYTES;

    uint32_t hw_state = 0U;
    uint8_t loader_fault = 0U;
    uint8_t fw_loaded = 0U;
    z8_xm6_read_hw_raw(b, &hw_state, &loader_fault, &fw_loaded);

    out->hw_state        = (uint8_t)(hw_state & 0xFFU);
    out->loader_fault    = loader_fault;
    out->fw_loaded       = fw_loaded;
    out->breaker_tripped = (b && __atomic_load_n(&b->breaker_tripped, __ATOMIC_ACQUIRE)) ? 1U : 0U;

    /* Auto-clear soft Live backoff on every DIAG run */
    if (b) {
        z8_xm6_clear_live_soft_backoff(b);
    }

    /* Safe immediate return when hardware is absent (e.g. QEMU): keep dsp_status_last == 0 */
    if (loader_fault != 0U || fw_loaded == 0U || hw_state != 2U) {
        return;
    }

    /* Explicit skip reason codes (0xF1..0xF7) when HW is present (hw_state==2, !loader_fault) */
    if (caller_skip_reason != 0U) {
        out->dsp_status_last = caller_skip_reason;
        return;
    }
    if (!b) {
        out->dsp_status_last = Z8_XM6_DIAG_SKIP_NO_CTX;
        return;
    }
    if (out->breaker_tripped != 0U) {
        return;
    }
    if (__atomic_load_n(&b->req_in_flight, __ATOMIC_ACQUIRE) != 0U) {
        out->dsp_status_last = Z8_XM6_DIAG_SKIP_REQ_IN_FLIGHT;
        return;
    }
    if (!z8_xm6_core0_mailbox_idle(b)) {
        out->dsp_status_last = Z8_XM6_DIAG_SKIP_MAILBOX_BUSY;
        return;
    }

    struct z8_nfsp_allocation blk;
    memset(&blk, 0, sizeof(blk));
    if (!hw_alloc_class24_xm6(b, XM6_DIAG_TOTAL_BYTES, &blk) ||
        !blk.cpu || !blk.dma || blk.actual < XM6_DIAG_TOTAL_BYTES) {
        if (blk.handle || blk.cpu) {
            hw_release_class24(b, &blk);
        }
        xm6_set_last_err(b, Z8_XM6_ERR_ALLOC_FAIL);
        out->dsp_status_last = Z8_XM6_DIAG_SKIP_ALLOC_FAIL;
        return;
    }

    if (!xm6_try_acquire_core0(b, 100U)) {
        hw_release_class24(b, &blk);
        xm6_set_last_err(b, Z8_XM6_ERR_BUSY);
        out->dsp_status_last = Z8_XM6_DIAG_SKIP_CORE0_BUSY;
        return;
    }

    uint8_t *src_cpu = blk.cpu + XM6_DIAG_SRC_OFF;
    uint8_t *xm6_cpu = blk.cpu + XM6_DIAG_XM6_OFF;
    uint8_t *arm_cpu = blk.cpu + XM6_DIAG_ARM_OFF;
    uint8_t *ws_cpu  = blk.cpu + XM6_DIAG_WS_OFF;
    uint64_t src_dma = blk.dma + XM6_DIAG_SRC_OFF;
    uint64_t xm6_dma = blk.dma + XM6_DIAG_XM6_OFF;

    /* Dedicated 64B-aligned 512 KiB Slot 41 external grid workspace inside diag block
     * (NEVER pass ext_work_addr = 0!) */
    uint64_t ext_raw_dma     = blk.dma + XM6_DIAG_HDF_EXT_WS_OFF;
    uint64_t ext_aligned_dma = (ext_raw_dma + 63U) & ~UINT64_C(63);
    uint64_t ext_delta       = ext_aligned_dma - ext_raw_dma;
    uint8_t *ext_aligned_cpu = blk.cpu + XM6_DIAG_HDF_EXT_WS_OFF + (size_t)ext_delta;
    z8_xm6_dcache_civac(b, ext_aligned_cpu, XM6_DIAG_HDF_EXT_WS_BYTES);
    uint32_t diag_ext_ws_dsp = 0U;
    (void)z8_xm6_core0_arm_to_dsp32(b, ext_aligned_dma, XM6_DIAG_HDF_EXT_WS_BYTES, &diag_ext_ws_dsp);

    /* Resolve grain maps: use real ZF_GRAIN.BIN if loaded, else deterministic synthetic maps */
    const int16_t *map0_cpu = 0;
    const int16_t *map1_cpu = 0;
    uint64_t map0_va = 0U;
    uint64_t map1_va = 0U;
    if (zf_raw_loaded && zf_raw_buf) {
        const uint8_t *raw = (const uint8_t *)zf_raw_buf;
        map0_cpu = (const int16_t *)(const void *)(raw + Z8_XM6_ZFG1_HEADER_BYTES);
        map1_cpu = (const int16_t *)(const void *)(raw + Z8_XM6_ZFG1_HEADER_BYTES + Z8_XM6_ZFG1_MAP_BYTES);
        map0_va = (uint64_t)(uintptr_t)map0_cpu;
        map1_va = (uint64_t)(uintptr_t)map1_cpu;
    } else {
        int16_t *sm0 = (int16_t *)(void *)(blk.cpu + XM6_DIAG_SYNMAP_OFF);
        int16_t *sm1 = sm0 + (Z8_XM6_ZF_MAP_DIM * Z8_XM6_ZF_MAP_DIM);
        diag_fill_syn_grain_maps(sm0, sm1);
        z8_xm6_dcache_civac(b, blk.cpu + XM6_DIAG_SYNMAP_OFF, XM6_DIAG_SYNMAP_BYTES);
        map0_cpu = sm0;
        map1_cpu = sm1;
        map0_va = blk.dma + XM6_DIAG_SYNMAP_OFF;
        map1_va = blk.dma + XM6_DIAG_SYNMAP_OFF + Z8_XM6_ZFG1_MAP_BYTES;
    }

    uint32_t dsp_st = Z8_XM6_DSP_STATUS_SENTINEL;
    uint32_t dt_us = 0U;
    uint16_t err = Z8_XM6_ERR_OK;
    int any_hung_dma = 0;

    /* ---- Test 0: Slot 39 FilmGrain EXACT (128x128 NV16 Y, out != in) ---- */
    {
        uint32_t y_bytes = XM6_DIAG_STILL_W * XM6_DIAG_STILL_H;
        uint32_t seed = UINT32_C(0x5a380001);
        int hung_dma = 0;
        dsp_st = Z8_XM6_DSP_STATUS_SENTINEL;
        dt_us = 0U;
        diag_fill_pattern(src_cpu, 0, XM6_DIAG_STILL_W, XM6_DIAG_STILL_H, 0);
        memcpy(arm_cpu, src_cpu, y_bytes);
        memset(xm6_cpu, 0xA5, y_bytes);
        z8_xm6_dcache_civac(b, src_cpu, y_bytes);
        z8_xm6_dcache_civac(b, xm6_cpu, y_bytes);

        out->ran_mask |= (uint16_t)Z8_XM6_DIAG_GRAIN_EXACT;
        err = xm6_dispatch_grain_locked(b, src_dma, xm6_dma,
                                        XM6_DIAG_STILL_W, XM6_DIAG_STILL_H,
                                        XM6_DIAG_STILL_W, y_bytes,
                                        3U, 2U, Z8_ZF_BLUR_EXACT, 0U, seed,
                                        map0_va, map1_va, Z8_XM6_TIMEOUT_DIAG_MS,
                                        &dsp_st, &dt_us, &hung_dma);
        out->us_grain_exact = dt_us;
        out->dsp_status_last = dsp_st;
        any_hung_dma |= hung_dma;
        if (!hung_dma) {
            z8_xm6_dcache_civac(b, xm6_cpu, y_bytes);
        }

        int arc = z8_fs_stage_grain_run_raw_maps(3U, 2U, Z8_ZF_BLUR_EXACT, 1U,
                                                 map0_cpu, map1_cpu, seed,
                                                 arm_cpu, XM6_DIAG_STILL_W,
                                                 XM6_DIAG_STILL_W, XM6_DIAG_STILL_H,
                                                 ws_cpu, XM6_DIAG_WS_BYTES);
        uint32_t diff = (err == Z8_XM6_ERR_OK && arc == Z8_FS_INT_OK)
                            ? diag_count_diff(xm6_cpu, arm_cpu, y_bytes)
                            : UINT32_C(0xffffffff);
        out->diff_grain_exact = diff;
        if (err == Z8_XM6_ERR_OK && arc == Z8_FS_INT_OK && diff == 0U) {
            out->passed_mask |= (uint16_t)Z8_XM6_DIAG_GRAIN_EXACT;
        }
    }

    /* ---- Test 1: Slot 39 FilmGrain SHARP (128x128 NV16 Y, out != in) ---- */
    if (__atomic_load_n(&b->breaker_tripped, __ATOMIC_ACQUIRE) == 0U) {
        uint32_t y_bytes = XM6_DIAG_STILL_W * XM6_DIAG_STILL_H;
        uint32_t seed = UINT32_C(0x5a380002);
        int hung_dma = 0;
        dsp_st = Z8_XM6_DSP_STATUS_SENTINEL;
        dt_us = 0U;
        diag_fill_pattern(src_cpu, 0, XM6_DIAG_STILL_W, XM6_DIAG_STILL_H, 0);
        memcpy(arm_cpu, src_cpu, y_bytes);
        memset(xm6_cpu, 0xA5, y_bytes);
        z8_xm6_dcache_civac(b, src_cpu, y_bytes);
        z8_xm6_dcache_civac(b, xm6_cpu, y_bytes);

        out->ran_mask |= (uint16_t)Z8_XM6_DIAG_GRAIN_SHARP;
        err = xm6_dispatch_grain_locked(b, src_dma, xm6_dma,
                                        XM6_DIAG_STILL_W, XM6_DIAG_STILL_H,
                                        XM6_DIAG_STILL_W, y_bytes,
                                        4U, 3U, Z8_ZF_BLUR_SHARP, 0U, seed,
                                        map0_va, map1_va, Z8_XM6_TIMEOUT_DIAG_MS,
                                        &dsp_st, &dt_us, &hung_dma);
        out->us_grain_sharp = dt_us;
        out->dsp_status_last = dsp_st;
        any_hung_dma |= hung_dma;
        if (!hung_dma) {
            z8_xm6_dcache_civac(b, xm6_cpu, y_bytes);
        }

        int arc = z8_fs_stage_grain_run_raw_maps(4U, 3U, Z8_ZF_BLUR_SHARP, 1U,
                                                 map0_cpu, map1_cpu, seed,
                                                 arm_cpu, XM6_DIAG_STILL_W,
                                                 XM6_DIAG_STILL_W, XM6_DIAG_STILL_H,
                                                 ws_cpu, XM6_DIAG_WS_BYTES);
        uint32_t diff = (err == Z8_XM6_ERR_OK && arc == Z8_FS_INT_OK)
                            ? diag_count_diff(xm6_cpu, arm_cpu, y_bytes)
                            : UINT32_C(0xffffffff);
        out->diff_grain_sharp = diff;
        if (err == Z8_XM6_ERR_OK && arc == Z8_FS_INT_OK && diff == 0U) {
            out->passed_mask |= (uint16_t)Z8_XM6_DIAG_GRAIN_SHARP;
        }
    }

    /* ---- Test 2: Slot 41 HDF STILL_GENERAL (128x128 NV16 Y+CbCr, out != in) ---- */
    if (__atomic_load_n(&b->breaker_tripped, __ATOMIC_ACQUIRE) == 0U) {
        uint32_t y_bytes = XM6_DIAG_STILL_W * XM6_DIAG_STILL_H;
        uint32_t c_bytes = y_bytes;
        int hung_dma = 0;
        dsp_st = Z8_XM6_DSP_STATUS_SENTINEL;
        dt_us = 0U;
        uint8_t *src_c = src_cpu + y_bytes;
        uint8_t *xm6_c = xm6_cpu + y_bytes;
        uint8_t *arm_c = arm_cpu + y_bytes;

        diag_fill_pattern(src_cpu, src_c, XM6_DIAG_STILL_W, XM6_DIAG_STILL_H, 0);
        memcpy(arm_cpu, src_cpu, y_bytes + c_bytes);
        memset(xm6_cpu, 0xA5, y_bytes + c_bytes);
        z8_xm6_dcache_civac(b, src_cpu, y_bytes + c_bytes);
        z8_xm6_dcache_civac(b, xm6_cpu, y_bytes + c_bytes);

        out->ran_mask |= (uint16_t)Z8_XM6_DIAG_HDF_STILL;
        err = xm6_dispatch_hdf_locked(b, src_dma, src_dma + y_bytes,
                                      xm6_dma, xm6_dma + y_bytes,
                                      XM6_DIAG_STILL_W, XM6_DIAG_STILL_H,
                                      XM6_DIAG_STILL_W, XM6_DIAG_STILL_W,
                                      y_bytes, c_bytes,
                                      Z8_HDF_MEDIUM, Z8_XM6_HDF_MODE_STILL_GENERAL,
                                      Z8_XM6_FMT_NV16,
                                      diag_ext_ws_dsp, XM6_DIAG_HDF_EXT_WS_BYTES,
                                      Z8_XM6_TIMEOUT_DIAG_MS, 0,
                                      &dsp_st, &dt_us, &hung_dma);
        out->us_hdf_still = dt_us;
        out->dsp_status_last = dsp_st;
        any_hung_dma |= hung_dma;
        if (!hung_dma) {
            z8_xm6_dcache_civac(b, xm6_cpu, y_bytes + c_bytes);
        }

        z8_fs_stage_plane pl;
        memset(&pl, 0, sizeof(pl));
        pl.y = arm_cpu;
        pl.c = arm_c;
        pl.y_bytes = y_bytes;
        pl.c_bytes = c_bytes;
        pl.width = XM6_DIAG_STILL_W;
        pl.height = XM6_DIAG_STILL_H;
        pl.y_stride = XM6_DIAG_STILL_W;
        pl.c_stride = XM6_DIAG_STILL_W;
        pl.format = Z8_FS_INT_FMT_NV16;
        pl.is_full_res = 1U;
        int arc = z8_fs_stage_hdf_run(Z8_HDF_MEDIUM, UINT64_C(1), 1U, 0U, &pl,
                                      ws_cpu, XM6_DIAG_WS_BYTES, 0, 0);

        uint32_t diff = (err == Z8_XM6_ERR_OK && arc == Z8_FS_INT_OK)
                            ? (diag_count_diff(xm6_cpu, arm_cpu, y_bytes) +
                               diag_count_diff(xm6_c, arm_c, c_bytes))
                            : UINT32_C(0xffffffff);
        out->diff_hdf_still = diff;
        if (err == Z8_XM6_ERR_OK && arc == Z8_FS_INT_OK && diff == 0U) {
            out->passed_mask |= (uint16_t)Z8_XM6_DIAG_HDF_STILL;
        }
    }

    /* ---- Test 3: Slot 41 HDF LIVE_256_YC (1024x680 NV12 Y+CbCr, out != in) ---- */
    if (__atomic_load_n(&b->breaker_tripped, __ATOMIC_ACQUIRE) == 0U) {
        uint32_t y_bytes = XM6_DIAG_LIVE_Y_BYTES;
        uint32_t c_bytes = XM6_DIAG_LIVE_C_BYTES;
        int hung_dma = 0;
        dsp_st = Z8_XM6_DSP_STATUS_SENTINEL;
        dt_us = 0U;
        uint8_t *src_c = src_cpu + y_bytes;
        uint8_t *xm6_c = xm6_cpu + y_bytes;
        uint8_t *arm_c = arm_cpu + y_bytes;

        diag_fill_pattern(src_cpu, src_c, XM6_DIAG_LIVE_W, XM6_DIAG_LIVE_H, 1);
        memcpy(arm_cpu, src_cpu, y_bytes + c_bytes);
        memset(xm6_cpu, 0xA5, y_bytes + c_bytes);
        z8_xm6_dcache_civac(b, src_cpu, y_bytes + c_bytes);
        z8_xm6_dcache_civac(b, xm6_cpu, y_bytes + c_bytes);

        out->ran_mask |= (uint16_t)Z8_XM6_DIAG_HDF_LIVE;
        err = xm6_dispatch_hdf_locked(b, src_dma, src_dma + y_bytes,
                                      xm6_dma, xm6_dma + y_bytes,
                                      XM6_DIAG_LIVE_W, XM6_DIAG_LIVE_H,
                                      XM6_DIAG_LIVE_W, XM6_DIAG_LIVE_W,
                                      y_bytes, c_bytes,
                                      Z8_HDF_MEDIUM, Z8_XM6_HDF_MODE_LIVE_256_YC,
                                      Z8_XM6_FMT_NV12,
                                      diag_ext_ws_dsp, XM6_DIAG_HDF_EXT_WS_BYTES,
                                      Z8_XM6_TIMEOUT_DIAG_MS, 0,
                                      &dsp_st, &dt_us, &hung_dma);
        out->us_hdf_live = dt_us;
        out->dsp_status_last = dsp_st;
        any_hung_dma |= hung_dma;
        if (!hung_dma) {
            z8_xm6_dcache_civac(b, xm6_cpu, y_bytes + c_bytes);
        }

        z8_fs_stage_plane pl;
        memset(&pl, 0, sizeof(pl));
        pl.y = arm_cpu;
        pl.c = arm_c;
        pl.y_bytes = y_bytes;
        pl.c_bytes = c_bytes;
        pl.width = XM6_DIAG_LIVE_W;
        pl.height = XM6_DIAG_LIVE_H;
        pl.y_stride = XM6_DIAG_LIVE_W;
        pl.c_stride = XM6_DIAG_LIVE_W;
        pl.format = Z8_FS_INT_FMT_NV12;
        pl.is_full_res = 0U;
        int arc = z8_fs_stage_hdf_run(Z8_HDF_MEDIUM, UINT64_C(1), 1U, 0U, &pl,
                                      ws_cpu, XM6_DIAG_WS_BYTES, 0, 0);

        uint32_t diff = (err == Z8_XM6_ERR_OK && arc == Z8_FS_INT_OK)
                            ? (diag_count_diff(xm6_cpu, arm_cpu, y_bytes) +
                               diag_count_diff(xm6_c, arm_c, c_bytes))
                            : UINT32_C(0xffffffff);
        out->diff_hdf_live = diff;
        if (err == Z8_XM6_ERR_OK && arc == Z8_FS_INT_OK && diff == 0U) {
            out->passed_mask |= (uint16_t)Z8_XM6_DIAG_HDF_LIVE;
        }
    }

    xm6_release_core0(b);
#ifdef FILMSIM_HOST_TEST
    (void)any_hung_dma;
    hw_release_class24(b, &blk);
#else
    /* Close Diag Class 0x24 UAF: only release blk on bare metal when no DSP job is in flight
     * and Core 0 is either powered down or proven mailbox-idle. */
    uint32_t post_hw_state = 0U;
    uint8_t post_lf = 0U, post_fl = 0U;
    z8_xm6_read_hw_raw(b, &post_hw_state, &post_lf, &post_fl);
    if (!any_hung_dma &&
        __atomic_load_n(&b->req_in_flight, __ATOMIC_ACQUIRE) == 0U &&
        (post_hw_state != 2U || z8_xm6_core0_mailbox_idle(b))) {
        hw_release_class24(b, &blk);
    }
#endif

    out->breaker_tripped = __atomic_load_n(&b->breaker_tripped, __ATOMIC_ACQUIRE) ? 1U : 0U;
    __atomic_store_n(&b->diag_ran, 1U, __ATOMIC_RELEASE);
    uint32_t all_ok = (out->ran_mask == Z8_XM6_DIAG_ALL_MASK &&
                       out->passed_mask == Z8_XM6_DIAG_ALL_MASK) ? 1U : 0U;
    __atomic_store_n(&b->diag_passed, all_ok, __ATOMIC_RELEASE);
}

void z8_xm6_run_diag(struct z8_xm6_bridge *b,
                     const void *zf_raw_buf,
                     uint32_t zf_raw_loaded,
                     Z8Xm6DiagResult *out)
{
    z8_xm6_run_diag_ex(b, zf_raw_buf, zf_raw_loaded, 0U, out);
}

/* ---- Read-only vtable (.rodata, zero .data/.bss) & Initialization ---- */

static const Z8Xm6BridgeOps k_z8_xm6_ops = {
    .hw_ready        = z8_xm6_core0_hw_ready,
    .check_iommu     = xm6_check_iommu_cb,
    .still_hdf       = z8_xm6_still_hdf,
    .still_grain     = z8_xm6_still_grain,
    .record_still_ok = z8_xm6_record_still_ok,
    .record_fallback = z8_xm6_record_fallback,
    .fold_grain_seed = z8_xm6_fold_grain_seed,
    .still_rgblut    = z8_xm6_rgblut_nv16
};

void z8_xm6_bridge_init(struct z8_xm6_bridge *b, const Z8Xm6HwOps *hw)
{
    if (!b) {
        return;
    }
    memset(b, 0, sizeof(*b));
    b->ops = &k_z8_xm6_ops;
    b->hw  = hw;
}

void z8_xm6_bridge_set_hw_ops(struct z8_xm6_bridge *b, const Z8Xm6HwOps *hw)
{
    if (!b) {
        return;
    }
    b->hw = hw;
}
