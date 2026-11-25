/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Display Layer 0 WYSIWYG Preview (LUT + Ricoh HDF + Nikon Zf Grain),
 * HangFix V69 pool_suspend/pool_resume, and POWER_OFF_SYNC/POWER_CANCEL_SYNC.
 *
 * Adapted from Fenestra-Z8-C311-source (modules/lut_preview/layer0_provider.h) for
 * V71.7 static unified EL1 integration:
 *   - All 6 verified-clean stock ROM hook sites:
 *       0x40E9A540 (BL 0x40CD7AD8 -> v71r7_layer0_enqueue_hook)
 *       0x40CD862C (BL 0x402B6D88 -> v71r7_layer0_display_hook)
 *       0x40CD72A4 (BL 0x40CD9640 -> v71r7_layer0_vsync_hook)
 *       0x40CD72D4 (BL 0x40CD9640 -> v71r7_layer0_vsync_hook)
 *       0x40D67328 (BL 0x40D60E94 -> v71r7_power_sync_hook, POWER_CANCEL_SYNC=11)
 *       0x40D67340 (BL 0x40D60E94 -> v71r7_power_sync_hook, POWER_OFF_SYNC=10)
 *   - ZERO .data/.bss in ROM: all mutable state lives in heap-allocated Layer0Ctx
 *     anchored at v71r7_anchor()->layer0_ctx (DRAM 0xA11640000).
 *   - STRICT SCALAR: compiles cleanly with -mgeneral-regs-only (0 FP, 0 SIMD).
 */
#ifndef V71R7_LAYER0_PROVIDER_H
#define V71R7_LAYER0_PROVIDER_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "../lut_core/v71r7_lut_abi.h"
#include "photo_workspace.h"

/* Version 11 metrics ABI (upstream-compatible layout, heap-embedded inside Layer0Ctx). */
typedef struct {
    _Atomic uint32_t version, bytes;
    _Atomic int32_t hw_current;
    _Atomic uint32_t active_banks_mask, disp_calls, disp_substituted;
    _Atomic uint32_t disp_kept_current, disp_stock_pass, disp_rejects;
    _Atomic uint32_t vsync_ticks, banks_retired;
    _Atomic uint32_t enqueue_seen, enqueue_copied, enqueue_skipped_busy;
    _Atomic uint32_t enqueue_rejected_fmt, enqueue_content_changed;
    _Atomic uint32_t copy_us, max_copy_us, grades, grade_us, max_grade_us, grade_timeouts;
    _Atomic uint32_t display_mode, copy_mode, last_latency_us, max_latency_us;
    _Atomic uint32_t last_interframe_ms, max_interframe_ms, distinct_sources, repeated_sources;
    _Atomic uint32_t errors;
    _Atomic int32_t last_result, pool_result;
    _Atomic uint32_t online, enabled, display_ready, mode_ok, hook_active, quarantined;
    _Atomic uint32_t lut_bytes, grid, surface_bytes, disp_shadow_flag, stock_baseline_confirmed;
    _Atomic uint32_t source_lag, mismatch_y, mismatch_uv, mismatch_meta, last_mismatch_row, step_bursts;
    _Atomic uint64_t last_captured_ms, last_presented_ms, last_source_stamp;
    _Atomic uint64_t last_sub_raw_y, last_stock_y, vsync_seq, last_graded_ms, last_presented_generation;
    _Atomic uint32_t sentinel_status, neon_active;
    _Atomic uint32_t active_route, lcd_substituted, evf_substituted, worker_cores, fps_milli, capture_aborts;
    _Atomic int32_t lcd_hw_current, evf_hw_current;
    _Atomic uint64_t evf_vsync_seq;
    _Atomic uint32_t helper_sentinel[3];
    uint32_t reserved_v9;
    _Atomic uint32_t wait_no_source, wait_no_bank, vsync_claim_misses, cross_route_retires;
    _Atomic uint32_t last_sleep_us, max_sleep_us;
    _Atomic uint32_t frame_us, max_frame_us, overlapped_frames, jpeg_pauses, deferred_joins;
    uint32_t pad_v11;
    /* V71.7-r4 appended split per-stage telemetry (offset >= 376; preserves v11 offsets 0..375) */
    _Atomic uint32_t last_copy_us, last_lut_us, last_hdf_us, last_grain_us, last_clean_us;
    _Atomic uint32_t live_reuse_hits, live_reuse_misses, smp_helpers_online, force_retires, reserved_v12;
} Layer0Metrics;

enum {
    LAYER0_EVIDENCE_COLS = 32,
    LAYER0_EVIDENCE_ROWS = 24,
    LAYER0_QUAD_BYTES    = 12
};

typedef struct {
    _Atomic uint32_t ready;
    uint32_t generation;
    uint64_t stamp;
    uint32_t copy_mode;
    uint32_t reserved;
    uint8_t quads[LAYER0_EVIDENCE_ROWS][LAYER0_EVIDENCE_COLS][LAYER0_QUAD_BYTES];
} Layer0Evidence;

typedef struct {
    uint32_t version, bytes;
    _Atomic uint32_t off_calls, cancel_calls, before_mask, after_mask, elapsed_us, result;
} RearPowerMetrics;

_Static_assert(offsetof(Layer0Metrics, last_captured_ms) == 200 &&
               offsetof(Layer0Metrics, sentinel_status) == 264 &&
               offsetof(Layer0Metrics, neon_active) == 268 &&
               offsetof(Layer0Metrics, active_route) == 272 &&
               offsetof(Layer0Metrics, lcd_hw_current) == 296 &&
               offsetof(Layer0Metrics, evf_vsync_seq) == 304 &&
               offsetof(Layer0Metrics, helper_sentinel) == 312 &&
               offsetof(Layer0Metrics, wait_no_source) == 328 &&
               offsetof(Layer0Metrics, frame_us) == 352 &&
               offsetof(Layer0Metrics, last_copy_us) == 376 &&
               offsetof(Layer0Metrics, last_lut_us) == 380 &&
               offsetof(Layer0Metrics, last_hdf_us) == 384 &&
               offsetof(Layer0Metrics, last_grain_us) == 388 &&
               offsetof(Layer0Metrics, last_clean_us) == 392 &&
               offsetof(Layer0Metrics, live_reuse_hits) == 396 &&
               offsetof(Layer0Metrics, live_reuse_misses) == 400 &&
               offsetof(Layer0Metrics, smp_helpers_online) == 404 &&
               offsetof(Layer0Metrics, force_retires) == 408 &&
               sizeof(Layer0Metrics) == 416,
               "version 12 metrics ABI (v11 offsets 0..375 preserved)");
_Static_assert(offsetof(Layer0Evidence, quads) == 24 && sizeof(Layer0Evidence) == 9240,
               "version 6 evidence ABI");

/* Layer 0 status codes (compatible with upstream FxResult + REAR_STREAMING). */
enum V71r7Layer0Status {
    V71R7_L0_OK        = 0,
    V71R7_L0_ARGUMENT  = 1,
    V71R7_L0_STATE     = 2,
    V71R7_L0_BUSY      = 3,
    V71R7_L0_EMPTY     = 4,
    V71R7_L0_TIMEOUT   = 5,
    V71R7_L0_STALE     = 6,
    V71R7_L0_BUDGET    = 7,
    V71R7_L0_INTEGRITY = 8,
    V71R7_L0_CLOSE     = 9
};

/* Unified WYSIWYG stage bitmask (V71R7_L0_STAGE_LUT/HDF/GRAIN) defined in v71r7_lut_abi.h. */

/* 512 KiB HDF scratch size carved from unused stage_cpu bank after source copy. */
#define V71R7_LAYER0_HDF_SCRATCH_BYTES  524288U

/* FilmSim WYSIWYG preview state snapshot queried by layer0_provider. */
typedef struct {
    uint32_t active;       /* Non-zero if HDF or Zf Grain WYSIWYG preview is active */
    uint8_t  hdf_level;    /* 0=Off, 1=Weak, 2=Medium, 3=Strong */
    uint8_t  zf_intensity; /* 0=Off, 1..6=Nikon Zf Grain intensity */
    uint8_t  xm6_mode;     /* 0=Auto (prefer XM6 Slot 41 on LCD), 1=ARM Only */
    uint8_t  monochrome;   /* 0=Color, 1=Monochrome HDF */
    uint32_t cfg_version;  /* Monotonic FilmSim/Per-NP3 config version */
} V71r7WysiwygState;

/* Clean provider callback table so filmsim_runtime.c plugs directly into
 * layer0_render_bank() during Stage 1 integration with zero coupling. */
typedef struct {
    void *ctx;
    /* Non-blocking query of active FilmSim WYSIWYG config. Returns 1 if active, 0 if off. */
    int (*query_state)(void *ctx, V71r7WysiwygState *out);

    /* Optional: non-zero while Still JPEG capture/processing owns XM6 or high-prio pipeline. */
    int (*is_still_capturing)(void *ctx);

    /* Stage 1 — Ricoh HDF in-place on NV12 output bank.
     * - LCD (is_evf==0, 1024x682): tries non-blocking XM6 Slot 41 LIVE_256_YC when
     *   y_dma != 0 && xm6_mode == 0, else ARM NEON fallback using scratch_512k.
     * - EVF (is_evf==1, 1280x852): runs ARM NEON / SMP using scratch_512k.
     * Returns 0 if applied, >0 if bypassed, <0 on cancel/error. */
    int (*apply_hdf_nv12)(void *ctx,
                          uint8_t *y_cpu, uint8_t *uv_cpu,
                          uintptr_t y_dma, uintptr_t uv_dma,
                          uint32_t width, uint32_t height,
                          uint32_t y_stride, uint32_t uv_stride,
                          uint8_t hdf_level, uint8_t xm6_mode, uint8_t monochrome,
                          int is_evf,
                          uint8_t *scratch_512k, uint32_t scratch_bytes,
                          int (*cancelled)(void *), void *cancel_ctx);

    /* Stage 2 — Nikon Zf Film Grain in-place on NV12 luma plane (512x256 GTILE NEON < 0.5 ms).
     * Returns 0 if applied, >0 if bypassed/not-ready, <0 on error. */
    int (*apply_grain_nv12)(void *ctx,
                            uint8_t *y_cpu,
                            uint32_t width, uint32_t height,
                            uint32_t y_stride);
} V71r7WysiwygOps;

/* Verified stock ROM hook site metadata (exported in .rodata). */
typedef struct {
    uint32_t patch_va;
    uint32_t stock_target_va;
    uint32_t expected_insn;
    uint32_t role_id;
} V71r7HookSpec;

extern const V71r7HookSpec v71r7_layer0_hook_specs[6];

/* --- V71.7 Layer 0 Integration & Control API (extends v71r7_lut_abi.h) --- */

/* Register/clear FilmSim WYSIWYG provider callbacks (pass NULL to unregister). */
int  v71r7_layer0_set_wysiwyg_ops(const V71r7WysiwygOps *ops);

/* Optional push notification when FilmSim WYSIWYG state toggles without polling callback. */
void v71r7_layer0_notify_wysiwyg_changed(int hdf_level, int zf_intensity, uint32_t cfg_version);

/* Permanent lifecycle stop (sets FX_Z8_LUT_IO_STOPPED so late POWER_CANCEL_SYNC cannot reopen). */
void v71r7_layer0_lifecycle_stop(void);

/* Optional GUI poll helper to clear mode_ok/layout_ticket immediately when leaving shooting view. */
void v71r7_layer0_gui_poll(void);

/* Diagnostic & runtime configuration accessors. */
uint32_t v71r7_layer0_is_presented(void);
uint32_t v71r7_layer0_set_display_mode(uint32_t mode);
uint32_t v71r7_layer0_set_copy_mode(uint32_t mode);
uint32_t v71r7_layer0_set_source_lag(uint32_t lag);
int      v71r7_layer0_workspace_allocated(void);
uint32_t v71r7_layer0_last_stages_applied(void);
const Layer0Metrics    *v71r7_layer0_metrics_ptr(void);
const RearPowerMetrics *v71r7_layer0_power_metrics_ptr(void);
const Layer0Evidence   *v71r7_layer0_evidence_ptr(void);
void                    v71r7_layer0_lock_stats(uint32_t *waits_out, uint32_t *wait_ok_out);

/* --- 6 Verified-Clean Stock ROM Hook Entry Points --- */
uint32_t v71r7_layer0_enqueue_hook(uint32_t route, const void *pkt, const void *rect,
                                   const void *stamp, void *cb);
uint32_t v71r7_layer0_display_hook(uint32_t device, void *layer_desc);
void     v71r7_layer0_vsync_hook(uint32_t route);
void     v71r7_power_sync_hook(uint32_t command, uint64_t payload);

/* C implementations targeted by lut_hooks.S (AArch64 EL1) and direct wrappers (host). */
uint32_t v71r7_layer0_enqueue_hook_c(uint32_t route, const void *pkt, const void *rect,
                                     const void *stamp, void *cb);
uint32_t v71r7_layer0_display_hook_c(uint32_t device, void *layer_desc);
void     v71r7_layer0_vsync_hook_c(uint32_t route);
void     v71r7_power_sync_hook_c(uint32_t command, uint64_t payload);

#ifdef V71R7_HOST_TEST
/* Host ASan+UBSan mock hooks implemented in test_layer0_host.c (zero globals in library). */
void     *v71r7_host_heap_alloc(uint32_t bytes, uint32_t flags);
void      v71r7_host_heap_free(void *ptr);
uint64_t  v71r7_host_ticks(void);
uint64_t  v71r7_host_freq(void);
int32_t   v71r7_host_sleep(uint32_t ms);
void     *v71r7_host_image_copy(void *dst, const void *src, size_t bytes);

uint32_t  v71r7_host_panel_bank(void);
uint64_t  v71r7_host_current_view(void);
uint64_t  v71r7_host_shooting_view(void);
uint32_t  v71r7_host_gui_lv_canvas(void);
uint64_t  v71r7_host_gui_lv_bounds(void);
uint32_t  v71r7_host_view_id(uintptr_t view);
uint32_t  v71r7_host_view_state(uintptr_t view);
uint32_t  v71r7_host_read_property(uint32_t prop, uint32_t *out);

uint32_t  v71r7_host_disp_status(void);
uint32_t  v71r7_host_disp_state(void);
uint32_t  v71r7_host_disp_layer0_enabled(void);
uint8_t   v71r7_host_disp_shadow_flag(void);
uint8_t   v71r7_host_disp_update_enabled(void);
uintptr_t v71r7_host_disp_crop_origin_ptr(void);
uint64_t  v71r7_host_disp_cache_desc_y(void);
uint64_t  v71r7_host_disp_cache_desc_uv(void);
uint64_t  v71r7_host_disp_programmed(void);

int       v71r7_host_pool_lock_timed(uint32_t tmout_ms);
void      v71r7_host_pool_unlock(void);
uint32_t  v71r7_host_gui_release_mode(void);
int       v71r7_host_pool_owns_source(uintptr_t y, uintptr_t *rec_io,
                                      uintptr_t *owner_io, uint32_t *slot_io);
int       v71r7_host_source_planes_valid(uintptr_t y, uintptr_t uv,
                                         uint32_t active_y_bytes, uint32_t uv_offset);
uintptr_t v71r7_host_stock_complete_cb(void);

uint32_t  v71r7_host_stock_enqueue(uint32_t route, const void *pkt, const void *rect,
                                   const void *stamp, void *cb);
uint32_t  v71r7_host_stock_display(uint32_t device, void *layer_desc);
void      v71r7_host_stock_vsync(uint32_t route);
void      v71r7_host_stock_power_sync(uint32_t command, uint64_t payload);
#endif

#endif /* V71R7_LAYER0_PROVIDER_H */
