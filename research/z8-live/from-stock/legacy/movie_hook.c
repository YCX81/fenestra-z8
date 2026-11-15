// SPDX-License-Identifier: GPL-2.0-or-later
// In-camera form of liveify.py. Runs right after the recorder closes the finished MOV.
// Any failure leaves the MOV as the firmware wrote it.
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;

// The file service below every other layer; zero means success, except seek, whose bit 0 does. Flags as the firmware uses them:
// 0 reads an existing file, 0x101 creates one.
#define FS_OPEN(path, flags) ((int (*)(const char *, int, int))0x402daa4c)(path, flags, 0x20)
#define FS_CLOSE ((int (*)(int))0x402daaac)
#define FS_READ ((int (*)(int, void *, u32, u32 *))0x402dab4c)
#define FS_WRITE ((int (*)(int, const void *, u32, u32 *))0x402dabb8)
#define FS_SEEK ((int (*)(int, u64, int, u64 *))0x402dac94)
#define FS_UNLINK ((int (*)(const char *))0x402dada8)
// The recorder keeps the current file's path here; the cave itself is read-only.
#define SAVED_PATH (*(char **)0xa03194f00 + 0x68)
// Empty functions at fixed addresses, so a trace point shows how far the hook got.
#define MARK(n) ((void (*)(long))(0x408c0024 + (n) * 4))(0)
#define MARKV(n, v) ((void (*)(long))(0x408c0024 + (n) * 4))(v)
#define TAG(a, b, c, d) ((u32)(a) << 24 | (b) << 16 | (c) << 8 | (d))
#define SAMPLE_BOX 17

static const char content[] = "com.apple.quicktime.content.identifier";
static const char still_key[] = "com.apple.quicktime.still-image-time";

static u32 be(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static u8 *put(u8 *p, u32 v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
    return p + 4;
}

static u8 *copy(u8 *p, const void *src, u32 n)
{
    const u8 *s = src;
    while (n--) *p++ = *s++;
    return p;
}

static u8 *zero(u8 *p, u32 n)
{
    while (n--) *p++ = 0;
    return p;
}

static u8 *open_box(u8 *p, u32 tag) { return put(p + 4, tag); }
static u8 *open_full(u8 *p, u32 tag) { return zero(open_box(p, tag), 4); }
static u8 *close_box(u8 *start, u8 *end) { put(start, end - start); return end; }

static int write_at(int fd, u64 at, const void *buf, u32 n)
{
    u32 got = 0;
    u64 now;
    return (FS_SEEK(fd, at, 0, &now) & 1) && !FS_WRITE(fd, buf, n, &got) && got == n;
}

static int read_at(int fd, u64 at, void *buf, u32 n)
{
    u32 got = 0;
    u64 now;
    int r = FS_SEEK(fd, at, 0, &now);
    if (!(r & 1)) { MARKV(8, r); return 0; }
    r = FS_READ(fd, buf, n, &got);
    if (r) { MARKV(9, r); return 0; }
    if (got != n) { MARKV(10, got); return 0; }
    return 1;
}

// Find a child box inside [pos, end); returns 1 with its position and size.
static int child(int fd, u64 pos, u64 end, u32 tag, u64 *at, u32 *size)
{
    u8 h[8];
    while (pos + 8 <= end) {
        u32 s;
        if (!read_at(fd, pos, h, 8)) return 0;
        s = be(h);
        if (s < 8 || pos + s > end) return 0;
        if (be(h + 4) == tag) { *at = pos; *size = s; return 1; }
        pos += s;
    }
    return 0;
}

static u8 *atoms(u8 *p, const char *id, u32 id_len, u32 movie_scale, u32 media_scale, u32 delta,
                 u32 still, u32 sample_pos, u32 video, u32 track)
{
    u8 *meta = p, *a, *b, *c, *d, *e;
    u32 start = (u64)still * delta * movie_scale / media_scale, one = (u64)delta * movie_scale / media_scale, i;
    static const u32 matrix[9] = {65536, 0, 0, 0, 65536, 0, 0, 0, 0x40000000};

    p = open_box(p, TAG('m', 'e', 't', 'a'));
    a = p; p = open_full(p, TAG('h', 'd', 'l', 'r')); p = zero(p, 4); p = put(p, TAG('m', 'd', 't', 'a')); p = zero(p, 12); p = close_box(a, p);
    a = p; p = open_full(p, TAG('k', 'e', 'y', 's')); p = put(p, 1);
    b = p; p = open_box(p, TAG('m', 'd', 't', 'a')); p = copy(p, content, sizeof(content) - 1); p = close_box(b, p); p = close_box(a, p);
    a = p; p = open_box(p, TAG('i', 'l', 's', 't'));
    b = p; p = open_box(p, 1);
    c = p; p = open_box(p, TAG('d', 'a', 't', 'a')); p = put(p, 1); p = put(p, 0); p = copy(p, id, id_len); p = close_box(c, p);
    p = close_box(b, p); p = close_box(a, p); p = close_box(meta, p);

    a = p; p = open_box(p, TAG('t', 'r', 'a', 'k'));
    b = p; p = open_full(p, TAG('t', 'k', 'h', 'd')); b[11] = 3; p = zero(p, 8); p = put(p, track); p = zero(p, 4); p = put(p, start + one); p = zero(p, 16);
    for (i = 0; i < 9; i++) p = put(p, matrix[i]);
    p = zero(p, 8); p = close_box(b, p);
    b = p; p = open_box(p, TAG('t', 'r', 'e', 'f'));
    c = p; p = open_box(p, TAG('c', 'd', 's', 'c')); p = put(p, video); p = close_box(c, p); p = close_box(b, p);
    b = p; p = open_box(p, TAG('e', 'd', 't', 's'));
    c = p; p = open_full(p, TAG('e', 'l', 's', 't')); p = put(p, start ? 2 : 1);
    if (start) { p = put(p, start); p = put(p, 0xffffffff); p = put(p, 0x10000); }
    p = put(p, one); p = put(p, 0); p = put(p, 0x10000); p = close_box(c, p); p = close_box(b, p);

    b = p; p = open_box(p, TAG('m', 'd', 'i', 'a'));
    c = p; p = open_full(p, TAG('m', 'd', 'h', 'd')); p = zero(p, 8); p = put(p, media_scale); p = put(p, delta); p = put(p, 0x55c40000); p = close_box(c, p);
    c = p; p = open_full(p, TAG('h', 'd', 'l', 'r')); p = zero(p, 4); p = put(p, TAG('m', 'e', 't', 'a')); p = zero(p, 13); p = close_box(c, p);
    c = p; p = open_box(p, TAG('m', 'i', 'n', 'f'));
    d = p; p = open_full(p, TAG('n', 'm', 'h', 'd')); p = close_box(d, p);
    d = p; p = open_box(p, TAG('d', 'i', 'n', 'f'));
    e = p; p = open_full(p, TAG('d', 'r', 'e', 'f')); p = put(p, 1); p = put(p, 12); p = put(p, TAG('u', 'r', 'l', ' ')); p = put(p, 1); p = close_box(e, p);
    p = close_box(d, p);
    d = p; p = open_box(p, TAG('s', 't', 'b', 'l'));
    e = p; p = open_full(p, TAG('s', 't', 's', 'd')); p = put(p, 1);
    {
        u8 *f = p, *g, *h, *k;
        p = open_box(p, TAG('m', 'e', 'b', 'x')); p = zero(p, 6); *p++ = 0; *p++ = 1;
        g = p; p = open_box(p, TAG('k', 'e', 'y', 's'));
        h = p; p = open_box(p, 1);
        k = p; p = open_box(p, TAG('k', 'e', 'y', 'd')); p = put(p, TAG('m', 'd', 't', 'a')); p = copy(p, still_key, sizeof(still_key) - 1); p = close_box(k, p);
        k = p; p = open_box(p, TAG('d', 't', 'y', 'p')); p = put(p, 0); p = put(p, 65); p = close_box(k, p);
        p = close_box(h, p); p = close_box(g, p); p = close_box(f, p);
    }
    p = close_box(e, p);
    e = p; p = open_full(p, TAG('s', 't', 't', 's')); p = put(p, 1); p = put(p, 1); p = put(p, delta); p = close_box(e, p);
    e = p; p = open_full(p, TAG('s', 't', 's', 'c')); p = put(p, 1); p = put(p, 1); p = put(p, 1); p = put(p, 1); p = close_box(e, p);
    e = p; p = open_full(p, TAG('s', 't', 's', 'z')); p = put(p, 9); p = put(p, 1); p = close_box(e, p);
    e = p; p = open_full(p, TAG('s', 't', 'c', 'o')); p = put(p, 1); p = put(p, sample_pos); p = close_box(e, p);
    p = close_box(d, p); p = close_box(c, p); p = close_box(b, p);
    return close_box(a, p);
}

static void le16(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; }
static void le32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// Exif orientation for the video track's tkhd matrix (a, b of the first row). The NCVW JPEG
// is stored landscape like the video samples, so the still needs the same rotation flag.
static u32 orientation(u32 a, u32 b)
{
    if (b == 0x10000) return 6;
    if (b == 0xffff0000) return 8;
    if (a == 0xffff0000) return 3;
    return 1;
}

// Exif APP1 holding the orientation and the Apple MakerNote with the content identifier.
static u32 exif(u8 *o, const char *id, u32 id_len, u32 orient)
{
    static const u8 note_head[14] = {'A', 'p', 'p', 'l', 'e', ' ', 'i', 'O', 'S', 0, 0, 1, 'M', 'M'};
    u32 note_len = 32 + id_len + 1, tiff_len = 56 + note_len, payload = 6 + tiff_len;
    u8 *t = o + 10, *n = t + 56;
    o[0] = 0xff; o[1] = 0xe1; o[2] = (payload + 2) >> 8; o[3] = payload + 2;
    copy(o + 4, "Exif\0\0", 6);
    copy(t, "II*\0\x08\0\0\0", 8);
    // IFD0 at 8: Orientation, ExifIFD pointer; Exif IFD at 38: MakerNote; note at 56.
    le16(t + 8, 2);
    le16(t + 10, 0x0112); le16(t + 12, 3); le32(t + 14, 1); le32(t + 18, orient);
    le16(t + 22, 0x8769); le16(t + 24, 4); le32(t + 26, 1); le32(t + 30, 38); le32(t + 34, 0);
    le16(t + 38, 1); le16(t + 40, 0x927c); le16(t + 42, 7); le32(t + 44, note_len); le32(t + 48, 56); le32(t + 52, 0);
    copy(n, note_head, 14);
    n[14] = 0; n[15] = 1; n[16] = 0; n[17] = 17; n[18] = 0; n[19] = 2;
    put(n + 20, id_len + 1); put(n + 24, 32); zero(n + 28, 4);
    copy(n + 32, id, id_len); n[32 + id_len] = 0;
    return 4 + payload;
}

static int write_still(int fd, u64 at, u32 size, const char *id, u32 id_len, u32 orient, u8 *buf, u32 cap)
{
    char path[0x80];
    const char *src = SAVED_PATH;
    u32 i = 0, n, got;
    u64 end = at + size, pos;
    int out, ok = 1;
    u8 h[2];

    while (i < sizeof(path) - 1 && src[i]) { path[i] = src[i]; i++; }
    if (i < 4 || path[i - 4] != '.') return 0;
    path[i] = 0; path[i - 3] = 'J'; path[i - 2] = 'P'; path[i - 1] = 'G';
    // The JPEG starts somewhere inside the NCDT entry, after its own small header.
    for (pos = at + 8; pos + 2 < end; pos++) {
        if (!read_at(fd, pos, h, 2)) return 0;
        if (h[0] == 0xff && h[1] == 0xd8) break;
    }
    if (pos + 2 >= end) return 0;
    out = FS_OPEN(path, 0x101);
    if (out < 0) return 0;
    buf[0] = 0xff; buf[1] = 0xd8;
    n = 2 + exif(buf + 2, id, id_len, orient);
    ok = !FS_WRITE(out, buf, n, &got);
    pos += 2;
    while (ok && pos < end) {
        n = end - pos > cap ? cap : end - pos;
        ok = read_at(fd, pos, buf, n) && !FS_WRITE(out, buf, n, &got);
        pos += n;
    }
    FS_CLOSE(out);
    return ok;
}

static u8 *text_id(char *o)
{
    static const char hex[] = "0123456789ABCDEF";
    u64 a, b;
    u32 i;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(a));
    b = a * 0x9e3779b97f4a7c15UL ^ (a >> 7);
    a = b * 0xc2b2ae3d27d4eb4fUL ^ (b >> 11);
    for (i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { o[i] = '-'; continue; }
        if (i == 14) { o[i] = '4'; continue; }
        if (i < 18) { o[i] = hex[a & 15]; a >>= 4; } else { o[i] = hex[b & 15]; b >>= 4; }
    }
    return (u8 *)o;
}

// First full press of a recording: remember where it fell. The cave's writable alias in RAM
// holds this, since the cave itself is read-only.
#define PRESS ((volatile u64 *)0xa0fec3c00)
#define RECORDER_STATE (*(volatile u32 *)0xa03bc22d4)
#define PRESS_MAGIC 0x4c495645u
#define PICTURES PRESS[3]
#define STOP_STAGE PRESS[4]
#define KEY_STATE ((volatile u32 *)0xa0320169c)
#define KEY_CONSUMER ((void (*)(const u32 *))0x40d271e4)
#define SW1_MOVIE 0x2000
#define KEY_HOLD 8
#define DROPPED PRESS[5]
#define HEAD_SAVED PRESS[6]
#define SAVED_HEAD ((u8 *)0xa0fec3c80)
#define VIDEO_QUEUE ((void *)0xa031b9198)
#define QUEUE_COUNT ((u32 (*)(void *, u32))0x40ca341c)
// Batches of 15 pictures kept in the firmware's own queue before the press.
#ifndef HOLD_BATCHES
#define HOLD_BATCHES 4
#endif
#define MODE PRESS[7]
#define MODE_OFF 1
#define MODE_ON 2
#define BATCH_FRAMES 0xb50
#define BATCH_HEAD 0xb54
#define BATCH_END 0xc18
// The recorder task has about 7 KiB of stack left here, too little for these buffers.
#define WORK ((u8 *)0xa0fec4000)

#ifndef PRE_FRAMES
#define PRE_FRAMES 45
#endif
#ifndef POST_FRAMES
#define POST_FRAMES 45
#endif
#define GOP 15

// New sample tables for [start, end) in place of stts..stbl end; the removed length becomes a
// free box. Returns the block length, or 0 if the file does not have the expected shape.
static u32 trimmed_tables(int fd, u8 *o, u64 stbl, u32 stbl_size, u64 stts, u32 delta, u32 start, u32 end)
{
    u8 h[32], *p = o, *b;
    u64 stsc, stsz, co64, stss;
    u32 sc, sz, cz, ss, kept = end - start, chunk = start / GOP, chunks = (end - 1) / GOP - chunk + 1, tail, i;
    u64 stbl_end = stbl + stbl_size;

    if (!child(fd, stbl + 8, stbl_end, TAG('s', 't', 's', 'c'), &stsc, &sc)) return 0;
    if (!child(fd, stbl + 8, stbl_end, TAG('s', 't', 's', 'z'), &stsz, &sz)) return 0;
    if (!child(fd, stbl + 8, stbl_end, TAG('c', 'o', '6', '4'), &co64, &cz)) return 0;
    if (!child(fd, stbl + 8, stbl_end, TAG('s', 't', 's', 's'), &stss, &ss)) return 0;
    if (kept > 400 || chunks > 32 || start % GOP) return 0;
    if (!read_at(fd, stsc, h, 28) || be(h + 16) != 1 || be(h + 20) != GOP) return 0;
    if (!read_at(fd, stss + 16 + (start / GOP) * 4, h, 4) || be(h) != start + 1) return 0;

    b = p; p = open_full(p, TAG('s', 't', 't', 's')); p = put(p, 1); p = put(p, kept); p = put(p, delta); p = close_box(b, p);
    b = p; p = open_full(p, TAG('s', 't', 's', 'c'));
    if (kept % GOP && chunks > 1) {
        p = put(p, 2); p = put(p, 1); p = put(p, GOP); p = put(p, 1);
        p = put(p, chunks); p = put(p, kept % GOP); p = put(p, 1);
    } else {
        p = put(p, 1); p = put(p, 1); p = put(p, kept < GOP ? kept : GOP); p = put(p, 1);
    }
    p = close_box(b, p);
    b = p; p = open_full(p, TAG('s', 't', 's', 'z')); p = put(p, 0); p = put(p, kept);
    if (!read_at(fd, stsz + 20 + start * 4ul, p, kept * 4)) return 0;
    p = close_box(b, p + kept * 4);
    b = p; p = open_full(p, TAG('c', 'o', '6', '4')); p = put(p, chunks);
    if (!read_at(fd, co64 + 16 + chunk * 8ul, p, chunks * 8)) return 0;
    p = close_box(b, p + chunks * 8);
    b = p; p = open_full(p, TAG('s', 't', 's', 's')); p = put(p, (kept + GOP - 1) / GOP);
    for (i = 0; i < kept; i += GOP) p = put(p, i + 1);
    p = close_box(b, p);
    tail = stbl_end - stts;
    if (p - o > tail || tail - (p - o) < 8) return 0;
    b = p; p = open_box(p, TAG('f', 'r', 'e', 'e')); p = zero(p, tail - (b - o) - 8);
    return close_box(b, p) - o;
}

// Audio chunks pair one to one with the video batches, so the same chunk range applies. Builds
// the replacement for stts..stbl end; returns its length and leaves the positions to write at.
static u32 audio_tables(int fd, u8 *o, u64 trak, u32 trak_size, u32 first, u32 chunks, int tail_cut,
                        u64 *at, u64 *mdhd, u64 *tkhd, u32 *samples)
{
    u8 h[64], *p = o, *b;
    u64 mdia, minf, stbl, stts, stsc, stsz, co64, dummy;
    u32 n, size, stbl_size, total, per_chunk, have, keep, rest;

    if (!child(fd, trak + 8, trak + trak_size, TAG('t', 'k', 'h', 'd'), tkhd, &size)) return 0;
    if (!child(fd, trak + 8, trak + trak_size, TAG('m', 'd', 'i', 'a'), &mdia, &n)) return 0;
    if (!child(fd, mdia + 8, mdia + n, TAG('m', 'd', 'h', 'd'), mdhd, &size)) return 0;
    if (!child(fd, mdia + 8, mdia + n, TAG('m', 'i', 'n', 'f'), &minf, &n)) return 0;
    if (!child(fd, minf + 8, minf + n, TAG('s', 'm', 'h', 'd'), &dummy, &size)) return 0;
    if (!child(fd, minf + 8, minf + n, TAG('s', 't', 'b', 'l'), &stbl, &stbl_size)) return 0;
    if (!child(fd, stbl + 8, stbl + stbl_size, TAG('s', 't', 't', 's'), &stts, &size) || !read_at(fd, stts, h, 24)) return 0;
    if (be(h + 12) != 1 || be(h + 20) != 1) return 0;
    total = be(h + 16);
    if (!child(fd, stbl + 8, stbl + stbl_size, TAG('s', 't', 's', 'c'), &stsc, &size) || !read_at(fd, stsc, h, 40 < size ? 40 : size)) return 0;
    if (be(h + 12) < 1 || be(h + 12) > 2 || be(h + 16) != 1) return 0;
    per_chunk = be(h + 20);
    if (!child(fd, stbl + 8, stbl + stbl_size, TAG('s', 't', 's', 'z'), &stsz, &size) || !read_at(fd, stsz, o, 20)) return 0;
    rest = be(o + 12);
    if (!rest || be(o + 16) != total) return 0;
    if (!child(fd, stbl + 8, stbl + stbl_size, TAG('c', 'o', '6', '4'), &co64, &size) || !read_at(fd, co64, o, 16)) return 0;
    have = be(o + 12);
    if (first >= have) return 0;
    keep = have - first < chunks ? have - first : chunks;
    if (keep > 32) return 0;
    *samples = tail_cut && keep < have - first ? keep * per_chunk : total - first * per_chunk;

    b = p; p = open_full(p, TAG('s', 't', 't', 's')); p = put(p, 1); p = put(p, *samples); p = put(p, 1); p = close_box(b, p);
    b = p; p = open_full(p, TAG('s', 't', 's', 'c'));
    if (*samples % per_chunk && keep > 1) {
        p = put(p, 2); p = put(p, 1); p = put(p, per_chunk); p = put(p, 1);
        p = put(p, keep); p = put(p, *samples % per_chunk); p = put(p, 1);
    } else {
        p = put(p, 1); p = put(p, 1); p = put(p, keep > 1 ? per_chunk : *samples); p = put(p, 1);
    }
    p = close_box(b, p);
    b = p; p = open_full(p, TAG('s', 't', 's', 'z')); p = put(p, rest); p = put(p, *samples); p = close_box(b, p);
    b = p; p = open_full(p, TAG('c', 'o', '6', '4')); p = put(p, keep);
    if (!read_at(fd, co64 + 16 + first * 8ul, p, keep * 8)) return 0;
    p = close_box(b, p + keep * 8);
    rest = stbl + stbl_size - stts;
    if (p - o > rest || (rest - (p - o) && rest - (p - o) < 8)) return 0;
    if (rest - (p - o)) { b = p; p = open_box(p, TAG('f', 'r', 'e', 'e')); p = zero(p, rest - (b - o) - 8); p = close_box(b, p); }
    *at = stts;
    return p - o;
}

static void live_run(int fd)
{
    u8 *buf = WORK, *tables = WORK + 0x1000, h[4], h8[8], *p;
    u32 orient;
    char id[36];
    u32 press, start, end, table_len, duration, stbl_size, audio_len = 0, samples = 0, trak_size;
    u64 audio_trak, audio_at = 0, audio_mdhd = 0, audio_tkhd = 0;
    u64 elst, edts;
    u64 moov = 24, free_at;
    u32 msize, fsize, size, movie_scale, media_scale, next, video, frames, delta, n, sample_pos, left;
    u64 trak, mvhd, tkhd, mdia, mdhd, minf, stbl, stts, udta, ncdt, ncvw;

    MARK(2);
    if (!read_at(fd, 0, buf, 32) || be(buf + 4) != TAG('f', 't', 'y', 'p') || be(buf) != 24) { MARK(12); return; }
    if (be(buf + 28) != TAG('m', 'o', 'o', 'v')) { MARK(13); return; }
    msize = be(buf + 24);
    free_at = moov + msize;
    if (!read_at(fd, free_at, buf, 8) || be(buf + 4) != TAG('f', 'r', 'e', 'e')) { MARK(14); return; }
    fsize = be(buf);
    if (!child(fd, moov + 8, free_at, TAG('m', 'v', 'h', 'd'), &mvhd, &size)) { MARK(15); return; }
    if (!read_at(fd, mvhd, buf, 112)) { MARK(16); return; }
    movie_scale = be(buf + 20); next = be(buf + 104);
    if (!child(fd, moov + 8, free_at, TAG('t', 'r', 'a', 'k'), &trak, &n)) { MARK(17); return; }
    trak_size = n;
    if (!child(fd, trak + 8, trak + n, TAG('t', 'k', 'h', 'd'), &tkhd, &size) || !read_at(fd, tkhd, buf, 24)) { MARK(18); return; }
    video = be(buf + 20);
    // tkhd v0: the display matrix starts at +48.
    orient = read_at(fd, tkhd + 48, h8, 8) ? orientation(be(h8), be(h8 + 4)) : 1;
    if (!child(fd, trak + 8, trak + n, TAG('e', 'd', 't', 's'), &edts, &size) ||
        !child(fd, edts + 8, edts + size, TAG('e', 'l', 's', 't'), &elst, &size)) { MARK(37); return; }
    if (!child(fd, trak + 8, trak + n, TAG('m', 'd', 'i', 'a'), &mdia, &n)) { MARK(19); return; }
    if (!child(fd, mdia + 8, mdia + n, TAG('m', 'd', 'h', 'd'), &mdhd, &size) || !read_at(fd, mdhd, buf, 24)) { MARK(20); return; }
    media_scale = be(buf + 20);
    if (!child(fd, mdia + 8, mdia + n, TAG('m', 'i', 'n', 'f'), &minf, &n)) { MARK(21); return; }
    if (!child(fd, minf + 8, minf + n, TAG('s', 't', 'b', 'l'), &stbl, &n)) { MARK(22); return; }
    stbl_size = n;
    if (!child(fd, stbl + 8, stbl + n, TAG('s', 't', 't', 's'), &stts, &size) || !read_at(fd, stts, buf, 24)) { MARK(23); return; }
    if (be(buf + 12) != 1) { MARK(24); return; }
    frames = be(buf + 16); delta = be(buf + 20);
    if (!child(fd, moov + 8, free_at, TAG('u', 'd', 't', 'a'), &udta, &n)) { MARK(25); return; }
    if (!child(fd, udta + 8, udta + n, TAG('N', 'C', 'D', 'T'), &ncdt, &n)) { MARK(26); return; }
    if (!child(fd, ncdt + 8, ncdt + n, TAG('N', 'C', 'V', 'W'), &ncvw, &size)) { MARK(27); return; }

    press = PRESS[1] - DROPPED;
    if (press >= frames) press = frames - 1;
    start = (press > PRE_FRAMES ? press - PRE_FRAMES : 0) / GOP * GOP;
    // Ends on a batch boundary, where the audio chunks end too.
    end = (press + POST_FRAMES + GOP) / GOP * GOP;
    if (end > frames) end = frames;
    table_len = 0;
    if (start || end != frames) {
        table_len = trimmed_tables(fd, tables, stbl, stbl_size, stts, delta, start, end);
        if (!table_len) { MARK(34); return; }
        if (child(fd, trak + trak_size, free_at, TAG('t', 'r', 'a', 'k'), &audio_trak, &n)) {
            audio_len = audio_tables(fd, WORK + 0x3000, audio_trak, n, start / GOP, (end - 1) / GOP - start / GOP + 1,
                                     end != frames, &audio_at, &audio_mdhd, &audio_tkhd, &samples);
            if (!audio_len) { MARK(34); return; }
        }
    }
    duration = (u64)(end - start) * delta * movie_scale / media_scale;

    MARK(3);
    text_id(id);
    n = atoms(buf, id, 36, movie_scale, media_scale, delta, press - start, 0, video, next) - buf;
    if (fsize < n + SAMPLE_BOX + 8) { MARK(28); return; }
    // Still first: if it cannot be written, the MOV stays an ordinary recording.
    if (!write_still(fd, ncvw, size, id, 36, orient, buf, 0x1000)) { MARK(29); return; }

    MARK(4);
    sample_pos = free_at + n + 8;
    n = atoms(buf, id, 36, movie_scale, media_scale, delta, press - start, sample_pos, video, next) - buf;
    p = buf + n;
    p = put(p, SAMPLE_BOX); p = put(p, TAG('m', 'd', 'a', 't'));
    p = put(p, 9); p = put(p, 1); *p++ = 0xff;
    left = fsize - n - SAMPLE_BOX;
    p = put(p, left); p = put(p, TAG('f', 'r', 'e', 'e'));
    if (table_len) {
        u8 d[4];
        put(d, duration);
        if (!write_at(fd, stts, tables, table_len)) { MARK(35); return; }
        put(h, (u64)(end - start) * delta);
        if (!write_at(fd, mdhd + 24, h, 4) || !write_at(fd, mvhd + 24, d, 4) || !write_at(fd, tkhd + 28, d, 4) ||
            !write_at(fd, elst + 16, d, 4)) { MARK(36); return; }
        if (audio_len) {
            u8 m[4];
            if (!write_at(fd, audio_at, WORK + 0x3000, audio_len)) { MARK(35); return; }
            put(m, samples);
            put(d, (u64)samples * movie_scale / 48000);
            if (!write_at(fd, audio_mdhd + 24, m, 4) || !write_at(fd, audio_tkhd + 28, d, 4)) { MARK(36); return; }
        }
    }
    if (!write_at(fd, free_at, buf, p - buf)) { MARK(30); return; }
    put(buf, next + 1);
    if (!write_at(fd, mvhd + 104, buf, 4)) { MARK(31); return; }
    put(buf, msize + n);
    if (!write_at(fd, moov, buf, 4)) { MARK(32); return; }
    MARK(5);
}

void live_press(void)
{
    u64 now;
    if (RECORDER_STATE != 3 || PRESS[0] == PRESS_MAGIC) return;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
    PRESS[1] = PICTURES;
    PRESS[2] = now;
    PRESS[0] = PRESS_MAGIC;
}

// Same decoded input the MCU path hands over, with only the movie key changed.
static void movie_key(u32 down)
{
    u32 keys[11], i;
    for (i = 0; i < 11; i++) keys[i] = KEY_STATE[i];
    keys[1] = down ? keys[1] | SW1_MOVIE : keys[1] & ~SW1_MOVIE;
    KEY_CONSUMER(keys);
}

// Runs once per picture handed to the encoder. Enough pictures after the press: press the
// movie key once, then let go. The close in live_main lets go if the pictures end first.
void live_picture(void)
{
    u64 n;
    if (RECORDER_STATE != 3) return;
    n = PICTURES++;
    if (PRESS[0] != PRESS_MAGIC) return;
    if (MODE != MODE_ON) return;
    if (!STOP_STAGE && n + 1 >= (PRESS[1] + POST_FRAMES + GOP) / GOP * GOP) {
        STOP_STAGE = n + 1;
        movie_key(1);
        MARKV(38, n);
    } else if (STOP_STAGE > 1 && n >= STOP_STAGE + KEY_HOLD) {
        STOP_STAGE = 1;
        movie_key(0);
    }
}

static int holding(void)
{
    return MODE == MODE_ON && PRESS[0] != PRESS_MAGIC;
}

// A file named LIVE.TXT in the root of the card selects Live recording; otherwise every hook
// stays out of the way and the recording is the firmware's own.
static void read_mode(void)
{
    char path[0x20];
    int fd;
#ifdef FLAG_TEST_JPG
    static const char name[] = "DCIM\\100NCZ_8\\DSC_0001.JPG";
#else
    static const char name[] = "LIVE.TXT";
#endif
    copy((u8 *)path, SAVED_PATH, 3);
    copy((u8 *)path + 3, name, sizeof(name));
    fd = FS_OPEN(path, 0);
    if (fd >= 0) FS_CLOSE(fd);
    MODE = fd >= 0 ? MODE_ON : MODE_OFF;
    MARKV(39, MODE);
}

// Before the file service takes a batch: while holding, a short queue waits.
u32 live_gate_take(u32 take)
{
    if (!MODE) read_mode();
    if (take && holding() && RECORDER_STATE == 3 && QUEUE_COUNT(VIDEO_QUEUE, 0) <= HOLD_BATCHES) return 0;
    return take;
}

// With a batch taken: nonzero sends it down the firmware's own drop path, which releases it
// exactly as a finished write does. The sequence header rides on the first batch only.
u32 live_gate_batch(u8 *batch)
{
    u32 i;
    if (holding()) {
        if (batch[BATCH_HEAD] && !HEAD_SAVED) {
            copy(SAVED_HEAD, batch + BATCH_HEAD, BATCH_END - BATCH_HEAD);
            HEAD_SAVED = 1;
        }
        DROPPED += *(u32 *)(batch + BATCH_FRAMES);
        MARKV(11, DROPPED);
        return 1;
    }
    if (HEAD_SAVED == 1) {
        HEAD_SAVED = 2;
        if (!batch[BATCH_HEAD])
            for (i = 0; i < BATCH_END - BATCH_HEAD; i++) batch[BATCH_HEAD + i] = SAVED_HEAD[i];
    }
    return batch[0];
}

void live_main(void)
{
    char path[0x80];
    u32 i;
    int fd;

    MARK(1);
    if (STOP_STAGE > 1) movie_key(0);
    if (MODE != MODE_ON) goto done;
    // Keep the path on this task's stack, as the firmware's own callers do.
    for (i = 0; i < sizeof(path); i++) path[i] = SAVED_PATH[i];
    MARK(7);
    fd = FS_OPEN(path, 2);
    MARK(6);
    if (fd >= 0) {
        if (PRESS[0] == PRESS_MAGIC) live_run(fd);
        FS_CLOSE(fd);
    }
    // Nothing was kept: no press means no file.
    if (PRESS[0] != PRESS_MAGIC) MARKV(33, FS_UNLINK(path));
done:
    PRESS[0] = 0;
    MODE = 0;
    PICTURES = 0;
    STOP_STAGE = 0;
    DROPPED = 0;
    HEAD_SAVED = 0;
}

__asm__(
    ".section .text.entry\n"
    ".global live_entry\n"
    "live_entry:\n"
    "  stp x19, x30, [sp, #-16]!\n"
    "  movz x16, #0xaac4\n"
    "  movk x16, #0x4034, lsl #16\n"
    "  blr x16\n"
    "  mov w19, w0\n"
    "  bl live_main\n"
    "  mov w0, w19\n"
    "  ldp x19, x30, [sp], #16\n"
    "  ret\n"
    ".balign 4\n"
    ".global live_marks\n"
    "live_marks:\n"
    ".rept 40\n"
    "  ret\n"
    ".endr\n"
    ".global live_press_entry\n"
    "live_press_entry:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  stp x0, x1, [sp, #-16]!\n"
    "  stp x2, x3, [sp, #-16]!\n"
    "  stp x4, x5, [sp, #-16]!\n"
    "  stp x6, x7, [sp, #-16]!\n"
    "  bl live_press\n"
    "  ldp x6, x7, [sp], #16\n"
    "  ldp x4, x5, [sp], #16\n"
    "  ldp x2, x3, [sp], #16\n"
    "  ldp x0, x1, [sp], #16\n"
    "  ldp x29, x30, [sp], #16\n"
    "  str x30, [sp, #-16]!\n"
    "  movz x16, #0xc9cc\n"
    "  movk x16, #0x40fd, lsl #16\n"
    "  br x16\n"
    ".global live_picture_entry\n"
    "live_picture_entry:\n"
    "  str x30, [sp, #-16]!\n"
    "  bl live_picture\n"
    "  ldr x30, [sp], #16\n"
    "  mov w1, #0x100\n"
    "  ret\n"
    ".macro save_all\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  stp x0, x1, [sp, #-16]!\n"
    "  stp x2, x3, [sp, #-16]!\n"
    "  stp x4, x5, [sp, #-16]!\n"
    "  stp x6, x7, [sp, #-16]!\n"
    "  stp x9, x10, [sp, #-16]!\n"
    "  stp x11, x12, [sp, #-16]!\n"
    "  stp x13, x14, [sp, #-16]!\n"
    "  stp x15, x16, [sp, #-16]!\n"
    "  stp x17, x18, [sp, #-16]!\n"
    ".endm\n"
    ".macro load_all\n"
    "  ldp x17, x18, [sp], #16\n"
    "  ldp x15, x16, [sp], #16\n"
    "  ldp x13, x14, [sp], #16\n"
    "  ldp x11, x12, [sp], #16\n"
    "  ldp x9, x10, [sp], #16\n"
    "  ldp x6, x7, [sp], #16\n"
    "  ldp x4, x5, [sp], #16\n"
    "  ldp x2, x3, [sp], #16\n"
    "  ldp x0, x1, [sp], #16\n"
    "  ldp x29, x30, [sp], #16\n"
    ".endm\n"
    ".balign 128\n"
    ".global live_take_entry\n"
    "live_take_entry:\n"
    "  save_all\n"
    "  mov w0, w8\n"
    "  bl live_gate_take\n"
    "  mov w8, w0\n"
    "  load_all\n"
    "  ldr w9, [x9]\n"
    "  ret\n"
    ".balign 128\n"
    ".global live_batch_entry\n"
    "live_batch_entry:\n"
    "  save_all\n"
    "  mov x0, x22\n"
    "  bl live_gate_batch\n"
    "  mov w8, w0\n"
    "  load_all\n"
    "  ret\n"
    ".text\n"
    );
