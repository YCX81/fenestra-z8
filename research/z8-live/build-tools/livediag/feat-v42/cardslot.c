/* Fenestra card-slot fallback for Z8 C3.11 (cave code, linked at 0x401fe300).
 *
 * The compiled Fenestra core, Live and the WLM runtime spell every FENESTRA path as
 * "A:\FENESTRA..." (A = CFexpress/XQD slot). The five stock path-taking file wrappers
 * (open, remove, rename, opendir, mkdir) are entered through these hooks. A path whose
 * prefix is exactly "A:\FENESTRA" followed by '\' or NUL is sent to the volume chosen
 * once per boot; every other path takes the original wrapper untouched.
 *
 * Choice (latched in RAM, boot-cleared C64 reservation tail 0xa1171b000):
 *   A has \FENESTRA                                   -> A
 *   A mounted without it, or A slot empty, B has it   -> B
 *   neither has it (both settled)                     -> A (stock behaviour)
 *   a present card that is not mounted yet            -> undecided: the FENESTRA call
 *       fails with an I/O error (never "absent"), the core retries on its inspect
 *       budget and on CardMountComplete; after 20 s the best settled answer is taken.
 * Card presence: CFexpress 0x17a00044 bit1, SD 0x17a0000c bit0 inverted (both polled
 * by the stock card-detect code). An opendir that fails but publishes a handle has
 * uncertain ownership: never close it, never fail over (latch A). */
#include <stdint.h>

#define MAGIC 0x314c5343u            /* 'CSL1' */
#define BUFN 160u
struct cs {
    uint32_t magic, drive, reason, probes;
    uint64_t first, last;
    uint32_t code_a, code_b, root_a, root_b, reg_a, reg_b, rewrites, refused;
    uint32_t class_a, class_b;
};
#define S ((volatile struct cs *)(uintptr_t)UINT64_C(0xa1171b000))
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define CLOSEDIR ((uint32_t (*)(int32_t))(uintptr_t)UINT64_C(0x402ddd6c))

/* Trampolines (relocated first instruction + branch back), see cardslot_patch.py. */
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
__attribute__((noinline)) static uint32_t drive(void) {
    volatile struct cs *s = S;
    if (s->magic != MAGIC) {
        for (unsigned i = 0; i < sizeof(struct cs) / 4; ++i) ((volatile uint32_t *)s)[i] = 0;
        s->magic = MAGIC;
    }
    uint32_t d = __atomic_load_n(&s->drive, __ATOMIC_ACQUIRE);
    if (d) return d;
    uint64_t now = ticks(), f = freq();
    if (s->probes && now - s->last < f / 10u) return 0;          /* at most 10 probes/s */
    if (!s->probes) s->first = now;
    s->last = now; s->probes++;
    int timed = now - s->first >= 20u * f;
    uint32_t ra = REG(0x17a00044u), rb = REG(0x17a0000cu);
    s->reg_a = ra; s->reg_b = rb;
    unsigned a = classify("A:\\FENESTRA", "A:\\", (ra >> 1) & 1u, &s->code_a, &s->root_a), b = 0;
    s->class_a = a;
    uint32_t pick;
    if (a == HAS || a == UNCERTAIN) pick = 1;
    else if (a == PENDING && !timed) return 0;
    else {
        b = classify("B:\\FENESTRA", "B:\\", !(rb & 1u), &s->code_b, &s->root_b);
        s->class_b = b;
        if (b == HAS) pick = 2;
        else if (b == PENDING && !timed) return 0;
        else pick = 1;
    }
    s->reason = a | b << 4 | (uint32_t)timed << 8;
    uint32_t zero = 0;
    __atomic_compare_exchange_n(&s->drive, &zero, pick, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&s->drive, __ATOMIC_ACQUIRE);
}
static inline int fenestra(const char *p) {
    if (!p || p[0] != 'A' || p[1] != ':' || p[2] != '\\' || p[3] != 'F' || p[4] != 'E' || p[5] != 'N' ||
        p[6] != 'E' || p[7] != 'S' || p[8] != 'T' || p[9] != 'R' || p[10] != 'A')
        return 0;
    return p[11] == 0 || p[11] == '\\';
}
/* Native file calls want the original mapping, as in Live: map a cacheable alias back. */
static char *native(char *p) {
    uintptr_t a = (uintptr_t)p;
    return (char *)(a >= UINT64_C(0xc00000000) && a < UINT64_C(0x1000000000) ? a - UINT64_C(0x400000000) : a);
}
/* 0 = refuse (undecided or too long), else the path to use. */
static const char *route(const char *p, char *buf) {
    uint32_t d = drive();
    if (d == 1) return p;
    if (d == 2) {
        unsigned i = 0;
        for (; i < BUFN - 1u && p[i]; ++i) buf[i] = p[i];
        if (p[i]) { S->refused++; return 0; }
        buf[i] = 0; buf[0] = 'B';
        S->rewrites++;
        return native(buf);
    }
    S->refused++;
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
/* Entry points: the common (non-FENESTRA) case is a plain tail branch, no stack. */
int32_t hook_open(const char *p, uint32_t f, uint32_t a) { return fenestra(p) ? slow_open(p, f, a) : o_open(p, f, a); }
uint32_t hook_remove(const char *p) { return fenestra(p) ? slow_remove(p) : o_remove(p); }
uint32_t hook_rename(const char *p, const char *n) { return fenestra(p) || fenestra(n) ? slow_rename(p, n) : o_rename(p, n); }
uint32_t hook_opendir(const char *p, int32_t *fd) { return fenestra(p) ? slow_opendir(p, fd) : o_opendir(p, fd); }
uint32_t hook_mkdir(const char *p) { return fenestra(p) ? slow_mkdir(p) : o_mkdir(p); }

/* v42: SD (B:) transfers through the stock serialized read/write wrappers are DMA without
 * data-cache maintenance on the caller's buffer (measured on hw 11-12: stale cache lines in
 * reads, lost CPU writes in writes; CFexpress is fine). Maintain the buffer here for every
 * call: clean+invalidate before and after a read, clean before a write. Only RAM addresses
 * (cacheable 0xa.. and its 0xe.. alias) are touched; anything else passes through unchanged. */
#define CACHE_OP(op, a, n) do { \
    uint64_t ctr_; __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr_)); \
    uint64_t line_ = 4u << ((ctr_ >> 16) & 15u), at_ = (uint64_t)(uintptr_t)(a) & ~(line_ - 1u), end_ = (uint64_t)(uintptr_t)(a) + (n); \
    for (; at_ < end_; at_ += line_) __asm__ volatile("dc " op ", %0" : : "r"(at_) : "memory"); \
    __asm__ volatile("dsb sy" : : : "memory"); } while (0)
static inline int ram(const void *p, uint32_t n) {
    uint64_t a = (uint64_t)(uintptr_t)p;
    return n && n <= 0x4000000u && a >= UINT64_C(0xa00000000) && a < UINT64_C(0x1000000000) && a + n <= UINT64_C(0x1000000000);
}
uint32_t hook_read(int32_t fd, void *buf, uint32_t n, uint32_t *done) {
    int m = ram(buf, n);
    if (m) CACHE_OP("civac", buf, n);
    uint32_t r = o_read(fd, buf, n, done);
    if (m) CACHE_OP("civac", buf, n);
    return r;
}
uint32_t hook_write(int32_t fd, const void *buf, uint32_t n, uint32_t *done, void *x) {
    if (ram(buf, n)) CACHE_OP("cvac", buf, n);
    return o_write(fd, buf, n, done, x);
}
