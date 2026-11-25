/* SPDX-License-Identifier: MIT
 * Film Frame / XPan WonderLantern Module Core State Machine & Codec.
 * Freestanding C11, zero libc dependencies, zero pointer relocations.
 */
#ifndef FILMFRAME_CORE_H
#define FILMFRAME_CORE_H

#include "fenestra_scene_sdk.h"
#include "fenestra_file_sdk.h"
#include "fenestra_bus_sdk.h"
#include "filmframe_abi.h"

_Static_assert(sizeof(Z8FilmFrameConfig) == Z8_FILM_FRAME_CFG_BYTES,
               "Z8FilmFrameConfig must remain 64 bytes");
_Static_assert(offsetof(Z8FilmFrameConfig, magic) == 0u, "magic offset");
_Static_assert(offsetof(Z8FilmFrameConfig, version) == 4u, "version offset");
_Static_assert(offsetof(Z8FilmFrameConfig, bytes) == 6u, "bytes offset");
_Static_assert(offsetof(Z8FilmFrameConfig, seq) == 8u, "seq offset");
_Static_assert(offsetof(Z8FilmFrameConfig, crc32) == 12u, "crc32 offset");
_Static_assert(offsetof(Z8FilmFrameConfig, xpan_mode) == 16u, "xpan_mode offset");
_Static_assert(offsetof(Z8FilmFrameConfig, live_link) == 23u, "live_link offset");
_Static_assert(offsetof(Z8FilmFrameConfig, reserved) == 24u, "reserved offset");

/* Native bounce buffer layout (allocated once via core.memory) */
#define FF_BOUNCE_BYTES        256u
#define FF_BOUNCE_PATH_OFF     0u
#define FF_BOUNCE_DATA_OFF     64u
#define FF_BOUNCE_DONE_OFF     136u
#define FF_BOUNCE_INIT_SPAN    140u

#define FF_OPEN_NOT_FOUND      UINT32_C(0x80000010)
#define FF_READ_MAX_BYTES      65u
#define FF_IO_MAX_CALLS        8u
#define FF_LOAD_MAX_RETRIES    3u

/* Shooting Viewfinder 1280x800 Global Scene Geometry for 65:24 XPan */
#define FF_SCENE_W             1280u
#define FF_SCENE_H             800u
#define FF_XPAN_ACTIVE_H       472u
#define FF_XPAN_BAR_H          164u
#define FF_XPAN_TOP_Y          0u
#define FF_XPAN_BOT_Y          636u
#define FF_XPAN_LINE_THICK     2u
#define FF_COLOR_DIM50         UINT32_C(0x80000000)
#define FF_COLOR_BLACK         UINT32_C(0xff000000)
#define FF_COLOR_FRAME_LINE    UINT32_C(0xffffffff)
#define FF_COLOR_MARKER_BG     UINT32_C(0x80000000)
#define FF_COLOR_MARKER_AMBER  UINT32_C(0xffe89826)
#define FF_SCENE_MAX_CMDS      32u

enum FfUiControlId {
    FF_UI_PAGE_ID    = 1u,
    FF_UI_XPAN_MODE  = 2u,
    FF_UI_BORDER     = 3u,
    FF_UI_EDGE_LABEL = 4u,
    FF_UI_DATE_FMT   = 5u,
    FF_UI_DATE_STYLE = 6u,
    FF_UI_DATE_GLOW  = 7u,
    FF_UI_VF_GUIDE   = 8u,
    FF_UI_LIVE_LINK  = 9u,
    FF_UI_CTRL_COUNT = 9u
};

enum FfSlotStatus {
    FF_SLOT_UNREAD  = 0u,
    FF_SLOT_MISSING = 1u,
    FF_SLOT_CORRUPT = 2u,
    FF_SLOT_IO_ERR  = 3u,
    FF_SLOT_VALID   = 4u
};

enum FfStage {
    FF_ST_OPEN_SCENE = 0u,
    FF_ST_QUERY_SCENE,
    FF_ST_QUERY_UI,
    FF_ST_OPEN_MEM,
    FF_ST_ALLOC_MEM,
    FF_ST_OPEN_CARD_OPEN,
    FF_ST_OPEN_CARD_READ,
    FF_ST_OPEN_CARD_WRITE,
    FF_ST_OPEN_CARD_CLOSE,
    FF_ST_OPEN_FF,
    FF_ST_FF_STATUS,
    FF_ST_OPEN_BUS,
    FF_ST_BUS_PUBLISH,
    FF_ST_BUS_RESERVE,
    FF_ST_LOAD_WRITE_PATH,
    FF_ST_LOAD_OPEN,
    FF_ST_LOAD_READ_CALL,
    FF_ST_LOAD_READ_DONE,
    FF_ST_LOAD_CLOSE,
    FF_ST_LOAD_READ_DATA,
    FF_ST_FF_PUSH_INIT,
    FF_ST_BUS_SEND,
    FF_ST_BUS_POLL_STALE,
    FF_ST_UI_BUILD,
    FF_ST_IDLE,
    FF_ST_SAVE_WRITE_MEM,
    FF_ST_SAVE_OPEN,
    FF_ST_SAVE_WRITE_CALL,
    FF_ST_SAVE_READ_DONE,
    FF_ST_SAVE_CLOSE,
    FF_ST_FF_SYNC,
    FF_ST_UI_SYNC_GET,
    FF_ST_SCENE_HIDE,
    FF_ST_SCENE_SUBMIT,
    FF_ST_FOCUS_QUERY,
    FF_ST_SCENE_STATUS,
    FF_ST_POLL_EVENTS
};

typedef struct {
    FxSdkClient sdk;
    uint32_t stage;

    /* Capabilities & bindings */
    uint32_t ui_caps;
    uint32_t scene_caps;
    uint32_t scene_max_cmds;
    uint64_t display_binding;
    uint64_t mem_binding;
    uint64_t buf_handle;
    uint64_t buf_native;
    uint64_t card_open_h;
    uint64_t card_read_h;
    uint64_t card_write_h;
    uint64_t card_close_h;
    uint64_t ff_binding;
    uint32_t ff_status;
    uint32_t ff_shot_seq;
    uint64_t bus_binding;
    uint64_t bus_endpoint;
    uint32_t bus_corr;
    uint64_t now_ms;
    uint64_t scene_retry_after_ms;

    uint8_t has_display;
    uint8_t has_global_scene;
    uint8_t has_bounce;
    uint8_t card_read_ok;
    uint8_t card_write_ok;
    uint8_t has_ff;
    uint8_t has_bus;
    uint8_t bus_init_phase;
    uint8_t bus_sync_needed;
    uint8_t bus_send_retries;
    uint8_t scene_argb_unsupported;
    uint8_t focus_foreground;
    uint64_t focus_epoch;

    /* Configuration & dual-slot persistence */
    Z8FilmFrameConfig cfg;
    Z8FilmFrameConfig slot_cfg[2];
    uint8_t slot_state[2];
    uint32_t active_slot;
    uint32_t load_slot;
    uint32_t load_retries;

    /* In-progress card I/O */
    int32_t card_fd;
    uint32_t io_done_acc;
    uint32_t io_calls;
    uint32_t io_err;
    uint32_t save_target_slot;
    Z8FilmFrameConfig save_cfg;

    /* UI build & synchronization */
    uint32_t ui_build_idx;
    uint32_t ctrl_rev[FF_UI_CTRL_COUNT + 1u];
    uint32_t ui_sync_ctrl;
    uint32_t idle_polls;

    uint8_t save_pending;
    uint8_t ff_sync_needed;
    uint8_t scene_sync_needed;
    uint8_t scene_hide_needed;
    uint8_t focus_query_needed;
    uint8_t scene_status_needed;
    uint8_t overlay_submitted;
    uint8_t overlay_pending;

    /* Counters for observability / testing */
    uint32_t saves_completed;
    uint32_t saves_failed;
    uint32_t stale_events_ignored;
    uint32_t scene_submissions;

    FxSceneCommand scene_cmds[FF_SCENE_MAX_CMDS];
    uint32_t scene_cmd_count;
} FfModuleState;

static inline void ff_zero(void *dst, uint32_t n) {
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t idx = 0u; idx < n; ++idx) {
        p[idx] = 0u;
    }
}

static inline void ff_copy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t idx = 0u; idx < n; ++idx) {
        d[idx] = s[idx];
    }
}

static inline void ff_str_copy(char *dst, uint32_t cap, const char *src) {
    if (!cap) {
        return;
    }
    uint32_t idx = 0u;
    while (idx + 1u < cap && src[idx]) {
        dst[idx] = src[idx];
        ++idx;
    }
    while (idx < cap) {
        dst[idx++] = '\0';
    }
}

static inline uint32_t ff_crc32_ieee(const uint8_t *data, uint32_t len) {
    uint32_t crc = UINT32_C(0xffffffff);
    for (uint32_t idx = 0u; idx < len; ++idx) {
        crc ^= (uint32_t)data[idx];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = (uint32_t)(0u - (crc & 1u));
            crc = (crc >> 1u) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

static inline uint32_t ff_cfg_crc32(const Z8FilmFrameConfig *cfg) {
    uint8_t raw[Z8_FILM_FRAME_CFG_BYTES];
    ff_copy(raw, cfg, Z8_FILM_FRAME_CFG_BYTES);
    raw[12] = 0u;
    raw[13] = 0u;
    raw[14] = 0u;
    raw[15] = 0u;
    return ff_crc32_ieee(raw, Z8_FILM_FRAME_CFG_BYTES);
}

static inline void ff_cfg_finalize(Z8FilmFrameConfig *cfg, uint32_t seq) {
    cfg->magic = Z8_FILM_FRAME_CFG_MAGIC;
    cfg->version = (uint16_t)Z8_FILM_FRAME_CFG_VERSION;
    cfg->bytes = (uint16_t)Z8_FILM_FRAME_CFG_BYTES;
    cfg->seq = seq;
    ff_zero(cfg->reserved, (uint32_t)sizeof(cfg->reserved));
    cfg->crc32 = 0u;
    cfg->crc32 = ff_cfg_crc32(cfg);
}

static inline void ff_cfg_default(Z8FilmFrameConfig *cfg) {
    ff_zero(cfg, (uint32_t)sizeof(*cfg));
    ff_cfg_finalize(cfg, 0u);
}

static inline int ff_cfg_is_valid(const Z8FilmFrameConfig *cfg) {
    if (!cfg) {
        return 0;
    }
    if (cfg->magic != Z8_FILM_FRAME_CFG_MAGIC ||
        cfg->version != Z8_FILM_FRAME_CFG_VERSION ||
        cfg->bytes != Z8_FILM_FRAME_CFG_BYTES) {
        return 0;
    }
    if (cfg->xpan_mode > Z8_XPAN_MODE_B_CROP ||
        cfg->border_style > Z8_BORDER_SPROCKET_DRK ||
        cfg->edge_label > Z8_EDGE_NIKON_Z8 ||
        cfg->date_format > Z8_DATE_DD_MM_YY ||
        cfg->date_style > Z8_DATE_STYLE_GR1S ||
        cfg->date_glow > Z8_DATE_GLOW_ON ||
        cfg->vf_guide > Z8_VF_GUIDE_LINES ||
        cfg->live_link > Z8_LIVE_LINK_ON) {
        return 0;
    }
    for (uint32_t idx = 0u; idx < (uint32_t)sizeof(cfg->reserved); ++idx) {
        if (cfg->reserved[idx] != 0u) {
            return 0;
        }
    }
    return cfg->crc32 == ff_cfg_crc32(cfg);
}

static inline void ff_cfg_select_dual(const Z8FilmFrameConfig *s0, int v0,
                                      const Z8FilmFrameConfig *s1, int v1,
                                      Z8FilmFrameConfig *out_cfg,
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
        ff_cfg_default(out_cfg);
        *out_active_slot = 1u; /* Next save writes slot 0 first */
    }
}

static inline void ff_slot_path(uint32_t slot, char out[32]) {
    ff_zero(out, 32u);
    if (slot == 0u) {
        ff_str_copy(out, 32u, Z8_FILM_FRAME_CFG_PATH_0);
    } else {
        ff_str_copy(out, 32u, Z8_FILM_FRAME_CFG_PATH_1);
    }
}

static inline int ff_build_ui_control(uint32_t id, uint32_t ui_caps,
                                      const Z8FilmFrameConfig *cfg,
                                      FxUiControl *out) {
    if (!cfg || !out || id < FF_UI_PAGE_ID || id > FF_UI_CTRL_COUNT) {
        return 0;
    }
    ff_zero(out, (uint32_t)sizeof(*out));
    out->abi = FX_PLATFORM_ABI;
    out->bytes = (uint32_t)sizeof(*out);
    out->id = id;
    out->flags = FX_UI_VISIBLE | FX_UI_ENABLED;

    switch (id) {
    case FF_UI_PAGE_ID:
        out->parent = 0u;
        out->kind = FX_UI_PAGE;
        if (ui_caps & FX_UI_CAP_ROOT) {
            out->flags |= FX_UI_ROOT;
        }
        ff_str_copy(out->label, FX_UI_TEXT, "Film Frame / XPan");
        return 1;

    case FF_UI_XPAN_MODE:
        out->parent = FF_UI_PAGE_ID;
        out->kind = FX_UI_CHOICE;
        out->option_count = 3u;
        out->value = cfg->xpan_mode <= 2u ? cfg->xpan_mode : 0u;
        ff_str_copy(out->label, FX_UI_TEXT, "XPan Mode");
        ff_str_copy(out->options[0], 32u, "Off 3:2");
        ff_str_copy(out->options[1], 32u, "Mode A Matte");
        ff_str_copy(out->options[2], 32u, "Mode B Crop");
        return 1;

    case FF_UI_BORDER:
        out->parent = FF_UI_PAGE_ID;
        out->kind = FX_UI_CHOICE;
        out->option_count = 3u;
        out->value = cfg->border_style <= 2u ? cfg->border_style : 0u;
        ff_str_copy(out->label, FX_UI_TEXT, "Border Style");
        ff_str_copy(out->options[0], 32u, "Off");
        ff_str_copy(out->options[1], 32u, "Light Sprocket");
        ff_str_copy(out->options[2], 32u, "Dark Sprocket");
        return 1;

    case FF_UI_EDGE_LABEL:
        out->parent = FF_UI_PAGE_ID;
        out->kind = FX_UI_CHOICE;
        out->option_count = 4u;
        out->value = cfg->edge_label <= 3u ? cfg->edge_label : 0u;
        ff_str_copy(out->label, FX_UI_TEXT, "Edge Label");
        ff_str_copy(out->options[0], 32u, "XPAN 65:24");
        ff_str_copy(out->options[1], 32u, "KODAK 400");
        ff_str_copy(out->options[2], 32u, "FUJI 400");
        ff_str_copy(out->options[3], 32u, "NIKON Z8");
        return 1;

    case FF_UI_DATE_FMT:
        out->parent = FF_UI_PAGE_ID;
        out->kind = FX_UI_CHOICE;
        out->option_count = 5u;
        out->value = cfg->date_format <= 4u ? cfg->date_format : 0u;
        ff_str_copy(out->label, FX_UI_TEXT, "Date Format");
        ff_str_copy(out->options[0], 32u, "Off");
        ff_str_copy(out->options[1], 32u, "'YY MM DD");
        ff_str_copy(out->options[2], 32u, "'YY MM DD HH:MM");
        ff_str_copy(out->options[3], 32u, "MM DD 'YY");
        ff_str_copy(out->options[4], 32u, "DD MM 'YY");
        return 1;

    case FF_UI_DATE_STYLE:
        out->parent = FF_UI_PAGE_ID;
        out->kind = FX_UI_CHOICE;
        out->option_count = 2u;
        out->value = cfg->date_style <= 1u ? cfg->date_style : 0u;
        ff_str_copy(out->label, FX_UI_TEXT, "Date Style");
        ff_str_copy(out->options[0], 32u, "Classic 7-Seg");
        ff_str_copy(out->options[1], 32u, "GR1s Dots");
        return 1;

    case FF_UI_DATE_GLOW:
        out->parent = FF_UI_PAGE_ID;
        out->kind = FX_UI_SWITCH;
        out->option_count = 0u;
        out->value = cfg->date_glow <= 1u ? cfg->date_glow : 0u;
        ff_str_copy(out->label, FX_UI_TEXT, "Date Glow");
        return 1;

    case FF_UI_VF_GUIDE:
        out->parent = FF_UI_PAGE_ID;
        out->kind = FX_UI_CHOICE;
        out->option_count = 4u;
        out->value = cfg->vf_guide <= 3u ? cfg->vf_guide : 0u;
        ff_str_copy(out->label, FX_UI_TEXT, "VF Guide");
        ff_str_copy(out->options[0], 32u, "Off");
        ff_str_copy(out->options[1], 32u, "50% Dim");
        ff_str_copy(out->options[2], 32u, "100% Black");
        ff_str_copy(out->options[3], 32u, "Frame Lines");
        return 1;

    case FF_UI_LIVE_LINK:
        out->parent = FF_UI_PAGE_ID;
        out->kind = FX_UI_SWITCH;
        out->option_count = 0u;
        out->value = cfg->live_link <= 1u ? cfg->live_link : 0u;
        ff_str_copy(out->label, FX_UI_TEXT, "Live Link");
        return 1;

    default:
        return 0;
    }
}

static inline int ff_apply_ui_change(Z8FilmFrameConfig *cfg, uint32_t control_id,
                                     uint32_t value, int *changed) {
    if (!cfg || !changed) {
        return 0;
    }
    *changed = 0;
    switch (control_id) {
    case FF_UI_XPAN_MODE:
        if (value > Z8_XPAN_MODE_B_CROP) return 0;
        if (cfg->xpan_mode != (uint8_t)value) { cfg->xpan_mode = (uint8_t)value; *changed = 1; }
        return 1;
    case FF_UI_BORDER:
        if (value > Z8_BORDER_SPROCKET_DRK) return 0;
        if (cfg->border_style != (uint8_t)value) { cfg->border_style = (uint8_t)value; *changed = 1; }
        return 1;
    case FF_UI_EDGE_LABEL:
        if (value > Z8_EDGE_NIKON_Z8) return 0;
        if (cfg->edge_label != (uint8_t)value) { cfg->edge_label = (uint8_t)value; *changed = 1; }
        return 1;
    case FF_UI_DATE_FMT:
        if (value > Z8_DATE_DD_MM_YY) return 0;
        if (cfg->date_format != (uint8_t)value) { cfg->date_format = (uint8_t)value; *changed = 1; }
        return 1;
    case FF_UI_DATE_STYLE:
        if (value > Z8_DATE_STYLE_GR1S) return 0;
        if (cfg->date_style != (uint8_t)value) { cfg->date_style = (uint8_t)value; *changed = 1; }
        return 1;
    case FF_UI_DATE_GLOW:
        if (value > Z8_DATE_GLOW_ON) return 0;
        if (cfg->date_glow != (uint8_t)value) { cfg->date_glow = (uint8_t)value; *changed = 1; }
        return 1;
    case FF_UI_VF_GUIDE:
        if (value > Z8_VF_GUIDE_LINES) return 0;
        if (cfg->vf_guide != (uint8_t)value) { cfg->vf_guide = (uint8_t)value; *changed = 1; }
        return 1;
    case FF_UI_LIVE_LINK:
        if (value > Z8_LIVE_LINK_ON) return 0;
        if (cfg->live_link != (uint8_t)value) { cfg->live_link = (uint8_t)value; *changed = 1; }
        return 1;
    default:
        return 0;
    }
}

static inline int ff_overlay_wanted(const Z8FilmFrameConfig *cfg) {
    return cfg && cfg->xpan_mode != Z8_XPAN_OFF && cfg->vf_guide != Z8_VF_GUIDE_OFF;
}

static inline uint32_t ff_build_vf_marker(const Z8FilmFrameConfig *cfg,
                                          char out[20]) {
    uint32_t len = 0u;
    const char *mode = (cfg->xpan_mode == Z8_XPAN_MODE_B_CROP) ? "XPAN B" : "XPAN A";
    while (mode[len] && len < 19u) {
        out[len] = mode[len];
        ++len;
    }
    if (cfg->border_style != Z8_BORDER_OFF && len + 2u < 20u) {
        out[len++] = ' ';
        out[len++] = 'R';
    }
    if (cfg->date_format != Z8_DATE_OFF && len + 2u < 20u) {
        out[len++] = ' ';
        out[len++] = 'D';
    }
    if (cfg->live_link != Z8_LIVE_LINK_OFF && len + 2u < 20u) {
        out[len++] = ' ';
        out[len++] = 'L';
    }
    out[len] = '\0';
    return len;
}

/* Returns 1 if active 65:24 overlay built, 0 if empty/cleared scene built. */
static inline int ff_build_vf_scene(const Z8FilmFrameConfig *cfg,
                                    FxSceneCommand *cmds,
                                    uint32_t capacity,
                                    uint32_t *out_count) {
    if (!cmds || !out_count || capacity < 1u) {
        return 0;
    }
    if (!ff_overlay_wanted(cfg)) {
        cmds[0] = fx_scene_clear(0u);
        *out_count = 1u;
        return 0;
    }
    uint32_t n = 0u;
    cmds[n++] = fx_scene_clear(0u);

    if (cfg->vf_guide == Z8_VF_GUIDE_DIM50 && n + 2u <= capacity) {
        cmds[n++] = fx_scene_rectangle(0u, FF_XPAN_TOP_Y, (uint16_t)FF_SCENE_W,
                                       FF_XPAN_BAR_H, FF_COLOR_DIM50);
        cmds[n++] = fx_scene_rectangle(0u, FF_XPAN_BOT_Y, (uint16_t)FF_SCENE_W,
                                       FF_XPAN_BAR_H, FF_COLOR_DIM50);
    } else if (cfg->vf_guide == Z8_VF_GUIDE_BLK && n + 2u <= capacity) {
        cmds[n++] = fx_scene_rectangle(0u, FF_XPAN_TOP_Y, (uint16_t)FF_SCENE_W,
                                       FF_XPAN_BAR_H, FF_COLOR_BLACK);
        cmds[n++] = fx_scene_rectangle(0u, FF_XPAN_BOT_Y, (uint16_t)FF_SCENE_W,
                                       FF_XPAN_BAR_H, FF_COLOR_BLACK);
    }

    if (n + 2u <= capacity) {
        cmds[n++] = fx_scene_rectangle(0u, (uint16_t)(FF_XPAN_BAR_H - FF_XPAN_LINE_THICK),
                                       (uint16_t)FF_SCENE_W, FF_XPAN_LINE_THICK,
                                       FF_COLOR_FRAME_LINE);
        cmds[n++] = fx_scene_rectangle(0u, (uint16_t)FF_XPAN_BOT_Y,
                                       (uint16_t)FF_SCENE_W, FF_XPAN_LINE_THICK,
                                       FF_COLOR_FRAME_LINE);
    }

    char marker[20];
    uint32_t mlen = ff_build_vf_marker(cfg, marker);
    if (mlen > 0u && n + 1u + mlen <= capacity) {
        uint16_t pill_w = (uint16_t)(mlen * 12u + 14u);
        cmds[n++] = fx_scene_rectangle(16u, 12u, pill_w, 26u, FF_COLOR_MARKER_BG);
        for (uint32_t idx = 0u; idx < mlen; ++idx) {
            cmds[n++] = fx_scene_glyph((uint16_t)(24u + idx * 12u), 18u,
                                       (uint16_t)(uint8_t)marker[idx], 2u,
                                       FF_COLOR_MARKER_AMBER);
        }
    }

    *out_count = n;
    return 1;
}

static inline FxResult ff_submit_mem_write(FfModuleState *st,
                                           const FxServiceModule *api,
                                           uint32_t offset,
                                           const void *data,
                                           uint32_t bytes) {
    uint8_t payload[sizeof(FxMemoryRequest) + FF_BOUNCE_INIT_SPAN];
    if (bytes > FF_BOUNCE_INIT_SPAN) {
        return FX_BOUNDS;
    }
    FxMemoryRequest req = {.handle = st->buf_handle, .bytes = bytes, .offset = offset};
    ff_copy(payload, &req, (uint32_t)sizeof(req));
    ff_copy(payload + sizeof(req), data, bytes);
    return fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->mem_binding,
                           FX_MEMORY_WRITE, 0u, payload,
                           (uint32_t)sizeof(req) + bytes);
}

static inline FxResult ff_submit_mem_read(FfModuleState *st,
                                          const FxServiceModule *api,
                                          uint32_t offset,
                                          uint32_t bytes) {
    FxMemoryRequest req = {.handle = st->buf_handle, .bytes = bytes, .offset = offset};
    return fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->mem_binding,
                           FX_MEMORY_READ, 0u, &req, (uint32_t)sizeof(req));
}

static inline void ff_handle_ui_event(FfModuleState *st, const FxServiceReply *r) {
    if (r->bytes != sizeof(FxUiEvent)) {
        return;
    }
    FxUiEvent ev;
    ff_copy(&ev, r->data, (uint32_t)sizeof(ev));

    if (ev.event == FX_INPUT_FOCUS) {
        st->focus_foreground = 1u;
        st->focus_epoch = ev.reserved;
        return;
    }
    if (ev.event == FX_INPUT_BLUR) {
        st->focus_foreground = 0u;
        st->focus_epoch = ev.reserved;
        st->scene_retry_after_ms = 0u;
        if (ff_overlay_wanted(&st->cfg) || st->overlay_submitted) {
            st->scene_sync_needed = 1u;
        }
        return;
    }
    if (ev.event == FX_INPUT_CHANGE &&
        ev.control >= FF_UI_XPAN_MODE && ev.control <= FF_UI_CTRL_COUNT) {
        if (st->ui_caps & FX_UI_CAP_EVENT_REVISION) {
            uint32_t ev_rev = fx_sdk_event_revision(&ev);
            uint32_t expected_rev = st->ctrl_rev[ev.control];
            if (ev_rev != 0u && expected_rev != 0u && ev_rev != expected_rev) {
                ++st->stale_events_ignored;
                return;
            }
        }
        Z8FilmFrameConfig next = st->cfg;
        int changed = 0;
        if (!ff_apply_ui_change(&next, ev.control, ev.value, &changed)) {
            return;
        }
        st->ui_sync_ctrl = ev.control;
        if (changed) {
            uint32_t next_seq = (st->cfg.seq < UINT32_MAX) ? (st->cfg.seq + 1u) : st->cfg.seq;
            ff_cfg_finalize(&next, next_seq);
            st->cfg = next;
            st->save_pending = 1u;
            if (st->has_ff) {
                st->ff_sync_needed = 1u;
            } else if (st->has_bus) {
                st->bus_sync_needed = 1u;
            }
            if (ff_overlay_wanted(&st->cfg) || st->overlay_submitted) {
                st->scene_sync_needed = 1u;
            }
        }
    }
}

static inline void ff_start_after_binding_probe(FfModuleState *st) {
    if (st->card_read_ok) {
        st->load_slot = 0u;
        st->stage = FF_ST_LOAD_WRITE_PATH;
        return;
    }
    ff_cfg_default(&st->cfg);
    st->active_slot = 1u;
    if (st->has_ff) {
        st->stage = FF_ST_FF_PUSH_INIT;
    } else if (st->has_bus) {
        st->bus_init_phase = 1u;
        st->bus_send_retries = 0u;
        st->stage = FF_ST_BUS_SEND;
    } else {
        st->stage = FF_ST_UI_BUILD;
    }
}

static inline void ff_finish_card_load(FfModuleState *st) {
    int v0 = st->slot_state[0] == FF_SLOT_VALID;
    int v1 = st->slot_state[1] == FF_SLOT_VALID;
    int io_err = (st->slot_state[0] == FF_SLOT_IO_ERR ||
                  st->slot_state[1] == FF_SLOT_IO_ERR);

    if (!v0 && !v1 && io_err && st->load_retries < FF_LOAD_MAX_RETRIES) {
        ++st->load_retries;
        st->load_slot = 0u;
        st->slot_state[0] = FF_SLOT_UNREAD;
        st->slot_state[1] = FF_SLOT_UNREAD;
        st->stage = FF_ST_LOAD_WRITE_PATH;
        return;
    }

    ff_cfg_select_dual(&st->slot_cfg[0], v0, &st->slot_cfg[1], v1,
                       &st->cfg, &st->active_slot);
    if (st->has_ff) {
        st->stage = FF_ST_FF_PUSH_INIT;
    } else if (st->has_bus) {
        st->bus_init_phase = 1u;
        st->bus_send_retries = 0u;
        st->stage = FF_ST_BUS_SEND;
    } else {
        st->stage = FF_ST_UI_BUILD;
    }
}

static inline void ff_advance_after_slot_load(FfModuleState *st) {
    if (st->load_slot == 0u) {
        st->load_slot = 1u;
        st->stage = FF_ST_LOAD_WRITE_PATH;
    } else {
        ff_finish_card_load(st);
    }
}

static inline int ff_handle_reply(FfModuleState *st, const FxServiceReply *r) {
    if (st->stage == FF_ST_SCENE_SUBMIT &&
        (r->result == FX_BUSY || r->result == FX_STATE)) {
        st->scene_sync_needed = 0u;
        st->scene_retry_after_ms = st->now_ms + 500u;
        st->overlay_submitted = 0u;
        st->overlay_pending = ff_overlay_wanted(&st->cfg) ? 1u : 0u;
        st->stage = FF_ST_IDLE;
        return 0;
    }
    if ((st->stage == FF_ST_BUS_SEND || st->stage == FF_ST_BUS_POLL_STALE) &&
        r->result == FX_BUSY) {
        ++st->bus_send_retries;
        if (st->stage == FF_ST_BUS_SEND) {
            if (st->bus_send_retries == 1u) {
                return 0; /* First FX_BUSY may be lock contention: retry send once */
            }
            if (st->bus_send_retries < 8u) {
                st->stage = FF_ST_BUS_POLL_STALE;
                return 0;
            }
        } else if (st->bus_send_retries < 8u) {
            return 0; /* Retry FX_BUS_POLL on lock contention */
        }
        if (st->bus_init_phase) {
            st->bus_init_phase = 0u;
            st->stage = FF_ST_UI_BUILD;
        } else {
            st->stage = FF_ST_IDLE;
        }
        return 0;
    }
    if (r->result == FX_BUSY) {
        return 0; /* Retry current stage unchanged */
    }

    switch (st->stage) {
    case FF_ST_OPEN_SCENE:
        if (r->result == FX_OK) {
            st->display_binding = r->value[0];
            st->has_display = 1u;
            st->stage = FF_ST_QUERY_SCENE;
        } else {
            st->has_display = 0u;
            st->stage = FF_ST_QUERY_UI;
        }
        return 0;

    case FF_ST_QUERY_SCENE:
        if (r->result == FX_OK && r->bytes == sizeof(FxSceneInfo)) {
            FxSceneInfo info;
            ff_copy(&info, r->data, (uint32_t)sizeof(info));
            if (info.abi == FX_SCENE_ABI) {
                st->scene_caps = info.capabilities;
                st->scene_max_cmds = info.max_commands;
                st->has_global_scene = (info.capabilities & FX_SCENE_CAP_GLOBAL) ? 1u : 0u;
            }
        }
        st->stage = FF_ST_QUERY_UI;
        return 0;

    case FF_ST_QUERY_UI:
        if (r->result == FX_OK && r->bytes == sizeof(FxUiInfo)) {
            FxUiInfo info;
            ff_copy(&info, r->data, (uint32_t)sizeof(info));
            if (info.abi == 1u && info.bytes == sizeof(info)) {
                st->ui_caps = info.capabilities;
            }
            st->stage = FF_ST_OPEN_MEM;
        } else if (r->result == FX_ARGUMENT || r->result == FX_CAPABILITY) {
            st->ui_caps = 0u;
            st->stage = FF_ST_OPEN_MEM;
        } else {
            return 4;
        }
        return 0;

    case FF_ST_OPEN_MEM:
        if (r->result == FX_OK) {
            st->mem_binding = r->value[0];
            st->stage = FF_ST_ALLOC_MEM;
        } else {
            st->mem_binding = 0u;
            st->has_bounce = 0u;
            st->stage = FF_ST_OPEN_FF;
        }
        return 0;

    case FF_ST_ALLOC_MEM:
        if (r->result == FX_OK && r->value[1] != 0u) {
            st->buf_handle = r->value[0];
            st->buf_native = r->value[1];
            st->has_bounce = 1u;
            st->stage = FF_ST_OPEN_CARD_OPEN;
        } else {
            st->has_bounce = 0u;
            st->stage = FF_ST_OPEN_FF;
        }
        return 0;

    case FF_ST_OPEN_CARD_OPEN:
        st->card_open_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->stage = FF_ST_OPEN_CARD_READ;
        return 0;

    case FF_ST_OPEN_CARD_READ:
        st->card_read_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->stage = FF_ST_OPEN_CARD_WRITE;
        return 0;

    case FF_ST_OPEN_CARD_WRITE:
        st->card_write_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->stage = FF_ST_OPEN_CARD_CLOSE;
        return 0;

    case FF_ST_OPEN_CARD_CLOSE:
        st->card_close_h = (r->result == FX_OK) ? r->value[0] : 0u;
        st->card_read_ok = (st->has_bounce && st->card_open_h &&
                            st->card_read_h && st->card_close_h) ? 1u : 0u;
        st->card_write_ok = (st->card_read_ok && st->card_write_h) ? 1u : 0u;
        st->stage = FF_ST_OPEN_FF;
        return 0;

    case FF_ST_OPEN_FF:
        if (r->result == FX_OK) {
            st->ff_binding = r->value[0];
            st->has_ff = 1u;
            st->stage = FF_ST_FF_STATUS;
        } else {
            st->ff_binding = 0u;
            st->has_ff = 0u;
            st->stage = FF_ST_OPEN_BUS;
        }
        return 0;

    case FF_ST_FF_STATUS:
        if (r->result == FX_OK) {
            st->ff_status = (uint32_t)r->value[0];
            st->ff_shot_seq = (uint32_t)r->value[1];
            ff_start_after_binding_probe(st);
        } else {
            st->has_ff = 0u;
            st->ff_binding = 0u;
            st->stage = FF_ST_OPEN_BUS;
        }
        return 0;

    case FF_ST_OPEN_BUS:
        if (r->result == FX_OK) {
            st->bus_binding = r->value[0];
            st->stage = FF_ST_BUS_PUBLISH;
        } else {
            st->bus_binding = 0u;
            st->has_bus = 0u;
            ff_start_after_binding_probe(st);
        }
        return 0;

    case FF_ST_BUS_PUBLISH:
        if (r->result == FX_OK) {
            st->bus_endpoint = r->value[0];
            st->stage = FF_ST_BUS_RESERVE;
        } else {
            st->bus_endpoint = 0u;
            st->has_bus = 0u;
            ff_start_after_binding_probe(st);
        }
        return 0;

    case FF_ST_BUS_RESERVE:
        st->has_bus = (r->result == FX_OK) ? 1u : 0u;
        ff_start_after_binding_probe(st);
        return 0;

    case FF_ST_LOAD_WRITE_PATH:
        if (r->result != FX_OK) {
            st->slot_state[st->load_slot] = FF_SLOT_IO_ERR;
            ff_advance_after_slot_load(st);
        } else {
            st->card_fd = -1;
            st->io_done_acc = 0u;
            st->io_calls = 0u;
            st->io_err = 0u;
            st->stage = FF_ST_LOAD_OPEN;
        }
        return 0;

    case FF_ST_LOAD_OPEN:
        if (r->result != FX_OK) {
            st->slot_state[st->load_slot] = FF_SLOT_IO_ERR;
            ff_advance_after_slot_load(st);
        } else {
            int32_t fd = (int32_t)(uint32_t)r->value[0];
            if (fd >= 0) {
                st->card_fd = fd;
                st->stage = FF_ST_LOAD_READ_CALL;
            } else if ((uint32_t)fd == FF_OPEN_NOT_FOUND) {
                st->slot_state[st->load_slot] = FF_SLOT_MISSING;
                ff_advance_after_slot_load(st);
            } else {
                st->slot_state[st->load_slot] = FF_SLOT_IO_ERR;
                ff_advance_after_slot_load(st);
            }
        }
        return 0;

    case FF_ST_LOAD_READ_CALL:
        if (r->result != FX_OK || (uint32_t)r->value[0] != 0u) {
            st->io_err = 1u;
            st->stage = FF_ST_LOAD_CLOSE;
        } else {
            st->stage = FF_ST_LOAD_READ_DONE;
        }
        return 0;

    case FF_ST_LOAD_READ_DONE:
        if (r->result != FX_OK || r->bytes != 4u) {
            st->io_err = 1u;
            st->stage = FF_ST_LOAD_CLOSE;
        } else {
            uint32_t done = 0u;
            ff_copy(&done, r->data, 4u);
            if (done > FF_READ_MAX_BYTES - st->io_done_acc) {
                st->io_err = 1u;
                st->stage = FF_ST_LOAD_CLOSE;
            } else {
                st->io_done_acc += done;
                ++st->io_calls;
                if (done > 0u && st->io_done_acc < FF_READ_MAX_BYTES &&
                    st->io_calls < FF_IO_MAX_CALLS) {
                    st->stage = FF_ST_LOAD_READ_CALL;
                } else {
                    st->stage = FF_ST_LOAD_CLOSE;
                }
            }
        }
        return 0;

    case FF_ST_LOAD_CLOSE: {
        uint32_t close_err = (r->result == FX_OK) ? (uint32_t)r->value[0] : 1u;
        st->card_fd = -1;
        if (close_err != 0u || st->io_err) {
            st->slot_state[st->load_slot] = FF_SLOT_IO_ERR;
            ff_advance_after_slot_load(st);
        } else if (st->io_done_acc != Z8_FILM_FRAME_CFG_BYTES) {
            st->slot_state[st->load_slot] = FF_SLOT_CORRUPT;
            ff_advance_after_slot_load(st);
        } else {
            st->stage = FF_ST_LOAD_READ_DATA;
        }
        return 0;
    }

    case FF_ST_LOAD_READ_DATA:
        if (r->result != FX_OK || r->bytes != Z8_FILM_FRAME_CFG_BYTES) {
            st->slot_state[st->load_slot] = FF_SLOT_IO_ERR;
        } else {
            Z8FilmFrameConfig tmp;
            ff_copy(&tmp, r->data, Z8_FILM_FRAME_CFG_BYTES);
            if (ff_cfg_is_valid(&tmp)) {
                st->slot_cfg[st->load_slot] = tmp;
                st->slot_state[st->load_slot] = FF_SLOT_VALID;
            } else {
                st->slot_state[st->load_slot] = FF_SLOT_CORRUPT;
            }
        }
        ff_advance_after_slot_load(st);
        return 0;

    case FF_ST_FF_PUSH_INIT:
        if (r->result != FX_OK) {
            st->has_ff = 0u;
            st->ff_binding = 0u;
            if (st->has_bus) {
                st->bus_init_phase = 1u;
                st->bus_send_retries = 0u;
                st->stage = FF_ST_BUS_SEND;
                return 0;
            }
        }
        st->stage = FF_ST_UI_BUILD;
        return 0;

    case FF_ST_BUS_SEND:
        if (r->result != FX_OK) {
            st->has_bus = 0u;
            st->bus_binding = 0u;
            st->bus_endpoint = 0u;
        }
        if (st->bus_init_phase) {
            st->bus_init_phase = 0u;
            st->stage = FF_ST_UI_BUILD;
        } else {
            st->stage = FF_ST_IDLE;
        }
        return 0;

    case FF_ST_BUS_POLL_STALE:
        st->stage = FF_ST_BUS_SEND;
        return 0;

    case FF_ST_UI_BUILD:
        if (r->result != FX_OK) {
            return 5;
        }
        st->ctrl_rev[st->ui_build_idx + 1u] = (uint32_t)r->value[0];
        ++st->ui_build_idx;
        if (st->ui_build_idx >= FF_UI_CTRL_COUNT) {
            if (ff_overlay_wanted(&st->cfg) && st->has_display && st->has_global_scene) {
                st->scene_sync_needed = 1u;
            }
            st->stage = FF_ST_IDLE;
        }
        return 0;

    case FF_ST_SAVE_WRITE_MEM:
        if (r->result != FX_OK) {
            ++st->saves_failed;
            st->stage = FF_ST_IDLE;
        } else {
            st->card_fd = -1;
            st->io_done_acc = 0u;
            st->io_calls = 0u;
            st->io_err = 0u;
            st->stage = FF_ST_SAVE_OPEN;
        }
        return 0;

    case FF_ST_SAVE_OPEN:
        if (r->result != FX_OK) {
            ++st->saves_failed;
            st->stage = FF_ST_IDLE;
        } else {
            int32_t fd = (int32_t)(uint32_t)r->value[0];
            if (fd < 0) {
                ++st->saves_failed;
                st->stage = FF_ST_IDLE;
            } else {
                st->card_fd = fd;
                st->stage = FF_ST_SAVE_WRITE_CALL;
            }
        }
        return 0;

    case FF_ST_SAVE_WRITE_CALL:
        if (r->result != FX_OK || (uint32_t)r->value[0] != 0u) {
            st->io_err = 1u;
            st->stage = FF_ST_SAVE_CLOSE;
        } else {
            st->stage = FF_ST_SAVE_READ_DONE;
        }
        return 0;

    case FF_ST_SAVE_READ_DONE:
        if (r->result != FX_OK || r->bytes != 4u) {
            st->io_err = 1u;
            st->stage = FF_ST_SAVE_CLOSE;
        } else {
            uint32_t done = 0u;
            ff_copy(&done, r->data, 4u);
            if (done == 0u || done > Z8_FILM_FRAME_CFG_BYTES - st->io_done_acc) {
                st->io_err = 1u;
                st->stage = FF_ST_SAVE_CLOSE;
            } else {
                st->io_done_acc += done;
                ++st->io_calls;
                if (st->io_done_acc < Z8_FILM_FRAME_CFG_BYTES &&
                    st->io_calls < FF_IO_MAX_CALLS) {
                    st->stage = FF_ST_SAVE_WRITE_CALL;
                } else {
                    st->stage = FF_ST_SAVE_CLOSE;
                }
            }
        }
        return 0;

    case FF_ST_SAVE_CLOSE: {
        uint32_t close_err = (r->result == FX_OK) ? (uint32_t)r->value[0] : 1u;
        st->card_fd = -1;
        if (!st->io_err && close_err == 0u &&
            st->io_done_acc == Z8_FILM_FRAME_CFG_BYTES) {
            st->active_slot = st->save_target_slot;
            st->slot_cfg[st->active_slot] = st->save_cfg;
            st->slot_state[st->active_slot] = FF_SLOT_VALID;
            ++st->saves_completed;
        } else {
            ++st->saves_failed;
        }
        st->stage = FF_ST_IDLE;
        return 0;
    }

    case FF_ST_FF_SYNC:
        if (r->result != FX_OK) {
            st->has_ff = 0u;
            st->ff_binding = 0u;
        }
        st->stage = FF_ST_IDLE;
        return 0;

    case FF_ST_UI_SYNC_GET:
        if (r->result == FX_OK && st->ui_sync_ctrl <= FF_UI_CTRL_COUNT) {
            st->ctrl_rev[st->ui_sync_ctrl] = (uint32_t)r->value[0];
        }
        st->ui_sync_ctrl = 0u;
        st->stage = FF_ST_IDLE;
        return 0;

    case FF_ST_SCENE_HIDE:
        st->scene_hide_needed = 0u;
        st->overlay_submitted = 0u;
        st->overlay_pending = 0u;
        st->stage = FF_ST_IDLE;
        return 0;

    case FF_ST_SCENE_SUBMIT:
        if (r->result == FX_OK) {
            ++st->scene_submissions;
            st->scene_sync_needed = 0u;
            st->scene_retry_after_ms = 0u;
            st->overlay_pending = 0u;
            if (ff_overlay_wanted(&st->cfg)) {
                st->overlay_submitted = 1u;
            } else {
                st->overlay_submitted = 0u;
                if (!st->has_ff) {
                    st->scene_hide_needed = 1u;
                }
            }
            ff_handle_ui_event(st, r);
        } else if (r->result == FX_BOUNDS) {
            st->scene_sync_needed = 0u;
            st->overlay_pending = 0u;
            st->scene_argb_unsupported = 1u;
        } else if (r->result == FX_CAPABILITY) {
            st->scene_sync_needed = 0u;
            st->overlay_pending = 0u;
            st->has_global_scene = 0u;
        }
        st->stage = FF_ST_IDLE;
        return 0;

    case FF_ST_FOCUS_QUERY:
        st->focus_query_needed = 0u;
        if (r->result == FX_OK && r->bytes == sizeof(FxUiFocus)) {
            FxUiFocus focus;
            ff_copy(&focus, r->data, (uint32_t)sizeof(focus));
            if (focus.abi == 1u && focus.bytes == sizeof(focus) &&
                focus.foreground <= 1u) {
                uint8_t prev_fg = st->focus_foreground;
                st->focus_foreground = (uint8_t)focus.foreground;
                if (focus.epoch != st->focus_epoch) {
                    st->focus_epoch = focus.epoch;
                    if (!st->focus_foreground && ff_overlay_wanted(&st->cfg)) {
                        st->scene_sync_needed = 1u;
                    }
                } else if (prev_fg && !st->focus_foreground && ff_overlay_wanted(&st->cfg)) {
                    st->scene_sync_needed = 1u;
                }
            }
        }
        st->stage = FF_ST_IDLE;
        return 0;

    case FF_ST_SCENE_STATUS:
        st->scene_status_needed = 0u;
        if (r->result == FX_OK) {
            uint64_t attached = r->value[1];
            if (!attached && ff_overlay_wanted(&st->cfg) && !st->focus_foreground) {
                st->overlay_submitted = 0u;
                st->scene_sync_needed = 1u;
            }
        }
        st->stage = FF_ST_IDLE;
        return 0;

    case FF_ST_POLL_EVENTS:
        if (r->result == FX_OK) {
            st->idle_polls = 0u;
            ff_handle_ui_event(st, r);
        } else if (r->result == FX_EMPTY) {
            ++st->idle_polls;
            if ((st->ui_caps & FX_UI_CAP_FOCUS) && (st->idle_polls % 8u == 0u)) {
                st->focus_query_needed = 1u;
            }
            if (!st->focus_foreground &&
                ff_overlay_wanted(&st->cfg) &&
                st->has_display && st->has_global_scene &&
                !st->scene_argb_unsupported) {
                if (st->overlay_pending && (st->idle_polls % 4u == 2u) &&
                    (st->scene_retry_after_ms == 0u ||
                     st->now_ms >= st->scene_retry_after_ms)) {
                    st->scene_sync_needed = 1u;
                } else if (st->overlay_submitted && (st->idle_polls % 16u == 12u)) {
                    st->scene_status_needed = 1u;
                }
            }
        }
        st->stage = FF_ST_IDLE;
        return 0;

    default:
        return 6;
    }
}

static inline void ff_schedule_idle_work(FfModuleState *st) {
    if (st->stage != FF_ST_IDLE) {
        return;
    }
    if (st->save_pending) {
        st->save_pending = 0u;
        if (st->has_bounce && st->card_write_ok) {
            st->save_target_slot = st->active_slot ^ 1u;
            st->save_cfg = st->cfg;
            st->stage = FF_ST_SAVE_WRITE_MEM;
            return;
        }
    }
    if (st->ff_sync_needed && st->has_ff) {
        st->ff_sync_needed = 0u;
        st->stage = FF_ST_FF_SYNC;
        return;
    }
    if (st->bus_sync_needed && st->has_bus) {
        st->bus_sync_needed = 0u;
        st->bus_init_phase = 0u;
        st->bus_send_retries = 0u;
        st->stage = FF_ST_BUS_SEND;
        return;
    }
    if (st->ui_sync_ctrl != 0u) {
        st->stage = FF_ST_UI_SYNC_GET;
        return;
    }
    if (st->scene_hide_needed && st->has_display) {
        st->stage = FF_ST_SCENE_HIDE;
        return;
    }
    if (st->scene_sync_needed && st->has_display &&
        st->has_global_scene && !st->scene_argb_unsupported &&
        (st->scene_retry_after_ms == 0u ||
         st->now_ms >= st->scene_retry_after_ms)) {
        st->stage = FF_ST_SCENE_SUBMIT;
        return;
    }
    if (st->focus_query_needed && (st->ui_caps & FX_UI_CAP_FOCUS)) {
        st->stage = FF_ST_FOCUS_QUERY;
        return;
    }
    if (st->scene_status_needed && st->has_display &&
        st->has_global_scene && !st->scene_argb_unsupported) {
        st->stage = FF_ST_SCENE_STATUS;
        return;
    }
    st->stage = FF_ST_POLL_EVENTS;
}

static inline int ff_module_step(FfModuleState *st, const FxModuleServices *base) {
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
            return 3;
        }
        int err = ff_handle_reply(st, &reply);
        if (err != 0) {
            return err;
        }
    }

    ff_schedule_idle_work(st);

    FxResult res = FX_ARGUMENT;
    switch (st->stage) {
    case FF_ST_OPEN_SCENE:
        res = fx_sdk_open(&st->sdk, api, "camera.display.scene", 1u);
        break;
    case FF_ST_QUERY_SCENE:
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->display_binding,
                              FX_SCENE_QUERY, 0u, 0, 0u);
        break;
    case FF_ST_QUERY_UI:
        res = fx_sdk_ui_query(&st->sdk, api, 0u);
        break;
    case FF_ST_OPEN_MEM:
        res = fx_sdk_open(&st->sdk, api, "core.memory", 1u);
        break;
    case FF_ST_ALLOC_MEM:
        res = fx_sdk_memory_allocate(&st->sdk, api, st->mem_binding,
                                     FF_BOUNCE_BYTES, 1u);
        break;
    case FF_ST_OPEN_CARD_OPEN:
        res = fx_sdk_open(&st->sdk, api, "camera.card.open", 1u);
        break;
    case FF_ST_OPEN_CARD_READ:
        res = fx_sdk_open(&st->sdk, api, "camera.card.read", 1u);
        break;
    case FF_ST_OPEN_CARD_WRITE:
        res = fx_sdk_open(&st->sdk, api, "camera.card.write", 1u);
        break;
    case FF_ST_OPEN_CARD_CLOSE:
        res = fx_sdk_open(&st->sdk, api, "camera.card.close", 1u);
        break;
    case FF_ST_OPEN_FF:
        res = fx_sdk_open(&st->sdk, api, Z8_FILM_FRAME_BINDING_NAME,
                          Z8_FILM_FRAME_BINDING_VER);
        break;
    case FF_ST_FF_STATUS:
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->ff_binding,
                              Z8_FF_OP_STATUS, 0u, 0, 0u);
        break;
    case FF_ST_OPEN_BUS:
        res = fx_sdk_open(&st->sdk, api, "core.services", 1u);
        break;
    case FF_ST_BUS_PUBLISH:
        res = fx_sdk_bus_named(&st->sdk, api, st->bus_binding, FX_BUS_PUBLISH,
                               Z8_FILM_FRAME_BINDING_NAME,
                               Z8_FILM_FRAME_BINDING_VER);
        break;
    case FF_ST_BUS_RESERVE:
        res = fx_sdk_bus_reserve(&st->sdk, api, st->bus_binding,
                                 st->bus_endpoint, 2u);
        break;
    case FF_ST_LOAD_WRITE_PATH: {
        char path[32];
        ff_slot_path(st->load_slot, path);
        res = ff_submit_mem_write(st, api, FF_BOUNCE_PATH_OFF, path, 32u);
        break;
    }
    case FF_ST_LOAD_OPEN: {
        uint64_t args[8] = {
            st->buf_native + FF_BOUNCE_PATH_OFF,
            0u, /* read-only */
            0u, 0u, 0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_open_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case FF_ST_LOAD_READ_CALL: {
        uint64_t args[8] = {
            (uint64_t)(uint32_t)st->card_fd,
            st->buf_native + FF_BOUNCE_DATA_OFF + st->io_done_acc,
            FF_READ_MAX_BYTES - st->io_done_acc,
            st->buf_native + FF_BOUNCE_DONE_OFF,
            0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_read_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case FF_ST_LOAD_READ_DONE:
        res = ff_submit_mem_read(st, api, FF_BOUNCE_DONE_OFF, 4u);
        break;
    case FF_ST_LOAD_CLOSE: {
        uint64_t args[8] = {(uint64_t)(uint32_t)st->card_fd, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_close_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case FF_ST_LOAD_READ_DATA:
        res = ff_submit_mem_read(st, api, FF_BOUNCE_DATA_OFF, Z8_FILM_FRAME_CFG_BYTES);
        break;
    case FF_ST_FF_PUSH_INIT:
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->ff_binding,
                              Z8_FF_OP_SET, 0u, &st->cfg, Z8_FILM_FRAME_CFG_BYTES);
        break;
    case FF_ST_BUS_SEND:
        res = fx_sdk_bus_send(&st->sdk, api, st->bus_binding, st->bus_endpoint,
                              ++st->bus_corr, &st->cfg,
                              Z8_FILM_FRAME_CFG_BYTES);
        break;
    case FF_ST_BUS_POLL_STALE:
        res = fx_sdk_bus_poll(&st->sdk, api, st->bus_binding, st->bus_endpoint);
        break;
    case FF_ST_UI_BUILD: {
        FxUiControl ctrl;
        if (!ff_build_ui_control(st->ui_build_idx + 1u, st->ui_caps, &st->cfg, &ctrl)) {
            return 7;
        }
        res = fx_sdk_ui(&st->sdk, api, &ctrl);
        break;
    }
    case FF_ST_SAVE_WRITE_MEM: {
        uint8_t block[FF_BOUNCE_INIT_SPAN];
        ff_zero(block, (uint32_t)sizeof(block));
        char path[32];
        ff_slot_path(st->save_target_slot, path);
        ff_copy(block + FF_BOUNCE_PATH_OFF, path, 32u);
        ff_copy(block + FF_BOUNCE_DATA_OFF, &st->save_cfg, Z8_FILM_FRAME_CFG_BYTES);
        res = ff_submit_mem_write(st, api, 0u, block, (uint32_t)sizeof(block));
        break;
    }
    case FF_ST_SAVE_OPEN: {
        uint64_t args[8] = {
            st->buf_native + FF_BOUNCE_PATH_OFF,
            UINT64_C(0x301), /* write + create + truncate */
            0u, 0u, 0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_open_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case FF_ST_SAVE_WRITE_CALL: {
        uint64_t args[8] = {
            (uint64_t)(uint32_t)st->card_fd,
            st->buf_native + FF_BOUNCE_DATA_OFF + st->io_done_acc,
            Z8_FILM_FRAME_CFG_BYTES - st->io_done_acc,
            st->buf_native + FF_BOUNCE_DONE_OFF,
            0u, 0u, 0u, 0u
        };
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_write_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case FF_ST_SAVE_READ_DONE:
        res = ff_submit_mem_read(st, api, FF_BOUNCE_DONE_OFF, 4u);
        break;
    case FF_ST_SAVE_CLOSE: {
        uint64_t args[8] = {(uint64_t)(uint32_t)st->card_fd, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->card_close_h,
                              0u, 0u, args, (uint32_t)sizeof(args));
        break;
    }
    case FF_ST_FF_SYNC:
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->ff_binding,
                              Z8_FF_OP_SET, 0u, &st->cfg, Z8_FILM_FRAME_CFG_BYTES);
        break;
    case FF_ST_UI_SYNC_GET:
        res = fx_sdk_ui_get(&st->sdk, api, st->ui_sync_ctrl);
        break;
    case FF_ST_SCENE_HIDE:
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->display_binding,
                              FX_SCENE_HIDE, 0u, 0, 0u);
        break;
    case FF_ST_SCENE_SUBMIT:
        (void)ff_build_vf_scene(&st->cfg, st->scene_cmds, FF_SCENE_MAX_CMDS,
                                &st->scene_cmd_count);
        res = fx_sdk_scene_submit_global(&st->sdk, api, st->display_binding,
                                         FF_SCENE_W, FF_SCENE_H,
                                         st->scene_cmds, st->scene_cmd_count);
        break;
    case FF_ST_FOCUS_QUERY:
        res = fx_sdk_ui_query(&st->sdk, api, 1u);
        break;
    case FF_ST_SCENE_STATUS:
        res = fx_sdk_platform(&st->sdk, api, FX_BIND_CALL, st->display_binding,
                              FX_SCENE_STATUS, 0u, 0, 0u);
        break;
    case FF_ST_POLL_EVENTS:
        res = fx_sdk_events(&st->sdk, api);
        break;
    default:
        return 8;
    }

    return res == FX_OK ? 0 : 9;
}

#endif /* FILMFRAME_CORE_H */
