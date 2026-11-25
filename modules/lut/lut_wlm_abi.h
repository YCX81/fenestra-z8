/* SPDX-License-Identifier: MIT
 * Shared EL0 <-> EL1 IPC and dual-slot card config ABI for lut.wlm (V71.7-r4).
 * Matches work/lut_core/v71r7_lut_abi.h (#339) byte-for-byte.
 */
#ifndef LUT_WLM_ABI_H
#define LUT_WLM_ABI_H

#include <stddef.h>
#include <stdint.h>

#define LUT_SLOTS_BINDING_NAME   "slots.state"
#define LUT_SLOTS_BINDING_VER    1u
#define LUT_SLOTS_LANG_GET       15u
#define LUT_LANG_MAGIC           UINT32_C(0x4c414e47) /* 'LANG' */
#define LUT_LANG_EN              1u
#define LUT_LANG_ZH_CN           8u
#define LUT_LANG_ZH_TW           9u

#define LUT_SLOTS_TUNNEL_MAGIC   UINT32_C(0x3149544c) /* 'LTI1' */
#define LUT_SLOTS_HDR_BYTES      16u
#define LUT_IPC_VERSION          1u

#define LUT_OP_GET_STATE         65u
#define LUT_OP_SET_CFG           66u
#define LUT_OP_CLEAR_LUT         67u
#define LUT_OP_RESET_PREVIEW     68u

#define LUT_STAGE_PREVIEW        (1u << 0)
#define LUT_STAGE_STILL          (1u << 1)
#define LUT_STAGE_LIVE           (1u << 2)
#define LUT_STAGE_ALL            0x07u

/* 32-byte dual-slot persisted config (<active_drive>:\FENESTRA\FXLUT0.CFG / FXLUT1.CFG) */
#define LUT_CFG_MAGIC            UINT32_C(0x31544c5a) /* 'ZLT1' */
#define LUT_CFG_VERSION          1u
#define LUT_CFG_BYTES            32u
#define LUT_CFG_READ_MAX         33u

/* 16 B exact (matches #339 V71r7LutIpcSetCfg) */
typedef struct {
    uint32_t magic;        /* +0:  0x3149544C ('LTI1') */
    uint16_t version;      /* +4:  1 */
    uint16_t bytes;        /* +6:  16 */
    uint8_t  master_on;    /* +8:  0/1 */
    uint8_t  stage_mask;   /* +9:  bit0=PREVIEW, bit1=STILL, bit2=LIVE */
    uint8_t  reserved[6];  /* +10..15: 0 */
} V71r7LutIpcSetCfg;

_Static_assert(sizeof(V71r7LutIpcSetCfg) == 16u, "V71r7LutIpcSetCfg must be 16 bytes");
_Static_assert(offsetof(V71r7LutIpcSetCfg, magic) == 0u, "lutcfg.magic");
_Static_assert(offsetof(V71r7LutIpcSetCfg, version) == 4u, "lutcfg.version");
_Static_assert(offsetof(V71r7LutIpcSetCfg, bytes) == 6u, "lutcfg.bytes");
_Static_assert(offsetof(V71r7LutIpcSetCfg, master_on) == 8u, "lutcfg.master_on");
_Static_assert(offsetof(V71r7LutIpcSetCfg, stage_mask) == 9u, "lutcfg.stage_mask");

/* 168 B exact (matches #339 V71r7LutIpcState) */
typedef struct {
    uint32_t magic;               /* +0:  0x3149544C ('LTI1') */
    uint16_t version;             /* +4:  1 */
    uint16_t bytes;               /* +6:  168 */
    uint8_t  master_on;           /* +8 */
    uint8_t  preview_on;          /* +9 */
    uint8_t  still_on;            /* +10 */
    uint8_t  live_on;             /* +11 */
    uint8_t  active_drive;        /* +12: 'A'/'B' */
    uint8_t  is_1d_separable;     /* +13 */
    uint8_t  neon_allowed;        /* +14 */
    uint8_t  quarantined;         /* +15 */
    uint8_t  lut_active;          /* +16 */
    uint8_t  smp_helpers_online;  /* +17 */
    uint8_t  reserved0[2];        /* +18..19 */
    uint32_t worker_phase;        /* +20: 0..12 (12=PHOTO_MODULE_DISABLED) */
    int32_t  last_result;         /* +24 */
    int32_t  pool_result;         /* +28 */
    uint32_t generation;          /* +32 */
    uint32_t ws_bytes;            /* +36 */
    uint32_t fps_milli;           /* +40 */
    uint32_t grades;              /* +44 */
    uint32_t disp_sub;            /* +48 */
    uint32_t disp_rejects;        /* +52 */
    uint32_t xm6_calls;           /* +56 */
    uint32_t nv16_neon;           /* +60 */
    uint32_t nv16_scalar;         /* +64 */
    uint32_t nv12_neon;           /* +68 */
    uint32_t wd_polls;            /* +72 */
    uint32_t live_reuse_hits;     /* +76 */
    uint32_t last_lut_us;         /* +80 */
    uint32_t last_hdf_us;         /* +84 */
    uint32_t last_grain_us;       /* +88 */
    uint32_t reserved1[3];        /* +92..103 */
    char     active_path[64];     /* +104..167 */
} V71r7LutIpcState;

_Static_assert(sizeof(V71r7LutIpcState) == 168u, "V71r7LutIpcState must be 168 bytes");
_Static_assert(offsetof(V71r7LutIpcState, magic) == 0u, "lutst.magic");
_Static_assert(offsetof(V71r7LutIpcState, master_on) == 8u, "lutst.master_on");
_Static_assert(offsetof(V71r7LutIpcState, active_drive) == 12u, "lutst.active_drive");
_Static_assert(offsetof(V71r7LutIpcState, lut_active) == 16u, "lutst.lut_active");
_Static_assert(offsetof(V71r7LutIpcState, worker_phase) == 20u, "lutst.worker_phase");
_Static_assert(offsetof(V71r7LutIpcState, last_result) == 24u, "lutst.last_result");
_Static_assert(offsetof(V71r7LutIpcState, pool_result) == 28u, "lutst.pool_result");
_Static_assert(offsetof(V71r7LutIpcState, generation) == 32u, "lutst.generation");
_Static_assert(offsetof(V71r7LutIpcState, ws_bytes) == 36u, "lutst.ws_bytes");
_Static_assert(offsetof(V71r7LutIpcState, fps_milli) == 40u, "lutst.fps_milli");
_Static_assert(offsetof(V71r7LutIpcState, disp_sub) == 48u, "lutst.disp_sub");
_Static_assert(offsetof(V71r7LutIpcState, disp_rejects) == 52u, "lutst.disp_rejects");
_Static_assert(offsetof(V71r7LutIpcState, wd_polls) == 72u, "lutst.wd_polls");
_Static_assert(offsetof(V71r7LutIpcState, active_path) == 104u, "lutst.active_path");

/* 32 B persisted config (FXLUT0.CFG / FXLUT1.CFG) */
typedef struct {
    uint32_t magic;        /* +0:  0x31544C5A ('ZLT1') */
    uint16_t version;      /* +4:  1 */
    uint16_t bytes;        /* +6:  32 */
    uint32_t seq;          /* +8:  monotonic generation */
    uint32_t crc32;        /* +12: IEEE 802.3 CRC32 over 32B with crc32 zeroed */
    uint8_t  master_on;    /* +16: 0/1 (default 1) */
    uint8_t  preview_on;   /* +17: 0/1 (default 1) */
    uint8_t  still_on;     /* +18: 0/1 (default 1) */
    uint8_t  live_on;      /* +19: 0/1 (default 1) */
    uint8_t  reserved[12]; /* +20..31: must be 0 */
} V71r7LutWlmCfg;

_Static_assert(sizeof(V71r7LutWlmCfg) == LUT_CFG_BYTES, "V71r7LutWlmCfg must be 32 bytes");
_Static_assert(offsetof(V71r7LutWlmCfg, magic) == 0u, "wlmcfg.magic");
_Static_assert(offsetof(V71r7LutWlmCfg, seq) == 8u, "wlmcfg.seq");
_Static_assert(offsetof(V71r7LutWlmCfg, crc32) == 12u, "wlmcfg.crc32");
_Static_assert(offsetof(V71r7LutWlmCfg, master_on) == 16u, "wlmcfg.master_on");
_Static_assert(offsetof(V71r7LutWlmCfg, live_on) == 19u, "wlmcfg.live_on");

static inline uint32_t lut_crc32_ieee(const uint8_t *p, uint32_t n) {
    uint32_t c = UINT32_C(0xffffffff);
    for (uint32_t i = 0u; i < n; ++i) {
        c ^= (uint32_t)p[i];
        for (uint32_t b = 0u; b < 8u; ++b) {
            c = (c >> 1u) ^ ((0u - (c & 1u)) & UINT32_C(0xedb88320));
        }
    }
    return ~c;
}

static inline uint32_t lut_cfg_crc32(const V71r7LutWlmCfg *c) {
    uint8_t raw[LUT_CFG_BYTES];
    const uint8_t *src = (const uint8_t *)c;
    for (uint32_t i = 0u; i < LUT_CFG_BYTES; ++i) raw[i] = src[i];
    raw[12] = raw[13] = raw[14] = raw[15] = 0u;
    return lut_crc32_ieee(raw, LUT_CFG_BYTES);
}

static inline void lut_cfg_finalize(V71r7LutWlmCfg *c, uint32_t seq) {
    c->magic = LUT_CFG_MAGIC;
    c->version = LUT_CFG_VERSION;
    c->bytes = LUT_CFG_BYTES;
    c->seq = seq;
    c->master_on = c->master_on ? 1u : 0u;
    c->preview_on = c->preview_on ? 1u : 0u;
    c->still_on = c->still_on ? 1u : 0u;
    c->live_on = c->live_on ? 1u : 0u;
    for (uint32_t i = 0u; i < 12u; ++i) c->reserved[i] = 0u;
    c->crc32 = 0u;
    c->crc32 = lut_cfg_crc32(c);
}

static inline void lut_cfg_default(V71r7LutWlmCfg *c) {
    uint8_t *p = (uint8_t *)c;
    for (uint32_t i = 0u; i < LUT_CFG_BYTES; ++i) p[i] = 0u;
    c->master_on = 1u;
    c->preview_on = 1u;
    c->still_on = 1u;
    c->live_on = 1u;
    lut_cfg_finalize(c, 0u);
}

static inline int lut_cfg_is_valid(const V71r7LutWlmCfg *c) {
    if (!c) return 0;
    if (c->magic != LUT_CFG_MAGIC || c->version != LUT_CFG_VERSION || c->bytes != LUT_CFG_BYTES) {
        return 0;
    }
    if (c->master_on > 1u || c->preview_on > 1u || c->still_on > 1u || c->live_on > 1u) {
        return 0;
    }
    for (uint32_t i = 0u; i < 12u; ++i) {
        if (c->reserved[i] != 0u) return 0;
    }
    return c->crc32 == lut_cfg_crc32(c);
}

static inline uint8_t lut_cfg_stage_mask(const V71r7LutWlmCfg *c) {
    uint8_t m = 0u;
    if (c->preview_on) m |= (uint8_t)LUT_STAGE_PREVIEW;
    if (c->still_on)   m |= (uint8_t)LUT_STAGE_STILL;
    if (c->live_on)    m |= (uint8_t)LUT_STAGE_LIVE;
    return m;
}

#endif /* LUT_WLM_ABI_H */
