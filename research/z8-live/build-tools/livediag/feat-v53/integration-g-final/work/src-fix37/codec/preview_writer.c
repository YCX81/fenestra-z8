/* v37 in-camera preview: baseline JPEG pictures in a trailing 'free' atom of the
 * Live MOV (layout in preview_writer.h). Integer only, no allocation, no statics
 * that are written; the caller supplies every buffer. */
#include "preview_writer.h"
static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { le16(p, v); le16(p + 2, v >> 16); }
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
    uint16_t q[2][64]; /* divisor per coefficient, already times 8 (the FDCT gain) */
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
/* jfdctint-style 1-D pass over 8 values `step` apart; results carry 2^up. */
static void fdct_pass(int32_t *p, int step, int up, int down) {
    int32_t t0 = p[0] + p[7 * step], t7 = p[0] - p[7 * step], t1 = p[step] + p[6 * step], t6 = p[step] - p[6 * step];
    int32_t t2 = p[2 * step] + p[5 * step], t5 = p[2 * step] - p[5 * step], t3 = p[3 * step] + p[4 * step],
            t4 = p[3 * step] - p[4 * step];
    int32_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
    if (up) { p[0] = (t10 + t11) * (1 << up); p[4 * step] = (t10 - t11) * (1 << up); }
    else { p[0] = DESCALE(t10 + t11, 2); p[4 * step] = DESCALE(t10 - t11, 2); }
    int32_t z1 = MUL(t12 + t13, 4433);
    p[2 * step] = DESCALE(z1 + MUL(t13, 6270), down);
    p[6 * step] = DESCALE(z1 - MUL(t12, 15137), down);
    int32_t z2, z3, z4, z5;
    z1 = t4 + t7; z2 = t5 + t6; z3 = t4 + t6; z4 = t5 + t7; z5 = MUL(z3 + z4, 9633);
    t4 = MUL(t4, 2446); t5 = MUL(t5, 16819); t6 = MUL(t6, 25172); t7 = MUL(t7, 12299);
    z1 = MUL(z1, -7373); z2 = MUL(z2, -20995); z3 = MUL(z3, -16069) + z5; z4 = MUL(z4, -3196) + z5;
    p[7 * step] = DESCALE(t4 + z1 + z3, down);
    p[5 * step] = DESCALE(t5 + z2 + z4, down);
    p[3 * step] = DESCALE(t6 + z2 + z3, down);
    p[step] = DESCALE(t7 + z1 + z4, down);
}
static unsigned magnitude(int32_t v) {
    unsigned n = 0, a = (unsigned)(v < 0 ? -v : v);
    while (a) { n++; a >>= 1; }
    return n;
}
/* src: top-left sample of the block inside a plane of w x h (edges replicate). */
static void block(encoder *e, const uint8_t *plane, unsigned w, unsigned h, unsigned x0, unsigned y0, unsigned table,
                  int *dc) {
    int32_t d[64];
    for (unsigned y = 0; y < 8; y++) {
        unsigned yy = y0 + y < h ? y0 + y : h - 1u;
        unsigned div=table?2u:1u,sw=e->source_w/div,sh=e->source_h/div;
        unsigned vdiv=table?e->chroma_v:1u;
        unsigned fw=e->fit_w/div,fh=e->fit_h/vdiv,left=e->left/div,top=e->top/vdiv;
        if(sw==w&&sh==h&&fw==w&&fh==h&&!left&&!top){
            /* Keep v47's ordinary preview path free of per-pixel divisions. */
            const uint8_t *row=plane+(size_t)yy*w;
            for(unsigned x=0;x<8;x++)d[y*8+x]=(int32_t)row[x0+x<w?x0+x:w-1u]-128;
        }else for (unsigned x = 0; x < 8; x++) {
            unsigned xx=x0+x<w?x0+x:w-1u,v=table?128u:0u;
            if(xx>=left&&xx-left<fw&&yy>=top&&yy-top<fh)
                v=plane[(size_t)((yy-top)*sh/fh)*sw+(xx-left)*sw/fw];
            d[y*8+x]=(int32_t)v-128;
        }
    }
    for (unsigned i = 0; i < 8; i++) fdct_pass(d + i * 8, 1, 2, 11);
    for (unsigned i = 0; i < 8; i++) fdct_pass(d + i, 8, 0, 15);
    int32_t prev = *dc;
    unsigned run = 0;
    for (unsigned k = 0; k < 64; k++) {
        unsigned n = zigzag[k];
        int32_t q = e->q[table][n], v = d[n];
        v = v < 0 ? -((-v + q / 2) / q) : (v + q / 2) / q;
        if (!k) {
            int32_t diff = v - prev;
            *dc = v;
            unsigned s = magnitude(diff);
            put_bits(e, e->dc_code[s], e->dc_size[s]);
            if (s) put_bits(e, (uint32_t)(diff < 0 ? diff - 1 : diff), (int)s);
            continue;
        }
        if (!v) { run++; continue; }
        while (run > 15) { put_bits(e, e->ac_code[0xf0], e->ac_size[0xf0]); run -= 16; }
        unsigned s = magnitude(v);
        if (s > 10) { s = 10; v = v < 0 ? -1023 : 1023; }
        put_bits(e, e->ac_code[run << 4 | s], e->ac_size[run << 4 | s]);
        put_bits(e, (uint32_t)(v < 0 ? v - 1 : v), (int)s);
        run = 0;
    }
    if (run) put_bits(e, e->ac_code[0], e->ac_size[0]);
}
static void marker(encoder *e, unsigned m, unsigned payload) {
    byte(e, 255); byte(e, m); byte(e, (payload + 2) >> 8); byte(e, payload + 2);
}
static int jpeg_scaled(const LivePreviewWriter *w, const uint8_t *i420, unsigned source_w, unsigned source_h,
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
            e.q[t][i] = (uint16_t)(v * 8u);
        }
        for (unsigned i = 0; i < 64; i++) byte(&e, table[zigzag[i]]);
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
    for (unsigned my = 0; my < height && !e.failed; my += 8u*chroma_v) {
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
        if (w->yield && (my / (8u*chroma_v)) % 4u == 3u) w->yield(w->ctx); /* v47: every 4 MCU rows (was every row) */
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
__attribute__((minsize)) int live_preview_write(const LivePreviewWriter *w, void *scratch, size_t bytes, uint8_t *io, size_t io_bytes,
                       uint64_t base, uint64_t *end, uint32_t audio_offset, uint32_t audio_frames, uint32_t duration_us) {
    static const uint8_t ladder[] = {45, 30, 20, 12, 6, 2};
    unsigned level = 0; /* stays coarser once a picture came out large (noisy scenes) */
    uint8_t h[8 + LIVE_PREVIEW_HEADER] = {0};
    /* LVJ1 stores a uniform integer interval. Round up so its image EOS
     * never precedes the actual clip; the excess is less than 45 us. */
    uint32_t frame_us = (uint32_t)(((uint64_t)duration_us + LIVE_PREVIEW_FRAMES - 1u) / LIVE_PREVIEW_FRAMES);
    if (!w || !w->frame || !w->write_at || !scratch || bytes < LIVE_PREVIEW_FRAME_BYTES || !io || io_bytes < 4096 ||
        !end || base > 0xffffffffu || frame_us < 33333u || frame_us > 4000000u / LIVE_PREVIEW_FRAMES)
        return -1;
    uint64_t at = base + sizeof h, high = 0;
    for (uint32_t i = 0; i < LIVE_PREVIEW_FRAMES; i++) {
        uint32_t n = 0;
        int r = -1;
        if (w->frame(w->ctx, i, scratch, LIVE_PREVIEW_FRAME_BYTES)) return -1;
        for (unsigned k = level; k < sizeof ladder; k++) {
            r = live_preview_jpeg(w, scratch, LIVE_PREVIEW_WIDTH, LIVE_PREVIEW_HEIGHT, ladder[k], io, io_bytes, at,
                                  LIVE_PREVIEW_JPEG_MAX, &n);
            if (at + n > high) high = at + n;
            if (r != 1) break; /* too large: same offset again, coarser */
        }
        if (r) return -1;
        if (n > LIVE_PREVIEW_JPEG_SOFT && level + 1 < sizeof ladder) level++;
        le32(h + 8 + 24 + 4 * i, n);
        at += n;
    }
    /* Keep the audio record immediately after the indexed JPEGs. Oversized
     * retry residue is padding AFTER that record, before the trailer. */
    if (audio_offset && audio_frames) {
        uint8_t a[16] = {'L', 'V', 'J', 'B'}; /* v50 LE24 track */
        le32(a + 4, audio_offset); le32(a + 8, audio_frames); le32(a + 12, 48000);
        if (w->write_at(w->ctx, at, a, sizeof a)) return -1;
        at += sizeof a;
    }
    if (high > at) at = high; /* cover any oversized JPEG retry residue */
    uint64_t total = at + 8 - base;
    if (total > 0x7fffffffu || at + 8 > 0xffffffffu) return -1;
    be32(h, (uint32_t)total);
    h[4] = 'f'; h[5] = 'r'; h[6] = 'e'; h[7] = 'e';
    h[8] = 'L'; h[9] = 'V'; h[10] = 'J'; h[11] = '1';
    le16(h + 12, 1); le16(h + 14, LIVE_PREVIEW_HEADER);
    le16(h + 16, LIVE_PREVIEW_WIDTH); le16(h + 18, LIVE_PREVIEW_HEIGHT);
    le32(h + 20, LIVE_PREVIEW_FRAMES); le32(h + 24, frame_us);
    uint8_t tail[8];
    le32(tail, (uint32_t)total);
    tail[4] = 'L'; tail[5] = 'V'; tail[6] = 'J'; tail[7] = 'T';
    /* Trailer first, header last: the atom only becomes well formed when all of it is on the card. */
    if (w->write_at(w->ctx, at, tail, sizeof tail) || w->write_at(w->ctx, base, h, sizeof h)) return -1;
    *end = at + 8;
    return 0;
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
 p=ntag(p,0x18,3,1);p=nshort(p,1); /* native generator 0x406d2a60 always writes 1 */
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
