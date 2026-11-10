/* Slice-header rewriter: Wave5 I pictures -> IDR, P pictures renumbered (see header). */
#include "live_h264_idr.h"
#include <string.h>

typedef struct { const uint8_t *p; size_t n, bit; int bad; } rd;
typedef struct { uint8_t *p; size_t cap, bit; int bad; } wr;

static unsigned u(rd *r, unsigned k) {
    unsigned v = 0;
    while (k--) {
        if (r->bit >= r->n * 8) { r->bad = 1; return 0; }
        v = v << 1 | (r->p[r->bit >> 3] >> (7 - (r->bit & 7)) & 1);
        r->bit++;
    }
    return v;
}
static unsigned ue(rd *r) {
    unsigned z = 0;
    while (!u(r, 1)) if (++z > 31 || r->bad) { r->bad = 1; return 0; }
    return z ? (1u << z) - 1 + u(r, z) : 0;
}
static int se(rd *r) { unsigned v = ue(r); return v & 1 ? (int)(v + 1) / 2 : -(int)(v / 2); }

static void put(wr *w, unsigned v, unsigned k) {
    while (k--) {
        if (w->bit >= w->cap * 8) { w->bad = 1; return; }
        uint8_t m = (uint8_t)(0x80 >> (w->bit & 7));
        if (!(w->bit & 7)) w->p[w->bit >> 3] = 0;
        if (v >> k & 1) w->p[w->bit >> 3] |= m;
        w->bit++;
    }
}
static void pue(wr *w, unsigned v) {
    unsigned z = 0, x = v + 1;
    while (x >> (z + 1)) z++;
    put(w, 0, z);
    put(w, x, z + 1);
}
static void pse(wr *w, int v) { pue(w, v > 0 ? 2u * (unsigned)v - 1 : 2u * (unsigned)(-v)); }

/* RBSP of NAL payload bytes [1, n) into out; returns length. map[i] = escaped index of rbsp byte i. */
static size_t unescape(const uint8_t *nal, size_t n, uint8_t *out, size_t cap, size_t *map) {
    size_t o = 0, zeros = 0;
    for (size_t i = 1; i < n && o < cap; i++) {
        if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
        zeros = nal[i] ? 0 : zeros + 1;
        if (map) map[o] = i;
        out[o++] = nal[i];
    }
    return o;
}

int live_h264_idr_init(live_h264_idr *s, const uint8_t *sps, size_t sps_n, const uint8_t *pps, size_t pps_n) {
    uint8_t b[256];
    memset(s, 0, sizeof(*s));
    if (!sps || !pps || sps_n < 4 || pps_n < 2 || sps_n > 256 || pps_n > 256) return -1;
    rd r = {b, unescape(sps, sps_n, b, sizeof b, 0), 0, 0};
    unsigned profile = u(&r, 8);
    u(&r, 16); ue(&r);
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128) {
        if (ue(&r) == 3) u(&r, 1);
        ue(&r); ue(&r); u(&r, 1);
        if (u(&r, 1)) return -1;   /* scaling lists: not produced by this encoder */
    }
    s->log2_max_frame_num = ue(&r) + 4;
    s->poc_type = ue(&r);
    if (s->poc_type == 0) s->log2_max_poc_lsb = ue(&r) + 4;
    else if (s->poc_type != 2) return -1;
    ue(&r); u(&r, 1); ue(&r); ue(&r);
    s->frame_mbs_only = u(&r, 1);
    if (r.bad || !s->frame_mbs_only) return -1;
    r = (rd){b, unescape(pps, pps_n, b, sizeof b, 0), 0, 0};
    ue(&r); ue(&r);
    s->cabac = u(&r, 1);
    s->bottom_field_poc = u(&r, 1);
    if (ue(&r)) return -1;          /* slice groups */
    s->num_ref_idx_l0 = ue(&r) + 1;
    s->num_ref_idx_l1 = ue(&r) + 1;
    s->weighted_pred = u(&r, 1);
    s->weighted_bipred = u(&r, 2);
    se(&r); se(&r); se(&r);
    s->deblocking_control = u(&r, 1);
    u(&r, 1);
    s->redundant_pic_cnt = u(&r, 1);
    if (r.bad || !s->cabac) return -1;
    memcpy(s->sps, sps, sps_n); s->sps_n = (unsigned)sps_n;
    memcpy(s->pps, pps, pps_n); s->pps_n = (unsigned)pps_n;
    s->ready = 1;
    return 0;
}

/* Rewrite one slice NAL (header byte at nal[0]). Returns bytes written to out or 0. */
static size_t slice(live_h264_idr *s, const uint8_t *nal, size_t n, uint8_t *out, size_t cap, unsigned *was_i) {
    uint8_t hb[96], hw[112];
    size_t map[96];
    size_t hn = unescape(nal, n, hb, sizeof hb, map);
    rd r = {hb, hn, 0, 0};
    unsigned ref_idc = nal[0] >> 5 & 3;
    unsigned first_mb = ue(&r), type = ue(&r), pps_id = ue(&r), t = type % 5;
    unsigned fn = u(&r, s->log2_max_frame_num), poc = 0, idr = t == 2;
    if (first_mb) return 0;   /* one slice per picture is what the encoder produces */
    if ((nal[0] & 31) == 5) {
        /* the encoder's own periodic IDR: keep it, renumber what follows from it */
        ue(&r);   /* idr_pic_id */
        if (s->poc_type == 0) poc = u(&r, s->log2_max_poc_lsb);
        if (r.bad || n > cap) return 0;
        s->base_frame_num = fn; s->base_poc_lsb = poc; s->have_base = 1;
        memcpy(out, nal, n);
        *was_i = 1;
        return n;
    }
    if (s->poc_type == 0) poc = u(&r, s->log2_max_poc_lsb);
    int dbot = s->poc_type == 0 && s->bottom_field_poc ? se(&r) : 0;
    unsigned redundant = s->redundant_pic_cnt ? ue(&r) : 0;
    unsigned override = 0, l0 = 0, mod = 0, mods[32][2], nm = 0;
    if (t == 0) {
        override = u(&r, 1);
        if (override) l0 = ue(&r);
        mod = u(&r, 1);
        while (mod && nm < 32) {
            mods[nm][0] = ue(&r);
            if (mods[nm][0] == 3) break;
            mods[nm][1] = ue(&r);
            nm++;
        }
        if (mod && nm == 32) return 0;
        if (s->weighted_pred) return 0;
    } else if (t != 2) return 0;   /* B/SP/SI never produced here */
    unsigned adaptive = 0, mmco[24], nmm = 0;
    if (ref_idc) {
        adaptive = u(&r, 1);
        /* MMCO operands are pic-num differences / long-term indices: unaffected by
         * renumbering, so they are copied; an IDR drops them (it resets the DPB) */
        while (adaptive && nmm + 3 <= 24) {
            unsigned op = ue(&r);
            mmco[nmm++] = op;
            if (!op) break;
            if (op == 1 || op == 3 || op == 2 || op == 4 || op == 6) mmco[nmm++] = ue(&r);
            if (op == 3) mmco[nmm++] = ue(&r);
            if (r.bad || op > 6) return 0;
        }
        if (adaptive && (!nmm || mmco[nmm - 1])) return 0;
    }
    unsigned cabac_init = t != 2 ? ue(&r) : 0;
    int qp = se(&r);
    unsigned dbk = 0; int alpha = 0, beta = 0;
    if (s->deblocking_control) {
        dbk = ue(&r);
        if (dbk != 1) { alpha = se(&r); beta = se(&r); }
    }
    if (r.bad) return 0;
    while (r.bit & 7) if (!u(&r, 1)) return 0;   /* cabac_alignment_one_bit */
    size_t data = r.bit >> 3;
    if (data >= hn) return 0;
    size_t src = map[data];

    if (idr) {
        s->base_frame_num = fn;
        s->base_poc_lsb = poc;
        s->have_base = 1;
    } else if (!s->have_base) return 0;
    unsigned mfn = (1u << s->log2_max_frame_num) - 1, mpoc = s->log2_max_poc_lsb ? (1u << s->log2_max_poc_lsb) - 1 : 0;
    unsigned nfn = idr ? 0 : (fn - s->base_frame_num) & mfn;
    unsigned npoc = idr ? 0 : (poc - s->base_poc_lsb) & mpoc;

    wr w = {hw, sizeof hw, 0, 0};
    put(&w, idr ? (3u << 5 | 5) : nal[0], 8);
    pue(&w, 0); pue(&w, type); pue(&w, pps_id);
    put(&w, nfn, s->log2_max_frame_num);
    if (idr) pue(&w, 0);   /* consecutive IDRs are never adjacent here (GOP >= 2) */
    if (s->poc_type == 0) {
        put(&w, npoc, s->log2_max_poc_lsb);
        if (s->bottom_field_poc) pse(&w, dbot);
    }
    if (s->redundant_pic_cnt) pue(&w, redundant);
    if (t == 0) {
        put(&w, override, 1);
        if (override) pue(&w, l0);
        put(&w, mod, 1);
        for (unsigned i = 0; mod && i <= nm; i++) {
            pue(&w, i < nm ? mods[i][0] : 3);
            if (i < nm) pue(&w, mods[i][1]);
        }
    }
    if (idr) put(&w, 0, 2);   /* no_output_of_prior_pics, long_term_reference */
    else if (ref_idc) {
        put(&w, adaptive, 1);
        for (unsigned i = 0; adaptive && i < nmm; i++) pue(&w, mmco[i]);
    }
    if (t != 2) pue(&w, cabac_init);
    pse(&w, qp);
    if (s->deblocking_control) {
        pue(&w, dbk);
        if (dbk != 1) { pse(&w, alpha); pse(&w, beta); }
    }
    while (w.bit & 7) put(&w, 1, 1);
    if (w.bad) return 0;

    /* escape the new header, then append the original (already escaped) slice data */
    size_t o = 0, zeros = 0, hlen = w.bit >> 3;
    for (size_t i = 0; i < hlen; i++) {
        if (o + 2 > cap) return 0;
        if (zeros >= 2 && hw[i] <= 3) { out[o++] = 3; zeros = 0; }
        out[o++] = hw[i];
        zeros = hw[i] ? 0 : zeros + 1;
    }
    if (zeros >= 2 && nal[src] <= 3) { if (o + 1 > cap) return 0; out[o++] = 3; }
    if (o + (n - src) > cap) return 0;
    memcpy(out + o, nal + src, n - src);
    *was_i = idr;
    return o + (n - src);
}

static size_t start_code(const uint8_t *p, size_t n, size_t a, unsigned *len) {
    for (; a + 3 <= n; a++)
        if (!p[a] && !p[a + 1]) {
            if (p[a + 2] == 1) { *len = 3; return a; }
            if (a + 4 <= n && !p[a + 2] && p[a + 3] == 1) { *len = 4; return a; }
        }
    *len = 0;
    return n;
}

int live_h264_idr_au(live_h264_idr *s, const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                     size_t *out_n, unsigned *idr) {
    static const uint8_t sc[4] = {0, 0, 0, 1};
    unsigned z, z2;
    size_t a = start_code(in, n, 0, &z), o = 0;
    *out_n = 0; *idr = 0;
    if (!s->ready) return -1;
    while (z) {
        size_t st = a + z, b = start_code(in, n, st, &z2), len = b - st;
        while (len && !in[st + len - 1]) len--;
        unsigned type = len ? in[st] & 31 : 0;
        if (type == 1 || type == 5) {
            unsigned was_i = 0;
            if (o + 4 > cap) return -1;
            size_t at = o + 4, k = slice(s, in + st, len, out + at, cap - at, &was_i);
            if (!k) return -1;
            if (was_i) *idr = 1;
            memcpy(out + o, sc, 4);
            o = at + k;
        }
        a = b; z = z2;
    }
    if (*idr) {
        /* SPS/PPS ahead of every IDR (input AUD/SPS/PPS are dropped): the MOV writer takes avcC from the stream */
        size_t pre = s->sps_n + s->pps_n + 8;
        if (o + pre > cap) return -1;
        memmove(out + pre, out, o);
        memcpy(out, sc, 4); memcpy(out + 4, s->sps, s->sps_n);
        memcpy(out + 4 + s->sps_n, sc, 4); memcpy(out + 8 + s->sps_n, s->pps, s->pps_n);
        o += pre;
    }
    *out_n = o;
    return o ? 0 : -1;
}
