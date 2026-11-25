/* SPDX-License-Identifier: MIT
 * lut.wlm Core State Machine (V71.7-r4).
 *
 * - 13 inline UI controls (<= FX_UI_CONTROLS 16) with native Debug Settings sub-page:
 *     1: PAGE_ROOT       - 3D LUT / 3D LUT 调色 / 3D LUT 調色
 *     2: SWITCH (p=1)    - 3D LUT Enable (default On)
 *     3: ACTION (p=1)    - Active LUT (<filename.cube> or None)
 *     4: SWITCH (p=1)    - Live View Preview (gates Layer 0 LUT)
 *     5: SWITCH (p=1)    - Still JPEG LUT (gates Stage 0 Still)
 *     6: SWITCH (p=1)    - Live Photo LUT (gates Stage 0 Live)
 *     7: ACTION (p=1)    - Clear Active LUT (LUT_OP_CLEAR_LUT)
 *     8: PAGE   (p=1)    - Debug Settings / 调试设置 / 調試設定
 *     9: ACTION (p=8)    - Pipeline & Accel
 *    10: ACTION (p=8)    - Layer 0 Preview (WS/fps/Sub/Rej)
 *    11: ACTION (p=8)    - Health & Drive (NEON/Q/Pool [A|B])
 *    12: ACTION (p=8)    - Watchdog (WD/Phase/Err)
 *    13: ACTION (p=8)    - Reset Preview Pool (LUT_OP_RESET_PREVIEW)
 * - Dual-slot CRC32 persistence: <active_drive>:\FENESTRA\FXLUT0.CFG / FXLUT1.CFG
 *   (32B 'ZLT1': master_on, preview_on, still_on, live_on, default all 1).
 * - EL1 IPC tunnel over slots.state ops 65..68 (magic 'LTI1').
 * - EN / zh-CN (8) / zh-TW (9) localization via slots.state op 15.
 * - A2 Hardening: never returns non-zero on transient IPC/UI errors.
 * - Freestanding C11, zero libc, zero relocations, zero FP/SIMD.
 */
#ifndef LUT_CORE_H
#define LUT_CORE_H

#include "fenestra_sdk.h"
#include "fenestra_file_sdk.h"
#include "lut_wlm_abi.h"

#define LUT_BOUNCE_BYTES         1024u
#define LUT_BOUNCE_PATH_OFF      0u
#define LUT_BOUNCE_DONE_OFF      64u
#define LUT_BOUNCE_DATA_OFF      128u

#define LUT_OPEN_NOT_FOUND       UINT32_C(0x80000010)
#define LUT_IO_MAX_CALLS         32u
#define LUT_LOAD_MAX_RETRIES     6u
#define LUT_SAVE_MAX_RETRIES     5u
#define LUT_SAVE_RETRY_MS        500u
#define LUT_LANG_POLL_MS         1000u
#define LUT_STATE_POLL_MS        500u
#define LUT_STATE_IDLE_PERIOD    16u
#define LUT_UI_BUILD_MAX_RETRIES 8u

typedef struct {
    uint32_t index, offset, bytes, value;
} LutSlotsRequest;

enum LutUiControlId {
    LUT_UI_PAGE_ROOT      = 1u,
    LUT_UI_MASTER_ON      = 2u,
    LUT_UI_ACTIVE_LUT     = 3u,
    LUT_UI_PREVIEW_ON     = 4u,
    LUT_UI_STILL_ON       = 5u,
    LUT_UI_LIVE_ON        = 6u,
    LUT_UI_CLEAR_LUT      = 7u,
    LUT_UI_DEBUG_PAGE     = 8u,
    LUT_UI_DBG_PIPE       = 9u,
    LUT_UI_DBG_L0         = 10u,
    LUT_UI_DBG_HEALTH     = 11u,
    LUT_UI_DBG_WD         = 12u,
    LUT_UI_DBG_RESET_POOL = 13u,
    LUT_UI_CTRL_COUNT     = 13u
};

_Static_assert(LUT_UI_CTRL_COUNT <= FX_UI_CONTROLS,
               "lut.wlm controls must fit within inline FX_UI_CONTROLS");

enum LutSlotStatus {
    LUT_SLOT_UNREAD  = 0u,
    LUT_SLOT_MISSING = 1u,
    LUT_SLOT_CORRUPT = 2u,
    LUT_SLOT_IO_ERR  = 3u,
    LUT_SLOT_VALID   = 4u
};

enum LutStage {
    LUT_ST_QUERY_UI = 0u,
    LUT_ST_OPEN_SLOTS,
    LUT_ST_LANG_INIT,
    LUT_ST_STATE_INIT,
    LUT_ST_OPEN_MEM,
    LUT_ST_ALLOC_MEM,
    LUT_ST_OPEN_CARD_OPEN,
    LUT_ST_OPEN_CARD_READ,
    LUT_ST_OPEN_CARD_WRITE,
    LUT_ST_OPEN_CARD_CLOSE,
    LUT_ST_LOAD_WRITE_PATH,
    LUT_ST_LOAD_OPEN,
    LUT_ST_LOAD_READ_CALL,
    LUT_ST_LOAD_READ_DONE,
    LUT_ST_LOAD_CLOSE,
    LUT_ST_LOAD_READ_DATA,
    LUT_ST_PUSH_INIT,
    LUT_ST_STATE_POST_INIT,
    LUT_ST_UI_BUILD,
    LUT_ST_IDLE,
    LUT_ST_SAVE_WRITE_PATH,
    LUT_ST_SAVE_WRITE_DATA,
    LUT_ST_SAVE_OPEN,
    LUT_ST_SAVE_WRITE_CALL,
    LUT_ST_SAVE_READ_DONE,
    LUT_ST_SAVE_CLOSE,
    LUT_ST_SET_CFG,
    LUT_ST_CLEAR_LUT,
    LUT_ST_RESET_PREVIEW,
    LUT_ST_STATE_POLL,
    LUT_ST_LANG_POLL,
    LUT_ST_UI_UPDATE,
    LUT_ST_UI_SYNC_GET,
    LUT_ST_POLL_EVENTS
};

typedef struct {
    FxSdkClient sdk;
    uint32_t stage;

    /* Capabilities & bindings */
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
    uint32_t ui_lang;

    uint8_t has_slots;
    uint8_t has_lang_svc;
    uint8_t has_lut_ipc;
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
    uint8_t clear_lut_pending;
    uint8_t reset_pool_pending;
    uint8_t reset_status; /* 0=idle, 1=ok, 2=quarantined/busy, 3=err */
    char    active_drive;

    /* EL1 telemetry snapshot */
    V71r7LutIpcState el1_state;

    /* Local config & dual-slot FXLUT0/1.CFG persistence */
    V71r7LutWlmCfg cfg;
    V71r7LutWlmCfg slot_cfg[2];
    uint8_t        slot_state[2];
    uint32_t       active_slot;
    uint32_t       load_slot;
    uint32_t       load_retries;

    /* Card I/O scratch */
    int32_t        card_fd;
    uint32_t       io_done_acc;
    uint32_t       io_calls;
    uint32_t       io_err;
    uint32_t       save_target_slot;
    V71r7LutWlmCfg save_cfg;
    uint64_t       save_fail_ms;
    uint8_t        save_pending;
    uint8_t        save_retries;

    /* IPC TX scratch */
    uint8_t ipc_tx_buf[LUT_SLOTS_HDR_BYTES + sizeof(V71r7LutIpcSetCfg)];

    /* UI state */
    uint32_t ui_build_idx;
    uint32_t ctrl_rev[LUT_UI_CTRL_COUNT + 1u];
    uint32_t ui_dirty_mask;
    uint32_t ui_sync_mask;
    uint32_t ui_active_ctrl;
    uint32_t idle_polls;
    char cached_text[LUT_UI_CTRL_COUNT + 1u][FX_UI_TEXT];

    /* Observability / test counters */
    uint32_t saves_completed;
    uint32_t saves_failed;
    uint32_t ipc_set_completed;
    uint32_t ipc_get_completed;
    uint32_t clears_completed;
    uint32_t resets_completed;
    uint32_t resets_busy;
    uint32_t lang_switches;
    uint32_t transient_errors;
} LutModuleState;

static inline void lut_zero(void *dst, uint32_t n) {
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t i = 0u; i < n; ++i) p[i] = 0u;
}

static inline void lut_copy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0u; i < n; ++i) d[i] = s[i];
}

static inline int lut_mem_eq(const void *a, const void *b, uint32_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (uint32_t i = 0u; i < n; ++i) {
        if (pa[i] != pb[i]) return 0;
    }
    return 1;
}

static inline uint32_t lut_rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static inline uint32_t lut_utf8_len(const char *s) {
    uint8_t b0 = (uint8_t)s[0];
    if (b0 < 0x80u) return 1u;
    if ((b0 & 0xe0u) == 0xc0u) return s[1] ? 2u : 1u;
    if ((b0 & 0xf0u) == 0xe0u) return (s[1] && s[2]) ? 3u : 1u;
    if ((b0 & 0xf8u) == 0xf0u) return (s[1] && s[2] && s[3]) ? 4u : 1u;
    return 1u;
}

static inline uint32_t lut_lang_bucket(uint32_t lang) {
    if (lang == LUT_LANG_ZH_CN) return 1u;
    if (lang == LUT_LANG_ZH_TW) return 2u;
    return 0u;
}

static inline const char *lut_tr(uint32_t lang, const char *en, const char *sc, const char *tc) {
    if (lang == LUT_LANG_ZH_CN) return sc;
    if (lang == LUT_LANG_ZH_TW) return tc;
    return en;
}

static __attribute__((noinline)) void lut_str_copy(char *dst, uint32_t cap, const char *src) {
    if (!cap) return;
    uint32_t pos = 0u;
    uint32_t max_b = cap - 1u;
    while (src && src[pos]) {
        uint32_t clen = lut_utf8_len(src + pos);
        if (pos + clen > max_b) break;
        for (uint32_t k = 0u; k < clen; ++k) {
            dst[pos] = src[pos];
            ++pos;
        }
    }
    while (pos < cap) dst[pos++] = '\0';
}

static __attribute__((noinline)) void lut_append_str(char *dst, uint32_t cap, uint32_t *pos, const char *src) {
    if (!dst || !pos || !src || !cap) return;
    uint32_t max_b = cap - 1u;
    uint32_t i = 0u;
    while (src[i]) {
        uint32_t clen = lut_utf8_len(src + i);
        if (*pos + clen > max_b) break;
        for (uint32_t k = 0u; k < clen; ++k) dst[(*pos)++] = src[i++];
    }
    dst[*pos] = '\0';
}

static __attribute__((noinline)) void lut_append_u32(char *dst, uint32_t cap, uint32_t *pos, uint32_t v) {
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

static __attribute__((noinline)) void lut_append_i32(char *dst, uint32_t cap, uint32_t *pos, int32_t v) {
    if (v < 0) {
        lut_append_str(dst, cap, pos, "-");
        uint32_t mag = (uint32_t)(-(v + 1)) + 1u;
        lut_append_u32(dst, cap, pos, mag);
    } else {
        lut_append_u32(dst, cap, pos, (uint32_t)v);
    }
}

static inline char lut_effective_drive(const LutModuleState *st) {
    return (st && st->active_drive == 'B') ? 'B' : 'A';
}

static inline void lut_cfg_path(char drive, uint32_t slot, char out[32]) {
    lut_zero(out, 32u);
    lut_str_copy(out, 32u, slot == 0u ? "A:\\FENESTRA\\FXLUT0.CFG" : "A:\\FENESTRA\\FXLUT1.CFG");
    out[0] = (drive == 'B') ? 'B' : 'A';
}

static inline const char *lut_basename(const char path[64]) {
    const char *base = path;
    for (uint32_t i = 0u; i < 63u && path[i]; ++i) {
        if (path[i] == '\\' || path[i] == '/') {
            base = path + i + 1u;
        }
    }
    return base;
}

static __attribute__((noinline)) void lut_format_active_lut(const LutModuleState *st, char out[FX_UI_TEXT]) {
    lut_zero(out, FX_UI_TEXT);
    uint32_t lang = st->ui_lang;
    if (st->state_valid && st->el1_state.active_path[0] != '\0') {
        char safe[64];
        lut_copy(safe, st->el1_state.active_path, 64u);
        safe[63] = '\0';
        const char *name = lut_basename(safe);
        if (name[0] != '\0') {
            lut_str_copy(out, FX_UI_TEXT, name);
            return;
        }
    }
    lut_str_copy(out, FX_UI_TEXT,
                 lut_tr(lang,
                        "None (Select in Photo Menu)",
                        "无 LUT (在照片菜单选择)",
                        "無 LUT (在照片選單選擇)"));
}

static __attribute__((noinline)) void lut_format_clear_lut(const LutModuleState *st, char out[FX_UI_TEXT]) {
    lut_zero(out, FX_UI_TEXT);
    uint32_t lang = st->ui_lang;
    if (!st->has_lut_ipc) {
        lut_str_copy(out, FX_UI_TEXT, lut_tr(lang, "No EL1", "无 EL1", "無 EL1"));
    } else if (st->state_valid && st->el1_state.active_path[0] != '\0') {
        lut_str_copy(out, FX_UI_TEXT, lut_tr(lang, "Tap to Clear", "按此清除", "按此清除"));
    } else {
        lut_str_copy(out, FX_UI_TEXT, lut_tr(lang, "None", "无", "無"));
    }
}

static __attribute__((noinline)) void lut_format_dbg_pipe(const LutModuleState *st, char out[FX_UI_TEXT]) {
    lut_zero(out, FX_UI_TEXT);
    uint32_t pos = 0u;
    if (!st->state_valid) {
        lut_append_str(out, FX_UI_TEXT, &pos, "No EL1");
        return;
    }
    const V71r7LutIpcState *s = &st->el1_state;
    if (!s->master_on) {
        lut_append_str(out, FX_UI_TEXT, &pos, "OFF");
    } else if (!s->lut_active) {
        lut_append_str(out, FX_UI_TEXT, &pos, "IDLE");
    } else if (s->is_1d_separable) {
        lut_append_str(out, FX_UI_TEXT, &pos, "1D-XM6");
    } else if (s->neon_allowed) {
        lut_append_str(out, FX_UI_TEXT, &pos, "3D-NEON");
    } else {
        lut_append_str(out, FX_UI_TEXT, &pos, "3D-CPU");
    }
    lut_append_str(out, FX_UI_TEXT, &pos, " C");
    lut_append_u32(out, FX_UI_TEXT, &pos, (uint32_t)s->smp_helpers_online + 1u);
    lut_append_str(out, FX_UI_TEXT, &pos, " Gen:");
    lut_append_u32(out, FX_UI_TEXT, &pos, s->generation);
}

static __attribute__((noinline)) void lut_format_dbg_l0(const LutModuleState *st, char out[FX_UI_TEXT]) {
    lut_zero(out, FX_UI_TEXT);
    uint32_t pos = 0u;
    if (!st->state_valid) {
        lut_append_str(out, FX_UI_TEXT, &pos, "No EL1");
        return;
    }
    const V71r7LutIpcState *s = &st->el1_state;
    uint32_t ws_mb = (s->ws_bytes + UINT32_C(524288)) / UINT32_C(1048576);
    uint32_t fps = (s->fps_milli + 500u) / 1000u;
    lut_append_str(out, FX_UI_TEXT, &pos, "WS:");
    lut_append_u32(out, FX_UI_TEXT, &pos, ws_mb);
    lut_append_str(out, FX_UI_TEXT, &pos, "M ");
    lut_append_u32(out, FX_UI_TEXT, &pos, fps);
    lut_append_str(out, FX_UI_TEXT, &pos, "fps Sub:");
    lut_append_u32(out, FX_UI_TEXT, &pos, s->disp_sub);
    lut_append_str(out, FX_UI_TEXT, &pos, " REJ:");
    lut_append_u32(out, FX_UI_TEXT, &pos, s->disp_rejects);
}

static __attribute__((noinline)) void lut_format_dbg_health(const LutModuleState *st, char out[FX_UI_TEXT]) {
    lut_zero(out, FX_UI_TEXT);
    uint32_t pos = 0u;
    char drv = lut_effective_drive(st);
    if (!st->state_valid) {
        lut_append_str(out, FX_UI_TEXT, &pos, drv == 'B' ? "No EL1 [B]" : "No EL1 [A]");
        return;
    }
    const V71r7LutIpcState *s = &st->el1_state;
    lut_append_str(out, FX_UI_TEXT, &pos, "NEON:");
    lut_append_u32(out, FX_UI_TEXT, &pos, s->neon_allowed ? 1u : 0u);
    lut_append_str(out, FX_UI_TEXT, &pos, " Q:");
    lut_append_u32(out, FX_UI_TEXT, &pos, s->quarantined ? 1u : 0u);
    lut_append_str(out, FX_UI_TEXT, &pos, " Pool:");
    lut_append_i32(out, FX_UI_TEXT, &pos, s->pool_result);
    lut_append_str(out, FX_UI_TEXT, &pos, drv == 'B' ? " [B]" : " [A]");
}

static __attribute__((noinline)) void lut_format_dbg_wd(const LutModuleState *st, char out[FX_UI_TEXT]) {
    lut_zero(out, FX_UI_TEXT);
    uint32_t pos = 0u;
    if (!st->state_valid) {
        lut_append_str(out, FX_UI_TEXT, &pos, "No EL1");
        return;
    }
    const V71r7LutIpcState *s = &st->el1_state;
    lut_append_str(out, FX_UI_TEXT, &pos, "WD:");
    lut_append_u32(out, FX_UI_TEXT, &pos, s->wd_polls);
    lut_append_str(out, FX_UI_TEXT, &pos, " Phase:");
    lut_append_u32(out, FX_UI_TEXT, &pos, s->worker_phase);
    lut_append_str(out, FX_UI_TEXT, &pos, " Err:");
    lut_append_i32(out, FX_UI_TEXT, &pos, s->last_result);
}

static __attribute__((noinline)) void lut_format_dbg_reset(const LutModuleState *st, char out[FX_UI_TEXT]) {
    lut_zero(out, FX_UI_TEXT);
    uint32_t lang = st->ui_lang;
    if (!st->has_lut_ipc) {
        lut_str_copy(out, FX_UI_TEXT, lut_tr(lang, "No EL1", "无 EL1", "無 EL1"));
    } else if (st->reset_status == 1u) {
        lut_str_copy(out, FX_UI_TEXT, lut_tr(lang, "Pool Reset OK", "重置完成", "重設完成"));
    } else if (st->reset_status == 2u) {
        lut_str_copy(out, FX_UI_TEXT, lut_tr(lang, "Busy (Quarantined)", "已隔离 不可重置", "已隔離 不可重設"));
    } else if (st->reset_status == 3u) {
        lut_str_copy(out, FX_UI_TEXT, lut_tr(lang, "Reset Error", "重置失败", "重設失敗"));
    } else {
        lut_str_copy(out, FX_UI_TEXT, lut_tr(lang, "Tap to Reset", "按此重置", "按此重設"));
    }
}

static __attribute__((noinline)) void lut_format_ctrl_text(const LutModuleState *st, uint32_t id, char out[FX_UI_TEXT]) {
    switch (id) {
    case LUT_UI_ACTIVE_LUT:     lut_format_active_lut(st, out); break;
    case LUT_UI_CLEAR_LUT:      lut_format_clear_lut(st, out); break;
    case LUT_UI_DBG_PIPE:       lut_format_dbg_pipe(st, out); break;
    case LUT_UI_DBG_L0:         lut_format_dbg_l0(st, out); break;
    case LUT_UI_DBG_HEALTH:     lut_format_dbg_health(st, out); break;
    case LUT_UI_DBG_WD:         lut_format_dbg_wd(st, out); break;
    case LUT_UI_DBG_RESET_POOL: lut_format_dbg_reset(st, out); break;
    default:                    lut_zero(out, FX_UI_TEXT); break;
    }
}

static inline void lut_mark_dynamic_dirty(LutModuleState *st) {
    char tmp[FX_UI_TEXT];
    for (uint32_t id = LUT_UI_ACTIVE_LUT; id <= LUT_UI_DBG_RESET_POOL; ++id) {
        if (id == LUT_UI_PREVIEW_ON || id == LUT_UI_STILL_ON ||
            id == LUT_UI_LIVE_ON || id == LUT_UI_DEBUG_PAGE) {
            continue;
        }
        lut_format_ctrl_text(st, id, tmp);
        if (!lut_mem_eq(tmp, st->cached_text[id], FX_UI_TEXT)) {
            st->ui_dirty_mask |= (UINT32_C(1) << id);
        }
    }
}

static __attribute__((noinline)) int lut_build_ui_control(uint32_t id,
                                                          const LutModuleState *st,
                                                          FxUiControl *out) {
    if (!st || !out || id < LUT_UI_PAGE_ROOT || id > LUT_UI_CTRL_COUNT) {
        return 0;
    }
    uint32_t lang = st->ui_lang;
    lut_zero(out, (uint32_t)sizeof(*out));
    out->abi = FX_PLATFORM_ABI;
    out->bytes = (uint32_t)sizeof(*out);
    out->id = id;
    out->flags = FX_UI_VISIBLE | FX_UI_ENABLED;

    switch (id) {
    case LUT_UI_PAGE_ROOT:
        out->parent = 0u;
        out->kind = FX_UI_PAGE;
        if (st->ui_caps & FX_UI_CAP_ROOT) {
            out->flags |= FX_UI_ROOT;
        }
        lut_str_copy(out->label, FX_UI_TEXT,
                     lut_tr(lang, "3D LUT", "3D LUT 调色", "3D LUT 調色"));
        return 1;

    case LUT_UI_MASTER_ON:
        out->parent = LUT_UI_PAGE_ROOT;
        out->kind = FX_UI_SWITCH;
        out->value = st->cfg.master_on ? 1u : 0u;
        lut_str_copy(out->label, FX_UI_TEXT,
                     lut_tr(lang, "3D LUT Enable", "3D LUT 开关", "3D LUT 開關"));
        return 1;

    case LUT_UI_ACTIVE_LUT:
        out->parent = LUT_UI_PAGE_ROOT;
        out->kind = FX_UI_ACTION;
        lut_str_copy(out->label, FX_UI_TEXT,
                     lut_tr(lang, "Active LUT", "当前 LUT", "目前 LUT"));
        lut_format_active_lut(st, out->text);
        return 1;

    case LUT_UI_PREVIEW_ON:
        out->parent = LUT_UI_PAGE_ROOT;
        out->kind = FX_UI_SWITCH;
        out->value = st->cfg.preview_on ? 1u : 0u;
        lut_str_copy(out->label, FX_UI_TEXT,
                     lut_tr(lang, "Live View Preview", "取景实时预览", "取景即時預覽"));
        return 1;

    case LUT_UI_STILL_ON:
        out->parent = LUT_UI_PAGE_ROOT;
        out->kind = FX_UI_SWITCH;
        out->value = st->cfg.still_on ? 1u : 0u;
        lut_str_copy(out->label, FX_UI_TEXT,
                     lut_tr(lang, "Still JPEG LUT", "照片 LUT", "照片 LUT"));
        return 1;

    case LUT_UI_LIVE_ON:
        out->parent = LUT_UI_PAGE_ROOT;
        out->kind = FX_UI_SWITCH;
        out->value = st->cfg.live_on ? 1u : 0u;
        lut_str_copy(out->label, FX_UI_TEXT,
                     lut_tr(lang, "Live Photo LUT", "实况照片 LUT", "實況照片 LUT"));
        return 1;

    case LUT_UI_CLEAR_LUT:
        out->parent = LUT_UI_PAGE_ROOT;
        out->kind = FX_UI_ACTION;
        lut_str_copy(out->label, FX_UI_TEXT,
                     lut_tr(lang, "Clear Active LUT", "清除当前 LUT", "清除目前 LUT"));
        lut_format_clear_lut(st, out->text);
        return 1;

    case LUT_UI_DEBUG_PAGE:
        out->parent = LUT_UI_PAGE_ROOT;
        out->kind = FX_UI_PAGE;
        lut_str_copy(out->label, FX_UI_TEXT,
                     lut_tr(lang, "Debug Settings", "调试设置", "調試設定"));
        return 1;

    case LUT_UI_DBG_PIPE:
        out->parent = LUT_UI_DEBUG_PAGE;
        out->kind = FX_UI_ACTION;
        lut_str_copy(out->label, FX_UI_TEXT,
                     lut_tr(lang, "Pipeline & Accel", "管线与加速", "管線與加速"));
        lut_format_dbg_pipe(st, out->text);
        return 1;

    case LUT_UI_DBG_L0:
        out->parent = LUT_UI_DEBUG_PAGE;
        out->kind = FX_UI_ACTION;
        lut_str_copy(out->label, FX_UI_TEXT,
                     lut_tr(lang, "Layer 0 Preview", "图层 0 预览", "圖層 0 預覽"));
        lut_format_dbg_l0(st, out->text);
        return 1;

    case LUT_UI_DBG_HEALTH:
        out->parent = LUT_UI_DEBUG_PAGE;
        out->kind = FX_UI_ACTION;
        lut_str_copy(out->label, FX_UI_TEXT,
                     lut_tr(lang, "Health & Drive", "健康与隔离", "健康與隔離"));
        lut_format_dbg_health(st, out->text);
        return 1;

    case LUT_UI_DBG_WD:
        out->parent = LUT_UI_DEBUG_PAGE;
        out->kind = FX_UI_ACTION;
        lut_str_copy(out->label, FX_UI_TEXT,
                     lut_tr(lang, "Watchdog", "看门狗状态", "看門狗狀態"));
        lut_format_dbg_wd(st, out->text);
        return 1;

    case LUT_UI_DBG_RESET_POOL:
        out->parent = LUT_UI_DEBUG_PAGE;
        out->kind = FX_UI_ACTION;
        lut_str_copy(out->label, FX_UI_TEXT,
                     lut_tr(lang, "Reset Preview Pool", "重置预览工作区", "重設預覽工作區"));
        lut_format_dbg_reset(st, out->text);
        return 1;

    default:
        return 0;
    }
}

static inline FxResult lut_submit_mem_write(LutModuleState *st, const FxServiceModule *api,
                                            uint32_t offset, const void *data, uint32_t bytes) {
    uint8_t payload[sizeof(FxMemoryRequest) + LUT_CFG_BYTES];
    if (bytes > LUT_CFG_BYTES) return FX_BOUNDS;
    FxMemoryRequest req = { .handle = st->buf_handle, .bytes = bytes, .offset = offset };
    lut_copy(payload, &req, (uint32_t)sizeof(req));
    lut_copy(payload + sizeof(req), data, bytes);
    return fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->mem_binding,
                           FX_MEMORY_WRITE, 0u, payload, (uint32_t)sizeof(req) + bytes);
}

static inline FxResult lut_submit_mem_read(LutModuleState *st, const FxServiceModule *api,
                                           uint32_t offset, uint32_t bytes) {
    FxMemoryRequest req = { .handle = st->buf_handle, .bytes = bytes, .offset = offset };
    return fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->mem_binding,
                           FX_MEMORY_READ, 0u, &req, (uint32_t)sizeof(req));
}

static inline FxResult lut_submit_ipc(LutModuleState *st, const FxServiceModule *api,
                                      uint32_t raw_op, const void *payload, uint32_t payload_bytes) {
    if (payload_bytes > sizeof(V71r7LutIpcSetCfg)) return FX_ARGUMENT;
    uint32_t hdr[4] = { LUT_SLOTS_TUNNEL_MAGIC, payload_bytes, 0u, 0u };
    lut_copy(st->ipc_tx_buf, hdr, LUT_SLOTS_HDR_BYTES);
    if (payload && payload_bytes) {
        lut_copy(st->ipc_tx_buf + LUT_SLOTS_HDR_BYTES, payload, payload_bytes);
    }
    return fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->slots_binding,
                           raw_op, 0u, st->ipc_tx_buf, LUT_SLOTS_HDR_BYTES + payload_bytes);
}

static inline void lut_parse_state_reply(LutModuleState *st, const FxServiceReply *r) {
    if (r->result != FX_OK || r->bytes != sizeof(V71r7LutIpcState)) return;
    V71r7LutIpcState tmp;
    lut_copy(&tmp, r->data, (uint32_t)sizeof(tmp));
    if (tmp.magic != LUT_SLOTS_TUNNEL_MAGIC || tmp.bytes != sizeof(V71r7LutIpcState)) return;
    tmp.active_path[63] = '\0';
    st->el1_state = tmp;
    st->state_valid = 1u;
    st->has_lut_ipc = 1u;
    if (tmp.active_drive == 'A' || tmp.active_drive == 'B') {
        st->active_drive = (char)tmp.active_drive;
    }
    lut_mark_dynamic_dirty(st);
}

static inline void lut_start_after_card_open(LutModuleState *st) {
    if (st->card_read_ok) {
        st->load_slot = 0u;
        st->slot_state[0] = LUT_SLOT_UNREAD;
        st->slot_state[1] = LUT_SLOT_UNREAD;
        st->stage = LUT_ST_LOAD_WRITE_PATH;
    } else {
        lut_cfg_default(&st->cfg);
        st->active_slot = 1u;
        st->stage = st->has_lut_ipc ? LUT_ST_PUSH_INIT : LUT_ST_UI_BUILD;
    }
}

static inline void lut_finish_load(LutModuleState *st) {
    int v0 = (st->slot_state[0] == LUT_SLOT_VALID);
    int v1 = (st->slot_state[1] == LUT_SLOT_VALID);
    int io_err = (st->slot_state[0] == LUT_SLOT_IO_ERR || st->slot_state[1] == LUT_SLOT_IO_ERR);
    if (!v0 && !v1 && io_err && st->load_retries < LUT_LOAD_MAX_RETRIES) {
        ++st->load_retries;
        st->load_slot = 0u;
        st->slot_state[0] = LUT_SLOT_UNREAD;
        st->slot_state[1] = LUT_SLOT_UNREAD;
        st->stage = LUT_ST_LOAD_WRITE_PATH;
        return;
    }
    if (v0 && v1) {
        uint32_t pick = (st->slot_cfg[1].seq > st->slot_cfg[0].seq) ? 1u : 0u;
        st->cfg = st->slot_cfg[pick];
        st->active_slot = pick;
    } else if (v0) {
        st->cfg = st->slot_cfg[0];
        st->active_slot = 0u;
    } else if (v1) {
        st->cfg = st->slot_cfg[1];
        st->active_slot = 1u;
    } else {
        lut_cfg_default(&st->cfg);
        st->active_slot = 1u;
    }
    st->stage = st->has_lut_ipc ? LUT_ST_PUSH_INIT : LUT_ST_UI_BUILD;
}

static inline void lut_advance_load(LutModuleState *st) {
    if (st->load_slot == 0u) {
        st->load_slot = 1u;
        st->stage = LUT_ST_LOAD_WRITE_PATH;
    } else {
        lut_finish_load(st);
    }
}

static inline void lut_handle_save_fail(LutModuleState *st) {
    ++st->saves_failed;
    if (st->save_retries < LUT_SAVE_MAX_RETRIES) {
        ++st->save_retries;
        st->save_fail_ms = st->now_ms;
        st->save_pending = 1u;
    } else {
        st->save_pending = 0u;
    }
    st->stage = LUT_ST_IDLE;
}

static inline uint32_t lut_lowest_ctrl_bit(uint32_t mask) {
    for (uint32_t id = LUT_UI_PAGE_ROOT; id <= LUT_UI_CTRL_COUNT; ++id) {
        if (mask & (UINT32_C(1) << id)) return id;
    }
    return 0u;
}

static inline void lut_recover(LutModuleState *st) {
    ++st->transient_errors;
    st->sdk.pending = 0u;
    if (st->ui_built) {
        if (st->card_fd >= 0) {
            if (st->stage >= LUT_ST_SAVE_WRITE_PATH && st->stage <= LUT_ST_SAVE_CLOSE) {
                st->io_err = 1u;
                st->stage = LUT_ST_SAVE_CLOSE;
                return;
            }
            st->card_fd = -1;
        }
        if (st->stage >= LUT_ST_SAVE_WRITE_PATH && st->stage <= LUT_ST_SAVE_CLOSE) {
            lut_handle_save_fail(st);
            return;
        }
        if (st->stage == LUT_ST_UI_UPDATE || st->stage == LUT_ST_UI_SYNC_GET) {
            if (st->ui_active_ctrl <= LUT_UI_CTRL_COUNT) {
                st->ui_dirty_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
                st->ui_sync_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
            }
            st->ui_active_ctrl = 0u;
        }
        st->stage = LUT_ST_IDLE;
        return;
    }
    switch (st->stage) {
    case LUT_ST_QUERY_UI:
        st->ui_caps = 0u;
        st->stage = LUT_ST_OPEN_SLOTS;
        break;
    case LUT_ST_OPEN_SLOTS:
    case LUT_ST_LANG_INIT:
    case LUT_ST_STATE_INIT:
        st->stage = LUT_ST_OPEN_MEM;
        break;
    case LUT_ST_OPEN_MEM:
    case LUT_ST_ALLOC_MEM:
    case LUT_ST_OPEN_CARD_OPEN:
    case LUT_ST_OPEN_CARD_READ:
    case LUT_ST_OPEN_CARD_WRITE:
    case LUT_ST_OPEN_CARD_CLOSE:
        st->card_read_ok = 0u;
        st->card_write_ok = 0u;
        lut_start_after_card_open(st);
        break;
    case LUT_ST_LOAD_WRITE_PATH:
    case LUT_ST_LOAD_OPEN:
    case LUT_ST_LOAD_READ_CALL:
    case LUT_ST_LOAD_READ_DONE:
    case LUT_ST_LOAD_CLOSE:
    case LUT_ST_LOAD_READ_DATA:
        st->card_fd = -1;
        st->slot_state[st->load_slot] = LUT_SLOT_IO_ERR;
        lut_advance_load(st);
        break;
    case LUT_ST_PUSH_INIT:
    case LUT_ST_STATE_POST_INIT:
        st->stage = LUT_ST_UI_BUILD;
        break;
    case LUT_ST_UI_BUILD:
        ++st->ui_build_retries;
        if (st->ui_build_retries > LUT_UI_BUILD_MAX_RETRIES) {
            st->ui_build_retries = 0u;
            ++st->ui_build_idx;
            if (st->ui_build_idx >= LUT_UI_CTRL_COUNT) {
                st->ui_built = 1u;
                st->stage = LUT_ST_IDLE;
            }
        }
        break;
    default:
        st->ui_built = 1u;
        st->stage = LUT_ST_IDLE;
        break;
    }
}

static inline void lut_commit_change(LutModuleState *st, uint32_t ctrl_id) {
    uint32_t next_seq = (st->cfg.seq < UINT32_MAX) ? (st->cfg.seq + 1u) : st->cfg.seq;
    lut_cfg_finalize(&st->cfg, next_seq);
    st->save_pending = 1u;
    st->save_retries = 0u;
    if (st->has_lut_ipc) {
        st->ipc_sync_needed = 1u;
    }
    st->ui_sync_mask |= (UINT32_C(1) << ctrl_id);
    lut_mark_dynamic_dirty(st);
}

static inline void lut_handle_ui_event(LutModuleState *st, const FxServiceReply *r) {
    if (r->bytes != sizeof(FxUiEvent)) return;
    FxUiEvent ev;
    lut_copy(&ev, r->data, (uint32_t)sizeof(ev));

    if (ev.event == FX_INPUT_FOCUS) {
        st->focus_foreground = 1u;
        if (st->has_lut_ipc) st->state_refresh_needed = 1u;
        if (st->has_lang_svc) st->lang_refresh_needed = 1u;
        return;
    }
    if (ev.event == FX_INPUT_BLUR) {
        st->focus_foreground = 0u;
        return;
    }
    if (ev.control < LUT_UI_MASTER_ON || ev.control > LUT_UI_CTRL_COUNT) return;
    if (st->ui_caps & FX_UI_CAP_EVENT_REVISION) {
        uint32_t ev_rev = fx_sdk_event_revision(&ev);
        uint32_t exp = st->ctrl_rev[ev.control];
        if (ev_rev != 0u && exp != 0u && ev_rev != exp) return;
    }

    if (ev.event == FX_INPUT_CHANGE && ev.value <= 1u) {
        uint8_t v = (uint8_t)ev.value;
        int changed = 0;
        if (ev.control == LUT_UI_MASTER_ON && st->cfg.master_on != v) {
            st->cfg.master_on = v;
            changed = 1;
        } else if (ev.control == LUT_UI_PREVIEW_ON && st->cfg.preview_on != v) {
            st->cfg.preview_on = v;
            changed = 1;
        } else if (ev.control == LUT_UI_STILL_ON && st->cfg.still_on != v) {
            st->cfg.still_on = v;
            changed = 1;
        } else if (ev.control == LUT_UI_LIVE_ON && st->cfg.live_on != v) {
            st->cfg.live_on = v;
            changed = 1;
        }
        if (changed) {
            lut_commit_change(st, ev.control);
        } else {
            st->ui_sync_mask |= (UINT32_C(1) << ev.control);
        }
        return;
    }

    if (ev.event == FX_INPUT_ACTIVATE) {
        if (ev.control == LUT_UI_CLEAR_LUT) {
            if (st->has_lut_ipc) st->clear_lut_pending = 1u;
            else lut_mark_dynamic_dirty(st);
            return;
        }
        if (ev.control == LUT_UI_DBG_RESET_POOL) {
            if (st->has_lut_ipc) st->reset_pool_pending = 1u;
            else {
                st->reset_status = 3u;
                lut_mark_dynamic_dirty(st);
            }
            return;
        }
        if (ev.control == LUT_UI_ACTIVE_LUT ||
            (ev.control >= LUT_UI_DBG_PIPE && ev.control <= LUT_UI_DBG_WD)) {
            if (st->has_lut_ipc) st->state_refresh_needed = 1u;
            else lut_mark_dynamic_dirty(st);
            return;
        }
    }
}

static inline void lut_handle_reply(LutModuleState *st, const FxServiceReply *r) {
    /* Special case: LUT_OP_RESET_PREVIEW returns FX_BUSY when hardware-quarantined.
     * Surface quarantined status cleanly instead of spinning in a retry loop! */
    if (st->stage == LUT_ST_RESET_PREVIEW && r->result == FX_BUSY) {
        ++st->resets_busy;
        st->reset_status = 2u;
        st->state_refresh_needed = 1u;
        lut_mark_dynamic_dirty(st);
        st->stage = LUT_ST_IDLE;
        return;
    }
    if (r->result == FX_BUSY) return; /* retry current stage */

    switch (st->stage) {
    case LUT_ST_QUERY_UI:
        if (r->result == FX_OK && r->bytes == sizeof(FxUiInfo)) {
            FxUiInfo info;
            lut_copy(&info, r->data, (uint32_t)sizeof(info));
            if (info.abi == 1u && info.bytes == sizeof(info)) {
                st->ui_caps = info.capabilities;
            }
        }
        st->stage = LUT_ST_OPEN_SLOTS;
        return;

    case LUT_ST_OPEN_SLOTS:
        if (r->result == FX_OK) {
            st->slots_binding = r->value[0];
            st->has_slots = 1u;
            st->stage = LUT_ST_LANG_INIT;
        } else {
            st->has_slots = 0u;
            st->has_lut_ipc = 0u;
            st->ui_lang = LUT_LANG_EN;
            st->stage = LUT_ST_OPEN_MEM;
        }
        return;

    case LUT_ST_LANG_INIT:
        if (r->result == FX_OK && r->value[1] == LUT_LANG_MAGIC) {
            st->has_lang_svc = 1u;
            st->ui_lang = (uint32_t)r->value[0];
            st->last_lang_ms = st->now_ms;
        } else {
            st->has_lang_svc = 0u;
            st->ui_lang = LUT_LANG_EN;
        }
        st->stage = LUT_ST_STATE_INIT;
        return;

    case LUT_ST_STATE_INIT:
        if (r->result == FX_OK && r->bytes == sizeof(V71r7LutIpcState)) {
            ++st->ipc_get_completed;
            lut_parse_state_reply(st, r);
        } else {
            st->has_lut_ipc = 0u;
        }
        st->stage = LUT_ST_OPEN_MEM;
        return;

    case LUT_ST_OPEN_MEM:
        if (r->result == FX_OK) {
            st->mem_binding = r->value[0];
            st->stage = LUT_ST_ALLOC_MEM;
        } else {
            st->has_bounce = 0u;
            lut_start_after_card_open(st);
        }
        return;

    case LUT_ST_ALLOC_MEM:
        if (r->result == FX_OK && r->value[1] != 0u) {
            st->buf_handle = r->value[0];
            st->buf_native = r->value[1];
            st->has_bounce = 1u;
            st->stage = LUT_ST_OPEN_CARD_OPEN;
        } else {
            st->has_bounce = 0u;
            lut_start_after_card_open(st);
        }
        return;

    case LUT_ST_OPEN_CARD_OPEN:
        st->card_open_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->stage = LUT_ST_OPEN_CARD_READ;
        return;

    case LUT_ST_OPEN_CARD_READ:
        st->card_read_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->stage = LUT_ST_OPEN_CARD_WRITE;
        return;

    case LUT_ST_OPEN_CARD_WRITE:
        st->card_write_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->stage = LUT_ST_OPEN_CARD_CLOSE;
        return;

    case LUT_ST_OPEN_CARD_CLOSE:
        st->card_close_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->card_read_ok = (st->has_bounce && st->card_open_h && st->card_read_h && st->card_close_h) ? 1u : 0u;
        st->card_write_ok = (st->has_bounce && st->card_open_h && st->card_write_h && st->card_close_h) ? 1u : 0u;
        lut_start_after_card_open(st);
        return;

    case LUT_ST_LOAD_WRITE_PATH:
        if (r->result != FX_OK) {
            st->slot_state[st->load_slot] = LUT_SLOT_IO_ERR;
            lut_advance_load(st);
        } else {
            st->card_fd = -1;
            st->io_done_acc = 0u;
            st->io_calls = 0u;
            st->io_err = 0u;
            st->stage = LUT_ST_LOAD_OPEN;
        }
        return;

    case LUT_ST_LOAD_OPEN:
        if (r->result != FX_OK) {
            st->slot_state[st->load_slot] = LUT_SLOT_IO_ERR;
            lut_advance_load(st);
        } else {
            int32_t fd = (int32_t)(uint32_t)r->value[0];
            if (fd >= 0) {
                st->card_fd = fd;
                st->stage = LUT_ST_LOAD_READ_CALL;
            } else if ((uint32_t)fd == LUT_OPEN_NOT_FOUND) {
                st->slot_state[st->load_slot] = LUT_SLOT_MISSING;
                lut_advance_load(st);
            } else {
                st->slot_state[st->load_slot] = LUT_SLOT_IO_ERR;
                lut_advance_load(st);
            }
        }
        return;

    case LUT_ST_LOAD_READ_CALL:
        if (r->result != FX_OK || (uint32_t)r->value[0] != 0u) {
            st->io_err = 1u;
            st->stage = LUT_ST_LOAD_CLOSE;
        } else {
            st->stage = LUT_ST_LOAD_READ_DONE;
        }
        return;

    case LUT_ST_LOAD_READ_DONE:
        if (r->result != FX_OK || r->bytes != 4u) {
            st->io_err = 1u;
            st->stage = LUT_ST_LOAD_CLOSE;
        } else {
            uint32_t done = lut_rd_u32(r->data);
            if (done > LUT_CFG_READ_MAX - st->io_done_acc) {
                st->io_err = 1u;
                st->stage = LUT_ST_LOAD_CLOSE;
            } else {
                st->io_done_acc += done;
                ++st->io_calls;
                if (done > 0u && st->io_done_acc < LUT_CFG_READ_MAX && st->io_calls < LUT_IO_MAX_CALLS) {
                    st->stage = LUT_ST_LOAD_READ_CALL;
                } else {
                    st->stage = LUT_ST_LOAD_CLOSE;
                }
            }
        }
        return;

    case LUT_ST_LOAD_CLOSE: {
        uint32_t cerr = (r->result == FX_OK) ? (uint32_t)r->value[0] : 1u;
        st->card_fd = -1;
        if (cerr != 0u || st->io_err) {
            st->slot_state[st->load_slot] = LUT_SLOT_IO_ERR;
            lut_advance_load(st);
        } else if (st->io_done_acc != LUT_CFG_BYTES) {
            st->slot_state[st->load_slot] = LUT_SLOT_CORRUPT;
            lut_advance_load(st);
        } else {
            st->stage = LUT_ST_LOAD_READ_DATA;
        }
        return;
    }

    case LUT_ST_LOAD_READ_DATA:
        if (r->result != FX_OK || r->bytes != LUT_CFG_BYTES) {
            st->slot_state[st->load_slot] = LUT_SLOT_IO_ERR;
        } else {
            V71r7LutWlmCfg tmp;
            lut_copy(&tmp, r->data, LUT_CFG_BYTES);
            if (lut_cfg_is_valid(&tmp)) {
                st->slot_cfg[st->load_slot] = tmp;
                st->slot_state[st->load_slot] = LUT_SLOT_VALID;
            } else {
                st->slot_state[st->load_slot] = LUT_SLOT_CORRUPT;
            }
        }
        lut_advance_load(st);
        return;

    case LUT_ST_PUSH_INIT:
        if (r->result == FX_OK) {
            ++st->ipc_set_completed;
            if (r->bytes == sizeof(V71r7LutIpcState)) {
                lut_parse_state_reply(st, r);
                st->ui_dirty_mask = 0u;
                st->stage = LUT_ST_UI_BUILD;
                return;
            }
            st->stage = LUT_ST_STATE_POST_INIT;
        } else {
            st->ui_dirty_mask = 0u;
            st->stage = LUT_ST_UI_BUILD;
        }
        return;

    case LUT_ST_STATE_POST_INIT:
        if (r->result == FX_OK && r->bytes == sizeof(V71r7LutIpcState)) {
            ++st->ipc_get_completed;
            lut_parse_state_reply(st, r);
        }
        st->ui_dirty_mask = 0u;
        st->stage = LUT_ST_UI_BUILD;
        return;

    case LUT_ST_UI_BUILD: {
        uint32_t id = st->ui_build_idx + 1u;
        if (r->result != FX_OK) {
            ++st->transient_errors;
            ++st->ui_build_retries;
            if (st->ui_build_retries <= LUT_UI_BUILD_MAX_RETRIES) return;
            st->ui_build_retries = 0u;
            st->ui_dirty_mask |= (UINT32_C(1) << id);
        } else {
            st->ui_build_retries = 0u;
            st->ctrl_rev[id] = (uint32_t)r->value[0];
            lut_format_ctrl_text(st, id, st->cached_text[id]);
        }
        ++st->ui_build_idx;
        if (st->ui_build_idx >= LUT_UI_CTRL_COUNT) {
            st->ui_built = 1u;
            st->stage = LUT_ST_IDLE;
        }
        return;
    }

    case LUT_ST_SAVE_WRITE_PATH:
        if (r->result != FX_OK) lut_handle_save_fail(st);
        else st->stage = LUT_ST_SAVE_WRITE_DATA;
        return;

    case LUT_ST_SAVE_WRITE_DATA:
        if (r->result != FX_OK) {
            lut_handle_save_fail(st);
        } else {
            st->card_fd = -1;
            st->io_done_acc = 0u;
            st->io_calls = 0u;
            st->io_err = 0u;
            st->stage = LUT_ST_SAVE_OPEN;
        }
        return;

    case LUT_ST_SAVE_OPEN:
        if (r->result != FX_OK) {
            lut_handle_save_fail(st);
        } else {
            int32_t fd = (int32_t)(uint32_t)r->value[0];
            if (fd < 0) {
                lut_handle_save_fail(st);
            } else {
                st->card_fd = fd;
                st->stage = LUT_ST_SAVE_WRITE_CALL;
            }
        }
        return;

    case LUT_ST_SAVE_WRITE_CALL:
        if (r->result != FX_OK || (uint32_t)r->value[0] != 0u) {
            st->io_err = 1u;
            st->stage = LUT_ST_SAVE_CLOSE;
        } else {
            st->stage = LUT_ST_SAVE_READ_DONE;
        }
        return;

    case LUT_ST_SAVE_READ_DONE:
        if (r->result != FX_OK || r->bytes != 4u) {
            st->io_err = 1u;
            st->stage = LUT_ST_SAVE_CLOSE;
        } else {
            uint32_t done = lut_rd_u32(r->data);
            if (done == 0u || done > LUT_CFG_BYTES - st->io_done_acc) {
                st->io_err = 1u;
                st->stage = LUT_ST_SAVE_CLOSE;
            } else {
                st->io_done_acc += done;
                ++st->io_calls;
                if (st->io_done_acc < LUT_CFG_BYTES && st->io_calls < LUT_IO_MAX_CALLS) {
                    st->stage = LUT_ST_SAVE_WRITE_CALL;
                } else {
                    st->stage = LUT_ST_SAVE_CLOSE;
                }
            }
        }
        return;

    case LUT_ST_SAVE_CLOSE: {
        uint32_t cerr = (r->result == FX_OK) ? (uint32_t)r->value[0] : 1u;
        st->card_fd = -1;
        if (!st->io_err && cerr == 0u && st->io_done_acc == LUT_CFG_BYTES) {
            st->active_slot = st->save_target_slot;
            st->slot_cfg[st->active_slot] = st->save_cfg;
            st->slot_state[st->active_slot] = LUT_SLOT_VALID;
            ++st->saves_completed;
            st->save_retries = 0u;
            if (st->cfg.seq != st->save_cfg.seq) {
                st->save_pending = 1u;
            }
            st->stage = LUT_ST_IDLE;
        } else {
            lut_handle_save_fail(st);
        }
        return;
    }

    case LUT_ST_SET_CFG:
        if (r->result == FX_OK) {
            ++st->ipc_set_completed;
            if (r->bytes == sizeof(V71r7LutIpcState)) {
                lut_parse_state_reply(st, r);
            } else {
                st->state_refresh_needed = 1u;
            }
        }
        st->stage = LUT_ST_IDLE;
        return;

    case LUT_ST_CLEAR_LUT:
        if (r->result == FX_OK) {
            ++st->clears_completed;
            st->el1_state.active_path[0] = '\0';
            st->el1_state.lut_active = 0u;
            st->state_refresh_needed = 1u;
            lut_mark_dynamic_dirty(st);
        }
        st->stage = LUT_ST_IDLE;
        return;

    case LUT_ST_RESET_PREVIEW:
        if (r->result == FX_OK) {
            ++st->resets_completed;
            st->reset_status = 1u;
            st->state_refresh_needed = 1u;
        } else {
            st->reset_status = 3u;
        }
        lut_mark_dynamic_dirty(st);
        st->stage = LUT_ST_IDLE;
        return;

    case LUT_ST_STATE_POLL:
        st->last_state_ms = st->now_ms;
        if (r->result == FX_OK && r->bytes == sizeof(V71r7LutIpcState)) {
            ++st->ipc_get_completed;
            lut_parse_state_reply(st, r);
        }
        st->stage = LUT_ST_IDLE;
        return;

    case LUT_ST_LANG_POLL:
        st->last_lang_ms = st->now_ms;
        if (r->result == FX_OK && r->value[1] == LUT_LANG_MAGIC) {
            uint32_t nl = (uint32_t)r->value[0];
            uint32_t old_b = lut_lang_bucket(st->ui_lang);
            uint32_t new_b = lut_lang_bucket(nl);
            st->ui_lang = nl;
            if (new_b != old_b) {
                for (uint32_t id = LUT_UI_PAGE_ROOT; id <= LUT_UI_CTRL_COUNT; ++id) {
                    st->ui_dirty_mask |= (UINT32_C(1) << id);
                }
                ++st->lang_switches;
            }
        }
        st->stage = LUT_ST_IDLE;
        return;

    case LUT_ST_UI_UPDATE:
        if (r->result == FX_OK && st->ui_active_ctrl >= LUT_UI_PAGE_ROOT &&
            st->ui_active_ctrl <= LUT_UI_CTRL_COUNT) {
            st->ctrl_rev[st->ui_active_ctrl] = (uint32_t)r->value[0];
            lut_format_ctrl_text(st, st->ui_active_ctrl, st->cached_text[st->ui_active_ctrl]);
        }
        if (st->ui_active_ctrl <= LUT_UI_CTRL_COUNT) {
            st->ui_dirty_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
            st->ui_sync_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
        }
        st->ui_active_ctrl = 0u;
        st->stage = LUT_ST_IDLE;
        return;

    case LUT_ST_UI_SYNC_GET:
        if (r->result == FX_OK && st->ui_active_ctrl >= LUT_UI_PAGE_ROOT &&
            st->ui_active_ctrl <= LUT_UI_CTRL_COUNT) {
            st->ctrl_rev[st->ui_active_ctrl] = (uint32_t)r->value[0];
        }
        if (st->ui_active_ctrl <= LUT_UI_CTRL_COUNT) {
            st->ui_sync_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
        }
        st->ui_active_ctrl = 0u;
        st->stage = LUT_ST_IDLE;
        return;

    case LUT_ST_POLL_EVENTS:
        if (r->result == FX_OK) {
            st->idle_polls = 0u;
            lut_handle_ui_event(st, r);
        } else if (r->result == FX_EMPTY) {
            ++st->idle_polls;
            if (st->has_lut_ipc && (st->idle_polls % LUT_STATE_IDLE_PERIOD == 0u)) {
                st->state_refresh_needed = 1u;
            }
        }
        st->stage = LUT_ST_IDLE;
        return;

    default:
        ++st->transient_errors;
        st->stage = LUT_ST_IDLE;
        return;
    }
}

static inline void lut_schedule_idle(LutModuleState *st) {
    if (st->stage != LUT_ST_IDLE) return;
    if (st->ipc_sync_needed && st->has_lut_ipc) {
        st->ipc_sync_needed = 0u;
        st->stage = LUT_ST_SET_CFG;
        return;
    }
    if (st->save_pending) {
        if (!st->has_bounce || !st->card_write_ok) {
            st->save_pending = 0u;
        } else if (st->save_retries == 0u ||
                   st->now_ms < st->save_fail_ms ||
                   (st->now_ms - st->save_fail_ms) >= LUT_SAVE_RETRY_MS) {
            st->save_pending = 0u;
            st->save_target_slot = st->active_slot ^ 1u;
            st->save_cfg = st->cfg;
            st->stage = LUT_ST_SAVE_WRITE_PATH;
            return;
        }
    }
    if (st->clear_lut_pending && st->has_lut_ipc) {
        st->clear_lut_pending = 0u;
        st->stage = LUT_ST_CLEAR_LUT;
        return;
    }
    if (st->reset_pool_pending && st->has_lut_ipc) {
        st->reset_pool_pending = 0u;
        st->stage = LUT_ST_RESET_PREVIEW;
        return;
    }
    if (st->has_lut_ipc &&
        (st->state_refresh_needed ||
         (st->focus_foreground && st->now_ms >= st->last_state_ms &&
          (st->now_ms - st->last_state_ms) >= LUT_STATE_POLL_MS))) {
        st->state_refresh_needed = 0u;
        st->stage = LUT_ST_STATE_POLL;
        return;
    }
    if (st->ui_dirty_mask != 0u) {
        uint32_t id = lut_lowest_ctrl_bit(st->ui_dirty_mask);
        if (id != 0u) {
            st->ui_active_ctrl = id;
            st->stage = LUT_ST_UI_UPDATE;
            return;
        }
        st->ui_dirty_mask = 0u;
    }
    if (st->ui_sync_mask != 0u) {
        uint32_t id = lut_lowest_ctrl_bit(st->ui_sync_mask);
        if (id != 0u) {
            st->ui_active_ctrl = id;
            st->stage = LUT_ST_UI_SYNC_GET;
            return;
        }
        st->ui_sync_mask = 0u;
    }
    if (st->has_lang_svc &&
        (st->lang_refresh_needed ||
         (st->now_ms >= st->last_lang_ms &&
          (st->now_ms - st->last_lang_ms) >= LUT_LANG_POLL_MS))) {
        st->lang_refresh_needed = 0u;
        st->stage = LUT_ST_LANG_POLL;
        return;
    }
    st->stage = LUT_ST_POLL_EVENTS;
}

static inline int lut_module_step(LutModuleState *st, const FxModuleServices *base) {
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
            lut_recover(st);
            return 0;
        }
        lut_handle_reply(st, &reply);
    }

    lut_schedule_idle(st);

    FxResult res = FX_ARGUMENT;
    switch (st->stage) {
    case LUT_ST_QUERY_UI:
        res = fx_sdk_ui_query(&st->sdk, api, 0u);
        break;
    case LUT_ST_OPEN_SLOTS:
        res = fx_sdk_open(&st->sdk, api, LUT_SLOTS_BINDING_NAME, LUT_SLOTS_BINDING_VER);
        break;
    case LUT_ST_LANG_INIT:
    case LUT_ST_LANG_POLL: {
        LutSlotsRequest q = { 0u, 0u, 0u, 0u };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->slots_binding,
                              LUT_SLOTS_LANG_GET, 0u, &q, (uint32_t)sizeof(q));
        break;
    }
    case LUT_ST_STATE_INIT:
    case LUT_ST_STATE_POST_INIT:
    case LUT_ST_STATE_POLL:
        res = lut_submit_ipc(st, api, LUT_OP_GET_STATE, 0, 0u);
        break;
    case LUT_ST_OPEN_MEM:
        res = fx_sdk_open(&st->sdk, api, "core.memory", 1u);
        break;
    case LUT_ST_ALLOC_MEM:
        res = fx_sdk_memory_allocate(&st->sdk, api, st->mem_binding, LUT_BOUNCE_BYTES, 1u);
        break;
    case LUT_ST_OPEN_CARD_OPEN:
        res = fx_sdk_open(&st->sdk, api, "camera.card.open", 1u);
        break;
    case LUT_ST_OPEN_CARD_READ:
        res = fx_sdk_open(&st->sdk, api, "camera.card.read", 1u);
        break;
    case LUT_ST_OPEN_CARD_WRITE:
        res = fx_sdk_open(&st->sdk, api, "camera.card.write", 1u);
        break;
    case LUT_ST_OPEN_CARD_CLOSE:
        res = fx_sdk_open(&st->sdk, api, "camera.card.close", 1u);
        break;
    case LUT_ST_LOAD_WRITE_PATH: {
        char path[32];
        lut_cfg_path(lut_effective_drive(st), st->load_slot, path);
        res = lut_submit_mem_write(st, api, LUT_BOUNCE_PATH_OFF, path, 32u);
        break;
    }
    case LUT_ST_LOAD_OPEN: {
        uint64_t args[8] = { st->buf_native + LUT_BOUNCE_PATH_OFF, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_open_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case LUT_ST_LOAD_READ_CALL: {
        uint64_t args[8] = {
            (uint64_t)(uint32_t)st->card_fd,
            st->buf_native + LUT_BOUNCE_DATA_OFF + st->io_done_acc,
            LUT_CFG_READ_MAX - st->io_done_acc,
            st->buf_native + LUT_BOUNCE_DONE_OFF,
            0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_read_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case LUT_ST_LOAD_READ_DONE:
    case LUT_ST_SAVE_READ_DONE:
        res = lut_submit_mem_read(st, api, LUT_BOUNCE_DONE_OFF, 4u);
        break;
    case LUT_ST_LOAD_CLOSE:
    case LUT_ST_SAVE_CLOSE: {
        uint64_t args[8] = { (uint64_t)(uint32_t)st->card_fd, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_close_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case LUT_ST_LOAD_READ_DATA:
        res = lut_submit_mem_read(st, api, LUT_BOUNCE_DATA_OFF, LUT_CFG_BYTES);
        break;
    case LUT_ST_PUSH_INIT:
    case LUT_ST_SET_CFG: {
        V71r7LutIpcSetCfg sc;
        lut_zero(&sc, (uint32_t)sizeof(sc));
        sc.magic = LUT_SLOTS_TUNNEL_MAGIC;
        sc.version = LUT_IPC_VERSION;
        sc.bytes = (uint16_t)sizeof(sc);
        sc.master_on = st->cfg.master_on ? 1u : 0u;
        sc.stage_mask = lut_cfg_stage_mask(&st->cfg);
        res = lut_submit_ipc(st, api, LUT_OP_SET_CFG, &sc, (uint32_t)sizeof(sc));
        break;
    }
    case LUT_ST_CLEAR_LUT:
        res = lut_submit_ipc(st, api, LUT_OP_CLEAR_LUT, 0, 0u);
        break;
    case LUT_ST_RESET_PREVIEW:
        res = lut_submit_ipc(st, api, LUT_OP_RESET_PREVIEW, 0, 0u);
        break;
    case LUT_ST_UI_BUILD: {
        FxUiControl ctrl;
        if (!lut_build_ui_control(st->ui_build_idx + 1u, st, &ctrl)) {
            lut_recover(st);
            return 0;
        }
        res = fx_sdk_ui(&st->sdk, api, &ctrl);
        break;
    }
    case LUT_ST_SAVE_WRITE_PATH: {
        char path[32];
        lut_cfg_path(lut_effective_drive(st), st->save_target_slot, path);
        res = lut_submit_mem_write(st, api, LUT_BOUNCE_PATH_OFF, path, 32u);
        break;
    }
    case LUT_ST_SAVE_WRITE_DATA:
        res = lut_submit_mem_write(st, api, LUT_BOUNCE_DATA_OFF, &st->save_cfg, LUT_CFG_BYTES);
        break;
    case LUT_ST_SAVE_OPEN: {
        uint64_t args[8] = {
            st->buf_native + LUT_BOUNCE_PATH_OFF,
            UINT64_C(0x301),
            0u, 0u, 0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_open_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case LUT_ST_SAVE_WRITE_CALL: {
        uint64_t args[8] = {
            (uint64_t)(uint32_t)st->card_fd,
            st->buf_native + LUT_BOUNCE_DATA_OFF + st->io_done_acc,
            LUT_CFG_BYTES - st->io_done_acc,
            st->buf_native + LUT_BOUNCE_DONE_OFF,
            0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_write_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case LUT_ST_UI_UPDATE: {
        FxUiControl ctrl;
        if (!lut_build_ui_control(st->ui_active_ctrl, st, &ctrl)) {
            lut_recover(st);
            return 0;
        }
        res = fx_sdk_ui(&st->sdk, api, &ctrl);
        break;
    }
    case LUT_ST_UI_SYNC_GET:
        res = fx_sdk_ui_get(&st->sdk, api, st->ui_active_ctrl);
        break;
    case LUT_ST_POLL_EVENTS:
        res = fx_sdk_events(&st->sdk, api);
        break;
    default:
        lut_recover(st);
        return 0;
    }

    if (res == FX_BUSY) return 0;
    if (res != FX_OK) {
        lut_recover(st);
        return 0;
    }
    return 0;
}

#endif /* LUT_CORE_H */
