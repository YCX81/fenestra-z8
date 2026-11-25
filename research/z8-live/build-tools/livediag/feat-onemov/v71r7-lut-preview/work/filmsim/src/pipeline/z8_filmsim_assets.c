/* SPDX-License-Identifier: MIT
 * Freestanding Card Asset Loader for Film Simulation Runtime.
 * Loads and CRC32-validates ZF_GRAIN.BIN (ZFG1, 27,588 B).
 * Zero malloc, zero writable .data/.bss, scalar-only (-mgeneral-regs-only), zero libc.
 */
#include "z8_filmsim_assets.h"
#include "z8_zf_grain.h"

_Static_assert(sizeof(z8_zf_grain_maps) <= sizeof(z8_filmsim_zf_maps_box),
               "z8_filmsim_zf_maps_box must hold z8_zf_grain_maps");

static int valid_drive(char d)
{
    return d == 'A' || d == 'B';
}

static size_t copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = 0U;
    while (src[n] != '\0') {
        if (n + 1U >= cap) {
            return 0U;
        }
        dst[n] = src[n];
        ++n;
    }
    dst[n] = '\0';
    return n;
}

size_t z8_filmsim_zf_grain_bin_bytes(void)
{
    return Z8_ZF_GRAIN_FILE_BYTES;
}

int z8_filmsim_format_zf_grain_path(char drive, char *out, size_t cap)
{
    if (!out || !valid_drive(drive) || cap < 25U) {
        return Z8_FS_ASSET_E_ARG;
    }
    size_t n = copy_str(out, cap, Z8_FILMSIM_ZF_GRAIN_PATH);
    if (n == 0U) {
        return Z8_FS_ASSET_E_ARG;
    }
    out[0] = drive;
    return Z8_FS_ASSET_OK;
}

int z8_filmsim_read_exact_file(const z8_filmsim_file_ops *ops,
                               const char *path,
                               void *dst, size_t expected_bytes)
{
    if (!ops || !ops->open_read || !ops->read || !ops->close ||
        !path || !dst || expected_bytes == 0U) {
        return Z8_FS_ASSET_E_ARG;
    }
    if (ops->cancelled && ops->cancelled(ops->opaque)) {
        return Z8_FS_ASSET_E_CANCELLED;
    }

    int fd = -1;
    if (ops->open_read(ops->opaque, path, &fd) != 0 || fd < 0) {
        return Z8_FS_ASSET_E_OPEN;
    }

    uint8_t *out = (uint8_t *)dst;
    size_t total = 0U;
    int err = Z8_FS_ASSET_OK;

    while (total < expected_bytes) {
        if (ops->cancelled && ops->cancelled(ops->opaque)) {
            err = Z8_FS_ASSET_E_CANCELLED;
            break;
        }
        size_t rem = expected_bytes - total;
        size_t want = rem > Z8_FILMSIM_IO_CHUNK_BYTES ? Z8_FILMSIM_IO_CHUNK_BYTES : rem;
        size_t got = 0U;
        if (ops->read(ops->opaque, fd, out + total, want, &got) != 0) {
            err = Z8_FS_ASSET_E_READ;
            break;
        }
        if (got == 0U) {
            err = Z8_FS_ASSET_E_SIZE; /* Truncated file */
            break;
        }
        if (got > want) {
            err = Z8_FS_ASSET_E_READ;
            break;
        }
        total += got;
    }

    /* Probe 1 trailing byte into an isolated 64-byte cache line so native
     * D-cache invalidation (dc ivac) can never clobber adjacent stack locals. */
    if (err == Z8_FS_ASSET_OK) {
        _Alignas(64) uint8_t extra[64];
        extra[0] = 0U;
        size_t got_extra = 0U;
        int r = ops->read(ops->opaque, fd, extra, 1U, &got_extra);
        if (r == 0 && got_extra > 0U) {
            err = Z8_FS_ASSET_E_SIZE;
        }
    }

    (void)ops->close(ops->opaque, fd);
    return err;
}

int z8_filmsim_load_zf_grain_bin(const z8_filmsim_file_ops *ops,
                                 char drive,
                                 void *buf, size_t buf_bytes,
                                 z8_filmsim_zf_maps_box *zf_maps_out)
{
    if (!ops || !buf || !zf_maps_out || buf_bytes < Z8_ZF_GRAIN_FILE_BYTES ||
        (((uintptr_t)buf) & 1U) != 0U) {
        return Z8_FS_ASSET_E_ARG;
    }

    char path[Z8_FILMSIM_ASSET_PATH_MAX];
    int rc = z8_filmsim_format_zf_grain_path(drive, path, sizeof(path));
    if (rc != Z8_FS_ASSET_OK) {
        return rc;
    }

    rc = z8_filmsim_read_exact_file(ops, path, buf, Z8_ZF_GRAIN_FILE_BYTES);
    if (rc != Z8_FS_ASSET_OK) {
        return rc;
    }

    for (unsigned i = 0U; i < 8U; ++i) {
        zf_maps_out->storage[i] = 0U;
    }
    z8_zf_grain_maps maps;
    if (z8_zf_grain_validate_bin(buf, Z8_ZF_GRAIN_FILE_BYTES, &maps) != Z8_ZF_GRAIN_OK) {
        return Z8_FS_ASSET_E_VALIDATE;
    }

    const uint8_t *src = (const uint8_t *)&maps;
    uint8_t *dst = (uint8_t *)zf_maps_out->storage;
    for (size_t i = 0U; i < sizeof(maps); ++i) {
        dst[i] = src[i];
    }
    return Z8_FS_ASSET_OK;
}

int z8_filmsim_format_recipe_path(char drive, uint32_t slot01, char *out, size_t cap)
{
    if (!out || !valid_drive(drive) || slot01 > 1U || cap < 24U) {
        return Z8_FS_ASSET_E_ARG;
    }
    const char *tmpl = (slot01 == 0U) ? Z8_FILMSIM_RECIPE_PATH_0 : Z8_FILMSIM_RECIPE_PATH_1;
    size_t n = copy_str(out, cap, tmpl);
    if (n == 0U) {
        return Z8_FS_ASSET_E_ARG;
    }
    out[0] = drive;
    return Z8_FS_ASSET_OK;
}

int z8_filmsim_load_recipe_table(const z8_filmsim_file_ops *ops,
                                 char drive,
                                 Z8FilmSimRecipeTable *out)
{
    if (!ops || !valid_drive(drive) || !out) {
        return Z8_FS_ASSET_E_ARG;
    }

    int have_best = 0;
    int saw_corrupt = 0;
    uint32_t best_seq = 0U;

    for (uint32_t slot = 0U; slot < 2U; ++slot) {
        char path[Z8_FILMSIM_ASSET_PATH_MAX];
        int rc = z8_filmsim_format_recipe_path(drive, slot, path, sizeof(path));
        if (rc != Z8_FS_ASSET_OK) {
            return rc;
        }
        Z8FilmSimRecipeTable cand;
        rc = z8_filmsim_read_exact_file(ops, path, &cand, Z8_NP3_TABLE_BYTES);
        if (rc == Z8_FS_ASSET_E_CANCELLED) {
            return rc;
        }
        if (rc != Z8_FS_ASSET_OK) {
            if (rc != Z8_FS_ASSET_E_OPEN) {
                saw_corrupt = 1;
            }
            continue;
        }
        if (!z8_np3_table_is_valid(&cand)) {
            saw_corrupt = 1;
            continue;
        }
        if (!have_best || cand.seq >= best_seq) {
            const uint8_t *s = (const uint8_t *)&cand;
            uint8_t *d = (uint8_t *)out;
            for (size_t i = 0U; i < sizeof(cand); ++i) {
                d[i] = s[i];
            }
            best_seq = cand.seq;
            have_best = 1;
        }
    }

    if (have_best) {
        return Z8_FS_ASSET_OK;
    }
    return saw_corrupt ? Z8_FS_ASSET_E_VALIDATE : Z8_FS_ASSET_E_OPEN;
}
