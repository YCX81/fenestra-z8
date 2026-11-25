/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Deduplicated .cube 3D LUT Parser, Tetrahedral Interpolator & 1D Curve Compiler.
 * Adapted from Fenestra-Z8-C311-source/modules/jpeg_lut/cube.c.
 * Strictly scalar (-mgeneral-regs-only), zero FP, zero SIMD, zero .data/.bss.
 */
#include "cube.h"
#include "v71r7_lut_abi.h"

enum {
    UNIT    = 1000000,
    MINIMUM = 1,
    MAXIMUM = 2,
    RANGE   = 4,
    STYLE   = 8
};

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static char *skip_ws(char *p) { while (is_space(*p)) ++p; return p; }

V71R7_BANK2_COLD static int match_token(char **cursor, const char *word)
{
    char *p = *cursor;
    while (*word && *p == *word) {
        ++word;
        ++p;
    }
    if (*word || (*p && !is_space(*p) && *p != '#')) {
        return 0;
    }
    *cursor = skip_ws(p);
    return 1;
}

static int at_end(const char *p) { return !*p || *p == '#'; }

/* Bounded decimal parser for CUBE input. Keeps micro-unit (1e-6) precision
 * and accepts scientific notation used by LUT exporters, with zero FP math. */
V71R7_BANK2_COLD static int parse_number(char **cursor, int32_t *result)
{
    char *p = skip_ws(*cursor);
    int negative = (*p == '-');
    if (*p == '+' || *p == '-') {
        ++p;
    }
    uint64_t mantissa = 0u;
    int fraction = 0;
    int digits = 0;
    while (is_digit(*p)) {
        if (++digits > 12) {
            return 0;
        }
        mantissa = mantissa * 10u + (unsigned)(*p++ - '0');
    }
    if (*p == '.') {
        ++p;
        while (is_digit(*p)) {
            if (++digits > 12) {
                return 0;
            }
            mantissa = mantissa * 10u + (unsigned)(*p++ - '0');
            ++fraction;
        }
    }
    if (!digits) {
        return 0;
    }
    int power = 6 - fraction;
    if (*p == 'e' || *p == 'E') {
        ++p;
        int minus = (*p == '-');
        if (*p == '+' || *p == '-') {
            ++p;
        }
        if (!is_digit(*p)) {
            return 0;
        }
        int exponent = 0;
        while (is_digit(*p)) {
            exponent = exponent * 10 + (*p++ - '0');
            if (exponent > 20) {
                return 0;
            }
        }
        power += minus ? -exponent : exponent;
    }
    if (*p && !is_space(*p) && *p != '#') {
        return 0;
    }
    while (power < 0) {
        mantissa /= 10u;
        ++power;
    }
    while (power > 0) {
        if (mantissa > 10000000u) {
            return 0;
        }
        mantissa *= 10u;
        --power;
    }
    if (mantissa > 100000000u) {
        return 0;
    }
    *result = negative ? -(int32_t)mantissa : (int32_t)mantissa;
    *cursor = skip_ws(p);
    return 1;
}

V71R7_BANK2_COLD static int parse_numbers(char *p, int32_t *values, unsigned count)
{
    for (unsigned i = 0u; i < count; ++i) {
        if (!parse_number(&p, &values[i])) {
            return 0;
        }
    }
    return at_end(p);
}

V71R7_BANK2_COLD void jpeg_cube_init(JpegCube *cube)
{
    if (!cube) {
        return;
    }
    cube->grid = 0u;
    cube->count = 0u;
    cube->flags = 0u;
    for (unsigned c = 0u; c < 3u; ++c) {
        cube->low[c] = 0;
        cube->high[c] = UNIT;
    }
}

V71R7_BANK2_COLD int jpeg_cube_line(JpegCube *cube, char *line)
{
    if (!cube || !line) {
        return 0;
    }
    char *p = skip_ws(line);
    if (match_token(&p, "#LUMIXPHOTOSTYLE")) {
        if ((cube->flags & STYLE) || !match_token(&p, "STD") || !at_end(p)) {
            return 0;
        }
        cube->flags |= STYLE;
        return 1;
    }
    if (at_end(p)) {
        return 1;
    }
    if (!cube->count && match_token(&p, "TITLE")) {
        return 1;
    }
    if (!cube->count && match_token(&p, "LUT_3D_SIZE")) {
        unsigned grid = 0u;
        if (cube->grid || !is_digit(*p)) {
            return 0;
        }
        while (is_digit(*p)) {
            grid = grid * 10u + (unsigned)(*p++ - '0');
            if (grid > CUBE_MAX_GRID) {
                return 0;
            }
        }
        if (grid < 2u || !at_end(skip_ws(p))) {
            return 0;
        }
        cube->grid = grid;
        return 1;
    }
    if (!cube->count && match_token(&p, "DOMAIN_MIN")) {
        if ((cube->flags & (MINIMUM | RANGE)) || !parse_numbers(p, cube->low, 3u)) {
            return 0;
        }
        cube->flags |= MINIMUM;
        return 1;
    }
    if (!cube->count && match_token(&p, "DOMAIN_MAX")) {
        if ((cube->flags & (MAXIMUM | RANGE)) || !parse_numbers(p, cube->high, 3u)) {
            return 0;
        }
        cube->flags |= MAXIMUM;
        return 1;
    }
    if (!cube->count && match_token(&p, "LUT_3D_INPUT_RANGE")) {
        int32_t range[2];
        if ((cube->flags & (MINIMUM | MAXIMUM | RANGE)) || !parse_numbers(p, range, 2u)) {
            return 0;
        }
        for (unsigned c = 0u; c < 3u; ++c) {
            cube->low[c] = range[0];
            cube->high[c] = range[1];
        }
        cube->flags |= RANGE;
        return 1;
    }
    uint32_t n = cube->grid;
    int32_t rgb[3];
    if (!n || cube->count >= n * n * n || !parse_numbers(p, rgb, 3u)) {
        return 0;
    }
    for (unsigned c = 0u; c < 3u; ++c) {
        int32_t value = rgb[c] < 0 ? 0 : (rgb[c] > UNIT ? UNIT : rgb[c]);
        cube->rgb[cube->count][c] = (uint16_t)(((uint64_t)value * 65535u + UNIT / 2u) / UNIT);
    }
    ++cube->count;
    return 1;
}

V71R7_BANK2_COLD static unsigned coordinate(const JpegCube *cube, unsigned c, unsigned v, unsigned cell_units)
{
    int64_t offset = (int64_t)v * UNIT - (int64_t)cube->low[c] * 255;
    int64_t span = ((int64_t)cube->high[c] - cube->low[c]) * 255;
    unsigned top = (cube->grid - 1u) * cell_units;
    if (offset <= 0) {
        return 0u;
    }
    if (offset >= span) {
        return top;
    }
    return (unsigned)((offset * top + span / 2) / span);
}

V71R7_BANK2_COLD int jpeg_cube_finish(JpegCube *cube)
{
    if (!cube) {
        return 0;
    }
    uint32_t n = cube->grid;
    if (n < 2u || n > CUBE_MAX_GRID || cube->count != n * n * n) {
        return 0;
    }
    for (unsigned c = 0u; c < 3u; ++c) {
        if (cube->high[c] <= cube->low[c]) {
            return 0;
        }
        for (unsigned v = 0u; v < 256u; ++v) {
            cube->axis[c][v] = (uint16_t)coordinate(cube, c, v, 256u);
        }
    }
    return 1;
}

V71R7_BANK2_COLD static int depends_only_on(const JpegCube *cube, unsigned channel, unsigned stride)
{
    unsigned block = cube->grid * stride;
    for (unsigned start = 0u; start < cube->count; start += block) {
        for (unsigned axis = 0u; axis < block; axis += stride) {
            uint16_t expected = cube->rgb[axis][channel];
            for (unsigned i = 0u; i < stride; ++i) {
                if (cube->rgb[start + axis + i][channel] != expected) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

V71R7_BANK2_COLD int jpeg_cube_compile_curves(const JpegCube *cube, uint16_t curves[3][256], uint8_t source[3])
{
    static const uint8_t orders[][3] = {
        {0u, 1u, 2u}, {0u, 2u, 1u}, {1u, 0u, 2u},
        {1u, 2u, 0u}, {2u, 0u, 1u}, {2u, 1u, 0u}
    };
    if (!cube || !curves || !source) {
        return 0;
    }
    unsigned n = cube->grid;
    if (n < 2u || n > CUBE_MAX_GRID || cube->count != n * n * n) {
        return 0;
    }
    const unsigned strides[3] = { 1u, n, n * n };
    unsigned allowed[3] = { 0u, 0u, 0u };
    for (unsigned c = 0u; c < 3u; ++c) {
        for (unsigned a = 0u; a < 3u; ++a) {
            if (depends_only_on(cube, c, strides[a])) {
                allowed[c] |= 1u << a;
            }
        }
        if (!allowed[c]) {
            return 0;
        }
    }
    unsigned order = 0u;
    for (; order < sizeof(orders) / sizeof(orders[0]); ++order) {
        const uint8_t *axes = orders[order];
        if ((allowed[0] & (1u << axes[0])) &&
            (allowed[1] & (1u << axes[1])) &&
            (allowed[2] & (1u << axes[2]))) {
            break;
        }
    }
    if (order == sizeof(orders) / sizeof(orders[0])) {
        return 0;
    }
    for (unsigned c = 0u; c < 3u; ++c) {
        unsigned a = orders[order][c];
        source[c] = (uint8_t)a;
        unsigned stride = strides[a];
        if (cube->high[a] <= cube->low[a]) {
            return 0;
        }
        for (unsigned i = 0u; i <= 128u; ++i) {
            unsigned input = i < 128u ? 2u * i : 255u;
            unsigned pos = coordinate(cube, a, input, 32768u);
            unsigned lo = pos >> 15;
            unsigned hi = lo < n - 1u ? lo + 1u : lo;
            unsigned f = pos & 32767u;
            unsigned value = (32768u - f) * cube->rgb[lo * stride][c] +
                             f * cube->rgb[hi * stride][c];
            /* One output code equals 128 DSP units: 65535*32768/(255*128) = 65792. */
            int knot = (int)((value + 32896u) / 65792u);
            if (i == 128u) {
                /* Code 255 is halfway between the last two hardware knots. */
                knot = 2 * knot - (int)curves[c][127];
                if (knot < 0) {
                    knot = 0;
                }
                if (knot > 32768) {
                    knot = 32768;
                }
            }
            curves[c][i] = (uint16_t)knot;
        }
        for (unsigned i = 129u; i < 256u; ++i) {
            curves[c][i] = curves[c][128];
        }
    }
    /* Verify every 8-bit input against full tetrahedral interpolation before accepting. */
    for (unsigned v = 0u; v < 256u; ++v) {
        uint8_t expected[3];
        jpeg_cube_rgb(cube, (uint8_t)v, (uint8_t)v, (uint8_t)v, expected);
        for (unsigned c = 0u; c < 3u; ++c) {
            unsigned pixel = (curves[c][v / 2u] + (unsigned)curves[c][(v + 1u) / 2u] + 128u) >> 8;
            if (pixel > 255u) {
                pixel = 255u;
            }
            if (pixel > (unsigned)expected[c] + 1u || pixel + 1u < (unsigned)expected[c]) {
                return 0;
            }
        }
    }
    return 1;
}

void jpeg_cube_rgb(const JpegCube *cube, uint8_t r, uint8_t g, uint8_t b, uint8_t out[3])
{
    unsigned n = cube->grid;
    unsigned pos[3] = { cube->axis[0][r], cube->axis[1][g], cube->axis[2][b] };
    unsigned stride[3] = { 1u, n, n * n };
    unsigned frac[3];
    unsigned base = 0u;
    for (unsigned c = 0u; c < 3u; ++c) {
        unsigned lo = pos[c] >> 8;
        if (lo == n - 1u) {
            --lo;
            frac[c] = 256u;
        } else {
            frac[c] = pos[c] & 255u;
        }
        base += lo * stride[c];
    }
    /* Sort fractions to select the four tetrahedral vertices. */
    unsigned a = 0u, d = 1u, e = 2u;
    if (frac[a] < frac[d]) {
        a = 1u;
        d = 0u;
    }
    if (frac[d] < frac[e]) {
        unsigned old = d;
        d = e;
        e = old;
    }
    if (frac[a] < frac[d]) {
        unsigned old = a;
        a = d;
        d = old;
    }
    unsigned w0 = 256u - frac[a];
    unsigned w1 = frac[a] - frac[d];
    unsigned w2 = frac[d] - frac[e];
    unsigned w3 = frac[e];
    unsigned p1 = base + stride[a];
    unsigned p2 = p1 + stride[d];
    unsigned p3 = p2 + stride[e];
    for (unsigned c = 0u; c < 3u; ++c) {
        unsigned value = w0 * cube->rgb[base][c] + w1 * cube->rgb[p1][c] +
                         w2 * cube->rgb[p2][c] + w3 * cube->rgb[p3][c];
        out[c] = (uint8_t)jpeg_cube_reduce((value + 32896u) >> 8);
    }
}

static uint8_t clamp_u8(int value)
{
    return (uint8_t)(value < 0 ? 0 : (value > 255 ? 255 : value));
}

static inline void sample_rgb(const JpegCube *cube, int luma, JpegCubeChroma offset, uint8_t rgb[3])
{
    jpeg_cube_rgb(cube, clamp_u8(luma + offset.r),
                  clamp_u8(luma + offset.g),
                  clamp_u8(luma + offset.b), rgb);
}

uint32_t jpeg_cube_sample(const JpegCube *cube, unsigned luma, JpegCubeChroma offset)
{
    uint8_t rgb[3];
    sample_rgb(cube, (int)luma, offset, rgb);
    int r = rgb[0], g = rgb[1], b = rgb[2];
    uint32_t y  = clamp_u8((19595 * r + 38470 * g + 7471 * b + 32768) >> 16);
    uint32_t cb = clamp_u8(128 + ((-11059 * r - 21709 * g + 32768 * b + 32768) >> 16));
    uint32_t cr = clamp_u8(128 + ((32768 * r - 27439 * g - 5329 * b + 32768) >> 16));
    return y | (cb << 8) | (cr << 16);
}

void jpeg_cube_422(const JpegCube *cube, uint8_t *y, uint8_t *uv, uint32_t width)
{
    for (uint32_t x = 0u; x < width; x += 2u) {
        JpegCubeChroma offset = jpeg_cube_chroma(uv[x], uv[x + 1u]);
        if (y[x] == y[x + 1u]) {
            uint32_t sample = jpeg_cube_sample(cube, y[x], offset);
            y[x] = y[x + 1u] = (uint8_t)sample;
            uv[x] = (uint8_t)(sample >> 8);
            uv[x + 1u] = (uint8_t)(sample >> 16);
            continue;
        }
        int sum_cb = 0, sum_cr = 0;
        for (unsigned i = 0u; i < 2u; ++i) {
            uint8_t rgb[3];
            sample_rgb(cube, y[x + i], offset, rgb);
            int r = rgb[0], g = rgb[1], b = rgb[2];
            y[x + i] = clamp_u8((19595 * r + 38470 * g + 7471 * b + 32768) >> 16);
            sum_cb += -11059 * r - 21709 * g + 32768 * b;
            sum_cr += 32768 * r - 27439 * g - 5329 * b;
        }
        uv[x] = clamp_u8(128 + ((sum_cb + 65536) >> 17));
        uv[x + 1u] = clamp_u8(128 + ((sum_cr + 65536) >> 17));
    }
}

void jpeg_cube_bake(const JpegCube *cube, uint32_t *table, uint32_t first, uint32_t end)
{
    for (uint32_t i = first; i < end; ++i) {
        uint8_t luma = (uint8_t)i;
        uint8_t cb = (uint8_t)(((i >> 8) & 31u) * 8u + 4u);
        uint8_t cr = (uint8_t)((i >> 13) * 8u + 4u);
        uint32_t sample = jpeg_cube_sample(cube, luma, jpeg_cube_chroma(cb, cr));
        uint32_t dy = (uint32_t)(255 + (int)(sample & 255u) - luma);
        uint32_t db = (uint32_t)(255 + (int)((sample >> 8) & 255u) - cb);
        uint32_t dr = (uint32_t)(255 + (int)(sample >> 16) - cr);
        table[i] = dy | (db << 10) | (dr << 20);
    }
}

void jpeg_cube_apply(const uint32_t *table, uint8_t *y, uint8_t *uv, uint32_t width)
{
    for (uint32_t x = 0u; x < width; x += 2u) {
        uint32_t base = ((uint32_t)(uv[x] >> 3) << 8) | ((uint32_t)(uv[x + 1u] >> 3) << 13);
        uint32_t a = table[base | y[x]];
        uint32_t b = table[base | y[x + 1u]];
        uint32_t sum = a + b + (1u << 10) + (1u << 20);
        y[x] = clamp_u8((int)y[x] + (int)(a & 511u) - 255);
        y[x + 1u] = clamp_u8((int)y[x + 1u] + (int)(b & 511u) - 255);
        uv[x] = clamp_u8((int)uv[x] + (int)((sum >> 11) & 511u) - 255);
        uv[x + 1u] = clamp_u8((int)uv[x + 1u] + (int)(sum >> 21) - 255);
    }
}
