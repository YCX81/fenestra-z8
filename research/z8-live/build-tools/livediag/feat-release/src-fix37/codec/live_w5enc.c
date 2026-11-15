/* Live photo-mode encoder on the Z8 Wave5 (C3.11 stock HEVC/AVC service), H.264 AVC.
 * Same contract as codec/live_encoder.c (minih264): I420 + PTS in, one Annex-B AU out.
 *
 * Verified on the camera with the probe (audio-probe/W5-BRINGUP.md): bring-up as client 4
 * in photo live view, the driver-assigned stream, open/configure/register/params/headers,
 * run, then batches of four {Y,C} slots and one trigger per slot. The stock exit
 * 0x402f3084 hangs the camera while photo LV runs, so Live itself never shuts Wave5 down
 * in photo mode: the instance and its pool memory stay for the rest of the boot and are
 * reused when Live is switched off and on again.
 *
 * Sharing with the stock users (movie REC 0x40d07ba4, MOVS playback 0x40cf9810): both need
 * core A of the service (open 0x40c32fec: max two streams, core-busy flags 0xa02d30130/134,
 * and a second open must pass the same arena) and neither checks the bring-up result. So
 * a hook on the bring-up entry 0x402f2d58 calls live_encoder_preempt() for every other
 * client: Live gives Wave5 back with the stock exit in that caller's context (exit then
 * bring-up is what happens between two stock movie recordings) and rebuilds on the next
 * live-view frame once the stock user has exited again. While someone else holds Wave5,
 * frames are dropped with LIVE_ENCODER_BUSY instead of failing Live.
 *
 * All Wave5 memory (arena, buffers, four NV12 slots, this state) comes from two photo
 * pool blocks allocated once. Frames are written by the CPU (pool memory is Normal
 * Non-cacheable) and read by the VPU through the 0x8xxxxxxxx window. */
#include "live_codec.h"
#include "../runtime.h"
#include "../ui/live_ui.h"
#include "live_h264_idr.h"
#include "../capture/live_alias.h"
#include <string.h>
#define FN(a, t) ((t)(uintptr_t)UINT64_C(a))
#define RD32(a) (*(volatile uint32_t *)(uintptr_t)UINT64_C(a))
void la_native_yield(void);

#define W5_MAGIC 0x57354c31u            /* "W5L1" */
#define W5_CLIENT 4u                    /* no stock caller uses it */
#define W5_KBPS 12000u
#define W5_ARENA 0x1000000u
#define W5_SLOTS 4u
#define W5_AU_CAP (512u * 1024u)
#define W5_WAIT_US 300000u
#define W5_PREEMPT_WAIT_US 600000u   /* longest in-flight frame: 300 ms completion wait + copy */
#ifndef LIVE_ENCODER_BUSY
#define LIVE_ENCODER_BUSY (-30)
#endif

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
};

_Static_assert(sizeof(struct live_w5) == 1053600, "v32 pool header size");
_Static_assert(offsetof(struct live_w5, rmw) == 1053592, "v32 tail padding");
_Static_assert(sizeof(struct live_runtime) == 23184, "v32 runtime ABI");
_Static_assert(offsetof(struct live_runtime, w5_close_lock) == 18908, "v32 log_pad slot");

/* stock entry points (C3.11), see W5-BRINGUP.md */
#define POOL_ALLOC(n) FN(0x4104d2dc, void *(*)(uint32_t, uint32_t, uint32_t))(0x4c4155, 0x60, (n))
#define POOL_GATE RD32(0xa03f31a68)
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
#define VDI_BASE (*(volatile uint64_t *)(uintptr_t)UINT64_C(0xa02d3d980))
#define CLIENT_STREAM(c) (*(volatile uint8_t *)(uintptr_t)(UINT64_C(0xa02e98480) + (c)))
#define INFO(stream, i) ((volatile uint8_t *)(uintptr_t)(UINT64_C(0xa02d38b98) + (stream) * 0x2400u + (i) * 0x480u))

static uint64_t now_us(void) {
    uint64_t n, f;
    __asm__ volatile("mrs %0, cntpct_el0; mrs %1, cntfrq_el0" : "=r"(n), "=r"(f));
    return f ? n / f * 1000000 + n % f * 1000000 / f : 0;
}

/* pool descriptor -> {cpu, bytes}; the block is physically contiguous */
static int pool(uint32_t bytes, void **desc, uint64_t *cpu, uint32_t *actual) {
    if (POOL_GATE) return -1;
    void *d = POOL_ALLOC(bytes);
    if (!d) return -1;
    const uint64_t *r = *(const uint64_t *const *)((const uint8_t *)d + 8);
    if (!r) return -1;
    *desc = d; *cpu = r[0]; *actual = (uint32_t)r[1];
    return *actual >= bytes ? 0 : -1;
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

/* Our instance is usable while the VDI is up, client 4 maps to our stream and the
 * service still has that stream slot open. After a preempt the stock exit has set the
 * client map to 0xff and closed the slot. */
static int alive(const struct live_w5 *w) {
    return w->magic == W5_MAGIC && VDI_BASE == VDI_UP && CLIENT_STREAM(W5_CLIENT) == w->stream &&
           w->stream <= 1 && SLOT_USED(w->stream);
}

#define W5_STOP FN(0x40c34078, uint32_t (*)(uint32_t))     /* EOS request, handled at the next picture */
#define W5_FINISH FN(0x40c340e8, uint32_t (*)(uint32_t))   /* frees the instance (worker 0x400/0x800) */
#define EOS_DONE(s) (*(volatile uint32_t *)(uintptr_t)(UINT64_C(0xa02d31070) + (s) * 4u))
#define WSTATE(s) (*(volatile uint32_t *)(uintptr_t)(UINT64_C(0xa02d3274c) + (s) * 4u))   /* v29: 4 run, 5 EOS, 0 free */
/* v27: stock end-of-clip order, then the stock exit (verified on the camera 11-08). No frame in flight. */
static int w5_close_body(struct live_w5 *w) {
    uint32_t s = w->stream;
    volatile uint32_t *inst = (volatile uint32_t *)(uintptr_t)*(volatile uint64_t *)(uintptr_t)(UINT64_C(0xa02d32758) + s * 8u);
    if (WSTATE(s) != 4u || !inst || inst[0] != 1u || inst[3] != 3u) return 1;
    unsigned k = w->frames % W5_SLOTS;
    if (!k) {
        uint8_t *list = w->scratch + 0x800;
        for (unsigned i = 0; i < W5_SLOTS; i++) {
            put64(list + i * 16, w->slot[i]);
            put64(list + i * 16 + 8, w->slot[i] + (uint64_t)w->width * ((w->height + 31) & ~31u));
        }
        if (SUBMIT(s, 0, list)) return 2;
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
    w->fin_rc |= (WSTATE(s) & 0xffu) << 16;
    if (WSTATE(s) != 0u || inst[0] != 0u) return 8;
    EXIT(W5_CLIENT);
    t = now_us();
    while (VDI_BASE) { if (now_us() - t > 1000000u) return 9; DELAY(1); }
    return 0;
}
static void w5_close(struct live_w5 *w) {
    uint32_t free_ = 0; uint64_t t = now_us();
    while (!__atomic_compare_exchange_n(&w->rmw->w5_close_lock, &free_, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        free_ = 0;
        if (now_us() - t > 4000000u) { w->close_failed = 1; w->eos_miss++; w->fin_rc = (w->fin_rc & 0xffffffu) | 11u << 24; return; }
        DELAY(1);
    }
    if (alive(w) && !w->close_failed) {
        int st = w5_close_body(w);
        if (st) { w->close_failed = 1; w->eos_miss++; w->fin_rc = (w->fin_rc & 0xffffffu) | (uint32_t)st << 24; } else w->eos_ok++;
        w->closes++; w->last_close_us = (uint32_t)(now_us() - t);
        if (!st) { w->frames = 0; w->batch_open = 0; }
    }
    __atomic_store_n(&w->rmw->w5_close_lock, 0, __ATOMIC_RELEASE);
}
/* v27: worker task, Live not encoding (not in photo mode, no clip in flight): give Wave5 back now */
int live_encoder_release(live_encoder *e) {
    struct live_w5 *w = e ? e->hw : 0;
    if (!w || w->magic != W5_MAGIC || !alive(w)) return 0;
    if (__atomic_load_n(&w->release_req, __ATOMIC_SEQ_CST)) return 0;   /* a preempt is doing it */
    __atomic_store_n(&w->busy, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&w->release_req, __ATOMIC_SEQ_CST)) { __atomic_store_n(&w->busy, 0, __ATOMIC_SEQ_CST); return 0; }
    w5_close(w);
    __atomic_store_n(&w->busy, 0, __ATOMIC_SEQ_CST);
    return alive(w) ? -1 : (VDI_BASE ? 2 : 1);
}

static int open_stream(struct live_w5 *w) {
    uint8_t *s = w->scratch;
    uint32_t stream;
    /* VDI up without our open slot: a stock user (movie/playback) owns Wave5 now. A
     * bring-up here would be refused by the service; wait until that user has exited. */
    if (w->close_failed) return LIVE_ENCODER_BUSY;   /* v31 */
    if (VDI_BASE) return LIVE_ENCODER_BUSY;
    if (RD32(0xa03bd062c) != 0) return LIVE_ENCODER_BUSY;   /* v30: bring-up only in photo mode */
    if (BRINGUP(W5_CLIENT, 8000u /* v25: service bring-up rate as up to v14 */, w->arena, W5_ARENA) != 1) return -3;
    stream = CLIENT_STREAM(W5_CLIENT);
    if (stream > 1) return -4;
    w->stream = stream;
    if (OPEN(stream, NOOP_CB)) return -5;
    uint32_t cfg[8] = {0, w->width, (w->height + 31) & ~31u, W5_KBPS, 1, 0, 8, 8};
    memcpy(s, cfg, sizeof cfg);
    if (CONFIGURE(s, s + 0x100)) return -6;
    table(w, s + 0x200);
    if (REGISTER(stream, s + 0x200)) return -7;
    /* movie mode-table entry layout (0x24 B): codec,?,depth,w,h,pad | fps num,den,gop,structure,bps | mode */
    uint8_t *m = s + 0x300;
    memset(m, 0, 0x24);
    uint16_t h16[6] = {0, 0, 8, (uint16_t)w->width, (uint16_t)w->height, 0};
    uint32_t w32[5] = {30000, 1001, 15, 1, W5_KBPS * 1000u};
    memcpy(m, h16, 12); memcpy(m + 12, w32, 20); m[0x20] = 1;
    memset(s + 0x400, 0, 0x220);
    PARAMS(m, 0, w->region[R_USER], s + 0x400);
    if (SET_PARAMS(stream, s + 0x400, 0)) return -8;
    uint64_t hdr = 0; uint32_t hn = 0, lens[4] = {0};
    if (HEADERS(stream, &hdr, &hn, lens) || hdr >> 32 != w->arena >> 32) return -9;
    /* AUD, SPS, PPS[, SEI]: each length excludes its 4-byte start code */
    const uint8_t *h = (const uint8_t *)(uintptr_t)hdr, *sps = 0, *pps = 0;
    uint32_t sn = 0, pn = 0, at = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (!lens[i]) continue;
        const uint8_t *nal = h + at + 4;
        if ((nal[0] & 31) == 7) { sps = nal; sn = lens[i]; }
        if ((nal[0] & 31) == 8) { pps = nal; pn = lens[i]; }
        at += lens[i] + 4;
    }
    if (!sps || !pps || live_h264_idr_init(&w->idr, sps, sn, pps, pn)) return -10;
    uint32_t run[4] = {1, 1, 15, 15};
    memcpy(s + 0x700, run, sizeof run);
    if (RUN(stream, s + 0x700)) return -11;
    w->hw_index = 0; w->batch_open = 0; w->frames = 0;
    return 0;
}

void live_encoder_set_flags(live_encoder *e, unsigned f) { if (e && e->hw && e->hw->magic == W5_MAGIC) e->hw->flags = f; }
int live_encoder_sizes(unsigned w, unsigned h, size_t *persist, size_t *scratch) {
    if (!persist || !scratch || !w || !h || (w & 1) || (h & 1) || w > 1920 || h > 1088) return -1;
    *persist = 16; *scratch = 2 * (size_t)W5_AU_CAP + 64;   /* AU/wrap staging in Live's (cacheable) allocation */
    return 0;
}

int live_encoder_init(live_encoder *e, void *p, void *s, unsigned width, unsigned height) {
    if (!e || !p || !s) return -1;
    struct live_w5 *w = e->hw;
    e->persist = p; e->scratch = s; e->width = width; e->height = height; e->frames = 0; e->last_pts = 0;
    if (w) {
        /* Live switched off and on: reuse the instance (geometry fixed per boot), rebuilding
         * it on the same memory if a movie recording powered Wave5 down meanwhile. A failed
         * first bring-up is not retried: its pool blocks stay pinned. */
        if (w->magic != W5_MAGIC) return -1;
        __atomic_store_n(&w->busy, 1, __ATOMIC_SEQ_CST);   /* v31: preempt waits for this path */
        if (w->width != width || w->height != height) {
            /* v17: another live-view channel: reopen the stream at the new size if the slots fit it */
            if ((size_t)width * ((height + 31) & ~31u) * 3 / 2 > w->slot_bytes) { __atomic_store_n(&w->busy, 0, __ATOMIC_SEQ_CST); return -1; }
            if (alive(w)) w5_close(w);
            w->width = width; w->height = height;
        }
        if (!alive(w)) {
            int rc = open_stream(w);
            if (rc && rc != LIVE_ENCODER_BUSY) { w->magic = 0; __atomic_store_n(&w->busy, 0, __ATOMIC_SEQ_CST); return -1; }   /* busy: retried per frame */
        }
        w->frames = 0; w->batch_open = 0;
        __atomic_store_n(&w->busy, 0, __ATOMIC_SEQ_CST);
        return 0;
    }
    /* Resolve the single cached owner before allocating/publishing pool state.
     * Pool memory cannot support exclusive RMW on this hardware. */
    struct live_runtime *r = live_ui_engine_get();
    if (!r || (uintptr_t)r < UINT64_C(0xa00000000) ||
        (uintptr_t)r > UINT64_C(0xa40000000) - sizeof(*r) || ((uintptr_t)r & 7u) ||
        r->magic != LIVE_RUNTIME_MAGIC || r->size != sizeof(*r) ||
        e != &r->stream.encoder) return -1;
    void *ad, *bd; uint64_t a, b; uint32_t an, bn;
    /* v17: slots for the larger of this size and 1280x852 (channel 2 at 1:1), so the size may change later */
    size_t sw = width > 1280u ? width : 1280u, sh = height > 852u ? height : 852u;
    uint32_t slot = (uint32_t)((sw * ((sh + 31) & ~(size_t)31) * 3 / 2 + 0xfff) & ~(size_t)0xfff);
    uint32_t need = (uint32_t)((sizeof(struct live_w5) + 0xfff) & ~(size_t)0xfff) + W5_SLOTS * slot;
    for (unsigned i = 0; i < R_COUNT; i++) need += region_size[i];
    if (pool(W5_ARENA + 0x1000, &ad, &a, &an) || pool(need + 0x1000, &bd, &b, &bn)) return -1;
    a = (a + 0xfff) & ~UINT64_C(0xfff); b = (b + 0xfff) & ~UINT64_C(0xfff);
    if ((a & UINT64_C(0xfffffff800000fff)) != UINT64_C(0x800000000) || a >> 32 != b >> 32) return -1;
    w = (struct live_w5 *)(uintptr_t)b;
    memset(w, 0, sizeof(*w));
    w->rmw = r; /* bound before e->hw publication; never reset the shared lock */
    w->arena = a; w->arena_desc = ad; w->block_desc = bd; w->width = width; w->height = height; w->slot_bytes = slot;
    uint64_t at = b + ((sizeof(struct live_w5) + 0xfff) & ~(size_t)0xfff);
    for (unsigned i = 0; i < R_COUNT; i++) { w->region[i] = at; at += region_size[i]; }
    for (unsigned i = 0; i < W5_SLOTS; i++) { w->slot[i] = at; at += slot; }
    e->hw = w;
    int rc = open_stream(w);
    if (rc == LIVE_ENCODER_BUSY) { w->magic = W5_MAGIC; return 0; }   /* opened on a later frame */
    if (rc) return rc;
    w->magic = W5_MAGIC;
    return 0;
}

/* Bring-up hook (0x402f2d58 entry), any task: another client is about to bring Wave5 up.
 * Let a frame in flight finish, then release with the stock exit (client map -> 0xff,
 * slot closed, VDI deinit, resets/clocks/power down); the caller's bring-up follows. */
void live_encoder_preempt(live_encoder *e, uint32_t client) {
    struct live_w5 *w = e ? e->hw : 0;
    if (client == W5_CLIENT || !w || w->magic != W5_MAGIC || !alive(w)) return;
    __atomic_store_n(&w->release_req, 1, __ATOMIC_SEQ_CST);
    uint64_t start = now_us();
    while (__atomic_load_n(&w->busy, __ATOMIC_SEQ_CST) && now_us() - start < 4000000u) DELAY(1);
    if (__atomic_load_n(&w->busy, __ATOMIC_SEQ_CST)) { w->close_failed = 1; w->eos_miss++; w->fin_rc = (w->fin_rc & 0xffffffu) | 10u << 24; }   /* v31: never close under a running call */
    else if (alive(w)) w5_close(w);   /* v27: the worker may have released it meanwhile */
    w->frames = 0; w->batch_open = 0; w->preempts++; w->preempt_us = now_us();
    __atomic_store_n(&w->release_req, 0, __ATOMIC_SEQ_CST);
}

int live_encoder_frame(live_encoder *e, const uint8_t *in, const uint8_t **out, size_t *bytes) {
    uint64_t last = 0; unsigned idr;
    return live_encoder_frame_timed(e, in, e->last_pts + 33367, &last, W5_KBPS * 1000u, 500000u, out, bytes, &idr);
}

static int frame_body(live_encoder *e, struct live_w5 *w, const uint8_t *in, uint64_t pts, uint64_t *last_idr,
                      unsigned interval, const uint8_t **out, size_t *bytes, unsigned *idr);

int live_encoder_frame_timed(live_encoder *e, const uint8_t *in, uint64_t pts, uint64_t *last_idr,
                             unsigned bitrate, unsigned interval, const uint8_t **out, size_t *bytes,
                             unsigned *idr) {
    struct live_w5 *w = e ? e->hw : 0;
    (void)bitrate;
    if (!w || w->magic != W5_MAGIC || !in || !out || !bytes || !last_idr || !idr || !interval) return -1;
    __atomic_store_n(&w->busy, 1, __ATOMIC_SEQ_CST);
    int rc;
    if (__atomic_load_n(&w->release_req, __ATOMIC_SEQ_CST)) rc = LIVE_ENCODER_BUSY;
    else {
        rc = frame_body(e, w, in, pts, last_idr, interval, out, bytes, idr);
        if (rc && rc != LIVE_ENCODER_BUSY && w->magic == W5_MAGIC && !alive(w))
            rc = LIVE_ENCODER_BUSY;   /* preempted while this frame was in flight */
    }
    if (rc == LIVE_ENCODER_BUSY) w->busy_drops++;
    __atomic_store_n(&w->busy, 0, __ATOMIC_SEQ_CST);
    return rc;
}

static int frame_body(live_encoder *e, struct live_w5 *w, const uint8_t *in, uint64_t pts, uint64_t *last_idr,
                      unsigned interval, const uint8_t **out, size_t *bytes, unsigned *idr) {
    if (w->close_failed) return LIVE_ENCODER_BUSY;   /* v30 */
    if (!alive(w)) {
        /* v22: a movie/playback client just took Wave5: do not take it back during its bring-up */
        if (w->preempt_us && now_us() - w->preempt_us < 3000000u) { w->reopen_blocked++; return LIVE_ENCODER_BUSY; }
        int rc = open_stream(w);
        if (rc == LIVE_ENCODER_BUSY) return rc;
        if (rc) { w->magic = 0; return -25; }
        w->frames = 0;
    }
    unsigned slot = w->frames % W5_SLOTS, width = w->width, height = w->height;
    if (!slot) {
        uint8_t *list = w->scratch + 0x800;
        for (unsigned i = 0; i < W5_SLOTS; i++) {
            put64(list + i * 16, w->slot[i]);
            put64(list + i * 16 + 8, w->slot[i] + (uint64_t)width * ((height + 31) & ~31u));
        }
        if (SUBMIT(w->stream, 0, list)) return -20;
    }
    /* I420 -> NV12 into the VPU slot (Y rows unchanged, chroma interleaved) */
    uint8_t *stage_au = w->au, *stage_wrap = w->wrap;
    if (e->scratch) { stage_au = (uint8_t *)(((uintptr_t)e->scratch + 63) & ~(uintptr_t)63); stage_wrap = stage_au + W5_AU_CAP; w->staging_scratch = 1; }
    uint64_t t_in0 = now_us();
    size_t slot_span = (size_t)width * ((height + 31) & ~31u) + (size_t)width * height / 2;
    uint8_t *y = (uint8_t *)(uintptr_t)w->slot[slot]; int slot_cached = 0;
    if (w->flags & 2u) {
        uintptr_t a = la_alias((uintptr_t)y, slot_span);
        if (a) { y = (uint8_t *)a; slot_cached = 1; w->alias_slot_ok++; } else w->alias_slot_fail++;
    }
    uint8_t *c = y + (size_t)width * ((height + 31) & ~31u);
    const uint8_t *u = in + (size_t)width * height, *v = u + (size_t)width * height / 4;
    w5_burst(y, in, (size_t)width * height);
    {
        uint8_t row[4096] __attribute__((aligned(16)));
        for (unsigned r = 0; r < height / 2; r++) {
            for (unsigned x = 0; x < width / 2; x++) {
                row[2 * x] = u[(size_t)r * (width / 2) + x];
                row[2 * x + 1] = v[(size_t)r * (width / 2) + x];
            }
            w5_burst(c + (size_t)r * width, row, width);
        }
    }
    if (slot_cached) la_dc_cvac((uintptr_t)y, slot_span);   /* the VPU reads PA memory */
    uint64_t t_in_end = now_us(); w->t_in = t_in_end - t_in0; w->s_in += w->t_in;
    unsigned force = !w->frames || pts - *last_idr >= interval;
    if (TRIGGER(w->stream, 0, slot, 0, force)) return -21;
    /* completion: picture n of the stream lands in frame-info slot n % 9 with a 90 kHz
     * stamp of n * 3000 (+0x40 is not a picture counter: it restarts at intra-period I) */
    volatile uint8_t *info = INFO(w->stream, w->hw_index % 9);
    uint32_t stamp = w->hw_index * 3000u;
    uint64_t start = now_us();
    for (;;) {
        uint64_t addr; uint32_t size, pts90;
        addr = *(volatile uint64_t *)info; size = *(volatile uint32_t *)(info + 8);
        pts90 = *(volatile uint32_t *)(info + 0x60);
        if (pts90 == stamp && size && addr >> 32 == w->arena >> 32) {
            /* A picture that reaches the end of bs0 or bs1 is split: the VPU leaves a few
             * bytes unused there and continues at the start of the other buffer. The info
             * entry then sets +0x0c and gives both parts: {+0x10 addr, +0x18 size} and
             * {+0x20 addr, +0x28 size} (seen on hardware at both boundaries). */
            uint64_t t_done = now_us(); w->t_wait = t_done - start; w->s_wait += w->t_wait;
            uint64_t bs0 = w->region[R_BS0], end = w->region[R_BS1] + region_size[R_BS1];
            if (addr < bs0 || addr >= end || size > W5_AU_CAP - 1024) return -22;
            const uint8_t *src = (const uint8_t *)(uintptr_t)addr;
            if (*(volatile uint32_t *)(info + 0x0c)) {
                uint64_t a1 = *(volatile uint64_t *)(info + 0x10), a2 = *(volatile uint64_t *)(info + 0x20);
                uint32_t n1 = *(volatile uint32_t *)(info + 0x18), n2 = *(volatile uint32_t *)(info + 0x28);
                if (a1 < bs0 || a1 + n1 > end || a2 < bs0 || a2 + n2 > end || n1 + n2 != size) return -26;
                uintptr_t c1 = (w->flags & 4u) ? la_alias(a1, n1) : 0, c2 = (w->flags & 4u) ? la_alias(a2, n2) : 0;
                if (c1 && c2) { la_dc_ivac(c1, n1); la_dc_ivac(c2, n2); a1 = c1; a2 = c2; w->alias_bs_ok++; }
                else if (w->flags & 4u) w->alias_bs_fail++;
                w5_burst(stage_wrap, (const uint8_t *)(uintptr_t)a1, n1);
                w5_burst(stage_wrap + n1, (const uint8_t *)(uintptr_t)a2, n2);
                src = stage_wrap;
            } else if (addr + size > end) return -22;
            else if (size + 16 <= W5_AU_CAP) {
                /* same 16-byte phase as the source so the copy runs in bursts */
                uintptr_t ca = (w->flags & 4u) ? la_alias(addr, size) : 0;
                if (ca) { la_dc_ivac(ca, size); src = (const uint8_t *)ca; w->alias_bs_ok++; }
                else if (w->flags & 4u) w->alias_bs_fail++;
                uint8_t *dst = stage_wrap + (addr & 15);
                w5_burst(dst, src, size);
                src = dst;
            }
            size_t n;
            uint64_t t_parse0 = now_us(); w->t_out = t_parse0 - t_done; w->s_out += w->t_out;
            if (live_h264_idr_au(&w->idr, src, size, stage_au, W5_AU_CAP, &n, idr)) return -23;
            w->t_parse = now_us() - t_parse0; w->s_parse += w->t_parse; w->n_timed++;
            *(volatile uint32_t *)(info + 8) = 0;   /* consumed: a stale entry never matches again */
            w->hw_index++; w->frames++; e->frames++; e->last_pts = pts;
            if (*idr) *last_idr = pts;
            *out = stage_au; *bytes = n;
            return 0;
        }
        if (now_us() - start > W5_WAIT_US) return -24;
        la_native_yield();
    }
}
