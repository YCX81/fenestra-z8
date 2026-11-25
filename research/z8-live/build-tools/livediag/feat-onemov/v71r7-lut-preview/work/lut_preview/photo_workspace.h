/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Display Layer 0 WYSIWYG Contiguous 14.55 MiB (0xE8C000) Class 0x24 Workspace.
 *
 * Adapted from Fenestra-Z8-C311-source (modules/lut_preview/photo_workspace.h) for
 * V71.7 static unified EL1 integration:
 *   - Single contiguous 14.55 MiB (0xE8C000) Class 0x24 block:
 *       0x000000..0x1C2000 : Stage bank (1.76 MiB; reused as 512 KiB HDF scratch after copy)
 *       0x1C2000..0x5C2000 : 4 MiB tagged YCbCr 3D LUT cache (0x400000)
 *       0x5C2000..0xE8C000 : 5x NV12 output banks (5 * 0x1C2000)
 *   - Optional 512 MiB window-straddle spacer retry for XM6 Core 0 LCD HDF (Slot 41).
 *   - Lazy allocate ONLY when active; 0 B Class 0x24 held when dormant or pool-suspended.
 *   - Freestanding C11, scalar-only (-mgeneral-regs-only), ZERO .data/.bss in ROM.
 */
#ifndef V71R7_PHOTO_WORKSPACE_H
#define V71R7_PHOTO_WORKSPACE_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

enum {
    PHOTO_WORKSPACE_BYTES = 11U * 1024U * 1024U,
    PHOTO_WORKSPACE_MAX_BYTES = 16U * 1024U * 1024U,
    PHOTO_WORKSPACE_PREVIEW_BANK_BYTES = 1280U * 960U * 3U / 2U,      /* 0x1C2000 (1,843,200 B) */
    PHOTO_WORKSPACE_PREVIEW_CACHE_BYTES = 4U * 1024U * 1024U,         /* 0x400000 (4,194,304 B) */
    PHOTO_WORKSPACE_PREVIEW_BANKS = 5U,
    PHOTO_WORKSPACE_PREVIEW_BYTES = (1U + PHOTO_WORKSPACE_PREVIEW_BANKS) *
                                    PHOTO_WORKSPACE_PREVIEW_BANK_BYTES +
                                    PHOTO_WORKSPACE_PREVIEW_CACHE_BYTES, /* 0xE8C000 (14.55 MiB) */
    PHOTO_WORKSPACE_DCACHE_CHUNK = 512U * 1024U                       /* 0x80000 <= 0x88000 guard */
};

_Static_assert(PHOTO_WORKSPACE_PREVIEW_BANK_BYTES == 0x1C2000U, "bank size 0x1C2000");
_Static_assert(PHOTO_WORKSPACE_PREVIEW_BYTES == 0xE8C000U, "workspace size 0xE8C000 (14.55 MiB)");
_Static_assert(PHOTO_WORKSPACE_PREVIEW_BYTES <= PHOTO_WORKSPACE_MAX_BYTES, "within 16 MiB cap");

enum PhotoWorkspaceStatus {
    PHOTO_WS_OK        =  1,
    PHOTO_WS_ARGUMENT  = -1,
    PHOTO_WS_BUSY      = -2,
    PHOTO_WS_MEMORY    = -3,
    PHOTO_WS_MAPPING   = -4,
    PHOTO_WS_RETAINED  = -5
};

enum PhotoWorkspaceFlags {
    PHOTO_WS_FLAG_NONE           = 0U,
    PHOTO_WS_FLAG_XM6_512M_GUARD = (1U << 0) /* Retry via spacer if block straddles 512 MiB window */
};

/* Heap-embedded bookkeeping inside Layer0Ctx (ZERO globals in ROM).
 * Keep alive until photo_workspace_release() returns PHOTO_WS_OK. */
typedef struct {
    _Atomic uint32_t state;
    uint32_t bytes;
    uint32_t xm6_window_ok;   /* 1 if entire raw span lies within one 512 MiB XM6 IOMMU window */
    uint32_t spacer_retries;  /* Cumulative 512 MiB straddle spacer retries */
    uintptr_t handle;
    uintptr_t raw;            /* Uncached raw DMA base (512B aligned) */
    uintptr_t cpu;            /* Cached CPU view base (raw + 0x400000000 on Z8) */
} PhotoWorkspace;

typedef struct {
    uint8_t  *cpu;
    uintptr_t raw;
    uint32_t  bytes;
    uint32_t  xm6_window_ok;
} PhotoWorkspaceView;

/* Acquire contiguous Class 0x24 workspace. Pass PHOTO_WS_FLAG_XM6_512M_GUARD when
 * XM6 Core 0 LCD HDF is active to avoid 512 MiB IOMMU window straddling via spacer retry. */
int photo_workspace_acquire_ex(PhotoWorkspace *ws, uint32_t bytes, uint32_t flags);
int photo_workspace_acquire(PhotoWorkspace *ws, uint32_t bytes);

int photo_workspace_pin(PhotoWorkspace *ws, PhotoWorkspaceView *view);
int photo_workspace_unpin(PhotoWorkspace *ws);
int photo_workspace_release(PhotoWorkspace *ws);

/* Non-blocking inspection helpers. */
int photo_workspace_is_allocated(const PhotoWorkspace *ws);
int photo_workspace_is_quarantined(const PhotoWorkspace *ws);

/* Chunked <= 512 KiB cache maintenance helpers (0x40D2B9E4 / 0x40D2B95C + dsb sy). */
void photo_workspace_dcache_clean_chunked(uintptr_t cpu_addr, uint32_t bytes);
void photo_workspace_dcache_invalidate_chunked(uintptr_t cpu_addr, uint32_t bytes);

#ifdef V71R7_HOST_TEST
/* Host ASan+UBSan mock hooks implemented in test_layer0_host.c (zero globals in library). */
int       v71r7_host_ws_config_lock(void);
void      v71r7_host_ws_config_unlock(void);
int       v71r7_host_ws_pool_valid(void);
uintptr_t v71r7_host_ws_alloc(uint32_t a, uint32_t b, uint32_t bytes);
uint32_t  v71r7_host_ws_free(uintptr_t handle);
int       v71r7_host_ws_inspect_handle(uintptr_t handle, uint32_t *cls_out,
                                       uintptr_t *owner_out, uintptr_t *owner_back_out,
                                       uintptr_t *base_out, uint32_t *capacity_out);
uintptr_t v71r7_host_ws_to_cpu(uintptr_t raw);
uintptr_t v71r7_host_ws_to_raw(uintptr_t cpu);
uint32_t  v71r7_host_ws_cacheable(uintptr_t addr);
int       v71r7_host_ws_one_region(uintptr_t start, uintptr_t end, uint32_t kind);
int       v71r7_host_ws_in_class24(uintptr_t base, uint32_t capacity);
uint64_t  v71r7_host_ws_dma_window_addr(uintptr_t raw);
void      v71r7_host_ws_clean(uintptr_t addr, uint32_t bytes, uint32_t mode);
void      v71r7_host_ws_invalidate(uintptr_t addr, uint32_t bytes, uint32_t mode);
int       v71r7_host_ws_verify_line(uintptr_t raw);
#endif

#endif /* V71R7_PHOTO_WORKSPACE_H */
