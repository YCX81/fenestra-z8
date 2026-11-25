/* SPDX-License-Identifier: MIT
 * livephoto.wlm Core State Machine (V71.7-r4).
 *
 * - 11 inline UI controls (<= FX_UI_CONTROLS 16) with native Debug Settings sub-page:
 *     1: PAGE_ROOT       - Live Photo / 实况照片 / 實況照片
 *     2: CHOICE (p=1)    - Live Photo (Off / On)
 *     3: CHOICE (p=1)    - Shutter Sound (On / Off)
 *     4: CHOICE (p=1)    - Playback Key (REC / Off)
 *     5: ACTION (p=1)    - Status (Off/Preparing/Short Clip/Ready/Busy/Unavailable/Card Error)
 *     6: PAGE   (p=1)    - Debug Settings / 调试设置 / 調試設定
 *     7: ACTION (p=6)    - Capture Stats (Tx/Enc/Drop)
 *     8: ACTION (p=6)    - Wave5 & Audio
 *     9: ACTION (p=6)    - Playback & Drive ([A|B])
 *    10: ACTION (p=6)    - Flag & Log (Park/Log)
 *    11: ACTION (p=6)    - Export LIVELOG.BIN (LP_OP_FLUSH_LOG)
 * - Dual-slot CRC32 persistence: <active_drive>:\FENESTRA\live-z8.0 / live-z8.1
 *   (64B 'FLP1', backward-compatible, +32 pb_key_cfg).
 * - EL1 IPC tunnel over slots.state ops 49..51 (magic 'LPI1').
 * - EN / zh-CN (8) / zh-TW (9) localization via slots.state op 15.
 * - A2 Hardening: never returns non-zero on transient IPC/UI errors.
 * - Freestanding C11, zero libc, zero relocations, zero FP/SIMD.
 */
#ifndef LIVEPHOTO_CORE_H
#define LIVEPHOTO_CORE_H

#include "fenestra_sdk.h"
#include "fenestra_file_sdk.h"
#include "livephoto_abi.h"

#define LP_BOUNCE_BYTES           1024u
#define LP_BOUNCE_PATH_OFF        0u
#define LP_BOUNCE_DONE_OFF        64u
#define LP_BOUNCE_DATA_OFF        128u

#define LP_OPEN_NOT_FOUND         UINT32_C(0x80000010)
#define LP_OPEN_REFUSED           UINT32_C(0x8000ff01)
#define LP_IO_MAX_CALLS           32u
#define LP_LOAD_MAX_RETRIES       6u
#define LP_SAVE_MAX_RETRIES       5u
#define LP_SAVE_RETRY_MS          500u
#define LP_LANG_POLL_MS           1000u
#define LP_STATE_POLL_MS          500u
#define LP_STATE_IDLE_PERIOD      16u
#define LP_UI_BUILD_MAX_RETRIES   8u

typedef struct {
    uint32_t index, offset, bytes, value;
} LpSlotsRequest;

enum LpUiControlId {
    LP_UI_PAGE_ROOT     = 1u,
    LP_UI_ENABLE        = 2u,
    LP_UI_SHUTTER_SOUND = 3u,
    LP_UI_PB_KEY        = 4u,
    LP_UI_STATUS        = 5u,
    LP_UI_DEBUG_PAGE    = 6u,
    LP_UI_DBG_CAPTURE   = 7u,
    LP_UI_DBG_W5_AUD    = 8u,
    LP_UI_DBG_PB_DRV    = 9u,
    LP_UI_DBG_FLAG_LOG  = 10u,
    LP_UI_DBG_FLUSH_LOG = 11u,
    LP_UI_CTRL_COUNT    = 11u
};

_Static_assert(LP_UI_CTRL_COUNT <= FX_UI_CONTROLS,
               "livephoto.wlm controls must fit within inline FX_UI_CONTROLS");

enum LpSlotStatus {
    LP_SLOT_UNREAD  = 0u,
    LP_SLOT_MISSING = 1u,
    LP_SLOT_CORRUPT = 2u,
    LP_SLOT_IO_ERR  = 3u,
    LP_SLOT_VALID   = 4u
};

enum LpStage {
    LP_ST_QUERY_UI = 0u,
    LP_ST_OPEN_SLOTS,
    LP_ST_LANG_INIT,
    LP_ST_STATE_INIT,
    LP_ST_OPEN_MEM,
    LP_ST_ALLOC_MEM,
    LP_ST_OPEN_CARD_OPEN,
    LP_ST_OPEN_CARD_READ,
    LP_ST_OPEN_CARD_WRITE,
    LP_ST_OPEN_CARD_CLOSE,
    LP_ST_LOAD_WRITE_PATH,
    LP_ST_LOAD_OPEN,
    LP_ST_LOAD_READ_CALL,
    LP_ST_LOAD_READ_DONE,
    LP_ST_LOAD_CLOSE,
    LP_ST_LOAD_READ_DATA,
    LP_ST_PUSH_INIT,
    LP_ST_UI_BUILD,
    LP_ST_IDLE,
    LP_ST_SAVE_WRITE_PATH,
    LP_ST_SAVE_WRITE_DATA,
    LP_ST_SAVE_OPEN,
    LP_ST_SAVE_WRITE_CALL,
    LP_ST_SAVE_READ_DONE,
    LP_ST_SAVE_CLOSE,
    LP_ST_SET_CFG,
    LP_ST_FLUSH_LOG,
    LP_ST_STATE_POLL,
    LP_ST_LANG_POLL,
    LP_ST_UI_UPDATE,
    LP_ST_UI_SYNC_GET,
    LP_ST_POLL_EVENTS
};

typedef struct {
    FxSdkClient sdk;
    uint32_t stage;

    /* Bindings & capabilities */
    uint32_t ui_caps;
    uint64_t slots_binding;
    uint64_t mem_binding;
    uint64_t buf_handle;
    uint64_t buf_native;
    uint64_t card_open_h;
    uint64_t card_read_h;
    uint64_t card_write_h;
    uint64_t card_close_h;

    uint64_t now_ms;
    uint64_t last_lang_ms;
    uint64_t last_state_ms;
    uint64_t last_reconcile_ms;
    uint32_t ui_lang;

    uint8_t has_slots;
    uint8_t has_lang_svc;
    uint8_t has_lp_ipc;
    uint8_t has_bounce;
    uint8_t card_read_ok;
    uint8_t card_write_ok;
    uint8_t state_valid;
    uint8_t focus_foreground;
    uint8_t ui_built;
    uint8_t ui_build_retries;
    uint8_t lang_refresh_needed;
    uint8_t state_refresh_needed;
    uint8_t ipc_sync_needed;
    uint8_t flush_log_pending;
    uint8_t flush_status; /* 0=idle, 1=ok, 2=err */
    char    active_drive;

    /* EL1 telemetry snapshot */
    LivePhotoIpcState el1_state;

    /* Local desired config & dual-slot live-z8.0/1 persistence */
    uint64_t cfg_gen;
    uint32_t cfg_requested;
    uint32_t cfg_mute;
    uint32_t cfg_pb_key_cfg;
    uint32_t active_slot;

    uint64_t slot_gen[2];
    uint32_t slot_req[2];
    uint32_t slot_mute[2];
    uint32_t slot_pbk[2];
    uint8_t  slot_state[2];
    uint32_t load_slot;
    uint32_t load_retries;

    /* Card I/O scratch */
    int32_t  card_fd;
    uint32_t io_done_acc;
    uint32_t io_calls;
    uint32_t io_err;
    uint32_t save_target_slot;
    uint64_t save_gen;
    uint8_t  save_raw[LP_CFG_BYTES];
    uint64_t save_fail_ms;
    uint8_t  save_pending;
    uint8_t  save_retries;
    uint8_t  save_error;

    /* IPC TX scratch */
    uint8_t  ipc_tx_buf[LP_SLOTS_HDR_BYTES + sizeof(LivePhotoIpcSetCfg)];

    /* UI state */
    uint32_t ui_build_idx;
    uint32_t ctrl_rev[LP_UI_CTRL_COUNT + 1u];
    uint32_t ui_dirty_mask;
    uint32_t ui_sync_mask;
    uint32_t ui_active_ctrl;
    uint32_t idle_polls;
    char cached_text[LP_UI_CTRL_COUNT + 1u][FX_UI_TEXT];

    /* Observability / test counters */
    uint32_t saves_completed;
    uint32_t saves_failed;
    uint32_t ipc_set_completed;
    uint32_t ipc_get_completed;
    uint32_t flush_completed;
    uint32_t flush_failed;
    uint32_t lang_switches;
    uint32_t transient_errors;
} LpModuleState;

static inline void lp_zero(void *dst, uint32_t n) {
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t i = 0u; i < n; ++i) p[i] = 0u;
}

static inline void lp_copy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0u; i < n; ++i) d[i] = s[i];
}

static inline int lp_mem_eq(const void *a, const void *b, uint32_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (uint32_t i = 0u; i < n; ++i) {
        if (pa[i] != pb[i]) return 0;
    }
    return 1;
}

static inline uint32_t lp_utf8_len(const char *s) {
    uint8_t b0 = (uint8_t)s[0];
    if (b0 < 0x80u) return 1u;
    if ((b0 & 0xe0u) == 0xc0u) return s[1] ? 2u : 1u;
    if ((b0 & 0xf0u) == 0xe0u) return (s[1] && s[2]) ? 3u : 1u;
    if ((b0 & 0xf8u) == 0xf0u) return (s[1] && s[2] && s[3]) ? 4u : 1u;
    return 1u;
}

static inline uint32_t lp_lang_bucket(uint32_t lang) {
    if (lang == LP_LANG_ZH_CN) return 1u;
    if (lang == LP_LANG_ZH_TW) return 2u;
    return 0u;
}

static inline const char *lp_tr(uint32_t lang, const char *en, const char *sc, const char *tc) {
    if (lang == LP_LANG_ZH_CN) return sc;
    if (lang == LP_LANG_ZH_TW) return tc;
    return en;
}

static __attribute__((noinline)) void lp_str_copy(char *dst, uint32_t cap, const char *src) {
    if (!cap) return;
    uint32_t pos = 0u;
    uint32_t max_b = cap - 1u;
    while (src && src[pos]) {
        uint32_t clen = lp_utf8_len(src + pos);
        if (pos + clen > max_b) break;
        for (uint32_t k = 0u; k < clen; ++k) {
            dst[pos] = src[pos];
            ++pos;
        }
    }
    while (pos < cap) dst[pos++] = '\0';
}

static __attribute__((noinline)) void lp_append_str(char *dst, uint32_t cap, uint32_t *pos, const char *src) {
    if (!dst || !pos || !src || !cap) return;
    uint32_t max_b = cap - 1u;
    uint32_t i = 0u;
    while (src[i]) {
        uint32_t clen = lp_utf8_len(src + i);
        if (*pos + clen > max_b) break;
        for (uint32_t k = 0u; k < clen; ++k) dst[(*pos)++] = src[i++];
    }
    dst[*pos] = '\0';
}

static __attribute__((noinline)) void lp_append_u32(char *dst, uint32_t cap, uint32_t *pos, uint32_t v) {
    char rev[12];
    uint32_t n = 0u;
    do {
        rev[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && n < 12u);
    while (n > 0u && *pos + 1u < cap) {
        dst[(*pos)++] = rev[--n];
    }
    if (cap) dst[*pos] = '\0';
}

static inline char lp_effective_drive(const LpModuleState *st) {
    return (st && st->active_drive == 'B') ? 'B' : 'A';
}

static inline void lp_cfg_path(char drive, uint32_t slot, char out[32]) {
    lp_zero(out, 32u);
    lp_str_copy(out, 32u, slot == 0u ? "A:\\FENESTRA\\live-z8.0" : "A:\\FENESTRA\\live-z8.1");
    out[0] = (drive == 'B') ? 'B' : 'A';
}

static __attribute__((noinline)) void lp_format_status(const LpModuleState *st, char out[FX_UI_TEXT]) {
    lp_zero(out, FX_UI_TEXT);
    uint32_t lang = st->ui_lang;
    if (st->save_error) {
        lp_str_copy(out, FX_UI_TEXT, lp_tr(lang, "Card Error", "卡错误", "卡錯誤"));
        return;
    }
    if (!st->has_lp_ipc) {
        lp_str_copy(out, FX_UI_TEXT, lp_tr(lang, "Unavailable", "不可用", "不可用"));
        return;
    }
    uint32_t phase = st->state_valid ? st->el1_state.ui_phase
                                     : (st->cfg_requested ? LP_PHASE_PREPARING : LP_PHASE_OFF);
    switch (phase) {
    case LP_PHASE_PREPARING:
        lp_str_copy(out, FX_UI_TEXT, lp_tr(lang, "Preparing", "准备中", "準備中"));
        break;
    case LP_PHASE_SHORT_CLIP:
        lp_str_copy(out, FX_UI_TEXT, lp_tr(lang, "Short Clip", "短片段", "短片段"));
        break;
    case LP_PHASE_READY:
        lp_str_copy(out, FX_UI_TEXT, lp_tr(lang, "Ready", "就绪", "就緒"));
        break;
    case LP_PHASE_BUSY:
        lp_str_copy(out, FX_UI_TEXT, lp_tr(lang, "Busy", "保存中", "儲存中"));
        break;
    case LP_PHASE_UNAVAILABLE:
        lp_str_copy(out, FX_UI_TEXT, lp_tr(lang, "Unavailable", "不可用", "不可用"));
        break;
    case LP_PHASE_CARD_ERR:
        lp_str_copy(out, FX_UI_TEXT, lp_tr(lang, "Card Error", "卡错误", "卡錯誤"));
        break;
    default:
        lp_str_copy(out, FX_UI_TEXT, lp_tr(lang, "Off", "关闭", "關閉"));
        break;
    }
}

static __attribute__((noinline)) void lp_format_dbg_capture(const LpModuleState *st, char out[FX_UI_TEXT]) {
    lp_zero(out, FX_UI_TEXT);
    uint32_t pos = 0u;
    if (!st->state_valid) {
        lp_append_str(out, FX_UI_TEXT, &pos, "No EL1");
        return;
    }
    const LivePhotoIpcState *s = &st->el1_state;
    lp_append_str(out, FX_UI_TEXT, &pos, "Tx:");
    lp_append_u32(out, FX_UI_TEXT, &pos, s->tx_count);
    lp_append_str(out, FX_UI_TEXT, &pos, "/");
    lp_append_u32(out, FX_UI_TEXT, &pos, s->tx_err);
    lp_append_str(out, FX_UI_TEXT, &pos, " Enc:");
    lp_append_u32(out, FX_UI_TEXT, &pos, s->encoded);
    lp_append_str(out, FX_UI_TEXT, &pos, " Drop:");
    lp_append_u32(out, FX_UI_TEXT, &pos, s->dropped);
}

static __attribute__((noinline)) void lp_format_dbg_w5_aud(const LpModuleState *st, char out[FX_UI_TEXT]) {
    lp_zero(out, FX_UI_TEXT);
    uint32_t pos = 0u;
    if (!st->state_valid) {
        lp_append_str(out, FX_UI_TEXT, &pos, "No EL1");
        return;
    }
    const LivePhotoIpcState *s = &st->el1_state;
    lp_append_str(out, FX_UI_TEXT, &pos, "W5:");
    if (s->w5_flags & 2u) {
        lp_append_str(out, FX_UI_TEXT, &pos, "ERR");
    } else if (s->w5_parked) {
        lp_append_str(out, FX_UI_TEXT, &pos, "PARK");
    } else if (s->w5_flags & 1u) {
        lp_append_str(out, FX_UI_TEXT, &pos, "ACT");
    } else {
        lp_append_str(out, FX_UI_TEXT, &pos, "IDLE");
    }
    lp_append_str(out, FX_UI_TEXT, &pos, " Aud:");
    lp_append_u32(out, FX_UI_TEXT, &pos, s->audio_phase);
    lp_append_str(out, FX_UI_TEXT, &pos, "/");
    lp_append_u32(out, FX_UI_TEXT, &pos, s->audio_tracks);
}

static __attribute__((noinline)) void lp_format_dbg_pb_drv(const LpModuleState *st, char out[FX_UI_TEXT]) {
    lp_zero(out, FX_UI_TEXT);
    uint32_t pos = 0u;
    char drv = lp_effective_drive(st);
    if (!st->state_valid) {
        lp_append_str(out, FX_UI_TEXT, &pos, drv == 'B' ? "No EL1 [B]" : "No EL1 [A]");
        return;
    }
    const LivePhotoIpcState *s = &st->el1_state;
    lp_append_str(out, FX_UI_TEXT, &pos, "REC:");
    lp_append_u32(out, FX_UI_TEXT, &pos, s->pb_consumed_rec);
    lp_append_str(out, FX_UI_TEXT, &pos, " PB:");
    lp_append_u32(out, FX_UI_TEXT, &pos, s->pb_frames);
    lp_append_str(out, FX_UI_TEXT, &pos, "/");
    lp_append_u32(out, FX_UI_TEXT, &pos, s->pb_err);
    lp_append_str(out, FX_UI_TEXT, &pos, drv == 'B' ? " [B]" : " [A]");
}

static __attribute__((noinline)) void lp_format_dbg_flag_log(const LpModuleState *st, char out[FX_UI_TEXT]) {
    lp_zero(out, FX_UI_TEXT);
    uint32_t pos = 0u;
    if (!st->state_valid) {
        lp_append_str(out, FX_UI_TEXT, &pos, "No EL1");
        return;
    }
    const LivePhotoIpcState *s = &st->el1_state;
    lp_append_str(out, FX_UI_TEXT, &pos, "Park:");
    lp_append_u32(out, FX_UI_TEXT, &pos, s->flag_parks);
    lp_append_str(out, FX_UI_TEXT, &pos, "/");
    lp_append_u32(out, FX_UI_TEXT, &pos, s->flag_restores);
    lp_append_str(out, FX_UI_TEXT, &pos, " Log:");
    lp_append_u32(out, FX_UI_TEXT, &pos, s->log_saves);
    if (s->log_err > 0u) {
        lp_append_str(out, FX_UI_TEXT, &pos, " E:");
        lp_append_u32(out, FX_UI_TEXT, &pos, s->log_err);
    }
}

static __attribute__((noinline)) void lp_format_dbg_flush(const LpModuleState *st, char out[FX_UI_TEXT]) {
    lp_zero(out, FX_UI_TEXT);
    uint32_t lang = st->ui_lang;
    if (!st->has_lp_ipc) {
        lp_str_copy(out, FX_UI_TEXT, lp_tr(lang, "No EL1", "无 EL1", "無 EL1"));
    } else if (st->flush_status == 1u) {
        lp_str_copy(out, FX_UI_TEXT, lp_tr(lang, "Exported", "已导出", "已匯出"));
    } else if (st->flush_status == 2u) {
        lp_str_copy(out, FX_UI_TEXT, lp_tr(lang, "Export Error", "导出失败", "匯出失敗"));
    } else {
        lp_str_copy(out, FX_UI_TEXT, lp_tr(lang, "Tap to Export", "按此导出", "按此匯出"));
    }
}

static __attribute__((noinline)) void lp_format_ctrl_text(const LpModuleState *st, uint32_t id, char out[FX_UI_TEXT]) {
    switch (id) {
    case LP_UI_STATUS:        lp_format_status(st, out); break;
    case LP_UI_DBG_CAPTURE:   lp_format_dbg_capture(st, out); break;
    case LP_UI_DBG_W5_AUD:    lp_format_dbg_w5_aud(st, out); break;
    case LP_UI_DBG_PB_DRV:    lp_format_dbg_pb_drv(st, out); break;
    case LP_UI_DBG_FLAG_LOG:  lp_format_dbg_flag_log(st, out); break;
    case LP_UI_DBG_FLUSH_LOG: lp_format_dbg_flush(st, out); break;
    default:                  lp_zero(out, FX_UI_TEXT); break;
    }
}

static inline void lp_mark_dynamic_dirty(LpModuleState *st) {
    char tmp[FX_UI_TEXT];
    for (uint32_t id = LP_UI_STATUS; id <= LP_UI_DBG_FLUSH_LOG; ++id) {
        if (id == LP_UI_DEBUG_PAGE) continue;
        lp_format_ctrl_text(st, id, tmp);
        if (!lp_mem_eq(tmp, st->cached_text[id], FX_UI_TEXT)) {
            st->ui_dirty_mask |= (UINT32_C(1) << id);
        }
    }
}

static __attribute__((noinline)) int lp_build_ui_control(uint32_t id,
                                                         const LpModuleState *st,
                                                         FxUiControl *out) {
    if (!st || !out || id < LP_UI_PAGE_ROOT || id > LP_UI_CTRL_COUNT) {
        return 0;
    }
    uint32_t lang = st->ui_lang;
    lp_zero(out, (uint32_t)sizeof(*out));
    out->abi = FX_PLATFORM_ABI;
    out->bytes = (uint32_t)sizeof(*out);
    out->id = id;
    out->flags = FX_UI_VISIBLE | FX_UI_ENABLED;

    switch (id) {
    case LP_UI_PAGE_ROOT:
        out->parent = 0u;
        out->kind = FX_UI_PAGE;
        if (st->ui_caps & FX_UI_CAP_ROOT) {
            out->flags |= FX_UI_ROOT;
        }
        lp_str_copy(out->label, FX_UI_TEXT,
                    lp_tr(lang, "Live Photo", "实况照片", "實況照片"));
        return 1;

    case LP_UI_ENABLE:
        out->parent = LP_UI_PAGE_ROOT;
        out->kind = FX_UI_CHOICE;
        out->option_count = 2u;
        out->value = st->cfg_requested ? 1u : 0u;
        lp_str_copy(out->label, FX_UI_TEXT,
                    lp_tr(lang, "Live Photo", "实况照片", "實況照片"));
        lp_str_copy(out->options[0], 32u, lp_tr(lang, "Off", "关闭", "關閉"));
        lp_str_copy(out->options[1], 32u, lp_tr(lang, "On", "开启", "開啟"));
        return 1;

    case LP_UI_SHUTTER_SOUND:
        out->parent = LP_UI_PAGE_ROOT;
        out->kind = FX_UI_CHOICE;
        out->option_count = 2u;
        out->value = st->cfg_mute ? 1u : 0u; /* 0=Sound On (mute=0), 1=Sound Off (mute=1) */
        lp_str_copy(out->label, FX_UI_TEXT,
                    lp_tr(lang, "Shutter Sound", "快门声音", "快門聲音"));
        lp_str_copy(out->options[0], 32u, lp_tr(lang, "On", "开启", "開啟"));
        lp_str_copy(out->options[1], 32u, lp_tr(lang, "Off", "关闭", "關閉"));
        return 1;

    case LP_UI_PB_KEY:
        out->parent = LP_UI_PAGE_ROOT;
        out->kind = FX_UI_CHOICE;
        out->option_count = 2u;
        out->value = (st->cfg_pb_key_cfg == LP_CFG_PB_OFF) ? 1u : 0u;
        lp_str_copy(out->label, FX_UI_TEXT,
                    lp_tr(lang, "Playback Key", "回放按键", "回放按鍵"));
        lp_str_copy(out->options[0], 32u, lp_tr(lang, "REC", "REC 键", "REC 鍵"));
        lp_str_copy(out->options[1], 32u, lp_tr(lang, "Off", "关闭", "關閉"));
        return 1;

    case LP_UI_STATUS:
        out->parent = LP_UI_PAGE_ROOT;
        out->kind = FX_UI_ACTION;
        lp_str_copy(out->label, FX_UI_TEXT,
                    lp_tr(lang, "Status", "当前状态", "目前狀態"));
        lp_format_status(st, out->text);
        return 1;

    case LP_UI_DEBUG_PAGE:
        out->parent = LP_UI_PAGE_ROOT;
        out->kind = FX_UI_PAGE;
        lp_str_copy(out->label, FX_UI_TEXT,
                    lp_tr(lang, "Debug Settings", "调试设置", "調試設定"));
        return 1;

    case LP_UI_DBG_CAPTURE:
        out->parent = LP_UI_DEBUG_PAGE;
        out->kind = FX_UI_ACTION;
        lp_str_copy(out->label, FX_UI_TEXT,
                    lp_tr(lang, "Capture Stats", "录制统计", "錄製統計"));
        lp_format_dbg_capture(st, out->text);
        return 1;

    case LP_UI_DBG_W5_AUD:
        out->parent = LP_UI_DEBUG_PAGE;
        out->kind = FX_UI_ACTION;
        lp_str_copy(out->label, FX_UI_TEXT,
                    lp_tr(lang, "Wave5 & Audio", "Wave5 与音频", "Wave5 與音訊"));
        lp_format_dbg_w5_aud(st, out->text);
        return 1;

    case LP_UI_DBG_PB_DRV:
        out->parent = LP_UI_DEBUG_PAGE;
        out->kind = FX_UI_ACTION;
        lp_str_copy(out->label, FX_UI_TEXT,
                    lp_tr(lang, "Playback & Drive", "回放与双卡", "回放與雙卡"));
        lp_format_dbg_pb_drv(st, out->text);
        return 1;

    case LP_UI_DBG_FLAG_LOG:
        out->parent = LP_UI_DEBUG_PAGE;
        out->kind = FX_UI_ACTION;
        lp_str_copy(out->label, FX_UI_TEXT,
                    lp_tr(lang, "Flag & Log", "关机保护与日志", "關機保護與記錄"));
        lp_format_dbg_flag_log(st, out->text);
        return 1;

    case LP_UI_DBG_FLUSH_LOG:
        out->parent = LP_UI_DEBUG_PAGE;
        out->kind = FX_UI_ACTION;
        lp_str_copy(out->label, FX_UI_TEXT,
                    lp_tr(lang, "Export LIVELOG.BIN", "导出诊断日志", "匯出檢測記錄"));
        lp_format_dbg_flush(st, out->text);
        return 1;

    default:
        return 0;
    }
}

static inline FxResult lp_submit_mem_write(LpModuleState *st, const FxServiceModule *api,
                                           uint32_t offset, const void *data, uint32_t bytes) {
    uint8_t payload[sizeof(FxMemoryRequest) + LP_CFG_BYTES];
    if (bytes > LP_CFG_BYTES) return FX_BOUNDS;
    FxMemoryRequest req = { .handle = st->buf_handle, .bytes = bytes, .offset = offset };
    lp_copy(payload, &req, (uint32_t)sizeof(req));
    lp_copy(payload + sizeof(req), data, bytes);
    return fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->mem_binding,
                           FX_MEMORY_WRITE, 0u, payload, (uint32_t)sizeof(req) + bytes);
}

static inline FxResult lp_submit_mem_read(LpModuleState *st, const FxServiceModule *api,
                                          uint32_t offset, uint32_t bytes) {
    FxMemoryRequest req = { .handle = st->buf_handle, .bytes = bytes, .offset = offset };
    return fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->mem_binding,
                           FX_MEMORY_READ, 0u, &req, (uint32_t)sizeof(req));
}

static inline FxResult lp_submit_ipc(LpModuleState *st, const FxServiceModule *api,
                                     uint32_t raw_op, const void *payload, uint32_t payload_bytes) {
    if (payload_bytes > sizeof(LivePhotoIpcSetCfg)) return FX_ARGUMENT;
    uint32_t hdr[4] = { LP_SLOTS_TUNNEL_MAGIC, payload_bytes, 0u, 0u };
    lp_copy(st->ipc_tx_buf, hdr, LP_SLOTS_HDR_BYTES);
    if (payload && payload_bytes) {
        lp_copy(st->ipc_tx_buf + LP_SLOTS_HDR_BYTES, payload, payload_bytes);
    }
    return fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->slots_binding,
                           raw_op, 0u, st->ipc_tx_buf, LP_SLOTS_HDR_BYTES + payload_bytes);
}

static inline void lp_parse_state_reply(LpModuleState *st, const FxServiceReply *r) {
    if (r->result != FX_OK || r->bytes != sizeof(LivePhotoIpcState)) return;
    LivePhotoIpcState tmp;
    lp_copy(&tmp, r->data, (uint32_t)sizeof(tmp));
    if (tmp.magic != LP_SLOTS_TUNNEL_MAGIC) return;
    st->el1_state = tmp;
    st->state_valid = 1u;
    st->has_lp_ipc = 1u;
    if (tmp.active_drive == 'A' || tmp.active_drive == 'B') {
        st->active_drive = (char)tmp.active_drive;
    }
    /* V71.7-r6: Auto-reconcile EL1 if card/USB event 24 revoked r->requested
     * (LP_W5_FLAG_RELOAD) or if EL1 drifted out of sync with cfg_requested. */
    if (st->ui_built && (tmp.w5_flags & LP_W5_FLAG_LP_LOADED)) {
        uint32_t want_req = st->cfg_requested ? 1u : 0u;
        uint32_t want_mute = st->cfg_mute ? 1u : 0u;
        uint8_t  want_pbk = lp_pb_key_from_cfg(st->cfg_pb_key_cfg);
        int drifted = ((tmp.w5_flags & LP_W5_FLAG_RELOAD) != 0u) ||
                      ((uint32_t)tmp.requested != want_req) ||
                      ((uint32_t)tmp.mute != want_mute) ||
                      (tmp.pb_key != want_pbk);
        if (drifted &&
            (st->last_reconcile_ms == 0u || st->now_ms < st->last_reconcile_ms ||
             (st->now_ms - st->last_reconcile_ms) >= 1500u)) {
            st->last_reconcile_ms = st->now_ms;
            st->ipc_sync_needed = 1u;
        }
    }
    lp_mark_dynamic_dirty(st);
}

static inline void lp_start_after_card_open(LpModuleState *st) {
    if (st->card_read_ok) {
        st->load_slot = 0u;
        st->slot_state[0] = LP_SLOT_UNREAD;
        st->slot_state[1] = LP_SLOT_UNREAD;
        st->stage = LP_ST_LOAD_WRITE_PATH;
    } else {
        st->cfg_gen = 0u;
        st->cfg_requested = 0u;
        st->cfg_mute = 0u;
        st->cfg_pb_key_cfg = LP_CFG_PB_REC;
        st->active_slot = 1u;
        st->stage = st->has_lp_ipc ? LP_ST_PUSH_INIT : LP_ST_UI_BUILD;
    }
}

static inline void lp_finish_load(LpModuleState *st) {
    int v0 = (st->slot_state[0] == LP_SLOT_VALID);
    int v1 = (st->slot_state[1] == LP_SLOT_VALID);
    int io_err = (st->slot_state[0] == LP_SLOT_IO_ERR || st->slot_state[1] == LP_SLOT_IO_ERR);
    if (!v0 && !v1 && io_err && st->load_retries < LP_LOAD_MAX_RETRIES) {
        ++st->load_retries;
        st->load_slot = 0u;
        st->slot_state[0] = LP_SLOT_UNREAD;
        st->slot_state[1] = LP_SLOT_UNREAD;
        st->stage = LP_ST_LOAD_WRITE_PATH;
        return;
    }
    if (v0 && v1) {
        uint32_t pick = (st->slot_gen[1] > st->slot_gen[0]) ? 1u : 0u;
        st->active_slot = pick;
        st->cfg_gen = st->slot_gen[pick];
        st->cfg_requested = st->slot_req[pick];
        st->cfg_mute = st->slot_mute[pick];
        st->cfg_pb_key_cfg = (st->slot_pbk[pick] == LP_CFG_PB_OFF) ? LP_CFG_PB_OFF : LP_CFG_PB_REC;
    } else if (v0) {
        st->active_slot = 0u;
        st->cfg_gen = st->slot_gen[0];
        st->cfg_requested = st->slot_req[0];
        st->cfg_mute = st->slot_mute[0];
        st->cfg_pb_key_cfg = (st->slot_pbk[0] == LP_CFG_PB_OFF) ? LP_CFG_PB_OFF : LP_CFG_PB_REC;
    } else if (v1) {
        st->active_slot = 1u;
        st->cfg_gen = st->slot_gen[1];
        st->cfg_requested = st->slot_req[1];
        st->cfg_mute = st->slot_mute[1];
        st->cfg_pb_key_cfg = (st->slot_pbk[1] == LP_CFG_PB_OFF) ? LP_CFG_PB_OFF : LP_CFG_PB_REC;
    } else {
        st->active_slot = 1u;
        st->cfg_gen = 0u;
        st->cfg_requested = 0u;
        st->cfg_mute = 0u;
        st->cfg_pb_key_cfg = LP_CFG_PB_REC;
    }
    st->stage = st->has_lp_ipc ? LP_ST_PUSH_INIT : LP_ST_UI_BUILD;
}

static inline void lp_advance_load(LpModuleState *st) {
    if (st->load_slot == 0u) {
        st->load_slot = 1u;
        st->stage = LP_ST_LOAD_WRITE_PATH;
    } else {
        lp_finish_load(st);
    }
}

static inline void lp_handle_save_fail(LpModuleState *st) {
    ++st->saves_failed;
    if (st->save_retries < LP_SAVE_MAX_RETRIES) {
        ++st->save_retries;
        st->save_fail_ms = st->now_ms;
        st->save_pending = 1u;
    } else {
        st->save_pending = 0u;
        st->save_error = 1u;
        lp_mark_dynamic_dirty(st);
    }
    st->stage = LP_ST_IDLE;
}

static inline uint32_t lp_lowest_ctrl_bit(uint32_t mask) {
    for (uint32_t id = LP_UI_PAGE_ROOT; id <= LP_UI_CTRL_COUNT; ++id) {
        if (mask & (UINT32_C(1) << id)) return id;
    }
    return 0u;
}

static inline void lp_recover(LpModuleState *st) {
    ++st->transient_errors;
    st->sdk.pending = 0u;
    if (st->ui_built) {
        if (st->card_fd >= 0) {
            if (st->stage >= LP_ST_SAVE_WRITE_PATH && st->stage <= LP_ST_SAVE_CLOSE) {
                st->io_err = 1u;
                st->stage = LP_ST_SAVE_CLOSE;
                return;
            }
            st->card_fd = -1;
        }
        if (st->stage >= LP_ST_SAVE_WRITE_PATH && st->stage <= LP_ST_SAVE_CLOSE) {
            lp_handle_save_fail(st);
            return;
        }
        if (st->stage == LP_ST_UI_UPDATE || st->stage == LP_ST_UI_SYNC_GET) {
            if (st->ui_active_ctrl <= LP_UI_CTRL_COUNT) {
                st->ui_dirty_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
                st->ui_sync_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
            }
            st->ui_active_ctrl = 0u;
        }
        st->stage = LP_ST_IDLE;
        return;
    }
    switch (st->stage) {
    case LP_ST_QUERY_UI:
        st->ui_caps = 0u;
        st->stage = LP_ST_OPEN_SLOTS;
        break;
    case LP_ST_OPEN_SLOTS:
    case LP_ST_LANG_INIT:
    case LP_ST_STATE_INIT:
        st->stage = LP_ST_OPEN_MEM;
        break;
    case LP_ST_OPEN_MEM:
    case LP_ST_ALLOC_MEM:
    case LP_ST_OPEN_CARD_OPEN:
    case LP_ST_OPEN_CARD_READ:
    case LP_ST_OPEN_CARD_WRITE:
    case LP_ST_OPEN_CARD_CLOSE:
        st->card_read_ok = 0u;
        st->card_write_ok = 0u;
        lp_start_after_card_open(st);
        break;
    case LP_ST_LOAD_WRITE_PATH:
    case LP_ST_LOAD_OPEN:
    case LP_ST_LOAD_READ_CALL:
    case LP_ST_LOAD_READ_DONE:
    case LP_ST_LOAD_CLOSE:
    case LP_ST_LOAD_READ_DATA:
        st->card_fd = -1;
        st->slot_state[st->load_slot] = LP_SLOT_IO_ERR;
        lp_advance_load(st);
        break;
    case LP_ST_PUSH_INIT:
        st->stage = LP_ST_UI_BUILD;
        break;
    case LP_ST_UI_BUILD:
        ++st->ui_build_retries;
        if (st->ui_build_retries > LP_UI_BUILD_MAX_RETRIES) {
            st->ui_build_retries = 0u;
            ++st->ui_build_idx;
            if (st->ui_build_idx >= LP_UI_CTRL_COUNT) {
                st->ui_built = 1u;
                st->stage = LP_ST_IDLE;
            }
        }
        break;
    default:
        st->ui_built = 1u;
        st->stage = LP_ST_IDLE;
        break;
    }
}

static inline void lp_commit_change(LpModuleState *st, uint32_t ctrl_id) {
    if (st->cfg_gen < UINT64_MAX) ++st->cfg_gen;
    st->save_pending = 1u;
    st->save_retries = 0u;
    st->save_error = 0u;
    if (st->has_lp_ipc) {
        st->ipc_sync_needed = 1u;
    }
    st->ui_sync_mask |= (UINT32_C(1) << ctrl_id);
    lp_mark_dynamic_dirty(st);
}

static inline void lp_handle_ui_event(LpModuleState *st, const FxServiceReply *r) {
    if (r->bytes != sizeof(FxUiEvent)) return;
    FxUiEvent ev;
    lp_copy(&ev, r->data, (uint32_t)sizeof(ev));

    if (ev.event == FX_INPUT_FOCUS) {
        st->focus_foreground = 1u;
        if (st->has_lp_ipc) st->state_refresh_needed = 1u;
        if (st->has_lang_svc) st->lang_refresh_needed = 1u;
        return;
    }
    if (ev.event == FX_INPUT_BLUR) {
        st->focus_foreground = 0u;
        return;
    }
    if (ev.control < LP_UI_ENABLE || ev.control > LP_UI_CTRL_COUNT) return;
    if (st->ui_caps & FX_UI_CAP_EVENT_REVISION) {
        uint32_t ev_rev = fx_sdk_event_revision(&ev);
        uint32_t exp = st->ctrl_rev[ev.control];
        if (ev_rev != 0u && exp != 0u && ev_rev != exp) return;
    }

    if (ev.event == FX_INPUT_CHANGE) {
        if (ev.control == LP_UI_ENABLE && ev.value <= 1u) {
            int el1_mismatch = (st->has_lp_ipc && st->state_valid &&
                                (uint32_t)st->el1_state.requested != ev.value);
            if (st->cfg_requested != ev.value) {
                st->cfg_requested = ev.value;
                lp_commit_change(st, ev.control);
            } else if (el1_mismatch) {
                st->ipc_sync_needed = 1u;
                st->ui_sync_mask |= (UINT32_C(1) << ev.control);
                lp_mark_dynamic_dirty(st);
            } else {
                st->ui_sync_mask |= (UINT32_C(1) << ev.control);
            }
            return;
        }
        if (ev.control == LP_UI_SHUTTER_SOUND && ev.value <= 1u) {
            int el1_mismatch = (st->has_lp_ipc && st->state_valid &&
                                (uint32_t)st->el1_state.mute != ev.value);
            if (st->cfg_mute != ev.value) {
                st->cfg_mute = ev.value;
                lp_commit_change(st, ev.control);
            } else if (el1_mismatch) {
                st->ipc_sync_needed = 1u;
                st->ui_sync_mask |= (UINT32_C(1) << ev.control);
                lp_mark_dynamic_dirty(st);
            } else {
                st->ui_sync_mask |= (UINT32_C(1) << ev.control);
            }
            return;
        }
        if (ev.control == LP_UI_PB_KEY && ev.value <= 1u) {
            uint32_t next_pbk = (ev.value == 1u) ? LP_CFG_PB_OFF : LP_CFG_PB_REC;
            int el1_mismatch = (st->has_lp_ipc && st->state_valid &&
                                st->el1_state.pb_key != lp_pb_key_from_cfg(next_pbk));
            if (st->cfg_pb_key_cfg != next_pbk) {
                st->cfg_pb_key_cfg = next_pbk;
                lp_commit_change(st, ev.control);
            } else if (el1_mismatch) {
                st->ipc_sync_needed = 1u;
                st->ui_sync_mask |= (UINT32_C(1) << ev.control);
                lp_mark_dynamic_dirty(st);
            } else {
                st->ui_sync_mask |= (UINT32_C(1) << ev.control);
            }
            return;
        }
    }

    if (ev.event == FX_INPUT_ACTIVATE) {
        if (ev.control == LP_UI_DBG_FLUSH_LOG) {
            if (st->has_lp_ipc) {
                st->flush_log_pending = 1u;
            } else {
                st->flush_status = 2u;
                lp_mark_dynamic_dirty(st);
            }
            return;
        }
        if (ev.control == LP_UI_STATUS ||
            (ev.control >= LP_UI_DBG_CAPTURE && ev.control <= LP_UI_DBG_FLAG_LOG)) {
            if (st->has_lp_ipc) {
                st->state_refresh_needed = 1u;
                if (ev.control == LP_UI_STATUS && st->state_valid &&
                    (uint32_t)st->el1_state.requested != (st->cfg_requested ? 1u : 0u)) {
                    st->ipc_sync_needed = 1u;
                }
            }
            else lp_mark_dynamic_dirty(st);
            return;
        }
    }
}

static inline void lp_handle_reply(LpModuleState *st, const FxServiceReply *r) {
    if (r->result == FX_BUSY) return; /* retry current stage */

    switch (st->stage) {
    case LP_ST_QUERY_UI:
        if (r->result == FX_OK && r->bytes == sizeof(FxUiInfo)) {
            FxUiInfo info;
            lp_copy(&info, r->data, (uint32_t)sizeof(info));
            if (info.abi == 1u && info.bytes == sizeof(info)) {
                st->ui_caps = info.capabilities;
            }
        }
        st->stage = LP_ST_OPEN_SLOTS;
        return;

    case LP_ST_OPEN_SLOTS:
        if (r->result == FX_OK) {
            st->slots_binding = r->value[0];
            st->has_slots = 1u;
            st->stage = LP_ST_LANG_INIT;
        } else {
            st->has_slots = 0u;
            st->has_lp_ipc = 0u;
            st->ui_lang = LP_LANG_EN;
            st->stage = LP_ST_OPEN_MEM;
        }
        return;

    case LP_ST_LANG_INIT:
        if (r->result == FX_OK && r->value[1] == LP_LANG_MAGIC) {
            st->has_lang_svc = 1u;
            st->ui_lang = (uint32_t)r->value[0];
            st->last_lang_ms = st->now_ms;
        } else {
            st->has_lang_svc = 0u;
            st->ui_lang = LP_LANG_EN;
        }
        st->stage = LP_ST_STATE_INIT;
        return;

    case LP_ST_STATE_INIT:
        if (r->result == FX_OK && r->bytes == sizeof(LivePhotoIpcState)) {
            ++st->ipc_get_completed;
            lp_parse_state_reply(st, r);
        } else {
            st->has_lp_ipc = 0u;
        }
        st->stage = LP_ST_OPEN_MEM;
        return;

    case LP_ST_OPEN_MEM:
        if (r->result == FX_OK) {
            st->mem_binding = r->value[0];
            st->stage = LP_ST_ALLOC_MEM;
        } else {
            st->has_bounce = 0u;
            lp_start_after_card_open(st);
        }
        return;

    case LP_ST_ALLOC_MEM:
        if (r->result == FX_OK && r->value[1] != 0u) {
            st->buf_handle = r->value[0];
            st->buf_native = r->value[1];
            st->has_bounce = 1u;
            st->stage = LP_ST_OPEN_CARD_OPEN;
        } else {
            st->has_bounce = 0u;
            lp_start_after_card_open(st);
        }
        return;

    case LP_ST_OPEN_CARD_OPEN:
        st->card_open_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->stage = LP_ST_OPEN_CARD_READ;
        return;

    case LP_ST_OPEN_CARD_READ:
        st->card_read_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->stage = LP_ST_OPEN_CARD_WRITE;
        return;

    case LP_ST_OPEN_CARD_WRITE:
        st->card_write_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->stage = LP_ST_OPEN_CARD_CLOSE;
        return;

    case LP_ST_OPEN_CARD_CLOSE:
        st->card_close_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->card_read_ok = (st->has_bounce && st->card_open_h && st->card_read_h && st->card_close_h) ? 1u : 0u;
        st->card_write_ok = (st->has_bounce && st->card_open_h && st->card_write_h && st->card_close_h) ? 1u : 0u;
        lp_start_after_card_open(st);
        return;

    case LP_ST_LOAD_WRITE_PATH:
        if (r->result != FX_OK) {
            st->slot_state[st->load_slot] = LP_SLOT_IO_ERR;
            lp_advance_load(st);
        } else {
            st->card_fd = -1;
            st->io_done_acc = 0u;
            st->io_calls = 0u;
            st->io_err = 0u;
            st->stage = LP_ST_LOAD_OPEN;
        }
        return;

    case LP_ST_LOAD_OPEN:
        if (r->result != FX_OK) {
            st->slot_state[st->load_slot] = LP_SLOT_IO_ERR;
            lp_advance_load(st);
        } else {
            int32_t fd = (int32_t)(uint32_t)r->value[0];
            if (fd >= 0) {
                st->card_fd = fd;
                st->stage = LP_ST_LOAD_READ_CALL;
            } else if ((uint32_t)fd == LP_OPEN_NOT_FOUND) {
                st->slot_state[st->load_slot] = LP_SLOT_MISSING;
                lp_advance_load(st);
            } else {
                st->slot_state[st->load_slot] = LP_SLOT_IO_ERR;
                lp_advance_load(st);
            }
        }
        return;

    case LP_ST_LOAD_READ_CALL:
        if (r->result != FX_OK || (uint32_t)r->value[0] != 0u) {
            st->io_err = 1u;
            st->stage = LP_ST_LOAD_CLOSE;
        } else {
            st->stage = LP_ST_LOAD_READ_DONE;
        }
        return;

    case LP_ST_LOAD_READ_DONE:
        if (r->result != FX_OK || r->bytes != 4u) {
            st->io_err = 1u;
            st->stage = LP_ST_LOAD_CLOSE;
        } else {
            uint32_t done = lp_rd_u32(r->data);
            if (done > LP_CFG_READ_MAX - st->io_done_acc) {
                st->io_err = 1u;
                st->stage = LP_ST_LOAD_CLOSE;
            } else {
                st->io_done_acc += done;
                ++st->io_calls;
                if (done > 0u && st->io_done_acc < LP_CFG_READ_MAX && st->io_calls < LP_IO_MAX_CALLS) {
                    st->stage = LP_ST_LOAD_READ_CALL;
                } else {
                    st->stage = LP_ST_LOAD_CLOSE;
                }
            }
        }
        return;

    case LP_ST_LOAD_CLOSE: {
        uint32_t cerr = (r->result == FX_OK) ? (uint32_t)r->value[0] : 1u;
        st->card_fd = -1;
        if (cerr != 0u || st->io_err) {
            st->slot_state[st->load_slot] = LP_SLOT_IO_ERR;
            lp_advance_load(st);
        } else if (st->io_done_acc != LP_CFG_BYTES) {
            st->slot_state[st->load_slot] = LP_SLOT_CORRUPT;
            lp_advance_load(st);
        } else {
            st->stage = LP_ST_LOAD_READ_DATA;
        }
        return;
    }

    case LP_ST_LOAD_READ_DATA:
        if (r->result != FX_OK || r->bytes != LP_CFG_BYTES) {
            st->slot_state[st->load_slot] = LP_SLOT_IO_ERR;
        } else {
            uint64_t g = 0u;
            uint32_t rq = 0u, mu = 0u, pb = 0u;
            if (lp_cfg_decode(r->data, LP_CFG_BYTES, &g, &rq, &mu, &pb)) {
                st->slot_gen[st->load_slot] = g;
                st->slot_req[st->load_slot] = rq;
                st->slot_mute[st->load_slot] = mu;
                st->slot_pbk[st->load_slot] = pb;
                st->slot_state[st->load_slot] = LP_SLOT_VALID;
            } else {
                st->slot_state[st->load_slot] = LP_SLOT_CORRUPT;
            }
        }
        lp_advance_load(st);
        return;

    case LP_ST_PUSH_INIT:
        if (r->result == FX_OK) {
            ++st->ipc_set_completed;
            if (r->bytes == sizeof(LivePhotoIpcState)) {
                lp_parse_state_reply(st, r);
            }
        }
        st->ui_dirty_mask = 0u;
        st->stage = LP_ST_UI_BUILD;
        return;

    case LP_ST_UI_BUILD: {
        uint32_t id = st->ui_build_idx + 1u;
        if (r->result != FX_OK) {
            ++st->transient_errors;
            ++st->ui_build_retries;
            if (st->ui_build_retries <= LP_UI_BUILD_MAX_RETRIES) return;
            st->ui_build_retries = 0u;
            st->ui_dirty_mask |= (UINT32_C(1) << id);
        } else {
            st->ui_build_retries = 0u;
            st->ctrl_rev[id] = (uint32_t)r->value[0];
            lp_format_ctrl_text(st, id, st->cached_text[id]);
        }
        ++st->ui_build_idx;
        if (st->ui_build_idx >= LP_UI_CTRL_COUNT) {
            st->ui_built = 1u;
            st->stage = LP_ST_IDLE;
        }
        return;
    }

    case LP_ST_SAVE_WRITE_PATH:
        if (r->result != FX_OK) lp_handle_save_fail(st);
        else st->stage = LP_ST_SAVE_WRITE_DATA;
        return;

    case LP_ST_SAVE_WRITE_DATA:
        if (r->result != FX_OK) {
            lp_handle_save_fail(st);
        } else {
            st->card_fd = -1;
            st->io_done_acc = 0u;
            st->io_calls = 0u;
            st->io_err = 0u;
            st->stage = LP_ST_SAVE_OPEN;
        }
        return;

    case LP_ST_SAVE_OPEN:
        if (r->result != FX_OK) {
            lp_handle_save_fail(st);
        } else {
            int32_t fd = (int32_t)(uint32_t)r->value[0];
            if (fd < 0) {
                lp_handle_save_fail(st);
            } else {
                st->card_fd = fd;
                st->stage = LP_ST_SAVE_WRITE_CALL;
            }
        }
        return;

    case LP_ST_SAVE_WRITE_CALL:
        if (r->result != FX_OK || (uint32_t)r->value[0] != 0u) {
            st->io_err = 1u;
            st->stage = LP_ST_SAVE_CLOSE;
        } else {
            st->stage = LP_ST_SAVE_READ_DONE;
        }
        return;

    case LP_ST_SAVE_READ_DONE:
        if (r->result != FX_OK || r->bytes != 4u) {
            st->io_err = 1u;
            st->stage = LP_ST_SAVE_CLOSE;
        } else {
            uint32_t done = lp_rd_u32(r->data);
            if (done == 0u || done > LP_CFG_BYTES - st->io_done_acc) {
                st->io_err = 1u;
                st->stage = LP_ST_SAVE_CLOSE;
            } else {
                st->io_done_acc += done;
                ++st->io_calls;
                if (st->io_done_acc < LP_CFG_BYTES && st->io_calls < LP_IO_MAX_CALLS) {
                    st->stage = LP_ST_SAVE_WRITE_CALL;
                } else {
                    st->stage = LP_ST_SAVE_CLOSE;
                }
            }
        }
        return;

    case LP_ST_SAVE_CLOSE: {
        uint32_t cerr = (r->result == FX_OK) ? (uint32_t)r->value[0] : 1u;
        st->card_fd = -1;
        if (!st->io_err && cerr == 0u && st->io_done_acc == LP_CFG_BYTES) {
            st->active_slot = st->save_target_slot;
            ++st->saves_completed;
            st->save_retries = 0u;
            st->save_error = 0u;
            if (st->cfg_gen != st->save_gen) {
                st->save_pending = 1u;
            }
            lp_mark_dynamic_dirty(st);
            st->stage = LP_ST_IDLE;
        } else {
            lp_handle_save_fail(st);
        }
        return;
    }

    case LP_ST_SET_CFG:
        if (r->result == FX_OK) {
            ++st->ipc_set_completed;
            if (r->bytes == sizeof(LivePhotoIpcState)) {
                lp_parse_state_reply(st, r);
            } else {
                st->state_refresh_needed = 1u;
            }
        }
        st->stage = LP_ST_IDLE;
        return;

    case LP_ST_FLUSH_LOG:
        if (r->result == FX_OK) {
            ++st->flush_completed;
            st->flush_status = 1u;
            st->state_refresh_needed = 1u;
        } else {
            ++st->flush_failed;
            st->flush_status = 2u;
        }
        lp_mark_dynamic_dirty(st);
        st->stage = LP_ST_IDLE;
        return;

    case LP_ST_STATE_POLL:
        st->last_state_ms = st->now_ms;
        if (r->result == FX_OK && r->bytes == sizeof(LivePhotoIpcState)) {
            ++st->ipc_get_completed;
            lp_parse_state_reply(st, r);
        }
        st->stage = LP_ST_IDLE;
        return;

    case LP_ST_LANG_POLL:
        st->last_lang_ms = st->now_ms;
        if (r->result == FX_OK && r->value[1] == LP_LANG_MAGIC) {
            uint32_t nl = (uint32_t)r->value[0];
            uint32_t old_b = lp_lang_bucket(st->ui_lang);
            uint32_t new_b = lp_lang_bucket(nl);
            st->ui_lang = nl;
            if (new_b != old_b) {
                for (uint32_t id = LP_UI_PAGE_ROOT; id <= LP_UI_CTRL_COUNT; ++id) {
                    st->ui_dirty_mask |= (UINT32_C(1) << id);
                }
                ++st->lang_switches;
            }
        }
        st->stage = LP_ST_IDLE;
        return;

    case LP_ST_UI_UPDATE:
        if (r->result == FX_OK && st->ui_active_ctrl >= LP_UI_PAGE_ROOT &&
            st->ui_active_ctrl <= LP_UI_CTRL_COUNT) {
            st->ctrl_rev[st->ui_active_ctrl] = (uint32_t)r->value[0];
            lp_format_ctrl_text(st, st->ui_active_ctrl, st->cached_text[st->ui_active_ctrl]);
        }
        if (st->ui_active_ctrl <= LP_UI_CTRL_COUNT) {
            st->ui_dirty_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
            st->ui_sync_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
        }
        st->ui_active_ctrl = 0u;
        st->stage = LP_ST_IDLE;
        return;

    case LP_ST_UI_SYNC_GET:
        if (r->result == FX_OK && st->ui_active_ctrl >= LP_UI_PAGE_ROOT &&
            st->ui_active_ctrl <= LP_UI_CTRL_COUNT) {
            st->ctrl_rev[st->ui_active_ctrl] = (uint32_t)r->value[0];
        }
        if (st->ui_active_ctrl <= LP_UI_CTRL_COUNT) {
            st->ui_sync_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
        }
        st->ui_active_ctrl = 0u;
        st->stage = LP_ST_IDLE;
        return;

    case LP_ST_POLL_EVENTS:
        if (r->result == FX_OK) {
            st->idle_polls = 0u;
            lp_handle_ui_event(st, r);
        } else if (r->result == FX_EMPTY) {
            ++st->idle_polls;
            if (st->has_lp_ipc && (st->idle_polls % LP_STATE_IDLE_PERIOD == 0u)) {
                st->state_refresh_needed = 1u;
            }
        }
        st->stage = LP_ST_IDLE;
        return;

    default:
        ++st->transient_errors;
        st->stage = LP_ST_IDLE;
        return;
    }
}

static inline void lp_schedule_idle(LpModuleState *st) {
    if (st->stage != LP_ST_IDLE) return;
    if (st->ipc_sync_needed && st->has_lp_ipc) {
        st->ipc_sync_needed = 0u;
        st->stage = LP_ST_SET_CFG;
        return;
    }
    if (st->save_pending) {
        if (!st->has_bounce || !st->card_write_ok) {
            st->save_pending = 0u;
            st->save_error = 1u;
            lp_mark_dynamic_dirty(st);
        } else if (st->save_retries == 0u ||
                   st->now_ms < st->save_fail_ms ||
                   (st->now_ms - st->save_fail_ms) >= LP_SAVE_RETRY_MS) {
            st->save_pending = 0u;
            st->save_target_slot = st->active_slot ^ 1u;
            st->save_gen = st->cfg_gen;
            lp_cfg_encode(st->save_raw, st->save_gen,
                          st->cfg_requested, st->cfg_mute, st->cfg_pb_key_cfg);
            st->stage = LP_ST_SAVE_WRITE_PATH;
            return;
        }
    }
    if (st->flush_log_pending && st->has_lp_ipc) {
        st->flush_log_pending = 0u;
        st->stage = LP_ST_FLUSH_LOG;
        return;
    }
    if (st->has_lp_ipc &&
        (st->state_refresh_needed ||
         (st->focus_foreground && st->now_ms >= st->last_state_ms &&
          (st->now_ms - st->last_state_ms) >= LP_STATE_POLL_MS))) {
        st->state_refresh_needed = 0u;
        st->stage = LP_ST_STATE_POLL;
        return;
    }
    if (st->ui_dirty_mask != 0u) {
        uint32_t id = lp_lowest_ctrl_bit(st->ui_dirty_mask);
        if (id != 0u) {
            st->ui_active_ctrl = id;
            st->stage = LP_ST_UI_UPDATE;
            return;
        }
        st->ui_dirty_mask = 0u;
    }
    if (st->ui_sync_mask != 0u) {
        uint32_t id = lp_lowest_ctrl_bit(st->ui_sync_mask);
        if (id != 0u) {
            st->ui_active_ctrl = id;
            st->stage = LP_ST_UI_SYNC_GET;
            return;
        }
        st->ui_sync_mask = 0u;
    }
    if (st->has_lang_svc &&
        (st->lang_refresh_needed ||
         (st->now_ms >= st->last_lang_ms &&
          (st->now_ms - st->last_lang_ms) >= LP_LANG_POLL_MS))) {
        st->lang_refresh_needed = 0u;
        st->stage = LP_ST_LANG_POLL;
        return;
    }
    st->stage = LP_ST_POLL_EVENTS;
}

static inline int lp_module_step(LpModuleState *st, const FxModuleServices *base) {
    if (!st || !base || base->abi != FX_ABI || base->bytes < sizeof(FxServiceModule)) {
        return 1;
    }
    const FxServiceModule *api = (const FxServiceModule *)base;
    if (api->service_abi != FX_SERVICE_ABI || !api->exchange) {
        return 2;
    }
    st->now_ms = api->time_ms;

    if (st->sdk.pending) {
        FxServiceReply reply;
        FxResult taken = fx_sdk_take(&st->sdk, api, &reply);
        if (taken == FX_BUSY) return 0;
        if (taken != FX_OK) {
            lp_recover(st);
            return 0;
        }
        lp_handle_reply(st, &reply);
    }

    lp_schedule_idle(st);

    FxResult res = FX_ARGUMENT;
    switch (st->stage) {
    case LP_ST_QUERY_UI:
        res = fx_sdk_ui_query(&st->sdk, api, 0u);
        break;
    case LP_ST_OPEN_SLOTS:
        res = fx_sdk_open(&st->sdk, api, LP_SLOTS_BINDING_NAME, LP_SLOTS_BINDING_VER);
        break;
    case LP_ST_LANG_INIT:
    case LP_ST_LANG_POLL: {
        LpSlotsRequest q = { 0u, 0u, 0u, 0u };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->slots_binding,
                              LP_SLOTS_LANG_GET, 0u, &q, (uint32_t)sizeof(q));
        break;
    }
    case LP_ST_STATE_INIT:
    case LP_ST_STATE_POLL:
        res = lp_submit_ipc(st, api, LP_OP_GET_STATE, 0, 0u);
        break;
    case LP_ST_OPEN_MEM:
        res = fx_sdk_open(&st->sdk, api, "core.memory", 1u);
        break;
    case LP_ST_ALLOC_MEM:
        res = fx_sdk_memory_allocate(&st->sdk, api, st->mem_binding, LP_BOUNCE_BYTES, 1u);
        break;
    case LP_ST_OPEN_CARD_OPEN:
        res = fx_sdk_open(&st->sdk, api, "camera.card.open", 1u);
        break;
    case LP_ST_OPEN_CARD_READ:
        res = fx_sdk_open(&st->sdk, api, "camera.card.read", 1u);
        break;
    case LP_ST_OPEN_CARD_WRITE:
        res = fx_sdk_open(&st->sdk, api, "camera.card.write", 1u);
        break;
    case LP_ST_OPEN_CARD_CLOSE:
        res = fx_sdk_open(&st->sdk, api, "camera.card.close", 1u);
        break;
    case LP_ST_LOAD_WRITE_PATH: {
        char path[32];
        lp_cfg_path(lp_effective_drive(st), st->load_slot, path);
        res = lp_submit_mem_write(st, api, LP_BOUNCE_PATH_OFF, path, 32u);
        break;
    }
    case LP_ST_LOAD_OPEN: {
        uint64_t args[8] = { st->buf_native + LP_BOUNCE_PATH_OFF, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_open_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case LP_ST_LOAD_READ_CALL: {
        uint64_t args[8] = {
            (uint64_t)(uint32_t)st->card_fd,
            st->buf_native + LP_BOUNCE_DATA_OFF + st->io_done_acc,
            LP_CFG_READ_MAX - st->io_done_acc,
            st->buf_native + LP_BOUNCE_DONE_OFF,
            0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_read_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case LP_ST_LOAD_READ_DONE:
    case LP_ST_SAVE_READ_DONE:
        res = lp_submit_mem_read(st, api, LP_BOUNCE_DONE_OFF, 4u);
        break;
    case LP_ST_LOAD_CLOSE:
    case LP_ST_SAVE_CLOSE: {
        uint64_t args[8] = { (uint64_t)(uint32_t)st->card_fd, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_close_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case LP_ST_LOAD_READ_DATA:
        res = lp_submit_mem_read(st, api, LP_BOUNCE_DATA_OFF, LP_CFG_BYTES);
        break;
    case LP_ST_PUSH_INIT:
    case LP_ST_SET_CFG: {
        LivePhotoIpcSetCfg cfg;
        lp_zero(&cfg, (uint32_t)sizeof(cfg));
        cfg.magic = LP_SLOTS_TUNNEL_MAGIC;
        cfg.requested = st->cfg_requested ? 1u : 0u;
        cfg.mute = st->cfg_mute ? 1u : 0u;
        cfg.pb_key = lp_pb_key_from_cfg(st->cfg_pb_key_cfg);
        cfg.apply_mask = LP_APPLY_ALL;
        res = lp_submit_ipc(st, api, LP_OP_SET_CFG, &cfg, (uint32_t)sizeof(cfg));
        break;
    }
    case LP_ST_FLUSH_LOG:
        res = lp_submit_ipc(st, api, LP_OP_FLUSH_LOG, 0, 0u);
        break;
    case LP_ST_UI_BUILD: {
        FxUiControl ctrl;
        if (!lp_build_ui_control(st->ui_build_idx + 1u, st, &ctrl)) {
            lp_recover(st);
            return 0;
        }
        res = fx_sdk_ui(&st->sdk, api, &ctrl);
        break;
    }
    case LP_ST_SAVE_WRITE_PATH: {
        char path[32];
        lp_cfg_path(lp_effective_drive(st), st->save_target_slot, path);
        res = lp_submit_mem_write(st, api, LP_BOUNCE_PATH_OFF, path, 32u);
        break;
    }
    case LP_ST_SAVE_WRITE_DATA:
        res = lp_submit_mem_write(st, api, LP_BOUNCE_DATA_OFF, st->save_raw, LP_CFG_BYTES);
        break;
    case LP_ST_SAVE_OPEN: {
        uint64_t args[8] = {
            st->buf_native + LP_BOUNCE_PATH_OFF,
            UINT64_C(0x301), /* write + create + truncate */
            0u, 0u, 0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_open_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case LP_ST_SAVE_WRITE_CALL: {
        uint64_t args[8] = {
            (uint64_t)(uint32_t)st->card_fd,
            st->buf_native + LP_BOUNCE_DATA_OFF + st->io_done_acc,
            LP_CFG_BYTES - st->io_done_acc,
            st->buf_native + LP_BOUNCE_DONE_OFF,
            0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_write_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case LP_ST_UI_UPDATE: {
        FxUiControl ctrl;
        if (!lp_build_ui_control(st->ui_active_ctrl, st, &ctrl)) {
            lp_recover(st);
            return 0;
        }
        res = fx_sdk_ui(&st->sdk, api, &ctrl);
        break;
    }
    case LP_ST_UI_SYNC_GET:
        res = fx_sdk_ui_get(&st->sdk, api, st->ui_active_ctrl);
        break;
    case LP_ST_POLL_EVENTS:
        res = fx_sdk_events(&st->sdk, api);
        break;
    default:
        lp_recover(st);
        return 0;
    }

    if (res == FX_BUSY) return 0;
    if (res != FX_OK) {
        lp_recover(st);
        return 0;
    }
    return 0;
}

#endif /* LIVEPHOTO_CORE_H */
