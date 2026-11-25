/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nef.h"

static uint16_t u16(const uint8_t *p, int be)
{
    return be ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t u32(const uint8_t *p, int be)
{
    return be ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                    ((uint32_t)p[2] << 8) | p[3] :
                p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                    ((uint32_t)p[3] << 24);
}

static int read_data(psm_nef *n, uint32_t offset, void *out, uint32_t size)
{
    uint8_t *destination = out;
    if (offset > n->file.size || size > n->file.size - offset)
        return PSM_BAD_NEF;
    while (size) {
        uint32_t available;
        if (!n->cache_bytes || offset < n->cache_offset ||
            offset - n->cache_offset >= n->cache_bytes) {
            n->cache_offset = offset & ~(n->cache_size - 1u);
            n->cache_bytes = n->file.size - n->cache_offset;
            if (n->cache_bytes > n->cache_size)
                n->cache_bytes = n->cache_size;
            if (n->file.read_at(n->file.context, n->cache_offset,
                                n->cache, n->cache_bytes))
                return PSM_READ_ERROR;
        }
        available = n->cache_bytes - (offset - n->cache_offset);
        if (available > size)
            available = size;
        for (uint32_t i = 0; i < available; ++i)
            destination[i] = n->cache[offset - n->cache_offset + i];
        destination += available;
        offset += available;
        size -= available;
    }
    return PSM_OK;
}

static int byte_order(const uint8_t *header)
{
    if (header[0] == 'I' && header[1] == 'I' && u16(header + 2, 0) == 42)
        return 0;
    if (header[0] == 'M' && header[1] == 'M' && u16(header + 2, 1) == 42)
        return 1;
    return -1;
}

static int value_at(psm_nef *n, const uint8_t *entry, int be, uint32_t base,
                    uint32_t index, uint32_t *value)
{
    uint16_t type = u16(entry + 2, be);
    uint32_t count = u32(entry + 4, be);
    uint32_t unit = type == 1 ? 1 : type == 3 ? 2 : type == 4 ? 4 : 0;
    uint8_t buffer[4];
    uint64_t offset;
    if (!unit || !count || count > 65536 || index >= count)
        return PSM_BAD_NEF;
    if (count * unit <= 4) {
        const uint8_t *data = entry + 8 + unit * index;
        *value = unit == 1 ? data[0] : unit == 2 ? u16(data, be) : u32(data, be);
        return PSM_OK;
    }
    offset = (uint64_t)base + u32(entry + 8, be) + unit * index;
    if (offset > UINT32_MAX || read_data(n, (uint32_t)offset, buffer, unit))
        return PSM_BAD_NEF;
    *value = unit == 1 ? buffer[0] : unit == 2 ? u16(buffer, be) : u32(buffer, be);
    return PSM_OK;
}

static int nikon_metadata(psm_nef *n, uint32_t offset, uint32_t length)
{
    static const uint8_t magic[10] = {'N','i','k','o','n',0,2,0x11,0,0};
    uint8_t header[18], entry[12], small[12];
    uint32_t base, first, count, selected = 0, selected_bytes = 0, selected_offset = 0;
    uint32_t seen = 0;
    int be;
    if (length < 18 || offset > n->file.size || length > n->file.size - offset ||
        read_data(n, offset, header, sizeof(header)))
        return PSM_BAD_NEF;
    for (uint32_t i = 0; i < 10; ++i) {
        if (header[i] != magic[i])
            return PSM_UNSUPPORTED_NEF;
    }
    be = byte_order(header + 10);
    if (be < 0)
        return PSM_BAD_NEF;
    base = offset + 10;
    first = u32(header + 14, be);
    if (first > length - 12 || read_data(n, base + first, small, 2))
        return PSM_BAD_NEF;
    count = u16(small, be);
    if (count > 512 || (uint64_t)first + 2 + 12 * count > length - 10)
        return PSM_BAD_NEF;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t tag, data_offset, bytes;
        if (read_data(n, base + first + 2 + 12 * i, entry, 12))
            return PSM_BAD_NEF;
        tag = u16(entry, be);
        if (tag != 0x8C && tag != 0x96)
            continue;
        bytes = u32(entry + 4, be);
        data_offset = u32(entry + 8, be);
        if ((seen & (tag == 0x96 ? 2u : 1u)) ||
            u16(entry + 2, be) != 7 || bytes < 10 ||
            data_offset > length - 10 || bytes > length - 10 - data_offset)
            return PSM_BAD_NEF;
        seen |= tag == 0x96 ? 2u : 1u;
        /* Z8 files contain an older 0x8c block as well as the active 0x96 block. */
        if (tag > selected) {
            selected = tag;
            selected_bytes = bytes;
            selected_offset = data_offset;
        }
    }
    if (!selected)
        return PSM_OK;
    if (n->metadata_found || selected_bytes < 10 ||
        read_data(n, base + selected_offset, small, 10))
        return PSM_BAD_NEF;
    if (small[0] != 0x46 || small[1] != 0x30)
        return PSM_UNSUPPORTED_NEF;
    for (uint32_t j = 0; j < 4; ++j) {
        n->seeds[j] = u16(small + 2 + 2 * j, be);
        if (n->seeds[j] > 16383)
            return PSM_BAD_NEF;
    }
    n->metadata_found = 1;
    return PSM_OK;
}

/* The Z 8 lossless table: 8-bit prefix -> code length << 8 | difference category. */
static void prefix_table(uint16_t *table)
{
    static const uint8_t counts[8] = {0,1,4,2,2,3,1,2};
    static const uint8_t categories[15] = {7,6,8,5,9,4,10,3,11,12,2,0,1,13,14};
    uint32_t code = 0, symbol = 0;
    for (uint32_t i = 0; i < 256; ++i)
        table[i] = 0;
    for (uint32_t length = 1; length <= 8; ++length) {
        for (uint32_t j = 0; j < counts[length - 1]; ++j) {
            uint32_t first = code << (8 - length);
            for (uint32_t k = 0; k < (1u << (8 - length)); ++k)
                table[first + k] = (uint16_t)((length << 8) | categories[symbol]);
            ++code;
            ++symbol;
        }
        code <<= 1;
    }
}

static int32_t difference_of(uint32_t payload, uint32_t category)
{
    int32_t difference = (int32_t)payload;
    if (category && !(payload & (1u << (category - 1u))))
        difference -= (1 << category) - 1;
    return difference;
}

void psm_nef_fast_table(int32_t *table)
{
    uint16_t prefix[256];
    prefix_table(prefix);
    for (uint32_t i = 0; i < (1u << PSM_FAST_BITS); ++i) {
        uint32_t huff = prefix[i >> (PSM_FAST_BITS - 8u)], length = huff >> 8, category = huff & 255u;
        if (!length || length + category > PSM_FAST_BITS) {
            table[i] = 0;
            continue;
        }
        table[i] = difference_of((i >> (PSM_FAST_BITS - length - category)) & ((1u << category) - 1u), category) * 256 +
                   (int32_t)(length + category);
    }
}

int psm_nef_open(psm_nef *n, const psm_file *file)
{
    uint8_t header[8], small[4], entry[12];
    uint32_t queue[64], visited[64], queued = 0, visits = 0, raw_images = 0;
    int be;
    if (!n || !file || !file->read_at || file->size < 8 || file->size >= 0x80000000u)
        return PSM_BAD_NEF;
    if (!n->cache || !n->fast || n->cache_size < 4096u || (n->cache_size & (n->cache_size - 1u)) ||
        ((uintptr_t)n->cache & 63u))
        return PSM_BAD_PLAN;
    n->file = *file;
    n->cache_bytes = n->metadata_found = 0;
    n->width = n->height = n->next_row = n->pending = n->held = 0;
    if (read_data(n, 0, header, sizeof(header)))
        return PSM_READ_ERROR;
    be = byte_order(header);
    if (be < 0)
        return PSM_BAD_NEF;
    n->big_endian = (uint8_t)be;
    queue[queued++] = u32(header + 4, be);
    while (queued) {
        uint32_t at = queue[--queued], count, next;
        uint32_t width = 0, height = 0, bits = 0, compression = 0, photo = 0;
        uint32_t strip = 0, strip_bytes = 0, samples = 1, fill_order = 1;
        uint32_t scalar_unsupported = 0;
        uint32_t cfa[4] = {3,3,3,3}, repeat[2] = {0,0};
        if (!at)
            continue;
        for (uint32_t i = 0; i < visits; ++i) {
            if (visited[i] == at)
                return PSM_BAD_NEF;
        }
        if (visits == 64 || read_data(n, at, small, 2))
            return PSM_BAD_NEF;
        visited[visits++] = at;
        count = u16(small, be);
        if (count > 512 || (uint64_t)at + 2 + 12 * count + 4 > n->file.size)
            return PSM_BAD_NEF;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t tag, values, value = 0;
            int status;
            if (read_data(n, at + 2 + 12 * i, entry, 12))
                return PSM_BAD_NEF;
            tag = u16(entry, be);
            values = u32(entry + 4, be);
            if (tag == 0x927C) {
                if (u16(entry + 2, be) != 7 || values < 18)
                    return PSM_BAD_NEF;
                status = nikon_metadata(n, u32(entry + 8, be), values);
                if (status)
                    return status;
                continue;
            }
            if (tag == 0x14A || tag == 0x8769) {
                if (!values || values > 16 || queued + values > 64)
                    return PSM_BAD_NEF;
                for (uint32_t j = 0; j < values; ++j) {
                    if (value_at(n, entry, be, 0, j, &value))
                        return PSM_BAD_NEF;
                    queue[queued++] = value;
                }
                continue;
            }
            if (tag == 0x828D || tag == 0x828E) {
                uint32_t expected = tag == 0x828D ? 2u : 4u;
                if (values != expected)
                    return PSM_BAD_NEF;
                for (uint32_t j = 0; j < expected; ++j) {
                    if (value_at(n, entry, be, 0, j, &value))
                        return PSM_BAD_NEF;
                    if (tag == 0x828D)
                        repeat[j] = value;
                    else
                        cfa[j] = value;
                }
                continue;
            }
            if (tag != 0x100 && tag != 0x101 && tag != 0x102 && tag != 0x103 &&
                tag != 0x106 && tag != 0x10A && tag != 0x111 && tag != 0x115 && tag != 0x117)
                continue;
            if (values != 1) {
                scalar_unsupported = 1;
                continue;
            }
            if (value_at(n, entry, be, 0, 0, &value))
                return PSM_BAD_NEF;
            switch (tag) {
            case 0x100: width = value; break;
            case 0x101: height = value; break;
            case 0x102: bits = value; break;
            case 0x103: compression = value; break;
            case 0x106: photo = value; break;
            case 0x10A: fill_order = value; break;
            case 0x111: strip = value; break;
            case 0x115: samples = value; break;
            case 0x117: strip_bytes = value; break;
            }
        }
        if (photo == 32803) {
            if (++raw_images != 1 || scalar_unsupported || !width || !height || width > PSM_MAX_SENSOR ||
                height > PSM_MAX_SENSOR || bits != 14 || compression != 34713 ||
                samples != 1 || fill_order != 1 || repeat[0] != 2 || repeat[1] != 2)
                return PSM_UNSUPPORTED_NEF;
            if (!strip_bytes || strip > n->file.size || strip_bytes > n->file.size - strip)
                return PSM_BAD_NEF;
            n->width = width;
            n->height = height;
            n->strip_offset = strip;
            n->strip_bytes = strip_bytes;
            for (uint32_t j = 0; j < 4; ++j) {
                if (cfa[j] > 2)
                    return PSM_BAD_NEF;
                n->cfa[j] = (uint8_t)cfa[j];
            }
        }
        if (read_data(n, at + 2 + 12 * count, small, 4))
            return PSM_BAD_NEF;
        next = u32(small, be);
        if (next) {
            if (queued == 64)
                return PSM_BAD_NEF;
            queue[queued++] = next;
        }
    }
    if (raw_images != 1 || !n->metadata_found)
        return PSM_UNSUPPORTED_NEF;
    prefix_table(n->huffman);
    for (uint32_t i = 0; i < 4; ++i)
        n->vertical[i / 2][i % 2] = n->seeds[i];
    n->cursor = n->strip_offset;
    return PSM_OK;
}

static int more_bits(psm_nef *n, uint32_t required)
{
    while (n->held < required) {
        uint32_t left = n->strip_bytes - (n->cursor - n->strip_offset);
        uint32_t off, avail;
        const uint8_t *c;
        if (!left) break;
        if (!n->cache_bytes || n->cursor < n->cache_offset ||
            n->cursor - n->cache_offset >= n->cache_bytes) {
            uint8_t byte;
            if (read_data(n, n->cursor, &byte, 1)) return PSM_READ_ERROR;
            /* Cache is now filled; consume below with the same bounds checks. */
        }
        off = n->cursor - n->cache_offset;
        avail = n->cache_bytes - off;
        if (avail > left) avail = left;
        c = n->cache + off;
        if (avail >= 4 && n->held <= 32) {
            uint32_t word = ((uint32_t)c[0] << 24) | ((uint32_t)c[1] << 16) |
                            ((uint32_t)c[2] << 8) | c[3];
            n->pending = (n->pending << 32) | word;
            n->held += 32;
            n->cursor += 4;
        } else {
            n->pending = (n->pending << 8) | *c;
            n->held += 8;
            ++n->cursor;
        }
    }
    return PSM_OK;
}

/* PS6's decoder for one symbol: refill to 22 bits, 8-bit prefix, then the payload. PS7: returns the difference in the
   low 32 bits, or 1 << 32 | status (by value, so the caller's difference stays in a register). */
#define SLOW_FAILED(status) ((1ull << 32) | (uint32_t)(status))
static uint64_t slow_symbol(psm_nef *n)
{
    uint32_t prefix, huff, length, category, payload;
    if ((n->held < 22 && more_bits(n, 22)) || !n->held)
        return SLOW_FAILED(PSM_BAD_NEF);
    prefix = n->held >= 8 ? n->pending >> (n->held - 8) : n->pending << (8 - n->held);
    huff = n->huffman[prefix & 255u];
    length = huff >> 8;
    category = huff & 255u;
    if (!length || n->held < length)
        return SLOW_FAILED(PSM_BAD_NEF);
    n->held -= length;
    if (n->held < category)
        return SLOW_FAILED(PSM_BAD_NEF);
    n->held -= category;
    payload = (n->pending >> n->held) & ((1u << category) - 1u);
    return (uint32_t)difference_of(payload, category);
}

/* PS7: the cached part of the strip after the cursor; the fast path refills from it without a call. */
static void window(const psm_nef *n, uint32_t *base, uint32_t *pos, uint32_t *limit)
{
    uint32_t end = n->strip_offset + n->strip_bytes;
    if (n->cache_bytes && n->cursor >= n->cache_offset && n->cursor - n->cache_offset < n->cache_bytes) {
        *base = n->cache_offset;
        *pos = n->cursor - n->cache_offset;
        *limit = end - n->cache_offset < n->cache_bytes ? end - n->cache_offset : n->cache_bytes;
    } else {
        *base = n->cursor;
        *pos = *limit = 0;
    }
}

/* PS7: the bit reservoir stays in registers; a symbol whose code and payload fit PSM_FAST_BITS takes one table
   lookup, any other symbol (and any refill outside the cache) goes through slow_symbol with PS6's checks. The range
   check of every value is kept, folded into one test per row. */
#define PSM_NEXT()                                                                                        \
    do {                                                                                                  \
        int32_t entry_ = 0;                                                                               \
        if (held < 22u && pos + 4u <= limit) {                                                            \
            bits = bits << 32 | ((uint32_t)c[pos] << 24 | (uint32_t)c[pos + 1] << 16 |                    \
                                 (uint32_t)c[pos + 2] << 8 | c[pos + 3]);                                 \
            pos += 4u;                                                                                    \
            held += 32u;                                                                                  \
        }                                                                                                 \
        if (held >= 22u)                                                                                  \
            entry_ = fast[(bits >> (held - PSM_FAST_BITS)) & ((1u << PSM_FAST_BITS) - 1u)];               \
        if (entry_) {                                                                                     \
            held -= (uint32_t)entry_ & 31u;                                                               \
            difference = entry_ >> 8;                                                                     \
        } else {                                                                                          \
            uint64_t slow_;                                                                               \
            n->pending = bits;                                                                            \
            n->held = held;                                                                               \
            n->cursor = base + pos;                                                                       \
            slow_ = slow_symbol(n);                                                                       \
            if (slow_ >> 32)                                                                              \
                return (int)(uint32_t)slow_;                                                              \
            difference = (int32_t)(uint32_t)slow_;                                                        \
            bits = n->pending;                                                                            \
            held = n->held;                                                                               \
            window(n, &base, &pos, &limit);                                                               \
        }                                                                                                 \
    } while (0)

static int decode_row(psm_nef *n, uint16_t *out)
{
    const int32_t *fast = n->fast;
    const uint8_t *c = n->cache;
    uint64_t bits = n->pending;
    uint32_t held = n->held, base, pos, limit, check = 0, width = n->width, x = 0;
    int32_t *vertical = n->vertical[n->next_row & 1u];
    int32_t difference = 0, even = 0, odd = 0;
    window(n, &base, &pos, &limit);
    /* the first two samples predict from the row two above, the rest from two to the left */
    for (; x < 2 && x < width; ++x) {
        PSM_NEXT();
        vertical[x] += difference;
        if (x) odd = vertical[1];
        else even = vertical[0];
        check |= (uint32_t)vertical[x];
        out[x] = (uint16_t)vertical[x];
    }
    for (; x + 1 < width; x += 2) {
        PSM_NEXT();
        even += difference;
        PSM_NEXT();
        odd += difference;
        out[x] = (uint16_t)even;
        out[x + 1] = (uint16_t)odd;
        check |= (uint32_t)even | (uint32_t)odd;
    }
    if (x < width) {
        PSM_NEXT();
        even += difference;
        out[x] = (uint16_t)even;
        check |= (uint32_t)even;
    }
    n->pending = bits;
    n->held = held;
    n->cursor = base + pos;
    if (check >> 14)
        return PSM_BAD_NEF;
    ++n->next_row;
    return PSM_OK;
}
#undef PSM_NEXT

int psm_nef_row(psm_nef *n, uint32_t row, uint16_t *samples, uint32_t width)
{
    if (!n || !samples || row < n->next_row || row >= n->height || width != n->width)
        return PSM_BAD_NEF;
    while (n->next_row <= row) {
        int status = decode_row(n, samples);
        if (status)
            return status;
    }
    return PSM_OK;
}
