/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Runtime NEON Sentinel Control Layer (Hardening #2).
 * Strictly scalar (-mgeneral-regs-only), zero FP, zero SIMD, zero .data/.bss.
 * All mutable state lives in DRAM anchor (0xA11640000) + sentinel slot (0xA1164080).
 */
#include <stddef.h>
#include <stdint.h>
#include "neon_sentinel.h"

#define V71R7_ADDR_DLY_TSK  UINT64_C(0x40187B00)

typedef struct {
    uint32_t magic;
    uint32_t last_status;
    uint32_t probe_runs;
    uint32_t fail_count;
    uint32_t probed_ok;
    uint32_t reserved0;
#ifdef V71R7_HOST_TEST
    V71r7NeonHostProbeFn host_probe;
    void                *host_ctx;
#else
    uint64_t             reserved1[2];
#endif
} V71r7NeonSentinelState;

_Static_assert(sizeof(V71r7NeonSentinelState) <= 128u,
               "V71r7NeonSentinelState must fit in 0xA1164080..0xA1164100");

static inline V71r7NeonSentinelState *sentinel_state(void)
{
    return V71R7_DRAM_PTR(V71r7NeonSentinelState, V71R7_DRAM_NEON_SENTINEL_VA);
}

static inline uint16_t sentinel_core_id(void)
{
#ifdef V71R7_HOST_TEST
    return 1u;
#else
    uint64_t id = 0u;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(id));
    return (uint16_t)((id & 15u) + 1u);
#endif
}

static int32_t sentinel_delay_ms(uint32_t ms)
{
#ifdef V71R7_HOST_TEST
    (void)ms;
    return 0;
#else
    return ((int32_t (*)(uint32_t))(uintptr_t)V71R7_ADDR_DLY_TSK)(ms);
#endif
}

void v71r7_neon_sentinel_latch_disabled(uint32_t reason, uint32_t detail)
{
    V71r7DramAnchor *anchor = v71r7_anchor();
    V71r7NeonSentinelState *st = sentinel_state();
    uint32_t prev = __atomic_fetch_or(&anchor->flags, V71R7_ANCHOR_FLAG_NEON_DISABLED,
                                      __ATOMIC_ACQ_REL);
    __atomic_store_n(&st->last_status, V71R7_NEON_FAILED, __ATOMIC_RELEASE);
    __atomic_fetch_add(&st->fail_count, 1u, __ATOMIC_RELAXED);
    /* Always record first transition and subsequent explicit faults */
    v71r7_flight_record(FX_Z8_FLIGHT_NEON_SENTINEL_FAIL, sentinel_core_id(),
                        reason, detail, (prev & V71R7_ANCHOR_FLAG_NEON_DISABLED) ? 1u : 0u);
}

int v71r7_neon_sentinel_is_allowed(void)
{
    const V71r7DramAnchor *anchor = v71r7_anchor();
    uint32_t flags = __atomic_load_n(&anchor->flags, __ATOMIC_ACQUIRE);
    if (flags & V71R7_ANCHOR_FLAG_NEON_DISABLED) {
        return 0;
    }
    return 1;
}

uint32_t v71r7_neon_sentinel_last_status(void)
{
    const V71r7NeonSentinelState *st = sentinel_state();
    if (__atomic_load_n(&st->magic, __ATOMIC_ACQUIRE) != V71R7_ANCHOR_MAGIC) {
        return V71R7_NEON_UNAVAILABLE;
    }
    return __atomic_load_n(&st->last_status, __ATOMIC_ACQUIRE);
}

int v71r7_neon_sentinel_probe_once(void)
{
    V71r7DramAnchor *anchor = v71r7_anchor();
    V71r7NeonSentinelState *st = sentinel_state();

    if (__atomic_load_n(&anchor->flags, __ATOMIC_ACQUIRE) & V71R7_ANCHOR_FLAG_NEON_DISABLED) {
        return 0;
    }

    if (__atomic_load_n(&st->magic, __ATOMIC_ACQUIRE) != V71R7_ANCHOR_MAGIC) {
        __atomic_store_n(&st->last_status, V71R7_NEON_UNAVAILABLE, __ATOMIC_RELAXED);
        __atomic_store_n(&st->probe_runs, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&st->fail_count, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&st->probed_ok, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&st->magic, V71R7_ANCHOR_MAGIC, __ATOMIC_RELEASE);
    }

    unsigned rc;
#ifdef V71R7_HOST_TEST
    V71r7NeonHostProbeFn fn = __atomic_load_n(&st->host_probe, __ATOMIC_ACQUIRE);
    void *ctx = __atomic_load_n(&st->host_ctx, __ATOMIC_ACQUIRE);
    if (fn) {
        rc = fn(ctx, sentinel_delay_ms);
    } else {
        rc = v71r7_neon_sentinel_hw_probe(sentinel_delay_ms);
    }
#else
    rc = v71r7_neon_sentinel_hw_probe(sentinel_delay_ms);
#endif

    __atomic_fetch_add(&st->probe_runs, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&st->last_status, (uint32_t)rc, __ATOMIC_RELEASE);

    if (rc == V71R7_NEON_PASS) {
        __atomic_store_n(&st->probed_ok, 1u, __ATOMIC_RELEASE);
        return 1;
    }
    if (rc == V71R7_NEON_FAILED) {
        v71r7_neon_sentinel_latch_disabled(V71R7_NEON_FAIL_PROBE_MISMATCH, (uint32_t)rc);
        return 0;
    }
    /* V71R7_NEON_UNAVAILABLE: FP disabled or IRQs masked right now; do not permit NEON
     * on this call, and do not falsely latch permanent hardware failure on transient IRQ mask. */
    return 0;
}

#ifdef V71R7_HOST_TEST
void v71r7_neon_sentinel_set_host_probe(V71r7NeonHostProbeFn fn, void *ctx)
{
    V71r7NeonSentinelState *st = sentinel_state();
    __atomic_store_n(&st->host_ctx, ctx, __ATOMIC_RELEASE);
    __atomic_store_n(&st->host_probe, fn, __ATOMIC_RELEASE);
    __atomic_store_n(&st->magic, V71R7_ANCHOR_MAGIC, __ATOMIC_RELEASE);
}

void v71r7_neon_sentinel_reset_for_test(void)
{
    V71r7DramAnchor *anchor = v71r7_anchor();
    V71r7NeonSentinelState *st = sentinel_state();
    __atomic_fetch_and(&anchor->flags, ~V71R7_ANCHOR_FLAG_NEON_DISABLED, __ATOMIC_ACQ_REL);
    __atomic_store_n(&st->magic, V71R7_ANCHOR_MAGIC, __ATOMIC_RELAXED);
    __atomic_store_n(&st->last_status, V71R7_NEON_UNAVAILABLE, __ATOMIC_RELAXED);
    __atomic_store_n(&st->probe_runs, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&st->fail_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&st->probed_ok, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&st->host_probe, (V71r7NeonHostProbeFn)0, __ATOMIC_RELAXED);
    __atomic_store_n(&st->host_ctx, (void *)0, __ATOMIC_RELEASE);
}
#endif
