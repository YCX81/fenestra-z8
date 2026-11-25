/* SPDX-License-Identifier: MIT
 * Z8 FilmSim EL1 Runtime Coordinator (V66 baseline, zero-invasive Fenestra Core).
 *
 * - Zero writes / zero patches to Fenestra Core (0x401a0000..0x401e2000) or PB (0x40196000..0x4019f020).
 * - Strict Dormant Bypass: allocates 0 bytes (heap & class24) and spawns 0 workers
 *   when filmsim.wlm is unloaded OR when all stages are Off.
 * - Consolidated SINGLE 512B-aligned class24 arena block (V71.3 Fuji-stripped:
 *   27.5 KiB photo-grain assets / 667.5 KiB Mode 1 Grain-only Live / 5.40 MiB Mode 2 HDF Live
 *   <= 1 Z8 11.45 MiB granule) holding ZF_GRAIN.BIN + 512 KiB Live workspace + 128 KiB baked
 *   Zf grain tile + 3-slot NV12 triple-buffer (Mode 2 only).
 * - Dedicated async CPU2 Live worker (prio 30) + CPU2 Photo worker (prio 30):
 *   CPU0 capture/Wave5 worker NEVER stalls and NEVER executes NEON instructions.
 * - Temporally coherent fixed Live grain seed (keeps HEVC P-frame bitrate ~3 MiB/93f,
 *   well inside 8 MiB LIVE_BITSTREAM_BYTES); per-shot unique seed for still photos.
 * - Exports filmsim_runtime_metadata[] in .rodata & filmsim_runtime_service_call().
 * Freestanding C11, scalar-only (-mgeneral-regs-only), zero .data/.bss.
 */
#include "filmsim_runtime.h"
#include "../photo/z8_native_filmsim_photo.h"
#include "../xm6/z8_xm6_bridge.h"
#include "../zf_grain/z8_zf_grain.h"
#include "../zf_grain/z8_zf_grain_kernels.h"
#include "z8_filmsim_pipeline.h"
#include "z8_filmsim_stages.h"
#include "z8_filmsim_assets.h"
#include "ui/live_ui.h"
#include "capture/native_capture.h"
#include "codec/native_files.h"
#include "fenestra_module_bus.h"
#include "fenestra_native_menu.h"
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#if !defined(FILMSIM_HOST_TEST) || defined(V71R7_HOST_TEST)
#include "../../../lut_core/v71r7_lut_abi.h"
#include "../../../lut_core/lut_runtime.h"
#include "../../../lut_preview/layer0_provider.h"
#define FS_HAS_V71R7 1
#else
#define FS_HAS_V71R7 0
#endif

static inline int rt_lut_is_active(void)
{
#if FS_HAS_V71R7
    return v71r7_lut_is_active(0, 0);
#else
    return 0;
#endif
}

static inline int rt_still_lut_is_active(void)
{
#if FS_HAS_V71R7
    return v71r7_lut_is_active_for(V71R7_LUT_STAGE_STILL, 0, 0);
#else
    return 0;
#endif
}

static inline int rt_live_lut_is_active(uint32_t *gen_out)
{
#if FS_HAS_V71R7
    return v71r7_lut_is_active_for(V71R7_LUT_STAGE_LIVE, gen_out, 0);
#else
    if (gen_out) *gen_out = 0u;
    return 0;
#endif
}

static inline int rt_live_lut_wanted(void)
{
#if FS_HAS_V71R7
    return rt_live_lut_is_active(0) && ((live_status() & LIVE_REQUESTED) != 0u);
#else
    return 0;
#endif
}

static inline int rt_neon_allowed(void)
{
#if FS_HAS_V71R7
    return v71r7_neon_sentinel_is_allowed();
#else
    return 1;
#endif
}

static inline void rt_neon_probe_once(void)
{
#if FS_HAS_V71R7
    (void)v71r7_neon_sentinel_probe_once();
#endif
}

static inline void rt_lut_bind_xm6(struct z8_xm6_bridge *b, uint8_t mode)
{
#if FS_HAS_V71R7
    v71r7_lut_bind_xm6(b, mode);
#else
    (void)b; (void)mode;
#endif
}

static inline void rt_layer0_notify_wysiwyg(int hdf, int grain, uint32_t ver)
{
#if FS_HAS_V71R7
    v71r7_layer0_notify_wysiwyg_changed(hdf, grain, ver);
#else
    (void)hdf; (void)grain; (void)ver;
#endif
}

static inline void rt_layer0_mark_pool_suspended(void)
{
#if FS_HAS_V71R7
    V71r7DramAnchor *anc = v71r7_anchor();
    if (__atomic_load_n(&anc->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC) {
        __atomic_fetch_or(&anc->flags, V71R7_ANCHOR_FLAG_POOL_SUSPENDED, __ATOMIC_ACQ_REL);
    }
#endif
}

static inline void rt_layer0_pool_suspend(void)
{
#if FS_HAS_V71R7
    v71r7_layer0_pool_suspend();
#endif
}

static inline void rt_layer0_pool_resume(void)
{
#if FS_HAS_V71R7
    v71r7_layer0_pool_resume();
#endif
}

static inline void rt_layer0_lifecycle_stop(void)
{
#if FS_HAS_V71R7
    v71r7_layer0_lifecycle_stop();
#endif
}

static inline void rt_photo_menu_on_card_change(void)
{
#if FS_HAS_V71R7
    v71r7_photo_menu_on_card_change();
#endif
}

extern int live_runtime_post_roll_active(void);

#define FN(a, t) ((t)(uintptr_t)(a))
#define FILMSIM_RUNTIME_MAGIC UINT32_C(0x46535231) /* 'FSR1' */
#define FILMSIM_META_MAGIC    UINT32_C(0x46534d44) /* 'FSMD' */

/* Temporally coherent fixed seed for Live video so HEVC P-frames compress cleanly
 * (~3 MiB per 93-frame clip vs >10 MiB with per-frame random seeds). */
#define FILMSIM_LIVE_GRAIN_SEED UINT64_C(0x46534c5600000001)

#define FS_ADDR_MODULE_LOADED UINT64_C(0x401ccde4)
#define FS_ADDR_MENU_METADATA UINT64_C(0x401e0f38)
#define FS_ADDR_SLOT_POOL_AT  UINT64_C(0x401ac208)
#define FS_ADDR_STOCK_ROOTS   UINT64_C(0xa032b50c8)
#define FS_ADDR_ALLOC         UINT64_C(0x40d2c408)
#define FS_ADDR_FREE          UINT64_C(0x40d2c448)

#define FS_CARD_SLOT_MAGIC (*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa1171b000))
#define FS_CARD_SLOT_DRIVE (*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa1171b004))

#ifdef FILMSIM_HOST_TEST
extern void *filmsim_host_alloc(uint32_t bytes);
extern void  filmsim_host_free(void *ptr);
extern void  filmsim_host_sleep_ms(uint32_t ms);
extern char  filmsim_host_card_drive(void);
extern int   filmsim_host_module_loaded(void);
extern int   filmsim_host_bus_read(Z8FilmSimConfig *out_cfg, uint64_t *out_corr);
extern uint32_t filmsim_host_read_pc_sel(void);
extern int   filmsim_host_read_sc_recipe(uint32_t sc_index, Z8Np3Recipe *out);
#define FS_ALLOC(n)     filmsim_host_alloc((n))
#define FS_FREE(p)      filmsim_host_free((p))
#define FS_SLEEP_MS(ms) filmsim_host_sleep_ms((ms))
#else
#define FS_ALLOC(n)     FN(FS_ADDR_ALLOC, void *(*)(uint32_t, uint32_t))((n), 0u)
#define FS_FREE(p)      FN(FS_ADDR_FREE, void (*)(void *))((p))
#define FS_SLEEP_MS(ms) FN(0x40187b00, int (*)(uint32_t))((ms))
#endif

/* V71.5 Arena & Resident Grain Buffer layout:
 *   - ZF_GRAIN.BIN (28,160 B = 27.5 KiB) lives permanently in kernel heap (ctx->zf_grain_buf,
 *     NEVER inside Class 0x24 arena_blk and NEVER cleared on Playback/Movie release -> 0 reloads!).
 *   - Still-only Grain / HDF (live_link == OFF): 0 B Class 0x24 arena_blk held between shots!
 *   - Live Mode 1 (Grain-only): 655,360 B (640 KiB)
 *   - Live Mode 2 (HDF / HDF+Grain): 5,632,000 B (5.37 MiB) */
#define FS_ZF_BUF_BYTES            28160U    /* >= 27,588 B, resident kernel-heap buffer */
#define FS_ARENA_LWS_OFF           0U
#define FS_ARENA_LWS_SIZE          524288U   /* 512 KiB shared pipeline/bake scratch */
#define FS_ARENA_GTILE_OFF         524288U
#define FS_ARENA_GTILE_SIZE        131072U   /* 128 KiB: baked 512x256 int8_t twisted-torus grain tile */
#define FS_ARENA_GRAIN_LIVE_BYTES  655360U   /* Mode 1 Grain-only Live (640 KiB, 1,280 * 512) */

#define FS_ARENA_LFRAME_SIZE       1658880U  /* 1280 * 864 * 3 / 2, multiple of 512 */
#define FS_ARENA_LBUF0_OFF         655360U   /* Triple-buffer NV12 Slot 0 */
#define FS_ARENA_LBUF1_OFF         2314240U  /* Triple-buffer NV12 Slot 1 */
#define FS_ARENA_LBUF2_OFF         3973120U  /* Triple-buffer NV12 Slot 2 */
#define FS_ARENA_FULL_BYTES        5632000U  /* Mode 2 HDF / HDF+Grain Live (5.37 MiB, 11,000 * 512) */

_Static_assert((FS_ARENA_LWS_OFF & 511U) == 0U, "LWS 512B align");
_Static_assert((FS_ARENA_GTILE_OFF & 511U) == 0U && FS_ARENA_GTILE_OFF == FS_ARENA_LWS_OFF + FS_ARENA_LWS_SIZE, "GTILE 512B align");
_Static_assert((FS_ARENA_GRAIN_LIVE_BYTES & 511U) == 0U && FS_ARENA_GRAIN_LIVE_BYTES == FS_ARENA_GTILE_OFF + FS_ARENA_GTILE_SIZE, "GRAIN_LIVE 512B align");
_Static_assert(FS_ARENA_LBUF0_OFF == FS_ARENA_GRAIN_LIVE_BYTES &&
               FS_ARENA_LBUF1_OFF == FS_ARENA_LBUF0_OFF + FS_ARENA_LFRAME_SIZE &&
               FS_ARENA_LBUF2_OFF == FS_ARENA_LBUF1_OFF + FS_ARENA_LFRAME_SIZE &&
               FS_ARENA_FULL_BYTES == FS_ARENA_LBUF2_OFF + FS_ARENA_LFRAME_SIZE, "LBUF layout");

#define FS_LIVE_MAX_W         1280U
#define FS_LIVE_MAX_H         864U
#define FS_LIVE_SLICE_US      30000U

enum fs_live_state {
    FS_LV_IDLE    = 0U,
    FS_LV_COPYING = 1U,
    FS_LV_PENDING = 2U,
    FS_LV_WORKING = 3U,
    FS_LV_READY   = 4U
};

/* V71 3-slot lock-free NV12 triple-buffer per-slot state (3 bits/slot in lv_tb_state) */
enum fs_tb_slot_state {
    FS_TB_FREE         = 0U,
    FS_TB_CPU0_WRITING = 1U,
    FS_TB_PENDING_RAW  = 2U,
    FS_TB_CPU2_GRADING = 3U,
    FS_TB_GRADED_READY = 4U,
    FS_TB_CPU0_FRONT   = 5U,
    FS_TB_QUARANTINED  = 6U
};

static inline uint32_t fs_tb_get(uint32_t packed, unsigned slot)
{
    return (packed >> (slot * 3u)) & 7u;
}

static inline uint32_t fs_tb_set(uint32_t packed, unsigned slot, uint32_t st)
{
    return (packed & ~(7u << (slot * 3u))) | ((st & 7u) << (slot * 3u));
}

static inline uint32_t fs_arena_lbuf_off(unsigned slot)
{
    return FS_ARENA_LBUF0_OFF + slot * FS_ARENA_LFRAME_SIZE;
}

/* V71.7-r4 Layer 0 stage bitmask matching V71R7_L0_STAGE_* in layer0_provider.h */
#define FS_L0_STAGE_LUT   (1U << 0)
#define FS_L0_STAGE_HDF   (1U << 1)
#define FS_L0_STAGE_GRAIN (1U << 2)

typedef struct {
    _Atomic uint32_t seqlock; /* Single-writer (live_worker ONLY): even = consistent, odd = writing */
    uint32_t seq;             /* Monotonic published frame sequence (>= 1) */
    uint64_t capture_us;      /* Original frame capture timestamp (ctx->lv_slot_us[slot]) */
    uint64_t publish_us;      /* Completion timestamp (z8_fs_plat_time_us) */
    uint64_t slot_gen;        /* Expected ctx->lv_slot_gen[slot] */
    uint32_t slot;            /* Triple-buffer slot index 0..2 */
    uint32_t w;               /* Frame width (1024) */
    uint32_t h;               /* Frame height (680 or 682) */
    uint32_t stride;          /* Source Y & UV stride */
    uint32_t uv_off;          /* Offset from slot base to UV plane */
    uint32_t lut_gen;         /* Active LUT gen applied (0 if none) */
    uint32_t stages_applied;  /* FS_L0_STAGE_* bitmask (never sets GRAIN) */
    uint8_t  hdf_level;       /* HDF level applied (0 if none) */
    uint8_t  valid;           /* 1 if valid published frame */
    uint8_t  pad[2];
} FsLcdSharedFrame;

struct filmsim_runtime_context {
    uint32_t magic;
    uint32_t size;

    _Atomic uint32_t stopped;
    _Atomic uint32_t module_loaded;
    _Atomic uint32_t cfg_active;
    _Atomic uint32_t dormant;
    _Atomic uint32_t cfg_seqlock;
    uint32_t cfg_valid;
    _Atomic uint32_t photo_worker_ready;
    _Atomic uint32_t live_worker_ready;
    _Atomic uint32_t card_epoch;

    /* Reader/writer barrier protecting arena_blk across CPU0 worker, CPU2 live_worker,
     * CPU2 photo_worker, and CPU2 mux_worker. */
    _Atomic uint32_t asset_closing;
    _Atomic uint32_t asset_readers;

    uint64_t bus_corr;
    Z8FilmSimConfig cfg;

    uint64_t bus_poll_calls;
    uint64_t bus_hits;
    uint64_t bus_lock_skips;
    uint64_t cfg_updates;

    /* Asset & single-arena residency tracking (legacy LUT fields kept zero for ABI offsets) */
    uint32_t zf_maps_loaded;
    uint32_t zf_maps_tried_epoch;
    uint32_t lut_loaded;        /* ABI reserved (always 0; Fuji stripped) */
    uint32_t active_lut_slot;   /* ABI reserved (always 0; Fuji stripped) */
    uint32_t reserved_lut_pad0; /* ABI pad */
    uint32_t reserved_lut_pad1; /* ABI pad */
    int32_t  last_error;
    uint32_t live_ws_ready;

    uint32_t class24_alloc_count;
    uint32_t class24_release_count;
    uint64_t class24_active_bytes;

    /* Counters mirrored in Z8FilmSimStatus */
    _Atomic uint32_t live_applied;
    _Atomic uint32_t live_skipped;
    _Atomic uint32_t last_live_stages;
    _Atomic uint32_t next_shot_seq;
    uint32_t error;
    uint32_t pad0;

    /* Async CPU2 Live worker state (zero CPU0 capture stall) */
    _Atomic uint32_t lv_state;
    _Atomic uint32_t lv_cancel;
    uint32_t lv_front;
    uint32_t lv_front_valid;
    uint32_t lv_width;
    uint32_t lv_height;
    uint32_t lv_work_cfg_ver;
    uint32_t lv_front_cfg_ver;
    uint32_t lv_polls;
    uint32_t lv_pad1;
    uint64_t lv_source_us;
    uint64_t lv_front_us;
    uint64_t lv_slice_start_us;
    _Atomic uint64_t lv_submitted;
    _Atomic uint64_t lv_busy_skips;

    /* Single consolidated 512B-aligned class24 arena block (NULL while dormant) */
    struct z8_nfsp_allocation arena_blk;

    /* Validated zero-copy descriptors pointing inside arena_blk */
    z8_filmsim_zf_maps_box zf_maps_box;
    uint64_t reserved_lut3d_abi[9]; /* 72 B ABI pad (former z8_lut3d) preserving pool_suspended == 0x1000 */

    /* Photo coordinator + CPU2 photo worker (prio 30) + CPU2 live worker (prio 30) */
    struct z8_native_filmsim_photo photo;
    struct la_native_task photo_task;
    void *photo_stack;
    struct la_native_task live_task;
    void *live_stack;
    _Atomic uint32_t pool_suspended; /* appended: metadata ABI offsets unchanged */

    /* --- V71 appended fields (all V69/V70 metadata & diagnostic offsets 100% preserved!) --- */
    _Atomic uint32_t lv_tb_state;       /* 9-bit packed 3-slot triple-buffer state */
    uint32_t reserved_lut17_ready;      /* ABI pad (always 0; Fuji stripped) */
    _Atomic uint32_t gtile_ready;       /* 1 when baked 512x256 grain tile at GTILE_OFF is valid */
    _Atomic uint32_t gtile_readers;     /* Dekker reader count protecting GTILE re-bake */
    _Atomic uint32_t lv_cpu0_gtile_held;/* 1 while CPU0's current live borrow holds gtile_readers */
    uint32_t reserved_lut_epoch;        /* ABI pad (always 0) */
    uint32_t zf_epoch;                  /* Bumped whenever ZF_GRAIN.BIN loads/unloads/releases */
    uint32_t reserved_lut17_key;        /* ABI pad (always 0) */
    uint32_t reserved_lut17_epoch;      /* ABI pad (always 0) */
    uint32_t gtile_baked_key;
    uint32_t gtile_baked_epoch;
    uint32_t lv_slot_w[3];
    uint32_t lv_slot_h[3];
    uint32_t lv_slot_uv_off[3];
    uint32_t lv_slot_cfg_ver[3];
    uint64_t lv_slot_us[3];
    uint64_t lv_slot_gen[3];
    uint64_t lv_work_gen;

    /* --- V71.5 appended fields (all V69/V70/V71 offsets 0x00..0x1000 100% preserved!) --- */
    void *zf_grain_buf;                  /* Resident 28 KiB kernel-heap ZF_GRAIN.BIN (NOT Class 0x24!) */
    uint64_t zf_maps_retry_after_us;     /* 1s backoff retry timestamp on transient card read failure */
    struct z8_native_filmsim_photo photo_slot1; /* Burst Slot 1 (0 B Class 0x24 while PENDING) */

    /* NP3 Per-Slot Recipe Binding state */
    Z8FilmSimConfig base_cfg;            /* User/WLM config (global sliders + apply_mode) */
    uint32_t base_cfg_valid;
    uint32_t recipe_table_loaded;        /* 1 when FLMREC0/1.CFG or IPC populated recipe_table */
    uint32_t recipe_table_synced_by_ipc; /* 1 when updated via Z8_FS_OP_{SET_RECIPE,SYNC_RECIPES} */
    uint32_t recipe_table_tried_epoch;
    uint64_t recipe_table_retry_after_us;
    uint64_t recipe_table_bus_corr;      /* Last bus_corr at which FLMREC0/1.CFG was reloaded */
    _Atomic uint32_t recipe_seqlock;     /* Seqlock protecting recipe_table reads vs writes */
    _Atomic uint32_t active_pc_sel;      /* Last observed photo PC selector (0xA02983CF8[0]) */
    _Atomic uint32_t active_recipe_src;  /* enum Z8Np3RecipeSource (0=None, 1=SC_RECORD, 2=FLMREC) */
    _Atomic uint32_t active_recipe_bound;/* 0=unbound, 1=bound */
    Z8FilmSimRecipeTable recipe_table;   /* 776 B 95-slot companion recipe table */

    /* Optional Host/Unicorn test hooks for DRAM selector & SC_RECORD +0x39C reads */
    uint32_t (*np3_read_sel)(void *opaque);
    int      (*np3_read_sc_recipe)(void *opaque, uint32_t sc_index, Z8Np3Recipe *out);
    void    *np3_hook_ctx;

    /* --- V71.6 appended field (all V69/V70/V71/V71.5 offsets 0x00..0x1000 100% preserved!) --- */
    struct z8_xm6_bridge xm6;

    /* --- V71.7 appended WYSIWYG GTILE cache (strictly after xm6; 0x00..0x1000 frozen!) --- */
    int8_t  *wy_gtile_buf;
    uint32_t wy_gtile_key;
    uint32_t wy_gtile_epoch;
    uint32_t wy_gtile_ready;

    /* --- V71.7-r4 appended Live->LCD shared frame producer state (strictly after wy_gtile!) --- */
    FsLcdSharedFrame lcd_shared;
    _Atomic uint32_t lv_slot_lcd_pins[3];
    _Atomic uint32_t lcd_shared_valid; /* Multi-thread invalidation flag; seqlock stays single-writer! */

    /* --- V71.7-r7 appended Layer0->Live reverse sharing telemetry (strictly after lcd_shared!) --- */
    _Atomic uint64_t l0_feed_last_push_us;
    _Atomic uint64_t l0_feed_pushes;
};

/* Freeze all V69/V70 diagnostic offsets (filmsim_guest_diag_v70.py) */
_Static_assert(offsetof(struct filmsim_runtime_context, photo_worker_ready) == 0x20u, "photo_worker_ready ABI");
_Static_assert(offsetof(struct filmsim_runtime_context, live_worker_ready) == 0x24u, "live_worker_ready ABI");
_Static_assert(offsetof(struct filmsim_runtime_context, class24_release_count) == 0xC4u, "class24_release_count ABI");
_Static_assert(offsetof(struct filmsim_runtime_context, arena_blk) == 0x138u, "arena_blk ABI");
_Static_assert(offsetof(struct filmsim_runtime_context, photo) +
               offsetof(struct z8_native_filmsim_photo, job2_grain_bypasses) == 0xFCCu,
               "job2_grain_bypasses ABI");
_Static_assert(offsetof(struct filmsim_runtime_context, pool_suspended) == 0x1000u, "pool_suspended ABI");
_Static_assert(offsetof(struct filmsim_runtime_context, xm6) +
               offsetof(struct z8_xm6_bridge, xm6_busy) == 0x2238u,
               "xm6.xm6_busy ABI");
_Static_assert(offsetof(struct filmsim_runtime_context, xm6) +
               offsetof(struct z8_xm6_bridge, fallback_cnt) == 0x226Cu,
               "xm6.fallback_cnt ABI");

#define FS_FIELD(m) \
    (uint32_t)offsetof(struct filmsim_runtime_context, m), \
    (uint32_t)sizeof(((struct filmsim_runtime_context *)0)->m)
#define FS_PHOTO(m) \
    (uint32_t)(offsetof(struct filmsim_runtime_context, photo) + \
               offsetof(struct z8_native_filmsim_photo, m)), \
    (uint32_t)sizeof(((struct z8_native_filmsim_photo *)0)->m)

/* Read-only diagnostic layout table (ABI-compatible with existing QA parser) */
const uint32_t filmsim_runtime_metadata[] = {
    FILMSIM_META_MAGIC,
    1u,
    (uint32_t)sizeof(struct filmsim_runtime_context),
    FS_FIELD(module_loaded),
    FS_FIELD(cfg_active),
    FS_FIELD(dormant),
    FS_FIELD(cfg_updates),
    FS_FIELD(bus_hits),
    FS_FIELD(zf_maps_loaded),
    FS_FIELD(lut_loaded),
    FS_FIELD(active_lut_slot),
    FS_FIELD(last_error),
    FS_FIELD(class24_alloc_count),
    FS_FIELD(class24_active_bytes),
    FS_FIELD(live_applied),
    FS_FIELD(live_skipped),
    FS_FIELD(last_live_stages),
    FS_PHOTO(photos_completed),
    FS_PHOTO(photos_bypassed),
    FS_PHOTO(photos_failed),
    FS_PHOTO(last_stages_applied),
    FS_PHOTO(last_main_w),
    FS_PHOTO(last_main_h),
    FS_PHOTO(reason),
    FS_FIELD(cfg)
};

#undef FS_FIELD
#undef FS_PHOTO

static char filmsim_card_drive(void)
{
#ifdef FILMSIM_HOST_TEST
    return filmsim_host_card_drive();
#else
    return (FS_CARD_SLOT_MAGIC == UINT32_C(0x314c5343) && FS_CARD_SLOT_DRIVE == 2u) ? 'B' : 'A';
#endif
}

static struct filmsim_runtime_context *filmsim_ctx_get(void)
{
    _Atomic(void *) *slot = live_ui_filmsim_context_slot();
    if (!slot) {
        return 0;
    }
    struct filmsim_runtime_context *ctx =
        (struct filmsim_runtime_context *)atomic_load_explicit(slot, memory_order_acquire);
    if (!ctx || ctx->magic != FILMSIM_RUNTIME_MAGIC || ctx->size != sizeof(*ctx)) {
        return 0;
    }
    return ctx;
}

uint64_t filmsim_runtime_time_us(void)
{
    return z8_fs_plat_time_us();
}

static int filmsim_is_module_loaded(void)
{
#ifdef FILMSIM_HOST_TEST
    return filmsim_host_module_loaded();
#else
    static const char k_filmsim_id[16] = Z8_FILMSIM_MODULE_ID;
    return FN(FS_ADDR_MODULE_LOADED, uint32_t (*)(const char *))(k_filmsim_id) != 0u;
#endif
}

#ifndef FILMSIM_HOST_TEST
static int filmsim_ram_ptr_valid(uintptr_t addr, size_t bytes)
{
    return (addr & 7u) == 0u &&
           addr >= UINT64_C(0xa00000000) &&
           addr <= UINT64_C(0xa40000000) - bytes;
}

static FxModuleBus *filmsim_locate_module_bus(void)
{
    const uint32_t *meta = (const uint32_t *)(uintptr_t)FS_ADDR_MENU_METADATA;
    if (meta[0] != UINT32_C(0x465a394d) || meta[1] != 4u || meta[2] < 256u) {
        return 0;
    }
    uintptr_t root = ((const volatile uintptr_t *)(uintptr_t)FS_ADDR_STOCK_ROOTS)[2];
    if (!filmsim_ram_ptr_valid(root, 664u * sizeof(FxNativePage))) {
        return 0;
    }
    const FxNativePage *setup = (const FxNativePage *)(root + 663u * sizeof(FxNativePage));
    uintptr_t state_addr = atomic_load_explicit((const _Atomic uintptr_t *)&setup->descriptor,
                                                memory_order_acquire);
    if (state_addr == UINT64_C(0xa028ca040) || !filmsim_ram_ptr_valid(state_addr, meta[2])) {
        return 0;
    }
    uint32_t magic = *(const uint32_t *)(state_addr + meta[3]);
    uint32_t installed = *(const uint32_t *)(state_addr + meta[3] + 4u);
    if (magic != UINT32_C(0x465a394d) || !installed) {
        return 0;
    }
    uintptr_t bus_addr = atomic_load_explicit((const _Atomic uintptr_t *)(state_addr + meta[11]),
                                              memory_order_acquire);
    if (!bus_addr || !filmsim_ram_ptr_valid(bus_addr, sizeof(FxModuleBus))) {
        return 0;
    }
    return (FxModuleBus *)bus_addr;
}
#endif

static int filmsim_bus_read_latest(Z8FilmSimConfig *out_cfg, uint64_t *out_corr)
{
#ifdef FILMSIM_HOST_TEST
    Z8FilmSimConfig decoded;
    uint64_t corr = out_corr ? *out_corr : 0u;
    int r = filmsim_host_bus_read(&decoded, &corr);
    if (r <= 0) {
        return r;
    }
    if (!z8_filmsim_cfg_validate(&decoded)) {
        return 0;
    }
    z8_filmsim_cfg_normalize(&decoded);
    *out_cfg = decoded;
    if (out_corr) {
        *out_corr = corr;
    }
    return 1;
#else
    FxModuleBus *bus = filmsim_locate_module_bus();
    if (!bus) {
        return 0;
    }
    uint32_t zero = 0u;
    if (!atomic_compare_exchange_strong_explicit(&bus->lock, &zero, 1u,
                                                 memory_order_acquire,
                                                 memory_order_relaxed)) {
        return -1;
    }

    int found = 0;
    FxModuleMessage msg;
    memset(&msg, 0, sizeof(msg));
    uint32_t issued = bus->endpoints.issued;
    void *(*pool_at)(const FxSlotPool *, uint32_t) =
        FN(FS_ADDR_SLOT_POOL_AT, void *(*)(const FxSlotPool *, uint32_t));

    for (uint32_t i = 0u; i < issued; ++i) {
        const FxModuleEndpoint *ep = (const FxModuleEndpoint *)pool_at(&bus->endpoints, i);
        if (!ep || ep->version != Z8_FILMSIM_BINDING_VER) {
            continue;
        }
        if (memcmp(ep->name, Z8_FILMSIM_BINDING_NAME, sizeof(Z8_FILMSIM_BINDING_NAME)) != 0) {
            continue;
        }
        if (!ep->owner ||
            atomic_load_explicit(&ep->owner->phase, memory_order_acquire) != FX_OWNER_RUNNING) {
            continue;
        }
        if (ep->queue && ep->count > 0u && ep->capacity > 0u) {
            uint32_t newest = (ep->head + ep->count - 1u) % ep->capacity;
            msg = ep->queue[newest];
            found = 1;
        }
        break;
    }

    atomic_store_explicit(&bus->lock, 0u, memory_order_release);

    if (!found || msg.bytes != Z8_FILMSIM_CFG_BYTES) {
        return 0;
    }
    Z8FilmSimConfig decoded;
    memcpy(&decoded, msg.data, sizeof(decoded));
    if (!z8_filmsim_cfg_validate(&decoded)) {
        return 0;
    }
    z8_filmsim_cfg_normalize(&decoded);
    *out_cfg = decoded;
    if (out_corr) {
        *out_corr = msg.correlation;
    }
    return 1;
#endif
}

/* --- V71.5 O(1) NP3 Picture Control Selector & Per-Slot Recipe Resolution --- */

static uint32_t filmsim_read_active_pc_sel(const struct filmsim_runtime_context *ctx)
{
    if (ctx && ctx->np3_read_sel) {
        return ctx->np3_read_sel(ctx->np3_hook_ctx);
    }
#ifdef FILMSIM_HOST_TEST
    return filmsim_host_read_pc_sel();
#else
    return *(const volatile uint32_t *)(uintptr_t)Z8_NP3_ACTIVE_SEL_ADDR;
#endif
}

static int filmsim_read_sc_record_recipe(const struct filmsim_runtime_context *ctx,
                                         uint32_t sc_index, Z8Np3Recipe *out)
{
    if (!out || sc_index < Z8_NP3_C64_FIRST_INDEX || sc_index > Z8_NP3_C64_LAST_INDEX) {
        return 0;
    }
    if (ctx && ctx->np3_read_sc_recipe) {
        return ctx->np3_read_sc_recipe(ctx->np3_hook_ctx, sc_index, out);
    }
#ifdef FILMSIM_HOST_TEST
    return filmsim_host_read_sc_recipe(sc_index, out);
#else
    /* SC_RECORD +0x39C is 4-byte aligned (0x39C & 7 == 4), so validate 4B align + DRAM range
     * and read byte-wise for 100% SCTLR_EL1.A == 1 safety. */
    uintptr_t addr = (uintptr_t)(Z8_NP3_C64_DRAM_BASE +
                                 (uint64_t)sc_index * Z8_NP3_SC_RECORD_STRIDE +
                                 Z8_NP3_SC_RECORD_RECIPE_OFF);
    if ((addr & 3u) != 0u ||
        addr < UINT64_C(0xa00000000) ||
        addr > UINT64_C(0xa40000000) - Z8_NP3_RECIPE_BYTES) {
        return 0;
    }
    const volatile uint8_t *src = (const volatile uint8_t *)addr;
    uint8_t *dst = (uint8_t *)out;
    for (uint32_t i = 0u; i < Z8_NP3_RECIPE_BYTES; ++i) {
        dst[i] = src[i];
    }
    return 1;
#endif
}

/* Resolve recipe for raw Z8 selector:
 *   1. If C10..C64 (0x1009..0x103F), read SC_RECORD +0x39C ('Z8FX', CRC32 verified).
 *   2. Else (or if SC_RECORD tail absent/unbound), query 95-slot FLMREC0/1.CFG table.
 *   3. Returns 1 if an enabled recipe is bound; 0 otherwise (out_rec set to valid unbound block). */
static int filmsim_resolve_np3_recipe(const struct filmsim_runtime_context *ctx,
                                      uint32_t sel,
                                      Z8Np3Recipe *out_rec,
                                      uint32_t *out_src,
                                      uint32_t *out_bound)
{
    if (out_rec) {
        z8_np3_entry_to_recipe(0, out_rec);
    }
    if (out_src) {
        *out_src = Z8_NP3_SRC_NONE;
    }
    if (out_bound) {
        *out_bound = 0u;
    }

    uint32_t sc_idx = 0u;
    if (z8_np3_selector_is_c64(sel, &sc_idx)) {
        Z8Np3Recipe sc_rec;
        if (filmsim_read_sc_record_recipe(ctx, sc_idx, &sc_rec) &&
            z8_np3_recipe_is_valid(&sc_rec) &&
            (sc_rec.flags & Z8_NP3_RECIPE_FLAG_ENABLED) != 0u) {
            if (out_rec) {
                *out_rec = sc_rec;
            }
            if (out_src) {
                *out_src = Z8_NP3_SRC_SC_RECORD;
            }
            if (out_bound) {
                *out_bound = 1u;
            }
            return 1;
        }
    }

    uint32_t slot = z8_np3_selector_to_slot(sel);
    if (ctx && slot < Z8_NP3_SLOT_COUNT) {
        Z8Np3SlotEntry ent;
        for (;;) {
            uint32_t s1 = atomic_load_explicit(&ctx->recipe_seqlock, memory_order_acquire);
            if (s1 & 1u) {
                continue;
            }
            ent = ctx->recipe_table.entries[slot];
            uint32_t s2 = atomic_load_explicit(&ctx->recipe_seqlock, memory_order_acquire);
            if (s1 == s2) {
                break;
            }
        }
        if (z8_np3_entry_is_valid(&ent) && (ent.flags & Z8_NP3_RECIPE_FLAG_ENABLED) != 0u) {
            if (out_rec) {
                z8_np3_entry_to_recipe(&ent, out_rec);
            }
            if (out_src) {
                *out_src = Z8_NP3_SRC_FLMREC;
            }
            if (out_bound) {
                *out_bound = 1u;
            }
            return 1;
        }
    }
    return 0;
}

static void filmsim_cfg_from_recipe(const Z8Np3Recipe *r, uint8_t apply_mode,
                                    uint8_t xm6_mode, uint8_t wysiwyg_preview, uint32_t seq,
                                    Z8FilmSimConfig *out)
{
    z8_filmsim_cfg_default(out);
    out->apply_mode = apply_mode;
    out->xm6_mode = (xm6_mode <= Z8_XM6_MODE_ARM_ONLY) ? xm6_mode : (uint8_t)Z8_XM6_MODE_AUTO;
    out->wysiwyg_preview = (wysiwyg_preview <= Z8_WYSIWYG_ON) ? wysiwyg_preview : (uint8_t)Z8_WYSIWYG_OFF;
    if (r && (r->flags & Z8_NP3_RECIPE_FLAG_ENABLED)) {
        out->film_style = r->film_style;
        out->zf_intensity = (r->film_style == Z8_STYLE_NIKON_ZF) ? r->zf_intensity : 0u;
        out->zf_size = (r->zf_size >= Z8_ZF_SIZE_MIN && r->zf_size <= Z8_ZF_SIZE_MAX)
                           ? r->zf_size : (uint8_t)Z8_ZF_SIZE_DEFAULT;
        out->zf_blur_mode = r->zf_blur_mode;
        out->hdf_level = r->hdf_level;
        out->live_link = r->live_link;
    }
    z8_filmsim_cfg_finalize(out, seq);
}

static void filmsim_resolve_effective_cfg(const struct filmsim_runtime_context *ctx,
                                          const Z8FilmSimConfig *base_cfg,
                                          Z8FilmSimConfig *eff_out,
                                          uint32_t *out_sel,
                                          uint32_t *out_src,
                                          uint32_t *out_bound)
{
    uint32_t sel = filmsim_read_active_pc_sel(ctx);
    uint32_t src = Z8_NP3_SRC_NONE;
    uint32_t bound = 0u;

    if (!base_cfg) {
        z8_filmsim_cfg_default(eff_out);
    } else if (base_cfg->apply_mode == Z8_FS_APPLY_PER_NP3) {
        Z8Np3Recipe rec;
        if (filmsim_resolve_np3_recipe(ctx, sel, &rec, &src, &bound)) {
            filmsim_cfg_from_recipe(&rec, Z8_FS_APPLY_PER_NP3, base_cfg->xm6_mode,
                                    base_cfg->wysiwyg_preview, base_cfg->seq, eff_out);
        } else {
            z8_filmsim_cfg_default(eff_out);
            eff_out->apply_mode = Z8_FS_APPLY_PER_NP3;
            eff_out->xm6_mode = base_cfg->xm6_mode;
            eff_out->wysiwyg_preview = base_cfg->wysiwyg_preview;
            z8_filmsim_cfg_finalize(eff_out, base_cfg->seq);
        }
    } else {
        *eff_out = *base_cfg;
    }

    if (out_sel) {
        *out_sel = sel;
    }
    if (out_src) {
        *out_src = src;
    }
    if (out_bound) {
        *out_bound = bound;
    }
}

static void filmsim_publish_cfg(struct filmsim_runtime_context *ctx,
                                const Z8FilmSimConfig *cfg,
                                uint32_t valid, uint32_t loaded, uint64_t corr)
{
    uint32_t is_dormant = (!loaded || !valid || z8_filmsim_cfg_is_dormant(cfg)) ? 1u : 0u;
    uint32_t active = is_dormant ? 0u : 1u;
    uint32_t changed = (valid != ctx->cfg_valid) ||
                       (corr != ctx->bus_corr) ||
                       (memcmp(&ctx->cfg, cfg, sizeof(*cfg)) != 0);

    atomic_store_explicit(&ctx->module_loaded, loaded, memory_order_release);
    if (!changed &&
        atomic_load_explicit(&ctx->cfg_active, memory_order_acquire) == active &&
        atomic_load_explicit(&ctx->dormant, memory_order_acquire) == is_dormant) {
        return;
    }

    uint32_t s = atomic_load_explicit(&ctx->cfg_seqlock, memory_order_relaxed);
    atomic_store_explicit(&ctx->cfg_seqlock, s + 1u, memory_order_release);
    ctx->cfg = *cfg;
    ctx->cfg_valid = valid;
    ctx->bus_corr = corr;
    if (changed && valid) {
        ++ctx->cfg_updates;
    }
    atomic_store_explicit(&ctx->cfg_seqlock, s + 2u, memory_order_release);
    atomic_store_explicit(&ctx->cfg_active, active, memory_order_release);
    atomic_store_explicit(&ctx->dormant, is_dormant, memory_order_release);

    rt_lut_bind_xm6(&ctx->xm6, cfg->xm6_mode);
    int wy_en = (active && cfg->wysiwyg_preview == Z8_WYSIWYG_ON) ? 1 : 0;
    rt_layer0_notify_wysiwyg(
        (wy_en && z8_filmsim_cfg_needs_hdf(cfg)) ? (int)cfg->hdf_level : 0,
        (wy_en && z8_filmsim_cfg_needs_grain(cfg)) ? (int)cfg->zf_intensity : 0,
        (uint32_t)ctx->cfg_updates);
}

static void filmsim_snapshot_cfg(const struct filmsim_runtime_context *ctx,
                                 Z8FilmSimConfig *out_cfg,
                                 uint32_t *out_active,
                                 uint32_t *out_ver)
{
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&ctx->cfg_seqlock, memory_order_acquire);
        if (s1 & 1u) {
            continue;
        }
        Z8FilmSimConfig tmp = ctx->cfg;
        uint32_t active = atomic_load_explicit(&ctx->cfg_active, memory_order_acquire);
        uint32_t ver = (uint32_t)ctx->cfg_updates;
        uint32_t s2 = atomic_load_explicit(&ctx->cfg_seqlock, memory_order_acquire);
        if (s1 == s2) {
            *out_cfg = tmp;
            if (out_active) {
                *out_active = active;
            }
            if (out_ver) {
                *out_ver = ver;
            }
            return;
        }
    }
}

/* --- Reader/writer barrier protecting single arena_blk --- */

static int filmsim_reader_enter(struct filmsim_runtime_context *ctx)
{
    if (!ctx || atomic_load_explicit(&ctx->asset_closing, memory_order_acquire)) {
        return 0;
    }
    atomic_fetch_add_explicit(&ctx->asset_readers, 1u, memory_order_seq_cst);
    if (atomic_load_explicit(&ctx->asset_closing, memory_order_seq_cst)) {
        atomic_fetch_sub_explicit(&ctx->asset_readers, 1u, memory_order_release);
        return 0;
    }
    return 1;
}

static void filmsim_reader_leave(struct filmsim_runtime_context *ctx)
{
    if (ctx) {
        atomic_fetch_sub_explicit(&ctx->asset_readers, 1u, memory_order_release);
    }
}

static int filmsim_photo_acquire_assets(void *opaque, z8_filmsim_assets *out)
{
    struct filmsim_runtime_context *ctx = (struct filmsim_runtime_context *)opaque;
    if (!out || !filmsim_reader_enter(ctx)) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->zf_maps = ctx->zf_maps_loaded ? (const void *)ctx->zf_maps_box.storage : 0;
    out->xm6_bridge = &ctx->xm6;
    out->xm6_zf_raw_buf = ctx->zf_maps_loaded ? ctx->zf_grain_buf : 0;
    return 1;
}

static void filmsim_photo_release_assets(void *opaque)
{
    filmsim_reader_leave((struct filmsim_runtime_context *)opaque);
}

static int filmsim_asset_writer_try_lock(struct filmsim_runtime_context *ctx)
{
    uint32_t expected = 0u;
    if (!atomic_compare_exchange_strong_explicit(&ctx->asset_closing, &expected, 1u,
                                                 memory_order_seq_cst,
                                                 memory_order_acquire)) {
        return 0;
    }
    atomic_store_explicit(&ctx->lv_cancel, 1u, memory_order_release);
    if (atomic_load_explicit(&ctx->asset_readers, memory_order_seq_cst) != 0u) {
        atomic_store_explicit(&ctx->asset_closing, 0u, memory_order_release);
        return 0;
    }
    return 1;
}

static void filmsim_asset_writer_unlock(struct filmsim_runtime_context *ctx)
{
    if (!atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire)) {
        atomic_store_explicit(&ctx->lv_cancel, 0u, memory_order_release);
    }
    atomic_store_explicit(&ctx->asset_closing, 0u, memory_order_release);
}

static void filmsim_lcd_shared_invalidate(struct filmsim_runtime_context *ctx)
{
    if (!ctx) {
        return;
    }
    /* Strictly single-writer seqlock invariant: pool_suspend / on_stop / reset ONLY
     * clear lcd_shared_valid and NEVER touch lcd_shared.seqlock, making a stuck-odd
     * seqlock physically impossible across concurrent Live publish vs suspend! */
    atomic_store_explicit(&ctx->lcd_shared_valid, 0u, memory_order_release);
    atomic_store_explicit(&ctx->l0_feed_last_push_us, 0u, memory_order_release);
}

static void filmsim_reset_live_tb(struct filmsim_runtime_context *ctx)
{
    filmsim_lcd_shared_invalidate(ctx);
    ctx->lv_front_valid = 0u;
    ctx->lv_front = 0u;
    atomic_store_explicit(&ctx->lv_state, FS_LV_IDLE, memory_order_release);
    atomic_store_explicit(&ctx->lv_tb_state, 0u, memory_order_release);
    atomic_store_explicit(&ctx->gtile_ready, 0u, memory_order_release);
    atomic_store_explicit(&ctx->gtile_readers, 0u, memory_order_release);
    atomic_store_explicit(&ctx->lv_cpu0_gtile_held, 0u, memory_order_release);
    for (unsigned i = 0u; i < 3u; ++i) {
        atomic_store_explicit(&ctx->lv_slot_lcd_pins[i], 0u, memory_order_release);
        ctx->lv_slot_w[i] = 0u;
        ctx->lv_slot_h[i] = 0u;
        ctx->lv_slot_uv_off[i] = 0u;
        ctx->lv_slot_cfg_ver[i] = 0u;
        ctx->lv_slot_us[i] = 0u;
        ctx->lv_slot_gen[i] = 0u;
    }
}

static void filmsim_release_arena(struct filmsim_runtime_context *ctx)
{
    /* V71.5: ZF_GRAIN.BIN (28 KiB) lives permanently in kernel heap (ctx->zf_grain_buf),
     * NOT inside Class 0x24 arena_blk -> NEVER clear ctx->zf_maps_loaded here!
     * Playback/Movie transitions leave Zf grain maps hot in RAM with 0 card reloads. */
    ctx->live_ws_ready = 0u;
    filmsim_reset_live_tb(ctx);

    if (ctx->arena_blk.handle) {
#ifndef FILMSIM_HOST_TEST
        /* UAF guard: NEVER release Live arena_blk while an XM6 DSP job is in flight,
         * OR after a hard breaker trip while Core 0 is still running & non-idle.
         * When Core 0 is off/absent (hw_state != 2, e.g. QEMU/sleep/movie) or bridge
         * is healthy with req_in_flight == 0 (already drained), always release so
         * HangFix V69 never leaks the 11.45 MiB Class 0x24 arena. */
        if (__atomic_load_n(&ctx->xm6.req_in_flight, __ATOMIC_ACQUIRE) != 0U) {
            return;
        }
        if (__atomic_load_n(&ctx->xm6.breaker_tripped, __ATOMIC_ACQUIRE) != 0U) {
            uint32_t hw_state = 0U;
            uint8_t lf = 0U, fl = 0U;
            z8_xm6_read_hw_raw(&ctx->xm6, &hw_state, &lf, &fl);
            if (hw_state == 2U && !z8_xm6_core0_mailbox_idle(&ctx->xm6)) {
                return;
            }
        }
#endif
        uint64_t bytes = ctx->arena_blk.actual;
        z8_fs_plat_release_class24(&ctx->arena_blk);
        ++ctx->class24_release_count;
        if (ctx->class24_active_bytes >= bytes) {
            ctx->class24_active_bytes -= bytes;
        } else {
            ctx->class24_active_bytes = 0u;
        }
    }
}

static int filmsim_ensure_arena(struct filmsim_runtime_context *ctx, uint32_t need_bytes)
{
    if (ctx->arena_blk.handle && ctx->arena_blk.actual >= need_bytes) {
        if (ctx->arena_blk.used < need_bytes) {
            ctx->arena_blk.used = need_bytes;
        }
        return 1;
    }
    /* Re-allocating invalidates existing zero-copy pointers into old arena */
    filmsim_release_arena(ctx);
    if (!z8_fs_plat_alloc_class24_xm6(need_bytes, &ctx->arena_blk) &&
        !z8_fs_plat_alloc_class24(need_bytes, &ctx->arena_blk)) {
        return 0;
    }
    if (ctx->arena_blk.actual < need_bytes ||
        !z8_fs_plat_clean_dcache(ctx->arena_blk.cpu, ctx->arena_blk.actual)) {
        z8_fs_plat_release_class24(&ctx->arena_blk);
        return 0;
    }
    ctx->arena_blk.used = need_bytes;
    ++ctx->class24_alloc_count;
    ctx->class24_active_bytes += ctx->arena_blk.actual;
    return 1;
}

/* Native card file adapter bridging live_file_ops -> z8_filmsim_file_ops */
struct fs_native_file_wrap {
    live_file_ops fops;
    live_native_file_context fctx;
    struct filmsim_runtime_context *rt;
};

static int fs_wrap_open_read(void *op, const char *path, int *fd_out)
{
    struct fs_native_file_wrap *w = (struct fs_native_file_wrap *)op;
    return w->fops.open_read(w->fops.opaque, path, fd_out);
}

static int fs_wrap_read(void *op, int fd, void *buf, size_t n, size_t *got)
{
    struct fs_native_file_wrap *w = (struct fs_native_file_wrap *)op;
    return w->fops.read(w->fops.opaque, fd, buf, n, got);
}

static int fs_wrap_close(void *op, int fd)
{
    struct fs_native_file_wrap *w = (struct fs_native_file_wrap *)op;
    return w->fops.close(w->fops.opaque, fd);
}

static int fs_wrap_cancelled(void *op)
{
    struct fs_native_file_wrap *w = (struct fs_native_file_wrap *)op;
    return atomic_load_explicit(&w->rt->stopped, memory_order_acquire) != 0u;
}

static void filmsim_maybe_load_recipe_table(struct filmsim_runtime_context *ctx,
                                            uint64_t corr, int force_bus_reload)
{
    uint32_t epoch = atomic_load_explicit(&ctx->card_epoch, memory_order_acquire) + 1u;
    int card_changed = (ctx->recipe_table_tried_epoch != 0u &&
                        ctx->recipe_table_tried_epoch != epoch);
    uint64_t now = z8_fs_plat_time_us();

    int need_load = 0;
    if (card_changed || force_bus_reload || corr != ctx->recipe_table_bus_corr) {
        need_load = 1;
    } else if (!ctx->recipe_table_loaded) {
        if (ctx->recipe_table_tried_epoch != epoch || now >= ctx->recipe_table_retry_after_us) {
            need_load = 1;
        }
    }
    if (!need_load) {
        return;
    }

    struct fs_native_file_wrap fw;
    memset(&fw, 0, sizeof(fw));
    fw.rt = ctx;
    live_native_file_ops(&fw.fops, &fw.fctx);
    z8_filmsim_file_ops fops = {
        .opaque = &fw,
        .open_read = fs_wrap_open_read,
        .read = fs_wrap_read,
        .close = fs_wrap_close,
        .cancelled = fs_wrap_cancelled
    };
    char drive = filmsim_card_drive();
    Z8FilmSimRecipeTable cand;
    int rc = z8_filmsim_load_recipe_table(&fops, drive, &cand);
    ctx->recipe_table_tried_epoch = epoch;
    ctx->recipe_table_bus_corr = corr;

    if (rc == Z8_FS_ASSET_OK) {
        /* Avoid regressing an IPC-synced table that already has a strictly newer seq
         * unless the card itself was swapped. */
        if (!ctx->recipe_table_loaded || !ctx->recipe_table_synced_by_ipc ||
            card_changed || cand.seq >= ctx->recipe_table.seq) {
            uint32_t s = atomic_load_explicit(&ctx->recipe_seqlock, memory_order_relaxed);
            atomic_store_explicit(&ctx->recipe_seqlock, s + 1u, memory_order_release);
            ctx->recipe_table = cand;
            atomic_store_explicit(&ctx->recipe_seqlock, s + 2u, memory_order_release);
        }
        ctx->recipe_table_loaded = 1u;
        ctx->recipe_table_retry_after_us = 0u;
    } else {
        /* Backoff 5s when FLMREC is simply absent (E_OPEN), 1s on transient I/O errors;
         * any subsequent bus_corr bump or card swap retries immediately. */
        uint64_t delay = (rc == Z8_FS_ASSET_E_OPEN) ? UINT64_C(5000000) : UINT64_C(1000000);
        ctx->recipe_table_retry_after_us = now + delay;
    }
}

static void filmsim_sync_assets(struct filmsim_runtime_context *ctx,
                                const Z8FilmSimConfig *cfg,
                                uint32_t is_dormant)
{
    uint32_t epoch = atomic_load_explicit(&ctx->card_epoch, memory_order_acquire) + 1u;
    int want_live_lut = rt_live_lut_wanted();

    /* 1. Strict Dormant cleanup: release Class 0x24 Live arena & XM6 ext_ws once readers & photos are idle.
     *    Resident 28 KiB zf_grain_buf stays hot in kernel heap (0 Class 0x24, 0 card reloads). */
    if (is_dormant && !want_live_lut) {
        if (z8_nfsp_class24_busy(&ctx->photo) || z8_nfsp_class24_busy(&ctx->photo_slot1)) {
            return;
        }
        z8_xm6_release_ext_ws(&ctx->xm6);
        if (!ctx->arena_blk.handle) {
            return;
        }
        if (!filmsim_asset_writer_try_lock(ctx)) {
            return;
        }
        filmsim_release_arena(ctx);
        filmsim_asset_writer_unlock(ctx);
        return;
    }

    int want_hdf = !is_dormant && z8_filmsim_cfg_needs_hdf(cfg);
    if (!want_hdf &&
        !z8_nfsp_class24_busy(&ctx->photo) && !z8_nfsp_class24_busy(&ctx->photo_slot1)) {
        z8_xm6_release_ext_ws(&ctx->xm6);
    }
    int want_grain = !is_dormant && z8_filmsim_cfg_needs_grain(cfg);
    int want_live = want_live_lut ||
                    (!is_dormant && (cfg->live_link == Z8_LIVE_LINK_ON) && (want_hdf || want_grain));

    /* V71.5/V71.7: Still-only Grain (live_link == OFF) needs 0 bytes of Class 0x24 arena_blk
     * because ZF_GRAIN.BIN lives permanently in kernel heap (ctx->zf_grain_buf)!
     * Live Photo + LUT uses FS_ARENA_FULL_BYTES ONLY while Live Photo is requested. */
    uint32_t need_arena = (want_live && (want_hdf || want_live_lut)) ? FS_ARENA_FULL_BYTES :
                          (want_live && want_grain) ? FS_ARENA_GRAIN_LIVE_BYTES : 0u;

    uint64_t now = z8_fs_plat_time_us();
    int zf_retry_ok = (ctx->zf_maps_tried_epoch != epoch) ||
                      (now >= ctx->zf_maps_retry_after_us);
    int need_zf_load = want_grain && !ctx->zf_maps_loaded && zf_retry_ok;
    int need_alloc = (need_arena > 0u) &&
                     (!ctx->arena_blk.handle || ctx->arena_blk.actual < need_arena);
    int need_free_all = (need_arena == 0u) && (ctx->arena_blk.handle != 0);
    int need_live_toggle = (want_live && !ctx->live_ws_ready) ||
                           (!want_live && ctx->live_ws_ready);

    if (!need_alloc && !need_free_all && !need_zf_load && !need_live_toggle) {
        return;
    }
    if (need_alloc && !z8_fs_plat_pool_valid()) {
        /* Even if Class 0x24 pool is busy, still allow loading resident 28 KiB ZF_GRAIN.BIN! */
        need_alloc = 0;
        if (!need_zf_load) {
            return;
        }
    }
    if (!filmsim_asset_writer_try_lock(ctx)) {
        return;
    }

    /* Resident 28 KiB ZF_GRAIN.BIN in kernel heap (allocated once, NEVER cleared on arena release,
     * with 1-second backoff retry on transient card errors instead of permanent epoch lockout). */
    if (need_zf_load) {
        if (!ctx->zf_grain_buf) {
            ctx->zf_grain_buf = FS_ALLOC(FS_ZF_BUF_BYTES);
        }
        if (!ctx->zf_grain_buf) {
            ctx->zf_maps_tried_epoch = epoch;
            ctx->zf_maps_retry_after_us = now + UINT64_C(1000000);
            ctx->last_error = Z8_FS_E_WORKSPACE;
        } else {
            struct fs_native_file_wrap fw;
            memset(&fw, 0, sizeof(fw));
            fw.rt = ctx;
            live_native_file_ops(&fw.fops, &fw.fctx);
            z8_filmsim_file_ops fops = {
                .opaque = &fw,
                .open_read = fs_wrap_open_read,
                .read = fs_wrap_read,
                .close = fs_wrap_close,
                .cancelled = fs_wrap_cancelled
            };
            char drive = filmsim_card_drive();
            int rc = z8_filmsim_load_zf_grain_bin(&fops, drive,
                                                  ctx->zf_grain_buf,
                                                  FS_ZF_BUF_BYTES,
                                                  &ctx->zf_maps_box);
            if (rc == Z8_FS_ASSET_OK) {
                /* Flush read-only ZF_GRAIN.BIN (27,588 B) once to DRAM for XM6 Core 0 */
                z8_xm6_dcache_civac(&ctx->xm6, ctx->zf_grain_buf, Z8_XM6_ZFG1_CLEAN_BYTES);
                ctx->zf_maps_loaded = 1u;
                ctx->zf_maps_tried_epoch = 0u;
                ctx->zf_maps_retry_after_us = 0u;
                ++ctx->zf_epoch;
                atomic_store_explicit(&ctx->gtile_ready, 0u, memory_order_release);
                ctx->last_error = 0;
            } else {
                ctx->zf_maps_loaded = 0u;
                ctx->zf_maps_tried_epoch = epoch;
                ctx->zf_maps_retry_after_us = now + UINT64_C(1000000);
                ctx->last_error = rc;
            }
        }
    }

    if (need_free_all) {
        if (!z8_nfsp_class24_busy(&ctx->photo) && !z8_nfsp_class24_busy(&ctx->photo_slot1)) {
            filmsim_release_arena(ctx);
        }
        filmsim_asset_writer_unlock(ctx);
        return;
    }

    if (need_arena > 0u && z8_fs_plat_pool_valid()) {
        if (!filmsim_ensure_arena(ctx, need_arena)) {
            ctx->last_error = Z8_FS_E_WORKSPACE;
            filmsim_asset_writer_unlock(ctx);
            return;
        }
    }

    /* Live workspace + GTILE (+ 3-slot NV12 buffers for Mode 2 HDF / LUT) live in arena_blk */
    uint32_t live_min = (want_hdf || want_live_lut) ? FS_ARENA_FULL_BYTES : FS_ARENA_GRAIN_LIVE_BYTES;
    if (want_live && ctx->arena_blk.handle && ctx->arena_blk.actual >= live_min) {
        if (!ctx->live_ws_ready) {
            filmsim_reset_live_tb(ctx);
            ctx->live_ws_ready = 1u;
        }
    } else {
        ctx->live_ws_ready = 0u;
        filmsim_reset_live_tb(ctx);
    }

    filmsim_asset_writer_unlock(ctx);
}

/* --- Dedicated CPU2 Workers: Photo (prio 30) & Async Live (prio 30) --- */

static int filmsim_live_needs_cpu2(const struct filmsim_runtime_context *ctx)
{
    if (!ctx || atomic_load_explicit(&ctx->stopped, memory_order_acquire) ||
        atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire) ||
        (atomic_load_explicit(&ctx->dormant, memory_order_acquire) && !rt_lut_is_active()) ||
        atomic_load_explicit(&ctx->lv_cancel, memory_order_acquire)) {
        return 0;
    }
    uint32_t st = atomic_load_explicit(&ctx->lv_tb_state, memory_order_acquire);
    for (unsigned s = 0u; s < 3u; ++s) {
        uint32_t v = fs_tb_get(st, s);
        if (v == FS_TB_PENDING_RAW || v == FS_TB_CPU2_GRADING) {
            return 1;
        }
    }
    return 0;
}

static int filmsim_photo_should_defer(void *opaque, uint64_t stage_start_us)
{
    struct filmsim_runtime_context *ctx = (struct filmsim_runtime_context *)opaque;
    int post_roll = live_runtime_post_roll_active();
    /* Sentinel UINT64_MAX queried inside cancelled_cb() switches Still photo's
     * cooperative yield slice from 16ms -> 3ms whenever Mode 2 Live or post-roll is active. */
    if (stage_start_us == UINT64_MAX) {
        return filmsim_live_needs_cpu2(ctx) || post_roll;
    }
    /* V71.7-r7 Post-Shutter Live 29.97fps Fix:
     * Real-silicon MOV forensics (DSC_9335..9337) showed starting 45MP Still immediately at
     * t+40ms collapses post-shutter Live to 5.8 fps (~80% frozen CFR repeats). Defer starting
     * PENDING->COPY (0 B Class 0x24 held, 0 XM6 contention) until the 1.5s Live post-roll
     * finishes (hard-capped at 1.65s << 30s FILMSIM_PHOTO_TIMEOUT_US), bypassing deferral
     * immediately during bursts when both Slot 0 and Slot 1 are occupied! */
    if (ctx && post_roll && stage_start_us != 0u) {
        uint32_t p0 = __atomic_load_n(&ctx->photo.phase, __ATOMIC_ACQUIRE);
        uint32_t p1 = __atomic_load_n(&ctx->photo_slot1.phase, __ATOMIC_ACQUIRE);
        int burst_full = (p0 != Z8_NFSP_IDLE && p1 != Z8_NFSP_IDLE);
        if (!burst_full) {
            uint64_t now = z8_fs_plat_time_us();
            if (now >= stage_start_us && (now - stage_start_us) < UINT64_C(1650000)) {
                return 1;
            }
        }
    }
    /* Brief <= 40ms yield if independent Live HDF is finishing an in-flight frame. */
    if (filmsim_live_needs_cpu2(ctx) && stage_start_us != 0u) {
        uint64_t now = z8_fs_plat_time_us();
        if (now >= stage_start_us && now - stage_start_us < UINT64_C(40000)) {
            return 1;
        }
    }
    return 0;
}

static void filmsim_photo_worker_step(struct filmsim_runtime_context *ctx)
{
    struct z8_native_filmsim_photo *s0 = &ctx->photo;
    struct z8_native_filmsim_photo *s1 = &ctx->photo_slot1;

    z8_nfsp_reclaim_cancelled(s0);
    z8_nfsp_reclaim_cancelled(s1);

    uint32_t p0 = __atomic_load_n(&s0->phase, __ATOMIC_ACQUIRE);
    uint32_t p1 = __atomic_load_n(&s1->phase, __ATOMIC_ACQUIRE);
    int b0 = z8_nfsp_class24_busy(s0);
    int b1 = z8_nfsp_class24_busy(s1);

    /* Pick oldest PENDING slot (FIFO by monotonic shot_seq), strictly serializing Class 0x24
     * allocation so peak Class 0x24 usage never exceeds 1 shot while burst Shot 2 queues safely. */
    struct z8_native_filmsim_photo *pick = 0;
    if (p0 == Z8_NFSP_PENDING && !b1) {
        pick = s0;
    }
    if (p1 == Z8_NFSP_PENDING && !b0) {
        if (!pick || (int32_t)(s1->shot_seq - s0->shot_seq) < 0) {
            pick = s1;
        }
    }

    if (pick) {
        if (!__atomic_load_n(&pick->cancel, __ATOMIC_ACQUIRE) &&
            !pick->retire &&
            filmsim_photo_should_defer(ctx, pick->stage_start_us)) {
            return;
        }
        z8_nfsp_worker(pick);
    } else {
        if (p0 != Z8_NFSP_IDLE && p0 != Z8_NFSP_PENDING) {
            z8_nfsp_worker(s0);
        }
        if (p1 != Z8_NFSP_IDLE && p1 != Z8_NFSP_PENDING) {
            z8_nfsp_worker(s1);
        }
    }
}

static void filmsim_photo_worker_entry(void *arg)
{
    struct filmsim_runtime_context *ctx = (struct filmsim_runtime_context *)arg;
    rt_neon_probe_once();
    for (;;) {
        filmsim_photo_worker_step(ctx);
        uint32_t p0 = __atomic_load_n(&ctx->photo.phase, __ATOMIC_ACQUIRE);
        uint32_t p1 = __atomic_load_n(&ctx->photo_slot1.phase, __ATOMIC_ACQUIRE);
        uint32_t delay_ms = 1u;
        if (p0 == Z8_NFSP_IDLE && p1 == Z8_NFSP_IDLE) {
            int quiet = atomic_load_explicit(&ctx->stopped, memory_order_acquire) ||
                        atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire) ||
                        (atomic_load_explicit(&ctx->dormant, memory_order_acquire) && !rt_lut_is_active());
            delay_ms = quiet ? 20u : 2u;
        }
        FS_SLEEP_MS(delay_ms);
    }
}

static int filmsim_live_cancelled(void *opaque)
{
    struct filmsim_runtime_context *ctx = (struct filmsim_runtime_context *)opaque;
    if (atomic_load_explicit(&ctx->stopped, memory_order_acquire) != 0u ||
        (atomic_load_explicit(&ctx->dormant, memory_order_acquire) != 0u && !rt_lut_is_active()) ||
        atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire) != 0u ||
        atomic_load_explicit(&ctx->lv_cancel, memory_order_acquire) != 0u ||
        atomic_load_explicit(&ctx->asset_closing, memory_order_acquire) != 0u) {
        return 1;
    }
    /* V71.3: 30 ms cooperative time-slice yield on CPU2 (keeps >= 33 Hz service for
     * WTDG_C3(40) / AFLENS(36) while avoiding mid-frame dly_tsk(0) on 25-29 ms frames). */
    if ((++ctx->lv_polls & 15u) == 0u) {
        uint64_t now = z8_fs_plat_time_us();
        if (now < ctx->lv_slice_start_us || now - ctx->lv_slice_start_us >= FS_LIVE_SLICE_US) {
            la_native_yield();
            ctx->lv_slice_start_us = z8_fs_plat_time_us();
        }
    }
    return atomic_load_explicit(&ctx->stopped, memory_order_acquire) != 0u ||
           (atomic_load_explicit(&ctx->dormant, memory_order_acquire) != 0u && !rt_lut_is_active()) ||
           atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire) != 0u ||
           atomic_load_explicit(&ctx->lv_cancel, memory_order_acquire) != 0u;
}

/* Lazy-bake 512x256 GTILE (128 KiB slot) on CPU2 live_worker whenever Zf grain
 * settings or ZF_GRAIN.BIN epoch change (runs in both Mode 1 GRAIN_ONLY and Mode 2 HDF+Grain!).
 * Returns 1 if a bake was executed, 0 if idle/up-to-date. */
static int filmsim_live_bake_locked(struct filmsim_runtime_context *ctx,
                                    const Z8FilmSimConfig *cfg)
{
    int need_grain = z8_filmsim_cfg_needs_grain(cfg) && ctx->zf_maps_loaded;
    if (!need_grain) {
        atomic_store_explicit(&ctx->gtile_ready, 0u, memory_order_release);
        return 0;
    }

    uint32_t gkey = ((uint32_t)cfg->zf_intensity) |
                    ((uint32_t)cfg->zf_size << 4) |
                    ((uint32_t)cfg->zf_blur_mode << 8);
    uint32_t zepoch = ctx->zf_epoch;
    if (!atomic_load_explicit(&ctx->gtile_ready, memory_order_acquire) ||
        ctx->gtile_baked_key != gkey || ctx->gtile_baked_epoch != zepoch) {
        /* Dekker close: wait until CPU0 finishes any current GTILE borrow before overwriting */
        atomic_store_explicit(&ctx->gtile_ready, 0u, memory_order_seq_cst);
        if (atomic_load_explicit(&ctx->gtile_readers, memory_order_seq_cst) == 0u) {
            int8_t *gtile = (int8_t *)(void *)(ctx->arena_blk.cpu + FS_ARENA_GTILE_OFF);
            uint8_t *ws = ctx->arena_blk.cpu + FS_ARENA_LWS_OFF;
            int brc = z8_fs_stage_grain_live_bake(cfg->zf_intensity, cfg->zf_size,
                                                  cfg->zf_blur_mode,
                                                  (const void *)ctx->zf_maps_box.storage,
                                                  FILMSIM_LIVE_GRAIN_SEED,
                                                  gtile, FS_ARENA_GTILE_SIZE,
                                                  ws, FS_ARENA_LWS_SIZE);
            if (brc == Z8_FS_INT_OK) {
                ctx->gtile_baked_key = gkey;
                ctx->gtile_baked_epoch = zepoch;
                atomic_store_explicit(&ctx->gtile_ready, 1u, memory_order_release);
            }
            return 1;
        }
    }
    return 0;
}

/* Lock-free CAS: claim PENDING_RAW slot -> CPU2_GRADING. Returns 0..2 or -1. */
static int filmsim_tb_claim_pending(struct filmsim_runtime_context *ctx)
{
    uint32_t cur = atomic_load_explicit(&ctx->lv_tb_state, memory_order_acquire);
    for (;;) {
        int found = -1;
        for (unsigned s = 0u; s < 3u; ++s) {
            if (fs_tb_get(cur, s) == FS_TB_PENDING_RAW) {
                found = (int)s;
                break;
            }
        }
        if (found < 0) {
            return -1;
        }
        uint32_t next = fs_tb_set(cur, (unsigned)found, FS_TB_CPU2_GRADING);
        if (atomic_compare_exchange_weak_explicit(&ctx->lv_tb_state, &cur, next,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return found;
        }
    }
}

/* Lock-free CAS: finish grading slot `s` (publish > 0 -> GRADED_READY demoting any older
 * GRADED_READY->FREE; publish == 0 -> FREE; publish < 0 -> QUARANTINED for hung DSP DMA). */
static void filmsim_tb_finish_grading(struct filmsim_runtime_context *ctx,
                                      unsigned s, int publish)
{
    uint32_t cur = atomic_load_explicit(&ctx->lv_tb_state, memory_order_acquire);
    for (;;) {
        if (fs_tb_get(cur, s) != FS_TB_CPU2_GRADING) {
            return;
        }
        uint32_t next = cur;
        if (publish > 0) {
            for (unsigned k = 0u; k < 3u; ++k) {
                if (k != s && fs_tb_get(next, k) == FS_TB_GRADED_READY) {
                    next = fs_tb_set(next, k, FS_TB_FREE);
                }
            }
            next = fs_tb_set(next, s, FS_TB_GRADED_READY);
        } else if (publish < 0) {
            next = fs_tb_set(next, s, FS_TB_QUARANTINED);
        } else {
            next = fs_tb_set(next, s, FS_TB_FREE);
        }
        if (atomic_compare_exchange_weak_explicit(&ctx->lv_tb_state, &cur, next,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return;
        }
    }
}

/* Returns 1 if it processed a frame or performed meaningful state work, 0 if idle. */
static int filmsim_live_worker_step(struct filmsim_runtime_context *ctx)
{
    uint32_t live_lut_gen = 0u;
    int want_lut = rt_live_lut_is_active(&live_lut_gen);
    if (atomic_load_explicit(&ctx->stopped, memory_order_acquire) ||
        (atomic_load_explicit(&ctx->dormant, memory_order_acquire) && !want_lut) ||
        atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire)) {
        return 0;
    }
    if (!filmsim_reader_enter(ctx)) {
        return 0;
    }

    Z8FilmSimConfig cfg;
    uint32_t active = 0u, ver = 0u;
    filmsim_snapshot_cfg(ctx, &cfg, &active, &ver);

    int want_fs_live = (active && cfg.live_link == Z8_LIVE_LINK_ON && !z8_filmsim_cfg_is_dormant(&cfg));
    if ((!want_lut && !want_fs_live) ||
        !ctx->live_ws_ready || !ctx->arena_blk.cpu ||
        ctx->arena_blk.actual < FS_ARENA_GRAIN_LIVE_BYTES) {
        filmsim_reader_leave(ctx);
        return 0;
    }

    /* Always keep 512x256 GTILE freshly baked (both Mode 1 GRAIN_ONLY and Mode 2 HDF+Grain!). */
    int did_work = 0;
    if (want_fs_live) {
        did_work |= filmsim_live_bake_locked(ctx, &cfg);
    }

    /* Mode 2 NV12 triple-buffer grading requires full 3-slot arena (Stage 0 LUT / Stage 1 HDF). */
    if (ctx->arena_blk.actual < FS_ARENA_FULL_BYTES) {
        filmsim_reader_leave(ctx);
        return did_work;
    }

    /* Claim any PENDING_RAW NV12 slot for in-place Stage 0 LUT -> Stage 1 Ricoh HDF grading. */
    int slot = filmsim_tb_claim_pending(ctx);
    if (slot < 0) {
        filmsim_reader_leave(ctx);
        return did_work;
    }

    unsigned uslot = (unsigned)slot;
    uint32_t w = ctx->lv_slot_w[uslot];
    uint32_t h = ctx->lv_slot_h[uslot];
    uint32_t uv_off = ctx->lv_slot_uv_off[uslot];
    size_t need_bytes = (size_t)uv_off + (size_t)w * (h >> 1);

    if (w < 4u || h < 2u || w > FS_LIVE_MAX_W || h > FS_LIVE_MAX_H ||
        (w & 1u) || (h & 1u) || need_bytes > FS_ARENA_LFRAME_SIZE) {
        filmsim_tb_finish_grading(ctx, uslot, 0);
        filmsim_reader_leave(ctx);
        atomic_fetch_add_explicit(&ctx->live_skipped, 1u, memory_order_relaxed);
        return 1;
    }

    uint32_t slot_off = fs_arena_lbuf_off(uslot);
    uint8_t *slot_base = ctx->arena_blk.cpu + slot_off;
    uint8_t *y_ptr = slot_base;
    uint8_t *c_ptr = slot_base + uv_off;
    uint64_t y_dma = ctx->arena_blk.dma ? (ctx->arena_blk.dma + slot_off) : 0u;
    uint64_t c_dma = ctx->arena_blk.dma ? (ctx->arena_blk.dma + slot_off + uv_off) : 0u;
    uint8_t *ws = ctx->arena_blk.cpu + FS_ARENA_LWS_OFF;

    ctx->lv_polls = 0u;
    ctx->lv_slice_start_us = z8_fs_plat_time_us();
    uint32_t stages = 0u;
    int rc = Z8_FS_BYPASS;
    int hung_dma = 0;
    uint32_t applied_lut_gen = 0u;
    int lcd_share_ok = 1;

    /* V71.7-r4 Stage 0: Apply active .cube LUT (borrowing Layer 0's 4 MiB cache).
     * Pass use_smp=2 (try-SMP without clobbering Layer 0's FX_Z8_HB_REAR_STEP heartbeat)
     * whenever no Still JPEG is in flight: non-blocking atomic CAS on FX_Z8_LUT_HELPER_REAR
     * cuts CPU2 Live LUT load ~50% when helper is free and falls back to 1-core instantaneously
     * when Layer 0 or Still JPEG owns it! */
#if FS_HAS_V71R7
    if (want_lut) {
        uint32_t cgen = 0u;
        int live_use_smp = (fx_z8_lut_frame_count(v71r7_io_gate()) == 0u) ? 2 : 0;
        uint32_t *baked_528k = v71r7_layer0_baked_acquire(&cgen);
        int lrc;
        int borrowed = 0;
        if (baked_528k) {
            borrowed = 1;
            lrc = v71r7_lut_apply_nv12_baked(y_ptr, c_ptr, w, h, w, w, baked_528k, &cgen, live_use_smp);
            v71r7_layer0_cache_release();
        } else {
            uint32_t *cache_4mib = v71r7_layer0_cache_acquire(&cgen);
            if (cache_4mib) {
                borrowed = 1;
            }
            lrc = v71r7_lut_apply_nv12(y_ptr, c_ptr, w, h, w, w, cache_4mib, &cgen, live_use_smp);
            if (cache_4mib) {
                v71r7_layer0_cache_release();
            }
        }
        if (lrc == -2) {
            hung_dma = 1;
            rc = Z8_FS_CANCELLED_DIRTY;
            lcd_share_ok = 0;
        } else if (lrc > 0) {
            stages |= Z8_FS_STAGE_LUT;
            rc = Z8_FS_OK;
            uint32_t gen_after = 0u;
            if (rt_live_lut_is_active(&gen_after) && gen_after == live_lut_gen &&
                live_lut_gen != 0u && (!borrowed || cgen == live_lut_gen)) {
                applied_lut_gen = live_lut_gen;
            } else {
                lcd_share_ok = 0;
            }
        }
    }
#endif

    /* V71.6/V71.7 Stage 1: Live Mode 2 NV12 HDF prefers XM6 Core 0 Slot 41
     * (MODE_LIVE_256_YC, 35ms bounded timeout, pre-clean + post-inv); instant fallback
     * to NEON-sentinel-gated ARM NEON if hardware absent (QEMU), Core 0 busy, or IOMMU OOB. */
    if (!hung_dma && want_fs_live && z8_filmsim_cfg_needs_hdf(&cfg)) {
        int xrc = z8_xm6_live_hdf_nv12(&ctx->xm6, cfg.xm6_mode,
                                       y_ptr, c_ptr, y_dma, c_dma,
                                       w, h, w, w, cfg.hdf_level);
        if (xrc == Z8_XM6_LIVE_OK) {
            rc = Z8_FS_OK;
            stages |= Z8_FS_STAGE_HDF;
        } else if (xrc == Z8_XM6_LIVE_HUNG_DMA) {
            hung_dma = 1;
            rc = Z8_FS_CANCELLED_DIRTY;
        } else if (xrc == Z8_XM6_LIVE_SKIP_DIRTY) {
            rc = Z8_FS_CANCELLED_DIRTY;
        } else if (rt_neon_allowed()) {
            uint32_t hstages = 0u;
            int hrc = z8_filmsim_apply_live_nv12(y_ptr, w, c_ptr, w, w, h,
                                                 FILMSIM_LIVE_GRAIN_SEED,
                                                 &cfg,
                                                 ws, FS_ARENA_LWS_SIZE,
                                                 filmsim_live_cancelled, ctx,
                                                 &hstages);
            if (hrc == Z8_FS_OK) {
                rc = Z8_FS_OK;
                stages |= hstages;
            } else if (!stages) {
                rc = hrc;
            }
        }
    }

    if (rc == Z8_FS_OK) {
        uint64_t new_gen = __atomic_add_fetch(&ctx->lv_work_gen, 1u, __ATOMIC_ACQ_REL);
        ctx->lv_slot_cfg_ver[uslot] = ver;
        ctx->lv_slot_gen[uslot] = new_gen;
        ctx->lv_work_cfg_ver = ver;
        filmsim_tb_finish_grading(ctx, uslot, 1);

        /* V71.7-r4 Single-Pass Live->LCD Shared Frame Publication:
         * LBUF contains strictly LUT + HDF (never Zf grain, which CPU0 fuses into Wave5).
         * Single-writer seqlock with full ARM64 release fences + original capture_us (lv_slot_us). */
        if (lcd_share_ok && w == 1024u && (h == 680u || h == 682u)) {
            uint32_t l0_stages = ((stages & Z8_FS_STAGE_LUT) ? FS_L0_STAGE_LUT : 0u) |
                                 ((stages & Z8_FS_STAGE_HDF) ? FS_L0_STAGE_HDF : 0u);
            if (l0_stages != 0u) {
                uint64_t pub_ts = z8_fs_plat_time_us();
                uint64_t cap_ts = ctx->lv_slot_us[uslot] ? ctx->lv_slot_us[uslot] : pub_ts;
                uint32_t s = atomic_load_explicit(&ctx->lcd_shared.seqlock, memory_order_relaxed);
                atomic_store_explicit(&ctx->lcd_shared.seqlock, s + 1u, memory_order_relaxed);
                atomic_thread_fence(memory_order_release);
                uint32_t next_seq = ctx->lcd_shared.seq + 1u;
                if (next_seq == 0u) {
                    next_seq = 1u;
                }
                ctx->lcd_shared.seq = next_seq;
                ctx->lcd_shared.capture_us = cap_ts;
                ctx->lcd_shared.publish_us = pub_ts;
                ctx->lcd_shared.slot_gen = new_gen;
                ctx->lcd_shared.slot = uslot;
                ctx->lcd_shared.w = w;
                ctx->lcd_shared.h = h;
                ctx->lcd_shared.stride = w;
                ctx->lcd_shared.uv_off = uv_off;
                ctx->lcd_shared.lut_gen = (l0_stages & FS_L0_STAGE_LUT) ? applied_lut_gen : 0u;
                ctx->lcd_shared.hdf_level = (l0_stages & FS_L0_STAGE_HDF) ? cfg.hdf_level : 0u;
                ctx->lcd_shared.stages_applied = l0_stages;
                ctx->lcd_shared.valid = 1u;
                atomic_store_explicit(&ctx->lcd_shared.seqlock, s + 2u, memory_order_release);
                if (!atomic_load_explicit(&ctx->stopped, memory_order_acquire) &&
                    !atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire) &&
                    !atomic_load_explicit(&ctx->lv_cancel, memory_order_acquire)) {
                    atomic_store_explicit(&ctx->lcd_shared_valid, 1u, memory_order_release);
                }
            }
        }
        atomic_fetch_add_explicit(&ctx->live_applied, 1u, memory_order_relaxed);
        atomic_store_explicit(&ctx->last_live_stages, stages, memory_order_relaxed);
    } else {
        filmsim_tb_finish_grading(ctx, uslot, hung_dma ? -1 : 0);
        atomic_fetch_add_explicit(&ctx->live_skipped, 1u, memory_order_relaxed);
    }
    filmsim_reader_leave(ctx);
    return 1;
}

static void filmsim_live_worker_entry(void *arg)
{
    struct filmsim_runtime_context *ctx = (struct filmsim_runtime_context *)arg;
    rt_neon_probe_once();
    for (;;) {
        int did_work = filmsim_live_worker_step(ctx);
        if (did_work) {
            la_native_yield();
        } else {
            int quiet = atomic_load_explicit(&ctx->stopped, memory_order_acquire) ||
                        atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire) ||
                        !ctx->live_ws_ready ||
                        (atomic_load_explicit(&ctx->dormant, memory_order_acquire) && !rt_lut_is_active());
            FS_SLEEP_MS(quiet ? 20u : 2u);
        }
    }
}

#if FS_HAS_V71R7
/* --- V71.7 Display Layer 0 WYSIWYG Provider Callbacks (Stage 1 HDF + Stage 2 Grain) --- */

static int filmsim_wysiwyg_query_state(void *opaque, V71r7WysiwygState *out)
{
    struct filmsim_runtime_context *ctx = (struct filmsim_runtime_context *)opaque;
    if (!ctx || !out ||
        atomic_load_explicit(&ctx->stopped, memory_order_acquire) ||
        atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire)) {
        return 0;
    }

    Z8FilmSimConfig cfg;
    uint32_t active = 0u, ver = 0u;
    filmsim_snapshot_cfg(ctx, &cfg, &active, &ver);

    if (ctx->base_cfg_valid && ctx->base_cfg.apply_mode == Z8_FS_APPLY_PER_NP3) {
        uint32_t sel = 0u, src = Z8_NP3_SRC_NONE, bound = 0u;
        filmsim_resolve_effective_cfg(ctx, &ctx->base_cfg, &cfg, &sel, &src, &bound);
        active = (bound && !z8_filmsim_cfg_is_dormant(&cfg)) ? 1u : 0u;
    }

    if (cfg.wysiwyg_preview != Z8_WYSIWYG_ON) {
        return 0;
    }
    uint8_t hdf = (active && z8_filmsim_cfg_needs_hdf(&cfg)) ? cfg.hdf_level : 0u;
    uint8_t grain = (active && z8_filmsim_cfg_needs_grain(&cfg) && ctx->zf_maps_loaded)
                        ? cfg.zf_intensity : 0u;
    if (!hdf && !grain) {
        return 0;
    }

    out->active = 1u;
    out->hdf_level = hdf;
    out->zf_intensity = grain;
    out->xm6_mode = cfg.xm6_mode;
    out->monochrome = 0u;
    out->cfg_version = ver;
    return 1;
}

static int filmsim_wysiwyg_is_still_capturing(void *opaque)
{
    struct filmsim_runtime_context *ctx = (struct filmsim_runtime_context *)opaque;
    if (!ctx) {
        return 0;
    }
    return z8_nfsp_class24_busy(&ctx->photo) || z8_nfsp_class24_busy(&ctx->photo_slot1);
}

static int filmsim_wysiwyg_apply_hdf_nv12(void *opaque,
                                          uint8_t *y_cpu, uint8_t *uv_cpu,
                                          uintptr_t y_dma, uintptr_t uv_dma,
                                          uint32_t width, uint32_t height,
                                          uint32_t y_stride, uint32_t uv_stride,
                                          uint8_t hdf_level, uint8_t xm6_mode,
                                          uint8_t monochrome, int is_evf,
                                          uint8_t *scratch_512k, uint32_t scratch_bytes,
                                          int (*cancelled)(void *), void *cancel_ctx)
{
    struct filmsim_runtime_context *ctx = (struct filmsim_runtime_context *)opaque;
    (void)monochrome;
    if (!ctx || !y_cpu || !uv_cpu || hdf_level == 0u) {
        return 1;
    }

    /* V71.7-r5: Both LCD (1024x682 LIVE_256_YC) and EVF (1280x852 LIVE_EVF_256_YC)
     * run on XM6 Core 0 Slot 41 whenever the bank lies inside a single 512 MiB window,
     * freeing CPU2 completely while maintaining 0-diff parity with ARM NEON fallback. */
    (void)is_evf;
    if (y_dma != 0u && uv_dma != 0u && xm6_mode == Z8_XM6_MODE_AUTO) {
        int xrc = z8_xm6_preview_hdf_nv12(&ctx->xm6, xm6_mode,
                                          y_cpu, uv_cpu,
                                          (uint64_t)y_dma, (uint64_t)uv_dma,
                                          width, height, y_stride, uv_stride,
                                          hdf_level);
        if (xrc == Z8_XM6_LIVE_OK) {
            return 0;
        }
        if (xrc == Z8_XM6_LIVE_HUNG_DMA) {
            return -2;
        }
        if (xrc == Z8_XM6_LIVE_SKIP_DIRTY) {
            return -1;
        }
    }

    /* EVF or XM6 fallback: NEON-sentinel-gated ARM NEON HDF using Layer 0's 512 KiB scratch */
    if (!rt_neon_allowed() || !scratch_512k || scratch_bytes < Z8_FILMSIM_WORKSPACE_BYTES) {
        return 1;
    }
    Z8FilmSimConfig tmp;
    z8_filmsim_cfg_default(&tmp);
    tmp.hdf_level = hdf_level;
    tmp.live_link = Z8_LIVE_LINK_ON;
    z8_filmsim_cfg_finalize(&tmp, 1u);

    uint32_t stages = 0u;
    int rc = z8_filmsim_apply_live_nv12(y_cpu, y_stride, uv_cpu, uv_stride,
                                        width, height, FILMSIM_LIVE_GRAIN_SEED,
                                        &tmp, scratch_512k, scratch_bytes,
                                        cancelled, cancel_ctx, &stages);
    if (rc == Z8_FS_OK) {
        return 0;
    }
    if (rc == Z8_FS_BYPASS) {
        return 1;
    }
    if (rc == Z8_FS_HUNG_SMP) {
        return -2;
    }
    return -1;
}

typedef struct {
    uint8_t      *y_cpu;
    const int8_t *gtile;
    uint32_t      width;
    uint32_t      y_stride;
} FsGrainStripeJob;

static void fs_grain_stripe_cb(void *opaque, uint32_t row_begin, uint32_t row_end, unsigned worker_idx)
{
    (void)worker_idx;
    const FsGrainStripeJob *j = (const FsGrainStripeJob *)opaque;
    z8_zf_grain_live_apply_tile_y_rows_clean(j->y_cpu, j->y_stride, j->width,
                                             row_begin, row_end, j->gtile, 1);
}

static int filmsim_apply_grain_smp_or_sync(uint8_t *y_cpu, uint32_t y_stride,
                                           uint32_t width, uint32_t height,
                                           const int8_t *gtile)
{
    if (height >= 512u && fx_z8_lut_frame_count(v71r7_io_gate()) == 0u) {
        (void)v71r7_smp_ensure_started();
        FsGrainStripeJob job = { y_cpu, gtile, width, y_stride };
        int undrained = v71r7_smp_run_stripes_copy(FX_Z8_LUT_HELPER_REAR, height, 32u,
                                                   fs_grain_stripe_cb, &job, sizeof(job),
                                                   FX_Z8_HB_REAR_STEP);
        return (undrained != 0) ? -2 : 0;
    }
    z8_zf_grain_live_apply_tile_y_rows_clean(y_cpu, y_stride, width, 0u, height, gtile, 1);
    return 0;
}

static int filmsim_wysiwyg_apply_grain_nv12(void *opaque,
                                            uint8_t *y_cpu,
                                            uint32_t width, uint32_t height,
                                            uint32_t y_stride)
{
    struct filmsim_runtime_context *ctx = (struct filmsim_runtime_context *)opaque;
    if (!ctx || !y_cpu || !rt_neon_allowed()) {
        return 1;
    }
    if (!filmsim_reader_enter(ctx)) {
        return 1;
    }

    Z8FilmSimConfig cfg;
    uint32_t active = 0u;
    filmsim_snapshot_cfg(ctx, &cfg, &active, 0);
    if (ctx->base_cfg_valid && ctx->base_cfg.apply_mode == Z8_FS_APPLY_PER_NP3) {
        uint32_t sel = 0u, src = Z8_NP3_SRC_NONE, bound = 0u;
        filmsim_resolve_effective_cfg(ctx, &ctx->base_cfg, &cfg, &sel, &src, &bound);
        active = (bound && !z8_filmsim_cfg_is_dormant(&cfg)) ? 1u : 0u;
    }

    if (!active || !z8_filmsim_cfg_needs_grain(&cfg) || !ctx->zf_maps_loaded) {
        filmsim_reader_leave(ctx);
        return 1;
    }

    /* Heap-cached 128 KiB WYSIWYG GTILE (0 B Class 0x24, permanent lifetime so SMP
     * helper quarantine can never UAF arena_blk on pool_suspend). When Live Photo's
     * arena tile is already baked for the same key, copy it once instead of re-baking. */
    uint32_t gkey = ((uint32_t)cfg.zf_intensity) |
                    ((uint32_t)cfg.zf_size << 4) |
                    ((uint32_t)cfg.zf_blur_mode << 8);
    uint32_t zepoch = ctx->zf_epoch;
    if (!ctx->wy_gtile_buf) {
        ctx->wy_gtile_buf = (int8_t *)FS_ALLOC(FS_ARENA_GTILE_SIZE);
    }
    if (ctx->wy_gtile_buf &&
        (!ctx->wy_gtile_ready || ctx->wy_gtile_key != gkey || ctx->wy_gtile_epoch != zepoch)) {
        int copied = 0;
        if (ctx->arena_blk.cpu && atomic_load_explicit(&ctx->gtile_ready, memory_order_acquire)) {
            atomic_fetch_add_explicit(&ctx->gtile_readers, 1u, memory_order_seq_cst);
            if (atomic_load_explicit(&ctx->gtile_ready, memory_order_seq_cst)) {
                memcpy(ctx->wy_gtile_buf,
                       ctx->arena_blk.cpu + FS_ARENA_GTILE_OFF,
                       FS_ARENA_GTILE_SIZE);
                copied = 1;
            }
            atomic_fetch_sub_explicit(&ctx->gtile_readers, 1u, memory_order_release);
        }
        if (copied) {
            ctx->wy_gtile_key = gkey;
            ctx->wy_gtile_epoch = zepoch;
            ctx->wy_gtile_ready = 1u;
        } else {
            void *tmp_ws = FS_ALLOC(Z8_ZF_GRAIN_WORKSPACE_BYTES);
            if (tmp_ws) {
                int brc = z8_fs_stage_grain_live_bake(cfg.zf_intensity, cfg.zf_size,
                                                      cfg.zf_blur_mode,
                                                      (const void *)ctx->zf_maps_box.storage,
                                                      FILMSIM_LIVE_GRAIN_SEED,
                                                      ctx->wy_gtile_buf, FS_ARENA_GTILE_SIZE,
                                                      tmp_ws, Z8_ZF_GRAIN_WORKSPACE_BYTES);
                FS_FREE(tmp_ws);
                if (brc == Z8_FS_INT_OK) {
                    ctx->wy_gtile_key = gkey;
                    ctx->wy_gtile_epoch = zepoch;
                    ctx->wy_gtile_ready = 1u;
                }
            }
        }
    }

    int ret = 1;
    if (ctx->wy_gtile_buf && ctx->wy_gtile_ready) {
        ret = filmsim_apply_grain_smp_or_sync(y_cpu, y_stride, width, height, ctx->wy_gtile_buf);
    }
    filmsim_reader_leave(ctx);
    return ret;
}
#endif /* FS_HAS_V71R7 */

static struct filmsim_runtime_context *filmsim_ensure_context(void)
{
    _Atomic(void *) *slot = live_ui_filmsim_context_slot();
    if (!slot) {
        return 0;
    }
    void *cur = atomic_load_explicit(slot, memory_order_acquire);
    if (cur) {
        struct filmsim_runtime_context *c = (struct filmsim_runtime_context *)cur;
        return (c->magic == FILMSIM_RUNTIME_MAGIC && c->size == sizeof(*c)) ? c : 0;
    }

    struct filmsim_runtime_context *ctx = (struct filmsim_runtime_context *)FS_ALLOC((uint32_t)sizeof(*ctx));
    if (!ctx) {
        return 0;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->magic = FILMSIM_RUNTIME_MAGIC;
    ctx->size = (uint32_t)sizeof(*ctx);
    atomic_store_explicit(&ctx->dormant, 1u, memory_order_relaxed);
    z8_filmsim_cfg_default(&ctx->cfg);
    z8_filmsim_cfg_default(&ctx->base_cfg);
    z8_np3_table_init_empty(&ctx->recipe_table);
    z8_xm6_bridge_init(&ctx->xm6, 0);

    struct z8_nfsp_ops ops;
    z8_nfsp_v66_ops(&ops);
    ops.acquire_assets = filmsim_photo_acquire_assets;
    ops.release_assets = filmsim_photo_release_assets;
    ops.should_defer = filmsim_photo_should_defer;
    ops.context = ctx;
    z8_nfsp_init(&ctx->photo, &ops);
    z8_nfsp_init(&ctx->photo_slot1, &ops);

    ctx->photo_stack = FS_ALLOC(32768u + 16u);
    ctx->live_stack = FS_ALLOC(32768u + 16u);

    void *expected = 0;
    if (!atomic_compare_exchange_strong_explicit(slot, &expected, ctx,
                                                 memory_order_release,
                                                 memory_order_acquire)) {
        if (ctx->live_stack) {
            FS_FREE(ctx->live_stack);
        }
        if (ctx->photo_stack) {
            FS_FREE(ctx->photo_stack);
        }
        FS_FREE(ctx);
        struct filmsim_runtime_context *winner = (struct filmsim_runtime_context *)expected;
        return (winner && winner->magic == FILMSIM_RUNTIME_MAGIC && winner->size == sizeof(*winner))
                   ? winner : 0;
    }

    /* V71.3 Defect 3D: Photo worker on CPU2 prio 30 (matches pb_worker & filmsim_live_worker
     * so entering Playback never starves 45MP photo processing into 16.7 s; remains strictly
     * below real-time AF/CV/audio prio 2..28, defers to Live post-roll via should_defer, and
     * sleeps 2 ms when idle so Live 30fps sees 0% background contention). */
    if (ctx->photo_stack &&
        la_native_cpu2_start(&ctx->photo_task, filmsim_photo_worker_entry, ctx,
                             (void *)(((uintptr_t)ctx->photo_stack + 15u) & ~(uintptr_t)15u),
                             32768u, 30u)) {
        atomic_store_explicit(&ctx->photo_worker_ready, 1u, memory_order_release);
    } else {
        ctx->error |= 1u;
    }

    /* Async Live worker on CPU2 prio 30 (strictly below real-time AF/CV/audio prio 2..28,
     * preempting bulk background CVSTLV01..33/CVSTST00..33/SUBSEL & photo_worker at prio 32) */
    if (ctx->live_stack &&
        la_native_cpu2_start(&ctx->live_task, filmsim_live_worker_entry, ctx,
                             (void *)(((uintptr_t)ctx->live_stack + 15u) & ~(uintptr_t)15u),
                             32768u, 30u)) {
        atomic_store_explicit(&ctx->live_worker_ready, 1u, memory_order_release);
    } else {
        ctx->error |= 2u;
    }

#if FS_HAS_V71R7
    /* Bind XM6 bridge for Stage 0 1D separable Op12 & register Layer 0 WYSIWYG provider ops */
    v71r7_lut_bind_xm6(&ctx->xm6, ctx->cfg.xm6_mode);
    (void)v71r7_layer0_init();
    V71r7WysiwygOps wops = {
        .ctx                = ctx,
        .query_state        = filmsim_wysiwyg_query_state,
        .is_still_capturing = filmsim_wysiwyg_is_still_capturing,
        .apply_hdf_nv12     = filmsim_wysiwyg_apply_hdf_nv12,
        .apply_grain_nv12   = filmsim_wysiwyg_apply_grain_nv12
    };
    (void)v71r7_layer0_set_wysiwyg_ops(&wops);
#endif
    return ctx;
}

void filmsim_runtime_ui_poll(void)
{
    int loaded = filmsim_is_module_loaded();
    int lut_on = rt_lut_is_active();
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();

    if (!loaded) {
        if (!lut_on) {
            if (ctx) {
                Z8FilmSimConfig def;
                z8_filmsim_cfg_default(&def);
                ctx->base_cfg = def;
                ctx->base_cfg_valid = 0u;
                atomic_store_explicit(&ctx->active_recipe_src, Z8_NP3_SRC_NONE, memory_order_release);
                atomic_store_explicit(&ctx->active_recipe_bound, 0u, memory_order_release);
                filmsim_publish_cfg(ctx, &def, 0u, 0u, ctx->bus_corr);
                z8_nfsp_cancel(&ctx->photo);
                z8_nfsp_cancel(&ctx->photo_slot1);
                filmsim_sync_assets(ctx, &def, 1u);
            }
            return; /* Strict Dormant Bypass: 0 bytes allocated */
        }
        /* Native .cube LUT is active even though filmsim.wlm is unloaded: ensure context
         * so Still JPEG + XM6 Op12 + Live Photo Stage 0 LUT run cleanly! */
        if (!ctx) {
            ctx = filmsim_ensure_context();
            if (!ctx) {
                return;
            }
        }
        if (atomic_load_explicit(&ctx->stopped, memory_order_acquire)) {
            return;
        }
        Z8FilmSimConfig def;
        z8_filmsim_cfg_default(&def);
        if (atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire)) {
            filmsim_sync_assets(ctx, &def, 1u);
            return;
        }
        ctx->base_cfg = def;
        ctx->base_cfg_valid = 0u;
        atomic_store_explicit(&ctx->active_recipe_src, Z8_NP3_SRC_NONE, memory_order_release);
        atomic_store_explicit(&ctx->active_recipe_bound, 0u, memory_order_release);
        filmsim_publish_cfg(ctx, &def, 0u, 0u, ctx->bus_corr);
        filmsim_sync_assets(ctx, &def, 1u);
        return;
    }

    Z8FilmSimConfig incoming;
    z8_filmsim_cfg_default(&incoming);
    uint64_t corr = ctx ? ctx->bus_corr : 0u;
    int rc = filmsim_bus_read_latest(&incoming, &corr);

    if (rc < 0) {
        if (ctx) {
            ++ctx->bus_poll_calls;
            ++ctx->bus_lock_skips;
            atomic_store_explicit(&ctx->module_loaded, 1u, memory_order_release);
        }
        return;
    }

    /* Strict Dormant Bypass: allocate 0 bytes in GLOBAL mode while all stages AND LUT are Off.
     * In PER_NP3 mode (apply_mode == 1) or when .cube LUT is active, ensure context. */
    int want_per_np3 = (rc > 0 && incoming.apply_mode == Z8_FS_APPLY_PER_NP3);
    int global_dormant = (rc == 0) || (!want_per_np3 && z8_filmsim_cfg_is_dormant(&incoming));

    if (!ctx && global_dormant && !lut_on) {
        return;
    }

    if (!ctx) {
        ctx = filmsim_ensure_context();
        if (!ctx) {
            return;
        }
    }
    if (atomic_load_explicit(&ctx->stopped, memory_order_acquire)) {
        return;
    }
    if (atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire)) {
        Z8FilmSimConfig def;
        z8_filmsim_cfg_default(&def);
        filmsim_sync_assets(ctx, &def, 1u);
        return;
    }

    ++ctx->bus_poll_calls;
    if (rc > 0) {
        ++ctx->bus_hits;
        int bus_changed = (!ctx->base_cfg_valid || corr != ctx->bus_corr ||
                           memcmp(&ctx->base_cfg, &incoming, sizeof(incoming)) != 0);
        if (bus_changed) {
            z8_xm6_clear_live_soft_backoff(&ctx->xm6);
        }
        ctx->base_cfg = incoming;
        ctx->base_cfg_valid = 1u;

        if (incoming.apply_mode == Z8_FS_APPLY_PER_NP3) {
            filmsim_maybe_load_recipe_table(ctx, corr, bus_changed);
        }

        Z8FilmSimConfig eff;
        uint32_t sel = 0u, src = Z8_NP3_SRC_NONE, bound = 0u;
        filmsim_resolve_effective_cfg(ctx, &ctx->base_cfg, &eff, &sel, &src, &bound);
        atomic_store_explicit(&ctx->active_pc_sel, sel, memory_order_release);
        atomic_store_explicit(&ctx->active_recipe_src, src, memory_order_release);
        atomic_store_explicit(&ctx->active_recipe_bound, bound, memory_order_release);

        int eff_dormant = z8_filmsim_cfg_is_dormant(&eff);
        filmsim_publish_cfg(ctx, &eff, 1u, 1u, corr);
        filmsim_sync_assets(ctx, &eff, (uint32_t)eff_dormant);
    } else if (ctx->base_cfg_valid) {
        /* Preserve IPC-configured base_cfg (Z8_FS_OP_SET) when bus has no newer message */
        if (ctx->base_cfg.apply_mode == Z8_FS_APPLY_PER_NP3) {
            filmsim_maybe_load_recipe_table(ctx, corr, 0);
        }
        Z8FilmSimConfig eff;
        uint32_t sel = 0u, src = Z8_NP3_SRC_NONE, bound = 0u;
        filmsim_resolve_effective_cfg(ctx, &ctx->base_cfg, &eff, &sel, &src, &bound);
        atomic_store_explicit(&ctx->active_pc_sel, sel, memory_order_release);
        atomic_store_explicit(&ctx->active_recipe_src, src, memory_order_release);
        atomic_store_explicit(&ctx->active_recipe_bound, bound, memory_order_release);

        int eff_dormant = z8_filmsim_cfg_is_dormant(&eff);
        filmsim_publish_cfg(ctx, &eff, 1u, 1u, corr);
        filmsim_sync_assets(ctx, &eff, (uint32_t)eff_dormant);
    } else {
        Z8FilmSimConfig def;
        z8_filmsim_cfg_default(&def);
        atomic_store_explicit(&ctx->active_recipe_src, Z8_NP3_SRC_NONE, memory_order_release);
        atomic_store_explicit(&ctx->active_recipe_bound, 0u, memory_order_release);
        filmsim_publish_cfg(ctx, &def, 0u, 1u, corr);
        if (!lut_on) {
            z8_nfsp_cancel(&ctx->photo);
            z8_nfsp_cancel(&ctx->photo_slot1);
        }
        filmsim_sync_assets(ctx, &def, 1u);
    }
}

void filmsim_runtime_on_stop(void)
{
    rt_layer0_lifecycle_stop();
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx) {
        return;
    }
    atomic_store_explicit(&ctx->stopped, 1u, memory_order_release);
    atomic_store_explicit(&ctx->cfg_active, 0u, memory_order_release);
    atomic_store_explicit(&ctx->dormant, 1u, memory_order_release);
    atomic_store_explicit(&ctx->lv_cancel, 1u, memory_order_release);
    filmsim_lcd_shared_invalidate(ctx);
    z8_xm6_set_pool_suspended(&ctx->xm6, 1u);
    z8_nfsp_cancel(&ctx->photo);
    z8_nfsp_cancel(&ctx->photo_slot1);
    z8_xm6_release_ext_ws(&ctx->xm6);
}

void filmsim_runtime_on_card_change(void)
{
    rt_photo_menu_on_card_change();
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx) {
        return;
    }
    atomic_fetch_add_explicit(&ctx->card_epoch, 1u, memory_order_acq_rel);
    ctx->zf_maps_retry_after_us = 0u;
    ctx->recipe_table_retry_after_us = 0u;
    atomic_store_explicit(&ctx->lv_cancel, 1u, memory_order_release);
    z8_nfsp_cancel(&ctx->photo);
    z8_nfsp_cancel(&ctx->photo_slot1);
}

void filmsim_runtime_pool_suspend(void)
{
    /* Halt Layer 0 display swaps immediately at t=0, drain FilmSim Still/Live (releasing any
     * borrowed Layer 0 4 MiB cache ref), then drain & release Layer 0's 14.55 MiB workspace! */
    rt_layer0_mark_pool_suspended();
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (ctx) {
        atomic_store_explicit(&ctx->pool_suspended, 1u, memory_order_release);
        atomic_store_explicit(&ctx->lv_cancel, 1u, memory_order_release);
        filmsim_lcd_shared_invalidate(ctx);
        z8_xm6_set_pool_suspended(&ctx->xm6, 1u);
        z8_nfsp_cancel(&ctx->photo);
        z8_nfsp_cancel(&ctx->photo_slot1);
        z8_nfsp_reclaim_cancelled(&ctx->photo);
        z8_nfsp_reclaim_cancelled(&ctx->photo_slot1);

        /* V71.5: Drain BOTH photo slots (0 & 1) AND the Live arena_blk while keeping
         * the 28 KiB kernel-heap zf_grain_buf permanently hot in RAM! */
        uint64_t t0 = z8_fs_plat_time_us();
        int arena_done = 0;
        for (unsigned tries = 0u; ; ++tries) {
            z8_nfsp_reclaim_cancelled(&ctx->photo);
            z8_nfsp_reclaim_cancelled(&ctx->photo_slot1);
            int photo_busy = z8_nfsp_class24_busy(&ctx->photo) ||
                             z8_nfsp_class24_busy(&ctx->photo_slot1);

            if (!arena_done && filmsim_asset_writer_try_lock(ctx)) {
                if (ctx->arena_blk.handle) {
                    filmsim_release_arena(ctx);
                }
                filmsim_asset_writer_unlock(ctx);
                arena_done = 1;
            }
            if (!photo_busy && arena_done) {
                break;
            }
            uint64_t now = z8_fs_plat_time_us();
            if ((t0 && now >= t0 && now - t0 >= UINT64_C(1500000)) || tries >= 750u) {
                break;
            }
            FS_SLEEP_MS(2u);
        }
        /* Release dedicated 512 KiB Slot 41 Class 0x24 ext_ws (strictly guarded against UAF) */
        z8_xm6_release_ext_ws(&ctx->xm6);
    }
    rt_layer0_pool_suspend();
}

void filmsim_runtime_pool_resume(void)
{
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (ctx) {
        z8_xm6_set_pool_suspended(&ctx->xm6, 0u);
        atomic_store_explicit(&ctx->pool_suspended, 0u, memory_order_release);
        atomic_store_explicit(&ctx->lv_cancel, 0u, memory_order_release);
    }
    rt_layer0_pool_resume();
}

int filmsim_runtime_photo_config(Z8FilmSimConfig *out, uint32_t *shot_seq_out)
{
    if (!out) {
        return 0;
    }
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx || atomic_load_explicit(&ctx->stopped, memory_order_acquire) ||
        atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire)) {
        return 0;
    }
    Z8FilmSimConfig cfg;
    uint32_t active = 0u;
    filmsim_snapshot_cfg(ctx, &cfg, &active, 0);

    /* V71.5 O(1) shutter-time NP3 check: if apply_mode == PER_NP3, read 0xA02983CF8
     * directly right at shutter press so switching Picture Control takes effect with
     * 0 ms polling lag (and unbound NP3 bypasses with 0 overhead!). */
    if (ctx->base_cfg_valid && ctx->base_cfg.apply_mode == Z8_FS_APPLY_PER_NP3) {
        uint32_t sel = 0u, src = Z8_NP3_SRC_NONE, bound = 0u;
        filmsim_resolve_effective_cfg(ctx, &ctx->base_cfg, &cfg, &sel, &src, &bound);
        atomic_store_explicit(&ctx->active_pc_sel, sel, memory_order_release);
        atomic_store_explicit(&ctx->active_recipe_src, src, memory_order_release);
        atomic_store_explicit(&ctx->active_recipe_bound, bound, memory_order_release);
        active = (bound && !z8_filmsim_cfg_is_dormant(&cfg)) ? 1u : 0u;
    }

    if (!active || z8_filmsim_cfg_is_dormant(&cfg)) {
        if (rt_still_lut_is_active()) {
            z8_filmsim_cfg_default(&cfg);
            if (ctx->base_cfg_valid) {
                cfg.xm6_mode = ctx->base_cfg.xm6_mode;
            }
            z8_filmsim_cfg_finalize(&cfg, 1u);
            *out = cfg;
            if (shot_seq_out) {
                *shot_seq_out = atomic_fetch_add_explicit(&ctx->next_shot_seq, 1u, memory_order_relaxed) + 1u;
            }
            return 1;
        }
        if (shot_seq_out) {
            *shot_seq_out = atomic_load_explicit(&ctx->next_shot_seq, memory_order_relaxed);
        }
        return 0;
    }
    *out = cfg;
    if (shot_seq_out) {
        *shot_seq_out = atomic_fetch_add_explicit(&ctx->next_shot_seq, 1u, memory_order_relaxed) + 1u;
    }
    return 1;
}

struct z8_native_filmsim_photo *filmsim_runtime_photo_slot(unsigned slot_idx)
{
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx || !atomic_load_explicit(&ctx->photo_worker_ready, memory_order_acquire)) {
        return 0;
    }
    if (slot_idx == 0u) {
        return &ctx->photo;
    }
    if (slot_idx == 1u) {
        return &ctx->photo_slot1;
    }
    return 0;
}

struct z8_native_filmsim_photo *filmsim_runtime_photo(void)
{
    return filmsim_runtime_photo_slot(0u);
}

/* --- V71 Non-blocking Native-NV12 Triple-Buffer Live API for CPU0 live_stream.c --- */

static const int8_t *filmsim_try_acquire_gtile_locked(struct filmsim_runtime_context *ctx)
{
    if (!atomic_load_explicit(&ctx->gtile_ready, memory_order_acquire)) {
        return 0;
    }
    atomic_fetch_add_explicit(&ctx->gtile_readers, 1u, memory_order_seq_cst);
    if (!atomic_load_explicit(&ctx->gtile_ready, memory_order_seq_cst)) {
        atomic_fetch_sub_explicit(&ctx->gtile_readers, 1u, memory_order_release);
        return 0;
    }
    atomic_store_explicit(&ctx->lv_cpu0_gtile_held, 1u, memory_order_release);
    return (const int8_t *)(const void *)(ctx->arena_blk.cpu + FS_ARENA_GTILE_OFF);
}

int filmsim_runtime_live_mode(void)
{
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx || atomic_load_explicit(&ctx->stopped, memory_order_acquire) ||
        atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire) ||
        !atomic_load_explicit(&ctx->live_worker_ready, memory_order_acquire)) {
        return FILMSIM_LIVE_MODE_OFF;
    }
    if (!ctx->live_ws_ready || !ctx->arena_blk.handle) {
        return FILMSIM_LIVE_MODE_OFF;
    }
    if (rt_live_lut_wanted()) {
        return (ctx->arena_blk.actual >= FS_ARENA_FULL_BYTES)
                   ? FILMSIM_LIVE_MODE_FULL_CPU2
                   : FILMSIM_LIVE_MODE_OFF;
    }
    Z8FilmSimConfig cfg;
    uint32_t active = 0u;
    filmsim_snapshot_cfg(ctx, &cfg, &active, 0);
    if (!active || cfg.live_link == Z8_LIVE_LINK_OFF || z8_filmsim_cfg_is_dormant(&cfg)) {
        return FILMSIM_LIVE_MODE_OFF;
    }
    if (z8_filmsim_cfg_needs_hdf(&cfg)) {
        return (ctx->arena_blk.actual >= FS_ARENA_FULL_BYTES)
                   ? FILMSIM_LIVE_MODE_FULL_CPU2
                   : FILMSIM_LIVE_MODE_OFF;
    }
    if (z8_filmsim_cfg_needs_grain(&cfg) && ctx->zf_maps_loaded &&
        ctx->arena_blk.actual >= FS_ARENA_GRAIN_LIVE_BYTES) {
        return FILMSIM_LIVE_MODE_GRAIN_ONLY;
    }
    return FILMSIM_LIVE_MODE_OFF;
}

int filmsim_runtime_live_active(void)
{
    return filmsim_runtime_live_mode() != FILMSIM_LIVE_MODE_OFF;
}

uint32_t filmsim_runtime_live_is_busy(void)
{
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx || atomic_load_explicit(&ctx->stopped, memory_order_acquire) ||
        atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire) ||
        (atomic_load_explicit(&ctx->dormant, memory_order_acquire) && !rt_lut_is_active()) ||
        atomic_load_explicit(&ctx->lv_cancel, memory_order_acquire)) {
        return 0u;
    }
    uint32_t st = atomic_load_explicit(&ctx->lv_tb_state, memory_order_acquire);
    for (unsigned s = 0u; s < 3u; ++s) {
        uint32_t v = fs_tb_get(st, s);
        if (v == FS_TB_PENDING_RAW || v == FS_TB_CPU2_GRADING || v == FS_TB_GRADED_READY) {
            return 1u;
        }
    }
    return 0u;
}

int filmsim_runtime_live_begin_submit_nv12(unsigned width, unsigned height,
                                           uint8_t **y_out, uint8_t **uv_out,
                                           unsigned *y_stride_out, unsigned *uv_stride_out)
{
    if (!y_out || !uv_out || width < 4u || height < 2u ||
        width > FS_LIVE_MAX_W || height > FS_LIVE_MAX_H || ((width | height) & 1u)) {
        return -1;
    }
    size_t uv_off = ((size_t)width * height + 63u) & ~(size_t)63u;
    size_t need = uv_off + (size_t)width * (height >> 1);
    if (need > FS_ARENA_LFRAME_SIZE) {
        return -1;
    }
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx || !filmsim_reader_enter(ctx)) {
        return -1;
    }
    if (!ctx->live_ws_ready || !ctx->arena_blk.cpu ||
        ctx->arena_blk.actual < FS_ARENA_FULL_BYTES) {
        filmsim_reader_leave(ctx);
        return -1;
    }

    /* Lock-free O(1) slot claim with Dekker pin exclusion against LCD shared-frame reader:
     * prefer FREE, else supersede PENDING_RAW, else promote GRADED_READY->CPU0_FRONT and
     * recycle old CPU0_FRONT->CPU0_WRITING (never touching any slot pinned by LCD copy!). */
    uint32_t cur = atomic_load_explicit(&ctx->lv_tb_state, memory_order_acquire);
    int chosen = -1;
    for (;;) {
        int s_free = -1, s_pend = -1, s_ready = -1, s_front = -1;
        for (unsigned s = 0u; s < 3u; ++s) {
            uint32_t st = fs_tb_get(cur, s);
            uint32_t pinned = atomic_load_explicit(&ctx->lv_slot_lcd_pins[s], memory_order_seq_cst);
            if (st == FS_TB_FREE && !pinned && s_free < 0) {
                s_free = (int)s;
            } else if (st == FS_TB_PENDING_RAW && !pinned && s_pend < 0) {
                s_pend = (int)s;
            } else if (st == FS_TB_GRADED_READY && s_ready < 0) {
                s_ready = (int)s;
            } else if (st == FS_TB_CPU0_FRONT && !pinned && s_front < 0) {
                s_front = (int)s;
            }
        }
        uint32_t next = cur;
        uint32_t prev_chosen_st = FS_TB_FREE;
        if (s_free >= 0) {
            chosen = s_free;
            prev_chosen_st = FS_TB_FREE;
            next = fs_tb_set(next, (unsigned)chosen, FS_TB_CPU0_WRITING);
        } else if (s_pend >= 0) {
            chosen = s_pend;
            prev_chosen_st = FS_TB_PENDING_RAW;
            next = fs_tb_set(next, (unsigned)chosen, FS_TB_CPU0_WRITING);
        } else if (s_ready >= 0 && s_front >= 0) {
            chosen = s_front;
            prev_chosen_st = FS_TB_CPU0_FRONT;
            next = fs_tb_set(next, (unsigned)s_ready, FS_TB_CPU0_FRONT);
            next = fs_tb_set(next, (unsigned)chosen, FS_TB_CPU0_WRITING);
        } else {
            atomic_fetch_add_explicit(&ctx->lv_busy_skips, 1u, memory_order_relaxed);
            filmsim_reader_leave(ctx);
            return -1;
        }
        if (atomic_compare_exchange_weak_explicit(&ctx->lv_tb_state, &cur, next,
                                                  memory_order_seq_cst,
                                                  memory_order_acquire)) {
            /* Dekker post-check: if an LCD reader pinned `chosen` right before our CAS,
             * safely restore `chosen` to `prev_chosen_st` (or FREE) and skip this tick. */
            if (atomic_load_explicit(&ctx->lv_slot_lcd_pins[(unsigned)chosen],
                                     memory_order_seq_cst) != 0u) {
                uint32_t rcur = atomic_load_explicit(&ctx->lv_tb_state, memory_order_acquire);
                uint32_t restore_st = (prev_chosen_st == FS_TB_PENDING_RAW) ? FS_TB_PENDING_RAW : FS_TB_FREE;
                while (fs_tb_get(rcur, (unsigned)chosen) == FS_TB_CPU0_WRITING) {
                    uint32_t rnext = fs_tb_set(rcur, (unsigned)chosen, restore_st);
                    if (atomic_compare_exchange_weak_explicit(&ctx->lv_tb_state, &rcur, rnext,
                                                              memory_order_acq_rel,
                                                              memory_order_acquire)) {
                        break;
                    }
                }
                atomic_fetch_add_explicit(&ctx->lv_busy_skips, 1u, memory_order_relaxed);
                filmsim_reader_leave(ctx);
                return -1;
            }
            break;
        }
    }

    uint8_t *base = ctx->arena_blk.cpu + fs_arena_lbuf_off((unsigned)chosen);
    *y_out = base;
    *uv_out = base + uv_off;
    if (y_stride_out) {
        *y_stride_out = width;
    }
    if (uv_stride_out) {
        *uv_stride_out = width;
    }
    return chosen;
}

void filmsim_runtime_live_commit_submit_nv12(int slot, unsigned width, unsigned height,
                                             uint64_t source_us, int commit)
{
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx) {
        return;
    }
    if (slot >= 0 && slot < 3) {
        unsigned uslot = (unsigned)slot;
        if (commit) {
            ctx->lv_slot_w[uslot] = width;
            ctx->lv_slot_h[uslot] = height;
            ctx->lv_slot_uv_off[uslot] = (uint32_t)(((size_t)width * height + 63u) & ~(size_t)63u);
            ctx->lv_slot_us[uslot] = source_us;
            ctx->lv_width = width;
            ctx->lv_height = height;
            ctx->lv_source_us = source_us;
            atomic_fetch_add_explicit(&ctx->lv_submitted, 1u, memory_order_relaxed);
        }
        uint32_t cur = atomic_load_explicit(&ctx->lv_tb_state, memory_order_acquire);
        for (;;) {
            if (fs_tb_get(cur, uslot) != FS_TB_CPU0_WRITING) {
                break;
            }
            uint32_t next = cur;
            if (commit) {
                for (unsigned k = 0u; k < 3u; ++k) {
                    if (k != uslot && fs_tb_get(next, k) == FS_TB_PENDING_RAW) {
                        next = fs_tb_set(next, k, FS_TB_FREE);
                    }
                }
                next = fs_tb_set(next, uslot, FS_TB_PENDING_RAW);
            } else {
                next = fs_tb_set(next, uslot, FS_TB_FREE);
            }
            if (atomic_compare_exchange_weak_explicit(&ctx->lv_tb_state, &cur, next,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
                break;
            }
        }
    }
    filmsim_reader_leave(ctx);
}

int filmsim_runtime_live_acquire_nv12(const uint8_t **y_out, const uint8_t **uv_out,
                                      unsigned *w_out, unsigned *h_out,
                                      uint64_t *gen_out, const int8_t **grain_tile_out)
{
    if (y_out) *y_out = 0;
    if (uv_out) *uv_out = 0;
    if (w_out) *w_out = 0u;
    if (h_out) *h_out = 0u;
    if (gen_out) *gen_out = 0u;
    if (grain_tile_out) *grain_tile_out = 0;

    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx || !filmsim_reader_enter(ctx)) {
        return 0;
    }
    if (!ctx->live_ws_ready || !ctx->arena_blk.cpu ||
        ctx->arena_blk.actual < FS_ARENA_FULL_BYTES) {
        filmsim_reader_leave(ctx);
        return 0;
    }

    uint32_t cur = atomic_load_explicit(&ctx->lv_tb_state, memory_order_acquire);
    int front = -1;
    for (;;) {
        int s_ready = -1, s_front = -1;
        for (unsigned s = 0u; s < 3u; ++s) {
            uint32_t st = fs_tb_get(cur, s);
            if (st == FS_TB_GRADED_READY && s_ready < 0) {
                s_ready = (int)s;
            } else if (st == FS_TB_CPU0_FRONT && s_front < 0) {
                s_front = (int)s;
            }
        }
        if (s_ready >= 0) {
            uint32_t next = fs_tb_set(cur, (unsigned)s_ready, FS_TB_CPU0_FRONT);
            if (s_front >= 0 && s_front != s_ready) {
                next = fs_tb_set(next, (unsigned)s_front, FS_TB_FREE);
            }
            if (atomic_compare_exchange_weak_explicit(&ctx->lv_tb_state, &cur, next,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
                front = s_ready;
                break;
            }
        } else if (s_front >= 0) {
            front = s_front;
            break;
        } else {
            filmsim_reader_leave(ctx);
            return 0;
        }
    }

    unsigned ufront = (unsigned)front;
    uint32_t w = ctx->lv_slot_w[ufront];
    uint32_t h = ctx->lv_slot_h[ufront];
    uint32_t uv_off = ctx->lv_slot_uv_off[ufront];
    uint64_t gen = ctx->lv_slot_gen[ufront];
    if (!gen || w < 4u || h < 2u || w > FS_LIVE_MAX_W || h > FS_LIVE_MAX_H) {
        filmsim_reader_leave(ctx);
        return 0;
    }

    ctx->lv_front = ufront;
    ctx->lv_front_valid = 1u;
    ctx->lv_front_us = ctx->lv_slot_us[ufront];
    ctx->lv_front_cfg_ver = ctx->lv_slot_cfg_ver[ufront];

    const uint8_t *base = ctx->arena_blk.cpu + fs_arena_lbuf_off(ufront);
    if (y_out) *y_out = base;
    if (uv_out) *uv_out = base + uv_off;
    if (w_out) *w_out = w;
    if (h_out) *h_out = h;
    if (gen_out) *gen_out = gen;
    if (grain_tile_out) {
        Z8FilmSimConfig cfg;
        uint32_t active = 0u;
        filmsim_snapshot_cfg(ctx, &cfg, &active, 0);
        if (active && z8_filmsim_cfg_needs_grain(&cfg)) {
            *grain_tile_out = filmsim_try_acquire_gtile_locked(ctx);
        }
    }
    return 1;
}

const int8_t *filmsim_runtime_live_acquire_grain_tile(void)
{
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx || !filmsim_reader_enter(ctx)) {
        return 0;
    }
    if (!ctx->live_ws_ready || !ctx->arena_blk.cpu ||
        ctx->arena_blk.actual < FS_ARENA_GRAIN_LIVE_BYTES) {
        filmsim_reader_leave(ctx);
        return 0;
    }
    Z8FilmSimConfig cfg;
    uint32_t active = 0u;
    filmsim_snapshot_cfg(ctx, &cfg, &active, 0);
    if (!active || cfg.live_link == Z8_LIVE_LINK_OFF || !z8_filmsim_cfg_needs_grain(&cfg)) {
        filmsim_reader_leave(ctx);
        return 0;
    }
    const int8_t *tile = filmsim_try_acquire_gtile_locked(ctx);
    if (!tile) {
        filmsim_reader_leave(ctx);
        return 0;
    }
    atomic_fetch_add_explicit(&ctx->live_applied, 1u, memory_order_relaxed);
    atomic_store_explicit(&ctx->last_live_stages, Z8_FS_STAGE_GRAIN, memory_order_relaxed);
    return tile;
}

void filmsim_runtime_live_release(void)
{
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx) {
        return;
    }
    if (atomic_exchange_explicit(&ctx->lv_cpu0_gtile_held, 0u, memory_order_acq_rel)) {
        atomic_fetch_sub_explicit(&ctx->gtile_readers, 1u, memory_order_release);
    }
    filmsim_reader_leave(ctx);
}

int filmsim_live_try_copy_graded_lcd(uint32_t w, uint32_t h,
                                     uint32_t lut_gen, uint8_t hdf_level,
                                     uint32_t max_age_us,
                                     uint32_t last_seq,
                                     uint8_t *dst_y, uint8_t *dst_uv,
                                     uint32_t dst_stride,
                                     uint32_t *out_stages_applied,
                                     uint32_t *out_seq,
                                     uint64_t *out_age_us)
{
    if (out_stages_applied) {
        *out_stages_applied = 0u;
    }
    if (out_seq) {
        *out_seq = 0u;
    }
    if (out_age_us) {
        *out_age_us = 0u;
    }
    if (!dst_y || !dst_uv || w != 1024u || (h != 680u && h != 682u) || dst_stride < w) {
        return 0;
    }
    if (lut_gen == 0u && hdf_level == 0u) {
        return 0;
    }
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx ||
        atomic_load_explicit(&ctx->stopped, memory_order_acquire) ||
        atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire) ||
        atomic_load_explicit(&ctx->lv_cancel, memory_order_acquire) ||
        !atomic_load_explicit(&ctx->lcd_shared_valid, memory_order_acquire)) {
        return 0;
    }
    if (!filmsim_reader_enter(ctx)) {
        return 0;
    }
    if (!ctx->live_ws_ready || !ctx->arena_blk.cpu ||
        ctx->arena_blk.actual < FS_ARENA_FULL_BYTES ||
        !atomic_load_explicit(&ctx->lcd_shared_valid, memory_order_acquire)) {
        filmsim_reader_leave(ctx);
        return 0;
    }

    /* Single-writer seqlock snapshot with full ARM64 acquire barriers */
    FsLcdSharedFrame snap;
    memset(&snap, 0, sizeof(snap));
    int snap_ok = 0;
    for (unsigned attempt = 0u; attempt < 4u; ++attempt) {
        uint32_t s1 = atomic_load_explicit(&ctx->lcd_shared.seqlock, memory_order_acquire);
        if (s1 == 0u || (s1 & 1u) != 0u) {
            continue;
        }
        snap.seq            = ctx->lcd_shared.seq;
        snap.capture_us     = ctx->lcd_shared.capture_us;
        snap.publish_us     = ctx->lcd_shared.publish_us;
        snap.slot_gen       = ctx->lcd_shared.slot_gen;
        snap.slot           = ctx->lcd_shared.slot;
        snap.w              = ctx->lcd_shared.w;
        snap.h              = ctx->lcd_shared.h;
        snap.stride         = ctx->lcd_shared.stride;
        snap.uv_off         = ctx->lcd_shared.uv_off;
        snap.lut_gen        = ctx->lcd_shared.lut_gen;
        snap.stages_applied = ctx->lcd_shared.stages_applied;
        snap.hdf_level      = ctx->lcd_shared.hdf_level;
        snap.valid          = ctx->lcd_shared.valid;
        atomic_thread_fence(memory_order_acquire);
        uint32_t s2 = atomic_load_explicit(&ctx->lcd_shared.seqlock, memory_order_relaxed);
        if (s1 == s2) {
            snap_ok = 1;
            break;
        }
    }

    if (!snap_ok || !atomic_load_explicit(&ctx->lcd_shared_valid, memory_order_acquire) ||
        !snap.valid || snap.seq == 0u || snap.slot >= 3u ||
        snap.w != w || (snap.h != h && !(snap.h == 680u && h == 682u)) || snap.stride != w ||
        snap.lut_gen != lut_gen || snap.hdf_level != hdf_level ||
        snap.stages_applied == 0u ||
        (size_t)snap.uv_off + (size_t)snap.stride * (snap.h >> 1) > FS_ARENA_LFRAME_SIZE) {
        filmsim_reader_leave(ctx);
        return 0;
    }

    /* Freshness is strictly measured from the original capture timestamp (lv_slot_us),
     * NOT post-grade publish time, so LCD latency & freshness reflect true sensor age. */
    uint64_t now = z8_fs_plat_time_us();
    if (now < snap.capture_us || (now - snap.capture_us) > (uint64_t)max_age_us) {
        filmsim_reader_leave(ctx);
        return 0;
    }
    uint64_t age_us = now - snap.capture_us;

    /* Duplicate-frame guard: if Live's latest fresh matching frame is the exact same seq
     * LCD already displayed, return -1 so Layer 0 keeps the current bank (zero double-present,
     * zero wasted redundant CPU2 grading). Once stale (> max_age_us), falls through to 0 above. */
    if (last_seq != 0u && snap.seq == last_seq) {
        filmsim_reader_leave(ctx);
        return -1;
    }

    /* Dekker slot pin: prevents CPU0 live_begin_submit_nv12 from recycling LBUF[slot]
     * into CPU0_WRITING while we copy. Re-verify slot state + generation after seq_cst pin. */
    atomic_fetch_add_explicit(&ctx->lv_slot_lcd_pins[snap.slot], 1u, memory_order_seq_cst);
    uint32_t tb = atomic_load_explicit(&ctx->lv_tb_state, memory_order_seq_cst);
    uint32_t st = fs_tb_get(tb, snap.slot);
    if ((st != FS_TB_GRADED_READY && st != FS_TB_CPU0_FRONT) ||
        ctx->lv_slot_gen[snap.slot] != snap.slot_gen) {
        atomic_fetch_sub_explicit(&ctx->lv_slot_lcd_pins[snap.slot], 1u, memory_order_release);
        filmsim_reader_leave(ctx);
        return 0;
    }

    const uint8_t *src_base = ctx->arena_blk.cpu + fs_arena_lbuf_off(snap.slot);
    const uint8_t *src_y = src_base;
    const uint8_t *src_uv = src_base + snap.uv_off;
    uint32_t copy_h = (snap.h < h) ? snap.h : h;
    uint32_t copy_ch = copy_h >> 1;
    uint32_t ch = h >> 1;

    if (dst_stride == snap.stride) {
        memcpy(dst_y, src_y, (size_t)w * copy_h);
        memcpy(dst_uv, src_uv, (size_t)w * copy_ch);
    } else {
        for (uint32_t r = 0u; r < copy_h; ++r) {
            memcpy(dst_y + (size_t)r * dst_stride, src_y + (size_t)r * snap.stride, w);
        }
        for (uint32_t r = 0u; r < copy_ch; ++r) {
            memcpy(dst_uv + (size_t)r * dst_stride, src_uv + (size_t)r * snap.stride, w);
        }
    }
    for (uint32_t r = copy_h; r < h; ++r) {
        memcpy(dst_y + (size_t)r * dst_stride, dst_y + (size_t)(copy_h - 1u) * dst_stride, w);
    }
    for (uint32_t r = copy_ch; r < ch; ++r) {
        memcpy(dst_uv + (size_t)r * dst_stride, dst_uv + (size_t)(copy_ch - 1u) * dst_stride, w);
    }

    atomic_fetch_sub_explicit(&ctx->lv_slot_lcd_pins[snap.slot], 1u, memory_order_release);
    filmsim_reader_leave(ctx);

    if (out_stages_applied) {
        *out_stages_applied = snap.stages_applied;
    }
    if (out_seq) {
        *out_seq = snap.seq;
    }
    if (out_age_us) {
        *out_age_us = age_us;
    }
    return 1;
}

/* V71.7-r7 Reverse Single-Pass Layer 0 -> Live Shared Frame Feed:
 * Layer 0 (EVF 1280x852 or LCD 1024x682) pushes pre-grain LUT+HDF NV12 directly into LBUF
 * as FS_TB_GRADED_READY (>= 18 ms cadence), eliminating 100% of duplicate Live LUT + XM6 HDF
 * + CPU0 1280->1024 downscale while keeping Live video 1:1 native 1280x852 sharpness! */
int filmsim_runtime_live_push_layer0_nv12(const uint8_t *y, const uint8_t *uv,
                                          uint32_t w, uint32_t h,
                                          uint32_t y_stride, uint32_t uv_stride,
                                          uint64_t capture_us,
                                          uint32_t lut_gen, uint8_t hdf_level)
{
    if (!y || !uv || w < 4u || h < 2u || w > FS_LIVE_MAX_W || h > FS_LIVE_MAX_H ||
        ((w | h) & 1u) != 0u || y_stride < w || uv_stride < w) {
        return 0;
    }
    size_t uv_off = ((size_t)w * h + 63u) & ~(size_t)63u;
    size_t need = uv_off + (size_t)w * (h >> 1);
    if (need > FS_ARENA_LFRAME_SIZE) {
        return 0;
    }
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx ||
        atomic_load_explicit(&ctx->stopped, memory_order_acquire) ||
        atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire) ||
        atomic_load_explicit(&ctx->lv_cancel, memory_order_acquire) ||
        !atomic_load_explicit(&ctx->live_worker_ready, memory_order_acquire) ||
        !ctx->live_ws_ready || !ctx->arena_blk.handle) {
        return 0;
    }
#if FS_HAS_V71R7
    if ((live_status() & LIVE_REQUESTED) == 0u) {
        atomic_store_explicit(&ctx->l0_feed_last_push_us, 0u, memory_order_release);
        return 0;
    }
#endif

    /* Exact stage-parity check: Layer 0's (lut_gen, hdf_level) MUST match what Live Mode 2 wants.
     * If mismatched (e.g. WYSIWYG Preview Off + Live HDF On), clear feed latch immediately so
     * CPU0 + live_worker stay cleanly decoupled without an 80 ms stall! */
    uint32_t want_live_lut_gen = 0u;
    int want_live_lut = rt_live_lut_is_active(&want_live_lut_gen);
    Z8FilmSimConfig cfg;
    uint32_t active = 0u, ver = 0u;
    filmsim_snapshot_cfg(ctx, &cfg, &active, &ver);
    uint8_t want_live_hdf = (active && cfg.live_link == Z8_LIVE_LINK_ON &&
                             z8_filmsim_cfg_needs_hdf(&cfg))
                                ? cfg.hdf_level
                                : 0u;
    if ((!want_live_lut && want_live_hdf == 0u) ||
        lut_gen != (want_live_lut ? want_live_lut_gen : 0u) ||
        hdf_level != want_live_hdf) {
        atomic_store_explicit(&ctx->l0_feed_last_push_us, 0u, memory_order_release);
        return 0;
    }

    /* >= 18 ms cadence gate: at 60 Hz EVF/LCD pushes exact 30.0 Hz (every 2nd frame);
     * at 30..50 Hz pushes every frame for minimum CFR latency and 0% repeat frames! */
    uint64_t now = z8_fs_plat_time_us();
    uint64_t last = atomic_load_explicit(&ctx->l0_feed_last_push_us, memory_order_acquire);
    if (last != 0u && now >= last && (now - last) < UINT64_C(18000)) {
        return 0;
    }

    if (!filmsim_reader_enter(ctx)) {
        return 0;
    }
    if (!ctx->live_ws_ready || !ctx->arena_blk.cpu ||
        ctx->arena_blk.actual < FS_ARENA_FULL_BYTES) {
        filmsim_reader_leave(ctx);
        return 0;
    }

    /* Lock-free slot claim: NEVER touch CPU0_FRONT, CPU0_WRITING, CPU2_GRADING, or QUARANTINED!
     * Prefer FREE, else PENDING_RAW, else unconsumed GRADED_READY (all with lcd_pins == 0). */
    uint32_t cur = atomic_load_explicit(&ctx->lv_tb_state, memory_order_acquire);
    int chosen = -1;
    for (;;) {
        int s_free = -1, s_pend = -1, s_ready = -1;
        for (unsigned s = 0u; s < 3u; ++s) {
            uint32_t st = fs_tb_get(cur, s);
            uint32_t pinned = atomic_load_explicit(&ctx->lv_slot_lcd_pins[s], memory_order_seq_cst);
            if (pinned) {
                continue;
            }
            if (st == FS_TB_FREE && s_free < 0) {
                s_free = (int)s;
            } else if (st == FS_TB_PENDING_RAW && s_pend < 0) {
                s_pend = (int)s;
            } else if (st == FS_TB_GRADED_READY && s_ready < 0) {
                s_ready = (int)s;
            }
        }
        chosen = (s_free >= 0) ? s_free : ((s_pend >= 0) ? s_pend : s_ready);
        if (chosen < 0) {
            filmsim_reader_leave(ctx);
            return 0;
        }
        uint32_t next = fs_tb_set(cur, (unsigned)chosen, FS_TB_CPU2_GRADING);
        if (atomic_compare_exchange_weak_explicit(&ctx->lv_tb_state, &cur, next,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            break;
        }
    }

    unsigned uslot = (unsigned)chosen;
    uint8_t *dst_base = ctx->arena_blk.cpu + fs_arena_lbuf_off(uslot);
    uint8_t *dst_y = dst_base;
    uint8_t *dst_uv = dst_base + uv_off;
    uint32_t ch = h >> 1;
    int use_neon = rt_neon_allowed();

    if (y_stride == w && uv_stride == w) {
        if (use_neon) {
            zf_grain_copy_u8_fast(dst_y, y, w * h);
            zf_grain_copy_u8_fast(dst_uv, uv, w * ch);
        } else {
            memcpy(dst_y, y, (size_t)w * h);
            memcpy(dst_uv, uv, (size_t)w * ch);
        }
    } else {
        for (uint32_t r = 0u; r < h; ++r) {
            memcpy(dst_y + (size_t)r * w, y + (size_t)r * y_stride, w);
        }
        for (uint32_t r = 0u; r < ch; ++r) {
            memcpy(dst_uv + (size_t)r * w, uv + (size_t)r * uv_stride, w);
        }
    }

    uint64_t new_gen = __atomic_add_fetch(&ctx->lv_work_gen, 1u, __ATOMIC_ACQ_REL);
    ctx->lv_slot_w[uslot] = w;
    ctx->lv_slot_h[uslot] = h;
    ctx->lv_slot_uv_off[uslot] = (uint32_t)uv_off;
    ctx->lv_slot_us[uslot] = capture_us ? capture_us : now;
    ctx->lv_slot_cfg_ver[uslot] = ver;
    ctx->lv_slot_gen[uslot] = new_gen;
    ctx->lv_work_cfg_ver = ver;

    /* Transition uslot -> GRADED_READY and drop any stale older GRADED_READY or PENDING_RAW! */
    uint32_t pcur = atomic_load_explicit(&ctx->lv_tb_state, memory_order_acquire);
    for (;;) {
        if (fs_tb_get(pcur, uslot) != FS_TB_CPU2_GRADING) {
            break;
        }
        uint32_t pnext = pcur;
        for (unsigned k = 0u; k < 3u; ++k) {
            if (k != uslot) {
                uint32_t ks = fs_tb_get(pnext, k);
                if (ks == FS_TB_GRADED_READY || ks == FS_TB_PENDING_RAW) {
                    pnext = fs_tb_set(pnext, k, FS_TB_FREE);
                }
            }
        }
        pnext = fs_tb_set(pnext, uslot, FS_TB_GRADED_READY);
        if (atomic_compare_exchange_weak_explicit(&ctx->lv_tb_state, &pcur, pnext,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            break;
        }
    }

    atomic_store_explicit(&ctx->l0_feed_last_push_us, now ? now : 1u, memory_order_release);
    atomic_fetch_add_explicit(&ctx->l0_feed_pushes, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&ctx->live_applied, 1u, memory_order_relaxed);
    uint32_t fs_stages = (lut_gen ? Z8_FS_STAGE_LUT : 0u) | (hdf_level ? Z8_FS_STAGE_HDF : 0u);
    atomic_store_explicit(&ctx->last_live_stages, fs_stages, memory_order_relaxed);
    filmsim_reader_leave(ctx);
    return 1;
}

int filmsim_runtime_live_layer0_feed_active(void)
{
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx ||
        atomic_load_explicit(&ctx->stopped, memory_order_acquire) ||
        atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire) ||
        atomic_load_explicit(&ctx->lv_cancel, memory_order_acquire) ||
        !ctx->live_ws_ready) {
        return 0;
    }
    uint64_t last = atomic_load_explicit(&ctx->l0_feed_last_push_us, memory_order_acquire);
    if (last == 0u) {
        return 0;
    }
    uint64_t now = z8_fs_plat_time_us();
    return (now >= last && (now - last) < UINT64_C(80000)) ? 1 : 0;
}

int filmsim_runtime_live_independent_active(void)
{
    if (filmsim_runtime_live_mode() != FILMSIM_LIVE_MODE_FULL_CPU2) {
        return 0;
    }
    return filmsim_runtime_live_layer0_feed_active() ? 0 : 1;
}

/* Legacy V70 I420 stubs retained for compatibility. */
void filmsim_runtime_live_submit(const uint8_t *i420, unsigned width,
                                 unsigned height, uint64_t source_us)
{
    (void)i420;
    (void)width;
    (void)height;
    (void)source_us;
}

const uint8_t *filmsim_runtime_live_acquire(unsigned width, unsigned height,
                                            uint64_t *front_us_out)
{
    (void)width;
    (void)height;
    (void)front_us_out;
    return 0;
}

void filmsim_runtime_apply_live_i420(uint8_t *i420, unsigned width, unsigned height)
{
    if (!i420 || width < 4u || height < 2u || ((width | height) & 1u)) {
        return;
    }
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx || atomic_load_explicit(&ctx->stopped, memory_order_acquire)) {
        return;
    }

    Z8FilmSimConfig cfg;
    uint32_t active = 0u;
    filmsim_snapshot_cfg(ctx, &cfg, &active, 0);
    if (!active || cfg.live_link == Z8_LIVE_LINK_OFF || z8_filmsim_cfg_is_dormant(&cfg)) {
        atomic_fetch_add_explicit(&ctx->live_skipped, 1u, memory_order_relaxed);
        return;
    }

    if (!filmsim_reader_enter(ctx)) {
        atomic_fetch_add_explicit(&ctx->live_skipped, 1u, memory_order_relaxed);
        return;
    }

    uint32_t min_arena = z8_filmsim_cfg_needs_hdf(&cfg)
                             ? FS_ARENA_FULL_BYTES
                             : FS_ARENA_GRAIN_LIVE_BYTES;
    if (!ctx->live_ws_ready || !ctx->arena_blk.cpu ||
        ctx->arena_blk.actual < min_arena) {
        filmsim_reader_leave(ctx);
        atomic_fetch_add_explicit(&ctx->live_skipped, 1u, memory_order_relaxed);
        return;
    }

    z8_filmsim_assets assets;
    memset(&assets, 0, sizeof(assets));
    assets.zf_maps = ctx->zf_maps_loaded ? (const void *)ctx->zf_maps_box.storage : 0;

    uint8_t *ws = ctx->arena_blk.cpu + FS_ARENA_LWS_OFF;
    uint8_t *cbcr = (ctx->arena_blk.actual >= FS_ARENA_FULL_BYTES)
                        ? (ctx->arena_blk.cpu + FS_ARENA_LBUF2_OFF)
                        : 0;
    size_t cbcr_bytes = cbcr ? FS_ARENA_LFRAME_SIZE : 0u;
    uint32_t stages = 0u;

    int rc = z8_filmsim_apply_live_i420(i420, width, height, FILMSIM_LIVE_GRAIN_SEED,
                                        &cfg, &assets,
                                        ws, FS_ARENA_LWS_SIZE,
                                        cbcr, cbcr_bytes,
                                        filmsim_live_cancelled, ctx,
                                        &stages);
    filmsim_reader_leave(ctx);

    if (rc == Z8_FS_OK) {
        atomic_fetch_add_explicit(&ctx->live_applied, 1u, memory_order_relaxed);
        atomic_store_explicit(&ctx->last_live_stages, stages, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&ctx->live_skipped, 1u, memory_order_relaxed);
    }
}

static void filmsim_fill_status_struct(struct filmsim_runtime_context *ctx, Z8FilmSimStatus *st)
{
    memset(st, 0, sizeof(*st));
    st->magic = Z8_FILMSIM_CFG_MAGIC;
    st->version = (uint16_t)Z8_FILMSIM_CFG_VERSION;
    st->bytes = (uint16_t)Z8_FILMSIM_STATUS_BYTES;
    uint8_t xm6_mode = (uint8_t)Z8_XM6_MODE_AUTO;

    if (!ctx) {
        st->flags = Z8_FS_FLAG_EL1_READY | Z8_FS_FLAG_DORMANT;
        st->active_pc_sel = filmsim_read_active_pc_sel(0);
        z8_xm6_fill_status(0, xm6_mode, st);
        st->active_drive = (uint8_t)filmsim_card_drive();
        return;
    }

    uint32_t fl = Z8_FS_FLAG_EL1_READY;
    if (ctx->zf_maps_loaded) {
        fl |= Z8_FS_FLAG_ZF_MAPS_LOADED;
    }
    if (atomic_load_explicit(&ctx->dormant, memory_order_acquire)) {
        fl |= Z8_FS_FLAG_DORMANT;
    }
    Z8FilmSimConfig snap;
    uint32_t act = 0u;
    filmsim_snapshot_cfg(ctx, &snap, &act, 0);
    if (act && snap.live_link == Z8_LIVE_LINK_ON && ctx->live_ws_ready) {
        fl |= Z8_FS_FLAG_LIVE_ACTIVE;
    }

    uint32_t sel = filmsim_read_active_pc_sel(ctx);
    uint32_t src = Z8_NP3_SRC_NONE;
    uint32_t bound = 0u;
    if (ctx->base_cfg_valid) {
        xm6_mode = ctx->base_cfg.xm6_mode;
        if (ctx->base_cfg.apply_mode == Z8_FS_APPLY_PER_NP3) {
            fl |= Z8_FS_FLAG_PER_NP3_MODE;
            (void)filmsim_resolve_np3_recipe(ctx, sel, 0, &src, &bound);
            if (bound) {
                fl |= Z8_FS_FLAG_RECIPE_BOUND;
            }
        }
    } else {
        xm6_mode = snap.xm6_mode;
    }

    st->flags = fl;
    st->active_lut_slot = 0u;
    st->last_error = ctx->last_error;
    st->photo_applied =
        __atomic_load_n(&ctx->photo.photos_completed, __ATOMIC_RELAXED) +
        __atomic_load_n(&ctx->photo_slot1.photos_completed, __ATOMIC_RELAXED);
    st->photo_skipped =
        __atomic_load_n(&ctx->photo.photos_bypassed, __ATOMIC_RELAXED) +
        __atomic_load_n(&ctx->photo_slot1.photos_bypassed, __ATOMIC_RELAXED);
    st->live_applied = atomic_load_explicit(&ctx->live_applied, memory_order_relaxed);
    st->live_skipped = atomic_load_explicit(&ctx->live_skipped, memory_order_relaxed);
    st->active_pc_sel = sel;
    st->active_recipe_src = (uint8_t)src;
    st->active_recipe_bound = (uint8_t)bound;
    z8_xm6_fill_status(&ctx->xm6, xm6_mode, st);
    st->active_drive = (uint8_t)filmsim_card_drive();
}

static void filmsim_try_lazy_load_zf_for_diag(struct filmsim_runtime_context *ctx)
{
    if (!ctx || ctx->zf_maps_loaded) {
        return;
    }
    if (!filmsim_asset_writer_try_lock(ctx)) {
        return;
    }
    if (!ctx->zf_maps_loaded) {
        if (!ctx->zf_grain_buf) {
            ctx->zf_grain_buf = FS_ALLOC(FS_ZF_BUF_BYTES);
        }
        if (ctx->zf_grain_buf) {
            struct fs_native_file_wrap fw;
            memset(&fw, 0, sizeof(fw));
            fw.rt = ctx;
            live_native_file_ops(&fw.fops, &fw.fctx);
            z8_filmsim_file_ops fops = {
                .opaque = &fw,
                .open_read = fs_wrap_open_read,
                .read = fs_wrap_read,
                .close = fs_wrap_close,
                .cancelled = fs_wrap_cancelled
            };
            char drive = filmsim_card_drive();
            int rc = z8_filmsim_load_zf_grain_bin(&fops, drive,
                                                  ctx->zf_grain_buf,
                                                  FS_ZF_BUF_BYTES,
                                                  &ctx->zf_maps_box);
            if (rc == Z8_FS_ASSET_OK) {
                z8_xm6_dcache_civac(&ctx->xm6, ctx->zf_grain_buf, Z8_XM6_ZFG1_CLEAN_BYTES);
                ctx->zf_maps_loaded = 1u;
                ctx->zf_maps_tried_epoch = 0u;
                ctx->zf_maps_retry_after_us = 0u;
                ++ctx->zf_epoch;
            }
        }
    }
    filmsim_asset_writer_unlock(ctx);
}

int filmsim_runtime_service_call_ctx(struct filmsim_runtime_context *ctx,
                                     uint32_t op,
                                     const void *req, uint32_t req_bytes,
                                     void *resp, uint32_t *resp_bytes)
{
    if (op == Z8_FS_OP_GET) {
        if (!resp || !resp_bytes || *resp_bytes < Z8_FILMSIM_CFG_BYTES) {
            return -1;
        }
        Z8FilmSimConfig out;
        if (ctx && ctx->base_cfg_valid) {
            out = ctx->base_cfg;
        } else if (ctx) {
            filmsim_snapshot_cfg(ctx, &out, 0, 0);
        } else {
            z8_filmsim_cfg_default(&out);
        }
        memcpy(resp, &out, sizeof(out));
        *resp_bytes = Z8_FILMSIM_CFG_BYTES;
        return 0;
    }

    if (op == Z8_FS_OP_SET) {
        if (!ctx || !req || req_bytes != Z8_FILMSIM_CFG_BYTES) {
            return -1;
        }
        Z8FilmSimConfig incoming;
        memcpy(&incoming, req, sizeof(incoming));
        if (!z8_filmsim_cfg_validate(&incoming)) {
            return -2;
        }
        z8_filmsim_cfg_normalize(&incoming);
        ctx->base_cfg = incoming;
        ctx->base_cfg_valid = 1u;
        z8_xm6_clear_live_soft_backoff(&ctx->xm6);

        Z8FilmSimConfig eff;
        uint32_t sel = 0u, src = Z8_NP3_SRC_NONE, bound = 0u;
        filmsim_resolve_effective_cfg(ctx, &ctx->base_cfg, &eff, &sel, &src, &bound);
        atomic_store_explicit(&ctx->active_pc_sel, sel, memory_order_release);
        atomic_store_explicit(&ctx->active_recipe_src, src, memory_order_release);
        atomic_store_explicit(&ctx->active_recipe_bound, bound, memory_order_release);
        filmsim_publish_cfg(ctx, &eff, 1u, 1u, ctx->bus_corr + 1u);
        if (resp_bytes) {
            *resp_bytes = 0u;
        }
        return 0;
    }

    if (op == Z8_FS_OP_STATUS) {
        if (!resp || !resp_bytes || *resp_bytes < Z8_FILMSIM_STATUS_BYTES) {
            return -1;
        }
        Z8FilmSimStatus st;
        filmsim_fill_status_struct(ctx, &st);
        memcpy(resp, &st, sizeof(st));
        *resp_bytes = Z8_FILMSIM_STATUS_BYTES;
        return 0;
    }

    if (op == Z8_FS_OP_XM6_DIAG) {
        if (req_bytes != 0u || !resp || !resp_bytes || *resp_bytes < Z8_XM6_DIAG_BYTES) {
            return -1;
        }
        uint32_t skip_reason = 0u;
        if (!ctx) {
            skip_reason = Z8_XM6_DIAG_SKIP_NO_CTX;
        } else if (atomic_load_explicit(&ctx->stopped, memory_order_acquire)) {
            skip_reason = Z8_XM6_DIAG_SKIP_STOPPED;
        } else if (atomic_load_explicit(&ctx->pool_suspended, memory_order_acquire)) {
            skip_reason = Z8_XM6_DIAG_SKIP_SUSPENDED;
        }
        if (ctx && skip_reason == 0u && !ctx->zf_maps_loaded && z8_xm6_core0_hw_only_ready(&ctx->xm6)) {
            filmsim_try_lazy_load_zf_for_diag(ctx);
        }
        Z8Xm6DiagResult diag;
        z8_xm6_run_diag_ex(ctx ? &ctx->xm6 : 0,
                           ctx ? ctx->zf_grain_buf : 0,
                           ctx ? ctx->zf_maps_loaded : 0u,
                           skip_reason,
                           &diag);
        memcpy(resp, &diag, sizeof(diag));
        *resp_bytes = Z8_XM6_DIAG_BYTES;
        return 0;
    }

    if (op == Z8_FS_OP_XM6_RESET_BREAKER) {
        if (req_bytes != 0u || !resp || !resp_bytes || *resp_bytes < Z8_FILMSIM_STATUS_BYTES) {
            return -1;
        }
        if (ctx) {
            z8_xm6_reset_breaker(&ctx->xm6);
        }
        Z8FilmSimStatus st;
        filmsim_fill_status_struct(ctx, &st);
        memcpy(resp, &st, sizeof(st));
        *resp_bytes = Z8_FILMSIM_STATUS_BYTES;
        return 0;
    }

    if (op == Z8_FS_OP_SET_RECIPE) {
        if (!ctx || !req || req_bytes != Z8_NP3_MSG_BYTES) {
            return -1;
        }
        Z8FilmSimRecipeMsg msg;
        memcpy(&msg, req, sizeof(msg));
        uint32_t slot = msg.slot_index;
        if (slot == Z8_NP3_SLOT_INVALID) {
            slot = z8_np3_selector_to_slot(msg.selector);
        } else if (msg.selector != Z8_NP3_SLOT_INVALID &&
                   z8_np3_selector_to_slot(msg.selector) != slot) {
            return -2;
        }
        if (slot >= Z8_NP3_SLOT_COUNT) {
            return -2;
        }
        if ((msg.recipe.flags & Z8_NP3_RECIPE_FLAG_ENABLED) != 0u) {
            if (!z8_np3_recipe_is_valid(&msg.recipe)) {
                return -2;
            }
        } else if (msg.recipe.flags != 0u) {
            return -2;
        }

        Z8Np3SlotEntry ent;
        z8_np3_recipe_to_entry(&msg.recipe, &ent);
        uint32_t s = atomic_load_explicit(&ctx->recipe_seqlock, memory_order_relaxed);
        atomic_store_explicit(&ctx->recipe_seqlock, s + 1u, memory_order_release);
        ctx->recipe_table.entries[slot] = ent;
        z8_np3_table_finalize(&ctx->recipe_table, ctx->recipe_table.seq + 1u);
        atomic_store_explicit(&ctx->recipe_seqlock, s + 2u, memory_order_release);
        ctx->recipe_table_loaded = 1u;
        ctx->recipe_table_synced_by_ipc = 1u;

        if (ctx->base_cfg_valid && ctx->base_cfg.apply_mode == Z8_FS_APPLY_PER_NP3) {
            Z8FilmSimConfig eff;
            uint32_t sel = 0u, src = Z8_NP3_SRC_NONE, bound = 0u;
            filmsim_resolve_effective_cfg(ctx, &ctx->base_cfg, &eff, &sel, &src, &bound);
            atomic_store_explicit(&ctx->active_pc_sel, sel, memory_order_release);
            atomic_store_explicit(&ctx->active_recipe_src, src, memory_order_release);
            atomic_store_explicit(&ctx->active_recipe_bound, bound, memory_order_release);
            filmsim_publish_cfg(ctx, &eff, 1u, 1u, ctx->bus_corr);
        }
        if (resp_bytes) {
            *resp_bytes = 0u;
        }
        return 0;
    }

    if (op == Z8_FS_OP_GET_RECIPE) {
        if (!req || req_bytes != Z8_NP3_QUERY_BYTES ||
            !resp || !resp_bytes || *resp_bytes < Z8_NP3_MSG_BYTES) {
            return -1;
        }
        Z8FilmSimRecipeQuery q;
        memcpy(&q, req, sizeof(q));
        uint32_t sel, slot;
        if (q.selector == Z8_NP3_SEL_ACTIVE_QUERY) {
            sel = filmsim_read_active_pc_sel(ctx);
            slot = z8_np3_selector_to_slot(sel);
        } else if (q.slot_index != Z8_NP3_SLOT_INVALID) {
            if (q.slot_index >= Z8_NP3_SLOT_COUNT) {
                return -2;
            }
            slot = q.slot_index;
            sel = (q.selector != Z8_NP3_SLOT_INVALID) ? q.selector : z8_np3_slot_to_selector(slot);
        } else {
            sel = q.selector;
            slot = z8_np3_selector_to_slot(sel);
        }

        Z8FilmSimRecipeMsg reply;
        memset(&reply, 0, sizeof(reply));
        reply.selector = sel;
        reply.slot_index = slot;
        (void)filmsim_resolve_np3_recipe(ctx, sel, &reply.recipe, 0, 0);
        memcpy(resp, &reply, sizeof(reply));
        *resp_bytes = Z8_NP3_MSG_BYTES;
        return 0;
    }

    if (op == Z8_FS_OP_SYNC_RECIPES) {
        if (!ctx || !req || req_bytes != Z8_NP3_CHUNK_BYTES) {
            return -1;
        }
        Z8FilmSimRecipeChunk chunk;
        memcpy(&chunk, req, sizeof(chunk));
        if (chunk.magic != Z8_NP3_TABLE_MAGIC ||
            chunk.count == 0u || chunk.count > Z8_NP3_CHUNK_MAX_SLOTS ||
            (uint32_t)chunk.start_slot + (uint32_t)chunk.count > Z8_NP3_SLOT_COUNT ||
            (chunk.flags & ~1u) != 0u) {
            return -2;
        }
        for (uint32_t i = 0u; i < chunk.count; ++i) {
            if (!z8_np3_entry_is_valid(&chunk.entries[i])) {
                return -2;
            }
        }

        uint32_t s = atomic_load_explicit(&ctx->recipe_seqlock, memory_order_relaxed);
        atomic_store_explicit(&ctx->recipe_seqlock, s + 1u, memory_order_release);
        if (chunk.flags & 1u) {
            z8_np3_table_init_empty(&ctx->recipe_table);
        }
        for (uint32_t i = 0u; i < chunk.count; ++i) {
            ctx->recipe_table.entries[chunk.start_slot + i] = chunk.entries[i];
        }
        z8_np3_table_finalize(&ctx->recipe_table, chunk.seq);
        atomic_store_explicit(&ctx->recipe_seqlock, s + 2u, memory_order_release);
        ctx->recipe_table_loaded = 1u;
        ctx->recipe_table_synced_by_ipc = 1u;

        if (ctx->base_cfg_valid && ctx->base_cfg.apply_mode == Z8_FS_APPLY_PER_NP3) {
            Z8FilmSimConfig eff;
            uint32_t sel = 0u, src = Z8_NP3_SRC_NONE, bound = 0u;
            filmsim_resolve_effective_cfg(ctx, &ctx->base_cfg, &eff, &sel, &src, &bound);
            atomic_store_explicit(&ctx->active_pc_sel, sel, memory_order_release);
            atomic_store_explicit(&ctx->active_recipe_src, src, memory_order_release);
            atomic_store_explicit(&ctx->active_recipe_bound, bound, memory_order_release);
            filmsim_publish_cfg(ctx, &eff, 1u, 1u, ctx->bus_corr);
        }
        if (resp_bytes) {
            *resp_bytes = 0u;
        }
        return 0;
    }

    return -3;
}

int filmsim_runtime_service_call(uint32_t op,
                                 const void *req, uint32_t req_bytes,
                                 void *resp, uint32_t *resp_bytes)
{
    struct filmsim_runtime_context *ctx = filmsim_ctx_get();
    if (!ctx && op == Z8_FS_OP_SET) {
        if (!req || req_bytes != Z8_FILMSIM_CFG_BYTES) {
            return -1;
        }
        Z8FilmSimConfig incoming;
        memcpy(&incoming, req, sizeof(incoming));
        if (!z8_filmsim_cfg_validate(&incoming)) {
            return -2;
        }
        z8_filmsim_cfg_normalize(&incoming);
        /* Preserve strict 0-byte dormant invariant on cold boot when default GLOBAL/AUTO is Off. */
        if (incoming.apply_mode == Z8_FS_APPLY_GLOBAL &&
            incoming.xm6_mode == Z8_XM6_MODE_AUTO &&
            z8_filmsim_cfg_is_dormant(&incoming)) {
            if (resp_bytes) {
                *resp_bytes = 0u;
            }
            return 0;
        }
        ctx = filmsim_ensure_context();
    } else if (!ctx && (op == Z8_FS_OP_SET_RECIPE ||
                        op == Z8_FS_OP_SYNC_RECIPES ||
                        (op == Z8_FS_OP_XM6_DIAG && z8_xm6_core0_hw_only_ready(0)))) {
        ctx = filmsim_ensure_context();
    }
    return filmsim_runtime_service_call_ctx(ctx, op, req, req_bytes, resp, resp_bytes);
}

/* Weak sub-dispatchers for Live Photo (ops 48..63, 'LPI1') and 3D LUT (ops 64..79, 'LTI1').
 * Overridden by strong implementations in live_ui.c (live_primary) and photo_worker.c (Bank 2). */
#ifndef FILMSIM_HOST_TEST_MOCK_IPC
__attribute__((weak))
int live_ipc_service_call(uint32_t raw_op,
                          const void *req, uint32_t req_bytes,
                          void *resp, uint32_t *resp_bytes)
{
    (void)raw_op;
    (void)req;
    (void)req_bytes;
    (void)resp;
    (void)resp_bytes;
    return -3; /* FX_CAPABILITY */
}

__attribute__((weak))
int v71r7_lut_ipc_service_call(uint32_t raw_op,
                               const void *req, uint32_t req_bytes,
                               void *resp, uint32_t *resp_bytes)
{
    (void)raw_op;
    (void)req;
    (void)req_bytes;
    (void)resp;
    (void)resp_bytes;
    return -3; /* FX_CAPABILITY */
}
#else
int live_ipc_service_call(uint32_t raw_op,
                          const void *req, uint32_t req_bytes,
                          void *resp, uint32_t *resp_bytes);
int v71r7_lut_ipc_service_call(uint32_t raw_op,
                               const void *req, uint32_t req_bytes,
                               void *resp, uint32_t *resp_bytes);
#endif

/* Fenestra slots.state EL1 IPC router (ops 32..79 -> z8_op 0..47):
 *   z8_op  0..15 (raw 32..47, 'FSI1'): filmsim_runtime_service_call(z8_op, ...)
 *   z8_op 16..31 (raw 48..63, 'LPI1'): live_ipc_service_call(raw_op, ...)
 *   z8_op 32..47 (raw 64..79, 'LTI1'): v71r7_lut_ipc_service_call(raw_op, ...)
 * FxServiceRequest layout: bytes @ +0x0c (u32), data[768] @ +0x30.
 * FxServiceReply   layout: bytes @ +0x08 (u32), value[0] @ +0x10 (u64), data[768] @ +0x30.
 * Returns Fenestra FxResult: 0=FX_OK, 1=FX_ARGUMENT, 2=FX_STATE, 3=FX_CAPABILITY, 5=FX_BUSY. */
__attribute__((noinline))
uint32_t filmsim_slots_svc_dispatch(uint32_t z8_op, const void *q_ptr, void *a_ptr)
{
    if (!q_ptr || !a_ptr || z8_op >= Z8_SLOTS_ROUTER_OP_COUNT) {
        return 1u; /* FX_ARGUMENT */
    }
    const uint8_t *qb = (const uint8_t *)q_ptr;
    uint8_t *ab = (uint8_t *)a_ptr;
    uint32_t q_bytes = 0u;
    memcpy(&q_bytes, qb + 0x0cu, sizeof(q_bytes));
    if (q_bytes < Z8_FS_SLOTS_HDR_BYTES || q_bytes > 768u) {
        return 1u; /* FX_ARGUMENT */
    }
    uint32_t hdr[4];
    memcpy(hdr, qb + 0x30u, sizeof(hdr));
    uint32_t expected_magic = (z8_op < Z8_FS_SLOTS_OP_COUNT)
                                  ? Z8_FS_SLOTS_TUNNEL_MAGIC
                                  : ((z8_op < 32u) ? Z8_LP_SLOTS_TUNNEL_MAGIC
                                                   : Z8_LUT_SLOTS_TUNNEL_MAGIC);
    if (hdr[0] != expected_magic ||
        hdr[1] > (q_bytes - Z8_FS_SLOTS_HDR_BYTES) ||
        hdr[2] != 0u || hdr[3] != 0u) {
        return 1u; /* FX_ARGUMENT */
    }

    uint32_t resp_bytes = 768u;
    const void *req_payload = (hdr[1] != 0u) ? (const void *)(qb + 0x30u + Z8_FS_SLOTS_HDR_BYTES) : 0;
    int rc;
    if (z8_op < Z8_FS_SLOTS_OP_COUNT) {
        rc = filmsim_runtime_service_call(z8_op, req_payload, hdr[1], ab + 0x30u, &resp_bytes);
    } else if (z8_op < 32u) {
        rc = live_ipc_service_call(z8_op + Z8_FS_SLOTS_OP_BASE,
                                   req_payload, hdr[1], ab + 0x30u, &resp_bytes);
    } else {
        rc = v71r7_lut_ipc_service_call(z8_op + Z8_FS_SLOTS_OP_BASE,
                                        req_payload, hdr[1], ab + 0x30u, &resp_bytes);
    }
    if (rc == 0) {
        if (resp_bytes > 768u) {
            resp_bytes = 768u;
        }
        memcpy(ab + 0x08u, &resp_bytes, sizeof(resp_bytes));
        uint64_t v0 = (uint64_t)resp_bytes;
        memcpy(ab + 0x10u, &v0, sizeof(v0));
        return 0u; /* FX_OK */
    }
    if (rc == -2) {
        return 1u; /* FX_ARGUMENT */
    }
    if (rc == -3) {
        return 3u; /* FX_CAPABILITY */
    }
    if (rc == -5) {
        return 5u; /* FX_BUSY */
    }
    return 2u; /* FX_STATE */
}

#if defined(__aarch64__) && !defined(FILMSIM_HOST_TEST)
/* Naked entry branched to from v68_lang_svc (0x401ff0f4) for slots.state ops 32..79.
 * In: x9 = op - 32 (0..47), x22 = const FxServiceRequest *, x19 = FxServiceReply *.
 * Out: jumps to SVC_RET (0x401a7898) with w0 = FxResult; callee-saved regs & SP preserved. */
__attribute__((naked, used)) void filmsim_slots_svc_trampoline(void)
{
    __asm__ volatile(
        "mov x0, x9\n"
        "mov x1, x22\n"
        "mov x2, x19\n"
        "bl filmsim_slots_svc_dispatch\n"
        "movz x9, #0x7898\n"
        "movk x9, #0x401a, lsl #16\n"
        "br x9\n"
    );
}
#else
void filmsim_slots_svc_trampoline(void) {}
#endif
