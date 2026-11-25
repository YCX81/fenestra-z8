/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Native Nikon Photo Shooting Menu UI Controller & Multilingual Strings.
 *
 * ZERO-GLOBAL, STRICT SCALAR (-mgeneral-regs-only).
 *   - Multilingual UTF-16LE strings (EN, zh-CN, zh-TW, JA) for text IDs 0x1F40..0x1F4A
 *     plus submenu status & 10 built-in LUT category labels.
 *   - v71r7_photo_menu_resolve_text() & v71r7_photo_menu_lookup_text() called cleanly
 *     from live_ui_menu_text (ZERO extra ROM text hooks at 0x4032B5DC/0x4032B72C).
 *   - 23-row paginated submenu controller for Page 0x5D6 (rows 0x4800..0x4816),
 *     LUT-only (BURST 0x5D5 excluded), with category folder browsing & back-to-focus.
 */
#include "photo_menu.h"

#define FN(address, type) ((type)(uintptr_t)UINT64_C(address))
#define ROM_CURRENT_PAGE      FN(0x403dbdb4, void *(*)(void))
#define ROM_FIND_WIDGET       FN(0x4039e810, void *(*)(uint32_t, void *))
#define ROM_FIND_CHILD        FN(0x4050a6f0, void *(*)(void *, uint32_t, uint32_t))
#define ROM_DRAW_FENCE        FN(0x402e76d4, void (*)(void))
#define ROM_DIRTY             FN(0x403f62b4, void (*)(void *, uint32_t))
#define ROM_DYNAMIC           FN(0x403f621c, void (*)(void *, uint32_t))
#define ROM_SET_TEXT          FN(0x403f2920, void (*)(const char *, uint32_t, uint32_t))
#define ROM_SET_WIDE          FN(0x403f2a58, void (*)(const uint16_t *, uint32_t, uint32_t))
#define ROM_BIND_TEXT         FN(0x4031e178, int (*)(void *, uint32_t, uint32_t))
#define ROM_LANGUAGE          FN(0x4032db54, uint32_t (*)(void))
#define ROM_TEXT_FONT         FN(0x4032e19c, uint32_t (*)(uint32_t, uint32_t))
#define ROM_ROW_COUNT         FN(0x403afed4, uint32_t (*)(void *))
#define ROM_ROW_DISABLE       FN(0x403b1ed0, void (*)(void *, uint32_t, uint32_t))
#define ROM_ROW_FOCUS         FN(0x403aff78, void (*)(void *, uint32_t, uint32_t))
#define ROM_ROW_DRAW          UINT64_C(0x4031f060)
#define ROM_LIST_LAYOUT       FN(0x403ae874, void (*)(void *))
#define ROM_VIEW_LAYOUT       FN(0x403aeb38, void (*)(void *))
#define ROM_SCROLLBAR         FN(0x403aec24, void (*)(void *))
#define ROM_RECT              FN(0x4031e2a8, void (*)(void *, const int16_t *))
#define ROM_VIEW_ID           FN(0x403f0db8, uint32_t (*)(uint32_t))
#define ROM_VIEW_SET          FN(0x4039ecc4, void (*)(uint32_t, const int16_t *))
#define ROM_VIEW_RECT         FN(0x4039ed6c, void (*)(uint32_t, int16_t *))
#define ROM_MARK              FN(0x4039e848, void (*)(void *, uintptr_t))
#define ROM_REDRAW            FN(0x403efed8, void (*)(uintptr_t, uint32_t, uint32_t))
#define ROM_TIMER_START       FN(0x403d7084, int (*)(uint32_t, uint32_t, void (*)(void)))
#define ROM_TIMER_CANCEL      FN(0x403d7144, void (*)(uint32_t))
#define ROM_STOCK_BACK_ACTION FN(0x403b1ba4, void (*)(void *))

enum {
    TIMER_SLOT   = 0x1au,
    TITLE_STATUS = 0x1405u,
    VALUE_CHILD  = 0x4ba7u,
    TEXT_BASE    = 0xfcu,
    FONT_FILE    = 0x207u,
    FONT_WIDE    = 0x21fu,
    UI_LEFT      = 0x285u,
    UI_TOUCH_BACK = 0x2a0u,
    UI_STAY      = 0x68cu
};

/* ============================================================================
 * 1. Multilingual UTF-16LE String Tables (100% pure .rodata, Stage 3 CJK audited)
 * ============================================================================ */
typedef struct {
    const uint16_t *en;
    const uint16_t *sc;
    const uint16_t *tc;
    const uint16_t *ja;
} Labels4;

#define WIDE(text) ((const uint16_t *)u##text)
#define LABEL4(en_s, sc_s, tc_s, ja_s) \
    { WIDE(en_s), WIDE(sc_s), WIDE(tc_s), WIDE(ja_s) }

static const Labels4 k_labels[] = {
    /* 0x1F40 (reserved) */
    LABEL4("Continuous shooting speed",
           "连拍速度",
           "連拍速度",
           "連続撮影速度"),
    /* 0x1F41 (reserved) */
    LABEL4("Set C30 and C120 shooting speeds.",
           "设置 C30 和 C120 的连拍速度。",
           "設定 C30 與 C120 的連拍速度。",
           "C30/C120 の連続撮影速度を設定します。"),
    /* 0x1F42 (Photo Shooting Menu Row 47 title + Page 0x5D6 header) */
    LABEL4("Select LUT",
           "选择 LUT",
           "選擇 LUT",
           "LUT 選択"),
    /* 0x1F43 (Photo Shooting Menu Row 47 help text) */
    LABEL4("Choose a LUT for the rear display and JPEG photos.",
           "选择用于显示屏和 JPEG 照片的 LUT。",
           "選擇用於顯示螢幕與 JPEG 照片的 LUT。",
           "モニター表示と JPEG 画像に適用する LUT を選びます。"),
    /* 0x1F44 (reserved) */
    LABEL4("Continuous shooting speed",
           "连拍速度",
           "連拍速度",
           "連続撮影速度"),
    /* 0x1F45 (reserved) */
    LABEL4("C30 shooting speed",
           "C30 连拍速度",
           "C30 連拍速度",
           "C30 連続撮影速度"),
    /* 0x1F46 (reserved) */
    LABEL4("C120 shooting speed",
           "C120 连拍速度",
           "C120 連拍速度",
           "C120 連続撮影速度"),
    /* 0x1F47 (Page 0x5D6 Row 0: clear / Off) */
    LABEL4("Off",
           "无LUT",
           "無LUT",
           "OFF"),
    /* 0x1F48 (Page 0x5D6 Row 21: Previous page) */
    LABEL4("Prev",
           "上一页",
           "上一頁",
           "前へ"),
    /* 0x1F49 (Page 0x5D6 Row 22: Next page) */
    LABEL4("Next",
           "下一页",
           "下一頁",
           "次へ"),
    /* 0x1F4A (Dynamic placeholder / empty) */
    LABEL4("", "", "", "")
};
_Static_assert(sizeof(k_labels) / sizeof(k_labels[0]) == PHOTO_TEXT_END - PHOTO_FIRST_TEXT,
               "published text ID count");

static const Labels4 k_back_label =
    LABEL4("Back", "返回上一级", "返回上一層", "戻る");
static const Labels4 k_flat_label =
    LABEL4("Root LUTs", "根目录 LUT", "根目錄 LUT", "ルート LUT");

static const struct {
    const char *category;
    Labels4     label;
} k_categories[] = {
    { "01-Fuji-Film",        LABEL4("01 Fuji Film",        "FUJI 色调",          "FUJI 色調",          "01 Fuji Film") },
    { "02-Kodak-Negative",   LABEL4("02 Kodak Negative",   "Kodak 负片",         "Kodak 負片",         "02 Kodak Negative") },
    { "03-Classic-Negative", LABEL4("03 Classic Negative", "传统负片",           "傳統負片",           "03 Classic Negative") },
    { "04-Color-Slide",      LABEL4("04 Color Slide",      "彩色反转片",         "彩色反轉片",         "04 Color Slide") },
    { "05-Black-and-White",  LABEL4("05 Black and White",  "黑白片",             "黑白片",             "05 Black and White") },
    { "06-Instant-Polaroid", LABEL4("06 Instant Polaroid", "拍立得即显",         "拍立得即顯",         "06 Instant Polaroid") },
    { "07-Cinema-Print",     LABEL4("07 Cinema Print",     "电影色调",           "電影色調",           "07 Cinema Print") },
    { "08-Camera-Style",     LABEL4("08 Camera Style",     "相机直出与机内风格", "相機直出與機內風格", "08 Camera Style") },
    { "09-Portrait-Daily",   LABEL4("09 Portrait Daily",   "人像与日常拍摄",     "人像與日常拍攝",     "09 Portrait Daily") },
    { "10-Creative-Vintage", LABEL4("10 Creative Vintage", "创意色调",           "創意色調",           "10 Creative Vintage") }
};

static const Labels4 k_statuses[] = {
    LABEL4("No LUT",                   "无LUT",                "無LUT",                "LUT なし"),
    LABEL4("Loading",                  "等待载入",             "等待載入",             "読込待機"),
    LABEL4("Reading",                  "正在读取",             "正在讀取",             "読込中"),
    LABEL4("Preparing",                "正在处理",             "正在處理",             "処理中"),
    LABEL4("Ready",                    "已载入",               "已載入",               "適用中"),
    LABEL4("LUT file missing",         "找不到 LUT 文件",      "找不到 LUT 檔案",      "LUT ファイルなし"),
    LABEL4("LUT load failed",          "LUT 载入失败",         "LUT 載入失敗",         "LUT 読込エラー"),
    LABEL4("Ready / save failed",      "已载入，保存失败",     "已載入，儲存失敗",     "適用済 / 保存エラー"),
    LABEL4("No LUT / save failed",     "无LUT，保存失败",      "無LUT，儲存失敗",      "解除 / 保存エラー"),
    LABEL4("LUT worker failed",        "LUT 启动失败",         "LUT 啟動失敗",         "LUT 起動エラー"),
    LABEL4("LUT settings invalid",     "LUT 设置读取失败",     "LUT 設定讀取失敗",     "LUT 設定エラー"),
    LABEL4("No LUT / closing preview", "无LUT，正在关闭预览",  "無LUT，正在關閉預覽",  "解除中"),
    LABEL4("LUT plugin off",           "LUT 插件未启用",       "LUT 插件未啟用",       "LUT プラグイン無効")
};
static const Labels4 k_status_invalid =
    LABEL4("Invalid LUT file", "LUT 文件无效", "LUT 檔案無效", "無効な LUT");
static const Labels4 k_status_resources =
    LABEL4("Insufficient memory", "内存不足", "記憶體不足", "メモリ不足");
static const Labels4 k_status_close_error =
    LABEL4("Card close failed", "存储卡关闭失败", "記憶卡關閉失敗", "カード終了エラー");

/* Dynamic Row 47 / Page 0x5D6 disabled & master-off labels (lut.wlm gating, Stage 3 audited). */
static const Labels4 k_title_mod_disabled =
    LABEL4("Select LUT (Off)",
           "选择 LUT (未启用)",
           "選擇 LUT (未啟用)",
           "LUT 選択 (無効)");
static const Labels4 k_help_mod_disabled =
    LABEL4("Enable LUT in Setup > Plugin Manager > Modules.",
           "请在 插件管理 > 已加载插件 中启用 LUT 插件。",
           "請在 插件管理 > 已載入插件 中啟用 LUT 插件。",
           "プラグイン管理で LUT を有効にしてください。");
static const Labels4 k_title_master_off =
    LABEL4("Select LUT (Off)",
           "选择 LUT (已关闭)",
           "選擇 LUT (已關閉)",
           "LUT 選択 (OFF)");
static const Labels4 k_help_master_off =
    LABEL4("Turn on 3D LUT in Plugin Manager > 3D LUT.",
           "请在 插件管理 > 3D LUT 调色 中开启 3D LUT。",
           "請在 插件管理 > 3D LUT 調色 中開啟 3D LUT。",
           "プラグイン管理で 3D LUT をオンにしてください。");
static const Labels4 k_status_master_off =
    LABEL4("3D LUT Off",
           "3D LUT 已关闭",
           "3D LUT 已關閉",
           "3D LUT OFF");
static const Labels4 k_row_mod_disabled =
    LABEL4("Enable LUT in Plugin Manager",
           "请在插件管理启用 LUT",
           "請在插件管理啟用 LUT",
           "プラグイン管理で LUT を有効化");
static const Labels4 k_row_master_off =
    LABEL4("Turn on 3D LUT in Plugin Manager",
           "请在插件管理开启 3D LUT",
           "請在插件管理開啟 3D LUT",
           "プラグイン管理で 3D LUT をオン");

/* 0 = enabled, 1 = lut.wlm module disabled/unloaded, 2 = lut.wlm loaded & master_on == 0 */
static uint32_t lut_ui_disabled_mode(void) {
    if (!v71r7_lut_is_module_loaded()) return 1u;
    if (!v71r7_lut_effective_master_on()) return 2u;
    return 0u;
}

static const uint16_t *select_locale(const Labels4 *label, uint32_t language) {
    if (language == PHOTO_LANG_ZH_CN) return label->sc;
    if (language == PHOTO_LANG_ZH_TW) return label->tc;
    if (language == PHOTO_LANG_JA)    return label->ja;
    return label->en;
}

/* ============================================================================
 * 2. Platform Abstraction Wrappers (direct ROM calls on EL1, mockable on Host)
 * ============================================================================ */
static uint32_t ui_get_language(PhotoMenuCtx *ctx) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->get_language)
        return ctx->plat->get_language(ctx->plat->user);
    return PHOTO_LANG_EN;
#else
    (void)ctx;
    return ROM_LANGUAGE();
#endif
}

static void *ui_current_page(PhotoMenuCtx *ctx) {
#ifdef V71R7_HOST_TEST
    return (ctx && ctx->plat && ctx->plat->current_page)
        ? ctx->plat->current_page(ctx->plat->user) : NULL;
#else
    (void)ctx;
    return ROM_CURRENT_PAGE();
#endif
}

static void *ui_find_widget(PhotoMenuCtx *ctx, uint32_t item, void *parent) {
#ifdef V71R7_HOST_TEST
    return (ctx && ctx->plat && ctx->plat->find_widget)
        ? ctx->plat->find_widget(ctx->plat->user, item, parent) : NULL;
#else
    (void)ctx;
    return ROM_FIND_WIDGET(item, parent);
#endif
}

static void *ui_find_child(PhotoMenuCtx *ctx, void *page, uint32_t item, uint32_t child) {
#ifdef V71R7_HOST_TEST
    return (ctx && ctx->plat && ctx->plat->find_child)
        ? ctx->plat->find_child(ctx->plat->user, page, item, child) : NULL;
#else
    (void)ctx;
    return ROM_FIND_CHILD(page, item, child);
#endif
}

static void ui_draw_fence(PhotoMenuCtx *ctx) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->draw_fence) ctx->plat->draw_fence(ctx->plat->user);
#else
    (void)ctx;
    ROM_DRAW_FENCE();
#endif
}

static void ui_dirty(PhotoMenuCtx *ctx, void *node, uint32_t flag) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->dirty) ctx->plat->dirty(ctx->plat->user, node, flag);
#else
    (void)ctx;
    ROM_DIRTY(node, flag);
#endif
}

static void ui_dynamic(PhotoMenuCtx *ctx, void *node, uint32_t flag) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->dynamic_flag) ctx->plat->dynamic_flag(ctx->plat->user, node, flag);
#else
    (void)ctx;
    ROM_DYNAMIC(node, flag);
#endif
}

static void ui_set_text(PhotoMenuCtx *ctx, const char *s, uint32_t slot, uint32_t font) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->set_text) ctx->plat->set_text(ctx->plat->user, s, slot, font);
#else
    (void)ctx;
    ROM_SET_TEXT(s, slot, font);
#endif
}

static void ui_set_wide(PhotoMenuCtx *ctx, const uint16_t *s, uint32_t slot, uint32_t font) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->set_wide) ctx->plat->set_wide(ctx->plat->user, s, slot, font);
#else
    (void)ctx;
    ROM_SET_WIDE(s, slot, font);
#endif
}

static void ui_bind_text(PhotoMenuCtx *ctx, void *node, uint32_t slot, uint32_t kind) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->bind_text) (void)ctx->plat->bind_text(ctx->plat->user, node, slot, kind);
#else
    (void)ctx;
    (void)ROM_BIND_TEXT(node, slot, kind);
#endif
}

static uint32_t ui_text_font(PhotoMenuCtx *ctx, uint32_t language, uint32_t sample_id) {
#ifdef V71R7_HOST_TEST
    return (ctx && ctx->plat && ctx->plat->text_font)
        ? ctx->plat->text_font(ctx->plat->user, language, sample_id) : 0u;
#else
    (void)ctx;
    return ROM_TEXT_FONT(language, sample_id);
#endif
}

static uint32_t ui_row_count(PhotoMenuCtx *ctx, void *page) {
#ifdef V71R7_HOST_TEST
    return (ctx && ctx->plat && ctx->plat->row_count)
        ? ctx->plat->row_count(ctx->plat->user, page) : 0u;
#else
    (void)ctx;
    return ROM_ROW_COUNT(page);
#endif
}

static void ui_row_disable(PhotoMenuCtx *ctx, void *page, uint32_t row, uint32_t disable) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->row_disable) ctx->plat->row_disable(ctx->plat->user, page, row, disable);
#else
    (void)ctx;
    ROM_ROW_DISABLE(page, row, disable);
#endif
}

static void ui_row_focus(PhotoMenuCtx *ctx, void *page, uint32_t row, uint32_t focus) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->row_focus) ctx->plat->row_focus(ctx->plat->user, page, row, focus);
#else
    (void)ctx;
    ROM_ROW_FOCUS(page, row, focus);
#endif
}

static void ui_list_layout(PhotoMenuCtx *ctx, void *page) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->list_layout) ctx->plat->list_layout(ctx->plat->user, page);
#else
    (void)ctx;
    ROM_LIST_LAYOUT(page);
#endif
}

static void ui_view_layout(PhotoMenuCtx *ctx, void *page) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->view_layout) ctx->plat->view_layout(ctx->plat->user, page);
#else
    (void)ctx;
    ROM_VIEW_LAYOUT(page);
#endif
}

static void ui_scrollbar(PhotoMenuCtx *ctx, void *page) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->scrollbar) ctx->plat->scrollbar(ctx->plat->user, page);
#else
    (void)ctx;
    ROM_SCROLLBAR(page);
#endif
}

static void ui_set_rect(PhotoMenuCtx *ctx, void *widget, const int16_t *bounds) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->set_rect) ctx->plat->set_rect(ctx->plat->user, widget, bounds);
#else
    (void)ctx;
    ROM_RECT(widget, bounds);
#endif
}

static uint32_t ui_view_id(PhotoMenuCtx *ctx, uint32_t group) {
#ifdef V71R7_HOST_TEST
    return (ctx && ctx->plat && ctx->plat->view_id)
        ? ctx->plat->view_id(ctx->plat->user, group) : group;
#else
    (void)ctx;
    return ROM_VIEW_ID(group);
#endif
}

static void ui_view_set(PhotoMenuCtx *ctx, uint32_t id, const int16_t *rect) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->view_set) ctx->plat->view_set(ctx->plat->user, id, rect);
#else
    (void)ctx;
    ROM_VIEW_SET(id, rect);
#endif
}

static void ui_view_rect(PhotoMenuCtx *ctx, uint32_t id, int16_t *rect) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->view_rect) ctx->plat->view_rect(ctx->plat->user, id, rect);
#else
    (void)ctx;
    ROM_VIEW_RECT(id, rect);
#endif
}

static void ui_repaint(PhotoMenuCtx *ctx, uint8_t *page) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->mark) ctx->plat->mark(ctx->plat->user, page + 0x18u, 0x403efe48u);
    if (ctx && ctx->plat && ctx->plat->redraw) ctx->plat->redraw(ctx->plat->user, 0x403efe74u, 3u, 7u);
#else
    (void)ctx;
    ROM_MARK(page + 0x18u, UINT64_C(0x403efe48));
    ROM_REDRAW(UINT64_C(0x403efe74), 3u, 7u);
#endif
}

static int ui_timer_start(PhotoMenuCtx *ctx, uint32_t slot, uint32_t ms, void (*cb)(void)) {
#ifdef V71R7_HOST_TEST
    return (ctx && ctx->plat && ctx->plat->timer_start)
        ? ctx->plat->timer_start(ctx->plat->user, slot, ms, cb) : 0;
#else
    (void)ctx;
    return ROM_TIMER_START(slot, ms, cb);
#endif
}

static void ui_timer_cancel(PhotoMenuCtx *ctx, uint32_t slot) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->timer_cancel) ctx->plat->timer_cancel(ctx->plat->user, slot);
#else
    (void)ctx;
    ROM_TIMER_CANCEL(slot);
#endif
}

static uintptr_t load_pointer(const uint8_t *memory) {
    uintptr_t value;
    memcpy(&value, memory, sizeof(value));
    return value;
}

static int ui_timer_active(PhotoMenuCtx *ctx, void (**cb_out)(void)) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->timer_active)
        return ctx->plat->timer_active(ctx->plat->user, cb_out);
    if (cb_out) *cb_out = NULL;
    return 0;
#else
    (void)ctx;
    int active = *(volatile const uint8_t *)(uintptr_t)UINT64_C(0xa032b4ba0) != 0u;
    if (cb_out) {
        uintptr_t p = load_pointer((const uint8_t *)(uintptr_t)UINT64_C(0xa032b4bb0));
        *cb_out = (void (*)(void))p;
    }
    return active;
#endif
}

static void ui_stock_back_action(PhotoMenuCtx *ctx, void *action) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->stock_back_action)
        ctx->plat->stock_back_action(ctx->plat->user, action);
#else
    (void)ctx;
    ROM_STOCK_BACK_ACTION(action);
#endif
}

static void *ui_font_record(PhotoMenuCtx *ctx, uint32_t font) {
#ifdef V71R7_HOST_TEST
    return (ctx && ctx->plat && ctx->plat->font_record_ptr)
        ? ctx->plat->font_record_ptr(ctx->plat->user, font) : NULL;
#else
    (void)ctx;
    return (void *)(uintptr_t)(UINT64_C(0xa032bf0c8) + 16u * font);
#endif
}

static void *ui_text_record(PhotoMenuCtx *ctx, uint32_t index) {
#ifdef V71R7_HOST_TEST
    return (ctx && ctx->plat && ctx->plat->text_record_ptr)
        ? ctx->plat->text_record_ptr(ctx->plat->user, index) : NULL;
#else
    (void)ctx;
    return (void *)(uintptr_t)(UINT64_C(0xa032bd4a8) + 24u * (TEXT_BASE + index));
#endif
}

static void *ui_disabled_flags(PhotoMenuCtx *ctx) {
#ifdef V71R7_HOST_TEST
    return (ctx && ctx->plat && ctx->plat->disabled_flags_ptr)
        ? ctx->plat->disabled_flags_ptr(ctx->plat->user) : NULL;
#else
    (void)ctx;
    return (void *)(uintptr_t)(UINT64_C(0xa032aa850) + PHOTO_LUT_ROW - 0x2921u);
#endif
}

/* ============================================================================
 * 3. Public Text Resolution APIs (for live_ui_menu_text & v71r7_lut_abi.h)
 * ============================================================================ */
int v71r7_photo_menu_lookup_text(int32_t language, uint32_t text_id,
                                 const uint16_t **text_out, uint32_t *bytes_out) {
    if (!text_out || !bytes_out) return 0;
    uint32_t index = text_id - PHOTO_FIRST_TEXT;
    if (index >= (uint32_t)(sizeof(k_labels) / sizeof(k_labels[0]))) return 0;
    const Labels4 *lbl = &k_labels[index];
    uint32_t dis = lut_ui_disabled_mode();
    if (dis == 1u) {
        if (text_id == 0x1F42u) lbl = &k_title_mod_disabled;
        else if (text_id == 0x1F43u) lbl = &k_help_mod_disabled;
    } else if (dis == 2u) {
        if (text_id == 0x1F42u) lbl = &k_title_master_off;
        else if (text_id == 0x1F43u) lbl = &k_help_master_off;
    }
    const uint16_t *s = select_locale(lbl, (uint32_t)language);
    uint32_t n = 0;
    while (s[n]) ++n;
    *text_out = s;
    *bytes_out = n * (uint32_t)sizeof(uint16_t);
    return 1;
}

uint32_t photo_text_lookup(int32_t language, int32_t id, const uint16_t **text, uint32_t *bytes) {
    if (id < (int32_t)PHOTO_FIRST_TEXT || id >= (int32_t)PHOTO_TEXT_END) return FX_ARGUMENT;
    return v71r7_photo_menu_lookup_text(language, (uint32_t)id, text, bytes) ? FX_OK : FX_ARGUMENT;
}

int v71r7_photo_menu_resolve_text(uint32_t text_id, uint16_t *dst_utf16, uint32_t max_chars) {
    if (!dst_utf16 || max_chars == 0u) return -1;
    const uint16_t *src = NULL;
    uint32_t bytes = 0u;
    uint32_t lang = ui_get_language(photo_menu_ctx());
    if (!v71r7_photo_menu_lookup_text((int32_t)lang, text_id, &src, &bytes)) return -1;
    uint32_t chars = bytes / (uint32_t)sizeof(uint16_t);
    if (chars >= max_chars) chars = max_chars - 1u;
    for (uint32_t i = 0; i < chars; ++i) dst_utf16[i] = src[i];
    dst_utf16[chars] = 0u;
    return (int)chars;
}

/* ============================================================================
 * 4. Submenu 0x5D6 (23 Rows, Paginated Browser + Category Return) Controller
 * ============================================================================ */
static uint32_t load_halfword(const uint8_t *memory) {
    return (uint32_t)memory[0] | ((uint32_t)memory[1] << 8);
}

static void store_halfword(uint8_t *memory, uint16_t value) {
    memcpy(memory, &value, sizeof(value));
}

static int same_lut_page(const void *page) {
    return page && load_pointer((const uint8_t *)page + 0x50u) == (uintptr_t)&v71r7_lut_child_desc;
}

static uint32_t absolute_row(const uint8_t *page) {
    return load_halfword(page + 0x32u) + load_halfword(page + 0x58u);
}

static void widgets_lease(PhotoMenuCtx *ctx, int save) {
    PhotoUiState *ui = &ctx->ui;
    for (uint32_t i = 0; i <= PHOTO_STATUS_SLOT; ++i) {
        PhotoTextLease *lease = &ui->saved_text[i];
        uint8_t *record = (uint8_t *)ui_text_record(ctx, i);
        if (!record) continue;
        uintptr_t buffer = 0;
        if (save) memcpy(lease->record, record, sizeof(lease->record));
        memcpy(&buffer, lease->record + 8u, sizeof(buffer));
        if (save) {
            if (buffer) memcpy(lease->buffer, (const void *)buffer, sizeof(lease->buffer));
        } else {
            if (buffer) memcpy((void *)buffer, lease->buffer, sizeof(lease->buffer));
            memcpy(record, lease->record, sizeof(lease->record));
        }
    }
    const uint32_t fonts[2] = { FONT_FILE, FONT_WIDE };
    for (unsigned i = 0; i < 2u; ++i) {
        void *record = ui_font_record(ctx, fonts[i]);
        if (!record) continue;
        if (save) memcpy(ui->saved_fonts[i], record, sizeof(ui->saved_fonts[i]));
        else memcpy(record, ui->saved_fonts[i], sizeof(ui->saved_fonts[i]));
    }
    void *flags = ui_disabled_flags(ctx);
    if (flags) {
        if (save) memcpy(ui->saved_disabled, flags, PHOTO_LUT_ROWS);
        else memcpy(flags, ui->saved_disabled, PHOTO_LUT_ROWS);
    }
}

static void bind_ascii(PhotoMenuCtx *ctx, void *node, uint32_t index, uint32_t font, const char *value) {
    if (!node) return;
    ui_set_text(ctx, value, TEXT_BASE + index, font);
    ui_bind_text(ctx, node, 0u, 0u);
    ui_bind_text(ctx, node, TEXT_BASE + index, 3u);
    ui_dirty(ctx, node, 1u);
}

static void bind_wide(PhotoMenuCtx *ctx, void *node, uint32_t index, uint32_t language, const uint16_t *value) {
    if (!node) return;
    ui_set_wide(ctx, value, TEXT_BASE + index, FONT_WIDE);
    if (language == PHOTO_LANG_JA || language == PHOTO_LANG_ZH_CN || language == PHOTO_LANG_ZH_TW) {
        uint8_t *fr = (uint8_t *)ui_font_record(ctx, FONT_WIDE);
        if (fr) *fr = (uint8_t)ui_text_font(ctx, language, 0x8aeu);
    }
    ui_bind_text(ctx, node, 0u, 0u);
    ui_bind_text(ctx, node, TEXT_BASE + index, 4u);
    ui_dirty(ctx, node, 1u);
}

static void bind_status(PhotoMenuCtx *ctx, void *node, uint32_t language, const PhotoStatus *s) {
    if (!node) return;
    const uint32_t count = (uint32_t)(sizeof(k_statuses) / sizeof(k_statuses[0]));
    const Labels4 *label = &k_statuses[s->phase < count ? s->phase : (uint32_t)PHOTO_LOAD_ERROR];
    if (s->phase == (uint32_t)PHOTO_LOAD_ERROR) {
        if (s->reason == (uint32_t)FX_ARGUMENT || s->reason == (uint32_t)FX_INTEGRITY)
            label = &k_status_invalid;
        else if (s->reason == (uint32_t)FX_BUDGET)
            label = &k_status_resources;
        else if (s->reason == (uint32_t)FX_CLOSE)
            label = &k_status_close_error;
    }
    const uint16_t *text = select_locale(label, language);
    uint16_t display[32];
    unsigned n = 0;
    while (text[n] && n < 25u) {
        display[n] = text[n];
        ++n;
    }
    if (s->phase == (uint32_t)PHOTO_PREPARING && s->total && s->done <= s->total) {
        uint32_t percent = (uint32_t)((uint64_t)s->done * 100u / s->total);
        display[n++] = ' ';
        if (percent == 100u) display[n++] = '1';
        if (percent >= 10u) display[n++] = (uint16_t)('0' + (percent / 10u) % 10u);
        display[n++] = (uint16_t)('0' + percent % 10u);
        display[n++] = '%';
    }
    display[n] = 0u;
    ui_dynamic(ctx, node, 1u);
    bind_wide(ctx, node, PHOTO_STATUS_SLOT, language, display);
}

static void release_ui(PhotoMenuCtx *ctx) {
    PhotoUiState *ui = &ctx->ui;
    if (!ui->page) return;
    ui->page = NULL;
    ++ui->epoch;
    ui->timer_epoch = 0u;
    void (*active_cb)(void) = NULL;
    if (ui_timer_active(ctx, &active_cb) && active_cb == photo_lut_timer)
        ui_timer_cancel(ctx, TIMER_SLOT);
    ui_draw_fence(ctx);
    widgets_lease(ctx, 0);
    if (ui->active) photo_lut_close_browser();
    ui->active = 0u;
    ui->lease_epoch = 0u;
    atomic_store_explicit(&ui->ui_leased, 0u, memory_order_release);
}

uint32_t photo_ui_pending(void) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx) return 0u;
    return atomic_load_explicit(&ctx->ui.ui_leased, memory_order_acquire);
}

static int lease_current(PhotoMenuCtx *ctx) {
    PhotoUiState *ui = &ctx->ui;
    if (!ui->page) return 0;
    if (ui->lease_epoch == photo_gui_epoch()) return 1;
    ui->resume_page = ui->page;
    ui->resume_active = ui->active;
    release_ui(ctx);
    return 0;
}

static void maybe_timer(PhotoMenuCtx *ctx) {
    PhotoUiState *ui = &ctx->ui;
    if (lease_current(ctx) && ui->active && !ui_timer_active(ctx, NULL)) {
        ui->timer_epoch = ui->epoch;
        if (!ui_timer_start(ctx, TIMER_SLOT, 100u, photo_lut_timer))
            ui->timer_epoch = 0u;
        (void)lease_current(ctx);
    }
}

void photo_ui_deactivate(void) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx) return;
    PhotoUiState *ui = &ctx->ui;
    if (!lease_current(ctx)) {
        if (ui->resume_page && photo_gui_epoch()) {
            uint8_t *page = ui->resume_page;
            uint32_t was_active = ui->resume_active;
            ui->resume_page = NULL;
            ui->resume_active = 0u;
            if (was_active && ui_current_page(ctx) == page && same_lut_page(page))
                (void)photo_lut_entry(page);
        }
        return;
    }
    if (ui_current_page(ctx) != ui->page || !same_lut_page(ui->page)) release_ui(ctx);
    else maybe_timer(ctx);
}

static int start_ui(PhotoMenuCtx *ctx, void *page) {
    PhotoUiState *ui = &ctx->ui;
    uint64_t epoch = photo_gui_epoch();
    int leased = lease_current(ctx);
    if (!epoch || !same_lut_page(page)) return 0;
    if (leased && ui->page == page && ui->active) return 1;
    release_ui(ctx);
    uint32_t unleased = 0u;
    if (!atomic_compare_exchange_strong(&ui->ui_leased, &unleased, 1u)) return 0;
    if (photo_gui_epoch() != epoch) {
        atomic_store_explicit(&ui->ui_leased, 0u, memory_order_release);
        return 0;
    }
    ui->active = 1u;
    ui->page = (uint8_t *)page;
    ui->resume_page = NULL;
    ui->resume_active = 0u;
    ui->lease_epoch = epoch;
    ++ui->epoch;
    ui->page_bound = 0u;
    ui->in_folder = 0u;
    ui->return_pending = 0u;
    ui->root_focus = 0u;
    ui->last_status.phase = UINT32_MAX;
    ui->language = UINT32_MAX;
    widgets_lease(ctx, 1);
    return lease_current(ctx);
}

static void browser_layout(PhotoMenuCtx *ctx, uint8_t *page, const PhotoPage *view) {
    PhotoUiState *ui = &ctx->ui;
    uint32_t old = absolute_row(page);
    int restore = (view->focus != 0u && view->focus == ui->root_focus);
    if (old < PHOTO_LUT_ROWS) ui_row_focus(ctx, page, old, 0u);
    for (uint32_t i = 0; i < PHOTO_LUT_ROWS; ++i) {
        uint32_t item = PHOTO_LUT_ROW + i;
        int show = (i <= view->count) ||
                   (i == PHOTO_ROW_PREVIOUS && view->previous) ||
                   (i == PHOTO_ROW_NEXT && view->next);
        void *widget = ui_find_widget(ctx, item, page + 0x18u);
        ui_row_disable(ctx, page, i, (uint32_t)!show);
        if (!widget) continue;
        uintptr_t row_draw = (uintptr_t)ROM_ROW_DRAW;
        memcpy((uint8_t *)widget + 0x48u, &row_draw, sizeof(row_draw));
        ui_dirty(ctx, widget, (uint32_t)show);
        for (uint32_t child_id = VALUE_CHILD; child_id <= 0x4baau; ++child_id) {
            void *child = ui_find_child(ctx, page, item, child_id);
            if (child) ui_dynamic(ctx, child, 0u);
        }
    }
    store_halfword(page + 0x30u, (uint16_t)ui_row_count(ctx, page));
    store_halfword(page + 0x32u, restore ? ui->root_row : 0u);
    store_halfword(page + 0x58u, restore ? ui->root_scroll : 0u);
    ui_list_layout(ctx, page);
    for (uint32_t i = 0; i < PHOTO_LUT_ROWS; ++i) {
        uint8_t *widget = (uint8_t *)ui_find_widget(ctx, PHOTO_LUT_ROW + i, page + 0x18u);
        if (!widget) continue;
        int16_t bounds[4];
        memcpy(bounds, widget + 0x10u, sizeof(bounds));
        bounds[2] = (int16_t)(bounds[0] + 708);
        ui_set_rect(ctx, widget, bounds);
        const int16_t text[4] = {
            (int16_t)(bounds[0] + 6), (int16_t)(bounds[1] + 8),
            (int16_t)(bounds[2] - 6), (int16_t)(bounds[1] + 48)
        };
        memcpy(widget + 0xc0u, text, sizeof(text));
    }
    ui_view_layout(ctx, page);
    if (restore) {
        store_halfword(page + 0x32u, ui->root_row);
        store_halfword(page + 0x58u, ui->root_scroll);
    }
    uint32_t view_id = ui_view_id(ctx, 2u);
    int16_t rect[4] = {0, 0, 0, 0};
    if (restore) {
        memcpy(rect, ui->root_view, sizeof(rect));
    } else {
        ui_view_rect(ctx, view_id, rect);
        rect[3] = (int16_t)(rect[3] - rect[1]);
        rect[1] = 0;
    }
    ui_view_set(ctx, view_id, rect);
    ui_row_focus(ctx, page, restore ? view->focus : 0u, 1u);
    ui_scrollbar(ctx, page);
}

static const Labels4 *category_label(const char *name) {
    if (name[0] == PHOTO_FLAT_FOLDER[0]) return &k_flat_label;
    for (unsigned i = 0; i < (unsigned)(sizeof(k_categories) / sizeof(k_categories[0])); ++i) {
        if (!photo_name_compare(name, k_categories[i].category))
            return &k_categories[i].label;
    }
    return NULL;
}

static void browser_text(PhotoMenuCtx *ctx, uint8_t *page, const PhotoPage *view, uint32_t language) {
    for (uint32_t i = 0; i < PHOTO_LUT_ROWS; ++i) {
        const Labels4 *label = NULL;
        const char *name = NULL;
        if (i == 0u) {
            label = (view->level == PHOTO_FOLDER) ? &k_back_label : &k_labels[7];
        } else if (i <= view->count) {
            name = view->names[i - 1u];
            if (view->level == PHOTO_ROOT) label = category_label(name);
        } else if (i == PHOTO_ROW_PREVIOUS && view->previous) {
            label = &k_labels[8];
        } else if (i == PHOTO_ROW_NEXT && view->next) {
            label = &k_labels[9];
        }
        if (!label && !name) continue;
        void *node = ui_find_widget(ctx, PHOTO_LUT_ROW + i, page + 0x18u);
        if (!node) continue;
        if (label) bind_wide(ctx, node, i, language, select_locale(label, language));
        else bind_ascii(ctx, node, i, FONT_FILE, name);
    }
}

int photo_lut_refresh(void *page_arg) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx) return 0;
    PhotoUiState *ui = &ctx->ui;
    uint8_t *page = (uint8_t *)page_arg;
    if (!lease_current(ctx) || ui->page != page || !ui->active || !same_lut_page(page)) return 0;

    uint32_t language = ui_get_language(ctx);
    int language_changed = (ui->language != language);

    /* Disabled / Master-Off mode: 0 card I/O, show guidance header + 1 non-actionable row. */
    uint32_t dis = lut_ui_disabled_mode();
    if (dis != 0u) {
        PhotoPage empty_view;
        memset(&empty_view, 0, sizeof(empty_view));
        empty_view.level = PHOTO_ROOT;
        PhotoStatus dis_status = { 0u, (uint32_t)PHOTO_MODULE_DISABLED, dis, 0u, 0u };
        int changed = !ui->page_bound || language_changed ||
                      memcmp(&dis_status, &ui->last_status, sizeof(dis_status)) != 0;
        if (changed) {
            if (language_changed) {
                void *ff = ui_font_record(ctx, FONT_FILE);
                void *fw = ui_font_record(ctx, FONT_WIDE);
                if (ff) memcpy(ff, ui->saved_fonts[0], sizeof(ui->saved_fonts[0]));
                if (fw) memcpy(fw, ui->saved_fonts[1], sizeof(ui->saved_fonts[1]));
            }
            browser_layout(ctx, page, &empty_view);
            ui->page_view = empty_view;
            ui->page_bound = 1u;
            ui->in_folder = 0u;
            ui->return_pending = 0u;
            void *row0 = ui_find_widget(ctx, PHOTO_LUT_ROW, page + 0x18u);
            const Labels4 *row_lbl = (dis == 1u) ? &k_row_mod_disabled : &k_row_master_off;
            bind_wide(ctx, row0, 0u, language, select_locale(row_lbl, language));
            void *st_node = ui_find_widget(ctx, TITLE_STATUS, page + 0x18u);
            if (st_node) {
                const Labels4 *st_lbl = (dis == 1u)
                    ? &k_statuses[PHOTO_MODULE_DISABLED]
                    : &k_status_master_off;
                ui_dynamic(ctx, st_node, 1u);
                bind_wide(ctx, st_node, PHOTO_STATUS_SLOT, language,
                          select_locale(st_lbl, language));
            }
            ui->last_status = dis_status;
            ui->language = language;
            ui_repaint(ctx, page);
        }
        maybe_timer(ctx);
        return 0;
    }

    PhotoPage view;
    int published = photo_lut_page(&view);
    int placeholder = !published &&
                      (!ui->page_bound || (ui->in_folder && ui->page_view.level == PHOTO_ROOT));
    int changed = placeholder ||
                  (published && (!ui->page_bound || memcmp(&view, &ui->page_view, sizeof(view)) != 0));
    if (changed) {
        if (placeholder) {
            memset(&view, 0, sizeof(view));
            view.level = ui->in_folder ? PHOTO_FOLDER : PHOTO_ROOT;
        }
        browser_layout(ctx, page, &view);
        ui->page_view = view;
        ui->page_bound = 1u;
        ui->in_folder = (view.level == PHOTO_FOLDER) ? 1u : 0u;
        if (!ui->in_folder) ui->return_pending = 0u;
    }
    if (language_changed) {
        void *ff = ui_font_record(ctx, FONT_FILE);
        void *fw = ui_font_record(ctx, FONT_WIDE);
        if (ff) memcpy(ff, ui->saved_fonts[0], sizeof(ui->saved_fonts[0]));
        if (fw) memcpy(fw, ui->saved_fonts[1], sizeof(ui->saved_fonts[1]));
    }
    if (changed || language_changed)
        browser_text(ctx, page, &ui->page_view, language);

    PhotoStatus now = photo_lut_status();
    if (language_changed || memcmp(&now, &ui->last_status, sizeof(now)) != 0) {
        bind_status(ctx, ui_find_widget(ctx, TITLE_STATUS, page + 0x18u), language, &now);
        ui->last_status = now;
        ui->language = language;
        changed = 1;
    }
    if (changed) ui_repaint(ctx, page);
    maybe_timer(ctx);
    return 0;
}

void photo_lut_timer(void) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx) return;
    PhotoUiState *ui = &ctx->ui;
    if (!lease_current(ctx) || !ui->active || !ui->timer_epoch || ui->timer_epoch != ui->epoch) return;
    if (ui_current_page(ctx) != ui->page || !same_lut_page(ui->page)) {
        release_ui(ctx);
        return;
    }
    (void)photo_lut_refresh(ui->page);
}

int photo_lut_entry(void *page) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx) return 0;
    PhotoUiState *ui = &ctx->ui;
    int existing = lease_current(ctx) && ui->page == page && ui->active;
    if (!existing && start_ui(ctx, page)) {
        if (lut_ui_disabled_mode() == 0u) {
            photo_lut_request_page(0);
        }
    }
    return photo_lut_refresh(page);
}

int photo_lut_exit(void *page) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx) return 0;
    if (lease_current(ctx) && ctx->ui.page == page && ctx->ui.active)
        release_ui(ctx);
    return 0;
}

static int owns_action_page(PhotoMenuCtx *ctx, const PhotoMenuAction *action) {
    PhotoUiState *ui = &ctx->ui;
    return action && ui->active && ui->page == action->page &&
           ui->lease_epoch == photo_gui_epoch() && same_lut_page(action->page);
}

static uint32_t event_row(PhotoMenuCtx *ctx, const PhotoMenuAction *action) {
    if (!owns_action_page(ctx, action)) return UINT32_MAX;
    uint32_t event = action->event;
    if (event != 0x286u && event != 0x28bu && event != 0x29fu && event != 0x2aeu)
        return UINT32_MAX;
    uint32_t row = absolute_row(action->page);
    if (row >= v71r7_lut_child_desc.count) return UINT32_MAX;
    const V71r7MenuRow *rows = (const V71r7MenuRow *)v71r7_lut_child_desc.rows;
    return rows[row].words[6] == PHOTO_LUT_ROW + row ? row : UINT32_MAX;
}

static void leave_folder(PhotoMenuCtx *ctx) {
    PhotoUiState *ui = &ctx->ui;
    if (!ui->return_pending) {
        ui->return_pending = 1u;
        photo_lut_leave_folder(ui->root_focus);
    }
}

int photo_lut_back_action(void *action_arg) {
    PhotoMenuAction *action = (PhotoMenuAction *)action_arg;
    if (!action) return 0;
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (ctx && owns_action_page(ctx, action) && ctx->ui.in_folder) {
        action->target = UI_STAY;
        leave_folder(ctx);
    } else if (action->event == UI_LEFT || action->event == UI_TOUCH_BACK) {
        ui_stock_back_action(ctx, action);
    }
    return 0;
}

int photo_lut_activate(void *action_arg) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx || lut_ui_disabled_mode() != 0u) return 0;
    PhotoUiState *ui = &ctx->ui;
    const PhotoMenuAction *action = (const PhotoMenuAction *)action_arg;
    uint32_t row = event_row(ctx, action);
    if (row >= PHOTO_LUT_ROWS) return 0;

    /* A folder can be left while its first snapshot is still scanning. Its
     * placeholder has only Back and never exposes stale file indices. */
    if (row == 0u && ui->page_bound && ui->page_view.level == PHOTO_FOLDER && !ui->page_view.serial) {
        leave_folder(ctx);
        return 0;
    }

    PhotoPage current;
    if (!ui->page_bound || !photo_lut_page(&current) ||
        current.epoch != ui->page_view.epoch || current.serial != ui->page_view.serial)
        return 0;

    if (row == 0u) {
        if (current.level == PHOTO_FOLDER) leave_folder(ctx);
        else (void)photo_lut_select("", ui->lease_epoch);
    } else if (row <= current.count) {
        const char *name = current.names[row - 1u];
        if (current.level == PHOTO_ROOT) {
            ui->root_row = (uint16_t)load_halfword(ui->page + 0x32u);
            ui->root_scroll = (uint16_t)load_halfword(ui->page + 0x58u);
            ui->root_focus = row;
            ui_view_rect(ctx, ui_view_id(ctx, 2u), ui->root_view);
            photo_lut_enter_folder(name, row);
            ui->in_folder = 1u;
        } else {
            char path[FX_Z8_LUT_PATH_BYTES];
            photo_folder_path(current.category, name, path);
            (void)photo_lut_select(path, ui->lease_epoch);
        }
    } else if (row == PHOTO_ROW_PREVIOUS && current.previous) {
        photo_lut_request_page(-1);
    } else if (row == PHOTO_ROW_NEXT && current.next) {
        photo_lut_request_page(1);
    }
    return 0;
}
