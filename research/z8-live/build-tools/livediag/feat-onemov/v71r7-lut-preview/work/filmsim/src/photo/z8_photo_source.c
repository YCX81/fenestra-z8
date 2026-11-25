#include "z8_photo_source.h"
#include <string.h>

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;
}
static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

int z8_photo_decode(const void *bytes, size_t n, struct z8_photo_descriptor *d) {
    const uint8_t *p = (const uint8_t *)bytes;
    if (!p || !d || n < Z8_PHOTO_DESCRIPTOR_BYTES) return 0;
    d->format = le32(p); d->layout = le32(p + 4);
    d->y_stride = le16(p + 8); d->uv_stride = le16(p + 10);
    d->storage_width = le16(p + 12); d->storage_height = le16(p + 14);
    d->width = le16(p + 16); d->height = le16(p + 18);
    d->origin_x = le16(p + 20); d->origin_y = le16(p + 22);
    d->y_address = le64(p + 24); d->uv_address = le64(p + 32);
    return 1;
}

enum z8_photo_layout_status z8_photo_nv16_spans(
    const struct z8_photo_descriptor *d, uint64_t *yb, uint64_t *cb) {
    uint64_t y, c;
    /* HW stores (address >> 4) in 32 bits. Never silently truncate high bits. */
    const uint64_t limit = UINT64_C(1) << 36;
    if (!d || !yb || !cb) return Z8_PHOTO_LAYOUT_BAD_ARGUMENT;
    *yb = *cb = 0;
    if (d->format != 1 || d->layout != 0 || d->origin_x || d->origin_y)
        return Z8_PHOTO_LAYOUT_UNSUPPORTED;
    if (!d->width || !d->height || (d->width & 1) ||
        d->width > d->storage_width || d->height > d->storage_height ||
        d->storage_width > d->y_stride || d->storage_width > d->uv_stride ||
        (d->y_stride & 1) || (d->uv_stride & 1))
        return Z8_PHOTO_LAYOUT_BAD_GEOMETRY;
    y = (uint64_t)d->y_stride * d->storage_height;
    c = (uint64_t)d->uv_stride * d->storage_height;
    if (!d->y_address || !d->uv_address ||
        (d->y_address & 15) || (d->uv_address & 15) ||
        d->y_address >= limit || d->uv_address >= limit ||
        y > limit - d->y_address || c > limit - d->uv_address)
        return Z8_PHOTO_LAYOUT_BAD_ADDRESS;
    if (d->y_address < d->uv_address + c &&
        d->uv_address < d->y_address + y)
        return Z8_PHOTO_LAYOUT_OVERLAP;
    *yb = y; *cb = c;
    return Z8_PHOTO_LAYOUT_OK;
}

static int read_at(z8_photo_read_fn rd, void *o, uint64_t base, uint64_t off,
                   void *dst, size_t n) {
    if (!base || off > UINT64_MAX - base ||
        n > UINT64_MAX - (base + off)) return 0;
    return rd(o, base + off, dst, n) != 0;
}
static int read32(z8_photo_read_fn rd, void *o, uint64_t base, uint64_t off,
                  uint32_t *out) {
    uint8_t b[4];
    if (!read_at(rd, o, base, off, b, sizeof b)) return 0;
    *out = le32(b); return 1;
}
static int read64(z8_photo_read_fn rd, void *o, uint64_t base, uint64_t off,
                  uint64_t *out) {
    uint8_t b[8];
    if (!read_at(rd, o, base, off, b, sizeof b)) return 0;
    *out = le64(b); return 1;
}

static int snapshot_once(z8_photo_read_fn rd, void *o, uint64_t context,
                         uint64_t outer, struct z8_photo_snapshot *s) {
    static const uint16_t outer_offsets[6] = {0x688,0x690,0x698,0x6a0,0x6a0,0x6a0};
    static const uint8_t desc_offsets[6] = {0x30,0x30,0x30,0x20,0x48,0x70};
    static const uint8_t flag_offsets[6] = {0x28,0x28,0x28,0x18,0x19,0x1a};
    unsigned i;
    memset(s, 0, sizeof *s);
    s->context = context;
    s->outer = outer;
    if (context && (!read64(rd,o,context,0x18,&s->outer) ||
                    !read32(rd,o,context,0,&s->context_word))) return 0;
    if (!read32(rd,o,s->outer,0x10,&s->outer_word10) ||
        !read64(rd,o,s->outer,0x790,&s->metadata) ||
        !read64(rd,o,s->outer,0x6a8,&s->output) ||
        !read32(rd,o,s->metadata,0,&s->mode) ||
        !read32(rd,o,s->metadata,0x294,&s->quality) ||
        !read32(rd,o,s->metadata,0x2fc,&s->container)) return 0;
    for (i = 0; i < Z8_PHOTO_SOURCE_COUNT; ++i) {
        struct z8_photo_source_snapshot *p = &s->sources[i];
        if (!read64(rd,o,s->outer,outer_offsets[i],&p->resource)) return 0;
        if (!p->resource) continue;
        if (p->resource > UINT64_MAX - desc_offsets[i]) return 0;
        p->descriptor_address = p->resource + desc_offsets[i];
        if (!read_at(rd,o,p->resource,desc_offsets[i],p->raw,sizeof p->raw) ||
            !read_at(rd,o,p->resource,flag_offsets[i],&p->firmware_done_flag,1))
            return 0;
        p->present = 1;
        if (!z8_photo_decode(p->raw,sizeof p->raw,&p->decoded)) return 0;
    }
    return 1;
}

static int snapshot_twice(z8_photo_read_fn rd, void *o, uint64_t context,
                         uint64_t outer, struct z8_photo_snapshot *s) {
    struct z8_photo_snapshot second;
    unsigned i;
    if (!rd || !s || (!context && !outer)) return 0;
    memset(s,0,sizeof *s);
    if (!snapshot_once(rd,o,context,outer,s) || !snapshot_once(rd,o,context,outer,&second))
        return 0;
    if (memcmp(s,&second,sizeof *s)) return 0;
    s->stable = 1;
    for (i=0;i<Z8_PHOTO_SOURCE_COUNT;++i) s->sources[i].stable = 1;
    return 1;
}

int z8_photo_snapshot_read(z8_photo_read_fn rd, void *o, uint64_t context,
                           struct z8_photo_snapshot *s) {
    return snapshot_twice(rd,o,context,0,s);
}

int z8_photo_snapshot_outer_read(z8_photo_read_fn rd, void *o, uint64_t outer,
                                 struct z8_photo_snapshot *s) {
    return snapshot_twice(rd,o,0,outer,s);
}
