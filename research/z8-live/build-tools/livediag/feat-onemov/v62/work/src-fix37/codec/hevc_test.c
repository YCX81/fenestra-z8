/* Host check: HEADERS-style param sets -> live_hevc_au per AU -> au ring style start at an
 * IRAP -> HEVC MOV. usage: hevc_test in.hevc width height out.mov [start_au] */
#include "live_codec.h"
#include "live_hevc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int wr(void *o, uint64_t off, const void *p, size_t n) { FILE *f = o; return fseek(f, (long)off, SEEK_SET) || fwrite(p, 1, n, f) != n; }
static size_t sc(const uint8_t *p, size_t n, size_t a, unsigned *sz) {
    for (; a + 3 <= n; a++) if (!p[a] && !p[a + 1]) { if (p[a + 2] == 1) { *sz = 3; return a; } if (a + 4 <= n && !p[a + 2] && p[a + 3] == 1) { *sz = 4; return a; } }
    *sz = 0; return n;
}
int main(int argc, char **argv) {
    if (argc < 5) return 2;
    unsigned W = atoi(argv[2]), H = atoi(argv[3]), start = argc > 5 ? atoi(argv[5]) : 0;
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint8_t *buf = malloc(n); fread(buf, 1, n, f); fclose(f);
    /* AU boundaries: a non-VCL after a VCL, or a VCL with first_slice_segment flag after a VCL */
    size_t au_at[4096]; unsigned nau = 0; int seen_vcl = 0; unsigned z, z2;
    static live_hevc h; memset(&h, 0, sizeof h);
    size_t a = sc(buf, n, 0, &z);
    while (z) {
        size_t s = a + z, b = sc(buf, n, s, &z2); unsigned t = (buf[s] >> 1) & 63;
        int vcl = t <= 31, first = vcl && (buf[s + 2] & 0x80);
        if (!nau || (seen_vcl && (!vcl || first))) { au_at[nau++] = a; seen_vcl = 0; }
        if (vcl) seen_vcl = 1;
        if (t >= 32 && t <= 34 && !h.ready) { size_t l = b - s; while (l && !buf[s + l - 1]) l--; if (live_hevc_param(&h, buf + s, l, W, H)) { puts("param fail"); return 1; } }
        a = b; z = z2;
    }
    au_at[nau] = n;
    printf("aus %u ready %u cropped %u sps %u->%u\n", nau, h.ready, h.cropped, h.sps_raw_n, h.sps_n);
    FILE *o = fopen(argv[4], "w+b"); static live_mov m;
    if (live_mov_begin_codec(&m, wr, o, W, H, LIVE_CODEC_HEVC)) return 1;
    static uint8_t out[1 << 20]; unsigned used = 0, began = 0;
    for (unsigned i = 0; i < nau; i++) {
        size_t on; unsigned irap;
        int rc = live_hevc_au(&h, buf + au_at[i], au_at[i + 1] - au_at[i], out, sizeof out, &on, &irap);
        if (rc) { printf("au %u rc %d\n", i, rc); return 1; }
        if (i < start) continue;
        if (!began && !irap) continue;
        began = 1;
        if (live_mov_add(&m, out, on, (uint64_t)i * 33367)) { printf("mov_add %u fail\n", i); return 1; }
        used++;
    }
    printf("irap %u idr %u cra %u trail %u inband %u mismatch %u dropped %u used %u\n", h.n_irap, h.n_idr, h.n_cra, h.n_trail, h.n_ps_inband, h.n_ps_mismatch, h.n_dropped_nal, used);
    static const char uuid[37] = "12345678-1234-1234-1234-123456789ABC";
    uint64_t first = m.pts[0];
    if (live_mov_finish(&m, first + 33367ull * (used / 2), 33367, uuid)) { puts("finish fail"); return 1; }
    fclose(o);
    puts("ok");
    return 0;
}
