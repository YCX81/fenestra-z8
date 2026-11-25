/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Dual-Core CPU2 + CPU3 Lock-Free Stripe Pool (Hardening #5).
 *
 * Adapted from Fenestra-Z8-C311-source/modules/jpeg_lut/smp.{h,c}.
 * Uses stock Z8 C3.11 cre_tsk (0x40185064), sta_tsk (0x401855E0), dly_tsk (0x40187B00),
 * semaphores (0x40180E50/102C/11E8/15CC), FxZ8LutIoGate helper-owner arbitration
 * (FX_Z8_LUT_HELPER_JPEG preempts FX_Z8_LUT_HELPER_REAR), heartbeat instrumentation,
 * and timeout quarantine (FX_Z8_FLIGHT_SMP_QUARANTINE).
 *
 * Strictly scalar (-mgeneral-regs-only), zero .data/.bss (pool state lives inside
 * heap-allocated LutCoreCtx anchored at v71r7_anchor()->lut_core_ctx).
 */
#ifndef V71R7_SMP_H
#define V71R7_SMP_H

#include <stddef.h>
#include <stdint.h>
#include "v71r7_lut_abi.h"

#ifdef V71R7_HOST_TEST
#include <pthread.h>
#endif

enum {
    V71R7_SMP_HELPER_COUNT       = 2u,
    V71R7_SMP_JOB_SLOTS          = 4u,
    V71R7_SMP_INLINE_CTX_BYTES   = 128u,
    V71R7_SMP_STACK_BYTES        = 16384u,
    V71R7_SMP_PRIORITY           = 30u, /* Strictly below Z8 prio<=28 RT tasks (HEVC/AF-C/CV/audio) */
    V71R7_SMP_PHASE_DEAD         = 0u,
    V71R7_SMP_PHASE_PARKED       = 1u,
    V71R7_SMP_PHASE_QUEUED       = 2u,
    V71R7_SMP_PHASE_ACTIVE       = 3u,
    V71R7_SMP_PHASE_QUARANTINED  = 4u,
    V71R7_SMP_PHASE_QUAR_DRAINED = 5u,
    V71R7_SMP_PHASE_MASK         = 7u,
    V71R7_SMP_NEXT_GEN           = 8u
};

/* Lock-free stripe job descriptor (pool-resident so quarantined late helpers
 * never touch a popped caller stack frame). */
typedef struct {
    _Alignas(64) uint32_t next_row;
    uint32_t total_rows;
    uint32_t stripe_rows;
    uint32_t cancelled;
    uint64_t owner;
    uint64_t generation;
    V71r7SmpStripeFn fn;
    void            *ctx;
    _Alignas(16) uint8_t ctx_storage[V71R7_SMP_INLINE_CTX_BYTES];
} V71r7SmpJob;

struct V71r7SmpPool;

typedef struct {
    _Alignas(64) uint32_t phase;
    uint32_t core;            /* 1-based RTOS processor ID: 2 (CPU1) or 3 (CPU2) */
    uint32_t slot;            /* 1 or 2 */
    uint32_t sentinel_status; /* V71R7_NEON_UNAVAILABLE / PASS / FAILED */
    int32_t  task_id;
    int32_t  wake_sem;
    void    *stack_raw;
    uint64_t             job_gen;
    V71r7SmpJob         *job;
    struct V71r7SmpPool *pool;
#ifdef V71R7_HOST_TEST
    pthread_t       thread;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    uint32_t        wake_seq;
    uint32_t        seen_seq;
    uint32_t        stop_req;
    uint32_t        thread_started;
#endif
} V71r7SmpHelper;

typedef struct V71r7SmpPool {
    uint32_t magic;
    uint32_t initialized;
    uint32_t stopped;
    uint32_t local_lock;
    uint32_t jpeg_waiting;
    uint32_t rear_preempted_cnt;
    uint32_t dispatches_jpeg;
    uint32_t dispatches_rear;
    uint32_t quarantine_cnt;
    uint32_t caller_stripes;
    uint32_t sync_yields;
    uint32_t helper_stripes[V71R7_SMP_HELPER_COUNT];
    uint64_t generation;
    uint64_t join_timeout_ms_override; /* 0 = default (50ms JPEG / 200ms Rear) */
    V71r7SmpJob    job_ring[V71R7_SMP_JOB_SLOTS];
    V71r7SmpHelper helpers[V71R7_SMP_HELPER_COUNT];
} V71r7SmpPool;

/* Telemetry snapshot for host tests & diagnostics */
typedef struct {
    uint32_t started;
    uint32_t available_helpers;
    uint32_t online_helpers;
    uint32_t dispatches_jpeg;
    uint32_t dispatches_rear;
    uint32_t rear_preempted_cnt;
    uint32_t quarantine_cnt;
    uint32_t caller_stripes;
    uint32_t sync_yields;
    uint32_t helper_stripes[V71R7_SMP_HELPER_COUNT];
    uint32_t helper_sentinel[3];
} V71r7SmpStats;

/* Accessors / test helpers */
void v71r7_smp_get_stats(V71r7SmpStats *out);
int  v71r7_smp_quarantine_drained(void);
void v71r7_smp_set_join_timeout_ms(uint64_t ms);

/* Internal pool getters implemented in lut_runtime.c (returns &LutCoreCtx->smp) */
V71r7SmpPool *v71r7_lut_core_smp_pool(void);
V71r7SmpPool *v71r7_lut_core_smp_pool_peek(void);

#endif /* V71R7_SMP_H */
