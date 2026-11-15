/* Host round trip for the v37 embedded preview: writer (Live layer) -> trailing
 * atom -> reader + JPEG decoder (playback layer).
 * usage: test_preview IN.yuv OUT.mov-tail DECODED.yuv [prefix-bytes] [fail-at]
 * IN.yuv: 45 I420 frames of 1024x682. */
#include "../src-fix37/codec/preview_writer.h"
#include "../pb-fix37/preview.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FRAME LIVE_PREVIEW_FRAME_BYTES
struct sink { uint8_t *data; size_t cap, high; const uint8_t *frames; unsigned yields, writes; long fail_at; };
static int frame(void *v, uint32_t i, void *out, size_t n) {
    struct sink *s = v;
    if (n != FRAME) return -1;
    memcpy(out, s->frames + (size_t)i * FRAME, FRAME);
    return 0;
}
static int write_at(void *v, uint64_t at, const void *p, size_t n) {
    struct sink *s = v;
    if (s->fail_at >= 0 && (long)s->writes++ >= s->fail_at) return -1;
    if (at + n > s->cap) return -1;
    memcpy(s->data + at, p, n);
    if (at + n > s->high) s->high = at + n;
    return 0;
}
static void yield(void *v) { ((struct sink *)v)->yields++; }
static double psnr(const uint8_t *a, const uint8_t *b, size_t n) {
    double e = 0;
    for (size_t i = 0; i < n; i++) { double d = (double)a[i] - b[i]; e += d * d; }
    return e ? 10 * log10(255.0 * 255.0 * n / e) : 99;
}
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    size_t prefix = argc > 4 ? strtoul(argv[4], 0, 0) : 4096;
    struct sink s = {0};
    s.fail_at = argc > 5 ? atol(argv[5]) : -1;
    uint8_t *frames = malloc((size_t)FRAME * LIVE_PREVIEW_FRAMES), *scratch = malloc(FRAME), *io = malloc(65540);
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(frames, FRAME, LIVE_PREVIEW_FRAMES, f) != LIVE_PREVIEW_FRAMES) { fprintf(stderr, "input\n"); return 2; }
    fclose(f);
    s.cap = prefix + (size_t)LIVE_PREVIEW_FRAMES * LIVE_PREVIEW_JPEG_MAX + 4096;
    s.data = calloc(1, s.cap); s.frames = frames; s.high = prefix;
    LivePreviewWriter w = {&s, frame, write_at, yield};
    uint64_t end = 0;
    int rc = live_preview_write(&w, scratch, FRAME, io, 65540, prefix, &end, 40, 1000);
    if (s.fail_at >= 0) { printf("fail_at=%ld rc=%d\n", s.fail_at, rc); return rc == -1 ? 0 : 1; }
    if (rc || end != s.high) { fprintf(stderr, "write rc=%d end=%llu high=%zu\n", rc, (unsigned long long)end, s.high); return 1; }
    f = fopen(argv[2], "wb"); fwrite(s.data + prefix, 1, end - prefix, f); fclose(f);
    /* reader */
    uint64_t atom = 0; uint32_t atom_bytes = 0; LpPreview p; LpClip c;
    if (lp_clip_tail(s.data + end - 8, end, &atom, &atom_bytes) || atom != prefix) { fprintf(stderr, "tail\n"); return 1; }
    size_t head = atom_bytes < LP_CLIP_HEAD ? atom_bytes : LP_CLIP_HEAD;
    if (lp_clip_open(s.data + atom, head, atom, atom_bytes, &p, &c, 7)) { fprintf(stderr, "open\n"); return 1; }
    if (p.frames != LIVE_PREVIEW_FRAMES || p.frame_bytes != FRAME || p.duration_us != 45u * 66667u) return 1;
    uint32_t idx = 99;
    if (lp_preview_index(&p, 0, 7, &idx) || idx != 0 || lp_preview_index(&p, 2999000, 7, &idx) || idx != 44 ||
        lp_preview_index(&p, 3000015, 7, &idx) != 1 || lp_preview_index(&p, 0, 8, &idx) != -1) { fprintf(stderr, "index\n"); return 1; }
    uint32_t pcm = 0, pcm_frames = 0;
    if (lp_clip_audio(s.data + c.at[p.frames], &c, &pcm, &pcm_frames) || pcm != 40 || pcm_frames != 1000) { fprintf(stderr, "audio record\n"); return 1; }
    LpJpeg *j = malloc(sizeof *j);
    uint8_t *out = malloc(FRAME);
    f = fopen(argv[3], "wb");
    double worst = 99, sum = 0; uint32_t big = 0, total = 0;
    for (uint32_t i = 0; i < p.frames; i++) {
        uint32_t n = (uint32_t)(c.at[i + 1] - c.at[i]);
        memset(out, 0x55, FRAME);
        if (lp_jpeg_decode(j, s.data + c.at[i], n, out, 1024, 682)) { fprintf(stderr, "decode %u\n", i); return 1; }
        double q = psnr(out, frames + (size_t)i * FRAME, FRAME);
        if (q < worst) worst = q;
        sum += q; total += n; if (n > big) big = n;
        fwrite(out, 1, FRAME, f);
        if (argc > 6) { char name[256]; snprintf(name, sizeof name, "%s/f%02u.jpg", argv[6], i); FILE *g = fopen(name, "wb"); fwrite(s.data + c.at[i], 1, n, g); fclose(g); }
    }
    fclose(f);
    /* truncated and corrupted input must fail or finish, never run away */
    uint32_t n0 = (uint32_t)(c.at[1] - c.at[0]);
    (void)lp_jpeg_decode(j, s.data + c.at[0], n0 / 2, out, 1024, 682);
    uint8_t *bad = malloc(n0); memcpy(bad, s.data + c.at[0], n0);
    for (uint32_t k = 700; k < n0; k += 97) bad[k] ^= 0x5a;
    (void)lp_jpeg_decode(j, bad, n0, out, 1024, 682);
    uint8_t tail[8]; memcpy(tail, s.data + end - 8, 8); tail[7] = 'X';
    if (!lp_clip_tail(tail, end, &atom, &atom_bytes)) return 1;
    printf("atom_bytes=%llu frames=%u avg_jpeg=%u max_jpeg=%u psnr_avg=%.2f psnr_min=%.2f yields=%u\n",
           (unsigned long long)(end - prefix), p.frames, total / p.frames, big, sum / p.frames, worst, s.yields);
    return 0;
}
