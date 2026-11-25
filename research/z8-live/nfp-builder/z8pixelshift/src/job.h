/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef Z8_PSM_JOB_H
#define Z8_PSM_JOB_H

#include "psm.h"

typedef int (*psm_list_fn)(void *, const char *, uint64_t, uint32_t);

typedef struct {
    void *context;
    void *(*allocate)(void *, uint32_t);
    void (*release)(void *, void *);
    int (*open)(void *, const char *, uint32_t *);
    int (*read_at)(void *, int, uint32_t, void *, uint32_t);
    int (*create)(void *, const char *);
    /* PS1: positioned write (the NEFX map is written back into the header) */
    int (*write_at)(void *, int, uint32_t, const void *, uint32_t);
    int (*close)(void *, int);
    int (*exists)(void *, const char *);
    int (*rename)(void *, const char *, const char *);
    /* PS1: delete a file (stale temporary file, partial output) */
    int (*remove)(void *, const char *);
    int (*cancelled)(void *);
    int (*list)(void *, const char *, uint32_t, psm_list_fn, void *);
    void (*progress)(void *, uint32_t, uint32_t);
    /* PS4: optional step report before the first merged row (may be NULL): phase, then two values. */
    void (*phase)(void *, uint32_t, uint32_t, uint32_t);
    /* PS7, optional: a free-running tick counter and its rate (per-step times), and the group a job starts:
       its number (1-based) and the number of groups of the job, "card:folder" and the first frame's number. */
    uint64_t (*ticks)(void *);
    uint64_t (*ticks_per_second)(void *);
    void (*group)(void *, uint32_t, uint32_t, const char *, const char *);
    /* PS8: optional single I/O worker. start may fail only before retaining arg;
       join returns after the callback and its final memory access. wait/wake are
       counting notifications (0 compute, 1 I/O), never unguarded sleeps. */
    int (*io_start)(void *, void (*)(void *), void *);
    void (*io_join)(void *);
    void (*io_wait)(void *, uint32_t);
    void (*io_wake)(void *, uint32_t);
    void (*prefetch)(void *, int, uint32_t, uint32_t);
    uint32_t io_mode;
} psm_platform;

/* PS4 steps shown on the progress page, so a stall on the body names its step.
   SCAN: drive letter, NEF files probed so far. READ: frame (1-based) | frame count << 8, read-pass percent.
   NAME/SETUP/CREATE/HEADER: no values. Rows after HEADER are reported by progress(). */
enum psm_phase { PSM_PHASE_SCAN = 1, PSM_PHASE_NAME = 2, PSM_PHASE_READ = 3, PSM_PHASE_SETUP = 4,
                 PSM_PHASE_CREATE = 5, PSM_PHASE_HEADER = 6 };
#define PSM_PHASE(p, code, first, second) \
    do { if ((p)->phase) (p)->phase((p)->context, (code), (first), (second)); } while (0)

/* PS1: what was written on success */
enum psm_format { PSM_FORMAT_NEFX = 2 };

/* PS7: the newest group without a merged output, or every such group (newest first). */
enum psm_mode { PSM_MODE_NEWEST = 0, PSM_MODE_ALL = 1 };

/* PS7: ticks spent per step of one group (psm_platform.ticks). read and write are the platform calls; the
   _rows parts fall inside the row loop. decode includes its reads, merge its decodes, pack its writes. */
typedef struct {
    uint64_t open, rows, decode, merge, pack, finish, read, write, read_rows, write_rows;
    uint64_t af;
    uint64_t io_service, input_wait, output_wait;
    uint32_t io_mode, io_fallback, io_read_slots, io_write_bytes, prefetch_hits, prefetch_misses, queue_high;
    uint64_t read_bytes, write_bytes;
    uint32_t reads, writes, reopens, cache_bytes;
    /* PS9: last failed allocation in a pre-output setup attempt. */
    uint32_t memory_stage, memory_bytes;
} psm_times;

typedef struct {
    uint32_t mode, groups, targets, merged, attempted;
    int32_t first_error;
    uint64_t ticks, ticks_per_second;
} psm_summary;

uint32_t psm_crc32(const void *data, uint32_t size, uint32_t previous);
int psm_camera_job(const psm_platform *, uint32_t mode, psm_summary *);

#endif
