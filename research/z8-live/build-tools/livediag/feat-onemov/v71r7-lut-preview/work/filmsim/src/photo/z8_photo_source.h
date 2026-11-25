#ifndef Z8_PHOTO_SOURCE_H
#define Z8_PHOTO_SOURCE_H

#include <stddef.h>
#include <stdint.h>

/* Observed in V62 MAIN 36c564c9..., not an SDK or a writable buffer lease. */
#define Z8_PHOTO_DESCRIPTOR_BYTES 0x28u
#define Z8_PHOTO_SOURCE_COUNT 6u

struct z8_photo_descriptor {
    uint32_t format, layout;
    uint16_t y_stride, uv_stride;
    uint16_t storage_width, storage_height;
    uint16_t width, height;
    uint16_t origin_x, origin_y;
    uint64_t y_address, uv_address;
};

enum z8_photo_layout_status {
    Z8_PHOTO_LAYOUT_OK = 0,
    Z8_PHOTO_LAYOUT_BAD_ARGUMENT,
    Z8_PHOTO_LAYOUT_UNSUPPORTED,
    Z8_PHOTO_LAYOUT_BAD_GEOMETRY,
    Z8_PHOTO_LAYOUT_BAD_ADDRESS,
    Z8_PHOTO_LAYOUT_OVERLAP
};

/* Decodes unaligned bytes without dereferencing any firmware pointer. */
int z8_photo_decode(const void *bytes, size_t length,
                    struct z8_photo_descriptor *out);

/* Strict candidate 8-bit semiplanar 4:2:2 layout, based on format=1/layout=0
 * factory descriptor and emulator evidence. Success proves geometry only.
 * It does NOT prove CPU mapping, readiness, cache coherency, lease or bit order.
 */
enum z8_photo_layout_status z8_photo_nv16_spans(
    const struct z8_photo_descriptor *d, uint64_t *y_bytes, uint64_t *uv_bytes);

enum z8_photo_role {
    Z8_PHOTO_MAIN = 0, Z8_PHOTO_PREVIEW, Z8_PHOTO_AUX,
    Z8_PHOTO_SMALL, Z8_PHOTO_MEDIUM, Z8_PHOTO_THUMBNAIL
};

struct z8_photo_source_snapshot {
    uint64_t resource, descriptor_address;
    uint8_t raw[Z8_PHOTO_DESCRIPTOR_BYTES];
    uint8_t present, stable, firmware_done_flag;
    struct z8_photo_descriptor decoded;
};

struct z8_photo_snapshot {
    uint64_t context, outer, metadata, output;
    uint32_t context_word, outer_word10;
    /* Metadata enum values are recorded, not interpreted as an allowlist. */
    uint32_t mode, quality, container;
    uint8_t stable;
    struct z8_photo_source_snapshot sources[Z8_PHOTO_SOURCE_COUNT];
};

/* Reader must reject unmapped/invalid ranges and avoid side-effectful MMIO.
 * There are no raw pointer casts or pixel reads in this module. */
typedef int (*z8_photo_read_fn)(void *opaque, uint64_t address,
                              void *destination, size_t length);

/* Bounded passive metadata capture, 2 passes. A stable snapshot is not a
 * producer fence: a producer can modify pixels without changing descriptors.
 * Returns 1 on complete capture; 0 on invalid argument/read failure/change.
 */
int z8_photo_snapshot_read(z8_photo_read_fn read, void *opaque,
                           uint64_t context, struct z8_photo_snapshot *out);
/* Same observation for audited points where only the outer R is live. */
int z8_photo_snapshot_outer_read(z8_photo_read_fn read, void *opaque,
                                 uint64_t outer, struct z8_photo_snapshot *out);

#endif
