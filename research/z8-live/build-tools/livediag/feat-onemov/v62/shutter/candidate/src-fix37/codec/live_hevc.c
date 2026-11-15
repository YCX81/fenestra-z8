/* HEVC access-unit handling for the Wave5 photo-mode Live encoder, see live_hevc.h. */
#include "live_hevc.h"
#include <string.h>

#define SPS_LIMIT 1024   /* v43: hw Wave5 SPS is 415 B */

static unsigned bit_at(const uint8_t *p, unsigned bit) { return (p[bit >> 3] >> (7 - (bit & 7))) & 1; }

struct rd { const uint8_t *p; unsigned pos, end; int bad; };
static unsigned rbits(struct rd *r, unsigned n) {
    unsigned v = 0;
    if (n > 32 || r->pos > r->end || n > r->end - r->pos) { r->bad = 1; return 0; }
    while (n--) v = (v << 1) | bit_at(r->p, r->pos++);
    return v;
}
static unsigned rue(struct rd *r) {
    unsigned z = 0;
    while (!r->bad && !rbits(r, 1)) if (++z > 20) { r->bad = 1; return 0; }
    return r->bad ? 0 : ((1u << z) - 1) + rbits(r, z);
}
static void pbit(uint8_t *p, unsigned *pos, unsigned b) { if (b) p[*pos >> 3] |= (uint8_t)(0x80u >> (*pos & 7)); ++*pos; }
static void pue(uint8_t *p, unsigned *pos, unsigned v) {
    unsigned x = v + 1, n = 0;
    while ((x >> n) > 1) n++;
    for (unsigned i = 0; i < n; i++) pbit(p, pos, 0);
    for (int i = (int)n; i >= 0; i--) pbit(p, pos, (x >> i) & 1);
}

/* EBSP (after the 2-byte header) -> RBSP; -1 on bad emulation prevention */
static int unescape(const uint8_t *nal, size_t n, uint8_t *rbsp, size_t *size) {
    unsigned zeros = 0; size_t k = 0;
    for (size_t i = 2; i < n; ++i) {
        uint8_t v = nal[i];
        if (zeros == 2 && v <= 3) {
            if (v != 3) return -1;
            zeros = 0; continue;
        }
        rbsp[k++] = v; zeros = v == 0 ? zeros + 1 : 0;
    }
    *size = k; return 0;
}

int live_hevc_sps_crop(const uint8_t *nal, size_t n, unsigned w, unsigned h, uint8_t *out, size_t cap, size_t *written) {
    if (!nal || !out || !written || n < 4 || n > SPS_LIMIT || ((nal[0] >> 1) & 63) != 33 || (nal[0] & 0x81) || (nal[1] & 7) != 1) return -1;
    uint8_t rbsp[SPS_LIMIT], o[SPS_LIMIT + 8], e[SPS_LIMIT + 8];
    size_t size;
    if (unescape(nal, n, rbsp, &size) || size < 14 || !rbsp[size - 1]) return -1;
    unsigned bits = (unsigned)size * 8;
    while (!bit_at(rbsp, bits - 1)) bits--;
    struct rd r = {rbsp, 0, bits - 1, 0};
    rbits(&r, 4); unsigned sub = rbits(&r, 3); rbits(&r, 1);
    rbits(&r, 32); rbits(&r, 32); rbits(&r, 32);   /* general PTL 96 bits */
    unsigned pp[8] = {0}, lp[8] = {0};
    for (unsigned i = 0; i < sub; i++) { pp[i] = rbits(&r, 1); lp[i] = rbits(&r, 1); }
    if (sub) for (unsigned i = sub; i < 8; i++) rbits(&r, 2);
    for (unsigned i = 0; i < sub; i++) { if (pp[i]) { rbits(&r, 32); rbits(&r, 32); rbits(&r, 24); } if (lp[i]) rbits(&r, 8); }
    rue(&r); unsigned chroma = rue(&r); unsigned W = rue(&r), H = rue(&r);
    if (r.bad || chroma != 1 || w > W || h > H || ((W - w) & 1) || ((H - h) & 1) || W - w >= 64 || H - h >= 64) return -1;
    unsigned start = r.pos, off[4] = {0};
    if (rbits(&r, 1)) for (unsigned i = 0; i < 4; i++) off[i] = rue(&r);
    unsigned tail = r.pos;
    unsigned dl = rue(&r), dc = rue(&r);
    if (r.bad || dl || dc) return -1;
    unsigned want_r = (W - w) / 2, want_b = (H - h) / 2;
    if (off[0] == 0 && off[2] == 0 && off[1] == want_r && off[3] == want_b) {
        if (cap < n) return -1;
        memmove(out, nal, n); *written = n; return 0;
    }
    if (off[0] || off[1] || off[2] || off[3]) return -1;   /* a different window: leave it */
    memset(o, 0, sizeof o);
    unsigned pos = 0;
    for (unsigned i = 0; i < start; i++) pbit(o, &pos, bit_at(rbsp, i));
    if (want_r || want_b) { pbit(o, &pos, 1); pue(o, &pos, 0); pue(o, &pos, want_r); pue(o, &pos, 0); pue(o, &pos, want_b); }
    else pbit(o, &pos, 0);
    if (pos + (bits - tail) > SPS_LIMIT * 8) return -1;
    for (unsigned i = tail; i < bits; i++) pbit(o, &pos, bit_at(rbsp, i));   /* includes the stop bit */
    size_t len = 2; unsigned zeros = 0;
    e[0] = nal[0]; e[1] = nal[1];
    for (unsigned i = 0; i < (pos + 7) / 8; i++) {
        uint8_t v = o[i];
        if (zeros == 2 && v <= 3) { if (len >= SPS_LIMIT) return -1; e[len++] = 3; zeros = 0; }
        if (len >= SPS_LIMIT) return -1;
        e[len++] = v; zeros = v == 0 ? zeros + 1 : 0;
    }
    if (cap < len) return -1;
    memcpy(out, e, len); *written = len; return 0;
}

int live_hevc_param(live_hevc *h, const uint8_t *nal, size_t n, unsigned width, unsigned height) {
    if (!h || !nal || n < 3) return -1;
    h->width = width; h->height = height;
    unsigned t = (nal[0] >> 1) & 63;
    if (t == 32) { if (n > sizeof h->vps) return -1; memcpy(h->vps, nal, n); h->vps_n = (unsigned)n; }
    else if (t == 34) { if (n > sizeof h->pps) return -1; memcpy(h->pps, nal, n); h->pps_n = (unsigned)n; }
    else if (t == 33) {
        size_t k;
        if (n > sizeof h->sps_raw) return -1;
        memcpy(h->sps_raw, nal, n); h->sps_raw_n = (unsigned)n;
        if (!live_hevc_sps_crop(nal, n, width, height, h->sps, sizeof h->sps, &k)) { h->sps_n = (unsigned)k; h->cropped = 1; }
        else { memcpy(h->sps, nal, n); h->sps_n = (unsigned)n; h->cropped = 0; }
    } else return 0;
    h->ready = h->vps_n && h->sps_n && h->pps_n;
    return 0;
}

static size_t sc(const uint8_t *p, size_t n, size_t a, unsigned *sz) {
    for (; a + 3 <= n; a++)
        if (p[a] == 0 && p[a + 1] == 0) {
            if (p[a + 2] == 1) { *sz = 3; return a; }
            if (a + 4 <= n && p[a + 2] == 0 && p[a + 3] == 1) { *sz = 4; return a; }
        }
    *sz = 0; return n;
}
static int emit(uint8_t *out, size_t cap, size_t *k, const uint8_t *nal, size_t n) {
    if (n > cap || cap - n < 4 || *k > cap - n - 4) return -1;
    out[*k] = 0; out[*k + 1] = 0; out[*k + 2] = 0; out[*k + 3] = 1;
    memcpy(out + *k + 4, nal, n); *k += n + 4; return 0;
}

int live_hevc_au(live_hevc *h, const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *out_n, unsigned *irap) {
    unsigned z, z2, vcl = 0, first = 64;
    size_t a, b, k = 0;
    if (!h || !h->ready || !in || !out || !out_n || !irap) return -1;
    *irap = 0;
    a = sc(in, n, 0, &z);
    if (a != 0) return -1;
    /* pass 1: find the first VCL type */
    for (size_t x = a; z; ) {
        size_t s = x + z; b = sc(in, n, s, &z2);
        size_t len = b - s;
        while (len && in[s + len - 1] == 0) len--;
        if (len >= 2) {
            unsigned t = (in[s] >> 1) & 63;
            if (t <= 31 && first == 64) first = t;
            if (t >= 32 && t <= 34) {
                const uint8_t *ref = t == 32 ? h->vps : t == 33 ? h->sps_raw : h->pps;
                unsigned rn = t == 32 ? h->vps_n : t == 33 ? h->sps_raw_n : h->pps_n;
                h->n_ps_inband++;
                if (rn != len || memcmp(ref, in + s, len)) {   /* encoder changed a set: adopt it */
                    h->n_ps_mismatch++;
                    if (live_hevc_param(h, in + s, len, h->width, h->height) || !h->ready) return -2;
                }
            }
        }
        x = b; z = z2;
    }
    if (first == 64) return -1;
    unsigned ra = first >= 16 && first <= 21;
    if (ra) {
        if (emit(out, cap, &k, h->vps, h->vps_n) || emit(out, cap, &k, h->sps, h->sps_n) || emit(out, cap, &k, h->pps, h->pps_n)) return -1;
    }
    a = sc(in, n, 0, &z);
    while (z) {
        size_t s = a + z; b = sc(in, n, s, &z2);
        size_t len = b - s;
        while (len && in[s + len - 1] == 0) len--;
        if (len >= 2) {
            unsigned t = (in[s] >> 1) & 63;
            if (t <= 31) { if (emit(out, cap, &k, in + s, len)) return -1; vcl = 1; }
            else if (t < 32 || t > 34) h->n_dropped_nal++;   /* in-band sets: stored copies go before IRAPs */   /* AUD 35, EOS/EOB, SEI 39/40, filler */
        }
        a = b; z = z2;
    }
    if (!vcl) return -1;
    h->n_au++; h->last_vcl = first;
    if (ra) { h->n_irap++; if (first == 19 || first == 20) h->n_idr++; else if (first == 21) h->n_cra++; }
    else h->n_trail++;
    *irap = ra; *out_n = k;
    return 0;
}
