/* Shared Fenestra extension registry for the pinned fix-20261104 core.
 * The core root keeps one spare pointer slot (events[46], offset 2528). Every
 * extension layer that needs a root pointer reaches it through this registry
 * instead of claiming the slot itself, and inserts its Fenestra menu row
 * under the registry's menu lock. Identical copy in live-a-v4/ui and
 * live-playback-ui; keep them in sync. */
#ifndef FX_REGISTRY_H
#define FX_REGISTRY_H
#include <stdint.h>
#include <stddef.h>
#include "fenestra_native_menu.h"

#define FX_REGISTRY_MAGIC 0x47525846u /* 'FXRG' */
#define FX_REGISTRY_ROOT_OFFSET 2528u
#define FX_CORE_BACK_TEXT (3154u + 4u)

typedef struct {
    uint32_t magic, bytes;
    uintptr_t live_ui;   /* plan A Live Photo UiState */
    uintptr_t playback;  /* Live playback PbRuntime */
    uint32_t menu_lock, reserved;
    uintptr_t spare[4];
} FxRegistry;

static inline int fx_registry_ram(uintptr_t p, size_t n)
{
    return !(p & 7u) && p >= UINT64_C(0xa00000000) && p <= UINT64_C(0xa40000000) - n;
}

/* slot: &core_root->events[46]. create: allocate on first use (native heap). */
static inline FxRegistry *fx_registry(uintptr_t *slot, int create)
{
    if (!slot) { return 0; }
    uintptr_t v = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (!v && create) {
        FxRegistry *r = ((void *(*)(uint32_t, uint32_t))(uintptr_t)UINT64_C(0x40d2c408))((uint32_t)sizeof(FxRegistry), 0u);
        if (!r) { return 0; }
        uint8_t *b = (uint8_t *)r;
        for (size_t i = 0; i < sizeof(*r); i++) { b[i] = 0; }
        r->magic = FX_REGISTRY_MAGIC; r->bytes = (uint32_t)sizeof(*r);
        uintptr_t z = 0;
        if (__atomic_compare_exchange_n(slot, &z, (uintptr_t)r, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { return r; }
        ((void (*)(void *))(uintptr_t)UINT64_C(0x40d2c448))(r);
        v = z;
    }
    if (!fx_registry_ram(v, sizeof(FxRegistry))) { return 0; }
    FxRegistry *r = (FxRegistry *)v;
    return r->magic == FX_REGISTRY_MAGIC && r->bytes == sizeof(*r) ? r : 0;
}

/* Claim a registry field once; returns 1 if this caller owns it. */
static inline int fx_registry_claim(uintptr_t *field, uintptr_t value)
{
    uintptr_t z = 0;
    return __atomic_compare_exchange_n(field, &z, value, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline int fx_menu_trylock(FxRegistry *r)
{
    uint32_t z = 0;
    return __atomic_compare_exchange_n(&r->menu_lock, &z, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static inline void fx_menu_unlock(FxRegistry *r) { __atomic_store_n(&r->menu_lock, 0u, __ATOMIC_RELEASE); }

/* Fenestra core page: 7 native rows, the last is Back (text FX_CORE_BACK_TEXT).
 * Each extension inserts one row just before Back. Returns the row index the
 * caller fills; the caller links core->handlers[index + 1] and then calls
 * fx_core_menu_commit(). Fails (-1) when the page is not in a known state. */
static inline int fx_core_menu_slot(FxNativeMenu *core, uint32_t first_text)
{
    uint32_t n = core ? core->descriptor.count : 0u;
    if (!core || n < 7u || n + 1u >= FX_NATIVE_ROWS || core->rows[0].word[0] != first_text ||
        core->rows[n - 1u].word[0] != FX_CORE_BACK_TEXT ||
        core->descriptor.rows != (uintptr_t)core->rows || core->descriptor.events != (uintptr_t)core->events) {
        return -1;
    }
    return (int)n - 1;
}
static inline void fx_core_menu_commit(FxNativeMenu *core, unsigned index, uint32_t text, uint32_t row_id)
{
    unsigned n = index + 1u;
    core->rows[n] = core->rows[index]; core->events[n] = core->events[index];
    core->rows[index] = core->rows[0]; core->rows[index].word[0] = text;
    core->rows[index].word[2] = 65535u; core->rows[index].word[6] = row_id;
    core->rows[index].word[4] = (core->rows[index].word[4] & ~0x41u) | 0x40u;
    core->events[index] = (uintptr_t)core->handlers[n];
    __atomic_thread_fence(__ATOMIC_RELEASE);
    core->descriptor.count = n + 1u;
}
#endif
