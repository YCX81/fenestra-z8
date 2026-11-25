/* SPDX-License-Identifier: GPL-3.0-only */
/* Unified Nikon Z8 C3.11 V71.7 LUT Preview + Layer 0 WYSIWYG + Hardening Shared ABI.
 *
 * Adapted from Fenestra-Z8-C311-source (tag wlm-2026.11.23-lut-preview) for static
 * unified EL1 integration into V71.7 (Bank 1: 0x409DB000..0x409EC000, Bank 2:
 * 0x40981000..0x40990000, DRAM Anchor+Flight: 0xA11640000..0xA11644000).
 *
 * STRICT INVARIANT: Zero mutable .data/.bss globals in ROM (0x40xxxxxx is RX-only!).
 * All fixed anchors live in verified-free DRAM 0xA11640000..0xA11644000; large state
 * lives on kernel heap (HEAP_ALLOC 0x40D2C408) and Class 0x24 photo_workspace.
 */
#ifndef V71R7_LUT_ABI_H
#define V71R7_LUT_ABI_H

#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * 1. Verified Free DRAM Anchor & Black-Box Flight Recorder Layout
 *    Located in C64 free DRAM gap 0xA1162B000..0xA1164D000 (C64 PNG cache ends
 *    at 0xA1163F000; 4 KiB guard 0xA1163F000..0xA11640000; 36 KiB free above).
 * ============================================================================ */
#define V71R7_DRAM_ANCHOR_VA        UINT64_C(0xA11640000)
#define V71R7_DRAM_IO_GATE_VA       UINT64_C(0xA11640040)
#ifdef V71R7_HOST_TEST
#define V71R7_DRAM_HOST_MOD_VA      UINT64_C(0xA11640048) /* 4B host mock: 0=loaded, 1=disabled */
#endif
#define V71R7_DRAM_FLIGHT_RING_VA   UINT64_C(0xA11641000)
#define V71R7_DRAM_FLIGHT_HEAD_VA   UINT64_C(0xA11641800) /* 64 * 32 B = 0x800 */
#define V71R7_DRAM_HEARTBEATS_VA    UINT64_C(0xA11641808) /* 8 * 32 B = 0x100 */
#define V71R7_DRAM_WATCHDOG_VA      UINT64_C(0xA11641908) /* 32 B status */
#define V71R7_DRAM_ANCHOR_END_VA    UINT64_C(0xA11644000)

#define V71R7_ANCHOR_MAGIC          UINT32_C(0x56373137) /* "V717" */
#define V71R7_CARDSLOT_STATE_VA     UINT64_C(0xA1171B000)
#define V71R7_CARDSLOT_MAGIC        UINT32_C(0x314C5343) /* "CSL1" */

/* Host-testable pointer indirection (on EL1 AArch64 compiles to fixed DRAM VA) */
#ifdef V71R7_HOST_TEST
extern uint8_t v71r7_host_dram_region[0x4000];
#define V71R7_DRAM_PTR(type, va) \
    ((type *)(void *)(v71r7_host_dram_region + ((va) - V71R7_DRAM_ANCHOR_VA)))
#define V71R7_BANK2_COLD
#else
#define V71R7_DRAM_PTR(type, va) ((type *)(uintptr_t)(va))
/* Place cold setup/parser/crash-log helpers into Bank 2 (.v71r7_lut_menu) where >22 KiB
 * is free, keeping hot Bank 1 (.v71r7_lut_core + z8_xm6_bridge) well under 68 KiB. */
#define V71R7_BANK2_COLD __attribute__((section(".v71r7_lut_menu.cold")))
#endif

/* ============================================================================
 * 2. LUT Selection, Path Validation, Progress & Flight-Recorder Enums
 * ============================================================================ */
#define FX_Z8_LUT_DEFAULT           "LOOK.CUBE"
enum {
    FX_Z8_LUT_NAME_BYTES = 32,
    FX_Z8_LUT_PATH_BYTES = 64
};

enum {
    Z8_LUT_OFF = 0,
    Z8_LUT_LOADING = 1,
    Z8_LUT_READY = 2,
    Z8_LUT_CAPTURING = 3,
    Z8_LUT_ERROR = 4
};

enum {
    FX_Z8_LUT_IDLE = 0,
    FX_Z8_LUT_QUEUED = 1,
    FX_Z8_LUT_READING = 2,
    FX_Z8_LUT_PREPARING = 3,
    FX_Z8_LUT_READY = 4,
    FX_Z8_LUT_ERROR = 5
};

enum {
    REAR_CLEAR = 4,
    REAR_READY = 5,
    REAR_RETIRE = 6,
    REAR_PRESENTED = 7,
    REAR_WORKER_STEP = 2,
    REAR_SWITCH = 8,
    REAR_FLIGHT_METRICS = 9,
    REAR_STREAMING = 0x100
};

#define FX_Z8_LUT_PROGRESS_VERSION  1u

typedef struct {
    uint32_t enabled;
    char name[FX_Z8_LUT_NAME_BYTES];
} FxZ8LutSelection;

typedef struct {
    uint32_t version, generation, phase, result;
    uint32_t file_bytes, work_done, work_total, cache_hit;
} FxZ8LutProgress;

typedef struct {
    uint32_t contention_passes, fallbacks, wait_ms, queue_full, deadline_ms, active_exposures;
} FxZ8JpegFlightMetrics;

typedef struct {
    uint32_t active_route, grade_us, worker_cores, neon_active, capture_aborts, timeouts;
} FxZ8RearFlightMetrics;

/* Black-box flight recorder event IDs (upstream 1..15 + V71.7 extensions 16..24) */
enum {
    FX_Z8_FLIGHT_JPEG_MAIN_ENTER = 1,
    FX_Z8_FLIGHT_JPEG_QUEUE_WAIT = 2,
    FX_Z8_FLIGHT_JPEG_GRADE_START = 3,
    FX_Z8_FLIGHT_JPEG_GRADE_DONE = 4,
    FX_Z8_FLIGHT_JPEG_ENCODE_START = 5,
    FX_Z8_FLIGHT_JPEG_ENCODE_DONE = 6,
    FX_Z8_FLIGHT_JPEG_FALLBACK = 7,
    FX_Z8_FLIGHT_JPEG_YIELD = 8,
    FX_Z8_FLIGHT_PREVIEW_ROUTE_SWITCH = 9,
    FX_Z8_FLIGHT_PREVIEW_CAPTURE_ABORT = 10,
    FX_Z8_FLIGHT_PREVIEW_POWER_OFF = 11,
    FX_Z8_FLIGHT_PACK_FAULT = 12,
    FX_Z8_FLIGHT_PACK_STOP = 13,
    FX_Z8_FLIGHT_STALL = 14,
    FX_Z8_FLIGHT_SMP_QUARANTINE = 15,
    /* V71.7 extensions */
    FX_Z8_FLIGHT_POOL_SUSPEND = 16,
    FX_Z8_FLIGHT_POOL_RESUME = 17,
    FX_Z8_FLIGHT_XM6_SUBMIT = 18,
    FX_Z8_FLIGHT_XM6_TIMEOUT = 19,
    FX_Z8_FLIGHT_XM6_SPACER = 20,
    FX_Z8_FLIGHT_NEON_SENTINEL_FAIL = 21,
    FX_Z8_FLIGHT_POWER_CANCEL = 22,
    FX_Z8_FLIGHT_L0_WYSIWYG = 23,
    FX_Z8_FLIGHT_BOOT = 24
};

enum {
    FX_Z8_FLIGHT_BURST_FALLBACK = 1u << 4,
    FX_Z8_FLIGHT_DEADLINE       = 1u << 5,
    FX_Z8_FLIGHT_CONTENTION     = 1u << 6
};

enum {
    FX_Z8_HB_JPEG_LANE0 = 0,
    FX_Z8_HB_JPEG_LANE1 = 1,
    FX_Z8_HB_JPEG_HELPER1 = 2,
    FX_Z8_HB_JPEG_HELPER2 = 3,
    FX_Z8_HB_REAR_STEP = 4,
    FX_Z8_HB_REAR_HELPER1 = 5,
    FX_Z8_HB_REAR_HELPER2 = 6,
    FX_Z8_HB_WORKER = 7,
    FX_Z8_HB_COUNT = 8
};

enum {
    FX_Z8_STAGE_IDLE = 0,
    FX_Z8_STAGE_JPEG_QUEUE_WAIT = 1,
    FX_Z8_STAGE_JPEG_SMP_DISPATCH = 2,
    FX_Z8_STAGE_JPEG_GRADE = 3,
    FX_Z8_STAGE_JPEG_SMP_JOIN = 4,
    FX_Z8_STAGE_JPEG_CACHE_MAINT = 5,
    FX_Z8_STAGE_JPEG_ENCODE_STAGED = 6,
    FX_Z8_STAGE_JPEG_ENCODE_STOCK = 7,
    FX_Z8_STAGE_JPEG_XM6 = 8,
    FX_Z8_STAGE_REAR_COPY = 9,
    FX_Z8_STAGE_REAR_GRADE = 10,
    FX_Z8_STAGE_REAR_JOIN = 11,
    FX_Z8_STAGE_WORKER_CARD_IO = 12,
    FX_Z8_STAGE_WORKER_STEP = 13,
    FX_Z8_STAGE_REAR_HDF_GRAIN = 14,
    FX_Z8_STAGE_POOL_SUSPEND = 15
};

enum {
    FX_Z8_HB_CARD_DRIVE = 1, FX_Z8_HB_CARD_OPEN, FX_Z8_HB_CARD_READ, FX_Z8_HB_CARD_WRITE,
    FX_Z8_HB_CARD_CLOSE, FX_Z8_HB_CARD_REMOVE, FX_Z8_HB_CARD_RENAME, FX_Z8_HB_DIR_OPEN,
    FX_Z8_HB_DIR_NEXT, FX_Z8_HB_DIR_CLOSE, FX_Z8_HB_CARD_CLEAN, FX_Z8_HB_CRASH_WRITE
};

typedef struct {
    uint64_t tick;
    uint16_t event;
    uint16_t core_flags;
    uint32_t seq;
    uint32_t a, b, c;
    uint32_t reserved;
} FxZ8FlightRecord;

typedef struct {
    uint32_t stage, cap_num, core, aux;
    uint64_t enter_tick, progress_tick;
} FxZ8Heartbeat;

typedef struct {
    uint32_t magic;
    uint32_t stalled_slots;
    uint32_t dumps_written;
    uint32_t last_err;
    uint64_t last_dump_tick;
    uint64_t reserved;
} FxZ8WatchdogStatus;

#define FX_Z8_FLIGHT_RING_COUNT     64u
#define FX_Z8_HEARTBEAT_BUSY        (UINT64_C(1) << 63)
#define FX_Z8_TICKS_PER_MS          24000u

/* ============================================================================
 * 3. Atomic I/O, Frame-Lease, Power-Sync & Helper-Ownership Gate (64-bit)
 * ============================================================================ */
typedef struct {
    const uint32_t *permit;
    uint32_t ticket;
} FxZ8LutOutput;

typedef struct {
    uint64_t state;
} FxZ8LutIoGate;

#define FX_Z8_LUT_IO_STOPPED        UINT64_C(0x80000000)
#define FX_Z8_LUT_IO_EARLY_OFF      UINT64_C(0x04000000)
#define FX_Z8_LUT_IO_REFS           UINT64_C(0x7bfffffe)
#define FX_Z8_LUT_HELPER_JPEG       UINT64_C(0x01000000)
#define FX_Z8_LUT_HELPER_REAR       UINT64_C(0x02000000)
#define FX_Z8_LUT_HELPERS           (FX_Z8_LUT_HELPER_JPEG | FX_Z8_LUT_HELPER_REAR)
#define FX_Z8_LUT_IO_FRAME          UINT64_C(0x78000000)
#define FX_Z8_LUT_IO_FRAME_ONE      UINT64_C(0x08000000)
#define FX_Z8_LUT_IO_CARD           (FX_Z8_LUT_IO_REFS & ~(FX_Z8_LUT_IO_FRAME | FX_Z8_LUT_HELPERS))

static inline FxZ8LutIoGate *v71r7_io_gate(void) {
    return V71R7_DRAM_PTR(FxZ8LutIoGate, V71R7_DRAM_IO_GATE_VA);
}

static inline uint32_t fx_z8_lut_io_epoch(const FxZ8LutIoGate *gate) {
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    return (state & 1u) ? (uint32_t)(state >> 32) : 0u;
}

static inline int fx_z8_lut_io_take(FxZ8LutIoGate *gate, uint32_t epoch) {
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    do {
        if (!epoch || (state >> 32) != epoch || !(state & 1u) ||
            (state & FX_Z8_LUT_IO_CARD) == FX_Z8_LUT_IO_CARD)
            return 0;
    } while (!__atomic_compare_exchange_n(&gate->state, &state, state + 2u,
                                           1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return 1;
}

static inline void fx_z8_lut_io_drop(FxZ8LutIoGate *gate) {
    __atomic_fetch_sub(&gate->state, 2u, __ATOMIC_RELEASE);
}

static inline int fx_z8_lut_frame_take(FxZ8LutIoGate *gate, uint32_t epoch) {
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    do {
        if (!epoch || (state >> 32) != epoch || !(state & 1u) ||
            (state & FX_Z8_LUT_IO_FRAME) == FX_Z8_LUT_IO_FRAME)
            return 0;
    } while (!__atomic_compare_exchange_n(&gate->state, &state, state + FX_Z8_LUT_IO_FRAME_ONE,
                                           1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return 1;
}

static inline void fx_z8_lut_frame_drop(FxZ8LutIoGate *gate) {
    __atomic_fetch_sub(&gate->state, FX_Z8_LUT_IO_FRAME_ONE, __ATOMIC_RELEASE);
}

static inline uint32_t fx_z8_lut_frame_count(const FxZ8LutIoGate *gate) {
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    return (uint32_t)((state & FX_Z8_LUT_IO_FRAME) / FX_Z8_LUT_IO_FRAME_ONE);
}

static inline int fx_z8_lut_helpers_take(FxZ8LutIoGate *gate, uint32_t epoch, uint64_t owner) {
    uint64_t busy = FX_Z8_LUT_HELPERS | (owner == FX_Z8_LUT_HELPER_REAR ? FX_Z8_LUT_IO_FRAME : 0u);
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    do {
        if (!epoch || (state >> 32) != epoch || !(state & 1u) || (state & busy))
            return 0;
    } while (!__atomic_compare_exchange_n(&gate->state, &state, state | owner,
                                           1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return 1;
}

static inline void fx_z8_lut_helpers_drop(FxZ8LutIoGate *gate, uint64_t owner) {
    __atomic_fetch_and(&gate->state, ~owner, __ATOMIC_RELEASE);
}

static inline void fx_z8_lut_io_close(FxZ8LutIoGate *gate, int lifecycle) {
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    uint64_t stop = lifecycle ? FX_Z8_LUT_IO_STOPPED : FX_Z8_LUT_IO_EARLY_OFF;
    while (!__atomic_compare_exchange_n(&gate->state, &state, (state & ~UINT64_C(1)) | stop,
                                        1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { }
}

static inline void fx_z8_lut_io_reopen(FxZ8LutIoGate *gate, int cancel) {
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    uint64_t next;
    do {
        const uint64_t epoch = state >> 32;
        if (epoch == UINT32_MAX || (cancel && (!epoch || (state & (FX_Z8_LUT_IO_STOPPED | 1u)))))
            return;
        next = ((epoch + 1u) << 32) | (state & FX_Z8_LUT_IO_REFS) | 1u;
    } while (!__atomic_compare_exchange_n(&gate->state, &state, next,
                                           1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

static inline int fx_z8_lut_output_valid(const FxZ8LutOutput *out) {
    return out && out->permit && !((uintptr_t)out->permit & 3u) &&
           (out->ticket & 1u) && out->ticket > 1u;
}

static inline int fx_z8_lut_output_closed(const FxZ8LutOutput *out) {
    return __atomic_load_n(out->permit, __ATOMIC_ACQUIRE) == (out->ticket & ~1u);
}

static inline int fx_z8_lut_output_open(const FxZ8LutOutput *out) {
    return __atomic_load_n(out->permit, __ATOMIC_ACQUIRE) == out->ticket;
}

static inline int fx_z8_lut_component_char(unsigned c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static inline unsigned fx_z8_lut_category_length(const char *name) {
    unsigned n = 0;
    while (n < 26u && fx_z8_lut_component_char((unsigned char)name[n])) ++n;
    return n;
}

static inline int fx_z8_lut_category(const char *name) {
    unsigned n = fx_z8_lut_category_length(name);
    return n && !name[n];
}

static inline int fx_z8_lut_name(const char *name) {
    unsigned n = fx_z8_lut_category_length(name);
    if (!n) return 0;
    return name[n] == '.' && (name[n+1] | 32) == 'c' && (name[n+2] | 32) == 'u' &&
           (name[n+3] | 32) == 'b' && (name[n+4] | 32) == 'e' && !name[n+5];
}

static inline int fx_z8_lut_path(const char *path) {
    if (!path || !path[0]) return 0;
    if ((path[0] | 32) != 'l' || (path[1] | 32) != 'u' ||
        (path[2] | 32) != 't' || (path[3] | 32) != 's' || path[4] != '\\')
        return fx_z8_lut_name(path);
    const char *category = path + 5;
    unsigned n = fx_z8_lut_category_length(category);
    return n && category[n] == '\\' && fx_z8_lut_name(category + n + 1u);
}

/* ============================================================================
 * 4. DRAM Root Anchor (56 bytes at 0xA11640000, strictly <= 64 B before IO_GATE)
 *    Connects Bank 1 (lut_core/layer0), Bank 2 (lut_menu), and FilmSim cleanly
 *    without a single mutable .data/.bss variable in RX ROM.
 *    Offsets +0..+39 are FROZEN for USB probes, QEMU, and Unicorn tests.
 * ============================================================================ */
typedef struct {
    uint32_t magic;               /* +0:  V71R7_ANCHOR_MAGIC */
    uint32_t flags;               /* +4:  bit0: neon_disabled, bit1: pool_suspended, bit2: lut_active, bit3: wysiwyg_active */
    void    *lut_core_ctx;        /* +8:  Heap-allocated compiled LUT + SMP + active 3D/1D table */
    void    *layer0_ctx;          /* +16: Heap-allocated Layer 0 provider + photo_workspace state */
    void    *menu_ctx;            /* +24: Heap-allocated Photo Menu + catalog + journal + worker state */
    uint32_t output_permit;       /* +32: Atomic permit counter for FxZ8LutOutput */
    uint32_t active_generation;   /* +36: Incremented whenever active LUT changes */
    void    *xm6_bridge;          /* +40: Bound XM6 bridge (avoids lut_ctx_ensure() while dormant!) */
    uint8_t  lut_master_on;       /* +48: 0=Off, 1=On (used when lut_cfg_set != 0; default 1) */
    uint8_t  lut_stage_mask;      /* +49: BIT0=PREVIEW, BIT1=STILL, BIT2=LIVE (default 0x7) */
    uint8_t  xm6_mode;            /* +50: 0=AUTO, 1=ARM_ONLY */
    uint8_t  lut_cfg_set;         /* +51: 0=default ON/ALL, 1=explicitly configured by lut.wlm */
    uint32_t lut_enable_epoch;    /* +52: Bumped on master_on 0->1 for journal auto-restore */
} V71r7DramAnchor;

_Static_assert(sizeof(V71r7DramAnchor) == 56u, "V71r7DramAnchor must stay <= 64 B");
_Static_assert(offsetof(V71r7DramAnchor, magic) == 0u &&
               offsetof(V71r7DramAnchor, flags) == 4u &&
               offsetof(V71r7DramAnchor, lut_core_ctx) == 8u &&
               offsetof(V71r7DramAnchor, layer0_ctx) == 16u &&
               offsetof(V71r7DramAnchor, menu_ctx) == 24u &&
               offsetof(V71r7DramAnchor, output_permit) == 32u &&
               offsetof(V71r7DramAnchor, active_generation) == 36u &&
               offsetof(V71r7DramAnchor, xm6_bridge) == 40u &&
               offsetof(V71r7DramAnchor, lut_master_on) == 48u &&
               offsetof(V71r7DramAnchor, lut_stage_mask) == 49u &&
               offsetof(V71r7DramAnchor, xm6_mode) == 50u &&
               offsetof(V71r7DramAnchor, lut_cfg_set) == 51u &&
               offsetof(V71r7DramAnchor, lut_enable_epoch) == 52u,
               "V71r7DramAnchor frozen layout");

#define V71R7_ANCHOR_FLAG_NEON_DISABLED   (1u << 0)
#define V71R7_ANCHOR_FLAG_POOL_SUSPENDED  (1u << 1)
#define V71R7_ANCHOR_FLAG_LUT_ACTIVE      (1u << 2)
#define V71R7_ANCHOR_FLAG_WYSIWYG_ACTIVE  (1u << 3)

/* LUT per-stage enable bitmask (controlled by lut.wlm via LUT_OP_SET_CFG) */
enum V71r7LutStageMask {
    V71R7_LUT_STAGE_PREVIEW = (1u << 0), /* BIT0: Layer 0 Rear/EVF preview */
    V71R7_LUT_STAGE_STILL   = (1u << 1), /* BIT1: Stage 0 Still JPEG NV16 */
    V71R7_LUT_STAGE_LIVE    = (1u << 2), /* BIT2: Stage 0 Live Photo NV12 */
    V71R7_LUT_STAGE_ALL     = 0x7u
};

/* Unified Layer 0 / Live->LCD WYSIWYG stage bitmask (Stage 0 LUT -> Stage 1 HDF -> Stage 2 Grain) */
#ifndef V71R7_L0_STAGE_BITS_DEFINED
#define V71R7_L0_STAGE_BITS_DEFINED 1
enum V71r7WysiwygStageBits {
    V71R7_L0_STAGE_LUT   = (1U << 0),
    V71R7_L0_STAGE_HDF   = (1U << 1),
    V71R7_L0_STAGE_GRAIN = (1U << 2)
};
#endif

static inline V71r7DramAnchor *v71r7_anchor(void) {
    return V71R7_DRAM_PTR(V71r7DramAnchor, V71R7_DRAM_ANCHOR_VA);
}

/* Single source of truth for lut.wlm module gating via Fenestra fx_native_module_loaded
 * (0x401ccde4, static 16B NUL-padded ID "lut"). Returns 0 when .wlm.disabled, unloaded,
 * or under FX_STARTUP_ABNORMAL (phase != 2). */
static inline int v71r7_lut_is_module_loaded(void) {
#ifdef V71R7_HOST_TEST
    return __atomic_load_n(V71R7_DRAM_PTR(uint32_t, V71R7_DRAM_HOST_MOD_VA),
                           __ATOMIC_ACQUIRE) == 0u;
#else
    static const char k_lut_mod_id[16] = "lut";
    return ((uint32_t (*)(const char *))(uintptr_t)UINT64_C(0x401ccde4))(k_lut_mod_id) != 0u;
#endif
}

#ifdef V71R7_HOST_TEST
static inline void v71r7_host_set_lut_module_loaded(int loaded) {
    __atomic_store_n(V71R7_DRAM_PTR(uint32_t, V71R7_DRAM_HOST_MOD_VA),
                     loaded ? 0u : 1u, __ATOMIC_RELEASE);
}
#endif

static inline uint8_t v71r7_lut_effective_master_on(void) {
    const V71r7DramAnchor *a = v71r7_anchor();
    if (__atomic_load_n(&a->magic, __ATOMIC_ACQUIRE) != V71R7_ANCHOR_MAGIC) {
        return 1u;
    }
    if (!__atomic_load_n(&a->lut_cfg_set, __ATOMIC_ACQUIRE)) {
        return 1u;
    }
    return __atomic_load_n(&a->lut_master_on, __ATOMIC_ACQUIRE) ? 1u : 0u;
}

static inline uint8_t v71r7_lut_effective_stage_mask(void) {
    const V71r7DramAnchor *a = v71r7_anchor();
    if (__atomic_load_n(&a->magic, __ATOMIC_ACQUIRE) != V71R7_ANCHOR_MAGIC) {
        return V71R7_LUT_STAGE_ALL;
    }
    if (!__atomic_load_n(&a->lut_cfg_set, __ATOMIC_ACQUIRE)) {
        return V71R7_LUT_STAGE_ALL;
    }
    return (uint8_t)(__atomic_load_n(&a->lut_stage_mask, __ATOMIC_ACQUIRE) & V71R7_LUT_STAGE_ALL);
}

static inline int v71r7_lut_is_enabled_for(uint32_t stage_bit) {
    return v71r7_lut_is_module_loaded() &&
           v71r7_lut_effective_master_on() != 0u &&
           ((v71r7_lut_effective_stage_mask() & stage_bit) != 0u);
}

/* ============================================================================
 * 4b. lut.wlm slots.state EL1 IPC ABI (ops 64..79, magic 'LTI1')
 *     Routed by filmsim_slots_svc_dispatch() -> v71r7_lut_ipc_service_call()
 *     in Bank 2 (photo_worker.c).
 * ============================================================================ */
#define V71R7_LUT_SLOTS_OP_BASE      64u
#define V71R7_LUT_SLOTS_OP_COUNT     16u
#define V71R7_LUT_IPC_MAGIC          UINT32_C(0x3149544C) /* "LTI1" */
#define V71R7_LUT_IPC_VERSION        1u
#define V71R7_LUT_IPC_HDR_BYTES      16u

enum V71r7LutIpcOp {
    V71R7_LUT_OP_GET_STATE     = 65u, /* subop 1: req 0B -> resp 168B V71r7LutIpcState */
    V71R7_LUT_OP_SET_CFG       = 66u, /* subop 2: req 16B V71r7LutIpcSetCfg -> resp 0B */
    V71R7_LUT_OP_CLEAR_LUT     = 67u, /* subop 3: req 0B -> resp 0B */
    V71R7_LUT_OP_RESET_PREVIEW = 68u  /* subop 4: req 0B -> resp 0B (-5/FX_BUSY if quarantined) */
};

typedef struct {
    uint32_t magic;       /* +0:  V71R7_LUT_IPC_MAGIC ('LTI1') */
    uint16_t version;     /* +4:  V71R7_LUT_IPC_VERSION (1) */
    uint16_t bytes;       /* +6:  sizeof(V71r7LutIpcSetCfg) == 16 */
    uint8_t  master_on;   /* +8:  0=Off, 1=On */
    uint8_t  stage_mask;  /* +9:  BIT0=PREVIEW, BIT1=STILL, BIT2=LIVE */
    uint8_t  reserved[6]; /* +10: 0 */
} V71r7LutIpcSetCfg;

_Static_assert(sizeof(V71r7LutIpcSetCfg) == 16u &&
               offsetof(V71r7LutIpcSetCfg, master_on) == 8u &&
               offsetof(V71r7LutIpcSetCfg, stage_mask) == 9u,
               "V71r7LutIpcSetCfg ABI == 16B");

typedef struct {
    uint32_t magic;              /* +0:   V71R7_LUT_IPC_MAGIC ('LTI1') */
    uint16_t version;            /* +4:   V71R7_LUT_IPC_VERSION (1) */
    uint16_t bytes;              /* +6:   sizeof(V71r7LutIpcState) == 168 */
    uint8_t  master_on;          /* +8:   0/1 */
    uint8_t  preview_on;         /* +9:   0/1 */
    uint8_t  still_on;           /* +10:  0/1 */
    uint8_t  live_on;            /* +11:  0/1 */
    uint8_t  active_drive;       /* +12:  'A' or 'B' from CSL1 */
    uint8_t  is_1d_separable;    /* +13:  0/1 */
    uint8_t  neon_allowed;       /* +14:  0/1 */
    uint8_t  quarantined;        /* +15:  0/1 */
    uint8_t  lut_active;         /* +16:  0/1 */
    uint8_t  smp_helpers_online; /* +17:  0..2 actual online SMP helpers */
    uint8_t  reserved0[2];       /* +18:  0 */
    uint32_t worker_phase;       /* +20:  PhotoPhase (0..12; 12=PHOTO_MODULE_DISABLED) */
    int32_t  last_result;        /* +24:  FxResult / Layer0 last_result */
    int32_t  pool_result;        /* +28:  PhotoWorkspaceStatus */
    uint32_t generation;         /* +32:  active LUT generation */
    uint32_t ws_bytes;           /* +36:  Layer 0 photo_workspace surface_bytes */
    uint32_t fps_milli;          /* +40:  Layer 0 fps_milli */
    uint32_t grades;             /* +44:  Layer 0 grades count */
    uint32_t disp_sub;           /* +48:  Layer 0 disp_substituted */
    uint32_t disp_rejects;       /* +52:  Layer 0 disp_rejects */
    uint32_t xm6_calls;          /* +56:  LUT Core nv16_xm6_calls */
    uint32_t nv16_neon;          /* +60:  LUT Core nv16_neon_calls */
    uint32_t nv16_scalar;        /* +64:  LUT Core nv16_scalar_calls */
    uint32_t nv12_neon;          /* +68:  LUT Core nv12_neon_calls */
    uint32_t wd_polls;           /* +72:  photo_worker watchdog_polls */
    uint32_t live_reuse_hits;    /* +76:  Layer 0 Live->LCD reuse hits */
    uint32_t last_lut_us;        /* +80:  Layer 0 last_lut_us */
    uint32_t last_hdf_us;        /* +84:  Layer 0 last_hdf_us */
    uint32_t last_grain_us;      /* +88:  Layer 0 last_grain_us */
    uint32_t reserved1[3];       /* +92:  0 */
    char     active_path[FX_Z8_LUT_PATH_BYTES]; /* +104..+167: NUL-terminated relative path */
} V71r7LutIpcState;

_Static_assert(sizeof(V71r7LutIpcState) == 168u &&
               offsetof(V71r7LutIpcState, worker_phase) == 20u &&
               offsetof(V71r7LutIpcState, generation) == 32u &&
               offsetof(V71r7LutIpcState, xm6_calls) == 56u &&
               offsetof(V71r7LutIpcState, live_reuse_hits) == 76u &&
               offsetof(V71r7LutIpcState, active_path) == 104u,
               "V71r7LutIpcState ABI == 168B");

/* Active storage drive letter ('A' or 'B') from V71.6 cardslot_v71r4 (0xA1171B000).
 * cardslot_v71r4 stores drive enum: 1='A', 2='B', 0=pending (fallback 'A');
 * raw ASCII 'A'/'B' is also accepted for compatibility. */
static inline char v71r7_active_card_drive(void) {
#ifdef V71R7_HOST_TEST
    return 'A';
#else
    const volatile uint32_t *cs = (const volatile uint32_t *)(uintptr_t)V71R7_CARDSLOT_STATE_VA;
    if (cs[0] == V71R7_CARDSLOT_MAGIC) {
        uint32_t d = cs[1];
        if (d == 1u || (d & 0xFFu) == 'A') return 'A';
        if (d == 2u || (d & 0xFFu) == 'B') return 'B';
    }
    return 'A';
#endif
}

/* ============================================================================
 * 5. Cross-Module Function Contracts (Implemented across Bank 1 & Bank 2)
 * ============================================================================ */

/* --- Flight recorder & heartbeat watchdog (work/lut_core/photo_flight.c) --- */
void v71r7_flight_init(void);
void v71r7_flight_record(uint16_t event, uint16_t core_flags, uint32_t a, uint32_t b, uint32_t c);
void v71r7_heartbeat_enter(unsigned slot, unsigned stage, uint32_t cap_num, uint32_t aux);
void v71r7_heartbeat_progress(unsigned slot);
void v71r7_heartbeat_idle(unsigned slot);
/* Polled periodically by photo_worker (~100ms): detects >2s stall and writes FXCRASH.LOG */
uint32_t v71r7_watchdog_poll(uint64_t now_tick);

/* --- Runtime NEON sentinel (work/lut_core/neon_sentinel.h) --- */
int v71r7_neon_sentinel_is_allowed(void);
int v71r7_neon_sentinel_probe_once(void); /* Called once per worker startup */

/* --- Shared CPU2+CPU3 lock-free stripe pool (work/lut_core/smp.c) --- */
typedef void (*V71r7SmpStripeFn)(void *ctx, uint32_t row_begin, uint32_t row_end, unsigned worker_idx);
int      v71r7_smp_ensure_started(void);
void     v71r7_smp_run_stripes(uint64_t helper_owner, uint32_t total_rows, uint32_t stripe_rows,
                               V71r7SmpStripeFn fn, void *ctx, unsigned caller_hb_slot);
int      v71r7_smp_run_stripes_copy(uint64_t helper_owner, uint32_t total_rows, uint32_t stripe_rows,
                                    V71r7SmpStripeFn fn, const void *ctx_src, size_t ctx_bytes,
                                    unsigned caller_hb_slot);
void     v71r7_smp_stop(void);
uint32_t v71r7_smp_online_helpers(void);
void     v71r7_smp_get_helper_sentinels(uint32_t out[3]);

/* --- Active .cube LUT runtime API (work/lut_core/lut_runtime.c) --- */
/* Prepare/compile a .cube file from buffer (1D separable XM6 curves + 3D cube table) */
int v71r7_lut_prepare_from_buffer(const uint8_t *buf, uint32_t bytes, const char *path,
                                  uint32_t generation, FxZ8LutProgress *prog_out);
/* Streaming .cube parser (V71.7-r6: ~213 KiB peak heap vs 3 MiB doubling buffer, 0 OOM on 33^3 .cube) */
void *v71r7_lut_stream_begin(uint32_t generation, FxZ8LutProgress *prog_out);
int   v71r7_lut_stream_feed(void *stream_handle, const uint8_t *chunk, uint32_t bytes,
                            FxZ8LutProgress *prog_out);
int   v71r7_lut_stream_commit(void *stream_handle, const char *path, uint32_t generation,
                              FxZ8LutProgress *prog_out);
void  v71r7_lut_stream_abort(void *stream_handle);
void v71r7_lut_clear_active(void);
int v71r7_lut_is_active(uint32_t *gen_out, int *is_1d_separable_out);
int v71r7_lut_is_active_for(uint32_t stage_bit, uint32_t *gen_out, int *is_1d_separable_out);
/* Apply active LUT Stage 0 to Still NV16 plane (XM6 Op12 if 1D separable, else SMP+NEON 3D) */
int v71r7_lut_apply_still_nv16(uint8_t *y, uint8_t *uv, uint32_t width, uint32_t height,
                               uint32_t y_stride, uint32_t uv_stride,
                               uint32_t *cache_words_4mib, uint32_t *cache_gen_inout);
/* Apply active LUT Stage 0 to Live/Layer-0 NV12 frame using 4 MiB cache */
int v71r7_lut_apply_nv12(uint8_t *y, uint8_t *uv, uint32_t width, uint32_t height,
                         uint32_t y_stride, uint32_t uv_stride,
                         uint32_t *cache_words_4mib, uint32_t *cache_gen_inout,
                         int use_smp);
/* V71.7-r7 fast L2-resident 528 KiB pre-baked 64x64x33 YCbCr 3D LUT NV12 path
 * (lazily bakes 64 Cr slices across 2 SMP cores on generation change, 0 cache misses!). */
int v71r7_lut_apply_nv12_baked(uint8_t *y, uint8_t *uv, uint32_t width, uint32_t height,
                               uint32_t y_stride, uint32_t uv_stride,
                               uint32_t *baked_table_528k, uint32_t *baked_gen_inout,
                               int use_smp);

/* --- Layer 0 WYSIWYG preview & Power Sync (work/lut_preview/) --- */
int       v71r7_layer0_init(void);
void      v71r7_layer0_pool_suspend(void); /* MANDATORY HangFix V69 hook: drain VSync + free 14.55MiB */
void      v71r7_layer0_pool_resume(void);  /* Re-arm workspace lazily when returning to Photo Mode */
void      v71r7_layer0_power_off_sync(void);
void      v71r7_layer0_power_cancel_sync(void);
void      v71r7_layer0_notify_lut_changed(uint32_t generation, int active);
void      v71r7_layer0_cache_invalidate_before_swap(void);
uint32_t  v71r7_layer0_worker_step(void);
void      v71r7_layer0_get_metrics(FxZ8RearFlightMetrics *out);
int       v71r7_layer0_workspace_allocated(void);
uint32_t *v71r7_layer0_cache_acquire(uint32_t *gen_out);
uint32_t *v71r7_layer0_baked_acquire(uint32_t *gen_out);
void      v71r7_layer0_cache_release(void);

/* --- Native Photo Menu, Worker & LUT IPC (work/lut_menu/) --- */
void v71r7_photo_menu_boot_once(void); /* Triggered cleanly from live_ui_system_dispatch(ev==19) */
int  v71r7_photo_menu_resolve_text(uint32_t text_id, uint16_t *dst_utf16, uint32_t max_chars);
void v71r7_photo_menu_on_card_change(void);
int  v71r7_lut_ipc_service_call(uint32_t op, const void *req, uint32_t req_bytes,
                                void *resp, uint32_t *resp_bytes);

#endif /* V71R7_LUT_ABI_H */
