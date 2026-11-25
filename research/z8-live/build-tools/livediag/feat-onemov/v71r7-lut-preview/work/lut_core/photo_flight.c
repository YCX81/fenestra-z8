/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Black-Box Lock-Free Flight Recorder + 2-Second Heartbeat Stall Watchdog (Hardening #4).
 *
 * Strictly scalar (-mgeneral-regs-only), zero FP, zero SIMD, zero .data/.bss, zero heap.
 * Uses stock/hardened Z8 C3.11 file I/O (0x402DD4F8/558/6C8/78C/9FC/A4C) + cardslot
 * active drive (0xA1171B000) + chunked D-cache clean (0x40D2B9C8).
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "photo_flight.h"

#define FLIGHT_FN(addr, type) ((type)(uintptr_t)(addr))

/* Verified Z8 C3.11 stock / cardslot_v71r4 hardened file I/O & cache maintenance */
#define Z8_ADDR_CARD_OPEN     UINT64_C(0x402DD4F8)
#define Z8_ADDR_CARD_CLOSE    UINT64_C(0x402DD558)
#define Z8_ADDR_CARD_WRITE    UINT64_C(0x402DD78C)
#define Z8_ADDR_CARD_RENAME   UINT64_C(0x402DD9FC)
#define Z8_ADDR_CARD_REMOVE   UINT64_C(0x402DDA4C)
#define Z8_ADDR_DCACHE_CIVAC  UINT64_C(0x40D2B9C8)

#define FLIGHT_SEQ_BUSY32     UINT32_C(0x80000000)
#define FLIGHT_SEQ_MASK32     UINT32_C(0x7FFFFFFF)
#define FLIGHT_ERR_IO         UINT32_C(0xE001)
#define FLIGHT_ERR_OPEN       UINT32_C(0xE002)
#define FLIGHT_ERR_WRITE      UINT32_C(0xE003)
#define FLIGHT_ERR_RENAME     UINT32_C(0xE004)
#define FLIGHT_ERR_FORMAT     UINT32_C(0xE005)
#define FLIGHT_ERR_GATE       UINT32_C(0xE006)

_Static_assert(sizeof(FxZ8FlightRecord) == 32u, "FxZ8FlightRecord must be 32 B");
_Static_assert(sizeof(FxZ8Heartbeat) == 32u, "FxZ8Heartbeat must be 32 B");
_Static_assert(sizeof(FxZ8WatchdogStatus) == 32u, "FxZ8WatchdogStatus must be 32 B");

typedef struct {
    _Alignas(64) char crash_buf[V71R7_FLIGHT_CRASH_BYTES]; /* 4,096 B DMA-safe bounce buffer */
    FxZ8FlightRecord  frozen_rows[FX_Z8_FLIGHT_RING_COUNT]; /* 2,048 B */
    FxZ8Heartbeat     stalled[FX_Z8_HB_COUNT];              /* 256 B */
    uint32_t          stalled_ms[FX_Z8_HB_COUNT];           /* 32 B */
    uint64_t          frozen_first;
    uint64_t          frozen_present;
    uint32_t          magic;
    uint32_t          stall_slot;
    uint32_t          stall_stage;
    uint32_t          stall_ms;
    uint32_t          write_attempts;
    uint32_t          write_successes;
    uint32_t          activity;
    uint32_t          reserved0;
#ifdef V71R7_HOST_TEST
    V71r7FlightHostOps host_ops;
    uint32_t           has_host_ops;
    uint32_t           reserved1;
#endif
} V71r7FlightPriv;

_Static_assert((V71R7_DRAM_FLIGHT_PRIV_VA - V71R7_DRAM_ANCHOR_VA) + sizeof(V71r7FlightPriv)
                   <= (V71R7_DRAM_ANCHOR_END_VA - V71R7_DRAM_ANCHOR_VA),
               "V71r7FlightPriv must fit inside 0xA11640000..0xA11644000");

static inline V71r7FlightPriv *flight_priv(void)
{
    return V71R7_DRAM_PTR(V71r7FlightPriv, V71R7_DRAM_FLIGHT_PRIV_VA);
}

uint64_t v71r7_flight_ticks(void)
{
#ifdef V71R7_HOST_TEST
    const V71r7FlightPriv *priv = flight_priv();
    if (priv->has_host_ops && priv->host_ops.read_ticks) {
        return priv->host_ops.read_ticks(priv->host_ops.ctx);
    }
    return 0u;
#else
    uint64_t t = 0u;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
    return t;
#endif
}

uint16_t v71r7_flight_core(void)
{
#ifdef V71R7_HOST_TEST
    const V71r7FlightPriv *priv = flight_priv();
    if (priv->has_host_ops && priv->host_ops.read_core) {
        return priv->host_ops.read_core(priv->host_ops.ctx);
    }
    return 1u;
#else
    uint64_t id = 0u;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(id));
    return (uint16_t)((id & 15u) + 1u);
#endif
}

static char flight_active_drive(void)
{
#ifdef V71R7_HOST_TEST
    const V71r7FlightPriv *priv = flight_priv();
    if (priv->has_host_ops && priv->host_ops.active_drive) {
        char d = priv->host_ops.active_drive(priv->host_ops.ctx);
        if (d == 'A' || d == 'B') {
            return d;
        }
    }
#endif
    return v71r7_active_card_drive();
}

V71R7_BANK2_COLD void v71r7_flight_init(void)
{
    V71r7DramAnchor *anchor = v71r7_anchor();
    FxZ8FlightRecord *ring = v71r7_flight_ring_ptr();
    uint64_t *head = v71r7_flight_head_ptr();
    FxZ8Heartbeat *hbs = v71r7_flight_heartbeats_ptr();
    FxZ8WatchdogStatus *wd = v71r7_flight_watchdog_ptr();
    V71r7FlightPriv *priv = flight_priv();

    memset(ring, 0, sizeof(FxZ8FlightRecord) * FX_Z8_FLIGHT_RING_COUNT);
    __atomic_store_n(head, 0u, __ATOMIC_RELEASE);
    memset(hbs, 0, sizeof(FxZ8Heartbeat) * FX_Z8_HB_COUNT);
    memset(wd, 0, sizeof(*wd));
    wd->magic = V71R7_ANCHOR_MAGIC;

    /* Preserve host_ops across v71r7_flight_init() in host tests if already installed */
#ifdef V71R7_HOST_TEST
    V71r7FlightHostOps saved_ops = priv->host_ops;
    uint32_t saved_has = priv->has_host_ops;
#endif
    memset(priv, 0, sizeof(*priv));
    priv->magic = V71R7_ANCHOR_MAGIC;
#ifdef V71R7_HOST_TEST
    priv->host_ops = saved_ops;
    priv->has_host_ops = saved_has;
#endif

    if (__atomic_load_n(&anchor->magic, __ATOMIC_ACQUIRE) != V71R7_ANCHOR_MAGIC) {
        __atomic_store_n(&anchor->magic, V71R7_ANCHOR_MAGIC, __ATOMIC_RELEASE);
    }
}

static inline uint32_t seq64_to_seq32(uint64_t seq)
{
    uint32_t s = (uint32_t)(seq & FLIGHT_SEQ_MASK32);
    return s ? s : 1u;
}

void v71r7_flight_record(uint16_t event, uint16_t core_flags, uint32_t a, uint32_t b, uint32_t c)
{
    uint64_t tick = v71r7_flight_ticks();
    uint64_t *head = v71r7_flight_head_ptr();
    FxZ8FlightRecord *ring = v71r7_flight_ring_ptr();

    uint64_t seq = __atomic_fetch_add(head, 1u, __ATOMIC_RELAXED) + 1u;
    FxZ8FlightRecord *slot = &ring[(seq - 1u) % FX_Z8_FLIGHT_RING_COUNT];
    uint32_t seq32 = seq64_to_seq32(seq);
    uint32_t old = __atomic_load_n(&slot->seq, __ATOMIC_RELAXED);

    /* Strictly lock-free: never spin on a preempted writer, and never overwrite a newer seq */
    if ((old & FLIGHT_SEQ_BUSY32) != 0u) {
        return;
    }
    if (!__atomic_compare_exchange_n(&slot->seq, &old, seq32 | FLIGHT_SEQ_BUSY32,
                                     1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        return;
    }

    __atomic_store_n(&slot->tick, tick, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->event, event, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->core_flags, core_flags, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->a, a, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->b, b, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->c, c, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->reserved, (uint32_t)(seq >> 31), __ATOMIC_RELAXED);
    __atomic_store_n(&slot->seq, seq32, __ATOMIC_RELEASE);

    V71r7FlightPriv *priv = flight_priv();
    if (!__atomic_load_n(&priv->activity, __ATOMIC_RELAXED)) {
        __atomic_store_n(&priv->activity, 1u, __ATOMIC_RELEASE);
    }
}

void v71r7_heartbeat_enter(unsigned slot, unsigned stage, uint32_t cap_num, uint32_t aux)
{
    if (slot >= FX_Z8_HB_COUNT) {
        return;
    }
    FxZ8Heartbeat *hb = &v71r7_flight_heartbeats_ptr()[slot];
    uint64_t tick = v71r7_flight_ticks() & ~FX_Z8_HEARTBEAT_BUSY;
    if (!tick) {
        tick = 1u;
    }
    uint32_t core = (uint32_t)v71r7_flight_core();

    __atomic_store_n(&hb->enter_tick, tick | FX_Z8_HEARTBEAT_BUSY, __ATOMIC_RELEASE);
    __atomic_store_n(&hb->stage, (uint32_t)stage, __ATOMIC_RELAXED);
    __atomic_store_n(&hb->cap_num, cap_num, __ATOMIC_RELAXED);
    __atomic_store_n(&hb->core, core, __ATOMIC_RELAXED);
    __atomic_store_n(&hb->aux, aux, __ATOMIC_RELAXED);
    __atomic_store_n(&hb->progress_tick, tick, __ATOMIC_RELAXED);
    __atomic_store_n(&hb->enter_tick, tick, __ATOMIC_RELEASE);
}

void v71r7_heartbeat_progress(unsigned slot)
{
    if (slot >= FX_Z8_HB_COUNT) {
        return;
    }
    FxZ8Heartbeat *hb = &v71r7_flight_heartbeats_ptr()[slot];
    uint64_t tick = v71r7_flight_ticks() & ~FX_Z8_HEARTBEAT_BUSY;
    if (!tick) {
        tick = 1u;
    }
    __atomic_store_n(&hb->progress_tick, tick, __ATOMIC_RELEASE);
}

void v71r7_heartbeat_idle(unsigned slot)
{
    if (slot >= FX_Z8_HB_COUNT) {
        return;
    }
    FxZ8Heartbeat *hb = &v71r7_flight_heartbeats_ptr()[slot];
    uint64_t tick = v71r7_flight_ticks() & ~FX_Z8_HEARTBEAT_BUSY;
    if (!tick) {
        tick = 1u;
    }
    __atomic_store_n(&hb->enter_tick, tick | FX_Z8_HEARTBEAT_BUSY, __ATOMIC_RELEASE);
    __atomic_store_n(&hb->stage, (uint32_t)FX_Z8_STAGE_IDLE, __ATOMIC_RELAXED);
    __atomic_store_n(&hb->aux, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&hb->progress_tick, tick, __ATOMIC_RELAXED);
    __atomic_store_n(&hb->enter_tick, tick, __ATOMIC_RELEASE);
}

static int sample_record(uint64_t seq, FxZ8FlightRecord *out)
{
    const FxZ8FlightRecord *slot = &v71r7_flight_ring_ptr()[(seq - 1u) % FX_Z8_FLIGHT_RING_COUNT];
    uint32_t expected_seq32 = seq64_to_seq32(seq);
    uint32_t s1 = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
    if (s1 != expected_seq32) {
        return 0;
    }
    out->tick       = __atomic_load_n(&slot->tick, __ATOMIC_RELAXED);
    out->event      = __atomic_load_n(&slot->event, __ATOMIC_RELAXED);
    out->core_flags = __atomic_load_n(&slot->core_flags, __ATOMIC_RELAXED);
    out->seq        = s1;
    out->a          = __atomic_load_n(&slot->a, __ATOMIC_RELAXED);
    out->b          = __atomic_load_n(&slot->b, __ATOMIC_RELAXED);
    out->c          = __atomic_load_n(&slot->c, __ATOMIC_RELAXED);
    out->reserved   = __atomic_load_n(&slot->reserved, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&slot->seq, __ATOMIC_RELAXED) == s1 &&
           out->reserved == (uint32_t)(seq >> 31);
}

static int sample_heartbeat(unsigned slot, FxZ8Heartbeat *out)
{
    const FxZ8Heartbeat *hb = &v71r7_flight_heartbeats_ptr()[slot];
    uint64_t entered = __atomic_load_n(&hb->enter_tick, __ATOMIC_ACQUIRE);
    if ((entered & FX_Z8_HEARTBEAT_BUSY) != 0u) {
        return 0;
    }
    out->stage         = __atomic_load_n(&hb->stage, __ATOMIC_RELAXED);
    out->cap_num       = __atomic_load_n(&hb->cap_num, __ATOMIC_RELAXED);
    out->core          = __atomic_load_n(&hb->core, __ATOMIC_RELAXED);
    out->aux           = __atomic_load_n(&hb->aux, __ATOMIC_RELAXED);
    out->enter_tick    = entered;
    out->progress_tick = __atomic_load_n(&hb->progress_tick, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&hb->enter_tick, __ATOMIC_RELAXED) == entered;
}

V71R7_BANK2_COLD uint32_t v71r7_flight_snapshot(FxZ8FlightRecord *out_rows, uint32_t max_rows, uint64_t *first_seq_out)
{
    uint64_t last = __atomic_load_n(v71r7_flight_head_ptr(), __ATOMIC_ACQUIRE);
    if (!last || !out_rows || !max_rows) {
        if (first_seq_out) {
            *first_seq_out = 0u;
        }
        return 0u;
    }
    uint64_t first = last > FX_Z8_FLIGHT_RING_COUNT ? last - FX_Z8_FLIGHT_RING_COUNT + 1u : 1u;
    if (first_seq_out) {
        *first_seq_out = first;
    }
    uint32_t count = 0u;
    for (uint64_t seq = first; seq <= last && count < max_rows; ++seq) {
        if (sample_record(seq, &out_rows[count])) {
            ++count;
        }
    }
    return count;
}

V71R7_BANK2_COLD static void freeze_ring(V71r7FlightPriv *priv)
{
    uint64_t last = __atomic_load_n(v71r7_flight_head_ptr(), __ATOMIC_ACQUIRE);
    if (!last) {
        priv->frozen_first = 0u;
        priv->frozen_present = 0u;
        return;
    }
    priv->frozen_first = last > FX_Z8_FLIGHT_RING_COUNT ? last - FX_Z8_FLIGHT_RING_COUNT + 1u : 1u;
    priv->frozen_present = 0u;
    for (uint64_t seq = priv->frozen_first; seq <= last; ++seq) {
        unsigned idx = (unsigned)(seq - priv->frozen_first);
        if (idx < FX_Z8_FLIGHT_RING_COUNT && sample_record(seq, &priv->frozen_rows[idx])) {
            priv->frozen_present |= UINT64_C(1) << idx;
        }
    }
}

/* ---- Crash Report Formatting (zero printf/libc heap) ---- */

typedef struct {
    char *next;
    char *end;
} FlightText;

V71R7_BANK2_COLD static void txt_append(FlightText *t, const char *s)
{
    while (*s && t->next < t->end) {
        *t->next++ = *s++;
    }
}

V71R7_BANK2_COLD static void txt_num(FlightText *t, uint64_t v, unsigned base, unsigned width)
{
    static const char k_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char rev[24];
    unsigned n = 0u;
    do {
        rev[n++] = k_digits[v % base];
        v /= base;
    } while ((v || n < width) && n < sizeof(rev));
    while (n && t->next < t->end) {
        *t->next++ = rev[--n];
    }
}

V71R7_BANK2_COLD static void txt_kv(FlightText *t, const char *label, uint64_t val)
{
    txt_append(t, label);
    txt_num(t, val, 10u, 0u);
}

V71R7_BANK2_COLD static uint32_t elapsed_ms(uint64_t now, uint64_t then)
{
    uint64_t ms = (now >= then) ? (now - then) / FX_Z8_TICKS_PER_MS : 0u;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

V71R7_BANK2_COLD static const char *event_name(uint16_t ev)
{
    static const char *const k_names[] = {
        "UNKNOWN",
        "JMAIN", "JQUEUE", "JGRADE_IN", "JGRADE_OUT", "JENC_IN", "JENC_OUT", "JFALLBACK", "JYIELD",
        "ROUTE", "CAP_ABORT", "POWER_OFF", "PACK_FAULT", "PACK_STOP", "STALL", "SMP_QUAR",
        "POOL_SUSP", "POOL_RESM", "XM6_SUB", "XM6_TMO", "XM6_SPCR", "NEON_FAIL", "PWR_CAN",
        "L0_WYSI", "BOOT"
    };
    if (ev >= sizeof(k_names) / sizeof(k_names[0])) {
        return k_names[0];
    }
    return k_names[ev];
}

V71R7_BANK2_COLD uint32_t v71r7_flight_format_crash(char *dst, uint32_t dst_bytes,
                                                    const char *reason, uint64_t now_tick)
{
    if (!dst || dst_bytes < 512u) {
        return 0u;
    }
    V71r7FlightPriv *priv = flight_priv();
    const FxZ8WatchdogStatus *wd = v71r7_flight_watchdog_ptr();
    freeze_ring(priv);

    FlightText t = { dst, dst + dst_bytes };
    txt_append(&t, "FXCRASH build=Z8-C311-V71.7 reason=");
    txt_append(&t, reason ? reason : "STALL");
    txt_kv(&t, " uptime_ms=", now_tick / FX_Z8_TICKS_PER_MS);
    txt_kv(&t, " flags=", __atomic_load_n(&v71r7_anchor()->flags, __ATOMIC_ACQUIRE));

    txt_append(&t, "\nHB: slot,stage,core,cap,stuck_ms,aux");
    for (unsigned slot = 0u; slot < FX_Z8_HB_COUNT; ++slot) {
        FxZ8Heartbeat hb;
        int is_stalled = (wd->stalled_slots >> slot) & 1u;
        if (is_stalled && priv->stalled[slot].stage) {
            hb = priv->stalled[slot];
        } else if (!sample_heartbeat(slot, &hb)) {
            txt_kv(&t, " ", slot);
            txt_append(&t, ",busy");
            continue;
        }
        uint32_t age = (is_stalled && priv->stalled[slot].stage)
                           ? priv->stalled_ms[slot]
                           : (hb.stage ? elapsed_ms(now_tick, hb.progress_tick) : 0u);
        txt_kv(&t, " ", slot);
        txt_kv(&t, ",", hb.stage);
        txt_kv(&t, ",", hb.core);
        txt_kv(&t, ",", hb.cap_num);
        txt_kv(&t, ",", age);
        txt_kv(&t, ",", hb.aux);
    }

    txt_kv(&t, "\nWD stalled_mask=", wd->stalled_slots);
    txt_kv(&t, " slot=", priv->stall_slot);
    txt_kv(&t, " stage=", priv->stall_stage);
    txt_kv(&t, " stall_ms=", priv->stall_ms);
    txt_kv(&t, " dumps=", wd->dumps_written);
    txt_kv(&t, " attempts=", priv->write_attempts);
    txt_kv(&t, " last_err=", wd->last_err);

    txt_kv(&t, "\nseq_base=", priv->frozen_first);
    txt_append(&t, "\nseq+ms32+core(hex) event a b c(base36)\n");
    for (unsigned i = 0u; i < FX_Z8_FLIGHT_RING_COUNT; ++i) {
        if (!(priv->frozen_present & (UINT64_C(1) << i))) {
            continue;
        }
        const FxZ8FlightRecord *r = &priv->frozen_rows[i];
        txt_num(&t, i, 16u, 2u);
        txt_num(&t, (uint32_t)(r->tick / FX_Z8_TICKS_PER_MS), 16u, 8u);
        txt_num(&t, r->core_flags & 15u, 16u, 1u);
        txt_append(&t, event_name(r->event));
        txt_append(&t, " ");
        txt_num(&t, r->a, 36u, 0u);
        txt_append(&t, " ");
        txt_num(&t, r->b, 36u, 0u);
        txt_append(&t, " ");
        txt_num(&t, r->c, 36u, 0u);
        txt_append(&t, "\n");
    }

    if (t.next >= t.end) {
        return 0u;
    }
    return (uint32_t)(t.next - dst);
}

/* ---- Card I/O Dispatch (Stock/Hardened 0x402DD4F8..A4C or Host Test Hooks) ---- */

static int32_t io_card_open(const char *path, uint32_t flags, uint32_t mode)
{
#ifdef V71R7_HOST_TEST
    const V71r7FlightPriv *priv = flight_priv();
    if (priv->has_host_ops && priv->host_ops.card_open) {
        return priv->host_ops.card_open(priv->host_ops.ctx, path, flags, mode);
    }
    (void)path;
    (void)flags;
    (void)mode;
    return -1;
#else
    return FLIGHT_FN(Z8_ADDR_CARD_OPEN, int32_t (*)(const char *, uint32_t, uint32_t))(
        path, flags, mode);
#endif
}

static uint32_t io_card_write(int32_t fd, const void *buf, uint32_t bytes, uint32_t *done)
{
#ifdef V71R7_HOST_TEST
    const V71r7FlightPriv *priv = flight_priv();
    if (priv->has_host_ops && priv->host_ops.card_write) {
        return priv->host_ops.card_write(priv->host_ops.ctx, fd, buf, bytes, done);
    }
    (void)fd;
    (void)buf;
    (void)bytes;
    if (done) {
        *done = 0u;
    }
    return FLIGHT_ERR_WRITE;
#else
    /* Clean D-cache on DMA bounce buffer before stock/hardened write */
    uint32_t aligned = (bytes + 63u) & ~63u;
    if (aligned) {
        (void)FLIGHT_FN(Z8_ADDR_DCACHE_CIVAC, int (*)(void *, uint32_t))(
            (void *)(uintptr_t)buf, aligned);
    }
    return FLIGHT_FN(Z8_ADDR_CARD_WRITE,
                     uint32_t (*)(int32_t, const void *, uint32_t, uint32_t *, uint32_t))(
        fd, buf, bytes, done, 0u);
#endif
}

static uint32_t io_card_close(int32_t fd)
{
#ifdef V71R7_HOST_TEST
    const V71r7FlightPriv *priv = flight_priv();
    if (priv->has_host_ops && priv->host_ops.card_close) {
        return priv->host_ops.card_close(priv->host_ops.ctx, fd);
    }
    (void)fd;
    return 0u;
#else
    return FLIGHT_FN(Z8_ADDR_CARD_CLOSE, uint32_t (*)(int32_t))(fd);
#endif
}

static uint32_t io_card_remove(const char *path)
{
#ifdef V71R7_HOST_TEST
    const V71r7FlightPriv *priv = flight_priv();
    if (priv->has_host_ops && priv->host_ops.card_remove) {
        return priv->host_ops.card_remove(priv->host_ops.ctx, path);
    }
    (void)path;
    return 0u;
#else
    return FLIGHT_FN(Z8_ADDR_CARD_REMOVE, uint32_t (*)(const char *))(path);
#endif
}

static uint32_t io_card_rename(const char *from, const char *to)
{
#ifdef V71R7_HOST_TEST
    const V71r7FlightPriv *priv = flight_priv();
    if (priv->has_host_ops && priv->host_ops.card_rename) {
        return priv->host_ops.card_rename(priv->host_ops.ctx, from, to);
    }
    (void)from;
    (void)to;
    return FLIGHT_ERR_RENAME;
#else
    return FLIGHT_FN(Z8_ADDR_CARD_RENAME, uint32_t (*)(const char *, const char *))(from, to);
#endif
}

static int io_card_exists(const char *path)
{
    int32_t fd = io_card_open(path, 0u, 0u);
    if (fd < 0) {
        return 0;
    }
    (void)io_card_close(fd);
    return 1;
}

static void build_crash_path(char drive, const char *ext, char out[32])
{
    static const char k_mid[] = ":\\FENESTRA\\FXCRASH.";
    unsigned n = 0u;
    out[n++] = (drive == 'B') ? 'B' : 'A';
    for (unsigned i = 0u; k_mid[i]; ++i) {
        out[n++] = k_mid[i];
    }
    for (unsigned i = 0u; ext[i] && n < 31u; ++i) {
        out[n++] = ext[i];
    }
    out[n] = '\0';
}

V71R7_BANK2_COLD uint32_t v71r7_flight_write_crash_log(const char *reason, uint64_t now_tick)
{
    V71r7FlightPriv *priv = flight_priv();
    FxZ8WatchdogStatus *wd = v71r7_flight_watchdog_ptr();
    FxZ8LutIoGate *gate = v71r7_io_gate();

    uint64_t gstate = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    uint32_t epoch = (gstate & 1u) ? (uint32_t)(gstate >> 32) : 0u;
    int took_gate = 0;
    if (epoch != 0u) {
        if (!fx_z8_lut_io_take(gate, epoch)) {
            wd->last_err = FLIGHT_ERR_GATE;
            return FLIGHT_ERR_GATE;
        }
        took_gate = 1;
    } else if ((gstate & (FX_Z8_LUT_IO_STOPPED | FX_Z8_LUT_IO_EARLY_OFF)) != 0u) {
        wd->last_err = FLIGHT_ERR_GATE;
        return FLIGHT_ERR_GATE;
    }

    ++priv->write_attempts;
    uint32_t bytes = v71r7_flight_format_crash(priv->crash_buf, V71R7_FLIGHT_CRASH_BYTES,
                                               reason, now_tick);
    if (!bytes) {
        if (took_gate) {
            fx_z8_lut_io_drop(gate);
        }
        wd->last_err = FLIGHT_ERR_FORMAT;
        return FLIGHT_ERR_FORMAT;
    }

    char drive = flight_active_drive();
    char path_tmp[32], path_old[32], path_log[32];
    build_crash_path(drive, "TMP", path_tmp);
    build_crash_path(drive, "OLD", path_old);
    build_crash_path(drive, "LOG", path_log);

    /* Atomic write + .TMP -> .OLD -> .LOG rotation */
    (void)io_card_remove(path_tmp);
    int32_t fd = io_card_open(path_tmp, 0x101u, 0u);
    if (fd < 0) {
        if (took_gate) {
            fx_z8_lut_io_drop(gate);
        }
        wd->last_err = FLIGHT_ERR_OPEN;
        return FLIGHT_ERR_OPEN;
    }

    uint32_t done = 0u;
    uint32_t werr = io_card_write(fd, priv->crash_buf, bytes, &done);
    (void)io_card_close(fd);
    if (werr != 0u || done != bytes) {
        if (took_gate) {
            fx_z8_lut_io_drop(gate);
        }
        wd->last_err = werr ? werr : FLIGHT_ERR_WRITE;
        return wd->last_err;
    }

    int had_prev = io_card_exists(path_log);
    if (had_prev) {
        (void)io_card_remove(path_old);
        uint32_t r1 = io_card_rename(path_log, path_old);
        if (r1 != 0u) {
            if (took_gate) {
                fx_z8_lut_io_drop(gate);
            }
            wd->last_err = r1;
            return r1;
        }
    }

    uint32_t r2 = io_card_rename(path_tmp, path_log);
    if (r2 != 0u) {
        if (had_prev && !io_card_exists(path_log)) {
            (void)io_card_rename(path_old, path_log);
        }
        if (took_gate) {
            fx_z8_lut_io_drop(gate);
        }
        wd->last_err = r2;
        return r2;
    }

    if (took_gate) {
        fx_z8_lut_io_drop(gate);
    }
    ++priv->write_successes;
    wd->magic = V71R7_ANCHOR_MAGIC;
    wd->dumps_written += 1u;
    wd->last_err = 0u;
    wd->last_dump_tick = now_tick;
    return 0u;
}

V71R7_BANK2_COLD uint32_t v71r7_watchdog_poll(uint64_t now_tick)
{
    if (!now_tick) {
        now_tick = v71r7_flight_ticks();
    }
    V71r7FlightPriv *priv = flight_priv();
    FxZ8WatchdogStatus *wd = v71r7_flight_watchdog_ptr();
    uint32_t newly_stalled = 0u;

    for (unsigned slot = 0u; slot < FX_Z8_HB_COUNT; ++slot) {
        FxZ8Heartbeat hb;
        if (!sample_heartbeat(slot, &hb)) {
            continue;
        }
        if (hb.stage == FX_Z8_STAGE_IDLE) {
            continue;
        }
        /* Avoid duplicate dump for the exact same enter_tick already latched */
        if (hb.enter_tick == priv->stalled[slot].enter_tick && priv->stalled[slot].stage != 0u) {
            continue;
        }
        if (now_tick < hb.progress_tick || (now_tick - hb.progress_tick) < V71R7_FLIGHT_STALL_TICKS) {
            continue;
        }
        priv->stalled[slot] = hb;
        priv->stalled_ms[slot] = elapsed_ms(now_tick, hb.progress_tick);
        priv->stall_slot = slot;
        priv->stall_stage = hb.stage;
        priv->stall_ms = priv->stalled_ms[slot];
        newly_stalled |= (1u << slot);
        v71r7_flight_record(FX_Z8_FLIGHT_STALL, (uint16_t)hb.core, slot, hb.stage, hb.cap_num);
    }

    if (newly_stalled != 0u) {
        wd->magic = V71R7_ANCHOR_MAGIC;
        wd->stalled_slots |= newly_stalled;
        (void)v71r7_flight_write_crash_log("STALL", now_tick);
    }
    return newly_stalled;
}

#ifdef V71R7_HOST_TEST
void v71r7_flight_set_host_ops(const V71r7FlightHostOps *ops)
{
    V71r7FlightPriv *priv = flight_priv();
    if (ops) {
        priv->host_ops = *ops;
        priv->has_host_ops = 1u;
    } else {
        memset(&priv->host_ops, 0, sizeof(priv->host_ops));
        priv->has_host_ops = 0u;
    }
}
#endif
