/* SPDX-License-Identifier: MIT
 * V70 Silky30fps AArch64 NEON Direct NV12 / I420 Scaler, Copy & Preview Kernel.
 *
 * Linked into .filmsim (0x409B0000..0x409D8000) as live_scale_neon.c.o.
 * Called EXCLUSIVELY from CPU0 Live worker task context (NEVER from any stock hook or ISR).
 * Freestanding C11, pure integer NEON (0 FP instructions, 0 .data/.bss, 0 libc deps).
 */
#ifndef LIVE_SCALE_NEON_H
#define LIVE_SCALE_NEON_H
#include <stddef.h>
#include <stdint.h>

/* Scale 1024x{682|680} NV12 -> 1280x852 NV12 directly into Wave5 slot (with bottom-row
 * replication up to coded_h, e.g. 864, and optional strip dc cvac + final dsb sy).
 * Returns 1 on success, 0 on invalid args. */
int live_scale_1024_to_1280_nv12_neon(
    uint8_t *dst_y, uint8_t *dst_uv, unsigned dst_y_stride, unsigned dst_uv_stride,
    unsigned coded_h, const uint8_t *src_y, const uint8_t *src_uv,
    unsigned src_h, unsigned src_y_stride, unsigned src_uv_stride,
    int lv_cached, int clean_dcache);

/* V71: Scale 1024x{682|680} NV12 -> 1280x852 NV12 with fused 512x256 twisted-torus
 * (twist_dx=192) int8_t luma grain tile applied in-register during vertical blend
 * (zero extra Y DRAM reads/writes; grain_tile == NULL behaves identically to plain). */
int live_scale_1024_to_1280_nv12_neon_grain(
    uint8_t *dst_y, uint8_t *dst_uv, unsigned dst_y_stride, unsigned dst_uv_stride,
    unsigned coded_h, const uint8_t *src_y, const uint8_t *src_uv,
    unsigned src_h, unsigned src_y_stride, unsigned src_uv_stride,
    int lv_cached, int clean_dcache, const int8_t *grain_tile);

/* Scale 1024x{682|680} NV12 -> 1280x852 planar I420 (for legacy callers).
 * Returns 1 on success, 0 on invalid args. */
int live_scale_1024_to_1280_i420_neon(
    uint8_t *dst_i420, const uint8_t *src_y, const uint8_t *src_uv,
    unsigned src_h, unsigned src_y_stride, unsigned src_uv_stride, int lv_cached);

/* 1:1 NV12 copy into Wave5 slot (for EVF 1280x852 or matching geometry) with bottom-row
 * replication to coded_h and optional strip dc cvac + final dsb sy. Returns 1 on success. */
int live_copy_nv12_neon(
    uint8_t *dst_y, uint8_t *dst_uv, unsigned w, unsigned h,
    unsigned dst_y_stride, unsigned dst_uv_stride, unsigned coded_h,
    const uint8_t *src_y, const uint8_t *src_uv,
    unsigned src_y_stride, unsigned src_uv_stride,
    int lv_cached, int clean_dcache);

/* V71: 1:1 NV12 copy with fused 512x256 twisted-torus (twist_dx=192) int8_t luma grain
 * tile (for native 1280x852 EVF with FilmSim grain; grain_tile == NULL behaves identically
 * to live_copy_nv12_neon). */
int live_copy_nv12_neon_grain(
    uint8_t *dst_y, uint8_t *dst_uv, unsigned w, unsigned h,
    unsigned dst_y_stride, unsigned dst_uv_stride, unsigned coded_h,
    const uint8_t *src_y, const uint8_t *src_uv,
    unsigned src_y_stride, unsigned src_uv_stride,
    int lv_cached, int clean_dcache, const int8_t *grain_tile);

/* Fast 1280x852 NV12 -> 1024x682 I420 (LIVE_PREVIEW_W x LIVE_PREVIEW_H) nearest-neighbor
 * downscale for 2.5fps preview ring (Y: 698368 B, U: 174592 B @ +698368, V: 174592 B @ +872960). */
void live_preview_nv12_to_i420_1024x680(
    uint8_t *dst_i420, const uint8_t *src_y, const uint8_t *src_uv,
    unsigned src_w, unsigned src_h, unsigned src_y_stride, unsigned src_uv_stride);

/* V71r2: Fast 5:4 NEON bilinear downscaler for EVF Mode 2 FilmSim (1280x852 NV12 -> 1024x682 NV12)
 * using exact 1/8th horizontal tbl2+umull/umlal/rshrn + 1/128th vertical blend + stnp stores.
 * Portable scalar fallback is 0-diff bit-identical to AArch64 NEON. */
void live_downscale_1280_to_1024_nv12_neon(
    const uint8_t *src_y, const uint8_t *src_uv, uint32_t src_stride,
    uint8_t *dst_y, uint8_t *dst_uv, uint32_t dst_stride);

#endif
