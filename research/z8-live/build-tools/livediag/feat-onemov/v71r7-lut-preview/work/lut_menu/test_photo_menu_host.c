/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Photo Shooting Menu Host ASan+UBSan Test Suite (Mac-only, 100% local).
 *
 * Covers:
 *   Suite 1: Dual-slot CRC32 journal (V1/V2 records, fault boundaries, same-letter swaps, nested 63B paths)
 *   Suite 2: .cube catalog scanning (root + 1-level categories, >20 pagination, >512 cache overflow fallback,
 *            category entry & back-to-category focus preservation)
 *   Suite 3: Multilingual photo_ui (0x1F40..0x1F4A in EN/zh-CN/zh-TW/JA via resolve_text & lookup_text,
 *            submenu 0x5D6 controller, status percentage formatting, widget lease save/restore)
 *   Suite 4: Resident photo_worker (boot once, cardslot settle wait, FXPHOTO0/1.CFG restore, chunked <=2MiB
 *            .cube streaming under FxZ8LutIoGate, >2MiB rejection, select/clear "无LUT", WYSIWYG pump,
 *            ~100ms watchdog poll cadence, card-change A<->B reload, Early OFF drain/reopen)
 *   Suite 5: Generated Bank 2 static RO table assertions (count=48, Row 45 NFP preserved, Row 47 Select LUT
 *            0x2DE7 -> Page 0x5D6, 23 child rows 0x4800..0x4816, zero BURST)
 */
#include "photo_menu.h"
#include "lut_runtime.h"
#include "../lut_preview/layer0_provider.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Host DRAM Backing & Mocked Bank 1 (lut_core / lut_preview) Symbols
 * ============================================================================ */
uint8_t v71r7_host_dram_region[0x4000];

typedef struct {
    int      flight_inits;
    int      flight_records;
    uint16_t last_flight_event;
    int      hb_enters;
    int      hb_idles;
    int      watchdog_polls;
    uint64_t last_watchdog_tick;
    int      layer0_inits;
    int      layer0_notifies;
    uint32_t last_notify_gen;
    int      last_notify_active;
    int      layer0_steps;
    uint32_t layer0_step_ret;
    int      layer0_ws_allocated;
    int      lut_prepares;
    int      lut_clears;
    int      lut_active;
    uint32_t lut_active_gen;
    uint32_t last_prep_bytes;
    char     last_prep_path[FX_Z8_LUT_PATH_BYTES];
    uint32_t last_prep_crc;
    int      force_prep_fail;
    int      cache_inval_calls;
    int      cache_inval_before_last_prep;
    int      pool_suspend_calls;
    int      pool_resume_calls;
    int      neon_allowed;
    uint32_t smp_online_helpers;
    Layer0Metrics mock_l0_metrics;
    V71r7LutRuntimeStats mock_rt_stats;
} MockBank1;

static MockBank1 g_b1;

void v71r7_flight_init(void) {
    ++g_b1.flight_inits;
    V71r7DramAnchor *a = v71r7_anchor();
    if (a->magic != V71R7_ANCHOR_MAGIC) {
        memset(a, 0, sizeof(*a));
        a->magic = V71R7_ANCHOR_MAGIC;
    }
}

void v71r7_flight_record(uint16_t event, uint16_t core_flags, uint32_t a, uint32_t b, uint32_t c) {
    (void)core_flags; (void)a; (void)b; (void)c;
    ++g_b1.flight_records;
    g_b1.last_flight_event = event;
}

void v71r7_heartbeat_enter(unsigned slot, unsigned stage, uint32_t cap_num, uint32_t aux) {
    (void)slot; (void)stage; (void)cap_num; (void)aux;
    ++g_b1.hb_enters;
}

void v71r7_heartbeat_progress(unsigned slot) { (void)slot; }

void v71r7_heartbeat_idle(unsigned slot) {
    (void)slot;
    ++g_b1.hb_idles;
}

uint32_t v71r7_watchdog_poll(uint64_t now_tick) {
    ++g_b1.watchdog_polls;
    g_b1.last_watchdog_tick = now_tick;
    return 0u;
}

typedef struct {
    uint32_t crc;
    uint32_t bytes;
    uint32_t generation;
} MockLutStream;

static uint32_t mock_crc32_update(uint32_t crc, const uint8_t *data, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc;
}

void *v71r7_lut_stream_begin(uint32_t generation, FxZ8LutProgress *prog_out) {
    MockLutStream *st = (MockLutStream *)calloc(1u, sizeof(MockLutStream));
    assert(st != NULL);
    st->crc = 0xFFFFFFFFu;
    st->bytes = 0u;
    st->generation = generation;
    if (prog_out) {
        prog_out->version = FX_Z8_LUT_PROGRESS_VERSION;
        prog_out->generation = generation;
        prog_out->phase = FX_Z8_LUT_PREPARING;
        prog_out->result = FX_OK;
    }
    return st;
}

int v71r7_lut_stream_feed(void *stream_handle, const uint8_t *chunk, uint32_t bytes,
                          FxZ8LutProgress *prog_out) {
    MockLutStream *st = (MockLutStream *)stream_handle;
    assert(st != NULL);
    if (bytes > PHOTO_MAX_CUBE_BYTES - st->bytes) {
        if (prog_out) {
            prog_out->phase = FX_Z8_LUT_ERROR;
            prog_out->result = 1u;
        }
        return 0;
    }
    if (chunk && bytes) {
        st->crc = mock_crc32_update(st->crc, chunk, bytes);
        st->bytes += bytes;
    }
    if (prog_out) {
        prog_out->version = FX_Z8_LUT_PROGRESS_VERSION;
        prog_out->generation = st->generation;
        prog_out->file_bytes = st->bytes;
        prog_out->phase = FX_Z8_LUT_PREPARING;
        prog_out->result = FX_OK;
    }
    return 1;
}

void v71r7_lut_stream_abort(void *stream_handle) {
    free(stream_handle);
}

int v71r7_lut_stream_commit(void *stream_handle, const char *path, uint32_t generation,
                            FxZ8LutProgress *prog_out) {
    MockLutStream *st = (MockLutStream *)stream_handle;
    assert(st != NULL);
    uint32_t bytes = st->bytes;
    uint32_t crc = bytes ? (st->crc ^ 0xFFFFFFFFu) : 0u;
    free(st);

    ++g_b1.lut_prepares;
    g_b1.cache_inval_before_last_prep = (g_b1.cache_inval_calls >= g_b1.lut_prepares) ? 1 : 0;
    g_b1.last_prep_bytes = bytes;
    memset(g_b1.last_prep_path, 0, sizeof(g_b1.last_prep_path));
    if (path) {
        size_t n = strlen(path);
        if (n >= sizeof(g_b1.last_prep_path)) n = sizeof(g_b1.last_prep_path) - 1u;
        memcpy(g_b1.last_prep_path, path, n);
    }
    g_b1.last_prep_crc = crc;
    if (prog_out) {
        prog_out->version = FX_Z8_LUT_PROGRESS_VERSION;
        prog_out->generation = generation;
        prog_out->file_bytes = bytes;
    }
    if (g_b1.force_prep_fail) {
        if (prog_out) {
            prog_out->phase = FX_Z8_LUT_ERROR;
            prog_out->result = (uint32_t)g_b1.force_prep_fail;
        }
        return 0;
    }
    g_b1.lut_active = 1;
    g_b1.lut_active_gen = generation;
    if (prog_out) {
        prog_out->phase = FX_Z8_LUT_READY;
        prog_out->result = FX_OK;
    }
    return 1;
}

int v71r7_lut_prepare_from_buffer(const uint8_t *buf, uint32_t bytes, const char *path,
                                  uint32_t generation, FxZ8LutProgress *prog_out) {
    ++g_b1.lut_prepares;
    g_b1.cache_inval_before_last_prep = (g_b1.cache_inval_calls >= g_b1.lut_prepares) ? 1 : 0;
    g_b1.last_prep_bytes = bytes;
    memset(g_b1.last_prep_path, 0, sizeof(g_b1.last_prep_path));
    if (path) {
        size_t n = strlen(path);
        if (n >= sizeof(g_b1.last_prep_path)) n = sizeof(g_b1.last_prep_path) - 1u;
        memcpy(g_b1.last_prep_path, path, n);
    }
    g_b1.last_prep_crc = buf ? photo_crc32(buf, bytes) : 0u;
    if (prog_out) {
        prog_out->version = FX_Z8_LUT_PROGRESS_VERSION;
        prog_out->generation = generation;
        prog_out->file_bytes = bytes;
    }
    if (g_b1.force_prep_fail) {
        if (prog_out) {
            prog_out->phase = FX_Z8_LUT_ERROR;
            prog_out->result = (uint32_t)g_b1.force_prep_fail;
        }
        return 0;
    }
    g_b1.lut_active = 1;
    g_b1.lut_active_gen = generation;
    if (prog_out) {
        prog_out->phase = FX_Z8_LUT_READY;
        prog_out->result = FX_OK;
    }
    return 1;
}

int v71r7_layer0_workspace_allocated(void) {
    return g_b1.layer0_ws_allocated;
}

void v71r7_lut_clear_active(void) {
    ++g_b1.lut_clears;
    g_b1.lut_active = 0;
    g_b1.lut_active_gen = 0u;
}

int v71r7_lut_is_active(uint32_t *gen_out, int *is_1d_out) {
    if (gen_out) *gen_out = g_b1.lut_active_gen;
    if (is_1d_out) *is_1d_out = 0;
    return g_b1.lut_active;
}

int v71r7_layer0_init(void) {
    ++g_b1.layer0_inits;
    return 0;
}

void v71r7_layer0_notify_lut_changed(uint32_t generation, int active) {
    ++g_b1.layer0_notifies;
    g_b1.last_notify_gen = generation;
    g_b1.last_notify_active = active;
}

uint32_t v71r7_layer0_worker_step(void) {
    ++g_b1.layer0_steps;
    return g_b1.layer0_step_ret;
}

void v71r7_layer0_cache_invalidate_before_swap(void) {
    ++g_b1.cache_inval_calls;
}

int v71r7_neon_sentinel_is_allowed(void) {
    return g_b1.neon_allowed ? 1 : 0;
}

uint32_t v71r7_smp_online_helpers(void) {
    return g_b1.smp_online_helpers;
}

int v71r7_lut_get_active_path(char path_out[FX_Z8_LUT_PATH_BYTES]) {
    if (!path_out) return 0;
    if (!g_b1.lut_active) {
        path_out[0] = '\0';
        return 0;
    }
    memcpy(path_out, g_b1.last_prep_path, FX_Z8_LUT_PATH_BYTES);
    return 1;
}

void v71r7_lut_runtime_get_stats(V71r7LutRuntimeStats *out) {
    if (out) *out = g_b1.mock_rt_stats;
}

const Layer0Metrics *v71r7_layer0_metrics_ptr(void) {
    return &g_b1.mock_l0_metrics;
}

void v71r7_layer0_pool_suspend(void) {
    ++g_b1.pool_suspend_calls;
}

void v71r7_layer0_pool_resume(void) {
    ++g_b1.pool_resume_calls;
}

/* ============================================================================
 * SUITE 1: Ported & Expanded Dual-Slot CRC32 Journal Tests
 * ============================================================================ */
enum { J_SLOT0, J_SLOT1, J_TEMP, J_LOOK, J_OTHER, J_NESTED, J_FILES };
enum { J_SWAP_NONE, J_SWAP_DEST_CHECK, J_SWAP_TEMP_RECHECK };

typedef struct {
    uint8_t  data[PHOTO_RECORD_BYTES + 1u];
    uint32_t length, exists;
} JFile;

typedef struct {
    JFile    cards[2][J_FILES];
    JFile    replacement[J_FILES];
    JFile   *open_file;
    int32_t  descriptor, last_fd;
    uint32_t offset, calls, fail_at, replace_at, short_write, closed_uncertain;
    uint32_t max_read, swap, swapped, dest, temp_created, temp_checks, removed_slots;
    char     drive;
    int      current;
} JCard;

static const char j_no_lut[FX_Z8_LUT_NAME_BYTES] = "";
static const char j_look[FX_Z8_LUT_NAME_BYTES] = "look.cube";
static const char j_other[FX_Z8_LUT_NAME_BYTES] = "other.CUBE";
static const char j_nested[] = "LUTS\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz.cube";
_Static_assert(sizeof(j_nested) == FX_Z8_LUT_PATH_BYTES, "exercise the longest path");

static int j_current(void *ctx) { return ((JCard *)ctx)->current; }

static unsigned j_file_index(const JCard *card, const char *name) {
    static const char *const names[J_FILES] = {
        "FXPHOTO0.CFG", "FXPHOTO1.CFG", "FXPHOTO.TMP",
        "look.cube", "other.CUBE", j_nested
    };
    assert(!memcmp(name + 1, ":\\FENESTRA\\", 11));
    assert(name[0] == card->drive);
    for (unsigned i = 0; i < J_FILES; ++i) {
        if (!strcmp(name + 12, names[i])) return i;
    }
    assert(0 && "unexpected filename");
    return J_FILES;
}

static JFile *j_file(JCard *card, const char *name) {
    JFile *mounted = card->swapped ? card->replacement : card->cards[card->drive - 'A'];
    return mounted + j_file_index(card, name);
}

static int j_at_fault(JCard *card) {
    ++card->calls;
    if (card->calls == card->replace_at) card->current = 0;
    return card->calls == card->fail_at;
}

static FxResult j_open(void *ctx, const char *name, uint32_t flags, int32_t *fd) {
    JCard *card = (JCard *)ctx;
    assert(card->current && !card->open_file);
    assert(!card->closed_uncertain);
    if (j_at_fault(card)) return FX_IO;
    unsigned slot = j_file_index(card, name);
    if (!flags && card->temp_created) {
        if (slot == J_TEMP) ++card->temp_checks;
        if ((slot == card->dest && card->swap == J_SWAP_DEST_CHECK) ||
            (slot == J_TEMP && card->temp_checks == 2 && card->swap == J_SWAP_TEMP_RECHECK))
            card->swapped = 1;
    }
    JFile *f = j_file(card, name);
    if (flags) {
        assert(flags == 0x101u);
        if (f->exists) return FX_IO;
        f->exists = 1u;
        f->length = 0u;
        assert(slot == J_TEMP);
        card->temp_created = 1u;
    } else if (!f->exists) {
        return FX_EMPTY;
    }
    card->open_file = f;
    card->offset = 0u;
    *fd = card->descriptor = ++card->last_fd;
    return FX_OK;
}

static FxResult j_read(void *ctx, int32_t fd, void *out, uint32_t amount, uint32_t *done) {
    JCard *card = (JCard *)ctx;
    assert(card->current && card->open_file && card->descriptor == fd);
    assert((uintptr_t)out % 64u == 0u);
    if (j_at_fault(card)) return FX_IO;
    uint32_t left = card->open_file->length - card->offset;
    if (amount > left) amount = left;
    if (amount > card->max_read) amount = card->max_read;
    memcpy(out, card->open_file->data + card->offset, amount);
    card->offset += amount;
    *done = amount;
    return FX_OK;
}

static FxResult j_write(void *ctx, int32_t fd, const void *in, uint32_t amount, uint32_t *done) {
    JCard *card = (JCard *)ctx;
    assert(card->current && card->open_file && card->descriptor == fd && !card->offset);
    if (j_at_fault(card)) return FX_IO;
    assert(amount == PHOTO_RECORD_BYTES && !card->open_file->length);
    if (card->short_write) --amount;
    memcpy(card->open_file->data, in, amount);
    card->open_file->length = *done = amount;
    return FX_OK;
}

static FxResult j_close(void *ctx, int32_t fd) {
    JCard *card = (JCard *)ctx;
    assert(card->open_file && card->descriptor == fd && !card->closed_uncertain);
    if (j_at_fault(card)) { card->closed_uncertain = 1u; return FX_CLOSE; }
    card->open_file = NULL;
    return FX_OK;
}

static FxResult j_remove(void *ctx, const char *name) {
    JCard *card = (JCard *)ctx;
    assert(card->current && !card->open_file);
    JFile *dest = j_file(card, name);
    if (j_file_index(card, name) <= J_SLOT1) ++card->removed_slots;
    if (j_at_fault(card)) return FX_IO;
    assert(dest->exists);
    dest->exists = 0u;
    return FX_OK;
}

static FxResult j_rename(void *ctx, const char *from, const char *to) {
    JCard *card = (JCard *)ctx;
    assert(card->current && !card->open_file);
    JFile *src = j_file(card, from), *dst = j_file(card, to);
    if (j_at_fault(card)) return FX_IO;
    assert(src->exists && !dst->exists && src->length == PHOTO_RECORD_BYTES);
    *dst = *src;
    src->exists = 0u;
    return FX_OK;
}

static PhotoFileIO j_io(JCard *card) {
    return (PhotoFileIO){ card, j_current, j_open, j_read, j_write, j_close, j_remove, j_rename };
}

static void j_checksum(uint8_t *data, size_t bytes) {
    uint32_t crc = photo_crc32(data, bytes - 4u);
    for (unsigned i = 0; i < 4u; ++i) data[bytes - 4u + i] = (uint8_t)(crc >> (8u * i));
}

static void j_seed(JFile *f, uint64_t generation, const char *name) {
    PhotoChoice rec;
    memset(&rec, 0, sizeof(rec));
    rec.generation = generation;
    size_t len = strlen(name);
    assert(len < sizeof(rec.path));
    memcpy(rec.path, name, len);
    assert(photo_choice_write(&rec, f->data) == FX_OK);
    f->length = PHOTO_RECORD_BYTES;
    f->exists = 1u;
}

static void j_seed_v1(JFile *f, uint64_t generation, const char *name) {
    memset(f->data, 0, sizeof(f->data));
    memcpy(f->data, "FXPHOTO\0", 8u);
    f->data[8] = 1u;
    f->data[10] = PHOTO_RECORD_V1_BYTES;
    for (unsigned i = 0; i < 8u; ++i) f->data[12u + i] = (uint8_t)(generation >> (8u * i));
    f->data[20] = (uint8_t)(*name != '\0');
    f->data[21] = (uint8_t)strlen(name);
    memcpy(f->data + 24u, name, strlen(name));
    j_checksum(f->data, PHOTO_RECORD_V1_BYTES);
    f->length = PHOTO_RECORD_V1_BYTES;
    f->exists = 1u;
}

static JCard j_fixture(char drive, unsigned active, uint32_t max_read) {
    JCard card;
    memset(&card, 0, sizeof(card));
    card.drive = drive;
    card.current = 1;
    card.max_read = max_read;
    card.dest = active ^ 1u;
    j_seed_v1(&card.cards[drive - 'A'][active], 2u, j_look);
    j_seed(&card.cards[drive - 'A'][active ^ 1u], 1u, j_no_lut);
    j_seed(&card.cards[drive - 'A'][J_TEMP], 99u, j_other);
    j_seed(&card.cards['B' - drive][active], 4u, j_other);
    return card;
}

static void j_reopen(JCard *card) {
    card->current = 1;
    card->open_file = NULL;
    card->fail_at = card->replace_at = card->closed_uncertain = 0u;
}

static void test_suite1_journal(void) {
    /* Record cases (V1 + V2 CRC vectors, corruption, bounds) */
    JCard card;
    memset(&card, 0, sizeof(card));
    card.drive = 'A';
    card.current = 1;
    card.max_read = 65u;
    JFile *files = card.cards[0];
    PhotoFileIO ops = j_io(&card);
    PhotoJournal chosen;

    j_seed(&files[J_TEMP], 10u, j_other);
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_EMPTY);
    j_seed_v1(&files[J_SLOT0], 1u, j_no_lut);
    j_seed_v1(&files[J_SLOT1], 2u, j_look);
    assert(!memcmp(files[J_SLOT0].data + 60, "\x44\x79\xaa\xd4", 4));
    assert(!memcmp(files[J_SLOT1].data + 60, "\xfb\x62\xd6\x1c", 4));
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_OK && chosen.slot == 1u);
    assert(photo_file_exists(&ops, 'A', chosen.choice.path) == FX_EMPTY);
    files[J_LOOK].exists = 1u;
    assert(photo_file_exists(&ops, 'A', chosen.choice.path) == FX_OK);

    files[J_SLOT1].data[60] ^= 1u;
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_OK && chosen.slot == 0u);
    files[J_SLOT0].data[60] ^= 1u;
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_INTEGRITY);
    assert(photo_journal_save(&ops, 'A', j_look) == FX_OK);
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_OK && chosen.slot == 0u);

    j_seed_v1(&files[J_SLOT1], 1u, j_look);
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_OK && chosen.slot == 0u);
    j_seed(&files[J_SLOT1], 1u, j_other);
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_INTEGRITY);
    assert(photo_journal_save(&ops, 'A', j_no_lut) == FX_INTEGRITY);
    ++files[J_SLOT1].length;
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_OK && chosen.slot == 0u);

    card.short_write = 1u;
    assert(photo_journal_save(&ops, 'A', j_no_lut) == FX_IO);
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_OK && chosen.slot == 0u);
    card.short_write = 0u;
    j_seed(&files[J_SLOT0], UINT64_MAX, j_look);
    assert(photo_journal_save(&ops, 'A', j_no_lut) == FX_BOUNDS);

    const uint8_t reserved[] = { 21, 22, 23, 24, 55, 56, 57, 58, 59 };
    for (unsigned i = 0; i < sizeof(reserved); ++i) {
        j_seed_v1(&files[J_SLOT0], 1u, j_no_lut);
        files[J_SLOT0].data[reserved[i]] = 1u;
        j_checksum(files[J_SLOT0].data, PHOTO_RECORD_V1_BYTES);
        assert(photo_choice_read(files[J_SLOT0].data, PHOTO_RECORD_V1_BYTES, &chosen.choice) == FX_INTEGRITY);
    }

    /* Nested 63-byte path cases on drive B */
    memset(&card, 0, sizeof(card));
    card.drive = 'B';
    card.current = 1;
    card.max_read = 13u;
    files = card.cards[1];
    ops = j_io(&card);
    j_seed(&files[J_SLOT0], 100u, j_nested);
    j_seed_v1(&files[J_SLOT1], 101u, j_look);
    assert(photo_journal_load(&ops, 'B', &chosen) == FX_OK && chosen.slot == 1u);
    assert(photo_journal_save(&ops, 'B', j_nested) == FX_OK);
    assert(files[J_SLOT0].length == PHOTO_RECORD_BYTES && files[J_SLOT0].data[8] == 2u);
    assert(files[J_SLOT0].data[21] == 63u && !memcmp(files[J_SLOT0].data + 24, j_nested, sizeof(j_nested)));
    assert(photo_journal_load(&ops, 'B', &chosen) == FX_OK && chosen.slot == 0u);
    assert(chosen.choice.generation == 102u && !strcmp(chosen.choice.path, j_nested));
    assert(photo_file_exists(&ops, 'B', chosen.choice.path) == FX_EMPTY);
    files[J_NESTED].exists = 1u;
    assert(photo_file_exists(&ops, 'B', chosen.choice.path) == FX_OK);
    assert(photo_journal_save(&ops, 'B', "LUTS\\..\\look.cube") == FX_ARGUMENT);
    assert(photo_journal_save(&ops, 'B', "BAD\\cat\\look.cube") == FX_ARGUMENT);

    const uint8_t invalid[] = { 8, 10, 20, 21, 22, 23, 25, 87, 88, 91, 92, 95 };
    for (unsigned i = 0; i < sizeof(invalid); ++i) {
        j_seed(&files[J_SLOT0], 102u, j_nested);
        files[J_SLOT0].data[invalid[i]] ^= 1u;
        if (invalid[i] < 92u) j_checksum(files[J_SLOT0].data, PHOTO_RECORD_BYTES);
        assert(photo_journal_load(&ops, 'B', &chosen) == FX_OK && chosen.slot == 1u);
    }

    /* Fault boundaries */
    unsigned injected = 0u;
    for (char drive = 'A'; drive <= 'B'; ++drive) {
        for (unsigned active = 0; active < 2u; ++active) {
            for (unsigned tombstone = 0; tombstone < 2u; ++tombstone) {
                const char *name = tombstone ? j_no_lut : j_nested;
                JCard clean = j_fixture(drive, active, 11u);
                ops = j_io(&clean);
                assert(photo_journal_save(&ops, drive, name) == FX_OK);
                for (unsigned replace = 0; replace < 2u; ++replace) {
                    for (uint32_t step = 1u; step <= clean.calls; ++step) {
                        JCard c = j_fixture(drive, active, 11u);
                        c.fail_at = replace ? 0u : step;
                        c.replace_at = replace ? step : 0u;
                        ops = j_io(&c);
                        assert(photo_journal_save(&ops, drive, name) != FX_OK);
                        assert(!memcmp(c.cards[drive - 'A'][active].data,
                                       clean.cards[drive - 'A'][active].data, PHOTO_RECORD_BYTES));
                        assert(c.cards[drive - 'A'][active].exists);
                        j_reopen(&c);
                        PhotoJournal restored;
                        assert(photo_journal_load(&ops, drive, &restored) == FX_OK);
                        if (restored.choice.generation == 2u) assert(!strcmp(restored.choice.path, j_look));
                        else {
                            assert(restored.choice.generation == 3u);
                            assert(!strcmp(restored.choice.path, name));
                        }
                        c.drive = (drive == 'A') ? 'B' : 'A';
                        assert(photo_journal_load(&ops, c.drive, &restored) == FX_OK);
                        assert(restored.choice.generation == 4u && !strcmp(restored.choice.path, j_other));
                        ++injected;
                    }
                }
            }
        }
    }

    /* Same-letter card swaps */
    unsigned swaps = 0u;
    for (char drive = 'A'; drive <= 'B'; ++drive) {
        for (unsigned active = J_SLOT0; active <= J_SLOT1; ++active) {
            for (unsigned scenario = 0; scenario < 4u; ++scenario) {
                JCard c = j_fixture(drive, active, 11u);
                c.swap = (scenario < 2u) ? J_SWAP_DEST_CHECK : J_SWAP_TEMP_RECHECK;
                j_seed_v1(&c.replacement[active], scenario == 1u ? 5u : 2u, j_look);
                j_seed(&c.replacement[active ^ 1u], scenario == 1u ? 1u : 9u, j_other);
                if (scenario == 0u) c.cards[drive - 'A'][active ^ 1u].exists = 0u;
                if (scenario < 2u) j_seed(&c.replacement[J_TEMP], 3u, j_no_lut);
                if (scenario == 3u) j_seed(&c.replacement[J_TEMP], 3u, j_other);
                JFile unchanged = c.replacement[active ^ 1u];
                ops = j_io(&c);
                assert(photo_journal_save(&ops, drive, j_no_lut) == FX_STALE);
                assert(c.swapped && c.current && !c.removed_slots);
                JFile *dest = &c.replacement[active ^ 1u];
                assert(dest->exists && dest->length == unchanged.length &&
                       !memcmp(dest->data, unchanged.data, unchanged.length));
                assert(c.cards[drive - 'A'][active].exists);
                ++swaps;
            }
        }
    }
    printf("[PASS] Suite 1 (Journal V1/V2): %u fault injections + %u same-letter swaps verified\n",
           injected, swaps);
}

/* ============================================================================
 * Virtual Filesystem + Mock UI Harness (for Suites 2, 3, 4)
 * ============================================================================ */
#define VFS_MAX_FILES 640
#define VFS_MAX_DIRS  32
#define VFS_MAX_FDS   8

typedef struct {
    int      used;
    char     path[PHOTO_PATH_BYTES];
    uint8_t *data;
    uint32_t len;
    int      is_dir;
} VfsNode;

typedef struct {
    int      open;
    int      is_dir;
    int      node_idx;
    char     dir_path[PHOTO_PATH_BYTES];
    uint32_t offset;
    uint32_t dir_cursor;
} VfsHandle;

typedef struct {
    uint8_t  raw[0x100];
    uint16_t wide_text[64];
    char     ascii_text[64];
    uint32_t bound_slot;
    uint32_t bound_kind;
    uint32_t dirty;
    uint32_t dynamic;
    int16_t  rect[4];
} MockWidget;

typedef struct {
    char      drive;
    int       drive_ready;
    uint64_t  ticks;
    uint32_t  language;
    int       task_started;
    uint32_t  task_prio;
    uint32_t  task_stack_bytes;
    int       alloc_count;
    int       free_count;
    uint32_t  max_alloc_limit;
    uint32_t  max_alloc_seen;
    uint32_t  max_read_chunk;

    VfsNode   nodes[VFS_MAX_FILES];
    VfsHandle fds[VFS_MAX_FDS];
    int       open_fd_count;
    int       open_dir_count;
    uint32_t  open_calls;
    uint32_t  dir_open_calls;

    /* Mock Nikon UI state */
    uint8_t    page_buf[0x80];
    void      *cur_page;
    MockWidget row_widgets[PHOTO_LUT_ROWS];
    MockWidget status_widget;
    uint8_t    disabled_flags[PHOTO_LUT_ROWS];
    uint8_t    font_records[0x240][16];
    uint8_t    text_records[0x140][24];
    uint8_t    text_buffers[0x140][64];
    int16_t    view_rect[4];
    uint32_t   focused_row;
    int        timer_running;
    void     (*timer_cb)(void);
    int        repaint_count;
    int        stock_back_calls;
} MockEnv;

static void vfs_reset(MockEnv *e) {
    memset(e, 0, sizeof(*e));
    e->drive = 'A';
    e->drive_ready = 1;
    e->language = PHOTO_LANG_EN;
    e->max_read_chunk = 32768u;
    for (unsigned i = 0; i < 0x140u; ++i) {
        uintptr_t bp = (uintptr_t)e->text_buffers[i];
        memcpy(e->text_records[i] + 8u, &bp, sizeof(bp));
    }
    uintptr_t desc_va = (uintptr_t)&v71r7_lut_child_desc;
    memcpy(e->page_buf + 0x50u, &desc_va, sizeof(desc_va));
    e->cur_page = e->page_buf;
}

static void vfs_destroy(MockEnv *e) {
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        free(e->nodes[i].data);
        e->nodes[i].data = NULL;
        e->nodes[i].used = 0;
    }
}

static int vfs_find(MockEnv *e, const char *path) {
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        if (e->nodes[i].used && !strcmp(e->nodes[i].path, path)) return i;
    }
    return -1;
}

static void vfs_add_dir(MockEnv *e, const char *path) {
    if (vfs_find(e, path) >= 0) return;
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        if (!e->nodes[i].used) {
            e->nodes[i].used = 1;
            e->nodes[i].is_dir = 1;
            snprintf(e->nodes[i].path, sizeof(e->nodes[i].path), "%s", path);
            return;
        }
    }
    assert(0 && "vfs full");
}

static void vfs_put_file(MockEnv *e, const char *path, const uint8_t *data, uint32_t len) {
    int idx = vfs_find(e, path);
    if (idx < 0) {
        for (int i = 0; i < VFS_MAX_FILES; ++i) {
            if (!e->nodes[i].used) { idx = i; break; }
        }
    }
    assert(idx >= 0);
    free(e->nodes[idx].data);
    e->nodes[idx].used = 1;
    e->nodes[idx].is_dir = 0;
    snprintf(e->nodes[idx].path, sizeof(e->nodes[idx].path), "%s", path);
    e->nodes[idx].len = len;
    e->nodes[idx].data = len ? (uint8_t *)malloc(len) : NULL;
    if (len && data) memcpy(e->nodes[idx].data, data, len);
}

static void vfs_put_journal(MockEnv *e, char drive, unsigned slot, uint64_t gen, const char *lut_path) {
    PhotoChoice ch;
    memset(&ch, 0, sizeof(ch));
    ch.generation = gen;
    snprintf(ch.path, sizeof(ch.path), "%s", lut_path);
    uint8_t rec[PHOTO_RECORD_BYTES];
    assert(photo_choice_write(&ch, rec) == FX_OK);
    char p[PHOTO_PATH_BYTES];
    photo_card_path(drive, slot ? "FXPHOTO1.CFG" : "FXPHOTO0.CFG", p);
    vfs_put_file(e, p, rec, PHOTO_RECORD_BYTES);
}

/* PlatOps callbacks */
static char m_card_drive(void *u) { return ((MockEnv *)u)->drive; }
static int  m_card_ready(void *u) { return ((MockEnv *)u)->drive_ready; }

static int32_t m_open(void *u, const char *path, uint32_t flags, uint32_t mode) {
    (void)mode;
    MockEnv *e = (MockEnv *)u;
    ++e->open_calls;
    int idx = vfs_find(e, path);
    if (flags == 0x101u) {
        if (idx >= 0) return (int32_t)0x80000020u;
        vfs_put_file(e, path, NULL, 0u);
        idx = vfs_find(e, path);
    } else if (idx < 0 || e->nodes[idx].is_dir) {
        return (int32_t)0x80000010u;
    }
    for (int fd = 0; fd < VFS_MAX_FDS; ++fd) {
        if (!e->fds[fd].open) {
            e->fds[fd].open = 1;
            e->fds[fd].is_dir = 0;
            e->fds[fd].node_idx = idx;
            e->fds[fd].offset = 0u;
            ++e->open_fd_count;
            return fd;
        }
    }
    return -1;
}

static uint32_t m_read(void *u, int32_t fd, void *dst, uint32_t bytes, uint32_t *done) {
    MockEnv *e = (MockEnv *)u;
    assert(fd >= 0 && fd < VFS_MAX_FDS && e->fds[fd].open && !e->fds[fd].is_dir);
    VfsNode *n = &e->nodes[e->fds[fd].node_idx];
    uint32_t left = n->len - e->fds[fd].offset;
    uint32_t take = bytes < left ? bytes : left;
    if (e->max_read_chunk && take > e->max_read_chunk) take = e->max_read_chunk;
    if (take) memcpy(dst, n->data + e->fds[fd].offset, take);
    e->fds[fd].offset += take;
    *done = take;
    return 0u;
}

static uint32_t m_write(void *u, int32_t fd, const void *src, uint32_t bytes, uint32_t *done, void *ex) {
    (void)ex;
    MockEnv *e = (MockEnv *)u;
    assert(fd >= 0 && fd < VFS_MAX_FDS && e->fds[fd].open && !e->fds[fd].is_dir);
    VfsNode *n = &e->nodes[e->fds[fd].node_idx];
    uint8_t *nb = (uint8_t *)realloc(n->data, e->fds[fd].offset + bytes);
    assert(nb != NULL);
    n->data = nb;
    memcpy(n->data + e->fds[fd].offset, src, bytes);
    e->fds[fd].offset += bytes;
    if (e->fds[fd].offset > n->len) n->len = e->fds[fd].offset;
    *done = bytes;
    return 0u;
}

static uint32_t m_close(void *u, int32_t fd) {
    MockEnv *e = (MockEnv *)u;
    assert(fd >= 0 && fd < VFS_MAX_FDS && e->fds[fd].open && !e->fds[fd].is_dir);
    e->fds[fd].open = 0;
    --e->open_fd_count;
    return 0u;
}

static uint32_t m_remove(void *u, const char *path) {
    MockEnv *e = (MockEnv *)u;
    int idx = vfs_find(e, path);
    if (idx < 0) return 0x80000010u;
    free(e->nodes[idx].data);
    memset(&e->nodes[idx], 0, sizeof(e->nodes[idx]));
    return 0u;
}

static uint32_t m_rename(void *u, const char *from, const char *to) {
    MockEnv *e = (MockEnv *)u;
    int s = vfs_find(e, from), d = vfs_find(e, to);
    if (s < 0 || d >= 0) return 1u;
    snprintf(e->nodes[s].path, sizeof(e->nodes[s].path), "%s", to);
    return 0u;
}

static uint32_t m_dir_open(void *u, const char *path, int32_t *fd_out) {
    MockEnv *e = (MockEnv *)u;
    ++e->dir_open_calls;
    int idx = vfs_find(e, path);
    if (idx < 0 || !e->nodes[idx].is_dir) {
        *fd_out = -1;
        return 0x80000010u;
    }
    for (int fd = 0; fd < VFS_MAX_FDS; ++fd) {
        if (!e->fds[fd].open) {
            e->fds[fd].open = 1;
            e->fds[fd].is_dir = 1;
            snprintf(e->fds[fd].dir_path, sizeof(e->fds[fd].dir_path), "%s", path);
            e->fds[fd].dir_cursor = 0u;
            ++e->open_dir_count;
            *fd_out = fd;
            return 0u;
        }
    }
    *fd_out = -1;
    return 1u;
}

static void encode_z8_dir_entry(uint8_t out[PHOTO_DIR_BYTES], const char *name, int is_dir, uint32_t size) {
    memset(out, 0, PHOTO_DIR_BYTES);
    out[0x20eu] = is_dir ? 0x10u : 0x20u;
    out[0x218u] = (uint8_t)(size & 0xFFu);
    out[0x219u] = (uint8_t)((size >> 8) & 0xFFu);
    out[0x21au] = (uint8_t)((size >> 16) & 0xFFu);
    out[0x21bu] = (uint8_t)((size >> 24) & 0xFFu);
    /* Write UTF-16LE long name at offset 14 */
    size_t n = strlen(name);
    for (size_t i = 0; i < n && i < 64u; ++i) {
        out[14u + 2u * i] = (uint8_t)name[i];
        out[15u + 2u * i] = 0u;
    }
}

static uint32_t m_dir_next(void *u, int32_t fd, void *entry_buf) {
    MockEnv *e = (MockEnv *)u;
    assert(fd >= 0 && fd < VFS_MAX_FDS && e->fds[fd].open && e->fds[fd].is_dir);
    size_t plen = strlen(e->fds[fd].dir_path);
    while (e->fds[fd].dir_cursor < VFS_MAX_FILES) {
        uint32_t i = e->fds[fd].dir_cursor++;
        if (!e->nodes[i].used) continue;
        if (strncmp(e->nodes[i].path, e->fds[fd].dir_path, plen) != 0) continue;
        if (e->nodes[i].path[plen] != '\\') continue;
        const char *child = e->nodes[i].path + plen + 1u;
        if (strchr(child, '\\') != NULL) continue; /* immediate children only */
        encode_z8_dir_entry((uint8_t *)entry_buf, child, e->nodes[i].is_dir, e->nodes[i].len);
        return 0u;
    }
    return 0x80000030u; /* EOF */
}

static uint32_t m_dir_close(void *u, int32_t fd) {
    MockEnv *e = (MockEnv *)u;
    assert(fd >= 0 && fd < VFS_MAX_FDS && e->fds[fd].open && e->fds[fd].is_dir);
    e->fds[fd].open = 0;
    --e->open_dir_count;
    return 0u;
}

static void *m_alloc(void *u, uint32_t bytes, uint32_t align) {
    (void)align;
    MockEnv *e = (MockEnv *)u;
    if (bytes > e->max_alloc_seen) e->max_alloc_seen = bytes;
    if (e->max_alloc_limit && bytes > e->max_alloc_limit) return NULL;
    ++e->alloc_count;
    return calloc(1u, bytes);
}

static void m_free(void *u, void *ptr) {
    MockEnv *e = (MockEnv *)u;
    if (ptr) {
        ++e->free_count;
        free(ptr);
    }
}

static int m_task_start(void *u, void (*entry)(void *), void *arg, void *stack, uint32_t sb, uint32_t prio) {
    (void)entry; (void)arg;
    MockEnv *e = (MockEnv *)u;
    assert(stack != NULL && ((uintptr_t)stack & 15u) == 0u);
    assert(sb == PHOTO_WORKER_STACK_BYTES);
    assert(prio == PHOTO_WORKER_PRIORITY);
    e->task_started = 1;
    e->task_prio = prio;
    e->task_stack_bytes = sb;
    return 1;
}

static int32_t m_delay(void *u, uint32_t ms) { (void)u; (void)ms; return 0; }
static uint64_t m_ticks(void *u) { return ((MockEnv *)u)->ticks; }
static uint32_t m_lang(void *u) { return ((MockEnv *)u)->language; }
static void *m_cur_page(void *u) { return ((MockEnv *)u)->cur_page; }

static void *m_find_widget(void *u, uint32_t item, void *parent) {
    (void)parent;
    MockEnv *e = (MockEnv *)u;
    if (item == 0x1405u) return &e->status_widget;
    if (item >= PHOTO_LUT_ROW && item < PHOTO_LUT_ROW + PHOTO_LUT_ROWS)
        return &e->row_widgets[item - PHOTO_LUT_ROW];
    return NULL;
}

static void *m_find_child(void *u, void *page, uint32_t item, uint32_t child) {
    (void)u; (void)page; (void)item; (void)child;
    return NULL;
}

static void m_draw_fence(void *u) { (void)u; }
static void m_dirty(void *u, void *node, uint32_t f) { (void)u; ((MockWidget *)node)->dirty = f; }
static void m_dynamic(void *u, void *node, uint32_t f) { (void)u; ((MockWidget *)node)->dynamic = f; }

static void m_set_text(void *u, const char *ascii, uint32_t slot, uint32_t font) {
    (void)font;
    MockEnv *e = (MockEnv *)u;
    if (slot < 0x140u) {
        size_t n = strlen(ascii);
        if (n > 50u) n = 50u;
        memcpy(e->text_buffers[slot], ascii, n);
        e->text_buffers[slot][n] = 0u;
    }
}

static void m_set_wide(void *u, const uint16_t *wide, uint32_t slot, uint32_t font) {
    (void)font;
    MockEnv *e = (MockEnv *)u;
    if (slot < 0x140u) {
        size_t n = 0;
        while (wide[n] && n < 25u) ++n;
        memcpy(e->text_buffers[slot], wide, (n + 1u) * 2u);
    }
}

static int m_bind_text(void *u, void *node, uint32_t slot, uint32_t kind) {
    MockEnv *e = (MockEnv *)u;
    MockWidget *w = (MockWidget *)node;
    w->bound_slot = slot;
    w->bound_kind = kind;
    if (kind == 3u && slot < 0x140u) {
        memcpy(w->ascii_text, e->text_buffers[slot], 51u);
    } else if (kind == 4u && slot < 0x140u) {
        memcpy(w->wide_text, e->text_buffers[slot], 52u);
    }
    return 0;
}

static uint32_t m_text_font(void *u, uint32_t lang, uint32_t sample) {
    (void)u; (void)sample;
    return lang == PHOTO_LANG_ZH_CN ? 2u : (lang == PHOTO_LANG_ZH_TW ? 0u : 13u);
}

static uint32_t m_row_count(void *u, void *page) {
    (void)page;
    MockEnv *e = (MockEnv *)u;
    uint32_t cnt = 0u;
    for (uint32_t i = 0; i < PHOTO_LUT_ROWS; ++i)
        if (!e->disabled_flags[i]) ++cnt;
    return cnt;
}

static void m_row_disable(void *u, void *page, uint32_t row, uint32_t dis) {
    (void)page;
    MockEnv *e = (MockEnv *)u;
    if (row < PHOTO_LUT_ROWS) e->disabled_flags[row] = (uint8_t)dis;
}

static void m_row_focus(void *u, void *page, uint32_t row, uint32_t focus) {
    (void)page;
    MockEnv *e = (MockEnv *)u;
    if (focus) e->focused_row = row;
}

static void m_list_layout(void *u, void *page) { (void)u; (void)page; }
static void m_view_layout(void *u, void *page) { (void)u; (void)page; }
static void m_scrollbar(void *u, void *page) { (void)u; (void)page; }
static void m_set_rect(void *u, void *widget, const int16_t *b) {
    (void)u;
    memcpy(((MockWidget *)widget)->rect, b, 8u);
}
static uint32_t m_view_id(void *u, uint32_t g) { (void)u; return g; }
static void m_view_set(void *u, uint32_t id, const int16_t *r) {
    (void)id;
    memcpy(((MockEnv *)u)->view_rect, r, 8u);
}
static void m_view_rect(void *u, uint32_t id, int16_t *r) {
    (void)id;
    memcpy(r, ((MockEnv *)u)->view_rect, 8u);
}
static void m_mark(void *u, void *t, uintptr_t fn) { (void)u; (void)t; (void)fn; }
static void m_redraw(void *u, uintptr_t fn, uint32_t a, uint32_t b) {
    (void)fn; (void)a; (void)b;
    ++((MockEnv *)u)->repaint_count;
}
static int m_timer_start(void *u, uint32_t slot, uint32_t ms, void (*cb)(void)) {
    (void)slot; (void)ms;
    MockEnv *e = (MockEnv *)u;
    e->timer_running = 1;
    e->timer_cb = cb;
    return 1;
}
static void m_timer_cancel(void *u, uint32_t slot) {
    (void)slot;
    MockEnv *e = (MockEnv *)u;
    e->timer_running = 0;
    e->timer_cb = NULL;
}
static int m_timer_active(void *u, void (**cb_out)(void)) {
    MockEnv *e = (MockEnv *)u;
    if (cb_out) *cb_out = e->timer_cb;
    return e->timer_running;
}
static void m_stock_back(void *u, void *action) { (void)action; ++((MockEnv *)u)->stock_back_calls; }
static void *m_font_rec(void *u, uint32_t font) { return ((MockEnv *)u)->font_records[font]; }
static void *m_text_rec(void *u, uint32_t idx) { return ((MockEnv *)u)->text_records[0xfcu + idx]; }
static void *m_dis_flags(void *u) { return ((MockEnv *)u)->disabled_flags; }

static PhotoMenuPlatOps make_plat(MockEnv *e) {
    return (PhotoMenuPlatOps){
        .user = e,
        .card_drive = m_card_drive,
        .card_drive_ready = m_card_ready,
        .file_open = m_open,
        .file_read = m_read,
        .file_write = m_write,
        .file_close = m_close,
        .file_remove = m_remove,
        .file_rename = m_rename,
        .dir_open = m_dir_open,
        .dir_next = m_dir_next,
        .dir_close = m_dir_close,
        .heap_alloc = m_alloc,
        .heap_free = m_free,
        .task_start = m_task_start,
        .task_delay = m_delay,
        .now_ticks = m_ticks,
        .get_language = m_lang,
        .current_page = m_cur_page,
        .find_widget = m_find_widget,
        .find_child = m_find_child,
        .draw_fence = m_draw_fence,
        .dirty = m_dirty,
        .dynamic_flag = m_dynamic,
        .set_text = m_set_text,
        .set_wide = m_set_wide,
        .bind_text = m_bind_text,
        .text_font = m_text_font,
        .row_count = m_row_count,
        .row_disable = m_row_disable,
        .row_focus = m_row_focus,
        .list_layout = m_list_layout,
        .view_layout = m_view_layout,
        .scrollbar = m_scrollbar,
        .set_rect = m_set_rect,
        .view_id = m_view_id,
        .view_set = m_view_set,
        .view_rect = m_view_rect,
        .mark = m_mark,
        .redraw = m_redraw,
        .timer_start = m_timer_start,
        .timer_cancel = m_timer_cancel,
        .timer_active = m_timer_active,
        .stock_back_action = m_stock_back,
        .font_record_ptr = m_font_rec,
        .text_record_ptr = m_text_rec,
        .disabled_flags_ptr = m_dis_flags,
    };
}

static void pump_slices(int n) {
    for (int i = 0; i < n; ++i) (void)photo_worker_slice();
}

static void set_page_row(MockEnv *e, uint16_t row, uint16_t scroll) {
    memcpy(e->page_buf + 0x32u, &row, 2u);
    memcpy(e->page_buf + 0x58u, &scroll, 2u);
}

static int u16_eq(const uint16_t *a, const uint16_t *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        ++a; ++b;
    }
    return *a == *b;
}

/* ============================================================================
 * SUITE 2: Catalog Scanning, >20 Pagination, >512 Overflow & Back-to-Category
 * ============================================================================ */
static void test_suite2_catalog_and_pagination(void) {
    memset(v71r7_host_dram_region, 0, sizeof(v71r7_host_dram_region));
    memset(&g_b1, 0, sizeof(g_b1));
    MockEnv env;
    vfs_reset(&env);
    PhotoMenuPlatOps plat = make_plat(&env);

    /* Validate photo_dir_name edge cases */
    uint8_t de[PHOTO_DIR_BYTES];
    char out_name[FX_Z8_LUT_NAME_BYTES];
    encode_z8_dir_entry(de, "look.cube", 0, 1024u);
    assert(photo_dir_name(de, "~", 0, out_name) == 1 && !strcmp(out_name, "look.cube"));
    encode_z8_dir_entry(de, "empty.cube", 0, 0u);
    assert(photo_dir_name(de, "~", 0, out_name) == 0); /* 0 bytes rejected */
    encode_z8_dir_entry(de, "huge.cube", 0, PHOTO_MAX_CUBE_BYTES + 1u);
    assert(photo_dir_name(de, "~", 0, out_name) == 0); /* > 2 MiB rejected */
    encode_z8_dir_entry(de, "readme.txt", 0, 500u);
    assert(photo_dir_name(de, "~", 0, out_name) == 0);
    encode_z8_dir_entry(de, "01-Fuji-Film", 1, 0u);
    assert(photo_dir_name(de, NULL, 1, out_name) == 1 && !strcmp(out_name, "01-Fuji-Film"));
    de[0x20eu] = 0x08u; /* volume label rejected */
    assert(photo_dir_name(de, NULL, 0, out_name) == 0);

    /* Seed root LUTs + 4 categories, including one with 45 .cube files (> 2 pages) */
    vfs_add_dir(&env, "A:\\FENESTRA");
    vfs_add_dir(&env, "A:\\FENESTRA\\LUTS");
    vfs_add_dir(&env, "A:\\FENESTRA\\LUTS\\01-Fuji-Film");
    vfs_add_dir(&env, "A:\\FENESTRA\\LUTS\\02-Kodak-Negative");
    vfs_add_dir(&env, "A:\\FENESTRA\\LUTS\\07-Cinema-Print");
    vfs_add_dir(&env, "A:\\FENESTRA\\LUTS\\Custom_Cat");

    const uint8_t dummy_cube[] = "LUT_3D_SIZE 2\n0 0 0\n";
    vfs_put_file(&env, "A:\\FENESTRA\\root_one.cube", dummy_cube, (uint32_t)sizeof(dummy_cube) - 1u);
    vfs_put_file(&env, "A:\\FENESTRA\\LUTS\\01-Fuji-Film\\classic.cube", dummy_cube, (uint32_t)sizeof(dummy_cube) - 1u);
    for (int i = 0; i < 45; ++i) {
        char p[PHOTO_PATH_BYTES];
        snprintf(p, sizeof(p), "A:\\FENESTRA\\LUTS\\02-Kodak-Negative\\film_%02d.cube", i);
        vfs_put_file(&env, p, dummy_cube, (uint32_t)sizeof(dummy_cube) - 1u);
    }

    assert(v71r7_photo_menu_host_init(&plat));
    pump_slices(5);

    /* Direct photo_catalog_scan_directory check */
    PhotoPage direct;
    assert(photo_catalog_scan_directory(NULL, 0, NULL, &direct));
    assert(direct.level == PHOTO_ROOT && direct.count == 5u);
    assert(!strcmp(direct.names[0], "01-Fuji-Film"));
    assert(!strcmp(direct.names[1], "02-Kodak-Negative"));
    assert(!strcmp(direct.names[2], "07-Cinema-Print"));
    assert(!strcmp(direct.names[3], "Custom_Cat"));
    assert(!strcmp(direct.names[4], "~")); /* Root LUTs sorts last */

    /* Direct pagination across 45 items (20 + 20 + 5) */
    assert(photo_catalog_scan_directory("02-Kodak-Negative", 0, NULL, &direct));
    assert(direct.count == 20u && direct.previous == 0u && direct.next == 1u);
    assert(!strcmp(direct.names[0], "film_00.cube") && !strcmp(direct.names[19], "film_19.cube"));
    char cur[FX_Z8_LUT_NAME_BYTES];
    memcpy(cur, direct.names[19], sizeof(cur));
    assert(photo_catalog_scan_directory("02-Kodak-Negative", 1, cur, &direct));
    assert(direct.count == 20u && direct.previous == 1u && direct.next == 1u);
    assert(!strcmp(direct.names[0], "film_20.cube") && !strcmp(direct.names[19], "film_39.cube"));
    memcpy(cur, direct.names[19], sizeof(cur));
    assert(photo_catalog_scan_directory("02-Kodak-Negative", 1, cur, &direct));
    assert(direct.count == 5u && direct.previous == 1u && direct.next == 0u);
    assert(!strcmp(direct.names[0], "film_40.cube") && !strcmp(direct.names[4], "film_44.cube"));
    /* Backward pagination */
    memcpy(cur, direct.names[0], sizeof(cur));
    assert(photo_catalog_scan_directory("02-Kodak-Negative", -1, cur, &direct));
    assert(direct.count == 20u && direct.previous == 1u && direct.next == 1u);
    assert(!strcmp(direct.names[0], "film_20.cube") && !strcmp(direct.names[19], "film_39.cube"));

    /* Worker + UI submenu category entry, pagination, and back-to-category position restoration! */
    assert(photo_lut_entry(env.page_buf) == 0);
    pump_slices(20);
    assert(photo_lut_refresh(env.page_buf) == 0);
    PhotoPage snap;
    assert(photo_lut_page(&snap) && snap.level == PHOTO_ROOT && snap.count == 5u);

    /* Focus row 2 ("02-Kodak-Negative", 1-based UI row 2), set scroll/view, activate */
    set_page_row(&env, 2u, 0u);
    env.view_rect[0] = 0; env.view_rect[1] = 12; env.view_rect[2] = 714; env.view_rect[3] = 430;
    PhotoMenuAction act = { .page = env.page_buf, .event = 0x286u, .target = 0u };
    assert(photo_lut_activate(&act) == 0);
    pump_slices(30);
    assert(photo_lut_refresh(env.page_buf) == 0);
    assert(photo_lut_page(&snap) && snap.level == PHOTO_FOLDER && snap.count == 20u && snap.next == 1u);
    assert(!strcmp(snap.category, "02-Kodak-Negative"));
    assert(!strcmp(snap.names[0], "film_00.cube"));

    /* Navigate Next -> Page 2 (served from 512-entry folder cache!) */
    set_page_row(&env, PHOTO_ROW_NEXT, 0u);
    assert(photo_lut_activate(&act) == 0);
    pump_slices(5);
    assert(photo_lut_refresh(env.page_buf) == 0);
    assert(photo_lut_page(&snap) && snap.count == 20u && snap.previous == 1u && snap.next == 1u);
    assert(!strcmp(snap.names[0], "film_20.cube"));

    /* Navigate Next -> Page 3 */
    set_page_row(&env, PHOTO_ROW_NEXT, 0u);
    assert(photo_lut_activate(&act) == 0);
    pump_slices(5);
    assert(photo_lut_refresh(env.page_buf) == 0);
    assert(photo_lut_page(&snap) && snap.count == 5u && snap.previous == 1u && snap.next == 0u);
    assert(!strcmp(snap.names[4], "film_44.cube"));

    /* Press Left (0x285) -> stays on Page 0x5D6 (UI_STAY) and restores root focus == 2! */
    PhotoMenuAction back_act = { .page = env.page_buf, .event = 0x285u, .target = PHOTO_PARENT_PAGE };
    assert(photo_lut_back_action(&back_act) == 0);
    assert(back_act.target == 0x68cu); /* UI_STAY */
    pump_slices(5);
    assert(photo_lut_refresh(env.page_buf) == 0);
    assert(photo_lut_page(&snap) && snap.level == PHOTO_ROOT && snap.focus == 2u);
    assert(env.focused_row == 2u);
    assert(env.view_rect[1] == 12 && env.view_rect[3] == 430);

    /* Press Left again at root -> delegates to stock back action (returns to Photo Shooting Menu) */
    PhotoMenuAction root_back = { .page = env.page_buf, .event = 0x285u, .target = PHOTO_PARENT_PAGE };
    assert(photo_lut_back_action(&root_back) == 0);
    assert(env.stock_back_calls == 1);

    /* Test > 512 files cache overflow fallback (530 files in Custom_Cat) */
    for (int i = 0; i < 530; ++i) {
        char p[PHOTO_PATH_BYTES];
        snprintf(p, sizeof(p), "A:\\FENESTRA\\LUTS\\Custom_Cat\\lot_%03d.cube", i);
        vfs_put_file(&env, p, dummy_cube, (uint32_t)sizeof(dummy_cube) - 1u);
    }
    set_page_row(&env, 4u, 0u); /* Row 4 = Custom_Cat */
    assert(photo_lut_activate(&act) == 0);
    pump_slices(120);
    assert(photo_lut_refresh(env.page_buf) == 0);
    assert(photo_lut_page(&snap) && snap.level == PHOTO_FOLDER && snap.count == 20u && snap.next == 1u);
    assert(photo_menu_ctx()->worker.cache == NULL); /* overflow freed 512 cache cleanly */
    assert(!strcmp(snap.names[0], "lot_000.cube") && !strcmp(snap.names[19], "lot_019.cube"));
    /* Uncached cursor Next still works! */
    set_page_row(&env, PHOTO_ROW_NEXT, 0u);
    assert(photo_lut_activate(&act) == 0);
    pump_slices(120);
    assert(photo_lut_refresh(env.page_buf) == 0);
    assert(photo_lut_page(&snap) && snap.count == 20u && snap.previous == 1u && snap.next == 1u);
    assert(!strcmp(snap.names[0], "lot_020.cube") && !strcmp(snap.names[19], "lot_039.cube"));

    assert(photo_lut_exit(env.page_buf) == 0);
    pump_slices(5);
    assert(env.open_fd_count == 0 && env.open_dir_count == 0);
    v71r7_photo_menu_host_teardown();
    assert(env.alloc_count == env.free_count);
    vfs_destroy(&env);
    printf("[PASS] Suite 2 (Catalog & Pagination): root + categories + 45-file paging + 530-file overflow + back-to-focus\n");
}

/* ============================================================================
 * SUITE 3: Multilingual UI Strings (EN / zh-CN / zh-TW / JA) & Submenu Controller
 * ============================================================================ */
static void test_suite3_multilingual_ui(void) {
    memset(v71r7_host_dram_region, 0, sizeof(v71r7_host_dram_region));
    memset(&g_b1, 0, sizeof(g_b1));
    MockEnv env;
    vfs_reset(&env);
    PhotoMenuPlatOps plat = make_plat(&env);

    /* Works even BEFORE boot_once() (zero-global .rodata!) */
    const uint16_t *ptr = NULL;
    uint32_t bytes = 0u;
    assert(v71r7_photo_menu_lookup_text(PHOTO_LANG_EN, 0x1F42u, &ptr, &bytes) == 1);
    assert(u16_eq(ptr, (const uint16_t *)u"Select LUT") && bytes == 20u);
    assert(v71r7_photo_menu_lookup_text(PHOTO_LANG_ZH_CN, 0x1F42u, &ptr, &bytes) == 1);
    assert(u16_eq(ptr, (const uint16_t *)u"选择 LUT"));
    assert(v71r7_photo_menu_lookup_text(PHOTO_LANG_ZH_TW, 0x1F42u, &ptr, &bytes) == 1);
    assert(u16_eq(ptr, (const uint16_t *)u"選擇 LUT"));
    assert(v71r7_photo_menu_lookup_text(PHOTO_LANG_JA, 0x1F42u, &ptr, &bytes) == 1);
    assert(u16_eq(ptr, (const uint16_t *)u"LUT 選択"));

    /* Off / "无LUT" / "無LUT" / "OFF" */
    assert(photo_text_lookup(PHOTO_LANG_ZH_CN, 0x1F47, &ptr, &bytes) == FX_OK);
    assert(u16_eq(ptr, (const uint16_t *)u"无LUT"));
    assert(photo_text_lookup(PHOTO_LANG_ZH_TW, 0x1F47, &ptr, &bytes) == FX_OK);
    assert(u16_eq(ptr, (const uint16_t *)u"無LUT"));
    assert(photo_text_lookup(PHOTO_LANG_EN, 0x1F47, &ptr, &bytes) == FX_OK);
    assert(u16_eq(ptr, (const uint16_t *)u"Off"));

    /* Out-of-range IDs must be rejected */
    assert(v71r7_photo_menu_lookup_text(PHOTO_LANG_EN, 0x1F3Fu, &ptr, &bytes) == 0);
    assert(v71r7_photo_menu_lookup_text(PHOTO_LANG_EN, 0x1F4Bu, &ptr, &bytes) == 0);
    assert(photo_text_lookup(PHOTO_LANG_EN, 0x1F4B, &ptr, &bytes) == FX_ARGUMENT);

    /* Now init host context and test v71r7_photo_menu_resolve_text across all 11 IDs in EN/SC/TC/JA */
    vfs_add_dir(&env, "A:\\FENESTRA");
    vfs_add_dir(&env, "A:\\FENESTRA\\LUTS");
    vfs_add_dir(&env, "A:\\FENESTRA\\LUTS\\01-Fuji-Film");
    const uint8_t sample[] = "LUT_3D_SIZE 2\n0 0 0\n";
    vfs_put_file(&env, "A:\\FENESTRA\\top.cube", sample, (uint32_t)sizeof(sample) - 1u);
    assert(v71r7_photo_menu_host_init(&plat));
    pump_slices(5);

    const uint32_t langs[] = { PHOTO_LANG_EN, PHOTO_LANG_ZH_CN, PHOTO_LANG_ZH_TW, PHOTO_LANG_JA };
    uint16_t buf[64];
    for (unsigned li = 0; li < 4u; ++li) {
        env.language = langs[li];
        for (uint32_t id = PHOTO_FIRST_TEXT; id < PHOTO_TEXT_END; ++id) {
            int n = v71r7_photo_menu_resolve_text(id, buf, 64u);
            if (id == 0x1F4Au) assert(n == 0 && buf[0] == 0u);
            else assert(n > 0 && buf[n] == 0u);
        }
        assert(v71r7_photo_menu_resolve_text(0x1F3Fu, buf, 64u) == -1);
        assert(v71r7_photo_menu_resolve_text(0x1F4Bu, buf, 64u) == -1);
    }
    /* Truncation & null guard */
    env.language = PHOTO_LANG_EN;
    assert(v71r7_photo_menu_resolve_text(0x1F42u, NULL, 10u) == -1);
    assert(v71r7_photo_menu_resolve_text(0x1F42u, buf, 0u) == -1);
    assert(v71r7_photo_menu_resolve_text(0x1F42u, buf, 5u) == 4 && u16_eq(buf, (const uint16_t *)u"Sele"));

    /* Test zh-CN & zh-TW submenu row bindings (category localization + Root LUTs + font switch) */
    env.language = PHOTO_LANG_ZH_CN;
    assert(photo_lut_entry(env.page_buf) == 0);
    pump_slices(15);
    assert(photo_lut_refresh(env.page_buf) == 0);
    assert(u16_eq(env.row_widgets[0].wide_text, (const uint16_t *)u"无LUT"));
    assert(u16_eq(env.row_widgets[1].wide_text, (const uint16_t *)u"FUJI 色调"));
    assert(u16_eq(env.row_widgets[2].wide_text, (const uint16_t *)u"根目录 LUT"));
    assert(u16_eq(env.status_widget.wide_text, (const uint16_t *)u"无LUT"));
    assert(env.font_records[0x21fu][0] == 2u); /* zh-CN font bound */

    /* Live language switch to zh-TW updates all rows & font immediately */
    env.language = PHOTO_LANG_ZH_TW;
    assert(photo_lut_refresh(env.page_buf) == 0);
    assert(u16_eq(env.row_widgets[0].wide_text, (const uint16_t *)u"無LUT"));
    assert(u16_eq(env.row_widgets[1].wide_text, (const uint16_t *)u"FUJI 色調"));
    assert(u16_eq(env.row_widgets[2].wide_text, (const uint16_t *)u"根目錄 LUT"));
    assert(env.font_records[0x21fu][0] == 0u); /* zh-TW font bound */

    /* Timer callback & lease cleanup on exit */
    assert(env.timer_running == 1 && env.timer_cb == photo_lut_timer);
    photo_lut_timer();
    assert(photo_ui_pending() == 1u);
    assert(photo_lut_exit(env.page_buf) == 0);
    assert(photo_ui_pending() == 0u && env.timer_running == 0);

    pump_slices(5);
    v71r7_photo_menu_host_teardown();
    assert(env.alloc_count == env.free_count);
    vfs_destroy(&env);
    printf("[PASS] Suite 3 (Multilingual UI): 0x1F40..0x1F4A EN/SC/TC/JA + 0x5D6 bindings + lease save/restore\n");
}

/* ============================================================================
 * SUITE 4: Worker Boot Restore, <=2MiB Streaming, Select/Clear, Watchdog, Card Swap
 * ============================================================================ */
static void test_suite4_worker_lifecycle(void) {
    memset(v71r7_host_dram_region, 0, sizeof(v71r7_host_dram_region));
    memset(&g_b1, 0, sizeof(g_b1));
    MockEnv env;
    vfs_reset(&env);
    PhotoMenuPlatOps plat = make_plat(&env);

    /* Build a 150 KiB multi-chunk .cube payload on Drive A */
    const uint32_t cube_sz = 150u * 1024u;
    uint8_t *cube_a = (uint8_t *)malloc(cube_sz);
    assert(cube_a != NULL);
    for (uint32_t i = 0; i < cube_sz; ++i) cube_a[i] = (uint8_t)('A' + (i % 26u));
    uint32_t cube_a_crc = photo_crc32(cube_a, cube_sz);

    vfs_add_dir(&env, "A:\\FENESTRA");
    vfs_add_dir(&env, "A:\\FENESTRA\\LUTS");
    vfs_add_dir(&env, "A:\\FENESTRA\\LUTS\\01-Fuji-Film");
    vfs_put_file(&env, "A:\\FENESTRA\\LUTS\\01-Fuji-Film\\classic.cube", cube_a, cube_sz);
    vfs_put_journal(&env, 'A', 0u, 7u, "LUTS\\01-Fuji-Film\\classic.cube");

    /* Start with cardslot still pending (drive_ready == 0) */
    env.drive_ready = 0;
    env.ticks = 1000u;
    assert(v71r7_photo_menu_host_init(&plat));
    assert(env.task_started == 1 && env.task_prio == 30u && env.task_stack_bytes == 32768u);
    assert(g_b1.last_flight_event == FX_Z8_FLIGHT_BOOT);
    /* Second boot_once must be a strict no-op */
    v71r7_photo_menu_boot_once();
    assert(g_b1.flight_records == 1);

    /* While cardslot is undecided, worker must NOT prepare any LUT, but still polls watchdog */
    pump_slices(3);
    assert(g_b1.lut_prepares == 0 && g_b1.watchdog_polls == 1);
    /* Advance 50ms -> watchdog should NOT fire again yet */
    env.ticks += 50u * FX_Z8_TICKS_PER_MS;
    pump_slices(1);
    assert(g_b1.watchdog_polls == 1);
    /* Advance another 55ms (>= 100ms) -> watchdog fires! */
    env.ticks += 55u * FX_Z8_TICKS_PER_MS;
    pump_slices(1);
    assert(g_b1.watchdog_polls == 2 && g_b1.last_watchdog_tick == env.ticks);

    /* Now cardslot settles on Drive A -> worker restores classic.cube via multi-chunk streaming */
    env.drive_ready = 1;
    g_b1.layer0_step_ret = REAR_STREAMING;
    pump_slices(20);
    assert(g_b1.lut_prepares == 1);
    assert(g_b1.last_prep_bytes == cube_sz && g_b1.last_prep_crc == cube_a_crc);
    assert(!strcmp(g_b1.last_prep_path, "LUTS\\01-Fuji-Film\\classic.cube"));
    assert(v71r7_anchor()->flags & V71R7_ANCHOR_FLAG_LUT_ACTIVE);
    assert(g_b1.last_notify_active == 1);
    assert(photo_lut_status().phase == PHOTO_READY);
    assert(photo_menu_ctx()->worker.cube_buf == NULL && photo_menu_ctx()->worker.cube_stream == NULL);
    assert(env.open_fd_count == 0);
    /* Verify FxZ8LutIoGate card ref count returned cleanly to 0 */
    assert((v71r7_io_gate()->state & FX_Z8_LUT_IO_CARD) == 0u);
    /* Next slice pumps v71r7_layer0_worker_step() and returns 1 (REAR_STREAMING) */
    int steps_before = g_b1.layer0_steps;
    assert(photo_worker_slice() == 1);
    assert(g_b1.layer0_steps == steps_before + 1);

    /* Test exact 2 MiB .cube SUCCEEDS under strict 32 KiB single-alloc cap (0 OOM on real Z8 General heap!)
     * and 2 MiB + 1 B is REJECTED with FX_BOUNDS */
    uint8_t *cube_2m = (uint8_t *)malloc(PHOTO_MAX_CUBE_BYTES + 1u);
    assert(cube_2m != NULL);
    memset(cube_2m, 'Z', PHOTO_MAX_CUBE_BYTES + 1u);
    uint32_t cube_2m_crc = photo_crc32(cube_2m, PHOTO_MAX_CUBE_BYTES);
    vfs_put_file(&env, "A:\\FENESTRA\\exact2m.cube", cube_2m, PHOTO_MAX_CUBE_BYTES);
    vfs_put_file(&env, "A:\\FENESTRA\\over2m.cube", cube_2m, PHOTO_MAX_CUBE_BYTES + 1u);

    env.max_read_chunk = 65536u;
    env.max_alloc_limit = 32768u;
    env.max_alloc_seen = 0u;
    assert(photo_lut_select("exact2m.cube", photo_gui_epoch()) != 0u);
    pump_slices(150);
    assert(photo_lut_status().phase == PHOTO_READY);
    assert(g_b1.last_prep_bytes == PHOTO_MAX_CUBE_BYTES && g_b1.last_prep_crc == cube_2m_crc);
    assert(env.max_alloc_seen == PHOTO_CUBE_CHUNK_BYTES);
    assert(env.open_fd_count == 0 && (v71r7_io_gate()->state & FX_Z8_LUT_IO_CARD) == 0u);
    assert(photo_menu_ctx()->worker.cube_buf == NULL && photo_menu_ctx()->worker.cube_stream == NULL);

    /* Verify journal saved generation 8 -> "exact2m.cube" on Drive A */
    PhotoFileIO check_io = {
        &env,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL
    };
    (void)check_io;
    int s1 = vfs_find(&env, "A:\\FENESTRA\\FXPHOTO1.CFG");
    assert(s1 >= 0 && env.nodes[s1].len == PHOTO_RECORD_BYTES);
    PhotoChoice saved;
    assert(photo_choice_read(env.nodes[s1].data, env.nodes[s1].len, &saved) == FX_OK);
    assert(saved.generation == 8u && !strcmp(saved.path, "exact2m.cube"));

    /* Now select oversized 2 MiB + 1 B -> must fail with FX_BOUNDS & clean up! */
    assert(photo_lut_select("over2m.cube", photo_gui_epoch()) != 0u);
    pump_slices(150);
    PhotoStatus st = photo_lut_status();
    assert(st.phase == PHOTO_LOAD_ERROR && st.reason == FX_BOUNDS);
    assert(env.open_fd_count == 0 && photo_menu_ctx()->worker.cube_buf == NULL &&
           photo_menu_ctx()->worker.cube_stream == NULL);
    assert(!(v71r7_anchor()->flags & V71R7_ANCHOR_FLAG_LUT_ACTIVE));
    env.max_alloc_limit = 0u;

    /* User clears LUT (selects "" / "无LUT") -> persists tombstone & goes PHOTO_NO_LUT */
    assert(photo_lut_select("", photo_gui_epoch()) != 0u);
    pump_slices(10);
    assert(photo_lut_status().phase == PHOTO_NO_LUT);
    assert(g_b1.last_notify_active == 0 && g_b1.lut_active == 0);
    int s0 = vfs_find(&env, "A:\\FENESTRA\\FXPHOTO0.CFG");
    assert(s0 >= 0 && photo_choice_read(env.nodes[s0].data, env.nodes[s0].len, &saved) == FX_OK);
    assert(saved.generation == 9u && saved.path[0] == '\0');

    /* V71.7 Suite #20 CPU2 idle sleep check:
     * When dormant (LUT Off, WYSIWYG Off, workspace unallocated, no job/scan/page_request,
     * even with drain_needed == 1), photo_worker MUST sleep 20 ms (NOT 2 ms)! */
    assert(photo_worker_idle_delay_for_test() == 20u);

    /* FilmSim-only WYSIWYG flag pumps Layer 0 & switches idle sleep to 2 ms while active! */
    v71r7_anchor()->flags |= V71R7_ANCHOR_FLAG_WYSIWYG_ACTIVE;
    assert(photo_worker_idle_delay_for_test() == 2u);
    steps_before = g_b1.layer0_steps;
    assert(photo_worker_slice() == 1);
    assert(g_b1.layer0_steps == steps_before + 1);
    v71r7_anchor()->flags &= ~V71R7_ANCHOR_FLAG_WYSIWYG_ACTIVE;
    assert(photo_worker_idle_delay_for_test() == 20u);

    /* Allocated Layer 0 workspace or active LUT or pending page request also uses 2 ms */
    g_b1.layer0_ws_allocated = 1;
    assert(photo_worker_idle_delay_for_test() == 2u);
    g_b1.layer0_ws_allocated = 0;
    assert(photo_worker_idle_delay_for_test() == 20u);

    v71r7_anchor()->flags |= V71R7_ANCHOR_FLAG_LUT_ACTIVE;
    assert(photo_worker_idle_delay_for_test() == 2u);
    v71r7_anchor()->flags &= ~V71R7_ANCHOR_FLAG_LUT_ACTIVE;
    assert(photo_worker_idle_delay_for_test() == 20u);

    photo_lut_request_page(0);
    assert(photo_worker_idle_delay_for_test() == 2u);
    pump_slices(10);
    assert(photo_worker_idle_delay_for_test() == 20u);

    /* Card-change reload (Drive A -> Drive B with "cinema.cube" in B:\FENESTRA\FXPHOTO0.CFG) */
    const uint8_t cube_b[] = "LUT_3D_SIZE 2\n0.1 0.2 0.3\n";
    vfs_add_dir(&env, "B:\\FENESTRA");
    vfs_put_file(&env, "B:\\FENESTRA\\cinema.cube", cube_b, (uint32_t)sizeof(cube_b) - 1u);
    vfs_put_journal(&env, 'B', 0u, 42u, "cinema.cube");

    uint64_t epoch_before = photo_gui_epoch();
    env.drive = 'B';
    v71r7_photo_menu_on_card_change();
    assert(photo_gui_epoch() != epoch_before);
    pump_slices(15);
    assert(photo_lut_status().phase == PHOTO_READY);
    assert(!strcmp(g_b1.last_prep_path, "cinema.cube"));
    assert(g_b1.last_prep_bytes == (uint32_t)sizeof(cube_b) - 1u);
    assert(v71r7_anchor()->flags & V71R7_ANCHOR_FLAG_LUT_ACTIVE);

    /* Power-off Early OFF closes gate, drains cleanly; cancel reopens & restores cleanly */
    fx_z8_lut_io_close(v71r7_io_gate(), 0);
    pump_slices(5);
    assert(!(v71r7_anchor()->flags & V71R7_ANCHOR_FLAG_LUT_ACTIVE));
    assert((v71r7_io_gate()->state & FX_Z8_LUT_IO_CARD) == 0u);
    fx_z8_lut_io_reopen(v71r7_io_gate(), 1);
    pump_slices(15);
    assert(photo_lut_status().phase == PHOTO_READY);
    assert(!strcmp(g_b1.last_prep_path, "cinema.cube"));

    photo_worker_shutdown();
    pump_slices(5);
    assert(photo_worker_pending() == 0u);
    assert(env.open_fd_count == 0 && env.open_dir_count == 0);

    free(cube_a);
    free(cube_2m);
    v71r7_photo_menu_host_teardown();
    assert(env.alloc_count == env.free_count);
    vfs_destroy(&env);
    printf("[PASS] Suite 4 (Worker Lifecycle): boot restore + 2MiB stream + clear + watchdog + card swap + power sync\n");
}

/* ============================================================================
 * SUITE 5: Generated Bank 2 Static RO Table Integrity Verification
 * ============================================================================ */
static void test_suite5_ro_tables(void) {
    /* Parent descriptor: LUT-only count=48 */
    assert(v71r7_photo_parent_desc.flags == 0x80001003u);
    assert(v71r7_photo_parent_desc.count == 48u);
    assert(v71r7_photo_parent_desc.rows == &v71r7_photo_parent_rows[0]);
    assert(v71r7_photo_parent_desc.events == &v71r7_photo_parent_events[0]);

    /* Row 45 preserves NFP v0.5.1 Pixel Shift (0x2C13) */
    assert(v71r7_photo_parent_rows[45].words[6] == 0x2C13u);
    assert((uintptr_t)v71r7_photo_parent_events[45] == (uintptr_t)UINT64_C(0x43E27B40));

    /* Row 47 is Select LUT (0x2DE7, text 0x1F42 / 0x1F43 -> Page 0x5D6) */
    assert(v71r7_photo_parent_rows[47].words[0] == 0x1F42u);
    assert(v71r7_photo_parent_rows[47].words[1] == 0x1F43u);
    assert(v71r7_photo_parent_rows[47].words[2] == 0x1F42u);
    assert(v71r7_photo_parent_rows[47].words[6] == 0x2DE7u);
    assert(v71r7_photo_parent_events[47] == &v71r7_photo_parent_lut_event_prog[0]);

    /* Parent LUT activation targets Page 0x5D6 */
    const unsigned act_idx[4] = { 5u, 6u, 9u, 11u };
    const uint64_t act_ev[4]  = { 0x286u, 0x28bu, 0x29fu, 0x2aeu };
    for (unsigned i = 0; i < 4u; ++i) {
        const V71r7MenuEventEntry *e = &v71r7_photo_parent_lut_event_prog[act_idx[i]];
        assert(e->event == act_ev[i] && e->target == PHOTO_LUT_PAGE);
    }
    assert(v71r7_photo_parent_lut_event_prog[92].event == 0x3BBu);

    /* Child Page 0x5D6 descriptor, vector, layout, 23 rows, 15 events */
    assert(v71r7_lut_child_desc.flags == 0x80001818u);
    assert(v71r7_lut_child_desc.count == 23u);
    assert(v71r7_lut_child_desc.viewport[0] == 6 && v71r7_lut_child_desc.viewport[1] == 60 &&
           v71r7_lut_child_desc.viewport[2] == 714 && v71r7_lut_child_desc.viewport[3] == 418);
    assert(v71r7_lut_child_vector[0] == photo_lut_entry);
    assert(v71r7_lut_child_vector[1] == photo_lut_exit);
    assert((uintptr_t)v71r7_lut_child_vector[2] == (uintptr_t)UINT64_C(0x403b2210));
    assert(v71r7_lut_child_vector[3] == NULL);
    assert(v71r7_lut_child_vector[4] == photo_lut_refresh);

    for (uint32_t i = 0; i < PHOTO_LUT_ROWS; ++i) {
        uint32_t expect_txt = (i == 0u) ? 0x1F47u :
                              (i == 21u) ? 0x1F48u :
                              (i == 22u) ? 0x1F49u : 0x1F4Au;
        assert(v71r7_lut_child_rows[i].words[0] == expect_txt);
        assert(v71r7_lut_child_rows[i].words[6] == PHOTO_LUT_ROW + i);
        assert(v71r7_lut_child_events[i] == &v71r7_lut_child_event_table[0]);
    }
    int act_hooks = 0, back_hooks = 0;
    for (unsigned i = 0; i < 15u; ++i) {
        const V71r7MenuEventEntry *e = &v71r7_lut_child_event_table[i];
        if (e->callback == photo_lut_activate) {
            assert(e->target == 0x68cu);
            ++act_hooks;
        } else if (e->callback == photo_lut_back_action) {
            assert(e->target == PHOTO_PARENT_PAGE);
            ++back_hooks;
        }
    }
    assert(act_hooks == 4 && back_hooks == 3);
    printf("[PASS] Suite 5 (Static RO Tables): count=48 LUT-only, Row 45 NFP + Row 47 0x2DE7 -> 0x5D6 (23 rows)\n");
}

/* ============================================================================
 * SUITE 6: V71.7-r4 Flight Pre-CAS Fix, Cache-Swap Ordering, Dormant 0 B,
 *          LUT IPC (ops 65..68) & Row 47 / Page 0x5D6 Disabled/Off Gating
 * ============================================================================ */
static void test_suite6_r4_ipc_dormant_flight_row47(void) {
    memset(v71r7_host_dram_region, 0, sizeof(v71r7_host_dram_region));
    memset(&g_b1, 0, sizeof(g_b1));
    g_b1.neon_allowed = 1;
    g_b1.smp_online_helpers = 2u;
    atomic_store(&g_b1.mock_l0_metrics.surface_bytes, 0xE8C000u);
    atomic_store(&g_b1.mock_l0_metrics.fps_milli, 29970u);
    atomic_store(&g_b1.mock_l0_metrics.grades, 123u);
    atomic_store(&g_b1.mock_l0_metrics.disp_substituted, 120u);
    atomic_store(&g_b1.mock_l0_metrics.disp_rejects, 0u);
    atomic_store(&g_b1.mock_l0_metrics.live_reuse_hits, 45u);
    atomic_store(&g_b1.mock_l0_metrics.last_lut_us, 4100u);
    atomic_store(&g_b1.mock_l0_metrics.last_hdf_us, 5100u);
    atomic_store(&g_b1.mock_l0_metrics.last_grain_us, 420u);
    g_b1.mock_rt_stats.nv16_xm6_calls = 7u;
    g_b1.mock_rt_stats.nv16_neon_calls = 3u;
    g_b1.mock_rt_stats.nv12_neon_calls = 99u;

    MockEnv env;
    vfs_reset(&env);
    vfs_add_dir(&env, "A:\\FENESTRA");
    uint8_t cube_data[] = "LUT_3D_SIZE 2\n0 0 0\n";
    vfs_put_file(&env, "A:\\FENESTRA\\LOOK.CUBE", cube_data, (uint32_t)sizeof(cube_data) - 1u);
    /* Write FXPHOTO0.CFG pointing to LOOK.CUBE */
    vfs_put_journal(&env, 'A', 0u, 1u, "LOOK.CUBE");

    /* 6a. Start with lut.wlm DISABLED -> boot MUST NOT wipe flight on repeated boot_once,
     *     AND worker MUST do 0 card I/O to FXPHOTO0.CFG while keeping WYSIWYG decoupled! */
    v71r7_host_set_lut_module_loaded(0);
    PhotoMenuPlatOps plat = make_plat(&env);
    assert(v71r7_photo_menu_host_init(&plat) == 1);
    assert(g_b1.flight_inits == 1);
    v71r7_photo_menu_boot_once(); /* Repeated ev==19 -> MUST NOT re-zero flight ring! */
    v71r7_photo_menu_boot_once();
    assert(g_b1.flight_inits == 1);

    uint32_t opens_before = env.open_calls;
    pump_slices(6);
    assert(env.open_calls == opens_before); /* 0 card I/O when lut.wlm disabled! */
    assert(g_b1.lut_prepares == 0);
    assert(photo_worker_idle_delay_for_test() == 20u);
    assert(g_b1.layer0_steps == 0);

    /* FilmSim WYSIWYG_ACTIVE == 1 stays decoupled and still pumps Layer 0 when lut.wlm disabled! */
    __atomic_fetch_or(&v71r7_anchor()->flags, V71R7_ANCHOR_FLAG_WYSIWYG_ACTIVE, __ATOMIC_ACQ_REL);
    assert(photo_worker_idle_delay_for_test() == 2u);
    pump_slices(3);
    assert(g_b1.layer0_steps >= 3);
    __atomic_fetch_and(&v71r7_anchor()->flags, ~V71R7_ANCHOR_FLAG_WYSIWYG_ACTIVE, __ATOMIC_ACQ_REL);
    assert(photo_worker_idle_delay_for_test() == 20u);

    /* 6b. Row 47 & Page 0x5D6 disabled UI in EN/zh-CN/zh-TW/JA: 0 card I/O & activate no-op! */
    const uint16_t *txt = NULL;
    uint32_t tbytes = 0u;
    assert(v71r7_photo_menu_lookup_text(PHOTO_LANG_EN, 0x1F42u, &txt, &tbytes) == 1);
    assert(tbytes > 10u * 2u); /* "Select LUT (Off)" > "Select LUT" */
    assert(v71r7_photo_menu_lookup_text(PHOTO_LANG_ZH_CN, 0x1F43u, &txt, &tbytes) == 1);
    assert(v71r7_photo_menu_lookup_text(PHOTO_LANG_ZH_TW, 0x1F43u, &txt, &tbytes) == 1);
    assert(v71r7_photo_menu_lookup_text(PHOTO_LANG_JA, 0x1F43u, &txt, &tbytes) == 1);

    uint8_t page_buf[0x80];
    memset(page_buf, 0, sizeof(page_buf));
    uintptr_t desc_p = (uintptr_t)&v71r7_lut_child_desc;
    memcpy(page_buf + 0x50u, &desc_p, sizeof(desc_p));
    env.cur_page = page_buf;
    env.language = PHOTO_LANG_ZH_CN;
    uint32_t dir_before = env.dir_open_calls;
    assert(photo_lut_entry(page_buf) == 0);
    assert(env.dir_open_calls == dir_before); /* 0 card I/O on page entry when disabled! */
    PhotoMenuAction act = { page_buf, 0x286u, 0u };
    assert(photo_lut_activate(&act) == 0); /* Strict no-op! */
    assert(photo_lut_exit(page_buf) == 0);

    /* 6c. Enable lut.wlm -> worker auto-restores FXPHOTO0.CFG and invalidates cache BEFORE prepare! */
    v71r7_host_set_lut_module_loaded(1);
    pump_slices(12);
    assert(g_b1.lut_prepares == 1);
    assert(g_b1.cache_inval_before_last_prep == 1);
    assert(g_b1.lut_active == 1);

    /* 6d. LUT IPC ops 65..68 (GET_STATE, SET_CFG, CLEAR_LUT, RESET_PREVIEW) */
    V71r7LutIpcState st;
    uint32_t rbytes = (uint32_t)sizeof(st);
    assert(v71r7_lut_ipc_service_call(V71R7_LUT_OP_GET_STATE, NULL, 0u, &st, &rbytes) == 0);
    assert(rbytes == sizeof(V71r7LutIpcState));
    assert(st.magic == V71R7_LUT_IPC_MAGIC && st.version == 1u && st.bytes == 168u);
    assert(st.master_on == 1u && st.preview_on == 1u && st.still_on == 1u && st.live_on == 1u);
    assert(st.active_drive == 'A' && st.lut_active == 1u && st.smp_helpers_online == 2u);
    assert(st.ws_bytes == 0xE8C000u && st.fps_milli == 29970u && st.live_reuse_hits == 45u);
    assert(st.xm6_calls == 7u && st.nv16_neon == 3u && st.nv12_neon == 99u);
    assert(strcmp(st.active_path, "LOOK.CUBE") == 0);

    /* SET_CFG: master_on 1 -> 0 releases LUT without erasing FXPHOTO0.CFG! */
    V71r7LutIpcSetCfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = V71R7_LUT_IPC_MAGIC;
    cfg.version = V71R7_LUT_IPC_VERSION;
    cfg.bytes = (uint16_t)sizeof(cfg);
    cfg.master_on = 0u;
    cfg.stage_mask = V71R7_LUT_STAGE_ALL;
    rbytes = 0u;
    assert(v71r7_lut_ipc_service_call(V71R7_LUT_OP_SET_CFG, &cfg, sizeof(cfg), NULL, &rbytes) == 0);
    assert(g_b1.lut_active == 0);
    pump_slices(4);
    assert(v71r7_photo_menu_lookup_text(PHOTO_LANG_ZH_CN, 0x1F42u, &txt, &tbytes) == 1);

    /* SET_CFG: master_on 0 -> 1 bumps lut_enable_epoch -> auto-restores LOOK.CUBE from FXPHOTO0.CFG! */
    cfg.master_on = 1u;
    assert(v71r7_lut_ipc_service_call(V71R7_LUT_OP_SET_CFG, &cfg, sizeof(cfg), NULL, &rbytes) == 0);
    pump_slices(12);
    assert(g_b1.lut_prepares == 2 && g_b1.lut_active == 1);

    /* RESET_PREVIEW: healthy -> calls pool_suspend + pool_resume; quarantined -> returns -5 (FX_BUSY)! */
    assert(v71r7_lut_ipc_service_call(V71R7_LUT_OP_RESET_PREVIEW, NULL, 0u, NULL, &rbytes) == 0);
    assert(g_b1.pool_suspend_calls == 1 && g_b1.pool_resume_calls == 1);
    atomic_store(&g_b1.mock_l0_metrics.quarantined, 1u);
    assert(v71r7_lut_ipc_service_call(V71R7_LUT_OP_RESET_PREVIEW, NULL, 0u, NULL, &rbytes) == -5);
    assert(g_b1.pool_suspend_calls == 1);
    atomic_store(&g_b1.mock_l0_metrics.quarantined, 0u);

    /* CLEAR_LUT: posts empty selection and clears active LUT */
    assert(v71r7_lut_ipc_service_call(V71R7_LUT_OP_CLEAR_LUT, NULL, 0u, NULL, &rbytes) == 0);
    pump_slices(8);
    assert(g_b1.lut_active == 0);

    /* Unknown op -> -3 (FX_CAPABILITY), malformed SET_CFG -> -2 (FX_ARGUMENT) */
    assert(v71r7_lut_ipc_service_call(79u, NULL, 0u, NULL, &rbytes) == -3);
    cfg.magic = 0xDEADBEEFu;
    assert(v71r7_lut_ipc_service_call(V71R7_LUT_OP_SET_CFG, &cfg, sizeof(cfg), NULL, &rbytes) == -2);

    photo_worker_shutdown();
    pump_slices(3);
    v71r7_photo_menu_host_teardown();
    assert(env.alloc_count == env.free_count);
    vfs_destroy(&env);
    printf("[PASS] Suite 6 (V71.7-r4): flight post-CAS, cache-inval order, 0 B dormant, LUT IPC 65..68 & Row 47 disabled UI\n");
}

int main(void) {
    test_suite1_journal();
    test_suite2_catalog_and_pagination();
    test_suite3_multilingual_ui();
    test_suite4_worker_lifecycle();
    test_suite5_ro_tables();
    test_suite6_r4_ipc_dormant_flight_row47();
    printf("ALL 6 V71.7-r4 PHOTO MENU HOST SUITES PASSED (ASan+UBSan clean, 0 leaks)!\n");
    return 0;
}
