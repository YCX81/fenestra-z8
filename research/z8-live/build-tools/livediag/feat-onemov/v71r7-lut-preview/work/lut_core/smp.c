/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Dual-Core CPU2 + CPU3 Lock-Free Stripe Pool (Hardening #5).
 *
 * Strictly scalar (-mgeneral-regs-only), zero FP, zero SIMD, zero .data/.bss.
 * Bare-metal uses stock Z8 C3.11 cre_tsk (0x40185064), sta_tsk (0x401855E0),
 * del_tsk (0x4018543C), dly_tsk (0x40187B00), and semaphores (0x40180E50/102C/11E8/15CC).
 * Host builds (V71R7_HOST_TEST) use POSIX pthreads for real parallel ASan+UBSan testing.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "smp.h"
#include "neon_sentinel.h"

#ifdef V71R7_HOST_TEST
#include <sched.h>
#include <time.h>
#endif

#define SMP_FN(addr, type) ((type)(uintptr_t)(addr))

/* Verified Z8 C3.11 RTOS addresses */
#define Z8_ADDR_TASK_CREATE  UINT64_C(0x40185064)
#define Z8_ADDR_TASK_DELETE  UINT64_C(0x4018543C)
#define Z8_ADDR_TASK_RESUME  UINT64_C(0x401855E0)
#define Z8_ADDR_TASK_DELAY   UINT64_C(0x40187B00)
#define Z8_ADDR_SEM_CREATE   UINT64_C(0x40180E50)
#define Z8_ADDR_SEM_DELETE   UINT64_C(0x4018102C)
#define Z8_ADDR_SEM_SIGNAL   UINT64_C(0x401811E8)
#define Z8_ADDR_SEM_WAIT     UINT64_C(0x401815CC)
#define Z8_ADDR_HEAP_ALLOC   UINT64_C(0x40D2C408)
#define Z8_ADDR_HEAP_FREE    UINT64_C(0x40D2C448)

#define SMP_SEM_ALREADY_SIGNALED (-43)

typedef struct {
    uint32_t  attributes;
    uint32_t  reserved;
    uintptr_t context;
    uintptr_t entry;
    uint32_t  priority;
    uint32_t  reserved2;
    uint64_t  stack_bytes;
    uintptr_t stack;
    uint32_t  processor;
    uint32_t  affinity;
} V71r7RtosTask;

_Static_assert(sizeof(V71r7RtosTask) == 56u &&
               offsetof(V71r7RtosTask, entry) == 16u &&
               offsetof(V71r7RtosTask, stack_bytes) == 32u &&
               offsetof(V71r7RtosTask, affinity) == 52u,
               "Z8 C3.11 cre_tsk descriptor ABI");

static unsigned smp_current_core(void)
{
#ifdef V71R7_HOST_TEST
    return 1u;
#else
    uint64_t mpidr = 0u;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (unsigned)(mpidr & 15u) + 1u;
#endif
}

static int smp_can_schedule(void)
{
#ifdef V71R7_HOST_TEST
    return 1;
#else
    uint64_t daif = 0u;
    const volatile uint32_t *pcb = 0;
    __asm__ volatile("mrs %0, daif; mrs %1, tpidr_el1" : "=r"(daif), "=r"(pcb));
    return (daif & 0x80u) == 0u && pcb != 0 && pcb[0x50 / 4u] == 0u && pcb[0x14 / 4u] != 0u;
#endif
}

static uint64_t smp_now_us(void)
{
#ifdef V71R7_HOST_TEST
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) + (uint64_t)ts.tv_nsec / UINT64_C(1000);
#else
    uint64_t t = 0u, hz = 0u;
    __asm__ volatile("mrs %0, cntvct_el0; mrs %1, cntfrq_el0" : "=r"(t), "=r"(hz));
    return hz ? (t / hz * UINT64_C(1000000) + (t % hz) * UINT64_C(1000000) / hz) : 0u;
#endif
}

static int32_t smp_delay_ms(uint32_t ms)
{
#ifdef V71R7_HOST_TEST
    if (ms == 0u) {
        sched_yield();
        return 0;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, 0);
    return 0;
#else
    return SMP_FN(Z8_ADDR_TASK_DELAY, int32_t (*)(uint32_t))(ms);
#endif
}

static void smp_backoff_short(void)
{
#ifdef V71R7_HOST_TEST
    sched_yield();
#else
    uint64_t start = 0u, now = 0u;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(start));
    do {
        __asm__ volatile("yield; mrs %0, cntpct_el0" : "=r"(now));
    } while (now - start < 2400u);
#endif
}

static inline int smp_claim_stripe(V71r7SmpJob *job, uint32_t *first_out, uint32_t *end_out)
{
    uint32_t start = __atomic_fetch_add(&job->next_row, job->stripe_rows, __ATOMIC_RELAXED);
    if (start >= job->total_rows) {
        return 0;
    }
    uint32_t lim = start + job->stripe_rows;
    *first_out = start;
    *end_out = (lim < job->total_rows) ? lim : job->total_rows;
    return 1;
}

static void smp_helper_run_job(V71r7SmpHelper *w, V71r7SmpJob *job, uint64_t expected_gen)
{
    V71r7SmpPool *pool = w->pool;
    if (__atomic_load_n(&job->generation, __ATOMIC_ACQUIRE) != expected_gen) {
        return;
    }
    uint64_t owner = job->owner;
    unsigned hb_base = (owner == FX_Z8_LUT_HELPER_JPEG)
                           ? FX_Z8_HB_JPEG_HELPER1
                           : FX_Z8_HB_REAR_HELPER1;
    unsigned hb_slot = hb_base + (w->slot - 1u);
    unsigned stage = (owner == FX_Z8_LUT_HELPER_JPEG)
                         ? FX_Z8_STAGE_JPEG_GRADE
                         : FX_Z8_STAGE_REAR_GRADE;

    v71r7_heartbeat_enter(hb_slot, stage, (uint32_t)expected_gen, w->core);
    for (;;) {
        if (__atomic_load_n(&job->cancelled, __ATOMIC_ACQUIRE) != 0u ||
            __atomic_load_n(&job->generation, __ATOMIC_ACQUIRE) != expected_gen) {
            break;
        }
        /* Cooperative preemption: yield helper immediately when Still JPEG waits or pool suspends */
        if (owner == FX_Z8_LUT_HELPER_REAR) {
            if (__atomic_load_n(&pool->jpeg_waiting, __ATOMIC_ACQUIRE) != 0u) {
                break;
            }
            const V71r7DramAnchor *anc = v71r7_anchor();
            if (__atomic_load_n(&anc->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC &&
                (__atomic_load_n(&anc->flags, __ATOMIC_ACQUIRE) & V71R7_ANCHOR_FLAG_POOL_SUSPENDED)) {
                break;
            }
        }
        uint32_t r0 = 0u, r1 = 0u;
        if (!smp_claim_stripe(job, &r0, &r1)) {
            break;
        }
        job->fn(job->ctx, r0, r1, w->slot);
        __atomic_fetch_add(&pool->helper_stripes[w->slot - 1u], 1u, __ATOMIC_RELAXED);
        v71r7_heartbeat_progress(hb_slot);
    }
    v71r7_heartbeat_idle(hb_slot);
}

#ifdef V71R7_HOST_TEST

static void *smp_host_worker_thread(void *arg)
{
    V71r7SmpHelper *w = (V71r7SmpHelper *)arg;
    int probe_ok = v71r7_neon_sentinel_probe_once();
    uint32_t probe_st = probe_ok ? V71R7_NEON_PASS : v71r7_neon_sentinel_last_status();
    __atomic_store_n(&w->sentinel_status, probe_st, __ATOMIC_RELEASE);

    for (;;) {
        pthread_mutex_lock(&w->mu);
        while (!w->stop_req && w->seen_seq == w->wake_seq) {
            pthread_cond_wait(&w->cv, &w->mu);
        }
        int stopping = (w->stop_req != 0u);
        w->seen_seq = w->wake_seq;
        pthread_mutex_unlock(&w->mu);

        if (stopping) {
            break;
        }

        uint32_t expected = __atomic_load_n(&w->phase, __ATOMIC_RELAXED);
        if ((expected & V71R7_SMP_PHASE_MASK) != V71R7_SMP_PHASE_QUEUED) {
            continue;
        }
        uint32_t claimed = (expected & ~V71R7_SMP_PHASE_MASK) | V71R7_SMP_PHASE_ACTIVE;
        if (!__atomic_compare_exchange_n(&w->phase, &expected, claimed,
                                         0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            continue;
        }

        V71r7SmpJob *job = w->job;
        uint64_t expected_gen = w->job_gen;
        if (job && job->fn) {
            smp_helper_run_job(w, job, expected_gen);
        }

        uint32_t parked = (expected & ~V71R7_SMP_PHASE_MASK) | V71R7_SMP_PHASE_PARKED;
        if (!__atomic_compare_exchange_n(&w->phase, &claimed, parked,
                                         0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            /* Caller quarantined us on timeout -> mark QUAR_DRAINED (5, NOT QUEUED=2!) and exit */
            uint32_t drained = (claimed & ~V71R7_SMP_PHASE_MASK) | V71R7_SMP_PHASE_QUAR_DRAINED;
            __atomic_store_n(&w->phase, drained, __ATOMIC_RELEASE);
            return 0;
        }
    }
    uint32_t cur = __atomic_load_n(&w->phase, __ATOMIC_RELAXED);
    if ((cur & V71R7_SMP_PHASE_MASK) != V71R7_SMP_PHASE_QUAR_DRAINED) {
        __atomic_store_n(&w->phase, V71R7_SMP_PHASE_DEAD, __ATOMIC_RELEASE);
    }
    return 0;
}

#else /* Bare-metal Z8 C3.11 EL1 */

static void smp_el1_worker_task(void *arg)
{
    V71r7SmpHelper *w = (V71r7SmpHelper *)arg;
    int probe_ok = v71r7_neon_sentinel_probe_once();
    uint32_t probe_st = probe_ok ? V71R7_NEON_PASS : v71r7_neon_sentinel_last_status();
    __atomic_store_n(&w->sentinel_status, probe_st, __ATOMIC_RELEASE);

    for (;;) {
        if (__atomic_load_n(&w->pool->stopped, __ATOMIC_ACQUIRE) != 0u) {
            break;
        }
        int32_t wrc = SMP_FN(Z8_ADDR_SEM_WAIT, int32_t (*)(int32_t))(w->wake_sem);
        if (wrc != 0) {
            if (smp_delay_ms(1u) != 0) {
                break;
            }
            continue;
        }
        if (__atomic_load_n(&w->pool->stopped, __ATOMIC_ACQUIRE) != 0u) {
            break;
        }
        uint32_t expected = __atomic_load_n(&w->phase, __ATOMIC_RELAXED);
        if ((expected & V71R7_SMP_PHASE_MASK) != V71R7_SMP_PHASE_QUEUED) {
            continue;
        }
        uint32_t claimed = (expected & ~V71R7_SMP_PHASE_MASK) | V71R7_SMP_PHASE_ACTIVE;
        if (!__atomic_compare_exchange_n(&w->phase, &expected, claimed,
                                         0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            continue;
        }

        V71r7SmpJob *job = w->job;
        uint64_t expected_gen = w->job_gen;
        if (job && job->fn && smp_current_core() == w->core) {
            smp_helper_run_job(w, job, expected_gen);
        }

        uint32_t parked = (expected & ~V71R7_SMP_PHASE_MASK) | V71R7_SMP_PHASE_PARKED;
        if (!__atomic_compare_exchange_n(&w->phase, &claimed, parked,
                                         0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            /* Caller quarantined us on timeout -> mark QUAR_DRAINED (5) and exit permanently */
            uint32_t drained = (claimed & ~V71R7_SMP_PHASE_MASK) | V71R7_SMP_PHASE_QUAR_DRAINED;
            __atomic_store_n(&w->phase, drained, __ATOMIC_RELEASE);
            return;
        }
    }
    __atomic_store_n(&w->phase, V71R7_SMP_PHASE_DEAD, __ATOMIC_RELEASE);
}

#endif /* V71R7_HOST_TEST */

int v71r7_smp_ensure_started(void)
{
    V71r7SmpPool *pool = v71r7_lut_core_smp_pool();
    if (!pool || !smp_can_schedule()) {
        return 0;
    }
    if (__atomic_load_n(&pool->initialized, __ATOMIC_ACQUIRE) == 2u) {
        return 1;
    }
    uint32_t exp = 0u;
    if (!__atomic_compare_exchange_n(&pool->initialized, &exp, 1u,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        /* Wait briefly if another core is mid-init */
        for (unsigned i = 0u; i < 50u; ++i) {
            if (__atomic_load_n(&pool->initialized, __ATOMIC_ACQUIRE) == 2u) {
                return 1;
            }
            smp_backoff_short();
        }
        return __atomic_load_n(&pool->initialized, __ATOMIC_ACQUIRE) == 2u;
    }

    pool->magic = V71R7_ANCHOR_MAGIC;
    __atomic_store_n(&pool->stopped, 0u, __ATOMIC_RELEASE);

    unsigned started = 0u;
    for (unsigned i = 0u; i < V71R7_SMP_HELPER_COUNT; ++i) {
        V71r7SmpHelper *w = &pool->helpers[i];
        memset(w, 0, sizeof(*w));
        w->slot = i + 1u;
        w->core = 2u + i; /* Z8 Core 2 (CPU1) and Core 3 (CPU2) */
        w->pool = pool;

#ifdef V71R7_HOST_TEST
        if (pthread_mutex_init(&w->mu, 0) != 0) {
            continue;
        }
        if (pthread_cond_init(&w->cv, 0) != 0) {
            pthread_mutex_destroy(&w->mu);
            continue;
        }
        __atomic_store_n(&w->phase, V71R7_SMP_PHASE_PARKED, __ATOMIC_RELEASE);
        if (pthread_create(&w->thread, 0, smp_host_worker_thread, w) == 0) {
            w->thread_started = 1u;
            ++started;
        } else {
            __atomic_store_n(&w->phase, V71R7_SMP_PHASE_DEAD, __ATOMIC_RELEASE);
            pthread_cond_destroy(&w->cv);
            pthread_mutex_destroy(&w->mu);
        }
#else
        const uint32_t sem_cfg[3] = { 0u, 0u, 1u };
        int32_t sem = SMP_FN(Z8_ADDR_SEM_CREATE, int32_t (*)(const uint32_t *))(sem_cfg);
        if (sem < 1) {
            continue;
        }
        void *stack_raw = SMP_FN(Z8_ADDR_HEAP_ALLOC, void *(*)(uint32_t, uint32_t))(
            V71R7_SMP_STACK_BYTES + 16u, 0u);
        if (!stack_raw) {
            (void)SMP_FN(Z8_ADDR_SEM_DELETE, int32_t (*)(int32_t))(sem);
            continue;
        }
        uintptr_t stack_aligned = ((uintptr_t)stack_raw + 15u) & ~(uintptr_t)15u;
        w->wake_sem = sem;
        w->stack_raw = stack_raw;

        const V71r7RtosTask task = {
            .attributes  = 0u,
            .reserved    = 0u,
            .context     = (uintptr_t)w,
            .entry       = (uintptr_t)smp_el1_worker_task,
            .priority    = V71R7_SMP_PRIORITY,
            .reserved2   = 0u,
            .stack_bytes = V71R7_SMP_STACK_BYTES,
            .stack       = stack_aligned,
            .processor   = w->core,
            .affinity    = 1u << (w->core - 1u)
        };
        int32_t tid = SMP_FN(Z8_ADDR_TASK_CREATE, int32_t (*)(const V71r7RtosTask *))(&task);
        if (tid > 0) {
            w->task_id = tid;
            __atomic_store_n(&w->phase, V71R7_SMP_PHASE_PARKED, __ATOMIC_RELEASE);
            if (SMP_FN(Z8_ADDR_TASK_RESUME, int32_t (*)(int32_t))(tid) == 0) {
                ++started;
                continue;
            }
            __atomic_store_n(&w->phase, V71R7_SMP_PHASE_DEAD, __ATOMIC_RELEASE);
            if (SMP_FN(Z8_ADDR_TASK_DELETE, int32_t (*)(int32_t))(tid) != 0) {
                /* Retain sem/stack if kernel refused delete */
                continue;
            }
        }
        SMP_FN(Z8_ADDR_HEAP_FREE, void (*)(void *))(stack_raw);
        w->stack_raw = 0;
        (void)SMP_FN(Z8_ADDR_SEM_DELETE, int32_t (*)(int32_t))(sem);
        w->wake_sem = 0;
#endif
    }

    __atomic_store_n(&pool->initialized, 2u, __ATOMIC_RELEASE);
    return started > 0u;
}

static uint32_t smp_revoke_queued(V71r7SmpHelper *w, uint32_t state)
{
    if ((state & V71R7_SMP_PHASE_MASK) == V71R7_SMP_PHASE_QUEUED) {
        uint32_t parked = state ^ (V71R7_SMP_PHASE_QUEUED ^ V71R7_SMP_PHASE_PARKED);
        if (__atomic_compare_exchange_n(&w->phase, &state, parked,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return parked;
        }
    }
    return state;
}

static int smp_wake_helper(V71r7SmpHelper *w)
{
#ifdef V71R7_HOST_TEST
    pthread_mutex_lock(&w->mu);
    w->wake_seq += 1u;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    return 1;
#else
    int32_t rc = SMP_FN(Z8_ADDR_SEM_SIGNAL, int32_t (*)(int32_t))(w->wake_sem);
    return (rc == 0 || rc == SMP_SEM_ALREADY_SIGNALED);
#endif
}

static void smp_run_sync_only(V71r7SmpPool *pool, uint32_t total_rows, uint32_t stripe_rows,
                              V71r7SmpStripeFn fn, void *ctx, unsigned caller_hb_slot,
                              uint64_t helper_owner)
{
    unsigned stage = (helper_owner == FX_Z8_LUT_HELPER_JPEG)
                         ? FX_Z8_STAGE_JPEG_GRADE
                         : FX_Z8_STAGE_REAR_GRADE;
    if (caller_hb_slot < FX_Z8_HB_COUNT) {
        v71r7_heartbeat_enter(caller_hb_slot, stage, 0u, 0u);
    }
    uint32_t stripe_idx = 0u;
    for (uint32_t r = 0u; r < total_rows; r += stripe_rows, ++stripe_idx) {
        uint32_t end = (r + stripe_rows < total_rows) ? (r + stripe_rows) : total_rows;
        fn(ctx, r, end, 0u);
        if (pool) {
            __atomic_fetch_add(&pool->caller_stripes, 1u, __ATOMIC_RELAXED);
        }
        if (caller_hb_slot < FX_Z8_HB_COUNT) {
            v71r7_heartbeat_progress(caller_hb_slot);
        }
        /* P1 Hardening: yield CPU2 every 8 stripes during 45MP Still 3D LUT via dly_tsk(0)
         * so AFLENS, photo_worker, and WTDG_C3 stay responsive. */
        if (helper_owner == FX_Z8_LUT_HELPER_JPEG && ((stripe_idx + 1u) & 7u) == 0u &&
            end < total_rows && smp_can_schedule()) {
            (void)smp_delay_ms(0u);
            if (pool) {
                __atomic_fetch_add(&pool->sync_yields, 1u, __ATOMIC_RELAXED);
            }
        }
    }
    if (caller_hb_slot < FX_Z8_HB_COUNT) {
        v71r7_heartbeat_idle(caller_hb_slot);
    }
}

/* Pick a job_ring slot that is NOT referenced by any still-QUARANTINED helper. */
static V71r7SmpJob *smp_pick_free_job_slot(V71r7SmpPool *pool, uint64_t gen)
{
    for (unsigned attempt = 0u; attempt < V71R7_SMP_JOB_SLOTS; ++attempt) {
        V71r7SmpJob *cand = &pool->job_ring[(gen + attempt) % V71R7_SMP_JOB_SLOTS];
        int pinned = 0;
        for (unsigned i = 0u; i < V71R7_SMP_HELPER_COUNT; ++i) {
            uint32_t ph = __atomic_load_n(&pool->helpers[i].phase, __ATOMIC_ACQUIRE) & V71R7_SMP_PHASE_MASK;
            if (ph == V71R7_SMP_PHASE_QUARANTINED && pool->helpers[i].job == cand) {
                pinned = 1;
                break;
            }
        }
        if (!pinned) {
            return cand;
        }
    }
    return 0;
}

static int smp_run_stripes_internal(uint64_t helper_owner, uint32_t total_rows, uint32_t stripe_rows,
                                    V71r7SmpStripeFn fn, const void *ctx_in, size_t copy_bytes,
                                    unsigned caller_hb_slot)
{
    if (!fn || total_rows == 0u) {
        return 0;
    }
    if (stripe_rows == 0u || stripe_rows > total_rows) {
        stripe_rows = total_rows;
    }

    V71r7SmpPool *pool = v71r7_lut_core_smp_pool_peek();
    if (!pool || __atomic_load_n(&pool->initialized, __ATOMIC_ACQUIRE) != 2u ||
        __atomic_load_n(&pool->stopped, __ATOMIC_ACQUIRE) != 0u ||
        total_rows <= stripe_rows || !smp_can_schedule() ||
        (copy_bytes != 0u && (!ctx_in || copy_bytes > V71R7_SMP_INLINE_CTX_BYTES))) {
        smp_run_sync_only(pool, total_rows, stripe_rows, fn, (void *)(uintptr_t)ctx_in,
                          caller_hb_slot, helper_owner);
        return 0;
    }

    FxZ8LutIoGate *gate = v71r7_io_gate();
    int is_jpeg = (helper_owner == FX_Z8_LUT_HELPER_JPEG);
    int took_gate_helpers = 0;
    int took_local_lock = 0;

    if (is_jpeg) {
        __atomic_fetch_add(&pool->jpeg_waiting, 1u, __ATOMIC_ACQ_REL);
    }

    /* Arbitrate helper ownership through FxZ8LutIoGate (JPEG preempts Rear!) */
    uint64_t gstate = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    uint32_t epoch = (gstate & 1u) ? (uint32_t)(gstate >> 32) : 0u;

    if (epoch != 0u) {
        if (is_jpeg) {
            /* Bounded wait up to ~5 ms for any Rear stripe to release helpers */
            for (unsigned attempt = 0u; attempt < 25u; ++attempt) {
                epoch = fx_z8_lut_io_epoch(gate);
                if (!epoch) {
                    break;
                }
                if (fx_z8_lut_helpers_take(gate, epoch, FX_Z8_LUT_HELPER_JPEG)) {
                    took_gate_helpers = 1;
                    break;
                }
                smp_backoff_short();
                if ((attempt & 3u) == 3u) {
                    (void)smp_delay_ms(1u);
                }
            }
        } else {
            /* Rear is strictly non-blocking and yields whenever JPEG is waiting or active */
            if (__atomic_load_n(&pool->jpeg_waiting, __ATOMIC_ACQUIRE) == 0u &&
                fx_z8_lut_helpers_take(gate, epoch, FX_Z8_LUT_HELPER_REAR)) {
                took_gate_helpers = 1;
            } else {
                __atomic_fetch_add(&pool->rear_preempted_cnt, 1u, __ATOMIC_RELAXED);
            }
        }
    } else if ((gstate & (FX_Z8_LUT_IO_STOPPED | FX_Z8_LUT_IO_EARLY_OFF)) == 0u) {
        /* Gate not opened yet (e.g. standalone early test): serialize via pool->local_lock */
        if (is_jpeg) {
            for (unsigned attempt = 0u; attempt < 25u; ++attempt) {
                uint32_t zero = 0u;
                if (__atomic_compare_exchange_n(&pool->local_lock, &zero, 1u,
                                                0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    took_local_lock = 1;
                    break;
                }
                smp_backoff_short();
            }
        } else {
            uint32_t zero = 0u;
            if (__atomic_load_n(&pool->jpeg_waiting, __ATOMIC_ACQUIRE) == 0u &&
                __atomic_compare_exchange_n(&pool->local_lock, &zero, 1u,
                                            0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                took_local_lock = 1;
            } else {
                __atomic_fetch_add(&pool->rear_preempted_cnt, 1u, __ATOMIC_RELAXED);
            }
        }
    }

    if (!took_gate_helpers && !took_local_lock) {
        if (is_jpeg) {
            __atomic_fetch_sub(&pool->jpeg_waiting, 1u, __ATOMIC_RELEASE);
        }
        smp_run_sync_only(pool, total_rows, stripe_rows, fn, (void *)(uintptr_t)ctx_in,
                          caller_hb_slot, helper_owner);
        return 0;
    }

    uint64_t gen = __atomic_fetch_add(&pool->generation, 1u, __ATOMIC_RELAXED) + 1u;
    V71r7SmpJob *job = smp_pick_free_job_slot(pool, gen);
    if (!job) {
        if (took_gate_helpers) {
            fx_z8_lut_helpers_drop(gate, helper_owner);
        }
        if (took_local_lock) {
            __atomic_store_n(&pool->local_lock, 0u, __ATOMIC_RELEASE);
        }
        if (is_jpeg) {
            __atomic_fetch_sub(&pool->jpeg_waiting, 1u, __ATOMIC_RELEASE);
        }
        smp_run_sync_only(pool, total_rows, stripe_rows, fn, (void *)(uintptr_t)ctx_in,
                          caller_hb_slot, helper_owner);
        return 0;
    }

    if (is_jpeg) {
        __atomic_fetch_add(&pool->dispatches_jpeg, 1u, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&pool->dispatches_rear, 1u, __ATOMIC_RELAXED);
    }

    __atomic_store_n(&job->next_row, 0u, __ATOMIC_RELAXED);
    job->total_rows  = total_rows;
    job->stripe_rows = stripe_rows;
    __atomic_store_n(&job->cancelled, 0u, __ATOMIC_RELAXED);
    job->owner = helper_owner;
    job->fn    = fn;
    if (copy_bytes != 0u) {
        memcpy(job->ctx_storage, ctx_in, copy_bytes);
        job->ctx = job->ctx_storage;
    } else {
        job->ctx = (void *)(uintptr_t)ctx_in;
    }
    __atomic_store_n(&job->generation, gen, __ATOMIC_RELEASE);

    unsigned caller_core = smp_current_core();
    unsigned dispatched_mask = 0u;

    if (caller_hb_slot < FX_Z8_HB_COUNT) {
        v71r7_heartbeat_enter(caller_hb_slot,
                              is_jpeg ? FX_Z8_STAGE_JPEG_SMP_DISPATCH : FX_Z8_STAGE_REAR_GRADE,
                              (uint32_t)gen, total_rows);
    }

    for (unsigned i = 0u; i < V71R7_SMP_HELPER_COUNT; ++i) {
        V71r7SmpHelper *w = &pool->helpers[i];
#ifndef V71R7_HOST_TEST
        if (w->core == caller_core) {
            continue;
        }
#else
        (void)caller_core;
#endif
        uint32_t st = __atomic_load_n(&w->phase, __ATOMIC_ACQUIRE);
        if ((st & V71R7_SMP_PHASE_MASK) != V71R7_SMP_PHASE_PARKED) {
            continue;
        }
        w->job = job;
        w->job_gen = gen;
        uint32_t queued = ((st & ~V71R7_SMP_PHASE_MASK) + V71R7_SMP_NEXT_GEN) | V71R7_SMP_PHASE_QUEUED;
        if (!__atomic_compare_exchange_n(&w->phase, &st, queued,
                                         0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            continue;
        }
        dispatched_mask |= (1u << i);
        if (!smp_wake_helper(w)) {
            /* Revoke if wake failed */
            (void)smp_revoke_queued(w, __atomic_load_n(&w->phase, __ATOMIC_ACQUIRE));
        }
    }

    /* Caller also steals stripes cooperatively (worker_idx = 0) */
    if (caller_hb_slot < FX_Z8_HB_COUNT) {
        v71r7_heartbeat_enter(caller_hb_slot,
                              is_jpeg ? FX_Z8_STAGE_JPEG_GRADE : FX_Z8_STAGE_REAR_GRADE,
                              (uint32_t)gen, dispatched_mask);
    }
    uint32_t caller_stripe_idx = 0u;
    for (;;) {
        uint32_t r0 = 0u, r1 = 0u;
        if (!smp_claim_stripe(job, &r0, &r1)) {
            break;
        }
        fn(job->ctx, r0, r1, 0u);
        __atomic_fetch_add(&pool->caller_stripes, 1u, __ATOMIC_RELAXED);
        ++caller_stripe_idx;
        if (caller_hb_slot < FX_Z8_HB_COUNT) {
            v71r7_heartbeat_progress(caller_hb_slot);
        }
        if (is_jpeg && (caller_stripe_idx & 7u) == 0u && r1 < total_rows && smp_can_schedule()) {
            (void)smp_delay_ms(0u);
            __atomic_fetch_add(&pool->sync_yields, 1u, __ATOMIC_RELAXED);
        }
    }

    /* All stripes claimed: revoke any still-QUEUED helpers, then join ACTIVE helpers */
    if (caller_hb_slot < FX_Z8_HB_COUNT) {
        v71r7_heartbeat_enter(caller_hb_slot,
                              is_jpeg ? FX_Z8_STAGE_JPEG_SMP_JOIN : FX_Z8_STAGE_REAR_JOIN,
                              (uint32_t)gen, dispatched_mask);
    }

    for (unsigned i = 0u; i < V71R7_SMP_HELPER_COUNT; ++i) {
        if (!(dispatched_mask & (1u << i))) {
            continue;
        }
        V71r7SmpHelper *w = &pool->helpers[i];
        uint32_t st = smp_revoke_queued(w, __atomic_load_n(&w->phase, __ATOMIC_ACQUIRE));
        if ((st & V71R7_SMP_PHASE_MASK) <= V71R7_SMP_PHASE_PARKED) {
            dispatched_mask &= ~(1u << i);
        }
    }

    uint64_t t_join_start = smp_now_us();
    uint64_t override_ms = __atomic_load_n(&pool->join_timeout_ms_override, __ATOMIC_ACQUIRE);
    uint64_t timeout_us = (override_ms ? override_ms : (is_jpeg ? 50u : 200u)) * UINT64_C(1000);
    unsigned quarantined_mask = 0u;

    while (dispatched_mask != 0u) {
        uint64_t now = smp_now_us();
        int expired = (now >= t_join_start && (now - t_join_start) >= timeout_us);
        for (unsigned i = 0u; i < V71R7_SMP_HELPER_COUNT; ++i) {
            if (!(dispatched_mask & (1u << i))) {
                continue;
            }
            V71r7SmpHelper *w = &pool->helpers[i];
            uint32_t st = __atomic_load_n(&w->phase, __ATOMIC_ACQUIRE);
            st = smp_revoke_queued(w, st);
            uint32_t ph = st & V71R7_SMP_PHASE_MASK;
            if (ph <= V71R7_SMP_PHASE_PARKED) {
                dispatched_mask &= ~(1u << i);
                continue;
            }
            if (expired && ph == V71R7_SMP_PHASE_ACTIVE) {
                __atomic_store_n(&job->cancelled, 1u, __ATOMIC_RELEASE);
                uint32_t quar = (st & ~V71R7_SMP_PHASE_MASK) | V71R7_SMP_PHASE_QUARANTINED;
                if (__atomic_compare_exchange_n(&w->phase, &st, quar,
                                                0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    dispatched_mask &= ~(1u << i);
                    quarantined_mask |= (1u << i);
                    __atomic_fetch_add(&pool->quarantine_cnt, 1u, __ATOMIC_RELAXED);
                    unsigned hb_base = is_jpeg ? FX_Z8_HB_JPEG_HELPER1 : FX_Z8_HB_REAR_HELPER1;
                    v71r7_flight_record(FX_Z8_FLIGHT_SMP_QUARANTINE, (uint16_t)smp_current_core(),
                                        hb_base + i, w->core, (uint32_t)gen);
                }
            }
        }
        if (dispatched_mask != 0u) {
            smp_backoff_short();
            if (caller_hb_slot < FX_Z8_HB_COUNT) {
                v71r7_heartbeat_progress(caller_hb_slot);
            }
        }
    }

    /* Post-quarantine bounded grace drain (production timeout only): give a preempted
     * helper up to 50 ms to finish its single 16/32-row in-flight stripe and reach
     * QUAR_DRAINED so caller can safely release cube/bank without triggering quarantine. */
    unsigned undrained_mask = 0u;
    if (quarantined_mask != 0u) {
        if (override_ms == 0u) {
            for (unsigned d = 0u; d < 50u; ++d) {
                unsigned still = 0u;
                for (unsigned i = 0u; i < V71R7_SMP_HELPER_COUNT; ++i) {
                    if (!(quarantined_mask & (1u << i))) {
                        continue;
                    }
                    uint32_t ph = __atomic_load_n(&pool->helpers[i].phase, __ATOMIC_ACQUIRE) &
                                  V71R7_SMP_PHASE_MASK;
                    if (ph == V71R7_SMP_PHASE_QUARANTINED) {
                        still |= (1u << i);
                    }
                }
                if (still == 0u) {
                    break;
                }
                (void)smp_delay_ms(1u);
            }
        }
        for (unsigned i = 0u; i < V71R7_SMP_HELPER_COUNT; ++i) {
            if (!(quarantined_mask & (1u << i))) {
                continue;
            }
            uint32_t ph = __atomic_load_n(&pool->helpers[i].phase, __ATOMIC_ACQUIRE) &
                          V71R7_SMP_PHASE_MASK;
            if (ph == V71R7_SMP_PHASE_QUARANTINED) {
                undrained_mask |= (1u << i);
            }
        }
    }

    if (caller_hb_slot < FX_Z8_HB_COUNT) {
        v71r7_heartbeat_idle(caller_hb_slot);
    }

    if (took_gate_helpers) {
        fx_z8_lut_helpers_drop(gate, helper_owner);
    }
    if (took_local_lock) {
        __atomic_store_n(&pool->local_lock, 0u, __ATOMIC_RELEASE);
    }
    if (is_jpeg) {
        __atomic_fetch_sub(&pool->jpeg_waiting, 1u, __ATOMIC_RELEASE);
    }
    return (int)undrained_mask;
}

void v71r7_smp_run_stripes(uint64_t helper_owner, uint32_t total_rows, uint32_t stripe_rows,
                           V71r7SmpStripeFn fn, void *ctx, unsigned caller_hb_slot)
{
    (void)smp_run_stripes_internal(helper_owner, total_rows, stripe_rows, fn, ctx, 0u, caller_hb_slot);
}

int v71r7_smp_run_stripes_copy(uint64_t helper_owner, uint32_t total_rows, uint32_t stripe_rows,
                               V71r7SmpStripeFn fn, const void *ctx_src, size_t ctx_bytes,
                               unsigned caller_hb_slot)
{
    return smp_run_stripes_internal(helper_owner, total_rows, stripe_rows, fn, ctx_src,
                                    ctx_bytes, caller_hb_slot);
}

void v71r7_smp_stop(void)
{
    V71r7SmpPool *pool = v71r7_lut_core_smp_pool_peek();
    if (!pool || __atomic_load_n(&pool->initialized, __ATOMIC_ACQUIRE) != 2u) {
        return;
    }
    __atomic_store_n(&pool->stopped, 1u, __ATOMIC_RELEASE);

#ifdef V71R7_HOST_TEST
    for (unsigned i = 0u; i < V71R7_SMP_HELPER_COUNT; ++i) {
        V71r7SmpHelper *w = &pool->helpers[i];
        if (!w->thread_started) {
            continue;
        }
        pthread_mutex_lock(&w->mu);
        w->stop_req = 1u;
        w->wake_seq += 1u;
        pthread_cond_signal(&w->cv);
        pthread_mutex_unlock(&w->mu);
        pthread_join(w->thread, 0);
        w->thread_started = 0u;
        pthread_cond_destroy(&w->cv);
        pthread_mutex_destroy(&w->mu);
    }
#else
    for (unsigned i = 0u; i < V71R7_SMP_HELPER_COUNT; ++i) {
        V71r7SmpHelper *w = &pool->helpers[i];
        if (w->wake_sem > 0) {
            (void)SMP_FN(Z8_ADDR_SEM_SIGNAL, int32_t (*)(int32_t))(w->wake_sem);
        }
    }
#endif
    __atomic_store_n(&pool->initialized, 0u, __ATOMIC_RELEASE);
}

int v71r7_smp_quarantine_drained(void)
{
    V71r7SmpPool *pool = v71r7_lut_core_smp_pool_peek();
    if (!pool || __atomic_load_n(&pool->initialized, __ATOMIC_ACQUIRE) != 2u) {
        return 1;
    }
    for (unsigned i = 0u; i < V71R7_SMP_HELPER_COUNT; ++i) {
        uint32_t ph = __atomic_load_n(&pool->helpers[i].phase, __ATOMIC_ACQUIRE) & V71R7_SMP_PHASE_MASK;
        if (ph == V71R7_SMP_PHASE_QUARANTINED) {
            return 0;
        }
    }
    return 1;
}

uint32_t v71r7_smp_online_helpers(void)
{
    V71r7SmpPool *pool = v71r7_lut_core_smp_pool_peek();
    if (!pool || __atomic_load_n(&pool->initialized, __ATOMIC_ACQUIRE) != 2u ||
        __atomic_load_n(&pool->stopped, __ATOMIC_ACQUIRE) != 0u) {
        return 0u;
    }
    uint32_t n = 0u;
    for (unsigned i = 0u; i < V71R7_SMP_HELPER_COUNT; ++i) {
        uint32_t ph = __atomic_load_n(&pool->helpers[i].phase, __ATOMIC_ACQUIRE) & V71R7_SMP_PHASE_MASK;
        if (ph == V71R7_SMP_PHASE_PARKED || ph == V71R7_SMP_PHASE_QUEUED ||
            ph == V71R7_SMP_PHASE_ACTIVE) {
            ++n;
        }
    }
    return n;
}

void v71r7_smp_get_helper_sentinels(uint32_t out[3])
{
    if (!out) {
        return;
    }
    out[0] = v71r7_neon_sentinel_last_status();
    out[1] = 0u;
    out[2] = 0u;
    V71r7SmpPool *pool = v71r7_lut_core_smp_pool_peek();
    if (!pool || __atomic_load_n(&pool->initialized, __ATOMIC_ACQUIRE) != 2u) {
        return;
    }
    out[1] = __atomic_load_n(&pool->helpers[0].sentinel_status, __ATOMIC_ACQUIRE);
    out[2] = __atomic_load_n(&pool->helpers[1].sentinel_status, __ATOMIC_ACQUIRE);
}

void v71r7_smp_set_join_timeout_ms(uint64_t ms)
{
    V71r7SmpPool *pool = v71r7_lut_core_smp_pool();
    if (pool) {
        __atomic_store_n(&pool->join_timeout_ms_override, ms, __ATOMIC_RELEASE);
    }
}

void v71r7_smp_get_stats(V71r7SmpStats *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    v71r7_smp_get_helper_sentinels(out->helper_sentinel);
    V71r7SmpPool *pool = v71r7_lut_core_smp_pool_peek();
    if (!pool) {
        return;
    }
    out->sync_yields    = __atomic_load_n(&pool->sync_yields, __ATOMIC_RELAXED);
    out->caller_stripes = __atomic_load_n(&pool->caller_stripes, __ATOMIC_RELAXED);
    if (__atomic_load_n(&pool->initialized, __ATOMIC_ACQUIRE) != 2u) {
        return;
    }
    out->started = 1u;
    for (unsigned i = 0u; i < V71R7_SMP_HELPER_COUNT; ++i) {
        uint32_t ph = __atomic_load_n(&pool->helpers[i].phase, __ATOMIC_ACQUIRE) & V71R7_SMP_PHASE_MASK;
        if (ph == V71R7_SMP_PHASE_PARKED) {
            out->available_helpers += 1u;
        }
        if (ph == V71R7_SMP_PHASE_PARKED || ph == V71R7_SMP_PHASE_QUEUED ||
            ph == V71R7_SMP_PHASE_ACTIVE) {
            out->online_helpers += 1u;
        }
        out->helper_stripes[i] = __atomic_load_n(&pool->helper_stripes[i], __ATOMIC_RELAXED);
    }
    out->dispatches_jpeg    = __atomic_load_n(&pool->dispatches_jpeg, __ATOMIC_RELAXED);
    out->dispatches_rear    = __atomic_load_n(&pool->dispatches_rear, __ATOMIC_RELAXED);
    out->rear_preempted_cnt = __atomic_load_n(&pool->rear_preempted_cnt, __ATOMIC_RELAXED);
    out->quarantine_cnt     = __atomic_load_n(&pool->quarantine_cnt, __ATOMIC_RELAXED);
}
