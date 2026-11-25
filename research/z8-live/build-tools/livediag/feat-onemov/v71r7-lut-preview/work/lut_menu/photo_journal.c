/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Dual-Slot CRC32 Atomic LUT Config Journal (Zero Globals, Strict Scalar).
 *
 * Persists selected LUT path atomically across:
 *   <drive>:\FENESTRA\FXPHOTO0.CFG
 *   <drive>:\FENESTRA\FXPHOTO1.CFG
 *   <drive>:\FENESTRA\FXPHOTO.TMP
 * Supports reading legacy V1 (64 B) and V2 (96 B) records, writing V2 records
 * with monotonic 64-bit generation counter, CRC32 integrity check, and
 * same-letter card-swap protection. Zero cold-retry3 symbols.
 */
#include "photo_menu.h"

static const char k_journal_slots[3][13] = {
    "FXPHOTO0.CFG",
    "FXPHOTO1.CFG",
    "FXPHOTO.TMP"
};

void photo_card_path(char drive, const char *relative, char *out) {
    memcpy(out, "A:\\FENESTRA\\", 12u);
    out[0] = drive;
    size_t i = 0;
    do {
        out[12u + i] = relative[i];
    } while (relative[i++]);
}

uint32_t photo_crc32(const uint8_t *data, size_t bytes) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < bytes; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8u; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & UINT32_C(0xedb88320));
    }
    return ~crc;
}

static uint64_t read_le(const uint8_t *data, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i)
        value |= (uint64_t)data[i] << (i * 8u);
    return value;
}

static void write_le(uint8_t *data, size_t bytes, uint64_t value) {
    for (size_t i = 0; i < bytes; ++i)
        data[i] = (uint8_t)(value >> (i * 8u));
}

FxResult photo_choice_read(const uint8_t *data, size_t bytes, PhotoChoice *out) {
    if (!data || !out) return FX_ARGUMENT;
    int legacy = (bytes == PHOTO_RECORD_V1_BYTES);
    if (!legacy && bytes != PHOTO_RECORD_BYTES) return FX_INTEGRITY;
    size_t capacity = legacy ? FX_Z8_LUT_NAME_BYTES : FX_Z8_LUT_PATH_BYTES;
    if (memcmp(data, "FXPHOTO\0", 8u) ||
        read_le(data + 8u, 2u) != (legacy ? 1u : 2u) ||
        read_le(data + 10u, 2u) != bytes ||
        !read_le(data + 12u, 8u) ||
        data[20] > 1u ||
        data[21] >= capacity ||
        read_le(data + 22u, 2u) ||
        read_le(data + bytes - 8u, 4u) ||
        read_le(data + bytes - 4u, 4u) != photo_crc32(data, bytes - 4u))
        return FX_INTEGRITY;

    const char *path = (const char *)(data + 24u);
    size_t path_bytes = data[21];
    if (data[20] != (uint8_t)(path_bytes != 0u)) return FX_INTEGRITY;
    for (size_t i = 0; i < capacity; ++i) {
        if ((i < path_bytes) != (path[i] != 0)) return FX_INTEGRITY;
    }
    if (path_bytes && !(legacy ? fx_z8_lut_name(path) : fx_z8_lut_path(path)))
        return FX_INTEGRITY;

    out->generation = read_le(data + 12u, 8u);
    memset(out->path, 0, sizeof(out->path));
    if (path_bytes) memcpy(out->path, path, path_bytes);
    return FX_OK;
}

FxResult photo_choice_write(const PhotoChoice *choice, uint8_t data[PHOTO_RECORD_BYTES]) {
    if (!choice || !data || !choice->generation ||
        (choice->path[0] && !fx_z8_lut_path(choice->path)))
        return FX_ARGUMENT;
    size_t path_bytes = 0;
    while (path_bytes < FX_Z8_LUT_PATH_BYTES && choice->path[path_bytes])
        ++path_bytes;
    if (path_bytes >= FX_Z8_LUT_PATH_BYTES) return FX_ARGUMENT;

    memset(data, 0, PHOTO_RECORD_BYTES);
    memcpy(data, "FXPHOTO\0", 8u);
    data[8] = 2u;
    data[10] = PHOTO_RECORD_BYTES;
    write_le(data + 12u, 8u, choice->generation);
    data[20] = (uint8_t)(path_bytes != 0u);
    data[21] = (uint8_t)path_bytes;
    if (path_bytes) memcpy(data + 24u, choice->path, path_bytes);
    write_le(data + PHOTO_RECORD_BYTES - 4u, 4u,
             photo_crc32(data, PHOTO_RECORD_BYTES - 4u));
    return FX_OK;
}

typedef struct {
    uint8_t  data[PHOTO_RECORD_BYTES + 1u];
    uint32_t bytes;
    int      present;
    int      complete;
} SlotImage;

/* Keep short reads, including the EOF probe, on the same 64B-aligned bounce buffer.
 * An oversized file is invalid and cannot be matched for safe removal. */
static FxResult read_image(const PhotoFileIO *io, const char *name, SlotImage *out) {
    memset(out, 0, sizeof(*out));
    if (!io->current(io->context)) return FX_STALE;
    int32_t fd = -1;
    FxResult rc = io->open(io->context, name, 0u, &fd);
    if (rc == FX_EMPTY) return io->current(io->context) ? FX_OK : FX_STALE;
    if (rc != FX_OK) return rc;
    out->present = 1;
    _Alignas(64) uint8_t bounce[128];
    while (1) {
        if (!io->current(io->context)) {
            rc = FX_STALE;
            break;
        }
        uint32_t amount = 0;
        uint32_t remaining = (uint32_t)sizeof(out->data) - out->bytes;
        uint32_t request = remaining ? remaining : 1u;
        rc = io->read(io->context, fd, bounce, request, &amount);
        if (rc != FX_OK) break;
        if (amount > request) {
            rc = FX_INTEGRITY;
            break;
        }
        if (!amount) {
            out->complete = 1;
            break;
        }
        if (!remaining) break;
        memcpy(out->data + out->bytes, bounce, amount);
        out->bytes += amount;
    }
    if (io->close(io->context, fd) != FX_OK) return FX_CLOSE;
    if (rc != FX_OK) return rc;
    return io->current(io->context) ? FX_OK : FX_STALE;
}

static int same_choice(const PhotoChoice *a, const PhotoChoice *b) {
    return a->generation == b->generation && !memcmp(a->path, b->path, sizeof(a->path));
}

static FxResult inspect_slots(const PhotoFileIO *io, char drive, PhotoJournal *out,
                              SlotImage images[2], int saving) {
    if (!io || !out || (drive != 'A' && drive != 'B')) return FX_ARGUMENT;
    memset(out, 0, sizeof(*out));
    PhotoChoice choices[2];
    int valid[2];
    int corrupt = 0;
    for (unsigned i = 0; i < 2u; ++i) {
        char file[PHOTO_PATH_BYTES];
        photo_card_path(drive, k_journal_slots[i], file);
        SlotImage *image = &images[i];
        FxResult rc = read_image(io, file, image);
        if (rc != FX_OK) return rc;
        valid[i] = image->present && image->complete &&
            photo_choice_read(image->data, image->bytes, &choices[i]) == FX_OK;
        corrupt |= (image->present && !valid[i]);
    }
    if (!valid[0] && !valid[1]) return (corrupt && !saving) ? FX_INTEGRITY : FX_EMPTY;
    unsigned newest = (!valid[0] || (valid[1] && choices[1].generation > choices[0].generation)) ? 1u : 0u;
    if (valid[0] && valid[1] && choices[0].generation == choices[1].generation &&
        !same_choice(&choices[0], &choices[1]))
        return FX_INTEGRITY;
    out->choice = choices[newest];
    out->slot = newest;
    return FX_OK;
}

FxResult photo_journal_load(const PhotoFileIO *io, char drive, PhotoJournal *out) {
    SlotImage images[2];
    return inspect_slots(io, drive, out, images, 0);
}

static FxResult file_exists_raw(const PhotoFileIO *io, const char *name) {
    if (!io->current(io->context)) return FX_STALE;
    int32_t fd = -1;
    FxResult rc = io->open(io->context, name, 0u, &fd);
    if (rc != FX_OK) return (rc == FX_EMPTY && !io->current(io->context)) ? FX_STALE : rc;
    if (io->close(io->context, fd) != FX_OK) return FX_CLOSE;
    return io->current(io->context) ? FX_OK : FX_STALE;
}

FxResult photo_file_exists(const PhotoFileIO *io, char drive, const char *path) {
    if (!io || (drive != 'A' && drive != 'B') || !fx_z8_lut_path(path)) return FX_ARGUMENT;
    char file[PHOTO_PATH_BYTES];
    photo_card_path(drive, path, file);
    return file_exists_raw(io, file);
}

static FxResult ensure_absent(const PhotoFileIO *io, const char *name) {
    FxResult rc = file_exists_raw(io, name);
    if (rc == FX_EMPTY) return FX_OK;
    if (rc != FX_OK) return rc;
    if (!io->current(io->context)) return FX_STALE;
    rc = io->remove(io->context, name);
    if (rc != FX_OK) return rc;
    rc = file_exists_raw(io, name);
    return rc == FX_EMPTY ? FX_OK : (rc == FX_OK ? FX_INTEGRITY : rc);
}

static FxResult verify_image(const PhotoFileIO *io, const char *name, const SlotImage *expected) {
    SlotImage actual;
    FxResult rc = read_image(io, name, &actual);
    if (rc != FX_OK) return rc;
    if (actual.present != expected->present ||
        (actual.present &&
         (!actual.complete || !expected->complete ||
          actual.bytes != expected->bytes ||
          memcmp(actual.data, expected->data, actual.bytes))))
        return FX_STALE;
    return FX_OK;
}

FxResult photo_journal_save(const PhotoFileIO *io, char drive, const char *path) {
    if (!io || !path || (path[0] && !fx_z8_lut_path(path))) return FX_ARGUMENT;
    PhotoJournal active;
    SlotImage before[2];
    FxResult initial = inspect_slots(io, drive, &active, before, 1);
    if (initial != FX_OK && initial != FX_EMPTY) return initial;
    if (active.choice.generation == UINT64_MAX) return FX_BOUNDS;

    PhotoChoice next;
    memset(&next, 0, sizeof(next));
    next.generation = active.choice.generation + 1u;
    for (size_t i = 0; i + 1u < sizeof(next.path) && path[i]; ++i)
        next.path[i] = path[i];

    SlotImage written;
    memset(&written, 0, sizeof(written));
    written.bytes = PHOTO_RECORD_BYTES;
    written.present = 1;
    written.complete = 1;
    FxResult rc = photo_choice_write(&next, written.data);
    if (rc != FX_OK) return rc;

    char temp[PHOTO_PATH_BYTES], dest[PHOTO_PATH_BYTES], peer[PHOTO_PATH_BYTES];
    unsigned target = (initial == FX_EMPTY) ? 0u : (active.slot ^ 1u);
    photo_card_path(drive, k_journal_slots[2], temp);
    photo_card_path(drive, k_journal_slots[target], dest);
    photo_card_path(drive, k_journal_slots[target ^ 1u], peer);

    rc = ensure_absent(io, temp);
    if (rc != FX_OK) return rc;
    if (!io->current(io->context)) return FX_STALE;

    int32_t fd = -1;
    rc = io->open(io->context, temp, 0x101u, &fd);
    if (rc != FX_OK) return rc;
    uint32_t amount = 0;
    rc = io->current(io->context)
        ? io->write(io->context, fd, written.data, written.bytes, &amount)
        : FX_STALE;
    if (io->close(io->context, fd) != FX_OK) return FX_CLOSE;
    if (rc != FX_OK) return rc;
    if (amount != written.bytes) return FX_IO;

    rc = verify_image(io, temp, &written);
    if (rc != FX_OK) return rc;
    /* Drive letters are reused on remount. Compare both slots byte for byte
     * and re-verify temp before replacing the inactive slot. */
    rc = verify_image(io, peer, &before[target ^ 1u]);
    if (rc != FX_OK) return rc;
    rc = verify_image(io, dest, &before[target]);
    if (rc != FX_OK) return rc;
    rc = verify_image(io, temp, &written);
    if (rc != FX_OK) return rc;
    if (!io->current(io->context)) return FX_STALE;

    if (before[target].present) {
        rc = io->remove(io->context, dest);
        if (rc != FX_OK) return rc;
        rc = verify_image(io, temp, &written);
        if (rc != FX_OK) return rc;
        SlotImage absent;
        memset(&absent, 0, sizeof(absent));
        rc = verify_image(io, dest, &absent);
        if (rc != FX_OK) return rc;
    }
    if (!io->current(io->context)) return FX_STALE;
    rc = io->rename(io->context, temp, dest);
    return rc == FX_OK ? verify_image(io, dest, &written) : rc;
}
