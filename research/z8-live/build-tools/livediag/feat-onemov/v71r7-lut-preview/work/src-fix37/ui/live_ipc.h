/* SPDX-License-Identifier: MIT
 * Frozen Shared IPC Contract between EL0 livephoto.wlm and EL1 live_ui/runtime/pb-fix37.
 * Routed via slots.state ops 48..63 (magic 'LPI1') through filmsim_slots_svc_dispatch.
 */
#ifndef LIVE_IPC_H
#define LIVE_IPC_H

#include <stddef.h>
#include <stdint.h>

#define LP_IPC_MAGIC            UINT32_C(0x3149504C) /* 'LPI1' */
#define LP_IPC_VERSION          1U
#define LP_SLOTS_OP_BASE        48U
#define LP_OP_GET_STATE         49U /* req 0 B  -> resp 64 B LivePhotoIpcState */
#define LP_OP_SET_CFG           50U /* req 16 B LivePhotoIpcSetCfg -> resp 64 B LivePhotoIpcState */
#define LP_OP_FLUSH_LOG         51U /* req 0 B  -> resp 0 B */

/* LivePhotoIpcState.ui_phase (0..6) */
#define LP_UI_OFF               0U
#define LP_UI_PREPARING         1U
#define LP_UI_SHORT_CLIP        2U
#define LP_UI_READY             3U
#define LP_UI_BUSY              4U
#define LP_UI_UNAVAILABLE       5U
#define LP_UI_CARD_ERR          6U

/* LivePhotoIpcState.w5_flags */
#define LP_W5_FLAG_HAS_POOL     (1U << 0)
#define LP_W5_FLAG_CLOSE_FAILED (1U << 1)
#define LP_W5_FLAG_LP_LOADED    (1U << 2)
#define LP_W5_FLAG_PB_READY     (1U << 3)
#define LP_W5_FLAG_RELOAD       (1U << 4) /* V71.7-r6: card/USB ev=24 revoked EL1 requested; EL0 should re-sync */

/* LivePhotoIpcSetCfg.apply_mask (0 treated as LP_SET_MASK_ALL) */
#define LP_SET_MASK_REQUESTED   (1U << 0)
#define LP_SET_MASK_MUTE        (1U << 1)
#define LP_SET_MASK_PB_KEY      (1U << 2)
#define LP_SET_MASK_ALL         0x07U

typedef struct {
    uint32_t magic;            /* +0   0x3149504C ('LPI1') */
    uint8_t  requested;        /* +4   0=Off, 1=On */
    uint8_t  mute;             /* +5   0=Shutter sound On, 1=Muted/Off */
    uint8_t  pb_key;           /* +6   0=Off, 9=REC */
    uint8_t  ui_phase;         /* +7   0..6 (LP_UI_*) */
    uint8_t  active_drive;     /* +8   'A' or 'B' (from CSL1 0xA1171B000) */
    uint8_t  audio_phase;      /* +9   0..3 (r->audio_phase) */
    uint8_t  w5_parked;        /* +10  0=normal/idle, 1=parked */
    uint8_t  w5_flags;         /* +11  LP_W5_FLAG_* bitmask */
    uint32_t tx_count;         /* +12  r->transaction_count */
    uint32_t tx_err;           /* +16  r->transaction_error */
    uint32_t encoded;          /* +20  (uint32_t)r->stream.encoded */
    uint32_t dropped;          /* +24  (uint32_t)(copy_dropped + backlog_dropped) */
    uint32_t audio_tracks;     /* +28  r->audio_tracks */
    uint32_t pb_consumed_rec;  /* +32  pb->consumed_rec */
    uint32_t pb_frames;        /* +36  pb->nb_starts + pb->frames */
    uint32_t pb_err;           /* +40  pb->error ? pb->error : pb->nb_fail */
    uint16_t twin_copies;      /* +44  sat16(LIVE_TWIN->copies) */
    uint16_t twin_err;         /* +46  sat16(LIVE_TWIN->errors) */
    uint16_t flag_parks;       /* +48  sat16(r->flag_parks) */
    uint16_t flag_restores;    /* +50  sat16(r->flag_restores) */
    uint32_t log_saves;        /* +52  r->log_saves */
    uint32_t log_err;          /* +56  r->log_err */
    uint32_t reserved;         /* +60  0 */
} LivePhotoIpcState;

typedef struct {
    uint32_t magic;            /* +0   0x3149504C ('LPI1') */
    uint8_t  requested;        /* +4   0 or 1 */
    uint8_t  mute;             /* +5   0 or 1 */
    uint8_t  pb_key;           /* +6   0 or 9 */
    uint8_t  apply_mask;       /* +7   LP_SET_MASK_* (0 -> 0x07 all) */
    uint32_t reserved[2];      /* +8..+15 0 */
} LivePhotoIpcSetCfg;

_Static_assert(sizeof(LivePhotoIpcState) == 64U, "LivePhotoIpcState must be 64 bytes");
_Static_assert(offsetof(LivePhotoIpcState, tx_count) == 12U, "LivePhotoIpcState tx_count offset");
_Static_assert(offsetof(LivePhotoIpcState, pb_consumed_rec) == 32U, "LivePhotoIpcState pb_consumed_rec offset");
_Static_assert(offsetof(LivePhotoIpcState, twin_copies) == 44U, "LivePhotoIpcState twin_copies offset");
_Static_assert(offsetof(LivePhotoIpcState, log_saves) == 52U, "LivePhotoIpcState log_saves offset");
_Static_assert(sizeof(LivePhotoIpcSetCfg) == 16U, "LivePhotoIpcSetCfg must be 16 bytes");

int live_ipc_service_call(uint32_t raw_op, const void *req, uint32_t req_bytes,
                          void *resp, uint32_t *resp_bytes);

#endif
