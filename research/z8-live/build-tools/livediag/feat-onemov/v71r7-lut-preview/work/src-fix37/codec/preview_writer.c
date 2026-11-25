/* v37 in-camera preview: baseline JPEG pictures in a trailing 'free' atom of the
 * Live MOV (layout in preview_writer.h). Integer only, no allocation, no statics
 * that are written; the caller supplies every buffer. */
#include "preview_writer.h"
#if defined(__aarch64__) && !defined(__APPLE__) && !defined(__linux__) && !defined(LIVE_HOST_TEST)
#define PW_FS_SEC __attribute__((section(".filmsim.pw")))
#else
#define PW_FS_SEC
#endif
static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
__attribute__((unused)) static void le32(uint8_t *p, uint32_t v) { le16(p, v); le16(p + 2, v >> 16); }
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static const uint8_t zigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59,
    52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
/* ITU T.81 Annex K: quantisation (natural order) and the luminance Huffman tables.
 * Chroma reuses the luminance Huffman tables, which the DHT segment declares. */
static const uint8_t base_q[2][64] = {
    {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56,
     14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
     49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99},
    {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99,
     47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99}};
static const uint8_t dc_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t ac_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t ac_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

typedef struct {
    const LivePreviewWriter *w;
    uint8_t *io;
    size_t cap, fill;
    uint64_t at;
    uint32_t total, limit, acc;
    int bits, failed;
    unsigned source_w, source_h, fit_w, fit_h, left, top, chroma_v;
    uint16_t dc_code[12], ac_code[256];
    uint8_t dc_size[12], ac_size[256];
    uint16_t qz[2][64];       /* divisor per coefficient in zigzag order (= table[zigzag[k]] * 8) */
    uint32_t qz_recip[2][64]; /* exact branchless shift-31 reciprocal for ((a + q/2) >> 3) / table */
} encoder;

static void flush(encoder *e) {
    if (e->fill && !e->failed) {
        if (e->w->write_at(e->w->ctx, e->at, e->io, e->fill)) e->failed = -1;
        else e->at += e->fill;
    }
    e->fill = 0;
}
static void byte(encoder *e, unsigned v) {
    if (e->failed) return;
    if (e->total >= e->limit) { e->failed = 1; return; }
    e->io[e->fill++] = (uint8_t)v;
    e->total++;
    if (e->fill == e->cap) flush(e);
}
static void bytes(encoder *e, const uint8_t *p, size_t n) { while (n--) byte(e, *p++); }
static void put_bits(encoder *e, uint32_t code, int n) {
    e->acc = (e->acc << n) | (code & ((1u << n) - 1u));
    e->bits += n;
    while (e->bits >= 8) {
        unsigned v = (e->acc >> (e->bits - 8)) & 255u;
        byte(e, v);
        if (v == 255u) byte(e, 0);
        e->bits -= 8;
    }
}
static void huffman(const uint8_t *bits, const uint8_t *vals, unsigned count, uint16_t *code, uint8_t *size) {
    unsigned k = 0, c = 0;
    for (unsigned len = 1; len <= 16; len++) {
        for (unsigned i = 0; i < bits[len - 1] && k < count; i++, k++) {
            unsigned v = vals ? vals[k] : k;
            code[v] = (uint16_t)c++;
            size[v] = (uint8_t)len;
        }
        c <<= 1;
    }
}
#define MUL(v, c) ((v) * (int32_t)(c))
#define DESCALE(v, n) (((v) + (1 << ((n) - 1))) >> (n))
/* Specialized 1-D row FDCT (step=1, up=2, down=11) with exact flat-row skip. */
static PW_FS_SEC __attribute__((noinline)) void fdct_row(int32_t *p) {
    int32_t p0 = p[0], p1 = p[1], p2 = p[2], p3 = p[3], p4 = p[4], p5 = p[5], p6 = p[6], p7 = p[7];
    if (((p0 ^ p1) | (p0 ^ p2) | (p0 ^ p3) | (p0 ^ p4) | (p0 ^ p5) | (p0 ^ p6) | (p0 ^ p7)) == 0) {
        p[0] = p0 * 32;
        p[1] = 0; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0; p[6] = 0; p[7] = 0;
        return;
    }
    int32_t t0 = p0 + p7, t7 = p0 - p7, t1 = p1 + p6, t6 = p1 - p6;
    int32_t t2 = p2 + p5, t5 = p2 - p5, t3 = p3 + p4, t4 = p3 - p4;
    int32_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
    p[0] = (t10 + t11) * 4;
    p[4] = (t10 - t11) * 4;
    int32_t z1 = MUL(t12 + t13, 4433);
    p[2] = DESCALE(z1 + MUL(t13, 6270), 11);
    p[6] = DESCALE(z1 - MUL(t12, 15137), 11);
    int32_t z2, z3, z4, z5;
    z1 = t4 + t7; z2 = t5 + t6; z3 = t4 + t6; z4 = t5 + t7; z5 = MUL(z3 + z4, 9633);
    t4 = MUL(t4, 2446); t5 = MUL(t5, 16819); t6 = MUL(t6, 25172); t7 = MUL(t7, 12299);
    z1 = MUL(z1, -7373); z2 = MUL(z2, -20995); z3 = MUL(z3, -16069) + z5; z4 = MUL(z4, -3196) + z5;
    p[7] = DESCALE(t4 + z1 + z3, 11);
    p[5] = DESCALE(t5 + z2 + z4, 11);
    p[3] = DESCALE(t6 + z2 + z3, 11);
    p[1] = DESCALE(t7 + z1 + z4, 11);
}
/* Specialized 1-D col FDCT (step=8, up=0, down=15) with exact zero-column skip. */
static PW_FS_SEC __attribute__((noinline)) void fdct_col(int32_t *p) {
    int32_t p0 = p[0], p1 = p[8], p2 = p[16], p3 = p[24], p4 = p[32], p5 = p[40], p6 = p[48], p7 = p[56];
    if ((p0 | p1 | p2 | p3 | p4 | p5 | p6 | p7) == 0) return;
    int32_t t0 = p0 + p7, t7 = p0 - p7, t1 = p1 + p6, t6 = p1 - p6;
    int32_t t2 = p2 + p5, t5 = p2 - p5, t3 = p3 + p4, t4 = p3 - p4;
    int32_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
    p[0]  = DESCALE(t10 + t11, 2);
    p[32] = DESCALE(t10 - t11, 2);
    int32_t z1 = MUL(t12 + t13, 4433);
    p[16] = DESCALE(z1 + MUL(t13, 6270), 15);
    p[48] = DESCALE(z1 - MUL(t12, 15137), 15);
    int32_t z2, z3, z4, z5;
    z1 = t4 + t7; z2 = t5 + t6; z3 = t4 + t6; z4 = t5 + t7; z5 = MUL(z3 + z4, 9633);
    t4 = MUL(t4, 2446); t5 = MUL(t5, 16819); t6 = MUL(t6, 25172); t7 = MUL(t7, 12299);
    z1 = MUL(z1, -7373); z2 = MUL(z2, -20995); z3 = MUL(z3, -16069) + z5; z4 = MUL(z4, -3196) + z5;
    p[56] = DESCALE(t4 + z1 + z3, 15);
    p[40] = DESCALE(t5 + z2 + z4, 15);
    p[24] = DESCALE(t6 + z2 + z3, 15);
    p[8]  = DESCALE(t7 + z1 + z4, 15);
}
static inline unsigned magnitude(int32_t v) {
    uint32_t a = (uint32_t)(v < 0 ? -v : v);
    return a ? (32u - (unsigned)__builtin_clz(a)) : 0u;
}
/* Compact unchecked 64-bit bit-buffer emitter (used when io has >= 512 B headroom & total+512 <= limit). */
static inline void flush_bits64(uint64_t *acc_p, int *bits_p, uint8_t **dst_p) {
    uint64_t acc = *acc_p;
    int bits = *bits_p;
    uint8_t *dst = *dst_p;
    while (bits >= 8) {
        unsigned bv = (unsigned)(acc >> (bits - 8)) & 255u;
        bits -= 8;
        *dst++ = (uint8_t)bv;
        if (__builtin_expect(bv == 255u, 0)) *dst++ = 0u;
    }
    *acc_p = acc;
    *bits_p = bits;
    *dst_p = dst;
}

static void emit_dc_only(encoder *e, int32_t v, int *dc) {
    int32_t diff = v - *dc;
    *dc = v;
    unsigned s = magnitude(diff);
    if (!e->failed && e->cap - e->fill >= 16u && e->limit - e->total >= 16u) {
        uint64_t acc = e->acc; int bits = e->bits;
        uint8_t *dst = e->io + e->fill, *start = dst;
        int dsz = (int)e->dc_size[s];
        acc = (acc << dsz) | e->dc_code[s]; bits += dsz;
        if (s) {
            uint32_t vb = (uint32_t)(diff < 0 ? diff - 1 : diff) & ((1u << s) - 1u);
            acc = (acc << s) | vb; bits += (int)s;
        }
        int esz = (int)e->ac_size[0];
        acc = (acc << esz) | e->ac_code[0]; bits += esz;
        flush_bits64(&acc, &bits, &dst);
        size_t wrote = (size_t)(dst - start);
        e->acc = (uint32_t)acc; e->bits = bits; e->fill += wrote; e->total += (uint32_t)wrote;
        return;
    }
    put_bits(e, e->dc_code[s], (int)e->dc_size[s]);
    if (s) put_bits(e, (uint32_t)(diff < 0 ? diff - 1 : diff), (int)s);
    put_bits(e, e->ac_code[0], (int)e->ac_size[0]);
}

/* Quantize & Huffman-encode an 8x8 block whose 8 row-FDCTs are ALREADY complete. */
static PW_FS_SEC __attribute__((noinline)) void block_finish(encoder *e, int32_t d[64], unsigned table, int *dc) {
    for (unsigned i = 0; i < 8; i++) fdct_col(d + i);
    const uint16_t *qz = e->qz[table];
    const uint32_t *qr = e->qz_recip[table];
    if (!e->failed && e->cap >= 512u && e->cap - e->fill < 512u) flush(e);
    if (__builtin_expect(!e->failed && e->cap - e->fill >= 512u && e->limit - e->total >= 512u, 1)) {
        uint64_t acc = e->acc;
        int bits = e->bits;
        uint8_t *dst = e->io + e->fill, *start = dst;
        const uint16_t *ac_code = e->ac_code;
        const uint8_t *ac_size = e->ac_size;
        int32_t prev = *dc;
        {
            int32_t q = (int32_t)qz[0], half = q >> 1, v = d[0];
            uint32_t a = (uint32_t)(v < 0 ? -v : v);
            int32_t qv = (int32_t)(((uint64_t)((a + (uint32_t)half) >> 3) * qr[0]) >> 31);
            if (v < 0) qv = -qv;
            int32_t diff = qv - prev;
            *dc = qv;
            unsigned s = magnitude(diff);
            int dsz = (int)e->dc_size[s];
            acc = (acc << dsz) | e->dc_code[s]; bits += dsz;
            if (s) {
                uint32_t vb = (uint32_t)(diff < 0 ? diff - 1 : diff) & ((1u << s) - 1u);
                acc = (acc << s) | vb; bits += (int)s;
            }
            if (bits >= 24) flush_bits64(&acc, &bits, &dst);
        }
        unsigned run = 0;
        for (unsigned k = 1; k < 64; k++) {
            int32_t v = d[zigzag[k]];
            int32_t q = (int32_t)qz[k], half = q >> 1;
            if ((uint32_t)(v + half - 1) < (uint32_t)(q - 1)) { run++; continue; }
            uint32_t a = (uint32_t)(v < 0 ? -v : v);
            int32_t qv = (int32_t)(((uint64_t)((a + (uint32_t)half) >> 3) * qr[k]) >> 31);
            if (v < 0) qv = -qv;
            unsigned s = magnitude(qv);
            if (s > 10) { s = 10; qv = qv < 0 ? -1023 : 1023; }
            while (run > 15) {
                int zsz = (int)ac_size[0xf0];
                acc = (acc << zsz) | ac_code[0xf0]; bits += zsz;
                if (bits >= 32) flush_bits64(&acc, &bits, &dst);
                run -= 16;
            }
            unsigned sym = (run << 4) | s;
            int csz = (int)ac_size[sym];
            uint32_t vb = (uint32_t)(qv < 0 ? qv - 1 : qv) & ((1u << s) - 1u);
            acc = (acc << csz) | ac_code[sym];
            acc = (acc << s) | vb;
            bits += csz + (int)s;
            if (bits >= 32) flush_bits64(&acc, &bits, &dst);
            run = 0;
        }
        if (run) {
            int esz = (int)ac_size[0];
            acc = (acc << esz) | ac_code[0]; bits += esz;
        }
        flush_bits64(&acc, &bits, &dst);
        size_t wrote = (size_t)(dst - start);
        e->acc = (uint32_t)acc; e->bits = bits; e->fill += wrote; e->total += (uint32_t)wrote;
        return;
    }
    /* Tight-buffer / near-limit fallback */
    int32_t prev = *dc;
    {
        int32_t q = (int32_t)qz[0], half = q >> 1, v = d[0];
        uint32_t a = (uint32_t)(v < 0 ? -v : v);
        int32_t qv = (int32_t)(((uint64_t)((a + (uint32_t)half) >> 3) * qr[0]) >> 31);
        if (v < 0) qv = -qv;
        int32_t diff = qv - prev;
        *dc = qv;
        unsigned s = magnitude(diff);
        put_bits(e, e->dc_code[s], (int)e->dc_size[s]);
        if (s) put_bits(e, (uint32_t)(diff < 0 ? diff - 1 : diff), (int)s);
    }
    unsigned run = 0;
    for (unsigned k = 1; k < 64; k++) {
        int32_t v = d[zigzag[k]];
        int32_t q = (int32_t)qz[k], half = q >> 1;
        if ((uint32_t)(v + half - 1) < (uint32_t)(q - 1)) { run++; continue; }
        uint32_t a = (uint32_t)(v < 0 ? -v : v);
        int32_t qv = (int32_t)(((uint64_t)((a + (uint32_t)half) >> 3) * qr[k]) >> 31);
        if (v < 0) qv = -qv;
        while (run > 15) { put_bits(e, e->ac_code[0xf0], (int)e->ac_size[0xf0]); run -= 16; }
        unsigned s = magnitude(qv);
        if (s > 10) { s = 10; qv = qv < 0 ? -1023 : 1023; }
        put_bits(e, e->ac_code[run << 4 | s], (int)e->ac_size[run << 4 | s]);
        put_bits(e, (uint32_t)(qv < 0 ? qv - 1 : qv), (int)s);
        run = 0;
    }
    if (run) put_bits(e, e->ac_code[0], (int)e->ac_size[0]);
}

/* Fast pre-mapped 8x8 block fetch + row-FDCT with duplicate-row reuse & O(1) pillarbox skip. */
static PW_FS_SEC __attribute__((noinline)) void block_mapped(encoder *e, const uint8_t *const rows[8], int row_state,
                                                   const int16_t *sx, unsigned table, int *dc) {
    /* sx is monotonic inside [left, left+fit_w): sx[0], sx[7] determine all_in/all_out in O(1)! */
    int sx_in = (sx[0] >= 0 && sx[7] >= 0);
    int sx_out = (sx[0] < 0 && sx[7] < 0);
    if (sx_out || row_state == 0) {
        int32_t q0 = (int32_t)e->qz[table][0], half0 = q0 >> 1;
        int32_t dc_val = 0;
        if (!table) {
            uint32_t a = (uint32_t)(64 * 128);
            dc_val = -(int32_t)(((uint64_t)((a + (uint32_t)half0) >> 3) * e->qz_recip[table][0]) >> 31);
        }
        emit_dc_only(e, dc_val, dc);
        return;
    }
    int32_t d[64];
    if (sx_in && row_state == 2) {
        unsigned x0 = (unsigned)sx[0], x1 = (unsigned)sx[1], x2 = (unsigned)sx[2], x3 = (unsigned)sx[3];
        unsigned x4 = (unsigned)sx[4], x5 = (unsigned)sx[5], x6 = (unsigned)sx[6], x7 = (unsigned)sx[7];
        const uint8_t *prev_r = 0;
        for (unsigned y = 0; y < 8; y++) {
            const uint8_t *r = rows[y];
            int32_t *dp = d + y * 8;
            if (y && r == prev_r) {
                const int32_t *sp = dp - 8;
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
                dp[4] = sp[4]; dp[5] = sp[5]; dp[6] = sp[6]; dp[7] = sp[7];
            } else {
                prev_r = r;
                dp[0] = (int32_t)r[x0] - 128; dp[1] = (int32_t)r[x1] - 128;
                dp[2] = (int32_t)r[x2] - 128; dp[3] = (int32_t)r[x3] - 128;
                dp[4] = (int32_t)r[x4] - 128; dp[5] = (int32_t)r[x5] - 128;
                dp[6] = (int32_t)r[x6] - 128; dp[7] = (int32_t)r[x7] - 128;
                fdct_row(dp);
            }
        }
    } else {
        int32_t def_s = table ? 0 : -128;
        for (unsigned y = 0; y < 8; y++) {
            const uint8_t *r = rows[y];
            int32_t *dp = d + y * 8;
            if (y && r == rows[y - 1]) {
                const int32_t *sp = dp - 8;
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
                dp[4] = sp[4]; dp[5] = sp[5]; dp[6] = sp[6]; dp[7] = sp[7];
            } else if (!r) {
                dp[0] = def_s * 32;
                dp[1] = 0; dp[2] = 0; dp[3] = 0; dp[4] = 0; dp[5] = 0; dp[6] = 0; dp[7] = 0;
            } else {
                for (unsigned x = 0; x < 8; x++) {
                    int16_t s = sx[x];
                    dp[x] = s >= 0 ? (int32_t)r[s] - 128 : def_s;
                }
                fdct_row(dp);
            }
        }
    }
    block_finish(e, d, table, dc);
}

/* Fallback for width > 2048 (unused by Nikon 160..1920 previews, keeps full API parity). */
static PW_FS_SEC __attribute__((minsize,noinline)) void block(encoder *e, const uint8_t *plane, unsigned w, unsigned h, unsigned x0, unsigned y0, unsigned table,
                  int *dc) {
    int32_t d[64];
    unsigned div = table ? 2u : 1u, sw = e->source_w / div, sh = e->source_h / div;
    unsigned vdiv = table ? e->chroma_v : 1u;
    unsigned fw = e->fit_w / div, fh = e->fit_h / vdiv, left = e->left / div, top = e->top / vdiv;
    int direct = (sw == w && sh == h && fw == w && fh == h && !left && !top);
    for (unsigned y = 0; y < 8; y++) {
        unsigned yy = y0 + y < h ? y0 + y : h - 1u;
        if (direct) {
            const uint8_t *row = plane + (size_t)yy * w;
            for (unsigned x = 0; x < 8; x++) d[y * 8 + x] = (int32_t)row[x0 + x < w ? x0 + x : w - 1u] - 128;
        } else {
            for (unsigned x = 0; x < 8; x++) {
                unsigned xx = x0 + x < w ? x0 + x : w - 1u, v = table ? 128u : 0u;
                if (xx >= left && xx - left < fw && yy >= top && yy - top < fh)
                    v = plane[(size_t)((yy - top) * sh / fh) * sw + (xx - left) * sw / fw];
                d[y * 8 + x] = (int32_t)v - 128;
            }
        }
        fdct_row(d + y * 8);
    }
    block_finish(e, d, table, dc);
}
static void marker(encoder *e, unsigned m, unsigned payload) {
    byte(e, 255); byte(e, m); byte(e, (payload + 2) >> 8); byte(e, payload + 2);
}
static PW_FS_SEC int jpeg_scaled(const LivePreviewWriter *w, const uint8_t *i420, unsigned source_w, unsigned source_h,
                      unsigned width, unsigned height, unsigned quality, uint8_t *io, size_t io_bytes, uint64_t at, uint32_t max_bytes,
                      uint32_t *out_bytes,unsigned chroma_v) {
    encoder e;
    if (!w || !w->write_at || !i420 || !io || io_bytes < 64 || !out_bytes || !width || !height || (width & 1) ||
        (height & 1) || width > 4096 || height > 4096 || !source_w || !source_h || (source_w&1) ||
        (source_h&1) || source_w>4096 || source_h>4096 || !quality || quality > 100)
        return -1;
    e.w = w; e.io = io; e.cap = io_bytes; e.fill = 0; e.at = at; e.total = 0; e.limit = max_bytes;
    e.acc = 0; e.bits = 0; e.failed = 0;
    e.source_w=source_w;e.source_h=source_h;e.chroma_v=chroma_v;
    if(source_w*height>width*source_h){e.fit_w=width;e.fit_h=(source_h*width/source_w)&~1u;}
    else {e.fit_h=height;e.fit_w=(source_w*height/source_h)&~1u;}
    if(!e.fit_w||!e.fit_h)return -1;
    e.left=((width-e.fit_w)/2u)&~1u;e.top=((height-e.fit_h)/2u)&~1u;
    for (unsigned i = 0; i < 256; i++) e.ac_size[i] = 0;
    huffman(dc_bits, 0, 12, e.dc_code, e.dc_size);
    huffman(ac_bits, ac_vals, 162, e.ac_code, e.ac_size);
    unsigned scale = quality < 50 ? 5000u / quality : 200u - 2u * quality;
    byte(&e, 255); byte(&e, 0xd8);
    if(chroma_v==1){
        static const uint8_t jfif[14]={'J','F','I','F',0,1,1,0,0,1,0,1,0,0};
        marker(&e,0xe0,sizeof jfif);bytes(&e,jfif,sizeof jfif);
    }else marker(&e, 0xdb, 130);
    for (unsigned t = 0; t < 2; t++) {
        if(chroma_v==1)marker(&e,0xdb,65);
        byte(&e, t);
        uint8_t table[64];
        for (unsigned i = 0; i < 64; i++) {
            unsigned v = (base_q[t][i] * scale + 50u) / 100u;
            v = v < 1 ? 1 : v > 255 ? 255 : v;
            table[i] = (uint8_t)v;
        }
        for (unsigned i = 0; i < 64; i++) {
            unsigned v = table[zigzag[i]];
            byte(&e, v);
            e.qz[t][i] = (uint16_t)(v * 8u);
            e.qz_recip[t][i] = v == 1u ? UINT32_C(0x80000000) : (uint32_t)((UINT64_C(1) << 31) / v + 1u);
        }
    }
    marker(&e, 0xc0, 15);
    byte(&e, 8); byte(&e, height >> 8); byte(&e, height); byte(&e, width >> 8); byte(&e, width); byte(&e, 3);
    byte(&e, 1); byte(&e, 0x20|chroma_v); byte(&e, 0);
    byte(&e, 2); byte(&e, 0x11); byte(&e, 1);
    byte(&e, 3); byte(&e, 0x11); byte(&e, 1);
    if(chroma_v==1){
        /* Native JPEG path uses 4:2:2 and separate table selectors for chroma.
         * Declare identical valid canonical tables under both IDs. */
        for(unsigned table=0;table<2;table++){
            marker(&e,0xc4,1+16+12);byte(&e,table);bytes(&e,dc_bits,16);
            for(unsigned i=0;i<12;i++)byte(&e,i);
            marker(&e,0xc4,1+16+162);byte(&e,0x10|table);bytes(&e,ac_bits,16);bytes(&e,ac_vals,162);
        }
    }else{
        marker(&e, 0xc4, 1 + 16 + 12 + 1 + 16 + 162);
        byte(&e, 0x00); bytes(&e, dc_bits, 16);
        for (unsigned i = 0; i < 12; i++) byte(&e, i);
        byte(&e, 0x10); bytes(&e, ac_bits, 16); bytes(&e, ac_vals, 162);
    }
    marker(&e, 0xda, 10);
    byte(&e, 3); byte(&e, 1); byte(&e, 0); byte(&e, 2); byte(&e, chroma_v==1?0x11:0); byte(&e, 3); byte(&e, chroma_v==1?0x11:0);
    byte(&e, 0); byte(&e, 63); byte(&e, 0);
    const uint8_t *y = i420, *u = y + (size_t)source_w * source_h, *v = u + (size_t)(source_w / 2) * (source_h / 2);
    int dc[3] = {0, 0, 0};
    if (width <= 2048u) {
        int16_t sx_y[2048 + 16], sx_c[1024 + 8];
        unsigned mw = (width + 15u) & ~15u;
        unsigned cw = width / 2u, mcw = mw / 2u;
        unsigned csw = source_w / 2u, csh = source_h / 2u;
        unsigned cfw = e.fit_w / 2u, cfh = e.fit_h / chroma_v;
        unsigned cleft = e.left / 2u, ctop = e.top / chroma_v, ch = height / chroma_v;
        for (unsigned x = 0; x < mw; x++) {
            unsigned xx = x < width ? x : width - 1u;
            sx_y[x] = (xx >= e.left && xx - e.left < e.fit_w)
                ? (int16_t)((xx - e.left) * source_w / e.fit_w) : (int16_t)-1;
        }
        for (unsigned x = 0; x < mcw; x++) {
            unsigned xx = x < cw ? x : cw - 1u;
            sx_c[x] = (xx >= cleft && xx - cleft < cfw)
                ? (int16_t)((xx - cleft) * csw / cfw) : (int16_t)-1;
        }
        for (unsigned my = 0; my < height && !e.failed; my += 8u * chroma_v) {
            if (w->write_at(w->ctx, e.at, e.io, 0)) { e.failed = -1; break; }
            const uint8_t *yr0[8], *yr1[8], *ur[8], *vr[8];
            int y0_cnt = 0, y1_cnt = 0, c_cnt = 0;
            for (unsigned r = 0; r < 8; r++) {
                unsigned yy = my + r < height ? my + r : height - 1u;
                yr0[r] = (yy >= e.top && yy - e.top < e.fit_h)
                    ? y + (size_t)((yy - e.top) * source_h / e.fit_h) * source_w : 0;
                if (yr0[r]) y0_cnt++;
                if (chroma_v == 2) {
                    unsigned yy2 = my + 8u + r < height ? my + 8u + r : height - 1u;
                    yr1[r] = (yy2 >= e.top && yy2 - e.top < e.fit_h)
                        ? y + (size_t)((yy2 - e.top) * source_h / e.fit_h) * source_w : 0;
                    if (yr1[r]) y1_cnt++;
                }
                unsigned cy = my / chroma_v + r;
                if (cy >= ch) cy = ch - 1u;
                if (cy >= ctop && cy - ctop < cfh) {
                    size_t coff = (size_t)((cy - ctop) * csh / cfh) * csw;
                    ur[r] = u + coff;
                    vr[r] = v + coff;
                    c_cnt++;
                } else {
                    ur[r] = 0;
                    vr[r] = 0;
                }
            }
            int rs0 = y0_cnt == 8 ? 2 : (y0_cnt ? 1 : 0);
            int rs1 = y1_cnt == 8 ? 2 : (y1_cnt ? 1 : 0);
            int rsc = c_cnt == 8 ? 2 : (c_cnt ? 1 : 0);
            for (unsigned mx = 0; mx < width; mx += 16) {
                block_mapped(&e, yr0, rs0, sx_y + mx, 0, &dc[0]);
                block_mapped(&e, yr0, rs0, sx_y + mx + 8, 0, &dc[0]);
                if (chroma_v == 2) {
                    block_mapped(&e, yr1, rs1, sx_y + mx, 0, &dc[0]);
                    block_mapped(&e, yr1, rs1, sx_y + mx + 8, 0, &dc[0]);
                }
                block_mapped(&e, ur, rsc, sx_c + mx / 2, 1, &dc[1]);
                block_mapped(&e, vr, rsc, sx_c + mx / 2, 1, &dc[2]);
            }
            if (w->yield && (my / (8u * chroma_v)) % 16u == 15u) w->yield(w->ctx); /* v67: responsive UI poll every 16 MCU rows */
        }
    } else {
        for (unsigned my = 0; my < height && !e.failed; my += 8u*chroma_v) {
            if (w->write_at(w->ctx, e.at, e.io, 0)) { e.failed = -1; break; }
            for (unsigned mx = 0; mx < width; mx += 16) {
                block(&e, y, width, height, mx, my, 0, &dc[0]);
                block(&e, y, width, height, mx + 8, my, 0, &dc[0]);
                if(chroma_v==2){
                    block(&e, y, width, height, mx, my + 8, 0, &dc[0]);
                    block(&e, y, width, height, mx + 8, my + 8, 0, &dc[0]);
                }
                block(&e, u, width / 2, height / chroma_v, mx / 2, my / chroma_v, 1, &dc[1]);
                block(&e, v, width / 2, height / chroma_v, mx / 2, my / chroma_v, 1, &dc[2]);
            }
            if (w->yield && (my / (8u*chroma_v)) % 16u == 15u) w->yield(w->ctx); /* v67: responsive UI poll every 16 MCU rows */
        }
    }
    if (e.bits) put_bits(&e, 0x7f, 8 - e.bits);
    byte(&e, 255); byte(&e, 0xd9);
    flush(&e);
    *out_bytes = (uint32_t)(e.at - at); /* bytes on the card, also after a failure */
    return e.failed;
}
int live_preview_jpeg_scaled(const LivePreviewWriter *w,const uint8_t *i420,unsigned source_w,unsigned source_h,
                      unsigned width,unsigned height,unsigned quality,uint8_t *io,size_t io_bytes,uint64_t at,uint32_t max_bytes,
                      uint32_t *out_bytes){
 return jpeg_scaled(w,i420,source_w,source_h,width,height,quality,io,io_bytes,at,max_bytes,out_bytes,2);
}
int live_preview_jpeg(const LivePreviewWriter *w,const uint8_t *i420,unsigned width,unsigned height,
                      unsigned quality,uint8_t *io,size_t io_bytes,uint64_t at,uint32_t max_bytes,uint32_t *out_bytes){
 return live_preview_jpeg_scaled(w,i420,width,height,width,height,quality,io,io_bytes,at,max_bytes,out_bytes);
}
/* NCTG type 2 = ASCII, type 3 = big-endian u16. Only current facts are emitted. */
static uint8_t *ntag(uint8_t*p,uint32_t id,unsigned type,unsigned count){
 be32(p,id);p[4]=(uint8_t)(type>>8);p[5]=(uint8_t)type;p[6]=(uint8_t)(count>>8);p[7]=(uint8_t)count;return p+8;
}
static uint8_t *nshort(uint8_t*p,unsigned n){p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n;return p+2;}
static int nbox(const LivePreviewWriter*w,uint64_t at,const char tag[4],uint32_t bytes){
 uint8_t h[8];be32(h,bytes);for(unsigned i=0;i<4;i++)h[4+i]=(uint8_t)tag[i];return w->write_at(w->ctx,at,h,8);
}
int live_nikon_metadata_write(const LivePreviewWriter*w,void*scratch,size_t scratch_bytes,uint8_t*io,size_t io_bytes,
                            uint64_t base,uint64_t*end,const LiveNikonFacts *facts){
 static const unsigned sizes[4][2]={{160,120},{1280,720},{640,360},{1920,1080}};
 static const char tags[4][4]={{'N','C','T','H'},{'N','C','V','W'},{'N','C','M','1'},{'N','C','M','2'}};
 static const char maker[18]="NIKON CORPORATION",model[16]="NIKON Z 8",version[32]="NIKON Z 8 Ver.04.06";
 if(!w||!w->frame||!w->write_at||!scratch||scratch_bytes<LIVE_PREVIEW_FRAME_BYTES||!io||io_bytes<4096||!end||
    !facts||!facts->width||!facts->height||facts->width>65535||facts->height>65535||
    !facts->frames||!facts->fps_num||!facts->fps_den||facts->video_codec<2||facts->video_codec>3||facts->has_audio>1||base>UINT32_MAX-3u*1024u*1024u)return -1;
 if(w->frame(w->ctx,LIVE_PREVIEW_FRAMES/2u,scratch,LIVE_PREVIEW_FRAME_BYTES))return -1;
 uint8_t h[512]={0},*p=h+16;
 be32(p,18);p[4]='N';p[5]='C';p[6]='H';p[7]='D';p[8]='N';p[9]='i';p[10]='k';p[11]='o';p[12]='n';p[14]=2;p+=18;
 uint8_t *tg=p;p+=8;
 p=ntag(p,1,2,sizeof maker);for(unsigned i=0;i<sizeof maker;i++)*p++=(uint8_t)maker[i];
 p=ntag(p,2,2,sizeof model);for(unsigned i=0;i<sizeof model;i++)*p++=(uint8_t)model[i];
 p=ntag(p,3,2,sizeof version);for(unsigned i=0;i<sizeof version;i++)*p++=(uint8_t)version[i];
 /* 0x13 is frame count (low/high words). Native nominal FPS is separate
  * from the actual presentation timestamps retained in stts. */
 p=ntag(p,0x13,4,2);be32(p,facts->frames);be32(p+4,0);p+=8;
 for(unsigned tag=0x16;tag<=0x17;tag++){p=ntag(p,tag,5,1);be32(p,facts->fps_num);be32(p+4,facts->fps_den);p+=8;}
 unsigned orient=(facts->orient>=1u && facts->orient<=8u)?facts->orient:1u;
 p=ntag(p,0x18,3,1);p=nshort(p,orient);
 p=ntag(p,0x21,3,1);p=nshort(p,facts->video_codec);
 p=ntag(p,0x22,3,1);p=nshort(p,facts->width);p=ntag(p,0x23,3,1);p=nshort(p,facts->height);
 /* This encoder's 8-bit 4:2:0 SDR format, matching the native mode entry. */
 p=ntag(p,0x24,3,1);p=nshort(p,1);p=ntag(p,0x25,3,1);p=nshort(p,8);p=ntag(p,0x26,3,1);p=nshort(p,1);
 p=ntag(p,0x31,3,1);p=nshort(p,facts->has_audio?1:0);
 p=ntag(p,0x32,3,1);p=nshort(p,facts->has_audio?2:0);
 p=ntag(p,0x33,3,1);p=nshort(p,facts->has_audio?24:0);
 p=ntag(p,0x34,3,1);p=nshort(p,facts->has_audio?48000:0);
 p=ntag(p,0x101,3,4);for(unsigned i=0;i<2;i++)for(unsigned j=0;j<2;j++)p=nshort(p,sizes[i][j]);
 p=ntag(p,0x102,3,8);for(unsigned i=2;i<4;i++)for(unsigned j=0;j<2;j++)p=nshort(p,sizes[i][j]);for(unsigned i=0;i<4;i++)p=nshort(p,0);
 be32(tg,(uint32_t)(p-tg));tg[4]='N';tg[5]='C';tg[6]='T';tg[7]='G';
 if(w->write_at(w->ctx,base,h,(size_t)(p-h)))return -1;
 uint64_t at=base+(size_t)(p-h);
 for(unsigned i=0;i<4;i++){
  if(at-base>3u*1024u*1024u-16u)return -1;
  uint32_t n=0,max=(uint32_t)(3u*1024u*1024u-(at-base)-16u);
  if(jpeg_scaled(w,scratch,LIVE_PREVIEW_WIDTH,LIVE_PREVIEW_HEIGHT,sizes[i][0],sizes[i][1],35,
      io,io_bytes,at+8,max,&n,1)||nbox(w,at,tags[i],n+8))return -1;
  at+=8u+n;if(w->yield)w->yield(w->ctx);
 }
 if(nbox(w,at,"NCDB",8))return -1;at+=8;
 if(nbox(w,base+8,"NCDT",(uint32_t)(at-base-8))||nbox(w,base,"udta",(uint32_t)(at-base)))return -1;
 *end=at;return 0;
}
