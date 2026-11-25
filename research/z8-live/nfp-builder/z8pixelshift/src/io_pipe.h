/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS8_IO_PIPE_H
#define PS8_IO_PIPE_H
#include "job.h"
typedef struct ps8_io ps8_io;
/* NULL: startup-only synchronous AF fallback. No FS ownership has changed. */
/* Fallback reason: 0 active, 1 no worker API, 2 control allocation,
   3 prefetch allocation, 4 output allocation, 5 worker startup. */
ps8_io *ps8_io_begin(const psm_platform *, uint32_t frames,uint32_t *fallback);
const psm_platform *ps8_io_platform(ps8_io *);
/* Drain -> close -> join -> free. Called only after merge_group releases all
   caller buffers; those releases are safe because every read RPC has completed. */
int ps8_io_end(ps8_io *, psm_times *);
#endif
