/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 .cube LUT Catalog Scanner & Paginator (Zero Globals, Strict Scalar).
 *
 * Scans:
 *   - <drive>:\FENESTRA\*.cube            ("Root LUTs" / PHOTO_FLAT_FOLDER "~")
 *   - <drive>:\FENESTRA\LUTS\<cat>\*.cube (1-level categories)
 * Supports 20 items/page, forward & backward pagination cursors, and
 * stock/hardened Z8 directory primitives 0x402DDC6C / 0x402DDD1C / 0x402DDD6C.
 */
#include "photo_menu.h"

#define PHOTO_FN(addr, type) ((type)(uintptr_t)UINT64_C(addr))
#define PHOTO_DIR_OPEN_ROM   PHOTO_FN(0x402ddc6c, uint32_t (*)(const char *, int32_t *))
#define PHOTO_DIR_NEXT_ROM   PHOTO_FN(0x402ddd1c, uint32_t (*)(int32_t, void *))
#define PHOTO_DIR_CLOSE_ROM  PHOTO_FN(0x402ddd6c, uint32_t (*)(int32_t))

enum {
    PHOTO_NATIVE_ENOENT = 0x80000010u,
    PHOTO_NATIVE_DIR_EOF = 0x80000030u
};

static unsigned folded(unsigned byte) {
    return (byte >= 'A' && byte <= 'Z') ? byte + ('a' - 'A') : byte;
}

int photo_name_compare(const char *a, const char *b) {
    while (*a && folded((unsigned char)*a) == folded((unsigned char)*b)) {
        ++a;
        ++b;
    }
    return (int)folded((unsigned char)*a) - (int)folded((unsigned char)*b);
}

static uint32_t dir_word(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void photo_catalog_start(PhotoCatalog *catalog, int32_t direction, const char *cursor) {
    memset(catalog, 0, sizeof(*catalog));
    catalog->direction = direction;
    if (direction && cursor) {
        size_t i = 0;
        for (; i + 1u < sizeof(catalog->cursor) && cursor[i]; ++i)
            catalog->cursor[i] = cursor[i];
    }
}

void photo_folder_path(const char *category, const char *name, char *out) {
    if (category && category[0] && category[0] != PHOTO_FLAT_FOLDER[0]) {
        memcpy(out, "LUTS\\", 5u);
        out += 5;
        while (*category) *out++ = *category++;
        if (name && name[0]) *out++ = '\\';
    }
    if (name) {
        while (*name) *out++ = *name++;
    }
    *out = '\0';
}

int photo_dir_name(const uint8_t entry[PHOTO_DIR_BYTES], const char *category, int folders,
                   char name[FX_Z8_LUT_NAME_BYTES]) {
    unsigned attrs = entry[0x20eu];
    if ((attrs & 8u) || (!!(attrs & 16u) != (folders != 0))) return 0;
    if (!folders) {
        uint32_t bytes = dir_word(entry + 0x218u);
        if (dir_word(entry + 0x21cu) || !bytes || bytes > PHOTO_MAX_CUBE_BYTES) return 0;
    }
    memset(name, 0, FX_Z8_LUT_NAME_BYTES);
    int long_name = (entry[0x0eu] || entry[0x0fu]);
    size_t limit = long_name ? FX_Z8_LUT_NAME_BYTES : 13u;
    size_t n = 0;
    for (; n < limit; ++n) {
        unsigned c = long_name
            ? ((unsigned)entry[14u + 2u * n] + 256u * (unsigned)entry[15u + 2u * n])
            : (unsigned)entry[n];
        if (!c) break;
        if (c > 127u) return 0;
        name[n] = (char)c;
    }
    if (n == limit) return 0;
    if (folders) return fx_z8_lut_category(name);
    if (!fx_z8_lut_name(name)) return 0;
    if (!category || !category[0] || category[0] == PHOTO_FLAT_FOLDER[0]) return 1;
    /* Relative path is "LUTS\\" + category + "\\" + name + NUL (6 extra bytes). */
    for (size_t i = 0; category[i]; ++i) ++n;
    return n + 6u < FX_Z8_LUT_PATH_BYTES;
}

void photo_catalog_add(PhotoCatalog *catalog, const char name[FX_Z8_LUT_NAME_BYTES]) {
    int sign = catalog->direction < 0 ? -1 : 1;
    int from_cursor = photo_name_compare(name, catalog->cursor);
    if (catalog->direction && from_cursor * sign <= 0) {
        catalog->opposite = 1u;
        return;
    }
    unsigned at = 0;
    for (; at < catalog->count; ++at) {
        int order = photo_name_compare(name, catalog->names[at]);
        if (!order) {
            if (memcmp(name, catalog->names[at], FX_Z8_LUT_NAME_BYTES) < 0)
                memcpy(catalog->names[at], name, FX_Z8_LUT_NAME_BYTES);
            return;
        }
        if (order * sign < 0) break;
    }
    if (at == PHOTO_FILES_PER_PAGE + 1u) return;
    if (catalog->count < PHOTO_FILES_PER_PAGE + 1u) ++catalog->count;
    for (unsigned i = catalog->count - 1u; i > at; --i)
        memcpy(catalog->names[i], catalog->names[i - 1u], FX_Z8_LUT_NAME_BYTES);
    memcpy(catalog->names[at], name, FX_Z8_LUT_NAME_BYTES);
}

void photo_catalog_finish(const PhotoCatalog *catalog, PhotoPage *page) {
    memset(page, 0, sizeof(*page));
    page->count = catalog->count > PHOTO_FILES_PER_PAGE ? PHOTO_FILES_PER_PAGE : catalog->count;
    int backwards = catalog->direction < 0;
    page->previous = backwards ? (catalog->count > PHOTO_FILES_PER_PAGE) : catalog->opposite;
    page->next = backwards ? catalog->opposite : (catalog->count > PHOTO_FILES_PER_PAGE);
    for (unsigned i = 0; i < page->count; ++i) {
        unsigned from = backwards ? (page->count - 1u - i) : i;
        memcpy(page->names[i], catalog->names[from], sizeof(page->names[i]));
    }
}

/* ============================================================================
 * Direct Synchronous Directory Scanner (uses v71r7_active_card_drive() +
 * hardened Z8 opendir/readdir/closedir 0x402DDC6C/D1C/D6C; mockable on host).
 * ============================================================================ */
static char cat_active_drive(void) {
#ifdef V71R7_HOST_TEST
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (ctx && ctx->plat && ctx->plat->card_drive)
        return ctx->plat->card_drive(ctx->plat->user);
#endif
    return v71r7_active_card_drive();
}

static uint32_t cat_dir_open(const char *path, int32_t *fd) {
#ifdef V71R7_HOST_TEST
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (ctx && ctx->plat && ctx->plat->dir_open)
        return ctx->plat->dir_open(ctx->plat->user, path, fd);
    *fd = -1;
    return PHOTO_NATIVE_ENOENT;
#else
    return PHOTO_DIR_OPEN_ROM(path, fd);
#endif
}

static uint32_t cat_dir_next(int32_t fd, void *buf) {
#ifdef V71R7_HOST_TEST
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (ctx && ctx->plat && ctx->plat->dir_next)
        return ctx->plat->dir_next(ctx->plat->user, fd, buf);
    return PHOTO_NATIVE_DIR_EOF;
#else
    return PHOTO_DIR_NEXT_ROM(fd, buf);
#endif
}

static uint32_t cat_dir_close(int32_t fd) {
#ifdef V71R7_HOST_TEST
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (ctx && ctx->plat && ctx->plat->dir_close)
        return ctx->plat->dir_close(ctx->plat->user, fd);
    return 0u;
#else
    return PHOTO_DIR_CLOSE_ROM(fd);
#endif
}

static int scan_one_dir(const char *dir_path, const char *category, int folders,
                        int stop_on_first_flat, PhotoCatalog *cat) {
    int32_t fd = -1;
    uint32_t rc = cat_dir_open(dir_path, &fd);
    if (rc || fd < 0) {
        if (fd >= 0) (void)cat_dir_close(fd);
        return (rc == PHOTO_NATIVE_ENOENT && fd < 0) ? 1 : 0;
    }
    _Alignas(64) uint8_t entry[PHOTO_DIR_BUF_BYTES];
    int ok = 1;
    for (;;) {
        memset(entry, 0, sizeof(entry));
        rc = cat_dir_next(fd, entry);
        if (rc == PHOTO_NATIVE_DIR_EOF) break;
        if (rc) {
            ok = 0;
            break;
        }
        char name[FX_Z8_LUT_NAME_BYTES];
        if (!photo_dir_name(entry, category, folders, name)) continue;
        if (stop_on_first_flat) {
            char flat[FX_Z8_LUT_NAME_BYTES];
            memset(flat, 0, sizeof(flat));
            flat[0] = PHOTO_FLAT_FOLDER[0];
            photo_catalog_add(cat, flat);
            break;
        }
        photo_catalog_add(cat, name);
    }
    if (cat_dir_close(fd)) ok = 0;
    return ok;
}

int photo_catalog_scan_directory(const char *category, int32_t direction, const char *cursor,
                                 PhotoPage *out_page) {
    if (!out_page) return 0;
    char drive = cat_active_drive();
    if (drive != 'A' && drive != 'B') return 0;

    PhotoCatalog cat;
    photo_catalog_start(&cat, direction, cursor);
    char dir_path[PHOTO_PATH_BYTES];

    if (!category || !category[0]) {
        /* Root view: scan <drive>:\FENESTRA\LUTS for categories + check <drive>:\FENESTRA\*.cube */
        photo_card_path(drive, "LUTS", dir_path);
        if (!scan_one_dir(dir_path, NULL, 1, 0, &cat)) return 0;
        memcpy(dir_path, "A:\\FENESTRA", 12u);
        dir_path[0] = drive;
        if (!scan_one_dir(dir_path, PHOTO_FLAT_FOLDER, 0, 1, &cat)) return 0;
        photo_catalog_finish(&cat, out_page);
        out_page->level = PHOTO_ROOT;
        return 1;
    }

    if (category[0] == PHOTO_FLAT_FOLDER[0] && !category[1]) {
        memcpy(dir_path, "A:\\FENESTRA", 12u);
        dir_path[0] = drive;
    } else if (fx_z8_lut_category(category)) {
        char rel[FX_Z8_LUT_PATH_BYTES];
        photo_folder_path(category, "", rel);
        photo_card_path(drive, rel, dir_path);
    } else {
        return 0;
    }

    if (!scan_one_dir(dir_path, category, 0, 0, &cat)) return 0;
    photo_catalog_finish(&cat, out_page);
    out_page->level = PHOTO_FOLDER;
    size_t i = 0;
    for (; i + 1u < sizeof(out_page->category) && category[i]; ++i)
        out_page->category[i] = category[i];
    return 1;
}
