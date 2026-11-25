/* SPDX-License-Identifier: MIT
 * Shared EL0 <-> EL1 IPC and dual-slot card config ABI for livephoto.wlm (V71.7-r4).
 * Matches work/src-fix37/ui/live_ipc.h & work/src-fix37/ui/live_config.h byte-for-byte.
 */
#ifndef LIVEPHOTO_ABI_H
#define LIVEPHOTO_ABI_H

#include <stddef.h>
#include <stdint.h>

#define LP_SLOTS_BINDING_NAME   "slots.state"
#define LP_SLOTS_BINDING_VER    1u
#define LP_SLOTS_LANG_GET       15u
#define LP_LANG_MAGIC           UINT32_C(0x4c414e47) /* 'LANG' */
#define LP_LANG_EN              1u
#define LP_LANG_ZH_CN           8u
#define LP_LANG_ZH_TW           9u

#define LP_SLOTS_TUNNEL_MAGIC   UINT32_C(0x3149504c) /* 'LPI1' */
#define LP_SLOTS_HDR_BYTES      16u
#define LP_OP_GET_STATE         49u
#define LP_OP_SET_CFG           50u
#define LP_OP_FLUSH_LOG         51u

#define LP_APPLY_REQUESTED      (1u << 0)
#define LP_APPLY_MUTE           (1u << 1)
#define LP_APPLY_PB_KEY         (1u << 2)
#define LP_APPLY_ALL            0x07u

#define LP_PB_KEY_OFF           0u
#define LP_PB_KEY_REC           9u

#define LP_W5_FLAG_HAS_POOL     (1u << 0)
#define LP_W5_FLAG_CLOSE_FAILED (1u << 1)
#define LP_W5_FLAG_LP_LOADED    (1u << 2)
#define LP_W5_FLAG_PB_READY     (1u << 3)
#define LP_W5_FLAG_RELOAD       (1u << 4)

/* 64-byte backward-compatible FLP1 config file (<active_drive>:\FENESTRA\live-z8.0 / .1).
 * Reuses reserved u32 at +32 for pb_key_cfg: 0=default REC(9), 1=Off(0), 2=REC(9). */
#define LP_CFG_MAGIC            UINT32_C(0x31504c46) /* 'FLP1' */
#define LP_CFG_VERSION          1u
#define LP_CFG_MODEL            UINT32_C(0x00080311)
#define LP_CFG_BYTES            64u
#define LP_CFG_READ_MAX         65u

#define LP_CFG_PB_DEFAULT_REC   0u
#define LP_CFG_PB_OFF           1u
#define LP_CFG_PB_REC           2u

enum LpUiPhase {
    LP_PHASE_OFF         = 0u,
    LP_PHASE_PREPARING   = 1u,
    LP_PHASE_SHORT_CLIP  = 2u,
    LP_PHASE_READY       = 3u,
    LP_PHASE_BUSY        = 4u,
    LP_PHASE_UNAVAILABLE = 5u,
    LP_PHASE_CARD_ERR    = 6u
};

/* 64 B exact, naturally aligned (matches #340 LivePhotoIpcState) */
typedef struct {
    uint32_t magic;            /* +0:  0x3149504C ('LPI1') */
    uint8_t  requested;        /* +4:  0/1 */
    uint8_t  mute;             /* +5:  0/1 */
    uint8_t  pb_key;           /* +6:  0/9 */
    uint8_t  ui_phase;         /* +7:  enum LpUiPhase (0..6) */
    uint8_t  active_drive;     /* +8:  'A'/'B' from CSL1 */
    uint8_t  audio_phase;      /* +9:  0..3 */
    uint8_t  w5_parked;        /* +10: 0/1 */
    uint8_t  w5_flags;         /* +11: bit0=has_pool, bit1=close_failed, bit2=lp_loaded, bit3=pb_ready */
    uint32_t tx_count;         /* +12 */
    uint32_t tx_err;           /* +16 */
    uint32_t encoded;          /* +20 */
    uint32_t dropped;          /* +24 */
    uint32_t audio_tracks;     /* +28 */
    uint32_t pb_consumed_rec;  /* +32 */
    uint32_t pb_frames;        /* +36 */
    uint32_t pb_err;           /* +40 */
    uint16_t twin_copies;      /* +44 */
    uint16_t twin_err;         /* +46 */
    uint16_t flag_parks;       /* +48 */
    uint16_t flag_restores;    /* +50 */
    uint32_t log_saves;        /* +52 */
    uint32_t log_err;          /* +56 */
    uint32_t reserved;         /* +60 */
} LivePhotoIpcState;

_Static_assert(sizeof(LivePhotoIpcState) == 64u, "LivePhotoIpcState must be 64 bytes");
_Static_assert(offsetof(LivePhotoIpcState, magic) == 0u, "lp.magic");
_Static_assert(offsetof(LivePhotoIpcState, requested) == 4u, "lp.requested");
_Static_assert(offsetof(LivePhotoIpcState, mute) == 5u, "lp.mute");
_Static_assert(offsetof(LivePhotoIpcState, pb_key) == 6u, "lp.pb_key");
_Static_assert(offsetof(LivePhotoIpcState, ui_phase) == 7u, "lp.ui_phase");
_Static_assert(offsetof(LivePhotoIpcState, active_drive) == 8u, "lp.active_drive");
_Static_assert(offsetof(LivePhotoIpcState, audio_phase) == 9u, "lp.audio_phase");
_Static_assert(offsetof(LivePhotoIpcState, w5_parked) == 10u, "lp.w5_parked");
_Static_assert(offsetof(LivePhotoIpcState, w5_flags) == 11u, "lp.w5_flags");
_Static_assert(offsetof(LivePhotoIpcState, tx_count) == 12u, "lp.tx_count");
_Static_assert(offsetof(LivePhotoIpcState, tx_err) == 16u, "lp.tx_err");
_Static_assert(offsetof(LivePhotoIpcState, encoded) == 20u, "lp.encoded");
_Static_assert(offsetof(LivePhotoIpcState, dropped) == 24u, "lp.dropped");
_Static_assert(offsetof(LivePhotoIpcState, audio_tracks) == 28u, "lp.audio_tracks");
_Static_assert(offsetof(LivePhotoIpcState, pb_consumed_rec) == 32u, "lp.pb_consumed_rec");
_Static_assert(offsetof(LivePhotoIpcState, pb_frames) == 36u, "lp.pb_frames");
_Static_assert(offsetof(LivePhotoIpcState, pb_err) == 40u, "lp.pb_err");
_Static_assert(offsetof(LivePhotoIpcState, twin_copies) == 44u, "lp.twin_copies");
_Static_assert(offsetof(LivePhotoIpcState, twin_err) == 46u, "lp.twin_err");
_Static_assert(offsetof(LivePhotoIpcState, flag_parks) == 48u, "lp.flag_parks");
_Static_assert(offsetof(LivePhotoIpcState, flag_restores) == 50u, "lp.flag_restores");
_Static_assert(offsetof(LivePhotoIpcState, log_saves) == 52u, "lp.log_saves");
_Static_assert(offsetof(LivePhotoIpcState, log_err) == 56u, "lp.log_err");
_Static_assert(offsetof(LivePhotoIpcState, reserved) == 60u, "lp.reserved");

/* 16 B exact, naturally aligned (matches #340 LivePhotoIpcSetCfg) */
typedef struct {
    uint32_t magic;            /* +0: 0x3149504C ('LPI1') */
    uint8_t  requested;        /* +4: 0/1 */
    uint8_t  mute;             /* +5: 0/1 */
    uint8_t  pb_key;           /* +6: 0/9 */
    uint8_t  apply_mask;       /* +7: bit0=requested, bit1=mute, bit2=pb_key */
    uint32_t reserved[2];      /* +8: 0, 0 */
} LivePhotoIpcSetCfg;

_Static_assert(sizeof(LivePhotoIpcSetCfg) == 16u, "LivePhotoIpcSetCfg must be 16 bytes");
_Static_assert(offsetof(LivePhotoIpcSetCfg, magic) == 0u, "lpcfg.magic");
_Static_assert(offsetof(LivePhotoIpcSetCfg, requested) == 4u, "lpcfg.requested");
_Static_assert(offsetof(LivePhotoIpcSetCfg, mute) == 5u, "lpcfg.mute");
_Static_assert(offsetof(LivePhotoIpcSetCfg, pb_key) == 6u, "lpcfg.pb_key");
_Static_assert(offsetof(LivePhotoIpcSetCfg, apply_mask) == 7u, "lpcfg.apply_mask");

static inline uint32_t lp_crc32_ieee(const uint8_t *p, uint32_t n) {
    uint32_t c = UINT32_C(0xffffffff);
    for (uint32_t i = 0u; i < n; ++i) {
        c ^= (uint32_t)p[i];
        for (uint32_t b = 0u; b < 8u; ++b) {
            c = (c >> 1u) ^ ((0u - (c & 1u)) & UINT32_C(0xedb88320));
        }
    }
    return ~c;
}

static inline uint32_t lp_rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static inline void lp_wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8u);
    p[2] = (uint8_t)(v >> 16u);
    p[3] = (uint8_t)(v >> 24u);
}

static inline uint8_t lp_pb_key_from_cfg(uint32_t pb_key_cfg) {
    return (pb_key_cfg == LP_CFG_PB_OFF) ? (uint8_t)LP_PB_KEY_OFF : (uint8_t)LP_PB_KEY_REC;
}

static inline uint32_t lp_cfg_from_pb_key(uint8_t pb_key) {
    return (pb_key == LP_PB_KEY_OFF) ? LP_CFG_PB_OFF : LP_CFG_PB_REC;
}

static inline void lp_cfg_encode(uint8_t out[LP_CFG_BYTES],
                                 uint64_t generation,
                                 uint32_t requested,
                                 uint32_t mute,
                                 uint32_t pb_key_cfg) {
    for (uint32_t i = 0u; i < LP_CFG_BYTES; ++i) {
        out[i] = 0u;
    }
    lp_wr_u32(out + 0u,  LP_CFG_MAGIC);
    lp_wr_u32(out + 4u,  LP_CFG_VERSION);
    lp_wr_u32(out + 8u,  LP_CFG_MODEL);
    lp_wr_u32(out + 12u, LP_CFG_BYTES);
    lp_wr_u32(out + 16u, (uint32_t)generation);
    lp_wr_u32(out + 20u, (uint32_t)(generation >> 32u));
    lp_wr_u32(out + 24u, requested ? 1u : 0u);
    lp_wr_u32(out + 28u, mute ? 1u : 0u);
    lp_wr_u32(out + 32u, pb_key_cfg <= LP_CFG_PB_REC ? pb_key_cfg : LP_CFG_PB_REC);
    lp_wr_u32(out + 60u, lp_crc32_ieee(out, 60u));
}

static inline int lp_cfg_decode(const uint8_t *p, uint32_t bytes,
                                uint64_t *generation,
                                uint32_t *requested,
                                uint32_t *mute,
                                uint32_t *pb_key_cfg) {
    if (!p || bytes != LP_CFG_BYTES) {
        return 0;
    }
    if (lp_rd_u32(p + 0u) != LP_CFG_MAGIC ||
        lp_rd_u32(p + 4u) != LP_CFG_VERSION ||
        lp_rd_u32(p + 8u) != LP_CFG_MODEL ||
        lp_rd_u32(p + 12u) != LP_CFG_BYTES) {
        return 0;
    }
    uint32_t req = lp_rd_u32(p + 24u);
    uint32_t mut = lp_rd_u32(p + 28u);
    uint32_t pbk = lp_rd_u32(p + 32u);
    if (req > 1u || mut > 1u || pbk > LP_CFG_PB_REC) {
        return 0;
    }
    if (lp_rd_u32(p + 60u) != lp_crc32_ieee(p, 60u)) {
        return 0;
    }
    if (generation) {
        *generation = (uint64_t)lp_rd_u32(p + 16u) | ((uint64_t)lp_rd_u32(p + 20u) << 32u);
    }
    if (requested) *requested = req;
    if (mute) *mute = mut;
    if (pb_key_cfg) *pb_key_cfg = pbk;
    return 1;
}

#endif /* LIVEPHOTO_ABI_H */
