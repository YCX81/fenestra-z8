/* SPDX-License-Identifier: MIT
 * Nikon Z8 C3.11 EL1 ARM64 <-> CEVA-XM6 Core 0 Hardware Acceleration Bridge.
 *
 * Strictly freestanding C11, scalar-only (-mgeneral-regs-only), zero FP,
 * zero SIMD, zero writable .data/.bss (all state & 64B-aligned DMA parameter
 * blocks reside inside filmsim_runtime_context in kernel heap 0x0a...).
 *
 * Verified Z8 C3.11 main.main addresses:
 *   - 0x40c909a8: DspFramework_SubmitAsync(Z8Xm6DspReq *req)
 *   - 0x40c90bec: DspFramework_CancelAsync(Z8Xm6DspReq *req)
 *   - 0x40cb6d50: IOMMU_GetArmBase(uint32_t win) -> uint64_t
 *   - 0x40cb6d0c: IOMMU_GetWindowSize(uint32_t win) -> uint32_t
 *   - 0x40cb6b50: IOMMU_ArmToDsp32(uint32_t win, uint64_t va, uint32_t *dsp) -> uint32_t
 *   - 0x40d2b9c8: DCache_CleanInvalidate(void *addr, uint32_t bytes)  [chunk <= 512 KiB]
 *   - 0x40187b00: dly_tsk(uint32_t ms)
 *   - 0x0a02e803ee: g_elf_loader_fault (uint8_t)
 *   - 0x0a02e803e8: g_core0_fw_loaded  (uint8_t)
 *   - 0x0a02e7e734: g_core0_ctrl.state (uint32_t, 2 == RUNNING)
 */
#ifndef Z8_XM6_BRIDGE_H
#define Z8_XM6_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include "z8_filmsim_abi.h"
#include "../photo/z8_native_filmsim_photo.h"

struct z8_xm6_bridge;

/* Core 0 slot IDs in g_NikonDspFrameworkFunctionTable (0x000394b8) */
#define Z8_XM6_SLOT_RGBLUT           12U /* Stock Core 0 Opcode 12: 1D separable RGBLUT */
#define Z8_XM6_SLOT_GRAIN_STILL      39U
#define Z8_XM6_SLOT_GRAIN_LIVE       40U
#define Z8_XM6_SLOT_HDF              41U

/* Core 0 stock mailbox idle words at 0x0a02e7eed0[0..3] */
#define Z8_XM6_MAILBOX_IDLE_W0       UINT32_C(0xdeadbeef)
#define Z8_XM6_MAILBOX_IDLE_W1       UINT32_C(0x10000000)
#define Z8_XM6_MAILBOX_IDLE_W2       UINT32_C(0xdeadbeef)
#define Z8_XM6_MAILBOX_IDLE_W3       UINT32_C(0x10000000)

/* Stock Opcode 12 (RGBLUT) geometry & arena sizing (verified on Z8 C3.11) */
#define Z8_XM6_RGBLUT_MAX_PITCH      8256U
#define Z8_XM6_RGBLUT_MAX_HEIGHT     5504U
#define Z8_XM6_RGBLUT_CONTROL_BYTES  0xc00U
#define Z8_XM6_RGBLUT_WORK_BASE      0x4b000U
#define Z8_XM6_RGBLUT_WORK_PER_PITCH 0x330U
#define Z8_XM6_RGBLUT_STATUS_INIT    UINT32_C(0x72504e44)
#define Z8_XM6_RGBLUT_LOCK_MS        200U

/* EXPEED 7 tagDSPImageParam::format codes */
#define Z8_XM6_FMT_Y_ONLY            0x10U
#define Z8_XM6_FMT_NV16              0x30U
#define Z8_XM6_FMT_NV12              0x40U

/* Slot 41 Ricoh HDF v2 engine modes */
#define Z8_XM6_HDF_MODE_AUTO            0U
#define Z8_XM6_HDF_MODE_LIVE_256_YC     1U
#define Z8_XM6_HDF_MODE_STILL_GENERAL   2U
#define Z8_XM6_HDF_MODE_LIVE_EVF_256_YC 3U

/* Zf C3.01 FilmGrain constants (exact match to z8_zf_grain.c & Slot 39/40 ELF0) */
#define Z8_XM6_ZF_MAP_DIM            83U
#define Z8_XM6_ZF_TILE_PERIOD        5U
#define Z8_XM6_ZF_TILE_STEP          23U
#define Z8_XM6_ZF_KNEE_LO            32U
#define Z8_XM6_ZF_KNEE_HI            223U
#define Z8_XM6_ZF_DEFAULT_SEED       UINT32_C(0x12345678)
#define Z8_XM6_ZFG1_HEADER_BYTES     32U
#define Z8_XM6_ZFG1_MAP_BYTES        13778U /* 83 * 83 * 2 */
#define Z8_XM6_ZFG1_FILE_BYTES       27588U
#define Z8_XM6_ZFG1_CLEAN_BYTES      27648U /* 64B-aligned >= 27,588 B */

/* Slot 41 dedicated 512 KiB external grid workspace (NEVER pass ext_work_addr=0:
 * 0x41940000..0x419A0000 inside FW_EXT_WORKBUF holds Nikon FreeRTOS TaskSwitchIn
 * 192 KiB IDM spill buffers tsk0esc & tsk1esc!). */
#define Z8_XM6_HDF_EXT_WS_BYTES      524288U
#define Z8_XM6_HDF_EXT_WS_ALLOC      (Z8_XM6_HDF_EXT_WS_BYTES + 64U)

/* Bounded timeout & cancel-drain budgets (ms / us) */
#define Z8_XM6_TIMEOUT_LIVE_MS       35U
#define Z8_XM6_TIMEOUT_STILL_MS      2500U
#define Z8_XM6_TIMEOUT_DIAG_MS       800U
#define Z8_XM6_LIVE_DRAIN_MS         120U
#define Z8_XM6_STILL_DRAIN_MS        500U
#define Z8_XM6_LIVE_BACKOFF_US       UINT64_C(5000000)
#define Z8_XM6_LIVE_SOFT_DISABLE_STREAK 3U

/* CEVA-XM6 DSP & Z8 DspFrameworkWrapper status codes */
#define Z8_XM6_DSP_OK                UINT32_C(0x00000000)
#define Z8_XM6_DSP_ERR_ABORT         UINT32_C(0x10000000)
#define Z8_XM6_DSP_ERR_BAD_PARAM     UINT32_C(0x10000001)
#define Z8_XM6_DSP_STATUS_SENTINEL   UINT32_C(0xdeadbeef)

/* Explicit Z8_FS_OP_XM6_DIAG skip reasons (reported in dsp_status_last when hw_state==2 && ran_mask==0) */
#define Z8_XM6_DIAG_SKIP_STOPPED       UINT32_C(0x000000F1)
#define Z8_XM6_DIAG_SKIP_SUSPENDED     UINT32_C(0x000000F2)
#define Z8_XM6_DIAG_SKIP_MAILBOX_BUSY  UINT32_C(0x000000F3)
#define Z8_XM6_DIAG_SKIP_ALLOC_FAIL    UINT32_C(0x000000F4)
#define Z8_XM6_DIAG_SKIP_CORE0_BUSY    UINT32_C(0x000000F5)
#define Z8_XM6_DIAG_SKIP_NO_CTX        UINT32_C(0x000000F6)
#define Z8_XM6_DIAG_SKIP_REQ_IN_FLIGHT UINT32_C(0x000000F7)

/* Core 0 IOMMU window range (0x28..0x2b = 1 GiB 0x0a..; 0x2c..0x2f = 2 GiB 0x084..0x08b) */
#define Z8_XM6_IOMMU_WIN_FIRST       0x28U
#define Z8_XM6_IOMMU_WIN_LAST        0x2FU
#define Z8_XM6_DCACHE_MAX_CHUNK      0x80000U /* 512 KiB, strictly < 0x88000 assert guard */

/* Live NV12 HDF return codes */
#define Z8_XM6_LIVE_OK               1  /* Graded on XM6; output cache invalidated */
#define Z8_XM6_LIVE_FALLBACK         0  /* Source untouched; fall back transparently to ARM NEON */
#define Z8_XM6_LIVE_SKIP_DIRTY      -1  /* Mid-execution timeout/DSP fault (drained); skip 1 frame */
#define Z8_XM6_LIVE_HUNG_DMA        -2  /* Timeout NOT drained (DMA still active); quarantine buffer */

/* Still XM6 per-stage return codes */
#define Z8_XM6_STILL_OK              1  /* Stage completed on XM6 */
#define Z8_XM6_STILL_FALLBACK_CLEAN  0  /* Destination untouched (or source untouched); safe fallback */
#define Z8_XM6_STILL_FALLBACK_DIRTY -1  /* Mid-execution fault on destination (drained); caller must recopy */
#define Z8_XM6_STILL_HUNG_DMA       -2  /* Timeout NOT drained (DMA still active); MUST quarantine blocks! */

/* Bounded Core 0 lock wait budgets (ms) for Live & Layer 0 preview when no 45MP still is in flight */
#define Z8_XM6_LIVE_WAIT_MS          16U
#define Z8_XM6_PREVIEW_WAIT_MS       16U

/* ---- Exact Binary Wire Structures (64B-aligned in kernel heap) ---- */

typedef struct {
    uint32_t core_id;       /* +0x00: 0 = Core 0 (z8_xm6_elf0.elf) */
    uint32_t reserved_04;   /* +0x04: 0 */
    uint32_t slot_id;       /* +0x08: 39 / 40 / 41 */
    uint32_t reserved_0c;   /* +0x0c: 0 */
    uint64_t in_param;      /* +0x10: Path A zero-extended 32-bit DSP VA */
    uint64_t out_param;     /* +0x18: Path A zero-extended 32-bit DSP VA */
    uint32_t channel;       /* +0x20: 0 = Normal queue (flag 0x80) */
    uint32_t reserved_24;   /* +0x24: 0 */
    void   (*callback)(uint32_t code, void *req); /* +0x28: MUST be non-NULL for 0x40c909a8 */
    uint32_t dsp_status;    /* +0x30: init 0xDEADBEEF; 0=OK, 0x10000000=abort, 0x10000001=bad_param */
    uint32_t reserved_34;   /* +0x34: 0 */
    uint64_t reserved_38;   /* +0x38: 0 (pad to 64 B) */
} Z8Xm6DspReq;

_Static_assert(sizeof(Z8Xm6DspReq) == 0x40U, "Z8Xm6DspReq must be 64 B");
_Static_assert(offsetof(Z8Xm6DspReq, core_id) == 0x00U, "req.core_id");
_Static_assert(offsetof(Z8Xm6DspReq, slot_id) == 0x08U, "req.slot_id");
_Static_assert(offsetof(Z8Xm6DspReq, in_param) == 0x10U, "req.in_param");
_Static_assert(offsetof(Z8Xm6DspReq, out_param) == 0x18U, "req.out_param");
_Static_assert(offsetof(Z8Xm6DspReq, channel) == 0x20U, "req.channel");
_Static_assert(offsetof(Z8Xm6DspReq, callback) == 0x28U, "req.callback");
_Static_assert(offsetof(Z8Xm6DspReq, dsp_status) == 0x30U, "req.dsp_status");

typedef struct {
    uint64_t plane_addr[3]; /* +0x00, +0x08, +0x10: zero-extended 32-bit DSP VA */
    uint32_t roi_x;         /* +0x18 */
    uint32_t roi_y;         /* +0x1c */
    int32_t  width;         /* +0x20 */
    int32_t  height;        /* +0x24 */
    int32_t  stride[3];     /* +0x28, +0x2c, +0x30 */
    uint32_t format;        /* +0x34: 0x10=Y, 0x30=NV16, 0x40=NV12 */
} Z8Xm6ImageParam;

_Static_assert(sizeof(Z8Xm6ImageParam) == 0x38U, "Z8Xm6ImageParam must be 56 B");
_Static_assert(offsetof(Z8Xm6ImageParam, format) == 0x34U, "img.format");

typedef struct {
    Z8Xm6ImageParam src;              /* +0x00 .. +0x37 (56 B) */
    uint8_t         reserved_38[20];  /* +0x38 .. +0x4b */
    int32_t         bypass_flag;      /* +0x4c */
    uint8_t         map_blend_weight; /* +0x50: Q4 [0..16] */
    uint8_t         blur_kernel_size; /* +0x51: {0, 3, 5, 7, 9} */
    uint8_t         map_dim;          /* +0x52: 83 */
    uint8_t         tile_period;      /* +0x53: 5 */
    uint8_t         tile_step;        /* +0x54: 23 */
    uint8_t         tone_knee_lo;     /* +0x55: 32 */
    uint8_t         tone_knee_hi;     /* +0x56: 223 */
    uint8_t         z8_mode_flags;    /* +0x57: 0=EXACT, 1=SHARP */
    uint32_t        prng_seed;        /* +0x58 */
    uint32_t        reserved_5c;      /* +0x5c */
    uint64_t        mapA_addr;        /* +0x60: zero-extended 32-bit DSP VA */
    uint32_t        mapA_dim;         /* +0x68: 83 */
    uint32_t        reserved_6c;      /* +0x6c */
    uint64_t        mapB_addr;        /* +0x70: 0 */
    uint32_t        mapB_dim;         /* +0x78: 83 */
    uint32_t        reserved_7c;      /* +0x7c */
} Z8Xm6GrainIn;

_Static_assert(sizeof(Z8Xm6GrainIn) == 0x80U, "Z8Xm6GrainIn must be 128 B");
_Static_assert(offsetof(Z8Xm6GrainIn, bypass_flag) == 0x4cU, "grain.bypass_flag");
_Static_assert(offsetof(Z8Xm6GrainIn, z8_mode_flags) == 0x57U, "grain.z8_mode_flags");
_Static_assert(offsetof(Z8Xm6GrainIn, mapA_addr) == 0x60U, "grain.mapA_addr");

typedef struct {
    Z8Xm6ImageParam src;            /* +0x00 .. +0x37 (56 B) */
    int32_t         bypass_flag;    /* +0x38 */
    uint8_t         level;          /* +0x3c: 0..3 */
    uint8_t         monochrome;     /* +0x3d: 0..1 */
    uint8_t         mode;           /* +0x3e: 0=AUTO, 1=LIVE_256_YC, 2=STILL_GENERAL */
    uint8_t         reserved_3f;    /* +0x3f: 0 */
    uint16_t        knee_q16;       /* +0x40: 0 -> default 22528 */
    uint16_t        neutrality_q8;  /* +0x42: 0xFFFF -> default 96 */
    uint16_t        ev_gain_q8;     /* +0x44: 256 (1.0x) */
    uint8_t         white[3];       /* +0x46..+0x48: {255, 255, 255} */
    uint8_t         reserved_49[7]; /* +0x49..+0x4f: 0 */
    uint64_t        ext_work_addr;  /* +0x50: MUST be non-zero 64B-aligned 32-bit DSP VA */
    uint32_t        ext_work_bytes; /* +0x58: MUST be >= 524288 (Z8_XM6_HDF_EXT_WS_BYTES) */
    uint32_t        reserved_5c;    /* +0x5c: 0 */
} Z8Xm6HdfIn;

_Static_assert(sizeof(Z8Xm6HdfIn) == 0x60U, "Z8Xm6HdfIn must be 96 B");
_Static_assert(offsetof(Z8Xm6HdfIn, bypass_flag) == 0x38U, "hdf.bypass_flag");
_Static_assert(offsetof(Z8Xm6HdfIn, level) == 0x3cU, "hdf.level");
_Static_assert(offsetof(Z8Xm6HdfIn, ext_work_addr) == 0x50U, "hdf.ext_work_addr");

/* Stock Core 0 Opcode 12 (RGBLUT) compiled 1D separable curves & wire ABI */
typedef struct {
    uint16_t values[3][256]; /* Q15 knots (32768 == 1.0) for R, G, B output channels */
    uint8_t  source[3];      /* Input channel permutation (0=R, 1=G, 2=B) */
    uint8_t  reserved;
} Z8Xm6RgblutCurves;

typedef struct {
    Z8Xm6ImageParam image;          /* +0x00 .. +0x37 (56 B) */
    uint32_t        reserved0;      /* +0x38 */
    uint32_t        mode;           /* +0x3c: 0x40 */
    uint64_t        work;           /* +0x40: DMA VA of scratch work buffer */
    uint32_t        work_bytes;     /* +0x48: 0x4b000 + 0x330 * pitch */
    uint32_t        reserved1;      /* +0x4c */
    int16_t         front[12];      /* +0x50 .. +0x67: YCbCr -> RGB matrix (permuted) */
    int16_t         back[12];       /* +0x68 .. +0x7f: RGB -> YCbCr matrix */
    uint16_t        curve[3][256];  /* +0x80 .. +0x67f: 3x256 Q15 curve table */
} Z8Xm6RgblutParam;

typedef struct {
    uint64_t          link;         /* +0x00: 0 */
    uint32_t          opcode;       /* +0x08: 12 (Z8_XM6_SLOT_RGBLUT) */
    uint32_t          reserved0;    /* +0x0c: 0 */
    uint64_t          in_ptr;       /* +0x10: Z8Xm6RgblutParam * (DMA VA) */
    uint64_t          out_ptr;      /* +0x18: Z8Xm6ImageParam * (DMA VA) */
    uint32_t          lane;         /* +0x20: 1 */
    uint32_t          reserved1[3]; /* +0x24 .. +0x2f */
    volatile uint32_t status;       /* +0x30: init 0x72504e44, 0 == OK */
    uint32_t          tail[3];      /* +0x34 .. +0x3f */
} Z8Xm6RgblutJob;

_Static_assert(sizeof(Z8Xm6RgblutParam) == 0x680U, "Z8Xm6RgblutParam must be 0x680 B");
_Static_assert(offsetof(Z8Xm6RgblutParam, front) == 0x50U, "rgblut.front");
_Static_assert(offsetof(Z8Xm6RgblutParam, curve) == 0x80U, "rgblut.curve");
_Static_assert(sizeof(Z8Xm6RgblutJob) == 0x40U, "Z8Xm6RgblutJob must be 64 B");
_Static_assert(offsetof(Z8Xm6RgblutJob, status) == 0x30U, "rgblut.status");

/* Optional hardware/mock operations table (NULL -> direct Z8 C3.11 EL1 bare-metal ROM calls) */
typedef struct {
    uint32_t (*read_core0_state)(void *ctx);
    uint8_t  (*read_loader_fault)(void *ctx);
    uint8_t  (*read_fw_loaded)(void *ctx);
    uint64_t (*iommu_win_base)(void *ctx, uint32_t win);
    uint32_t (*iommu_win_size)(void *ctx, uint32_t win);
    uint32_t (*iommu_xlat)(void *ctx, uint32_t win, uint64_t dma_va, uint32_t *out_dsp_va);
    void     (*dcache_civac)(void *ctx, void *addr, uint32_t bytes);
    void     (*async_submit)(void *ctx, Z8Xm6DspReq *req);
    void     (*async_cancel)(void *ctx, Z8Xm6DspReq *req);
    void     (*sleep_ms)(void *ctx, uint32_t ms);
    uint64_t (*time_us)(void *ctx);
    int      (*alloc_class24)(void *ctx, uint32_t requested, struct z8_nfsp_allocation *out);
    void     (*release_class24)(void *ctx, struct z8_nfsp_allocation *blk);
    void    *ctx;
    /* V71.7 optional extensions (zero-init NULL preserves 100% legacy V71.6 mock compat) */
    void     (*read_mailbox_words)(void *ctx, uint32_t out_words[4]);
    uint32_t (*sync_submit)(void *ctx, Z8Xm6RgblutJob *job, uint64_t job_dma, uint32_t lock_arg);
} Z8Xm6HwOps;

/* Indirect vtable stored in .rodata so z8_native_filmsim_photo.c & pipeline stages
 * can invoke XM6 without hard-linking z8_xm6_bridge.c in standalone host tests. */
typedef struct {
    int  (*hw_ready)(const struct z8_xm6_bridge *b, uint8_t xm6_mode);
    int  (*check_iommu)(const struct z8_xm6_bridge *b, uint64_t arm_va, uint32_t span_bytes);
    int  (*still_hdf)(struct z8_xm6_bridge *b, uint8_t xm6_mode,
                      uint64_t src_y_dma, uint64_t src_c_dma,
                      uint64_t dst_y_dma, uint64_t dst_c_dma,
                      uint32_t width, uint32_t height,
                      uint32_t y_stride, uint32_t c_stride,
                      uint32_t y_span, uint32_t c_span,
                      uint8_t hdf_level, uint32_t fmt_code,
                      uint32_t *elapsed_us_out);
    int  (*still_grain)(struct z8_xm6_bridge *b, uint8_t xm6_mode,
                        uint64_t src_y_dma, uint64_t dst_y_dma,
                        uint32_t width, uint32_t height,
                        uint32_t y_stride, uint32_t y_span,
                        uint8_t intensity, uint8_t size,
                        uint8_t blur_mode, uint8_t reduced_res,
                        uint32_t prng_seed, const void *zf_raw_buf,
                        uint32_t *elapsed_us_out);
    void (*record_still_ok)(struct z8_xm6_bridge *b, uint64_t total_us);
    void (*record_fallback)(struct z8_xm6_bridge *b, uint16_t err_code);
    uint32_t (*fold_grain_seed)(uint64_t shot_id);
    int  (*still_rgblut)(struct z8_xm6_bridge *b, uint8_t xm6_mode,
                         const Z8Xm6RgblutCurves *curves,
                         const uint8_t *src_y_cpu, const uint8_t *src_c_cpu,
                         uint64_t src_y_dma, uint64_t src_c_dma,
                         uint8_t *dst_y_cpu, uint8_t *dst_c_cpu,
                         uint64_t dst_y_dma, uint64_t dst_c_dma,
                         uint32_t width, uint32_t height,
                         uint32_t pitch,
                         uint32_t *elapsed_us_out);
} Z8Xm6BridgeOps;

/* 64B-aligned DMA parameter block layout stored inside dma_param_raw */
typedef struct {
    Z8Xm6DspReq           req;          /* +0x000 (64 B, own cache line) */
    Z8Xm6GrainIn          grain_in;     /* +0x040 (128 B, 2 cache lines) */
    Z8Xm6HdfIn            hdf_in;       /* +0x0c0 (96 B) */
    uint8_t               pad_120[32];  /* +0x120 (pad to 64B line) */
    Z8Xm6ImageParam       out_param;    /* +0x140 (56 B, own cache line) */
    uint8_t               pad_178[8];   /* +0x178 */
    struct z8_xm6_bridge *owner;        /* +0x180 (8 B, outside all DMA structs) */
} Z8Xm6DmaSlots;

_Static_assert(offsetof(Z8Xm6DmaSlots, req) == 0x000U, "slots.req");
_Static_assert(offsetof(Z8Xm6DmaSlots, grain_in) == 0x040U, "slots.grain_in");
_Static_assert(offsetof(Z8Xm6DmaSlots, hdf_in) == 0x0c0U, "slots.hdf_in");
_Static_assert(offsetof(Z8Xm6DmaSlots, out_param) == 0x140U, "slots.out_param");
_Static_assert(offsetof(Z8Xm6DmaSlots, owner) == 0x180U, "slots.owner");
_Static_assert(sizeof(Z8Xm6DmaSlots) <= 448U, "slots fits in dma_param_raw");

/* Heap-resident bridge state (embedded inside struct filmsim_runtime_context) */
struct z8_xm6_bridge {
    const Z8Xm6BridgeOps *ops;
    const Z8Xm6HwOps     *hw;

    /* Concurrency & one-shot circuit breaker (accessed via __atomic_* builtins) */
    uint32_t xm6_busy;
    uint32_t breaker_tripped;
    uint32_t done_flag;
    uint32_t req_in_flight;
    uint32_t cb_code;
    uint32_t diag_ran;
    uint32_t diag_passed;
    uint32_t last_err;
    uint32_t last_dsp_status;
    uint32_t last_still_ms;
    uint32_t last_live_us;
    uint32_t still_ok;
    uint32_t live_ok;
    uint32_t fallback_cnt;

    /* Dynamically 64B-aligned storage for Z8Xm6DmaSlots (zero .data/.bss!) */
    uint8_t  dma_param_raw[512];

    /* Appended silicon-hardening state (strictly AFTER dma_param_raw[512] so offsets
     * 0x00..0x240 / filmsim_runtime_context 0x2238..0x226C stay 100% unchanged!) */
    uint32_t live_soft_disabled;
    uint32_t live_timeout_streak;
    uint64_t live_backoff_until_us;
    uint32_t live_flight_logged;
    uint32_t ext_ws_dsp_va;
    uint8_t *ext_ws_aligned_ptr;
    struct z8_nfsp_allocation ext_ws_blk;
    /* V71.7-r3 isolated preview-only XM6 backoff (NEVER writes live_soft_disabled!) */
    uint32_t preview_timeout_streak;
    uint32_t preview_reserved;
    uint64_t preview_backoff_until_us;
    /* V71.7-r4: atomic still_in_flight fast-skip gate, hung-DMA latch, pool-suspend gate & waiters */
    uint32_t still_in_flight;
    uint32_t still_hung_dma;
    uint32_t pool_suspended;
    uint32_t waiters;
};

_Static_assert(offsetof(struct z8_xm6_bridge, xm6_busy) == 0x10U, "xm6.xm6_busy ABI");
_Static_assert(offsetof(struct z8_xm6_bridge, fallback_cnt) == 0x44U, "xm6.fallback_cnt ABI");
_Static_assert(offsetof(struct z8_xm6_bridge, dma_param_raw) == 0x48U, "xm6.dma_param_raw ABI");

/* ---- Public EL1 Bridge API ---- */

void z8_xm6_bridge_init(struct z8_xm6_bridge *b, const Z8Xm6HwOps *hw);
void z8_xm6_bridge_set_hw_ops(struct z8_xm6_bridge *b, const Z8Xm6HwOps *hw);

/* Raw hardware register read (safe even when b == NULL -> reads Z8 bare-metal regs) */
void z8_xm6_read_hw_raw(const struct z8_xm6_bridge *b,
                        uint32_t *state_out,
                        uint8_t *loader_fault_out,
                        uint8_t *fw_loaded_out);

/* Layer 1: O(1) HW ready gate (< 5 ns on QEMU -> returns 0), including
 * V71.7 Core 0 mailbox idle-word check (0xA02E7EED0[0..3]). */
int  z8_xm6_core0_mailbox_idle(const struct z8_xm6_bridge *b);
int  z8_xm6_core0_hw_ready(const struct z8_xm6_bridge *b, uint8_t xm6_mode);
int  z8_xm6_core0_hw_only_ready(const struct z8_xm6_bridge *b);

/* Compute required Class 0x24 arena size (control + work) for Opcode 12 RGBLUT */
uint32_t z8_xm6_rgblut_arena_bytes(uint32_t pitch);

/* Stock Core 0 Opcode 12 (RGBLUT) 1D separable .cube LUT on NV16 planes,
 * serialized under b->xm6_busy with IOMMU/D-Cache/512 MiB spacer/mailbox guards.
 * Returns Z8_XM6_STILL_OK (1), Z8_XM6_STILL_FALLBACK_CLEAN (0), or
 * Z8_XM6_STILL_FALLBACK_DIRTY (-1). */
int  z8_xm6_rgblut_nv16(struct z8_xm6_bridge *b, uint8_t xm6_mode,
                        const Z8Xm6RgblutCurves *curves,
                        const uint8_t *src_y_cpu, const uint8_t *src_c_cpu,
                        uint64_t src_y_dma, uint64_t src_c_dma,
                        uint8_t *dst_y_cpu, uint8_t *dst_c_cpu,
                        uint64_t dst_y_dma, uint64_t dst_c_dma,
                        uint32_t width, uint32_t height,
                        uint32_t pitch,
                        uint32_t *elapsed_us_out);

/* Layer 2: Safe Core 0 ARM64 -> DSP32 IOMMU translator (strips 0x0c4..0x0cb alias,
 * scans windows 0x28..0x2f for single-window contiguous containment, calls 0x40cb6b50). */
int  z8_xm6_core0_arm_to_dsp32(const struct z8_xm6_bridge *b,
                               uint64_t arm_va,
                               uint32_t span_bytes,
                               uint32_t *out_dsp_va);

/* Chunked <= 512 KiB D-Cache Clean+Invalidate (0x40d2b9c8) */
void z8_xm6_dcache_civac(const struct z8_xm6_bridge *b, void *addr, uint32_t bytes);

/* Exact Zf C3.01 18-level preset filler & SplitMix64 seed folder (bit-exact with z8_zf_grain.c) */
uint32_t z8_xm6_fold_grain_seed(uint64_t shot_id);
void z8_xm6_fill_grain_preset(Z8Xm6GrainIn *in,
                              uint8_t intensity,
                              uint8_t size,
                              uint8_t reduced_res,
                              uint8_t blur_mode,
                              uint32_t prng_seed,
                              uint32_t map0_dsp_va,
                              uint32_t map1_dsp_va);

/* Still Photo XM6 dispatchers (out-of-place when src != dst, in-place when src == dst) */
int  z8_xm6_still_hdf(struct z8_xm6_bridge *b, uint8_t xm6_mode,
                      uint64_t src_y_dma, uint64_t src_c_dma,
                      uint64_t dst_y_dma, uint64_t dst_c_dma,
                      uint32_t width, uint32_t height,
                      uint32_t y_stride, uint32_t c_stride,
                      uint32_t y_span, uint32_t c_span,
                      uint8_t hdf_level, uint32_t fmt_code,
                      uint32_t *elapsed_us_out);

int  z8_xm6_still_grain(struct z8_xm6_bridge *b, uint8_t xm6_mode,
                        uint64_t src_y_dma, uint64_t dst_y_dma,
                        uint32_t width, uint32_t height,
                        uint32_t y_stride, uint32_t y_span,
                        uint8_t intensity, uint8_t size,
                        uint8_t blur_mode, uint8_t reduced_res,
                        uint32_t prng_seed, const void *zf_raw_buf,
                        uint32_t *elapsed_us_out);

/* Live Mode 2 NV12 HDF (1024x{680,682}, Slot 41 LIVE_256_YC, 35ms timeout, 0ms lock wait).
 * Returns Z8_XM6_LIVE_OK (1), Z8_XM6_LIVE_FALLBACK (0), Z8_XM6_LIVE_SKIP_DIRTY (-1),
 * or Z8_XM6_LIVE_HUNG_DMA (-2). */
int  z8_xm6_live_hdf_nv12(struct z8_xm6_bridge *b, uint8_t xm6_mode,
                          uint8_t *y_cpu, uint8_t *c_cpu,
                          uint64_t y_dma, uint64_t c_dma,
                          uint32_t width, uint32_t height,
                          uint32_t y_stride, uint32_t c_stride,
                          uint8_t hdf_level);

/* Layer 0 LCD Preview NV12 HDF (1024x682 ONLY, Slot 41 LIVE_256_YC, 25ms timeout, 0ms lock wait).
 * Strictly isolated from Live Photo's live_soft_disabled/live_timeout_streak so preview hiccups
 * NEVER degrade 30fps Live Photo or Still Photo XM6 acceleration. EVF 1280x852 returns FALLBACK. */
int  z8_xm6_preview_hdf_nv12(struct z8_xm6_bridge *b, uint8_t xm6_mode,
                             uint8_t *y_cpu, uint8_t *c_cpu,
                             uint64_t y_dma, uint64_t c_dma,
                             uint32_t width, uint32_t height,
                             uint32_t y_stride, uint32_t c_stride,
                             uint8_t hdf_level);

/* Telemetry & Control */
void z8_xm6_record_still_ok(struct z8_xm6_bridge *b, uint64_t total_us);
void z8_xm6_record_fallback(struct z8_xm6_bridge *b, uint16_t err_code);
void z8_xm6_fill_status(const struct z8_xm6_bridge *b, uint8_t xm6_mode, Z8FilmSimStatus *st);
void z8_xm6_reset_breaker(struct z8_xm6_bridge *b);
void z8_xm6_clear_live_soft_backoff(struct z8_xm6_bridge *b);
void z8_xm6_release_ext_ws(struct z8_xm6_bridge *b);
void z8_xm6_set_pool_suspended(struct z8_xm6_bridge *b, uint32_t suspended);

/* Off-screen 0-diff hardware self-test (Z8_FS_OP_XM6_DIAG, op 10) */
void z8_xm6_run_diag(struct z8_xm6_bridge *b,
                     const void *zf_raw_buf,
                     uint32_t zf_raw_loaded,
                     Z8Xm6DiagResult *out);
void z8_xm6_run_diag_ex(struct z8_xm6_bridge *b,
                        const void *zf_raw_buf,
                        uint32_t zf_raw_loaded,
                        uint32_t skip_reason,
                        Z8Xm6DiagResult *out);

#endif /* Z8_XM6_BRIDGE_H */
