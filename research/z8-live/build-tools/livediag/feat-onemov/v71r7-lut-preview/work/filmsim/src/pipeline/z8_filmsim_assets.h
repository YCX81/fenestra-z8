/* SPDX-License-Identifier: MIT
 * Freestanding Card Asset Loader for Film Simulation Runtime.
 * Loads on demand (zero allocation while Dormant):
 *   - <drive>:\FENESTRA\ZF_GRAIN.BIN (27,588 B ZFG1 dual 83x83 maps, CRC32 checked)
 * Zero malloc, zero writable .data/.bss, scalar-only (-mgeneral-regs-only).
 */
#ifndef Z8_FILMSIM_ASSETS_H
#define Z8_FILMSIM_ASSETS_H

#include <stddef.h>
#include <stdint.h>
#include "z8_filmsim_abi.h"

#define Z8_FILMSIM_ASSET_PATH_MAX 48U
#define Z8_FILMSIM_IO_CHUNK_BYTES 65536U

enum z8_filmsim_asset_err {
    Z8_FS_ASSET_OK          =  0,
    Z8_FS_ASSET_E_ARG       = -1,
    Z8_FS_ASSET_E_OPEN      = -2,
    Z8_FS_ASSET_E_READ      = -3,
    Z8_FS_ASSET_E_SIZE      = -4,
    Z8_FS_ASSET_E_VALIDATE  = -5,
    Z8_FS_ASSET_E_CANCELLED = -6
};

/* Minimal freestanding file callback table (maps 1:1 to live_file_ops on Z8
 * and POSIX/in-memory fixtures on Host ASan/UBSan & Unicorn tests). */
typedef struct {
    void *opaque;
    int (*open_read)(void *opaque, const char *path, int *fd_out);
    int (*read)(void *opaque, int fd, void *buf, size_t bytes, size_t *got_out);
    int (*close)(void *opaque, int fd);
    int (*cancelled)(void *opaque);
} z8_filmsim_file_ops;

/* Format "<drive>:\FENESTRA\ZF_GRAIN.BIN" (drive 'A' or 'B'). */
int z8_filmsim_format_zf_grain_path(char drive, char *out, size_t cap);

/* Read exact-sized binary file in bounded <= 64 KiB chunks; rejects short or
 * oversized files (probes 1 extra byte after expected_bytes). */
int z8_filmsim_read_exact_file(const z8_filmsim_file_ops *ops,
                               const char *path,
                               void *dst, size_t expected_bytes);

/* Opaque aligned storage box holding a validated z8_zf_grain_maps descriptor
 * without exposing z8_zf_grain.h internals to unrelated EL1 translation units. */
typedef struct {
    uint64_t storage[8];
} z8_filmsim_zf_maps_box;

/* Load and CRC32-validate <drive>:\FENESTRA\ZF_GRAIN.BIN into caller's aligned
 * buffer and bind *zf_maps_out (z8_zf_grain_maps* inside z8_filmsim_zf_maps_box). */
int z8_filmsim_load_zf_grain_bin(const z8_filmsim_file_ops *ops,
                                 char drive,
                                 void *buf, size_t buf_bytes,
                                 z8_filmsim_zf_maps_box *zf_maps_out);

/* Return exact required buffer size in bytes for ZF_GRAIN.BIN. */
size_t z8_filmsim_zf_grain_bin_bytes(void);

/* Format "<drive>:\FENESTRA\FLMREC{0,1}.CFG" (drive 'A' or 'B', slot01 in {0,1}). */
int z8_filmsim_format_recipe_path(char drive, uint32_t slot01, char *out, size_t cap);

/* Load and CRC32-validate dual-slot <drive>:\FENESTRA\FLMREC0.CFG / FLMREC1.CFG,
 * selecting the valid slot with highest monotonic seq. */
int z8_filmsim_load_recipe_table(const z8_filmsim_file_ops *ops,
                                 char drive,
                                 Z8FilmSimRecipeTable *out);

#endif /* Z8_FILMSIM_ASSETS_H */
