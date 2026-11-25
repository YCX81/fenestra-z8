/* SPDX-License-Identifier: MIT
 * Nikon Z8 C3.11 CEVA-XM6 DSP Porting Kit — FilmGrain Reference Kernel (Slot 39 / 40)
 *
 * 100% bit-exact C11 reference implementation matching:
 *   1. Nikon Zf C3.01 CEVA-XM6 DSP FilmGrain() (z8_mode_flags == 0: EXACT mode)
 *   2. Nikon Z8 EL1 z8_zf_grain.c SHARP mode   (z8_mode_flags == 1: SHARP delta-only blur)
 *
 * Design constraints for CEVA-XM6 Core 0 (z8_xm6_elf0.elf):
 *   - Pure integer math: 0 floating-point operations.
 *   - Zero malloc, zero writable globals/static .data/.bss, stack frame <= 1 KiB.
 *   - All working memory resides in caller-provided IDM workspace (<= 128 KiB,
 *     fits easily inside FW_APP_WORKBUF 0x00008000..0x00038000 = 192 KiB).
 *   - Operates strictly on 8-bit Luma Plane 0 (Y); CbCr is never touched.
 *   - Supports in-place (dst_y == src_y) and out-of-place execution up to 8256x8256.
 */
#ifndef ZF_GRAIN_XM6_REF_H
#define ZF_GRAIN_XM6_REF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Nikon Zf C3.01 / Z8 C3.11 CEVA-XM6 DSP return codes */
#define XM6_DSP_OK                  0
#define XM6_DSP_ERR_INVALID_PARAM   0x10000001

/* Fixed Nikon Zf C3.01 constants */
#define XM6_ZF_MAP_DIM_MAX          128U
#define XM6_ZF_STD_MAP_DIM          83U
#define XM6_ZF_STD_TILE_PERIOD      5U
#define XM6_ZF_STD_TILE_STEP        23U
#define XM6_ZF_STD_KNEE_LO          32U
#define XM6_ZF_STD_KNEE_HI          223U
#define XM6_ZF_DEFAULT_SEED         UINT32_C(0x12345678)
#define XM6_ZF_BLUR_VALID_MASK      0x2A9U /* {0, 3, 5, 7, 9} */

/* Z8 Extension flags in TFilmGrainInputParam::z8_mode_flags (+0x57) */
#define XM6_ZF_MODE_EXACT           0U /* Bit-exact Nikon Zf C3.01 DSP */
#define XM6_ZF_MODE_SHARP           1U /* Bit-exact Z8 EL1 delta-only blur */

/* Required IDM scratch workspace size (fits inside 192 KiB FW_APP_WORKBUF) */
#define XM6_ZF_IDM_WORKSPACE_BYTES  131072U /* 128 KiB (actual struct ~99 KiB, 64B aligned) */

/* On-card ZFG1 binary header constants (ZF_GRAIN.BIN = 27,588 bytes) */
#define XM6_ZFG1_HEADER_BYTES       32U
#define XM6_ZFG1_MAP_ELEMENTS       6889U  /* 83 * 83 */
#define XM6_ZFG1_MAP_BYTES          13778U /* 83 * 83 * 2 */
#define XM6_ZFG1_FILE_BYTES         27588U

/* Exact 0x38-byte Nikon EXPEED 7 tagDSPImageParam binary wire layout.
 * On 32-bit CEVA-XM6, plane_addr[i] is stored in the low 32 bits (with hi=0);
 * on 64-bit host, plane_addr[i] holds the full 64-bit pointer seamlessly!
 */
typedef struct {
    uint64_t plane_addr[3]; /* +0x00, +0x08, +0x10: u32 DSP VA + u32 pad0 on XM6; uintptr on 64-bit host */
    uint32_t roi_x;         /* +0x18 */
    uint32_t roi_y;         /* +0x1c */
    int32_t  width;         /* +0x20 */
    int32_t  height;        /* +0x24 */
    int32_t  stride[3];     /* +0x28, +0x2c, +0x30 */
    uint32_t format;        /* +0x34: 0x10=1P Y, 0x30=NV16, 0x40=NV12 */
} tagDSPImageParam;

/* Exact 0x80-byte Nikon Zf C3.01 TFilmGrainInputParam binary wire layout */
typedef struct {
    tagDSPImageParam src;              /* +0x00 .. +0x37 (56 B) */
    uint8_t          reserved_38[20];  /* +0x38 .. +0x4b */
    int32_t          bypass_flag;      /* +0x4c: 1 = bypass return 0 */
    uint8_t          map_blend_weight; /* +0x50: Q4 [0..16] */
    uint8_t          blur_kernel_size; /* +0x51: {0, 3, 5, 7, 9} */
    uint8_t          map_dim;          /* +0x52: M (83) */
    uint8_t          tile_period;      /* +0x53: P (5) */
    uint8_t          tile_step;        /* +0x54: S (23) */
    uint8_t          tone_knee_lo;     /* +0x55: K_lo (32) */
    uint8_t          tone_knee_hi;     /* +0x56: K_hi (223) */
    uint8_t          z8_mode_flags;    /* +0x57: 0=EXACT (Zf stock), 1=SHARP (Z8 ext) */
    uint32_t         prng_seed;        /* +0x58: xorshift32 seed (0 -> 0x12345678) */
    uint32_t         reserved_5c;      /* +0x5c */
    uint64_t         mapA_addr;        /* +0x60: const int16_t[M][M] */
    uint32_t         mapA_dim;         /* +0x68: M */
    uint32_t         reserved_6c;      /* +0x6c */
    uint64_t         mapB_addr;        /* +0x70: const int16_t[M][M] or 0 (all-zero) */
    uint32_t         mapB_dim;         /* +0x78: M */
    uint32_t         reserved_7c;      /* +0x7c */
} TFilmGrainInputParam;

typedef tagDSPImageParam TFilmGrainOutputParam;

/* Compile-time ABI layout verification against Zf C3.01 & Z8 C3.11 */
typedef char xm6_zf_chk_img[(sizeof(tagDSPImageParam) == 0x38U) ? 1 : -1];
typedef char xm6_zf_chk_in[(sizeof(TFilmGrainInputParam) == 0x80U) ? 1 : -1];
typedef char xm6_zf_chk_out[(sizeof(TFilmGrainOutputParam) == 0x38U) ? 1 : -1];

/* Optional DMA hooks: when running on CEVA-XM6 hardware, the porting agent replaces
 * the default memcpy stubs with NikonDMA::CreateDescriptor1D (0x23c0) /
 * DispatchRequest (0x2960) / WaitRequest (0x2ac0) ping-pong transfers.
 */
typedef struct {
    void (*dma_ext_to_idm)(void *idm_dst, uint64_t ext_src_addr, uint32_t bytes);
    void (*dma_idm_to_ext)(uint64_t ext_dst_addr, const void *idm_src, uint32_t bytes);
} Xm6DmaOps;

/* Core FilmGrain entry point with explicit IDM scratch and optional DMA ops.
 * Pass dma_ops == NULL for direct memory access (host / XM6 simulator flat memory).
 */
int32_t zf_grain_xm6_run(const TFilmGrainInputParam *in,
                         const TFilmGrainOutputParam *out,
                         void *idm_workspace,
                         size_t idm_workspace_bytes,
                         const Xm6DmaOps *dma_ops);

/* Target Slot 39 / Slot 40 direct signature for Z8 C3.11 ELF0:
 * Uses FW_APP_WORKBUF (0x00008000) when built with -DXM6_TARGET_Z8_ELF0.
 */
int32_t Z8_FilmGrain_Slot39_40(const TFilmGrainInputParam *in,
                               TFilmGrainOutputParam *out);

/* Resolve Nikon Zf 18-level preset (intensity 0..6, size 1..3, reduced_res 0/1) */
void zf_grain_xm6_fill_preset(TFilmGrainInputParam *in,
                              uint8_t intensity,
                              uint8_t size,
                              uint8_t reduced_res,
                              uint8_t blur_mode,
                              uint32_t prng_seed,
                              uint64_t map0_addr,
                              uint64_t map1_addr);

#ifdef __cplusplus
}
#endif

#endif /* ZF_GRAIN_XM6_REF_H */
