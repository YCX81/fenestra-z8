/* Host check: hardware AUs (probe w5-clip) -> IDR rewrite -> v4 MOV writer. */
#include "live_codec.h"
#include "live_h264_idr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int write_at(void *o, uint64_t at, const void *p, size_t n) { FILE *f = o; return fseek(f, (long)at, SEEK_SET) || fwrite(p, 1, n, f) != n; }
int main(int argc, char **argv) {
    (void)argc;
    static unsigned char aus[8 << 20], hdr[1 << 16], out[1 << 20];
    FILE *f = fopen(argv[1], "rb"); size_t n = fread(aus, 1, sizeof aus, f); fclose(f);
    f = fopen(argv[2], "rb"); size_t hn = fread(hdr, 1, sizeof hdr, f); fclose(f);
    const unsigned char *sps = 0, *pps = 0; size_t sn = 0, pn = 0;
    for (size_t i = 0; i + 5 < hn && (!sps || !pps); i++)
        if (!hdr[i] && !hdr[i+1] && !hdr[i+2] && hdr[i+3] == 1) {
            size_t j = i + 4; while (j + 4 < hn && !(!hdr[j] && !hdr[j+1] && !hdr[j+2] && hdr[j+3] == 1)) j++;
            if ((hdr[i+4] & 31) == 7) { sps = hdr + i + 4; sn = j - i - 4; }
            if ((hdr[i+4] & 31) == 8) { pps = hdr + i + 4; pn = j - i - 4; }
        }
    live_h264_idr s; if (live_h264_idr_init(&s, sps, sn, pps, pn)) { puts("idr init"); return 1; }
    live_mov *m = calloc(1, sizeof *m); FILE *mov = fopen(argv[3], "w+b");
    if (live_mov_begin(m, write_at, mov, 1024, 680)) { puts("begin"); return 1; }
    unsigned count = 0, idrs = 0, started = 0; uint64_t first = 0, last = 0;
    for (size_t at = 0; at + 12 <= n;) {
        uint32_t size; uint64_t pts; memcpy(&size, aus + at, 4); memcpy(&pts, aus + at + 4, 8); at += 12;
        size_t on; unsigned idr;
        if (live_h264_idr_au(&s, aus + at, size, out, sizeof out, &on, &idr)) { printf("rewrite failed at %u\n", count); return 1; }
        at += size;
        if (!started && !idr) continue;
        if (!started) first = pts;
        started = 1; idrs += idr;
        if (count >= LIVE_MAX_FRAMES) break;
        if (live_mov_add(m, out, on, pts + 1000000)) { printf("mov_add failed at %u\n", count); return 1; }
        count++; last = pts;
    }
    char uuid[37] = "4C495645-5735-4857-8000-000000000001";
    if (live_mov_finish(m, first + 1000000 + (last - first) / 2, 33367, uuid)) { puts("finish"); return 1; }
    fclose(mov);
    printf("MOV: %u samples, %u IDR\n", count, idrs);
    return 0;
}
