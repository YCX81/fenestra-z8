#ifndef LIVE_AU_RING_H
#define LIVE_AU_RING_H
#include <stddef.h>
#include <stdint.h>
#define AU_RING_SLOTS 256u
struct au_entry { uint64_t pts; uint32_t offset, size; unsigned idr; };
struct au_ring { uint8_t *data; uint32_t capacity, used, head, count, write; struct au_entry entries[AU_RING_SLOTS]; };
void au_ring_init(struct au_ring *, uint8_t *, uint32_t);
int au_ring_push(struct au_ring *, const uint8_t *, uint32_t, uint64_t, unsigned);
int au_ring_find_start(const struct au_ring *, uint64_t, unsigned *);
int au_ring_copy(const struct au_ring *, unsigned, uint8_t *, uint32_t);
#endif
