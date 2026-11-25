/* SPDX-License-Identifier: MIT
 * Z8 C3.11 / V71.7 platform class24 allocator (incl. XM6 512 MiB spacer retry),
 * D-cache maintenance, and safe RAM reader.
 * Zero NEF metadata write helpers (NEF RAW strictly zero-touch).
 * Freestanding C11, scalar-only (-mgeneral-regs-only), zero .data/.bss.
 */
#include "z8_native_photo_platform.h"
#include <string.h>

#define FN(a, t) ((t)(uintptr_t)UINT64_C(a))
#define R8(a)    (*(volatile uint8_t *)(uintptr_t)(a))
#define R32(a)   (*(volatile uint32_t *)(uintptr_t)(a))
#define R64(a)   (*(volatile uint64_t *)(uintptr_t)(a))

#define CONFIG_MUTEX            UINT64_C(0xa03f31648)
#define CLASS24_STATE           (UINT64_C(0xa02f3e2c0) + 0x24u * UINT64_C(0x5ec8))
#define Z8_FLIGHT_XM6_SPACER_ID 20u /* Matches FX_Z8_FLIGHT_XM6_SPACER in v71r7_lut_abi.h */

#if defined(V71R7_HOST_TEST) || !defined(FILMSIM_HOST_TEST)
extern void v71r7_flight_record(uint16_t event, uint16_t core_flags,
                                uint32_t a, uint32_t b, uint32_t c);
#endif

static inline void plat_log_spacer(uint64_t spacer_dma, uint32_t spacer_actual,
                                   uint64_t fresh_dma, int ok)
{
#if defined(V71R7_HOST_TEST) || !defined(FILMSIM_HOST_TEST)
    v71r7_flight_record(Z8_FLIGHT_XM6_SPACER_ID, (uint16_t)(ok ? 1u : 0u),
                        (uint32_t)(spacer_dma >> 12), spacer_actual,
                        ok ? (uint32_t)(fresh_dma >> 12) : 0u);
#else
    (void)spacer_dma;
    (void)spacer_actual;
    (void)fresh_dma;
    (void)ok;
#endif
}

static int ram_ok(uint64_t a, size_t n)
{
    static const uint64_t bounds[][2] = {
        {UINT64_C(0xa00000000), UINT64_C(0xa40000000)},
        {UINT64_C(0x840000000), UINT64_C(0x8adf8da00)},
        {UINT64_C(0x8adf8dc00), UINT64_C(0x8fff47400)},
        {UINT64_C(0xc40000000), UINT64_C(0xcadf8da00)},
        {UINT64_C(0xcadf8dc00), UINT64_C(0xcfff47400)}
    };
    if (!n) {
        return 0;
    }
    for (unsigned i = 0u; i < sizeof(bounds) / sizeof(bounds[0]); ++i) {
        if (a >= bounds[i][0] && a < bounds[i][1] && n <= bounds[i][1] - a) {
            return 1;
        }
    }
    return 0;
}

#ifndef FILMSIM_HOST_TEST

static int read_native(void *o, uint64_t a, void *d, size_t n)
{
    (void)o;
    if (!d || !ram_ok(a, n)) {
        return 0;
    }
    const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)a;
    for (size_t i = 0u; i < n; ++i) {
        ((uint8_t *)d)[i] = s[i];
    }
    return 1;
}

int z8_fs_plat_pool_valid(void)
{
    return R32(UINT64_C(0xa03f31a68)) == 0u &&
           R8(UINT64_C(0xa03f31b4c)) == 0u &&
           !(R8(UINT64_C(0xa03f31d08)) & 1u) &&
           R64(UINT64_C(0xa03f317e0)) != 0u &&
           R8(CLASS24_STATE + 0x5ec0u) == 0u;
}

static int pool_valid_cb(void *o)
{
    (void)o;
    return z8_fs_plat_pool_valid();
}

static uint8_t *cpu_alias(uint64_t dma, uint32_t bytes)
{
    if (!ram_ok(dma, bytes) || dma >= UINT64_C(0xc00000000)) {
        return 0;
    }
    uint64_t a = FN(0x41186718, uint64_t (*)(uint64_t))(dma);
    if ((a != dma && a - dma != UINT64_C(0x400000000)) || !ram_ok(a, bytes)) {
        return 0;
    }
    return (uint8_t *)(uintptr_t)a;
}

int z8_fs_plat_clean_dcache(uint8_t *p, uint32_t bytes)
{
    if (!p || !bytes) {
        return 0;
    }
    uintptr_t start = (uintptr_t)p & ~UINT64_C(63);
    uintptr_t raw = (uintptr_t)p;
    if (raw > UINTPTR_MAX - bytes || raw + bytes > UINTPTR_MAX - 63u) {
        return 0;
    }
    uintptr_t end = (raw + bytes + 63u) & ~UINT64_C(63);
    size_t span = end - start;
    if (!ram_ok(start, span)) {
        return 0;
    }
    uint8_t *cur = (uint8_t *)start;
    /* Chunk strictly below native 0x88000 assert guard: 0x80000 = 512 KiB (8192 cache lines). */
    while (span > 0u) {
        uint32_t n = span > 0x80000u ? 0x80000u : (uint32_t)span;
        FN(0x40d2b9c8, int (*)(void *, uint32_t))(cur, n);
        cur += n;
        span -= n;
    }
    return 1;
}

static int clean_cb(void *o, uint8_t *p, uint32_t bytes)
{
    (void)o;
    return z8_fs_plat_clean_dcache(p, bytes);
}

void z8_fs_plat_release_class24(struct z8_nfsp_allocation *b)
{
    if (!b) {
        return;
    }
    if (b->handle) {
        int already_cleaned = (b->used & Z8_NFSP_USED_CLEANED_BIT) != 0u;
        uint32_t raw_used = b->used & ~Z8_NFSP_USED_CLEANED_BIT;
        uint32_t flush_bytes = raw_used ? raw_used : b->actual;
        if (!already_cleaned && b->cpu && flush_bytes) {
            (void)z8_fs_plat_clean_dcache(b->cpu, flush_bytes);
        }
        FN(0x4104d600, void (*)(void *))(b->handle);
    }
    memset(b, 0, sizeof(*b));
}

static void release_cb(void *o, struct z8_nfsp_allocation *b)
{
    (void)o;
    z8_fs_plat_release_class24(b);
}

int z8_fs_plat_alloc_class24(uint32_t requested, struct z8_nfsp_allocation *b)
{
    uint64_t resource = 0u, dma = 0u, owner, policy;
    uint32_t actual = 0u, cls = 0u, capacity;
    uint8_t flags[4];
    int valid = 0;

    if (!b) {
        return 0;
    }
    memset(b, 0, sizeof(*b));
    if (!requested || !z8_fs_plat_pool_valid()) {
        return 0;
    }
    if (!FN(0x40d2c674, int (*)(void *))((void *)(uintptr_t)CONFIG_MUTEX)) {
        return 0;
    }

    if (z8_fs_plat_pool_valid() && !R8(UINT64_C(0xa02f3e2b8)) &&
        R32(CLASS24_STATE + 0x5e3cu) != 0u) {
        owner = R64(UINT64_C(0xa03f317e0));
        capacity = R32(CLASS24_STATE + 8u);
        policy = R64(UINT64_C(0xa03f316b8));
        if (owner == R64(CLASS24_STATE) && ram_ok(owner, capacity) &&
            requested <= capacity &&
            policy <= UINT64_MAX - 0x94u &&
            read_native(0, policy + 0x90u, flags, sizeof(flags)) &&
            flags[0] == 1u && !flags[2] && !flags[3]) {
            b->handle = FN(0x41047478, void *(*)(uint32_t, uint32_t, uint32_t))(
                0x4653u, 0x24u, requested);
            valid = b->handle &&
                    read_native(0, (uintptr_t)b->handle + 8u, &resource, 8u) &&
                    read_native(0, (uintptr_t)b->handle + 0x10u, &cls, 4u) && cls == 0x24u &&
                    read_native(0, resource, &dma, 8u) &&
                    read_native(0, resource + 8u, &actual, 4u) &&
                    actual && (dma & 511u) == 0u &&
                    dma >= owner && dma - owner <= capacity &&
                    actual <= (uint64_t)capacity - (dma - owner);
        }
    }
    FN(0x40d2c660, int (*)(void *))((void *)(uintptr_t)CONFIG_MUTEX);

    if (!valid || !z8_fs_plat_pool_valid() ||
        !(b->cpu = cpu_alias(dma, actual)) || ((uintptr_t)b->cpu & 511u) != 0u) {
        z8_fs_plat_release_class24(b);
        return 0;
    }
    b->dma = dma;
    b->actual = actual;
    return 1;
}

static int allocate_cb(void *o, uint32_t requested, struct z8_nfsp_allocation *b)
{
    (void)o;
    /* V71.7-r4: Allocate once from Class 0x24 so OOM never hammers Selector 0 twice and
     * large 45MP still blocks (>24 MiB) NEVER trigger a 2x transient spacer spike (188 MiB)
     * that could starve stock PhotoDevelop (0x40eac56c unchecked NULL deref). If a large
     * block straddles 512 MiB, keep it directly for ARM NEON (XM6 cleanly falls back OOB). */
    if (!z8_fs_plat_alloc_class24(requested, b)) {
        return 0;
    }
    if (!z8_fs_plat_span_straddles_512m(b->dma, b->actual) ||
        requested > Z8_FS_XM6_SPACER_MAX_BYTES) {
        return 1;
    }
    struct z8_nfsp_allocation fresh;
    memset(&fresh, 0, sizeof(fresh));
    int fresh_ok = z8_fs_plat_alloc_class24(requested, &fresh);
    if (fresh_ok && !z8_fs_plat_span_straddles_512m(fresh.dma, fresh.actual)) {
        uint64_t spacer_dma = b->dma;
        uint32_t spacer_actual = b->actual;
        b->used |= Z8_NFSP_USED_CLEANED_BIT;
        z8_fs_plat_release_class24(b);
        plat_log_spacer(spacer_dma, spacer_actual, fresh.dma, 1);
        *b = fresh;
        return 1;
    }
    if (fresh_ok) {
        fresh.used |= Z8_NFSP_USED_CLEANED_BIT;
        z8_fs_plat_release_class24(&fresh);
    }
    plat_log_spacer(b->dma, b->actual, 0u, 0);
    /* Keep original single allocation so ARM NEON still grades cleanly when Class 0x24 is tight. */
    return 1;
}

static const uint8_t *map_source_cb(void *o, uint64_t dma, uint32_t bytes)
{
    (void)o;
    if (!bytes || (dma & 3u) != 0u) {
        return 0;
    }
    uint64_t line_base = dma & ~UINT64_C(63);
    if (dma > UINT64_MAX - bytes || dma + bytes > UINT64_MAX - 63u) {
        return 0;
    }
    uint64_t line_end = (dma + bytes + 63u) & ~UINT64_C(63);
    uint64_t span = line_end - line_base;
    if (span == 0u || span >= 0x88000u) {
        return 0;
    }
    uint8_t *cpu = cpu_alias(line_base, (uint32_t)span);
    if (!cpu) {
        return 0;
    }
    FN(0x40d2b940, int (*)(void *, uint32_t))(cpu, (uint32_t)span);
    return cpu + (dma - line_base);
}

static void yield_cb(void *o)
{
    (void)o;
    /* V71: dly_tsk(0) (la_native_yield, 0..1ms) paired with 16ms time-slicing in cancelled_cb() */
    FN(0x40187b00, int (*)(uint32_t))(0);
}

uint64_t z8_fs_plat_time_us(void)
{
    uint64_t tick = 0u, hz = 0u;
    __asm__ volatile("mrs %0, cntpct_el0; mrs %1, cntfrq_el0" : "=r"(tick), "=r"(hz));
    return hz ? tick / hz * 1000000u + (tick % hz) * 1000000u / hz : 0u;
}

static uint64_t time_us_cb(void *o)
{
    (void)o;
    return z8_fs_plat_time_us();
}

void z8_nfsp_v66_ops(struct z8_nfsp_ops *ops)
{
    if (!ops) {
        return;
    }
    memset(ops, 0, sizeof(*ops));
    ops->read = read_native;
    ops->allocate = allocate_cb;
    ops->release = release_cb;
    ops->map_source = map_source_cb;
    ops->clean = clean_cb;
    ops->pool_valid = pool_valid_cb;
    ops->yield = yield_cb;
    ops->time_us = time_us_cb;
}

#else /* FILMSIM_HOST_TEST */

int z8_fs_plat_pool_valid(void) { return 0; }
int z8_fs_plat_clean_dcache(uint8_t *p, uint32_t bytes) { (void)p; (void)bytes; return ram_ok(0, 0); }
void z8_fs_plat_release_class24(struct z8_nfsp_allocation *b)
{
    if (b) {
        memset(b, 0, sizeof(*b));
    }
}
int z8_fs_plat_alloc_class24(uint32_t requested, struct z8_nfsp_allocation *b)
{
    (void)requested;
    if (b) {
        memset(b, 0, sizeof(*b));
    }
    return 0;
}
uint64_t z8_fs_plat_time_us(void) { return 0u; }
void z8_nfsp_v66_ops(struct z8_nfsp_ops *ops)
{
    if (ops) {
        memset(ops, 0, sizeof(*ops));
    }
}

#endif /* FILMSIM_HOST_TEST */

/* ---- V71.7 XM6 Hardening #1: 512 MiB Window-Straddle Spacer Retry Allocator ---- */

static int native_alloc_raw_cb(void *ctx, uint32_t requested, struct z8_nfsp_allocation *out)
{
    (void)ctx;
    return z8_fs_plat_alloc_class24(requested, out);
}

static void native_release_raw_cb(void *ctx, struct z8_nfsp_allocation *blk)
{
    (void)ctx;
    z8_fs_plat_release_class24(blk);
}

int z8_fs_plat_alloc_class24_xm6_ops(uint32_t requested,
                                     struct z8_nfsp_allocation *out,
                                     const Z8Class24SpacerOps *ops)
{
    static const Z8Class24SpacerOps k_native_ops = {
        .alloc_raw   = native_alloc_raw_cb,
        .release_raw = native_release_raw_cb,
        .on_spacer   = 0,
        .ctx         = 0
    };
    const Z8Class24SpacerOps *use_ops = (ops && ops->alloc_raw && ops->release_raw)
                                            ? ops
                                            : &k_native_ops;
    struct z8_nfsp_allocation spacer;
    struct z8_nfsp_allocation fresh;

    if (!out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (!requested || requested > UINT32_C(0x20000000)) {
        return 0;
    }

    memset(&spacer, 0, sizeof(spacer));
    if (!use_ops->alloc_raw(use_ops->ctx, requested, &spacer)) {
        return 0;
    }

    /* Fast path: first block fits cleanly within a single 512 MiB DSP IOMMU window. */
    if (!z8_fs_plat_span_straddles_512m(spacer.dma, spacer.actual)) {
        *out = spacer;
        return 1;
    }

    /* Straddle detected: refuse 2x transient spacer allocation when requested exceeds
     * Z8_FS_XM6_SPACER_MAX_BYTES (24 MiB) so Selector 0 cannot be exhausted under load;
     * caller cleanly falls back to ARM NEON / bypass without leaking the spacer. */
    uint64_t spacer_dma = spacer.dma;
    uint32_t spacer_actual = spacer.actual;
    memset(&fresh, 0, sizeof(fresh));
    int fresh_ok = (requested <= Z8_FS_XM6_SPACER_MAX_BYTES)
                       ? use_ops->alloc_raw(use_ops->ctx, requested, &fresh)
                       : 0;

    /* Always release spacer immediately once retry completes (zero leak). */
    spacer.used |= Z8_NFSP_USED_CLEANED_BIT; /* Never dirtied by CPU */
    use_ops->release_raw(use_ops->ctx, &spacer);

    if (fresh_ok && z8_fs_plat_span_straddles_512m(fresh.dma, fresh.actual)) {
        fresh.used |= Z8_NFSP_USED_CLEANED_BIT;
        use_ops->release_raw(use_ops->ctx, &fresh);
        fresh_ok = 0;
    }

    plat_log_spacer(spacer_dma, spacer_actual, fresh_ok ? fresh.dma : 0u, fresh_ok);
    if (use_ops->on_spacer) {
        use_ops->on_spacer(use_ops->ctx, spacer_dma, spacer_actual,
                           fresh_ok ? fresh.dma : 0u, fresh_ok);
    }

    if (!fresh_ok) {
        return 0;
    }
    *out = fresh;
    return 1;
}

int z8_fs_plat_alloc_class24_xm6(uint32_t requested, struct z8_nfsp_allocation *out)
{
    return z8_fs_plat_alloc_class24_xm6_ops(requested, out, 0);
}
