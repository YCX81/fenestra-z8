/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Display Layer 0 WYSIWYG Preview, photo_workspace 512MiB Spacer,
 * HangFix V69 pool_suspend/resume, and POWER_OFF_SYNC/POWER_CANCEL_SYNC
 * Mac Host ASan+UBSan Verification Suite.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lut_core/v71r7_lut_abi.h"
#include "photo_workspace.h"
#include "layer0_provider.h"

/* Simulated 16 KiB DRAM Anchor + Flight Recorder region (0xA11640000..0xA11644000). */
uint8_t v71r7_host_dram_region[0x4000];

/* ============================================================================
 * Mock Infrastructure (Class 0x24, Display HW, Pool 0, Flight, LUT, WYSIWYG)
 * ============================================================================ */

#define MOCK_MAX_WS_ALLOCS 16
#define MOCK_FLIGHT_CAP    64

typedef struct {
    int       used;
    int       malformed;
    uint32_t  bytes;
    uint32_t  capacity;
    uint8_t  *backing;        /* Aligned backing buffer */
    uintptr_t base;           /* Simulated raw base (= backing + 0) */
    uint64_t  sim_dma_base;   /* Simulated Z8 DMA address in 0x840000000..0x8C0000000 */
    uintptr_t handle;         /* Unique non-zero token */
} MockWsAlloc;

typedef struct {
    uint16_t event;
    uint16_t core_flags;
    uint32_t a, b, c;
} MockFlightEntry;

typedef struct {
    /* Clock (24 MHz like Z8 C3.11) */
    uint64_t tick;
    uint64_t freq;

    /* Heap tracking */
    uint32_t heap_allocs;
    uint32_t heap_frees;

    /* Class 0x24 allocator mock */
    int          pool_valid;
    uint32_t     ws_config_mutex_depth;
    uint32_t     ws_config_lock_calls;
    int          ws_fail_config_lock;
    int          ws_invalidate_pool_on_lock;
    MockWsAlloc  ws[MOCK_MAX_WS_ALLOCS];
    uint32_t     ws_alloc_calls;
    uint32_t     ws_free_calls;
    uint32_t     ws_live_count;
    uint64_t     ws_live_bytes;
    int          ws_force_next_straddle;
    int          ws_force_next_malformed;
    uint32_t     ws_max_dcache_chunk;
    uint32_t     ws_clean_calls;
    uint32_t     ws_inv_calls;

    /* Single-pass Live->LCD reuse mock + SMP helper telemetry mock */
    int          live_reuse_enable;
    uint32_t     live_reuse_stages;
    uint32_t     live_reuse_seq;
    uint64_t     live_reuse_age_us;
    uint8_t      live_reuse_fill_y;
    uint8_t      live_reuse_fill_uv;
    uint32_t     live_reuse_calls;
    uint32_t     smp_online_helpers;
    uint32_t     smp_sentinels[3];

    /* Display & UI HW registers */
    uint32_t panel_bank;          /* 0=LCD, 1=EVF */
    uint64_t current_view;
    uint64_t shooting_view;
    uint32_t gui_lv_canvas;
    uint64_t gui_lv_bounds;
    uint32_t view_id;
    uint32_t view_state;
    uint32_t prop_mode_selector;  /* 0=Photo */

    uint32_t disp_status;         /* 3=running */
    uint32_t disp_state;          /* 3=running */
    uint32_t disp_layer0_enabled; /* 1=on */
    uint8_t  disp_shadow_flag;
    uint8_t  disp_update_enabled; /* 1=on */
    uint8_t  disp_crop_origin[16];
    uint64_t disp_cache_desc_y;
    uint64_t disp_cache_desc_uv;
    uint64_t disp_programmed;

    /* Pool 0 timed lock / source ownership & DS 0x2126 GUI release mode */
    int      pool_busy;
    int      pool_busy_only_on_poll;
    int      pool_owned;
    uint32_t pool_lock_depth;
    uint32_t pool_last_tmout_ms;
    uint32_t gui_release_mode; /* DS 0x2126: 0=S, 1=CL, 2=CH, 3=C30, 4=C120, 5=C60, 6=C15 */

    /* Stock hook invocation telemetry */
    uint32_t stock_enqueue_calls;
    uint32_t stock_display_calls;
    uint64_t stock_display_seen_y;
    uint64_t stock_display_seen_uv;
    uint32_t stock_display_rc;
    uint32_t stock_vsync_calls;
    uint32_t stock_power_sync_calls;
    uint32_t stock_power_last_cmd;
    uint64_t stock_power_last_payload;
    int      stock_power_saw_gate_closed;
    uint32_t stock_power_saw_active_banks;
    int      stock_power_saw_ws_allocated;

    /* Sleep hook for simulating concurrent VSync / I/O drain during suspend & power_off */
    void (*on_sleep)(uint32_t ms, uint32_t sleep_idx);
    uint32_t sleep_calls;

    /* Active LUT mock (v71r7_lut_core) */
    int      lut_active;
    uint32_t lut_gen;
    int      neon_allowed;
    uint32_t lut_apply_calls;
    int      lut_last_use_smp;
    uint32_t stage_order_log[16];
    uint32_t stage_order_len;

    /* Mid-copy corruption test */
    uint8_t *corrupt_after_copy_ptr;

    /* Flight recorder log */
    MockFlightEntry flight[MOCK_FLIGHT_CAP];
    uint32_t        flight_count;
} MockEnv;

static MockEnv g_env;

/* Helper: aligned allocation for buffers */
static uint8_t *alloc_aligned_512(size_t bytes)
{
    void *p = NULL;
    int rc = posix_memalign(&p, 512, bytes);
    assert(rc == 0 && p != NULL);
    memset(p, 0, bytes);
    return (uint8_t *)p;
}

static void env_set_route_lcd(void)
{
    g_env.panel_bank = 0U;
    g_env.gui_lv_canvas = 1024U | (682U << 16);
    g_env.gui_lv_bounds = ((uint64_t)1024U << 32) | ((uint64_t)682U << 48);
}

static void env_set_route_evf(void)
{
    g_env.panel_bank = 1U;
    g_env.gui_lv_canvas = 1280U | (960U << 16);
    g_env.gui_lv_bounds = ((uint64_t)54U << 16) | ((uint64_t)1280U << 32) |
                          ((uint64_t)(54U + 852U) << 48);
}

static void env_advance_ms(uint64_t ms)
{
    g_env.tick += ms * (g_env.freq / 1000U);
}

static void env_reset(void)
{
    for (int i = 0; i < MOCK_MAX_WS_ALLOCS; ++i) {
        if (g_env.ws[i].used && g_env.ws[i].backing) {
            free(g_env.ws[i].backing);
        }
    }
    /* Free any heap Layer0Ctx from previous test if still anchored */
    V71r7DramAnchor *anc = v71r7_anchor();
    if (anc->magic == V71R7_ANCHOR_MAGIC && anc->layer0_ctx) {
        free(anc->layer0_ctx);
        anc->layer0_ctx = NULL;
    }
    memset(v71r7_host_dram_region, 0, sizeof(v71r7_host_dram_region));
    memset(&g_env, 0, sizeof(g_env));

    g_env.freq = 24000000ULL;
    g_env.tick = 1000ULL * 24000ULL; /* Start at 1000 ms */
    g_env.pool_valid = 1;
    g_env.shooting_view = 0xA0282DAB8ULL;
    g_env.current_view = g_env.shooting_view;
    g_env.view_id = 3U;
    g_env.view_state = 1U;
    g_env.prop_mode_selector = 0U;
    g_env.disp_status = 3U;
    g_env.disp_state = 3U;
    g_env.disp_layer0_enabled = 1U;
    g_env.disp_update_enabled = 1U;
    g_env.pool_owned = 1;
    g_env.stock_display_rc = 1U;
    g_env.neon_allowed = 1;
    env_set_route_lcd();
}

static uint32_t flight_count_event(uint16_t ev)
{
    uint32_t n = 0U;
    for (uint32_t i = 0U; i < g_env.flight_count && i < MOCK_FLIGHT_CAP; ++i) {
        if (g_env.flight[i].event == ev) {
            ++n;
        }
    }
    return n;
}

static int flight_has_event(uint16_t ev)
{
    return flight_count_event(ev) != 0U;
}

/* ============================================================================
 * Mock Implementations of lut_core ABI + Host Hooks
 * ============================================================================ */

void v71r7_flight_record(uint16_t event, uint16_t core_flags, uint32_t a, uint32_t b, uint32_t c)
{
    if (g_env.flight_count < MOCK_FLIGHT_CAP) {
        g_env.flight[g_env.flight_count++] = (MockFlightEntry){event, core_flags, a, b, c};
    }
}
void v71r7_heartbeat_enter(unsigned slot, unsigned stage, uint32_t cap_num, uint32_t aux)
{
    (void)slot; (void)stage; (void)cap_num; (void)aux;
}
void v71r7_heartbeat_progress(unsigned slot) { (void)slot; }
void v71r7_heartbeat_idle(unsigned slot) { (void)slot; }

int v71r7_neon_sentinel_is_allowed(void) { return g_env.neon_allowed; }

int v71r7_lut_is_active(uint32_t *gen_out, int *is_1d_separable_out)
{
    if (gen_out) *gen_out = g_env.lut_active ? g_env.lut_gen : 0U;
    if (is_1d_separable_out) *is_1d_separable_out = 0;
    return g_env.lut_active;
}

/* Mock Stage 0 (.cube 3D LUT): non-commutative transform Y = clamp((Y + 10) * 2), UV ^= 0x11 */
int v71r7_lut_apply_nv12(uint8_t *y, uint8_t *uv, uint32_t width, uint32_t height,
                         uint32_t y_stride, uint32_t uv_stride,
                         uint32_t *cache_words_4mib, uint32_t *cache_gen_inout,
                         int use_smp)
{
    assert(y != NULL && uv != NULL);
    assert(cache_words_4mib != NULL && cache_gen_inout != NULL);
    if (!g_env.lut_active) {
        return 0;
    }
    *cache_gen_inout = g_env.lut_gen;
    cache_words_4mib[0] = g_env.lut_gen;
    ++g_env.lut_apply_calls;
    g_env.lut_last_use_smp = use_smp;
    if (g_env.stage_order_len < 16U) {
        g_env.stage_order_log[g_env.stage_order_len++] = V71R7_L0_STAGE_LUT;
    }

    for (uint32_t r = 0U; r < height; ++r) {
        uint8_t *row = y + (size_t)r * y_stride;
        for (uint32_t c = 0U; c < width; ++c) {
            uint32_t v = ((uint32_t)row[c] + 10U) * 2U;
            row[c] = (uint8_t)(v > 255U ? 255U : v);
        }
    }
    for (uint32_t r = 0U; r < height / 2U; ++r) {
        uint8_t *row = uv + (size_t)r * uv_stride;
        for (uint32_t c = 0U; c < width; ++c) {
            row[c] ^= 0x11U;
        }
    }
    return 1;
}

/* --- PhotoWorkspace Host Mocks --- */

int v71r7_host_ws_config_lock(void)
{
    ++g_env.ws_config_lock_calls;
    if (g_env.ws_fail_config_lock) {
        return 0;
    }
    assert(g_env.ws_config_mutex_depth == 0U && "recursive CONFIG_MUTEX lock!");
    g_env.ws_config_mutex_depth = 1U;
    if (g_env.ws_invalidate_pool_on_lock) {
        g_env.ws_invalidate_pool_on_lock = 0;
        g_env.pool_valid = 0;
    }
    return 1;
}

void v71r7_host_ws_config_unlock(void)
{
    assert(g_env.ws_config_mutex_depth == 1U && "unbalanced CONFIG_MUTEX unlock!");
    g_env.ws_config_mutex_depth = 0U;
}

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
    (void)lut_gen;
    (void)hdf_level;
    (void)max_age_us;
    ++g_env.live_reuse_calls;
    if (out_stages_applied) *out_stages_applied = 0U;
    if (out_seq) *out_seq = 0U;
    if (out_age_us) *out_age_us = 0U;
    if (!g_env.live_reuse_enable) {
        return 0;
    }
    uint32_t cur_seq = g_env.live_reuse_seq ? g_env.live_reuse_seq : 1U;
    if (last_seq != 0U && cur_seq == last_seq) {
        return -1;
    }
    assert(w == 1024U && (h == 682U || h == 680U));
    assert(dst_y != NULL && dst_uv != NULL && dst_stride >= w);
    for (uint32_t r = 0U; r < h; ++r) {
        memset(dst_y + (size_t)r * dst_stride, g_env.live_reuse_fill_y, w);
    }
    for (uint32_t r = 0U; r < h / 2U; ++r) {
        memset(dst_uv + (size_t)r * dst_stride, g_env.live_reuse_fill_uv, w);
    }
    if (out_stages_applied) {
        *out_stages_applied = g_env.live_reuse_stages;
    }
    if (out_seq) {
        *out_seq = cur_seq;
    }
    if (out_age_us) {
        *out_age_us = g_env.live_reuse_age_us;
    }
    return 1;
}

uint32_t v71r7_smp_online_helpers(void)
{
    return g_env.smp_online_helpers;
}

void v71r7_smp_get_helper_sentinels(uint32_t out[3])
{
    if (out) {
        out[0] = g_env.smp_sentinels[0];
        out[1] = g_env.smp_sentinels[1];
        out[2] = g_env.smp_sentinels[2];
    }
}

int v71r7_host_ws_pool_valid(void) { return g_env.pool_valid; }

uintptr_t v71r7_host_ws_alloc(uint32_t a, uint32_t b, uint32_t bytes)
{
    assert(a == 0U && b == 0x6bU);
    assert(g_env.ws_config_mutex_depth == 1U && "WS_ALLOC called without CONFIG_MUTEX!");
    ++g_env.ws_alloc_calls;
    for (int i = 0; i < MOCK_MAX_WS_ALLOCS; ++i) {
        if (!g_env.ws[i].used) {
            MockWsAlloc *m = &g_env.ws[i];
            memset(m, 0, sizeof(*m));
            m->used = 1;
            m->bytes = bytes;
            m->capacity = (bytes + 4095U) & ~4095U;
            m->handle = (uintptr_t)(0xA10000000ULL + (uint64_t)(i + 1) * 0x1000U);
            if (g_env.ws_force_next_malformed) {
                g_env.ws_force_next_malformed = 0;
                m->malformed = 1;
                return m->handle;
            }
            m->backing = alloc_aligned_512((size_t)m->capacity + 1024U);
            m->base = (uintptr_t)m->backing;
            if (g_env.ws_force_next_straddle) {
                g_env.ws_force_next_straddle = 0;
                /* Straddles 512 MiB boundary 0x860000000! */
                m->sim_dma_base = 0x85F800000ULL;
            } else {
                /* Cleanly inside single 512 MiB window 0x860000000..0x87FFFFFFF */
                m->sim_dma_base = 0x860100000ULL + (uint64_t)i * 0x01000000ULL;
            }
            ++g_env.ws_live_count;
            g_env.ws_live_bytes += m->capacity;
            return m->handle;
        }
    }
    return 0U;
}

uint32_t v71r7_host_ws_free(uintptr_t handle)
{
    assert(g_env.ws_config_mutex_depth == 1U && "WS_FREE called without CONFIG_MUTEX!");
    ++g_env.ws_free_calls;
    for (int i = 0; i < MOCK_MAX_WS_ALLOCS; ++i) {
        if (g_env.ws[i].used && g_env.ws[i].handle == handle) {
            if (!g_env.ws[i].malformed) {
                free(g_env.ws[i].backing);
                assert(g_env.ws_live_count > 0U);
                --g_env.ws_live_count;
                g_env.ws_live_bytes -= g_env.ws[i].capacity;
            }
            memset(&g_env.ws[i], 0, sizeof(g_env.ws[i]));
            return 1U;
        }
    }
    return 0U;
}

int v71r7_host_ws_inspect_handle(uintptr_t handle, uint32_t *cls_out,
                                 uintptr_t *owner_out, uintptr_t *owner_back_out,
                                 uintptr_t *base_out, uint32_t *capacity_out)
{
    for (int i = 0; i < MOCK_MAX_WS_ALLOCS; ++i) {
        if (g_env.ws[i].used && g_env.ws[i].handle == handle) {
            if (g_env.ws[i].malformed) {
                *cls_out = 0x99U; /* Invalid class */
                *owner_out = 0U;
                *owner_back_out = 0U;
                return 0;
            }
            *cls_out = 0x24U;
            *owner_out = handle + 0x40U;
            *owner_back_out = handle;
            *base_out = (uintptr_t)g_env.ws[i].sim_dma_base;
            *capacity_out = g_env.ws[i].capacity;
            return 1;
        }
    }
    return 0;
}

/* Host memory uses direct addresses (raw == cpu in user space; bit0 never used).
 * To distinguish raw vs cpu cleanly without changing real pointers, use a tagged
 * mock map OR simply verify exact round-trip via an offset table. Since host memory
 * is coherent and ASan validates real pointers, encode "raw" = addr and "cpu" = addr
 * in host tests while tracking cacheability by region helper. Wait: layer0_provider
 * checks !CACHEABLE(raw) && CACHEABLE(cpu)! So let's keep a 1-bit high-bit tag or
 * simply have raw == real_ptr (for u64 reads/writes!) and cpu == real_ptr (also
 * directly dereferenceable!), EXCEPT Wait: if raw == cpu, !CACHEABLE(raw) && CACHEABLE(cpu)
 * can't both be true for the SAME address!
 * Solution: keep a small host alias flag in to_cpu/to_raw? No — cpu MUST be a valid
 * dereferenceable pointer under ASan AND raw is dereferenced in spot_matches_row_range!
 * Look how easy it is: `ws_to_cpu` returns `raw` (valid ASan pointer!), and in
 * `v71r7_host_ws_cacheable`: return 0 on odd call / 1 on even call? NO, never flaky!
 * Even cleaner: allocate raw & cpu from a real `mmap` mirror OR check in
 * `v71r7_host_ws_cacheable`? Wait: look at `photo_workspace.c` and `layer0_provider.c`
 * under `V71R7_HOST_TEST`! In `photo_workspace.c`:
 *   a = raw - 64; b = raw + n + 63; c = WS_TO_CPU(a); d = WS_TO_CPU(b);
 *   !WS_CACHEABLE(a) && !WS_CACHEABLE(b) && WS_CACHEABLE(c) && WS_CACHEABLE(d)
 * Notice on Apple Silicon / macOS 64-bit, `mmap` can map TWO virtual addresses
 * pointing to the exact same physical memory OR even simpler: in `photo_workspace.c`,
 * `WS_TO_CPU` is ONLY dereferenced after `ws->cpu = ws_raw_to_cpu_addr(chosen.raw)`!
 * Wait: on macOS arm64, pointers have upper tag byte (TBI) or `vm_remap`! Wait: ASan
 * hates tagged pointers! So let's use TWO real equal-sized buffers OR keep `raw` as a
 * real buffer and `cpu` as the same buffer, and check how `ws_mapped` & `l0_mapped_plane`
 * check `WS_CACHEABLE`: wait — `WS_TO_CPU` is NEVER dereferenced inside `ws_mapped`!
 * Look! In `ws_mapped(raw, n)`: `a, b` are raw; `c, d` are `WS_TO_CPU(a), WS_TO_CPU(b)` —
 * `a, b, c, d` are NOT dereferenced in `ws_mapped`!
 * Where ARE `raw` and `cpu` dereferenced?
 * - `ws->cpu` is dereferenced everywhere (`memset`, `IMAGE_COPY`, `v71r7_lut_apply_nv12`, HDF, Grain).
 * - `bank->raw_y` is stored in `layer_desc+0x110` (only read by mock `stock_display`!).
 * - `source->y` (`raw`) is dereferenced in `spot_matches_row_range(raw, dst, ...)`, AND
 *   `l0_source_cached_view(raw)` is dereferenced in `IMAGE_COPY(dst, read_addr, ...)`!
 * THAT MEANS:
 * - For `photo_workspace`: `ws->cpu` is the real ASan pointer; `bank->raw_y` can be
 *   `sim_dma_base + offset` (a true Z8-style `0x860xxxxxx` address!) and `TO_RAW(cpu)`
 *   translates real `cpu` -> `sim_dma_base + (cpu - backing)` and `TO_CPU(raw)` translates
 *   `sim_dma_base + off` -> `backing + off`!!
 * OH!! THAT IS GENIUS!!
 * Let's verify that!
 * - Every Class `0x24` `photo_workspace` allocation has:
 *     - Real ASan heap buffer `backing` (used as `cpu`!) -> `WS_CACHEABLE(cpu) == 1`!
 *     - Simulated uncached DMA address `sim_dma_base` (`0x85F800000` or `0x860100000`) -> `WS_CACHEABLE(raw) == 0`!
 *     - `WS_TO_CPU(sim_dma_base + off)` returns `backing + off` (valid ASan pointer)!
 *     - `WS_TO_RAW(backing + off)` returns `sim_dma_base + off` (true Z8 uncached address)!
 *     - `v71r7_host_ws_verify_line(raw)` translates `raw -> backing + off` to test the real padding bytes!
 *     - `bank->raw_y` is a real Z8 `0x860xxxxxx` DMA address (so `layer_desc+0x110` has a real DMA address AND mock `stock_display` translates `bank->raw_y -> cpu` via `WS_TO_CPU` to inspect the actual graded pixels!)
 *     - And for source NV12 frames (in `enqueue_hook` / `copy_band`): `source->y` is a real ASan buffer pointer (directly dereferenced by `spot_matches_row_range`), and `l0_source_cached_view(source->y)` returns `source->y` directly (because in `copy_band` we don't call `l0_mapped_plane` on `source->y` — `l0_source_planes` calls `v71r7_host_source_planes_valid`)!
 */

static int ws_find_by_raw(uintptr_t raw, size_t *off_out)
{
    for (int i = 0; i < MOCK_MAX_WS_ALLOCS; ++i) {
        if (!g_env.ws[i].used || g_env.ws[i].malformed) continue;
        uint64_t lo = g_env.ws[i].sim_dma_base;
        uint64_t hi = lo + g_env.ws[i].capacity + 1024U;
        if ((uint64_t)raw >= lo && (uint64_t)raw < hi) {
            if (off_out) *off_out = (size_t)((uint64_t)raw - lo);
            return i;
        }
    }
    return -1;
}

static int ws_find_by_cpu(uintptr_t cpu, size_t *off_out)
{
    for (int i = 0; i < MOCK_MAX_WS_ALLOCS; ++i) {
        if (!g_env.ws[i].used || g_env.ws[i].malformed) continue;
        uintptr_t lo = (uintptr_t)g_env.ws[i].backing;
        uintptr_t hi = lo + g_env.ws[i].capacity + 1024U;
        if (cpu >= lo && cpu < hi) {
            if (off_out) *off_out = (size_t)(cpu - lo);
            return i;
        }
    }
    return -1;
}

uintptr_t v71r7_host_ws_to_cpu(uintptr_t raw)
{
    size_t off = 0U;
    int idx = ws_find_by_raw(raw, &off);
    if (idx >= 0) {
        return (uintptr_t)(g_env.ws[idx].backing + off);
    }
    return raw; /* Source NV12 buffers in host memory */
}

uintptr_t v71r7_host_ws_to_raw(uintptr_t cpu)
{
    size_t off = 0U;
    int idx = ws_find_by_cpu(cpu, &off);
    if (idx >= 0) {
        return (uintptr_t)(g_env.ws[idx].sim_dma_base + off);
    }
    return cpu;
}

uint32_t v71r7_host_ws_cacheable(uintptr_t addr)
{
    if (ws_find_by_raw(addr, NULL) >= 0) return 0U;
    if (ws_find_by_cpu(addr, NULL) >= 0) return 1U;
    return 1U;
}

int v71r7_host_ws_one_region(uintptr_t start, uintptr_t end, uint32_t kind)
{
    if (kind == 1U) {
        int i1 = ws_find_by_raw(start, NULL);
        int i2 = ws_find_by_raw(end, NULL);
        return i1 >= 0 && i1 == i2;
    }
    int i1 = ws_find_by_cpu(start, NULL);
    int i2 = ws_find_by_cpu(end, NULL);
    return i1 >= 0 && i1 == i2;
}

int v71r7_host_ws_in_class24(uintptr_t base, uint32_t capacity)
{
    return capacity != 0U && capacity <= 0x20000000U &&
           (uint64_t)base >= 0x840000000ULL &&
           (uint64_t)base <= 0x8C0000000ULL - capacity;
}

uint64_t v71r7_host_ws_dma_window_addr(uintptr_t raw)
{
    return (uint64_t)raw;
}

void v71r7_host_ws_clean(uintptr_t addr, uint32_t bytes, uint32_t mode)
{
    (void)addr; (void)mode;
    ++g_env.ws_clean_calls;
    if (bytes > g_env.ws_max_dcache_chunk) {
        g_env.ws_max_dcache_chunk = bytes;
    }
    /* Strictly enforce <= 512 KiB chunking invariant! */
    assert(bytes <= PHOTO_WORKSPACE_DCACHE_CHUNK);
}

void v71r7_host_ws_invalidate(uintptr_t addr, uint32_t bytes, uint32_t mode)
{
    (void)addr; (void)mode;
    ++g_env.ws_inv_calls;
    if (bytes > g_env.ws_max_dcache_chunk) {
        g_env.ws_max_dcache_chunk = bytes;
    }
    assert(bytes <= PHOTO_WORKSPACE_DCACHE_CHUNK);
}

int v71r7_host_ws_verify_line(uintptr_t raw)
{
    size_t off = 0U;
    int idx = ws_find_by_raw(raw, &off);
    if (idx < 0) return 0;
    uint64_t *p = (uint64_t *)(void *)(g_env.ws[idx].backing + off);
    uint64_t orig = *p;
    *p = orig ^ UINT64_C(0x75612cc68e2975b1);
    *p = orig;
    return 1;
}

/* Fix up inspect_handle to report sim_dma_base as base_out (done via wrapper below). */
/* Wait: inspect_handle was already defined above — let's make sure base_out uses sim_dma_base! */
/* Let's check: in v71r7_host_ws_inspect_handle above, it set *base_out = g_env.ws[i].base;
 * Let's update base inside alloc to equal sim_dma_base? Wait: m->base is only used there!
 * Better yet, just use ws_inspect_dma_fixup directly by setting m->base = m->sim_dma_base
 * in v71r7_host_ws_alloc? Look: let's fix v71r7_host_ws_inspect_handle to call ws_inspect_dma_fixup! */

/* --- Layer 0 Provider Host Mocks --- */

void *v71r7_host_heap_alloc(uint32_t bytes, uint32_t flags)
{
    (void)flags;
    ++g_env.heap_allocs;
    return calloc(1U, bytes);
}

void v71r7_host_heap_free(void *ptr)
{
    ++g_env.heap_frees;
    free(ptr);
}

uint64_t v71r7_host_ticks(void) { return g_env.tick; }
uint64_t v71r7_host_freq(void)  { return g_env.freq; }

int32_t v71r7_host_sleep(uint32_t ms)
{
    ++g_env.sleep_calls;
    env_advance_ms(ms);
    if (g_env.on_sleep) {
        g_env.on_sleep(ms, g_env.sleep_calls);
    }
    return 0;
}

void *v71r7_host_image_copy(void *dst, const void *src, size_t bytes)
{
    void *r = memcpy(dst, src, bytes);
    if (g_env.corrupt_after_copy_ptr &&
        (uintptr_t)g_env.corrupt_after_copy_ptr >= (uintptr_t)src &&
        (uintptr_t)g_env.corrupt_after_copy_ptr < (uintptr_t)src + bytes) {
        /* Simulate rotating ISP DMA overwriting source mid-copy! */
        *g_env.corrupt_after_copy_ptr ^= 0xFFU;
        g_env.corrupt_after_copy_ptr = NULL;
    }
    return r;
}

uint32_t  v71r7_host_panel_bank(void)          { return g_env.panel_bank; }
uint64_t  v71r7_host_current_view(void)        { return g_env.current_view; }
uint64_t  v71r7_host_shooting_view(void)       { return g_env.shooting_view; }
uint32_t  v71r7_host_gui_lv_canvas(void)       { return g_env.gui_lv_canvas; }
uint64_t  v71r7_host_gui_lv_bounds(void)       { return g_env.gui_lv_bounds; }
uint32_t  v71r7_host_view_id(uintptr_t view)   { (void)view; return g_env.view_id; }
uint32_t  v71r7_host_view_state(uintptr_t view){ (void)view; return g_env.view_state; }
uint32_t  v71r7_host_read_property(uint32_t prop, uint32_t *out)
{
    assert(prop == 0x06000003U);
    *out = g_env.prop_mode_selector;
    return 1U;
}

uint32_t  v71r7_host_disp_status(void)         { return g_env.disp_status; }
uint32_t  v71r7_host_disp_state(void)          { return g_env.disp_state; }
uint32_t  v71r7_host_disp_layer0_enabled(void) { return g_env.disp_layer0_enabled; }
uint8_t   v71r7_host_disp_shadow_flag(void)    { return g_env.disp_shadow_flag; }
uint8_t   v71r7_host_disp_update_enabled(void) { return g_env.disp_update_enabled; }
uintptr_t v71r7_host_disp_crop_origin_ptr(void){ return (uintptr_t)g_env.disp_crop_origin; }
uint64_t  v71r7_host_disp_cache_desc_y(void)   { return g_env.disp_cache_desc_y; }
uint64_t  v71r7_host_disp_cache_desc_uv(void)  { return g_env.disp_cache_desc_uv; }
uint64_t  v71r7_host_disp_programmed(void)     { return g_env.disp_programmed; }
uintptr_t v71r7_host_stock_complete_cb(void)   { return (uintptr_t)0x40E9A570ULL; }

int v71r7_host_pool_lock_timed(uint32_t tmout_ms)
{
    g_env.pool_last_tmout_ms = tmout_ms;
    if (g_env.pool_busy) {
        return 0;
    }
    if (g_env.pool_busy_only_on_poll && tmout_ms == 0U) {
        return 0;
    }
    ++g_env.pool_lock_depth;
    return 1;
}

uint32_t v71r7_host_gui_release_mode(void)
{
    return g_env.gui_release_mode;
}

void v71r7_host_pool_unlock(void)
{
    assert(g_env.pool_lock_depth > 0U);
    --g_env.pool_lock_depth;
}

int v71r7_host_pool_owns_source(uintptr_t y, uintptr_t *rec_io,
                                uintptr_t *owner_io, uint32_t *slot_io)
{
    if (!g_env.pool_owned) return 0;
    uintptr_t rec = y ^ 0x1000U;
    uintptr_t own = y ^ 0x2000U;
    if (*owner_io && (*owner_io != own || *rec_io != rec || *slot_io != 1U)) return 0;
    *rec_io = rec;
    *owner_io = own;
    *slot_io = 1U;
    return 1;
}

int v71r7_host_source_planes_valid(uintptr_t y, uintptr_t uv,
                                   uint32_t active_y_bytes, uint32_t uv_offset)
{
    (void)active_y_bytes;
    return y != 0U && !(y & 511U) && uv == y + uv_offset;
}

uint32_t v71r7_host_stock_enqueue(uint32_t route, const void *pkt, const void *rect,
                                  const void *stamp, void *cb)
{
    (void)route; (void)pkt; (void)rect; (void)stamp; (void)cb;
    ++g_env.stock_enqueue_calls;
    return 1U;
}

uint32_t v71r7_host_stock_display(uint32_t device, void *layer_desc)
{
    (void)device;
    ++g_env.stock_display_calls;
    uint8_t *d = (uint8_t *)layer_desc;
    memcpy(&g_env.stock_display_seen_y, d + 0x110, 8U);
    memcpy(&g_env.stock_display_seen_uv, d + 0x118, 8U);
    if (g_env.stock_display_rc & 1U) {
        g_env.disp_programmed = (uint64_t)(uintptr_t)(d + 0x100);
    }
    return g_env.stock_display_rc;
}

void v71r7_host_stock_vsync(uint32_t route)
{
    (void)route;
    ++g_env.stock_vsync_calls;
}

void v71r7_host_stock_power_sync(uint32_t command, uint64_t payload)
{
    ++g_env.stock_power_sync_calls;
    g_env.stock_power_last_cmd = command;
    g_env.stock_power_last_payload = payload;
    g_env.stock_power_saw_gate_closed = (fx_z8_lut_io_epoch(v71r7_io_gate()) == 0U);
    const Layer0Metrics *m = v71r7_layer0_metrics_ptr();
    g_env.stock_power_saw_active_banks = m ? atomic_load(&m->active_banks_mask) : 0U;
    g_env.stock_power_saw_ws_allocated = v71r7_layer0_workspace_allocated();
}

/* ============================================================================
 * Mock WYSIWYG FilmSim Provider (Stage 1 Ricoh HDF + Stage 2 Nikon Zf Grain)
 * ============================================================================ */

typedef struct {
    V71r7WysiwygState state;
    int still_capturing;
    uint32_t hdf_calls;
    uint32_t grain_calls;
    int last_hdf_is_evf;
    uintptr_t last_hdf_y_dma;
    uintptr_t last_hdf_uv_dma;
    uint32_t last_hdf_w;
    uint32_t last_hdf_h;
    uint32_t last_hdf_scratch_bytes;
} MockFilmSim;

static int mock_fs_query_state(void *ctx, V71r7WysiwygState *out)
{
    MockFilmSim *fs = (MockFilmSim *)ctx;
    *out = fs->state;
    return fs->state.active ? 1 : 0;
}

static int mock_fs_is_still_capturing(void *ctx)
{
    MockFilmSim *fs = (MockFilmSim *)ctx;
    return fs->still_capturing;
}

/* Non-commutative Stage 1 (Ricoh HDF): Y = Y - 5, UV += 3; exercises 512 KiB scratch! */
static int mock_fs_apply_hdf(void *ctx, uint8_t *y_cpu, uint8_t *uv_cpu,
                             uintptr_t y_dma, uintptr_t uv_dma,
                             uint32_t width, uint32_t height,
                             uint32_t y_stride, uint32_t uv_stride,
                             uint8_t hdf_level, uint8_t xm6_mode, uint8_t monochrome,
                             int is_evf,
                             uint8_t *scratch_512k, uint32_t scratch_bytes,
                             int (*cancelled)(void *), void *cancel_ctx)
{
    (void)hdf_level; (void)xm6_mode; (void)monochrome;
    MockFilmSim *fs = (MockFilmSim *)ctx;
    if (cancelled && cancelled(cancel_ctx)) {
        return -1;
    }
    assert(scratch_512k != NULL && scratch_bytes == V71R7_LAYER0_HDF_SCRATCH_BYTES);
    /* Exercise writing to the full 512 KiB scratch (verifies ASan bounds & stage_cpu reuse!). */
    scratch_512k[0] = 0xAAU;
    scratch_512k[scratch_bytes - 1U] = 0x55U;

    ++fs->hdf_calls;
    fs->last_hdf_is_evf = is_evf;
    fs->last_hdf_y_dma = y_dma;
    fs->last_hdf_uv_dma = uv_dma;
    fs->last_hdf_w = width;
    fs->last_hdf_h = height;
    fs->last_hdf_scratch_bytes = scratch_bytes;

    if (g_env.stage_order_len < 16U) {
        g_env.stage_order_log[g_env.stage_order_len++] = V71R7_L0_STAGE_HDF;
    }

    for (uint32_t r = 0U; r < height; ++r) {
        uint8_t *row = y_cpu + (size_t)r * y_stride;
        for (uint32_t c = 0U; c < width; ++c) {
            row[c] = (uint8_t)(row[c] >= 5U ? row[c] - 5U : 0U);
        }
    }
    for (uint32_t r = 0U; r < height / 2U; ++r) {
        uint8_t *row = uv_cpu + (size_t)r * uv_stride;
        for (uint32_t c = 0U; c < width; ++c) {
            row[c] = (uint8_t)(row[c] + 3U);
        }
    }
    return 0;
}

/* Non-commutative Stage 2 (Nikon Zf Grain): Y ^= 0x03 (luma-only!). */
static int mock_fs_apply_grain(void *ctx, uint8_t *y_cpu,
                               uint32_t width, uint32_t height, uint32_t y_stride)
{
    MockFilmSim *fs = (MockFilmSim *)ctx;
    ++fs->grain_calls;
    if (g_env.stage_order_len < 16U) {
        g_env.stage_order_log[g_env.stage_order_len++] = V71R7_L0_STAGE_GRAIN;
    }
    for (uint32_t r = 0U; r < height; ++r) {
        uint8_t *row = y_cpu + (size_t)r * y_stride;
        for (uint32_t c = 0U; c < width; ++c) {
            row[c] ^= 0x03U;
        }
    }
    return 0;
}

/* ============================================================================
 * Helpers for Building Valid Z8 C3.11 Enqueue Packets & Display Layer Descs
 * ============================================================================ */

static void build_enqueue_packet(uint8_t pkt[0x40], uint64_t *rect_out,
                                 uint32_t width, uint32_t height, uint32_t top,
                                 uintptr_t y_ptr, uintptr_t uv_ptr)
{
    memset(pkt, 0, 0x40);
    pkt[0] = 1U;
    uint64_t two = 2U;
    memcpy(pkt + 8, &two, 8U);
    uint16_t w = (uint16_t)width, h = (uint16_t)height;
    memcpy(pkt + 0x10, &w, 2U);
    memcpy(pkt + 0x12, &w, 2U);
    memcpy(pkt + 0x18, &w, 2U);
    memcpy(pkt + 0x1a, &h, 2U);
    uint64_t y64 = (uint64_t)y_ptr, uv64 = (uint64_t)uv_ptr;
    memcpy(pkt + 0x20, &y64, 8U);
    memcpy(pkt + 0x28, &uv64, 8U);
    *rect_out = ((uint64_t)top << 16) | ((uint64_t)width << 32) | ((uint64_t)(top + height) << 48);
}

static void build_display_desc(uint8_t desc[0x140], uint32_t width, uint32_t height,
                               int is_evf, uint64_t orig_y, uint64_t orig_uv)
{
    memset(desc, 0, 0x140);
    uint32_t fmt = 4U;
    memcpy(desc + 8, &fmt, 4U);
    desc[0x14] = 1U;
    uint16_t w = (uint16_t)width, h = (uint16_t)height;
    memcpy(desc + 0x16, &w, 2U);
    memcpy(desc + 0x18, &h, 2U);
    memcpy(desc + 0x1a, &w, 2U);
    memcpy(desc + 0x1c, &w, 2U);
    desc[0x1e] = 1U; /* Descriptor crop flag */
    memcpy(desc + 0x24, &w, 2U);
    memcpy(desc + 0x26, &h, 2U);
    if (is_evf) {
        desc[0x28] = 1U;
        uint16_t zero = 0U, top = 54U, bot = (uint16_t)(54U + height);
        memcpy(desc + 0x2a, &zero, 2U);
        memcpy(desc + 0x2c, &top, 2U);
        memcpy(desc + 0x2e, &w, 2U);
        memcpy(desc + 0x30, &bot, 2U);
    }
    memcpy(desc + 0x110, &orig_y, 8U);
    memcpy(desc + 0x118, &orig_uv, 8U);
}

static void fill_source_frame(uint8_t *buf, uint32_t width, uint32_t height,
                              uint8_t y_val, uint8_t uv_val)
{
    memset(buf, y_val, (size_t)width * height);
    memset(buf + 0x12C000U, uv_val, (size_t)width * (height / 2U));
}

/* ============================================================================
 * Suite 1: Dormant 0-Byte Class 0x24 Invariant & Early-Boot Hook Safety
 * ============================================================================ */
static void test_suite1_dormant_and_early_boot(void)
{
    printf("[Suite 1] Dormant 0-byte Class 0x24 invariant & early-boot safety...\n");
    env_reset();

    /* 1a. Early boot: hooks invoked BEFORE v71r7_layer0_init() must cleanly call stock! */
    _Alignas(8) uint8_t pkt[0x40], desc[0x140];
    uint64_t rect = 0U, stamp = 1U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, 0U, 0U);
    build_display_desc(desc, 1024U, 682U, 0, 0xA05000000ULL, 0xA0512C000ULL);
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    assert(v71r7_layer0_display_hook(0U, desc) == 1U);
    v71r7_layer0_vsync_hook(0U);
    assert(g_env.stock_enqueue_calls == 1U && g_env.stock_display_calls == 1U &&
           g_env.stock_vsync_calls == 1U);
    assert(g_env.ws_alloc_calls == 0U && g_env.ws_live_bytes == 0U);

    /* 1b. Init allocates Layer0Ctx on heap, 0 B Class 0x24! */
    assert(v71r7_layer0_init() == 0);
    assert(v71r7_layer0_init() == 0); /* Idempotent */
    assert(g_env.ws_alloc_calls == 0U && g_env.ws_live_count == 0U && g_env.ws_live_bytes == 0U);
    assert(!v71r7_layer0_workspace_allocated());

    /* 1c. Dormant loops (LUT=Off & FilmSim=Off): 0 B Class 0x24 allocated! */
    for (int i = 0; i < 5; ++i) {
        assert(v71r7_layer0_worker_step() == V71R7_L0_EMPTY);
        assert(v71r7_layer0_display_hook(0U, desc) == 1U);
        v71r7_layer0_vsync_hook(0U);
    }
    assert(g_env.ws_alloc_calls == 0U && g_env.ws_live_count == 0U && g_env.ws_live_bytes == 0U);
    assert(!v71r7_layer0_workspace_allocated());

    /* 1d. Enable LUT -> lazily allocates 14.55 MiB (0xE8C000); disable LUT -> frees back to 0 B! */
    g_env.lut_active = 1;
    g_env.lut_gen = 1U;
    v71r7_layer0_notify_lut_changed(1U, 1);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(v71r7_layer0_workspace_allocated());
    assert(g_env.ws_live_count == 1U);
    assert(v71r7_layer0_metrics_ptr()->surface_bytes == PHOTO_WORKSPACE_PREVIEW_BYTES);

    g_env.lut_active = 0;
    v71r7_layer0_notify_lut_changed(0U, 0);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(!v71r7_layer0_workspace_allocated());
    assert(g_env.ws_live_count == 0U && g_env.ws_live_bytes == 0U);
    assert(v71r7_layer0_metrics_ptr()->surface_bytes == 0U);

    /* 1e. FilmSim-only WYSIWYG (LUT=Off, HDF=2, Grain=3) -> allocates 14.55 MiB; turn Off -> 0 B! */
    MockFilmSim fs;
    memset(&fs, 0, sizeof(fs));
    fs.state = (V71r7WysiwygState){.active = 1U, .hdf_level = 2U, .zf_intensity = 3U, .xm6_mode = 0U, .cfg_version = 1U};
    V71r7WysiwygOps wops = {
        .ctx = &fs,
        .query_state = mock_fs_query_state,
        .is_still_capturing = mock_fs_is_still_capturing,
        .apply_hdf_nv12 = mock_fs_apply_hdf,
        .apply_grain_nv12 = mock_fs_apply_grain
    };
    assert(v71r7_layer0_set_wysiwyg_ops(&wops) == 0);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(v71r7_layer0_workspace_allocated());
    assert(g_env.ws_live_count == 1U);

    fs.state.active = 0U;
    fs.state.hdf_level = 0U;
    fs.state.zf_intensity = 0U;
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(!v71r7_layer0_workspace_allocated());
    assert(g_env.ws_live_count == 0U && g_env.ws_live_bytes == 0U);
    printf("  PASS (0 B Class 0x24 verified dormant & after deactivate)\n");
}

/* ============================================================================
 * Suite 2: photo_workspace 512 MiB Spacer Retry, Chunked D-Cache & Quarantine
 * ============================================================================ */
static void test_suite2_workspace_spacer_and_quarantine(void)
{
    printf("[Suite 2] photo_workspace 512 MiB spacer retry, chunked dcache & quarantine...\n");
    env_reset();

    /* Force first allocation to straddle 512 MiB boundary (0x85F800000..0x86068C000) */
    g_env.ws_force_next_straddle = 1;
    PhotoWorkspace ws;
    memset(&ws, 0, sizeof(ws));
    int rc = photo_workspace_acquire_ex(&ws, PHOTO_WORKSPACE_PREVIEW_BYTES,
                                        PHOTO_WS_FLAG_XM6_512M_GUARD);
    assert(rc == PHOTO_WS_OK);
    assert(ws.xm6_window_ok == 1U);
    assert(ws.spacer_retries == 1U);
    assert(g_env.ws_alloc_calls == 2U); /* Primary straddler + spacer-guarded retry */
    assert(g_env.ws_free_calls == 1U);  /* Spacer freed immediately! */
    assert(g_env.ws_live_count == 1U);  /* Exactly 1 live 14.55 MiB block */
    assert(flight_has_event(FX_Z8_FLIGHT_XM6_SPACER));
    assert(g_env.ws_max_dcache_chunk <= PHOTO_WORKSPACE_DCACHE_CHUNK);

    PhotoWorkspaceView view;
    assert(photo_workspace_pin(&ws, &view) == PHOTO_WS_OK);
    assert(view.cpu != NULL && view.bytes == PHOTO_WORKSPACE_PREVIEW_BYTES && view.xm6_window_ok == 1U);
    /* Release while pinned must return BUSY and keep block alive */
    assert(photo_workspace_release(&ws) == PHOTO_WS_BUSY);
    assert(photo_workspace_unpin(&ws) == PHOTO_WS_OK);
    assert(photo_workspace_release(&ws) == PHOTO_WS_OK);
    assert(g_env.ws_live_count == 0U && g_env.ws_live_bytes == 0U);
    assert(g_env.ws_max_dcache_chunk <= PHOTO_WORKSPACE_DCACHE_CHUNK);

    /* Quarantine malformed/untyped handle (must NEVER free untyped handle) */
    g_env.ws_force_next_malformed = 1;
    PhotoWorkspace ws_bad;
    memset(&ws_bad, 0, sizeof(ws_bad));
    uint32_t frees_before = g_env.ws_free_calls;
    assert(photo_workspace_acquire(&ws_bad, PHOTO_WORKSPACE_PREVIEW_BYTES) == PHOTO_WS_RETAINED);
    assert(photo_workspace_is_quarantined(&ws_bad));
    assert(g_env.ws_free_calls == frees_before); /* Untyped handle never freed! */
    assert(photo_workspace_acquire(&ws_bad, PHOTO_WORKSPACE_PREVIEW_BYTES) == PHOTO_WS_RETAINED);
    assert(photo_workspace_release(&ws_bad) == PHOTO_WS_RETAINED);
    printf("  PASS (spacer retry=1, live=1, chunk<=512KiB, quarantine verified)\n");
}

/* ============================================================================
 * Suite 3: Full WYSIWYG Pipeline (Stage 0 LUT -> Stage 1 HDF -> Stage 2 Grain),
 *          source_lag=1, Display Swap+Restore, 2-VSync & 4-VSync Cross-Route
 * ============================================================================ */
static void test_suite3_wysiwyg_pipeline_and_fences(void)
{
    printf("[Suite 3] End-to-end WYSIWYG (LUT->HDF->Grain), source_lag=1, swap+restore, VSync fences...\n");
    env_reset();
    assert(v71r7_layer0_init() == 0);

    MockFilmSim fs;
    memset(&fs, 0, sizeof(fs));
    fs.state = (V71r7WysiwygState){
        .active = 1U, .hdf_level = 2U, .zf_intensity = 4U, .xm6_mode = 0U, .cfg_version = 7U
    };
    V71r7WysiwygOps wops = {
        .ctx = &fs,
        .query_state = mock_fs_query_state,
        .is_still_capturing = mock_fs_is_still_capturing,
        .apply_hdf_nv12 = mock_fs_apply_hdf,
        .apply_grain_nv12 = mock_fs_apply_grain
    };
    assert(v71r7_layer0_set_wysiwyg_ops(&wops) == 0);

    g_env.lut_active = 1;
    g_env.lut_gen = 42U;
    v71r7_layer0_notify_lut_changed(42U, 1);

    /* First worker_step arms & lazily allocates 14.55 MiB workspace (0 frames queued yet). */
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(v71r7_layer0_workspace_allocated());

    /* Allocate 3 distinct aligned NV12 source frame buffers (0x1C2000 each). */
    uint8_t *src1 = alloc_aligned_512(0x1C2000U);
    uint8_t *src2 = alloc_aligned_512(0x1C2000U);
    uint8_t *src3 = alloc_aligned_512(0x1C2000U);
    fill_source_frame(src1, 1024U, 682U, 40U, 100U);
    fill_source_frame(src2, 1024U, 682U, 50U, 120U);
    fill_source_frame(src3, 1024U, 682U, 60U, 140U);

    _Alignas(8) uint8_t pkt[0x40], desc[0x140];
    uint64_t rect = 0U, stamp = 0U;

    /* Enqueue Frame 1 (gen=1): with source_lag=1, worker_step must NOT grade yet! */
    stamp = 1001U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(v71r7_layer0_metrics_ptr()->grades == 0U); /* source_lag=1 held back gen=1! */

    /* Enqueue Frame 2 (gen=2): now head=2 -> head - lag = 1 -> grades Frame 1 (Y=40, UV=100)! */
    stamp = 1002U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src2, (uintptr_t)(src2 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);

    g_env.stage_order_len = 0U;
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(v71r7_layer0_metrics_ptr()->grades == 1U);
    /* Verify EXACT non-commutative stage order: Stage 0 LUT -> Stage 1 HDF -> Stage 2 Grain! */
    assert(g_env.stage_order_len == 3U);
    assert(g_env.stage_order_log[0] == V71R7_L0_STAGE_LUT);
    assert(g_env.stage_order_log[1] == V71R7_L0_STAGE_HDF);
    assert(g_env.stage_order_log[2] == V71R7_L0_STAGE_GRAIN);
    assert(v71r7_layer0_last_stages_applied() ==
           (V71R7_L0_STAGE_LUT | V71R7_L0_STAGE_HDF | V71R7_L0_STAGE_GRAIN));
    assert(fs.last_hdf_is_evf == 0 && fs.last_hdf_y_dma != 0U && fs.last_hdf_uv_dma != 0U);
    assert(flight_has_event(FX_Z8_FLIGHT_L0_WYSIWYG));

    /* Display hook (0x40CD862C): swaps layer_desc+0x110/+0x118 -> restores immediately! */
    const uint64_t orig_y = 0xA05000000ULL, orig_uv = 0xA0512C000ULL;
    build_display_desc(desc, 1024U, 682U, 0, orig_y, orig_uv);
    assert(v71r7_layer0_display_hook(0U, desc) == 1U);

    /* Verify stock_display SAW the graded bank pointers! */
    assert(g_env.stock_display_seen_y != orig_y);
    assert(g_env.stock_display_seen_uv == g_env.stock_display_seen_y + 1024U * 682U);
    /* Verify original caller pointers restored IMMEDIATELY upon return! */
    uint64_t after_y = 0U, after_uv = 0U;
    memcpy(&after_y, desc + 0x110, 8U);
    memcpy(&after_uv, desc + 0x118, 8U);
    assert(after_y == orig_y && after_uv == orig_uv);

    /* Inspect exact graded pixels in the presented bank:
     * Frame 1 had Y=40, UV=100:
     *   Stage 0 LUT:   Y=(40+10)*2 = 100; UV=100^0x11 = 117
     *   Stage 1 HDF:   Y=100-5 = 95;      UV=117+3 = 120
     *   Stage 2 Grain: Y=95^3 = 92;       UV=120 (untouched) */
    const uint8_t *presented_y = (const uint8_t *)v71r7_host_ws_to_cpu((uintptr_t)g_env.stock_display_seen_y);
    const uint8_t *presented_uv = (const uint8_t *)v71r7_host_ws_to_cpu((uintptr_t)g_env.stock_display_seen_uv);
    assert(presented_y[0] == 92U && presented_y[1024U * 682U - 1U] == 92U);
    assert(presented_uv[0] == 120U && presented_uv[1024U * 341U - 1U] == 120U);

    /* Verify evidence captured BEFORE Stage 1 overwrote stage_cpu! */
    const Layer0Evidence *ev = v71r7_layer0_evidence_ptr();
    assert(ev->ready == 1U && ev->quads[0][0][0] == 40U && ev->quads[0][0][6] == 92U);
    assert(ev->quads[0][0][4] == 100U && ev->quads[0][0][10] == 120U);

    /* Verify Bank 0 is currently leased in hardware. */
    uint32_t mask1 = atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask);
    assert(mask1 == 1U);

    /* Test mid-copy mutation abort (uncached spot check catches ISP overwrite!). */
    env_advance_ms(30U);
    stamp = 1003U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src3, (uintptr_t)(src3 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    g_env.corrupt_after_copy_ptr = src2 + 0; /* Mutate row 0 of Frame 2 right during copy */
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(v71r7_layer0_metrics_ptr()->mismatch_y == 1U);
    assert(v71r7_layer0_metrics_ptr()->grades == 1U); /* Aborted cleanly, did not publish! */

    /* Enqueue Frame 4 (gen=4) -> grades Frame 3 (Y=60 -> ((60+10)*2 - 5)^3 = 135^3 = 132). */
    env_advance_ms(30U);
    stamp = 1004U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(v71r7_layer0_metrics_ptr()->grades == 2U);

    /* Present Bank 1 -> Bank 0 transitions to BANK_RETIRING; verify 2-VSync fence! */
    assert(v71r7_layer0_display_hook(0U, desc) == 1U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) == 3U); /* Bank 0 + Bank 1 */
    const uint8_t *presented2_y = (const uint8_t *)v71r7_host_ws_to_cpu((uintptr_t)g_env.stock_display_seen_y);
    assert(presented2_y[0] == 132U);

    /* 1st VSync: Bank 0 must STILL be leased (fence requires >= 2 VSyncs!). */
    v71r7_layer0_vsync_hook(0U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) == 3U);
    assert(v71r7_layer0_metrics_ptr()->banks_retired == 0U);

    /* 2nd VSync: Bank 0 retires cleanly! Only Bank 1 remains leased. */
    v71r7_layer0_vsync_hook(0U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) == 2U);
    assert(v71r7_layer0_metrics_ptr()->banks_retired == 1U);

    /* DISP_CACHE_DESC pin protection: pin Bank 1 in stock suspend cache. */
    g_env.disp_cache_desc_y = g_env.stock_display_seen_y;
    g_env.disp_cache_desc_uv = g_env.stock_display_seen_uv;
    /* Pass stock frame (temporarily turn display_mode=0) + 4 VSyncs: Bank 1 MUST stay pinned! */
    assert(v71r7_layer0_set_display_mode(0U) == V71R7_L0_OK);
    assert(v71r7_layer0_display_hook(0U, desc) == 1U);
    for (int i = 0; i < 4; ++i) v71r7_layer0_vsync_hook(0U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) == 2U);
    /* Clear DISP_CACHE_DESC, re-enable display_mode=1 -> Bank 1 ready to retire on next route! */
    g_env.disp_cache_desc_y = 0U;
    g_env.disp_cache_desc_uv = 0U;
    assert(v71r7_layer0_set_display_mode(1U) == V71R7_L0_OK);
    /* Re-pin Bank 1 on LCD via display_hook keep_current before switching to EVF */
    assert(v71r7_layer0_display_hook(0U, desc) == 1U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) == 2U);

    /* --- Switch to EVF (1280x852 y=54 in 1280x960) & verify 4-VSync + 100 ms cross-route fence! --- */
    env_set_route_evf();
    fill_source_frame(src1, 1280U, 852U, 30U, 90U);
    fill_source_frame(src2, 1280U, 852U, 35U, 95U);

    /* Enqueue 2 EVF frames (route=3, top=54) so lag=1 grades EVF Frame 1 (Y=30 -> ((30+10)*2-5)^3=75^3=72). */
    env_advance_ms(20U);
    stamp = 2001U;
    build_enqueue_packet(pkt, &rect, 1280U, 852U, 54U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(3U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    stamp = 2002U;
    build_enqueue_packet(pkt, &rect, 1280U, 852U, 54U, (uintptr_t)src2, (uintptr_t)(src2 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(3U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);

    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(fs.last_hdf_is_evf == 1 && fs.last_hdf_y_dma != 0U && fs.last_hdf_uv_dma != 0U);
    assert(fs.last_hdf_w == 1280U && fs.last_hdf_h == 852U);
    assert(fs.last_hdf_scratch_bytes == V71R7_LAYER0_HDF_SCRATCH_BYTES);

    /* Present EVF Bank (device=2): LCD Bank 1 enters cross-route retirement! */
    _Alignas(8) uint8_t evf_desc[0x140];
    build_display_desc(evf_desc, 1280U, 852U, 1, orig_y, orig_uv);
    assert(v71r7_layer0_display_hook(2U, evf_desc) == 1U);
    assert(g_env.stock_display_seen_uv == g_env.stock_display_seen_y + 0x12C000U);
    const uint8_t *evf_y = (const uint8_t *)v71r7_host_ws_to_cpu((uintptr_t)g_env.stock_display_seen_y);
    assert(evf_y[0] == 72U && evf_y[1280U * 852U - 1U] == 72U);
    assert((atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) & 2U) != 0U); /* LCD Bank 1 still leased */

    /* 4 EVF VSyncs with only 50 ms elapsed -> LCD Bank 1 MUST NOT retire yet (< 100 ms)! */
    env_advance_ms(50U);
    for (int i = 0; i < 4; ++i) v71r7_layer0_vsync_hook(3U);
    assert((atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) & 2U) != 0U);
    assert(v71r7_layer0_metrics_ptr()->cross_route_retires == 0U);

    /* Advance past 100 ms + 1 EVF VSync -> LCD Bank 1 retires cross-route! */
    env_advance_ms(60U); /* Total 110 ms >= 100 ms, 5 EVF VSyncs >= 4 */
    v71r7_layer0_vsync_hook(3U);
    assert((atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) & 2U) == 0U);
    assert(v71r7_layer0_metrics_ptr()->cross_route_retires == 1U);

    /* Verify reverse cross-route under-VSync guard: 3 LCD VSyncs + 150 ms MUST NOT retire EVF Bank! */
    uint32_t evf_mask = atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask);
    assert(evf_mask != 0U);
    env_set_route_lcd();
    assert(v71r7_layer0_display_hook(0U, desc) == 1U); /* Stock pass on LCD starts EVF cross-route retire */
    env_advance_ms(150U);
    for (int i = 0; i < 3; ++i) v71r7_layer0_vsync_hook(0U);
    assert((atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) & evf_mask) == evf_mask);
    v71r7_layer0_vsync_hook(0U); /* 4th LCD VSync with >= 100 ms retires EVF bank! */
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) == 0U);
    assert(v71r7_layer0_metrics_ptr()->cross_route_retires == 2U);

    /* Verify RTOS timed Pool 0 lock (0x40D2C698 tmout=2ms) recovers transient contention
     * with ZERO busy-skip, while persistent busy records enqueue_skipped_busy cleanly! */
    uint32_t prev_grades = atomic_load(&v71r7_layer0_metrics_ptr()->grades);
    uint32_t prev_busy = atomic_load(&v71r7_layer0_metrics_ptr()->enqueue_skipped_busy);
    g_env.pool_busy = 0;
    g_env.pool_busy_only_on_poll = 1;
    env_advance_ms(25U);
    stamp = 2003U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    stamp = 2004U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src2, (uintptr_t)(src2 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    uint32_t lock_waits = 0U, lock_wait_ok = 0U;
    v71r7_layer0_lock_stats(&lock_waits, &lock_wait_ok);
    assert(lock_waits >= 1U && lock_wait_ok == lock_waits);
    assert(g_env.pool_last_tmout_ms == 2U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->grades) > prev_grades);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->enqueue_skipped_busy) == prev_busy);
    g_env.pool_busy_only_on_poll = 0;

    /* Verify persistent Pool 0 busy skip, still_capturing pause, DS 0x2126 burst gate & gui_poll! */
    g_env.pool_busy = 1;
    env_advance_ms(25U);
    stamp = 2005U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    stamp = 2006U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src2, (uintptr_t)(src2 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(v71r7_layer0_metrics_ptr()->enqueue_skipped_busy >= 1U);
    g_env.pool_busy = 0;

    /* Verify DS 0x2126 release mode gate: S(0), CL(1), CH(2) allowed; C30(3)..C15(6) bypassed! */
    for (uint32_t rel = 3U; rel <= 6U; ++rel) {
        g_env.gui_release_mode = rel;
        assert(v71r7_layer0_worker_step() == V71R7_L0_EMPTY);
        assert(atomic_load(&v71r7_layer0_metrics_ptr()->mode_ok) == 0U);
    }
    for (uint32_t rel = 0U; rel <= 2U; ++rel) {
        g_env.gui_release_mode = rel;
        assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
        assert(atomic_load(&v71r7_layer0_metrics_ptr()->mode_ok) == 1U);
    }
    g_env.gui_release_mode = 0U;

    FxZ8RearFlightMetrics fm;
    v71r7_layer0_get_metrics(&fm);
    assert(fm.active_route == 0U && fm.neon_active == 1U);

    /* V71.7-r4 P0 Crash Fix #2: still capture pauses AND retires photo_workspace
     * to return 21.84 MiB Class 0x24 Selector 0 headroom to 45MP PhotoDevelop! */
    fs.still_capturing = 1;
    assert(v71r7_layer0_worker_step() == V71R7_L0_EMPTY);
    assert(v71r7_layer0_metrics_ptr()->jpeg_pauses >= 1U);
    assert(v71r7_layer0_workspace_allocated() == 0);
    fs.still_capturing = 0;

    /* Next active step re-acquires photo_workspace cleanly */
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    v71r7_layer0_get_metrics(&fm);
    assert(fm.active_route == 0U && fm.neon_active == 1U);

    g_env.current_view = 0U; /* Leave shooting view -> gui_poll clears mode_ok */
    v71r7_layer0_gui_poll();
    assert(v71r7_layer0_metrics_ptr()->mode_ok == 0U && !v71r7_layer0_is_presented());

    free(src1);
    free(src2);
    free(src3);
    printf("  PASS (LUT->HDF->Grain pixel-verified LCD+EVF, swap+restore, 2-VSync & 4-VSync+100ms fences)\n");
}

/* ============================================================================
 * Suite 4: HangFix V69 v71r7_layer0_pool_suspend() & pool_resume()
 * ============================================================================ */

static uint8_t g_suspend_desc[0x140];
static void sleep_pump_lcd_drain(uint32_t ms, uint32_t idx)
{
    (void)ms;
    if (idx == 1U) {
        /* Next display_hook sees pool_suspended/power_closing -> passes stock frame */
        assert(v71r7_layer0_display_hook(0U, g_suspend_desc) == 1U);
    } else if (idx == 2U || idx == 3U) {
        /* 2 VSyncs retire the leased hardware bank cleanly within 3 ms (< 100 ms)! */
        v71r7_layer0_vsync_hook(0U);
    }
}

static void test_suite4_hangfix_v69_pool_suspend_resume(void)
{
    printf("[Suite 4] HangFix V69 pool_suspend() drain+free 14.55MiB & pool_resume()...\n");
    env_reset();
    assert(v71r7_layer0_init() == 0);

    g_env.lut_active = 1;
    g_env.lut_gen = 5U;
    v71r7_layer0_notify_lut_changed(5U, 1);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);

    /* Enqueue 2 frames, grade & present Bank 0 so hardware actively holds a lease! */
    uint8_t *src1 = alloc_aligned_512(0x1C2000U);
    uint8_t *src2 = alloc_aligned_512(0x1C2000U);
    fill_source_frame(src1, 1024U, 682U, 40U, 128U);
    fill_source_frame(src2, 1024U, 682U, 42U, 128U);
    _Alignas(8) uint8_t pkt[0x40];
    uint64_t rect = 0U, stamp = 3001U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    stamp = 3002U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src2, (uintptr_t)(src2 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);

    build_display_desc(g_suspend_desc, 1024U, 682U, 0, 0xA05000000ULL, 0xA0512C000ULL);
    assert(v71r7_layer0_display_hook(0U, g_suspend_desc) == 1U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) != 0U);
    assert(g_env.ws_live_count == 1U);

    /* Trigger HangFix V69 pool_suspend(): drains VSync bank & releases 14.55 MiB! */
    g_env.sleep_calls = 0U;
    g_env.on_sleep = sleep_pump_lcd_drain;
    v71r7_layer0_pool_suspend();
    g_env.on_sleep = NULL;

    assert((v71r7_anchor()->flags & V71R7_ANCHOR_FLAG_POOL_SUSPENDED) != 0U);
    assert(flight_count_event(FX_Z8_FLIGHT_POOL_SUSPEND) == 1U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) == 0U);
    assert(!v71r7_layer0_workspace_allocated());
    assert(g_env.ws_live_count == 0U && g_env.ws_live_bytes == 0U); /* 0 B Class 0x24 held! */

    /* Redundant pool_suspend() while already suspended + 0 B workspace MUST NOT flood flight ring */
    v71r7_layer0_pool_suspend();
    assert(flight_count_event(FX_Z8_FLIGHT_POOL_SUSPEND) == 1U);

    /* While suspended, worker_step MUST NOT reallocate! */
    assert(v71r7_layer0_worker_step() == V71R7_L0_EMPTY);
    assert(g_env.ws_live_count == 0U && g_env.ws_live_bytes == 0U);

    /* HangFix V69 pool_resume(): clears flag lazily, edge-triggered flight log (no 10-30 Hz flood)! */
    v71r7_layer0_pool_resume();
    assert((v71r7_anchor()->flags & V71R7_ANCHOR_FLAG_POOL_SUSPENDED) == 0U);
    assert(flight_count_event(FX_Z8_FLIGHT_POOL_RESUME) == 1U);
    for (int r = 0; r < 5; ++r) {
        v71r7_layer0_pool_resume();
    }
    assert(flight_count_event(FX_Z8_FLIGHT_POOL_RESUME) == 1U);
    assert(g_env.ws_live_count == 0U); /* Lazy until worker_step */

    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(v71r7_layer0_workspace_allocated() && g_env.ws_live_count == 1U);

    /* Enqueue 2 fresh frames -> grades & swaps cleanly after resume! */
    env_advance_ms(30U);
    stamp = 3003U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    stamp = 3004U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src2, (uintptr_t)(src2 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(v71r7_layer0_display_hook(0U, g_suspend_desc) == 1U);
    assert(g_env.stock_display_seen_y != 0xA05000000ULL);

    free(src1);
    free(src2);
    printf("  PASS (0 B Class 0x24 while suspended, seamless lazy recovery on resume)\n");
}

/* ============================================================================
 * Suite 5: POWER_OFF_SYNC (10) / POWER_CANCEL_SYNC (11) & STOPPED Guard
 * ============================================================================ */

static int g_power_card_io_held = 0;
static int g_power_still_frame_held = 0;

static void sleep_pump_power_drain(uint32_t ms, uint32_t idx)
{
    (void)ms;
    if (idx == 1U) {
        assert(v71r7_layer0_display_hook(0U, g_suspend_desc) == 1U);
    } else if (idx == 2U || idx == 3U) {
        v71r7_layer0_vsync_hook(0U);
    } else if (idx == 4U && g_power_card_io_held) {
        fx_z8_lut_io_drop(v71r7_io_gate());
        g_power_card_io_held = 0;
    } else if (idx == 6U && g_power_still_frame_held) {
        fx_z8_lut_frame_drop(v71r7_io_gate());
        g_power_still_frame_held = 0;
    }
}

static void test_suite5_power_sync_and_lifecycle_stop(void)
{
    printf("[Suite 5] POWER_OFF_SYNC / POWER_CANCEL_SYNC & lifecycle STOPPED guard...\n");
    env_reset();
    assert(v71r7_layer0_init() == 0);

    g_env.lut_active = 1;
    g_env.lut_gen = 9U;
    v71r7_layer0_notify_lut_changed(9U, 1);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);

    /* Grade & lease Bank 0 in hardware. */
    uint8_t *src1 = alloc_aligned_512(0x1C2000U);
    uint8_t *src2 = alloc_aligned_512(0x1C2000U);
    fill_source_frame(src1, 1024U, 682U, 45U, 128U);
    fill_source_frame(src2, 1024U, 682U, 46U, 128U);
    _Alignas(8) uint8_t pkt[0x40];
    uint64_t rect = 0U, stamp = 4001U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    stamp = 4002U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src2, (uintptr_t)(src2 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);

    build_display_desc(g_suspend_desc, 1024U, 682U, 0, 0xA05000000ULL, 0xA0512C000ULL);
    assert(v71r7_layer0_display_hook(0U, g_suspend_desc) == 1U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) != 0U);

    /* Also hold active card I/O lease + Still JPEG frame lease on FxZ8LutIoGate! */
    FxZ8LutIoGate *gate = v71r7_io_gate();
    uint32_t epoch1 = fx_z8_lut_io_epoch(gate);
    assert(epoch1 == 1U);
    assert(fx_z8_lut_io_take(gate, epoch1) == 1);
    assert(fx_z8_lut_frame_take(gate, epoch1) == 1);
    g_power_card_io_held = 1;
    g_power_still_frame_held = 1;

    /* Trigger POWER_OFF_SYNC (10): must close gate, drain banks+IO+Still, free 14.55MiB BEFORE stock! */
    g_env.sleep_calls = 0U;
    g_env.on_sleep = sleep_pump_power_drain;
    v71r7_power_sync_hook(10U, 0xCAFEBABEULL);
    g_env.on_sleep = NULL;

    assert(g_env.stock_power_sync_calls == 1U && g_env.stock_power_last_cmd == 10U);
    assert(g_env.stock_power_saw_gate_closed == 1);
    assert(g_env.stock_power_saw_active_banks == 0U);
    assert(g_env.stock_power_saw_ws_allocated == 0); /* Freed BEFORE stock 0x40D60E94 ran! */
    assert(fx_z8_lut_io_epoch(gate) == 0U);
    assert(fx_z8_lut_io_take(gate, epoch1) == 0);    /* Gate closed rejects new leases */
    assert(g_env.ws_live_count == 0U && g_env.ws_live_bytes == 0U);
    assert(v71r7_layer0_power_metrics_ptr()->result == V71R7_L0_OK);
    assert(flight_has_event(FX_Z8_FLIGHT_PREVIEW_POWER_OFF));

    /* Trigger POWER_CANCEL_SYNC (11): reopens gate (epoch 2), resumes seamlessly! */
    v71r7_power_sync_hook(11U, 0xCAFEBABEULL);
    uint32_t epoch2 = fx_z8_lut_io_epoch(gate);
    assert(epoch2 == epoch1 + 1U);
    assert(flight_has_event(FX_Z8_FLIGHT_POWER_CANCEL));
    assert(v71r7_layer0_power_metrics_ptr()->cancel_calls == 1U);

    /* Next worker_step lazily reallocates workspace & resumes! */
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(v71r7_layer0_workspace_allocated() && g_env.ws_live_count == 1U);

    /* Permanent lifecycle stop sets FX_Z8_LUT_IO_STOPPED -> late POWER_CANCEL_SYNC CANNOT reopen! */
    v71r7_layer0_lifecycle_stop();
    assert((gate->state & FX_Z8_LUT_IO_STOPPED) != 0U);
    assert(fx_z8_lut_io_epoch(gate) == 0U);
    assert(!v71r7_layer0_workspace_allocated() && g_env.ws_live_count == 0U);

    v71r7_power_sync_hook(11U, 0xDEAD0011ULL);
    assert(fx_z8_lut_io_epoch(gate) == 0U);          /* STOPPED bit blocked reopen! */
    assert((gate->state & FX_Z8_LUT_IO_STOPPED) != 0U);
    assert(v71r7_layer0_worker_step() == V71R7_L0_STATE);
    assert(g_env.ws_live_count == 0U && g_env.ws_live_bytes == 0U);

    free(src1);
    free(src2);
    printf("  PASS (POWER_OFF drain+free before stock, POWER_CANCEL reopen, STOPPED lockout)\n");
}

/* ============================================================================
 * Suite 6: Exact AArch64 BL Preimage Verification for All 6 ROM Hook Sites
 * ============================================================================ */
static void test_suite6_hook_table_preimages(void)
{
    printf("[Suite 6] Verified 6-site stock ROM BL preimage math...\n");
    for (unsigned i = 0U; i < 6U; ++i) {
        const V71r7HookSpec *h = &v71r7_layer0_hook_specs[i];
        int64_t diff = (int64_t)(uint64_t)h->stock_target_va - (int64_t)(uint64_t)h->patch_va;
        assert((diff & 3) == 0);
        uint32_t expected_bl = 0x94000000U | ((uint32_t)(diff >> 2) & 0x03FFFFFFU);
        assert(expected_bl == h->expected_insn);
    }
    printf("  PASS (all 6 BL preimages match exact AArch64 encodings)\n");
}

/* ============================================================================
 * Suite 7: Hung XM6 Live DMA (-2) Bank Quarantine (Zero UAF on pool_suspend!)
 * ============================================================================ */
static int mock_fs_apply_hdf_hung_dma(void *ctx, uint8_t *y_cpu, uint8_t *uv_cpu,
                                      uintptr_t y_dma, uintptr_t uv_dma,
                                      uint32_t width, uint32_t height,
                                      uint32_t y_stride, uint32_t uv_stride,
                                      uint8_t hdf_level, uint8_t xm6_mode,
                                      uint8_t monochrome, int is_evf,
                                      uint8_t *scratch_512k, uint32_t scratch_bytes,
                                      int (*cancelled)(void *), void *cancel_ctx)
{
    (void)ctx; (void)y_cpu; (void)uv_cpu; (void)y_dma; (void)uv_dma;
    (void)width; (void)height; (void)y_stride; (void)uv_stride;
    (void)hdf_level; (void)xm6_mode; (void)monochrome; (void)is_evf;
    (void)scratch_512k; (void)scratch_bytes; (void)cancelled; (void)cancel_ctx;
    return -2; /* Z8_XM6_LIVE_HUNG_DMA */
}

static void test_suite7_hung_xm6_dma_quarantine(void)
{
    printf("[Suite 7] Hung XM6 Live DMA (-2) bank quarantine & pool_suspend UAF protection...\n");
    env_reset();
    assert(v71r7_layer0_init() == 0);

    MockFilmSim fs;
    memset(&fs, 0, sizeof(fs));
    fs.state.active = 1U;
    fs.state.hdf_level = 2U;
    V71r7WysiwygOps wops = {
        .ctx = &fs,
        .query_state = mock_fs_query_state,
        .is_still_capturing = mock_fs_is_still_capturing,
        .apply_hdf_nv12 = mock_fs_apply_hdf_hung_dma,
        .apply_grain_nv12 = mock_fs_apply_grain
    };
    assert(v71r7_layer0_set_wysiwyg_ops(&wops) == 0);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);

    uint8_t *src1 = alloc_aligned_512(0x1C2000U);
    uint8_t *src2 = alloc_aligned_512(0x1C2000U);
    fill_source_frame(src1, 1024U, 682U, 40U, 128U);
    fill_source_frame(src2, 1024U, 682U, 42U, 128U);
    _Alignas(8) uint8_t pkt[0x40];
    uint64_t rect = 0U, stamp = 7001U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    stamp = 7002U;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src2, (uintptr_t)(src2 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);

    /* -2 return MUST quarantine the bank (NOT return to BANK_FREE) & return V71R7_L0_BUSY */
    assert(v71r7_layer0_worker_step() == V71R7_L0_BUSY);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->quarantined) == 1U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) != 0U);

    /* Even after stock display pass + VSync ticks + pool_suspend, quarantined bank & workspace
     * MUST stay pinned (NEVER freed to Class 0x24 while hung DSP DMA may still write!) */
    _Alignas(8) uint8_t desc[0x140];
    build_display_desc(desc, 1024U, 682U, 0, 0xA05000000ULL, 0xA0512C000ULL);
    assert(v71r7_layer0_display_hook(0U, desc) == 1U);
    v71r7_layer0_vsync_hook(0U);
    v71r7_layer0_vsync_hook(0U);
    v71r7_layer0_vsync_hook(0U);
    v71r7_layer0_pool_suspend();
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) != 0U);
    assert(v71r7_layer0_workspace_allocated() && g_env.ws_live_count == 1U);

    free(src1);
    free(src2);
    printf("  PASS (hung XM6 Live DMA -2 bank permanently quarantined, zero Class 0x24 UAF)\n");
}

/* ============================================================================
 * Suite 8: V71.7-r4 P0 Crash Fixes (#1 Mutex/Unpin/Force-Retire, #2 Still Relief & Dormant)
 * ============================================================================ */
static void test_suite8_r4_stability_mutex_unpin_suspend_still(void)
{
    printf("[Suite 8] V71.7-r4 CONFIG_MUTEX, disp_reject unpin, pool_suspend force-retire, LCD->EVF & still relief...\n");
    env_reset();

    /* 8a. photo_workspace CONFIG_MUTEX failure & TOCTOU pool-invalidation safety */
    PhotoWorkspace ws;
    memset(&ws, 0, sizeof(ws));
    g_env.ws_fail_config_lock = 1;
    assert(photo_workspace_acquire(&ws, PHOTO_WORKSPACE_PREVIEW_BYTES) == PHOTO_WS_BUSY);
    assert(g_env.ws_alloc_calls == 0U && g_env.ws_config_mutex_depth == 0U);
    g_env.ws_fail_config_lock = 0;

    g_env.pool_valid = 1;
    g_env.ws_invalidate_pool_on_lock = 1; /* Pool turns invalid after lock -> TOCTOU caught! */
    assert(photo_workspace_acquire(&ws, PHOTO_WORKSPACE_PREVIEW_BYTES) == PHOTO_WS_BUSY);
    assert(g_env.ws_alloc_calls == 0U && g_env.ws_config_mutex_depth == 0U);
    g_env.pool_valid = 1;

    assert(photo_workspace_acquire(&ws, PHOTO_WORKSPACE_PREVIEW_BYTES) == PHOTO_WS_OK);
    assert(g_env.ws_alloc_calls == 1U && g_env.ws_config_mutex_depth == 0U);
    assert(photo_workspace_release(&ws) == PHOTO_WS_OK);
    assert(g_env.ws_free_calls == 1U && g_env.ws_live_count == 0U && g_env.ws_config_mutex_depth == 0U);

    /* 8b. disp_rejects (confirmed == 0) MUST immediately unpin selected bank (0 BANK_IN_HW leak!) */
    env_reset();
    assert(v71r7_layer0_init() == 0);
    g_env.lut_active = 1;
    g_env.lut_gen = 5U;
    v71r7_layer0_notify_lut_changed(5U, 1);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);

    uint8_t *src1 = alloc_aligned_512(0x1C2000U);
    uint8_t *src2 = alloc_aligned_512(0x1C2000U);
    fill_source_frame(src1, 1024U, 682U, 40U, 128U);
    fill_source_frame(src2, 1024U, 682U, 42U, 128U);
    _Alignas(8) uint8_t pkt[0x40], desc[0x140];
    uint64_t rect = 0U, stamp = 8001U;
    uint64_t orig_y = 0xA05000000ULL, orig_uv = 0xA0512C000ULL;
    build_display_desc(desc, 1024U, 682U, 0, orig_y, orig_uv);

    for (int rej = 0; rej < 6; ++rej) {
        env_advance_ms(20U);
        build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
        assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
        ++stamp;
        build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src2, (uintptr_t)(src2 + 0x12C000U));
        assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
        ++stamp;
        assert(v71r7_layer0_worker_step() == V71R7_L0_OK);

        /* Reject display call (rc == 0 -> confirmed == 0): MUST NOT leak active_banks_mask! */
        g_env.stock_display_rc = 0U;
        assert(v71r7_layer0_display_hook(0U, desc) == 0U);
        assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) == 0U);
        assert(atomic_load(&v71r7_layer0_metrics_ptr()->hw_current) == -1);
    }
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->disp_rejects) == 6U);
    g_env.stock_display_rc = 1U;

    /* 8c. pool_suspend() force-retire fallback when non-quarantined bank stays pinned across switch */
    env_advance_ms(20U);
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    ++stamp;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src2, (uintptr_t)(src2 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    ++stamp;
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(v71r7_layer0_display_hook(0U, desc) == 1U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) != 0U);

    /* Simulate stock Class 0x24 teardown during LCD->EVF switch with NO VSync/display ticks arriving:
     * force-retire MUST clear non-quarantined bank and free 14.55 MiB photo_workspace! */
    g_env.on_sleep = NULL;
    v71r7_layer0_pool_suspend();
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) == 0U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->force_retires) >= 1U);
    assert(!v71r7_layer0_workspace_allocated() && g_env.ws_live_count == 0U);
    v71r7_layer0_pool_resume();

    /* 8d. Rapid LCD -> EVF switch without confirmed EVF display_hook still starts cross-route retire */
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    env_advance_ms(20U);
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    ++stamp;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src2, (uintptr_t)(src2 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    ++stamp;
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(v71r7_layer0_display_hook(0U, desc) == 1U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) != 0U);

    env_set_route_evf();
    v71r7_layer0_vsync_hook(3U); /* Kicks off cross-route retire for LCD bank */
    env_advance_ms(120U);
    for (int i = 0; i < 4; ++i) v71r7_layer0_vsync_hook(3U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->active_banks_mask) == 0U);
    env_set_route_lcd();

    /* 8e. P0 Fix #2: Still capture (is_still_capturing==1) retires 14.55 MiB workspace for 45MP PhotoDevelop! */
    MockFilmSim fs;
    memset(&fs, 0, sizeof(fs));
    fs.state = (V71r7WysiwygState){.active = 1U, .hdf_level = 0U, .zf_intensity = 3U, .cfg_version = 1U};
    V71r7WysiwygOps wops = {
        .ctx = &fs,
        .query_state = mock_fs_query_state,
        .is_still_capturing = mock_fs_is_still_capturing,
        .apply_hdf_nv12 = mock_fs_apply_hdf,
        .apply_grain_nv12 = mock_fs_apply_grain
    };
    assert(v71r7_layer0_set_wysiwyg_ops(&wops) == 0);
    assert(v71r7_layer0_workspace_allocated() && g_env.ws_live_count == 1U);

    fs.still_capturing = 1;
    assert(v71r7_layer0_worker_step() == V71R7_L0_EMPTY);
    assert(!v71r7_layer0_workspace_allocated() && g_env.ws_live_count == 0U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->jpeg_pauses) >= 1U);

    /* Leaving still capture lazily re-acquires photo_workspace! */
    fs.still_capturing = 0;
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(v71r7_layer0_workspace_allocated() && g_env.ws_live_count == 1U);

    /* 8f. lut.wlm dormant gating keeps FilmSim WYSIWYG decoupled, and 0 B when both off! */
    v71r7_host_set_lut_module_loaded(0);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK); /* FilmSim WYSIWYG still keeps workspace! */
    assert(v71r7_layer0_workspace_allocated());
    fs.state.active = 0U;
    fs.state.zf_intensity = 0U;
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK); /* Both off -> retires to 0 B! */
    assert(!v71r7_layer0_workspace_allocated() && g_env.ws_live_count == 0U);
    assert(v71r7_layer0_worker_step() == V71R7_L0_EMPTY);
    assert(!v71r7_layer0_workspace_allocated() && g_env.ws_live_count == 0U);
    v71r7_host_set_lut_module_loaded(1);

    free(src1);
    free(src2);
    printf("  PASS (CONFIG_MUTEX verified, 0 rejected-bank leaks, pool_suspend force-retire, still 0 B relief, dormant 0 B)\n");
}

/* ============================================================================
 * Suite 9: Single-Pass Live->LCD Shared Frame Consumer & Split Per-Stage Telemetry
 * ============================================================================ */
static void test_suite9_r4_live_lcd_reuse_and_telemetry(void)
{
    printf("[Suite 9] V71.7-r4 Single-Pass Live->LCD reuse, zero double-grain, split telemetry & honest worker_cores...\n");
    env_reset();
    assert(v71r7_layer0_init() == 0);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->version) == 12U);
    assert(atomic_load(&v71r7_layer0_metrics_ptr()->bytes) == sizeof(Layer0Metrics));

    MockFilmSim fs;
    memset(&fs, 0, sizeof(fs));
    fs.state = (V71r7WysiwygState){.active = 1U, .hdf_level = 2U, .zf_intensity = 3U, .cfg_version = 1U};
    V71r7WysiwygOps wops = {
        .ctx = &fs,
        .query_state = mock_fs_query_state,
        .is_still_capturing = mock_fs_is_still_capturing,
        .apply_hdf_nv12 = mock_fs_apply_hdf,
        .apply_grain_nv12 = mock_fs_apply_grain
    };
    assert(v71r7_layer0_set_wysiwyg_ops(&wops) == 0);
    g_env.lut_active = 1;
    g_env.lut_gen = 9U;
    v71r7_layer0_notify_lut_changed(9U, 1);

    uint8_t *src1 = alloc_aligned_512(0x1C2000U);
    uint8_t *src2 = alloc_aligned_512(0x1C2000U);
    fill_source_frame(src1, 1024U, 682U, 30U, 128U);
    fill_source_frame(src2, 1024U, 682U, 35U, 128U);
    _Alignas(8) uint8_t pkt[0x40], desc[0x140];
    uint64_t rect = 0U, stamp = 9001U;
    build_display_desc(desc, 1024U, 682U, 0, 0xA05000000ULL, 0xA0512C000ULL);

    /* 9a. Live->LCD HIT with LUT+HDF applied by Live -> skips LUT+HDF, applies Grain ONCE! */
    g_env.live_reuse_enable = 1;
    g_env.live_reuse_seq = 1U;
    g_env.live_reuse_age_us = 10000U;
    g_env.live_reuse_stages = V71R7_L0_STAGE_LUT | V71R7_L0_STAGE_HDF;
    g_env.live_reuse_fill_y = 100U;
    g_env.live_reuse_fill_uv = 140U;
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK); /* Allocates workspace */
    env_advance_ms(20U);
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    ++stamp;
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src2, (uintptr_t)(src2 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    ++stamp;

    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    const Layer0Metrics *m = v71r7_layer0_metrics_ptr();
    assert(atomic_load(&m->live_reuse_hits) == 1U);
    assert(atomic_load(&m->live_reuse_misses) == 0U);
    assert(g_env.lut_apply_calls == 0U);
    assert(fs.hdf_calls == 0U);
    assert(fs.grain_calls == 1U); /* Grain applied ONCE on top of Live-graded frame */
    assert(atomic_load(&m->last_lut_us) == 0U && atomic_load(&m->last_hdf_us) == 0U);
    assert(v71r7_layer0_last_stages_applied() == (V71R7_L0_STAGE_LUT | V71R7_L0_STAGE_HDF | V71R7_L0_STAGE_GRAIN));

    /* Verify pixel value = live_reuse_fill_y (100) ^ zf_intensity (3) = 103 (NOT LUT/HDF twice!) */
    assert(v71r7_layer0_display_hook(0U, desc) == 1U);
    const uint8_t *out_y = (const uint8_t *)v71r7_host_ws_to_cpu((uintptr_t)g_env.stock_display_seen_y);
    assert(out_y[0] == (100U ^ 3U));

    /* 9a-dup. Same Live seq (1) again -> keep current bank (0 double-present, 0 redundant grade!) */
    env_advance_ms(16U);
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    ++stamp;
    uint32_t grades_before_dup = atomic_load(&m->grades);
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(atomic_load(&m->grades) == grades_before_dup);
    assert(atomic_load(&m->live_reuse_hits) == 1U);
    assert(atomic_load(&m->live_reuse_misses) == 0U);
    assert(g_env.lut_apply_calls == 0U && fs.hdf_calls == 0U && fs.grain_calls == 1U);

    /* 9b. Live->LCD HIT with new seq=2 where Live ALREADY applied Grain -> skips Grain too (0 double-grain!) */
    g_env.live_reuse_seq = 2U;
    g_env.live_reuse_age_us = 10000U;
    g_env.live_reuse_stages = V71R7_L0_STAGE_LUT | V71R7_L0_STAGE_HDF | V71R7_L0_STAGE_GRAIN;
    env_advance_ms(20U);
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    ++stamp;
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(atomic_load(&m->live_reuse_hits) == 2U);
    assert(fs.grain_calls == 1U); /* Unchanged -> zero double-grain! */

    /* 9b-mono. Backwards capture_us (age=64ms > 40ms STEP_IDLE_MS -> cap_ms < last_lcd_capture_ms)
     * MUST NOT jump backwards in time: falls through to live_reuse_misses and grades fresh source! */
    g_env.live_reuse_seq = 3U;
    g_env.live_reuse_age_us = 64000U; /* 64ms old vs ~40ms since 9b -> ~14ms older than last_lcd_capture_ms! */
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src2, (uintptr_t)(src2 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    ++stamp;
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(atomic_load(&m->live_reuse_hits) == 2U);
    assert(atomic_load(&m->live_reuse_misses) == 1U);
    assert(g_env.lut_apply_calls == 1U && fs.hdf_calls == 1U && fs.grain_calls == 2U);

    /* 9c. Live->LCD MISS -> normal pipeline + honest worker_cores (1 vs 2) + helper_sentinel[0..2] */
    g_env.live_reuse_enable = 0;
    g_env.smp_online_helpers = 0U; /* 0 helpers online -> worker_cores MUST report 1 (not lie 2!) */
    g_env.smp_sentinels[0] = 1U;
    g_env.smp_sentinels[1] = 0U;
    g_env.smp_sentinels[2] = 0U;
    env_advance_ms(20U);
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src2, (uintptr_t)(src2 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    ++stamp;
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(atomic_load(&m->live_reuse_misses) == 2U);
    assert(g_env.lut_apply_calls == 2U && fs.hdf_calls == 2U && fs.grain_calls == 3U);
    assert(atomic_load(&m->worker_cores) == 1U);
    assert(atomic_load(&m->smp_helpers_online) == 0U);
    assert(atomic_load(&m->helper_sentinel[0]) == 1U);

    /* With 2 SMP helpers online -> worker_cores honestly reports 2 & sentinels populate! */
    g_env.smp_online_helpers = 2U;
    g_env.smp_sentinels[0] = 1U;
    g_env.smp_sentinels[1] = 1U;
    g_env.smp_sentinels[2] = 1U;
    env_advance_ms(20U);
    build_enqueue_packet(pkt, &rect, 1024U, 682U, 0U, (uintptr_t)src1, (uintptr_t)(src1 + 0x12C000U));
    assert(v71r7_layer0_enqueue_hook(0U, pkt, &rect, &stamp, (void *)v71r7_host_stock_complete_cb()) == 1U);
    ++stamp;
    assert(v71r7_layer0_worker_step() == V71R7_L0_OK);
    assert(atomic_load(&m->worker_cores) == 2U);
    assert(atomic_load(&m->smp_helpers_online) == 2U);
    assert(atomic_load(&m->helper_sentinel[0]) == 1U &&
           atomic_load(&m->helper_sentinel[1]) == 1U &&
           atomic_load(&m->helper_sentinel[2]) == 1U);

    free(src1);
    free(src2);
    printf("  PASS (Live->LCD reuse hit/miss, 0 double-grain, split telemetry, honest worker_cores=1/2 & helper_sentinel[0..2])\n");
}

int main(void)
{
    test_suite1_dormant_and_early_boot();
    test_suite2_workspace_spacer_and_quarantine();
    test_suite3_wysiwyg_pipeline_and_fences();
    test_suite4_hangfix_v69_pool_suspend_resume();
    test_suite5_power_sync_and_lifecycle_stop();
    test_suite6_hook_table_preimages();
    test_suite7_hung_xm6_dma_quarantine();
    test_suite8_r4_stability_mutex_unpin_suspend_still();
    test_suite9_r4_live_lcd_reuse_and_telemetry();

    env_reset(); /* Final clean teardown for LeakSanitizer */
    printf("\nALL 9 V71.7-r4 LAYER 0 HOST SUITES PASSED (0 errors, 0 leaks)!\n");
    return 0;
}
