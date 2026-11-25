/* SPDX-License-Identifier: MIT
 * Z8 FilmSim EL1 Runtime Coordinator (V66 baseline, zero-invasive Fenestra Core).
 * Freestanding C11, scalar-only (-mgeneral-regs-only), zero .data/.bss.
 */
#ifndef FILMSIM_RUNTIME_H
#define FILMSIM_RUNTIME_H

#include <stdint.h>
#include "z8_filmsim_abi.h"

struct z8_native_filmsim_photo;
struct filmsim_runtime_context;

/* Polled periodically by mux_worker (CPU2 prio 31, never GUI/IRQ/CPU0 capture).
 * Strict Dormant Bypass: allocates 0 bytes when filmsim.wlm is unloaded OR
 * when all stages are Off (film_style == OFF && hdf_level == OFF). */
void filmsim_runtime_ui_poll(void);

/* Shutdown / power-off (event 7) notification: stops workers & cancels in-flight work. */
void filmsim_runtime_on_stop(void);

/* Card swap (event 24) notification: invalidates cached card assets safely. */
void filmsim_runtime_on_card_change(void);

/* Pool-layout switch coordination (V69 HangFix integration):
 * - pool_suspend(): cancels active work, releases held class24 arena_blk so
 *   Photo->Movie/Playback layout transitions never see class 0x24 BUSY.
 * - pool_resume(): clears suspend gate so next ui_poll() lazily restores arena. */
void filmsim_runtime_pool_suspend(void);
void filmsim_runtime_pool_resume(void);

/* Query active photo config; returns 1 and writes *out only if non-dormant. */
int filmsim_runtime_photo_config(Z8FilmSimConfig *out, uint32_t *shot_seq_out);

/* Return photo controller handle only when photo worker is active & ready; else NULL.
 * filmsim_runtime_photo_slot(0) == filmsim_runtime_photo(); slot 1 is the V71.5 burst slot. */
struct z8_native_filmsim_photo *filmsim_runtime_photo(void);
struct z8_native_filmsim_photo *filmsim_runtime_photo_slot(unsigned slot_idx);

/* V71.3 Live FilmSim operating modes returned by filmsim_runtime_live_mode():
 *   0 = OFF            (normal Direct-NV12 Live, 0 FilmSim overhead)
 *   1 = GRAIN_ONLY_CPU0 (CPU0 fuses baked 512x256 Zf grain tile into Wave5 NV12 scaler/copy; 0-lag 30fps!)
 *   2 = FULL_CPU2_NV12  (3-slot lock-free NV12 triple-buffer graded by CPU2 Ricoh HDF + optional CPU0 fused Zf grain) */
#define FILMSIM_LIVE_MODE_OFF        0
#define FILMSIM_LIVE_MODE_GRAIN_ONLY 1
#define FILMSIM_LIVE_MODE_FULL_CPU2  2

/* Non-blocking V71 Native-NV12 Async Live API called by live_stream.c (CPU0 worker, zero stall):
 * - live_mode(): returns FILMSIM_LIVE_MODE_{OFF,GRAIN_ONLY,FULL_CPU2}.
 * - live_active(): returns 1 iff live_mode() != 0.
 * - live_begin_submit_nv12(): lock-free O(1) claim of write slot in 3-slot NV12 triple-buffer.
 *   Returns slot index 0..2 on success (caller MUST call commit_submit_nv12), or -1 if unavailable.
 * - live_commit_submit_nv12(): commit=1 publishes PENDING_RAW for CPU2; commit=0 aborts (torn/cancel).
 * - live_acquire_nv12(): promotes GRADED_READY->CPU0_FRONT, borrows NV12 pointers + frame gen +
 *   optional baked 512x256 grain_tile (NULL if grain Off/baking). Returns 1 if acquired (caller MUST
 *   call live_release()), 0 during first-frame warmup.
 * - live_acquire_grain_tile(): Mode 1 non-blocking borrow of baked 512x256 grain tile (returns
 *   non-NULL -> caller MUST call live_release(); returns NULL if not baked yet, no release needed).
 * - live_release(): releases borrow immediately after NEON scale/copy into Wave5 slot.
 * - live_is_busy(): lock-free atomic check whether CPU2 triple-buffer has a
 *   PENDING_RAW, CPU2_GRADING, or freshly published GRADED_READY slot (V71.3). */
int            filmsim_runtime_live_mode(void);
int            filmsim_runtime_live_active(void);
uint32_t       filmsim_runtime_live_is_busy(void);
int            filmsim_runtime_live_begin_submit_nv12(unsigned width, unsigned height,
                                                      uint8_t **y_out, uint8_t **uv_out,
                                                      unsigned *y_stride_out, unsigned *uv_stride_out);
void           filmsim_runtime_live_commit_submit_nv12(int slot, unsigned width, unsigned height,
                                                       uint64_t source_us, int commit);
int            filmsim_runtime_live_acquire_nv12(const uint8_t **y_out, const uint8_t **uv_out,
                                                 unsigned *w_out, unsigned *h_out,
                                                 uint64_t *gen_out, const int8_t **grain_tile_out);
const int8_t  *filmsim_runtime_live_acquire_grain_tile(void);
void           filmsim_runtime_live_release(void);

/* V71.7-r4 Single-Pass Live->LCD Shared Frame Producer:
 * Copies a fresh, unseen, matching Mode 2 Live-graded 1024x{680,682} NV12 frame from LBUF
 * into Layer 0 BANK_WORKING under filmsim_reader_enter() + single-writer seqlock + per-slot
 * atomic pin (zero tearing, zero UAF, zero extra Class 0x24). LBUF never has Zf grain written
 * to it, so *out_stages_applied uses V71R7_L0_STAGE_* bits (0x1=LUT, 0x2=HDF; bit2 GRAIN 0).
 * Freshness & *out_age_us are measured from the original capture timestamp (lv_slot_us).
 * Returns:
 *   1 on hit (new seq copied; *out_stages_applied, *out_seq, *out_age_us set),
 *  -1 if the latest fresh matching frame has seq == last_seq (caller keeps current frame),
 *   0 on miss/stale/mismatch/invalidated (caller grades normally from Pool 0 source). */
int            filmsim_live_try_copy_graded_lcd(uint32_t w, uint32_t h,
                                                uint32_t lut_gen, uint8_t hdf_level,
                                                uint32_t max_age_us,
                                                uint32_t last_seq,
                                                uint8_t *dst_y, uint8_t *dst_uv,
                                                uint32_t dst_stride,
                                                uint32_t *out_stages_applied,
                                                uint32_t *out_seq,
                                                uint64_t *out_age_us);

/* V71.7-r7 Reverse Single-Pass Layer 0 -> Live Shared Frame Feed (EVF 1280x852 & LCD 1024x682):
 * - push_layer0_nv12(): called by Layer 0 strictly AFTER Stage 0 LUT + Stage 1 HDF and BEFORE
 *   Stage 2 Zf Grain. Copies pre-grain graded NV12 directly into LBUF as FS_TB_GRADED_READY
 *   (>= 18 ms cadence) when Live Mode 2 is active and wants the exact same (lut_gen, hdf_level).
 * - layer0_feed_active(): 1 while Layer 0 fed Live within 80 ms (CPU0 skips downscale + submit).
 * - independent_active(): 1 while Live Mode 2 runs independently of Layer 0 (e.g. WYSIWYG Off). */
int            filmsim_runtime_live_push_layer0_nv12(const uint8_t *y, const uint8_t *uv,
                                                     uint32_t w, uint32_t h,
                                                     uint32_t y_stride, uint32_t uv_stride,
                                                     uint64_t capture_us,
                                                     uint32_t lut_gen, uint8_t hdf_level);
int            filmsim_runtime_live_layer0_feed_active(void);
int            filmsim_runtime_live_independent_active(void);

/* Legacy V70 I420 stubs retained for symbol/test compatibility. */
void           filmsim_runtime_live_submit(const uint8_t *i420, unsigned width,
                                           unsigned height, uint64_t source_us);
const uint8_t *filmsim_runtime_live_acquire(unsigned width, unsigned height,
                                            uint64_t *front_us_out);

/* Direct synchronous Live I420 helper (retained for Unicorn / direct tests; uses
 * temporally coherent fixed Live grain seed). */
void filmsim_runtime_apply_live_i420(uint8_t *i420, unsigned width, unsigned height);

/* CNTPCT_EL0 microsecond clock. */
uint64_t filmsim_runtime_time_us(void);

/* EL1 Service ABI handler for Z8_FS_OP_{GET,SET,STATUS,SET_RECIPE,GET_RECIPE,
 * SYNC_RECIPES,XM6_DIAG,XM6_RESET_BREAKER} (ops 1..6, 10, 11).
 * Returns 0 on success, negative error code on invalid argument/state. */
int filmsim_runtime_service_call(uint32_t op,
                                 const void *req, uint32_t req_bytes,
                                 void *resp, uint32_t *resp_bytes);

/* Direct context variant for Unicorn / host regression harnesses. */
int filmsim_runtime_service_call_ctx(struct filmsim_runtime_context *ctx,
                                     uint32_t op,
                                     const void *req, uint32_t req_bytes,
                                     void *resp, uint32_t *resp_bytes);

/* slots.state EL1 IPC router (ops 32..79 -> z8_op 0..47):
 *   32..47 ('FSI1'): filmsim_runtime_service_call(z8_op, ...)
 *   48..63 ('LPI1'): weak live_ipc_service_call(raw_op, ...)
 *   64..79 ('LTI1'): weak v71r7_lut_ipc_service_call(raw_op, ...) */
uint32_t filmsim_slots_svc_dispatch(uint32_t z8_op, const void *q_ptr, void *a_ptr);
void filmsim_slots_svc_trampoline(void);

int live_ipc_service_call(uint32_t raw_op,
                          const void *req, uint32_t req_bytes,
                          void *resp, uint32_t *resp_bytes);
int v71r7_lut_ipc_service_call(uint32_t raw_op,
                               const void *req, uint32_t req_bytes,
                               void *resp, uint32_t *resp_bytes);

#endif /* FILMSIM_RUNTIME_H */
