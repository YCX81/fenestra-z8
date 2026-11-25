/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Native Nikon Photo Shooting Menu ("Select LUT / 选择 LUT"),
 * .cube Catalog, Dual-Slot CRC32 Journal, UI Controller & Resident Worker ABI.
 *
 * ZERO-GLOBAL INVARIANT:
 *   No C unit in work/lut_menu/ may define any mutable static or global variable
 *   (.data == 0, .bss == 0 in Bank 2 RX/RO 0x40981000..0x40990000).
 *   All mutable runtime state lives inside heap-allocated PhotoMenuCtx
 *   (allocated via HEAP_ALLOC 0x40D2C408, pointer anchored at v71r7_anchor()->menu_ctx).
 *
 * STRICT SCALAR INVARIANT:
 *   Every .c file compiles cleanly with -mgeneral-regs-only (0 FP, 0 SIMD).
 */
#ifndef V71R7_PHOTO_MENU_H
#define V71R7_PHOTO_MENU_H

#include "v71r7_lut_abi.h"
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef V71R7_HOST_TEST
#include <string.h>
#else
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
#endif

/* ============================================================================
 * 1. Result Codes (matching Fenestra FxResult ABI)
 * ============================================================================ */
#if !defined(FENESTRA_H) && !defined(V71R7_FX_RESULT_DEFINED)
#define V71R7_FX_RESULT_DEFINED 1
typedef enum {
    FX_OK = 0,
    FX_ARGUMENT = 1,
    FX_STATE = 2,
    FX_CAPABILITY = 3,
    FX_BUDGET = 4,
    FX_BUSY = 5,
    FX_STALE = 6,
    FX_TIMEOUT = 7,
    FX_IO = 8,
    FX_BOUNDS = 9,
    FX_INTEGRITY = 10,
    FX_CLOSE = 11,
    FX_EMPTY = 12,
    FX_INTERRUPTED = 13
} FxResult;
#endif

/* ============================================================================
 * 2. Photo Menu Constants, Language IDs, Text IDs & Submenu ABI
 * ============================================================================ */
enum {
    PHOTO_RECORD_V1_BYTES     = 64u,
    PHOTO_RECORD_BYTES        = 96u,
    PHOTO_FILES_PER_PAGE      = 20u,
    PHOTO_DIR_BYTES           = 0x220u,
    PHOTO_DIR_BUF_BYTES       = (PHOTO_DIR_BYTES + 63u) & ~63u,
    PHOTO_PATH_BYTES          = 12u + FX_Z8_LUT_PATH_BYTES,
    PHOTO_CACHE_FILES         = 512u,
    PHOTO_MAX_CUBE_BYTES      = 2u * 1024u * 1024u,
    PHOTO_CUBE_CHUNK_BYTES    = 16384u,
    PHOTO_WORKER_STACK_BYTES  = 32768u,
    PHOTO_WORKER_PRIORITY     = 30u,
    PHOTO_WORKER_CPU2_PROC    = 3u,   /* 1-based: 1=CPU0, 2=CPU1, 3=CPU2 */
    PHOTO_WORKER_CPU2_AFFIN   = 4u,   /* 1u << (3 - 1) */
    PHOTO_WATCHDOG_PERIOD_MS  = 100u
};

enum {
    PHOTO_ROOT   = 0u,
    PHOTO_FOLDER = 1u
};

#define PHOTO_FLAT_FOLDER "~"

/* Nikon Z8 Photo Shooting Menu LUT-only IDs (BURST 0x5D5 excluded in V71.7!) */
enum {
    PHOTO_PARENT_PAGE      = 0x19u,
    PHOTO_PARENT_ROW_COUNT = 48u,     /* 47 cloned stock/NFP rows + Row 47 Select LUT */
    PHOTO_LUT_PARENT_ITEM  = 0x2DE7u,
    PHOTO_LUT_PAGE         = 0x5D6u,
    PHOTO_LUT_ROW          = 0x4800u,
    PHOTO_LUT_ROWS         = 23u,
    PHOTO_ROW_PREVIOUS     = 21u,
    PHOTO_ROW_NEXT         = 22u,
    PHOTO_STATUS_SLOT      = 23u,
    PHOTO_FIRST_TEXT       = 0x1F40u,
    PHOTO_TEXT_END         = 0x1F4Bu  /* 11 IDs: 0x1F40..0x1F4A */
};

/* Nikon Z8 language IDs */
enum {
    PHOTO_LANG_JA    = 0u,
    PHOTO_LANG_EN    = 1u,
    PHOTO_LANG_ZH_CN = 8u,
    PHOTO_LANG_ZH_TW = 9u
};

#define PHOTO_MENU_CTX_MAGIC UINT32_C(0x504D4E55) /* "PMNU" */

/* ============================================================================
 * 3. Static RO Menu Table Structures (shared with photo_menu_ro_tables.c)
 * ============================================================================ */
typedef int (*PhotoMenuCallback)(void *);

typedef struct {
    uint32_t    flags;
    uint32_t    pad0;
    const void *layout;
    const void *rows;
    const void *events;
    uint16_t    count;
    int16_t     viewport[4];
    uint8_t     tail[38];
} V71r7MenuDescriptor;
_Static_assert(sizeof(V71r7MenuDescriptor) == 80u, "Nikon menu descriptor ABI");
_Static_assert(offsetof(V71r7MenuDescriptor, rows) == 0x10u, "descriptor rows offset");
_Static_assert(offsetof(V71r7MenuDescriptor, count) == 0x20u, "descriptor count offset");
_Static_assert(offsetof(V71r7MenuDescriptor, viewport) == 0x22u, "descriptor viewport offset");

typedef struct {
    uint32_t words[11];
} V71r7MenuRow;
_Static_assert(sizeof(V71r7MenuRow) == 0x2Cu, "Nikon menu row ABI");

typedef struct {
    uint64_t          event;
    PhotoMenuCallback callback;
    uint64_t          guard;
    uint64_t          target;
} V71r7MenuEventEntry;
_Static_assert(sizeof(V71r7MenuEventEntry) == 0x20u, "Nikon menu event entry ABI");

/* 0x403DB910 initializes this request for the action call at 0x40508DA0.
 * The dispatcher rereads target at 0x40508E04. */
typedef struct {
    uint8_t  *page;
    uint32_t  event;
    uint32_t  target;
} PhotoMenuAction;
_Static_assert(offsetof(PhotoMenuAction, target) == 0xCu, "native action target");

/* Symbols exported by photo_menu_ro_tables.c (linked into Bank 2 RO) */
extern const V71r7MenuDescriptor v71r7_photo_parent_desc;
extern const V71r7MenuRow        v71r7_photo_parent_rows[PHOTO_PARENT_ROW_COUNT];
extern const void *const         v71r7_photo_parent_events[PHOTO_PARENT_ROW_COUNT];
extern const V71r7MenuEventEntry v71r7_photo_parent_lut_event_prog[93];
extern const V71r7MenuDescriptor v71r7_lut_child_desc;
extern const PhotoMenuCallback   v71r7_lut_child_vector[5];
extern const uint32_t            v71r7_lut_child_layout[7][17];
extern const V71r7MenuRow        v71r7_lut_child_rows[PHOTO_LUT_ROWS];
extern const void *const         v71r7_lut_child_events[PHOTO_LUT_ROWS];
extern const V71r7MenuEventEntry v71r7_lut_child_event_table[15];

/* ============================================================================
 * 4. Journal, Catalog, Status & UI Data Structures
 * ============================================================================ */
typedef struct {
    uint64_t generation;
    char     path[FX_Z8_LUT_PATH_BYTES];
} PhotoChoice;

typedef struct {
    void *context;
    int (*current)(void *);
    FxResult (*open)(void *, const char *, uint32_t, int32_t *);
    FxResult (*read)(void *, int32_t, void *, uint32_t, uint32_t *);
    FxResult (*write)(void *, int32_t, const void *, uint32_t, uint32_t *);
    FxResult (*close)(void *, int32_t);
    FxResult (*remove)(void *, const char *);
    FxResult (*rename)(void *, const char *, const char *);
} PhotoFileIO;

typedef struct {
    PhotoChoice choice;
    uint32_t    slot;
} PhotoJournal;

typedef struct {
    uint32_t count;
    uint32_t previous;
    uint32_t next;
    uint32_t serial;
    uint32_t epoch;
    char     names[PHOTO_FILES_PER_PAGE][FX_Z8_LUT_NAME_BYTES];
    uint32_t level;
    uint32_t focus;
    char     category[FX_Z8_LUT_NAME_BYTES];
} PhotoPage;

typedef struct {
    uint32_t count;
    uint32_t opposite;
    int32_t  direction;
    char     cursor[FX_Z8_LUT_NAME_BYTES];
    char     names[PHOTO_FILES_PER_PAGE + 1u][FX_Z8_LUT_NAME_BYTES];
} PhotoCatalog;

typedef enum {
    PHOTO_NO_LUT = 0,
    PHOTO_QUEUED = 1,
    PHOTO_READING = 2,
    PHOTO_PREPARING = 3,
    PHOTO_READY = 4,
    PHOTO_MISSING = 5,
    PHOTO_LOAD_ERROR = 6,
    PHOTO_SAVE_ERROR = 7,
    PHOTO_CLEAR_SAVE_ERROR = 8,
    PHOTO_WORKER_ERROR = 9,
    PHOTO_JOURNAL_ERROR = 10,
    PHOTO_CLEAR_RETIRING = 11,
    PHOTO_MODULE_DISABLED = 12
} PhotoPhase;

typedef struct {
    uint32_t ticket;
    uint32_t phase;
    uint32_t reason;
    uint32_t done;
    uint32_t total;
} PhotoStatus;

typedef struct {
    uint8_t record[24];
    uint8_t buffer[51];
} PhotoTextLease;

typedef struct {
    uint32_t ticket;
    char     name[FX_Z8_LUT_PATH_BYTES];
} PhotoSelection;

typedef struct {
    uint64_t epoch;
    uint32_t serial;
    uint32_t root_focus;
    char     category[FX_Z8_LUT_NAME_BYTES];
} PhotoPageCommand;

typedef struct {
    int32_t  handle;
    uint32_t io_epoch;
    char     drive;
} PhotoRetainedHandle;

/* ============================================================================
 * 5. Host-Testable Platform Ops (ONLY active when V71R7_HOST_TEST is defined)
 * ============================================================================ */
#ifdef V71R7_HOST_TEST
typedef struct {
    void     *user;
    /* Storage & Card I/O */
    char     (*card_drive)(void *user);
    int      (*card_drive_ready)(void *user);
    int32_t  (*file_open)(void *user, const char *path, uint32_t flags, uint32_t mode);
    uint32_t (*file_read)(void *user, int32_t fd, void *dst, uint32_t bytes, uint32_t *done);
    uint32_t (*file_write)(void *user, int32_t fd, const void *src, uint32_t bytes, uint32_t *done, void *extra);
    uint32_t (*file_close)(void *user, int32_t fd);
    uint32_t (*file_remove)(void *user, const char *path);
    uint32_t (*file_rename)(void *user, const char *from, const char *to);
    uint32_t (*dir_open)(void *user, const char *path, int32_t *fd);
    uint32_t (*dir_next)(void *user, int32_t fd, void *entry_buf);
    uint32_t (*dir_close)(void *user, int32_t fd);
    /* Heap, Task & Timer */
    void    *(*heap_alloc)(void *user, uint32_t bytes, uint32_t align_hint);
    void     (*heap_free)(void *user, void *ptr);
    int      (*task_start)(void *user, void (*entry)(void *), void *arg, void *stack, uint32_t stack_bytes, uint32_t prio);
    int32_t  (*task_delay)(void *user, uint32_t ms);
    uint64_t (*now_ticks)(void *user);
    /* Stock Nikon UI Hooks */
    uint32_t (*get_language)(void *user);
    void    *(*current_page)(void *user);
    void    *(*find_widget)(void *user, uint32_t item_id, void *parent);
    void    *(*find_child)(void *user, void *page, uint32_t item_id, uint32_t child_id);
    void     (*draw_fence)(void *user);
    void     (*dirty)(void *user, void *node, uint32_t flag);
    void     (*dynamic_flag)(void *user, void *node, uint32_t flag);
    void     (*set_text)(void *user, const char *ascii, uint32_t slot, uint32_t font);
    void     (*set_wide)(void *user, const uint16_t *wide, uint32_t slot, uint32_t font);
    int      (*bind_text)(void *user, void *node, uint32_t slot, uint32_t kind);
    uint32_t (*text_font)(void *user, uint32_t language, uint32_t sample_id);
    uint32_t (*row_count)(void *user, void *page);
    void     (*row_disable)(void *user, void *page, uint32_t row, uint32_t disable);
    void     (*row_focus)(void *user, void *page, uint32_t row, uint32_t focus);
    void     (*list_layout)(void *user, void *page);
    void     (*view_layout)(void *user, void *page);
    void     (*scrollbar)(void *user, void *page);
    void     (*set_rect)(void *user, void *widget, const int16_t *bounds);
    uint32_t (*view_id)(void *user, uint32_t group);
    void     (*view_set)(void *user, uint32_t id, const int16_t *rect);
    void     (*view_rect)(void *user, uint32_t id, int16_t *rect);
    void     (*mark)(void *user, void *target, uintptr_t fn_addr);
    void     (*redraw)(void *user, uintptr_t fn_addr, uint32_t a, uint32_t b);
    int      (*timer_start)(void *user, uint32_t slot, uint32_t ms, void (*cb)(void));
    void     (*timer_cancel)(void *user, uint32_t slot);
    int      (*timer_active)(void *user, void (**cb_out)(void));
    void     (*stock_back_action)(void *user, void *action);
    void    *(*font_record_ptr)(void *user, uint32_t font);
    void    *(*text_record_ptr)(void *user, uint32_t index);
    void    *(*disabled_flags_ptr)(void *user);
} PhotoMenuPlatOps;
#endif

/* ============================================================================
 * 6. Heap-Allocated Zero-Global PhotoMenuCtx (anchored at v71r7_anchor()->menu_ctx)
 * ============================================================================ */
typedef struct {
    uint8_t        *page;
    uint8_t        *resume_page;
    uint64_t        lease_epoch;
    uint32_t        active;
    uint32_t        resume_active;
    uint32_t        epoch;
    uint32_t        timer_epoch;
    uint32_t        page_bound;
    uint32_t        language;
    uint32_t        in_folder;
    uint32_t        return_pending;
    uint32_t        root_focus;
    uint16_t        root_row;
    uint16_t        root_scroll;
    int16_t         root_view[4];
    uint8_t         saved_disabled[PHOTO_LUT_ROWS];
    uint8_t         saved_fonts[2][16];
    PhotoTextLease  saved_text[PHOTO_STATUS_SLOT + 1u];
    PhotoPage       page_view;
    PhotoStatus     last_status;
    _Atomic uint32_t ui_leased;
} PhotoUiState;

typedef struct {
    _Atomic uint64_t activity;
    _Atomic uint32_t restore_ticket;
    _Atomic uint32_t task_attempted;
    _Atomic uint32_t directory_owned;
    _Atomic uint32_t bound_io_epoch;
    _Atomic uint32_t drain_needed;
    _Atomic uint32_t retained;
    _Atomic uint32_t media_unavailable;
    _Atomic uint32_t card_change_seq;

    _Atomic uint32_t page_request;
    _Atomic uint32_t page_seq;
    _Atomic uint32_t page_words[(sizeof(PhotoPage) + 3u) / sizeof(uint32_t)];

    _Atomic uint32_t page_command_seq;
    _Atomic uint32_t page_command_words[(sizeof(PhotoPageCommand) + 3u) / sizeof(uint32_t)];

    _Atomic uint32_t status_seq;
    _Atomic uint32_t status_words[(sizeof(PhotoStatus) + 3u) / sizeof(uint32_t)];

    _Atomic uint32_t selection_seq;
    _Atomic uint32_t selection_words[(sizeof(PhotoSelection) + 3u) / sizeof(uint32_t)];

    /* Task bookkeeping */
    int32_t  task_id;
    uint32_t task_phase;        /* 0=init, 1=created, 2=running, 3=failed */
    int32_t  task_native_err;
    void    *stack_raw;
    void    *stack_aligned;

    /* Worker single-owner state */
    uint32_t epoch;
    uint32_t io_epoch;
    uint32_t seen_card_seq;
    uint32_t seen_lut_enabled;
    uint32_t seen_enable_epoch;
    uint32_t page_seen;
    uint32_t job;
    uint32_t restore;
    uint32_t ticket;
    uint32_t rear_wait;
    uint32_t rear_retiring;
    uint32_t rear_presented;
    uint32_t media_fault;
    uint32_t lut_generation;
    FxResult save_result;
    char     drive;
    char     name[FX_Z8_LUT_PATH_BYTES];

    /* Streaming .cube file state */
    int32_t  cube_fd;
    uint32_t cube_fd_open;
    uint8_t *cube_buf;
    uint32_t cube_cap;
    uint32_t cube_bytes;
    void    *cube_stream;

    /* Directory scan & folder cache state */
    int32_t  directory;
    PhotoRetainedHandle file_retained;
    PhotoRetainedHandle directory_retained;
    PhotoCatalog catalog;
    PhotoPage    page;
    PhotoPage    root_page;
    char         category[FX_Z8_LUT_NAME_BYTES];
    char       (*cache)[FX_Z8_LUT_NAME_BYTES];
    uint32_t     scan;
    uint32_t     cache_count;
    uint32_t     cache_base;
    uint32_t     cache_ready;

    /* Watchdog cadence */
    uint64_t last_watchdog_tick;
    uint32_t watchdog_polls;

    /* 64-byte DMA-aligned I/O buffers (zero ROM globals!) */
    _Alignas(64) uint8_t entry_buffer[PHOTO_DIR_BUF_BYTES];
    _Alignas(64) uint8_t write_buffer[128];
} PhotoWorkerState;

typedef struct {
    uint32_t         magic;
    uint32_t         size;
    void            *self_raw;
#ifdef V71R7_HOST_TEST
    const PhotoMenuPlatOps *plat;
#endif
    PhotoUiState     ui;
    PhotoWorkerState worker;
} PhotoMenuCtx;

/* Anchor accessor (inline, zero globals) */
static inline PhotoMenuCtx *photo_menu_ctx(void) {
    V71r7DramAnchor *a = v71r7_anchor();
    if (__atomic_load_n(&a->magic, __ATOMIC_ACQUIRE) != V71R7_ANCHOR_MAGIC) return NULL;
    uintptr_t raw = (uintptr_t)__atomic_load_n(&a->menu_ctx, __ATOMIC_ACQUIRE);
    if (raw <= 1u) return NULL;
    PhotoMenuCtx *ctx = (PhotoMenuCtx *)raw;
    return __atomic_load_n(&ctx->magic, __ATOMIC_ACQUIRE) == PHOTO_MENU_CTX_MAGIC ? ctx : NULL;
}

/* ============================================================================
 * 7. Module Function Prototypes
 * ============================================================================ */

/* --- photo_journal.c --- */
uint32_t photo_crc32(const uint8_t *data, size_t bytes);
void     photo_card_path(char drive, const char *relative, char *out);
FxResult photo_choice_read(const uint8_t *data, size_t bytes, PhotoChoice *out);
FxResult photo_choice_write(const PhotoChoice *choice, uint8_t data[PHOTO_RECORD_BYTES]);
FxResult photo_journal_load(const PhotoFileIO *io, char drive, PhotoJournal *out);
FxResult photo_journal_save(const PhotoFileIO *io, char drive, const char *path);
FxResult photo_file_exists(const PhotoFileIO *io, char drive, const char *path);

/* --- photo_catalog.c --- */
int  photo_name_compare(const char *a, const char *b);
void photo_folder_path(const char *category, const char *name, char *out);
int  photo_dir_name(const uint8_t entry[PHOTO_DIR_BYTES], const char *category, int folders,
                    char name[FX_Z8_LUT_NAME_BYTES]);
void photo_catalog_start(PhotoCatalog *catalog, int32_t direction, const char *cursor);
void photo_catalog_add(PhotoCatalog *catalog, const char name[FX_Z8_LUT_NAME_BYTES]);
void photo_catalog_finish(const PhotoCatalog *catalog, PhotoPage *page);
/* High-level directory scanner using v71r7_active_card_drive() + 0x402DDC6C/D1C/D6C */
int  photo_catalog_scan_directory(const char *category, int32_t direction, const char *cursor,
                                  PhotoPage *out_page);

/* --- photo_ui.c --- */
uint32_t photo_text_lookup(int32_t language, int32_t id, const uint16_t **text, uint32_t *bytes);
int      v71r7_photo_menu_lookup_text(int32_t language, uint32_t text_id,
                                      const uint16_t **text_out, uint32_t *bytes_out);
/* Declared in v71r7_lut_abi.h:
 *   int v71r7_photo_menu_resolve_text(uint32_t text_id, uint16_t *dst_utf16, uint32_t max_chars); */
void     photo_ui_deactivate(void);
uint32_t photo_ui_pending(void);
void     photo_lut_timer(void);
int      photo_lut_entry(void *page);
int      photo_lut_exit(void *page);
int      photo_lut_refresh(void *page);
int      photo_lut_activate(void *action);
int      photo_lut_back_action(void *action);

/* --- photo_worker.c --- */
/* Declared in v71r7_lut_abi.h:
 *   void v71r7_photo_menu_boot_once(void);
 *   void v71r7_photo_menu_on_card_change(void); */
void        v71r7_photo_menu_gui_poll(void);
uint64_t    photo_gui_epoch(void);
uint32_t    photo_lut_select(const char *name, uint64_t gui_epoch);
void        photo_lut_request_page(int32_t direction);
void        photo_lut_enter_folder(const char *category, uint32_t root_focus);
void        photo_lut_leave_folder(uint32_t root_focus);
void        photo_lut_close_browser(void);
int         photo_lut_page(PhotoPage *out);
PhotoStatus photo_lut_status(void);
int         photo_worker_slice(void);
uint32_t    photo_worker_pending(void);
void        photo_worker_shutdown(void);

#ifdef V71R7_HOST_TEST
int      v71r7_photo_menu_host_init(const PhotoMenuPlatOps *plat);
void     v71r7_photo_menu_host_teardown(void);
uint32_t photo_worker_idle_delay_for_test(void);
#endif

#endif /* V71R7_PHOTO_MENU_H */
