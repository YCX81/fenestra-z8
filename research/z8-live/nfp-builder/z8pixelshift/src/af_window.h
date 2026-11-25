/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS8_AF_WINDOW_H
#define PS8_AF_WINDOW_H
#include "psm.h"
typedef struct {
    uint16_t *rgb[5], *green[5], *features, *values;
    uint32_t row_id[5], slots, width;
} ps8_af_window;
size_t ps8_af_window_bytes(const psm_plan *);
int ps8_af_window_init(ps8_af_window *, const psm_plan *, void *, size_t);
int ps8_af_render(ps8_af_window *, const psm_plan *, uint32_t, uint16_t *);
#endif
