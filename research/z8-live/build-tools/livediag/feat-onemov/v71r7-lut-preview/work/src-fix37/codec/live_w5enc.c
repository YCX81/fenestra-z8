/* Live photo-mode encoder on the Z8 Wave5 (C3.11 stock HEVC/AVC service).
 * Same contract as codec/live_encoder.c (minih264): I420 + PTS in, one Annex-B AU out,
 * extended in V70 with:
 *   - Opt A: Zero-copy direct NV12 slot write (live_encoder_nv12_begin / submit_slot /
 *            slot_ptrs) + 0-copy / 0-dc-cvac CFR repeat re-triggering of last_valid_slot.
 *   - Opt B: 1-deep async pipelined Wave5 (submit_slot returns immediately after TRIGGER;
 *            poll_au / flush harvests INFO(stream, hw_index % 9) asynchronously; preempt /
 *            w5_close_body drains any 1 in-flight picture before W5_STOP / W5_FINISH / EXIT).
 *   - Opt C: Single-SUBMIT per open_stream in manual-trigger mode (0x40c38bb4 proven no-op
 *            on MMIO), direct cached BS parsing (zero stage_wrap copy on non-split cached AUs),
 *            and POOL_GATE-immune cached EV_DIAG3 per-stage timings in live_w5_mirror.
 *
 * Verified on real Z8 silicon (C3.11 0x40c38bb4 / 0x40c39058 / 0x40c45cb4) AND QEMU Wave5
 * emulator (nikon-z8-wave5-port.h). All Opt A/B cached RMW state lives in bytes 0..255 of
 * live_runtime.lv_regs[228] (struct live_w5_mirror_v70 <= 256 B; zero ABI change). */
#include "live_codec.h"
#include "live_w5_mirror.h"
#include "../runtime.h"
#include "../ui/live_ui.h"
#include "live_h264_idr.h"
#include "live_hevc.h"
#include "../capture/live_alias.h"
#include <string.h>
#define FN(a, t) ((t)(uintptr_t)UINT64_C(a))
#define RD32(a) (*(volatile uint32_t *)(uintptr_t)UINT64_C(a))
void la_native_yield(void);

#if defined(__aarch64__) && !defined(__APPLE__) && !defined(__linux__) && !defined(LIVE_W5_HOST_MOCK) && !defined(LIVE_HOST_TEST)
#define W5_V70_SEC __attribute__((section(".filmsim"), noinline, minsize))
#else
#define W5_V70_SEC __attribute__((noinline))
#endif

#define W5_MAGIC 0x57354c31u            /* "W5L1" */
#ifndef LIVE_W5_HEVC
#define LIVE_W5_HEVC 1                  /* hevc build: codec word 1 (Main, 8-bit) */
#endif
#define W5_CODEC (LIVE_W5_HEVC ? 1u : 0u)
#define W5_CLIENT 4u                    /* no stock caller uses it */
#define W5_KBPS 16000u
#define W5_ARENA 0x1000000u
#define W5_SLOTS 4u
#define W5_AU_CAP (512u * 1024u)
#define W5_WAIT_US 300000u
#define W5_PREEMPT_WAIT_US 600000u   /* longest in-flight frame: 300 ms completion wait + copy */
#define W5_HEADER_REPLY_OFF 0x720u
#ifndef W5_CACHE_OFFSET
#define W5_CACHE_OFFSET UINT64_C(0x400000000)
#endif
enum {
    W5_STAGED_SCRATCH    = 1u << 0,
    W5_ALIAS_SLOTS_TRIED = 1u << 1,
    W5_ALIAS_SLOTS_OK    = 1u << 2,
    W5_ALIAS_BS_TRIED    = 1u << 3,
    W5_ALIAS_BS_OK       = 1u << 4,
    W5_ALIAS_MASK        = W5_ALIAS_SLOTS_TRIED | W5_ALIAS_SLOTS_OK |
                           W5_ALIAS_BS_TRIED | W5_ALIAS_BS_OK
};
/* HEADERS keeps these pointers after its bounded wait returns. Keep the reply
 * in the retained pool, not the caller's stack: a late native completion writes
 * hdr, byte count and four NAL lengths. RUN ends at 0x710; SUBMIT starts 0x800. */
struct w5_header_reply { uint64_t data; uint32_t bytes, lens[4]; };
_Static_assert(offsetof(struct w5_header_reply, lens)==12, "native header lengths");
_Static_assert(sizeof(struct w5_header_reply)==32, "native header reply span");
_Static_assert(W5_HEADER_REPLY_OFF%_Alignof(struct w5_header_reply)==0 &&
               W5_HEADER_REPLY_OFF>=0x710u &&
               W5_HEADER_REPLY_OFF+sizeof(struct w5_header_reply)<=0x800u,
               "header reply does not overlap RUN or SUBMIT");
#ifndef LIVE_ENCODER_BUSY
#define LIVE_ENCODER_BUSY (-30)
#endif
#define W5_ERR_IN_FLIGHT (-28)

enum { R_FRAME, R_SUB1, R_SUB2, R_BS0, R_BS1, R_AUX, R_REF0, R_REF1, R_EXT0, R_EXT1, R_USER, R_COUNT };
static const uint32_t region_size[R_COUNT] = {0x1400000, 0x80000, 0x500000, 0x200000, 0x200000, 0x400000,
                                               0x200000, 0x200000, 0x200000, 0x200000, 0x80000};

struct live_w5 {
    uint32_t magic, stream, width, height, slot_bytes, hw_index, batch_open, frames;
    uint32_t busy, release_req, preempts, busy_drops;   /* encode/preempt handshake + counters */
    uint64_t arena, region[R_COUNT], slot[W5_SLOTS];
    void *arena_desc, *block_desc;
    live_h264_idr idr;
    uint8_t scratch[0x1000] __attribute__((aligned(16)));   /* service structs */
    uint8_t au[W5_AU_CAP] __attribute__((aligned(16)));
    uint8_t wrap[W5_AU_CAP] __attribute__((aligned(16)));   /* AU that wrapped the 4 MiB ring */
    uint64_t t_in, t_wait, t_out, t_parse, s_in, s_wait, s_out, s_parse, n_timed;   /* diag, us */
    uint64_t preempt_us; uint32_t reopen_blocked, closes;   /* v22 cooldown; v27 clean closes */
    uint32_t eos_ok, eos_miss, fin_rc, last_close_us;   /* v27 */
    uint32_t reserved_close_lock, close_failed;   /* v30 */
    uint32_t flags, alias_slot_ok, alias_slot_fail, alias_bs_ok, alias_bs_fail, staging_scratch;
    struct live_runtime *rmw; /* v36: cached owner, occupies former tail padding */
    uint32_t hevc_magic, hevc_open_rc, hevc_hdr_types, hevc_force;   /* hevc: "HEV1", HEADERS NAL types (4 x 8 bit) */
    live_hevc hevc;
    uint64_t native_instance; /* RUN binding; uses prior tail padding */
};
_Static_assert(offsetof(struct live_w5, native_instance)==1057016, "retained instance binding");
_Static_assert(sizeof(struct live_w5)==1057024, "retained binding preserves pool layout");

_Static_assert(offsetof(struct live_w5, hevc_magic) == 1053600, "hevc fields follow the v32 pool header");
_Static_assert(offsetof(struct live_w5, rmw) == 1053592, "v32 tail padding");
_Static_assert(sizeof(struct live_runtime) == 23904, "v68/v70 duplex shutter runtime ABI");
_Static_assert(offsetof(struct live_runtime, w5_close_lock) == 19284, "v53 shutter log_pad slot");
_Static_assert(offsetof(struct live_runtime, w5_mirror) == offsetof(struct live_runtime, lv_regs), "w5_mirror aliases lv_regs[0..255]");
_Static_assert(sizeof(((struct live_runtime *)0)->lv_regs) >= sizeof(struct live_w5_mirror_v70), "lv_regs accommodates w5_mirror_v70");
_Static_assert(offsetof(struct live_w5_mirror, magic) == offsetof(struct live_w5_mirror_v70, magic) &&
               offsetof(struct live_w5_mirror, stream) == offsetof(struct live_w5_mirror_v70, stream) &&
               offsetof(struct live_w5_mirror, busy) == offsetof(struct live_w5_mirror_v70, busy) &&
               offsetof(struct live_w5_mirror, release_req) == offsetof(struct live_w5_mirror_v70, release_req) &&
               offsetof(struct live_w5_mirror, close_failed) == offsetof(struct live_w5_mirror_v70, close_failed) &&
               offsetof(struct live_w5_mirror, handoffs) == offsetof(struct live_w5_mirror_v70, handoffs) &&
               offsetof(struct live_w5_mirror, arena) == offsetof(struct live_w5_mirror_v70, arena) &&
               offsetof(struct live_w5_mirror, native_instance) == offsetof(struct live_w5_mirror_v70, native_instance) &&
               offsetof(struct live_w5_mirror, arena_desc) == offsetof(struct live_w5_mirror_v70, arena_desc) &&
               offsetof(struct live_w5_mirror, block_desc) == offsetof(struct live_w5_mirror_v70, block_desc),
               "live_w5_mirror_v70 prefix must match V69 live_w5_mirror byte-for-byte");

typedef struct live_w5_mirror_v70 __attribute__((may_alias)) live_w5_m70;

static inline live_w5_m70 *w5_m70(struct live_runtime *r) {
    return (live_w5_m70 *)(void *)&r->w5_mirror;
}
static inline const live_w5_m70 *w5_m70_const(const struct live_runtime *r) {
    return (const live_w5_m70 *)(const void *)&r->w5_mirror;
}

/* stock entry points (C3.11), see W5-BRINGUP.md */
#ifndef LIVE_W5_HOST_MOCK
#define POOL_ALLOC(n) FN(0x4104d2dc, void *(*)(uint32_t, uint32_t, uint32_t))(0x4c4155, 0x60, (n))
#define POOL_FREE(d) FN(0x4104d600, void (*)(void *))(d)
#define POOL_GATE RD32(0xa03f31a68)
#define MODE_REG RD32(0xa03bd062c)
#define BRINGUP FN(0x402f2d58, uint32_t (*)(uint32_t, uint32_t, uint64_t, uint32_t))
#define EXIT FN(0x402f3084, uint64_t (*)(uint32_t))
#define DELAY FN(0x40187b00, int32_t (*)(uint32_t))
#define SLOT_USED(s) (*(volatile uint32_t *)(uintptr_t)(UINT64_C(0xa02d30118) + (s) * 0xcu))   /* service slot {used, coreA, coreB} */
#define OPEN FN(0x40c33834, uint32_t (*)(uint32_t, uint64_t))
#define CONFIGURE FN(0x40c333d0, uint32_t (*)(void *, void *))
#define REGISTER FN(0x40c33480, uint32_t (*)(uint32_t, void *))
#define PARAMS FN(0x402f6c10, void (*)(void *, uint32_t, uint64_t, void *))
#define SET_PARAMS FN(0x40c338ac, uint32_t (*)(uint32_t, void *, uint32_t))
#define HEADERS FN(0x40c33d98, uint32_t (*)(uint32_t, uint64_t *, uint32_t *, uint32_t *))
#define RUN FN(0x40c34164, uint32_t (*)(uint32_t, void *))
#define SUBMIT FN(0x40c33e34, uint32_t (*)(uint32_t, uint32_t, void *))
#define TRIGGER FN(0x40c33fb0, uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))
#define NOOP_CB UINT64_C(0x40c028ac)    /* mov w0,#0; ret */
/* Native VDI struct is 0xa02d3d928: +0x58 is the MMIO register base,
 * +0x28 is the original arena base. 0xa02d3d9d8 is instead struct+0xb0,
 * the second DMA allocation record, and must never identify the arena. */
#define VDI_BASE (*(volatile uint64_t *)(uintptr_t)UINT64_C(0xa02d3d980))
#define VDI_ARENA_BASE (*(volatile uint64_t *)(uintptr_t)UINT64_C(0xa02d3d950))
#define CLIENT_STREAM(c) (*(volatile uint8_t *)(uintptr_t)(UINT64_C(0xa02e98480) + (c)))
#define INFO(stream, i) ((volatile uint8_t *)(uintptr_t)(UINT64_C(0xa02d38b98) + (stream) * 0x2400u + (i) * 0x480u))
#define W5_STOP FN(0x40c34078, uint32_t (*)(uint32_t))     /* EOS request, handled at the next picture */
#define W5_FINISH FN(0x40c340e8, uint32_t (*)(uint32_t))   /* frees the instance (worker 0x400/0x800) */
#define EOS_DONE(s) (*(volatile uint32_t *)(uintptr_t)(UINT64_C(0xa02d31070) + (s) * 4u))
#define WSTATE(s) (*(volatile uint32_t *)(uintptr_t)(UINT64_C(0xa02d3274c) + (s) * 4u))   /* v29: 4 run, 5 EOS, 0 free */
#define W5_INST_PTR(s) (*(volatile uint64_t *)(uintptr_t)(UINT64_C(0xa02d32758) + (s) * 8u))

static uint64_t now_us(void) {
    uint64_t n, f;
    __asm__ volatile("mrs %0, cntpct_el0; mrs %1, cntfrq_el0" : "=r"(n), "=r"(f));
    return f ? n / f * 1000000 + n % f * 1000000 / f : 0;
}
#endif

/* pool descriptor -> {cpu, bytes}; the block is physically contiguous */
static int pool(uint32_t bytes, void **desc, uint64_t *cpu, uint32_t *actual) {
    *desc = 0; *cpu = 0; *actual = 0;
    if (POOL_GATE) return -1;
    void *d = POOL_ALLOC(bytes);
    if (!d) return -1;
    const uint64_t *r = *(const uint64_t *const *)((const uint8_t *)d + 8);
    if (!r || (uint32_t)r[1] < bytes) { POOL_FREE(d); return -1; }
    *desc = d; *cpu = r[0]; *actual = (uint32_t)r[1];
    return 0;
}

/* 64-byte bursts with ldp/stp (16 B per access) for uncached buffers; bytes when the two
 * pointers do not share 16-byte alignment, byte prefix until they are aligned. */
static void w5_burst(uint8_t *d,const uint8_t *s,size_t n) {
 if((((uintptr_t)d^(uintptr_t)s)&15)==0){
  while(n && ((uintptr_t)s&15)){*d++=*s++;n--;}
  while(n>=64){
   __asm__ volatile("ldp x9,x10,[%1]\n ldp x11,x12,[%1,#16]\n ldp x13,x14,[%1,#32]\n ldp x15,x16,[%1,#48]\n"
                    "stp x9,x10,[%0]\n stp x11,x12,[%0,#16]\n stp x13,x14,[%0,#32]\n stp x15,x16,[%0,#48]\n"
                    ::"r"(d),"r"(s):"x9","x10","x11","x12","x13","x14","x15","x16","memory");
   d+=64;s+=64;n-=64;}
 }
 while(n--)*d++=*s++;
}
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* 0xf0-byte buffer registration checked by 0x40c33480 */
static void table(struct live_w5 *w, uint8_t *t) {
    uint32_t hi = (uint32_t)(w->arena >> 32);
    static const struct { uint16_t off, r; } single[] = {{0x10, R_FRAME}, {0x20, R_SUB1}, {0x30, R_SUB2},
                                                         {0xc8, R_EXT0}, {0xd8, R_EXT1}};
    memset(t, 0, 0xf0);
    put32(t, hi); put32(t + 4, W5_ARENA); put64(t + 8, w->arena);
    for (unsigned i = 0; i < 5; i++) {
        put32(t + single[i].off, region_size[single[i].r]);
        put64(t + single[i].off + 8, w->region[single[i].r]);
    }
    put32(t + 0x40, 2); put32(t + 0x44, region_size[R_BS0]);
    put64(t + 0x48, w->region[R_BS0]); put64(t + 0x50, w->region[R_BS1]);
    put32(t + 0x68, hi); put32(t + 0x6c, region_size[R_AUX]); put64(t + 0x70, w->region[R_AUX]);
    put32(t + 0x78, hi); put32(t + 0x7c, 2); put32(t + 0x80, region_size[R_REF0]);
    put64(t + 0x88, w->region[R_REF0]); put64(t + 0x90, w->region[R_REF1]);
    put32(t + 0xe8, hi);
}

#define VDI_UP 0x16010000u

static __attribute__((noinline, minsize)) struct live_runtime *w5_runtime_of(const live_encoder *e) {
    struct live_runtime *r = live_ui_engine_get();
    if (!r || r->magic != LIVE_RUNTIME_MAGIC || r->size != sizeof(*r) ||
        !e || e != &r->stream.encoder) return 0;
    return r;
}

static __attribute__((noinline, minsize)) int w5_hw_alive(uint32_t magic, uint32_t stream) {
    return magic == W5_MAGIC && VDI_BASE == VDI_UP &&
           CLIENT_STREAM(W5_CLIENT) == stream && stream <= 1u && SLOT_USED(stream);
}

static inline int mirror_alive(const live_w5_m70 *m) {
    return w5_hw_alive(m->magic, m->stream);
}

/* Our instance is usable while the VDI is up, client 4 maps to our stream and the
 * service still has that stream slot open. After a preempt the stock exit has set the
 * client map to 0xff and closed the slot. */
static inline int alive(const struct live_w5 *w) {
    return w5_hw_alive(w->magic, w->stream);
}

static inline void w5_diag_eos_miss(live_w5_m70 *m) {
    if (!m) return;
    uint32_t cur = m->diag_close;
    uint32_t miss = cur & 0xffffu;
    if (miss < 0xffffu) miss++;
    m->diag_close = (cur & 0xffff0000u) | miss;
}
static inline void w5_diag_set_wstate(live_w5_m70 *m, uint32_t wstate) {
    if (!m) return;
    m->diag_close = (m->diag_close & 0xff00ffffu) | ((wstate & 0xffu) << 16);
}
static inline void w5_diag_set_stage(live_w5_m70 *m, uint32_t stage) {
    if (!m) return;
    m->diag_close = (m->diag_close & 0x00ffffffu) | ((stage & 0xffu) << 24);
}

static W5_V70_SEC void w5_reset_async_state(live_w5_m70 *m) {
    __atomic_store_n(&m->in_flight, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&m->park_err, 0u, __ATOMIC_RELEASE);
    m->in_flight_slot = 0u;
    m->in_flight_hw_idx = 0u;
    m->in_flight_stamp = 0u;
    m->last_valid_slot = -1;
    m->slot_valid_mask = 0u;
    m->next_slot_rr = 0u;
    m->batch_submitted = 0u;
    m->submitted_frames = 0u;
    m->in_flight_retrig = 0u;
    m->in_flight_pts = 0u;
    m->in_flight_t0_us = 0u;
    m->in_flight_idr_ptr = 0;
}

/* Populate the 4-slot manual-trigger table once per stream open (C3.11 0x40c38bb4 only
 * copies the 0x40-byte table to RAM in manual mode and never touches MMIO). */
static W5_V70_SEC int w5_submit_batch_locked(struct live_w5 *w, live_w5_m70 *m, int force) {
    if (!force && m->batch_submitted && w->batch_open) return 0;
    uint8_t *list = w->scratch + 0x800;
    uint64_t luma_span = (uint64_t)w->width * ((w->height + 31u) & ~31u);
    for (unsigned i = 0; i < W5_SLOTS; i++) {
        put64(list + i * 16, w->slot[i]);
        put64(list + i * 16 + 8, w->slot[i] + luma_span);
    }
    if (SUBMIT(w->stream, 0, list)) return -20;
    m->batch_submitted = 1u;
    w->batch_open = 1u;
    return 0;
}

/* Drain a 1-deep in-flight Wave5 picture before W5_STOP or session reset so VPU is 100% idle. */
static W5_V70_SEC int w5_drain_inflight_locked(struct live_w5 *w, live_w5_m70 *m, uint32_t timeout_us) {
    if (!__atomic_load_n(&m->in_flight, __ATOMIC_ACQUIRE)) return 0;
    uint32_t s = w->stream;
    volatile uint8_t *info = INFO(s, m->in_flight_hw_idx % 9u);
    uint32_t stamp = m->in_flight_stamp;
    uint64_t t0 = now_us();
    for (;;) {
        uint64_t addr = *(volatile uint64_t *)info;
        uint32_t sz = *(volatile uint32_t *)(info + 8);
        uint32_t pts90 = *(volatile uint32_t *)(info + 0x60);
        if (pts90 == stamp && sz && addr >> 32 == w->arena >> 32) {
            *(volatile uint32_t *)(info + 8) = 0;
            w->hw_index++;
            w->frames++;
            __atomic_store_n(&m->in_flight, 0u, __ATOMIC_RELEASE);
            __atomic_store_n(&m->park_err, 0u, __ATOMIC_RELEASE);
            return 0;
        }
        if (now_us() - t0 > timeout_us) return -24;
        DELAY(1);
    }
}

/* OPEN has no W5_MAGIC yet. Validate the exclusive pool/slot ownership without
 * treating an incomplete native initialization as a running encoder. */
static int w5_open_owned(const struct live_w5 *w) {
    uint32_t s = w->stream;
    if (s > 1u || w->close_failed ||
        __atomic_load_n(&w->release_req, __ATOMIC_ACQUIRE) ||
        VDI_BASE != VDI_UP || VDI_ARENA_BASE != w->arena ||
        CLIENT_STREAM(W5_CLIENT) != s || !SLOT_USED(s) || SLOT_USED(s ^ 1u)) return 0;
    for (unsigned c = 0; c < W5_CLIENT; c++) if (CLIENT_STREAM(c) != 0xffu) return 0;
    return 1;
}

/* The service ACK does not prove the worker has finished: SET_PARAMS returns
 * 0xa02d32768 while init completion writes 0xa02d32788. The native worker accepts
 * init only in state 0 (40c3b17c), publishes a valid instance before state 1
 * (40c3d4a0..40c3d524), then publishes state 2 only on success (40c3d8b4). */
static int w5_params_ready(struct live_w5 *w) {
    uint64_t start = now_us();
    for (;;) {
        if (!w5_open_owned(w)) return -12;
        uint32_t state = WSTATE(w->stream);
        if (state > 2u || (!state && w->native_instance)) return -12;
        if (state) {
            uint64_t p = W5_INST_PTR(w->stream);
            if (!p || (w->native_instance && p != w->native_instance)) return -12;
            const volatile uint32_t *inst = (const volatile uint32_t *)(uintptr_t)p;
            if (inst[0] != 1u || inst[3] != (W5_CODEC ? 1u : 3u)) return -12;
            w->native_instance = p;
            if (w->rmw) w5_m70(w->rmw)->native_instance = p;
            if (state == 2u) return 0;
        }
        if (now_us() - start >= 1000000u) return -13;
        DELAY(1);
    }
}

/* A retained photo-preview instance is resumable only while still exclusively
 * bound to this Live pool, slot and RUN instance with no busy/in-flight/failed close.
 * Caller holds runtime stock_handoff_lock, excluding preempt/dispose. */
__attribute__((minsize))
int live_encoder_retained(const live_encoder *e) {
 struct live_runtime *r=w5_runtime_of(e);
 if (!r || POOL_GATE) return 0;
 const live_w5_m70 *m=w5_m70_const(r);
 const struct live_w5 *w=e->hw;
 if (!w || m->magic!=W5_MAGIC || m->close_failed ||
     __atomic_load_n(&m->busy,__ATOMIC_ACQUIRE) ||
     __atomic_load_n(&m->in_flight,__ATOMIC_ACQUIRE) ||
     __atomic_load_n(&m->park_err,__ATOMIC_ACQUIRE) ||
     __atomic_load_n(&m->release_req,__ATOMIC_ACQUIRE) || !mirror_alive(m) ||
     w->magic!=W5_MAGIC || w->close_failed) return 0;
 for(unsigned c=0;c<W5_CLIENT;c++)if(CLIENT_STREAM(c)!=0xffu)return 0;
 if(SLOT_USED(m->stream^1u) || WSTATE(m->stream)!=4u ||
    VDI_ARENA_BASE!=m->arena)return 0;
 uint64_t p=W5_INST_PTR(m->stream);
 if(!p || p!=m->native_instance)return 0;
 const volatile uint32_t *inst=(const volatile uint32_t *)(uintptr_t)p;
 return inst[0]==1u && inst[3]==(W5_CODEC?1u:3u);
}

/* v27/v70: drain any 1 in-flight async picture first, then stock end-of-clip order
 * (SUBMIT if needed -> W5_STOP -> TRIGGER -> wait -> TRIGGER drain -> W5_FINISH -> EXIT). */
static __attribute__((noinline, minsize)) int w5_close_body(struct live_w5 *w) {
    uint32_t s = w->stream;
    live_w5_m70 *m = w->rmw ? w5_m70(w->rmw) : 0;
    volatile uint32_t *inst = (volatile uint32_t *)(uintptr_t)W5_INST_PTR(s);
    if (WSTATE(s) != 4u || !inst || (uint64_t)(uintptr_t)inst != w->native_instance ||
        VDI_ARENA_BASE != w->arena ||
        inst[0] != 1u || inst[3] != (W5_CODEC?1u:3u)) return 1;
    if (m && __atomic_load_n(&m->in_flight, __ATOMIC_ACQUIRE)) {
        if (w5_drain_inflight_locked(w, m, 500000u)) {
            w5_diag_eos_miss(m);
            if (POOL_GATE == 0u) w->eos_miss++;
            __atomic_store_n(&m->in_flight, 0u, __ATOMIC_RELEASE);
        }
    }
    unsigned k = w->frames % W5_SLOTS;
    if (!w->batch_open || (m && !m->batch_submitted)) {
        if (m) {
            if (w5_submit_batch_locked(w, m, 1)) return 2;
        } else {
            uint8_t *list = w->scratch + 0x800;
            for (unsigned i = 0; i < W5_SLOTS; i++) {
                put64(list + i * 16, w->slot[i]);
                put64(list + i * 16 + 8, w->slot[i] + (uint64_t)w->width * ((w->height + 31) & ~31u));
            }
            if (SUBMIT(s, 0, list)) return 2;
        }
    }
    if (W5_STOP(s)) return 3;
    uint32_t st0[9]; uint64_t ad0[9];
    for (unsigned i = 0; i < 9; i++) { st0[i] = *(volatile uint32_t *)(INFO(s, i) + 0x60); ad0[i] = *(volatile uint64_t *)INFO(s, i); }
    if (TRIGGER(s, 0, k, 0, 0)) return 4;
    uint64_t t = now_us();
    for (unsigned done = 0; !done; ) {
        for (unsigned i = 0; i < 9; i++) {
            volatile uint8_t *e = INFO(s, i);
            if (*(volatile uint32_t *)(e + 8) && (*(volatile uint32_t *)(e + 0x60) != st0[i] || *(volatile uint64_t *)e != ad0[i])) done = 1;
        }
        if (!done) { if (now_us() - t > 500000u) return 5; DELAY(1); }
    }
    w->hw_index++; w->frames++;
    if (TRIGGER(s, 0, (k + 1) % W5_SLOTS, 0, 0)) return 6;   /* drain */
    t = now_us();
    while (WSTATE(s) != 5u) { if (now_us() - t > 1000000u) return 7; DELAY(1); }
    w->fin_rc = 5u << 8;
    W5_FINISH(s);
    {
        uint32_t ws_after = WSTATE(s) & 0xffu;
        w->fin_rc |= ws_after << 16;
        w5_diag_set_wstate(m, ws_after);
    }
    if (WSTATE(s) != 0u || inst[0] != 0u) return 8;
    EXIT(W5_CLIENT);
    t = now_us();
    while (VDI_BASE) { if (now_us() - t > 1000000u) return 9; DELAY(1); }
    return 0;
}

static __attribute__((noinline, minsize)) void w5_close(struct live_w5 *w, struct live_runtime *r) {
    if (!r) return;
    live_w5_m70 *m = w5_m70(r);
    uint32_t free_ = 0; uint64_t t = now_us();
    while (!__atomic_compare_exchange_n(&r->w5_close_lock, &free_, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        free_ = 0;
        if (now_us() - t > 4000000u) {
            m->close_failed = 1u;
            w5_diag_eos_miss(m);
            w5_diag_set_stage(m, 11u);
            if (w && POOL_GATE == 0u) { w->close_failed = 1; w->eos_miss++; w->fin_rc = (w->fin_rc & 0xffffffu) | 11u << 24; }
            return;
        }
        DELAY(1);
    }
    if (mirror_alive(m) && !m->close_failed) {
        int st = (!w || w->magic != W5_MAGIC || w->arena != m->arena || w->rmw != r)
                 ? 12 : w5_close_body(w);
        if (st) {
            m->close_failed = 1u;
            w5_diag_eos_miss(m);
            w5_diag_set_stage(m, (uint32_t)st);
        } else {
            w5_reset_async_state(m);
        }
        if (w && POOL_GATE == 0u) {
            if (st) { w->close_failed = 1; w->eos_miss++; w->fin_rc = (w->fin_rc & 0xffffffu) | (uint32_t)st << 24; }
            else { w->eos_ok++; w->frames = 0; w->batch_open = 0; }
            w->closes++; w->last_close_us = (uint32_t)(now_us() - t);
        }
    }
    __atomic_store_n(&r->w5_close_lock, 0, __ATOMIC_RELEASE);
}

/* Retired in v32 (Wave5 close in photo worker caused mode-switch race); kept as ABI stub. */
int live_encoder_release(live_encoder *e) { (void)e; return 0; }

/* Lock-free fast-path probe for HangFix hooks: 1 while Wave5 pool blocks remain
 * allocated and eligible for handoff/disposal (0 on empty or permanently failed open/close). */
__attribute__((minsize))
int live_encoder_has_pool(const live_encoder *e) {
    struct live_runtime *r = w5_runtime_of(e);
    if (!r) return 0;
    const live_w5_m70 *m = w5_m70_const(r);
    return m->magic == W5_MAGIC && !m->close_failed;
}

/* The caller must first inhibit the worker and observe its stream/audio ACK.
 * Never free a pool that an incomplete native close or in-flight call may use.
 * v69/v70: reads disposal descriptors from cached live_runtime.w5_mirror so disposal
 * remains immune even if another layout reused class-0x24 DRAM meanwhile. */
__attribute__((minsize))
int live_encoder_dispose(live_encoder *e) {
    struct live_runtime *r = w5_runtime_of(e);
    if (!r) return 0;
    live_w5_m70 *m = w5_m70(r);
    struct live_w5 *w = __atomic_load_n(&e->hw, __ATOMIC_ACQUIRE);
    if (!m->arena_desc && !m->block_desc && !w) return 0;
    if (m->magic != W5_MAGIC) return -1;
    if (m->close_failed) return -2;
    if (__atomic_load_n(&m->busy, __ATOMIC_SEQ_CST) ||
        __atomic_load_n(&m->in_flight, __ATOMIC_SEQ_CST) ||
        __atomic_load_n(&m->release_req, __ATOMIC_SEQ_CST)) return -3;
    if (mirror_alive(m)) return -4;
    uint32_t mapped = CLIENT_STREAM(W5_CLIENT);
    if (mapped <= 1u && SLOT_USED(mapped)) return -5;
    void *arena = m->arena_desc, *block = m->block_desc;
    if (!arena || !block || arena == block) return -6;
    if (w && POOL_GATE == 0u) w->magic = 0;
    __atomic_store_n(&m->magic, 0u, __ATOMIC_RELEASE);
    m->arena_desc = 0; m->block_desc = 0; m->arena = 0; m->native_instance = 0; m->stream = 0xffu;
    w5_reset_async_state(m);
    __atomic_store_n(&e->hw, 0, __ATOMIC_RELEASE);
    e->persist = 0; e->scratch = 0; e->frames = 0; e->last_pts = 0;
    POOL_FREE(arena);
    POOL_FREE(block);
    return 0;
}

static __attribute__((noinline, minsize)) int open_stream(struct live_w5 *w) {
    w->native_instance=0;
    if (w->rmw) w5_m70(w->rmw)->native_instance = 0u;
    w->staging_scratch &= ~W5_ALIAS_MASK;
    uint8_t *s = w->scratch;
    uint32_t stream;
    if (w->close_failed) return LIVE_ENCODER_BUSY;   /* v31 */
    if (VDI_BASE) return LIVE_ENCODER_BUSY;
    if (MODE_REG != 0) return LIVE_ENCODER_BUSY;   /* v30: bring-up only in photo mode */
    if (BRINGUP(W5_CLIENT, 8000u /* v25: service bring-up rate as up to v14 */, w->arena, W5_ARENA) != 1) return -3;
    stream = CLIENT_STREAM(W5_CLIENT);
    if (stream > 1) return -4;
    w->stream = stream;
    if (w->rmw) w5_m70(w->rmw)->stream = stream;
    if (OPEN(stream, NOOP_CB)) return -5;
    uint32_t cfg[8] = {W5_CODEC, w->width, (w->height + 31) & ~31u, W5_KBPS, 1, 0, 8, 8};
    memcpy(s, cfg, sizeof cfg);
    if (CONFIGURE(s, s + 0x100)) return -6;
    table(w, s + 0x200);
    if (REGISTER(stream, s + 0x200)) return -7;
    /* movie mode-table entry layout (0x24 B): codec,?,depth,w,h,pad | fps num,den,gop,structure,bps | mode */
    uint8_t *m = s + 0x300;
    memset(m, 0, 0x24);
    uint16_t h16[6] = {(uint16_t)W5_CODEC, 0, 8, (uint16_t)w->width, (uint16_t)w->height, 0};
    uint32_t w32[5] = {LIVE_W5_FPS_NUM, LIVE_W5_FPS_DEN, 15, 1, W5_KBPS * 1000u};
    memcpy(m, h16, 12); memcpy(m + 12, w32, 20); m[0x20] = 1;
    memset(s + 0x400, 0, 0x220);
    PARAMS(m, 0, w->region[R_USER], s + 0x400);
    if (!w5_open_owned(w) || WSTATE(stream) != 0u) return -12;
    if (SET_PARAMS(stream, s + 0x400, 0)) return -8;
    int ready = w5_params_ready(w);
    if (ready) return ready;
    struct w5_header_reply *reply = (void *)(s + W5_HEADER_REPLY_OFF);
    memset(reply, 0, sizeof(*reply));
    if (HEADERS(stream, &reply->data, &reply->bytes, reply->lens) || reply->data >> 32 != w->arena >> 32) return -9;
    uint64_t hdr = reply->data; const uint32_t *lens = reply->lens;
    /* AUD, SPS, PPS[, SEI]: each length excludes its 4-byte start code */
    const uint8_t *h = (const uint8_t *)(uintptr_t)hdr, *sps = 0, *pps = 0;
    uint32_t sn = 0, pn = 0, at = 0;
    if (W5_CODEC) { memset(&w->hevc, 0, sizeof w->hevc); w->hevc_magic = 0x31564548u; w->hevc_hdr_types = 0; }
    for (unsigned i = 0; i < 4; i++) {
        if (!lens[i]) continue;
        const uint8_t *nal = h + at + 4;
        if (W5_CODEC) {
            w->hevc_hdr_types |= (uint32_t)(((nal[0] >> 1) & 63) | 0x80) << (8 * i);
            if (lens[i] > sizeof w->hevc.sps_raw || live_hevc_param(&w->hevc, nal, lens[i], w->width, w->height)) { w->hevc_open_rc = 1; return -10; }
        } else {
            if ((nal[0] & 31) == 7) { sps = nal; sn = lens[i]; }
            if ((nal[0] & 31) == 8) { pps = nal; pn = lens[i]; }
        }
        at += lens[i] + 4;
    }
    if (W5_CODEC) { if (!w->hevc.ready) { w->hevc_open_rc = 2; return -10; } w->hevc_open_rc = 0; }
    else if (!sps || !pps || live_h264_idr_init(&w->idr, sps, sn, pps, pn)) return -10;
    uint32_t run[4] = {1, 1, 15, 15};
    memcpy(s + 0x700, run, sizeof run);
    if (RUN(stream, s + 0x700)) return -11;
    w->native_instance = W5_INST_PTR(stream);
    w->hw_index = 0; w->batch_open = 0; w->frames = 0;
    if (w->rmw) {
        live_w5_m70 *m70 = w5_m70(w->rmw);
        m70->native_instance = w->native_instance;
        w5_reset_async_state(m70);
    }
    return 0;
}

void live_encoder_set_flags(live_encoder *e, unsigned f) { if (e && e->hw && e->hw->magic == W5_MAGIC) e->hw->flags = f; }
int live_encoder_sizes(unsigned w, unsigned h, size_t *persist, size_t *scratch) {
    if (!persist || !scratch || !w || !h || (w & 1) || (h & 1) || w > 1920 || h > 1088) return -1;
    *persist = 16; *scratch = 2 * (size_t)W5_AU_CAP + 64;   /* AU/wrap staging in Live's (cacheable) allocation */
    return 0;
}

__attribute__((minsize))
int live_encoder_init(live_encoder *e, void *p, void *s, unsigned width, unsigned height) {
    if (!e || !p || !s) return -1;
    struct live_w5 *w = e->hw;
    e->persist = p; e->scratch = s; e->width = width; e->height = height; e->frames = 0; e->last_pts = 0;
    if (w) {
        /* Live switched off and on: reuse the instance (geometry fixed per boot), rebuilding
         * it on the same memory if a movie recording powered Wave5 down meanwhile. A failed
         * first bring-up is not retried: its pool blocks stay pinned. */
        if (w->magic != W5_MAGIC || !w->rmw) return -1;
        struct live_runtime *r = w->rmw;
        live_w5_m70 *m = w5_m70(r);
        __atomic_store_n(&m->busy, 1, __ATOMIC_SEQ_CST);   /* v69: cached RMW busy flag */
        int ret = 0;
        if (alive(w) && __atomic_load_n(&m->in_flight, __ATOMIC_ACQUIRE)) {
            if (w5_drain_inflight_locked(w, m, 500000u)) {
                if (alive(w)) __atomic_store_n(&m->park_err, 24u, __ATOMIC_RELEASE);
                ret = -24;
            }
        }
        if (!ret && (w->width != width || w->height != height)) {
            if ((size_t)width * ((height + 31) & ~31u) * 3 / 2 > w->slot_bytes) ret = -1;
            else { if (alive(w)) w5_close(w, r); w->width = width; w->height = height; }
        }
        if (!ret && !alive(w)) {
            int rc = open_stream(w);
            if (rc && rc != LIVE_ENCODER_BUSY) { w->magic = 0; m->close_failed = 1u; ret = -1; }
        }
        if (!ret) { w->frames = 0; w->batch_open = 0; w5_reset_async_state(m); }
        __atomic_store_n(&m->busy, 0, __ATOMIC_SEQ_CST);
        return ret;
    }
    /* Resolve the single cached owner before allocating/publishing pool state.
     * Pool memory cannot support exclusive RMW on this hardware. */
    struct live_runtime *r = live_ui_engine_get();
#ifndef LIVE_W5_HOST_MOCK
    if (!r || (uintptr_t)r < UINT64_C(0xa00000000) ||
        (uintptr_t)r > UINT64_C(0xa40000000) - sizeof(*r) || ((uintptr_t)r & 7u) ||
        r->magic != LIVE_RUNTIME_MAGIC || r->size != sizeof(*r) ||
        e != &r->stream.encoder) return -1;
#else
    if (!r || r->magic != LIVE_RUNTIME_MAGIC || r->size != sizeof(*r) ||
        e != &r->stream.encoder) return -1;
#endif
    live_w5_m70 *m = w5_m70(r);
    void *ad = 0, *bd = 0; uint64_t a, b; uint32_t an, bn;
    /* v17: slots for the larger of this size and 1280x852 (channel 2 at 1:1), so the size may change later */
    size_t sw = width > 1280u ? width : 1280u, sh = height > 852u ? height : 852u;
    uint32_t slot = (uint32_t)((sw * ((sh + 31) & ~(size_t)31) * 3 / 2 + 0xfff) & ~(size_t)0xfff);
    uint32_t need = (uint32_t)((sizeof(struct live_w5) + 0xfff) & ~(size_t)0xfff) + W5_SLOTS * slot;
    for (unsigned i = 0; i < R_COUNT; i++) need += region_size[i];
    if (pool(W5_ARENA + 0x1000, &ad, &a, &an)) return -1;
    if (pool(need + 0x1000, &bd, &b, &bn)) { POOL_FREE(ad); return -1; }
    a = (a + 0xfff) & ~UINT64_C(0xfff); b = (b + 0xfff) & ~UINT64_C(0xfff);
#ifndef LIVE_W5_HOST_MOCK
    if ((a & UINT64_C(0xfffffff800000fff)) != UINT64_C(0x800000000) || a >> 32 != b >> 32) {
        POOL_FREE(ad); POOL_FREE(bd); return -1;
    }
#endif
    w = (struct live_w5 *)(uintptr_t)b;
    memset(w, 0, sizeof(*w));
    w->rmw = r; /* bound before e->hw publication; never reset the shared lock */
    w->arena = a; w->arena_desc = ad; w->block_desc = bd; w->width = width; w->height = height; w->slot_bytes = slot;
    w->flags = 6u; /* v70 default: cached_slot | cached_bs ON; live_encoder_set_flags overrides */
    uint64_t at = b + ((sizeof(struct live_w5) + 0xfff) & ~(size_t)0xfff);
    for (unsigned i = 0; i < R_COUNT; i++) { w->region[i] = at; at += region_size[i]; }
    for (unsigned i = 0; i < W5_SLOTS; i++) { w->slot[i] = at; at += slot; }
    memset(m, 0, sizeof(*m));
    m->stream = 0xffu; m->last_valid_slot = -1;
    m->arena = a; m->arena_desc = ad; m->block_desc = bd;
    __atomic_store_n(&m->magic, W5_MAGIC, __ATOMIC_RELEASE);
    __atomic_store_n(&e->hw, w, __ATOMIC_RELEASE);
    int rc = open_stream(w);
    if (rc == LIVE_ENCODER_BUSY) { w->magic = W5_MAGIC; return 0; }   /* opened on a later frame */
    if (rc) { m->close_failed = 1u; return rc; }
    w->magic = W5_MAGIC;
    return 0;
}

/* Bring-up hook (0x402f2d58 entry) / HangFix handoff, any task: another client is
 * about to take Wave5 or switch pool layout. Wait for busy==0, drain any 1 in-flight
 * async picture inside w5_close_body(), then release with the stock exit. */
__attribute__((minsize))
void live_encoder_preempt(live_encoder *e, uint32_t client) {
    struct live_runtime *r = w5_runtime_of(e);
    if (!r || client == W5_CLIENT) return;
    live_w5_m70 *m = w5_m70(r);
    if (m->magic != W5_MAGIC || !mirror_alive(m)) return;
    struct live_w5 *w = __atomic_load_n(&e->hw, __ATOMIC_ACQUIRE);
    __atomic_store_n(&m->release_req, 1, __ATOMIC_SEQ_CST);
    uint64_t start = now_us();
    while (__atomic_load_n(&m->busy, __ATOMIC_SEQ_CST) && now_us() - start < 4000000u) DELAY(1);
    if (__atomic_load_n(&m->busy, __ATOMIC_SEQ_CST)) {
        m->close_failed = 1u;
        w5_diag_eos_miss(m);
        w5_diag_set_stage(m, 10u);
        if (w && POOL_GATE == 0u) { w->close_failed = 1; w->eos_miss++; w->fin_rc = (w->fin_rc & 0xffffffu) | 10u << 24; }
    } else if (mirror_alive(m)) w5_close(w, r);
    if (w && POOL_GATE == 0u && w->magic == W5_MAGIC) {
        w->frames = 0; w->batch_open = 0; w->preempts++; w->preempt_us = now_us();
    }
    if (!m->close_failed) w5_reset_async_state(m);
    __atomic_store_n(&m->release_req, 0, __ATOMIC_SEQ_CST);
}

int live_encoder_frame(live_encoder *e, const uint8_t *in, const uint8_t **out, size_t *bytes) {
    uint64_t last = 0; unsigned idr;
    return live_encoder_frame_timed(e, in, e->last_pts + 33367, &last, W5_KBPS * 1000u, 500000u, out, bytes, &idr);
}

/* Validate all NV12 slot aliases once per stream open and return slot's cached VA. */
static uintptr_t slot_cached_va(struct live_w5 *w, unsigned slot, size_t span) {
    if (slot >= W5_SLOTS) return 0;
    if (!(w->staging_scratch & W5_ALIAS_SLOTS_TRIED)) {
        unsigned ok = 1;
        for (unsigned i = 0; i < W5_SLOTS; i++) {
            uintptr_t va = (uintptr_t)w->slot[i];
            uintptr_t al = w->slot_bytes ? la_alias(va, w->slot_bytes) : 0;
            if (!al || al != va + (uintptr_t)W5_CACHE_OFFSET) { ok = 0; break; }
        }
        w->staging_scratch |= W5_ALIAS_SLOTS_TRIED | (ok ? W5_ALIAS_SLOTS_OK : 0u);
    }
    if ((w->staging_scratch & W5_ALIAS_SLOTS_OK) && span && span <= w->slot_bytes)
        return (uintptr_t)w->slot[slot] + (uintptr_t)W5_CACHE_OFFSET;
    return 0;
}

/* Validate contiguous [R_BS0..R_BS1] bitstream alias once per stream open. */
static uintptr_t bs_cached_va(struct live_w5 *w, uint64_t addr, size_t n) {
    uint64_t bs0 = w->region[R_BS0];
    uint64_t end = w->region[R_BS1] + region_size[R_BS1];
    if (!(w->staging_scratch & W5_ALIAS_BS_TRIED)) {
        unsigned ok = 0;
        if (bs0 && bs0 + region_size[R_BS0] == w->region[R_BS1] && end > bs0) {
            size_t bs_span = (size_t)(end - bs0);
            uintptr_t al = la_alias((uintptr_t)bs0, bs_span);
            if (al && al == (uintptr_t)bs0 + (uintptr_t)W5_CACHE_OFFSET) ok = 1;
        }
        w->staging_scratch |= W5_ALIAS_BS_TRIED | (ok ? W5_ALIAS_BS_OK : 0u);
    }
    if ((w->staging_scratch & W5_ALIAS_BS_OK) && n && addr >= bs0 && addr <= end && n <= (size_t)(end - addr))
        return (uintptr_t)addr + (uintptr_t)W5_CACHE_OFFSET;
    return 0;
}

typedef uint32_t __attribute__((may_alias)) w5_u32a;
typedef uint64_t __attribute__((may_alias)) w5_u64a;
/* Interleave one planar chroma row (chroma_w U/V samples -> 2*chroma_w NV12 bytes).
 * Zero memcpy calls (stock memcpy under -fno-builtin is byte-by-byte). */
static __attribute__((noinline)) void w5_uv_interleave_row(uint8_t *row, const uint8_t *u_row, const uint8_t *v_row, unsigned chroma_w) {
    unsigned x = 0, k = 0;
    w5_u64a *dst64 = (w5_u64a *)(void *)row;
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    int aln = (((uintptr_t)u_row | (uintptr_t)v_row) & 3u) == 0u;
#else
    int aln = 0;
#endif
    for (; x + 4u <= chroma_w; x += 4u, k++) {
        uint64_t xu, xv;
        if (aln) {
            xu = ((const w5_u32a *)(const void *)u_row)[k];
            xv = ((const w5_u32a *)(const void *)v_row)[k];
        } else {
            xu = (uint32_t)u_row[x] | ((uint32_t)u_row[x + 1u] << 8) |
                 ((uint32_t)u_row[x + 2u] << 16) | ((uint32_t)u_row[x + 3u] << 24);
            xv = (uint32_t)v_row[x] | ((uint32_t)v_row[x + 1u] << 8) |
                 ((uint32_t)v_row[x + 2u] << 16) | ((uint32_t)v_row[x + 3u] << 24);
        }
        xu = (xu | (xu << 16)) & UINT64_C(0x0000ffff0000ffff);
        xu = (xu | (xu << 8)) & UINT64_C(0x00ff00ff00ff00ff);
        xv = (xv | (xv << 16)) & UINT64_C(0x0000ffff0000ffff);
        xv = (xv | (xv << 8)) & UINT64_C(0x00ff00ff00ff00ff);
        dst64[k] = xu | (xv << 8);
    }
    for (; x < chroma_w; x++) {
        row[2u * x] = u_row[x];
        row[2u * x + 1u] = v_row[x];
    }
}

/* Pack I420 frame into Wave5 NV12 slot with coded-height bottom-row replication. */
static int w5_pack_surface(struct live_w5 *w, unsigned slot, const uint8_t *in, unsigned width, unsigned height) {
    if (slot >= W5_SLOTS || !in || !width || !height || (width & 1u) || (height & 1u) || width > 4096u) return -27;
    unsigned coded_h = (height + 15u) & ~15u;
    unsigned luma_pad_h = (height + 31u) & ~31u;
    size_t slot_span = (size_t)width * luma_pad_h + (size_t)width * (coded_h / 2u);
    if (slot_span > w->slot_bytes) return -27;
    uint8_t *y = (uint8_t *)(uintptr_t)w->slot[slot];
    int slot_cached = 0;
    if (w->flags & 2u) {
        uintptr_t a = slot_cached_va(w, slot, slot_span);
        if (a) { y = (uint8_t *)a; slot_cached = 1; w->alias_slot_ok++; } else w->alias_slot_fail++;
    }
    uint8_t *c = y + (size_t)width * luma_pad_h;
    const uint8_t *u = in + (size_t)width * height, *v = u + (size_t)width * height / 4u;
    w5_burst(y, in, (size_t)width * height);
    const uint8_t *last_y = in + (size_t)(height - 1u) * width;
    for (unsigned py = height; py < coded_h; py++)
        w5_burst(y + (size_t)py * width, last_y, width);
    {
        uint8_t row[4096] __attribute__((aligned(16)));
        unsigned chroma_w = width / 2u, chroma_h = height / 2u, coded_ch = coded_h / 2u;
        int direct = slot_cached && (((uintptr_t)c | (uintptr_t)width) & 7u) == 0u;
        for (unsigned r = 0; r < chroma_h; r++) {
            uint8_t *dst = direct ? c + (size_t)r * width : row;
            w5_uv_interleave_row(dst, u + (size_t)r * chroma_w, v + (size_t)r * chroma_w, chroma_w);
            if (!direct) w5_burst(c + (size_t)r * width, row, width);
        }
        const uint8_t *last_c = direct ? c + (size_t)(chroma_h - 1u) * width : row;
        for (unsigned pr = chroma_h; pr < coded_ch; pr++)
            w5_burst(c + (size_t)pr * width, last_c, width);
    }
    if (slot_cached) la_dc_cvac((uintptr_t)y, slot_span);   /* the VPU reads PA memory */
    return 0;
}

/* Ensure Wave5 stream is open & usable while caller holds m->busy == 1. */
static W5_V70_SEC int w5_ensure_stream_locked(struct live_w5 *w, live_w5_m70 *m) {
    if (w->close_failed || m->close_failed) return LIVE_ENCODER_BUSY;
    if (alive(w)) return 0;
    if (w->preempt_us && now_us() - w->preempt_us < 500000u) { w->reopen_blocked++; return LIVE_ENCODER_BUSY; }
    int rc = open_stream(w);
    if (rc == LIVE_ENCODER_BUSY) return rc;
    if (rc) { w->magic = 0; m->close_failed = 1u; return -25; }
    w->frames = 0;
    return 0;
}

/* Harvest the 1 in-flight picture (caller holds m->busy == 1 and m->in_flight == 1).
 * Returns: 1 = harvested, 0 = still encoding (block==0 only), LIVE_ENCODER_BUSY = preempted, <0 = err. */
static W5_V70_SEC int w5_harvest_locked(live_encoder *e, struct live_w5 *w, live_w5_m70 *m,
                                        int block, uint64_t *pts_out, const uint8_t **out,
                                        size_t *bytes, unsigned *idr) {
    if (!alive(w)) { __atomic_store_n(&m->in_flight, 0u, __ATOMIC_RELEASE); return LIVE_ENCODER_BUSY; }
    volatile uint8_t *info = INFO(w->stream, m->in_flight_hw_idx % 9u);
    uint32_t stamp = m->in_flight_stamp;
    uint64_t wait_start = now_us();
    for (;;) {
        uint64_t addr = *(volatile uint64_t *)info;
        uint32_t size = *(volatile uint32_t *)(info + 8);
        uint32_t pts90 = *(volatile uint32_t *)(info + 0x60);
        if (pts90 == stamp && size && addr >> 32 == w->arena >> 32) {
            uint64_t t_done = now_us();
            uint64_t dt_wait = t_done - wait_start;
            w->t_wait = dt_wait; w->s_wait += dt_wait;
            m->sum_wait_us += dt_wait;
            m->last_poll_wait_us = (uint32_t)(dt_wait > UINT32_MAX ? UINT32_MAX : dt_wait);
            uint64_t out_pts = m->in_flight_pts;
            uint64_t *idr_ptr = m->in_flight_idr_ptr;

            /* Discard mode (flush from cfr_reset / release): skip BS copy & AU parse. */
            if (!out || !bytes) {
                *(volatile uint32_t *)(info + 8) = 0;
                w->hw_index++; w->frames++; e->frames++; e->last_pts = out_pts;
                __atomic_store_n(&m->in_flight, 0u, __ATOMIC_RELEASE);
                __atomic_store_n(&m->park_err, 0u, __ATOMIC_RELEASE);
                if (pts_out) *pts_out = out_pts;
                if (idr) *idr = 0;
                return 1;
            }

            uint64_t bs0 = w->region[R_BS0], end = w->region[R_BS1] + region_size[R_BS1];
            if (addr < bs0 || addr >= end || size > W5_AU_CAP - 4096) {
                *(volatile uint32_t *)(info + 8) = 0;
                w->hw_index++; w->frames++;
                __atomic_store_n(&m->in_flight, 0u, __ATOMIC_RELEASE);
                return -22;
            }

            uint8_t *stage_au = w->au, *stage_wrap = w->wrap;
            if (e->scratch) {
                stage_au = (uint8_t *)(((uintptr_t)e->scratch + 63) & ~(uintptr_t)63);
                stage_wrap = stage_au + W5_AU_CAP;
                w->staging_scratch |= W5_STAGED_SCRATCH;
            }

            const uint8_t *src = (const uint8_t *)(uintptr_t)addr;
            if (*(volatile uint32_t *)(info + 0x0c)) {
                uint64_t a1 = *(volatile uint64_t *)(info + 0x10), a2 = *(volatile uint64_t *)(info + 0x20);
                uint32_t n1 = *(volatile uint32_t *)(info + 0x18), n2 = *(volatile uint32_t *)(info + 0x28);
                if (a1 < bs0 || a1 + n1 > end || a2 < bs0 || a2 + n2 > end || n1 + n2 != size) {
                    *(volatile uint32_t *)(info + 8) = 0;
                    w->hw_index++; w->frames++;
                    __atomic_store_n(&m->in_flight, 0u, __ATOMIC_RELEASE);
                    return -26;
                }
                uintptr_t c1 = (w->flags & 4u) ? bs_cached_va(w, a1, n1) : 0;
                uintptr_t c2 = (w->flags & 4u) ? bs_cached_va(w, a2, n2) : 0;
                if (c1 && c2) { la_dc_ivac(c1, n1); la_dc_ivac(c2, n2); a1 = c1; a2 = c2; w->alias_bs_ok++; }
                else if (w->flags & 4u) w->alias_bs_fail++;
                w5_burst(stage_wrap, (const uint8_t *)(uintptr_t)a1, n1);
                w5_burst(stage_wrap + n1, (const uint8_t *)(uintptr_t)a2, n2);
                src = stage_wrap;
            } else if (addr + size > end) {
                *(volatile uint32_t *)(info + 8) = 0;
                w->hw_index++; w->frames++;
                __atomic_store_n(&m->in_flight, 0u, __ATOMIC_RELEASE);
                return -22;
            } else {
                /* Opt C: when cached BS alias is valid, invalidate cache and parse DIRECTLY
                 * from cached BS without copying through stage_wrap! */
                uintptr_t ca = (w->flags & 4u) ? bs_cached_va(w, addr, size) : 0;
                if (ca) {
                    la_dc_ivac(ca, size);
                    src = (const uint8_t *)ca;
                    w->alias_bs_ok++;
                } else {
                    if (w->flags & 4u) w->alias_bs_fail++;
                    if (size + 16 <= W5_AU_CAP) {
                        uint8_t *dst = stage_wrap + (addr & 15);
                        w5_burst(dst, src, size);
                        src = dst;
                    }
                }
            }

            uint64_t t_parse0 = now_us();
            uint64_t dt_out = t_parse0 - t_done;
            w->t_out = dt_out; w->s_out += dt_out; m->sum_out_us += dt_out;

            size_t n = 0;
            unsigned local_idr = 0;
            int prc = W5_CODEC
                ? live_hevc_au(&w->hevc, src, size, stage_au, W5_AU_CAP, &n, &local_idr)
                : live_h264_idr_au(&w->idr, src, size, stage_au, W5_AU_CAP, &n, &local_idr);

            uint64_t dt_parse = now_us() - t_parse0;
            w->t_parse = dt_parse; w->s_parse += dt_parse; m->sum_parse_us += dt_parse;
            w->n_timed++; m->n_timed_v70++;

            *(volatile uint32_t *)(info + 8) = 0;   /* consumed: stale entry never matches again */
            w->hw_index++; w->frames++; e->frames++; e->last_pts = out_pts;
            __atomic_store_n(&m->in_flight, 0u, __ATOMIC_RELEASE);
            __atomic_store_n(&m->park_err, 0u, __ATOMIC_RELEASE);

            if (prc) return -23;
            if (local_idr && idr_ptr) *idr_ptr = out_pts;
            if (pts_out) *pts_out = out_pts;
            *out = stage_au; *bytes = n;
            if (idr) *idr = local_idr;
            return 1;
        }
        if (now_us() - m->in_flight_t0_us > W5_WAIT_US) {
            if (!alive(w)) { __atomic_store_n(&m->in_flight, 0u, __ATOMIC_RELEASE); return LIVE_ENCODER_BUSY; }
            __atomic_store_n(&m->park_err, 24u, __ATOMIC_RELEASE);
            return -24;
        }
        if (!block) return 0;
        if (__atomic_load_n(&m->release_req, __ATOMIC_SEQ_CST)) return LIVE_ENCODER_BUSY;
        la_native_yield();
    }
}

/* ---- V70 Opt A + Opt B Public API (placed in .filmsim to save live_primary) ---- */

W5_V70_SEC int live_encoder_nv12_begin(live_encoder *e, unsigned *slot_out,
                                       uint8_t **y_out, uint8_t **uv_out,
                                       unsigned *y_stride_out, unsigned *uv_stride_out,
                                       unsigned *coded_h_out, int *cached_out) {
    struct live_w5 *w = e ? e->hw : 0;
    if (!w || w->magic != W5_MAGIC || !w->rmw || !slot_out || !y_out || !uv_out) return -1;
    live_w5_m70 *m = w5_m70(w->rmw);
    __atomic_store_n(&m->busy, 1, __ATOMIC_SEQ_CST);
    int rc = 0;
    if (__atomic_load_n(&m->release_req, __ATOMIC_SEQ_CST)) rc = LIVE_ENCODER_BUSY;
    else rc = w5_ensure_stream_locked(w, m);
    if (rc) {
        if (rc == LIVE_ENCODER_BUSY) w->busy_drops++;
        __atomic_store_n(&m->busy, 0, __ATOMIC_SEQ_CST);
        return rc;
    }

    unsigned width = w->width, height = w->height;
    unsigned coded_h = (height + 15u) & ~15u;
    unsigned luma_pad_h = (height + 31u) & ~31u;
    size_t slot_span = (size_t)width * luma_pad_h + (size_t)width * (coded_h / 2u);
    if (!width || !height || (width & 1u) || (height & 1u) || slot_span > w->slot_bytes) {
        __atomic_store_n(&m->busy, 0, __ATOMIC_SEQ_CST);
        return -27;
    }

    /* Pick a slot disjoint from both in_flight_slot and last_valid_slot (W5_SLOTS == 4 guarantees one). */
    unsigned slot = m->next_slot_rr % W5_SLOTS;
    for (unsigned i = 0; i < W5_SLOTS; i++) {
        unsigned cand = (slot + i) % W5_SLOTS;
        if (m->in_flight && cand == m->in_flight_slot) continue;
        if (m->last_valid_slot >= 0 && (int32_t)cand == m->last_valid_slot) continue;
        slot = cand;
        break;
    }
    m->next_slot_rr = (slot + 1u) % W5_SLOTS;
    m->slot_valid_mask &= ~(1u << slot);

    uint8_t *y = (uint8_t *)(uintptr_t)w->slot[slot];
    int slot_cached = 0;
    if (w->flags & 2u) {
        uintptr_t a = slot_cached_va(w, slot, slot_span);
        if (a) { y = (uint8_t *)a; slot_cached = 1; w->alias_slot_ok++; }
        else w->alias_slot_fail++;
    }
    *slot_out = slot;
    *y_out = y;
    *uv_out = y + (size_t)width * luma_pad_h;
    if (y_stride_out) *y_stride_out = width;
    if (uv_stride_out) *uv_stride_out = width;
    if (coded_h_out) *coded_h_out = coded_h;
    if (cached_out) *cached_out = slot_cached;

    __atomic_store_n(&m->busy, 0, __ATOMIC_SEQ_CST);
    return 0;
}

W5_V70_SEC int live_encoder_submit_slot(live_encoder *e, unsigned slot, int already_cleaned,
                                        uint64_t pts, uint64_t *last_idr,
                                        unsigned bitrate, unsigned interval) {
    struct live_w5 *w = e ? e->hw : 0;
    (void)bitrate;
    if (!w || w->magic != W5_MAGIC || !w->rmw || slot >= W5_SLOTS || !last_idr || !interval) return -1;
    live_w5_m70 *m = w5_m70(w->rmw);
    __atomic_store_n(&m->busy, 1, __ATOMIC_SEQ_CST);
    int rc = 0;
    if (__atomic_load_n(&m->release_req, __ATOMIC_SEQ_CST)) {
        rc = LIVE_ENCODER_BUSY;
    } else if (__atomic_load_n(&m->in_flight, __ATOMIC_ACQUIRE)) {
        rc = W5_ERR_IN_FLIGHT;
    } else {
        rc = w5_ensure_stream_locked(w, m);
    }
    if (rc) {
        if (rc == LIVE_ENCODER_BUSY) w->busy_drops++;
        __atomic_store_n(&m->busy, 0, __ATOMIC_SEQ_CST);
        return rc;
    }

    int is_retrig = (m->last_valid_slot >= 0 && (unsigned)m->last_valid_slot == slot &&
                     (m->slot_valid_mask & (1u << slot)) != 0u);

    rc = w5_submit_batch_locked(w, m, 0);
    if (rc) {
        if (!alive(w)) rc = LIVE_ENCODER_BUSY;
        if (rc == LIVE_ENCODER_BUSY) w->busy_drops++;
        __atomic_store_n(&m->busy, 0, __ATOMIC_SEQ_CST);
        return rc;
    }

    uint64_t t_in0 = now_us();
    if (!already_cleaned && (w->flags & 2u)) {
        unsigned coded_h = (w->height + 15u) & ~15u;
        unsigned luma_pad_h = (w->height + 31u) & ~31u;
        size_t slot_span = (size_t)w->width * luma_pad_h + (size_t)w->width * (coded_h / 2u);
        uintptr_t a = slot_cached_va(w, slot, slot_span);
        if (a) la_dc_cvac(a, slot_span);
    }
    uint64_t dt_in = now_us() - t_in0;
    w->t_in = dt_in; w->s_in += dt_in; m->sum_in_us += dt_in;

    int force_reset = (*last_idr == 0xFFFFFFFF00000000ULL);
    unsigned force = W5_CODEC ? (!m->submitted_frames || force_reset)
                              : (!m->submitted_frames || force_reset || !*last_idr || pts - *last_idr >= interval);
    w->hevc_force += force;
    if (TRIGGER(w->stream, 0, slot, 0, force)) {
        rc = !alive(w) ? LIVE_ENCODER_BUSY : -21;
        if (rc == LIVE_ENCODER_BUSY) w->busy_drops++;
        __atomic_store_n(&m->busy, 0, __ATOMIC_SEQ_CST);
        return rc;
    }
    if (force_reset) *last_idr = 0;

    m->in_flight_slot = slot;
    m->in_flight_hw_idx = w->hw_index;
    m->in_flight_stamp = w->hw_index * 3000u;
    m->in_flight_pts = pts;
    m->in_flight_t0_us = now_us();
    m->in_flight_idr_ptr = last_idr;
    m->in_flight_retrig = (uint32_t)is_retrig;
    m->submitted_frames++;
    m->last_valid_slot = (int32_t)slot;
    m->slot_valid_mask |= (1u << slot);
    if (is_retrig) m->async_retrig_cnt++;
    else m->direct_nv12_cnt++;
    __atomic_store_n(&m->in_flight, 1u, __ATOMIC_RELEASE);

    __atomic_store_n(&m->busy, 0, __ATOMIC_SEQ_CST);
    return 0;
}

W5_V70_SEC int live_encoder_poll_au(live_encoder *e, int block, uint64_t *pts_out,
                                    const uint8_t **out, size_t *bytes, unsigned *idr) {
    if (!e) return -1;
    struct live_w5 *w = e->hw;
    if (!w) return 0;
    if (w->magic != W5_MAGIC || !w->rmw) return -1;
    live_w5_m70 *m = w5_m70(w->rmw);
    if (!__atomic_load_n(&m->in_flight, __ATOMIC_ACQUIRE)) return 0;

    __atomic_store_n(&m->busy, 1, __ATOMIC_SEQ_CST);
    int rc;
    if (!__atomic_load_n(&m->in_flight, __ATOMIC_ACQUIRE)) {
        rc = 0;
    } else if (__atomic_load_n(&m->release_req, __ATOMIC_SEQ_CST)) {
        rc = LIVE_ENCODER_BUSY;
    } else {
        rc = w5_harvest_locked(e, w, m, block, pts_out, out, bytes, idr);
        if (rc < 0 && rc != LIVE_ENCODER_BUSY && w->magic == W5_MAGIC && !alive(w)) {
            __atomic_store_n(&m->in_flight, 0u, __ATOMIC_RELEASE);
            rc = LIVE_ENCODER_BUSY;
        }
    }
    if (rc == LIVE_ENCODER_BUSY) w->busy_drops++;
    __atomic_store_n(&m->busy, 0, __ATOMIC_SEQ_CST);
    return rc;
}

W5_V70_SEC int live_encoder_flush(live_encoder *e, uint64_t *pts_out,
                                  const uint8_t **out, size_t *bytes, unsigned *idr) {
    return live_encoder_poll_au(e, 1, pts_out, out, bytes, idr);
}

W5_V70_SEC int live_encoder_last_valid_slot(const live_encoder *e) {
    struct live_runtime *r = w5_runtime_of(e);
    if (!r) return -1;
    const live_w5_m70 *m = w5_m70_const(r);
    if (m->magic != W5_MAGIC || m->close_failed) return -1;
    int32_t s = m->last_valid_slot;
    if (s < 0 || s >= (int32_t)W5_SLOTS || !(m->slot_valid_mask & (1u << (unsigned)s))) return -1;
    return (int)s;
}

W5_V70_SEC int live_encoder_slot_ptrs(const live_encoder *e, unsigned slot,
                                      const uint8_t **y_out, const uint8_t **uv_out,
                                      unsigned *y_stride_out, unsigned *uv_stride_out) {
    if (!e || slot >= W5_SLOTS || !y_out || !uv_out) return -1;
    struct live_runtime *r = w5_runtime_of(e);
    if (!r || POOL_GATE) return -1;
    const live_w5_m70 *m = w5_m70_const(r);
    const struct live_w5 *w = e->hw;
    if (!w || m->magic != W5_MAGIC || m->close_failed || w->magic != W5_MAGIC) return -1;
    if (!(m->slot_valid_mask & (1u << slot))) return -1;

    unsigned width = w->width, height = w->height;
    unsigned coded_h = (height + 15u) & ~15u;
    unsigned luma_pad_h = (height + 31u) & ~31u;
    size_t slot_span = (size_t)width * luma_pad_h + (size_t)width * (coded_h / 2u);
    if (!width || !height || slot_span > w->slot_bytes) return -1;

    const uint8_t *y = (const uint8_t *)(uintptr_t)w->slot[slot];
    if ((w->flags & 2u) && (w->staging_scratch & W5_ALIAS_SLOTS_OK))
        y = (const uint8_t *)((uintptr_t)w->slot[slot] + (uintptr_t)W5_CACHE_OFFSET);
    *y_out = y;
    *uv_out = y + (size_t)width * luma_pad_h;
    if (y_stride_out) *y_stride_out = width;
    if (uv_stride_out) *uv_stride_out = width;
    return 0;
}

static uint16_t w5_clamp16(uint64_t v) { return v > 65535u ? 65535u : (uint16_t)v; }

W5_V70_SEC void live_encoder_diag_timings(const live_encoder *e,
                                          uint16_t *avg_in_us, uint16_t *avg_wait_us,
                                          uint16_t *avg_out_us, uint16_t *avg_parse_us,
                                          uint32_t *direct_cnt, uint32_t *retrig_cnt) {
    if (avg_in_us) *avg_in_us = 0;
    if (avg_wait_us) *avg_wait_us = 0;
    if (avg_out_us) *avg_out_us = 0;
    if (avg_parse_us) *avg_parse_us = 0;
    if (direct_cnt) *direct_cnt = 0;
    if (retrig_cnt) *retrig_cnt = 0;
    struct live_runtime *r = w5_runtime_of(e);
    if (!r) return;
    const live_w5_m70 *m = w5_m70_const(r);
    if (m->n_timed_v70) {
        if (avg_in_us) *avg_in_us = w5_clamp16(m->sum_in_us / m->n_timed_v70);
        if (avg_wait_us) *avg_wait_us = w5_clamp16(m->sum_wait_us / m->n_timed_v70);
        if (avg_out_us) *avg_out_us = w5_clamp16(m->sum_out_us / m->n_timed_v70);
        if (avg_parse_us) *avg_parse_us = w5_clamp16(m->sum_parse_us / m->n_timed_v70);
    }
    if (direct_cnt) *direct_cnt = m->direct_nv12_cnt;
    if (retrig_cnt) *retrig_cnt = m->async_retrig_cnt;
}

/* V71: Non-blocking -24 quiet-park query + single-shot late-INFO self-heal.
 * NEVER calls EXIT(4) or blocks. Returns 1 while parked so live_stream.c fails
 * cleanly before allocate(), 0 when healthy or self-healed. */
W5_V70_SEC int live_encoder_is_parked(const live_encoder *e) {
    struct live_runtime *r = w5_runtime_of(e);
    if (!r) return 0;
    live_w5_m70 *m = w5_m70(r);
    if (!__atomic_load_n(&m->park_err, __ATOMIC_ACQUIRE)) return 0;
    if (m->magic != W5_MAGIC || m->close_failed) return 1;
    if (!mirror_alive(m)) {
        if (!__atomic_load_n(&m->busy, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&m->release_req, __ATOMIC_ACQUIRE)) {
            w5_reset_async_state(m);
        }
        return 0;
    }
    if (!__atomic_load_n(&m->in_flight, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&m->park_err, 0u, __ATOMIC_RELEASE);
        return 0;
    }
    if (__atomic_load_n(&m->release_req, __ATOMIC_ACQUIRE)) return 1;
    uint32_t expected = 0u;
    if (!__atomic_compare_exchange_n(&m->busy, &expected, 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 1;
    int parked = 1;
    if (!mirror_alive(m)) {
        w5_reset_async_state(m);
        parked = 0;
    } else if (!__atomic_load_n(&m->in_flight, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&m->park_err, 0u, __ATOMIC_RELEASE);
        parked = 0;
    } else if (!__atomic_load_n(&m->release_req, __ATOMIC_ACQUIRE) && m->stream <= 1u) {
        volatile uint8_t *info = INFO(m->stream, m->in_flight_hw_idx % 9u);
        uint64_t addr = *(volatile uint64_t *)info;
        uint32_t sz = *(volatile uint32_t *)(info + 8);
        uint32_t pts90 = *(volatile uint32_t *)(info + 0x60);
        if (pts90 == m->in_flight_stamp && sz && addr >> 32 == m->arena >> 32) {
            *(volatile uint32_t *)(info + 8) = 0;
            struct live_w5 *w = e ? e->hw : 0;
            if (w && POOL_GATE == 0u && w->magic == W5_MAGIC) {
                w->hw_index++;
                w->frames++;
            }
            __atomic_store_n(&m->in_flight, 0u, __ATOMIC_RELEASE);
            __atomic_store_n(&m->park_err, 0u, __ATOMIC_RELEASE);
            parked = 0;
        }
    }
    __atomic_store_n(&m->busy, 0u, __ATOMIC_SEQ_CST);
    return parked;
}

/* V71: POOL_GATE-immune close-failure telemetry (low16=eos_miss, high16=fin_rc>>16). */
W5_V70_SEC uint32_t live_encoder_diag_close(const live_encoder *e) {
    struct live_runtime *r = w5_runtime_of(e);
    if (!r) return 0u;
    const live_w5_m70 *m = w5_m70_const(r);
    return m->diag_close;
}

/* Synchronous I420 encode path (FilmSim Live Link & legacy callers). Drains any prior
 * async picture cleanly, packs I420 -> NV12, submits, and harvests synchronously. */
static W5_V70_SEC int frame_body(live_encoder *e, struct live_w5 *w, live_w5_m70 *m,
                                 const uint8_t *in, uint64_t pts, uint64_t *last_idr,
                                 unsigned interval, const uint8_t **out, size_t *bytes, unsigned *idr) {
    int rc = w5_ensure_stream_locked(w, m);
    if (rc) return rc;
    if (__atomic_load_n(&m->in_flight, __ATOMIC_ACQUIRE)) {
        if (w5_drain_inflight_locked(w, m, W5_WAIT_US)) {
            if (!alive(w)) return LIVE_ENCODER_BUSY;
            __atomic_store_n(&m->park_err, 24u, __ATOMIC_RELEASE);
            return -24;
        }
    }
    rc = w5_submit_batch_locked(w, m, 0);
    if (rc) return rc;

    unsigned slot = m->next_slot_rr % W5_SLOTS;
    for (unsigned i = 0; i < W5_SLOTS; i++) {
        unsigned cand = (slot + i) % W5_SLOTS;
        if (m->last_valid_slot >= 0 && (int32_t)cand == m->last_valid_slot) continue;
        slot = cand;
        break;
    }
    m->next_slot_rr = (slot + 1u) % W5_SLOTS;
    m->slot_valid_mask &= ~(1u << slot);

    uint64_t t_in0 = now_us();
    int pack_rc = w5_pack_surface(w, slot, in, w->width, w->height);
    if (pack_rc) return pack_rc;
    uint64_t dt_in = now_us() - t_in0;
    w->t_in = dt_in; w->s_in += dt_in; m->sum_in_us += dt_in;

    int force_reset = (*last_idr == 0xFFFFFFFF00000000ULL);
    unsigned force = W5_CODEC ? (!m->submitted_frames || force_reset)
                              : (!m->submitted_frames || force_reset || !*last_idr || pts - *last_idr >= interval);
    w->hevc_force += force;
    if (TRIGGER(w->stream, 0, slot, 0, force)) return -21;
    if (force_reset) *last_idr = 0;

    m->in_flight_slot = slot;
    m->in_flight_hw_idx = w->hw_index;
    m->in_flight_stamp = w->hw_index * 3000u;
    m->in_flight_pts = pts;
    m->in_flight_t0_us = now_us();
    m->in_flight_idr_ptr = last_idr;
    m->in_flight_retrig = 0u;
    m->submitted_frames++;
    m->last_valid_slot = (int32_t)slot;
    m->slot_valid_mask |= (1u << slot);
    __atomic_store_n(&m->in_flight, 1u, __ATOMIC_RELEASE);

    rc = w5_harvest_locked(e, w, m, 1, 0, out, bytes, idr);
    return rc == 1 ? 0 : rc;
}

int live_encoder_frame_timed(live_encoder *e, const uint8_t *in, uint64_t pts, uint64_t *last_idr,
                             unsigned bitrate, unsigned interval, const uint8_t **out, size_t *bytes,
                             unsigned *idr) {
    struct live_w5 *w = e ? e->hw : 0;
    (void)bitrate;
    if (!w || w->magic != W5_MAGIC || !w->rmw || !in || !out || !bytes || !last_idr || !idr || !interval) return -1;
    live_w5_m70 *m = w5_m70(w->rmw);
    __atomic_store_n(&m->busy, 1, __ATOMIC_SEQ_CST);
    int rc;
    if (__atomic_load_n(&m->release_req, __ATOMIC_SEQ_CST)) rc = LIVE_ENCODER_BUSY;
    else {
        rc = frame_body(e, w, m, in, pts, last_idr, interval, out, bytes, idr);
        if (rc && rc != LIVE_ENCODER_BUSY && w->magic == W5_MAGIC && !alive(w)) {
            __atomic_store_n(&m->in_flight, 0u, __ATOMIC_RELEASE);
            rc = LIVE_ENCODER_BUSY;   /* preempted while this frame was in flight */
        }
    }
    if (rc == LIVE_ENCODER_BUSY) w->busy_drops++;
    __atomic_store_n(&m->busy, 0, __ATOMIC_SEQ_CST);
    return rc;
}

int live_encoder_codec(void) { return (int)W5_CODEC; }
