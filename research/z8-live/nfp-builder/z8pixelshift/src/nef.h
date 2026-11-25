/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef Z8_PSM_NEF_H
#define Z8_PSM_NEF_H

#include "psm.h"

typedef int (*psm_read_at_fn)(void *, uint32_t, void *, uint32_t);

typedef struct {
    psm_read_at_fn read_at;
    void *context;
    uint32_t size;
} psm_file;

typedef struct {
    psm_file file;
    uint32_t width, height, strip_offset, strip_bytes;
    uint32_t next_row, cursor, held;
    uint64_t pending;
    uint32_t cache_offset, cache_bytes;
    int32_t vertical[2][2];
    uint16_t huffman[256];
    uint16_t seeds[4];
    uint8_t cfa[4];
    /* PS7: set by the caller before psm_nef_open. cache: cache_size bytes (a power of two of at least 4 KiB),
       64-byte aligned (PS1: whole cache lines); fast: psm_nef_fast_table, which every frame can share. */
    uint8_t *cache;
    uint32_t cache_size;
    const int32_t *fast;
    uint8_t big_endian;
    uint8_t metadata_found;
} psm_nef;

/* PS7: one lookup of PSM_FAST_BITS stream bits gives code and payload together when both fit (the difference
   times 256 plus the bits used); 0 sends the symbol to PS6's prefix decoder. */
#define PSM_FAST_BITS 12u
void psm_nef_fast_table(int32_t *table);
int psm_nef_open(psm_nef *nef, const psm_file *file);
int psm_nef_row(psm_nef *nef, uint32_t row, uint16_t *samples, uint32_t width);

#endif
