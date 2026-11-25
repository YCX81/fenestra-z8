/* SPDX-License-Identifier: MIT
 * Frozen Shared ABI Contract between EL0 filmsim.wlm and EL1 z8_filmsim.
 * Keep work/filmsim/src/include/z8_filmsim_abi.h and
 * work/wlm-zh/filmsim/filmsim_abi.h byte-identical (verified by SHA-256).
 */
#ifndef Z8_FILMSIM_ABI_H
#define Z8_FILMSIM_ABI_H

#include <stddef.h>
#include <stdint.h>

#define Z8_FILMSIM_ABI               1U
#define Z8_FILMSIM_CFG_MAGIC         UINT32_C(0x3153465a) /* 'ZFS1' */
#define Z8_FILMSIM_CFG_VERSION       1U
#define Z8_FILMSIM_CFG_BYTES         64U
#define Z8_FILMSIM_STATUS_BYTES      64U

#define Z8_XM6_DIAG_MAGIC            UINT32_C(0x4458385a) /* 'Z8XD' */
#define Z8_XM6_DIAG_VERSION          1U
#define Z8_XM6_DIAG_BYTES            64U

#define Z8_FILMSIM_BINDING_NAME      "film.sim"
#define Z8_FILMSIM_BINDING_VER       1U
#define Z8_FILMSIM_MODULE_ID         "filmsim"

#define Z8_FILMSIM_CFG_PATH_0        "A:\\FENESTRA\\FLMSIM0.CFG"
#define Z8_FILMSIM_CFG_PATH_1        "A:\\FENESTRA\\FLMSIM1.CFG"
#define Z8_FILMSIM_RECIPE_PATH_0     "A:\\FENESTRA\\FLMREC0.CFG"
#define Z8_FILMSIM_RECIPE_PATH_1     "A:\\FENESTRA\\FLMREC1.CFG"
#define Z8_FILMSIM_ZF_GRAIN_PATH     "A:\\FENESTRA\\ZF_GRAIN.BIN"
#define Z8_FILMSIM_XM6_DIAG_LOG_PATH "A:\\FENESTRA\\XM6_DIAG.LOG"
#define Z8_FILMSIM_LUT_DIR           "A:\\FENESTRA\\LUTS"

/* Valid parameter ranges and canonical defaults */
#define Z8_ZF_INTENSITY_MIN          0U
#define Z8_ZF_INTENSITY_MAX          6U
#define Z8_ZF_SIZE_MIN               1U
#define Z8_ZF_SIZE_MAX               3U
#define Z8_ZF_SIZE_DEFAULT           2U   /* Medium (1=Small, 2=Medium, 3=Large) */
#define Z8_ZF_BLUR_DEFAULT           1U   /* Sharp (0=Zf Exact, 1=Sharp) */
#define Z8_FUJI_LUT_SLOT_MAX         16U  /* Legacy wire bound (Fuji stripped in V71.3) */
#define Z8_FUJI_LUT_STRENGTH_MAX     100U
#define Z8_FUJI_LUT_STRENGTH_DEFAULT 100U

/* NP3 Per-Slot Recipe Binding constants (V71.5) */
#define Z8_NP3_RECIPE_MAGIC          UINT32_C(0x5846385a) /* 'Z8FX' */
#define Z8_NP3_RECIPE_VERSION        1U
#define Z8_NP3_RECIPE_BYTES          16U
#define Z8_NP3_RECIPE_FLAG_ENABLED   (1U << 0)

#define Z8_NP3_TABLE_MAGIC           UINT32_C(0x3152465a) /* 'ZFR1' */
#define Z8_NP3_TABLE_VERSION         1U
#define Z8_NP3_SLOT_COUNT            95U  /* 40 Stock+C1..C9 (0x00..0x27) + 55 C10..C64 (0x1009..0x103F) */
#define Z8_NP3_STOCK_AND_C9_COUNT    40U  /* Canonical slots 0..39  <-> selectors 0x00..0x27 */
#define Z8_NP3_C64_FIRST_SLOT        40U  /* Canonical slots 40..94 <-> C10..C64 (0x1009..0x103F) */
#define Z8_NP3_C64_FIRST_INDEX       9U   /* SC_RECORD index 9..63 for C10..C64 */
#define Z8_NP3_C64_LAST_INDEX        63U
#define Z8_NP3_SLOT_INVALID          UINT32_C(0xffffffff)
#define Z8_NP3_SEL_ACTIVE_QUERY      UINT32_C(0xffffffff)
#define Z8_NP3_ENTRY_BYTES           8U
#define Z8_NP3_TABLE_BYTES           (16U + Z8_NP3_SLOT_COUNT * Z8_NP3_ENTRY_BYTES) /* 776 B */
#define Z8_NP3_CHUNK_MAX_SLOTS       64U  /* 16 + 64*8 = 528 B <= FX_SERVICE_BYTES (768 B) */
#define Z8_NP3_CHUNK_BYTES           (16U + Z8_NP3_CHUNK_MAX_SLOTS * Z8_NP3_ENTRY_BYTES) /* 528 B */
#define Z8_NP3_MSG_BYTES             24U
#define Z8_NP3_QUERY_BYTES           8U

/* Z8 C3.11 DRAM locations for O(1) EL1 active Picture Control & C10..C64 SC_RECORD inspection */
#define Z8_NP3_ACTIVE_SEL_ADDR       UINT64_C(0xa02983cf8) /* uint32_t[2]: [0]=photo, [1]=video */
#define Z8_NP3_C64_DRAM_BASE         UINT64_C(0xa1166b080) /* 128 x 1024 B SC_RECORD table */
#define Z8_NP3_SC_RECORD_STRIDE      UINT32_C(0x400)       /* 1024 B per slot */
#define Z8_NP3_SC_RECORD_RECIPE_OFF  UINT32_C(0x39c)       /* +0x39C..0x3AB: 16 B Z8Np3Recipe */

enum Z8FilmSimOp {
    Z8_FS_OP_GET               = 1U,  /* Reply payload: 64B Z8FilmSimConfig */
    Z8_FS_OP_SET               = 2U,  /* Request payload: 64B Z8FilmSimConfig */
    Z8_FS_OP_STATUS            = 3U,  /* Reply payload: 64B Z8FilmSimStatus */
    Z8_FS_OP_SET_RECIPE        = 4U,  /* Request payload: 24B Z8FilmSimRecipeMsg */
    Z8_FS_OP_GET_RECIPE        = 5U,  /* Request: 8B Z8FilmSimRecipeQuery -> Reply: 24B Z8FilmSimRecipeMsg */
    Z8_FS_OP_SYNC_RECIPES      = 6U,  /* Request payload: 528B Z8FilmSimRecipeChunk (bulk sync) */
    Z8_FS_OP_XM6_DIAG          = 10U, /* Request: 0B -> Reply: 64B Z8Xm6DiagResult */
    Z8_FS_OP_XM6_RESET_BREAKER = 11U  /* Request: 0B -> Reply: 64B Z8FilmSimStatus */
};

/* slots.state EL1 IPC tunnel (V71.6-XM6 / V71.7-r4 unified router: ops 32..79, z8_op 0..47):
 *   ops 32..47 (z8_op  0..15): filmsim.wlm   magic 'FSI1' (0x31495346)
 *   ops 48..63 (z8_op 16..31): livephoto.wlm magic 'LPI1' (0x3149504C)
 *   ops 64..79 (z8_op 32..47): lut.wlm       magic 'LTI1' (0x3149544C)
 * Request wire format: 16B header {magic, req_bytes, 0, 0} + req_bytes payload
 * (satisfies slots.state q->bytes >= 16 check; max 16 + 528 = 544 B <= FX_SERVICE_BYTES 768). */
#define Z8_FS_SLOTS_OP_BASE          32U
#define Z8_FS_SLOTS_OP_COUNT         16U
#define Z8_SLOTS_ROUTER_OP_COUNT     48U /* slots.state ops 32..79 -> z8_op 0..47 */
#define Z8_LP_SLOTS_OP_BASE          48U
#define Z8_LUT_SLOTS_OP_BASE         64U
#define Z8_FS_SLOTS_TUNNEL_MAGIC     UINT32_C(0x31495346) /* "FSI1" */
#define Z8_LP_SLOTS_TUNNEL_MAGIC     UINT32_C(0x3149504c) /* "LPI1" */
#define Z8_LUT_SLOTS_TUNNEL_MAGIC    UINT32_C(0x3149544c) /* "LTI1" */
#define Z8_FS_SLOTS_HDR_BYTES        16U

/* CEVA-XM6 Core 0 hardware acceleration mode (V71.6, stored at Z8FilmSimConfig+28) */
enum Z8Xm6Mode {
    Z8_XM6_MODE_AUTO     = 0U, /* Default (0-byte compat with legacy FLMSIM0/1.CFG): XM6 preferred, ARM NEON fallback */
    Z8_XM6_MODE_ARM_ONLY = 1U  /* Force ARM NEON (bypass CEVA-XM6 Core 0) */
};

/* Display Layer 0 live viewfinder WYSIWYG preview toggle (V71.7-r2, stored at Z8FilmSimConfig+29) */
enum Z8WysiwygPreview {
    Z8_WYSIWYG_OFF = 0U, /* Default (0-byte compat with legacy FLMSIM0/1.CFG): Stock 60/120Hz viewfinder */
    Z8_WYSIWYG_ON  = 1U  /* Render live Ricoh HDF + Zf Grain on Rear LCD & EVF Layer 0 */
};

/* Bitmask for Z8FilmSimStatus.xm6_hw_state (+42) */
#define Z8_XM6_HW_STATE_MASK         0x7FU /* low 7 bits = *(u32*)0x0a02e7e734 & 0x7F */
#define Z8_XM6_HW_LOADER_FAULT_BIT   0x80U /* bit7 = loader_fault != 0 */

/* Bitmask for Z8FilmSimStatus.xm6_runtime_flags (+43) */
enum Z8Xm6RuntimeFlags {
    Z8_XM6_FLAG_AUTO_ENABLED    = (1U << 0), /* bit0: xm6_mode == Z8_XM6_MODE_AUTO */
    Z8_XM6_FLAG_HW_READY        = (1U << 1), /* bit1: Core 0 state==2, fw_loaded!=0, loader_fault==0 */
    Z8_XM6_FLAG_BREAKER_TRIPPED = (1U << 2), /* bit2: One-shot circuit breaker tripped */
    Z8_XM6_FLAG_DIAG_RAN        = (1U << 3), /* bit3: Z8_FS_OP_XM6_DIAG ran at least once */
    Z8_XM6_FLAG_DIAG_PASSED     = (1U << 4)  /* bit4: Last Z8_FS_OP_XM6_DIAG passed 4/4 with 0-diff */
};

/* Error codes for Z8FilmSimStatus.xm6_last_err (+44) */
enum Z8Xm6Error {
    Z8_XM6_ERR_OK           = 0U,
    Z8_XM6_ERR_TIMEOUT      = 1U,
    Z8_XM6_ERR_DSP_ERR      = 2U,
    Z8_XM6_ERR_IOMMU_OOB    = 3U,
    Z8_XM6_ERR_ENQUEUE_FAIL = 4U,
    Z8_XM6_ERR_NOT_READY    = 5U,
    Z8_XM6_ERR_BUSY         = 6U,
    Z8_XM6_ERR_ALLOC_FAIL   = 7U
};

/* Bitmask for Z8Xm6DiagResult.passed_mask (+12) and ran_mask (+14) */
enum Z8Xm6DiagMask {
    Z8_XM6_DIAG_GRAIN_EXACT = (1U << 0), /* bit0: Slot 39 Zf Exact (128x128 NV16) */
    Z8_XM6_DIAG_GRAIN_SHARP = (1U << 1), /* bit1: Slot 39 Zf Sharp (128x128 NV16) */
    Z8_XM6_DIAG_HDF_STILL   = (1U << 2), /* bit2: Slot 41 HDF STILL_GENERAL (128x128 NV16) */
    Z8_XM6_DIAG_HDF_LIVE    = (1U << 3), /* bit3: Slot 41 HDF LIVE_256_YC (1024x680 NV12) */
    Z8_XM6_DIAG_ALL_MASK    = 0x0FU
};

enum Z8FilmSimApplyMode {
    Z8_FS_APPLY_GLOBAL  = 0U, /* Default: apply global Z8FilmSimConfig to every shot (legacy compat) */
    Z8_FS_APPLY_PER_NP3 = 1U  /* Auto-switch recipe by active Picture Control / NP3; Dormant if unbound */
};

enum Z8Np3RecipeSource {
    Z8_NP3_SRC_NONE      = 0U, /* Global mode or active NP3 has no bound recipe */
    Z8_NP3_SRC_SC_RECORD = 1U, /* Bound via C10..C64 SC_RECORD +0x39C ('Z8FX') */
    Z8_NP3_SRC_FLMREC    = 2U  /* Bound via 95-slot FLMREC0/1.CFG ('ZFR1') table */
};

enum Z8FilmStyle {
    Z8_STYLE_OFF      = 0U, /* Film style bypassed (HDF may still run orthogonally) */
    Z8_STYLE_NIKON_ZF = 1U, /* Nikon Zf C3.01 18-level Film Grain */
    Z8_STYLE_FUJIFILM = 2U, /* Legacy wire value (normalized to OFF) */
    Z8_STYLE_FUJI_ZF  = 3U  /* Legacy wire value (normalized to NIKON_ZF) */
};

enum Z8ZfBlurMode {
    Z8_ZF_BLUR_EXACT = 0U, /* Bit-exact Zf C3.01 DSP (BlurLineRing on Y_noisy) */
    Z8_ZF_BLUR_SHARP = 1U  /* Blur noise delta only to preserve 45MP sharpness */
};

enum Z8FujiChrome {
    Z8_CHROME_OFF    = 0U,
    Z8_CHROME_WEAK   = 1U,
    Z8_CHROME_STRONG = 2U
};

enum Z8FujiGrain {
    Z8_FUJI_GRAIN_OFF      = 0U,
    Z8_FUJI_GRAIN_WEAK_S   = 1U,
    Z8_FUJI_GRAIN_WEAK_L   = 2U,
    Z8_FUJI_GRAIN_STRONG_S = 3U,
    Z8_FUJI_GRAIN_STRONG_L = 4U
};

enum Z8HdfLevel {
    Z8_HDF_OFF    = 0U,
    Z8_HDF_WEAK   = 1U, /* Ricoh Optical HDF Weak (1/8) */
    Z8_HDF_MEDIUM = 2U, /* Ricoh Optical HDF Medium (1/4) */
    Z8_HDF_STRONG = 3U  /* Ricoh Optical HDF Strong (1/2) */
};

enum Z8LiveLink {
    Z8_LIVE_LINK_OFF = 0U,
    Z8_LIVE_LINK_ON  = 1U
};

enum Z8FilmSimStatusFlags {
    Z8_FS_FLAG_EL1_READY      = (1U << 0), /* bit0: EL1 runtime ready */
    Z8_FS_FLAG_ZF_MAPS_LOADED = (1U << 1), /* bit1: ZF_GRAIN.BIN loaded & CRC verified */
    Z8_FS_FLAG_LUT_LOADED     = (1U << 2), /* bit2: Legacy LUT bit (always 0) */
    Z8_FS_FLAG_DORMANT        = (1U << 3), /* bit3: Dormant bypass active (0 RAM alloc) */
    Z8_FS_FLAG_LIVE_ACTIVE    = (1U << 4), /* bit4: Live Photo Link filter active */
    Z8_FS_FLAG_PER_NP3_MODE   = (1U << 5), /* bit5: apply_mode == Z8_FS_APPLY_PER_NP3 */
    Z8_FS_FLAG_RECIPE_BOUND   = (1U << 6)  /* bit6: Active NP3 slot has an enabled recipe */
};

typedef struct {
    uint32_t magic;             /* Z8_FILMSIM_CFG_MAGIC ('ZFS1') */
    uint16_t version;           /* Z8_FILMSIM_CFG_VERSION (1) */
    uint16_t bytes;             /* sizeof(Z8FilmSimConfig) == 64 */
    uint32_t seq;               /* Monotonic save generation for dual-slot selection */
    uint32_t crc32;             /* IEEE 802.3 CRC32 over 64B with crc32 field zeroed */
    uint8_t  film_style;        /* enum Z8FilmStyle (0..3) */
    uint8_t  zf_intensity;      /* 0..6 (0=Off, 1..6) */
    uint8_t  zf_size;           /* 1..3 (1=Small, 2=Medium, 3=Large) */
    uint8_t  zf_blur_mode;      /* enum Z8ZfBlurMode (0..1) */
    uint8_t  fuji_chrome;       /* Legacy enum Z8FujiChrome (0..2) */
    uint8_t  fuji_fx_blue;      /* Legacy enum Z8FujiChrome (0..2) */
    uint8_t  fuji_lut_slot;     /* Legacy 0..16 */
    uint8_t  fuji_lut_strength; /* Legacy 0..100 (default 100) */
    uint8_t  fuji_grain;        /* Legacy enum Z8FujiGrain (0..4) */
    uint8_t  hdf_level;         /* enum Z8HdfLevel (0..3), orthogonal to film_style */
    uint8_t  live_link;         /* enum Z8LiveLink (0..1) */
    uint8_t  apply_mode;        /* enum Z8FilmSimApplyMode (0=Global, 1=Per-NP3) */
    uint8_t  xm6_mode;          /* enum Z8Xm6Mode (0=Auto XM6, 1=ARM Only) */
    uint8_t  wysiwyg_preview;   /* enum Z8WysiwygPreview (0=Off, 1=On) */
    uint8_t  reserved[34];      /* Must be 0 */
} Z8FilmSimConfig;

_Static_assert(sizeof(Z8FilmSimConfig) == Z8_FILMSIM_CFG_BYTES,
               "Z8FilmSimConfig must be 64 bytes");
_Static_assert(offsetof(Z8FilmSimConfig, magic) == 0U, "cfg.magic offset");
_Static_assert(offsetof(Z8FilmSimConfig, version) == 4U, "cfg.version offset");
_Static_assert(offsetof(Z8FilmSimConfig, bytes) == 6U, "cfg.bytes offset");
_Static_assert(offsetof(Z8FilmSimConfig, seq) == 8U, "cfg.seq offset");
_Static_assert(offsetof(Z8FilmSimConfig, crc32) == 12U, "cfg.crc32 offset");
_Static_assert(offsetof(Z8FilmSimConfig, film_style) == 16U, "cfg.film_style offset");
_Static_assert(offsetof(Z8FilmSimConfig, zf_intensity) == 17U, "cfg.zf_intensity offset");
_Static_assert(offsetof(Z8FilmSimConfig, zf_size) == 18U, "cfg.zf_size offset");
_Static_assert(offsetof(Z8FilmSimConfig, zf_blur_mode) == 19U, "cfg.zf_blur_mode offset");
_Static_assert(offsetof(Z8FilmSimConfig, fuji_chrome) == 20U, "cfg.fuji_chrome offset");
_Static_assert(offsetof(Z8FilmSimConfig, fuji_fx_blue) == 21U, "cfg.fuji_fx_blue offset");
_Static_assert(offsetof(Z8FilmSimConfig, fuji_lut_slot) == 22U, "cfg.fuji_lut_slot offset");
_Static_assert(offsetof(Z8FilmSimConfig, fuji_lut_strength) == 23U, "cfg.fuji_lut_strength offset");
_Static_assert(offsetof(Z8FilmSimConfig, fuji_grain) == 24U, "cfg.fuji_grain offset");
_Static_assert(offsetof(Z8FilmSimConfig, hdf_level) == 25U, "cfg.hdf_level offset");
_Static_assert(offsetof(Z8FilmSimConfig, live_link) == 26U, "cfg.live_link offset");
_Static_assert(offsetof(Z8FilmSimConfig, apply_mode) == 27U, "cfg.apply_mode offset");
_Static_assert(offsetof(Z8FilmSimConfig, xm6_mode) == 28U, "cfg.xm6_mode offset");
_Static_assert(offsetof(Z8FilmSimConfig, wysiwyg_preview) == 29U, "cfg.wysiwyg_preview offset");
_Static_assert(offsetof(Z8FilmSimConfig, reserved) == 30U, "cfg.reserved offset");

typedef struct {
    uint32_t magic;               /* Z8_FILMSIM_CFG_MAGIC ('ZFS1') */
    uint16_t version;             /* Z8_FILMSIM_CFG_VERSION (1) */
    uint16_t bytes;               /* sizeof(Z8FilmSimStatus) == 64 */
    uint32_t flags;               /* enum Z8FilmSimStatusFlags */
    uint32_t active_lut_slot;     /* Legacy 0..16 (0 in V71.3+) */
    int32_t  last_error;          /* 0=OK, negative=error */
    uint32_t photo_applied;
    uint32_t live_applied;
    uint32_t photo_skipped;
    uint32_t live_skipped;
    uint32_t active_pc_sel;       /* Current photo Picture Control selector (0xA02983CF8[0]) */
    uint8_t  active_recipe_src;   /* enum Z8Np3RecipeSource (0=None, 1=SC_RECORD, 2=FLMREC) */
    uint8_t  active_recipe_bound; /* 0=unbound, 1=active NP3 has valid enabled recipe */
    uint8_t  xm6_hw_state;        /* low7: *(u32*)0x0a02e7e734 & 0x7F; bit7: loader_fault != 0 */
    uint8_t  xm6_runtime_flags;   /* enum Z8Xm6RuntimeFlags (bit0..4) */
    uint16_t xm6_last_err;        /* enum Z8Xm6Error (0..7) */
    uint16_t xm6_last_still_ms;   /* Last XM6 still duration in ms (clamped 0..65535) */
    uint32_t xm6_still_ok;        /* Successful XM6 still dispatches */
    uint32_t xm6_live_ok;         /* Successful XM6 live dispatches */
    uint32_t xm6_fallback_cnt;    /* Fallback-to-ARM-NEON counter (when XM6 auto enabled) */
    uint16_t xm6_last_live_us;    /* Last XM6 live duration in us (clamped 0..65535) */
    uint8_t  active_drive;        /* Active boot card drive 'A'/'B' from CSL1 (0 if unset) */
    uint8_t  xm6_reserved;        /* Must be 0 */
} Z8FilmSimStatus;

_Static_assert(sizeof(Z8FilmSimStatus) == Z8_FILMSIM_STATUS_BYTES,
               "Z8FilmSimStatus must be 64 bytes");
_Static_assert(offsetof(Z8FilmSimStatus, magic) == 0U, "status.magic offset");
_Static_assert(offsetof(Z8FilmSimStatus, version) == 4U, "status.version offset");
_Static_assert(offsetof(Z8FilmSimStatus, bytes) == 6U, "status.bytes offset");
_Static_assert(offsetof(Z8FilmSimStatus, flags) == 8U, "status.flags offset");
_Static_assert(offsetof(Z8FilmSimStatus, active_lut_slot) == 12U, "status.active_lut_slot offset");
_Static_assert(offsetof(Z8FilmSimStatus, last_error) == 16U, "status.last_error offset");
_Static_assert(offsetof(Z8FilmSimStatus, photo_applied) == 20U, "status.photo_applied offset");
_Static_assert(offsetof(Z8FilmSimStatus, live_applied) == 24U, "status.live_applied offset");
_Static_assert(offsetof(Z8FilmSimStatus, photo_skipped) == 28U, "status.photo_skipped offset");
_Static_assert(offsetof(Z8FilmSimStatus, live_skipped) == 32U, "status.live_skipped offset");
_Static_assert(offsetof(Z8FilmSimStatus, active_pc_sel) == 36U, "status.active_pc_sel offset");
_Static_assert(offsetof(Z8FilmSimStatus, active_recipe_src) == 40U, "status.active_recipe_src offset");
_Static_assert(offsetof(Z8FilmSimStatus, active_recipe_bound) == 41U, "status.active_recipe_bound offset");
_Static_assert(offsetof(Z8FilmSimStatus, xm6_hw_state) == 42U, "status.xm6_hw_state offset");
_Static_assert(offsetof(Z8FilmSimStatus, xm6_runtime_flags) == 43U, "status.xm6_runtime_flags offset");
_Static_assert(offsetof(Z8FilmSimStatus, xm6_last_err) == 44U, "status.xm6_last_err offset");
_Static_assert(offsetof(Z8FilmSimStatus, xm6_last_still_ms) == 46U, "status.xm6_last_still_ms offset");
_Static_assert(offsetof(Z8FilmSimStatus, xm6_still_ok) == 48U, "status.xm6_still_ok offset");
_Static_assert(offsetof(Z8FilmSimStatus, xm6_live_ok) == 52U, "status.xm6_live_ok offset");
_Static_assert(offsetof(Z8FilmSimStatus, xm6_fallback_cnt) == 56U, "status.xm6_fallback_cnt offset");
_Static_assert(offsetof(Z8FilmSimStatus, xm6_last_live_us) == 60U, "status.xm6_last_live_us offset");
_Static_assert(offsetof(Z8FilmSimStatus, active_drive) == 62U, "status.active_drive offset");
_Static_assert(offsetof(Z8FilmSimStatus, xm6_reserved) == 63U, "status.xm6_reserved offset");

/* 64-byte wire payload returned by Z8_FS_OP_XM6_DIAG (op 10) */
typedef struct {
    uint32_t magic;            /* 0:  Z8_XM6_DIAG_MAGIC ('Z8XD', 0x4458385Au) */
    uint16_t version;          /* 4:  Z8_XM6_DIAG_VERSION (1) */
    uint16_t bytes;            /* 6:  Z8_XM6_DIAG_BYTES (64) */
    uint8_t  hw_state;         /* 8:  Raw Core 0 state (*(u32*)0x0a02e7e734 & 0xFF) */
    uint8_t  loader_fault;     /* 9:  *(u8*)0x0a02e803ee */
    uint8_t  fw_loaded;        /* 10: *(u8*)0x0a02e803e8 */
    uint8_t  breaker_tripped;  /* 11: 1 if circuit breaker is tripped */
    uint16_t passed_mask;      /* 12: enum Z8Xm6DiagMask (bit0..3) */
    uint16_t ran_mask;         /* 14: enum Z8Xm6DiagMask (bit0..3) */
    uint32_t diff_grain_exact; /* 16: Byte mismatch count vs ARM NEON (0 = bit-exact) */
    uint32_t diff_grain_sharp; /* 20: Byte mismatch count vs ARM NEON (0 = bit-exact) */
    uint32_t diff_hdf_still;   /* 24: Byte mismatch count vs ARM NEON (0 = bit-exact) */
    uint32_t diff_hdf_live;    /* 28: Byte mismatch count vs ARM NEON (0 = bit-exact) */
    uint32_t us_grain_exact;   /* 32: XM6 elapsed microseconds (CNTVCT_EL0) */
    uint32_t us_grain_sharp;   /* 36: XM6 elapsed microseconds (CNTVCT_EL0) */
    uint32_t us_hdf_still;     /* 40: XM6 elapsed microseconds (CNTVCT_EL0) */
    uint32_t us_hdf_live;      /* 44: XM6 elapsed microseconds (CNTVCT_EL0) */
    uint32_t dsp_status_last;  /* 48: Last Z8Xm6DspReq.dsp_status observed */
    uint32_t reserved[3];      /* 52..63: Must be 0 */
} Z8Xm6DiagResult;

_Static_assert(sizeof(Z8Xm6DiagResult) == Z8_XM6_DIAG_BYTES,
               "Z8Xm6DiagResult must be 64 bytes");
_Static_assert(offsetof(Z8Xm6DiagResult, magic) == 0U, "diag.magic offset");
_Static_assert(offsetof(Z8Xm6DiagResult, version) == 4U, "diag.version offset");
_Static_assert(offsetof(Z8Xm6DiagResult, bytes) == 6U, "diag.bytes offset");
_Static_assert(offsetof(Z8Xm6DiagResult, hw_state) == 8U, "diag.hw_state offset");
_Static_assert(offsetof(Z8Xm6DiagResult, loader_fault) == 9U, "diag.loader_fault offset");
_Static_assert(offsetof(Z8Xm6DiagResult, fw_loaded) == 10U, "diag.fw_loaded offset");
_Static_assert(offsetof(Z8Xm6DiagResult, breaker_tripped) == 11U, "diag.breaker_tripped offset");
_Static_assert(offsetof(Z8Xm6DiagResult, passed_mask) == 12U, "diag.passed_mask offset");
_Static_assert(offsetof(Z8Xm6DiagResult, ran_mask) == 14U, "diag.ran_mask offset");
_Static_assert(offsetof(Z8Xm6DiagResult, diff_grain_exact) == 16U, "diag.diff_grain_exact offset");
_Static_assert(offsetof(Z8Xm6DiagResult, diff_grain_sharp) == 20U, "diag.diff_grain_sharp offset");
_Static_assert(offsetof(Z8Xm6DiagResult, diff_hdf_still) == 24U, "diag.diff_hdf_still offset");
_Static_assert(offsetof(Z8Xm6DiagResult, diff_hdf_live) == 28U, "diag.diff_hdf_live offset");
_Static_assert(offsetof(Z8Xm6DiagResult, us_grain_exact) == 32U, "diag.us_grain_exact offset");
_Static_assert(offsetof(Z8Xm6DiagResult, us_grain_sharp) == 36U, "diag.us_grain_sharp offset");
_Static_assert(offsetof(Z8Xm6DiagResult, us_hdf_still) == 40U, "diag.us_hdf_still offset");
_Static_assert(offsetof(Z8Xm6DiagResult, us_hdf_live) == 44U, "diag.us_hdf_live offset");
_Static_assert(offsetof(Z8Xm6DiagResult, dsp_status_last) == 48U, "diag.dsp_status_last offset");
_Static_assert(offsetof(Z8Xm6DiagResult, reserved) == 52U, "diag.reserved offset");

/* 16-byte NP3 embedded recipe block (placed at SC_RECORD +0x39C..0x3AB, covered by SC CRC32) */
typedef struct {
    uint32_t magic;        /* 0:  Z8_NP3_RECIPE_MAGIC ('Z8FX', 0x5846385Au) */
    uint8_t  version;      /* 4:  Z8_NP3_RECIPE_VERSION (1u) */
    uint8_t  flags;        /* 5:  bit0 = Z8_NP3_RECIPE_FLAG_ENABLED, other bits 0 */
    uint8_t  film_style;   /* 6:  0=OFF, 1=NIKON_ZF */
    uint8_t  zf_intensity; /* 7:  0..6 (0 when OFF, 1..6 when NIKON_ZF) */
    uint8_t  zf_size;      /* 8:  1..3 (1=Small, 2=Medium, 3=Large) */
    uint8_t  zf_blur_mode; /* 9:  0=EXACT, 1=SHARP */
    uint8_t  hdf_level;    /* 10: 0..3 (0=Off, 1=Weak, 2=Medium, 3=Strong) */
    uint8_t  live_link;    /* 11: 0..1 (0=Off, 1=On) */
    uint32_t crc32;        /* 12: IEEE 802.3 CRC32 over bytes [0..11] */
} Z8Np3Recipe;

_Static_assert(sizeof(Z8Np3Recipe) == Z8_NP3_RECIPE_BYTES,
               "Z8Np3Recipe must be 16 bytes");
_Static_assert(offsetof(Z8Np3Recipe, magic) == 0U, "np3.magic offset");
_Static_assert(offsetof(Z8Np3Recipe, version) == 4U, "np3.version offset");
_Static_assert(offsetof(Z8Np3Recipe, flags) == 5U, "np3.flags offset");
_Static_assert(offsetof(Z8Np3Recipe, film_style) == 6U, "np3.film_style offset");
_Static_assert(offsetof(Z8Np3Recipe, zf_intensity) == 7U, "np3.zf_intensity offset");
_Static_assert(offsetof(Z8Np3Recipe, zf_size) == 8U, "np3.zf_size offset");
_Static_assert(offsetof(Z8Np3Recipe, zf_blur_mode) == 9U, "np3.zf_blur_mode offset");
_Static_assert(offsetof(Z8Np3Recipe, hdf_level) == 10U, "np3.hdf_level offset");
_Static_assert(offsetof(Z8Np3Recipe, live_link) == 11U, "np3.live_link offset");
_Static_assert(offsetof(Z8Np3Recipe, crc32) == 12U, "np3.crc32 offset");

/* 8-byte compact slot entry inside FLMREC0/1.CFG (all-zero when unbound) */
typedef struct {
    uint8_t flags;        /* 0: bit0 = Z8_NP3_RECIPE_FLAG_ENABLED; 0 = unbound (all fields 0) */
    uint8_t film_style;   /* 1: 0=OFF, 1=NIKON_ZF */
    uint8_t zf_intensity; /* 2: 0..6 */
    uint8_t zf_size;      /* 3: 1..3 when enabled, 0 when unbound */
    uint8_t zf_blur_mode; /* 4: 0=EXACT, 1=SHARP */
    uint8_t hdf_level;    /* 5: 0..3 */
    uint8_t live_link;    /* 6: 0..1 */
    uint8_t reserved;     /* 7: Must be 0 */
} Z8Np3SlotEntry;

_Static_assert(sizeof(Z8Np3SlotEntry) == Z8_NP3_ENTRY_BYTES,
               "Z8Np3SlotEntry must be 8 bytes");

/* 776-byte dual-slot persisted recipe table (A:\FENESTRA\FLMREC0.CFG / FLMREC1.CFG) */
typedef struct {
    uint32_t       magic;                        /* 0:  Z8_NP3_TABLE_MAGIC ('ZFR1') */
    uint16_t       version;                      /* 4:  Z8_NP3_TABLE_VERSION (1) */
    uint16_t       bytes;                        /* 6:  Z8_NP3_TABLE_BYTES (776) */
    uint32_t       seq;                          /* 8:  Monotonic save generation */
    uint32_t       crc32;                        /* 12: IEEE 802.3 CRC32 over 776B with crc32 zeroed */
    Z8Np3SlotEntry entries[Z8_NP3_SLOT_COUNT];   /* 16..775: 95 x 8 B = 760 B */
} Z8FilmSimRecipeTable;

_Static_assert(sizeof(Z8FilmSimRecipeTable) == Z8_NP3_TABLE_BYTES,
               "Z8FilmSimRecipeTable must be 776 bytes");
_Static_assert(offsetof(Z8FilmSimRecipeTable, entries) == 16U, "table.entries offset");

/* 8-byte query payload for Z8_FS_OP_GET_RECIPE */
typedef struct {
    uint32_t selector;   /* Raw PC selector, or Z8_NP3_SEL_ACTIVE_QUERY (0xFFFFFFFFu) for active slot */
    uint32_t slot_index; /* Canonical 0..94, or Z8_NP3_SLOT_INVALID (0xFFFFFFFFu) to derive from selector */
} Z8FilmSimRecipeQuery;

_Static_assert(sizeof(Z8FilmSimRecipeQuery) == Z8_NP3_QUERY_BYTES,
               "Z8FilmSimRecipeQuery must be 8 bytes");

/* 24-byte wire payload for Z8_FS_OP_SET_RECIPE (req) and Z8_FS_OP_GET_RECIPE (resp) */
typedef struct {
    uint32_t    selector;   /* Raw Z8 Picture Control selector (e.g. 0x01, 0x1C..0x24, 0x1009..0x103F) */
    uint32_t    slot_index; /* Canonical 0..94, or Z8_NP3_SLOT_INVALID to derive from selector */
    Z8Np3Recipe recipe;     /* 16 B 'Z8FX' block; flags=0 clears/unbinds the slot */
} Z8FilmSimRecipeMsg;

_Static_assert(sizeof(Z8FilmSimRecipeMsg) == Z8_NP3_MSG_BYTES,
               "Z8FilmSimRecipeMsg must be 24 bytes");

/* 528-byte chunked bulk sync payload for Z8_FS_OP_SYNC_RECIPES (fits within FX_SERVICE_BYTES 768) */
typedef struct {
    uint32_t       magic;                            /* 0:  Z8_NP3_TABLE_MAGIC ('ZFR1') */
    uint16_t       start_slot;                       /* 4:  First canonical slot (0..94) */
    uint16_t       count;                            /* 6:  Number of entries (1..64, start+count<=95) */
    uint32_t       seq;                              /* 8:  Table seq from FLMREC */
    uint32_t       flags;                            /* 12: bit0=clear_all_before_apply; else 0 */
    Z8Np3SlotEntry entries[Z8_NP3_CHUNK_MAX_SLOTS];  /* 16..527: 512 B */
} Z8FilmSimRecipeChunk;

_Static_assert(sizeof(Z8FilmSimRecipeChunk) == Z8_NP3_CHUNK_BYTES,
               "Z8FilmSimRecipeChunk must be 528 bytes");

/* ---- Freestanding inline helpers shared identically by EL0 & EL1 ---- */

static inline uint32_t z8_np3_crc32_ieee(const uint8_t *data, uint32_t len) {
    uint32_t crc = UINT32_C(0xffffffff);
    for (uint32_t i = 0U; i < len; ++i) {
        crc ^= (uint32_t)data[i];
        for (uint32_t b = 0U; b < 8U; ++b) {
            uint32_t mask = (uint32_t)(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

/* Map raw Z8 Picture Control selector -> canonical slot 0..94 (or Z8_NP3_SLOT_INVALID).
 *   0x0000..0x0027 (31 Stock PC + C1..C9) -> 0..39
 *   0x1009..0x103F (Photo C10..C64)       -> 40..94
 */
static inline uint32_t z8_np3_selector_to_slot(uint32_t selector) {
    if (selector < Z8_NP3_STOCK_AND_C9_COUNT) {
        return selector;
    }
    if ((selector >> 12U) == 1U) {
        uint32_t idx = selector & 0xfffU;
        if (idx >= Z8_NP3_C64_FIRST_INDEX && idx <= Z8_NP3_C64_LAST_INDEX) {
            return Z8_NP3_C64_FIRST_SLOT + (idx - Z8_NP3_C64_FIRST_INDEX);
        }
    }
    return Z8_NP3_SLOT_INVALID;
}

static inline uint32_t z8_np3_slot_to_selector(uint32_t slot) {
    if (slot < Z8_NP3_STOCK_AND_C9_COUNT) {
        return slot;
    }
    if (slot < Z8_NP3_SLOT_COUNT) {
        return UINT32_C(0x1000) + Z8_NP3_C64_FIRST_INDEX + (slot - Z8_NP3_C64_FIRST_SLOT);
    }
    return Z8_NP3_SLOT_INVALID;
}

/* Returns 1 and writes SC_RECORD index (9..63) if selector is photo C10..C64. */
static inline int z8_np3_selector_is_c64(uint32_t selector, uint32_t *out_sc_index) {
    if ((selector >> 12U) == 1U) {
        uint32_t idx = selector & 0xfffU;
        if (idx >= Z8_NP3_C64_FIRST_INDEX && idx <= Z8_NP3_C64_LAST_INDEX) {
            if (out_sc_index) {
                *out_sc_index = idx;
            }
            return 1;
        }
    }
    return 0;
}

static inline uint32_t z8_np3_recipe_crc32(const Z8Np3Recipe *r) {
    return z8_np3_crc32_ieee((const uint8_t *)r, 12U);
}

static inline void z8_np3_recipe_finalize(Z8Np3Recipe *r) {
    if (!r) {
        return;
    }
    r->magic = Z8_NP3_RECIPE_MAGIC;
    r->version = (uint8_t)Z8_NP3_RECIPE_VERSION;
    r->flags &= (uint8_t)Z8_NP3_RECIPE_FLAG_ENABLED;
    if (r->zf_size < Z8_ZF_SIZE_MIN || r->zf_size > Z8_ZF_SIZE_MAX) {
        r->zf_size = (uint8_t)Z8_ZF_SIZE_DEFAULT;
    }
    r->crc32 = z8_np3_recipe_crc32(r);
}

static inline int z8_np3_recipe_is_valid(const Z8Np3Recipe *r) {
    if (!r) {
        return 0;
    }
    if (r->magic != Z8_NP3_RECIPE_MAGIC ||
        r->version != Z8_NP3_RECIPE_VERSION ||
        (r->flags & (uint8_t)(~Z8_NP3_RECIPE_FLAG_ENABLED)) != 0U) {
        return 0;
    }
    if (r->film_style > Z8_STYLE_NIKON_ZF ||
        r->zf_intensity > Z8_ZF_INTENSITY_MAX ||
        r->zf_size < Z8_ZF_SIZE_MIN ||
        r->zf_size > Z8_ZF_SIZE_MAX ||
        r->zf_blur_mode > Z8_ZF_BLUR_SHARP ||
        r->hdf_level > Z8_HDF_STRONG ||
        r->live_link > Z8_LIVE_LINK_ON) {
        return 0;
    }
    return r->crc32 == z8_np3_recipe_crc32(r);
}

static inline int z8_np3_entry_is_valid(const Z8Np3SlotEntry *e) {
    if (!e) {
        return 0;
    }
    if (e->reserved != 0U) {
        return 0;
    }
    if (e->flags == 0U) {
        return e->film_style == 0U &&
               e->zf_intensity == 0U &&
               e->zf_size == 0U &&
               e->zf_blur_mode == 0U &&
               e->hdf_level == 0U &&
               e->live_link == 0U;
    }
    if (e->flags != Z8_NP3_RECIPE_FLAG_ENABLED) {
        return 0;
    }
    return e->film_style <= Z8_STYLE_NIKON_ZF &&
           e->zf_intensity <= Z8_ZF_INTENSITY_MAX &&
           e->zf_size >= Z8_ZF_SIZE_MIN &&
           e->zf_size <= Z8_ZF_SIZE_MAX &&
           e->zf_blur_mode <= Z8_ZF_BLUR_SHARP &&
           e->hdf_level <= Z8_HDF_STRONG &&
           e->live_link <= Z8_LIVE_LINK_ON;
}

static inline void z8_np3_recipe_to_entry(const Z8Np3Recipe *r, Z8Np3SlotEntry *out) {
    if (!out) {
        return;
    }
    out->flags = 0U;
    out->film_style = 0U;
    out->zf_intensity = 0U;
    out->zf_size = 0U;
    out->zf_blur_mode = 0U;
    out->hdf_level = 0U;
    out->live_link = 0U;
    out->reserved = 0U;
    if (!r || !z8_np3_recipe_is_valid(r) || !(r->flags & Z8_NP3_RECIPE_FLAG_ENABLED)) {
        return;
    }
    out->flags = (uint8_t)Z8_NP3_RECIPE_FLAG_ENABLED;
    out->film_style = r->film_style;
    out->zf_intensity = r->zf_intensity;
    out->zf_size = r->zf_size;
    out->zf_blur_mode = r->zf_blur_mode;
    out->hdf_level = r->hdf_level;
    out->live_link = r->live_link;
    out->reserved = 0U;
}

static inline void z8_np3_entry_to_recipe(const Z8Np3SlotEntry *e, Z8Np3Recipe *out) {
    if (!out) {
        return;
    }
    out->magic = Z8_NP3_RECIPE_MAGIC;
    out->version = (uint8_t)Z8_NP3_RECIPE_VERSION;
    out->flags = 0U;
    out->film_style = (uint8_t)Z8_STYLE_OFF;
    out->zf_intensity = 0U;
    out->zf_size = (uint8_t)Z8_ZF_SIZE_DEFAULT;
    out->zf_blur_mode = (uint8_t)Z8_ZF_BLUR_DEFAULT;
    out->hdf_level = (uint8_t)Z8_HDF_OFF;
    out->live_link = (uint8_t)Z8_LIVE_LINK_OFF;
    if (e && z8_np3_entry_is_valid(e) && (e->flags & Z8_NP3_RECIPE_FLAG_ENABLED)) {
        out->flags = (uint8_t)Z8_NP3_RECIPE_FLAG_ENABLED;
        out->film_style = e->film_style;
        out->zf_intensity = e->zf_intensity;
        out->zf_size = e->zf_size;
        out->zf_blur_mode = e->zf_blur_mode;
        out->hdf_level = e->hdf_level;
        out->live_link = e->live_link;
    }
    out->crc32 = z8_np3_recipe_crc32(out);
}

static inline void z8_np3_recipe_from_cfg(const Z8FilmSimConfig *cfg, Z8Np3Recipe *out) {
    if (!out) {
        return;
    }
    out->magic = Z8_NP3_RECIPE_MAGIC;
    out->version = (uint8_t)Z8_NP3_RECIPE_VERSION;
    out->flags = (uint8_t)Z8_NP3_RECIPE_FLAG_ENABLED;
    if (!cfg) {
        out->film_style = (uint8_t)Z8_STYLE_OFF;
        out->zf_intensity = 0U;
        out->zf_size = (uint8_t)Z8_ZF_SIZE_DEFAULT;
        out->zf_blur_mode = (uint8_t)Z8_ZF_BLUR_DEFAULT;
        out->hdf_level = (uint8_t)Z8_HDF_OFF;
        out->live_link = (uint8_t)Z8_LIVE_LINK_OFF;
    } else {
        uint8_t style = (cfg->film_style == Z8_STYLE_NIKON_ZF ||
                         cfg->film_style == Z8_STYLE_FUJI_ZF)
                            ? (uint8_t)Z8_STYLE_NIKON_ZF
                            : (uint8_t)Z8_STYLE_OFF;
        out->film_style = (style == Z8_STYLE_NIKON_ZF && cfg->zf_intensity > 0U)
                              ? (uint8_t)Z8_STYLE_NIKON_ZF
                              : (uint8_t)Z8_STYLE_OFF;
        out->zf_intensity = (out->film_style == Z8_STYLE_NIKON_ZF &&
                             cfg->zf_intensity <= Z8_ZF_INTENSITY_MAX)
                                ? cfg->zf_intensity
                                : 0U;
        out->zf_size = (cfg->zf_size >= Z8_ZF_SIZE_MIN && cfg->zf_size <= Z8_ZF_SIZE_MAX)
                           ? cfg->zf_size
                           : (uint8_t)Z8_ZF_SIZE_DEFAULT;
        out->zf_blur_mode = (cfg->zf_blur_mode <= Z8_ZF_BLUR_SHARP)
                                ? cfg->zf_blur_mode
                                : (uint8_t)Z8_ZF_BLUR_DEFAULT;
        out->hdf_level = (cfg->hdf_level <= Z8_HDF_STRONG) ? cfg->hdf_level : (uint8_t)Z8_HDF_OFF;
        out->live_link = (cfg->live_link <= Z8_LIVE_LINK_ON) ? cfg->live_link : (uint8_t)Z8_LIVE_LINK_OFF;
    }
    out->crc32 = z8_np3_recipe_crc32(out);
}

static inline uint32_t z8_np3_table_crc32(const Z8FilmSimRecipeTable *t) {
    const uint8_t *src = (const uint8_t *)t;
    uint32_t crc = UINT32_C(0xffffffff);
    for (uint32_t i = 0U; i < Z8_NP3_TABLE_BYTES; ++i) {
        uint8_t byte = (i >= 12U && i < 16U) ? 0U : src[i];
        crc ^= (uint32_t)byte;
        for (uint32_t b = 0U; b < 8U; ++b) {
            uint32_t mask = (uint32_t)(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

static inline void z8_np3_table_init_empty(Z8FilmSimRecipeTable *t) {
    if (!t) {
        return;
    }
    uint8_t *p = (uint8_t *)t;
    for (uint32_t i = 0U; i < Z8_NP3_TABLE_BYTES; ++i) {
        p[i] = 0U;
    }
    t->magic = Z8_NP3_TABLE_MAGIC;
    t->version = (uint16_t)Z8_NP3_TABLE_VERSION;
    t->bytes = (uint16_t)Z8_NP3_TABLE_BYTES;
    t->seq = 0U;
    t->crc32 = z8_np3_table_crc32(t);
}

static inline void z8_np3_table_finalize(Z8FilmSimRecipeTable *t, uint32_t seq) {
    if (!t) {
        return;
    }
    t->magic = Z8_NP3_TABLE_MAGIC;
    t->version = (uint16_t)Z8_NP3_TABLE_VERSION;
    t->bytes = (uint16_t)Z8_NP3_TABLE_BYTES;
    t->seq = seq;
    t->crc32 = 0U;
    t->crc32 = z8_np3_table_crc32(t);
}

static inline int z8_np3_table_is_valid(const Z8FilmSimRecipeTable *t) {
    if (!t) {
        return 0;
    }
    if (t->magic != Z8_NP3_TABLE_MAGIC ||
        t->version != Z8_NP3_TABLE_VERSION ||
        t->bytes != Z8_NP3_TABLE_BYTES) {
        return 0;
    }
    if (t->crc32 != z8_np3_table_crc32(t)) {
        return 0;
    }
    for (uint32_t i = 0U; i < Z8_NP3_SLOT_COUNT; ++i) {
        if (!z8_np3_entry_is_valid(&t->entries[i])) {
            return 0;
        }
    }
    return 1;
}

#endif /* Z8_FILMSIM_ABI_H */
