/* SPDX-License-Identifier: MIT
 * Nikon Zf C3.01 Bit-Exact Film Grain Kernel (z8_zf_grain).
 *
 * Freestanding C11 + __aarch64__ NEON implementation of Zf C3.01 CEVA-XM6
 * FilmGrain() (commit a1d5c29) and ARM64 DspFrameworkWrapper::FilmGrain
 * (FUN_40e44bec).
 *
 * Properties:
 *   - Pure integer math: 0 floating-point instructions anywhere.
 *   - Zero malloc, zero writable .data/.bss, <= 1024 B static stack frame.
 *   - Caller-owned scratch workspace: 80 KiB (<= 96 KiB budget, 16-byte aligned).
 *   - Operates strictly on 8-bit luma Plane 0 (Y); Cb/Cr are never touched.
 *   - Supports in-place (NV16 / NV12 / I420 Y) and out-of-place (Live copy+grain).
 *   - Dual blur mode: Zf Exact (0-diff vs CEVA-XM6 DSP) & Sharp (delta-only blur).
 *   - Map injection via validated ZFG1 card asset (ZF_GRAIN.BIN, 27,588 bytes)
 *     so EL1 .rodata stays tiny.
 *
 * Header compatibility:
 *   Safe to include alongside z8_filmsim_abi.h in either order (zero macro or
 *   enumerator collisions; blur mode and Fuji grain numeric values match 1:1).
 */
#ifndef Z8_ZF_GRAIN_H
#define Z8_ZF_GRAIN_H

#include <stddef.h>
#include <stdint.h>

#define Z8_ZF_GRAIN_ABI                 1U
#define Z8_ZF_GRAIN_MAX_PLANES          7U
#define Z8_ZF_GRAIN_MAX_WIDTH           8256U
#define Z8_ZF_GRAIN_MAX_HEIGHT          8256U
#define Z8_ZF_GRAIN_WORKSPACE_BYTES     81920U  /* 80 KiB (<= 96 KiB budget) */
#define Z8_ZF_GRAIN_WORKSPACE_ALIGNMENT 16U

/* Fixed Nikon Zf C3.01 ARM64/CEVA-XM6 DSP constants */
#define Z8_ZF_GRAIN_MAP_DIM             83U
#define Z8_ZF_GRAIN_MAP_ELEMENTS        6889U   /* 83 * 83 */
#define Z8_ZF_GRAIN_MAP_BYTES           13778U  /* 83 * 83 * sizeof(int16_t) */
#define Z8_ZF_GRAIN_TILE_PERIOD         5U      /* P */
#define Z8_ZF_GRAIN_TILE_STEP           23U     /* S */
#define Z8_ZF_GRAIN_DEFAULT_KNEE_LO     32U
#define Z8_ZF_GRAIN_DEFAULT_KNEE_HI     223U
#define Z8_ZF_GRAIN_DEFAULT_PRNG_SEED   UINT32_C(0x12345678)
#define Z8_ZF_GRAIN_BLUR_VALID_MASK     0x2A9U  /* allows {0, 3, 5, 7, 9} */
#define Z8_ZF_GRAIN_INTENSITY_MAX       6U
#define Z8_ZF_GRAIN_SIZE_MIN            1U
#define Z8_ZF_GRAIN_SIZE_MAX            3U
#define Z8_ZF_GRAIN_MAX_BLEND_W         16U

/* Card binary asset format ZFG1 (A:\FENESTRA\ZF_GRAIN.BIN) */
#define Z8_ZF_GRAIN_FILE_MAGIC          UINT32_C(0x3147465A) /* 'Z','F','G','1' LE */
#define Z8_ZF_GRAIN_FILE_VERSION        1U
#define Z8_ZF_GRAIN_HEADER_BYTES        32U
#define Z8_ZF_GRAIN_MAP_COUNT           2U
#define Z8_ZF_GRAIN_PAYLOAD_BYTES       27556U  /* 2 * 13,778 B */
#define Z8_ZF_GRAIN_FILE_BYTES          27588U  /* 32 B header + 27,556 B payload */

/* V71 Live Pre-Baked Seamless Twisted-Torus Grain Tile (128 KiB, 64B aligned twist) */
#define Z8_ZF_LIVE_TILE_W               512U
#define Z8_ZF_LIVE_TILE_H               256U
#define Z8_ZF_LIVE_TILE_TWIST           192U    /* 64B aligned, C^inf seamless torus */
#define Z8_ZF_LIVE_TILE_BYTES           131072U /* 512 * 256 int8_t */

/* Dual blur mode (matches enum Z8ZfBlurMode numeric values in z8_filmsim_abi.h) */
enum z8_zf_grain_blur_mode {
    Z8_ZF_GRAIN_BLUR_EXACT = 0U, /* Bit-exact Zf C3.01 DSP (BlurLineRing on Y_noisy) */
    Z8_ZF_GRAIN_BLUR_SHARP = 1U  /* Blur signed delta only (unbiased rounding), preserve 45MP Y */
};

/* Per-plane resolution mode in multi-plane exposure group jobs */
enum z8_zf_grain_res_mode {
    Z8_ZF_GRAIN_RES_AUTO    = 0U, /* Full-res on primary/largest plane(s), reduced on smaller */
    Z8_ZF_GRAIN_RES_FULL    = 1U, /* Force full-res LUT (w={2,3,7,11,16,16}, blur={3,5,9}) */
    Z8_ZF_GRAIN_RES_REDUCED = 2U  /* Force reduced-res LUT (w={2,2,3,4,6,9}, blur={3,3,3}, Map0) */
};

enum z8_zf_grain_result {
    Z8_ZF_GRAIN_OK              =   0,
    Z8_ZF_GRAIN_DISABLED        =   1, /* Full bypass: 0 bytes of planes/workspace touched */
    Z8_ZF_GRAIN_CANCELLED       =   2, /* Cancelled before any pixel write */
    Z8_ZF_GRAIN_CANCELLED_DIRTY =   3, /* Cancelled mid-frame; partial writes occurred */
    Z8_ZF_GRAIN_E_ARGUMENT      =  -1,
    Z8_ZF_GRAIN_E_FORMAT        =  -2,
    Z8_ZF_GRAIN_E_CAPACITY      =  -3,
    Z8_ZF_GRAIN_E_ALIAS         =  -4,
    Z8_ZF_GRAIN_E_WORKSPACE     =  -5,
    Z8_ZF_GRAIN_E_MAPS          =  -6,
    Z8_ZF_GRAIN_E_MAGIC         =  -7,
    Z8_ZF_GRAIN_E_VERSION       =  -8,
    Z8_ZF_GRAIN_E_CRC           =  -9,
    Z8_ZF_GRAIN_E_ALIGNMENT     = -10
};

/* On-card ZFG1 32-byte header (little-endian). */
typedef struct {
    uint8_t  magic[4];      /* 'Z', 'F', 'G', '1' */
    uint16_t version;       /* Z8_ZF_GRAIN_FILE_VERSION (1) */
    uint16_t header_bytes;  /* Z8_ZF_GRAIN_HEADER_BYTES (32) */
    uint16_t map_dim;       /* Z8_ZF_GRAIN_MAP_DIM (83) */
    uint16_t map_count;     /* Z8_ZF_GRAIN_MAP_COUNT (2) */
    uint32_t payload_bytes; /* Z8_ZF_GRAIN_PAYLOAD_BYTES (27556) */
    uint32_t maps_crc32;    /* IEEE 802.3 CRC32 over map0 || map1 (27,556 B) */
    uint32_t header_crc32;  /* IEEE 802.3 CRC32 over 32 B header with this field zeroed */
    uint8_t  tile_period;   /* 5 */
    uint8_t  tile_step;     /* 23 */
    uint8_t  knee_lo;       /* 32 */
    uint8_t  knee_hi;       /* 223 */
    uint8_t  reserved[4];   /* Must be 0 */
} z8_zf_grain_file_header;

_Static_assert(sizeof(z8_zf_grain_file_header) == Z8_ZF_GRAIN_HEADER_BYTES,
               "z8_zf_grain_file_header must be 32 bytes");

/* Complete on-card ZF_GRAIN.BIN binary image (27,588 bytes). */
typedef struct {
    z8_zf_grain_file_header header;
    int16_t                 map0[Z8_ZF_GRAIN_MAP_ELEMENTS]; /* map0_std (13,778 B) */
    int16_t                 map1[Z8_ZF_GRAIN_MAP_ELEMENTS]; /* map1_hi  (13,778 B) */
} z8_zf_grain_file;

_Static_assert(sizeof(z8_zf_grain_file) == Z8_ZF_GRAIN_FILE_BYTES,
               "z8_zf_grain_file must be 27,588 bytes");

/* Validated read-only map view injected by caller (card-loaded ZF_GRAIN.BIN or test buffer). */
typedef struct {
    const int16_t *map0; /* 83x83 int16_t (map0_std, >= 2-byte aligned) */
    const int16_t *map1; /* 83x83 int16_t (map1_hi,  >= 2-byte aligned) */
} z8_zf_grain_maps;

/* High-level Nikon Zf 18-level preset (Size 1..3 x Intensity 0..6). */
typedef struct {
    uint8_t intensity;   /* 0=Off (bypass), 1..6 */
    uint8_t size;        /* 1=Small, 2=Medium, 3=Large (ignored when intensity==0) */
    uint8_t blur_mode;   /* enum z8_zf_grain_blur_mode (0=Exact, 1=Sharp) */
    uint8_t reduced_res; /* 0=Full-res still, 1=Reduced-res (Live / previews / thumbnails) */
} z8_zf_grain_preset;

/* Low-level CEVA-XM6 DSP FilmGrain() parameters (direct control & golden vector tests). */
typedef struct {
    uint8_t bypass;     /* 1=bypass (returns Z8_ZF_GRAIN_DISABLED), 0=active */
    uint8_t blend_w;    /* Q4 mapA weight [0..16] (mapB == 0) */
    uint8_t blur_size;  /* Allowed by 0x2A9 mask: {0, 3, 5, 7, 9} */
    uint8_t map_select; /* 0=maps->map0, 1=maps->map1 */
    uint8_t knee_lo;    /* Q6 shadow knee [0..255], standard Zf = 32 */
    uint8_t knee_hi;    /* Q6 highlight knee [0..255], standard Zf = 223 */
    uint8_t blur_mode;  /* enum z8_zf_grain_blur_mode (0=Exact, 1=Sharp) */
    uint8_t reserved;   /* Must be 0 */
} z8_zf_grain_dsp_params;

/* Single-plane in-place (src_y == dst_y) OR out-of-place (disjoint src_y/dst_y) job.
 * Operates strictly on 8-bit luma Plane 0 (Y); Cb/Cr are untouched.
 */
typedef struct {
    uint32_t                abi;          /* Z8_ZF_GRAIN_ABI (1) */
    const z8_zf_grain_maps *maps;         /* Validated 83x83 map pair */
    z8_zf_grain_dsp_params  params;       /* Resolved DSP parameters */
    const uint8_t          *src_y;        /* Source Y plane (may equal dst_y for in-place) */
    size_t                  src_y_bytes;  /* Source backing size in bytes */
    uint32_t                src_y_stride; /* Source bytes per row (>= width) */
    uint8_t                *dst_y;        /* Destination Y plane (may equal src_y for in-place) */
    size_t                  dst_y_bytes;  /* Destination backing size in bytes */
    uint32_t                dst_y_stride; /* Destination bytes per row (>= width) */
    uint32_t                width;        /* 1..8256 */
    uint32_t                height;       /* 1..8256 */
    uint32_t                prng_seed;    /* xorshift32 seed (0 -> 0x12345678) */
    int                   (*cancelled)(void *context);
    void                   *cancel_context;
} z8_zf_grain_plane_job;

/* Per-plane descriptor for multi-plane in-place photo exposure group. */
typedef struct {
    uint8_t *y;           /* Writable 8-bit luma Plane 0 */
    size_t   y_bytes;     /* Backing size in bytes */
    uint32_t width;       /* 1..8256 */
    uint32_t height;      /* 1..8256 */
    uint32_t y_stride;    /* Bytes per row (>= width) */
    uint8_t  res_mode;    /* enum z8_zf_grain_res_mode (0=Auto, 1=Full, 2=Reduced) */
    uint8_t  reserved[3]; /* Must be 0 */
} z8_zf_grain_plane;

/* Multi-plane in-place exposure-group job (1..7 NV16 / NV12 / I420 Y planes). */
typedef struct {
    uint32_t                abi;      /* Z8_ZF_GRAIN_ABI (1) */
    uint32_t                count;    /* 1..Z8_ZF_GRAIN_MAX_PLANES */
    uint64_t                shot_id;  /* Deterministic seed basis */
    const z8_zf_grain_maps *maps;     /* Validated 83x83 map pair */
    z8_zf_grain_preset      preset;   /* Zf preset (or output of z8_zf_grain_from_fuji) */
    z8_zf_grain_plane       planes[Z8_ZF_GRAIN_MAX_PLANES];
    int                   (*cancelled)(void *context);
    void                   *cancel_context;
} z8_zf_grain_group_job;

/* IEEE 802.3 CRC32 (matches Python zlib.crc32). Pass crc=0 to start. */
uint32_t z8_zf_grain_crc32_update(uint32_t crc, const void *data, size_t bytes);

/* Validate a card-loaded ZFG1 binary buffer (27,588 bytes, >= 2-byte aligned)
 * and bind zero-copy pointers into maps_out.
 */
int z8_zf_grain_validate_bin(const void *buffer, size_t bytes, z8_zf_grain_maps *maps_out);

/* Fold a 64-bit shot/frame identifier into a deterministic non-zero xorshift32 seed. */
uint32_t z8_zf_grain_fold_seed(uint64_t shot_id);

/* Resolve high-level Nikon Zf preset (Size 1..3, Intensity 0..6, Full/Reduced)
 * into exact ARM64/XM6 DSP scalar parameters.
 */
int z8_zf_grain_resolve_preset(const z8_zf_grain_preset *preset,
                               z8_zf_grain_dsp_params *out);

/* Apply Zf C3.01 Film Grain to a single 8-bit luma plane (in-place or out-of-place).
 * Returns Z8_ZF_GRAIN_DISABLED without touching planes or workspace if params.bypass == 1.
 */
int z8_zf_grain_process_plane(const z8_zf_grain_plane_job *job,
                              void *workspace, size_t workspace_bytes);

/* Apply Zf C3.01 Film Grain in-place across 1..7 luma planes in an exposure group.
 * Validates all planes before any pixel write. Returns Z8_ZF_GRAIN_DISABLED
 * without touching planes or workspace when preset.intensity == 0.
 */
int z8_zf_grain_apply_group(const z8_zf_grain_group_job *job,
                            void *workspace, size_t workspace_bytes);

/* Pre-bake a 512x256 seamless twisted-torus int8 grain delta tile (twist_dx=192)
 * matching preset using ZF_GRAIN.BIN maps, xorshift32 PRNG, and exact
 * twisted-toroidal blur. Returns Z8_ZF_GRAIN_DISABLED without touching buffers
 * if preset->intensity == 0.
 */
int z8_zf_grain_live_bake_tile(const z8_zf_grain_preset *preset,
                               const z8_zf_grain_maps *maps,
                               uint64_t seed,
                               int8_t *tile_out, size_t tile_bytes,
                               void *workspace, size_t workspace_bytes);

/* Apply pre-baked 512x256 twisted-torus int8 grain tile in-place to 8-bit luma Y
 * using row_x0 = ((y >> 8) * 192u) & 511u and standard Zf knees (32..223) with
 * srshr #6 rounding. 100% SCTLR_EL1.A==1 safe for arbitrary alignment/stride/width.
 */
void z8_zf_grain_live_apply_tile_y(uint8_t *y, uint32_t y_stride,
                                   uint32_t width, uint32_t height,
                                   const int8_t *tile512x256);

/* Row-range variant for SMP stripe dispatch (row_y in [first_row, end_row));
 * preserves exact global row twist ((row_y >> 8) * 192) for 0-diff parity. */
void z8_zf_grain_live_apply_tile_y_rows(uint8_t *y, uint32_t y_stride,
                                        uint32_t width,
                                        uint32_t first_row, uint32_t end_row,
                                        const int8_t *tile512x256);

/* V71.7-r7 fused grain + L1-hot per-row dc cvac + end-of-stripe dsb sy variant. */
void z8_zf_grain_live_apply_tile_y_rows_clean(uint8_t *y, uint32_t y_stride,
                                              uint32_t width,
                                              uint32_t first_row, uint32_t end_row,
                                              const int8_t *tile512x256,
                                              int clean_dcache);

#endif /* Z8_ZF_GRAIN_H */
