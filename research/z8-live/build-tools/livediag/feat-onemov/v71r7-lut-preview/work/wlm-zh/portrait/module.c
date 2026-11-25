/* SPDX-License-Identifier: MIT
 * portrait.wlm (V71.7-r4): Portrait Video switch + Debug Settings sub-page.
 *
 * - Preserves exact existing slots.state ops 13 (PORTRAIT_SET) & 14 (PORTRAIT_GET,
 *   magic 'PRT1') for the EL1 portrait-video gate.
 * - Queries slots.state op 15 (SLOTS_LANG_GET, magic 'LANG') for live EN / zh-CN / zh-TW
 *   localization (8 = zh-CN, 9 = zh-TW, else English).
 * - 5 inline controls (<= FX_UI_CONTROLS 16):
 *     1: PAGE_ROOT      - Portrait Video / 竖屏录像 / 直拍錄影
 *     2: ACTION (p=1)   - Portrait Video (On/Off; OK toggles until restart)
 *     3: PAGE   (p=1)   - Debug Settings / 调试设置 / 調試設定
 *     4: ACTION (p=3)   - Runtime Status (EL1 Gate:<0|1> slots.state:OK)
 *     5: ACTION (p=3)   - Note (Resets on restart / 重启后恢复关闭 / 重啟後恢復關閉)
 * - A2 Hardening: never returns non-zero on transient IPC/UI errors (never FAULT(5)).
 * - Freestanding C11, zero libc dependencies, zero pointer relocations, zero FP/SIMD.
 */
#include "fenestra_sdk.h"

enum {
    PAGE_ROOT = 1u,
    R_TOGGLE  = 2u,
    R_DEBUG   = 3u,
    R_STATUS  = 4u,
    R_NOTE    = 5u,
    CONTROLS  = R_NOTE
};

_Static_assert(CONTROLS <= FX_UI_CONTROLS,
               "portrait.wlm controls must fit within inline FX_UI_CONTROLS");

enum {
    PORTRAIT_SET   = 13u,
    PORTRAIT_GET   = 14u,
    SLOTS_LANG_GET = 15u
};

#define PORTRAIT_MAGIC UINT32_C(0x50525431) /* 'PRT1' */
#define LANG_MAGIC     UINT32_C(0x4c414e47) /* 'LANG' */
#define LANG_ZH_CN     8u
#define LANG_ZH_TW     9u
#define LANG_POLL_MS   1000u
#define PT_UI_MAX      (FX_UI_TEXT - 1u)

typedef struct {
    uint32_t index, offset, bytes, value;
} SlotsRequest;

static FxSdkClient client;
static FxServiceReply reply;
static uint64_t slots, last_lang_ms;
static uint32_t pc, row, want, gate, fault, has_slots, ui_caps, ui_lang = 1u;
static uint32_t call_retries, boot_retries, transient_errors;
static uint32_t refresh_status;
static char label_buf[CONTROLS + 1u][FX_UI_TEXT];
static char text_buf[CONTROLS + 1u][FX_UI_TEXT];

/* A2-hardened coroutine CALL: retries transient IPC submit/take errors up to 3x,
 * then surfaces error in reply.result without ever returning non-zero (FAULT(5)). */
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

#define YIELD_TICK() YIELD_TICK_(__COUNTER__)
#define YIELD_TICK_(n)                                                           \
    do {                                                                         \
        pc = 2u * (n) + 1u;                                                      \
        return 0;                                                                \
    case 2u * (n) + 1u:;                                                         \
    } while (0)

static uint32_t t_add(char *o, uint32_t at, const char *s) {
    while (*s && at < PT_UI_MAX) {
        uint8_t ch = (uint8_t)*s;
        uint32_t clen = 1u;
        if ((ch & 0xe0u) == 0xc0u) clen = 2u;
        else if ((ch & 0xf0u) == 0xe0u) clen = 3u;
        else if ((ch & 0xf8u) == 0xf0u) clen = 4u;
        if (at + clen > PT_UI_MAX) break;
        for (uint32_t k = 0u; k < clen && *s; ++k) {
            o[at++] = *s++;
        }
    }
    o[at] = '\0';
    return at;
}

static const char *pt_tr(const char *en, const char *sc, const char *tc) {
    if (ui_lang == LANG_ZH_CN) return sc;
    if (ui_lang == LANG_ZH_TW) return tc;
    return en;
}

static void set_text(void) {
    t_add(label_buf[PAGE_ROOT], 0u,
          pt_tr("Portrait Video",
                "\xe7\xab\x96\xe5\xb1\x8f\xe5\xbd\x95\xe5\x83\x8f",   /* 竖屏录像 */
                "\xe7\x9b\xb4\xe6\x8b\x8d\xe9\x8c\x84\xe5\xbd\xb1")); /* 直拍錄影 */
    text_buf[PAGE_ROOT][0] = '\0';

    t_add(label_buf[R_TOGGLE], 0u,
          pt_tr("Portrait Video",
                "\xe7\xab\x96\xe5\xb1\x8f\xe5\xbd\x95\xe5\x83\x8f",   /* 竖屏录像 */
                "\xe7\x9b\xb4\xe6\x8b\x8d\xe9\x8c\x84\xe5\xbd\xb1")); /* 直拍錄影 */
    if (fault) {
        t_add(text_buf[R_TOGGLE], 0u,
              pt_tr("Unavailable",
                    "\xe4\xb8\x8d\xe5\x8f\xaf\xe7\x94\xa8",   /* 不可用 */
                    "\xe4\xb8\x8d\xe5\x8f\xaf\xe7\x94\xa8")); /* 不可用 */
    } else if (gate) {
        t_add(text_buf[R_TOGGLE], 0u,
              pt_tr("On",
                    "\xe5\xbc\x80\xe5\x90\xaf",   /* 开启 */
                    "\xe9\x96\x8b\xe5\x95\x9f")); /* 開啟 */
    } else {
        t_add(text_buf[R_TOGGLE], 0u,
              pt_tr("Off",
                    "\xe5\x85\xb3\xe9\x97\xad",   /* 关闭 */
                    "\xe9\x97\x9c\xe9\x96\x89")); /* 關閉 */
    }

    t_add(label_buf[R_DEBUG], 0u,
          pt_tr("Debug Settings",
                "\xe8\xb0\x83\xe8\xaf\x95\xe8\xae\xbe\xe7\xbd\xae",   /* 调试设置 */
                "\xe8\xaa\xbf\xe8\xa9\xa6\xe8\xa8\xad\xe5\xae\x9a")); /* 調試設定 */
    text_buf[R_DEBUG][0] = '\0';

    t_add(label_buf[R_STATUS], 0u,
          pt_tr("Runtime Status",
                "\xe8\xbf\x90\xe8\xa1\x8c\xe7\x8a\xb6\xe6\x80\x81",   /* 运行状态 */
                "\xe9\x81\x8b\xe8\xa1\x8c\xe7\x8b\x80\xe6\x85\x8b")); /* 運行狀態 */
    if (!has_slots) {
        t_add(text_buf[R_STATUS], 0u, "EL1 Gate:- slots.state:ERR");
    } else if (fault) {
        t_add(text_buf[R_STATUS], 0u, "EL1 Gate:ERR slots.state:OK");
    } else if (gate) {
        t_add(text_buf[R_STATUS], 0u, "EL1 Gate:1 slots.state:OK");
    } else {
        t_add(text_buf[R_STATUS], 0u, "EL1 Gate:0 slots.state:OK");
    }

    t_add(label_buf[R_NOTE], 0u,
          pt_tr("Note",
                "\xe8\xaf\xb4\xe6\x98\x8e",   /* 说明 */
                "\xe8\xaa\xaa\xe6\x98\x8e")); /* 說明 */
    t_add(text_buf[R_NOTE], 0u,
          pt_tr("Resets on restart",
                "\xe9\x87\x8d\xe5\x90\xaf\xe5\x90\x8e\xe6\x81\xa2\xe5\xa4\x8d\xe5\x85\xb3\xe9\x97\xad",   /* 重启后恢复关闭 */
                "\xe9\x87\x8d\xe5\x95\x9f\xe5\xbe\x8c\xe6\x81\xa2\xe5\xbe\xa9\xe9\x97\x9c\xe9\x96\x89")); /* 重啟後恢復關閉 */
}

static FxResult put_ctrl(const FxServiceModule *api, uint32_t id) {
    FxUiControl c;
    uint8_t *raw = (uint8_t *)&c;
    for (uint32_t k = 0u; k < (uint32_t)sizeof(c); ++k) {
        raw[k] = 0u;
    }
    c.abi = FX_PLATFORM_ABI;
    c.bytes = (uint32_t)sizeof(c);
    c.id = id;
    c.flags = FX_UI_VISIBLE | FX_UI_ENABLED;

    switch (id) {
    case PAGE_ROOT:
        c.parent = 0u;
        c.kind = FX_UI_PAGE;
        if (ui_caps & FX_UI_CAP_ROOT) {
            c.flags |= FX_UI_ROOT;
        }
        break;
    case R_TOGGLE:
        c.parent = PAGE_ROOT;
        c.kind = FX_UI_ACTION;
        break;
    case R_DEBUG:
        c.parent = PAGE_ROOT;
        c.kind = FX_UI_PAGE;
        break;
    case R_STATUS:
    case R_NOTE:
        c.parent = R_DEBUG;
        c.kind = FX_UI_ACTION;
        break;
    default:
        return FX_ARGUMENT;
    }
    t_add(c.label, 0u, label_buf[id]);
    t_add(c.text, 0u, text_buf[id]);
    return fx_sdk_ui(&client, api, &c);
}

static FxResult portrait_call(const FxServiceModule *api, uint32_t op, uint32_t value) {
    SlotsRequest q = { 0u, 0u, 0u, value };
    return fx_sdk_platform(&client, api, FX_BIND_CALL, slots, op, 0u, &q, (uint32_t)sizeof(q));
}

int fenestra_module_entry(const FxModuleServices *base) {
    if (!base || base->bytes < sizeof(FxServiceModule)) {
        return 1;
    }
    const FxServiceModule *api = (const FxServiceModule *)base;
    if (api->service_abi != FX_SERVICE_ABI || !api->exchange) {
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
        ui_caps = 0u;
        has_slots = 0u;
        fault = 0u;
        gate = 0u;
        want = 2u;
        refresh_status = 0u;
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

        for (boot_retries = 0u; boot_retries < 4u; ++boot_retries) {
            CALL(fx_sdk_open(&client, api, "slots.state", 1u));
            if (reply.result == FX_OK) {
                has_slots = 1u;
                slots = reply.value[0];
                break;
            }
            ++transient_errors;
            YIELD_TICK();
        }

        if (!has_slots) {
            fault = 1u;
            set_text();
            for (row = PAGE_ROOT; row <= CONTROLS; ++row) {
                for (boot_retries = 0u; boot_retries < 4u; ++boot_retries) {
                    CALL(put_ctrl(api, row));
                    if (reply.result == FX_OK) break;
                    YIELD_TICK();
                }
            }
            for (;;) {
                CALL(fx_sdk_events(&client, api));
            }
        }

        CALL(portrait_call(api, SLOTS_LANG_GET, 0u));
        if (reply.result == FX_OK && reply.value[1] == LANG_MAGIC) {
            ui_lang = (uint32_t)reply.value[0];
        }
        last_lang_ms = api->time_ms;

        set_text();
        for (row = PAGE_ROOT; row <= CONTROLS; ++row) {
            for (boot_retries = 0u; boot_retries < 4u; ++boot_retries) {
                CALL(put_ctrl(api, row));
                if (reply.result == FX_OK) break;
                ++transient_errors;
                YIELD_TICK();
            }
        }

        want = 1u;
        for (;;) {
            if (want != 2u) {
                CALL(portrait_call(api, PORTRAIT_SET, want));
                fault = (reply.result != FX_OK) ? 1u : 0u;
                CALL(portrait_call(api, PORTRAIT_GET, 0u));
                /* A base without the gate answers FX_ARGUMENT: report unavailable cleanly. */
                if (reply.result != FX_OK || reply.value[1] != PORTRAIT_MAGIC) {
                    fault = 1u;
                } else {
                    gate = (uint32_t)reply.value[0];
                }
                want = 2u;
                set_text();
                CALL(put_ctrl(api, R_TOGGLE));
                CALL(put_ctrl(api, R_STATUS));
            } else if (refresh_status) {
                refresh_status = 0u;
                CALL(portrait_call(api, PORTRAIT_GET, 0u));
                if (reply.result != FX_OK || reply.value[1] != PORTRAIT_MAGIC) {
                    fault = 1u;
                } else {
                    fault = 0u;
                    gate = (uint32_t)reply.value[0];
                }
                set_text();
                CALL(put_ctrl(api, R_TOGGLE));
                CALL(put_ctrl(api, R_STATUS));
            }

            CALL(fx_sdk_events(&client, api));
            if (reply.result == FX_OK && reply.bytes == sizeof(FxUiEvent)) {
                FxUiEvent e;
                uint8_t *p = (uint8_t *)&e;
                for (uint32_t k = 0u; k < (uint32_t)sizeof(e); ++k) {
                    p[k] = reply.data[k];
                }
                if (e.event == FX_INPUT_ACTIVATE && e.control == R_TOGGLE && !fault) {
                    want = gate ? 0u : 1u;
                } else if (e.event == FX_INPUT_ACTIVATE && e.control == R_STATUS) {
                    refresh_status = 1u;
                } else if (e.event == FX_INPUT_FOCUS) {
                    refresh_status = 1u;
                }
            }

            if (api->time_ms >= last_lang_ms && (api->time_ms - last_lang_ms) >= LANG_POLL_MS) {
                last_lang_ms = api->time_ms;
                CALL(portrait_call(api, SLOTS_LANG_GET, 0u));
                if (reply.result == FX_OK && reply.value[1] == LANG_MAGIC &&
                    (uint32_t)reply.value[0] != ui_lang) {
                    ui_lang = (uint32_t)reply.value[0];
                    set_text();
                    for (row = PAGE_ROOT; row <= CONTROLS; ++row) {
                        CALL(put_ctrl(api, row));
                    }
                }
            }
        }
    default:
        ++transient_errors;
        pc = 0u;
        return 0;
    }
}

#ifdef PORTRAIT_HOST_TEST
#include <string.h>
void portrait_host_reset(void) {
    memset(&client, 0, sizeof(client));
    memset(&reply, 0, sizeof(reply));
    slots = last_lang_ms = 0u;
    pc = row = want = gate = fault = has_slots = ui_caps = 0u;
    ui_lang = 1u;
    call_retries = boot_retries = transient_errors = refresh_status = 0u;
    memset(label_buf, 0, sizeof(label_buf));
    memset(text_buf, 0, sizeof(text_buf));
}
uint32_t portrait_host_get_transient_errors(void) {
    return transient_errors;
}
#endif
