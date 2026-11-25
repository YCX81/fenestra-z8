/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "psm.h"

static int displacement(int32_t half_pixels, uint32_t scale)
{
    return half_pixels * (int32_t)scale / 2;
}

/* PS6: frames per lattice point. 8 and 32 shots take every point twice: frame i + n/2 is frame i one Bayer period
   (2 sensor rows, dy2 + 4) lower. NX Studio 1.11 first averages each such pair, floor((a + b) / 2), then merges the
   pairs as a 4/16-shot group (research/pixelshift_nx_8_32_20261025). */
static uint32_t pairs(const psm_plan *p)
{
    return p->frame_count == 8 || p->frame_count == 32 ? p->frame_count / 2 : 0;
}

int psm_validate(const psm_plan *p)
{
    uint32_t seen = 0;
    uint32_t period, places;
    if (!p || !p->sensor_width || !p->sensor_height ||
        p->sensor_width > PSM_MAX_SENSOR || p->sensor_height > PSM_MAX_SENSOR ||
        !p->width || !p->height || p->width > 2 * PSM_MAX_SENSOR ||
        p->height > 2 * PSM_MAX_SENSOR)
        return PSM_BAD_PLAN;
    if (p->frame_count != 4 && p->frame_count != 8 && p->frame_count != 16 && p->frame_count != 32)
        return PSM_NEFX_MODE;
    if (!p->group_id || p->scale != (p->frame_count >= 16 ? 2u : 1u))
        return PSM_BAD_GROUP;
    period = 2 * p->scale;
    places = pairs(p) ? pairs(p) : p->frame_count;
    for (uint32_t i = 0; i < p->frame_count; ++i) {
        const psm_frame *f = &p->frames[i];
        uint32_t colors[3] = {0, 0, 0};
        int dx, dy;
        if (f->frame_id >= p->frame_count || (seen & (1u << f->frame_id)))
            return PSM_BAD_GROUP;
        seen |= 1u << f->frame_id;
        if (f->dx2 < -16 || f->dx2 > 16 || f->dy2 < -16 || f->dy2 > 16 ||
            (f->dx2 * (int)p->scale) % 2 || (f->dy2 * (int)p->scale) % 2)
            return PSM_BAD_LATTICE;
        for (uint32_t c = 0; c < 4; ++c) {
            if (f->cfa[c] > 2 || f->white[c] <= f->black[c])
                return PSM_BAD_PLAN;
            ++colors[f->cfa[c]];
        }
        if (colors[0] != 1 || colors[1] != 2 || colors[2] != 1)
            return PSM_BAD_PLAN;
        /* The green samples must occupy one of the two diagonals. */
        if (!((f->cfa[0] == 1 && f->cfa[3] == 1 && f->cfa[1] != f->cfa[2]) ||
              (f->cfa[1] == 1 && f->cfa[2] == 1 && f->cfa[0] != f->cfa[3])))
            return PSM_BAD_PLAN;
        dx = displacement(f->dx2, p->scale);
        dy = displacement(f->dy2, p->scale);
        if (p->x0 < dx || p->y0 < dy ||
            (int64_t)p->x0 + p->width - 1 >
                dx + (int64_t)p->scale * (p->sensor_width - 1) ||
            (int64_t)p->y0 + p->height - 1 >
                dy + (int64_t)p->scale * (p->sensor_height - 1))
            return PSM_BAD_CROP;
    }
    /* PS6: the second pass repeats the first frame for frame, in the same colours, two sensor rows lower. */
    for (uint32_t i = 0; pairs(p) && i < places; ++i) {
        const psm_frame *f = &p->frames[i], *g = &p->frames[i + places];
        if (f->frame_id != i || g->frame_id != i + places || g->dx2 != f->dx2 || g->dy2 != f->dy2 + 4)
            return PSM_BAD_LATTICE;
        for (uint32_t c = 0; c < 4; ++c) {
            if (g->cfa[c] != f->cfa[c] || g->black[c] != f->black[c] || g->white[c] != f->white[c])
                return PSM_BAD_PLAN;
        }
    }
    for (uint32_t py = 0; py < period; ++py) {
        for (uint32_t px = 0; px < period; ++px) {
            uint32_t counts[3] = {0, 0, 0};
            for (uint32_t i = 0; i < places; ++i) {
                const psm_frame *f = &p->frames[i];
                int x = (int)px - displacement(f->dx2, p->scale);
                int y = (int)py - displacement(f->dy2, p->scale);
                uint32_t phase;
                if (x % (int)p->scale || y % (int)p->scale)
                    continue;
                x /= (int)p->scale;
                y /= (int)p->scale;
                phase = ((uint32_t)y & 1u) * 2u + ((uint32_t)x & 1u);
                ++counts[f->cfa[phase]];
            }
            if (counts[0] != 1 || counts[1] != 2 || counts[2] != 1)
                return PSM_BAD_LATTICE;
        }
    }
    return PSM_OK;
}

size_t psm_scratch_size(const psm_plan *p)
{
    if (!p || p->width > 2 * PSM_MAX_SENSOR || p->sensor_width > PSM_MAX_SENSOR)
        return 0;
    /* PS6: two source rows, the second for the paired frame of an 8/32-shot group. PS7: no sums (rgb is the
       accumulator). */
    return 2u * (size_t)p->sensor_width * sizeof(uint16_t) + 8u;
}

int psm_merge_row(const psm_plan *p, uint32_t row,
                  psm_read_row_fn read_row, psm_cancel_fn cancel, void *context,
                  void *scratch, size_t scratch_size, uint16_t *rgb)
{
    int status = psm_validate(p);
    if (status) return status;
    return psm_merge_row_validated(p, row, read_row, cancel, context, scratch, scratch_size, rgb);
}

/* PS7: clear the row with aligned 64-bit stores (rgb is 8-byte aligned, the caller's contract). */
static void clear_row(uint16_t *rgb, uint32_t samples)
{
    typedef uint64_t alias_word __attribute__((may_alias));
    alias_word *w = (alias_word *)rgb;
    uint32_t words = samples / 4u;
    for (uint32_t i = 0; i < words; ++i) w[i] = 0;
    for (uint32_t i = words * 4u; i < samples; ++i) rgb[i] = 0;
}

static int merge_row_impl(const psm_plan *p, uint32_t row,
                  psm_read_row_fn read_row, psm_cancel_fn cancel, void *context,
                  void *scratch, size_t scratch_size, uint16_t *rgb,
                  uint16_t *green_sum, int correct_pairs)
{
    uint16_t *source, *second;
    uint32_t channels, places;
    if (!p || !scratch || !rgb || !read_row || row >= p->height)
        return PSM_BAD_PLAN;
    if (scratch_size < psm_scratch_size(p) || ((uintptr_t)scratch & 3u) || ((uintptr_t)rgb & 7u))
        return PSM_NO_MEMORY;
    channels = p->width * 3u;
    source = scratch;
    second = source + p->sensor_width;
    places = pairs(p) ? pairs(p) : p->frame_count;
    /* psm_validate proves every lattice phase has exactly R=1, G=2, B=1 (per pair for 8/32 shots), so a 16-bit
       sum cannot overflow: R and B hold one 14-bit code, G two. */
    clear_row(rgb, channels);
    for (uint32_t i = 0; i < places; ++i) {
        const psm_frame *f = &p->frames[i];
        int64_t sy = (int64_t)p->y0 + row - displacement(f->dy2, p->scale);
        int64_t base = (int64_t)p->x0 - displacement(f->dx2, p->scale), sx;
        uint32_t x, last, count, step, k, pick_blue = 0;
        uint8_t ch[2];
        uint16_t *d;
        const uint16_t *a, *b;
        if (cancel && cancel(context)) return PSM_CANCELLED;
        if (sy % p->scale) continue;
        sy /= p->scale;
        if (sy < 0 || sy >= p->sensor_height) return PSM_BAD_CROP;
        if (read_row(context, i, (uint32_t)sy, source, p->sensor_width))
            return PSM_READ_ERROR;
        if (places != p->frame_count) {
            /* the validated pair: same columns and colours, two sensor rows up in its own frame */
            int64_t sy2 = (int64_t)p->y0 + row - displacement(p->frames[i + places].dy2, p->scale);
            if (sy2 % p->scale) return PSM_BAD_LATTICE;
            sy2 /= p->scale;
            if (sy2 < 0 || sy2 >= p->sensor_height) return PSM_BAD_CROP;
            if (read_row(context, i + places, (uint32_t)sy2, second, p->sensor_width))
                return PSM_READ_ERROR;
            if (correct_pairs) {
                if (sy >= 29 && sy <= 5489 && sy % 12 == 5) pick_blue = 2;
                else if (sy2 >= 29 && sy2 <= 5489 && sy2 % 12 == 5) pick_blue = 1;
            }
        }
        /* The validated crop makes base nonnegative. Walk the lattice directly. */
        x = (uint32_t)((p->scale - base % p->scale) % p->scale);
        if (x >= p->width) continue;
        sx = (base + x) / p->scale;
        last = x + (p->width - 1 - x) / p->scale * p->scale;
        if (sx < 0 || (base + last) / p->scale >= p->sensor_width)
            return PSM_BAD_CROP;
        ch[0] = f->cfa[((uint32_t)sy & 1u) * 2u];
        ch[1] = f->cfa[((uint32_t)sy & 1u) * 2u + 1u];
        /* PS7: two lattice columns per step, the colour of each fixed (sensor columns alternate) */
        count = (last - x) / p->scale + 1u;
        step = 3u * p->scale;
        d = rgb + 3u * x;
        a = source + sx;
        b = second + sx;
        {
            uint32_t c0 = ch[sx & 1], c1 = step + ch[(sx + 1) & 1];
            if (places == p->frame_count) {
                for (k = 0; k + 1 < count; k += 2, d += 2u * step, a += 2) {
                    d[c0] = (uint16_t)(d[c0] + a[0]);
                    d[c1] = (uint16_t)(d[c1] + a[1]);
                }
                if (k < count) d[c0] = (uint16_t)(d[c0] + a[0]);
            } else {
                uint32_t select0 = c0 == 2 ? pick_blue : 0;
                uint32_t select1 = ch[(sx + 1) & 1] == 2 ? pick_blue : 0;
                for (k = 0; k + 1 < count; k += 2, d += 2u * step, a += 2, b += 2) {
                    uint32_t v0 = select0 == 1 ? a[0] : select0 == 2 ? b[0] : ((uint32_t)a[0] + b[0]) >> 1;
                    uint32_t v1 = select1 == 1 ? a[1] : select1 == 2 ? b[1] : ((uint32_t)a[1] + b[1]) >> 1;
                    d[c0] = (uint16_t)(d[c0] + v0);
                    d[c1] = (uint16_t)(d[c1] + v1);
                }
                if (k < count) {
                    uint32_t v = select0 == 1 ? a[0] : select0 == 2 ? b[0] : ((uint32_t)a[0] + b[0]) >> 1;
                    d[c0] = (uint16_t)(d[c0] + v);
                }
            }
        }
    }
    /* Preserve raw sensor codes and floor the green mean, exactly as PS1 NEFX. */
    for (uint32_t x = 0; x < p->width; ++x) {
        if (green_sum) green_sum[x] = rgb[3u*x+1u];
        rgb[3u*x+1u] = (uint16_t)(rgb[3u*x+1u] >> 1);
    }
    return PSM_OK;
}

int psm_merge_row_validated(const psm_plan *p, uint32_t row,
                  psm_read_row_fn read_row, psm_cancel_fn cancel, void *context,
                  void *scratch, size_t scratch_size, uint16_t *rgb)
{
    return merge_row_impl(p,row,read_row,cancel,context,scratch,scratch_size,rgb,0,0);
}

int psm_merge_row_af(const psm_plan *p, uint32_t row,
                  psm_read_row_fn read_row, psm_cancel_fn cancel, void *context,
                  void *scratch, size_t scratch_size, uint16_t *rgb, uint16_t *green_sum)
{
    return merge_row_impl(p,row,read_row,cancel,context,scratch,scratch_size,rgb,green_sum,1);
}
