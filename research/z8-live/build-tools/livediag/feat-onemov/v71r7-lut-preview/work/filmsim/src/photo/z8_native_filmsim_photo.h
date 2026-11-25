/* SPDX-License-Identifier: MIT
 * Whole-shot 6-slot NV16 Native Photo Coordinator for Z8 FilmSim WLM.
 * - Zero touch on NEF RAW sensor/metadata.
 * - 512 B aligned private class24 blocks, strict disjointness validation.
 * - Hardware D-cache discipline: dc ivac per source row, chunked < 0x88000 dc civac + dsb sy
 *   on every modified Y & UV plane before Z8_NFSP_READY and before pool release.
 * - Executes Stage 1 (Ricoh Optical HDF) -> Stage 2 (Nikon Zf Film Grain)
 *   sharing ONE 512 KiB per-shot scratch workspace inside the shot's class24 arena.
 * Freestanding C11, scalar-only (-mgeneral-regs-only), zero malloc, zero .data/.bss.
 */
#ifndef Z8_NATIVE_FILMSIM_PHOTO_H
#define Z8_NATIVE_FILMSIM_PHOTO_H

#include <stddef.h>
#include <stdint.h>
#include "z8_photo_source.h"
#include "z8_filmsim_pipeline.h"

#define Z8_NFSP_MAX_BLOCKS           8u
#define Z8_NFSP_WAIT_ALL             15u
#define Z8_NFSP_USED_CLEANED_BIT     UINT32_C(0x80000000)
#define Z8_NFSP_QUARANTINE_THRESHOLD 3u

enum z8_nfsp_phase {
    Z8_NFSP_IDLE = 0,
    Z8_NFSP_COLLECT,
    Z8_NFSP_PENDING,
    Z8_NFSP_COPY,
    Z8_NFSP_PROCESS,
    Z8_NFSP_READY,
    Z8_NFSP_BYPASS,
    Z8_NFSP_QUARANTINE,
    Z8_NFSP_RETIRING,
    Z8_NFSP_CLAIM,
    Z8_NFSP_PREPARE
};

enum z8_nfsp_reason {
    Z8_NFSP_NONE = 0,
    Z8_NFSP_DISABLED,
    Z8_NFSP_BUSY,
    Z8_NFSP_IDENTITY,
    Z8_NFSP_FENCE,
    Z8_NFSP_FORMAT,
    Z8_NFSP_SOURCE_SPAN,
    Z8_NFSP_ROI,
    Z8_NFSP_POOL,
    Z8_NFSP_CANCELLED,
    Z8_NFSP_KERNEL,
    Z8_NFSP_ENGINE_ERROR,
    Z8_NFSP_CHANGED,
    Z8_NFSP_TIMEOUT,
    Z8_NFSP_ASSET_MISSING
};

struct z8_nfsp_allocation {
    void    *handle;
    uint8_t *cpu;
    uint64_t dma;
    uint32_t actual;
    uint32_t used;
};

struct z8_nfsp_plane {
    uint8_t descriptor[Z8_PHOTO_DESCRIPTOR_BYTES];
    struct z8_photo_descriptor source;
    uint8_t *y;
    uint8_t *uv;
    uint64_t y_dma;
    uint64_t uv_dma;
    uint32_t y_bytes;
    uint32_t uv_bytes;
    uint16_t roi_x;
    uint16_t roi_y;
    uint16_t roi_w;
    uint16_t roi_h;
};

struct z8_nfsp_ops {
    z8_photo_read_fn read;
    int (*allocate)(void *ctx, uint32_t requested, struct z8_nfsp_allocation *out);
    void (*release)(void *ctx, struct z8_nfsp_allocation *blk);
    const uint8_t *(*map_source)(void *ctx, uint64_t dma, uint32_t bytes);
    int (*clean)(void *ctx, uint8_t *cpu, uint32_t bytes);
    int (*pool_valid)(void *ctx);
    void (*yield)(void *ctx);
    uint64_t (*time_us)(void *ctx);
    /* Optional runtime asset lock callbacks (hold card-loaded maps/LUTs immutable across worker) */
    int (*acquire_assets)(void *ctx, z8_filmsim_assets *out_assets);
    void (*release_assets)(void *ctx);
    /* Optional V71 post-roll defer hook: return 1 to defer PENDING->COPY while Live post-roll runs. */
    int (*should_defer)(void *ctx, uint64_t stage_start_us);
    void *context;
};

struct z8_native_filmsim_photo {
    struct z8_nfsp_ops ops;
    uint32_t phase, cancel, active_engines, retire, poisoned, reason, lock, yield_counter, committed;
    uint32_t wait_seen, wait_failed, thumbnail_seen, thumbnail_result;
    uint32_t count, block_count, source_reads_done;
    uint32_t source_gate, cleanup_active;
    uint64_t generation, cancel_epoch, context, outer, metadata;
    uint64_t stage_start_us, copy_done_us, finish_us;
    uint32_t context_word, outer_word;
    uint32_t shot_seq;
    Z8FilmSimConfig cfg;
    uint32_t yield_slice_us; /* occupies former 4B alignment pad -> sizeof & trailing offsets 100% unchanged */
    struct z8_photo_snapshot before_border, snapshot;
    uint8_t role_plane[Z8_PHOTO_SOURCE_COUNT];
    struct z8_nfsp_plane planes[Z8_PHOTO_SOURCE_COUNT];
    struct z8_nfsp_allocation blocks[Z8_NFSP_MAX_BLOCKS];
    z8_filmsim_job job;
    void *workspace;
    int kernel_result;
    uint32_t last_stages_applied;
    uint32_t last_main_w;
    uint32_t last_main_h;
    uint32_t photos_completed;
    uint32_t photos_bypassed;
    uint32_t photos_failed;
    uint32_t job2_grain_bypasses; /* appended: earlier offsets unchanged for metadata ABI */
};

enum z8_nfsp_engine_slot {
    Z8_NFSP_SLOT_DEFAULT     = 0u, /* Main / Secondary / Medium / Small / Thumbnail */
    Z8_NFSP_SLOT_NEF_PREVIEW = 1u  /* Job 2 (0x40eaf554): fixed 7.04 MiB NEF Basic preview */
};

/* Returns 1 if Job 2 (0x40eaf554, fixed 7,384,576 B Basic NEF preview buffer) must
 * bypass the grained private descriptor to guarantee zero EXPEED 7 engine 15e2
 * output-limit overflow (err 85 at 0x40e82210). Returns 0 when grain is inactive
 * (pure Ricoh HDF still applies to Job 2). */
int      z8_nfsp_job2_must_bypass(const Z8FilmSimConfig *cfg);

void     z8_nfsp_init(struct z8_native_filmsim_photo *p, const struct z8_nfsp_ops *ops);
int      z8_nfsp_begin(struct z8_native_filmsim_photo *p, uint64_t ctx,
                       const Z8FilmSimConfig *cfg, uint32_t shot_seq);
void     z8_nfsp_wait_return(struct z8_native_filmsim_photo *p, uint64_t outer,
                             uint32_t event, uint64_t result);
void     z8_nfsp_thumbnail_return(struct z8_native_filmsim_photo *p, uint64_t outer,
                                  uint64_t result);
uint64_t z8_nfsp_context_generation(struct z8_native_filmsim_photo *p, uint64_t ctx);
int      z8_nfsp_prepare_generation(struct z8_native_filmsim_photo *p, uint64_t ctx,
                                    uint64_t g, uint64_t start_us, uint64_t timeout_us);
void     z8_nfsp_worker(struct z8_native_filmsim_photo *p);
void     z8_nfsp_cancel(struct z8_native_filmsim_photo *p);
void     z8_nfsp_reclaim_cancelled(struct z8_native_filmsim_photo *p);
int      z8_nfsp_class24_busy(struct z8_native_filmsim_photo *p);
void     z8_nfsp_cancel_generation(struct z8_native_filmsim_photo *p, uint64_t g);
void     z8_nfsp_timeout_generation(struct z8_native_filmsim_photo *p, uint64_t g);
int      z8_nfsp_poll_generation(struct z8_native_filmsim_photo *p, uint64_t g);
int      z8_nfsp_engine_begin_ex(struct z8_native_filmsim_photo *p, uint64_t outer,
                                 const void *original_params, void *local_params,
                                 void *local_descriptor, uint32_t engine_slot);
int      z8_nfsp_engine_begin(struct z8_native_filmsim_photo *p, uint64_t outer,
                              const void *original_params, void *local_params,
                              void *local_descriptor);
void     z8_nfsp_engine_end(struct z8_native_filmsim_photo *p, uint64_t result);
void     z8_nfsp_retire(struct z8_native_filmsim_photo *p, uint64_t outer);
uint64_t z8_nfsp_cleanup_begin(struct z8_native_filmsim_photo *p, uint64_t outer);
int      z8_nfsp_cleanup_poll(struct z8_native_filmsim_photo *p, uint64_t g);
void     z8_nfsp_cleanup_end(struct z8_native_filmsim_photo *p, uint64_t g);

/* Concrete Z8 C3.11 / V66 platform ops & shared class24 helpers */
void z8_nfsp_v66_ops(struct z8_nfsp_ops *ops);
int  z8_fs_plat_pool_valid(void);
int  z8_fs_plat_alloc_class24(uint32_t requested, struct z8_nfsp_allocation *out);
int  z8_fs_plat_alloc_class24_xm6(uint32_t requested, struct z8_nfsp_allocation *out);
void z8_fs_plat_release_class24(struct z8_nfsp_allocation *blk);
int  z8_fs_plat_clean_dcache(uint8_t *cpu, uint32_t bytes);
uint64_t z8_fs_plat_time_us(void);

#endif /* Z8_NATIVE_FILMSIM_PHOTO_H */
