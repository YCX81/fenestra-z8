/* Card-stored Picture Control slots (C10-C64) with next-boot A/B card selector
 * and FontCJK ZH-CN / ZH-TW UI localization (SLOTS_LANG_GET op 15).
 *
 * Boot behavior:
 *   - Queries slots.state op 15 (SLOTS_LANG_GET, magic 'LANG') immediately after
 *     opening slots.state (BEFORE SLOTS_INFO, so c64-gate stays at LIMIT=9 until
 *     a target card is ready) and polls every 1s in all states for live language
 *     switching (8 = zh-CN, 9 = zh-TW, else English fallback).
 *   - Reads dual-slot CRC32 config SLTCARD0.CFG / SLTCARD1.CFG from the Fenestra
 *     boot card (A:\FENESTRA\..., routed to B:\FENESTRA\ by cardslot.c if booted
 *     from SD).
 *   - Mode Auto: uses Fenestra card.files (identical to original slotcard.wlm).
 *   - Mode CFexpress (A) or SD (B) [Strict Mode]:
 *       * Verifies the chosen card is mounted and has a \FENESTRA directory
 *         (retrying mount up to 8s while keeping UI responsive).
 *       * If Mode A is requested while cardslot.c latched drive=2 (booted SD),
 *         refuses to access A:\FENESTRA (which cardslot.c would redirect to B)
 *         and reports "Card A missing (booted SD)" (localized).
 *       * When the target card is missing, NEVER calls SLOTS_INFO (so c64-gate
 *         keeps LIMIT=9 / stock C1-C9 only), loads zero extended slots, never
 *         writes to the other card, displays missing state, and keeps the card
 *         selector row interactive so the user can switch back and reboot cleanly.
 *   - Changing the card selector writes SLTCARD0/1.CFG on the boot card and shows
 *     "(next boot)" / "(重启生效)" / "(重啟生效)" until restart.
 */
#include "fenestra_sdk.h"
#include "fenestra_file_sdk.h"
#include "fenestra_machine.h"
#include "slotcard.h"
#include "slotcard_cfg.h"

#define POLL_MS         2000u
#define SUMMARY_POLL_MS 10000u
#define SEL_DEBOUNCE_MS 10000u
#define BOOT_SETTLE_MS  500u
#define LANG_POLL_MS    1000u
#define PROBE_MS        250u
#define WAIT_MOUNT_MS   8000u
#define CHUNK           700u
#define DEFAULT_PHOTO   0x258u
#define BOUNCE_BYTES    1024u
#define BOUNCE_PATH_OFF 0u
#define BOUNCE_OUT_OFF  64u
#define BOUNCE_DATA_OFF 128u
#define CFG_IO_MAX_CALLS 16u
#define LANG_MAGIC      UINT32_C(0x4c414e47) /* 'LANG' */
#define LANG_ZH_CN      8u
#define LANG_ZH_TW      9u
#define SC_UI_MAX       (FX_UI_TEXT - 1u)

enum {
    PAGE = 1u,
    R_LIMIT = 2u,
    R_DRIVE = 3u,
    R_DEBUG = 4u,
    R_STATE = 5u,
    R_STORE = 6u,
    R_CSL1  = 7u,
    CONTROLS = R_CSL1
};
_Static_assert(CONTROLS <= FX_UI_CONTROLS, "slotcard controls must fit inline FX_UI_CONTROLS");
enum {
    SLOTS_READ = 1u, SLOTS_WRITE, SLOTS_COMMIT, SLOTS_SELECTION, SLOTS_APPLY,
    SLOTS_INVALIDATE, SLOTS_SUMMARY, SLOTS_HASH, SLOTS_STATUS_SET, SLOTS_INFO,
    SLOTS_LIMIT, SLOTS_PROXY, SLOTS_LANG_GET = 15u
};
enum {
    ST_START = 0u,
    ST_LOADING = 1u,
    ST_INJECTING = 2u,
    ST_APPLYING = 3u,
    ST_RUNNING = 4u,
    ST_OTHER_CARD = 5u,
    ST_WAITING_CARD = 6u,
    ST_CARD_MISSING = 7u
};

typedef struct { uint32_t index, offset, bytes, value; } SlotsRequest;

static FxSdkClient client;
static uint64_t files, slots, handle;
static uint64_t mem_h, buf_h, buf_native, machine_h;
static uint64_t card_open_h, card_read_h, card_write_h, card_close_h;
static uint64_t dir_open_h, dir_close_h;
static uint8_t has_native_io, has_dir_io, has_machine;

static Sc store;
static ScIo io;
static ScReply answer;
static FxServiceReply reply;

static uint32_t pc, row, i, offset, seen, moved, held, sent_proxy[2], proxy_sent;
static uint32_t base_crc[SC_SLOTS], summary, pending_summary, selected[2], legal[2];
static uint32_t state, load_status, injected, inject_failed, applied, apply_failed;
static uint32_t commits, commit_status, checks_other, used_count, want_commit;
static uint32_t slots_active, limit, slots_unlocked, ui_caps;
static uint32_t cs_magic_seen, cs_drive_seen;
static uint32_t ui_lang = 1u;
static uint64_t last_poll, last_lang_ms, wait_start_ms, last_probe_ms;
static uint64_t boot_start_ms, last_summary_ms, sel_dirty_ms;
static uint64_t cfg_wait_start, cfg_last_try;
static uint8_t record[SC_RECORD];
static char text[CONTROLS + 1u][FX_UI_TEXT];
_Static_assert(sizeof(text[0]) == FX_UI_TEXT, "row text buffer matches FxUiControl.label");

/* Drive selection, dual-slot selection persistence & strict missing state */
static uint32_t active_drive, next_drive, missing_reason;
static uint32_t cfg_slot, cfg_seq, cfg_saves, cfg_save_err, cfg_save_pending;
static uint32_t cfg_load_idx, cfg_io_acc, cfg_io_calls, cfg_io_err;
static uint32_t sel_slot, sel_seq, sel_saves, sel_has_valid, sel_dirty, sel_io_err;
static uint32_t sel_load_idx, need_slot_scan, commit_kind, sel_save_now;
static int32_t cfg_fd, native_fd, probe_fd;
static uint32_t probe_status, probe_ok, probe_done_once, ready_to_load;
static uint32_t fen_probe_status, mounted_no_hits;
static uint32_t io_done;
static uint32_t call_retries, transient_errors, scan_failed, slot_read_ok;
static uint32_t new_slot_crc, boot_retries;
static SlotCardDriveCfg cfg_slots[2], cfg_active, cfg_save_buf;
static uint8_t cfg_valid[2];
static SlotCardSelCfg sel_slots[2], sel_active, sel_save_buf;
static uint8_t sel_valid[2];
static char io_path[64];
static uint8_t io_xfer[SC_WIRE];

#define CALL(e) CALL_(e, __COUNTER__)
#define CALL_(e, n)                                                              \
    do {                                                                         \
        pc = 2u * (n) + 1u;                                                      \
    case 2u * (n) + 1u: {                                                        \
        FxResult t_ = (e);                                                       \
        if (t_ == FX_BUSY) return 0;                                             \
        if (t_ != FX_OK) {                                                       \
            ++transient_errors;                                                  \
            if (call_retries < 3u) {                                             \
                ++call_retries;                                                  \
                return 0;                                                        \
            }                                                                    \
            call_retries = 0u;                                                   \
            reply = (FxServiceReply){ .result = t_ };                            \
            pc = 2u * (n) + 2u;                                                  \
            return 0;                                                            \
        }                                                                        \
        call_retries = 0u;                                                       \
    }                                                                            \
        pc = 2u * (n) + 2u;                                                      \
        return 0;                                                                \
    case 2u * (n) + 2u:                                                          \
        if (reply.result == FX_BUSY) {                                           \
            pc = 2u * (n) + 1u;                                                  \
            return 0;                                                            \
        }                                                                        \
    } while (0)

/* Yield 1 tick with no new IPC request (shares __COUNTER__ space with CALL). */
#define YIELD_TICK() YIELD_TICK_(__COUNTER__)
#define YIELD_TICK_(n)                                                           \
    do {                                                                         \
        pc = 2u * (n) + 1u;                                                      \
        return 0;                                                                \
    case 2u * (n) + 1u:;                                                         \
    } while (0)

static int active_slot_idx(uint32_t index) {
    uint32_t slot = index % 64u;
    return slot >= 9u && slot < limit;
}

static uint32_t t_add_cap(char *o, uint32_t cap, uint32_t at, const char *s) {
    uint32_t max_b = cap ? (cap - 1u) : 0u;
    while (*s && at < max_b) {
        uint8_t ch = (uint8_t)*s;
        uint32_t clen = 1u;
        if ((ch & 0xe0u) == 0xc0u) {
            clen = 2u;
        } else if ((ch & 0xf0u) == 0xe0u) {
            clen = 3u;
        } else if ((ch & 0xf8u) == 0xf0u) {
            clen = 4u;
        }
        if (at + clen > max_b) {
            break;
        }
        for (uint32_t k = 0u; k < clen && *s; ++k) {
            o[at++] = *s++;
        }
    }
    if (cap) {
        o[at] = 0;
    }
    return at;
}

static uint32_t t_add(char *o, uint32_t at, const char *s) {
    return t_add_cap(o, FX_UI_TEXT, at, s);
}

static uint32_t t_num(char *o, uint32_t at, uint32_t v) {
    char d[12];
    uint32_t n = 0u;
    do {
        d[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n && at < SC_UI_MAX) {
        o[at++] = d[--n];
    }
    o[at] = 0;
    return at;
}

static const char *sc_tr(const char *en, const char *sc, const char *tc) {
    if (ui_lang == LANG_ZH_CN) return sc;
    if (ui_lang == LANG_ZH_TW) return tc;
    return en;
}

static const char *page_title(void) {
    if (ui_lang == LANG_ZH_CN) {
        return slots_active == 32u ? "C32 \xe6\xbb\xa4\xe9\x95\x9c\xe6\xa7\xbd"  /* C32 滤镜槽 */
                                   : "C64 \xe6\xbb\xa4\xe9\x95\x9c\xe6\xa7\xbd"; /* C64 滤镜槽 */
    }
    if (ui_lang == LANG_ZH_TW) {
        return slots_active == 32u ? "C32 \xe6\xbf\xbe\xe9\x8f\xa1\xe6\xa7\xbd"  /* C32 濾鏡槽 */
                                   : "C64 \xe6\xbf\xbe\xe9\x8f\xa1\xe6\xa7\xbd"; /* C64 濾鏡槽 */
    }
    return slots_active == 32u ? "C32 Filter Slots" : "C64 Filter Slots";
}

/* Pure switch helpers (no static const char* arrays) to keep 0 ELF relocations. */
static const char *state_label_sc(uint32_t st) {
    switch (st) {
    case ST_LOADING:   return "\xe8\xaf\xbb\xe5\x8f\x96\xe4\xb8\xad"; /* 读取中 */
    case ST_INJECTING: return "\xe5\x86\x99\xe5\x85\xa5\xe4\xb8\xad"; /* 写入中 */
    case ST_APPLYING:  return "\xe5\xba\x94\xe7\x94\xa8\xe4\xb8\xad"; /* 应用中 */
    case ST_RUNNING:   return "\xe8\xbf\x90\xe8\xa1\x8c\xe4\xb8\xad"; /* 运行中 */
    default:           return "\xe5\x90\xaf\xe5\x8a\xa8\xe4\xb8\xad"; /* 启动中 */
    }
}

static const char *state_label_tc(uint32_t st) {
    switch (st) {
    case ST_LOADING:   return "\xe8\xae\x80\xe5\x8f\x96\xe4\xb8\xad"; /* 讀取中 */
    case ST_INJECTING: return "\xe5\xaf\xab\xe5\x85\xa5\xe4\xb8\xad"; /* 寫入中 */
    case ST_APPLYING:  return "\xe5\xa5\x97\xe7\x94\xa8\xe4\xb8\xad"; /* 套用中 */
    case ST_RUNNING:   return "\xe9\x81\x8b\xe8\xa1\x8c\xe4\xb8\xad"; /* 運行中 */
    default:           return "\xe5\x95\x9f\xe5\x8b\x95\xe4\xb8\xad"; /* 啟動中 */
    }
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void build_ctrl_desc(uint32_t id, FxUiControl *out) {
    uint8_t *raw = (uint8_t *)out;
    for (uint32_t k = 0u; k < (uint32_t)sizeof(*out); ++k) {
        raw[k] = 0u;
    }
    out->abi = FX_PLATFORM_ABI;
    out->bytes = (uint32_t)sizeof(*out);
    out->id = id;
    out->flags = FX_UI_VISIBLE | FX_UI_ENABLED;
    _Static_assert(sizeof(out->label) == FX_UI_TEXT, "FxUiControl.label size");

    switch (id) {
    case PAGE:
        out->parent = 0u;
        out->kind = FX_UI_PAGE;
        if (ui_caps & FX_UI_CAP_ROOT) {
            out->flags |= FX_UI_ROOT;
        }
        t_add(out->label, 0u, page_title());
        break;

    case R_LIMIT: {
        uint32_t lim = store.next_limit ? store.next_limit : limit;
        out->parent = PAGE;
        out->kind = FX_UI_CHOICE;
        t_add(out->label, 0u,
              sc_tr("Slot Limit",
                    "\xe6\xbb\xa4\xe9\x95\x9c\xe6\xa7\xbd\xe5\xae\xb9\xe9\x87\x8f",   /* 滤镜槽容量 */
                    "\xe6\xbf\xbe\xe9\x8f\xa1\xe6\xa7\xbd\xe5\xae\xb9\xe9\x87\x8f")); /* 濾鏡槽容量 */
        if (!slots_unlocked) {
            out->flags = FX_UI_VISIBLE;
            out->option_count = 1u;
            out->value = 0u;
            t_add_cap(out->options[0], 32u, 0u,
                      state == ST_CARD_MISSING
                          ? sc_tr("C1-C9 (Missing)",
                                  "\xe4\xbb\x85 C1-C9 (\xe7\xbc\xba\xe5\x8d\xa1)",   /* 仅 C1-C9 (缺卡) */
                                  "\xe5\x83\x85 C1-C9 (\xe7\xbc\xba\xe5\x8d\xa1)")   /* 僅 C1-C9 (缺卡) */
                          : sc_tr("C1-C9 only",
                                  "\xe4\xbb\x85 C1-C9",   /* 仅 C1-C9 */
                                  "\xe5\x83\x85 C1-C9")); /* 僅 C1-C9 */
        } else if (slots_active == 32u) {
            out->flags = FX_UI_VISIBLE;
            out->option_count = 1u;
            out->value = 0u;
            t_add_cap(out->options[0], 32u, 0u,
                      sc_tr("C32 (32)", "C32 (32 \xe6\xa7\xbd)", "C32 (32 \xe6\xa7\xbd)"));
        } else {
            out->option_count = 2u;
            out->value = (lim == 32u) ? 1u : 0u;
            uint32_t a0 = t_add_cap(out->options[0], 32u, 0u,
                                    sc_tr("C64 (64)", "C64 (64 \xe6\xa7\xbd)", "C64 (64 \xe6\xa7\xbd)"));
            uint32_t a1 = t_add_cap(out->options[1], 32u, 0u,
                                    sc_tr("C32 (32)", "C32 (32 \xe6\xa7\xbd)", "C32 (32 \xe6\xa7\xbd)"));
            if (store.next_limit && store.next_limit != limit) {
                const char *sfx = sc_tr(" (next boot)",
                                        " (\xe9\x87\x8d\xe5\x90\xaf\xe7\x94\x9f\xe6\x95\x88)",   /* (重启生效) */
                                        " (\xe9\x87\x8d\xe5\x95\x9f\xe7\x94\x9f\xe6\x95\x88)"); /* (重啟生效) */
                if (out->value == 0u) {
                    t_add_cap(out->options[0], 32u, a0, sfx);
                } else {
                    t_add_cap(out->options[1], 32u, a1, sfx);
                }
            }
        }
        break;
    }

    case R_DRIVE: {
        uint32_t sel = next_drive <= SC_DRIVE_B ? next_drive : SC_DRIVE_AUTO;
        out->parent = PAGE;
        out->kind = FX_UI_CHOICE;
        out->option_count = 3u;
        out->value = sel;
        t_add(out->label, 0u,
              sc_tr("Storage Card",
                    "\xe5\xad\x98\xe5\x82\xa8\xe5\x8d\xa1",   /* 存储卡 */
                    "\xe8\xa8\x98\xe6\x86\xb6\xe5\x8d\xa1")); /* 記憶卡 */
        uint32_t a0 = t_add_cap(out->options[0], 32u, 0u,
                                sc_tr("Auto", "\xe8\x87\xaa\xe5\x8a\xa8", "\xe8\x87\xaa\xe5\x8b\x95"));
        uint32_t a1 = t_add_cap(out->options[1], 32u, 0u,
                                sc_tr("CFexpress (A)", "CFx (A)", "CFx (A)"));
        uint32_t a2 = t_add_cap(out->options[2], 32u, 0u, "SD (B)");
        uint32_t cur_at = (sel == 0u) ? a0 : ((sel == 1u) ? a1 : a2);
        if (next_drive != active_drive) {
            cur_at = t_add_cap(out->options[sel], 32u, cur_at,
                               sc_tr(" (next boot)",
                                     " (\xe9\x87\x8d\xe5\x90\xaf\xe7\x94\x9f\xe6\x95\x88)",   /* (重启生效) */
                                     " (\xe9\x87\x8d\xe5\x95\x9f\xe7\x94\x9f\xe6\x95\x88)")); /* (重啟生效) */
        }
        if (cfg_save_err) {
            t_add_cap(out->options[sel], 32u, cur_at,
                      sc_tr(" !err",
                            " !\xe5\xa4\xb1\xe8\xb4\xa5",   /* !失败 */
                            " !\xe5\xa4\xb1\xe6\x95\x97")); /* !失敗 */
        }
        break;
    }

    case R_DEBUG:
        out->parent = PAGE;
        out->kind = FX_UI_PAGE;
        t_add(out->label, 0u,
              sc_tr("Debug Settings",
                    "\xe8\xb0\x83\xe8\xaf\x95\xe8\xae\xbe\xe7\xbd\xae",   /* 调试设置 */
                    "\xe8\xaa\xbf\xe8\xa9\xa6\xe8\xa8\xad\xe5\xae\x9a")); /* 調試設定 */
        break;

    case R_STATE:
        out->parent = R_DEBUG;
        out->kind = FX_UI_ACTION;
        t_add(out->label, 0u,
              sc_tr("Slot State",
                    "\xe6\x8f\x92\xe6\xa7\xbd\xe7\x8a\xb6\xe6\x80\x81",   /* 插槽状态 */
                    "\xe6\x8f\x92\xe6\xa7\xbd\xe7\x8b\x80\xe6\x85\x8b")); /* 插槽狀態 */
        t_add(out->text, 0u, text[R_STATE]);
        break;

    case R_STORE:
        out->parent = R_DEBUG;
        out->kind = FX_UI_ACTION;
        t_add(out->label, 0u,
              sc_tr("Store Journal",
                    "\xe5\xad\x98\xe5\x82\xa8\xe6\x97\xa5\xe5\xbf\x97",   /* 存储日志 */
                    "\xe5\xad\x98\xe5\x84\xb2\xe8\xa8\x98\xe9\x8c\x84")); /* 存儲記錄 */
        t_add(out->text, 0u, text[R_STORE]);
        break;

    case R_CSL1:
        out->parent = R_DEBUG;
        out->kind = FX_UI_ACTION;
        t_add(out->label, 0u,
              sc_tr("Boot Card Latch",
                    "\xe5\x8d\xa1\xe6\xa7\xbd\xe5\xbc\x95\xe5\xaf\xbc",   /* 卡槽引导 */
                    "\xe5\x8d\xa1\xe6\xa7\xbd\xe5\xbc\x95\xe5\xb0\x8e")); /* 卡槽引導 */
        t_add(out->text, 0u, text[R_CSL1]);
        break;

    default:
        break;
    }
}

static FxResult put_ctrl(const FxServiceModule *api, uint32_t id) {
    FxUiControl c;
    build_ctrl_desc(id, &c);
    return fx_sdk_ui(&client, api, &c);
}

static FxResult slot_call(const FxServiceModule *api, uint32_t op, uint32_t index,
                          uint32_t at, uint32_t bytes, uint32_t value, const void *data) {
    struct { SlotsRequest r; uint8_t data[CHUNK]; } q;
    q.r = (SlotsRequest){ index, at, bytes, value };
    uint32_t size = (uint32_t)sizeof(SlotsRequest);
    if (data) {
        const uint8_t *p = (const uint8_t *)data;
        for (uint32_t k = 0u; k < bytes; ++k) {
            q.data[k] = p[k];
        }
        size += bytes;
    }
    return fx_sdk_platform(&client, api, FX_BIND_CALL, slots, op, 0u, &q, size);
}

static FxResult file_call(const FxServiceModule *api, uint32_t op, uint64_t h,
                          uint32_t bytes, uint32_t mode, const char *name,
                          const uint8_t *payload) {
    struct { FxFileRequest r; uint8_t data[SC_WIRE]; } q;
    uint8_t *raw = (uint8_t *)&q;
    for (uint32_t k = 0u; k < (uint32_t)sizeof(FxFileRequest); ++k) {
        raw[k] = 0u;
    }
    q.r.handle = h;
    q.r.bytes = bytes;
    q.r.mode = mode;
    for (uint32_t k = 0u; name && name[k] && k < 63u; ++k) {
        q.r.name[k] = name[k];
    }
    uint32_t size = (uint32_t)sizeof(FxFileRequest);
    if (payload) {
        for (uint32_t k = 0u; k < bytes; ++k) {
            q.data[k] = payload[k];
        }
        size += bytes;
    }
    return fx_sdk_platform(&client, api, FX_BIND_CALL, files, op, 0u, &q, size);
}

static FxResult mem_write_call(const FxServiceModule *api, uint32_t off,
                               const void *src, uint32_t bytes) {
    struct { FxMemoryRequest r; uint8_t data[SC_WIRE]; } q;
    q.r = (FxMemoryRequest){ buf_h, bytes, off };
    const uint8_t *p = (const uint8_t *)src;
    for (uint32_t k = 0u; k < bytes; ++k) {
        q.data[k] = p[k];
    }
    return fx_sdk_platform(&client, api, FX_BIND_CALL, mem_h, FX_MEMORY_WRITE,
                           0u, &q, (uint32_t)sizeof(FxMemoryRequest) + bytes);
}

static FxResult mem_read_call(const FxServiceModule *api, uint32_t off, uint32_t bytes) {
    FxMemoryRequest r = { buf_h, bytes, off };
    return fx_sdk_platform(&client, api, FX_BIND_CALL, mem_h, FX_MEMORY_READ,
                           0u, &r, (uint32_t)sizeof(r));
}

static FxResult native_open_call(const FxServiceModule *api, uint32_t flags) {
    uint64_t args[8] = { buf_native + BOUNCE_PATH_OFF, flags, 0u, 0u, 0u, 0u, 0u, 0u };
    return fx_sdk_platform(&client, api, FX_BIND_CALL, card_open_h, 0u, 0u,
                           args, (uint32_t)sizeof(args));
}

static FxResult native_read_call(const FxServiceModule *api, int32_t fd,
                                 uint32_t data_off, uint32_t bytes) {
    uint64_t args[8] = {
        (uint64_t)(uint32_t)fd,
        buf_native + data_off,
        bytes,
        buf_native + BOUNCE_OUT_OFF,
        0u, 0u, 0u, 0u
    };
    return fx_sdk_platform(&client, api, FX_BIND_CALL, card_read_h, 0u, 0u,
                           args, (uint32_t)sizeof(args));
}

static FxResult native_write_call(const FxServiceModule *api, int32_t fd,
                                  uint32_t data_off, uint32_t bytes) {
    uint64_t args[8] = {
        (uint64_t)(uint32_t)fd,
        buf_native + data_off,
        bytes,
        buf_native + BOUNCE_OUT_OFF,
        0u, 0u, 0u, 0u
    };
    return fx_sdk_platform(&client, api, FX_BIND_CALL, card_write_h, 0u, 0u,
                           args, (uint32_t)sizeof(args));
}

static FxResult native_close_call(const FxServiceModule *api, int32_t fd) {
    uint64_t args[8] = { (uint64_t)(uint32_t)fd, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    return fx_sdk_platform(&client, api, FX_BIND_CALL, card_close_h, 0u, 0u,
                           args, (uint32_t)sizeof(args));
}

static FxResult native_opendir_call(const FxServiceModule *api) {
    uint64_t args[8] = {
        buf_native + BOUNCE_PATH_OFF,
        buf_native + BOUNCE_OUT_OFF,
        0u, 0u, 0u, 0u, 0u, 0u
    };
    return fx_sdk_platform(&client, api, FX_BIND_CALL, dir_open_h, 0u, 0u,
                           args, (uint32_t)sizeof(args));
}

static FxResult native_closedir_call(const FxServiceModule *api, int32_t fd) {
    uint64_t args[8] = { (uint64_t)(uint32_t)fd, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    return fx_sdk_platform(&client, api, FX_BIND_CALL, dir_close_h, 0u, 0u,
                           args, (uint32_t)sizeof(args));
}

static FxResult machine_read_call(const FxServiceModule *api, uint64_t addr, uint32_t bytes) {
    FxMachineRequest mr = { addr, 0u, bytes, 0u };
    return fx_sdk_platform(&client, api, FX_BIND_CALL, machine_h,
                           FX_MACHINE_READ, 0u, &mr, (uint32_t)sizeof(mr));
}

static FxResult submit_card_files_io(const FxServiceModule *api) {
    char name[16];
    switch (io.op) {
    case SC_IO_OPEN_READ:
        sc_name(io.gen, name);
        return file_call(api, FX_FILE_OPEN, 0u, 0u, FX_FILE_READ_ONLY, name, 0);
    case SC_IO_CREATE:
        sc_name(io.gen, name);
        return file_call(api, FX_FILE_OPEN, 0u, 0u, FX_FILE_CREATE_EXCLUSIVE, name, 0);
    case SC_IO_READ:
        return file_call(api, FX_FILE_READ, handle, io.bytes, 0u, 0, 0);
    case SC_IO_WRITE:
        return file_call(api, FX_FILE_WRITE, handle, io.bytes, 0u, 0, io.write);
    case SC_IO_CLOSE:
        return file_call(api, FX_FILE_CLOSE, handle, 0u, 0u, 0, 0);
    default:
        return FX_ARGUMENT;
    }
}

static void map_card_files_reply(void) {
    answer = (ScReply){ SC_R_FAILED, 0u, 0u };
    if (reply.result == FX_EMPTY) {
        answer.result = SC_R_MISSING;
    } else if (reply.result == FX_OK) {
        answer.result = SC_R_OK;
        if (io.op == SC_IO_OPEN_READ || io.op == SC_IO_CREATE) {
            handle = reply.value[0];
        }
        if (io.op == SC_IO_WRITE) {
            answer.done = (uint32_t)reply.value[0];
        }
        if (io.op == SC_IO_READ) {
            answer.done = reply.bytes;
            if (reply.bytes <= io.bytes) {
                for (uint32_t k = 0u; k < reply.bytes; ++k) {
                    io.read[k] = reply.data[k];
                }
            }
        }
    }
}

static FxResult publish_status(const FxServiceModule *api) {
    uint32_t v[22] = {
        UINT32_C(0x53434431), state, load_status, store.head, used_count,
        store.bad_slots, store.files_skipped, injected, inject_failed,
        applied, apply_failed, commits, commit_status, selected[0], selected[1],
        checks_other, limit, store.next_limit,
        active_drive, next_drive, missing_reason, cfg_saves
    };
    return slot_call(api, SLOTS_STATUS_SET, 0u, 0u, (uint32_t)sizeof(v), 0u, v);
}

static uint32_t t_drive_name(char *o, uint32_t at, uint32_t mode) {
    if (mode == SC_DRIVE_A) {
        if (ui_lang == LANG_ZH_CN || ui_lang == LANG_ZH_TW) {
            return t_add(o, at, "CFx (A)");
        }
        return t_add(o, at, "CFexpress (A)");
    }
    if (mode == SC_DRIVE_B) {
        return t_add(o, at, "SD (B)");
    }
    if (ui_lang == LANG_ZH_CN) {
        return t_add(o, at, "\xe8\x87\xaa\xe5\x8a\xa8"); /* 自动 */
    }
    if (ui_lang == LANG_ZH_TW) {
        return t_add(o, at, "\xe8\x87\xaa\xe5\x8b\x95"); /* 自動 */
    }
    return t_add(o, at, "Auto");
}

static void set_csl1_text(void) {
    uint32_t a = t_add(text[R_CSL1], 0u, "CSL1:");
    if (cs_magic_seen == SLTCARD_CS_MAGIC) {
        if (cs_drive_seen == 1u || (cs_drive_seen & 0xffu) == 'A') {
            a = t_add(text[R_CSL1], a, "A");
        } else if (cs_drive_seen == 2u || (cs_drive_seen & 0xffu) == 'B') {
            a = t_add(text[R_CSL1], a, "B");
        } else {
            a = t_add(text[R_CSL1], a, "0");
        }
    } else {
        a = t_add(text[R_CSL1], a, "-");
    }
    a = t_add(text[R_CSL1], a, " Miss:");
    a = t_num(text[R_CSL1], a, missing_reason);
    a = t_add(text[R_CSL1], a, " SEQ:");
    t_num(text[R_CSL1], a, sel_seq);
}

static void set_text(void) {
    uint32_t a;
    uint32_t lim = store.next_limit ? store.next_limit : limit;
    set_csl1_text();

    if (ui_lang == LANG_ZH_CN) {
        /* Row 1: R_STATE (ZH-CN) */
        if (state == ST_WAITING_CARD) {
            t_add(text[R_STATE], 0u,
                  active_drive == SC_DRIVE_B
                      ? "\xe5\x8d\xa1" "B \xe7\xad\x89\xe5\xbe\x85\xe5\xb0\xb1\xe7\xbb\xaa..." /* 卡B 等待就绪... */
                      : "\xe5\x8d\xa1" "A \xe7\xad\x89\xe5\xbe\x85\xe5\xb0\xb1\xe7\xbb\xaa..."); /* 卡A 等待就绪... */
        } else if (state == ST_CARD_MISSING) {
            if (missing_reason == SC_MISS_NO_FENESTRA) {
                t_add(text[R_STATE], 0u,
                      active_drive == SC_DRIVE_B
                          ? "\xe5\x8d\xa1" "B \xe7\xbc\xba\xe5\xb0\x91 FENESTRA \xe7\x9b\xae\xe5\xbd\x95" /* 卡B 缺少 FENESTRA 目录 */
                          : "\xe5\x8d\xa1" "A \xe7\xbc\xba\xe5\xb0\x91 FENESTRA \xe7\x9b\xae\xe5\xbd\x95"); /* 卡A 缺少 FENESTRA 目录 */
            } else if (missing_reason == SC_MISS_BOOTED_SD) {
                t_add(text[R_STATE], 0u,
                      "\xe5\x8d\xa1" "A \xe4\xb8\x8d\xe5\x8f\xaf\xe7\x94\xa8 (\xe5\xbd\x93\xe5\x89\x8d\xe4\xbb\x8eSD\xe5\x90\xaf\xe5\x8a\xa8)"); /* 卡A 不可用 (当前从SD启动) */
            } else if (missing_reason == SC_MISS_NO_NATIVE_IO) {
                t_add(text[R_STATE], 0u,
                      active_drive == SC_DRIVE_B
                          ? "\xe5\x8d\xa1" "B \xe6\x9c\xaa\xe6\xa3\x80\xe6\xb5\x8b\xe5\x88\xb0 (\xe6\x97\xa0IO)" /* 卡B 未检测到 (无IO) */
                          : "\xe5\x8d\xa1" "A \xe6\x9c\xaa\xe6\xa3\x80\xe6\xb5\x8b\xe5\x88\xb0 (\xe6\x97\xa0IO)"); /* 卡A 未检测到 (无IO) */
            } else {
                t_add(text[R_STATE], 0u,
                      active_drive == SC_DRIVE_B
                          ? "\xe5\x8d\xa1" "B \xe6\x9c\xaa\xe6\xa3\x80\xe6\xb5\x8b\xe5\x88\xb0" /* 卡B 未检测到 */
                          : "\xe5\x8d\xa1" "A \xe6\x9c\xaa\xe6\xa3\x80\xe6\xb5\x8b\xe5\x88\xb0"); /* 卡A 未检测到 */
            }
        } else if (state == ST_OTHER_CARD) {
            t_add(text[R_STATE], 0u, "\xe5\xb7\xb2\xe6\x8d\xa2\xe5\x8d\xa1 (\xe8\xaf\xb7\xe9\x87\x8d\xe5\x90\xaf)"); /* 已换卡 (请重启) */
        } else {
            a = t_add(text[R_STATE], 0u, "\xe7\x8a\xb6\xe6\x80\x81 "); /* 状态  */
            a = t_add(text[R_STATE], a, state_label_sc(state));
            a = t_add(text[R_STATE], a, " \xe5\xb7\xb2\xe7\x94\xa8 "); /*  已用  */
            a = t_num(text[R_STATE], a, used_count);
            a = t_add(text[R_STATE], a, " \xe5\xbc\x82\xe5\xb8\xb8 "); /*  异常  */
            a = t_num(text[R_STATE], a, store.bad_slots);
            if (active_drive == SC_DRIVE_A) {
                t_add(text[R_STATE], a, " [A]");
            } else if (active_drive == SC_DRIVE_B) {
                t_add(text[R_STATE], a, " [B]");
            }
        }

        /* Row 2: R_STORE (ZH-CN) */
        if (state == ST_CARD_MISSING) {
            t_add(text[R_STORE], 0u, "C10-C64 \xe5\xb7\xb2\xe5\x81\x9c\xe7\x94\xa8 (\xe4\xbb\x85 C1-C9)"); /* C10-C64 已停用 (仅 C1-C9) */
        } else if (state == ST_WAITING_CARD) {
            t_add(text[R_STORE], 0u, "\xe6\xad\xa3\xe5\x9c\xa8\xe7\xad\x89\xe5\xbe\x85\xe7\x9b\xae\xe6\xa0\x87\xe5\xad\x98\xe5\x82\xa8\xe5\x8d\xa1..."); /* 正在等待目标存储卡... */
        } else {
            a = t_add(text[R_STORE], 0u, "\xe5\xad\x98\xe6\xa1\xa3\xe4\xbb\xa3 "); /* 存档代  */
            a = t_num(text[R_STORE], a, store.head);
            a = t_add(text[R_STORE], a, " \xe6\x8f\x90\xe4\xba\xa4 "); /*  提交  */
            t_num(text[R_STORE], a, commits);
        }

        /* Row 3: R_LIMIT (ZH-CN) */
        if (!slots_unlocked) {
            t_add(text[R_LIMIT], 0u, "\xe6\xa7\xbd\xe4\xbd\x8d\xe4\xb8\x8a\xe9\x99\x90: \xe4\xbb\x85 C1-C9"); /* 槽位上限: 仅 C1-C9 */
        } else {
            a = t_add(text[R_LIMIT], 0u, "\xe6\xa7\xbd\xe4\xbd\x8d\xe4\xb8\x8a\xe9\x99\x90 C"); /* 槽位上限 C */
            a = t_num(text[R_LIMIT], a, lim);
            if (store.next_limit && store.next_limit != limit) {
                t_add(text[R_LIMIT], a, " (\xe9\x87\x8d\xe5\x90\xaf\xe7\x94\x9f\xe6\x95\x88)"); /*  (重启生效) */
            }
        }

        /* Row 4: R_DRIVE (ZH-CN) */
        a = t_add(text[R_DRIVE], 0u, "\xe5\xad\x98\xe5\x82\xa8\xe5\x8d\xa1: "); /* 存储卡:  */
        a = t_drive_name(text[R_DRIVE], a, next_drive);
        if (next_drive != active_drive) {
            a = t_add(text[R_DRIVE], a, " (\xe9\x87\x8d\xe5\x90\xaf\xe7\x94\x9f\xe6\x95\x88)"); /*  (重启生效) */
        }
        if (cfg_save_err) {
            t_add(text[R_DRIVE], a, " !\xe5\xa4\xb1\xe8\xb4\xa5"); /*  !失败 */
        }
        return;
    }

    if (ui_lang == LANG_ZH_TW) {
        /* Row 1: R_STATE (ZH-TW) */
        if (state == ST_WAITING_CARD) {
            t_add(text[R_STATE], 0u,
                  active_drive == SC_DRIVE_B
                      ? "\xe5\x8d\xa1" "B \xe7\xad\x89\xe5\xbe\x85\xe5\xb0\xb1\xe7\xb7\x92..." /* 卡B 等待就緒... */
                      : "\xe5\x8d\xa1" "A \xe7\xad\x89\xe5\xbe\x85\xe5\xb0\xb1\xe7\xb7\x92..."); /* 卡A 等待就緒... */
        } else if (state == ST_CARD_MISSING) {
            if (missing_reason == SC_MISS_NO_FENESTRA) {
                t_add(text[R_STATE], 0u,
                      active_drive == SC_DRIVE_B
                          ? "\xe5\x8d\xa1" "B \xe7\xbc\xba\xe5\xb0\x91 FENESTRA \xe7\x9b\xae\xe9\x8c\x84" /* 卡B 缺少 FENESTRA 目錄 */
                          : "\xe5\x8d\xa1" "A \xe7\xbc\xba\xe5\xb0\x91 FENESTRA \xe7\x9b\xae\xe9\x8c\x84"); /* 卡A 缺少 FENESTRA 目錄 */
            } else if (missing_reason == SC_MISS_BOOTED_SD) {
                t_add(text[R_STATE], 0u,
                      "\xe5\x8d\xa1" "A \xe4\xb8\x8d\xe5\x8f\xaf\xe7\x94\xa8 (\xe7\x9b\xae\xe5\x89\x8d\xe5\xbe\x9eSD\xe5\x95\x9f\xe5\x8b\x95)"); /* 卡A 不可用 (目前從SD啟動) */
            } else if (missing_reason == SC_MISS_NO_NATIVE_IO) {
                t_add(text[R_STATE], 0u,
                      active_drive == SC_DRIVE_B
                          ? "\xe5\x8d\xa1" "B \xe6\x9c\xaa\xe5\x81\xb5\xe6\xb8\xac\xe5\x88\xb0 (\xe7\x84\xa1IO)" /* 卡B 未偵測到 (無IO) */
                          : "\xe5\x8d\xa1" "A \xe6\x9c\xaa\xe5\x81\xb5\xe6\xb8\xac\xe5\x88\xb0 (\xe7\x84\xa1IO)"); /* 卡A 未偵測到 (無IO) */
            } else {
                t_add(text[R_STATE], 0u,
                      active_drive == SC_DRIVE_B
                          ? "\xe5\x8d\xa1" "B \xe6\x9c\xaa\xe5\x81\xb5\xe6\xb8\xac\xe5\x88\xb0" /* 卡B 未偵測到 */
                          : "\xe5\x8d\xa1" "A \xe6\x9c\xaa\xe5\x81\xb5\xe6\xb8\xac\xe5\x88\xb0"); /* 卡A 未偵測到 */
            }
        } else if (state == ST_OTHER_CARD) {
            t_add(text[R_STATE], 0u, "\xe5\xb7\xb2\xe6\x8f\x9b\xe5\x8d\xa1 (\xe8\xab\x8b\xe9\x87\x8d\xe5\x95\x9f)"); /* 已換卡 (請重啟) */
        } else {
            a = t_add(text[R_STATE], 0u, "\xe7\x8b\x80\xe6\x85\x8b "); /* 狀態  */
            a = t_add(text[R_STATE], a, state_label_tc(state));
            a = t_add(text[R_STATE], a, " \xe5\xb7\xb2\xe7\x94\xa8 "); /*  已用  */
            a = t_num(text[R_STATE], a, used_count);
            a = t_add(text[R_STATE], a, " \xe7\x95\xb0\xe5\xb8\xb8 "); /*  異常  */
            a = t_num(text[R_STATE], a, store.bad_slots);
            if (active_drive == SC_DRIVE_A) {
                t_add(text[R_STATE], a, " [A]");
            } else if (active_drive == SC_DRIVE_B) {
                t_add(text[R_STATE], a, " [B]");
            }
        }

        /* Row 2: R_STORE (ZH-TW) */
        if (state == ST_CARD_MISSING) {
            t_add(text[R_STORE], 0u, "C10-C64 \xe5\xb7\xb2\xe5\x81\x9c\xe7\x94\xa8 (\xe5\x83\x85 C1-C9)"); /* C10-C64 已停用 (僅 C1-C9) */
        } else if (state == ST_WAITING_CARD) {
            t_add(text[R_STORE], 0u, "\xe6\xad\xa3\xe5\x9c\xa8\xe7\xad\x89\xe5\xbe\x85\xe7\x9b\xae\xe6\xa8\x99\xe8\xa8\x98\xe6\x86\xb6\xe5\x8d\xa1..."); /* 正在等待目標記憶卡... */
        } else {
            a = t_add(text[R_STORE], 0u, "\xe5\xad\x98\xe6\xaa\x94\xe4\xbb\xa3 "); /* 存檔代  */
            a = t_num(text[R_STORE], a, store.head);
            a = t_add(text[R_STORE], a, " \xe6\x8f\x90\xe4\xba\xa4 "); /*  提交  */
            t_num(text[R_STORE], a, commits);
        }

        /* Row 3: R_LIMIT (ZH-TW) */
        if (!slots_unlocked) {
            t_add(text[R_LIMIT], 0u, "\xe6\xa7\xbd\xe4\xbd\x8d\xe4\xb8\x8a\xe9\x99\x90: \xe5\x83\x85 C1-C9"); /* 槽位上限: 僅 C1-C9 */
        } else {
            a = t_add(text[R_LIMIT], 0u, "\xe6\xa7\xbd\xe4\xbd\x8d\xe4\xb8\x8a\xe9\x99\x90 C"); /* 槽位上限 C */
            a = t_num(text[R_LIMIT], a, lim);
            if (store.next_limit && store.next_limit != limit) {
                t_add(text[R_LIMIT], a, " (\xe9\x87\x8d\xe5\x95\x9f\xe7\x94\x9f\xe6\x95\x88)"); /*  (重啟生效) */
            }
        }

        /* Row 4: R_DRIVE (ZH-TW) */
        a = t_add(text[R_DRIVE], 0u, "\xe8\xa8\x98\xe6\x86\xb6\xe5\x8d\xa1: "); /* 記憶卡:  */
        a = t_drive_name(text[R_DRIVE], a, next_drive);
        if (next_drive != active_drive) {
            a = t_add(text[R_DRIVE], a, " (\xe9\x87\x8d\xe5\x95\x9f\xe7\x94\x9f\xe6\x95\x88)"); /*  (重啟生效) */
        }
        if (cfg_save_err) {
            t_add(text[R_DRIVE], a, " !\xe5\xa4\xb1\xe6\x95\x97"); /*  !失敗 */
        }
        return;
    }

    /* English fallback (all other languages or older firmware lacking op 15) */
    /* Row 1: R_STATE */
    if (state == ST_WAITING_CARD) {
        t_add(text[R_STATE], 0u,
              active_drive == SC_DRIVE_B ? "Card B waiting..." : "Card A waiting...");
    } else if (state == ST_CARD_MISSING) {
        a = t_add(text[R_STATE], 0u,
                  active_drive == SC_DRIVE_B ? "Card B missing" : "Card A missing");
        if (missing_reason == SC_MISS_NO_FENESTRA) {
            t_add(text[R_STATE], a, " (no FENESTRA)");
        } else if (missing_reason == SC_MISS_BOOTED_SD) {
            t_add(text[R_STATE], a, " (booted SD)");
        } else if (missing_reason == SC_MISS_NO_NATIVE_IO) {
            t_add(text[R_STATE], a, " (no IO)");
        }
    } else if (state == ST_OTHER_CARD) {
        t_add(text[R_STATE], 0u, "Other card (restart)");
    } else {
        a = t_add(text[R_STATE], 0u, "State ");
        a = t_num(text[R_STATE], a, state);
        a = t_add(text[R_STATE], a, " used ");
        a = t_num(text[R_STATE], a, used_count);
        a = t_add(text[R_STATE], a, " bad ");
        a = t_num(text[R_STATE], a, store.bad_slots);
        if (active_drive == SC_DRIVE_A) {
            t_add(text[R_STATE], a, " [A]");
        } else if (active_drive == SC_DRIVE_B) {
            t_add(text[R_STATE], a, " [B]");
        }
    }

    /* Row 2: R_STORE */
    if (state == ST_CARD_MISSING) {
        t_add(text[R_STORE], 0u, "C10-C64 off (C1-C9 only)");
    } else if (state == ST_WAITING_CARD) {
        t_add(text[R_STORE], 0u, "Waiting for target card...");
    } else {
        a = t_add(text[R_STORE], 0u, "Head ");
        a = t_num(text[R_STORE], a, store.head);
        a = t_add(text[R_STORE], a, " commits ");
        t_num(text[R_STORE], a, commits);
    }

    /* Row 3: R_LIMIT */
    if (!slots_unlocked) {
        t_add(text[R_LIMIT], 0u, "Slot limit: C1-C9 only");
    } else {
        a = t_add(text[R_LIMIT], 0u, "Slot limit C");
        a = t_num(text[R_LIMIT], a, lim);
        if (store.next_limit && store.next_limit != limit) {
            t_add(text[R_LIMIT], a, " (next boot)");
        }
    }

    /* Row 4: R_DRIVE */
    a = t_add(text[R_DRIVE], 0u, "Slot card: ");
    a = t_drive_name(text[R_DRIVE], a, next_drive);
    if (next_drive != active_drive) {
        a = t_add(text[R_DRIVE], a, " (next boot)");
    }
    if (cfg_save_err) {
        t_add(text[R_DRIVE], a, " !err");
    }
}

static int selector_extended(uint32_t v) { return sc_selector_extended(v, 0); }

/* Coroutine macro: probe directory at io_path. Leaves probe_ok (0/1) and probe_status. */
#define PROBE_DIR_NOW()                                                              \
    do {                                                                             \
        probe_ok = 0u;                                                               \
        probe_status = UINT32_C(0xffffffff);                                         \
        probe_fd = -1;                                                               \
        CALL(mem_write_call(api, BOUNCE_PATH_OFF, io_path, 64u));                    \
        if (reply.result == FX_OK) {                                                 \
            write_u32_le(io_xfer, UINT32_C(0xffffffff));                             \
            CALL(mem_write_call(api, BOUNCE_OUT_OFF, io_xfer, 4u));                  \
            if (reply.result == FX_OK) {                                             \
                CALL(native_opendir_call(api));                                      \
                if (reply.result == FX_OK) {                                         \
                    probe_status = (uint32_t)reply.value[0];                         \
                    if (probe_status == 0u) {                                        \
                        CALL(mem_read_call(api, BOUNCE_OUT_OFF, 4u));                \
                        if (reply.result == FX_OK && reply.bytes == 4u) {            \
                            probe_fd = (int32_t)read_u32_le(reply.data);             \
                            if (probe_fd >= 0) {                                     \
                                CALL(native_closedir_call(api, probe_fd));           \
                                probe_fd = -1;                                       \
                                if (reply.result == FX_OK &&                         \
                                    (uint32_t)reply.value[0] == 0u) {                \
                                    probe_ok = 1u;                                   \
                                }                                                    \
                            }                                                        \
                        }                                                            \
                    }                                                                \
                }                                                                    \
            }                                                                        \
        }                                                                            \
    } while (0)

/* Coroutine macro: drive Sc state machine over either card.files (Auto) or
 * explicit native camera.card.* (CFexpress A / SD B). */
#define RUN_MACHINE()                                                                \
    do {                                                                             \
        while (sc_next(&store, &answer, &io) == SC_RUN) {                            \
            if (active_drive == SC_DRIVE_AUTO) {                                     \
                CALL(submit_card_files_io(api));                                     \
                map_card_files_reply();                                              \
            } else if (io.op == SC_IO_OPEN_READ || io.op == SC_IO_CREATE) {          \
                answer = (ScReply){ SC_R_FAILED, 0u, 0u };                           \
                sc_gen_path(active_drive, io.gen, io_path);                          \
                CALL(mem_write_call(api, BOUNCE_PATH_OFF, io_path, 64u));            \
                if (reply.result == FX_OK) {                                         \
                    CALL(native_open_call(                                           \
                        api, io.op == SC_IO_OPEN_READ ? SLTCARD_OPEN_RDONLY          \
                                                      : SLTCARD_OPEN_CREATE_EX));    \
                    if (reply.result == FX_OK) {                                     \
                        native_fd = (int32_t)(uint32_t)reply.value[0];               \
                        if (native_fd >= 0) {                                        \
                            answer.result = SC_R_OK;                                 \
                        } else if ((uint32_t)native_fd == SLTCARD_OPEN_NOT_FOUND) {  \
                            answer.result = SC_R_MISSING;                            \
                        }                                                            \
                    }                                                                \
                }                                                                    \
            } else if (io.op == SC_IO_READ) {                                        \
                answer = (ScReply){ SC_R_FAILED, 0u, 0u };                           \
                if (io.bytes <= SC_WIRE && native_fd >= 0) {                         \
                    write_u32_le(io_xfer, 0u);                                       \
                    CALL(mem_write_call(api, BOUNCE_OUT_OFF, io_xfer, 4u));          \
                    if (reply.result == FX_OK) {                                     \
                        CALL(native_read_call(api, native_fd, BOUNCE_DATA_OFF,       \
                                              io.bytes));                            \
                        if (reply.result == FX_OK &&                                 \
                            (uint32_t)reply.value[0] == 0u) {                        \
                            CALL(mem_read_call(api, BOUNCE_OUT_OFF, 4u));            \
                            if (reply.result == FX_OK && reply.bytes == 4u) {        \
                                io_done = read_u32_le(reply.data);                   \
                                if (io_done == 0u) {                                 \
                                    answer = (ScReply){ SC_R_OK, 0u, 0u };           \
                                } else if (io_done <= io.bytes) {                    \
                                    CALL(mem_read_call(api, BOUNCE_DATA_OFF,         \
                                                       io_done));                    \
                                    if (reply.result == FX_OK &&                     \
                                        reply.bytes == io_done) {                    \
                                        for (uint32_t k = 0u; k < io_done; ++k) {    \
                                            io.read[k] = reply.data[k];              \
                                        }                                            \
                                        answer = (ScReply){ SC_R_OK, io_done, 0u };  \
                                    }                                                \
                                }                                                    \
                            }                                                        \
                        }                                                            \
                    }                                                                \
                }                                                                    \
            } else if (io.op == SC_IO_WRITE) {                                       \
                answer = (ScReply){ SC_R_FAILED, 0u, 0u };                           \
                if (io.bytes <= SC_WIRE && native_fd >= 0) {                         \
                    for (uint32_t k = 0u; k < io.bytes; ++k) {                       \
                        io_xfer[k] = io.write[k];                                    \
                    }                                                                \
                    CALL(mem_write_call(api, BOUNCE_DATA_OFF, io_xfer, io.bytes));   \
                    if (reply.result == FX_OK) {                                     \
                        write_u32_le(io_xfer, 0u);                                   \
                        CALL(mem_write_call(api, BOUNCE_OUT_OFF, io_xfer, 4u));      \
                        if (reply.result == FX_OK) {                                 \
                            CALL(native_write_call(api, native_fd, BOUNCE_DATA_OFF,  \
                                                   io.bytes));                       \
                            if (reply.result == FX_OK &&                             \
                                (uint32_t)reply.value[0] == 0u) {                    \
                                CALL(mem_read_call(api, BOUNCE_OUT_OFF, 4u));        \
                                if (reply.result == FX_OK && reply.bytes == 4u) {    \
                                    io_done = read_u32_le(reply.data);               \
                                    if (io_done <= io.bytes) {                       \
                                        answer = (ScReply){ SC_R_OK, io_done, 0u };  \
                                    }                                                \
                                }                                                    \
                            }                                                        \
                        }                                                            \
                    }                                                                \
                }                                                                    \
            } else if (io.op == SC_IO_CLOSE) {                                       \
                answer = (ScReply){ SC_R_FAILED, 0u, 0u };                           \
                if (native_fd >= 0) {                                                \
                    CALL(native_close_call(api, native_fd));                         \
                    native_fd = -1;                                                  \
                    if (reply.result == FX_OK && (uint32_t)reply.value[0] == 0u) {   \
                        answer.result = SC_R_OK;                                     \
                    }                                                                \
                }                                                                    \
            } else {                                                                 \
                answer = (ScReply){ SC_R_FAILED, 0u, 0u };                           \
            }                                                                        \
        }                                                                            \
    } while (0)

/* Coroutine macro: atomically overwrite alternate 64 B SCSEL0/1.CFG on active_drive
 * (Auto -> A:\FENESTRA\ routed by cardslot.c, Strict A -> A:, Strict B -> B:).
 * Leaves sel_io_err == 0 on success. */
#define SAVE_SEL_CFG()                                                               \
    do {                                                                             \
        sel_io_err = 0u;                                                             \
        if (!has_native_io) {                                                        \
            sel_io_err = 1u;                                                         \
        } else {                                                                     \
            sc_sel_finalize(&sel_save_buf, sel_seq + 1u, store.head,                 \
                            store.store_id,                                          \
                            store.next_selection[0], store.next_selection[1],        \
                            store.next_proxy[0], store.next_proxy[1]);               \
            sc_sel_path(active_drive, sel_slot ^ 1u, io_path);                       \
            CALL(mem_write_call(api, BOUNCE_PATH_OFF, io_path, 64u));                \
            if (reply.result != FX_OK) {                                             \
                sel_io_err = 1u;                                                     \
            }                                                                        \
            if (!sel_io_err) {                                                       \
                CALL(mem_write_call(api, BOUNCE_DATA_OFF, &sel_save_buf,             \
                                    SLTCARD_SEL_BYTES));                             \
                if (reply.result != FX_OK) {                                         \
                    sel_io_err = 1u;                                                 \
                }                                                                    \
            }                                                                        \
            if (!sel_io_err) {                                                       \
                CALL(native_open_call(api, SLTCARD_OPEN_TRUNC_WR));                  \
                if (reply.result != FX_OK) {                                         \
                    sel_io_err = 1u;                                                 \
                } else {                                                             \
                    cfg_fd = (int32_t)(uint32_t)reply.value[0];                      \
                    if (cfg_fd < 0) {                                                \
                        sel_io_err = 1u;                                             \
                    }                                                                \
                }                                                                    \
            }                                                                        \
            if (!sel_io_err) {                                                       \
                cfg_io_acc = 0u;                                                     \
                cfg_io_calls = 0u;                                                   \
                while (cfg_io_acc < SLTCARD_SEL_BYTES &&                             \
                       cfg_io_calls < CFG_IO_MAX_CALLS) {                            \
                    ++cfg_io_calls;                                                  \
                    write_u32_le(io_xfer, 0u);                                       \
                    CALL(mem_write_call(api, BOUNCE_OUT_OFF, io_xfer, 4u));          \
                    if (reply.result != FX_OK) {                                     \
                        sel_io_err = 1u;                                             \
                        break;                                                       \
                    }                                                                \
                    CALL(native_write_call(api, cfg_fd,                              \
                                           BOUNCE_DATA_OFF + cfg_io_acc,             \
                                           SLTCARD_SEL_BYTES - cfg_io_acc));         \
                    if (reply.result != FX_OK || (uint32_t)reply.value[0] != 0u) {   \
                        sel_io_err = 1u;                                             \
                        break;                                                       \
                    }                                                                \
                    CALL(mem_read_call(api, BOUNCE_OUT_OFF, 4u));                    \
                    if (reply.result != FX_OK || reply.bytes != 4u) {                \
                        sel_io_err = 1u;                                             \
                        break;                                                       \
                    }                                                                \
                    io_done = read_u32_le(reply.data);                               \
                    if (io_done == 0u || io_done > SLTCARD_SEL_BYTES - cfg_io_acc) { \
                        sel_io_err = 1u;                                             \
                        break;                                                       \
                    }                                                                \
                    cfg_io_acc += io_done;                                           \
                }                                                                    \
                if (cfg_io_acc != SLTCARD_SEL_BYTES) {                               \
                    sel_io_err = 1u;                                                 \
                }                                                                    \
                CALL(native_close_call(api, cfg_fd));                                \
                cfg_fd = -1;                                                         \
                if (reply.result != FX_OK || (uint32_t)reply.value[0] != 0u) {       \
                    sel_io_err = 1u;                                                 \
                }                                                                    \
            }                                                                        \
            if (!sel_io_err) {                                                       \
                sel_slot ^= 1u;                                                      \
                ++sel_seq;                                                           \
                ++sel_saves;                                                         \
            }                                                                        \
        }                                                                            \
    } while (0)

int fenestra_module_entry(const FxModuleServices *base) {
    if (!base || base->bytes != sizeof(FxServiceModule)) {
        return 1;
    }
    const FxServiceModule *api = (const FxServiceModule *)base;
    if (api->service_abi != FX_SERVICE_ABI) {
        return 2;
    }
    if (client.pending) {
        FxResult taken = fx_sdk_take(&client, api, &reply);
        if (taken == FX_BUSY) {
            return 0;
        }
        if (taken != FX_OK) {
            /* A2 Hardening: never return 3 / FAULT(5) on transient IPC errors. */
            client.pending = 0u;
            ++transient_errors;
            if ((pc & 1u) == 0u && pc > 0u && call_retries < 3u) {
                ++call_retries;
                pc -= 1u;
                return 0;
            }
            call_retries = 0u;
            reply = (FxServiceReply){ .result = taken };
        } else {
            call_retries = 0u;
        }
    }

    switch (pc) {
    case 0u:
        sc_init(&store, 1u, 0u);
        boot_start_ms = api->time_ms;
        native_fd = -1;
        cfg_fd = -1;
        probe_fd = -1;
        active_drive = SC_DRIVE_AUTO;
        next_drive = SC_DRIVE_AUTO;
        missing_reason = SC_MISS_NONE;
        slots_unlocked = 0u;
        ui_caps = 0u;
        cs_magic_seen = 0u;
        cs_drive_seen = 0u;
        cfg_slot = 1u;
        cfg_seq = 0u;
        sel_slot = 1u;
        sel_seq = 0u;
        sel_saves = 0u;
        sel_has_valid = 0u;
        sel_dirty = 0u;
        sel_io_err = 0u;
        ui_lang = 1u;

        CALL(fx_sdk_ui_query(&client, api, 0u));
        if (reply.result == FX_OK && reply.bytes == sizeof(FxUiInfo)) {
            FxUiInfo info;
            uint8_t *dst = (uint8_t *)&info;
            for (uint32_t k = 0u; k < (uint32_t)sizeof(info); ++k) {
                dst[k] = reply.data[k];
            }
            if (info.abi == 1u && info.bytes == sizeof(info)) {
                ui_caps = info.capabilities;
            }
        }
        for (boot_retries = 0u; boot_retries < 8u; ++boot_retries) {
            CALL(fx_sdk_open(&client, api, "card.files", 1u));
            if (reply.result == FX_OK) {
                break;
            }
            ++transient_errors;
            YIELD_TICK();
        }
        if (reply.result != FX_OK) {
            for (;;) {
                YIELD_TICK();
            }
        }
        files = reply.value[0];
        for (boot_retries = 0u; boot_retries < 8u; ++boot_retries) {
            CALL(fx_sdk_open(&client, api, "slots.state", 1u));
            if (reply.result == FX_OK) {
                break;
            }
            ++transient_errors;
            YIELD_TICK();
        }
        if (reply.result != FX_OK) {
            for (;;) {
                YIELD_TICK();
            }
        }
        slots = reply.value[0];

        /* Query active UI language BEFORE put_ctrl and BEFORE SLOTS_INFO
         * (op 15 is orthogonal to SLOTS_INFO and never unlocks c64-gate). */
        CALL(slot_call(api, SLOTS_LANG_GET, 0u, 0u, 0u, 0u, 0));
        if (reply.result == FX_OK && reply.value[1] == LANG_MAGIC) {
            ui_lang = (uint32_t)reply.value[0];
        }
        last_lang_ms = api->time_ms;

        set_text();
        for (row = PAGE; row <= CONTROLS; ++row) {
            for (boot_retries = 0u; boot_retries < 4u; ++boot_retries) {
                CALL(put_ctrl(api, row));
                if (reply.result == FX_OK) {
                    break;
                }
                ++transient_errors;
                YIELD_TICK();
            }
        }

        /* Open native card + memory + directory + machine bindings (optional for Auto,
         * required for explicit CFexpress A / SD B selection and SLTCARD*.CFG saves). */
        CALL(fx_sdk_open(&client, api, "core.memory", 1u));
        if (reply.result == FX_OK) {
            mem_h = reply.value[0];
            CALL(fx_sdk_memory_allocate(&client, api, mem_h, BOUNCE_BYTES, 1u));
            if (reply.result == FX_OK) {
                buf_h = reply.value[0];
                buf_native = reply.value[1];
            }
        }
        if (buf_native) {
            CALL(fx_sdk_open(&client, api, "camera.card.open", 1u));
            if (reply.result == FX_OK) card_open_h = reply.value[0];
            CALL(fx_sdk_open(&client, api, "camera.card.read", 1u));
            if (reply.result == FX_OK) card_read_h = reply.value[0];
            CALL(fx_sdk_open(&client, api, "camera.card.write", 1u));
            if (reply.result == FX_OK) card_write_h = reply.value[0];
            CALL(fx_sdk_open(&client, api, "camera.card.close", 1u));
            if (reply.result == FX_OK) card_close_h = reply.value[0];
            CALL(fx_sdk_open(&client, api, "camera.card.directory.open", 1u));
            if (reply.result == FX_OK) dir_open_h = reply.value[0];
            CALL(fx_sdk_open(&client, api, "camera.card.directory.close", 1u));
            if (reply.result == FX_OK) dir_close_h = reply.value[0];
            if (card_open_h && card_read_h && card_write_h && card_close_h) {
                has_native_io = 1u;
            }
            if (dir_open_h && dir_close_h) {
                has_dir_io = 1u;
            }
        }
        CALL(fx_sdk_open(&client, api, "core.machine", 1u));
        if (reply.result == FX_OK) {
            machine_h = reply.value[0];
            has_machine = 1u;
            CALL(machine_read_call(api, SLTCARD_CS_ADDR, 8u));
            if (reply.result == FX_OK) {
                cs_magic_seen = (uint32_t)reply.value[0];
                cs_drive_seen = (uint32_t)(reply.value[0] >> 32);
            }
        }

        /* Read dual-slot drive config SLTCARD0.CFG / SLTCARD1.CFG from boot card.
         * Retry transient cardslot.c REFUSED (0x8000ff01) with YIELD_TICK() up to
         * WAIT_MOUNT_MS so dual-card mount settlement never silently resets strict
         * CFx A / SD B back to Auto. */
        cfg_valid[0] = 0u;
        cfg_valid[1] = 0u;
        if (has_native_io) {
            cfg_wait_start = api->time_ms;
            for (cfg_load_idx = 0u; cfg_load_idx < 2u; ++cfg_load_idx) {
                sc_cfg_path(cfg_load_idx, io_path);
                cfg_fd = -1;
                cfg_io_acc = 0u;
                cfg_io_calls = 0u;
                cfg_io_err = 0u;
                CALL(mem_write_call(api, BOUNCE_PATH_OFF, io_path, 64u));
                if (reply.result != FX_OK) {
                    continue;
                }
                for (;;) {
                    CALL(native_open_call(api, SLTCARD_OPEN_RDONLY));
                    if (reply.result != FX_OK) {
                        cfg_fd = -1;
                        break;
                    }
                    cfg_fd = (int32_t)(uint32_t)reply.value[0];
                    if ((uint32_t)cfg_fd != SLTCARD_OPEN_REFUSED) {
                        break;
                    }
                    if (api->time_ms - cfg_wait_start >= WAIT_MOUNT_MS) {
                        cfg_fd = -1;
                        break;
                    }
                    cfg_last_try = api->time_ms;
                    while (api->time_ms - cfg_last_try < PROBE_MS) {
                        YIELD_TICK();
                        if (api->time_ms - cfg_wait_start >= WAIT_MOUNT_MS) {
                            break;
                        }
                    }
                }
                if (cfg_fd < 0) {
                    continue;
                }
                while (cfg_io_acc < SLTCARD_CFG_READ_MAX && cfg_io_calls < CFG_IO_MAX_CALLS) {
                    ++cfg_io_calls;
                    write_u32_le(io_xfer, 0u);
                    CALL(mem_write_call(api, BOUNCE_OUT_OFF, io_xfer, 4u));
                    if (reply.result != FX_OK) {
                        cfg_io_err = 1u;
                        break;
                    }
                    CALL(native_read_call(api, cfg_fd, BOUNCE_DATA_OFF + cfg_io_acc,
                                          SLTCARD_CFG_READ_MAX - cfg_io_acc));
                    if (reply.result != FX_OK || (uint32_t)reply.value[0] != 0u) {
                        cfg_io_err = 1u;
                        break;
                    }
                    CALL(mem_read_call(api, BOUNCE_OUT_OFF, 4u));
                    if (reply.result != FX_OK || reply.bytes != 4u) {
                        cfg_io_err = 1u;
                        break;
                    }
                    io_done = read_u32_le(reply.data);
                    if (io_done > SLTCARD_CFG_READ_MAX - cfg_io_acc) {
                        cfg_io_err = 1u;
                        break;
                    }
                    cfg_io_acc += io_done;
                    if (io_done == 0u) {
                        break;
                    }
                }
                CALL(native_close_call(api, cfg_fd));
                cfg_fd = -1;
                if (reply.result != FX_OK || (uint32_t)reply.value[0] != 0u) {
                    cfg_io_err = 1u;
                }
                if (!cfg_io_err && cfg_io_acc == SLTCARD_CFG_BYTES) {
                    CALL(mem_read_call(api, BOUNCE_DATA_OFF, SLTCARD_CFG_BYTES));
                    if (reply.result == FX_OK && reply.bytes == SLTCARD_CFG_BYTES) {
                        uint8_t *dst = (uint8_t *)&cfg_slots[cfg_load_idx];
                        for (uint32_t k = 0u; k < SLTCARD_CFG_BYTES; ++k) {
                            dst[k] = reply.data[k];
                        }
                        if (sc_cfg_valid(&cfg_slots[cfg_load_idx])) {
                            cfg_valid[cfg_load_idx] = 1u;
                        }
                    }
                }
            }
        }
        sc_cfg_select_dual(&cfg_slots[0], cfg_valid[0],
                           &cfg_slots[1], cfg_valid[1],
                           &cfg_active, &cfg_slot);
        active_drive = cfg_active.drive_mode;
        next_drive = active_drive;
        cfg_seq = cfg_active.seq;

        /* Decide startup path: Auto loads immediately; strict A/B checks target card first
         * BEFORE calling SLOTS_INFO (so c64-gate keeps LIMIT=9 when missing). */
        if (active_drive == SC_DRIVE_AUTO) {
            ready_to_load = 1u;
        } else if (!has_native_io || !has_dir_io) {
            missing_reason = SC_MISS_NO_NATIVE_IO;
            state = ST_CARD_MISSING;
        } else {
            /* If strict A is requested, verify cardslot.c has NOT latched drive=2 (SD),
             * because cardslot.c rewrites A:\FENESTRA\... -> B:\FENESTRA\... in that case. */
            if (active_drive == SC_DRIVE_A && has_machine) {
                CALL(machine_read_call(api, SLTCARD_CS_ADDR, 8u));
                if (reply.result == FX_OK) {
                    uint32_t cs_magic = (uint32_t)reply.value[0];
                    uint32_t cs_drive = (uint32_t)(reply.value[0] >> 32);
                    cs_magic_seen = cs_magic;
                    cs_drive_seen = cs_drive;
                    if (cs_magic == SLTCARD_CS_MAGIC && cs_drive == 2u) {
                        missing_reason = SC_MISS_BOOTED_SD;
                        state = ST_CARD_MISSING;
                    }
                }
            }
            if (state != ST_CARD_MISSING) {
                state = ST_WAITING_CARD;
                wait_start_ms = api->time_ms;
                last_probe_ms = api->time_ms;
                probe_done_once = 0u;
                mounted_no_hits = 0u;
            }
        }

        set_text();
        CALL(publish_status(api));
        CALL(put_ctrl(api, R_LIMIT));
        CALL(put_ctrl(api, R_DRIVE));
        CALL(put_ctrl(api, R_STATE));
        CALL(put_ctrl(api, R_STORE));
        CALL(put_ctrl(api, R_CSL1));

        for (;;) {
            /* 1. Always poll UI events (keeps R_DRIVE responsive even while waiting/missing). */
            CALL(fx_sdk_events(&client, api));
            if (reply.result == FX_OK && reply.bytes == sizeof(FxUiEvent)) {
                FxUiEvent e;
                uint8_t *p = (uint8_t *)&e;
                for (uint32_t k = 0u; k < (uint32_t)sizeof(e); ++k) {
                    p[k] = reply.data[k];
                }
                if (((e.event == FX_INPUT_CHANGE && e.value <= 1u) || e.event == FX_INPUT_ACTIVATE) &&
                    e.control == R_LIMIT && slots_unlocked && slots_active == 64u) {
                    if (e.event == FX_INPUT_CHANGE) {
                        store.next_limit = (e.value == 1u) ? 32u : 64u;
                    } else {
                        store.next_limit = (store.next_limit ? store.next_limit : limit) == 64u ? 32u : 64u;
                    }
                    want_commit = 1u;
                    set_text();
                    CALL(put_ctrl(api, R_LIMIT));
                } else if (((e.event == FX_INPUT_CHANGE && e.value <= 2u) || e.event == FX_INPUT_ACTIVATE) &&
                           e.control == R_DRIVE) {
                    next_drive = (e.event == FX_INPUT_CHANGE) ? e.value : ((next_drive + 1u) % 3u);
                    cfg_save_pending = 1u;
                    cfg_save_err = 0u;
                    set_text();
                    CALL(put_ctrl(api, R_DRIVE));
                } else if (e.event == FX_INPUT_ACTIVATE && e.control == R_CSL1 && has_machine) {
                    CALL(machine_read_call(api, SLTCARD_CS_ADDR, 8u));
                    if (reply.result == FX_OK) {
                        cs_magic_seen = (uint32_t)reply.value[0];
                        cs_drive_seen = (uint32_t)(reply.value[0] >> 32);
                    }
                    set_text();
                    CALL(put_ctrl(api, R_CSL1));
                }
            }

            /* 2. Persist drive selection atomically to alternate SLTCARD0/1.CFG on boot card. */
            if (cfg_save_pending) {
                cfg_save_pending = 0u;
                cfg_io_err = 0u;
                if (!has_native_io) {
                    cfg_io_err = 1u;
                } else {
                    sc_cfg_finalize(&cfg_save_buf, cfg_seq + 1u, (uint8_t)next_drive);
                    sc_cfg_path(cfg_slot ^ 1u, io_path);
                    CALL(mem_write_call(api, BOUNCE_PATH_OFF, io_path, 64u));
                    if (reply.result != FX_OK) {
                        cfg_io_err = 1u;
                    }
                    if (!cfg_io_err) {
                        CALL(mem_write_call(api, BOUNCE_DATA_OFF, &cfg_save_buf,
                                            SLTCARD_CFG_BYTES));
                        if (reply.result != FX_OK) {
                            cfg_io_err = 1u;
                        }
                    }
                    if (!cfg_io_err) {
                        CALL(native_open_call(api, SLTCARD_OPEN_TRUNC_WR));
                        if (reply.result != FX_OK) {
                            cfg_io_err = 1u;
                        } else {
                            cfg_fd = (int32_t)(uint32_t)reply.value[0];
                            if (cfg_fd < 0) {
                                cfg_io_err = 1u;
                            }
                        }
                    }
                    if (!cfg_io_err) {
                        cfg_io_acc = 0u;
                        cfg_io_calls = 0u;
                        while (cfg_io_acc < SLTCARD_CFG_BYTES &&
                               cfg_io_calls < CFG_IO_MAX_CALLS) {
                            ++cfg_io_calls;
                            write_u32_le(io_xfer, 0u);
                            CALL(mem_write_call(api, BOUNCE_OUT_OFF, io_xfer, 4u));
                            if (reply.result != FX_OK) {
                                cfg_io_err = 1u;
                                break;
                            }
                            CALL(native_write_call(api, cfg_fd,
                                                   BOUNCE_DATA_OFF + cfg_io_acc,
                                                   SLTCARD_CFG_BYTES - cfg_io_acc));
                            if (reply.result != FX_OK || (uint32_t)reply.value[0] != 0u) {
                                cfg_io_err = 1u;
                                break;
                            }
                            CALL(mem_read_call(api, BOUNCE_OUT_OFF, 4u));
                            if (reply.result != FX_OK || reply.bytes != 4u) {
                                cfg_io_err = 1u;
                                break;
                            }
                            io_done = read_u32_le(reply.data);
                            if (io_done == 0u || io_done > SLTCARD_CFG_BYTES - cfg_io_acc) {
                                cfg_io_err = 1u;
                                break;
                            }
                            cfg_io_acc += io_done;
                        }
                        if (cfg_io_acc != SLTCARD_CFG_BYTES) {
                            cfg_io_err = 1u;
                        }
                        CALL(native_close_call(api, cfg_fd));
                        cfg_fd = -1;
                        if (reply.result != FX_OK || (uint32_t)reply.value[0] != 0u) {
                            cfg_io_err = 1u;
                        }
                    }
                }
                if (!cfg_io_err) {
                    cfg_slot ^= 1u;
                    ++cfg_seq;
                    ++cfg_saves;
                    cfg_save_err = 0u;
                } else {
                    cfg_save_err = 1u;
                }
                set_text();
                CALL(publish_status(api));
                CALL(put_ctrl(api, R_DRIVE));
            }

            /* 2.5. Live UI language refresh in ALL states (incl. missing/waiting/other card). */
            if (api->time_ms - last_lang_ms >= LANG_POLL_MS) {
                last_lang_ms = api->time_ms;
                CALL(slot_call(api, SLOTS_LANG_GET, 0u, 0u, 0u, 0u, 0));
                if (reply.result == FX_OK && reply.value[1] == LANG_MAGIC &&
                    (uint32_t)reply.value[0] != ui_lang) {
                    ui_lang = (uint32_t)reply.value[0];
                    set_text();
                    for (row = PAGE; row <= CONTROLS; ++row) {
                        CALL(put_ctrl(api, row));
                    }
                }
            }

            /* 3. Strict missing state: never unlock C64, never touch sc*.rec. */
            if (state == ST_CARD_MISSING) {
                continue;
            }

            /* 4. Waiting for target card (strict A/B) to finish mounting.
             *    Gentle BOOT_SETTLE_MS (500 ms) yield before the very first probe avoids
             *    contending with native SD/CFx mount burst right after power-on. */
            if (state == ST_WAITING_CARD) {
                if (!probe_done_once) {
                    if (api->time_ms - boot_start_ms < BOOT_SETTLE_MS) {
                        continue;
                    }
                } else if (api->time_ms - last_probe_ms < PROBE_MS) {
                    continue;
                }
                probe_done_once = 1u;
                last_probe_ms = api->time_ms;

                /* Probe \FENESTRA first (matches cardslot.c): single opendir on ready card. */
                sc_fenestra_dir_path(active_drive, io_path);
                PROBE_DIR_NOW();
                if (probe_ok) {
                    /* Re-verify cardslot.c did not latch SD (drive=2) while waiting for A,
                     * otherwise A:\FENESTRA would silently rewrite to B:\FENESTRA. */
                    if (active_drive == SC_DRIVE_A && has_machine) {
                        CALL(machine_read_call(api, SLTCARD_CS_ADDR, 8u));
                        if (reply.result == FX_OK) {
                            uint32_t cs_magic = (uint32_t)reply.value[0];
                            uint32_t cs_drive = (uint32_t)(reply.value[0] >> 32);
                            cs_magic_seen = cs_magic;
                            cs_drive_seen = cs_drive;
                            if (cs_magic == SLTCARD_CS_MAGIC && cs_drive == 2u) {
                                missing_reason = SC_MISS_BOOTED_SD;
                                state = ST_CARD_MISSING;
                                set_text();
                                CALL(publish_status(api));
                                CALL(put_ctrl(api, R_LIMIT));
                                CALL(put_ctrl(api, R_STATE));
                                CALL(put_ctrl(api, R_STORE));
                                CALL(put_ctrl(api, R_CSL1));
                                continue;
                            }
                        }
                    }
                    ready_to_load = 1u;
                } else {
                    fen_probe_status = probe_status;
                    /* cardslot.c transient REFUSED (0xff01 / 0x8000ff01) means dual-card
                     * routing is still settling, NOT that \FENESTRA is absent on A:. */
                    if (fen_probe_status == SLTCARD_DIR_REFUSED ||
                        fen_probe_status == SLTCARD_OPEN_REFUSED) {
                        mounted_no_hits = 0u;
                        if (api->time_ms - wait_start_ms >= WAIT_MOUNT_MS) {
                            missing_reason = SC_MISS_TIMEOUT;
                            state = ST_CARD_MISSING;
                            set_text();
                            CALL(publish_status(api));
                            CALL(put_ctrl(api, R_LIMIT));
                            CALL(put_ctrl(api, R_STATE));
                            CALL(put_ctrl(api, R_STORE));
                            CALL(put_ctrl(api, R_CSL1));
                        }
                        continue;
                    }
                    /* Distinguish "still mounting / absent" from "volume mounted without \FENESTRA". */
                    sc_root_dir_path(active_drive, io_path);
                    PROBE_DIR_NOW();
                    if (!probe_ok) {
                        mounted_no_hits = 0u;
                        if (api->time_ms - wait_start_ms >= WAIT_MOUNT_MS) {
                            missing_reason = SC_MISS_TIMEOUT;
                            state = ST_CARD_MISSING;
                            set_text();
                            CALL(publish_status(api));
                            CALL(put_ctrl(api, R_LIMIT));
                            CALL(put_ctrl(api, R_STATE));
                            CALL(put_ctrl(api, R_STORE));
                            CALL(put_ctrl(api, R_CSL1));
                        }
                        continue;
                    }
                    ++mounted_no_hits;
                    if (fen_probe_status == SLTCARD_OPEN_NOT_FOUND ||
                        mounted_no_hits >= 2u ||
                        api->time_ms - wait_start_ms >= WAIT_MOUNT_MS) {
                        missing_reason = SC_MISS_NO_FENESTRA;
                        state = ST_CARD_MISSING;
                        set_text();
                        CALL(publish_status(api));
                        CALL(put_ctrl(api, R_LIMIT));
                        CALL(put_ctrl(api, R_STATE));
                        CALL(put_ctrl(api, R_STORE));
                        CALL(put_ctrl(api, R_CSL1));
                    }
                    continue;
                }
            }

            /* 5. One-shot boot load, unlock C64, inject records and restore selection.
             *    Auto mode also yields BOOT_SETTLE_MS (500 ms) to stay clear of the
             *    initial native SD mount burst while still unlocking C64 well under 2-3s. */
            if (ready_to_load) {
                if (api->time_ms - boot_start_ms < BOOT_SETTLE_MS) {
                    continue;
                }
                ready_to_load = 0u;
                for (boot_retries = 0u; boot_retries < 5u; ++boot_retries) {
                    CALL(slot_call(api, SLOTS_INFO, 0u, 0u, 0u, 0u, 0));
                    if (reply.result == FX_OK &&
                        (reply.value[0] == 32u || reply.value[0] == 64u)) {
                        break;
                    }
                    ++transient_errors;
                    YIELD_TICK();
                }
                if (reply.result != FX_OK ||
                    (reply.value[0] != 32u && reply.value[0] != 64u)) {
                    missing_reason = SC_MISS_NO_NATIVE_IO;
                    state = ST_CARD_MISSING;
                    set_text();
                    CALL(publish_status(api));
                    CALL(put_ctrl(api, R_LIMIT));
                    CALL(put_ctrl(api, R_STATE));
                    CALL(put_ctrl(api, R_STORE));
                    CALL(put_ctrl(api, R_CSL1));
                    continue;
                }
                slots_active = limit = (uint32_t)reply.value[0];
                slots_unlocked = 1u;
                state = ST_LOADING;
                CALL(publish_status(api));

                sc_begin_load(&store);
                answer = (ScReply){ 0 };
                RUN_MACHINE();
                load_status = store.status;

                /* Read dual-slot 64 B selection override SCSEL0/1.CFG from the active
                 * card. Apply only if valid, matching store_id (when known), and
                 * base_gen >= store.head (so newer PC-written SC*.REC takes precedence). */
                sel_valid[0] = 0u;
                sel_valid[1] = 0u;
                if (has_native_io) {
                    cfg_wait_start = api->time_ms;
                    for (sel_load_idx = 0u; sel_load_idx < 2u; ++sel_load_idx) {
                        sc_sel_path(active_drive, sel_load_idx, io_path);
                        cfg_fd = -1;
                        cfg_io_acc = 0u;
                        cfg_io_calls = 0u;
                        cfg_io_err = 0u;
                        CALL(mem_write_call(api, BOUNCE_PATH_OFF, io_path, 64u));
                        if (reply.result != FX_OK) {
                            continue;
                        }
                        for (;;) {
                            CALL(native_open_call(api, SLTCARD_OPEN_RDONLY));
                            if (reply.result != FX_OK) {
                                cfg_fd = -1;
                                break;
                            }
                            cfg_fd = (int32_t)(uint32_t)reply.value[0];
                            if ((uint32_t)cfg_fd != SLTCARD_OPEN_REFUSED) {
                                break;
                            }
                            if (api->time_ms - cfg_wait_start >= WAIT_MOUNT_MS) {
                                cfg_fd = -1;
                                break;
                            }
                            cfg_last_try = api->time_ms;
                            while (api->time_ms - cfg_last_try < PROBE_MS) {
                                YIELD_TICK();
                                if (api->time_ms - cfg_wait_start >= WAIT_MOUNT_MS) {
                                    break;
                                }
                            }
                        }
                        if (cfg_fd < 0) {
                            continue;
                        }
                        while (cfg_io_acc < SLTCARD_SEL_READ_MAX &&
                               cfg_io_calls < CFG_IO_MAX_CALLS) {
                            ++cfg_io_calls;
                            write_u32_le(io_xfer, 0u);
                            CALL(mem_write_call(api, BOUNCE_OUT_OFF, io_xfer, 4u));
                            if (reply.result != FX_OK) {
                                cfg_io_err = 1u;
                                break;
                            }
                            CALL(native_read_call(api, cfg_fd,
                                                  BOUNCE_DATA_OFF + cfg_io_acc,
                                                  SLTCARD_SEL_READ_MAX - cfg_io_acc));
                            if (reply.result != FX_OK || (uint32_t)reply.value[0] != 0u) {
                                cfg_io_err = 1u;
                                break;
                            }
                            CALL(mem_read_call(api, BOUNCE_OUT_OFF, 4u));
                            if (reply.result != FX_OK || reply.bytes != 4u) {
                                cfg_io_err = 1u;
                                break;
                            }
                            io_done = read_u32_le(reply.data);
                            if (io_done > SLTCARD_SEL_READ_MAX - cfg_io_acc) {
                                cfg_io_err = 1u;
                                break;
                            }
                            cfg_io_acc += io_done;
                            if (io_done == 0u) {
                                break;
                            }
                        }
                        CALL(native_close_call(api, cfg_fd));
                        cfg_fd = -1;
                        if (reply.result != FX_OK || (uint32_t)reply.value[0] != 0u) {
                            cfg_io_err = 1u;
                        }
                        if (!cfg_io_err && cfg_io_acc == SLTCARD_SEL_BYTES) {
                            CALL(mem_read_call(api, BOUNCE_DATA_OFF, SLTCARD_SEL_BYTES));
                            if (reply.result == FX_OK && reply.bytes == SLTCARD_SEL_BYTES) {
                                uint8_t *dst = (uint8_t *)&sel_slots[sel_load_idx];
                                for (uint32_t k = 0u; k < SLTCARD_SEL_BYTES; ++k) {
                                    dst[k] = reply.data[k];
                                }
                                if (sc_sel_valid(&sel_slots[sel_load_idx])) {
                                    sel_valid[sel_load_idx] = 1u;
                                }
                            }
                        }
                    }
                }
                sc_sel_select_dual(&sel_slots[0], sel_valid[0],
                                   &sel_slots[1], sel_valid[1],
                                   &sel_active, &sel_slot, &sel_has_valid);
                sel_seq = sel_has_valid ? sel_active.seq : 0u;
                if (sel_has_valid &&
                    sel_active.base_gen >= store.head &&
                    (store.store_id == 0u || sel_active.store_id == 0u ||
                     sel_active.store_id == store.store_id)) {
                    store.selection[0] = sel_active.still_sel;
                    store.selection[1] = sel_active.video_sel;
                    store.proxy[0] = sel_active.still_proxy;
                    store.proxy[1] = sel_active.video_proxy;
                }

                if (store.limit == 32u && slots_active == 64u) {
                    CALL(slot_call(api, SLOTS_LIMIT, 0u, 0u, 0u, 32u, 0));
                    if (reply.result == FX_OK) {
                        limit = 32u;
                    }
                }
                store.next_limit = store.limit;
                store.next_selection[0] = store.selection[0];
                store.next_selection[1] = store.selection[1];
                store.next_proxy[0] = store.proxy[0];
                store.next_proxy[1] = store.proxy[1];
                used_count = 0u;
                for (i = 0u; i < SC_SLOTS; ++i) {
                    used_count += store.used[i];
                }

                state = ST_INJECTING;
                CALL(publish_status(api));
                for (i = 0u; i < SC_SLOTS; ++i) {
                    base_crc[i] = 0u;
                }
                for (i = 0u; i < SC_SLOTS; ++i) {
                    if (!store.used[i] || store.bad[i]) {
                        continue;
                    }
                    /* Compute base_crc per slot inside the yielding loop so we never hash
                     * all 128 slots in a single un-yielded EL0 call. */
                    base_crc[i] = sc_crc32(0u, store.records[i], SC_RECORD);
                    if (!active_slot_idx(i)) {
                        YIELD_TICK();
                        continue;
                    }
                    for (offset = 0u; offset < SC_RECORD; offset += CHUNK) {
                        seen = SC_RECORD - offset;
                        if (seen > CHUNK) {
                            seen = CHUNK;
                        }
                        CALL(slot_call(api, SLOTS_WRITE, i, offset, seen, 0u,
                                       store.records[i] + offset));
                        if (reply.result != FX_OK) {
                            break;
                        }
                    }
                    if (offset < SC_RECORD) {
                        ++inject_failed;
                        continue;
                    }
                    CALL(slot_call(api, SLOTS_COMMIT, i, 0u, 0u, 0u, 0));
                    if (reply.result == FX_OK) {
                        ++injected;
                    } else {
                        ++inject_failed;
                    }
                }
                if (injected) {
                    CALL(slot_call(api, SLOTS_INVALIDATE, 0u, 0u, 0u, 0u, 0));
                }

                state = ST_APPLYING;
                CALL(publish_status(api));
                CALL(slot_call(api, SLOTS_SELECTION, 0u, 0u, 0u, 0u, 0));
                selected[0] = (uint32_t)reply.value[0];
                selected[1] = reply.value[2] ? 0u : (uint32_t)reply.value[1];
                for (i = 0u; i < 2u; ++i) {
                    legal[i] = sc_selector_legal(i, selected[i])
                                   ? selected[i]
                                   : (store.proxy[i] ? store.proxy[i]
                                                     : (i ? 0u : DEFAULT_PHOTO));
                }
                moved = 0u;
                held = 0u;
                for (i = 0u; i < 2u; ++i) {
                    uint32_t want = store.selection[i], index = 0u;
                    if (!sc_selector_extended(want, &index)) {
                        moved |= 1u << i;
                        continue;
                    }
                    if (store.used[index] && !store.bad[index] &&
                        !active_slot_idx(index) && index % 64u < slots_active) {
                        held |= 1u << i;
                    }
                    if (!store.used[index] || store.bad[index] ||
                        !active_slot_idx(index)) {
                        continue;
                    }
                    if (selected[i] != store.proxy[i] &&
                        selected[i] != (i ? 0u : DEFAULT_PHOTO)) {
                        moved |= 1u << i;
                        continue;
                    }
                    CALL(slot_call(api, SLOTS_APPLY, i, 0u, 0u, store.selection[i], 0));
                    if (reply.result == FX_OK) {
                        ++applied;
                    } else {
                        ++apply_failed;
                    }
                }
                CALL(slot_call(api, SLOTS_SELECTION, 0u, 0u, 0u, 0u, 0));
                selected[0] = (uint32_t)reply.value[0];
                selected[1] = reply.value[2] ? 0u : (uint32_t)reply.value[1];
                sel_dirty = 0u;
                for (i = 0u; i < 2u; ++i) {
                    if ((moved >> i & 1u) && selected[i] != store.selection[i]) {
                        store.next_selection[i] = selected[i];
                        store.next_proxy[i] = sc_selector_legal(i, selected[i])
                                                  ? selected[i]
                                                  : legal[i];
                        if (store.head || selector_extended(selected[i])) {
                            sel_dirty = 1u;
                            sel_dirty_ms = api->time_ms;
                        }
                    }
                }

                CALL(slot_call(api, SLOTS_SUMMARY, 0u, 0u, 0u, 0u, 0));
                summary = pending_summary = (uint32_t)reply.value[0];
                want_commit = 0u;
                state = ST_RUNNING;
                set_text();
                CALL(publish_status(api));
                for (row = PAGE; row <= CONTROLS; ++row) {
                    CALL(put_ctrl(api, row));
                }
                last_poll = api->time_ms;
                last_summary_ms = api->time_ms;
                continue;
            }

            /* 6. Normal running loop: immediate RAM-only proxy sync, 2s lightweight
             *    selection polling (10s debounced SCSEL0/1.CFG overwrite), and 10s
             *    throttled SLOTS_SUMMARY polling (SC*.REC written ONLY on slot/limit edits). */
            if (store.next_proxy[0] != sent_proxy[0] ||
                store.next_proxy[1] != sent_proxy[1] || !proxy_sent) {
                CALL(slot_call(api, SLOTS_PROXY, store.next_proxy[1], 0u, 0u,
                               store.next_proxy[0], 0));
                if (reply.result == FX_OK) {
                    sent_proxy[0] = store.next_proxy[0];
                    sent_proxy[1] = store.next_proxy[1];
                    proxy_sent = 1u;
                }
            }
            if (api->time_ms - last_poll < POLL_MS) {
                continue;
            }
            last_poll = api->time_ms;
            if (state == ST_OTHER_CARD) {
                continue;
            }

            /* 6a. Lightweight 2s selection check (1 small IPC, 0 CRC32, 0 SD I/O). */
            CALL(slot_call(api, SLOTS_SELECTION, 0u, 0u, 0u, 0u, 0));
            if (reply.result == FX_OK) {
                for (i = 0u; i < 2u; ++i) {
                    uint32_t now = i ? (reply.value[2] ? 0u : (uint32_t)reply.value[1])
                                     : (uint32_t)reply.value[0];
                    if (now == selected[i]) {
                        continue;
                    }
                    if ((held >> i & 1u) && !sc_selector_extended(now, 0)) {
                        selected[i] = now;
                        continue;
                    }
                    held &= ~(1u << i);
                    if (sc_selector_legal(i, selected[i])) {
                        legal[i] = selected[i];
                    }
                    selected[i] = now;
                    store.next_proxy[i] = sc_selector_legal(i, now) ? now : legal[i];
                    store.next_selection[i] = now;
                    if (store.next_selection[0] != store.selection[0] ||
                        store.next_selection[1] != store.selection[1] ||
                        store.next_proxy[0] != store.proxy[0] ||
                        store.next_proxy[1] != store.proxy[1]) {
                        sel_dirty = 1u;
                        sel_dirty_ms = api->time_ms;
                    } else {
                        sel_dirty = 0u;
                    }
                }
            }

            /* 6b. Heavy SLOTS_SUMMARY (112 KiB EL1 CRC32): throttled to every 10s while
             *     idle, confirmed 2s later once an actual slot-content edit is detected. */
            need_slot_scan = 0u;
            if (pending_summary != summary ||
                api->time_ms - last_summary_ms >= SUMMARY_POLL_MS) {
                last_summary_ms = api->time_ms;
                CALL(slot_call(api, SLOTS_SUMMARY, 0u, 0u, 0u, 0u, 0));
                if (reply.result == FX_OK) {
                    uint32_t now = (uint32_t)reply.value[0];
                    if (now != pending_summary) {
                        pending_summary = now;
                    } else if (now != summary) {
                        need_slot_scan = 1u;
                    }
                }
            }

            if (need_slot_scan) {
                scan_failed = 0u;
                for (i = 0u; i < SC_SLOTS; ++i) {
                    if (!active_slot_idx(i)) {
                        continue;
                    }
                    CALL(slot_call(api, SLOTS_HASH, i, 0u, 0u, 0u, 0));
                    if (reply.result != FX_OK) {
                        ++transient_errors;
                        scan_failed = 1u;
                        continue;
                    }
                    new_slot_crc = (uint32_t)reply.value[0];
                    if (new_slot_crc == base_crc[i] || reply.value[1] != 1u) {
                        continue;
                    }
                    slot_read_ok = 1u;
                    for (offset = 0u; offset < SC_RECORD; offset += seen) {
                        seen = SC_RECORD - offset;
                        if (seen > CHUNK) {
                            seen = CHUNK;
                        }
                        CALL(slot_call(api, SLOTS_READ, i, offset, seen, 0u, 0));
                        if (reply.result != FX_OK || reply.bytes != seen) {
                            /* A2 Hardening: never return 8! Keep old base_crc[i] and retry next poll. */
                            ++transient_errors;
                            slot_read_ok = 0u;
                            scan_failed = 1u;
                            break;
                        }
                        for (uint32_t k = 0u; k < seen; ++k) {
                            record[offset + k] = reply.data[k];
                        }
                    }
                    if (slot_read_ok) {
                        base_crc[i] = new_slot_crc;
                        sc_put_record(&store, i, record);
                    }
                }
                if (!scan_failed) {
                    summary = pending_summary;
                }
            }

            /* 6c. Decide persistence path:
             *     commit_kind == 1: incremental SC*.REC (slot recipes or limit changed,
             *                       or fallback when native IO is absent) + sync SCSEL.
             *     commit_kind == 2: selection-only 64 B SCSEL0/1.CFG overwrite after
             *                       10s debounce (0 SC*.REC, 0 SLOTS_HASH, 0 readback). */
            commit_kind = 0u;
            {
                uint32_t dirty_slots = 0u;
                for (uint32_t k = 0u; k < SC_SLOTS; ++k) {
                    dirty_slots |= store.dirty[k];
                }
                uint32_t limit_changed = store.next_limit != store.limit;
                uint32_t sel_changed = (store.next_selection[0] != store.selection[0] ||
                                        store.next_selection[1] != store.selection[1] ||
                                        store.next_proxy[0] != store.proxy[0] ||
                                        store.next_proxy[1] != store.proxy[1]);
                uint32_t sel_worth_saving = sel_changed &&
                                            (store.head ||
                                             selector_extended(store.next_selection[0]) ||
                                             selector_extended(store.next_selection[1]));
                uint32_t sel_due = (sel_dirty &&
                                    api->time_ms - sel_dirty_ms >= SEL_DEBOUNCE_MS);
                if (sel_due && !sel_worth_saving) {
                    sel_dirty = 0u;
                    sel_due = 0u;
                }
                if (dirty_slots || limit_changed || (sel_due && !has_native_io)) {
                    want_commit = 1u;
                    commit_kind = 1u;
                } else {
                    want_commit = 0u;
                    if (sel_due && has_native_io) {
                        commit_kind = 2u;
                    }
                }
            }

            if (!commit_kind) {
                continue;
            }

            sc_begin_check(&store);
            answer = (ScReply){ 0 };
            RUN_MACHINE();
            if (store.status == SC_OTHER_CARD) {
                ++checks_other;
                state = ST_OTHER_CARD;
                sel_dirty = 0u;
                want_commit = 0u;
                set_text();
                CALL(publish_status(api));
                CALL(put_ctrl(api, R_STATE));
                continue;
            }
            if (store.status != SC_SAME_CARD) {
                if (commit_kind == 2u) {
                    sel_dirty_ms = api->time_ms;
                }
                continue;
            }

            sel_save_now = 0u;
            if (commit_kind == 1u) {
                store.entropy = (api->time_ms + 1u) * UINT64_C(0x9e3779b97f4a7c15);
                sc_begin_commit(&store);
                answer = (ScReply){ 0 };
                RUN_MACHINE();
                commit_status = store.status;
                if (store.status == SC_OK) {
                    ++commits;
                    want_commit = 0u;
                    sel_dirty = 0u;
                    if (has_native_io) {
                        sel_save_now = 1u;
                    }
                }
                used_count = 0u;
                for (i = 0u; i < SC_SLOTS; ++i) {
                    used_count += store.used[i];
                }
                set_text();
                CALL(publish_status(api));
                CALL(put_ctrl(api, R_STATE));
                CALL(put_ctrl(api, R_STORE));
            } else {
                sel_save_now = 2u;
            }

            if (sel_save_now) {
                SAVE_SEL_CFG();
                if (sel_save_now == 2u) {
                    if (!sel_io_err) {
                        store.selection[0] = store.next_selection[0];
                        store.selection[1] = store.next_selection[1];
                        store.proxy[0] = store.next_proxy[0];
                        store.proxy[1] = store.next_proxy[1];
                        sel_dirty = 0u;
                    } else {
                        sel_dirty_ms = api->time_ms;
                    }
                }
                set_text();
                CALL(put_ctrl(api, R_CSL1));
                sel_save_now = 0u;
            }
        }
    default:
        ++transient_errors;
        pc = 0u;
        return 0;
    }
}

#ifdef SLOTCARD_HOST_TEST
#include <string.h>
void slotcard_host_reset(void) {
    memset(&client, 0, sizeof(client));
    files = slots = handle = 0u;
    mem_h = buf_h = buf_native = machine_h = 0u;
    card_open_h = card_read_h = card_write_h = card_close_h = 0u;
    dir_open_h = dir_close_h = 0u;
    has_native_io = has_dir_io = has_machine = 0u;
    memset(&store, 0, sizeof(store));
    memset(&io, 0, sizeof(io));
    memset(&answer, 0, sizeof(answer));
    memset(&reply, 0, sizeof(reply));
    pc = row = i = offset = seen = moved = held = proxy_sent = 0u;
    memset(sent_proxy, 0, sizeof(sent_proxy));
    memset(base_crc, 0, sizeof(base_crc));
    summary = pending_summary = 0u;
    memset(selected, 0, sizeof(selected));
    memset(legal, 0, sizeof(legal));
    state = load_status = injected = inject_failed = applied = apply_failed = 0u;
    commits = commit_status = checks_other = used_count = want_commit = 0u;
    slots_active = limit = slots_unlocked = ui_caps = 0u;
    cs_magic_seen = cs_drive_seen = 0u;
    ui_lang = 1u;
    last_poll = last_lang_ms = wait_start_ms = last_probe_ms = 0u;
    boot_start_ms = last_summary_ms = sel_dirty_ms = 0u;
    cfg_wait_start = cfg_last_try = 0u;
    memset(record, 0, sizeof(record));
    memset(text, 0, sizeof(text));
    active_drive = next_drive = missing_reason = 0u;
    cfg_slot = cfg_seq = cfg_saves = cfg_save_err = cfg_save_pending = 0u;
    cfg_load_idx = cfg_io_acc = cfg_io_calls = cfg_io_err = 0u;
    sel_slot = sel_seq = sel_saves = sel_has_valid = sel_dirty = sel_io_err = 0u;
    sel_load_idx = need_slot_scan = commit_kind = sel_save_now = 0u;
    cfg_fd = native_fd = probe_fd = -1;
    probe_status = probe_ok = probe_done_once = ready_to_load = 0u;
    fen_probe_status = mounted_no_hits = 0u;
    io_done = 0u;
    call_retries = transient_errors = scan_failed = slot_read_ok = 0u;
    new_slot_crc = boot_retries = 0u;
    memset(cfg_slots, 0, sizeof(cfg_slots));
    memset(&cfg_active, 0, sizeof(cfg_active));
    memset(&cfg_save_buf, 0, sizeof(cfg_save_buf));
    memset(cfg_valid, 0, sizeof(cfg_valid));
    memset(sel_slots, 0, sizeof(sel_slots));
    memset(&sel_active, 0, sizeof(sel_active));
    memset(&sel_save_buf, 0, sizeof(sel_save_buf));
    memset(sel_valid, 0, sizeof(sel_valid));
    memset(io_path, 0, sizeof(io_path));
    memset(io_xfer, 0, sizeof(io_xfer));
}

void slotcard_host_format_snapshot(
    uint32_t p_lang, uint32_t p_state, uint32_t p_active_drive, uint32_t p_next_drive,
    uint32_t p_missing_reason, uint32_t p_slots_unlocked, uint32_t p_slots_active,
    uint32_t p_limit, uint32_t p_next_limit, uint32_t p_cfg_save_err,
    uint32_t p_used_count, uint32_t p_bad_slots, uint32_t p_head, uint32_t p_commits,
    char out_labels[5][FX_UI_TEXT]) {
    ui_lang = p_lang;
    state = p_state;
    active_drive = p_active_drive;
    next_drive = p_next_drive;
    missing_reason = p_missing_reason;
    slots_unlocked = p_slots_unlocked;
    slots_active = p_slots_active;
    limit = p_limit;
    store.next_limit = p_next_limit;
    cfg_save_err = p_cfg_save_err;
    used_count = p_used_count;
    store.bad_slots = p_bad_slots;
    store.head = p_head;
    commits = p_commits;
    set_text();
    t_add(out_labels[0], 0u, page_title());
    t_add(out_labels[1], 0u, text[R_STATE]);
    t_add(out_labels[2], 0u, text[R_STORE]);
    t_add(out_labels[3], 0u, text[R_LIMIT]);
    t_add(out_labels[4], 0u, text[R_DRIVE]);
}
void slotcard_host_build_ctrl(uint32_t id, FxUiControl *out) {
    if (out) {
        build_ctrl_desc(id, out);
    }
}
void slotcard_host_get_base_crc(uint32_t out[SC_SLOTS]) {
    for (uint32_t k = 0u; k < SC_SLOTS; ++k) {
        out[k] = base_crc[k];
    }
}
void slotcard_host_get_sel_stats(uint32_t *out_saves, uint32_t *out_seq,
                                 uint32_t *out_slot, uint32_t *out_dirty) {
    if (out_saves) *out_saves = sel_saves;
    if (out_seq) *out_seq = sel_seq;
    if (out_slot) *out_slot = sel_slot;
    if (out_dirty) *out_dirty = sel_dirty;
}
uint32_t slotcard_host_get_transient_errors(void) {
    return transient_errors;
}
#endif
