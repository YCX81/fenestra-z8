/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef Z8_PSM_H
#define Z8_PSM_H

#include <stddef.h>
#include <stdint.h>

#define PSM_MAX_FRAMES 32u
#define PSM_MAX_SENSOR 20000u

enum psm_status {
    PSM_OK = 0,
    PSM_BAD_PLAN = 1,
    PSM_BAD_GROUP = 2,
    PSM_BAD_LATTICE = 3,
    PSM_BAD_CROP = 4,
    PSM_NO_MEMORY = 5,
    PSM_READ_ERROR = 6,
    PSM_CANCELLED = 7,
    PSM_BAD_NEF = 8,
    PSM_UNSUPPORTED_NEF = 9,
    PSM_WRITE_ERROR = 10,
    PSM_OUTPUT_EXISTS = 11,
    PSM_BAD_CONFIG = 12,
    PSM_NO_CARD = 16,
    PSM_SCAN_LIMIT = 15,
    PSM_NEFX_MODE = 13,         /* PS1: frame count without a known NEFX mode (PS6: 4, 8, 16, 32 known) */
    PSM_NO_NEW = 18             /* PS7: groups on the card, every complete one already merged (17 = firmware Busy) */
};

typedef struct {
    uint32_t frame_id;
    int32_t dx2, dy2;
    uint16_t black[4], white[4];
    uint8_t cfa[4];
} psm_frame;

typedef struct {
    uint32_t sensor_width, sensor_height;
    uint32_t frame_count, scale;
    int32_t x0, y0;
    uint32_t width, height;
    uint64_t group_id;
    psm_frame frames[PSM_MAX_FRAMES];
} psm_plan;

typedef int (*psm_read_row_fn)(void *, uint32_t, uint32_t, uint16_t *, uint32_t);
typedef int (*psm_cancel_fn)(void *);

int psm_validate(const psm_plan *plan);
size_t psm_scratch_size(const psm_plan *plan);
int psm_merge_row(const psm_plan *plan, uint32_t row,
                  psm_read_row_fn read_row, psm_cancel_fn cancel, void *context,
                  void *scratch, size_t scratch_size, uint16_t *rgb);

/* Internal fast path: caller must first pass psm_validate and keep the plan unchanged.
   PS7: rgb must be 8-byte aligned; it is the only accumulator (scratch holds the source rows). */
int psm_merge_row_validated(const psm_plan *plan, uint32_t row,
                  psm_read_row_fn read_row, psm_cancel_fn cancel, void *context,
                  void *scratch, size_t scratch_size, uint16_t *rgb);

/* PS8: preserve the pre-truncation green sum for the AF window. Paired modes
   reject AF blue samples while pairing; other channels retain PS7 arithmetic. */
int psm_merge_row_af(const psm_plan *, uint32_t, psm_read_row_fn, psm_cancel_fn,
                    void *, void *, size_t, uint16_t *, uint16_t *green_sum);

#endif
