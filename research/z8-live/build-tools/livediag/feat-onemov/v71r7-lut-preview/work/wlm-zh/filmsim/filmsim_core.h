/* SPDX-License-Identifier: MIT
 * Nikon Zf Film Grain & Ricoh HDF WonderLantern Module Core State Machine & Codec.
 * Exposes Nikon Zf 18-level Film Grain, Ricoh Optical HDF, Live Photo Link,
 * V71.5 Global / Per-NP3 Recipe Binding (95-slot FLMREC0/1.CFG + C10..C64
 * SC_RECORD +0x39C 'Z8FX' sync), dual-slot CRC32 card persistence
 * (FLMSIM0.CFG / FLMSIM1.CFG) with legacy Fuji style=2/3 backward compatibility,
 * EL1 "film.sim" v1 direct binding / zero-invasive FxModuleBus sync, and
 * A2 non-fatal transient error recovery (never triggers FAULT).
 *
 * Freestanding C11, zero libc dependencies, zero pointer relocations.
 */
#ifndef FILMSIM_CORE_H
#define FILMSIM_CORE_H

#include "fenestra_sdk.h"
#include "fenestra_file_sdk.h"
#include "fenestra_bus_sdk.h"
#include "filmsim_abi.h"

/* Native bounce buffer layout (allocated once via core.memory, 1024 B) */
#define FS_BOUNCE_BYTES           1024u
#define FS_BOUNCE_PATH_OFF        0u
#define FS_BOUNCE_DONE_OFF        64u
#define FS_BOUNCE_DATA_OFF        128u
#define FS_MEM_CHUNK_MAX          256u  /* Fits safely under FX_SERVICE_BYTES (768 B) */

#define FS_OPEN_NOT_FOUND         UINT32_C(0x80000010)
#define FS_READ_MAX_BYTES         65u   /* 64 B FLMSIM + 1 B oversized guard */
#define FS_REC_READ_MAX_BYTES     (Z8_NP3_TABLE_BYTES + 1u) /* 777 B FLMREC guard */
#define FS_XM6_LOG_MAX            640u  /* Fits under FS_BOUNCE_BYTES - FS_BOUNCE_DATA_OFF (896 B) */
#define FS_IO_MAX_CALLS           64u
#define FS_LOAD_MAX_RETRIES       3u
#define FS_SAVE_MAX_RETRIES       5u
#define FS_SAVE_RETRY_MS          500u
#define FS_SAVE_OPEN_FALLBACK_CNT 2u
#define FS_STATUS_POLL_PERIOD     16u
#define FS_UI_BUILD_MAX_RETRIES   8u

/* slots.state opcodes (orthogonal to SLOTS_INFO so c64-gate stays untouched) */
#define FS_SLOTS_BINDING_NAME     "slots.state"
#define FS_SLOTS_BINDING_VER      1u
#define FS_SLOTS_READ             1u
#define FS_SLOTS_WRITE            2u
#define FS_SLOTS_COMMIT           3u
#define FS_SLOTS_SELECTION        4u
#define FS_SLOTS_INVALIDATE       6u
#define FS_SLOTS_LANG_GET         15u
#define FS_LANG_MAGIC             UINT32_C(0x4c414e47) /* 'LANG' */
#define FS_LANG_EN                1u
#define FS_LANG_ZH_CN             8u
#define FS_LANG_ZH_TW             9u
#define FS_LANG_POLL_MS           1000u
#define FS_PC_POLL_MS             1000u
#define FS_SC_RECORD_BYTES        1024u
#define FS_SC_CHUNK_BYTES         512u  /* 16B header + 512B = 528B <= FX_SERVICE_BYTES (768B) */

enum FsLocale {
    FS_LOC_EN    = 0u,
    FS_LOC_ZH_CN = 1u,
    FS_LOC_ZH_TW = 2u
};

enum FsNp3PendingOp {
    FS_NP3_OP_NONE  = 0u,
    FS_NP3_OP_BIND  = 1u,
    FS_NP3_OP_CLEAR = 2u
};

typedef struct {
    uint32_t index;
    uint32_t offset;
    uint32_t bytes;
    uint32_t value;
} FsSlotsRequest;

_Static_assert(sizeof(FsSlotsRequest) == 16u, "FsSlotsRequest wire ABI must be 16 bytes");

enum FsUiControlId {
    FS_UI_PAGE_ROOT         = 1u,
    FS_UI_APPLY_MODE        = 2u,
    FS_UI_ZF_INTENSITY      = 3u,
    FS_UI_ZF_SIZE           = 4u,
    FS_UI_ZF_BLUR           = 5u,
    FS_UI_HDF_LEVEL         = 6u,
    FS_UI_LIVE_LINK         = 7u,
    FS_UI_WYSIWYG_PREVIEW   = 8u,
    FS_UI_BIND_NP3          = 9u,
    FS_UI_CLEAR_NP3         = 10u,
    FS_UI_DEBUG_PAGE        = 11u,
    FS_UI_XM6_MODE          = 12u,
    FS_UI_XM6_DIAG          = 13u,
    FS_UI_XM6_RESET         = 14u,
    FS_UI_STATUS_HW         = 15u,
    FS_UI_RESET_DEFAULTS    = 16u,
    FS_UI_CTRL_COUNT        = 16u
};

_Static_assert(FS_UI_CTRL_COUNT <= FX_UI_CONTROLS,
               "filmsim UI controls must fit within inline FX_UI_CONTROLS");

enum FsSlotStatus {
    FS_SLOT_UNREAD  = 0u,
    FS_SLOT_MISSING = 1u,
    FS_SLOT_CORRUPT = 2u,
    FS_SLOT_IO_ERR  = 3u,
    FS_SLOT_VALID   = 4u
};

enum FsStage {
    FS_ST_QUERY_UI = 0u,
    FS_ST_OPEN_SLOTS,
    FS_ST_LANG_INIT,
    FS_ST_PC_SEL_INIT,
    FS_ST_OPEN_MEM,
    FS_ST_ALLOC_MEM,
    FS_ST_OPEN_CARD_OPEN,
    FS_ST_OPEN_CARD_READ,
    FS_ST_OPEN_CARD_WRITE,
    FS_ST_OPEN_CARD_CLOSE,
    FS_ST_OPEN_FS,
    FS_ST_FS_STATUS_INIT,
    FS_ST_OPEN_BUS,
    FS_ST_BUS_PUBLISH,
    FS_ST_BUS_RESERVE,
    FS_ST_LOAD_WRITE_PATH,
    FS_ST_LOAD_OPEN,
    FS_ST_LOAD_READ_CALL,
    FS_ST_LOAD_READ_DONE,
    FS_ST_LOAD_CLOSE,
    FS_ST_LOAD_READ_DATA,
    /* Dual-slot FLMREC0/1.CFG boot load */
    FS_ST_REC_LOAD_WRITE_PATH,
    FS_ST_REC_LOAD_OPEN,
    FS_ST_REC_LOAD_READ_CALL,
    FS_ST_REC_LOAD_READ_DONE,
    FS_ST_REC_LOAD_CLOSE,
    FS_ST_REC_LOAD_READ_CHUNK,
    FS_ST_FS_GET_INIT,
    FS_ST_FS_PUSH_INIT,
    FS_ST_FS_SYNC_REC_CHUNK,
    FS_ST_FS_STATUS_POST_INIT,
    FS_ST_BUS_SEND,
    FS_ST_BUS_POLL_STALE,
    FS_ST_UI_BUILD,
    FS_ST_IDLE,
    /* Dual-slot FLMSIM0/1.CFG save */
    FS_ST_SAVE_WRITE_PATH,
    FS_ST_SAVE_WRITE_DATA,
    FS_ST_SAVE_OPEN,
    FS_ST_SAVE_WRITE_CALL,
    FS_ST_SAVE_READ_DONE,
    FS_ST_SAVE_CLOSE,
    /* Dual-slot FLMREC0/1.CFG save */
    FS_ST_REC_SAVE_WRITE_PATH,
    FS_ST_REC_SAVE_WRITE_CHUNK,
    FS_ST_REC_SAVE_OPEN,
    FS_ST_REC_SAVE_WRITE_CALL,
    FS_ST_REC_SAVE_READ_DONE,
    FS_ST_REC_SAVE_CLOSE,
    /* Active Picture Control & C10..C64 SC_RECORD +0x39C sync */
    FS_ST_PC_SEL_POLL,
    FS_ST_SC_TAIL_READ,
    FS_ST_SC_RMW_READ,
    FS_ST_SC_RMW_WRITE,
    FS_ST_SC_RMW_COMMIT,
    FS_ST_FS_SET_RECIPE,
    FS_ST_FS_SYNC,
    FS_ST_FS_XM6_RESET,
    FS_ST_FS_XM6_DIAG,
    FS_ST_FS_STATUS_POLL,
    /* XM6 diagnostic log card write (A:\FENESTRA\XM6_DIAG.LOG) */
    FS_ST_XM6_LOG_WRITE_PATH,
    FS_ST_XM6_LOG_WRITE_CHUNK,
    FS_ST_XM6_LOG_OPEN,
    FS_ST_XM6_LOG_WRITE_CALL,
    FS_ST_XM6_LOG_READ_DONE,
    FS_ST_XM6_LOG_CLOSE,
    FS_ST_LANG_POLL,
    FS_ST_UI_UPDATE,
    FS_ST_UI_SYNC_GET,
    FS_ST_POLL_EVENTS
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
    uint64_t fs_binding;
    uint64_t bus_binding;
    uint64_t bus_endpoint;
    uint32_t bus_corr;
    uint64_t now_ms;
    uint64_t last_lang_ms;
    uint64_t last_pc_ms;
    uint32_t ui_lang;

    uint8_t has_slots;
    uint8_t has_lang_svc;
    uint8_t lang_refresh_needed;
    uint8_t pc_refresh_needed;
    uint8_t has_bounce;
    uint8_t card_read_ok;
    uint8_t card_write_ok;
    uint8_t has_fs;
    uint8_t fs_via_slots;
    uint8_t has_fs_recipe_ops;
    uint8_t has_bus;
    uint8_t el1_status_valid;
    uint8_t bus_init_phase;
    uint8_t bus_sync_needed;
    uint8_t bus_send_retries;
    uint8_t focus_foreground;
    uint8_t ui_built;
    uint8_t ui_build_retries;
    uint64_t focus_epoch;

    /* Live EL1 telemetry snapshot & active boot drive ('A'/'B' from CSL1) */
    Z8FilmSimStatus el1_status;
    char active_drive;

    /* Active Picture Control & embedded SC_RECORD +0x39C view */
    uint32_t active_pc_sel;
    uint8_t  active_pc_valid;
    uint8_t  active_sc_recipe_valid;
    uint8_t  sc_tail_read_needed;
    uint8_t  pending_np3_op;
    Z8Np3Recipe active_sc_recipe;

    /* Global configuration & dual-slot FLMSIM0/1.CFG persistence */
    Z8FilmSimConfig cfg;
    Z8FilmSimConfig slot_cfg[2];
    uint8_t slot_state[2];
    uint32_t active_slot;
    uint32_t load_slot;
    uint32_t load_retries;

    /* 95-slot NP3 recipe table & dual-slot FLMREC0/1.CFG persistence */
    Z8FilmSimRecipeTable recipes;
    Z8FilmSimRecipeTable rec_tmp;
    Z8FilmSimRecipeChunk rec_chunk;
    Z8FilmSimRecipeMsg   fs_recipe_msg;
    uint8_t  fs_tx_buf[Z8_FS_SLOTS_HDR_BYTES + Z8_NP3_CHUNK_BYTES];
    uint8_t  rec_slot_state[2];
    uint32_t rec_active_slot;
    uint32_t rec_load_slot;
    uint32_t rec_load_retries;
    uint32_t rec_best_valid;
    uint32_t rec_chunk_off;
    uint32_t rec_sync_chunk_idx;
    uint32_t bound_count;
    uint8_t  rec_save_pending;
    uint8_t  rec_save_retries;
    uint8_t  rec_save_open_fails;
    uint8_t  rec_save_use_fallback;
    uint32_t rec_save_target_slot;
    uint64_t rec_save_fail_ms;
    uint8_t  fs_recipe_sync_pending;

    /* C10..C64 SC_RECORD full 1024 B read-modify-write scratch */
    uint8_t  sc_rec_buf[FS_SC_RECORD_BYTES];
    uint8_t  sc_rmw_pending;
    uint8_t  sc_rmw_clear;
    uint8_t  sc_rmw_step;
    uint32_t sc_rmw_index;
    Z8Np3Recipe sc_rmw_recipe;

    /* In-progress card I/O */
    int32_t  card_fd;
    uint32_t io_done_acc;
    uint32_t io_calls;
    uint32_t io_err;
    uint32_t save_target_slot;
    Z8FilmSimConfig save_cfg;

    /* UI build & synchronization */
    uint32_t ui_build_idx;
    uint32_t ctrl_rev[FS_UI_CTRL_COUNT + 1u];
    uint32_t ui_dirty_mask;
    uint32_t ui_sync_mask;
    uint32_t ui_active_ctrl;
    uint32_t idle_polls;
    char cached_bind_text[FX_UI_TEXT];
    char cached_clear_text[FX_UI_TEXT];
    char cached_xm6_diag[FX_UI_TEXT];
    char cached_xm6_reset[FX_UI_TEXT];
    char cached_status_hw[FX_UI_TEXT];

    /* CEVA-XM6 on-device self-test & A:\FENESTRA\XM6_DIAG.LOG writer */
    Z8Xm6DiagResult xm6_diag;
    uint8_t  xm6_diag_valid;
    uint8_t  xm6_diag_unsupported;
    uint8_t  xm6_diag_pending;
    uint8_t  xm6_reset_pending;
    uint8_t  xm6_diag_retried_reset;
    uint8_t  xm6_log_save_pending;
    uint8_t  xm6_log_saved;
    uint32_t xm6_log_len;
    uint32_t xm6_log_off;
    char     xm6_log_buf[FS_XM6_LOG_MAX];

    uint64_t save_fail_ms;
    uint8_t save_pending;
    uint8_t save_retries;
    uint8_t save_open_fails;
    uint8_t save_use_fallback_slot;
    uint8_t fs_sync_needed;
    uint8_t status_refresh_needed;

    /* Counters for observability / testing */
    uint32_t saves_completed;
    uint32_t saves_failed;
    uint32_t rec_saves_completed;
    uint32_t rec_saves_failed;
    uint32_t sc_rmw_completed;
    uint32_t fs_get_completed;
    uint32_t fs_set_completed;
    uint32_t fs_recipe_set_completed;
    uint32_t fs_recipe_sync_completed;
    uint32_t fs_status_completed;
    uint32_t xm6_diag_completed;
    uint32_t xm6_resets_completed;
    uint32_t xm6_logs_completed;
    uint32_t xm6_logs_failed;
    uint32_t resets_completed;
    uint32_t binds_completed;
    uint32_t clears_completed;
    uint32_t stale_events_ignored;
    uint32_t lang_polls_completed;
    uint32_t lang_switches;
    uint32_t transient_errors;
} FsModuleState;

static inline void fs_zero(void *dst, uint32_t n) {
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t idx = 0u; idx < n; ++idx) {
        p[idx] = 0u;
    }
}

static inline void fs_copy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t idx = 0u; idx < n; ++idx) {
        d[idx] = s[idx];
    }
}

static inline int fs_mem_eq(const void *a, const void *b, uint32_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (uint32_t idx = 0u; idx < n; ++idx) {
        if (pa[idx] != pb[idx]) {
            return 0;
        }
    }
    return 1;
}

static inline uint32_t fs_read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static inline uint32_t fs_utf8_char_len(const char *s) {
    uint8_t b0 = (uint8_t)s[0];
    if (b0 < 0x80u) {
        return 1u;
    }
    if ((b0 & 0xe0u) == 0xc0u) {
        return (s[1] != '\0') ? 2u : 1u;
    }
    if ((b0 & 0xf0u) == 0xe0u) {
        return (s[1] != '\0' && s[2] != '\0') ? 3u : 1u;
    }
    if ((b0 & 0xf8u) == 0xf0u) {
        return (s[1] != '\0' && s[2] != '\0' && s[3] != '\0') ? 4u : 1u;
    }
    return 1u;
}

static inline uint32_t fs_lang_bucket(uint32_t ui_lang) {
    if (ui_lang == FS_LANG_ZH_CN) {
        return FS_LOC_ZH_CN;
    }
    if (ui_lang == FS_LANG_ZH_TW) {
        return FS_LOC_ZH_TW;
    }
    return FS_LOC_EN;
}

static inline const char *fs_tr(uint32_t ui_lang,
                                const char *en,
                                const char *sc,
                                const char *tc) {
    if (ui_lang == FS_LANG_ZH_CN) {
        return sc;
    }
    if (ui_lang == FS_LANG_ZH_TW) {
        return tc;
    }
    return en;
}

static __attribute__((noinline)) void fs_str_copy(char *dst, uint32_t cap,
                                                  const char *src) {
    if (!cap) {
        return;
    }
    uint32_t pos = 0u;
    uint32_t max_bytes = cap - 1u;
    while (src && src[pos] != '\0') {
        uint32_t clen = fs_utf8_char_len(src + pos);
        if (pos + clen > max_bytes) {
            break;
        }
        for (uint32_t k = 0u; k < clen; ++k) {
            dst[pos] = src[pos];
            ++pos;
        }
    }
    while (pos < cap) {
        dst[pos++] = '\0';
    }
}

static __attribute__((noinline)) void fs_buf_append_str(char *dst, uint32_t cap,
                                                        uint32_t *pos,
                                                        const char *src) {
    if (!dst || !pos || !src || !cap) {
        return;
    }
    uint32_t max_bytes = cap - 1u;
    uint32_t i = 0u;
    while (src[i] != '\0') {
        uint32_t clen = fs_utf8_char_len(src + i);
        if (*pos + clen > max_bytes) {
            break;
        }
        for (uint32_t k = 0u; k < clen; ++k) {
            dst[(*pos)++] = src[i++];
        }
    }
    dst[*pos] = '\0';
}

static __attribute__((noinline)) void fs_buf_append_u32(char *dst, uint32_t cap,
                                                        uint32_t *pos,
                                                        uint32_t value) {
    char rev[12];
    uint32_t n = 0u;
    do {
        rev[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u && n < (uint32_t)sizeof(rev));
    while (n > 0u && *pos + 1u < cap) {
        dst[(*pos)++] = rev[--n];
    }
    if (cap) {
        dst[*pos] = '\0';
    }
}

static __attribute__((noinline)) void fs_buf_append_i32(char *dst, uint32_t cap,
                                                        uint32_t *pos,
                                                        int32_t value) {
    if (value < 0) {
        fs_buf_append_str(dst, cap, pos, "-");
        uint32_t mag = (uint32_t)(-(value + 1)) + 1u;
        fs_buf_append_u32(dst, cap, pos, mag);
    } else {
        fs_buf_append_u32(dst, cap, pos, (uint32_t)value);
    }
}

static __attribute__((noinline)) void fs_buf_append_hex_u32(char *dst, uint32_t cap,
                                                            uint32_t *pos,
                                                            uint32_t value,
                                                            uint32_t min_digits) {
    char rev[8];
    uint32_t n = 0u;
    do {
        uint32_t nib = value & 0xfu;
        rev[n++] = (char)((nib < 10u) ? ('0' + nib) : ('A' + (nib - 10u)));
        value >>= 4u;
    } while ((value != 0u || n < min_digits) && n < 8u);
    while (n > 0u && *pos + 1u < cap) {
        dst[(*pos)++] = rev[--n];
    }
    if (cap) {
        dst[*pos] = '\0';
    }
}

/* Format microseconds compactly as integer-rounded ms ("1.9ms" / "355ms") with 0 FP. */
static __attribute__((noinline)) void fs_buf_append_us_ms(char *dst, uint32_t cap,
                                                          uint32_t *pos,
                                                          uint32_t us) {
    if (us < 9950u) {
        uint32_t tenths = (us + 50u) / 100u;
        fs_buf_append_u32(dst, cap, pos, tenths / 10u);
        fs_buf_append_str(dst, cap, pos, ".");
        fs_buf_append_u32(dst, cap, pos, tenths % 10u);
        fs_buf_append_str(dst, cap, pos, "ms");
    } else {
        uint32_t ms = (us / 1000u) + ((us % 1000u) >= 500u ? 1u : 0u);
        fs_buf_append_u32(dst, cap, pos, ms);
        fs_buf_append_str(dst, cap, pos, "ms");
    }
}

static inline uint32_t fs_sat_add_u32(uint32_t a, uint32_t b) {
    return (a > UINT32_MAX - b) ? UINT32_MAX : (a + b);
}

static inline uint32_t fs_popcnt4(uint32_t mask) {
    uint32_t cnt = 0u;
    for (uint32_t b = 0u; b < 4u; ++b) {
        if (mask & (UINT32_C(1) << b)) {
            ++cnt;
        }
    }
    return cnt;
}

static inline uint32_t fs_crc32_ieee(const uint8_t *data, uint32_t len) {
    return z8_np3_crc32_ieee(data, len);
}

static inline uint32_t fs_cfg_crc32(const Z8FilmSimConfig *cfg) {
    uint8_t raw[Z8_FILMSIM_CFG_BYTES];
    fs_copy(raw, cfg, Z8_FILMSIM_CFG_BYTES);
    raw[12] = 0u;
    raw[13] = 0u;
    raw[14] = 0u;
    raw[15] = 0u;
    return fs_crc32_ieee(raw, Z8_FILMSIM_CFG_BYTES);
}

static inline void fs_cfg_finalize(Z8FilmSimConfig *cfg, uint32_t seq) {
    cfg->magic = Z8_FILMSIM_CFG_MAGIC;
    cfg->version = (uint16_t)Z8_FILMSIM_CFG_VERSION;
    cfg->bytes = (uint16_t)Z8_FILMSIM_CFG_BYTES;
    cfg->seq = seq;
    if (cfg->apply_mode > Z8_FS_APPLY_PER_NP3) {
        cfg->apply_mode = (uint8_t)Z8_FS_APPLY_GLOBAL;
    }
    if (cfg->xm6_mode > Z8_XM6_MODE_ARM_ONLY) {
        cfg->xm6_mode = (uint8_t)Z8_XM6_MODE_AUTO;
    }
    if (cfg->wysiwyg_preview > Z8_WYSIWYG_ON) {
        cfg->wysiwyg_preview = (uint8_t)Z8_WYSIWYG_OFF;
    }
    fs_zero(cfg->reserved, (uint32_t)sizeof(cfg->reserved));
    cfg->crc32 = 0u;
    cfg->crc32 = fs_cfg_crc32(cfg);
}

static inline void fs_cfg_set_default_fields(Z8FilmSimConfig *cfg) {
    cfg->film_style = Z8_STYLE_OFF;
    cfg->zf_intensity = 0u;
    cfg->zf_size = Z8_ZF_SIZE_DEFAULT;
    cfg->zf_blur_mode = Z8_ZF_BLUR_DEFAULT;
    cfg->fuji_chrome = Z8_CHROME_OFF;
    cfg->fuji_fx_blue = Z8_CHROME_OFF;
    cfg->fuji_lut_slot = 0u;
    cfg->fuji_lut_strength = Z8_FUJI_LUT_STRENGTH_DEFAULT;
    cfg->fuji_grain = Z8_FUJI_GRAIN_OFF;
    cfg->hdf_level = Z8_HDF_OFF;
    cfg->live_link = Z8_LIVE_LINK_OFF;
    cfg->apply_mode = Z8_FS_APPLY_GLOBAL;
    cfg->xm6_mode = Z8_XM6_MODE_AUTO;
    cfg->wysiwyg_preview = Z8_WYSIWYG_OFF;
}

static inline void fs_cfg_default(Z8FilmSimConfig *cfg) {
    fs_zero(cfg, (uint32_t)sizeof(*cfg));
    fs_cfg_set_default_fields(cfg);
    fs_cfg_finalize(cfg, 0u);
}

static inline int fs_cfg_is_valid(const Z8FilmSimConfig *cfg) {
    if (!cfg) {
        return 0;
    }
    if (cfg->magic != Z8_FILMSIM_CFG_MAGIC ||
        cfg->version != Z8_FILMSIM_CFG_VERSION ||
        cfg->bytes != Z8_FILMSIM_CFG_BYTES) {
        return 0;
    }
    /* Accept legacy film_style 2 (FUJIFILM) & 3 (FUJI_ZF) + legacy Fuji wire ranges
     * so existing FLMSIM0/1.CFG files validate CRC32 cleanly before normalization.
     * Old V71.4/V71.5 cards have byte +28 == 0 -> Z8_XM6_MODE_AUTO (100% compat). */
    if (cfg->film_style > Z8_STYLE_FUJI_ZF ||
        cfg->zf_intensity > Z8_ZF_INTENSITY_MAX ||
        cfg->zf_size < Z8_ZF_SIZE_MIN ||
        cfg->zf_size > Z8_ZF_SIZE_MAX ||
        cfg->zf_blur_mode > Z8_ZF_BLUR_SHARP ||
        cfg->fuji_chrome > Z8_CHROME_STRONG ||
        cfg->fuji_fx_blue > Z8_CHROME_STRONG ||
        cfg->fuji_lut_slot > Z8_FUJI_LUT_SLOT_MAX ||
        cfg->fuji_lut_strength > Z8_FUJI_LUT_STRENGTH_MAX ||
        cfg->fuji_grain > Z8_FUJI_GRAIN_STRONG_L ||
        cfg->hdf_level > Z8_HDF_STRONG ||
        cfg->live_link > Z8_LIVE_LINK_ON ||
        cfg->apply_mode > Z8_FS_APPLY_PER_NP3 ||
        cfg->xm6_mode > Z8_XM6_MODE_ARM_ONLY ||
        cfg->wysiwyg_preview > Z8_WYSIWYG_ON) {
        return 0;
    }
    for (uint32_t idx = 0u; idx < (uint32_t)sizeof(cfg->reserved); ++idx) {
        if (cfg->reserved[idx] != 0u) {
            return 0;
        }
    }
    return cfg->crc32 == fs_cfg_crc32(cfg);
}

/* Normalize legacy card/EL1 configs: style 2 (FUJI) -> 0 (OFF) keeping HDF & Live,
 * style 3 (FUJI_ZF) -> 1 (NIKON_ZF) keeping Zf grain, HDF & Live. Preserves seq
 * and apply_mode, recomputing CRC32 whenever any field is normalized. */
static inline void fs_cfg_normalize_legacy(Z8FilmSimConfig *cfg) {
    if (!cfg) {
        return;
    }
    int changed = 0;
    if (cfg->film_style == Z8_STYLE_FUJIFILM) {
        cfg->film_style = Z8_STYLE_OFF;
        changed = 1;
    } else if (cfg->film_style == Z8_STYLE_FUJI_ZF) {
        cfg->film_style = Z8_STYLE_NIKON_ZF;
        changed = 1;
    }
    if (cfg->fuji_chrome != Z8_CHROME_OFF) {
        cfg->fuji_chrome = Z8_CHROME_OFF;
        changed = 1;
    }
    if (cfg->fuji_fx_blue != Z8_CHROME_OFF) {
        cfg->fuji_fx_blue = Z8_CHROME_OFF;
        changed = 1;
    }
    if (cfg->fuji_lut_slot != 0u) {
        cfg->fuji_lut_slot = 0u;
        changed = 1;
    }
    if (cfg->fuji_lut_strength != Z8_FUJI_LUT_STRENGTH_DEFAULT) {
        cfg->fuji_lut_strength = Z8_FUJI_LUT_STRENGTH_DEFAULT;
        changed = 1;
    }
    if (cfg->fuji_grain != Z8_FUJI_GRAIN_OFF) {
        cfg->fuji_grain = Z8_FUJI_GRAIN_OFF;
        changed = 1;
    }
    if (changed) {
        fs_cfg_finalize(cfg, cfg->seq);
    }
}

static inline int fs_status_is_valid(const Z8FilmSimStatus *st) {
    if (!st) {
        return 0;
    }
    if (st->magic != Z8_FILMSIM_CFG_MAGIC ||
        st->version != Z8_FILMSIM_CFG_VERSION ||
        st->bytes != Z8_FILMSIM_STATUS_BYTES) {
        return 0;
    }
    if (st->active_lut_slot > Z8_FUJI_LUT_SLOT_MAX) {
        return 0;
    }
    return 1;
}

static inline int fs_xm6_diag_is_valid(const Z8Xm6DiagResult *d) {
    if (!d) {
        return 0;
    }
    if (d->magic != Z8_XM6_DIAG_MAGIC ||
        d->version != Z8_XM6_DIAG_VERSION ||
        d->bytes != Z8_XM6_DIAG_BYTES) {
        return 0;
    }
    if ((d->passed_mask & (uint16_t)(~Z8_XM6_DIAG_ALL_MASK)) != 0u ||
        (d->ran_mask & (uint16_t)(~Z8_XM6_DIAG_ALL_MASK)) != 0u) {
        return 0;
    }
    return 1;
}

/* Format pure-ASCII diagnostic report written to A:\FENESTRA\XM6_DIAG.LOG. */
static __attribute__((noinline)) void fs_format_xm6_diag_log(FsModuleState *st) {
    const Z8Xm6DiagResult *d = &st->xm6_diag;
    const Z8FilmSimStatus *s = &st->el1_status;
    char *buf = st->xm6_log_buf;
    uint32_t pos = 0u;
    fs_zero(buf, FS_XM6_LOG_MAX);

    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, "Z8_XM6_DIAG_V1\nhw_state=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, d->hw_state);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " loader_fault=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, d->loader_fault);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " fw_loaded=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, d->fw_loaded);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " breaker=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, d->breaker_tripped);

    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, "\npassed_mask=0x");
    fs_buf_append_hex_u32(buf, FS_XM6_LOG_MAX, &pos, d->passed_mask, 2u);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " ran_mask=0x");
    fs_buf_append_hex_u32(buf, FS_XM6_LOG_MAX, &pos, d->ran_mask, 2u);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " dsp_status=0x");
    fs_buf_append_hex_u32(buf, FS_XM6_LOG_MAX, &pos, d->dsp_status_last, 8u);

    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, "\ndiff_grain_exact=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, d->diff_grain_exact);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " diff_grain_sharp=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, d->diff_grain_sharp);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " diff_hdf_still=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, d->diff_hdf_still);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " diff_hdf_live=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, d->diff_hdf_live);

    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, "\nus_grain_exact=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, d->us_grain_exact);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " us_grain_sharp=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, d->us_grain_sharp);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " us_hdf_still=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, d->us_hdf_still);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " us_hdf_live=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, d->us_hdf_live);

    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, "\nstatus_hw=0x");
    fs_buf_append_hex_u32(buf, FS_XM6_LOG_MAX, &pos, st->el1_status_valid ? s->xm6_hw_state : 0u, 2u);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " status_flags=0x");
    fs_buf_append_hex_u32(buf, FS_XM6_LOG_MAX, &pos, st->el1_status_valid ? s->xm6_runtime_flags : 0u, 2u);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " status_err=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, st->el1_status_valid ? s->xm6_last_err : 0u);

    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, "\nxm6_still_ok=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, st->el1_status_valid ? s->xm6_still_ok : 0u);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " xm6_live_ok=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, st->el1_status_valid ? s->xm6_live_ok : 0u);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " xm6_fallback=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, st->el1_status_valid ? s->xm6_fallback_cnt : 0u);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " last_still_ms=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, st->el1_status_valid ? s->xm6_last_still_ms : 0u);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " last_live_us=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, st->el1_status_valid ? s->xm6_last_live_us : 0u);

    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, "\ncfg_xm6_mode=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, st->cfg.xm6_mode);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " cfg_seq=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, st->cfg.seq);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, " diag_seq=");
    fs_buf_append_u32(buf, FS_XM6_LOG_MAX, &pos, st->xm6_diag_completed);
    fs_buf_append_str(buf, FS_XM6_LOG_MAX, &pos, "\n");
    st->xm6_log_len = pos;
}

static inline int fs_cfg_is_active(const Z8FilmSimConfig *cfg) {
    if (!cfg) {
        return 0;
    }
    if (cfg->hdf_level != Z8_HDF_OFF) {
        return 1;
    }
    if ((cfg->film_style == Z8_STYLE_NIKON_ZF ||
         cfg->film_style == Z8_STYLE_FUJI_ZF) &&
        cfg->zf_intensity > 0u) {
        return 1;
    }
    return 0;
}

static inline void fs_cfg_select_dual(const Z8FilmSimConfig *s0, int v0,
                                      const Z8FilmSimConfig *s1, int v1,
                                      Z8FilmSimConfig *out_cfg,
                                      uint32_t *out_active_slot) {
    if (v0 && v1) {
        if (s1->seq > s0->seq) {
            *out_cfg = *s1;
            *out_active_slot = 1u;
        } else {
            *out_cfg = *s0;
            *out_active_slot = 0u;
        }
    } else if (v0) {
        *out_cfg = *s0;
        *out_active_slot = 0u;
    } else if (v1) {
        *out_cfg = *s1;
        *out_active_slot = 1u;
    } else {
        fs_cfg_default(out_cfg);
        *out_active_slot = 1u; /* Next save writes slot 0 first */
    }
}

static inline char fs_effective_drive(const FsModuleState *st) {
    return (st && st->active_drive == 'B') ? 'B' : 'A';
}

static inline void fs_slot_path(char drive, uint32_t slot, char out[32]) {
    fs_zero(out, 32u);
    if (slot == 0u) {
        fs_str_copy(out, 32u, Z8_FILMSIM_CFG_PATH_0);
    } else {
        fs_str_copy(out, 32u, Z8_FILMSIM_CFG_PATH_1);
    }
    out[0] = (drive == 'B') ? 'B' : 'A';
}

static inline void fs_rec_slot_path(char drive, uint32_t slot, char out[32]) {
    fs_zero(out, 32u);
    if (slot == 0u) {
        fs_str_copy(out, 32u, Z8_FILMSIM_RECIPE_PATH_0);
    } else {
        fs_str_copy(out, 32u, Z8_FILMSIM_RECIPE_PATH_1);
    }
    out[0] = (drive == 'B') ? 'B' : 'A';
}

static inline void fs_xm6_log_path(char drive, char out[32]) {
    fs_zero(out, 32u);
    fs_str_copy(out, 32u, Z8_FILMSIM_XM6_DIAG_LOG_PATH);
    out[0] = (drive == 'B') ? 'B' : 'A';
}

static inline uint32_t fs_count_bound_recipes(const Z8FilmSimRecipeTable *t) {
    uint32_t cnt = 0u;
    for (uint32_t i = 0u; i < Z8_NP3_SLOT_COUNT; ++i) {
        if (t->entries[i].flags & Z8_NP3_RECIPE_FLAG_ENABLED) {
            ++cnt;
        }
    }
    return cnt;
}

static inline uint32_t fs_effective_active_sel(const FsModuleState *st) {
    if (st->active_pc_valid) {
        return st->active_pc_sel;
    }
    if (st->has_fs && st->el1_status_valid &&
        z8_np3_selector_to_slot(st->el1_status.active_pc_sel) != Z8_NP3_SLOT_INVALID) {
        return st->el1_status.active_pc_sel;
    }
    return Z8_NP3_SLOT_INVALID;
}

static inline int fs_active_slot_is_bound(const FsModuleState *st) {
    uint32_t sel = fs_effective_active_sel(st);
    uint32_t slot = z8_np3_selector_to_slot(sel);
    if (slot == Z8_NP3_SLOT_INVALID) {
        return 0;
    }
    if (st->active_sc_recipe_valid &&
        (st->active_sc_recipe.flags & Z8_NP3_RECIPE_FLAG_ENABLED)) {
        return 1;
    }
    if (st->recipes.entries[slot].flags & Z8_NP3_RECIPE_FLAG_ENABLED) {
        return 1;
    }
    if (st->has_fs && st->el1_status_valid &&
        st->el1_status.active_pc_sel == sel &&
        (st->el1_status.active_recipe_bound ||
         (st->el1_status.flags & Z8_FS_FLAG_RECIPE_BOUND))) {
        return 1;
    }
    return 0;
}

/* Format short ASCII Picture Control label (0 missing glyphs in EN/SC/TC, 0 relocs). */
static __attribute__((noinline)) void fs_append_pc_name(char *dst, uint32_t cap,
                                                        uint32_t *pos,
                                                        uint32_t sel) {
    if (sel == Z8_NP3_SLOT_INVALID) {
        fs_buf_append_str(dst, cap, pos, "--");
        return;
    }
    uint32_t sc_idx = 0u;
    if (z8_np3_selector_is_c64(sel, &sc_idx)) {
        fs_buf_append_str(dst, cap, pos, "C-");
        fs_buf_append_u32(dst, cap, pos, sc_idx + 1u);
        return;
    }
    if (sel >= 0x1cu && sel <= 0x24u) {
        fs_buf_append_str(dst, cap, pos, "C-");
        fs_buf_append_u32(dst, cap, pos, (sel - 0x1cu) + 1u);
        return;
    }
    if (sel >= 0x08u && sel <= 0x1bu) {
        fs_buf_append_str(dst, cap, pos, "CR");
        fs_buf_append_u32(dst, cap, pos, (sel - 0x08u) + 1u);
        return;
    }
    switch (sel) {
    case 0x00u: fs_buf_append_str(dst, cap, pos, "AUTO"); break;
    case 0x01u: fs_buf_append_str(dst, cap, pos, "SD");   break;
    case 0x02u: fs_buf_append_str(dst, cap, pos, "NL");   break;
    case 0x03u: fs_buf_append_str(dst, cap, pos, "VI");   break;
    case 0x04u: fs_buf_append_str(dst, cap, pos, "MC");   break;
    case 0x05u: fs_buf_append_str(dst, cap, pos, "PT");   break;
    case 0x06u: fs_buf_append_str(dst, cap, pos, "LS");   break;
    case 0x07u: fs_buf_append_str(dst, cap, pos, "FL");   break;
    case 0x25u: fs_buf_append_str(dst, cap, pos, "RP");   break;
    case 0x26u: fs_buf_append_str(dst, cap, pos, "FM");   break;
    case 0x27u: fs_buf_append_str(dst, cap, pos, "DM");   break;
    default:    fs_buf_append_str(dst, cap, pos, "--");   break;
    }
}

static __attribute__((noinline)) void fs_format_bind_text(const FsModuleState *st,
                                                          char out[FX_UI_TEXT]) {
    fs_zero(out, FX_UI_TEXT);
    uint32_t lang = st->ui_lang;
    uint32_t pos = 0u;
    uint32_t sel = fs_effective_active_sel(st);
    fs_append_pc_name(out, FX_UI_TEXT, &pos, sel);
    if (sel == Z8_NP3_SLOT_INVALID) {
        return;
    }
    if (fs_active_slot_is_bound(st)) {
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, " [Bound]", " [已绑定]", " [已關聯]"));
    } else {
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, " [None]", " [未绑定]", " [未關聯]"));
    }
}

static __attribute__((noinline)) void fs_format_clear_text(const FsModuleState *st,
                                                           char out[FX_UI_TEXT]) {
    fs_zero(out, FX_UI_TEXT);
    uint32_t lang = st->ui_lang;
    uint32_t pos = 0u;
    fs_buf_append_str(out, FX_UI_TEXT, &pos,
                      fs_tr(lang, "Bound:", "已绑定:", "已關聯:"));
    fs_buf_append_u32(out, FX_UI_TEXT, &pos, st->bound_count);
}

static __attribute__((noinline)) void fs_format_xm6_reset(const FsModuleState *st,
                                                          char out[FX_UI_TEXT]) {
    fs_zero(out, FX_UI_TEXT);
    uint32_t lang = st->ui_lang;
    uint32_t pos = 0u;
    if (!st->has_fs) {
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, "No EL1", "无 EL1", "無 EL1"));
        return;
    }
    if (st->el1_status_valid &&
        (st->el1_status.xm6_runtime_flags & Z8_XM6_FLAG_BREAKER_TRIPPED)) {
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, "Tripped E:", "已熔断 E:", "已熔斷 E:"));
        fs_buf_append_u32(out, FX_UI_TEXT, &pos, st->el1_status.xm6_last_err);
        return;
    }
    fs_buf_append_str(out, FX_UI_TEXT, &pos,
                      fs_tr(lang, "OK", "正常", "正常"));
}

/* Concise Hardware & Runtime Status (<= 36B pure ASCII < 48B FX_UI_TEXT). */
static __attribute__((noinline)) void fs_format_status_hw(const FsModuleState *st,
                                                          char out[FX_UI_TEXT]) {
    fs_zero(out, FX_UI_TEXT);
    uint32_t cap = 37u; /* max 36 ASCII bytes + NUL */
    uint32_t pos = 0u;
    char drv = fs_effective_drive(st);

    if (st->has_fs && st->el1_status_valid) {
        const Z8FilmSimStatus *s = &st->el1_status;
        if (s->last_error != 0) {
            fs_buf_append_str(out, cap, &pos, "EL1:E");
            fs_buf_append_i32(out, cap, &pos, s->last_error);
        } else if (s->flags & Z8_FS_FLAG_DORMANT) {
            fs_buf_append_str(out, cap, &pos, "EL1:DM");
        } else if (s->flags & Z8_FS_FLAG_EL1_READY) {
            fs_buf_append_str(out, cap, &pos, "EL1:ON");
        } else {
            fs_buf_append_str(out, cap, &pos, "EL1:IN");
        }
        fs_buf_append_str(out, cap, &pos, " S:");
        fs_buf_append_u32(out, cap, &pos, s->photo_applied);
        fs_buf_append_str(out, cap, &pos, " L:");
        fs_buf_append_u32(out, cap, &pos, s->live_applied);

        int fault = (s->xm6_hw_state & Z8_XM6_HW_LOADER_FAULT_BIT) != 0u;
        uint32_t rt = s->xm6_runtime_flags;
        if (fault) {
            fs_buf_append_str(out, cap, &pos, " XM6:LF");
        } else if (rt & Z8_XM6_FLAG_BREAKER_TRIPPED) {
            fs_buf_append_str(out, cap, &pos, " XM6:TR");
        } else if (st->cfg.xm6_mode == Z8_XM6_MODE_ARM_ONLY) {
            fs_buf_append_str(out, cap, &pos, " XM6:CPU");
        } else if (!(rt & Z8_XM6_FLAG_HW_READY)) {
            fs_buf_append_str(out, cap, &pos, " XM6:NO");
        } else {
            uint32_t ok_cnt = fs_sat_add_u32(s->xm6_still_ok, s->xm6_live_ok);
            fs_buf_append_str(out, cap, &pos, " XM6:OK ");
            fs_buf_append_u32(out, cap, &pos, ok_cnt);
            fs_buf_append_str(out, cap, &pos, "/");
            fs_buf_append_u32(out, cap, &pos, s->xm6_fallback_cnt);
        }
        if (pos + 4u < cap) {
            fs_buf_append_str(out, cap, &pos, drv == 'B' ? " [B]" : " [A]");
        }
        return;
    }
    if (st->has_fs) {
        fs_buf_append_str(out, cap, &pos, drv == 'B' ? "EL1:RDY [B]" : "EL1:RDY [A]");
        return;
    }
    if (st->has_bus) {
        int active = (st->cfg.apply_mode == Z8_FS_APPLY_PER_NP3)
                         ? fs_active_slot_is_bound(st)
                         : fs_cfg_is_active(&st->cfg);
        if (active) {
            fs_buf_append_str(out, cap, &pos, drv == 'B' ? "BUS:ON [B]" : "BUS:ON [A]");
        } else {
            fs_buf_append_str(out, cap, &pos, drv == 'B' ? "BUS:DM [B]" : "BUS:DM [A]");
        }
        return;
    }
    fs_buf_append_str(out, cap, &pos, drv == 'B' ? "NO EL1 [B]" : "NO EL1 [A]");
}

static __attribute__((noinline)) void fs_format_xm6_diag(const FsModuleState *st,
                                                         char out[FX_UI_TEXT]) {
    fs_zero(out, FX_UI_TEXT);
    uint32_t lang = st->ui_lang;
    uint32_t pos = 0u;
    if (!st->has_fs) {
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, "No EL1", "无 EL1", "無 EL1"));
        return;
    }
    if (st->xm6_diag_unsupported) {
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, "EL1 Unsupported", "EL1 不支持", "EL1 不支援"));
        return;
    }
    if (!st->xm6_diag_valid) {
        if (st->el1_status_valid &&
            (st->el1_status.xm6_runtime_flags & Z8_XM6_FLAG_BREAKER_TRIPPED)) {
            fs_buf_append_str(out, FX_UI_TEXT, &pos,
                              fs_tr(lang, "Trip E:", "熔断 E:", "熔斷 E:"));
            fs_buf_append_u32(out, FX_UI_TEXT, &pos, st->el1_status.xm6_last_err);
            fs_buf_append_str(out, FX_UI_TEXT, &pos,
                              fs_tr(lang, " Tap Reset", " 按此重置", " 按此重設"));
            return;
        }
        if (st->el1_status_valid &&
            !(st->el1_status.xm6_runtime_flags & Z8_XM6_FLAG_HW_READY)) {
            uint32_t hw = (uint32_t)(st->el1_status.xm6_hw_state & Z8_XM6_HW_STATE_MASK);
            fs_buf_append_str(out, FX_UI_TEXT, &pos,
                              fs_tr(lang, "Tap to Test (S:", "按此自检 (S:", "按此自檢 (S:"));
            fs_buf_append_u32(out, FX_UI_TEXT, &pos, hw);
            fs_buf_append_str(out, FX_UI_TEXT, &pos, ")");
            return;
        }
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, "Tap to Run", "按此运行自检", "按此執行自檢"));
        return;
    }

    const Z8Xm6DiagResult *d = &st->xm6_diag;
    uint32_t total_diff = fs_sat_add_u32(
        fs_sat_add_u32(d->diff_grain_exact, d->diff_grain_sharp),
        fs_sat_add_u32(d->diff_hdf_still, d->diff_hdf_live));

    if (d->loader_fault != 0u) {
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, "XM6 LoadErr(F:", "XM6加载错(F:", "XM6載入錯(F:"));
        fs_buf_append_u32(out, FX_UI_TEXT, &pos, d->loader_fault);
        fs_buf_append_str(out, FX_UI_TEXT, &pos, " S:");
        fs_buf_append_u32(out, FX_UI_TEXT, &pos, d->hw_state);
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, ") ARM", ") 用ARM", ") 用ARM"));
        return;
    }
    if (d->ran_mask == 0u && d->hw_state != 2u) {
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, "XM6 NoHW(S:", "XM6无硬件(S:", "XM6無硬體(S:"));
        fs_buf_append_u32(out, FX_UI_TEXT, &pos, d->hw_state);
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, ") Auto ARM", ") 自动使用ARM", ") 自動使用ARM"));
        return;
    }
    if (d->ran_mask == 0u && d->breaker_tripped != 0u) {
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, "XM6 Tripped(S:", "XM6熔断(S:", "XM6熔斷(S:"));
        fs_buf_append_u32(out, FX_UI_TEXT, &pos, d->hw_state);
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, ") Auto ARM", ") 自动使用ARM", ") 自動使用ARM"));
        return;
    }
    if (d->ran_mask == 0u) {
        if (d->hw_state == 2u && st->cfg.xm6_mode == Z8_XM6_MODE_ARM_ONLY &&
            d->dsp_status_last == 0u) {
            fs_buf_append_str(out, FX_UI_TEXT, &pos,
                              fs_tr(lang, "CPU Only (S:", "仅 CPU (S:", "僅 CPU (S:"));
            fs_buf_append_u32(out, FX_UI_TEXT, &pos, d->hw_state);
            fs_buf_append_str(out, FX_UI_TEXT, &pos, ")");
            return;
        }
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, "XM6 Skip(S:", "XM6未运行(S:", "XM6未執行(S:"));
        fs_buf_append_u32(out, FX_UI_TEXT, &pos, d->hw_state);
        fs_buf_append_str(out, FX_UI_TEXT, &pos, " E:");
        fs_buf_append_hex_u32(out, FX_UI_TEXT, &pos, d->dsp_status_last, 1u);
        fs_buf_append_str(out, FX_UI_TEXT, &pos, ")");
        return;
    }
    if (d->passed_mask == Z8_XM6_DIAG_ALL_MASK &&
        d->ran_mask == Z8_XM6_DIAG_ALL_MASK &&
        total_diff == 0u) {
        uint32_t g_us = d->us_grain_exact ? d->us_grain_exact : d->us_grain_sharp;
        fs_buf_append_str(out, FX_UI_TEXT, &pos,
                          fs_tr(lang, "XM6 PASS 0err", "XM6自检通过 0错", "XM6自檢通過 0錯"));
        fs_buf_append_str(out, FX_UI_TEXT, &pos, " G:");
        fs_buf_append_us_ms(out, FX_UI_TEXT, &pos, g_us);
        fs_buf_append_str(out, FX_UI_TEXT, &pos, " H:");
        fs_buf_append_us_ms(out, FX_UI_TEXT, &pos, d->us_hdf_live);
        fs_buf_append_str(out, FX_UI_TEXT, &pos, " S:");
        fs_buf_append_us_ms(out, FX_UI_TEXT, &pos, d->us_hdf_still);
        return;
    }
    fs_buf_append_str(out, FX_UI_TEXT, &pos,
                      fs_tr(lang, "XM6 FAIL ", "XM6异常 ", "XM6異常 "));
    fs_buf_append_u32(out, FX_UI_TEXT, &pos, fs_popcnt4(d->passed_mask));
    fs_buf_append_str(out, FX_UI_TEXT, &pos, "/");
    fs_buf_append_u32(out, FX_UI_TEXT, &pos, fs_popcnt4(d->ran_mask));
    fs_buf_append_str(out, FX_UI_TEXT, &pos,
                      fs_tr(lang, " D:", " 差:", " 差:"));
    fs_buf_append_u32(out, FX_UI_TEXT, &pos, total_diff);
    fs_buf_append_str(out, FX_UI_TEXT, &pos, " M:");
    fs_buf_append_hex_u32(out, FX_UI_TEXT, &pos, d->passed_mask, 1u);
    if (d->dsp_status_last != 0u) {
        fs_buf_append_str(out, FX_UI_TEXT, &pos, " E:");
        fs_buf_append_hex_u32(out, FX_UI_TEXT, &pos, d->dsp_status_last, 1u);
    }
}

static inline void fs_mark_status_dirty_if_changed(FsModuleState *st) {
    char s_bind[FX_UI_TEXT];
    char s_clear[FX_UI_TEXT];
    char s_diag[FX_UI_TEXT];
    char s_reset[FX_UI_TEXT];
    char s_hw[FX_UI_TEXT];
    fs_format_bind_text(st, s_bind);
    fs_format_clear_text(st, s_clear);
    fs_format_xm6_diag(st, s_diag);
    fs_format_xm6_reset(st, s_reset);
    fs_format_status_hw(st, s_hw);
    if (!fs_mem_eq(s_bind, st->cached_bind_text, FX_UI_TEXT)) {
        st->ui_dirty_mask |= (UINT32_C(1) << FS_UI_BIND_NP3);
    }
    if (!fs_mem_eq(s_clear, st->cached_clear_text, FX_UI_TEXT)) {
        st->ui_dirty_mask |= (UINT32_C(1) << FS_UI_CLEAR_NP3);
    }
    if (!fs_mem_eq(s_diag, st->cached_xm6_diag, FX_UI_TEXT)) {
        st->ui_dirty_mask |= (UINT32_C(1) << FS_UI_XM6_DIAG);
    }
    if (!fs_mem_eq(s_reset, st->cached_xm6_reset, FX_UI_TEXT)) {
        st->ui_dirty_mask |= (UINT32_C(1) << FS_UI_XM6_RESET);
    }
    if (!fs_mem_eq(s_hw, st->cached_status_hw, FX_UI_TEXT)) {
        st->ui_dirty_mask |= (UINT32_C(1) << FS_UI_STATUS_HW);
    }
}

static __attribute__((noinline)) int fs_build_ui_control(uint32_t id,
                                                         const FsModuleState *st,
                                                         FxUiControl *out) {
    if (!st || !out || id < FS_UI_PAGE_ROOT || id > FS_UI_CTRL_COUNT) {
        return 0;
    }
    const Z8FilmSimConfig *cfg = &st->cfg;
    uint32_t lang = st->ui_lang;
    fs_zero(out, (uint32_t)sizeof(*out));
    out->abi = FX_PLATFORM_ABI;
    out->bytes = (uint32_t)sizeof(*out);
    out->id = id;
    out->flags = FX_UI_VISIBLE | FX_UI_ENABLED;

    switch (id) {
    case FS_UI_PAGE_ROOT:
        out->parent = 0u;
        out->kind = FX_UI_PAGE;
        if (st->ui_caps & FX_UI_CAP_ROOT) {
            out->flags |= FX_UI_ROOT;
        }
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "Film Simulation", "胶片模拟 / HDF", "膠片模擬 / HDF"));
        return 1;

    case FS_UI_APPLY_MODE:
        out->parent = FS_UI_PAGE_ROOT;
        out->kind = FX_UI_CHOICE;
        out->option_count = 2u;
        out->value = (cfg->apply_mode <= Z8_FS_APPLY_PER_NP3)
                         ? cfg->apply_mode
                         : Z8_FS_APPLY_GLOBAL;
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "Apply Mode", "生效模式", "生效模式"));
        fs_str_copy(out->options[0], 32u,
                    fs_tr(lang, "Global", "全局生效", "全域生效"));
        fs_str_copy(out->options[1], 32u,
                    fs_tr(lang, "Per-NP3", "按优化校准", "按優化校準"));
        return 1;

    case FS_UI_ZF_INTENSITY: {
        uint32_t eff_int = ((cfg->film_style == Z8_STYLE_NIKON_ZF ||
                             cfg->film_style == Z8_STYLE_FUJI_ZF) &&
                            cfg->zf_intensity <= Z8_ZF_INTENSITY_MAX)
                               ? cfg->zf_intensity
                               : 0u;
        out->parent = FS_UI_PAGE_ROOT;
        out->kind = FX_UI_CHOICE;
        out->option_count = 7u;
        out->value = eff_int;
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "Grain Intensity", "颗粒强度", "顆粒強度"));
        fs_str_copy(out->options[0], 32u, fs_tr(lang, "Off", "关闭", "關閉"));
        fs_str_copy(out->options[1], 32u, "1");
        fs_str_copy(out->options[2], 32u, "2");
        fs_str_copy(out->options[3], 32u, "3");
        fs_str_copy(out->options[4], 32u, "4");
        fs_str_copy(out->options[5], 32u, "5");
        fs_str_copy(out->options[6], 32u, "6");
        return 1;
    }

    case FS_UI_ZF_SIZE:
        out->parent = FS_UI_PAGE_ROOT;
        out->kind = FX_UI_CHOICE;
        out->option_count = 3u;
        out->value = (cfg->zf_size >= Z8_ZF_SIZE_MIN && cfg->zf_size <= Z8_ZF_SIZE_MAX)
                         ? (uint32_t)(cfg->zf_size - 1u)
                         : (uint32_t)(Z8_ZF_SIZE_DEFAULT - 1u);
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "Grain Size", "颗粒大小", "顆粒大小"));
        fs_str_copy(out->options[0], 32u, fs_tr(lang, "1 (Small)", "1 (小)", "1 (小)"));
        fs_str_copy(out->options[1], 32u, fs_tr(lang, "2 (Medium)", "2 (中)", "2 (中)"));
        fs_str_copy(out->options[2], 32u, fs_tr(lang, "3 (Large)", "3 (大)", "3 (大)"));
        return 1;

    case FS_UI_ZF_BLUR:
        out->parent = FS_UI_PAGE_ROOT;
        out->kind = FX_UI_CHOICE;
        out->option_count = 2u;
        out->value = cfg->zf_blur_mode <= Z8_ZF_BLUR_SHARP
                         ? cfg->zf_blur_mode
                         : Z8_ZF_BLUR_DEFAULT;
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "Grain Texture", "颗粒质感", "顆粒質感"));
        fs_str_copy(out->options[0], 32u, fs_tr(lang, "Soft (Zf)", "柔和 (Zf)", "柔和 (Zf)"));
        fs_str_copy(out->options[1], 32u, fs_tr(lang, "Sharp", "清晰", "清晰"));
        return 1;

    case FS_UI_HDF_LEVEL:
        out->parent = FS_UI_PAGE_ROOT;
        out->kind = FX_UI_CHOICE;
        out->option_count = 4u;
        out->value = cfg->hdf_level <= Z8_HDF_STRONG ? cfg->hdf_level : 0u;
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "HDF Level", "高光柔焦 HDF", "高光柔焦 HDF"));
        fs_str_copy(out->options[0], 32u, fs_tr(lang, "Off", "关闭", "關閉"));
        fs_str_copy(out->options[1], 32u, fs_tr(lang, "Weak (1/8)", "弱 (1/8)", "弱 (1/8)"));
        fs_str_copy(out->options[2], 32u, fs_tr(lang, "Medium (1/4)", "中 (1/4)", "中 (1/4)"));
        fs_str_copy(out->options[3], 32u, fs_tr(lang, "Strong (1/2)", "强 (1/2)", "強 (1/2)"));
        return 1;

    case FS_UI_LIVE_LINK:
        out->parent = FS_UI_PAGE_ROOT;
        out->kind = FX_UI_SWITCH;
        out->option_count = 0u;
        out->value = cfg->live_link <= Z8_LIVE_LINK_ON ? cfg->live_link : 0u;
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "Live Photo Link", "实况照片联动", "實況照片聯動"));
        return 1;

    case FS_UI_WYSIWYG_PREVIEW:
        out->parent = FS_UI_PAGE_ROOT;
        out->kind = FX_UI_SWITCH;
        out->option_count = 0u;
        out->value = cfg->wysiwyg_preview <= Z8_WYSIWYG_ON ? cfg->wysiwyg_preview : 0u;
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "WYSIWYG Preview", "取景实时预览", "取景即時預覽"));
        return 1;

    case FS_UI_BIND_NP3:
        out->parent = FS_UI_PAGE_ROOT;
        out->kind = FX_UI_ACTION;
        out->option_count = 0u;
        out->value = 0u;
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "Bind Active NP3", "绑定当前配方", "關聯目前配方"));
        fs_format_bind_text(st, out->text);
        return 1;

    case FS_UI_CLEAR_NP3:
        out->parent = FS_UI_PAGE_ROOT;
        out->kind = FX_UI_ACTION;
        out->option_count = 0u;
        out->value = 0u;
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "Clear Active NP3", "清除当前配方", "清除目前配方"));
        fs_format_clear_text(st, out->text);
        return 1;

    case FS_UI_DEBUG_PAGE:
        out->parent = FS_UI_PAGE_ROOT;
        out->kind = FX_UI_PAGE;
        out->option_count = 0u;
        out->value = 0u;
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "Debug Settings", "调试设置", "調試設定"));
        return 1;

    case FS_UI_XM6_MODE:
        out->parent = FS_UI_DEBUG_PAGE;
        out->kind = FX_UI_CHOICE;
        out->option_count = 2u;
        out->value = (cfg->xm6_mode <= Z8_XM6_MODE_ARM_ONLY)
                         ? cfg->xm6_mode
                         : Z8_XM6_MODE_AUTO;
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "XM6 DSP Mode", "XM6 DSP 模式", "XM6 DSP 模式"));
        fs_str_copy(out->options[0], 32u,
                    fs_tr(lang, "Auto", "自动", "自動"));
        fs_str_copy(out->options[1], 32u,
                    fs_tr(lang, "CPU Only", "仅 CPU", "僅 CPU"));
        return 1;

    case FS_UI_XM6_DIAG:
        out->parent = FS_UI_DEBUG_PAGE;
        out->kind = FX_UI_ACTION;
        out->option_count = 0u;
        out->value = 0u;
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "Run XM6 Self-Test", "运行 XM6 自检", "執行 XM6 自檢"));
        fs_format_xm6_diag(st, out->text);
        return 1;

    case FS_UI_XM6_RESET:
        out->parent = FS_UI_DEBUG_PAGE;
        out->kind = FX_UI_ACTION;
        out->option_count = 0u;
        out->value = 0u;
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "Reset XM6 Breaker", "重置 XM6 熔断器", "重設 XM6 熔斷器"));
        fs_format_xm6_reset(st, out->text);
        return 1;

    case FS_UI_STATUS_HW:
        out->parent = FS_UI_DEBUG_PAGE;
        out->kind = FX_UI_ACTION;
        out->option_count = 0u;
        out->value = 0u;
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "Hardware & Runtime", "硬件与运行状态", "硬體與運行狀態"));
        fs_format_status_hw(st, out->text);
        return 1;

    case FS_UI_RESET_DEFAULTS:
        out->parent = FS_UI_DEBUG_PAGE;
        out->kind = (st->ui_caps & FX_UI_CAP_MODAL) ? FX_UI_DIALOG : FX_UI_ACTION;
        out->option_count = 0u;
        out->value = 0u;
        fs_str_copy(out->label, FX_UI_TEXT,
                    fs_tr(lang, "Reset Defaults", "恢复默认设置", "還原預設設定"));
        fs_str_copy(out->text, FX_UI_TEXT,
                    fs_tr(lang, "Restore defaults?", "确认恢复默认设置？", "確認還原預設設定？"));
        return 1;

    default:
        return 0;
    }
}

static inline int fs_apply_ui_change(Z8FilmSimConfig *cfg, uint32_t control_id,
                                     uint32_t value, int *changed) {
    if (!cfg || !changed) {
        return 0;
    }
    *changed = 0;
    switch (control_id) {
    case FS_UI_APPLY_MODE:
        if (value > Z8_FS_APPLY_PER_NP3) return 0;
        if (cfg->apply_mode != (uint8_t)value) {
            cfg->apply_mode = (uint8_t)value;
            *changed = 1;
        }
        return 1;

    case FS_UI_ZF_INTENSITY: {
        if (value > Z8_ZF_INTENSITY_MAX) return 0;
        uint8_t next_style = (value > 0u) ? (uint8_t)Z8_STYLE_NIKON_ZF : (uint8_t)Z8_STYLE_OFF;
        if (cfg->zf_intensity != (uint8_t)value || cfg->film_style != next_style) {
            cfg->zf_intensity = (uint8_t)value;
            cfg->film_style = next_style;
            *changed = 1;
        }
        return 1;
    }

    case FS_UI_ZF_SIZE:
        if (value > 2u) return 0;
        if (cfg->zf_size != (uint8_t)(value + 1u)) {
            cfg->zf_size = (uint8_t)(value + 1u);
            *changed = 1;
        }
        return 1;

    case FS_UI_ZF_BLUR:
        if (value > Z8_ZF_BLUR_SHARP) return 0;
        if (cfg->zf_blur_mode != (uint8_t)value) {
            cfg->zf_blur_mode = (uint8_t)value;
            *changed = 1;
        }
        return 1;

    case FS_UI_HDF_LEVEL:
        if (value > Z8_HDF_STRONG) return 0;
        if (cfg->hdf_level != (uint8_t)value) {
            cfg->hdf_level = (uint8_t)value;
            *changed = 1;
        }
        return 1;

    case FS_UI_LIVE_LINK:
        if (value > Z8_LIVE_LINK_ON) return 0;
        if (cfg->live_link != (uint8_t)value) {
            cfg->live_link = (uint8_t)value;
            *changed = 1;
        }
        return 1;

    case FS_UI_WYSIWYG_PREVIEW:
        if (value > Z8_WYSIWYG_ON) return 0;
        if (cfg->wysiwyg_preview != (uint8_t)value) {
            cfg->wysiwyg_preview = (uint8_t)value;
            *changed = 1;
        }
        return 1;

    case FS_UI_XM6_MODE:
        if (value > Z8_XM6_MODE_ARM_ONLY) return 0;
        if (cfg->xm6_mode != (uint8_t)value) {
            cfg->xm6_mode = (uint8_t)value;
            *changed = 1;
        }
        return 1;

    default:
        return 0;
    }
}

static inline void fs_commit_cfg_change(FsModuleState *st,
                                        const Z8FilmSimConfig *next_fields,
                                        uint32_t dirty_controls_mask) {
    Z8FilmSimConfig next = *next_fields;
    uint32_t next_seq = (st->cfg.seq < UINT32_MAX) ? (st->cfg.seq + 1u) : st->cfg.seq;
    fs_cfg_finalize(&next, next_seq);
    st->cfg = next;
    if (!st->save_pending) {
        st->save_open_fails = 0u;
        st->save_use_fallback_slot = 0u;
    }
    st->save_pending = 1u;
    st->save_retries = 0u;
    st->ui_dirty_mask |= dirty_controls_mask;
    if (st->has_fs) {
        st->fs_sync_needed = 1u;
    } else if (st->has_bus) {
        st->bus_sync_needed = 1u;
    }
    fs_mark_status_dirty_if_changed(st);
}

static inline void fs_execute_np3_op(FsModuleState *st, uint32_t op) {
    uint32_t sel = fs_effective_active_sel(st);
    uint32_t slot = z8_np3_selector_to_slot(sel);
    if (slot == Z8_NP3_SLOT_INVALID) {
        fs_mark_status_dirty_if_changed(st);
        return;
    }

    Z8Np3Recipe r;
    if (op == FS_NP3_OP_BIND) {
        z8_np3_recipe_from_cfg(&st->cfg, &r);
        z8_np3_recipe_to_entry(&r, &st->recipes.entries[slot]);
        ++st->binds_completed;
    } else {
        fs_zero(&r, (uint32_t)sizeof(r));
        r.magic = Z8_NP3_RECIPE_MAGIC;
        r.version = (uint8_t)Z8_NP3_RECIPE_VERSION;
        r.flags = 0u;
        r.zf_size = (uint8_t)Z8_ZF_SIZE_DEFAULT;
        r.crc32 = z8_np3_recipe_crc32(&r);
        fs_zero(&st->recipes.entries[slot], (uint32_t)sizeof(Z8Np3SlotEntry));
        ++st->clears_completed;
    }

    uint32_t next_seq = (st->recipes.seq < UINT32_MAX)
                            ? (st->recipes.seq + 1u)
                            : st->recipes.seq;
    z8_np3_table_finalize(&st->recipes, next_seq);
    st->bound_count = fs_count_bound_recipes(&st->recipes);

    if (!st->rec_save_pending) {
        st->rec_save_open_fails = 0u;
        st->rec_save_use_fallback = 0u;
    }
    st->rec_save_pending = 1u;
    st->rec_save_retries = 0u;

    /* If active slot is C10..C64, also mirror into SC_RECORD +0x39C via slots.state */
    uint32_t sc_idx = 0u;
    if (z8_np3_selector_is_c64(sel, &sc_idx) && st->has_slots) {
        st->sc_rmw_pending = 1u;
        st->sc_rmw_clear = (op == FS_NP3_OP_CLEAR) ? 1u : 0u;
        st->sc_rmw_step = 0u;
        st->sc_rmw_index = sc_idx;
        st->sc_rmw_recipe = r;
        if (op == FS_NP3_OP_BIND) {
            st->active_sc_recipe = r;
            st->active_sc_recipe_valid = 1u;
        } else {
            fs_zero(&st->active_sc_recipe, (uint32_t)sizeof(st->active_sc_recipe));
            st->active_sc_recipe_valid = 0u;
        }
    }

    /* Push single-slot recipe update to EL1 (and bus refresh after card commit) */
    if (st->has_fs && st->has_fs_recipe_ops) {
        st->fs_recipe_msg.selector = sel;
        st->fs_recipe_msg.slot_index = slot;
        st->fs_recipe_msg.recipe = r;
        st->fs_recipe_sync_pending = 1u;
    }
    if (!st->card_write_ok && st->has_bus) {
        st->bus_sync_needed = 1u;
    }
    st->ui_dirty_mask |= (UINT32_C(1) << FS_UI_BIND_NP3) |
                         (UINT32_C(1) << FS_UI_CLEAR_NP3) |
                         (UINT32_C(1) << FS_UI_STATUS_HW);
    fs_mark_status_dirty_if_changed(st);
}

static inline void fs_handle_save_failure(FsModuleState *st, int open_failed) {
    ++st->saves_failed;
    if (open_failed && !st->save_use_fallback_slot) {
        if (st->save_open_fails < 255u) {
            ++st->save_open_fails;
        }
        if (st->save_open_fails >= FS_SAVE_OPEN_FALLBACK_CNT) {
            st->save_use_fallback_slot = 1u;
        }
    }
    if (st->save_retries < FS_SAVE_MAX_RETRIES) {
        ++st->save_retries;
        st->save_fail_ms = st->now_ms;
        st->save_pending = 1u;
    } else {
        st->save_pending = 0u;
    }
    st->stage = FS_ST_IDLE;
}

static inline void fs_handle_rec_save_failure(FsModuleState *st, int open_failed) {
    ++st->rec_saves_failed;
    if (open_failed && !st->rec_save_use_fallback) {
        if (st->rec_save_open_fails < 255u) {
            ++st->rec_save_open_fails;
        }
        if (st->rec_save_open_fails >= FS_SAVE_OPEN_FALLBACK_CNT) {
            st->rec_save_use_fallback = 1u;
        }
    }
    if (st->rec_save_retries < FS_SAVE_MAX_RETRIES) {
        ++st->rec_save_retries;
        st->rec_save_fail_ms = st->now_ms;
        st->rec_save_pending = 1u;
    } else {
        st->rec_save_pending = 0u;
        if (st->has_bus) {
            st->bus_sync_needed = 1u;
        }
    }
    st->stage = FS_ST_IDLE;
}

static inline FxResult fs_submit_mem_write(FsModuleState *st,
                                           const FxServiceModule *api,
                                           uint32_t offset,
                                           const void *data,
                                           uint32_t bytes) {
    uint8_t payload[sizeof(FxMemoryRequest) + FS_MEM_CHUNK_MAX];
    if (bytes > FS_MEM_CHUNK_MAX) {
        return FX_BOUNDS;
    }
    FxMemoryRequest req = {.handle = st->buf_handle, .bytes = bytes, .offset = offset};
    fs_copy(payload, &req, (uint32_t)sizeof(req));
    fs_copy(payload + sizeof(req), data, bytes);
    return fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->mem_binding,
                           FX_MEMORY_WRITE, 0u, payload,
                           (uint32_t)sizeof(req) + bytes);
}

static inline FxResult fs_submit_mem_read(FsModuleState *st,
                                          const FxServiceModule *api,
                                          uint32_t offset,
                                          uint32_t bytes) {
    FxMemoryRequest req = {.handle = st->buf_handle, .bytes = bytes, .offset = offset};
    return fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->mem_binding,
                           FX_MEMORY_READ, 0u, &req, (uint32_t)sizeof(req));
}

static inline void fs_parse_status_reply(FsModuleState *st,
                                         const FxServiceReply *r) {
    if (r->result != FX_OK) {
        return;
    }
    ++st->fs_status_completed;
    if (r->bytes == sizeof(Z8FilmSimStatus)) {
        Z8FilmSimStatus tmp;
        fs_copy(&tmp, r->data, (uint32_t)sizeof(tmp));
        if (fs_status_is_valid(&tmp)) {
            st->el1_status = tmp;
            st->el1_status_valid = 1u;
            if (tmp.active_drive == 'A' || tmp.active_drive == 'B') {
                st->active_drive = (char)tmp.active_drive;
            }
            if (!st->has_slots &&
                z8_np3_selector_to_slot(tmp.active_pc_sel) != Z8_NP3_SLOT_INVALID) {
                st->active_pc_sel = tmp.active_pc_sel;
                st->active_pc_valid = 1u;
            }
            fs_mark_status_dirty_if_changed(st);
            return;
        }
    }
    /* Minimal binding fallback: accept flags in value[0] if no 64B struct */
    if (r->bytes == 0u) {
        Z8FilmSimStatus tmp;
        fs_zero(&tmp, (uint32_t)sizeof(tmp));
        tmp.magic = Z8_FILMSIM_CFG_MAGIC;
        tmp.version = (uint16_t)Z8_FILMSIM_CFG_VERSION;
        tmp.bytes = (uint16_t)Z8_FILMSIM_STATUS_BYTES;
        tmp.flags = (uint32_t)r->value[0];
        st->el1_status = tmp;
        st->el1_status_valid = 1u;
        fs_mark_status_dirty_if_changed(st);
    }
}

static inline int fs_check_event_revision(FsModuleState *st,
                                          const FxUiEvent *ev) {
    if (!(st->ui_caps & FX_UI_CAP_EVENT_REVISION)) {
        return 1;
    }
    if (ev->control < FS_UI_PAGE_ROOT || ev->control > FS_UI_CTRL_COUNT) {
        return 0;
    }
    uint32_t ev_rev = fx_sdk_event_revision(ev);
    uint32_t expected_rev = st->ctrl_rev[ev->control];
    if (ev_rev != 0u && expected_rev != 0u && ev_rev != expected_rev) {
        ++st->stale_events_ignored;
        return 0;
    }
    return 1;
}

static inline void fs_handle_ui_event(FsModuleState *st,
                                      const FxServiceReply *r) {
    if (r->bytes != sizeof(FxUiEvent)) {
        return;
    }
    FxUiEvent ev;
    fs_copy(&ev, r->data, (uint32_t)sizeof(ev));

    if (ev.event == FX_INPUT_FOCUS) {
        st->focus_foreground = 1u;
        st->focus_epoch = ev.reserved;
        if (st->has_fs) {
            st->status_refresh_needed = 1u;
        }
        if (st->has_lang_svc) {
            st->lang_refresh_needed = 1u;
        }
        if (st->has_slots) {
            st->pc_refresh_needed = 1u;
        }
        return;
    }
    if (ev.event == FX_INPUT_BLUR) {
        st->focus_foreground = 0u;
        st->focus_epoch = ev.reserved;
        return;
    }

    if (ev.control < FS_UI_APPLY_MODE || ev.control > FS_UI_CTRL_COUNT) {
        return;
    }
    if (!fs_check_event_revision(st, &ev)) {
        return;
    }

    /* Bind current Zf Grain + HDF + Live Link to active NP3 / Picture Control */
    if (ev.control == FS_UI_BIND_NP3 && ev.event == FX_INPUT_ACTIVATE) {
        if (st->has_slots) {
            st->pending_np3_op = FS_NP3_OP_BIND;
            st->pc_refresh_needed = 1u;
        } else {
            fs_execute_np3_op(st, FS_NP3_OP_BIND);
        }
        return;
    }

    /* Clear recipe binding on active NP3 / Picture Control */
    if (ev.control == FS_UI_CLEAR_NP3 && ev.event == FX_INPUT_ACTIVATE) {
        if (st->has_slots) {
            st->pending_np3_op = FS_NP3_OP_CLEAR;
            st->pc_refresh_needed = 1u;
        } else {
            fs_execute_np3_op(st, FS_NP3_OP_CLEAR);
        }
        return;
    }

    /* Reset Defaults dialog accept or action activate */
    if (ev.control == FS_UI_RESET_DEFAULTS &&
        (ev.event == FX_INPUT_ACCEPT || ev.event == FX_INPUT_ACTIVATE)) {
        Z8FilmSimConfig next = st->cfg;
        fs_cfg_set_default_fields(&next);
        if (!fs_mem_eq(&next.film_style, &st->cfg.film_style, 14u)) {
            uint32_t all_settings = 0u;
            for (uint32_t id = FS_UI_APPLY_MODE; id <= FS_UI_WYSIWYG_PREVIEW; ++id) {
                all_settings |= (UINT32_C(1) << id);
            }
            all_settings |= (UINT32_C(1) << FS_UI_XM6_MODE);
            ++st->resets_completed;
            fs_commit_cfg_change(st, &next, all_settings);
        }
        return;
    }

    /* Run on-device XM6 0-diff self-test (auto-resetting tripped breaker first) */
    if (ev.control == FS_UI_XM6_DIAG && ev.event == FX_INPUT_ACTIVATE) {
        if (st->has_fs) {
            if (!st->xm6_diag_pending && !st->xm6_reset_pending) {
                st->xm6_diag_unsupported = 0u;
                st->xm6_diag_retried_reset = 0u;
                if (st->el1_status_valid &&
                    (st->el1_status.xm6_runtime_flags & Z8_XM6_FLAG_BREAKER_TRIPPED)) {
                    st->xm6_reset_pending = 1u;
                }
                st->xm6_diag_pending = 1u;
            }
        } else {
            fs_mark_status_dirty_if_changed(st);
        }
        return;
    }

    /* Explicit Reset XM6 Circuit Breaker action (Debug Sub-Page) */
    if (ev.control == FS_UI_XM6_RESET && ev.event == FX_INPUT_ACTIVATE) {
        if (st->has_fs) {
            st->xm6_reset_pending = 1u;
        } else {
            fs_mark_status_dirty_if_changed(st);
        }
        return;
    }

    /* Manual refresh on Hardware & Runtime Status row */
    if (ev.control == FS_UI_STATUS_HW && ev.event == FX_INPUT_ACTIVATE) {
        if (st->has_fs) {
            st->status_refresh_needed = 1u;
        } else {
            fs_mark_status_dirty_if_changed(st);
        }
        if (st->has_slots) {
            st->pc_refresh_needed = 1u;
        }
        return;
    }

    /* Standard choice / switch change */
    if (ev.event == FX_INPUT_CHANGE) {
        Z8FilmSimConfig next = st->cfg;
        int changed = 0;
        if (!fs_apply_ui_change(&next, ev.control, ev.value, &changed)) {
            return;
        }
        st->ui_sync_mask |= (UINT32_C(1) << ev.control);
        if (changed) {
            fs_commit_cfg_change(st, &next, 0u);
        }
    }
}

static inline void fs_start_after_recipe_load(FsModuleState *st) {
    int v0 = st->slot_state[0] == FS_SLOT_VALID;
    int v1 = st->slot_state[1] == FS_SLOT_VALID;
    if (st->has_fs) {
        st->stage = (!v0 && !v1) ? FS_ST_FS_GET_INIT : FS_ST_FS_PUSH_INIT;
    } else if (st->has_bus) {
        st->bus_init_phase = 1u;
        st->bus_send_retries = 0u;
        st->stage = FS_ST_BUS_SEND;
    } else {
        st->stage = FS_ST_UI_BUILD;
    }
}

static inline void fs_start_after_binding_probe(FsModuleState *st) {
    z8_np3_table_init_empty(&st->recipes);
    st->rec_active_slot = 1u; /* First save targets FLMREC0.CFG */
    st->rec_best_valid = 0u;
    st->bound_count = 0u;
    if (st->card_read_ok) {
        st->load_slot = 0u;
        st->stage = FS_ST_LOAD_WRITE_PATH;
        return;
    }
    fs_cfg_default(&st->cfg);
    st->active_slot = 1u;
    fs_start_after_recipe_load(st);
}

static inline void fs_finish_card_load(FsModuleState *st) {
    int v0 = st->slot_state[0] == FS_SLOT_VALID;
    int v1 = st->slot_state[1] == FS_SLOT_VALID;
    int io_err = (st->slot_state[0] == FS_SLOT_IO_ERR ||
                  st->slot_state[1] == FS_SLOT_IO_ERR);

    if (!v0 && !v1 && io_err && st->load_retries < FS_LOAD_MAX_RETRIES) {
        ++st->load_retries;
        st->load_slot = 0u;
        st->slot_state[0] = FS_SLOT_UNREAD;
        st->slot_state[1] = FS_SLOT_UNREAD;
        st->stage = FS_ST_LOAD_WRITE_PATH;
        return;
    }

    fs_cfg_select_dual(&st->slot_cfg[0], v0, &st->slot_cfg[1], v1,
                       &st->cfg, &st->active_slot);

    /* Proceed to load dual-slot FLMREC0/1.CFG (non-fatal if absent) */
    st->rec_load_slot = 0u;
    st->rec_load_retries = 0u;
    st->rec_slot_state[0] = FS_SLOT_UNREAD;
    st->rec_slot_state[1] = FS_SLOT_UNREAD;
    st->rec_best_valid = 0u;
    z8_np3_table_init_empty(&st->recipes);
    st->rec_active_slot = 1u;
    st->stage = FS_ST_REC_LOAD_WRITE_PATH;
}

static inline void fs_advance_after_slot_load(FsModuleState *st) {
    if (st->load_slot == 0u) {
        st->load_slot = 1u;
        st->stage = FS_ST_LOAD_WRITE_PATH;
    } else {
        fs_finish_card_load(st);
    }
}

static inline void fs_finish_rec_load(FsModuleState *st) {
    int io_err = (st->rec_slot_state[0] == FS_SLOT_IO_ERR ||
                  st->rec_slot_state[1] == FS_SLOT_IO_ERR);
    if (!st->rec_best_valid && io_err && st->rec_load_retries < FS_LOAD_MAX_RETRIES) {
        ++st->rec_load_retries;
        st->rec_load_slot = 0u;
        st->rec_slot_state[0] = FS_SLOT_UNREAD;
        st->rec_slot_state[1] = FS_SLOT_UNREAD;
        st->stage = FS_ST_REC_LOAD_WRITE_PATH;
        return;
    }
    st->bound_count = fs_count_bound_recipes(&st->recipes);
    fs_start_after_recipe_load(st);
}

static inline void fs_advance_after_rec_load(FsModuleState *st) {
    if (st->rec_load_slot == 0u) {
        st->rec_load_slot = 1u;
        st->stage = FS_ST_REC_LOAD_WRITE_PATH;
    } else {
        fs_finish_rec_load(st);
    }
}

static inline uint32_t fs_lowest_ctrl_bit(uint32_t mask) {
    for (uint32_t id = FS_UI_PAGE_ROOT; id <= FS_UI_CTRL_COUNT; ++id) {
        if (mask & (UINT32_C(1) << id)) {
            return id;
        }
    }
    return 0u;
}

static inline void fs_update_active_pc_sel(FsModuleState *st, uint32_t sel) {
    uint32_t changed = (!st->active_pc_valid || st->active_pc_sel != sel) ? 1u : 0u;
    st->active_pc_sel = sel;
    st->active_pc_valid = 1u;
    if (changed) {
        st->active_sc_recipe_valid = 0u;
        fs_zero(&st->active_sc_recipe, (uint32_t)sizeof(st->active_sc_recipe));
        uint32_t sc_idx = 0u;
        if (z8_np3_selector_is_c64(sel, &sc_idx) && st->has_slots) {
            st->sc_tail_read_needed = 1u;
        }
        fs_mark_status_dirty_if_changed(st);
    }
}

/* A2 Hardening: recover gracefully from any transient IPC/bus/file/take error
 * without ever returning a non-zero exit code (which triggers FAULT (5)). */
static inline void fs_recover_from_transient_error(FsModuleState *st) {
    ++st->transient_errors;
    st->sdk.pending = 0u;

    if (st->ui_built) {
        /* If a card file handle was left open during a transient submit/take error,
         * close it cleanly before returning to IDLE. */
        if (st->card_fd >= 0) {
            if (st->stage >= FS_ST_SAVE_WRITE_PATH && st->stage <= FS_ST_SAVE_CLOSE) {
                st->io_err = 1u;
                st->stage = FS_ST_SAVE_CLOSE;
                return;
            }
            if (st->stage >= FS_ST_REC_SAVE_WRITE_PATH && st->stage <= FS_ST_REC_SAVE_CLOSE) {
                st->io_err = 1u;
                st->stage = FS_ST_REC_SAVE_CLOSE;
                return;
            }
            if (st->stage >= FS_ST_XM6_LOG_WRITE_PATH && st->stage <= FS_ST_XM6_LOG_CLOSE) {
                st->io_err = 1u;
                st->stage = FS_ST_XM6_LOG_CLOSE;
                return;
            }
            st->card_fd = -1;
        }
        if (st->stage >= FS_ST_SAVE_WRITE_PATH && st->stage <= FS_ST_SAVE_CLOSE) {
            fs_handle_save_failure(st, st->stage == FS_ST_SAVE_OPEN);
            return;
        }
        if (st->stage >= FS_ST_REC_SAVE_WRITE_PATH && st->stage <= FS_ST_REC_SAVE_CLOSE) {
            fs_handle_rec_save_failure(st, st->stage == FS_ST_REC_SAVE_OPEN);
            return;
        }
        if (st->stage >= FS_ST_XM6_LOG_WRITE_PATH && st->stage <= FS_ST_XM6_LOG_CLOSE) {
            ++st->xm6_logs_failed;
            st->stage = FS_ST_IDLE;
            return;
        }
        if (st->stage == FS_ST_UI_UPDATE || st->stage == FS_ST_UI_SYNC_GET) {
            if (st->ui_active_ctrl <= FS_UI_CTRL_COUNT) {
                st->ui_dirty_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
                st->ui_sync_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
            }
            st->ui_active_ctrl = 0u;
        }
        st->stage = FS_ST_IDLE;
        return;
    }

    /* Boot-phase recovery: degrade gracefully without exiting non-zero */
    switch (st->stage) {
    case FS_ST_QUERY_UI:
        st->ui_caps = 0u;
        st->stage = FS_ST_OPEN_SLOTS;
        break;
    case FS_ST_OPEN_SLOTS:
    case FS_ST_LANG_INIT:
    case FS_ST_PC_SEL_INIT:
        st->stage = FS_ST_OPEN_MEM;
        break;
    case FS_ST_OPEN_MEM:
    case FS_ST_ALLOC_MEM:
        st->has_bounce = 0u;
        st->stage = FS_ST_OPEN_FS;
        break;
    case FS_ST_OPEN_CARD_OPEN:
    case FS_ST_OPEN_CARD_READ:
    case FS_ST_OPEN_CARD_WRITE:
    case FS_ST_OPEN_CARD_CLOSE:
        st->card_read_ok = 0u;
        st->card_write_ok = 0u;
        st->stage = FS_ST_OPEN_FS;
        break;
    case FS_ST_OPEN_FS:
    case FS_ST_FS_STATUS_INIT:
        st->has_fs = 0u;
        st->fs_via_slots = 0u;
        st->fs_binding = 0u;
        st->stage = FS_ST_OPEN_BUS;
        break;
    case FS_ST_OPEN_BUS:
    case FS_ST_BUS_PUBLISH:
    case FS_ST_BUS_RESERVE:
        st->has_bus = 0u;
        fs_start_after_binding_probe(st);
        break;
    case FS_ST_LOAD_WRITE_PATH:
    case FS_ST_LOAD_OPEN:
    case FS_ST_LOAD_READ_CALL:
    case FS_ST_LOAD_READ_DONE:
    case FS_ST_LOAD_CLOSE:
    case FS_ST_LOAD_READ_DATA:
        st->card_fd = -1;
        st->slot_state[st->load_slot] = FS_SLOT_IO_ERR;
        fs_advance_after_slot_load(st);
        break;
    case FS_ST_REC_LOAD_WRITE_PATH:
    case FS_ST_REC_LOAD_OPEN:
    case FS_ST_REC_LOAD_READ_CALL:
    case FS_ST_REC_LOAD_READ_DONE:
    case FS_ST_REC_LOAD_CLOSE:
    case FS_ST_REC_LOAD_READ_CHUNK:
        st->card_fd = -1;
        st->rec_slot_state[st->rec_load_slot] = FS_SLOT_IO_ERR;
        fs_advance_after_rec_load(st);
        break;
    case FS_ST_FS_GET_INIT:
    case FS_ST_FS_PUSH_INIT:
    case FS_ST_FS_SYNC_REC_CHUNK:
    case FS_ST_FS_STATUS_POST_INIT:
    case FS_ST_BUS_SEND:
    case FS_ST_BUS_POLL_STALE:
        st->bus_init_phase = 0u;
        st->stage = FS_ST_UI_BUILD;
        break;
    case FS_ST_UI_BUILD:
        ++st->ui_build_retries;
        if (st->ui_build_retries > FS_UI_BUILD_MAX_RETRIES) {
            st->ui_build_retries = 0u;
            ++st->ui_build_idx;
            if (st->ui_build_idx >= FS_UI_CTRL_COUNT) {
                st->ui_built = 1u;
                st->stage = FS_ST_IDLE;
            }
        }
        break;
    default:
        st->stage = FS_ST_IDLE;
        st->ui_built = 1u;
        break;
    }
}

static inline int fs_handle_reply(FsModuleState *st, const FxServiceReply *r) {
    if ((st->stage == FS_ST_BUS_SEND || st->stage == FS_ST_BUS_POLL_STALE) &&
        r->result == FX_BUSY) {
        ++st->bus_send_retries;
        if (st->stage == FS_ST_BUS_SEND) {
            if (st->bus_send_retries == 1u) {
                return 0; /* First FX_BUSY may be lock contention: retry send once */
            }
            if (st->bus_send_retries < 8u) {
                st->stage = FS_ST_BUS_POLL_STALE;
                return 0;
            }
        } else if (st->bus_send_retries < 8u) {
            return 0; /* Retry FX_BUS_POLL on lock contention */
        }
        if (st->bus_init_phase) {
            st->bus_init_phase = 0u;
            st->stage = FS_ST_UI_BUILD;
        } else {
            st->stage = FS_ST_IDLE;
        }
        return 0;
    }
    if (r->result == FX_BUSY) {
        return 0; /* Retry current stage unchanged */
    }

    switch (st->stage) {
    case FS_ST_QUERY_UI:
        if (r->result == FX_OK && r->bytes == sizeof(FxUiInfo)) {
            FxUiInfo info;
            fs_copy(&info, r->data, (uint32_t)sizeof(info));
            if (info.abi == 1u && info.bytes == sizeof(info)) {
                st->ui_caps = info.capabilities;
            }
        } else {
            st->ui_caps = 0u;
            if (r->result != FX_ARGUMENT && r->result != FX_CAPABILITY) {
                ++st->transient_errors;
            }
        }
        st->stage = FS_ST_OPEN_SLOTS;
        return 0;

    case FS_ST_OPEN_SLOTS:
        if (r->result == FX_OK) {
            st->slots_binding = r->value[0];
            st->has_slots = 1u;
            st->stage = FS_ST_LANG_INIT;
        } else {
            st->slots_binding = 0u;
            st->has_slots = 0u;
            st->has_lang_svc = 0u;
            st->ui_lang = FS_LANG_EN;
            st->stage = FS_ST_OPEN_MEM;
        }
        return 0;

    case FS_ST_LANG_INIT:
        if (r->result == FX_OK && r->value[1] == FS_LANG_MAGIC) {
            st->has_lang_svc = 1u;
            st->ui_lang = (uint32_t)r->value[0];
            st->last_lang_ms = st->now_ms;
            ++st->lang_polls_completed;
        } else {
            st->has_lang_svc = 0u;
            st->ui_lang = FS_LANG_EN;
        }
        st->stage = FS_ST_PC_SEL_INIT;
        return 0;

    case FS_ST_PC_SEL_INIT:
        st->last_pc_ms = st->now_ms;
        if (r->result == FX_OK) {
            fs_update_active_pc_sel(st, (uint32_t)r->value[0]);
        }
        st->stage = FS_ST_OPEN_MEM;
        return 0;

    case FS_ST_OPEN_MEM:
        if (r->result == FX_OK) {
            st->mem_binding = r->value[0];
            st->stage = FS_ST_ALLOC_MEM;
        } else {
            st->mem_binding = 0u;
            st->has_bounce = 0u;
            st->stage = FS_ST_OPEN_FS;
        }
        return 0;

    case FS_ST_ALLOC_MEM:
        if (r->result == FX_OK && r->value[1] != 0u) {
            st->buf_handle = r->value[0];
            st->buf_native = r->value[1];
            st->has_bounce = 1u;
            st->stage = FS_ST_OPEN_CARD_OPEN;
        } else {
            st->has_bounce = 0u;
            st->stage = FS_ST_OPEN_FS;
        }
        return 0;

    case FS_ST_OPEN_CARD_OPEN:
        st->card_open_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->stage = FS_ST_OPEN_CARD_READ;
        return 0;

    case FS_ST_OPEN_CARD_READ:
        st->card_read_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->stage = FS_ST_OPEN_CARD_WRITE;
        return 0;

    case FS_ST_OPEN_CARD_WRITE:
        st->card_write_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->stage = FS_ST_OPEN_CARD_CLOSE;
        return 0;

    case FS_ST_OPEN_CARD_CLOSE:
        st->card_close_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->card_read_ok = (st->has_bounce && st->card_open_h &&
                            st->card_read_h && st->card_close_h) ? 1u : 0u;
        st->card_write_ok = (st->has_bounce && st->card_open_h &&
                             st->card_write_h && st->card_close_h) ? 1u : 0u;
        st->stage = FS_ST_OPEN_FS;
        return 0;

    case FS_ST_OPEN_FS:
        if (r->result == FX_OK) {
            st->fs_binding = r->value[0];
            st->fs_via_slots = 0u;
            st->has_fs = 1u;
            st->has_fs_recipe_ops = 1u;
            st->stage = FS_ST_FS_STATUS_INIT;
        } else if (st->has_slots) {
            /* V71.6-XM6 EL1 IPC tunnel over slots.state ops 32..47 ('FSI1' framing) */
            st->fs_binding = st->slots_binding;
            st->fs_via_slots = 1u;
            st->has_fs = 1u;
            st->has_fs_recipe_ops = 1u;
            st->stage = FS_ST_FS_STATUS_INIT;
        } else {
            st->fs_binding = 0u;
            st->fs_via_slots = 0u;
            st->has_fs = 0u;
            st->has_fs_recipe_ops = 0u;
            st->stage = FS_ST_OPEN_BUS;
        }
        return 0;

    case FS_ST_FS_STATUS_INIT:
        if (r->result == FX_OK) {
            fs_parse_status_reply(st, r);
            fs_start_after_binding_probe(st);
        } else {
            st->has_fs = 0u;
            st->fs_via_slots = 0u;
            st->has_fs_recipe_ops = 0u;
            st->fs_binding = 0u;
            st->el1_status_valid = 0u;
            st->stage = FS_ST_OPEN_BUS;
        }
        return 0;

    case FS_ST_OPEN_BUS:
        if (r->result == FX_OK) {
            st->bus_binding = r->value[0];
            st->stage = FS_ST_BUS_PUBLISH;
        } else {
            st->bus_binding = 0u;
            st->has_bus = 0u;
            fs_start_after_binding_probe(st);
        }
        return 0;

    case FS_ST_BUS_PUBLISH:
        if (r->result == FX_OK) {
            st->bus_endpoint = r->value[0];
            st->stage = FS_ST_BUS_RESERVE;
        } else {
            st->bus_endpoint = 0u;
            st->has_bus = 0u;
            fs_start_after_binding_probe(st);
        }
        return 0;

    case FS_ST_BUS_RESERVE:
        st->has_bus = (r->result == FX_OK) ? 1u : 0u;
        fs_start_after_binding_probe(st);
        return 0;

    /* ---- Boot load: FLMSIM0/1.CFG (64 B) ---- */
    case FS_ST_LOAD_WRITE_PATH:
        if (r->result != FX_OK) {
            st->slot_state[st->load_slot] = FS_SLOT_IO_ERR;
            fs_advance_after_slot_load(st);
        } else {
            st->card_fd = -1;
            st->io_done_acc = 0u;
            st->io_calls = 0u;
            st->io_err = 0u;
            st->stage = FS_ST_LOAD_OPEN;
        }
        return 0;

    case FS_ST_LOAD_OPEN:
        if (r->result != FX_OK) {
            st->slot_state[st->load_slot] = FS_SLOT_IO_ERR;
            fs_advance_after_slot_load(st);
        } else {
            int32_t fd = (int32_t)(uint32_t)r->value[0];
            if (fd >= 0) {
                st->card_fd = fd;
                st->stage = FS_ST_LOAD_READ_CALL;
            } else if ((uint32_t)fd == FS_OPEN_NOT_FOUND) {
                st->slot_state[st->load_slot] = FS_SLOT_MISSING;
                fs_advance_after_slot_load(st);
            } else {
                st->slot_state[st->load_slot] = FS_SLOT_IO_ERR;
                fs_advance_after_slot_load(st);
            }
        }
        return 0;

    case FS_ST_LOAD_READ_CALL:
        if (r->result != FX_OK || (uint32_t)r->value[0] != 0u) {
            st->io_err = 1u;
            st->stage = FS_ST_LOAD_CLOSE;
        } else {
            st->stage = FS_ST_LOAD_READ_DONE;
        }
        return 0;

    case FS_ST_LOAD_READ_DONE:
        if (r->result != FX_OK || r->bytes != 4u) {
            st->io_err = 1u;
            st->stage = FS_ST_LOAD_CLOSE;
        } else {
            uint32_t done = 0u;
            fs_copy(&done, r->data, 4u);
            if (done > FS_READ_MAX_BYTES - st->io_done_acc) {
                st->io_err = 1u;
                st->stage = FS_ST_LOAD_CLOSE;
            } else {
                st->io_done_acc += done;
                ++st->io_calls;
                if (done > 0u && st->io_done_acc < FS_READ_MAX_BYTES &&
                    st->io_calls < FS_IO_MAX_CALLS) {
                    st->stage = FS_ST_LOAD_READ_CALL;
                } else {
                    st->stage = FS_ST_LOAD_CLOSE;
                }
            }
        }
        return 0;

    case FS_ST_LOAD_CLOSE: {
        uint32_t close_err = (r->result == FX_OK) ? (uint32_t)r->value[0] : 1u;
        st->card_fd = -1;
        if (close_err != 0u || st->io_err) {
            st->slot_state[st->load_slot] = FS_SLOT_IO_ERR;
            fs_advance_after_slot_load(st);
        } else if (st->io_done_acc != Z8_FILMSIM_CFG_BYTES) {
            st->slot_state[st->load_slot] = FS_SLOT_CORRUPT;
            fs_advance_after_slot_load(st);
        } else {
            st->stage = FS_ST_LOAD_READ_DATA;
        }
        return 0;
    }

    case FS_ST_LOAD_READ_DATA:
        if (r->result != FX_OK || r->bytes != Z8_FILMSIM_CFG_BYTES) {
            st->slot_state[st->load_slot] = FS_SLOT_IO_ERR;
        } else {
            Z8FilmSimConfig tmp;
            fs_copy(&tmp, r->data, Z8_FILMSIM_CFG_BYTES);
            if (fs_cfg_is_valid(&tmp)) {
                fs_cfg_normalize_legacy(&tmp);
                st->slot_cfg[st->load_slot] = tmp;
                st->slot_state[st->load_slot] = FS_SLOT_VALID;
            } else {
                st->slot_state[st->load_slot] = FS_SLOT_CORRUPT;
            }
        }
        fs_advance_after_slot_load(st);
        return 0;

    /* ---- Boot load: FLMREC0/1.CFG (776 B) ---- */
    case FS_ST_REC_LOAD_WRITE_PATH:
        if (r->result != FX_OK) {
            st->rec_slot_state[st->rec_load_slot] = FS_SLOT_IO_ERR;
            fs_advance_after_rec_load(st);
        } else {
            st->card_fd = -1;
            st->io_done_acc = 0u;
            st->io_calls = 0u;
            st->io_err = 0u;
            st->stage = FS_ST_REC_LOAD_OPEN;
        }
        return 0;

    case FS_ST_REC_LOAD_OPEN:
        if (r->result != FX_OK) {
            st->rec_slot_state[st->rec_load_slot] = FS_SLOT_IO_ERR;
            fs_advance_after_rec_load(st);
        } else {
            int32_t fd = (int32_t)(uint32_t)r->value[0];
            if (fd >= 0) {
                st->card_fd = fd;
                st->stage = FS_ST_REC_LOAD_READ_CALL;
            } else if ((uint32_t)fd == FS_OPEN_NOT_FOUND) {
                st->rec_slot_state[st->rec_load_slot] = FS_SLOT_MISSING;
                fs_advance_after_rec_load(st);
            } else {
                st->rec_slot_state[st->rec_load_slot] = FS_SLOT_IO_ERR;
                fs_advance_after_rec_load(st);
            }
        }
        return 0;

    case FS_ST_REC_LOAD_READ_CALL:
        if (r->result != FX_OK || (uint32_t)r->value[0] != 0u) {
            st->io_err = 1u;
            st->stage = FS_ST_REC_LOAD_CLOSE;
        } else {
            st->stage = FS_ST_REC_LOAD_READ_DONE;
        }
        return 0;

    case FS_ST_REC_LOAD_READ_DONE:
        if (r->result != FX_OK || r->bytes != 4u) {
            st->io_err = 1u;
            st->stage = FS_ST_REC_LOAD_CLOSE;
        } else {
            uint32_t done = 0u;
            fs_copy(&done, r->data, 4u);
            if (done > FS_REC_READ_MAX_BYTES - st->io_done_acc) {
                st->io_err = 1u;
                st->stage = FS_ST_REC_LOAD_CLOSE;
            } else {
                st->io_done_acc += done;
                ++st->io_calls;
                if (done > 0u && st->io_done_acc < FS_REC_READ_MAX_BYTES &&
                    st->io_calls < FS_IO_MAX_CALLS) {
                    st->stage = FS_ST_REC_LOAD_READ_CALL;
                } else {
                    st->stage = FS_ST_REC_LOAD_CLOSE;
                }
            }
        }
        return 0;

    case FS_ST_REC_LOAD_CLOSE: {
        uint32_t close_err = (r->result == FX_OK) ? (uint32_t)r->value[0] : 1u;
        st->card_fd = -1;
        if (close_err != 0u || st->io_err) {
            st->rec_slot_state[st->rec_load_slot] = FS_SLOT_IO_ERR;
            fs_advance_after_rec_load(st);
        } else if (st->io_done_acc != Z8_NP3_TABLE_BYTES) {
            st->rec_slot_state[st->rec_load_slot] = FS_SLOT_CORRUPT;
            fs_advance_after_rec_load(st);
        } else {
            st->rec_chunk_off = 0u;
            st->stage = FS_ST_REC_LOAD_READ_CHUNK;
        }
        return 0;
    }

    case FS_ST_REC_LOAD_READ_CHUNK: {
        uint32_t rem = Z8_NP3_TABLE_BYTES - st->rec_chunk_off;
        uint32_t want = rem > FS_MEM_CHUNK_MAX ? FS_MEM_CHUNK_MAX : rem;
        if (r->result != FX_OK || r->bytes != want) {
            st->rec_slot_state[st->rec_load_slot] = FS_SLOT_IO_ERR;
            fs_advance_after_rec_load(st);
            return 0;
        }
        fs_copy(((uint8_t *)&st->rec_tmp) + st->rec_chunk_off, r->data, want);
        st->rec_chunk_off += want;
        if (st->rec_chunk_off < Z8_NP3_TABLE_BYTES) {
            return 0; /* Read next chunk */
        }
        if (z8_np3_table_is_valid(&st->rec_tmp)) {
            st->rec_slot_state[st->rec_load_slot] = FS_SLOT_VALID;
            if (!st->rec_best_valid || st->rec_tmp.seq > st->recipes.seq) {
                st->recipes = st->rec_tmp;
                st->rec_active_slot = st->rec_load_slot;
                st->rec_best_valid = 1u;
            }
        } else {
            st->rec_slot_state[st->rec_load_slot] = FS_SLOT_CORRUPT;
        }
        fs_advance_after_rec_load(st);
        return 0;
    }

    /* ---- Boot EL1 / Bus Sync ---- */
    case FS_ST_FS_GET_INIT:
        if (r->result == FX_OK && r->bytes == Z8_FILMSIM_CFG_BYTES) {
            Z8FilmSimConfig tmp;
            fs_copy(&tmp, r->data, Z8_FILMSIM_CFG_BYTES);
            if (fs_cfg_is_valid(&tmp)) {
                fs_cfg_normalize_legacy(&tmp);
                ++st->fs_get_completed;
                if (tmp.seq > st->cfg.seq) {
                    st->cfg = tmp;
                }
            }
        }
        st->stage = FS_ST_FS_PUSH_INIT;
        return 0;

    case FS_ST_FS_PUSH_INIT:
        if (r->result == FX_OK) {
            ++st->fs_set_completed;
            if (st->rec_best_valid && st->has_fs_recipe_ops) {
                st->rec_sync_chunk_idx = 0u;
                st->stage = FS_ST_FS_SYNC_REC_CHUNK;
            } else {
                st->stage = FS_ST_FS_STATUS_POST_INIT;
            }
        } else {
            st->has_fs = 0u;
            st->fs_via_slots = 0u;
            st->has_fs_recipe_ops = 0u;
            st->fs_binding = 0u;
            st->el1_status_valid = 0u;
            st->stage = FS_ST_UI_BUILD;
        }
        return 0;

    case FS_ST_FS_SYNC_REC_CHUNK:
        if (r->result == FX_OK) {
            ++st->fs_recipe_sync_completed;
            if (st->rec_sync_chunk_idx == 0u) {
                st->rec_sync_chunk_idx = 1u;
                return 0; /* Send second chunk (slots 64..94) */
            }
        } else {
            /* Older EL1 without recipe IPC opcodes: keep has_fs intact! */
            st->has_fs_recipe_ops = 0u;
        }
        st->stage = FS_ST_FS_STATUS_POST_INIT;
        return 0;

    case FS_ST_FS_STATUS_POST_INIT:
        if (r->result == FX_OK) {
            fs_parse_status_reply(st, r);
        }
        st->ui_dirty_mask = 0u;
        st->stage = FS_ST_UI_BUILD;
        return 0;

    case FS_ST_BUS_SEND:
        if (r->result != FX_OK) {
            st->has_bus = 0u;
            st->bus_binding = 0u;
            st->bus_endpoint = 0u;
            fs_mark_status_dirty_if_changed(st);
        }
        if (st->bus_init_phase) {
            st->bus_init_phase = 0u;
            st->ui_dirty_mask = 0u;
            st->stage = FS_ST_UI_BUILD;
        } else {
            st->stage = FS_ST_IDLE;
        }
        return 0;

    case FS_ST_BUS_POLL_STALE:
        st->stage = FS_ST_BUS_SEND;
        return 0;

    case FS_ST_UI_BUILD: {
        uint32_t built_id = st->ui_build_idx + 1u;
        if (r->result != FX_OK) {
            ++st->transient_errors;
            ++st->ui_build_retries;
            if (st->ui_build_retries <= FS_UI_BUILD_MAX_RETRIES) {
                return 0; /* Retry this control on next tick, never FAULT(5)! */
            }
            st->ui_build_retries = 0u;
            st->ui_dirty_mask |= (UINT32_C(1) << built_id);
        } else {
            st->ui_build_retries = 0u;
            st->ctrl_rev[built_id] = (uint32_t)r->value[0];
            if (built_id == FS_UI_BIND_NP3) {
                fs_format_bind_text(st, st->cached_bind_text);
            } else if (built_id == FS_UI_CLEAR_NP3) {
                fs_format_clear_text(st, st->cached_clear_text);
            } else if (built_id == FS_UI_XM6_DIAG) {
                fs_format_xm6_diag(st, st->cached_xm6_diag);
            } else if (built_id == FS_UI_XM6_RESET) {
                fs_format_xm6_reset(st, st->cached_xm6_reset);
            } else if (built_id == FS_UI_STATUS_HW) {
                fs_format_status_hw(st, st->cached_status_hw);
            }
        }
        ++st->ui_build_idx;
        if (st->ui_build_idx >= FS_UI_CTRL_COUNT) {
            st->ui_built = 1u;
            st->stage = FS_ST_IDLE;
        }
        return 0;
    }

    /* ---- Save FLMSIM0/1.CFG (64 B) ---- */
    case FS_ST_SAVE_WRITE_PATH:
        if (r->result != FX_OK) {
            fs_handle_save_failure(st, 0);
        } else {
            st->stage = FS_ST_SAVE_WRITE_DATA;
        }
        return 0;

    case FS_ST_SAVE_WRITE_DATA:
        if (r->result != FX_OK) {
            fs_handle_save_failure(st, 0);
        } else {
            st->card_fd = -1;
            st->io_done_acc = 0u;
            st->io_calls = 0u;
            st->io_err = 0u;
            st->stage = FS_ST_SAVE_OPEN;
        }
        return 0;

    case FS_ST_SAVE_OPEN:
        if (r->result != FX_OK) {
            fs_handle_save_failure(st, 1);
        } else {
            int32_t fd = (int32_t)(uint32_t)r->value[0];
            if (fd < 0) {
                fs_handle_save_failure(st, 1);
            } else {
                st->card_fd = fd;
                st->stage = FS_ST_SAVE_WRITE_CALL;
            }
        }
        return 0;

    case FS_ST_SAVE_WRITE_CALL:
        if (r->result != FX_OK || (uint32_t)r->value[0] != 0u) {
            st->io_err = 1u;
            st->stage = FS_ST_SAVE_CLOSE;
        } else {
            st->stage = FS_ST_SAVE_READ_DONE;
        }
        return 0;

    case FS_ST_SAVE_READ_DONE:
        if (r->result != FX_OK || r->bytes != 4u) {
            st->io_err = 1u;
            st->stage = FS_ST_SAVE_CLOSE;
        } else {
            uint32_t done = 0u;
            fs_copy(&done, r->data, 4u);
            if (done == 0u || done > Z8_FILMSIM_CFG_BYTES - st->io_done_acc) {
                st->io_err = 1u;
                st->stage = FS_ST_SAVE_CLOSE;
            } else {
                st->io_done_acc += done;
                ++st->io_calls;
                if (st->io_done_acc < Z8_FILMSIM_CFG_BYTES &&
                    st->io_calls < FS_IO_MAX_CALLS) {
                    st->stage = FS_ST_SAVE_WRITE_CALL;
                } else {
                    st->stage = FS_ST_SAVE_CLOSE;
                }
            }
        }
        return 0;

    case FS_ST_SAVE_CLOSE: {
        uint32_t close_err = (r->result == FX_OK) ? (uint32_t)r->value[0] : 1u;
        st->card_fd = -1;
        if (!st->io_err && close_err == 0u &&
            st->io_done_acc == Z8_FILMSIM_CFG_BYTES) {
            st->active_slot = st->save_target_slot;
            st->slot_cfg[st->active_slot] = st->save_cfg;
            st->slot_state[st->active_slot] = FS_SLOT_VALID;
            ++st->saves_completed;
            st->save_retries = 0u;
            st->save_open_fails = 0u;
            st->save_use_fallback_slot = 0u;
            if (st->cfg.seq != st->save_cfg.seq) {
                st->save_pending = 1u;
            }
            fs_mark_status_dirty_if_changed(st);
            st->stage = FS_ST_IDLE;
        } else {
            fs_handle_save_failure(st, 0);
        }
        return 0;
    }

    /* ---- Save FLMREC0/1.CFG (776 B) ---- */
    case FS_ST_REC_SAVE_WRITE_PATH:
        if (r->result != FX_OK) {
            fs_handle_rec_save_failure(st, 0);
        } else {
            st->rec_chunk_off = 0u;
            st->stage = FS_ST_REC_SAVE_WRITE_CHUNK;
        }
        return 0;

    case FS_ST_REC_SAVE_WRITE_CHUNK: {
        if (r->result != FX_OK) {
            fs_handle_rec_save_failure(st, 0);
            return 0;
        }
        uint32_t rem = Z8_NP3_TABLE_BYTES - st->rec_chunk_off;
        uint32_t chunk = rem > FS_MEM_CHUNK_MAX ? FS_MEM_CHUNK_MAX : rem;
        st->rec_chunk_off += chunk;
        if (st->rec_chunk_off < Z8_NP3_TABLE_BYTES) {
            return 0;
        }
        st->card_fd = -1;
        st->io_done_acc = 0u;
        st->io_calls = 0u;
        st->io_err = 0u;
        st->stage = FS_ST_REC_SAVE_OPEN;
        return 0;
    }

    case FS_ST_REC_SAVE_OPEN:
        if (r->result != FX_OK) {
            fs_handle_rec_save_failure(st, 1);
        } else {
            int32_t fd = (int32_t)(uint32_t)r->value[0];
            if (fd < 0) {
                fs_handle_rec_save_failure(st, 1);
            } else {
                st->card_fd = fd;
                st->stage = FS_ST_REC_SAVE_WRITE_CALL;
            }
        }
        return 0;

    case FS_ST_REC_SAVE_WRITE_CALL:
        if (r->result != FX_OK || (uint32_t)r->value[0] != 0u) {
            st->io_err = 1u;
            st->stage = FS_ST_REC_SAVE_CLOSE;
        } else {
            st->stage = FS_ST_REC_SAVE_READ_DONE;
        }
        return 0;

    case FS_ST_REC_SAVE_READ_DONE:
        if (r->result != FX_OK || r->bytes != 4u) {
            st->io_err = 1u;
            st->stage = FS_ST_REC_SAVE_CLOSE;
        } else {
            uint32_t done = 0u;
            fs_copy(&done, r->data, 4u);
            if (done == 0u || done > Z8_NP3_TABLE_BYTES - st->io_done_acc) {
                st->io_err = 1u;
                st->stage = FS_ST_REC_SAVE_CLOSE;
            } else {
                st->io_done_acc += done;
                ++st->io_calls;
                if (st->io_done_acc < Z8_NP3_TABLE_BYTES &&
                    st->io_calls < FS_IO_MAX_CALLS) {
                    st->stage = FS_ST_REC_SAVE_WRITE_CALL;
                } else {
                    st->stage = FS_ST_REC_SAVE_CLOSE;
                }
            }
        }
        return 0;

    case FS_ST_REC_SAVE_CLOSE: {
        uint32_t close_err = (r->result == FX_OK) ? (uint32_t)r->value[0] : 1u;
        st->card_fd = -1;
        if (!st->io_err && close_err == 0u &&
            st->io_done_acc == Z8_NP3_TABLE_BYTES) {
            st->rec_active_slot = st->rec_save_target_slot;
            st->rec_slot_state[st->rec_active_slot] = FS_SLOT_VALID;
            st->rec_best_valid = 1u;
            ++st->rec_saves_completed;
            st->rec_save_retries = 0u;
            st->rec_save_open_fails = 0u;
            st->rec_save_use_fallback = 0u;
            if (st->recipes.seq != st->rec_tmp.seq) {
                st->rec_save_pending = 1u;
            }
            /* Trigger FxModuleBus refresh AFTER FLMREC0/1.CFG is safely closed on disk
             * so EL1 filmsim_runtime_ui_poll() reloads it immediately with zero race! */
            if (st->has_bus) {
                st->bus_sync_needed = 1u;
            }
            fs_mark_status_dirty_if_changed(st);
            st->stage = FS_ST_IDLE;
        } else {
            fs_handle_rec_save_failure(st, 0);
        }
        return 0;
    }

    /* ---- Active PC selector & C10..C64 SC_RECORD +0x39C sync ---- */
    case FS_ST_PC_SEL_POLL:
        st->last_pc_ms = st->now_ms;
        if (r->result == FX_OK) {
            fs_update_active_pc_sel(st, (uint32_t)r->value[0]);
        }
        if (st->pending_np3_op != FS_NP3_OP_NONE) {
            uint32_t op = st->pending_np3_op;
            st->pending_np3_op = FS_NP3_OP_NONE;
            fs_execute_np3_op(st, op);
        }
        st->stage = FS_ST_IDLE;
        return 0;

    case FS_ST_SC_TAIL_READ:
        if (r->result == FX_OK && r->bytes == Z8_NP3_RECIPE_BYTES) {
            Z8Np3Recipe tmp;
            fs_copy(&tmp, r->data, Z8_NP3_RECIPE_BYTES);
            if (z8_np3_recipe_is_valid(&tmp) &&
                (tmp.flags & Z8_NP3_RECIPE_FLAG_ENABLED)) {
                st->active_sc_recipe = tmp;
                st->active_sc_recipe_valid = 1u;
            } else {
                fs_zero(&st->active_sc_recipe, (uint32_t)sizeof(st->active_sc_recipe));
                st->active_sc_recipe_valid = 0u;
            }
            fs_mark_status_dirty_if_changed(st);
        }
        st->stage = FS_ST_IDLE;
        return 0;

    case FS_ST_SC_RMW_READ:
        if (r->result != FX_OK || r->bytes != FS_SC_CHUNK_BYTES) {
            st->stage = FS_ST_IDLE;
            return 0;
        }
        fs_copy(st->sc_rec_buf + (uint32_t)st->sc_rmw_step * FS_SC_CHUNK_BYTES,
                r->data, FS_SC_CHUNK_BYTES);
        if (st->sc_rmw_step == 0u) {
            st->sc_rmw_step = 1u;
            return 0;
        }
        /* Only patch populated SC_RECORD slots (u32 flag at +0 == 1) */
        if (fs_read_u32_le(st->sc_rec_buf) != 1u) {
            st->stage = FS_ST_IDLE;
            return 0;
        }
        if (st->sc_rmw_clear) {
            fs_zero(st->sc_rec_buf + Z8_NP3_SC_RECORD_RECIPE_OFF, Z8_NP3_RECIPE_BYTES);
        } else {
            fs_copy(st->sc_rec_buf + Z8_NP3_SC_RECORD_RECIPE_OFF,
                    &st->sc_rmw_recipe, Z8_NP3_RECIPE_BYTES);
        }
        st->sc_rmw_step = 0u;
        st->stage = FS_ST_SC_RMW_WRITE;
        return 0;

    case FS_ST_SC_RMW_WRITE:
        if (r->result != FX_OK) {
            st->stage = FS_ST_IDLE;
            return 0;
        }
        if (st->sc_rmw_step == 0u) {
            st->sc_rmw_step = 1u;
            return 0;
        }
        st->stage = FS_ST_SC_RMW_COMMIT;
        return 0;

    case FS_ST_SC_RMW_COMMIT:
        if (r->result == FX_OK) {
            ++st->sc_rmw_completed;
        }
        st->stage = FS_ST_IDLE;
        return 0;

    case FS_ST_FS_SET_RECIPE:
        if (r->result == FX_OK) {
            ++st->fs_recipe_set_completed;
            st->status_refresh_needed = 1u;
        } else {
            /* Older EL1 without Z8_FS_OP_SET_RECIPE: disable recipe IPC only, keep has_fs */
            st->has_fs_recipe_ops = 0u;
        }
        st->stage = FS_ST_IDLE;
        return 0;

    case FS_ST_FS_SYNC:
        if (r->result == FX_OK) {
            ++st->fs_set_completed;
            st->status_refresh_needed = 1u;
        } else {
            st->has_fs = 0u;
            st->fs_via_slots = 0u;
            st->has_fs_recipe_ops = 0u;
            st->fs_binding = 0u;
            st->el1_status_valid = 0u;
            st->status_refresh_needed = 0u;
            st->xm6_reset_pending = 0u;
            st->xm6_diag_pending = 0u;
            fs_mark_status_dirty_if_changed(st);
        }
        st->stage = FS_ST_IDLE;
        return 0;

    case FS_ST_FS_XM6_RESET:
        if (r->result == FX_OK) {
            ++st->xm6_resets_completed;
            fs_parse_status_reply(st, r);
        } else {
            /* Older EL1 without Z8_FS_OP_XM6_RESET_BREAKER: keep has_fs intact! */
            st->xm6_diag_pending = 0u;
            st->xm6_diag_unsupported = 1u;
            fs_mark_status_dirty_if_changed(st);
        }
        st->stage = FS_ST_IDLE;
        return 0;

    case FS_ST_FS_XM6_DIAG:
        if (r->result == FX_OK && r->bytes == Z8_XM6_DIAG_BYTES) {
            Z8Xm6DiagResult tmp;
            fs_copy(&tmp, r->data, Z8_XM6_DIAG_BYTES);
            if (fs_xm6_diag_is_valid(&tmp)) {
                /* If breaker was tripped before our last poll, auto-reset once & re-test */
                if (tmp.breaker_tripped != 0u && tmp.ran_mask == 0u &&
                    tmp.hw_state == 2u && tmp.loader_fault == 0u &&
                    tmp.fw_loaded != 0u && !st->xm6_diag_retried_reset) {
                    st->xm6_diag_retried_reset = 1u;
                    st->xm6_reset_pending = 1u;
                    st->xm6_diag_pending = 1u;
                    st->stage = FS_ST_IDLE;
                    return 0;
                }
                st->xm6_diag = tmp;
                st->xm6_diag_valid = 1u;
                st->xm6_diag_unsupported = 0u;
                ++st->xm6_diag_completed;
                st->status_refresh_needed = 1u;
                if (st->has_bounce && st->card_write_ok) {
                    st->xm6_log_save_pending = 1u;
                }
                fs_mark_status_dirty_if_changed(st);
                st->stage = FS_ST_IDLE;
                return 0;
            }
        }
        /* Older EL1 or unsupported diag opcode: non-fatal, keep has_fs intact */
        st->xm6_diag_unsupported = 1u;
        fs_mark_status_dirty_if_changed(st);
        st->stage = FS_ST_IDLE;
        return 0;

    /* ---- Write A:\FENESTRA\XM6_DIAG.LOG ---- */
    case FS_ST_XM6_LOG_WRITE_PATH:
        if (r->result != FX_OK) {
            ++st->xm6_logs_failed;
            st->stage = FS_ST_IDLE;
        } else {
            st->xm6_log_off = 0u;
            st->stage = FS_ST_XM6_LOG_WRITE_CHUNK;
        }
        return 0;

    case FS_ST_XM6_LOG_WRITE_CHUNK: {
        if (r->result != FX_OK) {
            ++st->xm6_logs_failed;
            st->stage = FS_ST_IDLE;
            return 0;
        }
        uint32_t rem = st->xm6_log_len - st->xm6_log_off;
        uint32_t chunk = rem > FS_MEM_CHUNK_MAX ? FS_MEM_CHUNK_MAX : rem;
        st->xm6_log_off += chunk;
        if (st->xm6_log_off < st->xm6_log_len) {
            return 0;
        }
        st->card_fd = -1;
        st->io_done_acc = 0u;
        st->io_calls = 0u;
        st->io_err = 0u;
        st->stage = FS_ST_XM6_LOG_OPEN;
        return 0;
    }

    case FS_ST_XM6_LOG_OPEN:
        if (r->result != FX_OK) {
            ++st->xm6_logs_failed;
            st->stage = FS_ST_IDLE;
        } else {
            int32_t fd = (int32_t)(uint32_t)r->value[0];
            if (fd < 0) {
                ++st->xm6_logs_failed;
                st->stage = FS_ST_IDLE;
            } else {
                st->card_fd = fd;
                st->stage = FS_ST_XM6_LOG_WRITE_CALL;
            }
        }
        return 0;

    case FS_ST_XM6_LOG_WRITE_CALL:
        if (r->result != FX_OK || (uint32_t)r->value[0] != 0u) {
            st->io_err = 1u;
            st->stage = FS_ST_XM6_LOG_CLOSE;
        } else {
            st->stage = FS_ST_XM6_LOG_READ_DONE;
        }
        return 0;

    case FS_ST_XM6_LOG_READ_DONE:
        if (r->result != FX_OK || r->bytes != 4u) {
            st->io_err = 1u;
            st->stage = FS_ST_XM6_LOG_CLOSE;
        } else {
            uint32_t done = 0u;
            fs_copy(&done, r->data, 4u);
            if (done == 0u || done > st->xm6_log_len - st->io_done_acc) {
                st->io_err = 1u;
                st->stage = FS_ST_XM6_LOG_CLOSE;
            } else {
                st->io_done_acc += done;
                ++st->io_calls;
                if (st->io_done_acc < st->xm6_log_len &&
                    st->io_calls < FS_IO_MAX_CALLS) {
                    st->stage = FS_ST_XM6_LOG_WRITE_CALL;
                } else {
                    st->stage = FS_ST_XM6_LOG_CLOSE;
                }
            }
        }
        return 0;

    case FS_ST_XM6_LOG_CLOSE: {
        uint32_t close_err = (r->result == FX_OK) ? (uint32_t)r->value[0] : 1u;
        st->card_fd = -1;
        if (!st->io_err && close_err == 0u && st->io_done_acc == st->xm6_log_len) {
            ++st->xm6_logs_completed;
            st->xm6_log_saved = 1u;
        } else {
            ++st->xm6_logs_failed;
        }
        st->stage = FS_ST_IDLE;
        return 0;
    }

    case FS_ST_FS_STATUS_POLL:
        if (r->result == FX_OK) {
            fs_parse_status_reply(st, r);
        } else {
            st->has_fs = 0u;
            st->fs_via_slots = 0u;
            st->has_fs_recipe_ops = 0u;
            st->fs_binding = 0u;
            st->el1_status_valid = 0u;
            st->xm6_reset_pending = 0u;
            st->xm6_diag_pending = 0u;
            fs_mark_status_dirty_if_changed(st);
        }
        st->stage = FS_ST_IDLE;
        return 0;

    case FS_ST_LANG_POLL:
        st->last_lang_ms = st->now_ms;
        if (r->result == FX_OK && r->value[1] == FS_LANG_MAGIC) {
            ++st->lang_polls_completed;
            uint32_t new_lang = (uint32_t)r->value[0];
            uint32_t old_loc = fs_lang_bucket(st->ui_lang);
            uint32_t new_loc = fs_lang_bucket(new_lang);
            st->ui_lang = new_lang;
            if (new_loc != old_loc) {
                uint32_t all_ctrls = 0u;
                for (uint32_t id = FS_UI_PAGE_ROOT; id <= FS_UI_CTRL_COUNT; ++id) {
                    all_ctrls |= (UINT32_C(1) << id);
                }
                st->ui_dirty_mask |= all_ctrls;
                ++st->lang_switches;
            }
        }
        st->stage = FS_ST_IDLE;
        return 0;

    case FS_ST_UI_UPDATE:
        if (r->result == FX_OK && st->ui_active_ctrl >= FS_UI_PAGE_ROOT &&
            st->ui_active_ctrl <= FS_UI_CTRL_COUNT) {
            st->ctrl_rev[st->ui_active_ctrl] = (uint32_t)r->value[0];
            if (st->ui_active_ctrl == FS_UI_BIND_NP3) {
                fs_format_bind_text(st, st->cached_bind_text);
            } else if (st->ui_active_ctrl == FS_UI_CLEAR_NP3) {
                fs_format_clear_text(st, st->cached_clear_text);
            } else if (st->ui_active_ctrl == FS_UI_XM6_DIAG) {
                fs_format_xm6_diag(st, st->cached_xm6_diag);
            } else if (st->ui_active_ctrl == FS_UI_XM6_RESET) {
                fs_format_xm6_reset(st, st->cached_xm6_reset);
            } else if (st->ui_active_ctrl == FS_UI_STATUS_HW) {
                fs_format_status_hw(st, st->cached_status_hw);
            }
        }
        if (st->ui_active_ctrl <= FS_UI_CTRL_COUNT) {
            st->ui_dirty_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
            st->ui_sync_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
        }
        st->ui_active_ctrl = 0u;
        st->stage = FS_ST_IDLE;
        return 0;

    case FS_ST_UI_SYNC_GET:
        if (r->result == FX_OK && st->ui_active_ctrl >= FS_UI_PAGE_ROOT &&
            st->ui_active_ctrl <= FS_UI_CTRL_COUNT) {
            st->ctrl_rev[st->ui_active_ctrl] = (uint32_t)r->value[0];
        }
        if (st->ui_active_ctrl <= FS_UI_CTRL_COUNT) {
            st->ui_sync_mask &= ~(UINT32_C(1) << st->ui_active_ctrl);
        }
        st->ui_active_ctrl = 0u;
        st->stage = FS_ST_IDLE;
        return 0;

    case FS_ST_POLL_EVENTS:
        if (r->result == FX_OK) {
            st->idle_polls = 0u;
            fs_handle_ui_event(st, r);
        } else if (r->result == FX_EMPTY) {
            ++st->idle_polls;
            if (st->has_fs && (st->idle_polls % FS_STATUS_POLL_PERIOD == 0u)) {
                st->status_refresh_needed = 1u;
            }
        }
        st->stage = FS_ST_IDLE;
        return 0;

    default:
        ++st->transient_errors;
        st->stage = FS_ST_IDLE;
        return 0;
    }
}

static inline void fs_schedule_idle_work(FsModuleState *st) {
    if (st->stage != FS_ST_IDLE) {
        return;
    }
    /* Prioritize pending Bind/Clear active selector refresh immediately */
    if (st->pending_np3_op != FS_NP3_OP_NONE && st->has_slots) {
        st->pc_refresh_needed = 0u;
        st->stage = FS_ST_PC_SEL_POLL;
        return;
    }
    if (st->save_pending) {
        if (!st->has_bounce || !st->card_write_ok) {
            st->save_pending = 0u;
            st->save_retries = 0u;
            st->save_open_fails = 0u;
            st->save_use_fallback_slot = 0u;
        } else if (st->save_retries == 0u ||
                   st->now_ms < st->save_fail_ms ||
                   (st->now_ms - st->save_fail_ms) >= FS_SAVE_RETRY_MS) {
            st->save_pending = 0u;
            st->save_target_slot = st->save_use_fallback_slot
                                       ? st->active_slot
                                       : (st->active_slot ^ 1u);
            st->save_cfg = st->cfg;
            st->stage = FS_ST_SAVE_WRITE_PATH;
            return;
        }
    }
    if (st->sc_rmw_pending && st->has_slots) {
        st->sc_rmw_pending = 0u;
        st->sc_rmw_step = 0u;
        st->stage = FS_ST_SC_RMW_READ;
        return;
    }
    if (st->rec_save_pending) {
        if (!st->has_bounce || !st->card_write_ok) {
            st->rec_save_pending = 0u;
            st->rec_save_retries = 0u;
            st->rec_save_open_fails = 0u;
            st->rec_save_use_fallback = 0u;
            if (st->has_bus) {
                st->bus_sync_needed = 1u;
            }
        } else if (st->rec_save_retries == 0u ||
                   st->now_ms < st->rec_save_fail_ms ||
                   (st->now_ms - st->rec_save_fail_ms) >= FS_SAVE_RETRY_MS) {
            st->rec_save_pending = 0u;
            st->rec_save_target_slot = st->rec_save_use_fallback
                                           ? st->rec_active_slot
                                           : (st->rec_active_slot ^ 1u);
            st->rec_tmp = st->recipes;
            st->stage = FS_ST_REC_SAVE_WRITE_PATH;
            return;
        }
    }
    if (st->fs_sync_needed && st->has_fs) {
        st->fs_sync_needed = 0u;
        st->stage = FS_ST_FS_SYNC;
        return;
    }
    if (st->fs_recipe_sync_pending && st->has_fs && st->has_fs_recipe_ops) {
        st->fs_recipe_sync_pending = 0u;
        st->stage = FS_ST_FS_SET_RECIPE;
        return;
    }
    if (st->bus_sync_needed && st->has_bus) {
        st->bus_sync_needed = 0u;
        st->bus_init_phase = 0u;
        st->bus_send_retries = 0u;
        st->stage = FS_ST_BUS_SEND;
        return;
    }
    if (st->xm6_reset_pending && st->has_fs) {
        st->xm6_reset_pending = 0u;
        st->stage = FS_ST_FS_XM6_RESET;
        return;
    }
    if (st->xm6_diag_pending && st->has_fs) {
        st->xm6_diag_pending = 0u;
        st->stage = FS_ST_FS_XM6_DIAG;
        return;
    }
    if (st->status_refresh_needed && st->has_fs) {
        st->status_refresh_needed = 0u;
        st->stage = FS_ST_FS_STATUS_POLL;
        return;
    }
    if (st->xm6_log_save_pending) {
        st->xm6_log_save_pending = 0u;
        if (st->has_bounce && st->card_write_ok && st->xm6_diag_valid) {
            fs_format_xm6_diag_log(st);
            if (st->xm6_log_len > 0u) {
                st->stage = FS_ST_XM6_LOG_WRITE_PATH;
                return;
            }
        }
    }
    if (st->sc_tail_read_needed && st->has_slots) {
        st->sc_tail_read_needed = 0u;
        uint32_t sc_idx = 0u;
        if (st->active_pc_valid && z8_np3_selector_is_c64(st->active_pc_sel, &sc_idx)) {
            st->stage = FS_ST_SC_TAIL_READ;
            return;
        }
    }
    if (st->ui_dirty_mask != 0u) {
        uint32_t id = fs_lowest_ctrl_bit(st->ui_dirty_mask);
        if (id != 0u) {
            st->ui_active_ctrl = id;
            st->stage = FS_ST_UI_UPDATE;
            return;
        }
        st->ui_dirty_mask = 0u;
    }
    if (st->ui_sync_mask != 0u) {
        uint32_t id = fs_lowest_ctrl_bit(st->ui_sync_mask);
        if (id != 0u) {
            st->ui_active_ctrl = id;
            st->stage = FS_ST_UI_SYNC_GET;
            return;
        }
        st->ui_sync_mask = 0u;
    }
    if (st->has_slots &&
        (st->pc_refresh_needed ||
         (st->focus_foreground && st->now_ms >= st->last_pc_ms &&
          (st->now_ms - st->last_pc_ms) >= FS_PC_POLL_MS))) {
        st->pc_refresh_needed = 0u;
        st->stage = FS_ST_PC_SEL_POLL;
        return;
    }
    if (st->has_lang_svc &&
        (st->lang_refresh_needed ||
         (st->now_ms >= st->last_lang_ms &&
          (st->now_ms - st->last_lang_ms) >= FS_LANG_POLL_MS))) {
        st->lang_refresh_needed = 0u;
        st->stage = FS_ST_LANG_POLL;
        return;
    }
    st->stage = FS_ST_POLL_EVENTS;
}

static inline FxResult fs_submit_el1(FsModuleState *st, const FxServiceModule *api,
                                     uint32_t z8_op, const void *payload,
                                     uint32_t payload_bytes) {
    if (!st->fs_via_slots) {
        return fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->fs_binding,
                               z8_op, 0u, payload, payload_bytes);
    }
    if (payload_bytes > Z8_NP3_CHUNK_BYTES) {
        return FX_ARGUMENT;
    }
    uint32_t hdr[4] = {Z8_FS_SLOTS_TUNNEL_MAGIC, payload_bytes, 0u, 0u};
    fs_copy(st->fs_tx_buf, hdr, Z8_FS_SLOTS_HDR_BYTES);
    if (payload && payload_bytes) {
        fs_copy(st->fs_tx_buf + Z8_FS_SLOTS_HDR_BYTES, payload, payload_bytes);
    }
    return fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->slots_binding,
                           Z8_FS_SLOTS_OP_BASE + z8_op, 0u,
                           st->fs_tx_buf, Z8_FS_SLOTS_HDR_BYTES + payload_bytes);
}

static inline int fs_module_step(FsModuleState *st, const FxModuleServices *base) {
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
        if (taken == FX_BUSY) {
            return 0;
        }
        if (taken != FX_OK) {
            /* A2 Hardening: never return 3 / FAULT(5) on transient IPC errors */
            fs_recover_from_transient_error(st);
            return 0;
        }
        (void)fs_handle_reply(st, &reply);
    }

    fs_schedule_idle_work(st);

    FxResult res = FX_ARGUMENT;
    switch (st->stage) {
    case FS_ST_QUERY_UI:
        res = fx_sdk_ui_query(&st->sdk, api, 0u);
        break;
    case FS_ST_OPEN_SLOTS:
        res = fx_sdk_open(&st->sdk, api, FS_SLOTS_BINDING_NAME,
                          FS_SLOTS_BINDING_VER);
        break;
    case FS_ST_LANG_INIT:
    case FS_ST_LANG_POLL: {
        FsSlotsRequest q = {0u, 0u, 0u, 0u};
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->slots_binding,
                              FS_SLOTS_LANG_GET, 0u, &q, (uint32_t)sizeof(q));
        break;
    }
    case FS_ST_PC_SEL_INIT:
    case FS_ST_PC_SEL_POLL: {
        FsSlotsRequest q = {0u, 0u, 0u, 0u};
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->slots_binding,
                              FS_SLOTS_SELECTION, 0u, &q, (uint32_t)sizeof(q));
        break;
    }
    case FS_ST_SC_TAIL_READ: {
        uint32_t sc_idx = 0u;
        if (!st->active_pc_valid || !z8_np3_selector_is_c64(st->active_pc_sel, &sc_idx)) {
            st->stage = FS_ST_IDLE;
            return 0;
        }
        FsSlotsRequest q = {sc_idx, Z8_NP3_SC_RECORD_RECIPE_OFF, Z8_NP3_RECIPE_BYTES, 0u};
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->slots_binding,
                              FS_SLOTS_READ, 0u, &q, (uint32_t)sizeof(q));
        break;
    }
    case FS_ST_SC_RMW_READ: {
        uint32_t off = (uint32_t)st->sc_rmw_step * FS_SC_CHUNK_BYTES;
        FsSlotsRequest q = {st->sc_rmw_index, off, FS_SC_CHUNK_BYTES, 0u};
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->slots_binding,
                              FS_SLOTS_READ, 0u, &q, (uint32_t)sizeof(q));
        break;
    }
    case FS_ST_SC_RMW_WRITE: {
        struct {
            FsSlotsRequest hdr;
            uint8_t data[FS_SC_CHUNK_BYTES];
        } req;
        uint32_t off = (uint32_t)st->sc_rmw_step * FS_SC_CHUNK_BYTES;
        req.hdr = (FsSlotsRequest){st->sc_rmw_index, off, FS_SC_CHUNK_BYTES, 0u};
        fs_copy(req.data, st->sc_rec_buf + off, FS_SC_CHUNK_BYTES);
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->slots_binding,
                              FS_SLOTS_WRITE, 0u, &req, (uint32_t)sizeof(req));
        break;
    }
    case FS_ST_SC_RMW_COMMIT: {
        FsSlotsRequest q = {st->sc_rmw_index, 0u, 0u, 0u};
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->slots_binding,
                              FS_SLOTS_COMMIT, 0u, &q, (uint32_t)sizeof(q));
        break;
    }
    case FS_ST_OPEN_MEM:
        res = fx_sdk_open(&st->sdk, api, "core.memory", 1u);
        break;
    case FS_ST_ALLOC_MEM:
        res = fx_sdk_memory_allocate(&st->sdk, api, st->mem_binding,
                                     FS_BOUNCE_BYTES, 1u);
        break;
    case FS_ST_OPEN_CARD_OPEN:
        res = fx_sdk_open(&st->sdk, api, "camera.card.open", 1u);
        break;
    case FS_ST_OPEN_CARD_READ:
        res = fx_sdk_open(&st->sdk, api, "camera.card.read", 1u);
        break;
    case FS_ST_OPEN_CARD_WRITE:
        res = fx_sdk_open(&st->sdk, api, "camera.card.write", 1u);
        break;
    case FS_ST_OPEN_CARD_CLOSE:
        res = fx_sdk_open(&st->sdk, api, "camera.card.close", 1u);
        break;
    case FS_ST_OPEN_FS:
        res = fx_sdk_open(&st->sdk, api, Z8_FILMSIM_BINDING_NAME,
                          Z8_FILMSIM_BINDING_VER);
        break;
    case FS_ST_FS_STATUS_INIT:
    case FS_ST_FS_STATUS_POST_INIT:
    case FS_ST_FS_STATUS_POLL:
        res = fs_submit_el1(st, api, Z8_FS_OP_STATUS, 0, 0u);
        break;
    case FS_ST_OPEN_BUS:
        res = fx_sdk_open(&st->sdk, api, "core.services", 1u);
        break;
    case FS_ST_BUS_PUBLISH:
        res = fx_sdk_bus_named(&st->sdk, api, st->bus_binding, FX_BUS_PUBLISH,
                               Z8_FILMSIM_BINDING_NAME,
                               Z8_FILMSIM_BINDING_VER);
        break;
    case FS_ST_BUS_RESERVE:
        res = fx_sdk_bus_reserve(&st->sdk, api, st->bus_binding,
                                 st->bus_endpoint, 2u);
        break;
    case FS_ST_LOAD_WRITE_PATH: {
        char path[32];
        fs_slot_path(fs_effective_drive(st), st->load_slot, path);
        res = fs_submit_mem_write(st, api, FS_BOUNCE_PATH_OFF, path, 32u);
        break;
    }
    case FS_ST_REC_LOAD_WRITE_PATH: {
        char path[32];
        fs_rec_slot_path(fs_effective_drive(st), st->rec_load_slot, path);
        res = fs_submit_mem_write(st, api, FS_BOUNCE_PATH_OFF, path, 32u);
        break;
    }
    case FS_ST_LOAD_OPEN:
    case FS_ST_REC_LOAD_OPEN: {
        uint64_t args[8] = {
            st->buf_native + FS_BOUNCE_PATH_OFF,
            0u, /* read-only */
            0u, 0u, 0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_open_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case FS_ST_LOAD_READ_CALL: {
        uint64_t args[8] = {
            (uint64_t)(uint32_t)st->card_fd,
            st->buf_native + FS_BOUNCE_DATA_OFF + st->io_done_acc,
            FS_READ_MAX_BYTES - st->io_done_acc,
            st->buf_native + FS_BOUNCE_DONE_OFF,
            0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_read_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case FS_ST_REC_LOAD_READ_CALL: {
        uint64_t args[8] = {
            (uint64_t)(uint32_t)st->card_fd,
            st->buf_native + FS_BOUNCE_DATA_OFF + st->io_done_acc,
            FS_REC_READ_MAX_BYTES - st->io_done_acc,
            st->buf_native + FS_BOUNCE_DONE_OFF,
            0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_read_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case FS_ST_LOAD_READ_DONE:
    case FS_ST_REC_LOAD_READ_DONE:
    case FS_ST_SAVE_READ_DONE:
    case FS_ST_REC_SAVE_READ_DONE:
    case FS_ST_XM6_LOG_READ_DONE:
        res = fs_submit_mem_read(st, api, FS_BOUNCE_DONE_OFF, 4u);
        break;
    case FS_ST_LOAD_CLOSE:
    case FS_ST_REC_LOAD_CLOSE:
    case FS_ST_SAVE_CLOSE:
    case FS_ST_REC_SAVE_CLOSE:
    case FS_ST_XM6_LOG_CLOSE: {
        uint64_t args[8] = {(uint64_t)(uint32_t)st->card_fd, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_close_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case FS_ST_LOAD_READ_DATA:
        res = fs_submit_mem_read(st, api, FS_BOUNCE_DATA_OFF, Z8_FILMSIM_CFG_BYTES);
        break;
    case FS_ST_REC_LOAD_READ_CHUNK: {
        uint32_t rem = Z8_NP3_TABLE_BYTES - st->rec_chunk_off;
        uint32_t want = rem > FS_MEM_CHUNK_MAX ? FS_MEM_CHUNK_MAX : rem;
        res = fs_submit_mem_read(st, api, FS_BOUNCE_DATA_OFF + st->rec_chunk_off, want);
        break;
    }
    case FS_ST_FS_GET_INIT:
        res = fs_submit_el1(st, api, Z8_FS_OP_GET, 0, 0u);
        break;
    case FS_ST_FS_PUSH_INIT:
    case FS_ST_FS_SYNC:
        res = fs_submit_el1(st, api, Z8_FS_OP_SET, &st->cfg, Z8_FILMSIM_CFG_BYTES);
        break;
    case FS_ST_FS_SYNC_REC_CHUNK: {
        uint32_t start = (st->rec_sync_chunk_idx == 0u) ? 0u : Z8_NP3_CHUNK_MAX_SLOTS;
        uint32_t count = (st->rec_sync_chunk_idx == 0u)
                             ? Z8_NP3_CHUNK_MAX_SLOTS
                             : (Z8_NP3_SLOT_COUNT - Z8_NP3_CHUNK_MAX_SLOTS);
        fs_zero(&st->rec_chunk, (uint32_t)sizeof(st->rec_chunk));
        st->rec_chunk.magic = Z8_NP3_TABLE_MAGIC;
        st->rec_chunk.start_slot = (uint16_t)start;
        st->rec_chunk.count = (uint16_t)count;
        st->rec_chunk.seq = st->recipes.seq;
        st->rec_chunk.flags = (st->rec_sync_chunk_idx == 0u) ? 1u : 0u;
        for (uint32_t k = 0u; k < count; ++k) {
            st->rec_chunk.entries[k] = st->recipes.entries[start + k];
        }
        res = fs_submit_el1(st, api, Z8_FS_OP_SYNC_RECIPES,
                            &st->rec_chunk, Z8_NP3_CHUNK_BYTES);
        break;
    }
    case FS_ST_FS_SET_RECIPE:
        res = fs_submit_el1(st, api, Z8_FS_OP_SET_RECIPE,
                            &st->fs_recipe_msg, Z8_NP3_MSG_BYTES);
        break;
    case FS_ST_FS_XM6_RESET:
        res = fs_submit_el1(st, api, Z8_FS_OP_XM6_RESET_BREAKER, 0, 0u);
        break;
    case FS_ST_FS_XM6_DIAG:
        res = fs_submit_el1(st, api, Z8_FS_OP_XM6_DIAG, 0, 0u);
        break;
    case FS_ST_BUS_SEND:
        res = fx_sdk_bus_send(&st->sdk, api, st->bus_binding, st->bus_endpoint,
                              ++st->bus_corr, &st->cfg, Z8_FILMSIM_CFG_BYTES);
        break;
    case FS_ST_BUS_POLL_STALE:
        res = fx_sdk_bus_poll(&st->sdk, api, st->bus_binding, st->bus_endpoint);
        break;
    case FS_ST_UI_BUILD: {
        FxUiControl ctrl;
        if (!fs_build_ui_control(st->ui_build_idx + 1u, st, &ctrl)) {
            fs_recover_from_transient_error(st);
            return 0;
        }
        res = fx_sdk_ui(&st->sdk, api, &ctrl);
        break;
    }
    case FS_ST_SAVE_WRITE_PATH: {
        char path[32];
        fs_slot_path(fs_effective_drive(st), st->save_target_slot, path);
        res = fs_submit_mem_write(st, api, FS_BOUNCE_PATH_OFF, path, 32u);
        break;
    }
    case FS_ST_SAVE_WRITE_DATA:
        res = fs_submit_mem_write(st, api, FS_BOUNCE_DATA_OFF,
                                  &st->save_cfg, Z8_FILMSIM_CFG_BYTES);
        break;
    case FS_ST_REC_SAVE_WRITE_PATH: {
        char path[32];
        fs_rec_slot_path(fs_effective_drive(st), st->rec_save_target_slot, path);
        res = fs_submit_mem_write(st, api, FS_BOUNCE_PATH_OFF, path, 32u);
        break;
    }
    case FS_ST_REC_SAVE_WRITE_CHUNK: {
        uint32_t rem = Z8_NP3_TABLE_BYTES - st->rec_chunk_off;
        uint32_t chunk = rem > FS_MEM_CHUNK_MAX ? FS_MEM_CHUNK_MAX : rem;
        res = fs_submit_mem_write(st, api, FS_BOUNCE_DATA_OFF + st->rec_chunk_off,
                                  ((const uint8_t *)&st->rec_tmp) + st->rec_chunk_off,
                                  chunk);
        break;
    }
    case FS_ST_XM6_LOG_WRITE_PATH: {
        char path[32];
        fs_xm6_log_path(fs_effective_drive(st), path);
        res = fs_submit_mem_write(st, api, FS_BOUNCE_PATH_OFF, path, 32u);
        break;
    }
    case FS_ST_XM6_LOG_WRITE_CHUNK: {
        uint32_t rem = st->xm6_log_len - st->xm6_log_off;
        uint32_t chunk = rem > FS_MEM_CHUNK_MAX ? FS_MEM_CHUNK_MAX : rem;
        res = fs_submit_mem_write(st, api, FS_BOUNCE_DATA_OFF + st->xm6_log_off,
                                  st->xm6_log_buf + st->xm6_log_off, chunk);
        break;
    }
    case FS_ST_SAVE_OPEN:
    case FS_ST_REC_SAVE_OPEN:
    case FS_ST_XM6_LOG_OPEN: {
        uint64_t args[8] = {
            st->buf_native + FS_BOUNCE_PATH_OFF,
            UINT64_C(0x301), /* write + create + truncate */
            0u, 0u, 0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_open_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case FS_ST_SAVE_WRITE_CALL: {
        uint64_t args[8] = {
            (uint64_t)(uint32_t)st->card_fd,
            st->buf_native + FS_BOUNCE_DATA_OFF + st->io_done_acc,
            Z8_FILMSIM_CFG_BYTES - st->io_done_acc,
            st->buf_native + FS_BOUNCE_DONE_OFF,
            0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_write_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case FS_ST_REC_SAVE_WRITE_CALL: {
        uint64_t args[8] = {
            (uint64_t)(uint32_t)st->card_fd,
            st->buf_native + FS_BOUNCE_DATA_OFF + st->io_done_acc,
            Z8_NP3_TABLE_BYTES - st->io_done_acc,
            st->buf_native + FS_BOUNCE_DONE_OFF,
            0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_write_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case FS_ST_XM6_LOG_WRITE_CALL: {
        uint64_t args[8] = {
            (uint64_t)(uint32_t)st->card_fd,
            st->buf_native + FS_BOUNCE_DATA_OFF + st->io_done_acc,
            st->xm6_log_len - st->io_done_acc,
            st->buf_native + FS_BOUNCE_DONE_OFF,
            0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_write_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case FS_ST_UI_UPDATE: {
        FxUiControl ctrl;
        if (!fs_build_ui_control(st->ui_active_ctrl, st, &ctrl)) {
            fs_recover_from_transient_error(st);
            return 0;
        }
        res = fx_sdk_ui(&st->sdk, api, &ctrl);
        break;
    }
    case FS_ST_UI_SYNC_GET:
        res = fx_sdk_ui_get(&st->sdk, api, st->ui_active_ctrl);
        break;
    case FS_ST_POLL_EVENTS:
        res = fx_sdk_events(&st->sdk, api);
        break;
    default:
        fs_recover_from_transient_error(st);
        return 0;
    }

    /* A2 Hardening: never return 9 / FAULT(5) on transient submit error! */
    if (res == FX_BUSY) {
        return 0;
    }
    if (res != FX_OK) {
        fs_recover_from_transient_error(st);
        return 0;
    }
    return 0;
}

#endif /* FILMSIM_CORE_H */
