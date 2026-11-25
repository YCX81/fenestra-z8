/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Display Layer 0 WYSIWYG Contiguous 14.55 MiB (0xE8C000) Class 0x24 Workspace.
 *
 * Strictly freestanding C11, scalar-only (-mgeneral-regs-only), ZERO .data/.bss.
 */
#include "photo_workspace.h"
#include "../lut_core/v71r7_lut_abi.h"

#include <stddef.h>
#include <stdint.h>

enum {
    WS_EMPTY       = 0U,
    WS_BUILDING    = 1U << 24,
    WS_CANCEL_BUILD= 2U << 24,
    WS_OPEN        = 3U << 24,
    WS_CLOSING     = 4U << 24,
    WS_RELEASING   = 5U << 24,
    WS_QUARANTINED = 6U << 24,
    WS_PAD         = 512U
};

#define WS_PINS UINT32_C(0xFFFFFF)

#ifndef V71R7_HOST_TEST

#define WS_ALLOC      ((uintptr_t (*)(uint32_t, uint32_t, uint32_t))(uintptr_t)0x4104d2dcU)
#define WS_FREE       ((uint32_t (*)(uintptr_t))(uintptr_t)0x4104d600U)
#define WS_TO_CPU     ((uintptr_t (*)(uintptr_t))(uintptr_t)0x41186718U)
#define WS_TO_RAW     ((uintptr_t (*)(uintptr_t))(uintptr_t)0x41186780U)
#define WS_CACHEABLE  ((uint32_t (*)(uintptr_t))(uintptr_t)0x41186810U)
#define WS_CLEAN      ((void (*)(uintptr_t, uint32_t, uint32_t))(uintptr_t)0x40d2b9e4U)
#define WS_INVALIDATE ((void (*)(uintptr_t, uint32_t, uint32_t))(uintptr_t)0x40d2b95cU)
#define WS_MTX_LOCK   ((int (*)(void *))(uintptr_t)0x40d2c674U)
#define WS_MTX_UNLOCK ((int (*)(void *))(uintptr_t)0x40d2c660U)
#define WS_REGIONS    ((uintptr_t)0xa0497d030ULL)
#define WS_DELTA      ((uintptr_t)0x400000000ULL)

/* Verified Z8 C3.11 Class 0x24 pool-state & CONFIG_MUTEX addresses
 * (matches z8_native_photo_platform.c:170-195). */
#define WS_CONFIG_MUTEX  UINT64_C(0xa03f31648)
#define WS_CLASS24_STATE (UINT64_C(0xa02f3e2c0) + 0x24U * UINT64_C(0x5ec8))

static uint8_t ws_u8(uintptr_t p) { return *(const volatile uint8_t *)p; }
static uint32_t ws_u32(uintptr_t p) { return *(const volatile uint32_t *)p; }
static uint64_t ws_u64(uintptr_t p) { return *(const volatile uint64_t *)p; }

static int ws_config_lock(void)
{
    return WS_MTX_LOCK((void *)(uintptr_t)WS_CONFIG_MUTEX) != 0;
}

static void ws_config_unlock(void)
{
    (void)WS_MTX_UNLOCK((void *)(uintptr_t)WS_CONFIG_MUTEX);
}

static void ws_barrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint16_t ws_core_id(void)
{
    uint64_t mpidr = 0u;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint16_t)((mpidr & 15u) + 1u);
}

static int ws_object(uintptr_t p)
{
    return !(p & 7U) && p >= UINT64_C(0xa00000000) && p <= UINT64_C(0xa40000000) - 0x80U;
}

static int ws_pool_valid(void)
{
    return ws_u32(UINT64_C(0xa03f31a68)) == 0U &&
           ws_u8(UINT64_C(0xa03f31b4c)) == 0U &&
           !(ws_u8(UINT64_C(0xa03f31d08)) & 1U) &&
           ws_u64(UINT64_C(0xa03f317e0)) != 0U &&
           ws_u8(WS_CLASS24_STATE + 0x5ec0U) == 0U;
}

/* Re-verified strictly inside WS_CONFIG_MUTEX (eliminates TOCTOU against stock
 * 0x410463e4 / 0x41047014 layout switches & deferred Class 0x24 teardown). */
static int ws_pool_valid_locked(void)
{
    if (!ws_pool_valid() || ws_u8(UINT64_C(0xa02f3e2b8)) != 0U ||
        ws_u32(WS_CLASS24_STATE + 0x5e3cU) == 0U) {
        return 0;
    }
    uint64_t owner = ws_u64(UINT64_C(0xa03f317e0));
    uint32_t capacity = ws_u32(WS_CLASS24_STATE + 8U);
    uint64_t policy = ws_u64(UINT64_C(0xa03f316b8));
    if (owner != ws_u64(WS_CLASS24_STATE) || capacity == 0U ||
        owner < UINT64_C(0x840000000) || owner >= UINT64_C(0x8fff47400) ||
        (uint64_t)capacity > UINT64_C(0x8fff47400) - owner) {
        return 0;
    }
    if (policy < UINT64_C(0xa00000000) || policy > UINT64_C(0xa40000000) - 0x94U) {
        return 0;
    }
    return ws_u8((uintptr_t)(policy + 0x90U)) == 1U &&
           ws_u8((uintptr_t)(policy + 0x92U)) == 0U &&
           ws_u8((uintptr_t)(policy + 0x93U)) == 0U;
}

/* Contiguous attribute table used by stock Z8 C3.11 address conversions (24 entries). */
static int ws_one_region(uintptr_t start, uintptr_t end, uint32_t kind)
{
    for (unsigned i = 0U; i < 23U; ++i) {
        uintptr_t row = WS_REGIONS + (uintptr_t)i * 16U;
        if (start >= (uintptr_t)ws_u64(row) && end < (uintptr_t)ws_u64(row + 16U)) {
            return ws_u32(row + 8U) == kind;
        }
    }
    return 0;
}

static int ws_mapped(uintptr_t raw, uint32_t n)
{
    uintptr_t a = raw - 64U;
    uintptr_t b = raw + n + 63U;
    uintptr_t c = a + WS_DELTA;
    uintptr_t d = b + WS_DELTA;
    return ws_one_region(a, b, 1U) && ws_one_region(c, d, 0U) &&
           WS_TO_CPU(a) == c && WS_TO_CPU(b) == d &&
           WS_TO_RAW(c) == a && WS_TO_RAW(d) == b &&
           !WS_CACHEABLE(a) && !WS_CACHEABLE(b) &&
           WS_CACHEABLE(c) && WS_CACHEABLE(d);
}

/* Bidirectional cache coherency verification on private padding lines outside payload. */
static int ws_verify_line(uintptr_t raw)
{
    uintptr_t cpu = raw + WS_DELTA;
    volatile uint64_t *r = (volatile uint64_t *)raw;
    volatile uint64_t *c = (volatile uint64_t *)cpu;
    uint64_t original = *r;
    uint64_t first = original ^ UINT64_C(0x75612cc68e2975b1);

    *r = first;
    ws_barrier();
    WS_INVALIDATE(cpu, 64U, 0U);
    ws_barrier();
    int pass = (*c == first);

    *r = original;
    ws_barrier();
    WS_INVALIDATE(cpu, 64U, 0U);
    ws_barrier();
    if (!pass || *c != original) {
        return 0;
    }

    *c = first;
    WS_CLEAN(cpu, 64U, 0U);
    ws_barrier();
    pass = (*r == first);

    *r = original;
    ws_barrier();
    WS_INVALIDATE(cpu, 64U, 0U);
    ws_barrier();
    return pass && (*c == original);
}

static int ws_inspect_handle(uintptr_t handle, uintptr_t *base_out, uint32_t *cap_out)
{
    uintptr_t owner = (ws_object(handle) && ws_u32(handle + 0x10U) == 0x24U)
                          ? (uintptr_t)ws_u64(handle + 8U)
                          : 0U;
    if (!ws_object(owner) || (uintptr_t)ws_u64(owner + 0x10U) != handle) {
        return 0; /* Malformed/untyped handle -> caller MUST quarantine */
    }
    *base_out = (uintptr_t)ws_u64(owner);
    *cap_out = ws_u32(owner + 8U);
    return 1;
}

static int ws_in_class24(uintptr_t base, uint32_t capacity)
{
    return capacity != 0U && capacity <= 0x20000000U &&
           base >= UINT64_C(0x840000000) &&
           base <= UINT64_C(0x8c0000000) - capacity;
}

static int ws_within_single_512m_window(uintptr_t raw, uint32_t span)
{
    if (!span) {
        return 0;
    }
    uint64_t a = (uint64_t)raw;
    uint64_t b = a + (uint64_t)(span - 1U);
    return (a >> 29) == (b >> 29);
}

static uintptr_t ws_raw_to_cpu_addr(uintptr_t raw)
{
    return raw + WS_DELTA;
}

#else /* V71R7_HOST_TEST */

#define WS_ALLOC(a, b, n)         v71r7_host_ws_alloc((a), (b), (n))
#define WS_FREE(h)                v71r7_host_ws_free((h))
#define WS_TO_CPU(a)              v71r7_host_ws_to_cpu((a))
#define WS_TO_RAW(a)              v71r7_host_ws_to_raw((a))
#define WS_CACHEABLE(a)           v71r7_host_ws_cacheable((a))
#define WS_CLEAN(a, n, m)         v71r7_host_ws_clean((a), (n), (m))
#define WS_INVALIDATE(a, n, m)    v71r7_host_ws_invalidate((a), (n), (m))

static int ws_config_lock(void) { return v71r7_host_ws_config_lock(); }
static void ws_config_unlock(void) { v71r7_host_ws_config_unlock(); }
static void ws_barrier(void) { atomic_thread_fence(memory_order_seq_cst); }
static uint16_t ws_core_id(void) { return 1U; }
static int ws_pool_valid(void) { return v71r7_host_ws_pool_valid(); }
static int ws_pool_valid_locked(void) { return v71r7_host_ws_pool_valid(); }

static int ws_mapped(uintptr_t raw, uint32_t n)
{
    uintptr_t a = raw - 64U;
    uintptr_t b = raw + n + 63U;
    uintptr_t c = WS_TO_CPU(a);
    uintptr_t d = WS_TO_CPU(b);
    return v71r7_host_ws_one_region(a, b, 1U) && v71r7_host_ws_one_region(c, d, 0U) &&
           WS_TO_RAW(c) == a && WS_TO_RAW(d) == b &&
           !WS_CACHEABLE(a) && !WS_CACHEABLE(b) &&
           WS_CACHEABLE(c) && WS_CACHEABLE(d);
}

static int ws_verify_line(uintptr_t raw)
{
    return v71r7_host_ws_verify_line(raw);
}

static int ws_inspect_handle(uintptr_t handle, uintptr_t *base_out, uint32_t *cap_out)
{
    uint32_t cls = 0U;
    uintptr_t owner = 0U, owner_back = 0U, base = 0U;
    uint32_t cap = 0U;
    if (!v71r7_host_ws_inspect_handle(handle, &cls, &owner, &owner_back, &base, &cap)) {
        return 0;
    }
    if (cls != 0x24U || !owner || owner_back != handle) {
        return 0;
    }
    *base_out = base;
    *cap_out = cap;
    return 1;
}

static int ws_in_class24(uintptr_t base, uint32_t capacity)
{
    return v71r7_host_ws_in_class24(base, capacity);
}

static int ws_within_single_512m_window(uintptr_t raw, uint32_t span)
{
    if (!span) {
        return 0;
    }
    uint64_t a = v71r7_host_ws_dma_window_addr(raw);
    uint64_t b = a + (uint64_t)(span - 1U);
    return (a >> 29) == (b >> 29);
}

static uintptr_t ws_raw_to_cpu_addr(uintptr_t raw)
{
    return WS_TO_CPU(raw);
}

#endif /* V71R7_HOST_TEST */

void photo_workspace_dcache_clean_chunked(uintptr_t cpu_addr, uint32_t bytes)
{
    if (!cpu_addr || !bytes) {
        return;
    }
    uintptr_t cur = cpu_addr;
    uint32_t rem = bytes;
    while (rem > 0U) {
        uint32_t chunk = rem > PHOTO_WORKSPACE_DCACHE_CHUNK ? PHOTO_WORKSPACE_DCACHE_CHUNK : rem;
        WS_CLEAN(cur, chunk, 0U);
        cur += chunk;
        rem -= chunk;
    }
    ws_barrier();
}

void photo_workspace_dcache_invalidate_chunked(uintptr_t cpu_addr, uint32_t bytes)
{
    if (!cpu_addr || !bytes) {
        return;
    }
    uintptr_t cur = cpu_addr;
    uint32_t rem = bytes;
    while (rem > 0U) {
        uint32_t chunk = rem > PHOTO_WORKSPACE_DCACHE_CHUNK ? PHOTO_WORKSPACE_DCACHE_CHUNK : rem;
        WS_INVALIDATE(cur, chunk, 0U);
        cur += chunk;
        rem -= chunk;
    }
    ws_barrier();
}

V71R7_BANK2_COLD static uint32_t ws_free_locked(uintptr_t handle)
{
    if (!handle) {
        return 1U;
    }
    if (!ws_config_lock()) {
        return 0U;
    }
    uint32_t rc = WS_FREE(handle);
    ws_config_unlock();
    return rc;
}

V71R7_BANK2_COLD static int ws_give(PhotoWorkspace *ws)
{
    if (ws->cpu) {
        uint32_t span = (ws->bytes + 63U) & ~63U;
        photo_workspace_dcache_clean_chunked(ws->cpu, span);
        photo_workspace_dcache_invalidate_chunked(ws->cpu, span);
    }
    if (ws->handle && ws_free_locked(ws->handle) != 1U) {
        atomic_store_explicit(&ws->state, WS_QUARANTINED, memory_order_release);
        return PHOTO_WS_RETAINED;
    }
    ws->handle = 0U;
    ws->raw = 0U;
    ws->cpu = 0U;
    ws->bytes = 0U;
    ws->xm6_window_ok = 0U;
    atomic_store_explicit(&ws->state, WS_EMPTY, memory_order_release);
    return PHOTO_WS_OK;
}

typedef struct {
    uintptr_t handle;
    uintptr_t raw;
    int       malformed;
    int       bounds_ok;
    int       pool_busy;
} WsCandidate;

V71R7_BANK2_COLD static WsCandidate ws_alloc_and_check(uint32_t span)
{
    WsCandidate c = {0U, 0U, 0, 0, 0};
    if (!ws_config_lock()) {
        c.pool_busy = 1;
        return c;
    }
    if (!ws_pool_valid_locked()) {
        ws_config_unlock();
        c.pool_busy = 1;
        return c;
    }
    uintptr_t handle = WS_ALLOC(0U, 0x6bU, span + 3U * WS_PAD);
    ws_config_unlock();
    if (!handle) {
        return c;
    }
    c.handle = handle;
    uintptr_t base = 0U;
    uint32_t capacity = 0U;
    if (!ws_inspect_handle(handle, &base, &capacity)) {
        c.malformed = 1;
        return c;
    }
    if (ws_in_class24(base, capacity)) {
        uintptr_t raw = (base + 2U * WS_PAD - 1U) & ~(uintptr_t)(WS_PAD - 1U);
        if (raw >= base && raw + span + WS_PAD <= base + capacity) {
            c.raw = raw;
            c.bounds_ok = 1;
        }
    }
    return c;
}

V71R7_BANK2_COLD int photo_workspace_acquire_ex(PhotoWorkspace *ws, uint32_t bytes, uint32_t flags)
{
    if (!ws || !bytes || bytes > PHOTO_WORKSPACE_MAX_BYTES) {
        return PHOTO_WS_ARGUMENT;
    }
    uint32_t expected = WS_EMPTY;
    uint32_t span = (bytes + 63U) & ~63U;
    if (!atomic_compare_exchange_strong(&ws->state, &expected, WS_BUILDING)) {
        return expected == WS_QUARANTINED ? PHOTO_WS_RETAINED : PHOTO_WS_BUSY;
    }
    if (!ws_pool_valid()) {
        atomic_store_explicit(&ws->state, WS_EMPTY, memory_order_release);
        return PHOTO_WS_BUSY;
    }

    WsCandidate primary = ws_alloc_and_check(span);
    if (!primary.handle) {
        atomic_store_explicit(&ws->state, WS_EMPTY, memory_order_release);
        return primary.pool_busy ? PHOTO_WS_BUSY : PHOTO_WS_MEMORY;
    }
    if (primary.malformed) {
        ws->handle = primary.handle;
        atomic_store_explicit(&ws->state, WS_QUARANTINED, memory_order_release);
        return PHOTO_WS_RETAINED;
    }

    WsCandidate chosen = primary;
    int xm6_ok = chosen.bounds_ok && ws_within_single_512m_window(chosen.raw, span);

    /* Optional 512 MiB window-straddle spacer retry for XM6 Core 0 LCD HDF (Slot 41).
     * Holding the straddling primary handle forces the Class 0x24 allocator past the
     * 512 MiB boundary on the second call, then frees the spacer immediately. */
    if ((flags & PHOTO_WS_FLAG_XM6_512M_GUARD) && chosen.bounds_ok && !xm6_ok) {
        ++ws->spacer_retries;
        v71r7_flight_record(FX_Z8_FLIGHT_XM6_SPACER, ws_core_id(),
                            (uint32_t)chosen.raw, span, ws->spacer_retries);
        WsCandidate retry = ws_alloc_and_check(span);
        if (retry.malformed) {
            /* Untyped handle on retry: free valid primary if possible, quarantine retry. */
            (void)ws_free_locked(primary.handle);
            ws->handle = retry.handle;
            atomic_store_explicit(&ws->state, WS_QUARANTINED, memory_order_release);
            return PHOTO_WS_RETAINED;
        }
        if (retry.handle && retry.bounds_ok && ws_within_single_512m_window(retry.raw, span)) {
            if (ws_free_locked(primary.handle) != 1U) {
                /* Spacer free failed: release retry and quarantine primary handle. */
                (void)ws_free_locked(retry.handle);
                ws->handle = primary.handle;
                atomic_store_explicit(&ws->state, WS_QUARANTINED, memory_order_release);
                return PHOTO_WS_RETAINED;
            }
            chosen = retry;
            xm6_ok = 1;
        } else if (retry.handle) {
            /* Retry still straddled or bad bounds: free retry, keep valid primary (ARM fallback). */
            if (ws_free_locked(retry.handle) != 1U) {
                (void)ws_free_locked(primary.handle);
                ws->handle = retry.handle;
                atomic_store_explicit(&ws->state, WS_QUARANTINED, memory_order_release);
                return PHOTO_WS_RETAINED;
            }
        }
    }

    ws->handle = chosen.handle;
    int result = PHOTO_WS_MEMORY;
    if (chosen.bounds_ok) {
        result = PHOTO_WS_MAPPING;
        if (ws_mapped(chosen.raw, span) &&
            ws_verify_line(chosen.raw - 64U) &&
            ws_verify_line(chosen.raw + span)) {
            ws->raw = chosen.raw;
            ws->cpu = ws_raw_to_cpu_addr(chosen.raw);
            ws->bytes = bytes;
            ws->xm6_window_ok = xm6_ok ? 1U : 0U;
            photo_workspace_dcache_invalidate_chunked(ws->cpu, span);
            expected = WS_BUILDING;
            if (atomic_compare_exchange_strong(&ws->state, &expected, WS_OPEN)) {
                return PHOTO_WS_OK;
            }
            result = PHOTO_WS_BUSY; /* Concurrent release latched CANCEL_BUILD while building. */
        }
    }

    atomic_store_explicit(&ws->state, WS_RELEASING, memory_order_release);
    return ws_give(ws) == PHOTO_WS_OK ? result : PHOTO_WS_RETAINED;
}

V71R7_BANK2_COLD int photo_workspace_acquire(PhotoWorkspace *ws, uint32_t bytes)
{
    return photo_workspace_acquire_ex(ws, bytes, PHOTO_WS_FLAG_NONE);
}

V71R7_BANK2_COLD int photo_workspace_pin(PhotoWorkspace *ws, PhotoWorkspaceView *view)
{
    if (!ws || !view) {
        return PHOTO_WS_ARGUMENT;
    }
    view->cpu = NULL;
    view->raw = 0U;
    view->bytes = 0U;
    view->xm6_window_ok = 0U;

    uint32_t s = atomic_load_explicit(&ws->state, memory_order_acquire);
    while ((s & ~WS_PINS) == WS_OPEN && (s & WS_PINS) != WS_PINS) {
        if (atomic_compare_exchange_weak(&ws->state, &s, s + 1U)) {
            view->cpu = (uint8_t *)ws->cpu;
            view->raw = ws->raw;
            view->bytes = ws->bytes;
            view->xm6_window_ok = ws->xm6_window_ok;
            return PHOTO_WS_OK;
        }
    }
    return s == WS_QUARANTINED ? PHOTO_WS_RETAINED : PHOTO_WS_BUSY;
}

V71R7_BANK2_COLD int photo_workspace_unpin(PhotoWorkspace *ws)
{
    if (!ws) {
        return PHOTO_WS_ARGUMENT;
    }
    uint32_t s = atomic_load_explicit(&ws->state, memory_order_acquire);
    while (((s & ~WS_PINS) == WS_OPEN || (s & ~WS_PINS) == WS_CLOSING) && (s & WS_PINS)) {
        if (atomic_compare_exchange_weak(&ws->state, &s, s - 1U)) {
            return PHOTO_WS_OK;
        }
    }
    return PHOTO_WS_ARGUMENT;
}

V71R7_BANK2_COLD int photo_workspace_release(PhotoWorkspace *ws)
{
    if (!ws) {
        return PHOTO_WS_ARGUMENT;
    }
    uint32_t s = atomic_load_explicit(&ws->state, memory_order_acquire);
    for (;;) {
        if (s == WS_EMPTY) {
            return PHOTO_WS_OK;
        }
        if (s == WS_QUARANTINED) {
            return PHOTO_WS_RETAINED;
        }
        if (s == WS_BUILDING) {
            if (atomic_compare_exchange_weak(&ws->state, &s, WS_CANCEL_BUILD)) {
                return PHOTO_WS_BUSY;
            }
        } else if ((s & ~WS_PINS) == WS_OPEN) {
            uint32_t closing = WS_CLOSING | (s & WS_PINS);
            if (atomic_compare_exchange_weak(&ws->state, &s, closing)) {
                s = closing;
            }
        } else if (s == WS_CLOSING) {
            if (atomic_compare_exchange_weak(&ws->state, &s, WS_RELEASING)) {
                return ws_give(ws);
            }
        } else {
            return PHOTO_WS_BUSY;
        }
    }
}

int photo_workspace_is_allocated(const PhotoWorkspace *ws)
{
    if (!ws) {
        return 0;
    }
    return atomic_load_explicit(&ws->state, memory_order_acquire) != WS_EMPTY;
}

int photo_workspace_is_quarantined(const PhotoWorkspace *ws)
{
    if (!ws) {
        return 0;
    }
    return atomic_load_explicit(&ws->state, memory_order_acquire) == WS_QUARANTINED;
}
