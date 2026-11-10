/* Host test: real Wave5 stream -> rewritten Annex-B starting at an I picture. */
#include "live_h264_idr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t next(const unsigned char *p, size_t n, size_t a) {
    for (; a + 4 <= n; a++) if (!p[a] && !p[a+1] && !p[a+2] && p[a+3] == 1) return a;
    return n;
}
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); static unsigned char in[1 << 20], out[1 << 18];
    size_t n = fread(in, 1, sizeof in, f); fclose(f);
    /* split into AUs at AUD (type 9) */
    size_t aus[64], na = 0;
    for (size_t a = next(in, n, 0); a < n; a = next(in, n, a + 4)) if ((in[a+4] & 31) == 9) aus[na++] = a;
    aus[na] = n;
    /* the stream after the last frame is stale buffer data: trust only the 10 known frames */
    if (na > 10) na = 10;
    const unsigned char *sps = 0, *pps = 0; size_t sn = 0, pn = 0;
    for (size_t a = next(in, n, 0); a < aus[1]; a = next(in, n, a + 4)) {
        size_t b = next(in, n, a + 4), t = in[a+4] & 31;
        if (t == 7) { sps = in + a + 4; sn = b - a - 4; }
        if (t == 8) { pps = in + a + 4; pn = b - a - 4; }
    }
    live_h264_idr s;
    if (live_h264_idr_init(&s, sps, sn, pps, pn)) { puts("init failed"); return 1; }
    printf("max_frame_num 2^%u poc_type %u lsb 2^%u cabac %u dbk %u wp %u\n", s.log2_max_frame_num,
           s.poc_type, s.log2_max_poc_lsb, s.cabac, s.deblocking_control, s.weighted_pred);
    FILE *o = fopen(argv[2], "wb"); unsigned started = 0, idrs = 0, written = 0;
    for (size_t i = 0; i < na; i++) {
        size_t on; unsigned idr;
        int rc = live_h264_idr_au(&s, in + aus[i], aus[i+1] - aus[i], out, sizeof out, &on, &idr);
        if (rc) { printf("au %zu: skipped (before first I)\n", i); continue; }
        if (!started && !idr) continue;
        started = 1; idrs += idr; written++;
        printf("au %zu: %zu -> %zu bytes idr %u\n", i, aus[i+1] - aus[i], on, idr);
        fwrite(out, 1, on, o);
    }
    fclose(o);
    printf("%u AUs written, %u IDR\n", written, idrs);
    return !(written && idrs);
}
