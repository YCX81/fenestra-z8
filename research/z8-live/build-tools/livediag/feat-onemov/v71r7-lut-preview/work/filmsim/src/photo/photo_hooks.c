/* SPDX-License-Identifier: MIT
 * Photo Hook Callbacks for Z8 FilmSim WLM (V66 baseline, 14 stock callsites).
 * Freestanding C11, scalar-only (-mgeneral-regs-only), zero .data/.bss.
 */
#include "z8_native_filmsim_photo.h"
#include "../runtime/filmsim_runtime.h"
#include <string.h>

#ifndef FILMSIM_PHOTO_TIMEOUT_US
#define FILMSIM_PHOTO_TIMEOUT_US UINT64_C(30000000)
#endif

#define FN(a, t) ((t)(uintptr_t)UINT64_C(a))

extern uint64_t z8_np_original_resize(uint64_t);
extern uint64_t z8_np_original_thumbnail(uint64_t);
extern uint64_t z8_np_original_cleanup(uint64_t);

uint64_t z8_np_hook_resize(uint64_t ctx)
{
    struct z8_native_filmsim_photo *s0 = filmsim_runtime_photo_slot(0u);
    struct z8_native_filmsim_photo *s1 = filmsim_runtime_photo_slot(1u);
    Z8FilmSimConfig cfg;
    uint32_t seq = 0u;
    int active = filmsim_runtime_photo_config(&cfg, &seq);
    if (active && (s0 || s1)) {
        struct z8_native_filmsim_photo *chosen = 0;
        int begun = 0;
        int tried_s0 = 0;
        if (s0 && __atomic_load_n(&s0->phase, __ATOMIC_ACQUIRE) == Z8_NFSP_IDLE) {
            tried_s0 = 1;
            begun = z8_nfsp_begin(s0, ctx, &cfg, seq);
            if (begun) {
                chosen = s0;
            }
        }
        if (!begun && s1 && __atomic_load_n(&s1->phase, __ATOMIC_ACQUIRE) == Z8_NFSP_IDLE) {
            begun = z8_nfsp_begin(s1, ctx, &cfg, seq);
            if (begun) {
                chosen = s1;
            }
        }
        if (begun && chosen) {
            Z8FilmSimConfig check;
            if (!filmsim_runtime_photo_config(&check, 0)) {
                z8_nfsp_cancel(chosen);
            }
        } else if (!tried_s0 && s0) {
            __atomic_fetch_add(&s0->photos_bypassed, 1u, __ATOMIC_RELAXED);
        }
    } else if (s0) {
        __atomic_fetch_add(&s0->photos_bypassed, 1u, __ATOMIC_RELAXED);
    }
    return z8_np_original_resize(ctx);
}

uint64_t z8_np_hook_thumbnail(uint64_t ctx)
{
    struct z8_native_filmsim_photo *s0 = filmsim_runtime_photo_slot(0u);
    struct z8_native_filmsim_photo *s1 = filmsim_runtime_photo_slot(1u);
    struct z8_native_filmsim_photo *p = s0;
    uint64_t g = z8_nfsp_context_generation(s0, ctx);
    if (!g && s1) {
        g = z8_nfsp_context_generation(s1, ctx);
        if (g) {
            p = s1;
        }
    }
    uint64_t result = z8_np_original_thumbnail(ctx);
    if (!p || !g || result) {
        return result;
    }
    uint64_t start_us = filmsim_runtime_time_us();
    if (z8_nfsp_prepare_generation(p, ctx, g, start_us, FILMSIM_PHOTO_TIMEOUT_US)) {
        while (!z8_nfsp_poll_generation(p, g)) {
            if (filmsim_runtime_time_us() - start_us >= FILMSIM_PHOTO_TIMEOUT_US) {
                z8_nfsp_timeout_generation(p, g);
            }
            FN(0x40187b00, int (*)(uint32_t))(1);
        }
    }
    return result;
}

uint64_t z8_np_hook_cleanup(uint64_t outer)
{
    struct z8_native_filmsim_photo *s0 = filmsim_runtime_photo_slot(0u);
    struct z8_native_filmsim_photo *s1 = filmsim_runtime_photo_slot(1u);
    struct z8_native_filmsim_photo *p = 0;
    uint64_t g = 0u;
    if (s0 && s0->outer == outer) {
        g = z8_nfsp_cleanup_begin(s0, outer);
        if (g) {
            p = s0;
        }
    } else if (s1 && s1->outer == outer) {
        g = z8_nfsp_cleanup_begin(s1, outer);
        if (g) {
            p = s1;
        }
    }
    if (!g) {
        g = z8_nfsp_cleanup_begin(s0, outer);
        if (g) {
            p = s0;
        } else {
            g = z8_nfsp_cleanup_begin(s1, outer);
            if (g) {
                p = s1;
            }
        }
    }
    while (p && !z8_nfsp_cleanup_poll(p, g)) {
        FN(0x40187b00, int (*)(uint32_t))(1);
    }
    uint64_t result = z8_np_original_cleanup(outer);
    if (p) {
        z8_nfsp_cleanup_end(p, g);
    }
    return result;
}

uint64_t z8_np_hook_wait64(uint64_t event_obj, uint32_t event, uint64_t output,
                           uint32_t timeout, uint64_t outer)
{
    uint64_t result = FN(0x40d2bd64, uint64_t (*)(uint64_t, uint32_t, uint64_t, uint32_t))(
        event_obj, event, output, timeout);
    z8_nfsp_wait_return(filmsim_runtime_photo_slot(0u), outer, event, result);
    z8_nfsp_wait_return(filmsim_runtime_photo_slot(1u), outer, event, result);
    return result;
}

uint64_t z8_np_hook_wait54(uint64_t event_obj, uint32_t event, uint64_t output,
                           uint32_t timeout, uint64_t outer)
{
    uint64_t result = FN(0x40d2be54, uint64_t (*)(uint64_t, uint32_t, uint64_t, uint32_t))(
        event_obj, event, output, timeout);
    z8_nfsp_wait_return(filmsim_runtime_photo_slot(0u), outer, event, result);
    z8_nfsp_wait_return(filmsim_runtime_photo_slot(1u), outer, event, result);
    return result;
}

uint64_t z8_np_hook_thumbnail_resize(uint64_t params, uint64_t outer)
{
    uint64_t result = FN(0x40e85c54, uint64_t (*)(uint64_t))(params);
    z8_nfsp_thumbnail_return(filmsim_runtime_photo_slot(0u), outer, result);
    z8_nfsp_thumbnail_return(filmsim_runtime_photo_slot(1u), outer, result);
    return result;
}

uint64_t z8_np_hook_engine_ex(uint64_t engine, void *params, uint64_t outer,
                              uint32_t engine_slot)
{
    struct z8_native_filmsim_photo *s0 = filmsim_runtime_photo_slot(0u);
    struct z8_native_filmsim_photo *s1 = filmsim_runtime_photo_slot(1u);
    struct z8_native_filmsim_photo *owner = 0;
    _Alignas(16) uint8_t local[0x50], descriptor[0x28];
    int swapped = z8_nfsp_engine_begin_ex(s0, outer, params, local, descriptor, engine_slot);
    if (swapped) {
        owner = s0;
    } else if (s1) {
        swapped = z8_nfsp_engine_begin_ex(s1, outer, params, local, descriptor, engine_slot);
        if (swapped) {
            owner = s1;
        }
    }
    uint64_t result = FN(0x40e82090, uint64_t (*)(uint64_t, void *))(
        engine, swapped ? local : params);
    if (swapped && owner) {
        memcpy((uint8_t *)params + 0x20u, local + 0x20u, 0x28u);
        z8_nfsp_engine_end(owner, result);
    }
    return result;
}

uint64_t z8_np_hook_engine(uint64_t engine, void *params, uint64_t outer)
{
    return z8_np_hook_engine_ex(engine, params, outer, Z8_NFSP_SLOT_DEFAULT);
}
