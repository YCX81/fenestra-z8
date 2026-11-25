/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Display Layer 0 WYSIWYG Preview (LUT + Ricoh HDF + Nikon Zf Grain),
 * HangFix V69 pool_suspend/pool_resume, and POWER_OFF_SYNC/POWER_CANCEL_SYNC.
 *
 * Strictly freestanding C11, scalar-only (-mgeneral-regs-only), ZERO .data/.bss.
 * All mutable runtime state lives inside heap-allocated Layer0Ctx anchored at
 * v71r7_anchor()->layer0_ctx (DRAM 0xA11640000).
 */
#include "layer0_provider.h"

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#ifdef V71R7_HOST_TEST
#include <string.h>
#else
extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int c, size_t n);
#endif

#define V71R7_LAYER0_CTX_MAGIC UINT32_C(0x4C305637) /* "L0V7" */
#define RECT(x, y, r, b) \
    ((uint64_t)(x) | ((uint64_t)(y) << 16) | ((uint64_t)(r) << 32) | ((uint64_t)(b) << 48))

/* Weak fallback for single-pass Live->LCD shared graded frame consumer.
 * Overridden by strong definition in filmsim_runtime.c (unified EL1 link) and
 * mockable in standalone host tests. */
__attribute__((weak))
int filmsim_live_try_copy_graded_lcd(uint32_t w, uint32_t h,
                                     uint32_t lut_gen, uint8_t hdf_level,
                                     uint32_t max_age_us,
                                     uint32_t last_seq,
                                     uint8_t *dst_y, uint8_t *dst_uv,
                                     uint32_t dst_stride,
                                     uint32_t *out_stages_applied,
                                     uint32_t *out_seq,
                                     uint64_t *out_age_us)
{
    (void)w;
    (void)h;
    (void)lut_gen;
    (void)hdf_level;
    (void)max_age_us;
    (void)last_seq;
    (void)dst_y;
    (void)dst_uv;
    (void)dst_stride;
    if (out_stages_applied) {
        *out_stages_applied = 0U;
    }
    if (out_seq) {
        *out_seq = 0U;
    }
    if (out_age_us) {
        *out_age_us = 0U;
    }
    return 0;
}

/* Weak fallbacks for V71.7-r7 Reverse Layer0->Live feed, NEON plane copy & baked LUT
 * (overridden strongly by filmsim_runtime.c & lut_core in unified EL1 link; mockable on host). */
__attribute__((weak))
int filmsim_runtime_live_push_layer0_nv12(const uint8_t *y, const uint8_t *uv,
                                          uint32_t w, uint32_t h,
                                          uint32_t y_stride, uint32_t uv_stride,
                                          uint64_t capture_us,
                                          uint32_t lut_gen, uint8_t hdf_level)
{
    (void)y; (void)uv; (void)w; (void)h; (void)y_stride; (void)uv_stride;
    (void)capture_us; (void)lut_gen; (void)hdf_level;
    return 0;
}

__attribute__((weak)) int filmsim_runtime_live_layer0_feed_active(void) { return 0; }
__attribute__((weak)) int filmsim_runtime_live_independent_active(void) { return 0; }

__attribute__((weak))
void layer0_copy_plane_neon(uint8_t *dst, const uint8_t *src, size_t bytes, int do_dc_ivac)
{
    (void)do_dc_ivac;
    if (dst && src && bytes) {
        memcpy(dst, src, bytes);
    }
}

#ifdef V71R7_HOST_TEST
__attribute__((weak))
int v71r7_lut_apply_nv12_baked(uint8_t *y, uint8_t *uv, uint32_t width, uint32_t height,
                               uint32_t y_stride, uint32_t uv_stride,
                               uint32_t *baked_table_528k, uint32_t *baked_gen_inout,
                               int use_smp)
{
    return v71r7_lut_apply_nv12(y, uv, width, height, y_stride, uv_stride,
                                baked_table_528k, baked_gen_inout, use_smp);
}
#endif

/* Weak fallbacks for SMP helper telemetry when linked standalone without smp.c. */
__attribute__((weak)) uint32_t v71r7_smp_online_helpers(void) { return 0U; }
__attribute__((weak)) void v71r7_smp_get_helper_sentinels(uint32_t out[3])
{
    if (out) {
        out[0] = 0U;
        out[1] = 0U;
        out[2] = 0U;
    }
}

#ifndef LAYER0_DEFAULT_DISPLAY_MODE
#define LAYER0_DEFAULT_DISPLAY_MODE 1U
#endif
#ifndef LAYER0_DEFAULT_COPY_MODE
#define LAYER0_DEFAULT_COPY_MODE 2U
#endif
#ifndef LAYER0_DEFAULT_SOURCE_LAG
#define LAYER0_DEFAULT_SOURCE_LAG 1U
#endif

_Static_assert(LAYER0_DEFAULT_DISPLAY_MODE == 0U || LAYER0_DEFAULT_DISPLAY_MODE == 1U,
               "display mode must be 0 or 1");
_Static_assert(LAYER0_DEFAULT_COPY_MODE <= 2U, "copy mode must be 0, 1 or 2");
_Static_assert(LAYER0_DEFAULT_SOURCE_LAG <= 2U, "source lag must be 0, 1 or 2");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2,
               "lock-free hook counters and metadata");

enum {
    HISTORY = 8U,
    STEP_BURST_MS = 220U,
    STEP_MAX_FRAMES = 12U,
    STEP_IDLE_MS = 40U,
    COOP_SLICE_US = 22000U,
    LCD_NEON_STEP_MS = 250U,
    LCD_NEON_STEP_FRAMES = 16U,
    LCD_NEON_FRAME_INTERVAL_MS = 12U,
    EVF_NEON_STEP_MS = 485U,
    EVF_NEON_STEP_FRAMES = 30U,
    LCD_HW_BANK_LIMIT = 3U,
    POOL_LOCK_WAIT_MS = 2U
};

enum {
    BANKS = PHOTO_WORKSPACE_PREVIEW_BANKS,
    BANK_BYTES = PHOTO_WORKSPACE_PREVIEW_BANK_BYTES,
    CACHE_OFFSET = BANK_BYTES,
    REAR_CACHE = PHOTO_WORKSPACE_PREVIEW_CACHE_BYTES,
    GRADED_OFFSET = CACHE_OFFSET + REAR_CACHE,
    WORK_BYTES = PHOTO_WORKSPACE_PREVIEW_BYTES,
    REAR_W = 1024U,
    REAR_H = 682U,
    LCD_Y_BYTES = REAR_W * REAR_H,
    EVF_W = 1280U,
    EVF_H = 852U,
    EVF_CANVAS_H = 960U,
    SOURCE_UV_OFFSET = 0x12c000U,
    SOURCE_BLOCK_BYTES = 0x1c2000U,
    SOURCE_MAX_AGE_MS = 250U,
    COPY_CHUNK_CACHED_ROWS = 432U,
    COPY_CHUNK_UNCACHED_ROWS = 16U,
    BAKED_LUT_OFFSET = V71R7_LAYER0_HDF_SCRATCH_BYTES,
    BAKED_LUT_BYTES = 64U * 64U * 33U * 4U,
    CACHE_CLEAR_CHUNK = 256U * 1024U,
    POOL_MAX_RECORDS = 64U,
    POOL_NODE_BYTES = 0x18U,
    DISP_STATE = 0xc60U,
    LAYER0_ENABLED = 0x248U,
    LAYER0_SHADOW_FLAG = 0x44U,
    DISPLAY_UPDATE_ENABLED = 0x2efU,
    DISP_CROP_ORIGIN = 0x2fcU,
    DISP_CACHE_DESC = 0xc78U,
    DESC_Y = 0x110U,
    DESC_UV = 0x118U,
    MAX_AGE_MS = 2000U,
    STOP_DRAIN_MS = 400U,
    POOL_SUSPEND_DRAIN_MS = 100U,
    POWER_DRAIN_MS = 100U,
    POWER_DRAIN_TICKS = 100U,
    POWER_FRAME_DRAIN_MS = 500U,
    POWER_OFF_SYNC = 10U,
    POWER_CANCEL_SYNC = 11U
};

_Static_assert(GRADED_OFFSET + BANKS * BANK_BYTES == WORK_BYTES && WORK_BYTES == 0xe8c000U &&
               WORK_BYTES <= PHOTO_WORKSPACE_MAX_BYTES, "workspace layout");
_Static_assert(LCD_Y_BYTES == 698368U && EVF_W * EVF_CANVAS_H == SOURCE_UV_OFFSET &&
               BANK_BYTES == SOURCE_BLOCK_BYTES && !(LCD_Y_BYTES % 128U) && !(EVF_W * EVF_H % 128U),
               "plane dimensions and cache line alignment");
_Static_assert(V71R7_LAYER0_HDF_SCRATCH_BYTES <= BANK_BYTES,
               "stage bank holds 512 KiB HDF scratch");
_Static_assert(BAKED_LUT_OFFSET + BAKED_LUT_BYTES <= BANK_BYTES,
               "stage bank holds 512 KiB HDF scratch + 528 KiB baked YCbCr LUT");

enum { LCD = 0U, EVF = 1U, NO_ROUTE = 2U, ROUTE_MASK = 3U, ROUTE_EPOCH = 4U };

typedef struct {
    uint32_t route, device, width, height, canvas, active_y_bytes, private_uv_offset;
    uint64_t bounds;
} Geometry;

/* Strictly read-only geometry table (.rodata, 0 mutable globals). */
static const Geometry geometry[NO_ROUTE] = {
    [LCD] = {0U, 0U, REAR_W, REAR_H, REAR_W | (REAR_H << 16), LCD_Y_BYTES, LCD_Y_BYTES,
             RECT(0, 0, REAR_W, REAR_H)},
    [EVF] = {3U, 2U, EVF_W, EVF_H, EVF_W | (EVF_CANVAS_H << 16), EVF_W * EVF_H, SOURCE_UV_OFFSET,
             RECT(0, 54, EVF_W, 54 + EVF_H)}
};

/* Verified-clean stock ROM BL hook descriptors (.rodata). */
const V71r7HookSpec v71r7_layer0_hook_specs[6] = {
    {0x40E9A540U, 0x40CD7AD8U, 0x97F8F566U, 0U}, /* Enqueue */
    {0x40CD862CU, 0x402B6D88U, 0x97D779D7U, 1U}, /* Display swap */
    {0x40CD72A4U, 0x40CD9640U, 0x940008E7U, 2U}, /* LCD VSync */
    {0x40CD72D4U, 0x40CD9640U, 0x940008DBU, 3U}, /* EVF VSync */
    {0x40D67328U, 0x40D60E94U, 0x97FFE6DBU, 4U}, /* POWER_CANCEL_SYNC (w0=11) */
    {0x40D67340U, 0x40D60E94U, 0x97FFE6D5U, 5U}  /* POWER_OFF_SYNC (w0=10) */
};

enum BankState {
    BANK_FREE = 0U,
    BANK_WORKING = 1U,
    BANK_READY = 2U,
    BANK_IN_HW = 3U,
    BANK_RETIRING = 4U,
    BANK_QUARANTINED = 5U
};

enum { REAR_HUNG_DMA = 0x101U };

typedef struct {
    uint8_t *cpu;
    uint64_t captured_ms, captured_tick, stamp, generation;
    uint32_t copy_mode, ticket;
} Stage;

typedef struct {
    _Atomic uint64_t seq;
    _Atomic uintptr_t y, uv;
    _Atomic uint64_t stamp, captured_ms, captured_tick;
    _Atomic uint32_t ticket;
} FrameMeta;

typedef struct {
    _Atomic uint32_t state;
    _Atomic uint32_t ticket;
    _Atomic uint64_t generation;
    uintptr_t raw_y, cpu_y;
    uint64_t captured_ms, captured_tick, replaced_at_vsync, replaced_at_ms, replaced_other_vsync;
} Bank;

typedef struct {
    FrameMeta history[HISTORY];
    _Atomic uint64_t history_head;
    uint64_t history_next, last_tried_gen;
    /* Owned by display_claim. VSync seq advances in the VSync hook. */
    uint64_t last_off_vsync, last_present_tick, presented_generation, jpeg_kept_ms;
    uint32_t off_streak, baseline_seen, last_present_ticket;
} RouteState;

typedef struct {
    uintptr_t record, owner;
    uint32_t slot;
} PoolOwner;

typedef struct {
    uintptr_t y, uv;
    const FrameMeta *slot;
} SourcePlanes;

/* Heap-allocated Layer 0 provider context (anchored at v71r7_anchor()->layer0_ctx). */
typedef struct {
    uint32_t magic;
    uint32_t size;

    Layer0Metrics metrics;
    _Atomic uint32_t pool_lock_waits;
    _Atomic uint32_t pool_lock_wait_ok;
    RearPowerMetrics power;
    Layer0Evidence evidence;

    _Atomic uint32_t busy;
    _Atomic uint32_t display_claim;
    _Atomic uint32_t power_closing;
    _Atomic uint32_t pool_suspended;
    _Atomic uint32_t stop_lifecycle;
    _Atomic uint32_t ticket;
    _Atomic uint32_t layout_ticket;
    _Atomic uint32_t quarantine_mask;
    _Atomic uint32_t io_epoch;

    /* Activation state (LUT + FilmSim WYSIWYG HDF/Grain). */
    _Atomic uint32_t lut_active;
    _Atomic uint32_t lut_generation;
    _Atomic uint32_t cache_invalidate_req;
    _Atomic uint32_t cache_ready_gen;
    _Atomic uint32_t baked_ready_gen;
    _Atomic uint32_t cache_readers;
    _Atomic uint32_t push_wysiwyg_hdf;
    _Atomic uint32_t push_wysiwyg_grain;
    _Atomic uint32_t push_wysiwyg_ver;
    _Atomic uint32_t last_stages_applied;

    uint32_t cached_banks;       /* Owned by display_claim */
    uint32_t cache_ready;
    uint32_t cache_lut_gen;
    uint32_t baked_lut_gen;
    uint64_t last_evf_paced_vsync;
    uint32_t last_logged_stages;
    uint32_t last_logged_route;
    uint32_t last_reused_live_seq;
    uint64_t last_lcd_capture_ms;
    uint64_t last_neon_frame_ms;
    uint64_t evf_slice_start;
    uint64_t stop_started_ms;
    uint32_t closing;
    uint32_t stop_started;
    uint32_t load_error;

    /* Registered FilmSim WYSIWYG provider callbacks (protected by ops_seqlock). */
    _Atomic uint32_t ops_seqlock;
    uint32_t ops_registered;
    V71r7WysiwygOps wysiwyg_ops;

    PhotoWorkspace pool;
    PhotoWorkspaceView view;
    uint8_t *stage_cpu;

    RouteState route[NO_ROUTE];
    Bank bank[BANKS];
} Layer0Ctx;

/* ============================================================================
 * Platform Abstraction Layer (Bare-Metal Z8 C3.11 vs Mac Host ASan+UBSan Mock)
 * ============================================================================ */
#ifndef V71R7_HOST_TEST

#define FN(a, t) ((t)(uintptr_t)(a))

/* Stock ROM adapters provided in lut_hooks.S (direct far-jump safe). */
extern uint32_t v71r7_stock_layer0_enqueue(uint32_t route, const void *pkt, const void *rect,
                                           const void *stamp, void *cb);
extern uint32_t v71r7_stock_layer0_display(uint32_t device, void *layer_desc);
extern void     v71r7_stock_layer0_vsync(uint32_t route);
extern void     v71r7_stock_power_sync(uint32_t command, uint64_t payload);

#define STOCK_COMPLETE UINT64_C(0x40e9a570)
#define TO_CPU         FN(0x41186718U, uintptr_t (*)(uintptr_t))
#define TO_RAW         FN(0x41186780U, uintptr_t (*)(uintptr_t))
#define CACHEABLE      FN(0x41186810U, uint32_t (*)(uintptr_t))
#define CLEAN_EX       FN(0x40d2b9e4U, void (*)(uintptr_t, uint32_t, uint32_t))
#define CV_CLEAN       FN(0x40d2b8d4U, void (*)(const void *, uint32_t, uint32_t))
#define IMAGE_COPY     FN(0x4127a900U, void *(*)(void *, const void *, size_t))
#define SLEEP_MS       FN(0x40187b00U, int32_t (*)(uint32_t))
#define HEAP_ALLOC     FN(0x40d2c408U, void *(*)(uint32_t, uint32_t))
#define HEAP_FREE      FN(0x40d2c448U, void (*)(void *))
#define POOL_UNLOCK_FN FN(0x40d2c660U, void (*)(uintptr_t))
#define VIEW_ID        FN(0x40393f6cU, uint32_t (*)(uintptr_t))
#define VIEW_STATE     FN(0x40393f28U, uint32_t (*)(uintptr_t))
#define READ_PROPERTY  FN(0x4101553cU, uint32_t (*)(uint32_t, uint32_t *))

#define POOL            UINT64_C(0xa03b6b538)
#define POOL_MUTEX      (POOL + 4U)
#define POOL_MAGIC      UINT64_C(0x410439ac)
#define RAM_START       UINT64_C(0xa00000000)
#define RAM_END         UINT64_C(0xa40000000)
#define DISP            UINT64_C(0xa02e70c98)
#define DISP_PROGRAMMED UINT64_C(0xa02e78c88)
#define PANEL_BANK      UINT64_C(0xa02810470)
#define CURRENT_VIEW    UINT64_C(0xa032280c8)
#define SHOOTING_VIEW   UINT64_C(0xa0282dab8)
#define GUI_LV          UINT64_C(0xa03229bc8)
#define DELTA           UINT64_C(0x400000000)
#define CONT_MODE_VA    UINT64_C(0xa03d7c7a4) /* DS 0x00002126: 0=S,1=CL,2=CH,3=C30,4=C120,5=C60,6=C15 */

static uint8_t  l0_u8(uintptr_t p)  { return *(const volatile uint8_t *)p; }
static uint16_t l0_u16(uintptr_t p) { return *(const volatile uint16_t *)p; }
static uint32_t l0_u32(uintptr_t p) { return *(const volatile uint32_t *)p; }
static uint64_t l0_u64(uintptr_t p) { return *(const volatile uint64_t *)p; }
static void     l0_s64(uintptr_t p, uint64_t v) { *(volatile uint64_t *)p = v; }
static void     l0_barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static uint64_t l0_ticks(void)
{
    uint64_t v = 0U;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}

static uint64_t l0_frequency(void)
{
    uint64_t v = 0U;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static uint16_t l0_core_id(void)
{
    uint64_t mpidr = 0U;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint16_t)((mpidr & 15U) + 1U);
}

static uint32_t l0_panel_bank(void)    { return l0_u32(PANEL_BANK); }
static uint64_t l0_current_view(void)  { return l0_u64(CURRENT_VIEW); }
static uint64_t l0_shooting_view(void) { return SHOOTING_VIEW; }
static uint32_t l0_gui_lv_canvas(void) { return l0_u32(GUI_LV + 4U); }
static uint64_t l0_gui_lv_bounds(void) { return l0_u64(GUI_LV + 8U); }

static uint32_t l0_disp_status(void)          { return l0_u32(DISP); }
static uint32_t l0_disp_state(void)           { return l0_u32(DISP + DISP_STATE); }
static uint32_t l0_disp_layer0_enabled(void)  { return l0_u32(DISP + LAYER0_ENABLED); }
static uint8_t  l0_disp_shadow_flag(void)     { return l0_u8(DISP + LAYER0_SHADOW_FLAG); }
static uint8_t  l0_disp_update_enabled(void)  { return l0_u8(DISP + DISPLAY_UPDATE_ENABLED); }
static uintptr_t l0_disp_crop_origin_ptr(void){ return (uintptr_t)(DISP + DISP_CROP_ORIGIN); }
static uint64_t l0_disp_cache_desc_y(void)    { return l0_u64(DISP + DISP_CACHE_DESC + DESC_Y); }
static uint64_t l0_disp_cache_desc_uv(void)   { return l0_u64(DISP + DISP_CACHE_DESC + DESC_UV); }
static uint64_t l0_disp_programmed(void)      { return l0_u64(DISP_PROGRAMMED); }
static uintptr_t l0_stock_complete_cb(void)   { return (uintptr_t)STOCK_COMPLETE; }

static int l0_pool_lock_timed(uint32_t tmout_ms)
{
    /* C3.11 0x40D2C698 wraps uITRON tloc_mtx(mtxid, tmout_ms) (verified 241 stock callers:
     * 0 = nonblocking poll; >0 = RTOS blocking timed wait in ms with priority inheritance,
     * 0% CPU busy-spin, instant wakeup on unl_mtx 0x40D2C660!). */
    return FN(0x40d2c698U, int (*)(uintptr_t, uint32_t))((uintptr_t)POOL_MUTEX, tmout_ms);
}

static void l0_pool_unlock(void)
{
    POOL_UNLOCK_FN((uintptr_t)POOL_MUTEX);
}

static uint32_t l0_gui_release_mode(void)
{
    return l0_u32(CONT_MODE_VA);
}

static int l0_in_pool_ram(uintptr_t p, uint64_t bytes)
{
    return !(p & 7U) && p >= RAM_START && bytes <= RAM_END - RAM_START && p <= RAM_END - bytes;
}

/* Pool 0's owner records can move between queues under the stock mutex. */
static __attribute__((noinline)) int l0_pool_owns_source(uintptr_t y, PoolOwner *identity)
{
    if (l0_u32(POOL) != 0U || l0_u64(POOL + 0x10U) != POOL_MAGIC) {
        return 0;
    }
    uint32_t a = l0_u32(POOL + 0x30U);
    uint32_t b = l0_u32(POOL + 0x70U);
    if (a > POOL_MAX_RECORDS || b > POOL_MAX_RECORDS - a || a + b == 0U) {
        return 0;
    }

    PoolOwner found = {0U, 0U, 0U};
    for (unsigned offset = 0x20U; offset <= 0x60U; offset += 0x40U) {
        uintptr_t q = (uintptr_t)(POOL + offset);
        uintptr_t head = (uintptr_t)l0_u64(q);
        uintptr_t tail = (uintptr_t)l0_u64(q + 8U);
        uint32_t count = (offset == 0x20U) ? a : b;
        if (!count) {
            if (head || tail) {
                return 0;
            }
            continue;
        }
        uint32_t cap = l0_u32(q + 0x14U);
        uintptr_t backing = (uintptr_t)l0_u64(q + 0x18U);
        uintptr_t node = head;
        uintptr_t prev = tail;
        uint64_t extent = (uint64_t)cap * POOL_NODE_BYTES;
        if (count > cap || !l0_in_pool_ram(backing, extent)) {
            return 0;
        }
        for (unsigned i = 0U; i < count; ++i) {
            if ((i && node == head) || node < backing || node - backing >= extent ||
                (node - backing) % POOL_NODE_BYTES || (uintptr_t)l0_u64(node) != prev) {
                return 0;
            }
            uintptr_t rec = (uintptr_t)l0_u64(node + 0x10U);
            if (!l0_in_pool_ram(rec, 0x28U)) {
                return 0;
            }
            uintptr_t owner = (uintptr_t)l0_u64(rec + 8U);
            if (!l0_in_pool_ram(owner, 0x10U)) {
                return 0;
            }
            if ((uintptr_t)l0_u64(owner) == y) {
                if (found.owner || (uintptr_t)l0_u64(rec + 0x20U) != y ||
                    l0_u32(owner + 8U) < SOURCE_BLOCK_BYTES) {
                    return 0;
                }
                found.record = rec;
                found.owner = owner;
                found.slot = l0_u32(rec + 0x18U);
            }
            prev = node;
            node = (uintptr_t)l0_u64(node + 8U);
        }
        if (prev != tail || node != head) {
            return 0;
        }
    }
    if (!found.owner || (identity->owner &&
                         (identity->owner != found.owner ||
                          identity->record != found.record ||
                          identity->slot != found.slot))) {
        return 0;
    }
    *identity = found;
    return 1;
}

static int l0_mapped_plane(uintptr_t raw, uintptr_t cpu, uint32_t bytes)
{
    uintptr_t end = (uintptr_t)bytes - 1U;
    return !(raw & 63U) && cpu == raw + DELTA &&
           TO_CPU(raw) == cpu && TO_CPU(raw + end) == cpu + end &&
           TO_RAW(cpu) == raw && TO_RAW(cpu + end) == raw + end &&
           !CACHEABLE(raw) && !CACHEABLE(raw + end) &&
           CACHEABLE(cpu) && CACHEABLE(cpu + end);
}

static int l0_source_planes(const Geometry *g, uintptr_t y, uintptr_t uv)
{
    return !(y & 511U) && y >= RAM_START &&
           y <= RAM_END - SOURCE_BLOCK_BYTES && uv == y + SOURCE_UV_OFFSET &&
           l0_mapped_plane(y, y + DELTA, g->active_y_bytes) &&
           l0_mapped_plane(uv, uv + DELTA, g->active_y_bytes / 2U);
}

static uintptr_t l0_source_cached_view(uintptr_t raw)
{
    return raw + DELTA;
}

static int l0_bank_in_single_512m_window(uintptr_t raw, uint32_t span)
{
    if (!span) {
        return 0;
    }
    uint64_t a = (uint64_t)raw;
    uint64_t b = a + (uint64_t)(span - 1U);
    return (a >> 29) == (b >> 29);
}

static void l0_cv_clean_chunked(const void *addr, uint32_t bytes)
{
    const uint8_t *cur = (const uint8_t *)addr;
    uint32_t rem = bytes;
    while (rem > 0U) {
        uint32_t chunk = rem > PHOTO_WORKSPACE_DCACHE_CHUNK ? PHOTO_WORKSPACE_DCACHE_CHUNK : rem;
        CV_CLEAN(cur, chunk, 0U);
        cur += chunk;
        rem -= chunk;
    }
    l0_barrier();
}

static uint32_t l0_stock_enqueue(uint32_t route, const void *pkt, const void *rect,
                                 const void *stamp, void *cb)
{
    return v71r7_stock_layer0_enqueue(route, pkt, rect, stamp, cb);
}

static uint32_t l0_stock_display(uint32_t device, void *desc)
{
    return v71r7_stock_layer0_display(device, desc);
}

static void l0_stock_vsync(uint32_t route)
{
    v71r7_stock_layer0_vsync(route);
}

static void l0_stock_power_sync(uint32_t command, uint64_t payload)
{
    v71r7_stock_power_sync(command, payload);
}

#else /* V71R7_HOST_TEST */

#define HEAP_ALLOC(n, f)   v71r7_host_heap_alloc((n), (f))
#define HEAP_FREE(p)       v71r7_host_heap_free((p))
#define IMAGE_COPY(d,s,n)  v71r7_host_image_copy((d), (s), (n))
#define SLEEP_MS(ms)       v71r7_host_sleep((ms))
#define TO_RAW(c)          v71r7_host_ws_to_raw((c))
#define VIEW_ID(v)         v71r7_host_view_id((v))
#define VIEW_STATE(v)      v71r7_host_view_state((v))
#define READ_PROPERTY(p,o) v71r7_host_read_property((p), (o))

static uint8_t  l0_u8(uintptr_t p)  { return *(const volatile uint8_t *)p; }
static uint16_t l0_u16(uintptr_t p)
{
    uint16_t v = 0U;
    memcpy(&v, (const void *)p, sizeof(v));
    return v;
}
static uint32_t l0_u32(uintptr_t p)
{
    uint32_t v = 0U;
    memcpy(&v, (const void *)p, sizeof(v));
    return v;
}
static uint64_t l0_u64(uintptr_t p)
{
    uint64_t v = 0U;
    memcpy(&v, (const void *)p, sizeof(v));
    return v;
}
static void l0_s64(uintptr_t p, uint64_t v)
{
    memcpy((void *)p, &v, sizeof(v));
}
static void     l0_barrier(void)      { atomic_thread_fence(memory_order_seq_cst); }
static uint64_t l0_ticks(void)        { return v71r7_host_ticks(); }
static uint64_t l0_frequency(void)    { return v71r7_host_freq(); }
static uint16_t l0_core_id(void)      { return 1U; }

static uint32_t  l0_panel_bank(void)           { return v71r7_host_panel_bank(); }
static uint64_t  l0_current_view(void)         { return v71r7_host_current_view(); }
static uint64_t  l0_shooting_view(void)        { return v71r7_host_shooting_view(); }
static uint32_t  l0_gui_lv_canvas(void)        { return v71r7_host_gui_lv_canvas(); }
static uint64_t  l0_gui_lv_bounds(void)        { return v71r7_host_gui_lv_bounds(); }
static uint32_t  l0_disp_status(void)          { return v71r7_host_disp_status(); }
static uint32_t  l0_disp_state(void)           { return v71r7_host_disp_state(); }
static uint32_t  l0_disp_layer0_enabled(void)  { return v71r7_host_disp_layer0_enabled(); }
static uint8_t   l0_disp_shadow_flag(void)     { return v71r7_host_disp_shadow_flag(); }
static uint8_t   l0_disp_update_enabled(void)  { return v71r7_host_disp_update_enabled(); }
static uintptr_t l0_disp_crop_origin_ptr(void) { return v71r7_host_disp_crop_origin_ptr(); }
static uint64_t  l0_disp_cache_desc_y(void)    { return v71r7_host_disp_cache_desc_y(); }
static uint64_t  l0_disp_cache_desc_uv(void)   { return v71r7_host_disp_cache_desc_uv(); }
static uint64_t  l0_disp_programmed(void)      { return v71r7_host_disp_programmed(); }
static uintptr_t l0_stock_complete_cb(void)    { return v71r7_host_stock_complete_cb(); }
static int       l0_pool_lock_timed(uint32_t tmout_ms) { return v71r7_host_pool_lock_timed(tmout_ms); }
static void      l0_pool_unlock(void)                  { v71r7_host_pool_unlock(); }
static uint32_t  l0_gui_release_mode(void)             { return v71r7_host_gui_release_mode(); }

static int l0_pool_owns_source(uintptr_t y, PoolOwner *identity)
{
    return v71r7_host_pool_owns_source(y, &identity->record, &identity->owner, &identity->slot);
}

static int l0_mapped_plane(uintptr_t raw, uintptr_t cpu, uint32_t bytes)
{
    uintptr_t end = (uintptr_t)bytes - 1U;
    return !(raw & 63U) && v71r7_host_ws_to_cpu(raw) == cpu &&
           v71r7_host_ws_to_raw(cpu) == raw &&
           v71r7_host_ws_to_raw(cpu + end) == raw + end &&
           !v71r7_host_ws_cacheable(raw) && v71r7_host_ws_cacheable(cpu);
}

static int l0_source_planes(const Geometry *g, uintptr_t y, uintptr_t uv)
{
    return v71r7_host_source_planes_valid(y, uv, g->active_y_bytes, SOURCE_UV_OFFSET);
}

static uintptr_t l0_source_cached_view(uintptr_t raw)
{
    return v71r7_host_ws_to_cpu(raw);
}

static int l0_bank_in_single_512m_window(uintptr_t raw, uint32_t span)
{
    if (!span) {
        return 0;
    }
    uint64_t a = v71r7_host_ws_dma_window_addr(raw);
    uint64_t b = a + (uint64_t)(span - 1U);
    return (a >> 29) == (b >> 29);
}

static void l0_cv_clean_chunked(const void *addr, uint32_t bytes)
{
    photo_workspace_dcache_clean_chunked((uintptr_t)addr, bytes);
}

static uint32_t l0_stock_enqueue(uint32_t route, const void *pkt, const void *rect,
                                 const void *stamp, void *cb)
{
    return v71r7_host_stock_enqueue(route, pkt, rect, stamp, cb);
}

static uint32_t l0_stock_display(uint32_t device, void *desc)
{
    return v71r7_host_stock_display(device, desc);
}

static void l0_stock_vsync(uint32_t route)
{
    v71r7_host_stock_vsync(route);
}

static void l0_stock_power_sync(uint32_t command, uint64_t payload)
{
    v71r7_host_stock_power_sync(command, payload);
}

#endif /* V71R7_HOST_TEST */

/* ============================================================================
 * Anchor Lookup & Time Helpers (Zero Globals)
 * ============================================================================ */

static Layer0Ctx *layer0_ctx_get(void)
{
    V71r7DramAnchor *anc = v71r7_anchor();
    if (__atomic_load_n(&anc->magic, __ATOMIC_ACQUIRE) != V71R7_ANCHOR_MAGIC) {
        return NULL;
    }
    Layer0Ctx *s = (Layer0Ctx *)__atomic_load_n(&anc->layer0_ctx, __ATOMIC_ACQUIRE);
    if (!s || s->magic != V71R7_LAYER0_CTX_MAGIC || s->size != sizeof(*s)) {
        return NULL;
    }
    return s;
}

static void anchor_flag_set(uint32_t bit)
{
    V71r7DramAnchor *anc = v71r7_anchor();
    if (__atomic_load_n(&anc->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC) {
        __atomic_fetch_or(&anc->flags, bit, __ATOMIC_ACQ_REL);
    }
}

static void anchor_flag_clear(uint32_t bit)
{
    V71r7DramAnchor *anc = v71r7_anchor();
    if (__atomic_load_n(&anc->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC) {
        __atomic_fetch_and(&anc->flags, ~bit, __ATOMIC_ACQ_REL);
    }
}

static int anchor_pool_suspended(const Layer0Ctx *s)
{
    if (s && atomic_load_explicit(&s->pool_suspended, memory_order_acquire)) {
        return 1;
    }
    const V71r7DramAnchor *anc = v71r7_anchor();
    if (__atomic_load_n(&anc->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC &&
        (__atomic_load_n(&anc->flags, __ATOMIC_ACQUIRE) & V71R7_ANCHOR_FLAG_POOL_SUSPENDED)) {
        return 1;
    }
    return 0;
}

static uint64_t tick_ms(uint64_t t)
{
    uint64_t hz = l0_frequency();
    return (hz && hz <= UINT32_MAX) ? (t / hz) * 1000U + ((t % hz) * 1000U) / hz : UINT64_MAX;
}

static uint64_t now_ms(void)
{
    return tick_ms(l0_ticks());
}

static uint32_t elapsed_us(uint64_t start, uint64_t end)
{
    uint64_t hz = l0_frequency();
    if (!hz || end < start) {
        return 0U;
    }
    uint64_t elapsed = end - start;
    if (elapsed > UINT64_MAX / 1000000U) {
        return UINT32_MAX;
    }
    uint64_t us = (elapsed * 1000000U) / hz;
    return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static uint32_t duration_us(uint64_t start)
{
    return elapsed_us(start, l0_ticks());
}

static void timed(_Atomic uint32_t *last, _Atomic uint32_t *max_v, uint32_t value)
{
    atomic_store_explicit(last, value, memory_order_relaxed);
    uint32_t prev = atomic_load_explicit(max_v, memory_order_relaxed);
    while (prev < value &&
           !atomic_compare_exchange_weak_explicit(max_v, &prev, value,
                                                  memory_order_relaxed, memory_order_relaxed)) {
    }
}

#define COUNT(s, field) ((void)atomic_fetch_add_explicit(&(s)->metrics.field, 1U, memory_order_relaxed))

static int claim(_Atomic uint32_t *state, uint32_t prior, uint32_t next)
{
    return atomic_compare_exchange_strong(state, &prior, next);
}

static int preview_delay(Layer0Ctx *s, uint64_t *slice_start)
{
    uint64_t start = l0_ticks();
    int32_t rc = SLEEP_MS(1U);
    *slice_start = l0_ticks();
    timed(&s->metrics.last_sleep_us, &s->metrics.max_sleep_us, elapsed_us(start, *slice_start));
    return rc == 0;
}

static int preview_checkpoint(Layer0Ctx *s, uint64_t *slice_start)
{
    return duration_us(*slice_start) < COOP_SLICE_US || preview_delay(s, slice_start);
}

/* Snapshot registered WYSIWYG provider ops safely via seqlock. */
static int snapshot_wysiwyg_ops(const Layer0Ctx *s, V71r7WysiwygOps *out)
{
    if (!s || !out) {
        return 0;
    }
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&s->ops_seqlock, memory_order_acquire);
        if (s1 & 1U) {
            continue;
        }
        uint32_t reg = s->ops_registered;
        V71r7WysiwygOps tmp = s->wysiwyg_ops;
        uint32_t s2 = atomic_load_explicit(&s->ops_seqlock, memory_order_acquire);
        if (s1 == s2) {
            *out = tmp;
            return reg != 0U;
        }
    }
}

static int query_wysiwyg_state(const Layer0Ctx *s, const V71r7WysiwygOps *ops, int has_ops,
                               V71r7WysiwygState *out)
{
    memset(out, 0, sizeof(*out));
    if (has_ops && ops->query_state) {
        int active = ops->query_state(ops->ctx, out);
        if (!active || (!out->hdf_level && !out->zf_intensity)) {
            out->active = 0U;
            return 0;
        }
        out->active = 1U;
        return 1;
    }
    uint32_t hdf = atomic_load_explicit(&s->push_wysiwyg_hdf, memory_order_acquire);
    uint32_t grain = atomic_load_explicit(&s->push_wysiwyg_grain, memory_order_acquire);
    if (hdf || grain) {
        out->active = 1U;
        out->hdf_level = (uint8_t)hdf;
        out->zf_intensity = (uint8_t)grain;
        out->cfg_version = atomic_load_explicit(&s->push_wysiwyg_ver, memory_order_acquire);
        return 1;
    }
    return 0;
}

static int is_still_capturing(const Layer0Ctx *s)
{
    V71r7WysiwygOps ops;
    if (snapshot_wysiwyg_ops(s, &ops) && ops.is_still_capturing) {
        if (ops.is_still_capturing(ops.ctx)) {
            return 1;
        }
    }
    return 0;
}

static int query_lut_active(const Layer0Ctx *s, uint32_t *gen_out)
{
    if (!v71r7_lut_is_enabled_for(V71R7_LUT_STAGE_PREVIEW)) {
        if (gen_out) {
            *gen_out = 0U;
        }
        return 0;
    }
    uint32_t core_gen = 0U;
    if (v71r7_lut_is_active(&core_gen, NULL)) {
        if (gen_out) {
            *gen_out = core_gen ? core_gen : atomic_load_explicit(&s->lut_generation, memory_order_acquire);
        }
        return 1;
    }
    if (atomic_load_explicit(&s->lut_active, memory_order_acquire)) {
        if (gen_out) {
            *gen_out = atomic_load_explicit(&s->lut_generation, memory_order_acquire);
        }
        return 1;
    }
    if (gen_out) {
        *gen_out = 0U;
    }
    return 0;
}

static int layer0_is_dormant_fast(const Layer0Ctx *s)
{
    const V71r7DramAnchor *anc = v71r7_anchor();
    uint32_t aflags = (__atomic_load_n(&anc->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC)
                          ? __atomic_load_n(&anc->flags, __ATOMIC_ACQUIRE)
                          : 0U;
    return !atomic_load_explicit(&s->lut_active, memory_order_acquire) &&
           !(aflags & V71R7_ANCHOR_FLAG_WYSIWYG_ACTIVE) &&
           !atomic_load_explicit(&s->metrics.active_banks_mask, memory_order_acquire) &&
           !photo_workspace_is_allocated(&s->pool);
}

static int layer0_has_quarantined_bank(const Layer0Ctx *s)
{
    for (unsigned i = 0U; i < BANKS; ++i) {
        if (atomic_load_explicit(&s->bank[i].state, memory_order_acquire) == BANK_QUARANTINED) {
            return 1;
        }
    }
    return 0;
}

/* ============================================================================
 * Route Visibility, Admission & Hardware Bank State Machine
 * ============================================================================ */

static int visible(unsigned index)
{
    const Geometry *g = &geometry[index];
    uint32_t bank = l0_panel_bank();
    return (bank == index || (index == EVF && bank == 2U)) &&
           l0_current_view() == l0_shooting_view() &&
           l0_gui_lv_canvas() == g->canvas &&
           l0_gui_lv_bounds() == g->bounds &&
           l0_panel_bank() == bank;
}

static uint32_t observe_route(Layer0Ctx *s)
{
    uint32_t prior = atomic_load(&s->ticket);
    for (unsigned retry = 0U; retry < 2U; ++retry) {
        unsigned index = visible(LCD) ? LCD : visible(EVF) ? EVF : NO_ROUTE;
        if ((prior & ROUTE_MASK) == index) {
            return prior;
        }
        uint32_t next = ((prior & ~(uint32_t)ROUTE_MASK) + ROUTE_EPOCH) | index;
        if (atomic_compare_exchange_strong(&s->ticket, &prior, next)) {
            v71r7_flight_record(FX_Z8_FLIGHT_PREVIEW_ROUTE_SWITCH, l0_core_id(),
                                prior & ROUTE_MASK, index, next);
            return next;
        }
    }
    return prior;
}

/* Verified C3.11 DS 0x00002126 (0xA03D7C7A4) high-speed burst gate:
 * 0=S, 1=CL, 2=CH, 3=C30, 4=C120, 5=C60, 6=C15.
 * Bypass Layer 0 during C15..C120 High-Speed Frame Capture+ while keeping S/CL/CH active. */
static int l0_release_allowed(void)
{
    uint32_t rel = l0_gui_release_mode();
    return !(rel >= 3U && rel <= 6U);
}

static int worker_layout(Layer0Ctx *s, uint32_t ticket)
{
    unsigned index = ticket & ROUTE_MASK;
    uint32_t selector = UINT32_MAX;
    uintptr_t sv = (uintptr_t)l0_shooting_view();
    int ok = index < NO_ROUTE && l0_release_allowed() && visible(index) &&
             VIEW_ID(sv) == 3U && VIEW_STATE(sv) == 1U &&
             READ_PROPERTY(0x06000003U, &selector) == 1U && selector == 0U &&
             observe_route(s) == ticket;
    atomic_store(&s->layout_ticket, ok ? ticket : NO_ROUTE);
    atomic_store(&s->metrics.mode_ok, ok ? 1U : 0U);
    atomic_store(&s->metrics.active_route, ok ? geometry[index].route : UINT32_MAX);
    return ok && !is_still_capturing(s);
}

static int output_open(const Layer0Ctx *s)
{
    if (!atomic_load(&s->metrics.enabled)) {
        return 0;
    }
    if (atomic_load(&s->stop_lifecycle) || anchor_pool_suspended(s)) {
        return 0;
    }
    uint32_t epoch = fx_z8_lut_io_epoch(v71r7_io_gate());
    return epoch != 0U;
}

static int armed(const Layer0Ctx *s)
{
    return atomic_load(&s->metrics.online) &&
           output_open(s) &&
           atomic_load(&s->metrics.display_ready) &&
           !atomic_load(&s->power_closing) &&
           !anchor_pool_suspended(s) &&
           l0_release_allowed();
}

static int fresh(uint64_t now, uint64_t captured)
{
    return now != UINT64_MAX && now >= captured && (now - captured) < MAX_AGE_MS;
}

static int packet_matches(const Geometry *g, uintptr_t p, uintptr_t rect)
{
    return p && rect && !((p | rect) & 7U) &&
           l0_u8(p) == 1U && l0_u64(p + 8U) == 2U &&
           l0_u16(p + 0x10U) == g->width && l0_u16(p + 0x12U) == g->width &&
           l0_u16(p + 0x18U) == g->width && l0_u16(p + 0x1aU) == g->height &&
           l0_u32(p + 0x1cU) == 0U && l0_u64(rect) == g->bounds;
}

static int spot_matches_row_range(uintptr_t raw, const uint8_t *stage, unsigned first,
                                  unsigned count, unsigned rows, unsigned width,
                                  uint32_t seed, unsigned *bad_row)
{
    if (count == 0U) {
        return 1;
    }
    unsigned last = first + count - 1U;
    for (unsigned i = first; i <= last; ++i) {
        if (i != first && i != last && (((i - first) & 31U) != 0U)) {
            raw += width;
            stage += width;
            continue;
        }
        unsigned left = 0U;
        unsigned right = width - 8U;
        if (i && i != rows - 1U) {
            uint32_t phase = (i * UINT32_C(2654435761)) ^ seed;
            left = ((phase >> 12) & 63U) * 8U;
            right = width / 2U + ((phase >> 22) & 63U) * 8U;
        }
        uint64_t sl = 0U, sr = 0U;
        memcpy(&sl, stage + left, sizeof(sl));
        memcpy(&sr, stage + right, sizeof(sr));
        if (l0_u64(raw + left) != sl || l0_u64(raw + right) != sr) {
            *bad_row = i;
            return 0;
        }
        raw += width;
        stage += width;
    }
    return 1;
}

static uint64_t read_stamp(uintptr_t p)
{
    return (uint64_t)l0_u32(p) | ((uint64_t)l0_u32(p + 4U) << 32);
}

static int layer_matches(const Geometry *g, uintptr_t d)
{
    return l0_u32(d + 8U) == 4U && l0_u8(d + 0x14U) == 1U &&
           l0_u16(d + 0x16U) == g->width && l0_u16(d + 0x18U) == g->height &&
           l0_u16(d + 0x1aU) == g->width && l0_u16(d + 0x1cU) == g->width;
}

static int display_running(unsigned index)
{
    return l0_disp_state() == 3U && l0_panel_bank() == index;
}

static int update_enabled(unsigned index)
{
    uint32_t status = l0_disp_status();
    return display_running(index) && l0_disp_update_enabled() == 1U &&
           (status == 3U || status == 5U || status == 6U);
}

static int any_update_enabled(void)
{
    return update_enabled(LCD) || update_enabled(EVF);
}

static int safe_crop(const Geometry *g, uintptr_t desc)
{
    uintptr_t crop = l0_u8(desc + 0x1eU) ? (desc + 0x20U) : l0_disp_crop_origin_ptr();
    return l0_u32(crop) == 0U &&
           l0_u16(crop + 4U) <= g->width && l0_u16(crop + 6U) <= g->height &&
           (g->route == 0U ||
            (l0_u8(desc + 0x28U) && l0_u16(desc + 0x2aU) == 0U && l0_u16(desc + 0x2cU) == 54U &&
             l0_u16(desc + 0x2eU) == g->width && l0_u16(desc + 0x30U) == 54U + g->height));
}

static int newest_ready(Layer0Ctx *s, uint32_t ticket, uint64_t newer_than)
{
    int best = -1;
    for (unsigned i = 0U; i < BANKS; ++i) {
        if (atomic_load(&s->bank[i].state) != BANK_READY ||
            atomic_load(&s->bank[i].ticket) != ticket) {
            continue;
        }
        uint64_t generation = atomic_load(&s->bank[i].generation);
        if (generation > newer_than) {
            newer_than = generation;
            best = (int)i;
        }
    }
    return best;
}

static int bank_address(const Bank *bank, uintptr_t y, uintptr_t uv)
{
    return (y - bank->raw_y < BANK_BYTES) || (y - bank->cpu_y < BANK_BYTES) ||
           (uv - bank->raw_y < BANK_BYTES) || (uv - bank->cpu_y < BANK_BYTES);
}

static int references_leased_bank(const Layer0Ctx *s, uintptr_t y, uintptr_t uv)
{
    uint32_t leased = atomic_load(&s->metrics.active_banks_mask);
    for (unsigned i = 0U; i < BANKS; ++i) {
        if ((leased & (1U << i)) && bank_address(&s->bank[i], y, uv)) {
            return 1;
        }
    }
    return 0;
}

static void refresh_cached_banks(Layer0Ctx *s)
{
    uintptr_t y = (uintptr_t)l0_disp_cache_desc_y();
    uintptr_t uv = (uintptr_t)l0_disp_cache_desc_uv();
    uint32_t leased = atomic_load(&s->metrics.active_banks_mask);
    uint32_t cached = 0U;
    for (unsigned i = 0U; i < BANKS; ++i) {
        if (!(leased & (1U << i)) || !bank_address(&s->bank[i], y, uv)) {
            continue;
        }
        cached |= (1U << i);
        if (atomic_load(&s->bank[i].state) != BANK_QUARANTINED) {
            atomic_store(&s->bank[i].state, BANK_IN_HW);
        }
    }
    s->cached_banks = cached;
}

static uint32_t hardware_pending(const Layer0Ctx *s)
{
    return atomic_load(&s->metrics.active_banks_mask);
}

static _Atomic uint64_t *vsync_seq(Layer0Ctx *s, unsigned index)
{
    return (index == LCD) ? &s->metrics.vsync_seq : &s->metrics.evf_vsync_seq;
}

static _Atomic int32_t *hw_current(Layer0Ctx *s, unsigned index)
{
    return (index == LCD) ? &s->metrics.lcd_hw_current : &s->metrics.evf_hw_current;
}

/* Retire non-quarantined, uncached banks when display hardware is already stopped/off.
 * Caller MUST hold display_claim == 1. */
static void retire_stopped_display_banks_locked(Layer0Ctx *s)
{
    if (any_update_enabled() || (l0_disp_state() == 3U && l0_disp_layer0_enabled() != 0U)) {
        return;
    }
    refresh_cached_banks(s);
    uint32_t leased = atomic_load(&s->metrics.active_banks_mask);
    for (unsigned i = 0U; i < BANKS; ++i) {
        if (!(leased & (1U << i)) || (s->cached_banks & (1U << i))) {
            continue;
        }
        Bank *bank = &s->bank[i];
        if (atomic_load(&bank->state) == BANK_QUARANTINED) {
            continue;
        }
        unsigned bidx = atomic_load(&bank->ticket) & ROUTE_MASK;
        atomic_fetch_and(&s->metrics.active_banks_mask, ~(1U << i));
        atomic_store(&bank->state, BANK_FREE);
        if (bidx < NO_ROUTE && atomic_load(hw_current(s, bidx)) == (int32_t)i) {
            atomic_store(hw_current(s, bidx), -1);
        }
        if (atomic_load(&s->metrics.hw_current) == (int32_t)i) {
            atomic_store(&s->metrics.hw_current, -1);
        }
        COUNT(s, banks_retired);
    }
}

/* Called with display_claim held. Keep hardware-owned banks pinned until a stock
 * replacement establishes a fresh VSync retirement fence. */
static void retain_hardware(Layer0Ctx *s)
{
    uint32_t leased = atomic_load(&s->metrics.active_banks_mask);
    atomic_store(&s->metrics.display_ready, 0U);
    atomic_store(&s->metrics.stock_baseline_confirmed, 0U);
    for (unsigned i = 0U; i < NO_ROUTE; ++i) {
        s->route[i].off_streak = 0U;
        s->route[i].baseline_seen = 0U;
    }
    uint32_t mask = 0U;
    for (unsigned i = 0U; i < BANKS; ++i) {
        if (!(leased & (1U << i))) {
            continue;
        }
        mask |= (1U << (atomic_load(&s->bank[i].ticket) & ROUTE_MASK));
        if (atomic_load(&s->bank[i].state) != BANK_QUARANTINED) {
            atomic_store(&s->bank[i].state, BANK_IN_HW);
        }
    }
    atomic_store(&s->quarantine_mask, mask);
    atomic_store(&s->metrics.quarantined, mask != 0U ? 1U : 0U);
}

static void replace_banks(Layer0Ctx *s, unsigned index, int keep, uint64_t seq,
                          uint64_t other_ms, uint64_t other_seq)
{
    for (unsigned i = 0U; i < BANKS; ++i) {
        Bank *bank = &s->bank[i];
        if ((int)i == keep || (s->cached_banks & (1U << i)) ||
            (atomic_load(&bank->ticket) & ROUTE_MASK) != index) {
            continue;
        }
        uint32_t st = atomic_load(&bank->state);
        if (st == BANK_IN_HW) {
            bank->replaced_at_vsync = seq;
            bank->replaced_at_ms = other_ms;
            bank->replaced_other_vsync = other_seq;
            atomic_store(&bank->state, BANK_RETIRING);
        } else if (st == BANK_RETIRING && other_ms && !bank->replaced_at_ms) {
            bank->replaced_at_ms = other_ms;
            bank->replaced_other_vsync = other_seq;
        }
    }
    atomic_store(hw_current(s, index), keep);
    s->route[index].off_streak = 0U;
}

static void pin_hardware(Layer0Ctx *s, unsigned index)
{
    atomic_fetch_or(&s->metrics.active_banks_mask, 1U << index);
    atomic_store(&s->bank[index].state, BANK_IN_HW);
}

static void record_present(Layer0Ctx *s, unsigned index, const Bank *bank, uint64_t now)
{
    if (now == UINT64_MAX) {
        return;
    }
    RouteState *r = &s->route[index];
    uint64_t present_tick = l0_ticks();
    uint32_t ticket = atomic_load(&bank->ticket);
    if (r->last_present_tick && r->last_present_ticket == ticket) {
        uint32_t gap = duration_us(r->last_present_tick);
        timed(&s->metrics.last_interframe_ms, &s->metrics.max_interframe_ms, gap / 1000U);
        atomic_store(&s->metrics.fps_milli, gap ? (1000000000U / gap) : 0U);
    } else {
        atomic_store(&s->metrics.fps_milli, 0U);
    }
    r->last_present_ticket = ticket;
    r->last_present_tick = present_tick;
    atomic_store(&s->metrics.last_presented_ms, now);
    timed(&s->metrics.last_latency_us, &s->metrics.max_latency_us, duration_us(bank->captured_tick));
    atomic_store(&s->metrics.last_sub_raw_y, bank->raw_y);
    r->presented_generation = atomic_load(&bank->generation);
    atomic_store(&s->metrics.last_presented_generation, r->presented_generation);
}

static int route_banks(const Layer0Ctx *s, unsigned index)
{
    unsigned count = 0U;
    uint32_t leased = atomic_load(&s->metrics.active_banks_mask);
    for (unsigned i = 0U; i < BANKS; ++i) {
        if ((leased & (1U << i)) && (atomic_load(&s->bank[i].ticket) & ROUTE_MASK) == index) {
            ++count;
        }
    }
    return (int)count;
}

/* Caller owns display_claim and has refreshed cached_banks via DISP_CACHE_DESC. */
static void retire_banks(Layer0Ctx *s, unsigned active_index, uint64_t active_seq, uint64_t now)
{
    uint32_t retained = atomic_load(&s->quarantine_mask);
    int fence_allowed = !(retained & (1U << active_index)) ||
                        (atomic_load(&s->metrics.online) && s->route[active_index].baseline_seen);
    for (unsigned i = 0U; i < BANKS; ++i) {
        Bank *bank = &s->bank[i];
        if ((s->cached_banks & (1U << i)) || atomic_load(&bank->state) != BANK_RETIRING) {
            continue;
        }
        unsigned index = atomic_load(&bank->ticket) & ROUTE_MASK;
        if (index == active_index) {
            if (!fence_allowed || active_seq < bank->replaced_at_vsync ||
                (active_seq - bank->replaced_at_vsync) < 2U) {
                continue;
            }
        } else {
            int replaced = !(retained & (1U << index)) ||
                           (atomic_load(&s->metrics.online) && s->route[index].baseline_seen);
            if (!replaced || !bank->replaced_at_ms || now == UINT64_MAX ||
                now < bank->replaced_at_ms || (now - bank->replaced_at_ms) < 100U ||
                active_seq < bank->replaced_other_vsync ||
                (active_seq - bank->replaced_other_vsync) < 4U) {
                continue;
            }
            COUNT(s, cross_route_retires);
        }
        atomic_fetch_and(&s->metrics.active_banks_mask, ~(1U << i));
        atomic_store(&bank->state, BANK_FREE);
        COUNT(s, banks_retired);
    }
    for (unsigned index = 0U; retained && index < NO_ROUTE; ++index) {
        if ((retained & (1U << index)) && !route_banks(s, index)) {
            retained &= ~(1U << index);
        }
    }
    if (retained != atomic_load(&s->quarantine_mask)) {
        atomic_store(&s->quarantine_mask, retained);
        atomic_store(&s->metrics.quarantined, retained != 0U ? 1U : 0U);
    }
}

static void replace_sink(Layer0Ctx *s, unsigned index, int keep, uint32_t ticket)
{
    uint64_t seq = atomic_load(vsync_seq(s, index));
    uint64_t now = ((ticket & ROUTE_MASK) == index && visible(index)) ? now_ms() : 0U;
    replace_banks(s, index, keep, seq, 0U, 0U);
    replace_banks(s, index ^ 1U, -1, atomic_load(vsync_seq(s, index ^ 1U)),
                  now == UINT64_MAX ? 0U : now, seq);
}

static int can_substitute(Layer0Ctx *s, unsigned index, uint32_t ticket, uintptr_t d)
{
    return armed(s) &&
           !atomic_load(&s->metrics.quarantined) &&
           atomic_load(&s->metrics.display_mode) == 1U &&
           atomic_load(&s->layout_ticket) == ticket &&
           (ticket & ROUTE_MASK) == index &&
           visible(index) &&
           update_enabled(index) &&
           safe_crop(&geometry[index], d) &&
           !is_still_capturing(s);
}

static int keep_current(RouteState *r, const Bank *bank, uint64_t now)
{
    if (fx_z8_lut_frame_count(v71r7_io_gate())) {
        r->jpeg_kept_ms = now;
        return now != UINT64_MAX;
    }
    return fresh(now, bank->captured_ms) ||
           (r->jpeg_kept_ms && now >= r->jpeg_kept_ms &&
            (now - r->jpeg_kept_ms) <= SOURCE_MAX_AGE_MS);
}

/* ============================================================================
 * 6 Verified-Clean Stock ROM Hook Implementations
 * ============================================================================ */

static void record_meta(Layer0Ctx *s, uint32_t route, uintptr_t p, uintptr_t rect,
                        uintptr_t stamp, uintptr_t cb)
{
    if ((route != 0U && route != 3U) || cb != l0_stock_complete_cb() || !armed(s)) {
        return;
    }
    COUNT(s, enqueue_seen);
    uint32_t ticket = observe_route(s);
    unsigned index = ticket & ROUTE_MASK;
    if (index >= NO_ROUTE || geometry[index].route != route) {
        return;
    }
    const Geometry *g = &geometry[index];
    if (!packet_matches(g, p, rect) || !stamp || (stamp & 3U)) {
        COUNT(s, enqueue_rejected_fmt);
        return;
    }
    uintptr_t y = (uintptr_t)l0_u64(p + 0x20U);
    uintptr_t uv = (uintptr_t)l0_u64(p + 0x28U);
    if (!l0_source_planes(g, y, uv)) {
        COUNT(s, enqueue_rejected_fmt);
        return;
    }
    uint64_t source_stamp = read_stamp(stamp);
    uint64_t tick = l0_ticks();
    uint64_t captured_ms = tick_ms(tick);

    RouteState *r = &s->route[index];
    uint64_t gen = ++r->history_next;
    FrameMeta *slot = &r->history[gen & (HISTORY - 1U)];
    atomic_store_explicit(&slot->seq, 0U, memory_order_release);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&slot->y, y, memory_order_relaxed);
    atomic_store_explicit(&slot->uv, uv, memory_order_relaxed);
    atomic_store_explicit(&slot->stamp, source_stamp, memory_order_relaxed);
    atomic_store_explicit(&slot->captured_ms, captured_ms, memory_order_relaxed);
    atomic_store_explicit(&slot->captured_tick, tick, memory_order_relaxed);
    atomic_store_explicit(&slot->ticket, ticket, memory_order_relaxed);
    atomic_store_explicit(&slot->seq, gen, memory_order_release);
    atomic_store_explicit(&r->history_head, gen, memory_order_release);
}

/* Hook #1: 0x40E9A540 (BL 0x40CD7AD8 -> v71r7_layer0_enqueue_hook). */
uint32_t v71r7_layer0_enqueue_hook_c(uint32_t route, const void *pkt, const void *rect,
                                     const void *stamp, void *cb)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (!s || layer0_is_dormant_fast(s)) {
        return l0_stock_enqueue(route, pkt, rect, stamp, cb);
    }
    atomic_fetch_add(&s->metrics.hook_active, 1U);
    record_meta(s, route, (uintptr_t)pkt, (uintptr_t)rect, (uintptr_t)stamp, (uintptr_t)cb);
    uint32_t rc = l0_stock_enqueue(route, pkt, rect, stamp, cb);
    atomic_fetch_sub(&s->metrics.hook_active, 1U);
    return rc;
}

/* Hook #2: 0x40CD862C (BL 0x402B6D88 -> v71r7_layer0_display_hook). */
uint32_t v71r7_layer0_display_hook_c(uint32_t device, void *layer_desc)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (!s || layer0_is_dormant_fast(s)) {
        return l0_stock_display(device, layer_desc);
    }
    atomic_fetch_add(&s->metrics.hook_active, 1U);
    COUNT(s, disp_calls);
    uintptr_t d = (uintptr_t)layer_desc;
    if ((device != 0U && device != 2U) || !d || (d & 7U) || l0_u32(d) != 0U) {
        COUNT(s, disp_stock_pass);
        COUNT(s, disp_rejects);
        uint32_t rc = l0_stock_display(device, layer_desc);
        atomic_fetch_sub(&s->metrics.hook_active, 1U);
        return rc;
    }

    unsigned index = (device == 0U) ? LCD : EVF;
    const Geometry *g = &geometry[index];
    RouteState *r = &s->route[index];
    uint64_t orig_y = l0_u64(d + DESC_Y);
    uint64_t orig_uv = l0_u64(d + DESC_UV);
    atomic_store(&s->metrics.last_stock_y, orig_y);

    int tracking = claim(&s->display_claim, 0U, 1U);
    int format = layer_matches(g, d);
    int running = tracking && format && update_enabled(index);
    uint32_t ticket = observe_route(s);
    int previous = -1;
    int selected = -1;
    uint64_t now = UINT64_MAX;

    if (running && hardware_pending(s)) {
        refresh_cached_banks(s);
        now = now_ms();
        retire_banks(s, index, atomic_load(vsync_seq(s, index)),
                     (ticket & ROUTE_MASK) == index ? now : UINT64_MAX);
    } else if (tracking && !running && hardware_pending(s)) {
        retire_stopped_display_banks_locked(s);
    }
    if (running && can_substitute(s, index, ticket, d)) {
        if (now == UINT64_MAX) {
            now = now_ms();
        }
        previous = atomic_load(hw_current(s, index));
        int limit = (index == LCD) ? (int)LCD_HW_BANK_LIMIT : (int)(BANKS - 1U);
        int ready = (route_banks(s, index) < limit)
                        ? newest_ready(s, ticket, r->presented_generation)
                        : -1;
        if (ready >= 0 && claim(&s->bank[ready].state, BANK_READY, BANK_WORKING)) {
            Bank *bank = &s->bank[ready];
            if (atomic_load(&bank->ticket) == ticket &&
                atomic_load(&bank->generation) > r->presented_generation &&
                fresh(now, bank->captured_ms)) {
                selected = ready;
            } else {
                atomic_store(&bank->state, BANK_FREE);
            }
        }
        if (selected < 0 && previous >= 0 && route_banks(s, index ^ 1U) < (int)(BANKS - 1U) &&
            atomic_load(&s->bank[previous].ticket) == ticket &&
            keep_current(r, &s->bank[previous], now)) {
            selected = previous;
        }
    }

    /* Final admission recheck right before transient pointer swap. */
    if (selected >= 0 &&
        (observe_route(s) != ticket || !can_substitute(s, index, ticket, d) ||
         !(selected == previous ? keep_current(r, &s->bank[selected], now_ms())
                                : fresh(now_ms(), s->bank[selected].captured_ms)))) {
        if (selected != previous) {
            atomic_store(&s->bank[selected].state, BANK_FREE);
        }
        selected = -1;
    }

    if (selected >= 0) {
        Bank *bank = &s->bank[selected];
        atomic_store(&s->metrics.stock_baseline_confirmed, 0U);
        r->baseline_seen = 0U;
        pin_hardware(s, (unsigned)selected);
        l0_s64(d + DESC_Y, (uint64_t)bank->raw_y);
        l0_s64(d + DESC_UV, (uint64_t)(bank->raw_y + g->private_uv_offset));
    }

    uint32_t rc = l0_stock_display(device, layer_desc);

    /* Immediately restore caller's original Layer 0 pointers on return! */
    if (selected >= 0) {
        l0_s64(d + DESC_Y, orig_y);
        l0_s64(d + DESC_UV, orig_uv);
    }

    if (tracking) {
        int confirmed = running && (rc & 1U) && update_enabled(index) &&
                        l0_disp_programmed() == (uint64_t)(d + 0x100U);
        if (hardware_pending(s)) {
            refresh_cached_banks(s);
        }
        if (selected >= 0) {
            Bank *bank = &s->bank[selected];
            if (confirmed) {
                replace_sink(s, index, selected, ticket);
                atomic_store(&s->metrics.hw_current, selected);
                if (selected != previous) {
                    r->jpeg_kept_ms = 0U;
                    record_present(s, index, bank, now_ms());
                    COUNT(s, disp_substituted);
                    if (index == LCD) {
                        COUNT(s, lcd_substituted);
                    } else {
                        COUNT(s, evf_substituted);
                    }
                } else {
                    COUNT(s, disp_kept_current);
                }
            } else {
                /* P0 Fix #1: NEVER leak BANK_IN_HW on rejected display frames!
                 * Immediately unpin rejected fresh bank (or stopped-display bank) unless
                 * stock DISP_CACHE_DESC still references it. */
                if (!(s->cached_banks & (1U << selected)) &&
                    atomic_load(&bank->state) != BANK_QUARANTINED) {
                    if (selected != previous) {
                        atomic_fetch_and(&s->metrics.active_banks_mask, ~(1U << selected));
                        atomic_store(&bank->state, BANK_FREE);
                        atomic_store(hw_current(s, index), previous);
                        if (atomic_load(&s->metrics.hw_current) == selected) {
                            atomic_store(&s->metrics.hw_current, previous);
                        }
                    } else if (!update_enabled(index) &&
                               (l0_disp_state() != 3U || l0_disp_layer0_enabled() == 0U)) {
                        atomic_fetch_and(&s->metrics.active_banks_mask, ~(1U << selected));
                        atomic_store(&bank->state, BANK_FREE);
                        atomic_store(hw_current(s, index), -1);
                        if (atomic_load(&s->metrics.hw_current) == selected) {
                            atomic_store(&s->metrics.hw_current, -1);
                        }
                    }
                }
                r->off_streak = 0U;
                COUNT(s, disp_rejects);
            }
        } else {
            COUNT(s, disp_stock_pass);
            if (confirmed && orig_y && orig_uv &&
                !references_leased_bank(s, (uintptr_t)orig_y, (uintptr_t)orig_uv) &&
                (!atomic_load(&s->metrics.quarantined) || atomic_load(&s->metrics.online))) {
                replace_sink(s, index, -1, ticket);
                r->baseline_seen = 1U;
                s->route[index ^ 1U].baseline_seen = 1U;
                atomic_store(&s->metrics.hw_current, -1);
                atomic_store(&s->metrics.stock_baseline_confirmed, 1U);
            }
            if (!running && visible(index)) {
                COUNT(s, disp_rejects);
            }
        }
        atomic_store(&s->display_claim, 0U);
    } else {
        COUNT(s, disp_stock_pass);
        COUNT(s, disp_rejects);
    }
    atomic_fetch_sub(&s->metrics.hook_active, 1U);
    return rc;
}

/* Hooks #3 & #4: 0x40CD72A4 (LCD) & 0x40CD72D4 (EVF) (BL 0x40CD9640 -> v71r7_layer0_vsync_hook). */
void v71r7_layer0_vsync_hook_c(uint32_t route)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (!s || layer0_is_dormant_fast(s)) {
        l0_stock_vsync(route);
        return;
    }
    atomic_fetch_add(&s->metrics.hook_active, 1U);
    if (route == 0U || route == 3U) {
        unsigned index = (route == 0U) ? LCD : EVF;
        RouteState *r = &s->route[index];
        uint64_t seq = atomic_fetch_add(vsync_seq(s, index), 1U) + 1U;
        COUNT(s, vsync_ticks);
        if (claim(&s->display_claim, 0U, 1U)) {
            atomic_store(&s->metrics.disp_shadow_flag, l0_disp_shadow_flag());
            if (atomic_load(&s->metrics.active_banks_mask)) {
                refresh_cached_banks(s);
                int retained = (atomic_load(&s->quarantine_mask) & (1U << index)) != 0U;
                if (!retained && (l0_disp_state() != 3U || l0_disp_layer0_enabled() == 0U)) {
                    r->off_streak = (r->last_off_vsync + 1U == seq) ? (r->off_streak + 1U) : 1U;
                    r->last_off_vsync = seq;
                    if (r->off_streak >= 2U) {
                        int32_t current = atomic_load(hw_current(s, index));
                        replace_banks(s, index, -1, seq, 0U, 0U);
                        if (current >= 0 && atomic_load(&s->metrics.hw_current) == current) {
                            atomic_store(&s->metrics.hw_current, -1);
                        }
                    }
                } else {
                    r->off_streak = 0U;
                    /* LCD <-> EVF rapid switch safety: once active panel definitely owns `index`,
                     * ensure any remaining non-quarantined banks on the opposite route enter
                     * cross-route retirement even if no confirmed display_hook arrived yet. */
                    if (l0_panel_bank() == index && route_banks(s, index ^ 1U) > 0) {
                        uint64_t now_cross = now_ms();
                        replace_banks(s, index ^ 1U, -1, atomic_load(vsync_seq(s, index ^ 1U)),
                                      now_cross == UINT64_MAX ? 0U : now_cross, seq);
                    }
                }
                retire_banks(s, index, seq, visible(index) ? now_ms() : UINT64_MAX);
            }
            atomic_store(&s->display_claim, 0U);
        } else {
            COUNT(s, vsync_claim_misses);
        }
    }
    l0_stock_vsync(route);
    atomic_fetch_sub(&s->metrics.hook_active, 1U);
}

/* ============================================================================
 * Workspace Prepare / Retire & Unified WYSIWYG Render Pipeline
 * ============================================================================ */

static int display_drained(Layer0Ctx *s)
{
    if (!claim(&s->display_claim, 0U, 1U)) {
        return 0;
    }
    int drained = !hardware_pending(s) && !atomic_load(&s->metrics.quarantined);
    atomic_store(&s->display_claim, 0U);
    return drained;
}

static int io_pending(void)
{
    return (__atomic_load_n(&v71r7_io_gate()->state, __ATOMIC_ACQUIRE) & FX_Z8_LUT_IO_REFS) != 0U;
}

/* Caller holds s->busy == 1. Releases photo_workspace when no hardware banks remain leased.
 * When force_on_suspend != 0 (called ONLY after POOL_SUSPEND_DRAIN_MS grace wait expires
 * during stock Class 0x24 teardown), safely force-retires non-BANK_QUARANTINED banks so
 * photo_workspace is NEVER leaked across stock 0x410463e4/0x41046ef8 teardown. */
static uint32_t retire_workspace_locked(Layer0Ctx *s, int allow_retained, int force_on_suspend)
{
    s->closing = 1U;
    atomic_store(&s->metrics.enabled, 0U);
    atomic_store(&s->metrics.display_ready, 0U);
    atomic_store(&s->metrics.mode_ok, 0U);
    atomic_store(&s->layout_ticket, NO_ROUTE);
    s->evf_slice_start = 0U;

    uint64_t now = now_ms();
    if (!s->stop_started) {
        s->stop_started_ms = now;
        s->stop_started = 1U;
    }
    if (!claim(&s->display_claim, 0U, 1U)) {
        return V71R7_L0_BUSY;
    }
    if (atomic_load(&s->metrics.hook_active)) {
        atomic_store(&s->display_claim, 0U);
        return V71R7_L0_BUSY;
    }
    if (hardware_pending(s) != 0U) {
        retire_stopped_display_banks_locked(s);
    }
    int held = hardware_pending(s) != 0U;
    if (held && force_on_suspend && !layer0_has_quarantined_bank(s) &&
        !atomic_load(&s->metrics.quarantined)) {
        /* Grace wait expired during stock Class 0x24 teardown/switch and no hung DSP DMA
         * bank exists: force-clear non-quarantined bank pins so Class 0x24 frees cleanly. */
        uint32_t leased = atomic_load(&s->metrics.active_banks_mask);
        for (unsigned i = 0U; i < BANKS; ++i) {
            if ((leased & (1U << i)) && atomic_load(&s->bank[i].state) != BANK_QUARANTINED) {
                atomic_fetch_and(&s->metrics.active_banks_mask, ~(1U << i));
                atomic_store(&s->bank[i].state, BANK_FREE);
                COUNT(s, banks_retired);
            }
        }
        atomic_store(&s->metrics.hw_current, -1);
        atomic_store(&s->metrics.lcd_hw_current, -1);
        atomic_store(&s->metrics.evf_hw_current, -1);
        s->cached_banks = 0U;
        COUNT(s, force_retires);
        held = hardware_pending(s) != 0U;
    }
    if (held) {
        int expired = (now == UINT64_MAX || s->stop_started_ms == UINT64_MAX ||
                       now < s->stop_started_ms || (now - s->stop_started_ms) >= STOP_DRAIN_MS);
        if (!allow_retained || (!atomic_load(&s->metrics.quarantined) &&
                                !atomic_load(&s->power_closing) &&
                                any_update_enabled() && !expired)) {
            atomic_store(&s->display_claim, 0U);
            return V71R7_L0_BUSY;
        }
        if (!atomic_load(&s->metrics.quarantined) || !atomic_load(&s->metrics.online)) {
            retain_hardware(s);
        }
    }

    for (unsigned i = 0U; i < NO_ROUTE; ++i) {
        RouteState *r = &s->route[i];
        atomic_store(&r->history_head, 0U);
        r->history_next = 0U;
        r->last_tried_gen = 0U;
        r->last_present_tick = 0U;
        r->presented_generation = 0U;
        memset(r->history, 0, sizeof(r->history));
    }
    atomic_store(&s->metrics.active_route, UINT32_MAX);
    atomic_store(&s->metrics.fps_milli, 0U);
    atomic_store(&s->metrics.last_presented_generation, 0U);
    s->last_reused_live_seq = 0U;
    s->last_lcd_capture_ms = 0U;

    uint32_t leased = atomic_load(&s->metrics.active_banks_mask);
    for (unsigned i = 0U; i < BANKS; ++i) {
        if (!(leased & (1U << i))) {
            atomic_store(&s->bank[i].state, BANK_FREE);
        }
    }
    atomic_store(&s->display_claim, 0U);

    if (!held) {
        atomic_store_explicit(&s->cache_ready_gen, 0U, memory_order_seq_cst);
        atomic_store_explicit(&s->baked_ready_gen, 0U, memory_order_seq_cst);
        if (atomic_load_explicit(&s->cache_readers, memory_order_seq_cst) != 0U) {
            return V71R7_L0_BUSY;
        }
        if (s->view.cpu) {
            if (photo_workspace_unpin(&s->pool) != PHOTO_WS_OK) {
                return V71R7_L0_CLOSE;
            }
            memset(&s->view, 0, sizeof(s->view));
            s->stage_cpu = NULL;
        }
        int ws_rc = photo_workspace_release(&s->pool);
        atomic_store(&s->metrics.pool_result, ws_rc);
        if (ws_rc != PHOTO_WS_OK) {
            return (ws_rc == PHOTO_WS_BUSY) ? V71R7_L0_BUSY : V71R7_L0_CLOSE;
        }
        atomic_store(&s->metrics.surface_bytes, 0U);
    }

    atomic_store_explicit(&s->cache_ready_gen, 0U, memory_order_release);
    atomic_store_explicit(&s->baked_ready_gen, 0U, memory_order_release);
    s->cache_ready = 0U;
    s->cache_lut_gen = 0U;
    s->baked_lut_gen = 0U;
    s->last_evf_paced_vsync = 0U;
    s->load_error = V71R7_L0_OK;
    s->closing = 0U;
    s->stop_started = 0U;
    return V71R7_L0_OK;
}

static uint32_t prepare_workspace(Layer0Ctx *s, uint64_t *slice_start, int want_xm6_guard)
{
    if (atomic_load(&s->metrics.quarantined)) {
        return V71R7_L0_BUSY;
    }
    if (anchor_pool_suspended(s) || atomic_load(&s->power_closing) ||
        atomic_load(&s->stop_lifecycle)) {
        return V71R7_L0_STATE;
    }
    if (s->load_error != V71R7_L0_OK) {
        return s->load_error;
    }

    if (!s->view.cpu) {
        uint32_t ws_flags = want_xm6_guard ? PHOTO_WS_FLAG_XM6_512M_GUARD : PHOTO_WS_FLAG_NONE;
        int status = photo_workspace_acquire_ex(&s->pool, WORK_BYTES, ws_flags);
        atomic_store(&s->metrics.pool_result, status);
        if (status != PHOTO_WS_OK) {
            return (status == PHOTO_WS_BUSY) ? V71R7_L0_BUSY : V71R7_L0_BUDGET;
        }
        status = photo_workspace_pin(&s->pool, &s->view);
        atomic_store(&s->metrics.pool_result, status);
        if (status != PHOTO_WS_OK) {
            (void)photo_workspace_release(&s->pool);
            s->load_error = V71R7_L0_STATE;
            return V71R7_L0_STATE;
        }
        atomic_store(&s->metrics.surface_bytes, WORK_BYTES);
        s->stage_cpu = s->view.cpu;
        for (unsigned i = 0U; i < BANKS; ++i) {
            Bank *bank = &s->bank[i];
            uintptr_t cpu = (uintptr_t)(s->view.cpu + GRADED_OFFSET + (size_t)i * BANK_BYTES);
            uintptr_t raw = TO_RAW(cpu);
            bank->raw_y = raw;
            bank->cpu_y = cpu;
            if (!l0_mapped_plane(bank->raw_y, bank->cpu_y, BANK_BYTES)) {
                s->load_error = V71R7_L0_INTEGRITY;
                return V71R7_L0_INTEGRITY;
            }
            /* Pad unused EVF backing tail cleanly (luma=0, chroma=128). */
            unsigned y_bytes = EVF_W * EVF_H;
            unsigned pad = SOURCE_UV_OFFSET - y_bytes;
            memset((void *)(cpu + y_bytes), 0, pad);
            memset((void *)(cpu + SOURCE_UV_OFFSET + y_bytes / 2U), 128, pad / 2U);
            l0_cv_clean_chunked((const void *)(cpu + y_bytes), pad);
            l0_cv_clean_chunked((const void *)(cpu + SOURCE_UV_OFFSET + y_bytes / 2U), pad / 2U);
            if (anchor_pool_suspended(s) || atomic_load(&s->power_closing) ||
                atomic_load(&s->stop_lifecycle) || !preview_checkpoint(s, slice_start)) {
                return V71R7_L0_STATE;
            }
        }
        s->cache_ready = 0U;
        s->cache_lut_gen = 0U;
        s->baked_lut_gen = 0U;
        atomic_store_explicit(&s->baked_ready_gen, 0U, memory_order_release);
    }

    if (!atomic_load(&s->metrics.online) || anchor_pool_suspended(s) ||
        atomic_load(&s->power_closing)) {
        return V71R7_L0_STATE;
    }
    if (atomic_load_explicit(&s->cache_invalidate_req, memory_order_acquire)) {
        atomic_store_explicit(&s->cache_ready_gen, 0U, memory_order_seq_cst);
        atomic_store_explicit(&s->baked_ready_gen, 0U, memory_order_seq_cst);
        if (atomic_load_explicit(&s->cache_readers, memory_order_seq_cst) != 0U) {
            return V71R7_L0_BUSY;
        }
        atomic_store_explicit(&s->cache_invalidate_req, 0U, memory_order_release);
        s->cache_ready = 0U;
        s->cache_lut_gen = 0U;
        s->baked_lut_gen = 0U;
    }
    if (!s->cache_ready) {
        atomic_store_explicit(&s->cache_ready_gen, 0U, memory_order_seq_cst);
        if (atomic_load_explicit(&s->cache_readers, memory_order_seq_cst) != 0U) {
            return V71R7_L0_BUSY;
        }
        for (unsigned offset = 0U; offset < REAR_CACHE; offset += CACHE_CLEAR_CHUNK) {
            if (anchor_pool_suspended(s) || atomic_load(&s->power_closing) ||
                atomic_load(&s->stop_lifecycle)) {
                return V71R7_L0_STATE;
            }
            memset(s->view.cpu + CACHE_OFFSET + offset, 0, CACHE_CLEAR_CHUNK);
            if (!preview_checkpoint(s, slice_start)) {
                return V71R7_L0_STATE;
            }
        }
        s->cache_ready = 1U;
        uint32_t cur_gen = 0U;
        if (query_lut_active(s, &cur_gen) && cur_gen != 0U) {
            s->cache_lut_gen = cur_gen;
            atomic_store_explicit(&s->cache_ready_gen, cur_gen, memory_order_release);
        } else {
            s->cache_lut_gen = 0U;
        }
    }
    if (!atomic_load(&s->metrics.online) || anchor_pool_suspended(s) ||
        atomic_load(&s->power_closing)) {
        return V71R7_L0_STATE;
    }
    atomic_store(&s->metrics.enabled, 1U);
    atomic_store(&s->metrics.display_ready, 1U);
    return V71R7_L0_OK;
}

static int source_generation_current(const RouteState *r, const FrameMeta *slot, uint64_t gen)
{
    return atomic_load_explicit(&slot->seq, memory_order_acquire) == gen &&
           (atomic_load_explicit(&r->history_head, memory_order_acquire) - gen) <= 4U;
}

static int read_source_frame(Layer0Ctx *s, uint32_t ticket, Stage *snapshot, SourcePlanes *source)
{
    unsigned index = ticket & ROUTE_MASK;
    RouteState *r = &s->route[index];
    uint64_t head = atomic_load_explicit(&r->history_head, memory_order_acquire);
    uint32_t lag = atomic_load(&s->metrics.source_lag);
    if (head <= lag) {
        return 0;
    }
    uint64_t gen = head - lag;
    if (gen <= r->last_tried_gen) {
        return 0;
    }
    r->last_tried_gen = gen;

    const FrameMeta *slot = &r->history[gen & (HISTORY - 1U)];
    if (atomic_load_explicit(&slot->seq, memory_order_acquire) != gen) {
        goto metadata_changed;
    }
    uintptr_t y = atomic_load_explicit(&slot->y, memory_order_relaxed);
    uintptr_t uv = atomic_load_explicit(&slot->uv, memory_order_relaxed);
    uint64_t stamp = atomic_load_explicit(&slot->stamp, memory_order_relaxed);
    uint64_t captured_ms = atomic_load_explicit(&slot->captured_ms, memory_order_relaxed);
    uint64_t captured_tick = atomic_load_explicit(&slot->captured_tick, memory_order_relaxed);
    uint32_t source_ticket = atomic_load_explicit(&slot->ticket, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&slot->seq, memory_order_acquire) != gen ||
        (atomic_load_explicit(&r->history_head, memory_order_acquire) - gen) > 3U ||
        source_ticket != ticket) {
        goto metadata_changed;
    }
    uint64_t now = now_ms();
    if (now == UINT64_MAX || now < captured_ms || (now - captured_ms) > SOURCE_MAX_AGE_MS) {
        return 0;
    }
    if (!l0_source_planes(&geometry[index], y, uv)) {
        return 0;
    }
    source->y = y;
    source->uv = uv;
    source->slot = slot;
    snapshot->cpu = s->stage_cpu;
    snapshot->captured_ms = captured_ms;
    snapshot->captured_tick = captured_tick;
    snapshot->stamp = stamp;
    snapshot->generation = gen;
    snapshot->copy_mode = atomic_load(&s->metrics.copy_mode);
    snapshot->ticket = ticket;
    return 1;

metadata_changed:
    COUNT(s, mismatch_meta);
    COUNT(s, enqueue_content_changed);
    return 0;
}

static __attribute__((noinline)) int copy_band(Layer0Ctx *s, const Stage *stage, uintptr_t plane,
                                               unsigned width, unsigned first, unsigned count,
                                               unsigned rows, int chroma)
{
    const Geometry *g = &geometry[stage->ticket & ROUTE_MASK];
    uintptr_t raw = plane + (uintptr_t)first * width;
    uintptr_t read_addr = raw;
    uint8_t *dst = stage->cpu + (chroma ? g->private_uv_offset : 0U) + (size_t)first * width;
    if (stage->copy_mode != 1U) {
        read_addr = l0_source_cached_view(raw);
#ifndef V71R7_HOST_TEST
        if (v71r7_neon_sentinel_is_allowed()) {
            layer0_copy_plane_neon(dst, (const uint8_t *)read_addr, (size_t)count * width, 1);
            l0_barrier();
            goto verify_spot;
        }
#endif
        photo_workspace_dcache_clean_chunked(read_addr, count * width);
    } else {
        l0_barrier();
    }
    IMAGE_COPY(dst, (const void *)read_addr, (size_t)count * width);
    l0_barrier();

#ifndef V71R7_HOST_TEST
verify_spot:;
#endif
    unsigned bad_row = 0U;
    uint32_t seed = (uint32_t)stage->stamp ^ (uint32_t)(stage->stamp >> 32);
    if (spot_matches_row_range(raw, dst, first, count, rows, width, seed, &bad_row)) {
        return 1;
    }
    if (chroma) {
        COUNT(s, mismatch_uv);
    } else {
        COUNT(s, mismatch_y);
    }
    atomic_store(&s->metrics.last_mismatch_row, (chroma ? g->height : 0U) + bad_row);
    return 0;
}

/* Real uITRON RTOS timed mutex acquisition on Pool 0 (0x40D2C698 -> tloc_mtx):
 * - Fast path: 0 ms nonblocking poll (< 1 us when uncontended).
 * - Contended path: blocks cleanly in uITRON scheduler (<= 2 ms, priority inheritance,
 *   0% CPU busy-spin, instant wakeup on unl_mtx 0x40D2C660) so AFLENS/WTDG_C3/lower-prio
 *   tasks run freely instead of starving under a busy loop! */
static int l0_pool_acquire(Layer0Ctx *s)
{
    if (l0_pool_lock_timed(0U) == 1) {
        return 1;
    }
    atomic_fetch_add_explicit(&s->pool_lock_waits, 1U, memory_order_relaxed);
    if (l0_pool_lock_timed(POOL_LOCK_WAIT_MS) == 1) {
        atomic_fetch_add_explicit(&s->pool_lock_wait_ok, 1U, memory_order_relaxed);
        return 1;
    }
    return 0;
}

static __attribute__((noinline)) int copy_source_frame(Layer0Ctx *s, const Stage *stage,
                                                       const SourcePlanes *source)
{
    uint32_t ticket = stage->ticket;
    unsigned index = ticket & ROUTE_MASK;
    const Geometry *g = &geometry[index];
    unsigned width = g->width;
    unsigned height = g->height;
    RouteState *r = &s->route[index];
    uint64_t gen = stage->generation;
    uint64_t captured_ms = stage->captured_ms;
    uint64_t copy_start = l0_ticks();
    unsigned chunk_rows = (stage->copy_mode == 1U) ? COPY_CHUNK_UNCACHED_ROWS : COPY_CHUNK_CACHED_ROWS;
    PoolOwner owner = {0U, 0U, 0U};
    unsigned total = height + height / 2U;

    if (!worker_layout(s, ticket) || !armed(s) || !source_generation_current(r, source->slot, gen)) {
        goto metadata_changed;
    }

    for (unsigned row = 0U; row < total;) {
        if (is_still_capturing(s)) {
            return 0;
        }
        if (!armed(s) || !source_generation_current(r, source->slot, gen)) {
            goto metadata_changed;
        }
        uint64_t now = now_ms();
        if (now == UINT64_MAX || now < captured_ms || (now - captured_ms) > SOURCE_MAX_AGE_MS) {
            return 0;
        }
        if (!l0_pool_acquire(s)) {
            COUNT(s, enqueue_skipped_busy);
            if (row == 0U && r->last_tried_gen == gen && gen > 0U) {
                r->last_tried_gen = gen - 1U;
            }
            return 0;
        }
        if (!l0_pool_owns_source(source->y, &owner) ||
            !source_generation_current(r, source->slot, gen)) {
            l0_pool_unlock();
            goto metadata_changed;
        }

        int chroma = (row >= height);
        unsigned first = chroma ? (row - height) : row;
        unsigned rows = chroma ? (height / 2U) : height;
        unsigned count = (rows - first < chunk_rows) ? (rows - first) : chunk_rows;
        int matches = copy_band(s, stage, chroma ? source->uv : source->y,
                                width, first, count, rows, chroma);
        l0_pool_unlock();
        if (!matches) {
            goto source_changed;
        }
        if (!source_generation_current(r, source->slot, gen)) {
            goto metadata_changed;
        }
        row += count;
        v71r7_heartbeat_progress(FX_Z8_HB_REAR_STEP);
    }

    if (!worker_layout(s, ticket) || !armed(s) || !source_generation_current(r, source->slot, gen)) {
        goto metadata_changed;
    }

    uint32_t copy_elapsed_us = duration_us(copy_start);
    timed(&s->metrics.copy_us, &s->metrics.max_copy_us, copy_elapsed_us);
    atomic_store_explicit(&s->metrics.last_copy_us, copy_elapsed_us, memory_order_relaxed);
    atomic_store(&s->metrics.last_captured_ms, captured_ms);
    COUNT(s, enqueue_copied);
    return 1;

metadata_changed:
    COUNT(s, mismatch_meta);
source_changed:
    COUNT(s, enqueue_content_changed);
    return 0;
}

static Bank *output_bank(Layer0Ctx *s, uint32_t ticket)
{
    for (unsigned i = 0U; i < BANKS; ++i) {
        if (claim(&s->bank[i].state, BANK_FREE, BANK_WORKING)) {
            return &s->bank[i];
        }
    }
    if (!claim(&s->display_claim, 0U, 1U)) {
        return NULL;
    }
    Bank *oldest = NULL;
    unsigned ready_current = 0U;
    int oldest_stale = 0;
    for (unsigned i = 0U; i < BANKS; ++i) {
        Bank *bank = &s->bank[i];
        if (atomic_load(&bank->state) != BANK_READY) {
            continue;
        }
        int stale = (atomic_load(&bank->ticket) != ticket);
        if (!stale) {
            ++ready_current;
        }
        if (!oldest || stale > oldest_stale ||
            (stale == oldest_stale && bank->captured_tick < oldest->captured_tick)) {
            oldest = bank;
            oldest_stale = stale;
        }
    }
    Bank *claimed = (oldest && (oldest_stale || ready_current >= 2U) &&
                     claim(&oldest->state, BANK_READY, BANK_WORKING))
                        ? oldest
                        : NULL;
    atomic_store(&s->display_claim, 0U);
    return claimed;
}

typedef struct {
    Layer0Ctx *s;
    const Stage *stage;
    uint32_t abort_reason;
} RenderCancelCtx;

static int render_should_abort(RenderCancelCtx *c)
{
    if (c->abort_reason != V71R7_L0_OK) {
        return 1;
    }
    Layer0Ctx *s = c->s;
    const Stage *st = c->stage;
    int capture = is_still_capturing(s);
    uint32_t reason = V71R7_L0_OK;
    if (capture || !armed(s) || !visible(st->ticket & ROUTE_MASK) ||
        atomic_load(&s->ticket) != st->ticket ||
        atomic_load(&s->layout_ticket) != st->ticket) {
        reason = V71R7_L0_EMPTY;
    } else if (!fresh(now_ms(), st->captured_ms)) {
        reason = V71R7_L0_TIMEOUT;
    }
    if (reason == V71R7_L0_OK) {
        return 0;
    }
    c->abort_reason = reason;
    if (capture) {
        COUNT(s, capture_aborts);
        v71r7_flight_record(FX_Z8_FLIGHT_PREVIEW_CAPTURE_ABORT, l0_core_id(),
                            st->ticket, (uint32_t)st->generation, (uint32_t)st->stamp);
    }
    return 1;
}

static int render_cancelled_cb(void *opaque)
{
    return render_should_abort((RenderCancelCtx *)opaque);
}

static void evidence_capture_src(Layer0Ctx *s, const Stage *stage, const Geometry *g)
{
    Layer0Evidence *out = &s->evidence;
    if ((atomic_load(&out->ready) && out->reserved == g->route) ||
        stage->copy_mode != atomic_load(&s->metrics.copy_mode)) {
        return;
    }
    atomic_store(&out->ready, 0U);
    unsigned width = g->width;
    unsigned height = g->height;
    const uint8_t *src = stage->cpu;
    for (unsigned j = 0U; j < LAYER0_EVIDENCE_ROWS; ++j) {
        unsigned row = (j * (height - 2U) / (LAYER0_EVIDENCE_ROWS - 1U)) & ~1U;
        for (unsigned i = 0U; i < LAYER0_EVIDENCE_COLS; ++i) {
            unsigned col = (i * (width - 2U) / (LAYER0_EVIDENCE_COLS - 1U)) & ~1U;
            unsigned at = row * width + col;
            unsigned uv = g->private_uv_offset + (row / 2U) * width + col;
            uint8_t *q = out->quads[j][i];
            q[0] = src[at];
            q[1] = src[at + 1U];
            q[2] = src[at + width];
            q[3] = src[at + width + 1U];
            q[4] = src[uv];
            q[5] = src[uv + 1U];
        }
    }
}

static void evidence_capture_dst(Layer0Ctx *s, const Stage *stage, const Bank *bank,
                                 const Geometry *g, uint64_t generation)
{
    Layer0Evidence *out = &s->evidence;
    if ((atomic_load(&out->ready) && out->reserved == g->route) ||
        stage->copy_mode != atomic_load(&s->metrics.copy_mode)) {
        return;
    }
    unsigned width = g->width;
    unsigned height = g->height;
    const uint8_t *dst = (const uint8_t *)bank->cpu_y;
    for (unsigned j = 0U; j < LAYER0_EVIDENCE_ROWS; ++j) {
        unsigned row = (j * (height - 2U) / (LAYER0_EVIDENCE_ROWS - 1U)) & ~1U;
        for (unsigned i = 0U; i < LAYER0_EVIDENCE_COLS; ++i) {
            unsigned col = (i * (width - 2U) / (LAYER0_EVIDENCE_COLS - 1U)) & ~1U;
            unsigned at = row * width + col;
            unsigned uv = g->private_uv_offset + (row / 2U) * width + col;
            uint8_t *q = out->quads[j][i];
            q[6] = dst[at];
            q[7] = dst[at + 1U];
            q[8] = dst[at + width];
            q[9] = dst[at + width + 1U];
            q[10] = dst[uv];
            q[11] = dst[uv + 1U];
        }
    }
    out->generation = (uint32_t)generation;
    out->stamp = stage->stamp;
    out->copy_mode = stage->copy_mode;
    out->reserved = g->route;
    atomic_store(&out->ready, 1U);
}

/* Unified WYSIWYG Frame Pipeline:
 *   Live->LCD Shared Reuse OR Copy source
 *   -> Stage 0 (.cube 3D LUT) -> Stage 1 (Ricoh HDF) -> Stage 2 (Nikon Zf Grain)
 *   -> Chunked <= 512 KiB dc civac + dsb sy -> Publish BANK_READY.
 */
static __attribute__((noinline))
uint32_t layer0_render_bank(Layer0Ctx *s, const Stage *stage, const SourcePlanes *source,
                            Bank *bank, int lut_active, const V71r7WysiwygOps *wops, int has_wops,
                            const V71r7WysiwygState *wstate)
{
    unsigned index = stage->ticket & ROUTE_MASK;
    const Geometry *g = &geometry[index];
    RenderCancelCtx cancel = {s, stage, V71R7_L0_OK};

    if (render_should_abort(&cancel)) {
        return cancel.abort_reason;
    }

    uint8_t *bank_y = (uint8_t *)bank->cpu_y;
    uint8_t *bank_uv = bank_y + g->private_uv_offset;
    Stage direct_stage = *stage;
    direct_stage.cpu = bank_y;

    uint32_t stages_applied = 0U;
    uint32_t lut_us = 0U, hdf_us = 0U, grain_us = 0U, clean_us = 0U;
    uint32_t want_lut_gen = 0U;
    if (lut_active) {
        if (!query_lut_active(s, &want_lut_gen)) {
            lut_active = 0;
            want_lut_gen = 0U;
        }
    }

    /* Single-Pass Live->LCD Shared Frame Consumer (legacy fallback ONLY when Layer0->Live
     * reverse sharing is NOT active, so LCD runs at full 60 fps and pushes 30 fps to Live!). */
    int live_reused = 0;
    uint64_t eff_captured_ms = stage->captured_ms;
    uint64_t eff_captured_tick = stage->captured_tick;
    if (index == LCD && g->width == 1024U && (g->height == 682U || g->height == 680U) &&
        (lut_active || wstate->hdf_level > 0U) &&
        !filmsim_runtime_live_layer0_feed_active()) {
        uint32_t reused_stages = 0U;
        uint32_t reused_seq = 0U;
        uint64_t reused_age_us = 0U;
        uint64_t reuse_start = l0_ticks();
        int rrc = filmsim_live_try_copy_graded_lcd(g->width, g->height, want_lut_gen,
                                                   wstate->hdf_level, 65000U,
                                                   s->last_reused_live_seq,
                                                   bank_y, bank_uv, g->width,
                                                   &reused_stages, &reused_seq, &reused_age_us);
        if (rrc < 0) {
            return V71R7_L0_STALE;
        }
        if (rrc > 0) {
            uint64_t now_m = now_ms();
            uint64_t age_ms = reused_age_us / 1000U;
            uint64_t cap_ms = (now_m != UINT64_MAX && now_m >= age_ms)
                                  ? (now_m - age_ms)
                                  : stage->captured_ms;
            if (s->last_lcd_capture_ms != 0U && cap_ms != UINT64_MAX &&
                cap_ms < s->last_lcd_capture_ms) {
                /* Do not jump backwards in time relative to the already-displayed LCD frame */
                COUNT(s, live_reuse_misses);
            } else {
                uint32_t reuse_us = duration_us(reuse_start);
                uint64_t hz = l0_frequency();
                uint64_t now_t = l0_ticks();
                uint64_t age_ticks = hz ? ((reused_age_us / 1000000U) * hz +
                                           ((reused_age_us % 1000000U) * hz) / 1000000U)
                                        : 0U;
                eff_captured_ms = cap_ms;
                eff_captured_tick = (now_t >= age_ticks) ? (now_t - age_ticks) : stage->captured_tick;
                s->last_reused_live_seq = reused_seq;
                timed(&s->metrics.copy_us, &s->metrics.max_copy_us, reuse_us);
                atomic_store_explicit(&s->metrics.last_copy_us, reuse_us, memory_order_relaxed);
                atomic_store(&s->metrics.last_captured_ms, eff_captured_ms);
                COUNT(s, live_reuse_hits);
                stages_applied |= (reused_stages & (V71R7_L0_STAGE_LUT | V71R7_L0_STAGE_HDF |
                                                    V71R7_L0_STAGE_GRAIN));
                live_reused = 1;
            }
        } else {
            COUNT(s, live_reuse_misses);
        }
    }

    if (!live_reused) {
        /* Copy validated source directly into exclusively-owned BANK_WORKING bank_y/bank_uv,
         * eliminating the redundant 1.05-1.64 MiB stage_cpu -> bank IMAGE_COPY while leaving
         * s->stage_cpu completely free for ARM HDF's 512 KiB scratch! */
        if (!copy_source_frame(s, &direct_stage, source)) {
            return V71R7_L0_STALE;
        }
    }
    if (render_should_abort(&cancel)) {
        return cancel.abort_reason;
    }

    uint64_t grade_start = l0_ticks();
#ifdef V71R7_HOST_TEST
    uint32_t *cache_4mib = (uint32_t *)(void *)(s->view.cpu + CACHE_OFFSET);
#endif
    uint32_t *baked_528k = (uint32_t *)(void *)(s->stage_cpu + BAKED_LUT_OFFSET);

    /* Capture source evidence quads before in-place LUT/HDF/Grain mutates bank_y/bank_uv. */
    if (!live_reused) {
        evidence_capture_src(s, &direct_stage, g);
    }

    int neon_allowed = v71r7_neon_sentinel_is_allowed();
    atomic_store_explicit(&s->metrics.neon_active, neon_allowed ? 1U : 0U, memory_order_relaxed);

    /* Refresh helper sentinel & online SMP helper telemetry honestly. */
    uint32_t hsent[3] = {0U, 0U, 0U};
    v71r7_smp_get_helper_sentinels(hsent);
    for (unsigned hi = 0U; hi < 3U; ++hi) {
        atomic_store_explicit(&s->metrics.helper_sentinel[hi], hsent[hi], memory_order_relaxed);
    }

    /* Stage 0: .cube 3D LUT (V71.7-r7 528 KiB L2-resident 64x64x33 pre-baked YCbCr LUT in
     * unused stage_cpu + 512 KiB tail, SMP CPU1+CPU2 prio 30, 0 cache misses!). */
    if (lut_active && !(stages_applied & V71R7_L0_STAGE_LUT)) {
        if (render_should_abort(&cancel)) {
            return cancel.abort_reason;
        }
        uint32_t cur_gen = 0U;
        if (!query_lut_active(s, &cur_gen) || cur_gen == 0U) {
            if (!wstate->active) {
                return V71R7_L0_STALE;
            }
            goto stage1_hdf;
        }
        if (s->cache_lut_gen != cur_gen ||
            atomic_load_explicit(&s->cache_invalidate_req, memory_order_acquire) != 0U) {
            atomic_store_explicit(&s->cache_ready_gen, 0U, memory_order_seq_cst);
            atomic_store_explicit(&s->baked_ready_gen, 0U, memory_order_seq_cst);
            if (atomic_load_explicit(&s->cache_readers, memory_order_seq_cst) != 0U) {
                return V71R7_L0_BUSY;
            }
            for (unsigned offset = 0U; offset < REAR_CACHE; offset += CACHE_CLEAR_CHUNK) {
                if (render_should_abort(&cancel)) {
                    return cancel.abort_reason;
                }
                memset(s->view.cpu + CACHE_OFFSET + offset, 0, CACHE_CLEAR_CHUNK);
            }
            atomic_store_explicit(&s->cache_invalidate_req, 0U, memory_order_release);
            s->cache_lut_gen = cur_gen;
            s->baked_lut_gen = 0U;
            s->cache_ready = 1U;
            atomic_store_explicit(&s->cache_ready_gen, cur_gen, memory_order_release);
        }

        /* Pin cache_readers so concurrent LUT swap waits for us before touching generation/cache. */
        atomic_fetch_add_explicit(&s->cache_readers, 1U, memory_order_seq_cst);
        if (atomic_load_explicit(&s->cache_ready_gen, memory_order_seq_cst) != cur_gen ||
            atomic_load_explicit(&s->cache_invalidate_req, memory_order_seq_cst) != 0U) {
            atomic_fetch_sub_explicit(&s->cache_readers, 1U, memory_order_release);
            return V71R7_L0_BUSY;
        }

        uint64_t lut_start = l0_ticks();
        v71r7_heartbeat_enter(FX_Z8_HB_REAR_STEP, FX_Z8_STAGE_REAR_GRADE, 0U, stage->ticket);
        int use_smp = (fx_z8_lut_frame_count(v71r7_io_gate()) == 0U) ? 1 : 0;
#ifdef V71R7_HOST_TEST
        int lrc = v71r7_lut_apply_nv12_baked(bank_y, bank_uv, g->width, g->height,
                                             g->width, g->width, cache_4mib, &s->cache_lut_gen, use_smp);
        (void)baked_528k;
#else
        int lrc = v71r7_lut_apply_nv12_baked(bank_y, bank_uv, g->width, g->height,
                                             g->width, g->width, baked_528k, &s->baked_lut_gen, use_smp);
#endif
        atomic_fetch_sub_explicit(&s->cache_readers, 1U, memory_order_release);
        v71r7_heartbeat_progress(FX_Z8_HB_REAR_STEP);
        lut_us = duration_us(lut_start);

        uint32_t online_helpers = v71r7_smp_online_helpers();
        atomic_store_explicit(&s->metrics.smp_helpers_online, online_helpers, memory_order_relaxed);
        v71r7_smp_get_helper_sentinels(hsent);
        for (unsigned hi = 0U; hi < 3U; ++hi) {
            atomic_store_explicit(&s->metrics.helper_sentinel[hi], hsent[hi], memory_order_relaxed);
        }

        if (lrc == -2) {
            return REAR_HUNG_DMA;
        }
        if (lrc > 0) {
            if (s->cache_lut_gen != 0U &&
                !atomic_load_explicit(&s->cache_invalidate_req, memory_order_acquire)) {
                atomic_store_explicit(&s->cache_ready_gen, s->cache_lut_gen, memory_order_release);
            }
            if (s->baked_lut_gen != 0U &&
                !atomic_load_explicit(&s->cache_invalidate_req, memory_order_acquire)) {
                atomic_store_explicit(&s->baked_ready_gen, s->baked_lut_gen, memory_order_release);
            }
            stages_applied |= V71R7_L0_STAGE_LUT;
            atomic_store_explicit(&s->metrics.worker_cores,
                                  (use_smp && online_helpers > 0U) ? 2U : 1U,
                                  memory_order_relaxed);
        } else if (!wstate->active) {
            return V71R7_L0_STALE;
        }
    } else {
        uint32_t online_helpers = v71r7_smp_online_helpers();
        atomic_store_explicit(&s->metrics.smp_helpers_online, online_helpers, memory_order_relaxed);
        atomic_store_explicit(&s->metrics.worker_cores, 1U, memory_order_relaxed);
    }

stage1_hdf:
    /* Stage 1: Ricoh Optical HDF (LCD 1024x682 prefers isolated non-blocking XM6 Core 0
     * Slot 41 LIVE_256_YC when bank lies in one 512 MiB window; EVF 1280x852 & fallback
     * use ARM NEON with s->stage_cpu as 512 KiB HDF scratch!). */
    if (wstate->hdf_level > 0U && !(stages_applied & V71R7_L0_STAGE_HDF) &&
        has_wops && wops->apply_hdf_nv12) {
        if (render_should_abort(&cancel)) {
            return cancel.abort_reason;
        }
        uint64_t hdf_start = l0_ticks();
        v71r7_heartbeat_enter(FX_Z8_HB_REAR_STEP, FX_Z8_STAGE_REAR_HDF_GRAIN, 0U, stage->ticket);
        int xm6_bank_ok = (index == LCD || index == EVF) &&
                          l0_bank_in_single_512m_window(bank->raw_y, BANK_BYTES);
        uintptr_t y_dma = xm6_bank_ok ? bank->raw_y : 0U;
        uintptr_t uv_dma = xm6_bank_ok ? (bank->raw_y + g->private_uv_offset) : 0U;
        int hrc = wops->apply_hdf_nv12(wops->ctx, bank_y, bank_uv, y_dma, uv_dma,
                                       g->width, g->height, g->width, g->width,
                                       wstate->hdf_level, wstate->xm6_mode, wstate->monochrome,
                                       (index == EVF) ? 1 : 0,
                                       s->stage_cpu, V71R7_LAYER0_HDF_SCRATCH_BYTES,
                                       render_cancelled_cb, &cancel);
        v71r7_heartbeat_progress(FX_Z8_HB_REAR_STEP);
        hdf_us = duration_us(hdf_start);
        if (hrc == -2) {
            return REAR_HUNG_DMA;
        }
        if (hrc < 0) {
            return cancel.abort_reason != V71R7_L0_OK ? cancel.abort_reason : V71R7_L0_STALE;
        }
        if (hrc == 0) {
            stages_applied |= V71R7_L0_STAGE_HDF;
        }
    }

    /* V71.7-r7 Reverse Single-Pass Layer 0 -> Live Shared Frame Feed:
     * Strictly AFTER Stage 0 LUT + Stage 1 HDF and strictly BEFORE Stage 2 Zf Grain
     * (Live Photo CPU0 fuses Zf Grain cleanly into Wave5 -> zero double-grain!).
     * Feeds native EVF 1280x852 or LCD 1024x682 into LBUF at >= 18 ms cadence so Live
     * does ZERO duplicate LUT/HDF and ZERO XM6 contention! */
    if (!live_reused && (stages_applied & (V71R7_L0_STAGE_LUT | V71R7_L0_STAGE_HDF)) != 0U &&
        !render_should_abort(&cancel)) {
        uint64_t hz = l0_frequency();
        uint64_t cap_us = (hz && eff_captured_tick)
                              ? (eff_captured_tick / hz * UINT64_C(1000000) +
                                 (eff_captured_tick % hz) * UINT64_C(1000000) / hz)
                              : 0U;
        (void)filmsim_runtime_live_push_layer0_nv12(
            bank_y, bank_uv, g->width, g->height, g->width, g->width, cap_us,
            (stages_applied & V71R7_L0_STAGE_LUT) ? want_lut_gen : 0U,
            (stages_applied & V71R7_L0_STAGE_HDF) ? wstate->hdf_level : 0U);
    }

    /* Stage 2: Nikon Zf Film Grain (512x256 GTILE 1-pass NEON + fused L1-hot dc cvac).
     * Skipped if Live->LCD shared frame already applied grain (zero double-grain!). */
    if (wstate->zf_intensity > 0U && !(stages_applied & V71R7_L0_STAGE_GRAIN) &&
        has_wops && wops->apply_grain_nv12) {
        if (render_should_abort(&cancel)) {
            return cancel.abort_reason;
        }
        uint64_t grain_start = l0_ticks();
        v71r7_heartbeat_enter(FX_Z8_HB_REAR_STEP, FX_Z8_STAGE_REAR_HDF_GRAIN, 0U, stage->ticket);
        int grc = wops->apply_grain_nv12(wops->ctx, bank_y, g->width, g->height, g->width);
        v71r7_heartbeat_progress(FX_Z8_HB_REAR_STEP);
        grain_us = duration_us(grain_start);
        if (grc == -2) {
            return REAR_HUNG_DMA;
        }
        if (grc < 0) {
            return cancel.abort_reason != V71R7_L0_OK ? cancel.abort_reason : V71R7_L0_STALE;
        }
        if (grc == 0) {
            stages_applied |= V71R7_L0_STAGE_GRAIN;
        }
    }

    if (!stages_applied) {
        return V71R7_L0_EMPTY;
    }

    /* Chunked <= 512 KiB dc civac + dsb sy before publishing bank to 0x40CD862C.
     * On bare metal, Stage 2 Grain already cleaned every Y row L1-hot + dsb sy! */
    uint64_t clean_start = l0_ticks();
#ifndef V71R7_HOST_TEST
    if (!(stages_applied & V71R7_L0_STAGE_GRAIN)) {
        photo_workspace_dcache_clean_chunked((uintptr_t)bank_y, g->active_y_bytes);
    }
#else
    photo_workspace_dcache_clean_chunked((uintptr_t)bank_y, g->active_y_bytes);
#endif
    photo_workspace_dcache_clean_chunked((uintptr_t)bank_uv, g->active_y_bytes / 2U);
    clean_us = duration_us(clean_start);

    atomic_store_explicit(&s->metrics.last_lut_us, lut_us, memory_order_relaxed);
    atomic_store_explicit(&s->metrics.last_hdf_us, hdf_us, memory_order_relaxed);
    atomic_store_explicit(&s->metrics.last_grain_us, grain_us, memory_order_relaxed);
    atomic_store_explicit(&s->metrics.last_clean_us, clean_us, memory_order_relaxed);
    timed(&s->metrics.grade_us, &s->metrics.max_grade_us, duration_us(grade_start));

    if (render_should_abort(&cancel)) {
        return cancel.abort_reason;
    }
    if (!worker_layout(s, stage->ticket) || !armed(s)) {
        return V71R7_L0_EMPTY;
    }
    uint64_t now = now_ms();
    if (!fresh(now, eff_captured_ms)) {
        return V71R7_L0_TIMEOUT;
    }

    uint64_t generation = stage->generation;
    bank->captured_ms = eff_captured_ms;
    bank->captured_tick = eff_captured_tick;
    atomic_store(&bank->generation, generation);
    atomic_store(&bank->ticket, stage->ticket);
    if (!live_reused) {
        evidence_capture_dst(s, stage, bank, g, generation);
    }
    if (index == LCD && eff_captured_ms != UINT64_MAX && eff_captured_ms > s->last_lcd_capture_ms) {
        s->last_lcd_capture_ms = eff_captured_ms;
    }

    if (!atomic_load(&s->metrics.grades) || atomic_load(&s->metrics.last_source_stamp) != stage->stamp) {
        COUNT(s, distinct_sources);
    } else {
        COUNT(s, repeated_sources);
    }
    atomic_store(&s->metrics.last_source_stamp, stage->stamp);
    atomic_store(&s->metrics.last_graded_ms, now);
    atomic_store(&s->last_stages_applied, stages_applied);

    if (stages_applied != s->last_logged_stages || g->route != s->last_logged_route) {
        s->last_logged_stages = stages_applied;
        s->last_logged_route = g->route;
        v71r7_flight_record(FX_Z8_FLIGHT_L0_WYSIWYG, l0_core_id(),
                            g->route, stages_applied, wstate->cfg_version);
    }

    if (render_should_abort(&cancel)) {
        return cancel.abort_reason;
    }
    atomic_store(&bank->state, BANK_READY);
    COUNT(s, grades);
    return V71R7_L0_OK;
}

static __attribute__((noinline)) uint32_t make_frame(Layer0Ctx *s, int lut_active,
                                                     const V71r7WysiwygOps *wops, int has_wops,
                                                     const V71r7WysiwygState *wstate)
{
    uint64_t entered = l0_ticks();
    uint64_t slice_start = entered;
    uint32_t ticket = observe_route(s);
    unsigned index = ticket & ROUTE_MASK;
    if (index == EVF && s->evf_slice_start) {
        slice_start = s->evf_slice_start;
    }
    s->evf_slice_start = 0U;

    if (is_still_capturing(s)) {
        COUNT(s, jpeg_pauses);
        return V71R7_L0_EMPTY;
    }
    if (!worker_layout(s, ticket) || !preview_checkpoint(s, &slice_start)) {
        return V71R7_L0_EMPTY;
    }

    int want_xm6_guard = (wstate->hdf_level > 0U && wstate->xm6_mode == 0U);
    uint32_t rc = prepare_workspace(s, &slice_start, want_xm6_guard);
    if (rc != V71R7_L0_OK) {
        return rc;
    }

    RouteState *r = &s->route[index];
    int neon = v71r7_neon_sentinel_is_allowed();
    atomic_store_explicit(&s->metrics.sentinel_status, neon ? 1U : 0U, memory_order_relaxed);
    if (!neon) {
        atomic_store_explicit(&s->metrics.neon_active, 0U, memory_order_relaxed);
    }

    int evf_neon = (index == EVF && neon);
    unsigned turn_ms = evf_neon ? EVF_NEON_STEP_MS : (neon ? LCD_NEON_STEP_MS : STEP_BURST_MS);
    unsigned turn_frames = evf_neon ? EVF_NEON_STEP_FRAMES
                                    : (neon ? LCD_NEON_STEP_FRAMES : STEP_MAX_FRAMES);
    COUNT(s, step_bursts);

    uint64_t last_activity_ms = now_ms();
    uint64_t burst_start = evf_neon ? tick_ms(entered) : last_activity_ms;
    uint64_t last_seen_head = atomic_load_explicit(&r->history_head, memory_order_acquire);
    unsigned graded = 0U;
    int turn_expired = 0;

    while (graded < turn_frames) {
        uint64_t now = now_ms();
        if (now == UINT64_MAX || now < burst_start) {
            break;
        }
        if ((now - burst_start) >= turn_ms) {
            turn_expired = 1;
            break;
        }
        if (anchor_pool_suspended(s) || atomic_load(&s->power_closing) ||
            atomic_load(&s->stop_lifecycle)) {
            rc = V71R7_L0_EMPTY;
            break;
        }
        if (is_still_capturing(s)) {
            COUNT(s, jpeg_pauses);
            break;
        }
        if (!worker_layout(s, ticket) || !armed(s)) {
            rc = V71R7_L0_EMPTY;
            break;
        }
        uint64_t head = atomic_load_explicit(&r->history_head, memory_order_acquire);
        uint32_t lag = atomic_load(&s->metrics.source_lag);
        if (head != last_seen_head) {
            last_seen_head = head;
            last_activity_ms = now;
        }
        if (head <= lag || (head - lag) <= r->last_tried_gen) {
            COUNT(s, wait_no_source);
            if (now >= last_activity_ms && (now - last_activity_ms) >= STEP_IDLE_MS) {
                break;
            }
            if (!preview_delay(s, &slice_start)) {
                break;
            }
            continue;
        }
        if (index == LCD && neon && now >= s->last_neon_frame_ms &&
            (now - s->last_neon_frame_ms) < LCD_NEON_FRAME_INTERVAL_MS) {
            if (!preview_delay(s, &slice_start)) {
                break;
            }
            continue;
        }
        /* Adaptive 30.0 fps EVF VSync pacing ONLY when independent Live Mode 2 is running
         * (e.g. WYSIWYG Off + Live HDF On); when Layer0->Live shares frames or Live is Off,
         * EVF runs completely unthrottled at 50-60 fps! */
        if (index == EVF && neon && filmsim_runtime_live_independent_active()) {
            uint64_t ev_seq = atomic_load_explicit(&s->metrics.evf_vsync_seq, memory_order_acquire);
            if (s->last_evf_paced_vsync != 0U && ev_seq >= s->last_evf_paced_vsync &&
                (ev_seq - s->last_evf_paced_vsync) < 2U) {
                if (!preview_delay(s, &slice_start)) {
                    break;
                }
                continue;
            }
        }

        Bank *bank = output_bank(s, ticket);
        if (!bank) {
            COUNT(s, wait_no_bank);
            if (!preview_delay(s, &slice_start)) {
                break;
            }
            continue;
        }

        FxZ8LutIoGate *gate = v71r7_io_gate();
        uint32_t epoch = fx_z8_lut_io_epoch(gate);
        if (!fx_z8_lut_io_take(gate, epoch)) {
            atomic_store(&bank->state, BANK_FREE);
            rc = V71R7_L0_EMPTY;
            break;
        }

        Stage snapshot;
        SourcePlanes source;
        uint64_t frame_start = l0_ticks();
        v71r7_heartbeat_enter(FX_Z8_HB_REAR_STEP, FX_Z8_STAGE_REAR_COPY, 0U, ticket);
        if (read_source_frame(s, ticket, &snapshot, &source)) {
            rc = layer0_render_bank(s, &snapshot, &source, bank, lut_active, wops, has_wops, wstate);
        } else {
            rc = V71R7_L0_STALE;
        }
        v71r7_heartbeat_enter(FX_Z8_HB_REAR_STEP, FX_Z8_STAGE_WORKER_STEP, 0U, ticket);
        fx_z8_lut_io_drop(gate);

        if (rc == REAR_HUNG_DMA) {
            /* Hung XM6 Live DMA (-2): permanently quarantine this bank so it is NEVER
             * returned to BANK_FREE or freed to Class 0x24 while DSP DMA may still write! */
            unsigned bidx = (unsigned)(bank - &s->bank[0]);
            atomic_store(&bank->ticket, ticket);
            atomic_store(&bank->state, BANK_QUARANTINED);
            atomic_fetch_or(&s->metrics.active_banks_mask, 1U << bidx);
            atomic_fetch_or(&s->quarantine_mask, 1U << (ticket & ROUTE_MASK));
            atomic_store(&s->metrics.quarantined, 1U);
            rc = V71R7_L0_BUSY;
            break;
        }
        if (rc == V71R7_L0_STALE) {
            atomic_store(&bank->state, BANK_FREE);
            rc = V71R7_L0_OK;
            last_activity_ms = now_ms();
            if (!preview_delay(s, &slice_start)) {
                break;
            }
            continue;
        }
        if (rc == V71R7_L0_TIMEOUT) {
            COUNT(s, grade_timeouts);
        }
        if (rc != V71R7_L0_OK) {
            if (rc == V71R7_L0_EMPTY) {
                r->last_tried_gen = atomic_load_explicit(&r->history_head, memory_order_acquire);
            }
            atomic_store(&bank->state, BANK_FREE);
            break;
        }

        if (index == LCD && neon) {
            s->last_neon_frame_ms = now;
        } else if (index == EVF) {
            s->last_evf_paced_vsync = atomic_load_explicit(&s->metrics.evf_vsync_seq, memory_order_acquire);
        }
        timed(&s->metrics.frame_us, &s->metrics.max_frame_us, duration_us(frame_start));
        ++graded;
        last_activity_ms = now_ms();
        int arm_capture = (fx_z8_lut_frame_count(gate) != 0U);
        if (!(!arm_capture ? preview_checkpoint(s, &slice_start)
                           : preview_delay(s, &slice_start))) {
            break;
        }
    }

    if (rc != V71R7_L0_OK && rc != V71R7_L0_EMPTY) {
        return rc;
    }
    int live = armed(s) && visible(index) && atomic_load(&s->ticket) == ticket;
    if (neon && graded && (turn_expired || graded == turn_frames) && live &&
        update_enabled(index) && atomic_load(&s->layout_ticket) == ticket && !is_still_capturing(s)) {
        if (evf_neon) {
            s->evf_slice_start = slice_start;
        }
        return REAR_STREAMING;
    }
    return (graded || live) ? V71R7_L0_OK : V71R7_L0_EMPTY;
}

/* ============================================================================
 * Public V71.7 Layer 0 Lifecycle, HangFix V69 & Power Sync Entry Points
 * ============================================================================ */

int v71r7_layer0_init(void)
{
    V71r7DramAnchor *anc = v71r7_anchor();
    if (__atomic_load_n(&anc->magic, __ATOMIC_ACQUIRE) != V71R7_ANCHOR_MAGIC) {
        memset(anc, 0, sizeof(*anc));
        __atomic_store_n(&anc->magic, V71R7_ANCHOR_MAGIC, __ATOMIC_RELEASE);
    }

    Layer0Ctx *existing = layer0_ctx_get();
    if (existing) {
        return 0;
    }

    Layer0Ctx *s = (Layer0Ctx *)HEAP_ALLOC((uint32_t)sizeof(*s), 0U);
    if (!s) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->magic = V71R7_LAYER0_CTX_MAGIC;
    s->size = (uint32_t)sizeof(*s);

    atomic_store(&s->metrics.version, 12U);
    atomic_store(&s->metrics.bytes, (uint32_t)sizeof(Layer0Metrics));
    atomic_store(&s->metrics.hw_current, -1);
    atomic_store(&s->metrics.active_route, UINT32_MAX);
    atomic_store(&s->metrics.lcd_hw_current, -1);
    atomic_store(&s->metrics.evf_hw_current, -1);
    atomic_store(&s->metrics.display_mode, LAYER0_DEFAULT_DISPLAY_MODE);
    atomic_store(&s->metrics.copy_mode, LAYER0_DEFAULT_COPY_MODE);
    atomic_store(&s->metrics.source_lag, LAYER0_DEFAULT_SOURCE_LAG);
    atomic_store(&s->metrics.last_mismatch_row, UINT32_MAX);
    atomic_store(&s->metrics.online, 1U);

    s->power.version = 1U;
    s->power.bytes = (uint32_t)sizeof(RearPowerMetrics);

    atomic_store(&s->ticket, NO_ROUTE);
    atomic_store(&s->layout_ticket, NO_ROUTE);
    s->last_logged_route = UINT32_MAX;

    /* Open FxZ8LutIoGate if not yet opened and not lifecycle-stopped. */
    FxZ8LutIoGate *gate = v71r7_io_gate();
    if (!fx_z8_lut_io_epoch(gate) &&
        !(__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) & FX_Z8_LUT_IO_STOPPED)) {
        fx_z8_lut_io_reopen(gate, 0);
    }
    atomic_store(&s->io_epoch, fx_z8_lut_io_epoch(gate));

    /* Sync initial pool_suspended flag if anchor already marked it. */
    if (__atomic_load_n(&anc->flags, __ATOMIC_ACQUIRE) & V71R7_ANCHOR_FLAG_POOL_SUSPENDED) {
        atomic_store(&s->pool_suspended, 1U);
    }

    void *expected = NULL;
    if (!__atomic_compare_exchange_n(&anc->layer0_ctx, &expected, s,
                                     0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        HEAP_FREE(s);
        return 0;
    }
    /* Strict Dormant Invariant: 0 B Class 0x24 allocated at init! */
    return 0;
}

int v71r7_layer0_set_wysiwyg_ops(const V71r7WysiwygOps *ops)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (!s) {
        return -1;
    }
    uint32_t seq = atomic_load_explicit(&s->ops_seqlock, memory_order_relaxed);
    atomic_store_explicit(&s->ops_seqlock, seq + 1U, memory_order_release);
    if (ops) {
        s->wysiwyg_ops = *ops;
        s->ops_registered = 1U;
    } else {
        memset(&s->wysiwyg_ops, 0, sizeof(s->wysiwyg_ops));
        s->ops_registered = 0U;
    }
    atomic_store_explicit(&s->ops_seqlock, seq + 2U, memory_order_release);
    return 0;
}

void v71r7_layer0_notify_wysiwyg_changed(int hdf_level, int zf_intensity, uint32_t cfg_version)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (hdf_level > 0 || zf_intensity > 0) {
        anchor_flag_set(V71R7_ANCHOR_FLAG_WYSIWYG_ACTIVE);
    } else {
        anchor_flag_clear(V71R7_ANCHOR_FLAG_WYSIWYG_ACTIVE);
    }
    if (!s) {
        return;
    }
    atomic_store_explicit(&s->push_wysiwyg_hdf, hdf_level > 0 ? (uint32_t)hdf_level : 0U,
                          memory_order_release);
    atomic_store_explicit(&s->push_wysiwyg_grain, zf_intensity > 0 ? (uint32_t)zf_intensity : 0U,
                          memory_order_release);
    atomic_store_explicit(&s->push_wysiwyg_ver, cfg_version, memory_order_release);
}

void v71r7_layer0_cache_invalidate_before_swap(void)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (!s) {
        return;
    }
    atomic_store_explicit(&s->cache_ready_gen, 0U, memory_order_seq_cst);
    atomic_store_explicit(&s->baked_ready_gen, 0U, memory_order_seq_cst);
    atomic_store_explicit(&s->cache_invalidate_req, 1U, memory_order_seq_cst);
    /* Wait briefly for any active cache reader (Live or Layer 0) to leave before
     * LutCoreCtx->generation advances, eliminating concurrent 4 MiB memset races. */
    for (unsigned i = 0U; i < 50U; ++i) {
        if (atomic_load_explicit(&s->cache_readers, memory_order_seq_cst) == 0U) {
            break;
        }
        if (SLEEP_MS(1U) != 0) {
            break;
        }
    }
}

void v71r7_layer0_notify_lut_changed(uint32_t generation, int active)
{
    V71r7DramAnchor *anc = v71r7_anchor();
    if (__atomic_load_n(&anc->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC) {
        __atomic_store_n(&anc->active_generation, generation, __ATOMIC_RELEASE);
        if (active) {
            anchor_flag_set(V71R7_ANCHOR_FLAG_LUT_ACTIVE);
        } else {
            anchor_flag_clear(V71R7_ANCHOR_FLAG_LUT_ACTIVE);
        }
    }
    Layer0Ctx *s = layer0_ctx_get();
    if (!s) {
        return;
    }
    uint32_t prev_gen = atomic_exchange_explicit(&s->lut_generation, generation, memory_order_acq_rel);
    atomic_store_explicit(&s->lut_active, active ? 1U : 0U, memory_order_release);
    if (!active || prev_gen != generation) {
        atomic_store_explicit(&s->cache_ready_gen, 0U, memory_order_seq_cst);
        atomic_store_explicit(&s->baked_ready_gen, 0U, memory_order_seq_cst);
    }
    if (active && prev_gen != generation) {
        atomic_store(&s->evidence.ready, 0U);
        atomic_store_explicit(&s->cache_invalidate_req, 1U, memory_order_seq_cst);
    }
}

/* HangFix V69 Mandatory Hook: disable swaps at 0x40CD862C, wait bounded <= 100 ms
 * for active VSync bank leases & worker_step to drain, release 14.55 MiB photo_workspace
 * (0 B held across stock Class 0x24 teardown, with safe non-quarantined force-retire). */
void v71r7_layer0_pool_suspend(void)
{
    V71r7DramAnchor *anc = v71r7_anchor();
    uint32_t prev_flag = 0U;
    if (__atomic_load_n(&anc->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC) {
        prev_flag = __atomic_fetch_or(&anc->flags, V71R7_ANCHOR_FLAG_POOL_SUSPENDED,
                                      __ATOMIC_ACQ_REL) & V71R7_ANCHOR_FLAG_POOL_SUSPENDED;
    }
    Layer0Ctx *s = layer0_ctx_get();
    if (!s) {
        if (!prev_flag) {
            v71r7_flight_record(FX_Z8_FLIGHT_POOL_SUSPEND, l0_core_id(), 0U, 0U, 0U);
        }
        return;
    }

    uint32_t prev_ctx = atomic_exchange_explicit(&s->pool_suspended, 1U, memory_order_acq_rel);
    atomic_store_explicit(&s->cache_ready_gen, 0U, memory_order_seq_cst);
    atomic_store_explicit(&s->baked_ready_gen, 0U, memory_order_seq_cst);
    atomic_store(&s->metrics.display_ready, 0U);
    atomic_store(&s->metrics.enabled, 0U);

    uint32_t mask_before = atomic_load(&s->metrics.active_banks_mask);
    uint32_t ws_before = atomic_load(&s->metrics.surface_bytes);
    if (!prev_ctx || ws_before != 0U) {
        v71r7_flight_record(FX_Z8_FLIGHT_POOL_SUSPEND, l0_core_id(), mask_before, ws_before, 0U);
    }

    uint64_t entered = now_ms();
    int have_busy = claim(&s->busy, 0U, 1U);
    int drained = display_drained(s);

    for (unsigned i = 0U; i < POOL_SUSPEND_DRAIN_MS; ++i) {
        if (!have_busy) {
            have_busy = claim(&s->busy, 0U, 1U);
        }
        if (!drained && claim(&s->display_claim, 0U, 1U)) {
            retire_stopped_display_banks_locked(s);
            atomic_store(&s->display_claim, 0U);
        }
        drained = display_drained(s);
        if (have_busy && drained && !atomic_load(&s->metrics.hook_active) &&
            atomic_load_explicit(&s->cache_readers, memory_order_acquire) == 0U) {
            break;
        }
        uint64_t now = now_ms();
        if (entered == UINT64_MAX || now == UINT64_MAX || now < entered ||
            (now - entered) >= POOL_SUSPEND_DRAIN_MS || SLEEP_MS(1U)) {
            break;
        }
    }

    if (!have_busy) {
        have_busy = claim(&s->busy, 0U, 1U);
    }
    if (have_busy) {
        if (photo_workspace_is_allocated(&s->pool) || s->view.cpu) {
            (void)retire_workspace_locked(s, 0, 1);
        }
        atomic_store(&s->busy, 0U);
    }
}

void v71r7_layer0_pool_resume(void)
{
    V71r7DramAnchor *anc = v71r7_anchor();
    uint32_t prev_flag = 0U;
    if (__atomic_load_n(&anc->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC) {
        prev_flag = __atomic_fetch_and(&anc->flags, ~V71R7_ANCHOR_FLAG_POOL_SUSPENDED,
                                       __ATOMIC_ACQ_REL) & V71R7_ANCHOR_FLAG_POOL_SUSPENDED;
    }
    Layer0Ctx *s = layer0_ctx_get();
    uint32_t prev_ctx = s ? atomic_exchange_explicit(&s->pool_suspended, 0U, memory_order_acq_rel)
                          : 0U;
    if (!prev_flag && !prev_ctx) {
        return;
    }
    if (!s) {
        v71r7_flight_record(FX_Z8_FLIGHT_POOL_RESUME, l0_core_id(), 0U, 0U, 0U);
        return;
    }
    uint32_t lut_on = atomic_load_explicit(&s->lut_active, memory_order_acquire);
    uint32_t ws_on = (__atomic_load_n(&anc->flags, __ATOMIC_ACQUIRE) &
                      V71R7_ANCHOR_FLAG_WYSIWYG_ACTIVE) ? 1U : 0U;
    v71r7_flight_record(FX_Z8_FLIGHT_POOL_RESUME, l0_core_id(), lut_on, ws_on, 0U);
}

/* POWER_OFF_SYNC (10): close FxZ8LutIoGate, drain VSync banks (<= 100 ms) + Still/IO leases
 * (<= 500 ms), release 14.55 MiB photo_workspace cleanly BEFORE BSShutdown (ev=7). */
void v71r7_layer0_power_off_sync(void)
{
    FxZ8LutIoGate *gate = v71r7_io_gate();
    fx_z8_lut_io_close(gate, 0);

    Layer0Ctx *s = layer0_ctx_get();
    if (!s) {
        v71r7_flight_record(FX_Z8_FLIGHT_PREVIEW_POWER_OFF, l0_core_id(),
                            POWER_OFF_SYNC, 0U, fx_z8_lut_frame_count(gate));
        return;
    }

    atomic_store(&s->power_closing, 1U);
    atomic_store(&s->metrics.display_ready, 0U);
    atomic_store(&s->metrics.enabled, 0U);
    atomic_fetch_add(&s->power.off_calls, 1U);
    uint32_t mask = atomic_load(&s->metrics.active_banks_mask);
    atomic_store(&s->power.before_mask, mask);
    v71r7_flight_record(FX_Z8_FLIGHT_PREVIEW_POWER_OFF, l0_core_id(),
                        POWER_OFF_SYNC, mask, fx_z8_lut_frame_count(gate));

    uint64_t start = l0_ticks();
    uint64_t entered = tick_ms(start);
    int drained = display_drained(s);

    for (unsigned i = 0U; i < POWER_DRAIN_TICKS && (!drained || io_pending()); ++i) {
        uint64_t now = now_ms();
        if (entered == UINT64_MAX || now == UINT64_MAX || now < entered ||
            (now - entered) >= POWER_DRAIN_MS ||
            (!io_pending() && (!any_update_enabled() || atomic_load(&s->metrics.quarantined))) ||
            SLEEP_MS(1U)) {
            break;
        }
        drained = display_drained(s);
    }

    if (__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) & FX_Z8_LUT_IO_FRAME) {
        atomic_store(&s->power.result, V71R7_L0_BUSY);
        uint64_t grace = l0_ticks();
        for (unsigned i = 0U; i < POWER_FRAME_DRAIN_MS &&
             (__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) & FX_Z8_LUT_IO_FRAME); ++i) {
            if (duration_us(grace) >= 1000U * POWER_FRAME_DRAIN_MS || SLEEP_MS(1U)) {
                break;
            }
        }
        drained = display_drained(s);
    }

    /* Release photo_workspace cleanly once hardware banks & worker_step are idle. */
    int have_busy = claim(&s->busy, 0U, 1U);
    for (unsigned i = 0U; !have_busy && i < 20U; ++i) {
        if (SLEEP_MS(1U)) {
            break;
        }
        have_busy = claim(&s->busy, 0U, 1U);
    }
    if (have_busy) {
        if (photo_workspace_is_allocated(&s->pool) || s->view.cpu) {
            (void)retire_workspace_locked(s, 0, 0);
        }
        atomic_store(&s->busy, 0U);
    }

    drained = display_drained(s);
    atomic_store(&s->power.elapsed_us, duration_us(start));
    atomic_store(&s->power.after_mask, atomic_load(&s->metrics.active_banks_mask));
    atomic_store(&s->power.result,
                 (drained && !io_pending() && !photo_workspace_is_allocated(&s->pool))
                     ? V71R7_L0_OK
                     : V71R7_L0_BUSY);
}

/* POWER_CANCEL_SYNC (11): reopen FxZ8LutIoGate unless lifecycle STOPPED bit is set. */
void v71r7_layer0_power_cancel_sync(void)
{
    FxZ8LutIoGate *gate = v71r7_io_gate();
    fx_z8_lut_io_reopen(gate, 1);
    uint32_t new_epoch = fx_z8_lut_io_epoch(gate);

    Layer0Ctx *s = layer0_ctx_get();
    if (s) {
        atomic_fetch_add(&s->power.cancel_calls, 1U);
        if (new_epoch != 0U && !atomic_load(&s->stop_lifecycle)) {
            atomic_store(&s->io_epoch, new_epoch);
            atomic_store(&s->power_closing, 0U);
        }
        v71r7_flight_record(FX_Z8_FLIGHT_POWER_CANCEL, l0_core_id(),
                            POWER_CANCEL_SYNC, new_epoch, atomic_load(&s->power_closing));
    } else {
        v71r7_flight_record(FX_Z8_FLIGHT_POWER_CANCEL, l0_core_id(),
                            POWER_CANCEL_SYNC, new_epoch, 1U);
    }
}

/* Hooks #5 & #6: 0x40D67328 (POWER_CANCEL_SYNC=11) & 0x40D67340 (POWER_OFF_SYNC=10). */
void v71r7_power_sync_hook_c(uint32_t command, uint64_t payload)
{
    if (command == POWER_OFF_SYNC) {
        v71r7_layer0_power_off_sync();
    }
    l0_stock_power_sync(command, payload);
    if (command == POWER_CANCEL_SYNC) {
        v71r7_layer0_power_cancel_sync();
    }
}

void v71r7_layer0_lifecycle_stop(void)
{
    fx_z8_lut_io_close(v71r7_io_gate(), 1); /* Sets FX_Z8_LUT_IO_STOPPED bit 31 permanently */
    Layer0Ctx *s = layer0_ctx_get();
    if (!s) {
        return;
    }
    atomic_store(&s->stop_lifecycle, 1U);
    atomic_store(&s->metrics.online, 0U);
    atomic_store(&s->metrics.enabled, 0U);
    atomic_store(&s->metrics.display_ready, 0U);
    if (claim(&s->busy, 0U, 1U)) {
        (void)retire_workspace_locked(s, 1, 0);
        atomic_store(&s->busy, 0U);
    }
}

uint32_t v71r7_layer0_worker_step(void)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (!s) {
        return V71R7_L0_STATE;
    }
    if (!claim(&s->busy, 0U, 1U)) {
        return V71R7_L0_BUSY;
    }

    uint32_t result = V71R7_L0_EMPTY;
    if (!atomic_load(&s->metrics.online) || atomic_load(&s->stop_lifecycle)) {
        if (photo_workspace_is_allocated(&s->pool) || s->view.cpu) {
            result = retire_workspace_locked(s, 1, 0);
        } else {
            result = V71R7_L0_STATE;
        }
        goto done;
    }

    if (anchor_pool_suspended(s) || atomic_load(&s->power_closing) ||
        fx_z8_lut_io_epoch(v71r7_io_gate()) == 0U) {
        atomic_store(&s->metrics.display_ready, 0U);
        atomic_store(&s->metrics.enabled, 0U);
        if (photo_workspace_is_allocated(&s->pool) || s->view.cpu) {
            result = retire_workspace_locked(s, 0, 0);
        } else {
            result = V71R7_L0_EMPTY;
        }
        goto done;
    }

    /* P0 Crash Fix #2: During 45MP still capture, immediately bypass Layer 0 and
     * retire photo_workspace when banks drain so stock PhotoDevelop (0x40eac56c)
     * gets 21.84 MiB (2u) of Class 0x24 Selector 0 headroom back! */
    if (is_still_capturing(s)) {
        COUNT(s, jpeg_pauses);
        atomic_store(&s->metrics.display_ready, 0U);
        atomic_store(&s->metrics.enabled, 0U);
        if (photo_workspace_is_allocated(&s->pool) || s->view.cpu) {
            result = retire_workspace_locked(s, 0, 0);
            if (result == V71R7_L0_OK) {
                result = V71R7_L0_EMPTY;
            }
        } else {
            result = V71R7_L0_EMPTY;
        }
        goto done;
    }

    uint32_t lut_gen = 0U;
    int lut_on = query_lut_active(s, &lut_gen);
    V71r7WysiwygOps wops;
    int has_wops = snapshot_wysiwyg_ops(s, &wops);
    V71r7WysiwygState wstate;
    int wysiwyg_on = query_wysiwyg_state(s, &wops, has_wops, &wstate);

    if (wysiwyg_on) {
        anchor_flag_set(V71R7_ANCHOR_FLAG_WYSIWYG_ACTIVE);
    } else {
        anchor_flag_clear(V71R7_ANCHOR_FLAG_WYSIWYG_ACTIVE);
    }

    /* Strict Dormant Bypass: when LUT=Off & HDF=Off & Grain=Off, hold 0 B Class 0x24! */
    if (!lut_on && !wysiwyg_on) {
        atomic_store(&s->metrics.enabled, 0U);
        atomic_store(&s->metrics.display_ready, 0U);
        if (photo_workspace_is_allocated(&s->pool) || s->view.cpu) {
            result = retire_workspace_locked(s, 0, 0);
        } else {
            result = V71R7_L0_EMPTY;
        }
        goto done;
    }

    v71r7_heartbeat_enter(FX_Z8_HB_REAR_STEP, FX_Z8_STAGE_WORKER_STEP, 0U, atomic_load(&s->ticket));
    result = make_frame(s, lut_on, &wops, has_wops, &wstate);
    v71r7_heartbeat_idle(FX_Z8_HB_REAR_STEP);

done:
    atomic_store(&s->metrics.last_result,
                 (result == REAR_STREAMING) ? (int32_t)V71R7_L0_OK : (int32_t)result);
    if (result != V71R7_L0_OK && result != REAR_STREAMING && result != V71R7_L0_BUSY &&
        result != V71R7_L0_EMPTY && result != V71R7_L0_STALE) {
        COUNT(s, errors);
    }
    atomic_store(&s->busy, 0U);
    return result;
}

void v71r7_layer0_get_metrics(FxZ8RearFlightMetrics *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    const Layer0Ctx *s = layer0_ctx_get();
    if (!s) {
        out->active_route = UINT32_MAX;
        return;
    }
    out->active_route = atomic_load_explicit(&s->metrics.active_route, memory_order_relaxed);
    out->grade_us = atomic_load_explicit(&s->metrics.grade_us, memory_order_relaxed);
    out->worker_cores = atomic_load_explicit(&s->metrics.worker_cores, memory_order_relaxed);
    out->neon_active = atomic_load_explicit(&s->metrics.neon_active, memory_order_relaxed);
    out->capture_aborts = atomic_load_explicit(&s->metrics.capture_aborts, memory_order_relaxed);
    out->timeouts = atomic_load_explicit(&s->metrics.grade_timeouts, memory_order_relaxed);
}

void v71r7_layer0_gui_poll(void)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (!s) {
        return;
    }
    if ((observe_route(s) & ROUTE_MASK) == NO_ROUTE) {
        atomic_store(&s->layout_ticket, NO_ROUTE);
        atomic_store(&s->metrics.mode_ok, 0U);
        atomic_store(&s->metrics.active_route, UINT32_MAX);
    }
}

uint32_t v71r7_layer0_is_presented(void)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (!s) {
        return 0U;
    }
    uint32_t ticket = observe_route(s);
    unsigned index = ticket & ROUTE_MASK;
    if (index >= NO_ROUTE || !armed(s) || atomic_load(&s->layout_ticket) != ticket) {
        return 0U;
    }
    int32_t current = atomic_load(hw_current(s, index));
    return (current >= 0 && atomic_load(&s->bank[current].ticket) == ticket &&
            atomic_load(&s->metrics.last_presented_generation) != 0U) ? 1U : 0U;
}

uint32_t v71r7_layer0_set_display_mode(uint32_t mode)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (!s || mode > 1U) {
        return V71R7_L0_ARGUMENT;
    }
    if (mode && atomic_load(&s->metrics.quarantined)) {
        return V71R7_L0_STATE;
    }
    atomic_store(&s->metrics.display_mode, mode);
    return V71R7_L0_OK;
}

uint32_t v71r7_layer0_set_copy_mode(uint32_t mode)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (!s || mode > 2U) {
        return V71R7_L0_ARGUMENT;
    }
    if (!claim(&s->busy, 0U, 1U)) {
        return V71R7_L0_BUSY;
    }
    if (atomic_exchange(&s->metrics.copy_mode, mode) != mode) {
        atomic_store(&s->evidence.ready, 0U);
    }
    atomic_store(&s->busy, 0U);
    return V71R7_L0_OK;
}

uint32_t v71r7_layer0_set_source_lag(uint32_t lag)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (!s || lag > 2U) {
        return V71R7_L0_ARGUMENT;
    }
    if (!claim(&s->busy, 0U, 1U)) {
        return V71R7_L0_BUSY;
    }
    if (atomic_exchange(&s->metrics.source_lag, lag) != lag) {
        atomic_store(&s->evidence.ready, 0U);
    }
    atomic_store(&s->busy, 0U);
    return V71R7_L0_OK;
}

int v71r7_layer0_workspace_allocated(void)
{
    const Layer0Ctx *s = layer0_ctx_get();
    return s ? photo_workspace_is_allocated(&s->pool) : 0;
}

uint32_t *v71r7_layer0_cache_acquire(uint32_t *gen_out)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (!s || !gen_out || anchor_pool_suspended(s) ||
        atomic_load_explicit(&s->power_closing, memory_order_acquire) ||
        atomic_load_explicit(&s->stop_lifecycle, memory_order_acquire) ||
        atomic_load_explicit(&s->cache_invalidate_req, memory_order_acquire)) {
        return NULL;
    }
    uint32_t active_gen = 0U;
    if (!v71r7_lut_is_active(&active_gen, NULL) || active_gen == 0U) {
        return NULL;
    }
    if (atomic_load_explicit(&s->cache_ready_gen, memory_order_acquire) != active_gen) {
        return NULL;
    }
    PhotoWorkspaceView v;
    if (photo_workspace_pin(&s->pool, &v) != PHOTO_WS_OK) {
        return NULL;
    }
    atomic_fetch_add_explicit(&s->cache_readers, 1U, memory_order_seq_cst);
    if (anchor_pool_suspended(s) ||
        atomic_load_explicit(&s->power_closing, memory_order_seq_cst) ||
        atomic_load_explicit(&s->stop_lifecycle, memory_order_seq_cst) ||
        atomic_load_explicit(&s->cache_invalidate_req, memory_order_seq_cst) ||
        atomic_load_explicit(&s->cache_ready_gen, memory_order_seq_cst) != active_gen) {
        atomic_fetch_sub_explicit(&s->cache_readers, 1U, memory_order_release);
        (void)photo_workspace_unpin(&s->pool);
        return NULL;
    }
    *gen_out = active_gen;
    return (uint32_t *)(void *)(v.cpu + CACHE_OFFSET);
}

uint32_t *v71r7_layer0_baked_acquire(uint32_t *gen_out)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (!s || !gen_out || anchor_pool_suspended(s) ||
        atomic_load_explicit(&s->power_closing, memory_order_acquire) ||
        atomic_load_explicit(&s->stop_lifecycle, memory_order_acquire) ||
        atomic_load_explicit(&s->cache_invalidate_req, memory_order_acquire)) {
        return NULL;
    }
    uint32_t active_gen = 0U;
    if (!v71r7_lut_is_active(&active_gen, NULL) || active_gen == 0U) {
        return NULL;
    }
    if (atomic_load_explicit(&s->baked_ready_gen, memory_order_acquire) != active_gen) {
        return NULL;
    }
    PhotoWorkspaceView v;
    if (photo_workspace_pin(&s->pool, &v) != PHOTO_WS_OK) {
        return NULL;
    }
    atomic_fetch_add_explicit(&s->cache_readers, 1U, memory_order_seq_cst);
    if (anchor_pool_suspended(s) ||
        atomic_load_explicit(&s->power_closing, memory_order_seq_cst) ||
        atomic_load_explicit(&s->stop_lifecycle, memory_order_seq_cst) ||
        atomic_load_explicit(&s->cache_invalidate_req, memory_order_seq_cst) ||
        atomic_load_explicit(&s->baked_ready_gen, memory_order_seq_cst) != active_gen) {
        atomic_fetch_sub_explicit(&s->cache_readers, 1U, memory_order_release);
        (void)photo_workspace_unpin(&s->pool);
        return NULL;
    }
    *gen_out = active_gen;
    return (uint32_t *)(void *)(v.cpu + BAKED_LUT_OFFSET);
}

void v71r7_layer0_cache_release(void)
{
    Layer0Ctx *s = layer0_ctx_get();
    if (!s) {
        return;
    }
    atomic_fetch_sub_explicit(&s->cache_readers, 1U, memory_order_release);
    (void)photo_workspace_unpin(&s->pool);
}

uint32_t v71r7_layer0_last_stages_applied(void)
{
    const Layer0Ctx *s = layer0_ctx_get();
    return s ? atomic_load_explicit(&s->last_stages_applied, memory_order_acquire) : 0U;
}

const Layer0Metrics *v71r7_layer0_metrics_ptr(void)
{
    const Layer0Ctx *s = layer0_ctx_get();
    return s ? &s->metrics : NULL;
}

const RearPowerMetrics *v71r7_layer0_power_metrics_ptr(void)
{
    const Layer0Ctx *s = layer0_ctx_get();
    return s ? &s->power : NULL;
}

const Layer0Evidence *v71r7_layer0_evidence_ptr(void)
{
    const Layer0Ctx *s = layer0_ctx_get();
    return s ? &s->evidence : NULL;
}

void v71r7_layer0_lock_stats(uint32_t *waits_out, uint32_t *wait_ok_out)
{
    const Layer0Ctx *s = layer0_ctx_get();
    if (waits_out) {
        *waits_out = s ? atomic_load_explicit(&s->pool_lock_waits, memory_order_relaxed) : 0U;
    }
    if (wait_ok_out) {
        *wait_ok_out = s ? atomic_load_explicit(&s->pool_lock_wait_ok, memory_order_relaxed) : 0U;
    }
}

#ifdef V71R7_HOST_TEST
/* Host test direct wrappers (on AArch64 EL1, lut_hooks.S defines these entry points). */
uint32_t v71r7_layer0_enqueue_hook(uint32_t route, const void *pkt, const void *rect,
                                   const void *stamp, void *cb)
{
    return v71r7_layer0_enqueue_hook_c(route, pkt, rect, stamp, cb);
}

uint32_t v71r7_layer0_display_hook(uint32_t device, void *layer_desc)
{
    return v71r7_layer0_display_hook_c(device, layer_desc);
}

void v71r7_layer0_vsync_hook(uint32_t route)
{
    v71r7_layer0_vsync_hook_c(route);
}

void v71r7_power_sync_hook(uint32_t command, uint64_t payload)
{
    v71r7_power_sync_hook_c(command, payload);
}
#endif
