/* SPDX-License-Identifier: MIT
 * Nikon Z8 C3.11 EXPEED 7 CEVA-XM6 DSP — Ricoh Optical HDF v2 Reference Kernel (Core 0 Slot 41)
 *
 * Clean-room, standalone, zero-dependency, pure-integer C11 kernel aligned 100% bit-for-bit
 * (0-diff) with Z8 V71.5 production EL1 engine (work/filmsim/src/hdf/z8_hdf.{h,c} + z8_hdf_kernels.{h,c}):
 *   1. STILL_GENERAL (up to 8256x5504 NV16 / NV12, 45.4MP):
 *        - Stage 1: V71.5 strided 4x4 (b>=12) / 2x2 (6<=b<12) / 1x1 (b<6) highlight extract + block accum
 *        - Stage 2: 100% IDM-resident single-channel 3-scale optical pyramid + row-major O(1) V-blur
 *        - Stage 3: Fused 512-px tiled Q15 hlerp + dark zero-halo fast-path + knot16 + shoulder + clz_umull64
 *   2. LIVE_256_YC (1024x{680,682} NV12 @ 30fps):
 *        - Stage 1: Adaptive 256/512 chunk highlight extract + edge masks + 256x171 RGB cache
 *        - Stage 2: 100% IDM-resident Q13 sigma-matched sym53 + box16(r=3,4,5,5) + k-fold + grid_y
 *        - Stage 3: 256x171 direct chroma halo delta + masked exact 512x341 edge fallback + full-res Y rowpair
 *
 * Hard CEVA-XM6 Core 0 (z8_xm6_elf0.elf) constraints strictly obeyed:
 *   - ZERO floating-point operations or FP library calls.
 *   - ZERO malloc / free / dynamic allocation.
 *   - ZERO writable static / global variables (.data = 0 B, .bss = 0 B).
 *   - Explicit caller-supplied IDM (<= 192 KiB = FW_APP_WORKBUF @ 0x00008000) and
 *     EXT DRAM (<= 512 KiB = FW_EXT_WORKBUF @ 0x41930000) workspaces.
 *   - Explicit XM6_DMA_SIM hooks (1D & 2D EXT<->IDM) + optional Z8Xm6HdfProfile for
 *     measuring pure VPU compute cycles vs. DMA-included cycles in xm6sim.
 *   - Binary wire layout of Z8Xm6ImageParam (0x38 B) & Z8Xm6HdfParams (0x60 B) is 100%
 *     identical to tagDSPImageParam & TRicohHdfInputParam in Z8-XM6-DSP-Porting-Kit-v1.
 */
#ifndef RICOH_HDF_XM6_V2_H
#define RICOH_HDF_XM6_V2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Nikon EXPEED 7 CEVA-XM6 Core 0 return codes */
#define Z8_XM6_DSP_OK                   0
#define Z8_XM6_DSP_ERR_INVALID_PARAM    0x10000001

/* Nikon EXPEED 7 tagDSPImageParam::format codes */
#define Z8_XM6_FMT_Y_ONLY               0x10U
#define Z8_XM6_FMT_NV16                 0x30U /* 4:2:2 semi-planar Y + interleaved CbCr */
#define Z8_XM6_FMT_NV12                 0x40U /* 4:2:0 semi-planar Y + interleaved CbCr */

/* Ricoh HDF optical strength levels (exact match to enum z8_hdf_level) */
#define Z8_XM6_HDF_LEVEL_OFF            0U
#define Z8_XM6_HDF_LEVEL_WEAK           1U
#define Z8_XM6_HDF_LEVEL_MEDIUM         2U
#define Z8_XM6_HDF_LEVEL_STRONG         3U

/* Engine selection mode in Z8Xm6HdfParams::mode */
#define Z8_XM6_HDF_MODE_AUTO            0U /* 1024x{680,682} -> LIVE_256_YC; 1280x{850..856} -> LIVE_EVF_256_YC; else STILL */
#define Z8_XM6_HDF_MODE_LIVE_256_YC     1U /* Force 1024x{680,682} NV12 Live fast path */
#define Z8_XM6_HDF_MODE_STILL_GENERAL   2U /* Force general WxH NV16/NV12 45.4MP path */
#define Z8_XM6_HDF_MODE_LIVE_EVF_256_YC 3U /* Force 1280x{850,852,854,856} NV12 EVF fast path */

/* Exact Z8 C3.11 ELF0 stock scratch capacities (zero extra ARM64 scratch allocation!) */
#define Z8_XM6_HDF_IDM_ADDR             0x00008000U /* FW_APP_WORKBUF */
#define Z8_XM6_HDF_IDM_BYTES            196608U     /* 192 KiB zero-wait IDM */
#define Z8_XM6_HDF_EXT_ADDR             0x41930000U /* FW_EXT_WORKBUF */
#define Z8_XM6_HDF_EXT_BYTES            524288U     /* 512 KiB external DRAM scratch */

#define Z8_XM6_HDF_MAX_WIDTH            8256U
#define Z8_XM6_HDF_MAX_HEIGHT           8256U

/* Exact 0x38-byte Nikon EXPEED 7 tagDSPImageParam wire layout.
 * On 32-bit CEVA-XM6, plane_addr[i] carries the 32-bit DSP IOMMU VA in the low 32 bits (hi=0).
 * On 64-bit host, plane_addr[i] carries the full 64-bit pointer seamlessly.
 */
typedef struct {
    uint64_t plane_addr[3]; /* +0x00, +0x08, +0x10: [0]=Y, [1]=CbCr, [2]=unused */
    uint32_t roi_x;         /* +0x18: normally 0 */
    uint32_t roi_y;         /* +0x1c: normally 0 */
    int32_t  width;         /* +0x20: width in pixels (multiple of 4, <= 8256) */
    int32_t  height;        /* +0x24: height in rows (<= 8256) */
    int32_t  stride[3];     /* +0x28, +0x2c, +0x30: row stride in bytes */
    uint32_t format;        /* +0x34: Z8_XM6_FMT_NV16 (0x30) or Z8_XM6_FMT_NV12 (0x40) */
} Z8Xm6ImageParam;

/* Unified 0x60-byte CEVA-XM6 Slot 41 Ricoh HDF Parameter Block.
 * 100% binary-layout compatible with TRicohHdfInputParam in Z8-XM6-DSP-Porting-Kit-v1.
 * Passed via Path A (0x40c90aec, zero ARM64 DSP-driver patches) with pre-translated
 * 32-bit DSP IOMMU VAs (via 0x40c91c84(va, {0x28, 0x2f})) and cleaned D-cache (0x40d2b9c8).
 */
typedef struct {
    Z8Xm6ImageParam src;            /* +0x00 .. +0x37 (56 B): input Y/CbCr plane descriptor */
    int32_t         bypass_flag;    /* +0x38: 1 = immediate bypass return 0 */
    uint8_t         level;          /* +0x3c: 0=OFF, 1=WEAK, 2=MEDIUM, 3=STRONG */
    uint8_t         monochrome;     /* +0x3d: 0=color, 1=monochrome */
    uint8_t         mode;           /* +0x3e: Z8_XM6_HDF_MODE_* */
    uint8_t         reserved_3f;    /* +0x3f: must be 0 */
    uint16_t        knee_q16;       /* +0x40: 0 = preset default (22528) */
    uint16_t        neutrality_q8;  /* +0x42: 0..256 (0xFFFF = default 96) */
    uint16_t        ev_gain_q8;     /* +0x44: 0 = default 256 (1.0x) */
    uint8_t         white[3];       /* +0x46..+0x48: WB white-point (0 -> 255) */
    uint8_t         reserved_49[7]; /* +0x49..+0x4f: must be 0 */
    uint64_t        ext_work_addr;  /* +0x50: 0 = use FW_EXT_WORKBUF (0x41930000) */
    uint32_t        ext_work_bytes; /* +0x58: >= 524288 when ext_work_addr != 0 */
    uint32_t        reserved_5c;    /* +0x5c: must be 0 */
} Z8Xm6HdfParams;

typedef Z8Xm6ImageParam Z8Xm6HdfOutParam;

/* Backwards-compatible aliases with Z8-XM6-DSP-Porting-Kit-v1 */
typedef Z8Xm6HdfParams   TRicohHdfInputParam;
typedef Z8Xm6HdfOutParam TRicohHdfOutputParam;

/* Compile-time wire-layout verification */
typedef char z8_xm6_hdf_chk_img[(sizeof(Z8Xm6ImageParam) == 0x38U) ? 1 : -1];
typedef char z8_xm6_hdf_chk_in[(sizeof(Z8Xm6HdfParams) == 0x60U) ? 1 : -1];
typedef char z8_xm6_hdf_chk_out[(sizeof(Z8Xm6HdfOutParam) == 0x38U) ? 1 : -1];

/* Explicit DMA abstraction (XM6_DMA_SIM / NikonDMA 1D & 2D adapter).
 * - Pass dma_ops == NULL for zero-overhead flat-memory memcpy (pure compute profiling).
 * - Provide dma_1d_* (and optionally dma_2d_*) to instrument exact DMA byte/call counts,
 *   simulate AXI/EDMA latency in xm6sim (-DXM6_DMA_SIM=1), or bind hardware NikonDMA
 *   (CreateDescriptor1D @ 0x23c0, CreateDescriptor2D @ 0x2480, DispatchRequestNoSync @ 0x2560,
 *   DispatchRequest @ 0x2960, WaitRequest @ 0x2ac0) on Z8 Core 0.
 */
typedef struct {
    void *ctx;
    void (*dma_1d_ext_to_idm)(void *ctx, void *idm_dst, uint64_t ext_src_addr, uint32_t bytes);
    void (*dma_1d_idm_to_ext)(void *ctx, uint64_t ext_dst_addr, const void *idm_src, uint32_t bytes);
    /* Optional 2D strip hooks (NULL -> automatic row-loop fallback via dma_1d_*) */
    void (*dma_2d_ext_to_idm)(void *ctx, void *idm_dst, uint32_t idm_stride,
                              uint64_t ext_src_addr, uint32_t ext_stride,
                              uint32_t width_bytes, uint32_t height_rows);
    void (*dma_2d_idm_to_ext)(void *ctx, uint64_t ext_dst_addr, uint32_t ext_stride,
                              const void *idm_src, uint32_t idm_stride,
                              uint32_t width_bytes, uint32_t height_rows);
} Z8Xm6DmaOps;

/* Optional per-invocation profiling & telemetry record (pass NULL in firmware Slot 41).
 * Populated without any static/global state so the kernel remains 100% reentrant.
 */
typedef struct {
    uint64_t stage1_cycles;
    uint64_t stage2_cycles;
    uint64_t stage3_cycles;
    uint64_t total_cycles;
    uint64_t dma_ext_to_idm_bytes;
    uint64_t dma_idm_to_ext_bytes;
    uint32_t dma_ext_to_idm_calls;
    uint32_t dma_idm_to_ext_calls;
    uint32_t dark_fastpath_tiles;
    uint32_t dark_skipped_rows;
    uint32_t total_s3_tiles;
    uint32_t engine_used; /* 1 = LIVE_256_YC, 2 = STILL_GENERAL */
    uint32_t grid_b;
    uint32_t s1_step;
    uint32_t gw;
    uint32_t gh;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
} Z8Xm6HdfProfile;

/* Initialize parameter block with Ricoh HDF optical defaults */
void z8_xm6_hdf_init_params(Z8Xm6HdfParams *in,
                            uint8_t level,
                            uint8_t monochrome,
                            uint8_t mode);

/* Compute exact V71.5 ev_gain_q8 from exposure-known scene_ev256 (EV100 * 256) */
uint16_t z8_xm6_hdf_ev_gain_from_ev256(int32_t scene_ev256);

/* Core Ricoh HDF v2 entry point with explicit IDM (192 KiB), EXT DRAM (512 KiB),
 * optional DMA ops, and optional per-stage profiling output.
 */
int32_t ricoh_hdf_xm6_v2_run(const Z8Xm6HdfParams *in,
                             const Z8Xm6HdfOutParam *out,
                             void *idm_workspace,
                             size_t idm_workspace_bytes,
                             void *ext_workspace,
                             size_t ext_workspace_bytes,
                             const Z8Xm6DmaOps *dma_ops,
                             Z8Xm6HdfProfile *profile);

/* Target Core 0 Slot 41 (0x0003955c) direct firmware entry for z8_xm6_elf0.elf.
 * Compiled when -DXM6_TARGET_Z8_ELF0 is defined.
 */
int32_t Z8_RicohHdf_Slot41(const Z8Xm6HdfParams *in,
                           Z8Xm6HdfOutParam *out);

#ifdef __cplusplus
}
#endif

#endif /* RICOH_HDF_XM6_V2_H */
