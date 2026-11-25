/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PS1 NEFX writer.
 *
 * Same rules as research/pixelshift_review_20261022/realshot/nefx_writer.py, whose files NX Studio 1.11
 * opened and exported (A and D of nefx_test_20261022). The output is the first frame's NEF with:
 *   - every out-of-line value on a 16-byte file boundary, values in tag order; blocks in the order
 *     IFD0, Exif (MakerNote as one value), MakerNote preview IFD + JPEG, GPS, IFD0 thumbnail,
 *     SubIFD0..n with their values, then the SubIFD data in the source order (the raw strip last);
 *   - IFD0 Software trimmed of its trailing NULs (DateTime and everything else kept);
 *   - MakerNote: 0x0058, 0x0096 dropped; 0x0045 = (0, 0, W, H); 0x0051 byte 10: 3 -> 10; 0x0054
 *     numerators x scale; 0x00B7 bytes 62..79 cleared at scale 2; 0x0E2D added (mode 1-4 for 4, 8, 16, 32
 *     frames, PS6: the camera's 0x005E mode code, as NX Studio 1.11 writes it);
 *     0x0E2E added (640 x 424 x 3 floor block means, integer partition i * H / 424);
 *   - NEFInfo (raw SubIFD 0xC7D5): 0x07, 0x0F dropped; 0x08 x scale, 0x0B = (0, 0, W, H),
 *     0x0D byte 4: 3 -> 10, each with its CRC-16 (0xFFFF, 0x1021);
 *   - raw SubIFD: W x H x 3 samples, 14 bits each, little-endian bit stream, photometric 2.
 * Positions are fixed by a layout pass; the emit pass repeats it and checks every position.
 */
#include "nefx.h"

enum { K_INLINE, K_SRC, K_LOCAL, K_MAP, K_MN, K_NI, K_SUBS };

static const uint8_t nx_unit[14] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};
static const uint8_t nx_mn_head[18] = {'N','i','k','o','n',0,2,0x11,0,0,'I','I',42,0,8,0,0,0};
static const uint8_t nx_ni_head[18] = {'N','i','k','o','n',0,1,3,0,0,'I','I',42,0,8,0,0,0};

static uint32_t nx_get16(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t nx_get32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void nx_put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void nx_put32(uint8_t *p, uint32_t v)
{
    for (uint32_t i = 0; i < 4; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void nx_copy(void *d, const void *s, uint32_t n)
{
    uint8_t *a = d;
    const uint8_t *b = s;
    while (n--)
        *a++ = *b++;
}

static void nx_clear(void *d, uint32_t n)
{
    uint8_t *a = d;
    while (n--)
        *a++ = 0;
}

static int nx_same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    while (n--) {
        if (*a++ != *b++) return 0;
    }
    return 1;
}

static uint32_t nx_crc16(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFu;
    while (n--) {
        c ^= (uint32_t)*p++ << 8;
        for (uint32_t k = 0; k < 8; ++k)
            c = (c & 0x8000u) ? ((c << 1) ^ 0x1021u) & 0xFFFFu : (c << 1) & 0xFFFFu;
    }
    return c;
}

/* Metadata reads of the first frame, through a small aligned window. */
static int nx_read(nefx *x, uint32_t offset, void *out, uint32_t n)
{
    uint8_t *d = out;
    if (offset > x->src.size || n > x->src.size - offset)
        return PSM_BAD_NEF;
    while (n) {
        uint32_t avail;
        if (!x->md_bytes || offset < x->md_offset || offset - x->md_offset >= x->md_bytes) {
            x->md_offset = offset & ~(NX_MDCACHE - 1u);
            x->md_bytes = x->src.size - x->md_offset;
            if (x->md_bytes > NX_MDCACHE) x->md_bytes = NX_MDCACHE;
            if (x->src.read_at(x->src.context, x->md_offset, x->md, x->md_bytes)) {
                x->md_bytes = 0;
                return PSM_READ_ERROR;
            }
        }
        avail = x->md_bytes - (offset - x->md_offset);
        if (avail > n) avail = n;
        nx_copy(d, x->md + (offset - x->md_offset), avail);
        d += avail;
        offset += avail;
        n -= avail;
    }
    return PSM_OK;
}

static uint8_t *nx_alloc(nefx *x, uint32_t n)
{
    uint8_t *p;
    n = (n + 3u) & ~3u;
    if (n > NX_ARENA - x->arena_used) return 0;
    p = x->arena + x->arena_used;
    x->arena_used += n;
    nx_clear(p, n);
    return p;
}

static nx_tag *nx_find(nx_ifd *ifd, uint32_t tag)
{
    for (uint32_t i = 0; i < ifd->n; ++i) {
        if (ifd->t[i].tag == tag) return &ifd->t[i];
    }
    return 0;
}

static void nx_drop(nx_ifd *ifd, uint32_t tag)
{
    for (uint32_t i = 0; i < ifd->n; ++i) {
        if (ifd->t[i].tag != tag) continue;
        for (uint32_t k = i + 1; k < ifd->n; ++k)
            ifd->t[k - 1] = ifd->t[k];
        --ifd->n;
        return;
    }
}

/* Find or insert (tag order) an entry; the caller fills its value. */
static nx_tag *nx_set(nx_ifd *ifd, uint32_t tag, uint32_t type, uint32_t count)
{
    nx_tag *t = nx_find(ifd, tag);
    if (!t) {
        uint32_t i = ifd->n;
        if (ifd->n == ifd->cap) return 0;
        while (i && ifd->t[i - 1].tag > tag) {
            ifd->t[i] = ifd->t[i - 1];
            --i;
        }
        ++ifd->n;
        t = &ifd->t[i];
    }
    nx_clear(t, sizeof(*t));
    t->tag = (uint16_t)tag;
    t->type = (uint16_t)type;
    t->count = count;
    t->bytes = count * nx_unit[type];
    t->kind = K_INLINE;
    return t;
}

static int nx_set_inline(nx_ifd *ifd, uint32_t tag, uint32_t type, uint32_t value)
{
    nx_tag *t = nx_set(ifd, tag, type, 1);
    if (!t) return PSM_UNSUPPORTED_NEF;
    if (type == 3) nx_put16(t->inl, value);
    else nx_put32(t->inl, value);
    return PSM_OK;
}

static int nx_set_bytes(nefx *x, nx_ifd *ifd, uint32_t tag, uint32_t type, uint32_t count,
                        const uint8_t *value, uint32_t kind)
{
    nx_tag *t = nx_set(ifd, tag, type, count);
    uint8_t *p;
    if (!t) return PSM_UNSUPPORTED_NEF;
    if (t->bytes <= 4 && kind == K_LOCAL) {
        nx_copy(t->inl, value, t->bytes);
        return PSM_OK;
    }
    p = nx_alloc(x, kind == K_MAP ? 16u : t->bytes);
    if (!p) return PSM_UNSUPPORTED_NEF;
    nx_copy(p, value, kind == K_MAP ? 16u : t->bytes);
    t->local = p;
    t->kind = (uint8_t)kind;
    return PSM_OK;
}

/* Bring a source value into the arena so it can be edited. */
static int nx_take(nefx *x, nx_tag *t)
{
    uint8_t *p;
    if (!t || t->kind != K_SRC) return t && t->kind == K_LOCAL ? PSM_OK : PSM_UNSUPPORTED_NEF;
    p = nx_alloc(x, t->bytes);
    if (!p || nx_read(x, t->src, p, t->bytes)) return PSM_UNSUPPORTED_NEF;
    t->local = p;
    t->kind = K_LOCAL;
    return PSM_OK;
}

static int nx_value(const nx_tag *t, uint32_t *value)
{
    if (!t || t->kind != K_INLINE || t->count != 1) return PSM_UNSUPPORTED_NEF;
    if (t->type == 3) *value = nx_get16(t->inl);
    else if (t->type == 4 || t->type == 13) *value = nx_get32(t->inl);
    else return PSM_UNSUPPORTED_NEF;
    return PSM_OK;
}

static int nx_parse(nefx *x, nx_ifd *ifd, uint32_t base, uint32_t at, uint32_t cap)
{
    uint8_t e[12];
    uint32_t n;
    if (x->used_tags + cap > NX_MAX_TAGS || nx_read(x, at, e, 2))
        return PSM_BAD_NEF;
    n = nx_get16(e);
    if (!n || n + 4u > cap)
        return PSM_UNSUPPORTED_NEF;
    ifd->t = x->pool + x->used_tags;
    x->used_tags += cap;
    ifd->cap = cap;
    ifd->n = 0;
    ifd->pos = ifd->base = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t tag, type, count, k;
        uint64_t bytes;
        nx_tag *t;
        if (nx_read(x, at + 2 + 12 * i, e, 12)) return PSM_BAD_NEF;
        tag = nx_get16(e);
        type = nx_get16(e + 2);
        count = nx_get32(e + 4);
        if (!type || type > 13) return PSM_BAD_NEF;
        bytes = (uint64_t)count * nx_unit[type];
        if (bytes > x->src.size) return PSM_BAD_NEF;
        for (k = ifd->n; k && ifd->t[k - 1].tag >= tag; --k) {
            if (ifd->t[k - 1].tag == tag) return PSM_BAD_NEF;
        }
        for (uint32_t m = ifd->n; m > k; --m)
            ifd->t[m] = ifd->t[m - 1];
        ++ifd->n;
        t = &ifd->t[k];
        nx_clear(t, sizeof(*t));
        t->tag = (uint16_t)tag;
        t->type = (uint16_t)type;
        t->count = count;
        t->bytes = (uint32_t)bytes;
        if (bytes <= 4) {
            t->kind = K_INLINE;
            nx_copy(t->inl, e + 8, 4);
        } else {
            uint64_t src = (uint64_t)base + nx_get32(e + 8);
            if (src > x->src.size || bytes > x->src.size - src) return PSM_BAD_NEF;
            t->kind = K_SRC;
            t->src = (uint32_t)src;
        }
    }
    return PSM_OK;
}

/* ---- emitter: the layout pass only advances the cursor, the emit pass writes ---- */

static int nx_flush(nefx *x)
{
    if (x->emit && x->fill) {
        if (x->write(x->context, x->flushed, x->stage, x->fill)) return PSM_WRITE_ERROR;
        x->flushed += x->fill;
        x->fill = 0;
    }
    return PSM_OK;
}

static int nx_out(nefx *x, const void *data, uint32_t n)
{
    const uint8_t *p = data;
    if (n > 0x7FFFFFFFu - x->cursor) return PSM_BAD_PLAN;
    x->cursor += n;
    if (!x->emit) return PSM_OK;
    while (n) {
        uint32_t chunk = NX_STAGE - x->fill;
        if (!chunk) {
            if (nx_flush(x)) return PSM_WRITE_ERROR;
            continue;
        }
        if (chunk > n) chunk = n;
        if (p) {
            nx_copy(x->stage + x->fill, p, chunk);
            p += chunk;
        } else {
            nx_clear(x->stage + x->fill, chunk);
        }
        x->fill += chunk;
        n -= chunk;
    }
    return PSM_OK;
}

static int nx_out_copy(nefx *x, uint32_t src, uint32_t n)
{
    if (n > 0x7FFFFFFFu - x->cursor || src > x->src.size || n > x->src.size - src)
        return PSM_BAD_NEF;
    if (!x->emit) {
        x->cursor += n;
        return PSM_OK;
    }
    while (n) {
        uint32_t chunk = NX_STAGE - x->fill;
        if (!chunk) {
            if (nx_flush(x)) return PSM_WRITE_ERROR;
            continue;
        }
        if (chunk > n) chunk = n;
        if (x->src.read_at(x->src.context, src, x->stage + x->fill, chunk)) return PSM_READ_ERROR;
        x->fill += chunk;
        x->cursor += chunk;
        src += chunk;
        n -= chunk;
    }
    return PSM_OK;
}

static int nx_align(nefx *x)
{
    return nx_out(x, 0, (0u - x->cursor) & 15u);
}

/* Record a position in the layout pass, check it in the emit pass. */
static int nx_mark(nefx *x, uint32_t *position)
{
    if (!x->emit) *position = x->cursor;
    else if (*position != x->cursor) return PSM_WRITE_ERROR;
    return PSM_OK;
}

static uint32_t nx_pointer(nefx *x, const nx_ifd *ifd, const nx_tag *t, uint32_t *value)
{
    if (ifd == &x->ifd0) {
        if (t->tag == 0x8769) { *value = x->exif.pos; return 1; }
        if (t->tag == 0x8825 && x->gps.t) { *value = x->gps.pos; return 1; }
        if (t->tag == 0x0201) { *value = x->thumb_pos; return 1; }
        if (t->tag == 0x014A && x->nsubs == 1) { *value = x->subs[0].pos; return 1; }
    } else if (ifd == &x->mn) {
        if (t->tag == 0x0011) { *value = x->prev.pos - x->mn.base; return 1; }
    } else if (ifd == &x->prev) {
        if (t->tag == 0x0201) { *value = x->pjpeg_pos - x->mn.base; return 1; }
    } else {
        for (uint32_t i = 0; i < x->nblocks; ++i) {
            if (ifd == &x->subs[x->blocks[i].sub] && t->tag == x->blocks[i].tag) {
                *value = x->blocks[i].pos;
                return 1;
            }
        }
    }
    return 0;
}

static int nx_write_ifd(nefx *x, nx_ifd *ifd);

static int nx_values(nefx *x, nx_ifd *ifd)
{
    for (uint32_t i = 0; i < ifd->n; ++i) {
        nx_tag *t = &ifd->t[i];
        uint32_t at;
        int status = PSM_OK;
        if (t->kind == K_INLINE) continue;
        if (nx_align(x)) return PSM_WRITE_ERROR;
        at = x->cursor;
        if (t->kind == K_SRC || t->kind == K_LOCAL || t->kind == K_SUBS || t->kind == K_MAP) {
            if (nx_mark(x, &t->pos)) return PSM_WRITE_ERROR;
        }
        switch (t->kind) {
        case K_SRC:
            status = nx_out_copy(x, t->src, t->bytes);
            break;
        case K_LOCAL:
            status = nx_out(x, t->local, t->bytes);
            break;
        case K_SUBS:
            for (uint32_t k = 0; k < x->nsubs && !status; ++k) {
                uint8_t v[4];
                nx_put32(v, x->subs[k].pos);
                status = nx_out(x, v, 4);
            }
            break;
        case K_MAP:
            status = nx_out(x, t->local, 16);
            if (!status) status = nx_mark(x, &x->map_pos);
            if (!status) status = nx_out(x, 0, NX_MAP_W * NX_MAP_H * 6u);
            break;
        case K_MN:
            if (nx_mark(x, &x->mn_at)) return PSM_WRITE_ERROR;
            x->mn.base = at + 10;
            status = nx_out(x, nx_mn_head, sizeof(nx_mn_head));
            if (!status) status = nx_write_ifd(x, &x->mn);
            if (!status) status = nx_align(x);
            if (!status && !x->emit) x->mn_bytes = x->cursor - at;
            if (!status && x->cursor - at != x->mn_bytes) status = PSM_WRITE_ERROR;
            break;
        case K_NI:
            if (nx_mark(x, &x->ni_at)) return PSM_WRITE_ERROR;
            x->ni.base = at + 10;
            status = nx_out_copy(x, x->ni_src, 18);
            if (!status) status = nx_write_ifd(x, &x->ni);
            if (!status) status = nx_align(x);
            if (!status && !x->emit) x->ni_bytes = x->cursor - at;
            if (!status && x->cursor - at != x->ni_bytes) status = PSM_WRITE_ERROR;
            break;
        default:
            status = PSM_BAD_PLAN;
        }
        if (status) return status;
    }
    return PSM_OK;
}

static int nx_write_ifd(nefx *x, nx_ifd *ifd)
{
    uint8_t e[12];
    if (nx_mark(x, &ifd->pos)) return PSM_WRITE_ERROR;
    nx_put16(e, ifd->n);
    if (nx_out(x, e, 2)) return PSM_WRITE_ERROR;
    for (uint32_t i = 0; i < ifd->n; ++i) {
        const nx_tag *t = &ifd->t[i];
        uint32_t value, count = t->count;
        nx_put16(e, t->tag);
        nx_put16(e + 2, t->type);
        nx_copy(e + 8, t->inl, 4);
        if (t->kind == K_MN) {
            count = x->mn_bytes;
            nx_put32(e + 8, x->mn_at - ifd->base);
        } else if (t->kind == K_NI) {
            count = x->ni_bytes;
            nx_put32(e + 8, x->ni_at - ifd->base);
        } else if (t->kind != K_INLINE) {
            nx_put32(e + 8, t->pos - ifd->base);
        } else if (nx_pointer(x, ifd, t, &value)) {
            nx_put32(e + 8, value);
        }
        nx_put32(e + 4, count);
        if (nx_out(x, e, 12)) return PSM_WRITE_ERROR;
    }
    nx_clear(e, 4);
    if (nx_out(x, e, 4)) return PSM_WRITE_ERROR;
    return nx_values(x, ifd);
}

static int nx_walk(nefx *x)
{
    static const uint8_t head[8] = {'I', 'I', 42, 0, 8, 0, 0, 0};
    int status;
    x->cursor = x->flushed = x->fill = 0;
    if ((status = nx_out(x, head, 8)) || (status = nx_write_ifd(x, &x->ifd0)) ||
        (status = nx_align(x)) || (status = nx_write_ifd(x, &x->exif)) || (status = nx_align(x)))
        return status;
    x->prev.base = x->mn.base;
    if ((status = nx_write_ifd(x, &x->prev)) || (status = nx_align(x)) ||
        (status = nx_mark(x, &x->pjpeg_pos)) || (status = nx_out_copy(x, x->pjpeg_src, x->pjpeg_bytes)) ||
        (status = nx_align(x)))
        return status;
    if (x->gps.t && (status = nx_write_ifd(x, &x->gps)))
        return status;
    if ((status = nx_align(x)) || (status = nx_mark(x, &x->thumb_pos)) ||
        (status = nx_out_copy(x, x->thumb_src, x->thumb_bytes)))
        return status;
    for (uint32_t i = 0; i < x->nsubs; ++i) {
        if ((status = nx_align(x)) || (status = nx_write_ifd(x, &x->subs[i])))
            return status;
    }
    if ((status = nx_align(x))) return status;
    for (uint32_t i = 0; i < x->nblocks; ++i) {
        nx_block *b = &x->blocks[i];
        if ((status = nx_align(x)) || (status = nx_mark(x, &b->pos))) return status;
        if (b->raw) {
            if (i + 1 != x->nblocks) return PSM_UNSUPPORTED_NEF;
            if (nx_mark(x, &x->raw_pos)) return PSM_WRITE_ERROR;
            if (!x->emit) x->end = x->cursor + x->strip_bytes;
            return nx_flush(x);
        }
        if ((status = nx_out_copy(x, b->src, b->bytes))) return status;
    }
    return PSM_UNSUPPORTED_NEF;
}

/* ---- plan ---- */

static int nx_scale_rationals(uint8_t *v, uint32_t at, uint32_t scale)
{
    for (uint32_t k = 0; k < 2; ++k) {
        int32_t numerator = (int32_t)nx_get32(v + at + 8 * k);
        nx_put32(v + at + 8 * k, (uint32_t)(numerator * (int32_t)scale));
    }
    return PSM_OK;
}

static int nx_with_crc(nx_tag *t)
{
    if (!t || t->kind != K_LOCAL || t->bytes < 3) return PSM_UNSUPPORTED_NEF;
    nx_put16(t->local + t->bytes - 2, nx_crc16(t->local, t->bytes - 2));
    return PSM_OK;
}

static int nx_edit(nefx *x)
{
    uint32_t W = x->width, H = x->height, mode = x->mode;
    uint8_t v[24];
    nx_tag *t;
    nx_ifd *raw = &x->subs[x->raw_sub];
    int status;
    /* IFD0 Software: the camera's own string without its NUL padding */
    t = nx_find(&x->ifd0, 0x0131);
    if (!t || t->type != 2) return PSM_UNSUPPORTED_NEF;
    if (t->kind == K_SRC && nx_take(x, t)) return PSM_UNSUPPORTED_NEF;
    {
        const uint8_t *s = t->kind == K_INLINE ? t->inl : t->local;
        uint32_t n = t->bytes;
        uint8_t text[64];
        while (n && !s[n - 1]) --n;
        if (n + 1 > sizeof(text)) return PSM_UNSUPPORTED_NEF;
        nx_copy(text, s, n);
        text[n] = 0;
        if ((status = nx_set_bytes(x, &x->ifd0, 0x0131, 2, n + 1, text, K_LOCAL))) return status;
    }
    /* MakerNote */
    nx_drop(&x->mn, 0x0058);
    nx_drop(&x->mn, 0x0096);
    nx_put16(v, 0); nx_put16(v + 2, 0); nx_put16(v + 4, W); nx_put16(v + 6, H);
    if ((status = nx_set_bytes(x, &x->mn, 0x0045, 3, 4, v, K_LOCAL))) return status;
    t = nx_find(&x->mn, 0x0051);
    if (nx_take(x, t) || t->bytes < 11 || t->local[10] != 3) return PSM_UNSUPPORTED_NEF;
    t->local[10] = 10;
    t = nx_find(&x->mn, 0x0054);
    if (nx_take(x, t) || t->bytes < 20) return PSM_UNSUPPORTED_NEF;
    nx_scale_rationals(t->local, 4, x->scale);
    if (x->scale == 2) {
        t = nx_find(&x->mn, 0x00B7);
        if (nx_take(x, t) || t->bytes < 80) return PSM_UNSUPPORTED_NEF;
        nx_clear(t->local + 62, 18);
    }
    {
        static const uint8_t e2d[16] = {'0','1','0','0',2,0,0,0,0,0,0,0,0,0,0,0};
        uint8_t m[16];
        nx_copy(m, e2d, 16);
        m[6] = m[7] = (uint8_t)mode;
        if ((status = nx_set_bytes(x, &x->mn, 0x0E2D, 7, 16, m, K_LOCAL))) return status;
        nx_copy(m, "0100", 4);
        nx_put32(m + 4, 16);
        nx_put16(m + 8, NX_MAP_W);
        nx_put16(m + 10, NX_MAP_H);
        m[12] = 3; m[13] = m[14] = m[15] = 14;
        if ((status = nx_set_bytes(x, &x->mn, 0x0E2E, 7, 16 + NX_MAP_W * NX_MAP_H * 6u, m, K_MAP))) return status;
    }
    /* NEFInfo */
    nx_drop(&x->ni, 0x07);
    nx_drop(&x->ni, 0x0F);
    t = nx_find(&x->ni, 0x08);
    if (nx_take(x, t) || t->bytes < 22) return PSM_UNSUPPORTED_NEF;
    nx_scale_rationals(t->local, 4, x->scale);
    if (nx_with_crc(t)) return PSM_UNSUPPORTED_NEF;
    nx_copy(v, "0100", 4);
    nx_put16(v + 4, 0); nx_put16(v + 6, 0); nx_put16(v + 8, W); nx_put16(v + 10, H);
    if ((status = nx_set_bytes(x, &x->ni, 0x0B, 7, 14, v, K_LOCAL))) return status;
    if (nx_with_crc(nx_find(&x->ni, 0x0B))) return PSM_UNSUPPORTED_NEF;
    t = nx_find(&x->ni, 0x0D);
    if (nx_take(x, t) || t->bytes < 7 || t->local[4] != 3) return PSM_UNSUPPORTED_NEF;
    t->local[4] = 10;
    if (nx_with_crc(t)) return PSM_UNSUPPORTED_NEF;
    /* raw SubIFD */
    nx_put16(v, 14); nx_put16(v + 2, 14); nx_put16(v + 4, 14);
    if ((status = nx_set_inline(raw, 0x0100, 4, W)) || (status = nx_set_inline(raw, 0x0101, 4, H)) ||
        (status = nx_set_bytes(x, raw, 0x0102, 3, 3, v, K_LOCAL)) ||
        (status = nx_set_inline(raw, 0x0106, 3, 2)) || (status = nx_set_inline(raw, 0x0115, 3, 3)) ||
        (status = nx_set_inline(raw, 0x0116, 4, H)) || (status = nx_set_inline(raw, 0x0117, 4, x->strip_bytes)))
        return status;
    return PSM_OK;
}

int nefx_plan(nefx *x, const psm_file *first, uint32_t width, uint32_t height, uint32_t scale, uint32_t frames)
{
    uint8_t h[18];
    uint32_t value = 0, mbase, n = 0;
    uint64_t strip;
    nx_tag *t;
    int status = PSM_OK;
    if (!x || !first || !first->read_at || (scale != 1 && scale != 2) || width % 4u ||
        width < NX_MAP_W || height < NX_MAP_H || width > 65535u || height > 65535u)
        return PSM_BAD_PLAN;
    /* PS6: 0x0E2D mode = 4, 8, 16, 32 frames -> 1, 2, 3, 4; 4 and 8 frames at scale 1, 16 and 32 at scale 2 */
    if ((frames != 4 && frames != 8 && frames != 16 && frames != 32) || scale != (frames >= 16 ? 2u : 1u))
        return PSM_NEFX_MODE;
    nx_clear(&x->pool, sizeof(x->pool));
    x->src = *first;
    x->used_tags = x->arena_used = x->nsubs = x->nblocks = x->md_bytes = 0;
    x->ifd0.t = x->exif.t = x->mn.t = x->prev.t = x->gps.t = x->ni.t = 0;
    x->width = width;
    x->height = height;
    x->scale = scale;
    x->mode = frames == 4 ? 1u : frames == 8 ? 2u : frames == 16 ? 3u : 4u;
    x->row_bytes = width / 4u * 21u;                       /* 3 samples x 14 bits per pixel */
    strip = (uint64_t)x->row_bytes * height;
    if (strip > 0x7F000000u) return PSM_BAD_PLAN;
    x->strip_bytes = (uint32_t)strip;
    if ((status = nx_read(x, 0, h, 8))) return status;
    if (h[0] != 'I' || h[1] != 'I' || nx_get16(h + 2) != 42) return PSM_UNSUPPORTED_NEF;
    if ((status = nx_parse(x, &x->ifd0, 0, nx_get32(h + 4), 64))) return status;
    if (nx_value(nx_find(&x->ifd0, 0x8769), &value) || (status = nx_parse(x, &x->exif, 0, value, 96)))
        return status ? status : PSM_UNSUPPORTED_NEF;
    if (nx_find(&x->ifd0, 0x8825)) {
        if (nx_value(nx_find(&x->ifd0, 0x8825), &value) || (status = nx_parse(x, &x->gps, 0, value, 64)))
            return status ? status : PSM_UNSUPPORTED_NEF;
    }
    /* MakerNote and its preview */
    t = nx_find(&x->exif, 0x927C);
    if (!t || t->kind != K_SRC || t->type != 7 || t->bytes < 18 || nx_read(x, t->src, h, 18) ||
        !nx_same(h, nx_mn_head, 18))
        return PSM_UNSUPPORTED_NEF;
    t->kind = K_MN;
    x->mn_src = t->src;
    mbase = t->src + 10;
    if ((status = nx_parse(x, &x->mn, mbase, mbase + 8, 160))) return status;
    if (nx_value(nx_find(&x->mn, 0x0011), &value) || (status = nx_parse(x, &x->prev, mbase, mbase + value, 32)))
        return status ? status : PSM_UNSUPPORTED_NEF;
    if (nx_value(nx_find(&x->prev, 0x0201), &value) || nx_value(nx_find(&x->prev, 0x0202), &n) ||
        (uint64_t)mbase + value + n > x->src.size)
        return PSM_UNSUPPORTED_NEF;
    x->pjpeg_src = mbase + value;
    x->pjpeg_bytes = n;
    /* IFD0 thumbnail */
    if (nx_value(nx_find(&x->ifd0, 0x0201), &value) || nx_value(nx_find(&x->ifd0, 0x0202), &n) ||
        (uint64_t)value + n > x->src.size)
        return PSM_UNSUPPORTED_NEF;
    x->thumb_src = value;
    x->thumb_bytes = n;
    /* SubIFDs */
    t = nx_find(&x->ifd0, 0x014A);
    if (!t || (t->type != 4 && t->type != 13) || !t->count || t->count > NX_MAX_SUBS)
        return PSM_UNSUPPORTED_NEF;
    x->nsubs = t->count;
    if (t->count > 1) t->kind = K_SUBS;
    x->raw_sub = NX_MAX_SUBS;
    for (uint32_t i = 0; i < x->nsubs; ++i) {
        uint8_t o[4];
        if (x->nsubs == 1) nx_copy(o, t->inl, 4);
        else if (nx_read(x, t->src + 4 * i, o, 4)) return PSM_BAD_NEF;
        if ((status = nx_parse(x, &x->subs[i], 0, nx_get32(o), 40))) return status;
        if (x->raw_sub == NX_MAX_SUBS && !nx_value(nx_find(&x->subs[i], 0x0106), &value) && value == 32803)
            x->raw_sub = i;
    }
    if (x->raw_sub == NX_MAX_SUBS) return PSM_UNSUPPORTED_NEF;
    /* NEFInfo of the raw SubIFD */
    t = nx_find(&x->subs[x->raw_sub], 0xC7D5);
    if (!t || t->kind != K_SRC || t->type != 7 || t->bytes < 20 || nx_read(x, t->src, h, 18) ||
        !nx_same(h, nx_ni_head, 18))
        return PSM_UNSUPPORTED_NEF;
    t->kind = K_NI;
    x->ni_src = t->src;
    if ((status = nx_parse(x, &x->ni, t->src + 10, t->src + 18, 40))) return status;
    for (uint32_t i = 0; i < x->ni.n; ++i) {
        const nx_tag *e = &x->ni.t[i];
        if (e->kind == K_SRC && (e->src < x->ni_src || e->src + e->bytes > x->ni_src + t->bytes))
            return PSM_UNSUPPORTED_NEF;
    }
    if ((status = nx_edit(x))) return status;
    /* data blocks in the source order */
    for (uint32_t i = 0; i < x->nsubs; ++i) {
        nx_ifd *s = &x->subs[i];
        nx_block *b = &x->blocks[x->nblocks];
        uint32_t offset, bytes;
        if (nx_find(s, 0x0201) && !nx_value(nx_find(s, 0x0202), &bytes) && bytes) {
            if (nx_value(nx_find(s, 0x0201), &offset)) return PSM_UNSUPPORTED_NEF;
            b->tag = 0x0201;
        } else if (nx_find(s, 0x0111) && !nx_value(nx_find(s, 0x0117), &bytes) && bytes) {
            if (nx_value(nx_find(s, 0x0111), &offset)) return PSM_UNSUPPORTED_NEF;
            b->tag = 0x0111;
        } else {
            continue;
        }
        b->raw = i == x->raw_sub;
        if (!b->raw && (uint64_t)offset + bytes > x->src.size) return PSM_BAD_NEF;
        b->src = offset;
        b->bytes = bytes;
        b->sub = i;
        b->pos = 0;
        for (uint32_t k = x->nblocks; k && (x->blocks[k - 1].src > offset ||
             (x->blocks[k - 1].src == offset && x->blocks[k - 1].sub > i)); --k) {
            nx_block tmp = x->blocks[k];
            x->blocks[k] = x->blocks[k - 1];
            x->blocks[k - 1] = tmp;
        }
        ++x->nblocks;
    }
    /* map partition, then the layout pass */
    for (uint32_t j = 0; j <= NX_MAP_W; ++j)
        x->xs[j] = (uint16_t)(j * width / NX_MAP_W);
    for (uint32_t i = 0; i <= NX_MAP_H; ++i)
        x->ys[i] = (uint16_t)(i * height / NX_MAP_H);
    x->emit = 0;
    x->mn.base = 0;
    if ((status = nx_walk(x))) return status;
    return x->end ? PSM_OK : PSM_BAD_PLAN;
}

int nefx_header(nefx *x, nefx_write_fn write, void *context)
{
    if (!x || !write || !x->end) return PSM_BAD_PLAN;
    x->write = write;
    x->context = context;
    x->emit = 1;
    x->map_row = x->map_first = x->acc_rows = 0;
    nx_clear(x->acc, sizeof(x->acc));
    return nx_walk(x);
}

/* PS7: 32 samples of 14 bits = 7 little-endian 64-bit words, the same bit stream as PS1's 7 bytes per 4 samples. */
static void nx_pack32(uint64_t *w, const uint16_t *s)
{
    w[0] = (uint64_t)s[0] | (uint64_t)s[1] << 14 | (uint64_t)s[2] << 28 | (uint64_t)s[3] << 42 | (uint64_t)s[4] << 56;
    w[1] = (uint64_t)s[4] >> 8 | (uint64_t)s[5] << 6 | (uint64_t)s[6] << 20 | (uint64_t)s[7] << 34 | (uint64_t)s[8] << 48 |
           (uint64_t)s[9] << 62;
    w[2] = (uint64_t)s[9] >> 2 | (uint64_t)s[10] << 12 | (uint64_t)s[11] << 26 | (uint64_t)s[12] << 40 |
           (uint64_t)s[13] << 54;
    w[3] = (uint64_t)s[13] >> 10 | (uint64_t)s[14] << 4 | (uint64_t)s[15] << 18 | (uint64_t)s[16] << 32 |
           (uint64_t)s[17] << 46 | (uint64_t)s[18] << 60;
    w[4] = (uint64_t)s[18] >> 4 | (uint64_t)s[19] << 10 | (uint64_t)s[20] << 24 | (uint64_t)s[21] << 38 |
           (uint64_t)s[22] << 52;
    w[5] = (uint64_t)s[22] >> 12 | (uint64_t)s[23] << 2 | (uint64_t)s[24] << 16 | (uint64_t)s[25] << 30 |
           (uint64_t)s[26] << 44 | (uint64_t)s[27] << 58;
    w[6] = (uint64_t)s[27] >> 6 | (uint64_t)s[28] << 8 | (uint64_t)s[29] << 22 | (uint64_t)s[30] << 36 |
           (uint64_t)s[31] << 50;
}

int nefx_row(nefx *x, uint32_t row, const uint16_t *rgb, uint8_t *packed)
{
    typedef uint64_t alias_word __attribute__((may_alias));
    uint32_t samples = x->width * 3u, i = 0, check = 0;
    uint8_t *o = packed;
    if (!x->emit || row != x->acc_rows || row >= x->height) return PSM_BAD_PLAN;
    for (uint32_t k = 0; k < samples; ++k)
        check |= rgb[k];
    if (check >> 14) return PSM_BAD_PLAN;
    if (!((uintptr_t)packed & 7u)) {
        for (; i + 32u <= samples; i += 32u, o += 56) {
            uint64_t w[7];
            nx_pack32(w, rgb + i);
            for (uint32_t k = 0; k < 7; ++k)
                ((alias_word *)o)[k] = w[k];
        }
    }
    for (; i < samples; i += 4) {
        uint64_t v = rgb[i] | ((uint64_t)rgb[i + 1] << 14) | ((uint64_t)rgb[i + 2] << 28) | ((uint64_t)rgb[i + 3] << 42);
        for (uint32_t k = 0; k < 7; ++k)
            *o++ = (uint8_t)(v >> (8 * k));
    }
    if (x->write(x->context, x->raw_pos + row * x->row_bytes, packed, x->row_bytes))
        return PSM_WRITE_ERROR;
    for (uint32_t j = 0, px = 0; j < NX_MAP_W; ++j) {
        uint32_t *a = x->acc + 3 * j;
        for (; px < x->xs[j + 1]; ++px) {
            a[0] += rgb[3 * px];
            a[1] += rgb[3 * px + 1];
            a[2] += rgb[3 * px + 2];
        }
    }
    ++x->acc_rows;
    if (x->acc_rows == x->ys[x->map_row + 1]) {
        uint32_t rows = x->ys[x->map_row + 1] - x->ys[x->map_row];
        uint16_t *m = x->map + (x->map_row - x->map_first) * NX_MAP_W * 3u;
        for (uint32_t j = 0; j < NX_MAP_W; ++j) {
            uint32_t n = rows * (uint32_t)(x->xs[j + 1] - x->xs[j]);
            for (uint32_t c = 0; c < 3; ++c) {
                m[3 * j + c] = (uint16_t)(x->acc[3 * j + c] / n);
                x->acc[3 * j + c] = 0;
            }
        }
        ++x->map_row;
        if (x->map_row - x->map_first == NX_MAP_CHUNK || x->map_row == NX_MAP_H) {
            if (x->write(x->context, x->map_pos + x->map_first * NX_MAP_W * 6u, x->map,
                         (x->map_row - x->map_first) * NX_MAP_W * 6u))
                return PSM_WRITE_ERROR;
            x->map_first = x->map_row;
        }
    }
    return PSM_OK;
}

uint32_t nefx_size(const nefx *x)
{
    return x ? x->end : 0;
}
