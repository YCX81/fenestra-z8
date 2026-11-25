/* SPDX-License-Identifier: MIT
 * Z8 C3.11 / V71.7 platform Class 0x24 allocator (incl. XM6 512 MiB spacer retry),
 * D-cache maintenance, and safe RAM reader.
 * Freestanding C11, scalar-only (-mgeneral-regs-only), zero .data/.bss.
 */
#ifndef Z8_NATIVE_PHOTO_PLATFORM_H
#define Z8_NATIVE_PHOTO_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include "z8_native_filmsim_photo.h"

/* 512 MiB (1 << 29) CEVA-XM6 Core 0 IOMMU window boundary check.
 * Returns 1 if [dma, dma + actual - 1] straddles a 512 MiB window boundary
 * or is invalid (actual == 0 or arithmetic overflow). */
static inline int z8_fs_plat_span_straddles_512m(uint64_t dma, uint32_t actual)
{
    if (!actual) {
        return 1;
    }
    if (dma > UINT64_MAX - (uint64_t)(actual - 1u)) {
        return 1;
    }
    return (dma >> 29) != ((dma + (uint64_t)(actual - 1u)) >> 29);
}

/* Optional injectable ops for host unit testing of the 512 MiB spacer retry logic.
 * Pass NULL to use the concrete Z8 C3.11 bare-metal Class 0x24 allocator. */
typedef struct {
    int  (*alloc_raw)(void *ctx, uint32_t requested, struct z8_nfsp_allocation *out);
    void (*release_raw)(void *ctx, struct z8_nfsp_allocation *blk);
    void (*on_spacer)(void *ctx, uint64_t spacer_dma, uint32_t spacer_actual,
                      uint64_t fresh_dma, int ok);
    void *ctx;
} Z8Class24SpacerOps;

/* Maximum single-block size eligible for 2x transient 512 MiB spacer retry.
 * Covers all XM6 arenas (ext_ws 512 KiB, rgblut <=2.7 MiB, diag 4.0 MiB, Live 5.37 MiB)
 * while preventing 45MP still blocks (45-94 MiB) from doubling Selector 0 peak to 188 MiB. */
#define Z8_FS_XM6_SPACER_MAX_BYTES   UINT32_C(0x01800000) /* 24 MiB */

/* Class 0x24 allocator guaranteed not to straddle a 512 MiB XM6 IOMMU boundary:
 * If the first allocation straddles ((dma >> 29) != ((dma + actual - 1) >> 29)),
 * holds the straddling block as a spacer (only when requested <= Z8_FS_XM6_SPACER_MAX_BYTES),
 * allocates a fresh non-straddling block, frees the spacer, and logs FX_Z8_FLIGHT_XM6_SPACER. */
int z8_fs_plat_alloc_class24_xm6_ops(uint32_t requested,
                                     struct z8_nfsp_allocation *out,
                                     const Z8Class24SpacerOps *ops);
int z8_fs_plat_alloc_class24_xm6(uint32_t requested, struct z8_nfsp_allocation *out);

#endif /* Z8_NATIVE_PHOTO_PLATFORM_H */
