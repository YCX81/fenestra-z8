/* V68/V69 Nikon Z8 C3.11 runtime overlay (linked into RX cave 0x401fef00..0x401fff80).
 * Pure read-only code & const tables; ZERO writable .data/.bss (uses 32 B of
 * boot-cleared slot-reservation RAM at 0xa116d3490 for the 3 cached font faces).
 *
 * Components:
 *   1. v68_set_primary_font (hooks 0x40880d74):
 *        When Slot 0 is any stock CJK face (Font[0,1,2,3,13,14,15,22,23]:
 *        asc=1760, desc=288, adv0=2048), binds:
 *          Slot 1 = stock Font[18] (95/95 printable ASCII + symbols, asc=1760, desc=288)
 *          Slot 2 = CJK_EXT_1 @ 0x408c6000 (GB2312 L1 + CJK punct, asc=1760, desc=288)
 *          Slot 3 = CJK_EXT_2 @ 0x40b8a000 (GB2312 L1 rest + Trad + L2, asc=1760, desc=288)
 *        When Slot 0 is non-CJK (English Font[16], Arabic, Thai, NULL, ...):
 *          Clears Slot 1..3 = NULL -> 100% stock metrics & rendering.
 *   2. v68_lang_svc (hooks 0x401fe20c in slots.state):
 *        Adds op 15 (SLOTS_LANG_GET): returns current Nikon UI language ID
 *        (*(uint32_t *)0xa02f2ab70) in reply.value[0] and magic 0x4c414e47 ('LANG')
 *        in reply.value[1], status FX_OK (0). Other unknown ops -> 0x401a7898.
 *   3. v68_menu_text (hooks 0x4032e130):
 *        For language != 8 && language != 9: calls dynamically resolved ORIG_TEXT_VA.
 *        For language == 8 (zh-CN) or 9 (zh-TW): calls ORIG_TEXT_VA first, then
 *        translates Live Photo, Shutter sound, Live playback key, Fenestra core UI,
 *        and WonderLantern module titles ("portrait", "slotcard", "filmsim",
 *        "Film Simulation") into Chinese.
 */
#include <stdint.h>

#define FONT18_ADDR   UINT64_C(0x42dbf97d)
#define FONT18_SIZE   10591u
#define CJK1_ADDR     UINT64_C(0x408c6000)
#define CJK2_ADDR     UINT64_C(0x40b8a000)
#define FONT_ROM_LO   UINT64_C(0x42b47218)
#define FONT_ROM_HI   UINT64_C(0x42dec310)
#define FONT_CACHE_VA UINT64_C(0xa116d3490)
#define OPEN_FONT_VA  UINT64_C(0x40881888)

#ifndef ORIG_TEXT_VA
#error "ORIG_TEXT_VA must be defined at compile time from 0x4032e130"
#endif
#ifndef CJK1_SIZE
#error "CJK1_SIZE must be defined at compile time"
#endif
#ifndef CJK2_SIZE
#error "CJK2_SIZE must be defined at compile time"
#endif

typedef struct {
    uint64_t engine;
    uint64_t f18;
    uint64_t cjk1;
    uint64_t cjk2;
} V68FontCache;

typedef uint64_t (*OpenFontFn)(uint64_t engine, uint32_t type, uint64_t data, uint32_t size);
typedef void (*MenuTextFn)(int32_t language, int32_t id, const uint16_t **text, uint32_t *bytes);

static int is_stock_cjk_face(uint64_t face) {
    if (!face) return 0;
    uint64_t rom = *(const volatile uint64_t *)(uintptr_t)(face + 24u);
    if (rom < FONT_ROM_LO || rom >= FONT_ROM_HI) return 0;
    uint32_t asc_desc = *(const uint32_t *)(uintptr_t)(rom + 8u);
    uint16_t adv0 = *(const uint16_t *)(uintptr_t)(rom + 24u);
    return asc_desc == 0x012006e0u && adv0 == 2048u;
}

uint32_t v68_set_primary_font(uint64_t engine, uint64_t face) {
    if (!engine) return 5u;

    uint64_t s1 = 0u, s2 = 0u, s3 = 0u;
    if (is_stock_cjk_face(face)) {
        volatile V68FontCache *c = (volatile V68FontCache *)(uintptr_t)FONT_CACHE_VA;
        if (c->engine != engine) {
            c->f18 = 0u;
            c->cjk1 = 0u;
            c->cjk2 = 0u;
            c->engine = engine;
        }
        OpenFontFn open_font = (OpenFontFn)(uintptr_t)OPEN_FONT_VA;
        if (!c->f18)  c->f18  = open_font(engine, 0u, FONT18_ADDR, FONT18_SIZE);
        if (!c->cjk1) c->cjk1 = open_font(engine, 0u, CJK1_ADDR, (uint32_t)CJK1_SIZE);
        if (!c->cjk2) c->cjk2 = open_font(engine, 0u, CJK2_ADDR, (uint32_t)CJK2_SIZE);
        s1 = c->f18;
        s2 = c->cjk1;
        s3 = c->cjk2;
    }

    volatile uint64_t *slots = (volatile uint64_t *)(uintptr_t)(engine + 232u);
    if (slots[0] == face && slots[1] == s1 && slots[2] == s2 && slots[3] == s3) {
        return 0u;
    }
    slots[0] = face;
    slots[1] = s1;
    slots[2] = s2;
    slots[3] = s3;
    *(volatile uint32_t *)(uintptr_t)(engine + 18640u) = 1u;
    *(volatile int32_t  *)(uintptr_t)(engine + 224u)   = -1;
    return 0u;
}

/* Naked asm handler branched to from 0x401fe20c (b.ne v68_lang_svc).
 * In: x8 = op, w0 = FX_ARGUMENT(1), x19 = reply ptr, x22 = request ptr.
 * Out: op 15 -> SLOTS_LANG_GET; ops 32..79 -> filmsim_slots_svc_sym (x9 = op - 32:
 *      32..47 FilmSim, 48..63 LivePhoto, 64..79 3D LUT);
 *      other unknown ops -> SVC_RET (0x401a7898) with w0 = FX_ARGUMENT(1). */
__attribute__((naked)) void v68_lang_svc(void) {
    __asm__ volatile(
        "cmp x8, #15\n"
        "b.ne 1f\n"
        "movz x9, #0xab70\n"
        "movk x9, #0x02f2, lsl #16\n"
        "movk x9, #0x000a, lsl #32\n"
        "ldr w8, [x9]\n"
        "str x8, [x19, #0x10]\n"
        "movz x8, #0x4e47\n"
        "movk x8, #0x4c41, lsl #16\n"
        "str x8, [x19, #0x18]\n"
        "mov w0, #0\n"
        "2:\n"
        "b svc_ret_sym\n"
        "1:\n"
        "sub x9, x8, #32\n"
        "cmp x9, #48\n"
        "b.hs 2b\n"
        "b filmsim_slots_svc_sym\n"
    );
}

typedef struct {
    const char     *en;
    const uint16_t *sc;
    const uint16_t *tc;
    uint16_t        en_chars;
    uint16_t        sc_bytes;
    uint16_t        tc_bytes;
} TransRow;

#define TR(en_s, sc_s, tc_s) \
    { en_s, u##sc_s, u##tc_s, \
      (uint16_t)(sizeof(en_s) - 1u), \
      (uint16_t)(sizeof(u##sc_s) - 2u), \
      (uint16_t)(sizeof(u##tc_s) - 2u) }

static const TransRow k_translations[] = {
    /* Shared On / Off choices (3168, 3169) */
    TR("On",                      "开启",               "開啟"),
    TR("Off",                     "关闭",               "關閉"),
    /* Fenestra core UI (3154..3179) */
    TR("WonderLantern",           "插件管理",           "插件管理"),
    TR("Activate",                "激活",               "激活"),
    TR("Disabled due to abnormal shutdown", "异常关机已禁用", "異常關機已停用"),
    TR("Status",                  "状态",               "狀態"),
    TR("Back",                    "返回",               "返回"),
    TR("No loaded modules",       "未加载插件",         "未載入插件"),
    TR("Modules",                 "已加载插件",         "已載入插件"),
    TR("Auto activate",           "开机自动激活",       "開機自動激活"),
    TR("Shutdown guard",          "异常关机保护",       "異常關機保護"),
    TR("Clear abnormal flag",     "清除异常标记",       "清除異常標記"),
    TR("Cancel",                  "取消",               "取消"),
    TR("Normal",                  "正常",               "正常"),
    TR("Abnormal",                "异常",               "異常"),
    TR("USB Debug",               "USB 调试",           "USB 調試"),
    TR("Previous",                "上一页",             "上一頁"),
    TR("Next",                    "下一页",             "下一頁"),
    TR("Next page",               "下一页",             "下一頁"),
    TR("UI resources unavailable","界面资源不可用",     "介面資源不可用"),
    TR("Retry",                   "重试",               "重試"),
    TR("Checking card",           "检测存储卡",         "檢測存儲卡"),
    TR("Card error",              "存储卡错误",         "存儲卡錯誤"),
    TR("Abnormal shutdown",       "异常关机",           "異常關機"),
    TR("Ready",                   "就绪",               "就緒"),
    TR("Loading",                 "加载中",             "載入中"),
    TR("Active",                  "已激活",             "已激活"),
    /* WonderLantern Apps list & root page module titles (id >= 3254) */
    TR("portrait",                "竖屏录像",           "直拍錄影"),
    TR("slotcard",                "C64 滤镜槽",         "C64 濾鏡槽"),
    TR("filmsim",                 "胶片模拟 / HDF",     "膠片模擬 / HDF"),
    TR("Film Simulation",         "胶片模拟 / HDF",     "膠片模擬 / HDF"),
    TR("livephoto",               "实况照片",           "實況照片"),
    TR("Live Photo",              "实况照片",           "實況照片"),
    TR("lut",                     "3D LUT 调色",        "3D LUT 調色"),
    TR("3D LUT",                  "3D LUT 调色",        "3D LUT 調色"),
};

static int match_utf16_ascii(const uint16_t *u16, uint32_t bytes, const char *ascii, uint32_t n) {
    if (bytes != n * 2u) return 0;
    for (uint32_t i = 0u; i < n; ++i) {
        if (u16[i] != (uint16_t)(uint8_t)ascii[i]) return 0;
    }
    return 1;
}

static const uint16_t k_plugin_mgr_en[] = u"Plugin Manager";

/* NFP Anamorphic (3154..3160) & H.265 (3161..3179) share string IDs with
 * Fenestra Core / Live Photo (3163, 3177) / Live playback key (3160..3162).
 * When the active menu page (via stock 0x403dbdb4) is NOT a Fenestra/WL
 * extension page [0x7000, 0x9500) (and for 3154/3155 is specifically Video
 * Menu 0x00FE or Anamorphic submenu 0x00B6, preserving Setup Menu 0x0297),
 * resolve directly from Nikon's native language tables (exact 0x4032e134 logic)
 * so NFP menus show their own localized text instead of Fenestra/Live labels. */
__attribute__((noinline, minsize))
static int v68_nfp_overlap_text(int32_t language, int32_t id,
                                const uint16_t **text, uint32_t *bytes) {
    const uint32_t *cur = ((const uint32_t *(*)(void))(uintptr_t)UINT64_C(0x403dbdb4))();
    if (!cur) return 0;
    uint32_t w = *cur;
    uint32_t group = w & 0xffffu;
    uint32_t page  = w >> 16;
    if (group == 2u && (page - 0x7000u) < 0x2500u) return 0;
    if (id <= 3155 && !(group == 2u && (page == 0x00b6u || page == 0x00feu))) return 0;
    if ((uint32_t)language >= 36u) return 0;
    const volatile uintptr_t *dir = (const volatile uintptr_t *)(uintptr_t)UINT64_C(0xa02f2ac38);
    const uint8_t *const *blobs   = (const uint8_t *const *)dir[0];
    const int32_t *const *tables  = (const int32_t *const *)dir[2];
    if (!tables || !blobs) return 0;
    const int32_t *t = tables[language];
    const uint8_t *b = blobs[language];
    if (!t || !b) return 0;
    uint32_t count = *(const volatile uint32_t *)(uintptr_t)UINT64_C(0xa0281cd5c);
    int32_t idx = ((uint32_t)id < count) ? id : 0;
    int32_t start = t[idx];
    int32_t end   = t[idx + 1];
    *text  = (const uint16_t *)(const void *)(b + (intptr_t)start);
    *bytes = (uint32_t)(end - start);
    return 1;
}

__attribute__((minsize))
void v68_menu_text(int32_t language, int32_t id, const uint16_t **text, uint32_t *bytes) {
    if (text && bytes && (uint32_t)(id - 3154) <= 25u &&
        v68_nfp_overlap_text(language, id, text, bytes)) {
        return;
    }
    MenuTextFn orig = (MenuTextFn)(uintptr_t)ORIG_TEXT_VA;
    orig(language, id, text, bytes);
    if (!text || !bytes || !*text || !*bytes) return;
    if (language != 8 && language != 9) {
        if (id == 3155 && match_utf16_ascii(*text, *bytes, "WonderLantern", 13u)) {
            *text = k_plugin_mgr_en;
            *bytes = (uint32_t)(sizeof(k_plugin_mgr_en) - 2u);
        }
        return;
    }
    /* Only translate Fenestra / Live / Playback / Module IDs (>= 3154).
     * Stock Nikon IDs (< 3154) and NFP IDs (3137..3153, 3180..3249) stay untouched! */
    if (id < 3154 || (id >= 3180 && id < 3254)) return;

    const uint16_t *cur = *text;
    uint32_t cur_bytes = *bytes;
    uint32_t count = (uint32_t)(sizeof(k_translations) / sizeof(k_translations[0]));
    for (uint32_t i = 0u; i < count; ++i) {
        if (match_utf16_ascii(cur, cur_bytes, k_translations[i].en, k_translations[i].en_chars)) {
            if (language == 8) {
                *text = k_translations[i].sc;
                *bytes = k_translations[i].sc_bytes;
            } else {
                *text = k_translations[i].tc;
                *bytes = k_translations[i].tc_bytes;
            }
            return;
        }
    }
}
