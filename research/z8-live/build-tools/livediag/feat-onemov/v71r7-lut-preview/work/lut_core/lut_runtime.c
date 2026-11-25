/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Unified .cube LUT Runtime Context & Stage 0 Dispatcher.
 *
 * Strictly scalar (-mgeneral-regs-only), zero FP, zero SIMD, zero .data/.bss!
 * Allocates LutCoreCtx lazily via HEAP_ALLOC (0x40D2C408) and anchors it in
 * v71r7_anchor()->lut_core_ctx (DRAM 0xA11640000).
 * Delegates NEON row/frame grading strictly to cube_frame_neon.c & layer0_color_neon.c
 * gated by v71r7_neon_sentinel_is_allowed().
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "lut_runtime.h"
#include "cube_frame.h"
#include "layer0_color.h"
#include "neon_sentinel.h"
#include "../filmsim/src/xm6/z8_xm6_bridge.h"

#ifdef V71R7_HOST_TEST
#include <stdlib.h>
#endif

#define LUT_FN(addr, type)   ((type)(uintptr_t)(addr))
#define Z8_ADDR_HEAP_ALLOC   UINT64_C(0x40D2C408)
#define Z8_ADDR_HEAP_FREE    UINT64_C(0x40D2C448)
#define Z8_ADDR_DLY_TSK      UINT64_C(0x40187B00)

#define LUT_CTX_MAGIC        UINT32_C(0x4C555437) /* "LUT7" */
#define LUT_RW_WRITER_BIT    UINT32_C(0x80000000)
#define LUT_RETIRED_CUBE_CAP 8u

/* Weak hook overridden strongly by layer0_provider.c in Bank 1 link:
 * clears Layer 0's cache_ready_gen and drains cache_readers BEFORE ctx->generation changes. */
__attribute__((weak)) void v71r7_layer0_cache_invalidate_before_swap(void) {}

typedef struct {
    uint32_t magic;
    uint32_t rw_lock;          /* bit31=writer, bits0..30=readers */
    uint32_t active;
    uint32_t generation;
    int      is_1d_separable;
    uint8_t  xm6_mode;
    uint8_t  pad[3];
    JpegCube *active_cube;     /* Lazily allocated on heap only while LUT active */
    JpegCube *retired_cubes[LUT_RETIRED_CUBE_CAP]; /* Deferred free while SMP helper quarantined */
    Z8Xm6RgblutCurves curves;
    char     active_path[FX_Z8_LUT_PATH_BYTES];
    struct z8_xm6_bridge *xm6_bridge;
    V71r7LutRuntimeStats  stats;
    V71r7SmpPool          smp;
} LutCoreCtx;

static void *lut_heap_alloc(uint32_t bytes)
{
#ifdef V71R7_HOST_TEST
    return calloc(1u, bytes ? bytes : 16u);
#else
    void *p = LUT_FN(Z8_ADDR_HEAP_ALLOC, void *(*)(uint32_t, uint32_t))(bytes, 0u);
    if (p) {
        memset(p, 0, bytes);
    }
    return p;
#endif
}

static void lut_heap_free(void *ptr)
{
    if (!ptr) {
        return;
    }
#ifdef V71R7_HOST_TEST
    free(ptr);
#else
    LUT_FN(Z8_ADDR_HEAP_FREE, void (*)(void *))(ptr);
#endif
}

static void lut_yield(void)
{
#ifdef V71R7_HOST_TEST
    /* Lightweight spin yield */
    __asm__ volatile("" ::: "memory");
#else
    (void)LUT_FN(Z8_ADDR_DLY_TSK, int32_t (*)(uint32_t))(1u);
#endif
}

static LutCoreCtx *lut_ctx_get(void)
{
    V71r7DramAnchor *anchor = v71r7_anchor();
    void *ptr = __atomic_load_n(&anchor->lut_core_ctx, __ATOMIC_ACQUIRE);
    if (ptr) {
        LutCoreCtx *ctx = (LutCoreCtx *)ptr;
        if (ctx->magic == LUT_CTX_MAGIC) {
            return ctx;
        }
    }
    return 0;
}

static LutCoreCtx *lut_ctx_ensure(void)
{
    LutCoreCtx *ctx = lut_ctx_get();
    if (ctx) {
        return ctx;
    }
    LutCoreCtx *fresh = (LutCoreCtx *)lut_heap_alloc((uint32_t)sizeof(LutCoreCtx));
    if (!fresh) {
        return 0;
    }
    fresh->magic = LUT_CTX_MAGIC;

    V71r7DramAnchor *anchor = v71r7_anchor();
    void *expected = 0;
    if (!__atomic_compare_exchange_n(&anchor->lut_core_ctx, &expected, (void *)fresh,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        lut_heap_free(fresh);
        return (LutCoreCtx *)expected;
    }
    if (__atomic_load_n(&anchor->magic, __ATOMIC_ACQUIRE) != V71R7_ANCHOR_MAGIC) {
        __atomic_store_n(&anchor->magic, V71R7_ANCHOR_MAGIC, __ATOMIC_RELEASE);
    }
    /* Pick up XM6 bridge bound earlier while LutCoreCtx was dormant. */
    fresh->xm6_bridge = (struct z8_xm6_bridge *)__atomic_load_n(&anchor->xm6_bridge, __ATOMIC_ACQUIRE);
    fresh->xm6_mode   = __atomic_load_n(&anchor->xm6_mode, __ATOMIC_ACQUIRE);
    return fresh;
}

/* Exposed to smp.c (avoids any global state in smp.c!) */
V71r7SmpPool *v71r7_lut_core_smp_pool(void)
{
    LutCoreCtx *ctx = lut_ctx_ensure();
    return ctx ? &ctx->smp : 0;
}

/* Non-allocating peek so dormant status/stats/stop queries NEVER allocate LutCoreCtx! */
V71r7SmpPool *v71r7_lut_core_smp_pool_peek(void)
{
    LutCoreCtx *ctx = lut_ctx_get();
    return ctx ? &ctx->smp : 0;
}

static void lut_drain_retired_cubes(LutCoreCtx *ctx, int force)
{
    if (!ctx) {
        return;
    }
    if (!force && !v71r7_smp_quarantine_drained()) {
        return;
    }
    for (unsigned i = 0u; i < LUT_RETIRED_CUBE_CAP; ++i) {
        JpegCube *old = __atomic_exchange_n(&ctx->retired_cubes[i], (JpegCube *)0, __ATOMIC_ACQ_REL);
        if (old) {
            lut_heap_free(old);
        }
    }
}

static void lut_retire_cube(LutCoreCtx *ctx, JpegCube *old_cube)
{
    lut_drain_retired_cubes(ctx, 0);
    if (!old_cube) {
        return;
    }
    if (v71r7_smp_quarantine_drained()) {
        lut_heap_free(old_cube);
        return;
    }
    /* An SMP helper was quarantined and has not yet finished its in-flight stripe:
     * defer freeing old_cube until v71r7_smp_quarantine_drained() confirms zero readers! */
    if (ctx) {
        for (unsigned i = 0u; i < LUT_RETIRED_CUBE_CAP; ++i) {
            JpegCube *empty = 0;
            if (__atomic_compare_exchange_n(&ctx->retired_cubes[i], &empty, old_cube,
                                            0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                return;
            }
        }
    }
    /* Table full: intentionally retain rather than UAF a live helper. */
}

/* ---- Atomic Reader-Writer Lock for Active JpegCube ---- */

static int rw_read_lock(LutCoreCtx *ctx)
{
    for (unsigned i = 0u; i < 200u; ++i) {
        uint32_t v = __atomic_load_n(&ctx->rw_lock, __ATOMIC_ACQUIRE);
        if ((v & LUT_RW_WRITER_BIT) != 0u || (v & ~LUT_RW_WRITER_BIT) >= 0x7ffffff0u) {
            lut_yield();
            continue;
        }
        if (__atomic_compare_exchange_n(&ctx->rw_lock, &v, v + 1u,
                                        1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return 1;
        }
    }
    return 0;
}

static void rw_read_unlock(LutCoreCtx *ctx)
{
    __atomic_fetch_sub(&ctx->rw_lock, 1u, __ATOMIC_RELEASE);
}

static void rw_write_lock(LutCoreCtx *ctx)
{
    for (;;) {
        uint32_t expected = 0u;
        if (__atomic_compare_exchange_n(&ctx->rw_lock, &expected, LUT_RW_WRITER_BIT,
                                        1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return;
        }
        lut_yield();
    }
}

static void rw_write_unlock(LutCoreCtx *ctx)
{
    __atomic_store_n(&ctx->rw_lock, 0u, __ATOMIC_RELEASE);
}

/* ---- .cube Buffer Parsing & Preparation ---- */

static void prog_update(FxZ8LutProgress *p, uint32_t gen, uint32_t phase,
                        uint32_t result, uint32_t bytes, uint32_t done, uint32_t total)
{
    if (!p) {
        return;
    }
    p->version    = FX_Z8_LUT_PROGRESS_VERSION;
    p->generation = gen;
    p->phase      = phase;
    p->result     = result;
    p->file_bytes = bytes;
    p->work_done  = done;
    p->work_total = total;
    p->cache_hit  = 0u;
}

typedef struct {
    JpegCube *cube;
    char      line[CUBE_LINE];
    uint32_t  line_len;
    uint32_t  total_bytes;
    uint32_t  generation;
    uint8_t   bom_seen;
    uint8_t   bom_buf[3];
    uint8_t   bom_len;
    uint8_t   failed;
} LutCubeStream;

static int lut_stream_push_byte(LutCubeStream *st, uint8_t ch)
{
    if (ch == 0u) {
        /* Embedded NUL byte -> malformed binary input */
        st->failed = 1u;
        return 0;
    }
    if (ch == '\n') {
        st->line[st->line_len] = '\0';
        if (!jpeg_cube_line(st->cube, st->line)) {
            st->failed = 1u;
            return 0;
        }
        st->line_len = 0u;
    } else {
        if (st->line_len >= CUBE_LINE - 1u) {
            st->failed = 1u;
            return 0;
        }
        st->line[st->line_len++] = (char)ch;
    }
    return 1;
}

void *v71r7_lut_stream_begin(uint32_t generation, FxZ8LutProgress *prog_out)
{
    LutCoreCtx *ctx = lut_ctx_ensure();
    if (!ctx) {
        prog_update(prog_out, generation, FX_Z8_LUT_ERROR, 2u, 0u, 0u, 0u);
        return 0;
    }

    LutCubeStream *st = (LutCubeStream *)lut_heap_alloc((uint32_t)sizeof(LutCubeStream));
    if (!st) {
        __atomic_fetch_add(&ctx->stats.prepare_fail_cnt, 1u, __ATOMIC_RELAXED);
        prog_update(prog_out, generation, FX_Z8_LUT_ERROR, 3u, 0u, 0u, 0u);
        return 0;
    }
    JpegCube *tmp = (JpegCube *)lut_heap_alloc((uint32_t)sizeof(JpegCube));
    if (!tmp) {
        lut_heap_free(st);
        __atomic_fetch_add(&ctx->stats.prepare_fail_cnt, 1u, __ATOMIC_RELAXED);
        prog_update(prog_out, generation, FX_Z8_LUT_ERROR, 3u, 0u, 0u, 0u);
        return 0;
    }

    memset(st, 0, sizeof(*st));
    st->cube = tmp;
    st->generation = generation;
    jpeg_cube_init(tmp);
    prog_update(prog_out, generation, FX_Z8_LUT_PREPARING, 0u, 0u, 0u, 0u);
    return st;
}

int v71r7_lut_stream_feed(void *stream_handle, const uint8_t *chunk, uint32_t bytes,
                          FxZ8LutProgress *prog_out)
{
    LutCubeStream *st = (LutCubeStream *)stream_handle;
    if (!st || !st->cube || (!chunk && bytes > 0u)) {
        return 0;
    }
    if (st->failed) {
        return 0;
    }
    if (bytes > CUBE_MAX_BYTES - st->total_bytes) {
        st->failed = 1u;
        prog_update(prog_out, st->generation, FX_Z8_LUT_ERROR, 1u,
                    st->total_bytes, st->total_bytes, st->total_bytes);
        return 0;
    }

    for (uint32_t i = 0u; i < bytes; ++i) {
        uint8_t ch = chunk[i];
        if (!st->bom_seen) {
            st->bom_buf[st->bom_len++] = ch;
            int could_be_bom =
                (st->bom_len == 1u && st->bom_buf[0] == 0xEFu) ||
                (st->bom_len == 2u && st->bom_buf[0] == 0xEFu && st->bom_buf[1] == 0xBBu);
            if (could_be_bom) {
                continue;
            }
            st->bom_seen = 1u;
            if (st->bom_len == 3u &&
                st->bom_buf[0] == 0xEFu && st->bom_buf[1] == 0xBBu && st->bom_buf[2] == 0xBFu) {
                st->bom_len = 0u;
                continue;
            }
            for (uint8_t k = 0u; k < st->bom_len; ++k) {
                if (!lut_stream_push_byte(st, st->bom_buf[k])) {
                    prog_update(prog_out, st->generation, FX_Z8_LUT_ERROR, 4u,
                                st->total_bytes + i, st->total_bytes + i, st->total_bytes + bytes);
                    return 0;
                }
            }
            st->bom_len = 0u;
        } else {
            if (!lut_stream_push_byte(st, ch)) {
                prog_update(prog_out, st->generation, FX_Z8_LUT_ERROR, 4u,
                            st->total_bytes + i, st->total_bytes + i, st->total_bytes + bytes);
                return 0;
            }
        }
    }

    st->total_bytes += bytes;
    prog_update(prog_out, st->generation, FX_Z8_LUT_PREPARING, 0u,
                st->total_bytes, st->total_bytes, st->total_bytes);
    return 1;
}

void v71r7_lut_stream_abort(void *stream_handle)
{
    LutCubeStream *st = (LutCubeStream *)stream_handle;
    if (!st) {
        return;
    }
    if (st->cube) {
        lut_heap_free(st->cube);
        st->cube = 0;
    }
    lut_heap_free(st);
}

int v71r7_lut_stream_commit(void *stream_handle, const char *path, uint32_t generation,
                            FxZ8LutProgress *prog_out)
{
    LutCubeStream *st = (LutCubeStream *)stream_handle;
    LutCoreCtx *ctx = lut_ctx_ensure();
    if (!st || !st->cube || !ctx) {
        v71r7_lut_stream_abort(stream_handle);
        prog_update(prog_out, generation, FX_Z8_LUT_ERROR, 2u, 0u, 0u, 0u);
        return 0;
    }

    uint32_t bytes = st->total_bytes;
    if (!st->failed && !st->bom_seen && st->bom_len > 0u) {
        st->bom_seen = 1u;
        for (uint8_t k = 0u; k < st->bom_len; ++k) {
            if (!lut_stream_push_byte(st, st->bom_buf[k])) {
                break;
            }
        }
        st->bom_len = 0u;
    }
    if (!st->failed && bytes == 0u) {
        st->failed = 1u;
    }
    if (!st->failed && st->line_len > 0u) {
        st->line[st->line_len] = '\0';
        if (!jpeg_cube_line(st->cube, st->line)) {
            st->failed = 1u;
        }
    }
    if (!st->failed && !jpeg_cube_finish(st->cube)) {
        st->failed = 1u;
    }

    if (st->failed) {
        v71r7_lut_stream_abort(st);
        __atomic_fetch_add(&ctx->stats.prepare_fail_cnt, 1u, __ATOMIC_RELAXED);
        prog_update(prog_out, generation, FX_Z8_LUT_ERROR, 4u, bytes, bytes, bytes);
        return 0;
    }

    JpegCube *tmp = st->cube;
    st->cube = 0;
    lut_heap_free(st);

    Z8Xm6RgblutCurves compiled;
    memset(&compiled, 0, sizeof(compiled));
    int is_1d = jpeg_cube_compile_curves(tmp, compiled.values, compiled.source);

    /* P1 Cache Race Fix: clear Layer 0 cache_ready_gen & drain readers BEFORE advancing
     * LutCoreCtx->generation so ensure_cache_gen() never memsets concurrently with readers! */
    v71r7_layer0_cache_invalidate_before_swap();

    /* Swap new compiled LUT atomically under writer lock */
    rw_write_lock(ctx);
    JpegCube *old_cube = ctx->active_cube;
    ctx->active_cube     = tmp;
    ctx->curves          = compiled;
    ctx->is_1d_separable = is_1d;
    uint32_t final_gen = generation ? generation : (ctx->generation + 1u);
    if (!final_gen) {
        final_gen = 1u;
    }
    __atomic_store_n(&ctx->generation, final_gen, __ATOMIC_RELEASE);
    memset(ctx->active_path, 0, sizeof(ctx->active_path));
    if (path) {
        size_t i = 0u;
        for (; i < sizeof(ctx->active_path) - 1u && path[i]; ++i) {
            ctx->active_path[i] = path[i];
        }
        ctx->active_path[i] = '\0';
    }
    __atomic_store_n(&ctx->active, 1u, __ATOMIC_RELEASE);

    V71r7DramAnchor *anchor = v71r7_anchor();
    __atomic_store_n(&anchor->active_generation, final_gen, __ATOMIC_RELEASE);
    __atomic_fetch_or(&anchor->flags, V71R7_ANCHOR_FLAG_LUT_ACTIVE, __ATOMIC_ACQ_REL);
    rw_write_unlock(ctx);

    lut_retire_cube(ctx, old_cube);

    __atomic_fetch_add(&ctx->stats.prepare_ok_cnt, 1u, __ATOMIC_RELAXED);
    prog_update(prog_out, final_gen, FX_Z8_LUT_READY, 0u, bytes, bytes, bytes);
    return 1;
}

int v71r7_lut_prepare_from_buffer(const uint8_t *buf, uint32_t bytes, const char *path,
                                  uint32_t generation, FxZ8LutProgress *prog_out)
{
    if (!buf || bytes == 0u || bytes > CUBE_MAX_BYTES) {
        prog_update(prog_out, generation, FX_Z8_LUT_ERROR, 1u, bytes, 0u, bytes);
        LutCoreCtx *c = lut_ctx_get();
        if (c) {
            __atomic_fetch_add(&c->stats.prepare_fail_cnt, 1u, __ATOMIC_RELAXED);
        }
        return 0;
    }
    void *st = v71r7_lut_stream_begin(generation, prog_out);
    if (!st) {
        return 0;
    }
    (void)v71r7_lut_stream_feed(st, buf, bytes, prog_out);
    return v71r7_lut_stream_commit(st, path, generation, prog_out);
}

void v71r7_lut_clear_active(void)
{
    v71r7_layer0_cache_invalidate_before_swap();

    LutCoreCtx *ctx = lut_ctx_get();
    V71r7DramAnchor *anchor = v71r7_anchor();
    if (__atomic_load_n(&anchor->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC) {
        __atomic_fetch_and(&anchor->flags, ~V71R7_ANCHOR_FLAG_LUT_ACTIVE, __ATOMIC_ACQ_REL);
        __atomic_fetch_add(&anchor->active_generation, 1u, __ATOMIC_RELEASE);
    }

    if (!ctx) {
        return;
    }
    rw_write_lock(ctx);
    JpegCube *old_cube = ctx->active_cube;
    ctx->active_cube = 0;
    ctx->is_1d_separable = 0;
    ctx->active_path[0] = '\0';
    __atomic_store_n(&ctx->active, 0u, __ATOMIC_RELEASE);
    rw_write_unlock(ctx);

    lut_retire_cube(ctx, old_cube);
}

int v71r7_lut_is_active(uint32_t *gen_out, int *is_1d_separable_out)
{
    if (!v71r7_lut_is_module_loaded() || !v71r7_lut_effective_master_on()) {
        if (gen_out) {
            *gen_out = 0u;
        }
        if (is_1d_separable_out) {
            *is_1d_separable_out = 0;
        }
        return 0;
    }
    LutCoreCtx *ctx = lut_ctx_get();
    if (!ctx || !__atomic_load_n(&ctx->active, __ATOMIC_ACQUIRE)) {
        if (gen_out) {
            *gen_out = 0u;
        }
        if (is_1d_separable_out) {
            *is_1d_separable_out = 0;
        }
        return 0;
    }
    if (gen_out) {
        *gen_out = __atomic_load_n(&ctx->generation, __ATOMIC_ACQUIRE);
    }
    if (is_1d_separable_out) {
        *is_1d_separable_out = ctx->is_1d_separable;
    }
    return 1;
}

int v71r7_lut_is_active_for(uint32_t stage_bit, uint32_t *gen_out, int *is_1d_separable_out)
{
    if (!v71r7_lut_is_enabled_for(stage_bit)) {
        if (gen_out) {
            *gen_out = 0u;
        }
        if (is_1d_separable_out) {
            *is_1d_separable_out = 0;
        }
        return 0;
    }
    return v71r7_lut_is_active(gen_out, is_1d_separable_out);
}

int v71r7_lut_get_active_curves(uint16_t curves_out[3][256], uint8_t source_out[3],
                                uint32_t *gen_out)
{
    if (!v71r7_lut_is_module_loaded() || !v71r7_lut_effective_master_on()) {
        return 0;
    }
    LutCoreCtx *ctx = lut_ctx_get();
    if (!ctx || !curves_out || !source_out) {
        return 0;
    }
    if (!rw_read_lock(ctx)) {
        return 0;
    }
    int ok = 0;
    if (ctx->active && ctx->active_cube && ctx->is_1d_separable) {
        memcpy(curves_out, ctx->curves.values, sizeof(ctx->curves.values));
        memcpy(source_out, ctx->curves.source, sizeof(ctx->curves.source));
        if (gen_out) {
            *gen_out = ctx->generation;
        }
        ok = 1;
    }
    rw_read_unlock(ctx);
    return ok;
}

int v71r7_lut_get_active_path(char path_out[FX_Z8_LUT_PATH_BYTES])
{
    if (!v71r7_lut_is_module_loaded() || !v71r7_lut_effective_master_on()) {
        if (path_out) {
            path_out[0] = '\0';
        }
        return 0;
    }
    LutCoreCtx *ctx = lut_ctx_get();
    if (!ctx || !path_out) {
        return 0;
    }
    if (!rw_read_lock(ctx)) {
        return 0;
    }
    int ok = 0;
    if (ctx->active) {
        memcpy(path_out, ctx->active_path, FX_Z8_LUT_PATH_BYTES);
        ok = 1;
    }
    rw_read_unlock(ctx);
    return ok;
}

void v71r7_lut_bind_xm6(struct z8_xm6_bridge *bridge, uint8_t xm6_mode)
{
    /* Strict 0 B dormant invariant: NEVER call lut_ctx_ensure() here!
     * Store xm6_bridge/xm6_mode in DRAM Anchor; mirror to LutCoreCtx only if already allocated. */
    V71r7DramAnchor *anchor = v71r7_anchor();
    if (__atomic_load_n(&anchor->magic, __ATOMIC_ACQUIRE) != V71R7_ANCHOR_MAGIC) {
        memset(anchor, 0, sizeof(*anchor));
        __atomic_store_n(&anchor->magic, V71R7_ANCHOR_MAGIC, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&anchor->xm6_bridge, (void *)bridge, __ATOMIC_RELEASE);
    __atomic_store_n(&anchor->xm6_mode, xm6_mode, __ATOMIC_RELEASE);

    LutCoreCtx *ctx = lut_ctx_get();
    if (!ctx) {
        return;
    }
    rw_write_lock(ctx);
    ctx->xm6_bridge = bridge;
    ctx->xm6_mode   = xm6_mode;
    rw_write_unlock(ctx);
}

/* ---- Cache Coherence Helper (Acquire/Release) ---- */

static void ensure_cache_gen(LutCoreCtx *ctx, uint32_t *cache_words_4mib,
                             uint32_t *cache_gen_inout, uint32_t cur_gen)
{
    if (!cache_words_4mib || !cache_gen_inout) {
        return;
    }
    if (__atomic_load_n(cache_gen_inout, __ATOMIC_ACQUIRE) != cur_gen) {
        __atomic_store_n(cache_gen_inout, 0u, __ATOMIC_RELEASE);
        memset(cache_words_4mib, 0, CUBE_FRAME_CACHE_BYTES);
        __atomic_store_n(cache_gen_inout, cur_gen, __ATOMIC_RELEASE);
        __atomic_fetch_add(&ctx->stats.cache_clears, 1u, __ATOMIC_RELAXED);
    }
}

/* ---- NV16 (Still) Stripe Worker Callback ---- */

typedef struct {
    const JpegCube *cube;
    uint32_t       *cache;
    const uint8_t  *src_y;
    const uint8_t  *src_uv;
    uint8_t        *dst_y;
    uint8_t        *dst_uv;
    uint32_t        width;
    uint32_t        y_stride;
    uint32_t        uv_stride;
    int             use_neon;
} Nv16StripeJob;

_Static_assert(sizeof(Nv16StripeJob) <= V71R7_SMP_INLINE_CTX_BYTES,
               "Nv16StripeJob must fit in persistent heap V71r7SmpJob.ctx_storage");

static void nv16_stripe_cb(void *arg, uint32_t row_begin, uint32_t row_end, unsigned worker_idx)
{
    (void)worker_idx;
    const Nv16StripeJob *j = (const Nv16StripeJob *)arg;
    for (uint32_t r = row_begin; r < row_end; ++r) {
        const uint8_t *sy = j->src_y  + (size_t)r * j->y_stride;
        const uint8_t *sc = j->src_uv + (size_t)r * j->uv_stride;
        uint8_t       *dy = j->dst_y  + (size_t)r * j->y_stride;
        uint8_t       *dc = j->dst_uv + (size_t)r * j->uv_stride;
        if (j->cache) {
            if (j->use_neon) {
                (void)jpeg_cube_cache_row_neon(j->cube, j->cache, sy, sc, dy, dc, j->width);
            } else {
                (void)jpeg_cube_cache_row_scalar(j->cube, j->cache, sy, sc, dy, dc, j->width);
            }
        } else {
            if (dy != sy) {
                memcpy(dy, sy, j->width);
            }
            if (dc != sc) {
                memcpy(dc, sc, j->width);
            }
            jpeg_cube_422(j->cube, dy, dc, j->width);
        }
    }
}

int v71r7_lut_apply_still_nv16_oop(const uint8_t *src_y, const uint8_t *src_uv,
                                   uint64_t src_y_dma, uint64_t src_uv_dma,
                                   uint8_t *dst_y, uint8_t *dst_uv,
                                   uint64_t dst_y_dma, uint64_t dst_uv_dma,
                                   uint32_t width, uint32_t height,
                                   uint32_t y_stride, uint32_t uv_stride,
                                   uint32_t *cache_words_4mib, uint32_t *cache_gen_inout)
{
    if (!v71r7_lut_is_enabled_for(V71R7_LUT_STAGE_STILL)) {
        return 0;
    }
    if (!src_y || !src_uv || !dst_y || !dst_uv ||
        width < 2u || (width & 1u) != 0u || height == 0u ||
        y_stride < width || uv_stride < width) {
        return 0;
    }
    LutCoreCtx *ctx = lut_ctx_get();
    if (!ctx || !rw_read_lock(ctx)) {
        return 0;
    }
    if (!ctx->active || !ctx->active_cube) {
        rw_read_unlock(ctx);
        return 0;
    }

    /* Fast path: 1D separable .cube on XM6 Core 0 Opcode 12 (RGBLUT) when out-of-place */
    const V71r7DramAnchor *anchor = v71r7_anchor();
    struct z8_xm6_bridge *xm6_br = ctx->xm6_bridge;
    uint8_t xm6_md = ctx->xm6_mode;
    if (!xm6_br && __atomic_load_n(&anchor->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC) {
        xm6_br = (struct z8_xm6_bridge *)__atomic_load_n(&anchor->xm6_bridge, __ATOMIC_ACQUIRE);
        xm6_md = __atomic_load_n(&anchor->xm6_mode, __ATOMIC_ACQUIRE);
    }
    if (ctx->is_1d_separable && xm6_br &&
        src_y != dst_y && src_uv != dst_uv && y_stride == uv_stride) {
        uint32_t elapsed_us = 0u;
        int xrc = z8_xm6_rgblut_nv16(xm6_br, xm6_md, &ctx->curves,
                                     src_y, src_uv, src_y_dma, src_uv_dma,
                                     dst_y, dst_uv, dst_y_dma, dst_uv_dma,
                                     width, height, y_stride, &elapsed_us);
        if (xrc == Z8_XM6_STILL_OK) {
            __atomic_fetch_add(&ctx->stats.nv16_xm6_calls, 1u, __ATOMIC_RELAXED);
            rw_read_unlock(ctx);
            return 2;
        }
        /* Clean or dirty fallback: fall through transparently to ARM SMP+NEON/Scalar */
    }

    ensure_cache_gen(ctx, cache_words_4mib, cache_gen_inout,
                     __atomic_load_n(&ctx->generation, __ATOMIC_ACQUIRE));
    int use_neon = v71r7_neon_sentinel_is_allowed();

    Nv16StripeJob job = {
        .cube      = ctx->active_cube,
        .cache     = cache_words_4mib,
        .src_y     = src_y,
        .src_uv    = src_uv,
        .dst_y     = dst_y,
        .dst_uv    = dst_uv,
        .width     = width,
        .y_stride  = y_stride,
        .uv_stride = uv_stride,
        .use_neon  = use_neon
    };

    /* Smaller 32-row stripes (~5 ms at 45MP) keep helper cancellation & prio-30 yields tight */
    uint32_t stripe = height >= 256u ? 32u : (height >= 32u ? 16u : height);
    if (height >= 32u) {
        (void)v71r7_smp_ensure_started();
        (void)v71r7_smp_run_stripes_copy(FX_Z8_LUT_HELPER_JPEG, height, stripe,
                                         nv16_stripe_cb, &job, sizeof(job),
                                         FX_Z8_HB_JPEG_LANE0);
    } else {
        nv16_stripe_cb(&job, 0u, height, 0u);
    }

    if (use_neon) {
        __atomic_fetch_add(&ctx->stats.nv16_neon_calls, 1u, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&ctx->stats.nv16_scalar_calls, 1u, __ATOMIC_RELAXED);
    }

    rw_read_unlock(ctx);
    return 1;
}

int v71r7_lut_apply_still_nv16(uint8_t *y, uint8_t *uv, uint32_t width, uint32_t height,
                               uint32_t y_stride, uint32_t uv_stride,
                               uint32_t *cache_words_4mib, uint32_t *cache_gen_inout)
{
    int rc = v71r7_lut_apply_still_nv16_oop(y, uv, 0u, 0u, y, uv, 0u, 0u,
                                            width, height, y_stride, uv_stride,
                                            cache_words_4mib, cache_gen_inout);
    return rc > 0 ? 1 : 0;
}

/* ---- NV12 (Layer 0 / Live) Stripe Worker Callback ---- */

typedef struct {
    const JpegCube *cube;
    uint32_t       *cache;
    Layer0Frame     frame;
    int             use_neon;
    int             check_suspend;
} Nv12StripeJob;

_Static_assert(sizeof(Nv12StripeJob) <= V71R7_SMP_INLINE_CTX_BYTES,
               "Nv12StripeJob must fit in persistent heap V71r7SmpJob.ctx_storage");

static void nv12_stripe_cb(void *arg, uint32_t row_begin, uint32_t row_end, unsigned worker_idx)
{
    (void)worker_idx;
    const Nv12StripeJob *j = (const Nv12StripeJob *)arg;
    if (j->check_suspend) {
        const V71r7DramAnchor *anc = v71r7_anchor();
        if (__atomic_load_n(&anc->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC &&
            (__atomic_load_n(&anc->flags, __ATOMIC_ACQUIRE) & V71R7_ANCHOR_FLAG_POOL_SUSPENDED)) {
            return;
        }
    }
    if (j->cache) {
        if (j->use_neon) {
            layer0_grade_nv12_planes_neon(j->cube, j->cache, &j->frame, row_begin, row_end);
        } else {
            layer0_grade_nv12_planes(j->cube, j->cache, &j->frame, row_begin, row_end);
        }
    } else {
        /* Cacheless safety fallback for degenerate callers passing NULL cache */
        size_t y_s  = j->frame.dst_y_stride  ? j->frame.dst_y_stride  : j->frame.width;
        size_t uv_s = j->frame.dst_uv_stride ? j->frame.dst_uv_stride : j->frame.width;
        for (uint32_t r = row_begin & ~1u; r < (row_end & ~1u); r += 2u) {
            uint8_t *y0 = j->frame.dst_y  + (size_t)r * y_s;
            uint8_t *y1 = y0 + y_s;
            uint8_t *uv = j->frame.dst_uv + (size_t)(r >> 1) * uv_s;
            for (uint32_t x = 0u; x < j->frame.width; x += 2u) {
                JpegCubeChroma off = jpeg_cube_chroma(uv[x], uv[x + 1u]);
                uint32_t s00 = jpeg_cube_sample(j->cube, y0[x], off);
                uint32_t s01 = jpeg_cube_sample(j->cube, y0[x + 1u], off);
                uint32_t s10 = jpeg_cube_sample(j->cube, y1[x], off);
                uint32_t s11 = jpeg_cube_sample(j->cube, y1[x + 1u], off);
                y0[x]      = (uint8_t)s00;
                y0[x + 1u] = (uint8_t)s01;
                y1[x]      = (uint8_t)s10;
                y1[x + 1u] = (uint8_t)s11;
                unsigned cb = (unsigned)(uint8_t)(s00 >> 8)  + (uint8_t)(s01 >> 8) +
                              (unsigned)(uint8_t)(s10 >> 8)  + (uint8_t)(s11 >> 8);
                unsigned cr = (unsigned)(uint8_t)(s00 >> 16) + (uint8_t)(s01 >> 16) +
                              (unsigned)(uint8_t)(s10 >> 16) + (uint8_t)(s11 >> 16);
                uv[x]      = (uint8_t)((cb + 2u) >> 2);
                uv[x + 1u] = (uint8_t)((cr + 2u) >> 2);
            }
        }
    }
}

int v71r7_lut_apply_nv12(uint8_t *y, uint8_t *uv, uint32_t width, uint32_t height,
                         uint32_t y_stride, uint32_t uv_stride,
                         uint32_t *cache_words_4mib, uint32_t *cache_gen_inout,
                         int use_smp)
{
    if (!v71r7_lut_is_enabled_for(V71R7_LUT_STAGE_PREVIEW | V71R7_LUT_STAGE_LIVE)) {
        return 0;
    }
    if (!y || !uv || width < 2u || height < 2u ||
        ((width | height) & 1u) != 0u ||
        y_stride < width || uv_stride < width) {
        return 0;
    }
    LutCoreCtx *ctx = lut_ctx_get();
    if (!ctx || !rw_read_lock(ctx)) {
        return 0;
    }
    if (!ctx->active || !ctx->active_cube) {
        rw_read_unlock(ctx);
        return 0;
    }

    ensure_cache_gen(ctx, cache_words_4mib, cache_gen_inout,
                     __atomic_load_n(&ctx->generation, __ATOMIC_ACQUIRE));
    int use_neon = v71r7_neon_sentinel_is_allowed();

    Nv12StripeJob job = {
        .cube          = ctx->active_cube,
        .cache         = cache_words_4mib,
        .frame         = {
            .src_y = y, .src_uv = uv,
            .dst_y = y, .dst_uv = uv,
            .width = width, .height = height,
            .src_y_stride = y_stride, .src_uv_stride = uv_stride,
            .dst_y_stride = y_stride, .dst_uv_stride = uv_stride
        },
        .use_neon      = use_neon,
        .check_suspend = use_smp ? 1 : 0
    };

    /* NV12 stripes MUST have even row boundaries so 2x2 chroma quads never split!
     * 16-row stripes (~0.18 ms on LCD) give near-instantaneous cancel/preempt response. */
    uint32_t stripe = height >= 64u ? 16u : (height >= 16u ? 8u : height);
    stripe &= ~1u;
    if (stripe < 2u) {
        stripe = 2u;
    }

    int undrained_quar = 0;
    if (use_smp && height > stripe) {
        (void)v71r7_smp_ensure_started();
        /* use_smp == 2 (Live Photo worker) passes FX_Z8_HB_COUNT so it never clobbers
         * Layer 0's FX_Z8_HB_REAR_STEP heartbeat slot when running concurrently! */
        unsigned hb_slot = (use_smp == 2) ? FX_Z8_HB_COUNT : FX_Z8_HB_REAR_STEP;
        undrained_quar = v71r7_smp_run_stripes_copy(FX_Z8_LUT_HELPER_REAR, height, stripe,
                                                    nv12_stripe_cb, &job, sizeof(job),
                                                    hb_slot);
    } else {
        nv12_stripe_cb(&job, 0u, height, 0u);
    }

    if (use_neon) {
        __atomic_fetch_add(&ctx->stats.nv12_neon_calls, 1u, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&ctx->stats.nv12_scalar_calls, 1u, __ATOMIC_RELAXED);
    }

    rw_read_unlock(ctx);
    return undrained_quar ? -2 : 1;
}

/* ---- V71.7-r7 528 KiB L2-Resident Pre-Baked 64x64x33 YCbCr 3D LUT Dispatcher ---- */

typedef struct {
    const JpegCube *cube;
    uint32_t       *table;
} BakedLutBuildJob;

_Static_assert(sizeof(BakedLutBuildJob) <= V71R7_SMP_INLINE_CTX_BYTES,
               "BakedLutBuildJob must fit in persistent heap V71r7SmpJob.ctx_storage");

static void baked_lut_build_cb(void *arg, uint32_t cr_begin, uint32_t cr_end, unsigned worker_idx)
{
    (void)worker_idx;
    const BakedLutBuildJob *j = (const BakedLutBuildJob *)arg;
    layer0_bake_ycbcr_64x64x33(j->cube, j->table, cr_begin, cr_end);
}

typedef struct {
    const uint32_t *table;
    Layer0Frame     frame;
    int             use_neon;
    int             check_suspend;
} Nv12BakedStripeJob;

_Static_assert(sizeof(Nv12BakedStripeJob) <= V71R7_SMP_INLINE_CTX_BYTES,
               "Nv12BakedStripeJob must fit in persistent heap V71r7SmpJob.ctx_storage");

static void nv12_baked_stripe_cb(void *arg, uint32_t row_begin, uint32_t row_end, unsigned worker_idx)
{
    (void)worker_idx;
    const Nv12BakedStripeJob *j = (const Nv12BakedStripeJob *)arg;
    if (j->check_suspend) {
        const V71r7DramAnchor *anc = v71r7_anchor();
        if (__atomic_load_n(&anc->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC &&
            (__atomic_load_n(&anc->flags, __ATOMIC_ACQUIRE) & V71R7_ANCHOR_FLAG_POOL_SUSPENDED)) {
            return;
        }
    }
    if (j->use_neon) {
        layer0_grade_nv12_baked_neon(j->table, &j->frame, row_begin, row_end);
    } else {
        layer0_grade_nv12_baked(j->table, &j->frame, row_begin, row_end);
    }
}

int v71r7_lut_apply_nv12_baked(uint8_t *y, uint8_t *uv, uint32_t width, uint32_t height,
                               uint32_t y_stride, uint32_t uv_stride,
                               uint32_t *baked_table_528k, uint32_t *baked_gen_inout,
                               int use_smp)
{
    if (!baked_table_528k || !baked_gen_inout) {
        return v71r7_lut_apply_nv12(y, uv, width, height, y_stride, uv_stride,
                                    0, 0, use_smp);
    }
    if (!v71r7_lut_is_enabled_for(V71R7_LUT_STAGE_PREVIEW | V71R7_LUT_STAGE_LIVE)) {
        return 0;
    }
    if (!y || !uv || width < 2u || height < 2u ||
        ((width | height) & 1u) != 0u ||
        y_stride < width || uv_stride < width) {
        return 0;
    }
    LutCoreCtx *ctx = lut_ctx_get();
    if (!ctx || !rw_read_lock(ctx)) {
        return 0;
    }
    if (!ctx->active || !ctx->active_cube) {
        rw_read_unlock(ctx);
        return 0;
    }

    uint32_t cur_gen = __atomic_load_n(&ctx->generation, __ATOMIC_ACQUIRE);
    unsigned hb_slot = (use_smp == 2) ? FX_Z8_HB_COUNT : FX_Z8_HB_REAR_STEP;

    if (__atomic_load_n(baked_gen_inout, __ATOMIC_ACQUIRE) != cur_gen) {
        __atomic_store_n(baked_gen_inout, 0u, __ATOMIC_RELEASE);
        BakedLutBuildJob bjob = { .cube = ctx->active_cube, .table = baked_table_528k };
        if (use_smp) {
            (void)v71r7_smp_ensure_started();
            int bquar = v71r7_smp_run_stripes_copy(FX_Z8_LUT_HELPER_REAR,
                                                   LAYER0_BAKED_CHROMA, 4u,
                                                   baked_lut_build_cb, &bjob, sizeof(bjob),
                                                   hb_slot);
            if (bquar) {
                rw_read_unlock(ctx);
                return -2;
            }
        } else {
            baked_lut_build_cb(&bjob, 0u, LAYER0_BAKED_CHROMA, 0u);
        }
        __atomic_store_n(baked_gen_inout, cur_gen, __ATOMIC_RELEASE);
        __atomic_fetch_add(&ctx->stats.cache_clears, 1u, __ATOMIC_RELAXED);
    }

    int use_neon = v71r7_neon_sentinel_is_allowed();
    Nv12BakedStripeJob job = {
        .table         = baked_table_528k,
        .frame         = {
            .src_y = y, .src_uv = uv,
            .dst_y = y, .dst_uv = uv,
            .width = width, .height = height,
            .src_y_stride = y_stride, .src_uv_stride = uv_stride,
            .dst_y_stride = y_stride, .dst_uv_stride = uv_stride
        },
        .use_neon      = use_neon,
        .check_suspend = use_smp ? 1 : 0
    };

    /* 32-row stripes cut SMP dispatch overhead in half while keeping < 0.15 ms cancel latency */
    uint32_t stripe = height >= 128u ? 32u : (height >= 16u ? 8u : height);
    stripe &= ~1u;
    if (stripe < 2u) {
        stripe = 2u;
    }

    int undrained_quar = 0;
    if (use_smp && height > stripe) {
        (void)v71r7_smp_ensure_started();
        undrained_quar = v71r7_smp_run_stripes_copy(FX_Z8_LUT_HELPER_REAR, height, stripe,
                                                    nv12_baked_stripe_cb, &job, sizeof(job),
                                                    hb_slot);
    } else {
        nv12_baked_stripe_cb(&job, 0u, height, 0u);
    }

    if (use_neon) {
        __atomic_fetch_add(&ctx->stats.nv12_neon_calls, 1u, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&ctx->stats.nv12_scalar_calls, 1u, __ATOMIC_RELAXED);
    }

    rw_read_unlock(ctx);
    return undrained_quar ? -2 : 1;
}

void v71r7_lut_runtime_get_stats(V71r7LutRuntimeStats *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    LutCoreCtx *ctx = lut_ctx_get();
    if (!ctx) {
        return;
    }
    out->prepare_ok_cnt   = __atomic_load_n(&ctx->stats.prepare_ok_cnt, __ATOMIC_RELAXED);
    out->prepare_fail_cnt = __atomic_load_n(&ctx->stats.prepare_fail_cnt, __ATOMIC_RELAXED);
    out->nv16_neon_calls  = __atomic_load_n(&ctx->stats.nv16_neon_calls, __ATOMIC_RELAXED);
    out->nv16_scalar_calls= __atomic_load_n(&ctx->stats.nv16_scalar_calls, __ATOMIC_RELAXED);
    out->nv16_xm6_calls   = __atomic_load_n(&ctx->stats.nv16_xm6_calls, __ATOMIC_RELAXED);
    out->nv12_neon_calls  = __atomic_load_n(&ctx->stats.nv12_neon_calls, __ATOMIC_RELAXED);
    out->nv12_scalar_calls= __atomic_load_n(&ctx->stats.nv12_scalar_calls, __ATOMIC_RELAXED);
    out->cache_clears     = __atomic_load_n(&ctx->stats.cache_clears, __ATOMIC_RELAXED);
}

#ifdef V71R7_HOST_TEST
void v71r7_lut_runtime_shutdown_for_test(void)
{
    v71r7_smp_stop();
    v71r7_lut_clear_active();
    V71r7DramAnchor *anchor = v71r7_anchor();
    LutCoreCtx *ctx = (LutCoreCtx *)__atomic_exchange_n(&anchor->lut_core_ctx, (void *)0,
                                                        __ATOMIC_ACQ_REL);
    if (ctx) {
        lut_drain_retired_cubes(ctx, 1);
        if (ctx->active_cube) {
            lut_heap_free(ctx->active_cube);
        }
        lut_heap_free(ctx);
    }
}
#endif
