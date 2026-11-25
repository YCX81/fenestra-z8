/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Unified .cube LUT Runtime Context & Stage 0 Dispatcher.
 *
 * Strictly scalar (-mgeneral-regs-only), zero .data/.bss.
 * Mutable state lives in heap-allocated LutCoreCtx (HEAP_ALLOC 0x40D2C408)
 * anchored at v71r7_anchor()->lut_core_ctx.
 */
#ifndef V71R7_LUT_RUNTIME_H
#define V71R7_LUT_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include "v71r7_lut_abi.h"
#include "cube.h"
#include "smp.h"

struct z8_xm6_bridge;

/* Copy active compiled 1D separable XM6 curves (returns 1 if active & 1D separable) */
int v71r7_lut_get_active_curves(uint16_t curves_out[3][256], uint8_t source_out[3],
                                uint32_t *gen_out);

/* Query active LUT relative path (returns 1 if active) */
int v71r7_lut_get_active_path(char path_out[FX_Z8_LUT_PATH_BYTES]);

/* Bind optional XM6 Core 0 bridge for 1D separable Opcode 12 (RGBLUT) Still acceleration */
void v71r7_lut_bind_xm6(struct z8_xm6_bridge *bridge, uint8_t xm6_mode);

/* Out-of-place Still NV16 apply: uses XM6 Opcode 12 (RGBLUT) when active LUT is 1D
 * separable and XM6 bridge is bound + ready; transparently falls back to SMP+NEON/Scalar.
 * Returns: 2 = XM6 Op12 used, 1 = ARM SMP+NEON/Scalar used, 0 = inactive/error. */
int v71r7_lut_apply_still_nv16_oop(const uint8_t *src_y, const uint8_t *src_uv,
                                   uint64_t src_y_dma, uint64_t src_uv_dma,
                                   uint8_t *dst_y, uint8_t *dst_uv,
                                   uint64_t dst_y_dma, uint64_t dst_uv_dma,
                                   uint32_t width, uint32_t height,
                                   uint32_t y_stride, uint32_t uv_stride,
                                   uint32_t *cache_words_4mib, uint32_t *cache_gen_inout);

/* Read last execution path telemetry for testing & diagnostics */
typedef struct {
    uint32_t prepare_ok_cnt;
    uint32_t prepare_fail_cnt;
    uint32_t nv16_neon_calls;
    uint32_t nv16_scalar_calls;
    uint32_t nv16_xm6_calls;
    uint32_t nv12_neon_calls;
    uint32_t nv12_scalar_calls;
    uint32_t cache_clears;
} V71r7LutRuntimeStats;

void v71r7_lut_runtime_get_stats(V71r7LutRuntimeStats *out);

#ifdef V71R7_HOST_TEST
/* Cleanly stops SMP threads and frees LutCoreCtx + active JpegCube for ASan/LSan tests */
void v71r7_lut_runtime_shutdown_for_test(void);
#endif

#endif /* V71R7_LUT_RUNTIME_H */
