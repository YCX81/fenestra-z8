/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Resident Photo LUT Worker, Catalog Scanner & Watchdog Pump.
 *
 * ZERO-GLOBAL, STRICT SCALAR (-mgeneral-regs-only).
 *   - Booted via v71r7_photo_menu_boot_once() (called from live_ui_system_dispatch(ev==19),
 *     ZERO 0x402DD4F8 open hook!).
 *   - Allocates PhotoMenuCtx + 32 KiB worker stack via HEAP_ALLOC (0x40D2C408), anchored at
 *     v71r7_anchor()->menu_ctx (DRAM 0xA11640000).
 *   - Spawns CPU2 prio 30 resident worker via stock cre_tsk (0x40185064) + sta_tsk (0x401855E0).
 *   - Uses v71r7_active_card_drive() (cardslot 0xA1171B000) and hardened 0x402DD4F8/6C8/78C/558/9FC/A4C
 *     + 0x402DDC6C/D1C/D6C (ZERO missing cold-retry3 symbols!).
 *   - Restores FXPHOTO0/1.CFG at boot, streams .cube (<= 2 MiB) under FxZ8LutIoGate,
 *     calls v71r7_lut_prepare_from_buffer() & v71r7_layer0_notify_lut_changed(),
 *     pumps v71r7_layer0_worker_step(), and polls v71r7_watchdog_poll() every ~100ms.
 */
#include "photo_menu.h"
#include "lut_runtime.h"
#include "../lut_preview/layer0_provider.h"

#define FN(address, type) ((type)(uintptr_t)UINT64_C(address))
#define ROM_CARD_OPEN   FN(0x402dd4f8, int32_t  (*)(const char *, uint32_t, uint32_t))
#define ROM_CARD_CLOSE  FN(0x402dd558, uint32_t (*)(int32_t))
#define ROM_CARD_READ   FN(0x402dd6c8, uint32_t (*)(int32_t, void *, uint32_t, uint32_t *))
#define ROM_CARD_WRITE  FN(0x402dd78c, uint32_t (*)(int32_t, const void *, uint32_t, uint32_t *, void *))
#define ROM_CARD_RENAME FN(0x402dd9fc, uint32_t (*)(const char *, const char *))
#define ROM_CARD_REMOVE FN(0x402dda4c, uint32_t (*)(const char *))
#define ROM_DIR_OPEN    FN(0x402ddc6c, uint32_t (*)(const char *, int32_t *))
#define ROM_DIR_NEXT    FN(0x402ddd1c, uint32_t (*)(int32_t, void *))
#define ROM_DIR_CLOSE   FN(0x402ddd6c, uint32_t (*)(int32_t))
#define ROM_HEAP_ALLOC  FN(0x40d2c408, void *(*)(uint32_t, uint32_t))
#define ROM_HEAP_FREE   FN(0x40d2c448, void (*)(void *))
#define ROM_CRE_TSK     FN(0x40185064, int32_t (*)(const void *))
#define ROM_STA_TSK     FN(0x401855e0, int32_t (*)(int32_t))
#define ROM_DLY_TSK     FN(0x40187b00, int32_t (*)(uint32_t))

typedef struct {
    uint32_t  attributes;
    uint32_t  reserved;
    uintptr_t context;
    uintptr_t entry;
    uint32_t  priority;
    uint32_t  reserved2;
    uint64_t  stack_bytes;
    uintptr_t stack;
    uint32_t  processor;
    uint32_t  affinity;
} PhotoRtosTaskRecord;
_Static_assert(sizeof(PhotoRtosTaskRecord) == 56u, "C3.11 task ABI");
_Static_assert(offsetof(PhotoRtosTaskRecord, processor) == 48u, "processor ABI");
_Static_assert(offsetof(PhotoMenuCtx, worker.entry_buffer) % 64u == 0u, "entry_buffer DMA align");
_Static_assert(offsetof(PhotoMenuCtx, worker.write_buffer) % 64u == 0u, "write_buffer DMA align");

enum {
    MODE_PARKED = 0u,
    MODE_ARMED = 1u,
    MODE_DRIVE_A = 2u,
    MODE_DRIVE_B = 3u,
    MODE_WORKER_FAILED = 4u,
    MODE_MASK = 7u,
    FLAG_WORKER_BUSY = 8u,
    FLAG_GUI_OPEN = 16u,
    FLAG_GUI_EARLY_OFF = 32u
};

enum {
    JOB_NONE = 0u,
    JOB_DRAIN = 1u,
    JOB_OPEN_CUBE = 2u,
    JOB_READ_CUBE = 3u,
    JOB_PREPARE = 4u,
    JOB_SAVE = 5u,
    JOB_OUTPUT = 6u,
    JOB_CLEAN_ERROR = 7u
};

enum {
    PAGE_OPEN = 1u,
    PAGE_PREVIOUS = 2u,
    PAGE_NEXT = 3u,
    PAGE_ENTER = 4u,
    PAGE_BACK = 5u,
    PAGE_CLOSE = 6u,
    PAGE_OP_MASK = 7u
};

enum {
    SCAN_NONE = 0u,
    SCAN_ROOT_FOLDERS = 1u,
    SCAN_ROOT_FIND_FLAT = 2u,
    SCAN_FOLDER_FILES = 3u
};

enum {
    FILE_RETAINED = 1u,
    DIRECTORY_RETAINED = 2u
};

enum {
    NATIVE_ENOENT = 0x80000010u,
    NATIVE_DIR_EOF = 0x80000030u,
    NATIVE_CARDSLOT_REFUSED = 0x8000ff01u
};

/* ============================================================================
 * 1. Platform Abstraction Wrappers (ROM on EL1 AArch64, PlatOps on Host Test)
 * ============================================================================ */
static void *w_heap_alloc(PhotoMenuCtx *ctx, uint32_t bytes, uint32_t align_hint) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->heap_alloc)
        return ctx->plat->heap_alloc(ctx->plat->user, bytes, align_hint);
    return NULL;
#else
    (void)ctx;
    (void)align_hint;
    /* Z8 0x40D2C408 takes (size, pool_flags=0), never alignment in arg2. */
    return ROM_HEAP_ALLOC(bytes, 0u);
#endif
}

static void w_heap_free(PhotoMenuCtx *ctx, void *ptr) {
    if (!ptr) return;
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->heap_free)
        ctx->plat->heap_free(ctx->plat->user, ptr);
#else
    (void)ctx;
    ROM_HEAP_FREE(ptr);
#endif
}

static int w_card_drive_ready(PhotoMenuCtx *ctx) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->card_drive_ready)
        return ctx->plat->card_drive_ready(ctx->plat->user);
    return 1;
#else
    (void)ctx;
    const volatile uint32_t *cs = (const volatile uint32_t *)(uintptr_t)V71R7_CARDSLOT_STATE_VA;
    if (cs[0] == V71R7_CARDSLOT_MAGIC && cs[1] == 0u) return 0;
    return 1;
#endif
}

static char w_card_drive(PhotoMenuCtx *ctx) {
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_CARD_IO, 0u, FX_Z8_HB_CARD_DRIVE);
    char drive;
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->card_drive)
        drive = ctx->plat->card_drive(ctx->plat->user);
    else
        drive = v71r7_active_card_drive();
#else
    (void)ctx;
    drive = v71r7_active_card_drive();
#endif
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_STEP, 0u, ctx ? ctx->worker.job : 0u);
    return drive;
}

static int32_t w_card_open(PhotoMenuCtx *ctx, const char *path, uint32_t flags, uint32_t mode) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->file_open)
        return ctx->plat->file_open(ctx->plat->user, path, flags, mode);
    return (int32_t)NATIVE_ENOENT;
#else
    (void)ctx;
    return ROM_CARD_OPEN(path, flags, mode);
#endif
}

static uint32_t w_card_read(PhotoMenuCtx *ctx, int32_t fd, void *dst, uint32_t bytes, uint32_t *done) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->file_read)
        return ctx->plat->file_read(ctx->plat->user, fd, dst, bytes, done);
    if (done) *done = 0u;
    return 1u;
#else
    (void)ctx;
    return ROM_CARD_READ(fd, dst, bytes, done);
#endif
}

static uint32_t w_card_write(PhotoMenuCtx *ctx, int32_t fd, const void *src, uint32_t bytes, uint32_t *done) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->file_write)
        return ctx->plat->file_write(ctx->plat->user, fd, src, bytes, done, NULL);
    if (done) *done = 0u;
    return 1u;
#else
    (void)ctx;
    /* Clean caller-owned 64B cache lines before DMA write (belt-and-suspenders
     * alongside cardslot_v71r4 hook_write). Strictly scalar inline asm. */
    uintptr_t p = (uintptr_t)src & ~(uintptr_t)63u;
    uintptr_t end = (uintptr_t)src + bytes;
    for (; p < end; p += 64u)
        __asm__ volatile("dc cvac, %0" : : "r"(p) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
    return ROM_CARD_WRITE(fd, src, bytes, done, NULL);
#endif
}

static uint32_t w_card_close(PhotoMenuCtx *ctx, int32_t fd) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->file_close)
        return ctx->plat->file_close(ctx->plat->user, fd);
    return 0u;
#else
    (void)ctx;
    return ROM_CARD_CLOSE(fd);
#endif
}

static uint32_t w_card_remove(PhotoMenuCtx *ctx, const char *path) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->file_remove)
        return ctx->plat->file_remove(ctx->plat->user, path);
    return 1u;
#else
    (void)ctx;
    return ROM_CARD_REMOVE(path);
#endif
}

static uint32_t w_card_rename(PhotoMenuCtx *ctx, const char *from, const char *to) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->file_rename)
        return ctx->plat->file_rename(ctx->plat->user, from, to);
    return 1u;
#else
    (void)ctx;
    return ROM_CARD_RENAME(from, to);
#endif
}

static uint32_t w_dir_open(PhotoMenuCtx *ctx, const char *path, int32_t *fd) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->dir_open)
        return ctx->plat->dir_open(ctx->plat->user, path, fd);
    *fd = -1;
    return NATIVE_ENOENT;
#else
    (void)ctx;
    return ROM_DIR_OPEN(path, fd);
#endif
}

static uint32_t w_dir_next(PhotoMenuCtx *ctx, int32_t fd, void *buf) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->dir_next)
        return ctx->plat->dir_next(ctx->plat->user, fd, buf);
    return NATIVE_DIR_EOF;
#else
    (void)ctx;
    return ROM_DIR_NEXT(fd, buf);
#endif
}

static uint32_t w_dir_close(PhotoMenuCtx *ctx, int32_t fd) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->dir_close)
        return ctx->plat->dir_close(ctx->plat->user, fd);
    return 0u;
#else
    (void)ctx;
    return ROM_DIR_CLOSE(fd);
#endif
}

static int32_t w_task_delay(PhotoMenuCtx *ctx, uint32_t ms) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->task_delay)
        return ctx->plat->task_delay(ctx->plat->user, ms);
    return 0;
#else
    (void)ctx;
    return ROM_DLY_TSK(ms);
#endif
}

static uint64_t w_now_ticks(PhotoMenuCtx *ctx) {
#ifdef V71R7_HOST_TEST
    if (ctx && ctx->plat && ctx->plat->now_ticks)
        return ctx->plat->now_ticks(ctx->plat->user);
    return 0u;
#else
    (void)ctx;
    uint64_t t;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
    return t;
#endif
}

/* ============================================================================
 * 2. Atomic Seqlock Helpers & Output Permit Management
 * ============================================================================ */
static uint32_t state_mode(uint64_t state) {
    return (uint32_t)state & MODE_MASK;
}

static char state_drive(uint64_t state) {
    uint32_t m = state_mode(state);
    return m == MODE_DRIVE_A ? 'A' : (m == MODE_DRIVE_B ? 'B' : 0);
}

static char live_drive(PhotoMenuCtx *ctx) {
    return state_drive(atomic_load(&ctx->worker.activity));
}

static uint32_t permit_load(void) {
    return __atomic_load_n(&v71r7_anchor()->output_permit, __ATOMIC_ACQUIRE);
}

static int next_output(uint32_t old, uint32_t *next) {
    *next = (old & ~1u) + 2u;
    return *next != 0u;
}

static uint32_t close_output(void) {
    uint32_t *slot = &v71r7_anchor()->output_permit;
    uint32_t old = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    uint32_t next;
    do {
        if (!next_output(old, &next)) next = old & ~1u;
    } while (!__atomic_compare_exchange_n(slot, &old, next, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return (old & ~1u) == next ? 0u : (next | 1u);
}

static void publish_seq(_Atomic uint32_t *sequence, _Atomic uint32_t *words,
                        const void *data, size_t bytes) {
    atomic_fetch_add(sequence, 1u);
    const uint8_t *p = (const uint8_t *)data;
    size_t offset = 0;
    for (; offset + sizeof(uint32_t) <= bytes; offset += sizeof(uint32_t)) {
        uint32_t word;
        memcpy(&word, p + offset, sizeof(word));
        atomic_store(&words[offset / sizeof(uint32_t)], word);
    }
    if (offset < bytes) {
        uint32_t word = 0u;
        memcpy(&word, p + offset, bytes - offset);
        atomic_store(&words[offset / sizeof(uint32_t)], word);
    }
    atomic_fetch_add(sequence, 1u);
}

static int snapshot_seq(const _Atomic uint32_t *sequence, const _Atomic uint32_t *words,
                        void *data, size_t bytes) {
    uint8_t *p = (uint8_t *)data;
    uint32_t before = atomic_load(sequence);
    if (!before || (before & 1u)) return 0;
    size_t offset = 0;
    for (; offset + sizeof(uint32_t) <= bytes; offset += sizeof(uint32_t)) {
        uint32_t word = atomic_load(&words[offset / sizeof(uint32_t)]);
        memcpy(p + offset, &word, sizeof(word));
    }
    if (offset < bytes) {
        uint32_t word = atomic_load(&words[offset / sizeof(uint32_t)]);
        memcpy(p + offset, &word, bytes - offset);
    }
    return atomic_load(sequence) == before;
}

static void publish_status(PhotoMenuCtx *ctx, PhotoPhase phase, FxResult reason,
                           uint32_t done, uint32_t total) {
    const PhotoStatus report = { ctx->worker.ticket, (uint32_t)phase, (uint32_t)reason, done, total };
    publish_seq(&ctx->worker.status_seq, ctx->worker.status_words, &report, sizeof(report));
}

/* ============================================================================
 * 3. Public GUI-Side Query & Command APIs
 * ============================================================================ */
uint64_t photo_gui_epoch(void) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx) return 0u;
    uint64_t state = atomic_load(&ctx->worker.activity);
    if ((state & (FLAG_GUI_OPEN | FLAG_GUI_EARLY_OFF)) != FLAG_GUI_OPEN) return 0u;
    uint32_t io_epoch = fx_z8_lut_io_epoch(v71r7_io_gate());
    return io_epoch ? ((state & (UINT64_MAX << 32)) | io_epoch) : 0u;
}

PhotoStatus photo_lut_status(void) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    uint32_t ticket = permit_load() | 1u;
    if (!ctx) return (PhotoStatus){ .ticket = ticket, .phase = PHOTO_NO_LUT };
    uint32_t m = state_mode(atomic_load(&ctx->worker.activity));
    if (m == MODE_WORKER_FAILED)
        return (PhotoStatus){ .ticket = ticket, .phase = PHOTO_WORKER_ERROR };
    if (m == MODE_PARKED)
        return (PhotoStatus){ .ticket = ticket, .phase = PHOTO_NO_LUT };
    PhotoStatus out;
    if (snapshot_seq(&ctx->worker.status_seq, ctx->worker.status_words, &out, sizeof(out)) &&
        out.ticket == ticket)
        return out;
    return (PhotoStatus){ .ticket = ticket, .phase = PHOTO_QUEUED };
}

uint32_t photo_lut_select(const char *name, uint64_t gui_epoch) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx || !name || (name[0] && !fx_z8_lut_path(name))) return 0u;
    FxZ8LutIoGate *gate = v71r7_io_gate();
    uint64_t state = atomic_load(&ctx->worker.activity) & ~((uint64_t)FLAG_WORKER_BUSY);
    uint32_t epoch = fx_z8_lut_io_epoch(gate);
    if (!(state & FLAG_GUI_OPEN) || !state_drive(state) || !epoch ||
        ((state & (UINT64_MAX << 32)) | epoch) != gui_epoch ||
        epoch != atomic_load(&ctx->worker.bound_io_epoch) ||
        atomic_load(&ctx->worker.media_unavailable))
        return 0u;

    uint32_t *permit_slot = &v71r7_anchor()->output_permit;
    uint32_t old = __atomic_load_n(permit_slot, __ATOMIC_ACQUIRE);
    uint32_t closed;
    if (!next_output(old, &closed) ||
        (atomic_load(&ctx->worker.activity) & ~((uint64_t)FLAG_WORKER_BUSY)) != state ||
        fx_z8_lut_io_epoch(gate) != epoch ||
        epoch != atomic_load(&ctx->worker.bound_io_epoch) ||
        atomic_load(&ctx->worker.media_unavailable) ||
        !__atomic_compare_exchange_n(permit_slot, &old, closed, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0u;

    PhotoSelection next;
    memset(&next, 0, sizeof(next));
    next.ticket = closed | 1u;
    for (unsigned i = 0; i + 1u < sizeof(next.name) && name[i]; ++i)
        next.name[i] = name[i];
    publish_seq(&ctx->worker.selection_seq, ctx->worker.selection_words, &next, sizeof(next));
    return next.ticket;
}

static void request_page(PhotoMenuCtx *ctx, uint32_t op, const char *category, uint32_t focus) {
    if (!ctx) return;
    uint32_t previous = atomic_load(&ctx->worker.page_request) & ~PAGE_OP_MASK;
    PhotoPageCommand command;
    memset(&command, 0, sizeof(command));
    command.epoch = photo_gui_epoch();
    command.serial = previous + PAGE_OP_MASK + 1u + op;
    command.root_focus = focus;
    if (category) {
        for (size_t i = 0; i + 1u < sizeof(command.category) && category[i]; ++i)
            command.category[i] = category[i];
    }
    publish_seq(&ctx->worker.page_command_seq, ctx->worker.page_command_words, &command, sizeof(command));
    atomic_store(&ctx->worker.page_request, command.serial);
}

void photo_lut_request_page(int32_t direction) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    uint32_t op = direction < 0 ? PAGE_PREVIOUS : (direction > 0 ? PAGE_NEXT : PAGE_OPEN);
    request_page(ctx, op, NULL, 0u);
}

void photo_lut_enter_folder(const char *category, uint32_t root_focus) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (ctx && category &&
        (fx_z8_lut_category(category) || !photo_name_compare(category, PHOTO_FLAT_FOLDER)))
        request_page(ctx, PAGE_ENTER, category, root_focus);
}

void photo_lut_leave_folder(uint32_t root_focus) {
    request_page(photo_menu_ctx(), PAGE_BACK, NULL, root_focus);
}

void photo_lut_close_browser(void) {
    request_page(photo_menu_ctx(), PAGE_CLOSE, NULL, 0u);
}

int photo_lut_page(PhotoPage *out) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx || !out) return 0;
    uint32_t epoch = atomic_load(&ctx->worker.restore_ticket);
    FxZ8LutIoGate *gate = v71r7_io_gate();
    if (!live_drive(ctx) || !photo_gui_epoch() ||
        atomic_load(&ctx->worker.bound_io_epoch) != fx_z8_lut_io_epoch(gate) ||
        !snapshot_seq(&ctx->worker.page_seq, ctx->worker.page_words, out, sizeof(*out)))
        return 0;
    uint32_t request = atomic_load(&ctx->worker.page_request);
    return (request & PAGE_OP_MASK) != PAGE_CLOSE &&
           out->epoch == epoch &&
           epoch == atomic_load(&ctx->worker.restore_ticket) &&
           out->serial == request;
}

void v71r7_photo_menu_gui_poll(void) {
    photo_ui_deactivate();
}

/* ============================================================================
 * 4. Card I/O Callbacks Under FxZ8LutIoGate (No cold-retry3 Symbols!)
 * ============================================================================ */
static int storage_current(PhotoMenuCtx *ctx) {
    PhotoWorkerState *w = &ctx->worker;
    return w->io_epoch &&
           fx_z8_lut_io_epoch(v71r7_io_gate()) == w->io_epoch &&
           live_drive(ctx) == w->drive &&
           w_card_drive(ctx) == w->drive &&
           atomic_load(&w->card_change_seq) == w->seen_card_seq;
}

static int work_current(void *context) {
    PhotoMenuCtx *ctx = (PhotoMenuCtx *)context;
    return (permit_load() | 1u) == ctx->worker.ticket && storage_current(ctx);
}

static int io_admit(PhotoMenuCtx *ctx, int journal) {
    FxZ8LutIoGate *gate = v71r7_io_gate();
    if (!storage_current(ctx) || !fx_z8_lut_io_take(gate, ctx->worker.io_epoch)) return 0;
    if (journal ? work_current(ctx) : storage_current(ctx)) return 1;
    fx_z8_lut_io_drop(gate);
    return 0;
}

static FxResult finish_io(PhotoMenuCtx *ctx, uint32_t native_error, int read_op) {
    fx_z8_lut_io_drop(v71r7_io_gate());
    if (!native_error) return FX_OK;
    int mount_lost = (native_error == UINT32_C(0x80001005)) ||
                     (native_error == UINT32_C(0x80001003)) ||
                     (native_error == UINT32_C(0x88000105)) ||
                     (native_error == NATIVE_CARDSLOT_REFUSED);
    if ((read_op || mount_lost) && storage_current(ctx)) ctx->worker.media_fault = 1u;
    return FX_IO;
}

static int owns_handle(PhotoMenuCtx *ctx) {
    PhotoWorkerState *w = &ctx->worker;
    FxZ8LutIoGate *gate = v71r7_io_gate();
    return w->io_epoch &&
           w_card_drive(ctx) == w->drive &&
           (__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) >> 32) == w->io_epoch;
}

static void retain_handle(PhotoMenuCtx *ctx, int32_t handle, uint32_t kind) {
    PhotoWorkerState *w = &ctx->worker;
    PhotoRetainedHandle *slot = (kind == FILE_RETAINED) ? &w->file_retained : &w->directory_retained;
    *slot = (PhotoRetainedHandle){ handle, w->io_epoch, w->drive };
    atomic_fetch_or(&w->retained, kind);
    w->media_fault = 1u;
}

static FxResult file_open_cb(void *context, const char *path, uint32_t flags, int32_t *fd) {
    PhotoMenuCtx *ctx = (PhotoMenuCtx *)context;
    *fd = -1;
    if (atomic_load(&ctx->worker.retained) & FILE_RETAINED) return FX_CLOSE;
    if (!io_admit(ctx, 1)) return FX_STALE;
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_CARD_IO, 0u, FX_Z8_HB_CARD_OPEN);
    *fd = w_card_open(ctx, path, flags, 0u);
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_STEP, 0u, ctx->worker.job);
    if ((uint32_t)*fd == NATIVE_ENOENT) {
        fx_z8_lut_io_drop(v71r7_io_gate());
        return FX_EMPTY;
    }
    return *fd < 0 ? finish_io(ctx, (uint32_t)*fd, flags == 0u) : FX_OK;
}

static FxResult file_read_cb(void *context, int32_t fd, void *dst, uint32_t bytes, uint32_t *done) {
    PhotoMenuCtx *ctx = (PhotoMenuCtx *)context;
    if ((uintptr_t)dst & 63u) return FX_ARGUMENT;
    if (!io_admit(ctx, 1)) return FX_STALE;
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_CARD_IO, 0u, FX_Z8_HB_CARD_READ);
    uint32_t rc = w_card_read(ctx, fd, dst, bytes, done);
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_STEP, 0u, ctx->worker.job);
    return finish_io(ctx, rc, 1);
}

static FxResult file_write_cb(void *context, int32_t fd, const void *src, uint32_t bytes, uint32_t *done) {
    PhotoMenuCtx *ctx = (PhotoMenuCtx *)context;
    if (bytes != PHOTO_RECORD_BYTES) return FX_ARGUMENT;
    memcpy(ctx->worker.write_buffer, src, bytes);
    if (!io_admit(ctx, 1)) return FX_STALE;
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_CARD_IO, 0u, FX_Z8_HB_CARD_WRITE);
    uint32_t rc = w_card_write(ctx, fd, ctx->worker.write_buffer, bytes, done);
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_STEP, 0u, ctx->worker.job);
    return finish_io(ctx, rc, 0);
}

static FxResult file_close_cb(void *context, int32_t fd) {
    PhotoMenuCtx *ctx = (PhotoMenuCtx *)context;
    int closed = 0;
    if (owns_handle(ctx)) {
        v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_CARD_IO, 0u, FX_Z8_HB_CARD_CLOSE);
        closed = (w_card_close(ctx, fd) == 0u);
        v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_STEP, 0u, ctx->worker.job);
    }
    fx_z8_lut_io_drop(v71r7_io_gate());
    if (closed) return FX_OK;
    retain_handle(ctx, fd, FILE_RETAINED);
    return FX_CLOSE;
}

static FxResult file_remove_cb(void *context, const char *path) {
    PhotoMenuCtx *ctx = (PhotoMenuCtx *)context;
    if (!io_admit(ctx, 1)) return FX_STALE;
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_CARD_IO, 0u, FX_Z8_HB_CARD_REMOVE);
    uint32_t rc = w_card_remove(ctx, path);
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_STEP, 0u, ctx->worker.job);
    return finish_io(ctx, rc, 0);
}

static FxResult file_rename_cb(void *context, const char *from, const char *to) {
    PhotoMenuCtx *ctx = (PhotoMenuCtx *)context;
    if (!io_admit(ctx, 1)) return FX_STALE;
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_CARD_IO, 0u, FX_Z8_HB_CARD_RENAME);
    uint32_t rc = w_card_rename(ctx, from, to);
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_STEP, 0u, ctx->worker.job);
    return finish_io(ctx, rc, 0);
}

static PhotoFileIO make_file_io(PhotoMenuCtx *ctx) {
    return (PhotoFileIO){
        ctx, work_current, file_open_cb, file_read_cb, file_write_cb,
        file_close_cb, file_remove_cb, file_rename_cb
    };
}

/* ============================================================================
 * 5. Non-Blocking Directory Scanner & 512-Entry Folder Cache
 * ============================================================================ */
static int close_directory(PhotoMenuCtx *ctx) {
    PhotoWorkerState *w = &ctx->worker;
    int ok = 1;
    if (atomic_load(&w->directory_owned)) {
        uint32_t rc = FX_STALE;
        if (owns_handle(ctx)) {
            v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_CARD_IO, 0u, FX_Z8_HB_DIR_CLOSE);
            rc = w_dir_close(ctx, w->directory);
            v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_STEP, 0u, w->job);
        }
        if (rc) {
            retain_handle(ctx, w->directory, DIRECTORY_RETAINED);
            ok = 0;
        }
        fx_z8_lut_io_drop(v71r7_io_gate());
        atomic_store(&w->directory_owned, 0u);
    }
    return ok;
}

static void publish_page(PhotoMenuCtx *ctx, uint32_t request) {
    PhotoWorkerState *w = &ctx->worker;
    w->page.serial = request;
    w->page.epoch = w->epoch;
    publish_seq(&w->page_seq, w->page_words, &w->page, sizeof(w->page));
}

static FxResult open_directory(PhotoMenuCtx *ctx) {
    PhotoWorkerState *w = &ctx->worker;
    if ((atomic_load(&w->retained) & DIRECTORY_RETAINED) || !io_admit(ctx, 0)) return FX_STALE;
    char path[PHOTO_PATH_BYTES];
    memcpy(path, "A:\\FENESTRA", 12u);
    if (w->scan == SCAN_ROOT_FOLDERS) {
        photo_card_path(w->drive, "LUTS", path);
    } else if (w->scan == SCAN_FOLDER_FILES && w->category[0] != PHOTO_FLAT_FOLDER[0]) {
        char relative[FX_Z8_LUT_PATH_BYTES];
        photo_folder_path(w->category, "", relative);
        photo_card_path(w->drive, relative, path);
    } else {
        path[0] = w->drive;
    }
    w->directory = -1;
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_CARD_IO, 0u, FX_Z8_HB_DIR_OPEN);
    uint32_t opened = w_dir_open(ctx, path, &w->directory);
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_STEP, 0u, w->job);
    if (opened || w->directory < 0) {
        if (opened == NATIVE_ENOENT && w->directory < 0) {
            fx_z8_lut_io_drop(v71r7_io_gate());
            return FX_EMPTY;
        }
        FxResult error = finish_io(ctx, opened ? opened : 1u, 1);
        if (w->directory >= 0) retain_handle(ctx, w->directory, DIRECTORY_RETAINED);
        return error;
    }
    atomic_store(&w->directory_owned, 1u);
    return FX_OK;
}

static void restore_from_journal(PhotoMenuCtx *ctx);

static void clear_cache(PhotoMenuCtx *ctx) {
    PhotoWorkerState *w = &ctx->worker;
    if (w->cache) w_heap_free(ctx, w->cache);
    w->cache = NULL;
    w->cache_ready = 0u;
    w->cache_count = 0u;
    w->cache_base = 0u;
}

static void cache_add(PhotoMenuCtx *ctx, const char name[FX_Z8_LUT_NAME_BYTES]) {
    PhotoWorkerState *w = &ctx->worker;
    unsigned low = 0u, high = w->cache_count;
    while (low < high) {
        unsigned middle = (low + high) / 2u;
        int order = photo_name_compare(name, w->cache[middle]);
        if (!order) {
            if (memcmp(name, w->cache[middle], FX_Z8_LUT_NAME_BYTES) < 0)
                memcpy(w->cache[middle], name, FX_Z8_LUT_NAME_BYTES);
            return;
        }
        if (order < 0) high = middle;
        else low = middle + 1u;
    }
    if (w->cache_count == PHOTO_CACHE_FILES) {
        clear_cache(ctx);
        return;
    }
    for (unsigned i = w->cache_count; i > low; --i)
        memcpy(w->cache[i], w->cache[i - 1u], FX_Z8_LUT_NAME_BYTES);
    memcpy(w->cache[low], name, FX_Z8_LUT_NAME_BYTES);
    ++w->cache_count;
}

static void cache_page(PhotoMenuCtx *ctx, int direction) {
    PhotoWorkerState *w = &ctx->worker;
    if (direction > 0) w->cache_base += PHOTO_FILES_PER_PAGE;
    if (direction < 0) w->cache_base -= PHOTO_FILES_PER_PAGE;
    uint32_t remaining = w->cache_count - w->cache_base;
    memset(&w->page, 0, sizeof(w->page));
    w->page.count = remaining < PHOTO_FILES_PER_PAGE ? remaining : PHOTO_FILES_PER_PAGE;
    w->page.previous = (w->cache_base != 0u) ? 1u : 0u;
    w->page.next = (remaining > PHOTO_FILES_PER_PAGE) ? 1u : 0u;
    if (w->page.count)
        memcpy(w->page.names, w->cache + w->cache_base, w->page.count * FX_Z8_LUT_NAME_BYTES);
}

static void scan_begin(PhotoMenuCtx *ctx, int direction) {
    PhotoWorkerState *w = &ctx->worker;
    unsigned cursor = (direction > 0 && w->page.count > 0u) ? (w->page.count - 1u) : 0u;
    photo_catalog_start(&w->catalog, direction, w->page.names[cursor]);
    w->scan = w->category[0] ? SCAN_FOLDER_FILES : SCAN_ROOT_FOLDERS;
}

static void publish_browser(PhotoMenuCtx *ctx, uint32_t request) {
    PhotoWorkerState *w = &ctx->worker;
    w->page.level = w->category[0] ? PHOTO_FOLDER : PHOTO_ROOT;
    memcpy(w->page.category, w->category, sizeof(w->category));
    publish_page(ctx, request);
}

static void scan_complete(PhotoMenuCtx *ctx, uint32_t request, int ok) {
    PhotoWorkerState *w = &ctx->worker;
    if (ok && w->scan == SCAN_ROOT_FOLDERS) {
        w->scan = SCAN_ROOT_FIND_FLAT;
        return;
    }
    w->scan = SCAN_NONE;
    if (!ok) {
        clear_cache(ctx);
        memset(&w->page, 0, sizeof(w->page));
    } else if (w->cache) {
        w->cache_ready = 1u;
        cache_page(ctx, 0);
    } else if (!w->catalog.count && w->catalog.direction) {
        scan_begin(ctx, 0);
        return;
    } else {
        photo_catalog_finish(&w->catalog, &w->page);
    }
    publish_browser(ctx, request);
    if (ok && !atomic_load(&w->retained) && atomic_exchange(&w->media_unavailable, 0u)) {
        w->job = JOB_NONE;
        restore_from_journal(ctx);
    }
}

static int accept_page_request(PhotoMenuCtx *ctx, uint32_t request) {
    PhotoWorkerState *w = &ctx->worker;
    PhotoPageCommand command;
    if (!snapshot_seq(&w->page_command_seq, w->page_command_words, &command, sizeof(command)) ||
        command.serial != request)
        return 0;
    (void)close_directory(ctx);
    w->scan = SCAN_NONE;
    w->page_seen = request;
    uint32_t op = request & PAGE_OP_MASK;
    if (op != PAGE_CLOSE && command.epoch != photo_gui_epoch()) op = PAGE_OPEN;
    switch (op) {
    case PAGE_ENTER:
        if (w->category[0] || !command.root_focus || command.root_focus > w->page.count ||
            photo_name_compare(command.category, w->page.names[command.root_focus - 1u]))
            break;
        w->root_page = w->page;
        memcpy(w->category, command.category, sizeof(w->category));
        clear_cache(ctx);
        w->cache = (char (*)[FX_Z8_LUT_NAME_BYTES])w_heap_alloc(
            ctx, PHOTO_CACHE_FILES * FX_Z8_LUT_NAME_BYTES, 0u);
        scan_begin(ctx, 0);
        return 1;
    case PAGE_BACK:
        if (w->category[0]) w->page = w->root_page;
        memset(w->category, 0, sizeof(w->category));
        clear_cache(ctx);
        w->page.focus = command.root_focus <= w->page.count ? command.root_focus : 0u;
        break;
    case PAGE_PREVIOUS:
    case PAGE_NEXT: {
        int direction = (op == PAGE_PREVIOUS) ? -1 : 1;
        if (!w->page.count || (direction < 0 ? !w->page.previous : !w->page.next)) break;
        if (w->cache_ready) {
            cache_page(ctx, direction);
            break;
        }
        scan_begin(ctx, direction);
        return 1;
    }
    case PAGE_OPEN:
    case PAGE_CLOSE:
        memset(w->category, 0, sizeof(w->category));
        clear_cache(ctx);
        if (op == PAGE_CLOSE) return 0;
        scan_begin(ctx, 0);
        return 1;
    default:
        return 0;
    }
    publish_browser(ctx, request);
    return 0;
}

static void directory_slice(PhotoMenuCtx *ctx) {
    PhotoWorkerState *w = &ctx->worker;
    uint32_t request = atomic_load(&w->page_request);
    if (request != w->page_seen && !accept_page_request(ctx, request)) return;
    for (unsigned i = 0; i < 8u && w->scan; ++i) {
        if (request != atomic_load(&w->page_request)) return;
        if (!atomic_load(&w->directory_owned)) {
            FxResult opened = open_directory(ctx);
            if (opened != FX_OK) {
                scan_complete(ctx, request, opened == FX_EMPTY);
                continue;
            }
        }
        if (request != atomic_load(&w->page_request) || !io_admit(ctx, 0)) {
            (void)close_directory(ctx);
            return;
        }
        memset(w->entry_buffer, 0, sizeof(w->entry_buffer));
        v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_CARD_IO, 0u, FX_Z8_HB_DIR_NEXT);
        uint32_t rc = w_dir_next(ctx, w->directory, w->entry_buffer);
        v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_STEP, 0u, w->job);
        int eof = (rc == NATIVE_DIR_EOF);
        (void)finish_io(ctx, eof ? 0u : rc, 1);
        if (rc) {
            scan_complete(ctx, request, close_directory(ctx) && eof);
            continue;
        }
        char name[FX_Z8_LUT_NAME_BYTES];
        int folders = (w->scan == SCAN_ROOT_FOLDERS);
        if (!photo_dir_name(w->entry_buffer, w->category, folders, name)) continue;
        if (w->scan == SCAN_ROOT_FIND_FLAT) {
            char flat[FX_Z8_LUT_NAME_BYTES];
            memset(flat, 0, sizeof(flat));
            flat[0] = PHOTO_FLAT_FOLDER[0];
            photo_catalog_add(&w->catalog, flat);
            scan_complete(ctx, request, close_directory(ctx));
        } else {
            photo_catalog_add(&w->catalog, name);
            if (w->cache) cache_add(ctx, name);
        }
    }
}

/* ============================================================================
 * 6. Streaming .cube Reader, LUT Prepare, Journal Restore/Save & Layer 0 Pump
 * ============================================================================ */
static void release_cube_stream(PhotoMenuCtx *ctx) {
    PhotoWorkerState *w = &ctx->worker;
    if (w->cube_fd_open) {
        (void)file_close_cb(ctx, w->cube_fd);
        w->cube_fd = -1;
        w->cube_fd_open = 0u;
    }
    if (w->cube_stream) {
        v71r7_lut_stream_abort(w->cube_stream);
        w->cube_stream = NULL;
    }
    if (w->cube_buf) {
        w_heap_free(ctx, w->cube_buf);
        w->cube_buf = NULL;
    }
    w->cube_cap = 0u;
    w->cube_bytes = 0u;
}

static void deactivate_lut_core(PhotoMenuCtx *ctx) {
    V71r7DramAnchor *a = v71r7_anchor();
    v71r7_lut_clear_active();
    __atomic_fetch_and(&a->flags, ~V71R7_ANCHOR_FLAG_LUT_ACTIVE, __ATOMIC_ACQ_REL);
    uint32_t gen = __atomic_add_fetch(&a->active_generation, 1u, __ATOMIC_ACQ_REL);
    ctx->worker.lut_generation = gen;
    v71r7_layer0_notify_lut_changed(gen, 0);
}

static void fail_job(PhotoMenuCtx *ctx, FxResult rc) {
    PhotoWorkerState *w = &ctx->worker;
    release_cube_stream(ctx);
    uint32_t opened = w->ticket;
    (void)__atomic_compare_exchange_n(&v71r7_anchor()->output_permit, &opened,
                                      w->ticket & ~1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    w->save_result = rc;
    publish_status(ctx, rc == FX_EMPTY ? PHOTO_MISSING : PHOTO_LOAD_ERROR, rc, 0u, 0u);
    if (rc == FX_IO || rc == FX_CLOSE || rc == FX_STALE) w->media_fault = 1u;
    w->job = JOB_CLEAN_ERROR;
}

static void restore_from_journal(PhotoMenuCtx *ctx) {
    PhotoWorkerState *w = &ctx->worker;
    PhotoFileIO io = make_file_io(ctx);
    PhotoJournal journal;
    FxResult rc = photo_journal_load(&io, w->drive, &journal);
    if (!work_current(ctx)) return;
    if (rc == FX_EMPTY || (rc == FX_OK && !journal.choice.path[0])) {
        deactivate_lut_core(ctx);
        publish_status(ctx, PHOTO_NO_LUT, FX_OK, 0u, 0u);
        return;
    }
    if (rc != FX_OK) {
        deactivate_lut_core(ctx);
        publish_status(ctx, PHOTO_JOURNAL_ERROR, rc, 0u, 0u);
        return;
    }
    rc = photo_file_exists(&io, w->drive, journal.choice.path);
    if (!work_current(ctx)) return;
    if (rc != FX_OK) {
        deactivate_lut_core(ctx);
        publish_status(ctx, rc == FX_EMPTY ? PHOTO_MISSING : PHOTO_LOAD_ERROR, rc, 0u, 0u);
        return;
    }
    memcpy(w->name, journal.choice.path, sizeof(w->name));
    w->restore = 1u;
    w->job = JOB_DRAIN;
    publish_status(ctx, PHOTO_QUEUED, FX_OK, 0u, 0u);
}

static int step_job(PhotoMenuCtx *ctx) {
    PhotoWorkerState *w = &ctx->worker;
    PhotoFileIO io = make_file_io(ctx);
    FxResult rc;

    switch (w->job) {
    case JOB_DRAIN:
        release_cube_stream(ctx);
        deactivate_lut_core(ctx);
        if (w->name[0]) {
            w->job = JOB_OPEN_CUBE;
            publish_status(ctx, PHOTO_READING, FX_OK, 0u, 0u);
        } else {
            w->job = w->restore ? JOB_NONE : JOB_SAVE;
            if (w->restore) publish_status(ctx, PHOTO_NO_LUT, FX_OK, 0u, 0u);
        }
        break;

    case JOB_OPEN_CUBE: {
        char full_path[PHOTO_PATH_BYTES];
        photo_card_path(w->drive, w->name, full_path);
        int32_t fd = -1;
        rc = file_open_cb(ctx, full_path, 0u, &fd);
        if (rc != FX_OK) {
            fail_job(ctx, rc);
            break;
        }
        w->cube_fd = fd;
        w->cube_fd_open = 1u;
        w->cube_bytes = 0u;
        /* Fixed 16 KiB chunk buffer + streaming parser (~229 KiB total peak heap vs 3 MiB doubling). */
        w->cube_buf = (uint8_t *)w_heap_alloc(ctx, PHOTO_CUBE_CHUNK_BYTES, 0u);
        if (!w->cube_buf) {
            fail_job(ctx, FX_BUDGET);
            break;
        }
        w->cube_cap = PHOTO_CUBE_CHUNK_BYTES;
        V71r7DramAnchor *a = v71r7_anchor();
        uint32_t next_gen = __atomic_load_n(&a->active_generation, __ATOMIC_ACQUIRE) + 1u;
        if (!next_gen) next_gen = 1u;
        FxZ8LutProgress prog;
        memset(&prog, 0, sizeof(prog));
        w->cube_stream = v71r7_lut_stream_begin(next_gen, &prog);
        if (!w->cube_stream) {
            fail_job(ctx, FX_BUDGET);
            break;
        }
        w->job = JOB_READ_CUBE;
        publish_status(ctx, PHOTO_READING, FX_OK, 0u, 0u);
        break;
    }

    case JOB_READ_CUBE: {
        if (!w->cube_fd_open || !w->cube_buf || !w->cube_stream) {
            fail_job(ctx, FX_STATE);
            break;
        }
        _Alignas(64) uint8_t eof_probe[64];
        void *dst;
        uint32_t want;
        int probing_overflow = (w->cube_bytes >= PHOTO_MAX_CUBE_BYTES);
        if (probing_overflow) {
            dst = eof_probe;
            want = 1u;
        } else {
            uint32_t room = PHOTO_MAX_CUBE_BYTES - w->cube_bytes;
            want = room < w->cube_cap ? room : w->cube_cap;
            dst = w->cube_buf;
        }
        if (!io_admit(ctx, 1)) {
            fail_job(ctx, FX_STALE);
            break;
        }
        uint32_t got = 0u;
        v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_CARD_IO, 0u, FX_Z8_HB_CARD_READ);
        uint32_t native_rc = w_card_read(ctx, w->cube_fd, dst, want, &got);
        v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_STEP, 0u, w->job);
        rc = finish_io(ctx, native_rc, 1);
        if (rc != FX_OK) {
            fail_job(ctx, rc);
            break;
        }
        if (got > want) {
            fail_job(ctx, FX_INTEGRITY);
            break;
        }
        if (got == 0u) {
            rc = file_close_cb(ctx, w->cube_fd);
            w->cube_fd = -1;
            w->cube_fd_open = 0u;
            if (w->cube_buf) {
                w_heap_free(ctx, w->cube_buf);
                w->cube_buf = NULL;
                w->cube_cap = 0u;
            }
            if (rc != FX_OK) {
                fail_job(ctx, rc);
                break;
            }
            if (w->cube_bytes == 0u || w->cube_bytes > PHOTO_MAX_CUBE_BYTES) {
                fail_job(ctx, w->cube_bytes ? FX_BOUNDS : FX_INTEGRITY);
                break;
            }
            w->job = JOB_PREPARE;
            publish_status(ctx, PHOTO_PREPARING, FX_OK, 0u, w->cube_bytes);
            break;
        }
        if (probing_overflow) {
            fail_job(ctx, FX_BOUNDS);
            break;
        }
        FxZ8LutProgress prog;
        memset(&prog, 0, sizeof(prog));
        if (!v71r7_lut_stream_feed(w->cube_stream, w->cube_buf, got, &prog)) {
            FxResult err = (prog.result == 1u) ? FX_BOUNDS :
                           (prog.result == 2u || prog.result == 3u) ? FX_BUDGET : FX_INTEGRITY;
            fail_job(ctx, err);
            break;
        }
        w->cube_bytes += got;
        publish_status(ctx, PHOTO_READING, FX_OK, w->cube_bytes, 0u);
        break;
    }

    case JOB_PREPARE: {
        if (!w->cube_stream) {
            fail_job(ctx, FX_STATE);
            break;
        }
        V71r7DramAnchor *a = v71r7_anchor();
        uint32_t next_gen = __atomic_load_n(&a->active_generation, __ATOMIC_ACQUIRE) + 1u;
        if (!next_gen) next_gen = 1u;
        /* P1 Cache Race Fix: invalidate Layer 0's 4 MiB cache & drain readers BEFORE
         * v71r7_lut_stream_commit() advances LutCoreCtx->generation! */
        v71r7_layer0_cache_invalidate_before_swap();
        FxZ8LutProgress prog;
        memset(&prog, 0, sizeof(prog));
        void *stream = w->cube_stream;
        w->cube_stream = NULL; /* commit always takes ownership and frees stream */
        int prep_rc = v71r7_lut_stream_commit(stream, w->name, next_gen, &prog);
        release_cube_stream(ctx);
        if (!work_current(ctx)) {
            if (prep_rc > 0) deactivate_lut_core(ctx);
            break;
        }
        if (prep_rc <= 0) {
            FxResult err = (prog.result == 2u || prog.result == 3u) ? FX_BUDGET : FX_INTEGRITY;
            fail_job(ctx, err);
            break;
        }
        __atomic_store_n(&a->active_generation, next_gen, __ATOMIC_RELEASE);
        __atomic_fetch_or(&a->flags, V71R7_ANCHOR_FLAG_LUT_ACTIVE, __ATOMIC_ACQ_REL);
        w->lut_generation = next_gen;
        v71r7_layer0_notify_lut_changed(next_gen, 1);

        uint32_t closed = w->ticket & ~1u;
        if (work_current(ctx) &&
            __atomic_compare_exchange_n(&a->output_permit, &closed, w->ticket, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            publish_status(ctx, PHOTO_READY, FX_OK, 0u, 0u);
            w->rear_presented = 0u;
            w->save_result = FX_OK;
            w->job = w->restore ? JOB_OUTPUT : JOB_SAVE;
        }
        break;
    }

    case JOB_SAVE:
        rc = photo_journal_save(&io, w->drive, w->name);
        if (!work_current(ctx)) break;
        w->save_result = rc;
        if (rc == FX_STALE) w->media_fault = 1u;
        if (w->name[0]) {
            publish_status(ctx, rc == FX_OK ? PHOTO_READY : PHOTO_SAVE_ERROR, rc, 0u, 0u);
            w->job = JOB_OUTPUT;
        } else {
            publish_status(ctx, rc == FX_OK ? PHOTO_NO_LUT : PHOTO_CLEAR_SAVE_ERROR, rc, 0u, 0u);
            w->job = JOB_NONE;
        }
        break;

    case JOB_OUTPUT: {
        uint32_t active_gen = 0u;
        if (!v71r7_lut_is_active(&active_gen, NULL)) {
            fail_job(ctx, FX_STATE);
            break;
        }
        uint32_t step_rc = v71r7_layer0_worker_step();
        if (!work_current(ctx)) break;
        return step_rc == REAR_STREAMING ? 1 : 0;
    }

    case JOB_CLEAN_ERROR:
        release_cube_stream(ctx);
        deactivate_lut_core(ctx);
        w->job = JOB_NONE;
        break;

    case JOB_NONE:
    default: {
        uint32_t flags = __atomic_load_n(&v71r7_anchor()->flags, __ATOMIC_ACQUIRE);
        if ((flags & V71R7_ANCHOR_FLAG_WYSIWYG_ACTIVE) || v71r7_layer0_workspace_allocated()) {
            uint32_t step_rc = v71r7_layer0_worker_step();
            return step_rc == REAR_STREAMING ? 1 : 0;
        }
        break;
    }
    }
    return 0;
}

static void media_unavailable_now(PhotoMenuCtx *ctx) {
    PhotoWorkerState *w = &ctx->worker;
    release_cube_stream(ctx);
    (void)close_directory(ctx);
    clear_cache(ctx);
    w->scan = SCAN_NONE;
    memset(w->category, 0, sizeof(w->category));
    w->media_fault = 0u;
    if (!atomic_exchange(&w->media_unavailable, 1u)) w->ticket = close_output();
    w->name[0] = '\0';
    w->save_result = atomic_load(&w->retained) ? FX_CLOSE : FX_STALE;
    w->job = JOB_CLEAN_ERROR;
    memset(&w->page, 0, sizeof(w->page));
    publish_page(ctx, atomic_load(&w->page_request));
    publish_status(ctx, PHOTO_JOURNAL_ERROR, w->save_result, 0u, 0u);
}

static int drain_owner(PhotoMenuCtx *ctx) {
    PhotoWorkerState *w = &ctx->worker;
    release_cube_stream(ctx);
    (void)close_directory(ctx);
    clear_cache(ctx);
    w->scan = SCAN_NONE;
    memset(w->category, 0, sizeof(w->category));
    deactivate_lut_core(ctx);
    atomic_store(&w->drain_needed, 0u);
    return 1;
}

static void update_drive_mode(PhotoMenuCtx *ctx, char drive) {
    uint32_t mode_val = (drive == 'B') ? MODE_DRIVE_B : MODE_DRIVE_A;
    uint64_t old = atomic_load(&ctx->worker.activity);
    uint64_t next;
    do {
        next = (old & ~(uint64_t)MODE_MASK) | mode_val;
    } while (!atomic_compare_exchange_weak(&ctx->worker.activity, &old, next));
}

static int lut_worker_enabled(void) {
    return v71r7_lut_is_module_loaded() && (v71r7_lut_effective_master_on() != 0u);
}

static uint32_t lut_anchor_enable_epoch(void) {
    const V71r7DramAnchor *a = v71r7_anchor();
    if (__atomic_load_n(&a->magic, __ATOMIC_ACQUIRE) != V71R7_ANCHOR_MAGIC) return 0u;
    return __atomic_load_n(&a->lut_enable_epoch, __ATOMIC_ACQUIRE);
}

static int begin_epoch(PhotoMenuCtx *ctx, char drive, uint32_t io_epoch, uint32_t epoch) {
    PhotoWorkerState *w = &ctx->worker;
    atomic_store(&w->bound_io_epoch, 0u);
    if (!drain_owner(ctx)) return 0;
    if (w->epoch == epoch) {
        epoch = close_output();
        atomic_store(&w->restore_ticket, epoch);
    }
    if (!epoch) return 0;
    w->epoch = epoch;
    w->io_epoch = io_epoch;
    w->drive = drive;
    w->seen_card_seq = atomic_load(&w->card_change_seq);
    w->seen_enable_epoch = lut_anchor_enable_epoch();
    w->page_seen = atomic_load(&w->page_request) ? UINT32_MAX : 0u;
    w->job = JOB_NONE;
    w->ticket = epoch;
    w->rear_wait = 0u;
    w->name[0] = '\0';
    memset(&w->page, 0, sizeof(w->page));
    update_drive_mode(ctx, drive);
    atomic_store(&w->drain_needed, 1u);
    atomic_store(&w->media_unavailable, 0u);
    int enabled = lut_worker_enabled();
    w->seen_lut_enabled = enabled ? 1u : 0u;
    if (!enabled) {
        /* Strict 0 B dormant invariant: skip FXPHOTO0/1.CFG card I/O when lut.wlm disabled/off. */
        deactivate_lut_core(ctx);
        publish_status(ctx, PHOTO_MODULE_DISABLED, FX_OK, 0u, 0u);
    } else if (atomic_load(&w->retained)) {
        media_unavailable_now(ctx);
    } else {
        restore_from_journal(ctx);
    }
    atomic_store(&w->bound_io_epoch, io_epoch);
    return 1;
}

static int work_slice(PhotoMenuCtx *ctx, char drive) {
    PhotoWorkerState *w = &ctx->worker;
    FxZ8LutIoGate *gate = v71r7_io_gate();
    uint32_t io_epoch = fx_z8_lut_io_epoch(gate);
    if (!io_epoch) {
        atomic_store(&w->bound_io_epoch, 0u);
        (void)drain_owner(ctx);
        return 0;
    }

    int enabled = lut_worker_enabled();
    uint32_t enable_epoch = lut_anchor_enable_epoch();
    if (!enabled) {
        if (w->seen_lut_enabled || w->job != JOB_NONE || w->scan != SCAN_NONE ||
            w->cube_buf || w->cube_stream || w->cache) {
            release_cube_stream(ctx);
            (void)close_directory(ctx);
            clear_cache(ctx);
            w->scan = SCAN_NONE;
            memset(w->category, 0, sizeof(w->category));
            w->job = JOB_NONE;
            w->seen_lut_enabled = 0u;
            deactivate_lut_core(ctx);
            publish_status(ctx, PHOTO_MODULE_DISABLED, FX_OK, 0u, 0u);
        }
        /* Keep filmsim.wlm standalone WYSIWYG Preview & workspace retirement decoupled! */
        return step_job(ctx);
    }

    if (!w_card_drive_ready(ctx)) return 0;
    char cur_drive = w_card_drive(ctx);
    if (cur_drive != 'A' && cur_drive != 'B') return 0;

    uint32_t card_seq = atomic_load(&w->card_change_seq);
    uint32_t epoch = atomic_load(&w->restore_ticket);
    int need_rebind = (!drive || cur_drive != w->drive ||
                       w->seen_card_seq != card_seq ||
                       w->epoch != epoch || w->io_epoch != io_epoch ||
                       !w->seen_lut_enabled || w->seen_enable_epoch != enable_epoch);
    if (need_rebind) {
        if (!begin_epoch(ctx, cur_drive, io_epoch, epoch)) return 0;
    }

    uint32_t ticket = permit_load() | 1u;
    PhotoSelection posted;
    if (!atomic_load(&w->media_unavailable) &&
        snapshot_seq(&w->selection_seq, w->selection_words, &posted, sizeof(posted)) &&
        posted.ticket == ticket && posted.ticket != w->ticket && storage_current(ctx)) {
        w->ticket = posted.ticket;
        memcpy(w->name, posted.name, sizeof(w->name));
        w->restore = 0u;
        w->rear_wait = 0u;
        w->job = JOB_DRAIN;
        publish_status(ctx, PHOTO_QUEUED, FX_OK, 0u, 0u);
    }

    int streaming = work_current(ctx) && !w->media_fault && step_job(ctx);
    if (storage_current(ctx) && !w->media_fault) directory_slice(ctx);
    else (void)close_directory(ctx);
    if (w->media_fault) media_unavailable_now(ctx);
    return streaming && !atomic_load(&w->media_unavailable);
}

static void maybe_poll_watchdog(PhotoMenuCtx *ctx) {
    PhotoWorkerState *w = &ctx->worker;
    uint64_t now = w_now_ticks(ctx);
    uint64_t period = (uint64_t)PHOTO_WATCHDOG_PERIOD_MS * (uint64_t)FX_Z8_TICKS_PER_MS;
    if (!w->last_watchdog_tick || now < w->last_watchdog_tick ||
        now - w->last_watchdog_tick >= period) {
        w->last_watchdog_tick = now;
        ++w->watchdog_polls;
        (void)v71r7_watchdog_poll(now);
    }
}

int photo_worker_slice(void) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx) return 0;
    PhotoWorkerState *w = &ctx->worker;
    uint64_t state = atomic_load(&w->activity);
    char drive = state_drive(state);
    int drain = (state_mode(state) == MODE_PARKED) && atomic_load(&w->drain_needed);
    if ((state & FLAG_WORKER_BUSY) || (!drive && !drain && state_mode(state) != MODE_ARMED))
        return 0;
    if (!atomic_compare_exchange_strong(&w->activity, &state, state | FLAG_WORKER_BUSY))
        return 0;

    maybe_poll_watchdog(ctx);
    v71r7_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_STEP, 0u, w->job);
    int streaming = work_slice(ctx, drive);
    v71r7_heartbeat_idle(FX_Z8_HB_WORKER);
    atomic_fetch_and(&w->activity, ~((uint64_t)FLAG_WORKER_BUSY));
    return streaming;
}

static uint32_t photo_worker_idle_delay_ms(PhotoMenuCtx *ctx) {
    if (!ctx) {
        return 20u;
    }
    PhotoWorkerState *w = &ctx->worker;
    V71r7DramAnchor *anc = v71r7_anchor();
    uint32_t anc_flags = (__atomic_load_n(&anc->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC)
                             ? __atomic_load_n(&anc->flags, __ATOMIC_ACQUIRE)
                             : 0u;
    if (!lut_worker_enabled()) {
        int fs_active = ((anc_flags & V71R7_ANCHOR_FLAG_WYSIWYG_ACTIVE) != 0u) ||
                        (v71r7_layer0_workspace_allocated() != 0);
        return fs_active ? 2u : 20u;
    }
    if (!live_drive(ctx)) {
        return 20u;
    }
    int active = (w->job != JOB_NONE) ||
                 (w->scan != SCAN_NONE) ||
                 (atomic_load(&w->page_request) != w->page_seen) ||
                 ((anc_flags & (V71R7_ANCHOR_FLAG_LUT_ACTIVE | V71R7_ANCHOR_FLAG_WYSIWYG_ACTIVE)) != 0u) ||
                 (v71r7_layer0_workspace_allocated() != 0);
    return active ? 2u : 20u;
}

#ifdef V71R7_HOST_TEST
uint32_t photo_worker_idle_delay_for_test(void) {
    return photo_worker_idle_delay_ms(photo_menu_ctx());
}
#endif

static void photo_worker_task_entry(void *arg) {
    PhotoMenuCtx *ctx = (PhotoMenuCtx *)arg;
    for (;;) {
        if (!photo_worker_slice()) {
            uint32_t delay_ms = photo_worker_idle_delay_ms(ctx);
            (void)w_task_delay(ctx, delay_ms);
        }
    }
}

/* ============================================================================
 * 7. Lifecycle: Boot Once (ev==19), Card Change (ev==24), Pending & Shutdown
 * ============================================================================ */
static int spawn_worker_task(PhotoMenuCtx *ctx) {
    PhotoWorkerState *w = &ctx->worker;
#ifdef V71R7_HOST_TEST
    if (ctx->plat && ctx->plat->task_start) {
        int ok = ctx->plat->task_start(ctx->plat->user, photo_worker_task_entry, ctx,
                                       w->stack_aligned, PHOTO_WORKER_STACK_BYTES,
                                       PHOTO_WORKER_PRIORITY);
        w->task_phase = ok ? 2u : 3u;
        return ok;
    }
    w->task_phase = 2u;
    return 1;
#else
    if (!w->stack_aligned || ((uintptr_t)w->stack_aligned & 15u)) {
        w->task_phase = 3u;
        return 0;
    }
    PhotoRtosTaskRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.context     = (uintptr_t)ctx;
    rec.entry       = (uintptr_t)photo_worker_task_entry;
    rec.priority    = PHOTO_WORKER_PRIORITY;
    rec.stack_bytes = PHOTO_WORKER_STACK_BYTES;
    rec.stack       = (uintptr_t)w->stack_aligned;
    rec.processor   = PHOTO_WORKER_CPU2_PROC;
    rec.affinity    = PHOTO_WORKER_CPU2_AFFIN;
    w->task_id = ROM_CRE_TSK(&rec);
    if (w->task_id < 1) {
        w->task_native_err = w->task_id;
        w->task_phase = 3u;
        return 0;
    }
    w->task_phase = 1u;
    w->task_native_err = ROM_STA_TSK(w->task_id);
    if (w->task_native_err != 0) {
        w->task_phase = 3u;
        return 0;
    }
    w->task_phase = 2u;
    return 1;
#endif
}

static void boot_with_plat(PhotoMenuCtx *prealloc_or_null) {
    V71r7DramAnchor *a = v71r7_anchor();
    if (__atomic_load_n(&a->magic, __ATOMIC_ACQUIRE) != V71R7_ANCHOR_MAGIC) {
        memset(a, 0, sizeof(*a));
        __atomic_store_n(&a->magic, V71R7_ANCHOR_MAGIC, __ATOMIC_RELEASE);
    }

    /* Idempotent once-only claim on a->menu_ctx (NULL -> sentinel 1 -> ctx).
     * P1 Fix: NEVER call v71r7_flight_init() before winning the CAS, so repeated
     * ev==19 calls cannot wipe live flight-recorder telemetry! */
    void *expected = NULL;
    if (!__atomic_compare_exchange_n(&a->menu_ctx, &expected, (void *)(uintptr_t)1u,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;

    v71r7_flight_init();

    PhotoMenuCtx *ctx = prealloc_or_null;
    if (!ctx) {
#ifdef V71R7_HOST_TEST
        __atomic_store_n(&a->menu_ctx, NULL, __ATOMIC_RELEASE);
        return;
#else
        void *raw = ROM_HEAP_ALLOC((uint32_t)(sizeof(PhotoMenuCtx) + 64u), 0u);
        if (!raw) {
            __atomic_store_n(&a->menu_ctx, NULL, __ATOMIC_RELEASE);
            return;
        }
        uintptr_t aligned = ((uintptr_t)raw + 63u) & ~(uintptr_t)63u;
        ctx = (PhotoMenuCtx *)aligned;
        memset(ctx, 0, sizeof(*ctx));
        ctx->self_raw = raw;
#endif
    }

    ctx->size = (uint32_t)sizeof(*ctx);
    ctx->worker.directory = -1;
    ctx->worker.cube_fd = -1;

    /* Allocate 32 KiB worker stack (16B-aligned) on kernel heap. */
    void *stack_raw = w_heap_alloc(ctx, PHOTO_WORKER_STACK_BYTES + 16u, 16u);
    if (!stack_raw) {
        void *self_raw = ctx->self_raw;
        w_heap_free(ctx, self_raw);
        __atomic_store_n(&a->menu_ctx, NULL, __ATOMIC_RELEASE);
        return;
    }
    ctx->worker.stack_raw = stack_raw;
    ctx->worker.stack_aligned = (void *)(((uintptr_t)stack_raw + 15u) & ~(uintptr_t)15u);

    /* Ensure FxZ8LutIoGate has an open epoch and Layer 0 provider is initialized. */
    FxZ8LutIoGate *gate = v71r7_io_gate();
    if (!fx_z8_lut_io_epoch(gate)) fx_z8_lut_io_reopen(gate, 0);
    (void)v71r7_layer0_init();

    uint32_t ticket = close_output();
    atomic_store(&ctx->worker.restore_ticket, ticket);
    uint64_t initial_state = (UINT64_C(1) << 32) |
                             (ticket ? MODE_ARMED : MODE_WORKER_FAILED) |
                             FLAG_GUI_OPEN;
    atomic_store(&ctx->worker.activity, initial_state);
    atomic_store(&ctx->worker.task_attempted, 1u);

    __atomic_store_n(&ctx->magic, PHOTO_MENU_CTX_MAGIC, __ATOMIC_RELEASE);
    __atomic_store_n(&a->menu_ctx, ctx, __ATOMIC_RELEASE);

    if (!spawn_worker_task(ctx)) {
        uint64_t old = atomic_load(&ctx->worker.activity);
        atomic_store(&ctx->worker.activity,
                     (old & ~(uint64_t)MODE_MASK) | MODE_WORKER_FAILED);
    }
    v71r7_flight_record(FX_Z8_FLIGHT_BOOT, 0u, (uint32_t)ctx->worker.task_id,
                        ctx->worker.task_phase, ticket);
}

void v71r7_photo_menu_boot_once(void) {
    boot_with_plat(NULL);
}

void v71r7_photo_menu_on_card_change(void) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx) return;
    atomic_fetch_add(&ctx->worker.card_change_seq, 1u);
    atomic_store(&ctx->worker.bound_io_epoch, 0u);
    /* Advance GUI lifecycle serial so open browser pages cleanly rebind to the new card. */
    uint64_t old = atomic_load(&ctx->worker.activity);
    uint64_t next;
    do {
        uint64_t serial = (old >> 32) + 1u;
        next = (serial << 32) | (old & UINT64_C(0xffffffff));
    } while (!atomic_compare_exchange_weak(&ctx->worker.activity, &old, next));
    uint32_t ticket = close_output();
    if (ticket) atomic_store(&ctx->worker.restore_ticket, ticket);
}

uint32_t photo_worker_pending(void) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx) return 0u;
    PhotoWorkerState *w = &ctx->worker;
    return ((atomic_load(&w->activity) & FLAG_WORKER_BUSY) != 0u) ||
           atomic_load(&w->directory_owned) ||
           ((!live_drive(ctx) || !fx_z8_lut_io_epoch(v71r7_io_gate())) &&
            atomic_load(&w->drain_needed)) ||
           atomic_load(&w->retained) ||
           photo_ui_pending();
}

void photo_worker_shutdown(void) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx) return;
    fx_z8_lut_io_close(v71r7_io_gate(), 1);
    uint64_t old = atomic_load(&ctx->worker.activity);
    uint64_t next;
    do {
        uint64_t serial = (old >> 32) + 1u;
        next = (serial << 32) | (old & FLAG_WORKER_BUSY) | MODE_PARKED;
    } while (!atomic_compare_exchange_weak(&ctx->worker.activity, &old, next));
    if (old & FLAG_GUI_OPEN) (void)close_output();
    atomic_store(&ctx->worker.bound_io_epoch, 0u);
    v71r7_flight_record(FX_Z8_FLIGHT_PACK_STOP, 0u, 0u, 0u, 0u);
}

/* ============================================================================
 * 8. lut.wlm slots.state EL1 IPC Handler (ops 65..68, magic 'LTI1', Bank 2)
 *    Called by filmsim_slots_svc_dispatch() for ops 64..79.
 *    Return mapping: 0=FX_OK, -2=FX_ARGUMENT, -3=FX_CAPABILITY, -5=FX_BUSY, <0=FX_STATE.
 * ============================================================================ */
int v71r7_lut_ipc_service_call(uint32_t op, const void *req, uint32_t req_bytes,
                               void *resp, uint32_t *resp_bytes) {
    uint32_t subop = (op >= V71R7_LUT_SLOTS_OP_BASE) ? (op - V71R7_LUT_SLOTS_OP_BASE) : op;
    V71r7DramAnchor *a = v71r7_anchor();
    PhotoMenuCtx *ctx = photo_menu_ctx();

    switch (subop) {
    case 1u: { /* V71R7_LUT_OP_GET_STATE (65) */
        if (!resp || !resp_bytes || *resp_bytes < sizeof(V71r7LutIpcState)) {
            return -2;
        }
        V71r7LutIpcState st;
        memset(&st, 0, sizeof(st));
        st.magic   = V71R7_LUT_IPC_MAGIC;
        st.version = V71R7_LUT_IPC_VERSION;
        st.bytes   = (uint16_t)sizeof(st);

        uint8_t master = v71r7_lut_effective_master_on();
        uint8_t smask  = v71r7_lut_effective_stage_mask();
        st.master_on  = master;
        st.preview_on = (smask & V71R7_LUT_STAGE_PREVIEW) ? 1u : 0u;
        st.still_on   = (smask & V71R7_LUT_STAGE_STILL)   ? 1u : 0u;
        st.live_on    = (smask & V71R7_LUT_STAGE_LIVE)    ? 1u : 0u;
        char drv = ctx ? w_card_drive(ctx) : v71r7_active_card_drive();
        st.active_drive = (uint8_t)((drv == 'B') ? 'B' : 'A');

        uint32_t gen = 0u;
        int is_1d = 0;
        int active = v71r7_lut_is_active(&gen, &is_1d);
        st.lut_active         = active ? 1u : 0u;
        st.is_1d_separable    = is_1d ? 1u : 0u;
        st.generation         = gen;
        st.neon_allowed       = v71r7_neon_sentinel_is_allowed() ? 1u : 0u;
        st.smp_helpers_online = (uint8_t)v71r7_smp_online_helpers();

        PhotoStatus ps = photo_lut_status();
        st.worker_phase = (!v71r7_lut_is_module_loaded() || !master)
                              ? (uint32_t)PHOTO_MODULE_DISABLED
                              : ps.phase;
        st.last_result = (int32_t)ps.reason;

        const Layer0Metrics *m = v71r7_layer0_metrics_ptr();
        if (m) {
            st.quarantined     = atomic_load_explicit(&m->quarantined, memory_order_relaxed) ? 1u : 0u;
            st.pool_result     = atomic_load_explicit(&m->pool_result, memory_order_relaxed);
            st.ws_bytes        = atomic_load_explicit(&m->surface_bytes, memory_order_relaxed);
            st.fps_milli       = atomic_load_explicit(&m->fps_milli, memory_order_relaxed);
            st.grades          = atomic_load_explicit(&m->grades, memory_order_relaxed);
            st.disp_sub        = atomic_load_explicit(&m->disp_substituted, memory_order_relaxed);
            st.disp_rejects    = atomic_load_explicit(&m->disp_rejects, memory_order_relaxed);
            st.live_reuse_hits = atomic_load_explicit(&m->live_reuse_hits, memory_order_relaxed);
            st.last_lut_us     = atomic_load_explicit(&m->last_lut_us, memory_order_relaxed);
            st.last_hdf_us     = atomic_load_explicit(&m->last_hdf_us, memory_order_relaxed);
            st.last_grain_us   = atomic_load_explicit(&m->last_grain_us, memory_order_relaxed);
            if (st.last_result == 0) {
                st.last_result = atomic_load_explicit(&m->last_result, memory_order_relaxed);
            }
        }

        V71r7LutRuntimeStats rs;
        v71r7_lut_runtime_get_stats(&rs);
        st.xm6_calls   = rs.nv16_xm6_calls;
        st.nv16_neon   = rs.nv16_neon_calls;
        st.nv16_scalar = rs.nv16_scalar_calls;
        st.nv12_neon   = rs.nv12_neon_calls;
        st.wd_polls    = ctx ? ctx->worker.watchdog_polls : 0u;

        (void)v71r7_lut_get_active_path(st.active_path);
        st.active_path[FX_Z8_LUT_PATH_BYTES - 1u] = '\0';

        memcpy(resp, &st, sizeof(st));
        *resp_bytes = (uint32_t)sizeof(st);
        return 0;
    }

    case 2u: { /* V71R7_LUT_OP_SET_CFG (66) */
        if (!req || req_bytes != sizeof(V71r7LutIpcSetCfg)) {
            return -2;
        }
        V71r7LutIpcSetCfg cfg;
        memcpy(&cfg, req, sizeof(cfg));
        if (cfg.magic != V71R7_LUT_IPC_MAGIC ||
            cfg.version != V71R7_LUT_IPC_VERSION ||
            cfg.bytes != sizeof(V71r7LutIpcSetCfg) ||
            cfg.master_on > 1u ||
            (cfg.stage_mask & ~V71R7_LUT_STAGE_ALL) != 0u) {
            return -2;
        }
        if (__atomic_load_n(&a->magic, __ATOMIC_ACQUIRE) != V71R7_ANCHOR_MAGIC) {
            memset(a, 0, sizeof(*a));
            __atomic_store_n(&a->magic, V71R7_ANCHOR_MAGIC, __ATOMIC_RELEASE);
        }
        uint8_t prev_master = v71r7_lut_effective_master_on();
        __atomic_store_n(&a->lut_master_on, cfg.master_on, __ATOMIC_RELEASE);
        __atomic_store_n(&a->lut_stage_mask, cfg.stage_mask, __ATOMIC_RELEASE);
        __atomic_store_n(&a->lut_cfg_set, 1u, __ATOMIC_RELEASE);

        if (prev_master && !cfg.master_on) {
            /* 1 -> 0: release JpegCube* & let Layer 0 retire workspace without erasing FXPHOTO0/1.CFG */
            v71r7_lut_clear_active();
            uint32_t gen = __atomic_add_fetch(&a->active_generation, 1u, __ATOMIC_ACQ_REL);
            v71r7_layer0_notify_lut_changed(gen, 0);
        } else if (!prev_master && cfg.master_on) {
            /* 0 -> 1: bump enable epoch so photo_worker auto-restores FXPHOTO0/1.CFG */
            __atomic_add_fetch(&a->lut_enable_epoch, 1u, __ATOMIC_ACQ_REL);
        } else if (cfg.master_on) {
            if (!(cfg.stage_mask & V71R7_LUT_STAGE_PREVIEW)) {
                uint32_t gen = __atomic_load_n(&a->active_generation, __ATOMIC_ACQUIRE);
                v71r7_layer0_notify_lut_changed(gen, 0);
            } else {
                uint32_t cur_gen = 0u;
                if (v71r7_lut_is_active(&cur_gen, NULL)) {
                    v71r7_layer0_notify_lut_changed(cur_gen, 1);
                }
            }
        }
        if (resp_bytes) {
            *resp_bytes = 0u;
        }
        return 0;
    }

    case 3u: { /* V71R7_LUT_OP_CLEAR_LUT (67) */
        uint32_t posted = 0u;
        if (ctx) {
            posted = photo_lut_select("", photo_gui_epoch());
        }
        if (!posted) {
            v71r7_lut_clear_active();
            if (__atomic_load_n(&a->magic, __ATOMIC_ACQUIRE) == V71R7_ANCHOR_MAGIC) {
                uint32_t gen = __atomic_add_fetch(&a->active_generation, 1u, __ATOMIC_ACQ_REL);
                v71r7_layer0_notify_lut_changed(gen, 0);
            }
        }
        if (resp_bytes) {
            *resp_bytes = 0u;
        }
        return 0;
    }

    case 4u: { /* V71R7_LUT_OP_RESET_PREVIEW (68) */
        const Layer0Metrics *m = v71r7_layer0_metrics_ptr();
        if (m && atomic_load_explicit(&m->quarantined, memory_order_acquire) != 0u) {
            return -5; /* FX_BUSY: hardware-quarantined bank must not be reset */
        }
        v71r7_layer0_pool_suspend();
        v71r7_layer0_pool_resume();
        if (resp_bytes) {
            *resp_bytes = 0u;
        }
        return 0;
    }

    default:
        return -3; /* FX_CAPABILITY */
    }
}

#ifdef V71R7_HOST_TEST
int v71r7_photo_menu_host_init(const PhotoMenuPlatOps *plat) {
    if (!plat || !plat->heap_alloc || !plat->heap_free) return 0;
    void *raw = plat->heap_alloc(plat->user, (uint32_t)(sizeof(PhotoMenuCtx) + 64u), 64u);
    if (!raw) return 0;
    uintptr_t aligned = ((uintptr_t)raw + 63u) & ~(uintptr_t)63u;
    PhotoMenuCtx *ctx = (PhotoMenuCtx *)aligned;
    memset(ctx, 0, sizeof(*ctx));
    ctx->self_raw = raw;
    ctx->plat = plat;
    boot_with_plat(ctx);
    return photo_menu_ctx() == ctx;
}

void v71r7_photo_menu_host_teardown(void) {
    PhotoMenuCtx *ctx = photo_menu_ctx();
    if (!ctx) return;
    release_cube_stream(ctx);
    (void)close_directory(ctx);
    clear_cache(ctx);
    void *stack_raw = ctx->worker.stack_raw;
    void *self_raw = ctx->self_raw;
    const PhotoMenuPlatOps *plat = ctx->plat;
    __atomic_store_n(&ctx->magic, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&v71r7_anchor()->menu_ctx, NULL, __ATOMIC_RELEASE);
    if (plat && plat->heap_free) {
        if (stack_raw) plat->heap_free(plat->user, stack_raw);
        if (self_raw) plat->heap_free(plat->user, self_raw);
    }
}
#endif
