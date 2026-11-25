/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PS1: merged pixel-shift NEFX, laid out the way NX Studio 1.11 writes it. */
#ifndef Z8_PSM_NEFX_H
#define Z8_PSM_NEFX_H

#include "nef.h"

#define NX_MAX_TAGS 768u
#define NX_MAX_SUBS 8u
#define NX_ARENA 2048u
#define NX_STAGE 32768u
#define NX_MDCACHE 8192u
#define NX_MAP_W 640u
#define NX_MAP_H 424u
#define NX_MAP_CHUNK 16u                    /* map block rows per seek-and-write */

typedef int (*nefx_write_fn)(void *, uint32_t, const void *, uint32_t);

typedef struct {
    uint16_t tag, type;
    uint32_t count, bytes;
    uint32_t src;                           /* out-of-line source value: absolute offset */
    uint8_t *local;                         /* replaced out-of-line value */
    uint8_t inl[4];                         /* inline value as stored in the entry */
    uint8_t kind;
    uint32_t pos;                           /* absolute output position of an out-of-line value */
} nx_tag;

typedef struct {
    nx_tag *t;
    uint32_t n, cap, pos, base;
} nx_ifd;

typedef struct {
    uint32_t src, bytes, pos, sub;
    uint16_t tag;
    uint8_t raw;
} nx_block;

typedef struct {
    uint8_t stage[NX_STAGE] __attribute__((aligned(64)));
    uint8_t md[NX_MDCACHE] __attribute__((aligned(64)));
    uint16_t map[NX_MAP_CHUNK * NX_MAP_W * 3] __attribute__((aligned(64)));
    uint32_t acc[NX_MAP_W * 3];
    uint16_t xs[NX_MAP_W + 1], ys[NX_MAP_H + 1];
    nx_tag pool[NX_MAX_TAGS];
    uint8_t arena[NX_ARENA];
    nx_ifd ifd0, exif, mn, prev, gps, ni, subs[NX_MAX_SUBS];
    nx_block blocks[NX_MAX_SUBS];
    psm_file src;
    uint32_t used_tags, arena_used, nsubs, nblocks, raw_sub;
    uint32_t md_offset, md_bytes;
    uint32_t width, height, scale, mode, row_bytes, strip_bytes;
    uint32_t mn_src, ni_src, pjpeg_src, pjpeg_bytes, thumb_src, thumb_bytes;
    uint32_t mn_at, mn_bytes, ni_at, ni_bytes, pjpeg_pos, thumb_pos, map_pos, raw_pos, end;
    uint32_t map_row, map_first, acc_rows;
    /* emitter */
    nefx_write_fn write;
    void *context;
    uint32_t cursor, flushed, fill, emit;
} nefx;

/* Reads the first frame and fixes every position of the output. PS6: frames (4, 8, 16, 32) sets the 0x0E2D mode. */
int nefx_plan(nefx *x, const psm_file *first, uint32_t width, uint32_t height, uint32_t scale, uint32_t frames);
/* Writes [0, raw data) through write(context, offset, data, bytes). */
int nefx_header(nefx *x, nefx_write_fn write, void *context);
/* Packs one merged row (raw units, RGB) into packed, writes it and folds it into the 0x0E2E map. */
int nefx_row(nefx *x, uint32_t row, const uint16_t *rgb, uint8_t *packed);
uint32_t nefx_size(const nefx *x);

#endif
