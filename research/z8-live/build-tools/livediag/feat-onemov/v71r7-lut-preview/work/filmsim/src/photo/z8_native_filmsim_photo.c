/* SPDX-License-Identifier: MIT
 * Whole-shot 6-slot NV16 Native Photo Coordinator for Z8 FilmSim WLM.
 * Native sources and NEF RAW are strictly zero-touch.
 * Freestanding C11, scalar-only (-mgeneral-regs-only), zero malloc, zero .data/.bss.
 */
#include "z8_native_filmsim_photo.h"
#include "../zf_grain/z8_zf_grain_kernels.h"
#include "../xm6/z8_xm6_bridge.h"
#include <string.h>

#if !defined(FILMSIM_HOST_TEST) || defined(V71R7_HOST_TEST)
#include "../../../lut_core/v71r7_lut_abi.h"
#include "../../../lut_core/lut_runtime.h"
#define NFSP_HAS_V71R7_LUT 1
#else
#define NFSP_HAS_V71R7_LUT 0
#define FX_Z8_FLIGHT_JPEG_MAIN_ENTER  1u
#define FX_Z8_FLIGHT_JPEG_GRADE_START 3u
#define FX_Z8_FLIGHT_JPEG_GRADE_DONE  4u
#define FX_Z8_FLIGHT_JPEG_FALLBACK    7u
#define FX_Z8_STAGE_JPEG_GRADE        3u
#define FX_Z8_STAGE_JPEG_CACHE_MAINT  5u
#define FX_Z8_STAGE_JPEG_XM6          8u
#endif

static inline int nfsp_lut_is_active(uint32_t *gen_out, int *is_1d_out)
{
#if NFSP_HAS_V71R7_LUT
    return v71r7_lut_is_active_for(V71R7_LUT_STAGE_STILL, gen_out, is_1d_out);
#else
    if (gen_out) *gen_out = 0u;
    if (is_1d_out) *is_1d_out = 0;
    return 0;
#endif
}

static inline int nfsp_xm6_is_hung_dma(int rc, const struct z8_xm6_bridge *xm6)
{
    if (rc == Z8_XM6_STILL_HUNG_DMA) {
        return 1;
    }
    if (rc == Z8_XM6_STILL_FALLBACK_DIRTY && xm6 &&
        __atomic_load_n(&xm6->still_hung_dma, __ATOMIC_ACQUIRE) != 0u) {
        return 1;
    }
    return 0;
}

static inline int nfsp_lut_get_curves(Z8Xm6RgblutCurves *out)
{
#if NFSP_HAS_V71R7_LUT
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    return v71r7_lut_get_active_curves(out->values, out->source, 0);
#else
    (void)out;
    return 0;
#endif
}

static inline int nfsp_lut_apply_still_nv16(uint8_t *y, uint8_t *uv,
                                            uint32_t width, uint32_t height,
                                            uint32_t y_stride, uint32_t uv_stride,
                                            uint32_t *cache_4mib, uint32_t *cache_gen_inout)
{
#if NFSP_HAS_V71R7_LUT
    return v71r7_lut_apply_still_nv16(y, uv, width, height, y_stride, uv_stride,
                                      cache_4mib, cache_gen_inout);
#else
    (void)y; (void)uv; (void)width; (void)height;
    (void)y_stride; (void)uv_stride; (void)cache_4mib; (void)cache_gen_inout;
    return 0;
#endif
}

static inline int nfsp_neon_allowed(void)
{
#if NFSP_HAS_V71R7_LUT
    return v71r7_neon_sentinel_is_allowed();
#else
    return 1;
#endif
}

static inline void nfsp_flight(uint16_t ev, uint16_t cf, uint32_t a, uint32_t b, uint32_t c)
{
#if NFSP_HAS_V71R7_LUT
    v71r7_flight_record(ev, cf, a, b, c);
#else
    (void)ev; (void)cf; (void)a; (void)b; (void)c;
#endif
}

static inline void nfsp_hb_enter(unsigned stage, uint32_t cap_num, uint32_t aux)
{
#if NFSP_HAS_V71R7_LUT
    v71r7_heartbeat_enter(FX_Z8_HB_JPEG_LANE0, stage, cap_num, aux);
#else
    (void)stage; (void)cap_num; (void)aux;
#endif
}

static inline void nfsp_hb_progress(void)
{
#if NFSP_HAS_V71R7_LUT
    v71r7_heartbeat_progress(FX_Z8_HB_JPEG_LANE0);
#endif
}

static inline void nfsp_hb_idle(void)
{
#if NFSP_HAS_V71R7_LUT
    v71r7_heartbeat_idle(FX_Z8_HB_JPEG_LANE0);
#endif
}

static inline int nfsp_frame_take(int *held_out)
{
    *held_out = 0;
#if NFSP_HAS_V71R7_LUT
    FxZ8LutIoGate *gate = v71r7_io_gate();
    uint32_t epoch = fx_z8_lut_io_epoch(gate);
    if (epoch != 0u) {
        if (fx_z8_lut_frame_take(gate, epoch)) {
            *held_out = 1;
            return 1;
        }
        return 0;
    }
    uint64_t st = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    if (st & (FX_Z8_LUT_IO_STOPPED | FX_Z8_LUT_IO_EARLY_OFF)) {
        return 0;
    }
#endif
    return 1;
}

static inline void nfsp_frame_drop(int *held)
{
#if NFSP_HAS_V71R7_LUT
    if (held && *held) {
        fx_z8_lut_frame_drop(v71r7_io_gate());
        *held = 0;
    }
#else
    if (held) *held = 0;
#endif
}

static uint64_t u64_le(const void *p)
{
    uint64_t v = 0u;
    memcpy(&v, p, 8u);
    return v;
}

static void put64_le(void *p, uint64_t v)
{
    memcpy(p, &v, 8u);
}

static uint32_t phase_load(const struct z8_native_filmsim_photo *p)
{
    return __atomic_load_n(&p->phase, __ATOMIC_ACQUIRE);
}

static void lock_shot(struct z8_native_filmsim_photo *p)
{
    while (__atomic_exchange_n(&p->lock, 1u, __ATOMIC_ACQUIRE)) {
        if (p->ops.yield) {
            p->ops.yield(p->ops.context);
        }
    }
}

static void unlock_shot(struct z8_native_filmsim_photo *p)
{
    __atomic_store_n(&p->lock, 0u, __ATOMIC_RELEASE);
}

static void set_reason(struct z8_native_filmsim_photo *p, uint32_t why)
{
    uint32_t zero = 0u;
    __atomic_compare_exchange_n(&p->reason, &zero, why, 0,
                                __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

#define SOURCE_CLOSED  UINT32_C(0x80000000)
#define SOURCE_READERS UINT32_C(0x7fffffff)

struct source_reader {
    struct z8_native_filmsim_photo *p;
    uint64_t generation;
};

static uint64_t gen_load(const struct z8_native_filmsim_photo *p)
{
    return __atomic_load_n(&p->generation, __ATOMIC_ACQUIRE);
}

static int source_enter(struct z8_native_filmsim_photo *p, uint64_t g)
{
    int accepted = 0;
    lock_shot(p);
    uint32_t gate = __atomic_load_n(&p->source_gate, __ATOMIC_ACQUIRE);
    if (g && gen_load(p) == g && !(gate & SOURCE_CLOSED) &&
        (gate & SOURCE_READERS) != SOURCE_READERS && phase_load(p) != Z8_NFSP_IDLE) {
        __atomic_add_fetch(&p->source_gate, 1u, __ATOMIC_ACQ_REL);
        accepted = 1;
    }
    unlock_shot(p);
    return accepted;
}

static void source_leave(struct z8_native_filmsim_photo *p)
{
    __atomic_sub_fetch(&p->source_gate, 1u, __ATOMIC_RELEASE);
}

static void source_close(struct z8_native_filmsim_photo *p)
{
    __atomic_fetch_or(&p->source_gate, SOURCE_CLOSED, __ATOMIC_ACQ_REL);
}

static int source_drained(const struct z8_native_filmsim_photo *p)
{
    uint32_t gate = __atomic_load_n(&p->source_gate, __ATOMIC_ACQUIRE);
    return (gate & SOURCE_CLOSED) && !(gate & SOURCE_READERS);
}

static int rd_gen(struct z8_native_filmsim_photo *p, uint64_t g,
                  uint64_t a, void *d, size_t n)
{
    if (!a || a > UINT64_MAX - n || !source_enter(p, g)) {
        return 0;
    }
    int ok = p->ops.read(p->ops.context, a, d, n);
    source_leave(p);
    return ok;
}

static int rd(struct z8_native_filmsim_photo *p, uint64_t a, void *d, size_t n)
{
    return rd_gen(p, gen_load(p), a, d, n);
}

static int snapshot_read(void *o, uint64_t a, void *d, size_t n)
{
    struct source_reader *r = (struct source_reader *)o;
    return rd_gen(r->p, r->generation, a, d, n);
}

static int cancelled_cb(void *o)
{
    struct z8_native_filmsim_photo *p = (struct z8_native_filmsim_photo *)o;
    uint32_t ph = phase_load(p);
    if (ph == Z8_NFSP_COPY || ph == Z8_NFSP_PROCESS) {
        nfsp_hb_progress();
    }
    if (__atomic_load_n(&p->cancel, __ATOMIC_ACQUIRE)) {
        return 1;
    }
    if (p->ops.pool_valid && !p->ops.pool_valid(p->ops.context)) {
        set_reason(p, Z8_NFSP_POOL);
        __atomic_store_n(&p->cancel, 1u, __ATOMIC_RELEASE);
        return 1;
    }
    /* V71.5: 16 ms cooperative time-slice yield via dly_tsk(0), tightening to 3 ms
     * while Live HDF has a pending frame on CPU2 so Mode 2 Live stays smooth 30fps
     * without blocking Still photo for 1.52s during post-roll. */
    if ((++p->yield_counter & 15u) == 0u && p->ops.yield) {
        if (p->ops.time_us) {
            uint32_t slice_us = 16000u;
            if (p->ops.should_defer && p->ops.should_defer(p->ops.context, UINT64_MAX)) {
                slice_us = 3000u;
            }
            uint32_t now32 = (uint32_t)p->ops.time_us(p->ops.context);
            if ((uint32_t)(now32 - p->yield_slice_us) >= slice_us) {
                p->ops.yield(p->ops.context);
                if (ph == Z8_NFSP_COPY || ph == Z8_NFSP_PROCESS) {
                    nfsp_hb_progress();
                }
                p->yield_slice_us = (uint32_t)p->ops.time_us(p->ops.context);
            }
        } else if ((p->yield_counter & 31u) == 0u) {
            p->ops.yield(p->ops.context);
            if (ph == Z8_NFSP_COPY || ph == Z8_NFSP_PROCESS) {
                nfsp_hb_progress();
            }
        }
    }
    return __atomic_load_n(&p->cancel, __ATOMIC_ACQUIRE) != 0u;
}

static void free_blocks(struct z8_native_filmsim_photo *p)
{
    while (p->block_count) {
        p->ops.release(p->ops.context, &p->blocks[--p->block_count]);
    }
    p->workspace = 0;
}

static void reap(struct z8_native_filmsim_photo *p)
{
    int should = 0;
    lock_shot(p);
    uint32_t s = phase_load(p);
    if (p->retire && !p->active_engines && !p->cleanup_active &&
        !(__atomic_load_n(&p->source_gate, __ATOMIC_ACQUIRE) & SOURCE_READERS) &&
        (s == Z8_NFSP_READY || s == Z8_NFSP_BYPASS)) {
        if (p->poisoned >= Z8_NFSP_QUARANTINE_THRESHOLD) {
            __atomic_store_n(&p->phase, Z8_NFSP_QUARANTINE, __ATOMIC_RELEASE);
        } else {
            if (p->committed &&
                __atomic_load_n(&p->reason, __ATOMIC_RELAXED) != Z8_NFSP_ENGINE_ERROR) {
                /* Clean committed shot succeeded: reset consecutive engine-error counter. */
                p->poisoned = 0u;
            }
            __atomic_store_n(&p->phase, Z8_NFSP_RETIRING, __ATOMIC_RELEASE);
            should = 1;
        }
    }
    unlock_shot(p);
    if (should) {
        free_blocks(p);
        __atomic_store_n(&p->phase, Z8_NFSP_IDLE, __ATOMIC_RELEASE);
    }
}

static void bypass(struct z8_native_filmsim_photo *p, uint32_t why)
{
    set_reason(p, why);
    free_blocks(p);
    if (p->ops.time_us) {
        p->finish_us = p->ops.time_us(p->ops.context);
    }
    __atomic_store_n(&p->source_reads_done, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&p->phase, Z8_NFSP_BYPASS, __ATOMIC_RELEASE);
    reap(p);
}

/* V71.7-r4 P1 Crash Fix #3: Quarantine Class 0x24 shadow blocks on un-drained XM6 Core 0
 * timeout so free_blocks() is NEVER called while DSP DMA may still write d->y_dma / d->uv_dma! */
static void quarantine_hung_dma(struct z8_native_filmsim_photo *p, uint32_t why)
{
    set_reason(p, why);
    if (p->ops.time_us) {
        p->finish_us = p->ops.time_us(p->ops.context);
    }
    lock_shot(p);
    p->poisoned = Z8_NFSP_QUARANTINE_THRESHOLD;
    __atomic_store_n(&p->source_reads_done, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&p->phase, Z8_NFSP_QUARANTINE, __ATOMIC_RELEASE);
    unlock_shot(p);
}

void z8_nfsp_init(struct z8_native_filmsim_photo *p, const struct z8_nfsp_ops *ops)
{
    if (!p) {
        return;
    }
    memset(p, 0, sizeof(*p));
    if (ops) {
        p->ops = *ops;
    }
}

int z8_nfsp_begin(struct z8_native_filmsim_photo *p, uint64_t ctx,
                  const Z8FilmSimConfig *cfg, uint32_t shot_seq)
{
    if (!p) {
        return 0;
    }
    if (!ctx || !cfg || !z8_filmsim_cfg_validate(cfg) ||
        (z8_filmsim_cfg_is_dormant(cfg) && !nfsp_lut_is_active(0, 0))) {
        __atomic_fetch_add(&p->photos_bypassed, 1u, __ATOMIC_RELAXED);
        return 0;
    }
    if (!p->ops.read || !p->ops.allocate || !p->ops.release ||
        !p->ops.map_source || !p->ops.clean || !p->ops.pool_valid ||
        !p->ops.yield) {
        __atomic_fetch_add(&p->photos_bypassed, 1u, __ATOMIC_RELAXED);
        return 0;
    }

    uint64_t ce = __atomic_load_n(&p->cancel_epoch, __ATOMIC_ACQUIRE);
    lock_shot(p);
    if (phase_load(p) != Z8_NFSP_IDLE) {
        unlock_shot(p);
        __atomic_fetch_add(&p->photos_bypassed, 1u, __ATOMIC_RELAXED);
        return 0;
    }
    __atomic_store_n(&p->phase, Z8_NFSP_CLAIM, __ATOMIC_RELEASE);
    __atomic_store_n(&p->cancel, 0u, __ATOMIC_RELEASE);
    p->retire = 0u;
    __atomic_store_n(&p->reason, 0u, __ATOMIC_RELAXED);
    p->active_engines = 0u;
    p->committed = 0u;
    p->wait_seen = p->wait_failed = 0u;
    p->thumbnail_seen = p->thumbnail_result = 0u;
    p->source_reads_done = p->count = p->block_count = p->yield_counter = 0u;
    p->cleanup_active = 0u;
    __atomic_store_n(&p->source_gate, 0u, __ATOMIC_RELEASE);
    p->kernel_result = 0;
    p->cfg = *cfg;
    p->shot_seq = shot_seq ? shot_seq : 1u;
    p->context = ctx;
    p->stage_start_us = p->copy_done_us = p->finish_us = 0u;
    p->yield_slice_us = 0u;
    p->outer = p->metadata = 0u;
    if (!__atomic_add_fetch(&p->generation, 1u, __ATOMIC_ACQ_REL)) {
        __atomic_add_fetch(&p->generation, 1u, __ATOMIC_ACQ_REL);
    }
    uint64_t g = gen_load(p);
    memset(&p->before_border, 0, sizeof(p->before_border));
    unlock_shot(p);

    uint64_t outer = 0u, metadata = 0u;
    uint32_t context_word = 0u, outer_word = 0u;
    int valid = rd_gen(p, g, ctx + 0x18u, &outer, 8u) &&
                rd_gen(p, g, ctx, &context_word, 4u) &&
                rd_gen(p, g, outer + 0x790u, &metadata, 8u) &&
                rd_gen(p, g, outer + 0x10u, &outer_word, 4u);

    lock_shot(p);
    p->outer = outer;
    p->metadata = metadata;
    p->context_word = context_word;
    p->outer_word = outer_word;
    if (__atomic_load_n(&p->cancel_epoch, __ATOMIC_ACQUIRE) != ce) {
        set_reason(p, Z8_NFSP_CANCELLED);
        __atomic_store_n(&p->cancel, 1u, __ATOMIC_RELEASE);
    }
    if (!valid || p->retire) {
        set_reason(p, p->retire ? Z8_NFSP_CANCELLED : Z8_NFSP_IDENTITY);
        __atomic_store_n(&p->source_reads_done, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&p->phase, Z8_NFSP_BYPASS, __ATOMIC_RELEASE);
        unlock_shot(p);
        reap(p);
        return 0;
    }
    __atomic_store_n(&p->phase, Z8_NFSP_COLLECT, __ATOMIC_RELEASE);
    unlock_shot(p);
    return 1;
}

void z8_nfsp_wait_return(struct z8_native_filmsim_photo *p, uint64_t outer,
                         uint32_t event, uint64_t result)
{
    uint32_t bit = event == 0x80000u ? 1u :
                   event == 0x40000u ? 2u :
                   event == 0x200000u ? 4u :
                   event == 0x40u ? 8u : 0u;
    if (!p || !bit) {
        return;
    }
    lock_shot(p);
    if (phase_load(p) == Z8_NFSP_COLLECT && p->outer == outer && !p->retire) {
        p->wait_seen |= bit;
        if (!(result & 1u)) {
            p->wait_failed |= bit;
        }
    }
    unlock_shot(p);
}

void z8_nfsp_thumbnail_return(struct z8_native_filmsim_photo *p, uint64_t outer,
                              uint64_t result)
{
    if (!p) {
        return;
    }
    lock_shot(p);
    if (phase_load(p) != Z8_NFSP_COLLECT || p->outer != outer || p->retire) {
        unlock_shot(p);
        return;
    }
    struct source_reader reader = {p, gen_load(p)};
    unlock_shot(p);

    struct z8_photo_snapshot before;
    if (result == 1u && !z8_photo_snapshot_outer_read(snapshot_read, &reader, outer, &before)) {
        result = 0u;
    }

    lock_shot(p);
    if (gen_load(p) == reader.generation && phase_load(p) == Z8_NFSP_COLLECT && !p->retire) {
        p->thumbnail_seen = 1u;
        p->thumbnail_result = (uint32_t)result;
        if (result == 1u) {
            p->before_border = before;
        }
    }
    unlock_shot(p);
    reap(p);
}

uint64_t z8_nfsp_context_generation(struct z8_native_filmsim_photo *p, uint64_t ctx)
{
    if (!p) {
        return 0u;
    }
    lock_shot(p);
    uint64_t g = (phase_load(p) == Z8_NFSP_COLLECT && p->context == ctx && !p->retire)
                     ? gen_load(p) : 0u;
    unlock_shot(p);
    return g;
}

int z8_nfsp_prepare_generation(struct z8_native_filmsim_photo *p, uint64_t ctx,
                               uint64_t g, uint64_t start_us, uint64_t timeout_us)
{
    if (!p) {
        return 0;
    }
    lock_shot(p);
    if (!g || gen_load(p) != g || phase_load(p) != Z8_NFSP_COLLECT ||
        ctx != p->context || p->retire) {
        unlock_shot(p);
        return 0;
    }
    __atomic_store_n(&p->phase, Z8_NFSP_PREPARE, __ATOMIC_RELEASE);
    p->stage_start_us = start_us;
    p->yield_slice_us = (uint32_t)start_us;
    (void)timeout_us;
    struct source_reader reader = {p, g};
    int fences = p->wait_seen == Z8_NFSP_WAIT_ALL && !p->wait_failed &&
                 p->thumbnail_seen && p->thumbnail_result == 1u;
    unlock_shot(p);

    if (cancelled_cb(p)) {
        bypass(p, Z8_NFSP_CANCELLED);
        return 0;
    }
    if (!fences) {
        bypass(p, Z8_NFSP_FENCE);
        return 0;
    }
    if (!z8_photo_snapshot_read(snapshot_read, &reader, ctx, &p->snapshot) ||
        p->snapshot.outer != p->outer ||
        p->snapshot.metadata != p->metadata ||
        p->snapshot.context_word != p->context_word ||
        p->snapshot.outer_word10 != p->outer_word) {
        bypass(p, Z8_NFSP_IDENTITY);
        return 0;
    }
    if (cancelled_cb(p)) {
        bypass(p, Z8_NFSP_CANCELLED);
        return 0;
    }
    lock_shot(p);
    if (p->retire || __atomic_load_n(&p->cancel, __ATOMIC_ACQUIRE)) {
        unlock_shot(p);
        bypass(p, Z8_NFSP_CANCELLED);
        return 0;
    }
    __atomic_store_n(&p->phase, Z8_NFSP_PENDING, __ATOMIC_RELEASE);
    unlock_shot(p);
    return 1;
}

static int span_inside(uint64_t a, uint64_t n, uint64_t base, uint32_t cap)
{
    return a >= base && a - base <= cap && n <= (uint64_t)cap - (a - base);
}

static int source_span(struct z8_native_filmsim_photo *p,
                       const struct z8_photo_source_snapshot *s,
                       struct z8_nfsp_plane *d)
{
    struct z8_photo_descriptor q = s->decoded;
    uint64_t block = 0u, base = 0u, yb = 0u, cb = 0u;
    uint32_t actual = 0u;
    uint32_t ox = q.origin_x, oy = q.origin_y;
    q.origin_x = q.origin_y = 0u;

    if (!s->present || s->firmware_done_flag != 1u ||
        z8_photo_nv16_spans(&q, &yb, &cb) != Z8_PHOTO_LAYOUT_OK ||
        ((q.y_stride | q.uv_stride | q.width | ox) & 3u) != 0u ||
        ((q.y_address | q.uv_address) & 63u) != 0u ||
        ox + q.width > q.storage_width ||
        oy + q.height > q.storage_height ||
        yb > UINT32_MAX || cb > UINT32_MAX ||
        !rd(p, s->resource + 8u, &block, 8u) ||
        !rd(p, block, &base, 8u) ||
        !rd(p, block + 8u, &actual, 4u) ||
        !span_inside(q.y_address, yb, base, actual) ||
        !span_inside(q.uv_address, cb, base, actual)) {
        return 0;
    }
    memset(d, 0, sizeof(*d));
    memcpy(d->descriptor, s->raw, sizeof(d->descriptor));
    d->source = s->decoded;
    d->y_bytes = (uint32_t)yb;
    d->uv_bytes = (uint32_t)cb;
    d->roi_x = (uint16_t)ox;
    d->roi_y = (uint16_t)oy;
    d->roi_w = q.width;
    d->roi_h = q.height;
    return 1;
}

static int plan_group(struct z8_native_filmsim_photo *p)
{
    const struct z8_photo_snapshot *s = &p->snapshot;
    if (!s->sources[Z8_PHOTO_MAIN].present || s->sources[Z8_PHOTO_AUX].present) {
        return 0;
    }
    memset(p->role_plane, 0xff, sizeof(p->role_plane));
    for (uint32_t i = 0u; i < Z8_PHOTO_SOURCE_COUNT; ++i) {
        if (!s->sources[i].present) {
            continue;
        }
        uint32_t j = 0u;
        for (; j < p->count; ++j) {
            if (memcmp(p->planes[j].descriptor, s->sources[i].raw,
                       Z8_PHOTO_DESCRIPTOR_BYTES) == 0) {
                break;
            }
        }
        if (j == p->count) {
            if (!source_span(p, &s->sources[i], &p->planes[j])) {
                return 0;
            }
            ++p->count;
        }
        p->role_plane[i] = (uint8_t)j;
        /* Preserve thumbnail's pre-border active rectangle so letterbox margins stay black. */
        if (i == Z8_PHOTO_THUMBNAIL) {
            const struct z8_photo_descriptor *b = &p->before_border.sources[i].decoded;
            struct z8_nfsp_plane *d = &p->planes[j];
            if (!p->before_border.sources[i].present ||
                b->y_address != d->source.y_address ||
                b->uv_address != d->source.uv_address ||
                b->y_stride != d->source.y_stride ||
                b->uv_stride != d->source.uv_stride ||
                b->storage_width != d->source.storage_width ||
                b->storage_height != d->source.storage_height ||
                ((b->origin_x | b->width) & 3u) != 0u ||
                !b->width || !b->height ||
                (uint32_t)b->origin_x + b->width > b->storage_width ||
                (uint32_t)b->origin_y + b->height > b->storage_height) {
                return 0;
            }
            d->roi_x = b->origin_x;
            d->roi_y = b->origin_y;
            d->roi_w = b->width;
            d->roi_h = b->height;
        }
    }
    const struct z8_nfsp_plane *main_pl = &p->planes[p->role_plane[Z8_PHOTO_MAIN]];
    if (main_pl->roi_w < 16u || main_pl->roi_h < 16u) {
        return 0;
    }
    for (uint32_t j = 0u; j < p->count; ++j) {
        const struct z8_nfsp_plane *d = &p->planes[j];
        int64_t delta = (int64_t)main_pl->roi_w * d->roi_h -
                        (int64_t)main_pl->roi_h * d->roi_w;
        if (delta < 0) {
            delta = -delta;
        }
        /* Tolerance 4*(W+H) covers stock crops + NFP v0.5.0 4:3, 65:24 and 2.39:1 L/M/S
         * (2.39:1 L/M/S max |main_w*d_h - main_h*d_w| = 32,256 vs 4*(8256+3456)=46,848),
         * while still rejecting genuinely mismatched aspect ratios (>500,000). */
        if (delta > 4 * (int64_t)(main_pl->roi_w + main_pl->roi_h)) {
            return 0;
        }
    }
    return p->count > 0u && p->count <= Z8_FILMSIM_MAX_PLANES;
}

static uint8_t *arena_alloc(struct z8_native_filmsim_photo *p,
                            uint32_t n, uint32_t requested, uint64_t *dma_out)
{
    for (uint32_t i = 0u; i < p->block_count; ++i) {
        struct z8_nfsp_allocation *b = &p->blocks[i];
        uint64_t at = ((uint64_t)b->used + 511u) & ~UINT64_C(511);
        if (at <= b->actual && n <= b->actual - at) {
            b->used = (uint32_t)at + n;
            *dma_out = b->dma + at;
            return b->cpu + at;
        }
    }
    if (p->block_count == Z8_NFSP_MAX_BLOCKS || cancelled_cb(p)) {
        return 0;
    }
    struct z8_nfsp_allocation *b = &p->blocks[p->block_count];
    memset(b, 0, sizeof(*b));
    if (!p->ops.allocate(p->ops.context, requested, b)) {
        return 0;
    }
    ++p->block_count;
    if (cancelled_cb(p)) {
        return 0;
    }
    if (!b->handle || !b->cpu || !b->dma ||
        (b->dma & 511u) != 0u || ((uintptr_t)b->cpu & 511u) != 0u ||
        b->actual < n ||
        b->dma > UINT64_MAX - b->actual ||
        (uintptr_t)b->cpu > UINTPTR_MAX - b->actual) {
        return 0;
    }
    /* Strict disjointness against all native input planes and prior private blocks */
    for (uint32_t i = 0u; i < p->count; ++i) {
        const struct z8_nfsp_plane *q = &p->planes[i];
        if ((b->dma < q->source.y_address + q->y_bytes && q->source.y_address < b->dma + b->actual) ||
            (b->dma < q->source.uv_address + q->uv_bytes && q->source.uv_address < b->dma + b->actual)) {
            return 0;
        }
    }
    for (uint32_t i = 0u; i + 1u < p->block_count; ++i) {
        const struct z8_nfsp_allocation *q = &p->blocks[i];
        if (b->dma < q->dma + q->actual && q->dma < b->dma + b->actual) {
            return 0;
        }
    }
    /* V71.5: Skip redundant pre-copy dc civac on freshly allocated Class 0x24 block:
     * copy_rows overwrites every destination plane before any DMA reader observes it,
     * and lines below explicitly dc civac every written plane + workspace before READY. */
    b->used = n;
    *dma_out = b->dma;
    return b->cpu;
}

static int copy_rows(struct z8_native_filmsim_photo *p, uint8_t *dest,
                     uint64_t source, uint32_t stride, uint32_t rows)
{
    if (!stride || !rows) {
        return 1;
    }
    /* V71: batch contiguous rows per map_source / dc ivac (<= 480 KiB, safely below
     * native 0x88000 = 544 KiB assert guard even after 64B line rounding). */
    uint32_t max_chunk_rows = 491520u / stride;
    if (max_chunk_rows == 0u) {
        max_chunk_rows = 1u;
    }
    uint32_t r = 0u;
    while (r < rows) {
        if (cancelled_cb(p)) {
            return 0;
        }
        uint32_t chunk = rows - r;
        if (chunk > max_chunk_rows) {
            chunk = max_chunk_rows;
        }
        uint32_t bytes = chunk * stride;
        if (!source_enter(p, gen_load(p))) {
            return 0;
        }
        const uint8_t *s = p->ops.map_source(p->ops.context,
                                             source + (uint64_t)r * stride, bytes);
        if (s) {
            if (nfsp_neon_allowed()) {
                zf_grain_copy_u8_fast(dest + (size_t)r * stride, s, bytes);
            } else {
                memcpy(dest + (size_t)r * stride, s, bytes);
            }
        }
        source_leave(p);
        if (!s) {
            return 0;
        }
        r += chunk;
    }
    return 1;
}

void z8_nfsp_worker(struct z8_native_filmsim_photo *p)
{
    uint32_t expected = Z8_NFSP_PENDING;
    if (!p) {
        return;
    }
    z8_nfsp_reclaim_cancelled(p);
    reap(p);
    /* V71 Post-Roll Starvation Fix: defer starting photo_worker (CPU2 prio 30)
     * BEFORE CAS(PENDING->COPY) & BEFORE ~100MB class24 alloc while Live post-roll
     * (job==1) is active, bounded by 2.0s hard cap and bypassed on cancel/retire. */
    if (__atomic_load_n(&p->phase, __ATOMIC_ACQUIRE) == Z8_NFSP_PENDING &&
        !__atomic_load_n(&p->cancel, __ATOMIC_ACQUIRE) && !p->retire &&
        p->ops.should_defer && p->ops.should_defer(p->ops.context, p->stage_start_us)) {
        return;
    }
    if (!__atomic_compare_exchange_n(&p->phase, &expected, Z8_NFSP_COPY, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (p->ops.time_us) {
        p->yield_slice_us = (uint32_t)p->ops.time_us(p->ops.context);
    }

    int frame_held = 0;
    int hb_entered = 0;
    int assets_held = 0;
    uint32_t abort_reason = Z8_NFSP_CANCELLED;
    int abort_count_fail = 0;
    int abort_count_bypass = 0;
    int hung_dma_abort = 0;

    if (!nfsp_frame_take(&frame_held)) {
        bypass(p, Z8_NFSP_CANCELLED);
        return;
    }
    nfsp_hb_enter(FX_Z8_STAGE_JPEG_GRADE, p->shot_seq, (uint32_t)(p->generation & UINT32_C(0xffffffff)));
    hb_entered = 1;

    if (cancelled_cb(p)) {
        abort_reason = Z8_NFSP_CANCELLED;
        goto abort_shot;
    }
    if (!plan_group(p)) {
        abort_reason = Z8_NFSP_SOURCE_SPAN;
        abort_count_fail = 1;
        goto abort_shot;
    }
    nfsp_flight(FX_Z8_FLIGHT_JPEG_MAIN_ENTER, 0u, p->shot_seq, p->count,
                p->planes[p->role_plane[Z8_PHOTO_MAIN]].roi_w);

    z8_filmsim_assets assets;
    memset(&assets, 0, sizeof(assets));
    if (p->ops.acquire_assets) {
        assets_held = p->ops.acquire_assets(p->ops.context, &assets);
    }

    uint32_t lut_gen = 0u;
    int lut_is_1d = 0;
    int want_lut = nfsp_lut_is_active(&lut_gen, &lut_is_1d);
    int want_hdf = z8_filmsim_cfg_needs_hdf(&p->cfg);
    int want_grain = z8_filmsim_cfg_needs_grain(&p->cfg);
    int run_grain = want_grain && (assets.zf_maps != 0);

    if (!want_lut && !want_hdf && !want_grain) {
        abort_reason = Z8_NFSP_DISABLED;
        abort_count_bypass = 1;
        goto abort_shot;
    }

    /* V71.5 Grain-Only Y-only allocation: Nikon Zf grain modifies Y strictly without
     * touching UV, so when BOTH LUT and HDF are OFF we keep descriptor+32 at native
     * source.uv_address (0-touch, 100% safe for RAW+JPG Job 2) and cut Class 0x24 RAM in half! */
    int need_uv = want_lut || want_hdf;
    uint32_t ws_bytes = want_hdf ? Z8_FILMSIM_WORKSPACE_BYTES :
                        (want_grain ? Z8_FILMSIM_GRAIN_WS_MIN : 0u);
    uint32_t lut_cache_bytes = want_lut ? UINT32_C(0x400000) : 0u; /* Private 4 MiB cache slice */
    uint64_t total = (uint64_t)ws_bytes + (uint64_t)lut_cache_bytes + 4096u;
    for (uint32_t j = 0u; j < p->count; ++j) {
        total += (uint64_t)p->planes[j].y_bytes +
                 (need_uv ? (uint64_t)p->planes[j].uv_bytes : 0u) + 1024u;
    }
    if (total > UINT32_MAX) {
        abort_reason = Z8_NFSP_POOL;
        abort_count_fail = 1;
        goto abort_shot;
    }

    uint32_t main_idx = p->role_plane[Z8_PHOTO_MAIN];
    memset(&p->job, 0, sizeof(p->job));
    p->job.abi = Z8_FILMSIM_PIPELINE_ABI;
    p->job.count = p->count;
    p->job.analysis_index = main_idx;
    p->job.shot_id = ((uint64_t)p->shot_seq << 32) ^
                     ((uint64_t)(p->generation & UINT32_C(0xffffffff))) ^
                     (p->stage_start_us * UINT64_C(0x9e3779b97f4a7c15));
    if (p->job.shot_id == 0u) {
        p->job.shot_id = 1u;
    }
    p->job.cfg = p->cfg;
    p->job.assets = assets;
    p->job.cancelled = cancelled_cb;
    p->job.cancel_ctx = p;
    p->job.stages_applied_out = &p->last_stages_applied;

    for (uint32_t j = 0u; j < p->count; ++j) {
        struct z8_nfsp_plane *d = &p->planes[j];
        d->y = arena_alloc(p, d->y_bytes, (uint32_t)total, &d->y_dma);
        total -= d->y_bytes;
        if (need_uv) {
            d->uv = arena_alloc(p, d->uv_bytes, (uint32_t)total, &d->uv_dma);
            total -= d->uv_bytes;
        } else {
            d->uv = 0;
            d->uv_dma = d->source.uv_address;
        }
        if (!d->y || (need_uv && !d->uv)) {
            abort_reason = Z8_NFSP_POOL;
            abort_count_fail = 1;
            goto abort_shot;
        }
        put64_le(d->descriptor + 24u, d->y_dma);
        if (need_uv) {
            put64_le(d->descriptor + 32u, d->uv_dma);
        }
        size_t yo = (size_t)d->roi_y * d->source.y_stride + d->roi_x;
        size_t co = (size_t)d->roi_y * d->source.uv_stride + d->roi_x;
        p->job.planes[j].y = d->y + yo;
        p->job.planes[j].c = need_uv ? (d->uv + co) : 0;
        p->job.planes[j].y_bytes = d->y_bytes - yo;
        p->job.planes[j].c_bytes = need_uv ? (d->uv_bytes - co) : 0u;
        p->job.planes[j].width = d->roi_w;
        p->job.planes[j].height = d->roi_h;
        p->job.planes[j].y_stride = d->source.y_stride;
        p->job.planes[j].c_stride = need_uv ? d->source.uv_stride : 0u;
        p->job.planes[j].format = Z8_FS_FMT_NV16;
        p->job.planes[j].is_full_res = (j == main_idx) ? 1u : 0u;
    }

    uint64_t ws_dma = 0u;
    if (ws_bytes > 0u) {
        p->workspace = arena_alloc(p, ws_bytes, (uint32_t)total, &ws_dma);
        total -= ws_bytes;
        if (!p->workspace) {
            abort_reason = Z8_NFSP_POOL;
            abort_count_fail = 1;
            goto abort_shot;
        }
    } else {
        p->workspace = 0;
    }

    uint32_t *lut_cache = 0;
    uint32_t lut_cache_gen = 0u;
    int lut_cache_cpu_dirty = 0;
    if (lut_cache_bytes > 0u) {
        uint64_t cache_dma = 0u;
        lut_cache = (uint32_t *)(void *)arena_alloc(p, lut_cache_bytes, (uint32_t)total, &cache_dma);
        total -= lut_cache_bytes;
        if (!lut_cache) {
            abort_reason = Z8_NFSP_POOL;
            abort_count_fail = 1;
            goto abort_shot;
        }
    }

    struct z8_xm6_bridge *xm6 = (struct z8_xm6_bridge *)assets.xm6_bridge;
    const Z8Xm6BridgeOps *xops = (xm6 && xm6->ops) ? xm6->ops : 0;
    int xm6_auto = (xops && p->cfg.xm6_mode == Z8_XM6_MODE_AUTO &&
                    ((want_lut && lut_is_1d) || want_hdf || run_grain));
    int try_xm6 = xm6_auto && xops->hw_ready(xm6, p->cfg.xm6_mode);
    if (xm6_auto && !try_xm6) {
        xops->record_fallback(xm6, Z8_XM6_ERR_NOT_READY);
    }

    Z8Xm6RgblutCurves lut_curves;
    int have_1d_curves = 0;
    if (want_lut && lut_is_1d && try_xm6 && xops->still_rgblut) {
        have_1d_curves = nfsp_lut_get_curves(&lut_curves);
    }

    uint8_t plane_copied[Z8_PHOTO_SOURCE_COUNT];
    uint8_t plane_lut_done[Z8_PHOTO_SOURCE_COUNT];
    uint8_t plane_hdf_done[Z8_PHOTO_SOURCE_COUNT];
    uint8_t plane_grain_done[Z8_PHOTO_SOURCE_COUNT];
    uint8_t plane_cpu_dirty[Z8_PHOTO_SOURCE_COUNT];
    memset(plane_copied, 0, sizeof(plane_copied));
    memset(plane_lut_done, 0, sizeof(plane_lut_done));
    memset(plane_hdf_done, 0, sizeof(plane_hdf_done));
    memset(plane_grain_done, 0, sizeof(plane_grain_done));
    memset(plane_cpu_dirty, 0, sizeof(plane_cpu_dirty));

    uint64_t xm6_total_us = 0u;
    int any_xm6_ok = 0;

    for (uint32_t j = 0u; j < p->count; ++j) {
        struct z8_nfsp_plane *d = &p->planes[j];
        if (cancelled_cb(p)) {
            abort_reason = Z8_NFSP_CANCELLED;
            abort_count_fail = 1;
            goto abort_shot;
        }
        nfsp_hb_progress();

        if (!want_lut && try_xm6 && xops->hw_ready(xm6, p->cfg.xm6_mode)) {
            int is_full_frame = (d->roi_x == 0u && d->roi_y == 0u &&
                                 d->roi_w == d->source.storage_width &&
                                 d->roi_h == d->source.storage_height);
            uint8_t reduced_res = (j == main_idx) ? 0u : 1u;
            uint32_t grain_seed = xops->fold_grain_seed(
                p->job.shot_id ^ ((uint64_t)j * UINT64_C(0x9E3779B97F4A7C15)));
            int can_grain_xm6 = run_grain && (assets.xm6_zf_raw_buf != 0);
            uint32_t dt_us = 0u;

            if (is_full_frame) {
                /* Full-frame zero-copy out-of-place: src=source.dma -> out=shadow.dma
                 * (skips ~90 MiB copy_rows AND ~90 MiB D-Cache clean!). */
                if (want_hdf) {
                    if (!source_enter(p, gen_load(p))) {
                        abort_reason = Z8_NFSP_CANCELLED;
                        abort_count_fail = 1;
                        goto abort_shot;
                    }
                    int hrc = xops->still_hdf(xm6, p->cfg.xm6_mode,
                                              d->source.y_address, d->source.uv_address,
                                              d->y_dma, d->uv_dma,
                                              d->roi_w, d->roi_h,
                                              d->source.y_stride, d->source.uv_stride,
                                              d->y_bytes, d->uv_bytes,
                                              p->cfg.hdf_level, Z8_XM6_FMT_NV16, &dt_us);
                    source_leave(p);

                    if (nfsp_xm6_is_hung_dma(hrc, xm6)) {
                        hung_dma_abort = 1;
                        abort_reason = Z8_NFSP_TIMEOUT;
                        abort_count_fail = 1;
                        goto abort_shot;
                    }
                    if (hrc == Z8_XM6_STILL_OK) {
                        plane_copied[j] = 1u;
                        plane_hdf_done[j] = 1u;
                        plane_cpu_dirty[j] = 0u;
                        any_xm6_ok = 1;
                        xm6_total_us += dt_us;

                        if (run_grain) {
                            int grc = Z8_XM6_STILL_FALLBACK_CLEAN;
                            if (can_grain_xm6 && xops->hw_ready(xm6, p->cfg.xm6_mode)) {
                                /* Stage 2: Slot 39 in-place directly on shadow.dma Y in DRAM */
                                grc = xops->still_grain(xm6, p->cfg.xm6_mode,
                                                        d->y_dma, d->y_dma,
                                                        d->roi_w, d->roi_h,
                                                        d->source.y_stride, d->y_bytes,
                                                        p->cfg.zf_intensity, p->cfg.zf_size,
                                                        p->cfg.zf_blur_mode, reduced_res,
                                                        grain_seed, assets.xm6_zf_raw_buf, &dt_us);
                            }
                            if (nfsp_xm6_is_hung_dma(grc, xm6)) {
                                hung_dma_abort = 1;
                                abort_reason = Z8_NFSP_TIMEOUT;
                                abort_count_fail = 1;
                                goto abort_shot;
                            }
                            if (grc == Z8_XM6_STILL_OK) {
                                plane_grain_done[j] = 1u;
                                xm6_total_us += dt_us;
                            } else if (grc == Z8_XM6_STILL_FALLBACK_CLEAN) {
                                /* Shadow DRAM holds clean Stage 1 HDF; invalidate CPU cache before ARM Grain */
                                (void)p->ops.clean(p->ops.context, d->y, d->y_bytes);
                            } else {
                                /* Mid-execution timeout on Stage 2: source.dma is still 100% pristine */
                                plane_copied[j] = 0u;
                                plane_hdf_done[j] = 0u;
                                plane_grain_done[j] = 0u;
                            }
                        }
                    }
                } else if (can_grain_xm6) {
                    /* Grain-only out-of-place: src=source.y_address -> out=shadow.y_dma */
                    if (!source_enter(p, gen_load(p))) {
                        abort_reason = Z8_NFSP_CANCELLED;
                        abort_count_fail = 1;
                        goto abort_shot;
                    }
                    int grc = xops->still_grain(xm6, p->cfg.xm6_mode,
                                                d->source.y_address, d->y_dma,
                                                d->roi_w, d->roi_h,
                                                d->source.y_stride, d->y_bytes,
                                                p->cfg.zf_intensity, p->cfg.zf_size,
                                                p->cfg.zf_blur_mode, reduced_res,
                                                grain_seed, assets.xm6_zf_raw_buf, &dt_us);
                    source_leave(p);

                    if (nfsp_xm6_is_hung_dma(grc, xm6)) {
                        hung_dma_abort = 1;
                        abort_reason = Z8_NFSP_TIMEOUT;
                        abort_count_fail = 1;
                        goto abort_shot;
                    }
                    if (grc == Z8_XM6_STILL_OK) {
                        plane_copied[j] = 1u;
                        plane_grain_done[j] = 1u;
                        plane_cpu_dirty[j] = 0u;
                        any_xm6_ok = 1;
                        xm6_total_us += dt_us;
                    }
                }
            } else {
                /* Cropped ROI / letterboxed thumbnail: preserve borders via copy_rows first,
                 * then chunked clean + in-place XM6 if ROI lies inside Core 0 IOMMU. */
                if (!copy_rows(p, d->y, d->source.y_address, d->source.y_stride, d->source.storage_height) ||
                    (d->uv && !copy_rows(p, d->uv, d->source.uv_address, d->source.uv_stride, d->source.storage_height))) {
                    abort_reason = Z8_NFSP_CANCELLED;
                    abort_count_fail = 1;
                    goto abort_shot;
                }
                plane_copied[j] = 1u;
                plane_cpu_dirty[j] = 1u;

                size_t yo = (size_t)d->roi_y * d->source.y_stride + d->roi_x;
                size_t co = (size_t)d->roi_y * d->source.uv_stride + d->roi_x;
                uint32_t y_roi_span = (uint32_t)(d->y_bytes - yo);
                uint32_t c_roi_span = need_uv ? (uint32_t)(d->uv_bytes - co) : 0u;
                int iommu_ok = xops->check_iommu(xm6, d->y_dma + yo, y_roi_span) &&
                               (!want_hdf || xops->check_iommu(xm6, d->uv_dma + co, c_roi_span));
                if (iommu_ok) {
                    if (p->ops.clean(p->ops.context, d->y, d->y_bytes) &&
                        (!d->uv || p->ops.clean(p->ops.context, d->uv, d->uv_bytes))) {
                        plane_cpu_dirty[j] = 0u;
                        int dirty_abort = 0;
                        if (want_hdf) {
                            int hrc = xops->still_hdf(xm6, p->cfg.xm6_mode,
                                                      d->y_dma + yo, d->uv_dma + co,
                                                      d->y_dma + yo, d->uv_dma + co,
                                                      d->roi_w, d->roi_h,
                                                      d->source.y_stride, d->source.uv_stride,
                                                      y_roi_span, c_roi_span,
                                                      p->cfg.hdf_level, Z8_XM6_FMT_NV16, &dt_us);
                            if (nfsp_xm6_is_hung_dma(hrc, xm6)) {
                                hung_dma_abort = 1;
                                abort_reason = Z8_NFSP_TIMEOUT;
                                abort_count_fail = 1;
                                goto abort_shot;
                            }
                            if (hrc == Z8_XM6_STILL_OK) {
                                plane_hdf_done[j] = 1u;
                                any_xm6_ok = 1;
                                xm6_total_us += dt_us;
                            } else if (hrc == Z8_XM6_STILL_FALLBACK_DIRTY) {
                                dirty_abort = 1;
                            }
                        }
                        if (!dirty_abort && run_grain && (!want_hdf || plane_hdf_done[j])) {
                            int grc = Z8_XM6_STILL_FALLBACK_CLEAN;
                            if (can_grain_xm6 && xops->hw_ready(xm6, p->cfg.xm6_mode)) {
                                grc = xops->still_grain(xm6, p->cfg.xm6_mode,
                                                        d->y_dma + yo, d->y_dma + yo,
                                                        d->roi_w, d->roi_h,
                                                        d->source.y_stride, y_roi_span,
                                                        p->cfg.zf_intensity, p->cfg.zf_size,
                                                        p->cfg.zf_blur_mode, reduced_res,
                                                        grain_seed, assets.xm6_zf_raw_buf, &dt_us);
                            }
                            if (nfsp_xm6_is_hung_dma(grc, xm6)) {
                                hung_dma_abort = 1;
                                abort_reason = Z8_NFSP_TIMEOUT;
                                abort_count_fail = 1;
                                goto abort_shot;
                            }
                            if (grc == Z8_XM6_STILL_OK) {
                                plane_grain_done[j] = 1u;
                                any_xm6_ok = 1;
                                xm6_total_us += dt_us;
                            } else if (grc == Z8_XM6_STILL_FALLBACK_DIRTY) {
                                dirty_abort = 1;
                            } else if (plane_hdf_done[j]) {
                                (void)p->ops.clean(p->ops.context, d->y, d->y_bytes);
                            }
                        }
                        if (dirty_abort) {
                            /* Re-copy pristine source rows before ARM NEON fallback */
                            plane_copied[j] = 0u;
                            plane_hdf_done[j] = 0u;
                            plane_grain_done[j] = 0u;
                        }
                    }
                } else {
                    xops->record_fallback(xm6, Z8_XM6_ERR_IOMMU_OOB);
                }
            }
        } else if (want_lut && have_1d_curves && try_xm6 && xops->still_rgblut &&
                   xops->hw_ready(xm6, p->cfg.xm6_mode)) {
            /* V71.7 Stage 0 Fast Path: Full-frame 1D separable .cube LUT via XM6 Op12
             * out-of-place (source.dma -> shadow.dma), chaining Stage 1 HDF + Stage 2 Grain
             * directly in DRAM with 0 CPU copy & 0 D-cache clean! */
            int is_full_frame = (d->roi_x == 0u && d->roi_y == 0u &&
                                 d->roi_w == d->source.storage_width &&
                                 d->roi_h == d->source.storage_height &&
                                 d->source.y_stride == d->source.uv_stride);
            if (is_full_frame) {
                uint32_t dt_us = 0u;
                nfsp_hb_enter(FX_Z8_STAGE_JPEG_XM6, p->shot_seq, j);
                if (!source_enter(p, gen_load(p))) {
                    abort_reason = Z8_NFSP_CANCELLED;
                    abort_count_fail = 1;
                    goto abort_shot;
                }
                int lrc = xops->still_rgblut(xm6, p->cfg.xm6_mode, &lut_curves,
                                             0, 0,
                                             d->source.y_address, d->source.uv_address,
                                             0, 0,
                                             d->y_dma, d->uv_dma,
                                             d->roi_w, d->roi_h,
                                             d->source.y_stride, &dt_us);
                source_leave(p);

                if (nfsp_xm6_is_hung_dma(lrc, xm6)) {
                    hung_dma_abort = 1;
                    abort_reason = Z8_NFSP_TIMEOUT;
                    abort_count_fail = 1;
                    goto abort_shot;
                }
                if (lrc == Z8_XM6_STILL_OK) {
                    plane_copied[j] = 1u;
                    plane_lut_done[j] = 1u;
                    plane_cpu_dirty[j] = 0u;
                    any_xm6_ok = 1;
                    xm6_total_us += dt_us;

                    uint8_t reduced_res = (j == main_idx) ? 0u : 1u;
                    uint32_t grain_seed = xops->fold_grain_seed(
                        p->job.shot_id ^ ((uint64_t)j * UINT64_C(0x9E3779B97F4A7C15)));
                    int can_grain_xm6 = run_grain && (assets.xm6_zf_raw_buf != 0);
                    int dirty_abort = 0;

                    if (want_hdf) {
                        int hrc = Z8_XM6_STILL_FALLBACK_CLEAN;
                        if (xops->hw_ready(xm6, p->cfg.xm6_mode)) {
                            hrc = xops->still_hdf(xm6, p->cfg.xm6_mode,
                                                  d->y_dma, d->uv_dma,
                                                  d->y_dma, d->uv_dma,
                                                  d->roi_w, d->roi_h,
                                                  d->source.y_stride, d->source.uv_stride,
                                                  d->y_bytes, d->uv_bytes,
                                                  p->cfg.hdf_level, Z8_XM6_FMT_NV16, &dt_us);
                        }
                        if (nfsp_xm6_is_hung_dma(hrc, xm6)) {
                            hung_dma_abort = 1;
                            abort_reason = Z8_NFSP_TIMEOUT;
                            abort_count_fail = 1;
                            goto abort_shot;
                        }
                        if (hrc == Z8_XM6_STILL_OK) {
                            plane_hdf_done[j] = 1u;
                            xm6_total_us += dt_us;
                        } else if (hrc == Z8_XM6_STILL_FALLBACK_DIRTY) {
                            dirty_abort = 1;
                        } else {
                            /* Clean Stage 0 DRAM output -> invalidate CPU cache before ARM HDF */
                            (void)p->ops.clean(p->ops.context, d->y, d->y_bytes);
                            (void)p->ops.clean(p->ops.context, d->uv, d->uv_bytes);
                        }
                    }

                    if (!dirty_abort && run_grain && (!want_hdf || plane_hdf_done[j])) {
                        int grc = Z8_XM6_STILL_FALLBACK_CLEAN;
                        if (can_grain_xm6 && xops->hw_ready(xm6, p->cfg.xm6_mode)) {
                            grc = xops->still_grain(xm6, p->cfg.xm6_mode,
                                                    d->y_dma, d->y_dma,
                                                    d->roi_w, d->roi_h,
                                                    d->source.y_stride, d->y_bytes,
                                                    p->cfg.zf_intensity, p->cfg.zf_size,
                                                    p->cfg.zf_blur_mode, reduced_res,
                                                    grain_seed, assets.xm6_zf_raw_buf, &dt_us);
                        }
                        if (nfsp_xm6_is_hung_dma(grc, xm6)) {
                            hung_dma_abort = 1;
                            abort_reason = Z8_NFSP_TIMEOUT;
                            abort_count_fail = 1;
                            goto abort_shot;
                        }
                        if (grc == Z8_XM6_STILL_OK) {
                            plane_grain_done[j] = 1u;
                            xm6_total_us += dt_us;
                        } else if (grc == Z8_XM6_STILL_FALLBACK_DIRTY) {
                            dirty_abort = 1;
                        } else {
                            (void)p->ops.clean(p->ops.context, d->y, d->y_bytes);
                        }
                    }

                    if (dirty_abort) {
                        plane_copied[j] = 0u;
                        plane_lut_done[j] = 0u;
                        plane_hdf_done[j] = 0u;
                        plane_grain_done[j] = 0u;
                    }
                }
            }
        }

        if (!plane_copied[j]) {
            if (!copy_rows(p, d->y, d->source.y_address, d->source.y_stride, d->source.storage_height) ||
                (d->uv && !copy_rows(p, d->uv, d->source.uv_address, d->source.uv_stride, d->source.storage_height))) {
                abort_reason = Z8_NFSP_CANCELLED;
                abort_count_fail = 1;
                goto abort_shot;
            }
            plane_copied[j] = 1u;
            plane_cpu_dirty[j] = 1u;
        }
    }

    struct z8_photo_snapshot now;
    struct source_reader reader = {p, gen_load(p)};
    if (!z8_photo_snapshot_read(snapshot_read, &reader, p->context, &now) ||
        memcmp(&now, &p->snapshot, sizeof(now)) != 0) {
        abort_reason = Z8_NFSP_CHANGED;
        abort_count_fail = 1;
        goto abort_shot;
    }

    __atomic_store_n(&p->source_reads_done, 1u, __ATOMIC_RELEASE);
    if (p->ops.time_us) {
        p->copy_done_us = p->ops.time_us(p->ops.context);
    }
    __atomic_store_n(&p->phase, Z8_NFSP_PROCESS, __ATOMIC_RELEASE);
    nfsp_hb_enter(FX_Z8_STAGE_JPEG_GRADE, p->shot_seq, (uint32_t)(want_lut | (want_hdf << 1) | (run_grain << 2)));
    nfsp_flight(FX_Z8_FLIGHT_JPEG_GRADE_START, 0u, p->shot_seq,
                (uint32_t)(want_lut | (want_hdf << 1) | (run_grain << 2)), (uint32_t)lut_is_1d);

    /* V71.7 Stage 0: Apply 3D/fallback .cube LUT via SMP + NEON/Scalar (with 4 MiB cache)
     * strictly BEFORE Stage 1 HDF & Stage 2 Grain. */
    if (want_lut) {
        for (uint32_t j = 0u; j < p->count; ++j) {
            if (plane_lut_done[j]) {
                continue;
            }
            if (cancelled_cb(p)) {
                p->kernel_result = Z8_FS_CANCELLED;
                goto process_done;
            }
            struct z8_nfsp_plane *d = &p->planes[j];
            size_t yo = (size_t)d->roi_y * d->source.y_stride + d->roi_x;
            size_t co = (size_t)d->roi_y * d->source.uv_stride + d->roi_x;
            int lrc = nfsp_lut_apply_still_nv16(d->y + yo, d->uv + co,
                                                d->roi_w, d->roi_h,
                                                d->source.y_stride, d->source.uv_stride,
                                                lut_cache, &lut_cache_gen);
            if (lut_cache) {
                lut_cache_cpu_dirty = 1;
            }
            nfsp_hb_progress();
            if (lrc <= 0) {
                p->kernel_result = Z8_FS_E_INTERNAL;
                goto process_done;
            }
            plane_lut_done[j] = 1u;
            plane_cpu_dirty[j] = 1u;

            /* Optional XM6 Stage 1 HDF + Stage 2 Grain chaining directly after CPU 3D LUT */
            if ((want_hdf || run_grain) && try_xm6 && xops->hw_ready(xm6, p->cfg.xm6_mode)) {
                uint32_t y_roi_span = (uint32_t)(d->y_bytes - yo);
                uint32_t c_roi_span = (uint32_t)(d->uv_bytes - co);
                int iommu_ok = xops->check_iommu(xm6, d->y_dma + yo, y_roi_span) &&
                               (!want_hdf || xops->check_iommu(xm6, d->uv_dma + co, c_roi_span));
                if (iommu_ok &&
                    p->ops.clean(p->ops.context, d->y, d->y_bytes) &&
                    p->ops.clean(p->ops.context, d->uv, d->uv_bytes)) {
                    uint8_t reduced_res = (j == main_idx) ? 0u : 1u;
                    uint32_t grain_seed = xops->fold_grain_seed(
                        p->job.shot_id ^ ((uint64_t)j * UINT64_C(0x9E3779B97F4A7C15)));
                    int can_grain_xm6 = run_grain && (assets.xm6_zf_raw_buf != 0);
                    uint32_t dt_us = 0u;
                    plane_cpu_dirty[j] = 0u;

                    if (want_hdf) {
                        int hrc = xops->still_hdf(xm6, p->cfg.xm6_mode,
                                                  d->y_dma + yo, d->uv_dma + co,
                                                  d->y_dma + yo, d->uv_dma + co,
                                                  d->roi_w, d->roi_h,
                                                  d->source.y_stride, d->source.uv_stride,
                                                  y_roi_span, c_roi_span,
                                                  p->cfg.hdf_level, Z8_XM6_FMT_NV16, &dt_us);
                        if (nfsp_xm6_is_hung_dma(hrc, xm6)) {
                            hung_dma_abort = 1;
                            abort_reason = Z8_NFSP_TIMEOUT;
                            abort_count_fail = 1;
                            goto abort_shot;
                        }
                        if (hrc == Z8_XM6_STILL_OK) {
                            plane_hdf_done[j] = 1u;
                            any_xm6_ok = 1;
                            xm6_total_us += dt_us;
                        } else if (hrc == Z8_XM6_STILL_FALLBACK_DIRTY) {
                            p->kernel_result = Z8_FS_E_INTERNAL;
                            goto process_done;
                        }
                    }
                    if (run_grain && (!want_hdf || plane_hdf_done[j])) {
                        int grc = Z8_XM6_STILL_FALLBACK_CLEAN;
                        if (can_grain_xm6 && xops->hw_ready(xm6, p->cfg.xm6_mode)) {
                            grc = xops->still_grain(xm6, p->cfg.xm6_mode,
                                                    d->y_dma + yo, d->y_dma + yo,
                                                    d->roi_w, d->roi_h,
                                                    d->source.y_stride, y_roi_span,
                                                    p->cfg.zf_intensity, p->cfg.zf_size,
                                                    p->cfg.zf_blur_mode, reduced_res,
                                                    grain_seed, assets.xm6_zf_raw_buf, &dt_us);
                        }
                        if (nfsp_xm6_is_hung_dma(grc, xm6)) {
                            hung_dma_abort = 1;
                            abort_reason = Z8_NFSP_TIMEOUT;
                            abort_count_fail = 1;
                            goto abort_shot;
                        }
                        if (grc == Z8_XM6_STILL_OK) {
                            plane_grain_done[j] = 1u;
                            any_xm6_ok = 1;
                            xm6_total_us += dt_us;
                        } else if (grc == Z8_XM6_STILL_FALLBACK_DIRTY) {
                            p->kernel_result = Z8_FS_E_INTERNAL;
                            goto process_done;
                        } else if (plane_hdf_done[j]) {
                            (void)p->ops.clean(p->ops.context, d->y, d->y_bytes);
                        }
                    }
                }
            }
        }
    }

    int any_xm6_retained = 0;
    for (uint32_t j = 0u; j < p->count; ++j) {
        if (plane_hdf_done[j] || plane_grain_done[j] || (want_lut && plane_lut_done[j] && !plane_cpu_dirty[j])) {
            any_xm6_retained = 1;
            break;
        }
    }
    any_xm6_ok = any_xm6_retained;

    int filmsim_needed = want_hdf || run_grain;
    int all_filmsim_done = 1;
    for (uint32_t j = 0u; j < p->count; ++j) {
        if ((want_hdf && !plane_hdf_done[j]) || (run_grain && !plane_grain_done[j])) {
            all_filmsim_done = 0;
            break;
        }
    }

    int ws_cpu_dirty = 0;
    if (!filmsim_needed) {
        /* Pure Stage 0 LUT shot (or Grain requested without ZF_GRAIN.BIN) */
        if (want_lut) {
            p->kernel_result = Z8_FS_OK;
            p->last_stages_applied = Z8_FS_STAGE_LUT |
                                     (want_grain ? Z8_FS_STAGE_GRAIN_MISSING : 0u);
        } else {
            p->kernel_result = Z8_FS_E_ASSET;
            p->last_stages_applied = Z8_FS_STAGE_GRAIN_MISSING;
        }
    } else if (all_filmsim_done) {
        p->kernel_result = Z8_FS_OK;
        p->last_stages_applied = (want_lut ? Z8_FS_STAGE_LUT : 0u) |
                                 (want_hdf ? Z8_FS_STAGE_HDF : 0u) |
                                 (run_grain ? Z8_FS_STAGE_GRAIN :
                                  (want_grain ? Z8_FS_STAGE_GRAIN_MISSING : 0u));
    } else if (!nfsp_neon_allowed()) {
        /* NEON sentinel tripped: never enter ARM NEON HDF/Grain kernels! */
        nfsp_flight(FX_Z8_FLIGHT_JPEG_FALLBACK, 0u, p->shot_seq,
                    (uint32_t)(want_lut | (want_hdf << 1) | (run_grain << 2)), 0u);
        if (want_lut) {
            p->kernel_result = Z8_FS_OK;
            p->last_stages_applied = Z8_FS_STAGE_LUT;
        } else {
            p->kernel_result = Z8_FS_BYPASS;
        }
    } else {
        int any_filmsim_xm6 = 0;
        for (uint32_t j = 0u; j < p->count; ++j) {
            if (plane_hdf_done[j] || plane_grain_done[j]) {
                any_filmsim_xm6 = 1;
                break;
            }
        }
        if (!any_filmsim_xm6) {
            p->kernel_result = z8_filmsim_apply_group(&p->job, p->workspace, ws_bytes);
            if (want_lut && p->kernel_result == Z8_FS_OK) {
                p->last_stages_applied |= Z8_FS_STAGE_LUT;
            }
            ws_cpu_dirty = 1;
            for (uint32_t j = 0u; j < p->count; ++j) {
                plane_cpu_dirty[j] = 1u;
            }
        } else {
            /* Partial XM6 completion: run ARM NEON only for remaining planes/stages */
            uint32_t combined_stages = want_lut ? Z8_FS_STAGE_LUT : 0u;
            int rc = Z8_FS_OK;
            for (uint32_t j = 0u; j < p->count; ++j) {
                if (plane_hdf_done[j]) {
                    combined_stages |= Z8_FS_STAGE_HDF;
                }
                if (plane_grain_done[j]) {
                    combined_stages |= Z8_FS_STAGE_GRAIN;
                }
            }
            if (want_grain && !run_grain) {
                combined_stages |= Z8_FS_STAGE_GRAIN_MISSING;
            }

            if (want_hdf) {
                z8_filmsim_job hjob = p->job;
                hjob.count = 0u;
                hjob.analysis_index = 0u;
                hjob.cfg.film_style = Z8_STYLE_OFF;
                hjob.cfg.zf_intensity = 0u;
                z8_filmsim_cfg_finalize(&hjob.cfg, p->cfg.seq);
                uint32_t hstages = 0u;
                hjob.stages_applied_out = &hstages;
                uint64_t hmax_area = 0u;
                for (uint32_t j = 0u; j < p->count; ++j) {
                    if (!plane_hdf_done[j]) {
                        uint64_t area = (uint64_t)p->job.planes[j].width * p->job.planes[j].height;
                        if (hjob.count == 0u || area > hmax_area) {
                            hmax_area = area;
                            hjob.analysis_index = hjob.count;
                        }
                        hjob.planes[hjob.count++] = p->job.planes[j];
                        plane_cpu_dirty[j] = 1u;
                    }
                }
                if (hjob.count > 0u) {
                    ws_cpu_dirty = 1;
                    rc = z8_filmsim_apply_group(&hjob, p->workspace, ws_bytes);
                    combined_stages |= hstages;
                }
            }
            if (rc == Z8_FS_OK && run_grain) {
                for (uint32_t j = 0u; j < p->count; ++j) {
                    if (!plane_grain_done[j]) {
                        z8_filmsim_job gjob = p->job;
                        gjob.count = 1u;
                        gjob.analysis_index = 0u;
                        gjob.planes[0] = p->job.planes[j];
                        uint64_t psid = p->job.shot_id ^ ((uint64_t)j * UINT64_C(0x9E3779B97F4A7C15));
                        gjob.shot_id = psid ? psid : 1u;
                        gjob.cfg.hdf_level = Z8_HDF_OFF;
                        z8_filmsim_cfg_finalize(&gjob.cfg, p->cfg.seq);
                        uint32_t gstages = 0u;
                        gjob.stages_applied_out = &gstages;
                        ws_cpu_dirty = 1;
                        plane_cpu_dirty[j] = 1u;
                        rc = z8_filmsim_apply_group(&gjob, p->workspace, ws_bytes);
                        combined_stages |= gstages;
                        if (rc != Z8_FS_OK) {
                            break;
                        }
                    }
                }
            }
            p->kernel_result = rc;
            p->last_stages_applied = combined_stages;
        }
    }

process_done:
    if (assets_held && p->ops.release_assets) {
        p->ops.release_assets(p->ops.context);
        assets_held = 0;
    }

    if (p->kernel_result == Z8_FS_BYPASS) {
        abort_reason = Z8_NFSP_DISABLED;
        abort_count_bypass = 1;
        goto abort_shot;
    }
    if (p->kernel_result != Z8_FS_OK || cancelled_cb(p)) {
        abort_reason = (p->kernel_result == Z8_FS_E_ASSET) ? Z8_NFSP_ASSET_MISSING :
                       (p->kernel_result == Z8_FS_CANCELLED ||
                        p->kernel_result == Z8_FS_CANCELLED_DIRTY) ? Z8_NFSP_CANCELLED :
                       Z8_NFSP_KERNEL;
        abort_count_fail = 1;
        goto abort_shot;
    }

    if (any_xm6_ok && xops) {
        xops->record_still_ok(xm6, xm6_total_us);
    }

    /* Hardware D-cache publication: chunked < 0x88000 dc civac + dsb sy ONLY on planes,
     * workspace, and LUT cache dirtied by CPU (planes processed 100% on XM6 in DRAM skip
     * the 90 MiB clean!), then tag blocks with Z8_NFSP_USED_CLEANED_BIT. */
    nfsp_hb_enter(FX_Z8_STAGE_JPEG_CACHE_MAINT, p->shot_seq, p->last_stages_applied);
    for (uint32_t j = 0u; j < p->count; ++j) {
        if (!plane_cpu_dirty[j]) {
            continue;
        }
        struct z8_nfsp_plane *d = &p->planes[j];
        if (cancelled_cb(p) ||
            !p->ops.clean(p->ops.context, d->y, d->y_bytes)) {
            abort_reason = Z8_NFSP_POOL;
            abort_count_fail = 1;
            goto abort_shot;
        }
        nfsp_hb_progress();
        if (d->uv && !p->ops.clean(p->ops.context, d->uv, d->uv_bytes)) {
            abort_reason = Z8_NFSP_POOL;
            abort_count_fail = 1;
            goto abort_shot;
        }
        nfsp_hb_progress();
    }
    if (ws_cpu_dirty && p->workspace && ws_bytes > 0u &&
        (cancelled_cb(p) || !p->ops.clean(p->ops.context, (uint8_t *)p->workspace, ws_bytes))) {
        abort_reason = Z8_NFSP_POOL;
        abort_count_fail = 1;
        goto abort_shot;
    }
    if (lut_cache_cpu_dirty && lut_cache && lut_cache_bytes > 0u &&
        (cancelled_cb(p) || !p->ops.clean(p->ops.context, (uint8_t *)lut_cache, lut_cache_bytes))) {
        abort_reason = Z8_NFSP_POOL;
        abort_count_fail = 1;
        goto abort_shot;
    }
    for (uint32_t i = 0u; i < p->block_count; ++i) {
        p->blocks[i].used |= Z8_NFSP_USED_CLEANED_BIT;
    }
    if (cancelled_cb(p)) {
        abort_reason = Z8_NFSP_CANCELLED;
        abort_count_fail = 1;
        goto abort_shot;
    }

    p->last_main_w = p->planes[main_idx].roi_w;
    p->last_main_h = p->planes[main_idx].roi_h;
    __atomic_fetch_add(&p->photos_completed, 1u, __ATOMIC_RELAXED);

    if (p->ops.time_us) {
        p->finish_us = p->ops.time_us(p->ops.context);
    }
    nfsp_flight(FX_Z8_FLIGHT_JPEG_GRADE_DONE, 0u, p->shot_seq,
                p->last_stages_applied, (uint32_t)any_xm6_ok);
    if (hb_entered) {
        nfsp_hb_idle();
    }
    nfsp_frame_drop(&frame_held);

    __atomic_store_n(&p->phase, Z8_NFSP_READY, __ATOMIC_RELEASE);
    z8_nfsp_reclaim_cancelled(p);
    reap(p);
    return;

abort_shot:
    if (assets_held && p->ops.release_assets) {
        p->ops.release_assets(p->ops.context);
    }
    if (hb_entered) {
        nfsp_hb_idle();
    }
    nfsp_frame_drop(&frame_held);
    if (abort_count_bypass) {
        __atomic_fetch_add(&p->photos_bypassed, 1u, __ATOMIC_RELAXED);
    } else if (abort_count_fail) {
        __atomic_fetch_add(&p->photos_failed, 1u, __ATOMIC_RELAXED);
    }
    if (hung_dma_abort) {
        quarantine_hung_dma(p, abort_reason);
    } else {
        bypass(p, abort_reason);
    }
}

void z8_nfsp_cancel(struct z8_native_filmsim_photo *p)
{
    if (!p) {
        return;
    }
    __atomic_add_fetch(&p->cancel_epoch, 1u, __ATOMIC_ACQ_REL);
    set_reason(p, Z8_NFSP_CANCELLED);
    __atomic_store_n(&p->cancel, 1u, __ATOMIC_SEQ_CST);
}

/* V71.3 Defect 3B: Immediately reclaim uncommitted Class 0x24 photo.blocks when a
 * READY shot has been cancelled before EXPEED 7 engine_begin_ex commits it.
 * Active-engine fence guarantees reap() and z8_nfsp_class24_busy() wait until
 * every detached block handle has finished p->ops.release(). */
void z8_nfsp_reclaim_cancelled(struct z8_native_filmsim_photo *p)
{
    if (!p) {
        return;
    }
    struct z8_nfsp_allocation detached[Z8_NFSP_MAX_BLOCKS];
    uint32_t detached_count = 0u;
    int reclaimed = 0;

    lock_shot(p);
    if (phase_load(p) == Z8_NFSP_READY &&
        __atomic_load_n(&p->cancel, __ATOMIC_ACQUIRE) &&
        !p->committed && !p->active_engines &&
        p->poisoned < Z8_NFSP_QUARANTINE_THRESHOLD) {
        detached_count = p->block_count;
        if (detached_count > Z8_NFSP_MAX_BLOCKS) {
            detached_count = Z8_NFSP_MAX_BLOCKS;
        }
        for (uint32_t i = 0u; i < detached_count; ++i) {
            detached[i] = p->blocks[i];
            memset(&p->blocks[i], 0, sizeof(p->blocks[i]));
        }
        p->block_count = 0u;
        p->workspace = 0;
        __atomic_store_n(&p->phase, Z8_NFSP_BYPASS, __ATOMIC_RELEASE);
        if (detached_count) {
            ++p->active_engines;
        }
        reclaimed = 1;
    }
    unlock_shot(p);

    if (detached_count) {
        if (p->ops.release) {
            for (uint32_t i = detached_count; i > 0u; --i) {
                p->ops.release(p->ops.context, &detached[i - 1u]);
            }
        }
        lock_shot(p);
        if (p->active_engines) {
            --p->active_engines;
        }
        unlock_shot(p);
    }
    if (reclaimed) {
        reap(p);
    }
}

/* Returns 1 while photo.blocks (~110 MiB Class 0x24) may still be held, allocated,
 * or mid-release; returns 0 once guaranteed drained (including BYPASS after
 * free_blocks() and PENDING/COLLECT/PREPARE after z8_nfsp_cancel()). */
int z8_nfsp_class24_busy(struct z8_native_filmsim_photo *p)
{
    if (!p) {
        return 0;
    }
    lock_shot(p);
    uint32_t s = __atomic_load_n(&p->phase, __ATOMIC_SEQ_CST);
    int busy;
    if (s == Z8_NFSP_QUARANTINE) {
        busy = 0;
    } else if (s == Z8_NFSP_COPY || s == Z8_NFSP_PROCESS ||
               s == Z8_NFSP_READY || s == Z8_NFSP_RETIRING) {
        busy = 1;
    } else {
        busy = (p->block_count != 0u || p->active_engines != 0u) ? 1 : 0;
    }
    unlock_shot(p);
    return busy;
}

static void cancel_generation(struct z8_native_filmsim_photo *p, uint64_t g, uint32_t why)
{
    if (!p || !g) {
        return;
    }
    lock_shot(p);
    uint32_t s = phase_load(p);
    if (gen_load(p) == g && s != Z8_NFSP_IDLE && s != Z8_NFSP_RETIRING) {
        __atomic_add_fetch(&p->cancel_epoch, 1u, __ATOMIC_ACQ_REL);
        set_reason(p, why);
        __atomic_store_n(&p->cancel, 1u, __ATOMIC_RELEASE);
    }
    unlock_shot(p);
}

void z8_nfsp_cancel_generation(struct z8_native_filmsim_photo *p, uint64_t g)
{
    cancel_generation(p, g, Z8_NFSP_CANCELLED);
}

void z8_nfsp_timeout_generation(struct z8_native_filmsim_photo *p, uint64_t g)
{
    cancel_generation(p, g, Z8_NFSP_TIMEOUT);
}

int z8_nfsp_poll_generation(struct z8_native_filmsim_photo *p, uint64_t g)
{
    if (!p || !g) {
        return 1;
    }
    lock_shot(p);
    uint32_t s = phase_load(p);
    int done;
    if (gen_load(p) != g || s == Z8_NFSP_IDLE) {
        done = 1;
    } else if (__atomic_load_n(&p->cancel, __ATOMIC_ACQUIRE) && !p->committed) {
        source_close(p);
        done = source_drained(p);
        if (done) {
            __atomic_store_n(&p->source_reads_done, 1u, __ATOMIC_RELEASE);
        }
    } else {
        done = s != Z8_NFSP_CLAIM && s != Z8_NFSP_COLLECT && s != Z8_NFSP_PREPARE &&
               s != Z8_NFSP_PENDING && s != Z8_NFSP_COPY && s != Z8_NFSP_PROCESS;
    }
    unlock_shot(p);
    return done;
}

int z8_nfsp_job2_must_bypass(const Z8FilmSimConfig *cfg)
{
    if (!cfg || !z8_filmsim_cfg_validate(cfg)) {
        return 0;
    }
    /* Conservative 100%-safe invariant: bypass Job 2 (0x40eaf554, fixed 7.04 MiB
     * Basic NEF preview buffer) whenever Nikon Zf Film Grain is enabled (all 18
     * Nikon Zf presets) so EXPEED 7 engine 15e2 can never overflow Job 2 on
     * detailed real-world scenes (err 85 at 0x40e82210). Job 1 (Main Fine* JPEG,
     * 29.5 MB cap) always receives the full pipeline; pure Ricoh HDF without grain
     * still applies to Job 2. */
    return z8_filmsim_cfg_needs_grain(cfg) ? 1 : 0;
}

int z8_nfsp_engine_begin_ex(struct z8_native_filmsim_photo *p, uint64_t outer,
                            const void *original, void *local, void *desc,
                            uint32_t engine_slot)
{
    uint8_t raw[Z8_PHOTO_DESCRIPTOR_BYTES];
    uint64_t m = 0u;
    uint32_t word = 0u;

    if (!p || !original || !local || !desc) {
        return 0;
    }
    lock_shot(p);
    if (phase_load(p) != Z8_NFSP_READY || p->retire ||
        (__atomic_load_n(&p->cancel, __ATOMIC_ACQUIRE) && !p->committed) ||
        p->outer != outer) {
        unlock_shot(p);
        return 0;
    }
    if (engine_slot == Z8_NFSP_SLOT_NEF_PREVIEW && z8_nfsp_job2_must_bypass(&p->cfg)) {
        __atomic_fetch_add(&p->job2_grain_bypasses, 1u, __ATOMIC_RELAXED);
        unlock_shot(p);
        return 0;
    }
    ++p->active_engines;
    unlock_shot(p);

    uint64_t address = u64_le((const uint8_t *)original + 8u);
    if (!rd(p, outer + 0x790u, &m, 8u) ||
        !rd(p, outer + 0x10u, &word, 4u) ||
        m != p->metadata || word != p->outer_word ||
        !rd(p, address, raw, sizeof(raw))) {
        goto unchanged;
    }

    for (uint32_t role = 0u; role < Z8_PHOTO_SOURCE_COUNT; ++role) {
        uint32_t j = p->role_plane[role];
        if (j >= p->count ||
            address != p->snapshot.sources[role].descriptor_address ||
            memcmp(raw, p->snapshot.sources[role].raw, sizeof(raw)) != 0) {
            continue;
        }
        memcpy(desc, p->planes[j].descriptor, Z8_PHOTO_DESCRIPTOR_BYTES);
        memcpy(local, original, 0x50u);
        put64_le((uint8_t *)local + 8u, (uintptr_t)desc);
        lock_shot(p);
        if (p->retire || (__atomic_load_n(&p->cancel, __ATOMIC_ACQUIRE) && !p->committed)) {
            unlock_shot(p);
            goto unchanged;
        }
        p->committed = 1u;
        unlock_shot(p);
        return 1;
    }

unchanged:
    z8_nfsp_engine_end(p, 1u);
    return 0;
}

int z8_nfsp_engine_begin(struct z8_native_filmsim_photo *p, uint64_t outer,
                         const void *original, void *local, void *desc)
{
    return z8_nfsp_engine_begin_ex(p, outer, original, local, desc, Z8_NFSP_SLOT_DEFAULT);
}

void z8_nfsp_engine_end(struct z8_native_filmsim_photo *p, uint64_t result)
{
    if (!p) {
        return;
    }
    lock_shot(p);
    if (result != 1u) {
        /* V71.5 3-strike quarantine tolerance: transient single engine error
         * logs failure and recovers cleanly to IDLE; only >= 3 consecutive
         * failing shots enter Z8_NFSP_QUARANTINE. */
        if (__atomic_load_n(&p->reason, __ATOMIC_RELAXED) != Z8_NFSP_ENGINE_ERROR) {
            if (p->poisoned < Z8_NFSP_QUARANTINE_THRESHOLD) {
                ++p->poisoned;
            }
            __atomic_store_n(&p->reason, Z8_NFSP_ENGINE_ERROR, __ATOMIC_RELAXED);
            __atomic_fetch_add(&p->photos_failed, 1u, __ATOMIC_RELAXED);
        }
    }
    if (p->active_engines) {
        --p->active_engines;
    }
    unlock_shot(p);
    reap(p);
}

void z8_nfsp_retire(struct z8_native_filmsim_photo *p, uint64_t outer)
{
    if (!p) {
        return;
    }
    lock_shot(p);
    uint32_t s = phase_load(p);
    if (s != Z8_NFSP_IDLE && s != Z8_NFSP_RETIRING &&
        (p->outer == outer || s == Z8_NFSP_CLAIM)) {
        p->retire = 1u;
        source_close(p);
        z8_nfsp_cancel(p);
        if (s == Z8_NFSP_COLLECT) {
            __atomic_store_n(&p->phase, Z8_NFSP_BYPASS, __ATOMIC_RELEASE);
        }
        if (source_drained(p)) {
            __atomic_store_n(&p->source_reads_done, 1u, __ATOMIC_RELEASE);
        }
    }
    unlock_shot(p);
    reap(p);
}

uint64_t z8_nfsp_cleanup_begin(struct z8_native_filmsim_photo *p, uint64_t outer)
{
    if (!p) {
        return 0u;
    }
    uint64_t g = 0u;
    lock_shot(p);
    uint32_t s = phase_load(p);
    if (s != Z8_NFSP_IDLE && s != Z8_NFSP_RETIRING &&
        (p->outer == outer || s == Z8_NFSP_CLAIM)) {
        g = gen_load(p);
        ++p->cleanup_active;
        p->retire = 1u;
        source_close(p);
        z8_nfsp_cancel(p);
        if (s == Z8_NFSP_COLLECT) {
            __atomic_store_n(&p->phase, Z8_NFSP_BYPASS, __ATOMIC_RELEASE);
        }
    }
    unlock_shot(p);
    return g;
}

int z8_nfsp_cleanup_poll(struct z8_native_filmsim_photo *p, uint64_t g)
{
    if (!p || !g) {
        return 1;
    }
    lock_shot(p);
    int done = gen_load(p) != g || source_drained(p);
    if (done && gen_load(p) == g) {
        __atomic_store_n(&p->source_reads_done, 1u, __ATOMIC_RELEASE);
    }
    unlock_shot(p);
    return done;
}

void z8_nfsp_cleanup_end(struct z8_native_filmsim_photo *p, uint64_t g)
{
    if (!p || !g) {
        return;
    }
    lock_shot(p);
    if (gen_load(p) == g && p->cleanup_active) {
        --p->cleanup_active;
    }
    unlock_shot(p);
    reap(p);
}
