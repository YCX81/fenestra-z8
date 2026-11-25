/* V71.3 SMP-safe Fenestra card-slot routing + hardened SD DMA cache maintenance.
 *
 * Root causes fixed vs v42/cardslot.c:
 *  1. Atomic 3-state one-time init on s->magic (0/garbage -> INIT_MAGIC -> MAGIC)
 *     so concurrent CPU0 (live_ui) / CPU1 (loader) / CPU2 (native_sync) calls can
 *     never race-zero struct cs mid-probe or corrupt s->first / s->drive.
 *  2. Bounded try-spinlock (s->lock at +0x48, outside the 72-byte ABI prefix)
 *     serializing drive() probes across cores without risking same-core priority
 *     deadlock.
 *  3. Probe throttle tightened from 100 ms to 20 ms (and bypassed whenever card
 *     presence registers change) so CardMountComplete (event 24) on a second card
 *     is not dropped as spurious REFUSED / FX_STARTUP_IO_ERROR ("Card error").
 *  4. Settle timeout aligned from 20 s to 8 s so cardslot always latches before
 *     bootguard's ~10 s (1000-tick) inspect budget expires.
 *  5. Atomic counter increments for s->rewrites and s->refused.
 *  6. Hardened hook_read() D-cache maintenance:
 *     - Post-DMA uses dc ivac (NEVER dc civac) on strictly-owned full cache lines
 *       so dirty/speculative CPU cache lines can never write back over fresh DMA.
 *     - Unaligned head/tail portions (< line bytes) are bounced through an
 *       exclusive 64B-aligned stack cache line and byte-copied into the caller
 *       buffer, so adjacent CPU variables sharing the head/tail cache line (such
 *       as *done or neighboring struct fields) are never corrupted or lost.
 */
#include <stdint.h>

#define MAGIC      0x314c5343u       /* 'CSL1' */
#define INIT_MAGIC 0x304c5343u       /* 'CSL0' */
#define BUFN 160u
struct cs {
    uint32_t magic, drive, reason, probes;
    uint64_t first, last;
    uint32_t code_a, code_b, root_a, root_b, reg_a, reg_b, rewrites, refused;
    uint32_t class_a, class_b;
    /* Appended tail (inside boot-cleared 0xa1171b000..0xa1172b000), ABI-compatible: */
    uint32_t lock, reserved;
};
#define S ((volatile struct cs *)(uintptr_t)UINT64_C(0xa1171b000))
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define CLOSEDIR ((uint32_t (*)(int32_t))(uintptr_t)UINT64_C(0x402ddd6c))

extern int32_t o_open(const char *, uint32_t, uint32_t);
extern uint32_t o_remove(const char *);
extern uint32_t o_rename(const char *, const char *);
extern uint32_t o_opendir(const char *, int32_t *);
extern uint32_t o_mkdir(const char *);
extern uint32_t o_read(int32_t, void *, uint32_t, uint32_t *);
extern uint32_t o_write(int32_t, const void *, uint32_t, uint32_t *, void *);

enum { HAS = 1, MOUNTED_NO = 2, ABSENT = 3, PENDING = 4, UNCERTAIN = 5 };

static uint64_t ticks(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v)); return v; }
static uint64_t freq(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v ? v : 24000000u; }

static int dir_ok(const char *p, volatile uint32_t *code) {
    int32_t fd = -1;
    uint32_t r = o_opendir(p, &fd);
    *code = r;
    if (!r && fd >= 0) { (void)CLOSEDIR(fd); return 1; }
    return fd >= 0 ? -1 : 0;
}
static unsigned classify(const char *fen, const char *root, uint32_t present,
                         volatile uint32_t *code, volatile uint32_t *root_code) {
    int k = dir_ok(fen, code);
    if (k > 0) return HAS;
    if (k < 0) return UNCERTAIN;
    if (!present) return ABSENT;
    k = dir_ok(root, root_code);
    if (k > 0) return MOUNTED_NO;
    return k < 0 ? UNCERTAIN : PENDING;
}
static void cs_init_once(volatile struct cs *s) {
    for (unsigned spin = 0; spin < 4096u; ++spin) {
        uint32_t m = __atomic_load_n(&s->magic, __ATOMIC_ACQUIRE);
        if (m == MAGIC) return;
        if (m == INIT_MAGIC) {
            __asm__ volatile("yield" ::: "memory");
            continue;
        }
        uint32_t exp = m;
        if (__atomic_compare_exchange_n(&s->magic, &exp, INIT_MAGIC, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            for (unsigned i = 1u; i < sizeof(struct cs) / 4u; ++i)
                ((volatile uint32_t *)s)[i] = 0u;
            __atomic_store_n(&s->magic, MAGIC, __ATOMIC_RELEASE);
            return;
        }
    }
}
__attribute__((noinline)) static uint32_t drive(void) {
    volatile struct cs *s = S;
    if (__atomic_load_n(&s->magic, __ATOMIC_ACQUIRE) != MAGIC) {
        cs_init_once(s);
        if (__atomic_load_n(&s->magic, __ATOMIC_ACQUIRE) != MAGIC) return 0;
    }
    uint32_t d = __atomic_load_n(&s->drive, __ATOMIC_ACQUIRE);
    if (d) return d;

    unsigned got = 0;
    for (unsigned spin = 0; spin < 4096u; ++spin) {
        if (!__atomic_exchange_n(&s->lock, 1u, __ATOMIC_ACQUIRE)) { got = 1u; break; }
        d = __atomic_load_n(&s->drive, __ATOMIC_ACQUIRE);
        if (d) return d;
        __asm__ volatile("yield" ::: "memory");
    }
    if (!got) return __atomic_load_n(&s->drive, __ATOMIC_ACQUIRE);

    d = __atomic_load_n(&s->drive, __ATOMIC_ACQUIRE);
    if (d) { __atomic_store_n(&s->lock, 0u, __ATOMIC_RELEASE); return d; }

    uint64_t now = ticks(), f = freq();
    uint32_t ra = REG(0x17a00044u), rb = REG(0x17a0000cu);
    if (s->probes && ra == s->reg_a && rb == s->reg_b && now - s->last < f / 50u) {
        __atomic_store_n(&s->lock, 0u, __ATOMIC_RELEASE);
        return 0;
    }
    if (!s->probes) s->first = now;
    s->last = now; s->probes++;
    int timed = now - s->first >= 8u * f;
    s->reg_a = ra; s->reg_b = rb;
    unsigned a = classify("A:\\FENESTRA", "A:\\", (ra >> 1) & 1u, &s->code_a, &s->root_a), b = 0;
    s->class_a = a;
    uint32_t pick = 0;
    if (a == HAS || a == UNCERTAIN) pick = 1;
    else if (a == PENDING && !timed) pick = 0;
    else {
        b = classify("B:\\FENESTRA", "B:\\", !(rb & 1u), &s->code_b, &s->root_b);
        s->class_b = b;
        if (b == HAS) pick = 2;
        else if (b == PENDING && !timed) pick = 0;
        else pick = 1;
    }
    if (pick) {
        s->reason = a | b << 4 | (uint32_t)timed << 8;
        __atomic_store_n(&s->drive, pick, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&s->lock, 0u, __ATOMIC_RELEASE);
    return pick;
}
__attribute__((noinline)) static int fenestra(const char *p) {
    static const char tag[] = "A:\\FENESTRA";
    if (!p) return 0;
    for (unsigned i = 0; i < 11u; ++i)
        if (p[i] != tag[i]) return 0;
    return p[11] == 0 || p[11] == '\\';
}
static char *native(char *p) {
    uintptr_t a = (uintptr_t)p;
    return (char *)(a >= UINT64_C(0xc00000000) && a < UINT64_C(0x1000000000) ? a - UINT64_C(0x400000000) : a);
}
static const char *route(const char *p, char *buf) {
    uint32_t d = drive();
    if (d == 1) return p;
    if (d == 2) {
        unsigned i = 0;
        for (; i < BUFN - 1u && p[i]; ++i) buf[i] = p[i];
        if (p[i]) { __atomic_fetch_add(&S->refused, 1u, __ATOMIC_RELAXED); return 0; }
        buf[i] = 0; buf[0] = 'B';
        __atomic_fetch_add(&S->rewrites, 1u, __ATOMIC_RELAXED);
        return native(buf);
    }
    __atomic_fetch_add(&S->refused, 1u, __ATOMIC_RELAXED);
    return 0;
}
#define REFUSED 0xff01u
__attribute__((noinline)) static int32_t slow_open(const char *p, uint32_t f, uint32_t a) {
    char b[BUFN]; const char *q = route(p, b);
    return q ? o_open(q, f, a) : (int32_t)0x8000ff01u;
}
__attribute__((noinline)) static uint32_t slow_remove(const char *p) {
    char b[BUFN]; const char *q = route(p, b);
    return q ? o_remove(q) : REFUSED;
}
__attribute__((noinline)) static uint32_t slow_rename(const char *p, const char *n) {
    char b1[BUFN], b2[BUFN];
    const char *q = fenestra(p) ? route(p, b1) : p, *r = fenestra(n) ? route(n, b2) : n;
    return q && r ? o_rename(q, r) : REFUSED;
}
__attribute__((noinline)) static uint32_t slow_opendir(const char *p, int32_t *fd) {
    char b[BUFN]; const char *q = route(p, b);
    if (q) return o_opendir(q, fd);
    if (fd) *fd = -1;
    return REFUSED;
}
__attribute__((noinline)) static uint32_t slow_mkdir(const char *p) {
    char b[BUFN]; const char *q = route(p, b);
    return q ? o_mkdir(q) : REFUSED;
}
int32_t hook_open(const char *p, uint32_t f, uint32_t a) { return fenestra(p) ? slow_open(p, f, a) : o_open(p, f, a); }
uint32_t hook_remove(const char *p) { return fenestra(p) ? slow_remove(p) : o_remove(p); }
uint32_t hook_rename(const char *p, const char *n) { return fenestra(p) || fenestra(n) ? slow_rename(p, n) : o_rename(p, n); }
uint32_t hook_opendir(const char *p, int32_t *fd) { return fenestra(p) ? slow_opendir(p, fd) : o_opendir(p, fd); }
uint32_t hook_mkdir(const char *p) { return fenestra(p) ? slow_mkdir(p) : o_mkdir(p); }

static inline uint64_t dcache_line(void) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t l = 4u << ((ctr >> 16) & 15u);
    return (l >= 16u && l <= 64u) ? l : 64u;
}
__attribute__((noinline)) static void dc_civac(uintptr_t a, uint32_t n, uint64_t line) {
    uintptr_t at = a & ~(line - 1u), end = a + n;
    for (; at < end; at += line) __asm__ volatile("dc civac, %0" : : "r"(at) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}
__attribute__((noinline)) static void dc_ivac(uintptr_t a, uint32_t n, uint64_t line) {
    uintptr_t at = a & ~(line - 1u), end = a + n;
    for (; at < end; at += line) __asm__ volatile("dc ivac, %0" : : "r"(at) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}
__attribute__((noinline)) static void dc_cvac(uintptr_t a, uint32_t n, uint64_t line) {
    uintptr_t at = a & ~(line - 1u), end = a + n;
    for (; at < end; at += line) __asm__ volatile("dc cvac, %0" : : "r"(at) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}
static inline int ram(const void *p, uint32_t n) {
    uint64_t a = (uint64_t)(uintptr_t)p;
    return n && n <= 0x4000000u && a >= UINT64_C(0xa00000000) && a < UINT64_C(0x1000000000) && a + n <= UINT64_C(0x1000000000);
}
__attribute__((noinline)) static uint32_t read_bounce(
    int32_t fd, uint8_t *dst, uint32_t chunk, uint8_t *bbuf, uint64_t line, uint32_t *got_out) {
    uint32_t got = 0;
    dc_civac((uintptr_t)bbuf, (uint32_t)line, line);
    uint32_t r = o_read(fd, bbuf, chunk, &got);
    dc_ivac((uintptr_t)bbuf, (uint32_t)line, line);
    uint32_t k = got < chunk ? got : chunk;
    volatile const uint8_t *src = bbuf;
    for (uint32_t i = 0; i < k; ++i) dst[i] = src[i];
    *got_out = k;
    return r;
}
uint32_t hook_read(int32_t fd, void *buf, uint32_t n, uint32_t *done) {
    if (!ram(buf, n)) return o_read(fd, buf, n, done);
    uint64_t line = dcache_line(), mask = line - 1u;
    uintptr_t start = (uintptr_t)buf, end = start + n;
    if (((start | end) & mask) == 0u) {
        uintptr_t da = (uintptr_t)done;
        int alias = done && da + sizeof(*done) > start && da < end;
        uint32_t local_done = 0;
        dc_civac(start, n, line);
        uint32_t r = o_read(fd, buf, n, alias ? &local_done : done);
        dc_ivac(start, n, line);
        if (alias) *done = local_done;
        return r;
    }
    uint8_t raw[128];
    uint8_t *bbuf = (uint8_t *)(((uintptr_t)raw + 63u) & ~(uintptr_t)63u);
    uint8_t *p = (uint8_t *)buf;
    uint32_t total = 0, r = 0, got = 0;
    if (n <= line) {
        r = read_bounce(fd, p, n, bbuf, line, &got);
        if (done) *done = got;
        return r;
    }
    uint32_t head = (uint32_t)((line - (start & mask)) & mask);
    if (head) {
        r = read_bounce(fd, p, head, bbuf, line, &got);
        total += got;
        if (r || got < head) {
            if (done) *done = total;
            return r;
        }
        p += head;
        n -= head;
    }
    uint32_t mid = n & ~(uint32_t)mask;
    if (mid) {
        uintptr_t ma = (uintptr_t)p;
        got = 0;
        dc_civac(ma, mid, line);
        r = o_read(fd, p, mid, &got);
        dc_ivac(ma, mid, line);
        if (got > mid) got = mid;
        total += got;
        if (r || got < mid) {
            if (done) *done = total;
            return r;
        }
        p += mid;
        n -= mid;
    }
    if (n) {
        r = read_bounce(fd, p, n, bbuf, line, &got);
        total += got;
    }
    if (done) *done = total;
    return r;
}
uint32_t hook_write(int32_t fd, const void *buf, uint32_t n, uint32_t *done, void *x) {
    if (ram(buf, n)) dc_cvac((uintptr_t)buf, n, dcache_line());
    return o_write(fd, buf, n, done, x);
}
